// The .iui codec; the format and its contracts are described in UI/UIAsset.h.
#include "UI/UIAsset.h"

#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"           // PercentDecode
#include "Resource/AuthoringSchema.h"     // the Cv* contracts
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"      // ParseLine, RewriteFromKV
#include "System/Log.h"
#include "System/PathUtil.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"            // IsUINode, UIChildrenOf

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace UI
{
namespace
{
    using Json = nlohmann::json;

    // ==== The renaming layer: .iui fields <-> Cv* serializer keys ====
    enum class Kind : std::uint8_t
    {
        Number,    // one float
        Integer,
        Boolean,
        Enum,      // an integer key; named values in the file
        Text,      // a string the serializer writes with PercentEncodeText (a newline survives)
        Path,      // a string the serializer writes with PercentEncode (an asset path)
        Vec,       // N floats '_'-joined in ONE key ("1_1_1_1")
        Pair,      // two floats in TWO keys (rect anchorMin -> aMinX, aMinY)
    };
    struct FieldDef
    {
        const char*              json;
        Kind                     kind;
        const char*              key;
        const char*              key2  = nullptr;   // Pair
        int                      arity = 1;         // Vec
        std::vector<const char*> names = {};        // Enum, by value
    };
    struct BlockDef
    {
        const char*           json;
        const char*           tag;
        std::vector<FieldDef> fields;   // their order is the canonical order
    };

    // Every key each Cv* serializer writes, but CvRect's "sibling" (a node's "order") and
    // CvParent (the nesting). tests/unit/test_ui_asset.py checks this against the
    // serializers, so a new authored field cannot be forgotten here.
    const std::vector<BlockDef>& Blocks()
    {
        static const std::vector<BlockDef> kBlocks = {
            { "canvas", "CvCanvas", {
                { "renderMode", Kind::Enum, "renderMode", nullptr, 1, { "screen_space_overlay" } },
                { "scaleMode", Kind::Enum, "scaleMode", nullptr, 1, { "constant_pixel_size", "scale_with_screen_size" } },
                { "referenceResolution", Kind::Pair, "refW", "refH" },
                { "match", Kind::Number, "match" },
                { "sortOrder", Kind::Integer, "sortOrder" },
                { "visible", Kind::Boolean, "visible" } } },
            { "rect", "CvRect", {
                { "anchorMin", Kind::Pair, "aMinX", "aMinY" },
                { "anchorMax", Kind::Pair, "aMaxX", "aMaxY" },
                { "pivot", Kind::Pair, "pivX", "pivY" },
                { "size", Kind::Pair, "szX", "szY" },
                { "offset", Kind::Pair, "offX", "offY" },
                { "visible", Kind::Boolean, "visible" } } },
            { "clip", "CvClip", {
                { "enabled", Kind::Boolean, "enabled" } } },
            { "image", "CvImage", {
                { "texture", Kind::Path, "tex" },
                { "color", Kind::Vec, "color", nullptr, 4 },
                { "uv0", Kind::Vec, "uv0", nullptr, 2 },
                { "uv1", Kind::Vec, "uv1", nullptr, 2 },
                { "wrap", Kind::Enum, "wrap", nullptr, 1, { "clamp", "wrap", "mirror" } },
                { "pointFilter", Kind::Boolean, "point" },
                { "slice", Kind::Vec, "slice", nullptr, 4 },
                { "visible", Kind::Boolean, "visible" } } },
            { "text", "CvText", {
                { "text", Kind::Text, "text" },
                { "font", Kind::Path, "font" },
                { "color", Kind::Vec, "color", nullptr, 4 },
                { "fontScale", Kind::Number, "fontScale" },
                { "alignH", Kind::Enum, "alignH", nullptr, 1, { "left", "center", "right" } },
                { "alignV", Kind::Enum, "alignV", nullptr, 1, { "top", "middle", "bottom" } },
                { "wrapWidth", Kind::Number, "wrapW" },
                { "outline", Kind::Boolean, "fxO" },
                { "outlineColor", Kind::Vec, "outlineCol", nullptr, 4 },
                { "outlineWidth", Kind::Number, "outlineW" },
                { "glow", Kind::Boolean, "fxG" },
                { "glowColor", Kind::Vec, "glowCol", nullptr, 4 },
                { "glowWidth", Kind::Number, "glowW" },
                { "shadow", Kind::Boolean, "fxS" },
                { "shadowColor", Kind::Vec, "shadowCol", nullptr, 4 },
                { "shadowOffset", Kind::Pair, "shadowOX", "shadowOY" },
                { "jitter", Kind::Boolean, "fxJ" },
                { "jitterAmplitude", Kind::Number, "jitterA" },
                { "jitterFrequency", Kind::Number, "jitterF" },
                { "customShader", Kind::Path, "customShader" },
                { "customParam0", Kind::Vec, "cpA", nullptr, 4 },
                { "customParam1", Kind::Vec, "cpB", nullptr, 4 },
                { "visible", Kind::Boolean, "visible" } } },
            { "interact", "CvInteract", {
                { "raycast", Kind::Boolean, "raycast" },
                { "disabled", Kind::Boolean, "disabled" },
                { "focusable", Kind::Boolean, "focusable" },
                { "absorbNavX", Kind::Boolean, "absorbNavX" },
                { "tint", Kind::Boolean, "tint" },
                { "normalColor", Kind::Vec, "normal", nullptr, 4 },
                { "hoverColor", Kind::Vec, "hover", nullptr, 4 },
                { "pressedColor", Kind::Vec, "pressed", nullptr, 4 },
                { "disabledColor", Kind::Vec, "disabledCol", nullptr, 4 } } },
            { "sprite", "CvSpriteAnim", {
                { "columns", Kind::Integer, "cols" },
                { "rows", Kind::Integer, "rows" },
                { "frames", Kind::Integer, "frames" },
                { "fps", Kind::Number, "fps" },
                { "loop", Kind::Boolean, "loop" },
                { "playing", Kind::Boolean, "playing" },
                { "pingpong", Kind::Boolean, "pingpong" } } },
        };
        return kBlocks;
    }

    const BlockDef* BlockByJson(std::string_view json)
    {
        for (const BlockDef& b : Blocks())
            if (json == b.json) return &b;
        return nullptr;
    }

    const BlockDef* BlockByTag(std::string_view tag)
    {
        for (const BlockDef& b : Blocks())
            if (tag == b.tag) return &b;
        return nullptr;
    }

    // ==== The serializer's own text forms ====
    // A float exactly as the serializers print one (an ostream at its default precision).
    std::string FloatText(float v)
    {
        if (!std::isfinite(v)) v = 0.f;
        std::ostringstream o;
        o << v;
        return o.str();
    }

    float ParseFloat(const std::string& s)
    {
        return std::strtof(s.c_str(), nullptr);
    }

    long long ParseInt(const std::string& s)
    {
        return std::strtoll(s.c_str(), nullptr, 10);
    }

    std::vector<float> ParseVec(const std::string& s, int n)
    {
        std::vector<float> out(static_cast<std::size_t>(n), 0.f);
        const char* p = s.c_str();
        for (int i = 0; i < n && *p; ++i)
        {
            char* end = nullptr;
            out[static_cast<std::size_t>(i)] = std::strtof(p, &end);
            if (end == p) break;
            p = (*end == '_') ? end + 1 : end;
        }
        return out;
    }

    std::string VecText(const std::vector<float>& v)
    {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            if (i) s += '_';
            s += FloatText(v[i]);
        }
        return s;
    }

    // ComponentSerializers.cpp's PercentEncodeText (the text) and PercentEncode (a path);
    // both decode with the one PercentDecode, so only the bytes they escape differ.
    std::string EncodeText(const std::string& s)
    {
        static const char* kHex = "0123456789ABCDEF";
        std::string r;
        for (const unsigned char c : s)
        {
            if (c == '%' || c <= 0x20 || c == 0x7F) { r += '%'; r += kHex[c >> 4]; r += kHex[c & 0xF]; }
            else r += static_cast<char>(c);
        }
        return r;
    }

    std::string EncodePath(const std::string& s)
    {
        std::string r;
        for (const char c : s)
        {
            if (c == '%') r += "%25";
            else if (c == ' ') r += "%20";
            else r += c;
        }
        return r;
    }

    std::string Decode(const std::string& s) { return Resource::AssetRefs::PercentDecode(s); }

    // A serializer line ("  Tag: k=v k=v\n") as its pairs, in the line's order.
    std::vector<std::pair<std::string, std::string>> LinePairs(const std::string& line)
    {
        std::vector<std::pair<std::string, std::string>> out;
        const auto colon = line.find(':');
        if (colon == std::string::npos) return out;
        std::istringstream tok(line.substr(colon + 1));
        std::string t;
        while (tok >> t)
        {
            const auto eq = t.find('=');
            if (eq != std::string::npos) out.emplace_back(t.substr(0, eq), t.substr(eq + 1));
        }
        return out;
    }

    // What a deserializer makes of NO keys, written back by its serializer: the default
    // of every key, in the serializer's own text. Taken from the serializer pair itself
    // (a scratch entity), so it can never disagree with the scene loader.
    const std::unordered_map<std::string, std::string>& DefaultsOf(const std::string& tag)
    {
        static std::mutex s_mutex;
        static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_cache;
        std::lock_guard<std::mutex> lk(s_mutex);
        auto it = s_cache.find(tag);
        if (it != s_cache.end()) return it->second;
        std::unordered_map<std::string, std::string> defs;
        if (const ComponentSerializer* s = GetComponentRegistry().FindByTag(tag); s && s->deserialize && s->serialize)
        {
            World scratch;
            const Entity e = scratch.CreateEntity();
            s->deserialize(scratch, e, KVMap{}, nullptr);
            std::ostringstream ss;
            s->serialize(scratch, e, ss);
            for (auto& [k, v] : LinePairs(ss.str())) defs[k] = v;
        }
        return s_cache.emplace(tag, std::move(defs)).first->second;
    }

    std::string KvGet(const std::vector<std::pair<std::string, std::string>>& kv, const char* key)
    {
        for (const auto& [k, v] : kv)
            if (k == key) return v;
        return {};
    }

    bool KvHas(const std::vector<std::pair<std::string, std::string>>& kv, const char* key)
    {
        for (const auto& p : kv)
            if (p.first == key) return true;
        return false;
    }

    // The value of key @p key in a block: its own, else the default.
    std::string ValueOr(const UIAssetBlock& b, const std::unordered_map<std::string, std::string>& defs, const char* key)
    {
        if (const std::string* v = b.Get(key)) return *v;
        const auto it = defs.find(key);
        return it != defs.end() ? it->second : std::string();
    }

    // ==== JSON text ====
    std::string Quote(const std::string& s)
    {
        std::string r = "\"";
        for (const unsigned char c : s)
        {
            switch (c)
            {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char u[8];
                    std::snprintf(u, sizeof(u), "\\u%04x", c);
                    r += u;
                }
                else r += static_cast<char>(c);
            }
        }
        return r + "\"";
    }

    // The field's text in the file, or "" when it is at its default (left out).
    std::string FieldJson(const FieldDef& f, const UIAssetBlock& b, const std::unordered_map<std::string, std::string>& defs)
    {
        const std::string v  = ValueOr(b, defs, f.key);
        const auto        di = defs.find(f.key);
        const std::string d  = di != defs.end() ? di->second : std::string();
        switch (f.kind)
        {
        case Kind::Number:
            if (ParseFloat(v) == ParseFloat(d)) return {};
            return FloatText(ParseFloat(v));
        case Kind::Integer:
        case Kind::Boolean:
            if (ParseInt(v) == ParseInt(d)) return {};
            return f.kind == Kind::Boolean ? (ParseInt(v) ? "true" : "false") : std::to_string(ParseInt(v));
        case Kind::Enum:
        {
            const long long n = ParseInt(v);
            if (n == ParseInt(d)) return {};
            if (n >= 0 && n < static_cast<long long>(f.names.size())) return Quote(f.names[static_cast<std::size_t>(n)]);
            return std::to_string(n);   // an out-of-range value survives (Parse refuses it)
        }
        case Kind::Text:
        case Kind::Path:
            if (Decode(v) == Decode(d)) return {};
            return Quote(Decode(v));
        case Kind::Vec:
        {
            const std::vector<float> a = ParseVec(v, f.arity), z = ParseVec(d, f.arity);
            if (a == z) return {};
            std::string s = "[";
            for (std::size_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + FloatText(a[i]);
            return s + "]";
        }
        case Kind::Pair:
        {
            const std::string v2 = ValueOr(b, defs, f.key2);
            const auto        d2 = defs.find(f.key2);
            const float x = ParseFloat(v), y = ParseFloat(v2);
            if (x == ParseFloat(d) && y == ParseFloat(d2 != defs.end() ? d2->second : std::string())) return {};
            return "[" + FloatText(x) + ", " + FloatText(y) + "]";
        }
        }
        return {};
    }

    void WriteNode(const UIAssetNode& n, int indent, std::string& out)
    {
        const std::string pad(static_cast<std::size_t>(indent), ' '), in(static_cast<std::size_t>(indent) + 2, ' ');
        std::vector<std::string> lines;
        lines.push_back(in + "\"id\": " + Quote(n.id));
        lines.push_back(in + "\"name\": " + Quote(n.name));
        if (n.hasOrder) lines.push_back(in + "\"order\": " + std::to_string(n.order));
        for (const BlockDef& bd : Blocks())
        {
            const UIAssetBlock* b = n.Block(bd.tag);
            if (!b) continue;
            const auto& defs = DefaultsOf(bd.tag);
            std::string fields;
            for (const FieldDef& f : bd.fields)
            {
                const std::string v = FieldJson(f, *b, defs);
                if (v.empty()) continue;
                fields += (fields.empty() ? " " : ", ") + Quote(f.json) + ": " + v;
            }
            lines.push_back(in + Quote(bd.json) + ": {" + fields + (fields.empty() ? "}" : " }"));
        }
        if (!n.children.empty())
        {
            std::string c = in + "\"children\": [\n";
            for (std::size_t i = 0; i < n.children.size(); ++i)
            {
                WriteNode(n.children[i], indent + 4, c);
                c += (i + 1 < n.children.size()) ? ",\n" : "\n";
            }
            c += in + "]";
            lines.push_back(c);
        }
        out += pad + "{\n";
        for (std::size_t i = 0; i < lines.size(); ++i) out += lines[i] + (i + 1 < lines.size() ? ",\n" : "\n");
        out += pad + "}";
    }

    // ==== Parsing ====
    bool ValidId(const std::string& id)
    {
        if (id.empty() || id.size() > 64) return false;
        for (const unsigned char c : id)
            if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
        return true;
    }

    struct Parser
    {
        std::vector<UIAssetIssue>&      issues;
        std::unordered_set<std::string> ids;
        // A file is untrusted input: Node recurses once per nesting level (and so
        // do the writer, the spawn and every walk after it) on a 1 MB stack, and a
        // spawn walks every node, so a broken or hostile file is refused, not obeyed.
        std::size_t nodes   = 0;
        bool        tooMany = false;

        void Issue(const std::string& where, const std::string& msg) { issues.push_back({ where, msg }); }

        // A JSON number that a float holds.
        bool Float(const Json& j, const std::string& where, float& out)
        {
            if (!j.is_number()) { Issue(where, "a number"); return false; }
            const double d = j.get<double>();
            if (!std::isfinite(d) || std::fabs(d) > static_cast<double>((std::numeric_limits<float>::max)()))
            {
                Issue(where, "a finite float");
                return false;
            }
            out = static_cast<float>(d);
            return true;
        }

        void Field(const FieldDef& f, const Json& j, const std::string& where, UIAssetBlock& b)
        {
            auto put = [&](const char* key, std::string v) { b.kv.emplace_back(key, std::move(v)); };
            switch (f.kind)
            {
            case Kind::Number:
            {
                float v;
                if (Float(j, where, v)) put(f.key, FloatText(v));
                break;
            }
            case Kind::Integer:
                if (!j.is_number_integer()) { Issue(where, "an integer"); break; }
                put(f.key, std::to_string(j.get<long long>()));
                break;
            case Kind::Boolean:
                if (!j.is_boolean()) { Issue(where, "true or false"); break; }
                put(f.key, j.get<bool>() ? "1" : "0");
                break;
            case Kind::Enum:
            {
                std::string names;
                for (std::size_t i = 0; i < f.names.size(); ++i)
                {
                    if (j.is_string() && j.get<std::string>() == f.names[i]) { put(f.key, std::to_string(i)); return; }
                    names += (i ? ", " : "") + std::string(f.names[i]);
                }
                Issue(where, "one of " + names);
                break;
            }
            case Kind::Text:
            case Kind::Path:
                if (!j.is_string()) { Issue(where, "a string"); break; }
                put(f.key, f.kind == Kind::Text ? EncodeText(j.get<std::string>()) : EncodePath(j.get<std::string>()));
                break;
            case Kind::Vec:
            case Kind::Pair:
            {
                const int n = f.kind == Kind::Pair ? 2 : f.arity;
                if (!j.is_array() || static_cast<int>(j.size()) != n)
                {
                    Issue(where, "an array of " + std::to_string(n) + " numbers");
                    break;
                }
                std::vector<float> v(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i)
                    if (!Float(j[static_cast<std::size_t>(i)], where + "[" + std::to_string(i) + "]", v[static_cast<std::size_t>(i)]))
                        return;
                if (f.kind == Kind::Pair)
                {
                    put(f.key, FloatText(v[0]));
                    put(f.key2, FloatText(v[1]));
                }
                else
                    put(f.key, VecText(v));
                break;
            }
            }
        }

        void Node(const Json& j, const std::string& where, bool isRoot, UIAssetNode& n, int depth = 0)
        {
            if (!j.is_object()) { Issue(where, "a node is an object"); return; }
            if (++nodes > kUIAssetMaxNodes)
            {
                if (!tooMany) Issue(where, "more than " + std::to_string(kUIAssetMaxNodes) + " nodes in one asset");
                tooMany = true;
                return;
            }
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                const std::string& key = it.key();
                const std::string  at  = where + "." + key;
                if (key == "id")
                {
                    if (!it->is_string() || !ValidId(it->get<std::string>()))
                        Issue(at, "an id: 1..64 of [A-Za-z0-9_.-]");
                    else
                        n.id = it->get<std::string>();
                }
                else if (key == "name")
                {
                    if (!it->is_string()) Issue(at, "a string");
                    else n.name = it->get<std::string>();
                }
                else if (key == "order")
                {
                    if (!it->is_number_integer()) Issue(at, "an integer (the siblingIndex)");
                    else
                    {
                        n.hasOrder = true;
                        n.order    = static_cast<int>(it->get<long long>());
                    }
                }
                else if (key == "children")
                {
                    if (!it->is_array()) { Issue(at, "an array of nodes"); continue; }
                    if (depth + 1 > kUIAssetMaxDepth)
                    {
                        Issue(at, "nested deeper than " + std::to_string(kUIAssetMaxDepth) + " levels");
                        continue;
                    }
                    for (std::size_t i = 0; i < it->size() && !tooMany; ++i)
                    {
                        n.children.emplace_back();
                        Node((*it)[i], at + "[" + std::to_string(i) + "]", false, n.children.back(), depth + 1);
                    }
                }
                else if (const BlockDef* bd = BlockByJson(key))
                {
                    if (!it->is_object()) { Issue(at, "a block is an object"); continue; }
                    if (std::string_view(bd->tag) == "CvCanvas" && !isRoot)
                    {
                        Issue(at, "a canvas is a root: only the root node can have one");
                        continue;
                    }
                    UIAssetBlock b;
                    b.tag = bd->tag;
                    for (auto f = it->begin(); f != it->end(); ++f)
                    {
                        const FieldDef* fd = nullptr;
                        for (const FieldDef& c : bd->fields)
                            if (f.key() == c.json) fd = &c;
                        if (!fd)
                        {
                            std::string known;
                            for (const FieldDef& c : bd->fields) known += (known.empty() ? "" : ", ") + std::string(c.json);
                            Issue(at + "." + f.key(), "no such field in " + key + " (" + known + ")");
                            continue;
                        }
                        Field(*fd, *f, at + "." + f.key(), b);
                    }
                    // The Cv* authoring contract judges every value (ranges, integers).
                    std::string err;
                    if (!Resource::Authoring::Validate(b.tag, b.kv, err)) Issue(at, err);
                    n.blocks.push_back(std::move(b));
                }
                else
                    Issue(at, "no such key in a node (id, name, order, canvas, rect, clip, image, text, interact, sprite, "
                              "children)");
            }
            // Canonical block order, whatever order the file had.
            std::stable_sort(n.blocks.begin(), n.blocks.end(), [](const UIAssetBlock& a, const UIAssetBlock& b) {
                auto rank = [](const std::string& t) {
                    int i = 0;
                    for (const BlockDef& bd : Blocks()) { if (t == bd.tag) return i; ++i; }
                    return i;
                };
                return rank(a.tag) < rank(b.tag);
            });
            if (n.id.empty()) Issue(where, "a node needs an id");
            else if (!ids.insert(n.id).second) Issue(where + ".id", "'" + n.id + "' is the id of another node too");
            if (n.name.empty()) n.name = n.id;
            const bool canvas = n.Block("CvCanvas") != nullptr, rect = n.Block("CvRect") != nullptr;
            if (canvas && rect) Issue(where, "a canvas root has no rect (the screen is its rect)");
            if (!canvas && !rect) Issue(where, isRoot ? "the root needs a canvas or a rect" : "a node needs a rect");
        }
    };

    // ==== Identity ====
    std::uint64_t Fnv(std::string_view s, std::uint64_t h)
    {
        for (const unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
        return h;
    }

    ECS::Guid BaseGuid(const std::string& assetKey, const std::string& id)
    {
        const std::string key = assetKey + '\n' + id;
        ECS::Guid g;
        g.hi = Fnv(key, 0xcbf29ce484222325ull);
        g.lo = Fnv(key, 0x84222325cbf29ce4ull);
        if (!g.IsValid()) g.lo = 1;
        return g;
    }
} // namespace

const std::string* UIAssetBlock::Get(std::string_view key) const
{
    for (const auto& [k, v] : kv)
        if (k == key) return &v;
    return nullptr;
}

const UIAssetBlock* UIAssetNode::Block(std::string_view tag) const
{
    for (const UIAssetBlock& b : blocks)
        if (b.tag == tag) return &b;
    return nullptr;
}

UIAssetBlock* UIAssetNode::Block(std::string_view tag)
{
    for (UIAssetBlock& b : blocks)
        if (b.tag == tag) return &b;
    return nullptr;
}

std::size_t UIAsset::NodeCount() const
{
    std::size_t n = 0;
    std::function<void(const UIAssetNode&)> walk = [&](const UIAssetNode& x) {
        ++n;
        for (const UIAssetNode& c : x.children) walk(c);
    };
    walk(root);
    return n;
}

std::string UIAssetIdFromName(std::string_view name)
{
    std::string id;
    bool under = false;
    for (const unsigned char c : name)
    {
        if (std::isalnum(c)) { id += static_cast<char>(std::tolower(c)); under = false; }
        else if (!id.empty() && !under) { id += '_'; under = true; }
    }
    while (!id.empty() && id.back() == '_') id.pop_back();
    if (id.size() > 48) id.resize(48);
    return id.empty() ? std::string("node") : id;
}

bool ParseUIAsset(std::string_view text, UIAsset& out, std::vector<UIAssetIssue>& issues)
{
    out = UIAsset{};
    const std::size_t before = issues.size();
    Json j;
    try
    {
        j = Json::parse(text.begin(), text.end());
    }
    catch (const std::exception& e)
    {
        issues.push_back({ "", std::string("not JSON: ") + e.what() });
        return false;
    }
    if (!j.is_object()) { issues.push_back({ "", "a .iui is a JSON object" }); return false; }
    Parser p{ issues, {} };
    bool hasRoot = false;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (it.key() == "format")
        {
            if (!it->is_string() || it->get<std::string>() != "iui") p.Issue("format", "\"iui\"");
        }
        else if (it.key() == "version")
        {
            if (!it->is_number_integer() || it->get<long long>() != 1) p.Issue("version", "1 (the only version)");
        }
        else if (it.key() == "root")
        {
            hasRoot = true;
            p.Node(*it, "root", true, out.root);
        }
        else
            p.Issue(it.key(), "no such key (format, version, root)");
    }
    if (!j.contains("format")) p.Issue("format", "missing: \"format\": \"iui\"");
    if (!hasRoot) p.Issue("root", "missing: the root node");
    return issues.size() == before;
}

std::string WriteUIAsset(const UIAsset& asset)
{
    std::string out = "{\n  \"format\": \"iui\",\n  \"version\": " + std::to_string(asset.version) + ",\n  \"root\": ";
    std::string node;
    WriteNode(asset.root, 2, node);
    // The root's "{" goes on the key's line.
    out += node.substr(node.find('{')) + "\n}\n";
    return out;
}

std::vector<UIAssetRef> UIAssetRefs(const UIAsset& asset)
{
    std::vector<UIAssetRef> refs;
    std::function<void(const UIAssetNode&)> walk = [&](const UIAssetNode& n) {
        for (const UIAssetBlock& b : n.blocks)
        {
            auto add = [&](const char* key, const char* field) {
                if (const std::string* v = b.Get(key); v && !v->empty()) refs.push_back({ n.id, field, Decode(*v) });
            };
            if (b.tag == "CvImage") add("tex", "image.texture");
            if (b.tag == "CvText")
            {
                add("font", "text.font");
                add("customShader", "text.customShader");
            }
        }
        for (const UIAssetNode& c : n.children) walk(c);
    };
    walk(asset.root);
    return refs;
}

bool CaptureUIAsset(World& world, Entity root, UIAsset& out, std::string* why,
                    const std::vector<std::pair<Entity, std::string>>* ids,
                    std::vector<std::pair<Entity, std::string>>* idsOut)
{
    out = UIAsset{};
    if (idsOut) idsOut->clear();
    if (!IsUINode(world, root))
    {
        if (why) *why = "not a canvas or a UI node";
        return false;
    }
    ComponentSerializerRegistry& reg = GetComponentRegistry();
    std::unordered_set<std::string> used;
    std::unordered_set<Entity> seen;
    auto idOf = [&](Entity e) {
        std::string id;
        if (ids)
            for (const auto& [ent, s] : *ids)
                if (ent == e) id = s;
        if (id.empty())
            if (const UIAssetNodeId* nid = world.GetComponent<UIAssetNodeId>(e)) id = nid->id;
        if (id.empty() || !ValidId(id)) id = UIAssetIdFromName(world.GetName(e));
        std::string unique = id;
        for (int k = 2; !used.insert(unique).second; ++k) unique = id + "_" + std::to_string(k);
        return unique;
    };
    bool ok = true;
    std::function<void(Entity, UIAssetNode&, int, bool, int)> capture = [&](Entity e, UIAssetNode& n, int index,
                                                                           bool isRoot, int depth) {
        if (!seen.insert(e).second) { ok = false; if (why) *why = "a UI parent cycle"; return; }
        // The asset's own limits (a capture is parsed back before anything writes it).
        if (depth > kUIAssetMaxDepth || seen.size() > kUIAssetMaxNodes)
        {
            ok = false;
            if (why)
                *why = "nested deeper than " + std::to_string(kUIAssetMaxDepth) + " levels or more than " +
                       std::to_string(kUIAssetMaxNodes) + " nodes: past what one .iui holds";
            return;
        }
        n.id   = idOf(e);
        n.name = world.GetName(e);
        if (idsOut) idsOut->emplace_back(e, n.id);
        for (const BlockDef& bd : Blocks())
        {
            const ComponentSerializer* s = reg.FindByTag(bd.tag);
            if (!s || !s->has || !s->serialize || !s->has(world, e)) continue;
            std::ostringstream ss;
            s->serialize(world, e, ss);
            UIAssetBlock b;
            b.tag = bd.tag;
            std::string line = ss.str();
            if (const auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
            for (auto& [k, v] : LinePairs(line))
            {
                if (b.tag == "CvRect" && k == "sibling")
                {
                    const int sib = static_cast<int>(ParseInt(v));
                    if (!isRoot && sib != index) { n.hasOrder = true; n.order = sib; }
                    continue;
                }
                b.kv.emplace_back(k, v);
            }
            n.blocks.push_back(std::move(b));
        }
        // A sprite animation writes its image's uv0 / uv1 every frame from its own frame
        // counter (AdvanceSpriteAnimations, playing or not): they are its output, never
        // authored - left out, or two captures a frame apart would differ.
        if (n.Block("CvSpriteAnim"))
            if (UIAssetBlock* img = n.Block("CvImage"))
                img->kv.erase(std::remove_if(img->kv.begin(), img->kv.end(),
                                             [](const auto& p) { return p.first == "uv0" || p.first == "uv1"; }),
                              img->kv.end());
        std::vector<Entity> kids;
        UIChildrenOf(world, e, kids);
        for (std::size_t i = 0; i < kids.size() && ok; ++i)
        {
            n.children.emplace_back();
            capture(kids[i], n.children.back(), static_cast<int>(i), false, depth + 1);
        }
    };
    capture(root, out.root, 0, true, 0);
    return ok;
}

Entity SpawnUIAsset(World& world, const UIAsset& asset, const UISpawnOptions& opt, std::string* why,
                    std::vector<std::pair<std::string, Entity>>* idsOut)
{
    auto fail = [why](const std::string& s) { if (why) *why = s; return NullEntity; };
    const bool canvasRoot = asset.root.Block("CvCanvas") != nullptr;
    if (canvasRoot && opt.parent != NullEntity) return fail("a canvas asset is a root: it takes no parent");
    if (opt.parent != NullEntity && !IsUINode(world, opt.parent))
        return fail("the parent is not a canvas or a UI node");
    const ECS::Guid seed = opt.seed.IsValid() ? opt.seed : ECS::Guid::Generate();
    ComponentSerializerRegistry& reg = GetComponentRegistry();

    std::function<Entity(const UIAssetNode&, Entity, int)> spawn = [&](const UIAssetNode& n, Entity parent, int index) {
        const Entity e = world.CreateEntity();
        world.SetName(e, n.name);
        // Identity first: the attach below then names this GUID instead of minting one.
        GuidComponent gc;
        gc.guid = ECS::Guid::Derive(BaseGuid(opt.assetKey, n.id), seed);
        world.AddComponent<GuidComponent>(e, gc);
        ECS::GuidRegistry::Get().Register(gc.guid, e);
        world.AddComponent<UIAssetNodeId>(e, UIAssetNodeId{ n.id });
        for (const UIAssetBlock& b : n.blocks)
        {
            KVMap kv(b.kv.begin(), b.kv.end());
            if (b.tag == "CvRect") kv["sibling"] = std::to_string(n.hasOrder ? n.order : index);
            ComponentWrite::RewriteFromKV(reg, b.tag, world, e, kv, opt.assets);
        }
        if (parent != NullEntity) AttachUIParent(world, e, parent);
        if (idsOut) idsOut->emplace_back(n.id, e);
        for (std::size_t i = 0; i < n.children.size(); ++i) spawn(n.children[i], e, static_cast<int>(i));
        return e;
    };
    const Entity root = spawn(asset.root, NullEntity, 0);
    if (!canvasRoot && opt.parent != NullEntity)
    {
        // Last among the parent's children.
        std::vector<Entity> sibs;
        UIChildrenOf(world, opt.parent, sibs);
        int next = 0;
        for (const Entity s : sibs)
            if (const UIRect* r = world.GetComponent<UIRect>(s)) next = (std::max)(next, r->siblingIndex + 1);
        AttachUIParent(world, root, opt.parent);
        if (UIRect* r = world.GetComponent<UIRect>(root)) r->siblingIndex = next;
    }
    return root;
}

Entity FindUINode(const World& world, Entity root, std::string_view id)
{
    if (root == NullEntity || !world.IsAlive(root)) return NullEntity;
    if (const UIAssetNodeId* n = world.GetComponent<UIAssetNodeId>(root); n && n->id == id) return root;
    // parent -> children in ONE pass over the UIParent pool (UIChildrenOf per node
    // would scan the pool once per node of the instance).
    std::unordered_map<Entity, std::vector<Entity>> kids;
    if (const auto* pool = world.GetPool<UIParent>())
    {
        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        for (std::size_t i = 0; i < data.size(); ++i)
            if (world.IsAlive(ents[i]))
                if (const Entity p = ResolveUIParent(world, data[i]); p != NullEntity && p != ents[i])
                    kids[p].push_back(ents[i]);
    }
    std::vector<Entity> stack{ root };
    std::unordered_set<Entity> seen;
    while (!stack.empty())
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (!seen.insert(e).second) continue;
        if (const UIAssetNodeId* n = world.GetComponent<UIAssetNodeId>(e); n && n->id == id) return e;
        if (const auto it = kids.find(e); it != kids.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
    }
    return NullEntity;
}

namespace
{
    struct CacheEntry
    {
        std::int64_t  stamp = 0;
        std::uint64_t size  = 0;
        std::shared_ptr<const UIAsset> asset;
    };
    std::mutex                                  g_cacheMutex;
    std::unordered_map<std::string, CacheEntry> g_cache;
    std::atomic<std::uint64_t>                  g_generation{ 1 };   // UIAssetGeneration

    std::string CanonPath(const std::string& path)
    {
        std::string p = Sys::RelativizeAssetPath(path);
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
    }

    // The file's (stamp, size): the disk's write time and size; a pak entry never changes.
    bool Stat(const std::string& canon, std::int64_t& stamp, std::uint64_t& size)
    {
        Resource::AssetFS& fs = Resource::AssetFS::Get();
        if (fs.HasInPak(canon)) { stamp = -1; size = 0; return true; }
        std::string onDisk = fs.ResolveOnDisk(canon);
        if (onDisk.empty()) onDisk = canon;
        std::error_code ec;
        const auto p = Sys::U8Path(onDisk);
        size = std::filesystem::file_size(p, ec);
        if (ec) return false;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return false;
        stamp = static_cast<std::int64_t>(t.time_since_epoch().count());
        return true;
    }
}

std::shared_ptr<const UIAsset> LoadUIAsset(const std::string& path, std::string* why)
{
    const std::string canon = CanonPath(path);
    std::int64_t  stamp = 0;
    std::uint64_t size  = 0;
    if (canon.empty() || !Stat(canon, stamp, size))
    {
        if (why) *why = "no such file: " + path;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        const auto it = g_cache.find(canon);
        if (it != g_cache.end() && it->second.stamp == stamp && it->second.size == size) return it->second.asset;
    }
    std::vector<std::uint8_t> bytes;
    if (!Resource::AssetFS::Get().ReadFile(canon, bytes))
    {
        if (why) *why = "cannot read " + canon;
        return nullptr;
    }
    auto asset = std::make_shared<UIAsset>();
    std::vector<UIAssetIssue> issues;
    if (!ParseUIAsset(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), *asset, issues))
    {
        if (why)
        {
            *why = canon + ":";
            for (std::size_t i = 0; i < issues.size() && i < 8; ++i)
                *why += " [" + issues[i].where + "] " + issues[i].message + ";";
            if (issues.size() > 8) *why += " ... " + std::to_string(issues.size() - 8) + " more";
        }
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_cache[canon] = CacheEntry{ stamp, size, asset };
    g_generation.fetch_add(1, std::memory_order_relaxed);
    return asset;
}

void InvalidateUIAsset(const std::string& path)
{
    const std::string canon = CanonPath(path);
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_cache.erase(canon);
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t UIAssetGeneration() { return g_generation.load(std::memory_order_relaxed); }

} // namespace UI
