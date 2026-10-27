#include "PostProcess/LuaPostProcessBindings.h"

#include "PostProcess/ProfileSystem.h"
#include "PostProcess/PostProcessRuntime.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include <string>
#include <algorithm>

using namespace DirectX;

namespace PostProcess
{
namespace
{
    // ---- "group.member" property-path dispatch (X-macro driven) ----
    // Set a property on a profile by path. float3 paths take a scalar applied to all
    // three components (enough for the Lua intent layer).
    bool SetProfileProperty(PostProcessProfile& p, const std::string& path, double value)
    {
#define PP_BOOL(g, m, l, d)            if (path == #g "." #m) { p.g.m.Set(value != 0.0); return true; }
// The X-macro carries each property's authored min / max; clamp to them as the
// editor sliders do, so a script cannot inject out-of-range values (an inverted
// log-luma range, a negative weight) into the profile.
#define PP_FLOAT(g, m, l, d, a, b)     if (path == #g "." #m) { p.g.m.Set(std::clamp(static_cast<float>(value), (a), (b))); return true; }
#define PP_FLOAT3(g, m, l, dx, dy, dz) if (path == #g "." #m) { float f = static_cast<float>(value); p.g.m.Set(XMFLOAT3{f, f, f}); return true; }
#define PP_COLOR(g, m, l, dr, dg, db)  if (path == #g "." #m) { float f = static_cast<float>(value); p.g.m.Set(XMFLOAT3{f, f, f}); return true; }
// Clamped as a double before the narrowing cast - a negative Lua number cast
// straight to uint32_t would wrap to a huge value instead of clamping to min.
#define PP_UINT(g, m, l, d, a, b)      if (path == #g "." #m) { p.g.m.Set(static_cast<uint32_t>(std::clamp(value, static_cast<double>(a), static_cast<double>(b)))); return true; }
#include "PostProcess/PostProcessProperties.inl"
        return false;
    }

    // Read a resolved scalar by path (bool -> 0/1, float3 -> .x). found = false if
    // no property matches.
    double GetResolvedValue(const ResolvedPostProcessSettings& r,
                            const std::string& path, bool& found)
    {
        found = true;
#define PP_BOOL(g, m, l, d)            if (path == #g "." #m) return r.g.m ? 1.0 : 0.0;
#define PP_FLOAT(g, m, l, d, a, b)     if (path == #g "." #m) return static_cast<double>(r.g.m);
#define PP_FLOAT3(g, m, l, dx, dy, dz) if (path == #g "." #m) return static_cast<double>(r.g.m.x);
#define PP_COLOR(g, m, l, dr, dg, db)  if (path == #g "." #m) return static_cast<double>(r.g.m.x);
#define PP_UINT(g, m, l, d, a, b)      if (path == #g "." #m) return static_cast<double>(r.g.m);
#include "PostProcess/PostProcessProperties.inl"
        found = false;
        return 0.0;
    }
}

void RegisterLuaPostProcessBindings(sol::state& lua, World& world)
{
    sol::table tbl = lua.create_named_table("PostProcess");

    // PostProcess.spawnVolume{ profile=path, shape="box"/"sphere"/"global",
    //   priority=, blendWeight=, blendDistance=, x=,y=,z=, sx=,sy=,sz= }
    // Creates an entity carrying a PostProcessVolumeComponent + Transform.
    // Returns the entity id.
    tbl.set_function("spawnVolume",
        [&world](sol::table opts) -> LuaEntity
        {
            Entity e = world.CreateEntity();

            ECS::PostProcessVolumeComponent vc;
            const std::string shape = opts.get_or<std::string>("shape", "box");
            if (shape == "global")      vc.isGlobal = true;
            else if (shape == "sphere") vc.shape = ECS::PPVolumeShape::Sphere;
            else                        vc.shape = ECS::PPVolumeShape::Box;

            vc.priority      = opts.get_or("priority", 0.0f);
            vc.blendWeight   = opts.get_or("blendWeight", 1.0f);
            vc.blendDistance = opts.get_or("blendDistance", 1.0f);

            vc.profilePath = opts.get_or<std::string>("profile", "");
            if (!vc.profilePath.empty())
                vc.profile = ProfileSystem::Get().Acquire(vc.profilePath);
            else
                vc.profile = ProfileSystem::Get().CreateRuntime();

            world.EnsurePool<ECS::PostProcessVolumeComponent>();
            world.AddComponent<ECS::PostProcessVolumeComponent>(e, vc);

            // Transform (bounds source). GlobalTransform is seeded so the very
            // first resolve already sees the volume; TransformSystem keeps it
            // updated afterwards.
            LocalTransform lt;
            lt.translation = { opts.get_or("x", 0.0f), opts.get_or("y", 0.0f), opts.get_or("z", 0.0f) };
            lt.scale       = { opts.get_or("sx", 1.0f), opts.get_or("sy", 1.0f), opts.get_or("sz", 1.0f) };
            world.EnsurePool<LocalTransform>();
            world.AddComponent<LocalTransform>(e, lt);
            GlobalTransform gt;
            XMStoreFloat4x4(&gt.matrix, lt.ToMatrix());
            world.EnsurePool<GlobalTransform>();
            world.AddComponent<GlobalTransform>(e, gt);

            return LuaId::Out(world, e);
        });

    // PostProcess.destroyVolume(entity) - remove a volume entity.
    tbl.set_function("destroyVolume",
        [&world](LuaEntity entity)
        {
            const Entity e = LuaId::ResolveLive(world, entity);
            if (e == NullEntity) return;
            world.DestroyEntity(e);
        });

    // PostProcess.setOverride(entity, "group.member", value) - write a property on
    // the volume's profile (Overridable, marked overriding).
    tbl.set_function("setOverride",
        [&world](LuaEntity entity, const std::string& path, double value) -> bool
        {
            auto* vc = world.GetComponent<ECS::PostProcessVolumeComponent>(
                LuaId::ResolveLive(world, entity));
            if (!vc) return false;
            PostProcessProfile* p = ProfileSystem::Get().Get(vc->profile);
            if (!p) return false;
            return SetProfileProperty(*p, path, value);
        });

    // PostProcess.setEngineDefault("group.member", value) - tweak the base look.
    tbl.set_function("setEngineDefault",
        [](const std::string& path, double value) -> bool
        {
            return SetProfileProperty(ProfileSystem::Get().EngineDefault(), path, value);
        });

    // PostProcess.pushTransient{ priority=, masterWeight=, fadeIn=, hold=,
    //   fadeOut=, label=, set={ ["group.member"]=value, ... } } -> override id.
    tbl.set_function("pushTransient",
        [](sol::table opts) -> uint64_t
        {
            PostProcessOverride ov;
            ov.priority     = opts.get_or("priority", 1000.0f);
            ov.masterWeight = opts.get_or("masterWeight", 1.0f);
            ov.fadeIn       = opts.get_or("fadeIn", 0.2f);
            ov.hold         = opts.get_or("hold", 1.0f);
            ov.fadeOut      = opts.get_or("fadeOut", 0.5f);
            const std::string label = opts.get_or<std::string>("label", "");
            if (!label.empty())
            {
                const size_t n = std::min(label.size(), sizeof(ov.label) - 1);
                memcpy(ov.label, label.data(), n);
                ov.label[n] = '\0';
            }

            sol::object setObj = opts["set"];
            if (setObj.is<sol::table>())
            {
                sol::table set = setObj.as<sol::table>();
                for (auto& kvp : set)
                {
                    if (!kvp.first.is<std::string>()) continue;
                    if (!kvp.second.is<double>())     continue;
                    SetProfileProperty(ov.profile, kvp.first.as<std::string>(),
                                       kvp.second.as<double>());
                }
            }
            return Runtime::Get().PushOverride(ov);
        });

    // PostProcess.removeTransient(id) - cancel a pushed override early.
    tbl.set_function("removeTransient",
        [](uint64_t id) { Runtime::Get().RemoveOverride(id); });

    // PostProcess.getResolved("group.member") -> the current resolved scalar.
    tbl.set_function("getResolved",
        [](const std::string& path) -> double
        {
            bool found = false;
            const double v = GetResolvedValue(Runtime::Get().resolved, path, found);
            return found ? v : 0.0;
        });
}

} // namespace PostProcess
