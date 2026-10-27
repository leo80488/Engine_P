#include "App.h"
#include "Graphics/AnimProfile.h"

// ==== Engine / ECS ====
#include "ECS/EngineSystems.h"
#include "ECS/TimelineSystem.h"
#include "Sequencer/SequencerSystem.h"
#include "Sequencer/SequenceEval.h"      // Seq::Preview::OwnsViewport (the sequencer's Camera Lock)
#include "Sequencer/SequenceTargets.h"   // Seq::RegisterBuiltinSeqTargets
#include "Sequencer/LuaSequenceBindings.h"

// Sink target for Seq::TimeScale (see the SetSink call in the system registration).
static ScriptSystem* g_seqTimeScaleTarget = nullptr;
#include "ECS/NotifyConsumerSystems.h"
#include "ECS/ProjectileComponent.h"
#include "ECS/LifetimeComponent.h"
#include "ECS/DissolveComponent.h"
#include "ECS/FrameContext.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform / GlobalTransform
#include "ECS/LuaCharacterStateBindings.h"
#include "ECS/LuaFacialBindings.h"      // Face.* (morph targets / blend shapes)
#include "ECS/LuaSkeletonBindings.h"    // Skeleton.* (bones by name, sockets)
#include "ECS/LuaPlayerBindings.h"
#include "ECS/LuaIKBindings.h"
#include "ECS/CameraStackSystem.h"
#include "ECS/CameraSystem.h"       // CameraSystem::SyncControllerFromTransform / ApplyFly
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraLook.h"         // Camera::ApplyCameraLook
#include "ECS/GuidRegistry.h"
#include "Gameplay/EventDelivery.h"   // the two EventBus drains at phase edges
#include "Scripting/ScriptOwner.h"    // LegacyTimers
#include "Gameplay/SaveGame.h"   // InstallPersistenceHooks (save-game v3 graveyard)

// ==== Scene ====
#include "Scene/LuaSceneBindings.h"
#include "Scene/MeshSpawner.h"
#include "Scene/MeshInstanceSystem.h"   // StaticMesh owners -> section entities
#include "Scene/SceneInstanceLoader.h"  // SynthesizeMeshLibIdentity - MeshLibrary's identity provider
#include "Scene/TransformSystem.h"

// ==== Graphics ====
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/StreamlineBackend.h"
#include "Renderer/Renderer.h"
#include "Renderer/RenderExplain.h"             // Render.explain
#include "Renderer/AssetQuery.h"                // Asset.get / Asset.loaded
#include "Renderer/Preview/PreviewHarnessGame.h" // the game-capable -preview_* flags
#include "Graphics/AfterimageSystem.h"
#include "Graphics/WaterRippleSystem.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "Tools/CollisionMeshBaker.h"

// ==== Resource (loaders + importers) ====
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Resource/BCCompressor.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"
#include "Resource/SharedContent.h"
#include "Resource/TextureLoader.h"
#include "Resource/ShaderLoader.h"
#include "Resource/MaterialLoader.h"
#include "Resource/AnimationLoader.h"
#include "Resource/AnimGraphLoader.h"
#include "Resource/AnimSetLoader.h"
#include "ECS/LuaAnimGraphBindings.h"
#include "ECS/AnimEntityResolve.h"
#include "Resource/AnimationClipSystem.h"
#include "Audio/AudioClipLoader.h"
#ifdef WITH_EDITOR
// Importers are editor-only; runtime loads pre-imported .itex/.imsh/.imat/.ianim/.aclip.
#include "Resource/TextureImporter.h"
#include "Resource/ShaderImporter.h"
#include "Resource/MaterialImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/VmdImporter.h"
#include "Audio/AudioImporter.h"
#endif

// ==== Audio / UI / AI / Nav / Physics ====
#include "Audio/LuaAudioBindings.h"
#include "Diagnostics/LuaDebugBindings.h"     // Debug.*
#include "Diagnostics/LuaProfilerBindings.h"  // Profiler.*
#include "Module/ModuleRegistry.h"                 // the enabled modules' hooks
#include "Physics/LuaChainLODBindings.h"      // ChainLOD.*
#include "Renderer/Preview/LuaPreviewBindings.h" // Preview.*
#include "ECS/LuaHitboxBindings.h"          // Hitbox.IsActive
#include "Graphics/LuaVFXBindings.h"         // VFX.*
#include "Graphics/LuaDDGIBindings.h"        // Engine.DDGIWatch*
#include "Graphics/LuaWaterRippleBindings.h" // Engine.RippleAt
#include "Renderer/LuaCameraViewBindings.h"  // Engine.GetCamera*
#include "Resource/LuaPrefabBindings.h"      // Engine.SpawnPrefab
#include "Resource/PrefabSpawnQueue.h"       // the prefab preloads' shutdown
#include "UI/LuaUIEngineBindings.h"          // Engine.SpawnUIPrefab / ResolveUILayout / GetUIPointer
#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "Diagnostics/FrameTiming.h"
#include "Diagnostics/OwnerAccounting.h"
#include "AI/LuaBTBindings.h"
#include "Intent/LuaIntentBindings.h"
#include "Intent/LuaAIBindings.h"
#include "Physics/LuaPhysicsBindings.h"
#include "Resource/PrefabSerializer.h"   // Resource::LoadPrefab (Engine.SpawnPrefab)
#include "Resource/LuaSettingsBindings.h" // Settings.* (in-game options menu)
#include "Scripting/LuaMathTypes.h"      // LuaVec3 (camera getters)
#include "Scripting/LuaGameFlowBindings.h"
#include "Scripting/LuaEntityBindings.h" // Entity.get / find / schema / set (serializable components)
#include "Scripting/LuaGameplayTagBindings.h" // Tags.*
#include "Scripting/LuaAttributeBindings.h"   // Attr.*
#include "Scripting/LuaDataTableBindings.h"   // DataTable.*
#include "Scripting/LuaDoc.h"           // help("Entity.set"): the described bindings
#include "Scripting/BootScripts.h"       // Scripting::RunBootScripts
#include "Resource/ContentRoots.h"       // Resource::RequireGameContent
#include "Resource/EngineDevSettings.h"  // PeekEngineDevPhysics: the physics world's rows
#include "Graphics/PsoPrepare.h"           // the boot precompile
#include "Platform/LuaPlatformBindings.h"   // the Platform Lua table
#include "Platform/PlatformServices.h"      // Plat::Services().Update
#include "Gameplay/StartupState.h"      // Gameplay::RequestStartupState
#include "Scene/BootScene.h"            // Scene::ActivateBootScene
#include "Diagnostics/ProfilerBoot.h"   // Diag::Prof::StartFromCommandLine
#include "Renderer/RenderPathFlags.h"   // -pathtrace / -gipath
#include "Settings/SettingsStore.h"     // the [SETTINGS] boot line; the pass rows' boot read
#include "System/PsoCachePaths.h"        // Sys::ResolvePsoCachePaths
#include "Harness/ExecScenario.h"        // Harness::StartExecScenario (WITH_HARNESS)
#include "PostProcess/LuaPostProcessBindings.h"
#include "ECS/PostProcessResolveSystem.h"

// ==== System / Input ====
#include "System/TaskSystem.h"
#include "Scripting/ScriptVM.h"   // [LUAVM] summary
#include "System/Log.h"
#include "System/BootTimeline.h"   // bootPhasesMs marks
#include "System/ProcessMemory.h"  // the [MEM] line at the first frame
#include "System/PathUtil.h"   // Sys::ExeDir / Sys::UserDataDir: the two roots
#include "System/HostPolicy.h"   // Sys::IsEditorHost (never #ifdef WITH_EDITOR)
#include "System/BuildInfo.h"    // the window title: the product, its version and configuration
#include "System/CommandLine.h"   // -scene / -frames / -hidden
#include "System/ThreadContext.h" // render-context tripwire (game/render split)
#include "Harness/ExitCodes.h"
#include "Harness/RunLedger.h"
#include "Harness/GameFlowDump.h"
#include "Harness/HarnessCaptureSystem.h"
#include "Harness/StreamUploadProbe.h"   // -streamupload_probe's writer
#include "Harness/DeviceLossProbe.h"     // -test_remove_device_after_ms
#include "Harness/PrefabPrewarm.h"       // -prefab_prewarm
#include "Harness/CmdBufTestSystem.h"   // -cmdbuftest
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Diagnostics/ProfilerCollector.h"
#include "Harness/Determinism.h"
#include "Harness/ControlChannel.h"
#include "Harness/CatalogDump.h"        // -dump_catalog (WITH_HARNESS only)
#include "System/Timer.h"
#include "System/FileWatcher.h"
#include "System/HotReloadReceipt.h"      // [HOTRELOAD] receipts (material reloads)
#include "Graphics/ShaderHotReload.h"     // pre-flighted shader reloads
#include "System/HotReloadService.h"      // HotReload::WatchService
#include "System/EventBus.h"
#include "System/AppEvents.h"   // Sys::AppFocusChangedEvent: the alt-tab edge
#include "Scene/SceneEvents.h"  // Scene::SceneLoadedEvent: the flow rebuild hook
#include "Input/InputSystem.h"
#include "Input/OsMouseLook.h"     // Game.exe's mouse look
#include "UI/UIInputSampler.h"    // the Canvas pointer + navigation edges

#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#include "Editor/MaterialPreviewWidget.h"    // preview.enabled pref (harness force-on)
#include "Editor/ChainPhysicsPreviewWindow.h" // the chain-physics preview's EnterPlay teardown
#include "Editor/MeshEditorWindow.h"          // -mesheditor_*
#include "Editor/SkeletonEditorWindow.h"      // -skeleton_editor*
#include "Editor/UIEditorWindow.h"            // the UI asset stages' EnterPlay teardown
#include "Editor/TextureViewerWindow.h"       // -texviewer_*
#include "Editor/MaterialEditorWindow.h"      // -mateditor_*
#include "Editor/NodeEditorSmoke.h"           // -nodeeditor_smoke*
#include "Editor/PreviewThumbnailCache.h"    // -preview_thumb_cache_dir
#include "Editor/EditorCameraController.h" // the editor free camera
#include "Editor/LuaDebugDrawBindings.h"     // Debug.SetDraw, the editor half
#include "imgui/imgui.h"
#endif

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>      // -preview_asset_rewrite_at reads/writes the fixture bytes
#include <iterator>     // istreambuf_iterator, same
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

static const UINT  WindowWidth  = 1920;
static const UINT  WindowHeight = 1080;

// The window App opens: the client size settings.json asks for, read before anything else
// exists (Resource::PeekSettingsWindowSize reads the same bytes BootRenderer loads later), so
// the window and the swap chain are right from the first frame instead of being resized at the
// first loop top. Under -hidden, or with no size in the file, the compiled-in size. The title
// is game.json's product name; outside Retail it adds the version and configuration, and the
// editor says so. A Retail player's second launch finds the window by this exact title.
static Plat::WindowDesc MakeWindowDesc()
{
    static std::wstring s_title;
    const Sys::BuildInfo& build = Sys::Build();
    std::string title = Sys::ProductTitle();
    if (Sys::IsEditorHost()) title += " - Editor";
    if (std::string(build.config) != "Retail")
        title += std::string(" ") + build.version + " (" + build.config + ")";
    LOG_INFO("[WINDOW] title '%s'", title.c_str());
    s_title = Sys::Utf8ToWide(title);
    Plat::WindowDesc desc{ static_cast<int>(WindowWidth), static_cast<int>(WindowHeight), s_title.c_str() };
    Resource::PeekSettingsWindowSize(desc.clientWidth, desc.clientHeight);
    desc.startHidden = true;   // shown at the first frame, when the splash closes
    return desc;
}

// The physics world with engine.json's physics.gravity / physics.timeBeforeSleep (or their -set:).
// App reads the rows: the physics core does not link the settings layer, which pulls the renderer
// into every unit test that links PhysicsSystem on its own.
static void InitPhysicsWorld(DX12Physics::PhysicsSystem& physics)
{
    physics.Init();
    const Resource::PhysicsDevSettings rows = Resource::PeekEngineDevPhysics();
    physics.ApplyWorldSettings(rows.gravity, rows.timeBeforeSleep);
}

// ==== Construction / Run ====

App::App()
    : wnd(MakeWindowDesc())
      // The device needs no window; m_mainSurface, the next member, puts its swap chain on wnd
      // at the client size the window actually got (the request, if it opened minimized under
      // -hidden).
    , m_gfx(std::make_unique<GraphicsDX12>())
{
    Sys::Boot::Mark("window");   // the window, the device and Streamline are up
    LOG_INFO("App initialized");
    // Stamp the bus's main thread explicitly rather than relying on the first caller.
    EventBus::Get().BindMainThread();
#ifdef WITH_EDITOR
    m_editorLayer.OnAttach();
#else
    m_viewportFullscreen = true;   // Game build: the scene always fills the backbuffer.
#endif
    Sys::Boot::Mark("appAttach");   // the host attached (the editor layer, or Game's fullscreen viewport)
}

App::~App()
{
    // Frame coroutines drain before Run unwinds and before renderer teardown.
    // The render-owner lane goes first when it exists: it holds the device
    // frame thread and its queue may only be drained on its own thread.
    m_pipeline.Shutdown();   // the lane, the park hook, the GPU completions, the queue
}

int App::Run()
{
    // The frame pipeline recorded Main's id (the one thread DrainFrameWork serves) when App
    // was constructed, so a boot chunk may drain from its first line.
#ifdef TRACY_ENABLE
    tracy::SetThreadName("Main");
#endif
    // First thing: the engine profiler's registry entry for this thread (the
    // P in W/(P*T) counts it) and its SetThreadDescription.
    Diag::Prof::RegisterThread("Main", Diag::Prof::Role::Main);
    // -taskaffinity reserves physical core 0 for main (a no-op without it).
    TaskSystem::Get().PinOwnerThread(0);
    BootContent();

    IGraphicsDevice& backend = *m_gfx;

    RegisterResourceLoaders(backend);

    const std::string psoCacheWritePath = InitPsoLibrary(backend);
    // The boot precompile's workers build into the device: cancelled (what they built is saved) on
    // every way out of Run, before the device goes.
    struct PsoPrepareGuard
    {
        IGraphicsDevice& device;
        ~PsoPrepareGuard() { try { Gfx::PsoPrepare::Cancel(device); } catch (...) {} }
    } psoPrepareGuard{ backend };
    Gfx::PsoPrepare::Begin(backend, psoCacheWritePath);

    InitCoreSystems(backend);

    // The pass settings (Settings/PassSettings.h): each pass row's command-line flag and
    // -set:pass.<...>, applied before the renderer builds a pass that could read one.
    Settings::ApplyPassCommandLine();
    Sys::Boot::Mark("runSetup");        // world, systems, loaders - everything before the renderer
    Renderer renderer(backend);
    Sys::Boot::Mark("rendererCtor");
    // Drains frame work before the run-local renderer goes: the renderer must outlive every
    // accepted frame continuation, even when gameplay, hot reload or a harness assertion unwinds
    // the loop.
    struct RunFrameDrain {
        App& app;
        ~RunFrameDrain() { try { app.m_pipeline.DrainFrameWork(Diag::DrainCause::Quit); } catch (...) {} }
    } runFrameDrain{*this};
    BootRenderer(backend, renderer);

    const bool bootScriptsRefused = BootScripting(backend, renderer);

    InitPhysicsWorld(m_physicsSystem);
    InitAudio();

    // ---- Build GameModeContext ----
    GameModeContext ctx{ backend, renderer };
    ctx.resourceMgr = &m_resourceMgr;
    ctx.textureSys  = &m_textureSys;
    ctx.meshLib     = &m_meshLib;
    ctx.assetMgr    = &m_assetMgr;
    ctx.world       = &m_world;
    ctx.sceneManager = &m_sceneManager;

    // Pending scene swap, drained once per frame in the loop; disengaged = no swap pending.
    // An optional, not an empty-string sentinel: an empty id is reachable (Lua's Scene.Load(""),
    // a GameFlow state with a blank scene field) and must reach ActivateScene, whose base-scene
    // fallback publishes SceneLoaded / SceneLoadFailed. Dropping it would leave a Loading state
    // with no edge to leave on (Scene/SceneEvents.h).
    std::optional<std::string> pendingScene;
    ctx.requestReplaceMode = [&pendingScene](const std::string& next) {
        pendingScene = next;
    };
    // Faded variant: scenes call this for player-facing switches; the manager
    // drives fade-out/loading/fade-in and triggers the raw swap above when the
    // screen is fully black (see SceneTransitionManager::Tick).
    ctx.beginTransition = [this](const std::string& next) {
        m_transition.Begin(next);
    };

    BootScenes(backend, renderer, ctx, bootScriptsRefused);

    // renderer.SetPhysicsSystem / SetAnimationClipSystem and RegisterTickSystems run inside
    // BootScenes, before the boot-scene push: the flow-driven boot asks the flow which states it
    // defines, and only RegisterTickSystems' FinalizeDefinitions makes that answer trustworthy.
    // WireEditor runs after the push: AdoptLoadedScene needs the boot scene loaded, or the
    // editor has no current scene and the first Ctrl+S degrades into a Save As dialog.
    WireEditor(backend, renderer, ctx);   // the editor host's wiring (a no-op in Game.exe)

    InstallWindowHooks();              // alt-tab latch, the close veto (AFTER WireEditor)

    WirePlayMode(backend, renderer);   // the Edit/Play hooks, Game's Playing pin, -play

#ifdef WITH_EDITOR
    // ImGui docking settles across frame boundaries.  Resolve it before the
    // first visible scene frame so UpdateViewportSize starts at the final dock
    // extent instead of rebuilding 1920x1001 -> floating -> docked textures.
    m_editorLayer.SetWorld(ctx.world);
    m_editorLayer.SetRenderView(renderer.GetView());
    m_editorLayer.PrimeViewportLayout();
#endif

    Timer timer;

    // The shader and asset watchers: hot reload with receipts, drained at the loop top before
    // BeginFrame (System/HotReloadService.cpp). The editor's asset browser and thumbnail cache
    // listen through the hooks.
    HotReload::WatchService hotReload{ m_world, m_textureSys, m_resourceMgr, HotReload::WatchService::Hooks{
        [this] { if (m_pipeline.RenderOwnerEnabled()) m_pipeline.DrainFrameWork(Diag::DrainCause::AssetWatch); },
#ifdef WITH_EDITOR
        [this](const std::vector<std::string>& rooted) { m_editorLayer.NotifyThumbnailDependencyChanged(rooted); },
        [this](std::size_t count)
        {
            LOG_INFO("Asset watch: %zu external file change(s) — refreshing the browser",
                     count);
            m_editorLayer.NotifyExternalAssetChange();
        },
#endif
    } };
    hotReload.Start();

    // -frames=N: quit after exactly N fully presented frames, not counting the frames the boot's
    // PSO preparation screen shows (a first launch's length depends on the driver, not the run);
    // 0 (the default) runs until something else ends the process. Read once, not per frame:
    // CommandLine::GetInt scans linearly and can LOG_WARNING on a malformed value, which per frame
    // would bury the log.
    const int frameBudget = Sys::CommandLine::Get().GetInt("-frames", 0);
    if (frameBudget > 0)
        LOG_INFO("Frame budget: %d frame(s)", frameBudget);
    std::uint64_t prepareFrames = 0;   // frames behind the PSO preparation screen, outside the budget

#ifdef WITH_HARNESS
    // -dump_catalog=<dir>: the engine half of the generated catalog (Harness/CatalogDump.h).
    // <dir> is relative to the working directory, not the artifacts dir; its home is
    // build/catalog/engine/<host> (tools/gen_catalog.py --engine).
    const std::string catalogDumpDir = Sys::CommandLine::Get().Get("-dump_catalog");
    bool catalogDumpDone = catalogDumpDir.empty();
#endif

    // The frame executor: -rendermode, the device-thread park hook, -renderowner,
    // -animoverlap, -brslane, -brsplanlane, the destroy deferral and the access
    // tooling, each with its refusals and [SPLITCHECK] lines (Renderer/FramePipeline.cpp).
    m_pipeline.Configure(backend, renderer);

    // -play_frames=N: leave Play (the editor's Stop path) at frame N. Read once, like -frames.
    // The command-line driver of the Stop path (snapshot restore, OnWorldClear ordering,
    // ScriptSystem::ClearInstances), whose teardown order is load-bearing; Lua's Editor.Stop()
    // is the scripted one. The run continues to -frames afterwards, so the restored editor world
    // still renders and can be dumped and diffed against the pre-Play state.
    const int  playStopFrame = Sys::CommandLine::Get().GetInt("-play_frames", 0);
    bool       playStopSent  = false;
    if (playStopFrame > 0)
        LOG_INFO("Play budget: Stop requested at frame %d", playStopFrame);

    // -play_at=N: defer -play's RequestPlay to frame N. -play alone enters Play from Init, so
    // the editor-world snapshot (PlayModeController's CloneState) is taken before anything a
    // harness fixture spawns exists. This flag answers whether a fixture ARMED IN EDIT MODE
    // survives the Play -> Stop restore: World::AdoptState restores ids and names, so an entity in
    // the snapshot comes back under its old name and one created after it does not.
    // -play is still required beside it: the -exec Edit-mode warning and GameFlow's resolve gate
    // read Has("-play") as "this run intends to reach Play". This flag moves the when, not the
    // whether.
    const int  playAtFrame = Sys::CommandLine::Get().GetInt("-play_at", 0);
    bool       playAtSent  = false;
    if (playAtFrame > 0)
        LOG_INFO("Play budget: Play requested at frame %d (deferred from boot)",
                 playAtFrame);

#if defined(WITH_HARNESS)
    // The game-capable -preview_* flags (-preview_pip, -preview_idle_frames,
    // -preview_meshlib_cap, -preview_ibl_delay, -preview_key_light, -preview_pip_lights,
    // -preview_pip_lights_at), parsed by Preview::HarnessGame
    // (Renderer/Preview/PreviewHarnessGame.cpp). A Run() local: its latches live for the run.
    Preview::HarnessGame previewHarnessGame{ m_world };
    previewHarnessGame.ParseBootFlags(renderer);
#endif

#if defined(WITH_HARNESS) && defined(WITH_EDITOR)
    // The editor-host -preview_* flags, the editor tools' flags and -chainpreview_*, parsed by
    // Editor::PreviewHarness (Editor/PreviewHarness.cpp). Parsed here, after WireEditor and
    // PrimeViewportLayout: the order of the refusals and their log lines is the packs' contract.
    if (Sys::IsEditorHost())
        m_previewHarness.ParseBootFlags();
#endif

    while (true)
    {
        const auto loopStart = std::chrono::steady_clock::now();
        // Closes the previous frame's profiler window (its per-thread CPU
        // record) and opens this one. The window IS the main loop period.
        Diag::Prof::BeginFrame(ctx.frame);
        m_pipeline.LogGameBegin(ctx.frame);
        // Rethrow exceptions that escaped a fire-and-forget TaskSystem::Push. Push has no join
        // point, so without this poll a worker-side failure (async resource load, BLAS build, ...)
        // would only log and leave half-loaded state. Rethrown on the main thread at a frame
        // boundary where nothing is mid-record; main.cpp's try/catch reports it and exits cleanly.
        if (TaskSystem::Get().HasPendingTaskException())
            TaskSystem::Get().RethrowPendingTaskException();

        // ---- Streamline frame boundary ----
        // Take this frame's token before anything else touches Streamline. Every SL call for the
        // frame (Reflex sleep, the PCL markers, DLSS's constants / tags / evaluate) must get the SAME
        // token: the CPU runs up to kFrameCount-1 frames ahead, so a second token taken later would
        // pair one frame's constants with another frame's tags, which DLSS-G rejects and DLSS SR
        // reprojects wrongly.
        {
            auto& sl = SL::StreamlineBackend::Get();
            void* const slFrame =
                sl.NewFrameToken(static_cast<uint32_t>(ctx.frame));

            // Sleep BEFORE the OS message pump: Reflex holds the frame back so the CPU starts as
            // late as it safely can, which only cuts latency if input is sampled after the wait.
            sl.ReflexSleep(slFrame);
            sl.SetLatencyMarker(SL::LatencyMarker::SimulationStart, slFrame);
        }

        // Hot reload, drained BEFORE BeginFrame so no in-flight command list still references
        // the old PSOs (a reload flushes and waits on the GPU).
        {
            hotReload.Poll(ctx, backend, renderer);

#ifdef WITH_EDITOR
#ifdef WITH_HARNESS
            // -preview_touch_at / -preview_asset_appear_at / -preview_asset_rewrite_at,
            // delivered HERE, beside the poll: the one place that knows the watcher-
            // relative <-> root-relative mapping (Editor/PreviewHarness.cpp).
            m_previewHarness.DeliverDue(ctx, HotReload::WatchService::kShaderWatchPrefix,
                                        HotReload::WatchService::kAssetWatchPrefix);
#endif
#endif
        }

        // The messages, and a swallowed WM_CLOSE's close handlers (Plat::Window::Pump).
        if (auto ecode = wnd.Pump())
        {
            Sys::Boot::Mark("loopExit");   // teardown starts: shutdown cost = shutdown - loopExit
            // Record the frame count before teardown: only this point knows it, and run.json's
            // framesRun tells "the run finished" apart from "something killed it at frame 3".
            // First CONSUME the packet still in flight: on every quit path WM_QUIT is only queued, so
            // the last iteration already kicked; dropping that frame would leave presented ==
            // ctx.frame - 1 in threaded mode and break framesRun ("frames the screen showed") for every
            // pack that asserts on it.
            m_pipeline.DrainFrameWork(Diag::DrainCause::Quit);
            Sys::Boot::Mark("exitDrain");   // the packet in flight consumed

            Harness::RunLedger::Get().NoteFrames(ctx.frame);

            // GAME frames vs PRESENTED frames, reported unconditionally
            // (FramePipeline::ReportExitFrameCounts).
            m_pipeline.ReportExitFrameCounts(backend, ctx);
#ifdef WITH_HARNESS
            // Same reason as framesRun: the last point that still sees the state machine. run.json
            // is the only place "which state did it end in, and was anything refused" survives the
            // process.
            Harness::NoteGameFlowToLedger(m_gameFlow);
#endif
            // BEFORE ShutdownAllSystems: the pipe thread can be blocked waiting
            // for a frame drain that will now never happen, and teardown is where
            // the world it would have queried goes away. Stopping first releases
            // that client with "shutting_down" instead of a 30 s hang.
            Harness::ControlChannel::Get().Stop();
            Harness::ControlChannel::Get().SetEditorHandler({});
            // Writes any capture window still open (marked truncated) and the
            // summary, then joins the collector - before the systems whose
            // threads it may still be reading go away.
            Diag::Prof::StopCollector();
            ShutdownAllSystems(backend, renderer);
            Sys::Boot::Mark("exitSystems");   // every system shut down
            // The per-user root: see the two-root note in InitPsoLibrary. A precompile still running is
            // cancelled first, so the save holds what it built. The cancel waits for the pipelines the
            // driver is compiling (seconds for the slowest): the window goes first, not frozen on screen.
            if (Gfx::PsoPrepare::Active())
                ::ShowWindow(wnd.NativeHandle(), SW_HIDE);
            Gfx::PsoPrepare::Cancel(backend);
            backend.SavePSOLibrary(psoCacheWritePath.c_str());
            Sys::Boot::Mark("exitPsoSave");   // then Run's locals (the renderer) go: main's runReturned
            return *ecode;
        }

        // The window's notifications - the alt-tab edge, device changes, dropped
        // files, the modal-loop end - handed to App's handlers (InstallWindowHooks)
        // HERE and not inside the WndProc: EventBus::Publish CREATES the channel
        // on first use - an unordered_map insert - and the WndProc is re-entrant,
        // reachable from inside a subscriber callback that pumps messages
        // (SetWindowPos / ShowWindow / a DXGI mode change), i.e. from inside the
        // DispatchAll that is walking that very map. Costs at most one frame.
        wnd.DispatchEvents();

        // Frame boundary: the previous frame's EndFrame has returned and this frame's BeginFrame
        // has not run, so nothing holds a back buffer. Only here is destroying and rebuilding the
        // swap chain legal, so every fullscreen change (Alt+Enter, the Graphics Settings panel,
        // EngineSettings::ApplyTo) goes through Window::RequestFullscreen and is serviced here;
        // applying it inline from an ImGui handler faults inside the display driver on the next draw.
        // Minimized: hold the loop to about 10 Hz. Window itself refuses under -hidden /
        // -unattended; App adds the one exemption it owns: an open control channel.
        wnd.ThrottleInBackground(Harness::ControlChannel::Get().Enabled());

        wnd.ServicePendingFullscreen();
        // Same rule, same place: a windowed resize dispatches WM_SIZE synchronously, which
        // rebuilds every back buffer. After the fullscreen service on purpose: a request that
        // arrived with a fullscreen toggle must see the final mode, since ServicePendingSize drops
        // the resize when the window ends up fullscreen.
        wnd.ServicePendingSize();
        // The same rule one level down: the DEVICE also latches a plain Resize() (and a direct
        // SetFullscreen that bypassed Window::RequestFullscreen) arriving mid-frame, because a
        // WM_SIZE can be delivered synchronously inside a frame (SetWindowPos / ShowWindow / DXGI's
        // SetFullscreenState). ServicePendingSwapChainOps applies the latched op here, outside any
        // frame. Not load-bearing: BeginFrame drains the same latch as a backstop; this saves a
        // frame of latency and keeps the rebuild outside BeginFrame.
        // CompatibilityBoundary: the loop-top drain (or, with frame overlap, pump) of render frame
        // work, before the CPU timing bracket. Removing it requires a game/render shared-state audit.
        m_pipeline.CompatibilityBoundary();
        m_gfx->Surface().ServicePendingSwapChainOps();

        // timer.Mark() is called unconditionally even when -fixeddt overrides
        // the result: it also advances the timer's internal epoch, and
        // skipping it would make the FIRST real-clock read after the run
        // report the whole elapsed session.
        const float realDt = timer.Mark();

        // -fixeddt is applied here, at the single source, because this value feeds BOTH clocks:
        // FrameContext::deltaTime goes through PlayModeController (Pause/Step aware), but
        // GameModeContext keeps the raw dt, which drives Renderer::BeginFrame, m_globalTimeSec,
        // LightCB.timeParams, particles / trails, grass and water wind, custom-shader time and FSR2.
        // Overriding only the play-mode clock would leave the picture on wall clock.
        // The real clock is clamped here too: a stall (the window drag / resize modal loop, a
        // dialog, the Alt menu, a breakpoint, a long load) is wall time the game did not run, and
        // advancing the world by all of it in one frame makes animation, particles and Lua timers
        // jump. 0.25 s matches the physics accumulator's cap. -fixeddt still wins, and realDt stays
        // raw for the frame-timing stats.
        constexpr float kMaxFrameDt = 0.25f;
        const bool modalLoopEnded = std::exchange(m_modalLoopEnded, false);
        if (realDt > kMaxFrameDt && (modalLoopEnded || realDt > 1.0f))
            LOG_INFO("App: a %.2f s frame (%s) advances the game by %.2f s", realDt,
                     modalLoopEnded ? "the window was dragged or resized" : "a stall", kMaxFrameDt);
        const float fixedDt = Harness::Determinism::Get().FixedDt();
        const float dt      = (fixedDt > 0.0f) ? fixedDt : (std::min)(realDt, kMaxFrameDt);

        LARGE_INTEGER cpuStart, cpuEnd, cpuFreq;
        QueryPerformanceCounter(&cpuStart);
        // Bracket-local owner instrumentation. Reset with the bracket, not at
        // the loop top: the loop-top Pump above is outside cpuFrameMs and
        // charging its drains to this frame would not compose with cpuMs.
        m_pipeline.BeginFrameBracket();
        const float loopTopMs = static_cast<float>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - loopStart).count());

        UpdateViewportSize(ctx, backend);

        // Apply queued Edit/Play transitions before Canvas resolves hierarchy;
        // Stop may restore the editor-world snapshot in place. The simulation runs on the
        // world clock: 0 while the path tracer's time freeze holds (Renderer::BeginWorldClock),
        // and so does everything on FrameContext::deltaTime (animation, the camera stack, VFX
        // lifetimes); the editor camera, UI painting and scene transitions keep dt (realDeltaTime).
        const float worldDt = ctx.renderer.BeginWorldClock(dt);
        m_playModeController.BeginFrame(worldDt);
#ifdef WITH_EDITOR
        // The Game View's route (Editor/GameView.h): which panel the main pipeline draws into, through which
        // camera, at which size - after the Edit / Play transition, before anything reads the size.
        m_editorLayer.UpdateMainViewRoute(m_playModeController.Mode() != EngineMode::Edit, m_viewportFullscreen,
                                          ctx.viewportW, ctx.viewportH);
        // What the game may read this frame on the Game View's routes, and its mouse capture: taken before
        // any input is sampled, so the Esc that releases the capture never reaches the game.
        const EditorLayer::GameInputGate gameGate =
            m_editorLayer.UpdateGameInputGate(wnd.Keyboard(), m_flowWantsCapture);
        SyncWindowCursor();
#endif

        // Canvas input/layout runs before every gameplay phase. It reads the
        // ungated platform snapshot (updated even while ImGui or Canvas owns a
        // gesture), then publishes this frame's gameplay-capture decision.
        {
            float vpMinX = 0.f, vpMinY = 0.f;
            float pointerScaleX = 1.f, pointerScaleY = 1.f;
            bool  hostOwnsPointer = false;
#ifdef WITH_EDITOR
            if (!m_viewportFullscreen)
                m_editorLayer.GetViewportMin(vpMinX, vpMinY);
            // Editor chrome sits above the game Canvas: keep tracking physical edges, but stop an
            // ImGui-owned click from hitting underneath. The exception is the scene image itself:
            // WantCaptureMouse is true over ANY ImGui window, and the scene is always drawn inside one
            // (the docked "Viewport" panel, or in F11 mode a borderless host window holding only the
            // scene image). IsViewportHovered(), set from the scene image's own hover test in both
            // layouts, tells editor chrome from the game view.
            // While another ImGui widget owns a drag (a slider dragged out over the scene) the gesture
            // stays ImGui's. Ask the editor's IsOtherImGuiItemActive(), not a raw
            // ImGui::IsAnyItemActive(): the docked viewport's InvisibleButton holds the ActiveId for the
            // whole press, so the raw query would claim every click aimed at the game.
            // The rect tool lives ON the scene image, so neither term sees it; without
            // IsUIRectToolClaimingMouse, grabbing a button's resize handle in Play also presses it.
            hostOwnsPointer =
                (ImGui::GetIO().WantCaptureMouse
                 && (!m_editorLayer.IsViewportHovered()
                     || m_editorLayer.IsOtherImGuiItemActive()))
                || m_editorLayer.IsUIRectToolClaimingMouse();
            // On the Game View's route the Canvas is that panel's picture, letterboxed and maybe scaled.
            m_editorLayer.MapGamePointer(vpMinX, vpMinY, pointerScaleX, pointerScaleY, hostOwnsPointer);
#endif
            // The pointer, its edges and the wheel, and the keyboard / pad navigation
            // edges - derived once, here (UI/UIInputSampler.cpp).
            UI::SampleCanvasInput(wnd.Pointer(), m_uiInput, m_prevUILeft, vpMinX, vpMinY, hostOwnsPointer,
                                  pointerScaleX, pointerScaleY);

            UI::UIScreen canvas;
            canvas.size = { static_cast<float>(ctx.viewportW),
                            static_cast<float>(ctx.viewportH) };
            m_uiCanvasSystem.UpdateInput(m_world, m_uiInput, canvas);
        }

        // Snapshot gameplay input after Canvas hit-testing so UI-owned mouse
        // holds and edges cannot leak into scripts/controllers this frame.
        bool suppressGameplayMouse = m_uiCanvasSystem.WantsCaptureMouse();
#ifdef WITH_EDITOR
        // ImGui owns the mouse over any window - except the Game View's picture while the game has its focus.
        suppressGameplayMouse = suppressGameplayMouse
                                || m_editorLayer.SuppressGameplayMouse(ImGui::GetIO().WantCaptureMouse);
#endif

        // ---- Viewport mouse delta, before the input snapshot ----
        // Computed before Input::Update so there is one input write window per frame: the mouse
        // pixel delta and the pad poll must land in the same ActionState, or a Cmd() reader would see
        // this frame's stick against last frame's mouse. Its only inputs are the window's
        // PointerState (fed during wnd.Pump()), EditorLayer members written in last frame's
        // OnUIRender and m_uiCanvasSystem.WantsCaptureMouse() (published by UpdateInput); Input::Update
        // touches none of them.
        float mouseLookDX = 0.f, mouseLookDY = 0.f;
        bool  mouseLookHeld = false;
#ifdef WITH_EDITOR
        if (m_uiCanvasSystem.WantsCaptureMouse())
        {
            mouseLookHeld = false;
            mouseLookDX = 0.f;
            mouseLookDY = 0.f;
        }
        else
        {
            // The Scene viewport's drag, or the Game View's on its route (EditorLayer_GameView.cpp).
            m_editorLayer.GetGameMouseLook(mouseLookHeld, mouseLookDX, mouseLookDY);
        }
        // The editor looks through ImGui's delta and drops the raw motion the window accumulated, so it does
        // not pile up for the session - except under the Game View's capture, where look is the raw motion with
        // no button held (Game.exe's captured rule).
        const auto rawLook = wnd.Pointer().ConsumeRawDelta();
        if (gameGate.captured && !m_uiCanvasSystem.WantsCaptureMouse())
        {
            mouseLookHeld = true;
            mouseLookDX   = static_cast<float>(rawLook.first);
            mouseLookDY   = static_cast<float>(rawLook.second);
        }
#else
        // Game build (no editor viewport): drive the free-look camera from the OS mouse
        // (Input/OsMouseLook.cpp, which remembers the previous position). Right button held = look;
        // the delta is this frame's mouse movement in pixels, the editor's ImGui MouseDelta
        // convention, so CameraSystem sensitivity matches. A CAPTURED cursor (GameFlow's "captured":
        // hidden and clipped) looks from raw motion instead, without the right button.
        m_osMouseLook.Sample(wnd.Pointer(), m_uiCanvasSystem.WantsCaptureMouse(),
                             !wnd.CursorEnabled(), mouseLookDX, mouseLookDY, mouseLookHeld);
#endif

        // ---- The single per-frame input snapshot ----
        // Keyboard + gamepad are sampled here and resolved into one immutable ActionState. Nothing
        // downstream may poll a device or derive its own edges; everything reads Input::Get().Cmd().
        // Do not move this call: it must run after Canvas hit-testing (a UI-owned mouse hold must not
        // leak into gameplay) and before TickCamera (the camera must see this frame's stick).
        {
            Input::UpdateInputs ui;
            ui.suppressMouseButtons = suppressGameplayMouse;

            // The pad is not a pointer: do not gate it on WantCaptureMouse, which is true whenever the
            // cursor is over an ImGui window (in a docked editor, nearly always) and, with the
            // viewport-hover gate below, would make pad look dead both ways. "The UI owns the pad now"
            // is a mapping context with blockLower, pushed by whoever opened the UI.
            ui.suppressGamepad = false;
#ifdef WITH_EDITOR
            // Typing into an Inspector field is the one genuine conflict: the
            // pad should not drive gameplay while a text cursor is live.
            ui.suppressGamepad = ImGui::GetIO().WantTextInput;
            // The Game View's routes: the game reads input only while that panel has focus or the capture.
            ui.hostFocus       = gameGate.hostFocus;
            ui.quarantineKey   = gameGate.quarantineKey;
            ui.suppressGamepad = ui.suppressGamepad || !gameGate.hostFocus;
#endif
            ui.simAdvancing  = m_playModeController.SimAdvancing();
            ui.mouseLookDX   = mouseLookDX;
            ui.mouseLookDY   = mouseLookDY;
            ui.mouseLookHeld = mouseLookHeld;
            // The keys and the mouse buttons as the window's WndProc saw them
            // (Platform/KeyboardState.h).
            ui.keyboard      = &wnd.Keyboard();
            Input::Get().Update(ui);

            // The look the snapshot RESOLVED - in a harness build the synthetic
            // one (Test.Look, -input_replay) is part of it - so the camera
            // gates published below (fly on RMB, the look delta) agree with
            // Cmd(): an injected drag steers and arms fly as a real one does.
            const Input::UpdateInputs& resolved = Input::Get().LastInputs();
            mouseLookDX   = resolved.mouseLookDX;
            mouseLookDY   = resolved.mouseLookDY;
            mouseLookHeld = resolved.mouseLookHeld;
        }

#ifdef WITH_EDITOR
        if (Input::Get().WasKeyPressed(VK_F11))
            m_viewportFullscreen = !m_viewportFullscreen;
#endif

        // Debug HUD level cycle: F9 forward, Shift+F9 back (Off/Fps/Unit/Full). Not #ifdef'd: the
        // overlay uses the engine's SDF font instead of ImGui so it works in Game.exe and in editor
        // F11 fullscreen, where OnUIRender returns before the Profiler panel.
        // A raw key poll, not a GameAction: actions are serialized by name into every player's
        // rebind block, so a debug key would show up in the shipped controls UI. F9 is free; VK_F11
        // is the only other bound VK_F* key.
        if (Input::Get().WasKeyPressed(VK_F9))
            m_debugHud.CycleLevel(Input::Get().IsKeyDown(VK_SHIFT) ? -1 : +1);

        // The menu key (a host action, exempt from the simAdvancing gate) goes through the game
        // flow, not EngineMode::Paused, which would stop Lua and any pause menu with it. Game
        // builds only: in the editor Escape means "deselect / close popup".
#ifndef WITH_EDITOR
        if (Input::Get().Cmd().Pressed(GameAction::MenuToggle) && !m_gameFlow.OnMenuKey())
            m_playModeController.RequestTogglePlay();
#endif

        // ---- Publish the camera drive fields into the mode context ----
        ctx.mouseViewportDX        = mouseLookDX;
        ctx.mouseViewportDY        = mouseLookDY;
        // RMB only: this gates the Free-mode WASDQE fly branch, so a resting stick must never set
        // it, or the editor's W/E/R gizmo shortcuts would fly the camera.
        ctx.viewportRightMouseHeld = mouseLookHeld;
        // Device-agnostic ROTATION gate: RMB or the right stick off-centre.
        ctx.cameraLookActive       = Input::Get().Cmd().lookActive;
#ifdef WITH_EDITOR
        // Edit mode only: ortho views zero the mouse delta and App overrides the render camera
        // afterwards, so a stick must not fight that, and while authoring the pad must not rotate
        // the scene when the user works in a docked panel. In Play the pad owns the camera
        // unconditionally: a controller player leaves the mouse anywhere, so a hover gate would
        // silence the right stick for the session.
        if (m_playModeController.Mode() == EngineMode::Edit)
        {
            if (m_editorLayer.GetEditorViewMode() != 0   // 0 == Perspective
                || !m_editorLayer.IsViewportHovered())
                ctx.cameraLookActive = ctx.viewportRightMouseHeld;
        }
#endif

        // Promote async-loaded resources to GPU (2 ms budget). Renderer material
        // resources consume the promoted handles during BuildScene_SyncMaterials.
        {
            ZoneScopedN("ResourceUpload");
            m_resourceMgr.ProcessPendingGPUUploads(2.0f);
        }

        ctx.deltaTime = dt;

        RefreshMainCamera();
        TickCamera(dt, ctx);

        // ---- Edit/Play state machine ----
        // The editor toolbar queued Play/Stop/Pause/Step intents last frame; BeginFrame (applied
        // above) fires onEnterPlay / onExitPlay (world snapshot / restore + temporal cut) on the
        // Edit<->Play edges, advances the GameClock and resolves this frame's simulation dt (real
        // dt, or one fixed 1/60 s tick while Stepping). Game builds are pinned to Playing at boot,
        // so transitions are inert there.
        const EngineMode mode        = m_playModeController.Mode();
        const float      effectiveDt = m_playModeController.EffectiveDeltaTime();
        const bool       runUpdate   = m_playModeController.SimAdvancing();

        // The renderer's chain solver and the audio transport are set in ApplyGameFlowResources,
        // after the Input phase: they answer to the editor transport OR the game flow, and the
        // flow's value is this frame's only once the resolver has run.

        // scaledDt feeds gameplay / physics / AI; deltaTime feeds hit-stop / UI.
        // Only the ScriptSystem factor is known here. The GameFlow factor is multiplied in after the
        // Input phase, where a pause requested last frame becomes current: folding it in here would
        // read the previous frame's state, so the frame you press pause on would run gameplay at full
        // speed while the fixed physics loop already saw the new value. FrameContext is mutated in
        // place across phase groups; this is one of those crossings.
        float scaledDt = effectiveDt * m_scriptSystem.GetTimeScale();

        FrameContext frameCtx;
        frameCtx.deltaTime        = effectiveDt;
        frameCtx.realDeltaTime    = ctx.renderer.IsWorldClockFrozen() ? dt : effectiveDt;
        frameCtx.scaledDeltaTime  = scaledDt;
        frameCtx.fixedDeltaTime   = 1.0f / 60.0f;
        frameCtx.frameIndex       = ctx.frame;
        frameCtx.isFixedTickPhase = false;
        frameCtx.mode             = mode;
        frameCtx.runUpdate        = runUpdate;
        frameCtx.cameraEntity     = static_cast<uint32_t>(m_cameraEntity);
#ifdef WITH_EDITOR
        // In Edit mode the systems that still run and read the camera entity
        // (debug-draw distances, post-process volume fallback) should measure
        // from the actual editor view, not the parked scene camera.
        if (mode == EngineMode::Edit && m_editorCamera.CameraEntity() != NullEntity
            && m_world.IsAlive(m_editorCamera.CameraEntity()))
            frameCtx.cameraEntity = static_cast<uint32_t>(m_editorCamera.CameraEntity());
#endif
        frameCtx.commandBuffer    = &m_commandBuffer;
        frameCtx.jobSystem        = &TaskSystem::Get();

        // ---- Variable-rate simulation phases ----
        {
            ZoneScopedN("Simulation");
            PROF_SCOPE("Simulation");
            m_scheduler.RunPhase(TickPhase::Input,             m_world, frameCtx);

            // GameFlow has resolved: its time scale is now this frame's. It multiplies the script
            // scale rather than replacing it: calling ScriptSystem::SetTimeScale from here would clobber
            // a cutscene's Seq::TimeScale stack and restore the wrong value on resume.
            scaledDt = scaledDt * m_gameFlow.EffectiveTimeScale();
            frameCtx.scaledDeltaTime = scaledDt;
            ApplyGameFlowResources(renderer, frameCtx);

            m_pipeline.PumpFrameWork();
            // The asynchronous prefab spawns commit here, at the top of the game half before
            // GameplayPreLogic: between phases nothing iterates the World, and it runs in every mode (a
            // request from the pipe or the console in Edit mode still commits). Not a system: an
            // exclusive one would add a scheduler batch to every frame for a queue that is empty on
            // nearly all of them. Their onReady runs in this frame's ScriptTimerSystem
            // (GameplayPreLogic).
            Resource::PrefabSpawnQueue::CommitSpawns(m_world, frameCtx.frameIndex);
            m_scheduler.RunPhase(TickPhase::GameplayPreLogic,  m_world, frameCtx);
            m_scheduler.RunPhase(TickPhase::GameplayLogic,     m_world, frameCtx);
            m_scheduler.RunPhase(TickPhase::GameplayPostLogic, m_world, frameCtx);
            m_pipeline.PumpFrameWork();
            m_scheduler.RunPhase(TickPhase::AI,                m_world, frameCtx);
        }

        {
            ZoneScopedN("Physics");
            PROF_SCOPE("Physics");
            m_pipeline.PumpFrameWork();
            RunFixedPhysicsLoop(scaledDt, frameCtx, ctx.renderer.IsWorldClockFrozen());
        }

        // A store's callbacks (an unlock confirmed, its overlay opened), on the main thread.
        Plat::Services().Update();

        // Advance any in-flight scene transition (fade/loading). When fade-out
        // completes it calls ctx.requestReplaceMode, which the drain below picks
        // up the SAME frame, so the blocking scene load runs behind full black.
        Gfx::PsoPrepare::Poll(backend);
        if (Gfx::PsoPrepare::Active())
            ++prepareFrames;
        if (runUpdate)
            m_transition.Tick(frameCtx.realDeltaTime, ctx.requestReplaceMode);

        // Scene swap drain, inline (not a System adapter) so the blocking load runs at a fixed
        // point in the frame. A swap waits for the boot precompile: the scene's first draws must
        // find their pipelines in the library.
        if (runUpdate && pendingScene && !Gfx::PsoPrepare::Active())
        {
            // Take the request before the blocking load: ActivateScene binds the next scene script
            // and queues its events rather than running Lua, so nothing should re-request mid-call; if
            // something does, the new request survives to the next frame instead of being erased.
            const std::string target = std::move(*pendingScene);
            pendingScene.reset();
            m_sceneManager.ActivateScene(target);
        }

        m_scheduler.RunPhase(TickPhase::PhysicsInterpolation, m_world, frameCtx);

        // Animation owns the upload rings and the next mutable render preparation; the game / AI
        // / physics phases before it may overlap the frozen recording of F-1.
        // This barrier closes the overlap window: every render continuation of F-1 that the
        // PumpFrameWork sites did not reach runs here, blocking, timed apart from the other blocking
        // drains so the remaining overlap window is a number.
        // -animoverlap moves this drain to the DrainFrameWork at the top of TickPhase::Render, the
        // frame's other hard-sync point; everything between the two (the upload-slot acquire, the
        // four animation phases, SetCamera, BuildGameView) then runs beside the lane's render of F-1.
        // The window and the wait are measured at whichever site is the barrier, so winMs= and
        // tailMs= mean the same in both arms.
        m_pipeline.AnimationBoundaryBarrier();
#if defined(WITH_HARNESS) && WITH_HARNESS
        renderer.GetMeshManager().GetDescriptorHeap().ValidateMeshAABBIdleWrite();
#endif

        m_pipeline.AcquireUploadSlot(backend, renderer, ctx);   // the slot, the visibility prepass
        {
            // This "Animation" zone spans four phases (Animation, BoneAttachment, SecondaryPhysics,
            // PreRender), not just the animation one: read the Phase.* zones inside it for attribution.
            // The outer name stays for continuity with existing captures.
            ZoneScopedN("Animation");
            PROF_SCOPE("Animation");
            Anim::StageTimer _tAll;
            {
                ZoneScopedN("Phase.Animation");
                Anim::StageTimer _t;
                m_scheduler.RunPhase(TickPhase::Animation,        m_world, frameCtx);
                Anim::AnimProfile::Get().phaseAnimationMs = _t.Ms();
            }
            {
                ZoneScopedN("Phase.BoneAttachment");
                Anim::StageTimer _t;
                m_scheduler.RunPhase(TickPhase::BoneAttachment,   m_world, frameCtx);
                Anim::AnimProfile::Get().phaseBoneAttachmentMs = _t.Ms();
            }
            // -brssplit (default off): publish the STATIC half of the next scene build before
            // SecondaryPhysics. The lanes touch only renderer-owned storage and read-only
            // MaterialComponent data, and JoinStaticSceneEarly comes before PreRender, the first phase
            // that writes material content (FaceShading). Chain physics runs in Animation, so
            // SecondaryPhysics is empty and the split gains ~0.035 ms. No-op with the flag off.
            // TODO(Frame_Job_Graph_Plan M4.3): cross-phase overlap, independent owner work in main's join wait.
            renderer.PrepareStaticSceneEarly(m_world);
            {
                ZoneScopedN("Phase.SecondaryPhysics");
                Anim::StageTimer _t;
                m_scheduler.RunPhase(TickPhase::SecondaryPhysics, m_world, frameCtx);
                Anim::AnimProfile::Get().phaseSecondaryPhysMs = _t.Ms();
            }
            renderer.JoinStaticSceneEarly();
            {
                ZoneScopedN("Phase.PreRender");
                Anim::StageTimer _t;
                m_scheduler.RunPhase(TickPhase::PreRender,        m_world, frameCtx);
                Anim::AnimProfile::Get().phasePreRenderMs = _t.Ms();
            }
            Anim::AnimProfile::Get().phaseAllFourMs = _tAll.Ms();
        }

        // Forward the resolved Live camera to the Renderer: CameraResolveSystem (PreRender) just
        // blended the stack into LiveCameraComponent on the Main channel entity. Fall back to the
        // camera entity's CameraComponent when the channel entity does not exist yet (the first
        // frame, before TickCamera backfills it, or a scene without a camera).
        bool sentCamera = false;
        if (Entity ch = Camera::FindChannelEntity(m_world, Camera::kMainChannel);
            ch != NullEntity)
        {
            if (auto* live = m_world.GetComponent<LiveCameraComponent>(ch))
            {
                RenderCamera rc;
                rc.position = live->position;
                rc.forward  = live->forward;
                // Up from the resolved orientation so banked/rolled cinematic
                // cameras (e.g. MMD camera VMDs) render their roll.
                {
                    const DirectX::XMVECTOR q =
                        DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&live->rotation));
                    DirectX::XMStoreFloat3(&rc.up,
                        DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.f, 1.f, 0.f, 0.f), q));
                }
                rc.fov      = live->fov;
                rc.nearZ    = live->nearZ;
                rc.farZ     = live->farZ;
                rc.historyValid = live->historyValid;
                renderer.SetCamera(rc);
                sentCamera = true;
            }
        }
        if (!sentCamera)
        if (CameraComponent* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
        {
            if (const GlobalTransform* gt =
                    m_world.GetComponent<GlobalTransform>(m_cameraEntity))
            {
                const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&gt->matrix);
                RenderCamera rc;
                DirectX::XMStoreFloat3(&rc.position, world.r[3]);
                DirectX::XMStoreFloat3(&rc.forward,
                                       DirectX::XMVector3Normalize(world.r[2]));
                DirectX::XMStoreFloat3(&rc.up,
                                       DirectX::XMVector3Normalize(world.r[1]));
                rc.fov   = cam->fov;
                rc.nearZ = cam->nearZ;
                rc.farZ  = cam->farZ;
                renderer.SetCamera(rc);
            }
        }

#ifdef WITH_EDITOR
        // Axis-locked ORTHOGRAPHIC editor views (Edit mode only) override the
        // resolved camera at this seam (Editor/EditorCameraController.cpp).
        m_editorCamera.ApplyOrthoView(renderer, m_world, m_editorLayer, mode);
        // Play with the Game View open but hidden: the Scene viewport is the main pipeline's panel and shows
        // the editor camera (the game's cameras are untouched).
        m_editorCamera.ApplyObserverView(renderer, m_world, m_editorLayer.MainViewObservesPlay());
#if defined(WITH_HARNESS)
        // -preview_parity: pin the main camera to the studio rig. LAST, so it
        // wins over both the VCam stack and the ortho override (Editor/PreviewHarness.cpp).
        m_previewHarness.PinParityCamera(renderer);
#endif
#endif

        // Compute the game-side view before the render phase: the UI paint uses the two
        // jitter-free matrices built here (from the camera SetCamera just published and the known
        // viewport size), so it does not wait for renderer.GetView(), which becomes this frame's only
        // inside Renderer::BeginFrame.
        renderer.BuildGameView(ctx.viewportW, ctx.viewportH);

        {
            ZoneScopedN("Render");
            PROF_SCOPE("Render");
            m_scheduler.RunPhase(TickPhase::Render, m_world, frameCtx);

            // CPU frame time closes here, before the PostRender dispatch: PostRender is where the
            // harness records the frame and it needs this frame's number (reading the counter afterwards
            // would lag every recorded cpuMs by one frame), and the figure must exclude the harness's
            // own write cost. So the editor's CPU Frame Time readout excludes PostRender, which is empty
            // unless -artifacts= is given.
            QueryPerformanceCounter(&cpuEnd);
            QueryPerformanceFrequency(&cpuFreq);
            Harness::FrameProbe::Get().cpuFrameMs =
                static_cast<float>(cpuEnd.QuadPart - cpuStart.QuadPart) * 1000.0f
                / static_cast<float>(cpuFreq.QuadPart);

            // The frame-level timing snapshot and the per-frame verify lines
            // (Renderer/FramePipeline.cpp), the HUD's profiler wants (UI/DebugHudFrame.cpp), the
            // PostRender phase and -phaselog_every's breakdown, in this order and inside the Render zone.
            m_pipeline.PublishFrameTiming(backend, renderer, realDt, loopTopMs, m_frameIndex);
            m_debugHud.SyncProfilerWants(backend);
            m_scheduler.RunPhase(TickPhase::PostRender, m_world, frameCtx);
            m_pipeline.LogPhaseCpu();
        }

        // The ONE control-channel service point. Here because the scheduler has
        // flushed its deferred CommandBuffer (so a query that adds or destroys
        // an entity is not racing a structural change mid-phase) and no pass is
        // recording. Never blocks: if the pipe thread holds the slot lock this
        // frame, the command is serviced on the next one.
        if (Harness::ControlChannel::Get().Enabled())
        {
            if (auto ctlQuit = Harness::ControlChannel::Get().Drain(
                    m_world, m_scriptSystem, ctx.frame, [this] { m_pipeline.DrainFrameWork(Diag::DrainCause::Harness); }))
            {
                LOG_INFO("App: control channel requested quit (%d)", *ctlQuit);
                m_quitExitCode = *ctlQuit;
                PostQuitMessage(*ctlQuit);
            }
        }

#ifdef WITH_HARNESS
        // -dump_catalog: once, at the control channel's serial point, from frame
        // 3 on - the render graph has executed (so each pass's `ran` is real)
        // and every Lua binding and system is registered. An -unattended run
        // exits 0 once it is written; a failed write is a script fault.
        if (!catalogDumpDone && ctx.frame >= 3)
        {
            catalogDumpDone = true;
            if (m_pipeline.RenderOwnerEnabled()) m_pipeline.DrainFrameWork(Diag::DrainCause::Harness);   // no pass recording
            const Harness::CatalogSources catalogSrc{ m_world, m_scriptSystem, m_systemRegistry,
                                                      renderer.GetMainGraph(), ctx.frame };
            std::string catalogErr;
            if (Harness::WriteCatalogDump(catalogSrc, catalogDumpDir, catalogErr))
            {
                LOG_INFO("[CATALOG] wrote %s at frame %llu", catalogDumpDir.c_str(),
                         static_cast<unsigned long long>(ctx.frame));
                if (Sys::CommandLine::Get().Has("-unattended")) PostQuitMessage(Harness::Ok);
            }
            else
            {
                LOG_ERROR("[CATALOG] -dump_catalog=%s failed: %s", catalogDumpDir.c_str(), catalogErr.c_str());
                Harness::RunLedger::Get().NoteScriptFault("-dump_catalog", catalogErr.c_str());
                if (Sys::CommandLine::Get().Has("-unattended")) PostQuitMessage(Harness::PreconditionFail);
            }
        }
#endif

#ifdef WITH_EDITOR
        m_editorLayer.SetCPUFrameTime(Harness::FrameProbe::Get().cpuFrameMs);

        // Drain late GPU readbacks, but never apply an edit-mode click in Play.
        // Under -renderowner this runs in the Render phase's parked window
        // instead (see the ProcessPendingActions site): here the lane may be
        // recording, and ResolvePick reads PickingPass state the lane writes.
        if (!m_pipeline.RenderOwnerEnabled())
        {
            // A harness pick (the pipe's `pick` verb) is HarnessCaptureSystem's
            // to resolve: taken here it would become a click and move the
            // selection.
            Entity pickedEntity;
            if (!renderer.IsHarnessPick() && renderer.ResolvePick(pickedEntity) &&
                m_playModeController.IsEditing())
                m_editorLayer.SetPickResult(pickedEntity);
        }
#else
#endif

        // Frame pacing: after the CPU-frame-time capture and the scheduler phase timings, so the
        // editor's CPU Frame Time and the Render phase measure real work, not the frame period;
        // after Present (inside EndFrame) so the sleep overlaps GPU execution; before FrameMark so
        // Tracy frame boundaries are unchanged.
        backend.MainSurface().LimitFrameRate();

        ++ctx.frame;
        m_frameIndex = ctx.frame;   // mirror for Lua's Engine.Frame() / the harness
        Logger::SetFrame(ctx.frame);   // stamped on every log entry from here on
        if (ctx.frame == 1)
        {
            // The first presented frame ends the boot: publish the timeline in the log too, for runs
            // with no run.json (tools/boot_report.py reads the run.json copy).
            Sys::Boot::Mark("firstFrame");
            Sys::Boot::Phase phases[Sys::Boot::kMaxPhases];
            const std::size_t n = Sys::Boot::Snapshot(phases, Sys::Boot::kMaxPhases);
            std::string line;
            for (std::size_t i = 0; i < n; ++i)
            {
                char one[64];
                std::snprintf(one, sizeof(one), "%s%s=%.0f", i ? " " : "", phases[i].name, phases[i].ms);
                line += one;
            }
            LOG_INFO("[BOOT] phases (ms since process creation): %s", line.c_str());
            Sys::LogProcessMemory("first frame");
            Resource::AssetFS::Get().LogStats("first frame");
        }

        // Frame-boundary requests: -play_at's deferred Play, then the -play_frames Stop, then the
        // -frames quit. Play and Stop are latched and fire before the -frames quit: PlayModeController
        // defers a request to its next BeginFrame, so firing it on the exit frame would queue a Stop
        // nothing services. Play is tested first, with its own latch, so a pack that asks for both
        // gets them in order even with the two frames swapped: the Stop then fires on the next frame
        // that satisfies it.
        if (playAtFrame > 0 && !playAtSent &&
            ctx.frame >= static_cast<std::uint64_t>(playAtFrame))
        {
            playAtSent = true;
            LOG_INFO("-play_at=%d reached; requesting Play.", playAtFrame);
            m_playModeController.RequestPlay();
        }
        if (playStopFrame > 0 && !playStopSent && (playAtFrame <= 0 || playAtSent) &&
            ctx.frame >= static_cast<std::uint64_t>(playStopFrame))
        {
            playStopSent = true;
            LOG_INFO("-play_frames=%d reached; requesting Stop.", playStopFrame);
            m_playModeController.RequestStop();
        }

#if defined(WITH_HARNESS)
        // The game-capable -preview_* steps: each pushed into the Renderer once, or fired at its
        // app frame (Renderer/Preview/PreviewHarnessGame.cpp).
        previewHarnessGame.AfterFrame(ctx, renderer);
#endif

#if defined(WITH_HARNESS) && defined(WITH_EDITOR)
        // -preview_material / -preview_parity / -preview_lights_parity: the one-shot
        // editor actions, at a frame boundary after boot (Editor/PreviewHarness.cpp).
        m_previewHarness.AfterFrame();
#endif

        // The frame count a crash or watchdog verdict reports (run.json framesRun).
        Harness::RunLedger::Get().NoteFrameProgress(ctx.frame);

        // The -frames quit. PostQuitMessage only queues WM_QUIT, so the orderly path at the loop
        // top still runs (ControlChannel::Stop, ShutdownAllSystems, SavePSOLibrary). Do not exit()
        // here: it would discard the PSO cache and make every later launch rebuild every PSO from
        // DXIL.
        if (frameBudget > 0 && ctx.frame >= static_cast<std::uint64_t>(frameBudget) + prepareFrames)
        {
            if (prepareFrames > 0)
                LOG_INFO("-frames=%d reached (after %llu frame(s) of PSO preparation); exiting.", frameBudget,
                         static_cast<unsigned long long>(prepareFrames));
            else
                LOG_INFO("-frames=%d reached; exiting.", frameBudget);
            PostQuitMessage(Harness::Ok);
        }

        FrameMark;
    }
}

// ==== Run() helpers: initialisation, in the order Run() calls them ====

void App::BootContent()
{
#if defined(WITH_HARNESS) && defined(WITH_EDITOR)
    // BEFORE any asset is read. A fixture left rewritten by a run that died is
    // exactly the state this boot must not measure.
    Editor::PreviewHarness::RestorePendingRewritesAtBoot();
#endif
    // The content roots and game.ipak are mounted already: main does it before App
    // builds the device (Resource/ContentRoots.cpp).

    // A game host with neither a pak nor a loose tree refuses to boot here, naming
    // both roots it looked in (Resource/ContentRoots.cpp).
    ::Resource::RequireGameContent();
}

std::string App::InitPsoLibrary(IGraphicsDevice& backend)
{
    // PSO disk cache: read from two roots (the per-user write path when it exists, else the
    // shipped one), write to exactly one (System/PsoCachePaths.cpp, -pso_cache_dir included).
    std::string psoCacheWritePath, psoCacheShippedPath;
    Sys::ResolvePsoCachePaths(psoCacheWritePath, psoCacheShippedPath);
    {
        const std::string loadFrom =
            Sys::FileExists(psoCacheWritePath) ? psoCacheWritePath
                                               : psoCacheShippedPath;
        backend.InitPSOLibrary(loadFrom.c_str());
    }
    // Each finished scene load writes what the session has built so far (the earlier scenes'
    // pipelines: a scene's own build at its first draws), so a crash or a killed process does not
    // cost the next launch every PSO again. A load is a moment that may stall.
    EventBus::Get().Subscribe<Scene::SceneLoadedEvent>(
        [&backend, psoCacheWritePath](const Scene::SceneLoadedEvent&)
        {
            backend.CheckpointPSOLibrary(psoCacheWritePath.c_str());
        });
    m_psoWritePath = psoCacheWritePath;
    return psoCacheWritePath;
}

void App::InitCoreSystems(IGraphicsDevice& backend)
{
    m_assetMgr.Init(m_textureSys, m_resourceMgr, backend);
    m_animClipSys.Init(m_resourceMgr);

    // GUID registry: a destroy listener unregisters entities with a GuidComponent when they
    // die. SceneSerializer separately clears and rebuilds it from the world around each scene
    // load.
    ECS::GuidRegistry::Get().RegisterWithWorld(m_world);
    // Save game: record destroyed PersistentTag entities in the GameState graveyard, so
    // "picked up" things stay gone across loads.
    Gameplay::InstallPersistenceHooks(m_world);
    // The project's modules (game.json "modules", read through the content roots BootContent
    // mounted), then their Initialize, before the renderer exists.
    Module::Registry::Get().Configure();
    Module::Context moduleCtx = ModuleContext(nullptr);
    Module::Registry::Get().Initialize(moduleCtx);
}

void App::BootRenderer(IGraphicsDevice& backend, Renderer& renderer)
{
    // ---- Player graphics / quality settings (settings.json) ----
    // Writable, loose, in the PER-USER root (like pso_cache.bin), never sealed in the pak: the
    // player edits it. Loaded before Compile() so shadows.csmResolution sizes the CSM atlas,
    // shadows.pointResolution the point-shadow cubes and textures.anisotropy the material
    // samplers; the live-mutable knobs (frame cap / VSync / AA / render features / render scale /
    // fullscreen / audio bus volumes) are applied below, once every pass exists. Absent file =
    // engine defaults. The load also records what the file supplied in the settings store's
    // Player layer (Settings/SettingsStore.h).
    Resource::LoadBootEngineSettings(m_settings, &backend);   // backend: a first boot's quality levels

    // Seed the input layer from settings: tuning scalars, then the rebind deltas on
    // top of the compiled-in defaults (Resource/EngineSettings.cpp).
    Resource::ApplyInputBindings(m_settings);
    // Font init AFTER settings so uiDefaultFont from settings.json wins; the
    // library prewarm-bakes ASCII, everything else bakes on demand at use.
    InitUIFont(backend);
    Sys::Boot::Mark("uiFont");          // settings + the font library's ASCII prewarm
    renderer.SetShadowResolution(static_cast<uint32_t>(m_settings.csmResolution));
    renderer.SetTextureAnisotropy(static_cast<uint32_t>(m_settings.textureAnisotropy));
    renderer.SetPointShadowResolution(static_cast<uint32_t>(m_settings.pointShadowResolution));
    renderer.Compile();
    Sys::Boot::Mark("renderer");   // passes set up, shaders / PSOs loaded (graphCompile: the graph's half)
    // A .meshlib with no identity of its own gets the one its model's .iscn implies,
    // synthesized from the bytes MeshLibrary::Load already holds: the library is read once, and
    // the identity matches key for key what the backfill tool would write into it.
    m_meshLib.SetIdentityProvider([](const std::string& path, const uint8_t* blob, size_t size) {
        return std::make_shared<const Resource::MeshIdentity>(
            SceneInstanceLoader::SynthesizeMeshLibIdentity(path, blob, size));
    });
    renderer.SetMeshLibrary(&m_meshLib);
    renderer.SetTextureSystem(m_assetMgr.GetTextureSystem());
    renderer.SetResourceManager(m_assetMgr.GetResourceManager());
    // The modules' renderer-side wiring (a chain hook, a device-sized texture), now that the
    // passes and the skinned subsystem exist.
    {
        Module::Context moduleCtx = ModuleContext(&renderer);
        Module::Registry::Get().OnRendererReady(moduleCtx);
    }
    // Live-mutable settings, now that all passes exist (frame cap / VSync /
    // G-Sync / AA mode / render features / render-scale / fullscreen).
    m_settings.ApplyTo(renderer, backend, &wnd);

    // Apply CLI render-path overrides before Lua captures the settings snapshot
    // (-pathtrace, -gipath: Renderer/RenderPathFlags.cpp).
    ApplyRenderPathFlags(renderer);
    // [SETTINGS] proof of life: what the boot writers recorded, per layer.
    Settings::LogBootSummary();

    // engine.json, the DEVELOPER half. Loaded after settings.json applies; the field sets
    // are disjoint, so the order is not load-bearing. AssetFS is already mounted (BootContent),
    // so this resolves from inside game.ipak in a shipped build. Absent file = every developer
    // value keeps its C++ default. The boot load records the file's keys and any -set: on its
    // paths in the developer settings store.
    Resource::LoadBootEngineDevSettings(m_devSettings);
    m_devSettings.ApplyTo(renderer);
    // The ReSTIR backends from the command line - after engine.json, which they
    // override for this run (Renderer/RenderPathFlags.cpp).
    ApplyRestirBackendFlags(renderer);
    Settings::LogDevBootSummary();
    // Boot level for the stat-unit HUD. Not part of ApplyTo: that is the Renderer knob list,
    // and the HUD lives on App.
    m_debugHud.SetLevel(m_devSettings.debugHudLevel);
}

bool App::BootScripting(IGraphicsDevice& backend, Renderer& renderer)
{
    // ---- Engine systems (App-owned; persist across scene switches) ----
    m_scriptSystem.Initialize();
    m_scriptSystem.BindWorld(m_world);
    m_scriptSystem.SetSystemSetActivePredicate(
        [this](std::string_view setName)
        {
            return m_gameFlow.IsSystemSetActive(setName);
        });
    m_scriptSystem.SetSystemSetRegistrar(
        [this](std::string_view setName)
        {
            m_gameFlow.RegisterSystemSet(setName);
        });
    // Bind GameFlow before the boot scripts load so a Lua System file may
    // define the state graph at top level.
    InitLuaBindings(backend, renderer);

    // Boot scripts: the well-known manifest when it exists, else a folder scan
    // (Scripting/BootScripts.cpp). A refusal is a PostQuitMessage, never an early return: the
    // tail of Run() still saves the PSO cache, and nothing below may execute a script chunk.
    const bool bootScriptsRefused = Scripting::RunBootScripts(m_scriptSystem);

#ifdef WITH_HARNESS
    // -exec=<file.lua>: the scenario driver, gated on -artifacts (Harness/ExecScenario.cpp).
    Harness::StartExecScenario(m_scriptSystem, bootScriptsRefused);
#endif
    return bootScriptsRefused;
}

void App::BootScenes(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx,
                     bool bootScriptsRefused)
{
    // ---- Data-driven scene system ----
    // SceneManager owns the scene registry (game.json) and the blocking world-reload path. It
    // routes Scene.Load() through the same transition closures below, handing them the target
    // scene id to wrap in the fade.
    m_sceneManager.Configure(&backend, &renderer, &m_world, &m_assetMgr,
                             &m_animClipSys, &m_scriptSystem,
                             &m_physicsSystem, &m_audioSystem);
    m_sceneManager.SetTransitionHooks(ctx.beginTransition, ctx.requestReplaceMode);
    // Loading-bar seams: the transition polls SceneManager's async asset prewarm
    // for REAL 0..1 progress, and pulls the per-load transition-effect selection
    // (game.json / Lua Scene.Load opts). Both are no-op-safe if never set.
    m_transition.SetLoadPoll([this](float& p)
    {
        if (Gfx::PsoPrepare::Active()) { p = Gfx::PsoPrepare::Progress(); return false; }
        return m_sceneManager.PollActivePreload(p);
    });
    m_transition.SetMessageProvider([] { return Gfx::PsoPrepare::Active() ? Gfx::PsoPrepare::Message() : std::string(); });
    m_transition.SetParamsProvider([this] { return m_sceneManager.TakePendingTransitionParams(); });
    // Resolved through AssetFS, so it comes from the project root when one is
    // open and from the cwd otherwise.
    m_sceneManager.LoadManifest("game.json");

    // Renderer-side wiring both exes need: without it CharacterStateSystem has no
    // AnimationClipSystem, so Lua state-machine clips (Character.AddState / SetState) never load
    // and skinned characters stay in bind pose, and foot IK loses its physics ground-raycast
    // source. Wired before the boot scene loads, so its characters find both systems ready.
    renderer.SetPhysicsSystem(&m_physicsSystem);
    renderer.SetAnimationClipSystem(&m_animClipSys);

    // Before the flow-driven boot below: RegisterTickSystems ends in
    // m_gameFlow.FinalizeDefinitions(), and nothing may ask the flow whether it defines a state
    // before that validates the graph. IsDefined answers from the raw definition map, so asking
    // earlier would accept a state validation is about to quarantine, skip the scene push for a
    // state that can never be entered and leave an empty world for the run.
    // Safe in a world with no scene yet: every OnRegister only EnsurePool()s or installs an
    // entity-destroy listener, and World::Clear leaves listeners in place, so the load that
    // follows cannot unhook one.
    RegisterTickSystems(backend, renderer, ctx);

    // ---- Flow-driven boot (game.json "startup_state") ----
    // true = the state was accepted and its on_enter owns the first Scene.Load, so the
    // startup_scene push below is skipped (Gameplay/StartupState.cpp).
    const bool flowOwnsBootScene =
        Gameplay::RequestStartupState(m_gameFlow, m_sceneManager.StartupState());


    // -control: open the live control channel. Opt-in per run, never implicit: the pipe is not
    // read-only (see ControlChannel.h), and a dev build must not open one by surprise. Not gated
    // on -artifacts= like -exec: the point is attaching to a session launched by hand; the build
    // gate and the explicit flag carry the weight. Opened before the boot scene loads, so a
    // 20-second load answers `status` with stage "loading_scene: <path>" instead of looking dead.
    // Main-thread verbs still wait for the first frame boundary.
    if (Sys::CommandLine::Get().Has("-control"))
        Harness::ControlChannel::Get().Start();

    // Initial scene: game.json's startup_scene, -scene=<name-or-path> over it, or the
    // base scene - skipped when the boot scripts were refused or the startup state
    // owns the first world. Scene/BootScene.cpp, with every refusal and its reason.
    // While the boot precompile runs, the boot scene loads behind the loading screen once it is done;
    // a player's Game.exe loads it there too (the window keeps answering through a long load).
    const bool pipelinesPreparing = Gfx::PsoPrepare::Active();
    Scene::ActivateBootScene(m_sceneManager, bootScriptsRefused, flowOwnsBootScene,
                             pipelinesPreparing || Scene::BootThroughTransition()
                                 ? ctx.beginTransition
                                 : std::function<void(const std::string&)>{},
                             pipelinesPreparing);

    // -profile*: the engine profiler; a malformed or UNKNOWN -profile* flag refuses
    // the run before a frame is drawn (Diagnostics/ProfilerBoot.cpp).
    Diag::Prof::StartFromCommandLine(backend);
}

void App::InstallWindowHooks()
{
    // ---- App's subscriptions to the window (both exes) ----
    // Each is a Plat::Subscription in m_windowSubs: no static slot, no file-static for a handler
    // to reach back through, no install order to keep. The event handlers run at DispatchEvents
    // and the close handlers at Pump, both at the loop top and never inside the WndProc, so they
    // may publish to the EventBus.

    // 1. The window's notifications.
    m_windowSubs.push_back(wnd.AddEventHandler([this](const Plat::Event& e)
    {
        switch (e.type)
        {
        // Alt-tab, published as an event so anything (audio ducking, a pause
        // state, a Lua handler) can react. The window dedupes the edge and
        // seeded its latch at construction, so the first edge is a real one.
        case Plat::Event::Type::FocusChanged:
            EventBus::Get().Publish(Sys::AppFocusChangedEvent{ e.focused });
            LOG_INFO("App: application %s the foreground", e.focused ? "gained" : "lost");
            ApplyBackgroundPolicy(e.focused);
            break;
        // The machine slept or woke: a resume is wall time the game did not run, and the log is on disk
        // before the machine goes.
        case Plat::Event::Type::PowerChanged:
            LOG_INFO("App: the machine is %s", e.suspending ? "suspending" : "resuming");
            if (e.suspending) Logger::Get().FlushBlocking();
            else              m_modalLoopEnded = true;
            break;
        // Controller hot-plug: the next Input::Update re-probes every XInput slot instead of
        // waiting for its throttled round-robin rescan.
        case Plat::Event::Type::DevicesChanged:
            Input::Get().OnDeviceChange();
            // A camera plugged in or out: a module enumerates its devices again.
            Module::Registry::Get().OnDeviceChange();
            break;
        case Plat::Event::Type::ModalLoopEnded:
            m_modalLoopEnded = true;   // the dt clamp's log line names the cause
            break;
#ifdef WITH_EDITOR
        // Files dragged in from Explorer: queued and routed by the editor's next pending actions
        // (a model onto its tile: its reimport; else the Import Model dialog; images: the texture
        // review).
        case Plat::Event::Type::FilesDropped:
            m_editorLayer.QueueDroppedFiles(e.paths, e.x, e.y);
            break;
#endif
        default:
            break;
        }
    }));

    // The session ends (logoff, shutdown): the process is terminated when the WndProc returns from
    // WM_ENDSESSION, so what must survive is saved there - the PSO library (a first launch's pipelines
    // cost seconds) and, through the window, the log. The editor holds a logoff back for unsaved work.
    m_windowSubs.push_back(wnd.AddSessionEndHandler([this]
    {
        if (m_gfx && !m_psoWritePath.empty())
        {
            m_gfx->SavePSOLibrary(m_psoWritePath.c_str());
            LOG_INFO("[SESSION] saved the PSO library before the session ends (%s)", m_psoWritePath.c_str());
        }
    }));
#ifdef WITH_EDITOR
    m_windowSubs.push_back(wnd.AddSessionQuery([this]() -> std::wstring
    {
        return m_editorLayer.HasUnsavedChanges() ? std::wstring(L"The editor has unsaved scene changes") : std::wstring();
    }));
#endif
    // Main waits for the render-owner lane in the drains: the window keeps answering while it does.
    m_pipeline.SetWaitPump([this] { return wnd.PumpWhileWaiting(); });

    // 2. The game Canvas's capture predicates, for both exes. The editor's ImGui
    //    registers its own (EditorLayer::InitImGuiBackends); the window ORs them.
    m_windowSubs.push_back(wnd.AddCapturePredicate(Plat::CaptureKind::Mouse,
        &UI::UICanvasSystem::GlobalWantsCaptureMouse));
    m_windowSubs.push_back(wnd.AddCapturePredicate(Plat::CaptureKind::Keyboard,
        &UI::UICanvasSystem::GlobalWantsCaptureKeyboard));
    // The game's IME opens only while a Canvas text field wants the keyboard; the editor's ImGui
    // manages its own.
    wnd.ManageImeContext(!Sys::IsEditorHost());

    // 3. Title-bar X / Alt+F4 / the taskbar's Close / the editor's Exit item.
    //    The window swallows WM_CLOSE and runs these at the next Pump, in order;
    //    the first Veto keeps it open.
    //
    //    First the game's quit-confirm screen, for both exes: a pass-through unless the
    //    project defines the state engine.json's gameFlow.quitConfirmState names.
    m_windowSubs.push_back(wnd.AddCloseHandler(0,
        [this, state = m_devSettings.gameFlowQuitConfirmState]() -> Plat::CloseVerdict
    {
        // Exactly one answer is a veto: a request the machine accepted. Every other outcome (no
        // such state, refused by a guard, PendingOccupied because another switch owns this frame)
        // closes, because the alternative's failure mode is a window that cannot be shut. Already
        // being in the confirmation state closes too: the second X is the player insisting.
        if (!state.empty() && m_gameFlow.IsDefined(state) && !m_gameFlow.IsInState(state)
            && m_gameFlow.TryRequestSwitch(state) == Gameplay::SwitchResult::Accepted)
        {
            // TryRequestSwitch only queues (the pure guards and a pending target);
            // GameFlowResolveSystem performs the transition in this frame's Input phase. The state
            // quits later (Engine.Quit() from its "Yes").
            LOG_INFO("WM_CLOSE swallowed — handed to GameFlow state '%s'", state.c_str());
            return Plat::CloseVerdict::Veto;
        }
        return Plat::CloseVerdict::Allow;
    }));
#ifdef WITH_EDITOR
    //    Then the editor: a dirty scene raises the Save / Discard / Cancel modal,
    //    answered later through ConsumeExitApproved() in the main loop.
    m_windowSubs.push_back(wnd.AddCloseHandler(10, [this]() -> Plat::CloseVerdict
    {
        return m_editorLayer.RequestExit() ? Plat::CloseVerdict::Allow : Plat::CloseVerdict::Veto;
    }));
    wnd.AcceptFileDrops(true);
#endif
}

void App::ApplyBackgroundPolicy(bool focused)
{
    // The pause is ours alone: not a script's (Lua's reload does not know it) and not the menu key's.
    constexpr std::uint64_t kFocusPauseOwner = Gameplay::GameFlowStateMachine::kHostPauseOwner - 1;
    Plat::BackgroundState state;
    state.focused = focused;
    const Plat::BackgroundDecision d = Plat::DecideBackground(
        Plat::CurrentBackgroundPolicy(), state, Plat::Window::BackgroundHostFor(Harness::ControlChannel::Get().Enabled()));

    m_audioEngine.SetBackgroundMute(d.mute);
    if (d.pause && m_focusPause == 0)
    {
        m_focusPause = m_gameFlow.RequestPause("application lost the foreground", kFocusPauseOwner);
        LOG_INFO("[BACKGROUND] paused: the application lost the foreground (pause %llu)",
                 static_cast<unsigned long long>(m_focusPause));
    }
    else if (!d.pause && m_focusPause != 0)
    {
        m_gameFlow.ReleasePause(m_focusPause);
        LOG_INFO("[BACKGROUND] resumed: the application has the foreground again");
        m_focusPause = 0;
    }
    LOG_INFO("[BACKGROUND] focus=%d mute=%d pause=%d", focused ? 1 : 0, d.mute ? 1 : 0, d.pause ? 1 : 0);
}

void App::WirePlayMode(IGraphicsDevice& backend, Renderer& renderer)
{
    // ---- Edit/Play world snapshot and restore (state isolation) ----
    // EnterPlay snapshots the pristine editor world, ExitPlay restores it in place
    // (ECS/PlayModeSession.cpp, with the teardown order and its reasons). The editor tears its
    // preview stages down before the snapshot.
#ifdef WITH_EDITOR
    m_playSession.beforeSnapshot = [this](Renderer& renderer, World& world)
    {
            // A UI rect drag still held is committed while undo still records (the controller
            // enters Play after this), before the snapshot takes its value.
            m_editorLayer.CommitGesturesForPlay();
            Editor::ChainPhysicsPreviewWindow::Get().TearDownForPlay(&renderer, &world);
            // The Skeleton Editor's Animation-host stage (it shows the reference pose until Stop).
            Editor::SkeletonEditorWindow::Get().TearDownForPlay(&renderer, &world);
            // The UI Editor's asset stages: detached with their ids parked, attached again after
            // Stop.
            Editor::UIEditorWindow::Get().TearDownForPlay(&renderer, &world);
    };
#endif
    m_playModeController.onEnterPlay = [this, &renderer]() { m_playSession.EnterPlay(renderer); };
    m_playModeController.onExitPlay  = [this, &renderer, &backend]() { m_playSession.ExitPlay(renderer, backend); };

    // Boot mode: the editor starts in Edit (no simulation until Play); Game builds have no
    // edit phase and always simulate.
#ifndef WITH_EDITOR
    m_playModeController.SetMode(EngineMode::Playing);
#endif

    // -play: enter Play from the command line. Only meaningful for an editor host (Game.exe
    // is already Playing, set above); it makes a scenario runnable in Editor.exe at all, since
    // ScriptLogicSystem is RunMask::Play and the editor otherwise boots into Edit. A runtime
    // query (Sys::IsEditorHost(), the sanctioned test), not #ifdef.
    if (Sys::CommandLine::Get().Has("-play") && Sys::IsEditorHost())
    {
        // -play_at=<N> holds the request back to frame N, so a harness fixture spawned in Edit
        // mode lands in the snapshot (see the -play_at block in Run()).
        // RequestPlay, not SetMode: SetMode assigns the mode and skips onEnterPlay, so no
        // editor-world snapshot is cloned, and a later Stop hits the `!m_editorWorldSnapshot`
        // bail-out and silently skips every teardown step behind it (ClearInstances included). The
        // request costs one frame of Edit mode and makes -play mean what clicking Play means.
        if (Sys::CommandLine::Get().GetInt("-play_at", 0) > 0)
        {
            LOG_INFO("App: -play - Play deferred to frame %d by -play_at",
                     Sys::CommandLine::Get().GetInt("-play_at", 0));
        }
        else
        {
            LOG_INFO("App: -play - entering Play mode at boot");
            m_playModeController.RequestPlay();
        }
    }
}

void App::RegisterResourceLoaders(IGraphicsDevice& backend)
{
    // Internal format -> Resource object.
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::TextureLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::ShaderLoader>(&backend));
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::MaterialLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::AnimationLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::AnimGraphLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Resource::AnimSetLoader>());
    m_resourceMgr.RegisterLoader(std::make_shared<Audio::AudioClipLoader>());

#ifdef WITH_EDITOR
    // External source -> internal blob.
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::TextureImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::ShaderImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::MaterialImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::AnimationImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Resource::VmdImporter>());
    m_resourceMgr.RegisterImporter(std::make_shared<Audio::AudioImporter>());
#endif
}

void App::InitUIFont(IGraphicsDevice& backend)
{
    // The default font comes from settings.json (uiDefaultFont); FGMiraiRen ships with the
    // engine as the fallback. FontLibrary owns every Font: UI elements pick theirs via fontPath,
    // and glyphs bake on demand.
    constexpr const char* kDefaultTTF = "engine/font/FGMiraiRen.ttf";
    const std::string ttf = m_settings.uiDefaultFont.empty()
                          ? std::string(kDefaultTTF) : m_settings.uiDefaultFont;
    if (!UI::FontLibrary::Get().Init(backend, ttf))
    {
        // Bad settings path? Retry the shipped default before giving up.
        if (ttf != kDefaultTTF && UI::FontLibrary::Get().Init(backend, kDefaultTTF))
            LOG_WARNING("uiDefaultFont '%s' failed — using %s", ttf.c_str(), kDefaultTTF);
        else
            LOG_WARNING("UI font init failed — text rendering disabled");
    }
}

void App::InitLuaBindings(IGraphicsDevice& backend, Renderer& renderer)
{
    sol::state* lua = m_scriptSystem.GetLua();
    if (!lua) return;

    AI::RegisterLuaBTBindings(*lua);
    Intent::RegisterLuaIntentBindings(*lua, m_world);
    Intent::RegisterLuaAIBindings(*lua, m_world, m_aiLODSystem, m_perceptionSystem);
    DX12Physics::RegisterLuaPhysicsBindings(*lua, m_physicsSystem, m_world);
    // Hitbox.IsActive - after the Physics bindings, which create the Hitbox table
    // (ECS/LuaHitboxBindings.cpp).
    RegisterLuaHitboxBindings(*lua, m_world, m_systemRegistry);
    renderer.GetAnimGraphSystem()->Init(m_resourceMgr);
    RegisterLuaAnimGraphBindings(*lua, m_world, *renderer.GetAnimGraphSystem());
    RegisterLuaCharacterStateBindings(*lua, m_world, m_animClipSys);
    // Face.*: morph-target (blend shape) control. Bound here, not in a world-only binding
    // file, because resolving morph names needs the SkeletonRegistry, which lives on the
    // Renderer (as for ChainLOD below).
    {
        Renderer* r = &renderer;   // Morph.GetPoolWeights reads what BuildSkinJobs wrote
        RegisterLuaFacialBindings(*lua, m_world, renderer.GetSkeletonRegistry(), &renderer.GetMorphClipLibrary(),
            [r](Entity e, std::vector<std::pair<uint16_t, float>>& out) { return r->ReadMorphPoolWeights(e, out); });
    }
    // Skeleton.* - bones by name and the sockets (the model's and the entity's);
    // the registry is the Renderer's, as for Face.*.
    RegisterLuaSkeletonBindings(*lua, m_world, renderer.GetSkeletonRegistry(), &renderer.GetPoseRingBuffer());
    RegisterLuaPlayerBindings(*lua, m_world);
    RegisterLuaIKBindings(*lua,m_world);
    RegisterLuaSequenceBindings(*lua, m_world);
    PostProcess::RegisterLuaPostProcessBindings(*lua, m_world);
    Audio::RegisterLuaAudioBindings(*lua, m_world);
    // Entity.*: get / find / schema on every serializable component and set on the
    // allow-listed ones, through the same registry .iscene save uses.
    RegisterLuaEntityBindings(*lua, m_world);
    // Tags.*: the gameplay tags of an entity (Scripting/LuaGameplayTagBindings.cpp).
    RegisterLuaGameplayTagBindings(*lua, m_world);
    // Attr.*: the attributes of an entity (Scripting/LuaAttributeBindings.cpp).
    RegisterLuaAttributeBindings(*lua, m_world);
    // DataTable.*: schema'd data tables (Scripting/LuaDataTableBindings.cpp).
    RegisterLuaDataTableBindings(*lua);

    // ChainLOD.* - chain-physics distance LOD (Physics/LuaChainLODBindings.cpp).
    RegisterLuaChainLODBindings(*lua, renderer.GetChainPhysicsSystem());

    // Preview.* - the in-game picture-in-picture (Renderer/Preview/LuaPreviewBindings.cpp).
    Preview::RegisterLuaPreviewBindings(*lua, renderer, m_world);

    // Debug.* - the stat-unit HUD's control + readout surface (Diagnostics/LuaDebugBindings.cpp).
    Diag::RegisterLuaDebugBindings(*lua, renderer, m_debugHud, m_audioEngine, m_audioClipSys, *m_gfx);
#ifdef WITH_EDITOR
    // Debug.SetDraw: the editor's debug-draw toggles, over the Game stub the call above
    // registered (Editor/LuaDebugDrawBindings.cpp). Here, not in WireEditor: a boot script
    // must already see the editor's version.
    Editor::RegisterLuaDebugDrawBindings(*lua, m_debugDraw);
#endif

    // ---- Render.explain: why an entity looks the way it does ----
    // Registered here because App holds both the Renderer and DrainFrameWork, which the query
    // needs before it reads what the scene build writes (Renderer/RenderExplain.h).
    RenderExplain::RegisterLua(lua->lua_state(), m_world, renderer,
                               [this] { m_pipeline.DrainFrameWork(Diag::DrainCause::Harness); });
    // Asset.*: what the running engine holds of an asset - loaded, ready or why not, and the
    // entities it is resolved for (Renderer/AssetQuery.h).
    AssetQuery::RegisterLua(lua->lua_state(), m_world, renderer, m_assetMgr,
                            [this] { m_pipeline.DrainFrameWork(Diag::DrainCause::Harness); });
    // Render.targets / Render.target: the main view shows one render target instead of the
    // lit image (Renderer/DebugTargets.h).
    DebugTargets::RegisterLua(lua->lua_state(), renderer,
                              [this] { m_pipeline.DrainFrameWork(Diag::DrainCause::Harness); });
    // Control.start / stop / state: open the control channel in a running
    // engine - the editor's Tools > AI Control Channel does the same
    // (Harness/ControlChannel.h; absent in Retail).
    Harness::ControlChannel::RegisterLua(lua->lua_state());
    // help("Entity.set") / help("Entity"): what a described binding IS - its
    // types deduced from the C++, its meaning, effect and errors (Scripting/LuaDoc.h).
    LuaDoc::RegisterHelp(lua->lua_state());

    // Profiler.* - the engine profiler's runtime surface (Diagnostics/LuaProfilerBindings.cpp).
    Diag::RegisterLuaProfilerBindings(*lua, &backend);
    // The enabled modules' tables (FaceTracking.*, ...) and Engine.hasModule / Engine.modules.
    {
        Module::Context moduleCtx = ModuleContext(&renderer);
        moduleCtx.lua = lua;
        Module::Registry::Get().RegisterLua(moduleCtx);
        Module::RegisterLuaBindings(*lua);
    }
    // The flow goes in so Engine.SaveGame records which state the player was
    // in and Engine.LoadGame queues putting them back there.
    Scene::RegisterLuaSceneBindings(*lua, m_sceneManager, &m_gameFlow);
    // Settings.*: the player-settings surface for an in-game options menu. Registered here,
    // the only place holding Renderer, IGraphicsDevice and Window (EngineSettings::ApplyTo's
    // signature). Must come after the boot ApplyTo (BootRenderer), since the bridge seeds its
    // "applied" snapshot with a CaptureFrom.
    Resource::RegisterLuaSettingsBindings(*lua, renderer, *m_gfx, &wnd);
    // Platform.*: achievements, stats, presence, the save folder (the active store's, else the local one).
    Plat::RegisterLuaPlatformBindings(*lua);
    Gameplay::RegisterLuaGameFlowBindings(*lua, m_gameFlow, m_world,
                                           m_sceneManager, m_systemRegistry,
                                           m_scriptSystem);
    m_aiSystem.Init(lua);

    // VFX.SpawnAfterimage (Graphics/LuaVFXBindings.cpp). The drain is App's: a sync-point
    // call that cannot be deferred drains the frame work first.
    const auto drainFrameWork = [this](Diag::DrainCause cause) { m_pipeline.DrainFrameWork(cause); };
    RegisterLuaVFXBindings(*lua, m_world, renderer, drainFrameWork);

    // Engine.*: functions on the table ScriptSystem::RegisterBindings creates (Initialize()
    // runs first), each registered by the module it binds. Engine.Quit and Engine.Frame read
    // App's own state (the quit latch, the frame mirror), so they are bound here.
    Resource::RegisterLuaPrefabBindings(*lua, m_world, m_assetMgr, renderer, m_animClipSys);   // SpawnPrefab
    RegisterLuaCameraViewBindings(*lua, renderer);                     // GetCameraPosition / GetCameraForward
    RegisterLuaDDGIBindings(*lua, renderer, drainFrameWork);           // DDGIWatchProbe / DDGIWatchedProbe
    RegisterLuaPlayerEngineBindings(*lua, m_world, m_cameraEntity);    // GetPlayer
    UI::RegisterLuaUIEngineBindings(*lua, m_world, m_uiCanvasSystem, m_uiInput, m_assetMgr, renderer,
                                    m_animClipSys);                    // SpawnUIPrefab / ResolveUILayout / GetUIPointer
    RegisterLuaWaterRippleBindings(*lua, renderer, drainFrameWork);    // RippleAt
    {
        sol::table engineTbl = (*lua)["Engine"];

        // Engine.Quit(): request application exit. Latches; App::RenderPhase drains it at the
        // frame boundary and calls PostQuitMessage. Script has no other way out of the process: `os`
        // is not opened, and Window's only close-related public API is the veto hook.
        // The optional int is a suggestion: RunLedger::Finalize may raise it (device lost, error
        // budget, a failed expectation) after the logger drains, so a scenario that calls
        // Engine.Quit(0) after a failed assertion still exits non-zero. sol::optional<int> keeps the
        // zero-argument call (asset/scripts/scenes/Title.lua) working.
        engineTbl.set_function("Quit", [this](sol::optional<int> code)
        {
            m_quitRequested = true;
            m_quitExitCode  = code.value_or(static_cast<int>(Harness::ExitCode::Ok));
        });

        // Engine.Frame(): frames fully presented so far. Reads App::m_frameIndex, not
        // GameModeContext::frame: ctx is a Run() local declared after InitLuaBindings runs, so it
        // cannot be captured here. Time.elapsed would be worse: Time.dt / elapsed are written only
        // inside ScriptSystem::Update (RunMask::Play), so they freeze in Edit / Paused while frames
        // advance.
        LuaDoc::Fn(engineTbl, "Engine", "Frame", [this]() -> std::uint64_t { return m_frameIndex; },
            // Shipped in every build, Retail too: no harness file names here
            // (tools/strip_gate.py looks for them in the Retail binary).
            { .doc = "The index of the frame being simulated - the numbering the harness records and the control channel use." });
    }

    // The BT Actions / Conditions global libraries load with the other boot scripts in
    // BootScripting (the script manifest, else the folder scan), which runs after
    // RegisterLuaBTBindings / RegisterLuaNavBindings above.
}

Module::Context App::ModuleContext(Renderer* renderer)
{
    Module::Context ctx;
    ctx.world     = &m_world;
    ctx.gfx       = m_gfx.get();
    ctx.renderer  = renderer;
    ctx.systems   = &m_systemRegistry;
    ctx.physics   = &m_physicsSystem;
    ctx.resources = &m_resourceMgr;
    ctx.assets    = &m_assetMgr;
    return ctx;
}

void App::InitAudio()
{
    // AudioSystem installs an entity-destroy listener so component teardown
    // stops live voices and releases clip refcounts.
    m_audioClipSys.Init(m_resourceMgr);
    if (m_audioEngine.Initialize())
    {
        m_audioSystem.BindEngine(m_audioEngine);
        m_audioSystem.BindClipSystem(m_audioClipSys);
        m_audioSystem.BindWorld(m_world);
        m_audio3DSystem.BindEngine(m_audioEngine);
    }
}

#ifdef WITH_EDITOR
void App::WireEditor(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx)
{
    m_editorLayer.SetGraphicsDevice(&backend);
    m_editorLayer.SetResourceSystems(&m_textureSys, &m_resourceMgr);
    m_editorLayer.SetAssetManager(&m_assetMgr);
    m_editorLayer.SetRenderer(&renderer);
    m_editorLayer.SetWindow(&wnd);            // Graphics Settings window: fullscreen apply
    m_editorLayer.SetAnimationClipSystem(&m_animClipSys);
    m_editorLayer.SetAnimationSystem(renderer.GetAnimationSystem());
    m_editorLayer.SetPhysicsSystem(&m_physicsSystem);
    // The renderer's SetPhysicsSystem / SetAnimationClipSystem are wired in BootScenes, for
    // both exes.
    m_editorLayer.SetAISystem(&m_aiSystem);
    m_editorLayer.SetScriptSystem(&m_scriptSystem);   // exposed-var inspector
    m_editorLayer.SetDebugDrawSystem(&m_debugDraw);   // debug category toggles
    // The boot scene is already loaded (BootScenes runs before WireEditor), so hand the
    // editor its .ippc and scene-script bindings, or the first File > Save Scene would drop
    // them, and its .iscene path, or the first Ctrl+S degrades into a Save As dialog.
    m_editorLayer.AdoptLoadedScene(m_sceneManager.CurrentPostProcessPath(),
                                   m_sceneManager.CurrentSceneScript(),
                                   m_sceneManager.CurrentSceneSystems(),
                                   m_sceneManager.CurrentScenePath());
    // Hand the editor toolbar the authoritative Edit/Play state machine.
    m_editorLayer.SetPlayModeController(&m_playModeController);
    m_editorLayer.RegisterDefaultEditors();   // must be after SetRenderer()
    // The project's content root. The bare "asset/" when no -project was given, so the
    // default single-root layout produces byte-identical paths: the browser enumerates them and
    // they end up in .iscene files, where a cosmetic "./" would show in saved references.
    m_editorLayer.SetAssetDirectory(
        Sys::ProjectDir() == "." ? std::string("asset/")
                                 : Sys::ProjectDir() + "/asset/");
    m_editorLayer.SetGPUProfiler(&static_cast<GraphicsDX12&>(backend).GetGPUProfiler());
    // After SetGraphicsDevice and SetWindow: ImGui's message filter and capture
    // predicates subscribe to the window here. The close handler (a dirty scene's
    // Save / Discard modal) and dropped files are App's subscriptions
    // (InstallWindowHooks).
    m_editorLayer.InitImGuiBackends();

    m_editorLayer.SetCreateMeshCallback([&ctx](int meshType, const DirectX::XMFLOAT3& pos) {
        if (!ctx.world) return;
        MeshSpawner::Spawn(meshType, *ctx.world, pos);
    });

    m_editorLayer.SetPickCallback([this, &renderer, &ctx](float px, float py) {
        if (!m_playModeController.IsEditing()) return;
        if (ctx.viewportW == 0 || ctx.viewportH == 0) return;
        renderer.RequestPick(px, py);
    });

    // Last, because these need everything wired above.
    Harness::ControlChannel::Get().SetEditorHandler(
        [this](const std::string& command) { return m_editorLayer.ExecuteEditorCommand(command); });
    m_editorLayer.InstallEntityWriteObserver();   // Entity.set -> undo + dirty
    // The Lua `Editor` table: scripted authoring through the editor's undo, selection and
    // dirty state. After the wiring above: it needs the world.
    if (sol::state* editorLua = m_scriptSystem.GetLua())
        m_editorLayer.RegisterAuthoringLua(editorLua->lua_state());

    // The editor's part of the frame (Editor/EditorFrameHooks.h); before Configure.
    m_pipeline.SetEditorHooks(&m_editorFrameHooks);
}
#else
void App::WireEditor(IGraphicsDevice&, Renderer&, GameModeContext&) {}
#endif

void App::RegisterTickSystems(IGraphicsDevice& backend, Renderer& renderer,
                               GameModeContext& ctx)
{
    // Single source of tick order for the ECS scheduler (see
    // docs/design/System_Scheduler_Architecture.md). Order INSIDE a phase is
    // registration order; phases run sequentially per kPhaseDescriptors.
    auto& reg = m_systemRegistry;

    // Fixed frame-start transition point. Input has no other registered systems; its
    // phase-boundary CommandBuffer flush destroys every exited StateScoped entity before
    // GameplayPreLogic begins.
    reg.Add<Gameplay::GameFlowResolveSystem>(m_gameFlow);

    // GameplayPreLogic: LifetimeSystem first, so expired VFX die before scripts / AI iterate;
    // notify-spawned emitters from the previous frame's TimelineSystem all expire here. (The
    // asynchronous prefab spawns commit just before this phase, in App::Run.)
    reg.AddInSet<LifetimeSystem>(Sets::kGameplayFX);
    // Dissolve amount lerp, before the Render phase reads the component, so the on-screen
    // value is this frame's (RunMask::Play; Write<Dissolve> only).
    reg.AddInSet<DissolveSystem>(Sets::kGameplayPresentation);
    // Projectiles move in GameplayPostLogic so LocalToWorld (Animation) turns
    // the new pose into GlobalTransform before HitboxSystem (BoneAttachment)
    // reads it -- the damage volume rides this frame's position, not last
    // frame's. Needs the PhysicsSystem for its swept block test.
    reg.AddInSet<ProjectileSystem>(Sets::kGameplaySimulation, &m_physicsSystem);
    // The real-time services (Time.realDt, timed requests, real timers and tasks, an async spawn's
    // onReady, the playtime) belong to no set: a state without GameplayScripts must not stop them.
    // -timers_legacy puts the system back in the set.
    if (Scripting::LegacyTimers())
        reg.AddInSet<ScriptTimerSystem>(Sets::kGameplayScripts, m_scriptSystem);
    else
        reg.AddAlwaysRun<ScriptTimerSystem>(m_scriptSystem);

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -cmdbuftest: two declared systems that share one parallel batch and recycle a tagged
    // population through the CommandBuffer, the determinism fixture for its replay order.
    if (Sys::CommandLine::Get().Has("-cmdbuftest"))
    {
        reg.Add<CmdBufTestSystem>(0u);
        reg.Add<CmdBufTestSystem>(1u);
    }
#endif

    // GameplayLogic.
    // Deliberately ungated: this one C++ system dispatches every Lua System and every entity
    // Logic slot, so a set excluding it would stop Lua entirely, including the flow controllers
    // that issue GameFlow.Switch, and nothing could switch back out. Lua is gated per script
    // instead: a System table's `set = "..."` (ScriptSystem::SystemEntry) and a Logic template's
    // `T.set` (ScriptSystem::LogicTemplate), both checked inside Update.
    reg.AddAlwaysRun<ScriptLogicSystem>(m_scriptSystem);

    // Per-state on_update + timeout, AFTER the scripts that feed them so a
    // state's own logic sees this frame's script writes. Ungated for the same
    // reason as ScriptLogicSystem: it is part of how the flow drives itself, and
    // a state whose set is suspended must still be able to tick its way out.
    reg.AddAlwaysRun<Gameplay::GameFlowTickSystem>(m_gameFlow);

    // Harness per-frame record, registered unconditionally in every harness exe: inert (no
    // file, no formatting, no allocation) unless -artifacts= is given. Never behind #ifdef
    // WITH_EDITOR: App.cpp is compiled per exe, and the harness is exactly what a Game.exe run
    // needs.
#ifdef WITH_HARNESS
    {
        auto& capture = reg.Add<Harness::HarnessCaptureSystem>(renderer, backend);
        m_pipeline.SetHarnessCapture(&capture);
        // The capture flags get the same refusal as every -preview_* flag: the system reports a
        // refused -screenshot2 / -at_frame2 configuration and the exit happens here, on the main
        // thread, so a pack asserting on the second capture never runs with it unarmed.
        if (capture.ConfigRefused())
        {
            LOG_ERROR("The capture configuration was refused (see the "
                      "-screenshot2 / -at_frame2 error above). Refusing to run: "
                      "a pack that asserts on the second capture would otherwise "
                      "measure a run in which it was never armed.");
            PostQuitMessage(Harness::PreconditionFail);
        }
        // The streaming upload probe's writers (TickPhase::PreRender, after the
        // Animation phase's AcquireUploadSlot, and GameplayPreLogic, before
        // it), only when -streamupload_probe asks for them: registered in every
        // run each would be a batch of its own in every run's plan.
        if (Harness::StreamUploadProbe::Requested())
        {
            reg.Add<Harness::StreamUploadProbeSystem>(backend);
            reg.Add<Harness::StreamUploadProbeEarlySystem>(backend);
        }
        // -prefab_prewarm=<path>@<frame>: spawns the prefab once at that frame and keeps it, the
        // reverse arm of prefab_spawn_cost. Only when asked for, like the two probes above.
        if (Harness::PrefabPrewarmSystem::Requested())
            reg.Add<Harness::PrefabPrewarmSystem>(m_assetMgr, renderer, m_animClipSys);
        // -test_remove_device_after_ms=N: the device is removed N ms in (the device-lost path, run for real).
        Harness::ArmDeviceLossProbe(backend);
    }
#endif

    // AI phase: LOD -> Perception -> BT -> NavAgent -> Player -> CharacterMotor (layered
    // movement model, docs/design/character_movement_architecture.md):
    //   * BT writes NavAgentComponent fields via `Intent.MoveTo`.
    //   * NavAgentSystem paths to NavAgent.destination over the NavMesh and writes
    //     CharacterIntentComponent (LocalTransform directly for an entity without a character
    //     controller).
    //   * PlayerController writes the same intent for the player (camera-relative WASD).
    //   * CharacterMotor turns the intent into the character controller's drive fields.
    //   * The KCC step inside PhysicsStepSystem (next phase) does the sweep-and-slide via
    //     Jolt CharacterVirtual.
    reg.AddInSet<AILODTickSystem>(Sets::kGameplaySimulation, m_aiLODSystem);
    // Perception BEFORE the trees: a tree asking "can I see the player"
    // should read this frame's sighting, not last frame's.
    reg.AddInSet<PerceptionTickSystem>(Sets::kGameplaySimulation, m_perceptionSystem,
                                       m_physicsSystem);
    reg.AddInSet<AIBTTickSystem>(Sets::kGameplaySimulation, m_aiSystem, m_scriptSystem);
    // AITacticalTickSystem is not registered: the strategic AIGoal layer has no users
    // (every BT writes Intent.MoveTo directly) and enemy positioning lives in Lua
    // (PrepareAttack). Register it here to revive the goal layer.
    // The Nav module's NavAgentSystem goes here (AddInSetBefore the player brain).
    reg.AddInSet<PlayerControlTickSystem>(Sets::kGameplaySimulation, m_playerCtrl);
    // CharacterMotor, the single intent consumer. Must be last in the AI phase: every brain
    // (NavAgent steering, player input) has written CharacterIntentComponent by now; the motor
    // turns it into the character controller's drive fields + facing, the same whoever is
    // possessing.
    reg.AddInSet<CharacterMotorTickSystem>(Sets::kGameplaySimulation);

    // GameplayPostLogic: ProjectileSystem (registered above) and the Sequencer below. The
    // pending scene swap is not a system: App::Run drains it inline because it needs the live
    // GameModeContext.

    // Sequencer, the cinematic evaluator. Here because this is the only gameplay phase the
    // scheduler keeps SERIAL (Spawnables call CreateEntity / DestroyEntity directly), and it must
    // publish upstream of every consumer in the same frame: TransformPropagate, MotionDirector,
    // CameraMotion, the animation chain and CameraStack / Resolve read what it writes. RunMask
    // Always, not Play: ctx.deltaTime is real wall clock in Edit, so a sequence plays from the
    // inspector transport without entering Play.
    // The Property / Material tracks address components through a hand-written name -> setter
    // table, registered here so Game.exe has it too; the editor combo box lists the same table.
    Seq::RegisterBuiltinSeqTargets();
    // The TimeDilation track composes through a stack (so two sequences and a gameplay
    // hit-stop cannot clobber each other's value, and a Lua reload cannot silently reset it).
    // This is the ONE place the resolved value reaches the engine's global time scale.
    // A file-static, not a capture: Seq::TimeScale takes a plain function pointer on purpose,
    // since the sink outlives any frame.
    g_seqTimeScaleTarget = &m_scriptSystem;
    Seq::TimeScale::SetSink([](float v) {
        if (g_seqTimeScaleTarget) g_seqTimeScaleTarget->SetSequencerTimeScale(v);
    });

    // Deliberately ungated, unlike the cinematic systems it drives: a sequence player can
    // carry a `gameFlowState`, so this system requests transitions. In Cinematics it would
    // deadlock: the cutscene state would have to be active for the system that switches into it
    // to run. Same rule as the Lua flow controllers: whatever decides the flow must outlive every
    // gate the flow can close.
    reg.Add<SequencerTickSystem>(&renderer, &m_animClipSys, &m_gameFlow, &m_assetMgr);

    // GameplayPostLogic (serial): StaticMesh owners' section entities. Ungated, like
    // TransformPropagate: the sections are derived data, and an owner must draw in every flow
    // state.
    reg.Add<MeshInstanceSystem>(&renderer);

    // FixedPhysics: a single adapter, stepped at PhysicsSystem's fixed dt by
    // App::RunFixedPhysicsLoop, which owns the accumulator.
    reg.AddInSet<PhysicsKinematicSystem>(Sets::kGameplaySimulation, m_physicsSystem);
    reg.AddInSet<PhysicsStepSystem>(Sets::kGameplaySimulation, m_physicsSystem);

    // PhysicsInterpolation: Propagate (always on, so gizmo drags fan out even when paused)
    // -> PhysicsInterp apply -> CameraFollow resolve.
    reg.Add<TransformPropagateSystem>();
    reg.Add<PhysicsInterpApplySystem>(m_physicsSystem);
    reg.Add<CameraFollowResolveSystem>(m_physicsSystem);
    // Camera-stack behaviour systems, Follow then Aim: Follow is the baseline, Aim layers on
    // as a higher-priority VCam in combat / aim states. Both write CameraPoseComponent on their
    // VCam entities, which CameraStack / CameraResolve (PreRender) read.
    reg.Add<FollowCameraTickSystem>();
    reg.Add<AimCameraTickSystem>(&m_physicsSystem);
    // MotionDirector: the master clock that syncs a camera-motion file with a separate
    // skinned-motion file. Runs before CameraMotion so it sets each track's time first (it also
    // writes AnimationComponent.primaryTime, read in the Animation phase). The clip sources let
    // it resolve a total span for countdown / UI when a track has no authored `duration`.
    reg.AddInSet<MotionDirectorTickSystem>(Sets::kCinematics, &m_animClipSys,
                                          &renderer.GetClipLibrary());
    // CameraMotion: plays a camera .ianim (e.g. an MMD camera VMD) onto a dedicated
    // high-priority VCam; needs the clip system to acquire / sample.
    reg.AddInSet<CameraMotionTickSystem>(Sets::kCinematics, &m_animClipSys);

    // PreRender: audio here so collision SFX fire the same frame as the collision (after
    // PhysicsInterpolation, before Render).
    reg.AddInSet<AudioTickSystem>(Sets::kGameplayAudio, m_audioClipSys, m_audioSystem,
                                  m_audio3DSystem, m_audioEngine);

    // CharacterAnimDriver: intent / motor-driven animation selection (loco param bridge +
    // discrete gait fallback + action layer). Before the animation chain so its Animator.Play
    // requests join this frame's mailbox. Opt-in per entity via CharacterAnimDriverComponent.
    reg.AddInSet<CharacterAnimDriverTickSystem>(Sets::kGameplaySimulation, m_animClipSys);

    // Animation: wraps the chain (character state -> AnimSystem sample -> IK -> ChainPhys ->
    // L2W -> Socket/Follow -> bone AABB merge -> SkinMatrix -> BuildSkinJobs). Must run before
    // Render so SkinningPass sees the offsets. Ungated: it rotates the upload rings of the frame's
    // GPU slot, so a state that leaves it out would break skinning (a frozen pose is the game
    // clock's job: a time scale of 0). -eventdelivery_legacy puts it back in the set, with the
    // event drain at its tail.
    if (Gameplay::EventDelivery::LegacyDispatch())
        reg.AddInSet<RendererAnimationChainSystem>(Sets::kGameplayPresentation, renderer);
    else
        reg.AddAlwaysRun<RendererAnimationChainSystem>(renderer);

    // TimelineSystem runs after the animation chain so the prev -> curr time window tests
    // against the latest sampled primaryTime. It reads clip-authored notify tracks via the
    // ClipLibrary (Path A) plus per-entity TimelineComponent overrides (Path B).
    reg.AddInSet<TimelineSystem>(Sets::kGameplayPresentation, renderer.GetClipLibrary(), renderer.GetAnimGraphSystem());

    // BoneAttachment: notify consumers drain the mailboxes TimelineSystem wrote in the
    // previous phase, same render frame. Registration order is chosen for the scheduler's
    // linear-greedy batcher; the plan it builds (the [SCHEDPLAN] log line) is
    //   [StateToggle, CameraEffect, Hitbox] [AudioPlay] [GenericNotify, AnimFXGate] [VFXSpawn*]
    // Hitbox, AudioPlay and GenericNotify all hold kEventBus, so each starts a new batch after
    // the previous holder; the opaque VFXSpawnSystem goes last so it cannot split a batch.
    reg.AddInSet<StateToggleSystem>(Sets::kGameplayFX);
    reg.AddInSet<CameraEffectSystem>(Sets::kGameplayFX);
    // HitboxSystem publishes HitboxHitEvent, so it holds the same main-thread-
    // only kEventBus exclusive AudioPlaySystem and GenericNotifyDispatcher do,
    // plus kPhysicsWorld for its Jolt sweep / overlap queries.
    reg.AddInSet<HitboxSystem>(Sets::kGameplayFX, &m_physicsSystem);
    reg.AddInSet<AudioPlaySystem>(Sets::kGameplayFX);
    reg.AddInSet<GenericNotifyDispatcher>(Sets::kGameplayFX);
    // Level, not a mailbox: mirrors TimelineSystem's ActiveFXGates onto the
    // resident Trail/ParticleEmitter of socket-attached weapon FX. Placed
    // before VFXSpawnSystem so it stays inside the parallel batch.
    reg.AddInSet<AnimFXGateSystem>(Sets::kGameplayFX);
    reg.AddInSet<VFXSpawnSystem>(Sets::kGameplayFX);

    // Camera stack pipeline: in PreRender, after every behaviour system (PhysicsInterpolation)
    // has written CameraPoseComponent and before Render. Stack tick advances the blend state
    // machine; Resolve produces the LiveCameraComponent the Renderer reads; Shake layers
    // additive trauma noise on top.
    reg.Add<CameraStackTickSystem>();
    reg.Add<CameraResolveTickSystem>();
    reg.Add<CameraShakeTickSystem>();
    // After the camera blend resolves: turn volumes + override stack into the
    // frame's ResolvedPostProcessSettings (PostProcess::Runtime).
    reg.Add<PostProcessResolveSystem>();

    // Render: the frame's game half (the barrier, the scene build, the UI paint, the editor
    // chrome), App::RenderPhase in src/App_Render.cpp, which ends by handing the frame to the
    // render half (FramePipeline::SubmitFrame: backend.BeginFrame -> Renderer ->
    // backend.EndFrame).
    reg.Add<RenderSystem>("RenderSystem",
        [this, &backend, &renderer, &ctx](World& world, const FrameContext& frameCtx)
    {
        RenderPhase(backend, renderer, ctx, world, frameCtx);
    });


    // The enabled modules' systems, before the sets are validated below.
    {
        Module::Context moduleCtx = ModuleContext(&renderer);
        moduleCtx.lua = m_scriptSystem.GetLua();
        Module::Registry::Get().RegisterSystems(moduleCtx);
    }

    // C++ and Lua have both declared their SystemSets by now. Validate every
    // Lua state reference before the first transition is allowed to resolve.
    for (const auto& [id, name] : reg.GetSystemSetNames())
    {
        (void)id;
        m_gameFlow.RegisterSystemSet(name);
    }
    m_scriptSystem.ValidateSystemSetAssignments();

    // Declare the whole shared vocabulary, not just the names with a member today. A set
    // becomes "known" when something registers into it, so a bucket whose systems are all
    // ungated (or that only holds Lua scripts, like GameplayUI) would quarantine every state
    // naming it: a game-layer typo and a legitimately empty engine bucket would be the same
    // error. The vocabulary is engine knowledge; membership is not.
    for (const std::string_view setName : {
             Sets::kGameplaySimulation, Sets::kGameplayScripts,
             Sets::kGameplayPresentation, Sets::kGameplayFX,
             Sets::kGameplayAudio, Sets::kCinematics, Sets::kGameplayUI })
    {
        m_gameFlow.RegisterSystemSet(setName);
    }

    m_gameFlow.FinalizeDefinitions();

    // Tell the flow which systems declared no set. It cannot work this out itself (it
    // deliberately knows nothing about ISystem), and the default AlwaysRun policy makes a
    // forgotten `set` silent: the system keeps running and no state can gate it. One Warning
    // diagnostic shows the omission in the panel and to check_gameflow.py.
    {
        std::vector<std::string> unset;
        for (size_t p = 0; p < static_cast<size_t>(TickPhase::COUNT); ++p)
        {
            for (const auto& system : reg.GetSystems(static_cast<TickPhase>(p)))
            {
                if (reg.GetSystemSets(*system).empty() && !reg.IsAlwaysRun(*system))
                    unset.emplace_back(system->GetName());
            }
        }
        m_gameFlow.SetUnsetSystemReport(std::move(unset));
    }

    // Publish the flow to the harness. Here, not earlier: FinalizeDefinitions is the first
    // moment the table is validated and `reg` is fully populated, so the dump's set membership
    // is the membership that gates the run. Unguarded on purpose, like FrameProbe: two pointer
    // stores a Retail build pays once, against an #ifdef that could leave the probe silently
    // null.
    Harness::GameFlowProbe::Get().Bind(&m_gameFlow, &reg, &m_scriptSystem);

    // A scene load runs World::Clear, which destroys the current state's StateScoped entities
    // without running its OnExit or re-entering it, so a HUD spawned by on_enter vanishes and
    // the state never knows. States that declare rebuild_on_scene_load get on_enter run again
    // against the new world: the entities come back with fresh ids in the live world, which is
    // what every reference to them expects.
    EventBus::Get().Subscribe<Scene::SceneLoadedEvent>(
        [this](const Scene::SceneLoadedEvent&)
        {
            m_gameFlow.RebuildAfterWorldReload(m_world);
            // A LoadGame queued the state the save was taken in. THIS is
            // the moment it becomes applicable: the states about to be
            // entered spawn entities, and until now there was no world to
            // spawn them into.
            m_gameFlow.ApplyQueuedRestore();
        });

    m_scheduler.SetRegistry(&reg);
    m_scheduler.SetGameFlow(&m_gameFlow);
    // The C++ event bus drains at the start of GameplayPreLogic and the end of BoneAttachment,
    // outside every set (Gameplay/EventDelivery.h).
    Gameplay::EventDelivery::Install(m_scheduler);
    reg.Initialize(m_world);
}

// ==== Run() helpers: per-frame ====

void App::RefreshMainCamera()
{
    // Resolution order:
    //   1. The entity carrying ActiveCameraTag (set by Camera.SetActive),
    //      provided it's still alive and still has a CameraComponent.
    //   2. The previously-cached entity, if still valid.
    //   3. First-found-with-CameraComponent fallback (covers initial boot,
    //      world load before any Camera.SetActive is issued, and the case
    //      where the active camera entity was destroyed).
    Entity taggedActive = NullEntity;
    m_world.ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
    {
        if (taggedActive == NullEntity
            && m_world.IsAlive(e)
            && m_world.HasComponent<CameraComponent>(e))
        {
            taggedActive = e;
        }
    });
    if (taggedActive != NullEntity)
    {
        m_cameraEntity = taggedActive;
    }
    else if (m_cameraEntity == NullEntity
             || !m_world.IsAlive(m_cameraEntity)
             || !m_world.HasComponent<CameraComponent>(m_cameraEntity))
    {
        // Fallback: pick whichever camera-bearing entity comes first.
        m_cameraEntity = NullEntity;
        for (Entity e : m_world.GetEntities())
        {
            if (m_world.IsAlive(e) && m_world.HasComponent<CameraComponent>(e))
            { m_cameraEntity = e; break; }
        }
    }
    // Expose to Lua so BT/Logic scripts can read ctx:GetEntityPosition(CameraEntity).
    if (sol::state* lua = m_scriptSystem.GetLua())
        (*lua)["CameraEntity"] = LuaId::Out(m_world, m_cameraEntity);
}

void App::TickCamera(float dt, const GameModeContext& ctx)
{
#ifdef WITH_EDITOR
    // Edit mode: the editor's own transient free camera owns the view and the scene
    // camera is left untouched; in Play its VCam is parked so gameplay cameras win
    // (Editor/EditorCameraController.cpp).
    Editor::FlyInput sceneFly;
    m_editorLayer.GetSceneFlyInput(sceneFly);
    if (m_editorCamera.Tick(m_world, m_playModeController.Mode(), ctx, dt, sceneFly))
        return;
#endif

    if (!m_world.GetComponent<CameraComponent>(m_cameraEntity)) return;

    // The camera is an ordinary ECS entity: pose on LocalTransform/GlobalTransform,
    // FPS state on CameraControllerComponent. Backfill anything missing.
    if (!m_world.HasComponent<LocalTransform>(m_cameraEntity))
        m_world.AddComponent<LocalTransform>(m_cameraEntity, LocalTransform{});
    if (!m_world.HasComponent<GlobalTransform>(m_cameraEntity))
        m_world.AddComponent<GlobalTransform>(m_cameraEntity, GlobalTransform{});
    if (!m_world.HasComponent<CameraControllerComponent>(m_cameraEntity))
        m_world.AddComponent<CameraControllerComponent>(m_cameraEntity, CameraControllerComponent{});

    // Backfill the VCam stack components. The lens params are copied from CameraComponent on
    // first touch; the Camera Controller inspector and serialization still edit CameraComponent,
    // so the lens is re-synced each frame below.
    if (!m_world.HasComponent<VirtualCameraComponent>(m_cameraEntity))
    {
        VirtualCameraComponent vc{};
        if (auto* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
        {
            vc.fov   = cam->fov;
            vc.nearZ = cam->nearZ;
            vc.farZ  = cam->farZ;
        }
        m_world.AddComponent<VirtualCameraComponent>(m_cameraEntity, vc);
    }
    if (!m_world.HasComponent<CameraPoseComponent>(m_cameraEntity))
        m_world.AddComponent<CameraPoseComponent>(m_cameraEntity, CameraPoseComponent{});
    if (!m_world.HasComponent<VCamPriorityComponent>(m_cameraEntity))
    {
        VCamPriorityComponent prio{};
        prio.priority = 0;       // baseline: gameplay VCams push at a higher priority
        prio.weight   = 1.f;
        prio.enabled  = true;
        m_world.AddComponent<VCamPriorityComponent>(m_cameraEntity, std::move(prio));
    }
    if (!m_world.HasComponent<VCamBlendComponent>(m_cameraEntity))
    {
        VCamBlendComponent blend{};
        // Baseline starts already Active so first-frame resolve finds a winner.
        blend.currentBlend     = 1.f;
        blend.state            = BlendState::Active;
        blend.blendInDuration  = 0.f;
        blend.blendOutDuration = 0.25f;
        m_world.AddComponent<VCamBlendComponent>(m_cameraEntity, std::move(blend));
    }
    // Mirror the CameraComponent lens (fov / near / far) onto the VCam each frame, so
    // inspector edits still apply.
    if (auto* cam = m_world.GetComponent<CameraComponent>(m_cameraEntity))
    if (auto* vc  = m_world.GetComponent<VirtualCameraComponent>(m_cameraEntity))
    {
        vc->fov   = cam->fov;
        vc->nearZ = cam->nearZ;
        vc->farZ  = cam->farZ;
    }
    // Ensure the Main channel entity exists.
    Camera::GetOrCreateChannelEntity(m_world, Camera::kMainChannel);

    auto* ctrl = m_world.GetComponent<CameraControllerComponent>(m_cameraEntity);
    auto* lt   = m_world.GetComponent<LocalTransform>(m_cameraEntity);
    if (!ctrl || !lt) return;

    if (ctx.cameraLookActive)
    {
        Camera::ApplyCameraLook(*ctrl, *lt, ctx, dt);
    }
    else
    {
        // Idle: a script / turntable / gizmo / world load may own the pose; keep yaw / pitch
        // tracking it so re-grabbing the camera does not snap.
        CameraSystem::SyncControllerFromTransform(*ctrl, *lt);
        if (ctx.viewportRightMouseHeld && dt > 0.f)
            CameraSystem::ApplyFly(*ctrl, *lt, dt);
    }

    // The camera is a root entity: patch its GlobalTransform now so systems that run before
    // TransformSystem::Propagate (AI LOD, collision viz) see it.
    if (auto* gt = m_world.GetComponent<GlobalTransform>(m_cameraEntity))
        DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());
}


void App::UpdateViewportSize(GameModeContext& ctx, IGraphicsDevice& backend)
{
    unsigned int baseW, baseH;
    if (m_viewportFullscreen)
    {
        baseW = backend.GetWidth();
        baseH = backend.GetHeight();
    }
    else
    {
#ifdef WITH_EDITOR
        unsigned int vpW = 0, vpH = 0;
        m_editorLayer.GetViewportSize(vpW, vpH);
        baseW = vpW ? vpW : backend.GetWidth();
        baseH = vpH ? vpH : backend.GetHeight();
#else
        baseW = backend.GetWidth();
        baseH = backend.GetHeight();
#endif
    }

#if defined(WITH_EDITOR) && defined(WITH_HARNESS)
    // -preview_parity: force the SQUARE viewport the preview renders into, so
    // the two captures are the same size and the same projection. See
    // Editor::PreviewHarness::m_previewParityPx for why this is forced rather than configured.
    if (m_previewHarness.m_previewParityPx) { baseW = baseH = m_previewHarness.m_previewParityPx; }
#endif

    // This is the display viewport. Internal 3D render scaling is applied only
    // when sizing the HDR target in RenderSystem; UI and input stay in pixels of
    // the visible viewport at every renderScale value.
    ctx.viewportW = baseW;
    ctx.viewportH = baseH;
}

void App::SyncWindowCursor()
{
    bool captured = m_flowWantsCapture;
    RECT clip{};
    bool clipped = false;
#ifdef WITH_EDITOR
    const EditorLayer::WindowCursor want = m_editorLayer.DesiredWindowCursor(m_flowWantsCapture);
    captured = want.captured;
    clipped  = want.clipped;
    clip     = RECT{ want.clipX, want.clipY, want.clipX + want.clipW, want.clipY + want.clipH };
#endif
    const bool clipMoved = clipped && (clip.left != m_cursorApplied.clip.left || clip.top != m_cursorApplied.clip.top
                                       || clip.right != m_cursorApplied.clip.right
                                       || clip.bottom != m_cursorApplied.clip.bottom);
    // The clip first: DisableCursor confines to it.
    if (clipped != m_cursorApplied.clipped || clipMoved)
        wnd.SetCursorClipRect(clipped ? &clip : nullptr);
    if (captured != m_cursorApplied.captured)
    {
        if (captured) wnd.DisableCursor();
        else          wnd.EnableCursor();
    }
    m_cursorApplied = AppliedCursor{ captured, clipped, clipped ? clip : RECT{} };
}

void App::ApplyGameFlowResources(Renderer& renderer, const FrameContext& frameCtx)
{
    // Renderer::BeginFrame must keep running in Edit/Pause to draw skinned
    // meshes, but its manually-ordered chain solver sits outside the Scheduler's
    // run-mask, so it has to be gated by hand. Two authorities, ANDed: the
    // editor transport and the game flow's time scale.
    const bool simulate = frameCtx.runUpdate && frameCtx.scaledDeltaTime > 0.f;
    renderer.SetChainPhysicsSimulationEnabled(simulate);
    // A playing scene whose time stands still (hit-stop, a paused game state, the path tracer's
    // time freeze) keeps its chains' pose; the editor transport's Edit / Pause show the rest pose.
    renderer.SetChainPhysicsHold(frameCtx.runUpdate && !simulate);

    // Audio, ORed rather than last-writer-wins: the editor Pause button and a paused game
    // state are independent reasons for silence, and whichever is still true keeps it. The
    // game-flow half is what lets Game.exe (pinned to Playing) pause audio at all.
    m_audioEngine.SetGlobalPause(frameCtx.mode == EngineMode::Paused
                                 || m_gameFlow.EffectiveAudioPaused());

    // ---- Input contexts ----
    // Diffed against what was pushed last frame rather than pushed in OnEnter and popped in
    // OnExit: Input::PushContext is a stack, and a push twice / pop once leaves the player
    // silently unable to move for the rest of the session.
    const std::vector<std::string>& wanted = m_gameFlow.EffectiveInputContexts();
    if (wanted != m_flowInputContexts)
    {
        for (const std::string& ctx : m_flowInputContexts)
        {
            if (std::find(wanted.begin(), wanted.end(), ctx) == wanted.end())
                Input::Get().PopContext(ctx.c_str());
        }
        for (const std::string& ctx : wanted)
        {
            if (std::find(m_flowInputContexts.begin(), m_flowInputContexts.end(),
                          ctx) == m_flowInputContexts.end())
            {
                Input::Get().PushContext(ctx.c_str());
            }
        }
        m_flowInputContexts = wanted;
    }
    // A script's pop of the same name (Input.PopContext, or its owner's release popping what it
    // pushed) must not take a flow context with it: re-seat any wanted one that went inactive.
    for (const std::string& ctx : m_flowInputContexts)
        if (!Input::Get().IsContextActive(ctx.c_str()))
            Input::Get().PushContext(ctx.c_str());

    // ---- Cursor ----
    // The flow's wish changes only on a change of its resolved mode. Inherit leaves the wish alone: going
    // back to "no opinion" is not the same as demanding the cursor back.
#ifdef WITH_EDITOR
    if (frameCtx.mode == EngineMode::Edit)
    {
        // The editor owns the cursor in Edit mode; the next Play asks the flow again.
        m_flowCursorApplied = Gameplay::CursorMode::Inherit;
        m_flowWantsCapture  = false;
    }
    else
#endif
    if (const Gameplay::CursorMode cursor = m_gameFlow.EffectiveCursorMode();
        cursor != m_flowCursorApplied)
    {
        m_flowCursorApplied = cursor;
        if (cursor == Gameplay::CursorMode::Captured)      m_flowWantsCapture = true;
        else if (cursor == Gameplay::CursorMode::Free)     m_flowWantsCapture = false;
    }
    SyncWindowCursor();

    // ---- Bus ducking ----
    const auto& ducks = m_gameFlow.EffectiveBusDucks();
    auto busFromName = [](const std::string& name) -> Audio::BusType
    {
        if (name == "Music")   return Audio::BusType::Music;
        if (name == "SFX")     return Audio::BusType::SFX;
        if (name == "Voice")   return Audio::BusType::Voice;
        if (name == "Ambient") return Audio::BusType::Ambient;
        if (name == "UI")      return Audio::BusType::UI;
        if (name == "Master")  return Audio::BusType::Master;
        return Audio::BusType::Count;   // unknown: reported once below
    };

    for (const std::string& bus : m_flowDuckedBuses)
    {
        const bool stillDucked = std::any_of(ducks.begin(), ducks.end(),
            [&](const auto& e) { return e.first == bus; });
        if (stillDucked) continue;
        if (const Audio::BusType id = busFromName(bus); id != Audio::BusType::Count)
            m_audioEngine.SetBusVolume(id, 1.f);
    }
    m_flowDuckedBuses.clear();
    for (const auto& [bus, volume] : ducks)
    {
        const Audio::BusType id = busFromName(bus);
        if (id == Audio::BusType::Count)
        {
            // Once per distinct name: this runs every frame the state is
            // current, and a typo would otherwise fill the log by itself.
            static std::unordered_set<std::string> s_warnedBuses;
            if (s_warnedBuses.insert(bus).second)
                LOG_WARNING("GameFlow: unknown audio bus '%s' in bus_duck "
                            "(expected Master/Music/SFX/Voice/Ambient/UI)",
                            bus.c_str());
            continue;
        }
        m_audioEngine.SetBusVolume(id, volume);
        m_flowDuckedBuses.push_back(bus);
    }
}

void App::RunFixedPhysicsLoop(float scaledDt, FrameContext& frameCtx, bool worldFrozen)
{
    // App owns the accumulator. PreAllSteps runs once for body create / teleport detection,
    // the Pre/Step/Post triplet N times (spiral of death capped at 5 steps and a 0.25 s clamp),
    // then PostAllSteps drains contacts. physicsAlpha = the leftover fraction for
    // PhysicsInterpApplySystem's slerp.
    // Frozen by either authority: the editor transport (runUpdate) or a game state with
    // time_scale = 0. Do not fall through with scaledDt == 0 instead: PreAllSteps / PostAllSteps
    // would still run every frame and the accumulator would keep its partial step, so the resume
    // frame would start mid-step (the ragdoll / chain / skirt twitch on un-pause).
    const float kFixedDt = DX12Physics::PhysicsSystem::GetFixedDt();
    if (!frameCtx.runUpdate || scaledDt <= 0.f)
    {
        // The path tracer's time freeze holds the world mid-step: keep the partial step and its
        // interpolation fraction, so each Render.ptStep advances physics by exactly its length
        // and the frozen frames show the bodies where the step left them.
        if (frameCtx.runUpdate && worldFrozen)
        {
            frameCtx.physicsAlpha = std::clamp(m_physicsAccumulator / kFixedDt, 0.f, 1.f);
            return;
        }
        // Edit mode: no step and no event, but the bodies follow the edits (PhysicsSystem::SyncEditMode).
        if (frameCtx.mode == EngineMode::Edit) m_physicsSystem.SyncEditMode(m_world);
        // Edit and Paused: zero the accumulator so resuming doesn't burn catch-up steps.
        m_physicsAccumulator = 0.f;
        frameCtx.physicsAlpha = 0.f;
        return;
    }

    m_physicsSystem.PreAllSteps(m_world);

    m_physicsAccumulator += scaledDt;
    // A frozen world's step (up to 1 s) runs all of its fixed steps: there is no frame-time
    // spiral to guard against.
    const float maxAccum = worldFrozen ? 1.f + kFixedDt : 0.25f;
    if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;

    int safety = worldFrozen ? 61 : 5;
    // The substeps this frame will run, for the Kinematic movers to divide their motion over.
    int substeps = 0;
    for (float left = m_physicsAccumulator; left >= kFixedDt && substeps < safety; left -= kFixedDt) ++substeps;
    m_physicsSystem.SetFrameSubsteps(static_cast<unsigned>((std::max)(substeps, 1)));
    frameCtx.isFixedTickPhase = true;
    while (m_physicsAccumulator >= kFixedDt && safety-- > 0)
    {
        m_scheduler.RunPhase(TickPhase::FixedPhysicsPre,  m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::FixedPhysics,     m_world, frameCtx);
        m_scheduler.RunPhase(TickPhase::FixedPhysicsPost, m_world, frameCtx);
        m_physicsAccumulator -= kFixedDt;
        frameCtx.physicsStepIndex++;
    }
    frameCtx.isFixedTickPhase = false;

    m_physicsSystem.PostAllSteps(m_world);

    frameCtx.physicsAlpha = m_physicsAccumulator / kFixedDt;
    if (frameCtx.physicsAlpha < 0.f) frameCtx.physicsAlpha = 0.f;
    if (frameCtx.physicsAlpha > 1.f) frameCtx.physicsAlpha = 1.f;
}



void App::ShutdownAllSystems(IGraphicsDevice& backend, Renderer& renderer)
{
    m_pipeline.DrainFrameWork(Diag::DrainCause::Quit);
    FramePipeline::LogDrainStats("total", false, m_frameIndex);
    if (Sys::CommandLine::Get().Has("-luavmcheck"))
        LOG_INFO("[LUAVM] checks=%llu violations=%llu",
                 static_cast<unsigned long long>(Sys::ScriptVM::Checks().load()),
                 static_cast<unsigned long long>(Sys::ScriptVM::Violations().load()));
    // No frame is in flight any more: the modules close what they hold (face sources)
    // before anything it could outlive goes.
    Module::Registry::Get().Shutdown();
    // Join the render-owner lane while the device, the renderer and the task system are all
    // alive: its queue's CloseAndDrain runs on the lane and needs all three.
    // This function has exactly one call site, the orderly exit in App::Run: the lane is started
    // once and stopped once, and every other path drains it instead:
    //   world reload / scene load   SceneManager tears the world down from the game half on
    //       main; Renderer::OnWorldClear first drains the lane (DrainCpuFrame -> the park hook ->
    //       DrainFrameWork), so it and GraphicsDX12::WaitForPreviousFrame run with no frame in
    //       flight. The lane is a process-lifetime thread and keeps running across a reload
    //       (renderowner_reload).
    //   swap-chain resize           Window::ServicePendingSize -> ApplyResizeNow on main,
    //       outside the device frame; device work that parks goes through the hook ->
    //       DrainFrameWork -> DrainRenderOwner. The lane survives the rebuild
    //       (renderowner_resize).
    //   device lost                 a ThrowIfFailed exception unwinds out of App::Run to
    //       wWinMain's catch and never reaches here; ~App's FramePipeline::Shutdown stops the
    //       lane.
    // Nothing below may run while a frame is in flight, which the DrainFrameWork above
    // guarantees.
    m_pipeline.StopRenderOwner();
    m_pipeline.UnhookDevice(true);   // the park hook; [FRAME_EXECUTOR] drained=1

#ifdef WITH_EDITOR
    // The one-shot thumbnail summary for the last churn pass (a single-pass session reports
    // pass=1). Here because it needs the descriptor / command-list pool accessors while the
    // device is alive; it emits nothing when the asset browser produced no material tile.
    m_editorLayer.ReportThumbnailSummary(backend);
    // ...and the tiles themselves, here rather than in ~EditorLayer for the same reason: the
    // last point at which the device is provably alive.
    m_editorLayer.ReleaseThumbnailTiles(backend);
    // ...and every preview view. A panel hands its view back when it did not draw last frame,
    // which needs a next frame; there is none after this, so without this call a panel open at
    // exit would leave a live view slot. With it, `live == 0` is an invariant PreviewRenderer's
    // destructor checks.
    m_editorLayer.ReleasePreviewViews(renderer);
#endif
#if defined(WITH_HARNESS) && defined(WITH_EDITOR)
    // Split cameras, the -preview_touch_at mtime, the -preview_asset_appear_at copy and
    // every armed file restore, put back while the World is whole (Editor/PreviewHarness.cpp).
    m_previewHarness.Shutdown();
#endif

    // Resolve suspended consumers before any world teardown callback runs.
    m_pipeline.CloseQueues();

    // Give the flow its last word while the world is still whole: OnExit,
    // leaf -> root. Without this the process simply stops and no state ever
    // learns it was the last one, so "save on quit" is not expressible.
    m_gameFlow.ShutdownRuntime(m_world, m_commandBuffer);

    // Drop App's window subscriptions first: past this point the GameFlow
    // machine and (in the editor) EditorLayer's world/renderer pointers are
    // being torn down, and a stray WM_CLOSE or WM_ACTIVATEAPP arriving during
    // shutdown must not reach either of them. (EditorLayer's own ImGui
    // subscriptions go with its backends.)
    m_windowSubs.clear();
#ifdef WITH_EDITOR
    wnd.AcceptFileDrops(false);   // the editor's file drops
#endif

    // The prefab preloads' workers finish, then their pins go back, while the renderer, the
    // libraries and the texture system are still whole.
    Resource::PrefabSpawnQueue::Shutdown(backend, m_meshLib, m_textureSys);

    // Tear down ISystem adapters BEFORE the concrete singletons they wrap so
    // OnUnregister can still detach event listeners against live targets.
    m_systemRegistry.Shutdown(m_world);

    m_physicsSystem.Shutdown();

    // Audio: detach subscribers / entity-destroy listener before World tears
    // down; shutdown the clip system AFTER the engine releases voices because
    // VoiceRecord borrows AudioClipResource pointers until DestroyVoice flushes.
    m_audioSystem.Unbind();
    m_audioEngine.Shutdown();
    m_audioClipSys.Shutdown();

    // Explicit GPU-resource release while device is still alive.
    m_animClipSys.Shutdown();
    m_uiFramePainter.ReleaseAll(m_textureSys, backend);   // the Canvas image leases
    m_assetMgr.Shutdown();
    m_meshLib.Shutdown(backend);
    m_textureSys.Shutdown(backend);
    m_resourceMgr.Shutdown();
    Resource::BCCompressor::Get().Shutdown();
}
