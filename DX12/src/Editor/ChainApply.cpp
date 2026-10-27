// ChainApply.cpp - the Chain Physics Preview's .ipfb write; see the header.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/ChainApply.h"

#include "Resource/AssetHeader.h"
#include "Resource/SkeletonAsset.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace CT = Editor::ComponentTransfer;

namespace Editor::ChainApply
{
namespace
{
    // PrefabSerializer's PercentDecode (space %20, literal %25) - a SceneRef
    // path is percent-encoded on the wire ("GirlsFrontline%20SoppoDefault").
    std::string PercentDecode(const std::string& s)
    {
        std::string r;
        r.reserve(s.size());
        for (std::size_t i = 0; i < s.size();)
        {
            if (s[i] == '%' && i + 2 < s.size())
            {
                const char hex[3] = { s[i + 1], s[i + 2], '\0' };
                char* end = nullptr;
                const long v = std::strtol(hex, &end, 16);
                if (end == hex + 2) { r += static_cast<char>(v); i += 3; continue; }
            }
            r += s[i++];
        }
        return r;
    }

    // "  Tag: k=v k=v" -> the value of `key` (first match), "" when absent.
    std::string LineValue(const std::string& line, const char* key)
    {
        const std::string want = std::string(" ") + key + "=";
        const std::size_t p = line.find(want);
        if (p == std::string::npos) return {};
        const std::size_t b = p + want.size();
        const std::size_t e = line.find(' ', b);
        return line.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }

    const CT::PrefabDoc::Comp* FindComp(const CT::PrefabDoc::Node& n, const char* tag)
    {
        for (const CT::PrefabDoc::Comp& c : n.comps)
            if (c.tag == tag) return &c;
        return nullptr;
    }

    std::vector<std::uint8_t> ReadAll(const std::string& path)
    {
        std::ifstream f = Sys::OpenIfstream(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        const std::streamoff n = f.tellg();
        if (n <= 0) return {};
        std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(b.data()), n);
        return f ? b : std::vector<std::uint8_t>{};
    }
}

std::uint32_t BoneNameHash(const SkeletonAsset& skel)
{
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint8_t b) { h = (h ^ b) * 16777619u; };
    // The REAL bones: the rig the .iskel on disk has (a model's virtual bones are
    // appended at load and never written to it).
    const std::uint32_t n = skel.RealBoneCount();
    for (int k = 0; k < 4; ++k) mix(static_cast<std::uint8_t>(n >> (8 * k)));
    for (std::uint32_t b = 0; b < n; ++b)
    {
        for (const char* s = skel.boneNames[b]; *s; ++s) mix(static_cast<std::uint8_t>(*s));
        mix(0);   // separator: "ab"+"c" must not hash like "a"+"bc"
    }
    return h;
}

int FindChainNode(const CT::PrefabDoc& doc, const std::string& carrierName, std::string* why)
{
    std::vector<int> withChain;
    for (std::size_t i = 0; i < doc.nodes.size(); ++i)
        if (FindComp(doc.nodes[i], "ChainPhysics")) withChain.push_back(static_cast<int>(i));
    if (withChain.size() == 1) return withChain.front();
    std::vector<int> named;
    const std::vector<int>& pool = withChain;
    if (!pool.empty())
    {
        for (int i : pool)
            if (doc.nodes[static_cast<std::size_t>(i)].name == carrierName) named.push_back(i);
    }
    else
    {
        for (std::size_t i = 0; i < doc.nodes.size(); ++i)
            if (doc.nodes[i].name == carrierName && FindComp(doc.nodes[i], "SceneRef"))
                named.push_back(static_cast<int>(i));
    }
    if (named.size() == 1) return named.front();
    if (why)
        *why = withChain.empty()
            ? (named.empty() ? "no node carries ChainPhysics and none named '" + carrierName +
                                   "' has a SceneRef"
                             : "several nodes named '" + carrierName + "' carry a SceneRef")
            : (named.empty() ? std::to_string(withChain.size()) + " nodes carry ChainPhysics and "
                                   "none is named '" + carrierName + "'"
                             : std::to_string(named.size()) + " ChainPhysics nodes are named '" +
                                   carrierName + "'");
    return -1;
}

std::string SkeletonPathOfNode(const CT::PrefabDoc& doc, int node)
{
    if (node < 0 || node >= static_cast<int>(doc.nodes.size())) return {};
    const CT::PrefabDoc::Comp* sr = FindComp(doc.nodes[static_cast<std::size_t>(node)], "SceneRef");
    if (!sr || sr->lines.empty() || sr->lines.front() >= doc.lines.size()) return {};
    const std::string iscn = PercentDecode(LineValue(doc.lines[sr->lines.front()], "path"));
    if (iscn.empty()) return {};
    // The .iscn is a binary container whose payload is text lines; the "K
    // file=" line names the .iskel beside it (SceneInstanceLoader::ParseKLine).
    const std::vector<std::uint8_t> b = ReadAll(iscn);
    if (b.empty()) return {};
    const std::string text(b.begin(), b.end());
    std::size_t p = 0;
    while ((p = text.find("K file=", p)) != std::string::npos)
    {
        if (p == 0 || text[p - 1] == '\n' || text[p - 1] == '\0')
        {
            std::size_t e = text.find_first_of("\r\n", p);
            std::string f = text.substr(p + 7, e == std::string::npos ? std::string::npos : e - p - 7);
            while (!f.empty() && (f.back() == ' ' || f.back() == '\0')) f.pop_back();
            if (f.empty()) return {};
            const std::filesystem::path dir = Sys::U8Path(iscn).parent_path();
            return Sys::PathToUtf8(dir / Sys::U8Path(f));
        }
        p += 7;
    }
    return {};
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
CapsuleBonesOfNode(const CT::PrefabDoc& doc, int node)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    if (node < 0 || node >= static_cast<int>(doc.nodes.size())) return out;
    const CT::PrefabDoc::Comp* cc = FindComp(doc.nodes[static_cast<std::size_t>(node)], "CapsuleCollider");
    if (!cc || cc->lines.empty() || cc->lines.front() >= doc.lines.size()) return out;
    const std::string& l = doc.lines[cc->lines.front()];
    const int count = std::atoi(LineValue(l, "count").c_str());
    for (int i = 0; i < count && i < 64; ++i)
    {
        const std::string k = "c" + std::to_string(i) + "_";
        const std::string en = LineValue(l, (k + "enabled").c_str());
        if (!en.empty() && en == "0") continue;
        const std::string a = LineValue(l, (k + "boneA").c_str());
        const std::string bb = LineValue(l, (k + "boneB").c_str());
        if (a.empty() || bb.empty()) continue;
        out.emplace_back(static_cast<std::uint32_t>(std::strtoul(a.c_str(), nullptr, 10)),
                         static_cast<std::uint32_t>(std::strtoul(bb.c_str(), nullptr, 10)));
    }
    return out;
}

PrefabWrite WriteChainBlob(const std::string& path, const std::string& carrierName,
                           const std::string& blob)
{
    PrefabWrite w;
    CT::PrefabDoc doc;
    if (!CT::LoadPrefabDoc(path, doc, &w.err)) return w;
    w.node = FindChainNode(doc, carrierName, &w.err);
    if (w.node < 0) return w;
    const std::string before = CT::GetNodeComponentBlob(doc, static_cast<std::size_t>(w.node),
                                                        "ChainPhysics");
    w.unchanged = before == blob;
    if (!CT::SetNodeComponentBlob(doc, static_cast<std::size_t>(w.node), "ChainPhysics", blob))
    {
        w.err = "SetNodeComponentBlob refused the blob";
        return w;
    }
    if (!CT::SavePrefabDoc(doc, &w.err)) return w;
    w.ok = true;

    // ---- Verify what is ON DISK, not what we meant to write ----
    std::error_code ec;
    w.bak = std::filesystem::exists(Sys::U8Path(path + ".bak"), ec);
    const std::vector<std::uint8_t> b = ReadAll(path);
    w.fileBytes = b.size();
    constexpr std::size_t kHead = sizeof(Resource::AssetHeader) + sizeof(Resource::PrefabMetadata);
    if (b.size() >= kHead)
    {
        const auto* hdr  = reinterpret_cast<const Resource::AssetHeader*>(b.data());
        const auto* meta = reinterpret_cast<const Resource::PrefabMetadata*>(
            b.data() + sizeof(Resource::AssetHeader));
        w.dataSize   = hdr->dataSize;
        w.textLength = meta->textLength;
        w.sizeOk = hdr->dataSize == meta->textLength + 1u &&
                   b.size() == kHead + static_cast<std::size_t>(hdr->dataSize) &&
                   b.back() == 0;
    }
    CT::PrefabDoc again;
    std::string err2;
    if (CT::LoadPrefabDoc(path, again, &err2))
    {
        std::string why;
        const int n2 = FindChainNode(again, carrierName, &why);
        w.roundTrip = n2 == w.node &&
                      CT::GetNodeComponentBlob(again, static_cast<std::size_t>(n2), "ChainPhysics") == blob;
    }
    return w;
}

} // namespace Editor::ChainApply
