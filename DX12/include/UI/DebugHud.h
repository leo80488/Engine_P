#pragma once

// DebugHud - Unreal-style "stat unit" overlay drawn with the engine's own SDF font into
// the UIPass draw list, post-tonemap and post-upscale, at display resolution, in both exes
// (the SceneTransitionManager::DrawOverlay path). Not ImGui: Game.exe has none, and the
// editor's F11 fullscreen - the mode this HUD exists for - returns from
// EditorLayer::OnUIRender before the Profiler panel.
// Frame contract (read before adding a call site):
// - Call Emit() on the main thread, between the transition overlay and
//   FontLibrary::FlushPendingUploads() (App::RenderPhase). Font::DrawText bakes missing
//   glyphs into the CPU atlas mirror and only that flush uploads them: emitting later draws
//   new glyphs from stale texels for a frame; off the main thread it races the glyph map.
// - Emit() is a pure producer and must not mutate HUD state: UIPass::Execute runs twice per
//   frame under DLSS Frame Generation. All sampling happens in Update().

#include "UI/UIDrawList.h"

#include <cstdint>

class Renderer;
class IGraphicsDevice;

namespace UI
{
    // Caller-filled snapshot, so this header needs neither Renderer.h nor UIPass.h (UIPass owns
    // the UIDrawList, not the reverse; do not make the UI layer depend upward).
    // Every `*Valid` flag marks a number that can be absent rather than zero; the HUD renders
    // "--" for it, never a misleading 0.
    struct DebugHudStats
    {
        // ---- Timing (Diag::FrameTiming) ----
        float    fps        = 0.f;
        float    frameMs    = 0.f;   // TRUE wall period (includes pacing sleep)
        float    cpuMs      = 0.f;   // bracket MINUS the GPU fence wait
        float    waitMs     = 0.f;   // the GPU fence wait, shown separately
        float    gameMs     = 0.f;
        float    drawMs     = 0.f;
        float    gpuBusyMs  = 0.f;   // interval union, NOT a wall-clock span
        bool     gpuValid     = false;
        bool     gpuCalibrated = false;
        uint32_t gpuAgeFrames  = 0;
        // ---- Game/render split ----
        // renderPhaseMs is the whole BeginFrame..EndFrame span. joinWait has no meaning on the
        // serial path, so it renders as "--" rather than 0.00.
        float    renderPhaseMs = 0.f;
        float    joinWaitMs    = 0.f;
        bool     joinWaitValid = false;
        uint8_t  renderMode    = 0;   // 0 = serial
        // ---- Owner-active rows (Diag::OwnerAccounting) ----
        // RenderPhase is a span across coroutine suspensions (the frame period under pacing);
        // RenderOwner is the owner-active time of the same frame's render continuations. Shown
        // side by side: the gap is suspended time, not work. Absent, never 0, once the accounting
        // ring no longer holds the frame.
        float    renderOwnerMs    = 0.f;
        bool     renderOwnerValid = false;
        float    tailDrainMs      = 0.f;  // the Animation-boundary blocking drain
        uint32_t parkHookCount    = 0;    // overlap-window collapses this frame

        // ---- Geometry (Renderer::FrameStats) ----
        uint64_t triScene       = 0;  bool triSceneValid   = false;
        uint64_t triMainView    = 0;  bool cpuCullBypassed = false;
        uint64_t triOutline     = 0;
        uint64_t triShadowOnly  = 0;
        uint64_t triTransparent = 0;
        uint32_t drawPackets    = 0;
        uint32_t instances      = 0;

        uint64_t gpuVisTris = 0;  bool gpuVisValid     = false;
        uint32_t gpuVisCmds = 0;  bool gpuVisCmdsValid = false;

        // ---- Meshlet path (Renderer::FrameStats) ----
        uint32_t meshletPath    = 0;      // 0 vs, 1 cpu, 2 indirect
        bool     meshletValid   = false;
        uint32_t meshletVisible = 0;
        uint32_t meshletTested  = 0;
        uint64_t meshletTris    = 0;
    };

    class DebugHud
    {
    public:
        // Unreal's escalation: nothing / stat fps / stat unit / stat unit + stat scenerendering;
        // then three pages read from the engine profiler - stat threads / stat gpu / stat mem.
        // The pages are not supersets of Full: each is the Frame row plus its own rows.
        enum class Level : uint32_t { Off = 0, Fps = 1, Unit = 2, Full = 3,
                                      Threads = 4, Gpu = 5, Mem = 6 };
        static constexpr int kLevelCount = 7;

        struct Cost { uint32_t vertices = 0; uint32_t indices = 0; };

        // A shipped build (no harness) has Off and Fps only: the other levels are a developer's.
        static constexpr int kShippedLevelCount = 2;

        void  CycleLevel(int dir);              // +1 / -1, wraps through the levels this build has
        void  SetLevel(int n);
        Level GetLevel() const { return m_level; }

        void  SetUserScale(float s) { m_userScale = (s > 0.f) ? s : 1.f; }
        float GetUserScale() const  { return m_userScale; }

        // Editor courtesy: hide while the ImGui Profiler window is showing the
        // same numbers. EngineCore cannot see EditorLayer::m_showProfiler, so
        // the editor pushes it in.
        void  SetSuppressed(bool s) { m_suppressed = s; }

        // True when the HUD wants the GPU timestamp profiler running. SyncProfilerWants
        // turns this into a want-count on the device, so closing the editor's Profiler
        // window cannot switch the HUD's GPU row off underneath it.
        bool  WantsGPUProfiler() const
        {
            return !m_suppressed && m_level >= Level::Unit && !m_captureSuppressed;
        }

        // True while a profiler page is showing. SyncProfilerWants then keeps the engine
        // profiler at Stats with a collector running (the GPU verdict and the memory sample
        // come from the collector), unless -profile* already did.
        bool  WantsProfiler() const
        {
            return !m_suppressed && !m_captureSuppressed && m_level >= Level::Threads;
        }

        // The formatted rows of the current page (Lua Debug.HudRows): the HUD
        // itself is pixels, which a scenario driver cannot read.
        int         RowCount() const { return m_rowCount; }
        const char* RowLabel(int i) const { return (i >= 0 && i < m_rowCount) ? m_label[i] : ""; }
        const char* RowValue(int i) const { return (i >= 0 && i < m_rowCount) ? m_value[i] : ""; }

        // Sample the stats and format this frame's rows. Mutating half.
        void Update(const DebugHudStats& s);

        // Appends the rows to @p dl (see the frame contract above). @p indexBudgetRemaining is
        // how many uint16 indices the draw list can still take: the HUD is appended last, so
        // UIPass would truncate it first on exactly the heavy frames it diagnoses; it drops rows
        // instead.
        void Emit(UIDrawList& dl, uint32_t canvasW, uint32_t canvasH,
                  uint32_t indexBudgetRemaining) const;

        // ---- The frame's two calls (UI/DebugHudFrame.cpp) ----
        // Render phase, in the HUD's one legal window (after Renderer::BeginFrame, before the font
        // flush): sample Diag::FrameTiming and the renderer's census, Update, and append within
        // the draw list's remaining index budget. Nothing without a UIPass.
        void EmitFrame(Renderer& renderer, uint32_t viewportW, uint32_t viewportH);
        // Frame-timing publication: hold the GPU timestamp profiler and the engine
        // profiler exactly while the current level asks for them.
        void SyncProfilerWants(IGraphicsDevice& backend);

        Cost EstimateCost() const;

    private:
        static constexpr int kMaxRows  = 24;
        static constexpr int kRowChars = 48;

        static constexpr int kBoundHistory = 60;

        // Full needs 17 rows. Fixed arrays, so Emit() allocates nothing on the frame path and
        // Row() clamps rather than grows.
        void  Row(const char* label, const char* value, Color32 col);
        float ResolveSizePx(uint32_t canvasH) const;
        // The three profiler pages (Update's half for levels Threads..Mem).
        void  UpdateProfilerPage();

        // Whether the HUD holds a GPU-profiler want. Tracked so Add and Release stay paired across
        // level changes: the device counts them, and a missed Release pins the profiler on.
        bool    m_heldGpuWant = false;
        // The HUD's profiler pages hold the engine profiler at Stats (and, when
        // no -profile* flag started one, a collector the HUD started itself).
        bool    m_heldProfiler = false;
        bool    m_ownsCollector = false;

        Level   m_level      = Level::Off;
        bool    m_suppressed = false;
        // Latched in Update() from Sys::HasArtifactDir(): a golden-image run
        // must never composite live FPS digits into the captured PNG.
        bool    m_captureSuppressed = false;
        float   m_userScale  = 1.f;
        int     m_rowCount   = 0;
        char    m_label[kMaxRows][kRowChars]{};
        char    m_value[kMaxRows][kRowChars]{};
        Color32 m_rowCol[kMaxRows]{};

        // stat gpu: the verdicts of the last kBoundHistory frames that have one
        // (the collector derives them 1-8 frames after the frame closed).
        uint8_t  m_bound[kBoundHistory]{};
        int      m_boundCount = 0, m_boundHead = 0;
        uint64_t m_boundNext = ~0ull;
    };

} // namespace UI
