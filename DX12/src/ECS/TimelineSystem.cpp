#include "ECS/TimelineSystem.h"
#include "ECS/AnimGraphSystem.h"
#include "Anim/Core/NotifyDispatch.h"
#include "Graphics/AnimProfile.h"

#include "ECS/AnimationComponents.h"
#include "ECS/AnimFXGateComponent.h"  // ActiveFXGates - the level (non-mailbox) output
#include "ECS/ExclusiveResourceIds.h"
#include "ECS/FrameContext.h"
#include "ECS/NotifyTypes.h"
#include "ECS/StateToggle.h"
#include "Gameplay/GameplayTagsComponent.h"   // ActiveStateToggles
#include "Resource/SkeletonAsset.h"   // ClipLibrary, ClipAsset
#include "System/Check.h"
#include "System/CommandLine.h"   // -timeline_prepare_legacy (harness)

#include <algorithm>
#include <cmath>

namespace
{
    // The dispatch helpers AddComponent a mailbox the first time a category fires. With the pools
    // created beforehand that Add only grows a pool this system exclusively Write-declares, and never
    // reaches World::GetOrCreatePool's m_pools.emplace (a world-wide map rehash that would race every
    // co-batched GetPool / GetComponent).
    void EnsureMailboxPools(World& world)
    {
        world.EnsurePool<PendingHitboxCommands>();
        world.EnsurePool<PendingVFXSpawns>();
        world.EnsurePool<PendingCameraEffects>();
        world.EnsurePool<PendingAudioPlays>();
        world.EnsurePool<PendingStateToggles>();
        world.EnsurePool<PendingGenericNotifies>();
        world.EnsurePool<ActiveFXGates>();
        world.EnsurePool<ActiveStateToggles>();
    }
}

void TimelineSystem::OnRegister(World& world)
{
    EnsureMailboxPools(world);
}

void TimelineSystem::PrepareStructure(World& world)
{
#ifdef WITH_HARNESS
    // -timeline_prepare_legacy: only ActiveStateToggles is re-created each phase, so after a World::Clear
    // Update creates a mailbox pool itself (the reverse arm of accessguard_timeline).
    if (Sys::CommandLine::Get().Has("-timeline_prepare_legacy"))
    {
        world.EnsurePool<ActiveStateToggles>();
        return;
    }
#endif
    // World::Clear drops pools, so every phase re-creates them.
    EnsureMailboxPools(world);
}

void TimelineSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Writes the per-entity notify cursors and every mailbox type. Reads clip notify tracks from the
    // ClipLibrary, shared with the animation chain and the editor's timeline authoring: keep any other
    // ClipLibrary toucher in this phase mutually exclusive (kAnimClipSystem).
    b.Write<AnimationComponent>()       // prevNotifyTime / lastNotifyClip cursors
     .Write<TimelineComponent>()        // lastObservedTime cursor (Path B)
     .Read<AnimatorTransitionComponent>() // clip alias for the clip_end payload
     .Write<PendingHitboxCommands>()
     .Write<PendingVFXSpawns>()
     .Write<PendingCameraEffects>()
     .Write<PendingAudioPlays>()
     .Write<PendingStateToggles>()
     .Write<PendingGenericNotifies>()
     .Write<ActiveFXGates>()            // level output, rewritten every frame
     .Write<ActiveStateToggles>()       // the open StateToggle intervals, rewritten every frame
     .ExclusiveResource(ExclusiveRes::kAnimClipSystem);
}

namespace Anim::NotifyDispatch
{
    // True iff @p t lies in the half-open interval (prev, curr] on a looping clip of length
    // @p duration; handles the wrap when curr < prev. Open at prev so a notify exactly at the wrap
    // point fires once per loop, not twice; closed at curr so notifies authored at clip boundaries
    // fire.
    bool CrossedTime(float prev, float curr, float duration, float t)
    {
        if (duration <= 0.f) return false;

        auto wrap = [duration](float x) {
            float r = std::fmod(x, duration);
            return (r < 0.f) ? r + duration : r;
        };

        prev = wrap(prev);
        curr = wrap(curr);

        if (prev <= curr) return t > prev && t <= curr;
        // wrapped: hit if in (prev, duration) U [0, curr]
        return (t > prev) || (t <= curr);
    }

    // ---- Per-category dispatch into the Pending* mailbox components ----
    // Shared by every notify path. A world.AddComponent here is safe: the mailbox component type is
    // distinct from the pool being iterated.

    // The one place that turns an authored PropertyBag into HitboxParams, so the point-notify path and
    // the two NotifyState edges cannot drift apart.
    // Every Get* silently returns the default when the key is absent or holds the wrong variant
    // alternative. "socket" must be authored as a string: the editor's add-param type combo defaults
    // to float, and a socket typed there without switching the combo reads as "", so the hitbox falls
    // back to the entity origin.
    void ReadHitboxParams(const PropertyBag& bag, HitboxParams& out)
    {
        out.radius      = bag.GetFloat("radius",     0.5f);
        out.halfHeight  = bag.GetFloat("halfHeight", 0.f);
        out.damage      = bag.GetFloat("damage",    10.f);
        out.hitGroup    = static_cast<uint32_t>(bag.GetInt("hitGroup", 0));
        out.localOffset = bag.GetVec3("offset", { 0, 0, 0 });
        out.layerMask   = static_cast<uint16_t>(bag.GetInt("layerMask", 0));
        // shape: explicit key wins, else infer from halfHeight so authoring a
        // half-height is enough to mean "capsule".
        const int shapeKey = bag.GetInt("shape", -1);
        out.shape = (shapeKey >= 0)
                  ? static_cast<HitboxShapeKind>(shapeKey)
                  : (out.halfHeight > 0.f ? HitboxShapeKind::Capsule
                                          : HitboxShapeKind::Sphere);
        const std::string socket = bag.GetString("socket", "");
        out.socketHash = socket.empty() ? 0u : SocketComponent::HashName(socket.c_str());
        out.requireTag = bag.GetString("requireTag", "");
        out.ignoreTag  = bag.GetString("ignoreTag",  "");
        // Gameplay tags, comma separated dotted names; an illegal one is an authoring error.
        out.requireTags.clear();
        out.ignoreTags.clear();
        std::string tagError;
        if (!Gameplay::ParseTagList(bag.GetString("requireTags", ""), out.requireTags, &tagError) ||
            !Gameplay::ParseTagList(bag.GetString("ignoreTags", ""), out.ignoreTags, &tagError))
            ENGINE_CHECK_ONCE(false, "[Hitbox] a hitbox notify's tag list is illegal: %s", tagError.c_str());
    }

    void DispatchPoint(World& world, Entity e, const Notify& n)
    {
        switch (n.category)
        {
        case NotifyCategory::Hitbox: {
            auto* mb = world.GetComponent<PendingHitboxCommands>(e);
            if (!mb) { world.AddComponent<PendingHitboxCommands>(e, {}); mb = world.GetComponent<PendingHitboxCommands>(e); }
            PendingHitboxCommands::Cmd cmd;
            cmd.type     = static_cast<HitboxCmdType>(n.params.GetInt("cmdType", 0));
            cmd.slot     = static_cast<uint32_t>(n.params.GetInt("slot", 0));
            ReadHitboxParams(n.params, cmd.params);
            mb->commands.push_back(cmd);
        } break;

        case NotifyCategory::VFX: {
            auto* mb = world.GetComponent<PendingVFXSpawns>(e);
            if (!mb) { world.AddComponent<PendingVFXSpawns>(e, {}); mb = world.GetComponent<PendingVFXSpawns>(e); }
            PendingVFXSpawns::Spawn s;
            s.assetPath = n.params.GetString("asset");
            s.mode      = static_cast<VFXAttachMode>(n.params.GetInt("attach", 0));
            s.boneIndex = n.params.GetInt("bone", -1);
            s.localPos  = n.params.GetVec3("offset", {0,0,0});
            // Default 0 = let the .ivfx own its lifetime (the consumer treats duration <= 0 as inherit:
            // DUR / LIFE per emitter, else the prefab's DURATION line). A non-zero notify duration overrides
            // the asset.
            s.duration  = n.params.GetFloat("duration", 0.f);
            mb->spawns.push_back(std::move(s));
        } break;

        case NotifyCategory::Camera: {
            auto* mb = world.GetComponent<PendingCameraEffects>(e);
            if (!mb) { world.AddComponent<PendingCameraEffects>(e, {}); mb = world.GetComponent<PendingCameraEffects>(e); }
            PendingCameraEffects::Effect ef;
            ef.type = static_cast<CameraEffectType>(n.params.GetInt("kind", 0));
            ef.params.shakeAmplitude = n.params.GetFloat("amp",      0.2f);
            ef.params.shakeFrequency = n.params.GetFloat("freq",    20.f);
            ef.params.fovDelta       = n.params.GetFloat("fovDelta", 0.f);
            ef.params.duration       = n.params.GetFloat("duration",0.05f);
            ef.params.timeScale      = n.params.GetFloat("scale",   0.05f);
            mb->effects.push_back(ef);
        } break;

        case NotifyCategory::Audio: {
            auto* mb = world.GetComponent<PendingAudioPlays>(e);
            if (!mb) { world.AddComponent<PendingAudioPlays>(e, {}); mb = world.GetComponent<PendingAudioPlays>(e); }
            PendingAudioPlays::Play p;
            p.clipPath   = n.params.GetString("clip");
            p.category   = static_cast<AudioCategoryTag>(n.params.GetInt("cat", 0));
            p.volume     = n.params.GetFloat("volume", 1.f);
            p.pitch      = n.params.GetFloat("pitch",  1.f);
            p.attachBone = n.params.GetInt("bone", -1);
            mb->plays.push_back(std::move(p));
        } break;

        case NotifyCategory::StateToggle: {
            PendingStateToggles::Toggle t;
            std::string error;
            // A toggle that names no tag was reported where it was loaded (StateToggle::Problems).
            if (!StateToggle::ReadTag(n.params, t.tag, error)) break;
            auto* mb = world.GetComponent<PendingStateToggles>(e);
            if (!mb) { world.AddComponent<PendingStateToggles>(e, {}); mb = world.GetComponent<PendingStateToggles>(e); }
            t.enable   = n.params.GetInt("enable", 1) != 0;
            t.duration = n.params.GetFloat("duration", 0.f);
            mb->toggles.push_back(t);
        } break;

        case NotifyCategory::Custom: {
            auto* mb = world.GetComponent<PendingGenericNotifies>(e);
            if (!mb) { world.AddComponent<PendingGenericNotifies>(e, {}); mb = world.GetComponent<PendingGenericNotifies>(e); }
            PendingGenericNotifies::Notify gn;
            gn.typeId = StringID(n.params.GetString("type", n.displayName));
            gn.params = n.params;
            mb->notifies.push_back(std::move(gn));
        } break;

        default: break;
        }
    }

    // NotifyState Begin - interval start crossed going forward. Uses the same mailbox cmd layout as a
    // point notify, so consumers need no new branch. Hitbox has its own Begin; StateToggle has none
    // (its interval is a level, CollectLevels); every other category fires its point effect.
    void DispatchStateBegin(World& world, Entity e, const NotifyState& s)
    {
        switch (s.category)
        {
        case NotifyCategory::Hitbox: {
            auto* mb = world.GetComponent<PendingHitboxCommands>(e);
            if (!mb) { world.AddComponent<PendingHitboxCommands>(e, {}); mb = world.GetComponent<PendingHitboxCommands>(e); }
            PendingHitboxCommands::Cmd cmd;
            cmd.type     = HitboxCmdType::Begin;
            cmd.slot     = static_cast<uint32_t>(s.params.GetInt("slot", 0));
            ReadHitboxParams(s.params, cmd.params);
            mb->commands.push_back(cmd);
        } break;
        case NotifyCategory::StateToggle:
            break;
        // Camera / VFX / Audio / Custom: a NotifyState's start edge fires the same one-shot effect a point
        // Notify would, so an interval authored on these categories triggers (e.g. a Camera-Shake
        // NotifyState kicks the shake on Begin).
        default: {
            Notify n;
            n.id          = s.id;
            n.category    = s.category;
            n.time        = s.startTime;
            n.params      = s.params;
            n.displayName = s.displayName;
            n.color       = s.color;
            DispatchPoint(world, e, n);
        } break;
        }
    }

    // NotifyState End - interval end crossed. Hitbox -> End cmd, Custom -> "<type>:end" event.
    // Camera / VFX / Audio are one-shots fired on Begin; StateToggle is a level; their End does nothing.
    void DispatchStateEnd(World& world, Entity e, const NotifyState& s)
    {
        switch (s.category)
        {
        case NotifyCategory::Hitbox: {
            auto* mb = world.GetComponent<PendingHitboxCommands>(e);
            if (!mb) { world.AddComponent<PendingHitboxCommands>(e, {}); mb = world.GetComponent<PendingHitboxCommands>(e); }
            PendingHitboxCommands::Cmd cmd;
            cmd.type = HitboxCmdType::End;
            cmd.slot = static_cast<uint32_t>(s.params.GetInt("slot", 0));
            cmd.params.hitGroup = static_cast<uint32_t>(s.params.GetInt("hitGroup", 0));
            mb->commands.push_back(cmd);
        } break;
        case NotifyCategory::Custom: {
            auto* mb = world.GetComponent<PendingGenericNotifies>(e);
            if (!mb) { world.AddComponent<PendingGenericNotifies>(e, {}); mb = world.GetComponent<PendingGenericNotifies>(e); }
            PendingGenericNotifies::Notify gn;
            gn.typeId = StringID(s.params.GetString("type", s.displayName) + ":end");
            gn.params = s.params;
            mb->notifies.push_back(std::move(gn));
        } break;
        default: break;
        }
    }

    // Evaluate one track list over the [prev, curr] window (clip length
    // `duration`) and dispatch every crossed point notify / state edge.
    void FireTracks(World& world, Entity e,
                    const std::vector<NotifyTrack>& tracks,
                    float prev, float curr, float duration)
    {
        for (const auto& track : tracks)
        {
            if (track.muted) continue;

            for (const auto& s : track.states)
            {
                if (CrossedTime(prev, curr, duration, s.startTime)) DispatchStateBegin(world, e, s);
                if (CrossedTime(prev, curr, duration, s.endTime))   DispatchStateEnd(world, e, s);
            }
            for (const auto& n : track.notifies)
            {
                if (CrossedTime(prev, curr, duration, n.time)) DispatchPoint(world, e, n);
            }
        }
    }

    // ---- FXGate and StateToggle: level evaluation, not edge dispatch ----
    // The functions above answer "what happened between prev and curr"; this answers "what is true
    // right now", the only form that survives a clip switch mid-interval: FireTracks is gated on
    // lastNotifyClip == primaryClip, so the End edge of a cancelled combo is never evaluated and an
    // edge-driven trail would latch on forever. Recomputing from the current clip's intervals every
    // frame makes "off" free to reach, including for a dead character whose clip has no gate.
    // Not skipped while paused on purpose: a paused preview mid-swing keeps showing the armed effect,
    // and scrubbing tracks the interval.
    // A StateToggle interval is the same kind of level: the tag it names is open while the clip's time
    // lies inside it (one that names no tag was reported where it was loaded and opens nothing).
    void CollectLevels(const std::vector<NotifyTrack>& tracks, float t,
                       ActiveFXGates& gates, ActiveStateToggles& toggles)
    {
        for (const auto& track : tracks)
        {
            if (track.muted) continue;
            for (const auto& s : track.states)
            {
                if (t < s.startTime || t > s.endTime) continue;

                if (s.category == NotifyCategory::StateToggle)
                {
                    Gameplay::Tag tag;
                    std::string error;
                    if (StateToggle::ReadTag(s.params, tag, error)) toggles.open.push_back(tag);
                    continue;
                }
                if (s.category != NotifyCategory::FXGate) continue;

                const std::string g = s.params.GetString("group");
                if (g.empty()) gates.matchesAny = true;
                else           gates.groups.push_back(AnimFX::HashGroup(g));
            }
        }
    }
}

void TimelineSystem::Update(World& world, const FrameContext&)
{
    using namespace Anim::NotifyDispatch;
    Anim::StageTimer _tTl;
    // ---- Path A: clip-authored notify tracks (Unreal AnimSequence style) ----
    // Drive notifies off the active clip via the per-entity cursor on the AnimationComponent. One
    // authored set covers every entity that plays the clip.
    const uint32_t clipCount = m_clips.Count();
    world.ForEach<AnimationComponent>([&](Entity e, AnimationComponent& anim)
    {
        if(m_graph && m_graph->OwnsPose(e)) { anim.prevNotifyTime=-1; anim.lastNotifyClip=kInvalidAnimHandle; return; }
        if (anim.primaryClip == kInvalidAnimHandle || anim.primaryClip >= clipCount)
            return;

        const ClipAsset& clip = m_clips.Get(anim.primaryClip);
        if (clip.duration <= 0.f)
        {
            // Keep the cursor tracking so a later switch to a notify-bearing
            // clip doesn't replay a stale window.
            anim.prevNotifyTime = anim.primaryTime;
            anim.lastNotifyClip = anim.primaryClip;
            return;
        }

        const float curr = anim.primaryTime;

        // First sight or clip switch -> snapshot, do not fire.
        if (anim.prevNotifyTime < 0.f || anim.lastNotifyClip != anim.primaryClip)
        {
            anim.prevNotifyTime = curr;
            anim.lastNotifyClip = anim.primaryClip;
            return;
        }

        const float prev = anim.prevNotifyTime;
        anim.prevNotifyTime = curr;
        anim.lastNotifyClip = anim.primaryClip;

        if (anim.paused || prev == curr) return;

        if (!clip.notifyTracks.empty())
            FireTracks(world, e, clip.notifyTracks, prev, curr, clip.duration);

        // ---- clip_end: a non-looping clip crossed its final frame this tick ----
        // AdvancePlaybackTime clamps primaryTime to exactly clip.duration, so this edge fires once
        // (afterwards prev == curr returns early above). Skipped for MotionDirector-driven clips
        // (externalTime scrubbing) and reverse playback (which ends at 0). Routed through the Custom-notify
        // mailbox like designer events: GenericNotifyDispatcher -> CustomNotifyEvent -> Lua
        // OnAnimEvent(self, "clip_end").
        if (!anim.looping && !anim.externalTime &&
            prev < clip.duration && curr >= clip.duration)
        {
            auto* mb = world.GetComponent<PendingGenericNotifies>(e);
            if (!mb)
            {
                world.AddComponent<PendingGenericNotifies>(e, {});
                mb = world.GetComponent<PendingGenericNotifies>(e);
            }
            PendingGenericNotifies::Notify gn;
            gn.typeId = StringID("clip_end");
            if (const auto* transition =
                    world.GetComponent<AnimatorTransitionComponent>(e))
                gn.params.SetString("clip", transition->currentClipName);
            gn.params.SetFloat("duration", clip.duration);
            mb->notifies.push_back(std::move(gn));
        }
    });

    // ---- Path B: per-entity TimelineComponent override tracks ----
    // Uses the component's own lastObservedTime cursor. Additive to Path A.
    world.ForEach<TimelineComponent>([&](Entity e, TimelineComponent& tl)
    {
        const auto* anim = world.GetComponent<AnimationComponent>(e);
        if (!anim || anim->paused) return;
        if (tl.clipDuration <= 0.f) return;
        if (tl.tracks.empty()) return;

        const float curr = anim->primaryTime;

        if (tl.lastObservedTime < 0.f) { tl.lastObservedTime = curr; return; }

        const float prev = tl.lastObservedTime;
        tl.lastObservedTime = curr;
        if (prev == curr) return;

        FireTracks(world, e, tl.tracks, prev, curr, tl.clipDuration);
    });

    // ---- Path C: FXGate level pass ----
    // Clear first, then refill: an entity that stopped playing a gated clip must end the frame with an
    // empty list, not a stale one. That clear is what turns the effects off on cancel / death / clip
    // switch with no event of any kind.
    world.ForEach<ActiveFXGates>([](Entity, ActiveFXGates& g)
    {
        g.groups.clear();
        g.matchesAny = false;
    });
    world.ForEach<ActiveStateToggles>([](Entity, ActiveStateToggles& s) { s.open.clear(); });

    world.ForEach<AnimationComponent>([&](Entity e, AnimationComponent& anim)
    {
        ActiveFXGates open;
        ActiveStateToggles openToggles;

        if(m_graph && m_graph->OwnsPose(e)) {
            if(const auto* instance=m_graph->GetInstance(e))for(const auto& tick:instance->ticks) {
                const auto& node=instance->definition->nodes[tick.node];
                if(tick.weight<node.eventThreshold || tick.clip>=clipCount)continue;
                const auto& clip=m_clips.Get(tick.clip);
                float time=clip.duration>0?std::fmod(tick.time,clip.duration):0; if(time<0)time+=clip.duration;
                CollectLevels(clip.notifyTracks,time,open,openToggles);
            }
        }

        if ((!m_graph || !m_graph->OwnsPose(e)) && anim.primaryClip != kInvalidAnimHandle && anim.primaryClip < clipCount)
        {
            const ClipAsset& clip = m_clips.Get(anim.primaryClip);
            if (clip.duration > 0.f && !clip.notifyTracks.empty())
                CollectLevels(clip.notifyTracks, anim.primaryTime, open, openToggles);
        }
        if (const auto* tl = world.GetComponent<TimelineComponent>(e))
        {
            if (!tl->tracks.empty())
                CollectLevels(tl->tracks, anim.primaryTime, open, openToggles);
        }

        auto* dst = world.GetComponent<ActiveFXGates>(e);
        if (!dst && (!open.groups.empty() || open.matchesAny))
        {
            // Only materialise the component once this entity has actually
            // opened a gate: characters that never author one stay out of the
            // pool entirely, and AnimFXGateSystem reads "no component" as
            // "nothing open" anyway.
            world.AddComponent<ActiveFXGates>(e, {});
            dst = world.GetComponent<ActiveFXGates>(e);
        }
        if (dst)
        {
            dst->groups     = std::move(open.groups);
            dst->matchesAny = open.matchesAny;
        }

        // The same for the StateToggle intervals; StateToggleSystem turns them into the owner's tags.
        auto* toggles = world.GetComponent<ActiveStateToggles>(e);
        if (!toggles && !openToggles.open.empty())
        {
            world.AddComponent<ActiveStateToggles>(e, {});
            toggles = world.GetComponent<ActiveStateToggles>(e);
        }
        if (toggles) toggles->open = std::move(openToggles.open);
    });
    // Path D: anim-graph notifies. Drains worker-owned buffers only after the pose jobs have joined.
    if(m_graph) {
        auto dispatch=[&](Entity e,const Anim::Graph::SampledNotify& event) {
            if(event.edge==Anim::Graph::NotifyEdge::Point) {
                auto point=event.point;point.params.SetInt("_animGraph",1);point.params.SetFloat("weight",event.weight);point.params.SetString("sourcePath",event.sourcePath);point.params.SetInt("sourceNode",event.source);point.params.SetInt("activeBranch",event.activeBranch?1:0);
                DispatchPoint(world,e,point);
            }else {
                auto state=event.state;state.params.SetInt("_animGraph",1);state.params.SetFloat("weight",event.weight);state.params.SetString("sourcePath",event.sourcePath);state.params.SetInt("sourceNode",event.source);state.params.SetInt("activeBranch",event.activeBranch?1:0);
                state.params.SetInt("forcedEnd",event.forcedEnd?1:0);
                if(event.edge==Anim::Graph::NotifyEdge::Begin)DispatchStateBegin(world,e,state);
                else DispatchStateEnd(world,e,state);
            }
        };
        m_graph->DrainRetired(world,dispatch);
        m_graph->ForEachOutput(world,[&](Entity e,Anim::Graph::Instance& instance) {
            for(const auto& event:instance.notifyEvents)dispatch(e,event);
            instance.notifyEvents.clear();
            if(m_graph->OwnsPose(e))for(const auto& event:instance.events) {
                Notify notify;notify.displayName=event.name;notify.params.SetString("graphEvent",event.name);
                notify.params.SetInt("_animGraph",1);
                notify.params.SetFloat("weight",event.weight);notify.params.SetInt("sourceNode",event.source);
                DispatchPoint(world,e,notify);
            }
            instance.events.clear();
        });
    }
    Anim::AnimProfile::Get().timelineMs = _tTl.Ms();
}
