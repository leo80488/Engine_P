#pragma once

// LuaNavBindings - exposes Nav.* on the engine's sol::state; the Nav module's RegisterLua calls it once.
// No movement here: intent is Intent.MoveTo / Intent.LookAt / Intent.Stop (Intent/LuaIntentBindings.cpp).
//   Nav.IsReady() / Nav.Load(path) / Nav.Save(path)    -> bool
//   Nav.BuildSurface(vertices, triangles)              -> bool  (one-based soup, small test arenas)
//   Nav.FindPath(fromX,fromY,fromZ, toX,toY,toZ)       -> { {x,y,z}, ... }, reason ("unreachable" / "partial")
//   Nav.Raycast (fromX,fromY,fromZ, toX,toY,toZ)       -> clear, hitX, hitY, hitZ, reason
//   Nav.Project (worldX,worldY,worldZ)                  -> ok, x, y, z, reason ("off-mesh")
//   Nav.SoupStats()                                     -> { colliders, failed, tris, verts, area,
//                                                            box..asset, min, max, skipped }  (needs `physics`)
// A query's reason is nil on success, "not-ready" without a navmesh, else the one named.

namespace sol { class state; }
class World;   // global ::World
namespace DX12Physics { class PhysicsSystem; }

namespace Nav
{
    class NavMeshSystem;

    void RegisterLuaNavBindings(sol::state& lua, NavMeshSystem& nav, ::World& world,
                                DX12Physics::PhysicsSystem* physics = nullptr);
}
