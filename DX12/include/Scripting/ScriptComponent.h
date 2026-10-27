#pragma once

// ScriptComponent: attaches one or more Lua Logic scripts to an entity.
// Each slot runs independently (a Unity-style component list): its own per-entity Lua
// instance table, OnSpawn/OnUpdate/OnDestroy lifecycle and exposed-variable overrides.
// Engine-driven events (OnEnter, OnAnimEvent) fire on every attached script that defines
// the handler. ScriptSystem owns the Lua VM and each slot's sol::environment (keyed by
// Entity + slot index, which keeps non-trivial types out of std::any); this component
// stores only the per-slot path, flags and var overrides.

#include "Scripting/ScriptExposedVar.h"

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>

// One attached Lua Logic script and its per-entity authoring data.
struct ScriptInstance
{
    std::string scriptPath;    // e.g. "asset/scripts/rotate.lua"
    bool        enabled = true;

    // Per-entity overrides for editor-exposed script variables (declared via the
    // script's `exposed` table). Keyed by variable name; absent keys fall back to
    // the schema default. The editor writes these; ScriptSystem injects them onto
    // this slot's Lua instance table before OnSpawn so the script reads them as
    // `self.<name>`. Serialized inline on this slot's Script line.
    std::unordered_map<std::string, ScriptVarValue> vars;
};

struct ScriptComponent
{
    // Attached scripts, run in order. An empty vector is a valid (inert) component: the
    // editor's "Add Component" creates one before any script slot is added.
    std::vector<ScriptInstance> scripts;
};
