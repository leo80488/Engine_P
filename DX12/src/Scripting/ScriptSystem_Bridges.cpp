// ScriptSystem_Bridges.cpp - the table of C++ EventBus events that reach Lua: one row per event
// (its Lua-bus name, how the payload is built, what else the event does), including the contact
// rows' targeted OnEnter / OnExit. Delivery is EventBus's, at Gameplay/EventDelivery's drain points.

#include "Scripting/ScriptSystem.h"
#include "ECS/FollowEvents.h"
#include "ECS/EquipmentEvents.h"
#include "ECS/NotifyTypes.h"
#include "ECS/ProjectileComponent.h"   // ProjectileImpactEvent
#include "Gameplay/Damage.h"           // DamagedEvent / DiedEvent / AttributeChangedEvent
#include "Gameplay/GameFlowState.h"    // StateChangedEvent / StateWillChangeEvent
#include "Physics/PhysicsEvents.h"
#include "Resource/AssetFS.h"          // ScriptSystem_Internal.h
#include "Scene/SceneEvents.h"         // SceneLoaded / SceneLoadFailed
#include "System/AppEvents.h"          // AppFocusChanged
#include "System/EventBus.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"     // LuaEntity + LuaId::* (needs sol first)
#include "ScriptSystem_Internal.h"     // private to src/Scripting, not on the include path
using namespace ScriptInternal;        // GetInstance

#include <optional>

// ==== One bridge ====
// @p fill builds the Lua payload (and may rename the event, or return false to drop it); @p after
// runs once the event is on the Lua bus. @p scoped borrows the bound world and defers
// DestroyEntity around both: the bridges that call into entity scripts run from a drain point
// where m_world is null.
template <typename E, typename Fill, typename After>
void ScriptSystem::AddBridge(const char* luaName, bool scoped, Fill fill, After after)
{
    const auto sub = EventBus::Get().Subscribe<E>(
        [this, luaName, scoped, fill, after](const E& e)
        {
            std::optional<WorldBorrow>   borrow;
            std::optional<DeferDestroys> defer;
            if (scoped)
            {
                borrow.emplace(*this);
                defer.emplace(*this);
            }
            sol::table  payload = m_lua->create_table();
            const char* name    = luaName;
            if (!fill(e, payload, name)) return;
            m_luaBus->Publish(name, payload);
            after(e, payload);
        });
    m_cppBridgeUnsubscribers.push_back([sub] { EventBus::Get().Unsubscribe<E>(sub); });
}

void ScriptSystem::RegisterCppEventBridges()
{
    const auto nothingAfter = [](const auto&, const sol::table&) {};
    const auto worldNow = [this]() -> const World* { return m_world ? m_world : m_boundWorld; };

    // OnEnter(self, other, point, normal) on each body's Logic instance, which makes any Logic
    // script defining OnEnter a trigger volume. A body without its own OnEnter bubbles the event
    // up its Parent chain (FindContactHandlerUpwards), so a prefab can keep its sensor on a child
    // and its script on the root. The normal points from self toward other: the event's
    // (bodyA -> bodyB) for A, negated for B.
    // The events are delivered at a drain point after the step, so a handle may have died or been recycled by
    // then: such a body is not called, and a dead other side reaches the callback as 0.
    const auto fireEnter = [this, worldNow](const ContactBeganEvent& e, const sol::table& point,
                                            const sol::table& normal)
    {
        const World* w = worldNow();
        const auto live = [w](const EntityHandle& h) { return !w || w->IsHandleValid(h); };
        auto fire = [&](const EntityHandle& selfH, const EntityHandle& otherH, bool selfIsB)
        {
            if (!live(selfH)) return;
            const Entity self  = selfH.entity;
            const Entity other = live(otherH) ? otherH.entity : NullEntity;
            const Entity target = FindContactHandlerUpwards(self, "OnEnter");
            if (target == NullEntity) return;
            auto sIt = m_states.find(target);
            if (sIt == m_states.end()) return;
            sol::table selfNormal = normal;
            if (selfIsB)
            {
                selfNormal = m_lua->create_table();
                selfNormal["x"] = -e.normal.x; selfNormal["y"] = -e.normal.y; selfNormal["z"] = -e.normal.z;
            }
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(target), i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnEnter"];
                if (!fn.valid()) continue;
                OwnerScope as(*this, sIt->second.slots[i].owner);
                auto res = fn(inst, LuaId::OutMaybe(m_world, other), point, selfNormal);
                if (!res.valid())
                {
                    sol::error err = res;
                    LOG_ERROR("Lua OnEnter error [%s] entity %u slot %zu: %s",
                              sIt->second.slots[i].path.c_str(), target, i, err.what());
                }
            }
        };
        fire(e.bodyA, e.bodyB, false);
        fire(e.bodyB, e.bodyA, true);
    };

    // OnExit(self, other), mirroring OnEnter ("player left the trigger zone"). Resolution is per
    // callback name: an ancestor defining only OnExit receives exits whose enters went to a
    // child's OnEnter-only script (like Unity's per-message routing); define both handlers on the
    // same entity.
    const auto fireExit = [this, worldNow](const ContactEndedEvent& e)
    {
        const World* w = worldNow();
        const auto live = [w](const EntityHandle& h) { return !w || w->IsHandleValid(h); };
        auto fire = [&](const EntityHandle& selfH, const EntityHandle& otherH)
        {
            if (!live(selfH)) return;
            const Entity self  = selfH.entity;
            const Entity other = live(otherH) ? otherH.entity : NullEntity;
            const Entity target = FindContactHandlerUpwards(self, "OnExit");
            if (target == NullEntity) return;
            auto sIt = m_states.find(target);
            if (sIt == m_states.end()) return;
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(target), i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnExit"];
                if (!fn.valid()) continue;
                OwnerScope as(*this, sIt->second.slots[i].owner);
                auto res = fn(inst, LuaId::OutMaybe(m_world, other));
                if (!res.valid())
                {
                    sol::error err = res;
                    LOG_ERROR("Lua OnExit error [%s] entity %u slot %zu: %s",
                              sIt->second.slots[i].path.c_str(), target, i, err.what());
                }
            }
        };
        fire(e.bodyA, e.bodyB);
        fire(e.bodyB, e.bodyA);
    };

    // ---- The table: one row per C++ event ----

    // Fires while the outgoing state is still fully alive, which StateChanged cannot be: a
    // subscriber that wants to fade out the music it owns has to run before OnExit, not after.
    AddBridge<Gameplay::StateWillChangeEvent>("StateWillChange", false,
        [](const Gameplay::StateWillChangeEvent& e, sol::table& t, const char*&)
        {
            t["from"] = e.fromName;
            t["to"]   = e.toName;
            return true;
        }, nothingAfter);

    // GameFlow.Pause / the menu key took effect or ended, at the start of the frame.
    AddBridge<Gameplay::PauseChangedEvent>("PauseChanged", false,
        [](const Gameplay::PauseChangedEvent& e, sol::table& t, const char*&)
        {
            t["paused"]   = e.paused;
            t["requests"] = e.requests;
            t["reasons"]  = e.reasons;
            return true;
        }, nothingAfter);

    AddBridge<Gameplay::StateChangedEvent>("StateChanged", false,
        [this](const Gameplay::StateChangedEvent& e, sol::table& t, const char*&)
        {
            t["from"] = e.fromName;
            t["to"]   = e.toName;
            // Forward the exited / entered state lists the LCA walk computed, so subscribers
            // need not recompute a hierarchy they do not own.
            sol::table exited = m_lua->create_table();
            for (size_t i = 0; i < e.exited.size(); ++i) exited[i + 1] = e.exited[i];
            t["exited"] = exited;
            sol::table entered = m_lua->create_table();
            for (size_t i = 0; i < e.entered.size(); ++i) entered[i + 1] = e.entered[i];
            t["entered"] = entered;
            t["trigger"] = e.trigger;
            return true;
        }, nothingAfter);

    // The edge a Loading state waits on, so the flow owns the sequence instead of the loaded
    // scene's script reporting its arrival.
    AddBridge<Scene::SceneLoadedEvent>("SceneLoaded", false,
        [](const Scene::SceneLoadedEvent& e, sol::table& t, const char*&)
        {
            t["name"] = e.name;
            t["path"] = e.path;
            t["ok"]   = e.ok;
            return true;
        }, nothingAfter);

    AddBridge<Scene::SceneLoadFailedEvent>("SceneLoadFailed", false,
        [](const Scene::SceneLoadFailedEvent& e, sol::table& t, const char*&)
        {
            t["name"]   = e.name;
            t["path"]   = e.path;
            t["reason"] = e.reason;
            return true;
        }, nothingAfter);

    // Alt-tab. Auto-pause is a game decision, so the engine only delivers the edge to Lua.
    AddBridge<Sys::AppFocusChangedEvent>("AppFocusChanged", false,
        [](const Sys::AppFocusChangedEvent& e, sol::table& t, const char*&)
        {
            t["focused"] = e.focused;
            return true;
        }, nothingAfter);

    AddBridge<FollowTargetLostEvent>("FollowTargetLost", false,
        [worldNow](const FollowTargetLostEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["follower"]     = LuaId::OutMaybe(w, e.follower);
            t["formerTarget"] = LuaId::OutMaybe(w, e.formerTarget.entity);
            return true;
        }, nothingAfter);

    // Clip-authored custom AnimNotifies (and the engine-fired "clip_end"), delivered two ways:
    // the entity-scoped OnAnimEvent(self, name, payload) (OnAnimGraphEvent when the payload
    // carries graphEvent; anim-graph events also go to the graph root), and a Lua-bus broadcast
    // for System scripts ("AnimNotify", or "AnimGraph" for graph lifecycle events). Queued, not
    // dispatched: the drain runs with m_world null (see AnimEventQueue in ScriptSystem.h).
    AddBridge<CustomNotifyEvent>("AnimNotify", false,
        [worldNow](const CustomNotifyEvent& e, sol::table& t, const char*& name)
        {
            const World* world = worldNow();
            const bool graph = e.params.GetInt("_animGraph", 0) != 0;
            const Entity root = graph
                ? static_cast<Entity>(e.params.GetInt("_animRoot", static_cast<int>(e.entity)))
                : e.entity;
            t["entity"] = LuaId::OutMaybe(world, e.entity);
            t["name"]   = e.typeId.str;
            if (graph)
            {
                t["root"]         = LuaId::OutMaybe(world, root);
                t["weight"]       = e.params.GetFloat("weight");
                t["sourcePath"]   = e.params.GetString("sourcePath");
                t["sourceNode"]   = e.params.GetInt("sourceNode");
                t["activeBranch"] = e.params.GetInt("activeBranch") != 0;
                t["forcedEnd"]    = e.params.GetInt("forcedEnd") != 0;
            }
            if (graph && !e.params.GetString("graphEvent").empty()) name = "AnimGraph";
            return true;
        },
        [this, worldNow](const CustomNotifyEvent& e, const sol::table&)
        {
            const World* world = worldNow();
            const bool graph = e.params.GetInt("_animGraph", 0) != 0;
            const Entity root = graph
                ? static_cast<Entity>(e.params.GetInt("_animRoot", static_cast<int>(e.entity)))
                : e.entity;
            QueueAnimEvent(e.entity, e.typeId.str, &e.params);
            if (graph && root != e.entity && world && world->IsAlive(root))
                QueueAnimEvent(root, e.typeId.str, &e.params);
        });

    // Once, on the frame a projectile is blocked. Carries point and normal so an impact VFX /
    // decal / sound handler need not query a projectile that may already be expiring. hitEntity
    // is 0 when nothing was struck (it ran out of life).
    AddBridge<ProjectileImpactEvent>("ProjectileImpact", false,
        [worldNow](const ProjectileImpactEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["projectile"] = LuaId::OutMaybe(w, e.projectile);
            t["hitEntity"]  = LuaId::OutMaybe(w, e.hitEntity);
            t["x"]  = e.point.x;   t["y"]  = e.point.y;   t["z"]  = e.point.z;
            t["nx"] = e.normal.x;  t["ny"] = e.normal.y;  t["nz"] = e.normal.z;
            return true;
        }, nothingAfter);

    // What the damage pipeline made of a hit (Gameplay/Damage.h), in the same drain as the HitboxHit
    // that caused it: `reason` is "applied" when Health went down, and "unmanaged" when the hit landed on
    // something with no Health; every other reason (immune, blocked, vetoed, dead, same_hit_group,
    // resisted, no_target, no_amount) is a hit that changed nothing, which HitStop and the quests ignore.
    AddBridge<Gameplay::Damage::DamagedEvent>("Damaged", false,
        [worldNow](const Gameplay::Damage::DamagedEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["target"]     = LuaId::OutMaybe(w, e.target);
            t["instigator"] = LuaId::OutMaybe(w, e.instigator);
            t["source"]     = LuaId::OutMaybe(w, e.source);
            t["part"]       = LuaId::OutMaybe(w, e.part);
            t["type"]       = e.type;
            t["requested"]  = e.requested;
            t["applied"]    = e.applied;
            t["overkill"]   = e.overkill;
            t["health"]     = e.health;
            t["reason"]     = Gameplay::Damage::ReasonName(e.reason);
            t["landed"]     = Gameplay::Damage::Landed(e.reason);
            t["detail"]     = e.detail;
            t["hitGroup"]   = e.hitGroup;
            t["fromHit"]    = e.fromHit;
            t["x"] = e.point.x;  t["y"] = e.point.y;  t["z"] = e.point.z;
            t["nx"] = e.normal.x; t["ny"] = e.normal.y; t["nz"] = e.normal.z;
            return true;
        }, nothingAfter);

    // Once per life: Health went from above zero to zero.
    AddBridge<DiedEvent>("Died", false,
        [worldNow](const DiedEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["target"]     = LuaId::OutMaybe(w, e.target);
            t["instigator"] = LuaId::OutMaybe(w, e.instigator);
            t["source"]     = LuaId::OutMaybe(w, e.source);
            t["type"]       = e.type;
            t["overkill"]   = e.overkill;
            t["hitGroup"]   = e.hitGroup;
            return true;
        }, nothingAfter);

    // An attribute's current value moved (Attr.Set, a hit, a profile): what a HUD binds to.
    AddBridge<AttributeChangedEvent>("AttributeChanged", false,
        [worldNow](const AttributeChangedEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["entity"]   = LuaId::OutMaybe(w, e.entity);
            t["name"]     = e.name;
            t["previous"] = e.previous;
            t["current"]  = e.current;
            t["base"]     = e.base;
            return true;
        }, nothingAfter);

    // Deliberately NOT applied to anything: `damage` is a number for a combat system to
    // interpret, and whether the victim actually loses health (blocking, armour) is a gameplay
    // decision the animation pipeline must not make. `result` says what was known at the overlap:
    // "immune" when the victim held State.IFrame or State.Invincible, else "hit". What came of the
    // hit is the "Damaged" event that follows.
    AddBridge<HitboxHitEvent>("HitboxHit", false,
        [worldNow](const HitboxHitEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["attacker"] = LuaId::OutMaybe(w, e.attacker);
            t["source"]   = LuaId::OutMaybe(w, e.source);
            t["victim"]   = LuaId::OutMaybe(w, e.victim);
            t["part"]     = LuaId::OutMaybe(w, e.victimPart);
            t["slot"]     = e.slot;
            t["hitGroup"] = e.hitGroup;
            t["damage"]   = e.damage;
            t["result"]   = e.result == HitResult::Immune ? "immune" : "hit";
            t["x"] = e.point.x;  t["y"] = e.point.y;  t["z"] = e.point.z;
            t["nx"] = e.normal.x; t["ny"] = e.normal.y; t["nz"] = e.normal.z;
            return true;
        }, nothingAfter);

    AddBridge<WeaponEquippedEvent>("WeaponEquipped", false,
        [worldNow](const WeaponEquippedEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["character"]   = LuaId::OutMaybe(w, e.character.entity);
            t["item"]        = LuaId::OutMaybe(w, e.item.entity);
            t["socketIndex"] = e.socketIndex;
            return true;
        }, nothingAfter);

    AddBridge<WeaponUnequippedEvent>("WeaponUnequipped", false,
        [worldNow](const WeaponUnequippedEvent& e, sol::table& t, const char*&)
        {
            const World* w = worldNow();
            t["character"] = LuaId::OutMaybe(w, e.character.entity);
            t["item"]      = LuaId::OutMaybe(w, e.item.entity);
            return true;
        }, nothingAfter);

    // Contacts: the generic broadcast on the Lua bus, then the targeted OnEnter / OnExit. Scoped:
    // that Lua may use world-dependent bindings, and a destroy it asks for waits for the next script
    // phase: the drain at the end of BoneAttachment runs after this frame's animation chain has built
    // its skin jobs, and OnEnter walks m_states, which a synchronous destroy would erase under it.
    // KCC characters leaving a sensor end through the PostAllSteps overlap sweep.
    AddBridge<ContactBeganEvent>("ContactBegan", true,
        [this](const ContactBeganEvent& e, sol::table& t, const char*&)
        {
            sol::table point  = m_lua->create_table();
            point["x"] = e.point.x; point["y"] = e.point.y; point["z"] = e.point.z;
            sol::table normal = m_lua->create_table();
            normal["x"] = e.normal.x; normal["y"] = e.normal.y; normal["z"] = e.normal.z;
            t["bodyA"]  = LuaId::OutMaybe(m_world, e.bodyA.entity);
            t["bodyB"]  = LuaId::OutMaybe(m_world, e.bodyB.entity);
            t["point"]  = point;
            t["normal"] = normal;
            return true;
        },
        [fireEnter](const ContactBeganEvent& e, const sol::table& t)
        {
            const sol::table point  = t["point"];
            const sol::table normal = t["normal"];
            fireEnter(e, point, normal);
        });

    AddBridge<ContactEndedEvent>("ContactEnded", true,
        [this](const ContactEndedEvent& e, sol::table& t, const char*&)
        {
            t["bodyA"] = LuaId::OutMaybe(m_world, e.bodyA.entity);
            t["bodyB"] = LuaId::OutMaybe(m_world, e.bodyB.entity);
            return true;
        },
        [fireExit](const ContactEndedEvent& e, const sol::table&) { fireExit(e); });

    // Input device identity and hotplug edges are not rows: input is a per-frame poll with no
    // C++ event, so Update() publishes "InputDeviceChanged" / "GamepadConnected" /
    // "GamepadDisconnected" itself.
    SeedInputDeviceEdge();
}
