#include "UI/DebugHud.h"

#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Diagnostics/ProfilerGpu.h"
#include "Diagnostics/ProfilerMemory.h"

#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace UI
{
    namespace
    {
        // ---- Formatting ----
        // All write into a caller-owned buffer: the HUD formats into fixed member arrays, so the
        // per-frame path never allocates.

        void FmtMs(char* out, size_t n, float ms)
        {
            std::snprintf(out, n, "%.2f ms", ms);
        }

        // Triangle counts are abbreviated: a scene total is 7-9 digits and the exact value is
        // never the question.
        void FmtTris(char* out, size_t n, uint64_t v)
        {
            if      (v >= 1000000ull) std::snprintf(out, n, "%.2f M", double(v) / 1000000.0);
            else if (v >= 1000ull)    std::snprintf(out, n, "%.1f K", double(v) / 1000.0);
            else                      std::snprintf(out, n, "%llu", (unsigned long long)v);
        }

        // Draw / instance counts stay exact: they are small, and 736 vs 740 is a real signal.
        void FmtCount(char* out, size_t n, uint64_t v)
        {
            char raw[32];
            std::snprintf(raw, sizeof(raw), "%llu", (unsigned long long)v);
            const int len = int(std::strlen(raw));
            int outPos = 0;
            for (int i = 0; i < len; ++i)
            {
                if (i > 0 && ((len - i) % 3) == 0 && outPos + 1 < int(n)) out[outPos++] = ',';
                if (outPos + 1 < int(n)) out[outPos++] = raw[i];
            }
            out[std::min<int>(outPos, int(n) - 1)] = '\0';
        }

        constexpr Color32 kTextCol   { 232, 232, 236, 255 };
        constexpr Color32 kDimCol    { 150, 150, 160, 255 };
        constexpr Color32 kGoodCol   { 130, 226, 150, 255 };
        constexpr Color32 kWarnCol   { 240, 208, 120, 255 };
        constexpr Color32 kBadCol    { 240, 130, 130, 255 };
        // Alpha is deliberately high: the composition buffer is extended gamma (1.0 = paper
        // white, headroom above), so a half-transparent panel over a bright sky composites
        // brighter than the paper-white text on it.
        constexpr Color32 kPanelCol  { 10, 10, 12, 224 };

        const char* const kAbsent = "--";
    }

    namespace
    {
#ifdef WITH_HARNESS
        constexpr int kLevels = DebugHud::kLevelCount;
#else
        constexpr int kLevels = DebugHud::kShippedLevelCount;
#endif
    }

    void DebugHud::SetLevel(int n)
    {
        m_level = static_cast<Level>(std::clamp(n, 0, kLevels - 1));
    }

    void DebugHud::CycleLevel(int dir)
    {
        const int next = (int(m_level) % kLevels + (dir >= 0 ? 1 : kLevels - 1)) % kLevels;   // +1 / -1 wrap
        m_level = static_cast<Level>(next);
    }

    void DebugHud::Row(const char* label, const char* value, Color32 col)
    {
        if (m_rowCount >= kMaxRows) return;
        std::snprintf(m_label[m_rowCount], kRowChars, "%s", label ? label : "");
        std::snprintf(m_value[m_rowCount], kRowChars, "%s", value ? value : "");
        m_rowCol[m_rowCount] = col;
        ++m_rowCount;
    }

    float DebugHud::ResolveSizePx(uint32_t canvasH) const
    {
        // kTextRefPx is raw device pixels and the process is per-monitor DPI aware, so a fixed
        // 24 px is half-height at 4K. The engine has no global UI scale, so the HUD derives one
        // from the canvas height.
        const float h = (canvasH > 0) ? float(canvasH) : 1080.f;
        const float dpi = std::clamp(h / 1080.f, 0.75f, 3.0f);
        return std::max(10.f, std::round(kTextRefPx * m_userScale * dpi));
    }

    void DebugHud::Update(const DebugHudStats& s)
    {
        // A golden-image run must never composite live FPS digits into the PNG:
        // Renderer::CaptureViewportToPNG reads the texture UIPass drew the HUD into, so every
        // -artifacts screenshot would carry it. Latched here rather than tested in Emit, so
        // WantsGPUProfiler() sees it too and a capture run pays for no timestamps.
        m_captureSuppressed =
            Sys::HasArtifactDir() && !Sys::CommandLine::Get().Has("-stat_in_capture");

        m_rowCount = 0;
        if (m_level == Level::Off || m_suppressed || m_captureSuppressed) return;

        char v[kRowChars];

        // ---- Frame / FPS ----
        // FPS comes from the true wall period, never the CPU bracket: the pacing sleep and the
        // message pump sit outside the bracket, so 1000/cpuMs reports 300 fps on a 60 fps
        // VSync'd frame.
        const Color32 frameCol = (s.fps >= 58.f) ? kGoodCol
                               : (s.fps >= 28.f) ? kWarnCol
                                                 : kBadCol;
        std::snprintf(v, sizeof(v), "%.2f ms   %.1f fps", s.frameMs, s.fps);
        Row("Frame", v, frameCol);

        // stat threads / gpu / mem: pages of their own, not supersets of Full.
        if (m_level >= Level::Threads)
        {
            UpdateProfilerPage();
            return;
        }

        if (m_level >= Level::Unit)
        {
            FmtMs(v, sizeof(v), s.cpuMs);   Row("  CPU",  v, kTextCol);
            FmtMs(v, sizeof(v), s.waitMs);  Row("  Wait", v, kDimCol);
            FmtMs(v, sizeof(v), s.gameMs);  Row("  Game", v, kTextCol);
            FmtMs(v, sizeof(v), s.drawMs);  Row("  Draw", v, kTextCol);
            // The full BeginFrame..EndFrame span (Draw plus the slot wait, asset tick, font flush,
            // composition and present); Join below is the hand-over wait.
            FmtMs(v, sizeof(v), s.renderPhaseMs); Row("  RenderPhase", v, kDimCol);
            // Beside RenderPhase, which spans suspensions (and the pacing sleep): RenderOwner is time
            // the owner thread spent executing this frame's render continuations.
            if (s.renderOwnerValid)
            {
                FmtMs(v, sizeof(v), s.renderOwnerMs);
                Row("  RenderOwner", v, kTextCol);
            }
            else
            {
                Row("  RenderOwner", kAbsent, kDimCol);
            }
            // The barrier that closes the game/render overlap window, and how
            // many times a park hook collapsed it outright.
            std::snprintf(v, sizeof(v), "%.2f ms   (park %u)", s.tailDrainMs, s.parkHookCount);
            Row("  TailDrain", v, kDimCol);
            if (s.joinWaitValid)
            {
                FmtMs(v, sizeof(v), s.joinWaitMs);
                Row("  Join", v, kDimCol);
            }
            else
            {
                Row("  Join", kAbsent, kDimCol);
            }

            if (s.gpuValid)
            {
                std::snprintf(v, sizeof(v), "%.2f ms%s   (age %u)",
                              s.gpuBusyMs, s.gpuCalibrated ? "" : "*", s.gpuAgeFrames);
                Row("GPU busy", v, kTextCol);
            }
            else
            {
                // Absent, not zero. "0.00 ms" here would read as a free GPU.
                Row("GPU busy", kAbsent, kDimCol);
            }
        }

        if (m_level >= Level::Full)
        {
            Row("", "", kDimCol);
            // "Mesh tris", not "Triangles": terrain and grass go through DispatchMesh with
            // amplification culling and have no CPU triangle count, and particles/UI are absent too,
            // so on a landscape this under-reports by an unbounded factor.
            Row("Mesh tris", "", kDimCol);

            if (s.triSceneValid) { FmtTris(v, sizeof(v), s.triScene); Row("  Scene", v, kTextCol); }
            else                 { Row("  Scene", kAbsent, kDimCol); }

            FmtTris(v, sizeof(v), s.triMainView);
            // Under emit-once (cpuCullBypassed) static packets keep their natural filter regardless
            // of the frustum, so this is what the CPU submitted, not what is visible; "Visible" would
            // look exactly like a broken cull.
            Row(s.cpuCullBypassed ? "  CPU-submitted" : "  CPU visible", v, kTextCol);

            if (s.gpuVisValid) { FmtTris(v, sizeof(v), s.gpuVisTris); Row("  GPU visible", v, kTextCol); }
            else               { Row("  GPU visible", kAbsent, kDimCol); }

            // Meshlets: clusters the amplification shader kept / looked at, and
            // their triangles - absent (not 0) while GBuffer.vs draws everything.
            if (s.meshletPath != 0 && s.meshletValid)
            {
                char a[24], b[24], t[24];
                FmtCount(a, sizeof(a), s.meshletVisible);
                FmtCount(b, sizeof(b), s.meshletTested);
                FmtTris(t, sizeof(t), s.meshletTris);
                std::snprintf(v, sizeof(v), "%s / %s, %s", a, b, t);
                Row("  Meshlets", v, kTextCol);
            }
            else
                Row("  Meshlets", s.meshletPath != 0 ? kAbsent : "VS path", kDimCol);

            FmtTris(v, sizeof(v), s.triOutline);     Row("  Outline",     v, kDimCol);
            FmtTris(v, sizeof(v), s.triShadowOnly);  Row("  Shadow-only", v, kDimCol);
            FmtTris(v, sizeof(v), s.triTransparent); Row("  Transparent", v, kDimCol);

            if (s.gpuVisCmdsValid)
            {
                char a[24], b[24];
                FmtCount(a, sizeof(a), s.drawPackets);
                FmtCount(b, sizeof(b), s.gpuVisCmds);
                std::snprintf(v, sizeof(v), "%s -> %s", a, b);
            }
            else
            {
                FmtCount(v, sizeof(v), s.drawPackets);
            }
            Row("Draws", v, kTextCol);

            FmtCount(v, sizeof(v), s.instances);
            Row("Instances", v, kTextCol);

            // Culled% is suppressed when the CPU cull was bypassed (it reads ~0, like a broken culler)
            // and when there is no valid pre-cull total.
            // The two sides must be like for like: triScene omits DrawFilter::Custom (outline shells
            // reuse their mesh's index count and would double the total), but triMainView includes
            // them because they are submitted, and shells can be a third of it. Subtract the shells
            // here.
            if (s.triSceneValid && !s.cpuCullBypassed && s.triScene > 0)
            {
                const uint64_t mainNoShells =
                    (s.triMainView > s.triOutline) ? (s.triMainView - s.triOutline) : 0;
                const double kept = double(std::min(mainNoShells, s.triScene));
                const double pct  = (1.0 - kept / double(s.triScene)) * 100.0;
                std::snprintf(v, sizeof(v), "%.1f %%", std::clamp(pct, 0.0, 100.0));
                Row("Culled", v, kDimCol);
            }
        }
    }

    // ==== Profiler pages ====
    // Everything comes from the engine profiler's frame records (thread-safe copies); nothing
    // here samples the OS or the GPU. Late halves - the GPU numbers (~3 frames), the GPU
    // verdict and the memory sample (the collector, 1-8 frames) - are read from the newest
    // record that has them, and are absent ("--") when none in reach does, never 0.
    void DebugHud::UpdateProfilerPage()
    {
        namespace P = Diag::Prof;
        char v[kRowChars];
        if (P::GetLevel() == P::Level::Off)
        {
            Row("Profiler", "off (-profile=stats)", kDimCol);
            return;
        }
        P::FrameRecord r;
        const uint64_t latest = P::LatestRecordedFrame();
        if (latest == ~0ull || !P::GetFrameRecord(latest, r))
        {
            Row("Profiler", "waiting for a frame", kDimCol);
            return;
        }
        // The newest record (within `back` frames) whose `ok` half has arrived.
        auto newestWith = [&](int back, auto ok, P::FrameRecord& out) -> bool {
            for (int k = 0; k <= back && uint64_t(k) <= latest; ++k)
                if (P::GetFrameRecord(latest - uint64_t(k), out) && ok(out)) return true;
            return false;
        };
        auto pct = [](float part, float whole) { return whole > 0.f ? 100.f * part / whole : 0.f; };

        if (m_level == Level::Threads)
        {
            std::snprintf(v, sizeof(v), "%.0f %%   W %.1f ms   P %u", 100.f * r.util, r.wTotalMs,
                          unsigned(r.poolThreads));
            Row("Pool util", v, kTextCol);
            std::snprintf(v, sizeof(v), "%.2f   join %.2f   gpu %.2f", r.sMainMs, r.mainWaitJoinMs,
                          r.mainWaitGpuMs);
            Row("Main ms", v, kTextCol);
            FmtMs(v, sizeof(v), r.sLaneMs);                         Row("Lane work", v, kTextCol);
            std::snprintf(v, sizeof(v), "%.1f ms (%u thr)", r.workerWorkMs, unsigned(r.workerThreads));
            Row("Worker work", v, kTextCol);
            P::FrameRecord t;
            if (newestWith(4, [](const P::FrameRecord& x) { return x.timelineValid; }, t))
            {
                std::snprintf(v, sizeof(v), "%.2f ms (%.0f %%)", t.allIdleMs, pct(t.allIdleMs, t.periodMs));
                Row("All-idle", v, (t.allIdleMs > 0.25f * t.periodMs) ? kWarnCol : kTextCol);
            }
            else Row("All-idle", kAbsent, kDimCol);   // needs -profile=timeline
            if (r.osPoolCpuMs >= 0.f) { std::snprintf(v, sizeof(v), "%.1f ms", r.osPoolCpuMs); Row("OS pool CPU", v, kDimCol); }
            else                        Row("OS pool CPU", kAbsent, kDimCol);
            Row("", "", kDimCol);
            // Every recorded thread by Work, as a share of the frame.
            struct TRow { float work, wait; uint16_t index; };
            TRow rows[P::kFrameThreads];
            int n = 0;
            for (uint32_t i = 0; i < r.threadCount && n < P::kFrameThreads; ++i)
            {
                const P::ThreadFrame& tf = r.threads[i];
                const float wait = tf.catMs[static_cast<int>(P::Cat::WaitJoin)] +
                                   tf.catMs[static_cast<int>(P::Cat::WaitGpu)] +
                                   tf.catMs[static_cast<int>(P::Cat::WaitPacing)] +
                                   tf.catMs[static_cast<int>(P::Cat::WaitOther)];
                rows[n++] = { tf.catMs[static_cast<int>(P::Cat::Work)], wait, tf.index };
            }
            std::sort(rows, rows + n, [](const TRow& a, const TRow& b) { return a.work > b.work; });
            for (int i = 0; i < n && m_rowCount < kMaxRows; ++i)
            {
                P::ThreadCounters tc;
                const char* name = P::ReadThreadCounters(rows[i].index, tc) ? tc.name : "?";
                std::snprintf(v, sizeof(v), "%3.0f %%  %.2f ms  wait %.2f", pct(rows[i].work, r.periodMs),
                              rows[i].work, rows[i].wait);
                Row(name, v, rows[i].work > 0.8f * r.periodMs ? kWarnCol : kDimCol);
            }
            return;
        }

        if (m_level == Level::Gpu)
        {
            P::FrameRecord g;
            if (!newestWith(8, [](const P::FrameRecord& x) { return x.gpuValid; }, g))
            {
                Row("GPU", "-- (no timestamps yet)", kDimCol);
                return;
            }
            std::snprintf(v, sizeof(v), "%.2f ms%s   (frame %llu)", g.gpuBusyMs, g.gpuCalibrated ? "" : "*",
                          static_cast<unsigned long long>(g.frame));
            Row("GPU busy", v, kTextCol);
            std::snprintf(v, sizeof(v), "gfx %.2f  comp %.2f  copy %.2f", g.gpuQueueMs[0], g.gpuQueueMs[1],
                          g.gpuQueueMs[2]);
            Row("Queues", v, kDimCol);
            FmtMs(v, sizeof(v), g.gpuOverlapMs); Row("Overlap", v, kDimCol);
            P::FrameRecord d;
            if (newestWith(12, [](const P::FrameRecord& x) { return x.gpuDerived; }, d))
            {
                std::snprintf(v, sizeof(v), "%.0f %%   period %.2f ms", 100.f * d.gpuUtil, d.gpuPeriodMs);
                Row("GPU util", v, kTextCol);
                std::snprintf(v, sizeof(v), "%.2f / %.2f ms", d.gpuStarveMs, d.gpuBubbleMs);
                Row("Starve / bubbles", v, kDimCol);
                FmtMs(v, sizeof(v), d.gpuSubmitToStartMs); Row("Submit->start", v, kDimCol);
            }
            else
            {
                Row("GPU util", kAbsent, kDimCol);   // the verdict half comes from the collector
            }
            // Verdict shares over the last frames that have one.
            if (m_boundNext == ~0ull || latest > m_boundNext + 200)
                m_boundNext = latest > 16 ? latest - 16 : 0;
            for (int step = 0; step < 16 && m_boundNext <= latest; ++step)
            {
                P::FrameRecord b;
                if (!P::GetFrameRecord(m_boundNext, b)) { ++m_boundNext; continue; }
                if (!b.gpuDerived) break;          // not judged yet; retry next frame
                m_bound[m_boundHead] = static_cast<uint8_t>(b.bound);
                m_boundHead = (m_boundHead + 1) % kBoundHistory;
                if (m_boundCount < kBoundHistory) ++m_boundCount;
                ++m_boundNext;
            }
            if (m_boundCount > 0)
            {
                int c[5]{};
                for (int i = 0; i < m_boundCount; ++i) if (m_bound[i] < 5) ++c[m_bound[i]];
                const float nn = float(m_boundCount);
                std::snprintf(v, sizeof(v), "gpu %.0f%% cpu %.0f%% pace %.0f%%", 100.f * c[1] / nn,
                              100.f * c[2] / nn, 100.f * c[3] / nn);
                Row("Bound", v, kTextCol);
            }
            else Row("Bound", kAbsent, kDimCol);
            // Top passes of the newest GPU frame, top-level regions only (a
            // nested region is inside its parent's time).
            static P::GpuFrameData s_gpu;          // 24 KB: not on the stack; main thread only
            const uint64_t gf = P::LatestGpuFrame();
            if (gf != ~0ull && P::GetGpuFrame(gf, s_gpu))
            {
                struct Pass { P::NameId name; float ms; };
                Pass top[64];
                int np = 0;
                for (uint32_t i = 0; i < s_gpu.spanCount; ++i)
                {
                    const P::GpuSpan& sp = s_gpu.spans[i];
                    if (sp.depth != 0) continue;
                    const float ms = float(sp.endNs - sp.beginNs) * 1e-6f;
                    int k = 0;
                    while (k < np && top[k].name != sp.name) ++k;
                    if (k < np) top[k].ms += ms;
                    else if (np < 64) top[np++] = { sp.name, ms };
                }
                std::sort(top, top + np, [](const Pass& a, const Pass& b) { return a.ms > b.ms; });
                Row("", "", kDimCol);
                for (int i = 0; i < np && i < 8 && m_rowCount < kMaxRows; ++i)
                {
                    FmtMs(v, sizeof(v), top[i].ms);
                    Row(P::NameOf(top[i].name), v, i == 0 ? kTextCol : kDimCol);
                }
            }
            return;
        }

        // Level::Mem
        P::FrameRecord m;
        if (newestWith(6, [](const P::FrameRecord& x) { return x.memValid; }, m))
        {
            const float vram = float(double(m.vramLocalBytes) / 1048576.0);
            const float budget = float(double(m.vramLocalBudget) / 1048576.0);
            const float share = pct(vram, budget);
            std::snprintf(v, sizeof(v), "%.0f / %.0f MB (%.0f %%)", vram, budget, share);
            Row("VRAM", v, share > 95.f ? kBadCol : share > 80.f ? kWarnCol : kTextCol);
            std::snprintf(v, sizeof(v), "%.0f MB", double(m.vramNonLocalBytes) / 1048576.0);
            Row("Non-local", v, kDimCol);
            std::snprintf(v, sizeof(v), "%.0f MB", double(m.workingSetBytes) / 1048576.0);
            Row("Working set", v, kTextCol);
            std::snprintf(v, sizeof(v), "%.0f MB", double(m.privateBytes) / 1048576.0);
            Row("Private", v, kDimCol);
            std::snprintf(v, sizeof(v), "%u /frame", m.pageFaults);
            Row("Page faults", v, m.pageFaults > 1000 ? kWarnCol : kDimCol);
        }
        else
        {
            Row("VRAM", kAbsent, kDimCol);         // sampled by the collector
        }
        std::snprintf(v, sizeof(v), "%u /frame   %.2f MB", r.allocs, double(r.allocBytes) / 1048576.0);
        Row("Heap allocs", v, kTextCol);
        std::snprintf(v, sizeof(v), "%u /frame", r.frees);
        Row("Heap frees", v, kDimCol);
        // The GPU resource ledger: what the engine created, by category; "unattributed" is DXGI's
        // local usage the ledger cannot see (SDKs, driver-internal memory) - large is expected,
        // growing is not.
        {
            P::GpuLedger L;
            P::ReadGpuLedger(L);
            constexpr double kMB = 1048576.0;
            std::snprintf(v, sizeof(v), "%.0f MB   local %.0f", double(L.totalBytes) / kMB, double(L.localBytes) / kMB);
            Row("GPU ledger", v, kTextCol);
            int order[P::kGpuMemCats];
            for (int i = 0; i < P::kGpuMemCats; ++i) order[i] = i;
            std::sort(order, order + P::kGpuMemCats, [&](int a, int b) { return L.bytes[a] > L.bytes[b]; });
            for (int k = 0; k < 4; ++k)
            {
                const int i = order[k];
                if (!L.bytes[i]) break;
                char lab[kRowChars];
                std::snprintf(lab, sizeof(lab), "  %s", P::GpuMemCatName(static_cast<P::GpuMemCat>(i)));
                std::snprintf(v, sizeof(v), "%.0f MB  (%lld)", double(L.bytes[i]) / kMB, static_cast<long long>(L.count[i]));
                Row(lab, v, kDimCol);
            }
            P::FrameRecord mm;
            if (newestWith(6, [](const P::FrameRecord& x) { return x.memValid; }, mm))
            {
                std::snprintf(v, sizeof(v), "%.0f MB",
                              double(mm.vramLocalBytes) / kMB - double(L.localBytes) / kMB);
                Row("  unattributed", v, kDimCol);
            }
        }
        {
            double live = 0.0, cap = 0.0, lua = 0.0, jolt = 0.0;
            if (P::ReadGauge(P::Gauge::DescriptorsLive, live) && P::ReadGauge(P::Gauge::DescriptorsCapacity, cap))
            {
                std::snprintf(v, sizeof(v), "%.0f / %.0f", live, cap);
                Row("Descriptors", v, (cap > 0.0 && live > 0.9 * cap) ? kWarnCol : kDimCol);
            }
            if (P::ReadGauge(P::Gauge::LuaKB, lua))
            {
                std::snprintf(v, sizeof(v), "%.1f MB", lua / 1024.0);
                Row("Lua heap", v, kDimCol);
            }
            if (P::ReadGauge(P::Gauge::JoltStepPeakKB, jolt))
            {
                std::snprintf(v, sizeof(v), "%.0f KB", jolt);
                Row("Jolt step peak", v, kDimCol);
            }
        }
        Row("", "", kDimCol);
        struct ARow { uint32_t allocs; uint16_t index; };
        ARow rows[P::kFrameThreads];
        int n = 0;
        for (uint32_t i = 0; i < r.threadCount && n < P::kFrameThreads; ++i)
            if (r.threads[i].allocs) rows[n++] = { r.threads[i].allocs, r.threads[i].index };
        std::sort(rows, rows + n, [](const ARow& a, const ARow& b) { return a.allocs > b.allocs; });
        for (int i = 0; i < n && i < 4 && m_rowCount < kMaxRows; ++i)
        {
            P::ThreadCounters tc;
            const char* name = P::ReadThreadCounters(rows[i].index, tc) ? tc.name : "?";
            std::snprintf(v, sizeof(v), "%u allocs", rows[i].allocs);
            Row(name, v, kDimCol);
        }
    }

    DebugHud::Cost DebugHud::EstimateCost() const
    {
        // One glyph is exactly one quad: 4 verts + 6 indices (UIDrawList::
        // ReserveQuad). Spaces emit no quad, but counting them costs nothing
        // and erring high is the safe direction for a budget check.
        uint32_t glyphs = 0;
        for (int i = 0; i < m_rowCount; ++i)
            glyphs += uint32_t(std::strlen(m_label[i]) + std::strlen(m_value[i]));

        Cost c;
        c.vertices = glyphs * 4 + 4;   // + the backdrop quad
        c.indices  = glyphs * 6 + 6;
        return c;
    }

    void DebugHud::Emit(UIDrawList& dl, uint32_t canvasW, uint32_t canvasH,
                        uint32_t indexBudgetRemaining) const
    {
        (void)canvasW;
        if (m_level == Level::Off || m_suppressed || m_captureSuppressed) return;
        if (m_rowCount == 0 || canvasH == 0) return;

        Font& f = DefaultFont();
        if (!f.IsReady()) return;

        const float sizePx = ResolveSizePx(canvasH);
        const float pad    = std::round(sizePx * 0.45f);
        const float rowH   = std::round(sizePx * 1.28f);
        const float gap    = std::round(sizePx * 1.20f);
        const float margin = std::round(sizePx * 0.60f);

        // Measure first so numeric columns can be right-aligned: no monospace font ships (every
        // TTF under engine/font is proportional), so left-aligned digits would jitter every frame.
        float labelW = 0.f, valueW = 0.f;
        for (int i = 0; i < m_rowCount; ++i)
        {
            if (m_label[i][0]) labelW = std::max(labelW, f.Measure(m_label[i], sizePx).x);
            if (m_value[i][0]) valueW = std::max(valueW, f.Measure(m_value[i], sizePx).x);
        }

        const float panelW = pad * 2.f + labelW + gap + valueW;
        const float panelH = pad * 2.f + rowH * float(m_rowCount);
        const float x0 = margin, y0 = margin;

        // Budget check. Appended last so it draws on top, the HUD is also the first thing UIPass
        // truncates when the shared 65535-index draw list overflows - on exactly the heavy frames
        // it diagnoses. Drop rows deliberately instead, and say so.
        const Cost cost = EstimateCost();
        int rowLimit = m_rowCount;
        if (cost.indices > indexBudgetRemaining)
        {
            ENGINE_CHECK_ONCE(false,
                "DebugHud: UI index budget exhausted (need %u, have %u) — dropping rows",
                cost.indices, indexBudgetRemaining);
            uint32_t used = 6;   // backdrop
            rowLimit = 0;
            for (int i = 0; i < m_rowCount; ++i)
            {
                const uint32_t rowCost =
                    uint32_t(std::strlen(m_label[i]) + std::strlen(m_value[i])) * 6u;
                if (used + rowCost > indexBudgetRemaining) break;
                used += rowCost;
                ++rowLimit;
            }
            if (rowLimit == 0) return;
        }

        // Two phases on purpose: the backdrop is materialID 0 on the default white texture and
        // every glyph materialID 1 on the SDF atlas. UIDrawList::EnsureCmd cannot merge across
        // them, so a panel per row would cost two draw commands per row instead of two in all.
        dl.AddRectFilled(Vec2{ x0, y0 }, Vec2{ x0 + panelW, y0 + panelH }, kPanelCol);

        TextDrawParams tp;
        tp.sizePx = sizePx;
        // Outline rather than shadow: Font::DrawText re-walks the entire string
        // for a shadow (doubling the glyph count), while the outline is
        // shader-side and costs one deduped effect slot.
        tp.effects.outline        = true;
        tp.effects.outlineColor   = Color32(0, 0, 0, 255);
        tp.effects.outlineWidthPx = std::max(1.5f, sizePx * 0.08f);

        for (int i = 0; i < rowLimit; ++i)
        {
            const float ty = y0 + pad + rowH * float(i);
            if (m_label[i][0])
                f.DrawText(dl, Vec2{ x0 + pad, ty }, m_rowCol[i], m_label[i], tp);
            if (m_value[i][0])
            {
                const Vec2 vs = f.Measure(m_value[i], sizePx);
                f.DrawText(dl, Vec2{ x0 + panelW - pad - vs.x, ty },
                           m_rowCol[i], m_value[i], tp);
            }
        }
    }

} // namespace UI
