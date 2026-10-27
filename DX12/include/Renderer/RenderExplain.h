#pragma once
// Renderer/RenderExplain.h - "why does this entity look like that?"
//
// One read-only query gathers what is otherwise spread over the ECS, the
// renderer's private runtime-material maps, the texture and resource systems, the
// shader libraries' negative caches and the last frame's draw lists, and names the
// likely cause. Its rules are the ones the shaders and passes really implement,
// several of which are invisible from outside:
//   * a texture that never becomes GPU-resident samples a FALLBACK: white for base
//     colour and the Surface (ARM) map, a flat normal; with the Surface map on
//     white, metalness = metalnessMax (default 1.0) - a fully metallic surface,
//     black wherever there is nothing to reflect;
//   * a custom pixel shader that fails to compile (or whose file is missing) makes
//     the passes SKIP the draw - the entity is absent, not black, and still casts
//     shadows;
//   * the default Emissive map is a 1x1 black texture MULTIPLIED in: emissive
//     strength does nothing until the slot is cleared or replaced.
//
// Lua: Render.explain(id) -> table (Lua_API_Reference.md); SDK eng.explain(id). It
// drains the frame's render work first (main thread only), because it reads the
// draw list and the runtime-material maps the scene build writes.

#include "ECS/ECS.h"
#include "Graphics/ShaderLibrary.h"    // DynamicShaderStatus
#include "Resource/TextureSystem.h"    // TextureSystem::Status

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Renderer;
class World;
struct lua_State;

// The renderer's half of the report, filled by Renderer::ExplainEntity from
// state the renderer keeps private.
struct EntityRenderReport
{
    struct Texture
    {
        std::string slot;          // engine slot label ("Base Color") or custom binding name
        uint32_t    engineSlot = ~0u;   // MaterialComponent::TEXTURESLOT, ~0u = custom
        std::string path;          // what the runtime resource resolved ("" = empty slot)
        Resource::TextureSystem::Status status;
        int32_t     bindless   = -1;
        bool        overridden = false;   // a per-entity runtime override view
    };

    bool bound   = false;   // the entity has a runtime material binding
    bool emitted = false;   // it emitted draws (any list) in the last scene build

    std::vector<Texture> textures;

    int customPSID = -1;           // -1 never resolved, 0 none, >0 dynamic shader id
    int customVSID = -1;
    ShaderLibrary::DynamicShaderStatus ps, vs;   // over the GBuffer + Transparent libraries

    // The last built frame's draw lists holding its instances.
    uint32_t inList[8] = {};       // indexed by DrawFilter
    bool     mainView  = false;    // an instance in a list the main view draws
    bool     gpuCulling = false;   // frustum / occlusion culled later, on the GPU
};

namespace RenderExplain
{
    // Registers the Lua table `Render` with `explain(id)`. `drain` finishes the
    // frame's in-flight render work (FramePipeline::DrainFrameWork); it is called before
    // every read and must only be callable on the main thread.
    void RegisterLua(lua_State* L, World& world, Renderer& renderer, std::function<void()> drain);

    struct Hint
    {
        std::string code;   // "fully_metallic", "custom_ps_failed", ...
        std::string kind;   // "cause" or "note"
        std::string text;
    };
    // The MATERIAL hints of Render.explain(id) for `e`: the same rules, codes and
    // texts in the same order (material_file_missing, custom_ps / _vs _missing |
    // _failed, texture_failed | _loading, material_not_bound, base_color_black,
    // fully_metallic, transparent_invisible | masked_clipped, emissive_masked,
    // unlit) - one rule set, which Render.explain itself calls. Empty without a
    // MaterialComponent. It reads the renderer's runtime-material maps
    // (Renderer::ExplainEntity), so call it only where the render half is idle: the
    // editor's ProcessPendingActions or after FramePipeline::DrainFrameWork, never from an
    // ImGui draw. The Inspector's material row shows these.
    std::vector<Hint> MaterialHints(World& world, const Renderer& renderer, Entity e);
}
