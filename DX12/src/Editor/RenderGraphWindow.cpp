// RenderGraphWindow.cpp - see include/Editor/RenderGraphWindow.h.

#include "Editor/RenderGraphWindow.h"

#include "Editor/GraphLayout.h"      // LayersOrdered
#include "Editor/NodeGraphView.h"    // imgui-node-editor's context policy
#include "Graphics/FormatNames.h"
#include "Graphics/FrameSnapshot.h"
#include "Graphics/GraphicsStruct.h"    // RHI::ResourceState (the Resources tab's colours)
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/FrameCapture.h"
#include "RenderGraph/ImportSpec.h"
#include "RenderGraph/RenderGraphDag.h"   // the static DAG the layout is cut from
#include "RenderGraph/SceneView.h"       // the capture's view and what it hid
#include "RenderGraph/Stage.h"
#include "RenderGraph/StencilBits.h"   // the Resources tab's stencil field rows
#include "Renderer/Renderer.h"   // the pass capture: the texture Inspector's preview
#include "System/CommandLine.h"   // -rgpanel_pick, -rgpanel_save, -rgpanel_preview (harness)
#include "System/Log.h"
#include "System/PathUtil.h"      // Save JSON's folder
#if defined(WITH_HARNESS) && WITH_HARNESS
#include "Harness/RenderGraphDump.h"   // Save JSON: the -rgdump builder
#endif

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Editor
{
namespace
{
    using Snap   = RG::GraphSnapshot;
    using Device = GfxDiag::FrameSnapshot;

    constexpr double kLiveSeconds    = 0.5;   // Live: one capture asked per half second
    constexpr double kPendingSeconds = 5.0;   // an unanswered ask is asked again after this

    namespace ed = ax::NodeEditor;

    // The Profiler's palette rule (EditorLayer_Profiler.cpp NameColor): a
    // golden-ratio hue walk over the stage's place in RG::Stage, so a stage
    // keeps its colour across captures and the neighbours stay apart.
    ImU32 StageColor(const char* stage, float alpha = 1.f)
    {
        int id = 0;
        for (int s = 0; s < static_cast<int>(RG::Stage::Count); ++s)
            if (std::strcmp(stage, RG::StageName(static_cast<RG::Stage>(s))) == 0) id = s + 1;
        if (id == 0) return ImGui::GetColorU32(ImVec4(0.55f, 0.55f, 0.55f, alpha));   // Unstaged
        const float h = std::fmod(0.13f + 0.61803398875f * float(id), 1.0f);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(h, 0.45f, 0.80f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
    }

    bool Writes(Snap::Access a)
    {
        return a == Snap::Access::RenderTarget || a == Snap::Access::Depth || a == Snap::Access::UAV ||
               a == Snap::Access::Manual || a == Snap::Access::CopyDst;
    }
    bool Reads(Snap::Access a) { return !Writes(a) || a == Snap::Access::UAV || a == Snap::Access::Manual; }

    bool Matches(const std::string& name, const char* filter)
    {
        if (!filter[0]) return true;
        std::string a = name, b = filter;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return a.find(b) != std::string::npos;
    }

    std::string Ms(float ms) { char b[32]; std::snprintf(b, sizeof(b), ms < 0.f ? "-" : "%.3f", ms); return b; }

    // The edge colours: imgui-node-editor draws no dashes, so the kinds are told
    // apart by colour and thickness - RAW the data, WAR / WAW the ordering a
    // reorder must keep, the order constraints, and (with Inactive on) the static
    // edges of a pass that did not run.
    const ImVec4 kRawColor(0.40f, 0.78f, 1.00f, 1.f);
    const ImVec4 kWarColor(1.00f, 0.70f, 0.30f, 1.f);
    const ImVec4 kWawColor(0.80f, 0.55f, 1.00f, 1.f);
    const ImVec4 kOrderColor(0.45f, 1.00f, 0.50f, 1.f);
    const ImVec4 kStaticColor(0.45f, 0.45f, 0.45f, 0.6f);

    // Link ids apart from the node ids (1 + pass index) and the pin ids (NodeGraphView::Pin).
    constexpr uint64_t kLinkBase = 1ull << 40;

    // The Resources tab: a colour per state the graph's tracker holds.
    ImU32 StateColor(uint32_t state, float alpha = 0.40f)
    {
        using RS = RHI::ResourceState;
        ImVec4 c(0.35f, 0.35f, 0.35f, alpha);
        switch (static_cast<RS>(state))
        {
        case RS::RENDERTARGET:            c = ImVec4(0.95f, 0.55f, 0.20f, alpha); break;
        case RS::DEPTHSTENCIL:            c = ImVec4(0.90f, 0.25f, 0.25f, alpha); break;
        case RS::DEPTHSTENCIL_READONLY:
        case RS::DEPTH_READ_SRV:          c = ImVec4(0.90f, 0.45f, 0.65f, alpha); break;
        case RS::SHADER_RESOURCE:         c = ImVec4(0.30f, 0.80f, 0.35f, alpha); break;
        case RS::SHADER_RESOURCE_COMPUTE: c = ImVec4(0.25f, 0.75f, 0.75f, alpha); break;
        case RS::UNORDERED_ACCESS:        c = ImVec4(0.65f, 0.40f, 0.95f, alpha); break;
        case RS::COPY_SRC:
        case RS::COPY_DST:                c = ImVec4(0.30f, 0.50f, 0.95f, alpha); break;
        default: break;
        }
        return ImGui::GetColorU32(c);
    }

    // The Frame tab: a device resource by its debug name, an unnamed one (buffers
    // are) by its address; a list by its name or pool slot.
    std::string ResourceName(const Device& d, uint32_t r)
    {
        if (r == Device::kNone) return "<all UAVs>";
        if (r >= d.resources.size()) return "<resource " + std::to_string(r) + ">";
        const Device::Resource& x = d.resources[r];
        if (!x.name.empty()) return x.name;
        char b[48];
        std::snprintf(b, sizeof(b), "<%s 0x%llX>", x.buffer ? "buffer" : "texture", static_cast<unsigned long long>(x.address));
        return b;
    }
    std::string ListName(const Device& d, uint32_t slot)
    {
        if (slot < d.commandLists.size() && !d.commandLists[slot].name.empty()) return d.commandLists[slot].name;
        return "cmd" + std::to_string(slot);
    }

    // A cell's glyph: what the pass declared on the texture (the plane's).
    const char* Glyph(const Snap::PassAccess& a)
    {
        switch (a.kind)
        {
        case Snap::Access::Read:         return "R";
        case Snap::Access::History:      return "H";
        case Snap::Access::Scoped:       return "S";
        case Snap::Access::DsvRead:      return "t";
        case Snap::Access::Depth:        return "D";
        case Snap::Access::RenderTarget: return a.load == 1 ? "W!" : "W";
        case Snap::Access::UAV:          return "U";
        case Snap::Access::Manual:       return "M";
        case Snap::Access::CopySrc:      return "cs";
        case Snap::Access::CopyDst:      return "cd";
        }
        return "?";
    }
}

// ==== the model: one per capture shown ====
struct RenderGraphWindow::Model
{
    struct ResRow { uint32_t texture = 0; int8_t plane = -1; int8_t field = -1; };

    std::shared_ptr<const RG::FrameCapture> cap;
    const Snap*   snap   = nullptr;
    const Device* device = nullptr;
    uint32_t      active = 0;
    std::vector<std::vector<uint32_t>> findingsOfPass;   // indices into snap->findings
    std::vector<std::vector<uint32_t>> writers, readers; // per texture: the passes that declared it
    uint32_t      hidden = 0;                            // observed handoffs the declarations miss
    // The Graph tab: the static DAG (every pass treated as running, its edges
    // from the declarations - RG::DeriveDag), which lays out the passes that did
    // not run when the tab shows them; the order constraints as edges; the pass
    // list's key (a new capture of the same list keeps the placed nodes).
    std::vector<Snap::Dependency>       staticEdges;
    std::vector<std::array<uint32_t,2>> orderEdges;      // {from, to}
    uint64_t                            layoutKey = 0;   // the pass list's (names in order)
    // The passes the first sort took out of their registration order (the staged
    // passes off a longest run that kept it - [RG_ORDER] moved).
    std::vector<uint8_t>                movedBySort;

    // The Resources tab: a row per texture (a two-plane texture a depth row and a
    // stencil row), the state each row is in once each pass's barriers are done,
    // replayed from the tracker's initial state; whether the replay ends where the
    // tracker did; the alias candidates. Under the scene depth's stencil row, a row
    // per field of its byte (RG::StencilBits::kFields, field = the index; -1 = the
    // plane itself).
    std::vector<ResRow>                 resRows;
    std::vector<std::vector<uint32_t>>  stateAfter;      // [row][pass]
    std::vector<uint8_t>                replayAgrees;    // [row]
    std::vector<std::array<uint32_t,2>> aliasPairs;

    std::string PassName(uint32_t p) const
    {
        return p < snap->passes.size() ? snap->passes[p].name : "<pass " + std::to_string(p) + ">";
    }
    std::string TexName(uint32_t t) const
    {
        return t < snap->textures.size() ? snap->textures[t].name : "<texture " + std::to_string(t) + ">";
    }
};

RenderGraphWindow& RenderGraphWindow::Get()
{
    static RenderGraphWindow s_window;
    return s_window;
}

// ==== the capture ====
void RenderGraphWindow::Refresh()
{
    RG::FrameCaptureBoard& board = RG::FrameCaptures();
    std::shared_ptr<const RG::FrameCapture> latest = board.Latest();
    if (latest && latest->graph && !m_frozen &&
        (!m_model || !m_model->cap || latest->sequence != m_model->cap->sequence))
        Build(std::move(latest));

    const double now = ImGui::GetTime();
    if (m_pending && (board.PublishedSequence() > m_requestedAt || now - m_lastRequest > kPendingSeconds))
        m_pending = false;
    const bool due = !m_model || m_refreshNow || (m_live && !m_frozen && now - m_lastRequest >= kLiveSeconds);
    if (due && !m_pending)
    {
        m_requestedAt = board.Request();
        m_pending     = true;
        m_lastRequest = now;
        m_refreshNow  = false;
    }
}

void RenderGraphWindow::Build(std::shared_ptr<const RG::FrameCapture> cap)
{
    auto m = std::make_shared<Model>();
    m->cap    = std::move(cap);
    m->snap   = m->cap->graph.get();
    m->device = m->cap->device.get();
    const Snap& s = *m->snap;
    m->findingsOfPass.assign(s.passes.size(), {});
    for (uint32_t f = 0; f < s.findings.size(); ++f)
        if (s.findings[f].pass < s.passes.size()) m->findingsOfPass[s.findings[f].pass].push_back(f);
    m->writers.assign(s.textures.size(), {});
    m->readers.assign(s.textures.size(), {});
    for (uint32_t p = 0; p < s.passes.size(); ++p)
    {
        if (s.passes[p].Ran()) ++m->active;
        for (const Snap::PassAccess& a : s.passes[p].accesses)
        {
            if (a.texture >= s.textures.size()) continue;
            auto add = [p](std::vector<uint32_t>& v) { if (v.empty() || v.back() != p) v.push_back(p); };
            if (Writes(a.kind)) add(m->writers[a.texture]);
            if (Reads(a.kind))  add(m->readers[a.texture]);
        }
    }
    m->hidden = m->cap->coverage.hiddenEdges;

    // The static DAG and the layout.
    {
        std::vector<std::vector<Snap::PassAccess>> accesses(s.passes.size());
        for (size_t p = 0; p < s.passes.size(); ++p) accesses[p] = s.passes[p].accesses;
        std::vector<uint8_t> all(s.passes.size(), 1), fresh(s.textures.size(), 0), planar(s.textures.size(), 0);
        for (size_t t = 0; t < s.textures.size(); ++t)
        {
            fresh[t]  = s.textures[t].imported && s.textures[t].importEntry == 1 ? 1 : 0;   // ImportEntry::Fresh
            planar[t] = s.textures[t].planar ? 1 : 0;
        }
        RG::Dag dag;
        RG::DagScratch scratch;
        RG::DeriveDag(accesses, all, s.textures.size(), dag, scratch, &fresh, &planar);
        m->staticEdges = dag.edges;
        for (uint32_t p = 0; p < s.passes.size(); ++p)
            for (const RG::OrderRule& r : s.passes[p].order)
                for (uint32_t q = 0; q < s.passes.size(); ++q)
                {
                    const std::string& n = s.passes[q].name;
                    if (q == p || s.passes[q].stage != s.passes[p].stage || n.compare(0, r.pass.size(), r.pass) != 0 ||
                        (n.size() != r.pass.size() && n[r.pass.size()] != '#'))
                        continue;
                    m->orderEdges.push_back(r.kind == RG::Order::Kind::After ? std::array<uint32_t,2>{ q, p }
                                                                            : std::array<uint32_t,2>{ p, q });
                }
        uint64_t key = 1469598103934665603ull;   // FNV-1a over the pass names in order
        for (const Snap::Pass& p : s.passes)
            for (char c : p.name + "|") key = (key ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        m->layoutKey = key;

        // The passes the first sort moved: the staged ones off a longest increasing
        // run of registration numbers in declaration order.
        {
            std::vector<uint32_t> idx;
            for (uint32_t p = 0; p < s.passes.size(); ++p)
                if (std::strcmp(s.passes[p].stage, "Unstaged") != 0) idx.push_back(p);
            std::vector<uint32_t> run(idx.size(), 1);
            std::vector<size_t> prev(idx.size(), SIZE_MAX);
            size_t best = 0;
            for (size_t a = 0; a < idx.size(); ++a)
            {
                for (size_t b = 0; b < a; ++b)
                    if (s.passes[idx[b]].registered < s.passes[idx[a]].registered && run[b] + 1 > run[a])
                    {
                        run[a] = run[b] + 1;
                        prev[a] = b;
                    }
                if (run[a] > run[best]) best = a;
            }
            m->movedBySort.assign(s.passes.size(), 1);
            for (uint32_t p = 0; p < s.passes.size(); ++p)
                if (std::strcmp(s.passes[p].stage, "Unstaged") == 0) m->movedBySort[p] = 0;
            for (size_t a = idx.empty() ? SIZE_MAX : best; a != SIZE_MAX; a = prev[a]) m->movedBySort[idx[a]] = 0;
        }

        // The Resources tab's rows and the replay. The scene depth's stencil byte also
        // as its fields - who writes / tests 0x40, 0x80 - the bits the registry names
        // (the other depth textures' stencil is a copy of it, or unused).
        for (uint32_t t = 0; t < s.textures.size(); ++t)
        {
            if (s.textures[t].planar)
            {
                m->resRows.push_back({ t, 0 });
                m->resRows.push_back({ t, 1 });
                if (s.textures[t].name == "GBuffer_Depth")
                    for (int8_t f = 0; f < static_cast<int8_t>(std::size(RG::StencilBits::kFields)); ++f)
                        m->resRows.push_back({ t, 1, f });
            }
            else
                m->resRows.push_back({ t, -1 });
        }
        m->stateAfter.assign(m->resRows.size(), std::vector<uint32_t>(s.passes.size(), 0));
        m->replayAgrees.assign(m->resRows.size(), 1);
        for (size_t r = 0; r < m->resRows.size(); ++r)
        {
            const Model::ResRow& row = m->resRows[r];
            const Snap::Texture& x = s.textures[row.texture];
            uint32_t state = row.plane == 1 ? x.stencilInitialState : x.initialState;
            for (uint32_t p = 0; p < s.passes.size(); ++p)
            {
                for (const Snap::Barrier& b : s.passes[p].barriers)
                    if (b.texture == row.texture && (row.plane < 0 || b.plane < 0 || b.plane == row.plane))
                        state = b.after;
                m->stateAfter[r][p] = state;
            }
            m->replayAgrees[r] = state == (row.plane == 1 ? x.stencilFinalState : x.finalState) ? 1 : 0;
        }
        for (uint32_t a = 0; a < s.textures.size(); ++a)
            for (uint32_t b = a + 1; b < s.textures.size() && m->aliasPairs.size() < 64; ++b)
            {
                const Snap::Texture& x = s.textures[a];
                const Snap::Texture& y = s.textures[b];
                if (x.imported || y.imported || x.firstUse < 0 || y.firstUse < 0) continue;
                if (x.width != y.width || x.height != y.height || x.format != y.format || x.arraySize != y.arraySize ||
                    x.isDepth != y.isDepth)
                    continue;
                if (x.lastUse < y.firstUse || y.lastUse < x.firstUse) m->aliasPairs.push_back({ a, b });
            }
    }

    // A selection the new capture no longer has is dropped.
    if ((m_pick == Pick::Pass && m_pickIndex >= s.passes.size()) ||
        (m_pick == Pick::Texture && m_pickIndex >= s.textures.size()) ||
        (m_pick == Pick::List && (!m->device || m_pickIndex >= m->device->commandLists.size())))
        m_pick = Pick::None;

    static const char* const kTabNames[] = { "Graph", "Resources", "Frame" };
    LOG_INFO("[RG_PANEL] frame=%llu seq=%llu passes=%zu active=%u textures=%zu edges=%zu findings=%zu lists=%zu "
             "observed=%zu hidden=%u tab=%s",
             static_cast<unsigned long long>(m->cap->frame), static_cast<unsigned long long>(m->cap->sequence),
             s.passes.size(), m->active, s.textures.size(), s.edges.size(), s.findings.size(),
             m->device ? m->device->commandLists.size() : size_t(0), m->cap->observed.size(), m->hidden,
             kTabNames[static_cast<int>(m_tab)]);
    m_model   = std::move(m);
    m_arrived = ImGui::GetTime();

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -rgpanel_pick=<pass, texture or list> : select it once the first capture is in,
    // so a harness picture shows the Inspector (rg_panel, test_rg_panel.py).
    static const std::string s_pick = Sys::CommandLine::Get().Get("-rgpanel_pick");
    static bool s_picked = false;
    if (!s_picked && !s_pick.empty())
    {
        s_picked = true;
        const Snap& now = *m_model->snap;
        for (uint32_t i = 0; i < now.passes.size() && m_pick == Pick::None; ++i)
            if (now.passes[i].name == s_pick) { m_pick = Pick::Pass; m_pickIndex = i; }
        for (uint32_t t = 0; t < now.textures.size() && m_pick == Pick::None; ++t)
            if (now.textures[t].name == s_pick) { m_pick = Pick::Texture; m_pickIndex = t; }
        if (m_model->device)   // a command list: its name, or cmd<slot>
            for (uint32_t c = 0; c < m_model->device->commandLists.size() && m_pick == Pick::None; ++c)
                if (ListName(*m_model->device, c) == s_pick || "cmd" + std::to_string(c) == s_pick)
                {
                    m_pick = Pick::List;
                    m_pickIndex = c;
                }
        LOG_INFO("[RG_PANEL] pick=%s found=%d", s_pick.c_str(), m_pick != Pick::None ? 1 : 0);
    }
    // -rgpanel_preview=<pass | end>[:stencil]: with -rgpanel_pick naming a texture,
    // its preview after that pass - "end" the frame's end, :stencil the stencil
    // plane of a depth texture - asked once (test_rg_panel.py).
    static const std::string s_preview = Sys::CommandLine::Get().Get("-rgpanel_preview");
    static bool s_previewed = false;
    if (!s_previewed && !s_preview.empty() && m_pick == Pick::Texture && m_pickIndex < m_model->snap->textures.size())
    {
        s_previewed = true;
        const size_t colon = s_preview.find(':');
        const std::string after = s_preview.substr(0, colon);
        m_previewTexture = m_model->snap->textures[m_pickIndex].name;
        m_previewAfter   = after == "end" ? std::string() : after;
        m_previewMode    = 0;
        m_previewChannel = -1;
        m_previewPlane   = colon != std::string::npos && s_preview.substr(colon + 1) == "stencil" ? 1 : 0;
        m_previewRange   = false;
        m_previewAsk     = true;
        LOG_INFO("[RG_PANEL] preview asked texture=%s after=%s plane=%d", m_previewTexture.c_str(),
                 m_previewAfter.empty() ? "-" : m_previewAfter.c_str(), m_previewPlane);
    }
    // -rgpanel_save : Save JSON once, on the first capture shown (rg_panel, test_rg_report.py).
    static const bool s_save = Sys::CommandLine::Get().Has("-rgpanel_save");
    static bool s_saved = false;
    if (s_save && !s_saved)
    {
        s_saved = true;
        SaveJson();
    }
#endif
}

#if defined(WITH_HARNESS) && WITH_HARNESS
// Save JSON: the capture shown, by the builder -rgdump and `rg full` use, so
// tools/rg_report.py reads the three alike. Beside the run's other state dumps: a
// harness run's artifact folder, WritableDataDir() otherwise.
void RenderGraphWindow::SaveJson()
{
    if (!m_model || !m_model->cap) return;
    const std::string path = Sys::DiagnosticsDir() + "/rg/rg_" + std::to_string(m_model->cap->frame) + ".json";
    if (Harness::WriteRenderGraphDump(*m_model->cap, path))
    {
        m_savedPath = path;
        LOG_INFO("[RG_PANEL] saved=%s frame=%llu seq=%llu", path.c_str(),
                 static_cast<unsigned long long>(m_model->cap->frame),
                 static_cast<unsigned long long>(m_model->cap->sequence));
    }
    else
    {
        m_savedPath.clear();
        LOG_WARNING("[RG_PANEL] Save JSON could not write %s", path.c_str());
    }
}
#endif

void RenderGraphWindow::SetGpuWant(IGraphicsDevice* gfx, bool want)
{
    if (!gfx || want == m_gpuWant) return;
    if (want) gfx->AddGPUProfilerWant();
    else      gfx->ReleaseGPUProfilerWant();
    m_gpuWant = want;
}

void RenderGraphWindow::Shutdown(IGraphicsDevice* gfx)
{
    SetGpuWant(gfx, false);
    m_model.reset();
    NodeGraphView::DestroyContext(m_nodeContext);
    m_nodeContext = nullptr;
}

// ==== the window ====
void RenderGraphWindow::Draw(const Context& ctx)
{
    const auto held = [&](const char* name) { return ctx.held && ctx.held(name); };
    const bool heldNow = held("Render Graph") || held("Render Graph/Graph") || held("Render Graph/Resources") ||
                         held("Render Graph/Frame");
    if (heldNow)
        m_open = true;
    else if (m_heldLast)   // a later -ui_open_menu entry took the hold: closed, as a person closes it
        m_open = false;
    m_heldLast = heldNow;
    m_renderer = ctx.renderer;
    if (!m_open)
    {
        // Closed: nothing asked, nothing held.
        ReleasePreview();
        SetGpuWant(ctx.gfx, false);
        if (m_model) m_model.reset();
        m_pending = false;
        if (m_nodeContext)
        {
            NodeGraphView::DestroyContext(m_nodeContext);
            m_nodeContext = nullptr;
            m_placedLayout = 0;
        }
        return;
    }
    SetGpuWant(ctx.gfx, m_gpuTimes);
    Refresh();

    ImGui::SetNextWindowSize(ImVec2(1180, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render Graph", &m_open))
    {
        ImGui::End();
        return;
    }
    DrawToolbar(ctx);
    if (!m_model || !m_model->snap)
    {
        ImGui::TextDisabled("Waiting for the main graph's first frame capture ...");
        ImGui::End();
        return;
    }
    m_previewDrawn = false;
    if (ImGui::BeginTable("##rg_panes", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("Navigator", ImGuiTableColumnFlags_WidthFixed, 230.f);
        ImGui::TableSetupColumn("Main", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 340.f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawNavigator();
        ImGui::TableSetColumnIndex(1);
        DrawTabs(ctx);
        ImGui::TableSetColumnIndex(2);
        DrawInspector();
        ImGui::EndTable();
    }
    ImGui::End();
    // The preview not shown this frame (another selection, the header folded):
    // the pass capture is let go.
    if (!m_previewDrawn) ReleasePreview();
}

void RenderGraphWindow::DrawToolbar(const Context&)
{
    ImGui::Checkbox("Live", &m_live);
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) m_refreshNow = true;
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &m_frozen);
    ImGui::SameLine();
    ImGui::Checkbox("GPU times", &m_gpuTimes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Holds a GPU profiler want while ticked, so each pass's GPU time is measured\n"
                          "(the Profiler window's mechanism). Untick to stop timing.");
    ImGui::SameLine();
    ImGui::Checkbox("Inactive", &m_showInactive);
    if (!m_model || !m_model->snap) return;
#if defined(WITH_HARNESS) && WITH_HARNESS
    ImGui::SameLine();
    if (ImGui::Button("Save JSON")) SaveJson();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes this capture as -rgdump does (the same builder) to\n"
                          "<diagnostics>/rg/rg_<frame>.json: the run's -artifacts folder, else the data folder.\n"
                          "python tools/rg_report.py summary | dot | diff reads it.%s%s",
                          m_savedPath.empty() ? "" : "\nLast saved: ", m_savedPath.c_str());
#endif
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    ImGui::SameLine();
    ImGui::TextDisabled("|  frame %llu  seq %llu  %.1fs ago  |  passes %zu (ran %u)  textures %zu  edges %zu"
                        "  findings %zu  lists %zu  hidden %u",
                        static_cast<unsigned long long>(m.cap->frame), static_cast<unsigned long long>(m.cap->sequence),
                        ImGui::GetTime() - m_arrived, s.passes.size(), m.active, s.textures.size(), s.edges.size(),
                        s.findings.size(), m.device ? m.device->commandLists.size() : size_t(0), m.hidden);
    // The view this capture drew, and the flags it did not show.
    const uint32_t hides = ~s.viewShow & RG::kShowAll;
    ImGui::SameLine();
    ImGui::TextDisabled("|  view %s%s%s", s.viewName.c_str(), hides ? "  hides " : "",
                        hides ? RG::ShowFlagList(hides, ", ").c_str() : "");
}

// ==== the navigator: the passes by stage, the textures ====
void RenderGraphWindow::DrawNavigator()
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##rg_filter", "filter", m_filter, sizeof(m_filter));
    ImGui::BeginChild("##rg_nav", ImVec2(0, 0), false);
    if (ImGui::CollapsingHeader("Passes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* stage = nullptr;
        for (uint32_t i = 0; i < s.passes.size(); ++i)
        {
            const Snap::Pass& p = s.passes[i];
            if ((!m_showInactive && !p.Ran()) || !Matches(p.name, m_filter)) continue;
            if (!stage || std::strcmp(stage, p.stage) != 0)
            {
                stage = p.stage;
                ImGui::PushStyleColor(ImGuiCol_Text, StageColor(stage));
                ImGui::TextUnformatted(stage);
                ImGui::PopStyleColor();
            }
            ImGui::PushID(static_cast<int>(i));
            if (!p.Ran()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            const bool picked = m_pick == Pick::Pass && m_pickIndex == i;
            std::string label = "  " + p.name;
            if (!m.findingsOfPass[i].empty()) label += "  (!)";
            if (ImGui::Selectable(label.c_str(), picked)) { m_pick = Pick::Pass; m_pickIndex = i; }
            if (!p.Ran()) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Textures"))
    {
        for (uint32_t t = 0; t < s.textures.size(); ++t)
        {
            const Snap::Texture& x = s.textures[t];
            if (!Matches(x.name, m_filter)) continue;
            ImGui::PushID(static_cast<int>(1000000 + t));
            const bool picked = m_pick == Pick::Texture && m_pickIndex == t;
            std::string label = x.name + (x.imported ? "  [import]" : "");
            if (ImGui::Selectable(label.c_str(), picked)) { m_pick = Pick::Texture; m_pickIndex = t; }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// ==== the tabs ====
void RenderGraphWindow::DrawTabs(const Context& ctx)
{
    const auto tabFlags = [&](const char* name)
    {
        return ctx.held && ctx.held(name) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (!ImGui::BeginTabBar("##rg_tabs")) return;
    if (ImGui::BeginTabItem("Graph", nullptr, tabFlags("Render Graph/Graph")))
    {
        m_tab = Tab::Graph;
        if (ImGui::RadioButton("Graph", !m_listView)) m_listView = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("List", m_listView)) m_listView = true;
        if (m_listView) DrawPassList();
        else            DrawGraph();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Resources", nullptr, tabFlags("Render Graph/Resources")))
    {
        m_tab = Tab::Resources;
        DrawResources();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Frame", nullptr, tabFlags("Render Graph/Frame")))
    {
        m_tab = Tab::Frame;
        DrawFrame();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// The passes as a table (the Graph tab's list view; DrawGraph is the node view).
void RenderGraphWindow::DrawPassList()
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##rg_passes", 9, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 200.f);
    ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed, 120.f);
    ImGui::TableSetupColumn("Ran");
    ImGui::TableSetupColumn("CPU ms");
    ImGui::TableSetupColumn("GPU ms");
    ImGui::TableSetupColumn("Barriers");
    ImGui::TableSetupColumn("Waits");
    ImGui::TableSetupColumn("Findings");
    ImGui::TableHeadersRow();
    for (uint32_t i = 0; i < s.passes.size(); ++i)
    {
        const Snap::Pass& p = s.passes[i];
        if ((!m_showInactive && !p.Ran()) || !Matches(p.name, m_filter)) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%u", i);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(static_cast<int>(i));
        if (!p.Ran()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(p.name.c_str(), m_pick == Pick::Pass && m_pickIndex == i,
                              ImGuiSelectableFlags_SpanAllColumns))
        {
            m_pick = Pick::Pass;
            m_pickIndex = i;
        }
        if (!p.Ran()) ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, StageColor(p.stage));
        ImGui::TextUnformatted(p.stage);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(p.Ran() ? "yes" : Snap::SkipName(p.skip));
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(p.Ran() ? Ms(p.cpuMs).c_str() : "");
        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted(p.Ran() ? Ms(p.gpuMs).c_str() : "");
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%zu", p.barriers.size() + p.uavBarriers.size());
        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%u", p.externalWaits);
        ImGui::TableSetColumnIndex(8);
        if (!m.findingsOfPass[i].empty()) ImGui::Text("%zu", m.findingsOfPass[i].size());
    }
    ImGui::EndTable();
}

// ==== the Resources tab: textures by passes ====
void RenderGraphWindow::DrawResources()
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    // The legend: the states' colours and the glyphs.
    {
        using RS = RHI::ResourceState;
        const std::pair<RS, const char*> states[] = {
            { RS::RENDERTARGET, "RT" }, { RS::DEPTHSTENCIL, "DSV" }, { RS::DEPTH_READ_SRV, "depth read" },
            { RS::SHADER_RESOURCE, "SRV" }, { RS::SHADER_RESOURCE_COMPUTE, "SRV (cs)" }, { RS::UNORDERED_ACCESS, "UAV" },
            { RS::COPY_DST, "copy" }, { RS::UNDEFINED, "undefined" } };
        for (const auto& st : states)
        {
            ImGui::ColorButton(st.second, ImGui::ColorConvertU32ToFloat4(StateColor(static_cast<uint32_t>(st.first), 0.9f)),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::SameLine(0, 3);
            ImGui::TextDisabled("%s", st.second);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("|  R read  H history  S scoped  t depth test  D depth write  W / W! RT (load / clear)  "
                            "U UAV  M manual  cs / cd copy  |  > barrier");
    }
    std::vector<uint32_t> cols;
    for (uint32_t p = 0; p < s.passes.size(); ++p)
        if (m_showInactive || s.passes[p].Ran()) cols.push_back(p);
    const int columns = 1 + static_cast<int>(std::min<size_t>(cols.size(), 400));
    const float aliasHeight = m.aliasPairs.empty() ? 0.f : 120.f;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_HighlightHoveredColumn;
    if (ImGui::BeginTable("##rg_resources", columns, flags, ImVec2(0, -aliasHeight)))
    {
        ImGui::TableSetupScrollFreeze(1, 2);
        ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 200.f);
        for (int c = 1; c < columns; ++c)
            ImGui::TableSetupColumn(s.passes[cols[c - 1]].name.c_str(),
                                    ImGuiTableColumnFlags_AngledHeader | ImGuiTableColumnFlags_WidthFixed, 26.f);
        ImGui::TableAngledHeadersRow();
        ImGui::TableHeadersRow();
        for (size_t r = 0; r < m.resRows.size(); ++r)
        {
            const Model::ResRow& row = m.resRows[r];
            const Snap::Texture& x = s.textures[row.texture];
            if (!Matches(x.name, m_filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const RG::StencilBits::Field* field = row.field >= 0 ? &RG::StencilBits::kFields[row.field] : nullptr;
            char fieldLabel[48] = {};
            if (field) std::snprintf(fieldLabel, sizeof(fieldLabel), "    %s 0x%02X", field->name, field->mask);
            std::string label = field ? std::string(fieldLabel)
                                      : x.name + (row.plane == 0 ? " (depth)" : row.plane == 1 ? " (stencil)" : "") +
                                            (x.imported ? " *" : "") + (m.replayAgrees[r] ? "" : " !");
            ImGui::PushID(static_cast<int>(r));
            if (ImGui::Selectable(label.c_str(), m_pick == Pick::Texture && m_pickIndex == row.texture))
            {
                m_pick = Pick::Texture;
                m_pickIndex = row.texture;
            }
            if (!m.replayAgrees[r] && ImGui::IsItemHovered())
                ImGui::SetTooltip("The replayed barriers end in %s, the tracker in %s (a transition outside the graph's barriers).",
                                  Snap::StateName(m.stateAfter[r].empty() ? 0 : m.stateAfter[r].back()).c_str(),
                                  Snap::StateName(row.plane == 1 ? x.stencilFinalState : x.finalState).c_str());
            for (int c = 1; c < columns; ++c)
            {
                const uint32_t p = cols[c - 1];
                const Snap::Pass& pass = s.passes[p];
                ImGui::TableSetColumnIndex(c);
                const bool inLife = x.firstUse >= 0 && static_cast<int32_t>(p) >= x.firstUse &&
                                    static_cast<int32_t>(p) <= x.lastUse;
                const uint32_t state = m.stateAfter[r][p];
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, StateColor(state, inLife ? 0.45f : 0.12f));
                std::string glyph;
                for (const Snap::PassAccess& a : pass.accesses)
                    if (a.texture == row.texture && (row.plane < 0 || a.plane < 0 || a.plane == row.plane) &&
                        (!field || (a.stencilMask & field->mask) != 0))   // the field's bits
                        glyph += Glyph(a);
                bool barrier = false;
                const Snap::Barrier* last = nullptr;
                for (const Snap::Barrier& b : pass.barriers)
                    if (!field && b.texture == row.texture && (row.plane < 0 || b.plane < 0 || b.plane == row.plane))
                    {
                        barrier = true;
                        last = &b;
                    }
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::PushID(c);
                if (!pass.Ran()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                ImGui::Selectable(glyph.empty() ? "##c" : (glyph + "##c").c_str(), false, ImGuiSelectableFlags_None,
                                  ImVec2(22.f, 0.f));
                if (!pass.Ran()) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s  x  %s", pass.name.c_str(), label.c_str());
                    ImGui::Text("state %s%s", Snap::StateName(state).c_str(), inLife ? "" : "  (outside its use)");
                    if (last) ImGui::Text("barrier %s -> %s", Snap::StateName(last->before).c_str(),
                                          Snap::StateName(last->after).c_str());
                    if (!glyph.empty()) ImGui::Text("declared %s", glyph.c_str());
                    if (!pass.Ran()) ImGui::TextDisabled("did not run (%s)", Snap::SkipName(pass.skip));
                    ImGui::EndTooltip();
                }
                if (ImGui::IsItemClicked())
                {
                    m_pick = Pick::Pass;
                    m_pickIndex = p;
                }
                ImGui::PopID();
                if (barrier)
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float h = ImGui::GetTextLineHeight();
                    dl->AddTriangleFilled(ImVec2(at.x - 2.f, at.y), ImVec2(at.x + 4.f, at.y + h * 0.5f),
                                          ImVec2(at.x - 2.f, at.y + h), IM_COL32(255, 255, 255, 200));
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!m.aliasPairs.empty())
    {
        ImGui::BeginChild("##rg_alias", ImVec2(0, aliasHeight), true);
        ImGui::TextDisabled("Alias candidates - graph-owned textures with one size and format whose uses do not overlap "
                            "(the input RG.M6 needs; nothing is aliased here):");
        for (const auto& pr : m.aliasPairs)
        {
            const Snap::Texture& x = s.textures[pr[0]];
            const Snap::Texture& y = s.textures[pr[1]];
            ImGui::BulletText("%s (%s .. %s)  <->  %s (%s .. %s)   %u x %u %s", x.name.c_str(),
                              m.PassName(static_cast<uint32_t>(x.firstUse)).c_str(),
                              m.PassName(static_cast<uint32_t>(x.lastUse)).c_str(), y.name.c_str(),
                              m.PassName(static_cast<uint32_t>(y.firstUse)).c_str(),
                              m.PassName(static_cast<uint32_t>(y.lastUse)).c_str(), x.width, x.height,
                              RHI::FormatName(static_cast<RHI::Format>(x.format)));
        }
        ImGui::EndChild();
    }
}

// ==== the Frame tab: the queues' command lists ====
void RenderGraphWindow::DrawFrame()
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    if (!m.device)
    {
        ImGui::TextDisabled("This capture has no device frame.");
        return;
    }
    const Device& d = *m.device;
    const RG::FrameCapture& cap = *m.cap;
    const auto& cv = cap.coverage;
    ImGui::TextDisabled("lists %u  batches %u  barriers %u (native %u)  |  handoffs %u: covered %u  hidden %u  readRead %u  "
                        "intraPass %u  crossQueue %u  unordered %u  |  state mismatches %u",
                        cv.commandLists, cv.batches, cv.barriers, cv.nativeBarriers, cv.observedEdges, cv.coveredEdges,
                        cv.hiddenEdges, cv.readReadEdges, cv.intraPassEdges, cv.crossQueueEdges, cv.unorderedEdges,
                        cv.stateMismatches);
    ImGui::Checkbox("Waits", &m_showWaits);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A list's declared waits (waitFor) - grey; across queues, the fence wait EndFrame issued - yellow.");
    ImGui::SameLine();
    ImGui::Checkbox("Hidden handoffs", &m_showHandoffs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Observed handoffs (one list left a resource in a state, a later one moved it on) that\n"
                          "no declaration keeps (hidden) or that no wait orders (unordered) - red.");
    ImGui::SameLine();
    ImGui::Checkbox("Fit##lists", &m_fitLists);
    if (!m_fitLists)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.f);
        ImGui::SliderFloat("##rg_box", &m_boxWidth, 50.f, 220.f, "list width %.0f");
    }

    constexpr float kLabel = 74.f, kLane = 48.f, kLaneGap = 14.f, kGap = 6.f;
    if (m_fitLists && !d.submitOrder.empty())
        m_boxWidth = std::max(36.f, (ImGui::GetContentRegionAvail().x - kLabel - 30.f) / float(d.submitOrder.size()) - kGap);
    ImGui::BeginChild("##rg_frame", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const size_t submits = d.submitOrder.size();
    ImGui::Dummy(ImVec2(kLabel + submits * (m_boxWidth + kGap) + 20.f, 3.f * (kLane + kLaneGap) + 40.f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int q = 0; q < 3; ++q)
    {
        const float y = origin.y + q * (kLane + kLaneGap);
        dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + kLabel + submits * (m_boxWidth + kGap) + 20.f, y + kLane),
                          IM_COL32(255, 255, 255, 10), 3.f);
        dl->AddText(ImVec2(origin.x + 4.f, y + kLane * 0.5f - 7.f), IM_COL32(200, 200, 200, 255), Device::QueueName(static_cast<uint8_t>(q)));
    }
    // Each list's rectangle, by its place in the submission order.
    struct Box { ImVec2 a, b; bool valid = false; };
    std::vector<Box> box(d.commandLists.size());
    for (size_t k = 0; k < submits; ++k)
    {
        const uint32_t slot = d.submitOrder[k];
        if (slot >= d.commandLists.size()) continue;
        const int q = std::min<int>(d.commandLists[slot].queue, 2);
        const float x = origin.x + kLabel + k * (m_boxWidth + kGap);
        const float y = origin.y + q * (kLane + kLaneGap) + 4.f;
        box[slot] = { ImVec2(x, y), ImVec2(x + m_boxWidth, y + kLane - 8.f), true };
    }
    auto right = [&](uint32_t s2) { return ImVec2(box[s2].b.x, (box[s2].a.y + box[s2].b.y) * 0.5f); };
    auto left  = [&](uint32_t s2) { return ImVec2(box[s2].a.x, (box[s2].a.y + box[s2].b.y) * 0.5f); };
    // Under the boxes: the waits and the hidden / unordered handoffs.
    if (m_showWaits)
        for (uint32_t slot = 0; slot < d.commandLists.size(); ++slot)
        {
            if (!box[slot].valid) continue;
            const Device::CommandList& cl = d.commandLists[slot];
            for (uint32_t on : cl.waitFor)
            {
                if (on >= box.size() || !box[on].valid) continue;
                const bool cross = d.commandLists[on].queue != cl.queue;
                const ImVec2 a = right(on), b = left(slot);
                dl->AddBezierCubic(a, ImVec2(a.x + 30.f, a.y), ImVec2(b.x - 30.f, b.y), b,
                                   cross ? IM_COL32(255, 210, 60, 220) : IM_COL32(150, 150, 150, 110), cross ? 2.5f : 1.f);
            }
        }
    if (m_showHandoffs)
        for (const RG::FrameCapture::ObservedEdge& e : cap.observed)
        {
            if (e.kind != RG::FrameCapture::EdgeClass::Hidden && e.ordered) continue;
            if (e.from >= box.size() || e.to >= box.size() || !box[e.from].valid || !box[e.to].valid) continue;
            const ImVec2 a((box[e.from].a.x + box[e.from].b.x) * 0.5f, box[e.from].b.y);
            const ImVec2 b((box[e.to].a.x + box[e.to].b.x) * 0.5f, box[e.to].b.y);
            const float drop = 18.f + 0.08f * std::fabs(b.x - a.x);
            dl->AddBezierCubic(a, ImVec2(a.x, a.y + drop), ImVec2(b.x, b.y + drop), b, IM_COL32(255, 70, 60, 200), 1.5f);
        }
    // The batches: a bracket under each run of lists one ExecuteCommandLists sent.
    for (const Device::Batch& bt : d.batches)
    {
        if (bt.count == 0 || bt.firstSubmit + bt.count > submits) continue;
        const uint32_t first = d.submitOrder[bt.firstSubmit], last = d.submitOrder[bt.firstSubmit + bt.count - 1];
        if (first >= box.size() || last >= box.size() || !box[first].valid || !box[last].valid) continue;
        const float y = box[first].b.y + 3.f;
        dl->AddLine(ImVec2(box[first].a.x, y), ImVec2(box[last].b.x, y), IM_COL32(120, 200, 255, 160), 2.f);
    }
    // The boxes.
    for (uint32_t slot = 0; slot < d.commandLists.size(); ++slot)
    {
        if (!box[slot].valid) continue;
        const Device::CommandList& cl = d.commandLists[slot];
        const int32_t pass = slot < cap.passOfList.size() ? cap.passOfList[slot] : -1;
        const ImU32 fill = pass >= 0 && static_cast<uint32_t>(pass) < s.passes.size()
                         ? StageColor(s.passes[pass].stage, 0.55f) : IM_COL32(110, 110, 110, 90);
        dl->AddRectFilled(box[slot].a, box[slot].b, fill, 3.f);
        const bool picked = m_pick == Pick::List && m_pickIndex == slot;
        dl->AddRect(box[slot].a, box[slot].b, picked ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 120), 3.f, 0,
                    picked ? 2.f : 1.f);
        uint32_t mismatches = 0;
        for (const RG::FrameCapture::Mismatch& mm : cap.mismatches)
            if (mm.barrier >= cl.firstBarrier && mm.barrier < cl.firstBarrier + cl.barrierCount) ++mismatches;
        dl->PushClipRect(box[slot].a, box[slot].b, true);
        dl->AddText(ImVec2(box[slot].a.x + 3.f, box[slot].a.y + 2.f), IM_COL32(240, 240, 240, 255), ListName(d, slot).c_str());
        char line[48];
        std::snprintf(line, sizeof(line), "%u barriers", cl.barrierCount);
        dl->AddText(ImVec2(box[slot].a.x + 3.f, box[slot].a.y + 18.f), IM_COL32(200, 200, 200, 200), line);
        dl->PopClipRect();
        if (mismatches)
            dl->AddCircleFilled(ImVec2(box[slot].b.x - 6.f, box[slot].a.y + 6.f), 4.f, IM_COL32(255, 60, 60, 255));
        ImGui::SetCursorScreenPos(box[slot].a);
        ImGui::PushID(static_cast<int>(slot));
        ImGui::InvisibleButton("##list", ImVec2(box[slot].b.x - box[slot].a.x, box[slot].b.y - box[slot].a.y));
        if (ImGui::IsItemClicked())
        {
            m_pick = Pick::List;
            m_pickIndex = slot;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nqueue %s  submit #%u  batch %u\n%u barriers  %zu waits%s%s%s", ListName(d, slot).c_str(),
                              Device::QueueName(cl.queue), cl.submitIndex, cl.batch, cl.barrierCount, cl.waitFor.size(),
                              pass >= 0 ? "\npass " : "", pass >= 0 ? m.PassName(static_cast<uint32_t>(pass)).c_str() : "",
                              mismatches ? "\nstate mismatch!" : "");
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void RenderGraphWindow::InspectList(uint32_t slot)
{
    const Model& m = *m_model;
    if (!m.device || slot >= m.device->commandLists.size()) return;
    const Device& d = *m.device;
    const RG::FrameCapture& cap = *m.cap;
    const Device::CommandList& cl = d.commandLists[slot];
    ImGui::TextUnformatted(ListName(d, slot).c_str());
    ImGui::Text("queue %s  |  slot %u  |  submit #%u", Device::QueueName(cl.queue), slot, cl.submitIndex);
    if (cl.batch < d.batches.size())
        ImGui::Text("batch %u  |  signal %llu", cl.batch, static_cast<unsigned long long>(d.batches[cl.batch].signalValue));
    const int32_t pass = slot < cap.passOfList.size() ? cap.passOfList[slot] : -1;
    if (pass >= 0)
    {
        ImGui::TextUnformatted("pass:");
        ImGui::SameLine();
        if (ImGui::SmallButton(m.PassName(static_cast<uint32_t>(pass)).c_str()))
        {
            m_pick = Pick::Pass;
            m_pickIndex = static_cast<uint32_t>(pass);
            return;
        }
    }
    if (!cl.waitFor.empty() && ImGui::CollapsingHeader("Waits for", ImGuiTreeNodeFlags_DefaultOpen))
        for (uint32_t on : cl.waitFor) ImGui::BulletText("%s", ListName(d, on).c_str());
    if (!cl.waits.empty() && ImGui::CollapsingHeader("Fence waits", ImGuiTreeNodeFlags_DefaultOpen))
        for (const Device::Wait& w : cl.waits)
            ImGui::BulletText("%s (%s) value %llu", ListName(d, w.on).c_str(), Device::QueueName(w.queue),
                              static_cast<unsigned long long>(w.value));
    if (cl.barrierCount && ImGui::CollapsingHeader("Barriers", ImGuiTreeNodeFlags_DefaultOpen))
        for (uint32_t k = cl.firstBarrier; k < cl.firstBarrier + cl.barrierCount && k < d.journal.size(); ++k)
        {
            const Device::Barrier& b = d.journal[k];
            std::string sub = b.subresource == Device::kNone ? "" : " [" + std::to_string(b.subresource) + "]";
            if (b.kind == Device::Kind::Transition)
                ImGui::BulletText("%s%s: %s -> %s%s", ResourceName(d, b.resource).c_str(), sub.c_str(),
                                  Device::D3DStateName(b.before).c_str(), Device::D3DStateName(b.after).c_str(),
                                  b.path == Device::Path::Native ? "  (native)" : "");
            else
                ImGui::BulletText("%s %s%s", Device::KindName(b.kind), ResourceName(d, b.resource).c_str(),
                                  b.path == Device::Path::Native ? "  (native)" : "");
        }
    bool header = false;
    for (const RG::FrameCapture::ObservedEdge& e : cap.observed)
    {
        if (e.from != slot && e.to != slot) continue;
        if (!header && !(header = true, ImGui::CollapsingHeader("Handoffs", ImGuiTreeNodeFlags_DefaultOpen))) break;
        const bool bad = e.kind == RG::FrameCapture::EdgeClass::Hidden || !e.ordered;
        if (bad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.45f, 0.4f, 1.f));
        ImGui::BulletText("%s %s: %s in %s  (%s%s%s)", e.to == slot ? "from" : "to",
                          ListName(d, e.to == slot ? e.from : e.to).c_str(), ResourceName(d, e.resource).c_str(),
                          Device::D3DStateName(e.state).c_str(), RG::FrameCapture::ClassName(e.kind),
                          e.ordered ? "" : ", unordered", e.crossQueue ? ", cross-queue" : "");
        if (bad) ImGui::PopStyleColor();
    }
    header = false;
    for (const RG::FrameCapture::Mismatch& mm : cap.mismatches)
    {
        if (mm.barrier < cl.firstBarrier || mm.barrier >= cl.firstBarrier + cl.barrierCount || mm.barrier >= d.journal.size())
            continue;
        if (!header && !(header = true, ImGui::CollapsingHeader("State mismatches", ImGuiTreeNodeFlags_DefaultOpen))) break;
        const Device::Barrier& b = d.journal[mm.barrier];
        ImGui::BulletText("%s: before %s, left in %s by %s", ResourceName(d, b.resource).c_str(),
                          Device::D3DStateName(b.before).c_str(), Device::D3DStateName(mm.expected).c_str(),
                          ListName(d, mm.lastList).c_str());
    }
}

// ==== the Graph tab: the passes as nodes ====
void RenderGraphWindow::DrawGraph()
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("WAR", &m_showWar);
    ImGui::SameLine();
    ImGui::Checkbox("WAW", &m_showWaw);
    ImGui::SameLine();
    ImGui::Checkbox("Order", &m_showOrder);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The RG::Order constraints (RG.M5), green.");
    ImGui::SameLine();
    ImGui::Checkbox("Inactive##graph", &m_graphInactive);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also the passes that did not run, laid out on the static DAG (every pass treated as\n"
                          "running) - much wider: the HDR target's write chain is ~30 layers long.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.f);
    const char* texLabel = m_texFilter >= 0 && m_texFilter < static_cast<int>(s.textures.size())
                         ? s.textures[m_texFilter].name.c_str() : "every texture";
    if (ImGui::BeginCombo("##rg_tex", texLabel))
    {
        if (ImGui::Selectable("every texture", m_texFilter < 0)) m_texFilter = -1;
        for (int t = 0; t < static_cast<int>(s.textures.size()); ++t)
            if (ImGui::Selectable(s.textures[t].name.c_str(), m_texFilter == t)) m_texFilter = t;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) m_fit = true;

    if (!m_nodeContext) m_nodeContext = NodeGraphView::CreateContext();
    ed::SetCurrentEditor(static_cast<ed::EditorContext*>(m_nodeContext));
    ed::Begin("##rg_graph");
    const auto shown = [&](uint32_t p) { return p < s.passes.size() && (m_graphInactive || s.passes[p].Ran()); };

    // The layout of what is drawn: the passes that ran on this frame's DAG, or
    // (Inactive) every pass on the static DAG; the order constraints too.
    {
        uint64_t key = m.layoutKey;
        for (uint32_t i = 0; i < s.passes.size(); ++i) key = (key ^ (shown(i) ? 1u : 2u)) * 1099511628211ull;
        if (key != m_layoutKey)
        {
            std::vector<uint32_t> index(s.passes.size(), ~0u), passOf;
            for (uint32_t i = 0; i < s.passes.size(); ++i)
                if (shown(i)) { index[i] = static_cast<uint32_t>(passOf.size()); passOf.push_back(i); }
            std::vector<std::vector<size_t>> inputs(passOf.size());
            auto add = [&](uint32_t from, uint32_t to)
            {
                if (from >= index.size() || to >= index.size() || index[from] == ~0u || index[to] == ~0u || from == to) return;
                std::vector<size_t>& in = inputs[index[to]];
                if (std::find(in.begin(), in.end(), index[from]) == in.end()) in.push_back(index[from]);
            };
            for (const Snap::Dependency& e : m_graphInactive ? m.staticEdges : s.edges) add(e.from, e.to);
            for (const auto& o : m.orderEdges) add(o[0], o[1]);
            constexpr float dx = 240.f, dy = 96.f, gap = 70.f;
            std::vector<std::array<float, 2>> compact = GraphLayout::LayersOrdered(inputs, dx, dy);
            // A frame is mostly one chain of HDR writers: its layers make a long
            // thin strip. Wrap them into bands stacked top to bottom - as many as
            // bring the whole near the canvas's aspect.
            int layers = 0;
            float tall = 0.f;
            for (const auto& p : compact)
            {
                layers = std::max(layers, static_cast<int>(p[0] / dx + 0.5f) + 1);
                tall = std::max(tall, p[1] + dy);
            }
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const float aspect = area.y > 1.f ? std::max(0.8f, area.x / area.y) : 1.6f;
            int perBand = layers;
            for (int bands = 1; bands <= layers; ++bands)
            {
                const int k = (layers + bands - 1) / bands;
                const float w = k * dx, h = bands * (tall + gap);
                perBand = k;
                if (w / h <= aspect) break;
            }
            for (auto& p : compact)
            {
                const int layer = static_cast<int>(p[0] / dx + 0.5f);
                p = { (layer % perBand) * dx, p[1] + (layer / perBand) * (tall + gap) };
            }
            m_layout.assign(s.passes.size(), { 0.f, 0.f });
            for (size_t k = 0; k < passOf.size(); ++k) m_layout[passOf[k]] = compact[k];
            m_layoutKey = key;
        }
    }
    const bool place = m_placedLayout != m_layoutKey;

    // The nodes.
    for (uint32_t i = 0; i < s.passes.size(); ++i)
    {
        if (!shown(i)) continue;
        const Snap::Pass& p = s.passes[i];
        const bool ran = p.Ran();
        if (place && i < m_layout.size()) ed::SetNodePosition(ed::NodeId(i + 1), ImVec2(m_layout[i][0], m_layout[i][1]));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImGui::ColorConvertU32ToFloat4(StageColor(p.stage, ran ? 1.f : 0.35f)));
        ed::PushStyleColor(ed::StyleColor_NodeBg, ran ? ImVec4(0.12f, 0.12f, 0.13f, 0.90f) : ImVec4(0.09f, 0.09f, 0.09f, 0.55f));
        ed::BeginNode(ed::NodeId(i + 1));
        ed::BeginPin(NodeGraphView::Pin(i, 0, false), ed::PinKind::Input);
        ed::PinPivotAlignment(ImVec2(0.f, 0.5f));
        ImGui::TextDisabled(">");
        ed::EndPin();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, StageColor(p.stage, ran ? 1.f : 0.5f));
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ed::BeginPin(NodeGraphView::Pin(i, 0, true), ed::PinKind::Output);
        ed::PinPivotAlignment(ImVec2(1.f, 0.5f));
        ImGui::TextDisabled(">");
        ed::EndPin();
        if (ran)
            ImGui::Text("CPU %s  GPU %s", Ms(p.cpuMs).c_str(), Ms(p.gpuMs).c_str());
        else
            ImGui::TextDisabled("%s  (%s)", p.stage, Snap::SkipName(p.skip));
        if (p.externalWaits || !p.barriers.empty())
            ImGui::TextDisabled("barriers %zu  waits %u", p.barriers.size() + p.uavBarriers.size(), p.externalWaits);
        if (!m.findingsOfPass[i].empty())
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%zu finding(s)", m.findingsOfPass[i].size());
        if (m.movedBySort[i]) ImGui::TextDisabled("moved by its order (registered #%u)", p.registered);
        if (p.unread) ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "unread");
        ed::EndNode();
        ed::PopStyleColor(2);
    }

    // The links: this frame's DAG, the constraints, the static edges of the passes that did not run.
    struct Drawn { uint32_t from, to, texture; int kind; uint8_t planes; };   // kind: 0-2 Edge, 3 order, 4 static
    std::vector<Drawn> links;
    for (const Snap::Dependency& e : s.edges)
    {
        if ((e.kind == Snap::Edge::WAR && !m_showWar) || (e.kind == Snap::Edge::WAW && !m_showWaw)) continue;
        if (m_texFilter >= 0 && e.texture != static_cast<uint32_t>(m_texFilter)) continue;
        if (shown(e.from) && shown(e.to)) links.push_back({ e.from, e.to, e.texture, static_cast<int>(e.kind), e.planes });
    }
    if (m_showOrder && m_texFilter < 0)
        for (const auto& o : m.orderEdges)
            if (shown(o[0]) && shown(o[1])) links.push_back({ o[0], o[1], ~0u, 3, 0 });
    if (m_graphInactive)
        for (const Snap::Dependency& e : m.staticEdges)
        {
            if (e.kind != Snap::Edge::RAW || (s.passes[e.from].Ran() && s.passes[e.to].Ran())) continue;
            if (m_texFilter >= 0 && e.texture != static_cast<uint32_t>(m_texFilter)) continue;
            links.push_back({ e.from, e.to, e.texture, 4, e.planes });
        }
    for (size_t k = 0; k < links.size(); ++k)
    {
        const Drawn& l = links[k];
        const ImVec4 color = l.kind == 0 ? kRawColor : l.kind == 1 ? kWarColor : l.kind == 2 ? kWawColor
                           : l.kind == 3 ? kOrderColor : kStaticColor;
        ed::Link(ed::LinkId(kLinkBase + k), NodeGraphView::Pin(l.from, 0, true), NodeGraphView::Pin(l.to, 0, false), color,
                 l.kind == 0 ? 2.f : 1.2f);
    }

    // Selection both ways, and the tips.
    // A node clicked here picks its pass; a pass picked elsewhere (the
    // navigator, the Inspector) is selected here. m_syncedPick is the pass the
    // two last agreed on.
    ed::NodeId selected;
    const int64_t clicked = ed::GetSelectedNodes(&selected, 1) > 0 ? static_cast<int64_t>(selected.Get()) - 1 : -1;
    if (clicked >= 0 && clicked != m_syncedPick && clicked < static_cast<int64_t>(s.passes.size()))
    {
        m_pick = Pick::Pass;
        m_pickIndex = static_cast<uint32_t>(clicked);
        m_syncedPick = clicked;
    }
    else if (m_pick == Pick::Pass && m_syncedPick != static_cast<int64_t>(m_pickIndex) && shown(m_pickIndex))
    {
        ed::ClearSelection();
        ed::SelectNode(ed::NodeId(m_pickIndex + 1));
        m_navDelay = 3;
        m_syncedPick = m_pickIndex;
    }
    if (const ed::LinkId hovered = ed::GetHoveredLink())
    {
        const size_t k = static_cast<size_t>(hovered.Get() - kLinkBase);
        if (k < links.size())
        {
            const Drawn& l = links[k];
            static const char* const kKinds[] = { "RAW", "WAR", "WAW", "order", "static RAW" };
            ed::Suspend();
            if (l.kind == 3)
                ImGui::SetTooltip("order: %s before %s", m.PassName(l.from).c_str(), m.PassName(l.to).c_str());
            else
                ImGui::SetTooltip("%s  %s%s%s\n%s -> %s", kKinds[l.kind], m.TexName(l.texture).c_str(),
                                  l.texture < s.textures.size() && s.textures[l.texture].planar ? "  " : "",
                                  l.texture < s.textures.size() && s.textures[l.texture].planar ? Snap::PlanesName(l.planes) : "",
                                  m.PassName(l.from).c_str(), m.PassName(l.to).c_str());
            ed::Resume();
        }
    }
    if (place || m_fit)
    {
        m_fitDelay = 3;
        m_fit = false;
    }
    if (m_fitDelay > 0 && --m_fitDelay == 0 && m_navDelay == 0) ed::NavigateToContent(0.f);
    if (m_navDelay > 0 && --m_navDelay == 0) ed::NavigateToSelection(false, 0.f);
    m_placedLayout = m_layoutKey;
    ed::End();
    ed::SetCurrentEditor(nullptr);
}

// ==== the inspector ====
void RenderGraphWindow::DrawInspector()
{
    ImGui::BeginChild("##rg_inspector", ImVec2(0, 0), false);
    if (m_pick == Pick::Pass)         InspectPass(m_pickIndex);
    else if (m_pick == Pick::Texture) InspectTexture(m_pickIndex);
    else if (m_pick == Pick::List)    InspectList(m_pickIndex);
    else                              ImGui::TextDisabled("Select a pass, a texture or (Frame) a command list.");
    ImGui::EndChild();
}

void RenderGraphWindow::InspectPass(uint32_t i)
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    if (i >= s.passes.size()) return;
    const Snap::Pass& p = s.passes[i];
    ImGui::PushStyleColor(ImGuiCol_Text, StageColor(p.stage));
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::PopStyleColor();
    ImGui::Text("stage %s  |  declared #%u  |  registered #%u", p.stage, p.index, p.registered);
    ImGui::Text("ran %s  |  CPU %s ms  |  GPU %s ms", p.Ran() ? "yes" : Snap::SkipName(p.skip),
                Ms(p.cpuMs).c_str(), Ms(p.gpuMs).c_str());
    // The ShowFlags it draws under, and the ones this view hid.
    if (p.showFlags)
    {
        const uint32_t missing = p.showFlags & ~s.viewShow;
        if (missing)
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "hidden: view %s does not show %s",
                               s.viewName.c_str(), RG::ShowFlagList(missing, ", ").c_str());
        else
            ImGui::TextDisabled("shown with %s", RG::ShowFlagList(p.showFlags, ", ").c_str());
    }
    if (p.Ran())
        ImGui::Text("list cmd%u%s  |  external waits %u", p.cl,
                    p.auxTailCl != ~0u ? (" (tail cmd" + std::to_string(p.auxTailCl) + ")").c_str() : "",
                    p.externalWaits);
    if (p.unread) ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "unread: nothing in the graph reads what it wrote");
    if (!p.order.empty() && ImGui::CollapsingHeader("Order", ImGuiTreeNodeFlags_DefaultOpen))
        for (const RG::OrderRule& r : p.order)
        {
            ImGui::BulletText("%s %s", RG::OrderKindName(r.kind), r.pass.c_str());
            ImGui::Indent();
            ImGui::TextWrapped("%s", r.why.c_str());
            ImGui::Unindent();
        }
    if (ImGui::CollapsingHeader("Accesses", ImGuiTreeNodeFlags_DefaultOpen))
        for (const Snap::PassAccess& a : p.accesses)
        {
            std::string extra;
            if (a.plane >= 0) extra += std::string(" ") + Snap::PlaneName(a.plane);
            if (a.plane == 1 && a.stencilMask != 0xFF)
            {
                char b[16];
                std::snprintf(b, sizeof(b), " 0x%02X", a.stencilMask);
                extra += b;
            }
            if (a.kind == Snap::Access::RenderTarget && a.load) extra += std::string(" ") + Snap::LoadName(a.load);
            if ((a.kind == Snap::Access::Manual && a.entryState) || a.kind == Snap::Access::Scoped)
                extra += " " + Snap::StateName(a.entryState);
            ImGui::PushID(&a);
            if (ImGui::Selectable((std::string(Snap::AccessName(a.kind)) + "  " + m.TexName(a.texture) + extra).c_str()))
            {
                m_pick = Pick::Texture;
                m_pickIndex = a.texture;
            }
            ImGui::PopID();
        }
    if (!p.barriers.empty() && ImGui::CollapsingHeader("Barriers", ImGuiTreeNodeFlags_DefaultOpen))
        for (const Snap::Barrier& b : p.barriers)
            ImGui::BulletText("%s%s%s: %s -> %s", m.TexName(b.texture).c_str(), b.plane >= 0 ? " " : "",
                              b.plane >= 0 ? Snap::PlaneName(b.plane) : "", Snap::StateName(b.before).c_str(),
                              Snap::StateName(b.after).c_str());
    if (!p.uavBarriers.empty() && ImGui::CollapsingHeader("UAV barriers"))
        for (uint32_t t : p.uavBarriers) ImGui::BulletText("%s", m.TexName(t).c_str());
    if (!m.findingsOfPass[i].empty() && ImGui::CollapsingHeader("Findings", ImGuiTreeNodeFlags_DefaultOpen))
        for (uint32_t f : m.findingsOfPass[i])
        {
            const Snap::Finding& x = s.findings[f];
            ImGui::BulletText("%s%s%s%s%s", Snap::CheckName(x.check), x.texture != ~0u ? "  " : "",
                              x.texture != ~0u ? m.TexName(x.texture).c_str() : "", x.other != ~0u ? "  after " : "",
                              x.other != ~0u ? m.PassName(x.other).c_str() : "");
        }
    if (!p.undeclared.empty() && ImGui::CollapsingHeader("Undeclared"))
        for (uint32_t t : p.undeclared) ImGui::BulletText("%s", m.TexName(t).c_str());
}

void RenderGraphWindow::InspectTexture(uint32_t t)
{
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    if (t >= s.textures.size()) return;
    const Snap::Texture& x = s.textures[t];
    ImGui::TextUnformatted(x.name.c_str());
    ImGui::Text("%u x %u%s  |  %s", x.width, x.height,
                x.arraySize > 1 ? (" x" + std::to_string(x.arraySize)).c_str() : "",
                RHI::FormatName(static_cast<RHI::Format>(x.format)));
    ImGui::Text("%s%s%s%s%s", x.isDepth ? "depth  " : "", x.planar ? "two planes  " : "", x.isUAV ? "UAV  " : "",
                x.onDemand ? "on demand  " : "", x.backed ? "backed" : "not backed");
    if (x.imported)
        ImGui::Text("import  |  entry %s  |  tracking %s%s%s",
                    RG::ImportEntryName(static_cast<RG::ImportEntry>(x.importEntry)),
                    RG::ImportTrackingName(static_cast<RG::ImportTracking>(x.importTracking)),
                    x.deviceName.empty() ? "" : "  |  ", x.deviceName.c_str());
    ImGui::Text("state %s -> %s", Snap::StateName(x.initialState).c_str(), Snap::StateName(x.finalState).c_str());
    if (x.planar)
        ImGui::Text("stencil %s -> %s", Snap::StateName(x.stencilInitialState).c_str(),
                    Snap::StateName(x.stencilFinalState).c_str());
    if (x.firstUse >= 0)
        ImGui::Text("used %s .. %s", m.PassName(static_cast<uint32_t>(x.firstUse)).c_str(),
                    m.PassName(static_cast<uint32_t>(x.lastUse)).c_str());
    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen))
        DrawPreview(t);
    auto passes = [&](const char* title, const std::vector<uint32_t>& list)
    {
        if (list.empty() || !ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) return;
        for (uint32_t p : list)
        {
            const bool ran = p < s.passes.size() && s.passes[p].Ran();
            ImGui::PushID(static_cast<int>(p));
            if (!ran) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(m.PassName(p).c_str()))
            {
                m_pick = Pick::Pass;
                m_pickIndex = p;
            }
            if (!ran) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    };
    passes("Written by", m.writers[t]);
    passes("Read by", m.readers[t]);
}

// ==== the texture Inspector's preview ====
// The Renderer's pass capture (Renderer/PassCapture.h): the texture as the frame
// left it after the chosen pass, copied in the graph's own chain, painted with
// the debug target shader. One request per Capture / Live tick / change.
void RenderGraphWindow::DrawPreview(uint32_t t)
{
    m_previewDrawn = true;
    const Model& m = *m_model;
    const Snap& s = *m.snap;
    const Snap::Texture& x = s.textures[t];
    if (!m_renderer)
    {
        ImGui::TextDisabled("no renderer in this host");
        return;
    }
    if (m_previewTexture != x.name)   // another texture: start over from its frame's end
    {
        m_previewTexture = x.name;
        m_previewAfter.clear();
        m_previewMode    = 0;
        m_previewChannel = -1;
        m_previewPlane   = 0;
        m_previewRange   = false;
        m_previewAsk     = true;
    }

    // After which pass: the frame's end, or a pass that ran and touched it.
    int lastRan = -1;
    for (uint32_t p = 0; p < s.passes.size(); ++p)
        if (s.passes[p].Ran()) lastRan = static_cast<int>(p);
    const std::string endLabel = std::string("the frame's end") +
                                 (lastRan >= 0 ? " (after " + s.passes[lastRan].name + ")" : std::string());
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##rg_preview_after", m_previewAfter.empty() ? endLabel.c_str()
                                                                       : ("after " + m_previewAfter).c_str()))
    {
        if (ImGui::Selectable(endLabel.c_str(), m_previewAfter.empty()))
        {
            m_previewAfter.clear();
            m_previewAsk = true;
        }
        for (uint32_t p = 0; p < s.passes.size(); ++p)
        {
            const Snap::Pass& pass = s.passes[p];
            if (!pass.Ran()) continue;
            bool reads = false, writes = false;
            for (const Snap::PassAccess& a : pass.accesses)
                if (a.texture == t) (Writes(a.kind) ? writes : reads) = true;
            if (!reads && !writes) continue;
            const std::string label = "after " + pass.name + (writes ? "  (W)" : "");
            if (ImGui::Selectable(label.c_str(), m_previewAfter == pass.name))
            {
                m_previewAfter = pass.name;
                m_previewAsk   = true;
            }
        }
        ImGui::EndCombo();
    }

    // How it is painted (Render.target's options).
    static const char* const kModes[] = { "auto", "raw", "channel", "hdr", "depth", "velocity", "signed", "heat",
                                          "categorical", "bits", "ev" };
    ImGui::SetNextItemWidth(110.f);
    if (ImGui::Combo("##rg_preview_mode", &m_previewMode, kModes, IM_ARRAYSIZE(kModes))) m_previewAsk = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    if (ImGui::SliderInt("##rg_preview_ch", &m_previewChannel, -1, 3, m_previewChannel < 0 ? "ch auto" : "ch %d"))
        m_previewAsk = true;
    if (x.planar)
    {
        ImGui::SameLine();
        if (ImGui::RadioButton("depth", m_previewPlane == 0)) { m_previewPlane = 0; m_previewAsk = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("stencil", m_previewPlane == 1)) { m_previewPlane = 1; m_previewAsk = true; }
    }
    if (ImGui::Checkbox("range", &m_previewRange)) m_previewAsk = true;
    if (m_previewRange)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::DragFloatRange2("##rg_preview_range", &m_previewMin, &m_previewMax, 0.01f)) m_previewAsk = true;
    }
    if (m_previewMode == static_cast<int>(DebugTargets::Mode::Hdr) ||
        m_previewMode == static_cast<int>(DebugTargets::Mode::Ev))
    {
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderFloat("##rg_preview_exposure", &m_previewExposure, 0.01f, 16.f, "exposure %.2f",
                               ImGuiSliderFlags_Logarithmic))
            m_previewAsk = true;
    }
    if (ImGui::Button("Capture")) m_previewAsk = true;
    ImGui::SameLine();
    ImGui::Checkbox("Live##preview", &m_previewLive);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Capture again with the panel's refresh (every 0.5 s): one texture copy each time.");

    // Ask: once per change / Capture, or with the Live refresh.
    const double now = ImGui::GetTime();
    if (m_previewAsk || (m_previewLive && now - m_previewAskedAt >= 0.5))
    {
        PassCapture::Desc d;
        d.texture        = x.name;
        d.afterPass      = m_previewAfter;
        d.picture        = true;
        d.plane          = m_previewPlane;
        d.view.mode      = static_cast<DebugTargets::Mode>(m_previewMode);
        d.view.channel   = m_previewChannel;
        d.view.exposure  = m_previewExposure;
        if (m_previewRange)
        {
            d.view.rangeMin = m_previewMin;
            d.view.rangeMax = m_previewMax;
            d.view.minGiven = d.view.maxGiven = true;
        }
        d.id = m_previewId = m_previewNextId++;
        m_renderer->RequestPassCapture(d);
        m_previewAsk     = false;
        m_previewAskedAt = now;
    }

    // What came back for the last request.
    const PassCapture::Status st = m_renderer->GetPassCaptureStatus();
    if (st.id != m_previewId || st.seq == 0)
    {
        ImGui::TextDisabled("copying ...");
        return;
    }
    if (st.seq != m_previewLoggedSeq)
    {
        m_previewLoggedSeq = st.seq;
        LOG_INFO("[RG_PANEL] preview texture=%s after=%s frame=%llu done=%d picture=%d why=%s", st.texture.c_str(),
                 st.afterPass.empty() ? "-" : st.afterPass.c_str(), static_cast<unsigned long long>(st.frame),
                 st.done ? 1 : 0, st.pictureSrv != 0 ? 1 : 0,
                 !st.why.empty() ? st.why.c_str() : !st.pictureWhy.empty() ? st.pictureWhy.c_str() : "-");
    }
    if (!st.done)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 110, 255));
        ImGui::TextWrapped("%s%s", st.pending ? "waiting: " : "", st.why.c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::TextDisabled("frame %llu  |  after %s  |  %s", static_cast<unsigned long long>(st.frame),
                        st.afterPass.c_str(), DebugTargets::ModeName(st.shown.mode));
    if (!st.pictureSrv)
    {
        ImGui::TextWrapped("%s", st.pictureWhy.empty() ? "no picture" : st.pictureWhy.c_str());
        return;
    }
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = st.pictureW ? w * static_cast<float>(st.pictureH) / static_cast<float>(st.pictureW) : w;
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(st.pictureSrv)), ImVec2(w, h));
}

// The pass capture let go: the Renderer releases its copy and picture later.
void RenderGraphWindow::ReleasePreview()
{
    if (m_previewId == 0 || !m_renderer) return;
    m_renderer->ReleasePassCapture();
    m_previewId = 0;
    m_previewTexture.clear();
    m_previewLive = false;
}
}
