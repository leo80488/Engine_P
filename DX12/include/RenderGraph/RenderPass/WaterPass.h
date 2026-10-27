#pragma once

// WaterPass - forward water surface: Fresnel sky reflection + flow normals.
//
// Runs right after SkyboxPass (the sky behind the water is already in HDR) and before
// CloudPass / VolumetricFogPass, so clouds composite above the water and the fog
// attenuates it. Draws a procedural grid (SV_VertexID, no buffers) into the HDR
// target with the GBuffer depth bound: reversed-Z GREATER_EQUAL test + depth WRITE,
// so later depth-aware passes (clouds, fog) treat the surface as real geometry. Alpha
// blend gives the analytic shoreline fade.
//
// The water depth (absorption tint + shore fade) comes from the terrain heightmap in
// the PS - water plane Y minus terrain Y, with Terrain.ms.hlsl's UV math - because the
// engine has no read-only DSV: one draw cannot depth-test and sample scene depth.
//
// Reflections:
//   - sky cube: SkyIBLPass::ResolveSkyboxSrvHandle() (atmosphere cube or the static
//     skybox .itex), pushed per frame by the Renderer.
//   - SSR: the water WRITES its flow normal into GBuffer1 and (roughness, 0, 1, 0.5)
//     into GBuffer2, so the post-frame SSR chain traces wavy reflections off it and
//     SSRComposite adds ssr x F x conf onto the HDR the SAME frame. The PS reads the
//     PREVIOUS frame's SSR result (bindless, LightCB.ssrResultIdx; this pass is its
//     only consumer) only to dampen its sky term by (1 - conf) so the composite does
//     not double-count, sampled at the surface point's previous-frame UV (prevClip) so
//     the stale mask does not shear off the reflection while the camera moves.
//
// Renderer wiring (BuildScene_SyncWater): one WaterInstanceCB + WaterInstanceBindings
// per live WaterComponent (+ the first TerrainComponent for the depth mapping), armed
// via SetInstances(). Each body's CB is bound at a 256 B offset (b2) with its own
// heightmap / flow maps / ripple field per sub-draw. The pass skips itself with no
// water bodies or no sky cube yet.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <array>
#include <vector>

namespace RG { class RenderContext; }

// Per-body constant buffer - the HLSL mirror of WaterCB in Water.vs/ps.hlsl (b2
// space0). One is bound per body at a 256 B offset (alignas(256): each instance is a
// self-contained CBV slot). Rows 0-10, padded to 256 B.
struct alignas(256) WaterInstanceCB
{
    // row 0
    float    waterOriginX, waterOriginZ;
    float    waterSize;
    float    waterHeight;
    // rows 1/2
    float    deepColor[4];
    float    shallowColor[4];
    // row 3
    float    flowDirX, flowDirZ;
    float    flowSpeed;
    float    normalTiling;
    // row 4
    float    normalStrength;
    float    absorbDist;
    float    shoreFade;
    float    fresnelF0;
    // row 5
    float    specPower;
    float    reflStrength;
    float    time;
    uint32_t gridQuads;
    // row 6
    float    terrainOriginX, terrainOriginZ;
    float    terrainSize;
    float    terrainBaseY;
    // row 7
    float    terrainHeightScale;
    uint32_t hasTerrain;
    float    hmTexel;
    float    surfaceRoughness;   // GBuffer2.r at water pixels (drives SSR cone)
    // row 8
    float    hmUVOffsetX, hmUVOffsetY;
    float    hmUVScaleX,  hmUVScaleY;
    // row 9 - interactive ripple field
    float    rippleNormalStrength;
    float    rippleTexel;        // 1 / rippleResolution (height-gradient stencil)
    uint32_t hasRipple;          // 0 -> flat fallback bound (no ripple perturbation)
    float    _ripplePad;
    // row 10 - the ripple sim WINDOW footprint (world XZ -> field uv; the field is a
    // local high-detail window, not the whole body)
    float    rippleWinOriginX, rippleWinOriginZ;
    float    rippleWinSize;
    float    _ripplePad2;
};
static_assert(sizeof(WaterInstanceCB) == 256,
    "WaterInstanceCB must be 256B (one CBV slot) — sync Water.vs/ps.hlsl WaterCB");

class WaterPass : public RG::RenderPass
{
public:
    // Max water bodies drawn at once (one CBV slot each in the per-frame ring); the
    // Renderer (BuildScene_SyncWater) stops collecting at this.
    //
    // Paired with, but deliberately above, WaterRippleSystem::kMaxBodies (8): a ripple
    // slot costs a 512^2 ping-pong pair (~2 MB) instead of a 256 B CBV slot, so the two
    // have different budgets. A drawn body without a ripple slot renders flat
    // (WaterRippleSystem::Sync logs it once). Raising this does not raise the ripple pool,
    // and a ripple cap above this one would simulate fields for bodies that never draw.
    static constexpr uint32_t kMaxWaterBodies = 16;

    // Per-body texture bindings, bound per sub-draw beside the per-body CB.
    struct WaterInstanceBindings
    {
        uint64_t heightmapSRV = 0;   // body's terrain heightmap (t2); 0 -> flat fallback
        uint64_t normalASRV   = 0;   // flow-normal map A (t4); 0 -> flat fallback
        uint64_t normalBSRV   = 0;   // flow-normal map B (t5); 0 -> flat fallback
        uint64_t rippleSRV    = 0;   // interactive ripple field (t6); 0 -> flat fallback
    };

    WaterPass(RG::RGTextureHandle normal,
              RG::RGTextureHandle surface,
              RG::RGTextureHandle velocity,
              RG::RGTextureHandle depth);

    const char* GetName() const override { return "WaterPass"; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's early-outs, without its PSO check (see the definition).
    bool IsActive() const override;
    // The main view hides the planar water in wireframe (RenderGraph/SceneView.h).
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Water); }
    // It records any view: the bodies and their CBs are the scene's, the camera comes from the
    // bound view's PerView / LightCB. The bodies' back-to-front order is the main camera's.
    uint32_t HandledEvents() const override { return kViewSafe; }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Per-frame water bodies pushed by the Renderer (BuildScene_SyncWater) before the
    // graph executes; empty -> the pass skips itself. The caller sizes / clamps them.
    void SetInstances(std::vector<WaterInstanceBindings> binds,
                      std::vector<WaterInstanceCB>        cbs)
    {
        m_instBinds = std::move(binds);
        m_instCBs   = std::move(cbs);
    }

    // Any water body queued this frame? Gates the late screen-space SSR chain: the water
    // stamps its GBuffer normal / roughness AFTER the unified reflection pass ran, so
    // water pixels still need it.
    bool HasWater() const { return !m_instBinds.empty(); }

    // Reflection source, pushed by Renderer::SyncSkyboxIBL where the SkyboxPass env map
    // is resolved, so the water reflects exactly the sky being drawn. 0 -> skip the draw.
    void SetSkyCube(uint64_t srv) { m_skyCubeSRV = srv; }

    // Previous-frame SSR result (rgb = reflection, a = confidence), pushed by the
    // Renderer every BeginFrame right after SSRSubsystem::OnResize. 0 -> the pass's 1x1
    // black fallback (conf 0: pure sky reflection, no dampening).
    void SetSSRResult(uint64_t srv) { m_ssrResultSRV = srv; }

    // The SRV behind LightCB.ssrResultIdx: the Renderer resolves its heap index every
    // frame and Water.ps opens it with ResourceDescriptorHeap[]. The fallback is this
    // pass's OWN 1x1 black Texture2D<float4>, never the reflection-probe cube array: the
    // shader dereferences the descriptor, and a dimension mismatch is UB with no
    // debug-layer message under Resource Binding Tier 3.
    //
    // Never cache what the Renderer derives from this: SSRSubsystem::OnResize recreates
    // SSRUpsamplePass::m_colorTex on a render-resolution change, so the handle and its
    // heap index change. The order that keeps it right is OnResize -> SetSSRResult ->
    // resolve -> graph.Execute, within one frame.
    uint64_t GetSSRResultSrv() const
    {
        return m_ssrResultSRV ? m_ssrResultSRV : m_fallbackBlackSRV;
    }
    // A secondary view's: the black stand-in - the SSR result is the main camera's screen.
    uint64_t GetNoSSRSrv() const { return m_fallbackBlackSRV; }

    // The CSM cascade array (the handle LightingPass binds): the sun glint is direct
    // light and must respect terrain shadows. Pushed per frame by
    // Renderer::Render_BindFrameResources; 0 -> the pass's fully-lit stand-in is bound.
    void SetShadowMap(uint64_t srv) { m_shadowSrvHandle = srv; }

    void SetTextureAnisotropy(uint32_t x) { m_textureAnisotropy = x; }

private:
    using WaterCBFrame = std::array<WaterInstanceCB, kMaxWaterBodies>;

    PSODesc BuildPSODesc() const;

    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    int                 m_wrapSamplerIdx   = -1;  // the run-time sampler: flow-normal maps (world-XZ tiling)
    uint32_t            m_textureAnisotropy = 8;
    RG::RGTextureHandle m_normal;    // GBuffer1 - water flow normal for the SSR trace
    RG::RGTextureHandle m_surface;   // GBuffer2 - water roughness for the SSR cone / Fresnel
    RG::RGTextureHandle m_velocity;  // GBuffer3 - water motion vectors (TAA / SSR temporal)
    RG::RGTextureHandle m_depth;

    // Per-frame ring of kMaxWaterBodies CBV slots (one per body, bound at a
    // 256B offset). One physical buffer per frame-in-flight (FrameCB).
    FrameCB<WaterCBFrame>              m_waterCB;
    std::vector<WaterInstanceBindings> m_instBinds;   // pushed by Renderer
    std::vector<WaterInstanceCB>       m_instCBs;      // parallel to m_instBinds
    uint64_t             m_skyCubeSRV      = 0;
    uint64_t             m_ssrResultSRV    = 0;
    uint64_t             m_shadowSrvHandle = 0;
    // 1x1 fully-lit CSM stand-in, bound whenever the real shadow array is absent so the
    // shader never samples an unbound t9 (created in Init).
    RHI::Texture        m_shadowFallbackTex;
    uint64_t            m_shadowFallbackHandle = 0;

    // 1x1 R16_UNORM zero heightmap, so t2 always holds a valid Texture2D descriptor when
    // no terrain exists.
    RHI::Texture m_fallbackHeightmap;
    uint64_t     m_fallbackHeightmapSRV = 0;

    // 1x1 flat tangent-space normal (128,128,255) for t4 / t5 while the flow maps load or
    // are unset - calm water instead of garbage.
    RHI::Texture m_fallbackFlatNormal;
    uint64_t     m_fallbackFlatNormalSRV = 0;

    // 1x1 black Texture2D<float4> for the SSR result while SSR is off or not yet
    // produced: conf reads 0, so the (1 - conf) dampening is a no-op. Handed to the
    // Renderer as a heap index (GetSSRResultSrv), not bound.
    RHI::Texture m_fallbackBlack;
    uint64_t     m_fallbackBlackSRV = 0;

    // 1x1 R16G16 zero ripple field (t6) for bodies without ripples or before the sim
    // produced a frame: height 0 everywhere -> no normal perturbation.
    RHI::Texture m_fallbackRipple;
    uint64_t     m_fallbackRippleSRV = 0;
};
