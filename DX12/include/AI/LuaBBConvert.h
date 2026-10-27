#pragma once

// LuaBBConvert - the one BBValue <-> Lua conversion, used by the tree-param parser (AISystem.cpp)
// and ctx:SetBB / GetBB (LuaBTBindings.cpp). Do not fork it: two copies drift, and a drifted copy
// silently collapsed string lists to zero vectors.
// Tables (the first positional element decides):
//   table  -> vector<XMFLOAT3> (waypoints, formation slots, ...)
//   string -> vector<string>   (SetRandomState candidates, ...)
//   else   -> one XMFLOAT3     ({x=,y=,z=} or positional {x,y,z})
// Any other value (nil included) becomes an empty string.
// Entities: a packed Lua entity id (LuaEntityId.h: generation << 32 | id, > 2^32 for live entities)
// is stored as the raw Entity so blackboard equality stays number-based. The generation is dropped:
// blackboard entity refs are generation-less, so check Engine.IsAlive when it matters.

#include "AI/AIComponents.h"   // BBValue
#include "ECS/ECS.h"

#include <sol/sol.hpp>

#include <DirectXMath.h>
#include <cmath>
#include <string>
#include <vector>

namespace AI
{
    inline BBValue LuaToBBValue(const sol::object& v)
    {
        if (v.is<bool>()) return v.as<bool>();

        // Packed entity id: test before the int / float branches, which cannot hold a value above int32
        // exactly and would silently destroy it.
        if (v.get_type() == sol::type::number)
        {
            const double d = v.as<double>();
            if (d > 4294967295.0 && d <= 9007199254740992.0
                && d == std::floor(d))
            {
                return static_cast<Entity>(
                    static_cast<uint64_t>(d) & 0xFFFFFFFFull);
            }
        }

        if (v.is<int>())         return v.as<int>();
        if (v.is<float>())       return v.as<float>();
        if (v.is<std::string>()) return v.as<std::string>();
        if (v.is<sol::table>())
        {
            sol::table t = v.as<sol::table>();
            sol::object first = t[1];
            if (first.is<sol::table>())
            {
                std::vector<DirectX::XMFLOAT3> arr;
                for (size_t i = 1; ; ++i)
                {
                    sol::object slot = t[i];
                    if (!slot.is<sol::table>()) break;
                    sol::table st = slot.as<sol::table>();
                    DirectX::XMFLOAT3 f{};
                    if (st["x"].valid()) f.x = st.get_or("x", 0.0f);
                    else                 f.x = st.get_or(1,   0.0f);
                    if (st["y"].valid()) f.y = st.get_or("y", 0.0f);
                    else                 f.y = st.get_or(2,   0.0f);
                    if (st["z"].valid()) f.z = st.get_or("z", 0.0f);
                    else                 f.z = st.get_or(3,   0.0f);
                    arr.push_back(f);
                }
                return arr;
            }
            if (first.is<std::string>())
            {
                std::vector<std::string> arr;
                for (size_t i = 1; ; ++i)
                {
                    sol::object slot = t[i];
                    if (!slot.is<std::string>()) break;
                    arr.push_back(slot.as<std::string>());
                }
                return arr;
            }
            // One XMFLOAT3. get_or with both arguments deduced avoids sol2's get_or<T> overload ambiguity.
            DirectX::XMFLOAT3 f3{};
            if (t["x"].valid()) f3.x = t.get_or("x", 0.0f);
            else                f3.x = t.get_or(1,   0.0f);
            if (t["y"].valid()) f3.y = t.get_or("y", 0.0f);
            else                f3.y = t.get_or(2,   0.0f);
            if (t["z"].valid()) f3.z = t.get_or("z", 0.0f);
            else                f3.z = t.get_or(3,   0.0f);
            return f3;
        }
        return std::string{};
    }

    // BBValue -> Lua. An XMFLOAT3 becomes a table with both {x,y,z} and positional [1..3] (scripts use
    // both); vectors become 1-based arrays; an Entity is pushed as the raw uint32 so blackboard
    // equality stays number-based.
    inline sol::object BBValueToLua(sol::state_view lua, const BBValue& v)
    {
        return std::visit([&](auto&& x) -> sol::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>)
            {
                sol::table t = lua.create_table();
                t["x"] = x.x; t["y"] = x.y; t["z"] = x.z;
                t[1]   = x.x; t[2]   = x.y; t[3]   = x.z;
                return sol::make_object(lua, t);
            }
            else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
            {
                sol::table arr = lua.create_table();
                for (size_t i = 0; i < x.size(); ++i)
                {
                    sol::table t = lua.create_table();
                    t["x"] = x[i].x; t["y"] = x[i].y; t["z"] = x[i].z;
                    t[1]   = x[i].x; t[2]   = x[i].y; t[3]   = x[i].z;
                    arr[i + 1] = t;
                }
                return sol::make_object(lua, arr);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::string>>)
            {
                sol::table arr = lua.create_table();
                for (size_t i = 0; i < x.size(); ++i)
                    arr[i + 1] = x[i];
                return sol::make_object(lua, arr);
            }
            else if constexpr (std::is_same_v<T, Entity>)
            {
                return sol::make_object(lua, static_cast<uint32_t>(x));
            }
            else
            {
                return sol::make_object(lua, x);
            }
        }, v);
    }
}
