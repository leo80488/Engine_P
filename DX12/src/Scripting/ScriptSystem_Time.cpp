// ScriptSystem_Time.cpp - the game time scale and one entity's own time: Engine.SetTimeScale's
// legacy factor, the Sequencer's factor and Time.RequestScale's requests composed into the scale
// FrameContext::scaledDeltaTime carries, and Time.RequestEntityScale / Time.SetRealTime, which
// TimeDilationComponent carries to the animation chain and HitboxSystem.

#include "Scripting/ScriptSystem.h"
#include "Scripting/LuaDoc.h"         // bindings described as they are bound
#include <sol/sol.hpp>
#include "Scripting/LuaEntityId.h"    // LuaEntity + LuaId::* (needs sol first)
#include "ECS/AnimEntityResolve.h"    // a request reaches the entity's animated descendant
#include "Gameplay/GameplayTime.h"    // TimeDilationComponent

#include <algorithm>
#include <unordered_map>
#include <vector>

// ==== Game time scale ====
void ScriptSystem::SetTimeScale(float s)
{
    m_legacyTimeScale = s < 0.f ? 0.f : s;
    RefreshTimeScale();
}

void ScriptSystem::SetSequencerTimeScale(float s)
{
    m_sequencerTimeScale = s < 0.f ? 0.f : s;
    RefreshTimeScale();
}

void ScriptSystem::RefreshTimeScale()
{
    float slowest = m_sequencerTimeScale;
    for (const TimeRequest& r : m_timeRequests) slowest = std::min(slowest, r.scale);
    m_timeScale = m_legacyTimeScale * slowest;
}

void ScriptSystem::TickTimeRequests(float realDt)
{
    bool expired = false;
    for (auto it = m_timeRequests.begin(); it != m_timeRequests.end();)
    {
        if (it->remaining >= 0.f)
        {
            it->remaining -= realDt;
            if (it->remaining <= 0.f) { it = m_timeRequests.erase(it); expired = true; continue; }
        }
        ++it;
    }
    if (expired) RefreshTimeScale();

    if (m_entityTimeRequests.empty() && !m_entityTimeDirty) return;
    // A request on an entity that died since goes too: nothing else would release one held until
    // released.
    const World* world = m_world ? m_world : m_boundWorld;
    bool entityExpired = m_entityTimeDirty;
    for (auto it = m_entityTimeRequests.begin(); it != m_entityTimeRequests.end();)
    {
        const bool dead = world && (!world->IsAlive(it->entity)
                                    || world->GetGeneration(it->entity) != it->generation);
        bool timedOut = false;
        if (it->remaining >= 0.f)
        {
            it->remaining -= realDt;
            timedOut = it->remaining <= 0.f;
        }
        if (dead || timedOut)
        {
            it = m_entityTimeRequests.erase(it);
            entityExpired = true;
            continue;
        }
        ++it;
    }
    if (entityExpired) RefreshEntityTimeScales();
}

// ==== One entity's time ====
void ScriptSystem::RefreshEntityTimeScales()
{
    World* world = m_world ? m_world : m_boundWorld;
    if (!world) { m_entityTimeDirty = true; return; }
    m_entityTimeDirty = false;

    std::erase_if(m_entityTimeRequests, [world](const EntityTimeRequest& r)
    {
        return !world->IsAlive(r.entity) || world->GetGeneration(r.entity) != r.generation;
    });

    // The slowest request per entity, on the named entity and on the one its clock lives on.
    std::unordered_map<Entity, float> slowest;
    for (const EntityTimeRequest& r : m_entityTimeRequests)
    {
        const Entity targets[2] = { r.entity, Anim::ResolveAnimEntityOrSelf(*world, r.entity) };
        for (const Entity t : targets)
        {
            auto [it, inserted] = slowest.try_emplace(t, r.scale);
            if (!inserted) it->second = (std::min)(it->second, r.scale);
        }
    }

    // Rewrite every existing component (back to 1 without a request); a component neither slowed
    // nor on real time is removed, so the pools stay as small as the requests.
    std::vector<Entity> backToNormal;
    if (auto* pool = world->GetPool<TimeDilationComponent>())
    {
        const auto& entities = EntitiesOf(pool);
        auto& data = pool->Data();
        for (std::size_t i = 0; i < data.size() && i < entities.size(); ++i)
        {
            auto found = slowest.find(entities[i]);
            data[i].scale = found != slowest.end() ? found->second : 1.f;
            if (found != slowest.end()) slowest.erase(found);
            else if (!data[i].realTime) backToNormal.push_back(entities[i]);
        }
    }
    for (const Entity e : backToNormal) world->RemoveComponent<TimeDilationComponent>(e);
    for (const auto& [e, scale] : slowest)
        world->AddComponent<TimeDilationComponent>(e, TimeDilationComponent{ scale, false });
}

void ScriptSystem::ApplyRealTime(World& world, Entity e, bool realTime)
{
    const Entity targets[2] = { e, Anim::ResolveAnimEntityOrSelf(world, e) };
    for (const Entity t : targets)
    {
        if (TimeDilationComponent* dilation = world.GetComponent<TimeDilationComponent>(t))
            dilation->realTime = realTime;
        else if (realTime)
            world.AddComponent<TimeDilationComponent>(t, TimeDilationComponent{ 1.f, true });
    }
    // Turning real time off can leave a component with nothing to do: the refresh removes it.
    RefreshEntityTimeScales();
}

// ==== Lua: Time.* ====
void ScriptSystem::RegisterTimeBindings()
{
    sol::table timeTable = (*m_lua)["Time"];
    LuaDoc::Fn(timeTable, "Time", "RequestScale",
        [this](float scale, sol::optional<float> realSeconds) -> std::uint64_t
        {
            const float s = scale < 0.f ? 0.f : scale;
            // No realSeconds = until released; 0 (or less) = released at the next tick.
            const float r = realSeconds ? (*realSeconds > 0.f ? *realSeconds : 0.f) : -1.f;
            const std::uint64_t id = ++m_nextTimeRequestId;
            m_timeRequests.push_back(TimeRequest{ id, m_currentOwner, s, r });
            RefreshTimeScale();
            return id;
        },
        { .doc = "Slow (or stop) game time until released, or for realSeconds of real time. The strongest slow-down among all requests wins, so a pause and a hit-stop compose instead of overwriting each other; a value above 1 has no effect. Released with the script that made it (its scene, System or entity).",
          .params = { { .name = "scale", .doc = "0 = frozen, 1 = normal" },
                      { .name = "realSeconds", .doc = "auto-release after this much real time (0 = at the next tick); omitted = until released" } },
          .returnsDoc = "handle for Time.ReleaseScale", .effect = LuaDoc::Effect::World });
    LuaDoc::Fn(timeTable, "Time", "ReleaseScale",
        [this](std::uint64_t handle) -> bool
        {
            for (auto it = m_timeRequests.begin(); it != m_timeRequests.end(); ++it)
                if (it->id == handle)
                {
                    m_timeRequests.erase(it);
                    RefreshTimeScale();
                    return true;
                }
            return false;
        },
        { .doc = "Release a Time.RequestScale request; false when it already expired or was released.",
          .params = { { .name = "handle" } }, .effect = LuaDoc::Effect::World });
    LuaDoc::Fn(timeTable, "Time", "Scale",
        [this]() -> float { return GetTimeScale(); },
        { .doc = "The script-side game time scale: Engine.SetTimeScale's factor times the strongest request or sequencer slow-down. A GameFlow state's time_scale multiplies on top of it." });

    // ---- One entity's time (TimeDilationComponent) ----
    LuaDoc::Fn(timeTable, "Time", "RequestEntityScale",
        [this](LuaEntity entity, float scale, sol::optional<float> realSeconds) -> std::uint64_t
        {
            if (!m_world) return 0ull;
            const Entity e = LuaId::ResolveLive(*m_world, entity);
            if (e == NullEntity) return 0ull;
            const float s = scale < 0.f ? 0.f : scale;
            // No realSeconds = until released; 0 (or less) = released at the next tick.
            const float r = realSeconds ? (*realSeconds > 0.f ? *realSeconds : 0.f) : -1.f;
            const std::uint64_t id = ++m_nextTimeRequestId;
            m_entityTimeRequests.push_back(
                EntityTimeRequest{ id, m_currentOwner, e, m_world->GetGeneration(e), s, r });
            RefreshEntityTimeScales();
            return id;
        },
        { .doc = "Slow (or stop) one entity's gameplay clocks on top of the game clock: its animation, attack clock, notifies and hitbox sweeps. Until released, or for realSeconds of real time. The slowest request on the entity wins; it applies to the entity and its animated descendant. Released with the script that made it.",
          .params = { { .name = "entity" },
                      { .name = "scale", .doc = "0 = frozen, 1 = normal" },
                      { .name = "realSeconds", .doc = "auto-release after this much real time (0 = at the next tick); omitted = until released" } },
          .returnsDoc = "handle for Time.ReleaseEntityScale; 0 when the entity is not alive", .effect = LuaDoc::Effect::World });
    LuaDoc::Fn(timeTable, "Time", "ReleaseEntityScale",
        [this](std::uint64_t handle) -> bool
        {
            for (auto it = m_entityTimeRequests.begin(); it != m_entityTimeRequests.end(); ++it)
                if (it->id == handle)
                {
                    m_entityTimeRequests.erase(it);
                    RefreshEntityTimeScales();
                    return true;
                }
            return false;
        },
        { .doc = "Release a Time.RequestEntityScale request; false when it already expired or was released.",
          .params = { { .name = "handle" } }, .effect = LuaDoc::Effect::World });
    LuaDoc::Fn(timeTable, "Time", "SetRealTime",
        [this](LuaEntity entity, bool realTime) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, entity);
            if (e == NullEntity) return false;
            ApplyRealTime(*m_world, e, realTime);
            return true;
        },
        { .doc = "Let an entity's animation keep real time through Time.RequestScale pauses, hit-stops and GameFlow slow-downs (a character shown behind a menu); its own Time.RequestEntityScale requests still apply. Applies to the entity and its animated descendant.",
          .params = { { .name = "entity" }, { .name = "realTime" } },
          .returnsDoc = "false when the entity is not alive", .effect = LuaDoc::Effect::World });
    LuaDoc::Fn(timeTable, "Time", "EntityScale",
        [this](LuaEntity entity) -> float
        {
            if (!m_world) return 1.f;
            const Entity e = LuaId::ResolveLive(*m_world, entity);
            const auto* dilation = e != NullEntity ? m_world->GetComponent<TimeDilationComponent>(e) : nullptr;
            return dilation ? dilation->scale : 1.f;
        },
        { .doc = "The entity's own time scale: its slowest Time.RequestEntityScale request, 1 when none. The game clock (Time.Scale) multiplies on top unless the entity keeps real time.",
          .params = { { .name = "entity" } } });
}
