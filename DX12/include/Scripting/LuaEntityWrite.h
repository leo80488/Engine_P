#pragma once
// Entity.set's write, shared with Engine.AddComponent.
// Engine.AddComponent(id, tag, kv) builds a component from .iscene key/values through the
// tag's deserializer, which replaces the slot wholesale: on an existing component that
// would reset every key not given and every runtime-only field (a leaked Jolt body, an
// orphaned voice, a light re-stamped as the sun, a second script slot). So an existing
// component is changed exactly as Entity.set changes it: merged over its current values,
// allow-listed, its runtime state carried (Resource/ComponentWrite.h). A new one is created
// from the key/values.
// Include <sol/sol.hpp> first (LuaEntityId.h needs it).

#include <string>

#include "ECS/ECS.h"
#include "Scripting/LuaEntityId.h"   // LuaEntity
#include "Resource/AuthoringSchema.h"

// Entity.set on an existing component: `fields` merged over its current
// values. Raises sol::error("Entity.set: <code>: <message>") like Entity.set;
// returns its result table ({ id, tag, applied, before, after, [warnings] }).
sol::table LuaEntityMergeWrite(sol::state_view lua, World& world, Entity e, LuaEntity id,
                               const std::string& tag, const sol::table& fields);

// A Lua value as the text a deserializer reads, for a NEW component: a number
// ("3", "0.25"), a boolean ("1" / "0", how the serializers write them), a list
// of numbers or a Vec3 ("a_b_c", the vector form), a string (percent-encoded
// the way the serializers write text - their reader decodes it). False, with
// `why`, for anything else.
bool LuaValueToFieldText(const sol::object& v, std::string& out, std::string& why);

// Strict creation of covered tags, before any mutation. Returns all defaults
// merged with the request; raises Entity.create: unknown_key / bad_value.
Resource::Authoring::Values LuaAuthoringCreateValues(const std::string& tag, const sol::table& fields);
