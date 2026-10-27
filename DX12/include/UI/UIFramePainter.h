#pragma once
// The Render phase's UI paint: sprite animation, the Canvas image texture leases, the
// second layout resolve, the Canvas paint, the UI Editor's off-screen views and the
// world-space UI tick. App runs it after Renderer::BeginFrame; world-space anchors project
// with this frame's game view.

#include "ECS/ECS.h"                   // Entity
#include "Graphics/ViewSlot.h"         // RHI::kMaxViews
#include "Resource/TextureSystem.h"    // Resource::TextureHandle
#include "UI/UICanvasLayout.h"         // a secondary view's HUD layout

#include <cstdint>
#include <string>
#include <unordered_map>

class IGraphicsDevice;
class Renderer;
struct FrameContext;
struct GameModeContext;
namespace Resource { class ResourceManager; }

namespace UI
{
    class UICanvasSystem;
    class WorldSpaceUISystem;

    class UIFramePainter
    {
    public:
        // @p debugWireframe / @p wireframeSelection: the editor's Canvas wireframe
        // and the entity it highlights (false / 0 in Game.exe).
        void Paint(World& world, const FrameContext& frameCtx, const GameModeContext& ctx,
                   IGraphicsDevice& backend, Renderer& renderer,
                   Resource::TextureSystem& textureSys, Resource::ResourceManager& resourceMgr,
                   UICanvasSystem& uiCanvasSystem, WorldSpaceUISystem& worldSpaceUISystem,
                   bool debugWireframe, uint32_t wireframeSelection);

        // Shutdown, while the device is alive: release every lease.
        void ReleaseAll(Resource::TextureSystem& textureSys, IGraphicsDevice& backend);

        // @p f(slot, width, height) for each secondary view whose HUD this frame's Paint painted
        // (UIPass::ViewDrawList(slot)): what covers the game screen covers them too.
        template <class F> void ForEachViewHud(F&& f) const
        {
            for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
                if (m_viewHuds[slot].painted) f(slot, m_viewHuds[slot].width, m_viewHuds[slot].height);
        }

    private:
        struct UIImageTextureLease
        {
            std::string             path;
            Resource::TextureHandle handle{};
        };
        // Authoritative TextureSystem ownership, keyed by UI entity. Keeping the
        // lease outside the component lets removals, world reloads and Undo restore
        // release/reuse handles even after the old component storage disappears.
        std::unordered_map<Entity, UIImageTextureLease> m_uiImageTextureLeases;

        // A secondary view's HUD: its layout (never UIComputedRect), its texts wrapped at its size,
        // and whether and at which size this frame painted it.
        struct ViewHud
        {
            std::vector<LayoutNode> nodes;
            TextWrapCache           wrap;
            bool                    painted = false;
            uint32_t                width   = 0;
            uint32_t                height  = 0;
        };
        ViewHud m_viewHuds[RHI::kMaxViews];   // [0] unused: the main view's is the game screen's
    };
}
