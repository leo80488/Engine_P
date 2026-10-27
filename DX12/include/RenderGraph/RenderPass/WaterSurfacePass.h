#pragma once

// WaterSurfacePass - mesh-driven water: "a plane + a water material IS water".
//
// Draws DrawFilter::Water, which BuildRenderScene assigns to any mesh entity whose
// MaterialComponent uses ShadingModel::Water (Unreal-style Single Layer Water: a
// translucent Surface material, not a domain). Geometry comes from the ordinary PVF
// bindless path (any mesh: a plane, a river spline, a carved lake bed) and the LOOK
// from the material's custom shader hook, like every other custom material.
//
// Frame slot - the same as the procedural WaterPass: after SkyboxPass (the sky behind
// the water is already in HDR) and before Cloud / HeightFog / VolumetricFog (they
// composite above the water and attenuate it, reading the depth written here).
//
// Output - 4 render targets, the same stamp WaterPass makes:
//   RT0 GBuffer1 normal   (opaque overwrite - the SSR trace input)
//   RT1 GBuffer2 surface  (opaque overwrite - roughness drives the SSR cone)
//   RT2 GBuffer3 velocity (opaque overwrite - TAA / SSR temporal motion)
//   RT3 HDR scene colour  (ALPHA BLENDED - the shoreline fade)
// plus a reversed-Z GREATER_EQUAL depth test with depth WRITE, so later depth-aware
// passes treat the surface as real geometry.
//
// Why not one pass with WaterPass: they cannot share an Execute(). WaterPass binds its
// own resources at t2-t6 (heightmap / sky cube / flow maps / ripple field), but a
// material shader's include closure claims every one of those registers
// (material_bindings.hlsli: t2 g_Materials, t3 g_BaseColor, t4 g_SurfaceMap, t5
// g_NormalMap; custom_surface.hlsli's forward block: t6-t8 for the IBL cubes). Their
// vertex interpolants disagree too (Water.vs.hlsl puts curClip at TEXCOORD0,
// MaterialSurfaceVaryings puts uv there), and they bind LightCB at different
// registers (WaterPass b3, the material path b2). So both passes run side by side, and
// EACH rebinds every slot it reads at the top of its own Execute and inherits nothing
// from the other.
// TODO(water_material_domain_plan M7): delete WaterPass once nothing authors a WaterComponent.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialSampler.h"

#include <vector>

namespace RG { class RenderContext; }

class WaterSurfacePass : public RG::RenderPass
{
public:
    WaterSurfacePass(RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle velocity,
                     RG::RGTextureHandle depth,
                     RG::RGTextureHandle sceneDepthSnapshot,
                     RG::RGTextureHandle refractionSource);

    const char* GetName() const override { return "WaterSurfacePass"; }
    void Setup(RG::RenderGraphBuilder& b)   override;
    void Init (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override;
    // It records any view: its draws are the bound view's Water list, its depth and refraction
    // snapshots the view's graph textures, its inputs pushed per view (BindViewInputs).
    uint32_t HandledEvents() const override { return kViewSafe; }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // A water material's dynamic id resolves in this library like in every other (one
    // DynamicShaderRegistry).
    ShaderLibrary* GetShaderLibrary() { return &m_shaderLib; }

    // Pushed by the Renderer on the MAIN thread after BuildScene classified the
    // candidates: Execute() runs on a render-graph worker and must not query renderer
    // state.
    void SetWaterDrawCount(uint32_t n) { m_waterDrawCount = n; }
    bool HasWater() const { return m_waterDrawCount > 0; }

    // CSM cascade array (the handle LightingPass binds): the sun glint is direct light and
    // must respect terrain shadows. 0 -> the fully-lit stand-in is bound.
    void SetShadowMap(uint64_t srv) { m_shadowSrvHandle = srv; }

    // IBL / environment, for the reflection. Water goes through gRadiance +
    // the reflection-probe path like every other forward material rather than a
    // dedicated sharp sky cube, so an indoor pool reflects its probe.
    void SetIBLHandles(uint64_t irradiance, uint64_t radiance, uint64_t brdfLut)
    { m_iblIrradianceHandle = irradiance; m_iblRadianceHandle = radiance; m_brdfLutHandle = brdfLut; }
    void SetSkySHHandle(uint64_t h) { m_skySHHandle = h; }

    // Clustered point / spot lights (S::ClusterLights / ClusterIndexList / ClusterGrid,
    // the buffers LightingPass reads). Without them a lamp or torch has no effect on the
    // water - the forward path is otherwise sun-only.
    void SetClusterSRVs(uint64_t lights, uint64_t indexList, uint64_t grid)
    { m_clusterLightsSRV = lights; m_clusterIndexListSRV = indexList; m_clusterGridSRV = grid; }
    // The spot-shadow per-slice view buffer (t22, Gfx::SpotShadowVP) - the current
    // frame's, as LightingPass and TransparentPass receive it - for WaterLocalLights' spot
    // shadows. 0 binds this pass's own 1-entry "no atlas" fallback.
    void SetSpotShadowVP(uint64_t vps) { m_spotShadowVPHandle = vps; }

    // Global view-mode wireframe (as the other geometry passes: a distinct PSO, NOT a
    // suppress - water stays visible as wires).
    void SetWireframe(bool w) { m_wireframeMode = w; }
    // The DEBUG_SHADE permutation on this frame's PSOs (see GBufferPass::SetDebugShade) -
    // water counts into the debug targets too.
    void SetDebugShade(bool on) { m_debugShade = on; }
    void SetTextureAnisotropy(uint32_t x) { m_textureAnisotropy = x; }
    // The material sampler's LOD bias follows the upscale ratio
    // (Graphics/MaterialSampler.h); Renderer::BeginFrame calls it with the graph's resize
    // recompile, never while a frame records.
    void SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW);

private:
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID,
                         uint32_t customVSID, bool mirrored) const;

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_depth;
    // Read-only inputs, declared with ReadSRV in Setup so (a) the graph puts them in
    // SHADER_RESOURCE before this pass and (b) - critical for ReflectionSource_NoTrace -
    // an SRV is CREATED for them at all: the Renderer creates that texture without isSRV,
    // and the bindless mirror is gated on `srv.IsValid()`. Drop the declaration and
    // LightCB's index silently names a stale heap slot, with no validation error.
    RG::RGTextureHandle m_sceneDepthSnapshot;
    RG::RGTextureHandle m_refractionSource;

    int   m_linearSamplerIdx = -1;   // the material sampler (SetDynamicSampler): aniso wrap
    float m_mipBias = MaterialSampler::kNativeBias;   // ...and its LOD bias
    uint32_t m_textureAnisotropy = 8;
    bool     m_debugShade = false;   // SetDebugShade

    uint32_t m_waterDrawCount      = 0;
    uint64_t m_shadowSrvHandle     = 0;
    // 1x1 fully-lit CSM stand-in: the shared forward BRDF (MS_ForwardLighting) samples t9
    // on every draw, so the table must never be unbound under it - UB that Tier 3
    // validation reports nothing about. R32_FLOAT / arraySize > 1 to match
    // `Texture2DArray<float>` exactly.
    RHI::Texture       m_shadowFallbackTex;
    uint64_t           m_shadowFallbackHandle = 0;
    uint64_t m_iblIrradianceHandle = 0;
    uint64_t m_iblRadianceHandle   = 0;
    uint64_t m_brdfLutHandle       = 0;
    uint64_t m_skySHHandle         = 0;
    uint64_t m_clusterLightsSRV    = 0;
    uint64_t m_clusterIndexListSRV = 0;
    uint64_t m_clusterGridSRV      = 0;
    uint64_t       m_spotShadowVPHandle   = 0;
    RHI::GPUBuffer m_spotShadowVPFallbackBuf;
    uint64_t       m_spotShadowVPFallback = 0;
    bool     m_wireframeMode       = false;

    // Per-draw engine texture slots must never be left unbound for D3D12
    // validation, and a Water material typically authors its maps in space3
    // rather than the engine slots.
    RHI::Texture m_defaultWhite;
    uint64_t     m_defaultWhiteGpuHandle = 0;
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;
};
