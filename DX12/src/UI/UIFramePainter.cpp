#include "UI/UIFramePainter.h"

#include "ECS/FrameContext.h"
#include "Scene/GameModeContext.h"
#include "Graphics/IGraphicsDevice.h"
#include "Renderer/Renderer.h"
#include "Renderer/UIViewService.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "RenderGraph/SceneView.h"
#include "Resource/ResourceManager.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasSystem.h"
#include "UI/WorldSpaceUISystem.h"

#include <unordered_set>

void UI::UIFramePainter::Paint(World& world, const FrameContext& frameCtx, const GameModeContext& ctx,
                               IGraphicsDevice& backend, Renderer& renderer,
                               Resource::TextureSystem& textureSys, Resource::ResourceManager& resourceMgr,
                               UICanvasSystem& uiCanvasSystem, WorldSpaceUISystem& worldSpaceUISystem,
                               bool debugWireframe, uint32_t wireframeSelection)
{
    UI::UIScreen canvas;
    canvas.size = { static_cast<float>(ctx.viewportW),
                    static_cast<float>(ctx.viewportH) };

    // Canvas image SRVs are re-resolved from their texture path every frame: a cached GPU
    // descriptor handle silently aliases whatever texture reuses its heap slot (after a font
    // re-bake, the glyph atlas). Path-less images are solid panels. Advance sprite-sheet
    // animations first, so this frame's uv0/uv1 are set before handles resolve and UI ticks.
    UI::AdvanceSpriteAnimations(world, frameCtx.realDeltaTime);

    std::unordered_set<Entity> liveUIImageEntities;
    auto resolveUiImageHandle = [&](Entity entity, UI::UIImage& image)
    {
        liveUIImageEntities.insert(entity);
        auto lease = m_uiImageTextureLeases.find(entity);

        // A deserialized / restored component reuses the lease kept for the same entity and path;
        // otherwise release the old lease before acquiring the replacement.
        if (lease != m_uiImageTextureLeases.end()
            && lease->second.path == image.texturePath
            && lease->second.handle.IsValid())
        {
            image.texture = lease->second.handle;
            image.resolvedTexturePath = lease->second.path;
        }
        else
        {
            if (lease != m_uiImageTextureLeases.end())
            {
                if (lease->second.handle.IsValid())
                    textureSys.Release(lease->second.handle, backend);
                m_uiImageTextureLeases.erase(lease);
            }
            image.texture = Resource::TextureHandle{};
            image.srvGpuHandle = 0;
            image.resolvedTexturePath.clear();

            if (!image.texturePath.empty())
            {
                image.texture = textureSys.Acquire(
                    image.texturePath, resourceMgr, backend);
                image.resolvedTexturePath = image.texturePath;
                if (image.texture.IsValid())
                    m_uiImageTextureLeases.emplace(entity,
                        UIImageTextureLease{ image.texturePath, image.texture });
            }
        }

        if (image.texturePath.empty()) return;
        image.srvGpuHandle = 0;
        if (textureSys.IsReady(image.texture))
        {
            if (const RHI::Texture* tex = textureSys.GetTexture(image.texture))
            {
                image.srvGpuHandle = backend.GetTextureSRVGpuHandle(*tex);
                // Source dimensions for 9-slice, stamped here because this is the one place per frame that
                // resolves the handle; UICanvasSystem has no route to TextureSystem and should not get one
                // just to read a width.
                image.texWidth  = static_cast<float>(tex->desc.width);
                image.texHeight = static_cast<float>(tex->desc.height);
            }
        }
        else
        {
            image.srvGpuHandle = 0; // not resident yet -> renderer skips it
            image.texWidth = image.texHeight = 0.f;
        }
    };

    // Canvas images use per-entity leases. Empty paths stay solid panels (UIPass's white
    // texture); authored paths keep a live TextureHandle whose descriptor is re-resolved every
    // frame.
    if (auto* canvasImgPool = world.GetPool<UI::UIImage>())
    {
        auto& entities = canvasImgPool->Entities();
        auto& images   = canvasImgPool->Data();
        for (size_t i = 0; i < images.size(); ++i)
        {
            resolveUiImageHandle(entities[i], images[i]);
        }
    }

    // Component removal and World::Clear have no destructor callback: sweep leases whose entity
    // is not in this frame's UIImage pool so TextureSystem refcounts stay balanced.
    for (auto it = m_uiImageTextureLeases.begin();
         it != m_uiImageTextureLeases.end();)
    {
        if (liveUIImageEntities.contains(it->first))
        {
            ++it;
            continue;
        }
        if (it->second.handle.IsValid())
            textureSys.Release(it->second.handle, backend);
        it = m_uiImageTextureLeases.erase(it);
    }

    if (UIPass* uip = renderer.GetUIPass())
    {
        // The Canvas must keep first claim on the shared UIPass draw list: the scene-transition
        // cover is appended after this paint.
        uiCanvasSystem.SetDebugWireframe(debugWireframe, wireframeSelection);
        // Resolve layout again right before painting: scripts, the sequencer or a deferred world
        // load may have created UI since UpdateInput's resolve, and Paint draws only nodes with a
        // UIComputedRect (see UICanvasSystem.h). Fingerprint-cached, so a compare unless something
        // changed. Not UpdateInput: its interaction and capture publication run once a frame.
        uiCanvasSystem.ResolveLayout(world, canvas);
        // The editor's Show > Screen UI off: laid out, not painted (Edit mode only).
        if (!renderer.HideCanvasUI())
            uiCanvasSystem.Paint(world, uip->GetDrawList(), frameCtx.realDeltaTime);
    }

    // The secondary views this frame (Renderer::AcquireView), through their cameras.
    Renderer::ViewFrameCamera viewCams[RHI::kMaxViews];
    bool                      viewLive[RHI::kMaxViews] = {};
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        viewLive[slot] = renderer.GetViewFrameCamera(slot, viewCams[slot]);

    // Their HUDs: a view that shows it lays the game screen out at its own size, into its own
    // storage, and paints its own list (UIPass::ViewDrawList), drawn in its overlay stage. The hit
    // test stays the game screen's (UIComputedRect).
    if (UIPass* uip = renderer.GetUIPass())
        for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        {
            ViewHud& hud = m_viewHuds[slot];
            UI::UIDrawList& list = uip->ViewDrawList(slot);
            list.Clear();
            hud.painted = false;
            if (!viewLive[slot] || !(viewCams[slot].show & static_cast<uint32_t>(RG::ShowFlag::Hud))) continue;
            uip->EnsureViewRing(slot);
            const UI::Vec2 size{ static_cast<float>(viewCams[slot].width), static_cast<float>(viewCams[slot].height) };
            uiCanvasSystem.LayoutScreen(world, size, hud.nodes);
            uiCanvasSystem.PaintScreen(world, hud.nodes, list, hud.wrap);
            hud.painted = true;
            hud.width   = viewCams[slot].width;
            hud.height  = viewCams[slot].height;
        }

    // Off-screen Canvas pictures - the UI Editor's designer views (Renderer/UIViewService.h).
    // Here because glyphs bake during paint and this is the window where that is legal (before
    // FontLibrary::FlushPendingUploads), and the UIImage SRVs above are this frame's. Each view
    // lays out at its own design size into its own storage (never UIComputedRect) and paints
    // into its own draw list. With no view it is one branch (every Game.exe frame). The game
    // screen's size goes along: a designer may ask to match it.
    if (UIViewService* views = renderer.GetUIViews())
        views->PaintPending(world, frameCtx.realDeltaTime, canvas.size);

    // World-space UI: per-frame fade/scale/cull + DamageNumber lifetimes.
    // WorldUIBillboardPass reads the same components using its own viewProj.
    UI::WorldSpaceUIView wsView;
    // The game-side view, not renderer.GetView(): built right after SetCamera from the same
    // camera and viewport, it matches the matrices UploadFrameData writes, so this paint does
    // not depend on Renderer::BeginFrame having run.
    const RenderView&    rv = renderer.GetGameView();
    wsView.viewProjMatrix = rv.viewProjMatrixNoJitter;
    // Camera right / up = first 3 elements of view-matrix columns 0 / 1.
    wsView.cameraRightWS = { rv.viewMatrix.m[0][0],
                              rv.viewMatrix.m[1][0],
                              rv.viewMatrix.m[2][0] };
    wsView.cameraUpWS    = { rv.viewMatrix.m[0][1],
                              rv.viewMatrix.m[1][1],
                              rv.viewMatrix.m[2][1] };
    wsView.canvasSize = { canvas.size.x, canvas.size.y };
    wsView.valid      = true;
    // The secondary views': their fade, scale and cull from their cameras (their lanes,
    // Renderer::ExtractWorldUI).
    UI::WorldSpaceUIViewSlot wsViews[RHI::kMaxViews];
    uint32_t                 wsViewCount = 0;
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        if (!viewLive[slot]) continue;
        const Renderer::ViewFrameCamera& c = viewCams[slot];
        UI::WorldSpaceUIViewSlot& v = wsViews[wsViewCount++];
        v.slot                = slot;
        v.view.viewProjMatrix = c.viewProjNoJitter;
        v.view.cameraRightWS  = { c.view.m[0][0], c.view.m[1][0], c.view.m[2][0] };
        v.view.cameraUpWS     = { c.view.m[0][1], c.view.m[1][1], c.view.m[2][1] };
        v.view.canvasSize     = { static_cast<float>(c.width), static_cast<float>(c.height) };
        v.view.valid          = true;
    }
    worldSpaceUISystem.Tick(world, wsView, frameCtx.realDeltaTime, wsViews, wsViewCount);
}

void UI::UIFramePainter::ReleaseAll(Resource::TextureSystem& textureSys, IGraphicsDevice& backend)
{
    for (const auto& [entity, lease] : m_uiImageTextureLeases)
    {
        (void)entity;
        if (lease.handle.IsValid())
            textureSys.Release(lease.handle, backend);
    }
    m_uiImageTextureLeases.clear();
}
