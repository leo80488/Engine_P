// Attr.* - Lua access to the attributes of an entity (Gameplay/AttributeSet.h).

#include "Scripting/LuaAttributeBindings.h"

#include "Gameplay/AttributeSet.h"
#include "Scripting/LuaDoc.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <string>
#include <tuple>
#include <vector>

namespace
{
    [[noreturn]] void Fail(const char* fn, const std::string& what)
    {
        throw sol::error(std::string("Attr.") + fn + ": " + what);
    }

    Entity Live(const char* fn, World& world, const LuaEntity& id)
    {
        const Entity e = LuaId::ResolveLive(world, id);
        if (e == NullEntity) Fail(fn, "stale_handle: the entity is not alive");
        return e;
    }

    const AttributeSetComponent* SetOf(World& world, Entity e)
    {
        return world.GetComponent<AttributeSetComponent>(e);
    }

    float Written(const char* fn, const Gameplay::Attr::WriteResult& result)
    {
        if (!result.ok) Fail(fn, result.error);
        return result.current;
    }
}

void RegisterLuaAttributeBindings(sol::state& lua, World& world)
{
    world.EnsurePool<AttributeSetComponent>();
    sol::table t = lua.create_named_table("Attr");

    LuaDoc::Fn(t, "Attr", "Init",
        [&world](const LuaEntity& id, sol::object source) -> std::tuple<bool, sol::optional<std::string>>
        {
            const Entity e = Live("Init", world, id);
            std::string error;
            if (source.is<std::string>())
            {
                if (Gameplay::Attr::Init(world, e, source.as<std::string>(), error)) return { true, sol::nullopt };
                return { false, error };
            }
            if (!source.is<sol::table>()) Fail("Init", "bad_profile: pass \"path/table.dtable#row\" or a table of values");
            std::vector<std::pair<std::string, float>> values;
            for (const auto& [key, value] : source.as<sol::table>())
            {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::number)
                    Fail("Init", "bad_attribute: a values table maps names to numbers");
                values.emplace_back(key.as<std::string>(), value.as<float>());
            }
            if (Gameplay::Attr::InitValues(world, e, values, error)) return { true, sol::nullopt };
            return { false, error };
        },
        { .doc = "Gives the entity its attributes, replacing any it had: from a data table row (\"path/table.dtable#row\": "
                 "each column of the row is an attribute, and a row that names MaxHealth but not Health starts at full "
                 "health) or from a table of name = value. Returns true, or false and the reason (a refused table, a row "
                 "that does not exist, an illegal name). Every current value that moved is announced as AttributeChanged.",
          .params = { { .name = "entity" }, { .name = "source", .doc = "a \"path#row\" profile or a table of values", .type = "string|table" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_profile", "bad_attribute" } });

    LuaDoc::Fn(t, "Attr", "Get",
        [&world](const LuaEntity& id, const std::string& name, sol::optional<float> fallback) -> sol::optional<float>
        {
            const Entity e = Live("Get", world, id);
            const AttributeSetComponent* set = SetOf(world, e);
            const AttributeSetComponent::Entry* entry = set ? set->Find(name) : nullptr;
            if (entry) return entry->current;
            return fallback;
        },
        { .doc = "The attribute's current value, or fallback (nil when none is given) when the entity does not have it.",
          .params = { { .name = "entity" }, { .name = "name", .doc = "such as \"Health\"" },
                      { .name = "fallback", .doc = "returned when the attribute is absent" } },
          .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Attr", "GetBase",
        [&world](const LuaEntity& id, const std::string& name) -> sol::optional<float>
        {
            const Entity e = Live("GetBase", world, id);
            const AttributeSetComponent* set = SetOf(world, e);
            const AttributeSetComponent::Entry* entry = set ? set->Find(name) : nullptr;
            if (entry) return entry->base;
            return sol::nullopt;
        },
        { .doc = "The attribute's base (permanent) value; nil when the entity does not have it.",
          .params = { { .name = "entity" }, { .name = "name" } }, .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Attr", "Set",
        [&world](const LuaEntity& id, const std::string& name, float value) -> float
        {
            const Entity e = Live("Set", world, id);
            return Written("Set", Gameplay::Attr::Write(world, e, name, value));
        },
        { .doc = "Sets the attribute's base and current value (adding the attribute, and the entity's attribute set, "
                 "when absent). Health is kept within 0 and MaxHealth, another attribute within its profile column's "
                 "range. Returns the value it holds now. Announces AttributeChanged, and Died when Health reaches zero.",
          .params = { { .name = "entity" }, { .name = "name" }, { .name = "value" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_attribute", "not_finite" } });

    LuaDoc::Fn(t, "Attr", "SetCurrent",
        [&world](const LuaEntity& id, const std::string& name, float value) -> float
        {
            const Entity e = Live("SetCurrent", world, id);
            return Written("SetCurrent", Gameplay::Attr::Write(world, e, name, value, Gameplay::Attr::Target::Current));
        },
        { .doc = "Sets only the attribute's current value, leaving the base: a temporary change.",
          .params = { { .name = "entity" }, { .name = "name" }, { .name = "value" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_attribute", "not_finite" } });

    LuaDoc::Fn(t, "Attr", "Add",
        [&world](const LuaEntity& id, const std::string& name, float delta) -> float
        {
            const Entity e = Live("Add", world, id);
            const AttributeSetComponent* set = SetOf(world, e);
            const AttributeSetComponent::Entry* entry = set ? set->Find(name) : nullptr;
            return Written("Add", Gameplay::Attr::Write(world, e, name, (entry ? entry->current : 0.f) + delta));
        },
        { .doc = "Adds delta (negative to take off) to the attribute's current value and sets the base to the result: "
                 "Attr.Add(e, \"Health\", 20) heals. Clamped like Attr.Set; not a hit, so no Damaged event - use "
                 "Damage.Apply for damage. Returns the value it holds now.",
          .params = { { .name = "entity" }, { .name = "name" }, { .name = "delta" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_attribute", "not_finite" } });

    LuaDoc::Fn(t, "Attr", "Has",
        [&world](const LuaEntity& id, sol::optional<std::string> name) -> bool
        {
            const Entity e = Live("Has", world, id);
            const AttributeSetComponent* set = SetOf(world, e);
            if (!set) return false;
            return !name || set->Find(*name) != nullptr;
        },
        { .doc = "Whether the entity has attributes, or (with a name) that attribute.",
          .params = { { .name = "entity" }, { .name = "name", .doc = "omit to ask for any" } }, .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Attr", "Remove",
        [&world](const LuaEntity& id, const std::string& name) -> bool
        {
            const Entity e = Live("Remove", world, id);
            AttributeSetComponent* set = world.GetComponent<AttributeSetComponent>(e);
            return set && set->Remove(name);
        },
        { .doc = "Takes the attribute off the entity. Returns whether it had it. Announces nothing.",
          .params = { { .name = "entity" }, { .name = "name" } }, .effect = LuaDoc::Effect::World, .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Attr", "List",
        [&world](sol::this_state ts, const LuaEntity& id) -> sol::table
        {
            const Entity e = Live("List", world, id);
            sol::state_view view(ts);
            sol::table out = view.create_table();
            const AttributeSetComponent* set = SetOf(world, e);
            if (!set) return out;
            int i = 1;
            for (const AttributeSetComponent::Entry& entry : set->entries)
            {
                sol::table row = view.create_table();
                row["name"]    = entry.name;
                row["base"]    = entry.base;
                row["current"] = entry.current;
                out[i++] = row;
            }
            return out;
        },
        { .doc = "The entity's attributes sorted by name: {name, base, current}.",
          .params = { { .name = "entity" } }, .returnsDoc = "a list of {name, base, current}", .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Attr", "Profile",
        [&world](const LuaEntity& id) -> sol::optional<std::string>
        {
            const Entity e = Live("Profile", world, id);
            const AttributeSetComponent* set = SetOf(world, e);
            if (!set || set->profile.empty()) return sol::nullopt;
            return set->profile;
        },
        { .doc = "The \"path#row\" the entity's attributes were filled from; nil when none.",
          .params = { { .name = "entity" } }, .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Attr", "ResistName",
        [](const std::string& damageType) -> std::string { return Gameplay::Attr::ResistName(damageType); },
        { .doc = "The attribute that holds the entity's resistance to a damage type: ResistName(\"Fire\") is \"Resist_Fire\". "
                 "A resistance of 0.5 takes half of that type's damage off; 1 all of it; below 0 adds to it.",
          .params = { { .name = "damageType" } } });
}
