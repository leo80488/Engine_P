// NavModule: navigation as an engine module (src/Modules/Nav.module.json) - Recast / Detour, the
// navmesh, NavAgentSystem (the path follow that turns a NavAgent's destination into steering),
// Nav.* in Lua, the scene's .inav and the navmesh / soup debug wireframes. The NavAgent component
// itself (its serializer, reflection and inspector) is the core's: AI, the character motor and
// Intent.* read and write it as data, and it is inert while this module is off.

#include "Module/EngineModule.h"

#include "ECS/SystemRegistry.h"
#include "ECS/SystemSet.h"
#include "Editor/DebugCategory.h"
#include "Editor/DebugDrawSystem.h"
#include "Nav/LuaNavBindings.h"
#include "Nav/NavAgentSystem.h"
#include "Nav/NavMeshSystem.h"
#include "Scene/SceneNavMesh.h"

#include <memory>
#include <string>

namespace
{
    // Created by Initialize and kept to the end of the process (CPU memory only): the Lua bindings
    // and NavAgentSystem hold it by reference.
    std::unique_ptr<Nav::NavMeshSystem> s_nav;
}

Nav::NavMeshSystem* Nav::ActiveNavMesh()
{
    return s_nav.get();
}

class NavModule final : public Module::IEngineModule
{
public:
    void Initialize(Module::Context&) override
    {
        s_nav = std::make_unique<Nav::NavMeshSystem>();
        // A scene's navMesh= loads here, so runtime FindPath / NavAgent work after a scene load.
        SceneNavMesh::Install({
            [](const std::string& path) { return s_nav->Load(path); },
            []() { return s_nav->SourcePath(); } });
        DebugDrawSystem::AddWireContributor({ DebugCategory::NavMesh,
            [](const DebugDrawSystem::WireContext& c) { s_nav->EmitDebugLines(c.wire); } });
        // The soup is Recast's input; it shares the Collision overlay's distance dial.
        DebugDrawSystem::AddWireContributor({ DebugCategory::NavSoup,
            [](const DebugDrawSystem::WireContext& c) { s_nav->EmitSoupDebugLines(c.wire, c.cameraPos, c.maxDistance); } });
    }

    void RegisterLua(Module::Context& ctx) override
    {
        Nav::RegisterLuaNavBindings(*ctx.lua, *s_nav, *ctx.world, ctx.physics);
    }

    // Right before the player brain: after the BT tick that writes NavAgent, ahead of CharacterMotor,
    // the single intent consumer (a phase's batches follow registration order).
    void RegisterSystems(Module::Context& ctx) override
    {
        ctx.systems->AddInSetBefore<NavAgentTickSystem>(Sets::kGameplaySimulation, "PlayerBrainSystem", *s_nav);
    }
};

ENGINE_MODULE(NavModule)
