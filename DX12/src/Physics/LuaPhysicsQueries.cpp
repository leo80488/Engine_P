// The Lua queries of Physics.*: Raycast, RayAll, SphereCast, BoxCast, CapsuleCast, OverlapSphere (broad phase),
// OverlapBox and OverlapCapsule (exact). Every one takes the same `filter` argument (LuaPhysicsInternal.h) and
// hands entities back with the generation their body was built for.

#include "LuaPhysicsInternal.h"

#include "Physics/PhysicsSystem.h"
#include "Scripting/LuaDoc.h"
#include "Scripting/LuaMathTypes.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

namespace DX12Physics
{
namespace
{
    const char* const kFilterType = "integer|string|string[]|table";

    // The entity of a result, with the generation its body was built for (0 = no entity), so a body whose entity
    // died this frame still names that entity. -physics_legacy_hit_ids (harness) stamps the generation the id has
    // NOW instead, as every query did: a stranger once the id is recycled.
    LuaEntity HitEntity(const World& world, Entity entity, std::uint32_t generation)
    {
        if (entity == NullEntity) return LuaEntity{};
#if WITH_HARNESS
        static const bool s_legacy = Sys::CommandLine::Get().Has("-physics_legacy_hit_ids");
        if (s_legacy) return LuaId::Out(world, entity);
#endif
        return LuaEntity{ entity, generation & LuaId::kGenMask };
    }

    bool ReadVec3(const sol::object& o, DirectX::XMFLOAT3& out)
    {
        if (o.is<LuaVec3>())
        {
            const LuaVec3& v = o.as<LuaVec3&>();
            out = { v.x, v.y, v.z };
            return true;
        }
        if (o.is<sol::table>())
        {
            const sol::table t = o.as<sol::table>();
            out = { t.get_or("x", 0.f), t.get_or("y", 0.f), t.get_or("z", 0.f) };
            return true;
        }
        return false;
    }

    DirectX::XMFLOAT4 ReadQuat(const sol::object& o)
    {
        if (o.is<LuaQuat>())
        {
            const LuaQuat& q = o.as<LuaQuat&>();
            return { q.x, q.y, q.z, q.w };
        }
        if (o.is<sol::table>())
        {
            const sol::table t = o.as<sol::table>();
            return { t.get_or("x", 0.f), t.get_or("y", 0.f), t.get_or("z", 0.f), t.get_or("w", 1.f) };
        }
        return { 0.f, 0.f, 0.f, 1.f };
    }

    // A field of a query table that must be a vector; false (and a warning) when it is missing.
    bool NeedVec3(const sol::table& opts, const char* key, const char* where, DirectX::XMFLOAT3& out)
    {
        if (ReadVec3(opts[key], out)) return true;
        LOG_WARNING("%s: '%s' must be a Vec3 or {x=,y=,z=}", where, key);
        return false;
    }

    sol::table PointTable(sol::state& lua, const World& world, Entity entity, std::uint32_t generation,
                          const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT3& normal)
    {
        sol::table t = lua.create_table();
        t["entity"] = HitEntity(world, entity, generation);
        t["x"] = point.x;   t["y"] = point.y;   t["z"] = point.z;
        t["nx"] = normal.x; t["ny"] = normal.y; t["nz"] = normal.z;
        return t;
    }

    sol::table OverlapTable(sol::state& lua, const World& world, const std::vector<OverlapHit>& hits)
    {
        sol::table list = lua.create_table(static_cast<int>(hits.size()), 0);
        int n = 0;
        for (const OverlapHit& h : hits)
        {
            sol::table t = PointTable(lua, world, h.entity, h.generation, h.point, h.normal);
            t["depth"] = h.depth;
            list[++n] = t;
        }
        return list;
    }

    sol::object CastTable(sol::state& lua, const World& world, const ShapeCastHit& h)
    {
        if (!h.hit) return sol::make_object(lua, sol::lua_nil);
        sol::table t = PointTable(lua, world, h.entity, h.generation, h.point, h.normal);
        t["distance"] = h.distance;
        t["fraction"] = h.fraction;
        return sol::make_object(lua, t);
    }
}

bool ParseQueryFilter(const sol::object& v, std::uint16_t fallbackMask, const char* where, World& world,
                      QueryFilter& out)
{
    out = QueryFilter{};
    out.layerMask = fallbackMask;
    if (v.is<sol::table>())
    {
        const sol::table t = v.as<sol::table>();
        const sol::object layers = t["layers"], triggers = t["triggers"], ignore = t["ignore"];
        if (layers.valid() || triggers.valid() || ignore.valid())
        {
            if (layers.valid()) out.layerMask = ParseLayerMask(layers, fallbackMask, where);
            if (triggers.is<std::string>())
            {
                const std::string s = triggers.as<std::string>();
                if      (s == "include") out.triggers = TriggerMode::Include;
                else if (s == "exclude") out.triggers = TriggerMode::Exclude;
                else if (s == "only")    out.triggers = TriggerMode::Only;
                else LOG_WARNING("%s: unknown triggers '%s' (include/exclude/only)", where, s.c_str());
            }
            if (ignore.is<LuaEntity>())
                out.ignoreEntity = LuaId::ResolveLive(world, ignore.as<LuaEntity>());
            return true;
        }
    }
    out.layerMask = ParseLayerMask(v, fallbackMask, where);
    return true;
}

void RegisterLuaPhysicsQueries(sol::state& lua, sol::table& physics, PhysicsSystem& ps, World& world)
{
    // Physics.Raycast(fx,fy,fz, dx,dy,dz, maxDist [, filter])
    //   -> hit, hitX,hitY,hitZ, nX,nY,nZ, entityId
    LuaDoc::Fn(physics, "Physics", "Raycast",
        [&ps, &world](float fx, float fy, float fz, float dx, float dy, float dz, float maxDist,
                      sol::object filter)
            -> std::tuple<bool, float, float, float, float, float, float, LuaEntity>
        {
            QueryFilter f;
            ParseQueryFilter(filter, LAYER_MASK_DEFAULT, "Physics.Raycast", world, f);
            const RayHit r = ps.CastRayClosest({ fx, fy, fz }, { dx, dy, dz }, maxDist, f);
            return { r.hit,
                     r.point.x,  r.point.y,  r.point.z,
                     r.normal.x, r.normal.y, r.normal.z,
                     HitEntity(world, r.entity, r.generation) };
        },
        { .doc = "Closest hit along a ray against every body (static, kinematic, dynamic and character query bodies). "
                 "The direction need not be normalised; maxDist alone bounds the ray. The default filter skips the "
                 "query-only layers (HurtBox, AttackBox, Sensing), so a gameplay ray passes through trigger volumes.",
          .params = { { .name = "fx", .doc = "origin x" }, { .name = "fy", .doc = "origin y" },
                      { .name = "fz", .doc = "origin z" }, { .name = "dx", .doc = "direction x" },
                      { .name = "dy", .doc = "direction y" }, { .name = "dz", .doc = "direction z" },
                      { .name = "maxDist", .doc = "ray length in metres" },
                      { .name = "filter", .doc = "a layer mask, a layer name, a list of names, or "
                                                 "{layers=, triggers='include'|'exclude'|'only', ignore=entity}",
                        .type = kFilterType, .optional = true } },
          .returnsDoc = "hit, x, y, z, nx, ny, nz, entity: the point and normal mean something only when hit is true; "
                        "entity is 0 for a body no entity owns, and carries the generation its body was built for" });

    // Physics.RayAll(fx,fy,fz, dx,dy,dz, maxDist [, filter [, maxResults]]) -> { hit, ... }
    LuaDoc::Fn(physics, "Physics", "RayAll",
        [&ps, &world, &lua](float fx, float fy, float fz, float dx, float dy, float dz, float maxDist,
                            sol::object filter, sol::optional<int> maxResults) -> sol::table
        {
            QueryFilter f;
            ParseQueryFilter(filter, LAYER_MASK_DEFAULT, "Physics.RayAll", world, f);
            const std::size_t cap = maxResults && *maxResults > 0 ? static_cast<std::size_t>(*maxResults) : 64u;
            std::vector<RayHit> hits;
            ps.CastRayAll({ fx, fy, fz }, { dx, dy, dz }, maxDist, hits, f, cap);
            sol::table list = lua.create_table(static_cast<int>(hits.size()), 0);
            int n = 0;
            for (const RayHit& h : hits)
            {
                sol::table t = PointTable(lua, world, h.entity, h.generation, h.point, h.normal);
                t["distance"] = h.distance;
                list[++n] = t;
            }
            return list;
        },
        { .doc = "Every body along a ray, nearest first, one hit per body. Same filter as Raycast.",
          .params = { { .name = "fx", .doc = "origin x" }, { .name = "fy", .doc = "origin y" },
                      { .name = "fz", .doc = "origin z" }, { .name = "dx", .doc = "direction x" },
                      { .name = "dy", .doc = "direction y" }, { .name = "dz", .doc = "direction z" },
                      { .name = "maxDist", .doc = "ray length in metres" },
                      { .name = "filter", .doc = "as Physics.Raycast", .type = kFilterType, .optional = true },
                      { .name = "maxResults", .doc = "the nearest N are kept (default 64)" } },
          .returns = "table", .returnsDoc = "array of {entity, x, y, z, nx, ny, nz, distance}, ascending distance" });

    // Physics.SphereCast(fx,fy,fz, dx,dy,dz, radius, maxDist [, filter])
    //   -> hit, hx,hy,hz, nx,ny,nz, distance, fraction, entityId
    LuaDoc::Fn(physics, "Physics", "SphereCast",
        [&ps, &world](float fx, float fy, float fz, float dx, float dy, float dz, float radius, float maxDist,
                      sol::object filter)
            -> std::tuple<bool, float, float, float, float, float, float, float, float, LuaEntity>
        {
            QueryFilter f;
            ParseQueryFilter(filter, LAYER_MASK_DEFAULT, "Physics.SphereCast", world, f);
            const ShapeCastHit h = ps.CastSphereClosest({ fx, fy, fz }, { dx, dy, dz }, (std::max)(0.001f, radius),
                                                        maxDist, f.ignoreBodyId, f.layerMask);
            return { h.hit,
                     h.point.x,  h.point.y,  h.point.z,
                     h.normal.x, h.normal.y, h.normal.z,
                     h.distance, h.fraction,
                     HitEntity(world, h.entity, h.generation) };
        },
        { .doc = "Sweeps a sphere along a ray and reports the closest hit (a swing through two targets reports one: "
                 "use RayAll or an overlap for the rest). Same filter as Raycast.",
          .params = { { .name = "fx", .doc = "start x" }, { .name = "fy", .doc = "start y" },
                      { .name = "fz", .doc = "start z" }, { .name = "dx", .doc = "direction x" },
                      { .name = "dy", .doc = "direction y" }, { .name = "dz", .doc = "direction z" },
                      { .name = "radius", .doc = "sphere radius, at least 0.001" },
                      { .name = "maxDist", .doc = "sweep length in metres" },
                      { .name = "filter", .doc = "as Physics.Raycast", .type = kFilterType, .optional = true } },
          .returnsDoc = "hit, x, y, z, nx, ny, nz, distance, fraction, entity" });

    // Physics.BoxCast{ center=, half=, rotation=, direction=, maxDist=, filter= } -> hit | nil
    LuaDoc::Fn(physics, "Physics", "BoxCast",
        [&ps, &world, &lua](sol::table opts) -> sol::object
        {
            DirectX::XMFLOAT3 center, half, dir;
            if (!NeedVec3(opts, "center", "Physics.BoxCast", center) ||
                !NeedVec3(opts, "half", "Physics.BoxCast", half) ||
                !NeedVec3(opts, "direction", "Physics.BoxCast", dir))
                return sol::make_object(lua, sol::lua_nil);
            QueryFilter f;
            ParseQueryFilter(opts["filter"], LAYER_MASK_DEFAULT, "Physics.BoxCast", world, f);
            const ShapeCastHit h = ps.CastBoxClosest(center, ReadQuat(opts["rotation"]), half, dir,
                                                     opts.get_or("maxDist", 0.f), f);
            return CastTable(lua, world, h);
        },
        { .doc = "Sweeps a box (any orientation) along a direction and reports the closest hit, or nil.",
          .params = { { .name = "opts", .doc = "center, half (half extents), direction, maxDist, rotation (Quat, default "
                                               "identity), filter (as Physics.Raycast)" } },
          .returns = "table?", .returnsDoc = "{entity, x, y, z, nx, ny, nz, distance, fraction} or nil" });

    // Physics.CapsuleCast{ center=, radius=, halfHeight=, rotation=, direction=, maxDist=, filter= } -> hit | nil
    LuaDoc::Fn(physics, "Physics", "CapsuleCast",
        [&ps, &world, &lua](sol::table opts) -> sol::object
        {
            DirectX::XMFLOAT3 center, dir;
            if (!NeedVec3(opts, "center", "Physics.CapsuleCast", center) ||
                !NeedVec3(opts, "direction", "Physics.CapsuleCast", dir))
                return sol::make_object(lua, sol::lua_nil);
            QueryFilter f;
            ParseQueryFilter(opts["filter"], LAYER_MASK_DEFAULT, "Physics.CapsuleCast", world, f);
            const ShapeCastHit h = ps.CastCapsuleClosest(center, ReadQuat(opts["rotation"]), dir,
                                                         opts.get_or("radius", 0.5f), opts.get_or("halfHeight", 0.f),
                                                         opts.get_or("maxDist", 0.f), f);
            return CastTable(lua, world, h);
        },
        { .doc = "Sweeps a capsule whose segment runs along the rotated local Y axis, and reports the closest hit, or "
                 "nil. halfHeight is the half length of the segment (0 makes a sphere).",
          .params = { { .name = "opts", .doc = "center, radius, halfHeight, direction, maxDist, rotation (Quat, default "
                                               "identity), filter (as Physics.Raycast)" } },
          .returns = "table?", .returnsDoc = "{entity, x, y, z, nx, ny, nz, distance, fraction} or nil" });

    // Physics.OverlapSphere(x, y, z, radius [, filter [, maxResults]]) -> { entityId, ... }
    LuaDoc::Fn(physics, "Physics", "OverlapSphere",
        [&ps, &world, &lua](float x, float y, float z, float radius, sol::object filter,
                            sol::optional<int> maxResults) -> sol::table
        {
            QueryFilter f;
            ParseQueryFilter(filter, MakeLayerMask(Layer::Character), "Physics.OverlapSphere", world, f);
            const std::size_t cap = maxResults && *maxResults > 0 ? static_cast<std::size_t>(*maxResults) : 256u;
            std::vector<EntityHandle> hits;
            hits.reserve(cap < 64u ? cap : 64u);
            ps.OverlapSphere({ x, y, z }, radius, hits, f, cap);
            sol::table t = lua.create_table(static_cast<int>(hits.size()), 0);
            int n = 0;
            for (const EntityHandle& h : hits) t[++n] = HitEntity(world, h.entity, h.generation);
            return t;
        },
        { .doc = "The bodies near a point, for perception. BROAD PHASE: it tests body AABBs, so it returns bodies slightly "
                 "outside the radius - a candidate set; re-check the distance, or use OverlapCapsule (halfHeight 0) for "
                 "an exact answer. The default layer is Character, not the gameplay default.",
          .params = { { .name = "x", .doc = "centre x" }, { .name = "y", .doc = "centre y" },
                      { .name = "z", .doc = "centre z" }, { .name = "radius", .doc = "sphere radius" },
                      { .name = "filter", .doc = "as Physics.Raycast", .type = kFilterType, .optional = true },
                      { .name = "maxResults", .doc = "default 256" } },
          .returns = "EntityId[]", .returnsDoc = "entities in body order, not by distance" });

    // Physics.OverlapBox{ center=, half=, rotation=, filter=, maxResults= } -> { hit, ... }
    LuaDoc::Fn(physics, "Physics", "OverlapBox",
        [&ps, &world, &lua](sol::table opts) -> sol::table
        {
            DirectX::XMFLOAT3 center, half;
            if (!NeedVec3(opts, "center", "Physics.OverlapBox", center) ||
                !NeedVec3(opts, "half", "Physics.OverlapBox", half))
                return lua.create_table();
            QueryFilter f;
            ParseQueryFilter(opts["filter"], LAYER_MASK_DEFAULT, "Physics.OverlapBox", world, f);
            const int cap = opts.get_or("maxResults", 64);
            std::vector<OverlapHit> hits;
            ps.OverlapBoxAll(center, ReadQuat(opts["rotation"]), half, hits, f,
                             cap > 0 ? static_cast<std::size_t>(cap) : 64u);
            return OverlapTable(lua, world, hits);
        },
        { .doc = "The bodies a box really overlaps (exact, no AABB false positives), one entry per entity, with the "
                 "deepest contact: the point on the other body, a normal pointing back at the box, and the depth.",
          .params = { { .name = "opts", .doc = "center, half (half extents), rotation (Quat, default identity), filter "
                                               "(as Physics.Raycast), maxResults (default 64)" } },
          .returns = "table", .returnsDoc = "array of {entity, x, y, z, nx, ny, nz, depth}" });

    // Physics.OverlapCapsule{ center=, radius=, halfHeight=, rotation=, filter=, maxResults= } -> { hit, ... }
    LuaDoc::Fn(physics, "Physics", "OverlapCapsule",
        [&ps, &world, &lua](sol::table opts) -> sol::table
        {
            DirectX::XMFLOAT3 center;
            if (!NeedVec3(opts, "center", "Physics.OverlapCapsule", center)) return lua.create_table();
            QueryFilter f;
            ParseQueryFilter(opts["filter"], LAYER_MASK_DEFAULT, "Physics.OverlapCapsule", world, f);
            const int cap = opts.get_or("maxResults", 64);
            std::vector<OverlapHit> hits;
            ps.OverlapCapsuleAll(center, ReadQuat(opts["rotation"]), opts.get_or("radius", 0.5f),
                                 opts.get_or("halfHeight", 0.f), hits, f,
                                 cap > 0 ? static_cast<std::size_t>(cap) : 64u);
            return OverlapTable(lua, world, hits);
        },
        { .doc = "The bodies a capsule (or a sphere, with halfHeight 0) really overlaps: exact, one entry per entity, "
                 "with the deepest contact. The segment runs along the rotated local Y axis.",
          .params = { { .name = "opts", .doc = "center, radius, halfHeight, rotation (Quat, default identity), filter "
                                               "(as Physics.Raycast), maxResults (default 64)" } },
          .returns = "table", .returnsDoc = "array of {entity, x, y, z, nx, ny, nz, depth}" });
}

} // namespace DX12Physics
