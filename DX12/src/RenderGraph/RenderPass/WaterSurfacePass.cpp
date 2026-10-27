#include "RenderGraph/RenderPass/WaterSurfacePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/SpotShadowView.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// Root slots - the single source of truth is RootSignatureLayout.h.
//   S::  = ABSOLUTE root-parameter index (BindRootBufferSRV / BindDescriptorTableHandle)
//   SR:: = GROUP-RELATIVE index          (BindCB / BindBufferSRV)
// Passing an absolute index to a group-relative bind silently binds NOTHING.
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

WaterSurfacePass::WaterSurfacePass(RG::RGTextureHandle normal,
                                   RG::RGTextureHandle surface,
                                   RG::RGTextureHandle velocity,
                                   RG::RGTextureHandle depth,
                                   RG::RGTextureHandle sceneDepthSnapshot,
                                   RG::RGTextureHandle refractionSource)
    : m_normal(normal), m_surface(surface), m_velocity(velocity), m_depth(depth)
    , m_sceneDepthSnapshot(sceneDepthSnapshot), m_refractionSource(refractionSource)
{}

void WaterSurfacePass::Setup(RG::RenderGraphBuilder& b)
{
    // The same declarations as WaterPass: the graph emits the SHADER_RESOURCE ->
    // RENDER_TARGET transitions from these, and the WaterGBufferRestore no-op later
    // declares ReadSRV on normal / surface to put them back before the SSR / XeGTAO /
    // post chain runs.
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    // Velocity is NOT optional: without it TAA and the SSR temporal pass reproject water
    // pixels with the motion vectors of whatever the GBuffer drew underneath (submerged
    // ground, shoreline grass) -> ghosting on the water and shimmer at the waterline.
    b.WriteRenderTarget(m_velocity);
    // Depth test AND write - the water surface is real geometry for the depth-aware
    // passes that follow (clouds, height fog, volumetrics). With planes, the depth plane
    // only - no stencil bit; -rgplaneoff declares WriteDepthStencil.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Write);
    else                                  b.WriteDepthStencil(m_depth);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds or
    // clears it) and bound in Execute via SetRenderTargetsAndHdr. The colour target MUST
    // stay None: SetColorTarget binds AND CLEARS, and would wipe the HDR scene the sky
    // just drew.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);

    // Read-only inputs for water depth + refraction; neither is optional:
    //   * SSRTraceDepth - the opaque depth snapshot: the water depth-tests the MAIN depth
    //     buffer and SAMPLES this copy in the same draw (the engine has no read-only DSV).
    //   * ReflectionSource_NoTrace - the lit opaque scene, for refraction. Only this
    //     declaration gives it an SRV (see m_sceneDepthSnapshot); without it the bindless
    //     index silently points at an unrelated descriptor.
    b.ReadSRV(m_sceneDepthSnapshot);
    b.ReadSRV(m_refractionSource);
    // The pass textures the water main samples (bound or named by LightCB; their
    // producers barrier them: Owner): the CSM, the spot / point shadows of water_common's
    // light loop, the per-object map (on for water), the RT sun mask and the sky's
    // specular cube; no AO or SSGI (water's proximity gate is 0 - nothing is sampled). The
    // cloud shadow is LAST frame's - CloudPass runs after this pass.
    b.ReadPassTextures({ RG::PassTex::kShadowCSM, RG::PassTex::kShadowSpotAtlas,
                         RG::PassTex::kShadowSpotAtlasMedium, RG::PassTex::kShadowSpotAtlasHigh,
                         RG::PassTex::kShadowPointCube, RG::PassTex::kShadowPerObject,
                         RG::PassTex::kRTShadowMask, RG::PassTex::kSkySpecularCube });
    b.ReadPassTextureHistory({ RG::PassTex::kCloudShadow });
}

void WaterSurfacePass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,      RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::WaterSurface_PS, RHI::ShaderStage::PS, "WaterSurface.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Warm the default water PSO so the first lake does not hitch.
    {
        PermutationKey perm;
        perm.Set(PermutationKey::WATER_SURFACE, true);
        if (!m_psoCache.GetOrCreate(BuildPSODesc(perm, 0, 0, false)))
            LOG_ERROR("WaterSurfacePass: base PSO creation failed");
        else
            LOG_INFO("WaterSurfacePass: base PSO ready");
    }

    // 1x1 white - engine base-color / surface slots (t3 / t4).
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t white = 0xFFFFFFFFu;
        RHI::SubresourceData init{ &white, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultWhite, &init))
            m_defaultWhiteGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultWhite);
        else
            LOG_ERROR("WaterSurfacePass: default white texture failed");
    }

    // 1x1 flat tangent normal (128,128,255) - engine normal slot (t5).
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flat = 0xFFFF8080u;
        RHI::SubresourceData init{ &flat, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultFlatNormal, &init))
            m_defaultFlatNormalGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultFlatNormal);
        else
            LOG_ERROR("WaterSurfacePass: flat-normal texture failed");
    }

    // 1x1 fully-lit CSM stand-in (see m_shadowFallbackTex): a missing shadow map degrades
    // to "no shadow" instead of garbage.
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
            LOG_ERROR("WaterSurfacePass: CSM fallback texture failed");
    }

    // The spot-shadow VP fallback (t22): one entry with atlasIndex ~0 ("no atlas",
    // SampleSpotShadow returns lit), built like TransparentPass's and LightingPass's, so
    // the table is never unbound under WaterLocalLights. Renderer-lifetime, like the CSM
    // fallback.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(SpotShadowViewGPU);
        bd.stride     = sizeof(SpotShadowViewGPU);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        SpotShadowViewGPU none;
        DirectX::XMStoreFloat4x4(&none.vp, DirectX::XMMatrixIdentity());
        if (gfx.CreateBuffer(bd, m_spotShadowVPFallbackBuf, &none))
            m_spotShadowVPFallback = gfx.GetBufferSRVGpuHandle(m_spotShadowVPFallbackBuf);
        else
            LOG_ERROR("WaterSurfacePass: spot-shadow VP fallback failed");
    }

    // The material sampler (SetDynamicSampler) - aniso wrap, as GBufferPass /
    // TransparentPass, so water's material textures integrate with TAA like opaque
    // surfaces. The IBL (and scene-colour / ripple) and CSM comparison samplers are fixed
    // heap slots that custom_surface.hlsli names.
    if (!gfx.CreateSampler(MaterialSampler::Desc(m_textureAnisotropy, m_mipBias), m_linearSamplerIdx))
        LOG_ERROR("WaterSurfacePass: linear sampler failed");
}

PSODesc WaterSurfacePass::BuildPSODesc(PermutationKey perm, uint32_t customPSID,
                                       uint32_t customVSID, bool mirrored) const
{
    PSODesc desc;
    desc.vsID = (customVSID != 0) ? static_cast<ShaderID>(customVSID)
                                  : ShaderID::GBuffer_VS;
    desc.psID = (customPSID != 0) ? static_cast<ShaderID>(customPSID)
                                  : ShaderID::WaterSurface_PS;
    // WATER_SURFACE is what selects the 4-MRT main inside custom_surface.hlsli.
    // ApplyBlendMode already set it, but force it here too: a PSO built from
    // this desc MUST have a 4-target pixel shader or PSO creation fails.
    perm.Set(PermutationKey::WATER_SURFACE, true);
    perm.Set(PermutationKey::DEBUG_SHADE, m_debugShade);
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;   // PVF bindless fetch

    desc.rs.cullMode = perm.Has(PermutationKey::DOUBLE_SIDED)
                        ? RHI::CullMode::NONE : RHI::CullMode::BACK;
    // Mirrored (negative-scale) instances reverse the winding - flip the front-face
    // convention rather than the cull mode, so SV_IsFrontFace stays meaningful for the
    // DOUBLE_SIDED normal flip.
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.fillMode         = m_wireframeMode ? RHI::FillMode::WIREFRAME
                                                : RHI::FillMode::SOLID;
    desc.rs.depthClipEnable = true;

    // Reverse-Z test, and WRITE so clouds/fog treat the surface as geometry.
    // Shoreline pixels are clip()ed in the PS so they stamp neither depth nor
    // the GBuffer.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;

    // RT layout - SetRenderTargetsAndHdr appends the HDR target LAST.
    //   RT0 GBuffer1 normal   (opaque overwrite)
    //   RT1 GBuffer2 surface  (opaque overwrite)
    //   RT2 GBuffer3 velocity (opaque overwrite)
    //   RT3 HDR colour        (alpha blend - the shoreline fade)
    desc.bs.independentBlendEnable = true;
    for (int i = 0; i < 3; ++i)
    {
        auto& rt = desc.bs.renderTarget[i];
        rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        rt.blendEnable             = false;
    }
    {
        // RT3's blend state follows the material's Blend Mode, so a water material is as
        // controllable as any other transparent one. The three GBuffer stamps stay opaque
        // overwrites in every mode - they are data for the reflection / TAA chain, not colour.
        auto& rt = desc.bs.renderTarget[3];
        rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        rt.blendEnable     = true;
        rt.blendOp         = RHI::BlendOp::ADD;
        rt.srcBlendAlpha  = RHI::Blend::ONE;
        rt.destBlendAlpha = RHI::Blend::INV_SRC_ALPHA;
        rt.blendOpAlpha   = RHI::BlendOp::ADD;

        if (perm.Has(PermutationKey::ADDITIVE_BLEND))
        {
            // The water main premultiplies by opacity for this mode, so the
            // Opacity output still fades the effect out.
            rt.srcBlend  = RHI::Blend::ONE;
            rt.destBlend = RHI::Blend::ONE;
        }
        else if (perm.Has(PermutationKey::PREMULTIPLIED_BLEND))
        {
            rt.srcBlend  = RHI::Blend::ONE;
            rt.destBlend = RHI::Blend::INV_SRC_ALPHA;
        }
        else
        {
            // Straight alpha - the default, and what ordinary water uses; the shoreline fade
            // comes from the hook's s.opacity.
            rt.srcBlend  = RHI::Blend::SRC_ALPHA;
            rt.destBlend = RHI::Blend::INV_SRC_ALPHA;
        }
    }

    // MUST match the actual resources: GBuffer1 is _UNORM (its alpha carries an exact
    // 16-bit material index; FP16 would corrupt it above slot 2048).
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_UNORM;   // GBuffer1 normal
    desc.rtvFormats[1] = RHI::Format::R8G8B8A8_UNORM;       // GBuffer2 surface
    desc.rtvFormats[2] = RHI::Format::R16G16_FLOAT;         // GBuffer3 velocity
    desc.rtvFormats[3] = RHI::Format::R16G16B16A16_FLOAT;   // HDR
    desc.rtvCount      = 4;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// Keep in lockstep with Execute's early-outs. Deliberately stops short of the PSO
// check: GetOrCreate can COMPILE, and IsActive must stay a pure query.
//
// No sky-cube gate: water reflects through gRadiance / probes, and a WorldEnvironment
// with no sky is legitimate - gating on it would make water silently vanish in exactly
// those scenes.
bool WaterSurfacePass::IsActive() const
{
    return m_waterDrawCount > 0;
}

RHI::CommandList WaterSurfacePass::Execute(RHI::CommandList cl)
{
    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Water);
    if (draws.empty()) return cl;

    // GBuffer normal + surface + velocity + HDR (appended last) + depth. All the
    // transitions were emitted from the Setup declarations.
    cl.SetRenderTargetsAndHdr({ m_normal, m_surface, m_velocity }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const RHI::PipelineState* activePSO = nullptr;

    // Every slot this pass reads is rebound here. Root bindings persist along the command
    // list, so the neighbouring passes (WaterPass before, Cloud / HeightFog after) leave
    // their own descriptors in these slots - inheriting any would bind the wrong resource,
    // with the symptom in a DIFFERENT pass.
    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
        cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
        // t0 space1 - the PVF vertex-fetch buffers; GBuffer.vs.hlsl cannot fetch a single
        // vertex without them.
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);

        cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);   // VS
        cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);   // PS - sun, IBL, water rows
        {
            const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer);
            if (matBuf && matBuf->IsValid())
                cl.BindBufferSRV(SR::SRV_t2, *matBuf);
        }

        // The material sampler (sampler_heap.hlsli SmpDynamic).
        if (m_linearSamplerIdx >= 0) cl.SetDynamicSampler(m_linearSamplerIdx);

        if (m_iblIrradianceHandle) cl.BindDescriptorTableHandle(S::IBLIrradiance, m_iblIrradianceHandle);
        if (m_iblRadianceHandle)   cl.BindDescriptorTableHandle(S::IBLRadiance,   m_iblRadianceHandle);
        if (m_brdfLutHandle)       cl.BindDescriptorTableHandle(S::BRDFLUT,       m_brdfLutHandle);
        if (m_skySHHandle)         cl.BindDescriptorTableHandle(S::SkySH,         m_skySHHandle);
        // CSM - the sun glint and the sun-aligned lobe of the reflection are direct light and
        // must vanish where terrain shadows the surface. UNCONDITIONAL: MS_ForwardLighting
        // samples t9 on every draw, so an unbound table is UB; the 1x1 fully-lit stand-in
        // covers the frames before ShadowPass has an array.
        {
            const uint64_t shadowSrv = m_shadowSrvHandle ? m_shadowSrvHandle
                                                         : m_shadowFallbackHandle;
            if (shadowSrv) cl.BindDescriptorTableHandle(S::ShadowSRV, shadowSrv);
        }
        // Clustered local lights. water_common.hlsli's WaterLocalLights() early-
        // outs on clusterLightCount == 0, so an unbound table is never read in a
        // scene with no point/spot lights.
        if (m_clusterLightsSRV)    cl.BindDescriptorTableHandle(S::ClusterLights,    m_clusterLightsSRV);
        if (m_clusterIndexListSRV) cl.BindDescriptorTableHandle(S::ClusterIndexList, m_clusterIndexListSRV);
        if (m_clusterGridSRV)      cl.BindDescriptorTableHandle(S::ClusterGrid,      m_clusterGridSRV);
        // The lights' spot-shadow views (t22), or the "no atlas" fallback - never
        // unbound under WaterLocalLights, which reads it for a light owning a slice.
        {
            const uint64_t vps = m_spotShadowVPHandle ? m_spotShadowVPHandle : m_spotShadowVPFallback;
            if (vps) cl.BindDescriptorTableHandle(S::SpotShadowVP, vps);
        }

        // Engine texture slots seeded so they are never unbound.
        if (m_defaultWhiteGpuHandle)
        {
            cl.BindDescriptorTableHandle(S::SRV_t3, m_defaultWhiteGpuHandle);
            cl.BindDescriptorTableHandle(S::SRV_t4, m_defaultWhiteGpuHandle);
        }
        if (m_defaultFlatNormalGpuHandle)
            cl.BindDescriptorTableHandle(S::SRV_t5, m_defaultFlatNormalGpuHandle);

        // t0 space2 - g_AllTextures: water_common.hlsli reaches the scene depth snapshot, the
        // refraction source and the ripple fields through the bindless heap, and the default
        // shader samples its normal map from it.
        if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
            cl.BindDescriptorTableHandle(S::BindlessTex, texTable);
    };

    for (const DrawPacket& dp : draws)
    {
        // Re-resolved every frame on purpose: a shader hot-reload calls
        // PSOCache::Clear(), so a cached PipelineState* held across frames would
        // dangle and crash on the next SetPipelineState.
        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID,
                                                dp.customVSID, dp.mirrored != 0));
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            bindGlobals();
        }

        // Per-draw engine texture slots.
        {
            const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
            const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
            const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
            if (bc) cl.BindDescriptorTableHandle(S::SRV_t3, bc);
            if (sm) cl.BindDescriptorTableHandle(S::SRV_t4, sm);
            if (nm) cl.BindDescriptorTableHandle(S::SRV_t5, nm);
        }

        // The material's authored knobs (b8 space0) and textures (space3) - what makes "the
        // custom shader picks the flow map" work. Packed by BuildRenderScene phases E and F,
        // filter-agnostic, so water gets them with no extra plumbing.
        if (dp.customCbvVA)    cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl);
        if (dp.customTexTable) cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl);

        // All four PVF root constants in ONE call - splitting into 3+1 hits a driver corner
        // case documented in GBufferPass. prevPosElementBase must be included, or the VS reads
        // a stale value left by an earlier pass and the velocity goes wrong.
        {
            uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.prevPosElementBase
            };
            cl.gfx->SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
        }
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }

    return cl;
}

// The material sampler's LOD bias for the current render / display widths.
void WaterSurfacePass::SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW)
{
    MaterialSampler::Rebuild(gfx, m_textureAnisotropy,
                             MaterialSampler::Bias(MaterialSampler::kNativeBias, renderW, displayW),
                             m_linearSamplerIdx, m_mipBias);
}
