#pragma once
// ISurface - presentation: what a device does with the swap chain on a window.
//
// A caller that presents, resizes, switches fullscreen / HDR, paces against the display or captures
// the back buffer asks the device for its surface (IGraphicsDevice::MainSurface()); frames, command
// lists, resources and pipelines stay on IGraphicsDevice. There is one main surface (SurfaceDX12,
// owned by GraphicsDX12), its swap chain created and released by App around the device's life
// (GraphicsDX12::CreateMainSurface / ReleaseMainSurface). The window sees only this interface
// (Plat::Window::AttachSurface), never the device. The presentation calls name the surface they
// mean: what a second surface, or none (headless), would build on.
//
// Resize, SetFullscreen and an HDR mode change LATCH and are applied outside a frame (the
// FRAME-BOUNDARY CONTRACT in GraphicsDX12.h).

#include "Graphics/GraphicsStruct.h"
#include "Graphics/HDROutput.h"

#include <cstdint>
#include <string>

namespace RHI
{
    // Presentation + pacing knobs shared by the settings file, the editor panels and the frame
    // limiter, grouped so callers can read-modify-write without reaching into backend fields.
    struct PresentSettings
    {
        // Sentinel for the dynamic mode, so call sites do not spell 4.
        static constexpr int kFrameGenDynamic = 4;

        int   frameRateCap = 0;      // 0 = uncapped (CPU-side limiter)
        bool  vsync        = true;
        bool  gsync        = false;  // output is a VRR panel
        float renderScale  = 1.0f;   // HDR target scale vs swap-chain size

        // ---- HDR display output ----
        // Here rather than behind its own setter because this struct is the ONLY channel
        // EngineSettings::ApplyTo / CaptureFrom reach, so anything that round-trips through settings.json
        // without an EventBus mirror lives here.
        // Caution: hdrMode rebuilds the swap chain, which is legal only outside a frame, while the callers
        // (the editor panels, EngineSettings::ApplyTo) run mid-frame inside an ImGui handler.
        // SetPresentSettings therefore LATCHES a mode change (SurfaceDX12::RequestHDROutputMode); every
        // other field is a store that is safe from any call site.
        HDROutputMode hdrMode        = HDROutputMode::SDR;
        // The player's paper white override; 0 follows Windows' SDR white level for the monitor
        // (ISurface::GetPaperWhiteNits is what runs).
        float paperWhiteNits         = 0.0f;
        float peakNits               = kHDRPeakNitsDefault;

        // ---- NVIDIA Reflex ----
        // A pacing setting, here for the same round-trip reason as hdrMode. Mirrors SL::ReflexMode (0 off,
        // 1 low latency, 2 low latency + boost) as an int so this header does not depend on Streamline;
        // the surface does the mapping.
        // When active, Reflex owns frame pacing and LimitFrameRate stands down: frameRateCap still picks
        // the number, Reflex does the waiting. Ignored when the runtime or GPU has no Reflex, so it is
        // safe to persist.
        int reflexMode               = 0;

        // ---- DLSS Frame Generation ----
        // Frames to GENERATE between rendered ones: 0 = off, 1 = 2x, 2 = 3x, 3 = 4x, 4 = DYNAMIC (the
        // multiplier tracks the display refresh; needs >2x-capable hardware and a recent driver). Capped at
        // runtime by what the GPU reports. Dynamic shares this field so there is one source of truth for
        // what Frame Generation is doing.
        // Needs a Streamline proxy swap chain and an SDR or HDR10 back buffer (DLSS-G does not support
        // scRGB FP16), and raises the effective Reflex mode to at least low latency without rewriting
        // reflexMode. Refused in the docked editor, whose back buffer is ImGui chrome with the scene in one
        // panel; the F11 fullscreen viewport allows it.
        int frameGenFrames           = 0;

        // ---- Colour-vision assist ----
        // The present shader's daltonization (Present.ps.hlsl): 0 off, 1 protanopia, 2 deuteranopia, 3
        // tritanopia, at colorVisionSeverity 0..1. Plain stores, safe from any call site.
        int   colorVision            = 0;
        float colorVisionSeverity    = 1.0f;
    };

    // What the display the swap chain currently lives on can do. Read-only, refreshed by the backend
    // whenever the output changes; all-zero / false means "not queried yet".
    struct DisplayHDRInfo
    {
        bool  hdrCapable          = false;  // OS reports the output in HDR mode
        float maxLuminanceNits    = 0.0f;   // DXGI_OUTPUT_DESC1::MaxLuminance
        float minLuminanceNits    = 0.0f;   // DXGI_OUTPUT_DESC1::MinLuminance
        float maxFullFrameNits    = 0.0f;   // sustained full-screen ceiling
    };
}

class ISurface
{
public:
    struct BackBufferShot
    {
        std::string   path;
        bool          ok = false;
        std::uint32_t width = 0, height = 0;
        std::string   error;
        bool          uniform = false;   // every pixel one colour: nothing was drawn on it
    };

    virtual ~ISurface() = default;

    // ---- presentation and pacing settings ----
    // Read/write the presentation + pacing settings as a group. SetPresentSettings may recompute
    // derived state (e.g. the VRR refresh ceiling), so callers must not cache across the call.
    virtual void                 SetPresentSettings(const RHI::PresentSettings&) {}
    virtual RHI::PresentSettings GetPresentSettings() const { return {}; }

    // What the display the swap chain currently sits on can do. Refreshed at boot, on fullscreen
    // transitions and on monitor / display changes; cheap to read every frame. The all-zero default
    // reads as "not HDR capable", the safe answer.
    virtual RHI::DisplayHDRInfo GetDisplayHDRInfo() const { return {}; }

    // The mode actually in effect. May lag a requested mode by one frame (latched to the next frame
    // boundary) and may be SDR if the swap chain refused the colour space. Anything that must match the
    // composition-buffer format MUST read this, never the setting.
    virtual HDROutputMode GetHDROutputMode() const { return HDROutputMode::SDR; }

    // The paper white and peak (nits) the HDR encode uses, resolved: the override or Windows' SDR white
    // level, and the peak raised to at least the paper white. GetPresentSettings reports the requests.
    virtual float GetPaperWhiteNits() const { return kHDRPaperWhiteDefault; }
    virtual float GetPeakNits() const { return kHDRPeakNitsDefault; }

    // CPU-side frame-rate limiter (sleep + spin until the frame deadline). App::Run calls it once per
    // frame AFTER EndFrame and AFTER capturing CPU frame time, so the pacing sleep never counts as CPU
    // work. No-op when there is no limiter or the cap is unlimited.
    virtual void LimitFrameRate() {}
    // Last frame's pacing wait (ms) inside LimitFrameRate, 0 when uncapped, so debug UIs can report
    // the sleep apart from real work.
    virtual float GetLastFramePacingWaitMs() const { return 0.f; }

    // ---- the window it presents to ----
    // Both LATCH (the window calls them from its WndProc / fullscreen service); the swap chain is
    // rebuilt at the next frame boundary.
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void SetFullscreen(bool fullscreen) = 0;

    // WM_DISPLAYCHANGE (the window's WndProc, main thread): the desktop mode, the monitor set or the
    // Windows HDR switch changed. Only raises a flag; the surface re-queries its output at its next
    // display poll.
    virtual void NoteDisplayChanged() noexcept {}

    // Bumped on every swap-chain rebuild (ResizeBuffers, an HDR format switch, fullscreen) and nothing
    // else, not a hard sync: the re-base point for anything relating DXGI's back-buffer index to a frame
    // counter, since ResizeBuffers resets that index to 0 while a monotonic frame counter keeps counting.
    virtual uint32_t GetSwapChainGeneration() const { return 0; }

    // The window is not visible to the person: Present reported DXGI_STATUS_OCCLUDED on several
    // consecutive frames (another window covers it, the screen is locked, the window is minimized).
    // Any thread; the background policy reads it.
    virtual bool IsOccluded() const { return false; }

    // Frames actually presented - not the game loop's frame counter (the render half can run a frame
    // behind, and a quit can retire the loop with a packet unrendered).
    virtual uint64_t PresentedFrames() const { return 0; }

    // ---- Streamline: Frame Generation and the per-frame tags ----
    // Frames Frame Generation is ACTUALLY generating (0 = off).
    virtual int GetFrameGenActive() const { return 0; }
    // Copies @p src (the final scene colour before the HUD, in COPY_SOURCE, back-buffer sized) into
    // Frame Generation's HUD-less input and tags it. No-op when Frame Generation is off.
    virtual void CaptureHudLessColor(RHI::CommandList /*cl*/, const RHI::Texture& /*src*/) {}
    // Frame Generation's UI layer: a cleared, transparent target (in RENDERTARGET) to draw the HUD
    // into, or nullptr when it is off. Call EndFrameGenUICapture after the HUD has been drawn.
    virtual const RHI::Texture* BeginFrameGenUICapture(RHI::CommandList /*cl*/) { return nullptr; }
    virtual void EndFrameGenUICapture(RHI::CommandList /*cl*/) {}
    // Tags this frame's depth and motion vectors (both in NON_PIXEL_SHADER_RESOURCE) for Streamline,
    // once, with the longer ValidUntilPresent lifetime: DLSS SR consumes them inside its evaluate,
    // Frame Generation at Present, and Frame Generation can run without DLSS SR. No-op without
    // Streamline.
    virtual void TagDepthAndMotion(RHI::CommandList /*cl*/, const RHI::Texture& /*depth*/,
                                   const RHI::Texture& /*motion*/, void* /*frameToken*/) {}

    // ---- recording onto the back buffer ----
    // Binds the swap-chain back buffer and clears it. Records into @p cmd.
    virtual void SetRenderTargetToSwapChain(const float clearColor[4], RHI::CommandList cmd) = 0;

    // Draws a fullscreen triangle sampling @p srvGpuHandle into the bound back buffer: THE DISPLAY
    // ENCODE. @p srvGpuHandle must be an extended-gamma composition buffer (Graphics/HDROutput.h); the
    // shader re-encodes for the swap chain (passthrough SDR, scaled linear scRGB, Rec.2020 + ST.2084
    // HDR10), so a raw linear HDR scene texture would be double-encoded. Call after
    // SetRenderTargetToSwapChain, on the same list.
    virtual void CompositeTextureToSwapChain(uint64_t srvGpuHandle, RHI::CommandList cmd) = 0;

    // The same draw into a SUB-RECT of the back buffer (an in-game picture-in-picture): same PSO, same
    // encode, no clear, no blend. Call after the full-screen composite, on the same list.
    virtual void CompositeTextureToSwapChainRect(uint64_t /*srvGpuHandle*/, int32_t /*x*/, int32_t /*y*/,
                                                 int32_t /*w*/, int32_t /*h*/, RHI::CommandList /*cmd*/) {}

    // Editor-only present path: ImGui composes the whole UI (scene image included) into an offscreen
    // buffer whose format never changes; ResolvePresentCompositionToSwapChain then writes it to the
    // back buffer through the display encode. Two calls because ImGui's submission sits between them.
    virtual void SetRenderTargetToPresentComposition(const float /*clearColor*/[4], RHI::CommandList /*cmd*/) {}
    virtual void ResolvePresentCompositionToSwapChain(RHI::CommandList /*cmd*/) {}

    // ---- capturing what was presented ----
    // Copies the NEXT presented back buffer (the whole window) into a PNG (-ui_screenshot, the pipe's
    // capture {source: app_ui}). Recorded inside EndFrame, written after that frame's GPU work
    // completes. Harness callers only; false = refused (a capture is already pending).
    virtual bool RequestBackBufferCapture(const char* /*path*/) { return false; }

    // How back-buffer captures ended, oldest first. Every request ends here once; a caller takes until
    // it finds the @p path it asked for. TakeBackBufferShot returns false when none is waiting.
    virtual bool TakeBackBufferShot(BackBufferShot& /*out*/) { return false; }
};
