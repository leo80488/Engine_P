#include "MaterialGraph/MaterialGraph.h"

#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace MaterialGraph
{
// ==== Value ====

Value Value::Number(double v)                { Value r; r.kind = Kind::Number;  r.num = v;             return r; }
Value Value::Bool(bool v)                    { Value r; r.kind = Kind::Bool;    r.b = v;               return r; }
Value Value::String(std::string v)           { Value r; r.kind = Kind::String;  r.str = std::move(v);  return r; }
Value Value::Numbers(std::vector<double> v)  { Value r; r.kind = Kind::Numbers; r.nums = std::move(v); return r; }

int Value::Components() const
{
    switch (kind)
    {
        case Kind::Number:
        case Kind::Bool:    return 1;
        case Kind::Numbers: return static_cast<int>(nums.size());
        default:            return 0;
    }
}

double Value::At(int i) const
{
    switch (kind)
    {
        case Kind::Number:  return i == 0 ? num : 0.0;
        case Kind::Bool:    return i == 0 ? (b ? 1.0 : 0.0) : 0.0;
        case Kind::Numbers: return (i >= 0 && i < static_cast<int>(nums.size())) ? nums[i] : 0.0;
        default:            return 0.0;
    }
}

bool Value::operator==(const Value& o) const
{
    if (kind != o.kind) return false;
    switch (kind)
    {
        case Kind::Number:  return num == o.num;
        case Kind::Bool:    return b == o.b;
        case Kind::String:  return str == o.str;
        case Kind::Numbers: return nums == o.nums;
        default:            return true;
    }
}

// ==== Node / Graph ====

const Value* Node::Prop(std::string_view key) const
{
    const auto it = props.find(std::string(key));
    return it != props.end() ? &it->second : nullptr;
}

double Node::PropNumber(std::string_view key, double fallback) const
{
    const Value* v = Prop(key);
    return (v && v->IsNumeric() && v->Components() >= 1) ? v->At(0) : fallback;
}

std::string Node::PropString(std::string_view key, const std::string& fallback) const
{
    const Value* v = Prop(key);
    return (v && v->kind == Value::Kind::String) ? v->str : fallback;
}

bool Node::PropBool(std::string_view key, bool fallback) const
{
    const Value* v = Prop(key);
    if (!v) return fallback;
    if (v->kind == Value::Kind::Bool)   return v->b;
    if (v->kind == Value::Kind::Number) return v->num != 0.0;
    return fallback;
}

Node* Graph::FindNode(std::uint32_t id)
{
    for (Node& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

const Node* Graph::FindNode(std::uint32_t id) const
{
    for (const Node& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

const Link* Graph::FindInputLink(std::uint32_t toNode, std::string_view toPin) const
{
    for (const Link& l : links)
        if (l.toNode == toNode && l.toPin == toPin) return &l;
    return nullptr;
}

// ==== Number / string spelling ====

std::string FormatNumber(double v)
{
    if (!std::isfinite(v)) return "0";   // JSON cannot carry one; the parser never makes one
    const float f = static_cast<float>(v);
    if (f == 0.0f) return "0";           // -0 too
    if (std::fabs(f) < 1.0e7f && std::floor(f) == f)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.0f", static_cast<double>(f));
        return b;
    }
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), f);   // shortest round-trip
    return std::string(buf, r.ptr);
}

namespace
{
    using Json = nlohmann::json;

    void AppendJsonString(std::string& out, const std::string& s)
    {
        out += '"';
        for (const char ch : s)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20)
                    {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", c);
                        out += b;
                    }
                    else out += ch;   // UTF-8 passes through: valid JSON
            }
        }
        out += '"';
    }

    void AppendNumbers(std::string& out, const double* v, std::size_t n)
    {
        out += '[';
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i) out += ", ";
            out += FormatNumber(v[i]);
        }
        out += ']';
    }

    void AppendNumbers(std::string& out, const float* v, std::size_t n)
    {
        double d[4] = {};
        for (std::size_t i = 0; i < n && i < 4; ++i) d[i] = v[i];
        AppendNumbers(out, d, n);
    }

    void AppendValue(std::string& out, const Value& v)
    {
        switch (v.kind)
        {
            case Value::Kind::Number:  out += FormatNumber(v.num); break;
            case Value::Kind::Bool:    out += v.b ? "true" : "false"; break;
            case Value::Kind::String:  AppendJsonString(out, v.str); break;
            case Value::Kind::Numbers: AppendNumbers(out, v.nums.data(), v.nums.size()); break;
            default:                   out += "null"; break;
        }
    }

    void AppendValueMap(std::string& out, const ValueMap& m)
    {
        out += '{';
        bool first = true;
        for (const auto& [k, v] : m)   // std::map: sorted keys
        {
            if (!first) out += ", ";
            first = false;
            AppendJsonString(out, k);
            out += ": ";
            AppendValue(out, v);
        }
        out += '}';
    }

    bool ParseValue(const Json& j, Value& out)
    {
        if (j.is_boolean()) { out = Value::Bool(j.get<bool>()); return true; }
        if (j.is_number())
        {
            const double d = j.get<double>();
            if (!std::isfinite(d)) return false;
            out = Value::Number(d);
            return true;
        }
        if (j.is_string()) { out = Value::String(j.get<std::string>()); return true; }
        if (j.is_array())
        {
            std::vector<double> nums;
            nums.reserve(j.size());
            for (const Json& e : j)
            {
                if (!e.is_number()) return false;
                const double d = e.get<double>();
                if (!std::isfinite(d)) return false;
                nums.push_back(d);
            }
            out = Value::Numbers(std::move(nums));
            return true;
        }
        return false;
    }

    bool ParseValueMap(const Json& j, ValueMap& out, const std::string& where, std::string& error)
    {
        if (!j.is_object()) { error = where + " is not an object"; return false; }
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            Value v;
            if (!ParseValue(it.value(), v))
            {
                error = where + "." + it.key() + ": want a number, bool, string or array of numbers";
                return false;
            }
            out[it.key()] = std::move(v);
        }
        return true;
    }

    bool ParseId(const Json& j, std::uint32_t& out)
    {
        if (!j.is_number_integer()) return false;
        const long long v = j.get<long long>();
        if (v < 1 || v > static_cast<long long>(std::numeric_limits<std::uint32_t>::max())) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    bool ParseFloats(const Json& j, float* dst, std::size_t n)
    {
        if (!j.is_array() || j.size() != n) return false;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!j[i].is_number()) return false;
            const double d = j[i].get<double>();
            if (!std::isfinite(d)) return false;
            dst[i] = static_cast<float>(d);
        }
        return true;
    }
} // namespace

bool ParseGraph(std::string_view text, Graph& out, std::string& error)
{
    out = Graph{};
    const Json j = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions*/ false);
    if (j.is_discarded() || !j.is_object()) { error = "not a JSON object"; return false; }

    if (!j.contains("format") || !j["format"].is_string() ||
        (j["format"].get<std::string>() != kFormatTag && j["format"].get<std::string>() != kFunctionFormatTag))
    {
        error = "\"format\" is not \"imatgraph\" (or \"imatfunc\")";
        return false;
    }
    out.function = j["format"].get<std::string>() == kFunctionFormatTag;
    if (j.contains("version"))
    {
        if (!j["version"].is_number_integer()) { error = "\"version\" is not an integer"; return false; }
        out.version = j["version"].get<int>();
        if (out.version < 1 || out.version > kFormatVersion)
        {
            error = "version " + std::to_string(out.version) + " is not readable by this build (max " +
                    std::to_string(kFormatVersion) + ")";
            return false;
        }
    }
    if (j.contains("compiler"))
    {
        if (!j["compiler"].is_number_integer()) { error = "\"compiler\" is not an integer"; return false; }
        out.compiler = j["compiler"].get<int>();
    }
    if (j.contains("ownerGuid"))
    {
        if (!j["ownerGuid"].is_string()) { error = "\"ownerGuid\" is not a string"; return false; }
        out.ownerGuid = j["ownerGuid"].get<std::string>();
    }
    std::uint32_t maxId = 0;
    if (j.contains("nextId") && !ParseId(j["nextId"], out.nextId))
    {
        error = "\"nextId\" is not a positive integer";
        return false;
    }

    std::vector<std::uint32_t> ids;
    if (j.contains("nodes"))
    {
        const Json& nodes = j["nodes"];
        if (!nodes.is_array()) { error = "\"nodes\" is not an array"; return false; }
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const Json& jn = nodes[i];
            const std::string where = "nodes[" + std::to_string(i) + "]";
            if (!jn.is_object()) { error = where + " is not an object"; return false; }
            Node n;
            if (!jn.contains("id") || !ParseId(jn["id"], n.id)) { error = where + ".id is not a positive integer"; return false; }
            if (!jn.contains("type") || !jn["type"].is_string() || jn["type"].get<std::string>().empty())
            {
                error = where + ".type is missing";
                return false;
            }
            n.type = jn["type"].get<std::string>();
            if (jn.contains("pos") && !ParseFloats(jn["pos"], n.pos, 2)) { error = where + ".pos is not [x, y]"; return false; }
            if (jn.contains("props") && !ParseValueMap(jn["props"], n.props, where + ".props", error)) return false;
            if (jn.contains("inputs") && !ParseValueMap(jn["inputs"], n.inputs, where + ".inputs", error)) return false;
            ids.push_back(n.id);
            maxId = std::max(maxId, n.id);
            out.nodes.push_back(std::move(n));
        }
    }

    if (j.contains("links"))
    {
        const Json& links = j["links"];
        if (!links.is_array()) { error = "\"links\" is not an array"; return false; }
        for (std::size_t i = 0; i < links.size(); ++i)
        {
            const Json& jl = links[i];
            Link l;
            if (!jl.is_array() || jl.size() != 4 || !ParseId(jl[0], l.fromNode) || !jl[1].is_string() ||
                !ParseId(jl[2], l.toNode) || !jl[3].is_string())
            {
                error = "links[" + std::to_string(i) + "] is not [fromNode, \"pin\", toNode, \"pin\"]";
                return false;
            }
            l.fromPin = jl[1].get<std::string>();
            l.toPin   = jl[3].get<std::string>();
            out.links.push_back(std::move(l));
        }
    }

    if (j.contains("comments"))
    {
        const Json& comments = j["comments"];
        if (!comments.is_array()) { error = "\"comments\" is not an array"; return false; }
        for (std::size_t i = 0; i < comments.size(); ++i)
        {
            const Json& jc = comments[i];
            const std::string where = "comments[" + std::to_string(i) + "]";
            Comment c;
            if (!jc.is_object() || !jc.contains("id") || !ParseId(jc["id"], c.id))
            {
                error = where + ".id is not a positive integer";
                return false;
            }
            if (jc.contains("rect") && !ParseFloats(jc["rect"], c.rect, 4)) { error = where + ".rect is not [x0, y0, x1, y1]"; return false; }
            if (jc.contains("color") && !ParseFloats(jc["color"], c.color, 4)) { error = where + ".color is not [r, g, b, a]"; return false; }
            if (jc.contains("text"))
            {
                if (!jc["text"].is_string()) { error = where + ".text is not a string"; return false; }
                c.text = jc["text"].get<std::string>();
            }
            ids.push_back(c.id);
            maxId = std::max(maxId, c.id);
            out.comments.push_back(std::move(c));
        }
    }

    std::sort(ids.begin(), ids.end());
    const auto dup = std::adjacent_find(ids.begin(), ids.end());
    if (dup != ids.end())
    {
        error = "id " + std::to_string(*dup) + " is used twice";
        return false;
    }
    if (out.nextId <= maxId) out.nextId = maxId + 1;
    return true;
}

std::string SerializeGraph(const Graph& g)
{
    std::vector<const Node*> nodes;
    nodes.reserve(g.nodes.size());
    for (const Node& n : g.nodes) nodes.push_back(&n);
    std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) { return a->id < b->id; });

    std::vector<const Link*> links;
    links.reserve(g.links.size());
    for (const Link& l : g.links) links.push_back(&l);
    std::sort(links.begin(), links.end(), [](const Link* a, const Link* b) {
        return std::tie(a->toNode, a->toPin, a->fromNode, a->fromPin) <
               std::tie(b->toNode, b->toPin, b->fromNode, b->fromPin);
    });

    std::vector<const Comment*> comments;
    comments.reserve(g.comments.size());
    for (const Comment& c : g.comments) comments.push_back(&c);
    std::sort(comments.begin(), comments.end(), [](const Comment* a, const Comment* b) { return a->id < b->id; });

    std::string out;
    out.reserve(256 + nodes.size() * 120 + links.size() * 40);
    out += "{\n";

    // Top-level keys in sorted order: comments compiler format links nextId nodes ownerGuid version.
    out += "  \"comments\": [";
    for (std::size_t i = 0; i < comments.size(); ++i)
    {
        const Comment& c = *comments[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"color\": ";
        AppendNumbers(out, c.color, 4);
        out += ", \"id\": " + std::to_string(c.id) + ", \"rect\": ";
        AppendNumbers(out, c.rect, 4);
        out += ", \"text\": ";
        AppendJsonString(out, c.text);
        out += '}';
    }
    out += comments.empty() ? "],\n" : "\n  ],\n";

    out += "  \"compiler\": " + std::to_string(g.compiler) + ",\n";
    out += "  \"format\": \"";
    out += g.function ? kFunctionFormatTag : kFormatTag;
    out += "\",\n";

    out += "  \"links\": [";
    for (std::size_t i = 0; i < links.size(); ++i)
    {
        const Link& l = *links[i];
        out += i ? ",\n    " : "\n    ";
        out += '[' + std::to_string(l.fromNode) + ", ";
        AppendJsonString(out, l.fromPin);
        out += ", " + std::to_string(l.toNode) + ", ";
        AppendJsonString(out, l.toPin);
        out += ']';
    }
    out += links.empty() ? "],\n" : "\n  ],\n";

    out += "  \"nextId\": " + std::to_string(g.nextId) + ",\n";

    out += "  \"nodes\": [";
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const Node& n = *nodes[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"id\": " + std::to_string(n.id);
        if (!n.inputs.empty())
        {
            out += ", \"inputs\": ";
            AppendValueMap(out, n.inputs);
        }
        out += ", \"pos\": ";
        AppendNumbers(out, n.pos, 2);
        if (!n.props.empty())
        {
            out += ", \"props\": ";
            AppendValueMap(out, n.props);
        }
        out += ", \"type\": ";
        AppendJsonString(out, n.type);
        out += '}';
    }
    out += nodes.empty() ? "],\n" : "\n  ],\n";

    out += "  \"ownerGuid\": ";
    AppendJsonString(out, g.ownerGuid);
    out += ",\n";
    out += "  \"version\": " + std::to_string(g.version) + "\n";
    out += "}\n";
    return out;
}

bool LoadGraph(const std::string& utf8Path, Graph& out, std::string& error)
{
    std::string text;
    if (!Sys::ReadFileText(utf8Path, text))
    {
        error = "cannot read '" + utf8Path + "'";
        return false;
    }
    if (!ParseGraph(text, out, error))
    {
        error = utf8Path + ": " + error;
        return false;
    }
    return true;
}

namespace
{
    struct CachedFunction
    {
        std::shared_ptr<const Graph> graph;
        std::string                  error;
        std::uintmax_t               size = 0;
        std::int64_t                 time = 0;
        double                       checkedAt = -1.0;
    };
    std::mutex                                      g_fnMutex;
    std::unordered_map<std::string, CachedFunction> g_functions;
    std::atomic<std::uint64_t>                      g_fnEpoch{ 1 };

    double NowSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

std::shared_ptr<const Graph> LoadFunctionCached(const std::string& utf8Path, std::string& error)
{
    std::lock_guard<std::mutex> lock(g_fnMutex);
    CachedFunction& c = g_functions[utf8Path];
    const double now = NowSeconds();
    if (c.checkedAt < 0.0 || now - c.checkedAt >= 0.25)
    {
        c.checkedAt = now;
        std::error_code ec;
        const std::filesystem::path p = Sys::U8Path(utf8Path);
        const std::uintmax_t size = std::filesystem::file_size(p, ec);
        const std::int64_t time = ec ? 0 : static_cast<std::int64_t>(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
        if (ec)
        {
            if (c.graph || c.error.empty()) g_fnEpoch.fetch_add(1);
            c.graph.reset();
            c.error = "cannot read '" + utf8Path + "'";
            c.size = 0;
            c.time = 0;
        }
        else if (size != c.size || time != c.time || (!c.graph && c.error.empty()))
        {
            auto g = std::make_shared<Graph>();
            std::string err;
            if (LoadGraph(utf8Path, *g, err)) { c.graph = std::move(g); c.error.clear(); }
            else                              { c.graph.reset(); c.error = err; }
            c.size = size;
            c.time = time;
            g_fnEpoch.fetch_add(1);
        }
    }
    if (!c.graph) error = c.error;
    return c.graph;
}

std::uint64_t FunctionEpoch()
{
    return g_fnEpoch.load();
}

void TouchCalledFunctions(const Graph& g)
{
    for (const Node& n : g.nodes)
        if (n.type == "MaterialFunctionCall")
        {
            std::string error;
            LoadFunctionCached(n.PropString("function"), error);
        }
}

bool SaveGraph(const std::string& utf8Path, const Graph& g, std::string& error)
{
    const std::string text = SerializeGraph(g);
    Sys::CreateParentDirs(utf8Path);
    bool replaceFailed = false;
    if (!Sys::WriteFileBytesAtomic(utf8Path, text.data(), text.size(), &replaceFailed))
    {
        error = replaceFailed ? "'" + utf8Path + "' stayed locked by another reader"
                              : "cannot write '" + utf8Path + "'";
        return false;
    }
    return true;
}
} // namespace MaterialGraph
