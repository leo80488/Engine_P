#pragma once

// LuaEntityId: generation-carrying entity ids across the Lua boundary.
// A bare uint32 id cached in a Lua table can silently alias a recycled entity slot (the
// free list reuses ids LIFO, so a stale id most likely names the newest spawned entity).
// C++ uses EntityHandle{entity, generation}; a Lua number carries the same because a
// double holds 53-bit exact integers:
//
//     packed = (generation & 0x1FFFFF) << 32 | entityId      (id 32b + gen 21b)
//
// World generations start at 1 (ECS.h), so every live entity's packed id is > 2^32. A
// packed value with zero generation bits is a raw id (hard-coded 0, or a value that went
// through a gen-stripping store such as the AI blackboard) and is accepted without the
// generation check.
//
// Usage in binding code (include after <sol/sol.hpp>):
//   * Accept entities as `LuaEntity` parameters: sol2 unpacks the number via the
//     customization points below; .raw() is the bare id (low 32 bits).
//   * Gate destructive / component-mutating paths with `LuaId::ResolveLive(world, v)`:
//     NullEntity for dead ids and for stale ids whose slot was recycled.
//   * Return entities as `LuaEntity` via `LuaId::Out(world, e)` so scripts get the
//     generation-stamped form, pushed as a Lua integer (string.format("%d", id) works).

#include "ECS/ECS.h"

#include <sol/sol.hpp>

#include <cstdint>

struct LuaEntity
{
    Entity   id  = NullEntity;
    uint32_t gen = 0;   // 0 = unspecified (raw id): skip the gen check

    constexpr LuaEntity() = default;
    constexpr LuaEntity(Entity inId, uint32_t inGen = 0) : id(inId), gen(inGen) {}

    // The only sanctioned way to get the bare id: logging / formatting (pass this to LOG_*,
    // not the LuaEntity: a class type through printf varargs with %u is conditionally-supported
    // UB), or a caller that already validated liveness. Anything that touches the World uses
    // LuaId::ResolveLive: the bare id has no generation, so a stale value silently addresses
    // whatever entity recycled the slot.
    constexpr Entity raw() const { return id; }

    // Do not add an implicit operator Entity(): static_cast<Entity>(id) would compile, run
    // and silently skip the generation check. World access goes through LuaId::ResolveLive;
    // code that only needs the number says .raw(). Without it generic code (sol2's default
    // __tostring probe, std comparison helpers) cannot treat a LuaEntity as a number either.
};

namespace LuaId
{
    // 21 bits of generation + 32 bits of id = 53 bits, the exact-integer capacity of a
    // double. Generations compare under this mask, so a wrap after 2M destroys of one slot is
    // ambiguous; far beyond any real session.
    constexpr uint32_t kGenMask = 0x1FFFFFu;

    inline LuaEntity Out(const World& w, Entity e)
    {
        if (e == NullEntity) return {};
        return LuaEntity{ e, w.GetGeneration(e) & kGenMask };
    }

    // For contexts where no world may be bound: encodes the raw id (gen 0), which downstream
    // accepts without the generation check.
    inline LuaEntity OutMaybe(const World* w, Entity e)
    {
        return w ? Out(*w, e) : LuaEntity{ e, 0u };
    }

    // Alive AND (when the value carries generation bits) not recycled.
    inline bool IsLive(const World& w, const LuaEntity& v)
    {
        if (v.id == NullEntity || !w.IsAlive(v.id)) return false;
        return v.gen == 0 || v.gen == (w.GetGeneration(v.id) & kGenMask);
    }

    // NullEntity for dead or stale ids; the raw id otherwise.
    inline Entity ResolveLive(const World& w, const LuaEntity& v)
    {
        return IsLive(w, v) ? v.id : NullEntity;
    }
}

// ==== sol2 customization points (ADL) ====
// Accept any Lua number (integer or float subtype: GameState round-trips produce floats);
// reject non-numbers.
template <typename Handler>
bool sol_lua_check(sol::types<LuaEntity>, lua_State* L, int index,
                   Handler&& handler, sol::stack::record& tracking)
{
    return sol::stack::check<double>(L, index, std::forward<Handler>(handler),
                                     tracking);
}

inline LuaEntity sol_lua_get(sol::types<LuaEntity>, lua_State* L, int index,
                             sol::stack::record& tracking)
{
    const double packed = sol::stack::get<double>(L, index, tracking);
    // 0 / negative / NaN / beyond 2^53 -> NullEntity.
    if (!(packed > 0.0) || packed > 9007199254740992.0) return {};
    const uint64_t u = static_cast<uint64_t>(packed);
    return LuaEntity{ static_cast<Entity>(u & 0xFFFFFFFFull),
                      static_cast<uint32_t>((u >> 32) & LuaId::kGenMask) };
}

inline int sol_lua_push(sol::types<LuaEntity>, lua_State* L, const LuaEntity& v)
{
    uint64_t packed = 0;
    if (v.id != NullEntity)
        packed = (static_cast<uint64_t>(v.gen & LuaId::kGenMask) << 32)
               |  static_cast<uint64_t>(v.id);
    return sol::stack::push(L, static_cast<lua_Integer>(packed));
}
