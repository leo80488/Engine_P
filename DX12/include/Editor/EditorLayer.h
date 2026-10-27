#pragma once

// EditorLayer - the ImGui docking editor: menu bar, DockSpace and every editor panel.
// Layout: MenuBar | top = Hierarchy | Viewport | Inspector | bottom = Content Browser, Output Log.

#include "ECS/ECS.h"
#include "ECS/NotifyTypes.h"            // TimelineComponent (scratch edit target)
#include "ECS/PhysicsComponents.h"      // ColliderComponent (Edit Collider, Create > Collision)
#include "Tools/CollisionMeshBaker.h"   // the collision bake (a member: the popup and Editor.BakeCollision)
#include "Editor/AssetReferenceScanner.h"   // where-used reverse index
#include "Editor/MaterialInstanceRepair.h"  // scan / repair accidental override marks in .imatinst files
#include "Editor/ComponentTransferWindow.h" // Editor::ComponentTransfer::State - entity/prefab component copy
#include "Editor/NotifyTrackEditor.h"
#include "Editor/TimelineEditor.h"      // Timeline::TimelineEditor - read-only bone-curve overlay
#include "Editor/SequencerWindow.h"     // Editor::SequencerWindow - .iseq cinematic authoring
#include "Editor/ImGuiManager.h"
#include "Editor/OutputLogPanel.h"     // the Output Log's mirror + filtered list
#include "System/OwnerTask.h"
#include "Editor/PreviewThumbnailCache.h"    // the material-tile disk cache
#include "Renderer/Preview/PreviewTypes.h"   // Preview::ThumbResult (the tile lane)
#include "Editor/Undo/EditHistory.h"        // Editor::Undo::EditHistory - undo/redo stack
#include "Editor/Undo/EditSink.h"           // Editor::Undo::EditSink - a UI asset tab's own ring
#include "Editor/EditorDocuments.h"         // Editor::DocumentManager - what Save / Undo act on
#include "Editor/EditorUITests.h"           // Reverted: SyncSceneDirtyToHistory's reverse arm
#include "Editor/EditorCameraController.h"  // Editor::FlyInput - the Scene viewport's fly in Play
#include "Editor/Undo/TransformEditAction.h"// LocalTransform-typed undo action
#include "Graphics/RenderTypes.h"
#include "Physics/ChainPhysicsSystem.h" // ChainPhysicsComponent / BoneRestPose (bone-pose authoring)
#include "Editor/ChainParamTree.h"      // ChainParamHooks (the shared parameter tree)
#include "Editor/AssetCatalog.h"  // the Content Browser's search over every folder
#include "Editor/PropertyRow.h"   // the Inspector's RowFilter
#include "Reflection/ReflectionEditor.h"
#include "Reflection/ReflectRegistry.h"
#include "Resource/SkeletonAsset.h"    // SkeletonAsset / ClipAsset (retarget source cache)
#include "Resource/SystemHandles.h"
#include "Resource/MaterialSerializer.h" // Resource::MaterialLoadInfo (asset-mode material editing)
#include "Resource/TextureImporter.h"
#include "Editor/MaterialPropertyPanel.h"   // MaterialPropertyPanel - every control of one MaterialComponent
#include "Editor/MaterialEditorWindow.h"    // MaterialEditorWindow - one .imat / .imatinst per tab
#include "Editor/MaterialSlotRow.h"         // MaterialSlotRow - a material slot drawn as one row
#include "Resource/UnrealSceneImporter.h"
#include "Resource/ModelImportWrite.h"  // the re-import's report (MeshIdentityAdoption) and the .iscn SR record
#include "Scene/MeshAssetRegistry.h"   // Content Browser: a model's Meshes
#include "Scene/MeshExtract.h"         // Extract Mesh...
#include "Platform/Subscription.h"     // ImGui's message filter / capture predicates on the window
#include "imgui/imgui.h"
#include "imgui/ImGuizmo/ImGuizmo.h"

#include <DirectXMath.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IGraphicsDevice;
class Renderer;
namespace Plat { class Window; class KeyboardState; }
class AnimationSystem;
class ScriptSystem;
class DebugDrawSystem;
struct GPUProfiler;
struct ScriptComponent;
namespace Resource { class TextureSystem; class ResourceManager; class AssetManager; class AnimationClipSystem;
                     struct TextureFileInfo; }
namespace DX12Physics { class PhysicsSystem; }
namespace AI { class AISystem; }
class PlayModeController;
enum class EngineMode : uint8_t;
class SystemRegistry;
namespace Gameplay { class GameFlowStateMachine; }
namespace Editor { struct MeshEditorContext; }
namespace Editor { struct SkeletonEditorContext; }
namespace Editor { struct TextureViewerContext; }
namespace Editor { struct UIEditorContext; }
namespace Editor { struct ModuleHost; }

// Editor-facing view of the engine's Edit/Play state: the toolbar's projection of the
// authoritative PlayModeController (EngineCore): Edit -> Stopped, Playing/Stepping ->
// Playing, Paused -> Paused. The inspector's "is Play active" checks read it.
enum class ViewportPlayState { Stopped, Playing, Paused };

class EditorLayer
{
    // The UI tests (EditorUITests.cpp) read and drive editor state through this one friend.
    friend struct EditorUITestAccess;
    friend struct EditorServiceAdapters;   // EditorLayer_Services.cpp

public:
    static constexpr uint64_t FrameSceneTexturePlaceholder = ~uint64_t{0} - 1;

    EditorLayer();
    ~EditorLayer();

    // ---- Lifecycle ----
    void OnAttach();
    void OnDetach() {}
    // Init ImGui DX12+Win32 backends + window message hooks. Call once after SetGraphicsDevice().
    void InitImGuiBackends();
    // Register all built-in component editors + tags. Call once after SetRenderer().
    void RegisterDefaultEditors();

    // ---- Per-frame ----
    void BeginImGuiFrame();
    void EndImGuiFrame(RHI::CommandList cmd);

    bool PrepareImGuiFrameSnapshot(uint64_t frame);
    void DiscardImGuiFrameSnapshot();
    // Render-owner lane on the editor host: the lane's EndImGuiFrameAsync only RECORDS the
    // snapshot, and main gives ImGui its buffers back in CompleteDeferredImGuiSnapshot once
    // the lane has finished (FramePipeline::DrainFrameWork). ImGui stays main-thread only.
    // A snapshot the lane has not TAKEN yet is not given back (the early UI build freezes
    // this frame's snapshot before RenderHalf's entry drain). @p frameLost = no frame will
    // be posted for it any more (a failed frame unwinding, or the Quit drain): give back and
    // forget even that one.
    void SetDeferImGuiSnapshotCompletion(bool on) { m_deferSnapshotCompletion = on; }
    void CompleteDeferredImGuiSnapshot(bool frameLost = false);
    Sys::OwnerTask EndImGuiFrameAsync(RHI::CommandList cmd, uint64_t frame, uint64_t sceneTexture = 0);
    // Resolve the restored/default dock tree before the first rendered frame.
    // Runs ImGui layout only and does not submit draw data to the GPU.
    void PrimeViewportLayout();
    // Draws DockSpace + all panels. Call once per frame.
    void OnUIRender();
    // Run editor actions queued from inside an ImGui frame (Load Scene, ...). Call BEFORE
    // BeginImGuiFrame and Renderer::Render, so resource teardown happens outside any
    // in-flight draw list (see m_pendingLoadScenePath).
    void ProcessPendingActions();

    // ---- Material-tile invalidation and reporting (driven by App) ----
    // A texture was re-imported or a custom shader source changed. @p changedPaths (root-relative)
    // comes from HotReload::WatchService's shader / asset watchers, which App polls at the loop top.
    void NotifyThumbnailDependencyChanged(const std::vector<std::string>& changedPaths);
    // The tile lane's one-shot exit summary. Needs the device for the pool accessors, so App
    // calls it from ShutdownAllSystems while the device is alive. Emits nothing when the
    // lane did no work (packs that never show a tile are unaffected). Reports the last churn
    // pass; BeginThumbnailPass emits the earlier ones at their boundaries.
    void ReportThumbnailSummary(IGraphicsDevice& gfx) const;
    // Hand every asset-browser thumbnail tile back (one RHI::Texture + SRV descriptor per
    // cached tile). App calls it at shutdown beside ReportThumbnailSummary, while the device
    // is alive, which ~EditorLayer cannot promise; otherwise the D3D12 live-object report
    // lists the tiles. The tiles are editor-owned; the preview lane only borrows them
    // (PreviewRenderer::CancelThumbnail).
    // Cancels the lane's in-flight and queued requests first: a request holds a handle by
    // value, and a handle whose pool slot was recycled names another texture.
    void ReleaseThumbnailTiles(IGraphicsDevice& gfx);

    // Close every preview surface at process shutdown: the four panels' views and, through
    // the Renderer, the in-game PiP's. A panel normally hands its view back when it did not
    // draw last frame, which needs a next frame; with this, `live == 0` at teardown is an
    // invariant PreviewRenderer's destructor checks.
    // Called from App::ShutdownAllSystems beside ReleaseThumbnailTiles (render thread joined,
    // device alive). Drains the release queue itself: ServicePreviewReleases will not run
    // again.
    void ReleasePreviewViews(Renderer& renderer);

    // How many times ProcessPendingActions has run: the editor's own frame number, on the
    // harness tick's clock (TickHarnessPreview runs only from ProcessPendingActions) but
    // present in every configuration (Retail has no m_harnessFrameTick). Starts at 1 on the
    // first call.
    std::uint64_t EditorFrame() const { return m_editorFrame; }

    // ---- Harness hooks ----
    // Entry points of the editor-side -preview_* flags: Editor::PreviewHarness parses each flag
    // and calls these (the game-capable ones are Preview::HarnessGame's). Thin forwards,
    // deliberately: no behaviour lives here.
    // -preview_material=<path>: same effect as double-clicking the asset.
    void HarnessOpenMaterialAsset(const std::string& path);
    // -preview_frames=N: the preview widget starts requesting at editor frame N
    // (0 = immediately). Frames are counted in ProcessPendingActions.
    void HarnessSetPreviewStartFrame(std::uint64_t frame);
    // HarnessOpenAssetPreview: -preview_asset, open the Asset Preview panel on a .meshlib /
    // .ipfb; false when @p px is outside the quantised size set.
    // HarnessOpenCameraPreview: open the Camera Preview panel on a named scene camera at an
    // optional pinned size (0,0 = the panel's own measurement); the panel resolves the name
    // every frame until it matches (the scene loads asynchronously).
    // HarnessSetPreviewSize: -preview_size=<128|256|384|512> pins the committed panel size
    // (otherwise always 256 under -hidden, where io.DisplaySize is 0x0); false, changing
    // nothing, for a value outside the quantised set.
    bool HarnessOpenAssetPreview(const std::string& assetPath, std::uint32_t px);
    void HarnessOpenCameraPreview(const std::string& entityName,
                                  std::uint32_t w, std::uint32_t h);
    bool HarnessSetPreviewSize(std::uint32_t px);
    // -preview_resize=<frame>:<size>: commit @p px at editor frame @p frame through
    // SetFixedDimensions + MarkDirty + Compile, which destroys and recreates all seven of the
    // preview graph's physical textures. False for a size outside the quantised set.
    bool HarnessSetPreviewResize(std::uint64_t frame, std::uint32_t px);
    // -preview_shape=<sphere|cube|plane|torus>. False, changing nothing, for an unrecognised
    // name.
    bool HarnessSetPreviewShape(const std::string& name);
    // -preview_background=<solid|checker|skybox>. False, changing nothing, for an
    // unrecognised name.
    bool HarnessSetPreviewBackground(const std::string& name);
    // -preview_mesh=<path.meshlib>[#meshId]. False when the path is not a .meshlib or the id
    // does not parse: the preview falls back to the primitive either way, so an ignored flag
    // would pass a pack that previewed a sphere.
    bool HarnessSetPreviewMesh(const std::string& spec);
    // -preview_orbit=<yawDeg>,<pitchDeg>: seed the panel's orbit, the only way a pack can put
    // the key light side-on (MaterialPreviewWidget::SetHarnessOrbit says why the shadow needs
    // that). False for anything but two comma-separated decimals.
    bool HarnessSetPreviewOrbit(const std::string& spec);
    // -preview_rig=<full|key>: `key` drops the clustered fill and back lights (the rollback
    // lever for those lights, and the control arm of the back-light transmission measurement).
    // False for anything else.
    bool HarnessSetPreviewRig(const std::string& name);
    // -preview_shadow=<0|1>: the studio key-light shadow pass.
    void HarnessSetPreviewShadow(bool on);
    // -preview_scene_lighting=<0|1>. Non-deterministic by construction
    // (Preview::Feature::kSceneLighting): a pack that sets it must not compare images.
    void HarnessSetPreviewSceneLighting(bool on);

#ifdef WITH_HARNESS

    // The fixture and parity flags. WITH_HARNESS, like Editor::PreviewHarness, which drives them:
    // Engine.Cpp.props defines the macro for every configuration except Retail and EditorCore
    // imports it, so these entry points, the private drivers below, their members and the
    // .cpp's includes leave the shipped editor with the harness. A configuration macro is
    // legal here where WITH_EDITOR would not be (see Engine.Cpp.props).

    // The surface bits of a -preview_close_at / -preview_open_at step's mask
    // (HarnessSetPreviewToggleFrames).
    static constexpr std::uint8_t kPreviewSurfaceMaterial = 1u;
    static constexpr std::uint8_t kPreviewSurfaceCamera   = 2u;
    static constexpr std::uint8_t kPreviewSurfaceAsset    = 4u;
    static constexpr std::uint8_t kPreviewSurfaceSplit    = 8u;
    static constexpr std::uint8_t kPreviewSurfaceAll      = 15u;

    // -preview_fixture=<name>[,<name>...] + -preview_fixture_every=<N>. Each name selects a
    // MaterialComponent synthesised in code (Preview::MakeFixtureMaterial), installed in the
    // asset-mode material buffer as if an .imat carrying it had been opened. Several names
    // cycle: fixture i owns editor frames [start + i*N, start + (i+1)*N) and writes
    // preview_<name>.png near the end of its window, one picture per shading model.
    // Synthesised because most shading models have no .imat in the tree (its .imat files are
    // Standard or Toon; PreviewFixtures.h says why). False, installing nothing, if any name is
    // unknown: a partly honoured list would run a shorter schedule than the pack asserts.
    bool HarnessSetPreviewFixtures(const std::vector<std::string>& names,
                                   std::uint64_t startFrame,
                                   std::uint32_t everyFrames);

    // -preview_shadow_alpha=<0|1>[,<0|1>...]: the studio shadow pass's alpha-test pixel stage,
    // per fixture window; the last entry repeats for every window the list does not reach,
    // so a single `0` or `1` applies to the whole run.
    // It gives pixel evidence beside the PSO-level `alphaTested=N` log field, and works per
    // window because expect.imagePair compares two captures of the same run, so "clip" and
    // "no clip" must be two windows of one schedule. Name the same fixture twice and flip this
    // between them, e.g. `-preview_fixture=maskedshadow,maskedshadow -preview_shadow_alpha=1,0`:
    // the second occurrence captures as preview_<name>_2.png (ServiceFixtureShot).
    void HarnessSetPreviewShadowAlpha(const std::vector<std::uint8_t>& perWindow);

    // -preview_shadow_win=<0|1>[,<0|1>...]: the studio shadow pass itself, per fixture window,
    // the last entry repeating as for -preview_shadow_alpha.
    // At the default framing the studio shadow is a visual no-op (every occluded point is
    // already at N.L <= 0), a luma band cannot see the difference, and -preview_shadow is a
    // start-up flag, so only per-window switching forms an imagePair. Pair it with
    // -preview_key_light to ask whether the pass moves pixels where a shadow can land.
    // Editor::PreviewHarness refuses it together with -preview_shadow: one sets the bit at
    // start-up, the other per window, so the pack's stated shadow state would be whichever ran
    // last.
    void HarnessSetPreviewShadowWindows(const std::vector<std::uint8_t>& perWindow);

    // -preview_edit_at=<frame>: at editor frame @p frame, change one scalar of the open
    // material buffer (roughness) the way dragging the Inspector slider does (SetEditDirty,
    // which changes HashMatParams), then assert that a preview render was recorded on a later
    // frame within the one-frame budget ("drag a slider -> the picture updates within one
    // frame").
    void HarnessSetPreviewEditAt(std::uint64_t frame);

    // -preview_asset_edit=<.imat path>: aim the -preview_edit_at one-shot at an open asset
    // preview's part material and drive it through a real Resource::SaveMaterial, not the
    // inspector's in-memory buffer. An asset preview has no such buffer (its parts hold
    // resolved MaterialComponent values composed from the files), so only a file write, as
    // the editor's Save button does, shows whether a save reaches an open Asset Preview.
    // The path is seeded at the first harness tick with a dull "before" material and
    // overwritten at the edit frame with a bright one, so the pictures do not depend on the
    // file's contents: point it at a fixture, never at a shipped asset.
    void HarnessSetPreviewAssetEditMaterial(const std::string& matPath);

    // -preview_parity=<fixture>: build the A/B parity fixture - the same synthesised material
    // on the same primitive under the same studio rig, once through the preview graph and
    // once through the main pipeline, compared pixel for pixel.
    // This sets up the preview side (the fixture installed; the ambient stand-in
    // Preview::Feature::kNoAmbientFill dropped, since the main pipeline forces ambient to zero
    // in every sky mode; the studio shadow off; the subject framed at
    // Preview::kParityDistanceScale), hides the editor grid and the light icons (drawn into
    // HDR scene colour, they would appear only in the main capture) and queues the World for
    // BuildParityWorld. Editor::PreviewHarness pins the main camera (PinParityCamera) and the
    // square viewport. False for an unknown fixture name.
    bool HarnessSetPreviewParity(const std::string& fixtureName);

    // -preview_lights_parity=<baseline|lights>: build the secondary-view lights fixture world:
    // two planes covering every pixel, a camera entity named
    // Preview::kLightsParityCameraName, a StaticSkybox environment, no directional light and,
    // for `lights`, the punctual set Preview::kLightsParityLights. Hides the editor grid and the
    // light icons, as HarnessSetPreviewParity does.
    // Not a variant of HarnessSetPreviewParity: that one tests the main pipeline against the
    // synthetic lane (own studio rig, no SkyboxPass); this one against the WorldCull lane (a
    // second camera on the same world, its own SkyboxPass, a copied LightCB, a borrowed CSM).
    // It installs no fixture material and touches no preview widget: the picture under test
    // is the PiP (-preview_pip).
    // `baseline` (zero punctual lights on either side) is the pair's floor and is required to
    // read `lights`. False for an unknown arm name.
    bool HarnessSetLightsParity(const std::string& arm);

    // -preview_churn=<dir>: drive asset-browser material tiles over every folder under @p dir
    // that holds .imat / .imatinst, one folder at a time, through the browser's own path
    // (repoint m_currentDir, force a rescan), so ScanAssets' snapshot / restore / orphan
    // release and the eviction sweep run as for a scrolling user. Every entry counts as
    // visible: under -hidden every ImGui window has a degenerate rect and the real visibility
    // test is never true.
    // False, doing nothing, when @p dir holds no material: a churn of zero assets is a false
    // pass.
    bool HarnessSetPreviewChurn(const std::string& dir);
    // -preview_churn_passes=<N>: how many times the churn walks its folder rotation. Each pass
    // logs its own summary with per-pass numbers, so one run asserts both the cold pass
    // (render every material, write every tile) and the warm one (render nothing, serve every
    // tile from disk).
    void HarnessSetPreviewChurnPasses(std::uint32_t passes);
    // -preview_thumb_cache=clear: delete the whole cache directory so a pack can force a cold
    // run.
    void HarnessClearThumbnailCache();
    // -preview_thumb_evict_ms=<ms>: shorten the 8 s off-screen eviction delay so a churn
    // really evicts and reloads.
    void HarnessSetThumbnailEvictMs(std::uint32_t ms);
    // -preview_touch_at=<frame>:<rootRelativePath>: at editor frame @p frame, bump the file's
    // mtime and deliver the dependency-changed event for it without waiting for the OS
    // watcher (Editor::PreviewHarness's flag block says why). @p path is root-relative
    // ("asset/.../x.itex"), the form the cache's sidecars store.
    bool HarnessSetPreviewTouchAt(std::uint64_t frame, const std::string& path);
    // True on exactly the frame the touch is due, with @p outPath set to the root-relative
    // path. Editor::PreviewHarness::DeliverDue owns the watcher-relative normalisation and the
    // delivery.
    bool HarnessTakeDuePreviewTouch(std::string& outPath);
    // HarnessSetAssetPreviewDeadline: -preview_asset_deadline=<panelFrames>, forwarded to
    // AssetPreviewWindow::HarnessSetDeadlineFrames (Editor::PreviewHarness does not include
    // that header). False when @p frames is 0.
    // HarnessSetPreviewAppearAt: -preview_asset_appear_at=<frame>:<srcRootRel>:<dstRootRel>,
    // at editor frame <frame> copy an existing asset onto a path that does not exist. The
    // recovery case: a preview of a missing `.meshlib` must notice when the file turns up (why
    // the failure is retried, not memoised). Only a mid-run copy reaches it: a pack that ships
    // the file is never in the failed state, and deleting a shipped file would leave the tree
    // modified if the run died. The copy is removed at shutdown.
    // No watcher is told, deliberately: the panel must come back on its own retry clock
    // (-preview_touch_at, by contrast, tests the watcher path).
    bool HarnessSetAssetPreviewDeadline(std::uint32_t frames);
    bool HarnessSetPreviewAppearAt(std::uint64_t frame,
                                   const std::string& srcPath,
                                   const std::string& dstPath);
    // -preview_asset_rewrite_at=<frame>:<srcRootRel>:<dstRootRel>: at editor frame <frame>,
    // overwrite an existing file in place with another one's bytes; the original is restored
    // at shutdown.
    // The content-change case the retry clock cannot reach: a `.meshlib` that opens fine but
    // whose content changed lives entirely on the watcher -> InvalidatePreviewAsset route
    // (PreviewAssetSource retries only on !libOk). Pair it with -preview_touch_at on the same
    // path at a later frame for the delivery half; two flags keep the mechanism under test
    // visible in the pack's arguments.
    // Editor::PreviewHarness keeps the original bytes in memory and in a restore sidecar the
    // next boot puts back, so a tracked fixture can be the target even if the run dies. The
    // destination must exist: a missing one is refused, not created, because the appear case
    // would look identical in the log.
    bool HarnessSetPreviewRewriteAt(std::uint64_t frame,
                                    const std::string& srcPath,
                                    const std::string& dstPath);
    // -preview_bake_restore_at=<frame>[:<meshlib>]: drive the collision baker's preview LOAD
    // at @p frame and its RESTORE a few frames later, through the two intents the GUI buttons
    // record - the only route to Renderer::OnWorldClear(idsPreserved=false,
    // worldSurvives=true). @p libPath must be an existing `.meshlib` (the caller refuses the
    // run otherwise: a failed load takes the restore's early-out and executes nothing). False
    // for a zero frame.
    bool HarnessSetBakeRestoreAt(std::uint64_t frame, const std::string& libPath);
    // True on exactly the frame the rewrite is due. Editor::PreviewHarness::DeliverDue owns the
    // on-disk half (resolve, back up, copy), as for the appear.
    bool HarnessTakeDuePreviewRewrite(std::string& outSrc, std::string& outDst);
    // True on exactly the frame the appear is due; Editor::PreviewHarness::DeliverDue owns the
    // on-disk resolution and the copy.
    bool HarnessTakeDuePreviewAppear(std::string& outSrc, std::string& outDst);
    // -preview_asset_verify=<path>[,<path>...]: the asset preview's order self-check. For each
    // `.ipfb` / `.iscn` it spawns what the preview composes to through the real
    // SceneInstanceLoader::Load, walks Resource::CollectPrefabSubtreeDFS (which
    // PrefabSerializer::LoadPrefab keys per-part overrides through), compares the (DFS index
    // -> mesh) mapping and each part's world matrix, then destroys the spawn. One line per
    // path: `[PREVIEW] asset order verify: parts=N mismatches=M matrixMismatches=K`.
    // Deferred to a frame: it needs a live World, a MeshLibrary and the game half. Any
    // mismatch is a precondition failure (exit 12): overrides on the wrong parts still draw a
    // plausible picture.
    void HarnessSetAssetVerify(const std::string& spec);
    // -preview_split=<cam1>,<cam2>[,<cam3>[,<cam4>]]:<w>x<h>[:<tick>]: open the Split
    // Viewports panel with one cell per named scene camera; the panel resolves each name every
    // frame until the scene has loaded. False when the cell count is not 2..4, the size is not
    // a quantised one the panel can reach by hand, or the tick is out of range
    // (Editor::PreviewHarness then refuses the run).
    bool HarnessOpenSplitViewport(const std::vector<std::string>& camNames,
                                  std::uint32_t w, std::uint32_t h,
                                  std::uint32_t tick);
    // -preview_split_cams=<baseEntityName>:<n>: once the base entity is alive, spawn @p n
    // cameras `SplitCam0..n-1` at its world position with yaws evenly spaced over 360 deg
    // (four -> 0/90/180/270).
    // Editor packs cannot use a Lua driver (scripts are Play-only) and AIBench ships one
    // camera, so every cell would show the same camera; rotated cameras make each cell's
    // frustum disjoint from the main one, which the per-view `secondary` counts measure.
    // False for a bad spec; the spawn waits for a frame with a live World.
    bool HarnessSetSplitCameras(const std::string& baseName, std::uint32_t n);
    // Destroy the SplitCam* entities this fixture spawned, through Hierarchy::DestroySubtree
    // and without marking the scene dirty; returns how many. Safe when nothing was spawned.
    // Editor::PreviewHarness::Shutdown calls it after ReleaseThumbnailTiles, while the World is
    // still whole, so the cameras do not leak into process exit. The epoch boundary calls it
    // too, with @p reason naming which: its guards (IsAlive, then "still named SplitCam*")
    // reject everything after an ids-destroyed boundary (Scene.Load: the list is just dropped)
    // and pass after an ids-preserved one (World::AdoptState / Play -> Stop: the previous
    // tenancy is taken back, not orphaned and duplicated). The spawn latch re-arms on a
    // World::Epoch() change, so the cameras return after a scene reload.
    std::uint32_t HarnessReleaseSplitCameras(const char* reason = "shutdown");
    // -preview_split_hover=<cell>: pin one split cell as hovered, the only way a pack reaches
    // the scheduler's priority seam (under -hidden ImGui::IsItemHovered() never fires). False
    // for a cell the panel does not open - every cell when -preview_split was not given, so
    // the flag cannot be used alone.
    bool HarnessSetSplitHover(std::uint32_t cell);
    // One step of -preview_split_layout_at=<f>:<l>[,<f>:<l>...]: switch the split panel's
    // layout (0 = 1x2, 1 = 2x2) at a chosen panel frame; call once per step. Shrinking checks
    // that the cells that leave hand their views back (else they hold an HDR slot and a
    // round-robin turn for nothing); re-expanding checks that a returning cell re-resolves
    // its camera by name and renders again. False for frame 0, a layout outside 0..1, a
    // repeated frame, or more steps than the panel stores.
    bool HarnessSetSplitLayoutAt(std::uint64_t frame, int layout);
    // -preview_split_resize_at=<frame>:<w>x<h>: move the split panel's pinned cell size at a
    // chosen panel frame - the only way a pack reaches the whole-lane move with two or more
    // live cells. False for frame 0, a size the panel's quantiser would change, or a panel
    // -preview_split did not open.
    bool HarnessSetSplitResizeAt(std::uint64_t frame, std::uint32_t w,
                                 std::uint32_t h);
    // -preview_match_size_at=<frame>: drive the split panel's "Match size" adoption at a
    // chosen panel frame (the button is unreachable under -hidden: no cursor, and the size
    // pin rewrites the cell size every Draw). False for frame 0 or a panel -preview_split did
    // not open.
    bool HarnessSetSplitMatchSizeAt(std::uint64_t frame);
    // -preview_camera_resize=<frame>:<w>x<h>: move the Camera Preview panel's pinned size at a
    // chosen panel frame (a drag-resize without the wall clock). The sole live camera view
    // re-fixes the WorldCull lane and the world graph recompiles once; with a second live
    // camera view at another size it is refused instead. False for frame 0 or a zero extent.
    bool HarnessSetCameraPreviewResize(std::uint64_t frame, std::uint32_t w,
                                       std::uint32_t h);
    // -preview_camera_close_at=<frame>: close the Camera Preview panel at a chosen panel frame;
    // its view goes back at the next frame boundary - the one event that releases a share of
    // the WorldCull lane while another panel still asks for a size it was refused. False for
    // frame 0 or a panel -preview_camera did not open.
    bool HarnessSetCameraPreviewCloseAt(std::uint64_t frame);

    // -preview_close_at=<f>[:<which>][,...] / -preview_open_at=<f>[:<which>][,...]: schedule
    // panel closes and reopens at editor frames, which makes the idle-teardown sweep reachable
    // from a pack (no title-bar X or menu click under -hidden).
    // A close takes away the surfaces in the step's mask - the asset-mode material inspector,
    // Camera Preview, Asset Preview and Split Viewports - and remembers which of those it
    // took. It does not touch the thumbnail lane (no panel: it idles out once the browser
    // stops queueing tiles). The mask lets the material inspector close while the camera
    // panel keeps rendering (m_live torn down, m_world still compiled), and the split panel
    // close without the camera panel. Default is every surface.
    // An open mask must be a subset of the closes before it: `-preview_open_at=260:all` after
    // `-preview_close_at=150:material` is refused (exit 12), since it would close the camera
    // panel and log a reopen. At run time an open restores only the surfaces a close still
    // holds (m_previewClosedPending); the reopen line reports `which=` and `restored=`.
    // Both refuse frame 0 (the editor frame counter has not ticked yet), duplicate frames, an
    // empty mask and more than kMaxPreviewToggleSteps entries; Editor::PreviewHarness turns
    // false into exit 12. True when the schedule was installed.
    // @p steps is (editor frame, surface mask), the mask built from the kPreviewSurface*
    // constants; @p open selects the open schedule over the close one.
    bool HarnessSetPreviewToggleFrames(
        const std::vector<std::pair<std::uint64_t, std::uint8_t>>& steps,
        bool open);
#endif  // WITH_HARNESS

    // The Profiler window's state (EditorLayer_Profiler.cpp). Public only so
    // that file's free helper functions can name it.
    struct ProfilerPanelState;

    // How SelectEntity / SelectAsset apply a click to the entity / asset selection.
    enum class SelectMode { Replace, Add, Toggle, Range };

    // What the viewport skeleton overlay drew in a frame (m_skelOverlay).
    struct SkeletonOverlayStats
    {
        Entity   entity  = NullEntity;   // the entity whose skeleton / sockets were drawn
        uint32_t bones   = 0, lines = 0, sockets = 0;
        uint64_t frame   = 0;            // the editor frame it was drawn in
    };

    // The authored-rest-pose fields CaptureBoneRestPose snapshots (it says why not more).
    struct BonePoseBaseline
    {
        std::vector<BoneRestPose> pose;
        bool  enabled = true;
        float weight  = 1.f;
        bool  valid   = false;
    };

    // What an asset item's drag publishes (AssetDragPayloadFor).
    struct AssetDragPayload
    {
        bool        valid  = false;   // false = this item is not draggable
        bool        multi  = false;   // ASSET_MOVE (folder / multi-selection)
        const char* id     = nullptr; // the payload id actually published
        const char* prefix = "";      // drag-preview label prefix
    };

    // ---- System injection ----
    void SetGraphicsDevice(IGraphicsDevice* gfx)             { m_gfx = gfx; }
    void SetRenderer(Renderer* renderer)                     { m_renderer = renderer; }
    // Before InitImGuiBackends: that is where ImGui's message filter and
    // capture predicates subscribe to the window.
    void SetWindow(Plat::Window* w)                          { m_window = w; }
    void SetAssetManager(Resource::AssetManager* assetMgr)   { m_assetMgr = assetMgr; }
    void SetGPUProfiler(GPUProfiler* p)                      { m_gpuProfiler = p; }
    void SetCPUFrameTime(float ms)                           { m_cpuFrameMs = ms; }
    void SetAnimationClipSystem(Resource::AnimationClipSystem* acs) { m_animClipSys = acs; }
    void SetAnimationSystem(AnimationSystem* sys)            { m_animSys = sys; }
    // Injected so the "Bake Collision Meshes" tool can flush PhysicsSystem's trimesh shape
    // cache after a re-bake. Optional: null makes the feature a no-op.
    void SetPhysicsSystem(DX12Physics::PhysicsSystem* phys)  { m_physicsSys = phys; }
    // Injected so the "Build NavMesh" tool can Build / Save / Load the active navmesh.
    // Optional: null makes the feature a no-op.
    // Injected so the AIComponent Inspector can resolve treePath -> BTAsset via AcquireTree.
    // Optional: without it the field is read-only.
    void SetAISystem(AI::AISystem* ai)                       { m_aiSys = ai; }
    // Injected so the Script Inspector can read a Logic script's `exposed` variable schema
    // and push edited values onto the live Lua instance during Play. Optional: without it the
    // exposed-var UI is hidden.
    void SetScriptSystem(ScriptSystem* sys)                  { m_scriptSys = sys; }
    // Editor-only debug-visual toggles (categories). The viewport toolbar's Show dropdown drives
    // them.
    void SetDebugDrawSystem(DebugDrawSystem* sys);
    // Authoritative Edit/Play state machine (EngineCore). The viewport toolbar drives it via
    // Request*(); App reads it to gate the scheduler. App wires one; until then the toolbar
    // uses the local m_playState.
    void SetPlayModeController(PlayModeController* c)        { m_playModeController = c; }
    // Injected so the Game Flow panel can read the state machine's snapshot / diagnostics /
    // history and (in Play only) request a switch. Optional: the panel says "No
    // GameFlowStateMachine attached." when null.
    void SetGameFlow(Gameplay::GameFlowStateMachine* flow)   { m_gameFlow = flow; }
    // Injected so the Game Flow panel can list the C++ half of each SystemSet's membership
    // (the Lua half comes from m_scriptSys). Optional: without it the Members column shows Lua
    // systems only, and says so.
    void SetSystemRegistry(SystemRegistry* reg)              { m_systemRegistry = reg; }
    void SetResourceSystems(Resource::TextureSystem* texSys, Resource::ResourceManager* rm)
    {
        m_textureSys  = texSys;
        m_resourceMgr = rm;
    }
    // Root directory scanned for converted engine assets (.itex, .ishdr, ...).
    void SetAssetDirectory(const std::string& dir)
    {
        m_assetDir   = dir;
        m_currentDir = dir;
        m_needsRescan = true;
    }

    // ---- ECS connection ----
    // (Re)binds an entity-destroy listener that clears a stale selection / highlight when the
    // selected entity is destroyed at run time (a Lua script, LifetimeSystem). App calls it
    // every frame; a no-op unless the World pointer changes.
    void SetWorld(World* world);
    void SetRenderView(const RenderView& view)   { m_renderView = view; }

    // ---- Boot-scene adoption ----
    // App boots the startup scene through SceneManager before the editor is wired, so the
    // editor's LoadScene bookkeeping never ran for it. This hands over the scene's companion
    // paths (@p ppcPath post-process config, @p sceneScript Lua scene script, @p sceneSystems its
    // scene-scoped Lua Systems) so File > Save Scene keeps those bindings. @p scenePath
    // (optional) is the .iscene it came from; without it the first save of a booted scene opens
    // Save As.
    void AdoptLoadedScene(const std::string& ppcPath, const std::string& sceneScript,
                          const std::vector<std::string>& sceneSystems,
                          const std::string& scenePath = std::string{})
    {
        m_postProcessConfigPath = ppcPath;
        m_sceneScriptPath       = sceneScript;
        m_sceneSystems          = sceneSystems;
        if (!scenePath.empty()) m_currentScenePath = scenePath;
        // A freshly adopted scene is exactly what is on disk.
        MarkSceneClean();
    }

    // ---- Unsaved changes / current scene ----
    // True when the authoring world holds edits not on disk yet. Fed from the undo record
    // points (PushEdit / EditUndo / EditRedo), so "can be undone" and "needs saving" cannot
    // drift apart. Cleared by a successful save and by a fresh load.
    bool HasUnsavedChanges() const { return m_sceneDirty; }

    // External (DCC) write under asset/: HotReload::WatchService's loop-top drain calls this
    // (App's hook) so the browser grid rescans without a manual Refresh. The service itself
    // reloads a changed .itex in place, re-resolves the materials a changed .imat feeds and
    // refreshes anim graphs / sets, .iui assets and face mappings; loaded meshes are not
    // reloaded.
    void NotifyExternalAssetChange() { m_needsRescan = true; }
    // The .iscene the editor considers open. Empty = untitled (never saved).
    const std::string& CurrentScenePath() const { return m_currentScenePath; }

    // ---- Application-exit interception ----
    // WM_CLOSE is handled by Window / App: App's close handler vetoes the close when
    // RequestExit() returns false, and the main loop posts the quit once
    // ConsumeExitApproved() is true. RequestExit returns true at once when neither the scene
    // nor a UI Editor asset tab has unsaved edits; otherwise it raises the Save / Discard /
    // Cancel modal New / Load Scene use and returns false, and the answer surfaces through
    // ConsumeExitApproved().
    bool RequestExit();
    bool ConsumeExitApproved();

    // App's Play snapshot hook calls this first: a UI rect gesture still held (the main
    // viewport's Rect Tool, the UI Editor's drag or nudge) becomes its undo step while undo
    // still records, instead of being dropped once Play has started.
    void CommitGesturesForPlay();

    // Files dropped on the window from the OS (the FilesDropped window event, which App
    // dispatches at the loop top), @p x / @p y in client pixels. Only queued: the next frame's
    // pending actions route them (ProcessDroppedFiles -> HandleDroppedFiles, which
    // Editor.DropFiles calls directly).
    void QueueDroppedFiles(std::vector<std::string> paths, int x, int y);

    // ---- Viewport state (driven by App each frame) ----
    void SetSceneTextureId(unsigned long long texId);
    // F11: render viewport over the whole window (hide docking UI).
    void SetViewportFullscreen(bool fullscreen)  { m_viewportFullscreen = fullscreen; }
    // Last Viewport panel content size; (0,0) before first frame.
    void GetViewportSize(unsigned int& outWidth, unsigned int& outHeight) const;
    // Screen-space top-left of Viewport content. Used to translate window-space
    // mouse coords into panel-relative pixels for runtime UI hit-testing.
    void GetViewportMin(float& x, float& y) const { x = m_viewportMin.x; y = m_viewportMin.y; }
    void GetViewportMouseDelta(float& dx, float& dy) const { dx = m_vpMouseDX; dy = m_vpMouseDY; }
    bool IsViewportRightDragging() const         { return m_viewportRightDragging; }
    // Whether the mouse is over the viewport image. Gamepad look is gated on
    // this so a resting stick cannot rotate the scene while the user is
    // working inside a docked panel.
    bool IsViewportHovered() const               { return m_viewportHovered; }
    // "An ImGui widget OTHER THAN the viewport's own click surface owns the
    // mouse." Callers must use this instead of ImGui::IsAnyItemActive() when
    // deciding whether a click over the scene image belongs to them: the docked
    // viewport's "##viewport" InvisibleButton takes ImGui's ActiveId on the
    // mouse-DOWN edge and holds it for the whole press, so the raw query is true
    // for exactly the clicks that were meant to reach the scene.
    bool IsOtherImGuiItemActive() const          { return m_viewportOtherItemActive; }
    // The editor's Canvas rect tool owns the pointer (dragging, or armed over a
    // handle). App must withhold the mouse from the running game's Canvas while
    // this is true, or authoring a button's rect in Play mode also CLICKS it.
    bool IsUIRectToolClaimingMouse() const       { return m_uiRectToolClaimsMouse; }
    // ---- Game View: the main pipeline's route (App, once a frame; EditorLayer_GameView.cpp) ----
    // Decides this frame's route (Editor::GameView) after the Edit / Play transition, hands the main view's
    // owner to the renderer and, while the Game View is the main pipeline's panel, its extent into
    // @p viewportW / @p viewportH. @p fullscreen = App's F11 state.
    void UpdateMainViewRoute(bool simulating, bool fullscreen, unsigned int& viewportW, unsigned int& viewportH);
    // Play, and the Scene viewport shows the editor camera through the main pipeline (the Game View is open
    // but hidden): App's camera seam renders the editor camera and the HUD is not painted.
    bool MainViewObservesPlay() const;
    // The Scene viewport's own fly input for the editor camera in Play; empty unless the Scene shows it.
    void GetSceneFlyInput(Editor::FlyInput& out) const;
    // The game's mouse look this frame: the Scene viewport's right drag (Edit, the single-view editor), the
    // Game View's (its route), none while the Scene shows the editor camera.
    void GetGameMouseLook(bool& held, float& dx, float& dy) const;
    // On the Game View's route the game Canvas's pointer comes from its picture: its top-left (@p minX,
    // @p minY), Canvas pixels per window pixel (@p scaleX, @p scaleY) and whether the host owns the pointer
    // (off the picture, on a bar, another widget's drag). The Scene showing the editor camera owns it; the
    // other routes leave every argument as it is.
    void MapGamePointer(float& minX, float& minY, float& scaleX, float& scaleY, bool& hostOwnsPointer) const;
    // The game's input on the Game View's routes this frame.
    struct GameInputGate
    {
        bool hostFocus      = true;    // the game may read keys, buttons, the pad and look
        int  quarantineKey  = 0;       // a key the editor took (the Esc that released the capture)
        bool captured       = false;   // the game holds the mouse: hidden, clipped to the picture, raw look
    };
    // App, before any input is sampled: the gate from the Game View's focus and capture. @p keyboard is the
    // window's (its Esc / Shift+F1 edges release the capture before the game sees them); @p gameWantsCapture:
    // the game flow asks for a captured cursor.
    GameInputGate UpdateGameInputGate(const Plat::KeyboardState& keyboard, bool gameWantsCapture);
    // The window's cursor this frame. Edit mode: the editor's (shown, free). Play on the Game View's routes:
    // the Game View's capture, clipped to its picture. Play on the single-view route (the Game View closed,
    // F11): the game flow's wish (@p flowWantsCapture) over the whole window. Worked out again every frame,
    // so a route change mid-Play hands the cursor over in both directions; App applies only a difference.
    struct WindowCursor
    {
        bool captured = false;
        bool clipped  = false;   // to clipX.. (client pixels); otherwise the whole client area
        long clipX = 0, clipY = 0, clipW = 0, clipH = 0;
    };
    WindowCursor DesiredWindowCursor(bool flowWantsCapture) const;
    // Gameplay's mouse buttons are hidden this frame: on the Game route unless the pointer is on the picture
    // with the game's focus (or the capture); elsewhere @p imguiWantsMouse, ImGui's own rule.
    bool SuppressGameplayMouse(bool imguiWantsMouse) const;

    // Derived from PlayModeController when wired; falls back to the local
    // m_playState before App wires it. Out-of-line so the header
    // doesn't need the PlayModeController definition.
    ViewportPlayState GetPlayState() const;
    // True for one frame after user clicks "Update 1 frame"; consume per frame.
    bool ConsumeWantsStepFrame();

    // ---- Window-toggle accessors ----
    void SetShowAnimDebug(bool show) { m_showAnimDebug = show; }
    bool GetShowAnimDebug() const    { return m_showAnimDebug; }
    // Phase Debug panel: its ImGui draw lives in EditorFrameHooks.cpp (it needs the
    // Scheduler), gated on this toggle. Off by default; opened from Window > Analysis.
    void SetShowPhaseDebug(bool show) { m_showPhaseDebug = show; }
    bool GetShowPhaseDebug() const    { return m_showPhaseDebug; }
    // Canvas-native overlay of resolved UI layout / clip bounds. App forwards
    // this to UICanvasSystem immediately before the UIPass is painted.
    bool GetShowUICanvasWireframe() const { return m_showUICanvasWireframe; }

    // ---- Callbacks ----
    // @p meshType: 0 = Cube, 1 = Sphere, 2 = Cone (the Create menu). @p pos is a world-space
    // spawn point in front of the camera, so a new primitive appears in the current view.
    void SetCreateMeshCallback(std::function<void(int meshType, const DirectX::XMFLOAT3& pos)> cb)
    {
        m_createMeshCallback = std::move(cb);
    }
    // Left-click in the viewport -> the callback receives viewport-relative pixel coords.
    void SetPickCallback(std::function<void(float px, float py)> cb)
    {
        m_pickCallback = std::move(cb);
    }
    // Called by App after Renderer::ResolvePick (1-frame delay).
    void SetPickResult(Entity entity);
    // Read by App to push the editor's selection into Renderer::SetPickingOutlineEntity
    // for the picking-highlight overlay.
    Entity GetSelectedEntity() const { return m_selectedEntity; }

    // ---- Multi-selection ----
    // m_selectedEntity stays the PRIMARY: the last entity clicked, what the Inspector shows
    // and what the gizmo anchors on. ReconcileSelection() runs once per frame and collapses
    // the multi-selection whenever something assigned m_selectedEntity directly, because
    // every direct assignment means "select exactly this, or nothing". Multi-select code
    // updates both, so it never triggers the collapse.
    // Order is click order; back() is the primary.
    const std::vector<Entity>& GetSelectedEntities() const { return m_selectedEntities; }
    bool   IsEntitySelected(Entity e) const;
    size_t SelectionCount() const { return m_selectedEntities.size(); }
    void   SelectEntity(Entity e, SelectMode mode);
    void   ClearEntitySelection();

    // The selected entities that have NO selected ancestor. EVERY batch
    // operation must iterate this, not the raw selection: transforming or
    // deleting a parent already carries its children, so applying to both
    // double-transforms (or deletes an entity twice).
    std::vector<Entity> SelectionRoots() const;

    // Test seams. ReconcileSelection and the direct-primary assignment are the two halves of
    // the collapse contract above and both fail silently, so a harness reaches them without
    // a graphics device.
    void TestOnly_ReconcileSelection()            { ReconcileSelection(); }
    void TestOnly_AssignPrimaryDirect(Entity e)   { m_selectedEntity = e; }

    // Batch operations. Both take / derive selection roots and commit as one undo entry: a
    // gesture that needs N presses to undo is a bug.
    void DeleteEntities(const std::vector<Entity>& roots);
    void DuplicateSelection();
    size_t MoveEntitiesToFolder(const std::vector<Entity>& entities,
                                const std::string& path, bool resolveRoots = true);
    // JSON command handler; invoked only by the control channel's main-thread drain.
    std::string ExecuteEditorCommand(const std::string& request);
    // Entity.set (the generic component write) becomes an ordinary edit: undo + the scene's
    // dirty flag. App calls it once.
    void InstallEntityWriteObserver();
    // The Lua `Editor` table: scripted authoring through the editor's own undo, selection and
    // dirty state (Batch = one undo entry, all-or-nothing; Create, InstantiatePrefab, Delete,
    // Duplicate, Reparent, AssignMaterial, Undo / Redo / State, the scene New / Open / Save
    // jobs, the asset editors' entry points). In EditorLayer_Authoring.cpp; App calls it
    // once.
    void RegisterAuthoringLua(struct lua_State* L);
    // Editor.ProjectSetting(path, value): the write a Project Settings control makes, for
    // scripts and tests (a harness run has no mouse). In EditorLayer_GraphicsSettings.cpp;
    // called by RegisterAuthoringLua.
    void RegisterProjectSettingsLua(struct lua_State* L);
    // Editor.GetProperty / SetProperty / ResetProperty(id, "Tag.path"): a reflected field read or
    // written as the Inspector does (one undo step). In EditorLayer_Properties.cpp; called by
    // RegisterAuthoringLua.
    void RegisterPropertyLua(struct lua_State* L);
    // Editor.Panels() / Editor.SetPanelOpen(id, open): the panel registry. In EditorLayer_Panels.cpp.
    void RegisterPanelLua(struct lua_State* L);
    // One settings row's Project Settings control (a player, developer or pass row by path): the
    // search results page draws every matching row with it. In EditorLayer_GraphicsSettings.cpp.
    bool DrawSettingsRow(const char* path);

    // Editor-only chain-physics (KawaiiPhysics-style) authoring overlay. Two
    // phases because DebugWirePass::Clear() is gated by the pass's `enabled`
    // flag inside BeginFrame, while the lines must be pushed afterwards:
    //   PrepareChainPhysicsOverlay  -> BEFORE BeginFrame: force the wire pass on
    //       so Clear() runs and the ring slot accepts this frame's lines.
    //   EmitChainPhysicsOverlay     -> AFTER BeginFrame (debug-submit window):
    //       push root / simulated-chain / excluded-bone wire geometry.
    void PrepareChainPhysicsOverlay(Renderer& renderer, World& world);
    void EmitChainPhysicsOverlay(Renderer& renderer, World& world);
    // Overlay target: the hierarchy selection; the preview stage never qualifies.
    Entity ChainOverlayCandidate(World& world) const;

    // ---- Skeleton overlay (viewport Show > Skeleton) ----
    // The main viewport's skeleton (viewport Show > Skeleton): the selection's skeleton in its
    // live pose (BuildBoneWorldMatrices, the chain overlay's FK) as depth-aware lines, and its
    // sockets as small axis tripods (a StaticMesh owner's mesh sockets too). Same two phases
    // as the chain overlay (Prepare before BeginFrame, Emit in the debug-submit window).
    void PrepareSkeletonOverlay(Renderer& renderer, World& world);
    void EmitSkeletonOverlay(Renderer& renderer, World& world);

    // The entity the overlay draws for the selection: its skeleton carrier (a
    // character root's, a SkeletonRef's), else the selection itself when it has
    // sockets; NullEntity = nothing to draw.
    Entity SkeletonOverlayTarget(World& world) const;

    // ---- Bone-level authoring (authored rest pose) ----
    // Bone -> MODEL space matrices for a skinned entity, and the entity's world matrix
    // separately (@p outEntWorld; callers compute boneToModel[b] * entWorld). FK over the final
    // LocalPose (physics included; the root's translation from the rest pose, as skinning
    // does), then skel.bindPose for any bone the pose does not cover. Not the chain solver's
    // boneWorldMatCache: that is its pre-simulation target pose.
    // Shared by the overlay, the bone picker and the bone gizmo, so the three cannot disagree
    // about where a bone is.
    bool BuildBoneWorldMatrices(Renderer& renderer, World& world, Entity e,
                                std::vector<DirectX::XMFLOAT4X4>& outBoneToModel,
                                DirectX::XMMATRIX& outEntWorld) const;

    // Bones of the entity's authored chain groups. focusGroup >= 0 restricts the set to that
    // group's subtree: a PMX character with 20 hair chains is otherwise an unpickable
    // blizzard of handles.
    static void CollectChainBones(const SkeletonAsset& skel,
                                  const ChainPhysicsComponent& comp,
                                  int focusGroup,
                                  std::vector<uint8_t>& outPickable);

    // Screen-space nearest chain bone under (px, py), or ~0u. Not Ray::FromViewport: that
    // un-projects with the jittered viewProj while ImGuizmo draws with projMatrixNoJitter, so
    // with TAA / FSR2 on, the handle and its hit test would sit a fraction of a pixel apart.
    uint32_t PickChainBone(Renderer& renderer, World& world, Entity e,
                           float px, float py, float vpW, float vpH) const;

    // ROTATE gizmo for the selected chain bone; writes the authored rest pose.
    void RenderBonePoseGizmo(ImVec2 vpMin, ImVec2 vpMax);
    // Brackets one bone-gizmo drag into a single undo entry (mirrors
    // UpdateTransformEditTracking, which handles entity transforms).
    void UpdateBonePoseEditTracking();
    void CommitBonePoseEdit();

    // Snapshot / push helpers for the bone gizmo's drag (UpdateBonePoseEditTracking /
    // CommitBonePoseEdit), which runs outside DrawComponentWithUndo's bracket. Do not call them
    // from controls drawn inside that bracket (the rest-pose section's buttons): the bracket
    // already records those, and a second entry would take two Ctrl+Z presses.
    // BonePoseBaseline (CaptureBoneRestPose) holds only the authored-rest-pose fields, not the
    // whole component: the registered captureValue deep-copies up to 64 ~1 KB ChainGroupDefs,
    // and the drag tracker refreshes its baseline every idle frame (~85 KB of allocation per
    // frame).
    BonePoseBaseline CaptureBoneRestPose(Entity e) const;
    // No-op when nothing actually changed (compares field by field: two
    // std::function RestoreFns cannot be compared, so the generic fingerprint
    // path is unavailable here).
    void PushBoneRestPoseEdit(Entity e, const BonePoseBaseline& before,
                              const char* label);
    // "Authored rest pose" block of the chain parameter tree. hooks.entity is the entity it
    // describes; every edit in it is structural and calls hooks.invalidate
    // (Editor/ChainParamTree.h). Its pose-tool state is PoseStateFor(hooks).
    void DrawAuthoredRestPoseSection(ChainPhysicsComponent& comp,
                                     const SkeletonAsset& skel,
                                     const ChainParamHooks& hooks);
    // The chain parameter tree, drawn by the Chain Physics Preview panel: on its staged copy
    // in Edit mode, and on a scene entity's live component (DrawChainEditorTarget) in Play.
    // Editor/ChainParamTree.h has the hooks contract.
    void DrawChainParamTree(ChainPhysicsComponent& comp, const SkeletonAsset& skel,
                            const ChainParamHooks& hooks);
    // The tree's pose-tool state: hooks.pose when given, the main viewport's when
    // hooks.mainOverlayTarget, else a scratch state keyed by hooks.entity (reset on a new
    // entity).
    ChainPoseState& PoseStateFor(const ChainParamHooks& hooks);
    // A scene entity's live chain parameters inside DrawComponentWithUndo (the registered
    // ChainPhysics capture; no global undo in Play): the body of the panel's Play-mode "LIVE"
    // section. A preview stage entity is refused (the panel's own tree edits it).
    // @p probeEdit (harness only) receives the component after the tree, inside the same
    // bracket.
    void DrawChainEditorTarget(World& world, Renderer& renderer, Entity entity,
                               const SkeletonAsset& skel,
                               const std::function<void(ChainPhysicsComponent&)>& probeEdit);
    // The carrier the Play "LIVE" section edits: the panel's Inspector request, else its scene
    // target, else the selection (ResolveChainTarget), else the first scene instance of the
    // subject prefab. *outWhy names the rule that matched.
    Entity ResolveChainLiveTarget(const char** outWhy) const;
    // The functor the panel calls for its Play-mode Parameters body (label, "Keep these
    // values", the live tree).
    void DrawChainLiveEdit(const std::function<void(ChainPhysicsComponent&)>& probeEdit = {});
    // "Keep these values": capture @p carrier's live component into the panel's keep slot
    // (material only for a prefab-ref root).
    void KeepChainLiveValues(Entity carrier);
    // Save-dialog + write, defined in EditorLayer.cpp where the Win32 wrappers
    // live in that TU's anonymous namespace. Other EditorLayer translation units
    // call this instead of duplicating the dialog. Returns false on cancel or on
    // a failed write; outPath (when given) receives the chosen UTF-8 path.
    bool SaveBlobWithDialog(const char* filter, const char* defaultExt,
                            const char* initDir,
                            const std::vector<uint8_t>& blob,
                            std::string* outPath = nullptr);
    // Ensure an entry exists for @p bone and return it, seeding the rotation from the bone's
    // current clean base; nullptr when the list is full. @p reseedExisting also overwrites an
    // existing entry's rotation ("Capture current pose"); without it a capture is a no-op on
    // every bone already in the list.
    BoneRestPose* EnsureBoneRestPoseEntry(ChainPhysicsComponent& comp,
                                          const SkeletonAsset& skel,
                                          World& world, Entity e, uint32_t bone,
                                          bool reseedExisting = false);

    // ---- Component editor registration ----
    // @p drawFn receives (live component ptr, world, selected entity).
    template<typename T>
    void RegisterComponentEditor(const char* label,
                                  std::function<void(void*, World*, Entity)> drawFn,
                                  int priority = 100,
                                  std::function<bool(World&, Entity)> requiresFn = nullptr)
    {
        ComponentEditorEntry entry;
        entry.label    = label;
        entry.type     = std::type_index(typeid(T));
        entry.module   = m_registeringModule.empty() ? ModuleOwningComponent(entry.type) : m_registeringModule;
        entry.draw     = std::move(drawFn);
        entry.priority  = priority;
        entry.condition = std::move(requiresFn);
        entry.fetch = [](World& world, Entity e) -> void* {
            return world.GetComponent<T>(e);
        };
        entry.add = [](World& world, Entity e) {
            if (!world.HasComponent<T>(e))
                world.AddComponent<T>(e, T{});
        };
        entry.remove = [](World& world, Entity e) {
            world.RemoveComponent<T>(e);
        };
        // Undo support (field edits). captureValue snapshots the component by deep value copy
        // (right for POD and string/map-holding types alike); fingerprint is cheap change
        // detection for trivially-copyable types (raw bytes), so no-op edits are skipped. Other
        // types fall back to the serializer blob or, without one, count as always changed.
        if constexpr (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)
        {
            entry.captureValue = [](World& world, Entity e) -> Editor::Undo::RestoreFn {
                const T* c = world.GetComponent<T>(e);
                if (!c) return {};
                auto saved = std::make_shared<T>(*c);
                return [saved](World& w2, Entity e2) {
                    if (T* c2 = w2.GetComponent<T>(e2)) *c2 = *saved;
                };
            };
        }
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            entry.fingerprint = [](World& world, Entity e) -> std::string {
                const T* c = world.GetComponent<T>(e);
                return c ? std::string(reinterpret_cast<const char*>(c), sizeof(T))
                         : std::string{};
            };
        }
        m_componentEditors[std::type_index(typeid(T))] = std::move(entry);
        m_componentLabels [std::type_index(typeid(T))] = label;
    }

    // Open the inspector header for T closed by default (saves vertical space
    // for chunky/rarely-edited components like Visibility). User can still
    // expand it; this only affects the initial state per ImGui's policy.
    template<typename T>
    void SetComponentDefaultCollapsed(bool collapsed)
    {
        auto it = m_componentEditors.find(std::type_index(typeid(T)));
        if (it != m_componentEditors.end())
            it->second.defaultCollapsed = collapsed;
    }

    // Drop the raw-byte change-detection fingerprint for T, so ComponentFingerprint falls back
    // to the authored-only serializer blob.
    // Use it for any trivially-copyable component that also carries per-frame runtime state:
    // the byte fingerprint hashes the whole struct, so any widget interaction while the
    // runtime fields move makes fpBefore != fpAfter and records a bogus undo entry that also
    // marks the scene dirty. (A runtime-bearing component holding a std::string /
    // std::vector escapes this only because it is not trivially copyable.) T must have a
    // registered serializer, or change detection degrades to "always changed".
    template<typename T>
    void UseSerializerFingerprint()
    {
        auto it = m_componentEditors.find(std::type_index(typeid(T)));
        if (it != m_componentEditors.end())
            it->second.fingerprint = nullptr;
    }

    // Bucket a registered component into an Add Component submenu (default "Misc").
    template<typename T>
    void SetComponentCategory(const char* category)
    {
        auto it = m_componentEditors.find(std::type_index(typeid(T)));
        if (it != m_componentEditors.end())
            it->second.category = category;
    }

    // Auto-generated inspector via Reflect::Descriptor<T>. Pass postDraw to append
    // hand-written widgets (buttons, conditional UI) after the field widgets.
    // Requires REFLECT_BEGIN(T) + REFLECT_END() visible at the call site.
    template<typename T>
    void RegisterReflectedComponent(const char* label,
                                    int priority = 100,
                                    std::function<void(T&, World*, Entity)> postDraw = nullptr,
                                    std::function<bool(World&, Entity)> requiresFn = nullptr)
    {
        auto draw = [postDraw = std::move(postDraw)]
                    (void* data, World* w, Entity e)
        {
            T* obj = static_cast<T*>(data);
            Reflect::DrawObject(obj);
            if (postDraw) postDraw(*obj, w, e);
        };
        // Undo bracketing is applied at the inspector dispatch level (DrawComponentWithUndo), so
        // it covers hand-written editors too. Nothing to add here.
        RegisterComponentEditor<T>(label, std::move(draw), priority, std::move(requiresFn));
        // The same descriptor by type_index, for engine-side tools that never
        // name T (the -dump_catalog harness dump; Reflection/ReflectRegistry.h).
        Reflect::RegisterRuntime(std::type_index(typeid(T)), &Reflect::Describe<T>(), label);
    }

    // The entries registered until the next call belong to module @p name ("" = the core);
    // Editor::ModuleRegistry brackets each module's RegisterComponentEditors with it.
    void SetRegisteringModule(std::string name) { m_registeringModule = std::move(name); }
    // The module whose descriptor names @p type's component tag, "" for none: a component the
    // core registers but a module owns (NavAgent, the Nav module's) is not offered while that
    // module is off, like one the module registers itself.
    static std::string ModuleOwningComponent(std::type_index type);

    // Display-only component (named row, no edit UI).
    template<typename T>
    void RegisterComponentTag(const char* label)
    {
        m_componentLabels[std::type_index(typeid(T))] = label;
    }

    // @p e is @p item, or is placed through it: an ancestor, or a FollowSocket / FollowEntity
    // target, of @p e or of anything @p e follows or hangs under, is @p item. Attaching
    // @p item to a socket of such an @p e would make two entities follow each other (each
    // resolving against the other's zeroed transform).
    static bool FollowsEntity(World& world, Entity e, Entity item);

    // ---- Asset browser ----
    // The one payload a drag from an asset item publishes. ImGui keeps a single payload per
    // source, so publishing a second silently destroys the first (a .iscn / .ipfb drag then does
    // not reach the viewport). Deciding it in one place makes that unrepresentable; public
    // so a harness can pin it without an ImGui context.
    AssetDragPayload AssetDragPayloadFor(int index) const;
    // Test seam: stage a listing + selection without a filesystem scan.
    void TestOnly_SetAssets(const std::vector<std::string>& paths,
                            const std::vector<std::string>& selected);

private:
    // ---- Panel render methods ----
    struct PanelEntry
    {
        const char*               section;   // Window-menu group heading
        const char*               label;     // menu label == the window's visible title
        const char*               windowId;  // Begin() name to focus, "Title###ID" form included
        std::function<bool()>     isOpen;    // null = a one-shot opener (no check mark)
        std::function<void(bool)> setOpen;
        const char*               tooltip = nullptr;
    };

    struct BakedCollisionReport
    {
        std::vector<std::string> models;        // models whose asset collision was written
        uint32_t                 modelMeshes = 0;   // Meshes given BO / PR in them
        uint32_t                 refreshed   = 0;   // placed parts whose collider changed with them
        uint32_t                 perEntity   = 0;   // entities given a Mesh collider of their own
        uint32_t                 replaced    = 0;   //   ... replacing a collider they had
        uint32_t                 skippedSections = 0;   // Static Mesh sections of a model not written
        std::vector<std::pair<std::string, std::string>> perEntityModels;   // model, why not written
        std::string              error;             // why nothing was done
    };

    // ---- Asset browser ----
    enum class AssetType { Texture, Shader, Mesh, Material, Scene, Prefab, Skeleton, Animation, AnimGraph, AnimSet, Audio, Script, Font, VFX, Sequence, UIAsset, FaceMap, FaceTake, Folder, Unknown };

    // Tiles = the large-thumbnail grid; List / Details mirror Windows Explorer's small-icon
    // views and show the full file name (extension included) instead of the truncated stem.
    enum class AssetViewMode { Tiles, List, Details };

    // ---- Models expand into their Meshes ----
    struct AssetDisplayRow
    {
        int asset = -1;   // index into m_assets
        int mesh  = -1;   // -1: the asset itself; else index into model->meshes
        // The registry's view of the model (sub-rows only). Valid for the frame
        // the rows were built in: the registry drops views only at an epoch
        // change / world clear, which happens between frames.
        const MeshAssetRegistry::Model* model = nullptr;
    };

    // ---- Mesh operations (EditorLayer_MeshOps.cpp) ----
    struct MeshExtractOutcome
    {
        MeshExtract::Result file;
        uint32_t            retargeted = 0;
    };

    struct UnpackOutcome
    {
        uint32_t    owners = 0, parts = 0, colliders = 0, slotOverrides = 0;
        std::string code, error;
    };

    struct ExtractDialog
    {
        bool        open = false;
        std::string model, meshName;
        uint64_t    key = 0;
        char        dest[512] = {};
        bool        retarget = true;
        std::string lastError;
    };

    // ---- Panel render methods ----
    void SetupDockSpace();
    // Main menu bar (EditorLayer_MainMenu.cpp). One function per top-level
    // menu; the bar itself is File / Edit / Create / Window / Tools / Build /
    // Debug / Help. Menu labels are the TITLES of the windows they open.
    void RenderMenuBar();
    void RenderFileMenu();
    void RenderEditMenu();
    void RenderWindowMenu();
    void RenderToolsMenu();
    void RenderBuildMenu();
    void RenderDebugMenu();
    void RenderHelpMenu();
    // The Create menu's items (EditorLayer_CreateMenu.cpp), shared by the main
    // menu bar and the Hierarchy's "+ Create" button. Every item is recorded
    // through RecordSpawn, so creating anything is one Ctrl+Z.
    void DrawCreateMenuItems();
    // The viewport toolbar's "Show" dropdown (grid, canvas bounds, gizmo icons, probe viz,
    // debug wireframes). EditorLayer_MainMenu.cpp.
    void DrawViewportShowMenuItems();
    // File > Open Scene... (dialog, then the guarded RequestLoadScene path).
    void OpenSceneWithDialog();
    // Toolbar Play/Pause and Ctrl+P: pre-Play autosave + RequestTogglePlay.
    void TogglePlayPause();

    // Every dockable panel the Window menu lists: one table, read by the Window menu and the
    // Command Palette so the two cannot drift apart.
    std::vector<PanelEntry> CollectPanelEntries();

    // Tools > Command Palette / Ctrl+Shift+P (EditorLayer_CommandPalette.cpp):
    // a searchable list of every panel and editor action.
    void RenderCommandPalette();

    // File > Exit: posts WM_CLOSE, so the exit goes through App's normal
    // unsaved-changes confirmation (RequestExit / ConsumeExitApproved).
    void RequestAppExit();

    void ProcessCollisionBake();

    // "Use baked as collision": the finished bake becomes the collision of what it was baked
    // from. A part of a model the editor can write (not through a junction, no `C` records left
    // to migrate with -assettool=collision_migrate) gets it as the model's asset collision - per
    // Mesh, `PR type=mesh lib=<bake> sec=` - and every placement follows
    // (RefreshAssetCollision); anything else gets a Mesh collider of its own. One undo step;
    // refused in Play and while the bake is previewed in the world.
    BakedCollisionReport UseBakedAsCollision();

    // A model's collision records changed on disk (Use Baked as Collision and its
    // undo / redo, a Mesh Editor save): the physics shape caches forget them,
    // Static Mesh owners re-derive, every placement follows (RefreshAssetCollision).
    // Returns the part collider changes.
    uint32_t ModelCollisionChanged(const std::string& model);
    // A model's socket records changed on disk: every placed instance re-derives them
    // (SceneInstanceLoader::RefreshModelSockets).
    uint32_t ModelSocketsChanged(const std::string& model);
    // @p item follows @p socket of @p target (an entity with that socket): a FollowSocket,
    // the item's LocalTransform zeroed (it sits on the socket), as one undo step ("Attach to
    // Socket"). "" = done, else why not.
    std::string AttachToSocket(Entity item, Entity target, const std::string& socket);
    // Why AttachToSocket refuses @p item whatever the socket (playing, a parent, inside a
    // model instance); "" = it may attach. The Outliner's menu shows it.
    std::string AttachRefusal(Entity item);
    // The placed instance of @p model nearest to @p item that has @p socket.
    Entity NearestModelInstance(Entity item, const std::string& model, const std::string& socket, std::string& why);

    // An .iskel's humanoid profile changed on disk (the Skeleton Editor's Save): every
    // registry skeleton that a model root in the World (a placement or a preview stage) got
    // from @p iskel takes @p hp, and their AnimLayer masks are rebuilt. Returns the roots
    // updated.
    uint32_t ModelHumanoidChanged(const std::string& iskel, const HumanoidProfile& hp, const SkeletonAsset& fileSkel);
    // The Animation Retarget window aimed at a placed instance of @p model (the selection);
    // "" = done, else why not.
    std::string OpenRetargetFor(const std::string& model);

    // Edit-menu keyboard routing: true when the window that had keyboard focus last frame is
    // the Hierarchy or the Viewport. Delete / F2 / Esc act on the entity selection only then;
    // other panels (Sequencer, notify tracks, text fields) own those keys.
    bool SelectionPanelFocused() const;
    // Harness-only (-ui_open_menu): should the named menu / dropdown be held
    // open this frame so a -ui_screenshot can show it? Always false in Retail.
    bool HarnessMenuHeldOpen(const char* name) const;
    // What a module's editor hooks get this frame (Editor/EditorModule.h).
    Editor::ModuleHost ModuleHostFor();

    // Animation-retarget tool window (drag an .iskel / Browse, pose + option
    // controls, bake onto the selected entity's skeleton). See
    // docs/design/RETARGET_ARCHITECTURE.md.
    void RenderRetargetWindow();               // toggled by m_showRetargetWindow
    // Bake srcPath's clips onto the selected entity's skeleton using the m_rt*
    // option members. Fills outStatus; returns success.
    bool DoRetarget(const std::string& srcPath, std::string& outStatus);
    // Batch: DFS srcRoot for animation sources, retarget each onto the selected
    // entity's skeleton, and write the .ianim under outRoot mirroring the source
    // tree. Needs the Source-skeleton override (one reference pose for all files).
    bool DoBatchRetarget(const std::string& srcRoot, const std::string& outRoot,
                         std::string& outStatus);
    // Shared by both paths: the selected entity's skeleton with a valid profile.
    SkeletonAsset* ResolveRetargetTarget(Entity& outSkelEntity, std::string& err);
    // One source file -> clips bound to refSkel (.ianim / .iskel, or any Assimp clip source
    // imported in memory). No cache: a batch touches many files.
    bool LoadRetargetClipsFor(const std::string& srcPath, SkeletonAsset& refSkel,
                              std::vector<ClipAsset>& outClips, std::string& err);
    // Unity-Avatar-style categorized humanoid slot list with per-slot manual
    // re-assignment (rendered inside the retarget window). Edits either the
    // TARGET (selected entity's registry skeleton) or the cached SOURCE .iskel.
    void RenderHumanoidMappingUI();
    // Load (or return cached) source skeleton+clips for m_retargetSrcPath.
    // The cache keeps manual mapping edits alive across bakes.
    SkeletonAsset* GetOrLoadRetargetSource();
    void RenderHierarchyPanel();
    // Invalidate the Hierarchy panel's cross-frame cached view (logical map /
    // folder tree / row icons). Call after any structural edit: create /
    // delete / duplicate / reparent, folder ops, prefab spawn, undo/redo.
    // Entity-count, world-pointer/epoch changes and a 32-frame safety net also
    // trigger the rebuild, so a missed call self-heals within ~0.5 s.
    void MarkHierarchyDirty() { m_hierarchyStructureDirty = true; }
    // One-time bake of the debug-gizmo icon glyphs (Font Awesome) -> engine textures.
    void EnsureGizmoIconsBaked();
    // Frame the selected entity in the viewport (editor free-fly camera). F key.
    void FocusSelectedEntity();
    void RenderViewportPanel();
    void RenderFullscreenViewport();
    bool CacheRenderableViewportSize(const ImVec2& size);
    void RenderViewportToolbar();
    void RenderInspectorPanel();
    // Content Browser (asset tree + tiles) and Output Log: two dockable windows.
    void RenderContentBrowserPanel();
    void RenderOutputLogPanel();
    // Every loaded .meshlib and who holds it (Window > Analysis).
    void RenderLoadedGeometryWindow();
    // Every texture the TextureSystem holds - size, format, mips, VRAM, refs, users, state -
    // sortable and filterable, with subtotals by format (Lua Asset.textures is the same list).
    void RenderLoadedTexturesWindow();
    void RefreshLoadedTextures();   // ProcessPendingActions: the snapshot + users
    void RenderPostProcessPanel();       // color grading / post-process settings
    void RenderGraphicsSettingsWindow(); // toggled by m_showGraphicsSettings - "Project Settings" (settings.json / engine.json)
    // The Ray Tracing page's live ReSTIR / PT GPU cost holds a GPU profiler want while it is
    // on screen; released when it is not.
    void SyncRestirCostWant(bool want);
    void RenderEditorPreferencesWindow(); // toggled by m_showEditorPreferences - per-user editor_prefs.ini (EditorLayer_Preferences.cpp)
    // EditorLayer_StatusBar.cpp: the bottom status bar (drawn BEFORE the dock
    // space so its work-area inset applies) and the two Help windows.
    void RenderStatusBar();
    // Tools > AI Control Channel / the command palette: open the control pipe in this running
    // editor, or close it at the next frame boundary (Harness/ControlChannel.h).
    void ToggleControlChannel();
    void RenderKeyboardShortcutsWindow(); // Help > Keyboard Shortcuts / F1
    void RenderAboutWindow();             // Help > About
    void RenderEnvironmentWindow();      // toggled by m_showEnvironmentWindow - consolidated scene-environment panel
    void RenderWorldSettingsWindow();    // toggled by m_showWorldSettings - per-scene bindings + AO/Outline (EditorLayer_WorldSettings.cpp)

    // ---- Fog helpers (EditorLayer_CreateMenu.cpp) ----
    // A new "Volumetric Fog" entity whose component is seeded from the pass-level fog values
    // (settings.json / .ippc), so converting a scene is visually lossless. Callers wrap it in
    // RecordSpawn.
    Entity SpawnVolumetricFogFromLegacy();
    Entity SpawnLocalFogVolume();
    // Post-process volume with a fresh runtime profile (global = unbounded).
    Entity SpawnPostProcessVolume(bool global);
    // Status + one-click fixes under the fog UI (sun shafts, local volume
    // budget). Shared by the Inspector and the Environment window.
    void DrawVolumetricFogStatus();
    // Inspector "Add Component" semantics from any window: applied at the
    // frame boundary, recorded as one undo step. `add` must construct the full
    // value (redo replays it).
    void AddComponentWithUndo(Entity e, std::type_index type,
                              std::function<void(World&, Entity)> add);
    // A component body drawn outside the Inspector with the Inspector's
    // field-edit undo bracket (capture / fingerprint of its registered entry).
    void DrawComponentBodyWithUndo(Entity e, std::type_index type,
                                   const std::function<void()>& drawBody);
    // One registered component's whole Inspector section drawn outside the
    // Inspector (the UI Editor's Details): its header, its right-click Remove
    // Component (undoable) and its field-edit undo bracket. False: no editor for
    // the type, or the entity lacks it.
    bool DrawComponentSection(Entity e, std::type_index type);

    // ---- Panel render methods ----
    void RenderTimelinePanel();         // toggled by m_showTimeline (tabbed: Clip Asset / Entity)
    void RenderClipAssetTab();           // edit notify tracks on a .ianim clip asset
    void RenderEntityTimelineTab();      // per-entity TimelineComponent editing
    void LoadAnimForEdit(const std::string& path); // begin async .ianim load for the editor
    bool SaveAnimEdit();                 // re-serialize the loaded resource to .ianim on disk
    void RenderProfilerPanel();          // toggled by m_showProfiler
    // Every frame: take the engine profiler (GPU want, and a collector when the
    // command line started none) while the window is open, release it after.
    void SyncProfilerHold();
    // Toggled by m_showPoolWatermarks. A separate window from the Profiler on purpose: that
    // one is per-frame GPU timing, this one cross-frame monotonic high-water marks;
    // interleaved, both are unreadable.
    void RenderPoolWatermarkPanel();
    void RenderAnimationDebugWindow();   // toggled by m_showAnimDebug
    void RenderDecalMaterialsWindow();   // toggled by m_showDecalMaterials (Window > Content, or a Decal's Edit... button)
    void RenderFontEditorWindow();       // toggled by m_showFontEditor
    void RenderCameraSwitcherWindow();   // toggled by m_showCameraSwitcher
    void RenderAnimationSetEditorWindow(); // toggled by m_showAnimSetEditor (AnimationSetEditorWindow.cpp)
    void RenderVFXEditorWindow();          // toggled by m_showVFXEditor (VFXEditorWindow.cpp) - .ivfx authoring
    void RenderScriptRegistryWindow();     // toggled by m_showScriptRegistry (ScriptRegistryWindow.cpp)

    // ---- Console (LuaConsoleWindow.cpp) ----
    // The Output Log's command line and the Tools > Lua Script Tools runner. Both only queue;
    // ProcessPendingLuaChunk runs the chunk from ProcessPendingActions, outside the ImGui
    // frame (the file header says why that split is required). Echo, result and every line
    // the chunk logs on the main thread are log records with source=console and the
    // command's request id.
    void SubmitConsoleLine(const std::string& text, bool fromHarness);   // the Enter key
    void RunLuaScriptTool(const std::string& relPath);                  // a Tools menu item
    // One chunk in flight per frame; a second in the same frame is refused
    // (and says so) rather than silently replacing the first.
    bool QueueLuaChunk(std::string chunk, std::string label, std::uint32_t requestId, bool tryExpression);
    void ProcessPendingLuaChunk();         // drained from ProcessPendingActions
    void TickHarnessConsole();             // -console_exec=<editor frame>:<lua>
    void RenderLuaToolsMenu();             // Tools > Lua Script Tools
    void ScanLuaTools();                   // asset/scripts/editor/*.lua, cached

    // ---- Panel render methods ----
    void RenderGameFlowWindow();          // toggled by m_showGameFlow (GameFlowWindow.cpp)
    void OpenVFXAsset(const std::string& path); // load a .ivfx into the VFX Editor + show it (VFXEditorWindow.cpp)
    // Component Transfer (ComponentTransferWindow.cpp): diff two targets (entity or .ipfb)
    // component by component and copy across either way. Editor/ComponentTransferWindow.h
    // has the transfer + undo contract.
    void RenderComponentTransferWindow();  // toggled by m_showComponentTransfer
    void ComponentTransferRebuildRows();   // re-derive the merged row list from both targets
    bool ComponentTransferApply(bool aToB);// run every ticked row in one undo macro
    void DrawComponentTransferTarget(Editor::ComponentTransfer::Target& t,
                                     const char* id, bool isA);
    // Sequencer (.iseq cinematic authoring). The window owns its document and its own undo
    // ring, deliberately not the scene undo stack, which is the single MarkSceneDirty() choke
    // point (PushEdit / EditUndo / EditRedo).
    void RenderSequencerWindow();
    void OpenSequenceAsset(const std::string& path);
    // Record mode: turn a finished gizmo drag into Transform keys at the
    // Sequencer playhead. No-op unless Record + Preview are both on.
    void SequencerOnTransformCommitted(uint32_t entity);

    // ---- Asset browser ----
    // What the content browser considers an asset, in ONE place: ScanAssets
    // fills the right pane from it and RebuildAssetFolderTree counts with it.
    // Two lists would drift, and the tree would advertise a per-folder count
    // the folder does not actually show. `ext` must be lowercase, with the dot.
    static AssetType AssetTypeForExt(const std::string& ext);

    // ---- Content-browser presentation ----
    void ScanAssets();
    void RenderAssetBrowser();               // thumbnail streaming + view-mode dispatch
    void RenderAssetGrid();                  // Tiles
    void RenderAssetList();                  // small icon + full name, multi-column flow
    void RenderAssetDetails();               // sortable Name / Type / Size / Modified table
    void RenderAssetViewModeSelector();       // toolbar Tiles|List|Details toggle
    void RenderAssetEmptySpaceContextMenu();  // right-click empty space -> Import
    void RenderAssetEmptyHint();              // "no assets here" placeholder
    // Shared item plumbing so all three views behave identically.
    void        RebuildAssetDisplayOrder();
    bool        AssetMatchesFilter(const std::string& stem) const;
    const char* AssetIconChar(AssetType t) const;   // nullptr when the icon font is missing
    static ImU32       AssetTypeColor(AssetType t);
    static const char* AssetTypeName(AssetType t);
    static const char* AssetTypeTag(AssetType t);   // one-letter no-icon-font fallback
    void        DrawAssetItemIcon(int index, float size);
    void        DrawAssetRowItem(int index, float iconSize, bool spanAllColumns);
    // Per-asset right-click: Rename / Delete / Show in Explorer / Copy Path.
    void        RenderAssetItemContextMenu(int index);

    // ---- Models expand into their Meshes ----
    // A `.iscn` item can be expanded: its placeable Meshes (MeshAssetRegistry; hidden ones -
    // collision, mirror variants - left out) are listed right after it, in every view. A Mesh
    // item drags as "STATICMESH_REF" (viewport / Hierarchy / Static Mesh inspector),
    // double-click places it, right-click offers Place in Scene / Copy Mesh Reference.
    void        RebuildAssetDisplayRows();
    bool        IsModelExpandable(int index) const;
    // The expand arrow in front of a model's row / on its tile; toggles on click.
    void        DrawModelExpandArrow(int index, const ImVec2& min, float size);
    // The Mesh a sub-row shows (nullptr for an asset row / a stale row).
    const MeshAssetRegistry::Mesh* ModelMeshAt(const AssetDisplayRow& row) const;
    void        DrawModelMeshTile(const AssetDisplayRow& row, float tileSize, float labelH);
    void        DrawModelMeshRowItem(const AssetDisplayRow& row, float iconSize, bool spanAllColumns);
    void        WireModelMeshInteractions(const AssetDisplayRow& row);
    // Place a Mesh item in front of the camera as ONE undo entry; selects it.
    Entity      PlaceBrowserMesh(const AssetDisplayRow& row);

    // ---- Mesh operations (EditorLayer_MeshOps.cpp) ----
    // Extract Mesh...: one Mesh of a model written out as a model of its own (MeshExtract -
    // same key, same SectionKeys), stamped, listed; @p retarget re-points the open scene's
    // Static Meshes of (model, key) at it as one undo entry. @p dest "" = beside the model,
    // named after the Mesh.
    bool ExtractModelMesh(const std::string& model, uint64_t meshKey, const std::string& dest,
                          bool retarget, bool overwrite, MeshExtractOutcome& out);

    // Unpack Model Instance: a SceneRef root's spawned subtree becomes authored
    // nodes - each node's mesh parts replaced by a Static Mesh of the Mesh they
    // form (slot overrides for materials that differ from the model's), part
    // colliders kept as entities of their own, the root's SceneRef dropped. One
    // undo entry. Refuses (nothing changed) a skinned model, a part carrying a
    // material override bag or face shading, a node placing part of a Mesh.
    bool UnpackModelInstance(Entity root, UnpackOutcome& out);
    // Create > 3D Object: a Static Mesh of the engine's Primitives model
    // (Scene/PrimitiveAssets; PrimitiveMeshType order). NullEntity when the model cannot be
    // read; the caller then falls back to a MeshHandle primitive.
    Entity CreatePrimitiveStaticMesh(int primitiveType, const DirectX::XMFLOAT3& at);

    void RenderExtractMeshDialog();

    // ---- Asset browser ----
    // The single wiring point for every asset-view item (row and tile): drag source, path
    // tooltip, click / double-click activation, right-click menu. Call right after submitting
    // the item's interactive widget, inside its PushID(index). One function, so no view can
    // miss a piece of the wiring.
    void        WireAssetItemInteractions(int index, bool clicked);
    // "Who uses this asset?" - a window over the reverse reference index
    // (AssetReferenceScanner): the pre-rename / pre-delete safety check a path-string identity
    // model otherwise cannot offer.
    void        RenderAssetReferencesWindow();
    // Left pane: the whole asset/ folder tree. Clicking a node navigates the
    // right pane, exactly like Unreal's Content Browser / Unity's Project view.
    void        RenderAssetFolderTree();
    void        RebuildAssetFolderTree();
    void        DrawAssetFolderNode(int index, bool reveal);
    // Navigate the right pane to `dir` (folder-tree click, Up, folder tile).
    void        SetCurrentAssetDir(const std::string& dir);
    // Rename fix-up: repoint every indexed reference from `oldRel` to `newRel`
    // (both project-relative). Rescans before AND after, and opens the Asset
    // References window when any container could not be repointed.
    void        RepointAssetReferences(const std::string& oldRel,
                                       const std::string& newRel);

    void        BeginAssetDragSource(int index);
    void        HandleAssetItemActivate(int index, bool clicked, bool doubleClicked);
    void        StreamAssetThumbnail(int index, bool visible, float now);
    uint64_t    GetAssetThumbnailSrv(int index) const;
    // The texture file's description (nullptr for a non-texture entry or an unreadable file),
    // read once per entry and cached on it.
    const Resource::TextureFileInfo* AssetTextureInfo(int index);
    void RenderAssetImportMenuItems();
    void RunImport(AssetType type);
    void RenderTextureImportPopup();
    void ProcessPendingTextureImports();
    // Import > Unreal Scene: a file dialog picks the manifest. A UE scene's Reimport passes its
    // SR record's manifest, the scene's folder and name.
    void BeginUnrealManifestImport(const std::string& manifest = std::string(),
                                   const std::string& outputDir = std::string(),
                                   const std::string& sceneName = std::string());
    void RenderUnrealImportPopup();

    // Per-frame helpers.
    // @p assetMode drops the entity-oriented header + Save button row: in asset mode there is
    // no entity, no MaterialSourcePath and no per-entity override, and
    // RenderMaterialAssetInspector draws its own header.
    void RenderMaterialInspector(struct MaterialComponent& mat, bool assetMode = false);

    // ---- The Inspector's material row (EditorLayer_MaterialRow.cpp) ----
    // What the row shows for @p e's own MaterialComponent, and for a Static Mesh slot of
    // @p owner (its section key; `path` = the override or the model's default).
    Editor::MaterialRowInfo MaterialRowFor(Entity e);
    Editor::MaterialRowInfo MaterialSlotRowFor(Entity owner, uint64_t sectionKey, const std::string& path,
                                               bool overridden);
    // Carries out a row's action: the click, and Editor.InspectorMaterialAction.
    // @p sectionKey != 0: a Static Mesh slot of @p e. False + *why when refused.
    // @p recordUndo = false: a slot row's widget inside the Static Mesh inspector's undo
    // bracket (which records the slot change itself); a drop, an entity row and a script push
    // their own entry.
    bool RunMaterialRowAction(Entity e, uint64_t sectionKey, const Editor::MaterialRowResult& a,
                              std::string* why = nullptr, bool recordUndo = true);
    // The Material section's row for @p e. Its actions push their own undo entries, so it is
    // drawn outside DrawComponentWithUndo's bracket.
    void DrawInspectorMaterialRow(Entity e);
    // A Static Mesh slot's material: the override, else the model's default
    // (false when `owner` has no Static Mesh or no section `sectionKey`).
    bool StaticMeshSlotMaterial(Entity owner, uint64_t sectionKey, std::string& path, bool& overridden,
                                std::string* defaultPath = nullptr);
    // The row's "Clear" for an entity: the engine's default material, no asset
    // (MaterialSourcePath / MaterialInstanceInfo / MaterialOverride removed), as
    // ONE undoable edit - AssignMaterial's value copies. EditorLayer.cpp.
    bool ClearMaterial(Entity e, std::string* error = nullptr);

    // ---- The Material Editor's entity tabs ----
    // Write a tab's staged material into its entity: the MaterialComponent, and the
    // MaterialOverride re-derived against the entity's file (so a reload of the file replays
    // the edit, and the scene saves it like any override). @p first / @p last bracket one
    // scene undo step per gesture (CaptureMaterialState's value copies).
    // -mateditor_entity_override_off is the reverse arm: the edit stays an unsaved
    // MaterialComponent edit.
    bool WriteEntityMaterial(const EntityHandle& h, const struct MaterialComponent& staged, bool first, bool last);
    // The entity's MaterialOverride = its material's diff against its file (read
    // past the live overlay); ASSET_DIRTY off when that diff holds every edit.
    void SyncEntityMaterialOverride(Entity e);
    // An entity tab's buttons: "promote" / "apply" / "clear" (see MaterialEditorContext).
    bool MaterialEntityAction(const EntityHandle& h, const std::string& action, const std::string& path,
                              std::string& why);

    // ---- The Inspector's material row (EditorLayer_MaterialRow.cpp) ----
    // The material row's upkeep (ProcessPendingActions): Edit here ends on another selection,
    // and m_matRowHints refreshes every 30 frames or on a selection change.
    void RefreshMaterialRowHints();

    // ---- Material asset editing (no entity involved) ----
    // Without this a material no entity uses could not be edited at all, which includes
    // every instance the moment it is created. Reuses RenderMaterialInspector rather than a
    // second widget set, so the schema loop, texture slots and custom shader parameters come
    // for free.
    void OpenMaterialAsset(const std::string& path);
    // Returns true if it drew (i.e. an asset is open); the Inspector then skips
    // its "select an entity" placeholder.
    bool RenderMaterialAssetInspector();
    // Creates an .imatinst inheriting from parentPath, next to it. Returns the
    // new path, or "" on failure.
    std::string CreateMaterialInstance(const std::string& parentPath);

public:
    // ---- Skeleton overlay (viewport Show > Skeleton) ----
    bool                 m_showSelectionSkeleton = false;
    SkeletonOverlayStats m_skelOverlay;
    SkeletonOverlayStats m_skelOverlayLogged;   // the last one logged (a line per change)
    // Prepare's answer, taken by this frame's Emit (one subtree walk a frame, not two).
    Entity               m_skelOverlayTarget   = NullEntity;
    bool                 m_skelOverlayPrepared = false;

private:
    // ---- Panel render methods ----
    bool m_commandPaletteRequest = false;

    // Menu-bar requests for modals that must open OUTSIDE BeginMenuBar (an
    // ID-stack mismatch otherwise makes BeginPopupModal a silent no-op).
    // Consumed by SetupDockSpace after EndMenuBar.
    bool m_bakeCollisionOpenRequest = false;   // Tools > Bake > Collision Meshes...
    // Tools > Bake > Collision Meshes: the bake and its options - the popup and
    // Editor.BakeCollision share them. m_collisionBakeJobId: a scripted bake's job,
    // completed by ProcessCollisionBake (every frame) when the bake has written.
    Tools::CollisionMesh::BakeOptions m_collisionBakeOpts;
    Tools::CollisionMesh::BakeJob     m_collisionBake;
    bool                              m_collisionBakeScripted = false;   // ProcessCollisionBake ticks it
    std::uint64_t                     m_collisionBakeJobId = 0;          // 0 where there is no control channel

    BakedCollisionReport m_lastBakedCollision;

    // Edit > Rename / F2: the Hierarchy picks this up on its next draw and opens
    // the inline rename field on that row (the rename state lives in the
    // outliner TU).
    Entity m_hierarchyRenameRequest = NullEntity;

    // ---- Mesh operations (EditorLayer_MeshOps.cpp) ----
    // The Content Browser's "Extract Mesh..." dialog (a Mesh item's menu opens it).
    ExtractDialog m_extractDialog;

    Editor::MaterialPropertyPanel m_materialPanel;   // what RenderMaterialInspector draws

    // ---- The Inspector's material row (EditorLayer_MaterialRow.cpp) ----
    // The Material section and each Static Mesh slot draw one Editor::DrawMaterialRow: the
    // asset, what it is, its warnings and [Edit]; editing happens in the Material Editor
    // (the row's Edit here opens the entity's material as an entity tab there).

    // ---- The Material Editor's entity tabs ----
    // WriteEntityMaterial's undo baseline: the entity's material state before the open gesture.
    Editor::Undo::RestoreFn m_matTabBefore;
    // ---- The Inspector's material row (EditorLayer_MaterialRow.cpp) ----
    // One "[INSPECTOR] material row" line each time what a row shows changes (the harness reads it).
    std::unordered_map<std::string, std::string> m_matRowLogged;
    // Render.explain's material causes ((code, text), RenderExplain::MaterialHints)
    // for the selection and its Static Mesh sections. Refreshed in
    // ProcessPendingActions - the render half is idle there - every 30 frames
    // and on a selection change; the row reads the last answer.
    std::unordered_map<Entity, std::vector<std::pair<std::string, std::string>>> m_matRowHints;
    Entity   m_matRowHintsFor   = NullEntity;
    uint64_t m_matRowHintsFrame = 0;

    // ---- Asset browser ----
    // A Content Browser file to select once its folder is listed ("Find in Content Browser").
    std::string m_revealAssetPath;

    // ---- Harness preview fixtures ----
    // WITH_HARNESS, matching the public entry points above and Editor::PreviewHarness, the other
    // half of the same flags. ProcessPendingActions calls TickHarnessPreview() unconditionally;
    // without the macro the call resolves to the inline no-op below, so the call site needs no
    // #ifdef.
#ifdef WITH_HARNESS

    // The -preview_bake_restore_at schedule's gaps (see m_bakeRestoreFrame).
    static constexpr std::uint64_t kBakeRestoreGap    = 4;
    static constexpr std::uint64_t kBakeRestoreVerify = 12;

    // One step of the -preview_close_at / -preview_open_at schedule (m_previewToggleSteps).
    struct PreviewToggleStep
    {
        std::uint64_t frame = 0;   // EDITOR frames (m_harnessFrameTick)
        bool          open  = false;
        bool          done  = false;
        // Which surfaces this step names. kPreviewSurfaceAll is the flag syntax's default.
        std::uint8_t  surfaces = kPreviewSurfaceAll;
    };
    static constexpr std::uint32_t kMaxPreviewToggleSteps = 8;

    // Drives the -preview_fixture schedule and the -preview_edit_at one-shot.
    // Called from ProcessPendingActions - the same frame-boundary window the
    // preview widget ticks in, which is the only place a material buffer may be
    // swapped without tearing a descriptor an ImGui::Image already captured.
    // Inert (one integer compare) when no fixture list was given.
    void TickHarnessPreview();
    // -chainpreview_stage_edit_at=<editor frame>: edit the preview stage's
    // ChainPhysicsComponent through DrawComponentWithUndo (the scene branch's own path) with a
    // two-entity scene selection, and log whether the selection, the undo stack or the
    // scene-dirty flag moved. Called from the UI half right after the Chain Physics Preview
    // panel draws.
    // -chainpreview_panel_tree_at: the panel's own parameter tree (the drawParamTree functor on
    // m_staged with the panel's StagedHooks, as DrawStagedParams hands it) in a focused probe
    // window, a synthetic active-id bracket, then the panel-undo shortcut route through the
    // panel's own focus frame.
    // -chainpreview_play_edit=<key>=<value>@<f>: the Play "LIVE" body (DrawChainLiveEdit)
    // editing its target, optionally pressing "Keep these values" (-chainpreview_play_keep),
    // then the read-back after Stop.
    void TickChainStageEditProbe();
    void TickChainPanelTreeProbe();
    void TickChainPlayEditProbe();
    // -chainpreview_main_pose=<entity name>[#k]:<bone|#idx|*>@<f>: select that scene
    // character, arm the Inspector row's "Pose rest bones in main viewport", focus the camera
    // on it and pick the bone, then report whether RenderBonePoseGizmo submitted the ring for
    // it (the main viewport's bone gizmo has no window of its own).
    void TickChainMainPoseProbe();
    // Install fixture @p name as the open asset-mode material. The asset PATH is
    // synthetic ("<fixture:name>") on purpose: it is what the preview widget
    // keys its view lifecycle on, so a change releases and re-acquires the view
    // exactly as switching .imat files does, and it is what the runtime key is
    // NOT built from (that is a content hash of the material itself).
    bool InstallFixtureMaterial(const std::string& name);
    // Write the newest preview picture to <artifacts>/<fileStem>.png. Returns
    // false when there is nothing to write (no artifact dir, no preview render
    // yet) or the capture was refused - the CALLER decides whether that is
    // worth a log line, because the fixture cycle retries and the one-shot
    // before-picture does not. Called at a frame boundary BEFORE the device
    // frame opens, where the previous frame's tone-map output is complete and
    // in SHADER_RESOURCE.
    bool CapturePreviewPNG(const std::string& fileStem);
    // One fixture window's end-of-window service: check the stencil the draw packet carried
    // against the one this fixture's shading model derives (with or without -artifacts), then
    // write the picture. True when the window is done; false asks the caller to retry next
    // frame. @p lastChance turns a silent retry into a logged failure.
    // @p occurrence is 1 for the first window naming this fixture, 2 for the second, ...:
    // occurrence 1 writes preview_<name>.png, later ones preview_<name>_<occurrence>.png, so a
    // schedule that names a fixture twice (two captures of one material, a lever flipped
    // between them) keeps both pictures.
    bool ServiceFixtureShot(const std::string& name, bool lastChance,
                            std::size_t occurrence);
    // Replace the World with the A/B parity fixture: one primitive sphere at the origin
    // carrying the named synthesised material, one directional light whose direction and
    // colour come from PreviewStudioRig, and a WorldEnvironment / Skybox pair pointing at the
    // same IBL cubes the preview acquires. The skybox path is left empty, which does not
    // blacken the backdrop (Renderer_IBL falls back to the radiance cube and paints that);
    // Preview::kParityDistanceScale keeps it out of the comparison by framing the subject to
    // the corners. Runs in the ProcessPendingActions window, where the editor's own New Scene
    // teardown runs.
    void BuildParityWorld(const std::string& fixtureName);

    // The secondary-view lights fixture world. Same window, teardown order and StaticSkybox
    // environment as BuildParityWorld, with a different subject: a floor slab and a back wall
    // that cover every pixel (so SkyboxPass#3's forced-off sun disk cannot reach the
    // comparison), a camera entity for -preview_pip to resolve, no directional light, and the
    // punctual set Preview::kLightsParityLights when @p withLights.
    void BuildLightsParityWorld(bool withLights);

    std::vector<std::string> m_previewFixtures;      // empty = feature off
    std::uint64_t            m_previewFixtureStart = 0;
    std::uint32_t            m_previewFixtureEvery = 40;
    // SIZE_MAX, not 0: 0 is a REAL index, and "the schedule has not installed
    // anything yet" has to be distinguishable from "fixture 0 is current" or
    // the first fixture never gets installed at all.
    std::size_t              m_previewFixtureIdx   = static_cast<std::size_t>(-1);
    bool                     m_previewFixtureShot  = false; // this window done
    // -preview_shadow_alpha, one entry per fixture window; the last entry repeats past its
    // end. Empty = the flag was not given and nothing is pushed at the renderer, which keeps
    // every other pack bit-identical.
    std::vector<std::uint8_t> m_previewShadowAlpha;
    // -preview_shadow_win, same shape and the same empty rule: nothing is pushed at the widget,
    // and every other pack stays bit-identical.
    std::vector<std::uint8_t> m_previewShadowWin;
    std::uint64_t            m_previewEditAtFrame  = 0;   // 0 = off
    // Latched at the edit; compared on later frames. See ViewsRenderedTotal.
    std::uint64_t            m_previewEditBaseline = 0;
    bool                     m_previewEditArmed    = false;
    // -preview_asset_edit=<.imat path>. Empty = the edit-at one-shot targets the open
    // material buffer. Non-empty = it targets an open asset preview's part material by saving
    // that file: a save is the only thing that moves Resource::MaterialContentGeneration, and
    // nothing else about the asset request changes.
    std::string              m_previewAssetEditMatPath;
    bool                     m_previewAssetEditSeeded = false;
    std::uint64_t            m_harnessFrameTick    = 0;

    // ---- -preview_bake_restore_at=<frame>[:<meshlib>] ----
    // Renderer::OnWorldClear(idsPreserved, worldSurvives) has a branch that keeps the PiP /
    // camera-view state (so a bake does not cancel a Lua or harness SetGamePiP), and its only
    // caller is the collision baker's preview RESTORE (ProcessPendingBakePreview,
    // worldSurvives=true), a GUI action. This drives the real two steps: the Load button's
    // intent at `frame`, the Restore button's kBakeRestoreGap frames later, and a
    // verification line kBakeRestoreVerify frames after that.
    // The swap map is empty on purpose: swapping entity mesh references is the bake's
    // business, and the restore's tail under test (WaitIdleAndReleaseDeferred,
    // MeshLibrary::Release, OnWorldClear(worldSurvives=true)) runs identically either way.
    std::uint64_t            m_bakeRestoreFrame   = 0;   // 0 = off
    std::string              m_bakeRestoreLibPath;
    std::uint64_t            m_bakeRestoreBaseline = 0;  // renders before it ran
    bool                     m_bakeRestoreDone    = false;

    // The asset panel's never-resolved deadline is posted as exit 12 exactly once, from the
    // game half.
    bool                     m_assetPreviewDeadlinePosted = false;
    // -preview_asset_verify targets, run once at editor frame 60 (TickHarnessPreview), after
    // the world has settled.
    std::vector<std::string> m_assetVerifyPaths;
    bool                     m_assetVerifyDone = false;
    // -preview_parity: the fixture whose World has not been built yet. Empty
    // once built (or when the flag was never given).
    std::string              m_pendingParityFixture;
    // -preview_lights_parity: 0 = off, 1 = baseline (no punctual lights), 2 = lights. An int,
    // not a string: the arm is a boolean plus "not asked for", and an empty-string sentinel
    // next to m_pendingParityFixture would read as the same feature.
    int                      m_pendingLightsParity = 0;
    // -preview_split_cams: the base camera whose pose the spawned split cameras copy, how many
    // to make, and whether it has happened. Deferred to a frame with a live World: the scene
    // loads asynchronously, so a spawn at parse time would find no base entity and put the
    // cameras at the origin (a plausible picture of the wrong place).
    std::string              m_splitCamBase;
    std::uint32_t            m_splitCamCount   = 0;
    bool                     m_splitCamSpawned = false;
    std::uint32_t            m_splitCamTries   = 0;
    // The ids the fixture spawned, so shutdown can take them back, plus the world epoch they
    // belong to. Entity has no generation, so an id outside its epoch names whatever now owns
    // that slot: the boundary must not destroy through this list blindly.
    // Not every epoch bump destroys the World: Scene.Load / World::Clear do, but
    // World::AdoptState (the editor's Play -> Stop restore) bumps the epoch while putting the
    // snapshot back, so the ids and names survive. Dropping the list there would orphan the
    // live SplitCam* entities and spawn more on every Play / Stop. The boundary therefore runs
    // the same guarded release as shutdown (alive and still named SplitCam*): a no-op on the
    // ids-destroyed kind, a take-back on the ids-preserved one.
    std::vector<Entity>      m_splitCamEntities;
    std::uint32_t            m_splitCamWorldEpoch = 0;
    bool                     m_splitCamEpochSeen  = false;
    // The session ledger the packs gate on. `spawns` counts spawn events (one per tenancy, so
    // a run with N re-arming world boundaries reads N+1); `rearms` counts epoch boundaries
    // that re-armed the fixture; `rearmReleased` counts the entities the boundary releases
    // destroyed (0 on an ids-destroyed boundary, m_splitCamCount on an ids-preserved one).
    // Reported on the release line, so one gate covers both duplication and its opposite (a
    // release taking recycled ids).
    std::uint32_t            m_splitCamSpawnEvents   = 0;
    std::uint32_t            m_splitCamRearms        = 0;
    std::uint32_t            m_splitCamRearmReleased = 0;

    // ---- -preview_close_at / -preview_open_at ----
    // One sorted list of steps rather than two, so a close and an open on the same frame is a
    // shape the parser can refuse rather than a race between two drains. `done` is per step
    // and one-shot, as SplitViewportPanel's layout steps are: a later step reopening what an
    // earlier one closed must not re-fire the earlier one.
    // The steps close panels, not views: closing a panel is the user's event (it stops
    // drawing, and its auto-release, "the panel did not draw last frame", hands the view back
    // at the next frame boundary); releasing views directly would test a path no user can
    // reach.
    PreviewToggleStep m_previewToggleSteps[kMaxPreviewToggleSteps]{};
    std::uint32_t     m_previewToggleCount = 0;
    // What a CLOSE took away, so the matching OPEN can put it back. The
    // material path is the interesting one: the asset-mode inspector is the
    // only preview surface that is not a bool.
    std::string       m_previewReopenMatPath;
    bool              m_previewReopenCamera = false;
    bool              m_previewReopenAsset  = false;
    bool              m_previewReopenSplit  = false;
    // Which surfaces a close is currently holding, as a mask of the kPreviewSurface* bits. A
    // surface is restored only while its bit is set here: the close sets it, the open clears
    // it, and a surface nothing closed is left as the user (or an earlier open) left it - an
    // `:all` reopen after a `:material` close must not write m_showCameraPreview = false over
    // a panel no step closed. HarnessSetPreviewToggleFrames holds the static half of the
    // rule: it refuses an open mask that is not a subset of the closes scheduled before it.
    std::uint8_t      m_previewClosedPending = 0;
#else

    // ---- Harness preview fixtures ----
    // Retail: no harness, no fixture cycle. The stub is what lets
    // ProcessPendingActions keep one unconditional call.
    void TickHarnessPreview() {}
    void TickChainStageEditProbe() {}
    void TickChainPanelTreeProbe() {}
    void TickChainPlayEditProbe() {}
    void TickChainMainPoseProbe() {}
#endif  // WITH_HARNESS

public:
    // ---- Edit Collider (EditorLayer_ColliderGizmo.cpp) ----
    enum class ColliderGizmoOp { Translate, Rotate, Scale };

    // ---- Editor viewport camera view ----
    enum class EditorViewMode : int { Perspective = 0, Top, Bottom, Front, Back, Left, Right };

    // ---- Edit Collider (EditorLayer_ColliderGizmo.cpp) ----
    // A drag as the gizmo applies it (the same frame, the same write-back), one
    // undo step: Translate by @p v (world units), Rotate by @p deg about the
    // collider's local axis @p v, Scale by the factors @p v.
    bool ColliderGizmoDrag(Entity e, ColliderGizmoOp op, const DirectX::XMFLOAT3& v, float deg, std::string& why);
    // Fit a box / sphere / capsule collider to the entity's own geometry (its
    // LocalAabb, or its Static Mesh sections'): offset, rotation and size. No
    // undo step (the Inspector's dispatch records one; the scripted call its own).
    bool FitColliderToBounds(Entity e, ColliderComponent& c, std::string& why);
    // Create > Collision: an entity with a Collider of @p shape and a RigidBody -
    // a child of @p parent fitted to its geometry and Kinematic (it follows the
    // parent: Unreal's ShapeComponent), or a Static one in front of the camera.
    // The caller records the undo (RecordSpawn).
    Entity CreateCollisionEntity(ColliderComponent::Shape shape, Entity parent);
    bool          EditColliderEnabled() const { return m_editCollider; }
    void          SetEditCollider(bool on);
    std::uint32_t ColliderGizmoGestures() const { return m_colliderGizmoGestures; }

    // ---- UI Rect tool (uGUI-style direct manipulation) ----
    // The Canvas UI node drawn at (x, y) of the main view (UIComputedRect pixels, which are
    // the viewport image's): the frontmost by sortKey among nodes that draw (an image or a
    // text), inside their clip; never a canvas root or what the game screen does not lay out
    // (a UI Editor stage). NullEntity: none. The viewport's double-click opens it in the UI
    // Editor; Lua Editor.ScreenUIAt.
    Entity ScreenUIAt(float x, float y) const;

    // ---- Editor viewport camera view ----
    // 0 = Perspective free-fly; 1.. = axis-locked orthographic views. App reads these each
    // frame (Edit mode only) and overrides the render camera at the SetCamera seam; wheel
    // zoom / RMB pan are handled in the viewport.
    int   GetEditorViewMode() const { return static_cast<int>(m_editorViewMode); }
    float GetOrthoHalfHeight() const { return m_orthoHalfHeight; }
    DirectX::XMFLOAT3 GetOrthoFocus() const { return m_orthoFocus; }

    // ---- Folder operations (content browser) ----
    // Moving an asset is what the identity layer exists for, so each of these carries the
    // references along: the .meta sidecar travels with the file, AssetGuidIndex is repointed,
    // and every container that named the old path is rewritten. A move that skipped any of
    // the three would look fine in the browser and be broken everywhere else.
    // Public so a harness can drive them without a graphics device: they are the most
    // destructive things the browser does (a folder move renames N assets at once, and a
    // missed reference stays silent until someone opens a scene).
    void CreateAssetFolder(const std::string& parentDir, const std::string& name);
    // Create menu, folder context menu and authoring harness share this path.
    std::string CreateAnimationAsset(bool graph, const std::string& directory = {});
    // Move files (not folders) into `destDir`. Returns how many moved.
    size_t MoveAssetsInto(const std::vector<std::string>& srcPaths,
                          const std::string& destDir);
    // Move / rename a whole folder; every asset inside is repointed.
    bool   MoveAssetFolder(const std::string& srcDir, const std::string& destPath);
    bool   DeleteAssetFolder(const std::string& dir);

private:
    // ---- Viewport picking and gizmos ----
    struct GizmoLayerReport
    {
        std::string   owner, coveredBy;
        int           foreground = -1;
        std::uint64_t submits    = 0;
    };

    // ---- UI Rect tool (uGUI-style direct manipulation) ----
    struct UIRectDrag
    {
        bool        active   = false;
        int         handle   = -1;     // 0..7 edges/corners, 8 = move body
        Entity      entity   = NullEntity;
        // Authored values at drag start; every frame recomputes from these plus
        // the total delta rather than accumulating, so a drag cannot drift.
        float       startSizeX = 0.f, startSizeY = 0.f;
        float       startOffX  = 0.f, startOffY  = 0.f;
        ImVec2      startMouse{ 0, 0 };
        // Screen rect at press time. Snapping needs it because UIComputedRect is last frame's
        // layout output and already contains the drag applied so far: probing "current rect +
        // total delta" would count the motion twice and make the rect stick or oscillate near a
        // snap target.
        ImVec2      startMn{ 0, 0 }, startMx{ 0, 0 };
        std::string beforeBlob;        // for the undo entry, captured on press
    };

    // ---- Undo / redo ----
    struct SinkScope
    {
        EditorLayer&                            layer;
        std::shared_ptr<Editor::Undo::EditSink> prev;
        SinkScope(EditorLayer& l, std::shared_ptr<Editor::Undo::EditSink> s) : layer(l), prev(l.m_editSink)
        {
            if (s) layer.m_editSink = std::move(s);
        }
        ~SinkScope() { layer.m_editSink = std::move(prev); }
        SinkScope(const SinkScope&) = delete;
        SinkScope& operator=(const SinkScope&) = delete;
    };

    // ---- Component editor registration ----
    struct ComponentEditorEntry
    {
        std::string                                label;
        std::type_index                            type = std::type_index(typeid(void)); // component type key
        std::string                                category = "Misc"; // Add Component submenu
        std::function<void(void*, World*, Entity)> draw;     // live ptr + context
        std::function<void*(World&, Entity)>       fetch;
        std::function<void(World&, Entity)>        add;      // default-construct
        std::function<void(World&, Entity)>        remove;
        int                                        priority = 100;   // lower = first
        bool                                       defaultCollapsed = false; // true -> CollapsingHeader opens closed
        std::function<bool(World&, Entity)>        condition;        // null = always show
        // Undo (field edits): value-copy snapshot + change-detection fingerprint.
        Editor::Undo::CaptureFn                    captureValue;     // null if T not copyable
        std::function<std::string(World&, Entity)> fingerprint;      // null if T not trivially-copyable
        // The module that registered it ("" = the core): hidden from Add Component and
        // marked in the Inspector while the project leaves that module off.
        std::string                                module;
    };

    // ---- Undo / redo state ----
    struct FieldEditBracket
    {
        bool                    open     = false;
        Entity                  entity   = NullEntity;
        std::type_index         type     = std::type_index(typeid(void));
        ImGuiID                 activeId = 0;   // ImGui active-id that owns the open edit
        Editor::Undo::RestoreFn restoreBefore;  // pre-edit value snapshot
        std::string             fpBefore;       // pre-edit fingerprint (change detection)
        // Serialized pre-edit component, kept ONLY so a multi-selection edit can
        // work out WHICH keys the user changed. RestoreFn is an opaque closure;
        // it can put the value back but cannot be diffed.
        std::string             blobBefore;
        // A copy of the pre-edit component for a reflected type (SnapshotReflected): the gesture
        // commits as the fields it changed.
        std::shared_ptr<void>   snapshot;
        // The edit may reach the rest of the selection (the edited entity is in it).
        bool                    propagate = false;
    };

    // ---- Asset browser ----
    struct AssetEntry
    {
        std::string             path;       // full filesystem path
        std::string             stem;       // filename without extension
        std::string             name;       // filename WITH extension (List/Details views)
        AssetType               type        = AssetType::Unknown;
        // Streamed from disk by path. For a Texture that is the asset itself; for a Material it
        // is the cached .itex tile under Sys::WritableDataDir()/cache/thumbnails - the same
        // loader, 4-per-frame budget and eviction, so a cache hit needs no new code path.
        Resource::TextureHandle thumbnail   = Resource::kInvalidTextureHandle;
        float                   lastVisibleTime = -1.f;        // ImGui time last in-view; <0 = never
        uint64_t                sizeBytes   = 0;               // 0 for folders / stat failure
        int64_t                 mtime       = 0;               // time_t of last write; 0 = unknown

        // ---- The rendered material tile ----
        // Deliberately not `thumbnail`: that is a Resource::TextureHandle owned by TextureSystem
        // and released by path; this is a texture the browser creates and destroys outright,
        // because nothing on disk backs it until the readback ring has handed its pixels to the
        // encoder.
        RHI::Texture previewTile;                  // invalid = none
        // Stable identity across a rescan (an index into m_assets is not): the
        // low 64 bits of the asset's guid, or a hash of its canonical path when
        // it has no .meta sidecar. 0 = not resolved yet.
        uint64_t     previewKey   = 0;
        // 32-char hex guid, or empty when the asset is unstamped. Empty means
        // MEMORY-ONLY: a tile with no stable identity has nothing to key a disk
        // entry on, and stamping one here would write a .meta sidecar for every
        // material a churn happens to scroll past.
        std::string  previewGuid;
        // Bumped by every invalidation (a chain save, a texture re-import, a shader edit). A
        // result whose generation no longer matches is dropped rather than marking the tile
        // clean.
        uint32_t     previewGen   = 0;
        // Wants a (re)render. Set by the visibility sweep and by every invalidation; cleared only
        // at apply. Clearing it at enqueue would let a record early-out drop the asset out of the
        // queue for the rest of the session.
        bool         previewDirty = false;
        // The tile has pixels. Until then the tile texture is zero-filled and
        // must not be drawn, or a freshly created tile shows as a black square.
        bool         previewReady = false;
        // The lane returned ok=false: it gave up waiting for this material's textures, or the
        // record half refused it too many times. Without this the visibility sweep re-arms
        // previewDirty on the next frame and one broken material occupies the FIFO head forever.
        // Cleared by any invalidation, the signal that something actually changed.
        bool         previewGaveUp = false;

        // A Texture's DescribeTextureFile, read on first need (the tooltip, the Details columns)
        // and dropped with the entry at the next rescan; never carried across one.
        std::shared_ptr<const Resource::TextureFileInfo> texInfo;
    };

    // ---- Folder tree (left pane of the content browser) ----
    struct AssetFolderNode
    {
        std::string      name;              // display name (leaf directory name)
        std::string      path;              // full filesystem path, as enumerated
        std::vector<int> children;          // indices into m_folderTree
        int              assetCount = 0;    // assets directly in this folder
    };

    // ---- Import (textures, models, dropped files) ----
    struct PendingTextureImport
    {
        std::string sourcePath;
        std::string outputPath;
        Resource::TextureImportFormat suggestedFormat = Resource::TextureImportFormat::BC7_Linear;
        Resource::TextureImportFormat selectedFormat  = Resource::TextureImportFormat::BC7_Linear;
        // The rest of the import's choices (maxSize, mipFilter, flipGreen, generateMips); its
        // format is selectedFormat.
        Resource::TextureImportSettings options;
    };

    struct TextureImportOutcome
    {
        bool          ok = false;
        std::string   code, message;        // why it failed
        std::size_t   bytes = 0;
        std::uint32_t width = 0, height = 0;
        std::string   guid;                 // hex; "" when the path is not tracked
        std::string   reload;               // "requested" | "not_loaded" | "unchanged"
        std::uint64_t reloadJobId = 0;      // its receipt: a "hotreload.texture" job
        bool          record = false;       // the import.* record was written
    };

    // One model import, shared by File > Import > Model and the scripted Editor.Import: PMX
    // and VRM by their own importers, every other format through assimp (SceneImporter), then
    // Resource::WriteModelImportFiles - the one writer, which keeps an existing .imat and
    // carries the previous .iscn's material bindings over, so a reimport keeps what was
    // authored since. The .iscn's identity is kept, or minted on a first import.
    struct ModelImportOutcome
    {
        bool                     ok = false;
        std::string              code, message;   // why it failed
        std::vector<std::string> files;           // as the importer named them, relative to the directory
        std::string              iscn;            // the model's .iscn (root-relative); "" when none
        std::string              guid;            // the .iscn's, hex
        int written = 0, failed = 0, keptMaterials = 0, restoredBindings = 0;
        // Meshes given the source's collision, hulls appended, authored collision kept.
        int collisionMeshes = 0, collisionHulls = 0, keptCollision = 0;
        // Gone Meshes' authored collision kept as orphan=1.
        int orphanCollision = 0;
        // Meshes whose MK / MP records a re-import carried over (and kept as orphan).
        int keptShapeKeys = 0, orphanShapeKeys = 0;
        // The source's own records written (src=): presets and default weights.
        int sourcePresets = 0, sourceDefaults = 0;
        // The source's textures written.
        int textures = 0;
        // What the re-import did to the model's Meshes and sections (or, a dry run, would do),
        // and the SR record written.
        bool                           dryRun = false;
        Resource::MeshIdentityAdoption identity;
        Resource::ModelSourceRecord    source;
        // The options it ran with (the IM record written), Meshes given made collision, source
        // materials bound to a project .imat.
        Resource::ModelImportOptions   options;
        int madeCollision = 0, reusedMaterials = 0;
    };

    // What one model import is asked.
    struct ModelImportRequest
    {
        std::string                       source, outDir;
        std::string                       stem;             // "" = the source's file stem; a re-import: the model's
        std::vector<Resource::MeshRename> renames;          // explicit: the author's word
        bool                              geometryRenames = true;
        bool                              dryRun          = false;   // the identity report only, nothing written
        // The import options; none = the model's IM record when it is a re-import (the folder
        // has the model), else the defaults.
        std::optional<Resource::ModelImportOptions> options;
    };
    // The importer's output (the blobs), held between a review and its commit:
    // Prepare reads and imports - nothing is written - and Commit hands the
    // blobs to WriteModelImportFiles (a dry run first, when a review wants the
    // report).
    struct PreparedModelImport;

    struct ModelReimportReview
    {
        std::shared_ptr<PreparedModelImport> prepared;
        ModelImportRequest                   request;
        ModelImportOutcome                   report;       // the dry run's
        std::string                          model;        // the .iscn
        std::vector<char>                    keepRename;   // per report.identity.renamedMeshes: keep the key
        std::uint64_t                        jobId     = 0;
        std::uint32_t                        requestId = 0;
    };

    struct ModelImportDialog
    {
        std::vector<std::string>     sources;
        std::string                  outDir;
        std::string                  reimport;
        Resource::ModelImportOptions options;
        std::uint64_t                jobId     = 0;
        std::uint32_t                requestId = 0;
    };

    struct ModelImportTask
    {
        ModelImportRequest                   request;
        std::string                          reimport;         // the model: the review flow (a dry run first)
        std::uint64_t                        jobId     = 0;
        std::uint32_t                        requestId = 0;
        std::shared_ptr<PreparedModelImport> prepared;         // the worker's, read once `finished`
        ModelImportOutcome                   failure;          // a prepare that failed
        std::atomic<bool>                    finished { false };
        double                               startedAt = 0.0;  // seconds (the overlay's clock)
        std::string                          label;            // the source's file name
    };

    struct DroppedFiles
    {
        std::vector<std::string> paths;
        int                      x = 0, y = 0;
    };

    struct ModelTileRect
    {
        float       x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        std::string model;
    };

    struct ScriptedImport
    {
        PendingTextureImport item;
        bool                 model     = false;
        std::uint64_t        jobId     = 0;
        std::uint32_t        requestId = 0;
        // A model's options (renames, geometryRenames, dryRun); for Editor.Reimport the model
        // (`reimport`, resolved from its SR record when the job runs, or from `sourceOverride`)
        // and `review` (the Content Browser's flow: the review popup when the report asks for
        // one).
        ModelImportRequest   modelRequest;
        std::string          reimport;
        std::string          sourceOverride;
        bool                 review = false;
        bool                 dialog = false;   // the options dialog first
    };

    // An Editor.Play / Pause / Step / Stop job (ProcessScriptedPlay).
    struct ScriptedPlay
    {
        std::uint64_t jobId       = 0;
        EngineMode    target      {};
        bool          resumed     = false;   // Play from Paused
        std::uint64_t queuedFrame = 0;       // Logger::CurrentFrame() when queued
    };

    // ---- Unreal scene-manifest import (Import > Unreal Scene...) ----
    struct UeImportJob
    {
        Resource::UnrealSceneImporter::Progress progress;
        Resource::UnrealSceneImporter::Result   result;
        std::atomic<bool> running  { false };
        std::atomic<bool> finished { false };
        bool rescanQueued = false;   // UI thread only
    };

    // ---- Rendered material tiles ----
    struct CubeThumb { std::uint32_t viewId = ~0u; std::uint64_t lastFrame = 0; };

    static constexpr std::size_t kMaxCubeThumbs = 32;

    struct PendingThumbInfo
    {
        std::string  srcPath;
        std::string  guidHex;
        uint32_t     generation = 0;
        std::vector<Editor::PreviewThumbnailCache::Dep> deps;
    };

    struct ThumbPassBase
    {
        uint64_t rendered = 0;
        uint32_t applied = 0, hits = 0, writes = 0, evicted = 0;
        uint32_t rescans = 0, orphans = 0;
    };

    struct ChurnFolder { std::string path; uint32_t materials = 0; uint64_t dwell = 0; };

    // ---- Loaded Textures window (Window > Analysis) ----
    struct LoadedTextureRow
    {
        std::string   path;
        std::uint32_t width = 0, height = 0, depth = 1, mips = 0, arraySize = 0;
        bool          cube = false;
        std::string   format;
        bool          compressed = false;
        std::uint64_t bytes = 0;
        std::uint32_t refs = 0, users = 0;
        int           state = 0;                 // 0 loading, 1 ready, 2 failed
        std::string   reason;
    };

    // ---- Scene save state ----
    enum class PendingSceneOp { None, New, Load, Exit };

    // ---- Autosave / crash recovery ----
    static constexpr double kDefaultAutosaveIntervalSec = 180.0;   // 3 min between writes
    static constexpr int    kAutosaveKeep        = 3;

    // ---- Editor preferences (persist across sessions) ----
    static constexpr int kMaxRecentScenes = 8;
    static constexpr int kCameraBookmarks = 4;

    struct CameraBookmark
    {
        bool              valid = false;
        DirectX::XMFLOAT3 position{};
        float             yaw = 0.f, pitch = 0.f;
    };

    // ---- Deferred collision-baker preview swap ----
    enum class PendingBakePreviewOp : uint8_t { None, Load, Restore };

    // ---- Component authoring helpers ----
    // One-shot migration: WaterComponent -> the mesh-driven Water material (Plane +
    // Domain=Water + optional WaterInteractionComponent). @p dryRun logs the mapping without
    // mutating anything.
    void ConvertLegacyWater(bool dryRun);
    // Assign a dropped .ianim as the authored AnimationSet "Default" entry
    // and queue name-based playback. Async resource ownership stays in
    // AnimationClipSystem rather than an editor-local pending vector.
    void AssignAnimationAsset(Entity target, const std::string& path);
    // Full inspector for a ScriptComponent: one collapsible section per attached script slot
    // (path drag-drop + enabled + exposed vars + remove), plus an "+ Add Script" button. The
    // component holds a dynamic list of scripts, which a static reflected UI cannot draw.
    void DrawScriptComponentSlots(ScriptComponent& sc, World* world, Entity e);
    // Inspector block for ONE script slot's editor-exposed variables. Reads the
    // schema from m_scriptSys for that slot's path, renders one typed ImGui
    // widget per variable, and writes edits into that slot's override map
    // (pushing live during Play). No-op without a ScriptSystem or `exposed` table.
    void DrawScriptVarWidgets(ScriptComponent& sc, std::size_t slot, World* world, Entity e);

    // ---- Viewport picking and gizmos ----
    // The entity a viewport click on @p hit selects (a derived Static Mesh section counts as its
    // owner): its prefab instance root, else the nearest authored node (scene node or Static
    // Mesh owner) at or above it, else its model root (SceneSourcePath), else the hierarchy
    // root. NullEntity for a dead @p hit.
    Entity ViewportSelectionRoot(Entity hit) const;

    // Left-click pick + left-hold drag inside the viewport. @p allowBonePick: only the
    // docked-panel path may consume a click for bone picking, because only it also submits
    // the bone gizmo; the F11 fullscreen path passes false, or a click on a bone there would
    // be swallowed with no handle ever appearing.
    void HandleViewportPicking(float imageMinX, float imageMinY,
                               float contentW,  float contentH,
                               bool  hovered,
                               bool  allowBonePick = false);
    // ImGuizmo transform gizmo for the selected entity.
    void RenderViewportGizmo(ImVec2 vpMin, ImVec2 vpMax);
    // Edit Collider (EditorLayer_ColliderGizmo.cpp): the viewport gizmo drives the selected
    // entity's ColliderComponent (offset, rotation, and a box / sphere / capsule's size; a box
    // scales by its faces, ImGuizmo BOUNDS) instead of its transform, in the frame
    // PhysicsSystem builds the shape in. One drag = one undo step (the component's blob
    // before / after). False = not applicable (off, Play, no collider, a mirrored entity):
    // the transform gizmo draws instead.
    bool RenderColliderGizmo(ImVec2 vpMin, ImVec2 vpMax);
    void CommitColliderDrag();

    void DrawIKRigInspector(void*,World*,Entity);
    void RenderIKTargetGizmo(ImVec2 vpMin,ImVec2 vpMax);

    // ImGuizmo hit-tests against geometry cached inside Manipulate, and neither BeginFrame
    // nor Enable(false) clears it. On a frame that submits no gizmo (no GlobalTransform, a
    // Canvas node, or the F11 layout, which never submits one) the raw IsOver() /
    // IsUsingAny() keep answering from the last entity that had one: an invisible
    // click-swallowing hole in the viewport. Ask these instead.
    bool GizmoIsOver() const
    { return m_gizmoSubmittedThisFrame && ImGuizmo::IsOver(); }
    bool GizmoIsUsingAny() const
    { return m_gizmoSubmittedThisFrame && ImGuizmo::IsUsingAny(); }
    // Both main-viewport gizmos draw into the Viewport window's own draw list, never
    // ImGui::GetForegroundDrawList(): the foreground list sits above every window, so the
    // gizmo would paint over a floating panel, popup or menu covering the viewport. The window
    // list is clipped to the viewport and covered by anything above it (callers also clip it
    // to the scene image, vpMin..vpMax). SetAlternativeWindow blocks hover under a covering
    // window; the window list also avoids a spurious hover when the mouse is over no window
    // ('##Foreground' names no window, so a NULL hovered window matches it).
    // Call at the Viewport window's level (RenderViewportPanel, not inside a child).
    // @p which = "transform" | "restpose"; @p center = the gizmo's screen position (for the
    // self-report). -editor_gizmo_foreground (harness reverse arm) draws into the foreground
    // list instead.
    ImDrawList* ViewportGizmoDrawList(const char* which, const ImVec2& center);

    // ---- UI Rect tool (uGUI-style direct manipulation) ----
    // Draws the selected Canvas node's resolved rect plus 8 resize handles and a move body,
    // and writes drags back into its UIRect.
    // Separate from RenderViewportGizmo, which is ImGuizmo: that needs a GlobalTransform and
    // works in 3D, while UI nodes have no transform and are manipulated in 2D canvas space
    // (Unity's Rect Tool).
    // @p imageTopLeft is where the scene image starts on screen: UIComputedRect is in canvas
    // space (viewport-local pixels), so screen = imageTopLeft + canvas. Both viewport layouts
    // call it, hence the parameter.
    // @p otherItemActive means "an ImGui widget other than the viewport's own click surface is
    // active"; the tool refuses to start a drag while one is. A parameter, not a member, on
    // purpose: the docked path must exclude its "##viewport" InvisibleButton (which takes
    // ImGui's ActiveId on mouse-down, before this runs), while the F11 path has no such
    // button and passes IsAnyItemActive() through; a member would let one path inherit the
    // other's value.
    void RenderUIRectTool(ImVec2 imageTopLeft, bool otherItemActive);
    // Ends the current rect gesture, pushing its single undo entry if anything changed. Every
    // exit from RenderUIRectTool goes through here, not just the clean release (see the
    // definition).
    void CommitUIRectDrag();

    // ---- Undo / redo ----
    // Brackets a continuous gizmo drag into one undo entry: sampled once per frame at the top
    // of OnUIRender, it caches the selected entity's clean LocalTransform while idle and, on
    // the ImGuizmo::IsUsing() falling edge, pushes a single TransformEditAction(before ->
    // after).
    void UpdateTransformEditTracking();
    void CommitTransformEdit();
    // The one shortcut dispatcher (EditorLayer_Commands.cpp): the registry's chords against this
    // frame's keys, after the text-input / popup / capture gates and the focused panel's own keys.
    void HandleUndoShortcuts();

    // ---- Commands and documents (EditorLayer_Commands.cpp) ----
    // Built once, on first use: the documents Save / Undo / Redo act on, every editor command, the
    // user's chord overrides.
    void EnsureCommandsRegistered();
    void RegisterDocuments();
    void RegisterEditorCommands();
    // A menu item for command @p id: its label (or @p overrideLabel), icon, chord (or the chord
    // of command @p chordOf), checked and enabled state, tooltip. Runs the command when clicked.
    bool CommandMenuItem(const char* id, const char* overrideLabel = nullptr, const char* chordOf = nullptr);
    // editor_prefs.ini "keymap.<id>=<chord>" <-> the registry's user chords.
    void ApplyKeymapPrefs();
    void StoreKeymapPrefs();
    void EditUndo();
    void EditRedo();
    // Undo recording + apply are gated to Edit mode (never during Play).
    bool UndoEnabled() const;
    // The single choke point for undo recording: pushes onto the history and marks the scene
    // dirty. Every m_editHistory.Push in EditorLayer routes through here: an edit you can
    // Ctrl+Z is by definition an unsaved change, and one place keeps the dirty flag out of
    // the component editors.
    void PushEdit(std::unique_ptr<Editor::Undo::IEditAction> action);

    // While a UI Editor asset tab edits its stage, the edit helpers - PushEdit, the macros,
    // SyncedEditContext, DeleteEntities, MoveUINode, DuplicateEntity, the Inspector's field
    // bracket, Add / Remove Component - target that tab's own ring (EditSink) and mark it
    // dirty, never the scene. EditTarget() is the history in scope; SinkScope installs a sink
    // for one call (the UI Editor's context callbacks, Entity.set on a stage node). Inside
    // Editor.Batch, an asset tab's ring an edit reaches joins the batch (a macro of its own,
    // ended or aborted with the scene's: JoinBatch).
    Editor::Undo::EditHistory& EditTarget()
    {
        if (!m_editSink) return m_editHistory;
        if (m_batchDepth > 0) JoinBatch(m_editSink);
        return m_editSink->history;
    }
    void        JoinBatch(const std::shared_ptr<Editor::Undo::EditSink>& sink);
    std::size_t AbortBatchSinks();   // undo what the batch did in each ring it reached
    void        EndBatchSinks();     // commit each ring's batch step

    // ---- Scene save / dirty tracking ----
    // A change the scene's undo stack cannot reverse: the scene stays dirty until the next save,
    // whatever undo does. With a sink in scope the change is the sink owner's (its asset), not
    // the scene's. An edit recorded with PushEdit uses SyncSceneDirtyToHistory instead.
    void MarkSceneDirty()
    {
        if (m_editSink)
        {
            if (m_editSink->changed) m_editSink->changed();
            return;
        }
        m_editHistory.MarkUnrecoverable();
        m_sceneDirty = true;
    }
    // After an edit, undo or redo on the scene history: dirty unless the history is back at the
    // state last saved or loaded. An open macro has collected edits its serial does not show yet.
    void SyncSceneDirtyToHistory()
    {
        // -uitest_reverse=history.save_point: once dirty, dirty until a save, whatever the undo did.
        const bool sticky = m_sceneDirty && Editor::UITests::Reverted(Editor::UITests::Fix::SavePoint);
        m_sceneDirty = sticky || m_editHistory.MacroOpen() || !m_editHistory.AtSavePoint();
    }
    // The live world is what is on disk now: a successful save, a load, a new scene.
    void MarkSceneClean()
    {
        m_editHistory.MarkSaved();
        m_sceneDirty = false;
    }
    // Writes the live world to @p path (baked-probe cubemap export + SaveScene). On success
    // adopts @p path as the current scene and clears the dirty flag. Refuses in Play mode (the
    // definition says why). Run it from ProcessPendingSaveScene, never from an ImGui handler:
    // the baked-probe export submits its own out-of-band command list. @p errorCode
    // (optional) names a failure: play_mode, no_world, empty_world, open_failed, write_failed
    // (Resource::SaveScene).
    bool SaveSceneToPath(const std::string& path, std::string* errorCode = nullptr);
    // Queue a save for the frame boundary. Returns whether the request was accepted (path
    // known, world present, not Playing); a disk write failure surfaces later, from
    // ProcessPendingSaveScene.
    bool RequestSaveScene(std::string path);
    // Frame-boundary half of the save; called from ProcessPendingActions.
    void ProcessPendingSaveScene();
    // File > Save Scene: straight to m_currentScenePath when there is one, else the Save As
    // dialog. Both queue the write.
    bool SaveSceneNow();
    bool SaveSceneAs();
    // Guarded entry points for the two destructive scene ops: raise the
    // unsaved-changes modal when dirty, otherwise queue the work immediately.
    void RequestLoadScene(std::string path);
    void RequestNewScene();
    // Save / Discard / Cancel modal. Draw its body outside the menu-bar scope (SetupDockSpace,
    // next to RenderTextureImportPopup): a modal opened from inside BeginMenuBar / EndMenuBar
    // needs its BeginPopupModal in the caller's ID stack, not the menu bar's.
    void RenderUnsavedChangesPopup();
    // Performs whatever the modal was blocking once the user answered.
    void RunPendingSceneOp();

    // ---- Unsaved documents (EditorLayer_Documents.cpp) ----
    // One open document with edits not on disk, held by an editor window. The scene is not one
    // of them (m_sceneDirty).
    struct UnsavedDocument
    {
        std::string owner;            // the window holding it ("Material Editor")
        std::string name;             // its file, or what the window calls it
        bool        saveable = true;  // false: only its own panel can commit it (Chain Preview's Apply)
    };
    // Every window's unsaved documents. Asset documents survive a scene switch, so only an exit
    // asks about them.
    std::vector<UnsavedDocument> CollectUnsavedDocuments();
    // GAME half (PPA): writes every saveable unsaved document. False with @p why when one could
    // not be written or one can only be committed from its own panel.
    bool SaveUnsavedDocuments(std::string& why);
    // The modal's Save, run before the scene save: a failure cancels the exit it was guarding.
    void ProcessPendingSaveDocuments();
    // The VFX editor's document (VFXEditorWindow.cpp): "" when it has no unsaved edits.
    std::string UnsavedVfxPath() const;
    bool        SaveVfxNow(std::string& why);
    // Deferred New Scene teardown: runs from ProcessPendingActions for the same
    // descriptor-lifetime reason as the deferred load.
    void ProcessPendingNewScene();
    // Deferred collision-baker preview swap (MeshLibrary Load / Release +
    // Renderer::OnWorldClear). Same slot, same reason.
    void ProcessPendingBakePreview();

    // Duplicate / copy / paste (Ctrl+D / Ctrl+C / Ctrl+V). Duplicate clones the
    // subtree as a sibling and records one undoable create; copy remembers the
    // selected entity; paste duplicates the remembered entity.
    Entity DuplicateEntity(Entity src);
    // Skinned-character duplicate: re-instantiates the entity's .iscn (GPU skinning state
    // cannot be value-copied) and replays the source subtree's authored components onto it.
    // NullEntity when @p src has no usable SceneSourcePath; DuplicateEntity then falls back to
    // the generic clone. @p srcToDst (optional) collects source -> copy pairs for the
    // reference remap.
    Entity DuplicateSceneInstance(Entity src, Entity newParent,
                                  std::unordered_map<Entity, Entity>* srcToDst = nullptr);
    void   CopySelected();
    void   PasteClipboard();

    // Inspector field-edit undo. Wraps any component editor's draw call (reflected or
    // hand-written: Transform, Material, ...) and brackets a field edit into one value-copy
    // undo entry. Coalescing keys on ImGui's global active id: a widget going active opens the
    // bracket (a value-copy snapshot via `capture`), and the active id clearing commits it,
    // pushing only if `fingerprint` (or the serializer blob) shows a real change.
    void DrawComponentWithUndo(Entity e, std::type_index type,
                               const Editor::Undo::CaptureFn& capture,
                               const std::function<std::string(World&, Entity)>& fingerprint,
                               const std::function<void()>& drawBody);
    // Cheap change-detection key: the entry's byte fingerprint, else the serializer blob,
    // else "" (unavailable -> the caller assumes always changed).
    std::string ComponentFingerprint(const std::function<std::string(World&, Entity)>& fingerprint,
                                     Entity e, std::type_index type);
    // Closes a field-edit bracket left open by a selection change / entity
    // destroy (the normal close happens in-draw on the release frame).
    void FinalizeStaleFieldEdit();
    // Display label for a component type (for undo entry labels).
    std::string ComponentLabelFor(std::type_index type) const;
    // Refreshes the EditHistory's context (world/registry/assets) and returns it.
    Editor::Undo::EditContext& SyncedEditContext();

    // Runs `spawn` and records any entities it created as ONE undoable entry
    // (diffs World::GetEntities() before/after; groups multi-root spawns into an
    // EditMacro). Used for menu-create primitives and prefab/scene drag-drop.
    void RecordSpawn(const char* label, const std::function<void()>& spawn);

    // Point `e`'s material at the .imat / .imatinst `path` as ONE undoable edit
    // (value copies of MaterialComponent, MaterialSourcePath,
    // MaterialInstanceInfo and MaterialOverride before and after). Shared by the
    // viewport drop and Editor.AssignMaterial. False + *error when the entity
    // has no MaterialComponent or the file does not load.
    bool AssignMaterial(Entity e, const std::string& path, std::string* error = nullptr);
    // A StaticMesh owner's material for ONE slot (its SectionKey) as one
    // undoable edit of the owner's StaticMeshComponent: the slot override is
    // set to `path`, or removed with an empty `path` (back to the model's
    // default). MeshInstanceSystem rebuilds the sections from it. False +
    // *error when `owner` has no StaticMeshComponent or the file does not load.
    // recordUndo = false: the caller's undo bracket records the change (a
    // widget of the Static Mesh inspector, which DrawComponentWithUndo wraps).
    bool AssignStaticMeshSlot(Entity owner, uint64_t sectionKey, const std::string& path,
                              std::string* error = nullptr, bool recordUndo = true);
    // The entity a click on `e` means: the owner of a derived StaticMesh
    // section (never a selection target of its own), else `e`.
    Entity SelectableEntity(Entity e) const;
    // The Mesh Editor's view of this class (renderer, device, editor frame, play state, world
    // epoch, the thumbnail and Content Browser hooks).
    Editor::MeshEditorContext MeshEditorCtx();
    // What the Skeleton Editor needs from this class.
    Editor::SkeletonEditorContext SkeletonEditorCtx();
    // What the Texture Viewer needs from this class.
    Editor::TextureViewerContext TextureViewerCtx();
    // What the UI Editor needs from this class.
    Editor::UIEditorContext UIEditorCtx();
    // What the Material Editor needs from this class.
    Editor::MaterialEditorContext MaterialEditorCtx();

    // The Inspector's "Static Mesh" block: model (+ .iscn drop), mesh picker,
    // collision mode, per-slot materials (+ .imat drop), resolution status.
    void DrawStaticMeshInspector(struct StaticMeshComponent& sm, World* w, Entity e);
    // Re-parent `child` under `newParent` (NullEntity = root), keeping its world
    // pose, as ONE undoable edit - the Outliner drag's rules (no dead parent, no
    // cycle). EditorLayer_Outliner.cpp, where the hierarchy helpers live.
    bool ReparentWithUndo(Entity child, Entity newParent, std::string* error = nullptr);
    // A Canvas UI node under the UI node @p newParent (NullEntity = its canvas) at @p index
    // among its UI children in paint order (-1 = last), siblingIndex renumbered, as one undo
    // step. The Outliner drag / drop zones, Unparent, Editor.Reparent and the UI Editor all
    // move UI nodes through it.
    bool MoveUINode(Entity child, Entity newParent, int index, std::string* error = nullptr);
    // The Outliner's children of `e`, in the order it draws them (Lua Editor.Children).
    std::vector<Entity> OutlinerChildrenOf(Entity e);
    // A spawned prefab whose root is a UI node becomes a detached copy and (a UIRect root)
    // goes under @p target (NullEntity = the selection's canvas, else the first one). False =
    // not a UI root (place it in 3D); @p why = a UIRect root that found no canvas to go
    // under.
    bool AdoptUIPrefabRoot(Entity root, Entity target, std::string* why = nullptr);
    // A canvas the scene authors - not a UI Editor stage's, not an off-screen one, not a UI
    // Document instance's: the only kind a "put it under some canvas" fallback (Create > UI, a
    // UI prefab drop) may pick; anything else is in no scene file, or is replaced at the next
    // respawn.
    static bool AuthoredCanvas(const World& world, Entity canvas);
    // Selection setter routed to structural undo/redo (restores focus on a
    // resurrected root, clears it on delete).
    void SetSelectionFromUndo(Entity e);

    // World-space spawn point for newly created / dropped entities, so they appear in the
    // current view instead of at the origin. @p vpX / @p vpY are viewport-relative pixel
    // coords of the point of interest (e.g. the drop cursor); negative = the viewport center
    // (menu-created entities have no cursor target). Casts a ray through that point: snaps to
    // the ground plane (Y=0) when it hits within range, else places at a fixed distance in
    // front of the camera.
    DirectX::XMFLOAT3 ComputeSpawnPosition(float vpX = -1.f, float vpY = -1.f) const;

    // A StaticMesh owner for (model, key) at `position`: what a Content Browser Mesh drop,
    // "Place in Scene" and Editor.PlaceMesh create. Not recorded: callers wrap it in
    // RecordSpawn. MeshInstanceSystem derives its sections next frame.
    Entity CreateStaticMeshEntity(const std::string& model, uint64_t meshKey, const std::string& meshName,
                                  const DirectX::XMFLOAT3& position);
    // Parse a "STATICMESH_REF" drag payload ("model\nkeyhex\nname"). False on
    // anything else.
    static bool ParseStaticMeshRef(const char* data, std::string& model, uint64_t& key, std::string& name);

    // ---- Multi-selection ----
    // The per-frame collapse of the contract above GetSelectedEntities.
    void ReconcileSelection();

    // Apply one gizmo result to ONE entity. `newWorld` is that entity's target
    // world matrix; `deltaWorld` is this frame's incremental world transform
    // (ROTATE composes against it rather than re-decomposing an absolute
    // matrix, whose q / -q choice is not temporally stable).
    void ApplyGizmoToEntity(Entity target,
                            const DirectX::XMFLOAT4X4& newWorld,
                            const DirectX::XMMATRIX&   deltaWorld,
                            bool isPrimary);

    // Push the keys the user just changed on `primary` onto every other
    // selected entity that has the same component. Key-level, never whole-blob:
    // copying the blob would overwrite fields nobody touched, silently
    // flattening the rest of the selection onto the primary's values.
    void PropagateFieldEditToSelection(Entity primary, std::type_index type,
                                       const std::string& blobBefore,
                                       const std::string& blobAfter);

    // ---- Reflected property edits (EditorLayer_Properties.cpp) ----
    // A copy of the component, for a field diff when the gesture ends; null when @p type has no
    // descriptor that can copy it.
    std::shared_ptr<void> SnapshotReflected(std::type_index type, const void* comp) const;
    // Records the gesture that turned @p before into @p e's live component as one PropertyEditAction
    // per changed field, and (with @p propagate) writes those fields - only those - onto the rest of
    // the selection. False when the edit is not exactly a set of writable fields (a field the
    // descriptor does not cover changed, a vector changed length, a custom field changed): the caller
    // then records the whole value and propagates by serializer keys.
    bool CommitReflectedEdit(Entity e, std::type_index type, const void* before, bool propagate);
    // The fields of @p type whose value differs across the selection (drawn mixed).
    std::vector<const Reflect::FieldDescriptor*> MixedReflectedFields(Entity primary, std::type_index type) const;
    // The serializer tag of a component type ("RigidBody"; "Transform" for LocalTransform), "" for none.
    static const char* PropertyTagOf(std::type_index type);
    // The Inspector's top row: the field search, Modified only, Advanced, and the lock.
    void DrawInspectorToolbar();
    // The reflection drawers' editor hooks: entity names for EntityRef fields, the asset picker.
    void InstallReflectionHooks();

    // ---- Panels and services (EditorLayer_Panels.cpp) ----
    // The core's tool panels into Editor::PanelRegistry (once), then the modules' rows, then the
    // saved open states and workspaces.
    void EnsurePanelsRegistered();
    // At the frame boundary: the registry's per-frame hook, then its world / play / selection events.
    void TickPanelLifecycle();
    // The Window menu's Workspaces submenu.
    void RenderWorkspacesMenu();
    // The Content Browser's results over every folder (m_assetSearchAll): the catalog's entries whose
    // name matches the filter; a click goes to the asset in its folder.
    void RenderAssetSearchResults();
    // The editor's Editor::Services adapters (selection, documents, undo, notifications, jobs).
    void InstallServices();
    // The Jobs panel and the notice a finished job posts (EditorLayer_Jobs.cpp).
    void RenderJobsPanel();
    void TickJobNotices();
    // The Byte View panel (EditorLayer_ByteView.cpp): a component's or an asset file's bytes, read-only.
    void OpenByteViewForComponent(Entity e, std::type_index type);
    void OpenByteViewForFile(const std::string& path);
    void TickByteView();
    void RenderByteViewWindow();

    // ---- Game View (EditorLayer_GameView.cpp) ----
    // A Game View panel's frame (0 .. Editor::GameView::kMaxPanels - 1): its inputs from the editor's state, then
    // Editor::GameView::Draw.
    void DrawGameViewPanel(int panel);
    // SetupDockSpace's half of the Game Views' dock requests: split the Scene viewport's node, or join it.
    void ApplyGameViewDock();
    // The Scene viewport's picture on the Game route: the editor camera through its own secondary view.
    void DrawSceneAuxPicture(ImDrawList* dl, const ImVec2& vpMin, const ImVec2& vpMax);
    // Editor.GameView() / Editor.SetGameView{...}.
    void RegisterGameViewLua(struct lua_State* L);

    // ---- Morph editor and chain colliders ----
    void OpenMorphEditor(Entity entity);
    void FinishMorphAuthoredEdit();
    void DrawMorphEditor();
    void DrawChainColliders();
    EntityHandle ChainColliderTarget() const;

    // ---- Asset browser ----
    bool IsAssetSelected(int index) const;
    void SelectAsset(int index, SelectMode mode);
    void ClearAssetSelection();
    // Paths of every selected asset that still exists, primary last.
    std::vector<std::string> SelectedAssetPaths() const;

    // ---- Folder operations (content browser) ----
    // Shared tail: fix up references + sidecars for a set of (old, new) paths.
    void   RepointMovedAssets(const std::vector<std::pair<std::string, std::string>>& moves);
    // The reference index's roots beyond asset/: engine content, plus the
    // harness fixture directory -refscan_root=<dir> when given.
    std::vector<std::string> RefScanExtraRoots() const;
    // Drop target for a folder row/tile. Accepts the ASSET_MOVE payload the
    // browser drag source publishes.
    void   AcceptAssetMoveDrop(const std::string& destDir);
    // Run whatever drag-and-drop / the context menus queued, once per frame
    // AFTER the browser has drawn: every one of these mutates the lists the
    // traversal is walking.
    void   ApplyPendingFolderOps();
    // Right-click menu for a folder row/tile: new / rename / delete.
    void   RenderFolderContextMenu(const std::string& dir, bool isRoot);

    // ---- Rendered material tiles ----
    uint64_t MaterialRowThumb(const std::string& path);
    template <class Fn> void ForEachPreviewEntry(Fn&& fn)
    {
        for (AssetEntry& ae : m_assets)       fn(ae);
        for (auto& kv : m_meshTiles)          fn(kv.second);
        for (auto& kv : m_materialRowTiles)   fn(kv.second);
    }

    // ---- Import (textures, models, dropped files) ----
    // One texture import, shared by the review popup's confirm and the scripted
    // Editor.Import: read the source, import it with the chosen format, write the .itex
    // atomically (a failure leaves the old file whole), keep - or on a first import mint - its
    // .meta identity, and reload in place the texture the running engine already draws from
    // that path (TextureSystem::RequestReload), so a reimport shows up without reopening
    // anything. Every import also records how it was made (the source, the settings) as the
    // .meta's import.* keys (Resource/TextureImportRecord.h); Editor.Reimport, the Content
    // Browser's Reimport and the Texture Viewer's Import section read them back.
    TextureImportOutcome ImportTextureFile(const PendingTextureImport& item);
    // Reimport @p itex from its record (or the guess for a file without one), with
    // @p sourceOverride replacing the source: a job-less scripted import (the Content
    // Browser's Reimport / Reimport From...).
    void QueueTextureReimport(const std::string& itex, const std::string& sourceOverride);

    // Static: it runs on a TaskSystem worker, touching nothing of the editor's. A failed
    // prepare returns null and fills @p out's code / message.
    static std::shared_ptr<PreparedModelImport> PrepareModelImport(const ModelImportRequest& req,
                                                                   ModelImportOutcome& out);
    ModelImportOutcome CommitModelImport(const PreparedModelImport& prep, const ModelImportRequest& req);
    ModelImportOutcome ImportModel(const ModelImportRequest& req);
    // ImportModel with only @p source and @p outDir set, the rest at their defaults.
    ModelImportOutcome ImportModelFile(const std::string& source, const std::string& outDir);

    // Reimport: a model back through the import it came from. The model is the importer's
    // layout, <dir>/<stem>/<stem>.iscn - the request writes into <dir> under the model's stem;
    // the source is its SR record's (or @p sourceOverride: Reimport From..., the SR record
    // follows it). False with @p code / @p message when it cannot be (no_source_record,
    // source_missing, bad_layout, load_failed).
    bool ResolveModelReimport(const std::string& iscnPath, const std::string& sourceOverride,
                              ModelImportRequest& req, std::string& code, std::string& message);

    // The Content Browser's Reimport: prepared, dry-run, and - when the report
    // says someone should look (MeshIdentityAdoption::NeedsReview) - held for
    // the review popup (m_reimportReview), else committed at once. @p jobId /
    // @p requestId: an Editor.Reimport {review = true} job, finished when it is answered.
    // @p options: renames / geometryRenames (Editor.Reimport's); source, dir
    // and stem come from the model.
    void BeginModelReimport(const std::string& iscnPath, const std::string& sourceOverride,
                            std::uint64_t jobId = 0, std::uint32_t requestId = 0,
                            const ModelImportRequest* options = nullptr);
    // The popup's buttons (Editor.ReimportReview answers it the same way):
    // accept commits with the renames still ticked; either way it closes.
    void AnswerModelReimportReview(bool accept);
    void RenderModelReimportReview();

    // The "Import Model" options dialog: File > Import > Model's files (@p sources into
    // @p outDir), or the Content Browser's Reimport With Options... (@p reimport: the model).
    // Accepted, each source is imported with the options (over a model the folder has: the
    // reimport flow and its review); a reimport goes through BeginModelReimport. @p jobId: an
    // Editor.Import / Editor.Reimport {dialog = true} job, finished by the import it leads to
    // (or failed "cancelled").
    void OpenModelImportDialog(std::vector<std::string> sources, const std::string& outDir, const std::string& reimport,
                               const Resource::ModelImportOptions& options, std::uint64_t jobId = 0,
                               std::uint32_t requestId = 0);
    void AnswerModelImportDialog(bool accept);
    void RenderModelImportDialog();

    // False - the job failed "busy" - while another import writes the same model.
    bool StartModelImport(const ModelImportRequest& req, std::uint64_t jobId, std::uint32_t requestId,
                          const std::string& reimport = std::string());
    void ProcessModelImportTasks();
    void ContinueModelReimport(ModelImportTask& task);   // the review flow, once the prepare is done
    void RenderModelImportProgress();
    // A review open, or a re-import on its way to one: one at a time.
    bool ReviewPending() const;

    void ProcessDroppedFiles();
    // Where dropped files go: model sources - onto @p targetModel (its Reimport
    // From, the review when needed), else the Import Model dialog into the
    // Content Browser's folder; image sources - the texture import review;
    // anything else is reported and skipped. Returns what it did, for the log
    // and Editor.DropFiles.
    std::string HandleDroppedFiles(const std::vector<std::string>& paths, const std::string& targetModel);

    void ProcessScriptedImports();
    // A model import's job output (Editor.Import / Editor.Reimport, and the
    // review's answer): the files, the stats, the SR record and the diff;
    // @p failLog: the warnings / errors a failed job carries. jobId 0: nothing.
    void FinishModelImportJob(std::uint64_t jobId, const ModelImportOutcome& m, const std::string& source,
                              const std::string& dir, std::vector<std::string> failLog);

    // ---- Play mode and the hidden UI ----
    // Editor.Play / Pause / Step / Stop: the request they queued on the PlayModeController
    // (the toolbar's own) and its job, finished by ProcessScriptedPlay in the frame
    // BeginFrame applied it, or failed "superseded" when another request replaced it first.
    // jobId 0 = none.
    void ProcessScriptedPlay();
    // The pre-Play autosave the toolbar's Play writes (dirty scenes only).
    void WritePrePlayAutosave();
    // -hidden_ui: lay ImGui out at the back buffer's size while minimized.
    void SyncHiddenUiSize();

    // ---- Rendered material tiles ----
    // Everything here runs in ProcessPendingActions (the game half's serial window) except
    // the two marks the ImGui pass makes on AssetEntry::previewDirty / lastVisibleTime.
    // One entry point, called unconditionally from ProcessPendingActions:
    //   1. apply the results the render half published (clears previewDirty),
    //   2. drain the readback ring into the disk cache,
    //   3. destroy tiles the eviction sweep queued,
    //   4. evict tiles that have been off-screen too long,
    //   5. enqueue new work, cache hits first.
    void ServiceAssetThumbnails();
    // The stable key for one asset. Prefers the guid (rename / move safe, the disk cache's
    // key); falls back to a hash of the canonical path, which is memory-only. Never stamps a
    // sidecar (see AssetEntry::previewGuid).
    void ResolveAssetPreviewIdentity(AssetEntry& ae);
    // The entry forms of StreamAssetThumbnail / GetAssetThumbnailSrv (those take an index
    // into m_assets), so a Mesh item's tile (m_meshTiles) streams and draws through the same
    // code.
    void     StreamEntryThumbnail(AssetEntry& ae, bool visible, float now);
    uint64_t GetEntryThumbnailSrv(const AssetEntry& ae) const;
    // A cube texture's tile is its cross, drawn by a Renderer texture view (ImGui's shader
    // cannot sample a cube). One view per visible cube tile, acquired on first draw (the
    // service only latches, which is legal from a UI handler), released in
    // ServiceAssetThumbnails once the tile has not been drawn for two frames. mutable: drawn
    // from const paths.
    uint64_t CubeThumbnailSrv(const AssetEntry& ae, const RHI::Texture& tex) const;
    void     SweepCubeThumbnails(bool all);

    // The tile of a Mesh item row (created when @p create and missing); nullptr for an asset
    // row, a stale one, or a missing tile not created.
    AssetEntry* MeshTileFor(const AssetDisplayRow& row, bool create = true);
    // Queue @p ae's tile for destruction at the next serial window and reset its preview
    // state. Safe from an ImGui handler because it only moves the handle onto a list: the
    // descriptor an earlier ImGui::Image captured must outlive this frame's draw data.
    void EvictAssetPreviewTile(AssetEntry& ae, bool countIt);
    // A material chain member was saved. Evicts the disk entry and re-arms the in-memory tile
    // for @p matPath and for every material Resource::MaterialsDependingOn names.
    void InvalidateMaterialThumbnails(const std::string& matPath);

    // One summary line for one churn pass, split out of ReportThumbnailSummary so a pass
    // boundary and the shutdown emit the same shape.
    void EmitThumbnailPassSummary(IGraphicsDevice& gfx, uint32_t passIndex) const;
    // Close the current pass (emit its line) and re-baseline the counters.
    void BeginThumbnailPass(IGraphicsDevice& gfx);

    // ---- Animation-retarget tool state ----
    // The Animation Retarget window's reference skeleton (m_rtRefSkelPath), loaded once,
    // stood upright and cached; nullptr when no path is set or the file does not load.
    SkeletonAsset* GetOrLoadRetargetRefSkel();

    // ---- Autosave / crash recovery ----
    // Periodic + pre-Play snapshots into WritableDataDir()/autosave/, never over the authoring
    // .iscene, rotating kAutosaveKeep copies per scene stem. m_sceneDirty stays set: an
    // autosave is a fallback, not a save.
    // Recovery: RequestLoadScene offers a newer autosave via the modal below; "Restore" loads
    // the autosave file, then the deferred-load block re-points m_currentScenePath at the
    // original scene and re-marks dirty, so Ctrl+S writes the recovery back where it belongs.
    void        MaybeAutosave();
    void        WriteAutosave(const char* reason);
    std::string NewerAutosaveFor(const std::string& scenePath) const;
    void        RenderAutosaveRecoveryPopup();

    // ---- Editor preferences (persist across sessions) ----
    // Snap values, view toggles, log filters, recent scenes and camera bookmarks in a flat
    // key=value file at WritableDataDir()/editor_prefs.ini. Loaded in OnAttach, written in the
    // destructor (best effort: a failed write must never block shutdown).
    void LoadEditorPrefs();
    void SaveEditorPrefs();
    void AddRecentScene(const std::string& path);   // front, deduped, capped
    void StoreCameraBookmark(int idx);              // Ctrl+Shift+1..4
    void RestoreCameraBookmark(int idx);            // Ctrl+1..4
    void SnapSelectedToFloor();                     // End - rest AABB on floor
    // The view cube's drag: turns the free camera by @p dx / @p dy pixels, as a right-drag look
    // does. From an ortho view the free camera first takes that view's @p forward and the viewport
    // returns to perspective.
    void TurnEditorCamera(float dx, float dy, const DirectX::XMFLOAT3& forward);

    // ---- Material asset editing (no entity involved) ----
    // Last material-instance repair scan. Held so the dry-run report stays on
    // screen between frames and the Apply button acts on what was shown.
    Editor::MaterialInstanceRepairReport m_matRepair;
    bool                                 m_matRepairRan = false;

    std::string                m_assetMatPath;          // "" = nothing open
    // unique_ptr because MaterialComponent is only forward-declared here and this
    // header is included widely; ~EditorLayer is out-of-line, which is what makes
    // an incomplete-type unique_ptr member legal.
    std::unique_ptr<struct MaterialComponent> m_assetMatEdit;
    Resource::MaterialLoadInfo m_assetMatInfo;          // parent + authored key set
    bool                       m_assetMatDirty = false; // unsaved edits in the buffer

    // ---- Viewport picking and gizmos ----
    // The Hierarchy expands to and scrolls to this entity on its next draw (a viewport pick, a
    // rename request, a move into a folder, the control channel's select / focus / undo / redo).
    Entity m_hierarchyRevealEntity = NullEntity;

    int m_ikPreviewSlot=-1;
    Entity m_ikPreviewEntity=NullEntity;
    DirectX::XMFLOAT4X4 m_ikGizmoMatrix{};

    bool               m_gizmoForeground     = false;   // -editor_gizmo_foreground
    bool               m_gizmoForegroundRead = false;
    bool               m_gizmoLayerReportOn  = false;   // harness + -unattended only

    GizmoLayerReport   m_gizmoLayer[2];                 // [0] transform, [1] restpose

    // ---- UI Rect tool (uGUI-style direct manipulation) ----
    // Live drag state for the rect tool. Kept across frames because a drag
    // spans them, and because the undo entry is one BRACKET (press..release),
    // not one per mouse-move.
    UIRectDrag m_uiRectDrag;

    // ---- Undo / redo ----
    int                                                  m_batchDepth = 0;
    std::string                                          m_batchLabel;
    std::vector<std::shared_ptr<Editor::Undo::EditSink>> m_batchSinks;

    // ---- Viewport picking and gizmos ----
    // A second viewport click on a Static Mesh's section names that slot; the Inspector
    // scrolls to it and flashes it.
    Entity        m_slotFlashOwner = NullEntity;
    std::uint32_t m_slotFlashIndex = 0;
    double        m_slotFlashAt    = -100.0;
    bool          m_slotFlashScroll = false;

    // ---- ImGui backend ownership ----
    ImGuiManager m_imguiMgr;
    std::function<void()> m_preparedImGuiRecord;
    uint64_t m_preparedImGuiFrame = 0;
    bool m_hasPreparedImGuiFrame = false;
    bool m_preparedImGuiHadSnapshot = false;  // the prepare froze draw data (not an empty frame)
    bool m_deferSnapshotCompletion = false;   // SetDeferImGuiSnapshotCompletion
    bool         m_imguiBackendsReady = false;

    // ---- System refs (injected, not owned) ----
    IGraphicsDevice*               m_gfx          = nullptr;
    Renderer*                      m_renderer     = nullptr;
    Plat::Window*                  m_window       = nullptr;   // for fullscreen apply in Graphics Settings
    // ImGui's Win32 backend as the window's message filter, and ImGui's
    // WantCapture as two of its capture predicates (Platform/Window.h). Reset
    // with the backends; destroyed with the layer, before the window.
    Plat::Subscription             m_imguiMessageFilter;
    Plat::Subscription             m_imguiCapturesMouse;
    Plat::Subscription             m_imguiCapturesKeyboard;
    Resource::TextureSystem*       m_textureSys   = nullptr;
    Resource::ResourceManager*     m_resourceMgr  = nullptr;
    Resource::AssetManager*        m_assetMgr     = nullptr;
    Resource::AnimationClipSystem* m_animClipSys  = nullptr;
    AnimationSystem*               m_animSys      = nullptr;
    GPUProfiler*                   m_gpuProfiler  = nullptr;
    DX12Physics::PhysicsSystem*    m_physicsSys   = nullptr;
    DebugDrawSystem*               m_debugDraw    = nullptr;
    float m_debugIconMaxDistance = 50.f;
    float m_debugIconSizeScale = 1.f;
    AI::AISystem*                  m_aiSys        = nullptr;
    ScriptSystem*                  m_scriptSys    = nullptr;
    SystemRegistry*                m_systemRegistry = nullptr;
    Gameplay::GameFlowStateMachine* m_gameFlow      = nullptr;

    // ---- ECS state ----
    World*  m_world          = nullptr;
    // Inspector structure edits execute before extraction, never while ImGui
    // is recording the rendered frame. Requests carry a world epoch + handle.
    std::vector<std::function<void()>> m_pendingComponentAdds;
    Entity  m_selectedEntity = NullEntity;
    // Full selection; back() == m_selectedEntity. See GetSelectedEntities().
    std::vector<Entity> m_selectedEntities;
    // Anchor for Shift+click range selection in the Hierarchy: the row a range extends from,
    // which is not the primary once a range has been made (Shift-clicking again re-extends
    // from the anchor, not from the far end of the previous range).
    Entity  m_selectionRangeAnchor = NullEntity;
    // Flattened Hierarchy row order, rebuilt every frame as the tree draws. Shift+click needs
    // "what is visually between these two rows", which only the traversal knows (parent /
    // child order alone cannot answer it).
    std::vector<Entity> m_hierarchyRowOrder;

    // Compatibility mirrors of the primary selection. Row rendering uses the
    // actual selection set; a drag never substitutes a visual-only selection.
    Entity  m_hierarchyHighlight     = NullEntity;
    Entity  m_lastSeenSelectedEntity = NullEntity;
    // Ctrl+C clipboard: the copied entity (Ctrl+V duplicates it). Generation-
    // checked so a since-deleted source pastes nothing rather than mis-target.
    EntityHandle m_clipboardEntity   = NullEntityHandle;
    // Serialized twin of the handle above: WritableDataDir()/clipboard/clipboard.ipfb,
    // written on Copy. Lets Paste survive source deletion, scene reloads and a second editor
    // instance. Empty = snapshot failed.
    std::string  m_clipboardFile;
    // World entity-destroy listener handle (0 = unbound). Clears the selection/
    // highlight above when their entity is destroyed so a recycled ID can't be
    // silently re-selected. Bound in SetWorld, removed in SetWorld/~EditorLayer.
    uint32_t m_entityDestroyListener = 0;

    // Chain-physics (KawaiiPhysics-style) authoring: draw the selected entity's
    // authored chains in the viewport. Toggled from the Chain Physics inspector;
    // consumed each frame by EmitChainPhysicsOverlay().
    bool    m_chainOverlayEnabled    = true;
    // Index of the chain group whose editor node is expanded; that group draws in a vivid
    // highlight colour (others dimmed) so the designer can tell which chain they are editing.
    // -1 = none. Reset at the top of OnUIRender and re-asserted each frame by a parameter tree
    // drawn for the overlay's own target (ChainParamHooks::mainOverlayTarget), so a collapsed
    // header / hidden tab / F11 cannot leave a stale highlight.
    int     m_chainHighlightGroup    = -1;

    // ---- Bone-pose authoring (authored rest pose) ----
    // The main viewport's pose-tool state: the picker, the rest-pose gizmo and the overlay
    // markers of the selected character. Armed from the Inspector's Chain Physics row ("Pose
    // rest bones in main viewport"). focusGroup is separate from m_chainHighlightGroup on
    // purpose: that one is reset at the top of OnUIRender, so it is always -1 by the time
    // RenderViewportPanel runs. poseBone is only meaningful for `owner` / `skelAsset` (a bone
    // index names one skeleton); UpdateBonePoseEditTracking drops it on change.
    ChainPoseState m_chainMainPose;
    // A tree drawn for an entity with no state of its own (ChainParamHooks::pose null and not
    // the overlay target), keyed by its owner.
    ChainPoseState m_chainScratchPose;
    // Set when a viewport click hit a bone, so HandleViewportPicking can swallow it;
    // otherwise the GPU entity pick resolves a frame later and changes m_selectedEntity under
    // the pose session.
    bool     m_bonePickConsumed    = false;
    // Frozen matrix fed to ImGuizmo (see the entity gizmo's m_gizmoWorldMatrix):
    // a bone matrix is re-derived from an animating pose every frame, so feeding
    // the live value makes hit-testing flicker mid-drag.
    DirectX::XMFLOAT4X4 m_bonePoseGizmoMatrix{};
    uint32_t            m_bonePoseGizmoBone = ~0u;
    // How often RenderBonePoseGizmo submitted the ring / wrote an entry, and for whom
    // (-chainpreview_main_pose).
    std::uint64_t       m_bonePoseGizmoSubmits = 0;
    std::uint64_t       m_bonePoseGizmoEdits   = 0;
    Entity              m_bonePoseGizmoLastEntity = NullEntity;
    // Undo bracketing (UpdateBonePoseEditTracking): the authored rest pose as of the last
    // idle frame, and its copy taken when a drag starts (the pre-drag value).
    BonePoseBaseline m_bonePoseBaseline;
    BonePoseBaseline m_bonePoseBefore;
    EntityHandle     m_bonePoseRecordEntity = NullEntityHandle;
    bool             m_bonePoseRecording    = false;

    // ---- Dedicated editor window (Animation Set) ----
    // The target is latched via EntityHandle when the inspector's Edit button is clicked
    // (generation-checked: survives ID recycling and Play-mode snapshot / restore). It does
    // not follow m_selectedEntity: the user must be able to select colliders / other entities
    // while tuning; the window's "Use Selected" button re-latches. Cleared in SetWorld and by
    // the entity-destroy listener.
    EntityHandle m_animSetEditTarget = NullEntityHandle;
    int          m_animSetSelClip    = -1;  // selected row in the clip list (clamped per frame)
    char         m_chainBoneFilter[64] = {}; // root-bone picker filter

    // ---- Component editor registration ----
    std::unordered_map<std::type_index, ComponentEditorEntry> m_componentEditors;
    std::unordered_map<std::type_index, std::string>          m_componentLabels;
    // The module whose RegisterComponentEditors is running ("" = the core's own).
    std::string                                               m_registeringModule;

    // ---- Undo / redo state ----
    Editor::Undo::EditHistory m_editHistory;
    // The sink in scope (SinkScope); null = the scene's history (EditTarget()).
    std::shared_ptr<Editor::Undo::EditSink> m_editSink;
    // Transform-edit bracketing (UpdateTransformEditTracking): m_xformBaseline holds the
    // selected entity's LocalTransform as of the last idle frame (= the pre-drag value); when
    // a drag ends it pushes TransformEditAction(m_xformBefore -> current LocalTransform).
    bool           m_xformRecording      = false;  // a gizmo drag is in progress
    bool           m_xformBaselineValid  = false;
    Entity         m_xformBaselineEntity = NullEntity;
    Entity         m_xformRecordEntity   = NullEntity;
    LocalTransform m_xformBaseline{};               // clean value while idle
    LocalTransform m_xformBefore{};                 // captured pre-drag value
    // Multi-select drag: one baseline per moved entity. The whole drag still commits as one
    // undo entry (an EditMacro): a gesture that takes several Ctrl+Z presses to undo is a bug.
    std::vector<std::pair<Entity, LocalTransform>> m_xformBaselines;
    std::vector<std::pair<Entity, LocalTransform>> m_xformBefores;

    // Inspector field-edit bracket: at most one open at a time (single selection, single
    // active ImGui item). The *Before fields are captured on the idle frame before the edit.
    FieldEditBracket m_fieldEdit;

    // ---- Inspector toolbar (EditorLayer_Properties.cpp) ----
    // The field search and Modified only: the Inspector draws the fields it shows, flat.
    EditorUI::RowFilter m_inspectorFilter;
    char                m_inspectorSearch[64] = {};
    // Locked: the Inspector keeps the locked entity while the selection moves on.
    bool                m_inspectorLocked = false;
    EntityHandle        m_inspectorLockHandle;

    // ---- Panel registry state (EditorLayer_Panels.cpp) ----
    bool                               m_panelsRegistered = false;
    // The panel.<id> / workspace.<name>.* preferences as read (written back with the registry's).
    std::map<std::string, std::string> m_panelPrefs;
    // What the last frame boundary saw, for the registry's change events.
    const World*                       m_panelSeenWorld = nullptr;
    std::uint64_t                      m_panelSeenEpoch = 0;
    int                                m_panelSeenPlayState = 0;
    std::vector<Entity>                m_panelSeenSelection;
    std::uint64_t                      m_selectionEpoch = 0;
    // A workspace's dock layout, loaded before the next ImGui frame (BeginImGuiFrame).
    std::string                        m_pendingWorkspaceLayout;
    char                               m_workspaceName[48] = {};
    // The Services adapters (EditorLayer_Services.cpp).
    std::shared_ptr<void>              m_serviceAdapters;
    // ---- Jobs panel (EditorLayer_Jobs.cpp) ----
    bool                                             m_showJobs = false;
    bool                                             m_showByteView = false;   // EditorLayer_ByteView.cpp
    std::uint32_t                                    m_jobsSeenActive = 0;
    std::uint64_t                                    m_jobsSeenLastId = 0;
    std::unordered_map<std::uint64_t, std::string>   m_jobsSeenState;

    // ---- Render view (for picking ray construction) ----
    RenderView m_renderView{};

    // ---- Viewport state ----
    bool               m_layoutInitialized   = false;
    // Window > Reset Layout: rebuild the default DockBuilder tree next frame
    // even though imgui.ini restored one, and reopen the core panels.
    bool               m_resetLayoutRequested = false;
    // Core panels (Window > General). The Viewport has no flag: the engine's
    // render size follows it and it is never closed. Persisted in
    // editor_prefs.ini only when CLOSED (the file is tracked); forced open
    // under the harness layout, where packs assume every core panel draws.
    bool               m_showHierarchy       = true;
    bool               m_showInspector       = true;
    bool               m_showContentBrowser  = true;
    bool               m_showOutputLog       = true;
    // -unattended runs neither load nor save imgui.ini; the layout is the compiled-in default.
    // OnAttach says why: a docked panel is invisible under -hidden, and a corpus run must not
    // write the minimised layout over a developer's file.
    bool               m_harnessImGuiLayout  = false;

    // ---- Inspector panel visibility (-panelvisibleverify oracle) ----
    // RenderInspectorPanel honours ImGui::Begin()'s result: no rows for a collapsed, clipped
    // or unselected-tab window, except under -hidden (no UI surface), where it draws anyway.
    // These count what it does. Plain counters, not WITH_HARNESS members: EditorCore is
    // compiled once and App.h already pays for layout-sensitive conditional members; three
    // uint64s are not worth a second ABI. Only the [PANELVIS] emit is gated.
    std::uint64_t      m_inspectorDrewFrames    = 0; // Begin() said draw, body ran
    std::uint64_t      m_inspectorRefusedFrames = 0; // Begin() said no, body skipped
    std::uint64_t      m_inspectorNoSurface     = 0; // Begin() said no, but -hidden

    // ---- Viewport state ----
    bool               m_viewportFullscreen  = false;
    bool               m_hasFontAwesomeIcons = false;
    // Editor text size in px (editor_prefs.ini "uiFontSize"). Pinned to
    // EditorTheme::kDefaultFontSize under the harness layout (see OnAttach).
    float              m_uiFontSize          = 16.0f;
    bool               m_gizmoIconsBaked     = false;  // FA gizmo glyphs baked -> engine textures
    // Editor outliner folders (Unreal-style). Set of known folder full-paths
    // ("Enemies/Melee") so empty user-created folders still render; content
    // folders also derive from each entity's EditorFolderComponent.path.
    // Cleared on scene load (see File>Load path) to avoid cross-scene ghosts.
    std::set<std::string> m_editorFolders;
    unsigned long long m_sceneTextureId      = 0;
    ImVec2             m_viewportMin{};                       // screen-space top-left of viewport content
    unsigned int       m_viewportSizeX       = 0;
    unsigned int       m_viewportSizeY       = 0;
    ViewportPlayState  m_playState           = ViewportPlayState::Stopped; // fallback when m_playModeController == null
    PlayModeController* m_playModeController  = nullptr;                    // authoritative Edit/Play state machine (EngineCore)
    bool               m_wantsStepFrame      = false;
    bool               m_viewportRightDragging = false;
    bool               m_viewportHovered       = false;
    // Published for IsOtherImGuiItemActive(); written by BOTH viewport layouts.
    bool               m_viewportOtherItemActive = false;
    // Published for IsUIRectToolClaimingMouse(): the rect tool is dragging, or is
    // hovering a handle and would take the next press.
    bool               m_uiRectToolClaimsMouse   = false;
    // Was an ImGuizmo gizmo actually submitted this frame? ImGuizmo caches its hit-test
    // geometry inside Manipulate and BeginFrame does not reset it, so skipping Manipulate (no
    // GlobalTransform, or a Canvas node) leaves IsOver() answering from the previous entity's
    // gizmo: an invisible dead zone that swallows viewport clicks. Every IsOver() / IsUsing()
    // consumer must check this first.
    bool               m_gizmoSubmittedThisFrame = false;
    float              m_vpMouseDX           = 0.f;
    float              m_vpMouseDY           = 0.f;
    // The Scene viewport's fly input for the editor camera in Play (GetSceneFlyInput).
    Editor::FlyInput   m_sceneFly{};
    // The window keyboard's Esc / F1 last frame (UpdateGameInputGate's edges).
    bool               m_gateEscDown         = false;
    bool               m_gateF1Down          = false;

    // ---- Picking / drag ----
    std::function<void(int, const DirectX::XMFLOAT3&)> m_createMeshCallback;
    std::function<void(float, float)> m_pickCallback;
    // Viewport picking is a GPU READBACK: the result lands a frame or more
    // after the click, by which time the modifier keys may have changed. The
    // mode is therefore latched when the click is made, not when it resolves.
    SelectMode m_pickSelectMode = SelectMode::Replace;
    bool                              m_hasPendingDrag = false;  // click -> pick result interim
    std::string                       m_pendingMatDropPath;      // .imat dropped on viewport
    bool                              m_isDragging      = false;
    Entity                            m_dragEntity      = NullEntity;
    DirectX::XMFLOAT3                 m_dragPlaneNormal{};        // camera-facing plane normal
    DirectX::XMFLOAT3                 m_dragPlanePoint {};        // a point on the drag plane
    DirectX::XMFLOAT3                 m_dragEntityOffset{};       // entity origin - hit point

    // ---- Gizmo state ----
    ImGuizmo::OPERATION   m_gizmoOperation    = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE        m_gizmoMode         = ImGuizmo::LOCAL;
    // Editor reference grid (EditorGridPass): visibility + metres per cell.
    bool                  m_showEditorGrid    = true;
    // The viewport's view cube (Editor/EditorViewCube.h); editor_prefs.ini "viewport.viewCube".
    bool                  m_showViewCube      = true;
    // A UI test shows the cube under the harness layout too; the square it was drawn in last
    // frame (x, y, size; size 0 = not drawn).
    bool                  m_viewCubeForTests  = false;
    float                 m_viewCubeRect[3]   = {};
    // Harness-only override for the line above. A separate flag rather than writing
    // m_showEditorGrid, because that member is persisted to editor_prefs.ini on shutdown and
    // a test run must not change what the user sees next time.
    // Outside the WITH_HARNESS gate although only the harness half writes it: OnUIRender reads
    // it in every configuration, and gating one bool would put an #ifdef in the grid wiring.
    bool                  m_harnessHideGrid   = false;
    float                 m_gridCellSize      = 1.0f;
    // Drag snapping: off = free drag. Per-operation increments; holding Ctrl
    // inverts the toggle while dragging (temporary snap / temporary free).
    bool                  m_snapEnabled       = false;
    float                 m_snapTranslate     = 1.0f;   // metres
    float                 m_snapRotate        = 15.0f;  // degrees
    float                 m_snapScale         = 0.1f;   // scale factor step
    // Edit Collider: the toggle, the drag's entity and its component blob before the drag,
    // and the matrix fed to ImGuizmo (frozen while hovered or held, re-derived from the
    // component otherwise).
    bool                  m_editCollider          = false;
    Entity                m_colliderDragEntity    = NullEntity;
    std::string           m_colliderDragBefore;
    // ---- Morph editor and chain colliders ----
    // A Morph-panel slider drag writing the Facial Animation Authored list: its component blob
    // at the drag's start (the panel's own undo brackets only the MorphComponent it draws).
    std::string           m_morphAuthoredBefore;
    Entity                m_morphAuthoredEntity = NullEntity;
    bool m_morphEditorOpen = false;
    bool m_morphEditorFocus = false;
    EntityHandle m_morphEditorTarget = NullEntityHandle;
    uint32_t m_morphEditorEpoch = 0;
    bool m_chainColliderUndo = false;

    // ---- Gizmo state ----
    bool                  m_colliderGizmoUsing    = false;
    bool                  m_colliderGizmoOver     = false;
    Entity                m_colliderGizmoEntity   = NullEntity;
    DirectX::XMFLOAT4X4   m_colliderGizmoMatrix{};
    std::uint32_t         m_colliderGizmoGestures = 0;

    // ---- Editor viewport camera view ----
    EditorViewMode        m_editorViewMode    = EditorViewMode::Perspective;
    float                 m_orthoHalfHeight   = 10.f;
    DirectX::XMFLOAT3     m_orthoFocus        { 0.f, 0.f, 0.f };
    // Hierarchy: show editor-internal entities (editor camera, stack channel).
    bool                  m_showInternalEntities = false;
    // Hierarchy: show the section entities a StaticMesh derives (hidden by
    // default: they are rebuilt from the owner and never saved).
    bool                  m_showMeshSections = false;
    // Consumed by RenderHierarchyPanel to rebuild its cached hierarchy view
    // (see MarkHierarchyDirty for the invalidation contract).
    bool                  m_hierarchyStructureDirty = true;
    // Cached world matrix fed to ImGuizmo, frozen while hovered / dragged so hierarchy float
    // drift does not flicker the gizmo.
    DirectX::XMFLOAT4X4   m_gizmoWorldMatrix  {};
    Entity                m_gizmoCachedEntity = NullEntity;

    // ---- Asset browser ----
    std::string             m_assetDir;        // root (set once)

    // ---- Where-used (reverse reference index) ----
    Editor::AssetReferenceScanner m_refScanner;
    bool                    m_showReferencesWindow = false;
    std::string             m_refQueryPath;    // asset the window is asking about

    // ---- Asset browser ----
    std::string             m_currentDir;      // current browsing dir
    std::vector<AssetEntry> m_assets;
    bool                    m_needsRescan   = true;
    double                  m_lastScanTime  = -999.0;          // ImGui time of last scan
    int                     m_selectedAsset = -1;   // primary (last clicked)
    // Asset multi-selection, by path rather than by index: the browser rescans every 5 s and
    // indices are not stable across a rescan, so an index-based selection would silently
    // retarget onto whatever slid into that slot. m_selectedAsset stays an index because it
    // is only read within the frame that set it.
    std::vector<std::string> m_selectedAssetPaths;
    int                      m_assetRangeAnchor = -1;   // index, this frame only

    // ---- Folder operations (content browser) ----
    // Deferred so the move never runs inside the traversal that is drawing the
    // rows it is about to invalidate.
    std::vector<std::string> m_pendingMoveSrc;
    std::string              m_pendingMoveDest;
    std::string              m_pendingNewFolderParent;
    std::string              m_pendingDeleteFolder;
    std::string              m_folderRenameTarget;
    char                    m_assetFilter[128] {0};            // case-insensitive substring filter
    // The filter searches every folder (the background catalog) instead of the current one.
    bool                    m_assetSearchAll = false;
    Editor::AssetCatalog    m_assetCatalog;                    // the tree's metadata, walked off the UI thread
    double                  m_assetCatalogAskedAt = -1.0;      // ImGui time of the last Refresh
    std::string             m_pendingAssetSelect;              // a search result to select once its folder is listed

    // ---- Content-browser presentation ----
    // Display order is kept apart from m_assets so the Details table's sort specs never fight
    // ScanAssets' canonical order (folders, type, name). Session-only state, not persisted.
    AssetViewMode           m_assetViewMode   = AssetViewMode::Tiles;
    std::vector<int>        m_assetOrder;                      // filtered+sorted indices into m_assets
    bool                    m_assetOrderDirty = true;
    // m_assetOrder plus the Mesh sub-rows of every expanded model, rebuilt each frame (the
    // views iterate this), and which models are expanded (by their entry path; survives
    // rescans and folder changes).
    std::vector<AssetDisplayRow>    m_assetRows;
    std::unordered_set<std::string> m_expandedModels;
    // Each Mesh item's preview tile, keyed by its MeshAssetRef ("<model>#<keyhex>", which is
    // also the entry's `path`). Created when the item is first drawn, serviced by the same
    // thumbnail lane, cache and invalidation as the folder's entries (ForEachPreviewEntry),
    // and erased once it has been off-screen for the eviction delay; it is not part of the
    // folder listing, so no rescan drops it.
    std::unordered_map<std::string, AssetEntry> m_meshTiles;
    // The Inspector material rows' tiles, keyed by the material's path: the same lane, cache,
    // invalidation and eviction as the Mesh item tiles (created when a row draws, erased once
    // off screen for the eviction delay). MaterialRowThumb: the ImTextureID, 0 = none yet.
    std::unordered_map<std::string, AssetEntry> m_materialRowTiles;

    // Default = Type, which reproduces ScanAssets' grouping (folders, type, name), so the
    // Tiles view keeps ScanAssets' layout.
    int                     m_assetSortColumn = 1;             // 0=Name 1=Type 2=Size 3=Modified
    bool                    m_assetSortAsc    = true;
    float                   m_assetListColW   = 200.f;         // List view column width (measured on rebuild)

    // ---- Folder tree (left pane of the content browser) ----
    // Unreal / Unity shape: whole-tree navigation on the left, the current folder's assets on
    // the right. ScanAssets reads one directory only, so the tree is the only place the shape
    // of asset/ is visible.
    // Rebuilt with ScanAssets (the Refresh / m_needsRescan path), not on the 5 s auto-rescan:
    // it walks the whole tree, too much to pay every 5 s for a layout that rarely changes.
    std::vector<AssetFolderNode> m_folderTree;          // [0] is the asset root
    // The m_currentDir the tree last auto-expanded to reveal. Expansion is forced only on the
    // frame the selection changes from outside the tree (the Up button, double-clicking a
    // folder tile); forcing it every frame would make the current folder's ancestors
    // impossible to collapse.
    std::string             m_folderRevealedFor;
    bool                    m_showFolderTree  = true;

    // ---- Import (textures, models, dropped files) ----
    std::vector<PendingTextureImport> m_pendingTextureImports;
    bool m_openTextureImportPopup = false;
    bool m_textureImportConfirmed = false;

    // A re-import held for the review popup (BeginModelReimport).
    std::unique_ptr<ModelReimportReview> m_reimportReview;   // open while set
    bool m_openReimportReview = false;

    // The "Import Model" options dialog (OpenModelImportDialog).
    std::unique_ptr<ModelImportDialog> m_importDialog;   // open while set
    bool m_openImportDialog = false;

    // A model import off the main thread. Its prepare (the source read, the importer -
    // assimp, the texture cook) runs on a TaskSystem worker; ProcessModelImportTasks, every
    // frame on the main thread, commits the finished ones (a re-import through its review)
    // and finishes their jobs. The editor stays usable meanwhile; a small overlay shows what
    // is running.
    std::vector<std::shared_ptr<ModelImportTask>> m_modelImportTasks;

    // Dropped files, and where the Content Browser's model tiles were last drawn (screen
    // rect, the .iscn): a model file dropped on one is that model's Reimport From.
    std::vector<DroppedFiles> m_droppedFiles;

    std::vector<ModelTileRect> m_modelTileRects;

    // Editor.Import requests, run as jobs from ProcessPendingActions. A model's
    // item.outputPath is the output DIRECTORY (model = true).
    std::vector<ScriptedImport> m_scriptedImports;

    // ---- Play mode and the hidden UI ----
    ScriptedPlay m_scriptedPlay;

    // ---- Unreal scene-manifest import (Import > Unreal Scene...) ----
    // Job state is shared_ptr-owned so the TaskSystem worker keeps it alive
    // even if the editor clears m_ueImportJob while the cook is running.
    std::shared_ptr<UeImportJob> m_ueImportJob;
    bool m_openUeImportPopup = false;

    // ---- Rendered material tiles ----
    // Thumbnail streaming budget.
    int   m_previewLoadBudgetPerFrame = 4;
    int   m_previewLoadsThisFrame     = 0;
    float m_previewEvictDelay         = 8.f;                   // seconds off-screen before Release()

    mutable std::unordered_map<std::string, CubeThumb> m_cubeThumbs;

    // Tiles whose textures are owned outright. Destroyed in the serial window.
    std::vector<RHI::Texture> m_pendingTileDestroy;

    // What the disk encoder needs when the readback pixels arrive, two frames
    // after the enqueue: the source path, its guid and the dependency stamps to
    // write into the sidecar. Keyed by AssetEntry::previewKey and erased when
    // the pixels are consumed - the AssetEntry itself cannot hold it because a
    // rescan can drop the entry while its request is still in flight.
    std::unordered_map<uint64_t, PendingThumbInfo> m_thumbPendingInfo;
    std::vector<Preview::ThumbResult>              m_thumbResultScratch;
    // How many render requests may be built per frame. LoadMaterial is cached
    // after the first resolve, but the FIRST one parses a file, so a folder of
    // 500 materials would otherwise pay 500 parses in one ProcessPendingActions.
    // The RENDER budget is 1/frame regardless (PreviewRenderer); this only
    // bounds the enqueue.
    int      m_thumbEnqueuePerFrame = 4;
    uint32_t m_thumbApplied  = 0;   // results applied (== pictures rendered)
    uint32_t m_thumbEvicted  = 0;   // tiles destroyed by the sweep
    uint32_t m_thumbRescans  = 0;   // ScanAssets calls (snapshot/restore path)
    uint32_t m_thumbOrphansReleased = 0;   // tiles released BY a rescan
    bool     m_thumbGCDone   = false;

    // Counter values at the start of the current pass; every number on the
    // summary line is a DELTA against these. The renderer's own counter is
    // monotonic by contract (preview_edit latches it), so it cannot be reset.
    ThumbPassBase m_thumbPassBase{};
    // -preview_churn (see HarnessSetPreviewChurn). Non-empty = the churn driver is on.
    std::string              m_thumbChurnDir;

    // Every directory under the churn root, INCLUDING ones with no material.
    // A material-free folder is not filler: rotating into it is what makes
    // ScanAssets orphan the previous folder's tiles, which is the one part of
    // the tile lifecycle no amount of scrolling inside a single folder reaches.
    std::vector<ChurnFolder> m_thumbChurnFolders;
    std::size_t              m_thumbChurnIdx    = 0;
    uint64_t                 m_thumbChurnCycle  = 0;   // frames in ONE full rotation
    uint32_t                 m_thumbChurnPasses = 1;   // -preview_churn_passes
    uint32_t                 m_thumbChurnPass   = 0;   // 0-based, current
    // -preview_touch_at=<frame>:<path>
    uint64_t                 m_thumbTouchFrame  = 0;   // 0 = never
    std::string              m_thumbTouchPath;
    bool                     m_thumbTouchFired  = false;
    // -preview_asset_appear_at. Same shape, one-shot, 0 = never.
    uint64_t                 m_assetAppearFrame = 0;
    std::string              m_assetAppearSrc;
    std::string              m_assetAppearDst;
    bool                     m_assetAppearFired = false;
    // -preview_asset_rewrite_at. Same shape again; the difference is on disk: the destination
    // exists and Editor::PreviewHarness keeps its original bytes.
    uint64_t                 m_assetRewriteFrame = 0;
    std::string              m_assetRewriteSrc;
    std::string              m_assetRewriteDst;
    bool                     m_assetRewriteFired = false;

    // ---- Window-toggle flags ----
    bool m_showAnimDebug         = false;
    bool m_showRetargetWindow    = false;
    bool m_showPhaseDebug        = false;
    bool m_showUICanvasWireframe = false;
    // editor_prefs.ini viewport.uiRectTool / viewport.screenUI: the viewport's Canvas Rect
    // Tool is off by default (the UI Editor is the tool), and Show > Screen UI can hide the
    // game UI in Edit mode.
    bool m_uiRectToolInViewport = false;
    bool m_showScreenUI         = true;

    // ---- Animation-retarget tool state ----
    std::string m_retargetSrcPath;
    std::string m_retargetStatus;
    bool m_rtSrcAPose = false;   // source rest pose is A-pose (straighten arms)
    bool m_rtTrgAPose = true;    // target (PMX) rest pose is A-pose - common default
    bool m_rtUpper   = true;
    bool m_rtLower   = true;
    bool m_rtFingers = true;
    bool m_rtRoot    = true;
    bool m_rtCanonLegs = true;   // canonical straight-down leg rest on both rigs
    bool m_rtTwist     = true;   // spread limb roll onto the rig's twist bones (PMX nejiri)
    bool m_rtCanonFeet = true;   // canonical foot rest frame on both rigs
    bool m_rtCanonNeck = true;   // canonical neck/head rest frame on both rigs
    bool m_rtSaveIanim = true;   // persist baked clips as a .ianim after retarget

    // Batch retarget (folder in -> mirrored folder out)
    std::string m_rtBatchSrcDir;
    std::string m_rtBatchOutDir;
    std::string m_rtBatchStatus;
    bool        m_rtBatchOverwrite = false;  // replace existing .ianim outputs

    // Source .iskel cache, shared by the mapping UI and DoRetarget so manual slot edits
    // survive until the path changes. (unique_ptr: SkeletonAsset is ~260 KB; never on the
    // stack.)
    std::unique_ptr<SkeletonAsset> m_rtSrcSkel;
    std::vector<ClipAsset>         m_rtSrcClips;
    std::string                    m_rtSrcLoadedPath;
    int  m_rtMapEditTarget = 0;   // 0 = target (selected entity), 1 = source .iskel
    char m_rtBoneFilter[64] = {}; // bone-picker popup filter

    // OPTIONAL source-skeleton override (reference pose). UE animation-only FBX
    // does NOT carry the mannequin's reference pose in its node transforms, so
    // the anim file alone cannot drive the delta retarget. Import the skeletal-
    // mesh FBX (e.g. SKM_Manny) once -> its .iskel holds the true reference;
    // set it here and the animation file only contributes clips (joined by bone
    // name). Mixamo files carry their own T-pose, so this stays empty for them.
    std::string                    m_rtRefSkelPath;
    std::unique_ptr<SkeletonAsset> m_rtRefSkel;
    std::string                    m_rtRefLoadedPath;
    // Up-axis fix applied to the reference skeleton (UE = Z-up): animation
    // channels joined to it later must get the same global rotation.
    DirectX::XMFLOAT4X4            m_rtRefUpFix{};
    bool                           m_rtRefUpFixed = false;

    // ---- Window-toggle flags ----
    bool m_showFontEditor        = false;
    // Master switch for the reflection-probe debug balls. ANDed every frame in
    // OnUIRender with each probe's ReflectionProbeComponent::showVizSphere and
    // written to that entity's VisibilityComponent; per-probe edits are adopted
    // back into showVizSphere so they survive. Probe baking ignores visibility.
    bool m_showProbeVizSpheres   = true;
    bool m_showTimeline          = false;
    bool m_showProfiler          = false;
    bool m_showPoolWatermarks    = false;      // Window > Analysis > Pool Watermarks
    bool m_showLoadedGeometry    = false;      // Window > Analysis > Loaded Geometry
    char m_loadedGeometryFilter[128] = {};
    bool m_showLoadedTextures    = false;      // Window > Analysis > Loaded Textures
    char m_loadedTexturesFilter[128] = {};

    // Refreshed in ProcessPendingActions (the users need the render lane parked)
    // while the window is open: every 60 frames, 10 after the texture set changed.
    std::vector<LoadedTextureRow> m_loadedTextures;
    std::uint64_t m_loadedTexturesRevision = 0;   // 0 = not read yet (the window was closed)
    std::uint64_t m_loadedTexturesFrame    = 0;
    float         m_loadedTexturesMs       = 0.f;  // what the last read cost
    int  m_loadedTexturesSort = 4;             // column; 4 = VRAM
    bool m_loadedTexturesAsc  = false;
    bool m_loadedTexturesBig = false, m_loadedTexturesRaw = false, m_loadedTexturesOneMip = false,
         m_loadedTexturesFailed = false;
    bool m_showPostProcess       = false;
    bool m_showGraphicsSettings  = false;      // Edit > Project Settings...
    bool m_restirCostWant        = false;      // SyncRestirCostWant
    bool m_showEditorPreferences = false;      // Edit > Editor Preferences...
    // Bottom status bar (Editor Preferences > Panels; editor_prefs.ini
    // "statusBar=0" when off). OFF under the harness layout unless
    // -editor_statusbar: it shrinks the dock space by a row (the Viewport rect
    // packs measure) and its live fps text would differ between any two
    // -ui_screenshot captures of one run.
    bool m_showStatusBar         = true;
    bool m_showKeyboardShortcuts = false;      // Help > Keyboard Shortcuts (F1)
    // ---- Commands and documents ----
    Editor::DocumentManager m_documents;
    bool                    m_commandsRegistered = false;
    // Last frame's one-shot focus claims of the panels that read their own keys.
    bool                    m_focusSequencer = false, m_focusNotify = false;
    bool                    m_focusAnimGraph = false, m_focusAnimSet = false;
    // Help > Keyboard Shortcuts: the command taking the next chord ("" = none).
    std::string             m_rebindCommandId;
    // editor_prefs.ini keymap.* lines, kept whole so a command this build lacks keeps its chord.
    std::map<std::string, std::string> m_keymapPrefs;
    bool m_showAbout             = false;      // Help > About
    bool m_showEnvironmentWindow = false;      // Window > World > Environment
    bool m_showWorldSettings     = false;      // Window > World > World Settings
    bool m_showDecalMaterials    = false;
    bool m_showCameraSwitcher    = false;      // Window > World > Camera Switcher
    // Window > World > Camera Preview. The panel itself is Editor::CameraPreviewWindow (its
    // own TU); this is only the menu flag.
    bool m_showCameraPreview     = false;
    // Window > Content > Asset Preview. The panel itself is Editor::AssetPreviewWindow (its
    // own TU); this is only the menu flag.
    bool m_showAssetPreview      = false;
    // Window > World > Split Viewports. The panel itself is Editor::SplitViewportPanel (its
    // own TU); this is only the menu flag.
    bool m_showSplitViewport     = false;
    // Console (the Output Log's command line) + Lua Script Tools. There is no separate Lua
    // Console window: console output goes to the log.
    bool m_luaToolsScanned       = false;      // asset/scripts/editor/*.lua listing is cached
    std::vector<std::string> m_luaTools;
    std::string              m_pendingLuaChunk;    // queued by Draw, run at the frame boundary
    std::string              m_pendingLuaLabel;    // "" = a typed command; else the tool's path
    std::uint32_t            m_pendingLuaRequest = 0;   // Logger request id of the queued chunk
    bool                     m_pendingLuaExpr    = false;   // try it as an expression first
    // -console_exec=<editor frame>:<lua> (repeatable) and its reverse arm,
    // -console_context_off (run without the ScopedContext: nothing attributed).
    std::vector<std::pair<std::uint64_t, std::string>> m_harnessConsoleExecs;
    bool                     m_consoleContextOff = false;
    bool m_showScriptRegistry    = false;      // Window > Scripting & Gameplay > Script Registry
    bool m_showGameFlow          = false;      // Window > Scripting & Gameplay > Game Flow
    bool m_showAnimSetEditor     = false;      // Inspector "Edit Animation Set..." button
    bool m_showVFXEditor         = false;      // Window > Content > VFX Editor (.ivfx authoring)
    bool m_showSequencer         = false;      // Window > Animation > Sequencer (.iseq cinematic authoring)
    Editor::SequencerWindow m_sequencerWindow; // owns its document + its own undo ring
    bool m_showComponentTransfer = false;      // Tools > Component Transfer
    // Targets, merged row list and options for the Component Transfer window. Holds a parsed
    // .ipfb per prefab-side target, so it is a few KB rather than a couple of bools; kept by
    // value anyway, like m_sequencerWindow.
    Editor::ComponentTransfer::State m_compTransfer;
    bool m_tracerTestModeEnabled = false;      // Debug menu; T fires a tracer along view ray

    // ---- Animation timeline editor ----
    // Two-tab panel (RenderTimelinePanel):
    //   * "Clip Asset" - edit AnimNotify tracks that live on a .ianim clip (Unreal
    //                    AnimSequence-style); Save round-trips to disk.
    //   * "Entity"     - per-entity TimelineComponent editing.
    // The shared NotifyTrackEditor drives both via a TimelineComponent target.
    Editor::NotifyTrackEditor m_notifyTrackEditor;
    Timeline::TimelineEditor  m_curveEditor;        // read-only bone-curve overlay

    // ---- Clip-asset editing state ----
    Resource::AnimHandle m_animEditHandle{};        // loaded .ianim handle (0 = none)
    std::string          m_animEditPath;            // source path for Save
    bool                 m_animEditReady   = false; // resource finished loading
    int                  m_animEditClipIdx = 0;     // which clip in the resource we edit
    int                  m_animEditClipMirrored = -1;// clip idx the scratch currently mirrors
    TimelineComponent    m_clipScratch;             // NotifyTrackEditor edit target for the clip
    bool                 m_animPreviewOnEntity = false; // mirror edits into the selected entity's bound clip
    bool                 m_animShowCurves      = false;  // show the read-only bone-curve overlay
    char                 m_animPathInput[260]  = {};     // manual path entry buffer
    std::string          m_animSaveStatus;              // transient "Saved/Failed" message

    // ---- Profiler (EditorLayer_Profiler.cpp) ----
    // Everything the window keeps (the live timeline copy, the pass EMAs, the capture form)
    // in one struct defined in that file; null until the window first opens. A
    // function-pointer deleter, so this header never needs the complete type.
    std::unique_ptr<ProfilerPanelState, void (*)(ProfilerPanelState*)> m_prof{ nullptr, nullptr };
    // Written by App (SetCPUFrameTime, the harness frame probe); nothing in the editor reads
    // it. Kept so the setter's callers stay as they are.
    float m_cpuFrameMs = 0.f;

    // ---- Post-process ----
    // Path of the currently bound post-process config (.ippc). Updated by the Save / Load
    // buttons + LoadScene; passed back into SaveScene.
    std::string m_postProcessConfigPath;
    // Path of the scene's bound Lua scene script (W-line sceneScript=, the OnSceneEnter /
    // Update / Exit lifecycle). Updated by LoadScene + the Scene Script binding UI; passed
    // back into SaveScene so a re-save keeps a hand-authored binding.
    std::string m_sceneScriptPath;
    // The scene's scene-scoped Lua Systems (W-line systems=), round-tripped the same way and
    // edited in World Settings. They take effect on the next Play.
    std::vector<std::string> m_sceneSystems;
    // Path of the engine-default post-process profile (.ppprofile), the base look. Stamped
    // into the .ippc on Save Config so a scene restores its look.
    std::string m_engineProfilePath;

    // Load Scene is requested from inside the MainMenuBar (mid ImGui frame). Tearing down
    // inline would free descriptor heap slots that earlier ImGui::Image calls this frame
    // already captured as raw GPU handles, which the driver dereferences during EndImGuiFrame
    // (a crash inside nvwgf2umx.dll). So the reload is deferred to ProcessPendingActions(),
    // which App calls before the next BeginImGuiFrame.
    std::string m_pendingLoadScenePath;

    // ---- Scene save state ----
    // Path of the .iscene being authored. Stamped by a successful Load / Save As (and by
    // AdoptLoadedScene when the host supplies it), so File > Save Scene / Ctrl+S writes in
    // place instead of re-prompting.
    std::string m_currentScenePath;
    // Set at every undo record point, cleared on a successful save / fresh load.
    bool        m_sceneDirty = false;

    // The destructive op the unsaved-changes modal is currently blocking. Parked
    // here (with its argument) until the user answers Save / Discard / Cancel.
    PendingSceneOp m_pendingSceneOp = PendingSceneOp::None;
    std::string    m_pendingSceneOpPath;             // Load target
    bool           m_unsavedPopupOpenRequest = false; // menu-bar -> popup body handoff
    // Set when the user approved a host-requested quit; App's main loop polls it through
    // ConsumeExitApproved().
    bool           m_exitApproved            = false;
    // New Scene follows the SAME deferred path as Load (see the note on
    // m_pendingLoadScenePath): clearing the world mid-ImGui-frame frees
    // descriptor slots this frame's ImGui::Image calls already captured.
    bool           m_pendingNewScene         = false;

    // Save Scene is queued for the same slot. SaveSceneToPath ->
    // Renderer::ExportBakedProbeCubemaps -> GraphicsDX12::SaveTextureCubeToITEX
    // does FlushAndWait() and then records + submits its OWN one-shot command
    // list on the graphics queue; from a menu / Ctrl+S handler (inside
    // BeginImGuiFrame..EndImGuiFrame) that list executes BEFORE this frame's
    // already-recorded lists and re-states the probe cubemap array under them.
    std::string    m_pendingSaveScenePath;
    std::string    m_lastSaveResult = "none";
    // A save the control pipe asked for is a JOB (Harness/ControlChannelJobs.h):
    // its id and the [CONTROL] request that created it, so the deferred write's
    // result, reason and log lines reach `job get` - m_lastSaveResult alone is
    // overwritten by the next Ctrl+S and carries no reason. 0 = a human's save.
    std::uint64_t  m_pendingSaveJobId = 0;
    std::uint32_t  m_pendingSaveRequestId = 0;
    // Editor.NewScene / Editor.OpenScene are jobs the same way: they run in the deferred slot,
    // and the job finishes there (or fails, with why).
    std::uint64_t  m_pendingNewSceneJobId = 0;
    std::uint64_t  m_pendingLoadJobId     = 0;
    // Set when the unsaved-changes modal answered "Save": the destructive op it was blocking
    // (New / Load / Exit) is already queued optimistically, so a failed deferred write must
    // cancel it again, or a full disk would silently discard the edits the user asked to
    // keep.
    bool           m_pendingSaveGuardsSceneOp = false;
    // The exit modal's list of unsaved documents, taken when it opened (collecting asks every
    // window, the animation graph compiles), and its Save's request for ProcessPendingActions.
    std::vector<UnsavedDocument> m_unsavedDocuments;
    bool                         m_pendingSaveDocuments = false;

    // ---- Autosave / crash recovery ----
    // Periodic autosave is an Editor Preference (editor_prefs.ini "autosave" /
    // "autosaveMinutes", written only when non-default); the pre-Play snapshot follows the
    // same switch. Pinned to these defaults under the harness layout (see OnAttach).
    bool           m_autosaveEnabled     = true;
    double         m_autosaveIntervalSec = kDefaultAutosaveIntervalSec;
    double         m_lastAutosaveTime = 0.0;                // ImGui::GetTime seconds
    std::string    m_pendingAutosaveOffer;      // autosave file the modal offers
    std::string    m_pendingAutosaveOriginal;   // scene the user actually asked to load
    std::string    m_autosaveRestoreRepointPath;// post-load re-point target
    bool           m_autosavePopupOpenRequest = false;

    // ---- Editor preferences (persist across sessions) ----
    std::vector<std::string> m_recentScenes;

    CameraBookmark m_camBookmarks[kCameraBookmarks];

    // ---- Deferred collision-baker preview swap ----
    // "Preview baked in world" allocates GPU buffers + bindless slots (MeshLibrary::Load) and
    // "Restore originals" frees them and calls Renderer::OnWorldClear, which assumes a drained
    // GPU and a frame with nothing recorded yet. Both buttons therefore only record intent
    // here; the work runs from ProcessPendingBakePreview. The preview backup map lives at file
    // scope in EditorLayer.cpp (it needs MeshLibRef).
    PendingBakePreviewOp m_pendingBakePreviewOp = PendingBakePreviewOp::None;
    std::string          m_pendingBakePreviewPath;   // .meshlib to load (Load only)
    // (sourcePath, sourceMeshId) -> bakedMeshId, snapshotted from the bake job
    // at click time because the job is a function-local static of the popup.
    std::unordered_map<std::string,
        std::unordered_map<uint32_t, uint32_t>> m_pendingBakePreviewMap;

    // ---- Per-frame ----
    // The editor's own frame counter (see EditorFrame()). Bumped at the top of
    // ProcessPendingActions, before anything it drives.
    std::uint64_t m_editorFrame = 0;

    // ---- Log panel ----
    bool m_logShowSuccess = true;
    bool m_logShowInfo    = true;
    bool m_logShowWarning = true;
    bool m_logShowError   = true;
    // Auto-scroll: follows new lines while user is pinned to bottom; cancels
    // when user scrolls up, resumes when they scroll back down.
    bool m_logAutoScroll  = true;
    char m_logFilter[128] {0};
    // The panel's model: an incremental mirror of the Logger's record ring and
    // the filtered index the list draws through a clipper.
    Editor::OutputLogPanel m_outputLog;
    // -logpanel_verify: compare that mirror against the ring at shutdown and
    // log [LOGPANEL] verify (its reverse arm, -logpanel_verify_break, makes the
    // mirror drop one line so the comparison must go red).
    bool m_logPanelVerify = false;

    // ---- Decal Materials window ----
    std::string m_selectedDecalMatName;        // "" = no selection
    char        m_newDecalMatNameBuf[64] {0};  // "Create" row text buffer
};
