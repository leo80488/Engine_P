// Graphics/SurfaceDX12.cpp - the main surface (include/Graphics/SurfaceDX12.h): the swap chain,
// the latched swap-chain ops, HDR output, the present composition and display encode, the frame
// limiter, Reflex and Frame Generation. Device state is reached through m_gfx. The back-buffer
// capture's bodies live in GraphicsDX12_Capture.cpp beside the other captures.
#include "Graphics/SurfaceDX12.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/GpuMemoryLedger.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "Graphics/HDROutput.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/StreamlineBackend.h"
#include "Diagnostics/Profiler.h"
#include "System/Log.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost - Frame Generation's docked-editor gate
#include "Harness/Determinism.h"
#include <algorithm>
#include <cstring>
#include <string>
#include "d3dx12.h"
#include <timeapi.h>            // timeBeginPeriod/timeEndPeriod (frame limiter)
#pragma comment(lib, "winmm.lib")

// High-resolution waitable timer flag (Windows 10 1803+). Defined defensively
// in case an older SDK's headers predate it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Blur Busters' "-N fps" rule: hold the cap this many Hz under the panel max so
// the frame time never crosses the top of the VRR window (where G-Sync drops
// out into tearing / forced VSync).
static constexpr int kGsyncMarginHz = 3;
using Microsoft::WRL::ComPtr;

// Constructed detached (no swap chain) by the device's constructor (GraphicsDX12::m_surface);
// Create puts it on a window.
SurfaceDX12::SurfaceDX12(GraphicsDX12& gfx)
    : m_gfx(gfx), m_mainThread(std::this_thread::get_id())
{
    // Frame-rate limiter: a high-resolution waitable timer backs the coarse
    // wait in LimitFrameRate (sub-ms precision, no busy-spin). Its accuracy is
    // independent of the global timer resolution, so we only raise that (a
    // process-wide power cost) when we have to fall back to Sleep - i.e. when
    // the high-res timer isn't available (pre-1803 or creation failure).
    m_frameTimerHandle = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!m_frameTimerHandle)
    {
        m_frameTimerHandle = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
        timeBeginPeriod(1);          // tighten Sleep granularity for the fallback
        m_mmTimerPeriodSet = true;
    }
}

// Run by ~GraphicsDX12 at a fixed point, with the device whole: Release is a
// no-op by then (the device released the swap chain first), and what is left -
// the present composition and the display-encode PSO, the HUD-less copy - goes
// while the queues and heaps it was made from are alive.
SurfaceDX12::~SurfaceDX12()
{
    Release();
    // Frame-limiter teardown (balances the constructor).
    if (m_mmTimerPeriodSet) { timeEndPeriod(1); m_mmTimerPeriodSet = false; }
    if (m_frameTimerHandle) { CloseHandle(m_frameTimerHandle); m_frameTimerHandle = nullptr; }
}

// App calls this right after creating the device, from the window it owns; Release runs before
// the device goes.
void SurfaceDX12::Create(HWND hWnd, UINT width, UINT height)
{
    if (!ENGINE_CHECK(!m_swapChain, "CreateMainSurface: the device already has its main surface"))
        return;
    m_hWnd   = hWnd;
    m_width  = width;
    m_height = height;

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.BufferCount = kFrameCount;
    scd.Width = width; scd.Height = height;
    // Boot always comes up SDR. The player's HDR preference arrives later through
    // EngineSettings::ApplyTo -> SetPresentSettings -> RequestHDROutputMode, which switches format
    // before any frame is presented (inline outside a frame, latched to ServicePendingSwapChainOps
    // inside one). One code path switches format, so the boot and runtime switches cannot drift
    // apart.
    scd.Format = m_swapChainFormat;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    if (m_gfx.m_tearingSupported)
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc;
    ThrowIfFailed(m_gfx.m_factory->CreateSwapChainForHwnd(m_gfx.m_queues[0].Get(), hWnd, &scd, nullptr, nullptr, &sc));
    ThrowIfFailed(m_gfx.m_factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

    // No explicit upgrade here: the factory above is already Streamline's
    // proxy, so CreateSwapChainForHwnd returned a proxied swap chain. Wrapping
    // it a second time would only risk a double proxy.

    ThrowIfFailed(sc.As(&m_swapChain));

    // Verify the interface we KEPT is still Streamline's proxy. sc.As() is a
    // QueryInterface, and a proxy that hands back the native object for a
    // different IID would route every Present around Streamline - Frame
    // Generation then reports eDLSSGStatusOk and presents nothing, with no
    // error anywhere. slGetNativeInterface returning something DIFFERENT from
    // what we hold is the proof that we are still in front of the proxy.
    if (SL::StreamlineBackend::Get().IsInitialized())
    {
        void* native = SL::StreamlineBackend::Get().GetNativeInterface(m_swapChain.Get());
        if (native == static_cast<void*>(m_swapChain.Get()))
            LOG_WARNING("GraphicsDX12: swap chain is NOT behind the Streamline proxy after "
                        "QueryInterface - Frame Generation cannot intercept Present");
        else
            LOG_INFO("GraphicsDX12: swap chain confirmed behind the Streamline proxy");

        // Earliest point the Frame Generation caps can be asked for: the proxy
        // now exists, and Frame Generation is definitionally off, so there is no
        // present counter to drain. Doing it here rather than lazily is what
        // lets the editor's multiplier list be right on its first frame instead
        // of starting at 2x and correcting itself a second later.
        GetFrameGenCaps();
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    ++m_swapChainGeneration;  // swap-chain (re)build - slot-parity re-base point

    // Swap-chain RTV heap (not managed by allocator)
    D3D12_DESCRIPTOR_HEAP_DESC rhd = {};
    rhd.NumDescriptors = kFrameCount;
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_gfx.m_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&m_scRTVHeap)));

    AcquireBackBuffers();
    m_viewport    = { 0,0, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_scissorRect = { 0,0, static_cast<LONG>(m_width),  static_cast<LONG>(m_height) };

    m_gfx.CreateHdrTarget(0, width, height);

    // Detect the monitor's refresh rate for the frame limiter's G-Sync ceiling.
    RefreshDisplayInfo();

    LOG_SUCCESS("GraphicsDX12: main surface created (%ux%u)", width, height);
}

// Runs before the device and its window go (App's MainSurface member); the device
// destructor's own Streamline shutdown is then a no-op. Frame Generation goes off first
// (DLSS-G holds interpolation state against the swap chain), and the chain leaves
// fullscreen while its HWND is still alive.
void SurfaceDX12::Release() noexcept
{
    if (!m_swapChain)
        return;
    try
    {
        SuspendFrameGeneration();
        if (m_gfx.m_device) m_gfx.FlushAndWait();
    }
    catch (...) {}
    SL::StreamlineBackend::Get().Shutdown();

    for (UINT n = 0; n < kFrameCount; ++n) m_renderTargets[n].Reset();
    m_swapChain->SetFullscreenState(FALSE, nullptr);
    m_swapChain.Reset();
    m_scRTVHeap.Reset();
    m_hWnd = nullptr;
    LOG_INFO("GraphicsDX12: main surface released");
}

// ==== The display snapshot ====
// What the engine knows about its monitor is read in one pass by the main thread (Plat::QueryDisplayMonitor:
// the monitor, its desktop mode and exact refresh rate, the DXGI output's HDR capability, Windows' SDR
// white level) and published as one immutable value. The HDR capability is the ONLY way to learn whether
// HDR is switched on in Windows Display Settings - there is no D3D12 feature bit; an HDR-capable panel in
// SDR desktop mode reports RGB_FULL_G22_NONE_P709 and clips anything sent as PQ - and the answer changes
// while the process runs (the user toggles it, or the window moves to another monitor). Every step is
// non-fatal: no output attached, a driver refusing the QI to IDXGIOutput6, or a remote-desktop session all
// land on hdrCapable = false (the editor greys out the HDR modes).
void SurfaceDX12::RefreshDisplayInfo()
{
    m_displayLooks.fetch_add(1, std::memory_order_relaxed);
    Plat::DisplayMonitor next = Plat::QueryDisplayMonitor(m_hWnd);
#if WITH_HARNESS
    // -display_sdr_white=<nits>: the snapshot's SDR white level, so a pack can move it (a monitor whose
    // Windows brightness slider sits elsewhere) and watch the paper white follow.
    static const float s_sdrWhite = Sys::CommandLine::Get().GetFloat("-display_sdr_white", 0.0f);
    if (s_sdrWhite > 0.0f)
        next.sdrWhiteNits = std::clamp(s_sdrWhite, kHDRPaperWhiteMin, kHDRPaperWhiteMax);
#endif
    const Plat::DisplayMonitorPtr prev = Display();
    if (next.SameDisplay(*prev))
        return;

    // A rate the driver does not report (0 or 1: "hardware default") keeps the last good reading rather
    // than clobbering it.
    if (!next.refresh.Known() && next.monitor == prev->monitor)
        next.refresh = prev->refresh;
    next.generation = prev->generation + 1;

    const bool hdrChanged = next.hdrCapable != prev->hdrCapable || next.maxLuminanceNits != prev->maxLuminanceNits;
    const bool monitorChanged = next.monitor != prev->monitor;
    m_display.store(std::make_shared<const Plat::DisplayMonitor>(next), std::memory_order_release);

    // Logged on a real change only: this runs on every settings apply and every monitor change.
    LOG_INFO("[DISPLAY] snapshot %llu: monitor %s, desktop %dx%d, refresh %.3f Hz (%u/%u), sdr white %.0f nits",
             static_cast<unsigned long long>(next.generation), monitorChanged ? "changed" : "same", next.desktopWidth,
             next.desktopHeight, next.refresh.Hz(), next.refresh.numerator, next.refresh.denominator,
             next.sdrWhiteNits);
    ResolveHDRLevels();
    if (hdrChanged)
    {
        LOG_INFO("GraphicsDX12: display HDR = %s (max %.0f nits, min %.4f nits, "
                 "max full-frame %.0f nits)",
                 next.hdrCapable ? "ENABLED" : "off",
                 next.maxLuminanceNits, next.minLuminanceNits, next.maxFullFrameNits);
        ReconcileHDROutputMode();
    }
}

void SurfaceDX12::NotePresentResult(HRESULT hr) noexcept
{
    // Present answers DXGI_STATUS_OCCLUDED while nothing it draws can be seen. Some presents never do
    // (a tearing one, a Streamline proxy chain): the window's own hit test (Window::UpdateCoverage) is the
    // answer that does not depend on the present path.
    m_occludedPresents = hr == DXGI_STATUS_OCCLUDED ? m_occludedPresents + 1 : 0;
    const bool occluded = m_occludedPresents >= kOccludedPresents;
    if (occluded != m_occluded.load(std::memory_order_relaxed))
    {
        m_occluded.store(occluded, std::memory_order_relaxed);
        LOG_INFO("GraphicsDX12: Present %s", occluded ? "reports the window covered (DXGI_STATUS_OCCLUDED)"
                                                      : "reaches the screen again");
    }
}

void SurfaceDX12::ServiceDisplay()
{
    Plat::RefreshInputs in;
    in.displayChanged = m_displayChanged.exchange(false, std::memory_order_acq_rel);
    in.windowMonitor  = reinterpret_cast<std::uintptr_t>(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST));
    in.knownMonitor   = Display()->monitor;
    // While HDR was REQUESTED but demoted because the display was not HDR, m_hdrMode is SDR: the unsatisfied
    // request is what keeps the poll alive, so the retry can fire when the user switches HDR on.
    in.hdrUnsatisfied = m_hdrModeRequested != m_hdrMode;
    if (Plat::NeedsDisplayRefresh(in, m_displayPoll))
    {
        ZoneScopedN("DisplayRefresh");
        RefreshDisplayInfo();
    }
}

// Toggling HDR in Windows, or dragging the window from an SDR monitor to an HDR one, happens
// while the process runs. Without this retry the user would have to restart or re-pick the
// same mode in the settings panel (which RequestHDROutputMode's idempotence guard rejects,
// since the request never changed).
void SurfaceDX12::ReconcileHDROutputMode()
{
    if (m_hdrModeRequested == m_hdrMode) return;

    // Never re-enter a rebuild that is already running. ApplyHDROutputMode
    // calls RefreshDisplayHDRInfo at its tail, which lands right back here.
    if (m_swapChainOpActive.load(std::memory_order_acquire)) return;

    // Only retry an HDR mode once the display actually claims to support it.
    // Without this the demotion inside ApplyHDROutputMode and the retry here
    // would ping-pong: demote -> requested != effective -> retry -> refused ->
    // demote. The caller already fires this only when the cached capability
    // CHANGED, so this is the second of two independent brakes.
    if (IsHDR(m_hdrModeRequested) && !Display()->hdrCapable) return;

    // LATCH only, never apply inline: the common caller is EndFrame's
    // monitor-change check, which is inside a frame.
    {
        std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
        m_pendingSwapChainOp.hdrMode = m_hdrModeRequested;
    }
    LOG_INFO("GraphicsDX12: display capability changed ??retrying %s",
             HDROutputModeName(m_hdrModeRequested));
}

const SL::FrameGenState& SurfaceDX12::GetFrameGenCaps()
{
    if (m_fgCapsProbed) return m_fgCaps;

    auto& sl = SL::StreamlineBackend::Get();
    if (!sl.IsFeatureSupported(SL::Feature::DLSS_G))
    {
        // Latch anyway. Without DLSS-G the answer can never change, and probing
        // every frame would be a per-frame call into the SDK for a constant.
        m_fgCapsProbed = true;
        return m_fgCaps;
    }

    // Never probe while Frame Generation is running: the same call resets the
    // presents-since-query counter EndFrame measures the ratio over, and the editor panel reads
    // this accessor every frame. While FG is off there is no counter to drain, and once it is on
    // the caps are already latched (FG cannot start without a non-zero maximum).
    if (m_frameGenActive != 0) return m_fgCaps;

    // Retry sparsely until the driver gives a usable answer: a query issued
    // before the proxy swap chain exists can legitimately report nothing.
    if (++m_fgCapsProbeTick < kFGCapsProbeInterval) return m_fgCaps;
    m_fgCapsProbeTick = 0;

    SL::FrameGenState probe{};
    if (sl.GetFrameGenState(probe))
    {
        m_fgCaps = probe;
        // Only latch once the query produced a usable maximum. A driver that
        // answers 0 here has told us nothing, and caching "nothing" would pin
        // the multiplier list to 2x forever.
        if (probe.numFramesToGenerateMax > 0)
        {
            m_fgCapsProbed = true;
            // No VRAM figure here on purpose: DLSS-G only computes it after it
            // has been configured, so at this point it is always 0 and printing
            // it would read as "Frame Generation costs nothing". The editor
            // reports it from the live state instead.
            LOG_INFO("GraphicsDX12: Frame Generation caps - up to %ux, dynamic %s, VSync %s",
                     probe.numFramesToGenerateMax + 1,
                     probe.dynamicMFGSupported ? "yes" : "no",
                     probe.vsyncSupported ? "yes" : "no");
        }
    }
    return m_fgCaps;
}

// Resolves the Reflex + Frame Generation REQUESTS into what Streamline is told (see the
// declaration); talks to Streamline only when the effective values change.
void SurfaceDX12::ApplyFrameGeneration()
{
    auto& sl = SL::StreamlineBackend::Get();

    int wantFrames = m_frameGenRequested;
    const char* refusal = nullptr;
    // Published for the editor at every exit below, so the panel never has to
    // send the user to the log for a reason the engine already knows.
    struct RefusalPublish
    {
        const char*& dst; const char*& src;
        ~RefusalPublish() { dst = src; }
    } refusalPublish{ m_frameGenRefusal, refusal };

    if (wantFrames > 0)
    {
        if (!sl.IsFeatureSupported(SL::Feature::DLSS_G))
        {
            refusal = sl.GetFeatureStatusText(SL::Feature::DLSS_G);
            wantFrames = 0;
        }
        else if (Sys::IsEditorHost() && !m_editorViewportFullscreen)
        {
            // DOCKED editor only. There the back buffer is ImGui chrome with the scene as
            // one panel inside it, and the depth / motion vectors describe only that
            // sub-rectangle of a differently-sized target. In the F11 fullscreen viewport
            // the renderer's viewport is the whole window, the arrangement matches what the
            // game build presents, and FG is allowed: the practical way to try FG without
            // launching a game build.
            refusal = "the docked editor composites the scene into a panel - press F11 "
                      "for fullscreen viewport, or run a game build";
            wantFrames = 0;
        }
        else if (m_hdrMode == HDROutputMode::scRGB)
        {
            // scRGB FP16 back buffers are not supported by DLSS-G itself (its own status
            // has eFailHDRFormatNotSupported for exactly this). HDR10 IS supported: see the
            // format declaration below.
            refusal = "scRGB output is not supported by Frame Generation (use SDR or HDR10)";
            wantFrames = 0;
        }
        // No exclusive-fullscreen gate, on purpose: Frame Generation runs in fullscreen.
        // Present's DXGI_ERROR_INVALID_CALL there came from the swap chain entering exclusive
        // fullscreen with ALLOW_TEARING (not from FG; it happens without Streamline too), and
        // ApplyFullscreenNow avoids it by going borderless.
    }

    // FG mandates Reflex. Rather than refuse, or silently rewrite the user's saved preference
    // (which is how a setting quietly disappears), force the EFFECTIVE mode up and leave
    // m_reflexModeRequested as the user left it. The editor labels this so the two do not look
    // inconsistent.
    int effReflex = m_reflexModeRequested;
    if (wantFrames > 0 && effReflex == 0) effReflex = 1;

    if (effReflex != m_reflexModeEffective)
    {
        m_reflexModeEffective = effReflex;
        const SL::ReflexMode mode = (effReflex == 2) ? SL::ReflexMode::LowLatencyWithBoost
                                  : (effReflex == 1) ? SL::ReflexMode::LowLatency
                                                     : SL::ReflexMode::Off;
        // Force the cap to be re-pushed in either direction so the handover
        // between the engine limiter and Reflex is clean both ways.
        m_reflexFrameLimitUs = UINT32_MAX;
        if (sl.IsFeatureSupported(SL::Feature::Reflex))
        {
            if (sl.SetReflexOptions(mode, 0))
                LOG_INFO("GraphicsDX12: Reflex %s%s",
                         effReflex == 2 ? "low latency + boost"
                       : effReflex == 1 ? "low latency" : "off",
                         (effReflex != m_reflexModeRequested) ? " (required by Frame Generation)" : "");
        }
        else if (effReflex != 0)
        {
            LOG_INFO("GraphicsDX12: Reflex requested but unavailable (%s) - "
                     "the engine frame limiter keeps pacing",
                     sl.GetFeatureStatusText(SL::Feature::Reflex));
        }
    }

    if (wantFrames == m_frameGenActive)
    {
        if (refusal && m_frameGenRequested > 0 && !m_loggedFGRefusal)
        {
            m_loggedFGRefusal = true;
            LOG_INFO("GraphicsDX12: Frame Generation unavailable - %s", refusal);
        }
        return;
    }

    // ---- Frame boundary ----
    // Everything above is a Streamline options call, safe from any call site. Everything below
    // recreates the swap chain (Streamline owns it through its proxy), which is legal only outside
    // a frame. The settings write is NOT at a frame boundary: the editor's Frame Generation combo
    // and EngineSettings::ApplyTo reach SetPresentSettings from a mid-frame handler while
    // primaryCL records against the current back buffer. So latch, like RequestHDROutputMode,
    // and let ServicePendingSwapChainOps re-resolve at App::Run's loop top (its FGRestore guard
    // calls back here unconditionally, so the request cannot be lost). The Reflex resolve above
    // still runs mid-frame, so a Reflex-only change applies immediately.
    if (!m_gfx.CanRebuildSwapChain())
    {
        {
            std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
            m_pendingSwapChainOp.frameGen = true;
        }
        return;
    }

    // Clamp to what the GPU can generate: numFramesToGenerate must not exceed
    // DLSSGState::numFramesToGenerateMax, or slDLSSGSetOptions fails and FG never comes up (e.g. a
    // 4x request on 2x hardware). The maximum is QUERIED here (slDLSSGGetState is valid at any
    // time), not taken from the health snapshot: that is only filled while FG runs, and FG cannot
    // run while the request is out of range.
    const bool wantDynamic = (wantFrames == RHI::PresentSettings::kFrameGenDynamic);
    // Arm the probe: this call site is rare and is the one that must not run on
    // a stale zero, because the clamp below is what keeps an over-request from
    // killing Frame Generation outright.
    m_fgCapsProbeTick = kFGCapsProbeInterval;
    const SL::FrameGenState& caps = GetFrameGenCaps();

    if (wantDynamic && !caps.dynamicMFGSupported)
    {
        // Dynamic needs >2x-capable hardware and a recent driver. Fall back to a fixed
        // multiplier rather than refusing Frame Generation outright: the user asked for FG,
        // and the dynamic part is a refinement of it.
        if (!m_loggedFGClamp)
        {
            m_loggedFGClamp = true;
            LOG_INFO("GraphicsDX12: Dynamic Frame Generation not supported here - using the fixed "
                     "%ux multiplier instead",
                     caps.numFramesToGenerateMax ? caps.numFramesToGenerateMax + 1 : 2);
        }
        wantFrames = caps.numFramesToGenerateMax
                       ? static_cast<int>(caps.numFramesToGenerateMax) : 1;
    }

    if (!wantDynamic && wantFrames > 0 && caps.numFramesToGenerateMax > 0 &&
        wantFrames > static_cast<int>(caps.numFramesToGenerateMax))
    {
        const int capped = static_cast<int>(caps.numFramesToGenerateMax);
        if (!m_loggedFGClamp)
        {
            m_loggedFGClamp = true;
            LOG_INFO("GraphicsDX12: Frame Generation %dx requested but this GPU supports at most %dx - "
                     "clamped", wantFrames + 1, capped + 1);
        }
        wantFrames = capped;
    }

    // The HUD-less and UI buffers carry the COMPOSITION format, the tone-map output's:
    // R8G8B8A8_UNORM in SDR, R16G16B16A16_FLOAT in HDR (mirrors HDROutput.h's
    // HDRCompositionFormat; keep in sync). Declaring it is what lets HDR10 work at all: there the
    // back buffer is RGB10A2 while these two are FP16, and DLSS-G accepts the mismatch only when
    // told.
    const uint32_t compFmt = static_cast<uint32_t>(
        IsHDR(m_hdrMode) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);

    const bool dynamicNow = (wantFrames == RHI::PresentSettings::kFrameGenDynamic);
    const SL::FrameGenMode slMode = (wantFrames <= 0) ? SL::FrameGenMode::Off
                                  : dynamicNow        ? SL::FrameGenMode::Dynamic
                                                      : SL::FrameGenMode::On;
    // Dynamic ignores the count; pass the hardware maximum so the field is at
    // least in range rather than carrying the sentinel through to the SDK.
    const uint32_t slCount = dynamicNow
        ? (caps.numFramesToGenerateMax ? caps.numFramesToGenerateMax : 1u)
        : (wantFrames > 0 ? static_cast<uint32_t>(wantFrames) : 1u);

    if (sl.SetFrameGenOptions(slMode, slCount, compFmt, compFmt))
    {
        m_frameGenActive  = wantFrames;
        m_loggedFGRefusal = false;
        if (dynamicNow)
            LOG_INFO("GraphicsDX12: Frame Generation ON (dynamic - multiplier tracks the display refresh)");
        else if (wantFrames > 0)
            LOG_INFO("GraphicsDX12: Frame Generation ON (%dx - generating %d frame(s))",
                     wantFrames + 1, wantFrames);
        else
            LOG_INFO("GraphicsDX12: Frame Generation OFF");
    }
    if (refusal && m_frameGenRequested > 0 && !m_loggedFGRefusal)
    {
        m_loggedFGRefusal = true;
        LOG_INFO("GraphicsDX12: Frame Generation unavailable - %s", refusal);
    }
}

void SurfaceDX12::CaptureHudLessColor(RHI::CommandList cl, ID3D12Resource* src)
{
    if (m_frameGenActive == 0 || !src || !m_gfx.m_device) return;
    if (m_renderTargets[0] == nullptr) return;

    const D3D12_RESOURCE_DESC bb  = m_renderTargets[0]->GetDesc();
    const D3D12_RESOURCE_DESC srcD = src->GetDesc();

    // Only the EXTENT has to match the back buffer (DLSS-G's rule). The FORMAT does not: the
    // copy target is created from the source's own desc so CopyResource is always
    // format-identical, and ApplyFrameGeneration declares that format through
    // DLSSGOptions::hudLessBufferFormat. That is what makes HDR10 work, where the scene
    // composites in FP16 while the back buffer is RGB10A2. Checked, not assumed: a size
    // mismatch would be a device-removal-grade error inside CopyResource.
    if (srcD.Width != bb.Width || srcD.Height != bb.Height)
    {
        if (!m_loggedHudlessSkip)
        {
            m_loggedHudlessSkip = true;
            LOG_WARNING("GraphicsDX12: HUD-less capture skipped - scene target %llux%u does not match "
                        "the back buffer %llux%u. Frame Generation will report OK but generate nothing.",
                        static_cast<unsigned long long>(srcD.Width), srcD.Height,
                        static_cast<unsigned long long>(bb.Width),  bb.Height);
        }
        return;
    }

    if (!m_hudlessTex)
    {
        // From the SOURCE desc, not the back buffer's (see the extent check).
        D3D12_RESOURCE_DESC d = srcD;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;   // copy destination + SRV only
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(m_gfx.m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&m_hudlessTex))))
        {
            LOG_ERROR("GraphicsDX12: failed to create the HUD-less colour texture");
            return;
        }
        GpuLedger::TrackTexture(m_gfx.m_device.Get(), m_hudlessTex.Get(), "Surface.Hudless");
        m_hudlessTex->SetName(L"FG.HudLessColor");
    }

    ID3D12GraphicsCommandList* native = m_gfx.GetNativeCommandList(cl);
    if (!native) return;

    // The texture's ONE steady state is NON_PIXEL_SHADER_RESOURCE: the state the tag below
    // declares, and the state it must be in when Frame Generation reads it at Present, long
    // after this command list executed. So the copy is bracketed and the resource left
    // readable; leaving it in COPY_DEST would make the tag lie about a resource SL then reads.
    const CD3DX12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        m_hudlessTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    native->ResourceBarrier(1, &toCopy);
    GfxDiag::NoteNativeBarriers(native, &toCopy, 1);   // barrier journal

    native->CopyResource(m_hudlessTex.Get(), src);

    const CD3DX12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(
        m_hudlessTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    native->ResourceBarrier(1, &toRead);
    GfxDiag::NoteNativeBarriers(native, &toRead, 1);

    SL::ResourceTag tag{};
    tag.type     = SL::BufferType::HUDLessColor;
    tag.resource = m_hudlessTex.Get();
    tag.state    = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // ValidUntilPresent: Frame Generation consumes this when it takes over
    // Present, not during any Evaluate.
    tag.life     = SL::ResourceLife::ValidUntilPresent;
    SL::StreamlineBackend::Get().TagResourcesForFrame(
        &tag, 1, SL::StreamlineBackend::Get().RenderFrameToken(), native);
}

// The ISurface form: the Renderer hands an RHI texture.
void SurfaceDX12::CaptureHudLessColor(RHI::CommandList cl, const RHI::Texture& src)
{
    CaptureHudLessColor(cl, m_gfx.GetTextureResource(src));
}

// Tagged once per frame, on the pre-compute list, and NOT in DLSSPass, because two features
// want depth and motion with different lifetimes: DLSS SR consumes them inside
// slEvaluateFeature, Frame Generation at Present. ValidUntilPresent satisfies both, and Frame
// Generation can be on while DLSS SR is not (FSR2, or no upscaler at all).
void SurfaceDX12::TagDepthAndMotion(RHI::CommandList cl, const RHI::Texture& depth, const RHI::Texture& motion,
                                    void* frameToken)
{
    constexpr uint32_t kComputeRead =
        static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SL::ResourceTag slTags[2]{};
    slTags[0] = { SL::BufferType::Depth, m_gfx.GetTextureResource(depth), kComputeRead,
                  0, 0, 0, 0, SL::ResourceLife::ValidUntilPresent };
    slTags[1] = { SL::BufferType::MotionVectors, m_gfx.GetTextureResource(motion), kComputeRead,
                  0, 0, 0, 0, SL::ResourceLife::ValidUntilPresent };
    SL::StreamlineBackend::Get().TagResourcesForFrame(slTags, 2, frameToken, m_gfx.GetNativeCommandList(cl));
}

const RHI::Texture* SurfaceDX12::BeginFrameGenUICapture(RHI::CommandList cl)
{
    if (m_frameGenActive == 0 || !m_gfx.m_device || m_renderTargets[0] == nullptr) return nullptr;

    const D3D12_RESOURCE_DESC bb = m_renderTargets[0]->GetDesc();

    if (!m_fgUITexCreated)
    {
        RHI::TextureDesc td{};
        td.width  = static_cast<uint32_t>(bb.Width);
        td.height = bb.Height;
        // Extent must match the back buffer (DLSS-G requirement); the FORMAT must match the
        // TONE-MAP OUTPUT, because UIPass renders here with the PSOs it already built for that
        // target (the Renderer pushes the composition format via SetTargetFormat). A fixed
        // R8G8B8A8 would be a debug-layer RTV mismatch under HDR and invalidate every cached
        // custom-text-shader PSO. ApplyFrameGeneration declares this same format to DLSS-G.
        td.format     = HDRCompositionFormat(m_hdrMode);
        td.bindFlags = RHI::BindFlag::RENDER_TARGET | RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::RENDERTARGET;
        td.debugName = "FG.UIColorAndAlpha";
        if (!m_gfx.CreateTexture(td, m_fgUITex))
        {
            LOG_ERROR("GraphicsDX12: failed to create the Frame Generation UI target");
            return nullptr;
        }
        m_fgUITexCreated = true;
    }
    else
    {
        // Steady state between frames is SHADER_RESOURCE_COMPUTE (what the tag
        // declared last frame); bring it back to a render target.
        m_gfx.PushBarrier(RHI::GPUBarrier::Image(&m_fgUITex,
                        RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                        RHI::ResourceState::RENDERTARGET), cl);
    }

    // Fully transparent. UIPass does not clear (it normally draws over the tone-mapped
    // scene), so without this the HUD would accumulate across frames and the alpha channel -
    // the very thing DLSS-G separates the UI by - would be meaningless.
    static const float kTransparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_gfx.ClearRenderTarget(m_fgUITex, kTransparent, cl);

    return &m_fgUITex;
}

void SurfaceDX12::EndFrameGenUICapture(RHI::CommandList cl)
{
    if (m_frameGenActive == 0 || !m_fgUITexCreated) return;

    // UIPass leaves the target in SHADER_RESOURCE; move it to the state the tag
    // declares and that Frame Generation reads it in at Present.
    m_gfx.PushBarrier(RHI::GPUBarrier::Image(&m_fgUITex,
                    RHI::ResourceState::SHADER_RESOURCE,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);

    SL::ResourceTag tag{};
    tag.type     = SL::BufferType::UIColorAndAlpha;
    tag.resource = m_gfx.GetTextureResource(m_fgUITex);
    tag.state    = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tag.life     = SL::ResourceLife::ValidUntilPresent;
    SL::StreamlineBackend::Get().TagResourcesForFrame(
        &tag, 1, SL::StreamlineBackend::Get().RenderFrameToken(), m_gfx.GetNativeCommandList(cl));
}

void SurfaceDX12::SetEditorViewportFullscreen(bool v)
{
    if (v == m_editorViewportFullscreen) return;
    m_editorViewportFullscreen = v;
    // Toggling F11 changes whether FG is allowed, so re-resolve. Streamline rebuilds its swap
    // chain on an FG mode change, which is only legal outside a frame; App calls this from the
    // frame's editor section, and ApplyFrameGeneration latches the rebuild there.
    m_loggedFGRefusal = false;
    ApplyFrameGeneration();
}

void SurfaceDX12::ReleaseFrameGenTextures()
{
    // Sized to the back buffer, so a resize invalidates both. The GPU may still
    // be reading them, hence the deferred path rather than an immediate release.
    if (m_fgUITexCreated)
    {
        m_gfx.DestroyTexture(m_fgUITex);
        m_fgUITexCreated = false;
    }
    if (m_hudlessTex)
    {
        m_gfx.DeferReleaseResource(m_hudlessTex);
        m_hudlessTex.Reset();
    }
    m_loggedHudlessSkip = false;
}

bool SurfaceDX12::SuspendFrameGeneration()
{
    if (m_frameGenActive == 0) return false;
    SL::StreamlineBackend::Get().SetFrameGenOptions(SL::FrameGenMode::Off, 1);
    m_frameGenActive = 0;
    return true;
}

void SurfaceDX12::ResumeFrameGeneration(bool /*wasOn*/)
{
    // Deliberately ignores @p wasOn and always re-resolves: the gates can have CHANGED across
    // the operation this brackets. An HDR change to scRGB must leave FG off even though it was
    // on going in, and leaving scRGB must turn FG back on even though it was off going in;
    // keying the resume on the prior state gets the second case wrong and FG never returns.
    // ApplyFrameGeneration is idempotent, so this is free when nothing changed.
    ApplyFrameGeneration();
}

int SurfaceDX12::EffectiveFrameCap() const
{
    int cap = targetFrameRate > 0 ? targetFrameRate : 0;   // 0 = unlimited
    // The sub-refresh ceiling only makes sense when VRR can engage: it needs tearing-legal
    // presents (the vrrPresent gate in Present) and a known refresh. Without tearing support the
    // present path hard-syncs to vblank, so capping under refresh would only underrun it into
    // judder.
    // The whole Hz at or below the exact rate: a ceiling from 59.94 never reaches the panel's top.
    const int refreshHz = Display()->refresh.WholeHz();
    if (gsyncEnabled && m_gfx.m_tearingSupported && refreshHz > 1)
    {
        const int gsyncCeil = refreshHz > kGsyncMarginHz
                            ? refreshHz - kGsyncMarginHz
                            : refreshHz;
        // Ceiling wins over a higher / unlimited target: staying under the
        // panel max is what keeps G-Sync engaged.
        if (cap == 0 || cap > gsyncCeil) cap = gsyncCeil;
    }
    return cap;
}

void SurfaceDX12::LimitFrameRate()
{
    // -nopacing: a bounded automated run should finish at machine speed, not at the display
    // refresh rate (300 frames at a 60 fps cap are ~5 s of pure waiting). Simulation state is
    // unaffected once -fixeddt is in play, so this is free wall-clock.
    if (Harness::Determinism::Get().NoPacing()) return;
    const int cap = EffectiveFrameCap();

    // ---- Reflex owns pacing when it is active ----
    // Reflex delays the START of the next frame (slReflexSleep at the top of the loop); this
    // limiter delays the END of this one. Running both means two waits per frame, and the
    // engine's wait comes AFTER Present, lengthening exactly the interval Reflex shortens.
    // So the cap is DELEGATED, not dropped: EffectiveFrameCap still decides the number (the
    // G-Sync ceiling is about the panel, not about who sleeps) and Reflex enforces it via
    // frameLimitUs. Pushed only on change: slReflexSetOptions is not free, and the cap moves only
    // when a setting or the display does.
    auto& sl = SL::StreamlineBackend::Get();
    if (sl.IsReflexPacing())
    {
        const uint32_t limitUs = cap > 0 ? static_cast<uint32_t>(1000000.0 / cap) : 0u;
        if (limitUs != m_reflexFrameLimitUs)
        {
            m_reflexFrameLimitUs = limitUs;
            sl.SetReflexOptions(sl.GetReflexMode(), limitUs);
            // Say who is pacing. Otherwise the handover shows only as
            // GetLastFramePacingWaitMs() dropping to zero, which in a profiler looks
            // exactly like "the frame cap stopped working".
            if (limitUs)
                LOG_INFO("GraphicsDX12: frame cap %d FPS delegated to Reflex (%u us/frame); "
                         "engine limiter standing down", cap, limitUs);
            else
                LOG_INFO("GraphicsDX12: uncapped; Reflex pacing without a frame limit");
        }
        // Our own schedule must not persist across the handover, or re-enabling
        // the engine limiter later would resume from a stale deadline and burn
        // one long frame.
        m_nextFramePresentQpc = 0;
        m_lastPacingWaitMs    = 0.f;
        return;
    }
    // Reflex just turned off (or never ran): forget the delegated cap so the
    // next handover re-pushes it.
    m_reflexFrameLimitUs = UINT32_MAX;

    if (cap <= 0) { m_nextFramePresentQpc = 0; m_lastPacingWaitMs = 0.f; return; }   // unlimited: no pacing

    // Own named zone so Tracy attributes the limiter wait (waitable-timer sleep =
    // descheduled, near-zero CPU) to "FramePacingWait" instead of inflating the caller's zone,
    // where the pacing sleep would read as main-thread CPU work.
    ZoneScopedN("FramePacingWait");
    // The limiter: a waitable-timer sleep plus a sub-millisecond spin tail.
    // Both are "idle by design", so the whole block is pacing (the spin tail's
    // CPU cost is visible in the OS CPU column, not hidden in Work).
    PROF_WAIT(WaitPacing, "FramePacingWait");

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const int64_t waitStartQpc = now.QuadPart;              // for m_lastPacingWaitMs
    const int64_t period = freq.QuadPart / cap;            // QPC ticks / frame

    // Advance the schedule by one period; on the first call (or after an uncapped stretch
    // zeroed it) start fresh from 'now'. Then clamp so the deadline never trails 'now': a hitch
    // costs at most one paced frame, never a burst of zero-delay catch-up frames that overshoot
    // the cap (which on a VRR panel drops out of the G-Sync window).
    // A deadline behind 'now' resyncs to 'now' (present immediately), NOT 'now + period': that
    // would add a full idle period EVERY frame whenever the frame work A exceeds the period P,
    // making the steady-state period A + P and roughly halving FPS. Resyncing to 'now' still caps
    // catch-up at one frame; the next frame paces normally from here.
    if (m_nextFramePresentQpc == 0)
        m_nextFramePresentQpc = now.QuadPart + period;
    else
    {
        m_nextFramePresentQpc += period;
        if (m_nextFramePresentQpc < now.QuadPart)
            m_nextFramePresentQpc = now.QuadPart;
    }

    // Wait out the remaining budget: a coarse high-res-timer sleep for the bulk,
    // then a busy-spin over the final < ~1 ms for jitter-free precision.
    for (;;)
    {
        QueryPerformanceCounter(&now);
        const int64_t remaining = m_nextFramePresentQpc - now.QuadPart;
        if (remaining <= 0) break;

        const double remMs = static_cast<double>(remaining) * 1000.0
                           / static_cast<double>(freq.QuadPart);
        if (remMs > 1.0)
        {
            const DWORD sleepMs = static_cast<DWORD>(remMs);   // leave the tail to spin
            bool slept = false;
            if (m_frameTimerHandle)
            {
                LARGE_INTEGER due;   // negative = relative, in 100 ns units
                due.QuadPart = -static_cast<LONGLONG>((remMs - 0.5) * 10000.0);
                if (SetWaitableTimer(m_frameTimerHandle, &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject(m_frameTimerHandle, sleepMs + 2);
                    slept = true;
                }
            }
            // Fallback (no or failed high-res timer): Sleep the bulk, but never Sleep(0)-spin;
            // for the sub-2 ms band drop straight to YieldProcessor.
            if (!slept)
            {
                if (sleepMs > 1) Sleep(sleepMs - 1);
                else             YieldProcessor();
            }
        }
        else
        {
            YieldProcessor();   // spin the final sub-ms
        }
    }

    // Record how long we paced: debug UIs (Phase Debug panel) show this next to the phase
    // timings so "idle by design" is never mistaken for CPU work.
    QueryPerformanceCounter(&now);
    m_lastPacingWaitMs = static_cast<float>(
        static_cast<double>(now.QuadPart - waitStartQpc) * 1000.0
        / static_cast<double>(freq.QuadPart));
}

// ==== Present composite: THE DISPLAY ENCODE ====
// A self-contained PSO (own root signature and shaders, independent of the engine's default
// root signature and ShaderLibrary) samples the extended-gamma composition buffer and writes
// the bound swap-chain RTV. This is the ONE place the engine converts its internal
// composition space (Graphics/HDROutput.h) to the signal the display expects. Its inverse is
// the tail of ToneMap.cs.hlsl; the two must stay exact inverses, which is why both use the
// same 2.2 exponent and neither clamps in between.
// The shaders (shaders/Present.vs.hlsl, shaders/Present.ps.hlsl) load through
// ShaderDiskCache::LoadOrCompile: cached, recorded as variants a package carries cooked,
// compiled by the offline gates and hot-reloaded (ReloadPresentShaders). A shader that does
// not compile leaves m_presentPSO null and CompositeTextureToSwapChain draws nothing: on a
// black screen, check DX12Log.txt for "SurfacePresent" first.

void SurfaceDX12::EnsurePresentPSO()
{
    // Rebuild when the swap-chain format changed under us: the PSO bakes RTVFormats[0].
    if (m_presentPSO && m_presentPSOFormat == m_swapChainFormat) return;
    m_presentPSO.Reset();
    m_presentRootSig.Reset();
    (void)BuildPresentPSO(m_presentRootSig, m_presentPSO);
}

// Hot reload (the LoadOrCompile pattern): load again - which tells the reload's receipt this
// holder let go of the old code - and swap the new pipeline in only once it built, so a
// failure keeps the picture. The reload flushed the GPU first, so the old pipeline is idle.
// A surface that never composited has nothing to rebuild: its first composite loads the new
// code.
bool SurfaceDX12::ReloadPresentShaders()
{
    if (!m_presentPSO) return false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (!BuildPresentPSO(rootSig, pso)) return false;
    m_presentRootSig = std::move(rootSig);
    m_presentPSO     = std::move(pso);
    return true;
}

bool SurfaceDX12::BuildPresentPSO(Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRootSig,
                                  Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO)
{
    // Empty = failed; LoadOrCompile logged why (the file, DXC's message).
    const std::vector<uint8_t> vs = ShaderDiskCache::LoadOrCompile(
        "shaders/Present.vs.hlsl", "main", RHI::ShaderStage::VS, {}, "SurfacePresent");
    const std::vector<uint8_t> ps = ShaderDiskCache::LoadOrCompile(
        "shaders/Present.ps.hlsl", "main", RHI::ShaderStage::PS, {}, "SurfacePresent");
    if (vs.empty() || ps.empty()) return false;

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> err;

    // Root signature: one SRV table + six root constants + one static
    // linear-clamp sampler. Root constants (not a CBV) because the encode
    // parameters change at most once per frame and are 24 bytes - a constant
    // buffer would need a per-frame upload ring for no benefit.
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[1].InitAsConstants(6, /*shaderRegister b*/0, /*space*/0,
                              D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.RegisterSpace    = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(_countof(params), params, 1, &samp,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rsBlob, &err)))
    {
        LOG_ERROR("Present root sig: %s",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "(no diag)");
        return false;
    }
    ComPtr<ID3D12RootSignature> rootSig;
    HRESULT hr = m_gfx.m_device->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    if (FAILED(hr))
    {
        LOG_ERROR("Present root sig: CreateRootSignature failed 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = rootSig.Get();
    psoDesc.VS                    = CD3DX12_SHADER_BYTECODE(vs.data(), vs.size());
    psoDesc.PS                    = CD3DX12_SHADER_BYTECODE(ps.data(), ps.size());
    psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    // Tracks the live swap-chain format, which changes with the HDR output
    // mode. NOT a literal - a stale format here is a silent RTV mismatch.
    psoDesc.RTVFormats[0]         = m_swapChainFormat;
    psoDesc.SampleDesc.Count      = 1;

    // Through the PSO library like every other pipeline; the swap-chain format is the one state that
    // varies, so it is the name's state key.
    ComPtr<ID3D12PipelineState> pso;
    if (!m_gfx.CreateGraphicsPipelineCached(psoDesc, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            static_cast<uint64_t>(m_swapChainFormat), pso))
    {
        LOG_ERROR("Present PSO: CreateGraphicsPipelineState failed");
        return false;
    }
    outRootSig = std::move(rootSig);
    outPSO     = std::move(pso);
    m_presentPSOFormat = m_swapChainFormat;
    return true;
}

void SurfaceDX12::CompositeTextureToSwapChain(uint64_t srvGpuHandle,
                                               RHI::CommandList cmd)
{
    CompositeTextureToSwapChainRect(srvGpuHandle, 0, 0,
                                    static_cast<int32_t>(m_width),
                                    static_cast<int32_t>(m_height), cmd);
}

// The full-screen composite and the PiP inset are ONE function with a rect: same PSO, shader
// and display-encode root constants. A picture-in-picture with its own shader would need an
// offline-dxc entry, a shader cache slot and a packaging entry to draw the identical triangle.
void SurfaceDX12::CompositeTextureToSwapChainRect(uint64_t srvGpuHandle,
                                                   int32_t x, int32_t y,
                                                   int32_t w, int32_t h,
                                                   RHI::CommandList cmd)
{
    if (srvGpuHandle == 0) return;
    if (w <= 0 || h <= 0)  return;
    EnsurePresentPSO();
    if (!m_presentPSO) return;

    ID3D12GraphicsCommandList* list = m_gfx.GetPoolEntry(cmd).GetCommandList();

    // CLAMPED TO THE BACK BUFFER. A rect that hangs off the edge is a
    // D3D12 validation error on the scissor, not a clipped picture.
    const int32_t bw = static_cast<int32_t>(m_width);
    const int32_t bh = static_cast<int32_t>(m_height);
    if (x >= bw || y >= bh) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return;

    D3D12_VIEWPORT vp{ static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(w), static_cast<float>(h), 0.f, 1.f };
    D3D12_RECT sc{ static_cast<LONG>(x), static_cast<LONG>(y),
        static_cast<LONG>(x + w), static_cast<LONG>(y + h) };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    list->SetPipelineState(m_presentPSO.Get());
    list->SetGraphicsRootSignature(m_presentRootSig.Get());

    // Bind the shader-visible heaps here (the caller likely did, but switching root signatures
    // can reset the implicit binding). Both engine heaps, as on every engine list:
    // SetDescriptorHeaps replaces the bound set, and the shared signatures index the sampler
    // heap too, so a CBV/SRV/UAV-only bind would leave it unbound for anything after.
    ID3D12DescriptorHeap* heaps[] = { m_gfx.m_cbvSrvUavAllocator.GetGpuHeap(),
                                      m_gfx.m_samplerAllocator.GetGpuHeap() };
    list->SetDescriptorHeaps(2, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{ srvGpuHandle };
    list->SetGraphicsRootDescriptorTable(0, srvHandle);

    // Display-encode parameters. m_hdrMode is the mode ACTUALLY in effect
    // (post-fallback), so if the swap chain refused the HDR colour space this
    // automatically degrades to the SDR passthrough and the image stays correct
    // instead of being double-encoded against an sRGB back buffer.
    // The colour-vision assist rides along: a mode of 0 (or a severity of 0) is the passthrough.
    int         colorVision = m_colorVision.load(std::memory_order_relaxed);
    const float severity    = m_colorVisionSeverity.load(std::memory_order_relaxed);
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -present_no_colorvision (reverse arm): the assist never reaches the present shader, as when it
    // ran in the tone map and the HUD drawn after it stayed uncorrected.
    static const bool s_noAssist = Sys::CommandLine::Get().Has("-present_no_colorvision");
    if (s_noAssist) colorVision = 0;
#endif
    uint32_t debugFlags = 0;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -present_no_dither: the HDR10 signal without its dither (a reverse arm).
    static const bool s_noDither = Sys::CommandLine::Get().Has("-present_no_dither");
    if (s_noDither) debugFlags |= 1u;
#endif
    struct PresentConstants
    {
        uint32_t mode;
        float    paperWhiteNits;
        float    peakNits;
        uint32_t debugFlags;
        uint32_t colorVision;
        float    colorVisionSeverity;
    } pc{ static_cast<uint32_t>(m_hdrMode), m_paperWhiteNits, m_peakNits, debugFlags,
          static_cast<uint32_t>(colorVision), severity };
    static_assert(sizeof(PresentConstants) == 24,
                  "present root constants must stay 6 DWORDs (see InitAsConstants(6,...))");
    list->SetGraphicsRoot32BitConstants(1, 6, &pc, 0);

    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 0, nullptr);
    list->IASetIndexBuffer(nullptr);
    list->DrawInstanced(3, 1, 0, 0);

    // RESTORE THE FULL VIEWPORT after an inset. Viewport and scissor are COMMAND-LIST state, not
    // draw state: a latched corner rect would silently clip whatever is recorded next on this
    // list (today the PiP is the last draw before EndFrame; do not rely on it).
    if (x != 0 || y != 0 || w != bw || h != bh)
    {
        D3D12_VIEWPORT full{ 0.f, 0.f, static_cast<float>(bw),
                             static_cast<float>(bh), 0.f, 1.f };
        D3D12_RECT fullSc{ 0, 0, static_cast<LONG>(bw), static_cast<LONG>(bh) };
        list->RSSetViewports(1, &full);
        list->RSSetScissorRects(1, &fullSc);
    }
}

// ==== Window / resize / fullscreen ====

// The swap chain's buffers and their RTVs, after creation and after every ResizeBuffers: the
// one place the GPU ledger entries and debug names are made. A null RTV desc: the views take
// whatever format the buffers have now, so an HDR switch needs no per-format bookkeeping.
void SurfaceDX12::AcquireBackBuffers()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        GpuLedger::TrackCommitted(m_gfx.m_device.Get(), m_renderTargets[n].Get(), Diag::Prof::GpuMemCat::SwapChain,
                                  "SwapChain.BackBuffer");
        const std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
        m_renderTargets[n]->SetName(bbName.c_str());
        m_gfx.m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, h);
        h.ptr += m_gfx.m_rtvDescriptorSize;
    }
}

void SurfaceDX12::ResizeSwapChainAndRtv(UINT width, UINT height)
{
    // The Frame Generation scratch targets are sized and formatted to the back buffer, so
    // they die with it (RebuildBackBuffers does the same for an HDR format change).
    ReleaseFrameGenTextures();

    // Same reasoning as ApplyFullscreenNow: FG must be off while the swap chain is rebuilt,
    // and this is reached from paths that never pass through the latched
    // ServicePendingSwapChainOps guard.
    const bool fgWasOn = SuspendFrameGeneration();
    struct FGGuard
    {
        SurfaceDX12* self; bool wasOn;
        ~FGGuard() { self->ResumeFrameGeneration(wasOn); }
    } fgGuard{ this, fgWasOn };

    for (UINT n = 0; n < kFrameCount; ++n) m_renderTargets[n].Reset();
    // DXGI_FORMAT_UNKNOWN = "keep the current format". Deliberate: a plain
    // window resize must never drop an HDR format back to 8-bit. Only
    // ApplyHDROutputMode passes an explicit format.
    ThrowIfFailed(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
        m_gfx.m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    ++m_swapChainGeneration;  // swap-chain (re)build - slot-parity re-base point
    AcquireBackBuffers();
    m_width = width; m_height = height;
    m_viewport    = { 0,0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    m_scissorRect = { 0,0, static_cast<LONG>(width),  static_cast<LONG>(height) };

    // ResizeBuffers resets the swap chain's colour space to the sRGB default. Re-assert ours, or
    // an HDR session reverts to SDR interpretation on the first resize: the image stays
    // HDR-encoded but the display reads it as sRGB (blown-out neon, no error).
    // If it is refused (window dragged onto an SDR monitor, say) we cannot demote here - that
    // needs another ResizeBuffers inside this rebuild - so latch the demotion as one coherent mode
    // transition at the next frame boundary.
    if (!ApplySwapChainColorSpace() && IsHDR(m_hdrMode))
        {
            std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
            m_pendingSwapChainOp.hdrMode = HDROutputMode::SDR;
        }
}

// ==== HDR output mode ====

void SurfaceDX12::RequestHDROutputMode(HDROutputMode mode)
{
    // Idempotence is CORRECTNESS here, not an optimisation: the editor's "Reset to Defaults"
    // applies unconditionally on every press and EngineSettings::ApplyTo runs on every "Load &
    // Apply", so without this guard each click would tear down and rebuild every back buffer.
    // Compared against the REQUESTED mode, not the effective one: if the display refused HDR,
    // m_hdrMode was demoted to SDR, and comparing against that would make every unrelated
    // SetPresentSettings call (VSync toggle, frame-cap change) retry the whole rebuild.
    if (mode == m_hdrModeRequested) return;
    m_hdrModeRequested = mode;

    // Outside a frame we can just do it; that is the common case at boot, where
    // it saves presenting one SDR frame before the switch takes effect.
    if (m_gfx.CanRebuildSwapChain())
    {
        ApplyHDROutputMode(mode);
        return;
    }

    // Inside a frame: latch. An open command list is already recording against the current
    // back buffer, and D3D12 command lists do NOT hold a reference to the resources they name:
    // releasing the back buffer here kills it for real, and the crash lands deep inside the
    // display driver on the next ImGui draw, nowhere near this call.
    {
        std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
        m_pendingSwapChainOp.hdrMode = mode;
    }
}

DXGI_FORMAT SurfaceDX12::SwapChainFormatFor(HDROutputMode mode)
{
    switch (mode)
    {
        case HDROutputMode::HDR10: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case HDROutputMode::scRGB: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case HDROutputMode::SDR:
        default:                   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// Returns false if the display refused the colour space for the CURRENT m_hdrMode. Never
// demotes m_hdrMode itself (see the declaration): a demotion here would leave m_hdrMode == SDR
// with a 10-bit / FP16 chain, which ApplyHDROutputMode's idempotence guard would then never
// correct.
bool SurfaceDX12::ApplySwapChainColorSpace()
{
    if (!m_swapChain) return true;

    DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    switch (m_hdrMode)
    {
        case HDROutputMode::HDR10: cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; break;
        case HDROutputMode::scRGB: cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;    break;
        case HDROutputMode::SDR:
        default:                   cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;    break;
    }

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -hdr_composition_test: a display that cannot show HDR still gets the HDR composition buffer and the
    // HDR present encode (10-bit PQ or FP16 scRGB), so a pack can read the signal back from the back buffer.
    static const bool s_compositionTest = Sys::CommandLine::Get().Has("-hdr_composition_test");
    const bool testPass = s_compositionTest && IsHDR(m_hdrMode);
#else
    const bool testPass = false;
#endif
    UINT support = 0;
    if (FAILED(m_swapChain->CheckColorSpaceSupport(cs, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        if (testPass)
        {
            LOG_INFO("[HDR_TEST] the display refuses the colour space for %s; the encode is presented to it anyway",
                     HDROutputModeName(m_hdrMode));
            return true;
        }
        LOG_WARNING("GraphicsDX12: swap chain rejected the colour space for %s. "
                    "Check that HDR is enabled for this display in Windows "
                    "Display Settings.", HDROutputModeName(m_hdrMode));
        return false;
    }

    if (FAILED(m_swapChain->SetColorSpace1(cs)))
    {
        if (testPass) return true;
        LOG_WARNING("GraphicsDX12: SetColorSpace1 failed for %s",
                    HDROutputModeName(m_hdrMode));
        return false;
    }

    ApplyHDRMetadata();
    return true;
}

// Static HDR metadata (SMPTE ST.2086 + MaxCLL/MaxFALL). Purely advisory: it tells the display
// the mastering volume we authored for, so its own tone mapper does not roll our highlights
// off a second time. Only meaningful for HDR10; scRGB is compositor-managed and SDR has none.
// Split out of ApplySwapChainColorSpace so the nit sliders can refresh it WITHOUT
// CheckColorSpaceSupport / SetColorSpace1. SetHDRMetaData touches no back buffers, so unlike
// everything else here it is safe from a mid-frame ImGui handler and needs no latch.
void SurfaceDX12::ApplyHDRMetadata()
{
    if (!m_swapChain) return;

    Microsoft::WRL::ComPtr<IDXGISwapChain4> sc4;
    if (SUCCEEDED(m_swapChain.As(&sc4)) && sc4)
    {
        if (m_hdrMode == HDROutputMode::HDR10)
        {
            // Rec.2020 (BT.2020) primaries + D65 white, in DXGI's 1/50000 units. These
            // MUST be Rec.2020 because the present shader converts Rec.709 -> Rec.2020
            // before encoding; declaring DCI-P3 here (an easy mistake: the numbers look
            // similar) tells the display our container is a smaller gamut and it re-maps
            // every saturated colour.
            //   R (0.708, 0.292)  G (0.170, 0.797)  B (0.131, 0.046)
            //   W (0.3127, 0.3290) = D65
            DXGI_HDR_METADATA_HDR10 md{};
            md.RedPrimary[0]   = 35400; md.RedPrimary[1]   = 14600;
            md.GreenPrimary[0] =  8500; md.GreenPrimary[1] = 39850;
            md.BluePrimary[0]  =  6550; md.BluePrimary[1]  =  2300;
            md.WhitePoint[0]   = 15635; md.WhitePoint[1]   = 16450;
            // Mastering luminance is in 1/10000-nit units; content light levels
            // are in plain nits. Different scales on adjacent fields, so a
            // copy-paste between them is silently 10000x wrong.
            md.MaxMasteringLuminance = static_cast<UINT>(m_peakNits * 10000.0f);
            md.MinMasteringLuminance = 0;
            md.MaxContentLightLevel      = static_cast<UINT16>(m_peakNits);
            md.MaxFrameAverageLightLevel = static_cast<UINT16>(m_paperWhiteNits);
            sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(md), &md);
        }
        else
        {
            sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
        }
    }
}

void SurfaceDX12::ResolveHDRLevels()
{
    const float prevPaperWhite = m_paperWhiteNits;
    const float prevPeak       = m_peakNits;
    const bool  follows        = !(m_paperWhiteRequested > 0.0f);
    float white = m_paperWhiteRequested;
    if (follows)
    {
        white = Display()->sdrWhiteNits;
#if WITH_HARNESS
        // -paperwhite_fixed: paper white ignores Windows and takes the fixed default (the reverse arm).
        static const bool s_fixed = Sys::CommandLine::Get().Has("-paperwhite_fixed");
        if (s_fixed) white = kHDRPaperWhiteDefault;
#endif
    }
    m_paperWhiteNits = std::clamp(white, kHDRPaperWhiteMin, kHDRPaperWhiteMax);
    // A peak below paper white is degenerate (the tone curve would have negative headroom); raise it
    // rather than reject, so a bad file still boots.
    m_peakNits = std::max(m_peakRequested, m_paperWhiteNits);

    if (m_paperWhiteNits != prevPaperWhite || follows != m_paperWhiteFollows)
        LOG_INFO("[DISPLAY] paper white %.0f nits (%s)", m_paperWhiteNits,
                 follows ? "follows Windows' SDR white level" : "the player's setting");
    m_paperWhiteFollows = follows;

    // The nits reach the GPU by themselves next frame (present-shader root constants +
    // ToneMapPass's hdrPeakRatio), but the display's STATIC HDR10 metadata is pushed only from
    // ApplySwapChainColorSpace, which runs on mode changes, resizes and fullscreen transitions -
    // none of which a nit slider triggers. Without this the panel keeps tone-mapping against
    // the old mastering volume while our roll-off follows the slider. Metadata-only, so safe
    // from the mid-frame call sites.
    if (IsHDR(m_hdrMode) && (m_paperWhiteNits != prevPaperWhite || m_peakNits != prevPeak))
        ApplyHDRMetadata();
}

// Returns false and leaves the chain on its previous format if DXGI refuses @p fmt.
bool SurfaceDX12::RebuildBackBuffers(DXGI_FORMAT fmt)
{
    // An HDR mode change lands here WITHOUT going through ResizeSwapChainAndRtv, so it needs
    // the same two things that path does:
    //   * Frame Generation off across the swap-chain rebuild.
    //   * The FG scratch targets dropped: they carry the COMPOSITION format, which changes
    //     with the HDR mode (RGBA8 <-> FP16), and keeping them would hand DLSS-G buffers whose
    //     format no longer matches what ApplyFrameGeneration declared.
    // SetPresentSettings resolves FG before it requests the mode, so FG sees a new mode only
    // through an ApplyFrameGeneration after ApplyHDROutputMode stores m_hdrMode: on the latched
    // path, the FGRestore in ServicePendingSwapChainOps. The Resume below is not that one: it
    // fires when this returns, before that store, so it gates scRGB and declares the composition
    // format for the OLD mode (only the SDR demotion rebuild runs after its store). After an
    // inline ApplyHDROutputMode nothing re-resolves until an unrelated ApplyFrameGeneration (a
    // resize, a fullscreen toggle, F11, the next SetPresentSettings), and with the frame count
    // unchanged (SDR <-> HDR10) ApplyFrameGeneration returns before redeclaring the format.
    // TODO: re-resolve FG once m_hdrMode is stored, redeclaring the composition format
    ReleaseFrameGenTextures();
    const bool fgWasOn = SuspendFrameGeneration();
    struct FGGuard
    {
        SurfaceDX12* self; bool wasOn;
        ~FGGuard() { self->ResumeFrameGeneration(wasOn); }
    } fgGuard{ this, fgWasOn };

    for (UINT n = 0; n < kFrameCount; ++n) m_renderTargets[n].Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(
        kFrameCount, m_width, m_height, fmt,
        m_gfx.m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

    const bool ok = SUCCEEDED(hr);
    if (!ok)
    {
        // Recover to the format we came from rather than leaving the swap chain with no
        // back buffers at all, which would brick the window.
        LOG_ERROR("GraphicsDX12: ResizeBuffers to format %d failed (hr=0x%08X); "
                  "staying on the previous format",
                  static_cast<int>(fmt), static_cast<unsigned>(hr));
        ThrowIfFailed(m_swapChain->ResizeBuffers(
            kFrameCount, m_width, m_height, m_swapChainFormat,
            m_gfx.m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    }
    else
    {
        m_swapChainFormat = fmt;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    ++m_swapChainGeneration;  // swap-chain (re)build - slot-parity re-base point
    AcquireBackBuffers();     // null RTV desc: the views take the new format

    // The present PSO bakes RTVFormats[0]. EnsurePresentPSO early-outs on a
    // non-null PSO, so without this reset the encode would keep rendering
    // against the OLD format for the rest of the session.
    m_presentPSO.Reset();
    m_presentRootSig.Reset();
    m_presentPSOFormat = DXGI_FORMAT_UNKNOWN;

    return ok;
}

void SurfaceDX12::ApplyHDROutputMode(HDROutputMode mode)
{
    // No swap chain yet: record BOTH halves so Create picks the right format and the invariant
    // "m_swapChainFormat == SwapChainFormatFor(m_hdrMode)" holds from construction on. Setting
    // only the mode would make the first frame render an HDR-encoded image into an 8-bit sRGB
    // chain.
    if (!m_swapChain)
    {
        m_hdrMode         = mode;
        m_swapChainFormat = SwapChainFormatFor(mode);
        return;
    }

    // Guard 1 - idempotence. Compares the FORMAT as well as the mode: after a demotion the two
    // can disagree (mode SDR, chain still 10-bit), and a mode-only guard would swallow the
    // request to go back to SDR and strand the window on the wrong format until restart.
    if (mode == m_hdrMode && m_swapChainFormat == SwapChainFormatFor(mode)) return;

    // Guard 2 - frame boundary. Same contract as Resize / SetFullscreen: an open command list
    // references the back buffers we are about to release.
    if (!ENGINE_CHECK_ONCE(m_gfx.CanRebuildSwapChain(),
            "ApplyHDROutputMode called inside a frame ??refused. Route through "
            "RequestHDROutputMode(), which latches to the next frame boundary."))
        return;

    // Guard 3 - re-entrancy. SetFullscreenState delivers WM_SIZE synchronously and Resize()
    // can re-enter mid-rebuild, with the targets already Reset() but GetBuffer not yet issued.
    // Shared with Resize / SetFullscreen.
    if (!ClaimSwapChainOp()) return;
    struct OpGuard
    {
        SurfaceDX12* self;
        ~OpGuard() { self->ReleaseSwapChainOp(); }
    } opGuard{ this };

    // Full drain: every queue must be done with the back buffers before they go.
    m_gfx.WaitForPreviousFrame();

    if (RebuildBackBuffers(SwapChainFormatFor(mode)))
        m_hdrMode = mode;

    // If the display refuses the colour space, demote as a REAL transition (rebuild the back
    // buffers at 8-bit too) so m_hdrMode and m_swapChainFormat never disagree. Writing only
    // m_hdrMode would leave an HDR-format chain claiming to be SDR, which guard 1 would then
    // refuse to correct.
    if (!ApplySwapChainColorSpace() && IsHDR(m_hdrMode))
    {
        LOG_WARNING("GraphicsDX12: falling back to SDR (the %s preference is kept "
                    "and retried if the display becomes HDR-capable)",
                    HDROutputModeName(mode));
        m_hdrMode = HDROutputMode::SDR;
        RebuildBackBuffers(SwapChainFormatFor(HDROutputMode::SDR));
        // sRGB on an 8-bit flip chain is universally supported; if even this
        // fails something far more fundamental is wrong and the warning inside
        // says so.
        ApplySwapChainColorSpace();
    }

    RefreshDisplayHDRInfo();

    LOG_SUCCESS("GraphicsDX12: output mode = %s (swap chain %s)",
                HDROutputModeName(m_hdrMode),
                m_swapChainFormat == DXGI_FORMAT_R10G10B10A2_UNORM  ? "R10G10B10A2_UNORM"  :
                m_swapChainFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? "R16G16B16A16_FLOAT" :
                                                                      "R8G8B8A8_UNORM");
}

// Returns false when another rebuild already owns the swap chain: the WM_SIZE echo case,
// which carries no information (the outer op finishes at the correct state).
bool SurfaceDX12::ClaimSwapChainOp()
{
    std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
    if (m_swapChainOpActive.load(std::memory_order_relaxed)) return false;
    m_swapChainOpActive.store(true, std::memory_order_release);
    return true;
}

void SurfaceDX12::ReleaseSwapChainOp()
{
    std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
    m_swapChainOpActive.store(false, std::memory_order_release);
}

void SurfaceDX12::ServicePendingSwapChainOps()
{
    // The display is the main thread's to look at (the lane only ever calls this as BeginFrame's backstop).
    if (std::this_thread::get_id() == m_mainThread)
        ServiceDisplay();
    {
        std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
        if (!m_pendingSwapChainOp.Pending()) return;
    }
    // Only actual device operations need to drain RenderMain. Never hold the
    // request mutex across the park: RenderMain may itself inspect the latch.
    m_gfx.DrainFrameIfNeeded(Diag::DrainCause::Swapchain);

    // Never service from inside a frame: that defeats the point of latching. Keep the request
    // queued; the next boundary takes it.
    if (!ENGINE_CHECK_ONCE(!m_gfx.m_inFrame,
            "ServicePendingSwapChainOps() called between BeginFrame and EndFrame ??refused, "
            "request stays queued. Call it from App::Run's loop top, next to "
            "Window::ServicePendingFullscreen()."))
        return;

    PendingSwapChainOp op;
    {
        std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
        // BeginFrame may have consumed it while the caller was parking.
        if (!m_pendingSwapChainOp.Pending()) return;
        op = m_pendingSwapChainOp;
        // Consume BEFORE applying: the transition re-enters through WM_SIZE and would otherwise
        // re-queue the op being serviced.
        m_pendingSwapChainOp = {};
    }

    // Frame Generation OFF for the duration. Streamline owns the swap chain through its proxy
    // and keeps interpolation state sized to it, and DLSS-G requires the host to disable FG
    // before changing resolution or fullscreen state, which every path below does. Restored
    // through ApplyFrameGeneration, which re-evaluates the gates, so an HDR change to scRGB
    // leaves FG off rather than blindly turning it back on.
    const bool fgWasOn = SuspendFrameGeneration();
    struct FGRestore
    {
        SurfaceDX12* self; bool wasOn;
        ~FGRestore() { self->ResumeFrameGeneration(wasOn); }
    } fgRestore{ this, fgWasOn };   // fires on the early return below too

    // HDR FIRST, deliberately BEFORE the fullscreen early return: after that branch, a mode
    // change queued together with a fullscreen toggle would be silently swallowed (no crash, no
    // diagnostic, the setting just does not stick). The order matters the other way too: the
    // fullscreen path's own ResizeBuffers passes DXGI_FORMAT_UNKNOWN, so it keeps whatever
    // format is installed here.
    if (op.hdrMode.has_value())
        ApplyHDROutputMode(*op.hdrMode);

    if (op.fullscreen.has_value())
    {
        // Apply*Now, NOT the public entry points: those latch, so calling them here would
        // re-queue the op instead of applying it, and the window would never change -
        // silently, and forever.
        ApplyFullscreenNow(*op.fullscreen);
        // A fullscreen transition sizes the swap chain itself (output size going
        // in, client-rect size coming out) and DXGI has already delivered the
        // matching WM_SIZE. Applying a resize latched BEFORE the transition
        // would immediately fight it back to the stale window size.
        return;
    }

    if (op.haveResize)
        ApplyResizeNow(op.width, op.height);
}

// PUBLIC entry point: latches; ServicePendingSwapChainOps applies it at a frame boundary.
// Called straight from the WM_SIZE handler (Window.cpp) on the thread that pumps messages.
// Rebuilding the swap chain there would Reset() back buffers the render thread's open
// command lists have already recorded barriers against, and D3D12 command lists hold no
// refcount, so the buffer really dies under the driver.
// GUARD ORDER IS LOAD-BEARING: idempotence, then re-entrancy, then latch. Latching first
// would let SetFullscreenState's synchronous WM_SIZE echo re-latch the op being applied.
void SurfaceDX12::Resize(uint32_t width, uint32_t height)
{
    if (!width || !height) return;

    std::unique_lock<std::mutex> lk2(m_swapChainOpMutex);

    // IDEMPOTENCE against the MERGED PENDING state, not m_width / m_height: m_width changes
    // only on APPLY, so while an op is latched it still holds the pre-latch size.
    // Several WM_SIZE with no frame between them are normal while dragging the frame: the
    // size/move modal loop runs inside one DispatchMessage in Plat::Window::Pump. Comparing a
    // later size with the stale m_width would discard one that returns to the old size, the
    // service would apply the earlier one, and the swap chain would stay the wrong size with
    // nothing to point at.
    const uint32_t effW = m_pendingSwapChainOp.haveResize
                        ? m_pendingSwapChainOp.width  : m_width;
    const uint32_t effH = m_pendingSwapChainOp.haveResize
                        ? m_pendingSwapChainOp.height : m_height;
    if (width == effW && height == effH) return;

    // RE-ENTRANCY. SetFullscreenState delivers WM_SIZE synchronously, so this
    // re-enters while the outer transition has the back buffers half rebuilt.
    // The echo carries no new information - the outer op finishes at the right
    // size - so it is dropped rather than queued.
    if (m_swapChainOpActive.load(std::memory_order_acquire)) return;

    m_pendingSwapChainOp.haveResize = true;
    m_pendingSwapChainOp.width      = width;
    m_pendingSwapChainOp.height     = height;
}

// PRIVATE apply. Carries its own idempotence and re-entrancy guards: a request latched a
// frame ago may have become a no-op since, and an unguarded rebuild for it costs
// WaitForPreviousFrame plus every back buffer destroyed - fatal (see ApplyFullscreenNow),
// not merely wasteful.
void SurfaceDX12::ApplyResizeNow(uint32_t width, uint32_t height)
{
    if (!width || !height) return;
    if (width == m_width && height == m_height) return;
    if (!ENGINE_CHECK_ONCE(m_gfx.CanRebuildSwapChain(),
            "ApplyResizeNow called inside a frame - refused. The open primary "
            "command list has recorded a barrier and OMSetRenderTargets against "
            "the back buffers this is about to Reset()."))
        return;

    // Claim the rebuild under the lock, then RELEASE IT for the rebuild itself.
    //
    // Holding it across the rebuild would deadlock: ResizeBuffers and
    // SetFullscreenState deliver WM_SIZE synchronously on this same thread, and
    // the handler calls straight back into Resize(), which takes this mutex.
    // std::mutex is not recursive. Releasing it and leaving the active flag set
    // gives the re-entrant call exactly the answer it should get - "an op owns
    // the swap chain, drop the echo" - without blocking.
    if (!ClaimSwapChainOp()) return;
    struct OpGuard
    {
        SurfaceDX12* self;
        ~OpGuard() { self->ReleaseSwapChainOp(); }
    } opGuard{ this };

    m_gfx.WaitForPreviousFrame();
    ResizeSwapChainAndRtv(width, height);
}

// PUBLIC entry point: latches, for the same reason as Resize (its one caller, Plat::Window::SetFullscreen, runs on the thread that
// pumps messages, not the render thread). The latch's std::optional keeps "no request" and
// "requested windowed" apart: an unconditional SetFullscreen(false) on an already-windowed
// chain would rebuild every back buffer under an open command list.
void SurfaceDX12::SetFullscreen(bool fullscreen)
{
    std::unique_lock<std::mutex> lk2(m_swapChainOpMutex);

    // IDEMPOTENCE, against the merged pending state - see Resize for why the
    // live member is the wrong thing to compare with while an op is latched.
    const bool effFull = m_pendingSwapChainOp.fullscreen.has_value()
                       ? *m_pendingSwapChainOp.fullscreen : m_isFullscreen;
    if (fullscreen == effFull) return;

    if (m_swapChainOpActive.load(std::memory_order_acquire)) return;      // re-entrancy; see Resize

    m_pendingSwapChainOp.fullscreen = fullscreen;
}

// PRIVATE apply. Both branches destroy and rebuild every back buffer, so running it for a
// request that has since become a no-op is fatal, not merely wasteful: it Reset()s render
// targets an open command list has already recorded a PRESENT->RENDER_TARGET barrier and
// OMSetRenderTargets against, and D3D12 command lists hold no refcount - the back buffer
// dies while the driver's residency list still points at it (an access violation inside
// the driver during the next ImGui draw). Hence the guards are repeated here, not left to
// the caller.
void SurfaceDX12::ApplyFullscreenNow(bool fullscreen)
{
    if (fullscreen == m_isFullscreen) return;
    if (!ENGINE_CHECK_ONCE(m_gfx.CanRebuildSwapChain(),
            "ApplyFullscreenNow called inside a frame - refused."))
        return;

    // See ApplyResizeNow: claimed under the lock, rebuilt without it, because
    // SetFullscreenState's synchronous WM_SIZE re-enters Resize() on this thread.
    if (!ClaimSwapChainOp()) return;
    struct OpGuard
    {
        SurfaceDX12* self;
        ~OpGuard() { self->ReleaseSwapChainOp(); }
    } opGuard{ this };

    // Frame Generation OFF across the transition, released HERE and not only in
    // ServicePendingSwapChainOps: this function owns the transition, so the guard travels with it
    // wherever it is called from. DLSS-G requires FG disabled before a fullscreen toggle; without it the next
    // Present fails and ThrowIfFailed takes the engine down.
    // Nested inside ServicePendingSwapChainOps, FG is already off unless a latched HDR change in
    // the same op turned it back on (RebuildBackBuffers' guard) and this Suspend turns it off
    // again, so FG cycles twice there. The Resume re-resolves either way (ResumeFrameGeneration
    // ignores wasOn), so FG can come back on here rather than in the outer FGRestore. Every
    // rebuild still runs with FG off.
    const bool fgWasOn = SuspendFrameGeneration();
    struct FGGuard
    {
        SurfaceDX12* self; bool wasOn;
        ~FGGuard() { self->ResumeFrameGeneration(wasOn); }
    } fgGuard{ this, fgWasOn };

    if (fullscreen)
    {
        // BORDERLESS fullscreen, NOT DXGI-exclusive: a tearing-capable swap chain
        // (ALLOW_TEARING, fixed at creation) fails every Present in exclusive mode with
        // DXGI_ERROR_INVALID_CALL, and borderless keeps VRR and Frame Generation working.
        // The WINDOW makes itself borderless and covers its monitor (Plat::Window::SetFullscreen,
        // which saves and restores its own placement) before this op is latched; the device
        // only fits the swap chain to the client it got.
        RECT cr{};
        GetClientRect(m_hWnd, &cr);
        UINT w = static_cast<UINT>(cr.right - cr.left);
        UINT h = static_cast<UINT>(cr.bottom - cr.top);
        if (w == 0 || h == 0)
        {
            // Minimized or not yet shown: no client to fit, so use the output the
            // swap chain is on.
            ComPtr<IDXGIOutput> output;
            ThrowIfFailed(m_swapChain->GetContainingOutput(&output));
            DXGI_OUTPUT_DESC od; ThrowIfFailed(output->GetDesc(&od));
            w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
        }
        m_gfx.WaitForPreviousFrame();
        for (UINT n = 0; n < kFrameCount; ++n) m_renderTargets[n].Reset();
        ThrowIfFailed(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
            m_gfx.m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        ++m_swapChainGeneration;  // swap-chain (re)build - slot-parity re-base point
        AcquireBackBuffers();
        m_width = w; m_height = h;
        m_viewport    = { 0,0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
        m_scissorRect = { 0,0, static_cast<LONG>(w),  static_cast<LONG>(h) };
        m_gfx.CreateHdrTarget(0, w, h);
        // Both ResizeBuffers AND SetFullscreenState reset the colour space to the sRGB
        // default, so entering fullscreen would otherwise drop an HDR session back to SDR
        // interpretation (the windowed branch gets this via ResizeSwapChainAndRtv). A refusal
        // latches a real demotion for the next boundary, as in ResizeSwapChainAndRtv.
        if (!ApplySwapChainColorSpace() && IsHDR(m_hdrMode))
            {
                std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
                m_pendingSwapChainOp.hdrMode = HDROutputMode::SDR;
            }
        RefreshDisplayInfo();   // fullscreen mode may change the active refresh
        m_isFullscreen = true;
    }
    else
    {
        m_gfx.WaitForPreviousFrame();
        // A driver-initiated transition may have left the chain genuinely exclusive; this is
        // a no-op when the chain is already windowed (the normal case: fullscreen is
        // borderless).
        ThrowIfFailed(m_swapChain->SetFullscreenState(FALSE, nullptr));

        // The window has already put its frame and its saved placement back
        // (Plat::Window::SetFullscreen); fit the swap chain to that client.
        RECT r; if (GetClientRect(m_hWnd, &r))
        {
            UINT w = r.right - r.left, h = r.bottom - r.top;
            if (w && h) ResizeSwapChainAndRtv(w, h);   // re-asserts the colour space
        }
        // Unconditional, because the ResizeSwapChainAndRtv above is conditional:
        // a zero-sized client rect (minimised) would otherwise leave the swap
        // chain on sRGB after SetFullscreenState reset it. Idempotent.
        if (!ApplySwapChainColorSpace() && IsHDR(m_hdrMode))
            {
                std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
                m_pendingSwapChainOp.hdrMode = HDROutputMode::SDR;
            }
        RefreshDisplayInfo();   // back to the desktop refresh
        m_isFullscreen = false;
    }

    // A mode transition sets the swap-chain size AUTHORITATIVELY (output size going in, client
    // rect coming out), so any resize still in the latch is stale by definition. Dropping it
    // matters because Window::ServicePendingFullscreen runs BEFORE
    // ServicePendingSwapChainOps at App::Run's loop top: a WM_SIZE latched during an earlier
    // frame would otherwise be replayed after the transition and drag the swap chain back to
    // the pre-transition size. The fullscreen field is left alone: this function consumed it
    // only when routed through the latch, and clearing someone else's request would lose it.
    {
        std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
        m_pendingSwapChainOp.haveResize = false;
    }
}

// ==== Present composition buffer: the editor's ImGui target ====
// WHY THIS EXISTS. In an editor build ImGui does not draw the scene through
// CompositeTextureToSwapChain: it draws it as an ImGui::Image in its own draw list and renders
// the whole UI straight to the back buffer, with no display-encode hook. With a PQ back buffer
// every ImGui vertex colour and glyph would be read as a PQ code value (mid-grey 0.5 -> ~90
// nits, highlights crushed), and likewise for scRGB.
// Re-initialising the ImGui DX12 backend with the new RTV format on an HDR toggle is a trap:
// ImGui bakes RTVFormat into its PSO at init with no setter, so it needs a full Shutdown/Init,
// which frees every backend SRV descriptor, including the ones this frame's ImGui::Image /
// AddImage calls already captured (a use-after-free).
// So ImGui ALWAYS renders into this FP16 buffer, in SDR too, and its RTV format is a
// compile-time constant. One extra fullscreen blit buys live HDR switching in the editor with
// no ImGui re-initialisation. In SDR the encode is a passthrough and values in [0,1]
// round-trip exactly through FP16, so the SDR editor image is unchanged.
void SurfaceDX12::EnsurePresentComposition(UINT width, UINT height,
                                            const float clearColor[4])
{
    if (width == 0 || height == 0) return;
    // THE CLEAR COLOUR IS PART OF THE IDENTITY, not just the size (see the declaration).
    // Recreating on a colour change takes the same deferred-release path a resize does, so it
    // is safe mid-frame.
    const bool sameClear =
        clearColor == nullptr ||
        (m_presentCompClear[0] == clearColor[0] && m_presentCompClear[1] == clearColor[1] &&
         m_presentCompClear[2] == clearColor[2] && m_presentCompClear[3] == clearColor[3]);
    if (m_presentComposition && m_presentCompW == width && m_presentCompH == height &&
        sameClear)
        return;
    if (clearColor)
        for (int i = 0; i < 4; ++i) m_presentCompClear[i] = clearColor[i];

    // Defer, exactly as DestroyHdrTarget does: frames still in flight may
    // be reading the old buffer AND its descriptor slots, and freeing a slot
    // early lets it be handed to something else while the GPU still reads it.
    if (m_presentComposition)
    {
        std::lock_guard<std::mutex> lock(m_gfx.m_deferredMutex);
        auto& dr = m_gfx.PendingReleaseBucket();
        if (m_presentCompRtv.IsValid()) { dr.descriptors.push_back(m_presentCompRtv); m_presentCompRtv = {}; }
        if (m_presentCompSrv.IsValid()) { dr.descriptors.push_back(m_presentCompSrv); m_presentCompSrv = {}; }
        dr.resources.push_back(std::move(m_presentComposition));
        m_presentComposition.Reset();
    }

    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC   td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;  td.Height = height;
    td.DepthOrArraySize = 1;      td.MipLevels = 1;
    td.Format           = kPresentCompositionFormat;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{ kPresentCompositionFormat,
                          { m_presentCompClear[0], m_presentCompClear[1],
                            m_presentCompClear[2], m_presentCompClear[3] } };
    ThrowIfFailed(m_gfx.m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_presentComposition)));
    GpuLedger::TrackTexture(m_gfx.m_device.Get(), m_presentComposition.Get(), "Surface.PresentComposition");
    m_presentComposition->SetName(L"GraphicsDX12.PresentComposition");

    m_presentCompRtv = m_gfx.m_rtvAllocator.Allocate(1,
                                             "GraphicsDX12.PresentComposition");
    D3D12_RENDER_TARGET_VIEW_DESC rtvd{ kPresentCompositionFormat, D3D12_RTV_DIMENSION_TEXTURE2D };
    m_gfx.m_device->CreateRenderTargetView(m_presentComposition.Get(), &rtvd,
                                     m_presentCompRtv.GetCpuHandle());

    m_presentCompSrv = m_gfx.m_cbvSrvUavAllocator.Allocate(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = kPresentCompositionFormat;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_gfx.m_cbvSrvUavAllocator.WriteSrv(m_presentComposition.Get(), &srvd,
                                        m_presentCompSrv.GetCpuHandle());
    m_presentCompSrv.CopyToGpu();

    m_presentCompState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_presentCompW = width; m_presentCompH = height;
    LOG_INFO("GraphicsDX12: present composition buffer %ux%u (R16G16B16A16_FLOAT)", width, height);
}

void SurfaceDX12::SetRenderTargetToPresentComposition(const float clearColor[4],
                                                       RHI::CommandList cmd)
{
    EnsurePresentComposition(m_width, m_height, clearColor);
    if (!m_presentComposition) return;

    auto* cl = m_gfx.GetPoolEntry(cmd).GetCommandList();

    // Same HDR-scene hand-off SetRenderTargetToSwapChain performs: the editor
    // is about to sample the scene texture through ImGui, so it must not still
    // be bound as a render target.
    if (TextureDX12* hdr = m_gfx.HdrRecord();
        hdr && hdr->state == D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            hdr->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b);
        m_gfx.JournalNativeBarriers(cmd, &b, 1);   // barrier journal
        hdr->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }

    if (m_presentCompState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_presentComposition.Get(), m_presentCompState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_gfx.JournalNativeBarriers(cmd, &b, 1);
        m_presentCompState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    cl->RSSetViewports(1, &m_viewport);
    cl->RSSetScissorRects(1, &m_scissorRect);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_presentCompRtv.GetCpuHandle();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // clearColor is authored in the composition space (gamma-encoded, 1.0 == paper white),
    // which is what App's kClearColor already is, so unlike a raw back-buffer clear it needs no
    // per-mode adjustment.
    cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
}

void SurfaceDX12::ResolvePresentCompositionToSwapChain(RHI::CommandList cmd)
{
    if (!m_presentComposition) return;
    auto* cl = m_gfx.GetPoolEntry(cmd).GetCommandList();

    if (m_presentCompState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_presentComposition.Get(), m_presentCompState,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b);
        m_gfx.JournalNativeBarriers(cmd, &b, 1);   // barrier journal
        m_presentCompState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }

    // Bind the back buffer WITHOUT clearing: the fullscreen triangle below
    // covers every pixel, so a clear would be pure bandwidth.
    cl->RSSetViewports(1, &m_viewport);
    cl->RSSetScissorRects(1, &m_scissorRect);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_gfx.m_rtvDescriptorSize;
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    CompositeTextureToSwapChain(m_presentCompSrv.GetGpuHandle().ptr, cmd);
}

void SurfaceDX12::SetRenderTargetToSwapChain(const float clearColor[4], RHI::CommandList cmd)
{
    auto* cl = m_gfx.GetPoolEntry(cmd).GetCommandList();
    if (TextureDX12* hdr = m_gfx.HdrRecord();
        hdr && hdr->state == D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            hdr->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b);
        m_gfx.JournalNativeBarriers(cmd, &b, 1);   // barrier journal
        hdr->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    cl->RSSetViewports(1, &m_viewport);
    cl->RSSetScissorRects(1, &m_scissorRect);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_gfx.m_rtvDescriptorSize;
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
}

// Grouped present/pacing setter. RefreshDisplayInfo() is a REQUIRED side
// effect, not a courtesy: it recomputes the VRR refresh ceiling that
// EffectiveFrameCap() applies on top of targetFrameRate.
void SurfaceDX12::SetPresentSettings(const RHI::PresentSettings& ps)
{
    targetFrameRate = ps.frameRateCap;
    vsyncEnabled    = ps.vsync;
    gsyncEnabled    = ps.gsync;
    renderScale     = ps.renderScale;

    // Reflex and Frame Generation are stored as REQUESTS and resolved together: FG cannot run
    // without Reflex, so the effective Reflex mode depends on the FG request. Both are scalar
    // stores plus at most a Streamline call, safe from any call site, unlike hdrMode below.
    m_reflexModeRequested = ps.reflexMode;
    m_frameGenRequested   = ps.frameGenFrames;
    ApplyFrameGeneration();

    // The nits are plain scalars, safe to store from anywhere; they reach the GPU as
    // present-shader root constants next frame with no rebuild. Sanitised here rather than
    // trusting callers: a NaN from a hand-edited settings.json propagates into the PQ encode and
    // blanks the screen.
    // The colour-vision assist: a NaN severity from a hand-edited settings.json reads as full.
    m_colorVision.store((ps.colorVision >= 0 && ps.colorVision <= 3) ? ps.colorVision : 0,
                        std::memory_order_relaxed);
    m_colorVisionSeverity.store(ps.colorVisionSeverity == ps.colorVisionSeverity
                                    ? std::clamp(ps.colorVisionSeverity, 0.0f, 1.0f) : 1.0f,
                                std::memory_order_relaxed);

    // A paper white of 0, NaN or below zero follows Windows (ResolveHDRLevels).
    m_paperWhiteRequested = (ps.paperWhiteNits > 0.0f)
                          ? std::clamp(ps.paperWhiteNits, kHDRPaperWhiteMin, kHDRPaperWhiteMax)
                          : 0.0f;
    m_peakRequested       = (ps.peakNits > 0.0f && ps.peakNits == ps.peakNits)
                          ? std::clamp(ps.peakNits, kHDRPeakNitsMin, kHDRPeakNitsMax)
                          : kHDRPeakNitsDefault;
    ResolveHDRLevels();

    // hdrMode is NOT a scalar store: it rebuilds the swap chain. The callers (the editor's
    // Graphics Settings and post-process panels, EngineSettings::ApplyTo) run mid-frame inside
    // an ImGui handler, so this MUST go through the latch. RequestHDROutputMode also carries the
    // idempotence guard, so callers passing an unchanged mode cost nothing.
    RequestHDROutputMode(ps.hdrMode);

    RefreshDisplayInfo();
}

// ==== The device's frame, through the surface ====
// GraphicsDX12::BeginFrame opens the primary list and hands it here; EndFrame closes the
// back buffer on it, submits every list, then calls Present.
void SurfaceDX12::BeginFrameRecording(ID3D12GraphicsCommandList* cmd)
{
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);

    // Transition the swap-chain back buffer PRESENT -> RENDER_TARGET.
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);
    GfxDiag::NoteNativeBarriers(cmd, &b, 1);   // barrier journal (the primary list)
}

void SurfaceDX12::EndFrameRecording(ID3D12GraphicsCommandList* primaryCmd)
{
    // Transition the swap-chain back buffer RENDER_TARGET -> PRESENT on the primary CL.
    // A pending -ui_screenshot records RT -> COPY_SOURCE, the copy and COPY_SOURCE -> PRESENT
    // in its place.
    if (!RecordBackBufferCapture(primaryCmd, m_renderTargets[m_frameIndex].Get()))
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargets[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        primaryCmd->ResourceBarrier(1, &b);
        GfxDiag::NoteNativeBarriers(primaryCmd, &b, 1);   // barrier journal
    }
}

void SurfaceDX12::Present()
{
    // Present: VSync on -> SyncInterval = 1; VSync off -> 0 + ALLOW_TEARING.
    auto logRemovedReason = [&](HRESULT hr)
    {
        // State the Present configuration on EVERY failure, not just device removal. A
        // Present failing while Frame Generation owns the swap chain is a different
        // investigation from one failing on its own, and without this the ThrowIfFailed that
        // follows reports only an HRESULT, while the crash surfaces later inside the
        // destructor's slShutdown, pointing at the wrong place.
        LOG_ERROR("GraphicsDX12: Present failed hr=0x%08X (fullscreen=%d, FrameGen=%d, "
                  "vsync=%d, gsync=%d, tearing=%d)",
                  static_cast<unsigned>(hr), m_isFullscreen ? 1 : 0, m_frameGenActive,
                  vsyncEnabled ? 1 : 0, gsyncEnabled ? 1 : 0, m_gfx.m_tearingSupported ? 1 : 0);

        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_HUNG &&
            hr != DXGI_ERROR_DEVICE_RESET) return;
        m_gfx.ReportDeviceRemoved("Present");
    };

    // Present-mode selection.
    //  * ALLOW_TEARING is ILLEGAL in exclusive fullscreen (SetFullscreenState TRUE):
    //    DXGI_ERROR_INVALID_CALL, so gate it on the live fullscreen state, not just
    //    m_tearingSupported.
    //  * G-Sync / VRR must NOT hard-sync to vblank: the display follows the GPU, paced by
    //    LimitFrameRate. It needs a tearing-legal present AND a known refresh (so
    //    LimitFrameRate has a ceiling); otherwise VRR cannot engage.
    //  * When a G-Sync request cannot be honoured (fullscreen / no tearing / no known
    //    refresh), fall back to hard VSync rather than uncapped tearing.
    BOOL fsState = FALSE;
    {
        ZoneScopedN("EndFrame.FullscreenState");
        m_swapChain->GetFullscreenState(&fsState, nullptr);
    }
    const bool canTear   = m_gfx.m_tearingSupported && !fsState;
    const bool vrrPresent = gsyncEnabled && canTear && Display()->refresh.Known();

    // PCL Present markers must bracket the REAL Present call and nothing else:
    // once Frame Generation is on, this is the call Streamline takes over, and
    // its pacer measures the span to decide when to slot interpolated frames in.
    // All three branches below present exactly once, so one pair around the
    // whole if/else is correct and cannot double-fire.
    auto& slBackend = SL::StreamlineBackend::Get();
    void* const slFrame = slBackend.RenderFrameToken();

    // THE TOKEN ORACLE. sl::FrameToken's only public member is operator uint32_t()
    // (sl_core_types.h), so a token reports the frame it was minted for. That turns "did this
    // half use the right frame's token" into an assertion: a token filed under the wrong frame
    // gets no error from SL, only worse latency later.
    // Serial runs see equal indices; a render half one frame behind sees exactly one lower. The
    // check forbids the render half being AHEAD (it read the game latch by mistake) or more than
    // one behind (the publish at the seam was missed or stale). EndFrame runs once per frame on
    // the render half in every configuration, so every pack checks this through its
    // "[SPLITCHECK][" ban.
    {
        const uint32_t gi = SL::StreamlineBackend::TokenFrameIndex(slBackend.GameFrameToken());
        const uint32_t ri = SL::StreamlineBackend::TokenFrameIndex(slFrame);
        if (gi != UINT32_MAX && ri != UINT32_MAX)
        {
            ENGINE_CHECK_ONCE(ri <= gi && (gi - ri) <= 1,
                "[SPLITCHECK][SLTOKEN] render half is on Streamline frame %u while "
                "the game half is on %u — the render half must be on the game's "
                "frame or exactly one behind, never ahead and never further back",
                ri, gi);
        }
    }

    slBackend.SetLatencyMarker(SL::LatencyMarker::PresentStart, slFrame);

    // The back buffer must not have moved between BeginFrame and Present: this frame's
    // render-target pick and present barriers used m_frameIndex.
    ENGINE_CHECK_ONCE(m_frameIndex == m_swapChain->GetCurrentBackBufferIndex(),
        "[SPLITCHECK][SLOTPARITY] back-buffer index moved between BeginFrame (%u) "
        "and Present (%u)",
        m_frameIndex, m_swapChain->GetCurrentBackBufferIndex());

    {
    ZoneScopedN("EndFrame.Present");
    // Present blocks while the flip queue is full - the GPU (or the display)
    // is behind. Classified as a GPU wait; the driver's own CPU work inside it
    // is small next to that block and shows up in the OS CPU column.
    PROF_WAIT(WaitGpu, "EndFrame.Present");
    if (vrrPresent)
    {
        HRESULT hr = m_swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        if (FAILED(hr)) logRemovedReason(hr);
        NotePresentResult(hr);
        ThrowIfFailed(hr);
    }
    else if (vsyncEnabled || gsyncEnabled)   // VSync, or a VRR request we can't satisfy
    {
        HRESULT hr = m_swapChain->Present(1, 0);
        if (FAILED(hr)) logRemovedReason(hr);
        NotePresentResult(hr);
        ThrowIfFailed(hr);
    }
    else                                     // fully uncapped
    {
        UINT flags = canTear ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = m_swapChain->Present(0, flags);
        if (FAILED(hr)) logRemovedReason(hr);
        NotePresentResult(hr);
        ThrowIfFailed(hr);
    }

    }
    // Counted HERE, after the Present that did not throw - not at the top of
    // EndFrame and not in the game loop. "A frame was presented" is a fact only
    // this line can state.
    m_presentedFrames.fetch_add(1, std::memory_order_relaxed);
    FinishBackBufferCapture();   // -ui_screenshot's PNG (no-op otherwise)

    slBackend.SetLatencyMarker(SL::LatencyMarker::PresentEnd, slFrame);

    // ---- Frame Generation health ----
    // MEASURED, not assumed: slDLSSGSetOptions succeeding only means the request was accepted.
    // FG can report eDLSSGStatusOk and still generate nothing (a mandated input missing, Reflex
    // not detected at runtime, a format refused), and frame rate alone does not tell "FG off"
    // from "FG failing". The honest test is frames PRESENTED vs frames RENDERED over a window:
    // the SDK's counter resets on every query, so a per-frame poll always reads ~1.
    if (m_frameGenActive > 0)
    {
        // Stamp the window's start on the first frame after FG comes up. Without it the
        // first window has no start time and reports 0.0 FPS, which reads as "the frame
        // rate is the problem" when it is only an uninitialised measurement.
        if (m_fgWindowStartQpc == 0)
        {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            m_fgWindowStartQpc  = q.QuadPart;
            m_fgFramesSincePoll = 0;
        }
        if (++m_fgFramesSincePoll >= kFGPollWindow)
        {
            // Measure the window's real frame rate too. "DLSS-G self-disables below
            // ~10 FPS" is useless without the actual rate: that one number decides whether
            // the integration is suspect or the scene is simply too slow to interpolate.
            LARGE_INTEGER nowQpc, freq;
            QueryPerformanceCounter(&nowQpc);
            QueryPerformanceFrequency(&freq);
            float windowFps = 0.0f;
            if (m_fgWindowStartQpc != 0 && nowQpc.QuadPart > m_fgWindowStartQpc)
                windowFps = static_cast<float>(m_fgFramesSincePoll) * static_cast<float>(freq.QuadPart)
                          / static_cast<float>(nowQpc.QuadPart - m_fgWindowStartQpc);
            m_fgWindowStartQpc = nowQpc.QuadPart;

            // Independent of Streamline entirely: DXGI's own present counter.
            // If FG is interpolating, this climbs about twice as fast as our
            // rendered-frame count. Nothing in the engine resets it, so it
            // cannot be drained by a second reader the way DLSSGState's can.
            uint32_t dxgiPresentDelta = 0;
            bool     haveDxgiBaseline = false;
            {
                DXGI_FRAME_STATISTICS fs{};
                if (SUCCEEDED(m_swapChain->GetFrameStatistics(&fs)))
                {
                    if (m_fgLastPresentCount != 0 && fs.PresentCount >= m_fgLastPresentCount)
                    {
                        dxgiPresentDelta = fs.PresentCount - m_fgLastPresentCount;
                        haveDxgiBaseline = true;
                    }
                    m_fgLastPresentCount = fs.PresentCount;
                }
            }

            // The FIRST window has no previous PresentCount to subtract from, so its delta is
            // not a measurement, and "0.00x, not generating" from it would be confidently
            // wrong. Skip the verdict, but do not return early: m_fgFramesSincePoll must
            // still reset below.
            SL::FrameGenState fg{};
            if (haveDxgiBaseline && slBackend.GetFrameGenState(fg))
            {
                m_fgSnapshot = fg;   // publish before anyone else can query
                const float ratio = static_cast<float>(fg.framesPresentedSincePoll)
                                  / static_cast<float>(m_fgFramesSincePoll);
                m_fgLastRatio = ratio;
                const float dxgiRatio = static_cast<float>(dxgiPresentDelta)
                                      / static_cast<float>(m_fgFramesSincePoll);
                // NO VERDICT IS DRAWN FROM THESE NUMBERS: both counters are unreliable as an
                // "is FG interpolating" test.
                //   * DLSSGState::framesPresentedSincePoll resets on every read, so a second
                //     reader drains it; even as sole reader it reports 1-2 over hundreds of
                //     frames.
                //   * DXGI PresentCount is erratic under VRR + ALLOW_TEARING (consecutive
                //     windows of one steady 177 FPS scene read 4.99x, then 0.43x).
                // Print the numbers as a hint; the CONCLUSION needs an external tool
                // (PresentMon, the NVIDIA overlay, or the development SL build's on-screen FG
                // indicator).
                const int statusNow = static_cast<int>(fg.statusOk);
                if (statusNow != m_fgLastStatusOk)
                {
                    m_fgLastStatusOk = statusNow;
                    if (!fg.statusOk)
                        LOG_WARNING("GraphicsDX12: Frame Generation is enabled but reports a problem: %s",
                                    fg.statusText);
                    else
                        LOG_INFO("GraphicsDX12: Frame Generation running (max %ux on this GPU). Rendered "
                                 "%.1f FPS; present counters over %u frames: SL %u, DXGI %u. Both counters "
                                 "are unreliable in this configuration - confirm the 2x with PresentMon or "
                                 "the NVIDIA overlay.",
                                 fg.numFramesToGenerateMax + 1, windowFps, m_fgFramesSincePoll,
                                 fg.framesPresentedSincePoll, dxgiPresentDelta);
                }
                (void)ratio; (void)dxgiRatio;
            }
            m_fgFramesSincePoll = 0;
        }
    }
    else
    {
        m_fgFramesSincePoll = 0;
        m_fgLastStatusOk    = -1;
        m_fgWindowStartQpc  = 0;   // re-stamp on the next enable
    }
}

// After a full GPU drain (GraphicsDX12::WaitForPreviousFrame): the back buffer DXGI will
// hand out next. No swap chain (it is released before the device): no back buffer to track.
void SurfaceDX12::SyncBackBufferIndex()
{
    if (m_swapChain)
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// PendingSwapChainOp is several fields (two std::optional, two bools, two uint32s), not a
// word: read it under the lock, or another thread's latch can be seen half written.
bool SurfaceDX12::HasPendingSwapChainOp() const
{
    std::lock_guard<std::mutex> lk(m_swapChainOpMutex);
    return m_pendingSwapChainOp.Pending();
}
