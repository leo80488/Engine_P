#pragma once

// ScriptExposedVar: editor-exposed Lua script variables (Unity/Unreal-style).
// A Logic script declares a static `exposed` table on its returned template:
//
//     local Rotate = {}
//     Rotate.exposed = {
//         amplitude = { type='float', default=0.5, min=0.0, max=5.0, tooltip='Bob height' },
//         frequency = { type='float', default=2.0, min=0.1, max=10.0 },
//         tint      = { type='color', default={0.35, 0.85, 2.80}, hdr=true },
//         clip      = { type='asset', default='', ext='.ianim' },
//         speed     = 1.0,        -- shorthand: a bare literal infers its type
//     }
//
// The engine parses it once into a vector<ScriptVarDesc> (the schema: types, defaults, UI
// metadata). The editor draws one widget per entry and stores per-entity edits as
// ScriptVarValue overrides on the ScriptComponent (serialized into the scene). At spawn
// the engine writes each value onto the per-entity Lua instance table, read as
// `self.<name>`.
// Sol-free and ImGui-free so the widely included ScriptComponent.h stays cheap: the Lua
// parsing lives in ScriptSystem.cpp, the widgets in EditorLayer_ComponentEditors.cpp.

#include <algorithm>
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

// The editable kinds of an exposed variable. The byte values are not serialized (the
// serializer writes ScriptVarTypeToChar's stable char), so reordering this enum is safe.
enum class ScriptVarType : uint8_t
{
    Float,      // f: number; min/max -> slider, else drag
    Int,        // i: integer number
    Bool,       // b: checkbox
    Float3,     // v: xyz vector (injected as a Vec3 usertype: self.v.x/.y/.z)
    Color,      // c: rgb color (injected as Vec3: .x=r .y=g .z=b), optional HDR
    String,     // s: free text
    Entity,     // e: entity id (uint32)
    Asset,      // a: asset path string (drag-drop + ext filter)
};

// A concrete value for one exposed variable, stored per entity as an override on
// ScriptComponent and serialized inline into the scene. Same shape as
// MaterialOverride::Value (POD union + side string).
struct ScriptVarValue
{
    ScriptVarType type = ScriptVarType::Float;
    union
    {
        float    f;
        int32_t  i;
        bool     b;
        float    v3[3];     // Float3 / Color (rgb)
        uint32_t entity;    // Entity id (live-session fallback; persist via str GUID)
    } data = {};
    std::string str;        // String / Asset path; Entity: the target's GUID hex (via GuidRegistry)

    static ScriptVarValue MakeFloat (float v)              { ScriptVarValue r; r.type = ScriptVarType::Float;  r.data.f = v;  return r; }
    static ScriptVarValue MakeInt   (int32_t v)            { ScriptVarValue r; r.type = ScriptVarType::Int;    r.data.i = v;  return r; }
    static ScriptVarValue MakeBool  (bool v)               { ScriptVarValue r; r.type = ScriptVarType::Bool;   r.data.b = v;  return r; }
    static ScriptVarValue MakeFloat3(float x, float y, float z)
    { ScriptVarValue r; r.type = ScriptVarType::Float3; r.data.v3[0]=x; r.data.v3[1]=y; r.data.v3[2]=z; return r; }
    static ScriptVarValue MakeColor (float x, float y, float z)
    { ScriptVarValue r; r.type = ScriptVarType::Color;  r.data.v3[0]=x; r.data.v3[1]=y; r.data.v3[2]=z; return r; }
    static ScriptVarValue MakeString(std::string s)        { ScriptVarValue r; r.type = ScriptVarType::String; r.str = std::move(s); return r; }
    // guidHex = the target's persistent GUID (32-char hex); empty = none (live id only).
    static ScriptVarValue MakeEntity(uint32_t e, std::string guidHex = {})
    { ScriptVarValue r; r.type = ScriptVarType::Entity; r.data.entity = e; r.str = std::move(guidHex); return r; }
    static ScriptVarValue MakeAsset (std::string s)        { ScriptVarValue r; r.type = ScriptVarType::Asset;  r.str = std::move(s); return r; }
};

// One entry of a script's schema, parsed from `exposed`: type, default and UI hints. The
// default is also what the engine injects when an entity has no override.
struct ScriptVarDesc
{
    std::string    name;                 // lua field name == self.<name>
    std::string    label;                // inspector label (empty -> name)
    ScriptVarType  type   = ScriptVarType::Float;
    ScriptVarValue defVal;               // default value (type matches `type`)
    bool           hasMin = false;
    bool           hasMax = false;
    float          minVal = 0.f;
    float          maxVal = 0.f;
    float          speed  = 0.f;         // drag step (0 -> auto)
    bool           hdr    = false;       // Color only: allow values > 1
    std::string    tooltip;
    // type='enum', options={'auto','rotate','slide'}: a string-backed enum, persisted as the
    // string so scene / prefab overrides survive a reordering of the options.
    std::vector<std::string> options;
    std::string    assetExt;             // Asset only, e.g. ".ianim" (chooses the drop payload)
};

// Stable serialization char for a type (independent of the enum's byte values).
inline char ScriptVarTypeToChar(ScriptVarType t)
{
    switch (t)
    {
        case ScriptVarType::Float:  return 'f';
        case ScriptVarType::Int:    return 'i';
        case ScriptVarType::Bool:   return 'b';
        case ScriptVarType::Float3: return 'v';
        case ScriptVarType::Color:  return 'c';
        case ScriptVarType::String: return 's';
        case ScriptVarType::Entity: return 'e';
        case ScriptVarType::Asset:  return 'a';
    }
    return 'f';
}

inline bool ScriptVarTypeFromChar(char c, ScriptVarType& out)
{
    switch (c)
    {
        case 'f': out = ScriptVarType::Float;  return true;
        case 'i': out = ScriptVarType::Int;    return true;
        case 'b': out = ScriptVarType::Bool;   return true;
        case 'v': out = ScriptVarType::Float3; return true;
        case 'c': out = ScriptVarType::Color;  return true;
        case 's': out = ScriptVarType::String; return true;
        case 'e': out = ScriptVarType::Entity; return true;
        case 'a': out = ScriptVarType::Asset;  return true;
    }
    return false;
}

// Map an `exposed` descriptor's `type='...'` string to a ScriptVarType.
inline bool ScriptVarTypeFromString(const std::string& s, ScriptVarType& out)
{
    if (s == "float"  || s == "number")             { out = ScriptVarType::Float;  return true; }
    if (s == "int"    || s == "integer")            { out = ScriptVarType::Int;    return true; }
    if (s == "bool"   || s == "boolean")            { out = ScriptVarType::Bool;   return true; }
    if (s == "vec3"   || s == "float3")             { out = ScriptVarType::Float3; return true; }
    if (s == "color"  || s == "colour")             { out = ScriptVarType::Color;  return true; }
    if (s == "string" || s == "text" || s == "enum")               { out = ScriptVarType::String; return true; }
    if (s == "entity")                              { out = ScriptVarType::Entity; return true; }
    if (s == "asset"  || s == "path")               { out = ScriptVarType::Asset;  return true; }
    return false;
}

// The effective value of a variable: the per-entity override when its type matches the
// schema (and, for an enum, its string is one of the options), else the schema default.
// Used by the editor to seed widgets and by the engine to inject onto the instance.
inline ScriptVarValue ResolveScriptVar(const ScriptVarDesc& d,
                                       const std::unordered_map<std::string, ScriptVarValue>& overrides)
{
    auto it = overrides.find(d.name);
    if (it != overrides.end() && it->second.type == d.type
        && (d.options.empty() || std::find(d.options.begin(), d.options.end(), it->second.str) != d.options.end()))
        return it->second;
    return d.defVal;
}
