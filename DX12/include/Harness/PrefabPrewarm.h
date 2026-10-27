#pragma once
// Harness/PrefabPrewarm.h - -prefab_prewarm=<path.ipfb>@<frame>, the reverse arm of
// prefab_spawn_cost. At the top of the game half of frame <frame>
// (TickPhase::GameplayPreLogic) the harness spawns <path> once, as a runtime spawn, and
// KEEPS the instance: its mesh library, materials, skinned sources and skeleton stay
// resident, and its first gather resolves its custom shaders and acquires its textures.
// Every later spawn of that prefab is then warm, which a "the first spawn is cold" pack
// must be shown to fail on. Spawn-then-destroy would not do: the textures would be
// released with the last binding, and no gather would see the instance to resolve its
// shaders.
// App registers the system only when Requested(), since otherwise it adds a batch to
// every run's plan (ft_core pins GameplayPreLogic's). Compiled out of Retail with the
// rest of the harness.

#include "ECS/ISystem.h"

#include <cstdint>
#include <string>

class Renderer;
namespace Resource { class AssetManager; class AnimationClipSystem; }

namespace Harness
{
    class PrefabPrewarmSystem : public SystemInPhase<TickPhase::GameplayPreLogic>
    {
    public:
        // -prefab_prewarm given, well-formed or not (a malformed value is refused in the
        // constructor's log, and the system then does nothing).
        static bool Requested();

        PrefabPrewarmSystem(Resource::AssetManager& assetMgr, Renderer& renderer,
                            Resource::AnimationClipSystem& clips);

        void Update(World& world, const FrameContext& ctx) override;
        const char* GetName() const override { return "PrefabPrewarmSystem"; }
        SystemRunMask RunMask() const override { return SystemRunMask::Play; }

    private:
        Resource::AssetManager&        m_assetMgr;
        Renderer&                      m_renderer;
        Resource::AnimationClipSystem& m_clips;
        std::string                    m_path;
        std::uint64_t                  m_atFrame = 0;
        bool                           m_armed   = false;
    };
}
