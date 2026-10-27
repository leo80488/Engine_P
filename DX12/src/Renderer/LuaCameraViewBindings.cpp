#include "Renderer/LuaCameraViewBindings.h"

#include "Renderer/Renderer.h"
#include "Scripting/LuaMathTypes.h"    // LuaVec3

#include <sol/sol.hpp>

void RegisterLuaCameraViewBindings(sol::state& state, Renderer& renderer)
{
    sol::table engineTbl = state["Engine"];

    // Active-camera world pose (mirrors RenderView in Renderer::GetView()).
    engineTbl.set_function("GetCameraPosition",
        [&renderer]() -> LuaVec3 {
            const RenderView& v = renderer.GetView();
            return LuaVec3{ v.cameraPosition.x, v.cameraPosition.y, v.cameraPosition.z };
        });
    engineTbl.set_function("GetCameraForward",
        [&renderer]() -> LuaVec3 {
            const RenderView& v = renderer.GetView();
            return LuaVec3{ v.cameraForward.x, v.cameraForward.y, v.cameraForward.z };
        });
}
