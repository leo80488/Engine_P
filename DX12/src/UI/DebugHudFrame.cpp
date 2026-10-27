// UI::DebugHud's two per-frame calls: the HUD's sampling + emission, and the profiler
// wants its pages hold.

#include "UI/DebugHud.h"

#include "Diagnostics/FrameTiming.h"
#include "Diagnostics/ProfilerCollector.h"
#include "Diagnostics/Profiler.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "Renderer/Renderer.h"

void UI::DebugHud::EmitFrame(Renderer& renderer, uint32_t viewportW, uint32_t viewportH)
{
    if (UIPass* uip = renderer.GetUIPass())
    {
        const auto& ft = Diag::FrameTiming::Get();
        const auto& fs = renderer.GetFrameStats();

        UI::DebugHudStats hs{};
        hs.fps           = ft.fps;
        hs.frameMs       = ft.frameSm;
        // The bracket contains the GPU fence wait; charging it to "CPU" makes a GPU-bound frame
        // look CPU-bound, the misattribution stat unit exists to prevent. cpuPeriodSm is the EMA
        // of (bracket - wait) per frame, so both terms share one smoothing.
        hs.cpuMs         = ft.cpuPeriodSm;
        hs.waitMs        = ft.gpuWaitMs;
        hs.gameMs        = ft.gameSm;
        hs.drawMs        = ft.drawSm;
        hs.renderPhaseMs = ft.renderPhaseSm;
        // Raw, not smoothed: FrameTiming keeps no EMA for these, and its smoothing set is pinned to
        // EditorLayer's. Diagnostic rows, read beside a smoothed RenderPhase on purpose.
        hs.renderOwnerMs    = ft.renderOwnerMs;
        hs.renderOwnerValid = ft.renderOwnerValid;
        hs.tailDrainMs      = ft.tailDrainMs;
        hs.parkHookCount    = ft.parkHookCount;
        hs.joinWaitMs    = ft.joinWaitSm;
        hs.joinWaitValid = ft.joinWaitValid;
        hs.renderMode    = ft.renderMode;
        hs.gpuBusyMs     = ft.gpuSm;
        hs.gpuValid      = ft.gpuValid;
        hs.gpuCalibrated = ft.gpuCalibrated;
        hs.gpuAgeFrames  = ft.gpuAgeFrames;

        hs.drawPackets   = fs.drawPackets;
        hs.instances     = fs.instancesEmitted;
        hs.triScene        = fs.triScene;
        hs.triSceneValid   = fs.triSceneValid;
        hs.triMainView     = fs.triMainView;
        hs.cpuCullBypassed = fs.cpuCullBypassed;
        hs.triOutline      = fs.triOutline;
        hs.triShadowOnly   = fs.triShadowOnly;
        hs.triTransparent  = fs.triTransparent;
        hs.gpuVisTris      = fs.gpuVisibleTriangles;
        hs.gpuVisValid     = fs.gpuVisStatsValid;
        hs.meshletPath     = fs.meshletPath;
        hs.meshletValid    = fs.meshletStatsValid;
        hs.meshletVisible  = fs.meshletVisible;
        hs.meshletTested   = fs.meshletTested;
        hs.meshletTris     = fs.meshletTris;
        // Prefer the always-available counter; fall back to the -artifacts-gated survivor ring.
        hs.gpuVisCmds      = fs.gpuVisStatsValid ? fs.gpuVisibleCommands
                                                 : fs.visibleDrawCmds;
        hs.gpuVisCmdsValid = fs.gpuVisStatsValid || fs.visibleDrawCmdsValid;

        Update(hs);

        UI::UIDrawList& dl = uip->GetDrawList();
        const size_t used = dl.Indices().size();
        Emit(dl, viewportW, viewportH,
                        (used < 65535u) ? uint32_t(65535u - used) : 0u);
    }
}

void UI::DebugHud::SyncProfilerWants(IGraphicsDevice& backend)
{
    // The HUD holds the timestamp profiler only while it shows the GPU row, as a want rather
    // than a write to GPUProfiler::enabled: closing the editor's Profiler window hard-writes
    // that flag false and would switch the HUD's GPU row off underneath it.
    const bool wantGpu = WantsGPUProfiler();
    if (wantGpu != m_heldGpuWant)
    {
        if (wantGpu) backend.AddGPUProfilerWant();
        else         backend.ReleaseGPUProfilerWant();
        m_heldGpuWant = wantGpu;
    }

    // stat threads / gpu / mem read the engine profiler's records.
    // With no -profile* flag the HUD starts a collector of its own
    // (no files without -artifacts=; it derives the GPU verdict and
    // samples memory) and switches the level between Stats and Off
    // with the page; a collector the command line started is never
    // touched - its level belongs to the capture.
    const bool wantProf = WantsProfiler();
    if (wantProf != m_heldProfiler)
    {
        namespace P = Diag::Prof;
        if (wantProf)
        {
            if (!P::CollectorRunning())
            {
                P::CollectorConfig cfg;
                cfg.level = P::GetLevel() > P::Level::Stats ? P::GetLevel() : P::Level::Stats;
                P::StartCollector(cfg);
                m_ownsCollector = P::CollectorRunning();
            }
            else if (m_ownsCollector && P::GetLevel() == P::Level::Off)
            {
                P::SetLevel(P::Level::Stats);
            }
        }
        else if (m_ownsCollector)
        {
            P::SetLevel(P::Level::Off);
        }
        m_heldProfiler = wantProf;
    }
}
