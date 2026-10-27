// MaterialGraphCanvas.cpp - see include/Editor/MaterialGraphCanvas.h.
#include "Editor/MaterialGraphCanvas.h"
#include "Editor/NodeGraphView.h"

#include "Editor/MaterialGraphDocument.h"
#include "MaterialGraph/MaterialGraphCompiler.h"
#include "MaterialGraph/MaterialGraphNodes.h"
#include "System/Log.h"

#include "imgui-node-editor/imgui_node_editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace ed = ax::NodeEditor;

namespace Editor
{
namespace
{
    using MaterialGraph::Link;
    using MaterialGraph::Node;
    using MaterialGraph::NodeDesc;
    using MaterialGraph::PinDesc;
    using MaterialGraph::Value;

    constexpr std::uint64_t kOutputBit = 1ull << 15;

    struct PinRef
    {
        std::uint32_t node = 0;
        int           index = 0;
        bool          output = false;
    };

    // Ids. A node or comment is its graph id; a pin packs (node, output?, index);
    // a link is named by the input it feeds (an input has one link).
    ed::PinId  PinOf(std::uint32_t node, int index, bool output)
    {
        return ed::PinId((static_cast<std::uint64_t>(node) << 16) | (output ? kOutputBit : 0) |
                         static_cast<std::uint64_t>(index));
    }

    PinRef Decode(std::uint64_t v)
    {
        return { static_cast<std::uint32_t>(v >> 16), static_cast<int>(v & 0x7FFF), (v & kOutputBit) != 0 };
    }
    ed::LinkId LinkOf(std::uint32_t toNode, int toIndex)
    {
        return ed::LinkId((static_cast<std::uint64_t>(toNode) << 16) | static_cast<std::uint64_t>(toIndex) | (1ull << 62));
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    ImVec4 CategoryColor(const std::string& cat)
    {
        if (cat == "Output")     return ImVec4(0.55f, 0.25f, 0.22f, 1.f);
        if (cat == "Constants")  return ImVec4(0.38f, 0.38f, 0.38f, 1.f);
        if (cat == "Parameters") return ImVec4(0.25f, 0.48f, 0.28f, 1.f);
        if (cat == "Texture")    return ImVec4(0.55f, 0.32f, 0.22f, 1.f);
        if (cat == "Math")       return ImVec4(0.25f, 0.38f, 0.58f, 1.f);
        if (cat == "Vector")     return ImVec4(0.38f, 0.30f, 0.58f, 1.f);
        if (cat == "World")      return ImVec4(0.55f, 0.45f, 0.20f, 1.f);
        if (cat == "Utility")    return ImVec4(0.22f, 0.48f, 0.48f, 1.f);
        if (cat == "Engine")     return ImVec4(0.55f, 0.28f, 0.50f, 1.f);
        if (cat == "Custom")     return ImVec4(0.60f, 0.22f, 0.30f, 1.f);
        if (cat == "Water")      return ImVec4(0.18f, 0.40f, 0.62f, 1.f);
        if (cat == "Scene")      return ImVec4(0.42f, 0.42f, 0.28f, 1.f);
        return ImVec4(0.35f, 0.35f, 0.35f, 1.f);
    }

    // A pin's colour is its component count's (0 = follows its inputs).
    ImU32 PinColor(int components)
    {
        switch (components)
        {
            case 1:  return IM_COL32(190, 190, 190, 255);
            case 2:  return IM_COL32(120, 210, 100, 255);
            case 3:  return IM_COL32(240, 210, 80, 255);
            case 4:  return IM_COL32(230, 115, 190, 255);
            default: return IM_COL32(245, 245, 245, 255);
        }
    }

    std::string FileName(const std::string& p)
    {
        const std::size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    std::string Num(double v)
    {
        return MaterialGraph::FormatNumber(v);
    }

    // The line under a node's title: what makes this node this one.
    std::string Summary(const Node& n)
    {
        const std::string t = n.type;
        if (t == "ScalarParameter" || t == "VectorParameter" || t == "TextureSampleParameter2D")
            return n.PropString("name", "?");
        if (t == "Constant") return Num(n.PropNumber("value", 0.0));
        if (t == "Constant2Vector" || t == "Constant3Vector" || t == "Constant4Vector")
        {
            std::string s;
            if (const Value* v = n.Prop("value"))
                for (int i = 0; i < v->Components(); ++i) s += (i ? ", " : "") + Num(v->At(i));
            return s;
        }
        if (t == "TextureSample") return FileName(n.PropString("texture", "(no texture)"));
        if (t == "TextureCoordinate") return "UV" + Num(n.PropNumber("index", 0.0));
        if (t == "ComponentMask")
        {
            std::string m;
            for (const char* c : { "r", "g", "b", "a" })
                if (n.PropBool(c, false)) m += static_cast<char>(std::toupper(static_cast<unsigned char>(c[0])));
            return m.empty() ? "(none)" : m;
        }
        if (t == "Transform") return n.PropString("source", "Tangent") + " -> " + n.PropString("dest", "World");
        if (t == "NamedRerouteDeclaration" || t == "NamedRerouteUsage") return n.PropString("name", "?");
        if (t == "Sine" || t == "Cosine") return "period " + Num(n.PropNumber("period", 1.0));
        if (t == "Rotator") return "speed " + Num(n.PropNumber("speed", 0.25));
        if (t == "SceneTexture") return n.PropString("sceneTexture", "SceneDepth");
        if (t == "Custom")
        {
            const std::string desc = n.PropString("description");
            if (!desc.empty()) return desc;
            std::string code = n.PropString("code", MaterialGraph::kCustomDefaultCode);
            code = code.substr(0, code.find('\n'));
            return code.size() > 28 ? code.substr(0, 27) + "..." : code;
        }
        return {};
    }

    // A Named Reroute's colour: its declaration's props.color (a usage looks its name up).
    bool RerouteColor(const MaterialGraph::Graph& g, const Node& n, ImVec4& out)
    {
        if (n.type != "NamedRerouteDeclaration" && n.type != "NamedRerouteUsage") return false;
        const Node* decl = &n;
        if (n.type == "NamedRerouteUsage")
        {
            decl = nullptr;
            for (const Node& d : g.nodes)
                if (d.type == "NamedRerouteDeclaration" && d.PropString("name") == n.PropString("name")) decl = &d;
            if (!decl) return false;
        }
        const Value* c = decl->Prop("color");
        if (!c || !c->IsNumeric() || c->Components() < 3) out = ImVec4(0.25f, 0.55f, 0.95f, 1.f);
        else out = ImVec4(static_cast<float>(c->At(0)), static_cast<float>(c->At(1)), static_cast<float>(c->At(2)), 1.f);
        return true;
    }

    // An unconnected input's current value (its stored one, else the pin's default).
    int InputValue(const Node& n, const PinDesc& pin, float out[4])
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        const auto it = n.inputs.find(pin.name);
        if (it != n.inputs.end() && it->second.IsNumeric() && it->second.Components() >= 1 && it->second.Components() <= 4)
        {
            for (int i = 0; i < it->second.Components(); ++i) out[i] = static_cast<float>(it->second.At(i));
            return it->second.Components();
        }
        for (int i = 0; i < pin.defCount; ++i) out[i] = pin.def[i];
        return pin.defCount;
    }

    Value NumbersValue(const float* v, int n)
    {
        if (n == 1) return Value::Number(v[0]);
        std::vector<double> d;
        for (int i = 0; i < n; ++i) d.push_back(v[i]);
        return Value::Numbers(std::move(d));
    }

    // A pin's circle, filled when linked.
    void PinIcon(bool linked, ImU32 color)
    {
        const float r = ImGui::GetTextLineHeight() * 0.3f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(r * 2.0f + 2.0f, ImGui::GetTextLineHeight()));
        const ImVec2 c(p.x + r + 1.0f, p.y + ImGui::GetTextLineHeight() * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(color));   // the style's alpha (Hide Unrelated)
        if (linked) dl->AddCircleFilled(c, r, col);
        else        dl->AddCircle(c, r, col, 0, 1.5f);
    }

    // A unique parameter name for a node added from the palette: "Scalar", "Scalar1", ...
    std::string UniqueParamName(const MaterialGraphDocument& doc, const std::string& base)
    {
        std::set<std::string> used;
        for (const Node& n : doc.Graph().nodes)
            if (const Value* v = n.Prop("name"); v && v->kind == Value::Kind::String) used.insert(v->str);
        if (!used.count(base)) return base;
        for (int i = 1;; ++i)
            if (!used.count(base + std::to_string(i))) return base + std::to_string(i);
    }
}

MaterialGraphCanvas::MaterialGraphCanvas() = default;

MaterialGraphCanvas::~MaterialGraphCanvas()
{
    Shutdown();
}

void MaterialGraphCanvas::Shutdown()
{
    NodeGraphView::DestroyContext(m_ctx);
    m_ctx = nullptr;
}

bool MaterialGraphCanvas::TakePreviewRequest(std::uint32_t& node, std::string& pin)
{
    if (!m_previewRequested) return false;
    m_previewRequested = false;
    node = m_previewRequestNode;
    pin  = m_previewRequestPin;
    return true;
}

void MaterialGraphCanvas::SyncPositions(const MaterialGraphDocument& doc)
{
    // Only what the model changed (an op, an undo): a drag in progress is never fought.
    std::unordered_map<std::uint32_t, ImVec2> pushed;
    for (const Node& n : doc.Graph().nodes)
    {
        const ImVec2 p(n.pos[0], n.pos[1]);
        const auto it = m_pushedPos.find(n.id);
        if (it == m_pushedPos.end() || it->second.x != p.x || it->second.y != p.y)
            ed::SetNodePosition(ed::NodeId(static_cast<std::uint64_t>(n.id)), p);
        pushed[n.id] = p;
    }
    std::unordered_map<std::uint32_t, ImVec2> sizes;
    for (const MaterialGraph::Comment& c : doc.Graph().comments)
    {
        const ImVec2 p(c.rect[0], c.rect[1]);
        const ImVec2 s(std::max(40.0f, c.rect[2] - c.rect[0]), std::max(30.0f, c.rect[3] - c.rect[1]));
        const auto it = m_pushedPos.find(c.id);
        if (it == m_pushedPos.end() || it->second.x != p.x || it->second.y != p.y)
            ed::SetNodePosition(ed::NodeId(static_cast<std::uint64_t>(c.id)), p);
        const auto sit = m_pushedSize.find(c.id);
        if (sit == m_pushedSize.end() || sit->second.x != s.x || sit->second.y != s.y)
        {
            ed::SetGroupSize(ed::NodeId(static_cast<std::uint64_t>(c.id)), s);
            m_commentFrame.erase(c.id);   // re-measure the title + padding around it
        }
        pushed[c.id] = p;
        sizes[c.id]  = s;
    }
    m_pushedPos  = std::move(pushed);
    m_pushedSize = std::move(sizes);
    m_seenRevision = doc.Revision();
}

void MaterialGraphCanvas::Draw(MaterialGraphDocument& doc, const MaterialGraph::CompileResult& compile,
                               const std::string& logName)
{
    if (!m_ctx)
    {
        m_ctx = NodeGraphView::CreateContext();
    }
    ed::SetCurrentEditor(static_cast<ed::EditorContext*>(m_ctx));
    const MaterialGraph::Graph& g = doc.Graph();

    // Diagnostics by node: the worst severity and the messages.
    std::map<std::uint32_t, std::pair<int, std::string>> diag;   // node -> (2 error / 1 warning, text)
    for (const MaterialGraph::Diagnostic& d : compile.diagnostics)
    {
        if (!d.node || d.severity == MaterialGraph::Severity::Info) continue;
        auto& e = diag[d.node];
        e.first = std::max(e.first, d.severity == MaterialGraph::Severity::Error ? 2 : 1);
        e.second += (e.second.empty() ? "" : "\n") + std::string(MaterialGraph::SeverityName(d.severity)) + ": " +
                    (d.pin.empty() ? "" : d.pin + ": ") + d.message;
    }
    std::set<std::pair<std::uint32_t, std::string>> linkedInputs;
    std::set<std::pair<std::uint32_t, std::string>> linkedOutputs;
    for (const Link& l : g.links)
    {
        linkedInputs.insert({ l.toNode, l.toPin });
        linkedOutputs.insert({ l.fromNode, l.fromPin });
    }

    std::vector<GraphOp> ops;           // this frame's edits: one batch at the end
    auto setInput = [&](std::uint32_t node, const std::string& pin, const float* v, int n)
    {
        GraphOp op;
        op.kind = GraphOp::Kind::Set;
        op.node = node;
        op.inputs[pin] = NumbersValue(v, n);
        ops.push_back(std::move(op));
    };

    ed::Begin("materialgraph", ImVec2(0.0f, 0.0f));
    if (m_seenRevision != doc.Revision()) SyncPositions(doc);

    // ---- Comments (behind the nodes) ----
    for (const MaterialGraph::Comment& c : g.comments)
    {
        const ed::NodeId id(static_cast<std::uint64_t>(c.id));
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(c.color[0], c.color[1], c.color[2], std::max(0.15f, c.color[3] * 0.6f)));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(c.color[0], c.color[1], c.color[2], 0.9f));
        ed::BeginNode(id);
        ImGui::PushID(static_cast<int>(c.id));
        ImGui::TextUnformatted(c.text.empty() ? "Comment" : c.text.c_str());
        const auto sit = m_pushedSize.find(c.id);
        ed::Group(sit != m_pushedSize.end() ? sit->second : ImVec2(200, 100));
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleColor(2);
    }

    // ---- Hide Unrelated: what the selection neither reads nor feeds is dimmed ----
    std::set<std::uint32_t> related;
    const bool dimming = m_hideUnrelated && !m_selection.empty();
    if (dimming)
        for (const std::uint32_t id : doc.Related(m_selection)) related.insert(id);
    auto dimmed = [&](std::uint32_t id) { return dimming && !related.count(id); };

    // ---- Nodes ----
    for (const Node& n : g.nodes)
    {
        const NodeDesc* d = MaterialGraph::DescribeNode(n);   // a Custom node's pins are its props'
        const ed::NodeId id(static_cast<std::uint64_t>(n.id));
        const auto dg = diag.find(n.id);
        const int severity = dg != diag.end() ? dg->second.first : 0;
        const bool dim = dimmed(n.id);
        const bool previewed = m_previewNode != 0 && n.id == m_previewNode;
        int pushedColors = 0;
        if (severity) { ed::PushStyleColor(ed::StyleColor_NodeBorder, severity == 2 ? ImVec4(1.f, 0.25f, 0.2f, 1.f)
                                                                                   : ImVec4(1.f, 0.8f, 0.25f, 1.f)); ++pushedColors; }
        else if (previewed) { ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.35f, 0.85f, 1.f, 1.f)); ++pushedColors; }
        if (dim)
        {
            const ImVec4 bg = ed::GetStyle().Colors[ed::StyleColor_NodeBg];
            ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(bg.x, bg.y, bg.z, bg.w * 0.25f));
            ++pushedColors;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f);
        }
        ed::BeginNode(id);
        ImGui::PushID(static_cast<int>(n.id));
        const float titleTop = ImGui::GetCursorScreenPos().y;
        ImGui::TextUnformatted(d ? d->label : n.type.c_str());
        if (previewed)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.f, 1.f), "[previewing]");
        }
        const std::string sum = Summary(n);
        if (!sum.empty()) ImGui::TextDisabled("%s", sum.c_str());
        const float titleBottom = ImGui::GetCursorScreenPos().y;
        // A texture node shows its texture: a grey square of the same size while it streams in,
        // so the node never grows when it lands.
        if (m_thumbnails && (n.type == "TextureSample" || n.type == "TextureSampleParameter2D"))
        {
            const std::string tex = n.PropString(n.type == "TextureSample" ? "texture" : "default");
            const std::uint64_t srv = m_thumbnails(tex);
            const ImVec2 size(kThumbnailSize, kThumbnailSize);
            if (srv) ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), size);
            else
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(size);
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                                                          ImGui::GetColorU32(ImVec4(0.16f, 0.16f, 0.16f, 1.f)));
            }
        }
        if (!d)
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.35f, 1.f), "unknown type");
        else
        {
            ImGui::BeginGroup();
            for (int i = 0; i < static_cast<int>(d->inputs.size()); ++i)
            {
                const PinDesc& pin = d->inputs[static_cast<std::size_t>(i)];
                if (pin.hidden) continue;   // the compiler links it (a Named Reroute Usage's)
                const bool linked = linkedInputs.count({ n.id, pin.name }) != 0;
                ed::BeginPin(PinOf(n.id, i, false), ed::PinKind::Input);
                ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
                PinIcon(linked, PinColor(pin.components));
                ImGui::SameLine();
                ImGui::TextUnformatted(pin.label);
                ed::EndPin();
                if (!linked && pin.defCount > 0)
                {
                    // The unconnected input's value, edited in place (a drag is one undo step).
                    float v[4];
                    const int count = InputValue(n, pin, v);
                    ImGui::SameLine();
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(46.0f * static_cast<float>(count));
                    if (ImGui::DragScalarN("##v", ImGuiDataType_Float, v, count, 0.01f, nullptr, nullptr, "%.3g"))
                        setInput(n.id, pin.name, v, count);
                    ImGui::PopID();
                }
            }
            ImGui::EndGroup();
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::BeginGroup();
            for (int i = 0; i < static_cast<int>(d->outputs.size()); ++i)
            {
                const PinDesc& pin = d->outputs[static_cast<std::size_t>(i)];
                ed::BeginPin(PinOf(n.id, i, true), ed::PinKind::Output);
                ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
                ImGui::TextUnformatted(pin.label);
                ImGui::SameLine();
                PinIcon(linkedOutputs.count({ n.id, pin.name }) != 0, PinColor(pin.components));
                ed::EndPin();
            }
            ImGui::EndGroup();
        }
        ImGui::PopID();
        ed::EndNode();
        if (dim) ImGui::PopStyleVar();
        if (pushedColors) ed::PopStyleColor(pushedColors);
        // The title band in the category's colour (under the text: the background list);
        // a Named Reroute's is its declaration's colour.
        if (d)
        {
            const ImVec2 pos  = ed::GetNodePosition(id);
            const ImVec2 size = ed::GetNodeSize(id);
            if (size.x > 0.0f)
            {
                ImVec4 c = CategoryColor(d->category);
                RerouteColor(g, n, c);
                const float  pad = ed::GetStyle().NodePadding.y;
                ed::GetNodeBackgroundDrawList(id)->AddRectFilled(
                    ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + size.x - 1.0f, pos.y + pad + (titleBottom - titleTop)),
                    ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, dim ? 0.2f : 0.85f)), ed::GetStyle().NodeRounding,
                    ImDrawFlags_RoundCornersTop);
            }
        }
    }

    // ---- Links ----
    int drawnLinks = 0;
    for (const Link& l : g.links)
    {
        const Node* a = g.FindNode(l.fromNode);
        const Node* b = g.FindNode(l.toNode);
        const NodeDesc* da = a ? MaterialGraph::DescribeNode(*a) : nullptr;
        const NodeDesc* db = b ? MaterialGraph::DescribeNode(*b) : nullptr;
        if (!da || !db) continue;
        const int fi = da->OutputIndex(l.fromPin), ti = db->InputIndex(l.toPin);
        if (fi < 0 || ti < 0 || db->inputs[static_cast<std::size_t>(ti)].hidden) continue;
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(PinColor(da->outputs[static_cast<std::size_t>(fi)].components));
        if (dimmed(l.fromNode) || dimmed(l.toNode)) col.w *= 0.2f;
        ed::Link(LinkOf(l.toNode, ti), PinOf(l.fromNode, fi, true), PinOf(l.toNode, ti, false), col, 2.0f);
        ++drawnLinks;
    }

    // ---- Create: drag a link ----
    auto refuse = [](const char* why)
    {
        ed::RejectNewItem(ImVec4(1.f, 0.35f, 0.3f, 1.f), 2.0f);
        ed::Suspend();
        ImGui::SetTooltip("%s", why);
        ed::Resume();
    };
    if (ed::BeginCreate(ImVec4(1.f, 1.f, 1.f, 1.f), 2.0f))
    {
        ed::PinId pa, pb;
        if (ed::QueryNewLink(&pa, &pb) && pa.Get() && pb.Get())
        {
            PinRef a = Decode(pa.Get()), b = Decode(pb.Get());
            if (!a.output) std::swap(a, b);            // a = the output end
            const Node* na = g.FindNode(a.node);
            const Node* nb = g.FindNode(b.node);
            const NodeDesc* da = na ? MaterialGraph::DescribeNode(*na) : nullptr;
            const NodeDesc* db = nb ? MaterialGraph::DescribeNode(*nb) : nullptr;
            if (!da || !db || a.output == b.output)       refuse("an output connects to an input");
            else if (a.index >= static_cast<int>(da->outputs.size()) || b.index >= static_cast<int>(db->inputs.size()) ||
                     db->inputs[static_cast<std::size_t>(b.index)].hidden)
                refuse("that pin takes no link");
            else if (a.node == b.node)                    refuse("a node cannot feed itself");
            else
            {
                // Would it close a cycle? (Is a downstream of b?)
                bool cycle = false;
                {
                    std::vector<std::uint32_t> stack{ b.node };
                    std::set<std::uint32_t> seen;
                    while (!stack.empty() && !cycle)
                    {
                        const std::uint32_t x = stack.back();
                        stack.pop_back();
                        if (x == a.node) cycle = true;
                        if (!seen.insert(x).second) continue;
                        for (const Link& l : g.links)
                            if (l.fromNode == x) stack.push_back(l.toNode);
                    }
                }
                const int fromN = da->outputs[static_cast<std::size_t>(a.index)].components;
                const int toN   = db->inputs[static_cast<std::size_t>(b.index)].components;
                if (cycle) refuse("that link would close a cycle");
                else if (fromN > 1 && toN > 0 && fromN < toN) refuse("a vector does not widen (only a scalar does)");
                else if (ed::AcceptNewItem(ImVec4(0.45f, 0.95f, 0.45f, 1.f), 3.0f))
                {
                    GraphOp op;
                    op.kind    = GraphOp::Kind::Connect;
                    op.from    = a.node;
                    op.fromPin = da->outputs[static_cast<std::size_t>(a.index)].name;
                    op.to      = b.node;
                    op.toPin   = db->inputs[static_cast<std::size_t>(b.index)].name;
                    ops.push_back(std::move(op));
                }
            }
        }
        ed::PinId from;
        if (ed::QueryNewNode(&from) && from.Get() && ed::AcceptNewItem())
        {
            // Dropped on empty space: the palette, and the node it adds is connected.
            m_paletteFromPin = from.Get();
            m_paletteAt      = ed::ScreenToCanvas(ImGui::GetMousePos());
            m_openPalette    = true;
        }
    }
    ed::EndCreate();

    // ---- Delete ----
    if (ed::BeginDelete())
    {
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid))
            if (ed::AcceptDeletedItem())
            {
                const std::uint64_t v = lid.Get() & ~(1ull << 62);
                const std::uint32_t to = static_cast<std::uint32_t>(v >> 16);
                const Node* nb = g.FindNode(to);
                const NodeDesc* db = nb ? MaterialGraph::DescribeNode(*nb) : nullptr;
                const int ti = static_cast<int>(v & 0xFFFF);
                if (db && ti < static_cast<int>(db->inputs.size()))
                {
                    GraphOp op;
                    op.kind  = GraphOp::Kind::Disconnect;
                    op.to    = to;
                    op.toPin = db->inputs[static_cast<std::size_t>(ti)].name;
                    ops.push_back(std::move(op));
                }
            }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid))
        {
            const std::uint32_t id = static_cast<std::uint32_t>(nid.Get());
            if (id == doc.OutputId()) { ed::RejectDeletedItem(); continue; }
            if (ed::AcceptDeletedItem(false))
            {
                GraphOp op;
                op.kind = GraphOp::Kind::Remove;
                op.node = id;
                ops.push_back(std::move(op));
            }
        }
    }
    ed::EndDelete();
    // A link removed because its node went: the remove takes it; drop the disconnects
    // that no longer find their link (the batch is all or nothing).
    {
        std::set<std::uint32_t> removed;
        for (const GraphOp& op : ops) if (op.kind == GraphOp::Kind::Remove) removed.insert(op.node);
        if (!removed.empty())
            ops.erase(std::remove_if(ops.begin(), ops.end(), [&](const GraphOp& op) {
                          if (op.kind != GraphOp::Kind::Disconnect) return false;
                          if (removed.count(op.to)) return true;
                          for (const Link& l : g.links)
                              if (l.toNode == op.to && l.toPin == op.toPin && removed.count(l.fromNode)) return true;
                          return false;
                      }), ops.end());
    }

    // ---- Double-click a link: a Reroute in it ----
    if (const ed::LinkId dl = ed::GetDoubleClickedLink(); dl.Get())
    {
        const std::uint64_t v = dl.Get() & ~(1ull << 62);
        const std::uint32_t to = static_cast<std::uint32_t>(v >> 16);
        const int ti = static_cast<int>(v & 0xFFFF);
        const Node* nb = g.FindNode(to);
        const NodeDesc* db = nb ? MaterialGraph::DescribeNode(*nb) : nullptr;
        if (db && ti < static_cast<int>(db->inputs.size()))
            if (const Link* l = g.FindInputLink(to, db->inputs[static_cast<std::size_t>(ti)].name))
            {
                const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());
                GraphOp add;
                add.kind = GraphOp::Kind::Add;
                add.type = "Reroute";
                add.as   = "reroute";
                add.pos[0] = at.x;
                add.pos[1] = at.y;
                GraphOp in;
                in.kind = GraphOp::Kind::Connect;
                in.from = l->fromNode;
                in.fromPin = l->fromPin;
                in.toRef = "reroute";
                in.toPin = "Input";
                GraphOp out;
                out.kind = GraphOp::Kind::Connect;
                out.fromRef = "reroute";
                out.fromPin = "Out";
                out.to = to;
                out.toPin = l->toPin;
                ops.push_back(std::move(add));
                ops.push_back(std::move(in));
                ops.push_back(std::move(out));
            }
    }

    // ---- Shortcuts: the node editor's cut / copy / paste / duplicate ----
    std::string clipboardOut;
    bool paste = false, duplicate = false;
    std::vector<std::uint32_t> cutIds;
    if (ed::BeginShortcut())
    {
        std::vector<ed::NodeId> sel(static_cast<std::size_t>(std::max(0, ed::GetActionContextSize())));
        const int count = sel.empty() ? 0 : ed::GetActionContextNodes(sel.data(), static_cast<int>(sel.size()));
        std::vector<std::uint32_t> ids;
        for (int i = 0; i < count; ++i) ids.push_back(static_cast<std::uint32_t>(sel[static_cast<std::size_t>(i)].Get()));
        if (ed::AcceptCopy()) clipboardOut = doc.CopyText(ids);
        if (ed::AcceptCut())
        {
            clipboardOut = doc.CopyText(ids);
            cutIds = ids;
        }
        if (ed::AcceptPaste()) paste = true;
        if (ed::AcceptDuplicate())
        {
            clipboardOut = doc.CopyText(ids);
            duplicate = true;
        }
        if (ed::AcceptCreateNode())
        {
            m_paletteFromPin = 0;
            m_paletteAt      = ed::ScreenToCanvas(ImGui::GetMousePos());
            m_openPalette    = true;
        }
    }
    ed::EndShortcut();
    for (const std::uint32_t id : cutIds)
        if (id != doc.OutputId())
        {
            GraphOp op;
            op.kind = GraphOp::Kind::Remove;
            op.node = id;
            ops.push_back(std::move(op));
        }

    // ---- Requests (Lua, the harness, the Code tab) ----
    if (m_hasSelect)
    {
        ed::ClearSelection();
        for (const std::uint32_t id : m_select) ed::SelectNode(ed::NodeId(static_cast<std::uint64_t>(id)), true);
        m_hasSelect = false;
    }
    ++m_draws;
    if (m_draws == 2) m_fit = true;     // the first frame measured the nodes: frame them all
    if (m_fit) { ed::NavigateToContent(0.0f); m_fit = false; }
    if (m_frameSelection) { ed::NavigateToSelection(false, 0.0f); m_frameSelection = false; }

    // ---- Context menus ----
    ed::Suspend();
    ed::NodeId ctxNode;
    ed::LinkId ctxLink;
    if (ed::ShowBackgroundContextMenu())
    {
        m_paletteFromPin = 0;
        m_paletteAt      = ed::ScreenToCanvas(ImGui::GetMousePos());
        m_openPalette    = true;
    }
    else if (ed::ShowNodeContextMenu(&ctxNode))
    {
        m_contextNode = ctxNode.Get();
        ImGui::OpenPopup("mg_node");
    }
    else if (ed::ShowLinkContextMenu(&ctxLink))
    {
        m_contextLink = ctxLink.Get();
        ImGui::OpenPopup("mg_link");
    }
    if (m_openPalette)
    {
        ImGui::OpenPopup("mg_palette");
        m_openPalette = false;
    }
    DrawPalettePopup(doc);
    if (ImGui::BeginPopup("mg_node"))
    {
        const std::uint32_t id = static_cast<std::uint32_t>(m_contextNode);
        ImGui::TextDisabled("node #%u", id);
        if (ImGui::MenuItem("Delete", nullptr, false, id != doc.OutputId()))
        {
            GraphOp op;
            op.kind = GraphOp::Kind::Remove;
            op.node = id;
            ops.push_back(std::move(op));
        }
        if (ImGui::MenuItem("Duplicate", nullptr, false, id != doc.OutputId()))
        {
            clipboardOut = doc.CopyText({ id });
            duplicate = true;
        }
        // Unreal's "Start Previewing Node": the preview shows this output, unlit.
        ImGui::Separator();
        const Node* cn = g.FindNode(id);
        const NodeDesc* cd = cn ? MaterialGraph::DescribeNode(*cn) : nullptr;
        if (m_previewNode == id)
        {
            if (ImGui::MenuItem("Stop Previewing Node"))
            {
                m_previewRequested   = true;
                m_previewRequestNode = 0;
                m_previewRequestPin.clear();
            }
        }
        else if (cd && id != doc.OutputId() && !cd->outputs.empty())
        {
            auto request = [&](const char* pin) {
                m_previewRequested   = true;
                m_previewRequestNode = id;
                m_previewRequestPin  = pin;
            };
            if (cd->outputs.size() == 1)
            {
                if (ImGui::MenuItem("Start Previewing Node")) request(cd->outputs.front().name);
            }
            else if (ImGui::BeginMenu("Start Previewing Node"))
            {
                for (const PinDesc& p : cd->outputs)
                    if (ImGui::MenuItem(p.label)) request(p.name);
                ImGui::EndMenu();
            }
        }
        if (m_previewNode && m_previewNode != id && ImGui::MenuItem("Stop Previewing (the material again)"))
        {
            m_previewRequested   = true;
            m_previewRequestNode = 0;
            m_previewRequestPin.clear();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("mg_link"))
    {
        if (ImGui::MenuItem("Delete"))
        {
            const std::uint64_t v = m_contextLink & ~(1ull << 62);
            const std::uint32_t to = static_cast<std::uint32_t>(v >> 16);
            const Node* nb = g.FindNode(to);
            const NodeDesc* db = nb ? MaterialGraph::DescribeNode(*nb) : nullptr;
            const int ti = static_cast<int>(v & 0xFFFF);
            if (db && ti < static_cast<int>(db->inputs.size()))
            {
                GraphOp op;
                op.kind  = GraphOp::Kind::Disconnect;
                op.to    = to;
                op.toPin = db->inputs[static_cast<std::size_t>(ti)].name;
                ops.push_back(std::move(op));
            }
        }
        ImGui::EndPopup();
    }
    // A node's diagnostics on hover.
    if (const ed::NodeId hov = ed::GetHoveredNode(); hov.Get())
        if (const auto it = diag.find(static_cast<std::uint32_t>(hov.Get())); it != diag.end())
            ImGui::SetTooltip("%s", it->second.second.c_str());
    ed::Resume();

    // ---- Keys over the canvas: F frames the selection, Home everything, C comments it ----
    m_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const bool typing = ImGui::GetIO().WantTextInput;
    if (m_hovered && !typing)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) ed::NavigateToSelection(false, 0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) ed::NavigateToContent(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !ImGui::GetIO().KeyCtrl && !m_selection.empty())
        {
            ImVec2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
            for (const std::uint32_t id : m_selection)
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(static_cast<std::uint64_t>(id)));
                const ImVec2 s = ed::GetNodeSize(ed::NodeId(static_cast<std::uint64_t>(id)));
                lo = ImVec2(std::min(lo.x, p.x), std::min(lo.y, p.y));
                hi = ImVec2(std::max(hi.x, p.x + s.x), std::max(hi.y, p.y + s.y));
            }
            GraphOp op;
            op.kind = GraphOp::Kind::Comment;
            op.hasRect = true;
            op.rect[0] = lo.x - 24.0f; op.rect[1] = lo.y - 48.0f;
            op.rect[2] = hi.x + 24.0f; op.rect[3] = hi.y + 24.0f;
            op.text = "Comment";
            op.hasText = true;
            ops.push_back(std::move(op));
        }
    }

    ed::End();

    // ---- After the frame: drags ended, selection, zoom ----
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        for (const Node& n : g.nodes)
        {
            const ImVec2 p = ed::GetNodePosition(ed::NodeId(static_cast<std::uint64_t>(n.id)));
            if (std::isfinite(p.x) && (std::fabs(p.x - n.pos[0]) > 0.01f || std::fabs(p.y - n.pos[1]) > 0.01f))
            {
                GraphOp op;
                op.kind = GraphOp::Kind::Move;
                op.node = n.id;
                op.pos[0] = std::round(p.x);
                op.pos[1] = std::round(p.y);
                op.hasPos = true;
                ops.push_back(std::move(op));
            }
        }
        // A comment's rect is its position and its GROUP area (the box under its title): the
        // node's full size is that plus the title and padding, and the difference is measured once
        // per push, so a resize reads back exactly.
        for (const MaterialGraph::Comment& c : g.comments)
        {
            const ed::NodeId id(static_cast<std::uint64_t>(c.id));
            const ImVec2 p = ed::GetNodePosition(id);
            const ImVec2 s = ed::GetNodeSize(id);
            const auto pushed = m_pushedSize.find(c.id);
            if (!std::isfinite(p.x) || s.x <= 0.0f || pushed == m_pushedSize.end()) continue;
            auto frame = m_commentFrame.find(c.id);
            if (frame == m_commentFrame.end())
            {
                m_commentFrame[c.id] = ImVec2(s.x - pushed->second.x, s.y - pushed->second.y);
                continue;
            }
            const ImVec2 group(s.x - frame->second.x, s.y - frame->second.y);
            const float r[4] = { std::round(p.x), std::round(p.y), std::round(p.x + group.x), std::round(p.y + group.y) };
            if (std::fabs(r[0] - c.rect[0]) > 0.5f || std::fabs(r[1] - c.rect[1]) > 0.5f ||
                std::fabs(r[2] - c.rect[2]) > 0.5f || std::fabs(r[3] - c.rect[3]) > 0.5f)
            {
                GraphOp op;
                op.kind = GraphOp::Kind::SetComment;
                op.node = c.id;
                op.hasRect = true;
                std::copy(r, r + 4, op.rect);
                ops.push_back(std::move(op));
            }
        }
    }
    {
        const int count = ed::GetSelectedObjectCount();
        std::vector<ed::NodeId> sel(static_cast<std::size_t>(std::max(0, count)));
        const int got = count > 0 ? ed::GetSelectedNodes(sel.data(), count) : 0;
        m_selection.clear();
        for (int i = 0; i < got; ++i) m_selection.push_back(static_cast<std::uint32_t>(sel[static_cast<std::size_t>(i)].Get()));
        std::sort(m_selection.begin(), m_selection.end());
    }
    const float inv = ed::GetCurrentZoom();
    m_scale = inv > 0.0f ? 1.0f / inv : 1.0f;
    ed::SetCurrentEditor(nullptr);

    // ---- Apply this frame's edits: one batch ----
    if (!clipboardOut.empty() && !duplicate) ImGui::SetClipboardText(clipboardOut.c_str());
    if (!ops.empty())
    {
        std::string why;
        if (!doc.Apply(ops, nullptr, why)) LOG_WARNING("[MATGRAPH] canvas edit refused: %s", why.c_str());
    }
    if (paste || duplicate)
    {
        const std::string text = duplicate ? clipboardOut : std::string(ImGui::GetClipboardText() ? ImGui::GetClipboardText() : "");
        const float offset[2] = { 40.0f, 40.0f };
        std::vector<std::uint32_t> ids;
        std::string why;
        if (doc.Paste(text, offset, &ids, why)) RequestSelect(ids);
        else LOG_WARNING("[MATGRAPH] paste: %s", why.c_str());
    }

    // ---- Stats and the log line (when what is drawn changes) ----
    m_stats = {};
    m_stats.nodes    = static_cast<int>(g.nodes.size());
    m_stats.links    = drawnLinks;
    m_stats.comments = static_cast<int>(g.comments.size());
    for (const auto& [id, e] : diag) (e.first == 2 ? m_stats.errors : m_stats.warnings)++;
    char line[256];
    std::snprintf(line, sizeof(line), "nodes=%d links=%d comments=%d errors=%d warnings=%d", m_stats.nodes, m_stats.links,
                  m_stats.comments, m_stats.errors, m_stats.warnings);
    if (m_logged != line)
    {
        m_logged = line;
        LOG_INFO("[MATGRAPH] canvas %s: %s", logName.c_str(), line);
    }
}

void MaterialGraphCanvas::DrawPalettePopup(MaterialGraphDocument& doc)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 0), ImVec2(320, 480));
    if (!ImGui::BeginPopup("mg_palette")) return;
    if (ImGui::IsWindowAppearing())
    {
        m_search[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search nodes", m_search, sizeof(m_search));
    const std::string q = Lower(m_search);

    // A wire dragged into empty space offers only the node types that can take it (or feed
    // it), each connected on its first compatible pin.
    std::vector<MaterialGraphDocument::PaletteEntry> entries;
    if (m_paletteFromPin)
    {
        const PinRef from = Decode(m_paletteFromPin);
        const Node* n = doc.Graph().FindNode(from.node);
        const NodeDesc* nd = n ? MaterialGraph::DescribeNode(*n) : nullptr;
        const auto& pins = nd ? (from.output ? nd->outputs : nd->inputs) : std::vector<PinDesc>{};
        if (nd && from.index < static_cast<int>(pins.size()))
        {
            const PinDesc& p = pins[static_cast<std::size_t>(from.index)];
            entries = MaterialGraphDocument::CompatibleNodes(p.components, from.output);
            ImGui::TextDisabled("%s %s.%s (%s)", from.output ? "Takes" : "Feeds", nd->label, p.label,
                                p.components ? (p.components == 1 ? "float" : ("float" + std::to_string(p.components)).c_str())
                                             : "any size");
        }
    }
    else
        for (const NodeDesc& d : MaterialGraph::NodeRegistry())
            if (!(d.flags & MaterialGraph::kNodeOutput)) entries.push_back({ &d, std::string() });

    ImGui::BeginChild("##palette", ImVec2(260, 360), ImGuiChildFlags_None);
    std::string lastCategory;
    for (const MaterialGraphDocument::PaletteEntry& e : entries)
    {
        const NodeDesc& d = *e.desc;
        if (!q.empty() && Lower(d.label).find(q) == std::string::npos && Lower(d.type).find(q) == std::string::npos)
            continue;
        if (q.empty() && lastCategory != d.category)
        {
            lastCategory = d.category;
            ImGui::SeparatorText(d.category);
        }
        if (ImGui::Selectable(d.label))
        {
            AddFromPalette(doc, d.type, e.pin);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s", d.help, e.pin.empty() ? "" : "\n\nConnects on: ", e.pin.c_str());
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void MaterialGraphCanvas::AddFromPalette(MaterialGraphDocument& doc, const char* type, const std::string& newPin)
{
    const NodeDesc* d = MaterialGraph::FindNodeDesc(type);
    if (!d) return;
    GraphOp add;
    add.kind   = GraphOp::Kind::Add;
    add.type   = type;
    add.as     = "new";
    add.pos[0] = std::round(m_paletteAt.x);
    add.pos[1] = std::round(m_paletteAt.y);
    for (const MaterialGraph::PropDesc& p : d->props)
    {
        using K = MaterialGraph::PropDesc::Kind;
        switch (p.kind)
        {
        case K::Number:  add.props[p.name] = Value::Number(p.def[0]); break;
        case K::Numbers: add.props[p.name] = NumbersValue(p.def, p.count); break;
        case K::Bool:    add.props[p.name] = Value::Bool(p.defBool); break;
        default:
            if (std::string(p.name) == "name")
            {
                const std::string t = type;
                add.props["name"] = Value::String(UniqueParamName(doc, t == "ScalarParameter" ? "Scalar"
                                                                       : t == "VectorParameter" ? "Color" : "Texture"));
            }
            else if (p.defText[0]) add.props[p.name] = Value::String(p.defText);
            break;
        }
    }
    std::vector<GraphOp> ops{ add };
    if (m_paletteFromPin)
    {
        const PinRef pin = Decode(m_paletteFromPin);
        const Node* other = doc.Graph().FindNode(pin.node);
        const NodeDesc* od = other ? MaterialGraph::DescribeNode(*other) : nullptr;
        GraphOp c;
        c.kind = GraphOp::Kind::Connect;
        // The pin the palette chose (the first compatible one); else the first visible one.
        const auto firstInput = std::find_if(d->inputs.begin(), d->inputs.end(), [](const PinDesc& p) { return !p.hidden; });
        if (od && pin.output && (!newPin.empty() || firstInput != d->inputs.end()) &&
            pin.index < static_cast<int>(od->outputs.size()))
        {
            c.from    = pin.node;
            c.fromPin = od->outputs[static_cast<std::size_t>(pin.index)].name;
            c.toRef   = "new";
            c.toPin   = !newPin.empty() ? newPin : std::string(firstInput->name);
            ops.push_back(c);
        }
        else if (od && !pin.output && (!newPin.empty() || !d->outputs.empty()) &&
                 pin.index < static_cast<int>(od->inputs.size()))
        {
            c.fromRef = "new";
            c.fromPin = !newPin.empty() ? newPin : std::string(d->outputs[0].name);
            c.to      = pin.node;
            c.toPin   = od->inputs[static_cast<std::size_t>(pin.index)].name;
            ops.push_back(c);
        }
    }
    std::vector<std::uint32_t> ids;
    std::string why;
    if (doc.Apply(ops, &ids, why)) RequestSelect({ ids.front() });
    else LOG_WARNING("[MATGRAPH] add %s: %s", type, why.c_str());
    m_paletteFromPin = 0;
}

void MaterialGraphCanvas::DrawNodePanel(MaterialGraphDocument& doc)
{
    if (m_selection.size() != 1) return;
    const Node* n = doc.Graph().FindNode(m_selection.front());
    const NodeDesc* d = n ? MaterialGraph::DescribeNode(*n) : nullptr;
    if (!n || !d) return;
    ImGui::SeparatorText("Node");
    ImGui::TextDisabled("%s #%u", d->label, n->id);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", d->help);
    ImGui::PopTextWrapPos();
    const std::uint32_t id = n->id;
    auto set = [&](const char* key, Value v)
    {
        GraphOp op;
        op.kind = GraphOp::Kind::Set;
        op.node = id;
        op.props[key] = std::move(v);
        std::string why;
        if (!doc.Apply({ op }, nullptr, why)) LOG_WARNING("[MATGRAPH] set %s: %s", key, why.c_str());
    };
    for (const MaterialGraph::PropDesc& p : d->props)
    {
        using K = MaterialGraph::PropDesc::Kind;
        ImGui::PushID(p.name);
        const Value* cur = n->Prop(p.name);
        switch (p.kind)
        {
        case K::Number:
        {
            float v = cur && cur->IsNumeric() ? static_cast<float>(cur->At(0)) : p.def[0];
            if (ImGui::DragFloat(p.name, &v, 0.01f)) set(p.name, Value::Number(v));
            break;
        }
        case K::Numbers:
        {
            float v[4] = { p.def[0], p.def[1], p.def[2], p.def[3] };
            if (cur && cur->IsNumeric()) for (int i = 0; i < p.count; ++i) v[i] = static_cast<float>(cur->At(i));
            bool changed = false;
            if (p.color && p.count == 3) changed = ImGui::ColorEdit3(p.name, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            else if (p.color && p.count == 4) changed = ImGui::ColorEdit4(p.name, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            else changed = ImGui::DragScalarN(p.name, ImGuiDataType_Float, v, p.count, 0.01f);
            if (changed) set(p.name, NumbersValue(v, p.count));
            break;
        }
        case K::Bool:
        {
            bool v = cur ? n->PropBool(p.name, p.defBool) : p.defBool;
            if (ImGui::Checkbox(p.name, &v)) set(p.name, Value::Bool(v));
            break;
        }
        case K::Choice:
        {
            const std::string curText = cur && cur->kind == Value::Kind::String ? cur->str : p.defText;
            if (ImGui::BeginCombo(p.name, curText.c_str()))
            {
                std::string all = p.choices;
                std::size_t b = 0;
                while (b <= all.size())
                {
                    const std::size_t e = all.find('|', b);
                    const std::string item = all.substr(b, e == std::string::npos ? std::string::npos : e - b);
                    if (ImGui::Selectable(item.c_str(), item == curText)) set(p.name, Value::String(item));
                    if (e == std::string::npos) break;
                    b = e + 1;
                }
                ImGui::EndCombo();
            }
            break;
        }
        case K::Code:
        {
            // Multi-line HLSL (a Custom node's body): the text is committed when the
            // editor loses focus - one undo step, one recompile, not one per key.
            const std::string curText = cur && cur->kind == Value::Kind::String ? cur->str : p.defText;
            if (m_codeNode != id || !m_codeEditing)   // not being typed into: show the node's text
            {
                if (m_codeBuf.size() < curText.size() + 16384) m_codeBuf.assign(curText.size() + 16384, '\0');
                std::fill(m_codeBuf.begin(), m_codeBuf.end(), '\0');
                std::copy(curText.begin(), curText.end(), m_codeBuf.begin());
                m_codeNode = id;
            }
            ImGui::TextUnformatted(p.name);
            ImGui::InputTextMultiline("##code", m_codeBuf.data(), m_codeBuf.size(),
                                      ImVec2(-1.0f, ImGui::GetTextLineHeight() * 10.0f), ImGuiInputTextFlags_AllowTabInput);
            m_codeEditing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit() && curText != m_codeBuf.data())
                set(p.name, Value::String(m_codeBuf.data()));
            break;
        }
        default:   // String, Path
        {
            char buf[512];
            const std::string curText = cur && cur->kind == Value::Kind::String ? cur->str : p.defText;
            std::snprintf(buf, sizeof(buf), "%s", curText.c_str());
            if (ImGui::InputText(p.name, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) ||
                (ImGui::IsItemDeactivatedAfterEdit() && curText != buf))
                set(p.name, Value::String(buf));
            if (p.kind == K::Path && ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    set(p.name, Value::String(std::string(static_cast<const char*>(pl->Data))));
                ImGui::EndDragDropTarget();
            }
            break;
        }
        }
        ImGui::PopID();
    }
}
}
