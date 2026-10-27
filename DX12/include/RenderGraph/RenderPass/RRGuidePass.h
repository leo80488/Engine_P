#pragma once

// RRGuidePass - the DLSS-RR guides of a raster frame.
//
// When ray reconstruction denoises a raster (ReSTIR DI / GI) frame there is no path tracer to
// write RR's guides. This pass writes them from the GBuffer with the path tracer's formulas
// (shaders/RRGuides.cs.hlsl): the diffuse albedo, the split-sum specular albedo, the normal +
// roughness and the specular hit distance, into four render-sized textures it owns (~55 MiB at
// 1080p). Under kRasterModels it follows the raster lighting's shading-model rules
// (Lighting.ps.hlsl): a toon pixel authored without PBR specular has no specular albedo, a
// clear coat layers its film over the base.
//
// Placement (as VelocityFillPass): the pre-compute command list, after the whole render graph
// - every GBuffer writer has run - and before the upscaler. -rr_raster_guides=1 also runs it
// beside the path tracer's GBuffer primary, whose own guides it must equal there
// (tools/verify_rr_guides.py --raster); =2 hands DLSS the raster guides instead of the path
// tracer's.
//
// Threading: Prepare / Release on the main thread before the frame's command lists (they
// create and destroy textures); Execute records into the pre-compute list, also on the main
// thread.
//
// Own pipeline layout (one 44-DWORD root-constant block, a static linear-clamp sampler,
// heap-direct indexing): the shared compute root signature cannot give 44 root constants. The
// default layout is restored after the dispatch.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/GraphicsStruct.h"
#include <DirectXMath.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <atomic>

class IGraphicsDevice;

class RRGuidePass : public RG::PassLifecycle
{
public:
    // RRGuides.cs.hlsl's flags.
    static constexpr uint32_t kRasterModels = 1u;   // the raster lighting's shading-model rules
    static constexpr uint32_t kDiDistance   = 2u;   // RestirDIResult slice 1's alpha is the DI light distance
    static constexpr uint32_t kDiffuseOnly  = 4u;   // no specular albedo (the path tracer's -pt_diffuse)

    struct Inputs
    {
        DirectX::XMFLOAT4X4 viewProj{};       // jittered: the matrix the depth buffer was drawn with
        DirectX::XMFLOAT3   camera{};
        DirectX::XMFLOAT3   sunDirection{};   // the direction the light travels
        DirectX::XMFLOAT3   sunColor{};
        uint32_t width = 0, height = 0;
        // Heap indices; 0 = absent (materials: every pixel standard; extra:
        // the material's clear-coat values; diResult: no DI light distance).
        uint32_t depthIdx = 0, albedoIdx = 0, normalIdx = 0, surfaceIdx = 0;
        uint32_t extraIdx = 0, materialsIdx = 0, diResultIdx = 0;
        uint32_t flags = 0;
        // The traced reflection Lighting folds (t44: radiance, confidence), its kept hit distance,
        // and the sky's prefiltered radiance cube.
        uint32_t reflectionIdx = 0, reflectionDistanceIdx = 0;
        uint32_t radianceIdx = 0, radianceMips = 1;
        float    iblStrength = 1;
    };

    // Four render-sized guides (R16G16B16A16_FLOAT x 3 + R32_FLOAT, the path
    // tracer's formats). Main thread, before the frame's command lists.
    bool Prepare(IGraphicsDevice& gfx, uint32_t width, uint32_t height);
    // Frees the guides (RR off, or no raster guides wanted).
    void Release(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders | kShutdown; }   // Renderer::PassSet events this pass handles

    // Records the dispatch; the caller made the inputs compute-readable. The
    // guides end in SHADER_RESOURCE_COMPUTE (DLSS's tagged read state).
    // False when nothing was written (no pipeline, no guides, a missing input).
    bool Execute(RHI::CommandList cl, IGraphicsDevice& gfx, const Inputs& in);

    // The guide (0 albedo, 1 specular, 2 normal + roughness, 3 hit distance),
    // when the last Execute wrote it; nullptr otherwise.
    const RHI::Texture* GetGuide(uint32_t index) const
    {
        return index < 4 && m_wrote && m_guides[index].IsValid() ? &m_guides[index] : nullptr;
    }
    const RHI::Texture* GetGuide(uint32_t index, RHI::ResourceState& state) const
    {
        const RHI::Texture* t = GetGuide(index);
        if (t) state = m_states[index];
        return t;
    }
    // Did this frame's Execute write the guides? The Renderer clears it at the
    // frame's start (MarkStale), so a skipped frame is not reported as live.
    bool Wrote() const { return m_wrote; }
    void MarkStale() { m_wrote = false; }
    bool IsAllocated() const { return m_guides[0].IsValid(); }

private:
    bool BuildPipeline(IGraphicsDevice& gfx);

    RHI::PipelineLayout m_layout;   // its own compute layout (see the file header)
    RHI::PipelineState  m_pso;
    bool                m_pipelineFailed = false;
    RHI::Texture        m_guides[4];
    RHI::ResourceState  m_states[4]{};
    uint32_t            m_width = 0, m_height = 0;
    bool                m_wrote = false;
    bool                m_logged = false;
};

// RRColorSnapshot - DLSS-RR's colour before transparency: the HDR scene after the opaque
// lighting, the fog and the face overlay, before any transparency is drawn over it
// (TransparentPass, then the video quads and the particles). The DLSS-RR guide says this
// snapshot helps with ghosting or disappearing particles, and it is the one colour-chain input
// the guide documents. Streamline's other colour buffers (before fog / particles, the "after"
// ones) are undocumented for RR and are not tagged: ColorBeforeParticles alone turned the
// model's output black on a scene with no particles at all.
//
// RR's input is NPRStabilize's output, which counter-scales every exposure-protected pixel;
// Align adds the same correction to the snapshot, or RR reads the difference as a transparency
// layer (RRColorAlign.cs.hlsl).
//
// One render-sized copy of the HDR target (R16G16B16A16_FLOAT, matched exactly: a
// full-resource copy), ~16.6 MiB at 1080p, only while RR runs. The copy is a graph pass
// (Renderer::Compile) on a Phase B worker: CopyHdrSceneToScoped never touches the shared HDR
// state tracker.
class RRColorSnapshot : public RG::PassLifecycle
{
public:
    struct ServiceRequest { bool wanted = false; };

    // Main thread, at the frame boundary: allocate at the HDR target's size
    // (gfx.GetRenderWidth/Height) while @p wanted, release otherwise; nothing
    // is written yet this frame.
    void Prepare(IGraphicsDevice& gfx, bool wanted);

    // The service point: Prepare while RR runs.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext& c) override { Prepare(*c.gfx, m_request.wanted); }
    void Release(IGraphicsDevice& gfx);
    // The copy pass's activity predicate (Phase A).
    bool Wanted() const { return m_wanted && m_tex.IsValid(); }
    // The copy pass's body (a Phase B worker). Declines when the HDR target
    // is not the snapshot's size and format.
    void Copy(IGraphicsDevice& gfx, RHI::CommandList cl);
    // The snapshot when this frame's copy wrote it (NON_PIXEL_SHADER_RESOURCE).
    const RHI::Texture* Written() const
    { return m_tex.IsValid() && m_written.load(std::memory_order_acquire) ? &m_tex : nullptr; }
    // For the debug target rrColorBeforeTransparency, when written.
    const RHI::Texture* GetTexture(RHI::ResourceState& state) const
    {
        if (!m_tex.IsValid() || !m_written.load(std::memory_order_acquire)) return nullptr;
        state = m_state;
        return &m_tex;
    }
    // Add NPRStabilize's per-pixel correction (stabilized - unstabilized) to
    // the snapshot: the post list, before the evaluate; restores the engine's
    // compute root layout.
    bool Align(IGraphicsDevice& gfx, RHI::CommandList cl, uint64_t hdrSrv, uint64_t stabilizedSrv);
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders | kShutdown; }   // Renderer::PassSet events this pass handles
    void Shutdown(IGraphicsDevice& gfx) override;

private:
    bool BuildAlignPipeline(IGraphicsDevice& gfx);

    ServiceRequest m_request;   // set by the Renderer each frame, read by OnService

    RHI::Texture       m_tex;
    RHI::ResourceState m_state{};
    std::atomic<bool>  m_written{ false };
    uint32_t           m_w = 0, m_h = 0;
    bool               m_wanted = false;
    RHI::PipelineLayout m_alignLayout;   // its own compute layout (Align)
    RHI::PipelineState  m_alignPso;
    bool               m_alignFailed = false, m_alignLogged = false;
};
