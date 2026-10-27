// PlayModeSession: the Edit <-> Play world snapshot and restore; see ECS/PlayModeSession.h.

#include "ECS/PlayModeSession.h"

#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Diagnostics/OwnerAccounting.h"
#include "ECS/CameraStackSystem.h"
#include "ECS/GuidRegistry.h"
#include "Gameplay/GameFlowState.h"
#include "Graphics/IGraphicsDevice.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/Renderer.h"
#include "Scripting/ScriptSystem.h"
#include "Sequencer/SequenceEval.h"
#include "Sequencer/SequencerSystem.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <vector>

namespace
{
    // True under -pie_gamestate_leak (harness builds only): Stop leaves the gameplay store as Play
    // left it.
    bool LeaksGameState()
    {
#if WITH_HARNESS
        static const bool s_leak = Sys::CommandLine::Get().Has("-pie_gamestate_leak");
        return s_leak;
#else
        return false;
#endif
    }
}

// ==== Edit / Play world snapshot and restore (state isolation) ====
// EnterPlay (Edit -> Playing): deep-copy the pristine editor world so Stop can restore it bit for
// bit, then cut temporal history (the viewport jumps from the editor fly camera to the game camera
// stack).
void PlayModeSession::EnterPlay(Renderer& renderer)
{
    m_drainFrameWork(Diag::DrainCause::PlayModeEdge);
    // Zoned, with sub-zones, because the Edit -> Play edge is ~25 ms of serial main-thread CPU with
    // the GPU idle: a capture must say which step costs it.
    ZoneScopedN("EnterPlay");
    {
        // Before the snapshot: a live editor Sequencer preview has spawned cameras / fade quads / child
        // sequences in the editor world. CloneState would bake them in, and ExitPlay would restore them
        // owned by a player whose runtime half comes back scrubbed: orphans, a fresh set per Play press.
        ZoneScopedN("EnterPlay.Sequencer");
        SequencerTickSystem::ReleaseAll(m_world);
        Seq::Preview::SetOwnsViewport(false);
    }
    if (beforeSnapshot)
    {
        // The editor's preview stages (Chain Physics Preview, Skeleton Editor, UI Editor) live in THIS
        // world and CloneState has no opt-out: beforeSnapshot tears them down here, before the snapshot,
        // and their panels rebuild them after Stop. Same position and reason as the Sequencer release above.
        ZoneScopedN("EnterPlay.ChainPreview");
        beforeSnapshot(renderer, m_world);
    }
    {
        ZoneScopedN("EnterPlay.CloneState");
        m_editorWorldSnapshot = std::make_unique<World>(m_world.CloneState());
    }
    {
        ZoneScopedN("EnterPlay.CaptureResources");
        renderer.CaptureEditorWorldResources(*m_editorWorldSnapshot);
    }
    m_gameStateSnapshot = Gameplay::GameState::Get().Capture();
    m_physicsAccumulator  = 0.f;
    // Remember the scene binding this Play enters, so Stop can queue it again.
    m_scriptSystem.BeginPlaySession();
    // A fresh Play session always starts from the current animation/rest
    // pose. Discard particles, Verlet history, spring velocity, previous
    // capsules, and any LocalPose rotations written by the previous run.
    {
        ZoneScopedN("EnterPlay.ResetChainPhysics");
        renderer.ResetChainPhysicsSimulation();
    }
    renderer.InvalidateTemporalHistory();
    Camera::HardCutTo(m_world, NullEntity, Camera::kMainChannel);
    LOG_INFO("EnterPlay — snapshotted editor world (%zu entities)",
             m_world.GetEntities().size());
}

// ExitPlay (-> Edit): restore the editor snapshot IN PLACE (m_world's address never changes, so
// every system's cached World* and listeners stay valid). Order matters:
//   (0)  Audio: stop the voices, release the Play-acquired clip refs. (0b) Release any cinematic.
//   (1)  Destroy the play-transient entities (alive now, absent from the snapshot) through
//        DestroyEntity so their destroy listeners FIRE: they clean the renderer's entity-keyed
//        caches, release audio voices, run Lua OnDestroy and unregister GUIDs. AdoptState swaps
//        pools wholesale without listeners, so otherwise a recycled id would inherit stale caches.
//   (2)  GPU idle, then Renderer / Physics OnWorldClear: reset world-scoped GPU descriptors and
//        destroy every Jolt body (survivors' bodies were created during Play; they are rebuilt
//        lazily on the next Play). The full renderer clear is needed because a Play-time
//        Scene.Load can recycle the same ids; EnterPlay pins the snapshot's MeshLibraries, so it
//        is safe.
//   (3)  ClearInstances drops per-entity Lua state so the next Play starts from fresh scripts.
//   (4)  AdoptState moves the snapshot's component values back over the surviving editor
//        entities; then restore resources, rebuild the GUID map, reset GameFlow, cut history.
void PlayModeSession::ExitPlay(Renderer& renderer, IGraphicsDevice& backend)
{
    m_drainFrameWork(Diag::DrainCause::PlayModeEdge);
    // Zoned, with sub-zones, because Stop is a user-visible hitch of serial main-thread CPU with the
    // GPU idle: a capture must attribute it.
    ZoneScopedN("ExitPlay");

    // Stop is a destructive reset for chain physics, not a pause. Disable
    // it before any world teardown, then discard all state even if snapshot
    // restoration has to bail out below.
    renderer.SetChainPhysicsSimulationEnabled(false);
    renderer.ResetChainPhysicsSimulation();

    // The played scene ends first, with the play world intact: OnSceneExit and the scene-scoped
    // Systems' OnShutdown run, their resources are released, and the editor's scene is queued to
    // enter fresh on the next Play.
    {
        ZoneScopedN("ExitPlay.SceneExit");
        m_scriptSystem.EndPlaySession();
    }
    // After the scene's exit scripts, which may write the store too.
    if (m_gameStateSnapshot)
    {
        if (!LeaksGameState())
            Gameplay::GameState::Get().Restore(std::move(*m_gameStateSnapshot));
        m_gameStateSnapshot.reset();
    }

    if (!m_editorWorldSnapshot)
    {
        LOG_WARNING("ExitPlay — no editor snapshot to restore (skipped)");
        return;
    }
    // (0) Silence everything: stop every live voice so play sounds do not bleed into Edit, and
    //     release the clip refcounts AudioSystem acquired during Play. OnWorldClear must run BEFORE
    //     AdoptState (it walks the live play-world AudioSource pool); AdoptState swaps pools without
    //     firing destroy listeners, so otherwise the survivors' Play-acquired clip refs would leak on
    //     every Play -> Stop.
    {
        ZoneScopedN("ExitPlay.Audio");
        m_audioEngine.StopAll();
        m_audioSystem.OnWorldClear();
    }

    // (0b) Release any live cinematic. A Sequencer holds pose-authority
    //      claims (AnimPoseOverrideComponent) and a pre-animated cache
    //      keyed on live entity ids. AdoptState below swaps the pools
    //      WITHOUT firing destroy listeners, so a sequence still running at
    //      Stop would leave its actors claimed forever and its cache
    //      pointing at ids the editor world reuses for something else.
    {
        ZoneScopedN("ExitPlay.Sequencer");
        SequencerTickSystem::ReleaseAll(m_world);
        // Hard reset, not an opt-in call site: this global parks the editor
        // free-cam, and a stuck `true` leaves the user with no camera they
        // can drive and no obvious way back.
        Seq::Preview::SetOwnsViewport(false);
        // Same reasoning: a cutscene stopped by leaving Play must not leave
        // the game in slow motion.
        Seq::TimeScale::Clear();
    }

    // (1) Destroy play-spawned (transient) entities with full per-entity teardown. Copy the id list
    //     first: DestroyEntity mutates m_world's entity vector.
    std::vector<Entity> transient;
    {
        ZoneScopedN("ExitPlay.DestroyTransient");
        for (Entity e : m_world.GetEntities())
            if (!m_editorWorldSnapshot->IsAlive(e))
                transient.push_back(e);
        for (Entity e : transient)
            m_world.DestroyEntity(e);
    }

    // (2) (3) Wipe runtime backing outside the ECS pools. A Lua Scene.Load may have replaced the
    // whole live world and reset the renderer descriptor heap, so a hard GPU idle + full renderer
    // clear is required even when the new scene recycled the snapshot's entity ids.
    {
        ZoneScopedN("ExitPlay.WaitIdleAndRelease");
        backend.WaitIdleAndReleaseDeferred();
    }
    {
        ZoneScopedN("ExitPlay.RendererClear");
        // idsPreserved: AdoptState below restores the SAME world from the EnterPlay snapshot, so entity
        // ids keep naming the same skinned mesh and DDGISceneAS keeps its per-entity skinned BLAS
        // allocations (re-allocating them makes Stop a ~120 ms frame).
        renderer.OnWorldClear(/*idsPreserved=*/true);
    }
    {
        ZoneScopedN("ExitPlay.PhysicsClear");
        // Keep the mesh / hull shape cache: Stop restores a snapshot of the SAME scene, so every built
        // shape is still valid; dropping it would re-run PrewarmMeshShapes on every Play press (~39 ms of
        // hull / BVH construction on an imported level). Safe because the cache key carries the source
        // file's modification stamp (MeshShapeKey).
        m_physicsSystem.OnWorldClear(/*dropShapeCache=*/false);
    }
    {
        ZoneScopedN("ExitPlay.ClearScripts");
        m_scriptSystem.ClearInstances();
    }

    // (4) Restore the pristine editor component values + identity.
    {
        ZoneScopedN("ExitPlay.AdoptState");
        // Move: AdoptState CONSUMES the snapshot (dead after this anyway); a copy would be a second deep
        // copy of every component plus two World destructions, most of the Stop hitch. reset() then
        // destroys an emptied World for free, inside the zone so the measurement stays honest.
        m_world.AdoptState(std::move(*m_editorWorldSnapshot));
        m_editorWorldSnapshot.reset();
    }
    {
        ZoneScopedN("ExitPlay.RestoreResources");
        renderer.RestoreEditorWorldResources(m_world);
    }
    {
        ZoneScopedN("ExitPlay.RebuildGuids");
        ECS::GuidRegistry::Get().RebuildFromWorld(m_world);
    }
    // GameFlow lives outside ECS, so reset its runtime leaf explicitly and
    // replay the first boot request on the next Play session.
    m_gameFlow.ResetRuntime(/*queueInitial=*/true);
    m_physicsAccumulator = 0.f;
    renderer.InvalidateTemporalHistory();
    Camera::HardCutTo(m_world, NullEntity, Camera::kMainChannel);
    LOG_INFO("ExitPlay — restored editor world (%zu entities, %zu transient destroyed)",
             m_world.GetEntities().size(), transient.size());
}
