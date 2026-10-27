#pragma once

#include <thread>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <chrono>
#include <exception>
#include "System/OwnerTask.h"
#include "Platform/Window.h"
#include "Graphics/GraphicsDX12.h"   // m_gfx: App owns the device
#include "Renderer/FramePipeline.h"
#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#include "Editor/DebugDrawSystem.h"
#include "Editor/PreviewHarness.h"
#include "Editor/EditorCameraController.h"
#include "Editor/EditorFrameHooks.h"
#endif
#include "Scene/GameModeContext.h"
#include "Scene/SceneTransitionManager.h"
#include "Scene/SceneManager.h"
#include "Input/OsMouseLook.h"

// ---- Resource systems ----
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Resource/MeshLibrary.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/EngineSettings.h"
#include "Resource/EngineDevSettings.h"

// ---- ECS + scheduler ----
#include "ECS/ECS.h"
#include "ECS/PlayerControllerSystem.h"
#include "ECS/SystemRegistry.h"
#include "ECS/Scheduler.h"
#include "ECS/CommandBuffer.h"
#include "ECS/PlayModeController.h"
#include "ECS/PlayModeSession.h"
#include "Gameplay/GameFlowState.h"
#include "Module/EngineModule.h"   // Module::Context

#include <cstdint>
#include <memory>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// ---- Gameplay subsystems ----
#include "Scripting/ScriptSystem.h"
#include "AI/AISystem.h"
#include "AI/AILODSystem.h"
#include "AI/PerceptionSystem.h"
#include "Physics/PhysicsSystem.h"

// ---- Audio ----
#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Audio/Audio3DSystem.h"
#include "Audio/AudioClipSystem.h"

// ---- UI ----
#include "UI/UICanvasSystem.h"
#include "UI/WorldSpaceUISystem.h"
#include "UI/DebugHud.h"
#include "UI/UIFramePainter.h"
namespace Harness { class HarnessCaptureSystem; }
namespace Diag { enum class DrainCause : uint8_t; }   // Diagnostics/OwnerAccounting.h

class Renderer;
struct GameModeContext;
struct FrameContext;

class App
{
public:
    App();
    // Not implicit, and not cosmetic: `App app;` lives inside wWinMain's try block,
    // so a throw out of Run() destroys App before the catch runs. Destroying a
    // joinable std::thread is std::terminate (exit 3 through the terminate handler),
    // and the real exception never reaches the reporter that would name it. Join
    // every thread App owns here as well as on the orderly path.
    ~App();
    int Run();

    // The scene-transition driver, exposed only so a game can install its own
    // loading-screen art (SceneTransitionManager::SetOverlayHook). Begin / Tick stay
    // App's to call: driving a swap from outside the loop would queue a mode App is
    // not draining this frame. With no hook installed the built-in bar is drawn.
    SceneTransitionManager& Transition() noexcept { return m_transition; }

private:
    struct MainSurface
    {
        GraphicsDX12& gfx;
        MainSurface(GraphicsDX12& g, const Plat::Window& w) : gfx(g)
        {
            gfx.CreateMainSurface(w.NativeHandle(), static_cast<UINT>(w.ClientWidth()),
                                  static_cast<UINT>(w.ClientHeight()));
        }
        ~MainSurface() { gfx.ReleaseMainSurface(); }
        MainSurface(const MainSurface&) = delete;
        MainSurface& operator=(const MainSurface&) = delete;
    };

    struct SurfaceAttachment
    {
        Plat::Window& window;
        SurfaceAttachment(Plat::Window& w, ISurface& s) : window(w) { window.AttachSurface(&s); }
        ~SurfaceAttachment() { window.AttachSurface(nullptr); }
        SurfaceAttachment(const SurfaceAttachment&) = delete;
        SurfaceAttachment& operator=(const SurfaceAttachment&) = delete;
    };

    // ---- Run() helpers ----
    // Run()'s boot steps, in order, each one contiguous block of the boot sequence.
    // The loop's state (ctx, the pending scene, the hot-reload service, the harness
    // latches) stays in Run().
    void        BootContent();
    std::string InitPsoLibrary(IGraphicsDevice& backend);       // returns the write path
    void        InitCoreSystems(IGraphicsDevice& backend);
    void        BootRenderer(IGraphicsDevice& backend, Renderer& renderer);
    bool        BootScripting(IGraphicsDevice& backend, Renderer& renderer);   // true = refused
    void        BootScenes(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx,
                           bool bootScriptsRefused);
    void        InstallWindowHooks();
    // The background policy's focus half (Plat::DecideBackground): a game that lost the foreground
    // mutes its audio and, when the project asks, pauses through GameFlow under an owner of its own.
    void        ApplyBackgroundPolicy(bool focused);
    void        WirePlayMode(IGraphicsDevice& backend, Renderer& renderer);
    void RegisterResourceLoaders(IGraphicsDevice& backend);
    void InitUIFont(IGraphicsDevice& backend);
    void InitLuaBindings(IGraphicsDevice& backend, Renderer& renderer);
    // What a module hook gets (Module/EngineModule.h); @p renderer is null before BootRenderer.
    Module::Context ModuleContext(Renderer* renderer);
    void InitAudio();
    void WireEditor(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx);
    void RegisterTickSystems(IGraphicsDevice& backend, Renderer& renderer,
                              GameModeContext& ctx);
    // The Render phase's body (run by the RenderSystem registered above), in
    // src/App_Render.cpp, compiled per exe like this file.
    void RenderPhase(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx,
                     World& world, const FrameContext& frameCtx);
    void RefreshMainCamera();
    void TickCamera(float dt, const GameModeContext& ctx);
    void UpdateViewportSize(GameModeContext& ctx, IGraphicsDevice& backend);
    // @p worldFrozen: the path tracer's time freeze holds (Renderer::IsWorldClockFrozen).
    void RunFixedPhysicsLoop(float scaledDt, FrameContext& frameCtx, bool worldFrozen);

    // Make the world match what the current GameFlow state declared: input context,
    // cursor, audio pause, bus ducking, chain-physics simulation. Called once per
    // frame right after the Input phase resolves the flow. Idempotent (it diffs the
    // resolved value against what it applied last frame), so nothing is left
    // stranded by an aborted transition or a Play -> Stop.
    void ApplyGameFlowResources(Renderer& renderer, const FrameContext& frameCtx);
    // The window's cursor: the wanted state (the flow's wish; in the editor, EditorLayer::DesiredWindowCursor)
    // is worked out on every call and the window is told only what differs from m_cursorApplied - a
    // re-assert every frame would fight WM_SETFOCUS's confine.
    void SyncWindowCursor();

    // @p renderer is the App::Run local: the editor's preview panels hand their view
    // slots back here, in the one window where the render thread is joined and the
    // device is still alive.
    void ShutdownAllSystems(IGraphicsDevice& backend, Renderer& renderer);

    // What ApplyGameFlowResources put into the world last frame, so it knows
    // what to take back out.
    std::vector<std::string> m_flowInputContexts;
    std::vector<std::string> m_flowDuckedBuses;
    Gameplay::CursorMode     m_flowCursorApplied = Gameplay::CursorMode::Inherit;
    // The flow asks for a captured cursor (its last Captured / Free; Inherit keeps the answer). The editor's
    // Game View applies it only while it has the game's focus.
    bool                     m_flowWantsCapture = false;
    // What SyncWindowCursor last told the window.
    struct AppliedCursor
    {
        bool captured = false;
        bool clipped  = false;
        RECT clip{};
    };
    AppliedCursor            m_cursorApplied;

    // The window comes first: its dtor runs last, after the device below and every
    // GPU-resource-owning system.
    Plat::Window wnd;

    // The device: App's, not the window's. Built from the window right after it (its
    // native handle and actual client size) and declared right after it, so it is
    // destroyed right before it - the swap chain goes while its HWND is still alive.
    // Every GPU-resource-owning member below goes before both.
    std::unique_ptr<GraphicsDX12> m_gfx;

    // The main surface: the swap chain on the window. The device is created without
    // a window; this puts its swap chain on wnd (at the client size the window got)
    // right after, and releases it right before the device (Frame Generation off,
    // Streamline shut down, the swap chain gone) while the window is still alive.
    // Every GPU-resource owner below goes first.
    MainSurface m_mainSurface{ *m_gfx, wnd };

    // Attaches the main surface (an ISurface: the window never sees the device)
    // to the window while it exists: WM_SIZE, the fullscreen service and
    // WM_DISPLAYCHANGE reach it while attached, and nothing the window does
    // while the surface or the device is being torn down (SetFullscreenState
    // sends WM_SIZE) can reach them.
    SurfaceAttachment m_surfaceAttachment{ wnd, m_gfx->MainSurface() };

    // ---- Resource systems (GPU resources released in ShutdownAllSystems) ----
    Resource::ResourceManager      m_resourceMgr;
    Resource::TextureSystem        m_textureSys;
    Resource::MeshLibrary          m_meshLib;
    Resource::AssetManager         m_assetMgr;
    Resource::AnimationClipSystem  m_animClipSys;
    Audio::AudioClipSystem         m_audioClipSys;
    // Player graphics / quality options (settings.json): loaded at boot and applied
    // to the renderer / graphics / window; the editor's Graphics Settings window
    // edits and saves it. A writable loose file, never sealed into game.ipak.
    Resource::EngineSettings       m_settings;
    // Engine / developer configuration (engine.json): like game.json, sealed into
    // game.ipak, read through AssetFS and never written by a shipped build. Its
    // fields are disjoint from m_settings.
    Resource::EngineDevSettings    m_devSettings;

    // Engine.Quit() latch. A script cannot call PostQuitMessage itself (nor tear
    // anything down mid-frame), so the binding records the intent and Run() drains it
    // where the editor honours its WM_CLOSE veto. PostQuitMessage only queues
    // WM_QUIT, so the in-flight frame records and presents normally and
    // Window::ProcessMessage picks it up at the top of the next iteration. Main
    // thread only: every writer is a Lua binding.
    bool m_quitRequested = false;
    // Exit code carried by Engine.Quit(code) / Test.Done(code). A SUGGESTION:
    // RunLedger::Finalize may raise it after the logger drains, so a scenario
    // that quits with 0 having already failed an assertion still exits non-zero.
    int  m_quitExitCode  = 0;
    // Frames fully presented, mirrored from GameModeContext::frame each loop so
    // Lua (Engine.Frame) and the harness can read it. ctx is a Run()-local
    // declared after InitLuaBindings, so the bindings cannot capture it.
    std::uint64_t m_frameIndex = 0;

#ifdef WITH_EDITOR
    EditorLayer  m_editorLayer;
#endif
    // Drives the fade-out -> loading screen -> fade-in around scene switches.
    SceneTransitionManager m_transition;
    // Data-driven scene registry + unified loader. Routes Scene.Load() through the
    // same transition machinery, carrying just the target scene id.
    SceneManager m_sceneManager;

    // ---- Phase-based scheduler (docs/design/System_Scheduler_Architecture.md) ----
    // Declared before Lua-owned engine state so GameFlow.Dump's borrowed registry
    // reference stays alive through ScriptSystem::OnShutdown.
    SystemRegistry m_systemRegistry;
    Scheduler      m_scheduler;
    CommandBuffer  m_commandBuffer;

    // ---- Engine state (shared across scenes; survives level switches) ----
    // Scene loads replace this World's CONTENTS, never the World itself; the systems
    // below are stateful (Lua VM, Jolt, timers) and outlive every scene.
    World                       m_world;
    // The frame pipeline (Renderer/FramePipeline.h): the render-owner lane, the
    // drains, the render halves, the barriers and the frame's timing. After m_world
    // and m_scheduler, which it borrows. Its lane and queues are stopped in
    // ShutdownAllSystems and again in ~App's body, before any member is destroyed.
    FramePipeline               m_pipeline{ *m_gfx, m_world, m_scheduler };
#ifdef WITH_EDITOR
    // The editor's part of the frame, which the pipeline calls through its hooks
    // (Editor/EditorFrameHooks.h; installed in WireEditor, before Configure).
    Editor::EditorFrameHooks    m_editorFrameHooks{ m_editorLayer, m_scheduler, m_viewportFullscreen };
#endif
    PlayerControllerSystem      m_playerCtrl;
    // Declared before ScriptSystem so Lua (whose GameFlow closures borrow this
    // resource) is destroyed first; definitions themselves hold no sol objects.
    Gameplay::GameFlowStateMachine m_gameFlow;
    AI::AISystem                m_aiSystem;
    AI::AILODSystem             m_aiLODSystem;
    AI::PerceptionSystem        m_perceptionSystem;
    DX12Physics::PhysicsSystem  m_physicsSystem;
    // Editor-only single submission point for debug visuals (wireframes + light
    // icons), driven from App::RenderPhase. Guarded because DebugDrawSystem.h is
    // included only in editor builds.
#ifdef WITH_EDITOR
    DebugDrawSystem             m_debugDraw;
#endif
    Audio::AudioEngine          m_audioEngine;
    Audio::AudioSystem          m_audioSystem;
    Audio::Audio3DSystem        m_audio3DSystem;
    Entity                      m_cameraEntity{ NullEntity };

#ifdef WITH_EDITOR
    // The editor's free-fly camera, its cached pose and the ortho-view switch
    // (Editor/EditorCameraController.h).
    Editor::EditorCameraController m_editorCamera;
#if defined(WITH_HARNESS)
    // The editor host's harness: every editor-only -preview_* flag, the editor
    // tools' and -chainpreview_*, the steps they schedule in the frame loop and the
    // files they put back at shutdown (Editor/PreviewHarness.h).
    Editor::PreviewHarness m_previewHarness{ m_editorLayer, m_pipeline.RenderOwnerEnabledFlag(),
        [this](Diag::DrainCause cause) { m_pipeline.DrainFrameWork(cause); } };
#endif
#endif

    // ---- UI ----
    // UICanvasSystem is the single screen-space UI path and feeds UIPass.
    // WorldSpaceUISystem is a separate 3D billboard path.
    UI::UICanvasSystem          m_uiCanvasSystem;
    UI::UIInputState            m_uiInput;
    bool                        m_prevUILeft = false;
    // The Render phase's UI paint, and the Canvas image texture leases it owns
    // (UI/UIFramePainter.h; released in ShutdownAllSystems).
    UI::UIFramePainter          m_uiFramePainter;
    UI::WorldSpaceUISystem      m_worldSpaceUISystem;

    // "stat unit" overlay. Declared OUTSIDE every WITH_EDITOR guard on purpose:
    // the whole point of drawing it with the engine's SDF font instead of ImGui
    // is that it exists in Game.exe, and an editor-conditional member would
    // also change sizeof(App) between the two binaries.
    UI::DebugHud                m_debugHud;

    // Keep the Lua VM after every subsystem its registered C closures borrow.
    // Reverse member destruction therefore tears Lua down first, while World,
    // GameFlow, SceneManager, scheduler debug data, audio, nav, etc. are alive.
    ScriptSystem                m_scriptSystem;

    // ---- Edit/Play state machine (docs/design/editor_runtime_mode_architecture.md) ----
    // Authoritative Edit/Playing/Paused/Stepping mode + GameClock. The editor toolbar
    // drives it; App wires onEnterPlay/onExitPlay to snapshot / restore the world.
    // Game + ShaderLab builds pin it to Playing at boot.
    PlayModeController m_playModeController;

    // Externalised physics accumulator. Cleared on world reload so a long load does
    // not burn 5 catch-up steps on the first tick.
    float        m_physicsAccumulator{ 0.0f };
    // The Edit/Play snapshot + restore (ECS/PlayModeSession.h); after
    // m_physicsAccumulator, which it resets on both edges.
    PlayModeSession m_playSession{ m_world, m_audioEngine, m_audioSystem, m_physicsSystem,
                                   m_scriptSystem, m_gameFlow, m_physicsAccumulator,
                                   [this](Diag::DrainCause cause) { m_pipeline.DrainFrameWork(cause); } };
    bool         m_viewportFullscreen{ false };

    // Game build only: the OS-mouse look delta the editor would otherwise supply
    // from its viewport panel (ctx.mouseViewportDX/DY), and the previous position it
    // is taken from (Input/OsMouseLook.h). Unconditional; it costs three scalars.
    OsMouseLook  m_osMouseLook;

    // The size / move modal loop ended since the last frame (the window's
    // ModalLoopEnded event): names the cause of a clamped dt in the log.
    bool m_modalLoopEnded = false;

    // The PSO library's write path (InitPsoLibrary): the session end saves it (WM_ENDSESSION is the
    // process's last moment).
    std::string m_psoWritePath;
    // The GameFlow pause the application's loss of the foreground asked for (BackgroundPolicy
    // pauseOnFocusLoss), under its own owner; 0 = none.
    std::uint64_t m_focusPause = 0;

    // App's subscriptions to the window (InstallWindowHooks): the focus edge, device
    // changes, the modal-loop end, the close handlers, the Canvas's capture
    // predicates and (editor) dropped files. Last member on purpose: destroyed first,
    // before anything a handler captured, and cleared explicitly in
    // ShutdownAllSystems before teardown begins.
    std::vector<Plat::Subscription> m_windowSubs;
};
