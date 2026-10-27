#pragma once
// LuaGc: value usertypes the Lua collector need not finalize (docs/perf/lua_gc_policy.md).
// sol2 puts a __gc on the metatable of every usertype pushed by value, even one with a
// no-op destructor. That makes each value finalizable: the collection that finds it dead
// resurrects it, a later step runs the no-op destructor through the VM, and only the next
// collection frees it, counting it live until then. Most per-frame script garbage is such
// values (Vec3, Quat, the component proxies). When all of a scene's garbage is
// finalizable, the incremental collector's threshold grows by the whole live set every
// cycle (the heap grows without bound), and the generational one never leaves its
// stop-the-world fallback (the resurrected garbage grows per full collection, failing
// lgc.c stepgenfull's 9/8 test).
// DropTrivialFinalizer<T> removes the __gc from T's value metatables (T and const T) once
// lua.new_usertype<T> has made them, so a value pushed afterwards is plain userdata freed by
// the first collection that finds it dead. Only for a trivially destructible T.

#include <sol/sol.hpp>

#include <string>
#include <type_traits>

namespace LuaGc
{
    template <typename T>
    void DropTrivialFinalizer(lua_State* L)
    {
        static_assert(std::is_trivially_destructible_v<T>,
                      "a usertype whose destructor does something keeps its __gc");
        for (const std::string* name : { &sol::usertype_traits<T>::metatable(),
                                         &sol::usertype_traits<const T>::metatable() })
        {
            luaL_getmetatable(L, name->c_str());
            if (lua_istable(L, -1))
            {
                lua_pushnil(L);
                lua_setfield(L, -2, "__gc");
            }
            lua_pop(L, 1);
        }
    }
}
