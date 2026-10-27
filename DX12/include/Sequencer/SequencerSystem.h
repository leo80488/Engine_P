#pragma once

// SequencerTickSystem - the cinematic Sequencer's runtime.
//
// Phase GameplayPostLogic, for two reasons. It is the only gameplay phase with
// allowsParallel == false, and Spawnables need direct CreateEntity / DestroyEntity
// (CommandBuffer does not expose them; from a parallel phase that is a heap-corruption
// race - a static_assert in the .cpp guards the flag). And it runs upstream of every
// same-frame consumer: TransformPropagate, MotionDirector, CameraMotion, Animation,
// CameraStack/Resolve and PostProcessResolve.
//
// Run mask Always, not Play, and the clock is not gated on ctx.runUpdate: ctx.deltaTime
// is real wall-clock in Edit too, so the inspector transport, editor previews and
// headless Edit-mode playback all play, not only a hand-dragged scrubber.

#include "ECS/ECS.h"       // Entity, which ReleasePlayer takes
#include "ECS/ISystem.h"
#include "Sequencer/SequenceEval.h"   // Seq::OrphanedPlayerRelease, the listener's snapshot

#include <cstdint>
#include <memory>
#include <vector>

class Renderer;
namespace Resource { class AnimationClipSystem; class AssetManager; }
namespace Gameplay { class GameFlowStateMachine; }

class SequencerTickSystem final : public SystemInPhase<TickPhase::GameplayPostLogic>
{
public:
    // Every pointer may be null (headless / tools). `renderer` resets temporal history
    // across a cut; `clips` binds a clip alias for the Skeletal Animation track;
    // `gameFlow` is what a cutscene switches state on; `assetMgr` spawns prefab Spawnables.
    explicit SequencerTickSystem(Renderer* renderer = nullptr,
                                 Resource::AnimationClipSystem* clips = nullptr,
                                 Gameplay::GameFlowStateMachine* gameFlow = nullptr,
                                 Resource::AssetManager* assetMgr = nullptr)
        : m_renderer(renderer), m_clips(clips), m_gameFlow(gameFlow), m_assetMgr(assetMgr) {}

    const char* GetName() const override { return "SequencerTickSystem"; }

    // Runs in Edit as well as Play (see the file comment).
    SystemRunMask RunMask() const override { return SystemRunMask::Always; }

    void OnRegister(World& world) override;
    void OnUnregister(World& world) override;
    void Update(World& world, const FrameContext& ctx) override;

    // Opaque: the evaluator writes component types the asset chooses at runtime (a
    // Property track), so no static access set exists. GameplayPostLogic is serial anyway.
    void DeclareAccess(SystemAccessBuilder& b) const override { b.OpaqueAccess(); }

    // Drops every claim and restores every pre-animated state. Called on world clear and
    // at Edit<->Play transitions: World::AdoptState swaps pools without firing destroy
    // listeners, so a sequence live across that boundary would keep its actors claimed.
    static void ReleaseAll(World& world);

    // Synchronously releases one player: despawns its Spawnables and child sub-sequences,
    // drops its claims, restores its pre-animated state.
    // Anything that destroys a player entity must call this first: `stopRequested` is
    // consumed by the next Update, which never sees a destroyed entity, so everything that
    // player spawned would be orphaned.
    static void ReleasePlayer(World& world, Entity player);

private:
    Renderer*                      m_renderer = nullptr;
    Resource::AnimationClipSystem*  m_clips   = nullptr;
    Gameplay::GameFlowStateMachine* m_gameFlow = nullptr;
    Resource::AssetManager*         m_assetMgr = nullptr;

    // Entity-destroy listener, so a destroyed actor leaves the pre-animated cache instead
    // of being restored onto whatever recycles its id.
    uint32_t m_destroyListener = 0;
    bool     m_destroyListenerValid = false;

    // One temporal-history reset per frame across all players: during a scrub drag every
    // frame is a discontinuity, and the underlying call is global.
    uint64_t m_lastHistoryResetFrame = ~0ull;

    // Entities orphaned because their player was destroyed without ReleasePlayer (an
    // outliner delete, a Lua Destroy, a prefab teardown). The destroy listener cannot
    // destroy them (re-entrant DestroyEntity), so it queues them for the next Update.
    // Shared and captured by value: the listener is owned by the World, which can outlive
    // this system and fires listeners from its own destructor. Do not capture `this`.
    std::shared_ptr<std::vector<Entity>> m_orphanedSpawns =
        std::make_shared<std::vector<Entity>>();

    // Restore debts the same listener leaves when the destroyed entity was a live player:
    // restoring inline would mutate pools inside the outer DestroyEntity, so the listener
    // snapshots what the release needs and the next Update pays it. Shared / by value for
    // the same reason as m_orphanedSpawns.
    std::shared_ptr<std::vector<Seq::OrphanedPlayerRelease>> m_pendingReleases =
        std::make_shared<std::vector<Seq::OrphanedPlayerRelease>>();
};
