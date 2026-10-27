// ComponentTransferPrefab.cpp - the non-UI half of Tools > Component Transfer:
// the .ipfb text model (parse / patch / write-back) and the tag <-> type lookups.
//
// The per-component-type transfer POLICY lives next to the window it drives, in
// ComponentTransferWindow.cpp: it needs typeid() of ~40 component types across a
// dozen headers, and it is UI policy rather than file format.
//
// Nothing here touches ImGui or EditorLayer, so tests/unit/ct_prefab_test.cpp
// links it on its own and proves the one property that matters before this code
// touches a user's asset: a load -> save round trip with no edits reproduces the
// file BYTE FOR BYTE.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/ComponentTransferWindow.h"

#include "Resource/AssetHeader.h"
#include "Resource/ComponentSerializers.h"

#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace CT = Editor::ComponentTransfer;

namespace
{

// Split the payload into lines. Strips '\r' (a .ipfb written on a system with
// CRLF translation) and the trailing NUL that SavePrefab appends: both would
// otherwise survive into a rewritten file and slowly accumulate.
std::vector<std::string> SplitPayloadLines(const char* text, size_t len)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < len; ++i)
    {
        const char c = text[i];
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\0') cur += c;
    }
    // A payload that ends with '\n' (every SavePrefab output does) leaves `cur`
    // empty: do not emit that phantom last line, or every load/save round trip
    // would grow the file by one blank line.
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Mirrors PrefabSerializer's PercentDecode (space %20, literal %25).
std::string PercentDecode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            const char hex[3] = { s[i + 1], s[i + 2], '\0' };
            try { r += static_cast<char>(std::stoi(hex, nullptr, 16)); }
            catch (...) { r += s[i]; }
            i += 3;
        }
        else r += s[i++];
    }
    return r;
}

// "N idx=0 parent=-1 tx=.. .. name=Foo" -> idx / parent / decoded name.
bool ParseNodeLine(const std::string& line, int& idx, int& parent, std::string& name)
{
    if (line.size() < 2 || line[0] != 'N' || line[1] != ' ') return false;
    idx = -1; parent = -1; name.clear();

    std::istringstream ss(line.substr(2));
    std::string tok;
    while (ss >> tok)
    {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq);
        const std::string v = tok.substr(eq + 1);
        try
        {
            if      (k == "idx")    idx    = std::stoi(v);
            else if (k == "parent") parent = std::stoi(v);
            else if (k == "name")   name   = PercentDecode(v);
        }
        catch (...) {}
    }
    return idx >= 0;
}

// "  Tag: key=val ..." -> Tag. The shape PrefabSerializer's ParseComponentLine
// uses: exactly two leading spaces, then a ':'.
bool ParseComponentTag(const std::string& line, std::string& tag)
{
    if (line.size() < 4 || line[0] != ' ' || line[1] != ' ') return false;
    const auto colon = line.find(':', 2);
    if (colon == std::string::npos) return false;
    tag = line.substr(2, colon - 2);
    return !tag.empty();
}

// Rebuild PrefabDoc::nodes from PrefabDoc::lines. Called after load and after
// every SetNodeComponentBlob, because both invalidate the line indices.
void ReparseNodes(CT::PrefabDoc& doc)
{
    doc.nodes.clear();
    for (size_t i = 0; i < doc.lines.size(); ++i)
    {
        const std::string& line = doc.lines[i];

        int idx = -1, parent = -1;
        std::string name;
        if (ParseNodeLine(line, idx, parent, name))
        {
            if (!doc.nodes.empty()) doc.nodes.back().endLine = i;
            CT::PrefabDoc::Node n;
            n.idx = idx; n.parent = parent; n.name = std::move(name);
            n.headerLine = i;
            doc.nodes.push_back(std::move(n));
            continue;
        }

        std::string tag;
        if (!doc.nodes.empty() && ParseComponentTag(line, tag))
        {
            auto& comps = doc.nodes.back().comps;
            auto it = std::find_if(comps.begin(), comps.end(),
                                   [&tag](const CT::PrefabDoc::Comp& c){ return c.tag == tag; });
            if (it == comps.end())
            {
                CT::PrefabDoc::Comp c;
                c.tag = std::move(tag);
                c.lines.push_back(i);
                comps.push_back(std::move(c));
            }
            else
            {
                // Same tag again = a multi-line component (ScriptComponent emits
                // one line per slot). Both lines belong to ONE blob.
                it->lines.push_back(i);
            }
        }
    }
    if (!doc.nodes.empty()) doc.nodes.back().endLine = doc.lines.size();
}

} // anonymous namespace

namespace Editor::ComponentTransfer
{

// Prefab-local link records (`PrefabFollowTarget`, `PrefabSkeletonRef`, ...).
// SavePrefab writes them with a "Prefab" tag prefix precisely so LoadPrefab's
// registry walk skips them; their values are POSITIONAL node indices, so they
// are meaningless outside the file that produced them.
bool IsPrefabLocalTag(const std::string& tag)
{
    return tag.rfind("Prefab", 0) == 0;
}

// ==== PrefabDoc - load / save / patch ====

bool LoadPrefabDoc(const std::string& path, PrefabDoc& out, std::string* err)
{
    out.Clear();
    const auto fail = [&](std::string m) { if (err) *err = std::move(m); out.Clear(); return false; };

    // Deliberately NOT AssetFS::ReadFile: that resolves through the packed
    // game.ipak too, and a .ipfb served out of a pak is not a file we could
    // ever write back. Reading only the loose file keeps "what you see is what
    // you can edit" true.
    std::ifstream f = Sys::OpenIfstream(path, std::ios::binary | std::ios::ate);
    if (!f) return fail("cannot open '" + path + "'");

    const std::streamoff size = f.tellg();
    const size_t kMinSize = sizeof(Resource::AssetHeader) + sizeof(Resource::PrefabMetadata);
    if (size < static_cast<std::streamoff>(kMinSize))
        return fail("file is too small to be a .ipfb");

    std::vector<uint8_t> blob(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(blob.data()), size);
    if (!f) return fail("read error");

    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_PREFAB))
        return fail("not a valid .ipfb (magic / version / size check failed)");

    const Resource::AssetHeader* hdr = Resource::GetHeader(blob.data());
    if (hdr->metadataSize != sizeof(Resource::PrefabMetadata))
        return fail("unexpected PrefabMetadata size — refusing to rewrite this file");

    const auto* meta = Resource::GetMetadata<Resource::PrefabMetadata>(blob.data());
    const char* text = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));

    // textLength excludes the NUL; dataSize includes it. Trust textLength but
    // clamp to what is actually in the file, so a truncated/hand-edited blob
    // cannot walk off the end.
    const size_t avail   = blob.size() - (sizeof(Resource::AssetHeader) + hdr->metadataSize);
    const size_t textLen = std::min<size_t>(meta->textLength, avail);

    out.path = path;
    out.header.assign(blob.begin(),
                      blob.begin() + static_cast<std::ptrdiff_t>(sizeof(Resource::AssetHeader) + hdr->metadataSize));
    out.lines = SplitPayloadLines(text, textLen);
    ReparseNodes(out);

    if (out.nodes.empty())
        return fail("no 'N' node lines found in the payload");

    out.valid = true;
    return true;
}

bool SavePrefabDoc(const PrefabDoc& doc, std::string* err)
{
    const auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };

    if (!doc.valid ||
        doc.header.size() < sizeof(Resource::AssetHeader) + sizeof(Resource::PrefabMetadata))
        return fail("no prefab loaded");

    std::string text;
    for (const std::string& l : doc.lines) { text += l; text += '\n'; }

    // Back up first. Non-fatal: a missing .bak must not stop the user from
    // saving, but they should be told it did not happen.
    std::error_code ec;
    std::filesystem::copy_file(Sys::U8Path(doc.path), Sys::U8Path(doc.path + ".bak"),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        LOG_WARNING("ComponentTransfer: could not write backup '%s.bak' (%s) — saving anyway",
                    doc.path.c_str(), ec.message().c_str());

    // Patch only the two size fields; nodeCount is unchanged because this API
    // never adds or removes an 'N' line.
    std::vector<uint8_t> head = doc.header;
    auto* hdr  = reinterpret_cast<Resource::AssetHeader*>(head.data());
    auto* meta = reinterpret_cast<Resource::PrefabMetadata*>(head.data() + sizeof(Resource::AssetHeader));
    meta->textLength = static_cast<uint32_t>(text.size());
    hdr->dataSize    = static_cast<uint32_t>(text.size() + 1);

    std::ofstream o = Sys::OpenOfstream(doc.path, std::ios::binary | std::ios::trunc);
    if (!o) return fail("cannot open '" + doc.path + "' for writing");
    o.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    o.write(text.data(), static_cast<std::streamsize>(text.size()));
    const char nul = '\0';
    o.write(&nul, 1);
    if (!o.good()) return fail("write error on '" + doc.path + "'");

    LOG_SUCCESS("ComponentTransfer: wrote '%s' — %zu nodes, %u bytes",
                doc.path.c_str(), doc.nodes.size(), static_cast<uint32_t>(text.size()));
    return true;
}

std::string GetNodeComponentBlob(const PrefabDoc& doc, size_t nodeIdx, const std::string& tag)
{
    if (nodeIdx >= doc.nodes.size()) return {};
    for (const PrefabDoc::Comp& c : doc.nodes[nodeIdx].comps)
    {
        if (c.tag != tag) continue;
        std::string out;
        for (size_t li : c.lines)
            if (li < doc.lines.size()) { out += doc.lines[li]; out += '\n'; }
        return out;
    }
    return {};
}

bool SetNodeComponentBlob(PrefabDoc& doc, size_t nodeIdx,
                          const std::string& tag, const std::string& blob)
{
    if (!doc.valid || nodeIdx >= doc.nodes.size() || tag.empty()) return false;
    const PrefabDoc::Node& node = doc.nodes[nodeIdx];

    std::vector<std::string> incoming;
    {
        std::istringstream ss(blob);
        std::string l;
        while (std::getline(ss, l))
        {
            while (!l.empty() && (l.back() == '\r' || l.back() == '\0')) l.pop_back();
            if (!l.empty()) incoming.push_back(std::move(l));
            l.clear();
        }
    }

    // Lines this tag currently occupies (ascending: ReparseNodes appends in file
    // order).
    std::vector<size_t> drop;
    for (const PrefabDoc::Comp& c : node.comps)
        if (c.tag == tag) { drop = c.lines; break; }

    // Where the replacement goes: over the first existing line, or immediately
    // after the node's LAST content line. Not `endLine`: that is the next node's
    // header, and appending there would silently move the component into the
    // following node when a file has no blank separators.
    size_t insertAt;
    if (!drop.empty())
    {
        insertAt = drop.front();
    }
    else
    {
        size_t last = node.headerLine;
        for (const PrefabDoc::Comp& c : node.comps)
            for (size_t li : c.lines) last = std::max(last, li);
        insertAt = last + 1;
    }

    // Rebuild rather than splice: erase-then-insert on a shared index list is
    // where off-by-one line corruption lives, and this runs once per click.
    std::vector<std::string> out;
    out.reserve(doc.lines.size() + incoming.size());
    for (size_t i = 0; i < doc.lines.size(); ++i)
    {
        if (i == insertAt)
            for (const std::string& l : incoming) out.push_back(l);
        if (std::find(drop.begin(), drop.end(), i) != drop.end()) continue;
        out.push_back(doc.lines[i]);
    }
    if (insertAt >= doc.lines.size())
        for (const std::string& l : incoming) out.push_back(l);

    doc.lines = std::move(out);
    ReparseNodes(doc);
    return true;
}

const std::string* TagForType(const ComponentSerializerRegistry& reg, std::type_index ti)
{
    const auto& all = reg.All();
    const auto  it  = all.find(ti);
    if (it == all.end() || !it->second.serialize || !it->second.deserialize) return nullptr;
    return &it->second.tag;
}

std::type_index TypeForTag(const ComponentSerializerRegistry& reg,
                           const std::string& tag, bool* found)
{
    for (const auto& [ti, s] : reg.All())
        if (s.tag == tag)
        {
            if (found) *found = true;
            return ti;
        }
    if (found) *found = false;
    return std::type_index(typeid(void));
}

} // namespace Editor::ComponentTransfer
