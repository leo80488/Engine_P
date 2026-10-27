#pragma once
// LuaDoc: what a Lua binding is, machine-readable.
// A binding registered through LuaDoc::Fn is bound exactly as set_function binds it, and
// described. Its parameter and return types are deduced from the C++ callable
// (sol::meta::bind_traits) and written in LuaCATS syntax (integer, number, string,
// boolean, table, EntityId, T?), so they cannot drift from the code. The hand-written part
// is what code cannot say: parameter names, one line of meaning, a more precise type for a
// generic C++ one (a sol::table that is really a LightData patch), what the call may
// change, when it may be called, and its error codes.
// Read by the -dump_catalog lua.json (each member gets `documented` and, when it is,
// `doc`), Lua help("Entity.set") in harness configurations, and the LuaLS definition-file
// generator. Bindings not registered here still work; they are reported as undocumented.
// Main thread only: filled while the bindings register.

#include <sol/sol.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

struct LuaEntity;   // Scripting/LuaEntityId.h - an entity id ("EntityId" in LuaCATS)

namespace LuaDoc
{
    // What a call may change: the question an agent asks before it calls.
    enum class Effect : std::uint8_t
    {
        Read,       // reads only
        World,      // changes the runtime world (entities, components)
        Settings,   // changes engine or player settings
        Disk,       // writes files
        Process,    // the process itself (quit, open a pipe)
        Job,        // starts work that finishes later (answers a job id)
    };

    // When it may be called.
    enum class Context : std::uint8_t
    {
        Any,
        Update,     // only inside ScriptSystem::Update or a borrow - else a guarded no-op
        Editor,     // Editor.exe only
        Harness,    // harness configurations only
    };

    struct Param
    {
        const char* name     = "";
        const char* doc      = "";
        const char* type     = nullptr;   // overrides the deduced type ("LightDataPatch" for a sol::table)
        bool        optional = false;     // also deduced from sol::optional<T>
    };

    struct Doc
    {
        const char*              doc        = "";       // one line: what it does
        std::vector<Param>       params;                // in order; names for the deduced types
        const char*              returns    = nullptr;  // overrides the deduced return type ("Light?")
        const char*              returnsDoc = "";
        Effect                   effect     = Effect::Read;
        Context                  context    = Context::Any;
        std::vector<const char*> errors;                // codes it raises or answers
    };

    struct Entry
    {
        std::string              name;            // "Engine.GetLight"
        std::string              kind;            // "function" | "property" | "readonly property"
        Doc                      doc;
        std::vector<std::string> types;           // per Lua-visible parameter, deduced (LuaCATS)
        std::vector<bool>        optional;        // per Lua-visible parameter, deduced
        std::string              returns;         // deduced (LuaCATS); "" = returns nothing
        bool                     variadic = false;
    };

    void                      Register(Entry entry);
    const Entry*              Find(const std::string& name);
    std::vector<const Entry*> All();
    const char*               EffectName(Effect e);
    const char*               ContextName(Context c);

    // The parameter's type as documented: the Doc's override, else the deduced one.
    std::string ParamType(const Entry& e, std::size_t i);
    std::string ReturnType(const Entry& e);
    // "fun(id: EntityId, tag: string, fields: table, opts?: table): table"
    std::string Signature(const Entry& e);

    // Lua help("Entity.set") -> the entry as a table; help("Entity") -> the
    // documented members of that table. Harness configurations only.
    void RegisterHelp(lua_State* L);

    // Describe a binding without changing how it is bound: a usertype property,
    // an overload set, a function bound elsewhere. Types are written by hand here.
    void Describe(std::string name, const char* kind, Doc doc,
                  std::vector<std::string> types = {}, std::string returns = {});

    // ---- Type deduction ----
    namespace detail
    {
        template <typename T> struct Optional : std::false_type {};
        template <typename T> struct Optional<sol::optional<T>> : std::true_type { using inner = T; };

        template <typename T> struct Tuple : std::false_type {};
        template <typename... Ts> struct Tuple<std::tuple<Ts...>> : std::true_type {};

        // A multi-value return: one tag per element, to name each in turn.
        template <typename T> struct Tag { using type = T; };
        template <typename T> struct TupleTags;
        template <typename... Ts> struct TupleTags<std::tuple<Ts...>> { using type = std::tuple<Tag<Ts>...>; };

        template <typename T>
        using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

        // Lua never sees these parameters: sol2 fills them in.
        template <typename T>
        constexpr bool Hidden = std::is_same_v<Bare<T>, sol::this_state> ||
                                std::is_same_v<Bare<T>, sol::this_main_state> ||
                                std::is_same_v<Bare<T>, sol::this_environment>;

        template <typename T>
        std::string TypeName()
        {
            using U = Bare<T>;
            if constexpr (std::is_void_v<U>)                                    return "";
            else if constexpr (std::is_same_v<U, bool>)                         return "boolean";
            else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>)      return "integer";
            else if constexpr (std::is_floating_point_v<U>)                     return "number";
            else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view> ||
                               std::is_same_v<U, const char*> || std::is_same_v<U, char*>)
                return "string";
            else if constexpr (Optional<U>::value) return TypeName<typename Optional<U>::inner>() + "?";
            else if constexpr (std::is_same_v<U, LuaEntity>)                    return "EntityId";
            else if constexpr (std::is_same_v<U, sol::table>)                   return "table";
            else if constexpr (std::is_same_v<U, sol::function> ||
                               std::is_same_v<U, sol::protected_function>)      return "function";
            else if constexpr (std::is_same_v<U, sol::variadic_args> ||
                               std::is_same_v<U, sol::variadic_results>)        return "any...";
            else if constexpr (Tuple<U>::value)
            {
                std::string out;
                std::apply([&out](auto... tag) {
                    ((out += (out.empty() ? "" : ", ") + TypeName<typename decltype(tag)::type>()), ...);
                }, typename TupleTags<U>::type{});
                return out;
            }
            else return "any";   // sol::object, a usertype the generator does not name
        }

        template <typename A>
        void AddParam(Entry& e)
        {
            using U = Bare<A>;
            if constexpr (Hidden<U>) return;
            else if constexpr (std::is_same_v<U, sol::variadic_args>) e.variadic = true;
            else
            {
                e.types.push_back(TypeName<U>());
                e.optional.push_back(Optional<U>::value);
            }
        }

        template <typename Args, std::size_t... I>
        void AddParams(Entry& e, std::index_sequence<I...>)
        {
            (AddParam<std::tuple_element_t<I, Args>>(e), ...);
        }
    }

    // Bind AND describe: table.set_function(name, fn), with the parameter and
    // return types deduced from fn. @p tableName is the Lua global ("Engine").
    template <typename F>
    void Fn(sol::table table, const char* tableName, const char* name, F&& fn, Doc doc)
    {
        using Traits = sol::meta::bind_traits<detail::Bare<F>>;
        using Args   = typename Traits::args_tuple;
        Entry e;
        e.name = std::string(tableName) + "." + name;
        e.kind = "function";
        detail::AddParams<Args>(e, std::make_index_sequence<std::tuple_size_v<Args>>{});
        e.returns = detail::TypeName<typename Traits::return_type>();
        e.doc = std::move(doc);
        table.set_function(name, std::forward<F>(fn));
        Register(std::move(e));
    }
}
