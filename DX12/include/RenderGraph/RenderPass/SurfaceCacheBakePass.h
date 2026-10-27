#pragma once

// SurfaceCacheBakePass - rasterises a custom-shader material into its own UV space
// and resolves the result into a MaterialSurfaceCache atlas page.
//
// Placed right after GBufferPass and consuming the same opaque DrawList because the
// thing baked is a GBuffer draw: same PVF vertex fetch, custom pixel shader,
// SurfaceData contract and MRT encode; only SV_Position changes (UV space instead of
// clip space) and the target is a kPageSize^2 scratch set. Everything the draw needs
// (instance buffer, mesh descriptors, material buffer, the per-material b8 CBV and
// space3 texture table) is bound the way GBufferPass binds it, on the same root
// signature: this pass adds no root parameter.
//
// Per frame:
//   1. Walk the opaque DrawList; ask the cache which packets own a page that still
//      needs texels (capped at N pages per frame: a bake re-issues a real
//      mesh-section draw, so it is real raster cost).
//   2. For each: clear the 6 scratch MRTs, draw the packet with instanceCount forced
//      to 1, then dispatch the resolve + dilate compute shaders to repack and copy
//      into the atlas page.
//
// PSO: GBufferPass::BuildPSODesc with four changes, all forced here so the two cannot
// silently diverge: the bake VS, an RGBA rtvFormats[0], depth and stencil off, and
// CullMode::NONE. PSODesc::Hash mixes all of them, so this mints its own PSO.
//
// ALPHA_TEST is never in the permutation: custom_surface.hlsli's alpha test clips
// against a screen-space fwidth(a), which means nothing in UV space and punches holes
// in the page. Raw opacity lands in the albedo page's alpha instead, which is what the
// RT candidate alpha test wants.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialSurfaceCache.h"

class GBufferPass;
namespace RG { class RenderContext; }

class SurfaceCacheBakePass : public RG::RenderPass
{
public:
    SurfaceCacheBakePass(GBufferPass* gbuffer, MaterialSurfaceCache* cache)
        : m_gbuffer(gbuffer), m_cache(cache) {}

    const char* GetName() const override { return "SurfaceCacheBakePass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // Activity gate, queried in the graph's serial phase, so it may only read settled
    // state. Both terms are: the cache's schedule is final after BuildRenderScene, and
    // m_resourcesReady is set by OnPrepare, both earlier in the same Render().
    bool IsActive() const override
    { return m_resourcesReady && m_cache && m_cache->HasPendingBakes(); }

    // Main-thread lazy allocation at RG::PrepareStage::FrameLate, right before the
    // graph executes. The scratch MRTs, the CB ring and the two compute PSOs are created
    // here, not in Init(): with the feature off (the default) nothing is allocated at
    // all, and Execute() runs on a graph worker thread while CreateTexture touches the
    // descriptor-heap allocator, which the graph serialises through Init / OnCompile /
    // prepare-style entry points.
    void OnPrepare(const RG::PrepareContext& c) override;

    // Releases everything OnPrepare lazily allocated. RG::RenderPass has only a
    // defaulted virtual destructor, so without this the six scratch MRTs, the sampler
    // descriptor and the persistently mapped upload CB outlive the device (a debug-layer
    // live-object report). Idempotent: safe when OnPrepare never ran (feature off).
    void Shutdown(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kShutdown; }   // Renderer::PassSet's kShutdown runs Shutdown: the main graph is never Destroy()ed

    // Diagnostics: pages baked by the last Execute.
    uint32_t GetLastBakeCount() const { return m_lastBakeCount; }

    // The command list that wrote the atlases this frame (or an invalid one), and
    // forgets it. DDGI's hits sample these atlases on the compute queue, and a bake moves
    // both of them, whole, to UAV and back on the graphics queue; the Renderer makes
    // DDGI's list wait on this one. Written by Execute on a graph worker, taken on the
    // render thread once the graph has joined - every frame, so a stale list is never
    // handed out.
    RHI::CommandList TakeAtlasWriteList()
    {
        const RHI::CommandList cl = m_atlasWriteCL;
        m_atlasWriteCL = {};
        return cl;
    }

    // Builds now, on the calling thread (a preload's TaskSystem worker), the bake PSO
    // Execute would build for @p dp, a material's synthetic packet whose section the
    // atlas would page (the caller decides: Renderer::SurfaceAtlasEligible). Mirrors the
    // record path's choice; the PSO cache is thread-safe (single flight). Nothing is
    // bound or drawn.
    void WarmPipelines(const DrawPacket& dp);
    // Creates now what OnPrepare would create before the first bake (the scratch MRTs,
    // the resolve CB and the two compute PSOs: EnsureScratch + EnsureComputePSOs), on
    // main while the render lane is idle (App's AssetTick): a preload's listed big step.
    // Idempotent. false = the feature is off or a creation failed.
    bool Prewarm(IGraphicsDevice& gfx);
    // Loads the resolve / dilate compute shaders (the shader library is thread-safe)
    // on a preload's worker, so Prewarm's EnsureComputePSOs on main only builds the two
    // PSOs.
    void WarmShaders();

private:
    // Scratch MRT indices mirror the GBuffer MRT order exactly: the material PS writes
    // all six SV_TARGETs and the PSO must declare all six.
    enum ScratchRT : uint32_t
    {
        RT_Albedo = 0,   // R16G16B16A16_FLOAT (the GBuffer's is R11G11B10: no alpha)
        RT_Normal,       // R16G16B16A16_UNORM  (discarded)
        RT_Surface,      // R8G8B8A8_UNORM      (rough, metal, ao, refl -> repacked)
        RT_Velocity,     // R16G16_FLOAT        (discarded)
        RT_Extra,        // R16G16B16A16_FLOAT  (discarded)
        RT_Emissive,     // R16G16B16A16_FLOAT  (emissive + COVERAGE in .a)
        RT_Count
    };

    // One 256-byte CB slot per page baked. The cache clamps its per-frame budget to
    // kMaxBakesPerFrame, and the ring holds kFramesInFlight frames of those, so a slot
    // is rewritten only after the frame that recorded a dispatch reading it is fenced
    // out. A single-frame ring would let frame N+1's CPU write land in the buffer frame
    // N's queued dispatch still reads (every page would resolve with a later page's rect).
    static constexpr uint32_t kCBSlotStride = 256;
    static constexpr uint32_t kMaxBakesPerFrame = MaterialSurfaceCache::kMaxPagesPerFrame;
    static constexpr uint32_t kCBSlots = kMaxBakesPerFrame * RHI::kFramesInFlight;

    // No customVSID parameter, deliberately: the material's custom VS is a clip-space
    // world-position-offset hook (wind sway, inflate, jiggle), and a UV-space bake has
    // no clip space to offset into; its output would land in the same page texels, so
    // running it would cost a permutation and change nothing. The cost: a custom VS that
    // animates uv0 (scrolling detail) bakes at its rest UVs. Accept it, or exclude that
    // material.
    PSODesc BuildBakePSODesc(PermutationKey perm, uint32_t customPSID) const;

    void    BindGlobals(RHI::CommandList cl, uint64_t bindlessHandle) const;
    bool    EnsureScratch(IGraphicsDevice& gfx);
    bool    EnsureComputePSOs(IGraphicsDevice& gfx);

    GBufferPass*          m_gbuffer = nullptr;   // borrowed; owned by the graph
    MaterialSurfaceCache* m_cache   = nullptr;   // borrowed; owned by the Renderer

    // The bake VS lives in GBufferPass's ShaderLibrary, not our own, so a custom
    // material's PS (dp.customPSID) compiles once for the GBuffer draw and the bake.
    PSOCache      m_psoCache;
    // The two compute entry points need no dynamic id, so they keep their own library
    // and are plain PSOs (no cache: there are exactly two and neither is permuted).
    ShaderLibrary m_csLib;
    RHI::PipelineState m_resolvePSO;
    RHI::PipelineState m_dilatePSO;

    RHI::Texture       m_scratch[RT_Count];
    RHI::ResourceState m_scratchState[RT_Count] = {};

    RHI::GPUBuffer m_resolveCB;
    void*          m_resolveCBMapped = nullptr;
    uint32_t       m_cbCursor        = 0;   // monotonic ring cursor, wraps at kCBSlots

    int      m_samplerIdx     = -1;
    bool     m_resourcesReady = false;
    uint32_t m_lastBakeCount  = 0;
    RHI::CommandList m_atlasWriteCL{};   // see TakeAtlasWriteList
    // Gutter push radius. 4 texels at 128^2 is ~3% of the page and covers the
    // widest bilinear footprint any consumer can produce from a page edge.
    uint32_t m_dilateRings    = 4;
};
