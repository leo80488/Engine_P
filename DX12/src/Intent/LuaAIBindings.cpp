#include "Intent/LuaAIBindings.h"
#include "AI/AILODSystem.h"
#include "AI/AIProfile.h"
#include "Graphics/AnimProfile.h"
#include "AI/PerceptionSystem.h"
#include "AI/AIComponents.h"
#include "AI/AIBehavior.h"
#include "ECS/PerceptionComponent.h"
#include "ECS/HierarchyComponents.h"

#include "ECS/ECS.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/AnimationSystem.h"        // AnimStats.SetVirtualBones
#include "System/Log.h"

#include <sol/sol.hpp>
#include "AI/LuaBBConvert.h"

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <cstdint>
#include <string>

namespace Intent
{

namespace {

// Auto-promote helper, the same pattern as the tactical Intent bindings.
inline AIIntentComponent& GetOrCreateAIIntent(::World& world, Entity e)
{
    if (auto* p = world.GetComponent<AIIntentComponent>(e)) return *p;
    world.EnsurePool<AIIntentComponent>();
    AIIntentComponent fresh{};
    world.AddComponent<AIIntentComponent>(e, fresh);
    return *world.GetComponent<AIIntentComponent>(e);
}

// Goal name -> enum. An unknown name gives Idle, so a typo in a BT script does not
// silently keep the previous goal.
AIGoal GoalFromName(const std::string& s)
{
    if (s == "Idle")        return AIGoal::Idle;
    if (s == "Patrol")      return AIGoal::Patrol;
    if (s == "Investigate") return AIGoal::Investigate;
    if (s == "Attack")      return AIGoal::Attack;
    if (s == "Flee")        return AIGoal::Flee;
    if (s == "TakeCover")   return AIGoal::TakeCover;
    if (s == "Follow")      return AIGoal::Follow;
    if (s == "UseObject")   return AIGoal::UseObject;
    return AIGoal::Idle;
}

const char* NameFromGoal(AIGoal g)
{
    switch (g)
    {
        case AIGoal::Idle:        return "Idle";
        case AIGoal::Patrol:      return "Patrol";
        case AIGoal::Investigate: return "Investigate";
        case AIGoal::Attack:      return "Attack";
        case AIGoal::Flee:        return "Flee";
        case AIGoal::TakeCover:   return "TakeCover";
        case AIGoal::Follow:      return "Follow";
        case AIGoal::UseObject:   return "UseObject";
    }
    return "Idle";
}

// sol::object -> AIGoal: accepts the number (AI.Goal.Attack) or the name ("Attack");
// Idle on a type mismatch.
AIGoal ParseGoal(const sol::object& o)
{
    if (o.is<int>())                  return static_cast<AIGoal>(o.as<int>());
    if (o.is<std::uint32_t>())        return static_cast<AIGoal>(o.as<std::uint32_t>());
    if (o.is<std::string>())          return GoalFromName(o.as<std::string>());
    return AIGoal::Idle;
}

} // namespace

void RegisterLuaAIBindings(sol::state& lua, ::World& world,
                           AI::AILODSystem& lod,
                           AI::PerceptionSystem& perception)
{
    sol::table tbl = lua.create_table();

    // AI.Goal: enum mirror table, so BT leaves write AI.SetGoal(me, AI.Goal.Attack, target)
    // instead of magic numbers / strings.
    {
        sol::table goals = lua.create_table();
        goals["Idle"]        = static_cast<int>(AIGoal::Idle);
        goals["Patrol"]      = static_cast<int>(AIGoal::Patrol);
        goals["Investigate"] = static_cast<int>(AIGoal::Investigate);
        goals["Attack"]      = static_cast<int>(AIGoal::Attack);
        goals["Flee"]        = static_cast<int>(AIGoal::Flee);
        goals["TakeCover"]   = static_cast<int>(AIGoal::TakeCover);
        goals["Follow"]      = static_cast<int>(AIGoal::Follow);
        goals["UseObject"]   = static_cast<int>(AIGoal::UseObject);
        tbl["Goal"] = goals;
    }

    // AI.SetGoal(eid, goal[, targetEntity[, gx, gy, gz]]). Auto-creates AIIntentComponent;
    // AITacticalSystem (when registered) reads it next tick and writes NavAgent.destination /
    // facingMode. BT leaves set the goal, never the nav fields.
    tbl.set_function("SetGoal",
        [&world](sol::variadic_args args) -> bool
        {
            if (args.size() < 2) return false;
            const Entity e = LuaId::ResolveLive(world, args.get<LuaEntity>(0));
            if (e == NullEntity) return false;
            AIIntentComponent& intent = GetOrCreateAIIntent(world, e);
            intent.currentGoal = ParseGoal(args.get<sol::object>(1));

            if (args.size() >= 3)
                intent.targetEntity = LuaId::ResolveLive(world, args.get<LuaEntity>(2));
            else
                intent.targetEntity = NullEntity;

            if (args.size() >= 6)
            {
                intent.goalPosition = {
                    args.get<float>(3),
                    args.get<float>(4),
                    args.get<float>(5)
                };
            }
            // goalStartTime stays 0 (there is no clock handle here); a BT script can track elapsed
            // time itself via ctx:Elapsed(), and debug overlays tolerate 0.
            return true;
        });

    // AI.ClearGoal(eid): back to Idle; AITacticalSystem clears the agent's destination next
    // tick so it halts.
    tbl.set_function("ClearGoal",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            auto* p = e != NullEntity ? world.GetComponent<AIIntentComponent>(e) : nullptr;
            if (!p) return false;
            p->currentGoal  = AIGoal::Idle;
            p->targetEntity = NullEntity;
            return true;
        });

    // AI.GetGoal(eid) -> string. Returns "Idle" when no AIIntent attached
    // (consistent with default state).
    tbl.set_function("GetGoal",
        [&world](LuaEntity entityId) -> std::string
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            const auto* p = world.GetComponent<AIIntentComponent>(e);
            return NameFromGoal(p ? p->currentGoal : AIGoal::Idle);
        });

    // AI.GetTarget(eid) -> entityId. Returns 0 (NullEntity) when unset.
    tbl.set_function("GetTarget",
        [&world](LuaEntity entityId) -> LuaEntity
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            const auto* p = world.GetComponent<AIIntentComponent>(e);
            return LuaId::OutMaybe(&world, p ? p->targetEntity : NullEntity);
        });

    // ---- Not installed: the strategic goal layer has no consumer ----
    // AITacticalTickSystem is not registered (App.cpp), so nothing reads
    // AIIntentComponent: these functions would write it, return true and move nothing. `AI`
    // stays nil in Lua, so a leftover AI.SetGoal fails loudly at the call site; shipped
    // scripts drive the tactical layer through Intent.MoveTo (asset/ai/actions.lua).
    // To re-enable: register AITacticalTickSystem in App.cpp and publish the table
    // (`lua["AI"] = tbl;`). The bindings keep compiling so they cannot rot.
    (void)tbl;
    LOG_INFO("AI: Lua goal-layer bindings built but NOT installed "
             "(AITacticalTickSystem retired 2026-07-18 — use Intent.* instead)");

    // ---- AIBehavior.* (installed) ----
    // Authoring configuration is the serialized AIBehaviorComponent; only working memory is
    // exposed here, and targets come back as generation-checked entities.
    {
        sol::table behavior=lua.create_table();
        behavior.set_function("HasConfig",[&world](LuaEntity id){return world.HasComponent<AIBehaviorComponent>(LuaId::ResolveLive(world,id));});
        behavior.set_function("SetFixedTarget",[&world](LuaEntity id,LuaEntity other){
            Entity e=LuaId::ResolveLive(world,id),t=LuaId::ResolveLive(world,other);
            auto* c=world.GetComponent<AIBehaviorComponent>(e);if(!c||t==NullEntity||t==e)return false;
            c->fixedTarget.BindAndStamp(world,t);c->targetMode=2;AI::ResetBehavior(world,e);return true;
        });
        behavior.set_function("CanTarget",[&world](LuaEntity id,LuaEntity other){
            Entity e=LuaId::ResolveLive(world,id),t=LuaId::ResolveLive(world,other);
            if(e==NullEntity||t==NullEntity||e==t)return false;
            const auto* a=world.GetComponent<PerceptibleComponent>(e);const auto* b=world.GetComponent<PerceptibleComponent>(t);
            if(!a||!b||!b->enabled)return false;
            if(auto* c=world.GetComponent<AIBehaviorComponent>(e);c&&c->targetMode==2)return c->fixedTarget.Resolve(world)==t;
            return a->faction!=b->faction;
        });
        behavior.set_function("SetRoute",[&world](LuaEntity id,sol::table points,const std::string& mode){
            Entity e=LuaId::ResolveLive(world,id);if(e==NullEntity)return false;
            auto* c=world.GetComponent<AIBehaviorComponent>(e);if(!c||points.size()>256)return false;
            std::vector<AttachmentRef> refs;
            for(size_t i=1;i<=points.size();++i){
                LuaEntity id=points.get<LuaEntity>(i);Entity target=LuaId::ResolveLive(world,id);
                if(target==NullEntity||target==e)return false;
                AttachmentRef ref;ref.BindAndStamp(world,target);refs.push_back(ref);
            }
            c->patrolPoints=std::move(refs);c->patrolMode=mode=="pingpong"?1:mode=="once"?2:0;
            AI::ResetBehavior(world,e);return true;
        });
        behavior.set_function("Target",[&world](LuaEntity id){return LuaId::OutMaybe(&world,AI::BehaviorTarget(world,LuaId::ResolveLive(world,id)));});
        behavior.set_function("Get",[&world](LuaEntity id,const std::string& key,sol::this_state ts)->sol::object {
            sol::state_view lv(ts);Entity e=LuaId::ResolveLive(world,id);
            if(key=="TargetEntity")return sol::make_object(lv,LuaId::OutMaybe(&world,AI::BehaviorTarget(world,e)));
            auto* bb=world.GetComponent<BlackboardComponent>(e);
            if(!bb)return sol::lua_nil;
            auto it=bb->values.find(key);return it==bb->values.end()?sol::object(sol::lua_nil):AI::BBValueToLua(lv,it->second);
        });
        behavior.set_function("Alert",[&world](LuaEntity id,LuaEntity target){return AI::AlertBehavior(world,LuaId::ResolveLive(world,id),LuaId::ResolveLive(world,target));});
        behavior.set_function("Reset",[&world](LuaEntity id){AI::ResetBehavior(world,LuaId::ResolveLive(world,id));});
        behavior.set_function("SetTargetable",[&world](LuaEntity id,bool enabled){
            if(auto* p=world.GetComponent<PerceptibleComponent>(LuaId::ResolveLive(world,id)))p->enabled=enabled;
        });
        lua["AIBehavior"]=behavior;
    }

    // ---- Perception.* (installed): read-only by design ----
    // AI::PerceptionSystem senses in the AI phase; these only report what it found. There is
    // deliberately no Perception.Scan(): it would let a BT leaf fire a broad-phase sweep plus
    // raycasts at any point in the frame, the per-agent cost the system exists to remove.
    {
        sol::table pt = lua.create_table();

        // The 90% query: closest hostile visible RIGHT NOW, or nil.
        // visibleEntities is ordered by the broad phase, not by distance, so
        // the closest is picked here rather than assumed to be first.
        pt.set_function("GetTarget",
            [&world](LuaEntity entityId) -> sol::optional<LuaEntity>
            {
                const Entity e = LuaId::ResolveLive(world, entityId);
                const auto* p = world.GetComponent<PerceptionComponent>(e);
                if (!p || p->visibleEntities.empty()) return sol::nullopt;
                const auto* self = world.GetComponent<GlobalTransform>(e);
                if (!self) return sol::nullopt;
                const PerceivedEntity* best = nullptr;
                float bestSq = 3.4e38f;
                for (const auto& v : p->visibleEntities)
                {
                    const float dx = v.lastKnownPos.x - self->matrix._41;
                    const float dz = v.lastKnownPos.z - self->matrix._43;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestSq) { bestSq = d2; best = &v; }
                }
                if (!best) return sol::nullopt;
                return LuaId::OutMaybe(&world, best->entity);
            });

        pt.set_function("CanSee",
            [&world](LuaEntity entityId, LuaEntity targetId) -> bool
            {
                const Entity e = LuaId::ResolveLive(world, entityId);
                const Entity t = LuaId::ResolveLive(world, targetId);
                const auto* p = world.GetComponent<PerceptionComponent>(e);
                if (!p) return false;
                for (const auto& v : p->visibleEntities)
                    if (v.entity == t) return true;
                return false;
            });

        // Where the threat was last actually SEEN, and how stale that is. The
        // one question a stateless query cannot answer, and the whole reason
        // lastKnownThreat outlives visibility.
        pt.set_function("GetLastKnown",
            [&world](LuaEntity entityId, sol::this_state ts) -> sol::object
            {
                sol::state_view lv(ts);
                const Entity e = LuaId::ResolveLive(world, entityId);
                const auto* p = world.GetComponent<PerceptionComponent>(e);
                if (!p || p->lastKnownThreat == NullEntity) return sol::lua_nil;
                sol::table r = lv.create_table();
                r["x"] = p->lastKnownThreatPosition.x;
                r["y"] = p->lastKnownThreatPosition.y;
                r["z"] = p->lastKnownThreatPosition.z;
                r["entity"] = LuaId::OutMaybe(&world, p->lastKnownThreat);
                bool stillVisible = false;
                for (const auto& v : p->visibleEntities)
                    if (v.entity == p->lastKnownThreat) { stillVisible = true; break; }
                r["visible"] = stillVisible;
                return r;
            });

        // Per-agent query counts, so a benchmark can report "perception
        // queries / raycasts per second" without a profiler build.
        pt.set_function("GetStats",
            [&world](LuaEntity entityId, sol::this_state ts) -> sol::object
            {
                sol::state_view lv(ts);
                const Entity e = LuaId::ResolveLive(world, entityId);
                const auto* p = world.GetComponent<PerceptionComponent>(e);
                if (!p) return sol::lua_nil;
                sol::table r = lv.create_table();
                r["candidates"] = p->lastCandidates;
                r["rays"]       = p->lastRays;
                r["visible"]    = static_cast<int>(p->visibleEntities.size());
                return r;
            });

        // Fan the per-agent sensing loop across workers. Off by default in
        // the system itself; exposed here because "how much does perception
        // gain from workers" is a number the benchmark has to produce, and a
        // system that switched mode on its own would make it unreadable.
        pt.set_function("SetParallel", [&perception](bool on) { perception.parallel = on; });
        pt.set_function("IsParallel",  [&perception]() -> bool { return perception.parallel; });

        lua["Perception"] = pt;
    }

    // ---- AIStats.* (installed) ----
    // Last frame's AI-phase cost and query counts; the benchmark samples it once per frame
    // for p50/p95/p99 of the AI axis alone. Frame cpuMs would not do: at realistic agent
    // counts the AI signal sits under render and animation noise (see AIProfile.h).
    {
        sol::table st = lua.create_table();
        st.set_function("Get", [](sol::this_state ts) -> sol::table
        {
            sol::state_view lv(ts);
            const AI::AIProfile& p = AI::AIProfile::Get();
            sol::table r = lv.create_table();
            r["totalMs"]        = p.TotalMs();
            r["perceptionMs"]   = p.perceptionWallMs;
            // Summed across workers. perceptionCpuMs / perceptionMs is the
            // parallel speedup; the difference is work that left the main
            // thread. Neither number alone gives the split.
            r["perceptionCpuMs"] = p.perceptionCpuMs;
            r["btMs"]           = p.btWallMs;
            r["navMs"]          = p.navWallMs;
            r["motorMs"]        = p.motorWallMs;
            r["lodMs"]          = p.lodWallMs;
            r["agents"]         = p.aiAgents;
            r["btTicks"]        = p.btTicks;
            r["percAgents"]     = p.perceptionAgents;
            r["percTicked"]     = p.perceptionTicked;
            r["percCandidates"] = p.perceptionCandidates;
            r["percRays"]       = p.perceptionRays;
            r["navAgents"]      = p.navAgents;
            r["pathQueries"]    = p.pathQueries;
            r["pathFailures"]   = p.pathFailures;
            return r;
        });
        lua["AIStats"] = st;
    }

    // ---- AnimStats.* (installed) ----
    // AIStats' sibling for the animation chain: per-stage cost of
    // Renderer::TickAnimationChain without a Tracy capture per experiment. It lives here only
    // because Anim::AnimProfile::Get() is a process-global sink that needs no system
    // reference; move it to its own binding file once it needs one.
    {
        sol::table at = lua.create_table();
        at.set_function("Get", [](sol::this_state ts) -> sol::table
        {
            sol::state_view lv(ts);
            const Anim::AnimProfile& p = Anim::AnimProfile::Get();
            sol::table r = lv.create_table();
            r["totalMs"]        = p.TotalMs();
            r["cullMs"]         = p.cullMs;
            r["visibilityPrepassMs"] = p.visibilityPrepassMs;
            r["sampleMs"]       = p.sampleMs;
            r["ikRigMs"] = p.ikRigMs;
            r["ikRigPrepareMs"] = p.ikRigPrepareMs;
            r["ikRigSolveMs"] = p.ikRigSolveMs;
            r["ikRigEntities"] = p.ikRigEntities;
            r["ikRigProbes"] = p.ikRigProbes;
            r["ikRigSolves"] = p.ikRigSolves;
            r["ikMs"]           = p.ikMs;
            r["chainMs"]        = p.chainMs;
            r["localToWorldMs"] = p.localToWorldMs;
            // The chain's cost split: serial prologue, parallel wall, and parallel CPU summed across
            // workers; cpu / parallel is the speedup that phase achieved.
            r["chainSerialMs"]     = p.chainSerialMs;
            r["chainParallelMs"]   = p.chainParallelMs;
            r["chainCpuMs"]        = p.chainCpuMs;
            // The editor preview lane's own three (the ones above are the scene's; that lane never
            // writes them).
            r["previewChainSerialMs"]   = p.previewChainSerialMs;
            r["previewChainParallelMs"] = p.previewChainParallelMs;
            r["previewChainCpuMs"]      = p.previewChainCpuMs;
            r["sampleCpuMs"]       = p.sampleCpuMs;
            r["localToWorldCpuMs"] = p.localToWorldCpuMs;
            r["socketMs"]       = p.socketMs;
            r["followMs"]       = p.followMs;
            r["faceMs"]         = p.faceMs;
            r["headTrackMs"]    = p.headTrackMs;
            r["tailMs"]         = p.tailMs;
            r["prologueMs"]     = p.prologueMs;
            r["wallMs"]         = p.wallMs;
            r["phaseAllFourMs"] = p.phaseAllFourMs;
            r["phaseAnimMs"]    = p.phaseAnimationMs;
            r["animDriverMs"]   = p.animDriverMs;
            r["timelineMs"]     = p.timelineMs;
            r["boneAabbMs"]     = p.boneAabbMs;
            r["skinMatrixMs"]   = p.skinMatrixMs;
            r["skinJobsMs"]     = p.skinJobsMs;
            r["poseFlushMs"]    = p.poseFlushMs;
            // Which cascade admitted each shadow-only skeleton. cullShadow
            // alone cannot tell "casting a resolvable shadow into cascade 0"
            // from "clipped the far terrain cascade", and those want opposite
            // treatment. Sums to cullShadow.
            r["cullShadowC0"]   = p.cullShadowCascade[0];
            r["cullShadowC1"]   = p.cullShadowCascade[1];
            r["cullShadowC2"]   = p.cullShadowCascade[2];
            r["cullShadowC3"]   = p.cullShadowCascade[3];
            // True when the dev shadow holdout produced these numbers, i.e.
            // rendering was WRONG (T-posed casters) and the timing is a
            // measurement, never a shippable result.
            r["shadowHoldout"]  = p.shadowHoldout;
            r["skinSections"]   = p.skinSections;
            r["phaseBoneMs"]    = p.phaseBoneAttachmentMs;
            r["phaseSecPhysMs"] = p.phaseSecondaryPhysMs;
            r["phasePreRendMs"] = p.phasePreRenderMs;
            r["skeletons"]      = p.skeletons;
            r["animated"]       = p.animated;
            r["cullNoAabb"]     = p.cullNoAabb;
            r["cullFrustum"]    = p.cullFrustum;
            r["cullShadow"]     = p.cullShadow;
            r["cullSkipped"]    = p.cullSkipped;
            r["bonesSampled"]   = p.bonesSampled;
            r["skinnedVerts"]   = p.skinnedVerts;
            r["skinSections"]   = p.skinSections;
            // Facial (morph-target) layer. facialSlots is the only observable downstream of
            // BuildSkinJobs: Face.GetResolved proves the CPU fold, this proves the weights reached
            // the morph pool. facialMs, not faceMs: faceMs above is FaceShading (SDF face shadows).
            r["facialMs"]       = p.facialMs;
            r["facialEntities"] = p.facialEntities;
            r["facialSlots"]    = p.facialSlots;
            r["morphJobs"]      = p.morphJobs;     // jobs with an active morph weight
            r["morphActive"]    = p.morphActive;   // ... and those weights
            r["chainEntities"]   = p.chainEntities;
            r["chainSubsteps"]  = p.chainSubsteps;
            r["chainTier0"]     = p.chainTier0;
            r["chainTier1"]     = p.chainTier1;
            r["chainTier2"]     = p.chainTier2;
            // Where the chain's CPU goes (main lane): per tier, shadow-only, FK+prepare.
            r["chainTier0CpuMs"]      = p.chainTierCpuMs[0];
            r["chainTier1CpuMs"]      = p.chainTierCpuMs[1];
            r["chainTier2CpuMs"]      = p.chainTierCpuMs[2];
            r["chainShadowOnlyCpuMs"] = p.chainShadowOnlyCpuMs;
            r["chainPrepCpuMs"]       = p.chainPrepCpuMs;
            r["chainShadowOnly"]      = p.chainShadowOnly;
            return r;
        });
        // Switches the virtual bones' evaluation at runtime, so their cost is an in-process A/B
        // (tools/bench_virtual_bones.py). Returns the setting now in force.
        at.set_function("SetVirtualBones", [](bool on) -> bool
        {
            AnimationSystem::SetVirtualBonesEnabled(on);
            return AnimationSystem::VirtualBonesEnabled();
        });
        at.set_function("VirtualBones", []() -> bool { return AnimationSystem::VirtualBonesEnabled(); });
        lua["AnimStats"] = at;
    }

    // ---- AILOD.* (installed) ----
    // A separate table on purpose: `AI` stays nil so a leftover AI.SetGoal fails loudly, and
    // publishing these under `AI` would resurrect that surface. Scriptable because
    // AILODSystem rewrites every AIComponent::tickInterval from camera distance every frame:
    // a script's interval is overwritten before the next tick, and an agent-count benchmark
    // would really measure the camera spread. SetEnabled(false) hands the cadence to the
    // caller.
    {
        sol::table lt = lua.create_table();

        lt.set_function("SetEnabled", [&lod](bool on) { lod.enabled = on; });
        lt.set_function("IsEnabled",  [&lod]() -> bool { return lod.enabled; });

        // Author one agent's cadence; meaningful only with LOD disabled. Warn instead of a
        // silent no-op: the write lands and is clobbered next frame, which looks like a broken
        // binding.
        lt.set_function("SetTickInterval",
            [&world, &lod](LuaEntity entityId, float seconds) -> bool
            {
                const Entity e = LuaId::ResolveLive(world, entityId);
                auto* ai = world.GetComponent<AIComponent>(e);
                if (!ai) return false;
                static bool s_warned = false;
                if (lod.enabled && !s_warned)
                {
                    s_warned = true;
                    LOG_WARNING("AILOD.SetTickInterval: LOD is still enabled — "
                                "this value is overwritten every frame. "
                                "Call AILOD.SetEnabled(false) first.");
                }
                ai->tickInterval = seconds < 0.f ? 0.f : seconds;
                // Re-offset: an agent whose cadence just changed should not
                // inherit the phase it was given for the old interval.
                ai->phaseSeeded = false;
                return true;
            });

        lt.set_function("SetTiers",
            [&lod](sol::table t)
            {
                auto grab = [&t](const char* k, float& dst)
                {
                    sol::optional<float> v = t[k];
                    if (v) dst = *v;
                };
                grab("boss",          lod.tiers.bossInterval);
                grab("combat",        lod.tiers.combatInterval);
                grab("distant",       lod.tiers.distantInterval);
                grab("offscreen",     lod.tiers.offscreenInterval);
                grab("combatRadius",  lod.tiers.combatRadius);
                grab("distantRadius", lod.tiers.distantRadius);
            });

        lua["AILOD"] = lt;
    }
}

} // namespace Intent
