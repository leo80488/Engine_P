#pragma once

// PickingPass: renders per-instance slot ids to an R32_UINT offscreen texture.
//
// Each pixel stores (instanceSlot + 1) for the frontmost visible object; 0 = background (the
// clear value). The Renderer translates slot -> Entity.
//
// A request names a view (0 = the main view, else a secondary view's slot): only that view's
// segment draws it, into the view's own id and depth targets. One pick and one image at a time.
//
// Usage:
//   RequestPick(pixelX, pixelY)  -- before the frame's Execute call.
//   [frame renders + EndFrame submits]
//   IGraphicsDevice::FlushAndWait()
//   ReadPickResult()             -- returns the raw slot+1 value (0 = no hit).
//   ClearPickPending()           -- consume the result.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's id and depth targets)

#include <cstdint>
#include <vector>

class IGraphicsDevice;

class PickingPass : public RG::RenderPass
{
public:
    PickingPass() = default;
    ~PickingPass() override;

    const char* GetName() const override { return "PickingPass"; }
    void Setup    (RG::RenderGraphBuilder& b)                   override;
    void Init     (IGraphicsDevice& gfx)                        override;
    void OnCompile(IGraphicsDevice& gfx)                        override;
    RHI::CommandList Execute(RHI::CommandList cl)               override;
    // Picking is request-driven and idle on nearly every frame: no outstanding
    // RequestPick / RequestImage for the bound view and no pick copy awaiting
    // readback means Execute returns on its first line. Mirrors that exactly.
    bool IsActive() const override { return !RG::PassActivityGatesEnabled() || DrawsInBoundView(); }
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    // A secondary view's segment, serially before it records: its targets at its size when it has
    // a pick or an image to draw, and the image's readback buffer (Execute never creates one).
    void PrepareBoundView(IGraphicsDevice& gfx, uint32_t width, uint32_t height);
    // Drop a pick or an image asked of view @p viewSlot (it is going away).
    void CancelView(uint32_t viewSlot);
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Request a pick at pixel (pixelX, pixelY) of view @p viewSlot's render target.
    // The copy is recorded during that view's next Execute() call.
    void RequestPick(int pixelX, int pixelY, uint32_t viewSlot = 0);

    // After IGraphicsDevice::FlushAndWait(), read the readback buffer.
    // Returns the raw (instanceSlot + 1) value; 0 = background.
    uint32_t ReadPickResult() const;

    bool IsPickPending()  const { return m_pickPending; }
    bool IsPickRequested() const { return m_pickRequested; }   // asked, not yet drawn
    void ClearPickPending()     { m_pickPending = false; }
    // Drop whatever is in the slot (a harness pick given up on).
    void CancelPick()           { m_pickRequested = false; m_pickPending = false; }

    // The whole id buffer, the harness's object-ID image: the view's next Execute draws the full
    // view (no pick scissor) and copies every texel to a READBACK buffer, sized here to the main
    // texture's footprint (a secondary view's: in PrepareBoundView). Independent of the pick's
    // slot: both can be asked in one frame. False while the main view has no id texture (no
    // render size yet) or no buffer could be made.
    bool RequestImage(uint32_t viewSlot = 0);
    bool IsImageRequested() const { return m_imageRequested; }   // asked, not yet drawn
    bool IsImagePending()   const { return m_imagePending; }     // copied, not yet read
    // After IGraphicsDevice::FlushAndWait: the (instanceSlot + 1) values row
    // by row, tight - the footprint's 256-byte row pitch removed. Consumes it.
    bool ReadImage(std::vector<uint32_t>& out, uint32_t& outW, uint32_t& outH);
    void CancelImage()            { m_imageRequested = false; m_imagePending = false; }

private:
    // One view's id and depth targets.
    struct Targets
    {
        RHI::Texture       id;      // R32_UINT render target
        RHI::Texture       depth;   // D32_FLOAT depth buffer
        RHI::ResourceState idState = RHI::ResourceState::RENDERTARGET;
        uint32_t           w = 0;
        uint32_t           h = 0;
    };
    struct ViewTargets : ViewData
    {
        Targets targets;
        void Release(IGraphicsDevice& gfx) override;
    };

    PSODesc BuildPSODesc() const;
    // The bound view has a pick (or its copy awaiting readback) or an image to draw.
    bool DrawsInBoundView() const
    {
        return (m_pickView == m_boundSlot && (m_pickRequested || m_pickPending))
            || (m_imageView == m_boundSlot && m_imageRequested);
    }

    // Recreate @p t's ID/depth textures when the viewport changes.
    void EnsureTextures(IGraphicsDevice& gfx, Targets& t, uint32_t w, uint32_t h);
    // The image's READBACK buffer, at least @p t's id footprint.
    bool EnsureImageReadback(const Targets& t);
    static void ReleaseTargets(IGraphicsDevice& gfx, Targets& t);

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // Owned GPU resources (RHI handles, no DX12 types)
    Targets        m_main;             // the main view's
    Targets*       m_t = &m_main;      // the bound view's
    uint32_t       m_boundSlot = 0;
    RHI::GPUBuffer m_readbackBuffer;   // READBACK heap, single pixel

    // Pick request state.
    // m_pickRequested: set by RequestPick(), consumed by Execute() to record the GPU copy.
    // m_pickPending:   set by Execute() once the copy is recorded; cleared by ClearPickPending().
    //                  IsPickPending() / Renderer::ResolvePick only check this flag.
    bool     m_pickRequested = false;
    bool     m_pickPending   = false;
    int      m_pickX         = 0;
    int      m_pickY         = 0;
    uint32_t m_pickView      = 0;

    // The object-ID image (RequestImage / ReadImage).
    RHI::GPUBuffer m_imageReadback;        // READBACK heap, a whole id texture
    uint64_t       m_imageBytes    = 0;    // its size
    uint32_t       m_imageRowPitch = 0;    // the copy's footprint row pitch (256-aligned)
    uint32_t       m_imageW = 0, m_imageH = 0;   // the texture the copy was recorded from
    uint32_t       m_imageView      = 0;
    bool           m_imageRequested = false;
    bool           m_imagePending   = false;

    // Back-pointer to device for destructor cleanup
    IGraphicsDevice* m_gfx = nullptr;
};
