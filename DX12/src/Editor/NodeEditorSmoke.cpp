// NodeEditorSmoke.cpp - the node-editor library's acceptance run.
// See include/Editor/NodeEditorSmoke.h.
#include "Editor/NodeEditorSmoke.h"

#include "System/CommandLine.h"
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui-node-editor/imgui_canvas.h"          // CanvasZoomedTextDensity (patch 2)
#include "imgui-node-editor/imgui_node_editor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace ed = ax::NodeEditor;

namespace Editor
{
namespace
{
    // imgui_canvas.cpp's sentinel draw command, which Canvas::LeaveLocalSpace must erase.
    const ImDrawCallback kCanvasCallback = reinterpret_cast<ImDrawCallback>(static_cast<intptr_t>(-2));
    constexpr int kWarmupFrames = 10;   // the first frames bake the font and place the nodes

    std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::size_t b = 0;
        while (b <= s.size())
        {
            const std::size_t e = s.find(sep, b);
            std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
            if (!item.empty()) out.push_back(std::move(item));
            if (e == std::string::npos) break;
            b = e + 1;
        }
        return out;
    }

    bool IsFrame(const std::string& s)
    {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }
}

NodeEditorSmoke& NodeEditorSmoke::Get()
{
    static NodeEditorSmoke s_instance;
    return s_instance;
}

NodeEditorSmoke::~NodeEditorSmoke() = default;

void NodeEditorSmoke::Shutdown()
{
    // While ImGui is alive: the editor context's teardown touches it.
    if (m_ctx) ed::DestroyEditor(static_cast<ed::EditorContext*>(m_ctx));
    m_ctx = nullptr;
}

bool NodeEditorSmoke::HarnessConfigure(std::string& why)
{
    const auto& cl = Sys::CommandLine::Get();
    if (!cl.Has("-nodeeditor_smoke"))
    {
        for (const char* f : { "-nodeeditor_smoke_zoom", "-nodeeditor_smoke_report", "-nodeeditor_smoke_density_off" })
            if (cl.Has(f)) { why = std::string(f) + " without -nodeeditor_smoke=<nodes>"; return false; }
        return true;
    }
    const std::string n = cl.Get("-nodeeditor_smoke");
    m_nodes = IsFrame(n) ? std::atoi(n.c_str()) : 0;
    if (m_nodes < 1 || m_nodes > 5000) { why = "-nodeeditor_smoke=<nodes, 1..5000>"; return false; }
    if (cl.Has("-nodeeditor_smoke_zoom"))
        for (const std::string& item : Split(cl.Get("-nodeeditor_smoke_zoom"), ','))
        {
            const std::size_t at = item.find('@');
            ZoomStep z;
            z.mode = at == std::string::npos ? item : item.substr(0, at);
            const std::string f = at == std::string::npos ? std::string() : item.substr(at + 1);
            if ((z.mode != "fit" && z.mode != "in") || !IsFrame(f))
            {
                why = "-nodeeditor_smoke_zoom: '" + item + "' is not <fit|in>@<frame>";
                return false;
            }
            z.at = std::strtoull(f.c_str(), nullptr, 10);
            m_zoom.push_back(z);
        }
    if (cl.Has("-nodeeditor_smoke_report"))
    {
        const std::string f = cl.Get("-nodeeditor_smoke_report");
        if (!IsFrame(f)) { why = "-nodeeditor_smoke_report=<editor frame>"; return false; }
        m_reportAt = std::strtoull(f.c_str(), nullptr, 10);
    }
    // The reverse arm of patch 2: glyphs baked at the base size, then scaled (upstream).
    if (cl.Has("-nodeeditor_smoke_density_off")) ImGuiEx::CanvasZoomedTextDensity = false;
    LOG_INFO("[NODEEDITOR] smoke: nodes=%d zoomSteps=%zu report=%llu densityPatch=%d", m_nodes, m_zoom.size(),
             static_cast<unsigned long long>(m_reportAt), ImGuiEx::CanvasZoomedTextDensity ? 1 : 0);
    return true;
}

void NodeEditorSmoke::Draw(std::uint64_t frame)
{
    if (m_nodes <= 0) return;
    if (!m_ctx)
    {
        ed::Config cfg;
        cfg.SettingsFile     = nullptr;   // never NodeEditor.json in the working directory: a graph stores its own positions
        cfg.EnableSmoothZoom = false;     // a zoom lands in one frame: captures are deterministic
        m_ctx = ed::CreateEditor(&cfg);
    }
    ImGui::SetNextWindowPos(ImVec2(40.f, 60.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1100.f, 700.f), ImGuiCond_Always);
    const bool visible = ImGui::Begin("Node Editor Smoke", nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (visible)
    {
        auto* ctx = static_cast<ed::EditorContext*>(m_ctx);
        ed::SetCurrentEditor(ctx);
        const auto t0 = std::chrono::steady_clock::now();
        ed::Begin("smoke", ImVec2(0.f, 0.f));
        const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(m_nodes))));
        for (int i = 0; i < m_nodes; ++i)
        {
            const ed::NodeId id = static_cast<ed::NodeId>(i + 1);
            if (!m_placed)
                ed::SetNodePosition(id, ImVec2(static_cast<float>(i % cols) * 220.f, static_cast<float>(i / cols) * 110.f));
            ed::BeginNode(id);
            ImGui::Text("Node %d", i + 1);
            ed::BeginPin(static_cast<ed::PinId>(100000 + i * 2), ed::PinKind::Input);
            ImGui::TextUnformatted("-> In");
            ed::EndPin();
            ImGui::SameLine();
            ed::BeginPin(static_cast<ed::PinId>(100000 + i * 2 + 1), ed::PinKind::Output);
            ImGui::TextUnformatted("Out ->");
            ed::EndPin();
            ed::EndNode();
        }
        for (int i = 0; i + 1 < m_nodes; ++i)
            ed::Link(static_cast<ed::LinkId>(500000 + i), static_cast<ed::PinId>(100000 + i * 2 + 1),
                     static_cast<ed::PinId>(100000 + (i + 1) * 2));
        m_placed = true;
        for (ZoomStep& z : m_zoom)
        {
            if (z.done || frame < z.at) continue;
            z.done = true;
            if (z.mode == "fit")
                ed::NavigateToContent(0.0f);
            else
            {
                // The navigation reads the selection when it runs: node 1 stays selected.
                ed::ClearSelection();
                ed::SelectNode(static_cast<ed::NodeId>(1));
                ed::NavigateToSelection(true, 0.0f);
            }
            m_zoomLabel = z.mode;
            LOG_INFO("[NODEEDITOR] smoke zoom %s at frame %llu", z.mode.c_str(), static_cast<unsigned long long>(frame));
        }
        ed::End();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // GetCurrentZoom() is the view's INVERSE scale (canvas units per pixel).
        const float inv = ed::GetCurrentZoom();
        m_scale = inv > 0.f ? 1.f / inv : 1.f;
        ed::SetCurrentEditor(nullptr);
        if (++m_drawn > kWarmupFrames) m_ms.push_back(ms);

        // The canvas marks its range with a sentinel callback and must take it
        // out again: one left behind is an application callback in the frame's
        // draw data (the render owner then finishes before the next simulation).
        for (const ImDrawCmd& c : ImGui::GetWindowDrawList()->CmdBuffer)
            if (c.UserCallback == kCanvasCallback) ++m_leftover;
    }
    ImGui::End();

    if (!m_reported && m_reportAt && frame >= m_reportAt)
    {
        m_reported = true;
        std::vector<double> v = m_ms;
        std::sort(v.begin(), v.end());
        auto pct = [&v](double q) { return v.empty() ? -1.0 : v[static_cast<std::size_t>(q * (v.size() - 1))]; };
        // The density the canvas baked its text at for this view (patch 2's rule).
        const float density = ImGuiEx::CanvasZoomedTextDensity
            ? (std::min)(4.0f, (std::max)(0.25f, static_cast<float>(static_cast<int>(m_scale * 4.0f + 0.5f)) * 0.25f))
            : 1.0f;
        LOG_INFO("[NODEEDITOR] smoke nodes=%d links=%d frames=%zu msP50=%.3f msP99=%.3f msMax=%.3f scale=%.3f "
                 "density=%.2f leftoverCallbacks=%d mode=%s", m_nodes, (std::max)(0, m_nodes - 1), v.size(), pct(0.5),
                 pct(0.99), v.empty() ? -1.0 : v.back(), m_scale, density, m_leftover, m_zoomLabel.c_str());
    }
}
}
