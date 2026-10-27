// App::RenderPhase - the Render phase's GAME half: the hard-sync barrier, the structural
// sync point, the asset tick, the scene build, the UI paint and the editor's ImGui chrome.
// It ends by handing the frame to the render half (FramePipeline::SubmitFrame:
// backend.BeginFrame -> Renderer -> backend.EndFrame).
//
// PER EXE, like App.cpp: it defines App members, and App.h's WITH_EDITOR members make
// sizeof(App) differ between Editor.exe and Game.exe (CLAUDE.md invariant 2).

#include "App.h"
#include "Graphics/AnimProfile.h"

// ==== Engine / ECS ====
#include "ECS/EngineSystems.h"
#include "Sequencer/SequencerSystem.h"
#include "Sequencer/SequenceEval.h"      // Seq::Preview::OwnsViewport - Camera Lock
#include "Sequencer/LuaSequenceBindings.h"

#include "ECS/FrameContext.h"
#include "ECS/LuaCharacterStateBindings.h"
#include "ECS/LuaFacialBindings.h"      // Face.* (morph targets / blend shapes)
#include "ECS/LuaSkeletonBindings.h"    // Skeleton.* (bones by name, sockets)
#include "ECS/LuaPlayerBindings.h"
#include "ECS/LuaIKBindings.h"
#include "ECS/CameraStackSystem.h"
#include "ECS/GuidRegistry.h"
#include "Gameplay/SaveGame.h"   // InstallPersistenceHooks (save-game v3 graveyard)

// ==== Scene ====
#include "Scene/MeshSpawner.h"
#include "Scene/MeshInstanceSystem.h"   // StaticMesh owners -> section entities
#include "Scene/SceneInstanceLoader.h"  // SynthesizeMeshLibIdentity - MeshLibrary's identity provider
#include "Resource/PrefabSpawnQueue.h"  // the prefab preloads' AssetTick step
#include "Scene/TransformSystem.h"

// ==== Graphics ====
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/StreamlineBackend.h"
#include "Renderer/Renderer.h"
#include "Renderer/RenderExplain.h"             // Render.explain
#include "Renderer/AssetQuery.h"                // Asset.get / Asset.loaded
#include "Renderer/Preview/PreviewHarnessGame.h" // the game-capable -preview_* flags
#include "Graphics/AfterimageSystem.h"
#include "Graphics/WaterRippleSystem.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "Tools/CollisionMeshBaker.h"

// ==== Resource (loaders + importers) ====
#include "Resource/TextureSystem.h"
#include "Resource/BCCompressor.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"
#include "ECS/LuaAnimGraphBindings.h"
#include "Resource/AnimationClipSystem.h"
#ifdef WITH_EDITOR
// Importers are editor-only; runtime loads pre-imported .itex/.imsh/.imat/.ianim/.aclip.
#endif

// ==== Audio / UI / AI / Nav / Physics ====
#include "Audio/LuaAudioBindings.h"
#include "Diagnostics/LuaDebugBindings.h"     // Debug.*
#include "Diagnostics/LuaProfilerBindings.h"  // Profiler.*
#include "Renderer/Preview/LuaPreviewBindings.h" // Preview.*
#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "Diagnostics/FrameTiming.h"
#include "Diagnostics/OwnerAccounting.h"
#include "Intent/LuaIntentBindings.h"
#include "Intent/LuaAIBindings.h"
#include "Physics/LuaPhysicsBindings.h"
#include "Resource/PrefabSerializer.h"   // Resource::LoadPrefab (Engine.SpawnPrefab)
#include "Resource/LuaSettingsBindings.h" // Settings.* (in-game options menu)
#include "Scripting/LuaGameFlowBindings.h"
#include "PostProcess/LuaPostProcessBindings.h"
#include "ECS/PostProcessResolveSystem.h"

// ==== System / Input ====
#include "System/TaskSystem.h"
#include "System/Log.h"
#include "System/CommandLine.h"   // -scene / -frames / -hidden
#include "Harness/RunLedger.h"
#include "Harness/GameFlowDump.h"
#include "Harness/HarnessCaptureSystem.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Harness/Determinism.h"
#include "Harness/ControlChannel.h"
#include "Harness/CatalogDump.h"        // -dump_catalog (WITH_HARNESS only)
#include "Graphics/ShaderHotReload.h"     // pre-flighted shader reloads
#include "System/HotReloadService.h"      // HotReload::WatchService
#include "System/EventBus.h"
#include "Input/InputSystem.h"

#ifdef WITH_EDITOR
#include "Editor/EditorLayer.h"
#include "Editor/MaterialPreviewWidget.h"    // preview.enabled pref (harness force-on)
#include "Editor/ChainPhysicsPreviewWindow.h" // the preview's EnterPlay teardown
#include "Editor/MeshEditorWindow.h"          // -mesheditor_*
#include "Editor/SkeletonEditorWindow.h"      // -skeleton_editor*
#include "Editor/TextureViewerWindow.h"       // -texviewer_*
#include "Editor/MaterialEditorWindow.h"      // -mateditor_*
#include "Editor/NodeEditorSmoke.h"           // -nodeeditor_smoke*
#include "Editor/PreviewThumbnailCache.h"    // -preview_thumb_cache_dir
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

void App::RenderPhase(IGraphicsDevice& backend, Renderer& renderer, GameModeContext& ctx,
                      World& world, const FrameContext& frameCtx)
{
    // The frame's GAME half, on main. It ends in FramePipeline::SubmitFrame, which
    // hands the frame to the render half: that one opens the device frame
    // (backend.BeginFrame) and owns the [RENDERSPLIT] spans.
    // ---- GAME HALF starts here ----
    // The hard sync: complete accepted frame work before owner mutations, swap-chain
    // rebuilds or world teardown can invalidate its borrowed snapshots. Under
    // -renderowner DrainFrameWork routes to DrainRenderOwner and waits for the lane, so
    // everything downstream still runs with no render work in flight:
    // Window::ServicePendingSize -> ApplyResizeNow (swap-chain rebuild), SceneManager's
    // world teardown and its WaitForPreviousFrame drain, and
    // EditorLayer::ProcessPendingActions. None of them stops the lane: it is a
    // process-lifetime thread, started once by RenderHalf and joined once by
    // ShutdownAllSystems (packs renderowner_resize, renderowner_reload).
    // It is also the -animoverlap animation barrier: with the flag off this drain is a
    // no-op (the Animation-boundary one already ran); with it on this is the wait, and
    // it is measured here so [OWNERTIME] winMs= / tailMs= mean the same in both arms.
    m_pipeline.RenderPhaseBarrier();
    // ---- The structural sync point ----
    // The lane is quiescent (the drain above just returned) and the scene build has
    // not started: apply the render-side halves of the destroys recorded while a frame
    // was in flight (and the mailbox calls filed between them, in order), THEN hand
    // their ids back to the free list - never the other way round.
    // The RP.* scopes name this phase's steps on the engine profiler's timeline, so a
    // slow frame here (a prefab spawn's next one) is not one unscoped RenderSystem block.
    { PROF_SCOPE("RP.StructuralSync"); renderer.ApplyStructuralSyncPoint(); }
    // The frame's intent (Renderer/FramePlan.h) is captured here and nowhere else: the
    // render half is parked, and what this game half changes from now on (the editor
    // UI, the scene build) reaches the next frame's plan.
    renderer.CaptureFrameIntent();
    world.ReleaseQuarantinedIds();
    m_pipeline.VerifyAnimOverlap(backend, renderer, frameCtx);   // -animoverlapverify

    // None of this function runs inside the device frame: backend.BeginFrame opens
    // the render half after SubmitFrame; everything here stays on the game thread.
    // Keep ctx.viewport in display pixels: Canvas layout, mouse hit-testing,
    // ToneMap output and the editor viewport all share that coordinate space.
    // Only the HDR/GBuffer target receives FSR's ratio and renderScale.
    uint32_t hdrW = ctx.viewportW;
    uint32_t hdrH = ctx.viewportH;
    renderer.ComputeRenderResolution(ctx.viewportW, ctx.viewportH, hdrW, hdrH);
    // DLSS (as this frame's plan has it, Renderer::GamePlan) is exempt from the extra
    // renderScale multiply: its render extent comes from slDLSSGetOptimalSettings and
    // the feature validates the extent against that same table, so scaling it
    // afterwards fails silently. FSR has no such table and composes with renderScale.
    if (!renderer.GamePlan().dlssWanted)
    {
        const float renderScale = std::clamp(
            backend.MainSurface().GetPresentSettings().renderScale, 0.25f, 2.0f);
        hdrW = std::max(1u, static_cast<uint32_t>(
            std::lround(static_cast<float>(hdrW) * renderScale)));
        hdrH = std::max(1u, static_cast<uint32_t>(
            std::lround(static_cast<float>(hdrH) * renderScale)));
    }
    backend.SetViewportSize(hdrW, hdrH);

#ifdef WITH_EDITOR
    // Drain queued editor actions (Load World, ...) on the GAME half, with the render
    // half parked (RenderPhaseBarrier above) and before backend.BeginFrame in the
    // render half. Do not run them inline in the ImGui handler: that tears down
    // descriptor slots earlier ImGui::Image calls captured -> use-after-free.
    if (m_pipeline.RenderOwnerEnabled())
    {
        // The pick readback, with the render half parked: the lane is idle here, and
        // the pick lands before this frame's UI. (Not a harness pick - see the
        // non-renderowner site in App.cpp.)
        Entity pickedEntity;
        if (!renderer.IsHarnessPick() && renderer.ResolvePick(pickedEntity) &&
            m_playModeController.IsEditing())
            m_editorLayer.SetPickResult(pickedEntity);
    }
    { PROF_SCOPE("RP.EditorActions"); m_editorLayer.ProcessPendingActions(); }
#endif

    // Material-preview teardown rides the SAME slot, and not the ImGui handler that
    // asked for it: this is the only point at which a descriptor an earlier
    // ImGui::Image captured is provably unreferenced by any in-flight frame
    // (EditorLayer.h contract). Outside the WITH_EDITOR block on purpose: it is a
    // Renderer call, a no-op with no preview layer, and Game.exe reaching a preview
    // through a future in-game PiP must service it too.
    { PROF_SCOPE("RP.PreviewReleases"); renderer.ServicePreviewReleases(); }
    // The secondary views (Renderer::AcquireView / ReleaseView) are made and given back in the same
    // window, for the same reason: an earlier ImGui::Image of a released view's picture is unreferenced.
    { PROF_SCOPE("RP.Views"); renderer.ServiceViews(); }

    // ...and the preview GRAPH is built in this same window: RenderGraph::Compile
    // destroys and recreates every physical texture and runs each pass's
    // Init()/OnCompile() serially, all of which allocate descriptors. Do not move it
    // into Renderer::Render: that would allocate descriptors on (or beside) a graph
    // worker. Costs one branch when no panel is open.
    { PROF_SCOPE("RP.PreviewCompile"); renderer.ServicePreviewCompile(); }

#ifdef WITH_EDITOR
    // Second half of the WM_CLOSE veto installed in WireEditor: the modal has been
    // answered Save or Discard, so honour the close the WndProc swallowed.
    // PostQuitMessage only QUEUES WM_QUIT - Window::ProcessMessage picks it up at the
    // top of the next iteration, so this frame still records and presents normally
    // instead of tearing down mid-frame.
    if (m_editorLayer.ConsumeExitApproved())
        PostQuitMessage(0);
#endif

    // Engine.Quit() from script - same "queue it, finish the frame" contract as the
    // editor path above, and deliberately NOT inside the ifdef: a title-screen Quit
    // button is the whole reason this exists.
    if (m_quitRequested)
    {
        m_quitRequested = false;
        LOG_INFO("Engine.Quit(%d): quit requested from script", m_quitExitCode);
        PostQuitMessage(m_quitExitCode);
    }

    // Read the shadow-atlas demand here on the game half, so ServiceEnableGatedResources
    // (which may hard-sync) touches no World. The scan follows this frame's simulation, scene
    // load and editor actions, so their casters get the atlas this frame; a caster added
    // later in this game half (the editor UI) gets it next frame.
    { PROF_SCOPE("RP.ShadowDemand"); renderer.ExtractShadowAtlasDemand(world); }

    // TOP OF THE GAME HALF, and the position is load-bearing in two ways.
    // Legality: its disable paths hard-sync through WaitIdleAndReleaseDeferred, guarded
    // by CanHardSyncNow() = !m_inFrame || (cmdLists<=1 && !parallelRecording). Out
    // here there is no device frame and no open list at all - the strongest case. Do
    // not move it into Render: passes would already be recording.
    // Ordering: that hard sync ends in ProcessAllDeferredReleases, an UNCONDITIONAL
    // DrainReleasesUpTo(UINT64_MAX) that empties every stamped bucket including the
    // one for the frame in progress. It must run BEFORE this frame's own destroys
    // (TextureSystem::Tick's queue below, and the UI image SRV re-resolve after it):
    // placed after them, a toggle would recycle a descriptor heap slot under a handle
    // the game half had already captured - the aliasing that re-resolve prevents.
    { PROF_SCOPE("RP.EnableGated"); renderer.ServiceEnableGatedResources(ctx.viewportW, ctx.viewportH); }

    // GAME HALF, outside the device frame. What makes that safe:
    //   * Its deferred destroys go through GraphicsDX12::DestroyTexture into the
    //     FRAME-STAMPED release queue, which frees only once the stamped frame has
    //     retired: the queue, not the call position, guarantees GPU completion.
    //   * Its promote burst runs Begin/EndBufferUploadBatch, and each side has its OWN
    //     upload context (allocator, command list, private fence), so the flush waits
    //     on its own work and needs no open device frame.
    {
        ZoneScopedN("AssetTick");
        PROF_SCOPE("RP.AssetTick");
        // Also swaps in texture reloads (a reimport), with one GPU idle -
        // legal here for the reason ServiceEnableGatedResources is, and
        // before the destroys it flushes (TextureSystem::Tick).
        m_textureSys.Tick(m_resourceMgr, backend, frameCtx.frameIndex);
        // The prefab preloads' main steps, under their own budget: here for the same
        // reasons as the promotion above (no device frame, the render lane idle), after
        // it so a preload's budgeted textures were already offered their share.
        { PROF_SCOPE("RP.PreloadTick");
          Resource::PrefabSpawnQueue::Tick(backend, m_meshLib, m_textureSys, m_resourceMgr, m_frameIndex); }
        { PROF_SCOPE("RP.ClipTick"); m_animClipSys.Tick(); }
        { PROF_SCOPE("RP.ClipBinds"); m_animClipSys.ResolvePendingBinds(world, renderer); }
    }

    // UI paint (UIFramePainter::Paint in the UITick block below) runs AFTER
    // renderer.BeginFrame so world-space anchors project with THIS frame's
    // view-projection. Canvas layout/input was resolved before gameplay this frame.

#ifdef WITH_EDITOR
    // ---- The editor UI, built early (when the emit half is on the lane) ----
    // FramePipeline::BuildEarlyEditorUI: the lane is idle, the pick and the pending
    // actions have run, Renderer::BeginFrame has not.
    m_pipeline.BuildEarlyEditorUI(backend, renderer, world, ctx);
    renderer.SetPickingOutlineEntity(m_editorLayer.GetSelectedEntity());
    m_debugDraw.selectedEntity = m_editorLayer.GetSelectedEntity();   // the collision overlay's highlight
    // Push the editor's debug category toggles onto the renderer-owned
    // buckets BEFORE BeginFrame's wire-gather reads them. Editor-only:
    // Game builds never enable debug visuals.
    m_debugDraw.ApplyTo(renderer);
    // Force the debug wire pass on (before BeginFrame runs Clear()) when the
    // selected entity has authored chain-physics groups to visualize.
    m_editorLayer.PrepareChainPhysicsOverlay(renderer, world);
    // The same for the viewport's Show > Skeleton.
    m_editorLayer.PrepareSkeletonOverlay(renderer, world);
#endif
    // Simulation is done: SimulationEnd belongs to the GAME half and takes the GAME
    // token. Its twin RenderSubmitStart is emitted in the render half. Do not move
    // RenderSubmitStart back here: Reflex sizes the next frame's hold from the
    // RenderSubmitStart..End span, which would then swallow the game-to-render
    // handover and the queue wait - latency silently gets worse, with no error.
    SL::StreamlineBackend::Get().SetLatencyMarker(
        SL::LatencyMarker::SimulationEnd,
        SL::StreamlineBackend::Get().GameFrameToken());

    std::optional<Sys::FrameTaskContext> cpuFrame;
    m_pipeline.EmplaceFrameTaskContext(cpuFrame, backend, ctx);   // none under -brslane
    const auto _tb0 = std::chrono::steady_clock::now();
    {
        PROF_SCOPE("RP.BeginFrame");
        renderer.BeginFrame(world, ctx.frame, ctx.deltaTime,
                            ctx.viewportW, ctx.viewportH, cpuFrame ? &*cpuFrame : nullptr);
    }
    const auto _tb1 = std::chrono::steady_clock::now();
    // GAME -> RENDER: this frame's game-half term travels in the packet.
    m_pipeline.SetGameHalfMs(std::chrono::duration<double, std::milli>(_tb1 - _tb0).count());

#ifdef WITH_EDITOR
    // Single editor-only debug submission point - emits the external wireframes
    // (collision mesh, the modules') for enabled categories. Must run AFTER BeginFrame
    // (DebugWirePass ring slot is live) and BEFORE renderer.Render (which uploads and
    // draws the wire buffer).
    m_debugDraw.Submit(renderer, world, frameCtx, m_physicsSystem);
    // Chain-physics (KawaiiPhysics-style) authoring overlay - root bone, simulated
    // chain bones + links, and excluded bones for the selection.
    m_editorLayer.EmitChainPhysicsOverlay(renderer, world);
    // The selection's skeleton and sockets (Show > Skeleton).
    m_editorLayer.EmitSkeletonOverlay(renderer, world);
#endif

#ifdef WITH_EDITOR
    // The UI paint's editor input: the Canvas wireframe (off while a scene transition
    // covers the screen) and the selection it highlights.
    const bool     uiWireframe          = m_editorLayer.GetShowUICanvasWireframe()
                                         && !m_transition.IsActive();
    const uint32_t uiWireframeSelection = m_editorLayer.GetSelectedEntity();
#else
    const bool     uiWireframe          = false;
    const uint32_t uiWireframeSelection = 0u;
#endif
    {
        ZoneScopedN("UITick");
        PROF_SCOPE("RP.UITick");
        m_uiFramePainter.Paint(world, frameCtx, ctx, backend, renderer, m_textureSys, m_resourceMgr,
                               m_uiCanvasSystem, m_worldSpaceUISystem, uiWireframe,
                               uiWireframeSelection);   // UI/UIFramePainter.cpp
        // Build the world-space UI vertex lane HERE, on the game side, and AFTER the
        // paint: WorldSpaceUISystem::Tick runs inside it and produces the per-frame state
        // the build reads (alpha and scale, the cull flag, the damage-number offset, the
        // resolved custom-shader slot), and bakes the glyphs the text layout looks up.
        // Extracting earlier reads all of that one frame stale and drops newly-baked
        // glyphs for a frame.
        renderer.ExtractWorldUI(world);
    }

    // Scene-transition fade/loading overlay - appended on top of the UI draw list
    // (bypasses ECS, so the new scene's world.Clear() cannot wipe it mid-fade).
    // Consumed and cleared by the UIPass inside renderer.Render.
    if (UIPass* uip = renderer.GetUIPass())
    {
        m_transition.DrawOverlay(uip->GetDrawList(), ctx.viewportW, ctx.viewportH);
        // The game views' HUDs too: a transition covers every picture of the game.
        m_uiFramePainter.ForEachViewHud([&](uint32_t slot, uint32_t w, uint32_t h)
                                        { m_transition.DrawOverlay(uip->ViewDrawList(slot), w, h); });
    }

    // ---- Debug HUD ("stat unit") ----
    // The only legal window: AFTER renderer.BeginFrame and BEFORE FlushPendingUploads
    // below (Font::DrawText bakes missing glyphs into the CPU atlas mirror and only
    // that flush uploads dirty pages; emit later and a new glyph renders from stale
    // texels for one frame).
    // GetFrameStats() is the newest census on this thread (BeginFrame updates
    // m_frameStats); under -brslane the emit half runs on the render-owner lane after
    // the hand-off, so it is frame F-1's. FrameStats::statsFrame says which frame; a
    // pack that asserts on the numbers must read statsFrame / statsValid.
    // Drawn last so it sits on top of game UI, which also makes it the first thing
    // UIPass truncates when the shared 65535-index draw list overflows; hence EmitFrame
    // passes the remaining index budget.
    m_debugHud.EmitFrame(renderer, ctx.viewportW, ctx.viewportH);   // UI/DebugHudFrame.cpp

    // -materialcpuverify's [FRAME_PREPARE_OWNER_DONE] marker, outside the
    // `if (cpuFrame)` below (FramePipeline::LogPrepareOwnerDone says why both arms print it).
    m_pipeline.LogPrepareOwnerDone(renderer, ctx, cpuFrame.has_value());
    if (cpuFrame) {
        // UI/debug extraction does not consume material upload bytes: let packing run
        // during that owner work, then publish before any render packet or font flush can
        // reach a GPU consumer. Wait/commit time counts in the same frame's Draw term,
        // excluding the intervening UI.
        const auto prepareEndBegin = std::chrono::steady_clock::now();
        { PROF_SCOPE("RP.FinishPrep"); renderer.FinishFramePreparation(*cpuFrame); }
        const double prepareEndMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - prepareEndBegin).count();
        m_pipeline.AddGameHalfMs(prepareEndMs);
    }

    // Upload every font-atlas page dirtied by this frame's on-demand
    // glyph baking (Canvas text / world text / transition overlay above).
    // Must land before the graph records the UI passes; only frames that
    // actually baked new glyphs pay the synchronous upload.
    { PROF_SCOPE("RP.FontFlush"); UI::FontLibrary::Get().FlushPendingUploads(); }

    // Publish the bounded frame transaction - the packet and its emit lease - and run
    // RenderHalf (Renderer/FramePipeline.cpp).
    { PROF_SCOPE("RP.Submit"); m_pipeline.SubmitFrame(backend, renderer, ctx); }
}
