#include "Nav/LuaNavBindings.h"
#include "Nav/NavMeshSystem.h"

#include "ECS/ECS.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaDiag.h"   // WarnOnce

#include <DirectXMath.h>
#include <algorithm>
#include <tuple>
#include <cmath>

namespace Nav
{

void RegisterLuaNavBindings(sol::state& lua, NavMeshSystem& nav, ::World& world,
                            DX12Physics::PhysicsSystem* physics)
{
    sol::table tbl = lua.create_table();

    // Nav.SoupStats() -> { colliders, failed, tris, verts, area, box, sphere, capsule,
    //                      mesh, hull, asset, min = {x,y,z}, max = {x,y,z},
    //                      skipped = { noBody, trigger, moving, layer } }
    // What a bake from the world's colliders would read RIGHT NOW (the editor's Build
    // NavMesh input, BuildSoupFromWorldColliders with the physics shapes) - counted,
    // not baked. `area` is the summed triangle area (m^2), a cheap fingerprint of the
    // geometry beside the counts. nil without a PhysicsSystem.
    tbl.set_function("SoupStats", [&world, physics, &lua]() -> sol::object
    {
        if (!physics) return sol::object(sol::lua_nil);
        TriangleSoup soup;
        SoupStats    st;
        BuildSoupFromWorldColliders(world, soup, physics, &st);
        double area = 0.0;
        DirectX::XMFLOAT3 bmin{ 0.f, 0.f, 0.f }, bmax{ 0.f, 0.f, 0.f };
        if (!soup.positions.empty())
        {
            bmin = bmax = soup.positions.front();
            for (const auto& p : soup.positions)
            {
                bmin = { (std::min)(bmin.x, p.x), (std::min)(bmin.y, p.y), (std::min)(bmin.z, p.z) };
                bmax = { (std::max)(bmax.x, p.x), (std::max)(bmax.y, p.y), (std::max)(bmax.z, p.z) };
            }
        }
        for (size_t i = 0; i + 2 < soup.indices.size(); i += 3)
        {
            const auto& a = soup.positions[soup.indices[i]];
            const auto& b = soup.positions[soup.indices[i + 1]];
            const auto& c = soup.positions[soup.indices[i + 2]];
            const double ux = double(b.x) - a.x, uy = double(b.y) - a.y, uz = double(b.z) - a.z;
            const double vx = double(c.x) - a.x, vy = double(c.y) - a.y, vz = double(c.z) - a.z;
            const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
        sol::table t = lua.create_table();
        t["colliders"] = st.colliders;
        t["failed"]    = st.failed;
        t["tris"]      = static_cast<uint32_t>(soup.indices.size() / 3);
        t["verts"]     = static_cast<uint32_t>(soup.positions.size());
        t["area"]      = area;
        static const char* kShapeNames[6] = { "box", "sphere", "capsule", "mesh", "hull", "asset" };
        for (int s = 0; s < 6; ++s) t[kShapeNames[s]] = st.byShape[s];
        // What the soup leaves out (not ground: see BuildSoupFromWorldColliders).
        sol::table skipped = lua.create_table();
        skipped["noBody"]  = st.noBody;
        skipped["trigger"] = st.trigger;
        skipped["moving"]  = st.moving;
        skipped["layer"]   = st.layer;
        t["skipped"] = skipped;
        auto vec = [&lua](const DirectX::XMFLOAT3& v)
        {
            sol::table o = lua.create_table();
            o["x"] = v.x; o["y"] = v.y; o["z"] = v.z;
            return o;
        };
        t["min"] = vec(bmin);
        t["max"] = vec(bmax);
        return sol::make_object(lua, t);
    });

    tbl.set_function("IsReady", [&nav]() -> bool { return nav.IsReady(); });
    tbl.set_function("Load", [&nav](const std::string& path) { return nav.Load(path); });
    tbl.set_function("Save", [&nav](const std::string& path) { return nav.Save(path); });
    // Validated one-based triangle soup for small procedural test arenas.
    tbl.set_function("BuildSurface", [&nav](sol::table vertices, sol::table triangles) {
        TriangleSoup soup;
        if (vertices.size() < 3 || vertices.size() > 65536 ||
            triangles.size() < 3 || triangles.size() > 393216 || triangles.size() % 3) return false;
        for (size_t i = 1; i <= vertices.size(); ++i) {
            sol::object value = vertices[i];
            if (!value.is<sol::table>()) return false;
            sol::table p = value.as<sol::table>();
            if (!p[1].is<float>() || !p[2].is<float>() || !p[3].is<float>()) return false;
            DirectX::XMFLOAT3 v{p[1].get<float>(), p[2].get<float>(), p[3].get<float>()};
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                std::abs(v.x)>10000 || std::abs(v.y)>10000 || std::abs(v.z)>10000) return false;
            soup.positions.push_back(v);
        }
        for (size_t i = 1; i <= triangles.size(); ++i) {
            if (!triangles[i].is<int>()) return false;
            int index = triangles[i].get<int>();
            if (index < 1 || index > static_cast<int>(soup.positions.size())) return false;
            soup.indices.push_back(static_cast<uint32_t>(index-1));
        }
        BuildParams params;
        params.agentRadius = 0.35f;
        params.agentHeight = 1.8f;
        params.cellSize = 0.2f;
        params.cellHeight = 0.1f;
        return nav.Build(soup, params).success;
    });

    // Returns (waypoints, reason): waypoints an array of {x,y,z}; reason nil on success, else
    // "not-ready" / "unreachable" / "partial". The reason tells "never baked", "off-mesh / no route" and
    // a path Detour truncated (kMaxPolys / kMaxStraight = 256) apart: all three would otherwise look
    // like an empty or complete array.
    tbl.set_function("FindPath",
        [&nav, &lua](float fx, float fy, float fz, float tx, float ty, float tz)
            -> std::tuple<sol::table, sol::object>
        {
            sol::table out = lua.create_table();
            if (!nav.IsReady())
            {
                static constexpr char kSite[] = "Nav.FindPath/not-ready";
                LuaDiag::WarnOnce(kSite, "Nav.FindPath: no navmesh loaded "
                                         "(scene has no .inav, or it failed to load)");
                return { out, sol::make_object(lua, "not-ready") };
            }
            PathResult pr = nav.FindPath({ fx, fy, fz }, { tx, ty, tz });
            if (!pr.valid)
                return { out, sol::make_object(lua, "unreachable") };
            int idx = 1;
            for (const auto& p : pr.waypoints)
            {
                sol::table wp = lua.create_table();
                wp["x"] = p.x; wp["y"] = p.y; wp["z"] = p.z;
                out[idx++] = wp;
            }
            return { out, pr.partial ? sol::make_object(lua, "partial")
                                     : sol::object(sol::lua_nil) };
        });

    // Returns clear, hitX, hitY, hitZ, reason. reason is nil when the result is meaningful,
    // "not-ready" when there is no navmesh (else indistinguishable from "blocked at the target").
    tbl.set_function("Raycast",
        [&nav, &lua](float fx, float fy, float fz, float tx, float ty, float tz)
            -> std::tuple<bool, float, float, float, sol::object>
        {
            if (!nav.IsReady())
            {
                static constexpr char kSite[] = "Nav.Raycast/not-ready";
                LuaDiag::WarnOnce(kSite, "Nav.Raycast: no navmesh loaded");
                return { false, tx, ty, tz, sol::make_object(lua, "not-ready") };
            }
            DirectX::XMFLOAT3 hit{ tx, ty, tz };
            const bool clear = nav.Raycast({ fx, fy, fz }, { tx, ty, tz }, &hit);
            return { clear, hit.x, hit.y, hit.z, sol::object(sol::lua_nil) };
        });

    tbl.set_function("Project",
        [&nav, &lua](float x, float y, float z)
            -> std::tuple<bool, float, float, float, sol::object>
        {
            // Guard first: without a navmesh ProjectToMesh would report a value-initialised (0,0,0), which
            // reads as "the point projects to the origin".
            if (!nav.IsReady())
            {
                static constexpr char kSite[] = "Nav.Project/not-ready";
                LuaDiag::WarnOnce(kSite, "Nav.Project: no navmesh loaded");
                return { false, x, y, z, sol::make_object(lua, "not-ready") };
            }
            DirectX::XMFLOAT3 onMesh{};
            const bool ok = nav.ProjectToMesh({ x, y, z }, onMesh);
            return { ok, onMesh.x, onMesh.y, onMesh.z,
                     ok ? sol::object(sol::lua_nil) : sol::make_object(lua, "off-mesh") };
        });

    lua["Nav"] = tbl;
    LOG_INFO("Nav: registered Lua bindings");
}

} // namespace Nav
