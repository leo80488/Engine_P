#pragma once

// PropertyBag <-> sol::table, both directions, in one place.
// Header-only and sol-first: include <sol/sol.hpp> before this (as LuaEntityId.h).
// Vec3/Vec4 cross as { x, y, z[, w] } tables. Coming back, only string keys are read: a
// table with an `x` is a vector (Vec4 when it has a `w`), any other table is skipped, a
// boolean becomes int 0 / 1.

#include "ECS/NotifyTypes.h"   // PropertyBag

#include <string>

namespace Scripting
{

inline sol::table PropertyBagToLua(sol::state& lua, const PropertyBag& bag)
{
    sol::table t = lua.create_table();
    for (const auto& [key, value] : bag.values)
    {
        std::visit([&](const auto& v)
        {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, DirectX::XMFLOAT3>)
                t[key] = lua.create_table_with("x", v.x, "y", v.y, "z", v.z);
            else if constexpr (std::is_same_v<V, DirectX::XMFLOAT4>)
                t[key] = lua.create_table_with("x", v.x, "y", v.y,
                                               "z", v.z, "w", v.w);
            else
                t[key] = v;
        }, value);
    }
    return t;
}

// Lua numbers are all doubles, so the int/float split is reconstructed: an integral value
// becomes an int, anything else a float (otherwise params.count comes back as 3.0 and
// fails GetInt).
inline PropertyBag PropertyBagFromLua(const sol::table& t)
{
    PropertyBag bag;
    if (!t.valid()) return bag;

    for (const auto& [keyObj, valueObj] : t)
    {
        if (!keyObj.is<std::string>()) continue;   // array parts are not params
        const std::string key = keyObj.as<std::string>();

        if (valueObj.is<bool>())
        {
            bag.SetInt(key, valueObj.as<bool>() ? 1 : 0);
        }
        else if (valueObj.is<double>())
        {
            const double d = valueObj.as<double>();
            const double truncated = static_cast<double>(static_cast<int>(d));
            if (d == truncated) bag.SetInt(key, static_cast<int>(d));
            else                bag.SetFloat(key, static_cast<float>(d));
        }
        else if (valueObj.is<std::string>())
        {
            bag.SetString(key, valueObj.as<std::string>());
        }
        else if (valueObj.is<sol::table>())
        {
            sol::table v = valueObj.as<sol::table>();
            if (!v["x"].valid()) continue;         // not a vector; skip quietly
            const float x = v.get_or("x", 0.f);
            const float y = v.get_or("y", 0.f);
            const float z = v.get_or("z", 0.f);
            if (v["w"].valid())
                bag.SetVec4(key, DirectX::XMFLOAT4{ x, y, z, v.get_or("w", 0.f) });
            else
                bag.SetVec3(key, DirectX::XMFLOAT3{ x, y, z });
        }
    }
    return bag;
}

} // namespace Scripting
