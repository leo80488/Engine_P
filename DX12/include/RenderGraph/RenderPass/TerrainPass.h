#pragma once

// TerrainPass - mesh-shader heightmap-displaced terrain.
//
// Runs after GBufferPass and writes the same five GBuffer RTVs, the HDR scene colour
// and the shared depth, so terrain pixels take part in deferred lighting with no
// special case downstream. No clears (GBufferPass cleared); depth test GREATER_EQUAL
// (reversed-Z) with depth write.
//
// Pipeline:
//   - AS (Terrain.as.hlsl): one thread per sub-tile, frustum-culls against the
//     camera planes in TerrainCB, emits the surviving sub-tile indices to the MS
//     via payload.
//   - MS (Terrain.ms.hlsl): one group per visible sub-tile; a 12x12 vertex grid
//     (144 verts, 242 prims) fits one meshlet.
//   - PS (Terrain.ps.hlsl): full PBR per-layer blend (albedo / normal / ARM) gated
//     by per-layer world-metre height and slope ranges.
//   - No quadtree, no distance LOD; the splatmap is optional (when unset the
//     auto-blend gates pick the layer).
//   - The heightmap is sampled at t2 space0 through the per-draw SRV table (root
//     param 10), which has D3D12_SHADER_VISIBILITY_ALL: reachable from MS and PS
//     without touching the global root signature.
//
// Renderer wiring:
//   - The Renderer walks the TerrainComponent entities and uploads the active tile's
//     bounds + heightmap-UV remap into m_terrainCB, published through the
//     RenderContext as RGSlot::TerrainParams, so the pass needs no Renderer*.
//   - SetActiveTile() hands over the heightmap SRV; a 0 handle skips the dispatch
//     this frame.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialSampler.h"

namespace RG { class RenderContext; }

class TerrainPass : public RG::RenderPass
{
public:
    struct TileBindings
    {
        uint64_t heightmapSRV         = 0;
        uint64_t splatmapSRV          = 0;
        // StructuredBuffer<TerrainLayerGPU> SRV: the per-layer material table the PS loops
        // over by layerCount, bound to the free per-draw SRV table at t4 space0 (root param
        // 12). 0 falls back to the splatmap as a benign placeholder so the slot is always
        // valid.
        uint64_t layerBufferSRV       = 0;
        uint32_t dispatchAsGroupCount = 0;
    };

    TerrainPass(RG::RGTextureHandle albedo,
                RG::RGTextureHandle normal,
                RG::RGTextureHandle surface,
                RG::RGTextureHandle depth,
                RG::RGTextureHandle velocity,
                RG::RGTextureHandle emissive);

    const char* GetName() const override { return "TerrainPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's first-line early-out.
    bool IsActive() const override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // It records any view: the camera's frustum planes are in the bound view's TerrainParams
    // (a FrameCB ring per view; Renderer::BeginViewSegment writes a secondary view's).
    uint32_t HandledEvents() const override { return kViewSafe; }

    // Hot reload: the base default walks the shader library accessor.
    ShaderLibrary* GetReloadableShaderLibrary() override { return &m_shaderLib; }

    // Per-frame configuration written by the Renderer before the graph executes.
    // heightmapSRV: t2 space0, required (the MS samples it for displacement); 0 = no
    //   terrain entity this frame, skip the dispatch.
    // splatmapSRV: t3 space0, optional; 0 -> each layer's height / slope gate sets its
    //   weight (a slope-debug palette when there are no layers either).
    // dispatchAsGroupCount: amplification-shader groups to dispatch. Each covers
    //   AS_GROUP_SIZE (32) sub-tiles and emits 0..32 MS groups after frustum culling;
    //   the Renderer computes ceil(tilesPerSide^2 / AS_GROUP_SIZE).
    void SetActiveTile(const TileBindings& b) { m_tile = b; }

    // Global view mode: true selects the FILL_MODE_WIREFRAME PSO. Terrain writes the
    // GBuffer like opaque meshes, so its Unlit / Wireframe colour comes from the deferred
    // LightingPass branch; this only flips the rasterizer fill. Set per frame by the
    // Renderer from ViewMode::Wireframe.
    void SetWireframe(bool w) { m_wireframe = w; }
    void SetTextureAnisotropy(uint32_t x) { m_textureAnisotropy = x; }
    // The material sampler's LOD bias follows the upscale ratio
    // (Graphics/MaterialSampler.h); Renderer::BeginFrame calls it with the graph's resize
    // recompile, never while a frame records.
    void SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW);

    // Read-only accessor: ShadowPass dispatches a depth-only copy of the terrain into
    // each CSM cascade with the same heightmap SRV the colour pass uses this frame.
    // dispatchAsGroupCount == 0 or heightmapSRV == 0 means "skip terrain shadow".
    const TileBindings& GetTileBindings() const { return m_tile; }

private:
    // Builds the terrain mesh-shader PSO into @p outPso with the given fill
    // mode. Called twice at Init (solid + wireframe) so the view-mode switch is
    // a cheap PSO swap in Execute rather than a runtime rebuild.
    bool BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso);

    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;          // solid fill (FILL_MODE_SOLID)
    RHI::PipelineState  m_psoWire;      // wireframe fill (FILL_MODE_WIREFRAME)
    bool                m_wireframe = false;
    int                 m_wrapSamplerIdx  = -1;   // the run-time sampler: layer albedos at world XZ x tiling
    float               m_mipBias = MaterialSampler::kTerrainNativeBias;   // ...and its LOD bias
    uint32_t            m_textureAnisotropy = 8;

    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_emissive;

    // Per-frame state pushed by the Renderer.
    TileBindings m_tile{};

    // Device feature query result: Init records false when the GPU / runtime lacks
    // D3D12_MESH_SHADER_TIER_1, and Execute is then a silent no-op so the rest of the
    // engine still runs.
    bool m_meshShaderAvailable = false;
};
