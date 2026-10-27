#pragma once
// The Canvas UI's Engine.* Lua functions (Engine.SpawnUIPrefab, SpawnUI, CaptureUI,
// FindUINode, UIDocumentRoot, ResolveUILayout, UIPaintStats, UIPaintHash, GetUIPointer).

namespace sol { class state; }
class World;
class Renderer;
namespace Resource { class AssetManager; class AnimationClipSystem; }

namespace UI
{
    class UICanvasSystem;
    struct UIInputState;

    // Appends to the `Engine` table. @p uiInput is App's per-frame pointer snapshot, read
    // at call time. The references are App's members (and the Run() renderer); the bound
    // functions capture them, so they must outlive the Lua state.
    void RegisterLuaUIEngineBindings(sol::state& state, World& world, UICanvasSystem& uiCanvasSystem,
                                     const UIInputState& uiInput,
                                     Resource::AssetManager& assetMgr, Renderer& renderer,
                                     Resource::AnimationClipSystem& animClipSys);
}
