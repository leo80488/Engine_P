#pragma once

// FrameTiming - the single source of frame-level CPU / GPU timing numbers.
// Header-only and outside Harness/ because the debug HUD must work in a shipped Game.exe: Retail
// excludes src/Harness/*.cpp (Engine.Cpp.props), and a header has no vcxproj entry to exclude
// (Harness::FrameProbe survives the strip the same way).
// The numbers are not interchangeable: read the per-field notes before displaying one; several have
// a documented reason why the obvious reading is wrong.
// Writer: FramePipeline::PublishFrameTiming, once per frame at the CPU-bracket close (gpuWaitMs and
// the brs rows are written earlier in the frame). Readers: UI::DebugHud, the editor Profiler panel,
// Debug.Stats, frames.jsonl. The HUD and the Profiler share this object's smoothing, so they never
// disagree.

#include <cstdint>

namespace Diag
{
    struct FrameTiming
    {
        // ---- Smoothed ----
        // kEmaAlpha weights the newest frame in each EMA; the last kHistoryLen frames feed min / avg / max.
        static constexpr float kEmaAlpha   = 0.05f;
        static constexpr int   kHistoryLen = 240;

        // ---- Raw, written once per frame ----
        // True wall-clock frame period (realDt * 1000), including the pacing sleep and the message pump
        // outside the CPU bracket: derive FPS from this - 1000 / cpuFrameMs over-reports under VSync or any
        // frame cap.
        // One frame offset by design: realDt is timed from the top of the frame, so it is the previous
        // frame's period while cpuFrameMs is this frame's work. frameMs >= cpuFrameMs holds in steady state
        // but inverts across a spike (a first frame can take ~670 ms against an ~18 ms p50); compare the
        // smoothed pair (frameSm / cpuSm) when an invariant must hold.
        float    frameMs      = 0.f;
        // The App CPU bracket. NOT "pure CPU": it contains the frame-slot fence
        // wait (which is GPU back-pressure) and Present. Subtract gpuWaitMs
        // before labelling anything "CPU".
        float    cpuFrameMs   = 0.f;
        // Current loop entry to CPU-bracket start, including early park/pump.
        // Not additive with joinWaitMs (parks may lie inside either bracket).
        float    loopTopMs    = 0.f;
        // The render frame the render-span rows describe (an earlier packet than this bracket);
        // renderTimingValid is false until one completed.
        uint64_t renderTimingFrame = 0;
        bool     renderTimingValid = false;
        // The frame-slot fence wait, carved out of cpuFrameMs so a GPU-bound
        // frame does not inflate the CPU row.
        float    gpuWaitMs    = 0.f;
        // Sum of every non-Render scheduler phase: the "Game" row.
        float    gameMs       = 0.f;
        // Renderer::BeginFrame + Renderer::Render: the "Draw" row.
        float    drawMs       = 0.f;

        // GPUProfiler::effectiveFrameMs: the interval union across all queues. GPU busy time, not a
        // wall-clock span: every timestamp region wraps a pass and none spans the frame, so idle between
        // passes (cross-queue fences, barrier flushes) is invisible. Label the row "GPU busy".
        float    gpuBusyMs    = 0.f;
        float    gpuOverlapMs = 0.f;
        // False when the GPU profiler is off or has no results yet: render "--", never 0.00 (the same
        // contract as Renderer::FrameStats::visibleDrawCmdsValid).
        bool     gpuValid     = false;
        // GPUProfiler::calibrationValid. When false, gpuBusyMs degrades to the max of the per-queue unions
        // and gpuOverlapMs stays exactly 0.f - a confident zero, worse than an absent one: flag it.
        bool     gpuCalibrated = false;
        // How many frames ago the GPU numbers were measured (normally 2).
        // Never hard-code it; a skipped resolve makes it larger.
        uint32_t gpuAgeFrames = 0;

        // ---- Game / render split rows ----
        // renderPhaseMs is the Render phase's cost: beginWait + Renderer::BeginFrame + Render + rest +
        // present, the five [RENDERSPLIT] columns. `rest` (Render -> EndFrame plus the early UI build)
        // carries the present composition, the editor UI record and the PiP; a row without it would not be
        // comparable across executor shapes. Renderer::BeginFrame runs on the game half, so renderPhaseMs
        // is not a per-thread total (renderThreadMs is).
        float    beginWaitMs   = 0.f;   // device BeginFrame -> Render, incl. the GPU slot wait
        float    presentMs     = 0.f;   // backend.EndFrame wall time (submit + Present)
        float    restMs        = 0.f;   // Render -> EndFrame plus the early UI build
        float    renderPhaseMs = 0.f;   // beginWait + Renderer::BeginFrame + Render + rest + present
        // The render half's own span: beginWait + render + rest + present, without Renderer::BeginFrame
        // (and, under -renderowner, the UI build done on main). Elapsed time including suspended intervals
        // (and overlapped next-frame work in renderMode 3): neither owner active time nor OS CPU. Use
        // per-resume Tracy segments for owner wall time and OS thread accounting for CPU.
        float    renderThreadMs = 0.f;
        // Time the game side blocks handing a frame to a render thread. No executor joins one, so
        // joinWaitValid is always false and consumers render "--", not 0.00 (the gpuValid contract).
        float    joinWaitMs    = 0.f;
        bool     joinWaitValid = false;
        // The CPU bracket (cpuFrameMs) minus its GPU fence wait. Excludes loopTopMs; neither a complete
        // threaded critical path nor OS CPU work.
        float    cpuPeriodMs   = 0.f;
        // Which executor shape produced the frame; written into frames.jsonl and Debug.Stats so runs of
        // different modes are not mistaken for each other.
        uint8_t  renderMode    = 0; // 0 = serial UI, 2 = graph, 3 = graph-overlap; 1 is unused

        // ---- Owner-active rows ----
        // Beside the five span fields above, never replacing them: packs parse drawMs / renderPhaseMs /
        // renderThreadMs / restMs / beginWaitMs out of frames.jsonl, and their meaning must not move.
        // Those spans cross coroutine suspensions, so under pacing they read as a frame period, not CPU
        // (about 9.5x too high). These rows come from Diag::OwnerAccounting, which times node bodies on the
        // owner thread and sums them per render frame.
        // renderOwnerMs describes renderTimingFrame (the frame drawMs / renderPhaseMs describe) and is the
        // only row here that is not bracket-local, since a render frame's continuations straddle two
        // brackets: over a window mean(renderOwnerMs) is comparable with the bracket, per frame it is not.
        // Never subtract it from one frame's cpuFrameMs.
        float    renderOwnerMs = 0.f;
        // Absent, not zero: the accounting ring is 8 deep and a frame that
        // outlived it has no number at all.
        bool     renderOwnerValid = false;
        // Bracket-local (this frame's game half), unlike renderOwnerMs. tailDrainMs is the one
        // unconditional blocking DrainFrameWork at the Animation boundary: the barrier that closes the
        // overlap window, so it bounds the window a second owner thread could use. blockingDrainMs is every
        // blocking drain in the frame; blockingDrainMs - tailDrainMs is what the other drain sites cost.
        float    tailDrainMs     = 0.f;
        float    blockingDrainMs = 0.f;
        // Render continuations run opportunistically (the Pump sites) versus inside a blocking drain. Per
        // render frame, so they pair with renderOwnerMs and share its valid flag.
        uint32_t ownerContinuationsPump  = 0;
        uint32_t ownerContinuationsDrain = 0;
        // GraphicsDX12::DrainFrameIfNeeded park-hook fires during this frame's
        // bracket. Every fire collapses that frame's overlap window to zero.
        uint32_t parkHookCount = 0;

        // ---- BuildRenderScene's two halves ----
        // brsPlanMs is the sort + preamble + ResolveBatchPlan half, brsEmitMs the emit nest + post-nest
        // tail. With the lane emit (-brslane) both halves run on the render-owner lane (-brsplanlaneoff
        // keeps the plan on main); a lane half's time is published with the emit stats and written here on
        // main, one frame old.
        float    brsPlanMs = 0.f;
        float    brsEmitMs = 0.f;

        // ---- Smoothed ----
        // The HUD and the editor Profiler both read these, so the two readouts always agree.
        float frameSm = 0.f, cpuSm = 0.f, gameSm = 0.f, drawSm = 0.f, gpuSm = 0.f;
        float renderPhaseSm = 0.f, joinWaitSm = 0.f, cpuPeriodSm = 0.f;
        float fps     = 0.f;   // 1000 / frameSm

        float frameHistory[kHistoryLen]{};
        float cpuHistory  [kHistoryLen]{};
        float gpuHistory  [kHistoryLen]{};
        int   historyOffset = 0;
        int   filled        = 0;
        float frameMin = 0.f, frameAvg = 0.f, frameMax = 0.f;
        float gpuMin   = 0.f, gpuAvg   = 0.f, gpuMax   = 0.f;

        // ---- Singleton ----
        static FrameTiming& Get() { static FrameTiming s_instance; return s_instance; }

        // ---- Smoothed ----
        // Advance the EMAs and the history ring. Call exactly once per frame,
        // after the raw fields are written.
        void Update()
        {
            auto ema = [](float& smoothed, float raw)
            {
                // First-frame seed: an EMA started at 0 takes ~60 frames to
                // climb to a 16 ms reading, which reads as a fake warm-up spike.
                if (smoothed < 0.0001f) smoothed = raw;
                else                    smoothed += kEmaAlpha * (raw - smoothed);
            };
            ema(frameSm, frameMs);
            ema(cpuSm,   cpuFrameMs);
            ema(gameSm,  gameMs);
            ema(drawSm,  drawMs);
            ema(gpuSm,   gpuBusyMs);
            ema(renderPhaseSm, renderPhaseMs);
            ema(cpuPeriodSm,   cpuPeriodMs);
            // Only smooth the join wait once it is valid: an EMA seeded from the permanent 0 would read low for
            // ~60 frames after it becomes real.
            if (joinWaitValid) ema(joinWaitSm, joinWaitMs);

            fps = (frameSm > 0.0001f) ? (1000.0f / frameSm) : 0.0f;

            frameHistory[historyOffset] = frameMs;
            cpuHistory  [historyOffset] = cpuFrameMs;
            gpuHistory  [historyOffset] = gpuBusyMs;
            historyOffset = (historyOffset + 1) % kHistoryLen;
            if (filled < kHistoryLen) ++filled;

            frameMin = frameMax = frameAvg = 0.f;
            gpuMin   = gpuMax   = gpuAvg   = 0.f;
            if (filled > 0)
            {
                frameMin = gpuMin = 1e30f;
                float fSum = 0.f, gSum = 0.f;
                for (int i = 0; i < filled; ++i)
                {
                    const float f = frameHistory[i];
                    const float g = gpuHistory[i];
                    if (f < frameMin) frameMin = f;
                    if (f > frameMax) frameMax = f;
                    if (g < gpuMin)   gpuMin   = g;
                    if (g > gpuMax)   gpuMax   = g;
                    fSum += f; gSum += g;
                }
                frameAvg = fSum / static_cast<float>(filled);
                gpuAvg   = gSum / static_cast<float>(filled);
            }
        }
    };

} // namespace Diag
