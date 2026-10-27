#include "RenderGraph/RenderPass/WaterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>

// Root slots, from Graphics/RootSignatureLayout.h (the single source of the shared
// graphics root signature); ABSOLUTE indices, as BindDescriptorTableHandle takes:
//   t2 space0      <- terrain heightmap
//   t3 space0      <- sky TextureCube
//   t4 space0      <- flow-normal map A
//   t5 space0      <- flow-normal map B
//   t6 space0      <- ripple height field: the IBL-irradiance slot, which water never
//                     reads (Water.ps declares Texture2D<float2> g_RippleField at t6)
//   t9..t11 space0 <- CSM cascade array
// The previous-frame SSR result has no root slot: this pass OWNS the handle
// (SetSSRResult / m_fallbackBlackSRV), the Renderer folds it into
// LightCB.ssrResultIdx every frame via GetSSRResultSrv(), and Water.ps opens it with
// ResourceDescriptorHeap[] (LightCB is bound here at b3).
static constexpr auto kHeightmapRootSlot = RHI::Slots::Gfx::SRV_t2;
static constexpr auto kSkyCubeRootSlot   = RHI::Slots::Gfx::SRV_t3;
static constexpr auto kNormalARootSlot   = RHI::Slots::Gfx::SRV_t4;
static constexpr auto kNormalBRootSlot   = RHI::Slots::Gfx::SRV_t5;
static constexpr auto kRippleRootSlot    = RHI::Slots::Gfx::SRV_t6;  // == IBLIrradiance, repurposed
static constexpr auto kShadowSRVSlot     = RHI::Slots::Gfx::ShadowSRV;

// GROUP-RELATIVE indices - BindConstantBuffer / AtOffset add Gfx::CBVBase; an
// absolute root index silently binds NOTHING (the backend range-checks against the
// group Count). The per-body WaterParams CB is bound at b2 via
// BindConstantBufferAtOffset (CBV "slot" 1 = the b2 root CBV), one 256 B
// WaterInstanceCB per body.
static constexpr auto kWaterParamsCBSlot = RHI::Slots::GfxRel::CB_b2;   // b2
static constexpr auto kPerViewCBSlot     = RHI::Slots::GfxRel::CB_b1;   // b1
static constexpr auto kLightCBSlot       = RHI::Slots::GfxRel::CB_b3;   // b3

WaterPass::WaterPass(RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle velocity,
                     RG::RGTextureHandle depth)
    : m_normal(normal), m_surface(surface), m_velocity(velocity), m_depth(depth)
{}

void WaterPass::Setup(RG::RenderGraphBuilder& b)
{
    // GBuffer normal + surface as writes: the water stamps its flow normal /
    // roughness so the post-frame SSR chain traces off the water surface (the
    // WaterGBufferRestore no-op pass right after the water passes restores
    // SHADER_RESOURCE; SSRSubsystem is state-aware via RenderGraph::GetTextureState).
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    // Velocity too: without it TAA / SSR temporal reproject water pixels with the motion
    // vectors of whatever the GBuffer pass drew UNDERNEATH (submerged terrain, shoreline
    // grass) - ghosting over water, shimmer at the grass / water edge, reflection seams
    // under vertical camera motion. The plane is static, so its velocity is pure camera
    // reprojection. GBuffer3's graph-end state stays RENDER_TARGET; no restore needed.
    b.WriteRenderTarget(m_velocity);
    // Depth as write: hardware test + depth WRITE (the surface is real geometry for the
    // later depth-aware passes); the HDR RTV is bound by hand. With planes, the depth
    // plane only - water writes no stencil bit; -rgplaneoff keeps the whole-resource
    // write.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Write);
    else                                  b.WriteDepthStencil(m_depth);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds or
    // clears it) and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The pass textures Water.ps samples (bound or named by LightCB; their owners barrier
    // them): the CSM and the per-object map; LAST frame's cloud shadow (CloudPass runs
    // after this pass) and SSR result (the SSR group runs after the graph).
    b.ReadPassTextures({ RG::PassTex::kShadowCSM, RG::PassTex::kShadowPerObject });
    b.ReadPassTextureHistory({ RG::PassTex::kCloudShadow, RG::PassTex::kSSRResult });
}

void WaterPass::Init(IGraphicsDevice& gfx)
{
    // Fully-lit CSM stand-in (R32_FLOAT, arraySize 4), built like TransparentPass /
    // ParticleRenderPass / FaceOverlayPass / WaterSurfacePass's. Water.ps declares
    // gShadowCascades unconditionally and samples it, but the shadow array can be absent
    // (ShadowPass::Init logs an error and returns on allocation failure); binding nothing
    // leaves the slot on whatever descriptor was there - an undefined read, not a black
    // shadow.
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R32_FLOAT;
        desc.width      = 1;
        desc.height     = 1;
        desc.arraySize = 4;   // Texture2DArray SRV needs > 1
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        // arraySize 4 and texel 0.0 are BOTH load-bearing (as for LightingPass::Init's
        // per-object and point-cube fallbacks): the shader declares Texture2DArray, and
        // CreateTexture emits a TEXTURE2DARRAY SRV only when arraySize > 1 - a TEXTURE2D SRV in
        // an array slot is a Tier-3 type mismatch with no validation message. Under the
        // reversed-Z GREATER_EQUAL comparison sampler depth 0.0 is "no occluder" (the shadow
        // passes' clear value); a 1.0 texel is an occluder at the near plane, i.e. FULLY
        // SHADOWED.
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // ONE entry PER SUBRESOURCE: CreateTexture uploads mipLevels * arraySize subresources
        // and indexes this array - a single-element init with arraySize > 1 walks off the
        // stack.
        RHI::SubresourceData init[4];
        for (auto& sd : init) sd = { &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(desc, m_shadowFallbackTex, init))
            m_shadowFallbackHandle = gfx.GetTextureSRVGpuHandle(m_shadowFallbackTex);
        else
            LOG_ERROR("WaterPass: CSM fallback texture failed");
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Water_VS, RHI::ShaderStage::VS, "Water.vs.hlsl");
    m_shaderLib.Register(ShaderID::Water_PS, RHI::ShaderStage::PS, "Water.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_ERROR("WaterPass: PSO creation failed");

    // The heightmap (UV in [0,1]) and the sky cube use the fixed SMP_LINEAR_CLAMP heap
    // slot (cube sampling ignores addressing on the major axis anyway), the CSM cascades
    // SMP_CMP_LINEAR_BORDER_WHITE_GE (reversed-Z GREATER_EQUAL, white border = lit; as
    // LightingPass).
    //
    // The pass's run-time sampler (SetDynamicSampler) is an anisotropic wrap: the
    // flow-normal maps tile across world XZ (many integer UV periods) and the water is
    // seen at grazing angles, where anisotropy keeps the wave detail crisp.
    {
        RHI::SamplerDesc sd{};
        sd.filter         = (m_textureAnisotropy > 1)
                                ? RHI::Filter::ANISOTROPIC
                                : RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU      = RHI::TextureAddressMode::WRAP;
        sd.addressV      = RHI::TextureAddressMode::WRAP;
        sd.addressW      = RHI::TextureAddressMode::WRAP;
        sd.maxAnisotropy = m_textureAnisotropy;
        if (!gfx.CreateSampler(sd, m_wrapSamplerIdx))
            LOG_ERROR("WaterPass: failed to create wrap sampler");
    }

    if (!m_waterCB.Create(gfx, "WaterPass.WaterCB"))
        LOG_ERROR("WaterPass: WaterCB creation failed");

    // 1x1 R16_UNORM zero heightmap - a valid t2 descriptor when there is no terrain.
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R16_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint16_t zero = 0;
        RHI::SubresourceData init{ &zero, 2, 2 };
        if (gfx.CreateTexture(td, m_fallbackHeightmap, &init))
            m_fallbackHeightmapSRV = gfx.GetTextureSRVGpuHandle(m_fallbackHeightmap);
        else
            LOG_ERROR("WaterPass: fallback heightmap creation failed");
    }

    // 1x1 flat tangent-space normal (128,128,255) - calm water while the flow-normal maps
    // load (or when their paths are cleared).
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flat = 0xFFFF8080u;   // ABGR: A=FF B=FF(z) G=80 R=80
        RHI::SubresourceData init{ &flat, 4, 4 };
        if (gfx.CreateTexture(td, m_fallbackFlatNormal, &init))
            m_fallbackFlatNormalSRV = gfx.GetTextureSRVGpuHandle(m_fallbackFlatNormal);
        else
            LOG_ERROR("WaterPass: fallback flat-normal creation failed");
    }

    // 1x1 black SSR-result placeholder while SSR is off (conf reads 0 -> no-op). Not
    // bound to a root slot: its HEAP INDEX is what GetSSRResultSrv() hands the Renderer
    // for LightCB.ssrResultIdx, so it must stay a real Texture2D<float4> - Water.ps OPENS
    // it, and a wrong-dimension descriptor there is UB that Tier 3 never reports.
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t black = 0x00000000u;
        RHI::SubresourceData init{ &black, 4, 4 };
        if (gfx.CreateTexture(td, m_fallbackBlack, &init))
            m_fallbackBlackSRV = gfx.GetTextureSRVGpuHandle(m_fallbackBlack);
        else
            LOG_ERROR("WaterPass: fallback black texture creation failed");
    }

    // 1x1 R16G16 zero ripple-field placeholder (t6) for bodies without an active ripple
    // sim: height 0 + previous height 0 -> zero gradient -> no perturbation.
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R16G16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t zeroRG = 0x00000000u;   // two halfs = 0.0
        RHI::SubresourceData init{ &zeroRG, 4, 4 };
        if (gfx.CreateTexture(td, m_fallbackRipple, &init))
            m_fallbackRippleSRV = gfx.GetTextureSRVGpuHandle(m_fallbackRipple);
        else
            LOG_ERROR("WaterPass: fallback ripple texture creation failed");
    }

    LOG_SUCCESS("WaterPass: initialized");
}

PSODesc WaterPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Water_VS;
    desc.psID        = ShaderID::Water_PS;
    desc.inputLayout = InputLayoutType::None;   // SV_VertexID grid, no IA

    // Cull NONE - the plane must render from below too (underwater camera).
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.fillMode         = RHI::FillMode::SOLID;
    desc.rs.depthClipEnable = true;

    // Reversed-Z test against scene depth; WRITE so clouds/fog see the
    // surface. Shoreline pixels with ~zero alpha are discarded in the PS
    // (clip) so they don't stamp depth / GBuffer either.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;

    // RT layout (SetRenderTargetsAndHdr appends HDR LAST):
    //   RT0 = GBuffer1 normal   (opaque overwrite - SSR trace input)
    //   RT1 = GBuffer2 surface  (opaque overwrite - SSR roughness / Fresnel)
    //   RT2 = GBuffer3 velocity (opaque overwrite - TAA / SSR temporal motion)
    //   RT3 = HDR scene color   (alpha blend - shoreline fade)
    desc.bs.independentBlendEnable = true;
    auto& rtN = desc.bs.renderTarget[0];
    rtN.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rtN.blendEnable             = false;
    auto& rtS = desc.bs.renderTarget[1];
    rtS.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rtS.blendEnable             = false;
    auto& rtV = desc.bs.renderTarget[2];
    rtV.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rtV.blendEnable             = false;
    auto& rt = desc.bs.renderTarget[3];
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rt.blendEnable             = true;
    rt.srcBlend        = RHI::Blend::SRC_ALPHA;
    rt.destBlend       = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp         = RHI::BlendOp::ADD;
    rt.srcBlendAlpha  = RHI::Blend::ONE;
    rt.destBlendAlpha = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha   = RHI::BlendOp::ADD;

    // GBuffer1 normal MUST be _UNORM like the resource ("GBuffer1_Normal") and every
    // other producer's RTV (GBufferPass / Terrain / Grass / OutlineGeometry); FP16's
    // 11-bit mantissa would also corrupt the exact 16-bit material index packed into .a
    // above matIdx 2048.
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_UNORM;   // GBuffer1 normal
    desc.rtvFormats[1] = RHI::Format::R8G8B8A8_UNORM;       // GBuffer2 surface
    desc.rtvFormats[2] = RHI::Format::R16G16_FLOAT;         // GBuffer3 velocity
    desc.rtvFormats[3] = RHI::Format::R16G16B16A16_FLOAT;   // HDR
    desc.rtvCount      = 4;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// Keep in lockstep with Execute's early-outs. Deliberately stops short of the PSO
// check: GetOrCreate can COMPILE a PSO and IsActive must stay a pure query; Execute
// keeps that check.
bool WaterPass::IsActive() const
{
    if (!Shown())             return false;   // the view hides the water
    if (m_instCBs.empty())    return false;   // no water bodies this frame
    if (!m_skyCubeSRV)        return false;   // reflection source not ready
    if (!m_waterCB.IsValid()) return false;   // CB ring never created
    return true;
}

RHI::CommandList WaterPass::Execute(RHI::CommandList cl)
{
    if (!Shown())                return cl;
    if (m_instCBs.empty())       return cl;   // no water bodies this frame
    if (!m_skyCubeSRV)           return cl;   // reflection source not ready
    if (!m_waterCB.IsValid())    return cl;   // CB ring never created

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid()) return cl;

    auto& gfx  = cl.GetDevice();

    const uint32_t count =
        std::min<uint32_t>(static_cast<uint32_t>(m_instCBs.size()), kMaxWaterBodies);

    // Upload this frame's per-body CBs into their 256B ring slots.
    if (auto* frame = m_waterCB.Current(gfx))
        for (uint32_t i = 0; i < count; ++i)
            (*frame)[i] = m_instCBs[i];

    // GBuffer normal + surface + velocity + HDR (appended last) + GBuffer depth; Setup's
    // declarations already emitted every transition.
    cl.SetRenderTargetsAndHdr({ m_normal, m_surface, m_velocity }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // Globals - identical for every body.
    cl.BindCB(kPerViewCBSlot, RG::RGSlot::PerView);     // b1 (VS)
    cl.BindCB(kLightCBSlot, RG::RGSlot::LightCB);       // b3 (sun dir/color + cameraPos)
    cl.BindDescriptorTableHandle(kSkyCubeRootSlot, m_skyCubeSRV);
    // No SSR bind: Water.ps opens the previous-frame result from LightCB.ssrResultIdx
    // (bound just above at b3), which the Renderer derived from GetSSRResultSrv() this
    // frame.
    {   // Never leave t9 unbound - see the fallback note in Init().
        const uint64_t shadowSrv = m_shadowSrvHandle ? m_shadowSrvHandle
                                                     : m_shadowFallbackHandle;
        if (shadowSrv) cl.BindDescriptorTableHandle(kShadowSRVSlot, shadowSrv);
    }
    if (m_wrapSamplerIdx   >= 0) cl.SetDynamicSampler(m_wrapSamplerIdx);   // Water.ps's g_Wrap

    const RHI::GPUBuffer& ringBuf = m_waterCB.CurrentBuffer(gfx);

    // One sub-draw per water body: per-body CB at its 256B offset + per-body
    // heightmap / flow maps / ripple field.
    for (uint32_t i = 0; i < count; ++i)
    {
        const WaterInstanceBindings b =
            (i < m_instBinds.size()) ? m_instBinds[i] : WaterInstanceBindings{};
        const WaterInstanceCB& cb = m_instCBs[i];

        gfx.BindConstantBufferAtOffset(kWaterParamsCBSlot, ringBuf,
            static_cast<uint64_t>(i) * sizeof(WaterInstanceCB), cl);

        cl.BindDescriptorTableHandle(kHeightmapRootSlot,
            b.heightmapSRV ? b.heightmapSRV : m_fallbackHeightmapSRV);
        cl.BindDescriptorTableHandle(kNormalARootSlot,
            b.normalASRV ? b.normalASRV : m_fallbackFlatNormalSRV);
        cl.BindDescriptorTableHandle(kNormalBRootSlot,
            b.normalBSRV ? b.normalBSRV : m_fallbackFlatNormalSRV);
        cl.BindDescriptorTableHandle(kRippleRootSlot,
            b.rippleSRV ? b.rippleSRV : m_fallbackRippleSRV);

        const uint32_t quads = cb.gridQuads ? cb.gridQuads : 1u;
        cl.DrawInstanced(quads * quads * 6u, 1, 0, 0);
    }
    return cl;
}
