#pragma once

// Dockable editor panel showing an asset with its own materials on the studio rig: a .meshlib, a
// .ipfb, a model (.iscn) or one Mesh of a model ("<model .iscn>#<MeshKey hex>").
// It draws a Synthetic view with one draw packet per part: Renderer::ResolvePreviewAsset composes
// the asset on the game half into a flat {mesh, material, world matrix} list, each part with its
// own slot in the preview's private material table, resolved through the same by-key runtime the
// material inspector uses (not through a private World).
// Not drawn, and said in the panel because the picture cannot say it:
//   - skinned meshes stand in their rest (bind) pose: plain GBuffer VS, no skeleton, no pose, no
//     skinning buffer (a tooltip says so);
//   - a prefab's particles, trails, decals, lights, surfaces and world UI: their passes are not in
//     the preview graph (an amber line counts them);
//   - parts over Preview::kMaxPreviewParts (the panel says how many were dropped).
// UI invariants:
//   1. It never writes main-viewport input state: the orbit is the panel's own yaw / pitch /
//      distance, driven by an InvisibleButton over the image, never m_vpMouseDX/DY,
//      m_viewportHovered or another viewport input singleton.
//   2. The SRV is re-resolved every frame and never cached.
//   3. Size is quantised and debounced: a resize recompiles the graph, recreating seven physical
//      textures. It uses the material inspector's four square sizes, since a studio subject is
//      framed in a square box.
//   4. It keeps its own per-frame state and resets it every frame instead of inheriting the main
//      viewport's collapsed-tab input latch.
// Editor-only: compiled into EditorCore, never into EngineCore.

#include "Renderer/Preview/PreviewTypes.h"    // Preview::ViewDesc / AssetKind

#include <cstdint>
#include <string>

class Renderer;
class World;

namespace Editor
{
    class AssetPreviewWindow
    {
    public:
        static AssetPreviewWindow& Get();

        // Draws the panel; called from EditorLayer::OnUIRender behind m_showAssetPreview, like every
        // dockable window. @p open is the menu-item flag; the window's X button clears it.
        void Draw(Renderer* renderer, World* world, bool* open);

        // Game-half serial window (EditorLayer::ProcessPendingActions): hands the view back when the panel
        // did not draw last frame, the only legal release point.
        void ProcessPendingActions(Renderer* renderer);

        // Hands the preview view back at process shutdown, where the per-frame release cannot run (it
        // needs a next frame). Called once from EditorLayer::ReleasePreviewViews in App's shutdown window
        // (render thread joined, device alive), the same serial-window contract as the per-frame release.
        // Idempotent.
        void ReleaseViewsAtShutdown(Renderer* renderer);

        // Shows @p assetPath; called by the asset browser's preview action and by -preview_asset. Accepts
        // a .meshlib, a .ipfb, a model .iscn or "<model .iscn>#<MeshKey>"; anything else is refused:
        // returns false, keeps whatever was open and logs why. @p fromHarness logs the refusal as an ERROR
        // instead of a WARNING: a pack that named the wrong file would otherwise run its whole frame budget
        // against whatever happened to be open.
        bool Show(const std::string& assetPath, bool fromHarness = false);

        // The asset currently shown, for a pack or the browser to read.
        const std::string& AssetPath() const { return m_assetPath; }

        // ---- Harness (-preview_asset=<path>[:<px>]) ----
        // Opens the panel on @p path. The path resolves at once (a file, not a scene entity), but the
        // asset may produce no parts for many frames while its mesh library streams, so the never-resolved
        // deadline counts panel draws with zero parts.
        bool HarnessShow(const std::string& path)
        {
            // m_harnessPath is set only on acceptance, so the deadline never quotes a refused path or a
            // previous harness asset that is still open.
            if (!Show(path, /*fromHarness=*/true)) return false;
            m_harnessPath     = path;
            m_harnessResolved = false;
            m_harnessTries    = 0;
            return true;
        }
        // True once the never-resolved deadline expired. Read on the game half
        // (EditorLayer::TickHarnessPreview), which turns it into the exit-12 precondition failure: Draw
        // runs in a render-side thread context and must not post the quit itself.
        bool HarnessDeadlineExpired() const { return m_harnessDeadlineExpired; }
        // Pins the panel size to @p px, bypassing measurement and debounce: under -hidden the content
        // region is always <= 0, so a pack could otherwise never exercise a chosen size. Returns false
        // outside the quantised set, the rule -preview_size keeps.
        bool HarnessSetSize(std::uint32_t px);
        // -preview_asset_deadline=<panelFrames>: moves the never-resolved deadline off its 120-draw
        // default. Only a pack testing the retry itself needs it (a missing asset keeps retrying and
        // recovers when the file appears), so it must outlive the failing window; the pack still exits 12
        // if the asset never resolves. Zero is refused: no deadline is the false pass the counter exists
        // to remove.
        bool HarnessSetDeadlineFrames(std::uint32_t frames);

    private:
        AssetPreviewWindow() = default;

        std::string   m_assetPath;
        Preview::AssetKind m_assetKind = Preview::AssetKind::None;

        std::uint32_t m_viewId = ~0u;
        bool          m_visibleThisFrame = false;
        bool          m_releaseQueued    = false;
        // A world reload invalidated the composition memo; it is dropped on the game half in
        // ProcessPendingActions, not from Draw (a render-side thread context).
        bool          m_recomposeQueued  = false;

        // Committed size, and the debounce state behind it.
        std::uint32_t m_size        = 256;
        std::uint32_t m_sizePending = 256;
        double        m_sizePendingAt = 0.0;
        std::uint32_t m_harnessSize = 0;

        // The panel's OWN orbit, never the viewport's (invariant 1).
        float m_yawOffset   = 0.f;
        float m_pitchOffset = 0.f;
        float m_distScale   = 1.f;

        // Backdrop + rig toggles, mirroring the material inspector's so a user
        // who learned one knows the other.
        Preview::Background m_background   = Preview::Background::Checker;
        bool                m_studioShadow = true;

        // ---- Never-resolved deadline ----
        // Without it a pack that asked for an asset preview and got an empty panel would go green having
        // measured the no-panel case. Counted in panel draws (what this class can see): 120 draws is ~2 s
        // at 60 Hz, and every preview pack runs 200+ frames.
        std::string   m_harnessPath;
        std::uint32_t m_harnessTries    = 0;
        std::uint32_t m_harnessDeadline = 120;   // -preview_asset_deadline
        bool          m_harnessResolved = false;
        bool          m_harnessDeadlineExpired = false;

        // World::Epoch() as of the last Draw. A reload invalidates the mesh
        // descriptor slots the composition resolved to, so the cached
        // composition has to be dropped with it - the same reason
        // CameraPreviewWindow drops its entity selection.
        std::uint32_t m_worldEpoch     = 0;
        bool          m_worldEpochSeen = false;
    };
}
