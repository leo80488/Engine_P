#pragma once

// MaterialPreviewWidget - the live material preview in the asset-mode material
// Inspector (and the Material Editor window). It acquires a preview view, sends a
// Preview::Request with the edited material every frame it is visible, and shows
// the tone-mapped image.
//
// A standalone class rather than an EditorLayer function: it owns cross-frame
// state (a view id, a pending release, a debounced size) and is ticked at two
// points in the frame - inside the ImGui pass and inside ProcessPendingActions.
//
// Invariants:
//   1. It never writes main-viewport input state: m_vpMouseDX/DY fly the editor
//      camera.
//   2. The SRV is resolved every frame and never cached: descriptor slots are
//      recycled through a free list and ImGui's own SRVs share the heap, so
//      Renderer::GetPreviewSrv is called at draw time, once.
//   3. The panel size is quantised to 128/256/384/512 and committed only after
//      200 ms without change: a resize rebuilds the target, and unquantised
//      dragging would rebuild it once per pixel.
//
// Editor-only: compiled into EditorCore, never EngineCore (which must not see
// ImGui).

#include "Renderer/Preview/PreviewTypes.h"   // Preview::Shape

#include <cstdint>
#include <string>

// MaterialComponent arrives complete through PreviewTypes.h (Preview::Request
// carries one by value); a forward declaration would fight its alignas(32).
class Renderer;

namespace Editor
{
    class MaterialPreviewWidget
    {
    public:
        // The Inspector's instance (asset mode). The Material Editor window owns
        // another; each holds at most one view.
        static MaterialPreviewWidget& Get();
        MaterialPreviewWidget() = default;

        // The view mode (a Renderer::ViewMode value, ViewDesc::viewMode) and whether
        // the widget draws a combo for it. The combo is off by default (the Material
        // Editor turns it on), so the Inspector's panel - and every pack that captures
        // it - shows none.
        void          SetViewMode(std::uint32_t mode) { m_viewMode = mode; }
        std::uint32_t ViewMode() const                { return m_viewMode; }
        void          SetShowViewModeControl(bool on) { m_showViewMode = on; }

        // Draws the preview block. Call from inside the asset-mode material Inspector,
        // after its Save/Revert/Close row. @p assetPath identifies the open material;
        // a change releases the old view through the deferred path, never inline.
        // @p mat is the Inspector's live edit buffer. It is copied into the request
        // latch every frame (see Preview::Request): the record half may not read
        // editor state, and the copy lets it run the main pipeline's material packer.
        void Draw(Renderer* renderer, const std::string& assetPath,
                  const MaterialComponent& mat);

        // The panel is gone (Close pressed, or no asset open). Queues the release;
        // nothing is destroyed here. Safe to call every frame.
        void NotifyClosed();

        // Frame-boundary drain. Must be called from EditorLayer::ProcessPendingActions,
        // the only legal descriptor-release window. It is also this widget's per-frame
        // tick, which -preview_frames counts.
        void ProcessPendingActions(Renderer* renderer);

        // Hands this panel's preview view back at process shutdown. The ordinary
        // release is driven by "the panel did not draw last frame", which needs a next
        // frame that shutdown does not have. Called once from
        // EditorLayer::ReleasePreviewViews in App's shutdown window (render thread
        // joined, device alive) - the serial-window contract the per-frame release
        // keeps. Idempotent.
        void ReleaseViewsAtShutdown(Renderer* renderer);

        // editor_prefs.ini `preview.enabled` (default 1): the editor half of the
        // preview kill switch. The other half, DX12_PREVIEW=0, lives on the Renderer
        // and disables the layer engine-wide.
        // The value is pushed into the Renderer by ProcessPendingActions, not here:
        // LoadEditorPrefs runs in EditorLayer::OnAttach, long before App calls
        // SetRenderer. Renderer::IsPreviewEnabled then answers for both halves, so
        // Debug.Stats().previewEnabled, the Game twins' fixture check and this widget
        // agree.
        void SetPrefEnabled(bool v)
        {
            m_prefEnabled  = v;
            m_prefPushed   = false;
            // An EXPLICIT choice: the file carried the key, or a user toggled
            // it. Only an explicit value is written back (see PrefIsPersisted).
            m_prefExplicit = true;
        }
        bool PrefEnabled() const    { return m_prefEnabled; }

        // Forces the preview on for a harness run without changing what is saved.
        // editor_prefs.ini is a tracked file the editor rewrites on exit, so going
        // through SetPrefEnabled would dirty the working tree and overwrite a
        // developer's choice; this force is run-scoped.
        void ForcePrefEnabledForHarness()
        {
            if (!m_prefForcedByHarness)
            {
                m_prefForcedByHarness = true;
                m_prefSavedValue      = m_prefEnabled;
            }
            m_prefEnabled = true;
            m_prefPushed  = false;
        }
        // What a save writes: the pre-force value when a harness flag overrode it,
        // the live value otherwise.
        bool PrefEnabledForSave() const
        {
            return m_prefForcedByHarness ? m_prefSavedValue : m_prefEnabled;
        }
        // Whether the key belongs in the file at all. The default is on and the
        // tracked file does not carry the key, so it is written only when it was read
        // from the file or explicitly set - never as an unasked-for diff.
        bool PrefIsPersisted() const { return m_prefExplicit; }

        // -preview_frames=N (harness): start requesting at editor frame N; 0 (the
        // default) means immediately.
        void SetStartFrame(std::uint64_t f) { m_startFrame = f; }

        // ---- Harness size / shape overrides ----
        // Under -hidden the window is SW_SHOWMINNOACTIVE, so io.DisplaySize is 0x0 and
        // the content region is <= 0: every harness run would take the
        // QuantiseSize(256) branch and never reach a resize (SetFixedDimensions +
        // MarkDirty + Compile, which destroys and recreates all seven physical graph
        // textures while a target, a tone-map output and an ImGui descriptor are live
        // against the old ones). A pack cannot drag a panel; it can set these.
        // 0 / unset means "the panel decides".

        // -preview_size=<128|256|384|512>: pins the initial committed size, committed
        // immediately without the 200 ms debounce. The debounce absorbs a mouse drag;
        // on a command-line value it would make the frame a pack asserts on depend on
        // wall-clock speed.
        void SetHarnessSize(std::uint32_t px) { m_harnessSize = px; }
        // -preview_resize=<frame>:<size>: from editor frame @p frame onwards the
        // committed size becomes @p px - one quantised step at a deterministic frame,
        // which exercises the whole recompile path.
        void SetHarnessResize(std::uint64_t frame, std::uint32_t px)
        { m_harnessResizeFrame = frame; m_harnessResizePx = px; }
        // -preview_shape=<sphere|cube|plane|torus>.
        void SetHarnessShape(Preview::Shape s) { m_shape = s; m_harnessShapePinned = true; }
        // Preview::ViewDesc::features bits the harness pins, e.g.
        // Feature::kNoAmbientFill from -preview_parity: the A/B fixture drops the
        // ambient stand-in because the main pipeline forces ambient to zero in every
        // sky mode. No UI on purpose: it makes the preview less like what a user sees,
        // which only a parity measurement wants.
        void SetHarnessFeatures(std::uint32_t f) { m_harnessFeatures = f; }
        // ORs bits into the harness set, for flags that must not clobber an earlier
        // one (-preview_rig=key).
        void AddHarnessFeatures(std::uint32_t f) { m_harnessFeatures |= f; }
        // Feature bits the harness forces off, applied last. -preview_parity clears
        // Feature::kStudioShadow: the parity world's CSM is fitted to the main camera
        // and the studio pass to a 3.2 m box around the subject, so a shot with one
        // and not the other would fail on a shading difference that says nothing
        // about shading. ORing bits on cannot express that, hence a second mask.
        void SetHarnessFeatureClear(std::uint32_t f) { m_harnessFeatureClear = f; }
        // -preview_orbit=<yawDeg>,<pitchDeg> (converted to radians: @p yawRad, @p
        // pitchRad): seeds the orbit as one user drag would; wheel-zoom and drag still
        // work afterwards. It makes the studio shadow measurable: the key light sits
        // ~30 deg off the default camera axis, so at the default framing every
        // self-shadow falls behind its caster; at +115 deg of yaw the light is side-on
        // and a torus shows it.
        void SetHarnessOrbit(float yawRad, float pitchRad)
        { m_yawOffset = yawRad; m_pitchOffset = pitchRad; }
        // -preview_background=<solid|checker|skybox>.
        void SetHarnessBackground(Preview::Background b) { m_background = b; }
        // -preview_mesh=<path.meshlib>[#id]. Empty clears the override.
        void SetHarnessMesh(const std::string& path, std::uint32_t id)
        { m_meshPath = path; m_meshId = static_cast<int>(id); }
        // -preview_shadow=<0|1> / -preview_scene_lighting=<0|1>.
        void SetStudioShadow(bool v)  { m_studioShadow  = v; }
        void SetSceneLighting(bool v) { m_sceneLighting = v; }
        // Pins the orbit zoom. -preview_parity sets
        // PreviewStudioRig::kParityDistanceScale so the subject fills the frame (see
        // that constant for why the parity shot cannot use the panel's framing).
        // Wheel-zoom still works afterwards - this only seeds the value.
        void SetHarnessDistanceScale(float s) { m_distScale = s; }

        // The view id currently held, or ~0u (Preview::kInvalidView-equivalent) when
        // none. For diagnostics only.
        std::uint32_t ViewId() const { return m_viewId; }

    private:
        std::uint32_t m_viewId       = ~0u;
        std::uint32_t m_viewMode     = 0;       // Renderer::ViewMode
        bool          m_showViewMode = false;
        bool          m_releaseQueued = false;
        std::string   m_boundAsset;

        // Background: the colour the preview's HDR scene target clears to behind the
        // shaded subject.
        // Black by default on purpose: the preview HDR target's optimised clear value
        // is {0,0,0,0}, and any other colour raises D3D12's
        // CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE perf warning on every rendered
        // frame. The fourth component is 0 for the same reason (D3D12 compares alpha
        // too; Draw() sends 0 and ColorEdit3 never writes it). float[4] because
        // ImGui's colour helpers expect that shape.
        float         m_color[4]     = { 0.f, 0.f, 0.f, 0.f };

        // ---- View controls ----
        // Which primitive carries the material.
        Preview::Shape m_shape      = Preview::Shape::Sphere;
        // Orbit, stored as offsets from PreviewStudioRig's defaults so "never dragged"
        // is bit-identical to the rig table the A/B parity fixture compares against.
        // Invariant 1: the drag reads ImGui's own item state and writes only these
        // three floats, never m_vpMouseDX/DY (those fly the main editor camera).
        float         m_yawOffset   = 0.f;
        float         m_pitchOffset = 0.f;
        float         m_distScale   = 1.f;

        // Quantised size + the debounce that protects the rebuild (invariant 3).
        std::uint32_t m_sizeCommitted = 256;
        std::uint32_t m_sizePending   = 256;
        double        m_sizePendingAt = 0.0;

        // Backdrop mode. Solid (the default) is the plain clear colour.
        Preview::Background m_background = Preview::Background::Solid;
        // Studio key-light shadow. On by default: it makes a torus read as a torus,
        // and on a sphere it costs one 512^2 depth pass and changes nothing visible
        // (the self-shadow lands where N.L is already 0).
        bool          m_studioShadow  = true;
        // OFF by default, and it must stay that way: it makes the preview a
        // function of the scene clock. See Feature::kSceneLighting.
        bool          m_sceneLighting = false;
        // Mesh override: a .meshlib path and the mesh id inside it. Empty = the
        // primitive named by m_shape.
        std::string   m_meshPath;
        int           m_meshId = 0;

        // Harness overrides - see SetHarnessSize. 0 == unset everywhere.
        std::uint32_t m_harnessSize        = 0;
        std::uint32_t m_harnessFeatureClear = 0;
        // The "no free view" refusal has been logged for this spell of exhaustion.
        // Re-armed by a successful acquire.
        bool          m_noViewLogged       = false;
        // The "studio shadow is the rest shape" notice has been logged for the open
        // material: the studio shadow pass never runs a material's custom VS (except a
        // masked custom material's hook pair; see Draw), so a VS that moves the surface
        // (World-Position-Offset) casts a shadow that does not follow it. The badge is
        // drawn every frame the condition holds; the log line once per (material,
        // condition) edge.
        bool          m_restShapeBadgeLogged = false;
        std::uint64_t m_harnessResizeFrame = 0;
        std::uint32_t m_harnessResizePx    = 0;
        std::uint32_t m_harnessFeatures    = 0;
        // The Shape combo stays writable by hand; this only records that the initial
        // value came from the command line, so the status line can say so.
        bool          m_harnessShapePinned = false;

        bool          m_prefEnabled  = true;
        // See ForcePrefEnabledForHarness / PrefIsPersisted.
        bool          m_prefExplicit        = false;
        bool          m_prefForcedByHarness = false;
        bool          m_prefSavedValue      = true;
        // Cleared by SetPrefEnabled so a later change re-pushes; set once the Renderer
        // has been told (see SetPrefEnabled for why the push is deferred).
        bool          m_prefPushed   = false;
        std::uint64_t m_startFrame   = 0;
        std::uint64_t m_frameTick    = 0;
        bool          m_visibleThisFrame = false;
    };
}
