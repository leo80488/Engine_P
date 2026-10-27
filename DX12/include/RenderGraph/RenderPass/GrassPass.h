#pragma once

// GrassPass - GoT-style procedural grass blades (mesh-shader pipeline).
//
// Runs right after TerrainPass and writes the same five GBuffer RTVs + HDR
// emissive + shared depth with stencil ref 1 (PBR), so grass takes part in
// deferred lighting / CSM / DDGI / SSAO with no special case downstream.
//
// Pipeline (Grass.hlsli has the design notes):
// - AS (Grass.as.hlsl): one thread per grass patch - frustum cull (conservative
//   AABB, Terrain.as's p-vertex test), distance cull with dissolve fade, LOD pick
//   (blade segments / blades per group / density), and a group-scope prefix scan
//   that assigns MS group ranges (per-lane payload, wave-width agnostic).
// - MS (Grass.ms.hlsl): N blades per group, procedural quadratic Bezier ribbons -
//   hash placement (bit-reversed stratified slots keep positions stable across
//   density changes), bicubic heightmap root anchoring (the terrain MS's math),
//   Voronoi-lite clumps, wind noise re-evaluated at prevTime for TAA velocity,
//   per-blade world-Y / slope gates.
// - PS (Grass.ps.hlsl): GBuffer write (albedo gradient, softened normal,
//   roughness / AO, velocity, zero emissive).
// No vertex / instance / indirect buffers: blade data never touches memory.
// Grass casts no shadows (deliberate); it receives CSM in the deferred resolve.
//
// Renderer wiring (BuildScene_SyncGrass): fills GrassCBData from the first
// GrassComponent + the first TerrainComponent's heightmap mapping and arms the
// pass with SetActiveField(); Execute copies it into the frame's constants and
// binds it at b2. heightmapSRV == 0 -> a flat field at baseY on the pass-owned
// 1x1 fallback heightmap (the shader skips sampling via g_hasHeightmap).

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's camera)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

namespace RG { class RenderContext; }

// HLSL-layout mirror of Grass.hlsli's GrassCB (b2 space0). 20 rows x 16 B.
struct alignas(16) GrassCBData
{
    // row 0
    float    grassOriginX, grassOriginZ;
    float    grassSize;
    float    baseY;
    // row 1
    float    heightScale;
    float    hmTexel;
    uint32_t hasHeightmap;
    uint32_t patchesPerSide;
    // row 2
    float    hmUVOffsetX, hmUVOffsetY;
    float    hmUVScaleX,  hmUVScaleY;
    // row 3
    float    terrainOriginX, terrainOriginZ;
    float    terrainSize;
    float    _padT;
    // row 4
    float    cameraPos[3];
    float    time;
    // row 5
    float    prevTime;
    float    lod0Dist;
    float    lod1Dist;
    float    cullDist;
    // row 6
    float    density;
    float    bladeHeight;
    float    bladeHeightVar;
    float    bladeWidth;
    // row 7
    float    tiltMax;           // radians
    float    bendAmount;
    float    windStrength;
    float    windSpeed;
    // row 8
    float    windScale;
    float    windDirX, windDirZ;
    float    clumpCellSize;
    // row 9
    float    clumpBlend;
    float    minWorldY;
    float    maxWorldY;
    float    maxSlopeCos;
    // rows 10/11
    float    baseColor[4];
    float    tipColor[4];
    // row 12
    float    colorNoiseScale;
    float    colorNoiseAmount;
    float    rootAO;
    float    normalBlend;
    // row 13
    float    viewThicken;
    float    farWidthMul;
    float    roughness;
    uint32_t seed;
    // rows 14..19
    float    frustumPlanes[6][4];
};
static_assert(sizeof(GrassCBData) == 320,
    "GrassCBData layout drift — sync Grass.hlsli cbuffer GrassCB");

class GrassPass : public RG::RenderPass
{
public:
    struct FieldBindings
    {
        uint64_t heightmapSRV         = 0;   // 0 -> the pass-owned 1x1 fallback
        uint32_t dispatchAsGroupCount = 0;
    };

    GrassPass(RG::RGTextureHandle albedo,
              RG::RGTextureHandle normal,
              RG::RGTextureHandle surface,
              RG::RGTextureHandle depth,
              RG::RGTextureHandle velocity,
              RG::RGTextureHandle emissive);

    const char* GetName() const override { return "GrassPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's early-outs.
    bool IsActive() const override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    ShaderLibrary* GetReloadableShaderLibrary() override { return &m_shaderLib; }

    // Per-frame state pushed by Renderer (BuildScene_SyncGrass) before the graph
    // executes. dispatchAsGroupCount == 0 -> skip (no grass entity).
    void SetActiveField(const FieldBindings& b, const GrassCBData& cb)
    {
        m_field     = b;
        m_pendingCB = cb;
    }

    // Global view mode (Renderer::SetViewMode): swaps in the wireframe PSO.
    void SetWireframe(bool w) { m_wireframe = w; }

    // It records any view: the field and its CB are the frame's (SetActiveField, the main view's
    // camera), a secondary view culls and picks its LODs from its own camera (SetViewCamera, its
    // segment).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    void SetViewCamera(ViewState& view, const float cameraPos[3], const float frustumPlanes[6][4]);

private:
    struct ViewCamera : ViewData
    {
        float cameraPos[3]        = {};
        float frustumPlanes[6][4] = {};
        void Release(IGraphicsDevice&) override {}
    };

    bool BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso);

    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;
    RHI::PipelineState  m_psoWire;
    bool                m_wireframe = false;

    GrassCBData          m_pendingCB{};
    const ViewCamera*    m_viewCamera = nullptr;   // the bound secondary view's; null for the main view

    // 1x1 R16_UNORM fallback so root slot 10 always holds a valid Texture2D
    // descriptor when no terrain heightmap exists or is loaded.
    RHI::Texture        m_fallbackHeightmap;
    uint64_t            m_fallbackHeightmapSRV = 0;

    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_emissive;

    FieldBindings m_field{};

    bool m_meshShaderAvailable = false;
};
