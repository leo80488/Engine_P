// DataTable.* - Lua access to the schema'd data tables (Gameplay/DataTable.h).

#include "Scripting/LuaDataTableBindings.h"

#include "Gameplay/DataTable.h"
#include "Scripting/LuaDoc.h"

#include <sol/sol.hpp>

#include <memory>
#include <string>
#include <tuple>

namespace
{
    using Gameplay::Data::Json;

    sol::object ToLua(sol::state_view lua, const Json& j)
    {
        if (j.is_object() || j.is_array())
        {
            sol::table t = lua.create_table();
            if (j.is_array())
            {
                int i = 1;
                for (const Json& v : j) t[i++] = ToLua(lua, v);
            }
            else
                for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = ToLua(lua, it.value());
            return sol::make_object(lua, t);
        }
        if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
        if (j.is_number())  return sol::make_object(lua, j.get<double>());
        if (j.is_string())  return sol::make_object(lua, j.get<std::string>());
        return sol::make_object(lua, sol::lua_nil);
    }

    // The table at @p path, loading it first; null when it is refused.
    std::shared_ptr<const Gameplay::Data::Table> Open(const std::string& path)
    {
        std::string error;
        Gameplay::Data::Tables::Get().Load(path, error);
        return Gameplay::Data::Tables::Get().Find(path);
    }
}

void RegisterLuaDataTableBindings(sol::state& lua)
{
    sol::table t = lua.create_named_table("DataTable");

    LuaDoc::Fn(t, "DataTable", "Load",
        [](const std::string& path) -> std::tuple<bool, sol::optional<std::string>>
        {
            std::string error;
            if (Gameplay::Data::Tables::Get().Load(path, error)) return { true, sol::nullopt };
            return { false, error };
        },
        { .doc = "Loads the data table at path (once; a loaded one is kept). Returns true, or false and the reason it was "
                 "refused: an illegal schema, a row that breaks it, a file that is not JSON. A refused table serves nothing "
                 "and is listed by DataTable.Refused (and in a harness run's report).",
          .params = { { .name = "path", .doc = "an asset path of a .dtable file" } },
          .effect = LuaDoc::Effect::World,
          .errors = { "refused" } });

    LuaDoc::Fn(t, "DataTable", "Reload",
        [](const std::string& path) -> std::tuple<bool, sol::optional<std::string>>
        {
            std::string error;
            if (Gameplay::Data::Tables::Get().Reload(path, error)) return { true, sol::nullopt };
            return { false, error };
        },
        { .doc = "Reads the table at path again (the editor and the file watcher do this when the file changes). A reload that "
                 "is refused keeps serving the previous good table. Returns true, or false and the reason.",
          .params = { { .name = "path", .doc = "an asset path of a .dtable file" } },
          .effect = LuaDoc::Effect::World,
          .errors = { "refused" } });

    LuaDoc::Fn(t, "DataTable", "Get",
        [](sol::this_state ts, const std::string& path, const std::string& rowId) -> sol::object
        {
            const auto table = Open(path);
            sol::state_view view(ts);
            if (!table) return sol::make_object(view, sol::lua_nil);
            auto it = table->rows.find(rowId);
            return it == table->rows.end() ? sol::make_object(view, sol::lua_nil) : ToLua(view, it->second);
        },
        { .doc = "One row of the table (loaded on first use): a table of its columns, typed as the schema says, defaults "
                 "filled in (a column with neither a value nor a default is absent). Nil for an unknown row or a refused table.",
          .params = { { .name = "path" }, { .name = "rowId" } }, .returns = "table?" });

    LuaDoc::Fn(t, "DataTable", "Rows",
        [](sol::this_state ts, const std::string& path) -> sol::table
        {
            sol::state_view view(ts);
            sol::table out = view.create_table();
            if (const auto table = Open(path))
            {
                int i = 1;
                for (const auto& [id, row] : table->rows) out[i++] = id;
            }
            return out;
        },
        { .doc = "The row ids of the table, sorted; empty for a refused table.", .params = { { .name = "path" } } });

    LuaDoc::Fn(t, "DataTable", "Version",
        [](const std::string& path) -> int
        {
            const auto table = Gameplay::Data::Tables::Get().Find(path);
            return table ? static_cast<int>(table->version) : 0;
        },
        { .doc = "1 after the first load, +1 for every accepted reload; 0 when the table is not loaded.",
          .params = { { .name = "path" } } });

    LuaDoc::Fn(t, "DataTable", "Refused",
        [](sol::this_state ts) -> sol::table
        {
            sol::state_view view(ts);
            sol::table out = view.create_table();
            int i = 1;
            for (const Gameplay::Data::Refusal& r : Gameplay::Data::Tables::Get().Refusals())
            {
                sol::table row = view.create_table();
                row["path"]  = r.path;
                row["error"] = r.error;
                out[i++] = row;
            }
            return out;
        },
        { .doc = "Every table whose latest load or reload was refused: {path, error}, sorted by path.",
          .returnsDoc = "a list of {path, error}" });
}
