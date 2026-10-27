#include "RenderGraph/RenderPass/TranslucentFrontLayerPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MemoryPolicy.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// Root slots (RootSignatureLayout.h is the single source of truth):
//   S::  = absolute root-parameter index (BindRootBufferSRV / BindDescriptorTableHandle)
//   SR:: = group-relative index          (BindCB / BindBufferSRV)
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

TranslucentFrontLayerPass::TranslucentFrontLayerPass(RG::RenderGraph& graph,
                                                     RG::RGTextureHandle normal,
                                                     RG::RGTextureHandle surface,
                                                     RG::RGTextureHandle velocity,
                                                     RG::RGTextureHandle frontDepth,
                                                     RG::RGTextureHandle opaqueDepthSnapshot)
    : m_graph(&graph), m_normal(normal), m_surface(surface), m_velocity(velocity), m_frontDepth(frontDepth)
    , m_opaqueDepthSnapshot(opaqueDepthSnapshot)
{}

void TranslucentFrontLayerPass::Setup(RG::RenderGraphBuilder& b)
{
    // Three private targets + the private depth: the graph emits the SHADER_RESOURCE
    // -> RENDER_TARGET / DEPTHSTENCIL transitions, and the second RTReflectionPass's
    // ReadSRV declarations put them back.
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteDepthStencil(m_frontDepth);
    // The opaque depth snapshot is read BINDLESS by the PS (manual occlusion
    // test); the declaration is what keeps it DEPTH_READ_SRV here.
    b.ReadSRV(m_opaqueDepthSnapshot);
    // MUST be None: SetColorTarget binds AND CLEARS the HDR scene.
    b.SetColorTarget(RG::BuiltinTexture::None);
}

RG::TextureResidency TranslucentFrontLayerPass::Residency()
{
    return MemoryPolicy::EagerTransients() ? RG::TextureResidency::Always : RG::TextureResidency::OnDemand;
}

bool TranslucentFrontLayerPass::EnsureTargets(IGraphicsDevice& gfx)
{
    for (const RG::RGTextureHandle h : { m_normal, m_surface, m_velocity, m_frontDepth })
    {
        m_graph->EnsurePhysicalTexture(h, gfx);
        if (!m_graph->GetPhysicalTexture(h))
            return false;
    }
    return true;
}

void TranslucentFrontLayerPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,     RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    // The ordinary forward PS: the TRANSLUCENT_PREPASS permutation bit (in the
    // surface template's relevant-perm mask) selects the 3-MRT front-layer
    // main inside custom_surface.hlsli, so no new ShaderID is needed.
    m_shaderLib.Register(ShaderID::Transparent_PS, RHI::ShaderStage::PS, "Transparent.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    {
        PermutationKey perm;
        perm.Set(PermutationKey::ALPHA_BLEND, true);
        if (!m_psoCache.GetOrCreate(BuildPSODesc(perm, 0, 0, false)))
            LOG_ERROR("TranslucentFrontLayerPass: base PSO creation failed");
        else
            LOG_INFO("TranslucentFrontLayerPass: base PSO ready");
    }

    // 1x1 white - engine base-color / surface slots (t3/t4).
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t white = 0xFFFFFFFFu;
        RHI::SubresourceData init{ &white, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultWhite, &init))
            m_defaultWhiteGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultWhite);
        else
            LOG_ERROR("TranslucentFrontLayerPass: default white texture failed");
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
            LOG_ERROR("TranslucentFrontLayerPass: flat-normal texture failed");
    }
    // The material sampler (SetDynamicSampler), the same one TransparentPass uses, so
    // the normal / roughness stamped here are the ones the colour draw shades with (mip
    // bias included). The forward IBL sampler is a fixed heap slot: nothing else to
    // create.
    if (!gfx.CreateSampler(MaterialSampler::Desc(m_textureAnisotropy, m_mipBias), m_linearSamplerIdx))
        LOG_ERROR("TranslucentFrontLayerPass: linear sampler failed");
}

void TranslucentFrontLayerPass::WarmPipelines(const DrawPacket& dp)
{
    if (!m_enabled.load(std::memory_order_relaxed) || !IsFrontLayerPacket(dp)) return;
    (void)m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID, dp.customVSID, dp.mirrored != 0));
}

PSODesc TranslucentFrontLayerPass::BuildPSODesc(PermutationKey perm, uint32_t customPSID,
                                                uint32_t customVSID, bool mirrored) const
{
    PSODesc desc;
    desc.vsID = (customVSID != 0) ? static_cast<ShaderID>(customVSID)
                                  : ShaderID::GBuffer_VS;
    desc.psID = (customPSID != 0) ? static_cast<ShaderID>(customPSID)
                                  : ShaderID::Transparent_PS;
    // TRANSLUCENT_PREPASS selects the 3-MRT main; a PSO built from this desc
    // MUST have a 3-target pixel shader or creation fails.
    perm.Set(PermutationKey::TRANSLUCENT_PREPASS, true);
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;   // PVF bindless fetch

    // Same rasterizer decisions as TransparentPass, so the stamp and the colour
    // draw rasterise the SAME fragments (the fold's depth-equality test relies
    // on it): cull per DOUBLE_SIDED, front-face flip for mirrored instances.
    desc.rs.cullMode = perm.Has(PermutationKey::DOUBLE_SIDED)
                        ? RHI::CullMode::NONE : RHI::CullMode::BACK;
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.fillMode         = RHI::FillMode::SOLID;   // never active in Wireframe
    desc.rs.depthClipEnable = true;

    // Reverse-Z test against the PRIVATE depth (cleared to sky) + WRITE: the
    // nearest translucent layer wins; opaque occlusion is the PS's own test.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;

    // Data targets: opaque overwrite on every RT, no blending.
    desc.bs.independentBlendEnable = false;
    for (int i = 0; i < 3; ++i)
    {
        auto& rt = desc.bs.renderTarget[i];
        rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        rt.blendEnable             = false;
    }

    // Must match the graph textures (the Renderer creates them in the GBuffer formats;
    // GBuffer1 is _UNORM because .a carries an exact 16-bit matIdx).
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_UNORM;   // TranslucentFront_Normal
    desc.rtvFormats[1] = RHI::Format::R8G8B8A8_UNORM;       // TranslucentFront_Surface
    desc.rtvFormats[2] = RHI::Format::R16G16_FLOAT;         // TranslucentFront_Velocity
    desc.rtvCount      = 3;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

RHI::CommandList TranslucentFrontLayerPass::Execute(RHI::CommandList cl)
{
    // Clear FIRST, draws or not: the second RTReflectionPass and the colour
    // pass read these every frame the pass is active, and a stale stamp would
    // be a stale reflection on a pane that is no longer there.
    cl.SetRenderTargets({ m_normal, m_surface, m_velocity }, m_frontDepth);
    {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cl.ClearRenderTarget(m_normal,   zero);
        cl.ClearRenderTarget(m_surface,  zero);
        cl.ClearRenderTarget(m_velocity, zero);
        cl.ClearDepthStencil(m_frontDepth);   // reverse-Z: 0 = sky / no layer
    }

    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    if (draws.empty()) return cl;

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const RHI::PipelineState* activePSO = nullptr;

    // Everything the VS + the material hook can touch is rebound here (root
    // bindings persist along the CL and the neighbouring passes leave their
    // own descriptors in these slots). The lighting tables the forward include
    // closure DECLARES (IBL cubes, BRDF LUT, sky SH, CSM, probes) are not bound:
    // the front-layer main never calls MS_ForwardLighting, DXC strips the reads,
    // and D3D12 only validates descriptors a draw actually accesses.
    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
        cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);

        cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);   // VS + gFrameIndex/time
        cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);   // fwdReflDepthIdx (opaque test)
        {
            const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer);
            if (matBuf && matBuf->IsValid())
                cl.BindBufferSRV(SR::SRV_t2, *matBuf);
        }
        if (m_linearSamplerIdx >= 0) cl.SetDynamicSampler(m_linearSamplerIdx);   // the material sampler (sampler_heap.hlsli SmpDynamic)

        if (m_defaultWhiteGpuHandle)
        {
            cl.BindDescriptorTableHandle(S::SRV_t3, m_defaultWhiteGpuHandle);
            cl.BindDescriptorTableHandle(S::SRV_t4, m_defaultWhiteGpuHandle);
        }
        if (m_defaultFlatNormalGpuHandle)
            cl.BindDescriptorTableHandle(S::SRV_t5, m_defaultFlatNormalGpuHandle);

        // t0 space2 - g_AllTextures (material texture ids + the bindless opaque-depth
        // read).
        if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
            cl.BindDescriptorTableHandle(S::BindlessTex, texTable);
    };

    // The front stamp uses the same graphics root signature for every PSO.
    // Resolve each variant once per Execute, and preserve unchanged root
    // bindings across draws. Frame-local pointers remain safe across reloads.
    struct CachedPSO { uint32_t perm, ps, vs; bool mirrored; const RHI::PipelineState* pso; };
    std::vector<CachedPSO> framePSOs;
    framePSOs.reserve(16);
    bool globalsBound = false;
    uint64_t lastBC = 0, lastSM = 0, lastNM = 0, lastCB = 0, lastTex = 0;
    for (const DrawPacket& dp : draws)
    {
        if (!IsFrontLayerPacket(dp)) continue;

        const RHI::PipelineState* pso = nullptr;
        bool cached = false;
        for (const auto& entry : framePSOs)
            if (entry.perm == dp.permutation.bits && entry.ps == dp.customPSID &&
                entry.vs == dp.customVSID && entry.mirrored == (dp.mirrored != 0))
            { pso = entry.pso; cached = true; break; }
        if (!cached)
        {
            pso = m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID,
                                                    dp.customVSID, dp.mirrored != 0));
            framePSOs.push_back({dp.permutation.bits, dp.customPSID, dp.customVSID, dp.mirrored != 0, pso});
        }
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            if (!globalsBound) { bindGlobals(); globalsBound = true; }
        }

        {
            const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
            const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
            const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
            if (bc && bc != lastBC) { cl.BindDescriptorTableHandle(S::SRV_t3, bc); lastBC = bc; }
            if (sm && sm != lastSM) { cl.BindDescriptorTableHandle(S::SRV_t4, sm); lastSM = sm; }
            if (nm && nm != lastNM) { cl.BindDescriptorTableHandle(S::SRV_t5, nm); lastNM = nm; }
        }

        // Custom material CBV (b8 space0) + texture table (space3): the hook reads the
        // same authored knobs it reads in the colour pass.
        if (dp.customCbvVA && dp.customCbvVA != lastCB)
        { cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl); lastCB = dp.customCbvVA; }
        if (dp.customTexTable && dp.customTexTable != lastTex)
        { cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl); lastTex = dp.customTexTable; }

        // All four PVF root constants in one call (see GBufferPass's driver note);
        // prevPosElementBase feeds the velocity this pass stamps.
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
void TranslucentFrontLayerPass::SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW)
{
    MaterialSampler::Rebuild(gfx, m_textureAnisotropy,
                             MaterialSampler::Bias(MaterialSampler::kNativeBias, renderW, displayW),
                             m_linearSamplerIdx, m_mipBias);
}
