// Tags.* - Lua access to the gameplay tags of an entity (Gameplay/GameplayTags.h).

#include "Scripting/LuaGameplayTagBindings.h"

#include "ECS/ECS.h"
#include "Gameplay/GameplayTagsComponent.h"
#include "Scripting/LuaDoc.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <algorithm>
#include <string>
#include <tuple>

namespace
{
    using Gameplay::Tag;
    using Gameplay::TagTable;

    [[noreturn]] void Fail(const char* fn, const std::string& what)
    {
        throw sol::error(std::string("Tags.") + fn + ": " + what);
    }

    Tag Intern(const char* fn, const std::string& name)
    {
        std::string error;
        const Tag tag = TagTable::Get().Intern(name, &error);
        if (!tag.IsValid()) Fail(fn, "bad_tag: " + error);
        return tag;
    }

    // A Tag for a query: an unknown name is a tag nobody holds, not an error.
    Tag Known(const char* fn, const std::string& name)
    {
        std::string error;
        if (!TagTable::ValidName(name, &error)) Fail(fn, "bad_tag: " + error);
        return TagTable::Get().Find(name);
    }

    Entity Live(const char* fn, World& world, const LuaEntity& id)
    {
        const Entity e = LuaId::ResolveLive(world, id);
        if (e == NullEntity) Fail(fn, "stale_handle: the entity is not alive");
        return e;
    }
}

void RegisterLuaGameplayTagBindings(sol::state& lua, World& world)
{
    world.EnsurePool<GameplayTagsComponent>();
    sol::table t = lua.create_named_table("Tags");

    LuaDoc::Fn(t, "Tags", "Add",
        [&world](const LuaEntity& id, const std::string& name, sol::optional<int> count) -> int
        {
            const Entity e = Live("Add", world, id);
            const Tag tag = Intern("Add", name);
            GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(e);
            if (!tags)
            {
                world.AddComponent<GameplayTagsComponent>(e, {});
                tags = world.GetComponent<GameplayTagsComponent>(e);
            }
            tags->authored.Add(tag, std::max(1, count.value_or(1)));
            return tags->Count(tag);
        },
        { .doc = "Gives the entity a tag (a dotted name such as \"Team.Enemy\"), one more count of it: two sources granting "
                 "the same tag need two Tags.Remove. Returns how many of that exact tag the entity now holds.",
          .params = { { .name = "entity" }, { .name = "name", .doc = "a dotted tag name" },
                      { .name = "count", .doc = "how many to add, default 1" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_tag" } });

    LuaDoc::Fn(t, "Tags", "Remove",
        [&world](const LuaEntity& id, const std::string& name, sol::optional<int> count) -> int
        {
            const Entity e = Live("Remove", world, id);
            const Tag tag = Known("Remove", name);
            GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(e);
            if (!tags || !tag.IsValid()) return 0;
            tags->authored.Remove(tag, std::max(1, count.value_or(1)));
            return tags->Count(tag);
        },
        { .doc = "Takes one count (or count) of a tag off the entity, never below zero. Returns how many of that exact tag "
                 "it still holds, a tag a state toggle grants included.",
          .params = { { .name = "entity" }, { .name = "name" }, { .name = "count", .doc = "default 1" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_tag" } });

    LuaDoc::Fn(t, "Tags", "Clear",
        [&world](const LuaEntity& id, sol::optional<std::string> name)
        {
            const Entity e = Live("Clear", world, id);
            GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(e);
            if (!tags) return;
            if (!name) { tags->authored.ClearAll(); return; }
            const Tag tag = Known("Clear", *name);
            if (tag.IsValid()) tags->authored.Clear(tag);
        },
        { .doc = "Drops every count of one tag (or, with no name, every tag) the scripts and the scene gave the entity. A tag "
                 "an animation's state toggle grants is not touched.",
          .params = { { .name = "entity" }, { .name = "name", .doc = "omit to clear them all" } },
          .effect = LuaDoc::Effect::World, .errors = { "stale_handle", "bad_tag" } });

    LuaDoc::Fn(t, "Tags", "Has",
        [&world](const LuaEntity& id, const std::string& name) -> bool
        {
            const Entity e = Live("Has", world, id);
            const Tag tag = Known("Has", name);
            const GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(e);
            return tags && tag.IsValid() && tags->Matches(tag);
        },
        { .doc = "True when the entity holds the tag or any tag below it: Tags.Has(e, \"Team\") is true for an entity "
                 "carrying \"Team.Enemy\". Granted tags count, scripts' and animation's alike.",
          .params = { { .name = "entity" }, { .name = "name" } }, .errors = { "stale_handle", "bad_tag" } });

    LuaDoc::Fn(t, "Tags", "Count",
        [&world](const LuaEntity& id, const std::string& name) -> int
        {
            const Entity e = Live("Count", world, id);
            const Tag tag = Known("Count", name);
            const GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(e);
            return tags && tag.IsValid() ? tags->Count(tag) : 0;
        },
        { .doc = "How many of that exact tag the entity holds (not counting the tags below it); 0 when none.",
          .params = { { .name = "entity" }, { .name = "name" } }, .errors = { "stale_handle", "bad_tag" } });

    LuaDoc::Fn(t, "Tags", "List",
        [&world](sol::this_state ts, const LuaEntity& id) -> sol::table
        {
            const Entity e = Live("List", world, id);
            sol::state_view view(ts);
            sol::table out = view.create_table();
            const GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(e);
            if (!tags) return out;
            struct Row { std::string name; int count; const char* source; };
            std::vector<Row> rows;
            tags->authored.ForEach([&](Tag tag, int count) { rows.push_back({ TagTable::Get().Name(tag), count, "authored" }); });
            tags->derived.ForEach([&](Tag tag, int count) { rows.push_back({ TagTable::Get().Name(tag), count, "state" }); });
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });
            int i = 1;
            for (const Row& r : rows)
            {
                sol::table row = view.create_table();
                row["tag"]    = r.name;
                row["count"]  = r.count;
                row["source"] = r.source;
                out[i++] = row;
            }
            return out;
        },
        { .doc = "The tags the entity holds, sorted by name: {tag, count, source}. source is \"authored\" (Tags.Add, the scene) "
                 "or \"state\" (an animation's StateToggle, rebuilt every frame).",
          .params = { { .name = "entity" } }, .returnsDoc = "a list of {tag, count, source}", .errors = { "stale_handle" } });

    LuaDoc::Fn(t, "Tags", "Valid",
        [](const std::string& name) -> std::tuple<bool, sol::optional<std::string>>
        {
            std::string error;
            if (TagTable::ValidName(name, &error)) return { true, sol::nullopt };
            return { false, error };
        },
        { .doc = "Whether name is a legal tag name (1 to 8 dotted segments of letters, digits and '_', at most 63 characters); "
                 "false and the reason otherwise.",
          .params = { { .name = "name" } } });

    LuaDoc::Fn(t, "Tags", "Parent",
        [](const std::string& name) -> sol::optional<std::string>
        {
            const Tag parent = TagTable::Get().Parent(Known("Parent", name));
            if (!parent.IsValid()) return sol::nullopt;
            return TagTable::Get().Name(parent);
        },
        { .doc = "The tag one segment up (\"Team\" for \"Team.Enemy\"); nil for a root.",
          .params = { { .name = "name" } }, .errors = { "bad_tag" } });
}
