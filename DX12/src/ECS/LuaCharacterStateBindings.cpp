#include "ECS/AnimGraphSystem.h"
#include "Scripting/LuaDoc.h"   // bindings described as they are bound
#include "ECS/LuaCharacterStateBindings.h"
#include "Gameplay/AttackPlayback.h"
#include "ECS/AnimationComponents.h"
#include "ECS/CharacterStateComponent.h"
#include "ECS/CharacterIntentComponent.h"   // BrainComponent / intent mailbox
#include "ECS/CharacterAnimDriver.h"        // CharacterAnimDriverComponent
#include "ECS/LocomotionComponents.h"       // Locomotion.* bindings
#include "ECS/HierarchyComponents.h"        // Children (anim-entity walk)
#include "ECS/AnimEntityResolve.h"          // Anim::ResolveAnimEntity (shared)
#include "ECS/ECS.h"
#include "ECS/CharacterControllerComponent.h"
#include "Resource/AnimationClipSystem.h"

#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <algorithm>
#include <unordered_set>   // visited-set for the Children walk

namespace
{

CharacterStateComponent& EnsureComp(World& w, Entity e)
{
    if (auto* p = w.GetComponent<CharacterStateComponent>(e)) return *p;
    w.AddComponent<CharacterStateComponent>(e, CharacterStateComponent{});
    return *w.GetComponent<CharacterStateComponent>(e);
}

int FindByName(const CharacterStateComponent& cs, const std::string& name)
{
    for (size_t i = 0; i < cs.states.size(); ++i)
        if (cs.states[i].name == name) return static_cast<int>(i);
    return -1;
}

// Locomotion / AnimLayer / LocoTransition components live on the ANIMATED entity (the one with
// Skeleton + Animation), which on a PMX character is a descendant of the root gameplay code holds.
// Every Locomotion.* binding routes through this so scripts can pass the root; it is the same walk
// CharacterAnimDriver and AnimationClipSystem use (ECS/AnimEntityResolve.h), so all three agree on
// the target.
Entity ResolveAnimEntity(World& world, Entity root)
{
    return Anim::ResolveAnimEntity(world, root);
}

// Takes LuaEntity (not Entity) so the generation check happens here, once, for every Animator.*
// binding: a bare cast would address whatever entity recycled the slot. Walks descendants, so
// Animator.* accepts a character ROOT like Character.* / Locomotion.*: an alias table authored on the
// root itself would be registered, never found, and never logged. Falls back to the entity itself
// only when nothing under it is animated.
Entity ResolveAnimatorOwner(World& world, LuaEntity entity)
{
    return Anim::ResolveAnimEntityOrSelf(world, LuaId::ResolveLive(world, entity));
}

// Lowercases internally, so Trigger and SetupTransitions accept any case, and reports an unknown kind
// through `ok` (the result is then Start) so callers can warn instead of defaulting in silence.
uint8_t ParseTransitionKind(std::string s, bool* ok = nullptr)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ok) *ok = true;
    // "start" must be matched explicitly: the fall-through also returns Start but sets ok=false, and
    // "start" is SetupTransitions' default for a row without a `kind`.
    if (s == "start") return static_cast<uint8_t>(LocoTransitionKind::Start);
    if (s == "stop")  return static_cast<uint8_t>(LocoTransitionKind::Stop);
    if (s == "pivot") return static_cast<uint8_t>(LocoTransitionKind::Pivot);
    if (s == "turn" || s == "turninplace")
        return static_cast<uint8_t>(LocoTransitionKind::TurnInPlace);
    if (s == "jumpstart") return static_cast<uint8_t>(LocoTransitionKind::JumpStart);
    if (s == "jumploop")  return static_cast<uint8_t>(LocoTransitionKind::JumpLoop);
    if (s == "jumpend")   return static_cast<uint8_t>(LocoTransitionKind::JumpEnd);
    if (ok) *ok = false;
    return static_cast<uint8_t>(LocoTransitionKind::Start);
}

} // namespace

void RegisterLuaCharacterStateBindings(sol::state& lua, World& world,
                                       Resource::AnimationClipSystem& animationClips)
{
    Attack::RegisterLua(lua,world);
    // Merge into any pre-existing Character table (LuaPlayerBindings.cpp merges the same way): the
    // registration order of the two can swap, so both sides must be merge-safe.
    sol::object existing = lua["Character"];
    sol::table tbl = existing.is<sol::table>()
                   ? existing.as<sol::table>()
                   : lua.create_table();

    tbl.set_function("AddState",
        [&world, &animationClips](LuaEntity entityId, const std::string& name,
                 const std::string& clipPath, sol::object optsObj) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;

            CharacterStateComponent& cs = EnsureComp(world, e);
            const int idx = FindByName(cs, name);

            CharacterStateComponent::StateEntry st;
            st.name     = name;
            st.clipPath = clipPath;
            // Defaults; an opts table can override individually.
            st.looping       = true;
            st.playbackSpeed = 1.0f;
            std::string sourceClip;
            std::string sourceMorphClip;
            if (optsObj.is<sol::table>())
            {
                sol::table o = optsObj.as<sol::table>();
                if (o["loop"].valid())  st.looping       = o.get_or("loop",  true);
                if (o["speed"].valid()) st.playbackSpeed = o.get_or("speed", 1.0f);
                sourceClip      = o.get_or("source", std::string{});
                sourceMorphClip = o.get_or("morph",  std::string{});
            }

            // Authors the same alias-based AnimationSet entry Animator.Play uses, so Lua / BT scripts never keep
            // process-local clip handles in CharacterStateComponent.
            if (!animationClips.SetAnimatorClip(world, e, name, clipPath,
                                                 st.looping, st.playbackSpeed,
                                                 sourceClip, sourceMorphClip))
                return false;

            if (idx >= 0)
            {
                // Same-name re-registration overwrites the entry but keeps its slot: currentIdx / pendingIdx may
                // already point at it.
                auto& dst = cs.states[idx];
                const bool clipChanged = (dst.clipPath != st.clipPath);
                dst.clipPath      = st.clipPath;
                dst.looping       = st.looping;
                dst.playbackSpeed = st.playbackSpeed;
                if (clipChanged)
                {
                    if (dst.clipResHandle.IsValid())
                        animationClips.ReleaseClip(dst.clipResHandle);
                    dst.clipLibIdx    = kInvalidClipIndex;  // force re-bind
                    dst.clipResHandle = {};
                }
            }
            else
            {
                cs.states.push_back(std::move(st));
            }
            return true;
        });

    tbl.set_function("RemoveState",
        [&world, &animationClips](LuaEntity entityId, const std::string& name) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto* cs = world.GetComponent<CharacterStateComponent>(e);
            if (!cs) return false;

            const int idx = FindByName(*cs, name);
            if (idx < 0) return false;
            auto& removed = cs->states[static_cast<size_t>(idx)];
            if (removed.clipResHandle.IsValid())
                animationClips.ReleaseClip(removed.clipResHandle);

            const bool removedActive = (cs->currentIdx == idx || cs->pendingIdx == idx);
            cs->states.erase(cs->states.begin() + idx);
            if (cs->currentIdx == idx) cs->currentIdx = -1;
            else if (cs->currentIdx > idx) --cs->currentIdx;
            if (cs->pendingIdx == idx) cs->pendingIdx = -1;
            else if (cs->pendingIdx > idx) --cs->pendingIdx;
            cs->blendElapsed = 0.f;
            animationClips.RemoveAnimatorClip(world, e, name);
            if (removedActive) animationClips.StopAnimator(world, e);
            return true;
        });

    tbl.set_function("ClearStates",
        [&world, &animationClips](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto* cs = world.GetComponent<CharacterStateComponent>(e);
            if (!cs) return false;
            for (auto& state : cs->states)
            {
                if (state.clipResHandle.IsValid())
                    animationClips.ReleaseClip(state.clipResHandle);
                animationClips.RemoveAnimatorClip(world, e, state.name);
            }
            cs->states.clear();
            cs->currentIdx = cs->pendingIdx = -1;
            cs->blendElapsed = 0.f;
            animationClips.StopAnimator(world, e);
            return true;
        });

    tbl.set_function("SetState",
        [&world, &animationClips](LuaEntity entityId, const std::string& name,
                 sol::object blendObj) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto* cs = world.GetComponent<CharacterStateComponent>(e);

            float dur = cs ? cs->defaultBlendDuration : 0.2f;
            if (blendObj.is<float>())  dur = blendObj.as<float>();
            else if (blendObj.is<int>()) dur = static_cast<float>(blendObj.as<int>());

            // Preferred path: AddState mirrors definitions into the AnimationSet, so this becomes Animator.Play.
            if (animationClips.HasAnimatorClip(world, e, name))
                return animationClips.QueueAnimatorPlay(
                    world, e, name, std::max(0.f, dur), false);

            if (!cs) { LOG_WARNING("Character.SetState: entity %u has no CharacterStateComponent", entityId.raw()); return false; }
            if (cs->animatorMigrationComplete)
            {
                LOG_WARNING("Character.SetState: Animator alias '%s' is not registered on entity %u",
                            name.c_str(), entityId.raw());
                return false;
            }

            const int idx = FindByName(*cs, name);
            if (idx < 0) { LOG_WARNING("Character.SetState: state '%s' not registered on entity %u", name.c_str(), entityId.raw()); return false; }

            // Same as current AND no blend in flight -> no-op. Re-triggering a state mid-blend swaps the target
            // on purpose (IDLE -> WALK -> back to IDLE before WALK finishes).
            if (idx == cs->currentIdx && cs->pendingIdx < 0) return true;

            // Already blending toward this state: let the blend continue. Do not restart it: a caller that calls
            // SetState every BT tick (SetAnimState("RUN", 0.15) in a Chase loop) would reset blendElapsed each
            // frame and the cross-fade would never complete.
            if (idx == cs->pendingIdx) return true;

            // Blend duration: explicit argument wins; otherwise use the
            // component's default.
            cs->blendDuration = std::max(0.0f, dur);
            cs->blendElapsed  = 0.0f;
            cs->pendingIdx    = idx;
            return true;
        });

    tbl.set_function("GetState",
        [&world, &animationClips](sol::this_state ts, LuaEntity entityId) -> sol::object
        {
            sol::state_view L(ts);
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return sol::lua_nil;
            std::string current = animationClips.GetCurrentAnimatorClip(world, e);
            if (!current.empty()) return sol::make_object(L, std::move(current));
            const auto* cs = world.GetComponent<CharacterStateComponent>(e);
            if (!cs || cs->currentIdx < 0 ||
                static_cast<size_t>(cs->currentIdx) >= cs->states.size())
                return sol::lua_nil;
            return sol::make_object(L, cs->states[cs->currentIdx].name);
        });

    tbl.set_function("IsBlending",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            const Entity owner = ResolveAnimatorOwner(world, e);
            if (const auto* transition = world.GetComponent<AnimatorTransitionComponent>(owner))
                if (transition->active) return true;
            const auto* cs = world.GetComponent<CharacterStateComponent>(e);
            return cs && cs->pendingIdx >= 0;
        });

    // ---- Possession / intent (unified control layer) ----
    // Character.Possess(e, "Player"|"AI"|"None"): switch which brain writes this entity's
    // CharacterIntentComponent. Components stay attached; the brain systems gate on BrainComponent.type
    // (BrainAllows).
    tbl.set_function("Possess",
        [&world](LuaEntity entityId, const std::string& who) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            BrainType t = BrainType::None;
            if      (who == "Player") t = BrainType::Player;
            else if (who == "AI")     t = BrainType::AI;
            else if (who != "None")
            {
                LOG_WARNING("Character.Possess: unknown brain '%s' (Player/AI/None)",
                            who.c_str());
                return false;
            }
            world.EnsurePool<BrainComponent>();
            if (auto* b = world.GetComponent<BrainComponent>(e)) b->type = t;
            else
            {
                BrainComponent nb{};
                nb.type = t;
                world.AddComponent<BrainComponent>(e, nb);
            }
            // Hand-off: zero the standing intent so the previous brain's last movement does not keep driving the
            // motor.
            if (auto* it = world.GetComponent<CharacterIntentComponent>(e))
            {
                *it = CharacterIntentComponent{};
            }
            if (auto* cc=world.GetComponent<CharacterControllerComponent>(e)) {
                cc->desiredHorizontalVelocity={0.f,0.f,0.f};
                cc->jumpRequested=false;
            }
            return true;
        });

    tbl.set_function("GetBrain",
        [&world](LuaEntity entityId) -> std::string
        {
            const auto* b = world.GetComponent<BrainComponent>(
                entityId.raw());
            if (!b) return "Legacy";
            switch (b->type)
            {
                case BrainType::Player: return "Player";
                case BrainType::AI:     return "AI";
                default:                return "None";
            }
        });

    // Character.RequestAction(e, "Attack"|"Dodge"|"Interact"[, variant][, opts]): one-shot action
    // mailbox consumed by CharacterAnimDriverTickSystem.
    //   variant 0/absent = the driver picks a random alias from that kind's list; variant N plays entry N
    //   (1-based), which is how a combo addresses its steps.
    //   opts: { interrupt = bool, blend = float }. `interrupt` cuts a running action short; WITHOUT it a
    //   request made while an action plays is HELD, not dropped, and fires the frame that action ends:
    //   the one-slot input buffer a combo chains on.
    tbl.set_function("RequestAction",
        [&world](LuaEntity entityId, const std::string& action,
                 sol::optional<int> variant,
                 sol::optional<sol::table> optsObj) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            CharacterActionType t;
            if      (action == "Attack")   t = CharacterActionType::Attack;
            else if (action == "Dodge")    t = CharacterActionType::Dodge;
            else if (action == "Interact") t = CharacterActionType::Interact;
            else
            {
                LOG_WARNING("Character.RequestAction: unknown action '%s' "
                            "(Attack/Dodge/Interact)", action.c_str());
                return false;
            }
            world.EnsurePool<CharacterIntentComponent>();
            auto* it = world.GetComponent<CharacterIntentComponent>(e);
            if (!it)
            {
                world.AddComponent<CharacterIntentComponent>(e, {});
                it = world.GetComponent<CharacterIntentComponent>(e);
                if (!it) return false;
            }
            it->action        = t;
            it->actionVariant = static_cast<std::uint8_t>(
                (std::max)(0, variant.value_or(0)));
            it->actionInterrupt = false;
            it->actionBlend     = -1.f;
            if (optsObj)
            {
                sol::table o = *optsObj;
                it->actionInterrupt = o.get_or("interrupt", false);
                it->actionBlend     = o.get_or("blend", -1.f);
            }
            return true;
        });

    // Character.CancelAction(e): drop the running action clip with no replacement; the driver cross-fades
    // back to locomotion next tick. This is the ONLY way out of a hold action (death pose) and the only
    // real cancel: clearing the mailbox does nothing once an action has started, because the latch that
    // suppresses gait lives on the driver component. Returns false when there is no driver or nothing
    // was playing.
    tbl.set_function("CancelAction",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            // Drop a not-yet-consumed request too, or it would fire next tick
            // and immediately undo the cancel.
            if (auto* it = world.GetComponent<CharacterIntentComponent>(e))
            {
                it->action          = CharacterActionType::None;
                it->actionVariant   = 0;
                it->actionAlias.clear();
                it->actionHold      = false;
                it->actionInterrupt = false;
                it->actionBlend     = -1.f;
            }
            return Character::CancelAction(world, e);
        });

    // Character.GetAnimEntity(e): the entity that carries Skeleton + Animation under `e` (self,
    // SkeletonRef, or the first animated descendant); 0 when nothing under `e` is animated. The
    // Character.* / Locomotion.* / Animator.* bindings resolve this internally; it is for code that must
    // address the animated entity directly (AnimLayer authoring, a debug dump, an Engine.HasComponent
    // probe).
    tbl.set_function("GetAnimEntity",
        [&world](LuaEntity entityId) -> LuaEntity
        {
            const Entity root = LuaId::ResolveLive(world, entityId);
            if (root == NullEntity) return LuaId::Out(world, NullEntity);
            return LuaId::Out(world, ResolveAnimEntity(world, root));
        });

    // Character.IsActionActive(e): true while the driver's action clip is still playing; use it instead
    // of wall-clock duration tables.
    tbl.set_function("IsActionActive",
        [&world](LuaEntity entityId) -> bool
        {
            const auto* d = world.GetComponent<CharacterAnimDriverComponent>(
                entityId.raw());
            return d && d->actionActive;
        });

    // Character.PlayActionAlias(e, alias[, opts{blend=, hold=, interrupt=}]): play any AnimationSet alias
    // through the driver's action layer (hit reacts, deaths, emotes). hold=true freezes on the final pose
    // and gait never resumes: the death-pose contract.
    tbl.set_function("PlayActionAlias",
        [&world](LuaEntity entityId, const std::string& alias,
                 sol::optional<sol::table> optsObj) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity || alias.empty()) return false;
            world.EnsurePool<CharacterIntentComponent>();
            auto* it = world.GetComponent<CharacterIntentComponent>(e);
            if (!it)
            {
                world.AddComponent<CharacterIntentComponent>(e, {});
                it = world.GetComponent<CharacterIntentComponent>(e);
                if (!it) return false;
            }
            it->action          = CharacterActionType::Custom;
            it->actionAlias     = alias;
            it->actionHold      = false;
            it->actionBlend     = -1.f;
            it->actionInterrupt = false;
            if (optsObj)
            {
                sol::table o = *optsObj;
                it->actionHold      = o.get_or("hold",  false);
                it->actionBlend     = o.get_or("blend", -1.f);
                // Cut a running action short instead of queueing behind it: a hit react or a death cannot wait for
                // the swing that caused it to finish.
                it->actionInterrupt = o.get_or("interrupt", false);
            }
            return true;
        });

    // Adopt an already sampled cinematic pose without queueing/restarting its clip.
    // May be armed under a sequencer claim; the sequencer retains its clock until release.
    tbl.set_function("AdoptCurrentPose",
        [&world, &animationClips](LuaEntity entityId, float blend, bool hold) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            const Entity target = Anim::ResolveAnimEntityOrSelf(world, e);
            auto* drv = world.GetComponent<CharacterAnimDriverComponent>(e);
            auto* anim = world.GetComponent<AnimationComponent>(target);
            if (!drv || !anim) return false;
            const std::string alias = animationClips.GetCurrentAnimatorClip(world, target);
            if (alias.empty()) return false;
            if (auto* it = world.GetComponent<CharacterIntentComponent>(e))
            { it->action = CharacterActionType::None; it->actionAlias.clear(); }
            drv->actionActive = true;
            drv->actionStarted = true;
            drv->actionHold = hold;
            drv->actionArmTime = 0.f;
            drv->activeActionAlias = alias;
            drv->parkedAlias.clear();
            drv->gaitBlend = std::max(0.f, blend);
            if (auto* lc = world.GetComponent<LocomotionComponent>(target)) lc->locoWeight = 0.f;
            if (!anim->externalTime) anim->paused = true;
            return true;
        });

    // Character.SetupAnimDriver(e, opts): attach / configure the automatic animation driver. opts:
    //   { idle="IDLE" or idles={...}, walk="WALK" or walks={...},
    //     walkBack=, walkLeft=, walkRight=, run=, air=,
    //     walkThreshold=, runThreshold=, gaitBlend=, actionBlend=,
    //     airGrace=, idleReroll=,
    //     attacks={ "A1", "A2", ... },          -- Attack variants
    //     dodge="DASH" or dodges={ ... },       -- Dodge variants
    //     interact="PICKUP",                    -- Interact (single clip)
    //     graphSpeedParam=, graphDirectionParam=, ..., graphActionSlot= }   -- AnimGraph names
    tbl.set_function("SetupAnimDriver",
        [&world](LuaEntity entityId, sol::optional<sol::table> optsObj) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            world.EnsurePool<CharacterAnimDriverComponent>();
            auto* d = world.GetComponent<CharacterAnimDriverComponent>(e);
            if (!d)
            {
                world.AddComponent<CharacterAnimDriverComponent>(e, {});
                d = world.GetComponent<CharacterAnimDriverComponent>(e);
                if (!d) return false;
            }
            if (optsObj)
            {
                sol::table o = *optsObj;

                // Variant lists accept either a scalar key or an array key.
                auto readVariants = [&o](const char* scalarKey,
                                         const char* arrayKey,
                                         std::string* dst, int cap) -> int
                {
                    sol::object arr = o[arrayKey];
                    if (arr.is<sol::table>())
                    {
                        sol::table at = arr.as<sol::table>();
                        int count = 0;
                        for (int i = 1; i <= cap; ++i)
                        {
                            sol::object v = at[i];
                            if (!v.is<std::string>()) break;
                            dst[count++] = v.as<std::string>();
                        }
                        return count;
                    }
                    sol::object s = o[scalarKey];
                    if (s.is<std::string>())
                    {
                        dst[0] = s.as<std::string>();
                        return 1;
                    }
                    return -1;   // untouched
                };

                int nIdle = readVariants("idle", "idles", d->idleAliases,
                                         CharacterAnimDriverComponent::MAX_VARIANTS);
                if (nIdle >= 0) d->idleAliasCount = nIdle;
                int nWalk = readVariants("walk", "walks", d->walkAliases,
                                         CharacterAnimDriverComponent::MAX_VARIANTS);
                if (nWalk >= 0) d->walkAliasCount = nWalk;

                d->walkBackAlias  = o.get_or("walkBack",  d->walkBackAlias);
                d->walkLeftAlias  = o.get_or("walkLeft",  d->walkLeftAlias);
                d->walkRightAlias = o.get_or("walkRight", d->walkRightAlias);
                d->runAlias       = o.get_or("run",  d->runAlias);
                d->airAlias       = o.get_or("air",  d->airAlias);
                d->walkThreshold  = o.get_or("walkThreshold", d->walkThreshold);
                d->runThreshold   = o.get_or("runThreshold",  d->runThreshold);
                d->gaitBlend      = o.get_or("gaitBlend",     d->gaitBlend);
                d->actionBlend    = o.get_or("actionBlend",   d->actionBlend);
                d->airTimeGrace   = o.get_or("airGrace",      d->airTimeGrace);
                d->idleRerollSec  = o.get_or("idleReroll",    d->idleRerollSec);
                // Action-layer alias lists. Unlike idles/walks these are zeroed and refilled whenever the key is a
                // table, so an empty table clears the list and a request of that kind then plays nothing.
                auto readActionList = [&o](const char* key, std::string* dst,
                                           int cap, int* count)
                {
                    sol::object arr = o[key];
                    if (!arr.is<sol::table>()) return;
                    sol::table at = arr.as<sol::table>();
                    *count = 0;
                    for (int i = 1; i <= cap; ++i)
                    {
                        sol::object v = at[i];
                        if (!v.is<std::string>()) break;
                        dst[(*count)++] = v.as<std::string>();
                    }
                };
                readActionList("attacks", d->attackAliases,
                               CharacterAnimDriverComponent::MAX_ATTACKS,
                               &d->attackAliasCount);
                // Dodge accepts a list for directional / alternating rolls; `dodge` is the single-clip shorthand.
                // Both feed CharacterActionType::Dodge.
                readActionList("dodges", d->dodgeAliases,
                               CharacterAnimDriverComponent::MAX_VARIANTS,
                               &d->dodgeAliasCount);
                if (sol::object one = o["dodge"]; one.is<std::string>())
                {
                    d->dodgeAliases[0]  = one.as<std::string>();
                    d->dodgeAliasCount  = 1;
                }
                d->interactAlias = o.get_or("interact", d->interactAlias);
                d->graphSpeedParam=o.get_or("graphSpeedParam",d->graphSpeedParam);
                d->graphDirectionParam=o.get_or("graphDirectionParam",d->graphDirectionParam);
                d->graphGroundedParam=o.get_or("graphGroundedParam",d->graphGroundedParam);
                d->graphVerticalSpeedParam=o.get_or("graphVerticalSpeedParam",d->graphVerticalSpeedParam);
                d->graphAirTimeParam=o.get_or("graphAirTimeParam",d->graphAirTimeParam);
                d->graphTurnRateParam=o.get_or("graphTurnRateParam",d->graphTurnRateParam);
                d->graphMovingParam=o.get_or("graphMovingParam",d->graphMovingParam);
                d->graphGaitParam=o.get_or("graphGaitParam",d->graphGaitParam);
                d->graphActionSlot=o.get_or("graphActionSlot",d->graphActionSlot);

            }
            return true;
        });

    lua["Character"] = tbl;

    // AnimationSetComponent is authored in scenes/prefabs; this table exposes
    // only runtime intent. Lua state machines never handle clip indices or
    // ref-counted resource handles.
    sol::object animatorExisting = lua["Animator"];
    sol::table animator = animatorExisting.is<sol::table>()
                        ? animatorExisting.as<sol::table>()
                        : lua.create_table();

    animator.set_function("SetClip",
        [&world, &animationClips](LuaEntity entityId, const std::string& alias,
                                  const std::string& path, sol::object optsObj) -> bool
        {
            bool looping = true;
            float speed = 1.f;
            std::string sourceClip;
            std::string sourceMorphClip;
            bool makeDefault = false;
            bool playOnLoad = false;
            if (optsObj.is<sol::table>())
            {
                sol::table opts = optsObj.as<sol::table>();
                looping        = opts.get_or("loop", true);
                speed          = opts.get_or("speed", 1.f);
                sourceClip     = opts.get_or("source", std::string{});
                sourceMorphClip = opts.get_or("morph", std::string{});
                makeDefault    = opts.get_or("default", false);
                playOnLoad     = opts.get_or("playOnLoad", false);
            }
            if (!animationClips.SetAnimatorClip(world, LuaId::ResolveLive(world, entityId), alias, path,
                                                 looping, speed,
                                                 sourceClip, sourceMorphClip))
                return false;

            const Entity owner = ResolveAnimatorOwner(world, entityId);
            if (auto* set = owner == NullEntity
                          ? nullptr : world.GetComponent<AnimationSetComponent>(owner))
            {
                if (makeDefault || (set->defaultClip.empty() && playOnLoad))
                    set->defaultClip = alias;
                if (playOnLoad) set->playDefaultOnLoad = true;
            }
            return true;
        });

    animator.set_function("RemoveClip",
        [&world, &animationClips](LuaEntity entityId, const std::string& alias) -> bool
        {
            return animationClips.RemoveAnimatorClip(world, LuaId::ResolveLive(world, entityId), alias);
        });

    animator.set_function("ClearClips",
        [&world, &animationClips](LuaEntity entityId) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            if (owner == NullEntity ||
                !world.HasComponent<AnimationSetComponent>(owner))
                return false;
            animationClips.UnassignAnimator(world, owner, true);
            return true;
        });

    animator.set_function("HasClip",
        [&world, &animationClips](LuaEntity entityId, const std::string& alias) -> bool
        {
            return animationClips.HasAnimatorClip(world, LuaId::ResolveLive(world, entityId), alias);
        });

    animator.set_function("GetClips",
        [&world, &animationClips](sol::this_state ts, LuaEntity entityId) -> sol::table
        {
            sol::state_view L(ts);
            sol::table result = L.create_table();
            const auto aliases = animationClips.GetAnimatorClipAliases(world, LuaId::ResolveLive(world, entityId));
            for (size_t i = 0; i < aliases.size(); ++i)
                result[static_cast<int>(i + 1)] = aliases[i];
            return result;
        });

    animator.set_function("GetClipInfo",
        [&world](sol::this_state ts, LuaEntity entityId,
                 const std::string& alias) -> sol::object
        {
            sol::state_view L(ts);
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            const auto* set = owner == NullEntity
                            ? nullptr : world.GetComponent<AnimationSetComponent>(owner);
            const auto* ref = set ? set->Find(alias) : nullptr;
            if (!ref) return sol::make_object(L, sol::lua_nil);
            sol::table info = L.create_table();
            info["name"]   = ref->name;
            info["path"]   = ref->path;
            info["source"] = ref->sourceClip;
            info["morph"]  = ref->sourceMorphClip;
            info["loop"]   = ref->looping;
            info["speed"]  = ref->speed;
            info["default"] = set->defaultClip == ref->name;
            return sol::make_object(L, std::move(info));
        });

    animator.set_function("Play",
        [&world, &animationClips](LuaEntity entityId, const std::string& clipName,
                                  sol::object blendObj, sol::object restartObj) -> bool
        {
            float blend = 0.2f;
            if (blendObj.is<float>()) blend = blendObj.as<float>();
            else if (blendObj.is<int>()) blend = static_cast<float>(blendObj.as<int>());
            const bool restart = restartObj.is<bool>() && restartObj.as<bool>();
            return animationClips.QueueAnimatorPlay(
                world, LuaId::ResolveLive(world, entityId), clipName,
                std::max(0.f, blend), restart);
        });

    animator.set_function("Stop",
        [&world, &animationClips](LuaEntity entityId)
        {
            animationClips.StopAnimator(world, LuaId::ResolveLive(world, entityId));
        });

    animator.set_function("Pause",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            auto* playback = owner == NullEntity
                           ? nullptr : world.GetComponent<AnimationComponent>(owner);
            if (!playback) return false;
            playback->paused = true;
            return true;
        });

    animator.set_function("Resume",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            auto* playback = owner == NullEntity
                           ? nullptr : world.GetComponent<AnimationComponent>(owner);
            if (!playback) return false;
            playback->paused = false;
            return true;
        });

    animator.set_function("SetSpeed",
        [&world](LuaEntity entityId, float speed) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            auto* playback = owner == NullEntity
                           ? nullptr : world.GetComponent<AnimationComponent>(owner);
            if (!playback) return false;
            playback->speed = speed;
            if (auto* transition = world.GetComponent<AnimatorTransitionComponent>(owner))
                transition->targetSpeed = speed;
            return true;
        });

    LuaDoc::Fn(animator, "Animator", "GetCurrent",
        [&world, &animationClips](sol::this_state ts, LuaEntity entityId) -> sol::object
        {
            sol::state_view L(ts);
            std::string name = animationClips.GetCurrentAnimatorClip(world, LuaId::ResolveLive(world, entityId));
            return name.empty() ? sol::make_object(L, sol::lua_nil)
                                : sol::make_object(L, std::move(name));
        },
            { .doc = "The name of the clip the entity's animator is playing; nil when none.", .params = { { .name = "id" } }, .returns = "string?" });

    animator.set_function("GetTarget",
        [&world, &animationClips](sol::this_state ts, LuaEntity entityId) -> sol::object
        {
            sol::state_view L(ts);
            std::string name = animationClips.GetTargetAnimatorClip(world, LuaId::ResolveLive(world, entityId));
            return name.empty() ? sol::make_object(L, sol::lua_nil)
                                : sol::make_object(L, std::move(name));
        });

    animator.set_function("IsPlaying",
        [&world, &animationClips](LuaEntity entityId) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(owner)) {
                auto* active=graph->ActiveSlot(owner);
                return active!=nullptr;
            }
            const auto* playback = owner == NullEntity
                                 ? nullptr : world.GetComponent<AnimationComponent>(owner);
            return playback && playback->primaryClip != kInvalidAnimHandle;
        });

    animator.set_function("IsPaused",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            const auto* playback = owner == NullEntity
                                 ? nullptr : world.GetComponent<AnimationComponent>(owner);
            return playback && playback->paused;
        });

    animator.set_function("GetTime",
        [&world, &animationClips](sol::this_state ts, LuaEntity entityId) -> sol::object
        {
            sol::state_view L(ts);
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(owner)) {
                auto* active=graph->ActiveSlot(owner);
                return active?sol::make_object(L,active->time):sol::make_object(L,sol::lua_nil);
            }
            const auto* playback = owner == NullEntity
                                 ? nullptr : world.GetComponent<AnimationComponent>(owner);
            return playback && playback->primaryClip != kInvalidAnimHandle
                 ? sol::make_object(L, playback->primaryTime)
                 : sol::make_object(L, sol::lua_nil);
        });

    animator.set_function("SetTime",
        [&world, &animationClips](LuaEntity entityId, float seconds) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(owner)) {
                auto* active=graph->ActiveSlot(owner);
                if(!active)return false;active->time=std::max(0.f,seconds);active->previousTime=active->time;return true;
            }
            auto* playback = owner == NullEntity
                           ? nullptr : world.GetComponent<AnimationComponent>(owner);
            if (!playback || playback->primaryClip == kInvalidAnimHandle) return false;
            playback->primaryTime = std::max(0.f, seconds);
            playback->prevNotifyTime = -1.f;
            return true;
        });

    animator.set_function("GetDuration",
        [&world, &animationClips](sol::this_state ts, LuaEntity entityId) -> sol::object
        {
            sol::state_view L(ts);
            const float duration = animationClips.GetAnimatorDuration(world, LuaId::ResolveLive(world, entityId));
            return duration > 0.f ? sol::make_object(L, duration)
                                  : sol::make_object(L, sol::lua_nil);
        });

    animator.set_function("GetNormalizedTime",
        [&world, &animationClips](sol::this_state ts,
                                  LuaEntity entityId) -> sol::object
        {
            sol::state_view L(ts);
            const Entity owner = ResolveAnimatorOwner(
                world, entityId);
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(owner)) {
                auto* active=graph->ActiveSlot(owner);
                const float duration=active?graph->ClipDuration(owner,active->desc.alias):0;return duration>0?sol::make_object(L,active->time/duration):sol::make_object(L,sol::lua_nil);
            }
            const auto* playback = owner == NullEntity
                                 ? nullptr : world.GetComponent<AnimationComponent>(owner);
            const float duration = animationClips.GetAnimatorDuration(world, LuaId::ResolveLive(world, entityId));
            if (!playback || playback->primaryClip == kInvalidAnimHandle || duration <= 0.f)
                return sol::make_object(L, sol::lua_nil);
            return sol::make_object(L, playback->primaryTime / duration);
        });

    animator.set_function("SetNormalizedTime",
        [&world, &animationClips](LuaEntity entityId, float normalizedTime) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(
                world, entityId);
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(owner)) {
                auto* active=graph->ActiveSlot(owner);
                if(!active)return false;active->time=std::max(0.f,normalizedTime)*graph->ClipDuration(owner,active->desc.alias);active->previousTime=active->time;return true;
            }
            auto* playback = owner == NullEntity
                           ? nullptr : world.GetComponent<AnimationComponent>(owner);
            const float duration = animationClips.GetAnimatorDuration(world, LuaId::ResolveLive(world, entityId));
            if (!playback || playback->primaryClip == kInvalidAnimHandle || duration <= 0.f)
                return false;
            playback->primaryTime = (std::max)(0.f, normalizedTime) * duration;
            playback->prevNotifyTime = -1.f;
            return true;
        });

    animator.set_function("IsBlending",
        [&world,&animationClips](LuaEntity entityId) -> bool
        {
            const Entity owner = ResolveAnimatorOwner(world, entityId);
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(owner))if(auto* active=graph->ActiveSlot(owner))return active->weight<1 || active->status==Anim::Graph::SlotStatus::BlendingOut;
            const auto* transition = owner == NullEntity
                                   ? nullptr : world.GetComponent<AnimatorTransitionComponent>(owner);
            return transition && transition->active;
        });

    lua["Animator"] = animator;

    // ---- Locomotion.* ----
    // Every function takes the CHARACTER ROOT and resolves the animated entity itself.
    sol::table loco = lua.create_table();

    // Locomotion.Setup(e, {
    //     mode = "2d",            -- "1d" | "2d"   (default keeps current)
    //     damping = 6, playRate = 1, enabled = true,
    //     samples = { { path = "asset/...ianim", speed = 0.95, angle = -45 }, ... },
    // })  -> true on success
    loco.set_function("Setup",
        [&world](LuaEntity entityId, sol::optional<sol::table> optsObj) -> bool
        {
            const Entity root = LuaId::ResolveLive(world, entityId);
            const Entity e    = ResolveAnimEntity(world, root);
            if (e == NullEntity) return false;
            world.EnsurePool<LocomotionComponent>();
            auto* lc = world.GetComponent<LocomotionComponent>(e);
            if (!lc)
            {
                world.AddComponent<LocomotionComponent>(e, {});
                lc = world.GetComponent<LocomotionComponent>(e);
                if (!lc) return false;
            }
            if (!optsObj) return true;
            sol::table o = *optsObj;

            if (sol::object m = o["mode"]; m.is<std::string>())
            {
                const std::string s = m.as<std::string>();
                lc->mode = static_cast<uint8_t>((s == "2d" || s == "2D")
                    ? LocomotionMode::Blend2D : LocomotionMode::Blend1D);
            }
            lc->enabled  = o.get_or("enabled",  lc->enabled);
            lc->damping  = o.get_or("damping",  lc->damping);
            lc->playRate = o.get_or("playRate", lc->playRate);

            if (sol::object arr = o["samples"]; arr.is<sol::table>())
            {
                sol::table at = arr.as<sol::table>();
                int n = 0;
                for (int i = 1; i <= static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++i)
                {
                    sol::object row = at[i];
                    if (!row.is<sol::table>()) break;
                    sol::table r = row.as<sol::table>();
                    LocomotionSample s{};
                    s.animPath = r.get_or("path", std::string{});
                    s.speed    = r.get_or("speed", 0.f);
                    s.angleDeg = r.get_or("angle", 0.f);
                    lc->samples[n++] = std::move(s);   // clip/pending reset by ctor
                }
                lc->sampleCount = n;
            }
            return true;
        });

    // Manual parameter drive, for characters with no CharacterController (the AnimDriver bridge is the
    // normal writer and overwrites these). When an AnimGraph owns the pose, writes its Speed / Direction
    // parameters instead.
    loco.set_function("SetParams",
        [&world, &animationClips](LuaEntity entityId, float speed, sol::optional<float> dirDeg) -> bool
        {
            const Entity e = ResolveAnimEntity(world, LuaId::ResolveLive(world, entityId));
            if (auto* graph=animationClips.GraphSystem(); graph && graph->OwnsPose(e)) {
                bool changed=graph->Set(world,e,"Speed",Anim::Graph::ParamValue::FromFloat(speed));
                if(dirDeg) changed=graph->Set(world,e,"Direction",Anim::Graph::ParamValue::FromFloat(*dirDeg)) || changed;
                return changed;
            }
            auto* lc = e == NullEntity ? nullptr : world.GetComponent<LocomotionComponent>(e);
            if (!lc) return false;
            lc->speed = speed;
            if (dirDeg) lc->dirDeg = *dirDeg;
            return true;
        });

    loco.set_function("SetEnabled",
        [&world](LuaEntity entityId, bool on) -> bool
        {
            const Entity e = ResolveAnimEntity(world, LuaId::ResolveLive(world, entityId));
            auto* lc = e == NullEntity ? nullptr : world.GetComponent<LocomotionComponent>(e);
            if (!lc) return false;
            lc->enabled = on;
            return true;
        });

    // 1 = blend space owns the pose, 0 = the Animator's clip does. The driver
    // ramps this automatically for actions; set it by hand only for bespoke
    // sequences (and remember the driver will ramp it back).
    loco.set_function("SetWeight",
        [&world](LuaEntity entityId, float w) -> bool
        {
            const Entity e = ResolveAnimEntity(world, LuaId::ResolveLive(world, entityId));
            auto* lc = e == NullEntity ? nullptr : world.GetComponent<LocomotionComponent>(e);
            if (!lc) return false;
            lc->locoWeight = std::clamp(w, 0.f, 1.f);
            return true;
        });

    // Locomotion.GetState(e) -> table, or nil when there is no blend space.
    loco.set_function("GetState",
        [&world, &lua, &animationClips](LuaEntity entityId) -> sol::object
        {
            const Entity e = ResolveAnimEntity(world, LuaId::ResolveLive(world, entityId));
            if(auto* graph=animationClips.GraphSystem();graph && graph->OwnsPose(e)) {
                const auto* instance=graph->GetInstance(e);
                for(size_t i=0;i<instance->definition->nodes.size();++i) {
                    const auto& node=instance->definition->nodes[i];
                    if(node.type!=Anim::Graph::NodeType::BlendSpace1D && node.type!=Anim::Graph::NodeType::BlendSpace2D)continue;
                    const auto* space=instance->GetBlendSpace(static_cast<Anim::Graph::NodeIndex>(i));
                    sol::table t=lua.create_table();t["entity"]=static_cast<uint32_t>(e);t["enabled"]=true;
                    t["mode"]=node.type==Anim::Graph::NodeType::BlendSpace2D?"2d":"1d";
                    t["speed"]=space?space->speed:0;t["dir"]=space?space->dirDeg:0;
                    t["smoothedSpeed"]=space?space->smoothedSpeed:0;t["smoothedDir"]=space?space->smoothedDir:0;
                    t["phase"]=space?space->phase:instance->states[i].time;
                    float weight=0;int blends=0,resolved=0;
                    for(const auto& tick:instance->ticks)if(tick.node==i){weight+=tick.weight;if(tick.weight>Anim::Graph::kRelevanceEps)++blends;}
                    for(const auto& sample:node.samples){auto clip=instance->slots.find(sample.slot);if(clip!=instance->slots.end() && clip->second!=Anim::Graph::kInvalidClip)++resolved;}
                    t["weight"]=weight;t["blends"]=blends;t["samples"]=node.samples.size();t["resolved"]=resolved;return t;
                }
            }
            auto* lc = e == NullEntity ? nullptr : world.GetComponent<LocomotionComponent>(e);
            if (!lc) return sol::nil;
            sol::table t = lua.create_table();
            t["entity"]  = static_cast<uint32_t>(e);
            t["enabled"] = lc->enabled;
            t["mode"]    = (lc->mode == static_cast<uint8_t>(LocomotionMode::Blend2D))
                         ? "2d" : "1d";
            t["speed"]   = lc->speed;
            t["dir"]     = lc->dirDeg;
            t["smoothedSpeed"] = lc->smoothedSpeed;
            t["smoothedDir"]   = lc->smoothedDir;
            t["phase"]   = lc->phase;
            t["blends"]  = lc->outCount;
            t["weight"]  = lc->locoWeight;
            t["samples"] = lc->sampleCount;
            int resolved = 0;
            for (int i = 0; i < lc->sampleCount &&
                            i < static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++i)
                if (lc->samples[i].clip != ~0u) ++resolved;
            t["resolved"] = resolved;
            return t;
        });

    // Locomotion.SetupTransitions(e, {
    //     startSpeed = .25, stopSpeed = .10, pivotAngle = 100, turnRate = 120,
    //     blendIn = .15, blendOut = .20, cooldown = .25, releaseLead = .20, enabled = true,
    //     list = { { path = "...", kind = "start", angle = 0, speed = 1, maxDuration = 0, looping = false }, ... },
    // })
    loco.set_function("SetupTransitions",
        [&world](LuaEntity entityId, sol::optional<sol::table> optsObj) -> bool
        {
            const Entity e = ResolveAnimEntity(world, LuaId::ResolveLive(world, entityId));
            if (e == NullEntity) return false;
            world.EnsurePool<LocomotionTransitionComponent>();
            auto* tc = world.GetComponent<LocomotionTransitionComponent>(e);
            if (!tc)
            {
                world.AddComponent<LocomotionTransitionComponent>(e, {});
                tc = world.GetComponent<LocomotionTransitionComponent>(e);
                if (!tc) return false;
            }
            if (!optsObj) return true;
            sol::table o = *optsObj;

            tc->enabled     = o.get_or("enabled",     tc->enabled);
            tc->startSpeed  = o.get_or("startSpeed",  tc->startSpeed);
            tc->stopSpeed   = o.get_or("stopSpeed",   tc->stopSpeed);
            tc->pivotAngle  = o.get_or("pivotAngle",  tc->pivotAngle);
            tc->turnRate    = o.get_or("turnRate",    tc->turnRate);
            tc->blendIn     = o.get_or("blendIn",     tc->blendIn);
            tc->blendOut    = o.get_or("blendOut",    tc->blendOut);
            tc->cooldown    = o.get_or("cooldown",    tc->cooldown);
            tc->releaseLead = o.get_or("releaseLead", tc->releaseLead);

            if (sol::object arr = o["list"]; arr.is<sol::table>())
            {
                sol::table at = arr.as<sol::table>();
                int n = 0;
                for (int i = 1;
                     i <= LocomotionTransitionComponent::MAX_TRANSITIONS; ++i)
                {
                    sol::object row = at[i];
                    if (!row.is<sol::table>()) break;
                    sol::table r = row.as<sol::table>();
                    LocoTransition T{};
                    T.animPath    = r.get_or("path", std::string{});
                    {
                        bool kindOk = false;
                        const std::string rawKind = r.get_or("kind", std::string("start"));
                        T.kind = ParseTransitionKind(rawKind, &kindOk);
                        if (!kindOk)
                            LOG_WARNING("Locomotion.SetupTransitions: unknown kind '%s' "
                                        "(row %d) — defaulting to Start. Valid: start, stop, "
                                        "pivot, turn/turninplace, jumpstart, jumploop, jumpend",
                                        rawKind.c_str(), n);
                    }
                    T.angleDeg    = r.get_or("angle", 0.f);
                    T.speed       = r.get_or("speed", 1.f);
                    T.maxDuration = r.get_or("maxDuration", 0.f);
                    T.looping     = r.get_or("looping", false);
                    tc->transitions[n++] = std::move(T);
                }
                tc->count = n;
            }
            return true;
        });

    // Locomotion.Trigger(e, "pivot", 90): fire a transition by hand. Returns false when nothing of that
    // kind is authored (or its clip is not bound yet), so scripted sequences can fall back.
    loco.set_function("Trigger",
        [&world](LuaEntity entityId, std::string kind,
                 sol::optional<float> angle) -> bool
        {
            const Entity e = ResolveAnimEntity(world, LuaId::ResolveLive(world, entityId));
            auto* tc = e == NullEntity
                     ? nullptr : world.GetComponent<LocomotionTransitionComponent>(e);
            if (!tc || tc->count == 0) return false;
            bool kindOk = false;
            const uint8_t k = ParseTransitionKind(kind, &kindOk);   // lowercases internally
            if (!kindOk)
            {
                LOG_WARNING("Locomotion.Trigger: unknown kind '%s' — defaulting to Start. "
                            "Valid: stop, pivot, turn/turninplace, jumpstart, jumploop, jumpend",
                            kind.c_str());
            }
            const float   a = angle.value_or(0.f);

            int best = -1; float bestErr = 1e9f;
            for (int i = 0; i < tc->count &&
                            i < LocomotionTransitionComponent::MAX_TRANSITIONS; ++i)
            {
                const LocoTransition& T = tc->transitions[i];
                if (T.kind != k || T.clip == ~0u) continue;
                float d = std::fmod(T.angleDeg - a + 540.f, 360.f) - 180.f;
                d = std::fabs(d);
                if (d < bestErr) { bestErr = d; best = i; }
            }
            if (best < 0) return false;
            tc->active    = best;
            tc->time      = 0.f;
            tc->releasing = false;
            return true;
        });

    lua["Locomotion"] = loco;

    LOG_INFO("Character/Animator/Locomotion: registered Lua bindings");
}
