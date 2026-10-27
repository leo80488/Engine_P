// EditorLayer's core TU: the ImGui lifecycle, OnUIRender and the dock layout, and the frame-boundary
// drain ProcessPendingActions (deferred scene load / new / save, the bake preview swap, every preview
// panel's view lifecycle). Also the Inspector (with DrawComponentWithUndo), the Content Browser,
// Output Log, Environment, Post Processing, timeline and analysis windows, retargeting, autosave /
// prefs, and the chain-physics and skeleton overlays. Other EditorLayer members are in the
// EditorLayer_*.cpp TUs and the per-window TUs (ChainPhysicsEditorWindow.cpp, SequencerWindow.cpp, ...).
// Contract: world teardown, and anything that frees a descriptor an ImGui::Image may hold, runs from
// ProcessPendingActions, never inside the ImGui frame.
#include "Editor/ColorWidgets.h"
#include "Editor/EditorLayer.h"
#include "Editor/EditorPanels.h"
#include "Editor/EditorServices.h"
#include "Editor/EditorNotify.h"
#include "Scripting/LuaEntityId.h"   // the Inspector rows' Lua ids
#include "Editor/AttackEditor.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/EditorLayerShared.h"  // kWindow* titles, s_faIconsLoaded, SaveFileDialog (shared by EditorLayer_* TUs)
#include "Editor/EditorTheme.h"        // style + Noto/Font Awesome font chain
#include "Editor/EditorViewCube.h"     // the view cube: TurnEditorCamera
#include "Editor/EditorUITests.h"      // the UI tests' frame hook and shutdown
#include "ECS/CameraSystem.h"         // CameraSystem::MakeTransform (the free camera's turn)
#include "Editor/ImGuiWidgets.h"
#include "Editor/MaterialPreviewWidget.h"  // live material preview
#include "Editor/CameraPreviewWindow.h"    // live camera preview
#include "Editor/GameView.h"               // the Game View: routes, its secondary views
#include "Editor/AssetPreviewWindow.h"     // mesh / prefab preview
#include "Editor/MeshEditorWindow.h"       // the Mesh Editor
#include "Editor/SkeletonEditorWindow.h"   // the Skeleton Editor
#include "Editor/TextureViewerWindow.h"    // the Texture Viewer
#include "Editor/RenderGraphWindow.h"     // Window > Analysis > Render Graph
#include "Editor/EditorModule.h"         // the modules' panels
#include "Module/ModuleRegistry.h"       // a disabled module's components: not offered, marked
#include "Editor/UIEditorWindow.h"         // the UI Editor
#include "UI/UICanvasLayout.h"             // UI::IsUINode (the Inspector's linked-UI banner)
#include "UI/UIDocument.h"                 // the Inspector's UI Document instance banner
#include "Editor/MaterialPropertyPanel.h" // the material controls
#include "Editor/MaterialEditorWindow.h"  // the Material Editor
#include "Editor/NodeEditorSmoke.h"       // the node editor's acceptance run (-nodeeditor_smoke)
#include "Graphics/FormatNames.h"          // format names for the Loaded Textures window
#include "Editor/SplitViewportPanel.h"     // N split camera views
#include "Editor/ChainPhysicsPreviewWindow.h" // chain physics preview
#include "System/CommandLine.h"            // -unattended / -imguiini layout policy (OnAttach)
#include "Editor/PersonalFiles.h"          // imgui.ini / editor_prefs.ini and their seeds
#include "Harness/ControlChannelJobs.h" // a pipe-requested save is a job; an inert stub in Retail
#include "Editor/EntityRefPicker.h"
#include "ECS/PlayModeController.h"   // authoritative Edit/Play state machine
#include "Reflection/ComponentReflection.h" // Reflect::Descriptor<T> specializations for RegisterReflectedComponent
#include "Sequencer/SequenceEval.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "AI/AIComponents.h"
#include "AI/BTNode.h"
#include "ECS/Components.h"           // Transform, MaterialComponent
#include "ECS/HierarchyOps.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/FollowComponents.h"
#include "ECS/AnimFXGateComponent.h"
#include "Graphics/TracerSystem.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/TagComponent.h"
#include "Scene/MeshSpawner.h"
#include "Scene/SceneDefaults.h"   // Scene::MigrateEnvironment on File>Load, kBaseScenePath
#include "Resource/SceneSerializer.h"  // SaveScene / LoadScene
#include "Resource/AssetFS.h"          // SharedTwinDir: -preview_churn over a shared worktree's folders
#include "Resource/PostProcessConfig.h" // Save/Load/Apply post-process config (.ippc)
#include "Resource/EngineSettings.h"    // SaveCapturedEngineSettings: the Input Bindings window's save
#include "Editor/InputBindingsWindow.h" // Window > Scripting & Gameplay > Input Bindings (rebind + pad diagnostics)
#include "Platform/Window.h"            // fullscreen toggle, ImGui's message filter and capture predicates
#include "ECS/HierarchyComponents.h"  // LocalTransform, GlobalTransform, Parent, Children, etc.
#include "ECS/AnimEntityResolve.h"    // a model root's skeleton carrier
#include "ECS/StaticMeshComponents.h" // StaticMesh owners and their derived sections
#include "Resource/MeshLibIdentity.h" // MeshKeyToHex - StaticMesh slot keys
#include "Scene/MeshAssetRegistry.h"  // the Static Mesh inspector's mesh / slot lists
#include "ECS/EditorFolderComponent.h" // editor outliner folder membership
#include "ECS/PhysicsComponents.h"    // RigidBodyComponent, ColliderComponent
#include "Tools/CollisionMeshBaker.h" // Bake render meshes into combined .meshlib
#include "Resource/MeshLibrary.h"     // Load baked meshlib for in-world preview swap
#include "Resource/AssetHeader.h"
#include "System/TaskSystem.h"        // Worker count for the bake popup
#include "System/PathUtil.h"          // UTF-8 <-> UTF-16 filesystem boundary (non-ASCII paths)
#include "Editor/Undo/ComponentEditAction.h" // Add/RemoveComponentAction (pulls ComponentBlob.h)
#include "Editor/Undo/TransformEditAction.h" // Entity.set's Transform writes
#include "Scripting/LuaEntityBindings.h"
#include "Editor/Undo/ValueEditAction.h"     // ValueEditAction - value-copy field-edit undo
#include "Editor/Undo/StructuralAction.h"
#include "Editor/Undo/RenameAction.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneNavMesh.h"        // the scene's navMesh= on load and save
#include "Scene/SceneInstanceLoader.h"
#include "Resource/ModelImportWrite.h"   // non-destructive model-import write policy
#include "Resource/RetargetBaker.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "Editor/HumanoidMapping.h"             // the humanoid mapping rows, the .iskel save
#include "Resource/AnimationSerializer.h"
#include "Resource/AnimationLoader.h"   // sync .ianim parse for the retarget source
#include "Renderer/Renderer.h"
#include <chrono>                       // ProcessPendingActions load-profile timing
#include "Graphics/ShaderReflection.h"
#include "Graphics/ShaderLibrary.h"
#include "ECS/MaterialReflectionSync.h"
#include "ECS/MaterialSchema.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/EditorGridPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/FSR2Pass.h"
#include "RenderGraph/RenderPass/FXAAPass.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "PostProcess/PostChain.h"
#include "PostProcess/ProfileSystem.h"
#include "PostProcess/PostProcessProfile.h"
#include "PostProcess/PostProcessProfileSerializer.h"
#include "PostProcess/PostProcessRuntime.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "ECS/IKSystem.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "System/Log.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/AssetManager.h"
#include "Resource/MaterialImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialOverrideApply.h"  // ReloadMaterialsAffectedBy (asset-mode save)
#include "Resource/MaterialLiveOverlay.h"    // an entity's material override is diffed against the FILE
#include "Resource/AssetRefs.h"              // ToLowerAscii (-preview_churn extension test)
#include "Resource/DecalMaterialSerializer.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"
#include "Resource/TextureImporter.h"
#include "Resource/TextureImportRecord.h"   // the import record ImportTextureFile writes
#include "Resource/AssetGuid.h"        // an import keeps (or mints) its .meta identity
#include "System/Stage.h"              // a long import declares itself to `status`
#include "System/UiWindows.h"          // where the panels are, for the harness's app_ui captures
#include "Resource/ShaderImporter.h"
#include "Resource/SceneImporter.h"
#include "Resource/PmxImporter.h"
#include "Resource/VrmImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/VmdImporter.h"
#include "ECS/AnimationComponents.h"
#include "ECS/FacialAnimationSystem.h"
                                       // (the header above brings FacialAnimationComponent.h and SkeletonAsset.h)
#include "ECS/NotifyTypes.h"
#include "Editor/NotifyEditorCategoryDrawers.h"
#include "ECS/FollowComponents.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Editor/DebugDrawSystem.h"
#include "Scripting/ScriptSystem.h"     // exposed-var schema + live push
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/GPUProfiler.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

// Forward-decl for ImGui_ImplWin32_WndProcHandler (the window's message filter, below).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include "implot/implot.h"   // the Profiler's plots: the context lives with ImGui's
#include "Editor/ChainRestPoseMath.h"      // shared rest-pose delta math (the Chain Physics Preview gizmo)
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>        // localtime_s / strftime - asset browser "Modified" column
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>   // Build > Set Startup Scene patches game.json in place
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>          // sorted category buckets in Add Component popup
#include <optional>     // AssignMaterial's value-copy snapshots
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>   // IFileOpenDialog - folder picker for the batch retarget
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

using namespace DirectX;

namespace
{
    // Collision-mesh-baker preview swap: "Preview baked" points every entity whose MeshLibRef used a
    // source mesh at the baked library, and the originals are kept here for "Restore". File scope
    // because both halves run in ProcessPendingBakePreview at the frame boundary; the popup's
    // buttons only record intent.
    struct PreviewBackup { MeshLibRef original; };
    std::unordered_map<Entity, PreviewBackup> g_previewBackups;
    Resource::Handle                          g_previewBakedLib;   // invalid if not loaded
    std::string                               g_previewLoadedFor;  // path of currently-loaded baked lib
}

// Declared in EditorLayerShared.h: the import TU asks this instead of reading
// g_previewBackups, which stays file-local.
bool BakePreviewSwapActive()
{
    return !g_previewBackups.empty();
}

// Point game.json's "startup_scene" at `sceneIdOrPath` (a registry name or a project-root-relative
// .iscene path), keeping the rest of the manifest: the "scenes" registry that Lua Scene.Load("name")
// reads must survive. A missing or unparseable file starts from an empty object, so a fresh project
// can be set up from the editor. Declared in EditorLayerShared.h (Build > Set Startup Scene is in
// the main-menu TU).
void WriteStartupSceneToManifest(const std::string& sceneIdOrPath)
{
    nlohmann::json j = nlohmann::json::object();
    {
        // The PROJECT's manifest, not "whatever game.json the working
        // directory happens to hold".
        const std::string manifest = Sys::ProjectDir() + "/game.json";
        std::ifstream in(Sys::U8Path(manifest));
        if (in)
        {
            try { in >> j; }
            catch (const std::exception& ex)
            {
                LOG_WARNING("game.json is not valid JSON (%s) — rewriting from scratch",
                            ex.what());
                j = nlohmann::json::object();
            }
        }
    }
    if (!j.is_object()) j = nlohmann::json::object();

    j["startup_scene"] = sceneIdOrPath;

    std::ofstream out(Sys::U8Path(Sys::ProjectDir() + "/game.json"),
                      std::ios::trunc);
    if (!out)
    {
        LOG_ERROR("Failed to write game.json");
        return;
    }
    out << j.dump(2) << '\n';
    LOG_INFO("game.json updated: startup_scene = '%s'", sceneIdOrPath.c_str());
}

namespace
{
    // Convert a Win32 "double-NUL-terminated" filter/spec string ("A\0*.a\0\0")
    // from UTF-8 to UTF-16, preserving the embedded and terminating NULs so the
    // *W common-dialog APIs see the same segmented layout.
    std::wstring WidenDialogSpec(const char* spec)
    {
        if (!spec) return std::wstring(2, L'\0');
        const char* p = spec;
        while (p[0] || p[1]) ++p;                 // stop at the "\0\0" terminator
        const int bytes = static_cast<int>(p - spec) + 2;   // include both NULs
        const int wlen  = MultiByteToWideChar(CP_UTF8, 0, spec, bytes, nullptr, 0);
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, spec, bytes, w.data(), wlen);
        return w;
    }
}

// Open a Win32 multi-select file dialog; returns all chosen paths as UTF-8
// (empty on cancel). Uses the wide (*W) API so non-ASCII paths survive.
// Declared in EditorLayerShared.h: the asset import picks its sources with it.
std::vector<std::string> OpenFileDialogMulti(const char* filter, const char* initDir)
{
    // 32 KB (wide) buffer supports ~200+ typical file paths.
    static wchar_t s_buf[32768];
    s_buf[0] = L'\0';
    const std::wstring wFilter  = WidenDialogSpec(filter);
    const std::wstring wInitDir = initDir ? Sys::Utf8ToWide(initDir) : std::wstring{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.lpstrFilter     = wFilter.c_str();
    ofn.lpstrFile       = s_buf;
    ofn.nMaxFile        = static_cast<DWORD>(std::size(s_buf));
    ofn.lpstrInitialDir = wInitDir.empty() ? nullptr : wInitDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
              | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    {
        // Modal: the frame loop stops until the person answers - say so to the control channel's `status`.
        Sys::Stage::Scope stage("modal_dialog", "open files");
        if (!GetOpenFileNameW(&ofn))
            return {};
    }

    // Parse: single file = "full\path\0", multi = "dir\0file1\0file2\0\0".
    std::vector<std::string> paths;
    const wchar_t* p = s_buf;
    std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == L'\0')
    {
        paths.push_back(Sys::WideToUtf8(dir));
    }
    else
    {
        while (*p)
        {
            paths.push_back(Sys::WideToUtf8(dir + L"\\" + p));
            p += wcslen(p) + 1;
        }
    }
    return paths;
}

// Single-file open dialog; returns a UTF-8 path (empty on cancel). Declared in EditorLayerShared.h
// for the main-menu TU.
std::string OpenFileDialog(const char* filter, const char* initDir)
{
    static wchar_t s_buf[MAX_PATH];
    s_buf[0] = L'\0';
    const std::wstring wFilter  = WidenDialogSpec(filter);
    const std::wstring wInitDir = initDir ? Sys::Utf8ToWide(initDir) : std::wstring{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.lpstrFilter     = wFilter.c_str();
    ofn.lpstrFile       = s_buf;
    ofn.nMaxFile        = static_cast<DWORD>(std::size(s_buf));
    ofn.lpstrInitialDir = wInitDir.empty() ? nullptr : wInitDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    Sys::Stage::Scope stage("modal_dialog", "open file");   // the frame loop waits for the person
    return GetOpenFileNameW(&ofn) ? Sys::WideToUtf8(s_buf) : std::string{};
}

namespace
{
    // Folder picker; returns a UTF-8 path (empty on cancel). COM is initialized locally and balanced:
    // CoInitializeEx returns S_FALSE when this thread is already an STA (still a successful init that
    // must be paired), and RPC_E_CHANGED_MODE when it is an MTA - then CoUninitialize is skipped and the
    // shell dialog still works.
    std::string OpenFolderDialog(const char* initDir)
    {
        std::string result;
        const bool weInit = SUCCEEDED(
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dlg))))
        {
            DWORD flags = 0;
            dlg->GetOptions(&flags);
            dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (initDir && *initDir)
            {
                std::error_code ec;
                const std::filesystem::path abs =
                    std::filesystem::absolute(Sys::U8Path(initDir), ec);
                IShellItem* si = nullptr;
                if (!ec && SUCCEEDED(SHCreateItemFromParsingName(abs.c_str(), nullptr,
                                                                 IID_PPV_ARGS(&si))))
                { dlg->SetFolder(si); si->Release(); }
            }
            HRESULT shown = E_FAIL;
            {
                Sys::Stage::Scope stage("modal_dialog", "pick folder");   // the frame loop waits for the person
                shown = dlg->Show(nullptr);
            }
            if (SUCCEEDED(shown))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)))
                {
                    PWSTR w = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)))
                    { result = Sys::WideToUtf8(w); CoTaskMemFree(w); }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (weInit) CoUninitialize();
        return result;
    }
}

// Read a file from disk into a byte vector (UTF-8 path aware). Declared in
// EditorLayerShared.h.
std::vector<uint8_t> ReadFileFully(const std::string& path)
{
    std::vector<uint8_t> buf;
    Sys::ReadFileBytes(path, buf);   // returns empty vector on failure
    return buf;
}

// Write a byte vector to disk, creating directories as needed (UTF-8 path
// aware). Declared in EditorLayerShared.h.
bool WriteFileFully(const std::string& path, const std::vector<uint8_t>& data)
{
    return Sys::WriteFileBytes(path, data);
}

namespace
{
    // Strip MSVC "struct " / "class " / "enum " prefix from typeid().name().
    const char* StripTypePrefix(const char* raw)
    {
        if (!raw) return "(unknown)";
        if (std::strncmp(raw, "struct ", 7) == 0) return raw + 7;
        if (std::strncmp(raw, "class ",  6) == 0) return raw + 6;
        if (std::strncmp(raw, "enum ",   5) == 0) return raw + 5;
        return raw;
    }
}

// Single-file save dialog; returns a UTF-8 path (empty on cancel). Declared in EditorLayerShared.h:
// the outliner's "Save as Prefab" uses it too.
std::string SaveFileDialog(const char* filter, const char* defaultExt, const char* initDir)
{
    static wchar_t s_buf[MAX_PATH];
    s_buf[0] = L'\0';
    const std::wstring wFilter  = WidenDialogSpec(filter);
    const std::wstring wDefExt  = defaultExt ? Sys::Utf8ToWide(defaultExt) : std::wstring{};
    const std::wstring wInitDir = initDir ? Sys::Utf8ToWide(initDir) : std::wstring{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.lpstrFilter     = wFilter.c_str();
    ofn.lpstrFile       = s_buf;
    ofn.nMaxFile        = static_cast<DWORD>(std::size(s_buf));
    ofn.lpstrInitialDir = wInitDir.empty() ? nullptr : wInitDir.c_str();
    ofn.lpstrDefExt     = wDefExt.empty()  ? nullptr : wDefExt.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    Sys::Stage::Scope stage("modal_dialog", "save file");   // the frame loop waits for the person
    return GetSaveFileNameW(&ofn) ? Sys::WideToUtf8(s_buf) : std::string{};
}

// ImGui lifecycle (editor-owned). Engine is ImGui-free; backends initialised in InitImGuiBackends.
EditorLayer::EditorLayer() = default;

// Bind the editor to a World and install an entity-destroy listener that clears a stale selection /
// highlight (a recycled entity id would otherwise be re-selected, and the Inspector would show the
// wrong or a dead entity). App calls this every frame: early-out unless the World pointer changed, or
// the listener is re-installed 60+ times a second.
void EditorLayer::SetWorld(World* world)
{
    if (world == m_world) return;

    m_pendingComponentAdds.clear();

    if (m_world && m_entityDestroyListener != 0)
        m_world->RemoveEntityDestroyListener(m_entityDestroyListener);
    m_entityDestroyListener = 0;

    m_world = world;
    // Selection does not carry across a world swap.
    m_selectedEntity         = NullEntity;
    m_hierarchyHighlight     = NullEntity;
    m_lastSeenSelectedEntity = NullEntity;
    MarkHierarchyDirty();

    // Rebind the undo history to the new world and drop any actions (their entity handles belong to the
    // old world). Also reset transform bracketing.
    m_editHistory.Context().world = world;
    m_editHistory.Clear();
    m_xformRecording     = false;
    m_xformBaselineValid = false;
    m_fieldEdit.open     = false;

    // Dedicated-window targets belong to the old world. Windows stay open and
    // show "target lost" until the user re-latches with Use Selected / Edit.
    m_animSetEditTarget = NullEntityHandle;
    m_animSetSelClip    = -1;
    // A pose-tool bone index names one entity's skeleton - none of the old world's.
    m_chainMainPose.Reset(NullEntity);
    m_chainScratchPose.Reset(NullEntity);

    if (m_world)
    {
        m_entityDestroyListener = m_world->AddEntityDestroyListener(
            [this](Entity e)
            {
                if (m_selectedEntity         == e) m_selectedEntity         = NullEntity;
                if (m_hierarchyHighlight     == e) m_hierarchyHighlight     = NullEntity;
                if (m_lastSeenSelectedEntity == e) m_lastSeenSelectedEntity = NullEntity;
                if (m_animSetEditTarget.entity == e) m_animSetEditTarget = NullEntityHandle;
            });
    }
}

EditorLayer::~EditorLayer()
{
    // Best-effort - a CPU-only file write, and prefs are the one thing that should survive every way a
    // session can end.
    SaveEditorPrefs();
    // The panels' shutdown hooks while ImGui still exists; then nothing in the registry or the services
    // may point at this layer any more.
    Editor::PanelRegistry::Get().Shutdown();
    Editor::PanelRegistry::Get().Clear();
    Editor::EditorServices() = {};
    Editor::ModuleRegistry::Get().Shutdown();

    // Drop the entity-destroy listener so a World that outlives the editor never
    // fires into a destroyed `this`.
    if (m_world && m_entityDestroyListener != 0)
    {
        m_world->RemoveEntityDestroyListener(m_entityDestroyListener);
        m_entityDestroyListener = 0;
    }

    // The panels' provider reads the ImGui context this destructor tears down.
    Sys::UiWindows::SetProvider(nullptr);

    // Detach from the window first so in-flight messages don't re-enter half-destroyed ImGui state.
    m_imguiMessageFilter.Reset();
    m_imguiCapturesMouse.Reset();
    m_imguiCapturesKeyboard.Reset();

    if (m_imguiBackendsReady)
    {
        m_imguiMgr.Shutdown();
        m_imguiBackendsReady = false;
    }
    if (ImGui::GetCurrentContext())
    {
        // The test engine's order: stop before the ImGui context goes, destroy after it.
        Editor::UITests::StopEngine();
        // ImPlot's context reads ImGui's: it goes first.
        if (ImPlot::GetCurrentContext()) ImPlot::DestroyContext();
        ImGui::DestroyContext();
        Editor::UITests::DestroyEngine();
    }
}

void EditorLayer::InitImGuiBackends()
{
    if (m_imguiBackendsReady || !m_gfx) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    m_imguiMgr.SetStreamUploader(&dx12);   // the backend's texture uploads stream
    m_imguiMgr.Initialize(
        dx12.Surface().GetHwnd(),
        dx12.GetDevice(),
        dx12.GetGraphicsQueue(),
        dx12.GetCbvSrvUavAllocator(),
        dx12.GetFrameCount());
    m_imguiMgr.SetSamplerHeap(dx12.GetSamplerAllocator().GetGpuHeap());

    // Route Win32 messages into ImGui, and let the window skip gameplay input
    // while ImGui wants it. ImGui's backend stays a SYNCHRONOUS filter (it reads
    // GetKeyState and calls SetCapture at the moment of the message), the one
    // outside callback the WndProc runs (Platform/Window.h). Its predicates are
    // OR-ed with the game Canvas's, which App subscribes on its own.
    if (m_window)
    {
        m_imguiMessageFilter = m_window->AddMessageFilter([](HWND h, UINT m, WPARAM w, LPARAM l)
        {
            return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
        });
        m_imguiCapturesMouse    = m_window->AddCapturePredicate(Plat::CaptureKind::Mouse,
            []() -> bool { return ImGui::GetIO().WantCaptureMouse; });
        m_imguiCapturesKeyboard = m_window->AddCapturePredicate(Plat::CaptureKind::Keyboard,
            []() -> bool { return ImGui::GetIO().WantCaptureKeyboard; });
    }

    m_imguiBackendsReady = true;
}

void EditorLayer::BeginImGuiFrame()
{
    if (!m_imguiBackendsReady) return;
    SyncHiddenUiSize();
    // The UI tests' frame hook (the test engine's "after the swap"): here, on main, whichever path
    // records the previous frame's ImGui (the render owner's snapshot or EndImGuiFrame). Nothing
    // until a run asked for UI tests.
    Editor::UITests::PostFrame();
    if (!m_pendingWorkspaceLayout.empty())
    {
        ImGui::LoadIniSettingsFromMemory(m_pendingWorkspaceLayout.c_str(), m_pendingWorkspaceLayout.size());
        m_pendingWorkspaceLayout.clear();
    }
    m_imguiMgr.BeginFrame();
}

// -hidden_ui (harness): -hidden minimizes the window, and a minimized window
// gives ImGui a 0x0 display - no layout, no panels, a blank back buffer (and a
// blank app_ui capture). With the flag ImGui lays out at the back buffer's
// size instead, so a hidden editor draws exactly what a restored one would,
// the scene in its viewport panel. Off by default: every other hidden run keeps
// its UI-free frame.
void EditorLayer::SyncHiddenUiSize()
{
    static const bool s_hiddenUi = Sys::CommandLine::Get().Has("-hidden_ui");
    if (s_hiddenUi && m_gfx)
        m_imguiMgr.SetMinimizedDisplaySize(static_cast<float>(m_gfx->GetWidth()),
                                           static_cast<float>(m_gfx->GetHeight()));
}

void EditorLayer::PrimeViewportLayout()
{
    if (!m_imguiBackendsReady || m_viewportFullscreen) return;
    SyncHiddenUiSize();

    // DockBuilder changes are committed at frame boundaries.  The first pass
    // may still report the viewport's old/floating rectangle, so continue
    // until two consecutive layout-only frames agree (bounded for safety).
    unsigned int previousW = 0;
    unsigned int previousH = 0;
    unsigned int stableSamples = 0;
    constexpr unsigned int kMaxPrimeFrames = 4;

    for (unsigned int i = 0; i < kMaxPrimeFrames; ++i)
    {
        m_imguiMgr.BeginFrame();
        ImGuizmo::BeginFrame();
        // Same order as OnUIRender: the status bar's work-area inset must be
        // in place for the primed size to match the first real frame.
        RenderStatusBar();
        SetupDockSpace();

        // Docked siblings must be alive for ImGui to resolve the split-node
        // rectangles.  Minimal placeholder Begin/End pairs avoid running the
        // asset/inspector UI during this non-rendered bootstrap frame.
        ImGui::Begin(kWindowHierarchy);
        ImGui::End();
        RenderViewportPanel();
        ImGui::Begin(kWindowInspector);
        ImGui::End();
        // Log first, Content Browser last: see OnUIRender (creation order
        // decides which tab of the shared bottom node starts in front).
        ImGui::Begin(kWindowOutputLog);
        ImGui::End();
        ImGui::Begin(kWindowContentBrowser);
        ImGui::End();
        ImGui::EndFrame();

        if (m_viewportSizeX == previousW && m_viewportSizeY == previousH &&
            m_viewportSizeX != 0 && m_viewportSizeY != 0)
        {
            if (++stableSamples >= 1) break;
        }
        else
        {
            stableSamples = 0;
            previousW = m_viewportSizeX;
            previousH = m_viewportSizeY;
        }
    }

    LOG_INFO("EditorLayer: primed viewport layout at %ux%u",
             m_viewportSizeX, m_viewportSizeY);
}

// ==== Preview surfaces at shutdown ====
// Closes every preview surface (the contract is at the declaration in EditorLayer.h). The panels are
// asked in the same order ProcessPendingActions asks them: the order does not matter, and matching
// lists are easier to compare. The drain is DrainPreviewViewReleases, not ServicePreviewReleases:
// that one also runs the idle sweep, whose grace is counted in calls, so one extra call at shutdown
// could add a teardown to a session summary a pack pins exactly.
void EditorLayer::ReleasePreviewViews(Renderer& renderer)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // The Inspector's half of [PANELVIS] (the camera panel's is beside it): `drew` is the assertion,
    // `beginRefused` the diagnosis, and `noSurface` the drawn frames that ran only because -hidden
    // leaves visibility unanswerable (all of them under -hidden; 0 there means the run grew a UI
    // surface). Emitted whenever the flag is on, healthy or not.
    if (Sys::CommandLine::Get().Has("-panelvisibleverify"))
        LOG_INFO("[PANELVIS] inspector drew=%llu beginRefused=%llu noSurface=%llu",
                 static_cast<unsigned long long>(m_inspectorDrewFrames),
                 static_cast<unsigned long long>(m_inspectorRefusedFrames),
                 static_cast<unsigned long long>(m_inspectorNoSurface));
    // The Output Log mirror against the logger's ring (-logpanel_verify). The
    // logger is still running here, so this line reaches the file it describes.
    if (m_logPanelVerify)
    {
        const Editor::OutputLogPanel::VerifyResult v = m_outputLog.Verify();
        LOG_INFO("[LOGPANEL] verify: mirror=%zu ring=%zu overlap=%zu matched=%zu missing=%zu "
                 "mismatches=%zu dups=%zu gaps=%llu gapLines=%llu filtered=%zu tags=%zu "
                 "engine=%zu lua=%zu console=%zu control=%zu",
                 v.mirror, v.ring, v.overlap, v.matched, v.missing, v.mismatches, v.dups,
                 static_cast<unsigned long long>(v.gaps), static_cast<unsigned long long>(v.gapLines),
                 v.filtered, v.tags,
                 v.bySource[static_cast<std::size_t>(LogSource::Engine)],
                 v.bySource[static_cast<std::size_t>(LogSource::Lua)],
                 v.bySource[static_cast<std::size_t>(LogSource::Console)],
                 v.bySource[static_cast<std::size_t>(LogSource::Control)]);
    }
#endif
    Editor::MaterialPreviewWidget::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::CameraPreviewWindow::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::AssetPreviewWindow::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::MeshEditorWindow::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::SkeletonEditorWindow::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::TextureViewerWindow::Get().ReleaseAtShutdown(TextureViewerCtx());
    Editor::RenderGraphWindow::Get().Shutdown(m_gfx);
    Editor::UIEditorWindow::Get().ReleaseAtShutdown(UIEditorCtx());
    Editor::MaterialEditorWindow::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::MaterialEditorWindow::Get().Shutdown();   // the graph canvases' node editors
    Editor::NodeEditorSmoke::Get().Shutdown();
    Editor::AnimGraphEditor::Shutdown();
    Editor::SplitViewportPanel::Get().ReleaseViewsAtShutdown(&renderer);
    Editor::GameView::Get().ReleaseViewsAtShutdown(&renderer);
    // Also the World, read only, for -chainpreview_dump.
    Editor::ChainPhysicsPreviewWindow::Get().ReleaseViewsAtShutdown(&renderer, m_world);
    renderer.DrainPreviewViewReleases();
}

void EditorLayer::ProcessPendingActions()
{
    auto componentAdds = std::move(m_pendingComponentAdds);
    m_pendingComponentAdds.clear();
    for (auto& add : componentAdds) add();
    // The editor's own frame number, in every configuration. The harness tick counts the same clock
    // (TickHarnessPreview is called only from here) but does not exist in Retail, and the save->tile
    // invalidation line must name a frame there too.
    ++m_editorFrame;
    // The registered panels' per-frame hooks and their world / play / selection events.
    TickPanelLifecycle();

    // Material-preview view lifecycle. This is the widget's per-frame tick as
    // well as its release drain, and it runs unconditionally (before the early
    // returns further down) because -preview_frames counts these ticks.
    Editor::MaterialPreviewWidget::Get().ProcessPendingActions(m_renderer);
    // The camera-preview panel's view lifecycle, on the same terms: it holds an HDR slot and an
    // ImGui-sampled descriptor, and the frame boundary is the only place either can be handed back.
    Editor::CameraPreviewWindow::Get().ProcessPendingActions(m_renderer);
    // The Game View's game camera (or why there is none) and its secondary views' release.
    Editor::GameView::Get().UpdateCamera(m_world, GetPlayState() != ViewportPlayState::Stopped);
    Editor::GameView::Get().ProcessPendingActions(m_renderer, GetPlayState() != ViewportPlayState::Stopped);
    Editor::AssetPreviewWindow::Get().ProcessPendingActions(m_renderer);
    // The Mesh Editor's view lifecycle, its queued open / save (file writes + registry / preview
    // invalidation) and its harness steps - all game-half work, so here.
    Editor::MeshEditorWindow::Get().ProcessPendingActions(MeshEditorCtx());
    // The Skeleton Editor's view lifecycle, its queued open (file reads) and its harness steps -
    // game-half work, so here.
    Editor::SkeletonEditorWindow::Get().ProcessPendingActions(SkeletonEditorCtx());
    Editor::AnimGraphEditor::ProcessPendingActions(m_world,m_renderer,m_assetMgr,m_animClipSys,GetPlayState()==ViewportPlayState::Stopped);
    Editor::AnimSetEditor::ProcessPendingActions(m_renderer ? m_renderer->GetAnimGraphSystem() : nullptr);
    // The Texture Viewer's opens / closes (TextureSystem references), its texture view's description and
    // its harness steps and shots (CaptureTextureToPNG syncs) - game-half work, so here.
    Editor::TextureViewerWindow::Get().ProcessPendingActions(TextureViewerCtx());
    // The UI Editor's opens / closes, its tabs' subject check (a world swap re-resolves the canvas by
    // GUID), its UI view's description and release, its harness steps and shots - game-half work, so
    // here.
    Editor::UIEditorWindow::Get().ProcessPendingActions(UIEditorCtx());
    // The Material Editor's opens, saves (file writes + the entities using them re-apply), reverts, undo
    // requests, disk-change checks, its preview's view release and its harness steps.
    Editor::MaterialEditorWindow::Get().ProcessPendingActions(MaterialEditorCtx());
    // The Inspector material row's Render.explain warnings (the renderer's runtime-material maps, read
    // here where the render half is parked).
    RefreshMaterialRowHints();
    // The Loaded Textures window's rows (TextureConsumers needs the lane parked); a closed window keeps
    // none, so reopening it reads them afresh.
    if (m_showLoadedTextures) RefreshLoadedTextures();
    else if (m_loadedTexturesRevision != 0)
    {
        m_loadedTextures.clear();
        m_loadedTextures.shrink_to_fit();
        m_loadedTexturesRevision = 0;
    }
    Editor::SplitViewportPanel::Get().ProcessPendingActions(m_renderer);
    // The Chain Physics Preview panel. Unlike the material / camera / asset / split preview panels it also
    // spawns and destroys entities (its hidden stage), so it takes the World and the loaders and must run
    // in this window and nowhere else.
    {
        Editor::ChainPreviewContext cpv{};
        cpv.renderer    = m_renderer;
        cpv.world       = m_world;
        cpv.assetMgr    = m_assetMgr;
        cpv.animClipSys = m_animClipSys;
        cpv.physics     = m_physicsSys;
        cpv.gfx         = m_gfx;
        cpv.editing     = GetPlayState() == ViewportPlayState::Stopped;
        // The panel's StageClock runs on the editor's real frame delta (Edit mode's animDt is 0 by design).
        cpv.realDt      = ImGui::GetIO().DeltaTime;
        // The overlay's bone source is the same FK the main overlay, the bone picker and the gizmo use (never
        // derive FK a second time), and a panel-undo step is committed only once no ImGui item is held (a
        // slider drag is one step).
        cpv.buildBones  = [this](Renderer& r, World& w, Entity e,
                                 std::vector<DirectX::XMFLOAT4X4>& b2m,
                                 DirectX::XMMATRIX& ent)
        { return BuildBoneWorldMatrices(r, w, e, b2m, ent); };
        cpv.uiEditActive = ImGui::IsAnyItemActive();
        // Apply -> Scene entity / All instances pushes ONE entry onto this history (never the panel's own
        // staged ring), gated by the Inspector's Edit-mode rule.
        cpv.undoEnabled = UndoEnabled();
        cpv.pushEdit    = [this](std::unique_ptr<Editor::Undo::IEditAction> a)
        {
            SyncedEditContext();
            PushEdit(std::move(a));   // also marks the scene dirty
        };
        cpv.undoCount   = [this]() { return m_editHistory.UndoCount(); };
        cpv.scenePath   = m_currentScenePath;
        auto& chainPreview = Editor::ChainPhysicsPreviewWindow::Get();
        chainPreview.ProcessPendingActions(cpv);
        // Scene collider edits and their undo feed the isolated simulation at
        // the same serial boundary as the rest of the preview's World edits.
        const EntityHandle capsuleSource = ChainColliderTarget();
        const Entity capsuleStage = chainPreview.StageChainEntity();
        if (chainPreview.IsOpen() && m_world && m_world->IsHandleValid(capsuleSource) &&
            capsuleStage != NullEntity && m_world->IsAlive(capsuleStage) &&
            m_world->HasComponent<PreviewStageTag>(capsuleStage))
        {
            if (const auto* source = m_world->GetComponent<CapsuleColliderComponent>(capsuleSource.entity))
            {
                if (auto* stage = m_world->GetComponent<CapsuleColliderComponent>(capsuleStage))
                    *stage = *source;
                else m_world->AddComponent<CapsuleColliderComponent>(capsuleStage, *source);
            }
            else m_world->RemoveComponent<CapsuleColliderComponent>(capsuleStage);
        }
        // Harness -chainpreview_request: select:<name> sets the editor's selection (what Use Selected /
        // Apply -> Scene would see); worldload reloads the current scene through the ordinary queued load.
        if (const Entity sel = chainPreview.TakeHarnessSelect();
            sel != NullEntity && m_world && m_world->IsAlive(sel))
        {
            ClearEntitySelection();
            SelectEntity(sel, SelectMode::Replace);
            LOG_INFO("[CHAINPREVIEW] harness: editor selection -> entity %u ('%s')",
                     static_cast<unsigned>(sel), m_world->GetName(sel).c_str());
        }
        if (chainPreview.TakeWorldReloadRequest())
        {
            // Straight onto the queued load below (this same window), not
            // RequestLoadScene: its unsaved-changes / autosave-recovery modals
            // cannot be answered under -hidden, and the autosave one depends on
            // whatever the machine's writable dir holds. A dirty scene is not
            // thrown away behind anyone's back - refused and said.
            if (m_sceneDirty)
                LOG_WARNING("[CHAINPREVIEW] harness: worldload refused - the scene is dirty");
            else
            {
                LOG_INFO("[CHAINPREVIEW] harness: reloading the current scene '%s'",
                         m_currentScenePath.c_str());
                m_pendingLoadScenePath = m_currentScenePath;
            }
        }
    }

    // Harness fixture cycle / edit one-shot. Same window, same reason as the widget tick: swapping the
    // material buffer or capturing the preview target mid-ImGui-frame would touch a descriptor an
    // ImGui::Image already handed to the draw list. Inert (one integer compare) unless -preview_fixture
    // / -preview_edit_at was given.
    TickHarnessPreview();

    // The asset browser's material tiles - apply, encode, evict, enqueue. Same window, same reasons: it
    // creates and destroys textures, reads the renderer's runtime-material maps and parses materials
    // off disk, none of which may happen inside the ImGui pass or on a graph worker. One branch when the
    // browser has shown no material tile.
    ServiceAssetThumbnails();

    // FIRST: a console chunk may create or destroy entities, and every pending action below reads the
    // world. Running it here also keeps it outside the ImGui frame (see LuaConsoleWindow.cpp). The
    // harness's -console_exec submits through the same call the Enter key makes.
    TickHarnessConsole();
    ProcessPendingLuaChunk();

    ProcessPendingTextureImports();
    ProcessScriptedImports();
    ProcessModelImportTasks();   // the worker's finished prepares, committed here
    ProcessDroppedFiles();       // WM_DROPFILES / Editor.DropFiles
    ProcessCollisionBake();
    // Every frame, before the early returns below: this frame's BeginFrame
    // may have applied an Editor.Play / Pause / Step / Stop.
    ProcessScriptedPlay();

    // Scene save runs FIRST, and specifically before the New/Load teardown
    // below: when the unsaved-changes modal answered "Save", a failed write has
    // to be able to cancel the destructive op it was guarding (see
    // m_pendingSaveGuardsSceneOp). The other windows' documents go just before it.
    ProcessPendingSaveDocuments();
    ProcessPendingSaveScene();

    // Collision-baker preview swap (MeshLibrary Load / Release + Renderer::OnWorldClear) - same slot,
    // same reason as the load below.
    ProcessPendingBakePreview();

    // New Scene tears the world down exactly like a load, so it shares this
    // out-of-ImGui-frame slot.
    if (m_pendingNewScene)
        ProcessPendingNewScene();

    if (m_pendingLoadScenePath.empty()) return;

    // Always load from a clean Edit state. While Playing or Paused, request a Stop and retry next frame:
    // (a) loading mid-Play tangles runtime state (physics bodies, script instances, play-spawned
    // transients) with the new scene; (b) RequestStop is deferred to the next
    // PlayModeController::BeginFrame, whose onExitPlay -> AdoptState would restore the pre-Play snapshot
    // over the freshly loaded scene. m_pendingLoadScenePath stays set, so once the Stop applies
    // (mode == Edit) the load lands on the restored editor world.
    if (m_playModeController && !m_playModeController->IsEditing())
    {
        m_playModeController->RequestStop();
        return;
    }

    const std::string loadPath = std::move(m_pendingLoadScenePath);
    m_pendingLoadScenePath.clear();
    // Editor.OpenScene waits on this load: finished below with the path actually loaded (a human's
    // File > Open may have replaced it).
    const std::uint64_t loadJob = m_pendingLoadJobId;
    m_pendingLoadJobId = 0;

    if (!m_world || !m_assetMgr)
    {
        if (loadJob) Harness::Jobs::Fail(loadJob, "no_world", "the editor has no world or asset manager", {});
        return;
    }

    m_selectedEntity = NullEntity;

    // Loading replaces the world's contents (same World object, so SetWorld's
    // pointer check won't fire): drop undo actions whose handles belong to the
    // outgoing scene and reset transform bracketing.
    m_editHistory.Clear();
    m_xformRecording     = false;
    m_xformBaselineValid = false;
    m_fieldEdit.open     = false;

    // [LOAD-PROFILE] Measure where the world-load freeze goes: total wall-time vs
    // the CPU time lost CPU-blocking inside upload FlushUploadAndWait (the ceiling
    // on what an async-upload path could remove). Pairs with the WaitForHandlesReady
    // spin timer (SceneSerializer) which measures the CPU-decode-wait half.
    const auto _loadT0 = std::chrono::steady_clock::now();
    if (m_gfx) m_gfx->ResetUploadStats();

    // Hard-sync ALL queues (graphics + compute + copy - DDGI's compute work outlives FlushAndWait's
    // graphics-only sync) and drain every deferred-release slot before touching renderer caches, or the
    // MeshDescriptorHeap reset races in-flight compute work on the old bindless VB/IB.
    // Do not sync a second time after OnWorldClear to cut peak VRAM: that caused a delayed TDR ~5 s after
    // reload (bindless SRV slots still reached the freed memory). The Renderer defers releases instead
    // (textures through TextureSystem::Tick, MeshLibrary a one-world-lag two-stage release), so peak
    // VRAM is old + new for the duration of the reload.
    if (m_gfx)
        m_gfx->WaitIdleAndReleaseDeferred();

    if (m_renderer)
        m_renderer->OnWorldClear();

    // Destroy the outgoing scene's Jolt bodies before LoadScene clears the World (recycled entity ids
    // would otherwise orphan bodies: ghost colliders and a body-pool leak). Same order as
    // SceneManager::ActivateScene's reload.
    if (m_physicsSys)
        m_physicsSys->OnWorldClear();

    std::string sceneName;
    std::string ppcPath;
    std::string navPath;
    std::string sceneScript;
    std::vector<std::string> sceneSystems;
    const bool loaded = Resource::LoadScene(loadPath, *m_world, *m_assetMgr,
                       m_renderer, m_animClipSys, &sceneName, &ppcPath, &navPath,
                       &sceneScript, &sceneSystems);

    // Synthesise a WorldEnvironment singleton for a scene without one (an older file) so it renders as
    // authored; a no-op when the scene has one.
    Scene::MigrateEnvironment(*m_world);

    // Reset the outliner folders: content folders re-derive from each entity's EditorFolderComponent
    // next frame, and the previous scene's session-only empty folders are dropped (no ghost folders).
    m_editorFolders.clear();

    {
        const double   _totalMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - _loadT0).count();
        const double   _stallMs = m_gfx ? m_gfx->GetUploadStallMs()    : 0.0;
        const uint32_t _flushes = m_gfx ? m_gfx->GetUploadFlushCount() : 0u;
        LOG_INFO("[LOAD-PROFILE] World '%s': total %.1f ms | GPU upload-stall %.1f ms "
                 "across %u flushes (%.0f%% of load) <- Phase-3 async-upload ceiling",
                 loadPath.c_str(), _totalMs, _stallMs, _flushes,
                 _totalMs > 0.0 ? (_stallMs / _totalMs * 100.0) : 0.0);
    }

    m_postProcessConfigPath = ppcPath;
    m_sceneScriptPath       = sceneScript;   // retained so SaveScene round-trips it
    m_sceneSystems          = sceneSystems;

    // The loaded file is "the open scene": Save / Ctrl+S write back here instead of prompting, and
    // nothing is unsaved yet.
    m_currentScenePath = loadPath;
    MarkSceneClean();

    // Autosave restore: the file we just loaded is the autosave COPY. Re-point
    // at the original scene and re-mark dirty so Ctrl+S writes the recovery
    // back to the real .iscene instead of into the autosave directory.
    if (!m_autosaveRestoreRepointPath.empty())
    {
        m_currentScenePath = m_autosaveRestoreRepointPath;
        m_autosaveRestoreRepointPath.clear();
        MarkSceneDirty();
        LOG_WARNING("EditorLayer: restored from autosave — Save (Ctrl+S) writes back to '%s'",
                    m_currentScenePath.c_str());
    }

    // After the repoint on purpose: a restored autosave files under the REAL
    // scene, never under the autosave directory.
    AddRecentScene(m_currentScenePath);

    // The .iscene names a .inav beside it: load it so runtime FindPath / NavAgent path-following work
    // without a separate "Load NavMesh" click.
    if (!navPath.empty())
        SceneNavMesh::LoadForScene(navPath);

    // Activate the scene's Lua scene script (if it declares one). The swap is deferred inside
    // ScriptSystem until the editor is Playing (ScriptLogicSystem skips while Stopped), so OnSceneEnter
    // fires on the first played frame (Unity-style). An empty path deactivates any prior scene's script.
    if (m_scriptSys)
        m_scriptSys->SetActiveSceneScript(sceneScript, sceneName, sceneSystems);

    if (loadJob)
    {
        const nlohmann::json out{ { "scene", loadPath }, { "entities", m_world->GetEntities().size() } };
        if (loaded) Harness::Jobs::Succeed(loadJob, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        else        Harness::Jobs::Fail(loadJob, "load_failed", "'" + loadPath + "' did not load (see the log)", {},
                                        out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
}

// Frame-boundary half of the collision-baker preview swap: the Bake Collision Meshes popup's two
// buttons only record intent; everything that allocates or frees GPU objects runs here, in the
// deferred scene load's slot.
void EditorLayer::ProcessPendingBakePreview()
{
    const PendingBakePreviewOp op = m_pendingBakePreviewOp;
    m_pendingBakePreviewOp = PendingBakePreviewOp::None;
    if (op == PendingBakePreviewOp::None) return;

    Resource::MeshLibrary* libSys = m_renderer ? m_renderer->GetMeshLibrary() : nullptr;
    if (!m_world || !m_renderer || !m_gfx || !libSys)
    {
        m_pendingBakePreviewMap.clear();
        m_pendingBakePreviewPath.clear();
        return;
    }

    if (op == PendingBakePreviewOp::Load)
    {
        // Load (or re-load) the baked .meshlib into the runtime library system;
        // the returned handle is what the renderer dereferences each frame.
        const Resource::Handle baked = libSys->Load(m_pendingBakePreviewPath, *m_gfx);
        if (!baked.IsValid())
        {
            LOG_ERROR("CollisionMeshBaker: failed to load baked .meshlib '%s' for preview",
                      m_pendingBakePreviewPath.c_str());
        }
        else
        {
            g_previewBakedLib  = baked;
            g_previewLoadedFor = m_pendingBakePreviewPath;

            // Rewrite each MeshLibRef in place. m_pendingBakePreviewMap is (sourcePath, sourceMeshId) ->
            // bakedMeshId, snapshotted from the bake job when the button was clicked.
            uint32_t swapped = 0;
            m_world->ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
            {
                const MeshSourcePath* sp = m_world->GetComponent<MeshSourcePath>(e);
                if (!sp) return;
                auto pit = m_pendingBakePreviewMap.find(sp->path);
                if (pit == m_pendingBakePreviewMap.end()) return;
                auto mit = pit->second.find(ref.meshId);
                if (mit == pit->second.end()) return;

                g_previewBackups[e] = { ref };
                ref.libHandle        = g_previewBakedLib;
                ref.meshId           = mit->second;
                ref.cachedGeneration = 0; // force renderer re-lookup
                ++swapped;
            });
            LOG_INFO("CollisionMeshBaker: preview swapped %u entities to '%s'",
                     swapped, g_previewLoadedFor.c_str());
        }

        m_pendingBakePreviewMap.clear();
        m_pendingBakePreviewPath.clear();
        return;
    }

    // ---- Restore ----
    for (auto& [e, backup] : g_previewBackups)
    {
        if (MeshLibRef* ref = m_world->GetComponent<MeshLibRef>(e))
        {
            *ref = backup.original;
            ref->cachedGeneration = 0;
        }
    }
    LOG_INFO("CollisionMeshBaker: restored %zu entities from preview swap",
             g_previewBackups.size());
    g_previewBackups.clear();

    // Release the previewed baked lib and FLUSH the renderer's mesh-descriptor cache. MeshManager keys
    // its cache by (libIdx << 32) | meshId and MeshLibrary::Release reuses the libIdx slot on the next
    // Load(), so without the flush the next preview gets stale descriptors pointing at freed GPU
    // buffers - device removed on the next draw.
    if (g_previewBakedLib.IsValid())
    {
        // Hard-sync all queues first, as the deferred scene load does: Renderer::OnWorldClear does no sync
        // of its own (it bumps MeshManager's generation and drops the DDGI BLAS cache assuming the GPU is
        // drained), and earlier frames' command lists may still reference the buffers Release() retires.
        m_gfx->WaitIdleAndReleaseDeferred();
        libSys->Release(g_previewBakedLib, *m_gfx);
        // worldSurvives: this flush only retires the preview meshlib's descriptor caches - the World and its
        // skinned entities keep rendering, so the skinned-buffer ownership sweep must not run.
        m_renderer->OnWorldClear(/*idsPreserved=*/false, /*worldSurvives=*/true);
    }
    g_previewBakedLib = {};
    g_previewLoadedFor.clear();
}

// ==== Scene save / dirty tracking ====

// Deferred New Scene. Same slot and same ordering as the deferred load (see
// ProcessPendingActions): tearing the world down mid-ImGui-frame frees
// descriptor slots that this frame's ImGui::Image calls already captured.
void EditorLayer::ProcessPendingNewScene()
{
    // Refuse mid-Play for the same reason the load path does: RequestStop is applied by the NEXT
    // PlayModeController::BeginFrame, and its snapshot restore would land on top of the fresh (empty)
    // world. The request stays queued; once mode == Edit this runs against the restored editor world.
    if (m_playModeController && !m_playModeController->IsEditing())
    {
        m_playModeController->RequestStop();
        return;
    }

    m_pendingNewScene = false;
    if (!m_world)
    {
        if (m_pendingNewSceneJobId)
            Harness::Jobs::Fail(m_pendingNewSceneJobId, "no_world", "the editor has no world", {});
        m_pendingNewSceneJobId = 0;
        return;
    }

    m_selectedEntity = NullEntity;

    // The outgoing scene's entity ids are about to be recycled - the undo stack's handles would
    // resurrect into the new world. Same as the load path.
    m_editHistory.Clear();
    m_xformRecording     = false;
    m_xformBaselineValid = false;
    m_fieldEdit.open     = false;

    // Same teardown order as the load path (see there for why ALL queues are synced before the renderer
    // / physics cache wipes, and why there is no second sync afterwards).
    if (m_gfx)        m_gfx->WaitIdleAndReleaseDeferred();
    if (m_renderer)   m_renderer->OnWorldClear();
    if (m_physicsSys) m_physicsSys->OnWorldClear();

    m_world->Clear();

    // An empty world has no entities, so create the WorldEnvironment singleton here (LoadScene's
    // migration does it for loaded files) or the renderer has no sky / env binding. Then the NEW scene's
    // sky strength is physical; MigrateEnvironment keeps the 0.1 inheritance because it also serves
    // older files, whose look must not change.
    Scene::MigrateEnvironment(*m_world);
    Scene::ApplyNewSceneSkyStrength(*m_world);

    // Companion asset bindings belonged to the previous scene.
    m_postProcessConfigPath.clear();
    m_sceneScriptPath.clear();
    m_sceneSystems.clear();
    m_currentScenePath.clear();
    m_editorFolders.clear();
    MarkSceneClean();

    if (m_scriptSys)
        m_scriptSys->SetActiveSceneScript(std::string{}, std::string{});

    LOG_INFO("EditorLayer: new (empty) scene");
    if (m_pendingNewSceneJobId)   // Editor.NewScene: done, the world is empty
        Harness::Jobs::Succeed(m_pendingNewSceneJobId, "{\"scene\":\"\",\"entities\":" +
                               std::to_string(m_world->GetEntities().size()) + "}");
    m_pendingNewSceneJobId = 0;
}

// Queue a scene save for the frame-boundary drain. Everything knowable now (Play mode, no world, no
// path - including a cancelled Save As dialog) is still answered synchronously, so the caller and the
// unsaved-changes modal get an immediate verdict; only the write itself is deferred.
bool EditorLayer::RequestSaveScene(std::string path)
{
    if (path.empty() || !m_world) return false;

    // Same refusal (and wording) as SaveSceneToPath, checked here too so Ctrl+S during Play never
    // reaches the queue.
    if (GetPlayState() != ViewportPlayState::Stopped)
    {
        LOG_WARNING("EditorLayer: Save Scene refused — the world is in Play mode. "
                    "Play state is runtime data (Stop restores the pre-Play "
                    "snapshot); saving it would overwrite '%s' with gameplay state.",
                    path.c_str());
        return false;
    }

    m_pendingSaveScenePath = std::move(path);
    m_lastSaveResult = "queued";
    return true;
}

// Frame-boundary half of the save. Runs from ProcessPendingActions only.
void EditorLayer::ProcessPendingSaveScene()
{
    if (m_pendingSaveScenePath.empty()) return;

    const std::string path = std::move(m_pendingSaveScenePath);
    m_pendingSaveScenePath.clear();
    const std::uint64_t jobId     = std::exchange(m_pendingSaveJobId, 0);
    const std::uint32_t requestId = std::exchange(m_pendingSaveRequestId, 0);

    // A save the control pipe asked for is a job: the write's lines are
    // attributed to the request that queued it (so `log` finds them by its
    // #id) and captured for the job's error.
    std::vector<CapturedLogLine> captured;
    std::string errorCode;
    bool ok = false;
    Harness::Jobs::Start(jobId);
    if (jobId)
    {
        Logger::ScopedContext ctx(LogSource::Control, requestId, &captured, 64);
        ok = SaveSceneToPath(path, &errorCode);
    }
    else
        ok = SaveSceneToPath(path, &errorCode);
    m_lastSaveResult = ok ? "succeeded" : "failed";
    if (jobId)
    {
        if (ok)
        {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(Sys::U8Path(path), ec);
            nlohmann::json out{ { "path", path }, { "bytes", ec ? 0 : static_cast<std::uint64_t>(bytes) } };
            Harness::Jobs::Succeed(jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        }
        else
        {
            std::vector<std::string> lines;
            for (const CapturedLogLine& l : captured)
                lines.push_back(std::string(l.level == LogLevel::Error ? "ERROR: "
                                          : l.level == LogLevel::Warning ? "WARNING: " : "INFO: ")
                                + l.message);
            Harness::Jobs::Fail(jobId, errorCode.empty() ? "save_failed" : errorCode,
                                "the scene was NOT saved to '" + path + "'", std::move(lines));
        }
    }

    if (m_pendingSaveGuardsSceneOp)
    {
        m_pendingSaveGuardsSceneOp = false;
        if (!ok)
        {
            // The unsaved-changes modal's contract is "save, THEN do the destructive thing". The write runs in
            // this slot, so the modal queues the destructive op optimistically; a failed write must un-queue it
            // here, or a full disk / read-only file would throw away the edits the user asked to keep. This runs
            // before ProcessPendingNewScene and the load, so the cancel wins.
            m_pendingLoadScenePath.clear();
            m_pendingNewScene = false;
            m_exitApproved    = false;
            LOG_ERROR("EditorLayer: scene save failed — the queued New / Load / Exit "
                      "was CANCELLED and your edits are still unsaved.");
        }
    }
}

// Must run from ProcessPendingSaveScene (the ProcessPendingActions slot), never from an ImGui
// handler: ExportBakedProbeCubemaps reaches GraphicsDX12::SaveTextureCubeToITEX, which does
// FlushAndWait() and then records and submits its own one-shot command list on the graphics queue.
// Inside BeginImGuiFrame..EndImGuiFrame that list executes BEFORE this frame's already-recorded
// lists and re-states the probe cubemap array underneath them, and the INFINITE fence wait stalls
// with the primary command list open.
bool EditorLayer::SaveSceneToPath(const std::string& path, std::string* errorCode)
{
    if (path.empty() || !m_world)
    {
        if (errorCode) *errorCode = "no_world";
        return false;
    }

    // Play guard, enforced HERE as well as on the menu items: Ctrl+S has no menu item to grey out, and
    // overwriting a .iscene with runtime state is not recoverable (the save keeps no backup, and autosave
    // does not run in Play). Same test as UndoEnabled().
    if (GetPlayState() != ViewportPlayState::Stopped)
    {
        LOG_WARNING("EditorLayer: Save Scene refused — the world is in Play mode. "
                    "Play state is runtime data (Stop restores the pre-Play "
                    "snapshot); saving it would overwrite '%s' with gameplay state.",
                    path.c_str());
        if (errorCode) *errorCode = "play_mode";
        return false;
    }

    // Export baked (non-realtime) reflection-probe cubemaps to .dds beside the .iscene and stamp each
    // probe's bakedCubemapPath BEFORE serializing: SaveScene persists those paths, so the next load
    // restores the cubemaps off disk instead of re-baking.
    if (m_renderer)
        m_renderer->ExportBakedProbeCubemaps(*m_world, path);

    const bool ok = Resource::SaveScene(*m_world, path, "Scene",
                                        m_postProcessConfigPath,
                                        SceneNavMesh::Path(),
                                        m_sceneScriptPath, errorCode, m_sceneSystems);
    if (!ok)
    {
        // Deliberately keep m_sceneDirty set: a failed write means the edits are
        // still only in memory, and clearing the flag here would let the next
        // Load/quit throw them away without asking.
        LOG_ERROR("EditorLayer: failed to save scene to '%s' — changes are STILL unsaved",
                  path.c_str());
        return false;
    }

    m_currentScenePath = path;
    MarkSceneClean();
    AddRecentScene(path);   // Save As introduces new paths to the recent list
    LOG_SUCCESS("EditorLayer: saved scene '%s'", path.c_str());
    return true;
}

bool EditorLayer::SaveSceneNow()
{
    if (m_currentScenePath.empty()) return SaveSceneAs();
    // Returns "accepted", not "written to disk": the write happens in ProcessPendingSaveScene at the
    // frame boundary.
    return RequestSaveScene(m_currentScenePath);
}

// ==== Autosave / crash recovery (see the member block in EditorLayer.h) ====
namespace
{
    std::filesystem::path AutosaveDir()
    {
        return Sys::U8Path(Sys::WritableDataDir()) / "autosave";
    }
    std::string AutosaveStem(const std::string& scenePath)
    {
        return scenePath.empty()
            ? std::string("Untitled")
            : Sys::PathToUtf8(Sys::U8Path(scenePath).stem());
    }
}

void EditorLayer::MaybeAutosave()
{
    // Cheap per-frame gates; the write itself happens at most once per
    // m_autosaveIntervalSec (Editor Preferences > Autosave), and only while
    // there is actually unsaved work.
    if (!m_autosaveEnabled || !m_world || !m_sceneDirty) return;
    if (GetPlayState() != ViewportPlayState::Stopped) return;
    const double now = ImGui::GetTime();
    if (now - m_lastAutosaveTime < m_autosaveIntervalSec) return;
    m_lastAutosaveTime = now;
    WriteAutosave("periodic");
}

void EditorLayer::WriteAutosave(const char* reason)
{
    if (!m_world) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = AutosaveDir();
    fs::create_directories(dir, ec);

    const std::string stem = AutosaveStem(m_currentScenePath);

    // Rotate: 1 -> 2, 0 -> 1, newest write lands on 0.
    for (int i = kAutosaveKeep - 1; i >= 1; --i)
    {
        const fs::path from = dir / (stem + ".autosave" + std::to_string(i - 1) + ".iscene");
        const fs::path to   = dir / (stem + ".autosave" + std::to_string(i)     + ".iscene");
        if (fs::exists(from, ec)) { fs::remove(to, ec); fs::rename(from, to, ec); }
    }
    const std::string target =
        Sys::PathToUtf8(dir / (stem + ".autosave0.iscene"));

    // Straight Resource::SaveScene, deliberately not SaveSceneToPath: no probe-cubemap export (that
    // submits an out-of-band command list and writes .dds beside the file - an autosave stays a cheap,
    // silent, CPU-only serialize, which also makes it safe inline), and m_currentScenePath is not
    // stamped and m_sceneDirty STAYS SET - an autosave is a fallback, not a save.
    if (Resource::SaveScene(*m_world, target, "Scene", m_postProcessConfigPath,
                            SceneNavMesh::Path(),
                            m_sceneScriptPath, nullptr, m_sceneSystems))
        LOG_INFO("EditorLayer: autosave (%s) -> '%s'", reason, target.c_str());
    else
        LOG_WARNING("EditorLayer: autosave (%s) FAILED -> '%s'", reason, target.c_str());
}

std::string EditorLayer::NewerAutosaveFor(const std::string& scenePath) const
{
    if (scenePath.empty()) return {};
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path cand = AutosaveDir()
                        / (AutosaveStem(scenePath) + ".autosave0.iscene");
    if (!fs::exists(cand, ec) || ec) return {};

    const auto sceneT = fs::last_write_time(Sys::U8Path(scenePath), ec);
    if (ec) return Sys::PathToUtf8(cand);   // scene unreadable - offer anyway
    const auto asT = fs::last_write_time(cand, ec);
    if (ec) return {};
    return (asT > sceneT) ? Sys::PathToUtf8(cand) : std::string{};
}

void EditorLayer::RenderAutosaveRecoveryPopup()
{
    constexpr const char* kPopupName = "Autosave Recovery";
    if (m_autosavePopupOpenRequest)
    {
        ImGui::OpenPopup(kPopupName);
        m_autosavePopupOpenRequest = false;
    }
    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("An autosave newer than this scene exists.");
    ImGui::TextDisabled("The last session probably ended without saving.");
    ImGui::Spacing();
    ImGui::TextDisabled("Scene:    %s", m_pendingAutosaveOriginal.c_str());
    ImGui::TextDisabled("Autosave: %s", m_pendingAutosaveOffer.c_str());
    ImGui::Separator();

    if (ImGui::Button("Restore autosave", ImVec2(150.f, 0.f)))
    {
        // Load the autosave FILE; the deferred-load block re-points the scene
        // path at the original and re-marks dirty (Ctrl+S must write to the
        // real .iscene, never into the autosave directory).
        m_autosaveRestoreRepointPath = m_pendingAutosaveOriginal;
        m_pendingLoadScenePath       = m_pendingAutosaveOffer;
        m_pendingAutosaveOffer.clear();
        m_pendingAutosaveOriginal.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open saved scene", ImVec2(150.f, 0.f)))
    {
        m_pendingLoadScenePath = std::move(m_pendingAutosaveOriginal);
        m_pendingAutosaveOffer.clear();
        m_pendingAutosaveOriginal.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.f, 0.f))
        || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_pendingAutosaveOffer.clear();
        m_pendingAutosaveOriginal.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ==== Editor preferences (see the member block in EditorLayer.h) ====
// Flat key=value text on purpose: no schema, no library, greppable, and a corrupt line degrades to
// one ignored setting instead of a lost file.
namespace
{
    std::filesystem::path EditorPrefsPath()
    {
        // Per project: window layout, last-opened scene and the rest are
        // about THIS project, and two projects sharing one file means opening
        // the second silently rearranges the first.
        Sys::CreateDirs(Sys::ProjectDataDir());
        return Sys::U8Path(Sys::ProjectDataDir()) / "editor_prefs.ini";
    }
    // ImGuizmo enums are bitmasks; persist stable indices instead of raw values.
    int GizmoOpToIndex(ImGuizmo::OPERATION op)
    {
        return op == ImGuizmo::ROTATE ? 1 : op == ImGuizmo::SCALE ? 2 : 0;
    }
    ImGuizmo::OPERATION GizmoOpFromIndex(int i)
    {
        return i == 1 ? ImGuizmo::ROTATE : i == 2 ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE;
    }
}

void EditorLayer::OpenMorphEditor(Entity entity)
{
    if (!m_world || !m_world->IsAlive(entity)) return;
    FinishMorphAuthoredEdit();
    m_morphEditorTarget = m_world->MakeHandle(entity);
    m_morphEditorEpoch = m_world->Epoch();
    m_morphEditorOpen = m_morphEditorFocus = true;
}

void EditorLayer::FinishMorphAuthoredEdit()
{
    if (m_morphAuthoredBefore.empty()) return;
    if (m_world && m_world->Epoch() == m_morphEditorEpoch &&
        m_world->IsHandleValid(m_morphEditorTarget) &&
        m_morphAuthoredEntity == m_morphEditorTarget.entity && UndoEnabled())
    {
        const std::type_index type(typeid(FacialAnimationComponent));
        const std::string after = Editor::Undo::SerializeComponentBlob(
            GetComponentRegistry(), *m_world, m_morphAuthoredEntity, type);
        if (!after.empty() && after != m_morphAuthoredBefore)
            PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
                m_morphEditorTarget, type, m_morphAuthoredBefore, after, "Facial Animation"));
    }
    m_morphAuthoredBefore.clear();
    m_morphAuthoredEntity = NullEntity;
}

void EditorLayer::DrawMorphEditor()
{
    if (!m_morphEditorOpen) return;
    ImGui::SetNextWindowSize(ImVec2(560.f, 640.f), ImGuiCond_FirstUseEver);
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-editor_declutter_verify"))
        ImGui::SetNextWindowPos(ImVec2(1140.f, 60.f), ImGuiCond_Always);
#endif
    if (m_morphEditorFocus) { ImGui::SetNextWindowFocus(); m_morphEditorFocus = false; }
    if (ImGui::Begin("Morph / Face Editor", &m_morphEditorOpen))
    {
        if (!m_world || m_world->Epoch() != m_morphEditorEpoch ||
            !m_world->IsHandleValid(m_morphEditorTarget))
            ImGui::TextWrapped("The character is no longer available. Reopen Edit Morph / Face from its Inspector.");
        else
        {
            const Entity e = m_morphEditorTarget.entity;
            ImGui::TextWrapped("Character: %s", m_world->GetName(e).c_str());
            ImGui::TextWrapped("Edits update this scene character. Watch the result in the scene viewport.");
            if (GetPlayState() != ViewportPlayState::Stopped)
                ImGui::TextWrapped("Play mode: edits are temporary and revert on Stop.");
            if (ImGui::BeginTabBar("MorphFaceTabs"))
            {
                const auto draw = [&](const char* label, std::type_index type)
                {
                    if (!ImGui::BeginTabItem(label)) return;
                    auto it = m_componentEditors.find(type);
                    void* component = it == m_componentEditors.end() ? nullptr : it->second.fetch(*m_world, e);
                    if (component)
                        DrawComponentBodyWithUndo(e, type, [&]() { it->second.draw(component, m_world, e); });
                    else
                    {
                        ImGui::TextWrapped("Add this component to the character to edit it here.");
                        if (ImGui::Button("Add component"))
                        {
                            if (type == std::type_index(typeid(MorphComponent)))
                                AddComponentWithUndo(e, type, [](World& w, Entity ent) { w.AddComponent<MorphComponent>(ent, {}); });
                            else
                                AddComponentWithUndo(e, type, [](World& w, Entity ent) { w.AddComponent<FacialAnimationComponent>(ent, {}); });
                        }
                    }
                    ImGui::EndTabItem();
                };
                draw("Morph targets", std::type_index(typeid(MorphComponent)));
                draw("Facial animation", std::type_index(typeid(FacialAnimationComponent)));
                ImGui::EndTabBar();
            }
        }
    }
    ImGui::End();
    if (!m_morphEditorOpen || !ImGui::IsAnyItemActive()) FinishMorphAuthoredEdit();
}

EntityHandle EditorLayer::ChainColliderTarget() const
{
    if (!m_world) return NullEntityHandle;
    auto& preview = Editor::ChainPhysicsPreviewWindow::Get();
    const EntityHandle carrier = preview.SceneCarrierHandle();
    if (m_world->IsHandleValid(carrier)) return carrier;
    const EntityHandle root = preview.SceneTargetHandle();
    return m_world->IsHandleValid(root)
        ? m_world->MakeHandle(preview.CounterpartCarrier(*m_world, root.entity)) : NullEntityHandle;
}

void EditorLayer::DrawChainColliders()
{
    auto& preview = Editor::ChainPhysicsPreviewWindow::Get();
    const EntityHandle target = ChainColliderTarget();
    // The staged chain Apply and the scene's collider history have different
    // owners. A focused child routes Ctrl+Z to the scene history explicitly.
    ImGui::BeginChild("SceneCapsuleEditor", ImVec2(0.f, 360.f), true);
    m_chainColliderUndo = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (!m_world || !m_world->IsHandleValid(target))
        ImGui::TextWrapped("Use Selected (Scene), or open Edit Chains from a character, to edit its capsule colliders.");
    else
    {
        const Entity e = target.entity;
        ImGui::TextWrapped("Scene character: %s", m_world->GetName(e).c_str());
        ImGui::TextWrapped("Edits apply to this scene character immediately and update the preview. Ctrl+Z here uses scene undo; chain Apply / Revert affects chain parameters only.");
        if (!preview.SubjectInPlace())
            ImGui::TextWrapped("Prefab instance: use Save as Prefab to keep collider changes in a prefab asset.");
        const bool playing = GetPlayState() != ViewportPlayState::Stopped;
        ImGui::BeginDisabled(playing);
        if (playing) ImGui::TextWrapped("Stop Play to edit scene colliders.");
        const std::type_index type(typeid(CapsuleColliderComponent));
        if (auto* cc = m_world->GetComponent<CapsuleColliderComponent>(e))
        {
            auto it = m_componentEditors.find(type);
            if (it != m_componentEditors.end())
                DrawComponentBodyWithUndo(e, type, [&]() { it->second.draw(cc, m_world, e); });
#if defined(WITH_HARNESS) && WITH_HARNESS
            // Exercise a scene edit through the actual undo bracket, then
            // observe the simulation copy AFTER separate frame boundaries.
            if (Sys::CommandLine::Get().Has("-editor_declutter_verify") && cc->count > 0)
            {
                static int s_phase = 0;
                static uint64_t s_nextFrame = 120;
                static float s_before = 0.f, s_edited = 0.f;
                static size_t s_undoBefore = 0;
                if (m_editorFrame >= s_nextFrame && s_phase < 5 && !playing)
                {
                    auto* stage = m_world->GetComponent<CapsuleColliderComponent>(preview.StageChainEntity());
                    if (s_phase == 0)
                    {
                        s_before = cc->capsules[0].radius;
                        s_edited = s_before + 0.031f;
                        s_undoBefore = m_editHistory.UndoCount();
                        const ImGuiID id = ImGui::GetID("##collider_undo_probe");
                        DrawComponentBodyWithUndo(e, type, [&]() {
                            ImGui::SetActiveID(id, ImGui::GetCurrentWindow());
                            cc->capsules[0].radius = s_edited;
                        });
                        DrawComponentBodyWithUndo(e, type, [&]() { ImGui::ClearActiveID(); });
                        ENGINE_CHECK(m_editHistory.UndoCount() == s_undoBefore + 1,
                                     "Collider edit must create one scene undo entry");
                        OpenMorphEditor(e);
                        ENGINE_CHECK(m_world->HasComponent<MorphComponent>(e), "Morph editor probe needs a morph character");
                    }
                    else
                    {
                        const float expected = (s_phase == 2 || s_phase == 4) ? s_before : s_edited;
                        const bool synced = stage && stage->count == cc->count &&
                            std::abs(stage->capsules[0].radius - expected) < 0.00001f &&
                            std::abs(cc->capsules[0].radius - expected) < 0.00001f;
                        ENGINE_CHECK(synced, "Collider scene/stage sync failed at phase %d", s_phase);
                        LOG_INFO("[EDITOR_DECLUTTER] phase=%d sceneStageMatch=%d radius=%.4f morphOpen=%d",
                                 s_phase, synced ? 1 : 0, cc->capsules[0].radius, m_morphEditorOpen ? 1 : 0);
                        if (s_phase == 2) EditRedo(); else if (s_phase < 4) EditUndo();
                    }
                    ++s_phase;
                    s_nextFrame = m_editorFrame + 8;
                }
            }
#endif
        }
        else if (ImGui::Button("Add Capsule Colliders"))
            AddComponentWithUndo(e, type, [](World& w, Entity ent) { w.AddComponent<CapsuleColliderComponent>(ent, {}); });
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

void EditorLayer::SetDebugDrawSystem(DebugDrawSystem* sys)
{
    if (m_debugDraw == sys) return;
    m_debugDraw = sys;
    if (sys)
    {
        sys->iconMaxDistance = m_debugIconMaxDistance;
        sys->iconSizeScale = m_debugIconSizeScale;
    }
}

void EditorLayer::LoadEditorPrefs()
{
    const std::filesystem::path source = Editor::PersonalFiles::PrefsToRead(EditorPrefsPath());
    std::ifstream f(source);
    if (source.empty() || !f) return;   // no file and no seed - compiled-in defaults

    std::string line;
    while (std::getline(f, line))
    {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        const float       n = std::strtof(v.c_str(), nullptr);

        if      (k == "snapEnabled")     m_snapEnabled          = (v == "1");
        else if (k == "snapTranslate")   m_snapTranslate        = n;
        else if (k == "snapRotate")      m_snapRotate           = n;
        else if (k == "snapScale")       m_snapScale            = n;
        else if (k == "showEditorGrid")  m_showEditorGrid       = (v == "1");
        else if (k == "viewport.viewCube") m_showViewCube       = (v == "1");
        else if (k.rfind("keymap.", 0) == 0) m_keymapPrefs[k.substr(7)] = v;
        else if (k == "gridCellSize")    m_gridCellSize         = n;
        else if (k == "icons.maxDistance") m_debugIconMaxDistance = std::isfinite(n) ? std::clamp(n, 1.f, 10000.f) : 50.f;
        else if (k == "icons.sizeScale") m_debugIconSizeScale = std::isfinite(n) ? std::clamp(n, 0.1f, 3.f) : 1.f;
        else if (k == "gizmoOperation")  m_gizmoOperation       = GizmoOpFromIndex(static_cast<int>(n));
        else if (k == "gizmoMode")       m_gizmoMode            = (n != 0.f) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        else if (k == "assetViewMode")   m_assetViewMode        = static_cast<AssetViewMode>(
                                             std::clamp(static_cast<int>(n), 0, 2));
        else if (k == "showInternal")    m_showInternalEntities = (v == "1");
        else if (k == "showMeshSections") m_showMeshSections    = (v == "1");
        else if (k == "showProbeViz")    m_showProbeVizSpheres  = (v == "1");
        else if (k == "logShowSuccess")  m_logShowSuccess       = (v == "1");
        else if (k == "logShowInfo")     m_logShowInfo          = (v == "1");
        else if (k == "logShowWarning")  m_logShowWarning       = (v == "1");
        else if (k == "logShowError")    m_logShowError         = (v == "1");
        else if (k == "logAutoScroll")   m_logAutoScroll        = (v == "1");
        else if (k == "uiFontSize")      m_uiFontSize           = n;
        else if (k == "panel.hierarchy")      m_showHierarchy      = (v == "1");
        else if (k == "panel.inspector")      m_showInspector      = (v == "1");
        else if (k == "panel.contentBrowser") m_showContentBrowser = (v == "1");
        else if (k == "panel.outputLog")      m_showOutputLog      = (v == "1");
        // The registry's panels and the workspaces, applied once the panels are registered.
        else if (k.rfind("panel.", 0) == 0 || k.rfind("workspace.", 0) == 0) m_panelPrefs[k] = v;
        else if (k == "autosave")             m_autosaveEnabled    = (v == "1");
        else if (k == "statusBar")            m_showStatusBar      = (v == "1");
        else if (k == "autosaveMinutes")      m_autosaveIntervalSec =
                                                  std::clamp(static_cast<double>(n), 1.0, 60.0) * 60.0;
        // The editor half of the preview kill switch, default 1; the engine half is the DX12_PREVIEW
        // environment variable, and either being 0 means no PreviewRenderer is ever constructed.
        else if (k == "preview.enabled")
            Editor::MaterialPreviewWidget::Get().SetPrefEnabled(v == "1");
        // The viewport's Canvas Rect Tool and Screen UI toggles, and the UI Editor's per-canvas resolutions.
        else if (k == "viewport.uiRectTool")      m_uiRectToolInViewport    = (v == "1");
        else if (k == "viewport.screenUI")        m_showScreenUI            = (v == "1");
        // The Game View: what Esc does under the game's capture, and its framing.
        else if (k == "gameView.escReleasesCapture") Editor::GameView::Get().SetEscReleasesCapture(v != "0");
        else if (k == "gameView.preset")
        {
            for (int i = 0; i < Editor::GameViewLayout::kPresetCount; ++i)
                if (v == Editor::GameViewLayout::kPresets[i].label) Editor::GameView::Get().SetPreset(i);
        }
        else if (k.rfind("uieditor.", 0) == 0)    Editor::UIEditorWindow::Get().LoadPref(k, v);
        else if (k == "recent")
        {
            if (!v.empty() && m_recentScenes.size() < static_cast<size_t>(kMaxRecentScenes))
                m_recentScenes.push_back(v);
        }
        else if (k.rfind("bookmark", 0) == 0)
        {
            const int idx = std::atoi(k.c_str() + 8);
            if (idx < 0 || idx >= kCameraBookmarks) continue;
            CameraBookmark bm;
            if (sscanf_s(v.c_str(), "%f %f %f %f %f",
                         &bm.position.x, &bm.position.y, &bm.position.z,
                         &bm.yaw, &bm.pitch) == 5)
            {
                bm.valid = true;
                m_camBookmarks[idx] = bm;
            }
        }
    }
}

void EditorLayer::SaveEditorPrefs()
{
    // A harness run reads the seed, never the person's editor_prefs.ini, and writes nothing. AddRecentScene
    // saves on every scene load: packs that load worlds through the editor would push their scenes onto
    // the (capped) recent list and evict the developer's entries. Under -unattended the in-memory state
    // (Open Recent included) behaves normally and never reaches disk. The imgui.ini guard in OnAttach is
    // the same rule.
    if (Sys::CommandLine::Get().Has("-unattended")) return;

    std::error_code ec;
    std::filesystem::create_directories(EditorPrefsPath().parent_path(), ec);
    std::ofstream f(EditorPrefsPath(), std::ios::trunc);
    if (!f) return;   // shutdown path - never block on a failed prefs write

    f << "icons.maxDistance=" << (m_debugDraw ? m_debugDraw->iconMaxDistance : m_debugIconMaxDistance) << '\n'
      << "icons.sizeScale=" << (m_debugDraw ? m_debugDraw->iconSizeScale : m_debugIconSizeScale) << '\n';

    f << "snapEnabled="    << (m_snapEnabled ? 1 : 0)          << '\n'
      << "snapTranslate="  << m_snapTranslate                  << '\n'
      << "snapRotate="     << m_snapRotate                     << '\n'
      << "snapScale="      << m_snapScale                      << '\n'
      << "showEditorGrid=" << (m_showEditorGrid ? 1 : 0)       << '\n'
      << "gridCellSize="   << m_gridCellSize                   << '\n'
      << "gizmoOperation=" << GizmoOpToIndex(m_gizmoOperation) << '\n'
      << "gizmoMode="      << (m_gizmoMode == ImGuizmo::WORLD ? 1 : 0) << '\n'
      << "assetViewMode="  << static_cast<int>(m_assetViewMode) << '\n'
      << "showInternal="   << (m_showInternalEntities ? 1 : 0) << '\n'
      << "showMeshSections=" << (m_showMeshSections ? 1 : 0)   << '\n'
      << "showProbeViz="   << (m_showProbeVizSpheres  ? 1 : 0) << '\n'
      << "logShowSuccess=" << (m_logShowSuccess ? 1 : 0)       << '\n'
      << "logShowInfo="    << (m_logShowInfo    ? 1 : 0)       << '\n'
      << "logShowWarning=" << (m_logShowWarning ? 1 : 0)       << '\n'
      << "logShowError="   << (m_logShowError   ? 1 : 0)       << '\n'
      << "logAutoScroll="  << (m_logAutoScroll  ? 1 : 0)       << '\n';
    // Written only when it differs from the default (the tracked-file rule, see preview.enabled below):
    // a key that appears on the first exit after an upgrade is a repo diff nobody asked for.
    if (m_uiFontSize != EditorTheme::kDefaultFontSize)
        f << "uiFontSize=" << m_uiFontSize << '\n';
    if (!m_showViewCube) f << "viewport.viewCube=0\n";
    // Only a changed chord is written (the tracked-file rule); "" unbinds.
    for (const auto& [id, chord] : m_keymapPrefs) f << "keymap." << id << '=' << chord << '\n';
    // Core panels: only a CLOSED one is written (same tracked-file reason).
    if (!m_showHierarchy)      f << "panel.hierarchy=0\n";
    if (!m_showInspector)      f << "panel.inspector=0\n";
    if (!m_showContentBrowser) f << "panel.contentBrowser=0\n";
    if (!m_showOutputLog)      f << "panel.outputLog=0\n";
    // The registry's panels (a non-default open state only) and the workspaces; the keys of panels
    // this run did not register (a module that is off) are kept.
    {
        std::map<std::string, std::string> prefs = m_panelPrefs;
        if (m_panelsRegistered)
        {
            for (const Editor::PanelDesc& p : Editor::PanelRegistry::Get().All()) prefs.erase("panel." + p.id);
            for (auto it = prefs.begin(); it != prefs.end();)
                it = it->first.rfind("workspace.", 0) == 0 ? prefs.erase(it) : std::next(it);
            Editor::PanelRegistry::Get().WriteOpenStates(prefs);
            Editor::PanelRegistry::Get().WriteWorkspaces(prefs);
        }
        for (const auto& [key, value] : prefs) f << key << '=' << value << '\n';
    }
    if (!m_autosaveEnabled)    f << "autosave=0\n";
    if (!m_showStatusBar)      f << "statusBar=0\n";
    if (m_autosaveIntervalSec != kDefaultAutosaveIntervalSec)
        f << "autosaveMinutes=" << (m_autosaveIntervalSec / 60.0) << '\n';
    // preview.enabled is not written unconditionally. (1) editor_prefs.ini is tracked and the default is
    // on, so the key is written only when the file already carried it or something set it explicitly -
    // launching the editor must not produce a repo diff. (2) A -preview_* harness flag forces the pref
    // on for the run; PrefEnabledForSave returns the value from before the force, so a corpus run cannot
    // overwrite a developer's deliberate 0.
    {
        const auto& w = Editor::MaterialPreviewWidget::Get();
        if (w.PrefIsPersisted())
            f << "preview.enabled=" << (w.PrefEnabledForSave() ? 1 : 0) << '\n';
    }
    // Only the non-defaults (the tracked-file rule).
    if (m_uiRectToolInViewport) f << "viewport.uiRectTool=1\n";
    if (!m_showScreenUI)        f << "viewport.screenUI=0\n";
    if (!Editor::GameView::Get().EscReleasesCapture()) f << "gameView.escReleasesCapture=0\n";
    if (Editor::GameView::Get().Preset() != 0)
        f << "gameView.preset=" << Editor::GameViewLayout::kPresets[Editor::GameView::Get().Preset()].label << '\n';
    Editor::UIEditorWindow::Get().WritePrefs(f);
    for (const std::string& r : m_recentScenes)
        f << "recent=" << r << '\n';
    for (int i = 0; i < kCameraBookmarks; ++i)
        if (m_camBookmarks[i].valid)
            f << "bookmark" << i << '='
              << m_camBookmarks[i].position.x << ' '
              << m_camBookmarks[i].position.y << ' '
              << m_camBookmarks[i].position.z << ' '
              << m_camBookmarks[i].yaw   << ' '
              << m_camBookmarks[i].pitch << '\n';
}

void EditorLayer::AddRecentScene(const std::string& path)
{
    if (path.empty()) return;
    m_recentScenes.erase(
        std::remove(m_recentScenes.begin(), m_recentScenes.end(), path),
        m_recentScenes.end());
    m_recentScenes.insert(m_recentScenes.begin(), path);
    if (m_recentScenes.size() > static_cast<size_t>(kMaxRecentScenes))
        m_recentScenes.resize(kMaxRecentScenes);
    // Persist immediately: the recent list is the one pref whose loss after a
    // crash is actually felt, and the write is a handful of lines.
    SaveEditorPrefs();
}

// ==== Camera bookmarks ====
// Ctrl+Shift+1..4 store the free-cam pose, Ctrl+1..4 jump back. Perspective Edit mode only: the ortho
// views and Play's possessed camera are not the free-cam's pose to save.
namespace
{
    // The transient editor free camera (EditorCameraTag) - the same lookup FocusSelectedEntity does.
    Entity FindEditorCamera(World* world)
    {
        if (!world) return NullEntity;
        if (auto* tagPool = world->GetPool<EditorCameraTag>())
            for (Entity e : tagPool->Entities())
                if (world->IsAlive(e)) return e;
        return NullEntity;
    }
}

void EditorLayer::StoreCameraBookmark(int idx)
{
    if (idx < 0 || idx >= kCameraBookmarks || !m_world) return;
    const Entity cam = FindEditorCamera(m_world);
    if (cam == NullEntity) return;
    const LocalTransform* lt = m_world->GetComponent<LocalTransform>(cam);
    const CameraControllerComponent* cc = m_world->GetComponent<CameraControllerComponent>(cam);
    if (!lt || !cc) return;
    m_camBookmarks[idx] = { true, lt->translation, cc->yaw, cc->pitch };
    LOG_INFO("Editor: camera bookmark %d stored (Ctrl+%d recalls it)", idx + 1, idx + 1);
}

void EditorLayer::RestoreCameraBookmark(int idx)
{
    if (idx < 0 || idx >= kCameraBookmarks || !m_world) return;
    const CameraBookmark& bm = m_camBookmarks[idx];
    if (!bm.valid) return;
    const Entity cam = FindEditorCamera(m_world);
    if (cam == NullEntity) return;
    LocalTransform* lt = m_world->GetComponent<LocalTransform>(cam);
    CameraControllerComponent* cc = m_world->GetComponent<CameraControllerComponent>(cam);
    if (!lt || !cc) return;
    lt->translation = bm.position;
    cc->yaw   = bm.yaw;
    cc->pitch = bm.pitch;
    // Root entity: patch GlobalTransform so the jump shows this frame (the next
    // TransformSystem::Propagate reaffirms it), as FocusSelectedEntity does.
    if (GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(cam))
    {
        gt->matrix._41 = bm.position.x;
        gt->matrix._42 = bm.position.y;
        gt->matrix._43 = bm.position.z;
    }
}

void EditorLayer::TurnEditorCamera(float dx, float dy, const DirectX::XMFLOAT3& forward)
{
    const Entity cam = FindEditorCamera(m_world);
    if (cam == NullEntity) return;
    LocalTransform* lt = m_world->GetComponent<LocalTransform>(cam);
    CameraControllerComponent* cc = m_world->GetComponent<CameraControllerComponent>(cam);
    if (!lt || !cc) return;
    if (m_editorViewMode != EditorViewMode::Perspective)
    {
        Editor::ViewCube::YawPitchOf(forward, cc->yaw, cc->pitch);
        m_editorViewMode = EditorViewMode::Perspective;
    }
    // The cube turns with the mouse: drag right to see its left side, down to see its top.
    constexpr float kRadiansPerPixel = 0.008f;
    constexpr float kMaxPitch        = 1.5533430f;   // 89 degrees, the controller's range
    cc->yaw  += dx * kRadiansPerPixel;
    cc->pitch = std::clamp(cc->pitch + dy * kRadiansPerPixel, -kMaxPitch, kMaxPitch);
    // The rotation too: the controller re-derives yaw / pitch from the transform while idle.
    lt->rotation = CameraSystem::MakeTransform(*cc, lt->translation).rotation;
    if (GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(cam))
        DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());
}

bool EditorLayer::SaveSceneAs()
{
    // Checked before the (blocking, Win32-modal) dialog so Play mode doesn't
    // make the user pick a file only to be refused afterwards.
    if (GetPlayState() != ViewportPlayState::Stopped)
    {
        LOG_WARNING("EditorLayer: Save Scene As refused — press Stop first "
                    "(the played world is runtime state, not authoring data).");
        return false;
    }

    // Seed the dialog with the open scene's FOLDER (lpstrInitialDir wants a directory; a file path makes
    // the shell ignore it).
    const std::string initDir = m_currentScenePath.empty()
        ? std::string{}
        : Sys::PathToUtf8(Sys::U8Path(m_currentScenePath).parent_path());
    const std::string savePath = SaveFileDialog(
        "Scene (*.iscene)\0*.iscene\0All Files\0*.*\0\0",
        "iscene", initDir.c_str());
    if (savePath.empty()) return false;   // user cancelled
    // The file dialog stays inline: it is pure Win32 / CPU and the caller's "did the user commit?" answer
    // depends on it. Only the write (which touches GPU resources) is deferred.
    return RequestSaveScene(savePath);
}

void EditorLayer::RequestLoadScene(std::string path)
{
    if (path.empty() || !m_world) return;
    if (m_sceneDirty)
    {
        m_pendingSceneOp          = PendingSceneOp::Load;
        m_pendingSceneOpPath      = std::move(path);
        m_unsavedPopupOpenRequest = true;
        return;
    }
    // Crash recovery: an autosave newer than the file on disk means a session
    // ended without saving. Offer it before the load proceeds.
    if (std::string offer = NewerAutosaveFor(path); !offer.empty())
    {
        m_pendingAutosaveOffer       = std::move(offer);
        m_pendingAutosaveOriginal    = std::move(path);
        m_autosavePopupOpenRequest   = true;
        return;
    }
    m_pendingLoadScenePath = std::move(path);
}

void EditorLayer::RequestNewScene()
{
    if (m_sceneDirty)
    {
        m_pendingSceneOp          = PendingSceneOp::New;
        m_pendingSceneOpPath.clear();
        m_unsavedPopupOpenRequest = true;
        return;
    }
    m_pendingNewScene = true;
}

bool EditorLayer::RequestExit()
{
    // Every window's unsaved documents hold the exit too (their files are lost with the process; a
    // scene load / new scene keeps them).
    m_unsavedDocuments = CollectUnsavedDocuments();
    if (!m_sceneDirty && m_unsavedDocuments.empty()) return true;
    m_pendingSceneOp          = PendingSceneOp::Exit;
    m_pendingSceneOpPath.clear();
    m_unsavedPopupOpenRequest = true;
    return false;
}

void EditorLayer::CommitGesturesForPlay()
{
    if (m_uiRectDrag.active) CommitUIRectDrag();
    Editor::UIEditorWindow::Get().CommitGestures(UIEditorCtx());
}

bool EditorLayer::ConsumeExitApproved()
{
    const bool v = m_exitApproved;
    m_exitApproved = false;
    return v;
}

// Runs whatever the modal was blocking, once the user answered Save or Discard.
void EditorLayer::RunPendingSceneOp()
{
    const PendingSceneOp op = m_pendingSceneOp;
    m_pendingSceneOp = PendingSceneOp::None;

    switch (op)
    {
        // Both destructive ops stay deferred (ProcessPendingActions) even after confirmation: this still runs
        // inside the ImGui frame.
        case PendingSceneOp::Load: m_pendingLoadScenePath = std::move(m_pendingSceneOpPath); break;
        case PendingSceneOp::New:  m_pendingNewScene      = true;  break;
        case PendingSceneOp::Exit: m_exitApproved         = true;  break;
        case PendingSceneOp::None:
        default:                                                    break;
    }
    m_pendingSceneOpPath.clear();
}

void EditorLayer::RenderUnsavedChangesPopup()
{
    constexpr const char* kPopupName = "Unsaved Changes";
    if (m_unsavedPopupOpenRequest)
    {
        ImGui::OpenPopup(kPopupName);
        m_unsavedPopupOpenRequest = false;
    }

    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const char* what = "continuing";
    switch (m_pendingSceneOp)
    {
        case PendingSceneOp::Load: what = "loading another scene"; break;
        case PendingSceneOp::New:  what = "starting a new scene";  break;
        case PendingSceneOp::Exit: what = "closing the editor";    break;
        default: break;
    }
    // On exit, every window's unsaved documents are listed too (a scene switch keeps them), as
    // RequestExit collected them.
    const bool exiting = m_pendingSceneOp == PendingSceneOp::Exit;
    const std::vector<UnsavedDocument> noDocuments;
    const std::vector<UnsavedDocument>& docs = exiting ? m_unsavedDocuments : noDocuments;
    if (m_sceneDirty)
    {
        ImGui::Text("The current scene has unsaved changes.");
        ImGui::TextDisabled("They will be lost by %s.", what);
        ImGui::Spacing();
        ImGui::TextDisabled("%s", m_currentScenePath.empty()
                                      ? "Untitled scene (never saved)"
                                      : m_currentScenePath.c_str());
    }
    if (!docs.empty())
    {
        if (m_sceneDirty) ImGui::Spacing();
        ImGui::Text("Documents with unsaved edits:");
        bool panelOnly = false;
        for (const UnsavedDocument& d : docs)
        {
            ImGui::BulletText("%s - %s", d.owner.c_str(), d.name.c_str());
            if (!d.saveable)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(apply or discard it in its panel)");
                panelOnly = true;
            }
        }
        if (!m_sceneDirty) ImGui::TextDisabled("They will be lost by %s.", what);
        if (panelOnly) ImGui::TextDisabled("Save keeps the editor open while one of them is not committed.");
    }
    ImGui::Separator();

    // Saving stays Edit-mode-only here: the answer to "Play mode is dirty" is Stop, never a serialize of
    // the runtime world.
    const bool canSave = (GetPlayState() == ViewportPlayState::Stopped);
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("Save", ImVec2(110.f, 0.f)))
    {
        // An untitled scene has nowhere to write, so Save must prompt (otherwise the button would no-op and
        // then discard the edits). `queued` means accepted for the frame-boundary write in
        // ProcessPendingSaveScene, not on disk (see SaveSceneToPath); everything that can refuse the save
        // synchronously (Play mode, no path, cancelled dialog) still does.
        const bool sceneSave = m_sceneDirty;
        const bool queued = !sceneSave || (m_currentScenePath.empty() ? SaveSceneAs() : SaveSceneNow());
        if (queued)
        {
            // Arm the guards BEFORE queueing the destructive op: if a deferred
            // write fails on disk, ProcessPendingSaveDocuments / ProcessPendingSaveScene
            // cancel the op they were protecting instead of letting a failed save
            // discard the edits. The documents are written in PPA too: some of their
            // saves rebuild GPU state.
            if (sceneSave) m_pendingSaveGuardsSceneOp = true;
            if (!docs.empty()) m_pendingSaveDocuments = true;
            RunPendingSceneOp();
            ImGui::CloseCurrentPopup();
        }
        // Save refused / dialog cancelled: keep the modal up so the pending op
        // stays blocked rather than quietly proceeding.
    }
    ImGui::EndDisabled();
    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Press Stop first — the played world is runtime state,\n"
                          "saving it would overwrite the authoring .iscene.");

    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(110.f, 0.f)))
    {
        RunPendingSceneOp();
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.f, 0.f))
        || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_pendingSceneOp = PendingSceneOp::None;
        m_pendingSceneOpPath.clear();
        m_unsavedDocuments.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorLayer::EndImGuiFrame(RHI::CommandList cmd)
{
    if (!m_imguiBackendsReady || !m_gfx) return;
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    // Timestamped so this draw is measured: GPUProfiler's frame total is a union over instrumented
    // regions only, so unregioned work is invisible to it and to GPU.EffectiveFrameMs. Its own region
    // because the cost is editor-only and absent from Game.exe - it must be separable to answer how much
    // GPU budget the game has. Read this region; do not infer the cost from a utilization reading taken
    // at a different window size.
    const uint32_t r = m_gfx->BeginGPUTimestamp(cmd, "EditorImGuiDraw");
    m_imguiMgr.Render(dx12.GetNativeCommandList(cmd), dx12.GetCbvSrvUavAllocator());
    m_gfx->EndGPUTimestamp(cmd, r);
}

bool EditorLayer::PrepareImGuiFrameSnapshot(uint64_t frame)
{
    if (!m_imguiBackendsReady || !m_gfx) return true;
    if (m_hasPreparedImGuiFrame) throw std::logic_error("Editor frame snapshot already prepared");
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    m_preparedImGuiRecord = m_imguiMgr.PrepareRenderSnapshot(nullptr, dx12.GetCbvSrvUavAllocator(), frame);
    m_preparedImGuiFrame = frame;
    m_hasPreparedImGuiFrame = true;
    m_preparedImGuiHadSnapshot = m_imguiMgr.HasPreparedSnapshot();
    return !m_imguiMgr.SnapshotNeedsOwnerCallbacks();
}

void EditorLayer::DiscardImGuiFrameSnapshot()
{
    m_preparedImGuiRecord = {};
    m_hasPreparedImGuiFrame = false;
    m_imguiMgr.CompleteRenderSnapshot();
}

void EditorLayer::CompleteDeferredImGuiSnapshot(bool frameLost)
{
    // No-op unless a snapshot is in flight; CompleteRenderSnapshot refuses a
    // worker thread, and App calls this from main only.
    if (!m_deferSnapshotCompletion) return;
    // A snapshot PREPARED but not yet taken by the lane's EndImGuiFrameAsync belongs to the frame about
    // to be posted: the early UI build freezes it at the top of TickPhase::Render, before RenderHalf's
    // entry drain. Do not complete it there: ImGui would get its buffers back before the lane recorded
    // them and the editor would present with no UI. The lane takes it after the post and the next drain
    // gives it back; only a drain after which it can never be posted (unwinding a failed frame, or the
    // Quit drain) releases it here - shutdown refuses a snapshot still in flight.
    if (m_hasPreparedImGuiFrame)
    {
        if (!frameLost) return;
        m_preparedImGuiRecord = {};
        m_hasPreparedImGuiFrame = false;
    }
    m_imguiMgr.CompleteRenderSnapshot();
}

Sys::OwnerTask EditorLayer::EndImGuiFrameAsync(RHI::CommandList cmd, uint64_t frame, uint64_t sceneTexture)
{
    if (!m_imguiBackendsReady || !m_gfx) co_return;
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    const uint32_t region = m_gfx->BeginGPUTimestampNoTracy(cmd, "EditorImGuiDraw");
    m_gfx->BeginEventMarker(cmd, "EditorImGuiDraw");   // capture name (NoTracy opens none)
    // Under the render-owner lane the owner running this is NOT main: the
    // buffers stay with the snapshot and main completes it on its next drain
    // (CompleteDeferredImGuiSnapshot) - ImGui is never touched from the lane.
    struct Restore { ImGuiManager& manager; bool deferred;
                     ~Restore() { if (!deferred) manager.CompleteRenderSnapshot(); } }
        restore{ m_imguiMgr, m_deferSnapshotCompletion };
    std::function<void()> record;
    if (m_hasPreparedImGuiFrame) {
        if (m_preparedImGuiFrame != frame) throw std::logic_error("Editor snapshot frame mismatch");
        m_hasPreparedImGuiFrame = false;
        if (m_imguiMgr.HasPreparedSnapshot()) {
            m_imguiMgr.BindRenderSnapshot(dx12.GetNativeCommandList(cmd), frame, FrameSceneTexturePlaceholder, sceneTexture);
            record = std::move(m_preparedImGuiRecord);
        } else if (m_imguiMgr.SnapshotNeedsOwnerCallbacks()) {
            record = m_imguiMgr.PrepareRenderSnapshot(dx12.GetNativeCommandList(cmd), dx12.GetCbvSrvUavAllocator(), frame,
                FrameSceneTexturePlaceholder, sceneTexture);
        } else {
            // An EMPTY frame (minimized, no draw lists) prepares no snapshot and
            // legitimately lands here. One that DID freeze draw data and lost it
            // before this point draws no editor UI at all - loud, not a blank
            // window (see CompleteDeferredImGuiSnapshot).
            ENGINE_CHECK_ONCE(!m_preparedImGuiHadSnapshot,
                "[EDITORUI] frame %llu: the ImGui snapshot prepared for this frame was "
                "given back before the render owner recorded it - no editor UI is drawn",
                static_cast<unsigned long long>(frame));
        }
    } else {
        record = m_imguiMgr.PrepareRenderSnapshot(dx12.GetNativeCommandList(cmd), dx12.GetCbvSrvUavAllocator(), frame);
    }
    if (record) {
        co_await Sys::SubmitCpu([record = std::move(record), gfx = m_gfx, cmd, region] {
            gfx->BeginTracyGPUZone(cmd, "EditorImGuiDraw", region);
            try { record(); } catch (...) { gfx->EndTracyGPUZone(region); throw; }
            gfx->EndTracyGPUZone(region);
        });
    }
    m_gfx->EndEventMarker(cmd);
    m_gfx->EndGPUTimestampNoTracy(cmd, region);
}

void EditorLayer::OnAttach()
{
    // Prefs first: everything below (and every panel this frame) reads the
    // restored values instead of compiled-in defaults.
    LoadEditorPrefs();
    if (m_debugDraw)
    {
        m_debugDraw->iconMaxDistance = m_debugIconMaxDistance;
        m_debugDraw->iconSizeScale = m_debugIconSizeScale;
    }
    LOG_INFO("[EDITOR_PREFS] snapTranslate=%g recent=%zu%s", m_snapTranslate, m_recentScenes.size(),
             Sys::CommandLine::Get().Has("-unattended") ? " (read-only under -unattended: see SaveEditorPrefs)" : "");

    // The Sequencer window needs file dialogs for New / Open / Save As: hand it this TU's Win32 wrappers
    // as callbacks rather than duplicating them.
    m_sequencerWindow.SetFileDialogs(
        [] { return SaveFileDialog("Sequence (*.iseq)\0*.iseq\0", "iseq", "asset"); },
        [] { return OpenFileDialog("Sequence (*.iseq)\0*.iseq\0", "asset"); });

    // Create the ImGui context before any GetIO() call. Safe here: Window does not make one.
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }
    // The Profiler's plots (external/implot/README.md).
    if (!ImPlot::GetCurrentContext()) ImPlot::CreateContext();

    // Where the panels are, for the harness's app_ui captures (System/UiWindows.h): the top-level
    // windows ImGui laid out this frame. Sampled on the main thread at PostRender, after the frame's UI
    // was built, so the rects match that frame's picture.
    Sys::UiWindows::SetProvider([]
    {
        std::vector<Sys::UiWindows::Window> out;
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g) return out;
        const ImGuiViewport* mainVp = ImGui::GetMainViewport();
        // g->Windows is in display order, back to front; a docked window is
        // drawn with its dock tree's host, so it takes the host's place.
        std::unordered_map<const ImGuiWindow*, int> orderOf;
        for (int i = 0; i < g->Windows.Size; ++i) orderOf[g->Windows[i]] = i;
        for (int i = 0; i < g->Windows.Size; ++i)
        {
            const ImGuiWindow* w = g->Windows[i];
            if (!w || !w->Active || w->Hidden || !w->Name) continue;
            if (w->Flags & ImGuiWindowFlags_Tooltip) continue;
            // A docked panel IS flagged a child (BeginDocked adds ChildWindow:
            // it lives in its dock host); any other child is part of a panel.
            if ((w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive) continue;
            if (w->Name[0] == '#' && w->Name[1] == '#') continue;         // ImGui's own: dock hosts, ...
            if (w->DockIsActive && !w->DockTabIsVisible) continue;       // a tab behind another
            if (mainVp && w->Viewport && w->Viewport->ID != mainVp->ID) continue;   // a platform window
            Sys::UiWindows::Window win;
            win.name  = w->Name;
            win.label = win.name.substr(0, win.name.find("##"));
            win.x = w->Pos.x - (mainVp ? mainVp->Pos.x : 0.f);
            win.y = w->Pos.y - (mainVp ? mainVp->Pos.y : 0.f);
            win.w = w->Size.x;
            win.h = w->Size.y;
            const ImGuiWindow* host = w->RootWindowDockTree ? w->RootWindowDockTree : w;
            const auto it = orderOf.find(host);
            win.order     = it != orderOf.end() ? it->second : i;
            win.focused   = g->NavWindow && g->NavWindow->RootWindow == w->RootWindow;
            win.collapsed  = w->Collapsed;
            win.docked     = w->DockIsActive;
            // An overlay hides nothing: ImGuizmo's full-window "gizmo" draws
            // with a transparent WindowBg and lets the mouse through - the flag
            // it has is NoMouseInputs, not NoBackground.
            win.background = !(w->Flags & (ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMouseInputs));
            out.push_back(std::move(win));
        }
        return out;
    });

    // Wire the concrete per-NotifyCategory property panels into the registry the NotifyTrackEditor
    // consults. Idempotent: re-registering a category overwrites its drawer, so re-attaching the editor
    // layer is safe.
    RegisterDefaultNotifyEditors();

    ImGuiIO& io = ImGui::GetIO();

    // The harness does not inherit a human's window layout (imgui.ini, a personal file). Reading it: ImGui hides a docked window whose host dockspace is skipped or degenerate
    // (DockSpace() falls back to KeepAliveOnly, BeginDocked leaves DockTabIsVisible false, Begin()
    // returns false), and -hidden minimises the window to a ~32x29 dockspace, so a leaf deep in the split
    // tree gets no area; every preview panel then reads "Begin returned false" as "the user folded this
    // tab" and hands its view back - a pack's result would depend on where a developer last docked a
    // panel. Writing it: the run would save the minimised layout (Size=15,29 rectangles) over the
    // developer's.
    // So under -unattended the layout is the compiled-in default (SetupDockSpace's DockBuilder tree plus
    // floating panels) and nothing is persisted. -imguiini opts back in, for a human reproducing a pack
    // against their own layout by hand (a pack carrying it would depend on that layout).
    m_harnessImGuiLayout = Sys::CommandLine::Get().Has("-unattended") &&
                           !Sys::CommandLine::Get().Has("-imguiini");
    if (m_harnessImGuiLayout) io.IniFilename = nullptr;
    else Editor::PersonalFiles::UseImGuiIni(io);
    // The Chain Physics Preview's layout mode and split ratios live in imgui.ini: the handler must exist
    // before the first NewFrame reads the file (inert when IniFilename is null, as above).
    Editor::ChainPhysicsPreviewWindow::RegisterSettingsHandler();
    LOG_INFO("[EDITOR_LAYOUT] ini=%s source=%s",
             io.IniFilename ? io.IniFilename : "(none)",
             m_harnessImGuiLayout ? "builtin-unattended" : "file");

    // Style + typography live in EditorTheme (see its header for the font
    // chain: Noto Sans -> Font Awesome (whole PUA) -> Noto Sans TC -> JP).
    EditorTheme::ApplyStyle();

    // The font SIZE is a user preference, but under the harness layout it is
    // pinned to the compiled-in default for the same reason imgui.ini is
    // ignored above: the menu-bar and toolbar heights follow the font size,
    // and a pack's viewport rectangle must not depend on how large a developer
    // likes their editor text.
    if (m_harnessImGuiLayout)
    {
        m_uiFontSize = EditorTheme::kDefaultFontSize;
        // Same rule for the core panels: a developer who closed the Inspector
        // must not turn every material-inspector pack red.
        m_showHierarchy = m_showInspector = m_showContentBrowser = m_showOutputLog = true;
        m_autosaveEnabled     = true;
        m_autosaveIntervalSec = kDefaultAutosaveIntervalSec;
        // The status bar costs the dock space a row and carries a live fps readout - both would move what
        // packs measure. Opt in explicitly.
        m_showStatusBar = Sys::CommandLine::Get().Has("-editor_statusbar");
        // The Output Log's level filters are preferences too: a developer who hid Info and Success would cut
        // what a pack reading the panel (editor_log_panel's `filtered`) sees from ~2000 rows to 4.
        m_logShowSuccess = m_logShowInfo = m_logShowWarning = m_logShowError = true;
        m_logAutoScroll  = true;
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Output Log self-check: the panel stays open so its mirror is pumped every frame, and
    // ReleasePreviewViews compares it with the logger's ring at shutdown. The _break arm drops one line
    // on purpose.
    m_logPanelVerify = Sys::CommandLine::Get().Has("-logpanel_verify") ||
                       Sys::CommandLine::Get().Has("-logpanel_verify_break");
    if (m_logPanelVerify)
    {
        m_showOutputLog = true;
        m_outputLog.SetBreakVerifyForReverseArm(Sys::CommandLine::Get().Has("-logpanel_verify_break"));
    }
    // -console_exec=<editor frame>:<lua>, repeatable (every argv occurrence is read, in order - the
    // parsed command line keeps only the last). Submitted through SubmitConsoleLine on that editor frame.
    {
        for (const std::string& v : Sys::CommandLine::Get().GetAll("-console_exec"))
        {
            const std::size_t colon = v.find(':');
            char* end = nullptr;
            const unsigned long long f =
                colon == std::string::npos ? 0ull : std::strtoull(v.c_str(), &end, 10);
            if (colon == std::string::npos || colon == 0 || end != v.c_str() + colon ||
                colon + 1 >= v.size())
                ENGINE_CHECK(false, "-console_exec=%s: expected <editor frame>:<lua>", v.c_str());
            else
                m_harnessConsoleExecs.emplace_back(f, v.substr(colon + 1));
        }
    }
    m_consoleContextOff = Sys::CommandLine::Get().Has("-console_context_off");
#endif
    const EditorTheme::Fonts& fonts = EditorTheme::LoadFonts(m_uiFontSize);
    m_uiFontSize          = ImGui::GetStyle().FontSizeBase;   // clamped value
    m_hasFontAwesomeIcons = fonts.icons;
    s_faIconsLoaded       = fonts.icons;   // file-scope mirror for DrawEntityTree
}

void EditorLayer::SetSceneTextureId(unsigned long long texId)
{
    m_sceneTextureId = texId;
}

void EditorLayer::GetViewportSize(unsigned int& outWidth, unsigned int& outHeight) const
{
    outWidth = m_viewportSizeX;
    outHeight = m_viewportSizeY;
}

bool EditorLayer::ConsumeWantsStepFrame()
{
    bool v = m_wantsStepFrame;
    m_wantsStepFrame = false;
    return v;
}

// Project the authoritative EngineMode onto the editor's 3-state view: Edit -> Stopped, Paused ->
// Paused, Playing / Stepping -> Playing. Falls back to the local flag when no controller is wired
// (App always wires one).
ViewportPlayState EditorLayer::GetPlayState() const
{
    if (!m_playModeController) return m_playState;
    switch (m_playModeController->Mode())
    {
        case EngineMode::Edit:   return ViewportPlayState::Stopped;
        case EngineMode::Paused: return ViewportPlayState::Paused;
        case EngineMode::Playing:
        case EngineMode::Stepping:
        default:                 return ViewportPlayState::Playing;
    }
}

void EditorLayer::SetPickResult(Entity entity)
{
    // PickingPass has no visibility test (it draws the Opaque / Transparent lists without viewMask or
    // renderInMainPass), so an editor camera flown down to y=-4096 would pick the Chain Physics Preview
    // stage the main view never draws. Such a click resolves as if it hit nothing.
    if (entity != NullEntity && m_world && m_world->IsAlive(entity) &&
        m_world->HasComponent<PreviewStageTag>(entity))
        entity = NullEntity;

    const SelectMode mode = m_pickSelectMode;
    m_pickSelectMode      = SelectMode::Replace;   // one-shot

    // Ctrl / Shift-clicking empty space must NOT wipe the selection: the one gesture where "picked
    // nothing" means "leave it alone" rather than "deselect".
    Entity selection = entity;
    // Material drops always target the actual hit mesh. Ordinary clicks first
    // select the owning object, then drill into its mesh on subsequent clicks.
    if (entity != NullEntity && m_world && (m_world->HasComponent<MeshComponent>(entity)
            || m_world->HasComponent<MeshLibRef>(entity) || m_world->HasComponent<MeshHandle>(entity))
        && m_pendingMatDropPath.empty())
    {
        const Entity root = ViewportSelectionRoot(entity);
        const bool insideSelectedObject = m_selectedEntity != NullEntity
            && ViewportSelectionRoot(m_selectedEntity) == root;
        // The drill-down stops at a StaticMesh owner: its derived sections are
        // hidden, rebuilt at will and never saved.
        selection = mode == SelectMode::Replace && insideSelectedObject ? SelectableEntity(entity) : root;
        // The second click on a Static Mesh names the SECTION it hit: the Inspector flashes that material
        // slot.
        if (mode == SelectMode::Replace && insideSelectedObject)
            if (const MeshSectionOf* so = m_world->GetComponent<MeshSectionOf>(entity); so && so->owner == selection)
            {
                m_slotFlashOwner  = so->owner;
                m_slotFlashIndex  = so->index;
                m_slotFlashAt     = ImGui::GetTime();
                m_slotFlashScroll = true;
            }
    }
    if (!(selection == NullEntity && mode != SelectMode::Replace))
        SelectEntity(selection, mode);
    if (m_selectedEntity != NullEntity)
    {
        m_hierarchyRevealEntity = m_selectedEntity;
        m_hierarchyHighlight = m_selectedEntity;
        m_hierarchyStructureDirty = true;
    }

    m_hasPendingDrag = false;
    m_isDragging     = false;

    // If a material was dropped, apply it to the picked entity's MaterialComponent.
    if (!m_pendingMatDropPath.empty())
    {
        const std::string matPath = std::move(m_pendingMatDropPath);
        m_pendingMatDropPath.clear();

        const MeshSectionOf* section = (m_world && entity != NullEntity && m_world->IsAlive(entity))
                                     ? m_world->GetComponent<MeshSectionOf>(entity) : nullptr;
        if (section && SelectableEntity(entity) == section->owner)
        {
            // A derived StaticMesh section: the material is the OWNER's, per
            // slot - written on the section itself it would be lost at the next
            // rebuild and never saved.
            const Entity   owner = section->owner;
            const uint64_t key   = section->sectionKey;
            std::string why;
            if (AssignStaticMeshSlot(owner, key, matPath, &why))
                LOG_SUCCESS("EditorLayer: applied material '%s' -> slot %s of StaticMesh entity %u",
                            Sys::RelativizeAssetPath(matPath).c_str(), Resource::MeshKeyToHex(key).c_str(), owner);
            else
                LOG_ERROR("EditorLayer: failed to apply material '%s' to a StaticMesh slot (%s)",
                          matPath.c_str(), why.c_str());
        }
        else if (m_world && entity != NullEntity)
        {
            // AssignMaterial is the one assignment path, and it is undoable.
            std::string why;
            if (AssignMaterial(entity, matPath, &why))
                LOG_SUCCESS("EditorLayer: applied material '%s' → entity %u",
                            Sys::RelativizeAssetPath(matPath).c_str(), entity);
            else if (why.rfind("no_material", 0) == 0)
                LOG_WARNING("EditorLayer: drop target entity %u has no MaterialComponent", entity);
            else
                LOG_ERROR("EditorLayer: failed to apply material '%s' (%s)", matPath.c_str(), why.c_str());
        }
        else
            LOG_WARNING("EditorLayer: material drop — no entity under cursor");
    }
}

// The material state of one entity, as a RestoreFn over value copies of the four components an
// assignment touches. "Absent" is part of the state: a restore removes what was not there.
namespace
{
    template <class T>
    void RestoreOptional(World& w, Entity e, const std::optional<T>& v)
    {
        if (v)
        {
            if (T* c = w.GetComponent<T>(e)) *c = *v;
            else                             w.AddComponent<T>(e, T(*v));
        }
        else if (w.GetComponent<T>(e))
            w.RemoveComponent<T>(e);
    }

    template <class T>
    std::optional<T> CopyOptional(World& w, Entity e)
    {
        if (const T* c = w.GetComponent<T>(e)) return *c;
        return std::nullopt;
    }

    Editor::Undo::RestoreFn CaptureMaterialState(World& w, Entity e)
    {
        auto mat  = CopyOptional<MaterialComponent>(w, e);
        auto src  = CopyOptional<MaterialSourcePath>(w, e);
        auto inst = CopyOptional<MaterialInstanceInfo>(w, e);
        auto ovr  = CopyOptional<MaterialOverride>(w, e);
        return [mat, src, inst, ovr](World& world, Entity ent)
        {
            RestoreOptional(world, ent, mat);
            RestoreOptional(world, ent, src);
            RestoreOptional(world, ent, inst);
            RestoreOptional(world, ent, ovr);
            // The GPU copy follows only a DIRTY component.
            if (MaterialComponent* m = world.GetComponent<MaterialComponent>(ent)) m->SetDirty();
        };
    }
}

bool EditorLayer::AssignMaterial(Entity e, const std::string& path, std::string* error)
{
    auto fail = [error](std::string why) { if (error) *error = std::move(why); return false; };
    if (!m_world || e == NullEntity || !m_world->IsAlive(e))
        return fail("stale_handle: the entity is not alive");
    MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(e);
    if (!mat)
        return fail("no_material: the entity has no MaterialComponent (assign to a mesh entity, "
                    "not a prefab root)");

    // Two rules: publish the MaterialLoadInfo (ApplyMaterialLoadInfo) - without it a .imatinst leaves the
    // previous material's MaterialInstanceInfo in place, so the Inspector's "instance of:" row, the
    // override ticks and "Break Link" describe the wrong parent; and store the project-root-relative
    // form - a raw browser path hard-codes one machine's layout into the scene. Loaded into a copy first:
    // a file that does not load changes nothing.
    const std::string matRef = Sys::RelativizeAssetPath(path);
    MaterialComponent loaded = *mat;
    Resource::MaterialLoadInfo info;
    if (!Resource::LoadMaterial(matRef, loaded, &info))
        return fail("load_failed: '" + matRef + "' did not load as a material");

    const Editor::Undo::RestoreFn before = CaptureMaterialState(*m_world, e);
    *mat = std::move(loaded);
    mat->SetDirty();
    ApplyMaterialLoadInfo(*m_world, e, info);
    if (MaterialSourcePath* src = m_world->GetComponent<MaterialSourcePath>(e)) src->path = matRef;
    else m_world->AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ matRef });
    if (m_world->GetComponent<MaterialOverride>(e))
        m_world->RemoveComponent<MaterialOverride>(e);
    const Editor::Undo::RestoreFn after = CaptureMaterialState(*m_world, e);

    // Edit mode: an undo entry, which also dirties the scene (PushEdit). Play
    // mode: runtime state Stop discards - neither.
    if (UndoEnabled())
        PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(m_world->MakeHandle(e), before, after,
                                                                 "Material"));
    return true;
}

bool EditorLayer::ClearMaterial(Entity e, std::string* error)
{
    auto fail = [error](std::string why) { if (error) *error = std::move(why); return false; };
    if (!m_world || e == NullEntity || !m_world->IsAlive(e))
        return fail("stale_handle: the entity is not alive");
    MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(e);
    if (!mat) return fail("no_material: the entity has no MaterialComponent");

    // The engine's default material and no asset: what a new MaterialComponent
    // is. AssignMaterial's value copies make it one undo step.
    const Editor::Undo::RestoreFn before = CaptureMaterialState(*m_world, e);
    *mat = MaterialComponent{};
    mat->SetDirty();
    if (m_world->GetComponent<MaterialSourcePath>(e))   m_world->RemoveComponent<MaterialSourcePath>(e);
    if (m_world->GetComponent<MaterialInstanceInfo>(e)) m_world->RemoveComponent<MaterialInstanceInfo>(e);
    if (m_world->GetComponent<MaterialOverride>(e))     m_world->RemoveComponent<MaterialOverride>(e);
    const Editor::Undo::RestoreFn after = CaptureMaterialState(*m_world, e);
    if (UndoEnabled())
        PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(m_world->MakeHandle(e), before, after,
                                                                 "Clear Material"));
    return true;
}

// ==== Material Editor entity tabs ====
void EditorLayer::SyncEntityMaterialOverride(Entity e)
{
    MaterialComponent* mat = m_world ? m_world->GetComponent<MaterialComponent>(e) : nullptr;
    if (!mat) return;
    static const bool s_overrideOff = Sys::CommandLine::Get().Has("-mateditor_entity_override_off");
    const MaterialSourcePath* src = m_world->GetComponent<MaterialSourcePath>(e);
    // No file (an inline material), or the reverse arm: the edit stays an unsaved MaterialComponent edit
    // - what the scene save diffs, and what a reload of the file would overwrite.
    if (s_overrideOff || !src || src->path.empty()) { mat->SetEditDirty(); return; }

    // Against the FILE: another tab's live version of it is not what the scene saves.
    MaterialOverride ovr;
    MaterialComponent file{};
    {
        const Resource::LiveMaterialBypass fileNotLive;
        if (!Resource::BuildMaterialOverride(*mat, src->path, ovr) || !Resource::LoadMaterial(src->path, file))
        {
            mat->SetEditDirty();   // the file does not load: keep it an edit, the save's diff will say so
            return;
        }
    }
    if (ovr.IsEmpty())
    {
        if (m_world->GetComponent<MaterialOverride>(e)) m_world->RemoveComponent<MaterialOverride>(e);
    }
    else if (MaterialOverride* have = m_world->GetComponent<MaterialOverride>(e))
        *have = std::move(ovr);
    else
        m_world->AddComponent<MaterialOverride>(e, std::move(ovr));

    // The override holds the edit - unless it changed what no override key can
    // carry (a custom shader, its parameters or textures): then it stays an
    // unsaved edit, so a live push does not drop it.
    auto sameTextures = [](const MaterialComponent& a, const MaterialComponent& b)
    {
        if (a.customTextures.size() != b.customTextures.size()) return false;
        for (const auto& [name, t] : a.customTextures)
        {
            const auto it = b.customTextures.find(name);
            if (it == b.customTextures.end() || it->second.name != t.name || it->second.uvset != t.uvset) return false;
        }
        return true;
    };
    const bool representable = mat->useCustomShader == file.useCustomShader &&
                               mat->customShaderPath == file.customShaderPath &&
                               mat->customVertexShaderPath == file.customVertexShaderPath &&
                               mat->customParams == file.customParams && sameTextures(*mat, file);
    mat->SetAssetDirty(!representable);
}

bool EditorLayer::WriteEntityMaterial(const EntityHandle& h, const MaterialComponent& staged, bool first, bool last)
{
    if (!m_world || !m_world->IsHandleValid(h)) { m_matTabBefore = {}; return false; }
    const Entity e = h.entity;
    MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(e);
    if (!mat) { m_matTabBefore = {}; return false; }
    if (first || !m_matTabBefore) m_matTabBefore = CaptureMaterialState(*m_world, e);
    *mat = staged;
    SyncEntityMaterialOverride(e);
    mat->SetDirty();
    if (last)
    {
        if (UndoEnabled())
            PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(m_world->MakeHandle(e), std::move(m_matTabBefore),
                                                                     CaptureMaterialState(*m_world, e), "Material"));
        m_matTabBefore = {};
        const MaterialOverride* ovr = m_world->GetComponent<MaterialOverride>(e);
        std::vector<std::string> keys;
        if (ovr) for (const auto& [k, v] : ovr->props) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        std::string list;
        for (const std::string& k : keys) list += (list.empty() ? "" : ",") + k;
        LOG_INFO("[MATEDITOR] entity %s: overrides=%zu keys=%s unsaved=%d", m_world->GetName(e).c_str(), keys.size(),
                 list.empty() ? "-" : list.c_str(), mat->IsAssetDirty() ? 1 : 0);
    }
    return true;
}

bool EditorLayer::MaterialEntityAction(const EntityHandle& h, const std::string& action, const std::string& path,
                                       std::string& why)
{
    if (!m_world || !m_world->IsHandleValid(h)) { why = "stale_handle: the entity is gone"; return false; }
    const Entity e = h.entity;
    Editor::MaterialRowResult a;
    a.path = path;
    if (action == "promote")     a.action = Editor::MaterialRowAction::PromoteOverrides;
    else if (action == "clear")  a.action = Editor::MaterialRowAction::ClearOverrides;
    else if (action == "apply")
    {
        // The entity's overrides written into its file: the file as it is on
        // disk, the bag applied, saved as the same kind (an instance keeps its
        // explicit set, plus these keys) - then the entity reads the file again.
        const MaterialSourcePath* src = m_world->GetComponent<MaterialSourcePath>(e);
        const MaterialOverride*   ovr = m_world->GetComponent<MaterialOverride>(e);
        if (!src || src->path.empty()) { why = "no_asset: an inline material has no file"; return false; }
        if (!ovr || ovr->IsEmpty())    { why = "nothing_to_apply: the entity overrides nothing"; return false; }
        const std::string file = src->path;
        MaterialComponent mat{};
        Resource::MaterialLoadInfo info;
        {
            const Resource::LiveMaterialBypass fileNotLive;
            if (!Resource::LoadMaterial(file, mat, &info)) { why = "load_failed: '" + file + "'"; return false; }
        }
        Resource::ApplyMaterialOverride(mat, *ovr);
        bool ok = false;
        if (info.parentPath.empty())
            ok = Resource::SaveMaterial(mat, file);
        else
        {
            std::unordered_set<std::string> keys = info.overrideKeys;
            for (const auto& [k, v] : ovr->props) keys.insert(k);
            ok = Resource::SaveMaterialInstance(file, info.parentPath, mat, &keys, &info.unsetKeys);
        }
        if (!ok) { why = "save_failed: '" + file + "' was not written (see the log)"; return false; }
        const std::size_t applied = ovr->props.size();       // the bag is gone after the re-point
        if (!AssignMaterial(e, file, &why)) return false;   // reads it back; the bag goes (one undo step)
        Resource::ReloadMaterialsAffectedBy(*m_world, file);
        InvalidateMaterialThumbnails(file);
        LOG_INFO("[MATEDITOR] apply %s -> %s: keys=%zu", m_world->GetName(e).c_str(), file.c_str(), applied);
        return true;
    }
    else { why = "bad_value: '" + action + "' is promote, apply or clear"; return false; }
    return RunMaterialRowAction(e, 0, a, &why);
}

bool EditorLayer::AssignStaticMeshSlot(Entity owner, uint64_t sectionKey, const std::string& path,
                                       std::string* error, bool recordUndo)
{
    auto fail = [error](std::string why) { if (error) *error = std::move(why); return false; };
    if (!m_world || owner == NullEntity || !m_world->IsAlive(owner))
        return fail("stale_handle: the entity is not alive");
    StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(owner);
    if (!sm) return fail("no_static_mesh: the entity has no StaticMeshComponent");

    const std::string ref = path.empty() ? std::string() : Sys::RelativizeAssetPath(path);
    if (!ref.empty())
    {
        MaterialComponent probe{};
        if (!Resource::LoadMaterial(ref, probe))
            return fail("load_failed: '" + ref + "' did not load as a material");
    }

    // Value copies of the whole component: undo puts the authored fields back
    // and MeshInstanceSystem, seeing them differ from what the sections were
    // derived from, rebuilds the sections - no extra notification needed.
    const StaticMeshComponent before = *sm;
    sm->slotOverrides.erase(std::remove_if(sm->slotOverrides.begin(), sm->slotOverrides.end(),
                                           [sectionKey](const StaticMeshComponent::SlotOverride& o)
                                           { return o.sectionKey == sectionKey; }),
                            sm->slotOverrides.end());
    if (!ref.empty()) sm->slotOverrides.push_back({ sectionKey, ref });
    const StaticMeshComponent after = *sm;

    auto restoreTo = [](StaticMeshComponent v) -> Editor::Undo::RestoreFn
    {
        return [v](World& w, Entity e) { if (auto* c = w.GetComponent<StaticMeshComponent>(e)) *c = v; };
    };
    if (UndoEnabled() && recordUndo)
        PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(m_world->MakeHandle(owner), restoreTo(before),
                                                                 restoreTo(after), "Material Slot"));
    return true;
}

void EditorLayer::DrawStaticMeshInspector(StaticMeshComponent& sm, World* w, Entity e)
{
    if (!w) return;
    // Widgets that ACTIVATE (the text fields, the combos, the buttons) are
    // recorded by DrawComponentWithUndo's bracket; a drop activates nothing,
    // so each drop records its own undo entry.
    auto recordDrop = [this, w, e](const StaticMeshComponent& before, const char* label)
    {
        const StaticMeshComponent* now = w->GetComponent<StaticMeshComponent>(e);
        if (!now || !UndoEnabled()) return;
        auto restoreTo = [](StaticMeshComponent v) -> Editor::Undo::RestoreFn
        {
            return [v](World& ww, Entity ee) { if (auto* c = ww.GetComponent<StaticMeshComponent>(ee)) *c = v; };
        };
        PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(w->MakeHandle(e), restoreTo(before),
                                                                 restoreTo(*now), label));
    };

    // ---- Model ----
    char modelBuf[512];
    strncpy_s(modelBuf, sizeof(modelBuf), sm.model.c_str(), _TRUNCATE);
    if (ImGui::InputText("Model (.iscn)", modelBuf, sizeof(modelBuf), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        sm.model = Sys::RelativizeAssetPath(modelBuf);
        sm.meshKey = 0;   // a new model: the key is chosen below (or by name)
    }
    // A Mesh item (Content Browser) swaps model AND mesh; a model keeps the
    // name for the auto-pick below.
    auto acceptMeshRef = [&]()
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("STATICMESH_REF"))
        {
            std::string model, name;
            uint64_t    key = 0;
            if (ParseStaticMeshRef(static_cast<const char*>(p->Data), model, key, name))
            {
                const StaticMeshComponent before = sm;
                sm.model    = Sys::RelativizeAssetPath(model);
                sm.meshKey  = key;
                sm.meshName = name;
                recordDrop(before, "Static Mesh");
            }
        }
    };
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ISCN_PATH"))
        {
            const StaticMeshComponent before = sm;
            sm.model   = Sys::RelativizeAssetPath(static_cast<const char*>(p->Data));
            sm.meshKey = 0;
            recordDrop(before, "Static Mesh Model");
        }
        acceptMeshRef();
        ImGui::EndDragDropTarget();
    }

    MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
    const MeshAssetRegistry::Model* model = (reg && !sm.model.empty()) ? &reg->GetModel(sm.model, w->Epoch()) : nullptr;

    // A model and no key yet (just dropped): take the first placeable mesh, or
    // the one whose name the component still carries.
    if (model && sm.meshKey == 0 && !model->meshes.empty())
    {
        const MeshAssetRegistry::Mesh* pick = sm.meshName.empty() ? nullptr
                                            : MeshAssetRegistry::Find(*model, 0, sm.meshName);
        for (const MeshAssetRegistry::Mesh& m : model->meshes)
            if (!pick && !(m.flags & Resource::MESH_IDENT_HIDDEN)) pick = &m;
        if (pick) { sm.meshKey = pick->key; sm.meshName = pick->name; }
    }

    // ---- Mesh ----
    const MeshAssetRegistry::Mesh* mesh = model ? MeshAssetRegistry::Find(*model, sm.meshKey, sm.meshName) : nullptr;
    const std::string current = sm.meshName.empty() ? Resource::MeshKeyToHex(sm.meshKey)
                                                    : sm.meshName + "  (" + Resource::MeshKeyToHex(sm.meshKey) + ")";
    if (ImGui::BeginCombo("Mesh", current.c_str()))
    {
        if (!model || model->meshes.empty())
            ImGui::TextDisabled(model && !model->error.empty() ? model->error.c_str() : "(no meshes)");
        else
            for (const MeshAssetRegistry::Mesh& m : model->meshes)
            {
                const bool selected = (m.key == sm.meshKey);
                if ((m.flags & Resource::MESH_IDENT_HIDDEN) && !selected) continue;   // collision / mirror variants
                const std::string label = m.name + "  [" + std::to_string(m.sections.size()) + " slot" +
                                          (m.sections.size() == 1 ? "" : "s") + "]##" + Resource::MeshKeyToHex(m.key);
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    sm.meshKey  = m.key;
                    sm.meshName = m.name;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget())
    {
        acceptMeshRef();
        ImGui::EndDragDropTarget();
    }
    if (mesh && mesh->synthesized && ImGui::IsItemHovered())
        ImGui::SetTooltip("Legacy library: these keys are positional (the backfill writes the same ones).\n"
                          "Run  python tools/asset_tool.py identity <library> --apply  to store them.");

    // ---- Collision ----
    int collision = static_cast<int>(sm.collision);
    if (ImGui::Combo("Collision", &collision, "Asset\0None\0"))
        sm.collision = static_cast<StaticMeshComponent::Collision>(collision);

    if (!sm.status.empty())
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%s", sm.status.c_str());

    // The Mesh itself (its view modes, UVs and DEFAULT slots - what every instance without an override
    // draws).
    if (mesh && ImGui::Button("Open in Mesh Editor"))
        Editor::MeshEditorWindow::Get().RequestOpen(sm.model, mesh->key, mesh->name);

    // ---- Material slots ----
    if (mesh)
    {
        ImGui::SeparatorText("Material Slots");
        for (uint32_t k = 0; k < mesh->sections.size(); ++k)
        {
            const MeshAssetRegistry::Section& sec = mesh->sections[k];
            ImGui::PushID(static_cast<int>(k));
            const StaticMeshComponent::SlotOverride* ovr = sm.FindOverride(sec.key);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%u  %s", k, sec.slot.empty() ? "(unnamed)" : sec.slot.c_str());
            // A second viewport click on this section: scroll to its row once and flash it for a moment.
            if (m_slotFlashOwner == e && m_slotFlashIndex == k)
            {
                const double age = ImGui::GetTime() - m_slotFlashAt;
                if (age >= 0.0 && age < 1.5)
                {
                    if (m_slotFlashScroll) { ImGui::SetScrollHereY(0.3f); m_slotFlashScroll = false; }
                    const int a = static_cast<int>(255.0 * (1.0 - age / 1.5));
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                        IM_COL32(255, 210, 70, a), 2.f, 0, 2.f);
                }
            }
            // The slot's material as the Inspector's material row (thumbnail, asset field, type, warnings,
            // Edit). A widget's change is recorded by this inspector's undo bracket (DrawComponentWithUndo); a
            // drop activates nothing and records its own.
            const std::string slotPath = ovr ? ovr->material : sec.defaultMaterial;
            Editor::MaterialRowInfo row = MaterialSlotRowFor(e, sec.key, slotPath, ovr != nullptr);
            row.thumb = MaterialRowThumb(slotPath);
            const Editor::MaterialRowResult act = Editor::DrawMaterialRow("##slotrow", row);
            if (act.action != Editor::MaterialRowAction::None)
            {
                std::string why;
                if (!RunMaterialRowAction(e, sec.key, act, &why, /*recordUndo=*/act.dropped) &&
                    !why.empty() && why != "cancelled")
                    LOG_WARNING("Static Mesh: slot '%s' - %s", sec.slot.c_str(), why.c_str());
            }
            ImGui::PopID();
        }
    }

    // ---- Derived sections ----
    size_t derived = 0;
    if (const Children* ch = w->GetComponent<Children>(e))
        for (Entity c : ch->entities)
            if (const MeshSectionOf* so = w->GetComponent<MeshSectionOf>(c); so && so->owner == e) ++derived;
    ImGui::TextDisabled("%zu section entit%s derived (hidden in the Hierarchy unless \"Sections\")",
                        derived, derived == 1 ? "y" : "ies");
}

Entity EditorLayer::CreateStaticMeshEntity(const std::string& model, uint64_t meshKey,
                                           const std::string& meshName, const DirectX::XMFLOAT3& position)
{
    if (!m_world) return NullEntity;
    const Entity e = m_world->CreateEntity();
    m_world->SetName(e, meshName.empty() ? "Static Mesh" : meshName);
    LocalTransform lt{};
    lt.translation = position;
    m_world->AddComponent<LocalTransform>(e, lt);
    m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
    m_world->AddComponent<VisibilityComponent>(e, VisibilityComponent{});
    StaticMeshComponent sm;
    sm.model    = Sys::RelativizeAssetPath(model);
    sm.meshKey  = meshKey;
    sm.meshName = meshName;
    m_world->AddComponent<StaticMeshComponent>(e, std::move(sm));
    return e;
}

bool EditorLayer::ParseStaticMeshRef(const char* data, std::string& model, uint64_t& key, std::string& name)
{
    if (!data) return false;
    const std::string s(data);
    const size_t a = s.find('\n');
    const size_t b = (a == std::string::npos) ? std::string::npos : s.find('\n', a + 1);
    if (a == std::string::npos || b == std::string::npos) return false;
    model = s.substr(0, a);
    name  = s.substr(b + 1);
    return !model.empty() && Resource::MeshKeyFromHex(std::string_view(s).substr(a + 1, b - a - 1), key);
}

// What the Mesh Editor needs from this class.
Editor::MeshEditorContext EditorLayer::MeshEditorCtx()
{
    Editor::MeshEditorContext c;
    c.renderer    = m_renderer;
    c.gfx         = m_gfx;
    c.editorFrame = m_editorFrame;
    c.playing     = GetPlayState() != ViewportPlayState::Stopped;
    c.hasWorld    = m_world != nullptr;
    c.worldEpoch  = m_world ? m_world->Epoch() : 0u;
    c.filesChanged = [this](const std::vector<std::string>& paths) { NotifyThumbnailDependencyChanged(paths); };
    c.reveal = [this](const std::string& path)
    {
        SetCurrentAssetDir(Sys::PathToUtf8(Sys::U8Path(path).parent_path()));
        m_showContentBrowser = true;
    };
    c.collisionChanged = [this](const std::string& model) -> std::uint32_t { return ModelCollisionChanged(model); };
    return c;
}

// What the Texture Viewer needs from this class.
Editor::TextureViewerContext EditorLayer::TextureViewerCtx()
{
    Editor::TextureViewerContext c;
    c.renderer    = m_renderer;
    c.gfx         = m_gfx;
    c.textures    = m_textureSys;
    c.resources   = m_resourceMgr;
    c.editorFrame = m_editorFrame;
    c.referencingFiles = [this](const std::string& path, std::vector<std::string>& out)
    {
        if (!m_refScanner.Built()) m_refScanner.Rebuild(m_assetDir);
        for (const Editor::AssetReferenceScanner::Referrer& r : m_refScanner.FindReferrers(path))
            out.push_back(r.file);
    };
    c.reveal = [this](const std::string& path)
    {
        SetCurrentAssetDir(Sys::PathToUtf8(Sys::U8Path(path).parent_path()));
        m_showContentBrowser = true;
    };
    // The viewer's Reimport: the one texture import, run now (its ProcessPendingActions slot is the
    // serial window ImportTextureFile needs).
    c.reimport = [this](const std::string& itex, const std::string& source,
                        const Resource::TextureImportSettings& settings, std::string& why) -> bool
    {
        std::error_code ec;
        if (source.empty() || !std::filesystem::is_regular_file(Sys::U8Path(source), ec))
        {
            why = source.empty() ? "no source image" : "the source is not there: " + source;
            return false;
        }
        PendingTextureImport item;
        item.sourcePath      = source;
        item.outputPath      = itex;
        item.selectedFormat  = settings.format;
        item.suggestedFormat = settings.format;
        item.options         = settings;
        const TextureImportOutcome r = ImportTextureFile(item);
        if (!r.ok) why = r.message;
        return r.ok;
    };
    return c;
}

// What the UI Editor needs from this class.
Editor::UIEditorContext EditorLayer::UIEditorCtx()
{
    Editor::UIEditorContext c;
    c.renderer    = m_renderer;
    c.gfx         = m_gfx;
    c.world       = m_world;
    c.editorFrame = m_editorFrame;
    c.playing     = GetPlayState() != ViewportPlayState::Stopped;
    c.selected    = m_selectedEntity;
    c.assets      = m_assetMgr;
    // Scene tabs share the editor's selection.
    c.select      = [this](Entity e) { SelectEntity(e, SelectMode::Replace); };
    // Every edit below runs with the ACTIVE tab's EditSink in scope - an asset tab's ring and dirty file;
    // a scene tab has none, so its edits are the scene's. Looked up per call: the active tab can change
    // between two callbacks of one Draw.
    auto sinkNow = [] { return Editor::UIEditorWindow::Get().ActiveSink(); };
    // The scene's own edit operations, each one step on the editor's undo stack.
    c.pushEdit    = [this, sinkNow](std::unique_ptr<Editor::Undo::IEditAction> a)
    {
        SinkScope s(*this, sinkNow());
        PushEdit(std::move(a));
    };
    c.moveUINode  = [this, sinkNow](Entity child, Entity parent, int index, std::string* why)
    {
        SinkScope s(*this, sinkNow());
        return MoveUINode(child, parent, index, why);
    };
    c.deleteEntity = [this, sinkNow](Entity e)
    {
        SinkScope s(*this, sinkNow());
        DeleteEntities({ e });
    };
    c.rename      = [this, sinkNow](Entity e, const std::string& name)
    {
        if (!m_world || !m_world->IsAlive(e)) return;
        const std::string before = m_world->GetName(e);
        if (before == name) return;
        SinkScope s(*this, sinkNow());
        m_world->SetName(e, name);
        if (UndoEnabled())
            PushEdit(std::make_unique<Editor::Undo::RenameAction>(m_world->MakeHandle(e), before, name));
        MarkHierarchyDirty();
    };
    // What the UI Editor's panels do (spawn, duplicate), each one undo step with the active tab's sink
    // in scope.
    c.recordSpawn = [this, sinkNow](const char* label, const std::function<void()>& spawn)
    {
        SinkScope s(*this, sinkNow());
        RecordSpawn(label, spawn);
    };
    c.duplicate   = [this, sinkNow](Entity e)
    {
        SinkScope s(*this, sinkNow());
        return DuplicateEntity(e);
    };
    // A UI template (.ipfb) as it is - no parent, no adoption: a read-only tab's stage.
    c.loadPrefab  = [this](const std::string& path, std::string* why) -> Entity
    {
        if (!m_world || !m_assetMgr) { if (why) *why = "the editor has no asset manager"; return NullEntity; }
        const Entity root = Resource::LoadPrefab(Sys::RelativizeAssetPath(path), *m_world, *m_assetMgr, m_renderer,
                                                 m_animClipSys);
        if (root == NullEntity && why) *why = "'" + path + "' did not load (see the log)";
        return root;
    };
    // Undo / redo the active asset tab's ring with this editor's context (the world, the serializers,
    // the renderer's slot hooks).
    c.sinkUndo    = [this, sinkNow](bool redo) -> bool
    {
        const std::shared_ptr<Editor::Undo::EditSink> sink = sinkNow();
        if (!sink || !m_world || !UndoEnabled()) return false;
        if (redo ? !sink->history.CanRedo() : !sink->history.CanUndo()) return false;
        SinkScope s(*this, sink);
        SyncedEditContext();
        if (redo) sink->history.Redo();
        else      sink->history.Undo();
        MarkHierarchyDirty();
        MarkSceneDirty();   // = the sink's changed()
        return true;
    };
    // A UI template lands as Editor.InstantiatePrefab lands one: a detached copy on a UI parent. Anything
    // else is refused and destroyed - nothing is left.
    c.instantiate = [this](const std::string& path, Entity parent, std::string* why) -> Entity
    {
        auto fail = [why](std::string s) { if (why) *why = std::move(s); return NullEntity; };
        if (!m_world || !m_assetMgr) return fail("the editor has no asset manager");
        std::error_code ec;
        if (!std::filesystem::exists(Sys::U8Path(path), ec)) return fail("no such file: " + path);
        const Entity root = Resource::LoadPrefab(Sys::RelativizeAssetPath(path), *m_world, *m_assetMgr, m_renderer,
                                                 m_animClipSys);
        if (root == NullEntity) return fail("'" + path + "' did not load (see the log)");
        std::string refusal;
        if (!UI::IsUINode(*m_world, root))
            refusal = "'" + path + "' is no UI template (its root has no UIRect)";
        else if (m_world->HasComponent<UI::UICanvas>(root))
            refusal = "a canvas template is a root: it takes no parent";
        else
            AdoptUIPrefabRoot(root, parent, &refusal);
        if (!refusal.empty())
        {
            Hierarchy::DestroySubtree(*m_world, root);
            return fail(refusal);
        }
        return root;
    };
    c.drawComponent = [this, sinkNow](Entity e, std::type_index type)
    {
        SinkScope s(*this, sinkNow());
        return DrawComponentSection(e, type);
    };
    c.drawWithUndo  = [this, sinkNow](Entity e, std::type_index type, const std::function<void()>& body)
    {
        SinkScope s(*this, sinkNow());
        DrawComponentBodyWithUndo(e, type, body);
    };
    c.addComponent  = [this, sinkNow](Entity e, std::type_index type)
    {
        const auto it = m_componentEditors.find(type);
        SinkScope s(*this, sinkNow());   // the deferred add keeps it (AddComponentWithUndo)
        if (it != m_componentEditors.end() && it->second.add) AddComponentWithUndo(e, type, it->second.add);
    };
    c.beginMacro = [this, sinkNow](const std::string& label)
    {
        SinkScope s(*this, sinkNow());
        EditTarget().BeginMacro(label);
    };
    c.endMacro   = [this, sinkNow]
    {
        SinkScope s(*this, sinkNow());
        EditTarget().EndMacro();
    };
    return c;
}

// What the Material Editor needs from this class.
Editor::MaterialEditorContext EditorLayer::MaterialEditorCtx()
{
    Editor::MaterialEditorContext c;
    c.renderer    = m_renderer;
    c.world       = m_world;
    c.editorFrame = m_editorFrame;
    c.gfx         = m_gfx;           // texture nodes' thumbnails (the Content Browser's streaming)
    c.textures    = m_textureSys;
    c.resources   = m_resourceMgr;
    // A save is pushed at everything already using the file (and its instances)
    // and re-tiles its thumbnails - the asset inspector's Save does the same two.
    c.materialSaved = [this](const std::string& path)
    {
        if (m_world) Resource::ReloadMaterialsAffectedBy(*m_world, path);
        InvalidateMaterialThumbnails(path);
    };
    // A tab's live version changed or ended: the entities reading the file re-materialize from the
    // overlay (or the file again). Quiet, and an entity with unsaved edits of its own keeps them. A
    // material tile reads the file (LiveMaterialBypass); an asset tile composed while the file was live
    // is re-tiled when the live period ends.
    c.liveChanged = [this](const std::string& path, bool ended)
    {
        if (m_world) Resource::ReloadMaterialsAffectedBy(*m_world, path, /*live=*/true);
        if (ended) InvalidateMaterialThumbnails(path);
    };
    c.assign = [this](const std::string& name, const std::string& path, std::string& why) -> bool
    {
        if (!m_world) { why = "no editor world"; return false; }
        for (Entity e : m_world->GetEntities())
            if (m_world->IsAlive(e) && m_world->GetName(e) == name) return AssignMaterial(e, path, &why);
        why = "no entity named '" + name + "'";
        return false;
    };
    // An entity tab's edits land in its entity (one scene undo step per gesture), and its buttons.
    c.entityWrite = [this](const EntityHandle& h, const MaterialComponent& staged, bool first, bool last) -> bool
    {
        return WriteEntityMaterial(h, staged, first, last);
    };
    c.entityAction = [this](const EntityHandle& h, const std::string& action, const std::string& path,
                            std::string& why) -> bool
    {
        return MaterialEntityAction(h, action, path, why);
    };
    c.reveal = [this](const std::string& path)
    {
        SetCurrentAssetDir(Sys::PathToUtf8(Sys::U8Path(path).parent_path()));
        m_showContentBrowser = true;
    };
    return c;
}

// What the Skeleton Editor needs from this class.
Editor::SkeletonEditorContext EditorLayer::SkeletonEditorCtx()
{
    Editor::SkeletonEditorContext c;
    c.renderer    = m_renderer;
    c.gfx         = m_gfx;
    // The Animation host's stage spawns into this World (only in ProcessPendingActions).
    c.world       = m_world;
    c.assetMgr    = m_assetMgr;
    c.animClipSys = m_animClipSys;
    c.buildBones  = [this](Renderer& r, World& w, Entity e, std::vector<DirectX::XMFLOAT4X4>& b2m,
                           DirectX::XMMATRIX& ent) { return BuildBoneWorldMatrices(r, w, e, b2m, ent); };
    // The stage clip's playhead runs on the REAL frame delta (Edit mode's animDt is 0).
    c.realDt      = ImGui::GetIO().DeltaTime;
    c.editorFrame = m_editorFrame;
    c.hasWorld    = m_world != nullptr;
    c.worldEpoch  = m_world ? m_world->Epoch() : 0u;
    c.playing     = GetPlayState() != ViewportPlayState::Stopped;
    c.reveal = [this](const std::string& path)
    {
        SetCurrentAssetDir(Sys::PathToUtf8(Sys::U8Path(path).parent_path()));
        m_showContentBrowser = true;
    };
    // A Save re-derives every placed instance; Attach puts the selection on a socket.
    c.socketsChanged  = [this](const std::string& model) -> std::uint32_t { return ModelSocketsChanged(model); };
    c.attachSelection = [this](const std::string& model, const std::string& socket) -> std::string
    {
        ReconcileSelection();
        if (!m_world || m_selectedEntity == NullEntity || !m_world->IsAlive(m_selectedEntity))
            return "select the entity to attach in the scene first";
        std::string why;
        const Entity target = NearestModelInstance(m_selectedEntity, model, socket, why);
        return target == NullEntity ? why : AttachToSocket(m_selectedEntity, target, socket);
    };
    // Placed instances take a model's saved virtual bones when they spawn again.
    c.staleVirtualBones = [this](const std::string& model) -> std::uint32_t
    {
        if (!m_world || !m_renderer) return 0u;
        return SceneInstanceLoader::CountStaleVirtualBones(*m_world, m_renderer->GetSkeletonRegistry(), model);
    };
    c.reloadScene = [this]() -> bool
    {
        if (!m_world || m_currentScenePath.empty()) return false;
        RequestLoadScene(m_currentScenePath);   // the unsaved-changes prompt applies
        return true;
    };
    // A saved humanoid profile reaches the registry's copies; the Retarget window aimed at a placement of
    // the model.
    c.humanoidChanged = [this](const std::string& iskel, const HumanoidProfile& hp, const SkeletonAsset& fileSkel) -> std::uint32_t
    { return ModelHumanoidChanged(iskel, hp, fileSkel); };
    c.openRetarget = [this](const std::string& model) -> std::string { return OpenRetargetFor(model); };
    return c;
}

uint32_t EditorLayer::ModelSocketsChanged(const std::string& model)
{
    if (!m_world || !m_renderer) return 0u;
    return SceneInstanceLoader::RefreshModelSockets(*m_world, m_renderer->GetSkeletonRegistry(), model);
}

Entity EditorLayer::NearestModelInstance(Entity item, const std::string& model, const std::string& socket,
                                         std::string& why)
{
    if (!m_world) { why = "no world"; return NullEntity; }
    const std::string want = Resource::AssetRefs::NormalizeRefPath(model);
    const uint32_t    hash = SocketComponent::HashName(socket.c_str());
    DirectX::XMFLOAT3 at   = { 0.f, 0.f, 0.f };
    if (const GlobalTransform* g = m_world->GetComponent<GlobalTransform>(item))
        at = { g->matrix._41, g->matrix._42, g->matrix._43 };
    Entity best = NullEntity;
    float  bestD2 = FLT_MAX;
    std::size_t placed = 0;
    m_world->ForEach<SceneSourcePath>([&](Entity e, SceneSourcePath& s)
    {
        if (Resource::AssetRefs::NormalizeRefPath(s.path) != want) return;
        // A panel's preview stage (the Skeleton Editor's own model among them) is not a placement - never a
        // follower's target.
        if (m_world->HasComponent<PreviewStageTag>(e)) return;
        ++placed;
        const SocketComponent* sc = m_world->GetComponent<SocketComponent>(e);
        if (!sc || sc->FindByName(hash) == SocketComponent::kNotFound) return;
        // Never the item's own model (it cannot follow itself).
        for (Entity p = item; p != NullEntity;)
        {
            if (p == e) return;
            const Parent* par = m_world->GetComponent<Parent>(p);
            p = par ? par->entity : NullEntity;
        }
        // Nor one that follows the item: they would follow each other.
        if (FollowsEntity(*m_world, e, item)) return;
        float d2 = 0.f;
        if (const GlobalTransform* g = m_world->GetComponent<GlobalTransform>(e))
        {
            const float dx = g->matrix._41 - at.x, dy = g->matrix._42 - at.y, dz = g->matrix._43 - at.z;
            d2 = dx * dx + dy * dy + dz * dz;
        }
        if (d2 < bestD2) { bestD2 = d2; best = e; }
    });
    if (best == NullEntity)
        why = placed == 0 ? "no instance of '" + model + "' is placed in the scene"
                          : "no placed instance of '" + model + "' has a socket '" + socket + "' (save the model first)";
    return best;
}

bool EditorLayer::FollowsEntity(World& world, Entity e, Entity item)
{
    // Up every link that places `e`: its parents and what it (or they) follow.
    std::vector<Entity> stack{ e };
    std::unordered_set<Entity> seen;
    while (!stack.empty() && seen.size() < 4096)
    {
        const Entity x = stack.back();
        stack.pop_back();
        if (x == NullEntity || !seen.insert(x).second) continue;
        if (x == item) return true;
        if (const Parent* p = world.GetComponent<Parent>(x)) stack.push_back(p->entity);
        if (const FollowSocketComponent* fs = world.GetComponent<FollowSocketComponent>(x))
            stack.push_back(fs->target.Resolve(world));
        if (const FollowEntityComponent* fe = world.GetComponent<FollowEntityComponent>(x))
            stack.push_back(fe->target.Resolve(world));
    }
    return false;
}

uint32_t EditorLayer::ModelHumanoidChanged(const std::string& iskel, const HumanoidProfile& hp,
                                           const SkeletonAsset& fileSkel)
{
    if (!m_world || !m_renderer) return 0u;
    SkeletonRegistry& reg  = m_renderer->GetSkeletonRegistry();
    const std::string want = Resource::AssetRefs::NormalizeRefPath(iskel);
    // Each model file's skeleton line, read once per distinct model.
    std::unordered_map<std::string, bool> names;
    std::vector<Entity> roots;
    m_world->ForEach<SceneSourcePath>([&](Entity e, SceneSourcePath& s)
    {
        const std::string key = Resource::AssetRefs::NormalizeRefPath(s.path);
        auto it = names.find(key);
        if (it == names.end())
        {
            const std::string k = SceneInstanceLoader::ModelSkeletonPath(Sys::RelativizeAssetPath(s.path));
            it = names.emplace(key, !k.empty() && Resource::AssetRefs::NormalizeRefPath(k) == want).first;
        }
        if (it->second) roots.push_back(e);
    });
    // Only a skeleton whose real bones ARE the file's (count and names) takes the profile's bone
    // indices; one registered before a re-import changed the rig gets the file's at its next spawn (an
    // index past it would read out of bounds).
    const uint32_t fileReal = fileSkel.RealBoneCount();
    auto sameRig = [&](const SkeletonAsset& s)
    {
        if (s.RealBoneCount() != fileReal) return false;
        for (uint32_t b = 0; b < fileReal; ++b)
            if (std::strncmp(s.boneNames[b], fileSkel.boneNames[b], sizeof(s.boneNames[b])) != 0) return false;
        return true;
    };
    uint32_t updated = 0, stale = 0;
    for (Entity root : roots)
    {
        const Entity carrier = Anim::ResolveAnimEntityOrSelf(*m_world, root);
        const SkeletonComponent* sc = m_world->GetComponent<SkeletonComponent>(carrier);
        if (!sc || sc->assetIndex == kInvalidSkeletonIndex || sc->assetIndex >= reg.Count()) continue;
        if (!sameRig(reg.Get(sc->assetIndex))) { ++stale; continue; }
        reg.Get(sc->assetIndex).humanoid = hp;   // a shared entry: every sharer is this .iskel's
        // An Upper / Lower Body layer's cached mask was built from the old profile.
        if (AnimLayerComponent* layers = m_world->GetComponent<AnimLayerComponent>(carrier))
            for (int i = 0; i < layers->layerCount && i < AnimLayerComponent::MAX_LAYERS; ++i)
                layers->layers[i].maskSkel = ~0u;
        ++updated;
    }
    LOG_INFO("[SKELEDITOR] humanoid: '%s' - %u model root(s) in the world take the saved profile (valid=%u "
             "userEdited=%u; %u with another version of the rig skipped)", iskel.c_str(), updated, hp.valid,
             hp.userEdited, stale);
    return updated;
}

std::string EditorLayer::OpenRetargetFor(const std::string& model)
{
    if (!m_world) return "no world";
    if (GetPlayState() != ViewportPlayState::Stopped) return "the editor is playing - Stop first";
    const std::string want = Resource::AssetRefs::NormalizeRefPath(model);
    Entity pick = NullEntity;
    // The current selection when it is one; else the first placement.
    ReconcileSelection();
    m_world->ForEach<SceneSourcePath>([&](Entity e, SceneSourcePath& s)
    {
        if (Resource::AssetRefs::NormalizeRefPath(s.path) != want) return;
        if (m_world->HasComponent<PreviewStageTag>(e)) return;   // a panel's stage is not a placement
        if (!m_world->GetComponent<SkeletonComponent>(Anim::ResolveAnimEntityOrSelf(*m_world, e))) return;
        if (pick == NullEntity || e == m_selectedEntity) pick = e;
    });
    if (pick == NullEntity)
        return "no instance of '" + model + "' is placed in the scene - the retarget writes onto a scene entity";
    SetSelectionFromUndo(pick);
    m_showRetargetWindow = true;
    m_rtMapEditTarget    = 0;   // its mapping block edits the target
    LOG_INFO("[SKELEDITOR] retarget: the Animation Retarget window targets entity %u (a placement of '%s')", pick,
             model.c_str());
    return {};
}

Entity EditorLayer::SelectableEntity(Entity e) const
{
    if (!m_world || e == NullEntity || !m_world->IsAlive(e)) return e;
    if (const MeshSectionOf* so = m_world->GetComponent<MeshSectionOf>(e))
        if (m_world->IsAlive(so->owner) && m_world->GetGeneration(so->owner) == so->ownerGeneration)
            return so->owner;
    return e;
}

// Bake each debug-icon kind's Font Awesome glyph into an engine texture once, by copying its
// sub-rect out of the ImGui font atlas. The bindless index is registered with the Renderer (keyed by
// codepoint) and consumed by DebugDrawSystem::Submit, so the viewport gizmos reuse the UI icon font.
void EditorLayer::EnsureGizmoIconsBaked()
{
    if (m_gizmoIconsBaked || !m_hasFontAwesomeIcons || !m_renderer) return;
    m_gizmoIconsBaked = true;   // one-shot regardless of individual glyph results

    ImGuiIO&     io    = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;
    if (!atlas) return;

    // ImGui 1.92 dynamic atlas: FindGlyph lives on ImFontBaked and LOADS the glyph on demand. Bake at a
    // high pixel size (not the 16px UI size) so the world-space gizmo stays crisp when drawn large: the
    // FA font is vector, so this rasterizes a sharp glyph instead of upscaling the UI bitmap. Resolve
    // across whichever loaded font carries the codepoint (FA glyphs are merged into the primary font);
    // reject the fallback glyph (its Codepoint does not match).
    constexpr float kGizmoGlyphPx = 128.0f;
    auto findGlyph = [&](uint32_t cp) -> const ImFontGlyph* {
        for (ImFont* f : atlas->Fonts)
        {
            ImFontBaked* baked = f->GetFontBaked(kGizmoGlyphPx);
            if (!baked) continue;
            const ImFontGlyph* g = baked->FindGlyph(static_cast<ImWchar>(cp));
            if (g && g->Codepoint == cp) return g;
        }
        return nullptr;
    };

    // Pass 1: force every wanted glyph to load (this may grow / repack the atlas
    // texture, which is why the pixel read happens afterwards).
    std::vector<uint32_t> cps;
    for (int i = 0; i < DebugDrawSystem::IconKindCount(); ++i)
    {
        const uint32_t cp = DebugDrawSystem::GetIconKindGlyph(i);
        if (!cp) continue;                                             // texture-based kind
        if (m_renderer->GetGlyphIconBindless(cp) != 0xFFFFFFFFu) continue;  // already baked
        if (findGlyph(cp)) cps.push_back(cp);
    }
    if (cps.empty()) return;

    // Pass 2: read the now-stable atlas pixels (TexData is the live texture the
    // glyph UVs reference) and copy each glyph sub-rect into its own texture.
    ImTextureData* tex = atlas->TexData;
    if (!tex || !tex->Pixels || tex->Width <= 0 || tex->Height <= 0) return;
    const int aw  = tex->Width;
    const int ah  = tex->Height;
    const int bpp = tex->BytesPerPixel;
    auto texelRGBA = [&](int x, int y) -> uint32_t {
        const unsigned char* p = tex->Pixels + static_cast<size_t>(x + y * aw) * bpp;
        if (bpp == 4) return *reinterpret_cast<const uint32_t*>(p);
        return (static_cast<uint32_t>(p[0]) << 24) | 0x00FFFFFFu;      // Alpha8 -> white + alpha
    };

    for (uint32_t cp : cps)
    {
        const ImFontGlyph* g = findGlyph(cp);   // re-fetch: UVs are final now
        if (!g) continue;
        const int gx0 = static_cast<int>(g->U0 * aw), gy0 = static_cast<int>(g->V0 * ah);
        const int gx1 = static_cast<int>(g->U1 * aw), gy1 = static_cast<int>(g->V1 * ah);
        const int gw  = std::max(1, gx1 - gx0), gh = std::max(1, gy1 - gy0);

        // Centre the glyph in a padded SQUARE so the (square) billboard quad
        // shows it undistorted. Transparent background, white glyph + alpha.
        const int side = std::max(gw, gh);
        const int pad  = std::max(1, side / 8);
        const int dim  = side + 2 * pad;
        std::vector<uint32_t> buf(static_cast<size_t>(dim) * dim, 0u);
        const int offx = (dim - gw) / 2, offy = (dim - gh) / 2;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
            {
                const int sx = gx0 + x, sy = gy0 + y;
                if (sx >= 0 && sy >= 0 && sx < aw && sy < ah)
                    buf[static_cast<size_t>(offy + y) * dim + (offx + x)] = texelRGBA(sx, sy);
            }

        m_renderer->RegisterGlyphIcon(cp, buf.data(),
                                      static_cast<uint32_t>(dim), static_cast<uint32_t>(dim));
    }
}

void EditorLayer::AssignAnimationAsset(Entity target, const std::string& path)
{
    if (!m_world || !m_animClipSys || path.empty() || !m_world->IsAlive(target)) return;

    Entity owner = target;
    if (const auto* ref = m_world->GetComponent<SkeletonRef>(target))
    {
        if (m_world->IsAlive(ref->entity)) owner = ref->entity;
    }
    if (!m_world->HasComponent<SkeletonComponent>(owner))
    {
        LOG_WARNING("EditorLayer: entity %u has no skeleton for animation '%s'",
                    target, path.c_str());
        return;
    }

    AnimationSetComponent* set = m_world->GetComponent<AnimationSetComponent>(owner);
    if (!set)
    {
        m_world->AddComponent<AnimationSetComponent>(owner, AnimationSetComponent{});
        set = m_world->GetComponent<AnimationSetComponent>(owner);
    }

    const std::string assetPath = Sys::RelativizeAssetPath(path);
    set->Set("Default", assetPath, true, 1.f);
    set->defaultClip       = "Default";
    set->playDefaultOnLoad = true;
    m_world->RemoveComponent<AnimationSourcePath>(owner); // superseded by the AnimationSetComponent above
    m_animClipSys->QueueAnimatorPlay(*m_world, owner, "Default", 0.f, true);

    LOG_SUCCESS("EditorLayer: assigned animation '%s' as Default on entity %u",
                assetPath.c_str(), owner);
}

void EditorLayer::OnUIRender()
{
    ImGuizmo::BeginFrame();
    // BeginFrame resets only mDrawList and mbOverGizmoHotspot; every field IsOver() hit-tests against is
    // written inside Manipulate and survives. So track submission here: m_gizmoSubmittedThisFrame.
    m_gizmoSubmittedThisFrame = false;

    // One-time: bake the gizmo icon glyphs (Font Awesome) into engine textures
    // so the viewport light/camera billboards reuse the UI icon font instead of
    // separate .itex assets. Runs now (atlas built, renderer ready).
    EnsureGizmoIconsBaked();

    // ---- Undo / redo: bracket gizmo drags and handle Ctrl+Z / Y (both viewport modes) ----
    // Sampled at the top of the frame so ImGuizmo::IsUsing() reflects last frame's manipulation and
    // LocalTransform holds its final value.
    UpdateTransformEditTracking();
    // Same shape, for the bone-pose gizmo. Must run BEFORE any chain tree /
    // the Inspector draws, so a drag that ended last frame is committed against
    // the pre-drag baseline rather than against this frame's UI edits.
    UpdateBonePoseEditTracking();
    FinalizeStaleFieldEdit();
    // Rebind capture MUST consume its key BEFORE the undo shortcuts run, or
    // binding an action to Z while Ctrl is held performs an editor undo on the
    // very frame you bind it. HandleUndoShortcuts additionally early-outs on
    // WantsExclusiveKeyboard(), because the key edge that ended the capture is
    // still visible to ImGui::IsKeyPressed for the rest of this frame.
    Editor::InputBindingsWindow::Get().TickCapture();
    HandleUndoShortcuts();

    // Chain-overlay highlight: reset every frame; only a chain tree drawn for the overlay's own target
    // (ChainParamHooks::mainOverlayTarget) re-asserts it. Resetting here, not inside a draw body, means
    // a collapsed header, hidden tab, closed window or F11 fullscreen cannot leave a stale highlight
    // dimming the viewport overlay.
    m_chainHighlightGroup = -1;

    // ---- Reflection-probe viz visibility ----
    // A probe's viz sphere is a primitive Sphere mesh on the probe entity, so only that entity's
    // VisibilityComponent hides it. Two controls drive it: m_showProbeVizSpheres (the master switch,
    // the viewport toolbar's Show > Reflection Probe Viz) and ReflectionProbeComponent::showVizSphere
    // (per probe, serialized, editable from the Inspector and through the Visibility component's
    // "Visible" checkbox).
    // The walk keys off the probe pool, not the "ReflectionProbe" tag, so a probe spawned without the
    // tag obeys the master too. It (a) adopts an external edit of the Visible bit (Inspector, outliner,
    // undo) into the per-probe flag, then (b) re-asserts `master && showVizSphere`; writing an unchanged
    // bit back is free (the retained scene diff compares flags).
    // Baking ignores VisibilityComponent, so a hidden probe keeps capturing, and no probe's viz ball is
    // ever in a cubemap (the bake excludes every probe's MeshHandle via ProbeBakeRequest::probeMeshes).
    // Hiding one drops it from RT reflections and RTAO (DDGISceneAS mask bit 0 follows
    // IsEffectivelyVisible); DDGI still sees it (mask bit 2 is unconditional for statics).
    if (m_world)
    {
        if (auto* probePool = m_world->GetPool<ReflectionProbeComponent>())
        {
            const auto& probeEnts = probePool->Entities();
            auto&       probeData = probePool->Data();
            for (size_t i = 0; i < probeData.size(); ++i)
            {
                const Entity e = probeEnts[i];
                if (!m_world->IsAlive(e)) continue;
                // Only the primitive viz ball is ours to drive. A probe authored onto real scene geometry carries a
                // MeshLibRef (or no mesh at all) and is left alone: the master switch must never make an actual
                // prop disappear.
                if (!m_world->HasComponent<MeshHandle>(e)) continue;
                auto* vis = m_world->GetComponent<VisibilityComponent>(e);
                if (!vis) continue;

                ReflectionProbeComponent& rp = probeData[i];
                // (a0) A scene saved before showVizSphere existed: its hidden state lives only in the Visibility
                // flags about to be overwritten. Adopt it once.
                if (rp.vizSeedFromVisibility)
                {
                    rp.showVizSphere         = rp.showVizSphere && vis->IsVisible();
                    rp.vizSeedFromVisibility = false;
                }
                // (a) Someone else moved the bit since our last write: that is the artist's intent; keep it (and
                // persist it with the scene). Only while stopped: in Play the bit can be driven by gameplay or a
                // Sequencer Visibility track (SequenceEval), which is transient state, not .iscene data.
                else if (rp.vizPushValid && vis->IsVisible() != rp.vizPushedVisible
                         && GetPlayState() == ViewportPlayState::Stopped)
                    rp.showVizSphere = vis->IsVisible();

                // (b) Re-assert master AND per-probe.
                const bool want = m_showProbeVizSpheres && rp.showVizSphere;
                vis->SetVisible(want);
                rp.vizPushedVisible = want;
                rp.vizPushValid     = true;
            }
        }
    }

    ReconcileSelection();
    m_hierarchyHighlight = m_selectedEntity;
    m_lastSeenSelectedEntity = m_selectedEntity;

    // Editor reference grid: push state to the renderer once per frame, here
    // rather than in the viewport toolbar so it also runs in F11 fullscreen
    // (where the toolbar isn't drawn). Fullscreen is the game-preview mode,
    // so the grid force-hides there. Which views draw it is their EditorOverlays
    // show flag (the Game View's picture has none).
    if (m_renderer)
        if (EditorGridPass* grid = m_renderer->GetEditorGridPass())
        {
            grid->SetEnabled(m_showEditorGrid && !m_viewportFullscreen && !m_harnessHideGrid);
            grid->SetCellSize(m_gridCellSize);
        }
    // Show > Screen UI off hides the game UI in the Edit viewport (laid out, not painted); Play and F11
    // always show it.
    // The Scene showing the editor camera in Play (the Game View open but hidden) has no HUD either.
    if (m_renderer)
        m_renderer->SetHideCanvasUI((!m_showScreenUI && !m_viewportFullscreen &&
                                     GetPlayState() == ViewportPlayState::Stopped)
                                    || MainViewObservesPlay());

    if (m_viewportFullscreen)
    {
        RenderFullscreenViewport();
        return;
    }

    // Status bar FIRST: BeginViewportSideBar insets the main viewport's work
    // area, and SetupDockSpace sizes the dock host from that work area.
    RenderStatusBar();
    SetupDockSpace();
    RenderHierarchyPanel();
    RenderViewportPanel();
    RenderInspectorPanel();
    // Output Log before the Content Browser, on purpose: a window takes keyboard focus when first
    // created and a dock node shows its focused window's tab, so the last-created of the two - the
    // Content Browser - is in front at startup.
    RenderOutputLogPanel();
    RenderContentBrowserPanel();
    // Harness (-ui_open_menu=Post Processing@<frame> / Environment@<frame>).
    if (HarnessMenuHeldOpen("Post Processing")) m_showPostProcess = true;
    if (m_showPostProcess) RenderPostProcessPanel();
    // Harness (-ui_open_menu=Project Settings@<frame>): open it for a screenshot.
    if (HarnessMenuHeldOpen("Project Settings")) m_showGraphicsSettings = true;
    if (m_showGraphicsSettings) RenderGraphicsSettingsWindow();
    else SyncRestirCostWant(false);
    if (HarnessMenuHeldOpen("Editor Preferences")) m_showEditorPreferences = true;
    if (m_showEditorPreferences) RenderEditorPreferencesWindow();
    if (HarnessMenuHeldOpen("Keyboard Shortcuts")) m_showKeyboardShortcuts = true;
    if (m_showKeyboardShortcuts) RenderKeyboardShortcutsWindow();
    if (HarnessMenuHeldOpen("About")) m_showAbout = true;
    if (m_showAbout) RenderAboutWindow();
    RenderCommandPalette();   // self-gating: a popup, open only on request
    Editor::Notices::Get().Draw();   // the toasts (nothing under -unattended)
    if (HarnessMenuHeldOpen("Environment")) m_showEnvironmentWindow = true;
    if (m_showEnvironmentWindow) RenderEnvironmentWindow();
    if (HarnessMenuHeldOpen("World Settings")) m_showWorldSettings = true;
    if (m_showWorldSettings) RenderWorldSettingsWindow();
    RenderAnimationDebugWindow();
    RenderRetargetWindow();
    if (m_showTimeline) RenderTimelinePanel();
    // Harness (-ui_open_menu=Profiler@<frame>, Profiler/<Tab>@<frame>).
    if (HarnessMenuHeldOpen("Profiler") || HarnessMenuHeldOpen("Profiler/Overview") ||
        HarnessMenuHeldOpen("Profiler/Timeline") || HarnessMenuHeldOpen("Profiler/Threads") ||
        HarnessMenuHeldOpen("Profiler/Passes") || HarnessMenuHeldOpen("Profiler/Memory") ||
        HarnessMenuHeldOpen("Profiler/Captures"))
        m_showProfiler = true;
    SyncProfilerHold();   // also releases the profiler when the menu closed the window
    RenderAssetReferencesWindow();   // self-gating on m_showReferencesWindow
    // The registered tool panels (Profiler, Pool Watermarks, Loaded Geometry / Textures, Decal
    // Materials, Font Library, Camera Switcher, Script Registry, Game Flow; EditorLayer_Panels.cpp),
    // harness holds included.
    EnsurePanelsRegistered();
    Editor::PanelRegistry::Get().DrawOpen([this](const char* n) { return HarnessMenuHeldOpen(n); });
    // Every frame, open or not: a harness hold (-ui_open_menu=Render Graph[/<Tab>]@<frame>) opens it,
    // closing releases.
    Editor::RenderGraphWindow::Get().Draw({ m_gfx, [this](const char* n) { return HarnessMenuHeldOpen(n); }, m_renderer });
    // The enabled modules' panels, every frame, open or not (Editor/EditorModule.h).
    Editor::ModuleRegistry::Get().DrawPanels(ModuleHostFor());
    if (m_showCameraPreview)
        Editor::CameraPreviewWindow::Get().Draw(m_renderer, m_world,
                                                &m_showCameraPreview,
                                                m_selectedEntity);
    if (m_showAssetPreview)
        Editor::AssetPreviewWindow::Get().Draw(m_renderer, m_world,
                                               &m_showAssetPreview);
    // A Synthetic view like the Asset Preview's; the lane's size follows whichever of them is owed a
    // picture.
    if (Editor::MeshEditorWindow::Get().IsOpen())
        Editor::MeshEditorWindow::Get().Draw(MeshEditorCtx());
    // Another Synthetic view (the whole model).
    if (Editor::SkeletonEditorWindow::Get().IsOpen())
        Editor::SkeletonEditorWindow::Get().Draw(SkeletonEditorCtx());
    // Not a preview view: a texture view (Renderer/TextureViewService.h) drawn 1:1.
    if (Editor::TextureViewerWindow::Get().IsOpen())
        Editor::TextureViewerWindow::Get().Draw(TextureViewerCtx());
    // A UI view (Renderer/UIViewService.h) drawn 1:1.
    if (Editor::UIEditorWindow::Get().IsOpen())
        Editor::UIEditorWindow::Get().Draw(UIEditorCtx());
    // One preview view (the active tab's).
    if (Editor::MaterialEditorWindow::Get().IsOpen())
        Editor::MaterialEditorWindow::Get().Draw(MaterialEditorCtx());
    // The node editor's acceptance run (harness only: -nodeeditor_smoke=<nodes>).
    Editor::NodeEditorSmoke::Get().Draw(m_editorFrame);
    // N secondary scene views in one dockable panel. Drawn after the camera preview so that, when both
    // are open at different sizes, the older panel fixed the WorldCull lane's size and this one reports
    // the refusal - the order preview_split_size asserts.
    if (m_showSplitViewport)
        Editor::SplitViewportPanel::Get().Draw(m_renderer, m_world,
                                               &m_showSplitViewport);
    // The Morph editor, then the Chain Physics Preview: drawn after the camera preview and the split
    // panel, it yields the WorldCull lane to them - it adopts whatever size the lane already has rather
    // than suspending another panel's view.
    DrawMorphEditor();
    m_chainColliderUndo = false;
    Editor::ChainPhysicsPreviewWindow::Get().drawSceneColliders = [this]() { DrawChainColliders(); };
    if (Editor::ChainPhysicsPreviewWindow::Get().IsOpen())
        Editor::ChainPhysicsPreviewWindow::Get().Draw(m_renderer, m_world,
                                                      m_selectedEntity,
            // The chain parameter tree, drawn by the panel on its STAGED copy (panel state only).
            [this](ChainPhysicsComponent& c, const SkeletonAsset& s,
                   const ChainParamHooks& h) { DrawChainParamTree(c, s, h); },
            // The Play-mode "LIVE" body: a scene character's live component (World-touching, so it is ours).
            [this]() { DrawChainLiveEdit(); },
            GetPlayState() != ViewportPlayState::Stopped);
    // Harness-only probe of the stage-edit gates (inert - one static int compare - without
    // -chainpreview_stage_edit_at).
    TickChainStageEditProbe();
    // The panel-tree and Play LIVE probes (inert without -chainpreview_panel_tree_at /
    // -chainpreview_play_edit).
    TickChainPanelTreeProbe();
    TickChainPlayEditProbe();
    // Harness probe of the main viewport's bone-pose gizmo: inert without -chainpreview_main_pose.
    TickChainMainPoseProbe();
    {
        // The panel must not include EngineSettings (it sits below that layer),
        // so persistence is injected. CaptureFrom pulls the LIVE bindings, so
        // this writes the rebinds without clobbering graphics settings.
        auto& ibw = Editor::InputBindingsWindow::Get();
        if (!ibw.saveCallback && m_renderer && m_gfx)
        {
            ibw.saveCallback = [this]() -> bool {
                return Resource::SaveCapturedEngineSettings(*m_renderer, *m_gfx, m_window);
            };
        }
        ibw.Render();
    }
    if (m_showAnimSetEditor) RenderAnimationSetEditorWindow();
    AttackEditor::Draw();
    Editor::AnimGraphEditor::Draw(m_world, m_renderer ? m_renderer->GetAnimGraphSystem() : nullptr);
    Editor::AnimSetEditor::Draw(m_animClipSys);
    if (m_showVFXEditor)   RenderVFXEditorWindow();
    if (m_showSequencer)   RenderSequencerWindow();
    if (m_showComponentTransfer) RenderComponentTransferWindow();
}

bool EditorLayer::CacheRenderableViewportSize(const ImVec2& size)
{
    // Several post effects build mip chains and dispatch fixed-size groups.
    // A dock node can briefly report only a handful of pixels while ImGui is
    // rebuilding/collapsing the tree; retaining the last usable extent avoids
    // a full graph teardown for a rectangle that will never be presented.
    constexpr float kMinRenderableExtent = 64.0f;
    if (size.x < kMinRenderableExtent || size.y < kMinRenderableExtent)
        return false;

    const auto width  = static_cast<unsigned int>(size.x);
    const auto height = static_cast<unsigned int>(size.y);
    if (width < static_cast<unsigned int>(kMinRenderableExtent) ||
        height < static_cast<unsigned int>(kMinRenderableExtent))
        return false;

    m_viewportSizeX = width;
    m_viewportSizeY = height;
    return true;
}

void EditorLayer::RenderFullscreenViewport()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::Begin("##FullscreenViewportHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x <= 0 || size.y <= 0)
        size = vp->Size;

    // Match renderer viewport resolution to the whole window.  Ignore a
    // transient minimize/collapse rectangle rather than rebuilding the entire
    // render graph at a shader-hostile size such as 4x5.
    CacheRenderableViewportSize(size);

    const ImVec2 imageTopLeft = ImGui::GetCursorScreenPos();
    const float  contentW     = size.x;
    const float  contentH     = size.y;

    if (m_sceneTextureId != 0)
    {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_sceneTextureId)), size);
    }
    else
    {
        ImU32 col = IM_COL32(40, 44, 52, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(imageTopLeft, ImVec2(imageTopLeft.x + size.x, imageTopLeft.y + size.y), col);
    }

    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    m_viewportRightDragging = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    m_viewportHovered       = hovered;   // gamepad look gate (App.cpp)
    if (m_viewportRightDragging)
    {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        m_vpMouseDX = delta.x;
        m_vpMouseDY = delta.y;
    }
    else
    {
        m_vpMouseDX = 0.f;
        m_vpMouseDY = 0.f;
    }

    // Before picking: a click that lands on a rect handle must not also fire a GPU pick, whose result
    // arrives a frame later and would retarget the selection out from under the drag. No InvisibleButton
    // here: the scene is an ImGui::Image, whose item id is 0 and can never hold ActiveId, so
    // IsAnyItemActive() means exactly "another widget owns the mouse".
    m_viewportOtherItemActive = ImGui::IsAnyItemActive();
    // The viewport's Canvas Rect Tool is a preference (Show menu), off by default: the UI Editor is the UI
    // tool, and the viewport's clicks stay the 3D pick's.
    if (m_uiRectToolInViewport)
        RenderUIRectTool(imageTopLeft, m_viewportOtherItemActive);
    else
    {
        if (m_uiRectDrag.active) CommitUIRectDrag();
        m_uiRectToolClaimsMouse = false;
    }

    HandleViewportPicking(imageTopLeft.x, imageTopLeft.y, contentW, contentH, hovered);

    ImGui::End();
}

namespace
{
    // Rotate a named clip's ROOT-bone channels by the reference skeleton's up-axis fix. Only channels
    // that exist are touched: unkeyed roots default to the already-normalized reference rest in
    // BindToSkeleton, so there is no double rotation.
    void ApplyUpFixToNamedClip(Resource::AnimClipData& acd, const SkeletonAsset& refSkel,
                               const DirectX::XMFLOAT4X4& upFix)
    {
        using namespace DirectX;
        const XMVECTOR qG = XMQuaternionRotationMatrix(XMLoadFloat4x4(&upFix));
        for (auto& ch : acd.channels)
        {
            bool isRoot = false;
            for (uint32_t b = 0; b < refSkel.boneCount; ++b)
                if (refSkel.parentIndex[b] < 0 &&
                    std::strcmp(refSkel.boneNames[b], ch.name) == 0)
                { isRoot = true; break; }
            if (!isRoot) continue;

            for (auto& q4 : ch.rotations)
            {
                const XMVECTOR q = XMLoadFloat4(&q4);
                XMStoreFloat4(&q4, XMQuaternionNormalize(XMQuaternionMultiply(q, qG)));
            }
            for (auto& p3 : ch.positions)
            {
                const XMVECTOR p = XMLoadFloat3(&p3);
                XMStoreFloat3(&p3, XMVector3Rotate(p, qG));
            }
        }
    }

} // namespace

// Names of the required humanoid roles a profile is missing, for error strings.
static std::string MissingHumanoidRoles(const SkeletonAsset& sk)
{
    std::string miss;
    for (uint32_t i = 0; i < kHumanoidBoneCount; ++i)
        if (IsRequiredHumanoidBone(static_cast<HumanoidBone>(i)) && sk.humanoid.boneForRole[i] < 0)
        { miss += HumanoidRoleName(static_cast<HumanoidBone>(i)); miss += ' '; }
    return miss.empty() ? std::string("(bone names not recognized)") : miss;
}

// The retarget TARGET: the selected entity's skeleton, with its humanoid profile
// built/refreshed. Shared by the single-file and batch paths so they cannot
// drift apart. Returns nullptr + err.
SkeletonAsset* EditorLayer::ResolveRetargetTarget(Entity& outSkelEntity, std::string& err)
{
    if (!m_renderer || !m_world)        { err = "no renderer/world"; return nullptr; }
    if (m_selectedEntity == NullEntity)
    { err = "select a target entity with a skeleton first"; return nullptr; }

    Entity skelEntity = m_selectedEntity;
    if (const SkeletonRef* ref = m_world->GetComponent<SkeletonRef>(m_selectedEntity))
        if (ref->entity != NullEntity) skelEntity = ref->entity;

    auto* skelComp = m_world->GetComponent<SkeletonComponent>(skelEntity);
    if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle ||
        skelComp->assetIndex >= m_renderer->GetSkeletonRegistry().Count())
    { err = "selected entity has no skeleton"; return nullptr; }

    // Mutable so a profile can be built in place for an .iskel cooked before humanoid profiles existed.
    // PMX profiles are rebuilt (the mapping table evolves), but a hand-tuned mapping (userEdited, set by
    // the Humanoid Bone Mapping UI) is never clobbered. Do not mutate the skeleton asset here (no
    // ikChains / grant clearing): the same SkeletonAsset serves native VMD playback. The FK/IK split is
    // per clip: baked clips carry LegDriveMode::FK, which IKSystem honours per entity.
    SkeletonAsset& trgSkel = m_renderer->GetSkeletonRegistry().Get(skelComp->assetIndex);
    if (!trgSkel.humanoid.userEdited &&
        (!trgSkel.humanoid.IsValid() ||
         trgSkel.humanoid.convention == static_cast<uint8_t>(HumanoidConvention::PmxJp)))
        Resource::HumanoidProfileBuilder::Build(trgSkel);
    if (!trgSkel.humanoid.IsValid())
    {
        err = "target profile invalid; missing required roles: " + MissingHumanoidRoles(trgSkel) +
              " (fix in Humanoid Bone Mapping)";
        return nullptr;
    }
    outSkelEntity = skelEntity;
    return &trgSkel;
}

bool EditorLayer::DoRetarget(const std::string& srcPath, std::string& outStatus)
{
    auto fail = [&](std::string s) { outStatus = std::move(s); LOG_ERROR("Retarget: %s", outStatus.c_str()); return false; };

    if (srcPath.empty()) return fail("no source .iskel chosen");

    Entity skelEntity = NullEntity;
    std::string terr;
    SkeletonAsset* trgSkelPtr = ResolveRetargetTarget(skelEntity, terr);
    if (!trgSkelPtr) return fail(std::move(terr));
    SkeletonAsset& trgSkel = *trgSkelPtr;

    auto missingRoles = [](const SkeletonAsset& sk) { return MissingHumanoidRoles(sk); };

    // ---- Resolve SOURCE = reference skeleton + clips ----
    // Normally both come from one .iskel (a Mixamo anim FBX embeds its T-pose). A UE animation-only FBX
    // does not carry the mannequin reference pose, so an optional Source Skeleton override (.iskel from
    // the skeletal-mesh export, e.g. SKM_Manny) supplies the rest pose; the animation file then only
    // contributes clips, joined to the reference skeleton by bone name.
    m_retargetSrcPath = srcPath;
    SkeletonAsset* refSkel = GetOrLoadRetargetRefSkel();

    const bool isIanim = srcPath.size() > 6 &&
        _stricmp(srcPath.c_str() + srcPath.size() - 6, ".ianim") == 0;

    SkeletonAsset*         srcSkel = nullptr;
    std::vector<ClipAsset> boundClips;               // used when the override is active
    const std::vector<ClipAsset>* clipsPtr = nullptr;

    if (refSkel)
    {
        srcSkel = refSkel;
        if (!srcSkel->humanoid.IsValid())
            return fail("source-skeleton profile invalid; missing required roles: " +
                        missingRoles(*srcSkel) + " (fix in Humanoid Bone Mapping)");

        if (isIanim)
        {
            // Named channels -> bind directly onto the reference skeleton. Read
            // where playback would (a worktree's shared content root too).
            std::vector<uint8_t> bytes;
            if (!Resource::AssetFS::Get().ReadFile(srcPath, bytes))
                return fail("cannot open '" + srcPath + "'");
            Resource::AnimationLoader loader;
            Resource::LoadResult lr = loader.Load(srcPath, bytes);
            auto* animRes = static_cast<Resource::AnimationResource*>(lr.resource.get());
            if (!animRes || animRes->clips.empty())
                return fail("'" + srcPath + "' contains no bone clips");
            for (Resource::AnimClipData& acd : animRes->clips)
            {
                // .ianim channels are raw source-space; if the reference skeleton
                // was rotated upright (Z-up source), root channels must follow.
                if (m_rtRefUpFixed)
                    ApplyUpFixToNamedClip(acd, *srcSkel, m_rtRefUpFix);
                boundClips.push_back(acd.BindToSkeleton(*srcSkel));
            }
        }
        else
        {
            // .iskel animation: re-express its bone-indexed clips as named channels against ITS skeleton, then
            // re-bind by name onto the reference skeleton. Unanimated bones fall back to the REFERENCE rest -
            // exactly what the delta math needs.
            SkeletonAsset* animSkel = GetOrLoadRetargetSource();
            if (!animSkel) return fail("failed to load source '" + srcPath + "'");
            if (m_rtSrcClips.empty()) return fail("source '" + srcPath + "' has no clips");
            uint32_t ci = 0;
            for (const ClipAsset& c : m_rtSrcClips)
            {
                const std::string nm = "clip_" + std::to_string(ci++);
                Resource::AnimClipData named =
                    Resource::RetargetBaker::ToNamedClipData(c, *animSkel, nm.c_str());
                boundClips.push_back(named.BindToSkeleton(*srcSkel));
            }
        }
        clipsPtr = &boundClips;
    }
    else
    {
        if (isIanim)
            return fail(".ianim sources need a Source Skeleton (.iskel) — the animation "
                        "file carries no reference pose");
        srcSkel = GetOrLoadRetargetSource();
        if (!srcSkel)
            return fail("failed to load source .iskel '" + srcPath + "'");
        if (m_rtSrcClips.empty())
            return fail("source '" + srcPath + "' has no animation clips");
        if (!srcSkel->humanoid.IsValid())
            return fail("source profile invalid; missing required roles: " + missingRoles(*srcSkel) +
                        " (fix in Humanoid Bone Mapping)");
        clipsPtr = &m_rtSrcClips;
    }
    const std::vector<ClipAsset>& srcClips = *clipsPtr;

    Resource::RetargetOptions opts;
    opts.upperBody      = m_rtUpper;
    opts.lowerBody      = m_rtLower;
    opts.includeFingers = m_rtFingers;
    opts.bakeRootMotion = m_rtRoot;
    opts.sourceAPose    = m_rtSrcAPose;
    opts.targetAPose    = m_rtTrgAPose;
    opts.canonicalizeLegs = m_rtCanonLegs;
    opts.canonicalizeFeet  = m_rtCanonFeet;
    opts.canonicalizeNeck  = m_rtCanonNeck;
    opts.redistributeTwist = m_rtTwist;

    const std::string srcStem = Sys::PathToUtf8(Sys::U8Path(srcPath).stem());

    uint32_t firstIdx = kInvalidClipIndex, bakedCount = 0;
    Resource::AnimationResource bakedRes; // named-channel copies for .ianim persistence
    for (const ClipAsset& sc : srcClips)
    {
        ClipAsset baked = Resource::RetargetBaker::BakeClip(*srcSkel, sc, trgSkel, opts);

        // Named-channel copy BEFORE the move: this is what gets saved to disk.
        std::string clipName = srcStem + "_retarget";
        if (bakedCount > 0) clipName += "_" + std::to_string(bakedCount);
        bakedRes.clips.push_back(
            Resource::RetargetBaker::ToNamedClipData(baked, trgSkel, clipName.c_str()));

        const uint32_t idx = m_renderer->GetClipLibrary().Register(std::move(baked));
        if (firstIdx == kInvalidClipIndex) firstIdx = idx;
        ++bakedCount;
    }

    if (auto* ac = m_world->GetComponent<AnimationComponent>(skelEntity))
    { ac->primaryClip = firstIdx; ac->primaryTime = 0.f; }
    else
    { AnimationComponent nc{}; nc.primaryClip = firstIdx; m_world->AddComponent(skelEntity, nc); }

    // ---- Persist as a normal .ianim (survives an editor restart, draggable from the asset browser;
    // ANIM_FLAG_LEG_FK round-trips through BindToSkeleton) ----
    std::string savedNote;
    if (m_rtSaveIanim)
    {
        std::string savePath = SaveFileDialog(
            "Animation (*.ianim)\0*.ianim\0All Files\0*.*\0\0", "ianim", "asset/");
        if (!savePath.empty())
        {
            std::vector<uint8_t> blob = Resource::SerializeAnimation(bakedRes);
            if (!blob.empty() && WriteFileFully(savePath, blob))
            {
                // Store asset-relative when the file lives under asset/ so scenes
                // and prefabs stay portable through AnimationSetComponent.
                const std::string rel = Sys::RelativizeAssetPath(savePath);
                AssignAnimationAsset(skelEntity, savePath);
                savedNote = " Saved '" + rel + "'.";
                LOG_SUCCESS("Retarget: saved baked animation -> '%s' (%zu bytes, %u clips)",
                            rel.c_str(), blob.size(), bakedCount);
            }
            else
            {
                savedNote = " (SAVE FAILED: '" + savePath + "')";
                LOG_ERROR("Retarget: failed to save baked .ianim to '%s'", savePath.c_str());
            }
        }
        else savedNote = " (save skipped)";
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Baked %u clip(s) -> playing ClipLibrary[%u] on entity %u.",
                  bakedCount, firstIdx, skelEntity);
    outStatus = std::string(buf) + savedNote;
    LOG_SUCCESS("Retarget: %s (src '%s', src-Apose=%d trg-Apose=%d)",
                outStatus.c_str(), srcPath.c_str(), (int)m_rtSrcAPose, (int)m_rtTrgAPose);
    return true;
}

// ==== Batch retarget ====
// Walk a folder tree, retarget every animation source found, and mirror the tree under an output
// root.

// Source extensions the batch walk accepts. .ianim/.iskel are already engine
// assets; the rest go through AnimationImporter (Assimp) in memory first, so a
// raw FBX export tree can be cooked without importing each file by hand.
static bool IsBatchRetargetSource(const std::string& extLower)
{
    return extLower == ".ianim" || extLower == ".iskel" || extLower == ".fbx" ||
           extLower == ".gltf"  || extLower == ".glb"   || extLower == ".dae" ||
           extLower == ".bvh";
}

// Load one source file as clips bound to `refSkel`. This is the batch analogue
// of DoRetarget's source resolution, minus the caches: a batch touches hundreds
// of files, so nothing is retained between them.
bool EditorLayer::LoadRetargetClipsFor(const std::string& srcPath, SkeletonAsset& refSkel,
                                       std::vector<ClipAsset>& outClips, std::string& err)
{
    outClips.clear();
    std::string ext = Sys::PathToUtf8(Sys::U8Path(srcPath).extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".iskel")
    {
        // Bone-indexed clips against their own skeleton -> named channels ->
        // re-bound by name onto the reference skeleton (unanimated bones fall
        // back to the REFERENCE rest, which is what the delta math needs).
        auto animSkel = std::make_unique<SkeletonAsset>();   // ~260 KB, never on the stack
        std::vector<ClipAsset> animClips;
        if (!SceneInstanceLoader::LoadSkeletonFile(srcPath, *animSkel, animClips))
        { err = "cannot load .iskel"; return false; }
        if (animClips.empty()) { err = "no clips"; return false; }
        uint32_t ci = 0;
        for (const ClipAsset& c : animClips)
        {
            const std::string nm = "clip_" + std::to_string(ci++);
            Resource::AnimClipData named =
                Resource::RetargetBaker::ToNamedClipData(c, *animSkel, nm.c_str());
            outClips.push_back(named.BindToSkeleton(refSkel));
        }
        return true;
    }

    std::vector<uint8_t> bytes = ReadFileFully(srcPath);
    if (bytes.empty()) { err = "cannot read file"; return false; }
    if (ext != ".ianim")
    {
        bytes = Resource::AnimationImporter().Import(srcPath, bytes);
        if (bytes.empty()) { err = "no animation tracks"; return false; }
    }

    Resource::AnimationLoader loader;
    Resource::LoadResult lr = loader.Load(srcPath, bytes);
    auto* animRes = static_cast<Resource::AnimationResource*>(lr.resource.get());
    if (!animRes || animRes->clips.empty()) { err = "no bone clips"; return false; }
    for (Resource::AnimClipData& acd : animRes->clips)
    {
        // Raw source-space channels: if the reference skeleton was rotated
        // upright (Z-up UE source), root channels must follow.
        if (m_rtRefUpFixed) ApplyUpFixToNamedClip(acd, refSkel, m_rtRefUpFix);
        outClips.push_back(acd.BindToSkeleton(refSkel));
    }
    return true;
}

bool EditorLayer::DoBatchRetarget(const std::string& srcRoot, const std::string& outRoot,
                                  std::string& outStatus)
{
    namespace fs = std::filesystem;
    auto fail = [&](std::string s) { outStatus = std::move(s);
                                     LOG_ERROR("Batch retarget: %s", outStatus.c_str());
                                     return false; };

    if (srcRoot.empty()) return fail("no source folder chosen");
    if (outRoot.empty()) return fail("no output folder chosen");

    Entity skelEntity = NullEntity;
    std::string terr;
    SkeletonAsset* trgSkelPtr = ResolveRetargetTarget(skelEntity, terr);
    if (!trgSkelPtr) return fail(std::move(terr));
    SkeletonAsset& trgSkel = *trgSkelPtr;

    // A batch always joins clips to a reference pose by bone name, so the Source Skeleton override is
    // required: the per-file .iskel path would give each file its own (possibly bogus) rest pose.
    SkeletonAsset* refSkel = GetOrLoadRetargetRefSkel();
    if (!refSkel)
        return fail("batch needs a Source skeleton (.iskel) — set it above so every "
                    "file in the folder can be joined to one reference pose");
    if (!refSkel->humanoid.IsValid())
        return fail("source-skeleton profile invalid; missing required roles: " +
                    MissingHumanoidRoles(*refSkel) + " (fix in Humanoid Bone Mapping)");

    std::error_code ec;
    const fs::path srcBase = fs::absolute(Sys::U8Path(srcRoot), ec);
    // A worktree reading its binary assets from the main checkout: the sources
    // this checkout's sparse copy lacks are read from the folder's twin there
    // (the outputs still go under outRoot, here).
    const std::string twinDir = Resource::AssetFS::Get().SharedTwinDir(Sys::RelativizeAssetPath(srcRoot));
    const bool twin = !twinDir.empty() && fs::is_directory(Sys::U8Path(twinDir), ec);
    if (!twin && (ec || !fs::is_directory(srcBase, ec)))
        return fail("source folder does not exist: '" + srcRoot + "'");
    const fs::path outBase = fs::absolute(Sys::U8Path(outRoot), ec);
    if (ec) return fail("bad output folder: '" + outRoot + "'");

    // ---- DFS the tree ----
    std::vector<std::pair<fs::path, fs::path>> sources;   // (file, its path under the source folder)
    std::unordered_set<std::string> seenRel;
    auto walk = [&](const fs::path& base)
    {
        std::error_code wec;
        for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, wec),
                                              end; it != end && !wec; it.increment(wec))
        {
            if (!it->is_regular_file(wec)) continue;
            std::string ext = Sys::PathToUtf8(it->path().extension());
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!IsBatchRetargetSource(ext)) continue;
            const fs::path rel = it->path().lexically_relative(base);
            std::string key = Resource::AssetRefs::ToLowerAscii(Sys::PathToUtf8(rel));
            for (char& c : key) if (c == '\\') c = '/';
            if (seenRel.insert(std::move(key)).second)
                sources.emplace_back(it->path(), rel);
        }
    };
    walk(srcBase);
    if (twin) walk(Sys::U8Path(twinDir));
    if (sources.empty())
        return fail("no animation sources (.fbx/.gltf/.glb/.dae/.bvh/.ianim/.iskel) under '"
                    + srcRoot + "'");
    // Deterministic order + stable logs: by the path under the source folder.
    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

    Resource::RetargetOptions opts;
    opts.upperBody         = m_rtUpper;
    opts.lowerBody         = m_rtLower;
    opts.includeFingers    = m_rtFingers;
    opts.bakeRootMotion    = m_rtRoot;
    opts.sourceAPose       = m_rtSrcAPose;
    opts.targetAPose       = m_rtTrgAPose;
    opts.canonicalizeLegs  = m_rtCanonLegs;
    opts.canonicalizeFeet  = m_rtCanonFeet;
    opts.canonicalizeNeck  = m_rtCanonNeck;
    opts.redistributeTwist = m_rtTwist;

    uint32_t okCount = 0, skipCount = 0, failCount = 0, clipCount = 0;
    std::string firstError;

    for (const auto& [src, rel] : sources)
    {
        const std::string srcU8 = Sys::PathToUtf8(src);
        const fs::path dst = (rel.empty() ? outBase / src.filename() : outBase / rel).parent_path() /
                             (src.stem().wstring() + L".ianim");
        const std::string dstU8 = Sys::PathToUtf8(dst);

        // Never write over the file we are reading (srcRoot == outRoot with a
        // .ianim source would otherwise destroy the input).
        if (fs::exists(dst, ec) && fs::equivalent(src, dst, ec))
        {
            LOG_WARNING("Batch retarget: skip '%s' — output would overwrite the source",
                        srcU8.c_str());
            ++skipCount; continue;
        }
        if (!m_rtBatchOverwrite && fs::exists(dst, ec))
        {
            LOG_INFO("Batch retarget: skip '%s' — '%s' exists (enable Overwrite to replace)",
                     srcU8.c_str(), dstU8.c_str());
            ++skipCount; continue;
        }

        std::vector<ClipAsset> srcClips;
        std::string err;
        if (!LoadRetargetClipsFor(srcU8, *refSkel, srcClips, err))
        {
            LOG_ERROR("Batch retarget: '%s' -> %s", srcU8.c_str(), err.c_str());
            if (firstError.empty()) firstError = Sys::PathToUtf8(src.filename()) + ": " + err;
            ++failCount; continue;
        }

        const std::string stem = Sys::PathToUtf8(src.stem());
        Resource::AnimationResource bakedRes;
        uint32_t n = 0;
        for (const ClipAsset& sc : srcClips)
        {
            ClipAsset baked = Resource::RetargetBaker::BakeClip(*refSkel, sc, trgSkel, opts);
            std::string clipName = stem + "_retarget";
            if (n > 0) clipName += "_" + std::to_string(n);
            bakedRes.clips.push_back(
                Resource::RetargetBaker::ToNamedClipData(baked, trgSkel, clipName.c_str()));
            ++n;
        }

        std::vector<uint8_t> blob = Resource::SerializeAnimation(bakedRes);
        // WriteFileBytes creates the parent directories, so the mirrored tree
        // comes into being as we go.
        if (blob.empty() || !WriteFileFully(dstU8, blob))
        {
            LOG_ERROR("Batch retarget: failed to write '%s'", dstU8.c_str());
            if (firstError.empty()) firstError = Sys::PathToUtf8(src.filename()) + ": write failed";
            ++failCount; continue;
        }
        LOG_SUCCESS("Batch retarget: '%s' -> '%s' (%u clip(s), %zu bytes)",
                    Sys::PathToUtf8(rel).c_str(), Sys::RelativizeAssetPath(dstU8).c_str(),
                    n, blob.size());
        ++okCount; clipCount += n;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Batch: %u file(s) -> %u clip(s) written, %u skipped, %u failed (of %zu found).",
                  okCount, clipCount, skipCount, failCount, sources.size());
    outStatus = buf;
    if (failCount && !firstError.empty()) outStatus += " First error - " + firstError;
    if (okCount) LOG_SUCCESS("Batch retarget: %s", outStatus.c_str());
    else         LOG_ERROR("Batch retarget: %s", outStatus.c_str());
    return okCount > 0;
}

// The model (.iscn) @p e was spawned from - its own or an ancestor's SceneSourcePath; "" = none (a
// hand-built entity). Declared in EditorLayerShared.h.
std::string ModelPathOfEntity(World& world, Entity e)
{
    for (int guard = 0; e != NullEntity && guard < 64; ++guard)
    {
        if (const SceneSourcePath* s = world.GetComponent<SceneSourcePath>(e))
        {
            const std::string p = Sys::RelativizeAssetPath(s->path);
            if (p.size() > 5 && Resource::AssetRefs::ToLowerAscii(p.substr(p.size() - 5)) == ".iscn") return p;
        }
        const Parent* par = world.GetComponent<Parent>(e);
        e = par ? par->entity : NullEntity;
    }
    return {};
}

SkeletonAsset* EditorLayer::GetOrLoadRetargetRefSkel()
{
    if (m_rtRefSkelPath.empty()) return nullptr;
    if (m_rtRefSkel && m_rtRefLoadedPath == m_rtRefSkelPath) return m_rtRefSkel.get();

    auto sk = std::make_unique<SkeletonAsset>();
    std::vector<ClipAsset> ignoredClips;
    if (!SceneInstanceLoader::LoadSkeletonFile(m_rtRefSkelPath, *sk, ignoredClips))
        return nullptr;
    m_rtRefSkel       = std::move(sk);
    m_rtRefLoadedPath = m_rtRefSkelPath;
    m_rtRefUpFixed    = false;

    HumanoidProfile& hp = m_rtRefSkel->humanoid;
    if (!hp.userEdited &&
        (!hp.IsValid() || hp.convention == static_cast<uint8_t>(HumanoidConvention::PmxJp)))
        Resource::HumanoidProfileBuilder::Build(*m_rtRefSkel);

    // Up-axis normalization (UE FBX is Z-up: body up arrives as -Z). Rotate the
    // whole skeleton upright; animation channels joined later get the same fix.
    if (Resource::RetargetBaker::ComputeUpAxisCorrection(*m_rtRefSkel, m_rtRefUpFix))
    {
        Resource::RetargetBaker::ApplyUpAxisCorrection(*m_rtRefSkel, m_rtRefUpFix);
        Resource::HumanoidProfileBuilder::FinalizeDerived(*m_rtRefSkel);
        m_rtRefUpFixed = true;
        LOG_INFO("Retarget: source-skeleton up-axis normalized to +Y (Z-up source, e.g. UE)");
    }
    return m_rtRefSkel.get();
}

SkeletonAsset* EditorLayer::GetOrLoadRetargetSource()
{
    if (m_retargetSrcPath.empty()) return nullptr;
    if (m_rtSrcSkel && m_rtSrcLoadedPath == m_retargetSrcPath) return m_rtSrcSkel.get();

    auto sk = std::make_unique<SkeletonAsset>();
    std::vector<ClipAsset> clips;
    if (!SceneInstanceLoader::LoadSkeletonFile(m_retargetSrcPath, *sk, clips))
        return nullptr;
    m_rtSrcSkel       = std::move(sk);
    m_rtSrcClips      = std::move(clips);
    m_rtSrcLoadedPath = m_retargetSrcPath;

    // Initial profile: auto-detect unless hand-tuned (or stale PMX table).
    HumanoidProfile& hp = m_rtSrcSkel->humanoid;
    if (!hp.userEdited &&
        (!hp.IsValid() || hp.convention == static_cast<uint8_t>(HumanoidConvention::PmxJp)))
        Resource::HumanoidProfileBuilder::Build(*m_rtSrcSkel);

    // Up-axis normalization (Z-up sources, e.g. UE): rotate the skeleton AND its embedded clips upright
    // together. .iskel clips carry dense per-bone tracks, so root tracks always exist and the ClipAsset
    // fix applies cleanly.
    DirectX::XMFLOAT4X4 G;
    if (Resource::RetargetBaker::ComputeUpAxisCorrection(*m_rtSrcSkel, G))
    {
        Resource::RetargetBaker::ApplyUpAxisCorrection(*m_rtSrcSkel, G);
        for (ClipAsset& c : m_rtSrcClips)
            Resource::RetargetBaker::ApplyUpAxisCorrectionToClip(c, *m_rtSrcSkel, G);
        Resource::HumanoidProfileBuilder::FinalizeDerived(*m_rtSrcSkel);
        LOG_INFO("Retarget: source up-axis normalized to +Y (Z-up source, e.g. UE)");
    }
    return m_rtSrcSkel.get();
}

void EditorLayer::RenderHumanoidMappingUI()
{
    if (!ImGui::CollapsingHeader("Humanoid Bone Mapping"))
        return;

    ImGui::RadioButton("Target (selected)", &m_rtMapEditTarget, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Source (.iskel)", &m_rtMapEditTarget, 1);

    // ---- Resolve the skeleton being edited ----
    SkeletonAsset* sk = nullptr;
    std::string mapSavePath; // .iskel this mapping saves to (empty = prompt)
    if (m_rtMapEditTarget == 0)
    {
        if (m_world && m_renderer && m_selectedEntity != NullEntity)
        {
            Entity se = m_selectedEntity;
            if (const SkeletonRef* ref = m_world->GetComponent<SkeletonRef>(se))
                if (ref->entity != NullEntity) se = ref->entity;
            if (auto* sc = m_world->GetComponent<SkeletonComponent>(se))
                if (sc->assetIndex != kInvalidAnimHandle &&
                    sc->assetIndex < m_renderer->GetSkeletonRegistry().Count())
                    sk = &m_renderer->GetSkeletonRegistry().Get(sc->assetIndex);
        }
        if (!sk) { ImGui::TextDisabled("Select an entity with a skeleton."); return; }
        // The same rows beside the skeleton itself: a Save there writes the model's .iskel (this block edits
        // the registry's copy).
        const std::string model = ModelPathOfEntity(*m_world, m_selectedEntity);
        ImGui::BeginDisabled(model.empty());
        if (ImGui::SmallButton("Open in Skeleton Editor"))
        {
            Editor::SkeletonEditorWindow::Get().RequestOpen(model);
            Editor::SkeletonEditorWindow::Get().RequestTab(Editor::SkeletonEditorWindow::Tab::Retarget);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(model.empty() ? "The selection was not spawned from a model (.iscn)."
                                            : "The model's skeleton in the Skeleton Editor's Retarget tab: the tree,\n"
                                              "role markers on the joints, undo, and Save into its .iskel.");
    }
    else
    {
        // The retarget's effective source rest pose: the reference-skeleton
        // override when set, else the animation's own skeleton.
        if ((sk = GetOrLoadRetargetRefSkel()) != nullptr)
            mapSavePath = m_rtRefSkelPath;
        else if ((sk = GetOrLoadRetargetSource()) != nullptr)
            mapSavePath = m_retargetSrcPath;
        if (!sk) { ImGui::TextDisabled("Load a source .iskel above first."); return; }
    }
    HumanoidProfile& hp = *&sk->humanoid;

    // ---- Status + actions ----
    uint32_t missing = 0;
    for (uint32_t i = 0; i < kHumanoidBoneCount; ++i)
        if (IsRequiredHumanoidBone(static_cast<HumanoidBone>(i)) && hp.boneForRole[i] < 0) ++missing;
    ImGui::Text("%u bones | convention=%u | %s%s", sk->boneCount, hp.convention,
                hp.IsValid() ? "valid" : "INVALID", hp.userEdited ? " | edited" : "");
    if (missing)
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "%u required role(s) unmapped", missing);

    if (ImGui::Button("Auto-Detect"))
    {
        hp.userEdited = 0;
        Resource::HumanoidProfileBuilder::Build(*sk);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All"))
    {
        hp.Reset();
        hp.userEdited = 1;
        Resource::HumanoidProfileBuilder::FinalizeDerived(*sk);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to .iskel..."))
    {
        std::string path = !mapSavePath.empty() ? mapSavePath
            : OpenFileDialog("Skeleton (*.iskel)\0*.iskel\0All Files\0*.*\0\0", "asset/");
        if (!path.empty())
        {
            std::string why;
            if (Editor::SaveHumanoidProfileToIskel(path, hp, &why))
                LOG_SUCCESS("HumanoidMapping: saved profile -> '%s'", path.c_str());
            else
                LOG_ERROR("HumanoidMapping: failed to save profile -> '%s': %s", path.c_str(), why.c_str());
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the mapping into the .iskel's humanoid section so it\n"
                          "persists across editor restarts. Source saves to the loaded\n"
                          ".iskel; Target prompts for its .iskel file.");

    // ---- Categorized slot list (Unity Avatar style) ----
    // The rows are Editor/HumanoidMapping's, shared with the Skeleton Editor's Retarget tab (a change
    // sets userEdited = 1 and runs FinalizeDerived).
    Editor::DrawHumanoidRoleRows(*sk, m_rtBoneFilter, sizeof(m_rtBoneFilter));
}

void EditorLayer::RenderRetargetWindow()
{
    if (!m_showRetargetWindow) return;
    if (!ImGui::Begin("Animation Retarget", &m_showRetargetWindow))
    { ImGui::End(); return; }

    ImGui::TextWrapped("Retarget a humanoid animation (Mixamo / VRM / FBX .iskel, which carries "
                       "its skeleton + clips) onto the selected entity's skeleton (e.g. a PMX character).");
    ImGui::Separator();

    ImGui::Text("Target: ");
    ImGui::SameLine();
    if (m_selectedEntity == NullEntity)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "select an entity with a skeleton");
    else
        ImGui::Text("entity %u", static_cast<uint32_t>(m_selectedEntity));

    // ---- Source slot: animation (.iskel with clips, or .ianim) ----
    ImGui::Spacing();
    ImGui::TextUnformatted("Source animation (.iskel / .ianim):");
    {
        const std::string label = m_retargetSrcPath.empty()
            ? std::string("  drop an .iskel / .ianim here, or Browse...  ")
            : m_retargetSrcPath;
        ImGui::Button(label.c_str(), ImVec2(-1.f, 0.f));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ISKEL_PATH"))
                if (p->Data) m_retargetSrcPath.assign(static_cast<const char*>(p->Data));
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
                if (p->Data) m_retargetSrcPath.assign(static_cast<const char*>(p->Data));
            ImGui::EndDragDropTarget();
        }
    }
    if (ImGui::Button("Browse..."))
    {
        std::string p = OpenFileDialog(
            "Animation (*.iskel;*.ianim)\0*.iskel;*.ianim\0All Files\0*.*\0\0", "asset/");
        if (!p.empty()) m_retargetSrcPath = p;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) m_retargetSrcPath.clear();

    // ---- Optional source-skeleton override (reference pose) ----
    // Needed for UE: animation-only FBX exports do not carry the mannequin's reference pose - import the
    // skeletal-mesh FBX (SKM_Manny) once and set its .iskel here. Mixamo animation files embed their own
    // T-pose: leave it empty.
    ImGui::Spacing();
    ImGui::TextUnformatted("Source skeleton (optional, reference pose):");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE animation FBX files don't contain the mannequin's reference\n"
                          "pose. Import the skeletal-mesh FBX (e.g. SKM_Manny) as a Model\n"
                          "once, then set its .iskel here; the animation file above then\n"
                          "only supplies clips (joined by bone name).\n"
                          "Mixamo sources embed their own T-pose: leave this empty.");
    {
        const std::string label2 = m_rtRefSkelPath.empty()
            ? std::string("  (none — use the animation's own skeleton)  ")
            : m_rtRefSkelPath;
        ImGui::Button(label2.c_str(), ImVec2(-1.f, 0.f));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ISKEL_PATH"))
                if (p->Data) m_rtRefSkelPath.assign(static_cast<const char*>(p->Data));
            ImGui::EndDragDropTarget();
        }
    }
    if (ImGui::Button("Browse##refskel"))
    {
        std::string p = OpenFileDialog(
            "Skeleton (*.iskel)\0*.iskel\0All Files\0*.*\0\0", "asset/");
        if (!p.empty()) m_rtRefSkelPath = p;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##refskel")) m_rtRefSkelPath.clear();

    // ---- Rest-pose compensation ----
    ImGui::Separator();
    ImGui::TextWrapped("Rest pose (arms). Mark a rig A-pose to straighten its arms to a T-pose "
                       "reference so motion matches instead of over-bending.");
    const char* poses[] = { "T-pose", "A-pose" };
    int srcPose = m_rtSrcAPose ? 1 : 0, trgPose = m_rtTrgAPose ? 1 : 0;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("Source pose", &srcPose, poses, 2)) m_rtSrcAPose = (srcPose == 1);
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("Target pose", &trgPose, poses, 2)) m_rtTrgAPose = (trgPose == 1);

    // ---- Options ----
    ImGui::Separator();
    ImGui::Checkbox("Upper body", &m_rtUpper); ImGui::SameLine();
    ImGui::Checkbox("Lower body", &m_rtLower);
    ImGui::Checkbox("Fingers", &m_rtFingers);
    if (!m_rtLower) ImGui::BeginDisabled();
    ImGui::Checkbox("Root motion", &m_rtRoot); ImGui::SameLine();
    ImGui::Checkbox("Canonical legs", &m_rtCanonLegs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap both rigs' thigh/shin REST frames to straight-down before\n"
                          "the delta, so the source's leg stance transfers instead of the\n"
                          "target's own rest splay surviving.\n\n"
                          "UE thighs splay ~3 deg outward, MMD/PMX thighs ~3 deg inward:\n"
                          "without this the retargeted knees/ankles end up far too close\n"
                          "together and the shins can cross. Uncheck for the old behavior.");
    if (!m_rtLower) ImGui::EndDisabled();
    ImGui::Checkbox("Canonical feet", &m_rtCanonFeet); ImGui::SameLine();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap both rigs' FOOT rest frames to (body-forward, body-up roll)\n"
                          "before the delta, the same way the legs are handled.\n\n"
                          "UE's rest foot points 29 deg below horizontal and toes out 7 deg;\n"
                          "this PMX rig's points 39 deg down and dead ahead — 12 deg apart,\n"
                          "which the delta preserves, leaving the foot permanently over-\n"
                          "pointed. Measured: foot direction error vs the source 8.1 -> 6.4 deg.");
    ImGui::Checkbox("Canonical neck/head", &m_rtCanonNeck);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Same for the neck and head (straight up the body axis).\n"
                          "Measured: neck direction error vs the source 9.3 -> 2.7 deg.\n\n"
                          "Spine/chest are deliberately NOT canonicalized — measured, it\n"
                          "makes the chest much worse (4.5 -> 16.1 deg): spine curvature is\n"
                          "authored posture and the rigs spread it over different bone counts.");
    ImGui::Checkbox("Spread twist onto 捩 bones", &m_rtTwist);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Move each limb bone's AXIAL roll onto the rig's twist bone\n"
                          "(MMD 腕捩 / 手捩) instead of leaving it all on 腕/ひじ.\n\n"
                          "The transfer writes absolute bone orientations, so the whole\n"
                          "roll lands on one joint — but the mesh around the elbow is\n"
                          "skinned to 腕捩1/2/3 and 手捩1/2/3, which take 25/50/75%% of\n"
                          "the twist bone via 回転付与 so it becomes a gradient. Without\n"
                          "this the elbow is visibly wrung. The split is exact: the pose\n"
                          "of the hand and everything below is unchanged.");
    ImGui::Checkbox("Save as .ianim", &m_rtSaveIanim);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Persist the baked animation to disk (prompts for a path).\n"
                          "The saved .ianim is a normal animation asset: drag it onto the\n"
                          "model from the Asset browser after an editor restart.");

    // ---- Humanoid bone mapping (Unity-Avatar-style manual fine-tuning) ----
    ImGui::Separator();
    RenderHumanoidMappingUI();

    // ---- Run ----
    ImGui::Separator();
    const bool canRun = m_selectedEntity != NullEntity && !m_retargetSrcPath.empty();
    if (!canRun) ImGui::BeginDisabled();
    if (ImGui::Button("Retarget", ImVec2(-1.f, 0.f)))
        DoRetarget(m_retargetSrcPath, m_retargetStatus);
    if (!canRun) ImGui::EndDisabled();

    if (!m_retargetStatus.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_retargetStatus.c_str());
    }

    // ---- Batch: a whole folder tree at once ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Batch retarget (folder)"))
    {
        ImGui::TextWrapped(
            "Walks the source folder recursively and retargets every animation it finds "
            "(.fbx .gltf .glb .dae .bvh .ianim .iskel), writing one .ianim per file into "
            "the output folder under the SAME relative subfolders. Uses the options above. "
            "Needs the Source skeleton (.iskel) set, since every file is joined to that one "
            "reference pose by bone name.");

        auto folderRow = [](const char* label, const char* id, std::string& dir)
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", dir.c_str());
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(-70.f);
            if (ImGui::InputText(label, buf, sizeof(buf))) dir = buf;
            ImGui::SameLine();
            if (ImGui::Button("..."))
            {
                std::string picked = OpenFolderDialog(dir.empty() ? "asset/" : dir.c_str());
                if (!picked.empty()) dir = picked;
            }
            ImGui::PopID();
        };
        folderRow("Source folder", "batchsrc", m_rtBatchSrcDir);
        folderRow("Output folder", "batchout", m_rtBatchOutDir);

        ImGui::Checkbox("Overwrite existing .ianim", &m_rtBatchOverwrite);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: files that already exist in the output tree are skipped,\n"
                              "so an interrupted batch can be resumed cheaply.\n"
                              "A source is never overwritten by its own output either way.");

        const bool canBatch = m_selectedEntity != NullEntity &&
                              !m_rtBatchSrcDir.empty() && !m_rtBatchOutDir.empty() &&
                              !m_rtRefSkelPath.empty();
        if (!canBatch) ImGui::BeginDisabled();
        if (ImGui::Button("Run batch retarget", ImVec2(-1.f, 0.f)))
            DoBatchRetarget(m_rtBatchSrcDir, m_rtBatchOutDir, m_rtBatchStatus);
        if (!canBatch) ImGui::EndDisabled();
        // AllowWhenDisabled: the whole point of this tooltip is to explain the
        // disabled state, and a disabled item reports no hover without it.
        if (!canBatch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Needs: a selected target entity, a Source skeleton (.iskel), "
                              "and both folders.");

        if (!m_rtBatchStatus.empty()) ImGui::TextWrapped("%s", m_rtBatchStatus.c_str());
    }

    ImGui::End();
}

// RenderMenuBar and its per-menu functions live in EditorLayer_MainMenu.cpp;
// the Create menu items in EditorLayer_CreateMenu.cpp.

void EditorLayer::SetupDockSpace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("EditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    if (ImGui::BeginMenuBar())
    {
        RenderMenuBar();
        ImGui::EndMenuBar();
    }

    // File > New Scene / Open Scene... / Open Recent (and a host-requested quit) stage a
    // confirmation only. The body must be here, after EndMenuBar: a modal opened from inside the menu
    // bar needs its BeginPopupModal in the caller's ID stack, or ImGui silently never shows it.
    RenderUnsavedChangesPopup();
    RenderAutosaveRecoveryPopup();
    MaybeAutosave();

    // File > Import > Texture... (or the Content Browser's Import, or a dropped image) only stages
    // files and sets m_openTextureImportPopup; this EditorHost scope opens the modal. Confirmed work
    // then runs from ProcessPendingActions before ImGui starts.
    RenderTextureImportPopup();
    RenderModelReimportReview();
    RenderModelImportDialog();
    RenderModelImportProgress();

    // File > Import > Unreal Scene (manifest)... progress modal - same deferred popup placement; the
    // cook itself runs on a TaskSystem worker.
    RenderUnrealImportPopup();

    // Tools > Bake > Collision Meshes... popup. Body lives here (outside the
    // menu bar) so the modal's ID stack matches OpenPopup's caller scope.
    if (m_bakeCollisionOpenRequest)
    {
        ImGui::OpenPopup("Bake Collision Meshes");
        m_bakeCollisionOpenRequest = false;
    }
    {
        // Members, so Editor.BakeCollision drives the same bake.
        Tools::CollisionMesh::BakeOptions& s_opts = m_collisionBakeOpts;
        Tools::CollisionMesh::BakeJob&     s_job  = m_collisionBake;
        static char s_outPath[260] = {};
        static bool s_outPathInit  = false;
        if (!s_outPathInit)
        {
            std::snprintf(s_outPath, sizeof(s_outPath), "%s", s_opts.outputPath.c_str());
            s_outPathInit = true;
        }

        // The preview-swap state is at file scope (g_previewBackups / g_previewBakedLib /
        // g_previewLoadedFor) so ProcessPendingBakePreview can reach it.

        ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Bake Collision Meshes",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool running     = s_job.IsRunning();
            const bool hasPreview  = !g_previewBackups.empty();
            // A preview load / restore already queued for the frame boundary. ProcessPendingActions drains it
            // before the next OnUIRender, so this is false here in practice; kept as an explicit invariant so a
            // caller that skips the drain gets a disabled button, not two queued ops (overwritten intent or a
            // double Release).
            const bool previewQueued =
                (m_pendingBakePreviewOp != PendingBakePreviewOp::None);

            ImGui::TextWrapped(
                "Walks every MeshLibRef entity in the current world, simplifies "
                "each source mesh in parallel via meshoptimizer, and combines "
                "all results into ONE .meshlib (shared VB/IB + per-mesh entries "
                "— engine-native format). Use \"Preview baked\" to render the "
                "simplified meshes in-place for visual comparison.");
            ImGui::Separator();

            // ---- Options form (disabled while a bake is in flight) ----
            ImGui::BeginDisabled(running);
            EditorUI::DragFloat("Target triangle ratio",
                                &s_opts.targetTriangleRatio, 0.01f, 1.0f, "%.2f");
            EditorUI::DragFloat("Target error (normalized)",
                                &s_opts.targetError, 0.001f, 0.5f, "%.3f");
            int minTri = static_cast<int>(s_opts.minTriangles);
            if (ImGui::InputInt("Min triangles", &minTri))
                s_opts.minTriangles = static_cast<uint32_t>(std::max(0, minTri));

            ImGui::Checkbox("Skip skinned", &s_opts.skipSkinned);

            if (ImGui::InputText("Output .meshlib path", s_outPath, sizeof(s_outPath)))
                s_opts.outputPath = s_outPath;
            ImGui::EndDisabled();

            ImGui::Text("Worker threads: %u",  TaskSystem::Get().GetWorkerCount());
            ImGui::Separator();

            const bool hasWorld = (m_world != nullptr);
            if (!hasWorld)
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "No world bound.");

            // ---- Action buttons row ----
            ImGui::BeginDisabled(!hasWorld || running || hasPreview);
            if (ImGui::Button("Bake"))
            {
                s_opts.outputPath = s_outPath;
                s_job.Begin(*m_world, s_opts);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!running);
            if (ImGui::Button("Cancel"))
                s_job.Reset();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(running || hasPreview);
            if (ImGui::Button("Clear results"))
                s_job.Reset();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(running);
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();

            // ---- Progress bar ----
            if (running)
            {
                s_job.Tick();
                const float prog = s_job.Progress();
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%u / %u  (%.0f%%)",
                              s_job.Processed(), s_job.Total(), prog * 100.f);
                ImGui::ProgressBar(prog, ImVec2(-FLT_MIN, 0), overlay);
            }

            // ---- Preview row ----
            if (s_job.IsComplete() && !s_job.WriteFailed())
            {
                ImGui::Separator();
                const bool canPreview =
                    hasWorld && m_renderer && m_gfx && !hasPreview && !previewQueued;
                ImGui::BeginDisabled(!canPreview);
                if (ImGui::Button("Preview baked in world"))
                {
                    // Queue only - ProcessPendingBakePreview runs the load. MeshLibrary::Load creates the baked VB/IB
                    // through IGraphicsDevice::CreateBuffer, which can trip FlushUploadAndWait (Close / Execute /
                    // FlushAndWait / Reset on the shared upload command list) and registers fresh bindless slots;
                    // inside BeginImGuiFrame..EndImGuiFrame the primary list is open and this frame's scene is built
                    // off the old bindless table, so neither is legal here. Both halves of the preview (this and
                    // "Restore originals") share g_previewBakedLib and mutate from the same point in the frame.
                    // The payload (output path + the (sourcePath, meshId) -> bakedMeshId table) is snapshotted here:
                    // it is pure CPU work over s_job, a function-local static the servicer cannot see.
                    m_pendingBakePreviewPath = s_job.OutputPath();
                    m_pendingBakePreviewMap.clear();
                    for (const auto& r : s_job.Results())
                    {
                        if (!r.success) continue;
                        m_pendingBakePreviewMap[r.sourcePath][r.sourceMeshId] = r.bakedMeshId;
                    }
                    m_pendingBakePreviewOp = PendingBakePreviewOp::Load;
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!hasPreview || previewQueued);
                if (ImGui::Button("Restore originals"))
                {
                    // Queue only - ProcessPendingBakePreview runs the restore. Its tail calls MeshLibrary::Release +
                    // Renderer::OnWorldClear, which bumps MeshManager's generation, drops the DDGI BLAS cache and wipes
                    // m_runtimeMaterialResources with no GPU sync of its own, relying on the caller having synced
                    // before anything was recorded this frame. Inline it would free descriptor slots this frame's
                    // RenderGraph and ImGui::Image handles already reference - the reason the Load Scene teardown is
                    // deferred too.
                    m_pendingBakePreviewOp = PendingBakePreviewOp::Restore;
                }
                ImGui::EndDisabled();

                if (hasPreview)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.f, 1),
                        "(previewing %zu entities)", g_previewBackups.size());
                }

                // ---- Collision wire-up ----
                // UseBakedAsCollision: a writable model's parts get the bake as the MODEL's asset collision (every
                // placement follows), anything else a Mesh collider of its own; one undo step. Not in Play (a body
                // swapped mid-play outlives its entity) and not while the bake is previewed (the world shows the
                // baked meshes).
                const bool playing = GetPlayState() != ViewportPlayState::Stopped;
                const bool canApplyCollision = hasWorld && !running && !playing && !hasPreview && !previewQueued;
                ImGui::BeginDisabled(!canApplyCollision);
                if (ImGui::Button("Use baked as collision"))
                    UseBakedAsCollision();
                ImGui::EndDisabled();
                if (playing || hasPreview)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled(playing ? "(stop Play first)" : "(restore the originals first)");
                }
                const BakedCollisionReport& lr = m_lastBakedCollision;
                if (!lr.error.empty())
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", lr.error.c_str());
                else if (!lr.models.empty() || lr.perEntity > 0)
                {
                    ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1),
                        "%zu model(s) got it as asset collision (%u Mesh(es)); %u entit(ies) a collider of their own",
                        lr.models.size(), lr.modelMeshes, lr.perEntity);
                    for (const auto& [m, why] : lr.perEntityModels)
                        ImGui::TextDisabled("  %s: %s", m.c_str(), why.c_str());
                }

            }

            // ---- Summary / results ----
            if (s_job.IsComplete() || !s_job.Results().empty())
            {
                ImGui::Separator();
                const auto& results = s_job.Results();
                uint32_t ok = 0, fail = 0, skip = 0;
                for (const auto& r : results)
                {
                    if (r.skipped)      ++skip;
                    else if (r.success) ++ok;
                    else                ++fail;
                }
                ImGui::Text("Baked: %u    Failed: %u    Skipped: %u",
                            ok, fail, skip);
                if (s_job.IsComplete())
                {
                    if (s_job.WriteFailed())
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                            "Write FAILED — see DX12Log.txt");
                    else if (ok > 0)
                        ImGui::TextColored(ImVec4(0.5f, 1.f, 0.5f, 1),
                            "Wrote '%s'  (%llu -> %llu tris total)",
                            s_job.OutputPath().c_str(),
                            (unsigned long long)s_job.OrigTriTotal(),
                            (unsigned long long)s_job.BakedTriTotal());
                    else
                        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1),
                            "No meshes survived — nothing written.");
                }
                if (ImGui::BeginChild("##BakeResultsList", ImVec2(0, 240), true))
                {
                    for (const auto& r : results)
                    {
                        if (r.skipped)
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
                                "[SKIP] %s  (%s)",
                                r.sourcePath.empty() ? "(no source)"
                                                     : r.sourcePath.c_str(),
                                r.reason.c_str());
                        else if (r.success)
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1),
                                "[ OK ] %s [src=%u baked=%u]   %u -> %u tris   err %.4f",
                                r.sourcePath.c_str(), r.sourceMeshId, r.bakedMeshId,
                                r.origTriCount, r.bakedTriCount, r.actualError);
                        else
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                                "[FAIL] %s  %s",
                                r.sourcePath.empty() ? "(no source)"
                                                     : r.sourcePath.c_str(),
                                r.reason.c_str());
                    }
                }
                ImGui::EndChild();
            }

            ImGui::EndPopup();
        }
    }

    ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Default layout: left Hierarchy | center Viewport over Content Browser + Output Log (one node, two
    // tabs) | right Inspector. Built only when imgui.ini did not restore a dock tree (else the user's
    // layout would be wiped every launch) or when Window > Reset Layout asks for it. Under the harness
    // this tree IS the layout and packs measure the Viewport rect: keep the split ratios.
    if (!m_layoutInitialized || m_resetLayoutRequested)
    {
        m_layoutInitialized = true;
        const bool reset = m_resetLayoutRequested;
        m_resetLayoutRequested = false;

        ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspaceId);
        // -uitest_reverse=layout.reset: a reset leaves a split tree as it is, like a launch that restored one.
        const bool rebuild = reset && !Editor::UITests::Reverted(Editor::UITests::Fix::LayoutReset);
        const bool needDefault = rebuild || (existing == nullptr) || !existing->IsSplitNode();
        if (needDefault)
        {
            ImGui::DockBuilderRemoveNodeChildNodes(dockspaceId);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

            ImGuiID leftId, rightPartId;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.25f, &leftId, &rightPartId);

            ImGuiID inspectorId, centerPartId;
            ImGui::DockBuilderSplitNode(rightPartId, ImGuiDir_Right, 0.3f, &inspectorId, &centerPartId);

            ImGuiID bottomId, viewportId;
            ImGui::DockBuilderSplitNode(centerPartId, ImGuiDir_Down, 0.2f, &bottomId, &viewportId);

            ImGui::DockBuilderDockWindow(kWindowHierarchy, leftId);
            ImGui::DockBuilderDockWindow(kWindowViewport, viewportId);
            ImGui::DockBuilderDockWindow(kWindowInspector, inspectorId);
            ImGui::DockBuilderDockWindow(kWindowContentBrowser, bottomId);
            ImGui::DockBuilderDockWindow(kWindowOutputLog, bottomId);
            // Tab ORDER in the shared node: ImGui sorts by DockOrder, and DockBuilderDockWindow resets it to -1
            // (order of first appearance; the log is created first - see OnUIRender). Pin Content Browser |
            // Output Log explicitly: on the live window after a reset, or on its settings before it exists.
            {
                short order = 0;
                for (const char* name : { kWindowContentBrowser, kWindowOutputLog })
                {
                    const ImGuiID id = ImHashStr(name);
                    if (ImGuiWindow* w = ImGui::FindWindowByID(id))
                        w->DockOrder = order;
                    else if (ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(id))
                        ws->DockOrder = order;
                    ++order;
                }
            }
            ImGui::DockBuilderFinish(dockspaceId);
            // Front tab of the bottom node after a RESET, when both windows already exist (at startup creation
            // order decides - see OnUIRender). The rebuilt node's tab bar adopts node->SelectedTabId; a docked
            // window's tab ID is GetID("#TAB") inside that window, i.e. "#TAB" seeded with the window ID.
            if (ImGuiDockNode* bottom = ImGui::DockBuilderGetNode(bottomId))
                bottom->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr(kWindowContentBrowser));
            if (reset)
                LOG_INFO("Editor: layout reset to the default dock tree");
        }
    }
    // Before any docked window of the frame is submitted, like the default layout above.
    ApplyGameViewDock();

    ImGui::End();
}

std::string EditorLayer::ComponentLabelFor(std::type_index type) const
{
    const auto it = m_componentLabels.find(type);
    return it != m_componentLabels.end() ? it->second : std::string("Component");
}

// Change-detection fingerprint for a component: the entry's cheap byte fingerprint
// (trivially-copyable types), else the serializer blob. Empty when neither exists (e.g. Material);
// the caller then treats the edit as always changed.
std::string EditorLayer::ComponentFingerprint(
    const std::function<std::string(World&, Entity)>& fingerprint, Entity e, std::type_index type)
{
    if (fingerprint)
    {
        std::string s = fingerprint(*m_world, e);
        if (!s.empty()) return s;
    }
    if (Editor::Undo::HasSerializer(GetComponentRegistry(), type))
        return Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), *m_world, e, type);
    return {};
}

void EditorLayer::DrawComponentBodyWithUndo(Entity e, std::type_index type,
                                            const std::function<void()>& drawBody)
{
    const auto it = m_componentEditors.find(type);
    if (it == m_componentEditors.end())
    {
        drawBody();   // unregistered type: no snapshot to take, edit un-recorded
        return;
    }
    DrawComponentWithUndo(e, type, it->second.captureValue, it->second.fingerprint, drawBody);
}

bool EditorLayer::DrawComponentSection(Entity e, std::type_index type)
{
    if (!m_world || !m_world->IsAlive(e)) return false;
    const auto it = m_componentEditors.find(type);
    if (it == m_componentEditors.end() || !it->second.draw || !it->second.fetch) return false;
    ComponentEditorEntry& entry = it->second;
    void* comp = entry.fetch(*m_world, e);
    if (!comp) return false;
    ImGui::PushID(entry.label.c_str());
    const bool open = ImGui::CollapsingHeader(entry.label.c_str(), entry.defaultCollapsed
                                                                       ? ImGuiTreeNodeFlags_None
                                                                       : ImGuiTreeNodeFlags_DefaultOpen);
    bool removed = false;
    if (entry.remove && ImGui::BeginPopupContextItem("##comp_ctx"))
    {
        if (ImGui::MenuItem("Remove Component"))
        {
            // The Inspector's removal: the data first (undo restores it), then the component.
            if (UndoEnabled())
            {
                SyncedEditContext();
                std::string blob = Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), *m_world, e, type);
                PushEdit(std::make_unique<Editor::Undo::RemoveComponentAction>(
                    m_world->MakeHandle(e), type, entry.add, entry.remove, std::move(blob), ComponentLabelFor(type)));
            }
            entry.remove(*m_world, e);
            MarkHierarchyDirty();
            // The step was recorded before the removal: tell a UI asset tab's sink now
            // that it is done (it captures its stage); the scene is dirty already.
            if (m_editSink) MarkSceneDirty();
            removed = true;
        }
        ImGui::EndPopup();
    }
    if (open && !removed && !entry.module.empty() && !Module::Registry::Get().IsEnabled(entry.module))
        ImGui::TextDisabled("Module %s is off in this project: the data is kept, nothing runs it.",
                            entry.module.c_str());
    if (open && !removed)
    {
        // A multi-selection: the reflected fields that differ across it are drawn mixed.
        const bool multi = m_selectedEntities.size() > 1 &&
            std::find(m_selectedEntities.begin(), m_selectedEntities.end(), e) != m_selectedEntities.end();
        const std::vector<const Reflect::FieldDescriptor*> mixed =
            multi ? MixedReflectedFields(e, type) : std::vector<const Reflect::FieldDescriptor*>();
        if (multi) Reflect::SetMultiSelection(&mixed, static_cast<int>(m_selectedEntities.size()));
        DrawComponentWithUndo(e, type, entry.captureValue, entry.fingerprint,
                              [&entry, comp, this, e] { entry.draw(comp, m_world, e); });
        if (multi) Reflect::SetMultiSelection(nullptr, 1);
    }
    ImGui::PopID();
    return true;
}

void EditorLayer::AddComponentWithUndo(Entity e, std::type_index type,
                                       std::function<void(World&, Entity)> add)
{
    if (!m_world || !add || !m_world->IsAlive(e)) return;
    // Same deferral + epoch/handle guard as the Inspector's Add Component menu:
    // structural edits run at the frame boundary, never mid-ImGui.
    World* targetWorld = m_world;
    const auto         epoch  = targetWorld->Epoch();
    const EntityHandle handle = targetWorld->MakeHandle(e);
    // The sink in scope now is where the add lands later (a UI asset tab's ring); shared, so a tab
    // closed in between leaves a ring nobody reads, never a dangling one.
    m_pendingComponentAdds.emplace_back(
        [this, targetWorld, epoch, handle, type, add = std::move(add), sink = m_editSink]() {
            if (m_world != targetWorld || m_world->Epoch() != epoch ||
                !m_world->IsHandleValid(handle)) return;
            SinkScope scope(*this, sink);
            const auto it = m_componentEditors.find(type);
            if (it == m_componentEditors.end() || !it->second.remove) return;
            if (it->second.fetch && it->second.fetch(*m_world, handle.entity)) return; // already there
            add(*m_world, handle.entity);
            MarkHierarchyDirty();
            if (UndoEnabled())
            {
                SyncedEditContext();
                PushEdit(std::make_unique<Editor::Undo::AddComponentAction>(
                    handle, add, it->second.remove, ComponentLabelFor(type)));
            }
        });
}

void EditorLayer::DrawComponentWithUndo(Entity e, std::type_index type,
                                        const Editor::Undo::CaptureFn& capture,
                                        const std::function<std::string(World&, Entity)>& fingerprint,
                                        const std::function<void()>& drawBody)
{
    // The Chain Physics Preview's stage (PreviewStageTag) is not user content. An edit there must
    // (a) never enter the global undo stack - the stage is destroyed on every close / Play, leaving dead
    // Ctrl+Z steps; (b) never mark the scene dirty (autosave, "unsaved changes"); (c) never propagate to
    // the scene selection - the stage is never in it, so a multi-selection would receive the preview's
    // parameters. (a)+(b): a stage entity is not recorded at all (the panel keeps its own Play-boundary
    // copy, m_staged); (c) is enforced in general below - propagation is only for an edit of a SELECTED
    // entity. -chainpreview_edit_gate_off (harness reverse arm) drops both gates so the probe pack can
    // prove they bite.
    bool gateOn = true;
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_editGateOff =
        Sys::CommandLine::Get().Has("-chainpreview_edit_gate_off");
    gateOn = !s_editGateOff;
#endif
    const bool stageEntity = m_world && m_world->HasComponent<PreviewStageTag>(e);
    // A UI Editor asset tab's stage IS edited - through the tab's sink (its ring, its dirty file), never
    // the global stack. With no sink in scope a stage stays unrecorded, as above.
    const bool record = m_world && UndoEnabled() && static_cast<bool>(capture) &&
                        !(gateOn && stageEntity && !m_editSink);

    // Snapshot the pre-edit value + fingerprint while no bracket is open: this is the state that becomes
    // "before" if an edit begins this frame.
    Editor::Undo::RestoreFn restoreBefore;
    std::string             fpBefore;
    std::string blobBefore;
    std::shared_ptr<void>   snapshotBefore;
    bool                    propagate = false;
    if (record && !m_fieldEdit.open)
    {
        restoreBefore = capture(*m_world, e);
        fpBefore      = ComponentFingerprint(fingerprint, e, type);
        // Only worth serializing when there is somewhere to propagate to - and only when the edited entity
        // IS part of that selection.
        const bool inSelection =
            std::find(m_selectedEntities.begin(), m_selectedEntities.end(), e) !=
            m_selectedEntities.end();
        propagate = m_selectedEntities.size() > 1 && (inSelection || !gateOn);
        if (propagate)
            blobBefore = Editor::Undo::SerializeComponentBlob(
                GetComponentRegistry(), *m_world, e, type);
        // A reflected component commits as the fields the gesture changed (CommitReflectedEdit).
        const auto entryIt = m_componentEditors.find(type);
        if (entryIt != m_componentEditors.end() && entryIt->second.fetch)
            snapshotBefore = SnapshotReflected(type, entryIt->second.fetch(*m_world, e));
    }

    const ImGuiID beforeId = ImGui::GetActiveID();
    // A drag-and-drop delivered INTO this component (a texture / font / shader path onto a reflected
    // field) activates no widget, so the bracket never opens; it gets its own undo entry below. Delivery
    // turns on the frame a target accepts the payload: off before this body and on after it = this body
    // took the drop.
    const ImGuiPayload* dropBefore = ImGui::GetDragDropPayload();
    const bool deliveredBefore = dropBefore && dropBefore->Delivery;
    drawBody();
    const ImGuiID afterId  = ImGui::GetActiveID();
    const ImGuiPayload* dropAfter = ImGui::GetDragDropPayload();
    const bool droppedHere = !deliveredBefore && dropAfter && dropAfter->Delivery;

    if (!record) return;

    // A row's right-click Paste / Reset writes from a menu item, which holds no active id either.
    const bool rowActionHere = Reflect::ConsumeRowActionEdit();
    if (!m_fieldEdit.open && (droppedHere || rowActionHere))
    {
        const std::string fpAfter = ComponentFingerprint(fingerprint, e, type);
        const bool haveFp = !fpBefore.empty() || !fpAfter.empty();
        if ((haveFp ? fpBefore != fpAfter : true) && restoreBefore && m_world->IsAlive(e))
        {
            EditTarget().BeginMacro(ComponentLabelFor(type));
            const bool asFields = snapshotBefore && CommitReflectedEdit(e, type, snapshotBefore.get(), propagate);
            if (!asFields)
                PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(
                    m_world->MakeHandle(e), std::move(restoreBefore), capture(*m_world, e),
                    ComponentLabelFor(type)));
            EditTarget().EndMacro();
        }
        return;
    }

    // -uitest_reverse=inspector.drag_bracket: a held widget commits every other frame and opens again, each
    // piece of the gesture its own undo step (what the bracket is for).
    const bool perFrame = Editor::UITests::Reverted(Editor::UITests::Fix::InspectorDragBracket);
    if (!m_fieldEdit.open)
    {
        // A widget in THIS component became active this frame -> begin the bracket.
        const bool reopen = perFrame && afterId == m_fieldEdit.activeId && m_fieldEdit.entity == e &&
                            m_fieldEdit.type == type;
        if (afterId != 0 && (afterId != beforeId || reopen))
        {
            m_fieldEdit.open          = true;
            m_fieldEdit.entity        = e;
            m_fieldEdit.type          = type;
            m_fieldEdit.activeId      = afterId;
            m_fieldEdit.restoreBefore = std::move(restoreBefore);
            m_fieldEdit.fpBefore      = std::move(fpBefore);
            m_fieldEdit.blobBefore    = std::move(blobBefore);
            m_fieldEdit.snapshot      = std::move(snapshotBefore);
            m_fieldEdit.propagate     = propagate;
        }
    }
    else if (m_fieldEdit.entity == e && m_fieldEdit.type == type)
    {
        // The edited widget is no longer the active one -> the gesture ended.
        if (afterId != m_fieldEdit.activeId || perFrame)
        {
            const std::string fpAfter = ComponentFingerprint(fingerprint, e, type);
            // Changed if fingerprints differ; if neither has a fingerprint
            // (e.g. Material), we can't tell, so assume it changed.
            const bool haveFp  = !m_fieldEdit.fpBefore.empty() || !fpAfter.empty();
            const bool changed = haveFp ? (m_fieldEdit.fpBefore != fpAfter) : true;

            if (changed && m_world->IsAlive(e))
            {
                Editor::Undo::RestoreFn restoreAfter = capture(*m_world, e);
                // One macro so the primary's edit and the copies it pushes onto the rest of the selection are a
                // single Ctrl+Z. EndMacro unwraps a one-action macro, so a single-selection edit records one plain
                // action.
                EditTarget().BeginMacro(ComponentLabelFor(type));
                // A reflected edit is the fields it changed (undo puts back only those, a multi-selection
                // receives only those); anything else is the whole value and the serializer keys.
                const bool asFields = m_fieldEdit.snapshot &&
                    CommitReflectedEdit(e, type, m_fieldEdit.snapshot.get(), m_fieldEdit.propagate);
                if (!asFields)
                {
                    PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(
                        m_world->MakeHandle(e), std::move(m_fieldEdit.restoreBefore),
                        std::move(restoreAfter), ComponentLabelFor(type)));
                    if (!m_fieldEdit.blobBefore.empty())
                        PropagateFieldEditToSelection(
                            e, type, m_fieldEdit.blobBefore,
                            Editor::Undo::SerializeComponentBlob(
                                GetComponentRegistry(), *m_world, e, type));
                }
                EditTarget().EndMacro();
            }
            m_fieldEdit.open          = false;
            m_fieldEdit.restoreBefore = {};
            m_fieldEdit.fpBefore.clear();
            m_fieldEdit.blobBefore.clear();
            m_fieldEdit.snapshot.reset();
            m_fieldEdit.propagate     = false;
        }
    }
}

// Safety net for a bracket left open because its component stopped being drawn (selection moved
// away, or the entity was destroyed mid-edit while a widget was held). Drop the interrupted edit
// rather than commit it without the component's draw context.
void EditorLayer::FinalizeStaleFieldEdit()
{
    if (!m_fieldEdit.open) return;

    // The Animation Set window, the Morph editor and the Chain Physics Preview's collider editor edit a
    // LATCHED target that may differ from m_selectedEntity: a bracket owned by one is not stale just
    // because the hierarchy selection moved. A UI Editor asset tab's Details edit the tab's selection,
    // not the scene's. (The preview panel's Play LIVE section brackets with UndoEnabled() false, so it
    // opens no bracket.)
    const bool editedByWindow =
        (m_inspectorLocked && m_inspectorLockHandle.entity == m_fieldEdit.entity) ||
        (m_showAnimSetEditor && m_animSetEditTarget.entity == m_fieldEdit.entity) ||
        (m_morphEditorOpen && m_morphEditorTarget.entity == m_fieldEdit.entity) ||
        (Editor::ChainPhysicsPreviewWindow::Get().IsOpen() &&
         ChainColliderTarget().entity == m_fieldEdit.entity) ||
        Editor::UIEditorWindow::Get().EditsEntity(m_fieldEdit.entity);

    const bool stale = !m_world
                    || !m_world->IsAlive(m_fieldEdit.entity)
                    || (m_selectedEntity != m_fieldEdit.entity && !editedByWindow);
    if (!stale) return;   // still the active component; in-draw commit handles it

    m_fieldEdit.open          = false;
    m_fieldEdit.restoreBefore = {};
    m_fieldEdit.fpBefore.clear();
    m_fieldEdit.blobBefore.clear();
    m_fieldEdit.snapshot.reset();
    m_fieldEdit.propagate     = false;
}

void EditorLayer::RenderInspectorPanel()
{
    // Begin()'s result is honoured: false means collapsed, fully clipped or an unselected docked tab, and
    // nothing between Begin and End may run - otherwise the reflection walk and MaterialPreviewWidget run
    // for an invisible window, and the widget holds a preview view and HDR slot for a picture nobody
    // sees.
    // But only when the host has a UI surface (the qualifier MaterialPreviewWidget::Draw uses too): under
    // -hidden (SW_SHOWMINNOACTIVE) io.DisplaySize is 0x0 and every main-viewport window is skipped, and
    // honouring the refusal would turn -hidden from "off the user's screen" into "the editor does
    // nothing". With no surface the question is not asked; m_inspectorNoSurface counts those frames. The
    // counters are what [PANELVIS] reports, so "the panel drew" is a pack assertion, not an inference
    // from a picture.
    // Closed from its tab's X: nothing runs (Window > General reopens it). The harness layout forces it
    // open (OnAttach), so this never changes a pack's [PANELVIS] counters.
    if (!m_showInspector) return;
    // Locked: the selection is the locked entity while the panel draws, and is put back after unless
    // the panel itself selected something else.
    struct RestoreSelection
    {
        std::function<void()> fn;
        ~RestoreSelection() { if (fn) fn(); }
    } restoreSelection;
    if (m_inspectorLocked)
    {
        if (!m_world || !m_world->IsHandleValid(m_inspectorLockHandle))
            m_inspectorLocked = false;
        else if (m_selectedEntity != m_inspectorLockHandle.entity)
        {
            const Entity locked = m_inspectorLockHandle.entity;
            restoreSelection.fn = [this, locked, saved = m_selectedEntity, savedList = m_selectedEntities]() {
                if (m_selectedEntity != locked) return;
                m_selectedEntity = saved;
                m_selectedEntities = savedList;
            };
            m_selectedEntity = locked;
            m_selectedEntities.assign(1, locked);
        }
    }
    const bool inspectorOpen = ImGui::Begin(kWindowInspector, &m_showInspector);
    const ImGuiIO& inspectorIO = ImGui::GetIO();
    const bool hostHasSurface = inspectorIO.DisplaySize.x > 0.f &&
                                inspectorIO.DisplaySize.y > 0.f;
    if (!inspectorOpen)
    {
        if (hostHasSurface)
        {
            ++m_inspectorRefusedFrames;
            ImGui::End();
            return;
        }
        ++m_inspectorNoSurface;
    }
    ++m_inspectorDrewFrames;

    if (!m_world || m_selectedEntity == NullEntity ||
        !m_world->IsAlive(m_selectedEntity))
    {
        // Asset mode: a material opened from the browser, with no entity behind
        // it. This is the only way to edit an .imatinst that nothing in the scene
        // uses yet -- which is every instance the moment it is created.
        if (!RenderMaterialAssetInspector())
            ImGui::TextDisabled("Select an entity to inspect");
        ImGui::End();
        return;
    }

    DrawInspectorToolbar();

    // ---- Entity header ----
    // Multi-selection banner: the Inspector shows the PRIMARY, and an edit is copied onto every other
    // selected entity that has the same component - say so plainly.
    if (m_selectedEntities.size() > 1)
    {
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1.f),
                           "%zu entities selected", m_selectedEntities.size());
        ImGui::SameLine();
        ImGui::TextDisabled("- edits apply to all");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Fields shown belong to the primary (last clicked).\n"
                "Changing one copies THAT field onto every selected entity\n"
                "that has this component; the others keep their own values\n"
                "for every field you do not touch.");
        ImGui::Separator();
    }

    const std::string& name = m_world->GetName(m_selectedEntity);
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", name.c_str());
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Double-click to focus entity (F)");
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) FocusSelectedEntity();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("[id=%u]", m_selectedEntity);
    // If the entity carries a GuidComponent (something refers to it stably across save / load), show its
    // GUID next to the volatile entity id, copyable from the header without opening the GUID component
    // row. Click the badge to copy the full hex to the clipboard.
    if (auto* gc = m_world->GetComponent<GuidComponent>(m_selectedEntity);
        gc && gc->guid.IsValid())
    {
        const std::string fullGuid = gc->guid.ToString();
        ImGui::SameLine();
        char shortBadge[24];
        // Only the first 8 hex chars in the header: the full GUID is 36 chars and would dominate the line.
        // The tooltip and click-to-copy expose the full value.
        std::snprintf(shortBadge, sizeof(shortBadge), "[guid:%.8s]", fullGuid.c_str());
        ImGui::TextDisabled("%s", shortBadge);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n(click to copy)", fullGuid.c_str());
        if (ImGui::IsItemClicked())
            ImGui::SetClipboardText(fullGuid.c_str());
    }
    ImGui::Separator();

    // A Canvas UI node inside a LINKED prefab instance: the scene saves the instance as a bare reference
    // to its .ipfb, so the root's UI parent and every UI edit made here are gone after the next load (the
    // UI Editor itself only makes detached copies). Said here, with the way out.
    if (UI::IsUINode(*m_world, m_selectedEntity))
    {
        Entity instRoot = NullEntity;
        if (m_world->HasComponent<PrefabSourcePath>(m_selectedEntity))
            instRoot = m_selectedEntity;
        else if (const PrefabInstanceMember* pm = m_world->GetComponent<PrefabInstanceMember>(m_selectedEntity))
            if (pm->root != NullEntity && m_world->IsAlive(pm->root) &&
                m_world->HasComponent<PrefabSourcePath>(pm->root))
                instRoot = pm->root;
        if (instRoot != NullEntity)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.72f, 0.25f, 1.f));
            ImGui::TextWrapped("Linked prefab instance ('%s'): the scene saves it as a reference to its "
                               ".ipfb, so UI edits made here are lost on the next load. Detach the "
                               "instance (its root's Prefab Ref) to keep them.",
                               m_world->GetName(instRoot).c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
    }

    // A node of a UI Document instance is the .iui's, spawned at run time and replaced whenever the file
    // changes - nothing done to it here would last. Read-only, with the way to the asset.
    const Entity docHost = UI::DocumentHostOf(*m_world, m_selectedEntity);
    if (docHost != NullEntity)
    {
        const UI::UIDocument* doc = m_world->GetComponent<UI::UIDocument>(docHost);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.72f, 0.25f, 1.f));
        ImGui::TextWrapped("From %s (the UI Document on '%s'): read-only here - the instance is made from the "
                           "file at run time. Edit the asset itself.",
                           doc ? doc->path.c_str() : "a .iui", m_world->GetName(docHost).c_str());
        ImGui::PopStyleColor();
        if (doc && ImGui::SmallButton("Edit in UI Editor")) Editor::UIEditorWindow::Get().RequestOpenAsset(doc->path);
        ImGui::Separator();
    }

    // ---- Component badge strip: a pill for every type on the entity ----
    const auto typeIndices = m_world->GetComponentTypeIndices(m_selectedEntity);

    if (!typeIndices.empty())
    {
        ImGui::TextDisabled("Components (%zu):", typeIndices.size());

        // Sort by display label for stable order across frames.
        std::vector<std::pair<std::string, std::type_index>> sorted;
        sorted.reserve(typeIndices.size());
        for (const auto& ti : typeIndices)
        {
            auto it = m_componentLabels.find(ti);
            const std::string label = (it != m_componentLabels.end())
                ? it->second
                : StripTypePrefix(ti.name());
            sorted.push_back({ label, ti });
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });

        const float panelWidth  = ImGui::GetContentRegionAvail().x;
        const float badgePadX   = 6.f;
        const float badgePadY   = 2.f;
        const float spacing     = 4.f;
        const float lineH       = ImGui::GetTextLineHeight() + badgePadY * 2.f + 1.f;
        ImDrawList* drawList    = ImGui::GetWindowDrawList();
        const float startX      = ImGui::GetCursorScreenPos().x;
        float       screenX     = startX;
        float       screenY     = ImGui::GetCursorScreenPos().y;

        for (const auto& [label, ti] : sorted)
        {
            const float textW = ImGui::CalcTextSize(label.c_str()).x;
            const float badgeW = textW + badgePadX * 2.f;

            // Wrap to next line if needed
            if (screenX + badgeW > startX + panelWidth && screenX > startX)
            {
                screenX = startX;
                screenY += lineH + spacing;
            }

            const bool hasEditor = m_componentEditors.count(ti) > 0;
            const ImVec4 bgColF  = hasEditor
                ? ImVec4(0.25f, 0.45f, 0.70f, 0.85f)   // blue  = has editor
                : ImVec4(0.28f, 0.28f, 0.28f, 0.85f);  // gray  = tag only
            const ImU32 bgCol  = ImGui::ColorConvertFloat4ToU32(bgColF);
            const ImU32 txtCol = IM_COL32(230, 230, 230, 255);

            const ImVec2 bMin = { screenX,          screenY };
            const ImVec2 bMax = { screenX + badgeW, screenY + lineH };
            drawList->AddRectFilled(bMin, bMax, bgCol, 4.f);
            drawList->AddText({ bMin.x + badgePadX, bMin.y + badgePadY }, txtCol, label.c_str());

            screenX += badgeW + spacing;
        }

        // Advance the ImGui cursor past the drawn badges
        const float totalH = (screenY - ImGui::GetCursorScreenPos().y) + lineH + spacing;
        ImGui::Dummy(ImVec2(panelWidth, totalH));
    }

    ImGui::Separator();

    // ---- Registered component editors (collapsing headers); priority ascending, label tiebreak ----
    std::vector<ComponentEditorEntry*> activeEditors;
    for (auto& [typeIdx, entry] : m_componentEditors)
    {
        if (!entry.draw) continue;   // Add-menu-only entries (Material: the built-in section below owns its UI)
        if (typeIdx == std::type_index(typeid(CapsuleColliderComponent)) &&
            m_world->HasComponent<ChainPhysicsComponent>(m_selectedEntity)) continue;
        if (!entry.fetch(*m_world, m_selectedEntity)) continue;
        if (entry.condition && !entry.condition(*m_world, m_selectedEntity)) continue;
        activeEditors.push_back(&entry);
    }
    std::sort(activeEditors.begin(), activeEditors.end(),
              [](const ComponentEditorEntry* a, const ComponentEditorEntry* b){
                  if (a->priority != b->priority) return a->priority < b->priority;
                  return a->label < b->label;
              });

    // Track if we need to remove a component (deferred to avoid modifying during iteration)
    std::function<void()> pendingRemove;

#if defined(WITH_HARNESS) && WITH_HARNESS
    // Harness: -inspector_scroll_to=<component label> keeps that component's header at the top of the
    // Inspector, so a -ui_screenshot shows a section far down the list (the Chain Physics row).
    static const std::string s_inspectorScrollTo =
        Sys::CommandLine::Get().Get("-inspector_scroll_to");
#endif
    ImGui::BeginDisabled(docHost != NullEntity);   // an instance node is read-only
    // A search draws only the fields it matches, every component's flat (no collapsed headers).
    const bool filtering = m_inspectorFilter.Active();
    EditorUI::SetActiveRowFilter(filtering ? &m_inspectorFilter : nullptr);
    const bool multiSelected = m_selectedEntities.size() > 1;
    for (ComponentEditorEntry* entry : activeEditors)
    {
        void* compPtr = entry->fetch(*m_world, m_selectedEntity);
        const Reflect::RuntimeEntry* reflected = Reflect::FindRuntime(entry->type);
        if (filtering)
        {
            const bool shown = (reflected && reflected->desc)
                ? Reflect::AnyFieldShown(compPtr, *reflected->desc)
                : !m_inspectorFilter.modifiedOnly && m_inspectorFilter.Matches(entry->label.c_str(), nullptr, nullptr);
            if (!shown) continue;
        }
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (!s_inspectorScrollTo.empty() && entry->label == s_inspectorScrollTo)
            ImGui::SetScrollHereY(0.0f);
#endif
        ImGui::PushID(entry->label.c_str());
        const ImGuiTreeNodeFlags hdrFlags = entry->defaultCollapsed
                                          ? ImGuiTreeNodeFlags_None
                                          : ImGuiTreeNodeFlags_DefaultOpen;
        bool open = true;
        if (filtering) ImGui::SeparatorText(entry->label.c_str());
        else           open = ImGui::CollapsingHeader(entry->label.c_str(), hdrFlags);

        // Right-click on the header: View Bytes, Remove Component.
        if (!filtering && entry->remove && ImGui::BeginPopupContextItem("##comp_ctx"))
        {
            if (ImGui::MenuItem("View Bytes")) OpenByteViewForComponent(m_selectedEntity, entry->type);
            if (ImGui::MenuItem("Remove Component"))
            {
                auto removeFn = entry->remove;
                auto addFn    = entry->add;
                const std::type_index ctype = entry->type;
                Entity ent    = m_selectedEntity;
                pendingRemove = [this, removeFn, addFn, ctype, ent]()
                {
                    // Capture the component's data BEFORE removing so undo can
                    // restore it, then push a RemoveComponentAction.
                    if (UndoEnabled() && m_world && m_world->IsAlive(ent))
                    {
                        SyncedEditContext();
                        std::string blob = Editor::Undo::SerializeComponentBlob(
                            GetComponentRegistry(), *m_world, ent, ctype);
                        PushEdit(std::make_unique<Editor::Undo::RemoveComponentAction>(
                            m_world->MakeHandle(ent), ctype, addFn, removeFn,
                            std::move(blob), ComponentLabelFor(ctype)));
                    }
                    removeFn(*m_world, ent);
                };
            }
            ImGui::EndPopup();
        }

        if (open)
        {
            const Entity selEnt = m_selectedEntity;
            ComponentEditorEntry* ent = entry;
            void* cp = compPtr;
            // The fields that differ across a multi-selection are drawn mixed; each field's row chrome
            // names its path and the Lua call that sets it.
            const std::vector<const Reflect::FieldDescriptor*> mixed =
                multiSelected ? MixedReflectedFields(selEnt, entry->type) : std::vector<const Reflect::FieldDescriptor*>();
            if (multiSelected) Reflect::SetMultiSelection(&mixed, static_cast<int>(m_selectedEntities.size()));
            const LuaEntity luaId = LuaId::Out(*m_world, selEnt);
            Reflect::SetDrawContext(PropertyTagOf(entry->type),
                                    static_cast<double>((static_cast<uint64_t>(luaId.gen) << 32) | luaId.id));
            struct ClearDrawContext
            {
                bool multi;
                ~ClearDrawContext()
                {
                    Reflect::SetDrawContext("", 0.0);
                    if (multi) Reflect::SetMultiSelection(nullptr, 1);
                }
            } clearDrawContext{ multiSelected };
            DrawComponentWithUndo(selEnt, entry->type, entry->captureValue, entry->fingerprint,
                [this, ent, cp, selEnt]() {
                    if (ent->type == std::type_index(typeid(MorphComponent)) ||
                        ent->type == std::type_index(typeid(FacialAnimationComponent)))
                    {
                        if (auto* sk = m_world->GetComponent<SkeletonComponent>(selEnt);
                            sk && m_renderer && sk->assetIndex < m_renderer->GetSkeletonRegistry().Count())
                            ImGui::TextDisabled("%u morph targets", m_renderer->GetSkeletonRegistry().Get(sk->assetIndex).morphTargetCount);
                        if (ImGui::Button("Edit Morph / Face...")) OpenMorphEditor(selEnt);
                    }
                    else ent->draw(cp, m_world, selEnt);
                });
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    EditorUI::SetActiveRowFilter(nullptr);

    // Removing a component can change the logical parent (UIParent / Follow*) or the row icon:
    // invalidate the cached hierarchy view.
    if (pendingRemove) { pendingRemove(); MarkHierarchyDirty(); }

    // ---- Built-in: Material editor ----
    // A search keeps it only when it names the material.
    MaterialComponent* mat = (!filtering || (!m_inspectorFilter.modifiedOnly && m_inspectorFilter.Matches("Material", nullptr, nullptr)))
        ? m_world->GetComponent<MaterialComponent>(m_selectedEntity) : nullptr;
    if (mat)
    {
        ImGui::PushID("__material__");
        const Entity matSectionEntity = m_selectedEntity;
        const float  matSectionY0     = ImGui::GetCursorPosY();   // the section's height (harness)
#if defined(WITH_HARNESS) && WITH_HARNESS
        // -inspector_scroll_to=Material reaches this built-in section too (the material panel's before /
        // after captures).
        if (s_inspectorScrollTo == "Material") ImGui::SetScrollHereY(0.0f);
#endif
        bool matOpen = ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen);

        // Right-click remove - mesh parity for VFX entities (a trail / particle dropping its material falls
        // back to the component-driven look next frame). Undo restores a DEEP COPY via the add-fn
        // (MaterialComponent has no blob serializer; RemoveComponentAction with an empty blob calls only the
        // add-fn on revert). Removal happens inline, so every later use of `mat` is guarded by removedNow.
        bool removedNow = false;
        if (ImGui::BeginPopupContextItem("##mat_ctx"))
        {
            if (ImGui::MenuItem("Remove Component"))
            {
                const Entity ent = m_selectedEntity;
                auto saved = std::make_shared<MaterialComponent>(*mat);
                auto addFn = [saved](World& w, Entity e2)
                {
                    MaterialComponent copy(*saved);
                    copy.SetDirty();   // force renderer re-sync of restored state
                    w.AddComponent<MaterialComponent>(e2, std::move(copy));
                };
                auto removeFn = [](World& w, Entity e2)
                    { w.RemoveComponent<MaterialComponent>(e2); };
                if (UndoEnabled() && m_world->IsAlive(ent))
                {
                    SyncedEditContext();
                    PushEdit(std::make_unique<Editor::Undo::RemoveComponentAction>(
                        m_world->MakeHandle(ent), std::type_index(typeid(MaterialComponent)),
                        addFn, removeFn, std::string{}, "Material"));
                }
                removeFn(*m_world, ent);
                removedNow = true;
            }
            ImGui::EndPopup();
        }

        if (matOpen && !removedNow)
        {
            const Entity selEnt = m_selectedEntity;
            // ONE row - the asset, what it is, its warnings and [Edit] (the Material Editor). Drawn outside the
            // undo bracket below: its actions push their own single undo entries, and a button pressed inside
            // the bracket would push a second one.
            DrawInspectorMaterialRow(selEnt);
        }
        ImGui::PopID();
        // The slim section's height, one line per change (the inspector_material_row pack bounds it).
        if (Sys::CommandLine::Get().Has("-unattended") && matOpen && m_world->IsAlive(matSectionEntity))
        {
            const int  h    = static_cast<int>(ImGui::GetCursorPosY() - matSectionY0 + 0.5f);
            char line[160];
            std::snprintf(line, sizeof(line), "height=%d", h);
            std::string& last = m_matRowLogged["section:" + m_world->GetName(matSectionEntity)];
            if (last != line)
            {
                last = line;
                LOG_INFO("[INSPECTOR] material section %s: %s", m_world->GetName(matSectionEntity).c_str(), line);
            }
        }
    }

    // ---- Animation clip drop zone (only for entities with a SkeletonComponent) ----
    if (!filtering && m_world && m_world->HasComponent<SkeletonComponent>(m_selectedEntity))
    {
        ImGui::Separator();

        const ImVec2 dropSz  = ImVec2(ImGui::GetContentRegionAvail().x, 28.f);
        const ImVec2 dropMin = ImGui::GetCursorScreenPos();
        const ImVec2 dropMax = { dropMin.x + dropSz.x, dropMin.y + dropSz.y };

        const bool isDragActive = ImGui::GetDragDropPayload() &&
                                  ImGui::GetDragDropPayload()->IsDataType("IANIM_PATH");
        const ImU32 bgCol = isDragActive
            ? IM_COL32(40, 100, 40, 160)
            : IM_COL32(40, 40, 40, 80);
        ImGui::GetWindowDrawList()->AddRectFilled(dropMin, dropMax, bgCol, 4.f);
        ImGui::GetWindowDrawList()->AddRect(dropMin, dropMax,
            isDragActive ? IM_COL32(80, 200, 80, 200) : IM_COL32(80, 80, 80, 120), 4.f);
        const char* hint = isDragActive ? "Release to assign animation" : "Drop .ianim here to assign animation";
        const ImVec2 tsz  = ImGui::CalcTextSize(hint);
        ImGui::GetWindowDrawList()->AddText(
            { dropMin.x + (dropSz.x - tsz.x) * 0.5f, dropMin.y + (dropSz.y - tsz.y) * 0.5f },
            IM_COL32(180, 180, 180, 200), hint);

        ImGui::Dummy(dropSz);

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IANIM_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                if (m_animClipSys && m_selectedEntity != NullEntity)
                {
                    AssignAnimationAsset(m_selectedEntity, path);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    // ---- Add Component button ----
    ImGui::SetNextItemWidth(-1.f);
    ImGui::BeginDisabled(docHost != NullEntity);   // an instance node is read-only
    if (ImGui::Button("Add Component", ImVec2(-1.f, 0.f)))
        ImGui::OpenPopup("##add_comp_popup");
    ImGui::EndDisabled();

    if (ImGui::BeginPopup("##add_comp_popup"))
    {
        ImGui::TextDisabled("Select component to add:");
        ImGui::Separator();

        // Filter out unaddable / already-present / condition-failing entries.
        std::vector<ComponentEditorEntry*> addable;
        for (auto& [typeIdx, entry] : m_componentEditors)
        {
            if (!entry.add) continue;
            // A module the project leaves off: its components are not offered.
            if (!entry.module.empty() && !Module::Registry::Get().IsEnabled(entry.module)) continue;
            if (entry.fetch(*m_world, m_selectedEntity)) continue; // already has it
            if (entry.condition && !entry.condition(*m_world, m_selectedEntity)) continue;
            addable.push_back(&entry);
        }

        if (addable.empty())
        {
            ImGui::TextDisabled("(all registered components already added)");
        }
        else
        {
            // Group by category, each sorted by priority/label.
            std::map<std::string, std::vector<ComponentEditorEntry*>> byCategory;
            for (ComponentEditorEntry* entry : addable)
                byCategory[entry->category].push_back(entry);
            for (auto& [_cat, entries] : byCategory)
            {
                std::sort(entries.begin(), entries.end(),
                    [](const ComponentEditorEntry* a, const ComponentEditorEntry* b) {
                        if (a->priority != b->priority) return a->priority < b->priority;
                        return a->label < b->label;
                    });
            }

            // Preferred category order; unlisted go alphabetical, "Misc" pinned last.
            static const char* const kCategoryOrder[] = {
                "Transform",
                "Rendering",
                "Lighting",
                "Environment",
                "Camera",
                "Animation",
                "Physics",
                "Gameplay",
                "VFX",
                "Post-Process",
                "UI",
                "AI / Script",
                "Attachment",
            };

            auto drawCategory = [&](const std::string& cat,
                                    std::vector<ComponentEditorEntry*>& entries)
            {
                if (entries.empty()) return;
                if (ImGui::BeginMenu(cat.c_str()))
                {
                    for (ComponentEditorEntry* entry : entries)
                    {
                        if (ImGui::MenuItem(entry->label.c_str()))
                        {
                            const Entity ent = m_selectedEntity;
                            World* targetWorld = m_world;
                            const auto epoch = targetWorld->Epoch();
                            const EntityHandle handle = targetWorld->MakeHandle(ent);
                            const auto type = entry->type;
                            m_pendingComponentAdds.emplace_back([this, targetWorld, epoch, handle, type]() {
                                if (m_world != targetWorld || m_world->Epoch() != epoch ||
                                    !m_world->IsHandleValid(handle)) return;
                                const auto it = m_componentEditors.find(type);
                                if (it == m_componentEditors.end()) return;
                                const auto& component = it->second;
                                if (!component.add || component.fetch(*m_world, handle.entity) ||
                                    (component.condition && !component.condition(*m_world, handle.entity))) return;
                                component.add(*m_world, handle.entity);
                                MarkHierarchyDirty();
                                if (UndoEnabled())
                                {
                                    SyncedEditContext();
                                    PushEdit(std::make_unique<Editor::Undo::AddComponentAction>(
                                        handle, component.add, component.remove, ComponentLabelFor(type)));
                                }
                            });
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndMenu();
                }
            };

            // Pass 1: preferred order. Erase as we go so pass 2 only sees the
            // leftovers (alphabetical), with Misc always last.
            for (const char* cat : kCategoryOrder)
            {
                auto it = byCategory.find(cat);
                if (it == byCategory.end()) continue;
                drawCategory(it->first, it->second);
                byCategory.erase(it);
            }
            std::vector<ComponentEditorEntry*> miscBucket;
            for (auto& [cat, entries] : byCategory)
            {
                if (cat == "Misc") { miscBucket = std::move(entries); continue; }
                drawCategory(cat, entries);
            }
            if (!miscBucket.empty())
                drawCategory("Misc", miscBucket);
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ==== WaterComponent -> mesh-driven Water material ====
// A one-shot conversion. WaterComponent authors both placement and look; in the mesh model the mesh +
// transform own placement and the material owns the look, so each component becomes a Plane
// primitive scaled to worldSize at worldCenter (its Y is the water level) carrying an inline
// Water-domain material, plus a WaterInteractionComponent when ripples were enabled. The look knobs
// are not mapped one-for-one (the material reads them from its own b8 block, whose reflection would
// have to be live); what cannot be re-authored by eye is transferred: placement, size, level, deep
// colour and the ripple settings. The dry run prints that mapping first.
void EditorLayer::ConvertLegacyWater(bool dryRun)
{
    if (!m_world) return;
    auto* pool = m_world->GetPool<WaterComponent>();
    if (!pool || pool->Size() == 0)
    {
        LOG_INFO("ConvertLegacyWater: no WaterComponent in the open world.");
        return;
    }

    // Snapshot first: AddComponent / RemoveComponent below can reallocate pools.
    struct Item { Entity e; WaterComponent wc; };
    std::vector<Item> items;
    {
        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        for (size_t i = 0; i < ents.size(); ++i)
            if (ents[i] != NullEntity) items.push_back({ ents[i], data[i] });
    }

    LOG_INFO("ConvertLegacyWater: %s %zu water bod%s",
             dryRun ? "DRY RUN over" : "converting", items.size(),
             items.size() == 1 ? "y" : "ies");

    for (const Item& it : items)
    {
        const WaterComponent& wc = it.wc;
        LOG_INFO("  entity=%u  center=(%.2f, %.2f, %.2f)  size=%.2f  ripples=%d"
                 "  -> Plane scale=(%.2f, 1, %.2f) at y=%.2f",
                 it.e, wc.worldCenter.x, wc.worldCenter.y, wc.worldCenter.z,
                 wc.worldSize, wc.enableRipples ? 1 : 0,
                 wc.worldSize, wc.worldSize, wc.worldCenter.y);
        if (dryRun) continue;

        // Placement -> transform. worldCenter.y IS the surface level.
        LocalTransform lt{};
        if (const LocalTransform* cur = m_world->GetComponent<LocalTransform>(it.e))
            lt = *cur;
        lt.translation = wc.worldCenter;
        lt.scale       = { wc.worldSize, 1.0f, wc.worldSize };
        if (m_world->HasComponent<LocalTransform>(it.e))
            *m_world->GetComponent<LocalTransform>(it.e) = lt;
        else
            m_world->AddComponent<LocalTransform>(it.e, lt);
        if (!m_world->HasComponent<GlobalTransform>(it.e))
            m_world->AddComponent<GlobalTransform>(it.e, GlobalTransform{});

        // Geometry -> Plane primitive. gridQuads is dropped: tessellation is a property of the mesh, and the
        // built-in Plane is a coarse quad; a denser surface means assigning a denser mesh.
        MeshHandle mh;
        mh.gpuMeshID = static_cast<uint32_t>(PrimitiveMeshType::Plane);
        if (m_world->HasComponent<MeshHandle>(it.e))
            *m_world->GetComponent<MeshHandle>(it.e) = mh;
        else
            m_world->AddComponent<MeshHandle>(it.e, mh);
        if (!m_world->HasComponent<BoundingVolume>(it.e))
            m_world->AddComponent<BoundingVolume>(it.e, BoundingVolume{});
        if (!m_world->HasComponent<VisibilityComponent>(it.e))
            m_world->AddComponent<VisibilityComponent>(it.e, VisibilityComponent{});
        if (!m_world->HasComponent<RenderLayer>(it.e))
            m_world->AddComponent<RenderLayer>(it.e, RenderLayer{});

        // Look -> inline Water shading-model material. No MaterialSourcePath, so the
        // `MaterialInline:` serializer persists it with the scene (a shared
        // .imat would make every converted lake edit every other one).
        MaterialComponent mat{};
        if (const MaterialComponent* cur = m_world->GetComponent<MaterialComponent>(it.e))
            mat = *cur;
        mat.customShadingModel = ShadingModel::Water;
        mat.userBlendMode      = BlendMode::Alpha;
        mat.useCustomShader  = true;
        mat.customShaderPath = "asset/Shaders/water_surface.hlsl";
        mat.flags |= MaterialComponent::DOUBLE_SIDED;
        mat.flags &= ~static_cast<uint32_t>(MaterialComponent::CAST_SHADOW);
        mat.baseColor = { wc.deepColor.x, wc.deepColor.y, wc.deepColor.z, 1.0f };
        mat.SetDirty();
        if (m_world->HasComponent<MaterialComponent>(it.e))
            *m_world->GetComponent<MaterialComponent>(it.e) = mat;
        else
            m_world->AddComponent<MaterialComponent>(it.e, std::move(mat));
        if (m_world->HasComponent<MaterialSourcePath>(it.e))
            m_world->RemoveComponent<MaterialSourcePath>(it.e);

        // Ripples -> the interaction component (simulation budget only).
        if (wc.enableRipples && !m_world->HasComponent<WaterInteractionComponent>(it.e))
        {
            WaterInteractionComponent wi;
            wi.propagation    = wc.ripplePropagation;
            wi.damping        = wc.rippleDamping;
            wi.amplitude      = wc.rippleAmplitude;
            wi.normalStrength = wc.rippleNormalStrength;
            wi.areaSize       = wc.rippleAreaSize;
            m_world->AddComponent<WaterInteractionComponent>(it.e, wi);
        }

        // Finally drop the WaterComponent so its pass stops drawing it - otherwise both paths render the same
        // body and z-fight.
        m_world->RemoveComponent<WaterComponent>(it.e);
    }

    if (!dryRun)
        LOG_INFO("ConvertLegacyWater: done. CHECK the result, then save the scene. "
                 "Flow-map paths and shading knobs were NOT transferred — "
                 "re-assign them in the Material inspector.");
}

// ==== Material asset editing: the Inspector with a file open instead of an entity ====

void EditorLayer::OpenMaterialAsset(const std::string& path)
{
    if (path.empty()) return;

    auto edit = std::make_unique<MaterialComponent>();
    Resource::MaterialLoadInfo info;
    if (!Resource::LoadMaterial(path, *edit, &info))
    {
        LOG_ERROR("Material asset: '%s' failed to load", path.c_str());
        return;
    }

    m_assetMatPath  = Sys::RelativizeAssetPath(path);
    m_assetMatEdit  = std::move(edit);
    m_assetMatInfo  = std::move(info);
    m_assetMatDirty = false;

    // Opening an asset is a different thing to inspect, so stop showing an entity underneath it:
    // otherwise the Inspector keeps drawing the entity and the asset panel never appears.
    ClearEntitySelection();
    LOG_INFO("Material asset: opened '%s'%s", m_assetMatPath.c_str(),
             m_assetMatInfo.parentPath.empty() ? "" : " (instance)");
}

bool EditorLayer::RenderMaterialAssetInspector()
{
    if (m_assetMatPath.empty() || !m_assetMatEdit) return false;
    MaterialComponent& mat = *m_assetMatEdit;

    const bool isInstance = !m_assetMatInfo.parentPath.empty();

    ImGui::TextDisabled("Asset: %s", m_assetMatPath.c_str());
    if (m_assetMatDirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "(unsaved)");
    }

    if (isInstance)
    {
        if (!m_assetMatInfo.parentResolved)
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
                               "  ! parent unresolved: %s", m_assetMatInfo.parentPath.c_str());
        else
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f),
                               "  > instance of: %s", m_assetMatInfo.parentPath.c_str());
        ImGui::TextDisabled("  %zu overridden, everything else follows the parent",
                            m_assetMatInfo.overrideKeys.size());
        if (!m_assetMatInfo.explicitFormat)
        {
            ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.25f, 1.f),
                               "  legacy value-diff file");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Written before override marks existed, so its override set was\n"
                    "inferred from which values differ. Saving converts it to the\n"
                    "explicit format (imatfmt = 2) without changing any value.");
        }
    }
    else
        ImGui::TextDisabled("  standalone material (no parent)");

    if (ImGui::Button("Save"))
    {
        const bool ok = isInstance
            ? Resource::SaveMaterialInstance(m_assetMatPath, m_assetMatInfo.parentPath,
                                             mat, &m_assetMatInfo.overrideKeys,
                                             &m_assetMatInfo.unsetKeys)
            : Resource::SaveMaterial(mat, m_assetMatPath);
        if (ok)
        {
            m_assetMatDirty = false;
            mat.SetAssetDirty(false);   // the material's own mark, not only the panel's
            // Push the edit at anything already using this file, so the viewport
            // reflects it without a scene reload.
            if (m_world) Resource::ReloadMaterialsAffectedBy(*m_world, m_assetMatPath);
            // Thumbnail invalidation, on the same edge and deliberately here: this is the one place the editor
            // knows a material chain member reached disk. Evicts the tile (and the disk entry) for this file and
            // for every instance inheriting from it - MaterialsDependingOn reads the serializer's reverse index,
            // so it still answers after the save dropped the cache entries.
            InvalidateMaterialThumbnails(m_assetMatPath);
        }
        else
            LOG_ERROR("Material asset: save failed for '%s'", m_assetMatPath.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
    {
        // Discard rather than undo: this buffer is not an entity, so it is not on the entity undo stack, and
        // asset edits are not wired into it (only the entity path wraps RenderMaterialInspector in
        // DrawComponentWithUndo).
        const std::string path = m_assetMatPath;
        OpenMaterialAsset(path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
    {
        m_assetMatPath.clear();
        m_assetMatEdit.reset();
        m_assetMatDirty = false;
        // Queue the preview teardown; ProcessPendingActions performs it. Here it would release a descriptor
        // this ImGui frame may already have handed to AddImage.
        Editor::MaterialPreviewWidget::Get().NotifyClosed();
        return true;   // drew this frame; the panel is empty from the next one
    }

    // Live material preview, between the Save / Revert / Close row and the Separator.
    Editor::MaterialPreviewWidget::Get().Draw(m_renderer, m_assetMatPath, mat);

    // The unsaved-edit policy made visible: the widget above shows the material AS EDITED, the Asset
    // Browser tile AS ON DISK, always (a tile answers "what does this file look like"). When they
    // disagree the UI says so, or the next report is "the thumbnail is stale" about a system working as
    // designed.
    if (m_assetMatDirty)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  r = ImGui::GetTextLineHeight() * 0.25f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r + 2.f, p.y + r + 2.f), r, IM_COL32(255, 170, 60, 255));
        ImGui::Dummy(ImVec2(r * 2.f + 6.f, r * 2.f + 4.f));
        ImGui::SameLine();
        ImGui::TextDisabled("preview shows unsaved edits");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "This preview shows the material as you are editing it.\n"
                "The Asset Browser thumbnail shows the version ON DISK,\n"
                "and catches up when you press Save.");
    }

    ImGui::Separator();

    // The whole widget set (schema rows, texture slots, custom //@ parameters) reused as-is. Not wrapped
    // in DrawComponentWithUndo: that captures a deep copy of a MaterialComponent per gesture against an
    // ENTITY, and there is no entity here.
    const bool wasDirty = mat.IsAssetDirty();
    RenderMaterialInspector(mat, /*assetMode=*/true);
    if (mat.IsAssetDirty() && !wasDirty) m_assetMatDirty = true;

    return true;
}

// The body is Editor::MaterialPropertyPanel (Editor/MaterialPropertyPanel.h), shared with the
// Material Editor window; this is its entity / asset-mode host.
void EditorLayer::RenderMaterialInspector(MaterialComponent& mat, bool assetMode)
{
    Editor::MaterialPanelContext ctx;
    ctx.renderer   = m_renderer;
    ctx.world      = m_world;
    ctx.entity     = m_selectedEntity;
    ctx.assetMode  = assetMode;
    ctx.assetInfo  = &m_assetMatInfo;
    ctx.assetDirty = &m_assetMatDirty;
    m_materialPanel.Draw(mat, ctx);
}

// ==== Content Browser panel ====
// The asset tree + tiles. The body runs even when Begin() reports the window hidden: under -hidden
// every main-viewport window is skipped, and the thumbnail lane (preview_churn, asset_preview_*) is
// fed by the tiles this body submits. Only the user's close (the tab's X / Window > General) stops it.
void EditorLayer::RenderContentBrowserPanel()
{
    if (!m_showContentBrowser) return;
    ImGui::Begin(kWindowContentBrowser, &m_showContentBrowser);

    if (ImGui::BeginTabBar("ContentBrowserTabs", ImGuiTabBarFlags_None))
    {
        // ---- Assets tab ----
        if (ImGui::BeginTabItem("Assets"))
        {
            // ---- Content Browser toolbar: Import + Up + Tree + path + Refresh ----
            // Icon glyphs (Font Awesome Solid): U+F56F file-import, U+F062 arrow-up, U+F802 folder-tree, U+F021
            // arrows-rotate. "###cb*" keeps each button's ID the same with or without the icon font.
            if (ImGui::SmallButton(m_hasFontAwesomeIcons ? "\xef\x95\xaf Import###cbImport" : "Import###cbImport"))
                ImGui::OpenPopup("AssetImportPopup");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Import assets into the current folder");
            if (ImGui::BeginPopup("AssetImportPopup"))
            {
                RenderAssetImportMenuItems();
                ImGui::EndPopup();
            }
            ImGui::SameLine();

            namespace fs = std::filesystem;
            {
                const bool atRoot = m_currentDir.empty() || m_assetDir.empty() ||
                    (Sys::U8Path(m_currentDir) == Sys::U8Path(m_assetDir));

                if (atRoot) ImGui::BeginDisabled();
                if (ImGui::SmallButton(m_hasFontAwesomeIcons ? "\xef\x81\xa2###cbUp" : "^ Up###cbUp"))
                {
                    std::string up = Sys::PathToUtf8(Sys::U8Path(m_currentDir).parent_path());
                    // Clamp: never go above m_assetDir.
                    std::error_code ec;
                    if (!Sys::U8Path(up).is_relative() && !m_assetDir.empty())
                    {
                        // If somehow above the root, reset to the root.
                        auto rel = fs::relative(Sys::U8Path(up), Sys::U8Path(m_assetDir), ec);
                        if (ec || Sys::PathToUtf8(rel).find("..") != std::string::npos)
                            up = m_assetDir;
                    }
                    SetCurrentAssetDir(up);
                }
                if (atRoot) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Up one folder");
                ImGui::SameLine();

                // Folder-tree toggle, next to the navigation it belongs with.
                if (ImGui::SmallButton(m_hasFontAwesomeIcons ? "\xef\xa0\x82###cbTree"
                                       : (m_showFolderTree ? "< Tree###cbTree" : "Tree >###cbTree")))
                    m_showFolderTree = !m_showFolderTree;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show/hide the folder tree pane");
                ImGui::SameLine();

                // Show the path relative to m_assetDir.
                std::string displayPath;
                if (!m_assetDir.empty() && !m_currentDir.empty())
                {
                    std::error_code ec;
                    auto rel = fs::relative(Sys::U8Path(m_currentDir), Sys::U8Path(m_assetDir), ec);
                    displayPath = ec ? m_currentDir : Sys::PathToUtf8(rel);
                    if (displayPath == ".") displayPath.clear();
                }
                if (displayPath.empty())
                    ImGui::TextDisabled("(root)");
                else
                    ImGui::TextDisabled("%s", displayPath.c_str());

            }
            ImGui::SameLine();
            const bool refreshClicked = ImGui::SmallButton(
                m_hasFontAwesomeIcons ? "\xef\x80\xa1###cbRefresh" : "Refresh###cbRefresh");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rescan the current folder and the folder tree");
            if (refreshClicked || m_needsRescan)
            {
                // The background catalog follows every change the browser rescans for.
                if (m_assetSearchAll)
                {
                    m_assetCatalog.SetRoots({ m_assetDir, Sys::EngineDir() + "/engine" });
                    m_assetCatalog.Refresh();
                    m_assetCatalogAskedAt = ImGui::GetTime();
                }
                ScanAssets();
                // The tree is rebuilt on this path only, not by the 5-second auto-rescan below: it walks every
                // directory under asset/, and the layout changes on import / rename / delete (all of which set
                // m_needsRescan), not on a timer.
                RebuildAssetFolderTree();
                m_needsRescan    = false;
                m_lastScanTime   = ImGui::GetTime();
                // A search result asked for: select it now that its folder is listed.
                if (!m_pendingAssetSelect.empty())
                {
                    for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
                        if (m_assets[i].path == m_pendingAssetSelect)
                        {
                            m_selectedAsset = i;
                            m_selectedAssetPaths.assign(1, m_assets[i].path);
                            break;
                        }
                    m_pendingAssetSelect.clear();
                }
            }

            // Auto-rescan every 5 seconds.
            if (ImGui::GetTime() - m_lastScanTime > 5.0)
            {
                ScanAssets();
                m_lastScanTime = ImGui::GetTime();
            }

            // ---- Filter row (case-insensitive substring vs AssetEntry::stem) + Tiles / List / Details view
            // switch ----
            {
                ImGui::SetNextItemWidth(220.f);
                if (ImGui::InputTextWithHint("##AssetFilter", "Filter (name)",
                        m_assetFilter, sizeof(m_assetFilter)))
                    m_assetOrderDirty = true;
                if (m_assetFilter[0] != '\0')
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##AssetFilter"))
                    {
                        m_assetFilter[0]  = '\0';
                        m_assetOrderDirty = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("All folders", &m_assetSearchAll) && m_assetSearchAll)
                {
                    m_assetCatalog.SetRoots({ m_assetDir, Sys::EngineDir() + "/engine" });
                    m_assetCatalog.Refresh();
                    m_assetCatalogAskedAt = ImGui::GetTime();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("Search every folder (asset/ and the engine's content) by name,\n"
                                      "from a catalog walked off the UI thread.");
                ImGui::SameLine(0.f, 16.f);
                RenderAssetViewModeSelector();
            }
            if (m_assetSearchAll && m_assetFilter[0] != '\0') RenderAssetSearchResults();

            // Asset count summary (totals across all assets, unfiltered).
            int nTex = 0, nShd = 0, nMsh = 0, nMat = 0, nScn = 0, nPfb = 0, nSkel = 0, nAnim = 0;
            for (const auto& a : m_assets)
            {
                switch (a.type)
                {
                case AssetType::Texture:   ++nTex;  break;
                case AssetType::Shader:    ++nShd;  break;
                case AssetType::Mesh:      ++nMsh;  break;
                case AssetType::Material:  ++nMat;  break;
                case AssetType::Scene:     ++nScn;  break;
                case AssetType::Prefab:    ++nPfb;  break;
                case AssetType::Skeleton:  ++nSkel; break;
                case AssetType::Animation: ++nAnim; break;
                default: break;
                }
            }
            ImGui::TextDisabled("Textures: %d   Shaders: %d   Geometry: %d   Materials: %d   Models: %d   Prefabs: %d   Skeletons: %d   Animations: %d",
                                nTex, nShd, nMsh, nMat, nScn, nPfb, nSkel, nAnim);
            ImGui::Separator();

            // Unreal / Unity content-browser split: the whole tree on the left, the current folder's assets on
            // the right. A resizable table rather than a hand-rolled splitter: it gets the drag handle for free
            // AND persists the divider position in imgui.ini.
            if (m_showFolderTree
                && ImGui::BeginTable("AssetBrowserSplit", 2,
                                     ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("##Folders", ImGuiTableColumnFlags_WidthFixed, 220.f);
                ImGui::TableSetupColumn("##Assets",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string dirBeforeTree = m_currentDir;
                RenderAssetFolderTree();
                // Tree clicks happen after the toolbar's rescan. Refresh the
                // right pane now so it cannot render entries from the old dir.
                // Leave the tree rebuild to the next frame, outside traversal.
                if (m_currentDir != dirBeforeTree)
                {
                    ScanAssets();
                    m_lastScanTime = ImGui::GetTime();
                }
                ImGui::TableNextColumn();
                RenderAssetBrowser();
                ImGui::EndTable();
            }
            else
            {
                RenderAssetBrowser();
            }

            // Drag-drop and the folder menus only QUEUE their work; running it
            // inline would mutate m_assets and the folder tree in the middle of
            // the traversal drawing them.
            ApplyPendingFolderOps();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    RenderExtractMeshDialog();   // a Mesh item's "Extract Mesh..."

    // ---- Full-window IPFB drop target (spawn a prefab anywhere in the panel) ----
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (ImGui::GetDragDropPayload() &&
            ImGui::GetDragDropPayload()->IsDataType("IPFB_PATH"))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 bMin = win->InnerRect.Min;
            const ImVec2 bMax = win->InnerRect.Max;
            dl->AddRectFilled(bMin, bMax, IM_COL32(30, 80, 30, 60), 3.f);
            dl->AddRect(bMin, bMax, IM_COL32(70, 180, 70, 200), 3.f);
            const char* hint = "Drop .ipfb to spawn prefab";
            const ImVec2 tsz = ImGui::CalcTextSize(hint);
            dl->AddText(
                ImVec2(bMin.x + (bMax.x - bMin.x - tsz.x) * 0.5f,
                       bMax.y - tsz.y - 6.f),
                IM_COL32(120, 220, 120, 230), hint);
        }
        if (ImGui::BeginDragDropTargetCustom(win->InnerRect, win->ID + 1))
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
            {
                const char* pfbPath = static_cast<const char*>(p->Data);
                if (m_assetMgr && m_world)
                {
                    Entity spawned = NullEntity;
                    RecordSpawn("Spawn Prefab", [&]{
                        spawned = Resource::LoadPrefab(pfbPath, *m_world, *m_assetMgr,
                                                       m_renderer, m_animClipSys);
                        // Place the root in front of the camera (inside the record scope so the snapshot captures the
                        // final transform) - or, for a UI prefab, as a detached copy on a canvas.
                        if (spawned != NullEntity && !AdoptUIPrefabRoot(spawned, NullEntity))
                            if (auto* lt = m_world->GetComponent<LocalTransform>(spawned))
                                lt->translation = ComputeSpawnPosition();
                    });
                    if (spawned != NullEntity)
                    {
                        m_selectedEntity = spawned;
                        LOG_SUCCESS("EditorLayer: spawned prefab '%s' — root entity %u", pfbPath, spawned);
                    }
                    else
                        LOG_ERROR("EditorLayer: failed to spawn prefab '%s'", pfbPath);
                }
                else
                    LOG_ERROR("EditorLayer: assetMgr not wired — cannot spawn prefab");
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::End();
}

// ==== Output Log panel ====
// Its own dockable window, so the log and the Content Browser can be seen at the same time.
void EditorLayer::RenderOutputLogPanel()
{
    if (!m_showOutputLog) return;

    // Error / warning badge on the TAB: out of sight must not mean out of mind for errors.
    // kWindowOutputLog's "###" part keeps the window's identity (dock placement, focus) stable while the
    // visible text changes.
    const char* id = std::strstr(kWindowOutputLog, "###");
    char title[64];
    {
        const uint32_t errN = Logger::Get().ErrorCount();
        const uint32_t wrnN = Logger::Get().WarningCount();
        if      (errN) snprintf(title, sizeof(title), "Output Log (%u!)%s", errN, id);
        else if (wrnN) snprintf(title, sizeof(title), "Output Log (%u)%s",  wrnN, id);
        else           snprintf(title, sizeof(title), "Output Log%s", id);
    }
    // The mirror is pulled whether or not the window body draws this frame (a
    // collapsed tab or a hidden dock node still has an open panel): the pull
    // is incremental and cheap, and skipping it would only let the logger's
    // ring drop lines the panel then has to report as a gap.
    m_outputLog.Pump();
    if (m_outputLog.TakeWindowFocusRequest())   // Tools menu / Command Palette
        ImGui::SetNextWindowFocus();
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -logpanel_verify: bring the tab to the front once both bottom-node
    // windows exist (the Content Browser is created last and starts in front),
    // so a VISIBLE run exercises the list + command line, not only the pump.
    if (m_logPanelVerify && m_editorFrame == 30)
        ImGui::SetNextWindowFocus();
#endif
    if (!ImGui::Begin(title, &m_showOutputLog))
    {
        ImGui::End();
        return;
    }

    const Editor::OutputLogPanel::LevelFilters filters{
        &m_logShowSuccess, &m_logShowInfo, &m_logShowWarning, &m_logShowError,
        &m_logAutoScroll, m_logFilter, sizeof(m_logFilter) };
    // The command line only QUEUES (SubmitConsoleLine): the chunk runs from
    // ProcessPendingActions, outside the ImGui frame (LuaConsoleWindow.cpp).
    Editor::OutputLogPanel::ConsoleHooks console;
    console.available = m_scriptSys && m_scriptSys->GetLua() && m_world;
    console.playing   = m_playModeController && !m_playModeController->IsEditing();
    console.submit    = [this](const std::string& text) { SubmitConsoleLine(text, false); };
    m_outputLog.DrawBody(filters, m_hasFontAwesomeIcons, &console);

    ImGui::End();
}

// ==== Loaded Textures (Window > Analysis) ====
// Every texture the TextureSystem holds: the VRAM audit, the rows Lua Asset.textures answers. The
// snapshot (and its users, which need the render lane parked) is taken in ProcessPendingActions; the
// window only filters and sorts.
void EditorLayer::RefreshLoadedTextures()
{
    if (!m_textureSys || !m_resourceMgr) return;
    // Every 60 frames, or 10 after the texture set changed: a scene load moves
    // the revision with every upload, and must not pay for a refresh a frame.
    const std::uint64_t rev = m_textureSys->ContentRevision();
    const std::uint64_t wait = rev != m_loadedTexturesRevision ? 10 : 60;
    if (m_loadedTexturesRevision != 0 && m_editorFrame < m_loadedTexturesFrame + wait) return;
    const auto t0 = std::chrono::steady_clock::now();
    m_loadedTexturesRevision = rev;
    m_loadedTexturesFrame    = m_editorFrame;
    using State = Resource::TextureSystem::Status::State;
    m_loadedTextures.clear();
    std::unordered_map<std::uint64_t, std::uint32_t> users;
    if (m_renderer) m_renderer->TextureConsumerCounts(users);
    for (const Resource::TextureSystem::EntryView& e : m_textureSys->Snapshot(*m_resourceMgr))
    {
        LoadedTextureRow r;
        r.path       = Sys::RelativizeAssetPath(e.path);
        for (char& c : r.path) if (c == '\\') c = '/';
        r.width      = e.info.width;
        r.height     = e.info.height;
        r.depth      = e.info.depth;
        r.mips       = e.info.mipLevels;
        r.arraySize  = e.info.arraySize;
        r.cube       = e.info.isCube;
        r.format     = RHI::FormatName(e.info.format);
        r.compressed = r.format.rfind("BC", 0) == 0;
        r.bytes      = e.info.gpuBytes;
        r.refs       = e.info.refCount;
        r.state      = e.status.state == State::Ready ? 1 : e.status.state == State::Failed ? 2 : 0;
        r.reason     = e.status.reason;
        if (auto it = users.find(Renderer::TextureUserKey(e.path)); it != users.end()) r.users = it->second;
        m_loadedTextures.push_back(std::move(r));
    }
    m_loadedTexturesMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void EditorLayer::RenderLoadedTexturesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(900.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Loaded Textures", &m_showLoadedTextures))
    {
        ImGui::End();
        return;
    }
    // Filters: the path text, then the audit checkboxes.
    ImGui::SetNextItemWidth(220.f);
    ImGui::InputTextWithHint("##ltfilter", "filter by path", m_loadedTexturesFilter, sizeof(m_loadedTexturesFilter));
    ImGui::SameLine();
    ImGui::Checkbox("> 16 MB", &m_loadedTexturesBig);
    ImGui::SameLine();
    ImGui::Checkbox("Uncompressed", &m_loadedTexturesRaw);
    ImGui::SameLine();
    ImGui::Checkbox("One mip", &m_loadedTexturesOneMip);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Larger than 1x1 with a single mip: minified, it shimmers");
    ImGui::SameLine();
    ImGui::Checkbox("Failed", &m_loadedTexturesFailed);

    std::string filter = m_loadedTexturesFilter;
    for (char& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<const LoadedTextureRow*> rows;
    std::uint64_t total = 0;
    std::map<std::string, std::pair<std::uint32_t, std::uint64_t>> byFormat;
    for (const LoadedTextureRow& r : m_loadedTextures)
    {
        if (!filter.empty())
        {
            std::string l = r.path;
            for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (l.find(filter) == std::string::npos) continue;
        }
        if (m_loadedTexturesBig && r.bytes <= 16ull * 1024 * 1024) continue;
        if (m_loadedTexturesRaw && (r.compressed || r.state != 1)) continue;
        if (m_loadedTexturesOneMip && !(r.state == 1 && r.mips == 1 && (r.width > 1 || r.height > 1))) continue;
        if (m_loadedTexturesFailed && r.state != 2) continue;
        rows.push_back(&r);
        total += r.bytes;
        if (r.state == 1) { auto& f = byFormat[r.format]; ++f.first; f.second += r.bytes; }
    }
    ImGui::Text("%zu of %zu texture%s, %s of VRAM", rows.size(), m_loadedTextures.size(),
                m_loadedTextures.size() == 1 ? "" : "s", Resource::FormatByteSize(total).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(read in %.2f ms)", m_loadedTexturesMs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The rows are read again once a second, or soon after a texture loads,\n"
                          "unloads or is reimported. Users = entities whose material samples it.");
    if (ImGui::TreeNode("##byformat", "By format (%zu)", byFormat.size()))
    {
        for (const auto& [fmt, cb] : byFormat)
            ImGui::Text("%-20s %4u  %s", fmt.c_str(), cb.first, Resource::FormatByteSize(cb.second).c_str());
        ImGui::TreePop();
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable;
    if (ImGui::BeginTable("##loadedtex", 8, kFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0.f, 0);
        ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 110.f, 1);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 150.f, 2);
        ImGui::TableSetupColumn("Mips",   ImGuiTableColumnFlags_WidthFixed, 40.f, 3);
        ImGui::TableSetupColumn("VRAM",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                          ImGuiTableColumnFlags_PreferSortDescending, 80.f, 4);
        ImGui::TableSetupColumn("Refs",   ImGuiTableColumnFlags_WidthFixed, 40.f, 5);
        ImGui::TableSetupColumn("Users",  ImGuiTableColumnFlags_WidthFixed, 45.f, 6);
        ImGui::TableSetupColumn("State",  ImGuiTableColumnFlags_WidthFixed, 60.f, 7);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0)
        {
            m_loadedTexturesSort = static_cast<int>(specs->Specs[0].ColumnUserID);
            m_loadedTexturesAsc  = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
        }
        const int col = m_loadedTexturesSort;
        const bool asc = m_loadedTexturesAsc;
        std::stable_sort(rows.begin(), rows.end(), [col, asc](const LoadedTextureRow* a, const LoadedTextureRow* b)
        {
            auto key = [col](const LoadedTextureRow* r) -> double
            {
                switch (col)
                {
                case 1: return double(r->width) * r->height * (std::max)(1u, r->arraySize);
                case 3: return r->mips;
                case 4: return double(r->bytes);
                case 5: return r->refs;
                case 6: return r->users;
                case 7: return r->state;
                default: return 0.0;
                }
            };
            int cmp = 0;
            if (col == 0)      cmp = a->path.compare(b->path);
            else if (col == 2) cmp = a->format.compare(b->format);
            else               cmp = key(a) < key(b) ? -1 : key(a) > key(b) ? 1 : 0;
            if (cmp == 0) cmp = a->path.compare(b->path);
            return asc ? cmp < 0 : cmp > 0;
        });
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
            {
                const LoadedTextureRow& r = *rows[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                if (ImGui::Selectable(r.path.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    Editor::TextureViewerWindow::Get().RequestOpen(r.path);
                if (ImGui::BeginPopupContextItem("##ltctx"))
                {
                    if (ImGui::MenuItem("Open in Texture Viewer")) Editor::TextureViewerWindow::Get().RequestOpen(r.path);
                    if (ImGui::MenuItem("Show in Content Browser"))
                    {
                        SetCurrentAssetDir(Sys::PathToUtf8(Sys::U8Path(r.path).parent_path()));
                        m_showContentBrowser = true;
                    }
                    if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(r.path.c_str());
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered() && !r.reason.empty()) ImGui::SetTooltip("%s", r.reason.c_str());
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                if (r.cube)                ImGui::Text("Cube %ux%u", r.width, r.height);
                else if (r.arraySize > 1)  ImGui::Text("%ux%u x%u", r.width, r.height, r.arraySize);
                else                       ImGui::Text("%ux%u", r.width, r.height);
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(r.format.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::Text("%u", r.mips);
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(Resource::FormatByteSize(r.bytes).c_str());
                ImGui::TableSetColumnIndex(5); ImGui::Text("%u", r.refs);
                ImGui::TableSetColumnIndex(6); ImGui::Text("%u", r.users);
                ImGui::TableSetColumnIndex(7);
                if (r.state == 2)      ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "failed");
                else if (r.state == 1) ImGui::TextUnformatted("ready");
                else                   ImGui::TextDisabled("loading");
            }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ==== Loaded Geometry (Window > Analysis) ====
// A mesh library is something to AUDIT, not browse (the Content Browser shows models and their
// Meshes): who holds it (the world, the asset preview, Play's snapshot pin, a release waiting one
// world), how much VRAM, how many entities draw from it, whether its identity is the file's own or
// synthesized. The rows are Renderer::DescribeLoadedMeshLibraries - the same ones Lua
// Asset.meshLibraries() answers.
void EditorLayer::RenderLoadedGeometryWindow()
{
    ImGui::SetNextWindowSize(ImVec2(760.f, 460.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Loaded Geometry", &m_showLoadedGeometry))
    {
        ImGui::End();
        return;
    }
    Resource::MeshLibrary* meshLib = m_renderer ? m_renderer->GetMeshLibrary() : nullptr;
    if (!meshLib)
    {
        ImGui::TextDisabled("(MeshLibrary not wired)");
        ImGui::End();
        return;
    }

    const std::vector<Renderer::LoadedMeshLibInfo> libs = m_renderer->DescribeLoadedMeshLibraries(m_world);
    uint64_t totalBytes = 0, meshletBytes = 0;
    for (const Renderer::LoadedMeshLibInfo& l : libs)
    {
        totalBytes   += l.vramBytes;
        meshletBytes += l.meshletBytes;
    }
    ImGui::Text("%zu librar%s, %.1f MB of vertex + index buffers, %.1f MB of meshlets", libs.size(),
                libs.size() == 1 ? "y" : "ies", double(totalBytes) / (1024.0 * 1024.0),
                double(meshletBytes) / (1024.0 * 1024.0));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.f);
    ImGui::InputTextWithHint("##lgfilter", "filter by path", m_loadedGeometryFilter, sizeof(m_loadedGeometryFilter));
    ImGui::Separator();

    if (libs.empty())
    {
        ImGui::TextDisabled("No .meshlib is loaded.");
        ImGui::End();
        return;
    }

    std::string filter = m_loadedGeometryFilter;
    for (char& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    ImGui::BeginChild("##lglist");
    for (size_t li = 0; li < libs.size(); ++li)
    {
        const Renderer::LoadedMeshLibInfo& lib = libs[li];
        const Resource::Handle h    = lib.handle;
        const std::string&     path = lib.path;
        if (!filter.empty())
        {
            std::string lower = path;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find(filter) == std::string::npos) continue;
        }
        const uint32_t count   = lib.entries;
        const uint32_t vStride = lib.stride;

        ImGui::PushID(static_cast<int>(li));

        // Who holds it, in words: the header's first column.
        std::string held;
        auto hold = [&held](const std::string& s) { held += (held.empty() ? "" : ", ") + s; };
        if (lib.worldRefs)      hold(lib.worldRefs == 1 ? std::string("world") : "world x" + std::to_string(lib.worldRefs));
        if (lib.preview)        hold("preview");
        if (lib.editorPinned)   hold("Play pin");
        if (lib.pendingRelease) hold("releasing");
        if (held.empty())       hold("unowned");

        std::string basename = path;
        const auto slash = path.find_last_of("/\\");
        if (slash != std::string::npos) basename = path.substr(slash + 1);

        char header[512];
        std::snprintf(header, sizeof(header), "%s   [%s]   %u mesh%s / %u entr%s, %u user%s, %.2f MB, refs %u###lib%zu",
                      basename.c_str(), held.c_str(), lib.meshes, lib.meshes == 1 ? "" : "es", count,
                      count == 1 ? "y" : "ies", lib.users, lib.users == 1 ? "" : "s",
                      double(lib.vramBytes) / (1024.0 * 1024.0), lib.refs, li);
        const bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_SpanFullWidth);
        if (ImGui::IsItemHovered() && !path.empty())
            ImGui::SetTooltip("%s\nidentity: %s", path.c_str(), lib.identity);
        if (ImGui::BeginPopupContextItem("LibContext"))
        {
            if (ImGui::MenuItem("Reveal in Content Browser"))
            {
                const std::filesystem::path folder = Sys::U8Path(path).parent_path();
                SetCurrentAssetDir(Sys::PathToUtf8(folder));
                m_showContentBrowser = true;
            }
            if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(path.c_str());
            ImGui::EndPopup();
        }

        if (open)
        {
            const auto totals = meshLib->GetTotals(h);
            ImGui::Text("Path   : %s", path.c_str());
            ImGui::Text("Held by: %s   (refs %u)   Users: %u   Identity: %s", held.c_str(), lib.refs, lib.users,
                        lib.identity);
            ImGui::Text("Entries: %u   Verts: %u   Idx: %u   Stride: %u B",
                        count, totals.vertexCount, totals.indexCount, vStride);
            // The resident meshlet (MLET) data, or why there is none.
            if (lib.meshletClusters)
                ImGui::Text("Meshlets: %s   %u clusters   %u level records (up to %u per entry)   %.2f MB",
                            lib.meshlets, lib.meshletClusters, lib.meshletLevels, lib.meshletMaxLevels,
                            double(lib.meshletBytes) / (1024.0 * 1024.0));
            else
                ImGui::TextDisabled("Meshlets: %s%s", lib.meshlets,
                                    std::string(lib.meshlets) == "none"
                                        ? " - drawn through the vertex shader (python tools/asset_tool.py meshlets --apply)"
                                        : "");

            // The Meshes its identity names (what the browser lists under a model).
            if (const auto id = meshLib->GetIdentity(h); id && !id->meshes.empty() &&
                ImGui::TreeNodeEx("##meshes", ImGuiTreeNodeFlags_SpanFullWidth, "Meshes (%zu)", id->meshes.size()))
            {
                if (ImGui::BeginTable("idmeshes", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner |
                                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                                      ImVec2(0.0f, 160.0f)))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("name",     ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("key",      ImGuiTableColumnFlags_WidthFixed, 140);
                    ImGui::TableSetupColumn("sections", ImGuiTableColumnFlags_WidthFixed, 110);
                    ImGui::TableSetupColumn("flags",    ImGuiTableColumnFlags_WidthFixed, 110);
                    ImGui::TableHeadersRow();
                    ImGuiListClipper clip;
                    clip.Begin(static_cast<int>(id->meshes.size()));
                    while (clip.Step())
                        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
                        {
                            const Resource::MeshIdentity::Mesh& m = id->meshes[static_cast<size_t>(row)];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(m.name.c_str());
                            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(Resource::MeshKeyToHex(m.key).c_str());
                            ImGui::TableSetColumnIndex(2);
                            std::string secs;
                            for (size_t s = 0; s < m.sections.size() && s < 8; ++s)
                                secs += (s ? "," : "") + std::to_string(m.sections[s]);
                            if (m.sections.size() > 8) secs += ",...";
                            ImGui::TextUnformatted(secs.c_str());
                            ImGui::TableSetColumnIndex(3);
                            ImGui::Text("%s%s%s", (m.flags & Resource::MESH_IDENT_SKINNED) ? "skinned " : "",
                                        (m.flags & Resource::MESH_IDENT_HAS_MORPH) ? "morph " : "",
                                        (m.flags & Resource::MESH_IDENT_HIDDEN) ? "hidden" : "");
                        }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            ImGui::Separator();

            // The per-mesh table is lazy and clipped (Bistro has 22k meshes) so the ImGui pass does not stall.
            if (ImGui::BeginTable("meshes", 6,
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner |
                                  ImGuiTableFlags_RowBg   | ImGuiTableFlags_Resizable,
                                  ImVec2(0.0f, 240.0f)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("meshId",  ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("vStart",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("vCount",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("iStart",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("iCount",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("AABB (min → max)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(count));
                while (clipper.Step())
                {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    {
                        const auto* e = meshLib->GetEntry(h, static_cast<uint32_t>(row));
                        if (!e) continue;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%d",  row);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%u",  e->vertexStart);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%u",  e->vertexCount);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%u",  e->indexStart);
                        ImGui::TableSetColumnIndex(4); ImGui::Text("%u",  e->indexCount);
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("(%.2f,%.2f,%.2f) → (%.2f,%.2f,%.2f)",
                                    e->aabbMin.x, e->aabbMin.y, e->aabbMin.z,
                                    e->aabbMax.x, e->aabbMax.y, e->aabbMax.z);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

// Decal Materials window - CRUD + inspector for DecalMaterialLibrary; opened from Window > Content or
// a Decal's Edit... button.
void EditorLayer::RenderDecalMaterialsWindow()
{
    // Sized large enough for the split layout on first open; user can resize.
    ImGui::SetNextWindowSize(ImVec2(720.f, 540.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Decal Materials", &m_showDecalMaterials))
    {
        ImGui::End();
        return;
    }

    if (!m_renderer)
    {
        ImGui::TextDisabled("(Renderer not wired)");
        ImGui::End();
        return;
    }
    auto& lib = m_renderer->GetDecalMaterialLibrary();
    const auto& assets = lib.GetAll();   // std::unordered_map<string, shared_ptr<DecalMaterialAsset>>

    const bool haveTexSys = (m_textureSys != nullptr && m_gfx != nullptr);

    // ---- Top bar: create a new asset ----
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputTextWithHint("##new_decal_mat_name", "e.g. blood_a",
                             m_newDecalMatNameBuf, sizeof(m_newDecalMatNameBuf));
    ImGui::SameLine();
    const bool canCreate = (m_newDecalMatNameBuf[0] != '\0'
                         && assets.find(m_newDecalMatNameBuf) == assets.end());
    if (!canCreate) ImGui::BeginDisabled();
    if (ImGui::Button("Create"))
    {
        lib.GetOrCreate(m_newDecalMatNameBuf);
        m_selectedDecalMatName = m_newDecalMatNameBuf;
        m_newDecalMatNameBuf[0] = '\0';
    }
    if (!canCreate) ImGui::EndDisabled();

    // Load .idecalmat: adds (or overwrites) an asset in the library.
    ImGui::SameLine();
    static constexpr const char* kDecalMatFilter =
        "Decal Material (*.idecalmat)\0*.idecalmat\0All Files\0*.*\0\0";
    if (ImGui::Button("Load..."))
    {
        std::string path = OpenFileDialog(
            kDecalMatFilter, m_assetDir.empty() ? nullptr : m_assetDir.c_str());
        if (!path.empty())
        {
            // Load to scratch first to read embedded name; copy into named slot so key matches file identity.
            Resource::DecalMaterialAsset scratch;
            if (Resource::LoadDecalMaterial(scratch, path))
            {
                const std::string libName = scratch.name.empty() ? path : scratch.name;
                auto asset = lib.GetOrCreate(libName);
                // Copy fields; keep the library-assigned name as the key.
                for (uint32_t s = 0; s < Resource::DecalMaterialAsset::SLOT_COUNT; ++s)
                    asset->texPaths[s] = scratch.texPaths[s];
                asset->baseColorTint    = scratch.baseColorTint;
                asset->opacity          = scratch.opacity;
                asset->roughness        = scratch.roughness;
                asset->specular         = scratch.specular;
                asset->ao               = scratch.ao;
                asset->normalStrength   = scratch.normalStrength;
                asset->bumpStrength     = scratch.bumpStrength;
                asset->cavityStrength   = scratch.cavityStrength;
                asset->displacementScale = scratch.displacementScale;
                asset->flags            = scratch.flags;
                asset->normalBlendMode  = scratch.normalBlendMode;
                asset->angleFadeStart   = scratch.angleFadeStart;
                asset->sortLayer        = scratch.sortLayer;
                m_selectedDecalMatName  = libName;
            }
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu asset%s)", assets.size(), assets.size() == 1 ? "" : "s");

    ImGui::Separator();

    // ---- Split layout: list | inspector ----
    const float availW  = ImGui::GetContentRegionAvail().x;
    const float listW   = (availW > 500.f) ? 200.f : availW * 0.35f;

    // Left: selectable list
    if (ImGui::BeginChild("##decal_mat_list", ImVec2(listW, 0), ImGuiChildFlags_Borders))
    {
        // Sort names alphabetically for stable UI ordering.
        std::vector<const std::string*> sortedNames;
        sortedNames.reserve(assets.size());
        for (const auto& [name, _] : assets) sortedNames.push_back(&name);
        std::sort(sortedNames.begin(), sortedNames.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });

        for (const std::string* namePtr : sortedNames)
        {
            const bool selected = (m_selectedDecalMatName == *namePtr);
            if (ImGui::Selectable(namePtr->c_str(), selected))
                m_selectedDecalMatName = *namePtr;
        }
        if (assets.empty())
            ImGui::TextDisabled("(empty)");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: inspector for selected asset
    ImGui::BeginChild("##decal_mat_inspector", ImVec2(0, 0), ImGuiChildFlags_Borders);

    auto selectedAsset = lib.Find(m_selectedDecalMatName);
    if (!selectedAsset)
    {
        ImGui::TextDisabled("Select a decal material on the left, or Create a new one.");
    }
    else
    {
        auto& a = *selectedAsset;

        // Name (read-only: renaming would invalidate library key + serialization)
        ImGui::Text("Name: %s", a.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Save..."))
        {
            std::string path = SaveFileDialog(
                kDecalMatFilter, "idecalmat",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
                Resource::SaveDecalMaterial(a, path);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
        {
            if (haveTexSys)
                lib.Destroy(a.name, *m_textureSys, *m_gfx);
            m_selectedDecalMatName.clear();
            ImGui::EndChild();
            ImGui::End();
            return;
        }

        ImGui::Separator();

        // ---- Per-slot texture widget (Unreal-style, 9 channels): path + Clear + ITEX drop + 64x64 thumb ----
        auto drawSlot = [&](Resource::DecalMaterialAsset::TextureSlot slot)
        {
            constexpr float kPreviewSize = 48.f;
            ImGui::PushID(static_cast<int>(slot));

            const int32_t      bindless = a.texBindless[slot];
            const std::string& pathField = a.texPaths[slot];

            // Thumbnail
            uint64_t gpuHandle = 0;
            if (bindless >= 0 && haveTexSys)
            {
                Resource::TextureHandle h = a.texHandles[slot];
                if (h != Resource::kInvalidTextureHandle)
                    if (const RHI::Texture* tex = m_textureSys->GetTexture(h))
                        // The UNORM alias of an sRGB texture, as the Content Browser draws it: ImGui composes in gamma
                        // space.
                        gpuHandle = m_gfx->GetTexturePreviewSrvGpuHandle(*tex);
            }
            if (gpuHandle != 0)
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(gpuHandle)),
                             ImVec2(kPreviewSize, kPreviewSize));
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.f));
                ImGui::Button("##prev", ImVec2(kPreviewSize, kPreviewSize));
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();

            ImGui::BeginGroup();
            ImGui::TextUnformatted(Resource::DecalMaterialAsset::SlotLabel(slot));

            char buf[256];
            strncpy_s(buf, sizeof(buf), pathField.c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.f);
            if (ImGui::InputText("##path", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (haveTexSys)
                    a.SetTexturePath(slot, std::string(buf), *m_textureSys, *m_gfx);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear") && haveTexSys)
                a.SetTexturePath(slot, std::string{}, *m_textureSys, *m_gfx);

            ImGui::TextDisabled("%s",
                bindless >= 0       ? "bound"
                : pathField.empty() ? "empty"
                                    : "loading");
            ImGui::EndGroup();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                {
                    const char* texPath = static_cast<const char*>(p->Data);
                    if (haveTexSys && texPath)
                        a.SetTexturePath(slot, std::string(texPath), *m_textureSys, *m_gfx);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        };

        auto flagCheckbox = [&](const char* label, uint32_t bit)
        {
            bool v = (a.flags & bit) != 0;
            if (ImGui::Checkbox(label, &v))
            {
                if (v) a.flags |=  bit;
                else   a.flags &= ~bit;
            }
        };

        // ---- Group: Base ----
        if (ImGui::CollapsingHeader("Base", 0))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_BASECOLOR);
            drawSlot(Resource::DecalMaterialAsset::SLOT_OPACITY);
            EditorUI::ColorEdit4("Tint", &a.baseColorTint.x, ImGuiColorEditFlags_AlphaBar);
            EditorUI::DragFloat("Opacity Scalar", &a.opacity, 0.0f, 1.0f,
                               "%.2f  (× Opacity tex × Tint.a)");
            flagCheckbox("Write BaseColor", Resource::DecalMaterialAsset::WRITE_BASECOLOR);
        }

        // ---- Group: Surface (Roughness / Specular / AO) ----
        if (ImGui::CollapsingHeader("Surface", 0))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_ROUGHNESS);
            EditorUI::DragFloat("Roughness##s", &a.roughness, 0.0f, 1.0f,
                               "%.2f  (× Roughness tex.r if set)");
            flagCheckbox("Write Roughness", Resource::DecalMaterialAsset::WRITE_ROUGHNESS);
            ImGui::Separator();

            drawSlot(Resource::DecalMaterialAsset::SLOT_SPECULAR);
            EditorUI::DragFloat("Specular##s", &a.specular, 0.0f, 1.0f,
                               "%.2f  (dielectric F0)");
            flagCheckbox("Write Specular", Resource::DecalMaterialAsset::WRITE_SPECULAR);
            ImGui::Separator();

            drawSlot(Resource::DecalMaterialAsset::SLOT_AO);
            EditorUI::DragFloat("AO##s", &a.ao, 0.0f, 1.0f,
                               "%.2f  (× AO tex.r if set)");
            flagCheckbox("Write AO", Resource::DecalMaterialAsset::WRITE_AO);
        }

        // ---- Group: Normal / Bump ----
        if (ImGui::CollapsingHeader("Normal", 0))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_NORMAL);
            EditorUI::DragFloat("Normal Strength", &a.normalStrength, 0.0f, 4.0f, "%.2f");

            drawSlot(Resource::DecalMaterialAsset::SLOT_BUMP);
            EditorUI::DragFloat("Bump Strength", &a.bumpStrength, 0.0f, 2.0f,
                               "%.3f  (0 = flat, 0.1 = subtle, 1.0 = physical, 2+ = exaggerated)");

            flagCheckbox("Write Normal", Resource::DecalMaterialAsset::WRITE_NORMAL);

            static const char* kBlendLabels[] = { "RNM (recommended)", "Lerp" };
            int blendIdx = static_cast<int>(a.normalBlendMode);
            if (ImGui::Combo("Normal Blend", &blendIdx, kBlendLabels, IM_ARRAYSIZE(kBlendLabels)))
                a.normalBlendMode = static_cast<Resource::DecalMaterialAsset::NormalBlendMode>(blendIdx);
        }

        // ---- Group: Detail (Cavity + Displacement) ----
        if (ImGui::CollapsingHeader("Detail"))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_CAVITY);
            EditorUI::DragFloat("Cavity Strength", &a.cavityStrength, 0.0f, 1.0f,
                               "%.2f  (0 = ignore, 1 = full darkening)");
            ImGui::Separator();
            drawSlot(Resource::DecalMaterialAsset::SLOT_DISPLACEMENT);
            EditorUI::DragFloat("Displacement Scale", &a.displacementScale, 0.0f, 0.2f,
                               "%.3f  (parallax UV offset; 0 = off)");
        }

        // ---- Group: Blend & Sort ----
        if (ImGui::CollapsingHeader("Blend & Sort", 0))
        {
            EditorUI::DragFloat("Angle Fade Start", &a.angleFadeStart, 0.0f, 1.0f,
                               "%.2f  (dot(N,Z) threshold)");
            ImGui::DragInt("Sort Layer", &a.sortLayer, 1.0f, -128, 127);
        }

        // ---- Status line ----
        ImGui::Separator();
        const bool resolved = a.IsFullyResolved();
        ImGui::TextColored(
            resolved ? ImVec4(0.4f, 0.9f, 0.4f, 1.f) : ImVec4(0.9f, 0.8f, 0.3f, 1.f),
            resolved ? "All textures resolved." : "Some textures still loading...");
    }

    ImGui::EndChild();
    ImGui::End();
}

// ==== Post Processing / Color Grading panel ====
// Post-process profile inspector: per-property override widgets driven by the same X-macro that
// powers resolve and serialization (PostProcessProperties.inl), so a new property appears here
// automatically. Each row is an override checkbox + the value widget (greyed when not overriding) -
// the Unreal / Unity "bOverride_X" UX. Shared by the global-look panel and the volume inspector.
namespace
{
    enum PPCategory : int { kPPCatExposure, kPPCatColor, kPPCatLens, kPPCatImage,
                            kPPCatStylize, kPPCatCount };
    constexpr const char* kPPCategoryNames[kPPCatCount] = {
        "Exposure", "Color", "Lens", "Image", "Stylize" };

    bool DrawOvBool(const char* label, PostProcess::Overridable<bool>& o)
    {
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##ov", &o.overrideState);
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.overrideState);
        changed |= ImGui::Checkbox(label, &o.value);
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    bool DrawOvFloat(const char* label, PostProcess::Overridable<float>& o, float mn, float mx)
    {
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##ov", &o.overrideState);
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.overrideState);
        changed |= EditorUI::DragFloat(label, &o.value, mn, mx, "%.3f");
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    bool DrawOvFloat3(const char* label, PostProcess::Overridable<DirectX::XMFLOAT3>& o)
    {
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##ov", &o.overrideState);
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.overrideState);
        changed |= ImGui::DragFloat3(label, &o.value.x, 0.01f);
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    bool DrawOvColor(const char* label, PostProcess::Overridable<DirectX::XMFLOAT3>& o)
    {
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##ov", &o.overrideState);
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.overrideState);
        changed |= EditorUI::ColorEdit3(label, &o.value.x);
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    bool DrawOvUint(const char* label, PostProcess::Overridable<uint32_t>& o, uint32_t mn, uint32_t mx)
    {
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##ov", &o.overrideState);
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.overrideState);
        int v = static_cast<int>(o.value);
        if (EditorUI::DragInt(label, &v, static_cast<int>(mn), static_cast<int>(mx)))
        { o.value = static_cast<uint32_t>(v < 0 ? 0 : v); changed = true; }
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }

    // Look categories for the profile inspector's tab bar - the grouping UE's post-process volume and
    // HDRP's volume overrides both use. Keyed by the X-macro GROUP name; every "Stylize: ..." group files
    // itself by label, and a group nobody filed yet lands in "Image" rather than disappearing.
    int PPGroupCategory(const char* group, const char* label)
    {
        auto is = [group](const char* g) { return std::strcmp(group, g) == 0; };
        if (is("autoExposure"))                        return kPPCatExposure;
        if (is("colorGrading") || is("ueColorGrading")) return kPPCatColor;
        if (is("bloom") || is("lensFlare") || is("dof")) return kPPCatLens;
        if (std::strncmp(label, "Stylize", 7) == 0)    return kPPCatStylize;
        return kPPCatImage;   // cas, underwater, and anything not filed yet
    }

    // Effects switched ON per category (a group's 'enabled' bool, overriding and true), shown in the tab
    // label so a volume's profile reads at a glance without opening every tab.
    void CountActiveEffects(const PostProcess::PostProcessProfile& p, int (&active)[kPPCatCount])
    {
        int cur = kPPCatImage;
#define PP_GROUP(g, l)       cur = PPGroupCategory(#g, l);
#define PP_BOOL(g, m, l, d)  if (std::strcmp(#m, "enabled") == 0 && p.g.m.overrideState && p.g.m.value) ++active[cur];
#include "PostProcess/PostProcessProperties.inl"
    }

    // Draw every property of ONE category, grouped. Returns true if anything changed.
    bool DrawProfileCategory(PostProcess::PostProcessProfile& p, int cat)
    {
        bool changed = false;
        // Scope every property by its unique "group_member" name so duplicate
        // visible labels (e.g. "Levels" in both Posterize and Dither) don't
        // collide on the ImGui ID (which is derived from the widget label).
        //
        // Unreal/Unity-style fold: a group's body is drawn (indented) only while
        // its 'enabled' toggle is overridden or on (see PP_BOOL), and collapses to
        // just the header + toggle otherwise. The 'enabled' bool is always the FIRST
        // property of its group in PostProcessProperties.inl, so PP_BOOL latches
        // `ppOpen` for every PP_* that follows until the next PP_GROUP. Groups with
        // no 'enabled' member (e.g. Bloom) have nothing to gate on and stay open.
        // `ppSkip` drops every group outside the requested category.
        bool ppOpen   = true;   // is the current group's body being drawn?
        bool ppIndent = false;  // have we Indent()-ed the current group's body?
        bool ppSkip   = true;   // current group belongs to another category
#define PP_ENDSEC()  do { if (ppIndent) { ImGui::Unindent(); ppIndent = false; } } while (0)
#define PP_BODY(...) do { if (ppOpen && !ppSkip) { if (!ppIndent) { ImGui::Indent(); ppIndent = true; } __VA_ARGS__; } } while (0)
#define PP_GROUP(g, l)                 PP_ENDSEC(); ppSkip = (PPGroupCategory(#g, l) != cat); if (!ppSkip) ImGui::SeparatorText(l); ppOpen = true;
// The group body opens on overrideState OR value: keying on `value` alone would hide every member of
// a group whose bool is not overridden (a default profile is {overrideState=false, value=false}).
// Volume profiles are per-property (the resolve blends each field independently of the group bool),
// so a non-overriding bool must not suppress its siblings.
#define PP_BOOL(g, m, l, d)            if (!ppSkip) { ImGui::PushID(#g "_" #m); changed |= DrawOvBool(l, p.g.m); ImGui::PopID(); } ppOpen = p.g.m.overrideState || p.g.m.value;
#define PP_FLOAT(g, m, l, d, a, b)     PP_BODY(ImGui::PushID(#g "_" #m); changed |= DrawOvFloat(l, p.g.m, (a), (b)); ImGui::PopID());
#define PP_FLOAT3(g, m, l, dx, dy, dz) PP_BODY(ImGui::PushID(#g "_" #m); changed |= DrawOvFloat3(l, p.g.m); ImGui::PopID());
#define PP_COLOR(g, m, l, dr, dg, db)  PP_BODY(ImGui::PushID(#g "_" #m); changed |= DrawOvColor(l, p.g.m); ImGui::PopID());
#define PP_UINT(g, m, l, d, a, b)      PP_BODY(ImGui::PushID(#g "_" #m); changed |= DrawOvUint(l, p.g.m, (uint32_t)(a), (uint32_t)(b)); ImGui::PopID());
#include "PostProcess/PostProcessProperties.inl"
        PP_ENDSEC();   // balance the final group's indent
#undef PP_ENDSEC
#undef PP_BODY
        return changed;
    }
}

// Every property of a profile, one tab per look category; returns true if anything changed. Shared
// by the Post Processing panel (engine default) and the volume inspector. Declared in
// EditorLayerShared.h.
bool DrawProfileInspector(PostProcess::PostProcessProfile& p)
{
    int active[kPPCatCount] = {};
    CountActiveEffects(p, active);
    bool changed = false;
    if (ImGui::BeginTabBar("##ppLookCategories"))
    {
        for (int c = 0; c < kPPCatCount; ++c)
        {
            // "###" keeps the tab ID stable while the count in the label changes.
            char label[64];
            if (active[c] > 0)
                std::snprintf(label, sizeof(label), "%s (%d)###ppcat%d",
                              kPPCategoryNames[c], active[c], c);
            else
                std::snprintf(label, sizeof(label), "%s###ppcat%d", kPPCategoryNames[c], c);
            if (ImGui::BeginTabItem(label))
            {
                changed |= DrawProfileCategory(p, c);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    return changed;
}

void EditorLayer::RenderEnvironmentWindow()
{
    // Environment panel: one window over the scene-environment state spread across
    // singleton-by-convention entities ("World Environment", "Sky", "Time of Day", the
    // IndirectLightingSettings entity, SunLightTag / MoonLightTag lights). Widgets act directly on the
    // live components (the ones the Inspector edits). Find-or-create follows Create > Environment: a
    // deleted env entity must stick, so entities appear only through the explicit Create buttons.
    // TODO(undo): most panel edits bypass the undo stack; only the Fog section goes through
    // DrawComponentBodyWithUndo / AddComponentWithUndo, which the other sections could adopt.
    ImGui::SetNextWindowSize(ImVec2(520.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Environment", &m_showEnvironmentWindow))
    {
        ImGui::End();
        return;
    }
    if (!m_world)
    {
        ImGui::TextDisabled("(world not available)");
        ImGui::End();
        return;
    }

    // ---- WorldEnvironment singleton (first pool entry wins) ----
    Entity envEntity = NullEntity;
    if (auto* p = m_world->GetPool<WorldEnvironmentComponent>();
        p && !p->Entities().empty())
        envEntity = p->Entities()[0];
    if (envEntity == NullEntity)
    {
        ImGui::TextWrapped("No World Environment singleton in this scene — the "
                           "renderer falls back to legacy per-component behaviour.");
        if (ImGui::Button("Create World Environment"))
        {
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, "World Environment");
            m_world->AddComponent<WorldEnvironmentComponent>(e, WorldEnvironmentComponent{});
            envEntity        = e;
            m_selectedEntity = e;
        }
    }
    auto* wec = envEntity != NullEntity
              ? m_world->GetComponent<WorldEnvironmentComponent>(envEntity)
              : nullptr;
    if (!wec)
    {
        ImGui::End();
        return;
    }

    // The "Sky" entity - the AtmosphereComponent carrier (Create-menu convention); falls back to a bare
    // SkyboxComponent carrier for a static-sky scene.
    Entity skyEntity = NullEntity;
    if (auto* p = m_world->GetPool<AtmosphereComponent>();
        p && !p->Entities().empty())
        skyEntity = p->Entities()[0];
    if (skyEntity == NullEntity)
        if (auto* p = m_world->GetPool<SkyboxComponent>();
            p && !p->Entities().empty())
            skyEntity = p->Entities()[0];

    // ---- Mode ----
    ImGui::SeparatorText("Mode");
    {
        static const char* kModeLbl[] = { "None (no sky)", "Flat Color",
                                          "Static Skybox (cubemap)",
                                          "Procedural Atmosphere" };
        int mode = static_cast<int>(wec->mode);
        if (ImGui::Combo("Mode", &mode, kModeLbl, IM_ARRAYSIZE(kModeLbl)))
        {
            wec->mode = static_cast<WorldEnvironmentComponent::Mode>(mode);
            // Create-menu parity: switching to a sky-backed mode ensures the EXISTING Sky entity carries the
            // matching parameter component. A deleted Sky entity is never resurrected here (see the button
            // below).
            if (skyEntity != NullEntity)
            {
                if (wec->mode == WorldEnvironmentComponent::Mode::StaticSkybox &&
                    !m_world->HasComponent<SkyboxComponent>(skyEntity))
                    m_world->AddComponent<SkyboxComponent>(skyEntity, SkyboxComponent{});
                if (wec->mode == WorldEnvironmentComponent::Mode::ProceduralAtmosphere &&
                    !m_world->HasComponent<AtmosphereComponent>(skyEntity))
                    m_world->AddComponent<AtmosphereComponent>(skyEntity, AtmosphereComponent{});
            }
        }
        const bool needsSky =
            wec->mode == WorldEnvironmentComponent::Mode::StaticSkybox ||
            wec->mode == WorldEnvironmentComponent::Mode::ProceduralAtmosphere;
        if (needsSky && skyEntity == NullEntity)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                               "No Sky entity — this mode has no parameter source.");
            if (ImGui::Button("Create Sky Entity"))
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "Sky");
                m_world->AddComponent<AtmosphereComponent>(e, AtmosphereComponent{});
                if (wec->mode == WorldEnvironmentComponent::Mode::StaticSkybox)
                    m_world->AddComponent<SkyboxComponent>(e, SkyboxComponent{});
                skyEntity        = e;
                m_selectedEntity = e;
            }
        }
    }

    // The Sky entity's AtmosphereComponent (the inherit fallback below) and the IndirectLightingSettings
    // singleton. Fetched after the mode block so a component it added this frame is picked up.
    AtmosphereComponent* atmos = skyEntity != NullEntity
        ? m_world->GetComponent<AtmosphereComponent>(skyEntity) : nullptr;
    IndirectLightingSettingsComponent* ils = nullptr;
    if (auto* p = m_world->GetPool<IndirectLightingSettingsComponent>();
        p && !p->Entities().empty())
        ils = &p->Data()[0];

    // ---- Sky IBL Intensity (unified) ----
    // The slider shows the RESOLVED value: a negative authored value shows the AtmosphereComponent's live
    // value (0.1 without one) tagged "(inherited)"; dragging authors (>= 0) onto the unified
    // WorldEnvironment field, which then wins. The sky light has one intensity.
    ImGui::SeparatorText("Sky IBL Intensity (unified)");
    {
        const bool inherited = wec->iblStrength < 0.0f;
        float v = inherited ? (atmos ? atmos->iblStrength : 0.1f)
                            : wec->iblStrength;
        if (EditorUI::DragFloat("IBL Strength", &v, 0.0f, 3.0f, "%.3f",
                               ImGuiSliderFlags_AlwaysClamp))
            wec->iblStrength = std::max(v, 0.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("Inherit##iblStrength"))
            wec->iblStrength = -1.0f;
        if (inherited) { ImGui::SameLine(); ImGui::TextDisabled("(inherited)"); }
    }

    // ---- GI / Probes ----
    ImGui::SeparatorText("GI / Probes");
    if (ils)
    {
        ImGui::Checkbox("DDGI Enabled", &ils->ddgiEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "DDGI is the lowest tier that occludes the sky light. Off (or no\n"
                "DDGI volume) with no ReSTIR GI = DEBUG quality: the sky lights a\n"
                "closed room as if it stood outdoors (the log says [SKYLIGHT]\n"
                "unoccluded; View Mode > Sky Visibility shows it).");
        EditorUI::DragFloat("DDGI Diffuse Scale", &ils->ddgiDiffuseScale,           0.0f, 8.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("1 is physical (the probes store E/pi). It also scales SSGI and\n"
                              "ReSTIR GI. Not pi.");
        EditorUI::DragFloat("DDGI Near-Field AO", &ils->ddgiAONearFieldStrength,    0.0f, 1.0f);
        EditorUI::DragFloat("Probe Bake Ambient", &ils->reflectionProbeBakeAmbient, 0.0f, 2.0f);

        // ---- Screen-space GI (the near-field half of the hybrid) ----
        // This panel is hand-written and does not consume ComponentReflection, so a field added to
        // REFLECT_BEGIN(IndirectLightingSettingsComponent) shows up in the Inspector but NOT here. Both edit
        // the same component; keep them in sync deliberately.
        ImGui::Spacing();
        ImGui::Checkbox("SSGI Enabled", &ils->ssgiEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Near-field screen-space diffuse GI. DDGI keeps the far field and\n"
                "the two split the hemisphere, so OFF is bit-identical to before.\n"
                "It owns Hi-Z; reflections reuse that pyramid when both are on.");
        if (ils->ssgiEnabled)
        {
            ImGui::Indent();
            // 0..1: the fold saturates it.
            EditorUI::DragFloat("SSGI Intensity",   &ils->ssgiIntensity,   0.0f,  1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Kill switch / A-B fade ONLY. Brightness is DDGI Diffuse Scale,\n"
                    "shared with DDGI on purpose - a second scale would turn every\n"
                    "coverage gradient into a visible band. No effect under ReSTIR GI.");
            EditorUI::DragFloat("SSGI Max Distance",&ils->ssgiMaxDistance, 0.5f, 20.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "World metres. Keep it BELOW probe spacing - this tier exists\n"
                    "to supply what the probe grid cannot resolve.");
            int rays = (int)ils->ssgiRayCount;
            if (EditorUI::DragInt("SSGI Rays / Pixel", &rays, 2, 16))
                ils->ssgiRayCount = (uint32_t)(rays < 2 ? 2 : rays);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Legacy path only. The reservoir path always emits one ray per\n"
                    "half-res pixel, equivalent to 0.25 full-res rpp.");
            ImGui::Checkbox("SSGI Reservoir Reuse", &ils->ssgiReservoirEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "One fresh half-res ray + temporal reservoir + spatial\n"
                    "reconnection, then the existing REBLUR denoiser. Disable\n"
                    "to A/B against the legacy 2/4-rpp trace.");
            if (ils->ssgiReservoirEnabled)
            {
                int neighbours = (int)ils->ssgiReservoirSpatialCandidates;
                if (EditorUI::DragInt("Reservoir Neighbours", &neighbours, 0, 8))
                    ils->ssgiReservoirSpatialCandidates = (uint32_t)std::clamp(neighbours, 0, 8);
                int maxM = (int)ils->ssgiReservoirMaxM;
                if (EditorUI::DragInt("Reservoir Max M", &maxM, 1, 63))
                    ils->ssgiReservoirMaxM = (uint32_t)std::clamp(maxM, 1, 63);
                int maxAge = (int)ils->ssgiReservoirMaxAge;
                if (EditorUI::DragInt("Reservoir Max Age", &maxAge, 1, 63))
                    ils->ssgiReservoirMaxAge = (uint32_t)std::clamp(maxAge, 1, 63);
                int visibilityTaps = (int)ils->ssgiReservoirValidationSteps;
                if (EditorUI::DragInt("Reservoir Visibility Taps", &visibilityTaps, 4, 6))
                    ils->ssgiReservoirValidationSteps =
                        (uint32_t)std::clamp(visibilityTaps, 4, 6);
            }
            EditorUI::DragFloat("SSGI Thickness",   &ils->ssgiThickness,   0.01f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Hit-uncertainty gate. Raise if thin geometry loses bounce;\n"
                    "lower if rays accept surfaces they actually passed behind.");

            const char* kDbg[5] = { "Off",
                                    "1 Calibration (must NOT change)",
                                    "2 MAGENTA (binding test - start here)",
                                    "3 Coverage",
                                    "4 Near term only" };
            int dbg = (int)ils->ssgiDebugMode;
            if (dbg < 0 || dbg > 4) dbg = 0;
            if (ImGui::Combo("SSGI Debug View", &dbg, kDbg, 5))
                ils->ssgiDebugMode = (uint32_t)dbg;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "START WITH 2. It writes a CONSTANT magenta at coverage 1,\n"
                    "ignoring the trace and the denoiser entirely - so the\n"
                    "screen MUST turn magenta. If it does not, the problem is\n"
                    "ssgiResultIdx / the Lighting.ps fold, and nothing\n"
                    "downstream is worth looking at.\n\n"
                    "Then 3 (coverage) and 4 (radiance) isolate the march and\n"
                    "the previous-frame fetch. 1 is a NULL test - only\n"
                    "meaningful after 2 passes.");
            if (dbg != 0)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                                   "DEBUG VIEW ON - not a shipping mode");
            ImGui::Unindent();
        }
    }
    else
    {
        ImGui::TextDisabled("No IndirectLightingSettings entity in this scene.");
        if (ImGui::Button("Create Indirect Lighting Settings"))
        {
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, "Indirect Lighting Settings");
            m_world->AddComponent<IndirectLightingSettingsComponent>(
                e, IndirectLightingSettingsComponent{});
            m_selectedEntity = e;
        }
    }
    {
        // Same rebake path as the per-probe "Bake This Probe" button, applied
        // to every probe that already owns a cubemap-array slice (unassigned
        // probes get their first bake auto-queued by the Renderer anyway).
        auto* probePool = m_world->GetPool<ReflectionProbeComponent>();
        const int probeCount = probePool ? static_cast<int>(probePool->Data().size()) : 0;
        const bool canBake = m_renderer && probeCount > 0;
        if (!canBake) ImGui::BeginDisabled();
        if (ImGui::Button("Rebake All Probes") && probePool)
        {
            for (auto& probe : probePool->Data())
            {
                if (probe.cubemapSlice == ReflectionProbeComponent::kInvalidSlice)
                    continue;
                probe.RequestRebake();
                m_renderer->BakeProbe(probe.cubemapSlice);
            }
        }
        if (!canBake) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%d probe%s", probeCount, probeCount == 1 ? "" : "s");
    }

    // ---- Per-source parameters (gated by mode) ----
    switch (wec->mode)
    {
    case WorldEnvironmentComponent::Mode::None:
    case WorldEnvironmentComponent::Mode::FlatColor:
    {
        // The same two fields mean different things per mode (see WorldEnvironment.h).
        const bool flat = wec->mode == WorldEnvironmentComponent::Mode::FlatColor;
        ImGui::SeparatorText(flat ? "Background / Uniform Sky Light" : "Background / Fill");
        EditorUI::ColorEdit3("Background", &wec->flatColor.x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What the camera sees behind everything. Not a light.");
        EditorUI::ColorEdit3(flat ? "Sky Color" : "Fill Color (non-physical)", &wec->ambientColor.x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(flat
                ? "The radiance of a uniform sky that lights the scene like any sky:\n"
                  "DDGI / ReSTIR GI occlude and bounce it, reflections and the path\n"
                  "tracer see it. Color x Intensity; iblStrength does not apply."
                : "A constant ambient on top of everything, unoccluded and unseen by\n"
                  "GI and the path tracer (a metal reflects it, it is not diffuse on\n"
                  "a metal). For stylised or preview scenes; FlatColor makes it a sky.");
        EditorUI::DragFloat(flat ? "Sky Intensity" : "Fill Intensity", &wec->ambientIntensity, 0.0f, 8.0f);
        break;
    }

    case WorldEnvironmentComponent::Mode::StaticSkybox:
        ImGui::SeparatorText("Static Skybox");
        if (auto* sb = skyEntity != NullEntity
                     ? m_world->GetComponent<SkyboxComponent>(skyEntity) : nullptr)
            Reflect::DrawObject(sb);
        else
            ImGui::TextDisabled("(Sky entity has no SkyboxComponent)");
        break;

    case WorldEnvironmentComponent::Mode::ProceduralAtmosphere:
        ImGui::SeparatorText("Procedural Atmosphere");
        if (atmos)
            Reflect::DrawObject(atmos);
        else
            ImGui::TextDisabled("(Sky entity has no AtmosphereComponent)");
        if (skyEntity != NullEntity)
        {
            // Optional sky extras: a presence indicator or an explicit add; the full knob sets live on the Sky
            // entity's Inspector.
            if (m_world->HasComponent<CloudComponent>(skyEntity))
                ImGui::TextDisabled("Volumetric Clouds: present (tune on the Sky entity).");
            else if (ImGui::Button("Add Volumetric Clouds"))
            {
                CloudComponent c{}; c.enabled = true;
                m_world->AddComponent<CloudComponent>(skyEntity, c);
            }
        }
        break;
    }

    // ---- Fog ----
    // Three layers, the UE / HDRP arrangement: the froxel VOLUMETRIC fog near the camera (the scene's
    // global medium), bounded LOCAL fog volumes on top of it, and the analytic HEIGHT fog that carries
    // the far distance the froxel grid does not reach. Edits here go through the same undo bracket as
    // the Inspector.
    ImGui::SeparatorText("Fog");
    {
        const ImGuiTreeNodeFlags kOpen = ImGuiTreeNodeFlags_DefaultOpen;

        // -- Volumetric (global) --
        Entity fogEntity = NullEntity;
        if (auto* p = m_world->GetPool<VolumetricFogComponent>(); p && !p->Entities().empty())
            fogEntity = p->Entities()[0];
        ImGui::PushID("volfog");
        if (ImGui::TreeNodeEx("Volumetric Fog (global)", kOpen))
        {
            if (auto* vfc = fogEntity != NullEntity
                          ? m_world->GetComponent<VolumetricFogComponent>(fogEntity) : nullptr)
            {
                DrawComponentBodyWithUndo(fogEntity, std::type_index(typeid(VolumetricFogComponent)),
                                          [&] { Reflect::DrawObject(vfc); });
                DrawVolumetricFogStatus();
                if (ImGui::SmallButton("Select Entity")) m_selectedEntity = fogEntity;
            }
            else
            {
                const VolumetricFogPass* pass =
                    m_renderer ? m_renderer->GetVolumetricFogPass() : nullptr;
                // The .ippc's switch is the scene's intent; the player's Volumetric Fog setting only allows it.
                ImGui::TextWrapped("This scene does not own its fog: its .ippc decides whether it "
                                   "has fog (currently %s), and the player's Volumetric Fog "
                                   "setting can turn any fog off.",
                                   pass && pass->IsEnabled() ? "ON" : "OFF");
                if (ImGui::Button("Add Volumetric Fog to Scene"))
                    RecordSpawn("Volumetric Fog",
                                [&] { m_selectedEntity = SpawnVolumetricFogFromLegacy(); });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Seeded from the current values, so the picture does not\n"
                                      "change. From then on the scene owns its fog (its .ippc's\n"
                                      "switch no longer applies to it).");
                DrawVolumetricFogStatus();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();

        // -- Local volumes --
        ImGui::PushID("localfog");
        if (ImGui::TreeNodeEx("Local Fog Volumes", kOpen))
        {
            auto* pool = m_world->GetPool<LocalFogVolumeComponent>();
            const size_t n = pool ? pool->Entities().size() : 0;
            if (n == 0)
                ImGui::TextDisabled("None in this scene.");
            for (size_t i = 0; i < n; ++i)
            {
                const Entity e = pool->Entities()[i];
                const LocalFogVolumeComponent& v = pool->Data()[i];
                char label[160];
                std::snprintf(label, sizeof(label), "%s%s  (%s, density %.3f)##lfv%u",
                              m_world->GetName(e).c_str(), v.enabled ? "" : " [off]",
                              v.shape == LocalFogShape::Sphere ? "sphere" : "box",
                              v.density, static_cast<unsigned>(e));
                if (ImGui::Selectable(label, m_selectedEntity == e))
                    m_selectedEntity = e;
            }
            if (ImGui::Button("Create Local Fog Volume"))
                RecordSpawn("Local Fog Volume", [&] { m_selectedEntity = SpawnLocalFogVolume(); });
            ImGui::SameLine();
            ImGui::TextDisabled("(bounds = Transform; Show > Fog Volumes draws them)");
            ImGui::TreePop();
        }
        ImGui::PopID();

        // -- Height fog (analytic) --
        Entity hfEntity = NullEntity;
        if (auto* p = m_world->GetPool<HeightFogComponent>(); p && !p->Entities().empty())
            hfEntity = p->Entities()[0];
        ImGui::PushID("heightfog");
        if (ImGui::TreeNodeEx("Height Fog (analytic, far distance)",
                              hfEntity != NullEntity ? kOpen : 0))
        {
            if (auto* hf = hfEntity != NullEntity
                         ? m_world->GetComponent<HeightFogComponent>(hfEntity) : nullptr)
            {
                DrawComponentBodyWithUndo(hfEntity, std::type_index(typeid(HeightFogComponent)),
                                          [&] { Reflect::DrawObject(hf); });
                if (ImGui::SmallButton("Select Entity")) m_selectedEntity = hfEntity;
            }
            else if (skyEntity != NullEntity)
            {
                if (ImGui::Button("Add Height Fog"))
                    AddComponentWithUndo(skyEntity, std::type_index(typeid(HeightFogComponent)),
                        [](World& w, Entity e) {
                            HeightFogComponent hf{}; hf.enabled = true;
                            w.AddComponent<HeightFogComponent>(e, hf);
                        });
                ImGui::SameLine();
                ImGui::TextDisabled("(added to the Sky entity)");
            }
            else
                ImGui::TextDisabled("Needs a Sky entity (Create > Environment).");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ---- Time of Day ----
    ImGui::SeparatorText("Time of Day");
    {
        Entity todEntity = NullEntity;
        if (auto* p = m_world->GetPool<TODConfigComponent>();
            p && !p->Entities().empty())
            todEntity = p->Entities()[0];

        if (todEntity == NullEntity)
        {
            if (ImGui::Button("Create Time of Day"))
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "Time of Day");
                TODConfigComponent cfg{};
                cfg.enabled = true;
                m_world->AddComponent<TODConfigComponent>(e, cfg);
                m_world->AddComponent<TODOutputComponent>(e, TODOutputComponent{});
                wec->enableTimeOfDay = true;
                m_selectedEntity     = e;
            }
        }
        else if (auto* cfg = m_world->GetComponent<TODConfigComponent>(todEntity))
        {
            ImGui::Checkbox("Enabled##tod", &cfg->enabled);
            ImGui::SliderFloat("Time (0=midnight, 0.5=noon)", &cfg->timeOfDay, 0.0f, 1.0f);
            EditorUI::DragFloat("Speed (day/sec)", &cfg->timeSpeed,          0.0f,  0.5f);
            EditorUI::DragFloat("Latitude (rad)",  &cfg->latitudeRad,       -1.55f, 1.55f);
            EditorUI::DragFloat("Sun Brightness",  &cfg->sunBrightnessScale, 0.0f,  5.0f);
            EditorUI::DragFloat("Moon Intensity",  &cfg->moonIntensityScale, 0.0f,  1.0f);

            if (auto* out = m_world->GetComponent<TODOutputComponent>(todEntity))
            {
                ImGui::TextDisabled("Sun Dir:   (%.2f, %.2f, %.2f)",
                                    out->sunDirection.x, out->sunDirection.y, out->sunDirection.z);
                ImGui::TextDisabled("Sun Color: (%.2f, %.2f, %.2f)",
                                    out->sunColor.x, out->sunColor.y, out->sunColor.z);
                ImGui::TextDisabled("Moon Active: %s", out->isMoonActive ? "yes" : "no");
            }
        }

        // Tagged sun / moon lights: create buttons only while the tag is absent (Create-menu parity; deleting
        // a tagged light must stick).
        bool hasSun = false, hasMoon = false;
        if (auto* p = m_world->GetPool<SunLightTag>();  p && !p->Entities().empty()) hasSun  = true;
        if (auto* p = m_world->GetPool<MoonLightTag>(); p && !p->Entities().empty()) hasMoon = true;
        auto createTaggedLight = [this](bool moon) {
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, moon ? "Moon Light" : "Sun Light");
            LocalTransform lt{};
            lt.translation = ComputeSpawnPosition();
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
            // Aim via the transform: LightData has no direction (LightUtil.h).
            lt.rotation = LightUtil::RotationAiming({ 0.447f, -0.894f, 0.224f });
            if (auto* live = m_world->GetComponent<LocalTransform>(e)) live->rotation = lt.rotation;
            LightData ld;
            ld.type      = LightType::Directional;
            ld.color     = moon ? DirectX::XMFLOAT3{ 0.55f, 0.70f, 1.0f }
                                : DirectX::XMFLOAT3{ 1.0f, 0.96f, 0.9f };
            ld.intensity = moon ? 0.2f : 1.0f;
            m_world->AddComponent<LightData>(e, ld);
            if (moon) m_world->AddComponent<MoonLightTag>(e, MoonLightTag{});
            else      m_world->AddComponent<SunLightTag>(e, SunLightTag{});
            m_selectedEntity = e;
        };
        if (!hasSun && ImGui::Button("Create Sun Light"))
            createTaggedLight(false);
        if (!hasMoon)
        {
            if (!hasSun) ImGui::SameLine();
            if (ImGui::Button("Create Moon Light"))
                createTaggedLight(true);
        }
    }

    ImGui::End();
}

void EditorLayer::RenderPostProcessPanel()
{
    // Post Processing is the LOOK only: the base look asset (.ppprofile) and the engine-default profile
    // (grouped Exposure / Color / Lens / Image / Stylize like UE and HDRP), the scene's Post-Process
    // Volumes (list + which ones the camera is in), and a test bench for the gameplay override stack. AA
    // mode, render scale, FSR sharpness and TAA / FXAA tuning are in Project Settings > Upscaling & AA;
    // XeGTAO / Outline tuning and the .ippc + scene script bindings in World Settings (per scene);
    // volumetric fog in Environment > Fog.
    ImGui::SetNextWindowSize(ImVec2(540.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Post Processing", &m_showPostProcess);

    ToneMapPass* toneMap = m_renderer ? m_renderer->GetToneMapPass() : nullptr;
    if (!toneMap)
    {
        ImGui::TextDisabled("(renderer not available)");
        ImGui::End();
        return;
    }

    // The PostChain is a pure consumer; the look lives in the profile system.
    if (!m_renderer->GetPostChain())
    {
        ImGui::TextDisabled("(post-process chain not initialized)");
        ImGui::End();
        return;
    }

    auto& profiles    = PostProcess::ProfileSystem::Get();
    auto& engineLook  = profiles.EngineDefault();

    // ---- Base look asset (.ppprofile) ----
    // The engine-default profile is the base every volume blends over. A scene's World Settings > Render
    // Config records which .ppprofile it loads.
    {
        static constexpr const char* kProfFilter =
            "Post-Process Profile (*.ppprofile)\0*.ppprofile\0All Files\0*.*\0\0";
        EditorTheme::Heading("Base Look");
        if (ImGui::Button("Save Look..."))
        {
            std::string path = SaveFileDialog(kProfFilter, "ppprofile",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            // Serialized into the .ippc, which a scene then references - keep it relative.
            if (!path.empty() && PostProcess::SaveProfile(engineLook, path))
                m_engineProfilePath = Sys::RelativizeAssetPath(path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Look..."))
        {
            std::string path = OpenFileDialog(kProfFilter,
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
            {
                engineLook = PostProcess::MakeEngineDefaultProfile();
                if (PostProcess::LoadProfile(path, engineLook))
                    m_engineProfilePath = Sys::RelativizeAssetPath(path);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Look"))
            engineLook = PostProcess::MakeEngineDefaultProfile();
        ImGui::SameLine();
        if (m_engineProfilePath.empty())
            ImGui::TextDisabled("(not saved as a .ppprofile yet)");
        else
            ImGui::TextDisabled("%s", m_engineProfilePath.c_str());
        ImGui::SetItemTooltip("The engine-default profile: the base every Post-Process\n"
                              "Volume blends over, then gameplay overrides on top.\n"
                              "Which .ppprofile a scene loads is recorded in its\n"
                              "Render Config (Window > World Settings).");
    }

    // ---- Global Look: the engine-default profile, per-property overrides ----
    EditorTheme::Heading("Global Look");
    DrawProfileInspector(engineLook);

    // ---- Post-Process Volumes ----
    // Volumes are ordinary entities (bounds = Transform). Listed here so the panel answers "what is
    // layered over the base look in this scene".
    auto& rt = PostProcess::Runtime::Get();
    if (ImGui::CollapsingHeader("Post-Process Volumes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto* pool = m_world ? m_world->GetPool<ECS::PostProcessVolumeComponent>() : nullptr;
        const size_t n = pool ? pool->Entities().size() : 0;
        if (n == 0)
            ImGui::TextDisabled("No volumes in this scene.");
        for (size_t i = 0; i < n; ++i)
        {
            const Entity e = pool->Entities()[i];
            const ECS::PostProcessVolumeComponent& v = pool->Data()[i];
            const char* shape = v.isGlobal ? "global"
                              : (v.shape == ECS::PPVolumeShape::Sphere ? "sphere" : "box");
            char label[192];
            std::snprintf(label, sizeof(label), "%s  (%s, priority %.0f, weight %.2f)%s##ppv%u",
                          m_world->GetName(e).c_str(), shape, v.priority, v.blendWeight,
                          v.profilePath.empty() ? "" : "  *", static_cast<unsigned>(e));
            if (ImGui::Selectable(label, m_selectedEntity == e))
                m_selectedEntity = e;
            if (!v.profilePath.empty())
                ImGui::SetItemTooltip("Profile: %s", v.profilePath.c_str());
        }
        if (m_world)
        {
            if (ImGui::Button("+ Global Volume"))
                RecordSpawn("Post-Process Volume", [&] { m_selectedEntity = SpawnPostProcessVolume(true); });
            ImGui::SameLine();
            if (ImGui::Button("+ Box Volume"))
                RecordSpawn("Post-Process Volume", [&] { m_selectedEntity = SpawnPostProcessVolume(false); });
            ImGui::SameLine();
            ImGui::TextDisabled("(edit the look in the Inspector)");
        }

        // Which volumes the main camera is inside, with each one's effective
        // weight (filled by PostProcessResolveSystem while this is drawn).
        ImGui::Spacing();
        ImGui::TextDisabled("Camera is inside:");
        rt.debugCapture = true;
        if (rt.debugHits.empty())
            ImGui::TextDisabled("  (no contributing volume)");
        for (const auto& h : rt.debugHits)
            ImGui::Text("  %-24s w=%.3f  prio=%.1f%s",
                h.label, h.weight, h.priority, h.isGlobal ? "  [global]" : "");
    }
    else
    {
        rt.debugCapture = false;
    }

    // ---- Gameplay Override Stack ----
    // Non-spatial, time-enveloped overrides (damage flash, cool pulse, ...) pushed onto
    // PostProcess::Runtime and applied last in the resolve, above all volumes.
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Gameplay Overrides (test bench)", 0))
    {
        auto& rt = PostProcess::Runtime::Get();

        if (ImGui::Button("Damage Flash"))
        {
            PostProcess::PostProcessOverride ov;
            ov.priority = 2000.0f;
            ov.fadeIn = 0.05f; ov.hold = 0.10f; ov.fadeOut = 0.45f;
            snprintf(ov.label, sizeof(ov.label), "DamageFlash");
            auto& cg = ov.profile.colorGrading;
            cg.enabled.Set(true);
            cg.temperature.Set(1.0f);
            cg.saturation.Set(1.6f);
            cg.contrast.Set(1.3f);
            cg.vignetteStrength.Set(0.6f);
            rt.PushOverride(ov);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cool Pulse"))
        {
            PostProcess::PostProcessOverride ov;
            ov.priority = 1500.0f;
            ov.fadeIn = 0.3f; ov.hold = 1.5f; ov.fadeOut = 0.8f;
            snprintf(ov.label, sizeof(ov.label), "CoolPulse");
            auto& cg = ov.profile.colorGrading;
            cg.enabled.Set(true);
            cg.temperature.Set(-0.7f);
            cg.saturation.Set(0.6f);
            rt.PushOverride(ov);
        }
        ImGui::SameLine();
        if (ImGui::Button("Glass Shatter"))
        {
            if (m_renderer) m_renderer->TriggerGlassShatter(0.5f, 0.5f);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu active", rt.overrides.size());

        uint64_t pendingRemove = 0;
        for (const auto& o : rt.overrides)
        {
            const float total = o.fadeIn + (o.hold < 0.f ? 0.f : o.hold) + o.fadeOut;
            const float frac  = (total > 0.f) ? (o.elapsed / total) : 0.f;
            char line[128];
            snprintf(line, sizeof(line), "#%llu %s  t=%.2fs  p=%.0f",
                (unsigned long long)o.id, (o.label[0] ? o.label : "(unnamed)"),
                o.elapsed, o.priority);
            ImGui::ProgressBar(o.hold < 0.f ? 1.0f : frac, ImVec2(-1, 0), line);
            ImGui::PushID(static_cast<int>(o.id));
            if (ImGui::SmallButton("Remove")) pendingRemove = o.id;
            ImGui::PopID();
        }
        if (pendingRemove) rt.RemoveOverride(pendingRemove);
    }

    ImGui::End();
}

// ==== Animation timeline editor (dockable) ====
// Two tabs: "Clip Asset (.ianim)" (Unreal AnimSequence-style: edit AnimNotify tracks that live ON the
// clip; Save round-trips to disk) and "Entity Timeline" (per-entity TimelineComponent editing). The
// shared NotifyTrackEditor drives both; only its target differs.
void EditorLayer::RenderTimelinePanel()
{
    ImGui::SetNextWindowSize(ImVec2(960, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Timeline", &m_showTimeline)) { ImGui::End(); return; }

    // Drive the curve editor's read-only playback off our own clock; never let
    // it self-advance (the notify editor owns the preview clock).
    m_curveEditor.SetAnimationClipSystem(m_animClipSys);

    if (ImGui::BeginTabBar("##animTimelineTabs"))
    {
        if (ImGui::BeginTabItem("Clip Asset (.ianim)"))
        {
            RenderClipAssetTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Entity Timeline"))
        {
            RenderEntityTimelineTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ==== Clip-asset tab: edit notify tracks stored ON the .ianim clip ====
void EditorLayer::RenderClipAssetTab()
{
    // ---- Poll a pending async load ----
    if (m_animEditHandle.packed != 0 && !m_animEditReady && m_animClipSys
        && m_animClipSys->IsReady(m_animEditHandle))
    {
        m_animEditReady = true;
        m_animEditClipIdx = 0;
        m_animEditClipMirrored = -1;     // force a scratch refresh below
        if (const Resource::AnimationResource* res = m_animClipSys->GetResource(m_animEditHandle))
            m_curveEditor.LoadFromAnimationResource(*res);
    }

    // ---- Toolbar: path + load + save ----
    ImGui::SetNextItemWidth(360.f);
    ImGui::InputTextWithHint("##animpath", "asset/anim/clip.ianim",
                             m_animPathInput, sizeof(m_animPathInput));
    ImGui::SameLine();
    if (ImGui::Button("Load") && m_animPathInput[0])
        LoadAnimForEdit(m_animPathInput);
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        if (SaveAnimEdit()) m_animSaveStatus = "Saved " + m_animEditPath;
        else                m_animSaveStatus = "Save failed";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-serialize the loaded .ianim (bones/morphs untouched, notify tracks updated)");
    ImGui::SameLine();
    ImGui::TextDisabled("(drag a .ianim from the asset browser here)");

    // Window-wide drag-drop target for .ianim files.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
        {
            const char* path = static_cast<const char*>(p->Data);
            if (path && path[0]) LoadAnimForEdit(path);
        }
        ImGui::EndDragDropTarget();
    }

    if (!m_animSaveStatus.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1.f), "%s", m_animSaveStatus.c_str());
    }

    if (m_animEditHandle.packed == 0)
    {
        ImGui::Separator();
        ImGui::TextDisabled("No clip loaded. Drag a .ianim here or type a path and press Load.");
        return;
    }
    if (!m_animEditReady)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 1.f, 0.3f, 1.f), "Loading %s ...", m_animEditPath.c_str());
        return;
    }

    // Borrowed pointer, re-fetched every frame (never stored across frames, so
    // the "do NOT store" contract holds). Safe to hold through this function:
    // the editor runs single-threaded in the Render phase, after the frame's
    // AnimationClipSystem::Tick, and nothing here loads/unloads a resource that
    // could reallocate ResourceManager storage mid-call.
    Resource::AnimationResource* res =
        m_animClipSys ? m_animClipSys->GetResourceMutable(m_animEditHandle) : nullptr;
    if (!res || res->clips.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("Loaded resource has no bone clips.");
        return;
    }

    // ---- Clip selector (a resource can hold several clips) ----
    m_animEditClipIdx = std::clamp(m_animEditClipIdx, 0, (int)res->clips.size() - 1);
    if (res->clips.size() > 1)
    {
        if (ImGui::BeginCombo("Clip", res->clips[m_animEditClipIdx].name))
        {
            for (int i = 0; i < (int)res->clips.size(); ++i)
            {
                const bool sel = (i == m_animEditClipIdx);
                if (ImGui::Selectable(res->clips[i].name[0] ? res->clips[i].name : "(unnamed)", sel))
                {
                    // Do not reset m_animEditClipMirrored here: the sync block below detects the index change, flushes
                    // the OLD clip's edits back first, then re-points the scratch at the new clip. Resetting to -1
                    // would lose the old clip's identity and discard its unsaved edits.
                    m_animEditClipIdx = i;
                }
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        ImGui::Text("Clip: %s", res->clips[0].name[0] ? res->clips[0].name : "(unnamed)");
    }

    Resource::AnimClipData& clip = res->clips[m_animEditClipIdx];

    // ---- Sync scratch <-> clip ----
    // On (re)select, pull the clip's notify tracks into the scratch TimelineComponent the
    // NotifyTrackEditor edits. Re-pointing the editor's target resets its undo / selection state, which
    // is what we want per clip.
    if (m_animEditClipMirrored != m_animEditClipIdx)
    {
        // Flush the PREVIOUSLY-edited clip's scratch edits back first, or
        // switching clips would silently discard unsaved edits to the old one.
        if (m_animEditClipMirrored >= 0 &&
            m_animEditClipMirrored < (int)res->clips.size())
        {
            res->clips[m_animEditClipMirrored].notifyTracks = m_clipScratch.tracks;
            res->clips[m_animEditClipMirrored].nextNotifyId = m_clipScratch.nextNotifyId;
        }

        m_clipScratch.tracks       = clip.notifyTracks;
        m_clipScratch.clipDuration = clip.duration;
        m_clipScratch.nextNotifyId = clip.nextNotifyId;
        m_clipScratch.lastObservedTime = -1.f;
        m_animEditClipMirrored = m_animEditClipIdx;
        m_notifyTrackEditor.SetTarget(nullptr);             // force a clean re-arm
        m_notifyTrackEditor.SetTarget(&m_clipScratch);
    }

    // ---- Preview options ----
    ImGui::Checkbox("Preview on selected entity", &m_animPreviewOnEntity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While Play is active, drive the selected entity's animation\n"
                          "time and fire this clip's notifies through the real runtime path.");
    ImGui::SameLine();
    ImGui::Checkbox("Show bone curves", &m_animShowCurves);

    // Editing context: only bind to an entity when the user opted in. Resolve
    // the skeleton-owning entity (mesh children reference it via SkeletonRef);
    // AnimationComponent + SkeletonComponent live on that root.
    Entity previewEnt = NullEntity;
    if (m_animPreviewOnEntity && m_world && m_selectedEntity != NullEntity)
    {
        previewEnt = m_selectedEntity;
        if (auto* ref = m_world->GetComponent<SkeletonRef>(previewEnt))
            if (ref->entity != NullEntity) previewEnt = ref->entity;
    }
    // Re-arm the editor target every frame: the Entity-timeline tab may have pointed the shared
    // NotifyTrackEditor at a different TimelineComponent, so this reclaims the editor for the clip
    // scratch.
    m_notifyTrackEditor.SetTarget(&m_clipScratch);
    m_notifyTrackEditor.SetEditingEntity(previewEnt, m_world);
    m_notifyTrackEditor.SetOnEdited(nullptr);   // the clip is the asset's, saved by this tab

    // ---- The notify track editor (body only; we own the window) ----
    m_notifyTrackEditor.Render();

    // ---- Write scratch edits back into the clip asset (in memory) ----
    clip.notifyTracks = m_clipScratch.tracks;
    clip.nextNotifyId = m_clipScratch.nextNotifyId;

    // ---- Live preview: make the entity actually PLAY the edited clip ----
    // Bind this resource to the entity's skeleton (cached after the first call) to get the contiguous
    // ClipLibrary range, point primaryClip at the clip being edited (firstLib + local index), and stamp
    // the edited notify tracks onto that library clip. This guarantees previewed clip == edited clip
    // even for multi-clip resources, and TimelineSystem's clip-authoritative path then fires exactly
    // what the editor shows. Notifies are skeleton-independent, so the stamp is a straight copy.
    if (previewEnt != NullEntity && m_renderer && m_animClipSys)
    {
        auto* anim = m_world->GetComponent<AnimationComponent>(previewEnt);
        auto* skel = m_world->GetComponent<SkeletonComponent>(previewEnt);
        SkeletonRegistry& skelReg = m_renderer->GetSkeletonRegistry();
        if (anim && skel && skel->assetIndex != kInvalidAnimHandle
            && skel->assetIndex < skelReg.Count())
        {
            const SkeletonAsset& sk = skelReg.Get(skel->assetIndex);
            ClipLibrary& lib = m_renderer->GetClipLibrary();
            const uint32_t firstLib =
                m_animClipSys->BindToSkeleton(m_animEditHandle, sk, lib);
            if (firstLib != kInvalidClipIndex)
            {
                const uint32_t libIdx = firstLib + static_cast<uint32_t>(m_animEditClipIdx);
                if (libIdx < lib.Count())
                {
                    anim->primaryClip = libIdx;
                    lib.GetMutable(libIdx).notifyTracks = clip.notifyTracks;
                }
            }
        }
    }

    // ---- Optional read-only bone-curve overlay (Unreal-style combined view) ----
    if (m_animShowCurves)
    {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Bone Curves (read-only)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Mirror the notify editor's preview time onto the curve scrubber so
            // the two views stay aligned; never let the curve editor self-play.
            m_curveEditor.GetClip().isPlaying   = false;
            m_curveEditor.GetClip().currentTime = m_notifyTrackEditor.GetPreviewTime();
            ImGui::BeginChild("##curveOverlay", ImVec2(0, 240.f), true);
            m_curveEditor.Draw(0.f);
            ImGui::EndChild();
        }
    }
}

// ==== Entity-timeline tab: per-entity TimelineComponent editing ====
void EditorLayer::RenderEntityTimelineTab()
{
    TimelineComponent* tl = nullptr;
    if (m_world && m_selectedEntity != NullEntity) {
        tl = m_world->GetComponent<TimelineComponent>(m_selectedEntity);
        if (!tl && m_world->GetComponent<AnimationComponent>(m_selectedEntity)) {
            TimelineComponent fresh;
            fresh.clipDuration = 2.f;
            m_world->AddComponent<TimelineComponent>(m_selectedEntity, std::move(fresh));
            tl = m_world->GetComponent<TimelineComponent>(m_selectedEntity);
        }
    }

    ImGui::TextDisabled("Per-entity override tracks (additive to clip-authored notifies).");
    m_notifyTrackEditor.SetTarget(tl);
    m_notifyTrackEditor.SetEditingEntity(m_selectedEntity, m_world);
    // The tracks are the entity's TimelineComponent, saved with the scene. Their history is the
    // panel's own, so the scene's undo cannot take them back to the saved state.
    m_notifyTrackEditor.SetOnEdited([this] { MarkSceneDirty(); });
    m_notifyTrackEditor.Render();
}

// ==== LoadAnimForEdit: begin an async acquire of a .ianim for editing ====
void EditorLayer::LoadAnimForEdit(const std::string& path)
{
    if (!m_animClipSys) return;

    // Release any previously-held edit handle so its refcount drops.
    if (m_animEditHandle.packed != 0)
        m_animClipSys->ReleaseClip(m_animEditHandle);

    m_animEditPath    = path;
    m_animEditHandle  = m_animClipSys->AcquireClip(path);
    m_animEditReady   = false;
    m_animEditClipIdx = 0;
    m_animEditClipMirrored = -1;
    m_animSaveStatus.clear();
    // Keep the manual-path box in sync with drag-drop loads.
    std::snprintf(m_animPathInput, sizeof(m_animPathInput), "%s", path.c_str());

    // Ready immediately if cached.
    if (m_animClipSys->IsReady(m_animEditHandle))
    {
        m_animEditReady = true;
        if (const Resource::AnimationResource* res = m_animClipSys->GetResource(m_animEditHandle))
            m_curveEditor.LoadFromAnimationResource(*res);
    }
}

// ==== SaveAnimEdit: re-serialize the edited resource back to .ianim ====
bool EditorLayer::SaveAnimEdit()
{
    if (!m_animClipSys || m_animEditHandle.packed == 0 || m_animEditPath.empty())
        return false;
    Resource::AnimationResource* res = m_animClipSys->GetResourceMutable(m_animEditHandle);
    if (!res) return false;

    const std::vector<uint8_t> blob = Resource::SerializeAnimation(*res);
    if (blob.empty()) return false;

    std::ofstream out = Sys::OpenOfstream(m_animEditPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    return out.good();
}

// ==== Pool watermark panel: fixed-capacity pool usage vs its kMax* constant ====
// Its own window, not a Profiler tab: the Profiler's per-frame GPU timings are a live signal, these
// are monotonic high-water marks read as an accumulated record, and the reset button would look like
// it belonged to the timings.
// Every number is written by BuildRenderScene / SortAndEmit / Render (on main or the render-owner
// lane) and read here from OnUIRender on main while that lane is idle (the frame pipeline drains it
// before the UI build) - plain loads, no synchronisation. Do not call this from a RenderGraph Phase B
// worker.
// The point: each pool truncates SILENTLY on overflow (the object is not drawn, indistinguishable
// from a failed load or an over-aggressive cull); high water / capacity is the cheap way to see one
// coming.
void EditorLayer::RenderPoolWatermarkPanel()
{
    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Pool Watermarks", &m_showPoolWatermarks))
    {
        ImGui::End();
        return;
    }

    if (!m_renderer)
    {
        ImGui::TextDisabled("No renderer bound.");
        ImGui::End();
        return;
    }
    Renderer& r = *m_renderer;

    struct Row
    {
        const char* group;
        const char* name;
        uint32_t    current;      // this frame (0 when the pool has no live count)
        uint32_t    highWater;    // peak since start / since the last reset
        uint32_t    capacity;     // 0 = unbounded / not known here
        uint32_t    droppedLast;  // items lost in the last built frame
        uint32_t    overflowCount;// frames (or calls) that hit the cap
        const char* note;         // what breaks when this one runs out
    };

    std::vector<Row> rows;
    rows.reserve(24);

    // ---- Instances ----
    rows.push_back({ "Instances", "Instance emit budget",
        0, r.GetInstanceEmitHighWater(), r.GetInstanceCapacity(),
        r.GetInstanceDropLastFrame(), r.GetInstanceOverflowFrameCount(),
        "Dropped candidates produce NO draw packet. Drop order follows the "
        "batch sort key, so Custom-filter and NEAR transparent draws vanish "
        "first. Raise RendererPoolCaps::maxInstances." });
    rows.push_back({ "Instances", "Retained slots (static lane)",
        r.GetInstanceSlotCurrent(), r.GetInstanceSlotHighWater(),
        r.GetStaticInstanceLaneCapacity(),
        r.GetStaticLaneDeniedLastFrame(), 0,
        "Exhaustion is a PERFORMANCE cliff, not a visual one: denied entities "
        "still draw, but fall back to the per-frame dynamic segment and are "
        "re-staged every frame." });
    // Two quantities, two rows: this one is the dynamic segment (its reserve, 8192, bounds only the
    // dynamic slot range); staging records cover BOTH lanes and are bounded by the instance cap (32768
    // by default). Do not show staging against the dynamic reserve: a fresh load stages every retained
    // instance once and would read as overflow.
    rows.push_back({ "Instances", "Dynamic segment slots / frame",
        r.GetFrameStats().dynamicInstances, 0,
        Renderer::GetDynamicInstanceReserve(), 0, 0,
        "Instances that could not take a retained static slot (skinned, "
        "billboards, beams, afterimages, dissolving, untracked) and are "
        "rewritten wholesale each frame. Past the reserve they are DENIED a "
        "slot and the draw is dropped — a visual failure, not just a slow one." });
    rows.push_back({ "Instances", "Staging records / frame",
        r.GetInstanceStagingUsedLastFrame(), r.GetInstanceUploadRecordsPeak(),
        r.GetInstanceCapacity(), 0, 0,
        "GPUInstanceData records written to the upload buffer this frame, BOTH "
        "lanes. Spikes to the full instance count on a scene load (every record "
        "is fresh), then settles to the dynamic lane plus whatever changed." });

    // ---- Materials ----
    rows.push_back({ "Materials", "Material slot table",
        r.GetMaterialSlotCurrent(), r.GetMaterialSlotHighWater(),
        r.GetMaterialSlotCapacity(), 0, 0,
        "Past the cap a material silently reuses slot 0 — the object renders "
        "with the WRONG material rather than not at all." });
    rows.push_back({ "Materials", "Dirty-upload records / frame",
        0, r.GetMaterialUploadRecordsPeak(), 0, 0, 0,
        "Peak per-frame material copy records (no hard cap)." });
    rows.push_back({ "Materials", "Custom material SRV table",
        0, r.GetCustomMaterialTableHighWater(), 0, 0, 0,
        "Per-material custom texture table high-water." });
    rows.push_back({ "Materials", "Custom material CBV ring",
        0, r.GetCustomMaterialCbvHighWater(), 0, 0, 0,
        "Per-material custom constant ring high-water (bytes)." });
    rows.push_back({ "Materials", "Runtime material overrides (entities)",
        0, r.GetRuntimeMaterialEntityHighWater(), 0, 0, 0,
        "Entities holding a runtime material override." });
    rows.push_back({ "Materials", "Runtime material overrides (resources)",
        0, r.GetRuntimeMaterialResourceHighWater(), 0, 0, 0,
        "Distinct runtime override resources held." });

    // ---- Shadows ----
    rows.push_back({ "Shadows", "Spot shadow slices",
        0, r.GetSpotShadowCasterHighWater(), r.GetSpotShadowCasterCapacity(),
        r.GetSpotShadowCastersDropped(), 0,
        "Denied lights STILL LIGHT the scene, they just cast nothing. "
        "Raise SpotShadowPass::kMaxCasters." });
    rows.push_back({ "Shadows", "Point shadow cubes",
        0, r.GetPointShadowCasterHighWater(), r.GetPointShadowCasterCapacity(),
        r.GetPointShadowCastersDropped(), 0,
        "Denied lights STILL LIGHT the scene, they just cast nothing. "
        "Raise PointShadowPass::kMaxCasters." });

    // ---- Bindless mesh / buffer slots ----
    // One struct instead of twenty accessors (4 pools x 5 fields). bufferCurrent / meshCurrent do not
    // start at 0: the world allocator begins at the persistent reservation, so the reserved base is
    // subtracted before the number is labelled "world usage" (else an empty scene reads as a few hundred
    // leaked slots).
    const MeshDescriptorHeap::PoolStats ms =
        r.GetMeshManager().GetDescriptorHeap().GetPoolStats();
    {
        rows.push_back({ "Bindless", "Buffer slots (world)",
            ms.bufferCurrent - ms.bufferReserved,
            (ms.bufferHighWater > ms.bufferReserved)
                ? (ms.bufferHighWater - ms.bufferReserved) : 0u,
            ms.bufferCapacity - ms.bufferReserved,
            0, ms.bufferOverflows,
            "Exhaustion returns kInvalidBufferIndex and the mesh silently stops "
            "rendering. Raise MeshDescriptorHeap::kMaxBuffers." });
        rows.push_back({ "Bindless", "Mesh descriptors (world)",
            ms.meshCurrent - ms.meshReserved,
            (ms.meshHighWater > ms.meshReserved)
                ? (ms.meshHighWater - ms.meshReserved) : 0u,
            ms.meshCapacity - ms.meshReserved,
            0, ms.meshOverflows,
            "Exhaustion returns kInvalidBufferIndex and the mesh silently stops "
            "rendering. Raise MeshDescriptorHeap::kDefaultMaxMeshes." });
        rows.push_back({ "Bindless", "Buffer slots (persistent)",
            ms.persistentBufferCurrent, ms.persistentBufferCurrent,
            ms.persistentBufferCapacity, 0, ms.persistentBufferOverflows,
            "Engine-lifetime reservation, never recycled. Bump "
            "kPermanentBufferSlots." });
        rows.push_back({ "Bindless", "Mesh slots (persistent)",
            ms.persistentMeshCurrent, ms.persistentMeshCurrent,
            ms.persistentMeshCapacity, 0, ms.persistentMeshOverflows,
            "Engine-lifetime reservation, never recycled. Bump "
            "kPermanentMeshSlots." });
    }

    // ---- Device command-list pool ----
    // static_cast, not dynamic_cast: a DX12-only build, and RTTI is not relied on anywhere else in the
    // file.
    if (m_gfx)
    {
        auto* dx = static_cast<GraphicsDX12*>(m_gfx);
        rows.push_back({ "Device", "Command lists / frame",
            dx->GetCommandListCurrent(), dx->GetCommandListPeak(),
            GraphicsDX12::GetCommandListCapacity(), 0, 0,
            "Peak lists used in ONE frame (not the same as slots ever "
            "allocated). Overflow returns an invalid handle and the pass "
            "records nothing." });
    }

    // Worst first: the pool closest to its cap is the one anybody wants to see. Rows without a capacity
    // sort last (they carry no ratio).
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
    {
        const float ra = a.capacity ? float(a.highWater) / float(a.capacity) : -1.f;
        const float rb = b.capacity ? float(b.highWater) / float(b.capacity) : -1.f;
        return ra > rb;
    });

    if (ImGui::Button("Reset high-water"))
        r.ResetPoolWatermarks();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Nothing resets these on world load, so the first heavy scene of "
            "the session otherwise pins every reading for the rest of it. "
            "Press this after loading the scene you actually want to measure.");
    ImGui::SameLine();
    ImGui::TextDisabled("(peaks are since start or since the last reset)");
    // A big free list next to a high water pinned at the cap means recycling is not keeping up with
    // registration, not that the cap is too small.
    ImGui::TextDisabled(
        "Bindless free lists: %u buffer / %u mesh slots reusable now "
        "(kFrameCount-deferred buckets not counted).",
        ms.bufferFreeList, ms.meshFreeList);
    ImGui::Separator();

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##poolwatermarks", 7, kFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pool",     ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Current",  ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("High",     ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Capacity", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Dropped",  ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Overflows",ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("High / Cap", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();

        const char* lastGroup = nullptr;
        for (const Row& row : rows)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (!lastGroup || std::strcmp(lastGroup, row.group) != 0)
            {
                ImGui::TextDisabled("%s", row.group);
                ImGui::SameLine();
                lastGroup = row.group;
            }
            ImGui::TextUnformatted(row.name);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", row.note);

            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", row.current);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", row.highWater);

            ImGui::TableSetColumnIndex(3);
            if (row.capacity) ImGui::Text("%u", row.capacity);
            else              ImGui::TextDisabled("--");

            // Anything non-zero in these two columns means work was thrown away.
            ImGui::TableSetColumnIndex(4);
            if (row.droppedLast)
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%u", row.droppedLast);
            else
                ImGui::TextDisabled("0");

            ImGui::TableSetColumnIndex(5);
            if (row.overflowCount)
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%u", row.overflowCount);
            else
                ImGui::TextDisabled("0");

            ImGui::TableSetColumnIndex(6);
            if (row.capacity)
            {
                const float frac = std::min(1.f,
                    float(row.highWater) / float(row.capacity));
                ImVec4 bar(0.30f, 0.65f, 0.35f, 1.f);            // < 80%: fine
                if (frac >= 1.f)       bar = ImVec4(0.85f, 0.20f, 0.20f, 1.f);
                else if (frac >= 0.8f) bar = ImVec4(0.85f, 0.70f, 0.15f, 1.f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar);
                char label[32];
                std::snprintf(label, sizeof(label), "%.0f%%", frac * 100.f);
                ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.f), label);
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextDisabled("--");
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ==== Chain-physics (KawaiiPhysics-style) authoring overlay ====

// Returns the entity whose authored chains should be drawn this frame, or
// NullEntity. Shared guard for the prepare/emit phases.
static Entity ChainOverlayTarget(bool enabled, Entity selected, World& world)
{
    if (!enabled || selected == NullEntity || !world.IsAlive(selected))
        return NullEntity;
    // PrepareChainPhysicsOverlay forces the main DebugWirePass on for whatever this returns, so a STAGE
    // target would make the whole editor pay for the wire pass and draw the stage's bones at y=-4096.
    // The stage's overlay belongs to the preview panel.
    if (world.HasComponent<PreviewStageTag>(selected))
        return NullEntity;
    const ChainPhysicsComponent* comp = world.GetComponent<ChainPhysicsComponent>(selected);
    if (!comp || comp->groups.empty()) return NullEntity;
    return selected;
}

// The overlay (and the main-viewport pose tools) follow the hierarchy selection, gated by the
// Inspector's checkbox; the preview STAGE never qualifies (ChainOverlayTarget / RenderBonePoseGizmo
// refuse it).
Entity EditorLayer::ChainOverlayCandidate(World& world) const
{
    (void)world;
    return m_selectedEntity;
}

// ==== BuildBoneWorldMatrices: the one place bone positions come from ====
// Rebuild FK from the final LocalPose (including physics), with a bind-pose fallback when no pose is
// available. The solver's boneWorldMatCache is its PRE-simulation target pose, not its result;
// overlay, picker and gizmo must all use the same final pose or their handles disagree with the
// visible bones.
bool EditorLayer::BuildBoneWorldMatrices(Renderer& renderer, World& world, Entity e,
                                         std::vector<DirectX::XMFLOAT4X4>& outBoneToModel,
                                         DirectX::XMMATRIX& outEntWorld) const
{
    using namespace DirectX;

    outBoneToModel.clear();
    outEntWorld = XMMatrixIdentity();
    if (e == NullEntity || !world.IsAlive(e)) return false;

    const SkeletonComponent* skc = world.GetComponent<SkeletonComponent>(e);
    if (!skc || skc->assetIndex == kInvalidSkeletonIndex) return false;
    SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
    if (skc->assetIndex >= reg.Count()) return false;
    const SkeletonAsset& skel = reg.Get(skc->assetIndex);
    if (skel.boneCount == 0) return false;

    outBoneToModel.resize(skel.boneCount);

    // FK over the final LocalPose. Forward pass is valid
    // because parentIndex[b] < b, the same invariant the sim's cache build uses.
    // Bounded by SkeletonComponent::boneCount (what AnimationSystem sized the
    // pose cache to) so an asset swapped between Updates cannot over-read.
    uint32_t covered = 0;
    AnimationSystem* animSys = renderer.GetAnimationSystem();
    const AnimationSystem::LocalPose* poses =
        animSys ? animSys->GetLocalPose(e) : nullptr;
    if (poses)
    {
        covered = (std::min)(skel.boneCount, skc->boneCount);
        for (uint32_t b = 0; b < covered; ++b)
        {
            auto lp = poses[b];
            const int32_t par = skel.parentIndex[b];
            if (par < 0)
            {
                // Match LocalToWorldSystem's root translation policy used by
                // skinning; root rotation and scale still come from the pose.
                XMVECTOR scale, rotation, translation;
                XMMatrixDecompose(&scale, &rotation, &translation,
                                  XMLoadFloat4x4(&skel.restPoseLocal[b]));
                XMStoreFloat3(&lp.pos, translation);
            }
            XMMATRIX m = XMMatrixScaling(lp.scl.x, lp.scl.y, lp.scl.z)
                       * XMMatrixRotationQuaternion(XMLoadFloat4(&lp.rot))
                       * XMMatrixTranslation(lp.pos.x, lp.pos.y, lp.pos.z);
            if (par >= 0 && static_cast<uint32_t>(par) < b)
                m = m * XMLoadFloat4x4(&outBoneToModel[par]);
            XMStoreFloat4x4(&outBoneToModel[b], m);
        }
    }

    // Bind pose for the tail. Keep it: during the one frame an asset swap is in flight the leftover bones
    // would otherwise be read uninitialized.
    for (uint32_t b = covered; b < skel.boneCount; ++b)
        outBoneToModel[b] = skel.bindPose[b];

    if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e))
        outEntWorld = XMLoadFloat4x4(&gt->matrix);
    return true;
}

// ==== CollectChainBones: which bones the pose tools may touch ====
void EditorLayer::CollectChainBones(const SkeletonAsset& skel,
                                    const ChainPhysicsComponent& comp,
                                    int focusGroup,
                                    std::vector<uint8_t>& outPickable)
{
    // The body is Editor::ChainRestPose::CollectChainBones, so the Chain Physics Preview panel's
    // in-picture picker selects from the same set.
    Editor::ChainRestPose::CollectChainBones(skel, comp, focusGroup, outPickable);
}

// ==== PickChainBone: the screen-space nearest chain bone under the cursor ====
uint32_t EditorLayer::PickChainBone(Renderer& renderer, World& world, Entity e,
                                    float px, float py, float vpW, float vpH) const
{
    using namespace DirectX;
    if (vpW <= 0.f || vpH <= 0.f) return ~0u;

    const ChainPhysicsComponent* comp = world.GetComponent<ChainPhysicsComponent>(e);
    const SkeletonComponent*     skc  = world.GetComponent<SkeletonComponent>(e);
    if (!comp || !skc || skc->assetIndex == kInvalidSkeletonIndex) return ~0u;
    SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
    if (skc->assetIndex >= reg.Count()) return ~0u;
    const SkeletonAsset& skel = reg.Get(skc->assetIndex);

    std::vector<XMFLOAT4X4> boneToModel;
    XMMATRIX entWorld;
    if (!BuildBoneWorldMatrices(renderer, world, e, boneToModel, entWorld)) return ~0u;

    std::vector<uint8_t> pickable;
    CollectChainBones(skel, *comp, m_chainMainPose.focusGroup, pickable);

    // projMatrixNoJitter, matching what ImGuizmo is handed; the header says why Ray::FromViewport is
    // deliberately not used here.
    const XMMATRIX view = XMLoadFloat4x4(&m_renderView.viewMatrix);
    const XMMATRIX proj = XMLoadFloat4x4(&m_renderView.projMatrixNoJitter);
    const XMMATRIX vp   = XMMatrixMultiply(view, proj);

    constexpr float kPickRadiusPx = 12.f;
    uint32_t best     = ~0u;
    float    bestD2   = kPickRadiusPx * kPickRadiusPx;
    float    bestDepth = FLT_MAX;

    for (uint32_t b = 0; b < skel.boneCount && b < pickable.size(); ++b)
    {
        if (!pickable[b]) continue;
        const XMMATRIX bw = XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[b]), entWorld);
        const XMVECTOR wp = XMVectorSetW(bw.r[3], 1.f);
        const XMVECTOR cp = XMVector4Transform(wp, vp);
        const float w = XMVectorGetW(cp);
        if (w <= 0.f) continue;                       // behind the camera
        const float sx = (XMVectorGetX(cp) / w * 0.5f + 0.5f) * vpW;
        const float sy = (1.f - (XMVectorGetY(cp) / w * 0.5f + 0.5f)) * vpH;
        const float dx = sx - px, dy = sy - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 > bestD2) continue;
        // Nearest to the cursor wins; camera depth only breaks ties, so two
        // overlapping bones resolve to the one in front.
        if (d2 < bestD2 - 1e-3f || w < bestDepth)
        {
            bestD2    = d2;
            bestDepth = w;
            best      = b;
        }
    }
    return best;
}

BoneRestPose* EditorLayer::EnsureBoneRestPoseEntry(ChainPhysicsComponent& comp,
                                                   const SkeletonAsset& skel,
                                                   World& world, Entity e, uint32_t bone,
                                                   bool reseedExisting)
{
    using namespace DirectX;
    if (bone >= skel.boneCount) return nullptr;

    // Seed / re-seed value: the bone's current CLEAN base, so creating an entry
    // never moves anything. In Edit that is the LocalPose (animation / rest);
    // while the sim is RUNNING the LocalPose holds simulator output for exactly
    // these bones, so fall back to the modelled rest rather than baking a swing
    // into the authored pose.
    XMFLOAT4 seed{ 0.f, 0.f, 0.f, 1.f };
    bool seeded = false;
    ChainPhysicsSystem* cps = m_renderer ? m_renderer->GetChainPhysicsSystem() : nullptr;
    // Per entity: a preview-lane stage simulates in Edit mode.
    const bool simRunning = cps && cps->IsSimulating(e);
    if (!simRunning && m_renderer)
    {
        if (AnimationSystem* animSys = m_renderer->GetAnimationSystem())
            if (const AnimationSystem::LocalPose* poses = animSys->GetMutableLocalPose(e))
            {
                seed   = poses[bone].rot;
                seeded = true;
            }
    }
    if (!seeded)
    {
        XMVECTOR s, q, t;
        if (XMMatrixDecompose(&s, &q, &t, XMLoadFloat4x4(&skel.restPoseLocal[bone])))
            XMStoreFloat4(&seed, XMQuaternionNormalize(q));
    }
    (void)world;

    for (auto& r : comp.restPose)
        if (std::strcmp(r.boneName, skel.boneNames[bone]) == 0)
        {
            // "Capture current pose" has to overwrite, or it silently does
            // nothing for every bone already in the list.
            if (reseedExisting) r.rot = seed;
            return &r;
        }

    if (comp.restPose.size() >= static_cast<size_t>(ChainPhysicsComponent::MAX_REST_POSE_BONES))
        return nullptr;

    BoneRestPose r{};
    snprintf(r.boneName, sizeof(r.boneName), "%s", skel.boneNames[bone]);
    r.rot = seed;
    comp.restPose.push_back(r);
    return &comp.restPose.back();
}

// ==== RenderBonePoseGizmo: ROTATE handle on the selected chain bone ====
// Submitted from RenderViewportPanel immediately before RenderViewportGizmo and well before the
// "##viewport" InvisibleButton, because the patched ImGuizmo::CanActivate requires no hovered /
// active ImGui item.
void EditorLayer::RenderBonePoseGizmo(ImVec2 vpMin, ImVec2 vpMax)
{
    if (GetPlayState() != ViewportPlayState::Stopped) return;
    using namespace DirectX;
    if (!m_chainMainPose.poseMode || !m_world || !m_renderer) return;
    if (m_chainMainPose.poseBone == ~0u) return;

    const Entity e = ChainOverlayCandidate(*m_world);
    if (e == NullEntity || !m_world->IsAlive(e)) return;
    // The gizmo writes the LIVE component, and the preview panel is the only writer of its stage's live
    // component, so the gizmo never grabs a stage bone (at y=-4096 it is outside any sane main view
    // anyway; this makes it a rule).
    if (m_world->HasComponent<PreviewStageTag>(e)) return;

    ChainPhysicsComponent* comp = m_world->GetComponent<ChainPhysicsComponent>(e);
    const SkeletonComponent* skc = m_world->GetComponent<SkeletonComponent>(e);
    if (!comp || !skc || skc->assetIndex == kInvalidSkeletonIndex) return;
    SkeletonRegistry& reg = m_renderer->GetSkeletonRegistry();
    if (skc->assetIndex >= reg.Count()) return;
    const SkeletonAsset& skel = reg.Get(skc->assetIndex);
    if (m_chainMainPose.poseBone >= skel.boneCount) { m_chainMainPose.poseBone = ~0u; return; }

    std::vector<XMFLOAT4X4> boneToModel;
    XMMATRIX entWorld;
    if (!BuildBoneWorldMatrices(*m_renderer, *m_world, e, boneToModel, entWorld)) return;

    const XMMATRIX boneWorld =
        XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[m_chainMainPose.poseBone]), entWorld);

    // Frustum-cull, for the same reason as the entity gizmo: ImGuizmo otherwise pins handles to the
    // screen edge for off-screen bones.
    // Skipped while a drag is IN FLIGHT: returning mid-drag would stop calling Manipulate, so
    // HandleRotation never sees the mouse release and ImGuizmo's mbUsing latches true (the viewport then
    // behaves as if a gizmo were permanently held). A fast camera move, or the bone swinging off screen
    // under the rotation being dragged, is enough to trigger it.
    if (!ImGuizmo::IsUsingAny())
    {
        const XMMATRIX vp = XMMatrixMultiply(XMLoadFloat4x4(&m_renderView.viewMatrix),
                                             XMLoadFloat4x4(&m_renderView.projMatrixNoJitter));
        const XMVECTOR clip = XMVector4Transform(XMVectorSetW(boneWorld.r[3], 1.f), vp);
        const float w = XMVectorGetW(clip);
        if (w <= 0.f) return;
        const float ndcX = XMVectorGetX(clip) / w;
        const float ndcY = XMVectorGetY(clip) / w;
        constexpr float kMargin = 1.5f;
        if (ndcX < -kMargin || ndcX > kMargin || ndcY < -kMargin || ndcY > kMargin) return;
    }

    // Freeze the fed matrix while hovered / dragged: a bone matrix is re-derived from an animating pose
    // every frame and the hit test would flicker.
    if (m_chainMainPose.poseBone != m_bonePoseGizmoBone)
    {
        m_bonePoseGizmoBone = m_chainMainPose.poseBone;
        XMStoreFloat4x4(&m_bonePoseGizmoMatrix, boneWorld);
    }
    else if (!ImGuizmo::IsUsingAny() && !ImGuizmo::IsOver())
    {
        XMStoreFloat4x4(&m_bonePoseGizmoMatrix, boneWorld);
    }

    float worldMut[16];
    memcpy(worldMut, &m_bonePoseGizmoMatrix, sizeof(worldMut));

    // ALL FOUR of these must be set here. RenderViewportGizmo is the only other place that sets them and
    // it early-outs several ways before reaching them, so nothing can be inherited. ImGuizmo::BeginFrame
    // also resets mDrawList to its internal window every frame, and hover resolution dereferences it:
    // without SetAlternativeWindow the ring draws but HandleRotation early-outs on !mbMouseOver and it
    // can never be grabbed.
    ImGuizmo::SetOrthographic(m_editorViewMode != EditorViewMode::Perspective
                              && GetPlayState() == ViewportPlayState::Stopped);
    ImGuizmo::SetRect(vpMin.x, vpMin.y, vpMax.x - vpMin.x, vpMax.y - vpMin.y);
    // The Viewport window's own draw list - the gizmo layer rule (see
    // ViewportGizmoDrawList): a window above the viewport covers the ring.
    ImDrawList* gizmoDL = nullptr;
    {
        using namespace DirectX;
        const XMMATRIX vpm = XMMatrixMultiply(XMLoadFloat4x4(&m_renderView.viewMatrix),
                                              XMLoadFloat4x4(&m_renderView.projMatrixNoJitter));
        const XMVECTOR clip = XMVector4Transform(XMVectorSetW(boneWorld.r[3], 1.f), vpm);
        const float cw = XMVectorGetW(clip);
        ImVec2 c(0.f, 0.f);
        if (cw > 0.f)
            c = ImVec2(vpMin.x + (XMVectorGetX(clip) / cw * 0.5f + 0.5f) * (vpMax.x - vpMin.x),
                       vpMin.y + (0.5f - XMVectorGetY(clip) / cw * 0.5f) * (vpMax.y - vpMin.y));
        gizmoDL = ViewportGizmoDrawList("restpose", c);
        ImGuizmo::SetDrawlist(gizmoDL);
    }
    ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());

    // Distinct ID scope so this gizmo's state never aliases the entity gizmo's.
    ImGuizmo::PushID(0x50534531);

    float snapVal = m_snapRotate;
    const float* snap = (m_snapEnabled != ImGui::GetIO().KeyCtrl) ? &snapVal : nullptr;

    float deltaMut[16];
    m_gizmoSubmittedThisFrame = true;   // ImGuizmo's hit-test state is now current
    // Clipped to the scene image (not the toolbar row above it).
    gizmoDL->PushClipRect(vpMin, vpMax, true);
    const bool manipulated = ImGuizmo::Manipulate(
        reinterpret_cast<const float*>(&m_renderView.viewMatrix),
        reinterpret_cast<const float*>(&m_renderView.projMatrixNoJitter),
        ImGuizmo::ROTATE, m_gizmoMode, worldMut, deltaMut, snap);
    gizmoDL->PopClipRect();

    ImGuizmo::PopID();
    // -chainpreview_main_pose: the ring was submitted this frame, for this scene entity.
    ++m_bonePoseGizmoSubmits;
    m_bonePoseGizmoLastEntity = e;

    if (!manipulated) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;   // hover-only activation

    BoneRestPose* entry = EnsureBoneRestPoseEntry(*comp, skel, *m_world, e, m_chainMainPose.poseBone);
    if (!entry) return;

    // Parent space for the delta: the parent bone's world matrix (the entity's for a root bone). The
    // delta math is Editor::ChainRestPose::ApplyWorldDelta, shared with the Chain Physics Preview
    // panel's in-picture gizmo (translation dropped, determinant checked, orthonormalized, parent-frame
    // order, hemisphere kept).
    const int32_t par = skel.parentIndex[m_chainMainPose.poseBone];
    const XMMATRIX parentWorld = (par >= 0 && static_cast<uint32_t>(par) < skel.boneCount)
        ? XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[par]), entWorld)
        : entWorld;
    XMFLOAT4X4 parentStore, deltaStore;
    XMStoreFloat4x4(&parentStore, parentWorld);
    memcpy(&deltaStore, deltaMut, sizeof(deltaStore));
    XMFLOAT4 newRot;
    if (!Editor::ChainRestPose::ApplyWorldDelta(entry->rot, parentStore, deltaStore,
                                                Editor::ChainRestPose::DeltaOrder::ParentFrame,
                                                newRot))
        return;   // mirrored instance: a reflected frame has no rotation-only delta
    entry->rot = newRot;
    ++m_bonePoseGizmoEdits;   // -chainpreview_main_pose read-out

    // Keep the frozen matrix in step with the drag so IsOver() does not oscillate.
    memcpy(&m_bonePoseGizmoMatrix, worldMut, sizeof(m_bonePoseGizmoMatrix));
}

// ==== UpdateBonePoseEditTracking: one drag = one undo entry ====
void EditorLayer::UpdateBonePoseEditTracking()
{
    if (!m_world) { m_bonePoseRecording = false; m_bonePoseBaseline.valid = false; return; }

    const Entity e = ChainOverlayCandidate(*m_world);
    ChainPhysicsComponent* comp =
        (e != NullEntity && m_world->IsAlive(e))
            ? m_world->GetComponent<ChainPhysicsComponent>(e) : nullptr;

    // ---- Drop the selected bone whenever it stops meaning anything ----
    // A bone INDEX is valid only for one (entity, skeleton asset) pair; a stale one would read out of
    // range or pose the wrong bone.
    {
        // The state belongs to ONE entity: a new selection starts without a picked bone (the Inspector's arm
        // switch survives), whether or not any window is open.
        if (m_chainMainPose.owner != e)
        {
            m_chainMainPose.owner     = e;
            m_chainMainPose.poseBone  = ~0u;
            m_chainMainPose.skelAsset = 0xFFFFFFFFu;
            // A group index belongs to one character too: kept, it would restrict the picker / overlay to an
            // unrelated group (PoseStateFor never resets: owner == entity).
            m_chainMainPose.focusGroup = -1;
            m_bonePoseGizmoBone       = ~0u;
        }
        bool keep = m_chainMainPose.poseMode && m_chainMainPose.poseBone != ~0u
                 && comp != nullptr;
        uint32_t skelAsset = 0xFFFFFFFFu;
        if (keep)
        {
            const SkeletonComponent* skc = m_world->GetComponent<SkeletonComponent>(e);
            keep = skc && skc->assetIndex != kInvalidSkeletonIndex
                       && m_renderer
                       && skc->assetIndex < m_renderer->GetSkeletonRegistry().Count()
                       && m_chainMainPose.poseBone <
                          m_renderer->GetSkeletonRegistry().Get(skc->assetIndex).boneCount;
            if (keep)
            {
                skelAsset = skc->assetIndex;
                if (m_chainMainPose.skelAsset != 0xFFFFFFFFu &&
                    m_chainMainPose.skelAsset != skelAsset) keep = false;
            }
        }
        if (!keep)
        {
            m_chainMainPose.poseBone      = ~0u;
            m_bonePoseGizmoBone  = ~0u;
            m_chainMainPose.skelAsset = 0xFFFFFFFFu;
        }
        else
        {
            m_chainMainPose.skelAsset = skelAsset;
        }
    }

    // ImGuizmo::IsUsing() is ID-SCOPED (it compares GetCurrentID() to mEditingID, and an empty ID stack
    // reads -1). RenderBonePoseGizmo pushes an ID, so IsUsing() out here, outside any PushID scope, is
    // ALWAYS false and neither edge would fire. IsUsingAny() is scope-independent but also true for an
    // entity-transform drag, hence the mode / bone gate.
    // The reverse direction needs nothing: UpdateTransformEditTracking uses the root-scope IsUsing(),
    // which is false while our PushID scope owns the drag. Do not "fix" that one to IsUsingAny() for
    // symmetry: it would record a phantom transform edit on every bone drag.
    const bool manipulating = m_chainMainPose.poseMode
                           && m_chainMainPose.poseBone != ~0u
                           && comp != nullptr
                           && ImGuizmo::IsUsingAny();

    if (manipulating)
    {
        if (!m_bonePoseRecording && m_bonePoseBaseline.valid)
        {
            m_bonePoseRecording    = true;
            m_bonePoseRecordEntity = m_world->MakeHandle(e);
            m_bonePoseBefore       = m_bonePoseBaseline;   // last idle frame = pre-drag
        }
        return;   // hold the baseline frozen for the duration of the drag
    }

    if (m_bonePoseRecording)
    {
        CommitBonePoseEdit();
        m_bonePoseRecording = false;
    }

    // Refresh the baseline from restPose only: the registered captureValue would deep-copy the whole
    // component (up to 64 ~1 KB ChainGroupDefs) every idle frame, ~85 KB/frame on real assets.
    if (comp)
    {
        m_bonePoseBaseline.pose    = comp->restPose;
        m_bonePoseBaseline.enabled = comp->restPoseEnabled;
        m_bonePoseBaseline.weight  = comp->restPoseWeight;
        m_bonePoseBaseline.valid   = true;
    }
    else
    {
        m_bonePoseBaseline.valid = false;
    }
}

bool EditorLayer::SaveBlobWithDialog(const char* filter, const char* defaultExt,
                                     const char* initDir,
                                     const std::vector<uint8_t>& blob,
                                     std::string* outPath)
{
    if (blob.empty()) return false;
    const std::string path = SaveFileDialog(filter, defaultExt, initDir);
    if (path.empty()) return false;                 // cancelled
    if (outPath) *outPath = path;
    return WriteFileFully(path, blob);
}

EditorLayer::BonePoseBaseline EditorLayer::CaptureBoneRestPose(Entity e) const
{
    BonePoseBaseline b;
    if (!m_world || e == NullEntity || !m_world->IsAlive(e)) return b;
    if (const ChainPhysicsComponent* c = m_world->GetComponent<ChainPhysicsComponent>(e))
    {
        b.pose    = c->restPose;
        b.enabled = c->restPoseEnabled;
        b.weight  = c->restPoseWeight;
        b.valid   = true;
    }
    return b;
}

void EditorLayer::PushBoneRestPoseEdit(Entity e, const BonePoseBaseline& before,
                                        const char* label)
{
    if (!m_world || !UndoEnabled() || !before.valid) return;
    if (e == NullEntity || !m_world->IsAlive(e)) return;

    const ChainPhysicsComponent* c = m_world->GetComponent<ChainPhysicsComponent>(e);
    if (!c) return;

    // Field-by-field, because the generic path is unavailable here:
    // ChainPhysicsComponent is not trivially copyable so ComponentEditorEntry's
    // `fingerprint` is an empty std::function, and two RestoreFn closures cannot
    // be compared. Without this a click that grabbed the ring and let go without
    // moving would push an empty undo entry and dirty the scene.
    bool changed = (before.enabled != c->restPoseEnabled)
                || (before.weight  != c->restPoseWeight)
                || (before.pose.size() != c->restPose.size());
    for (size_t i = 0; !changed && i < before.pose.size(); ++i)
    {
        const BoneRestPose& a = before.pose[i];
        const BoneRestPose& b = c->restPose[i];
        changed = std::strcmp(a.boneName, b.boneName) != 0
               || a.rot.x != b.rot.x || a.rot.y != b.rot.y
               || a.rot.z != b.rot.z || a.rot.w != b.rot.w
               || a.weight != b.weight || a.enabled != b.enabled;
    }
    if (!changed) return;

    Renderer* renderer = m_renderer;
    auto makeRestore = [renderer](std::shared_ptr<BonePoseBaseline> snap)
        -> Editor::Undo::RestoreFn
    {
        return [snap, renderer](World& w, Entity e2)
        {
            ChainPhysicsComponent* c2 = w.GetComponent<ChainPhysicsComponent>(e2);
            if (!c2) return;
            c2->restPose        = snap->pose;
            c2->restPoseEnabled = snap->enabled;
            c2->restPoseWeight  = snap->weight;
            // Restoring the DATA is not enough: for a Cloth group the authored
            // rotation changes the cross-strand ring/shear rest lengths that
            // InitEntity baked, so the sim must re-init or the piece fights its
            // own rest shape. Same reason the registered ChainPhysics capture
            // calls Invalidate.
            if (renderer)
                if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
                    cps->Invalidate(e2);
        };
    };

    auto beforeSnap = std::make_shared<BonePoseBaseline>(before);
    auto afterSnap  = std::make_shared<BonePoseBaseline>(CaptureBoneRestPose(e));

    // PushEdit, never m_editHistory.Push directly: PushEdit is the single funnel that also
    // MarkSceneDirty()s.
    PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(
        m_world->MakeHandle(e),
        makeRestore(beforeSnap), makeRestore(afterSnap),
        label ? label : "Bone Rest Pose"));
}

void EditorLayer::CommitBonePoseEdit()
{
    if (!m_world || !UndoEnabled()) return;
    if (!m_world->IsHandleValid(m_bonePoseRecordEntity)) return;
    PushBoneRestPoseEdit(m_bonePoseRecordEntity.entity, m_bonePoseBefore,
                         "Bone Rest Pose");
}

// ==== Skeleton overlay: the main viewport's Show > Skeleton ====
// The selection's skeleton in its live pose, and its sockets.
Entity EditorLayer::SkeletonOverlayTarget(World& world) const
{
    if (!m_showSelectionSkeleton) return NullEntity;
    const Entity sel = SelectableEntity(m_selectedEntity);   // a StaticMesh section -> its owner
    if (sel == NullEntity || !world.IsAlive(sel) || world.HasComponent<PreviewStageTag>(sel)) return NullEntity;
    // An Outliner folder is not a character: the search below would show whichever skeleton it met first
    // inside it.
    if (world.HasComponent<EditorFolderComponent>(sel)) return NullEntity;
    Entity e = sel;
    if (const SkeletonRef* ref = world.GetComponent<SkeletonRef>(sel))
        if (ref->entity != NullEntity && world.IsAlive(ref->entity)) e = ref->entity;
    e = Anim::ResolveAnimEntityOrSelf(world, e);
    if (world.HasComponent<SkeletonComponent>(e)) return e;
    // No skeleton: the selection's own sockets (a StaticMesh owner's mesh sockets).
    if (const SocketComponent* sc = world.GetComponent<SocketComponent>(sel); sc && sc->count > 0) return sel;
    return NullEntity;
}

void EditorLayer::PrepareSkeletonOverlay(Renderer& renderer, World& world)
{
    // As PrepareChainPhysicsOverlay: the wire pass on before BeginFrame's Clear().
    // The target is kept for this frame's Emit (only BeginFrame runs between them):
    // resolving it walks the selection's subtree when nothing animated is on it.
    m_skelOverlayTarget   = SkeletonOverlayTarget(world);
    m_skelOverlayPrepared = true;
    if (m_skelOverlayTarget == NullEntity) return;
    if (DebugWirePass* dbg = renderer.GetDebugWirePass()) dbg->enabled = true;
}

void EditorLayer::EmitSkeletonOverlay(Renderer& renderer, World& world)
{
    using namespace DirectX;
    SkeletonOverlayStats st;
    st.frame = m_editorFrame;
    Entity e = m_skelOverlayPrepared ? m_skelOverlayTarget : SkeletonOverlayTarget(world);
    m_skelOverlayPrepared = false;
    if (e != NullEntity && !world.IsAlive(e)) e = NullEntity;
    DebugWirePass* dbg = renderer.GetDebugWirePass();
    if (e != NullEntity && dbg)
    {
        // Colours are 0xAARRGGBB (the chain overlay's).
        constexpr uint32_t kBone = 0xFF9FD4FFu, kVirtual = 0xFFC88CFFu;
        constexpr uint32_t kAxis[3] = { 0xFFFF5050u, 0xFF50FF50u, 0xFF5090FFu };
        st.entity  = e;
        float arm  = 0.05f;   // a socket tripod's arm: 5 cm, or 4 % of the skeleton's extent
        const SkeletonComponent* skc = world.GetComponent<SkeletonComponent>(e);
        SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
        if (skc && skc->assetIndex != kInvalidSkeletonIndex && skc->assetIndex < reg.Count())
        {
            const SkeletonAsset& skel = reg.Get(skc->assetIndex);
            std::vector<XMFLOAT4X4> boneToModel;
            XMMATRIX entWorld;
            if (BuildBoneWorldMatrices(renderer, world, e, boneToModel, entWorld))
            {
                const uint32_t n = std::min<uint32_t>(skel.boneCount, static_cast<uint32_t>(boneToModel.size()));
                std::vector<XMFLOAT3> p(n);
                XMVECTOR lo = XMVectorReplicate(FLT_MAX), hi = XMVectorReplicate(-FLT_MAX);
                for (uint32_t b = 0; b < n; ++b)
                {
                    const XMVECTOR at = XMVector3TransformCoord(
                        XMVectorZero(), XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[b]), entWorld));
                    XMStoreFloat3(&p[b], at);
                    lo = XMVectorMin(lo, at);
                    hi = XMVectorMax(hi, at);
                }
                const uint32_t real = skel.RealBoneCount();
                for (uint32_t b = 0; b < n; ++b)
                {
                    const int32_t parent = skel.parentIndex[b];
                    if (parent < 0 || static_cast<uint32_t>(parent) >= n || static_cast<uint32_t>(parent) == b) continue;
                    dbg->AddLineDepthTested(p[parent], p[b], b >= real ? kVirtual : kBone);   // virtual bone: violet
                    ++st.lines;
                }
                st.bones = n;
                if (n > 1) arm = std::max(0.01f, 0.04f * XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))));
            }
        }
        // The sockets: SocketSystem's world this frame, an axis tripod each.
        if (const SocketComponent* sc = world.GetComponent<SocketComponent>(e))
            for (uint32_t i = 0; i < sc->count; ++i)
            {
                const SocketComponent::Socket& s = sc->sockets[i];
                if (s.flags & SocketComponent::kBoneMissing) continue;   // not placed
                const XMFLOAT4X4& m = s.worldTransform;
                const XMFLOAT3 o = { m._41, m._42, m._43 };
                const float rows[3][3] = { { m._11, m._12, m._13 }, { m._21, m._22, m._23 }, { m._31, m._32, m._33 } };
                for (int a = 0; a < 3; ++a)
                {
                    const float len = std::sqrt(rows[a][0] * rows[a][0] + rows[a][1] * rows[a][1] + rows[a][2] * rows[a][2]);
                    if (len < 1e-8f) continue;
                    const float k = arm / len;
                    dbg->AddLineDepthTested(o, { o.x + rows[a][0] * k, o.y + rows[a][1] * k, o.z + rows[a][2] * k }, kAxis[a]);
                    ++st.lines;
                }
                ++st.sockets;
            }
    }
    m_skelOverlay = st;
    if (st.entity != m_skelOverlayLogged.entity || st.bones != m_skelOverlayLogged.bones ||
        st.lines != m_skelOverlayLogged.lines || st.sockets != m_skelOverlayLogged.sockets)
    {
        if (st.entity != NullEntity)
            LOG_INFO("[SKELOVERLAY] entity=%u bones=%u lines=%u sockets=%u", st.entity, st.bones, st.lines, st.sockets);
        m_skelOverlayLogged = st;
    }
}

void EditorLayer::PrepareChainPhysicsOverlay(Renderer& renderer, World& world)
{
    if (ChainOverlayTarget(m_chainOverlayEnabled, ChainOverlayCandidate(world), world) == NullEntity)
        return;
    // Force the wire pass on for this frame so BuildScene_DebugWireframes runs
    // Clear() (which caches the live ring-slot write pointer). ApplyTo ran just
    // before us and may have set enabled=false when the debug master is off;
    // with master off every show* gather flag is also false, so this reveals
    // nothing except our own overlay lines pushed in EmitChainPhysicsOverlay.
    if (DebugWirePass* dbg = renderer.GetDebugWirePass())
        dbg->enabled = true;
}

void EditorLayer::EmitChainPhysicsOverlay(Renderer& renderer, World& world)
{
    using namespace DirectX;

    const Entity e = ChainOverlayTarget(m_chainOverlayEnabled, ChainOverlayCandidate(world), world);
    if (e == NullEntity) return;

    const ChainPhysicsComponent* comp = world.GetComponent<ChainPhysicsComponent>(e);
    const SkeletonComponent* skc = world.GetComponent<SkeletonComponent>(e);
    if (!comp || !skc || skc->assetIndex == kInvalidSkeletonIndex) return;

    SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
    if (skc->assetIndex >= reg.Count()) return;
    const SkeletonAsset& skel = reg.Get(skc->assetIndex);
    if (skel.boneCount == 0) return;

    DebugWirePass* dbg = renderer.GetDebugWirePass();
    if (!dbg) return;

    // Bone positions come from the shared final-pose helper, so the overlay, the bone picker and the bone
    // gizmo can never disagree about where a bone is. The matrices are MODEL space (boneWorldMatCache is
    // misnamed - the sim never reads GlobalTransform), so entWorld must be applied here, or the whole
    // overlay sits at the origin - which looks right in a test scene with the character at (0,0,0).
    std::vector<XMFLOAT4X4> boneToModel;
    XMMATRIX entWorld;
    if (!BuildBoneWorldMatrices(renderer, world, e, boneToModel, entWorld)) return;

    auto bonePos = [&](uint32_t b) -> XMFLOAT3 {
        XMMATRIX bw = XMLoadFloat4x4(&boneToModel[b]);   // bone -> model space
        XMVECTOR p  = XMVector3TransformCoord(XMVectorZero(), XMMatrixMultiply(bw, entWorld));
        XMFLOAT3 o; XMStoreFloat3(&o, p); return o;
    };
    auto resolveBone = [&](const char* nm) -> uint32_t {
        if (!nm || !nm[0]) return ~0u;
        for (uint32_t b = 0; b < skel.boneCount; ++b)
            if (std::strcmp(skel.boneNames[b], nm) == 0) return b;
        return ~0u;
    };

    // Colours are 0xAARRGGBB (the debug-wire convention). Three palettes: normal (no group focused),
    // dimmed (a non-focused group while another is expanded), and highlight (the expanded group: a vivid
    // orange, distinct from the cyan default).
    constexpr uint32_t kRoot  = 0xFFFFD000u, kChain  = 0xFF00E0FFu, kLink  = 0xFF40FF40u, kExcl  = 0xFFFF3030u;
    constexpr uint32_t kRootD = 0xFF6A5A00u, kChainD = 0xFF0A5A6Au, kLinkD = 0xFF1E5A1Eu, kExclD = 0xFF6A1A1Au;
    constexpr uint32_t kRootH = 0xFFFFFFFFu, kChainH = 0xFFFF7A00u, kLinkH = 0xFFFFB000u, kExclH = 0xFFFF3030u;

    const int hi = m_chainHighlightGroup;

    // ---- Pose-authoring markers ----
    // Drawn first so the chain geometry below overdraws nothing important, and independent of `hi` (-1 in
    // Edit mode; the pose tools follow m_chainMainPose.focusGroup).
    if (m_chainMainPose.poseMode)
    {
        constexpr uint32_t kAuthored = 0xFFFF40FFu;  // magenta: has an authored rotation
        constexpr uint32_t kSelected = 0xFFFFFFFFu;  // white: the bone the gizmo owns
        std::vector<uint8_t> pickable;
        CollectChainBones(skel, *comp, m_chainMainPose.focusGroup, pickable);
        for (uint32_t b = 0; b < skel.boneCount && b < pickable.size(); ++b)
        {
            if (!pickable[b]) continue;
            bool authored = false;
            for (const BoneRestPose& r : comp->restPose)
                if (r.enabled && std::strcmp(r.boneName, skel.boneNames[b]) == 0)
                { authored = true; break; }
            if (authored) dbg->AddSphere(bonePos(b), 0.035f, kAuthored, 10);
        }
        if (m_chainMainPose.poseBone < skel.boneCount)
            dbg->AddSphere(bonePos(m_chainMainPose.poseBone), 0.07f, kSelected, 14);
    }

    // While posing with a focus group selected, draw ONLY that group. Every other
    // chain is noise you cannot click anyway (the picker is already restricted to
    // the focus group), and on a 20-chain character the rest of the skeleton's
    // spheres bury the one chain being authored. Focus = "All groups" keeps the
    // full overlay, and so does the normal (non-posing) overlay.
    const bool poseFocusOnly = m_chainMainPose.poseMode && m_chainMainPose.focusGroup >= 0
                            && m_chainMainPose.focusGroup < static_cast<int>(comp->groups.size());

    for (int gi = 0; gi < static_cast<int>(comp->groups.size()); ++gi)
    {
        if (poseFocusOnly && gi != m_chainMainPose.focusGroup) continue;

        const ChainGroupDef& g = comp->groups[gi];
        if (!g.enabled) continue;
        const uint32_t root = resolveBone(g.rootBone);
        if (root >= skel.boneCount) continue;

        // m_chainHighlightGroup stays -1 in Edit mode (only the Play-mode LIVE tree, drawn for the overlay's
        // own target, sets it), so treat the pose focus group as the highlighted one - otherwise the single
        // chain drawn comes out in the muted default palette and its rest-limit cone never shows.
        const bool isHi = (gi == hi) || poseFocusOnly;
        const bool dim  = (hi >= 0 && !isHi);
        const uint32_t cRoot  = isHi ? kRootH  : (dim ? kRootD  : kRoot);
        const uint32_t cChain = isHi ? kChainH : (dim ? kChainD : kChain);
        const uint32_t cLink  = isHi ? kLinkH  : (dim ? kLinkD  : kLink);
        const uint32_t cExcl  = isHi ? kExclH  : (dim ? kExclD  : kExcl);
        const float rootR  = isHi ? 0.06f  : (dim ? 0.03f  : 0.04f);
        const float chainR = isHi ? 0.03f  : (dim ? 0.015f : 0.02f);
        const float crossR = isHi ? 0.045f : 0.03f;

        dbg->AddSphere(bonePos(root), rootR, cRoot, 12);

        if (g.type == ChainGroupType::Spring) continue; // a single jiggle bone, no chain

        // Mark subtree + excludes - mirrors InitEntity so the overlay matches the sim.
        std::vector<uint8_t> mk(skel.boneCount, 0), ex(skel.boneCount, 0);
        mk[root] = 1;
        for (uint32_t b = root + 1; b < skel.boneCount; ++b)
            if (skel.parentIndex[b] >= 0 && mk[skel.parentIndex[b]]) mk[b] = 1;
        for (int j = 0; j < g.excludeCount; ++j) {
            uint32_t exb = resolveBone(g.excludeBones[j]);
            if (exb < skel.boneCount) ex[exb] = 1;
        }
        if (g.excludeSubtree)
            for (uint32_t b = 0; b < skel.boneCount; ++b)
                if (skel.parentIndex[b] >= 0 && ex[skel.parentIndex[b]]) ex[b] = 1;

        for (uint32_t b = 0; b < skel.boneCount; ++b)
        {
            if (!mk[b] || b == root) continue;
            const XMFLOAT3 p = bonePos(b);
            if (ex[b]) {
                dbg->AddCross(p, crossR, cExcl);
            } else {
                dbg->AddSphere(p, chainR, cChain, 8);
                const int32_t par = skel.parentIndex[b];
                if (par >= 0 && mk[par] && !ex[par])
                    dbg->AddLine(bonePos(static_cast<uint32_t>(par)), p, cLink);
            }
        }

        // ---- Rest-limit cone (expanded group only) ----
        // Draws the bound ChainPhysicsSystem's rest-limit clamp enforces, so the angle can be dialled in
        // against the actual silhouette. Only the focused group is drawn: a whole character's cones would
        // bury the mesh.
        // bonePos() reads BuildBoneWorldMatrices' final pose: in Edit that is the animated rest pose the clamp
        // measures against; in Play it includes the simulation, so the cone axes follow the simulated bones.
        const float limDeg = g.ovrEnabled
            ? g.ovrLimitAngle
            : (g.type == ChainGroupType::Cloth ? comp->skirtLimitAngle : comp->limitAngle);
        if (!isHi || limDeg <= 0.f || limDeg >= 180.f) continue;
        const float tipScale = (g.ovrEnabled
            ? g.ovrLimitTipScale
            : (g.type == ChainGroupType::Cloth ? comp->skirtLimitTipScale
                                               : comp->limitTipScale));

        // Each bone's depth from the group root, and the deepest simulated leaf below it. The sim
        // parameterises the cone by position along ONE strand; a forked subtree has several, so the longest
        // branch through a bone is used here. Exact for unforked chains, within a segment otherwise.
        std::vector<int> depth(skel.boneCount, 0), leafDepth(skel.boneCount, 0);
        for (uint32_t b = root + 1; b < skel.boneCount; ++b)
            if (mk[b] && !ex[b] && skel.parentIndex[b] >= 0)
                depth[b] = depth[skel.parentIndex[b]] + 1;
        for (uint32_t b = 0; b < skel.boneCount; ++b) leafDepth[b] = depth[b];
        for (uint32_t b = skel.boneCount; b-- > root + 1; )
        {
            if (!mk[b] || ex[b] || skel.parentIndex[b] < 0) continue;
            const uint32_t par = static_cast<uint32_t>(skel.parentIndex[b]);
            if (leafDepth[b] > leafDepth[par]) leafDepth[par] = leafDepth[b];
        }

        constexpr uint32_t kCone = 0xFFB060FFu;
        for (uint32_t c = root + 1; c < skel.boneCount; ++c)
        {
            if (!mk[c] || ex[c] || skel.parentIndex[c] < 0) continue;
            const uint32_t par = static_cast<uint32_t>(skel.parentIndex[c]);
            if (!mk[par] || ex[par]) continue;

            const XMFLOAT3 ap = bonePos(par);
            const XMFLOAT3 cp = bonePos(c);
            XMVECTOR axis = XMVectorSet(cp.x - ap.x, cp.y - ap.y, cp.z - ap.z, 0.f);
            const float L = XMVectorGetX(XMVector3Length(axis));
            if (L < 1e-5f) continue;
            axis = XMVectorScale(axis, 1.f / L);

            const float t = (leafDepth[c] > 0)
                ? static_cast<float>(depth[c]) / static_cast<float>(leafDepth[c]) : 0.f;
            float half = limDeg * (1.f + t * (tipScale - 1.f));
            if (half <= 0.f) continue;
            if (half > 179.f) half = 179.f;

            float sinH, cosH;
            XMScalarSinCos(&sinH, &cosH, half * 0.01745329252f);

            // Orthonormal basis around the axis.
            const XMVECTOR ref = (std::fabsf(XMVectorGetY(axis)) < 0.99f)
                ? XMVectorSet(0.f, 1.f, 0.f, 0.f) : XMVectorSet(1.f, 0.f, 0.f, 0.f);
            XMVECTOR u = XMVector3Cross(axis, ref);
            if (XMVectorGetX(XMVector3LengthSq(u)) < 1e-10f) continue;
            u = XMVector3Normalize(u);
            const XMVECTOR v = XMVector3Cross(axis, u);

            const XMVECTOR apex   = XMVectorSet(ap.x, ap.y, ap.z, 0.f);
            const XMVECTOR center = XMVectorAdd(apex, XMVectorScale(axis, L * cosH));
            const float    radius = L * sinH;

            constexpr int kSeg = 12;
            XMFLOAT3 prev{};
            for (int s = 0; s <= kSeg; ++s)
            {
                float sa, ca;
                XMScalarSinCos(&sa, &ca, 6.2831853f * static_cast<float>(s) / kSeg);
                XMFLOAT3 pt;
                XMStoreFloat3(&pt, XMVectorAdd(center,
                    XMVectorAdd(XMVectorScale(u, radius * ca),
                                XMVectorScale(v, radius * sa))));
                if (s > 0) dbg->AddLine(prev, pt, kCone);
                // Four spokes give the cone readable depth without 12 more lines.
                if (s % (kSeg / 4) == 0 && s < kSeg) dbg->AddLine(ap, pt, kCone);
                prev = pt;
            }
        }
    }
}
