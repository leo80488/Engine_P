// EditorLayer's harness hooks: the Harness* members whose contract EditorLayer.h states (the
// material / camera / asset preview drivers, the split viewport, the preview's size / shape /
// rig / shadow / fixtures, the parity and lights-parity worlds, the churn, touch, appear and
// rewrite schedules, the fixture shots) and the per-frame harness ticks (TickHarnessPreview and
// the chain stage / panel tree / play-edit / main-pose probes). Most of it is inside
// WITH_HARNESS.

#include "Editor/EditorLayer.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/EditorLayerShared.h"
#include "Editor/MaterialPreviewWidget.h"
#include "Editor/CameraPreviewWindow.h"
#include "Editor/AssetPreviewWindow.h"
#include "Editor/MeshEditorWindow.h"
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/TextureViewerWindow.h"
#include "Editor/UIEditorWindow.h"
#include "Editor/MaterialPropertyPanel.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/NodeEditorSmoke.h"
#include "Editor/SplitViewportPanel.h"
#include "Editor/ChainPhysicsPreviewWindow.h"
#include "System/CommandLine.h"            // the -chainpreview_* probe flags
#ifdef WITH_HARNESS
// Headers only this file's WITH_HARNESS code reads (Engine.Cpp.props defines the macro in every
// configuration but Retail).
#include "Harness/ExitCodes.h"        // Harness::PreconditionFail (exit 12)
#include "Renderer/Preview/PreviewFixtures.h" // -preview_fixture synthesised materials
#include "Renderer/Preview/PreviewStudioRig.h" // -preview_parity reads the SAME rig table
#endif
#include "Editor/EntityRefPicker.h"
#include "ECS/PlayModeController.h"   // the parity worlds' Play-mode refusal
#include "Reflection/ComponentReflection.h"
#include "Sequencer/SequenceEval.h"
#include "ECS/HierarchyOps.h"         // Hierarchy::DestroySubtree for the split-cam fixtures
#include "Resource/SceneSerializer.h"
#include "Resource/AssetFS.h"          // SharedTwinDir: -preview_churn over a shared worktree's folders
#include "Resource/PostProcessConfig.h"
#include "Resource/EngineSettings.h"
#include "Editor/InputBindingsWindow.h"
#include "Platform/Window.h"
#include "Resource/MeshLibIdentity.h"
#include "Scene/MeshAssetRegistry.h"
#include "Tools/CollisionMeshBaker.h"
#include "Resource/MeshLibrary.h"
#include "Resource/AssetHeader.h"
#include "System/TaskSystem.h"
#include "Editor/Undo/ComponentEditAction.h"
#include "Editor/Undo/TransformEditAction.h"
#include "Editor/Undo/ValueEditAction.h"
#include "Editor/Undo/StructuralAction.h"
#include "Editor/Undo/RenameAction.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneInstanceLoader.h"
#include "Resource/ModelImportWrite.h"
#include "Resource/RetargetBaker.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "Editor/HumanoidMapping.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/AnimationLoader.h"
#include "Renderer/Renderer.h"
#include <chrono>
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "PostProcess/ProfileSystem.h"
#include "PostProcess/PostProcessProfile.h"
#include "PostProcess/PostProcessRuntime.h"
#include "System/Log.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/AssetManager.h"
#include "Resource/MaterialImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialOverrideApply.h"
#include "Resource/MaterialLiveOverlay.h"
#include "Resource/AssetRefs.h"              // ToLowerAscii (-preview_mesh, -preview_churn)
#include "Resource/DecalMaterialSerializer.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"
#include "Resource/TextureImporter.h"
#include "Resource/TextureImportRecord.h"
#include "Resource/AssetGuid.h"
#include "Resource/ShaderImporter.h"
#include "Resource/SceneImporter.h"
#include "Resource/PmxImporter.h"
#include "Resource/VrmImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/VmdImporter.h"
#include "ECS/SkyboxComponent.h"
#include "Editor/DebugDrawSystem.h"
#include "Scripting/ScriptSystem.h"     // SetActiveSceneScript (the parity worlds' teardown)
#include "Graphics/GraphicsDX12.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include "Editor/ChainRestPoseMath.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <optional>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

using namespace DirectX;


// ==== Harness hooks (the contract is in EditorLayer.h) ====
void EditorLayer::HarnessOpenMaterialAsset(const std::string& path)
{
    OpenMaterialAsset(path);
}

void EditorLayer::HarnessSetPreviewStartFrame(std::uint64_t frame)
{
    Editor::MaterialPreviewWidget::Get().SetStartFrame(frame);
}

// -preview_camera=<entityName>[:<w>x<h>].
void EditorLayer::HarnessOpenCameraPreview(const std::string& entityName,
                                           std::uint32_t w, std::uint32_t h)
{
    m_showCameraPreview = true;
    Editor::CameraPreviewWindow::Get().HarnessSelectByName(entityName);
    if (w != 0 && h != 0)
        Editor::CameraPreviewWindow::Get().HarnessSetSize(w, h);
}

// -preview_asset=<path>[:<px>]. The panel retries the mesh library's streaming, not the path;
// its own deadline reports a path that never produces a drawable part.
bool EditorLayer::HarnessOpenAssetPreview(const std::string& assetPath,
                                          std::uint32_t px)
{
    // Validate the size before opening anything: a panel opened and then refused would run on at
    // the default 256, a size nobody asked for.
    if (px != 0 && !Editor::AssetPreviewWindow::Get().HarnessSetSize(px))
    {
        LOG_ERROR("-preview_asset size '%u' is not one of the quantised sizes "
                  "128/256/384/512 the panel can reach by hand; the panel is "
                  "NOT opened.", px);
        return false;
    }
    // HarnessShow refuses (as an ERROR) an extension the panel cannot draw; the menu flag is set only
    // on acceptance, so a refused flag leaves no empty panel behind.
    if (!Editor::AssetPreviewWindow::Get().HarnessShow(assetPath)) return false;
    m_showAssetPreview = true;
    return true;
}

#ifdef WITH_HARNESS
// ==== Split-viewport and camera-panel hooks (WITH_HARNESS) ====
// Every entry point here is declared in EditorLayer.h's WITH_HARNESS block and touches members
// declared there (m_showSplitViewport, m_splitCam*): do not move a definition outside the gate,
// or Retail (the one configuration without the macro) fails to compile EditorCore. The block
// closes before IsQuantisedPreviewSize; the always-on preview hooks (size / shape / rig / shadow)
// stay outside it, matching the header.

// -preview_split=<cam1>,<cam2>[,<cam3>[,<cam4>]]:<w>x<h>[:<tick>]. The panel validates everything
// (2..4 cells, a size the quantiser leaves unchanged, a tick in 1..8) before the menu flag is
// set, so a refused flag leaves no empty panel behind.
bool EditorLayer::HarnessOpenSplitViewport(const std::vector<std::string>& camNames,
                                           std::uint32_t w, std::uint32_t h,
                                           std::uint32_t tick)
{
    if (!Editor::SplitViewportPanel::Get().HarnessConfigure(camNames, w, h, tick))
        return false;
    m_showSplitViewport = true;
    return true;
}

// -preview_split_cams=<baseEntityName>:<n> (the header says why packs cannot use a Lua driver).
bool EditorLayer::HarnessSetSplitCameras(const std::string& baseName,
                                         std::uint32_t n)
{
    if (baseName.empty() || n < 2 || n > 4) return false;
    m_splitCamBase    = baseName;
    m_splitCamCount   = n;
    m_splitCamSpawned = false;
    m_splitCamTries   = 0;
    LOG_INFO("[PREVIEW] split: %u camera(s) will be spawned from '%s' as soon as "
             "it is alive (SplitCam0..%u, yaws evenly spaced over 360 degrees, "
             "%.1f degrees apart)",
             n, baseName.c_str(), n - 1, 360.f / static_cast<float>(n));
    return true;
}

// Called at shutdown (Editor::PreviewHarness::Shutdown, after ReleaseThumbnailTiles, while the
// World is still whole) and at a world epoch change; without it the SplitCam* fixtures would leak
// into a manual save or into process exit. DestroySubtree, not the flat World::DestroyEntity, so a
// child parented to a fixture camera is not orphaned. Do not mark the scene dirty: that arms
// MaybeAutosave, which would write a rotating .iscene into the repo as a side effect of a test.
std::uint32_t EditorLayer::HarnessReleaseSplitCameras(const char* reason)
{
    if (!m_world || m_splitCamEntities.empty())
    {
        m_splitCamEntities.clear();
        m_splitCamSpawned = false;
        m_splitCamTries   = 0;
        return 0;
    }
    const std::size_t tracked = m_splitCamEntities.size();
    std::uint32_t destroyed = 0;
    for (Entity e : m_splitCamEntities)
    {
        // IsAlive first: the harness may have deleted one by hand, and an id whose slot was recycled
        // names somebody else's entity.
        if (e == NullEntity || !m_world->IsAlive(e)) continue;
        if (m_world->GetName(e).rfind("SplitCam", 0) != 0) continue;
        destroyed += Hierarchy::DestroySubtree(*m_world, e);
    }
    m_splitCamEntities.clear();
    m_splitCamSpawned = false;
    m_splitCamTries   = 0;

    // The stray scan: `released == tracked` only says this call took back what it held, not that no
    // earlier tenancy was orphaned (an ids-preserved epoch bump that dropped the list and respawned
    // would leave two live SplitCam* per Play/Stop). A name scan over the camera pool sees them:
    // every fixture camera has a CameraComponent and World has no name index. Runs per release, not
    // per frame.
    std::uint32_t strays = 0;
    m_world->ForEach<CameraComponent>([&](Entity e, CameraComponent&)
    {
        if (m_world->GetName(e).rfind("SplitCam", 0) == 0) ++strays;
    });

    // name=value: the pack parses it (expect.logLineFields). `released` (what DestroySubtree took)
    // must equal `tracked` (what the last tenancy spawned): more would mean destroying recycled ids
    // from an earlier epoch. `strays` must be 0 after the destroys. `spawns` / `rearms` /
    // `rearmReleased` are the session ledger (see the members).
    LOG_INFO("[PREVIEW] split: released=%u tracked=%zu strays=%u spawns=%u "
             "rearms=%u rearmReleased=%u reason=%s (DestroySubtree; the scene "
             "was NOT marked dirty)",
             destroyed, tracked, strays, m_splitCamSpawnEvents, m_splitCamRearms,
             m_splitCamRearmReleased, reason ? reason : "?");
    return destroyed;
}

bool EditorLayer::HarnessSetSplitHover(std::uint32_t cell)
{
    // The panel owns the bound: how many cells -preview_split configured, not Preview::kMaxPreviews
    // (the storage limit, which would accept an index Draw then drops).
    return Editor::SplitViewportPanel::Get().HarnessSetHoverCell(
               static_cast<int>(cell));
}

// One step of -preview_split_layout_at=<f>:<l>[,<f>:<l>...].
bool EditorLayer::HarnessSetSplitLayoutAt(std::uint64_t frame, int layout)
{
    // The panel owns the validation, including whether there is a panel at all.
    if (!Editor::SplitViewportPanel::Get().HarnessSetLayoutAt(frame, layout))
        return false;
    LOG_INFO("[PREVIEW] split: layout will switch to %s at panel frame %llu",
             (layout == 0) ? "1x2" : "2x2",
             static_cast<unsigned long long>(frame));
    return true;
}

// One step of -preview_split_resize_at=<f>:<w>x<h>[,<f>:<w>x<h>...].
bool EditorLayer::HarnessSetSplitResizeAt(std::uint64_t frame, std::uint32_t w,
                                          std::uint32_t h)
{
    if (!Editor::SplitViewportPanel::Get().HarnessSetResizeAt(frame, w, h))
        return false;
    LOG_INFO("[PREVIEW] split: cells will resize to %ux%u at panel frame %llu",
             w, h, static_cast<unsigned long long>(frame));
    return true;
}

// -preview_match_size_at=<frame>.
bool EditorLayer::HarnessSetSplitMatchSizeAt(std::uint64_t frame)
{
    if (!Editor::SplitViewportPanel::Get().HarnessSetMatchSizeAt(frame))
        return false;
    LOG_INFO("[PREVIEW] split: the first refused cell will press Match size at "
             "panel frame %llu", static_cast<unsigned long long>(frame));
    return true;
}

// -preview_camera_resize=<frame>:<w>x<h>.
bool EditorLayer::HarnessSetCameraPreviewResize(std::uint64_t frame,
                                                std::uint32_t w, std::uint32_t h)
{
    // The panel owns the validation, and refuses when no -preview_camera opened
    // it: a resize of a panel that is not there would measure nothing.
    if (!Editor::CameraPreviewWindow::Get().HarnessSetResize(frame, w, h))
        return false;
    LOG_INFO("[PREVIEW] camera panel will resize to %ux%u at panel frame %llu",
             w, h, static_cast<unsigned long long>(frame));
    return true;
}

// -preview_camera_close_at=<frame>.
bool EditorLayer::HarnessSetCameraPreviewCloseAt(std::uint64_t frame)
{
    // The panel owns the validation, including whether there is a panel at all.
    if (!Editor::CameraPreviewWindow::Get().HarnessSetCloseAt(frame))
        return false;
    LOG_INFO("[PREVIEW] camera panel will close at panel frame %llu",
             static_cast<unsigned long long>(frame));
    return true;
}
#endif  // WITH_HARNESS

// The quantised set the widget itself snaps to (MaterialPreviewWidget's QuantiseSize). Any other
// size is refused, not rounded: a size the panel cannot reach by hand would exercise a recompile
// no user can provoke.
static bool IsQuantisedPreviewSize(std::uint32_t px)
{
    return px == 128u || px == 256u || px == 384u || px == 512u;
}

bool EditorLayer::HarnessSetPreviewSize(std::uint32_t px)
{
    if (!IsQuantisedPreviewSize(px)) return false;
    Editor::MaterialPreviewWidget::Get().SetHarnessSize(px);
    return true;
}

bool EditorLayer::HarnessSetPreviewResize(std::uint64_t frame, std::uint32_t px)
{
    if (!IsQuantisedPreviewSize(px)) return false;
    Editor::MaterialPreviewWidget::Get().SetHarnessResize(frame, px);
    return true;
}

bool EditorLayer::HarnessSetPreviewShape(const std::string& name)
{
    Preview::Shape s;
    if      (name == "sphere") s = Preview::Shape::Sphere;
    else if (name == "cube")   s = Preview::Shape::Cube;
    else if (name == "plane")  s = Preview::Shape::Plane;
    else if (name == "torus")  s = Preview::Shape::Torus;
    else return false;
    Editor::MaterialPreviewWidget::Get().SetHarnessShape(s);
    return true;
}

// The view controls below (background, mesh, orbit, rig) follow the size and shape hooks above: an
// unrecognised value is refused, not defaulted (a silently ignored flag is a false pass).
bool EditorLayer::HarnessSetPreviewBackground(const std::string& name)
{
    Preview::Background b;
    if      (name == "solid")   b = Preview::Background::Solid;
    else if (name == "checker") b = Preview::Background::Checker;
    else if (name == "skybox")  b = Preview::Background::Skybox;
    else return false;
    Editor::MaterialPreviewWidget::Get().SetHarnessBackground(b);
    return true;
}

bool EditorLayer::HarnessSetPreviewMesh(const std::string& spec)
{
    // "<path.meshlib>" or "<path.meshlib>#<id>", parsed by hand so a malformed id is a refusal, not
    // a partial read.
    std::string path = spec;
    std::uint32_t id = 0;
    const std::size_t hash = spec.rfind('#');
    if (hash != std::string::npos)
    {
        if (hash + 1 >= spec.size()) return false;
        const std::string idStr = spec.substr(hash + 1);
        for (char c : idStr) if (c < '0' || c > '9') return false;
        id   = static_cast<std::uint32_t>(std::strtoul(idStr.c_str(), nullptr, 10));
        path = spec.substr(0, hash);
    }
    // Only .meshlib: an .imsh has no path to a mesh-descriptor slot, and a flag that quietly
    // previewed a sphere instead would be a false pass. Case-insensitive like every other extension
    // test in the editor (AssetTypeForExt takes a ToLowerAscii'd extension), so "Head.MESHLIB", which
    // the Asset Browser types as a Mesh, is accepted.
    if (path.size() < 9 ||
        Resource::AssetRefs::ToLowerAscii(path.substr(path.size() - 8)) != ".meshlib")
        return false;
    Editor::MaterialPreviewWidget::Get().SetHarnessMesh(path, id);
    LOG_INFO("[PREVIEW] mesh override: '%s' mesh %u", path.c_str(), id);
    return true;
}

bool EditorLayer::HarnessSetPreviewOrbit(const std::string& spec)
{
    const std::size_t comma = spec.find(',');
    if (comma == std::string::npos || comma == 0 || comma + 1 >= spec.size())
        return false;
    try
    {
        const float yawDeg   = std::stof(spec.substr(0, comma));
        const float pitchDeg = std::stof(spec.substr(comma + 1));
        // Degrees on the command line, radians in the ViewDesc.
        constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
        Editor::MaterialPreviewWidget::Get().SetHarnessOrbit(yawDeg * kDeg2Rad,
                                                             pitchDeg * kDeg2Rad);
        LOG_INFO("[PREVIEW] orbit seeded: yaw %.1f deg, pitch %.1f deg",
                 yawDeg, pitchDeg);
        return true;
    }
    catch (...) { return false; }
}

bool EditorLayer::HarnessSetPreviewRig(const std::string& name)
{
    if (name == "full")
    {
        // A no-op that says so: `full` is the default, and clearing the harness feature mask would
        // silently undo bits other flags set. A pack passes `full` to state the configuration it
        // expects; the log line makes that checkable.
        LOG_INFO("[PREVIEW] studio rig: FULL (key + clustered fill + back) - "
                 "this is the default; nothing changed");
        return true;
    }
    if (name == "key")
    {
        // OR'd into the harness feature bits rather than replacing them: a pack
        // that also pins kNoAmbientFill (parity) must keep it.
        Editor::MaterialPreviewWidget::Get().AddHarnessFeatures(
            Preview::Feature::kKeyLightOnly);
        LOG_INFO("[PREVIEW] studio rig: KEY ONLY (clustered fill + back dropped)");
        return true;
    }
    return false;
}

void EditorLayer::HarnessSetPreviewShadow(bool on)
{
    Editor::MaterialPreviewWidget::Get().SetStudioShadow(on);
    LOG_INFO("[PREVIEW] studio shadow %s", on ? "ON" : "OFF");
}

void EditorLayer::HarnessSetPreviewSceneLighting(bool on)
{
    Editor::MaterialPreviewWidget::Get().SetSceneLighting(on);
    LOG_INFO("[PREVIEW] scene lighting %s (non-deterministic when ON)",
             on ? "ON" : "OFF");
}

#ifdef WITH_HARNESS
// ==== Harness preview fixtures (WITH_HARNESS; the contract is in EditorLayer.h) ====
// Engine.Cpp.props defines the macro per configuration (all but Retail) and EditorCore imports it,
// so every TU of one configuration agrees: a configuration macro is legal here where WITH_EDITOR
// would not be. The declarations, members and includes carry the same gate; ProcessPendingActions
// calls an inline no-op stub without it.
bool EditorLayer::InstallFixtureMaterial(const std::string& name)
{
    auto edit = std::make_unique<MaterialComponent>();
    if (!Preview::MakeFixtureMaterial(name, *edit)) return false;

    // Everything OpenMaterialAsset does, minus the disk read. The widget keys its view lifecycle on
    // the synthetic path; m_assetMatInfo is reset so the Inspector does not carry the previous
    // asset's parent / override set onto a material that has neither.
    m_assetMatPath  = "<fixture:" + name + ">";
    m_assetMatEdit  = std::move(edit);
    m_assetMatInfo  = Resource::MaterialLoadInfo{};
    m_assetMatDirty = false;
    ClearEntitySelection();

    LOG_INFO("[PREVIEW] fixture '%s' installed at editor frame %llu "
             "(expected stencil %u)", name.c_str(),
             static_cast<unsigned long long>(m_harnessFrameTick),
             Preview::FixtureStencil(name));
    return true;
}

bool EditorLayer::HarnessSetPreviewFixtures(const std::vector<std::string>& names,
                                            std::uint64_t startFrame,
                                            std::uint32_t everyFrames)
{
    if (names.empty()) return false;
    // All or nothing: one bad name would run a shorter schedule than the pack asserts and fail
    // hundreds of frames later as "file not written".
    for (const std::string& n : names)
    {
        MaterialComponent probe;
        if (!Preview::MakeFixtureMaterial(n, probe))
        {
            LOG_ERROR("-preview_fixture: '%s' is not a known fixture.", n.c_str());
            return false;
        }
    }
    m_previewFixtures     = names;
    m_previewFixtureStart = startFrame;
    m_previewFixtureEvery = everyFrames ? everyFrames : 40u;
    // SIZE_MAX, not 0 (see the member): 0 is a real index, so the tick would see want == current for
    // the first fixture and never install it.
    m_previewFixtureIdx   = static_cast<std::size_t>(-1);
    m_previewFixtureShot  = false;

    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i)
    { if (i) joined += ','; joined += names[i]; }
    LOG_INFO("[PREVIEW] fixture schedule: %zu fixture(s) [%s], %u frame(s) each, "
             "starting at editor frame %llu", names.size(), joined.c_str(),
             m_previewFixtureEvery,
             static_cast<unsigned long long>(m_previewFixtureStart));
    return true;
}

// Per window, not per run (see the header): expect.imagePair compares two captures of one run.
void EditorLayer::HarnessSetPreviewShadowAlpha(
    const std::vector<std::uint8_t>& perWindow)
{
    m_previewShadowAlpha = perWindow;
    if (m_previewShadowAlpha.empty()) return;
    std::string joined;
    for (std::size_t i = 0; i < m_previewShadowAlpha.size(); ++i)
    { if (i) joined += ','; joined += m_previewShadowAlpha[i] ? '1' : '0'; }
    LOG_INFO("[PREVIEW] studio shadow alpha schedule: [%s] (one per fixture "
             "window; the last entry repeats)", joined.c_str());
}

// The studio shadow pass per window (the header says why -preview_shadow is not enough).
void EditorLayer::HarnessSetPreviewShadowWindows(
    const std::vector<std::uint8_t>& perWindow)
{
    m_previewShadowWin = perWindow;
    if (m_previewShadowWin.empty()) return;
    std::string joined;
    for (std::size_t i = 0; i < m_previewShadowWin.size(); ++i)
    { if (i) joined += ','; joined += m_previewShadowWin[i] ? '1' : '0'; }
    LOG_INFO("[PREVIEW] studio shadow schedule: [%s] (one per fixture window; "
             "the last entry repeats)", joined.c_str());
}

void EditorLayer::HarnessSetPreviewEditAt(std::uint64_t frame)
{
    m_previewEditAtFrame = frame;
}

void EditorLayer::HarnessSetPreviewAssetEditMaterial(const std::string& matPath)
{
    m_previewAssetEditMatPath = matPath;
}

bool EditorLayer::HarnessSetPreviewParity(const std::string& fixtureName)
{
    if (!InstallFixtureMaterial(fixtureName)) return false;
    // The preview half of the parity configuration: kNoAmbientFill's comment (PreviewTypes.h) says
    // why the ambient stand-in goes, m_harnessHideGrid's why the editor grid does.
    Editor::MaterialPreviewWidget::Get().SetHarnessFeatures(
        Preview::Feature::kNoAmbientFill);
    // ---- Studio shadow off for the parity run ----
    // The preview's shadow is a 512^2 ortho slice fitted to a 3.2 m box around the subject; the main
    // pipeline's is a 4-cascade CSM fitted to the main camera's frustum with a rotating Bayer phase
    // (shadowFrameIndex) the preview pins to 0. They cannot be bit-identical, and a tolerance wide
    // enough would hide real shading differences. Both sides stay unshadowed by construction (the
    // fixtures carry DISABLE_RECEIVE_SHADOW and no CAST_SHADOW), so the comparison is exact; the
    // shadow path has its own coverage in preview_fixtures' `standard_shadow` row.
    Editor::MaterialPreviewWidget::Get().SetHarnessFeatureClear(
        Preview::Feature::kStudioShadow);
    // Fill the frame (see kParityDistanceScale): at the panel's own framing half of both pictures is
    // background, where the two pipelines differ structurally (the preview has no SkyboxPass).
    Editor::MaterialPreviewWidget::Get().SetHarnessDistanceScale(
        Preview::kParityDistanceScale);
    m_harnessHideGrid = true;
    // ...and the light gizmo: DebugDrawSystem::iconsEnabled defaults to true with Lights in the mask,
    // painting a billboard into HDR scene colour at each light. The parity light sits at the origin,
    // inside the subject, so its icon would be a saturated blob over the sphere in the main capture
    // only. Not persisted (editor_prefs.ini has no debug-draw keys), so it cannot leak into a later
    // session.
    if (m_debugDraw) m_debugDraw->iconsEnabled = false;
    // The World half is queued: it clears the world, which is legal only in the
    // ProcessPendingActions window (where New Scene does the same teardown).
    m_pendingParityFixture = fixtureName;
    LOG_INFO("[PREVIEW] parity fixture '%s': ambient stand-in OFF, editor grid "
             "hidden, world build queued", fixtureName.c_str());
    return true;
}

bool EditorLayer::HarnessSetLightsParity(const std::string& arm)
{
    int want = 0;
    if      (arm == "baseline") want = 1;
    else if (arm == "lights")   want = 2;
    else return false;

    // The same two editor overlays HarnessSetPreviewParity removes: the grid and the light-gizmo
    // billboards are drawn into HDR scene colour, so they appear only in the main capture, and this
    // fixture has six lights in frame. Neither is persisted.
    m_harnessHideGrid = true;
    if (m_debugDraw) m_debugDraw->iconsEnabled = false;

    // Queued, not built here: it clears the World, which is legal only in the ProcessPendingActions
    // window.
    m_pendingLightsParity = want;
    LOG_INFO("[PREVIEW] lights parity arm '%s': editor grid and light gizmos "
             "hidden, world build queued", arm.c_str());
    return true;
}

// The secondary-view lights fixture world: the main pipeline and the WorldCull preview lane draw
// the same pixels from the same camera, so the only difference left is the thing under test.
// Four construction rules, each removing a structural difference:
//   1. No sky pixels. SkyboxPass#3 draws the radiance cube with sun disk, moon and stars forced
//      off, the main pipeline as authored; the floor and back planes cover the whole frustum
//      (the framing arithmetic is on kLightsParityCamera).
//   2. No directional light. A secondary view borrows the main camera's cascades, so a
//      directional shadow outside the frusta's intersection would be wrong on one side only;
//      with lightColor zero the borrowed CSM contributes nothing.
//   3. StaticSkybox, not ProceduralAtmosphere: a procedural sky gives the two sides different
//      radiance during warm-up and a non-zero aerialMaxDistKm on the main side only.
//   4. No castsShadow on any punctual light: the preview binds no punctual atlases and forces
//      shadowSliceIdx to 0xFFFFFFFF, so a shadow-casting light would be shadowed on one side.
// TODO(MultiViewport_MaterialPreview_Plan NF-2): per-view cascades for a secondary view.
void EditorLayer::BuildLightsParityWorld(bool withLights)
{
    if (!m_world) return;

    // ---- Teardown: the New Scene order, as in BuildParityWorld ----
    // That function says why. The same single omission: Scene::MigrateEnvironment, because this
    // function authors its own WorldEnvironment singleton below.
    m_selectedEntity     = NullEntity;
    m_editHistory.Clear();
    m_xformRecording     = false;
    m_xformBaselineValid = false;
    m_fieldEdit.open     = false;
    if (m_gfx)        m_gfx->WaitIdleAndReleaseDeferred();
    if (m_renderer)   m_renderer->OnWorldClear();
    if (m_physicsSys) m_physicsSys->OnWorldClear();
    m_world->Clear();
    m_postProcessConfigPath.clear();
    m_sceneScriptPath.clear();
    m_sceneSystems.clear();
    m_currentScenePath.clear();
    m_editorFolders.clear();
    MarkSceneClean();
    if (m_scriptSys)
        m_scriptSys->SetActiveSceneScript(std::string{}, std::string{});

    // ---- The environment singleton ----
    // The same table BuildParityWorld reads: both sides must be lit by bit-identical IBL, and the
    // preview takes irradiance / radiance / mip count from the engine's resolved handles.
    {
        Entity env = m_world->CreateEntity();
        m_world->SetName(env, "LightsParityEnvironment");
        WorldEnvironmentComponent wec{};
        wec.mode = WorldEnvironmentComponent::Mode::StaticSkybox;
        wec.iblStrength        = Preview::kIBLStrength;
        wec.enableTimeOfDay    = false;
        m_world->AddComponent<WorldEnvironmentComponent>(env, wec);

        SkyboxComponent sky{};
        sky.irradiancePath    = Preview::kIrradiancePath;
        sky.radiancePath      = Preview::kRadiancePath;
        sky.skyboxPath.clear();
        sky.radianceMipLevels = Preview::kRadianceMipLevels;
        m_world->AddComponent<SkyboxComponent>(env, sky);
    }

    // ---- The room: a floor slab and a back wall ----
    // MeshManager's persistent Cube primitive, which both pipelines draw from the same descriptor
    // slot, with a default MaterialComponent: this test is about the light set reaching the shader,
    // so a plain opaque PBR surface keeps "the same pixel, twice" simplest.
    // Do not read `draws=` as a cull result: BuildScene_CullBVH makes no BVH leaf for a
    // primitive-only entity, so the secondary cull keeps everything (both sides draw the same set).
    auto addSlab = [&](const char* name, const DirectX::XMFLOAT3& t,
                       const float s[3])
    {
        Entity e = m_world->CreateEntity();
        m_world->SetName(e, name);
        LocalTransform lt{};
        lt.translation = t;
        lt.scale       = { s[0], s[1], s[2] };
        m_world->AddComponent<LocalTransform>(e, lt);
        m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
        m_world->AddComponent<VisibilityComponent>(e, VisibilityComponent{});
        m_world->AddComponent<RenderLayer>(e, RenderLayer{});
        m_world->AddComponent<Children>(e, Children{});
        MeshHandle mh;
        mh.gpuMeshID = static_cast<uint32_t>(PrimitiveMeshType::Cube);
        m_world->AddComponent<MeshHandle>(e, mh);
        m_world->AddComponent<BoundingVolume>(e, BoundingVolume{});
        MaterialComponent mat{};
        mat.SetDirty();
        m_world->AddComponent<MaterialComponent>(e, std::move(mat));
    };
    addSlab("LightsParityFloor",
            { 0.f, Preview::kLightsParityFloorY, Preview::kLightsParityFloorZ },
            Preview::kLightsParityFloorScale);
    addSlab("LightsParityWall",
            { 0.f, Preview::kLightsParityWallY, Preview::kLightsParityWallZ },
            Preview::kLightsParityWallScale);

    // ---- The camera entity ----
    // A real entity, unlike the studio parity camera, because -preview_pip resolves its camera by
    // name. The main camera is still pinned from the constant (-preview_pip_parity,
    // Editor::PreviewHarness::PinParityCamera), never from this entity: CameraResolveSystem and the
    // VCam blend stack can each add a frame of lag, which reads as a shading difference. Both come
    // from kLightsParityCamera.
    {
        const Preview::LightsParityCamera& lc = Preview::kLightsParityCamera;
        Entity e = m_world->CreateEntity();
        m_world->SetName(e, Preview::kLightsParityCameraName);
        DirectX::XMFLOAT3 f{ lc.forward[0], lc.forward[1], lc.forward[2] };
        const float yaw   = std::atan2(f.x, f.z);
        const float pitch = -std::asin(std::clamp(f.y, -1.f, 1.f));
        LocalTransform lt{};
        lt.translation = { lc.eye[0], lc.eye[1], lc.eye[2] };
        DirectX::XMStoreFloat4(&lt.rotation,
            DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.f));
        m_world->AddComponent<LocalTransform>(e, lt);
        m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
        m_world->AddComponent<Children>(e, Children{});
        CameraComponent cc{};
        cc.fov   = lc.fovY;
        cc.nearZ = lc.nearZ;
        cc.farZ  = lc.farZ;
        m_world->AddComponent<CameraComponent>(e, cc);
    }

    // ---- The punctual lights (the `lights` arm only) ----
    unsigned points = 0, spots = 0;
    if (withLights)
    {
        for (unsigned i = 0; i < Preview::kLightsParityLightCount; ++i)
        {
            const Preview::LightsParityLight& L = Preview::kLightsParityLights[i];
            Entity e = m_world->CreateEntity();
            char nm[64];
            std::snprintf(nm, sizeof(nm), "LightsParity%s%u",
                          L.spot ? "Spot" : "Point", i);
            m_world->SetName(e, nm);

            LocalTransform lt{};
            lt.translation = { L.position[0], L.position[1], L.position[2] };
            if (L.spot)
            {
                // LightData has no direction: it is the entity's +Z forward (LightUtil::DirectionFromWorld), and
                // the table's `direction` is the travel direction, the same convention. Same decomposition as
                // the light gizmo.
                DirectX::XMFLOAT3 d;
                DirectX::XMStoreFloat3(&d, DirectX::XMVector3Normalize(
                    DirectX::XMVectorSet(L.direction[0], L.direction[1],
                                         L.direction[2], 0.f)));
                const float yaw   = std::atan2(d.x, d.z);
                const float pitch = -std::asin(std::clamp(d.y, -1.f, 1.f));
                DirectX::XMStoreFloat4(&lt.rotation,
                    DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.f));
            }
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
            m_world->AddComponent<Children>(e, Children{});

            LightData ld{};
            ld.type      = L.spot ? LightType::Spot : LightType::Point;
            ld.color     = { L.color[0], L.color[1], L.color[2] };
            ld.intensity = L.intensity;
            ld.radius    = L.radius;
            ld.spotAngle = L.spotAngle;
            // Construction rule 4 (see the function comment).
            ld.castsShadow = false;
            m_world->AddComponent<LightData>(e, ld);
            if (L.spot) ++spots; else ++points;
        }
    }

    // ---- Bloom, zeroed on the live base profile ----
    // The same process-global write BuildParityWorld announces, for the same reason: the engine
    // default ships bloom.strength 0.04 while the preview binds a black bloom source at strength 0,
    // and there is no settings key for it. MakeEngineDefaultProfile() is untouched.
    PostProcess::ProfileSystem::Get().EngineDefault().bloom.strength.Set(0.0f);
    LOG_INFO("[PREVIEW] lights parity: bloom strength forced to 0 on the live "
             "engine-default profile (no settings key; MakeEngineDefaultProfile "
             "is unchanged)");

    // The witness line the packs gate on: `arm` tells the otherwise identical runs apart.
    LOG_INFO("[PREVIEW] lights fixture built: arm=%s points=%u spots=%u "
             "directional=0 slabs=2 camera=%s",
             withLights ? "lights" : "baseline", points, spots,
             Preview::kLightsParityCameraName);
}

void EditorLayer::BuildParityWorld(const std::string& fixtureName)
{
    if (!m_world) return;

    MaterialComponent fixture;
    if (!Preview::MakeFixtureMaterial(fixtureName, fixture)) return;

    // ---- The fixture's identity, read off the component ----
    // Captured here because `fixture` is moved into the world below. On the log line because the
    // parity packs differ only in -preview_parity=<name> and everything else they assert is
    // symmetric or shared (the imagePair compares the two sides of one run, the stencil is kPBR for
    // standard / standard_shadow / glass / twosided, a luma band fits a neighbour too): these six
    // numbers let a pack pin the material it believes it compares.
    const unsigned fxModel   = static_cast<unsigned>(fixture.customShadingModel);
    const unsigned fxTwoSide = fixture.IsDoubleSided() ? 1u : 0u;
    const float    fxSss     = fixture.subsurfaceStrength;
    const unsigned fxBlend   = static_cast<unsigned>(fixture.userBlendMode);
    const unsigned fxCast    = fixture.IsCastingShadow() ? 1u : 0u;
    const unsigned fxRecv    = fixture.IsReceiveShadow() ? 1u : 0u;

    // ---- Teardown: ProcessPendingNewScene minus one line ----
    // The middle four are the New Scene order: deferred GPU releases drain, the renderer and physics
    // drop their per-world caches, then the World loses its entities (ProcessPendingNewScene says
    // why). The rest is reset for the same reasons as there, not skipped as unneeded headless: the
    // edit history holds ids about to be recycled, a gizmo drag or open Inspector field would resume
    // against a missing target, and the .ippc / scene-script bindings belong to the outgoing scene.
    // The Play-mode refusal is at the call site (TickHarnessPreview): this function is unconditional
    // once entered.
    // The one omission is Scene::MigrateEnvironment: it would synthesise a second, default
    // WorldEnvironment singleton next to the one authored below, and the renderer picks the
    // singleton out of a pool.
    m_selectedEntity     = NullEntity;
    m_editHistory.Clear();
    m_xformRecording     = false;
    m_xformBaselineValid = false;
    m_fieldEdit.open     = false;
    if (m_gfx)        m_gfx->WaitIdleAndReleaseDeferred();
    if (m_renderer)   m_renderer->OnWorldClear();
    if (m_physicsSys) m_physicsSys->OnWorldClear();
    m_world->Clear();
    m_postProcessConfigPath.clear();
    m_sceneScriptPath.clear();
    m_sceneSystems.clear();
    m_currentScenePath.clear();
    m_editorFolders.clear();
    MarkSceneClean();
    if (m_scriptSys)
        m_scriptSys->SetActiveSceneScript(std::string{}, std::string{});

    // ---- The environment singleton ----
    // StaticSkybox, not ProceduralAtmosphere: the preview binds two shipped .itex cubes and never runs
    // the Hillaire LUTs, so a procedural sky would give the main pipeline different radiance.
    // An empty skyboxPath does not make the backdrop black: Renderer_IBL resolves it through a
    // fallback chain (skybox, else radiance, else irradiance), so SkyboxPass paints the radiance
    // cube; the empty path only saves a texture to sync and keep resident (the IBL handles are
    // separate). The framing keeps the backdrop out of the comparison: kParityDistanceScale zooms both
    // sides until the unit sphere covers the corners. Do not raise that scale, widen
    // kStudioCamera.fovY or shrink the primitive: the radiance cube would reappear at the corners,
    // where the preview (no SkyboxPass; it clears to its panel colour) can never match it.
    // ambientColor is not set: Renderer_IBL forces LightCB.ambient to zero in every sky mode, and the
    // preview's matching zero is Feature::kNoAmbientFill.
    {
        Entity env = m_world->CreateEntity();
        m_world->SetName(env, "ParityEnvironment");
        WorldEnvironmentComponent wec{};
        wec.mode = WorldEnvironmentComponent::Mode::StaticSkybox;
        // Authored (>= 0) so it does not fall back to AtmosphereComponent's hidden 0.1 default, which
        // would make the main pipeline's specular IBL a tenth of the preview's.
        wec.iblStrength        = Preview::kIBLStrength;
        wec.enableTimeOfDay    = false;
        m_world->AddComponent<WorldEnvironmentComponent>(env, wec);

        SkyboxComponent sky{};
        sky.irradiancePath    = Preview::kIrradiancePath;
        sky.radiancePath      = Preview::kRadiancePath;
        // Empty: the backdrop falls back to the radiance cube, which kParityDistanceScale keeps out of
        // frame (one texture fewer to sync, not a black background).
        sky.skyboxPath.clear();
        // Measured, not the component's default of 7 (see kRadianceMipLevels).
        sky.radianceMipLevels = Preview::kRadianceMipLevels;
        m_world->AddComponent<SkyboxComponent>(env, sky);
    }

    // ---- The key light ----
    // LightData has no direction: it is the entity's +Z forward (LightUtil::DirectionFromWorld), and
    // PreviewStudioRig::kKeyLight.dir is the travel direction, the same convention. Intensity stays 1
    // and the colour carries the magnitude, as the preview writes kKeyLight.color straight into
    // LightCB.lightColor. The shipped constant, not PreviewRenderer::KeyLightDir(): this world must
    // carry the preview's shipped rig, so do not combine -preview_key_light with -preview_parity (the
    // A side would move and the B side would not).
    {
        Entity lightE = m_world->CreateEntity();
        m_world->SetName(lightE, "ParityKeyLight");

        DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(DirectX::XMVectorSet(
            Preview::kKeyLight.dir[0], Preview::kKeyLight.dir[1],
            Preview::kKeyLight.dir[2], 0.f));
        // Build the rotation that maps +Z onto fwd. Yaw about world up then
        // pitch about the resulting right vector - the same decomposition the
        // light gizmo writes, so the value round-trips through the Inspector.
        DirectX::XMFLOAT3 f; DirectX::XMStoreFloat3(&f, fwd);
        const float yaw   = std::atan2(f.x, f.z);
        const float pitch = -std::asin(std::clamp(f.y, -1.f, 1.f));

        LocalTransform lt{};
        DirectX::XMStoreFloat4(&lt.rotation,
            DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.f));
        m_world->AddComponent<LocalTransform>(lightE, lt);
        m_world->AddComponent<GlobalTransform>(lightE, GlobalTransform{});
        m_world->AddComponent<Children>(lightE, Children{});

        LightData ld{};
        ld.type        = LightType::Directional;
        ld.color       = { Preview::kKeyLight.color[0],
                           Preview::kKeyLight.color[1],
                           Preview::kKeyLight.color[2] };
        ld.intensity   = 1.0f;
        // Ignored for directionals (CSM is always on); the two sides agree because the fixture material
        // carries DISABLE_RECEIVE_SHADOW (PreviewFixtures.h).
        ld.castsShadow = false;
        m_world->AddComponent<LightData>(lightE, ld);
    }

    // ---- Fill and back lights, as real point lights ----
    // The preview draws them as clustered point lights, so the parity world needs the same two. The
    // position rule lives in the rig: a StudioPointLight sits at -normalize(dir) * distance, computed
    // from the same table here and in PreviewRenderer's MakeRigPointLight. Do not type the resulting
    // coordinates here, or the test silently stops tracking the rig. The position comes from the
    // entity transform (LightData holds radius / intensity / colour), hence a translation here where
    // the directional got a rotation.
    {
        const Preview::StudioPointLight* rig[2] = { &Preview::kFillLight,
                                                    &Preview::kBackLight };
        const char* names[2] = { "ParityFillLight", "ParityBackLight" };
        for (int i = 0; i < 2; ++i)
        {
            const Preview::StudioPointLight& L = *rig[i];
            DirectX::XMFLOAT3 d;
            DirectX::XMStoreFloat3(&d, DirectX::XMVector3Normalize(
                DirectX::XMVectorSet(L.dir[0], L.dir[1], L.dir[2], 0.f)));

            Entity e = m_world->CreateEntity();
            m_world->SetName(e, names[i]);
            LocalTransform lt{};
            lt.translation = { -d.x * L.distance, -d.y * L.distance, -d.z * L.distance };
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
            m_world->AddComponent<Children>(e, Children{});

            LightData ld{};
            ld.type      = LightType::Point;
            ld.color     = { L.color[0], L.color[1], L.color[2] };
            ld.intensity = L.intensity;
            ld.radius    = L.radius;
            // No shadow slice on either side: the preview writes shadowSliceIdx 0xFFFFFFFF into its GPULight
            // records, and a castsShadow point light would take AccumClusteredLight's SamplePointShadow
            // branch on the main side only.
            ld.castsShadow = false;
            m_world->AddComponent<LightData>(e, ld);
        }
    }

    // ---- The subject ----
    // The primitive the preview draws (MeshManager's persistent sphere) with the identity transform
    // the preview writes (PreviewRenderer::ProcessQueue).
    {
        Entity e = m_world->CreateEntity();
        m_world->SetName(e, "ParitySubject");
        m_world->AddComponent<LocalTransform>(e, LocalTransform{});
        m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
        m_world->AddComponent<VisibilityComponent>(e, VisibilityComponent{});
        m_world->AddComponent<RenderLayer>(e, RenderLayer{});
        m_world->AddComponent<Children>(e, Children{});
        MeshHandle mh;
        mh.gpuMeshID = static_cast<uint32_t>(PrimitiveMeshType::Sphere);
        m_world->AddComponent<MeshHandle>(e, mh);
        m_world->AddComponent<BoundingVolume>(e, BoundingVolume{});
        // NO MaterialSourcePath: the fixture has no file, and a stale path
        // would invite ReloadMaterialsAffectedBy to overwrite it from disk.
        fixture.SetDirty();
        m_world->AddComponent<MaterialComponent>(e, std::move(fixture));
    }

    // ---- Bloom ----
    // The one post knob with no settings key: the engine default ships bloom.strength 0.04 and
    // ToneMapPass composites bloomSrv * strength, while the preview binds a black bloom source at
    // strength 0 (everything else the preview lacks is set by tests/profiles/preview_parity.json).
    // This writes process-global state, so it is announced: PostProcessConfig::ApplyTo (back to
    // MakeEngineDefaultProfile() plus the player's toggles) and EngineSettings::ApplyTo both re-seed
    // EngineDefault(). Neither runs after this in the unattended parity sequence; if one did, bloom
    // would return at 0.04 and fail the packs as a shading difference, and the log line below (both
    // parity packs assert it) is the clue. MakeEngineDefaultProfile() itself is untouched, so every
    // other run keeps bloom at 0.04.
    PostProcess::ProfileSystem::Get().EngineDefault().bloom.strength.Set(0.0f);
    LOG_INFO("[PREVIEW] parity: bloom strength forced to 0 on the live "
             "engine-default profile (no settings key; MakeEngineDefaultProfile "
             "is unchanged)");

    LOG_INFO("[PREVIEW] parity world built for fixture '%s': 1 sphere, 1 "
             "directional key light, 2 point lights (rig fill + back), "
             "StaticSkybox IBL; the backdrop falls back to the radiance cube "
             "and is framed out by kParityDistanceScale. fixture identity: "
             "shadingModel=%u doubleSided=%u subsurfaceStrength=%.3f "
             "blendMode=%u castShadow=%u recvShadow=%u",
             fixtureName.c_str(), fxModel, fxTwoSide, fxSss, fxBlend,
             fxCast, fxRecv);
}

bool EditorLayer::CapturePreviewPNG(const std::string& fileStem)
{
    if (!m_gfx || !m_renderer)  return false;
    if (!Sys::HasArtifactDir()) return false;

    const std::uint32_t viewId = m_renderer->GetLastRenderedPreviewView();
    const RHI::Texture* tex    = m_renderer->GetPreviewOutputTexture(viewId);
    if (!tex) return false;

    // Captured here, at the ProcessPendingActions frame boundary, not through HarnessCaptureSystem
    // (one shot at -at_frame; a fixture cycle needs one per window). The hard sync a capture performs
    // is legal here: the render half is parked, the device frame has not opened, and the previous
    // frame's ToneMapPass#2 left its output in SHADER_RESOURCE (TransitionForDisplay), the state
    // CaptureTextureToPNG assumes.
    const std::string path = Sys::ArtifactDir() + "/" + fileStem + ".png";
    return m_gfx->CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE,
                                      path.c_str());
}

bool EditorLayer::ServiceFixtureShot(const std::string& name, bool lastChance,
                                     std::size_t occurrence)
{
    if (!m_renderer) return false;

    // ---- The permutation check, and what it proves ----
    // wantStencil and gotStencil are not independent: both are ShadingStencil::RefFor of this
    // fixture's shading model, one derived here, one carried by the preview's draw packet. So this
    // cannot catch a wrong OMSetStencilRef, a broken PSO stencil test or a Lighting.ps binary that
    // ignores the ref. It proves the install path (asset buffer -> widget -> ViewDesc -> packet)
    // delivered this fixture's material, not the previous fixture's re-rendered under the new name.
    // The permutation evidence is measured elsewhere: standard vs toon (Toon() is Base() with only
    // customShadingModel changed, yet their mean lumas differ), the mutually distinct measured lumas
    // in preview_fixtures/scenario.json (standard and masked are pixel-identical by design: constant
    // coverage 1.0 against the opaque default textures; `dithered` proves the clip), and the A/B
    // parity packs, where a preview shading through the wrong Lighting.ps variant stops matching the
    // main pipeline (which picks its variant from its own GBuffer stencil).
    const unsigned wantStencil = Preview::FixtureStencil(name);
    const unsigned gotStencil  = m_renderer->GetLastRenderedPreviewStencil();
    // The route of the same picture (1 = TransparentPass#2). Logged, not checked: no fixture declares
    // a route; preview_fwd_parity asserts it per fixture with logLineFields.
    const unsigned gotForward  = m_renderer->GetLastRenderedPreviewForward();

    // Nothing has been drawn yet: no stencil to check and no pixels to write.
    // NOT latched by the caller, so the next frame of this window retries.
    if (gotStencil == 0u)
    {
        if (lastChance)
            LOG_ERROR("[PREVIEW] fixture '%s': no preview render to capture "
                      "- the pack's per-image expectation will fail.",
                      name.c_str());
        return false;
    }

    // Outside the artifact-dir gate on purpose: the check is about the engine, so a run without
    // -artifacts still asserts it.
    ENGINE_CHECK(gotStencil == wantStencil,
                 "preview fixture '%s' was drawn with stencil ref %u but its "
                 "shading model derives %u; the material that reached the draw "
                 "packet is not the one this fixture synthesised.",
                 name.c_str(), gotStencil, wantStencil);

    // No -artifacts: the check above is all this run wanted. Report success so
    // the window latches and the check does not repeat every frame.
    if (!Sys::HasArtifactDir()) return true;

    // The first window naming a fixture owns the plain file name; a schedule naming it twice (a
    // lever flipped between two windows) gets preview_<name>_2.png for the second.
    const std::string stem = (occurrence <= 1)
                           ? ("preview_" + name)
                           : ("preview_" + name + "_" +
                              std::to_string(occurrence));
    if (!CapturePreviewPNG(stem))
    {
        if (lastChance)
            LOG_ERROR("[PREVIEW] fixture '%s': capture refused or failed.",
                      name.c_str());
        return false;
    }
    // forward=N goes LAST so the packs that assert the whole
    // "captured stencil N -> preview_X.png" text still find it.
    LOG_INFO("[PREVIEW] fixture '%s' captured stencil %u -> %s.png forward=%u",
             name.c_str(), gotStencil, stem.c_str(), gotForward);
    return true;
}

// How long a churn folder is held: kChurnFramesPerMaterial per material plus kChurnFolderPad.
// The lane renders at most one picture per frame, but a request whose textures are still
// streaming is re-queued at the front and costs its frame (ExtractThumbnails never bakes a white
// thumbnail), so at one frame per material a large cold folder rotates away with materials never
// tiled and the cold pass leaks into the warm one; two frames per material clears it.
// The pad covers the round trip (enqueue, extract, record, the kFramesInFlight-deep readback ring,
// the .itex encode) plus time for -preview_thumb_evict_ms to elapse, so the eviction sweep runs
// while the harness watches. It is all an empty folder costs: dwell is per folder, not the widest
// folder's.
static constexpr std::uint64_t kChurnFramesPerMaterial = 2;
static constexpr std::uint64_t kChurnFolderPad         = 40;

bool EditorLayer::HarnessSetPreviewChurn(const std::string& dir)
{
    namespace fs = std::filesystem;
    m_thumbChurnFolders.clear();
    m_thumbChurnIdx = 0;

    std::error_code ec;
    // The folder's twin in the main checkout, for a worktree that reads its
    // binary assets from there (Resource/SharedContent.h): a folder holding
    // only payload exists only in the twin, and the rotation must be the tree
    // the browser shows, not this checkout's copy of it.
    const std::string twin = Resource::AssetFS::Get().SharedTwinDir(dir);
    if (!fs::exists(Sys::U8Path(dir), ec) && (twin.empty() || !fs::is_directory(Sys::U8Path(twin), ec)))
        return false;

    // Every folder under `dir`, with or without materials. The browser scans one directory at a time
    // (ScanAssets reads m_currentDir), so the churn rotates over folders, exercising the real path: a
    // rescan, the preview-state snapshot / restore, the orphan release of the previous folder's tiles,
    // a fresh round of enqueues. Do not skip material-free folders: rotating into one orphans every
    // tile of the previous folder, and the orphan release (a tile destroyed because its entry went
    // away, not because it aged out) runs only on a folder change.
    std::uint32_t materials = 0;
    auto countMaterials = [](const fs::path& d) -> std::uint32_t
    {
        std::error_code lec;
        std::uint32_t n = 0;
        for (const auto& e : fs::directory_iterator(d, lec))
        {
            std::error_code fec;
            if (!e.is_regular_file(fec)) continue;
            const std::string ext = Resource::AssetRefs::ToLowerAscii(
                Sys::PathToUtf8(e.path().extension()));
            if (ext == ".imat" || ext == ".imatinst") ++n;
        }
        return n;
    };
    auto consider = [&](const fs::path& d)
    {
        ChurnFolder f;
        f.path      = Sys::PathToUtf8(d);
        f.materials = countMaterials(d);
        // Dwell is per folder (see kChurnFramesPerMaterial).
        f.dwell     = static_cast<std::uint64_t>(f.materials) * kChurnFramesPerMaterial
                    + kChurnFolderPad;
        materials  += f.materials;
        m_thumbChurnFolders.push_back(std::move(f));
    };
    // Every folder under `dir` and under its twin, as paths relative to `dir`,
    // then in the order ONE tree's recursive_directory_iterator yields on NTFS -
    // pre-order, siblings by upper-cased name - so a shared worktree rotates
    // through the same folders in the same order as a full checkout.
    std::vector<std::vector<std::string>> rels;       // path components
    std::unordered_set<std::string> seenRel;          // lower-case "a/b/c"
    auto collect = [&](const fs::path& top)
    {
        std::error_code wec;
        for (auto it = fs::recursive_directory_iterator(top, wec);
             !wec && it != fs::recursive_directory_iterator(); it.increment(wec))
        {
            std::error_code dec;
            if (!it->is_directory(dec)) continue;
            std::vector<std::string> parts;
            std::string key;
            for (const fs::path& c : it->path().lexically_relative(top))
            {
                parts.push_back(Sys::PathToUtf8(c));
                key += Resource::AssetRefs::ToLowerAscii(parts.back()) + "/";
            }
            if (seenRel.insert(key).second) rels.push_back(std::move(parts));
        }
    };
    collect(Sys::U8Path(dir));
    if (!twin.empty()) collect(Sys::U8Path(twin));
    auto upperLess = [](const std::string& a, const std::string& b)
    {
        const std::size_t n = (std::min)(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            const unsigned char ca = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(a[i])));
            const unsigned char cb = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(b[i])));
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    };
    std::sort(rels.begin(), rels.end(), [&](const auto& a, const auto& b)
    {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), upperLess);
    });
    consider(Sys::U8Path(dir));
    for (const auto& parts : rels)
    {
        fs::path p = Sys::U8Path(dir);
        for (const std::string& c : parts) p /= Sys::U8Path(c);   // the iterator's own spelling
        consider(p);
    }

    if (materials == 0)
    {
        m_thumbChurnFolders.clear();
        LOG_ERROR("-preview_churn='%s' contains no .imat/.imatinst; the pack "
                  "would churn nothing and pass having tested nothing.",
                  dir.c_str());
        return false;
    }
    m_thumbChurnDir   = dir;
    m_thumbChurnCycle = 0;
    for (const ChurnFolder& f : m_thumbChurnFolders) m_thumbChurnCycle += f.dwell;

    LOG_INFO("[PREVIEW] churn armed: %zu folder(s), %u material(s) under '%s', "
             "%llu frames per pass, %u pass(es)",
             m_thumbChurnFolders.size(), materials, dir.c_str(),
             static_cast<unsigned long long>(m_thumbChurnCycle),
             m_thumbChurnPasses);
    return true;
}

void EditorLayer::HarnessSetPreviewChurnPasses(std::uint32_t passes)
{
    m_thumbChurnPasses = passes ? passes : 1u;
    LOG_INFO("[PREVIEW] churn passes -> %u (-preview_churn_passes)",
             m_thumbChurnPasses);
}

bool EditorLayer::HarnessSetPreviewTouchAt(std::uint64_t frame,
                                           const std::string& path)
{
    if (frame == 0 || path.empty()) return false;
    m_thumbTouchFrame = frame;
    m_thumbTouchPath  = path;
    m_thumbTouchFired = false;
    LOG_INFO("[PREVIEW] touch armed: '%s' at editor frame %llu "
             "(-preview_touch_at)", path.c_str(),
             static_cast<unsigned long long>(frame));
    return true;
}

bool EditorLayer::HarnessTakeDuePreviewTouch(std::string& outPath)
{
    if (m_thumbTouchFired || m_thumbTouchFrame == 0) return false;
    if (m_harnessFrameTick < m_thumbTouchFrame) return false;
    m_thumbTouchFired = true;
    outPath = m_thumbTouchPath;
    return true;
}

bool EditorLayer::HarnessSetAssetPreviewDeadline(std::uint32_t frames)
{
    return Editor::AssetPreviewWindow::Get().HarnessSetDeadlineFrames(frames);
}

bool EditorLayer::HarnessSetPreviewAppearAt(std::uint64_t frame,
                                            const std::string& srcPath,
                                            const std::string& dstPath)
{
    if (frame == 0 || srcPath.empty() || dstPath.empty()) return false;
    if (srcPath == dstPath) return false;   // would overwrite a shipped asset
    m_assetAppearFrame = frame;
    m_assetAppearSrc   = srcPath;
    m_assetAppearDst   = dstPath;
    m_assetAppearFired = false;
    LOG_INFO("[PREVIEW] asset appear armed: '%s' -> '%s' at editor frame %llu "
             "(-preview_asset_appear_at)", srcPath.c_str(), dstPath.c_str(),
             static_cast<unsigned long long>(frame));
    return true;
}

bool EditorLayer::HarnessTakeDuePreviewAppear(std::string& outSrc,
                                              std::string& outDst)
{
    if (m_assetAppearFired || m_assetAppearFrame == 0) return false;
    if (m_harnessFrameTick < m_assetAppearFrame) return false;
    m_assetAppearFired = true;
    outSrc = m_assetAppearSrc;
    outDst = m_assetAppearDst;
    return true;
}

bool EditorLayer::HarnessSetPreviewRewriteAt(std::uint64_t frame,
                                             const std::string& srcPath,
                                             const std::string& dstPath)
{
    if (frame == 0 || srcPath.empty() || dstPath.empty()) return false;
    if (srcPath == dstPath) return false;   // a no-op rewrite tests nothing
    m_assetRewriteFrame = frame;
    m_assetRewriteSrc   = srcPath;
    m_assetRewriteDst   = dstPath;
    m_assetRewriteFired = false;
    LOG_INFO("[PREVIEW] asset rewrite armed: '%s' -> '%s' (IN PLACE) at editor "
             "frame %llu (-preview_asset_rewrite_at)", srcPath.c_str(),
             dstPath.c_str(), static_cast<unsigned long long>(frame));
    return true;
}

bool EditorLayer::HarnessTakeDuePreviewRewrite(std::string& outSrc,
                                               std::string& outDst)
{
    if (m_assetRewriteFired || m_assetRewriteFrame == 0) return false;
    if (m_harnessFrameTick < m_assetRewriteFrame) return false;
    m_assetRewriteFired = true;
    outSrc = m_assetRewriteSrc;
    outDst = m_assetRewriteDst;
    return true;
}

void EditorLayer::HarnessClearThumbnailCache()
{
    const std::size_t n = Editor::PreviewThumbnailCache::Get().ClearAll();
    (void)n;   // ClearAll logs its own count
}

// -preview_asset_verify: parses the comma-separated target list. Nothing runs here (see
// TickHarnessPreview).
void EditorLayer::HarnessSetAssetVerify(const std::string& spec)
{
    m_assetVerifyPaths.clear();
    std::size_t at = 0;
    while (at <= spec.size())
    {
        const std::size_t comma = spec.find(',', at);
        std::string one = (comma == std::string::npos)
                        ? spec.substr(at) : spec.substr(at, comma - at);
        while (!one.empty() && one.front() == ' ') one.erase(one.begin());
        while (!one.empty() && one.back()  == ' ') one.pop_back();
        if (!one.empty())
            m_assetVerifyPaths.push_back(Resource::AssetRefs::PercentDecode(one));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    LOG_INFO("[PREVIEW] asset order verify armed for %zu target(s)",
             m_assetVerifyPaths.size());
}

// -preview_bake_restore_at (the contract is in EditorLayer.h).
bool EditorLayer::HarnessSetBakeRestoreAt(std::uint64_t frame,
                                          const std::string& libPath)
{
    if (frame == 0 || libPath.empty()) return false;
    m_bakeRestoreFrame   = frame;
    m_bakeRestoreLibPath = libPath;
    m_bakeRestoreDone    = false;
    LOG_INFO("[PREVIEW] bake restore armed: load '%s' at editor frame %llu, "
             "restore at %llu, verify at %llu (-preview_bake_restore_at)",
             libPath.c_str(), static_cast<unsigned long long>(frame),
             static_cast<unsigned long long>(frame + kBakeRestoreGap),
             static_cast<unsigned long long>(frame + kBakeRestoreGap +
                                             kBakeRestoreVerify));
    return true;
}

void EditorLayer::HarnessSetThumbnailEvictMs(std::uint32_t ms)
{
    m_previewEvictDelay = static_cast<float>(ms) / 1000.f;
    LOG_INFO("[PREVIEW] thumbnail eviction delay -> %.3f s (-preview_thumb_evict_ms)",
             m_previewEvictDelay);
}

// -preview_close_at / -preview_open_at (the contract is in EditorLayer.h).
bool EditorLayer::HarnessSetPreviewToggleFrames(
    const std::vector<std::pair<std::uint64_t, std::uint8_t>>& steps, bool open)
{
    for (const auto& step : steps)
    {
        const std::uint64_t f    = step.first;
        const std::uint8_t  mask = step.second;
        // An empty mask is a refusal, not a no-op step: "close nothing at frame 200" would measure a
        // session in which nothing closed under the name of one that closed something.
        if ((mask & kPreviewSurfaceAll) == 0)
        {
            LOG_ERROR("-preview_%s_at: the step at editor frame %llu names no "
                      "preview surface. Use material|camera|asset|split|all.",
                      open ? "open" : "close",
                      static_cast<unsigned long long>(f));
            return false;
        }
        if (f == 0)
        {
            LOG_ERROR("-preview_%s_at: frame 0 is not a schedulable point - the "
                      "editor frame counter has not ticked yet, so the step "
                      "would fire before any panel exists.",
                      open ? "open" : "close");
            return false;
        }
        for (std::uint32_t i = 0; i < m_previewToggleCount; ++i)
            if (m_previewToggleSteps[i].frame == f)
            {
                LOG_ERROR("-preview_%s_at: editor frame %llu already carries a "
                          "preview toggle. Two steps on one frame is an order "
                          "nothing defines, and the run would measure whichever "
                          "the parser happened to install second.",
                          open ? "open" : "close",
                          static_cast<unsigned long long>(f));
                return false;
            }
        if (m_previewToggleCount >= kMaxPreviewToggleSteps)
        {
            LOG_ERROR("-preview_close_at/-preview_open_at: at most %u steps "
                      "between them.", kMaxPreviewToggleSteps);
            return false;
        }
        PreviewToggleStep& s = m_previewToggleSteps[m_previewToggleCount++];
        s.frame    = f;
        s.open     = open;
        s.done     = false;
        s.surfaces = static_cast<std::uint8_t>(mask & kPreviewSurfaceAll);
    }
    // Sorted so the drain can stop at the first future step and so a close and
    // the open that follows it are applied in the order they were written,
    // whichever flag was parsed first.
    std::sort(m_previewToggleSteps, m_previewToggleSteps + m_previewToggleCount,
              [](const PreviewToggleStep& a, const PreviewToggleStep& b)
              { return a.frame < b.frame; });

    // ---- An open may only name surfaces a close before it named ----
    // The parse-time form of the runtime rule (TickHarnessPreview never writes a restore state no
    // close recorded), so the run is refused instead of silently doing less: reopening a panel that
    // was never closed would really be an extra close. Checked only when this call installed open
    // steps; Editor::PreviewHarness parses -preview_close_at first, so the union is complete here,
    // and an open with no close at all is refused (nothing captured a state to restore).
    if (open)
        for (std::uint32_t i = 0; i < m_previewToggleCount; ++i)
        {
            const PreviewToggleStep& o = m_previewToggleSteps[i];
            if (!o.open) continue;
            std::uint8_t closedBefore = 0;
            for (std::uint32_t j = 0; j < m_previewToggleCount; ++j)
            {
                const PreviewToggleStep& c = m_previewToggleSteps[j];
                if (!c.open && c.frame < o.frame) closedBefore |= c.surfaces;
            }
            const std::uint8_t extra =
                static_cast<std::uint8_t>(o.surfaces & ~closedBefore);
            if (extra)
            {
                LOG_ERROR("-preview_open_at: the step at editor frame %llu names "
                          "surface(s) 0x%X that no -preview_close_at before it "
                          "closes (open mask 0x%X, closed so far 0x%X). A reopen "
                          "restores what a close took away; for a surface "
                          "nothing closed there is no state to restore, and the "
                          "restore fields read as 'was not open' - so the step "
                          "would CLOSE that panel and report a reopen.",
                          static_cast<unsigned long long>(o.frame),
                          static_cast<unsigned>(extra),
                          static_cast<unsigned>(o.surfaces),
                          static_cast<unsigned>(closedBefore));
                return false;
            }
        }

    LOG_INFO("[PREVIEW] harness preview %s scheduled at %zu editor frame(s) "
             "(%u step(s) total)", open ? "OPEN" : "CLOSE", steps.size(),
             m_previewToggleCount);
    return true;
}

void EditorLayer::TickHarnessPreview()
{
    ++m_harnessFrameTick;

    // ---- The collision-baker preview load / restore one-shot ----
    // Writes the same two intents the popup's buttons write and lets ProcessPendingBakePreview do the
    // work at its own frame boundary, so the pack exercises the shipped path. The point of interest
    // is the restore's tail: WaitIdleAndReleaseDeferred, MeshLibrary::Release and
    // Renderer::OnWorldClear(idsPreserved=false, worldSurvives=true), the only caller of the branch
    // that must not wipe the PiP / camera-view state.
    if (m_bakeRestoreFrame != 0 && !m_bakeRestoreDone)
    {
        const std::uint64_t loadAt    = m_bakeRestoreFrame;
        const std::uint64_t restoreAt = loadAt + kBakeRestoreGap;
        const std::uint64_t verifyAt  = restoreAt + kBakeRestoreVerify;
        if (m_harnessFrameTick == loadAt)
        {
            // Empty map: no entity's MeshLibRef is swapped. The bake's job is the swap; this pack's is the
            // restore's teardown, which runs identically with zero backups (the restore loop reports 0).
            m_pendingBakePreviewMap.clear();
            m_pendingBakePreviewPath = m_bakeRestoreLibPath;
            m_pendingBakePreviewOp   = PendingBakePreviewOp::Load;
            LOG_INFO("[PREVIEW] bake restore: LOAD queued at editor frame %llu",
                     static_cast<unsigned long long>(m_harnessFrameTick));
        }
        else if (m_harnessFrameTick == restoreAt)
        {
            m_bakeRestoreBaseline = m_renderer
                ? m_renderer->GetPreviewViewsRenderedTotal() : 0ull;
            m_pendingBakePreviewOp = PendingBakePreviewOp::Restore;
            LOG_INFO("[PREVIEW] bake restore: RESTORE queued at editor frame "
                     "%llu (renders so far %llu)",
                     static_cast<unsigned long long>(m_harnessFrameTick),
                     static_cast<unsigned long long>(m_bakeRestoreBaseline));
        }
        else if (m_harnessFrameTick == verifyAt)
        {
            m_bakeRestoreDone = true;
            // The whole claim as four numbers: `pip` is the entity the PiP still points at (0 = the restore
            // cancelled it: the regression), `renders` counts preview renders after the restore, and
            // `draws` / `candidates` are the WorldCull accounting of the newest one (a surviving view that
            // culls everything draws 0 and is just as broken).
            const unsigned pip = m_renderer
                ? static_cast<unsigned>(m_renderer->GetGamePiPEntity()) : 0u;
            const unsigned long long now = m_renderer
                ? m_renderer->GetPreviewViewsRenderedTotal() : 0ull;
            LOG_INFO("[PREVIEW] bake restore verify: pip=%u renders=%llu "
                     "draws=%u candidates=%u frame=%llu",
                     pip,
                     now - static_cast<unsigned long long>(m_bakeRestoreBaseline),
                     m_renderer ? m_renderer->GetPreviewWorldCullDraws() : 0u,
                     m_renderer ? m_renderer->GetPreviewWorldCullCandidates() : 0u,
                     static_cast<unsigned long long>(m_harnessFrameTick));
        }
    }

    // ---- The panel close / reopen schedule ----
    // First in this function, so a close and the -preview_fixture / edit-at one-shots below never act
    // on the same panel in one frame with no defined winner.
    for (std::uint32_t i = 0; i < m_previewToggleCount; ++i)
    {
        PreviewToggleStep& s = m_previewToggleSteps[i];
        if (s.done || m_harnessFrameTick < s.frame) continue;
        s.done = true;
        // Per surface, both ways: the restore state is written and read through the same mask, so a
        // :material close then :material open touches only the inspector, and an unqualified step names
        // all four.
        const std::uint8_t m = s.surfaces;
        if (!s.open)
        {
            // Remember what was open among the surfaces this step names, so the reopen restores rather than
            // assumes, and a step cannot overwrite what an earlier step remembered of a surface it does not
            // name. A second close of an already closed surface does not re-record it: the restore state
            // belongs to the close that took the surface away (the one that set the pending bit), or the
            // matching open would restore `false` and the panel would never come back.
            const std::uint8_t record =
                static_cast<std::uint8_t>(m & ~m_previewClosedPending);
            if (m & kPreviewSurfaceMaterial)
            {
                if (record & kPreviewSurfaceMaterial)
                    m_previewReopenMatPath = m_assetMatPath;
                if (!m_assetMatPath.empty())
                {
                    m_assetMatPath.clear();
                    m_assetMatEdit.reset();
                    m_assetMatDirty = false;
                    Editor::MaterialPreviewWidget::Get().NotifyClosed();
                }
            }
            if (m & kPreviewSurfaceCamera)
            {
                if (record & kPreviewSurfaceCamera)
                    m_previewReopenCamera = m_showCameraPreview;
                m_showCameraPreview   = false;
            }
            if (m & kPreviewSurfaceAsset)
            {
                if (record & kPreviewSurfaceAsset)
                    m_previewReopenAsset = m_showAssetPreview;
                m_showAssetPreview   = false;
            }
            if (m & kPreviewSurfaceSplit)
            {
                if (record & kPreviewSurfaceSplit)
                    m_previewReopenSplit = m_showSplitViewport;
                m_showSplitViewport  = false;
            }
            // This close now owns these surfaces: only a surface whose bit is set here is restored by a
            // later open (see the member).
            m_previewClosedPending |= m;
            // The literal up to the frame number is fixed: preview_teardown asserts "... closed the preview
            // panels at editor frame 150" verbatim, and which= rides inside the parenthetical.
            LOG_INFO("[PREVIEW] harness closed the preview panels at editor "
                     "frame %llu (which=%u material=%d camera=%d asset=%d "
                     "split=%d) - their views are handed back at the next frame "
                     "boundary",
                     static_cast<unsigned long long>(m_harnessFrameTick),
                     static_cast<unsigned>(m),
                     ((m & kPreviewSurfaceMaterial) &&
                      !m_previewReopenMatPath.empty()) ? 1 : 0,
                     ((m & kPreviewSurfaceCamera) && m_previewReopenCamera) ? 1 : 0,
                     ((m & kPreviewSurfaceAsset)  && m_previewReopenAsset)  ? 1 : 0,
                     ((m & kPreviewSurfaceSplit)  && m_previewReopenSplit)  ? 1 : 0);
        }
        else
        {
            // Restore only what a close is holding: `m` is what this step names, m_previewClosedPending what
            // a close took away. Writing the restore state for a surface outside that intersection would
            // close a panel (the restore fields default to false / empty) while reporting a reopen.
            const std::uint8_t r = static_cast<std::uint8_t>(m & m_previewClosedPending);
            if ((r & kPreviewSurfaceMaterial) && !m_previewReopenMatPath.empty())
                OpenMaterialAsset(m_previewReopenMatPath);
            if (r & kPreviewSurfaceCamera) m_showCameraPreview = m_previewReopenCamera;
            if (r & kPreviewSurfaceAsset)  m_showAssetPreview  = m_previewReopenAsset;
            if (r & kPreviewSurfaceSplit)  m_showSplitViewport = m_previewReopenSplit;
            m_previewClosedPending = static_cast<std::uint8_t>(
                m_previewClosedPending & ~r);
            // `restored=` is the field the pack reads, inside the parenthetical so the prefix
            // preview_teardown asserts is untouched. restored < which means this open named a surface no
            // close was holding: legal, since the parse guard only forbids a mask outside the whole run's
            // closes.
            LOG_INFO("[PREVIEW] harness reopened the preview panels at editor "
                     "frame %llu (which=%u restored=%u)",
                     static_cast<unsigned long long>(m_harnessFrameTick),
                     static_cast<unsigned>(m), static_cast<unsigned>(r));
        }
    }

    // ---- -preview_split_cams, once the base camera is alive ----
    // Game half, serial window (ProcessPendingActions), where World mutation is legal. Retried every
    // frame because the scene loads asynchronously: a spawn without the base entity would put the
    // cameras at the world origin and produce plausible pictures of nowhere. Not marked dirty (that
    // arms MaybeAutosave, which writes a rotating .iscene into the repo).
    // Re-armed on a World::Epoch() change, the poll CameraPreviewWindow uses to re-resolve its camera
    // by name (Entity has no generation: an id means something only inside one epoch). An epoch bump
    // does not mean the cameras are gone: World::AdoptState (the Play -> Stop restore) brings ids and
    // names back from the snapshot and bumps the epoch with the SplitCam* entities alive. So the
    // previous tenancy goes back through HarnessReleaseSplitCameras, whose guards (IsAlive, then a
    // name still starting with SplitCam) reject every id after an ids-destroyed boundary and pass
    // after an ids-preserved one. Never DestroySubtree a recycled id: it would take out whatever owns
    // it now.
    if (m_world)
    {
        const std::uint32_t epoch = m_world->Epoch();
        if (m_splitCamEpochSeen && epoch != m_splitCamWorldEpoch &&
            !m_splitCamBase.empty())
        {
            const std::size_t held = m_splitCamEntities.size();
            // Counted before the call, so the ledger fields on the line it emits include this re-arm;
            // `rearmReleased` cannot, being its return value. The packs read the last matching line
            // (shutdown's), where both are final.
            ++m_splitCamRearms;
            const std::uint32_t took = HarnessReleaseSplitCameras("epoch");
            m_splitCamRearmReleased += took;
            LOG_INFO("[PREVIEW] split: world epoch %u -> %u, re-arming the "
                     "%u fixture camera(s) (%zu tracked id(s); %u still alive "
                     "and named SplitCam* and were destroyed - an ids-preserved "
                     "boundary such as Play/Stop, where a plain drop would "
                     "orphan them)",
                     m_splitCamWorldEpoch, epoch, m_splitCamCount, held, took);
        }
        m_splitCamWorldEpoch = epoch;
        m_splitCamEpochSeen  = true;
    }

    if (!m_splitCamBase.empty() && !m_splitCamSpawned && m_world)
    {
        // By name, over the pool that carries what is needed (a world transform): World has no name
        // index, and CameraPreviewWindow resolves its harness name the same way.
        Entity base = NullEntity;
        DirectX::XMFLOAT3 pos{ 0.f, 2.f, 0.f };
        m_world->ForEach<GlobalTransform>([&](Entity e, GlobalTransform& gt)
        {
            if (base != NullEntity) return;
            if (m_world->GetName(e) != m_splitCamBase) return;
            base = e;
            const DirectX::XMMATRIX m = DirectX::XMLoadFloat4x4(&gt.matrix);
            DirectX::XMStoreFloat3(&pos, m.r[3]);
        });
        if (base != NullEntity)
        {
            m_splitCamSpawned = true;
            m_splitCamEntities.clear();
            for (std::uint32_t i = 0; i < m_splitCamCount; ++i)
            {
                const Entity e = m_world->CreateEntity();
                if (e == NullEntity) break;
                // Tracked, so shutdown and the epoch poll above can take them back; an id means something only
                // inside this world epoch, which the release's guards check.
                m_splitCamEntities.push_back(e);
                m_world->SetName(e, "SplitCam" + std::to_string(i));
                LocalTransform lt;
                lt.translation = pos;
                // Yaw only, 360/n apart (four cameras at 0/90/180/270, three at 0/120/240). Cameras at one point
                // with distinct yaws have frusta that share only their apex, so every cell but the first sees
                // geometry the main camera cannot, which the per-view `secondary` counts measure.
                const float yaw = DirectX::XM_2PI *
                                  (static_cast<float>(i) /
                                   static_cast<float>(m_splitCamCount));
                DirectX::XMStoreFloat4(&lt.rotation,
                    DirectX::XMQuaternionRotationRollPitchYaw(0.f, yaw, 0.f));
                m_world->AddComponent<LocalTransform>(e, lt);
                m_world->AddComponent<GlobalTransform>(e, {});
                CameraComponent cc;
                cc.fov   = 1.0472f;   // 60 deg, AIBench's own lens
                cc.nearZ = 0.1f;
                cc.farZ  = 1500.f;
                m_world->AddComponent<CameraComponent>(e, cc);
            }
            // The session ledger's spawn half: one per tenancy, not per entity, so a pack can assert
            // "spawns == rearms + 1".
            ++m_splitCamSpawnEvents;
            LOG_INFO("[PREVIEW] split: spawned %u camera(s) at (%.2f, %.2f, %.2f) "
                     "from '%s' (spawns=%u rearms=%u)",
                     m_splitCamCount, pos.x, pos.y, pos.z,
                     m_splitCamBase.c_str(), m_splitCamSpawnEvents,
                     m_splitCamRearms);
        }
        else if (++m_splitCamTries == 180)
        {
            // A base name that never resolves leaves every cell empty, and the run would measure the no-view
            // case under the split pack's name.
            ENGINE_CHECK_ONCE(false,
                "-preview_split_cams: '%s' never resolved to a live entity in "
                "180 editor frames; no split cameras were spawned and the panel "
                "has nothing to show.", m_splitCamBase.c_str());
        }
    }

    // ---- -preview_asset_verify, once, after the world has settled ----
    // At frame 60, not at parse time: it needs a live World, a MeshLibrary and the game half. Opt-in
    // and run once: it spawns and destroys thousands of entities for a level-sized .iscn.
    if (!m_assetVerifyPaths.empty() && !m_assetVerifyDone &&
        m_harnessFrameTick >= 60 && m_renderer && m_world && m_assetMgr)
    {
        m_assetVerifyDone = true;
        bool allOk = true;
        for (const std::string& p : m_assetVerifyPaths)
            if (!m_renderer->VerifyPreviewAssetOrder(p, *m_world, *m_assetMgr))
                allOk = false;
        if (!allOk)
        {
            LOG_ERROR("-preview_asset_verify found a MISMATCH between what the "
                      "asset preview composes and what a spawn produces. The "
                      "preview would apply per-part material overrides to the "
                      "wrong parts (or place them wrongly) and still draw a "
                      "plausible picture; refusing to continue.");
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- The asset panel's never-resolved deadline, turned into an exit ----
    // The panel latches the expiry from Draw (render side); this is the game half, where posting the
    // quit is legal. Exit 12, as for -scene and -exec: the run could not be set up as asked.
    if (Editor::AssetPreviewWindow::Get().HarnessDeadlineExpired() &&
        !m_assetPreviewDeadlinePosted)
    {
        m_assetPreviewDeadlinePosted = true;
        PostQuitMessage(Harness::PreconditionFail);
    }
    // The same for the Mesh Editor: a subject that never opened, a shot whose view never settled, a
    // layer PNG that could not be written (each logged by the panel with its reason).
    {
        static bool s_meshEditorFailPosted = false;
        if (Editor::MeshEditorWindow::Get().HarnessFailed() && !s_meshEditorFailPosted)
        {
            s_meshEditorFailPosted = true;
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // The same for the Texture Viewer: a subject that never opened, a view spec it refused, a shot
    // that never settled.
    {
        static bool s_textureViewerFailPosted = false;
        if (Editor::TextureViewerWindow::Get().HarnessFailed() && !s_textureViewerFailPosted)
        {
            s_textureViewerFailPosted = true;
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // The same for the UI Editor: a canvas that never appeared, a view step it refused, a shot whose
    // picture never settled.
    {
        static bool s_uiEditorFailPosted = false;
        if (Editor::UIEditorWindow::Get().HarnessFailed() && !s_uiEditorFailPosted)
        {
            s_uiEditorFailPosted = true;
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // The same for the Material Editor: a subject that never opened, a step it refused.
    {
        static bool s_materialEditorFailPosted = false;
        if (Editor::MaterialEditorWindow::Get().HarnessFailed() && !s_materialEditorFailPosted)
        {
            s_materialEditorFailPosted = true;
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // The same for the Skeleton Editor: a subject that never opened, a bone the select step does
    // not name, a shot whose view never settled.
    {
        static bool s_skeletonEditorFailPosted = false;
        if (Editor::SkeletonEditorWindow::Get().HarnessFailed() && !s_skeletonEditorFailPosted)
        {
            s_skeletonEditorFailPosted = true;
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- -preview_churn ----
    // Rotates the browser through the folders found at arm time and forces every entry to count as
    // visible: under -hidden io.DisplaySize is 0x0 and every ImGui window has a degenerate rect, so the
    // grid's real test is never true. Everything else (ScanAssets, the snapshot / restore, the orphan
    // release, StreamAssetThumbnail, the eviction sweep) is the browser's own path.
    if (!m_thumbChurnFolders.empty() && m_thumbChurnCycle)
    {
        // Where in the rotation this frame falls. `pass` counts completed
        // rotations; `want` is the folder inside the current one. Both derived
        // from the frame tick rather than accumulated, so there is no state to
        // drift.
        const std::uint64_t intoPass = m_harnessFrameTick % m_thumbChurnCycle;
        const std::uint32_t pass     = static_cast<std::uint32_t>(
            m_harnessFrameTick / m_thumbChurnCycle);
        std::size_t want = 0;
        for (std::uint64_t acc = 0; want + 1 < m_thumbChurnFolders.size(); ++want)
        {
            acc += m_thumbChurnFolders[want].dwell;
            if (intoPass < acc) break;
        }

        // Past the last pass the churn stops dead: a further rotation's numbers would land in the final
        // summary line and loosen every bound the pack sets. Scoped to this block, not a return: the
        // fixture, parity and edit drivers below are independent of the churn.
        if (pass < m_thumbChurnPasses)
        {
        // ---- Pass boundary ----
        // Emits the finished pass's summary, re-baselines the counters and drops every resident tile, so
        // the next pass starts from the disk cache (every request goes through LookupFresh again): a
        // warm-cache measurement, not "the tiles were still on screen".
        if (pass != m_thumbChurnPass && pass < m_thumbChurnPasses)
        {
            if (m_gfx) BeginThumbnailPass(*m_gfx);
            for (AssetEntry& ae : m_assets)
                if (ae.type == AssetType::Material)
                    EvictAssetPreviewTile(ae, /*countIt=*/false);
            LOG_INFO("[PREVIEW] churn: pass %u of %u begins at frame %llu",
                     m_thumbChurnPass + 1, m_thumbChurnPasses,
                     static_cast<unsigned long long>(m_harnessFrameTick));
        }

        if (want != m_thumbChurnIdx || m_harnessFrameTick == 1)
        {
            m_thumbChurnIdx = want;
            m_currentDir    = m_thumbChurnFolders[want].path;
            m_needsRescan   = true;
            ScanAssets();
            LOG_INFO("[PREVIEW] churn: folder %zu/%zu '%s' (%u material(s)) at "
                     "frame %llu",
                     want + 1, m_thumbChurnFolders.size(), m_currentDir.c_str(),
                     m_thumbChurnFolders[want].materials,
                     static_cast<unsigned long long>(m_harnessFrameTick));
        }
        // A sliding window, not "everything visible": marking the whole folder visible every frame
        // refreshes lastVisibleTime on every entry, so the 8 s sweep could never fire and the pack would
        // measure zero evictions. A window rotating one entry per frame puts every tile through request ->
        // render -> off-screen -> evict -> re-request, and the re-request exercises the disk cache in the
        // same run.
        const float now = static_cast<float>(ImGui::GetTime());
        const std::size_t n = m_assets.size();
        if (n)
        {
            constexpr std::size_t kChurnWindow = 24;
            const std::size_t start = static_cast<std::size_t>(m_harnessFrameTick % n);
            for (std::size_t k = 0; k < kChurnWindow && k < n; ++k)
                StreamAssetThumbnail(static_cast<int>((start + k) % n),
                                     /*visible=*/true, now);
        }
        }   // pass < m_thumbChurnPasses
    }

    // ---- -preview_parity world, once ----
    // Deferred to this window: it clears the World, legal only where the editor's own New Scene does
    // it.
    if (!m_pendingParityFixture.empty())
    {
        // The refusal ProcessPendingNewScene makes: RequestStop is applied by the next
        // PlayModeController::BeginFrame, whose snapshot restore would land on top of the fixture world.
        // Keep the request queued; once mode == Edit it runs against the restored editor world. No parity
        // pack passes -play, so this guard has no coverage.
        if (m_playModeController && !m_playModeController->IsEditing())
            m_playModeController->RequestStop();
        else
        {
            const std::string fixture = m_pendingParityFixture;
            m_pendingParityFixture.clear();
            BuildParityWorld(fixture);
        }
    }

    // ---- -preview_lights_parity world, once ----
    // The same window and Play-mode refusal as the block above: this clears the World too.
    if (m_pendingLightsParity != 0)
    {
        if (m_playModeController && !m_playModeController->IsEditing())
            m_playModeController->RequestStop();
        else
        {
            const bool withLights = (m_pendingLightsParity == 2);
            m_pendingLightsParity = 0;
            BuildLightsParityWorld(withLights);
        }
    }

    // ---- -preview_fixture cycle ----
    if (!m_previewFixtures.empty() && m_harnessFrameTick >= m_previewFixtureStart)
    {
        const std::uint64_t local = m_harnessFrameTick - m_previewFixtureStart;
        const std::size_t   want  =
            static_cast<std::size_t>(local / m_previewFixtureEvery);
        if (want < m_previewFixtures.size())
        {
            if (want != m_previewFixtureIdx)
            {
                m_previewFixtureIdx  = want;
                m_previewFixtureShot = false;
                // The shadow alpha-test lever for this window, pushed before the material is installed so the
                // window's first render has it. Only when the flag was given: an empty schedule never touches the
                // renderer.
                if (!m_previewShadowAlpha.empty() && m_renderer)
                {
                    const std::size_t i = (want < m_previewShadowAlpha.size())
                                        ? want : m_previewShadowAlpha.size() - 1;
                    m_renderer->SetPreviewStudioShadowAlphaTest(
                        m_previewShadowAlpha[i] != 0);
                }
                // The studio shadow pass for this window, pushed the same way. It moves the widget's
                // kStudioShadow bit, so the next RequestPreview carries a different ViewDesc and the dirty-edge
                // regulator mints a new generation: the second window's capture is a new picture, not a copy of
                // the first.
                if (!m_previewShadowWin.empty())
                {
                    const std::size_t i = (want < m_previewShadowWin.size())
                                        ? want : m_previewShadowWin.size() - 1;
                    HarnessSetPreviewShadow(m_previewShadowWin[i] != 0);
                }
                InstallFixtureMaterial(m_previewFixtures[want]);
            }
            // At the end of the window, with retries. The captured picture was recorded on an earlier frame,
            // so it is a late render of this fixture (the window's opening frames show the outgoing view
            // being released and the runtime material key resolving). The capture, not the attempt, sets the
            // latch: a frame with nothing to capture leaves it alone and the next frame retries; only the
            // window's last frame reports a failure. kCaptureTail is small so the capture still lands late in
            // the window, long after the material settles.
            constexpr std::uint32_t kCaptureTail = 4;
            const std::uint64_t inWindow = local % m_previewFixtureEvery;
            const std::uint32_t tail =
                (m_previewFixtureEvery > kCaptureTail) ? kCaptureTail : 1u;
            if (!m_previewFixtureShot && inWindow + tail >= m_previewFixtureEvery)
            {
                const bool lastChance = (inWindow + 1 >= m_previewFixtureEvery);
                // Which occurrence of this name this window is, counted over the schedule rather than kept as a
                // counter, so a retry in the same window cannot advance it (and write a third file).
                std::size_t occurrence = 0;
                for (std::size_t i = 0; i <= want; ++i)
                    if (m_previewFixtures[i] == m_previewFixtures[want])
                        ++occurrence;
                m_previewFixtureShot =
                    ServiceFixtureShot(m_previewFixtures[want], lastChance,
                                       occurrence);
            }
        }
    }

    // ---- -preview_asset_edit seeding ----
    // Write the "before" material before the panel resolves anything, so the pre-edit picture belongs
    // to this run, not to whatever the fixture file held: otherwise a second run opens on the
    // material the first one saved and the imagePair delta collapses to zero with nothing failing.
    if (!m_previewAssetEditMatPath.empty() && !m_previewAssetEditSeeded &&
        m_harnessFrameTick >= 2)
    {
        m_previewAssetEditSeeded = true;
        MaterialComponent before{};
        before.baseColor    = { 0.22f, 0.24f, 0.30f, 1.0f };   // dull slate
        before.roughnessMin = before.roughnessMax = 0.90f;
        before.metalnessMin = before.metalnessMax = 0.0f;
        if (Resource::SaveMaterial(before, m_previewAssetEditMatPath))
            LOG_INFO("[PREVIEW] harness asset edit: seeded '%s' with the BEFORE "
                     "material (baseColor 0.22,0.24,0.30 roughness 0.90)",
                     m_previewAssetEditMatPath.c_str());
        else
            LOG_ERROR("[PREVIEW] harness asset edit: could not write the BEFORE "
                      "material to '%s'; the pack's before/after pair is not a "
                      "measurement", m_previewAssetEditMatPath.c_str());
    }

    // ---- -preview_edit_at one-shot ----
    if (m_previewEditAtFrame != 0 && m_renderer)
    {
        // ---- The asset route ----
        // An open asset preview has no inspector buffer to mutate: its parts hold resolved
        // MaterialComponent values composed from the files, so the edit that reaches it is a save, as
        // the editor's Save button does. The extract folds MaterialContentGeneration into its stamp, which
        // is what makes the panel redraw.
        if (!m_previewAssetEditMatPath.empty())
        {
            if (!m_previewEditArmed && m_harnessFrameTick == m_previewEditAtFrame)
            {
                if (Sys::HasArtifactDir())
                {
                    if (CapturePreviewPNG("preview_pre_edit"))
                        LOG_INFO("[PREVIEW] harness asset edit: pre-edit picture "
                                 "-> preview_pre_edit.png");
                    else
                        LOG_ERROR("[PREVIEW] harness edit: no preview render to "
                                  "capture before the edit; the pack's "
                                  "before/after pair will fail.");
                }
                MaterialComponent after{};
                after.baseColor    = { 0.95f, 0.16f, 0.06f, 1.0f };   // hot red
                after.roughnessMin = after.roughnessMax = 0.06f;
                after.metalnessMin = after.metalnessMax = 1.0f;
                after.emissiveColor = { 1.0f, 0.35f, 0.05f, 2.0f };
                m_previewEditBaseline = m_renderer->GetPreviewViewsRenderedTotal();
                const bool ok = Resource::SaveMaterial(after, m_previewAssetEditMatPath);
                m_previewEditArmed = ok;
                // The second half of what the Save button does: SaveMaterial is only the write; the editor's Save
                // also invalidates everything built from the file, including the browser tiles a pack checks.
                if (ok) InvalidateMaterialThumbnails(m_previewAssetEditMatPath);
                if (ok)
                    LOG_INFO("[PREVIEW] harness asset edit at editor frame %llu: "
                             "saved '%s' (renders so far %llu)",
                             static_cast<unsigned long long>(m_harnessFrameTick),
                             m_previewAssetEditMatPath.c_str(),
                             static_cast<unsigned long long>(m_previewEditBaseline));
                else
                    LOG_ERROR("[PREVIEW] harness asset edit: SaveMaterial('%s') "
                              "failed; nothing was edited",
                              m_previewAssetEditMatPath.c_str());
            }
            else if (m_previewEditArmed)
            {
                const std::uint64_t now = m_renderer->GetPreviewViewsRenderedTotal();
                if (now > m_previewEditBaseline)
                {
                    m_previewEditArmed = false;
                    const std::uint64_t renderedAt = m_harnessFrameTick - 1;
                    const std::uint64_t lag = renderedAt - m_previewEditAtFrame;
                    // Two frames on this route, structurally: the save happens at the top of a frame, the asset
                    // extract that folds MaterialContentGeneration into its stamp runs on the game half of the next
                    // frame, and the render it arms is recorded in that frame's Render. The material route measures
                    // from a buffer the widget reads within the same frame.
                    LOG_INFO("[PREVIEW] harness asset edit re-rendered during "
                             "editor frame %llu, %llu frame(s) after the edit",
                             static_cast<unsigned long long>(renderedAt),
                             static_cast<unsigned long long>(lag));
                    ENGINE_CHECK(lag <= 2,
                        "asset part material save took %llu frames to reach a "
                        "preview render; the extract folds "
                        "Resource::MaterialContentGeneration into its E12 stamp, "
                        "so the budget is the game half of the next frame plus "
                        "its render.",
                        static_cast<unsigned long long>(lag));
                }
            }
            return;
        }

        if (!m_previewEditArmed && m_harnessFrameTick == m_previewEditAtFrame &&
            m_assetMatEdit)
        {
            // ---- The before picture ----
            // <artifacts>/preview_pre_edit.png, named by convention off -preview_edit_at (one edit per run, so
            // one before-picture). Without it the only image gate is a luma band on the post-edit PNG, which
            // the pre-edit picture also fits, so a re-render on stale parameters would pass; the pack pairs
            // the two files with imagePair.meanAbsMin and fails if they are too similar. Captured before the
            // mutation, at the fixture shots' frame boundary: the last render of the material as opened.
            if (Sys::HasArtifactDir())
            {
                if (CapturePreviewPNG("preview_pre_edit"))
                    LOG_INFO("[PREVIEW] harness edit: pre-edit picture -> "
                             "preview_pre_edit.png");
                else
                    LOG_ERROR("[PREVIEW] harness edit: no preview render to "
                              "capture before the edit; the pack's before/after "
                              "pair will fail.");
            }

            // Exactly what dragging the Inspector's Roughness slider does: change the value and call
            // SetEditDirty (DIRTY | ASSET_DIRTY | GPU_LINEAR_CACHE_DIRTY). The preview reacts to
            // HashMatParams changing (SetDirty is cleared by the renderer every frame and IsAssetDirty is a
            // level, not an edge), so the assertion below is on a re-render, not on a flag.
            MaterialComponent& mat = *m_assetMatEdit;
            const float before = mat.roughnessMax;
            mat.roughnessMax = (before > 0.5f) ? 0.15f : 0.95f;
            mat.roughnessMin = mat.roughnessMax;
            mat.SetEditDirty();
            m_assetMatDirty = true;
            m_previewEditBaseline = m_renderer->GetPreviewViewsRenderedTotal();
            m_previewEditArmed    = true;
            LOG_INFO("[PREVIEW] harness edit at editor frame %llu: roughness "
                     "%.3f -> %.3f (renders so far %llu)",
                     static_cast<unsigned long long>(m_harnessFrameTick),
                     before, mat.roughnessMax,
                     static_cast<unsigned long long>(m_previewEditBaseline));
        }
        else if (m_previewEditArmed)
        {
            const std::uint64_t now = m_renderer->GetPreviewViewsRenderedTotal();
            if (now > m_previewEditBaseline)
            {
                m_previewEditArmed = false;
                // Minus one: this runs at the top of a frame (ProcessPendingActions) and the render it detects
                // happened during the previous frame's Render, so the first tick that sees the counter move is
                // one past the frame that moved it. The raw difference would report two frames for a one-frame
                // path.
                const std::uint64_t renderedAt = m_harnessFrameTick - 1;
                const std::uint64_t lag = renderedAt - m_previewEditAtFrame;
                // The budget is one frame, not zero: the widget consumes the edit in OnUIRender, after this
                // frame's Render, so the earliest new picture is next frame. The pack asserts the number.
                LOG_INFO("[PREVIEW] harness edit re-rendered during editor frame "
                         "%llu, %llu frame(s) after the edit",
                         static_cast<unsigned long long>(renderedAt),
                         static_cast<unsigned long long>(lag));
                ENGINE_CHECK(lag <= 1,
                    "material edit took %llu frames to reach a preview render; "
                    "sec.2.7 budgets exactly one (the widget consumes the edit "
                    "in OnUIRender, which runs AFTER this frame's Render, so "
                    "the new picture is recorded on the next frame).",
                    static_cast<unsigned long long>(lag));
            }
        }
    }
}

// -chainpreview_stage_edit_at probe: drives DrawComponentWithUndo (the Play LIVE section's
// scene-branch path) on the preview stage's ChainPhysicsComponent. Call 1's body moves `damping`
// and makes a widget active (a gesture begins), call 2's body releases it (it ends and commits):
// the active-id bracket of a slider drag, within one frame so no stale-bracket sweep interferes.
// The scene selection is two of the scene's own chain carriers; the log line says whether their
// components changed, how far the undo stack moved and whether the scene got dirty, and
// stageChanged proves the edit happened.
void EditorLayer::TickChainStageEditProbe()
{
    static const int s_at =
        Sys::CommandLine::Get().GetInt("-chainpreview_stage_edit_at", 0);
    static bool s_done = false;
    if (s_at <= 0 || s_done || !m_world) return;
    auto& panel = Editor::ChainPhysicsPreviewWindow::Get();
    const std::uint64_t tick = panel.EditorFrame();
    if (tick < static_cast<std::uint64_t>(s_at)) return;
    const Entity stage = panel.StageChainEntity();
    if (stage == NullEntity || !m_world->IsAlive(stage) ||
        !m_world->HasComponent<ChainPhysicsComponent>(stage))
    {
        // A probe that never runs must not pass as "nothing moved".
        if (tick >= static_cast<std::uint64_t>(s_at) + 240u)
        {
            s_done = true;
            LOG_ERROR("[CHAINPREVIEW] -chainpreview_stage_edit_at=%d: no stage "
                      "chain carrier in 240 editor frames. Refusing to run.", s_at);
            PostQuitMessage(Harness::PreconditionFail);
        }
        return;
    }
    s_done = true;

    std::vector<Entity> picks;
    if (auto* cp = m_world->GetPool<ChainPhysicsComponent>())
        for (Entity e : cp->Entities())
        {
            if (!m_world->IsAlive(e) || m_world->HasComponent<PreviewStageTag>(e))
                continue;
            picks.push_back(e);
            if (picks.size() == 2) break;
        }
    if (picks.size() < 2)
    {
        LOG_ERROR("[CHAINPREVIEW] -chainpreview_stage_edit_at needs a scene with at "
                  "least two ChainPhysicsComponent carriers of its own (found %zu). "
                  "Refusing to run - with fewer there is nothing to propagate to.",
                  picks.size());
        PostQuitMessage(Harness::PreconditionFail);
        return;
    }
    ClearEntitySelection();
    SelectEntity(picks[0], SelectMode::Replace);
    SelectEntity(picks[1], SelectMode::Add);

    ComponentSerializerRegistry& reg = GetComponentRegistry();
    const std::type_index type(typeid(ChainPhysicsComponent));
    auto blob = [&](Entity e)
    { return Editor::Undo::SerializeComponentBlob(reg, *m_world, e, type); };
    const std::string a0 = blob(picks[0]), b0 = blob(picks[1]), s0 = blob(stage);
    const std::size_t undo0  = m_editHistory.UndoCount();
    const bool        dirty0 = m_sceneDirty;

    Editor::Undo::CaptureFn                    capture;
    std::function<std::string(World&, Entity)> fingerprint;
    if (const auto it = m_componentEditors.find(type); it != m_componentEditors.end())
    {
        capture     = it->second.captureValue;
        fingerprint = it->second.fingerprint;
    }
    const float cur = m_world->GetComponent<ChainPhysicsComponent>(stage)->damping;
    const float next = (cur > 0.5f) ? cur - 0.25f : cur + 0.25f;
    const ImGuiID fakeId = ImGui::GetID("##cpv_stage_edit_probe");
    ImGuiWindow*  win    = ImGui::GetCurrentWindow();
    DrawComponentWithUndo(stage, type, capture, fingerprint, [&]()
    {
        if (ChainPhysicsComponent* c = m_world->GetComponent<ChainPhysicsComponent>(stage))
            c->damping = next;
        ImGui::SetActiveID(fakeId, win);     // the gesture begins
    });
    DrawComponentWithUndo(stage, type, capture, fingerprint, [&]()
    {
        ImGui::ClearActiveID();              // ... and ends: commit
    });

    const bool selChanged   = blob(picks[0]) != a0 || blob(picks[1]) != b0;
    const bool stageChanged = blob(stage) != s0;
    const long long undoDelta = static_cast<long long>(m_editHistory.UndoCount()) -
                                static_cast<long long>(undo0);
    LOG_INFO("[CHAINPREVIEW] stage edit probe: selected=%zu stage=%u selA=%u selB=%u "
             "stageChanged=%d selectionChanged=%d undoDelta=%lld sceneDirtied=%d "
             "dirtyBefore=%d damping=%.4f gate=%d",
             m_selectedEntities.size(), static_cast<unsigned>(stage),
             static_cast<unsigned>(picks[0]), static_cast<unsigned>(picks[1]),
             stageChanged ? 1 : 0, selChanged ? 1 : 0, undoDelta,
             (!dirty0 && m_sceneDirty) ? 1 : 0, dirty0 ? 1 : 0, next,
             Sys::CommandLine::Get().Has("-chainpreview_edit_gate_off") ? 0 : 1);
}

// -chainpreview_panel_tree_at probe: the path a user takes. The panel's Parameters section draws
// the chain tree on m_staged (HarnessDrawStagedTree, the same DrawStagedTree call DrawStagedParams
// makes, with StagedHooks), the panel's one writer syncs it into the live component, and Ctrl+Z
// routes to the panel's history through its focus frame (m_uiFocusFrame). UI frames:
//   +0  the tree runs in a focused probe window; the wrapped functor moves damping on the
//       component the panel handed it and opens a synthetic active-id bracket (a slider
//       mid-drag). ImGui drops the unsubmitted id at a later NewFrame, which is the release: the
//       panel commits one undo step then.
//   +4  read-back: staged moved? live == staged? global undo / scene dirty? Then the tree again,
//       focused, no edit (the focus frame Ctrl+Z reads).
//   +5  the Ctrl+Z route: OwnsUndoShortcut() -> RequestUndo().
//   +7  read-back after the panel applied the undo; one line.
// Reverse arm -chainpreview_tree_live: the tree on the stage's LIVE component with a direct
// Invalidate; m_staged never moves.
void EditorLayer::TickChainPanelTreeProbe()
{
    static const int s_at =
        Sys::CommandLine::Get().GetInt("-chainpreview_panel_tree_at", 0);
    static int   s_phase = 0;
    static int   s_uiFrame = 0;
    static float s_before = 0.f, s_next = 0.f, s_stagedAfterEdit = 0.f, s_liveAfterEdit = 0.f;
    static std::size_t   s_undo0 = 0;
    static bool          s_dirty0 = false, s_routed = false, s_liveEqStaged = false;
    static std::uint64_t s_commits0 = 0, s_undoOps0 = 0, s_commitsAfterEdit = 0;
    if (s_at <= 0 || s_phase < 0 || !m_world || !m_renderer) return;
    auto& panel = Editor::ChainPhysicsPreviewWindow::Get();
    const bool liveArm = Editor::ChainPhysicsPreviewWindow::TreeLiveArm();
    const Entity stage = panel.StageChainEntity();
    auto live = [&]() -> ChainPhysicsComponent*
    {
        return (stage != NullEntity && m_world->IsAlive(stage))
            ? m_world->GetComponent<ChainPhysicsComponent>(stage) : nullptr;
    };
    const SkeletonComponent* skc = (stage != NullEntity && m_world->IsAlive(stage))
        ? m_world->GetComponent<SkeletonComponent>(stage) : nullptr;
    const bool haveSkel = skc && skc->assetIndex < m_renderer->GetSkeletonRegistry().Count();
    // The tree functor the panel hands its m_staged to (DrawChainParamTree -
    // the same functor OnUIRender passes to Draw), optionally followed by the
    // probe's edit on the component the PANEL passed in.
    auto treeFn = [this](const std::function<void(ChainPhysicsComponent&)>& edit) -> ChainParamTreeFn
    {
        return [this, edit](ChainPhysicsComponent& c, const SkeletonAsset& s, const ChainParamHooks& h)
        {
            DrawChainParamTree(c, s, h);
            if (edit) edit(c);
        };
    };
    // The reverse arm's body: the stage's LIVE component, a direct Invalidate.
    auto liveTree = [&](const std::function<void(ChainPhysicsComponent&)>& edit)
    {
        ChainPhysicsComponent* l = live();
        if (!l || !haveSkel) return;
        ChainPhysicsSystem* cps = m_renderer->GetChainPhysicsSystem();
        ChainParamHooks hooks;
        hooks.entity     = stage;
        hooks.invalidate = [cps, stage]() { if (cps) cps->Invalidate(stage); };
        DrawChainParamTree(*l, m_renderer->GetSkeletonRegistry().Get(skc->assetIndex), hooks);
        if (edit) edit(*l);
    };
    if (s_phase == 0)
    {
        if (panel.EditorFrame() < static_cast<std::uint64_t>(s_at)) return;
        if (!live() || !haveSkel || !panel.EditStagedFor(stage))
        {
            if (panel.EditorFrame() >= static_cast<std::uint64_t>(s_at) + 240u)
            {
                s_phase = -1;
                LOG_ERROR("[CHAINPREVIEW] -chainpreview_panel_tree_at=%d: no stage chain "
                          "carrier with a staged copy in 240 editor frames. Refusing to run.", s_at);
                PostQuitMessage(Harness::PreconditionFail);
            }
            return;
        }
        s_before   = panel.EditStagedFor(stage)->damping;
        s_next     = (s_before > 0.5f) ? s_before - 0.25f : s_before + 0.25f;
        s_undo0    = m_editHistory.UndoCount();
        s_dirty0   = m_sceneDirty;
        s_commits0 = panel.PanelCommits();
        s_undoOps0 = panel.PanelUndoOps();
        // A focused window, so the panel's own focus frame is what Ctrl+Z is
        // routed by (Draw records it the same way).
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(320.f, 200.f), ImGuiCond_Always);
        if (ImGui::Begin("##cpv_panel_tree_probe", nullptr, ImGuiWindowFlags_NoSavedSettings))
        {
            const ImGuiID fakeId = ImGui::GetID("##cpv_panel_tree_probe_slider");
            ImGuiWindow*  win    = ImGui::GetCurrentWindow();
            auto edit = [&](ChainPhysicsComponent& c)
            {
                c.damping = s_next;               // the slider moved ...
                ImGui::SetActiveID(fakeId, win);  // ... and is still held
            };
            if (liveArm) liveTree(edit);
            else panel.HarnessDrawStagedTree(m_renderer, treeFn(edit),
                                             ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
        }
        ImGui::End();
        s_phase = 1;
        s_uiFrame = ImGui::GetFrameCount();
        return;
    }
    const int dt = ImGui::GetFrameCount() - s_uiFrame;
    // ImGui clears the unsubmitted active id two NewFrames later and the panel commits in the
    // ProcessPendingActions after that: read back at +4.
    if (s_phase == 1 && dt >= 4)
    {
        const ChainPhysicsComponent* staged = panel.EditStagedFor(stage);
        const ChainPhysicsComponent* l = live();
        s_stagedAfterEdit  = staged ? staged->damping : -1.f;
        s_liveAfterEdit    = l ? l->damping : -1.f;
        s_liveEqStaged     = staged && l && l->AuthoredEquals(*staged);
        s_commitsAfterEdit = panel.PanelCommits();
        // The tree again, focused, without an edit: the panel's focus frame is
        // what Ctrl+Z reads at the top of the NEXT frame.
        if (haveSkel)
        {
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowSize(ImVec2(320.f, 200.f), ImGuiCond_Always);
            if (ImGui::Begin("##cpv_panel_tree_probe", nullptr, ImGuiWindowFlags_NoSavedSettings))
            {
                if (liveArm) liveTree({});
                else panel.HarnessDrawStagedTree(m_renderer, treeFn({}),
                                                 ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
            }
            ImGui::End();
        }
        s_phase = 2;
        s_uiFrame = ImGui::GetFrameCount();
        return;
    }
    if (s_phase == 2 && dt >= 1)
    {
        // The Ctrl+Z route (OnUIRender's shortcut handler, minus the key).
        s_routed = panel.OwnsUndoShortcut();
        if (s_routed) panel.RequestUndo();
        s_phase = 3;
        s_uiFrame = ImGui::GetFrameCount();
        return;
    }
    if (s_phase == 3 && dt >= 2)
    {
        s_phase = -1;
        const ChainPhysicsComponent* staged = panel.EditStagedFor(stage);
        const ChainPhysicsComponent* l = live();
        const long long undoDelta = static_cast<long long>(m_editHistory.UndoCount()) -
                                    static_cast<long long>(s_undo0);
        LOG_INFO("[CHAINPREVIEW] panel tree probe: stagedBefore=%.4f edit=%.4f "
                 "stagedAfterEdit=%.4f liveAfterEdit=%.4f stagedChanged=%d liveEqualsStaged=%d "
                 "panelCommits=%llu undoDelta=%lld sceneDirtied=%d routed=%d panelUndos=%llu "
                 "stagedAfterUndo=%.4f liveAfterUndo=%.4f restored=%d treeLive=%d",
                 s_before, s_next, s_stagedAfterEdit, s_liveAfterEdit,
                 s_stagedAfterEdit == s_next ? 1 : 0, s_liveEqStaged ? 1 : 0,
                 static_cast<unsigned long long>(s_commitsAfterEdit - s_commits0), undoDelta,
                 (!s_dirty0 && m_sceneDirty) ? 1 : 0, s_routed ? 1 : 0,
                 static_cast<unsigned long long>(panel.PanelUndoOps() - s_undoOps0),
                 staged ? staged->damping : -1.f, l ? l->damping : -1.f,
                 (staged && l && staged->damping == s_before && l->damping == s_before) ? 1 : 0,
                 liveArm ? 1 : 0);
    }
}

// -chainpreview_play_edit=<key>=<value>@<editor frame> probe: once in Play and at the frame, the
// panel's Play-mode Parameters body (DrawChainLiveEdit: target resolution, the live tree inside
// DrawComponentWithUndo) runs in a probe window and the probe edits the component it hands over
// (a -chainpreview_param key); with -chainpreview_play_keep it then presses "Keep these values"
// (KeepChainLiveValues). After Stop it reads the same scene entity back (revertedOnStop: whether
// Stop's AdoptState reverted the edit).
void EditorLayer::TickChainPlayEditProbe()
{
    static const std::string s_spec = Sys::CommandLine::Get().Get("-chainpreview_play_edit");
    static int           s_phase = 0;
    static std::string   s_key, s_value;
    static std::uint64_t s_at = 0;
    static EntityHandle  s_target = NullEntityHandle;
    static std::string   s_blob0, s_name;
    static int           s_stopFrame = -1;
    if (s_spec.empty() || s_phase < 0 || !m_world || !m_renderer) return;
    auto& panel = Editor::ChainPhysicsPreviewWindow::Get();
    ComponentSerializerRegistry& reg = GetComponentRegistry();
    const std::type_index type(typeid(ChainPhysicsComponent));
    if (s_phase == 0 && s_key.empty())
    {
        // Validated at boot (Editor::ChainPreviewHarness::ApplyBootFlags); a malformed spec never runs.
        const std::size_t atPos = s_spec.rfind('@');
        const std::string body = s_spec.substr(0, atPos);
        const std::size_t eq = body.find('=');
        s_at = atPos == std::string::npos ? 0 : std::strtoull(s_spec.c_str() + atPos + 1, nullptr, 10);
        if (eq == std::string::npos || s_at == 0 ||
            !Editor::ChainPhysicsPreviewWindow::HarnessParamValid(body.substr(0, eq), body.substr(eq + 1)))
        { s_phase = -1; return; }
        s_key   = body.substr(0, eq);
        s_value = body.substr(eq + 1);
    }
    const bool playing = GetPlayState() != ViewportPlayState::Stopped;
    if (s_phase == 0)
    {
        if (panel.EditorFrame() < s_at) return;
        const char* why = "";
        const Entity target = playing ? ResolveChainLiveTarget(&why) : NullEntity;
        if (!playing || target == NullEntity)
        {
            // A probe that never runs must not pass as "nothing moved".
            if (panel.EditorFrame() >= s_at + 240u)
            {
                s_phase = -1;
                LOG_ERROR("[CHAINPREVIEW] -chainpreview_play_edit: %s in 240 editor frames. "
                          "Refusing to run.", playing ? "no LIVE target" : "the editor never entered Play");
                PostQuitMessage(Harness::PreconditionFail);
            }
            return;
        }
        s_target = m_world->MakeHandle(target);
        s_name   = m_world->GetName(Editor::ChainPhysicsPreviewWindow::SceneRootOf(*m_world, target));
        s_blob0  = Editor::Undo::SerializeComponentBlob(reg, *m_world, target, type);
        const std::size_t undo0  = m_editHistory.UndoCount();
        const bool        dirty0 = m_sceneDirty;
        const bool keep = Sys::CommandLine::Get().Has("-chainpreview_play_keep");
        bool applied = false;
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(320.f, 200.f), ImGuiCond_Always);
        if (ImGui::Begin("##cpv_play_edit_probe", nullptr, ImGuiWindowFlags_NoSavedSettings))
            DrawChainLiveEdit([&](ChainPhysicsComponent& c)
            {
                applied = Editor::ChainPhysicsPreviewWindow::HarnessApplyParam(c, s_key, s_value);
            });
        ImGui::End();
        if (keep) KeepChainLiveValues(target);
        const std::string blob1 = Editor::Undo::SerializeComponentBlob(reg, *m_world, target, type);
        const ChainPhysicsComponent* c = m_world->GetComponent<ChainPhysicsComponent>(target);
        const long long undoDelta = static_cast<long long>(m_editHistory.UndoCount()) -
                                    static_cast<long long>(undo0);
        LOG_INFO("[CHAINPREVIEW] play edit probe: target=%u name='%s' why='%s' key=%s value=%s "
                 "applied=%d liveChanged=%d undoDelta=%lld sceneDirtied=%d kept=%d damping=%.4f "
                 "frame=%llu", static_cast<unsigned>(target), s_name.c_str(), why, s_key.c_str(),
                 s_value.c_str(), applied ? 1 : 0, blob1 != s_blob0 ? 1 : 0, undoDelta,
                 (!dirty0 && m_sceneDirty) ? 1 : 0, (keep && panel.HasPlayKeep()) ? 1 : 0,
                 c ? c->damping : -1.f, static_cast<unsigned long long>(panel.EditorFrame()));
        s_phase = 1;
        return;
    }
    if (s_phase == 1)
    {
        if (playing) return;
        // Stop's AdoptState ran; give the restored world a few UI frames.
        if (s_stopFrame < 0) { s_stopFrame = ImGui::GetFrameCount(); return; }
        if (ImGui::GetFrameCount() - s_stopFrame < 5) return;
        s_phase = -1;
        const bool valid = m_world->IsHandleValid(s_target) &&
                           m_world->HasComponent<ChainPhysicsComponent>(s_target.entity);
        const std::string blob = valid
            ? Editor::Undo::SerializeComponentBlob(reg, *m_world, s_target.entity, type) : std::string();
        const ChainPhysicsComponent* c = valid
            ? m_world->GetComponent<ChainPhysicsComponent>(s_target.entity) : nullptr;
        LOG_INFO("[CHAINPREVIEW] play edit after stop: target=%u name='%s' valid=%d "
                 "revertedOnStop=%d liveDamping=%.4f", static_cast<unsigned>(s_target.entity),
                 s_name.c_str(), valid ? 1 : 0, (valid && blob == s_blob0) ? 1 : 0,
                 c ? c->damping : -1.f);
    }
}

// -chainpreview_main_pose=<entity name>[#k]:<bone|#idx|*>@<editor frame> probe ('*' = the first
// pickable chain bone). Drives the state the Inspector row's "Pose rest bones in main viewport"
// arms for the selection: select the character's chain carrier, arm m_chainMainPose as the
// checkbox does, frame it, pick the bone; then reads back whether UpdateBonePoseEditTracking kept
// the arm and the bone and whether RenderBonePoseGizmo submitted the ring for that entity.
// ImGuizmo cannot be dragged without a mouse: the drag's write is covered by
// chain_preview_restpose_gizmo, which drives the same Editor::ChainRestPose code.
void EditorLayer::TickChainMainPoseProbe()
{
    static const std::string s_spec = Sys::CommandLine::Get().Get("-chainpreview_main_pose");
    static int           s_phase = 0;
    static std::string   s_ent, s_bone;
    static std::uint64_t s_at = 0, s_mark = 0, s_submits0 = 0;
    static EntityHandle  s_target = NullEntityHandle;
    static std::uint32_t s_boneIdx = ~0u;
    static int           s_pickable = 0;
    if (s_spec.empty() || s_phase < 0 || !m_world || !m_renderer) return;
    auto& panel = Editor::ChainPhysicsPreviewWindow::Get();
    const std::uint64_t frame = panel.EditorFrame();
    auto refuse = [&](const char* why)
    {
        s_phase = -1;
        LOG_ERROR("[CHAINPREVIEW] -chainpreview_main_pose '%s': %s. Refusing to run.",
                  s_spec.c_str(), why);
        PostQuitMessage(Harness::PreconditionFail);
    };
    if (s_phase == 0 && s_ent.empty())
    {
        // Validated at boot (Editor::ChainPreviewHarness::ApplyBootFlags); a malformed spec never runs.
        const std::size_t atPos = s_spec.rfind('@');
        const std::string body  = s_spec.substr(0, atPos);
        const std::size_t c     = body.find(':');
        s_at = atPos == std::string::npos ? 0 : std::strtoull(s_spec.c_str() + atPos + 1, nullptr, 10);
        if (s_at == 0 || c == std::string::npos || c == 0 || c + 1 >= body.size())
        { s_phase = -1; return; }
        s_ent  = body.substr(0, c);
        s_bone = Resource::AssetRefs::PercentDecode(body.substr(c + 1));
    }
    if (s_phase == 0)
    {
        if (frame < s_at || GetPlayState() != ViewportPlayState::Stopped) return;
        const Entity root = panel.HarnessFindNamed(*m_world, s_ent);
        const Entity carrier = root != NullEntity ? panel.SceneCarrierOf(*m_world, root) : NullEntity;
        if (carrier == NullEntity) { refuse("no such scene character with a chain carrier"); return; }
        ClearEntitySelection();
        SelectEntity(carrier, SelectMode::Replace);
        // What the Inspector row's "Pose rest bones in main viewport" does.
        m_chainMainPose.poseMode = true;
        m_chainOverlayEnabled    = true;
        FocusSelectedEntity();
        s_target = m_world->MakeHandle(carrier);
        s_mark   = frame;
        s_phase  = 1;
        return;
    }
    if (!m_world->IsHandleValid(s_target)) { refuse("the character died"); return; }
    const Entity e = s_target.entity;
    if (s_phase == 1)
    {
        // UpdateBonePoseEditTracking adopts the new owner first (and drops any
        // bone on an owner change) - pick only once it has.
        if (m_chainMainPose.owner != e)
        {
            if (frame > s_mark + 120u) refuse("the main pose state never adopted the selection");
            return;
        }
        const SkeletonComponent*     skc  = m_world->GetComponent<SkeletonComponent>(e);
        const ChainPhysicsComponent* comp = m_world->GetComponent<ChainPhysicsComponent>(e);
        SkeletonRegistry& reg = m_renderer->GetSkeletonRegistry();
        if (!skc || !comp || skc->assetIndex == kInvalidSkeletonIndex || skc->assetIndex >= reg.Count())
        { refuse("the carrier has no skeleton"); return; }
        const SkeletonAsset& sk = reg.Get(skc->assetIndex);
        std::vector<uint8_t> pickable;
        CollectChainBones(sk, *comp, m_chainMainPose.focusGroup, pickable);
        if (s_bone == "*")
        {
            // '*' = the first bone the viewport picker could hit
            for (std::uint32_t b = 0; b < sk.boneCount && b < pickable.size(); ++b)
                if (pickable[b]) { s_boneIdx = b; break; }
        }
        else if (s_bone.size() > 1 && s_bone[0] == '#')
        {
            const unsigned long v = std::strtoul(s_bone.c_str() + 1, nullptr, 10);
            if (v < sk.boneCount) s_boneIdx = static_cast<std::uint32_t>(v);
        }
        else
            for (std::uint32_t b = 0; b < sk.boneCount; ++b)
                if (s_bone == sk.boneNames[b]) { s_boneIdx = b; break; }
        if (s_boneIdx == ~0u) { refuse("no such bone on the character's skeleton"); return; }
        s_pickable = (s_boneIdx < pickable.size() && pickable[s_boneIdx]) ? 1 : 0;
        // What a click on that bone in the viewport does (PickChainBone's hit).
        m_chainMainPose.poseBone = s_boneIdx;
        s_submits0 = m_bonePoseGizmoSubmits;
        s_mark     = frame;
        s_phase    = 2;
        return;
    }
    if (s_phase == 2)
    {
        if (frame < s_mark + 30u) return;
        s_phase = -1;
        const SkeletonComponent* skc = m_world->GetComponent<SkeletonComponent>(e);
        SkeletonRegistry& reg = m_renderer->GetSkeletonRegistry();
        const char* boneName = (skc && skc->assetIndex < reg.Count() &&
                                s_boneIdx < reg.Get(skc->assetIndex).boneCount)
            ? reg.Get(skc->assetIndex).boneNames[s_boneIdx] : "?";
        LOG_INFO("[CHAINPREVIEW] main pose probe: entity=%u name='%s' bone=%u boneName='%s' "
                 "pickable=%d armed=%d ownerIsTarget=%d boneKept=%d selected=%d stage=%d "
                 "submits=%llu submitsForTarget=%d frame=%llu",
                 static_cast<unsigned>(e), s_ent.c_str(), s_boneIdx, boneName, s_pickable,
                 m_chainMainPose.poseMode ? 1 : 0, m_chainMainPose.owner == e ? 1 : 0,
                 m_chainMainPose.poseBone == s_boneIdx ? 1 : 0, m_selectedEntity == e ? 1 : 0,
                 m_world->HasComponent<PreviewStageTag>(e) ? 1 : 0,
                 static_cast<unsigned long long>(m_bonePoseGizmoSubmits - s_submits0),
                 m_bonePoseGizmoLastEntity == e ? 1 : 0, static_cast<unsigned long long>(frame));
    }
}
#endif  // WITH_HARNESS
