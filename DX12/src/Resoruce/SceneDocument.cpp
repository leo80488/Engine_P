#include "Resource/SceneDocument.h"
#include "Resource/AuthoringSchema.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"
#include "Resource/ComponentSerializers.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>

namespace Resource
{
namespace
{
    constexpr std::size_t kWorldMetaSize = 16;   // nodeCount, textLength, pad[2] (SaveScene's WorldMeta)

    std::uint32_t ReadU32(const std::vector<std::uint8_t>& b, std::size_t off)
    {
        std::uint32_t v = 0;
        if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
        return v;
    }
    void WriteU32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v)
    {
        if (off + 4 <= b.size()) std::memcpy(b.data() + off, &v, 4);
    }

    bool IsComponentLine(const std::string& l)
    {
        return l.size() > 2 && l[0] == ' ' && l[1] == ' ';
    }

    // The `key=value` tokens of @p line from @p start: every byte that is not
    // a replaced value is kept, whitespace included, so an edit changes exactly
    // the values it names. @p missing receives the keys the line lacks.
    std::string ReplaceValues(const std::string& line, std::size_t start,
                              const std::vector<std::pair<std::string, std::string>>& values,
                              std::vector<std::string>* missing)
    {
        std::string out = line.substr(0, start);
        std::vector<bool> used(values.size(), false);
        std::size_t i = start;
        while (i < line.size())
        {
            if (std::isspace(static_cast<unsigned char>(line[i]))) { out.push_back(line[i]); ++i; continue; }
            std::size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            const std::string tok = line.substr(i, j - i);
            const std::size_t eq = tok.find('=');
            bool replaced = false;
            if (eq != std::string::npos && eq > 0)
            {
                const std::string key = tok.substr(0, eq);
                for (std::size_t k = 0; k < values.size(); ++k)
                    if (values[k].first == key)
                    {
                        out += key + "=" + values[k].second;
                        used[k] = replaced = true;
                        break;
                    }
            }
            if (!replaced) out += tok;
            i = j;
        }
        if (missing)
            for (std::size_t k = 0; k < values.size(); ++k)
                if (!used[k]) missing->push_back(values[k].first);
        return out;
    }

    std::vector<std::pair<std::string, std::string>> Tokens(const std::string& line, std::size_t start)
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::size_t i = start;
        while (i < line.size())
        {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            std::size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            if (j > i)
            {
                const std::string tok = line.substr(i, j - i);
                const std::size_t eq = tok.find('=');
                if (eq != std::string::npos && eq > 0) out.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
            }
            i = j;
        }
        return out;
    }

    std::string Fixed6(float v)
    {
        char b[48];
        std::snprintf(b, sizeof(b), "%.6f", v);
        return b;
    }

    // Reference stamps ($g_<key>=<guid>) for these lines, the way SaveScene
    // stamps a whole scene: AugmentSceneText is idempotent and per line.
    std::vector<std::string> Augment(const std::vector<std::string>& lines)
    {
        std::string text;
        for (const std::string& l : lines) { text += l; text += '\n'; }
        const std::string out = AssetRefs::AugmentSceneText(text);
        std::vector<std::string> result;
        std::size_t start = 0;
        while (start < out.size())
        {
            const std::size_t nl = out.find('\n', start);
            if (nl == std::string::npos) { result.push_back(out.substr(start)); break; }
            result.push_back(out.substr(start, nl - start));
            start = nl + 1;
        }
        return result;
    }
}

// ==== Load / save ====

bool SceneDocument::LoadFile(const std::string& utf8Path, std::string& error)
{
    std::ifstream f(Sys::U8Path(utf8Path), std::ios::binary);
    if (!f) { error = "cannot open '" + utf8Path + "'"; return false; }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return LoadBytes(bytes, error);
}

bool SceneDocument::LoadBytes(const std::vector<std::uint8_t>& bytes, std::string& error)
{
    *this = SceneDocument{};
    if (bytes.size() < sizeof(AssetHeader)) { error = "shorter than an asset header"; return false; }
    std::memcpy(&m_header, bytes.data(), sizeof(AssetHeader));
    if (m_header.magic != MAGIC_WORLD) { error = "not a scene: the magic is not RWLD"; return false; }
    const std::size_t metaEnd    = sizeof(AssetHeader) + static_cast<std::size_t>(m_header.metadataSize);
    const std::size_t payloadEnd = metaEnd + static_cast<std::size_t>(m_header.dataSize);
    if (metaEnd > bytes.size() || payloadEnd > bytes.size())
    {
        error = "the header's sizes run past the end of the file";
        return false;
    }
    m_meta.assign(bytes.begin() + sizeof(AssetHeader), bytes.begin() + static_cast<std::ptrdiff_t>(metaEnd));
    const char* payload = reinterpret_cast<const char*>(bytes.data() + metaEnd);
    const std::size_t payloadSize = m_header.dataSize;
    std::size_t textLen = 0;
    while (textLen < payloadSize && payload[textLen] != '\0') ++textLen;
    const std::string text(payload, textLen);
    m_payloadTail.assign(payload + textLen, payloadSize - textLen);
    m_fileTail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payloadEnd), bytes.end());

    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { m_lines.push_back(text.substr(start)); break; }
        m_lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    m_finalNewline = !text.empty() && text.back() == '\n';
    Reindex();
    return true;
}

std::vector<std::uint8_t> SceneDocument::Bytes() const
{
    std::string text;
    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        if (i) text += '\n';
        text += m_lines[i];
    }
    if (m_finalNewline) text += '\n';

    AssetHeader h = m_header;
    std::vector<std::uint8_t> meta = m_meta;
    if (m_edited)
    {
        h.dataSize = static_cast<std::uint32_t>(text.size() + m_payloadTail.size());
        if (meta.size() >= 8)
        {
            const std::int64_t count = static_cast<std::int64_t>(ReadU32(meta, 0)) + m_nodeDelta;
            WriteU32(meta, 0, static_cast<std::uint32_t>(count < 0 ? 0 : count));
            WriteU32(meta, 4, static_cast<std::uint32_t>(text.size()));
        }
    }
    std::vector<std::uint8_t> out;
    out.reserve(sizeof(AssetHeader) + meta.size() + text.size() + m_payloadTail.size() + m_fileTail.size());
    const auto* hp = reinterpret_cast<const std::uint8_t*>(&h);
    out.insert(out.end(), hp, hp + sizeof(AssetHeader));
    out.insert(out.end(), meta.begin(), meta.end());
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), m_payloadTail.begin(), m_payloadTail.end());
    out.insert(out.end(), m_fileTail.begin(), m_fileTail.end());
    return out;
}

std::uint32_t SceneDocument::NodeCount() const      { return ReadU32(m_meta, 0); }
std::uint32_t SceneDocument::MetaTextLength() const { return ReadU32(m_meta, 4); }

std::size_t SceneDocument::TextSize() const
{
    std::size_t n = 0;
    for (const std::string& l : m_lines) n += l.size() + 1;
    if (!m_lines.empty() && !m_finalNewline) --n;
    return n;
}

std::vector<std::pair<std::string, std::string>> SceneDocument::World() const
{
    for (const std::string& l : m_lines)
        if (l.size() > 1 && l[0] == 'W' && l[1] == ' ')
        {
            auto t = Tokens(l, 2);
            for (auto& [k, v] : t) v = SceneText::DecodeText(v);
            return t;
        }
    return {};
}

// A component line belongs to the most recent N line - the loader's rule.
void SceneDocument::Reindex()
{
    m_nodes.clear();
    int current = -1;
    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        const std::string& l = m_lines[i];
        SceneText::NodeLine nl;
        if (SceneText::ParseNodeLine(l, nl))
        {
            Node n;
            n.fields = nl;
            n.line   = i;
            m_nodes.push_back(std::move(n));
            current = static_cast<int>(m_nodes.size()) - 1;
            continue;
        }
        std::string tag;
        KVMap kv;
        if (current >= 0 && IsComponentLine(l) && SceneText::ParseComponentLine(l, tag, kv))
        {
            m_nodes[current].componentLines.push_back(i);
            m_nodes[current].tags.push_back(tag);
        }
    }
}

int SceneDocument::Find(const std::string& ref, std::string& error) const
{
    const bool numeric = !ref.empty() &&
        std::all_of(ref.begin() + (ref[0] == '-' ? 1 : 0), ref.end(), [](unsigned char c) { return std::isdigit(c); });
    if (numeric && ref != "-")
    {
        const int idx = std::atoi(ref.c_str());
        for (std::size_t i = 0; i < m_nodes.size(); ++i)
            if (m_nodes[i].fields.idx == idx) return static_cast<int>(i);
        error = "no node has idx " + ref;
        return -1;
    }
    std::vector<int> hits;
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].fields.name == ref) hits.push_back(static_cast<int>(i));
    if (hits.size() == 1) return hits[0];
    if (hits.empty()) { error = "no node is named '" + ref + "'"; return -1; }
    error = std::to_string(hits.size()) + " nodes are named '" + ref + "' (idx";
    for (int h : hits) error += " " + std::to_string(m_nodes[h].fields.idx);
    error += "): name one by its idx";
    return -1;
}

// ==== Edits ====

bool SceneDocument::SetTransform(int node, const float* t, const float* q, const float* s, std::string& error)
{
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) { error = "no such node"; return false; }
    Authoring::Values inputs;
    auto vectorText = [&](const char* key, const float* v, int count) {
        if (!v) return;
        std::string text;
        for (int i = 0; i < count; ++i)
        {
            char b[48]; std::snprintf(b, sizeof(b), "%.17g", double(v[i]));
            if (i) text += '_';
            text += b;
        }
        inputs.emplace_back(key, text);
    };
    vectorText("position", t, 3); vectorText("rotation", q, 4); vectorText("scale", s, 3);
    if (!Authoring::Validate("Transform", inputs, error)) return false;
    float normalized[4];
    if (q)
    {
        const double length = std::sqrt(double(q[0])*q[0] + double(q[1])*q[1] + double(q[2])*q[2] + double(q[3])*q[3]);
        for (int i = 0; i < 4; ++i) normalized[i] = float(q[i] / length);
        q = normalized;
    }
    std::vector<std::pair<std::string, std::string>> values;
    auto put = [&](const float* v, const char* const* keys, int n) -> bool {
        if (!v) return true;
        for (int i = 0; i < n; ++i)
        {
            if (!std::isfinite(v[i])) { error = std::string(keys[i]) + " is not a finite number"; return false; }
            values.emplace_back(keys[i], Fixed6(v[i]));
        }
        return true;
    };
    static const char* const kT[] = { "tx", "ty", "tz" };
    static const char* const kQ[] = { "qx", "qy", "qz", "qw" };
    static const char* const kS[] = { "sx", "sy", "sz" };
    if (!put(t, kT, 3) || !put(q, kQ, 4) || !put(s, kS, 3)) return false;
    if (values.empty()) return true;
    std::vector<std::string> missing;
    const std::size_t li = m_nodes[node].line;
    const std::string edited = ReplaceValues(m_lines[li], 2, values, &missing);
    if (!missing.empty()) { error = "the N line has no " + missing[0]; return false; }
    m_lines[li] = edited;
    m_edited = true;
    Reindex();
    return true;
}

bool SceneDocument::Rename(int node, const std::string& name, std::string& error)
{
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) { error = "no such node"; return false; }
    if (name.empty()) { error = "a name cannot be empty"; return false; }
    std::vector<std::string> missing;
    const std::size_t li = m_nodes[node].line;
    const std::string edited = ReplaceValues(m_lines[li], 2, { { "name", SceneText::EncodeText(name) } }, &missing);
    if (!missing.empty()) { error = "the N line has no name"; return false; }
    m_lines[li] = edited;
    m_edited = true;
    Reindex();
    return true;
}

bool SceneDocument::SetComponentValues(int node, const std::string& tag,
                                       const std::vector<std::pair<std::string, std::string>>& values,
                                       std::string& error, std::string* errorCode)
{
    if (errorCode) *errorCode = "unknown_key";
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) { error = "no such node"; return false; }
    const Node& n = m_nodes[node];
    std::vector<std::size_t> lines;
    for (std::size_t i = 0; i < n.tags.size(); ++i)
        if (n.tags[i] == tag) lines.push_back(n.componentLines[i]);
    if (lines.empty()) { error = "node '" + n.fields.name + "' has no " + tag + " line"; return false; }
    if (lines.size() > 1)
    {
        error = tag + " is " + std::to_string(lines.size()) + " lines (slots) on this node: a key does not name one value";
        return false;
    }
    const std::string& line = m_lines[lines[0]];
    const std::size_t colon = line.find(':', 2);
    std::vector<std::string> missing;
    if (!Authoring::Fields(tag).empty())
        for (const auto& [key, value] : values)
            if (!Authoring::KnownField(tag, key)) { error = tag + " has no authoring key '" + key + "'"; return false; }
    std::string edited = ReplaceValues(line, colon + 1, values, &missing);
    // A known schema field the line lacks (the file predates the field) is still
    // writable: append only the requested keys and keep every other byte,
    // unknown tokens included.
    missing.erase(std::remove_if(missing.begin(), missing.end(), [&](const std::string& key) {
        if (!Authoring::KnownField(tag, key)) return false;
        for (const auto& [k, v] : values) if (k == key) { edited += " " + k + "=" + v; break; }
        return true;
    }), missing.end());
    if (!missing.empty())
    {
        std::string have;
        for (const auto& [k, v] : Tokens(line, colon + 1))
            if (k.rfind("$g_", 0) != 0) { if (!have.empty()) have += ", "; have += k; }
        error = tag + " has no key '" + missing[0] + "' (the line has: " + have + ")";
        return false;
    }
    auto merged = Tokens(edited, colon + 1);
    Authoring::FillDefaults(tag, merged);
    if (!Authoring::Validate(tag, merged, error) || !Authoring::ValidateSubmitted(tag, values, merged, error))
    {
        if (errorCode) *errorCode = "bad_value";
        return false;
    }
    m_lines[lines[0]] = Augment({ edited }).front();
    m_edited = true;
    Reindex();
    return true;
}

int SceneDocument::AddNode(int parentIdx, const std::string& name, const float t[3], const float q[4],
                           const float s[3], const std::vector<std::string>& componentLines, std::string& error)
{
    if (name.empty()) { error = "a node needs a name"; return -1; }
    // SaveScene numbers EVERY entity (nodeCount of them), prefab members
    // included, and writes only the roots: an idx a member had can still be
    // some node's parent. So the new idx is past every number the file uses
    // anywhere - idx, parent, override keys - and past nodeCount.
    int maxIdx = static_cast<int>(NodeCount()) - 1;
    bool parentFound = parentIdx == -1;
    for (const Node& n : m_nodes)
    {
        maxIdx = std::max(maxIdx, std::max(n.fields.idx, n.fields.parent));
        if (n.fields.idx == parentIdx) parentFound = true;
    }
    for (const std::string& l : m_lines)
    {
        if (l.empty() || l[0] == '#' || l[0] == 'N' || l[0] == 'W' || IsComponentLine(l)) continue;
        for (const auto& [k, v] : Tokens(l, 0))
            if (k == "idx" || k == "root") maxIdx = std::max(maxIdx, std::atoi(v.c_str()));
    }
    if (!parentFound) { error = "no node has idx " + std::to_string(parentIdx) + " (the parent)"; return -1; }
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(t[i]) || !std::isfinite(s[i])) { error = "position / scale must be finite"; return -1; }
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(q[i])) { error = "rotation must be finite"; return -1; }
    const double length = std::sqrt(double(q[0])*q[0] + double(q[1])*q[1] + double(q[2])*q[2] + double(q[3])*q[3]);
    if (length < 1e-6) { error = "Transform.rotation: quaternion has zero length"; return -1; }

    SceneText::NodeLine nl;
    nl.idx = maxIdx + 1;
    nl.parent = parentIdx;
    std::copy(t, t + 3, nl.t);
    for (int i = 0; i < 4; ++i) nl.q[i] = static_cast<float>(q[i] / length);
    std::copy(s, s + 3, nl.s);
    nl.name = name;
    std::vector<std::string> block{ SceneText::FormatNodeLine(nl) };
    for (const std::string& c : componentLines)
    {
        std::string tag;
        KVMap kv;
        if (!IsComponentLine(c) || !SceneText::ParseComponentLine(c, tag, kv))
        {
            error = "not a component line (\"  Tag: k=v ...\"): '" + c + "'";
            return -1;
        }
        if (!GetComponentRegistry().FindByTag(tag)) { error = "unknown component '" + tag + "'"; return -1; }
        if (!Authoring::Fields(tag).empty())
        {
            Authoring::Values values;
            for (const auto& [key, value] : kv)
            {
                if (!Authoring::KnownField(tag, key)) { error = tag + "." + key + ": unknown authoring key"; return -1; }
                values.emplace_back(key, value);
            }
            Authoring::FillDefaults(tag, values);
            if (!Authoring::Validate(tag, values, error) || !Authoring::ValidateSubmitted(tag, values, values, error)) return -1;
            std::string line = "  " + tag + ":";
            for (const auto& [key, value] : values) line += " " + key + "=" + value;
            block.push_back(std::move(line));
        }
        else block.push_back(c);
    }
    block = Augment(block);

    std::size_t pos = m_lines.size();
    if (!m_nodes.empty())
    {
        const Node& last = m_nodes.back();
        pos = last.componentLines.empty() ? last.line : std::max(last.line, last.componentLines.back());
        ++pos;
    }
    else
        for (std::size_t i = 0; i < m_lines.size(); ++i)
            if (m_lines[i].size() > 1 && m_lines[i][0] == 'W' && m_lines[i][1] == ' ') { pos = i + 1; break; }
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(pos), block.begin(), block.end());
    if (pos == m_lines.size() - block.size() && !m_finalNewline && !m_lines.empty()) m_finalNewline = true;
    ++m_nodeDelta;
    m_edited = true;
    Reindex();
    return nl.idx;
}

bool SceneDocument::RemoveNode(int node, std::string& error)
{
    if (node < 0 || node >= static_cast<int>(m_nodes.size())) { error = "no such node"; return false; }
    const Node n = m_nodes[node];
    const int idx = n.fields.idx;
    for (const Node& o : m_nodes)
        if (o.fields.parent == idx)
        {
            error = "node " + std::to_string(o.fields.idx) + " ('" + o.fields.name + "') is its child: remove or re-parent it first";
            return false;
        }
    const std::string idxText = std::to_string(idx);
    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        const std::string& l = m_lines[i];
        if (l.empty() || l[0] == '#' || l[0] == 'N' || l[0] == 'W' || IsComponentLine(l)) continue;
        for (const auto& [k, v] : Tokens(l, 0))
            if ((k == "idx" || k == "root") && v == idxText)
            {
                error = "line " + std::to_string(i + 1) + " (an override keyed by this node) refers to it";
                return false;
            }
    }
    std::vector<std::size_t> drop = n.componentLines;
    drop.push_back(n.line);
    std::sort(drop.begin(), drop.end(), std::greater<std::size_t>());
    for (std::size_t li : drop) m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(li));
    --m_nodeDelta;
    m_edited = true;
    Reindex();
    return true;
}

// ==== Validation ====

std::vector<SceneDocument::Finding> SceneDocument::Validate(bool references) const
{
    std::vector<Finding> out;
    auto add = [&](bool error, const char* code, std::size_t line, std::string message) {
        out.push_back(Finding{ error, code, line, std::move(message) });
    };

    if (m_header.version > CurrentVersion(MAGIC_WORLD) || m_header.version < MinimumVersion(MAGIC_WORLD))
        add(true, "bad_version", 0, "header version " + std::to_string(m_header.version) +
                                    ", this engine reads " + std::to_string(MinimumVersion(MAGIC_WORLD)) + ".." +
                                    std::to_string(CurrentVersion(MAGIC_WORLD)));
    if (m_header.metadataSize != kWorldMetaSize)
        add(false, "meta_size", 0, "metadataSize is " + std::to_string(m_header.metadataSize) +
                                   " (SaveScene writes " + std::to_string(kWorldMetaSize) + ")");
    if (m_meta.size() >= 8 && MetaTextLength() != TextSize())
        add(false, "meta_text_length", 0, "WorldMeta.textLength says " + std::to_string(MetaTextLength()) +
                                          ", the text is " + std::to_string(TextSize()) + " bytes");
    if (m_payloadTail != std::string(1, '\0'))
        add(false, "payload_tail", 0, "the text is not followed by exactly one NUL");
    if (!m_fileTail.empty())
        add(false, "trailing_bytes", 0, std::to_string(m_fileTail.size()) + " bytes after the payload");

    bool sawWorld = false, sawNode = false;
    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        const std::string& l = m_lines[i];
        if (l.size() > 1 && l[0] == 'W' && l[1] == ' ') sawWorld = true;
        if (l.size() > 1 && l[0] == 'N' && l[1] == ' ')
        {
            SceneText::NodeLine nl;
            if (!SceneText::ParseNodeLine(l, nl)) add(true, "bad_node_line", i + 1, "an N line the loader skips (no idx)");
            else sawNode = true;
        }
        else if (IsComponentLine(l))
        {
            std::string tag;
            KVMap kv;
            if (!SceneText::ParseComponentLine(l, tag, kv))
                add(true, "bad_component_line", i + 1, "an indented line that is not \"  Tag: k=v\"");
            else if (!sawNode)
                add(true, "orphan_component", i + 1, tag + " comes before any N line: the loader drops it");
            else if (!GetComponentRegistry().FindByTag(tag))
                add(true, "unknown_component", i + 1, "'" + tag + "' is no component this engine knows: it is dropped on load");
        }
    }
    if (!sawWorld) add(false, "no_world_line", 0, "no W line (scene name and settings)");

    std::set<int> ids;
    for (const Node& n : m_nodes)
    {
        const auto& f = n.fields;
        if (!ids.insert(f.idx).second)
            add(true, "duplicate_idx", n.line + 1, "idx " + std::to_string(f.idx) + " is used twice");
        bool finite = true;
        for (float v : f.t) finite = finite && std::isfinite(v);
        for (float v : f.q) finite = finite && std::isfinite(v);
        for (float v : f.s) finite = finite && std::isfinite(v);
        if (!finite) { add(true, "bad_number", n.line + 1, "a transform field is not finite"); continue; }
        const float ql = std::sqrt(f.q[0] * f.q[0] + f.q[1] * f.q[1] + f.q[2] * f.q[2] + f.q[3] * f.q[3]);
        if (std::fabs(ql - 1.f) > 1e-3f)
            add(false, "quaternion_not_unit", n.line + 1, "the rotation's length is " + Fixed6(ql));
    }
    for (const Node& n : m_nodes)
        if (n.fields.parent != -1 && !ids.count(n.fields.parent))
            add(false, "parent_unknown", n.line + 1, "parent " + std::to_string(n.fields.parent) +
                                                     " is not a node of this file (a prefab member?)");

    if (references)
    {
        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            const std::string& l = m_lines[i];
            if (l.empty() || l[0] == '#' || l[0] == 'N') continue;
            std::size_t start = 0;
            if (IsComponentLine(l)) start = l.find(':', 2) + 1;
            else if (l.size() > 1 && l[1] == ' ') start = 2;
            const auto toks = Tokens(l, start);
            for (const auto& [k, v] : toks)
            {
                if (k.rfind("$g_", 0) == 0) continue;
                std::string path = SceneText::DecodeText(v);
                if (path.rfind("t:", 0) == 0) path = path.substr(2);
                if (!AssetRefs::ValueIsAssetPath(path)) continue;
                // Exact: the stored spelling, no relocation retry - but through
                // the search roots, so a worktree reading the main checkout's
                // payload (the shared content root) does not report it missing.
                if (Resource::AssetFS::Get().ExistsExact(path)) continue;
                const bool stamped = std::any_of(toks.begin(), toks.end(),
                                                 [&](const auto& t) { return t.first == "$g_" + k; });
                add(!stamped, stamped ? "reference_missing_guid" : "reference_missing", i + 1,
                    k + "=" + path + (stamped ? " does not exist (the loader will try its guid)"
                                              : " does not exist"));
            }
        }
    }
    return out;
}
}
