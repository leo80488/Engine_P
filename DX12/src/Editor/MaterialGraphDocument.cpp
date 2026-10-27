// MaterialGraphDocument.cpp - see include/Editor/MaterialGraphDocument.h.
#include "Editor/MaterialGraphDocument.h"

#include "MaterialGraph/MaterialGraphNodes.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace Editor
{
namespace
{
    using MaterialGraph::Graph;
    using MaterialGraph::Link;
    using MaterialGraph::Node;
    using MaterialGraph::Value;

    using Json = nlohmann::json;

    bool HasOutput(const Graph& g)
    {
        for (const Node& n : g.nodes)
            if (const auto* d = MaterialGraph::FindNodeDesc(n.type); d && (d->flags & MaterialGraph::kNodeOutput)) return true;
        return false;
    }

    // Would a link from -> to close a cycle (is `from` downstream of `to`)? A Named Reroute
    // declaration feeds every usage of its name, a wire the graph does not store.
    bool WouldCycle(const Graph& g, std::uint32_t from, std::uint32_t to)
    {
        if (from == to) return true;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> fwd;
        for (const Link& l : g.links) fwd[l.fromNode].push_back(l.toNode);
        for (const Node& d : g.nodes)
            if (d.type == "NamedRerouteDeclaration")
                for (const Node& u : g.nodes)
                    if (u.type == "NamedRerouteUsage" && u.PropString("name") == d.PropString("name")) fwd[d.id].push_back(u.id);
        std::vector<std::uint32_t> stack{ to };
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty())
        {
            const std::uint32_t n = stack.back();
            stack.pop_back();
            if (n == from) return true;
            if (!seen.insert(n).second) continue;
            if (const auto it = fwd.find(n); it != fwd.end())
                for (const std::uint32_t m : it->second) stack.push_back(m);
        }
        return false;
    }

    bool CheckInputs(const MaterialGraph::NodeDesc& d, const MaterialGraph::ValueMap& inputs, std::string& why)
    {
        for (const auto& [name, v] : inputs)
        {
            const MaterialGraph::PinDesc* p = d.Input(name);
            if (!p || p->hidden)
            {
                why = "unknown_pin: " + std::string(d.label) + " has no input '" + name + "'";
                return false;
            }
            if (!v.IsNumeric() || v.Components() < 1 || v.Components() > 4)
            {
                why = "bad_value: input '" + name + "' wants 1-4 numbers";
                return false;
            }
        }
        return true;
    }

    Node* FindNode(Graph& g, std::uint32_t id)
    {
        for (Node& n : g.nodes) if (n.id == id) return &n;
        return nullptr;
    }

    MaterialGraph::Comment* FindComment(Graph& g, std::uint32_t id)
    {
        for (MaterialGraph::Comment& c : g.comments) if (c.id == id) return &c;
        return nullptr;
    }

    bool JsonValue(const Json& j, Value& out)
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
            std::vector<double> v;
            for (const Json& e : j)
            {
                if (!e.is_number() || !std::isfinite(e.get<double>())) return false;
                v.push_back(e.get<double>());
            }
            out = Value::Numbers(std::move(v));
            return true;
        }
        return false;
    }

    bool JsonMap(const Json& j, const char* key, MaterialGraph::ValueMap& out, std::string& why)
    {
        if (!j.contains(key)) return true;
        const Json& m = j[key];
        if (!m.is_object())
        {
            // An empty Lua table converts to an empty array: accept it as {}.
            if (m.is_array() && m.empty()) return true;
            why = std::string("bad_value: \"") + key + "\" wants {name: value}";
            return false;
        }
        for (auto it = m.begin(); it != m.end(); ++it)
        {
            Value v;
            if (!JsonValue(it.value(), v))
            {
                why = std::string("bad_value: ") + key + "." + it.key() + " wants a number, bool, string or numbers";
                return false;
            }
            out[it.key()] = std::move(v);
        }
        return true;
    }

    bool JsonFloats(const Json& j, const char* key, float* out, std::size_t n, bool& has, std::string& why)
    {
        has = false;
        if (!j.contains(key)) return true;
        const Json& a = j[key];
        if (!a.is_array() || a.size() != n)
        {
            why = std::string("bad_value: \"") + key + "\" wants " + std::to_string(n) + " numbers";
            return false;
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!a[i].is_number() || !std::isfinite(a[i].get<double>()))
            {
                why = std::string("bad_value: \"") + key + "\" wants numbers";
                return false;
            }
            out[i] = static_cast<float>(a[i].get<double>());
        }
        has = true;
        return true;
    }

    // A node by id (a positive number) or by the batch-local name of an earlier add.
    bool JsonRef(const Json& j, const char* key, std::uint32_t& id, std::string& ref, bool required, std::string& why)
    {
        if (!j.contains(key))
        {
            if (required) why = std::string("bad_value: \"") + key + "\" is missing";
            return !required;
        }
        const Json& v = j[key];
        if (v.is_string()) { ref = v.get<std::string>(); return true; }
        if (v.is_number_integer() && v.get<long long>() > 0 && v.get<long long>() <= 0xFFFFFFFFll)
        {
            id = static_cast<std::uint32_t>(v.get<long long>());
            return true;
        }
        if (v.is_number_float() && v.get<double>() >= 1.0 && std::floor(v.get<double>()) == v.get<double>())
        {
            id = static_cast<std::uint32_t>(v.get<double>());
            return true;
        }
        why = std::string("bad_value: \"") + key + "\" wants a node id or the name of an earlier add";
        return false;
    }

    bool JsonString(const Json& j, const char* key, std::string& out, bool required, std::string& why)
    {
        if (!j.contains(key))
        {
            if (required) why = std::string("bad_value: \"") + key + "\" is missing";
            return !required;
        }
        if (!j[key].is_string())
        {
            why = std::string("bad_value: \"") + key + "\" wants a string";
            return false;
        }
        out = j[key].get<std::string>();
        return true;
    }

    bool JsonStrings(const Json& j, const char* key, std::vector<std::string>& out, std::string& why)
    {
        if (!j.contains(key)) return true;
        if (!j[key].is_array())
        {
            why = std::string("bad_value: \"") + key + "\" wants a list of names";
            return false;
        }
        for (const Json& e : j[key])
        {
            if (!e.is_string()) { why = std::string("bad_value: \"") + key + "\" wants names"; return false; }
            out.push_back(e.get<std::string>());
        }
        return true;
    }
}

bool ParseGraphOps(const std::string& jsonText, std::vector<GraphOp>& out, std::string& why)
{
    out.clear();
    Json j = Json::parse(jsonText, nullptr, false);
    if (j.is_discarded()) { why = "bad_value: the ops are not JSON"; return false; }
    if (j.is_object() && j.contains("ops")) j = j["ops"];
    if (j.is_object()) j = Json::array({ j });   // one op
    if (!j.is_array() || j.empty()) { why = "bad_value: want a list of ops"; return false; }
    for (std::size_t i = 0; i < j.size(); ++i)
    {
        const Json& o = j[i];
        const std::string at = " (op " + std::to_string(i + 1) + ")";
        if (!o.is_object() || !o.contains("op") || !o["op"].is_string())
        {
            why = "bad_value: every op is {\"op\": ...}" + at;
            return false;
        }
        const std::string kind = o["op"].get<std::string>();
        GraphOp op;
        bool ok = true;
        if (kind == "add")
        {
            op.kind = GraphOp::Kind::Add;
            ok = JsonString(o, "type", op.type, true, why) && JsonString(o, "as", op.as, false, why) &&
                 JsonFloats(o, "pos", op.pos, 2, op.hasPos, why) && JsonMap(o, "props", op.props, why) &&
                 JsonMap(o, "inputs", op.inputs, why);
        }
        else if (kind == "remove")
        {
            op.kind = GraphOp::Kind::Remove;
            ok = JsonRef(o, "node", op.node, op.nodeRef, true, why);
        }
        else if (kind == "connect" || kind == "disconnect")
        {
            op.kind = kind == "connect" ? GraphOp::Kind::Connect : GraphOp::Kind::Disconnect;
            const bool connect = op.kind == GraphOp::Kind::Connect;
            ok = JsonRef(o, "from", op.from, op.fromRef, connect, why) && JsonRef(o, "to", op.to, op.toRef, true, why) &&
                 JsonString(o, "fromPin", op.fromPin, connect, why) && JsonString(o, "toPin", op.toPin, true, why);
        }
        else if (kind == "set")
        {
            op.kind = GraphOp::Kind::Set;
            ok = JsonRef(o, "node", op.node, op.nodeRef, true, why) && JsonMap(o, "props", op.props, why) &&
                 JsonMap(o, "inputs", op.inputs, why) && JsonStrings(o, "unset", op.unsetProps, why) &&
                 JsonStrings(o, "unsetInputs", op.unsetInputs, why);
        }
        else if (kind == "move")
        {
            op.kind = GraphOp::Kind::Move;
            ok = JsonRef(o, "node", op.node, op.nodeRef, true, why) && JsonFloats(o, "pos", op.pos, 2, op.hasPos, why);
            if (ok && !op.hasPos) { why = "bad_value: \"pos\" is missing"; ok = false; }
        }
        else if (kind == "comment" || kind == "setcomment")
        {
            op.kind = kind == "comment" ? GraphOp::Kind::Comment : GraphOp::Kind::SetComment;
            ok = (op.kind == GraphOp::Kind::Comment || JsonRef(o, "node", op.node, op.nodeRef, true, why)) &&
                 JsonString(o, "as", op.as, false, why) && JsonFloats(o, "rect", op.rect, 4, op.hasRect, why) &&
                 JsonFloats(o, "color", op.color, 4, op.hasColor, why);
            if (ok && o.contains("text"))
            {
                ok = JsonString(o, "text", op.text, true, why);
                op.hasText = ok;
            }
            if (ok && op.kind == GraphOp::Kind::Comment && !op.hasRect) { why = "bad_value: \"rect\" is missing"; ok = false; }
        }
        else if (kind == "uncomment")
        {
            op.kind = GraphOp::Kind::Uncomment;
            ok = JsonRef(o, "node", op.node, op.nodeRef, true, why);
        }
        else
        {
            why = "bad_value: unknown op '" + kind + "' (add, remove, connect, disconnect, set, move, comment, "
                  "setcomment, uncomment)";
            ok = false;
        }
        if (!ok)
        {
            why += at;
            return false;
        }
        out.push_back(std::move(op));
    }
    return true;
}

bool MaterialGraphDocument::Open(const std::string& path, std::string& why)
{
    MaterialGraph::Graph g;
    if (!MaterialGraph::LoadGraph(path, g, why)) return false;
    m_path  = path;
    m_graph = std::move(g);
    m_undo.clear();
    m_redo.clear();
    m_dirty = false;
    ++m_revision;
    m_savedText = MaterialGraph::SerializeGraph(m_graph);
    NoteDiskStamp();
    return true;
}

void MaterialGraphDocument::New(const std::string& path, const std::string& ownerGuid)
{
    m_path  = path;
    m_graph = MaterialGraph::Graph{};
    m_graph.ownerGuid = ownerGuid;
    Node out;
    out.id     = m_graph.AllocateId();
    out.type   = "MaterialOutput";
    out.pos[0] = 400.0f;
    m_graph.nodes.push_back(std::move(out));
    m_undo.clear();
    m_redo.clear();
    ++m_revision;
    m_savedText.clear();      // never written: dirty until saved
    m_dirty = true;
    m_diskSize = 0;
    m_diskTime = 0;
}

void MaterialGraphDocument::NewFunction(const std::string& path)
{
    // A Material Function: one input straight to one output, to grow from.
    m_path  = path;
    m_graph = MaterialGraph::Graph{};
    m_graph.function = true;
    Node in;
    in.id    = m_graph.AllocateId();
    in.type  = "FunctionInput";
    in.props["name"] = Value::String("In");
    in.props["type"] = Value::String("float3");
    Node out;
    out.id     = m_graph.AllocateId();
    out.type   = "FunctionOutput";
    out.pos[0] = 400.0f;
    out.props["name"] = Value::String("Result");
    m_graph.links.push_back({ in.id, "Out", out.id, "Input" });
    m_graph.nodes.push_back(std::move(in));
    m_graph.nodes.push_back(std::move(out));
    m_undo.clear();
    m_redo.clear();
    ++m_revision;
    m_savedText.clear();      // never written: dirty until saved
    m_dirty = true;
}

std::uint32_t MaterialGraphDocument::OutputId() const
{
    for (const Node& n : m_graph.nodes)
        if (n.type == "MaterialOutput") return n.id;
    return 0;
}

bool MaterialGraphDocument::ApplyOne(MaterialGraph::Graph& g, const GraphOp& op, std::map<std::string, std::uint32_t>& names,
                                     std::vector<std::uint32_t>* newIds, std::string& why) const
{
    auto resolve = [&](std::uint32_t id, const std::string& ref, const char* role, std::uint32_t& out) -> bool
    {
        if (!ref.empty())
        {
            const auto it = names.find(ref);
            if (it == names.end())
            {
                why = "unknown_node: no node named '" + ref + "' earlier in this batch";
                return false;
            }
            out = it->second;
            return true;
        }
        if (id == 0)
        {
            why = std::string("bad_value: ") + role + " is missing";
            return false;
        }
        out = id;
        return true;
    };
    auto nodeOf = [&](std::uint32_t id) -> Node*
    {
        Node* n = FindNode(g, id);
        if (!n) why = "unknown_node: no node " + std::to_string(id);
        return n;
    };

    using K = GraphOp::Kind;
    switch (op.kind)
    {
    case K::Add:
    {
        if (!MaterialGraph::FindNodeDesc(op.type)) { why = "unknown_type: no node type '" + op.type + "'"; return false; }
        Node probe;   // a Custom node's pins are its props'
        probe.type  = op.type;
        probe.props = op.props;
        const MaterialGraph::NodeDesc* d = MaterialGraph::DescribeNode(probe);
        if ((d->flags & MaterialGraph::kNodeOutput) && g.function)
        {
            why = "refused: a Material Function ends in Function Outputs, not a Material Output";
            return false;
        }
        if ((d->flags & MaterialGraph::kNodeFunction) && !g.function)
        {
            why = "refused: Function Input / Output belong in a Material Function (.imatfunc)";
            return false;
        }
        if ((d->flags & MaterialGraph::kNodeOutput) && HasOutput(g))
        {
            why = "refused: a graph has one Material Output";
            return false;
        }
        if (!CheckInputs(*d, op.inputs, why)) return false;
        if (!op.as.empty() && names.count(op.as)) { why = "bad_value: two nodes of the batch are named '" + op.as + "'"; return false; }
        Node n;
        n.id     = g.AllocateId();
        n.type   = op.type;
        n.pos[0] = op.pos[0];
        n.pos[1] = op.pos[1];
        n.props  = op.props;
        n.inputs = op.inputs;
        if (!op.as.empty()) names[op.as] = n.id;
        if (newIds) newIds->push_back(n.id);
        g.nodes.push_back(std::move(n));
        return true;
    }
    case K::Remove:
    {
        std::uint32_t id = 0;
        if (!resolve(op.node, op.nodeRef, "node", id)) return false;
        if (FindComment(g, id))
        {
            g.comments.erase(std::remove_if(g.comments.begin(), g.comments.end(),
                                            [id](const MaterialGraph::Comment& c) { return c.id == id; }), g.comments.end());
            return true;
        }
        Node* n = nodeOf(id);
        if (!n) return false;
        if (n->type == "MaterialOutput") { why = "refused: the Material Output cannot be removed"; return false; }
        g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [id](const Node& x) { return x.id == id; }), g.nodes.end());
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                     [id](const Link& l) { return l.fromNode == id || l.toNode == id; }), g.links.end());
        return true;
    }
    case K::Connect:
    {
        std::uint32_t from = 0, to = 0;
        if (!resolve(op.from, op.fromRef, "from", from) || !resolve(op.to, op.toRef, "to", to)) return false;
        Node* a = nodeOf(from);
        if (!a) return false;
        Node* b = nodeOf(to);
        if (!b) return false;
        const auto* da = MaterialGraph::DescribeNode(*a);
        const auto* db = MaterialGraph::DescribeNode(*b);
        if (!da || !da->Output(op.fromPin))
        {
            why = "unknown_pin: node " + std::to_string(from) + " has no output '" + op.fromPin + "'";
            return false;
        }
        if (!db || !db->Input(op.toPin) || db->Input(op.toPin)->hidden)
        {
            why = "unknown_pin: node " + std::to_string(to) + " has no input '" + op.toPin + "'";
            return false;
        }
        if (WouldCycle(g, from, to)) { why = "would_cycle: node " + std::to_string(to) + " already feeds node " + std::to_string(from); return false; }
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                     [&](const Link& l) { return l.toNode == to && l.toPin == op.toPin; }), g.links.end());
        g.links.push_back({ from, op.fromPin, to, op.toPin });
        return true;
    }
    case K::Disconnect:
    {
        std::uint32_t to = 0;
        if (!resolve(op.to, op.toRef, "to", to)) return false;
        std::uint32_t from = 0;
        if ((op.from || !op.fromRef.empty()) && !resolve(op.from, op.fromRef, "from", from)) return false;
        const auto before = g.links.size();
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const Link& l) {
                          return l.toNode == to && l.toPin == op.toPin && (!from || l.fromNode == from) &&
                                 (op.fromPin.empty() || l.fromPin == op.fromPin);
                      }), g.links.end());
        if (g.links.size() == before)
        {
            why = "not_found: nothing is linked into " + std::to_string(to) + "." + op.toPin;
            return false;
        }
        return true;
    }
    case K::Set:
    {
        std::uint32_t id = 0;
        if (!resolve(op.node, op.nodeRef, "node", id)) return false;
        Node* n = nodeOf(id);
        if (!n) return false;
        // The props first: a Custom node's pins are what its props now declare.
        Node next = *n;
        for (const auto& [k, v] : op.props) next.props[k] = v;
        for (const std::string& k : op.unsetProps) next.props.erase(k);
        const auto* d = MaterialGraph::DescribeNode(next);
        if (d && !CheckInputs(*d, op.inputs, why)) return false;
        for (const auto& [k, v] : op.inputs) next.inputs[k] = v;
        for (const std::string& k : op.unsetInputs) next.inputs.erase(k);
        if (d && next.type == "Custom")
        {
            // A pin the node no longer declares: its link and stored value go with it.
            g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const Link& l) {
                              return (l.toNode == id && !d->Input(l.toPin)) || (l.fromNode == id && !d->Output(l.fromPin));
                          }), g.links.end());
            for (auto it = next.inputs.begin(); it != next.inputs.end();)
                it = d->Input(it->first) ? std::next(it) : next.inputs.erase(it);
        }
        *n = std::move(next);
        return true;
    }
    case K::Move:
    {
        std::uint32_t id = 0;
        if (!resolve(op.node, op.nodeRef, "node", id)) return false;
        if (MaterialGraph::Comment* c = FindComment(g, id))
        {
            const float w = c->rect[2] - c->rect[0], h = c->rect[3] - c->rect[1];
            c->rect[0] = op.pos[0];
            c->rect[1] = op.pos[1];
            c->rect[2] = op.pos[0] + w;
            c->rect[3] = op.pos[1] + h;
            return true;
        }
        Node* n = nodeOf(id);
        if (!n) return false;
        n->pos[0] = op.pos[0];
        n->pos[1] = op.pos[1];
        return true;
    }
    case K::Comment:
    {
        if (!op.as.empty() && names.count(op.as)) { why = "bad_value: two nodes of the batch are named '" + op.as + "'"; return false; }
        MaterialGraph::Comment c;
        c.id = g.AllocateId();
        std::copy(op.rect, op.rect + 4, c.rect);
        std::copy(op.color, op.color + 4, c.color);
        c.text = op.text;
        if (!op.as.empty()) names[op.as] = c.id;
        if (newIds) newIds->push_back(c.id);
        g.comments.push_back(std::move(c));
        return true;
    }
    case K::SetComment:
    case K::Uncomment:
    {
        std::uint32_t id = 0;
        if (!resolve(op.node, op.nodeRef, "node", id)) return false;
        MaterialGraph::Comment* c = FindComment(g, id);
        if (!c) { why = "unknown_node: no comment " + std::to_string(id); return false; }
        if (op.kind == K::Uncomment)
        {
            g.comments.erase(std::remove_if(g.comments.begin(), g.comments.end(),
                                            [id](const MaterialGraph::Comment& x) { return x.id == id; }), g.comments.end());
            return true;
        }
        if (op.hasRect) std::copy(op.rect, op.rect + 4, c->rect);
        if (op.hasColor) std::copy(op.color, op.color + 4, c->color);
        if (op.hasText) c->text = op.text;
        return true;
    }
    }
    why = "bad_value: unknown op";
    return false;
}

bool MaterialGraphDocument::Apply(const std::vector<GraphOp>& ops, std::vector<std::uint32_t>* newIds, std::string& why)
{
    if (ops.empty()) { why = "bad_value: no ops"; return false; }
    MaterialGraph::Graph next = m_graph;
    std::map<std::string, std::uint32_t> names;
    std::vector<std::uint32_t> ids;
    for (std::size_t i = 0; i < ops.size(); ++i)
        if (!ApplyOne(next, ops[i], names, &ids, why))
        {
            why = why + " (op " + std::to_string(i + 1) + ")";
            return false;
        }
    Commit(std::move(next));
    if (newIds) *newIds = std::move(ids);
    return true;
}

void MaterialGraphDocument::Commit(MaterialGraph::Graph&& next)
{
    if (!m_mergeNext)
    {
        m_undo.push_back(m_graph);
        if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    }
    m_committedThisFrame = true;
    m_redo.clear();
    m_graph = std::move(next);
    m_dirty = MaterialGraph::SerializeGraph(m_graph) != m_savedText;
    ++m_revision;
}

void MaterialGraphDocument::EndFrame(bool anyItemActive)
{
    m_mergeNext = anyItemActive && (m_mergeNext || m_committedThisFrame);
    m_committedThisFrame = false;
}

bool MaterialGraphDocument::Undo()
{
    if (m_undo.empty()) return false;
    m_redo.push_back(std::move(m_graph));
    m_graph = std::move(m_undo.back());
    m_undo.pop_back();
    m_dirty = MaterialGraph::SerializeGraph(m_graph) != m_savedText;
    m_mergeNext = false;
    ++m_revision;
    return true;
}

bool MaterialGraphDocument::Redo()
{
    if (m_redo.empty()) return false;
    m_undo.push_back(std::move(m_graph));
    m_graph = std::move(m_redo.back());
    m_redo.pop_back();
    m_dirty = MaterialGraph::SerializeGraph(m_graph) != m_savedText;
    m_mergeNext = false;
    ++m_revision;
    return true;
}

std::string MaterialGraphDocument::CopyText(const std::vector<std::uint32_t>& ids) const
{
    const std::unordered_set<std::uint32_t> want(ids.begin(), ids.end());
    MaterialGraph::Graph c;
    std::uint32_t maxId = 0;
    for (const Node& n : m_graph.nodes)
        if (want.count(n.id) && n.type != "MaterialOutput")
        {
            c.nodes.push_back(n);
            maxId = std::max(maxId, n.id);
        }
    std::unordered_set<std::uint32_t> kept;
    for (const Node& n : c.nodes) kept.insert(n.id);
    for (const Link& l : m_graph.links)
        if (kept.count(l.fromNode) && kept.count(l.toNode)) c.links.push_back(l);
    for (const MaterialGraph::Comment& cm : m_graph.comments)
        if (want.count(cm.id))
        {
            c.comments.push_back(cm);
            maxId = std::max(maxId, cm.id);
        }
    c.nextId = maxId + 1;
    return MaterialGraph::SerializeGraph(c);
}

bool MaterialGraphDocument::Paste(const std::string& text, const float offset[2], std::vector<std::uint32_t>* newIds,
                                  std::string& why)
{
    MaterialGraph::Graph clip;
    std::string err;
    if (!MaterialGraph::ParseGraph(text, clip, err)) { why = "bad_value: the clipboard holds no material graph (" + err + ")"; return false; }
    MaterialGraph::Graph next = m_graph;
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    std::vector<std::uint32_t> ids;
    for (Node n : clip.nodes)
    {
        if (n.type == "MaterialOutput") continue;
        const std::uint32_t id = next.AllocateId();
        remap[n.id] = id;
        n.id = id;
        n.pos[0] += offset[0];
        n.pos[1] += offset[1];
        ids.push_back(id);
        next.nodes.push_back(std::move(n));
    }
    for (const Link& l : clip.links)
    {
        const auto a = remap.find(l.fromNode), b = remap.find(l.toNode);
        if (a != remap.end() && b != remap.end()) next.links.push_back({ a->second, l.fromPin, b->second, l.toPin });
    }
    for (MaterialGraph::Comment c : clip.comments)
    {
        c.id = next.AllocateId();
        c.rect[0] += offset[0]; c.rect[2] += offset[0];
        c.rect[1] += offset[1]; c.rect[3] += offset[1];
        ids.push_back(c.id);
        next.comments.push_back(std::move(c));
    }
    if (ids.empty()) { why = "bad_value: nothing to paste"; return false; }
    Commit(std::move(next));
    if (newIds) *newIds = std::move(ids);
    return true;
}

std::vector<std::uint32_t> MaterialGraphDocument::Find(const std::string& text) const
{
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = lower(text);
    std::vector<std::uint32_t> out;
    if (q.empty()) return out;
    for (const Node& n : m_graph.nodes)
    {
        const MaterialGraph::NodeDesc* d = MaterialGraph::DescribeNode(n);
        std::string hay = "#" + std::to_string(n.id) + "\n" + n.type + "\n" + (d ? d->label : "");
        for (const auto& [k, v] : n.props)
            if (v.kind == Value::Kind::String) hay += "\n" + v.str;
        if (lower(hay).find(q) != std::string::npos) out.push_back(n.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::uint32_t> MaterialGraphDocument::Related(const std::vector<std::uint32_t>& seeds) const
{
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> fwd, back;
    for (const Link& l : m_graph.links)
    {
        fwd[l.fromNode].push_back(l.toNode);
        back[l.toNode].push_back(l.fromNode);
    }
    for (const Node& d : m_graph.nodes)
        if (d.type == "NamedRerouteDeclaration")
            for (const Node& u : m_graph.nodes)
                if (u.type == "NamedRerouteUsage" && u.PropString("name") == d.PropString("name"))
                {
                    fwd[d.id].push_back(u.id);
                    back[u.id].push_back(d.id);
                }
    std::unordered_set<std::uint32_t> seen;
    for (auto* dir : { &fwd, &back })
    {
        std::vector<std::uint32_t> stack(seeds.begin(), seeds.end());
        std::unordered_set<std::uint32_t> walked;
        while (!stack.empty())
        {
            const std::uint32_t n = stack.back();
            stack.pop_back();
            if (!walked.insert(n).second) continue;
            seen.insert(n);
            if (const auto it = dir->find(n); it != dir->end())
                for (const std::uint32_t m : it->second) stack.push_back(m);
        }
    }
    std::vector<std::uint32_t> out(seen.begin(), seen.end());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<MaterialGraphDocument::PaletteEntry> MaterialGraphDocument::CompatibleNodes(int components, bool fromOutput)
{
    // The canvas's drag rule (MaterialGraphCanvas): only a vector into a LARGER fixed pin is refused.
    auto fits = [](int from, int to) { return !(from > 1 && to > 0 && from < to); };
    std::vector<PaletteEntry> out;
    for (const MaterialGraph::NodeDesc& d : MaterialGraph::NodeRegistry())
    {
        if ((d.flags & MaterialGraph::kNodeOutput) || std::string_view(d.type) == "MaterialFunctionCall") continue;
        if (fromOutput)
        {
            for (const MaterialGraph::PinDesc& p : d.inputs)
                if (!p.hidden && fits(components, p.components)) { out.push_back({ &d, p.name }); break; }
        }
        else
        {
            for (const MaterialGraph::PinDesc& p : d.outputs)
                if (fits(p.components, components)) { out.push_back({ &d, p.name }); break; }
        }
    }
    return out;
}

namespace
{
    bool IsParameterType(const std::string& t)
    {
        return t == "ScalarParameter" || t == "VectorParameter" || t == "TextureSampleParameter2D" || t == "StaticSwitchParameter";
    }
}

std::vector<MaterialGraphDocument::ParameterRow> MaterialGraphDocument::Parameters() const
{
    std::map<std::pair<std::string, std::string>, ParameterRow> rows;   // (type, name)
    std::vector<const Node*> nodes;
    for (const Node& n : m_graph.nodes)
        if (IsParameterType(n.type)) nodes.push_back(&n);
    std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) { return a->id < b->id; });
    for (const Node* n : nodes)
    {
        ParameterRow& r = rows[{ n->type, n->PropString("name") }];
        if (!r.first) { r.first = n; r.name = n->PropString("name"); r.type = n->type; }
        r.nodes.push_back(n->id);
    }
    std::vector<ParameterRow> out;
    for (auto& [_, r] : rows) out.push_back(std::move(r));
    std::sort(out.begin(), out.end(), [](const ParameterRow& a, const ParameterRow& b) {
        const bool sa = a.type == "StaticSwitchParameter", sb = b.type == "StaticSwitchParameter";
        if (sa != sb) return sb;                                      // switches last
        const std::string ga = a.first->PropString("group"), gb = b.first->PropString("group");
        if (ga != gb) return ga < gb;
        const double oa = a.first->PropNumber("sort", 0.0), ob = b.first->PropNumber("sort", 0.0);
        if (oa != ob) return oa < ob;
        return a.name < b.name;
    });
    return out;
}

bool MaterialGraphDocument::SetParameter(const std::string& name, const MaterialGraph::ValueMap& props, std::string& why)
{
    std::vector<ParameterRow> rows = Parameters();
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const ParameterRow& r) { return r.name == name; });
    if (row == rows.end()) { why = "not_found: no parameter named '" + name + "'"; return false; }
    if (props.empty()) { why = "bad_value: nothing to set"; return false; }
    if (const auto it = props.find("name"); it != props.end())
    {
        if (it->second.kind != Value::Kind::String || it->second.str.empty())
        {
            why = "bad_value: props.name is a non-empty string";
            return false;
        }
        // Value parameters share one namespace, switches another.
        const bool isSwitch = row->type == "StaticSwitchParameter";
        for (const ParameterRow& r : rows)
            if (&r != &*row && r.name == it->second.str && (r.type == "StaticSwitchParameter") == isSwitch)
            {
                why = "param_name_duplicate: '" + it->second.str + "' is already a parameter (#" + std::to_string(r.nodes.front()) +
                      "): rename it first, or give a node that name on the canvas to make them one";
                return false;
            }
    }
    std::vector<GraphOp> ops;
    for (const std::uint32_t id : row->nodes)
    {
        GraphOp op;
        op.kind  = GraphOp::Kind::Set;
        op.node  = id;
        op.props = props;
        ops.push_back(std::move(op));
    }
    return Apply(ops, nullptr, why);
}

const MaterialGraph::CompileResult& MaterialGraphDocument::Compiled(const MaterialGraph::CompileOptions& o)
{
    const int sm = static_cast<int>(o.shadingModel), bm = static_cast<int>(o.blendMode);
    ++m_compileTick;
    // A function it calls, saved since (here or anywhere), recompiles it.
    MaterialGraph::TouchCalledFunctions(m_graph);
    const std::uint64_t epoch = MaterialGraph::FunctionEpoch();
    for (CompileSlot& s : m_compiled)
        if (s.revision == m_revision && s.shading == sm && s.blend == bm && s.label == o.sourceLabel &&
            s.previewNode == o.previewNode && s.previewPin == o.previewPin && s.switches == o.switches &&
            s.epoch == epoch)
        {
            s.used = m_compileTick;
            return s.result;
        }
    CompileSlot& s = m_compiled[0].used <= m_compiled[1].used ? m_compiled[0] : m_compiled[1];   // the older one
    s.result      = MaterialGraph::Compile(m_graph, o);
    s.revision    = m_revision;
    s.shading     = sm;
    s.blend       = bm;
    s.label       = o.sourceLabel;
    s.previewNode = o.previewNode;
    s.previewPin  = o.previewPin;
    s.switches    = o.switches;
    s.epoch       = MaterialGraph::FunctionEpoch();   // after: a nested function read now
    s.used        = m_compileTick;
    return s.result;
}

bool MaterialGraphDocument::Save(std::string& why)
{
    if (!MaterialGraph::SaveGraph(m_path, m_graph, why)) return false;
    m_savedText = MaterialGraph::SerializeGraph(m_graph);
    m_dirty = false;
    NoteDiskStamp();
    return true;
}

bool MaterialGraphDocument::SaveAs(const std::string& path, std::string& why)
{
    const std::string was = m_path;
    m_path = path;
    if (!Save(why))
    {
        m_path = was;
        return false;
    }
    return true;
}

bool MaterialGraphDocument::Revert(std::string& why)
{
    MaterialGraph::Graph g;
    if (!MaterialGraph::LoadGraph(m_path, g, why)) return false;
    m_savedText = MaterialGraph::SerializeGraph(g);
    Commit(std::move(g));
    NoteDiskStamp();
    return true;
}

void MaterialGraphDocument::NoteDiskStamp()
{
    std::error_code ec;
    const std::filesystem::path p = Sys::U8Path(m_path);
    m_diskSize = std::filesystem::file_size(p, ec);
    if (ec) m_diskSize = 0;
    const auto t = std::filesystem::last_write_time(p, ec);
    m_diskTime = ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

bool MaterialGraphDocument::ChangedOnDisk() const
{
    std::error_code ec;
    const std::filesystem::path p = Sys::U8Path(m_path);
    const std::uint64_t size = std::filesystem::file_size(p, ec);
    if (ec) return false;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    return size != m_diskSize || static_cast<std::int64_t>(t.time_since_epoch().count()) != m_diskTime;
}
}
