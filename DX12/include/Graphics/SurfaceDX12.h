#pragma once
// Graphics/SurfaceDX12.h - the main surface: a swap chain on a window, and everything about
// presenting through it.
//
// What lives here: the swap chain and its back buffers, the latch that defers Resize /
// SetFullscreen / an HDR mode change to the frame boundary, the HDR output mode and the display's
// capability, the editor's present composition buffer and the display-encode PSO, the frame
// limiter, Reflex and Frame Generation, and the back-buffer capture. The device holds one
// (Surface()) and hands it out as an ISurface (MainSurface()).
//
// What does not: frames, command lists, fences, resources, the HDR scene target, Streamline's
// device-level setup. The surface reaches those through its device (m_gfx; the two are friends -
// a split of ownership, not an RHI boundary), and the device calls the surface's frame hooks
// (BeginFrameRecording / EndFrameRecording / Present) from BeginFrame and EndFrame.
//
// Lifetime: the device constructs it (so its settings exist before there is a window) and
// destroys it at a fixed point of ~GraphicsDX12, while the device, its queues and its descriptor
// heaps are alive. Create puts the swap chain on a window; Release takes it off in the order
// Streamline needs (Frame Generation off, the GPU idle, Streamline shut down, then the swap chain
// on a live HWND). There is one main surface; a second one, or none (headless), would start here.

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <wrl.h>
#include "d3d12.h"
#include "dxgi1_6.h"

#include "Graphics/ISurface.h"
#include "Platform/DisplayMonitor.h"           // the display snapshot
#include "Graphics/FrameConstants.h"          // RHI::kFramesInFlight
#include "Graphics/DescriptorHeapAllocator.h"  // DescriptorAllocation
#include "Graphics/StreamlineBackend.h"        // SL::FrameGenState only

class GraphicsDX12;

class SurfaceDX12 final : public ISurface
{
public:
    // The swap chain's buffer count: the device's frames in flight.
    static constexpr UINT kFrameCount = RHI::kFramesInFlight;

    // A compile-time constant on purpose: ImGui bakes its RTV format into its PSO at init and has no
    // setter. Public because ImGuiManager must pass exactly this value to ImGui_ImplDX12_Init; a
    // mismatch is a PSO/RTV mismatch every editor frame, not a colour shift.
    static constexpr DXGI_FORMAT kPresentCompositionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Detached: no swap chain until Create. Sets up the frame limiter's timer.
    explicit SurfaceDX12(GraphicsDX12& gfx);
    ~SurfaceDX12() override;
    SurfaceDX12(const SurfaceDX12&) = delete;
    SurfaceDX12& operator=(const SurfaceDX12&) = delete;

    // ---- Lifetime ----
    // The swap chain on @p hWnd at a @p width x @p height client, its back buffers, the device's HDR
    // scene target sized to it, and its display. Once.
    void Create(HWND hWnd, UINT width, UINT height);
    // Frame Generation off, the GPU idle, Streamline shut down (its proxy lives
    // on this swap chain), then the swap chain - while its window is alive.
    // Idempotent.
    void Release() noexcept;
    bool HasSwapChain() const noexcept { return m_swapChain != nullptr; }

    // The client the swap chain is sized to (0 x 0 before Create).
    uint32_t Width()  const noexcept { return m_width; }
    uint32_t Height() const noexcept { return m_height; }
    // The window the swap chain is on (null while detached).
    HWND     GetHwnd() const noexcept { return m_hWnd; }

    // ---- The window it presents to (latched; see ISurface) ----
    void Resize(uint32_t width, uint32_t height)  override;
    void SetFullscreen(bool fullscreen)            override;

    // Applies the latched swap-chain work: Resize / SetFullscreen (always latched) and an HDR mode or Frame Generation change that could not
    // apply at once. Main thread, at a frame boundary only: App::Run's loop top, after Window's
    // ServicePendingFullscreen / ServicePendingSize. BeginFrame() also calls it as a backstop on the
    // device thread (before it touches the slot fence); on the render-owner lane it leaves the op to
    // the loop top. The loop-top call applies it a frame sooner and outside any frame.
    void ServicePendingSwapChainOps();
    // Claim / release exclusive ownership of the swap chain for one rebuild: held across the rebuild
    // through the flag, not the mutex, because ResizeBuffers and SetFullscreenState re-enter Resize()
    // synchronously on the same thread through WM_SIZE.
    bool ClaimSwapChainOp();
    void ReleaseSwapChainOp();
    // What ServicePendingSwapChainOps runs (Resize / SetFullscreen latch). They repeat the idempotence and re-entrancy guards because a
    // request latched a frame ago may have become a no-op.
    void ApplyResizeNow(uint32_t width, uint32_t height);
    void ApplyFullscreenNow(bool fullscreen);

    void SetRenderTargetToSwapChain(const float clearColor[4],
                                    RHI::CommandList cmd) override;
    void CompositeTextureToSwapChain(uint64_t         srvGpuHandle,
                                     RHI::CommandList cmd) override;
    // The same draw into a sub-rect (the in-game PiP inset).
    void CompositeTextureToSwapChainRect(uint64_t srvGpuHandle,
                                         int32_t x, int32_t y,
                                         int32_t w, int32_t h,
                                         RHI::CommandList cmd) override;
    // Shader hot reload (GraphicsDX12::ReloadDeviceShaders): rebuild the
    // present composite from shaders/Present.{vs,ps}.hlsl as reloaded; a
    // failure keeps the pipeline it had. False when nothing was rebuilt.
    bool ReloadPresentShaders();

    // ---- Present composition buffer (editor ImGui target) ----
    // Bind + clear the FP16 buffer ImGui composes into, then resolve it to the back buffer through the
    // display encode. Editor builds use this pair in place of SetRenderTargetToSwapChain +
    // CompositeTextureToSwapChain; game builds keep the direct path. ImGui bakes its RTV format at
    // init, so a target whose format never changes makes HDR switchable at runtime without
    // re-initialising the backend (see EnsurePresentComposition in the .cpp).
    void SetRenderTargetToPresentComposition(const float clearColor[4],
                                             RHI::CommandList cmd) override;
    void ResolvePresentCompositionToSwapChain(RHI::CommandList cmd) override;

    // The DXGI back-buffer index. Only the render-target pick and the present barriers may use it.
    uint32_t BackBufferIndex() const { return m_frameIndex; }

    uint32_t GetSwapChainGeneration() const override { return m_swapChainGeneration; }

    void                 SetPresentSettings(const RHI::PresentSettings& ps) override;
    // Reports the requested hdr mode, not the effective one: this is the struct
    // EngineSettings::CaptureFrom persists, and a display refusing a mode right now must not erase
    // the preference. GetHDROutputMode() says what is on screen. The nits are the requests too (paper
    // white 0 = follows Windows); GetPaperWhiteNits() / GetPeakNits() are what the encode uses.
    RHI::PresentSettings GetPresentSettings() const override
    { return { targetFrameRate, vsyncEnabled, gsyncEnabled, renderScale,
               m_hdrModeRequested, m_paperWhiteRequested, m_peakRequested,
               // REQUESTED, like hdrMode above: on a machine without Reflex the
               // request is ignored at runtime but must survive to settings.json,
               // or moving the file to a machine that has it loses the choice.
               m_reflexModeRequested, m_frameGenRequested,
               m_colorVision.load(std::memory_order_relaxed),
               m_colorVisionSeverity.load(std::memory_order_relaxed) }; }

    // ---- HDR display output ----
    // A new output mode rebuilds the swap chain with a new format, legal only outside a frame: outside
    // one (CanRebuildSwapChain, the boot case) this applies it at once through ApplyHDROutputMode;
    // inside one it latches into m_pendingSwapChainOp and ServicePendingSwapChainOps performs it at
    // the main-loop boundary. A request equal to the REQUESTED mode is dropped, so the editor's
    // unconditional "Reset to Defaults" does not rebuild back buffers on every press. Not the live
    // mode: after a demotion to SDR that compare would make every unrelated SetPresentSettings call
    // retry the rebuild.
    void RequestHDROutputMode(HDROutputMode mode);

    // ---- Frame Generation ----
    // Frames Streamline is actually generating (0 = off). Differs from the request whenever a gate
    // refuses it; the editor shows this one.
    int GetFrameGenActive() const override { return m_frameGenActive; }

    // Why Frame Generation is not running despite being requested, or nullptr when there is no such
    // reason. Shown in the editor: a silently refused request looks like a broken feature.
    const char* GetFrameGenRefusal() const { return m_frameGenRefusal; }

    // Last polled Frame Generation state, and the presented/rendered ratio over the poll window.
    // Read this, never slDLSSGGetState directly: DLSSGState's frame counter is "presents since the
    // previous query" and the SDK resets it on every call, so a second reader drains the poll window
    // and Frame Generation looks dead. One owner polls; everyone else reads this snapshot.
    const SL::FrameGenState& GetFrameGenSnapshot() const { return m_fgSnapshot; }
    float GetFrameGenPresentRatio() const { return m_fgLastRatio; }

    // Static Frame Generation capabilities: how many frames this GPU can generate, whether the
    // dynamic multiplier and VSync are available, and the SDK's VRAM estimate. Probed lazily, retried
    // every kFGCapsProbeInterval reads until the driver reports a maximum, then cached (at once,
    // empty, where DLSS-G is unsupported). Separate from the snapshot, which means something only
    // while FG runs: the editor's multiplier list and the clamp need the maximum also when FG will
    // not start. Never probed while FG runs: the same query resets the present counter
    // GetFrameGenSnapshot's window depends on.
    const SL::FrameGenState& GetFrameGenCaps();

    // Captures the pre-UI scene as Frame Generation's HUD-less colour input. @p src must already be
    // in COPY_SOURCE and match the back buffer in size and format (DLSS-G requires the back buffer's
    // extent; CopyResource requires identical formats). Required for Frame Generation: without it
    // slDLSSGGetState still reports eDLSSGStatusOk but no frames are generated. No-op when FG is
    // inactive.
    void CaptureHudLessColor(RHI::CommandList cl, ID3D12Resource* src);
    // The ISurface form, for an RHI texture.
    void CaptureHudLessColor(RHI::CommandList cl, const RHI::Texture& src) override;

    // Frame Generation's UI layer. Returns a cleared, transparent render target to draw the
    // screen-space HUD into, or nullptr when FG is inactive (then draw nothing extra).
    // While FG runs the HUD is drawn twice: into the scene, so the presented image is unchanged, and
    // into this target as a DLSS-G input. One extra HUD draw avoids compositing this layer back over
    // the scene, which would need a new blend pipeline and a read-modify-write on an R8G8B8A8 UAV
    // (typed load not guaranteed by D3D12). UIPass's blend state already gives the premultiplied
    // result DLSS-G expects (colour SRC_ALPHA/INV_SRC_ALPHA, alpha ONE/INV_SRC_ALPHA), so no shader or
    // state changes. On return the target is in RENDERTARGET: pass that as UIPass's entry state, then
    // call EndFrameGenUICapture.
    const RHI::Texture* BeginFrameGenUICapture(RHI::CommandList cl) override;

    // Transitions the UI target to a readable state and tags it. Must follow a non-null
    // BeginFrameGenUICapture, after the HUD is drawn.
    void EndFrameGenUICapture(RHI::CommandList cl) override;

    // This frame's depth and motion vectors for Streamline, ValidUntilPresent.
    void TagDepthAndMotion(RHI::CommandList cl, const RHI::Texture& depth, const RHI::Texture& motion,
                           void* frameToken) override;

    // Drops the Frame Generation scratch targets. They are back-buffer sized, so whatever rebuilds
    // the back buffers must call this, or the next capture copies into a stale-sized resource.
    void ReleaseFrameGenTextures();

    // Editor only: whether the viewport is in F11 fullscreen. Decides whether Frame Generation may
    // run in the editor: docked, the scene is one ImGui panel in a window of chrome, so back buffer
    // and scene differ in size and content and depth / motion describe a sub-rectangle - nothing
    // coherent to interpolate. In F11 the viewport is the whole window, so in SDR the tone-map output
    // matches the back buffer as in the game build. Game builds never call this.
    void SetEditorViewportFullscreen(bool v);

    // ---- Display, capture and frame pacing ----
    HDROutputMode       GetHDROutputMode()  const override { return m_hdrMode; }
    float               GetPaperWhiteNits() const override { return m_paperWhiteNits; }
    float               GetPeakNits()       const override { return m_peakNits; }
    RHI::DisplayHDRInfo GetDisplayHDRInfo() const override
    {
        const Plat::DisplayMonitorPtr d = Display();
        return { d->hdrCapable, d->maxLuminanceNits, d->minLuminanceNits, d->maxFullFrameNits };
    }
    // The monitor the window is on, as one immutable value (any thread). Written only on the main thread,
    // by RefreshDisplayInfo, and published whole.
    Plat::DisplayMonitorPtr Display() const { return m_display.load(std::memory_order_acquire); }
    DXGI_FORMAT         GetSwapChainFormat() const noexcept { return m_swapChainFormat; }

    // The harness's -ui_screenshot and the pipe's app_ui capture (see ISurface). One pending
    // request at a time; TakeBackBufferShot says how it ended.
    bool             RequestBackBufferCapture(const char* path) override;
    bool             TakeBackBufferShot(BackBufferShot& out) override;

    // Present answered DXGI_STATUS_OCCLUDED for kOccludedPresents frames in a row (any thread).
    bool IsOccluded() const override { return m_occluded.load(std::memory_order_relaxed); }

    // Frames actually presented. See m_presentedFrames for why this is not the
    // same number as the game loop's frame counter.
    uint64_t PresentedFrames() const noexcept override
    { return m_presentedFrames.load(std::memory_order_relaxed); }

    // Refresh rate of the monitor the window sits on, rounded to whole Hz (59.94 reads 60). 0 = unknown.
    // GetDisplayRefreshRate is the exact fraction.
    int      GetDisplayRefreshHz() const
    { return static_cast<int>(std::lround(Display()->refresh.Hz())); }
    Plat::RefreshRate GetDisplayRefreshRate() const { return Display()->refresh; }
    uint32_t          DisplayLooks() const { return m_displayLooks.load(std::memory_order_relaxed); }
    // Desktop resolution of the monitor the window is on, refreshed with the refresh rate; 0 until
    // the first successful query. A resolution picker filters its presets with it: the engine has no
    // display-mode enumeration, so this is the ceiling, not a mode list.
    int      GetDesktopWidth()  const { return Display()->desktopWidth;  }
    int      GetDesktopHeight() const { return Display()->desktopHeight; }
    // Re-query the monitor under the window (main thread: on a G-Sync toggle, a screen move, a fullscreen
    // change, a display change) and publish the result when anything differs. Also invoked once at the
    // end of Create.
    void     RefreshDisplayInfo();
    // WM_DISPLAYCHANGE (Window's WndProc, main thread): the desktop mode, the monitor set or the
    // Windows HDR switch changed. Only raises a flag; ServiceDisplay (the frame boundary) re-queries.
    void     NoteDisplayChanged() noexcept override { m_displayChanged.store(true, std::memory_order_release); }
    // The same as RefreshDisplayInfo: the HDR capability is part of the snapshot.
    void     RefreshDisplayHDRInfo() { RefreshDisplayInfo(); }
    // Latch a retry when the requested HDR mode is not the one in effect and
    // the display's capability just changed (user toggled HDR in Windows, or
    // the window moved to a different monitor).
    void     ReconcileHDROutputMode();
    // Effective cap (fps) after applying the G-Sync ceiling. 0 = unlimited.
    int      EffectiveFrameCap() const;

    // Busy-wait/sleep to hold the target frame period. No-op when uncapped.
    // Called by App::Run at true end-of-frame (see targetFrameRate comment).
    void     LimitFrameRate() override;
    // Last frame's pacing wait (ms); 0 when uncapped. For debug UIs.
    float    GetLastFramePacingWaitMs() const override { return m_lastPacingWaitMs; }

private:
    // A back-buffer copy recorded by RecordBackBufferCapture: the READBACK buffer, the copy's
    // footprint and the PNG path. armed until FinishBackBufferCapture takes it.
    struct UiCapture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT     footprint{};
        UINT                                   rows = 0;
        UINT64                                 rowBytes = 0;
        std::string                            path;
        bool                                   armed = false;
    };

    // Swap-chain work deferred to the next frame boundary (ServicePendingSwapChainOps applies it).
    struct PendingSwapChainOp
    {
        bool                haveResize = false;
        uint32_t            width      = 0;
        uint32_t            height     = 0;
        // std::optional, not bool: "no request" and "requested false" must stay distinct, or an
        // unconditional SetFullscreen(false) on a windowed swap chain rebuilds every back buffer under
        // an open command list (a crash).
        std::optional<bool> fullscreen;
        // HDR mode changes rebuild the swap chain with a different format, so they are latched like the
        // other two. std::optional for the same reason as `fullscreen`: "no request" must stay distinct
        // from "requested SDR", or the editor's Reset-to-Defaults (applied unconditionally) rebuilds every
        // back buffer on each click.
        std::optional<HDROutputMode> hdrMode;
        // Frame Generation: Streamline recreates the swap chain through its proxy when the multiplier
        // changes, so it shares the latch. A bool, not the value: m_frameGenRequested is already stored,
        // this only records that the resolve still owes a run, and ServicePendingSwapChainOps' FGRestore
        // guard re-resolves from the request unconditionally.
        bool                frameGen   = false;
        bool Pending() const noexcept
        { return haveResize || fullscreen.has_value() || hdrMode.has_value() || frameGen; }
    };

    // Consecutive occluded presents that make IsOccluded true.
    static constexpr uint32_t kOccludedPresents = 4;

    // Frames between ServiceDisplay's capability re-queries while the requested HDR mode is not the one
    // in effect (~2 s at 60 fps).
    static constexpr uint32_t kHdrCapabilityPollFrames = 120;

    // Rendered frames per FG measurement window. 600, not 120: DXGI's PresentCount does not advance
    // in lockstep with our frames (VRR, tearing presents and DLSS-G's own pacing make it lumpy), and
    // over 120 frames the ratio swung between 0.17x and 4.42x while steadily generating 2x.
    static constexpr uint32_t kFGPollWindow = 600;

    // GetFrameGenCaps reads between probes until the driver reports a maximum, so a panel reading the
    // caps every frame does not become a per-frame SDK query.
    static constexpr uint32_t kFGCapsProbeInterval = 120;

    friend class GraphicsDX12;

    // ---- The device's frame (GraphicsDX12::BeginFrame / EndFrame / WaitForPreviousFrame) ----
    // The primary list's full-window viewport, and the back buffer PRESENT -> RENDER_TARGET.
    void BeginFrameRecording(ID3D12GraphicsCommandList* cl);
    // The back buffer RENDER_TARGET -> PRESENT - or, with a back-buffer capture
    // pending, RT -> COPY_SOURCE, the copy and COPY_SOURCE -> PRESENT.
    void EndFrameRecording(ID3D12GraphicsCommandList* cl);
    // Present (the PCL markers around exactly that call), the capture's PNG,
    // Frame Generation's health window. After every list is submitted.
    void Present();
    // The loop top (ServicePendingSwapChainOps, main thread): look at the display again when
    // WM_DISPLAYCHANGE came, the window changed monitor, or an HDR request is waiting for the display to
    // support it. It used to run at the end of EndFrame, on the render-owner lane.
    void ServiceDisplay();
    // Present's answer: DXGI_STATUS_OCCLUDED in a row means the window is covered (IsOccluded).
    void NotePresentResult(HRESULT hr) noexcept;
    // After a full GPU drain: the back buffer DXGI will hand out next.
    void SyncBackBufferIndex();
    bool HasPendingSwapChainOp() const;

    // Present composition buffer (see the .cpp). Recreated on resize and on a change of
    // @p clearColor: D3D12 compares all four components of a ClearRenderTargetView with the optimised
    // clear value the resource was created with, and a mismatch raises
    // CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE every editor frame. Passing the colour in keeps the
    // two in step.
    void EnsurePresentComposition(UINT width, UINT height, const float clearColor[4]);

    // ---- Resize helpers ----
    void AcquireBackBuffers();   // the buffers + RTVs, after creation and every ResizeBuffers
    void ResizeSwapChainAndRtv(UINT width, UINT height);

    // Rebuild the swap chain into the format @p mode requires and hand the display the matching
    // colour space. Outside a frame only (refused inside one): RequestHDROutputMode calls it between
    // frames, ServicePendingSwapChainOps for a latched request. Before Create it only records the mode
    // and its format. Falls back to SDR (m_hdrMode demoted, back buffers rebuilt at 8 bit) if the
    // display refuses the colour space.
    void ApplyHDROutputMode(HDROutputMode mode);
    // Re-issue SetColorSpace1 for m_hdrMode. Must run after every ResizeBuffers and after
    // SetFullscreenState: both reset the colour space to the sRGB default, silently turning an HDR
    // frame over-saturated. Returns false if the display refused the space; it does not demote
    // m_hdrMode itself, since mode and back-buffer format move together and only ApplyHDROutputMode
    // rebuilds the back buffers.
    bool ApplySwapChainColorSpace();
    // Push DXGI_HDR_METADATA_HDR10 for the current mode + nit settings. Safe to
    // call mid-frame: it touches no back buffers.
    void ApplyHDRMetadata();
    // m_paperWhiteNits / m_peakNits from the requests and the display snapshot: the paper white is the
    // override or Windows' SDR white level, the peak is never below it. Pushes the metadata when they move.
    void ResolveHDRLevels();
    // Re-create the back buffers at an explicit format and rebuild their RTVs
    // (also invalidating the present PSO, which bakes the format). Suspends Frame
    // Generation and drops its scratch targets itself; the caller owns the
    // frame-boundary and re-entrancy guards and the GPU drain.
    bool RebuildBackBuffers(DXGI_FORMAT fmt);
    // The one authoritative mode -> back-buffer-format mapping.
    static DXGI_FORMAT SwapChainFormatFor(HDROutputMode mode);

    // ---- Present blit: the display encode (extended gamma -> swap chain) ----
    void EnsurePresentPSO();
    // shaders/Present.{vs,ps}.hlsl through ShaderDiskCache::LoadOrCompile into a new root signature
    // + PSO for m_swapChainFormat; false (logged) leaves both outputs untouched.
    bool BuildPresentPSO(Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRootSig,
                         Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO);

    // ---- Back-buffer capture (-ui_screenshot) ----
    // -ui_screenshot. The game half posts the request (HarnessCaptureSystem, PostRender) and whichever
    // EndFrame runs next consumes it, possibly on the render thread - hence the mutex. The bodies are
    // in GraphicsDX12_Capture.cpp. RecordBackBufferCapture replaces the back buffer's RENDER_TARGET ->
    // PRESENT barrier with RT -> COPY_SOURCE, a copy into a READBACK buffer and COPY_SOURCE ->
    // PRESENT; FinishBackBufferCapture (after Present) waits for that frame's GPU work on a private
    // fence and writes the PNG.
    bool RecordBackBufferCapture(ID3D12GraphicsCommandList* cl, ID3D12Resource* backBuffer);
    void FinishBackBufferCapture();
    // How a capture ended (TakeBackBufferShot), under m_uiCaptureMutex.
    void NoteBackBufferShot(const std::string& path, bool ok, std::uint32_t w, std::uint32_t h,
                            std::string error, bool uniform = false);

    // ---- Frame Generation ----
    // Applies m_frameGenRequested subject to every gate (support, editor host,
    // back-buffer format, Reflex). Safe to call repeatedly; only talks to
    // Streamline when the effective value changes.
    void     ApplyFrameGeneration();
    // Forces FG off and returns whether it was on. Streamline rebuilds the swap chain when FG
    // toggles, so every resize, fullscreen change and HDR format change brackets itself with
    // SuspendFrameGeneration / ResumeFrameGeneration. Resume ignores @p wasOn and re-resolves the gates
    // (ApplyFrameGeneration): the operation may have changed them.
    bool     SuspendFrameGeneration();
    void     ResumeFrameGeneration(bool wasOn);

    // ---- Present blit: the display encode (extended gamma -> swap chain) ----
    // Built lazily on the first composite (EnsurePresentPSO). Game-mode App::Run uses it to display the
    // tonemapped scene without ImGui; the editor uses it to resolve its composition buffer.
    // The PSO bakes RTVFormats[0] = m_swapChainFormat, so it must not outlive a format change: a
    // mismatched target is a device-removal-class bug that shows as "HDR does nothing".
    // RebuildBackBuffers (every HDR format change) resets it.
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_presentRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_presentPSO;
    // Format the cached PSO was built for. EnsurePresentPSO rebuilds when it differs from
    // m_swapChainFormat, so a path that changes the format without resetting the PSO cannot draw
    // into a mismatched target.
    DXGI_FORMAT                                  m_presentPSOFormat { DXGI_FORMAT_UNKNOWN };

    // Presented frames, counted where Present succeeds. Distinct from the game loop's ctx.frame
    // (what -frames=N and run.json's framesRun report): equal while Present runs in the iteration that
    // increments ctx.frame; with the render half a frame behind they diverge, most visibly on the
    // last frame, where the quit path can retire the loop with a packet unrendered. Atomic: the render
    // thread increments it, the game thread reads it at shutdown.
    std::atomic<uint64_t>                             m_presentedFrames { 0 };

    std::mutex                                        m_uiCaptureMutex;
    std::string                                       m_uiCaptureRequest;
    std::vector<BackBufferShot>                       m_uiCaptureShots;   // untaken, oldest first (<= 8)

    UiCapture                                         m_uiCapture;

    GraphicsDX12& m_gfx;

    HWND     m_hWnd  { nullptr };
    uint32_t m_width { 0 };
    uint32_t m_height{ 0 };
    Microsoft::WRL::ComPtr<IDXGISwapChain3>           m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_scRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_renderTargets[kFrameCount];

    // The DXGI back-buffer index: selects only the render target and the PRESENT<->RENDER_TARGET
    // barriers, not any per-frame upload ring. Read it through BackBufferIndex(), never
    // GetFrameIndex().
    uint32_t                                          m_frameIndex { 0 };
    // Bumped only where the swap chain is (re)built; see ISurface::GetSwapChainGeneration for
    // why a hard sync must not bump it.
    uint32_t                                          m_swapChainGeneration { 0 };

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissorRect{};

    // Re-entrancy latch for Resize / SetFullscreen. IDXGISwapChain::SetFullscreenState delivers WM_SIZE
    // synchronously, so the window proc calls back into Resize() while SetFullscreen() is halfway
    // through rebuilding the back buffers; the nested call only echoes the transition in progress.
    // Set only in ClaimSwapChainOp / ReleaseSwapChainOp, under m_swapChainOpMutex, so the claim is
    // atomic with the pending-op read. Atomic as well because ReconcileHDROutputMode reads it off-lock,
    // from the tail of a rebuild still in progress (ApplyHDROutputMode -> RefreshDisplayHDRInfo).
    std::atomic<bool>                                 m_swapChainOpActive { false };
    // Real swap-chain fullscreen state, so SetFullscreen can early-out: a no-op apply must not tear
    // down and rebuild every back buffer (see the idempotence guard in SetFullscreen).
    bool                                              m_isFullscreen { false };

    // The work latched so far; consumed (cleared before it is applied) by ServicePendingSwapChainOps.
    PendingSwapChainOp                                m_pendingSwapChainOp {};
    // Guards m_pendingSwapChainOp and m_swapChainOpActive together: they are one decision ("is a
    // rebuild latched or running"), and splitting them would let a WM_SIZE on the game thread latch
    // against a stale active-flag read.
    mutable std::mutex                                m_swapChainOpMutex;

    // ---- HDR display output ----
    // The live swap-chain format. Both ResizeBuffers paths (windowed and enter-fullscreen) pass
    // DXGI_FORMAT_UNKNOWN, so a plain resize keeps the current format; only ApplyHDROutputMode passes
    // an explicit one. Stored once here so the paths cannot diverge.
    DXGI_FORMAT                                       m_swapChainFormat { DXGI_FORMAT_R8G8B8A8_UNORM };
    // Two modes on purpose. m_hdrMode is what is in effect: the present shader and the
    // composition-buffer format follow it, or they disagree with the back buffer. m_hdrModeRequested
    // is what the user asked for: GetPresentSettings reports it, so EngineSettings::CaptureFrom
    // persists it. With one mode, picking HDR10 while Windows has HDR off for that display (the swap
    // chain refuses, the mode demotes to SDR) would write SDR back on the next settings save.
    HDROutputMode                                     m_hdrMode         { HDROutputMode::SDR };
    HDROutputMode                                     m_hdrModeRequested{ HDROutputMode::SDR };
    // The requests (PresentSettings: paper white 0 = follows Windows) and what they resolve to.
    float                                             m_paperWhiteRequested { 0.0f };
    float                                             m_peakRequested   { kHDRPeakNitsDefault };
    float                                             m_paperWhiteNits  { kHDRPaperWhiteDefault };
    float                                             m_peakNits        { kHDRPeakNitsDefault };
    // Whether the paper white last resolved followed Windows: a source change gets a log line.
    bool                                              m_paperWhiteFollows { false };
    // The colour-vision assist the present shader applies (PresentSettings::colorVision). Written by
    // the game half, read when the render half records the composite.
    std::atomic<int>                                  m_colorVision     { 0 };
    std::atomic<float>                                m_colorVisionSeverity { 1.0f };
    // The display snapshot (monitor, mode, refresh, HDR output capability, SDR white level). Replaced
    // whole by the main thread's RefreshDisplayInfo; every other thread only takes the pointer.
    std::atomic<Plat::DisplayMonitorPtr>              m_display { std::make_shared<const Plat::DisplayMonitor>() };

    // Frame counter for the capability poll that runs while a requested HDR mode is not the one in
    // effect (ServiceDisplay): toggling HDR in Windows does not change the HMONITOR, so a handle
    // compare cannot see it.
    Plat::RefreshPoll                                 m_displayPoll { 0, kHdrCapabilityPollFrames };
    // Raised by NoteDisplayChanged (main), consumed by ServiceDisplay.
    std::atomic<bool>                                 m_displayChanged { false };
    // Times the display was looked at (RefreshDisplayInfo), changed or not: a pack proves a
    // WM_DISPLAYCHANGE reached the surface with it.
    std::atomic<uint32_t>                             m_displayLooks { 0 };
    // Present's DXGI_STATUS_OCCLUDED answers in a row (written by whichever thread presents), and
    // whether that is enough to call the window covered. A single one is a transition (a window
    // just shown, a mode change), not an occlusion.
    uint32_t                                          m_occludedPresents { 0 };
    std::atomic<bool>                                 m_occluded { false };
    // The thread the surface was created on (the main thread): the only one that looks at the display.
    std::thread::id                                   m_mainThread {};

    // Present composition buffer + its views. Only allocated once an editor
    // build actually asks for it, so a Game.exe never pays for it.
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_presentComposition;
    DescriptorAllocation                              m_presentCompRtv  {};
    DescriptorAllocation                              m_presentCompSrv  {};
    D3D12_RESOURCE_STATES                             m_presentCompState{ D3D12_RESOURCE_STATE_RENDER_TARGET };
    UINT                                              m_presentCompW    { 0 };
    UINT                                              m_presentCompH    { 0 };
    // The optimised clear value the live resource was created with. Compared
    // (not just stored) by EnsurePresentComposition - see its declaration.
    float                                             m_presentCompClear[4]{ 0.f, 0.f, 0.f, 1.f };

    bool     vsyncEnabled = true; // false = uncapped FPS with ALLOW_TEARING

    // Internal render-scale [0.25..2.0]; App applies this only to the scene HDR
    // target before SetViewportSize (settings.json "display.renderScale").
    // It never changes the display viewport, UI canvas, or input coordinates.
    float    renderScale = 1.0f;

    // ---- Frame-rate limiter ----
    // targetFrameRate: CPU-side FPS cap enforced by LimitFrameRate(), which App::Run calls once per
    // frame after EndFrame and its frame-timing capture (so profilers never count the pacing sleep as
    // CPU work). 0 = unlimited. The editor offers presets (30/60/120/144/165/180).
    int      targetFrameRate = 0;
    // gsyncEnabled: the output is a G-Sync / FreeSync (VRR) panel. Then presents use ALLOW_TEARING
    // (no vblank block) so the display syncs to the GPU, and the effective cap is clamped just under
    // the panel's max refresh: above it G-Sync disengages (tearing or forced-VSync latency), so the
    // ceiling wins even over a higher or unlimited targetFrameRate.
    bool     gsyncEnabled = false;

    // ---- Frame-limiter pacing state ----
    HANDLE   m_frameTimerHandle    { nullptr }; // high-res waitable timer (may be null)
    int64_t  m_nextFramePresentQpc { 0 };       // QPC deadline of the next present
    bool     m_mmTimerPeriodSet    { false };   // did we raise the global timer resolution?
    float    m_lastPacingWaitMs    { 0.f };     // last frame's LimitFrameRate wait, for debug UIs
    // Frame cap last handed to Reflex, in microseconds. UINT32_MAX means "not
    // delegated", which forces a re-push on the next handover. Exists purely so
    // slReflexSetOptions is called on change rather than every frame.
    std::atomic<uint32_t> m_reflexFrameLimitUs { UINT32_MAX };

    // ---- Reflex and Frame Generation ----
    // What the user asked for (0 off / 1 low latency / 2 boost). Persisted even when the runtime
    // cannot honour it; see GetPresentSettings.
    int      m_reflexModeRequested { 0 };
    // Frames to generate, as requested. Persisted even when refused (no hardware / editor host /
    // scRGB back buffer): the same requested-vs-effective rule as hdrMode and Reflex.
    int      m_frameGenRequested   { 0 };
    // What Streamline is configured with right now. Differs from the request whenever FG is refused;
    // SuspendFrameGeneration sets it to 0.
    int      m_frameGenActive      { 0 };
    // Effective Reflex mode actually pushed to Streamline. Differs from
    // m_reflexModeRequested when Frame Generation forces it on.
    int      m_reflexModeEffective { 0 };
    bool     m_loggedFGRefusal     { false };
    bool     m_loggedFGClamp       { false };
    // Always a string literal or a Streamline-owned static, so storing the
    // pointer is safe.
    const char* m_frameGenRefusal  { nullptr };

    // Editor F11 viewport fullscreen (SetEditorViewportFullscreen). The window state saved for
    // borderless fullscreen belongs to Plat::Window::SetFullscreen.
    bool     m_editorViewportFullscreen { false };
    // Frame Generation's HUD-less colour copy. Matches the back buffer in size
    // and format; rebuilt whenever the back buffers are.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hudlessTex;
    bool     m_loggedHudlessSkip   { false };
    // Frame Generation's UI layer (premultiplied, back-buffer sized).
    RHI::Texture m_fgUITex{};
    bool     m_fgUITexCreated      { false };
    // Last FG health verdict, so the periodic poll logs only on a change. Tri-state: -1 = never
    // polled, 0 = failing, 1 = generating; a bool defaulting to false would make a failing FG equal
    // the initial value forever and never be reported.
    int      m_fgLastStatusOk      { -1 };

    // Rendered frames counted into the current FG measurement window (kFGPollWindow).
    uint32_t m_fgFramesSincePoll   { 0 };
    int64_t  m_fgWindowStartQpc    { 0 };   // QPC at the start of that window
    // Snapshot of the last poll, for every other reader (see
    // GetFrameGenSnapshot for why nobody else may query Streamline directly).
    SL::FrameGenState m_fgSnapshot{};
    float    m_fgLastRatio         { 0.0f };
    // Static capabilities (see GetFrameGenCaps); m_fgCapsProbed once they are cached for good.
    SL::FrameGenState m_fgCaps{};
    bool     m_fgCapsProbed        { false };

    // GetFrameGenCaps reads since the last probe. Starts armed, and ApplyFrameGeneration re-arms it
    // before its read: that read needs the real maximum to clamp against, so it probes at once.
    uint32_t m_fgCapsProbeTick     { kFGCapsProbeInterval };
    // Independent cross-check: DXGI's own present counter, which no engine code
    // resets and which does not depend on Streamline's counter semantics.
    uint32_t m_fgLastPresentCount  { 0 };
};
