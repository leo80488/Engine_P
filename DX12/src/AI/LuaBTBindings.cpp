#include "AI/LuaBTBindings.h"
#include "Diagnostics/Profiler.h"
#include "AI/BTNode.h"
#include "AI/AIComponents.h"
#include "AI/ActionRegistry.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"  // GlobalTransform: entity world pose
#include "ECS/AnimationComponents.h"  // AnimationComponent for FindAnimatedDescendant
#include "System/Log.h"

#include <sol/sol.hpp>

#include "AI/LuaBBConvert.h"          // shared BBValue <-> Lua conversion
#include "Scripting/LuaEntityId.h"    // LuaEntity + LuaId::* (needs sol first)
#include "Scripting/LuaGc.h"          // BTContext without a finalizer
#include "Scripting/LuaDiag.h"        // WarnOnceNamed (BT error flood)
#include <unordered_set>   // visited-set for the Children walk

#include <DirectXMath.h>
#include <cmath>
#include <string>
#include <string_view>

namespace AI
{
    // ==== BTContext userdata ====
    // Lua sees it as ctx: blackboard read / write, world-space positions, delta time.
    // Lifetime: the userdata holds a pointer to a stack BTContext owned by the C++ dispatcher and is
    // valid only during the synchronous Lua call. Do not let Lua keep ctx past the call.
    struct LuaCtxRef
    {
        BTContext* ptr = nullptr;
    };

    // PushBB / PullBB alias the AI/LuaBBConvert.h conversion, which the tree-param parser in
    // AISystem.cpp shares; do not fork it.
    static sol::object PushBB(sol::state_view lua, const BBValue& v)
    {
        return BBValueToLua(lua, v);
    }

    static BBValue PullBB(const sol::object& v)
    {
        return LuaToBBValue(v);
    }

    // ==== Userdata methods ====
    static sol::object Ctx_GetBB(LuaCtxRef& self, sol::this_state ts, const std::string& key)
    {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::lua_nil_t{};
        BlackboardComponent* bb = self.ptr->Blackboard();   // never cached: see BTContext::Blackboard
        if (!bb) return sol::lua_nil_t{};
        auto it = bb->values.find(key);
        if (it == bb->values.end()) return sol::lua_nil_t{};
        return PushBB(lua, it->second);
    }

    static void Ctx_SetBB(LuaCtxRef& self, const std::string& key, sol::object value)
    {
        if (!self.ptr) return;
        BlackboardComponent* bb = self.ptr->Blackboard();
        if (!bb) return;
        // SetBB(key, nil) erases the key, so an is_set gate sees it unset (PullBB would store "").
        if (value.get_type() == sol::type::lua_nil)
        {
            bb->values.erase(key);
            return;
        }
        bb->values[key] = PullBB(value);
    }

    static bool Ctx_HasBB(LuaCtxRef& self, const std::string& key)
    {
        if (!self.ptr) return false;
        BlackboardComponent* bb = self.ptr->Blackboard();
        return bb && bb->values.count(key) > 0;
    }

    // Deletes a blackboard key, for consume-once flags such as ActiveAttack / HitReactStart (SetBB(key,
    // nil) does the same). Returns true if a key was erased.
    static bool Ctx_RemoveBB(LuaCtxRef& self, const std::string& key)
    {
        if (!self.ptr) return false;
        BlackboardComponent* bb = self.ptr->Blackboard();
        return bb && bb->values.erase(key) > 0;
    }

    static LuaEntity Ctx_Entity(LuaCtxRef& self)
    {
        if (!self.ptr) return {};
        return LuaId::OutMaybe(self.ptr->world, self.ptr->entity);
    }

    static float Ctx_DeltaTime(LuaCtxRef& self)
    {
        return self.ptr ? self.ptr->deltaTime : 0.f;
    }

    static float Ctx_Elapsed(LuaCtxRef& self)
    {
        return self.ptr ? self.ptr->elapsed : 0.f;
    }

    // World-space position of any entity from its GlobalTransform translation (origin when it has
    // none). The camera is an ordinary entity, so ChaseCamera resolves through this path too.
    static DirectX::XMFLOAT3 ResolveEntityPos(const World& world, Entity e)
    {
        if (const auto* gt = world.GetComponent<GlobalTransform>(e))
            return { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
        return { 0.f, 0.f, 0.f };
    }

    static sol::table Ctx_GetEntityPosition(LuaCtxRef& self, sol::this_state ts, LuaEntity entityId)
    {
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f;
        if (!self.ptr || !self.ptr->world) return t;
        const auto p = ResolveEntityPos(*self.ptr->world, LuaId::ResolveLive(*self.ptr->world, entityId));
        t["x"] = p.x; t["y"] = p.y; t["z"] = p.z;
        return t;
    }

    // Distance between this entity and another.
    static float Ctx_DistanceTo(LuaCtxRef& self, LuaEntity targetId)
    {
        if (!self.ptr || !self.ptr->world) return 0.f;
        const auto a = ResolveEntityPos(*self.ptr->world, self.ptr->entity);
        const auto b = ResolveEntityPos(*self.ptr->world, LuaId::ResolveLive(*self.ptr->world, targetId));
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    // Depth-first walk of the hierarchy under id (id included); returns the first entity with an
    // AnimationComponent, or an empty LuaEntity. Mirrors Engine.FindAnimatedDescendant (which also
    // works during BT ticks, since AIBTTickSystem binds the world) through BTContext's world pointer.
    static LuaEntity Ctx_FindAnimatedDescendant(LuaCtxRef& self, LuaEntity id)
    {
        if (!self.ptr || !self.ptr->world) return {};
        const World& w = *self.ptr->world;
        const Entity start = LuaId::ResolveLive(w, id);
        if (start == NullEntity) return {};

        // Visited set: a cyclic authored Children graph would otherwise spin forever inside the BT tick.
        std::unordered_set<Entity> visited;
        std::vector<Entity> stack;
        stack.push_back(start);
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!visited.insert(e).second) continue;
            if (w.GetComponent<AnimationComponent>(e))
                return LuaId::Out(w, e);
            if (const auto* kids = w.GetComponent<Children>(e))
            {
                for (auto it = kids->entities.rbegin();
                     it != kids->entities.rend(); ++it)
                {
                    stack.push_back(*it);
                }
            }
        }
        return {};
    }

    void RegisterLuaBTBindings(sol::state& lua)
    {
        lua.new_usertype<LuaCtxRef>("BTContext",
            sol::no_constructor,
            "GetBB",                   &Ctx_GetBB,
            "SetBB",                   &Ctx_SetBB,
            "HasBB",                   &Ctx_HasBB,
            "RemoveBB",                &Ctx_RemoveBB,
            "Entity",                  &Ctx_Entity,
            "DeltaTime",               &Ctx_DeltaTime,
            "Elapsed",                 &Ctx_Elapsed,
            "GetEntityPosition",       &Ctx_GetEntityPosition,
            "DistanceTo",              &Ctx_DistanceTo,
            "FindAnimatedDescendant",  &Ctx_FindAnimatedDescendant);
        // One is pushed for every Lua action / condition call: no __gc
        // (Scripting/LuaGc.h).
        LuaGc::DropTrivialFinalizer<LuaCtxRef>(lua.lua_state());

        // Designer-facing tables. Scripts append entries:
        //   Actions.MoveToTarget    = function(ctx, params) ... end
        //   Conditions.IsLowHealth  = function(ctx, params) ... end
        if (!lua["Actions"].valid())    lua["Actions"]    = lua.create_table();
        if (!lua["Conditions"].valid()) lua["Conditions"] = lua.create_table();

        // Constants so handlers can write return BT.Success instead of a string literal; LuaInvokeBT
        // accepts both.
        sol::table bt = lua.create_table();
        bt["Success"] = "Success";
        bt["Failure"] = "Failure";
        bt["Running"] = "Running";
        lua["BT"] = bt;
    }

    // ==== LuaInvokeBT: called by ActionRegistry when no native handler exists ====
    NodeStatus LuaInvokeBT(sol::state& lua,
                           const char* tableName,
                           const std::string& name,
                           BTContext& ctx,
                           const std::unordered_map<std::string, BBValue>& params,
                           bool& outFoundCallable)
    {
        // Works on the raw Lua stack: sol2 objects took and released a registry reference per lookup and
        // copied the result string, ~2 of every ~5 us of a leaf call. Semantics to keep: the table and the
        // function are looked up on every call (a script may redefine a leaf at any time), a fresh params
        // table is built per call, grown from empty (a leaf may keep or mutate it, and the growth pattern
        // fixes its pairs order), the call runs under the engine's traceback handler, and the result is a
        // bool or one of the three strings, else Failure.
        PROF_SCOPE_D("AI.LuaLeaf");
        outFoundCallable = false;
        lua_State* L = lua.lua_state();
        const int base = lua_gettop(L);
        struct Restore { lua_State* L; int top; ~Restore() { lua_settop(L, top); } } restore{ L, base };

        // [base+1] the error handler protected_function would use (the
        // traceback ScriptSystem installs; nil = none -> msgh 0).
        lua_getglobal(L, sol::detail::default_handler_name());
        const int handlerIdx = lua_isnil(L, -1) ? 0 : lua_gettop(L);
        {
            PROF_SCOPE_D("AI.LuaLeaf.Resolve");
            if (lua_getglobal(L, tableName) != LUA_TTABLE) return NodeStatus::Failure;   // [base+2]
            lua_pushlstring(L, name.data(), name.size());
            lua_gettable(L, -2);                                                      // [base+3] tbl[name]
            if (!sol::stack::check<sol::protected_function>(L, -1, sol::no_panic))
                return NodeStatus::Failure;
        }
        outFoundCallable = true;

        // Arguments: the per-call ctx userdata (a copy of LuaCtxRef, exactly
        // what passing it by value to a sol function pushed) and the params.
        sol::stack::push(L, LuaCtxRef{ &ctx });                                        // [base+4]
        {
            PROF_SCOPE_D("AI.LuaLeaf.Params");
            lua_createtable(L, 0, 0);                                                  // [base+5]
            const int tbl = lua_gettop(L);
            for (const auto& kv : params)
            {
                lua_pushlstring(L, kv.first.data(), kv.first.size());
                PushBB(lua, kv.second).push(L);
                lua_settable(L, tbl);
            }
        }

        int status;
        {
            // Named after the leaf (Detail level only: interning a runtime name
            // per call is a hash lookup no other level should pay).
            ::Diag::Prof::Scope leafScope(name.c_str(), ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Detail);
            status = lua_pcall(L, 2, 1, handlerIdx);                                   // fn + 2 args -> 1 result
        }
        if (status != LUA_OK)
        {
            // Leaves tick at ~10 Hz per agent, so a broken action would log once per agent per tick and flush
            // the editor's 2000-line log ring, original error included, within seconds. Suppress per action
            // name so each broken leaf reports once, with the traceback ScriptSystem::Initialize installs.
            const char* msg = lua_tostring(L, -1);
            LuaDiag::WarnOnceNamed(name, "BT %s.%s lua error: %s",
                                   tableName, name.c_str(), msg ? msg : "(non-string error object)");
            return NodeStatus::Failure;
        }

        switch (lua_type(L, -1))
        {
        case LUA_TBOOLEAN:
            return lua_toboolean(L, -1) ? NodeStatus::Success : NodeStatus::Failure;
        case LUA_TSTRING:
        {
            size_t len = 0;
            const char* sv = lua_tolstring(L, -1, &len);
            const std::string_view r(sv, len);
            if (r == "Success") return NodeStatus::Success;
            if (r == "Failure") return NodeStatus::Failure;
            if (r == "Running") return NodeStatus::Running;
            return NodeStatus::Failure;
        }
        default:
            return NodeStatus::Failure;
        }
    }
}
