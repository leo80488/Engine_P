#include "MaterialGraph/MaterialGraphCompiler.h"

#include "ECS/Components.h"                 // BlendMode
#include "ECS/MaterialReflectionSync.h"     // the reserved cbuffer / texture names
#include "Graphics/ShadingModelTable.h"     // the pins each model reads, the authoring tokens
#include "Graphics/ShaderReflection.h"      // the @engine role names
#include "MaterialGraph/MaterialGraphNodes.h"
#include "System/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MaterialGraph
{
namespace
{
    // The engine compiles a per-material VS when the source DEFINES this (MaterialShaderDecl), so it
    // appears only in the #define, and only when World Position Offset is linked; a user's text that
    // names it is still rewritten (SafeText), which costs nothing.
    constexpr const char* kWpoToken = "MATERIAL_HAS_VERTEX_OFFSET";

    enum class Stage : std::uint8_t { Pixel = 0, Vertex = 1 };

    // ==== Values and HLSL spelling ====
    // One node output as HLSL: the code (an SSA name, a built-in or a literal)
    // and its float count. ok = false: the node failed; consumers skip silently
    // (the failure already has its diagnostic).
    struct Val
    {
        std::string code;
        int         n = 1;
        bool        ok = false;
        bool        literal = false;
        double      lit[4] = { 0, 0, 0, 0 };
    };

    Val Failed() { return Val{}; }

    Val Code(std::string code, int n)
    {
        Val v;
        v.code = std::move(code);
        v.n    = n;
        v.ok   = true;
        return v;
    }

    std::string TypeName(int n) { return n <= 1 ? std::string("float") : "float" + std::to_string(n); }

    std::string HlslFloat(double d)
    {
        std::string s = FormatNumber(d);
        if (s.find_first_of(".e") == std::string::npos) s += ".0";
        return s + "f";
    }

    Val Literal(const double* v, int n)
    {
        Val r;
        r.ok      = true;
        r.literal = true;
        r.n       = n;
        for (int i = 0; i < n && i < 4; ++i) r.lit[i] = v[i];
        if (n == 1) r.code = HlslFloat(v[0]);
        else
        {
            r.code = TypeName(n) + "(";
            for (int i = 0; i < n; ++i) r.code += (i ? ", " : "") + HlslFloat(v[i]);
            r.code += ")";
        }
        return r;
    }

    // An identifier or member access (n7_RGBA, p.uv0): swizzles apply directly.
    bool IsSimple(const std::string& c)
    {
        if (c.empty() || !(std::isalpha(static_cast<unsigned char>(c[0])) || c[0] == '_')) return false;
        for (const char ch : c)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.')) return false;
        return true;
    }

    std::string Wrap(const std::string& c) { return IsSimple(c) ? c : "(" + c + ")"; }

    // cast() of tools/cartoonstreet_migration/material_graph.py: the same count as is; fewer = .x / .xy /
    // .xyz; from a scalar = broadcast; otherwise padded with 0.
    Val CastVal(const Val& a, int n)
    {
        if (!a.ok || n == a.n) return a;
        if (a.literal)
        {
            double v[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < n; ++i) v[i] = (a.n == 1) ? a.lit[0] : (i < a.n ? a.lit[i] : 0.0);
            return Literal(v, n);
        }
        if (n < a.n) return Code(Wrap(a.code) + "." + std::string("xyzw").substr(0, static_cast<std::size_t>(n)), n);
        if (a.n == 1) return Code(Wrap(a.code) + "." + std::string(static_cast<std::size_t>(n), 'x'), n);
        std::string r = TypeName(n) + "(" + a.code;
        for (int i = a.n; i < n; ++i) r += ", 0.0f";
        return Code(r + ")", n);
    }

    std::string Cast(const Val& a, int n) { return CastVal(a, n).code; }

    // Channel swizzle (R, G, B, A, RGB, RGBA) of a value, read as a float4.
    Val Channels(const Val& a, const std::string& mask)
    {
        const int n = static_cast<int>(mask.size());
        auto index = [](char c) { return c == 'r' ? 0 : c == 'g' ? 1 : c == 'b' ? 2 : 3; };
        if (a.literal)
        {
            const Val four = CastVal(a, 4);
            double v[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < n; ++i) v[i] = four.lit[index(mask[static_cast<std::size_t>(i)])];
            return Literal(v, n);
        }
        bool inRange = true;
        for (const char c : mask) inRange = inRange && index(c) < a.n;
        if (inRange) return Code(Wrap(a.code) + "." + mask, n);
        return Code("(" + Cast(a, 4) + ")." + mask, n);
    }

    // ident() of tools/cartoonstreet_migration/material_graph.py: P_ + the name with every non
    // [A-Za-z0-9_] character (a UTF-8 code point counts once) replaced by '_'.
    std::string Ident(const std::string& name)
    {
        std::string r;
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (std::isalnum(c) || c == '_') { r += static_cast<char>(c); continue; }
            r += '_';
            if (c >= 0xC0)   // skip the code point's continuation bytes
                while (i + 1 < name.size() && (static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) ++i;
        }
        return r;
    }

    // Free text inside a comment or an annotation string: no line breaks, no
    // quote or '@' (the annotation parser splits on them), never the WPO token.
    std::string SafeText(const std::string& s)
    {
        std::string r;
        for (const char ch : s)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == '"' || c == '@') r += ' ';
            else r += ch;
        }
        for (std::size_t at = r.find(kWpoToken); at != std::string::npos; at = r.find(kWpoToken))
            r.replace(at, std::char_traits<char>::length(kWpoToken), "(reserved token)");
        return r;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool IsChannelPin(const std::string& pin)
    {
        return pin == "R" || pin == "G" || pin == "B" || pin == "A" || pin == "RGB" || pin == "RGBA";
    }

    // ==== Samplers ====
    struct SamplerKind
    {
        const char* name;
        double      fallback[4];   // what an UNBOUND slot reads (the engine binds default white)
    };
    constexpr SamplerKind kSamplers[] = {
        { "Color",     { 1.0, 1.0, 1.0, 1.0 } },
        { "Linear",    { 0.0, 0.0, 0.0, 1.0 } },
        { "Normal",    { 0.5, 0.5, 1.0, 1.0 } },
        { "Mask",      { 0.0, 0.0, 0.0, 1.0 } },
        { "Grayscale", { 0.0, 0.0, 0.0, 1.0 } },
    };

    const SamplerKind* FindSampler(const std::string& name)
    {
        for (const SamplerKind& s : kSamplers)
            if (Lower(name) == Lower(s.name)) return &s;
        return nullptr;
    }

    // ==== Output pin -> SurfaceData ====
    bool IsTranslucent(BlendMode b)
    {
        return b == BlendMode::Alpha || b == BlendMode::Additive || b == BlendMode::Premultiplied ||
               b == BlendMode::Multiply || b == BlendMode::Transmissive;
    }

    // ==== Functions expanded, static switches resolved ====
    // What the compile of an expanded graph needs to know about it.
    struct Expansion
    {
        // An expanded function's node -> the node of the graph a diagnostic and a
        // "// @n" marker name (the outermost call).
        std::unordered_map<std::uint32_t, std::uint32_t> alias;
        // Reachable with EVERY static branch: never "dead" (a branch not taken is not dead).
        std::unordered_set<std::uint32_t> potential;
    };

    constexpr int kMaxFunctionDepth = 8;

    class Flattener
    {
    public:
        Flattener(const Graph& in, const CompileOptions& o, Graph& out, Expansion& x, CompileResult& r)
            : m_in(in), m_opt(o), m_out(out), m_x(x), m_r(r) {}

        void Run()
        {
            m_out = m_in;
            if (m_in.function) FunctionPreview();
            Expand();
            Potential();
            ResolveStatics();
            m_r.functions.assign(m_functions.begin(), m_functions.end());
            std::sort(m_r.switches.begin(), m_r.switches.end(),
                      [](const StaticSwitchInfo& a, const StaticSwitchInfo& b) { return a.name < b.name; });
        }

    private:
        std::uint32_t Public(std::uint32_t id) const
        {
            const auto it = m_x.alias.find(id);
            return it == m_x.alias.end() ? id : it->second;
        }
        void Error(std::uint32_t node, const char* code, const std::string& msg)
        {
            m_r.diagnostics.push_back({ Public(node), "", Severity::Error, code, msg });
        }
        std::string Label(std::uint32_t id) const
        {
            const Node* n = m_out.FindNode(id);
            const NodeDesc* d = n ? FindNodeDesc(n->type) : nullptr;
            return (d ? std::string(d->label) : std::string("node")) + " #" + std::to_string(Public(id));
        }
        const Link* LinkInto(std::uint32_t node, const std::string& pin) const
        {
            for (const Link& l : m_out.links)
                if (l.toNode == node && l.toPin == pin) return &l;
            return nullptr;
        }
        void EraseLinksInto(std::uint32_t node, const std::string& pin)
        {
            m_out.links.erase(std::remove_if(m_out.links.begin(), m_out.links.end(),
                                             [&](const Link& l) { return l.toNode == node && l.toPin == pin; }),
                              m_out.links.end());
        }
        void Remove(std::uint32_t node)
        {
            m_out.nodes.erase(std::remove_if(m_out.nodes.begin(), m_out.nodes.end(), [&](const Node& n) { return n.id == node; }),
                              m_out.nodes.end());
            m_out.links.erase(std::remove_if(m_out.links.begin(), m_out.links.end(),
                                             [&](const Link& l) { return l.fromNode == node || l.toNode == node; }),
                              m_out.links.end());
        }

        // A function compiled on its own (its editor's preview): its first output, unlit.
        void FunctionPreview()
        {
            std::vector<FunctionPin> ins, outs;
            FunctionPins(m_out, ins, outs);
            if (outs.empty())
            {
                Error(0, "function_no_output", "a Material Function needs a Function Output");
                return;
            }
            Node output;
            output.id   = m_out.AllocateId();
            output.type = "MaterialOutput";
            m_x.alias[output.id] = outs.front().node;
            m_out.nodes.push_back(output);
            for (Node& n : m_out.nodes)
                if (n.type == "FunctionOutput")
                {
                    n.type = "Reroute";   // the pass-through its value reads
                    m_sinks.push_back(n.id);   // the other outputs are not dead, only not previewed
                }
            m_out.links.push_back({ outs.front().node, "Out", output.id, "EmissiveColor" });
        }

        void Expand()
        {
            struct Pending
            {
                std::uint32_t            id = 0;
                int                      depth = 0;
                std::vector<std::string> chain;
            };
            std::deque<Pending> queue;
            {
                std::vector<std::uint32_t> calls;
                for (const Node& n : m_out.nodes)
                    if (n.type == "MaterialFunctionCall") calls.push_back(n.id);
                std::sort(calls.begin(), calls.end());
                for (const std::uint32_t id : calls) queue.push_back({ id, 0, {} });
            }
            while (!queue.empty())
            {
                const Pending c = queue.front();
                queue.pop_front();
                const Node* call = m_out.FindNode(c.id);
                if (!call) continue;
                const std::string path = call->PropString("function");
                const ValueMap    callInputs = call->inputs;
                if (path.empty()) { Error(c.id, "function_missing", Label(c.id) + " has no function (props.function)"); Remove(c.id); continue; }
                if (c.depth >= kMaxFunctionDepth)
                {
                    Error(c.id, "function_depth", Label(c.id) + ": functions nest deeper than " + std::to_string(kMaxFunctionDepth));
                    Remove(c.id);
                    continue;
                }
                if (std::find(c.chain.begin(), c.chain.end(), path) != c.chain.end())
                {
                    Error(c.id, "function_recursive", Label(c.id) + ": '" + path + "' calls itself");
                    Remove(c.id);
                    continue;
                }
                std::string err;
                const std::shared_ptr<const Graph> fn = LoadFunctionCached(path, err);
                if (!fn || !fn->function)
                {
                    Error(c.id, "function_missing", Label(c.id) + ": " +
                          (fn ? "'" + path + "' is a material graph, not a Material Function (.imatfunc)" : err));
                    Remove(c.id);
                    continue;
                }
                std::vector<FunctionPin> ins, outs;
                FunctionPins(*fn, ins, outs);
                auto dup = [](const std::vector<FunctionPin>& v) -> std::string {
                    for (std::size_t i = 1; i < v.size(); ++i)
                        for (std::size_t j = 0; j < i; ++j)
                            if (v[i].name == v[j].name) return v[i].name;
                    return {};
                };
                if (const std::string d = !dup(ins).empty() ? dup(ins) : dup(outs); !d.empty())
                {
                    Error(c.id, "function_pin_duplicate", Label(c.id) + ": '" + path + "' has two pins named '" + d + "'");
                    Remove(c.id);
                    continue;
                }
                bool pinsOk = true;
                for (const Link& l : m_out.links)
                {
                    const bool in  = l.toNode == c.id &&
                                     std::none_of(ins.begin(), ins.end(), [&](const FunctionPin& p) { return p.name == l.toPin; });
                    const bool out = l.fromNode == c.id &&
                                     std::none_of(outs.begin(), outs.end(), [&](const FunctionPin& p) { return p.name == l.fromPin; });
                    if (in || out)
                    {
                        Error(c.id, "unknown_pin", Label(c.id) + ": '" + path + "' has no " + (in ? "input '" + l.toPin : "output '" + l.fromPin) + "'");
                        pinsOk = false;
                    }
                }
                if (!pinsOk) { Remove(c.id); continue; }
                m_functions.insert(path);

                // The function's nodes, fresh ids in its id order; its calls are expanded next.
                std::vector<const Node*> fnNodes;
                for (const Node& n : fn->nodes) fnNodes.push_back(&n);
                std::sort(fnNodes.begin(), fnNodes.end(), [](const Node* a, const Node* b) { return a->id < b->id; });
                std::unordered_map<std::uint32_t, std::uint32_t> ids;
                const std::uint32_t pub = Public(c.id);
                for (const Node* n : fnNodes)
                {
                    Node copy = *n;
                    copy.id = m_out.AllocateId();
                    ids[n->id] = copy.id;
                    m_x.alias[copy.id] = pub;
                    if (copy.type == "MaterialFunctionCall")
                    {
                        std::vector<std::string> chain = c.chain;
                        chain.push_back(path);
                        queue.push_back({ copy.id, c.depth + 1, std::move(chain) });
                    }
                    m_out.nodes.push_back(std::move(copy));
                }
                for (const Link& l : fn->links)
                    if (ids.count(l.fromNode) && ids.count(l.toNode))
                        m_out.links.push_back({ ids[l.fromNode], l.fromPin, ids[l.toNode], l.toPin });
                // Inputs: the call's link or value, else the Function Input's own Preview.
                for (const FunctionPin& p : ins)
                {
                    const std::uint32_t k = ids[p.node];
                    if (const Link* l = LinkInto(c.id, p.name))
                    {
                        const Link bound{ l->fromNode, l->fromPin, k, "Preview" };
                        EraseLinksInto(k, "Preview");
                        m_out.links.push_back(bound);
                        m_out.FindNode(k)->inputs.erase("Preview");
                    }
                    else if (const auto it = callInputs.find(p.name); it != callInputs.end())
                    {
                        EraseLinksInto(k, "Preview");
                        m_out.FindNode(k)->inputs["Preview"] = it->second;
                    }
                }
                // Outputs: pass-throughs the call's consumers now read.
                for (const FunctionPin& p : outs)
                {
                    const std::uint32_t o = ids[p.node];
                    m_out.FindNode(o)->type = "Reroute";
                    for (Link& l : m_out.links)
                        if (l.fromNode == c.id && l.fromPin == p.name)
                        {
                            l.fromNode = o;
                            l.fromPin  = "Out";
                        }
                }
                Remove(c.id);
            }
        }

        // Everything the Output reaches with both branches of every static switch.
        void Potential()
        {
            std::vector<std::uint32_t> stack(m_sinks.begin(), m_sinks.end());
            for (const Node& n : m_out.nodes)
                if (n.type == "MaterialOutput") stack.push_back(n.id);
            while (!stack.empty())
            {
                const std::uint32_t id = stack.back();
                stack.pop_back();
                if (!m_x.potential.insert(id).second) continue;
                for (const Link& l : m_out.links)
                    if (l.toNode == id) stack.push_back(l.fromNode);
                const Node* n = m_out.FindNode(id);
                if (n && n->type == "NamedRerouteUsage")
                    for (const Node& d : m_out.nodes)
                        if (d.type == "NamedRerouteDeclaration" && d.PropString("name") == n->PropString("name")) stack.push_back(d.id);
            }
        }

        // A Static Switch's Value: a Static Bool, through reroutes and function inputs.
        bool StaticValue(const Node& sw, bool& out) const
        {
            const Link* l = LinkInto(sw.id, "Value");
            if (!l)
            {
                const auto it = sw.inputs.find("Value");
                out = it != sw.inputs.end() && it->second.IsNumeric() ? it->second.At(0) != 0.0 : sw.PropBool("default", true);
                return true;
            }
            std::uint32_t cur = l->fromNode;
            for (int step = 0; step < 64; ++step)
            {
                const Node* n = m_out.FindNode(cur);
                if (!n) return false;
                if (n->type == "StaticBool") { out = n->PropBool("value", true); return true; }
                const char* pin = n->type == "Reroute" || n->type == "NamedRerouteDeclaration" ? "Input"
                                : n->type == "FunctionInput"                                 ? "Preview"
                                                                                              : nullptr;
                if (n->type == "NamedRerouteUsage")
                {
                    const Node* decl = nullptr;
                    for (const Node& d : m_out.nodes)
                        if (d.type == "NamedRerouteDeclaration" && d.PropString("name") == n->PropString("name")) decl = &d;
                    if (!decl) return false;
                    cur = decl->id;
                    continue;
                }
                if (!pin) return false;
                const Link* up = LinkInto(cur, pin);
                if (!up)
                {
                    // A stored value (a Function Input's Preview, a reroute's input) is static too.
                    const auto it = n->inputs.find(pin);
                    if (it == n->inputs.end() || !it->second.IsNumeric()) return false;
                    out = it->second.At(0) != 0.0;
                    return true;
                }
                cur = up->fromNode;
            }
            return false;
        }

        // A switch keeps the chosen branch: a pass-through (Reroute) of it.
        void Choose(std::uint32_t id, bool value)
        {
            const std::string chosen = value ? "True" : "False", other = value ? "False" : "True";
            EraseLinksInto(id, other);
            EraseLinksInto(id, "Value");
            Node* n = m_out.FindNode(id);
            bool linked = false;
            for (Link& l : m_out.links)
                if (l.toNode == id && l.toPin == chosen) { l.toPin = "Input"; linked = true; }
            Value fallback = Value::Number(value ? 1.0 : 0.0);   // the pins' defaults
            if (const auto it = n->inputs.find(chosen); it != n->inputs.end()) fallback = it->second;
            n->inputs.clear();
            if (!linked) n->inputs["Input"] = fallback;
            n->type = "Reroute";
        }

        void ResolveStatics()
        {
            // Static switches are their own namespace (as in Unreal: a switch and a texture may both be
            // "Normal" - the .imat keys them apart, sswitch_ / ctex_); same-named switches are ONE switch.
            std::vector<std::uint32_t> sp, sw;
            for (const Node& n : m_out.nodes)
            {
                if (n.type == "StaticSwitchParameter") sp.push_back(n.id);
                else if (n.type == "StaticSwitch") sw.push_back(n.id);
            }
            std::sort(sp.begin(), sp.end());
            std::sort(sw.begin(), sw.end());
            for (const std::uint32_t id : sp)
            {
                const Node& n = *m_out.FindNode(id);
                const std::string name = n.PropString("name");
                const bool def = n.PropBool("default", true);
                auto known = std::find_if(m_r.switches.begin(), m_r.switches.end(),
                                          [&](const StaticSwitchInfo& s) { return s.name == name; });
                if (name.empty()) Error(id, "param_name_invalid", Label(id) + " has no name (props.name)");
                else if (known != m_r.switches.end() && known->def != def)
                    Error(id, "param_name_duplicate", "static switch '" + name + "' is also declared by " + Label(known->node) +
                          " (different defaults)");
                const auto opt = m_opt.switches.find(name);
                const bool value = opt != m_opt.switches.end() ? opt->second : known != m_r.switches.end() ? known->def : def;
                if (known == m_r.switches.end() && !name.empty())
                    m_r.switches.push_back({ name, def, value, n.PropString("group"), n.PropNumber("sort", 0.0), Public(id) });
                Choose(id, value);
            }
            for (const std::uint32_t id : sw)
            {
                bool value = true;
                if (!StaticValue(*m_out.FindNode(id), value))
                {
                    Error(id, "static_value_required", Label(id) + ": Value must be a Static Bool (through reroutes and "
                          "function inputs) - it is chosen when compiled");
                    value = m_out.FindNode(id)->PropBool("default", true);
                }
                Choose(id, value);
            }
        }

        const Graph&          m_in;
        const CompileOptions& m_opt;
        Graph&                m_out;
        Expansion&            m_x;
        CompileResult&        m_r;
        std::set<std::string> m_functions;

        std::vector<std::uint32_t> m_sinks;
    };

    bool NeedsExpansion(const Graph& g)
    {
        if (g.function) return true;
        for (const Node& n : g.nodes)
            if (n.type == "MaterialFunctionCall" || n.type == "StaticSwitch" || n.type == "StaticSwitchParameter") return true;
        return false;
    }

    // ==== The compile ====
    struct TexSlot
    {
        TextureInfo info;
        bool        pixel = false;
    };

    struct StageGen
    {
        Stage                                             stage = Stage::Pixel;
        std::vector<std::string>                          lines;
        std::unordered_map<std::uint32_t, Val>            base;      // node -> its base value
        std::map<std::pair<std::uint32_t, std::string>, Val> pins;  // (node, output pin) -> value
        std::unordered_set<std::uint32_t>                 done;
        std::unordered_set<std::uint32_t>                 failed;
    };

    class Compiler
    {
    public:
        Compiler(const Graph& g, const CompileOptions& o, CompileResult& r, const Expansion* x = nullptr)
            : m_g(g), m_opt(o), m_res(r), m_x(x) {}

        void Run()
        {
            m_res.stats.nodes = static_cast<int>(m_g.nodes.size());
            const bool indexed = Index();
            m_res.graphHash   = GraphHash();   // after Index: a preview's pin as resolved
            if (!indexed) return;
            ValidateParameters();
            FindRoots();
            Reach();
            AssignTextures();
            FindOpacityMaskTexture();
            LayoutParams();
            if (HasError()) return;

            StageGen ps;
            ps.stage = Stage::Pixel;
            if (m_usesPixelNormal)
            {
                // Pixel Normal WS is the Normal pin's result: that pin first, then mg_PixelNormalWS, then the
                // pins that may read it. Only such graphs are reordered, so every other graph's line order is kept.
                std::stable_partition(m_psRoots.begin(), m_psRoots.end(),
                                      [this](const std::pair<int, const Link*>& r) { return r.first == m_normalRoot; });
                if (m_normalRoot < 0) EmitPixelNormal(ps);
            }
            for (const auto& [pinIdx, _] : m_psRoots)
            {
                Evaluate(ps, m_output->id, pinIdx);
                if (m_usesPixelNormal && pinIdx == m_normalRoot) EmitPixelNormal(ps);
            }
            std::vector<std::string> writes = PixelWrites(ps);

            StageGen vs;
            vs.stage = Stage::Vertex;
            std::string wpoReturn;
            if (m_vsRoot >= 0)
            {
                Evaluate(vs, m_output->id, m_vsRoot);
                const Val v = OutputValue(vs, m_vsRoot);
                if (v.ok) wpoReturn = "    return " + Cast(v, 3) + "; // @n" + std::to_string(m_output->id);
            }
            if (HasError()) return;
            Assemble(ps, writes, vs, wpoReturn);
        }

        void Finish()
        {
            m_res.textures.clear();
            for (const TexSlot& t : m_textures)
            {
                m_res.textures.push_back(t.info);
                for (std::uint32_t& id : m_res.textures.back().nodes) id = Public(id);   // calls, not their insides
            }
            m_res.stats.textures = static_cast<int>(m_res.textures.size());
            m_res.ok = !HasError();
            if (!m_res.ok) m_res.hlsl.clear();
            // Errors first, then warnings; each by node id - a stable order to diff.
            std::stable_sort(m_res.diagnostics.begin(), m_res.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
                if (a.severity != b.severity) return a.severity > b.severity;
                return a.node < b.node;
            });
        }

    private:
        // The node a diagnostic or a marker names: an expanded function's is its call.
        std::uint32_t Public(std::uint32_t id) const
        {
            if (!m_x) return id;
            const auto it = m_x->alias.find(id);
            return it == m_x->alias.end() ? id : it->second;
        }

        // ---- diagnostics ----
        void Diag(Severity s, std::uint32_t node, const std::string& pin, const char* code, const std::string& msg)
        {
            node = Public(node);
            const std::string key = std::to_string(node) + "|" + pin + "|" + code + "|" + msg;
            if (!m_diagSeen.insert(key).second) return;
            m_res.diagnostics.push_back({ node, pin, s, code, msg });
        }
        void Error(std::uint32_t node, const std::string& pin, const char* code, const std::string& msg)
        {
            Diag(Severity::Error, node, pin, code, msg);
        }
        void Warn(std::uint32_t node, const std::string& pin, const char* code, const std::string& msg)
        {
            Diag(Severity::Warning, node, pin, code, msg);
        }
        bool HasError() const
        {
            for (const Diagnostic& d : m_res.diagnostics)
                if (d.severity == Severity::Error) return true;
            return false;
        }

        std::string Label(std::uint32_t id) const
        {
            const auto it = m_desc.find(id);
            if (Public(id) != id)   // inside an expanded function: say so, and name its call
                return (it != m_desc.end() ? std::string(it->second->label) : std::string("node")) + " (in the function of #" +
                       std::to_string(Public(id)) + ")";
            return (it != m_desc.end() ? std::string(it->second->label) : std::string("node")) + " #" + std::to_string(id);
        }

        std::string GraphHash() const
        {
            Graph h = m_g;
            for (Node& n : h.nodes) n.pos[0] = n.pos[1] = 0.0f;
            h.comments.clear();
            h.ownerGuid.clear();
            h.nextId = 1;
            std::string text = SerializeGraph(h);
            text += "compiler " + std::to_string(kCompilerVersion) + " shading " +
                    std::to_string(static_cast<int>(m_opt.shadingModel)) + " blend " +
                    std::to_string(static_cast<int>(m_opt.blendMode)) + "\n";
            if (m_opt.previewNode)   // "" and the node's first output are the same preview
                text += "preview " + std::to_string(m_opt.previewNode) + " " +
                        (m_previewLink ? m_previewLink->fromPin : m_opt.previewPin) + "\n";
            return Sys::Sha256Hex(text.data(), text.size()).substr(0, 16);
        }

        // ---- Step 1: nodes, links, cycles, the output ----
        bool Index()
        {
            for (const Node& n : m_g.nodes)
            {
                if (m_nodes.count(n.id)) { Error(n.id, "", "duplicate_node_id", "node id " + std::to_string(n.id) + " is used twice"); continue; }
                m_nodes[n.id] = &n;
                const NodeDesc* d = DescribeNode(n);   // a Custom node's pins are its props'
                if (!d) { Error(n.id, "", "unknown_node_type", "unknown node type '" + n.type + "'"); continue; }
                m_desc[n.id] = d;
                if (d->flags & kNodeOutput)
                {
                    if (m_output) Error(n.id, "", "multiple_outputs", "a graph has one Material Output; #" +
                                        std::to_string(m_output->id) + " is the first");
                    else m_output = &n;
                }
            }
            if (!m_output && !HasError()) Error(0, "", "no_output", "the graph has no Material Output node");

            for (const Link& l : m_g.links)
            {
                const auto fromD = m_desc.find(l.fromNode);
                const auto toD   = m_desc.find(l.toNode);
                if (!m_nodes.count(l.fromNode)) { Error(l.toNode, l.toPin, "missing_node", "linked from node #" + std::to_string(l.fromNode) + ", which does not exist"); continue; }
                if (!m_nodes.count(l.toNode))   { Error(l.fromNode, l.fromPin, "missing_node", "linked to node #" + std::to_string(l.toNode) + ", which does not exist"); continue; }
                if (fromD == m_desc.end() || toD == m_desc.end()) continue;   // unknown type: already an error
                if (fromD->second->OutputIndex(l.fromPin) < 0)
                {
                    Error(l.fromNode, l.fromPin, "unknown_pin", Label(l.fromNode) + " has no output '" + l.fromPin + "'");
                    continue;
                }
                const int in = toD->second->InputIndex(l.toPin);
                if (in < 0) { Error(l.toNode, l.toPin, "unknown_pin", Label(l.toNode) + " has no input '" + l.toPin + "'"); continue; }
                if (toD->second->inputs[static_cast<std::size_t>(in)].hidden)
                {
                    Error(l.toNode, l.toPin, "unknown_pin", "input '" + l.toPin + "' of " + Label(l.toNode) +
                          " is linked by the compiler, not by hand");
                    continue;
                }
                const auto key = std::make_pair(l.toNode, in);
                if (m_inLinks.count(key)) { Error(l.toNode, l.toPin, "input_linked_twice", "input '" + l.toPin + "' of " + Label(l.toNode) + " has two links"); continue; }
                m_inLinks[key] = &l;
            }
            ResolveReroutes();
            ResolvePreview();
            if (HasError()) return false;
            return !FindCycles();
        }

        // A Named Reroute Usage reads its declaration: a link the compiler adds into its hidden input, so
        // cycles, reachability and order see it as any other.
        void ResolveReroutes()
        {
            std::vector<const Node*> decls, uses;
            for (const Node& n : m_g.nodes)
            {
                if (n.type == "NamedRerouteDeclaration") decls.push_back(&n);
                else if (n.type == "NamedRerouteUsage") uses.push_back(&n);
            }
            if (uses.empty() && decls.empty()) return;
            auto byId = [](const Node* a, const Node* b) { return a->id < b->id; };
            std::sort(decls.begin(), decls.end(), byId);
            std::sort(uses.begin(), uses.end(), byId);
            std::map<std::string, std::uint32_t> named;
            for (const Node* n : decls)
            {
                const std::string name = n->PropString("name");
                if (name.empty()) { Error(n->id, "", "reroute_name_invalid", Label(n->id) + " has no name (props.name)"); continue; }
                const auto [it, fresh] = named.emplace(name, n->id);
                if (!fresh)
                {
                    Error(n->id, "", "reroute_name_duplicate", "Named Reroute '" + name + "' is also declared by " + Label(it->second));
                    Error(it->second, "", "reroute_name_duplicate", "Named Reroute '" + name + "' is also declared by " + Label(n->id));
                }
            }
            for (const Node* n : uses)
            {
                const std::string name = n->PropString("name");
                const auto it = named.find(name);
                if (it == named.end())
                {
                    Error(n->id, "", "reroute_unknown", Label(n->id) + ": no Named Reroute is called '" + name + "'");
                    continue;
                }
                const int in = m_desc[n->id]->InputIndex("Input");
                m_synth.push_back({ it->second, "Out", n->id, "Input" });
                m_inLinks[{ n->id, in }] = &m_synth.back();
            }
        }

        // Node preview: the chosen pin feeds Emissive Color in place of the Output's links.
        void ResolvePreview()
        {
            if (!m_opt.previewNode || !m_output || m_opt.previewNode == m_output->id) return;
            const auto it = m_desc.find(m_opt.previewNode);
            if (it == m_desc.end())
            {
                Error(0, "", "preview_unknown", "the previewed node #" + std::to_string(m_opt.previewNode) + " does not exist");
                return;
            }
            const NodeDesc& d = *it->second;
            const std::string pin = !m_opt.previewPin.empty() ? m_opt.previewPin
                                  : d.outputs.empty()          ? std::string()
                                                                : std::string(d.outputs.front().name);
            if (d.OutputIndex(pin) < 0)
            {
                Error(m_opt.previewNode, pin, "preview_unknown", Label(m_opt.previewNode) + " has no output '" + pin + "' to preview");
                return;
            }
            m_previewInput = m_desc[m_output->id]->InputIndex("EmissiveColor");
            m_synth.push_back({ m_opt.previewNode, pin, m_output->id, "EmissiveColor" });
            m_previewLink = &m_synth.back();
        }

        // The link feeding a Material Output input - the preview's in place of Emissive Color.
        const Link* RootLink(int input) const
        {
            if (m_previewLink) return input == m_previewInput ? m_previewLink : nullptr;
            return InLink(m_output->id, input);
        }

        const Link* InLink(std::uint32_t node, int input) const
        {
            const auto it = m_inLinks.find({ node, input });
            return it != m_inLinks.end() ? it->second : nullptr;
        }

        // Iterative DFS (a deep chain must not overflow a worker's stack).
        bool FindCycles()
        {
            std::vector<std::uint32_t> ids;
            for (const auto& [id, _] : m_desc) ids.push_back(id);
            std::sort(ids.begin(), ids.end());
            std::unordered_map<std::uint32_t, int> color;   // 0 white, 1 on the path, 2 done
            bool any = false;
            for (const std::uint32_t root : ids)
            {
                if (color[root] != 0) continue;
                std::vector<std::pair<std::uint32_t, std::size_t>> stack{ { root, 0 } };
                color[root] = 1;
                while (!stack.empty())
                {
                    auto& [id, next] = stack.back();
                    const NodeDesc* d = m_desc[id];
                    if (next < d->inputs.size())
                    {
                        const Link* l = InLink(id, static_cast<int>(next++));
                        if (!l) continue;
                        const int c = color[l->fromNode];
                        if (c == 0) { color[l->fromNode] = 1; stack.push_back({ l->fromNode, 0 }); }
                        else if (c == 1)
                        {
                            // The stack from l->fromNode up to here is the cycle.
                            std::vector<std::uint32_t> cycle;
                            bool in = false;
                            for (const auto& [sid, _] : stack)
                            {
                                if (sid == l->fromNode) in = true;
                                if (in) cycle.push_back(sid);
                            }
                            std::string path;
                            for (const std::uint32_t sid : cycle) path += (path.empty() ? "#" : " -> #") + std::to_string(sid);
                            path += " -> #" + std::to_string(l->fromNode);
                            for (const std::uint32_t sid : cycle)
                                Error(sid, "", "cycle", "this node is part of a cycle: " + path);
                            any = true;
                        }
                        continue;
                    }
                    color[id] = 2;
                    stack.pop_back();
                }
            }
            return any;
        }

        // ---- Step 1b: parameter names ----
        void ValidateParameters()
        {
            std::map<std::string, std::uint32_t> seen;   // Ident(name) -> first node
            std::vector<const Node*> params;
            for (const Node& n : m_g.nodes)
            {
                const auto it = m_desc.find(n.id);
                if (it != m_desc.end() && (it->second->flags & kNodeParameter)) params.push_back(&n);
            }
            std::sort(params.begin(), params.end(), [](const Node* a, const Node* b) { return a->id < b->id; });
            for (const Node* n : params)
            {
                const std::string name = n->PropString("name");
                const bool texture = (m_desc[n->id]->flags & kNodeTexture) != 0;
                if (name.empty()) { Error(n->id, "", "param_name_invalid", Label(n->id) + " has no name (props.name)"); continue; }
                if (name[0] == '_')
                {
                    Error(n->id, "", "param_name_reserved", "'" + name + "' starts with '_': the reflection hides such names (authoring pads)");
                    continue;
                }
                if (name.rfind("Tex_", 0) == 0)
                {
                    Error(n->id, "", "param_name_reserved", "'" + name + "' starts with 'Tex_': that prefix is the b8 bindless-texture convention");
                    continue;
                }
                if (texture && name.rfind("Fixed_", 0) == 0)
                {
                    Error(n->id, "", "param_name_reserved", "'" + name + "' starts with 'Fixed_': T_Fixed_* are the fixed Texture Sample slots");
                    continue;
                }
                if (name.find(kWpoToken) != std::string::npos ||
                    MaterialReflectionSync::IsReservedCBuffer(name.c_str()) ||
                    MaterialReflectionSync::IsReservedTexture(name.c_str()))
                {
                    Error(n->id, "", "param_name_reserved", "'" + name + "' is a name the engine reserves");
                    continue;
                }
                const std::string id = Ident(name);
                const auto [it, fresh] = seen.emplace(id, n->id);
                m_paramOwner[n->id] = it->second;
                if (fresh) continue;
                // Nodes of one kind with one name are ONE parameter (Unreal's rule): one b8 entry or texture
                // slot, defined by the first; they must agree on what it is.
                const std::string why = ParamConflict(*m_nodes[it->second], *n);
                if (why.empty()) continue;
                Error(n->id, "", "param_name_duplicate", "parameter '" + name + "' is also declared by " + Label(it->second) +
                      " (" + why + ")");
                Error(it->second, "", "param_name_duplicate", "parameter '" + name + "' is also declared by " + Label(n->id) +
                      " (" + why + ")");
            }
        }

        // Why two same-named parameter nodes cannot be one parameter ("" when they can).
        std::string ParamConflict(const Node& a, const Node& b) const
        {
            if (a.PropString("name") != b.PropString("name"))
                return "both are " + Ident(a.PropString("name")) + " in HLSL";
            if (a.type != b.type) return "a " + std::string(m_desc.at(a.id)->label) + " and a " + m_desc.at(b.id)->label;
            if (a.type == "TextureSampleParameter2D")
            {
                if (a.PropString("default") != b.PropString("default")) return "different default textures";
                if (Lower(a.PropString("sampler", "Color")) != Lower(b.PropString("sampler", "Color"))) return "different samplers";
                return {};
            }
            const int n = a.type == "VectorParameter" ? 4 : 1;
            for (int i = 0; i < n; ++i)
                if (ParamDefault(a, i) != ParamDefault(b, i)) return "different defaults";
            return {};
        }

        // A scalar / vector parameter's default component (as LayoutParams reads it).
        static float ParamDefault(const Node& n, int i)
        {
            const bool vec = n.type == "VectorParameter";
            float d = vec && i == 3 ? 1.0f : 0.0f;
            if (const Value* v = n.Prop("default"); v && v->IsNumeric())
                d = static_cast<float>(v->Components() == 1 ? v->At(0) : v->At(i));
            return d;
        }

        // ---- Step 2: active outputs and reachability ----
        void FindRoots()
        {
            if (!m_output) return;
            if (m_previewLink)
            {
                m_psRoots.push_back({ m_previewInput, m_previewLink });   // the previewed pin, and nothing else
                return;
            }
            const NodeDesc& d = *m_desc[m_output->id];
            const bool unlit = m_opt.shadingModel == ShadingModel::Unlit;
            for (int i = 0; i < static_cast<int>(d.inputs.size()); ++i)
            {
                const Link* l = InLink(m_output->id, i);
                if (!l) continue;
                const std::string pin = d.inputs[static_cast<std::size_t>(i)].name;
                if (!OutputPinActive(pin, m_opt.shadingModel, m_opt.blendMode))
                {
                    Warn(m_output->id, pin, "inactive_pin", "'" + std::string(d.inputs[static_cast<std::size_t>(i)].label) +
                         "' is not used by shading model " + ShadingModelName(m_opt.shadingModel) + " / blend mode " +
                         BlendModeLabel(m_opt.blendMode) +
                         (unlit && pin == "BaseColor" ? " (a translucent Unlit shows Emissive Color alone)" : "") +
                         "; its link is ignored");
                    continue;
                }
                if (pin == "WorldPositionOffset")
                {
                    // An offset that is a zero constant (through reroutes - what a static switch leaves when its
                    // branch is "no offset") moves nothing: no vertex stage.
                    if (IsZeroConstant(l)) m_wpoZero = i;
                    else m_vsRoot = i;
                }
                else
                {
                    if (pin == "Normal") m_normalRoot = i;
                    m_psRoots.push_back({ i, l });
                }
            }
        }

        // A value that is zero whatever it is read as: a zero Constant .. Constant4Vector, or a
        // reroute of one (its stored value or its pin's default when unlinked).
        bool IsZeroConstant(const Link* l) const
        {
            std::uint32_t cur = l->fromNode;
            for (int step = 0; step < 64; ++step)
            {
                const auto it = m_nodes.find(cur);
                if (it == m_nodes.end()) return false;
                const Node& n = *it->second;
                if (n.type == "Constant2Vector") return false;   // a float2 into a float3 pin: the type error stays
                if (n.type == "Constant" || n.type == "Constant3Vector" || n.type == "Constant4Vector")
                {
                    const Value* v = n.Prop("value");
                    if (!v) return true;   // the props' default: zeros (a Constant4Vector's alpha is not an offset)
                    const int k = n.type == "Constant" ? 1 : n.type[8] - '0';
                    for (int c = 0; c < std::min(k, 3); ++c)
                        if (v->At(c) != 0.0) return false;
                    return true;
                }
                if (n.type != "Reroute" && n.type != "NamedRerouteDeclaration" && n.type != "NamedRerouteUsage") return false;
                const Link* up = InLink(cur, m_desc.at(cur)->InputIndex("Input"));
                if (!up)
                {
                    const auto in = n.inputs.find("Input");
                    if (in == n.inputs.end()) return true;   // Reroute's Input defaults to 0
                    if (!in->second.IsNumeric()) return false;
                    for (int c = 0; c < std::max(1, in->second.Components()); ++c)
                        if (in->second.At(c) != 0.0) return false;
                    return true;
                }
                cur = up->fromNode;
            }
            return false;
        }

        void Walk(int input, std::unordered_set<std::uint32_t>& reach)
        {
            std::vector<std::uint32_t> stack;
            if (const Link* l = RootLink(input)) stack.push_back(l->fromNode);
            while (!stack.empty())
            {
                const std::uint32_t id = stack.back();
                stack.pop_back();
                if (!reach.insert(id).second) continue;
                const NodeDesc* d = m_desc[id];
                for (int i = 0; i < static_cast<int>(d->inputs.size()); ++i)
                    if (const Link* l = InLink(id, i)) stack.push_back(l->fromNode);
            }
        }

        void Reach()
        {
            if (!m_output) return;
            for (const auto& [i, _] : m_psRoots) Walk(i, m_reachPS);
            if (m_vsRoot >= 0) Walk(m_vsRoot, m_reachVS);
            std::unordered_set<std::uint32_t> folded;   // a zero offset's chain: linked, not compiled, not dead
            if (m_wpoZero >= 0) Walk(m_wpoZero, folded);
            std::vector<std::uint32_t> dead;
            for (const auto& [id, d] : m_desc)
            {
                if (d->flags & kNodeOutput) continue;
                const bool r = m_reachPS.count(id) || m_reachVS.count(id);
                if (r) ++m_res.stats.reachable;
                else dead.push_back(id);
                if (m_reachPS.count(id) && std::string_view(d->type) == "PixelNormalWS") m_usesPixelNormal = true;
            }
            std::sort(dead.begin(), dead.end());
            if (!m_previewLink)   // previewing one node, the rest is not dead
                for (const std::uint32_t id : dead)
                {
                    // A function's unused parts and a static branch not taken are not dead.
                    if (m_x && (Public(id) != id || m_x->potential.count(id))) continue;
                    if (folded.count(id)) continue;
                    Warn(id, "", "dead_node", Label(id) + " reaches no active output pin; it is not compiled");
                }
            // Pixel Normal WS is the Normal output's result: it cannot feed the Normal pin.
            if (m_usesPixelNormal && m_normalRoot >= 0)
            {
                std::unordered_set<std::uint32_t> normal;
                Walk(m_normalRoot, normal);
                std::vector<std::uint32_t> bad;
                for (const std::uint32_t id : normal)
                    if (std::string_view(m_desc[id]->type) == "PixelNormalWS") bad.push_back(id);
                std::sort(bad.begin(), bad.end());
                for (const std::uint32_t id : bad)
                    Error(id, "", "pixel_normal_cycle", Label(id) + " is the Normal output's result: it cannot feed the Normal pin");
            }
        }

        bool Reached(std::uint32_t id) const { return m_reachPS.count(id) || m_reachVS.count(id); }

        // ---- Step 3: texture slots ----
        void AssignTextures()
        {
            std::map<std::string, std::size_t> byName;
            std::vector<std::uint32_t> ids;
            for (const auto& [id, d] : m_desc)
                if ((d->flags & kNodeTexture) && Reached(id)) ids.push_back(id);
            std::sort(ids.begin(), ids.end());
            for (const std::uint32_t id : ids)
            {
                const bool param = (m_desc[id]->flags & kNodeParameter) != 0;
                // A parameter is its name's first node (same-named nodes are one parameter).
                const auto owner = m_paramOwner.find(id);
                const Node& n = *m_nodes[param && owner != m_paramOwner.end() ? owner->second : id];
                TextureInfo t;
                t.parameter = param;
                if (param)
                {
                    t.name        = n.PropString("name");
                    t.hlslName    = "T_" + Ident(t.name);
                    t.defaultPath = n.PropString("default");
                    t.group       = n.PropString("group");
                    t.sort        = n.PropNumber("sort", 0.0);
                }
                else
                {
                    t.defaultPath = n.PropString("texture");
                    const std::string h = Sys::Sha256Hex(t.defaultPath.data(), t.defaultPath.size());
                    t.hlslName = "T_Fixed_" + h.substr(0, 8);
                    if (t.defaultPath.empty())
                        Warn(id, "", "texture_unset", Label(id) + " has no texture (props.texture): it samples the fallback");
                }
                std::string sampler = n.PropString("sampler", "Color");
                if (!FindSampler(sampler))
                {
                    Warn(id, "", "bad_value", "sampler '" + sampler + "' is not Color, Linear, Normal, Mask or Grayscale; using Color");
                    sampler = "Color";
                }
                t.sampler = FindSampler(sampler)->name;
                const auto it = byName.find(t.hlslName);
                if (it != byName.end())
                {
                    m_textures[it->second].info.nodes.push_back(id);
                    m_texOfNode[id] = it->second;
                    continue;
                }
                t.nodes.push_back(id);
                byName[t.hlslName] = m_textures.size();
                m_texOfNode[id] = m_textures.size();
                m_textures.push_back({ std::move(t), false });
            }

            // Parameters first (group, sort, name), then the fixed ones by name.
            std::vector<std::size_t> order(m_textures.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                const TextureInfo& x = m_textures[a].info;
                const TextureInfo& y = m_textures[b].info;
                if (x.parameter != y.parameter) return x.parameter;
                if (x.group != y.group) return x.group < y.group;
                if (x.sort != y.sort) return x.sort < y.sort;
                return x.hlslName < y.hlslName;
            });
            // Every model gets all eight: a Toon graph's shader says `//@engine explicit` (Assemble), so the
            // material packer reads no toon ramp from t3.
            if (static_cast<int>(m_textures.size()) > kMaxTextures)
            {
                Error(0, "", "capacity_textures", std::to_string(m_textures.size()) + " textures; a material has " +
                      std::to_string(kMaxTextures) + " space3 slots");
                return;
            }
            std::vector<TexSlot> sorted;
            std::uint32_t slot = 0;
            for (const std::size_t i : order)
            {
                sorted.push_back(std::move(m_textures[i]));
                sorted.back().info.slot = slot++;
            }
            m_textures = std::move(sorted);
            m_texOfNode.clear();
            for (std::size_t i = 0; i < m_textures.size(); ++i)
                for (const std::uint32_t id : m_textures[i].info.nodes)
                {
                    m_texOfNode[id] = i;
                    if (m_reachVS.count(id)) m_textures[i].info.vertex = true;
                    if (m_reachPS.count(id)) m_textures[i].pixel = true;
                }
        }

        // The texture channel an active Opacity Mask reads directly (through reroutes). The shadow pass
        // clips by the material's ENGINE OPACITYMAP.r, never by this graph: the editor offers to mirror
        // this texture there.
        void FindOpacityMaskTexture()
        {
            if (!m_output || m_previewLink) return;
            const int pin = m_desc[m_output->id]->InputIndex("OpacityMask");
            bool active = false;
            for (const auto& [i, _] : m_psRoots) active = active || i == pin;
            const Link* l = active ? RootLink(pin) : nullptr;
            for (int step = 0; l && step < 64; ++step)
            {
                const Node& n = *m_nodes[l->fromNode];
                if (n.type == "Reroute" || n.type == "NamedRerouteDeclaration" || n.type == "NamedRerouteUsage")
                {
                    l = InLink(n.id, m_desc[n.id]->InputIndex("Input"));
                    continue;
                }
                const auto t = m_texOfNode.find(n.id);
                if (t != m_texOfNode.end() && (l->fromPin == "R" || l->fromPin == "G" || l->fromPin == "B" || l->fromPin == "A"))
                {
                    m_res.opacityMaskTexture = static_cast<int>(t->second);
                    m_res.opacityMaskChannel = l->fromPin;
                }
                return;
            }
        }

        // ---- Step 4: the b8 cbuffer ----
        static int PackedBytes(const std::vector<ParamInfo>& ps, std::vector<std::uint32_t>* offsets)
        {
            std::uint32_t off = 0;
            for (const ParamInfo& p : ps)
            {
                if (p.components == 4 && off % 16u) off = (off + 15u) & ~15u;   // a float4 never straddles
                if (offsets) offsets->push_back(off);
                off += 4u * static_cast<std::uint32_t>(p.components);
            }
            return static_cast<int>((off + 15u) & ~15u);
        }

        void LayoutParams()
        {
            std::vector<ParamInfo> reserved, user;
            if (m_opt.shadingModel == ShadingModel::TwoSidedFoliage)
            {
                // The material packer finds these three by their @engine roles, so their place is free; they stay
                // first so existing graphs' cbuffer layouts do not move.
                auto add = [&](const char* name, const char* role, std::initializer_list<float> def,
                               std::initializer_list<float> mx, const char* label) {
                    ParamInfo p;
                    p.hlslName = name;
                    p.engineRole = role;
                    p.label = label;
                    p.components = 4;
                    int i = 0;
                    for (float v : def) p.def[i++] = v;
                    i = 0;
                    for (float v : mx) p.max[i++] = v;
                    for (float& v : p.min) v = 0.0f;
                    p.hasRange = true;
                    p.group = "Foliage Transmission";
                    reserved.push_back(p);
                };
                add("FoliageTransmissionColor", ShaderReflect::kEngineRoleFoliageTint, { 0.18f, 0.42f, 0.12f, 1.0f }, { 1, 1, 1, 1 }, "Transmission Color");
                add("FoliageTransmissionWrap", ShaderReflect::kEngineRoleFoliageWrap, { 0.5f, 0, 0, 0 }, { 1, 1, 1, 1 }, "Wrap|(unused)|(unused)|(unused)");
                add("FoliageTransmissionPower", ShaderReflect::kEngineRoleFoliageStrength, { 1.0f, 0, 0, 0 }, { 4, 1, 1, 1 }, "Transmission Strength|(unused)|(unused)|(unused)");
            }
            // One entry per parameter: a reached node stands for its name's first node.
            std::set<std::uint32_t> owners;
            for (const auto& [id, d] : m_desc)
                if ((d->flags & kNodeParameter) && !(d->flags & kNodeTexture) && Reached(id))
                {
                    const auto o = m_paramOwner.find(id);
                    owners.insert(o != m_paramOwner.end() ? o->second : id);
                }
            for (const std::uint32_t id : owners)
            {
                const Node& n = *m_nodes[id];
                const bool vec = n.type == "VectorParameter";
                ParamInfo p;
                p.name       = n.PropString("name");
                p.label      = p.name;
                p.hlslName   = "P_" + Ident(p.name);
                p.components = vec ? 4 : 1;
                p.group      = n.PropString("group");
                p.sort       = n.PropNumber("sort", 0.0);
                p.node       = Public(id);   // a function's parameter: its call
                if (vec) { p.def[3] = 1.0f; }
                if (const Value* v = n.Prop("default"))
                {
                    if (!v->IsNumeric()) Warn(id, "", "bad_value", "props.default is not a number");
                    else for (int i = 0; i < p.components; ++i) p.def[i] = static_cast<float>(v->Components() == 1 ? v->At(0) : v->At(i));
                }
                const Value* mn = n.Prop("min");
                const Value* mx = n.Prop("max");
                if ((mn && mn->IsNumeric()) || (mx && mx->IsNumeric()))
                {
                    p.hasRange = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (mn && mn->IsNumeric()) p.min[i] = static_cast<float>(mn->Components() == 1 ? mn->At(0) : mn->At(i));
                        if (mx && mx->IsNumeric()) p.max[i] = static_cast<float>(mx->Components() == 1 ? mx->At(0) : mx->At(i));
                    }
                }
                user.push_back(p);
            }
            // Groups stay contiguous (the inspector draws a header per group run, in
            // cbuffer order): "" first, then by name; inside a group float4s first,
            // then scalars, each by (sort, name).
            auto grouped = [](const ParamInfo& a, const ParamInfo& b) {
                if (a.group != b.group) return a.group < b.group;
                if (a.components != b.components) return a.components > b.components;
                if (a.sort != b.sort) return a.sort < b.sort;
                return a.hlslName < b.hlslName;
            };
            std::sort(user.begin(), user.end(), grouped);
            std::vector<ParamInfo> all = reserved;
            all.insert(all.end(), user.begin(), user.end());
            if (PackedBytes(all, nullptr) > kMaxCBufferBytes)
            {
                // Tighter packing (every float4 before every scalar) when the grouped
                // one pads past the limit; a group may then show twice.
                std::stable_sort(user.begin(), user.end(), [](const ParamInfo& a, const ParamInfo& b) { return a.components > b.components; });
                all = reserved;
                all.insert(all.end(), user.begin(), user.end());
            }
            std::vector<std::uint32_t> offsets;
            const int bytes = PackedBytes(all, &offsets);
            for (std::size_t i = 0; i < all.size(); ++i) all[i].offset = offsets[i];
            m_res.params = std::move(all);
            m_res.stats.params = static_cast<int>(m_res.params.size());
            m_res.stats.cbufferBytes = bytes;
            if (m_res.stats.params > kMaxParams)
                Error(0, "", "capacity_params", std::to_string(m_res.stats.params) + " parameters; a material mirrors at most " +
                      std::to_string(kMaxParams));
            if (bytes > kMaxCBufferBytes)
                Error(0, "", "capacity_bytes", "the parameters pack into " + std::to_string(bytes) + " bytes; a material's b8 slot holds " +
                      std::to_string(kMaxCBufferBytes));
        }

        // ---- Step 5: code ----
        std::string Marker(std::uint32_t id) const { return " // @n" + std::to_string(Public(id)); }

        void Line(StageGen& g, int n, const std::string& name, const std::string& expr, std::uint32_t id)
        {
            g.lines.push_back("    " + TypeName(n) + " " + name + " = " + expr + ";" + Marker(id));
        }

        // Post-order from one Material Output input: every node after its inputs.
        void Evaluate(StageGen& g, std::uint32_t outputId, int input)
        {
            (void)outputId;
            const Link* root = RootLink(input);
            if (!root || g.done.count(root->fromNode)) return;
            std::vector<std::pair<std::uint32_t, std::size_t>> stack{ { root->fromNode, 0 } };
            std::unordered_set<std::uint32_t> onStack{ root->fromNode };
            while (!stack.empty())
            {
                auto& [id, next] = stack.back();
                const NodeDesc* d = m_desc[id];
                if (next < d->inputs.size())
                {
                    const Link* l = InLink(id, static_cast<int>(next++));
                    if (l && !g.done.count(l->fromNode) && !onStack.count(l->fromNode))
                    {
                        onStack.insert(l->fromNode);
                        stack.push_back({ l->fromNode, 0 });
                    }
                    continue;
                }
                const std::uint32_t node = id;
                stack.pop_back();
                onStack.erase(node);
                Emit(g, node);
                g.done.insert(node);
            }
        }

        // The value of node.pin in this stage (derived pins get their own SSA line).
        Val PinValue(StageGen& g, std::uint32_t node, const std::string& pin)
        {
            if (g.failed.count(node)) return Failed();
            const auto key = std::make_pair(node, pin);
            if (const auto it = g.pins.find(key); it != g.pins.end()) return it->second;
            const auto bit = g.base.find(node);
            if (bit == g.base.end()) return Failed();
            const Val& base = bit->second;
            const std::string name = "n" + std::to_string(node) + "_" + pin;
            const NodeDesc* d = m_desc[node];
            Val v;
            if (std::string(d->type) == "StandardSurface")
            {
                static const std::map<std::string, std::pair<const char*, int>> kFields = {
                    { "BaseColor", { "baseColor", 3 } }, { "Opacity", { "opacity", 1 } },
                    { "Normal", { "normalWS", 3 } }, { "Roughness", { "roughness", 1 } },
                    { "Metallic", { "metallic", 1 } }, { "AmbientOcclusion", { "ao", 1 } },
                    { "Specular", { "reflectance", 1 } }, { "EmissiveColor", { "emissive", 3 } } };
                const auto& f = kFields.at(pin);
                Line(g, f.second, name, base.code + "." + f.first, node);
                v = Code(name, f.second);
            }
            else if (IsChannelPin(pin))
            {
                const Val c = Channels(base, Lower(pin));
                if (c.code == base.code) v = base;   // RGBA of a float4
                else { Line(g, c.n, name, c.code, node); v = Code(name, c.n); }
            }
            else v = base;   // "Out"
            g.pins[key] = v;
            return v;
        }

        // An input's value: its link, its stored value, the stage built-in, or the default.
        Val Input(StageGen& g, const Node& n, const char* pinName)
        {
            const NodeDesc& d = *m_desc[n.id];
            const int idx = d.InputIndex(pinName);
            if (idx < 0)
            {
                Error(n.id, pinName, "internal", std::string("the code generator reads an input '") + pinName + "' the node does not have");
                return Failed();
            }
            const PinDesc& pin = d.inputs[static_cast<std::size_t>(idx)];
            if (const Link* l = InLink(n.id, idx)) return Fit(n, pin, PinValue(g, l->fromNode, l->fromPin));
            if (const auto it = n.inputs.find(pin.name); it != n.inputs.end())
            {
                const Value& v = it->second;
                if (v.IsNumeric() && v.Components() >= 1 && v.Components() <= 4)
                {
                    double x[4] = { 0, 0, 0, 0 };
                    for (int i = 0; i < v.Components(); ++i) x[i] = v.At(i);
                    return Fit(n, pin, Literal(x, v.Components()));
                }
                Warn(n.id, pin.name, "bad_value", "the value of input '" + std::string(pin.name) +
                     "' is not 1-4 numbers; using the default");
            }
            if (pin.defCount == 0)
            {
                const char* code = g.stage == Stage::Pixel ? pin.implicitPS : pin.implicitVS;
                if (code) return Code(code, pin.components);
            }
            double x[4] = { pin.def[0], pin.def[1], pin.def[2], pin.def[3] };
            return Fit(n, pin, Literal(x, pin.defCount));
        }

        // A fixed-size pin takes a scalar (broadcast) or a larger value (truncated);
        // a smaller vector is a type error, as in Unreal.
        Val Fit(const Node& n, const PinDesc& pin, const Val& v)
        {
            if (!v.ok || pin.components == 0 || v.n == pin.components) return v;
            if (v.n < pin.components && v.n != 1)
            {
                Error(n.id, pin.name, "type_mismatch", "input '" + std::string(pin.label) + "' of " + Label(n.id) + " wants " +
                      TypeName(pin.components) + ", got " + TypeName(v.n) + " (only a scalar widens)");
                return Failed();
            }
            return CastVal(v, pin.components);
        }

        // The common size of arithmetic operands: each is a scalar or that size.
        int Unify(const Node& n, std::initializer_list<std::pair<const char*, const Val*>> ops)
        {
            int size = 1;
            for (const auto& [_, v] : ops)
            {
                if (!v->ok) return 0;
                size = std::max(size, v->n);
            }
            for (const auto& [name, v] : ops)
                if (v->n != 1 && v->n != size)
                {
                    std::string list;
                    for (const auto& [nm, x] : ops) list += (list.empty() ? "" : ", ") + std::string(nm) + " is " + TypeName(x->n);
                    Error(n.id, name, "type_mismatch", Label(n.id) + ": " + list + " (operands must match or be scalars)");
                    return 0;
                }
            return size;
        }

        std::string SampleExpr(StageGen& g, const Node& n, const Val& uv)
        {
            const TexSlot& t = m_textures[m_texOfNode.at(n.id)];
            const SamplerKind* sk = FindSampler(n.PropString("sampler", "Color"));
            if (!sk) sk = &kSamplers[0];
            double fb[4] = { sk->fallback[0], sk->fallback[1], sk->fallback[2], sk->fallback[3] };
            const std::string fallback = Literal(fb, 4).code;
            const std::string slot = std::to_string(t.info.slot) + "u";
            std::string s = g.stage == Stage::Pixel
                ? "MA_Tex(" + t.info.hlslName + ", g_LinearWrap, " + Cast(uv, 2) + ", p.materialIndex, " + slot + ", " + fallback + ")"
                : "MA_TexLevel(" + t.info.hlslName + ", g_LinearWrap, " + Cast(uv, 2) + ", p.materialIndex, " + slot + ", " +
                  fallback + ", 0.0f)";
            if (std::string(sk->name) == "Normal") { m_helpers.insert("UnpackNormal"); s = "MG_UnpackNormal(" + s + ")"; }
            else if (std::string(sk->name) == "Grayscale") s += ".rrra";
            return s;
        }

        void Emit(StageGen& g, std::uint32_t id)
        {
            const Node& n = *m_nodes[id];
            const NodeDesc& d = *m_desc[id];
            const std::string t = d.type;
            const bool pixel = g.stage == Stage::Pixel;
            if ((d.flags & kNodePixelOnly) && !pixel)
            {
                Error(id, "", "vertex_illegal", Label(id) + " runs in the pixel stage only, but World Position Offset reaches it");
                g.failed.insert(id);
                return;
            }
            const std::string out = "n" + std::to_string(id) + "_";
            Val v;   // the base value; its SSA name is out + basePin
            std::string basePin = "Out";
            auto in = [&](const char* pin) { return Input(g, n, pin); };
            auto unary = [&](const char* fn) {
                const Val a = in("Input");
                return a.ok ? Code(std::string(fn) + "(" + a.code + ")", a.n) : Failed();
            };
            auto binary = [&](const char* op) {
                const Val a = in("A"), b = in("B");
                const int s = Unify(n, { { "A", &a }, { "B", &b } });
                return s ? Code("(" + Cast(a, s) + " " + op + " " + Cast(b, s) + ")", s) : Failed();
            };
            auto builtin = [&](const char* ps, const char* vs, int comps) { return Code(pixel ? ps : vs, comps); };

            if (t == "Constant")
            {
                const double x = n.PropNumber("value", 0.0);
                v = Literal(&x, 1);
            }
            else if (t == "Constant2Vector" || t == "Constant3Vector" || t == "Constant4Vector")
            {
                const int k = t[8] - '0';
                double x[4] = { 0, 0, 0, 0 };
                if (const Value* pv = n.Prop("value")) for (int i = 0; i < k; ++i) x[i] = pv->At(i);
                v = Literal(x, k);
            }
            else if (t == "ScalarParameter") v = Code("P_" + Ident(n.PropString("name")), 1);
            else if (t == "VectorParameter") { v = Code("P_" + Ident(n.PropString("name")), 4); basePin = "RGBA"; }
            else if (t == "TextureSampleParameter2D" || t == "TextureSample")
            {
                const Val uv = in("UVs");
                v = uv.ok ? Code(SampleExpr(g, n, uv), 4) : Failed();
                basePin = "RGBA";
            }
            else if (t == "TextureCoordinate")
            {
                int index = static_cast<int>(n.PropNumber("index", 0.0));
                if (index < 0 || index > 3) { Warn(id, "", "bad_value", "props.index must be 0..3; clamped"); index = std::clamp(index, 0, 3); }
                const double u = n.PropNumber("uTiling", 1.0), w = n.PropNumber("vTiling", 1.0);
                const std::string uvn = "p.uv" + std::to_string(index);
                if (u == 1.0 && w == 1.0) v = Code(uvn, 2);
                else { const double x[2] = { u, w }; v = Code("(" + uvn + " * " + Literal(x, 2).code + ")", 2); }
            }
            else if (t == "Panner")
            {
                const Val c = in("Coordinate"), tm = in("Time"), sp = in("Speed");
                v = (c.ok && tm.ok && sp.ok) ? Code("(" + c.code + " + " + tm.code + " * " + sp.code + ")", 2) : Failed();
            }
            else if (t == "BumpOffset")
            {
                const Val c = in("Coordinate"), h = in("Height"), r = in("HeightRatio");
                const double ref = n.PropNumber("referencePlane", 0.5);
                v = (c.ok && h.ok && r.ok)
                    ? Code("(" + c.code + " + mul(p.tangentToWorld, p.viewDirWS).xy * (" + h.code + " - " + HlslFloat(ref) + ") * " + r.code + ")", 2)
                    : Failed();
            }
            else if (t == "Add") v = binary("+");
            else if (t == "Subtract") v = binary("-");
            else if (t == "Multiply") v = binary("*");
            else if (t == "Divide") v = binary("/");
            else if (t == "OneMinus")
            {
                const Val a = in("Input");
                v = a.ok ? Code("(1.0f - " + a.code + ")", a.n) : Failed();
            }
            else if (t == "Abs") v = unary("abs");
            else if (t == "Ceil") v = unary("ceil");
            else if (t == "Frac") v = unary("frac");
            else if (t == "Saturate") v = unary("saturate");
            else if (t == "Clamp")
            {
                const Val a = in("Input"), lo = in("Min"), hi = in("Max");
                const int s = Unify(n, { { "Input", &a }, { "Min", &lo }, { "Max", &hi } });
                v = s ? Code("clamp(" + Cast(a, s) + ", " + Cast(lo, s) + ", " + Cast(hi, s) + ")", s) : Failed();
            }
            else if (t == "Min" || t == "Max")
            {
                const Val a = in("A"), b = in("B");
                const int s = Unify(n, { { "A", &a }, { "B", &b } });
                v = s ? Code(Lower(t) + "(" + Cast(a, s) + ", " + Cast(b, s) + ")", s) : Failed();
            }
            else if (t == "Power")
            {
                const Val a = in("Base"), b = in("Exp");
                const int s = Unify(n, { { "Base", &a }, { "Exp", &b } });
                v = s ? Code("pow(max(abs(" + Cast(a, s) + "), 1e-8f), " + Cast(b, s) + ")", s) : Failed();
            }
            else if (t == "LinearInterpolate")
            {
                const Val a = in("A"), b = in("B"), al = in("Alpha");
                const int s = Unify(n, { { "A", &a }, { "B", &b }, { "Alpha", &al } });
                v = s ? Code("lerp(" + Cast(a, s) + ", " + Cast(b, s) + ", " + Cast(al, s) + ")", s) : Failed();
            }
            else if (t == "DotProduct")
            {
                const Val a = in("A"), b = in("B");
                const int s = Unify(n, { { "A", &a }, { "B", &b } });
                if (!s) v = Failed();
                else if (s == 1) v = Code("(" + a.code + " * " + b.code + ")", 1);
                else v = Code("dot(" + Cast(a, s) + ", " + Cast(b, s) + ")", 1);
            }
            else if (t == "CrossProduct")
            {
                const Val a = in("A"), b = in("B");
                v = (a.ok && b.ok) ? Code("cross(" + a.code + ", " + b.code + ")", 3) : Failed();
            }
            else if (t == "Normalize")
            {
                const Val a = in("Input");
                if (!a.ok) v = Failed();
                else if (a.n == 1) v = Code("sign(" + a.code + ")", 1);
                else v = Code("(" + Wrap(a.code) + " / max(length(" + a.code + "), 1e-5f))", a.n);
            }
            else if (t == "Distance")
            {
                const Val a = in("A"), b = in("B");
                const int s = Unify(n, { { "A", &a }, { "B", &b } });
                if (!s) v = Failed();
                else if (s == 1) v = Code("abs(" + a.code + " - " + b.code + ")", 1);
                else v = Code("distance(" + Cast(a, s) + ", " + Cast(b, s) + ")", 1);
            }
            else if (t == "DDX") v = unary("ddx");
            else if (t == "DDY") v = unary("ddy");
            else if (t == "ComponentMask")
            {
                const Val a = in("Input");
                std::string mask;
                for (const char* c : { "r", "g", "b", "a" })
                    if (n.PropBool(c, false)) mask += c;
                if (mask.empty())
                {
                    Warn(id, "", "mask_empty", "no channel selected (props r, g, b, a); using R");
                    mask = "r";
                }
                if (a.ok)
                {
                    const int last = static_cast<int>(std::string("rgba").find(mask.back()));
                    if (last >= a.n)
                        Warn(id, "", "mask_out_of_range", "selects " + std::string(1, static_cast<char>(std::toupper(mask.back()))) +
                             " of a " + TypeName(a.n) + ": it reads 0");
                }
                v = a.ok ? Channels(a, mask) : Failed();
            }
            else if (t == "AppendVector")
            {
                const Val a = in("A"), b = in("B");
                if (!a.ok || !b.ok) v = Failed();
                else if (a.n + b.n > 4)
                {
                    Error(id, "", "type_mismatch", "Append of " + TypeName(a.n) + " and " + TypeName(b.n) + " is " +
                          std::to_string(a.n + b.n) + " components (at most 4)");
                    v = Failed();
                }
                else v = Code(TypeName(a.n + b.n) + "(" + a.code + ", " + b.code + ")", a.n + b.n);
            }
            else if (t == "WorldPosition") v = builtin("p.worldPos", "p.worldPos", 3);
            else if (t == "ObjectPositionWS") v = builtin("p.objectOrigin", "p.worldMatrix[3].xyz", 3);
            else if (t == "CameraPositionWS") v = builtin("gCameraPosWS.xyz", "p.cameraPos", 3);
            else if (t == "CameraVectorWS") v = builtin("p.viewDirWS", "MA_SafeNormalize(p.cameraPos - p.worldPos)", 3);
            else if (t == "VertexNormalWS") v = builtin("p.geoNormalWS", "p.normalWS", 3);
            else if (t == "VertexColor") { v = builtin("p.vertexColor", "float4(p.vertexColor, 1.0f)", 4); basePin = "RGBA"; }
            else if (t == "TwoSidedSign") v = Code("(p.isFrontFace ? 1.0f : -1.0f)", 1);
            else if (t == "Time")
            {
                const double period = n.PropNumber("period", 0.0);
                v = period > 0.0 ? Code("fmod(p.time, " + HlslFloat(period) + ")", 1) : Code("p.time", 1);
            }
            else if (t == "Transform") v = TransformExpr(g, n);
            else if (t == "Fresnel")
            {
                const Val e = in("ExponentIn"), b = in("BaseReflectFractionIn"), nn = in("Normal");
                const std::string view = pixel ? "p.viewDirWS" : "MA_SafeNormalize(p.cameraPos - p.worldPos)";
                v = (e.ok && b.ok && nn.ok)
                    ? Code("(" + b.code + " + (1.0f - " + b.code + ") * pow(max(1.0f - saturate(dot(MA_SafeNormalize(" + nn.code +
                           "), " + view + ")), 1e-8f), " + e.code + "))", 1)
                    : Failed();
            }
            else if (t == "SphereMask")
            {
                const Val a = in("A"), b = in("B"), r = in("Radius"), h = in("Hardness");
                const int s = Unify(n, { { "A", &a }, { "B", &b } });
                if (!s || !r.ok || !h.ok) v = Failed();
                else
                {
                    const std::string dist = s == 1 ? "abs(" + a.code + " - " + b.code + ")"
                                                    : "length(" + Cast(a, s) + " - " + Cast(b, s) + ")";
                    v = Code("saturate((1.0f - " + dist + " / max(" + r.code + ", 1e-6f)) / max(1.0f - " + h.code + ", 1e-5f))", 1);
                }
            }
            else if (t == "Desaturation")
            {
                const Val a = in("Input"), f = in("Fraction");
                double lum[3] = { 0.3, 0.59, 0.11 };
                if (const Value* lv = n.Prop("luminanceFactors"); lv && lv->Components() == 3)
                    for (int i = 0; i < 3; ++i) lum[i] = lv->At(i);
                v = (a.ok && f.ok) ? Code("lerp(" + a.code + ", dot(" + a.code + ", " + Literal(lum, 3).code + ").xxx, " + f.code + ")", 3)
                                   : Failed();
            }
            else if (t == "RotateAboutAxis")
            {
                const Val ax = in("NormalizedRotationAxis"), an = in("RotationAngle"), pv = in("PivotPoint"), po = in("Position");
                m_helpers.insert("RotateAboutAxis");
                v = (ax.ok && an.ok && pv.ok && po.ok)
                    ? Code("MG_RotateAboutAxis(" + ax.code + ", " + an.code + ", " + pv.code + ", " + po.code + ")", 3)
                    : Failed();
            }
            else if (t == "Reroute" || t == "NamedRerouteDeclaration" || t == "NamedRerouteUsage")
            {
                const Val a = in("Input");   // a usage's: its declaration (the compiler's link)
                v = a.ok ? Code(a.code, a.n) : Failed();
            }
            else if (t == "Floor") v = unary("floor");
            else if (t == "Sqrt")
            {
                const Val a = in("Input");
                v = a.ok ? Code("sqrt(max(" + a.code + ", 0.0f))", a.n) : Failed();
            }
            else if (t == "Length")
            {
                const Val a = in("Input");
                v = !a.ok ? Failed() : a.n == 1 ? Code("abs(" + a.code + ")", 1) : Code("length(" + a.code + ")", 1);
            }
            else if (t == "Sine" || t == "Cosine")
            {
                const Val a = in("Input");
                const double period = n.PropNumber("period", 1.0);
                const std::string arg = period > 0.0 ? Wrap(a.code) + " * " + HlslFloat(6.283185307179586 / period) : a.code;
                v = a.ok ? Code(std::string(t == "Sine" ? "sin" : "cos") + "(" + arg + ")", a.n) : Failed();
            }
            else if (t == "If")
            {
                const Val a = in("A"), b = in("B"), gt = in("AGreaterThanB"), lt = in("ALessThanB");
                // A == B counts only when it is linked or given a value (Unreal's two-way form otherwise).
                const bool hasEq = InLink(id, d.InputIndex("AEqualsB")) != nullptr || n.inputs.count("AEqualsB") != 0;
                const Val eq = hasEq ? in("AEqualsB") : Val{};
                const int s = hasEq ? Unify(n, { { "AGreaterThanB", &gt }, { "AEqualsB", &eq }, { "ALessThanB", &lt } })
                                    : Unify(n, { { "AGreaterThanB", &gt }, { "ALessThanB", &lt } });
                if (!a.ok || !b.ok || !s) v = Failed();
                else
                {
                    std::string c = "(" + a.code + " >= " + b.code + " ? " + Cast(gt, s) + " : " + Cast(lt, s) + ")";
                    if (hasEq)
                        c = "(abs(" + a.code + " - " + b.code + ") > " + HlslFloat(n.PropNumber("threshold", 0.00001)) +
                            " ? " + c + " : " + Cast(eq, s) + ")";
                    v = Code(c, s);
                }
            }
            else if (t == "Rotator")
            {
                const Val c = in("Coordinate"), tm = in("Time");
                const double center[2] = { n.PropNumber("centerX", 0.5), n.PropNumber("centerY", 0.5) };
                m_helpers.insert("Rotator");
                v = (c.ok && tm.ok) ? Code("MG_Rotator(" + c.code + ", " + Wrap(tm.code) + " * " + HlslFloat(n.PropNumber("speed", 0.25)) +
                                           ", " + Literal(center, 2).code + ")", 2)
                                    : Failed();
            }
            else if (t == "BreakOutFloat4Components")
            {
                const Val a = in("Float4");
                v = a.ok ? Code(a.code, 4) : Failed();
                basePin = "RGBA";   // R, G, B, A are its channels
            }
            else if (t == "PixelNormalWS") v = Code("mg_PixelNormalWS", 3);
            else if (t == "PixelDepth") { m_helpers.insert("ViewClip"); v = Code("MG_PixelDepth(p.worldPos)", 1); }
            else if (t == "ScreenPosition")
            {
                m_helpers.insert("ViewClip");
                MultiOut(g, id, { { "ViewportUV", Code("MG_ViewportUV(p.worldPos)", 2) },
                                  { "PixelPosition", Code("p.svPosition.xy", 2) } });
                return;
            }
            else if (t == "SceneDepth")
            {
                WaterOnly(id, "1e6");
                m_helpers.insert("SceneDepth");
                MultiOut(g, id, { { "Depth", Code("MG_SceneDepth(p)", 1) }, { "WaterDepth", Code("WaterSceneDepthBelow(p)", 1) } });
                return;
            }
            else if (t == "SceneColor")
            {
                WaterOnly(id, "Fallback");
                const Val uv = in("UVs"), fb = in("Fallback");
                v = (uv.ok && fb.ok) ? Code("WaterSceneColor(p, " + uv.code + ", " + fb.code + ")", 3) : Failed();
            }
            else if (t == "WaterFresnel")
            {
                WaterOnly(id, "F0");
                const Val f0 = in("F0"), nn = in("Normal");
                m_helpers.insert("WaterFresnel");
                v = (f0.ok && nn.ok) ? Code("MG_WaterFresnel(p, " + nn.code + ", " + f0.code + ")", 1) : Failed();
            }
            else if (t == "SceneTexture")
            {
                Error(id, "", "scene_texture_unsupported", Label(id) + ": a surface material cannot read the scene it is drawn "
                      "into (" + n.PropString("sceneTexture", "SceneDepth") + "); in a Water material, Scene Depth and Scene "
                      "Color are the counterparts");
                v = Failed();
            }
            else if (t == "SceneTexelSize")
            {
                if (m_opt.shadingModel != ShadingModel::Water)
                {
                    Error(id, "", "scene_texel_size_unavailable", Label(id) + ": only the Water pass knows its render target's "
                          "size (this material is " + ShadingModelName(m_opt.shadingModel) + ")");
                    v = Failed();
                }
                else { m_helpers.insert("SceneTexelSize"); v = Code("MG_SceneTexelSize()", 2); }
            }
            else if (t == "SkyAtmosphereLightDirection")
            {
                if (!IsTranslucent(m_opt.blendMode) && m_opt.shadingModel != ShadingModel::Water)
                {
                    Error(id, "", "sky_light_unavailable", Label(id) + ": an opaque material is drawn into the GBuffer before "
                          "any light, with no sun direction - use a Vector Parameter (or a translucent blend mode)");
                    v = Failed();
                }
                else { m_helpers.insert("SunDirection"); v = Code("MG_SunDirection()", 3); }
            }
            else if (t == "Custom") v = CustomExpr(g, n, d);
            // Static Switch and Material Function Call nodes never get here (the Flattener resolves them);
            // a Static Bool read as a number and an expanded function's Function Inputs do.
            else if (t == "StaticBool")
            {
                const double x = n.PropBool("value", true) ? 1.0 : 0.0;
                v = Literal(&x, 1);
            }
            else if (t == "FunctionInput")
            {
                const Val a = in("Preview");   // the call's value, bound here - else the function's default
                v = a.ok ? Code(a.code, a.n) : Failed();
            }
            else if (t == "StandardSurface")
            {
                const std::string sname = out + "S";
                g.lines.push_back("    SurfaceData " + sname + " = s;" + Marker(id));
                g.lines.push_back("    MS_StandardSurface(p, " + sname + ");" + Marker(id));
                g.base[id] = Code(sname, 0);
                return;
            }
            else
            {
                Error(id, "", "unknown_node_type", "node type '" + t + "' has no code generator");
                v = Failed();
            }

            if (!v.ok) { g.failed.insert(id); return; }
            const std::string name = out + basePin;
            Line(g, v.n, name, v.code, id);
            const Val named = Code(name, v.n);
            g.base[id] = named;
            g.pins[{ id, basePin }] = named;
        }

        // A node with several named outputs (not channels of one value): one SSA line each.
        void MultiOut(StageGen& g, std::uint32_t id, std::initializer_list<std::pair<const char*, Val>> pins)
        {
            const std::string out = "n" + std::to_string(id) + "_";
            bool first = true;
            for (const auto& [pin, val] : pins)
            {
                Line(g, val.n, out + pin, val.code, id);
                const Val named = Code(out + pin, val.n);
                g.pins[{ id, pin }] = named;
                if (first) g.base[id] = named;
                first = false;
            }
        }

        // water_common.hlsli: the real function in the Water shading model, a stub elsewhere.
        void WaterOnly(std::uint32_t id, const char* stub)
        {
            if (m_opt.shadingModel == ShadingModel::Water) return;
            Warn(id, "", "water_only", Label(id) + " reads the scene only in the Water shading model; in " +
                 std::string(ShadingModelName(m_opt.shadingModel)) + " it is " + stub + " (water_common.hlsli's stub)");
        }

        static bool ContainsWord(const std::string& text, const char* word)
        {
            const std::size_t len = std::char_traits<char>::length(word);
            auto isId = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
            for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
                if ((at == 0 || !isId(text[at - 1])) && (at + len >= text.size() || !isId(text[at + len]))) return true;
            return false;
        }

        // The Custom node: its body becomes MG_Custom_n<id>_<PS|VS>(p, <inputs>), each line marked with
        // the node's "// @n<id>", so a DXC error in it lands on this node.
        Val CustomExpr(StageGen& g, const Node& n, const NodeDesc& d)
        {
            bool ok = true;
            std::vector<CustomInput> declared;
            std::string why;
            if (!ParseCustomInputs(n.PropString("inputs", kCustomDefaultInputs), declared, &why))
            {
                Error(n.id, "", "custom_bad_inputs", Label(n.id) + ": props.inputs: " + why);
                ok = false;
            }
            const std::string outType = n.PropString("outputType", kCustomDefaultOutput);
            const int outN = CustomTypeComponents(outType);
            if (!outN)
            {
                Error(n.id, "", "custom_bad_output", Label(n.id) + ": props.outputType '" + outType +
                      "' is not float, float2, float3 or float4");
                ok = false;
            }
            std::vector<std::string> lines;
            {
                const std::string code = n.PropString("code", kCustomDefaultCode);
                std::size_t b = 0;
                while (b <= code.size())
                {
                    std::size_t e = code.find('\n', b);
                    if (e == std::string::npos) e = code.size();
                    std::string line = code.substr(b, e - b);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    lines.push_back(std::move(line));
                    b = e + 1;
                }
                while (!lines.empty() && lines.back().find_first_not_of(" \t") == std::string::npos) lines.pop_back();
            }
            if (lines.empty())
            {
                Error(n.id, "", "custom_empty", Label(n.id) + ": props.code is empty");
                ok = false;
            }
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                const std::size_t at = lines[i].find_first_not_of(" \t");
                if (at != std::string::npos && lines[i][at] == '#')
                {
                    Error(n.id, "", "custom_directive", Label(n.id) + ": line " + std::to_string(i + 1) + " is a preprocessor line (" +
                          SafeText(lines[i].substr(at, 40)) + "): the engine headers are included already");
                    ok = false;
                }
            }
            if (!ok) return Failed();

            std::string args = "p";
            for (const PinDesc& pin : d.inputs)
            {
                const Val a = Input(g, n, pin.name);
                if (!a.ok) return Failed();
                args += ", " + Cast(a, pin.components);
            }
            const bool pixel = g.stage == Stage::Pixel;
            const std::string fn = "MG_Custom_n" + std::to_string(n.id) + (pixel ? "_PS" : "_VS");
            if (m_customDone.insert(fn).second)
            {
                const std::string mark = Marker(n.id);
                std::string f = TypeName(outN) + " " + fn + "(" + (pixel ? "MaterialPixelParams" : "MaterialVertexParams") + " p";
                for (const PinDesc& pin : d.inputs) f += ", " + TypeName(pin.components) + " " + pin.name;
                f += ")" + mark + "\n{" + mark + "\n";
                const bool body = std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return ContainsWord(l, "return"); });
                if (body)
                    for (const std::string& l : lines) f += "    " + l + mark + "\n";
                else
                {
                    // One expression (Unreal's rule: no 'return' = return it).
                    f += "    return" + mark + "\n";
                    for (const std::string& l : lines) f += "        " + l + mark + "\n";
                    f += "    ;" + mark + "\n";
                }
                f += "}" + mark + "\n";
                m_customFns.push_back(std::move(f));
            }
            return Code(fn + "(" + args + ")", outN);
        }

        Val TransformExpr(StageGen& g, const Node& n)
        {
            const Val a = Input(g, n, "Input");
            if (!a.ok) return Failed();
            const std::string src = Lower(n.PropString("source", "Tangent"));
            const std::string dst = Lower(n.PropString("dest", "World"));
            auto known = [](const std::string& s) { return s == "tangent" || s == "world" || s == "local"; };
            if (!known(src) || !known(dst))
            {
                Error(n.id, "", "bad_value", "props.source / props.dest must be Tangent, World or Local");
                return Failed();
            }
            if (src == dst) return a;
            const bool pixel = g.stage == Stage::Pixel;
            if (!pixel && (src == "tangent" || dst == "tangent"))
            {
                Error(n.id, "", "vertex_illegal", Label(n.id) + ": tangent space exists in the pixel stage only");
                return Failed();
            }
            // To world first, then to the destination.
            std::string w;
            if (src == "world") w = a.code;
            else if (src == "tangent") w = "mul(" + a.code + ", p.tangentToWorld)";
            else w = pixel ? "mul(" + a.code + " * p.objectScale, p.objectAxes)" : "mul(" + a.code + ", (float3x3)p.worldMatrix)";
            if (dst == "world") return Code(w, 3);
            if (dst == "tangent") return Code("mul(p.tangentToWorld, " + w + ")", 3);
            if (pixel) return Code("(mul(p.objectAxes, " + w + ") / max(p.objectScale, 1e-8f))", 3);
            m_helpers.insert("WorldToLocal");
            return Code("MG_WorldToLocal(" + w + ", p.worldMatrix)", 3);
        }

        Val OutputValue(StageGen& g, int input)
        {
            const Link* l = RootLink(input);
            const PinDesc& pin = m_desc[m_output->id]->inputs[static_cast<std::size_t>(input)];
            return l ? Fit(*m_output, pin, PinValue(g, l->fromNode, l->fromPin)) : Failed();
        }

        // What the Normal pin makes of the surface normal: the value Pixel Normal WS reads.
        std::string NormalExpr(const Val& v) const
        {
            return m_output->PropBool("tangentSpaceNormal", true)
                ? "MS_ApplyNormalMap(MA_SafeNormalize(" + v.code + "), p.tangentToWorld)"
                : "MA_SafeNormalize(" + v.code + ")";
        }

        void EmitPixelNormal(StageGen& g)
        {
            std::string expr = "p.geoNormalWS";
            if (m_normalRoot >= 0)
            {
                const Val v = OutputValue(g, m_normalRoot);
                if (!v.ok) return;
                expr = NormalExpr(v);
            }
            g.lines.push_back("    float3 mg_PixelNormalWS = " + expr + ";" + Marker(m_output->id));
        }

        std::vector<std::string> PixelWrites(StageGen& g)
        {
            std::vector<std::string> w;
            const std::string mark = Marker(m_output->id);
            const NodeDesc& d = *m_desc[m_output->id];
            const bool tangentSpace = m_output->PropBool("tangentSpaceNormal", true);
            if (m_previewLink)
            {
                // The previewed value as emissive over black (a scalar is grey, a float2
                // is (x, y, 0), a float4 its RGB) - nothing else of the material.
                const Val v = PinValue(g, m_previewLink->fromNode, m_previewLink->fromPin);
                w.push_back("    s.baseColor = float3(0.0f, 0.0f, 0.0f);" + mark);
                if (v.ok) w.push_back("    s.emissive = " + Cast(v, 3) + ";" + Marker(m_previewLink->fromNode));
                w.push_back("    s.normalWS = p.geoNormalWS;" + mark);
                return w;
            }
            // Unlit shows emissive and, drawn deferred, a linked Base Color under it.
            const bool unlitBase = std::any_of(m_psRoots.begin(), m_psRoots.end(), [&](const std::pair<int, const Link*>& r) {
                return std::string_view(d.inputs[static_cast<std::size_t>(r.first)].name) == "BaseColor";
            });
            if (m_opt.shadingModel == ShadingModel::Unlit && !unlitBase)
                w.push_back("    s.baseColor = float3(0.0f, 0.0f, 0.0f);" + mark);
            bool normal = false;
            for (const auto& [i, _] : m_psRoots)
            {
                const std::string pin = d.inputs[static_cast<std::size_t>(i)].name;
                const Val v = OutputValue(g, i);
                if (!v.ok) continue;
                const std::string c = v.code;
                std::string line;
                if      (pin == "BaseColor")          line = "s.baseColor = saturate(" + c + ");";
                else if (pin == "Metallic")           line = "s.metallic = saturate(" + c + ");";
                else if (pin == "Specular")           line = "s.reflectance = saturate(" + c + ");";
                else if (pin == "Roughness")          line = "s.roughness = saturate(" + c + ");";
                else if (pin == "Anisotropy")         line = "s.anisotropy = clamp(" + c + ", -1.0f, 1.0f);";
                else if (pin == "EmissiveColor")      line = "s.emissive = " + c + ";";
                else if (pin == "Opacity" || pin == "OpacityMask") line = "s.opacity = saturate(" + c + ");";
                else if (pin == "Normal")
                {
                    normal = true;
                    line = m_usesPixelNormal ? std::string("s.normalWS = mg_PixelNormalWS;")   // computed once, above
                                             : "s.normalWS = " + NormalExpr(v) + ";";
                }
                else if (pin == "Tangent")
                    line = tangentSpace ? "s.anisotropyDirWS = MA_SafeNormalize(mul(" + c + ", p.tangentToWorld));"
                                        : "s.anisotropyDirWS = MA_SafeNormalize(" + c + ");";
                else if (pin == "SubsurfaceOpacity")  line = "s.subsurfaceOpacity = saturate(" + c + ");";
                else if (pin == "SubsurfaceMask")     line = "s.subsurfaceMask = saturate(" + c + ");";
                else if (pin == "ClearCoat")          line = "s.clearCoat = saturate(" + c + ");";
                else if (pin == "ClearCoatRoughness") line = "s.clearCoatRoughness = saturate(" + c + ");";
                else if (pin == "AmbientOcclusion")   line = "s.ao = saturate(" + c + ");";
                else if (pin == "DirectAdditive")     line = "s.directAdditive = saturate(" + c + ");";
                if (!line.empty()) w.push_back("    " + line + mark);
            }
            // MS_DefaultSurface seeds normalWS with world +Z; Unreal's unconnected
            // Normal is the surface's own (tangent-space 0,0,1).
            if (!normal) w.push_back("    s.normalWS = p.geoNormalWS;" + mark);
            if (m_opt.shadingModel == ShadingModel::TwoSidedFoliage)
                w.push_back("    s.baseColor += 1e-8f * (FoliageTransmissionColor.rgb + FoliageTransmissionColor.a + "
                            "FoliageTransmissionWrap.x + FoliageTransmissionPower.x); // keeps the foliage parameters in the reflection" + mark);
            // A texture only World Position Offset samples must still be in the PIXEL reflection: the space3
            // table is built from it.
            for (const TexSlot& t : m_textures)
                if (t.info.vertex && !t.pixel)
                    w.push_back("    s.baseColor += 1e-8f * " + t.info.hlslName +
                                ".SampleLevel(g_LinearWrap, float2(0.0f, 0.0f), 0.0f).r; // binds a vertex-only texture" + mark);
            return w;
        }

        void Assemble(const StageGen& ps, const std::vector<std::string>& writes, const StageGen& vs, const std::string& wpoReturn)
        {
            const bool vertex = !wpoReturn.empty();
            std::string o;
            o += "// GENERATED by MaterialGraph v" + std::to_string(kCompilerVersion) + " from " +
                 SafeText(m_opt.sourceLabel.empty() ? std::string("(memory)") : m_opt.sourceLabel) +
                 " (graphHash " + m_res.graphHash + "). Do not edit.\n";
            o += "// Shading model " + SafeText(ShadingModelName(m_opt.shadingModel)) + ", blend mode " +
                 BlendModeLabel(m_opt.blendMode) + ". Edit the graph in the Material Editor.\n";
            if (m_previewLink)
                o += "// PREVIEW of " + SafeText(Label(m_previewLink->fromNode)) + " ." + SafeText(m_previewLink->fromPin) +
                     " (Start Previewing Node) - not the material.\n";
            if (!m_res.switches.empty())
            {
                // The static-switch combination this file is (one shader per combination).
                o += "// Static switches:";
                for (std::size_t i = 0; i < m_res.switches.size(); ++i)
                    o += (i ? ", " : " ") + SafeText(m_res.switches[i].name) + "=" + (m_res.switches[i].value ? "1" : "0");
                o += ".\n";
            }
            o += "#define MATERIAL_HAS_SURFACE\n";
            // The shader names its domain; none = surface. Recorded and shown (MaterialShaderDecl::Domain),
            // not enforced.
            if (m_opt.shadingModel == ShadingModel::Water) o += "#define MATERIAL_DOMAIN_WATER\n";
            if (vertex) o += "#define " + std::string(kWpoToken) + "\n";
            o += "#include \"custom_material.hlsli\"\n";
            o += "#include \"material_authoring.hlsli\"\n";
            // A Toon graph has no ramp texture: tell the material packer to read none from t3, so all eight
            // slots stay usable.
            if (m_opt.shadingModel == ShadingModel::Toon) o += "//@engine explicit\n";

            if (!m_res.params.empty())
            {
                o += "\ncbuffer MaterialGraphParams : register(b8, space0)\n{\n";
                for (const ParamInfo& p : m_res.params)
                {
                    o += "    " + TypeName(p.components) + " " + p.hlslName + "; //@default";
                    for (int i = 0; i < p.components; ++i) o += " " + FormatNumber(p.def[i]);
                    if (p.hasRange)
                    {
                        o += " @min";
                        for (int i = 0; i < p.components; ++i) o += " " + FormatNumber(p.min[i]);
                        o += " @max";
                        for (int i = 0; i < p.components; ++i) o += " " + FormatNumber(p.max[i]);
                    }
                    o += " @label \"" + SafeText(p.label) + "\"";
                    if (!p.group.empty()) o += " @group \"" + SafeText(p.group) + "\"";
                    if (!p.engineRole.empty()) o += " @engine " + p.engineRole;
                    o += "\n";
                }
                o += "};\n";
            }
            if (!m_textures.empty())
            {
                o += "\n";
                for (const TexSlot& t : m_textures)
                    o += "Texture2D " + t.info.hlslName + " : register(t" + std::to_string(t.info.slot) + ", space3);" +
                         Marker(t.info.nodes.front()) + "\n";
            }
            if (!m_helpers.empty())
            {
                o += "\n";
                // Every helper must also compile where it is not reached: the vertex, shadow and other pass
                // variants all parse EvaluateMaterial.
                if (m_helpers.count("UnpackNormal"))
                    o += "float4 MG_UnpackNormal(float4 t) { float2 v = t.rg * 2.0f - 1.0f; "
                         "return float4(v, sqrt(saturate(1.0f - dot(v, v))), t.a); }\n";
                if (m_helpers.count("RotateAboutAxis"))
                    o += "float3 MG_RotateAboutAxis(float3 axis, float turns, float3 pivot, float3 pos) { "
                         "axis = MA_SafeNormalize(axis); float s, c; sincos(turns * 6.28318530718f, s, c); float3 v = pos - pivot; "
                         "return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0f - c) - v; }\n";
                if (m_helpers.count("WorldToLocal"))
                    o += "float3 MG_WorldToLocal(float3 v, float4x4 m) { float3 x = m[0].xyz, y = m[1].xyz, z = m[2].xyz; "
                         "return float3(dot(v, x) / max(dot(x, x), 1e-8f), dot(v, y) / max(dot(y, y), 1e-8f), "
                         "dot(v, z) / max(dot(z, z), 1e-8f)); }\n";
                if (m_helpers.count("Rotator"))
                    o += "float2 MG_Rotator(float2 uv, float angle, float2 c) { float s, k; sincos(angle, s, k); "
                         "float2 d = uv - c; return float2(k * d.x - s * d.y, s * d.x + k * d.y) + c; }\n";
                if (m_helpers.count("ViewClip"))
                    o += "float4 MG_ViewClip(float3 worldPos)\n{\n"
                         "#if defined(CUSTOM_VS_SHADOW) && CUSTOM_VS_SHADOW\n"
                         "    return float4(0.0f, 0.0f, 0.0f, 1.0f);   // the shadow variant has no main view\n"
                         "#else\n"
                         "    return mul(float4(worldPos, 1.0f), curViewProjNoJitter);\n"
                         "#endif\n}\n"
                         "float2 MG_ViewportUV(float3 worldPos) { float4 c = MG_ViewClip(worldPos); "
                         "float2 ndc = c.xy / max(c.w, 1e-6f); return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f); }\n"
                         "float MG_PixelDepth(float3 worldPos) { return MG_ViewClip(worldPos).w; }\n";
                if (m_helpers.count("SceneDepth"))
                    o += "float MG_SceneDepth(MaterialPixelParams p) { float3 w; "
                         "return WaterSceneWorldPosBehind(p, w) ? distance(w, gCameraPosWS.xyz) : 1e6f; }\n";
                if (m_helpers.count("WaterFresnel"))
                    o += "float MG_WaterFresnel(MaterialPixelParams p, float3 n, float f0) { SurfaceData t = (SurfaceData)0; "
                         "t.normalWS = n; return WaterFresnel(p, t, f0); }\n";
                if (m_helpers.count("SceneTexelSize"))
                    o += "float2 MG_SceneTexelSize()\n{\n"
                         "#if defined(MS_IS_WATER) && MS_IS_WATER && defined(MS_IS_FORWARD) && MS_IS_FORWARD\n"
                         "    return 1.0f / max(gWaterRTSize.xy, float2(1.0f, 1.0f));\n"
                         "#else\n"
                         "    return float2(0.0f, 0.0f);   // a variant without the Water constants\n"
                         "#endif\n}\n";
                if (m_helpers.count("SunDirection"))
                    o += "float3 MG_SunDirection()\n{\n"
                         "#if defined(MS_IS_FORWARD) && MS_IS_FORWARD\n"
                         "    return normalize(-lightDir);\n"
                         "#else\n"
                         "    return float3(0.0f, 1.0f, 0.0f);   // a variant without the light constants\n"
                         "#endif\n}\n";
            }
            if (!m_customFns.empty())
            {
                o += "\n";
                for (const std::string& f : m_customFns) o += f;
            }

            o += "\nvoid EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)\n{\n";
            for (const std::string& l : ps.lines) o += l + "\n";
            for (const std::string& l : writes) o += l + "\n";
            o += "}\n";
            if (vertex)
            {
                o += "\nfloat3 MaterialGetWorldPositionOffset(MaterialVertexParams p)\n{\n";
                for (const std::string& l : vs.lines) o += l + "\n";
                o += wpoReturn + "\n}\n";
            }

            m_res.hlsl = std::move(o);
            m_res.stats.usesVertex  = vertex;
            m_res.stats.pixelLines  = static_cast<int>(ps.lines.size());
            m_res.stats.vertexLines = static_cast<int>(vs.lines.size());
            m_res.stats.hlslBytes   = static_cast<int>(m_res.hlsl.size());
        }

        const Graph&          m_g;
        const CompileOptions& m_opt;
        CompileResult&        m_res;
        const Expansion*      m_x = nullptr;   // set when the graph is an expansion (functions, static switches)

        std::unordered_map<std::uint32_t, const Node*>     m_nodes;
        std::unordered_map<std::uint32_t, const NodeDesc*> m_desc;
        std::map<std::pair<std::uint32_t, int>, const Link*> m_inLinks;   // (toNode, input index)
        const Node*                                         m_output = nullptr;
        std::vector<std::pair<int, const Link*>>            m_psRoots;     // output input index, link
        int                                                 m_vsRoot = -1;
        int                                                 m_wpoZero = -1;  // WPO's input when it is a zero constant
        std::unordered_set<std::uint32_t>                   m_reachPS, m_reachVS;
        std::vector<TexSlot>                                m_textures;
        std::unordered_map<std::uint32_t, std::size_t>      m_texOfNode;
        std::set<std::string>                               m_helpers;
        std::set<std::string>                               m_diagSeen;

        // ---- Synthesized links, node preview, Pixel Normal WS, Custom nodes ----
        std::deque<Link>                                    m_synth;        // compiler-made links (stable addresses)
        const Link*                                         m_previewLink = nullptr;
        int                                                 m_previewInput = -1;
        int                                                 m_normalRoot = -1;       // the Normal pin's input index
        bool                                                m_usesPixelNormal = false;
        std::vector<std::string>                            m_customFns;    // Custom nodes' functions, in emission order
        std::set<std::string>                               m_customDone;
        // A parameter node -> its name's first node (same-named nodes are one parameter).
        std::unordered_map<std::uint32_t, std::uint32_t>    m_paramOwner;
    };
} // namespace

CompileResult Compile(const Graph& graph, const CompileOptions& options)
{
    CompileResult r;
    if (NeedsExpansion(graph))
    {
        // Functions expanded in place, static switches resolved, then the usual compile of that graph
        // (its hash covers the functions' contents and the switches' values; a diagnostic inside a
        // function names the call).
        Graph     flat;
        Expansion x;
        Flattener(graph, options, flat, x, r).Run();
        Compiler c(flat, options, r, &x);
        c.Run();
        c.Finish();
        return r;
    }
    Compiler c(graph, options, r);
    c.Run();
    c.Finish();
    return r;
}

std::string CompileReportJson(const CompileResult& c, bool withHlsl)
{
    using Json = nlohmann::json;
    auto vec = [](const float* v, int n) {
        Json a = Json::array();
        for (int i = 0; i < n; ++i) a.push_back(v[i]);
        return a;
    };
    Json params = Json::array();
    for (const ParamInfo& p : c.params)
    {
        Json e{ { "name", p.name }, { "hlsl", p.hlslName }, { "label", p.label }, { "components", p.components },
                { "default", vec(p.def, p.components) }, { "offset", p.offset }, { "group", p.group }, { "node", p.node } };
        if (p.hasRange) { e["min"] = vec(p.min, p.components); e["max"] = vec(p.max, p.components); }
        params.push_back(std::move(e));
    }
    Json textures = Json::array();
    for (const TextureInfo& t : c.textures)
        textures.push_back(Json{ { "name", t.name }, { "hlsl", t.hlslName }, { "slot", t.slot }, { "default", t.defaultPath },
                                 { "sampler", t.sampler }, { "parameter", t.parameter }, { "vertex", t.vertex },
                                 { "nodes", t.nodes } });
    Json diags = Json::array();
    int errors = 0, warnings = 0;
    for (const Diagnostic& d : c.diagnostics)
    {
        if (d.severity == Severity::Error) ++errors;
        else if (d.severity == Severity::Warning) ++warnings;
        Json e{ { "node", d.node }, { "severity", SeverityName(d.severity) }, { "code", d.code }, { "message", d.message } };
        if (!d.pin.empty()) e["pin"] = d.pin;
        diags.push_back(std::move(e));
    }
    const CompileStats& s = c.stats;
    Json j{ { "compiled", c.ok }, { "graphHash", c.graphHash },
            { "hlslSha256", c.ok ? Sys::Sha256Hex(c.hlsl.data(), c.hlsl.size()) : std::string() },
            { "errors", errors }, { "warnings", warnings }, { "diagnostics", std::move(diags) },
            { "stats", { { "nodes", s.nodes }, { "reachable", s.reachable }, { "params", s.params },
                         { "cbufferBytes", s.cbufferBytes }, { "textures", s.textures }, { "hlslBytes", s.hlslBytes },
                         { "pixelLines", s.pixelLines }, { "vertexLines", s.vertexLines }, { "usesVertex", s.usesVertex } } },
            { "params", std::move(params) }, { "textures", std::move(textures) } };
    // The static switches met (the value each was compiled with) and the functions expanded.
    Json switches = Json::array();
    for (const StaticSwitchInfo& w : c.switches)
        switches.push_back(Json{ { "name", w.name }, { "default", w.def }, { "value", w.value }, { "group", w.group },
                                 { "node", w.node } });
    j["switches"]  = std::move(switches);
    j["functions"] = c.functions;
    // The texture channel an active Opacity Mask reads directly (the shadow pass's mirror).
    if (c.opacityMaskTexture >= 0 && c.opacityMaskTexture < static_cast<int>(c.textures.size()))
        j["opacityMask"] = Json{ { "texture", c.textures[static_cast<std::size_t>(c.opacityMaskTexture)].hlslName },
                                 { "channel", c.opacityMaskChannel } };
    if (withHlsl) j["hlsl"] = c.hlsl;
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::vector<Diagnostic> MapDxcDiagnostics(const std::string& dxcText, const std::string& hlsl, const std::string& sourcePath)
{
    // DXC speaks clang: "<file>:<line>:<col>: error: <message>". A line of the
    // generated file ends in "// @n<id>" when a node emitted it - that node gets
    // the error; anything else (an include, a line no node owns) is the graph's.
    std::vector<std::string> lines;
    for (std::size_t b = 0; b <= hlsl.size();)
    {
        const std::size_t e = hlsl.find('\n', b);
        lines.push_back(hlsl.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    auto canon = [](std::string p) {
        for (char& c : p) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return p;
    };
    const std::string src = canon(sourcePath);
    std::vector<Diagnostic> out;
    std::set<std::string> seen;
    std::size_t b = 0;
    while (b < dxcText.size())
    {
        std::size_t e = dxcText.find('\n', b);
        if (e == std::string::npos) e = dxcText.size();
        const std::string line = dxcText.substr(b, e - b);
        b = e + 1;
        const std::size_t tag = line.find(": error: ");
        if (tag == std::string::npos) continue;
        // file:line:col before the tag (the file may hold a drive colon).
        const std::size_t c2 = line.rfind(':', tag - 1);
        const std::size_t c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : line.rfind(':', c2 - 1);
        Diagnostic d;
        d.severity = Severity::Error;
        d.code     = "dxc";
        d.message  = line.substr(tag + 9);
        if (c1 != std::string::npos)
        {
            const std::string file = canon(line.substr(0, c1));
            const int ln = std::atoi(line.substr(c1 + 1, c2 - c1 - 1).c_str());
            const bool ours = file == src || (file.size() >= src.size() && file.compare(file.size() - src.size(), src.size(), src) == 0);
            if (ours && ln >= 1 && ln <= static_cast<int>(lines.size()))
            {
                const std::string& text = lines[static_cast<std::size_t>(ln - 1)];
                const std::size_t at = text.rfind("// @n");
                if (at != std::string::npos) d.node = static_cast<std::uint32_t>(std::strtoul(text.c_str() + at + 5, nullptr, 10));
                d.message = "line " + std::to_string(ln) + ": " + d.message;
            }
        }
        if (seen.insert(std::to_string(d.node) + d.message).second) out.push_back(std::move(d));
    }
    if (out.empty() && !dxcText.empty())
    {
        Diagnostic d;
        d.severity = Severity::Error;
        d.code     = "dxc";
        d.message  = dxcText.substr(0, 400);
        out.push_back(std::move(d));
    }
    return out;
}

bool OutputPinActive(std::string_view pin, ShadingModel model, BlendMode blend)
{
    // Which model reads which pin is the shading-model table's `pins` column
    // (shaders/shading_model_table.hlsli); only the blend-mode rules live here.
    const ShadingModels::Desc& sm = ShadingModels::Of(model);
    // An Unlit material the deferred path draws (opaque, masked, dithered) shows its base colour AND
    // its emissive (Lighting.ps's Unlit exit adds the emissive on top) - the engine's contract, which
    // Convert to Graph reproduces exactly; a forward one shows Emissive alone.
    if (pin == "BaseColor")
        return sm.route != SMT_ROUTE_UNLIT ||
               blend == BlendMode::Opaque || blend == BlendMode::Masked || blend == BlendMode::Dithered;
    if (pin == "Metallic" || pin == "Specular") return sm.Reads(SMT_PIN_METALLIC_SPECULAR);
    if (pin == "Roughness") return sm.Reads(SMT_PIN_ROUGHNESS);
    if (pin == "Anisotropy" || pin == "Tangent") return sm.Reads(SMT_PIN_ANISOTROPY);
    if (pin == "EmissiveColor" || pin == "Normal" || pin == "AmbientOcclusion" || pin == "WorldPositionOffset") return true;
    if (pin == "Opacity") return IsTranslucent(blend);
    // Opaque too: the engine alpha-tests an Opaque material whose alphaRef asks for it
    // (MaterialComponent::IsAlphaTestEnabled; the ALPHA_TEST permutation reads s.opacity); without
    // that no permutation reads the value and DXC drops it.
    if (pin == "OpacityMask") return blend == BlendMode::Opaque || blend == BlendMode::Masked || blend == BlendMode::Dithered;
    if (pin == "SubsurfaceOpacity" || pin == "SubsurfaceMask") return sm.Reads(SMT_PIN_SUBSURFACE);
    if (pin == "ClearCoat" || pin == "ClearCoatRoughness") return sm.Reads(SMT_PIN_CLEARCOAT);
    if (pin == "DirectAdditive") return sm.Reads(SMT_PIN_DIRECT_ADDITIVE);
    return false;
}

const char* SeverityName(Severity s)
{
    switch (s)
    {
        case Severity::Info:    return "info";
        case Severity::Warning: return "warning";
        default:                return "error";
    }
}

const char* BlendModeLabel(BlendMode b)
{
    switch (b)
    {
        case BlendMode::Opaque:        return "Opaque";
        case BlendMode::Alpha:         return "Alpha";
        case BlendMode::Additive:      return "Additive";
        case BlendMode::Premultiplied: return "Premultiplied";
        case BlendMode::Multiply:      return "Multiply";
        case BlendMode::Masked:        return "Masked";
        case BlendMode::Dithered:      return "Dithered";
        case BlendMode::Transmissive:  return "Transmissive";
        default:                       return "Unknown";
    }
}

namespace
{
    bool ParseSmallInt(std::string_view text, int& out)
    {
        if (text.empty() || text.size() > 3) return false;
        int v = 0;
        for (const char c : text)
        {
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    }
}

bool ParseShadingModel(std::string_view text, ShadingModel& out)
{
    // The table's `token` column (shaders/shading_model_table.hlsli), then a bare value.
    const std::string t = Lower(std::string(text));
    for (const ShadingModels::Desc& d : ShadingModels::kRows)
        if (t == d.token) { out = d.model; return true; }
    int v = 0;
    if (ParseSmallInt(text, v) && v < static_cast<int>(ShadingModel::Count))
    {
        out = static_cast<ShadingModel>(v);
        return true;
    }
    return false;
}

bool ParseGeneratedHeader(std::string_view hlsl, std::string& graph, CompileOptions& out)
{
    // Assemble()'s first lines: "// GENERATED by MaterialGraph v<N> from <graph> (graphHash <h>). Do not edit.",
    // "// Shading model <name>, blend mode <label>. Edit the graph in the Material Editor.",
    // then "// PREVIEW of ..." (a node preview: not a material's file) or "// Static switches: A=1, B=0.".
    auto line = [&](std::size_t& at) {
        const std::size_t e = hlsl.find('\n', at);
        std::string_view l = hlsl.substr(at, e == std::string_view::npos ? std::string_view::npos : e - at);
        at = e == std::string_view::npos ? hlsl.size() : e + 1;
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        return l;
    };
    std::size_t at = 0;
    const std::string_view l1 = line(at);
    const std::string_view kHead = "// GENERATED by MaterialGraph v";
    if (l1.substr(0, kHead.size()) != kHead) return false;
    const std::size_t from = l1.find(" from "), hash = l1.rfind(" (graphHash ");
    if (from == std::string_view::npos || hash == std::string_view::npos || hash <= from + 6) return false;
    graph = std::string(l1.substr(from + 6, hash - from - 6));
    const std::string_view l2 = line(at);
    const std::string_view kShading = "// Shading model ", kBlend = ", blend mode ";
    const std::size_t b = l2.find(kBlend), dot = l2.find(". ", b == std::string_view::npos ? 0 : b);
    if (l2.substr(0, kShading.size()) != kShading || b == std::string_view::npos || dot == std::string_view::npos) return false;
    CompileOptions o;
    // The names Assemble() wrote: ShadingModelName's ("Clear Coat", "Toon / NPR") and BlendModeLabel's.
    const std::string_view sm = l2.substr(kShading.size(), b - kShading.size());
    bool known = false;
    for (int i = 0; i < static_cast<int>(ShadingModel::Count) && !known; ++i)
        if (sm == ShadingModelName(static_cast<ShadingModel>(i))) { o.shadingModel = static_cast<ShadingModel>(i); known = true; }
    if (!known || !ParseBlendMode(l2.substr(b + kBlend.size(), dot - b - kBlend.size()), o.blendMode)) return false;
    const std::string_view l3 = line(at);
    if (l3.substr(0, 14) == "// PREVIEW of ") return false;
    const std::string_view kSwitches = "// Static switches:";
    if (l3.substr(0, kSwitches.size()) == kSwitches)
    {
        std::string_view rest = l3.substr(kSwitches.size());
        if (rest.empty() || rest.back() != '.') return false;
        rest.remove_suffix(1);
        while (!rest.empty())
        {
            const std::size_t comma = rest.find(", ");
            std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 2);
            while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
            const std::size_t eq = item.rfind('=');
            if (eq == std::string_view::npos || eq == 0 || (item.substr(eq + 1) != "1" && item.substr(eq + 1) != "0")) return false;
            if (!o.switches.emplace(std::string(item.substr(0, eq)), item.substr(eq + 1) == "1").second) return false;
        }
    }
    o.sourceLabel = graph;
    out = std::move(o);
    return true;
}

bool ParseBlendMode(std::string_view text, BlendMode& out)
{
    static const std::pair<const char*, BlendMode> kNames[] = {
        { "opaque", BlendMode::Opaque }, { "alpha", BlendMode::Alpha }, { "additive", BlendMode::Additive },
        { "premultiplied", BlendMode::Premultiplied }, { "multiply", BlendMode::Multiply },
        { "masked", BlendMode::Masked }, { "dithered", BlendMode::Dithered },
        { "transmissive", BlendMode::Transmissive } };
    const std::string t = Lower(std::string(text));
    for (const auto& [name, b] : kNames)
        if (t == name) { out = b; return true; }
    int v = 0;
    if (ParseSmallInt(text, v) && v >= 0 && v < static_cast<int>(BlendMode::Count))
    {
        out = static_cast<BlendMode>(v);
        return true;
    }
    return false;
}
} // namespace MaterialGraph
