#pragma once
// Engine.SpawnPrefab / PreloadPrefab / SpawnPrefabAsync - instantiate a .ipfb from
// Lua. The UI twin, Engine.SpawnUIPrefab, is in UI/LuaUIEngineBindings.h.

namespace sol { class state; }
class World;
class Renderer;
namespace Resource { class AssetManager; class AnimationClipSystem; }

namespace Resource
{
    // Appends to the `Engine` table (ScriptSystem::RegisterBindings creates it). The
    // references are App's members (and the Run() renderer) and outlive every call.
    void RegisterLuaPrefabBindings(sol::state& state, World& world, AssetManager& assetMgr,
                                   Renderer& renderer, AnimationClipSystem& animClipSys);
}
