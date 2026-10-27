#pragma once

// VideoQuadPass - renders VideoComponents that opt into world space: a
// `worldWidth x worldHeight` quad placed by the entity's GlobalTransform.
//
// In the graph after the opaque + transparent passes, so the quad is depth-ordered
// (occluded by closer opaque geometry, blended over transparent + skybox). Depth
// test on, depth write off, as in TransparentPass.
//
// Per frame (Execute): for each extracted Quad (SetQuads), write VideoQuadCB
// { world, viewProj, width, height, alpha, colorSpace } into this frame's CB ring,
// bind the NV12 texture's Y / UV plane SRVs (t6 / t7 space0) and
// DrawInstanced(6, 1) - a procedural quad in the VS.
//
// Each component's frame source puts its frame in the NV12 texture on the graphics
// queue (a copy that waits on the decoder's fence, or an upload) before the graph
// submits, so the PS always samples a finished frame.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FogApply.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's camera)

#include <DirectXMath.h>
#include <vector>

class VideoQuadPass : public RG::RenderPass
{
public:
    // ---- The extracted quad lane ----
    struct Quad
    {
        RHI::Texture        nv12;        // handle, not a resource: 8 bytes
        DirectX::XMFLOAT4X4 world;
        float               width      = 0.0f;
        float               height     = 0.0f;
        float               alpha      = 1.0f;
        uint32_t            colorSpace = 0;
    };

    // CB layout - must match VideoQuad.vs.hlsl + VideoQuad.ps.hlsl b2 space0.
    struct alignas(16) VideoQuadCB
    {
        float    worldMatrix [16];  // 64 B
        float    viewProjMatrix[16]; // 64 B
        float    quadWidth;
        float    quadHeight;
        float    alpha;
        uint32_t colorSpace;          // -> 16 B trailing block
    };

    explicit VideoQuadPass(RG::RGTextureHandle depth);
    ~VideoQuadPass();

    const char* GetName() const override { return "VideoQuadPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    // Mirrors Execute's early-outs. m_pendingGrowSlots is in the predicate on
    // purpose: the ring grow is serviced at the TOP of Execute, so a frame that
    // owes one still has work even if every quad went away in the meantime.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_gfx != nullptr && m_psoCreated
                && (!m_quads.empty() || m_pendingGrowSlots > 0));
    }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // The Renderer pushes the unjittered scene viewProj once per frame before Execute.
    // Passes do not include Renderer.h, so data flows through explicit setters, as in
    // CloudPass / VolumetricFogPass.
    void SetCamera(const DirectX::XMFLOAT4X4& viewProjNoJitter);
    // The bound secondary view's (its segment, VideoFeature::PrepareView); nothing for the main view.
    void SetViewCamera(const DirectX::XMFLOAT4X4& viewProjNoJitter);

    // The game side extracts what the draw needs - a texture handle, a matrix and four
    // scalars per quad - and Execute reads only that, never a live World: Execute runs
    // on a TaskSystem worker (RenderGraph Phase B ParallelFor). Every field is a value,
    // so the lane can be double-buffered and handed across threads with nothing to
    // keep alive.
    void SetQuads(std::vector<Quad>&& quads) { m_quads = std::move(quads); }
    // How many quads the ring can take this frame. The extractor uses it to
    // report a drop rather than silently truncating a frame's worth of video.
    uint32_t QuadCapacity() const;

private:
    // Multi-slot CB ring: one persistently mapped UPLOAD buffer per frame in flight,
    // sub-allocated per draw at 256 B (D3D12 CBV alignment), a region of capacitySlots
    // per view (RHI::kMaxViews) so the views of one frame do not overwrite each other.
    // Grows when a frame asks for more slots than a region holds.
    static constexpr uint32_t kCBStride       = RHI::kConstantBufferAlignment;  // D3D12 CB alignment
    static constexpr uint32_t kCBInitialSlots = 16;
    static constexpr uint32_t kFrameCount     = RHI::kFramesInFlight;

    struct CBRing
    {
        RHI::GPUBuffer buffer;
        uint8_t*       mapped = nullptr;
        uint32_t       capacitySlots = 0;
    };

    // A secondary view's camera.
    struct ViewCamera : ViewData
    {
        DirectX::XMFLOAT4X4 viewProj{};
        void Release(IGraphicsDevice&) override {}
    };

    // (Re-)allocates the ring buffer for one frame slot; frees the old buffer through
    // the standard deferred-release path.
    void EnsureCBRing(IGraphicsDevice& gfx, uint32_t frameIdx, uint32_t slots);

    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx   = nullptr;
    std::vector<Quad> m_quads;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;
    bool             m_psoCreated = false;

    CBRing   m_cbRing[kFrameCount];
    uint32_t m_pendingGrowSlots = 0;       // grow next frame when > 0

    DirectX::XMFLOAT4X4  m_viewProj{};             // the main view's
    DirectX::XMFLOAT4X4* m_cam       = &m_viewProj;   // the bound view's
    uint32_t             m_boundSlot = 0;
};
