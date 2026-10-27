// The editor's Profiler window. Tabs: Overview (frame / CPU / GPU headline, the FrameTiming
// history shared with the HUD, a boundness strip), Timeline (the profiler's flight recorder: one
// lane per thread and per GPU queue; zoom / pan / hover), Threads (per-thread work / wait / idle
// bars, the concurrency histogram), Passes (sortable GPU pass table), Memory (VRAM budget, the GPU
// resource ledger, gauges, heap traffic), Captures (capture windows, hitch arming, files written).
//
// Data: GPU numbers come only from the profiler's thread-safe snapshots (Diag::Prof::GetGpuFrame /
// GetFrameRecord / CopyLiveTimeline). Do not read GPUProfiler::results here: the render-owner lane
// may be writing it.
//
// Ownership: while open the window holds a GPU-profiler want (never a write to
// GPUProfiler::enabled) and, when no -profile* flag started a collector, one of its own at level
// Timeline that writes capture windows only to log/profile/<stamp>/. Closing releases both; a
// collector the command line started is never touched.
//
// Harness: -ui_open_menu=Profiler@<frame> opens it, Profiler/<Tab>@<frame> selects a tab, for a
// -ui_screenshot of a visible run (ImGui draws nothing under -hidden).

#include "Editor/EditorLayer.h"
#include "Diagnostics/FrameTiming.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerCollector.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Diagnostics/ProfilerGpu.h"
#include "Diagnostics/ProfilerMemory.h"
#include "Graphics/IGraphicsDevice.h"
#include "Renderer/Renderer.h"
#include "System/PathUtil.h"
#include "imgui/imgui.h"
#include "implot/implot.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace P = Diag::Prof;

struct EditorLayer::ProfilerPanelState
{
    // One row of the Passes table.
    struct PassRow
    {
        float    ema = 0.f, last = 0.f, peak = 0.f;
        uint8_t  queue = 0, depth = 0;
        uint32_t lastSeen = 0, lastActive = 0;
    };
    // One plotted series: the last kPlotLen samples.
    static constexpr int kPlotLen = 240;
    struct Ring
    {
        float v[kPlotLen]{};
        int   head = 0, count = 0;   // head = the next write; the oldest sample is head - count

        void  Push(float x) { v[head] = x; head = (head + 1) % kPlotLen; count = count < kPlotLen ? count + 1 : count; }
        float At(int i) const { return v[(head - count + i + kPlotLen) % kPlotLen]; }   // i = 0 is the oldest
    };

    bool held = false, ownsCollector = false, gpuWant = false;

    // ---- Timeline ----
    P::LiveTimeline tl;
    double lastCopy = -1.0;
    bool   paused = false, fit = true;
    int    frames = 6;
    double view0 = 0.0, view1 = 0.0;   // visible range, ms from the first frame's loop top

    // ---- Passes: EMA per name over the newest GPU frames ----
    std::unordered_map<uint16_t, PassRow> passes;
    uint64_t lastGpuFrame = ~0ull;
    uint32_t gpuTick = 0;
    float    busyEma = 0.f, queueEma[3]{}, overlapEma = 0.f;
    bool     showIdle = false;
    float    activeMs = 0.005f;
    ImGuiTextFilter passFilter;
    P::GpuFrameData gpu;              // 24 KB scratch: in the heap-allocated state, not on the stack

    // ---- Plots: rings filled once per frame by SampleModel, read by the draws ----
    float    budgetMs = 1000.f / 60.f;   // the frame-time plot's budget line
    Ring     vramMB, ledgerMB;   // MB: the usage lines (growth is what the plot is for; the bar has the budget)
    uint64_t lastMemFrame = ~0ull;
    // The passes ticked "Plot" in the Passes table: their GPU ms, one sample per GPU frame.
    std::unordered_map<uint16_t, Ring> passHistory;

    // ---- Captures ----
    char  captureName[64] = "capture";
    int   captureFrames = 120;
    char  windowName[64] = "window";
    bool  windowOpen = false;
    float hitchMs = 33.f;
    int   hitchMax = 3;
    std::string lastAction;
};

namespace
{
    const ImU32 kCatCol[9] = {
        IM_COL32(110, 200, 120, 255),   // work
        IM_COL32(235, 160,  70, 255),   // spin
        IM_COL32( 70,  70,  78, 255),   // idle
        IM_COL32( 90, 140, 220, 255),   // join
        IM_COL32(170, 110, 220, 255),   // gpu wait
        IM_COL32( 80, 200, 210, 255),   // pacing
        IM_COL32(220, 200,  90, 255),   // other wait
        IM_COL32(255, 255, 255, 255),   // marker
        IM_COL32(140,  60,  60, 255),   // untracked
    };
    const char* const kCatLabel[9] = { "work", "spin", "idle", "join", "gpu", "pacing", "other", "marker", "untracked" };
    const char* const kQueueName[3] = { "Graphics", "Compute", "Copy" };
    const ImU32 kBoundCol[5] = {
        IM_COL32( 60,  60,  60, 255),   // unknown
        IM_COL32(170, 110, 220, 255),   // gpu
        IM_COL32(235, 160,  70, 255),   // cpu
        IM_COL32( 80, 200, 210, 255),   // pacing
        IM_COL32(150, 150, 150, 255),   // mixed
    };

    ImU32 NameColor(uint16_t id)
    {
        // Stable per name: the golden-ratio hue walk keeps neighbours apart.
        const float h = std::fmod(0.13f + 0.61803398875f * float(id), 1.0f);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(h, 0.45f, 0.80f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, 1.f));
    }

    std::string Stamp()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char b[32];
        std::strftime(b, sizeof(b), "%Y%m%d_%H%M%S", &tm);
        return b;
    }

    // The newest record (within `back` frames) whose `ok` half has arrived.
    template <class Ok>
    bool NewestRecord(int back, Ok ok, P::FrameRecord& out)
    {
        const uint64_t latest = P::LatestRecordedFrame();
        if (latest == ~0ull) return false;
        for (int k = 0; k <= back && uint64_t(k) <= latest; ++k)
            if (P::GetFrameRecord(latest - uint64_t(k), out) && ok(out)) return true;
        return false;
    }

    void Swatch(ImU32 col, const char* label)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  s = ImGui::GetTextLineHeight() * 0.8f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + s, p.y + 2 + s), col);
        ImGui::Dummy(ImVec2(s, s));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0.f, 14.f);
    }

    // The @p p quantile (0..1) of @p n samples; 0 for none.
    float Percentile(const float* v, int n, float p)
    {
        if (n <= 0) return 0.f;
        std::vector<float> s(v, v + n);
        const int k = std::clamp(static_cast<int>(p * float(n - 1) + 0.5f), 0, n - 1);
        std::nth_element(s.begin(), s.begin() + k, s.end());
        return s[k];
    }

    // A ring as a line over "samples ago" (the newest at 0).
    void PlotRing(const char* label, const EditorLayer::ProfilerPanelState::Ring& ring)
    {
        float xs[EditorLayer::ProfilerPanelState::kPlotLen], ys[EditorLayer::ProfilerPanelState::kPlotLen];
        for (int i = 0; i < ring.count; ++i)
        {
            xs[i] = float(i - (ring.count - 1));
            ys[i] = ring.At(i);
        }
        ImPlot::PlotLine(label, xs, ys, ring.count);
    }

    // ==== GPU pass EMAs (shared by Overview and Passes) ====
    void UpdateGpu(EditorLayer::ProfilerPanelState& st)
    {
        const uint64_t gf = P::LatestGpuFrame();
        if (gf == ~0ull || gf == st.lastGpuFrame || !P::GetGpuFrame(gf, st.gpu)) return;
        st.lastGpuFrame = gf;
        ++st.gpuTick;
        constexpr float a = 0.05f;
        auto ema = [&](float& v, float x) { v = (v <= 0.f) ? x : v + a * (x - v); };
        ema(st.busyEma, st.gpu.busyMs);
        for (int q = 0; q < 3; ++q) ema(st.queueEma[q], st.gpu.queueMs[q]);
        ema(st.overlapEma, st.gpu.overlapMs);
        // One frame's time per name (a name may open several regions).
        std::unordered_map<uint16_t, float> frameMs;
        for (uint32_t i = 0; i < st.gpu.spanCount; ++i)
        {
            const P::GpuSpan& sp = st.gpu.spans[i];
            frameMs[sp.name] += float(sp.endNs - sp.beginNs) * 1e-6f;
            auto& row = st.passes[sp.name];
            row.queue = sp.queue;
            row.depth = (row.lastSeen == 0) ? sp.depth : std::min<uint8_t>(row.depth, sp.depth);
        }
        for (auto& [name, ring] : st.passHistory)
        {
            const auto it = frameMs.find(name);
            ring.Push(it != frameMs.end() ? it->second : 0.f);
        }
        for (auto& [name, ms] : frameMs)
        {
            auto& row = st.passes[name];
            row.ema  = (row.lastSeen == 0) ? ms : row.ema + a * (ms - row.ema);
            row.last = ms;
            row.peak = std::max(row.peak * 0.995f, ms);
            row.lastSeen = st.gpuTick;
            if (ms > st.activeMs) row.lastActive = st.gpuTick;
        }
        // Retire rows the GPU has not reported for ~2 s, so a switched-off pass
        // does not keep its last cost on screen.
        for (auto it = st.passes.begin(); it != st.passes.end();)
            it = (st.gpuTick - it->second.lastSeen > 120) ? st.passes.erase(it) : std::next(it);
    }

    // ==== The model ====
    // Once per frame while the window is visible, whichever tab is shown: the GPU pass EMAs and the
    // plotted rings. The tabs only draw.
    void SampleModel(EditorLayer::ProfilerPanelState& st)
    {
        UpdateGpu(st);
        constexpr double kMB = 1048576.0;
        P::FrameRecord m;
        if (NewestRecord(8, [](const P::FrameRecord& x) { return x.memValid; }, m) && m.frame != st.lastMemFrame)
        {
            st.lastMemFrame = m.frame;
            P::GpuLedger ledger;
            P::ReadGpuLedger(ledger);
            st.vramMB.Push(float(double(m.vramLocalBytes) / kMB));
            st.ledgerMB.Push(float(double(ledger.totalBytes) / kMB));
        }
    }

    // ==== Overview ====
    void DrawOverview(EditorLayer::ProfilerPanelState& st, Renderer* renderer)
    {
        const auto& ft = Diag::FrameTiming::Get();
        ImGui::Text("Frame  %.2f ms  (%.1f FPS)", ft.frameSm, ft.fps);
        // The CPU bracket contains the frame-slot fence wait and Present: the headline subtracts the
        // GPU wait, or a GPU-bound frame reads as CPU-bound. The wait and the full bracket follow it.
        const float cpuWork = std::max(0.f, ft.cpuSm - ft.gpuWaitMs);
        ImGui::Text("CPU    %.2f ms   GPU wait %.2f ms   (bracket %.2f)   Game %.2f   Draw %.2f",
                    cpuWork, ft.gpuWaitMs, ft.cpuSm, ft.gameSm, ft.drawSm);
        ImGui::Text("Period %.2f ms   RenderPhase %.2f ms   Join %s",
                    ft.cpuPeriodSm, ft.renderPhaseSm, ft.joinWaitValid ? "" : "--");
        if (ft.joinWaitValid) { ImGui::SameLine(0.f, 0.f); ImGui::Text("%.2f ms", ft.joinWaitSm); }

        // History: FrameTiming's rings (the same numbers the HUD shows), oldest first, over "frames ago".
        constexpr int kN = Diag::FrameTiming::kHistoryLen;
        const int n = std::clamp(ft.filled, 0, kN);
        float ago[kN], frame[kN], cpu[kN], gpu[kN];
        for (int m = 0; m < n; ++m)
        {
            const int k = (ft.historyOffset + (kN - n) + m) % kN;
            ago[m]   = float(m - (n - 1));
            frame[m] = ft.frameHistory[k];
            cpu[m]   = ft.cpuHistory[k];
            gpu[m]   = ft.gpuHistory[k];
        }
        const float p50 = Percentile(frame, n, 0.50f), p95 = Percentile(frame, n, 0.95f),
                    p99 = Percentile(frame, n, 0.99f);
        ImGui::Text("Frame p50 %.2f   p95 %.2f   p99 %.2f ms   (the last %d frames)", p50, p95, p99, n);
        ImGui::SameLine();
        static const float kBudgets[] = { 1000.f / 30.f, 1000.f / 60.f, 1000.f / 90.f, 1000.f / 120.f, 1000.f / 144.f };
        static const char* const kBudgetNames[] = { "30 fps", "60 fps", "90 fps", "120 fps", "144 fps" };
        int budget = 1;
        for (int i = 0; i < 5; ++i)
            if (std::fabs(st.budgetMs - kBudgets[i]) < 0.01f) budget = i;
        ImGui::SetNextItemWidth(90.f);
        if (ImGui::Combo("budget", &budget, kBudgetNames, 5)) st.budgetMs = kBudgets[budget];
        // The scale follows the typical frame, not the worst one: a single 1 s hitch would flatten every
        // other sample onto the axis. A hitch above the top is pinned there with its value.
        const float yTop = std::max({ 1.5f * st.budgetMs, 2.f * Percentile(frame, n, 0.95f),
                                      2.f * Percentile(cpu, n, 0.95f), 2.f * Percentile(gpu, n, 0.95f) });
        if (ImPlot::BeginPlot("##frame_history", ImVec2(-1, 190), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
        {
            ImPlot::SetupAxes("frames ago", "ms", ImPlotAxisFlags_None, ImPlotAxisFlags_LockMin);
            ImPlot::SetupAxisLimits(ImAxis_X1, -double(kN - 1), 0.0, ImPlotCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, yTop, ImPlotCond_Always);
            // Above the plot: inside, it would cover the hitch labels pinned to the top.
            ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
            ImPlot::PlotLine("Frame", ago, frame, n);
            ImPlot::PlotLine("CPU", ago, cpu, n);
            ImPlot::PlotLine("GPU busy", ago, gpu, n);
            const double budgetLine = st.budgetMs;
            const ImU32  budgetCol  = IM_COL32(235, 160, 70, 255);
            ImPlot::PlotInfLines("Budget", &budgetLine, 1,
                                 ImPlotSpec(ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal, ImPlotProp_LineColor, budgetCol));
            ImPlot::TagY(budgetLine, ImGui::ColorConvertU32ToFloat4(budgetCol), "%.1f", budgetLine);
            ImPlot::TagY(p99, ImVec4(0.85f, 0.35f, 0.35f, 1.f), "p99");
            // A hitch: well above both the typical frame and the budget.
            const float hitch = std::max(2.f * p50, 1.5f * st.budgetMs);
            float hx[kN], hy[kN];
            int   hits = 0;
            for (int m = 0; m < n; ++m)
                if (frame[m] > hitch)
                {
                    hx[hits] = ago[m];
                    hy[hits] = std::min(frame[m], 0.97f * yTop);
                    if (frame[m] > yTop)
                        ImPlot::Annotation(ago[m], hy[hits], ImVec4(0.85f, 0.35f, 0.35f, 1.f), ImVec2(0.f, 10.f), true,
                                           "%.0f ms", frame[m]);
                    ++hits;
                }
            if (hits)
                ImPlot::PlotScatter("Hitch", hx, hy, hits,
                                    ImPlotSpec(ImPlotProp_Marker, ImPlotMarker_Diamond, ImPlotProp_MarkerSize, 5.f));
            ImPlot::EndPlot();
        }

        // GPU headline from the profiler's snapshot: the union of every queue's regions
        // (nesting-proof), not max(Gfx, Compute).
        ImGui::Separator();
        if (st.lastGpuFrame == ~0ull)
            ImGui::TextDisabled("GPU: waiting for timestamps");
        else
        {
            ImGui::Text("GPU busy (union of all queues)  %.2f ms", st.busyEma);
            ImGui::Text("  Graphics %.2f   Compute %.2f   Copy %.2f   overlap %.2f ms",
                        st.queueEma[0], st.queueEma[1], st.queueEma[2], st.overlapEma);
        }

        // Boundness of the last frames that have a verdict (oldest left).
        P::FrameRecord r;
        const uint64_t latest = P::LatestRecordedFrame();
        if (latest != ~0ull)
        {
            constexpr int kStrip = 120;
            ImGui::TextUnformatted("Bound (last frames):");
            ImGui::SameLine();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = std::max(120.f, ImGui::GetContentRegionAvail().x);
            const float cell = w / float(kStrip), h = ImGui::GetTextLineHeight();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            int counts[5]{};
            for (int i = 0; i < kStrip; ++i)
            {
                const uint64_t f = latest >= uint64_t(kStrip - 1 - i) ? latest - uint64_t(kStrip - 1 - i) : ~0ull;
                int b = 0;
                if (f != ~0ull && P::GetFrameRecord(f, r) && r.gpuDerived) b = static_cast<int>(r.bound);
                counts[b < 5 ? b : 0]++;
                dl->AddRectFilled(ImVec2(p0.x + cell * i, p0.y), ImVec2(p0.x + cell * (i + 1) - 1.f, p0.y + h),
                                  kBoundCol[b < 5 ? b : 0]);
            }
            ImGui::Dummy(ImVec2(w, h));
            Swatch(kBoundCol[1], "gpu");   ImGui::Text("%d", counts[1]); ImGui::SameLine(0.f, 14.f);
            Swatch(kBoundCol[2], "cpu");   ImGui::Text("%d", counts[2]); ImGui::SameLine(0.f, 14.f);
            Swatch(kBoundCol[3], "pacing"); ImGui::Text("%d", counts[3]); ImGui::SameLine(0.f, 14.f);
            Swatch(kBoundCol[4], "mixed"); ImGui::Text("%d", counts[4]); ImGui::SameLine(0.f, 14.f);
            Swatch(kBoundCol[0], "no verdict"); ImGui::Text("%d", counts[0]);
        }
        else
            ImGui::TextDisabled("Bound: the profiler has recorded no frame yet");

        if (renderer)
        {
            ImGui::Separator();
            const Renderer::FrameStats& fs = renderer->GetFrameStats();
            ImGui::Text("Draw packets %u   instances %u   lights %u", fs.drawPackets, fs.instancesEmitted, fs.lights);
        }
    }

    // ==== Timeline ====
    void DrawTimeline(EditorLayer::ProfilerPanelState& st)
    {
        P::SetLiveTimeline(true);
        ImGui::Checkbox("Pause", &st.paused);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderInt("Frames", &st.frames, 1, 24);
        ImGui::SameLine();
        if (ImGui::Button("Fit")) st.fit = true;
        ImGui::SameLine();
        ImGui::TextDisabled("wheel = zoom, drag = pan, hover = details");

        const double now = ImGui::GetTime();
        if (!st.paused && now - st.lastCopy > 0.25)
        {
            const bool first = st.tl.frames.empty();
            P::CopyLiveTimeline(static_cast<uint32_t>(st.frames), st.tl);
            st.lastCopy = now;
            if (first) st.fit = true;
        }
        if (st.tl.frames.empty() || st.tl.frameBegin.size() < 2)
        {
            ImGui::TextDisabled(P::GetLevel() < P::Level::Timeline
                ? "The timeline needs profiler level timeline (the command line set a lower one)."
                : "Waiting for the recorder's first frames...");
            return;
        }
        const int64_t t0 = st.tl.frameBegin.front();
        const double spanMs = double(st.tl.frameBegin.back() - t0) * 1e-6;
        if (st.fit || st.view1 <= st.view0) { st.view0 = 0.0; st.view1 = spanMs; st.fit = false; }

        // Lanes: Main, RenderOwner, workers, others; then the GPU queues.
        std::vector<const P::LiveThread*> lanes;
        for (const auto& t : st.tl.threads) lanes.push_back(&t);
        auto rank = [](P::Role r) { return r == P::Role::Main ? 0 : r == P::Role::RenderOwner ? 1 : r == P::Role::Worker ? 2 : 3; };
        std::stable_sort(lanes.begin(), lanes.end(), [&](const P::LiveThread* a, const P::LiveThread* b) {
            return rank(a->role) != rank(b->role) ? rank(a->role) < rank(b->role) : a->index < b->index;
        });
        const float rowH = ImGui::GetTextLineHeight() + 2.f;
        constexpr int kMaxDepthRows = 6;
        auto laneRows = [&](const P::LiveThread& t) {
            int d = 0;
            for (const P::Event& e : t.events) d = std::max<int>(d, int(e.Depth()));
            return std::min(d + 1, kMaxDepthRows);
        };
        int gpuRows[3] = { 0, 0, 0 };
        for (const P::GpuSpan& sp : st.tl.gpu)
            if (sp.queue < 3) gpuRows[sp.queue] = std::max<int>(gpuRows[sp.queue], std::min<int>(sp.depth + 1, kMaxDepthRows));

        ImGui::BeginChild("##timeline", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoMove);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float gutter = 150.f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float areaX = origin.x + gutter;
        const float areaW = std::max(50.f, ImGui::GetContentRegionAvail().x - gutter);
        const double range = std::max(1e-6, st.view1 - st.view0);
        auto xOf = [&](double ms) { return areaX + float((ms - st.view0) / range * areaW); };

        // Zoom (wheel, around the cursor) and pan (drag).
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hoveredChild = ImGui::IsWindowHovered();
        if (hoveredChild && mouse.x >= areaX)
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f)
            {
                const double at = st.view0 + double(mouse.x - areaX) / areaW * range;
                const double k = std::pow(0.8, double(wheel));
                st.view0 = at - (at - st.view0) * k;
                st.view1 = at + (st.view1 - at) * k;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const double d = -double(ImGui::GetIO().MouseDelta.x) / areaW * range;
                st.view0 += d;
                st.view1 += d;
            }
        }

        // Frame boundaries across every lane.
        float y = origin.y + rowH;   // header row
        int totalRows = 0;
        for (const auto* t : lanes) totalRows += laneRows(*t);
        for (int q = 0; q < 3; ++q) totalRows += gpuRows[q];
        const float bottom = y + (totalRows + int(lanes.size()) + 3) * rowH;
        for (size_t f = 0; f + 1 < st.tl.frameBegin.size(); ++f)
        {
            const double ms = double(st.tl.frameBegin[f] - t0) * 1e-6;
            const float x = xOf(ms);
            if (x < areaX || x > areaX + areaW) continue;
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, bottom), IM_COL32(255, 255, 255, 60));
            char lab[32];
            std::snprintf(lab, sizeof(lab), "frame %llu", static_cast<unsigned long long>(st.tl.frames[f]));
            dl->AddText(ImVec2(x + 3.f, origin.y), IM_COL32(220, 220, 220, 200), lab);
        }

        struct Hover { std::string name; const char* cat; double ms; const char* lane; };
        Hover hover{};
        bool hovered = false;
        auto drawBox = [&](double ms0, double ms1, float yy, ImU32 col, const char* name, const char* cat,
                           const char* laneName) {
            if (ms1 < st.view0 || ms0 > st.view1) return;
            float x0 = std::max(xOf(ms0), areaX), x1 = std::min(xOf(ms1), areaX + areaW);
            if (x1 - x0 < 1.f) x1 = x0 + 1.f;
            const ImVec2 a(x0, yy), b(x1, yy + rowH - 1.f);
            dl->AddRectFilled(a, b, col);
            if (x1 - x0 > 36.f && name)
            {
                dl->PushClipRect(a, b, true);
                dl->AddText(ImVec2(x0 + 2.f, yy), IM_COL32(15, 15, 15, 255), name);
                dl->PopClipRect();
            }
            if (!hovered && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y)
            {
                hovered = true;
                hover = { name ? name : "?", cat, ms1 - ms0, laneName };
            }
        };

        for (const auto* t : lanes)
        {
            const int rows = laneRows(*t);
            dl->AddText(ImVec2(origin.x + 4.f, y), IM_COL32(230, 230, 230, 255), t->name[0] ? t->name : "?");
            dl->AddLine(ImVec2(origin.x, y - 1.f), ImVec2(areaX + areaW, y - 1.f), IM_COL32(255, 255, 255, 25));
            for (const P::Event& e : t->events)
            {
                const int d = std::min<int>(int(e.Depth()), kMaxDepthRows - 1);
                const double ms0 = double(e.startNs - t0) * 1e-6, ms1 = double(e.EndNs() - t0) * 1e-6;
                const int ci = static_cast<int>(e.Category());
                if (e.Category() == P::Cat::Marker)
                {
                    const float x = xOf(ms0);
                    if (x >= areaX && x <= areaX + areaW)
                        dl->AddLine(ImVec2(x, y), ImVec2(x, y + rowH * rows), kCatCol[7], 2.f);
                    continue;
                }
                const ImU32 col = (e.Category() == P::Cat::Work) ? NameColor(e.name) : kCatCol[ci < 9 ? ci : 6];
                drawBox(ms0, ms1, y + rowH * d, col, P::NameOf(e.name), kCatLabel[ci < 9 ? ci : 6], t->name);
            }
            y += rowH * rows + rowH * 0.5f;
        }
        for (int q = 0; q < 3; ++q)
        {
            if (!gpuRows[q]) continue;
            char lab[32];
            std::snprintf(lab, sizeof(lab), "GPU %s", kQueueName[q]);
            dl->AddText(ImVec2(origin.x + 4.f, y), IM_COL32(200, 170, 240, 255), lab);
            dl->AddLine(ImVec2(origin.x, y - 1.f), ImVec2(areaX + areaW, y - 1.f), IM_COL32(255, 255, 255, 25));
            for (const P::GpuSpan& sp : st.tl.gpu)
            {
                if (sp.queue != q) continue;
                const int d = std::min<int>(sp.depth, kMaxDepthRows - 1);
                drawBox(double(sp.beginNs - t0) * 1e-6, double(sp.endNs - t0) * 1e-6, y + rowH * d,
                        NameColor(sp.name), P::NameOf(sp.name), "gpu", kQueueName[q]);
            }
            y += rowH * gpuRows[q] + rowH * 0.5f;
        }
        ImGui::Dummy(ImVec2(gutter + areaW, y - origin.y));
        if (hovered)
            ImGui::SetTooltip("%s\n%s   %.3f ms\n%s", hover.name.c_str(), hover.cat, hover.ms, hover.lane);
        ImGui::EndChild();
    }

    // ==== Threads ====
    void DrawThreads()
    {
        P::FrameRecord r;
        if (P::GetLevel() == P::Level::Off || !NewestRecord(0, [](const P::FrameRecord&) { return true; }, r))
        {
            ImGui::TextDisabled("No frame record (profiler level off).");
            return;
        }
        // Average the last 30 records per thread index, so the bars read.
        constexpr int kAvg = 30;
        struct Acc { double cat[8]{}; double untracked = 0, os = 0; int n = 0, osN = 0; uint32_t allocs = 0; P::Role role = P::Role::Other; };
        std::unordered_map<uint16_t, Acc> acc;
        double period = 0.0, util = 0.0, w = 0.0, sMain = 0.0, sLane = 0.0, allIdle = 0.0;
        int nRec = 0, nIdle = 0;
        double conc[P::kFrameThreads + 1]{};
        const uint64_t latest = P::LatestRecordedFrame();
        for (int k = 0; k < kAvg && uint64_t(k) <= latest; ++k)
        {
            P::FrameRecord x;
            if (!P::GetFrameRecord(latest - uint64_t(k), x)) continue;
            ++nRec;
            period += x.periodMs; util += x.util; w += x.wTotalMs; sMain += x.sMainMs; sLane += x.sLaneMs;
            if (x.timelineValid)
            {
                ++nIdle;
                allIdle += x.allIdleMs;
                for (int c = 0; c <= P::kFrameThreads; ++c) conc[c] += x.concurrencyMs[c];
            }
            for (uint32_t i = 0; i < x.threadCount; ++i)
            {
                const P::ThreadFrame& tf = x.threads[i];
                Acc& a = acc[tf.index];
                for (int c = 0; c < 7; ++c) a.cat[c] += tf.catMs[c];
                a.untracked += tf.untrackedMs;
                if (tf.osCpuMs >= 0.f) { a.os += tf.osCpuMs; ++a.osN; }
                a.allocs += tf.allocs;
                a.role = tf.role;
                ++a.n;
            }
        }
        if (!nRec) return;
        period /= nRec;
        ImGui::Text("Average of the last %d frames: period %.2f ms   pool util %.0f%%   W %.1f ms-core   main %.2f   lane %.2f",
                    nRec, period, 100.0 * util / nRec, w / nRec, sMain / nRec, sLane / nRec);
        if (nIdle)
        {
            ImGui::Text("All-idle (main working, no task worker working): %.2f ms/frame (%.0f%%)",
                        allIdle / nIdle, period > 0 ? 100.0 * allIdle / nIdle / period : 0.0);
            float pct[P::kFrameThreads + 1];
            int top = 0;
            for (int c = 0; c <= P::kFrameThreads; ++c)
            {
                pct[c] = period > 0 ? float(100.0 * conc[c] / nIdle / period) : 0.f;
                if (pct[c] >= 0.5f) top = c;
            }
            if (ImPlot::BeginPlot("##concurrency", ImVec2(-1, 130),
                                  ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend))
            {
                ImPlot::SetupAxes("threads in Work at once", "% of the frame", ImPlotAxisFlags_AutoFit,
                                  ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_LockMin);
                ImPlot::SetupAxisTicks(ImAxis_X1, 0.0, double(top), top + 1);   // whole thread counts
                ImPlot::PlotBars("##conc", pct, top + 1, 0.67, 0.0);
                ImPlot::EndPlot();
            }
        }
        else
            ImGui::TextDisabled("All-idle / concurrency: need level timeline.");

        for (int c : { 0, 1, 3, 4, 5, 6, 2, 8 }) Swatch(kCatCol[c], kCatLabel[c]);
        ImGui::NewLine();

        std::vector<std::pair<uint16_t, Acc>> rows(acc.begin(), acc.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.cat[0] / std::max(1, a.second.n) > b.second.cat[0] / std::max(1, b.second.n);
        });
        if (ImGui::BeginTable("##threads", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 150.f);
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Work ms", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Wait ms", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Idle ms", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("OS CPU", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Allocs", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableHeadersRow();
            for (const auto& [idx, a] : rows)
            {
                const double n = std::max(1, a.n);
                P::ThreadCounters tc;
                const char* name = P::ReadThreadCounters(idx, tc) ? tc.name : "?";
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float bw = ImGui::GetContentRegionAvail().x, bh = ImGui::GetTextLineHeight();
                float x = p.x;
                const double tot = period > 0 ? period : 1.0;
                for (int c : { 0, 1, 3, 4, 5, 6, 2, 8 })
                {
                    const double ms = (c == 8 ? a.untracked : a.cat[c]) / n;
                    const float wpx = float(ms / tot) * bw;
                    if (wpx > 0.2f)
                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, p.y), ImVec2(x + wpx, p.y + bh), kCatCol[c]);
                    x += wpx;
                }
                ImGui::Dummy(ImVec2(bw, bh));
                ImGui::TableNextColumn(); ImGui::Text("%.2f", a.cat[0] / n);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", (a.cat[3] + a.cat[4] + a.cat[5] + a.cat[6]) / n);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", a.cat[2] / n);
                ImGui::TableNextColumn();
                if (a.osN) ImGui::Text("%.2f", a.os / a.osN); else ImGui::TextDisabled("--");
                ImGui::TableNextColumn(); ImGui::Text("%.0f", a.allocs / n);
            }
            ImGui::EndTable();
        }
    }

    // ==== Passes ====
    void DrawPasses(EditorLayer::ProfilerPanelState& st)
    {
        if (st.lastGpuFrame == ~0ull) { ImGui::TextDisabled("Waiting for GPU timestamps..."); return; }
        ImGui::Text("GPU busy %.2f ms (union)   Graphics %.2f   Compute %.2f   Copy %.2f   overlap %.2f",
                    st.busyEma, st.queueEma[0], st.queueEma[1], st.queueEma[2], st.overlapEma);
        ImGui::Checkbox("Show idle passes", &st.showIdle);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A pass that reports a region but did no work (a disabled pass still opens\n"
                              "a ~0 ms region every frame) is hidden unless this is on.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.f);
        ImGui::DragFloat("idle below (ms)", &st.activeMs, 0.001f, 0.f, 1.f, "%.3f");
        ImGui::SameLine();
        st.passFilter.Draw("filter", 160.f);

        struct Row { uint16_t name; const EditorLayer::ProfilerPanelState::PassRow* r; bool active; };
        std::vector<Row> rows;
        int hidden = 0;
        for (const auto& [name, r] : st.passes)
        {
            const bool active = r.lastActive != 0 && st.gpuTick - r.lastActive <= 60;
            if (!active && !st.showIdle) { ++hidden; continue; }
            if (!st.passFilter.PassFilter(P::NameOf(name))) continue;
            rows.push_back({ name, &r, active });
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d idle hidden)", hidden);

        // The ticked passes: GPU ms, one sample per GPU frame.
        if (!st.passHistory.empty()
            && ImPlot::BeginPlot("##pass_history", ImVec2(-1, 170), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
        {
            ImPlot::SetupAxes("GPU frames ago", "ms", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_LockMin);
            ImPlot::SetupAxisLimits(ImAxis_X1, -double(EditorLayer::ProfilerPanelState::kPlotLen - 1), 0.0,
                                    ImPlotCond_Once);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest);
            for (const auto& [name, ring] : st.passHistory) PlotRing(P::NameOf(name), ring);
            ImPlot::EndPlot();
        }

        if (ImGui::BeginTable("##passes", 7, ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                             ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                              ImGuiTableColumnFlags_PreferSortDescending, 70.f);
            ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("% busy", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 36.f);
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
            {
                if (ss->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& s = ss->Specs[0];
                    const bool asc = s.SortDirection == ImGuiSortDirection_Ascending;
                    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
                        int c = 0;
                        switch (s.ColumnIndex)
                        {
                        case 0: c = std::strcmp(P::NameOf(a.name), P::NameOf(b.name)); break;
                        case 1: c = int(a.r->queue) - int(b.r->queue); break;
                        case 3: c = (a.r->last < b.r->last) ? -1 : (a.r->last > b.r->last); break;
                        case 4: c = (a.r->peak < b.r->peak) ? -1 : (a.r->peak > b.r->peak); break;
                        default: c = (a.r->ema < b.r->ema) ? -1 : (a.r->ema > b.r->ema); break;
                        }
                        return asc ? c < 0 : c > 0;
                    });
                }
            }
            for (const Row& row : rows)
            {
                const auto& r = *row.r;
                ImGui::TableNextRow();
                if (!row.active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.f));
                ImGui::TableNextColumn();
                ImGui::Text("%*s%s", int(r.depth) * 2, "", P::NameOf(row.name));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.queue < 3 ? kQueueName[r.queue] : "?");
                ImGui::TableNextColumn(); ImGui::Text("%.3f", r.ema);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", r.last);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", r.peak);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", st.busyEma > 0 ? 100.f * r.ema / st.busyEma : 0.f);
                ImGui::TableNextColumn();
                bool plotted = st.passHistory.count(row.name) != 0;
                ImGui::PushID(row.name);
                if (ImGui::Checkbox("##plot", &plotted))
                {
                    if (plotted) st.passHistory[row.name];
                    else         st.passHistory.erase(row.name);
                }
                ImGui::PopID();
                if (!row.active) ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
    }

    // ==== Memory ====
    void DrawMemory(EditorLayer::ProfilerPanelState& st, bool* showPoolWatermarks)
    {
        constexpr double kMB = 1048576.0;
        P::FrameRecord m;
        if (NewestRecord(8, [](const P::FrameRecord& x) { return x.memValid; }, m))
        {
            const double vram = double(m.vramLocalBytes) / kMB, budget = double(m.vramLocalBudget) / kMB;
            char lab[64];
            std::snprintf(lab, sizeof(lab), "VRAM %.0f / %.0f MB", vram, budget);
            ImGui::ProgressBar(budget > 0 ? float(vram / budget) : 0.f, ImVec2(-1, 0), lab);
            if (st.vramMB.count > 1
                && ImPlot::BeginPlot("##vram_history", ImVec2(-1, 160), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
            {
                ImPlot::SetupAxes("samples ago", "MB", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_LockMin);
                ImPlot::SetupAxisLimits(ImAxis_X1, -double(EditorLayer::ProfilerPanelState::kPlotLen - 1), 0.0,
                                        ImPlotCond_Once);
                ImPlot::SetupLegend(ImPlotLocation_SouthWest, ImPlotLegendFlags_Horizontal);
                PlotRing("VRAM (DXGI local)", st.vramMB);
                PlotRing("Ledger", st.ledgerMB);
                ImPlot::EndPlot();
            }
            ImGui::Text("Non-local %.0f MB   working set %.0f MB   private %.0f MB   page faults %u /frame",
                        double(m.vramNonLocalBytes) / kMB, double(m.workingSetBytes) / kMB,
                        double(m.privateBytes) / kMB, m.pageFaults);
        }
        else
            ImGui::TextDisabled("VRAM / RAM: waiting for the collector's sample.");

        P::GpuLedger L;
        P::ReadGpuLedger(L);
        ImGui::SeparatorText("GPU resource ledger");
        ImGui::Text("%.0f MB total   local %.0f   system %.0f   placed resources %lld",
                    double(L.totalBytes) / kMB, double(L.localBytes) / kMB, double(L.nonLocalBytes) / kMB,
                    static_cast<long long>(L.placed));
        if (m.memValid)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("   unattributed local %.0f MB",
                                double(m.vramLocalBytes) / kMB - double(L.localBytes) / kMB);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("DXGI local usage minus the ledger's local categories: memory the engine did\n"
                                  "not create itself (DLSS / Frame Generation, FSR, the driver, ImGui, Tracy).\n"
                                  "Large is expected; growing is not.");
        }
        int order[P::kGpuMemCats];
        for (int i = 0; i < P::kGpuMemCats; ++i) order[i] = i;
        std::sort(order, order + P::kGpuMemCats, [&](int a, int b) { return L.bytes[a] > L.bytes[b]; });
        if (ImGui::BeginTable("##ledger", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("MB", ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableHeadersRow();
            for (int k = 0; k < P::kGpuMemCats; ++k)
            {
                const int i = order[k];
                if (!L.bytes[i] && !L.count[i]) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(P::GpuMemCatName(static_cast<P::GpuMemCat>(i)));
                ImGui::TableNextColumn(); ImGui::Text("%.1f", double(L.bytes[i]) / kMB);
                ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(L.count[i]));
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Gauges and heap traffic");
        double v = 0.0, cap = 0.0;
        if (P::ReadGauge(P::Gauge::DescriptorsLive, v) && P::ReadGauge(P::Gauge::DescriptorsCapacity, cap))
            ImGui::Text("Shader-visible descriptors: %.0f live of %.0f", v, cap);
        if (P::ReadGauge(P::Gauge::LuaKB, v)) ImGui::Text("Lua heap: %.1f MB", v / 1024.0);
        if (P::ReadGauge(P::Gauge::JoltStepPeakKB, v)) ImGui::Text("Jolt step arena peak: %.0f KB", v);
        P::FrameRecord r;
        if (NewestRecord(0, [](const P::FrameRecord&) { return true; }, r))
            ImGui::Text("operator new: %u /frame (%.2f MB), delete %u /frame, staging uploads %.2f MB",
                        r.allocs, double(r.allocBytes) / kMB, r.frees, double(r.uploadBytes) / kMB);
        if (ImGui::Button("Pool watermarks...")) *showPoolWatermarks = true;
    }

    // ==== Captures ====
    void DrawCaptures(EditorLayer::ProfilerPanelState& st)
    {
        P::CollectorStatus cs;
        const bool running = P::GetCollectorStatus(cs);
        const std::string out = P::CollectorOutputDir();
        ImGui::Text("Collector %s   level %s   frames processed %llu   windows %u / %u   hitches %u",
                    running ? "running" : "not running", P::LevelName(P::GetLevel()),
                    static_cast<unsigned long long>(cs.processed), cs.windowsFinished, cs.windowsRequested,
                    cs.hitches);
        ImGui::Text("Files: %s", out.empty() ? "(none - windows are summarised but not written)" : out.c_str());
        if (!out.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open folder"))
                ::ShellExecuteW(nullptr, L"explore", Sys::Utf8ToWide(out).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::TextDisabled("A window's .trace.json opens in https://ui.perfetto.dev (CPU threads and GPU queues on one timeline).");

        if (!running) ImGui::BeginDisabled();
        ImGui::SeparatorText("Capture");
        ImGui::SetNextItemWidth(160.f); ImGui::InputText("##capname", st.captureName, sizeof(st.captureName));
        ImGui::SameLine(); ImGui::SetNextItemWidth(90.f); ImGui::InputInt("frames", &st.captureFrames);
        st.captureFrames = std::clamp(st.captureFrames, 1, 100000);
        ImGui::SameLine();
        if (ImGui::Button("Capture next frames"))
            st.lastAction = P::RequestCapture(st.captureName, uint32_t(st.captureFrames))
                          ? "capture requested" : "refused: no collector";
        ImGui::SetNextItemWidth(160.f); ImGui::InputText("##winname", st.windowName, sizeof(st.windowName));
        ImGui::SameLine();
        if (!st.windowOpen ? ImGui::Button("Begin window") : ImGui::Button("End window"))
        {
            const bool ok = st.windowOpen ? P::EndNamedWindow(st.windowName) : P::BeginNamedWindow(st.windowName);
            if (ok) st.windowOpen = !st.windowOpen;
            st.lastAction = ok ? (st.windowOpen ? "window begun" : "window ended") : "refused: no collector";
        }
        ImGui::SeparatorText("Hitches");
        ImGui::SetNextItemWidth(90.f); ImGui::InputFloat("ms threshold", &st.hitchMs, 1.f, 5.f, "%.1f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(90.f); ImGui::InputInt("max", &st.hitchMax);
        st.hitchMax = std::clamp(st.hitchMax, 1, 64);
        ImGui::SameLine();
        if (ImGui::Button("Arm")) st.lastAction = P::SetHitchCapture(st.hitchMs, uint32_t(st.hitchMax)) ? "hitch capture armed" : "refused";
        ImGui::SameLine();
        if (ImGui::Button("Disarm")) st.lastAction = P::SetHitchCapture(0.f, 1) ? "hitch capture off" : "refused";
        if (!running) ImGui::EndDisabled();
        if (!st.lastAction.empty()) ImGui::TextDisabled("%s", st.lastAction.c_str());

        ImGui::SeparatorText("Written this session");
        const auto caps = P::ListCaptures();
        if (caps.empty()) ImGui::TextDisabled("none yet");
        else if (ImGui::BeginTable("##caps", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("Trace");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50.f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < caps.size(); ++i)
            {
                const auto& c = caps[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.kind.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu +%u%s", static_cast<unsigned long long>(c.first), c.recorded,
                                                      c.complete ? "" : " (cut)");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.trace.empty() ? "--" : c.trace.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(int(i));
                if (!c.trace.empty() && ImGui::SmallButton("Copy")) ImGui::SetClipboardText(c.trace.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

// ==== EditorLayer: the window and its profiler hold ====
void EditorLayer::SyncProfilerHold()
{
    const bool want = m_showProfiler;
    if (!m_prof)
    {
        if (!want) return;
        m_prof = { new ProfilerPanelState(), [](ProfilerPanelState* p) { delete p; } };
    }
    ProfilerPanelState& st = *m_prof;
    if (want == st.held) return;
    if (want)
    {
        if (!P::CollectorRunning())
        {
            P::CollectorConfig cfg;
            cfg.level   = P::GetLevel() > P::Level::Timeline ? P::GetLevel() : P::Level::Timeline;
            cfg.outDir  = Sys::LogDir() + "/profile/" + Stamp();
            cfg.streams = false;   // capture windows only: no per-frame records.jsonl in an editor session
            P::StartCollector(cfg);
            st.ownsCollector = P::CollectorRunning();
        }
        else if (st.ownsCollector && P::GetLevel() < P::Level::Timeline)
        {
            P::SetLevel(P::Level::Timeline);
        }
        if (m_gfx) { m_gfx->AddGPUProfilerWant(); st.gpuWant = true; }
        st.held = true;
    }
    else
    {
        P::SetLiveTimeline(false);
        if (st.ownsCollector) P::SetLevel(P::Level::Off);
        if (st.gpuWant && m_gfx) { m_gfx->ReleaseGPUProfilerWant(); st.gpuWant = false; }
        st.held = false;
    }
}

void EditorLayer::RenderProfilerPanel()
{
    SyncProfilerHold();
    if (!m_prof) return;
    ProfilerPanelState& st = *m_prof;

    ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profiler", &m_showProfiler))
    {
        ImGui::End();
        return;
    }
    SampleModel(st);
    bool timelineShown = false;
    auto tabFlags = [&](const char* harnessName) {
        return HarnessMenuHeldOpen(harnessName) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabBar("##profiler_tabs"))
    {
        if (ImGui::BeginTabItem("Overview", nullptr, tabFlags("Profiler/Overview")))
        {
            DrawOverview(st, m_renderer);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Timeline", nullptr, tabFlags("Profiler/Timeline")))
        {
            timelineShown = true;
            DrawTimeline(st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Threads", nullptr, tabFlags("Profiler/Threads")))
        {
            DrawThreads();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Passes", nullptr, tabFlags("Profiler/Passes")))
        {
            DrawPasses(st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory", nullptr, tabFlags("Profiler/Memory")))
        {
            DrawMemory(st, &m_showPoolWatermarks);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Captures", nullptr, tabFlags("Profiler/Captures")))
        {
            DrawCaptures(st);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    // The recorder copies each frame's events only while the Timeline tab is
    // actually on screen (or -profile_hitch needs them).
    if (!timelineShown) P::SetLiveTimeline(false);
    // Closed with the window's own X: release in the same frame.
    if (!m_showProfiler) SyncProfilerHold();
}
