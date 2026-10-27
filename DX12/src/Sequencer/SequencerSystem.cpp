#include "Sequencer/SequencerSystem.h"

#include "ECS/AnimationComponents.h"   // AnimPoseOverrideComponent, the pose-authority claim
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"   // Children: the orphan sweep walks subtrees
#include "ECS/TickPhase.h"
#include "Sequencer/SequenceComponents.h"
#include "Sequencer/SequenceEval.h"
#include "Gameplay/GameFlowState.h"
#include "Gameplay/GameState.h"
#include "Resource/PrefabSpawnQueue.h"   // PreloadSpawnables
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>


namespace
{
    // Preloads a sequence's prefab spawnables when a player takes the asset, so the shot's
    // first frame (ClaimActors -> SpawnSpawnables) spawns them warm. Dropping the returned
    // hold releases them (asset replaced, player destroyed, world cleared).
    std::shared_ptr<void> PreloadSpawnables(const Seq::Sequence& seq)
    {
        using Id = Resource::PrefabSpawnQueue::PreloadId;
        std::vector<Id> ids;
        for (const Seq::Binding& b : seq.bindings)
            if (b.IsSpawnable() && b.archetype == Seq::SpawnArchetype::FromPrefab && !b.prefabPath.empty())
                if (const Id id = Resource::PrefabSpawnQueue::RequestPreload(b.prefabPath, {}))
                    ids.push_back(id);
        if (ids.empty()) return nullptr;
        return std::shared_ptr<void>(new std::vector<Id>(std::move(ids)), [](void* held)
        {
            auto* v = static_cast<std::vector<Id>*>(held);
            for (const Id id : *v) Resource::PrefabSpawnQueue::ReleasePreload(id);
            delete v;
        });
    }

    // True when a world clear (the editor's Play / Stop one too) took the GPU half of any
    // of these preloads with it.
    bool SpawnablePreloadsStale(const std::shared_ptr<void>& held)
    {
        using Id = Resource::PrefabSpawnQueue::PreloadId;
        for (const Id id : *static_cast<const std::vector<Id>*>(held.get()))
            if (Resource::PrefabSpawnQueue::GetPreloadState(id) == Resource::PrefabSpawnQueue::PreloadState::Stale)
                return true;
        return false;
    }
}

// The Spawnable lifecycle does direct CreateEntity / DestroyEntity inside Update, which
// is safe only because this phase is serial. Nothing else enforces the coupling, so
// flipping the flag is a build break here rather than an intermittent heap corruption.
static_assert(GetPhaseDescriptor(TickPhase::GameplayPostLogic).allowsParallel == false,
              "SequencerTickSystem performs direct structural ECS changes; "
              "TickPhase::GameplayPostLogic must remain serial.");

namespace
{
    // The dt clamp is Seq::kMaxSequenceStep (SequenceClock.h), applied in Seq::AdvanceClock.

    // ---- Gameplay hold ----
    // The flow holds the cutscene, not the player: the cutscene's state declares its own
    // `input_context`, and the state it came from lives in the flow's one-deep return slot.
    // Whether a release is owed is asked of the machine, never of a copy, so a flow that
    // moved on for another reason is not switched back to a state this cutscene never left.
    void TakeGameFlow(SequencePlayerComponent& p,
                      Gameplay::GameFlowStateMachine* gameFlow)
    {
        if (p.gameFlowState.empty() || !gameFlow) return;
        // An overlay switch, so the flow records where to come back to. A repeat call is
        // harmless: entering the current state is refused as AlreadyCurrent.
        const Gameplay::SwitchResult result =
            gameFlow->TryRequestSwitchOverlay(p.gameFlowState);
        if (result != Gameplay::SwitchResult::Accepted
            && result != Gameplay::SwitchResult::AlreadyCurrent)
        {
            LOG_ERROR("Sequencer: could not take GameFlow state '%s' (%s); the "
                      "cutscene will play with gameplay still active",
                      p.gameFlowState.c_str(), Gameplay::ToString(result));
        }
    }

    void ReleaseGameFlow(SequencePlayerComponent& p,
                         Gameplay::GameFlowStateMachine* gameFlow)
    {
        if (p.gameFlowState.empty() || !gameFlow) return;
        // Give it back only while we still hold it: whatever took the flow meanwhile (a death,
        // a disconnect, a higher-priority switch) outranks this cutscene.
        if (!gameFlow->IsInState(p.gameFlowState)) return;
        gameFlow->RequestReturn();
    }

    // True while this player is playing or still holds the flow (asked of the machine, so
    // the answer cannot drift).
    bool SequenceIsLive(const SequencePlayerComponent& p,
                        const Gameplay::GameFlowStateMachine* gameFlow)
    {
        if (p.playing) return true;
        return gameFlow && !p.gameFlowState.empty()
            && gameFlow->IsInState(p.gameFlowState);
    }

    // Fired on every end (natural, skipped or stopped), as an edge so gameplay need not
    // poll. Takes copies, not the component: every caller has just run ReleaseOne, which
    // can move the component.
    void PublishFinished(Entity self, const std::string& sequencePath,
                         bool recordWatched, bool skipped)
    {
        EventBus::Get().Publish(SequenceFinishedEvent{ self, skipped, sequencePath });
        if (recordWatched && !sequencePath.empty())
            Gameplay::GameState::Get().SetBool("seq.watched." + sequencePath, true);
    }

    // Shared by the stop, skip and natural-end edges. Takes the entity, not component
    // references: ReleaseActors destroys this player's sub-sequence children (players too),
    // so their pools can compact during the call. Writes use a fresh fetch, and callers
    // must re-fetch their own p / rt afterwards.
    void ReleaseOne(World& world, Entity self, EngineMode mode)
    {
        if (auto* rt = world.GetComponent<SequenceRuntimeComponent>(self); rt && rt->asset)
        {
            // The local ref keeps the asset alive: the slot `rt` occupies can be compacted away
            // mid-teardown.
            const std::shared_ptr<const Seq::Sequence> asset = rt->asset;
            Seq::ReleaseActors(world, *asset, *rt, asset->restoreMode, mode);
        }
        // Time dilation is not a component, so the pre-animated cache cannot restore it: drop
        // this player's claim, or a stopped cutscene leaves the game in slow motion.
        Seq::TimeScale::Release(static_cast<uint32_t>(self));

        if (auto* p = world.GetComponent<SequencePlayerComponent>(self))
        {
            p->playing   = false;
            p->prevFrame = -1;
            p->subFrame  = 0.f;
        }
    }
}

// ==== Lifecycle ====

void SequencerTickSystem::OnRegister(World& world)
{
    world.EnsurePool<SequencePlayerComponent>();
    world.EnsurePool<SequenceRuntimeComponent>();
    world.EnsurePool<AnimPoseOverrideComponent>();
    world.EnsurePool<SeqSpawnedTag>();

    // A destroyed actor must leave the pre-animated cache: entity ids are recycled, so a
    // stale entry would restore one entity's components onto an unrelated one.
    m_destroyListener = world.AddEntityDestroyListener(
        [orphans = m_orphanedSpawns, pending = m_pendingReleases,
         flow = m_gameFlow, &world](Entity dead)
    {
        // The destroyed entity may be a player: queue its Spawnables and child sub-sequences,
        // or they stay in the world forever (an outliner delete or a Lua Destroy does this).
        // Queued, not destroyed: this fires inside DestroyEntity, and destroying re-entrantly
        // would mutate the entity table the outer call is walking.
        if (auto* dying = world.GetComponent<SequenceRuntimeComponent>(dead))
        {
            for (Entity e : dying->spawned)
                if (e != NullEntity) orphans->push_back(e);
            for (const auto& c : dying->children)
                if (c.entity != NullEntity) orphans->push_back(c.entity);

            // A live player dying here skipped ReleasePlayer: move its claims and pre-animated
            // cache out now and let the next Update restore them (inline is unsafe, as above).
            // Sanctioned teardowns already ran ReleaseActors, which clears `armed`: a no-op for them.
            if (dying->armed && dying->asset)
            {
                Seq::OrphanedPlayerRelease rec;
                rec.asset    = dying->asset;
                rec.bound    = dying->bound;
                rec.preAnim  = std::move(dying->preAnim);
                dying->armed = false;
                pending->push_back(std::move(rec));
            }
            // Side table, not a component: safe inline, and the last moment anything knows this
            // entity may hold a TimeScale claim.
            Seq::TimeScale::Release(static_cast<uint32_t>(dead));
        }

        // Same for the flow: a player destroyed mid-cutscene never reaches its release path,
        // so its state would stay current with its input context pushed (the pad blocked for
        // the session). RequestReturn only queues a switch (run at the next Input phase), so
        // it is safe inside a destroy listener.
        if (auto* p = world.GetComponent<SequencePlayerComponent>(dead))
            ReleaseGameFlow(*p, flow);

        auto* pool = world.GetPool<SequenceRuntimeComponent>();
        if (!pool) return;
        for (SequenceRuntimeComponent& rt : pool->Data())
        {
            auto& pa = rt.preAnim;
            pa.erase(std::remove_if(pa.begin(), pa.end(),
                                    [dead](const SequenceRuntimeComponent::PreAnim& e)
                                    { return e.entity == dead; }),
                     pa.end());
            for (Entity& b : rt.bound) if (b == dead) b = NullEntity;
        }
    });
    m_destroyListenerValid = true;
}

void SequencerTickSystem::OnUnregister(World& world)
{
    ReleaseAll(world);
    if (m_destroyListenerValid)
    {
        world.RemoveEntityDestroyListener(m_destroyListener);
        m_destroyListenerValid = false;
    }
}

void SequencerTickSystem::ReleaseAll(World& world)
{
    auto* pPlayer = world.GetPool<SequencePlayerComponent>();
    if (!pPlayer) return;

    // Snapshot the entity list and look everything up by entity: releasing a player
    // destroys its sub-sequence children, which are players too, so the release removes
    // entries from the very pool this loop walks.
    const std::vector<Entity> ents = pPlayer->Entities();

    for (const Entity e : ents)
    {
        if (!world.IsAlive(e)) continue;            // already destroyed as someone's child

        auto* rt = world.GetComponent<SequenceRuntimeComponent>(e);
        if (rt && rt->asset)
        {
            // Always restore, whatever the asset's RestoreMode: this is world teardown or a mode
            // switch, and "leave the actors posed" only means posed within THIS world.
            Seq::ReleaseActors(world, *rt->asset, *rt, Seq::RestoreMode::Always, EngineMode::Edit);
        }
        Seq::TimeScale::Release(static_cast<uint32_t>(e));

        // Re-fetch: ReleaseActors above may have reallocated the player pool.
        auto* p = world.GetComponent<SequencePlayerComponent>(e);
        if (!p) continue;
        // The flow is not released here on purpose: this runs on the editor's Stop, where
        // GameFlowStateMachine::ResetRuntime clears the current state on the same edge (App
        // pops the emptied input contexts next frame). A return switch would queue a
        // transition into a runtime being torn down.
        p->playing   = false;
        p->prevFrame = -1;
        p->subFrame  = 0.f;
    }
}

void SequencerTickSystem::ReleasePlayer(World& world, Entity player)
{
    if (player == NullEntity || !world.IsAlive(player)) return;

    auto* rt = world.GetComponent<SequenceRuntimeComponent>(player);
    if (rt && rt->asset)
    {
        // RestoreMode::Always whatever the asset says: this is a teardown, and "leave the
        // actors posed" only means posed within a world that still has this sequence.
        Seq::ReleaseActors(world, *rt->asset, *rt, Seq::RestoreMode::Always, EngineMode::Edit);
    }
    Seq::TimeScale::Release(static_cast<uint32_t>(player));

    if (auto* p = world.GetComponent<SequencePlayerComponent>(player))
    {
        // No flow release: the only caller is SequencerWindow's inspector teardown, and in
        // Edit mode the flow has no current state to give back.
        p->playing       = false;
        p->stopRequested = false;
        p->prevFrame     = -1;
        p->subFrame      = 0.f;
    }
}

// ==== Update ====

void SequencerTickSystem::Update(World& world, const FrameContext& ctx)
{
    ZoneScopedN("Sequencer.Tick");

    // Debts left by the destroy listener (players destroyed without ReleasePlayer),
    // restored before the orphan sweep so the cast is back in its pre-animated state the
    // same tick its player disappeared.
    if (m_pendingReleases && !m_pendingReleases->empty())
    {
        std::vector<Seq::OrphanedPlayerRelease> due;
        due.swap(*m_pendingReleases);
        for (Seq::OrphanedPlayerRelease& rec : due)
            Seq::ReleaseOrphanedPlayerState(world, rec);
        LOG_WARNING("Sequencer: restored %zu live player(s) destroyed without "
                    "SequencerTickSystem::ReleasePlayer", due.size());
    }

    // Sweep anything orphaned by a player destroyed outside ReleasePlayer.
    // Whole subtrees, because a prefab Spawnable is a subtree.
    if (m_orphanedSpawns && !m_orphanedSpawns->empty())
    {
        std::vector<Entity> queued;
        queued.swap(*m_orphanedSpawns);
        for (Entity root : queued)
        {
            if (root == NullEntity || !world.IsAlive(root)) continue;
            std::vector<Entity> all, stack{ root };
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (!world.IsAlive(e)) continue;
                all.push_back(e);
                if (const Children* ch = world.GetComponent<Children>(e))
                    for (Entity c : ch->entities) stack.push_back(c);
            }
            for (auto it = all.rbegin(); it != all.rend(); ++it)
                if (world.IsAlive(*it)) world.DestroyEntity(*it);
        }
        LOG_WARNING("Sequencer: swept %zu entity subtree(s) orphaned by a player that was "
                    "destroyed without SequencerTickSystem::ReleasePlayer", queued.size());
    }

    // ---- Orphan GC ----
    // Everything a sequence creates carries SeqSpawnedTag with its owner's id. One whose
    // owner is dead, has no runtime component or is not armed is unreachable by any release
    // path. Example: EnterPlay deep-copies a world with a live editor preview, and ExitPlay
    // restores its spawns under an owner whose SequenceRuntimeComponent the ComponentCloner
    // reset (by design) - one stray camera and fade quad per Play press.
    if (auto* pTag = world.GetPool<SeqSpawnedTag>(); pTag && !pTag->Data().empty())
    {
        std::vector<Entity> orphans;
        const auto& tagEnts = pTag->Entities();
        const auto& tagData = pTag->Data();
        for (std::size_t i = 0; i < tagEnts.size() && i < tagData.size(); ++i)
        {
            const Entity self  = tagEnts[i];
            const Entity owner = static_cast<Entity>(tagData[i].owner);
            if (!world.IsAlive(self)) continue;

            bool owned = false;
            if (owner != NullEntity && world.IsAlive(owner))
                if (const auto* ort = world.GetComponent<SequenceRuntimeComponent>(owner))
                    owned = ort->armed;
            if (!owned) orphans.push_back(self);
        }

        for (Entity root : orphans)
        {
            if (!world.IsAlive(root)) continue;
            std::vector<Entity> all, stack{ root };
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (!world.IsAlive(e)) continue;
                all.push_back(e);
                if (const Children* ch = world.GetComponent<Children>(e))
                    for (Entity k : ch->entities) stack.push_back(k);
            }
            for (auto it = all.rbegin(); it != all.rend(); ++it)
                if (world.IsAlive(*it)) world.DestroyEntity(*it);
        }
        if (!orphans.empty())
            LOG_INFO("Sequencer: collected %zu orphaned spawn(s) — owner gone or no longer armed",
                     orphans.size());
    }

    auto* pPlayer = world.GetPool<SequencePlayerComponent>();
    if (!pPlayer || pPlayer->Data().empty()) return;
    auto* pRt = world.EnsurePool<SequenceRuntimeComponent>();

    // Snapshot: evaluation can add SequenceRuntimeComponent and spawn entities, either of
    // which may reallocate the pool.
    std::vector<Entity> players = pPlayer->Entities();

    // Cross-sequence arbitration by layer, last writer wins. Pool order is entity creation
    // order, which LoadScene rebuilds in file order, so two sequences on one transform
    // would swap winners across a save/load. Sorting by the authored layer keeps the
    // outcome deterministic and explainable.
    std::stable_sort(players.begin(), players.end(),
                     [&](Entity a, Entity b)
                     {
                         auto* ra = pRt->Get(a);
                         auto* rb = pRt->Get(b);
                         const int32_t la = (ra && ra->asset) ? ra->asset->layer : 0;
                         const int32_t lb = (rb && rb->asset) ? rb->asset->layer : 0;
                         return la < lb;
                     });

    for (const Entity e : players)
    {
        // The snapshot can name an entity that has died since: releasing one player destroys
        // its sub-sequence children, which are players in this same list.
        if (!world.IsAlive(e)) continue;

        SequencePlayerComponent* p = pPlayer->Get(e);
        if (!p) continue;
        if (!pRt->Get(e)) world.AddComponent<SequenceRuntimeComponent>(e, SequenceRuntimeComponent{});
        SequenceRuntimeComponent* rt = pRt->Get(e);
        if (!rt) continue;

        // ---- asset ----
        // Two distinct events land here:
        //   path change     a different sequence: release the old cast, rewind, start over.
        //   content change  the same sequence edited underneath (an editor publish or a Save):
        //                   the playhead must not move.
        const bool     pathChanged = (rt->loadedPath != p->sequencePath);
        const uint32_t cacheRev    = Seq::SequenceCacheRevision();
        if (pathChanged || rt->assetRevision != cacheRev)
        {
            std::shared_ptr<const Seq::Sequence> next = Seq::AcquireSequence(p->sequencePath);
            rt->assetRevision = cacheRev;

            // Pointer identity is the real test: the revision counter is global, so most players
            // see a bump and find their own asset unchanged, which must cost nothing.
            if (pathChanged || next != rt->asset)
            {
                // Captured before the release below, which clears `armed`. A player that has never run
                // stays untouched: publishing an edit must not arm every SequencePlayerComponent in the
                // level that points at the same .iseq; only one already showing a pose re-evaluates.
                const bool live = p->playing || rt->armed;

                // A re-arm despawns and respawns the whole cast; doing it on every published keyframe
                // drag would churn the shot camera (VCam plus a temporal-history reset) every frame. So
                // re-arm only when the edit changed something ClaimActors baked in.
                const uint64_t nextSig = next ? Seq::ArmSignature(*next) : 0ull;
                const bool     rearm   = pathChanged || nextSig != rt->armSignature;

                if (rt->armed && rt->asset && rearm)
                {
                    // A same-path re-arm forces RestoreMode::Always: ClaimActors re-captures the
                    // pre-animated state right afterwards, and without the restore it would capture the
                    // sequenced pose as the new baseline and bake the shot into the scene.
                    Seq::ReleaseActors(world, *rt->asset, *rt,
                                       pathChanged ? rt->asset->restoreMode
                                                   : Seq::RestoreMode::Always,
                                       ctx.mode);

                    // ReleaseActors destroys this player's sub-sequence children (players too), so both
                    // pools may have reallocated and `p` / `rt` may dangle: re-fetch. A live editor publish
                    // reaches this on any structural edit.
                    p  = pPlayer->Get(e);
                    rt = pRt->Get(e);
                    if (!p || !rt) continue;
                }

                rt->asset        = next;
                rt->loadedPath   = p->sequencePath;
                rt->armSignature = nextSig;
                // Preload the new asset's spawnables (a path shared with the old asset keeps its
                // preload: it is requested before the old hold lets go).
                rt->spawnablePreloads = next ? PreloadSpawnables(*next) : nullptr;

                if (pathChanged)
                {
                    rt->ResetDerived();
                    p->playing  = false;
                    p->started  = false;
                    p->frame    = 0;
                    p->subFrame = 0.f;
                }
                else
                {
                    if (rt->asset)
                    {
                        // Not in the arm signature (it would respawn the camera
                        // on every frame of a DragFloat3), so it is applied here.
                        Seq::ApplySpawnTransforms(world, *rt->asset, *rt);
                    }

                    // Force one evaluation at the current frame. With the transport stopped the tick below
                    // early-outs on `!playing && !seeked`, so the edit would show only once the playhead
                    // moved; re-seeking to the current frame refreshes the pose without moving anything.
                    // A playing player evaluates every tick anyway, and a seek would zero its sub-frame
                    // accumulator and hitch playback.
                    if (live && !p->playing)
                    {
                        p->seekRequested   = true;
                        p->seekTargetFrame = p->frame;
                    }
                }

                // The next evaluation cannot be treated as continuous: a
                // section may have moved out from under a one-shot that already
                // fired, and every range-state edge has to be re-derived
                // against the new document.
                p->prevFrame = -1;
                rt->activeKeys.clear();
            }
        }
        if (!rt->asset) continue;
        // Re-request stale spawnable preloads, or the shot's first frame would spawn them cold.
        if (rt->spawnablePreloads && SpawnablePreloadsStale(rt->spawnablePreloads))
            rt->spawnablePreloads = PreloadSpawnables(*rt->asset);
        const Seq::Sequence& seq = *rt->asset;

        const float   fps      = seq.frameRate > 0.f ? seq.frameRate : 30.f;
        const int32_t lastFrame = std::max(0, seq.durationFrames);
        const bool    looping  = p->overrideLooping ? p->looping : seq.looping;

        // ---- transport edges ----
        if (p->stopRequested)
        {
            p->stopRequested = false;
            const bool wasLive = SequenceIsLive(*p, m_gameFlow);
            // Copied out first: ReleaseOne can move the component this pointer targets, and
            // PublishFinished must not read through it afterwards.
            const std::string seqPath    = p->sequencePath;
            const bool        recWatched = p->recordWatched;
            ReleaseGameFlow(*p, m_gameFlow);
            ReleaseOne(world, e, ctx.mode);
            if (wasLive) PublishFinished(e, seqPath, recWatched, /*skipped*/ false);
            p  = pPlayer->Get(e);
            rt = pRt->Get(e);
            if (!p || !rt) continue;
        }

        // Skip. JumpToEnd is a plain seek to the last frame: the absolute evaluator produces
        // the authored final state there, so every actor, camera and post-process lands where
        // the shot leaves them.
        bool skippedThisTick = false;
        if (p->skipRequested)
        {
            p->skipRequested = false;
            if (p->playing)
            {
                switch (p->skipPolicy)
                {
                case SequencePlayerComponent::SkipPolicy::JumpToEnd:
                    p->seekRequested   = true;
                    p->seekTargetFrame = std::max(0, seq.durationFrames);
                    p->rate            = std::abs(p->rate);   // never skip backwards
                    skippedThisTick    = true;
                    break;
                case SequencePlayerComponent::SkipPolicy::StopImmediately:
                {
                    // Same copy-then-re-fetch dance as the stop edge above.
                    const std::string seqPath    = p->sequencePath;
                    const bool        recWatched = p->recordWatched;
                    ReleaseGameFlow(*p, m_gameFlow);
                    ReleaseOne(world, e, ctx.mode);
                    PublishFinished(e, seqPath, recWatched, /*skipped*/ true);
                    p  = pPlayer->Get(e);
                    rt = pRt->Get(e);
                    if (!p || !rt) continue;   // died with its children
                    break;
                }
                case SequencePlayerComponent::SkipPolicy::None:
                default:
                    break;   // this cutscene is not skippable
                }
            }
        }
        if (p->playRequested)
        {
            p->playRequested = false;
            p->playing   = true;
            p->started   = true;
            p->prevFrame = -1;          // first sight => discontinuity => no event storm
            p->subFrame  = 0.f;
            TakeGameFlow(*p, m_gameFlow);
        }
        // playOnLoad deliberately waits for the sim to advance, so opening a
        // scene in the editor does not immediately start every cutscene in it.
        if (p->playOnLoad && !p->started && !p->playing && ctx.runUpdate)
        {
            p->playing   = true;
            p->started   = true;
            p->frame     = 0;
            p->subFrame  = 0.f;
            p->prevFrame = -1;
            TakeGameFlow(*p, m_gameFlow);
        }

        bool seeked = false;
        if (p->seekRequested)
        {
            p->seekRequested = false;
            p->frame    = std::clamp(p->seekTargetFrame, 0, lastFrame);
            p->subFrame = 0.f;
            seeked      = true;
        }

        // An idle player evaluates nothing. A seek still does, even while stopped: that is
        // scrubbing, and the pose must persist until the user stops or plays.
        if (!p->playing && !seeked) continue;

        // Captured before the clock so the natural end is detected as an edge. Releasing
        // whenever `playing` is false would restore the pre-animated state on the same tick a
        // stopped-player scrub wrote its pose, undoing the scrub.
        const bool wasPlaying = p->playing;

        // ---- bindings + claim ----
        Seq::ResolveBindings(world, seq, *p, *rt);
        Seq::ClaimActors(world, seq, *rt, e, m_assetMgr, m_renderer, m_clips);

        // ClaimActors can spawn a prefab Spawnable whose subtree carries player components: a
        // pool push_back that reallocates the slab both pointers point into.
        p  = pPlayer->Get(e);
        rt = pRt->Get(e);
        if (!p || !rt) continue;

        // ---- clock ----
        // Integer frames at the asset's own rate. The sub-frame is an accumulator: it feeds
        // curve evaluation, never an event test. Seq::AdvanceClock holds the arithmetic so it
        // is unit-testable outside the editor.
        const int32_t dir = (p->rate < 0.f) ? -1 : 1;

        const bool frozen = p->paused || ctx.mode == EngineMode::Paused;
        const bool stepOne = !frozen && ctx.mode == EngineMode::Stepping;

        float dtSec = 0.f;
        if (!frozen && !stepOne)
        {
            switch (seq.clockSource)
            {
            case Seq::ClockSource::Scaled: dtSec = ctx.scaledDeltaTime; break;
            case Seq::ClockSource::Fixed:  dtSec = 1.f / fps;           break;
            case Seq::ClockSource::Unscaled:
            default:                       dtSec = ctx.deltaTime;       break;
            }
        }

        const Seq::ClockStep step = Seq::AdvanceClock(
            p->frame, p->subFrame, dtSec, fps, p->rate, lastFrame, looping, stepOne);
        p->frame    = step.frame;
        p->subFrame = step.subFrame;
        if (step.ended) p->playing = false;

        const bool discontinuous = Seq::IsDiscontinuous(
            p->frame, p->prevFrame, dir, seq.maxForwardJump, seeked, step.wrapped);

        if (discontinuous)
        {
            // One temporal-history reset per frame across all players: during a scrub drag every
            // frame is a discontinuity, and the underlying call is global.
            const bool quiet = (m_lastHistoryResetFrame == ctx.frameIndex);
            Seq::OnDiscontinuity(world, seq, *rt, quiet ? nullptr : m_renderer, true);
            m_lastHistoryResetFrame = ctx.frameIndex;
        }

        // ---- evaluate ----
        Seq::EvalContext ec;
        ec.world     = &world;
        ec.player    = e;
        ec.renderer  = m_renderer;
        ec.clips     = m_clips;
        ec.assetMgr  = m_assetMgr;
        ec.frame     = p->frame;
        ec.frameF    = static_cast<float>(p->frame) + p->subFrame;
        ec.prevFrame = p->prevFrame;
        ec.dir       = dir;
        ec.continuous= !discontinuous;
        ec.frameRate = fps;
        ec.mode      = ctx.mode;

        Seq::Evaluate(seq, *rt, ec);

        // Evaluation creates entities (Spawnables, sub-sequence children) and adds components
        // to them, either of which can reallocate these pointers' pools: re-acquire, the ECS
        // equivalent of an iterator invalidated by push_back.
        p  = pPlayer->Get(e);
        rt = pRt->Get(e);
        if (!p || !rt) continue;

        p->prevFrame = p->frame;

        // Natural end, as an edge. The final frame is evaluated first and then the pre-animated
        // state restored, so the last authored pose is what this frame renders.
        if (wasPlaying && !p->playing)
        {
            // Copied out first, as at the stop edge. Nothing reads p / rt after this block, so no
            // re-fetch is needed.
            const std::string seqPath    = p->sequencePath;
            const bool        recWatched = p->recordWatched;
            ReleaseGameFlow(*p, m_gameFlow);
            ReleaseOne(world, e, ctx.mode);
            PublishFinished(e, seqPath, recWatched, skippedThisTick);
        }
    }
}
