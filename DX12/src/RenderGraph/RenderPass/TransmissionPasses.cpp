#include "RenderGraph/RenderPass/TransmissionPasses.h"
#include "Graphics/BindlessKind.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MaterialSampler.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "RenderGraph/RenderContext.h"
#include "Settings/PassSettings.h"
#include "System/Log.h"

#include <cmath>
#include <vector>

// Root slots (RootSignatureLayout.h is the single source of truth):
//   S::  = absolute root-parameter index (BindRootBufferSRV / BindDescriptorTableHandle)
//   SR:: = group-relative index          (BindCB / BindBufferSRV)
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

namespace
{
RG::FullscreenDraw ApplyDraw(RG::RGTextureHandle depth)
{
    return { .ps      = ShaderID::TransmissionApply_PS,
             .target  = RG::FullscreenTarget::HdrWithDepth(depth),
             .blend   = RG::FullscreenBlend::Opaque,
             .gpuZone = "Transmission.Apply" };
}
}

// ==== TransmissionPrepass ====

TransmissionPrepass::TransmissionPrepass(RG::RGTextureHandle depth, RG::RGTextureHandle params,
                                         RG::RGTextureHandle tint, RG::RGTextureHandle layerDepth)
    : m_depth(depth), m_params(params), m_tint(tint), m_layerDepth(layerDepth)
{}

void TransmissionPrepass::Setup(RG::RenderGraphBuilder& b)
{
    // The GBuffer depth occludes through a read-only test, as in TransparentPass.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Read);
    else                                  b.WriteDepthStencil(m_depth);
    b.WriteRenderTarget(m_params);
    b.WriteRenderTarget(m_tint);
    b.WriteRenderTarget(m_layerDepth);
    // Must be None: SetColorTarget binds AND clears the HDR scene.
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void TransmissionPrepass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,     RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    // The ordinary forward PS: the TRANSMISSION_PREPASS bit selects the 3-target main in
    // custom_surface.hlsli, so no new ShaderID; custom materials reach it through their
    // dynamic ids.
    m_shaderLib.Register(ShaderID::Transparent_PS, RHI::ShaderStage::PS, "Transparent.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PermutationKey perm;
        perm.Set(PermutationKey::TRANSMISSIVE_BLEND, true);
        if (!m_psoCache.GetOrCreate(BuildPSODesc(perm, 0, 0, false)))
            LOG_ERROR("TransmissionPrepass: base PSO creation failed");
    }

    // 1x1 white (base colour / surface slots t3, t4) and a flat tangent normal (t5).
    auto makeTexel = [&](uint32_t texel, RHI::Texture& tex, uint64_t& handle, const char* what)
    {
        RHI::TextureDesc desc;
        desc.format    = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        RHI::SubresourceData init{ &texel, 4, 4 };
        if (gfx.CreateTexture(desc, tex, &init)) handle = gfx.GetTextureSRVGpuHandle(tex);
        else LOG_ERROR("TransmissionPrepass: %s texture failed", what);
    };
    makeTexel(0xFFFFFFFFu, m_defaultWhite, m_defaultWhiteGpuHandle, "default white");
    makeTexel(0xFFFF8080u, m_defaultFlatNormal, m_defaultFlatNormalGpuHandle, "flat normal");

    if (!gfx.CreateSampler(MaterialSampler::Desc(8, MaterialSampler::kNativeBias), m_linearSamplerIdx))
        LOG_ERROR("TransmissionPrepass: material sampler failed");
}

PSODesc TransmissionPrepass::BuildPSODesc(PermutationKey perm, uint32_t customPSID,
                                          uint32_t customVSID, bool mirrored) const
{
    PSODesc desc;
    desc.vsID = (customVSID != 0) ? static_cast<ShaderID>(customVSID) : ShaderID::GBuffer_VS;
    desc.psID = (customPSID != 0) ? static_cast<ShaderID>(customPSID) : ShaderID::Transparent_PS;
    // The 3-target main; a PSO built from this desc must have it.
    perm.Set(PermutationKey::TRANSMISSION_PREPASS, true);
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;   // PVF bindless fetch

    // The colour draw's rasterizer, so both cover the same fragments.
    desc.rs.cullMode              = perm.Has(PermutationKey::DOUBLE_SIDED) ? RHI::CullMode::NONE
                                                                           : RHI::CullMode::BACK;
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.fillMode              = RHI::FillMode::SOLID;
    desc.rs.depthClipEnable       = true;

    desc.dss.depthEnable    = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc      = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed Z

    // Commutative blends, so the layers need no order: offsets and spreads add, tints
    // multiply, the depth keeps the nearest (reverse Z: the largest).
    desc.bs.independentBlendEnable = true;
    auto& params = desc.bs.renderTarget[0];
    params.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    params.blendEnable    = true;
    params.srcBlend       = RHI::Blend::ONE;
    params.destBlend      = RHI::Blend::ONE;
    params.blendOp        = RHI::BlendOp::ADD;
    params.srcBlendAlpha  = RHI::Blend::ONE;
    params.destBlendAlpha = RHI::Blend::ONE;
    params.blendOpAlpha   = RHI::BlendOp::ADD;
    auto& tint = desc.bs.renderTarget[1];
    tint.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    tint.blendEnable    = true;
    tint.srcBlend       = RHI::Blend::ZERO;
    tint.destBlend      = RHI::Blend::SRC_COLOR;
    tint.blendOp        = RHI::BlendOp::ADD;
    tint.srcBlendAlpha  = RHI::Blend::ZERO;
    tint.destBlendAlpha = RHI::Blend::ONE;
    tint.blendOpAlpha   = RHI::BlendOp::ADD;
    auto& nearest = desc.bs.renderTarget[2];
    nearest.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    nearest.blendEnable    = true;
    nearest.srcBlend       = RHI::Blend::ONE;
    nearest.destBlend      = RHI::Blend::ONE;
    nearest.blendOp        = RHI::BlendOp::MAX;
    nearest.srcBlendAlpha  = RHI::Blend::ONE;
    nearest.destBlendAlpha = RHI::Blend::ONE;
    nearest.blendOpAlpha   = RHI::BlendOp::MAX;

    desc.rtvFormats[0] = kParamsFormat;
    desc.rtvFormats[1] = kTintFormat;
    desc.rtvFormats[2] = kLayerDepthFormat;
    desc.rtvCount      = 3;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

RHI::CommandList TransmissionPrepass::Execute(RHI::CommandList cl)
{
    // The targets are on demand (TransmissionFeature backs them): nothing to write before.
    RG::RenderContext& ctx = cl.GetContext();
    if (!m_active || !ctx.GetTexture(m_params) || !ctx.GetTexture(m_tint) || !ctx.GetTexture(m_layerDepth))
        return cl;
    // Cleared first, draws or not: the apply reads all three every frame it runs.
    cl.SetRenderTargets({ m_params, m_tint, m_layerDepth }, m_depth);
    {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float one[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
        cl.ClearRenderTarget(m_params, zero);
        cl.ClearRenderTarget(m_tint, one);
        cl.ClearRenderTarget(m_layerDepth, zero);   // reverse Z: 0 = no layer
    }
    m_ran = true;

    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    if (draws.empty()) return cl;

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
        cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
        if (bindlessHandle) cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);
        cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);   // VS, the PS's view-projection and camera
        cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);   // a hook may read it
        if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer);
            matBuf && matBuf->IsValid())
            cl.BindBufferSRV(SR::SRV_t2, *matBuf);
        if (m_linearSamplerIdx >= 0) cl.SetDynamicSampler(m_linearSamplerIdx);
        if (m_defaultWhiteGpuHandle)
        {
            cl.BindDescriptorTableHandle(S::SRV_t3, m_defaultWhiteGpuHandle);
            cl.BindDescriptorTableHandle(S::SRV_t4, m_defaultWhiteGpuHandle);
        }
        if (m_defaultFlatNormalGpuHandle) cl.BindDescriptorTableHandle(S::SRV_t5, m_defaultFlatNormalGpuHandle);
        if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
            cl.BindDescriptorTableHandle(S::BindlessTex, texTable);
    };

    struct CachedPSO { uint32_t perm, ps, vs; bool mirrored; const RHI::PipelineState* pso; };
    std::vector<CachedPSO> framePSOs;
    framePSOs.reserve(8);
    const RHI::PipelineState* activePSO = nullptr;
    bool globalsBound = false;
    uint64_t lastBC = 0, lastSM = 0, lastNM = 0, lastCB = 0, lastTex = 0;
    for (const DrawPacket& dp : draws)
    {
        if (!IsTransmissivePacket(dp)) continue;

        const RHI::PipelineState* pso = nullptr;
        bool cached = false;
        for (const CachedPSO& entry : framePSOs)
            if (entry.perm == dp.permutation.bits && entry.ps == dp.customPSID &&
                entry.vs == dp.customVSID && entry.mirrored == (dp.mirrored != 0))
            { pso = entry.pso; cached = true; break; }
        if (!cached)
        {
            pso = m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID, dp.customVSID,
                                                      dp.mirrored != 0));
            framePSOs.push_back({ dp.permutation.bits, dp.customPSID, dp.customVSID, dp.mirrored != 0, pso });
        }
        if (!pso || !pso->IsValid()) continue;
        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            if (!globalsBound) { bindGlobals(); globalsBound = true; }
        }

        const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
        const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
        const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
        if (bc && bc != lastBC) { cl.BindDescriptorTableHandle(S::SRV_t3, bc); lastBC = bc; }
        if (sm && sm != lastSM) { cl.BindDescriptorTableHandle(S::SRV_t4, sm); lastSM = sm; }
        if (nm && nm != lastNM) { cl.BindDescriptorTableHandle(S::SRV_t5, nm); lastNM = nm; }
        if (dp.customCbvVA && dp.customCbvVA != lastCB)
        { cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl); lastCB = dp.customCbvVA; }
        if (dp.customTexTable && dp.customTexTable != lastTex)
        { cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl); lastTex = dp.customTexTable; }

        cl.gfx->SetRootConstants4(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex,
                                  dp.prevPosElementBase, cl);
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }
    return cl;
}

// ==== TransmissionApplyPass ====

TransmissionApplyPass::TransmissionApplyPass(RG::RenderGraph& graph, RG::RGTextureHandle depth,
                                             RG::RGTextureHandle params, RG::RGTextureHandle tint,
                                             RG::RGTextureHandle layerDepth)
    : m_graph(graph), m_depth(depth), m_params(params), m_tint(tint), m_layerDepth(layerDepth)
{}

void TransmissionApplyPass::Setup(RG::RenderGraphBuilder& b)
{
    // Read bindless by the apply shader: declared so the graph keeps them shader-readable.
    b.ReadSRV(m_params);
    b.ReadSRV(m_tint);
    b.ReadSRV(m_layerDepth);
    b.ReadSRV(m_depth);
    // The fullscreen target binds the depth with the test off (HeightFogPass's pattern).
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::None);
    // The HDR scene: copied first (CopyHdrSceneToScoped wants RENDER_TARGET), then written.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void TransmissionApplyPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS,   RHI::ShaderStage::VS, "VolumetricApply.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::TransmissionApply_PS, RHI::ShaderStage::PS, "TransmissionApply.ps.hlsl", "main");
    m_psoCache.Init(gfx, m_shaderLib);
    if (!m_psoCache.GetOrCreate(RG::FullscreenPipelineDesc(ApplyDraw(m_depth), gfx.GetHdrSceneColorFormat(),
                                                           RHI::Format::D32_FLOAT_S8X24_UINT)))
        LOG_ERROR("TransmissionApplyPass: apply PSO failed");
    m_pyramid.Init(gfx);
}

bool TransmissionApplyPass::EnsureTargets(IGraphicsDevice& gfx)
{
    uint32_t w = 0, h = 0;
    if (!gfx.GetHdrSceneSize(w, h) || w == 0 || h == 0) return false;
    if (!m_snapshot.IsValid() || m_snapshotW != w || m_snapshotH != h)
    {
        if (m_snapshot.IsValid()) gfx.DestroyTexture(m_snapshot);
        m_snapshotW = m_snapshotH = 0;
        // CopyHdrSceneToScoped is a whole-resource copy: the HDR target's size and format.
        RHI::TextureDesc desc{};
        desc.width      = w;
        desc.height     = h;
        desc.format     = gfx.GetHdrSceneColorFormat();
        desc.bindFlags  = RHI::BindFlag::SHADER_RESOURCE;
        desc.usage      = RHI::Usage::DEFAULT;
        desc.layout     = RHI::ResourceState::COPY_DST;
        desc.debugName  = "TransmissionSceneSnapshot";
        if (!gfx.CreateTexture(desc, m_snapshot))
        {
            LOG_ERROR("TransmissionApplyPass: scene snapshot %ux%u failed", w, h);
            return false;
        }
        m_snapshotState = RHI::ResourceState::COPY_DST;
        m_snapshotW = w;
        m_snapshotH = h;
    }
    m_pyramid.EnsureTexture(w, h);
    return m_pyramid.GetTexture()->IsValid();
}

RHI::CommandList TransmissionApplyPass::Execute(RHI::CommandList cl)
{
    if (!m_active || !m_snapshot.IsValid()) return cl;
    IGraphicsDevice& gfx = cl.GetDevice();

    // The scene before translucency, and its mip chain for the rough lobes.
    if (m_snapshotState != RHI::ResourceState::COPY_DST)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_snapshot, m_snapshotState, RHI::ResourceState::COPY_DST), cl);
    gfx.CopyHdrSceneToScoped(m_snapshot, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_snapshot, RHI::ResourceState::COPY_DST,
                                           RHI::ResourceState::SHADER_RESOURCE), cl);
    m_snapshotState = RHI::ResourceState::SHADER_RESOURCE;
    m_pyramid.Execute(cl, &m_snapshot);

    const RHI::Texture* params     = m_graph.GetPhysicalTexture(m_params);
    const RHI::Texture* tint       = m_graph.GetPhysicalTexture(m_tint);
    const RHI::Texture* layerDepth = m_graph.GetPhysicalTexture(m_layerDepth);
    const RHI::Texture* depth      = m_graph.GetPhysicalTexture(m_depth);
    const uint64_t      sourceSrv  = m_pyramid.GetSrvHandle();
    if (!params || !tint || !layerDepth || !depth || !sourceSrv) return cl;

    namespace K = RHI::Bindless::Kind;
    Constants c{};
    c.paramsIdx     = gfx.Srv<K::Tex2DFloat>(*params,     "transmission params").Bits();
    c.tintIdx       = gfx.Srv<K::Tex2DFloat>(*tint,       "transmission tint").Bits();
    c.layerDepthIdx = gfx.Srv<K::Tex2DFloat>(*layerDepth, "transmission layer depth").Bits();
    c.sceneDepthIdx = gfx.Srv<K::Tex2DFloat>(*depth,      "transmission scene depth").Bits();
    c.sourceIdx     = gfx.BindlessOf<K::Tex2DFloat>(sourceSrv, "transmission background").Bits();
    c.sourceMaxMip  = float(m_pyramid.GetMipCount() > 0 ? m_pyramid.GetMipCount() - 1 : 0);
    c.nearZ         = m_view.nearZ;
    c.farZ          = m_view.farZ;
    c.renderSize[0] = float(m_snapshotW);
    c.renderSize[1] = float(m_snapshotH);
    c.pixelAngle    = 2.0f * std::tan(0.5f * m_view.fovY) / float(m_snapshotH);
    c.blurScale     = Settings::Pass::transmissionBlurScale();

    RG::DrawFullscreen(cl, m_psoCache, ApplyDraw(m_depth), SR::CB_b2, c);
    return cl;
}

void TransmissionApplyPass::ReloadShaders(IGraphicsDevice& gfx)
{
    RG::RenderPass::ReloadShaders(gfx);
    m_pyramid.ReloadShaders(gfx);
}

void TransmissionApplyPass::Shutdown(IGraphicsDevice& gfx)
{
    if (m_snapshot.IsValid()) gfx.DestroyTexture(m_snapshot);
    m_snapshot  = {};
    m_snapshotW = m_snapshotH = 0;
}

// ==== TransmissionMotionPass ====

void TransmissionMotionPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TransmissionMotion_CS,      RHI::ShaderStage::CS, "TransmissionMotion.cs.hlsl", "CSMotion");
    m_shaderLib.Register(ShaderID::TransmissionMotionWrite_CS, RHI::ShaderStage::CS, "TransmissionMotion.cs.hlsl", "CSWrite");
    CreatePipelines(gfx);
}

// Creates into the live members; a failed rebuild keeps the old pipelines.
bool TransmissionMotionPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* motion = m_shaderLib.GetShader(ShaderID::TransmissionMotion_CS);
    const RHI::Shader* write  = m_shaderLib.GetShader(ShaderID::TransmissionMotionWrite_CS);
    if (!motion || !write)
    {
        LOG_ERROR("TransmissionMotionPass: TransmissionMotion.cs.hlsl not found");
        return false;
    }
    RHI::PipelineStateDesc motionDesc{};
    motionDesc.cs = motion;
    RHI::PipelineStateDesc writeDesc{};
    writeDesc.cs = write;
    if (!gfx.CreatePipelineState(motionDesc, m_motionPso) || !gfx.CreatePipelineState(writeDesc, m_writePso))
    {
        LOG_ERROR("TransmissionMotionPass: PSO creation failed");
        return false;
    }
    return true;
}

void TransmissionMotionPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void TransmissionMotionPass::Shutdown(IGraphicsDevice& gfx)
{
    for (RHI::Texture& target : m_targets)
    {
        if (target.IsValid()) gfx.DestroyTexture(target);
        target = {};
    }
    m_width = m_height = 0;
    m_historyValid = false;
}

bool TransmissionMotionPass::EnsureTargets(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    if (m_width == w && m_height == h && m_targets[0].IsValid())
        return true;
    Shutdown(gfx);
    static const char* const kNames[kTargets] = { "TransmissionOffsetA", "TransmissionOffsetB", "TransmissionMotion" };
    for (int i = 0; i < kTargets; ++i)
    {
        RHI::TextureDesc desc{};
        desc.width     = w;
        desc.height    = h;
        desc.format    = RHI::Format::R16G16_FLOAT;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        desc.usage     = RHI::Usage::DEFAULT;
        desc.layout    = RHI::ResourceState::UNORDERED_ACCESS;
        desc.debugName = kNames[i];
        if (!gfx.CreateTexture(desc, m_targets[i]))
        {
            LOG_ERROR("TransmissionMotionPass: %s %ux%u failed", kNames[i], w, h);
            Shutdown(gfx);
            return false;
        }
        m_states[i] = RHI::ResourceState::UNORDERED_ACCESS;
    }
    m_width  = w;
    m_height = h;
    return true;
}

void TransmissionMotionPass::Transition(IGraphicsDevice& gfx, RHI::CommandList cl, int target, RHI::ResourceState to)
{
    if (m_states[target] == to) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_targets[target], m_states[target], to), cl);
    m_states[target] = to;
}

bool TransmissionMotionPass::Execute(RHI::CommandList cl, const Inputs& in)
{
    namespace C = RHI::Slots::Cmp;
    IGraphicsDevice& gfx = cl.GetDevice();
    if (!m_motionPso.IsValid() || !m_writePso.IsValid() || !in.params || !in.layerDepth || !in.velocity ||
        in.depthSrv == 0 || in.width == 0 || in.height == 0 || !EnsureTargets(gfx, in.width, in.height))
        return false;
    const uint64_t paramsSrv     = gfx.GetTextureSRVGpuHandle(*in.params);
    const uint64_t layerDepthSrv = gfx.GetTextureSRVGpuHandle(*in.layerDepth);
    const uint64_t velocitySrv   = gfx.GetTextureSRVGpuHandle(*in.velocity);
    const uint64_t velocityUav   = gfx.GetTextureUAVGpuHandle(*in.velocity);
    if (!paramsSrv || !layerDepthSrv || !velocitySrv || !velocityUav)
        return false;

    const int cur  = m_current ^ 1;
    const int prev = m_current;
    RHI::ResourceState velocityState = in.velocityState;
    auto velocityTo = [&](RHI::ResourceState to)
    {
        if (velocityState == to) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(in.velocity, velocityState, to), cl);
        velocityState = to;
    };

    // The motions, from the buffer as the opaque passes and the sky's fill left it.
    Transition(gfx, cl, prev, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    Transition(gfx, cl, cur, RHI::ResourceState::UNORDERED_ACCESS);
    Transition(gfx, cl, kMotion, RHI::ResourceState::UNORDERED_ACCESS);
    velocityTo(RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    const Constants c{ in.width, in.height, m_historyValid ? 1u : 0u, 0u };
    const RG::Groups groups = RG::Groups2D(in.width, in.height, 8);
    const bool motions = RG::DispatchCS(cl, m_motionPso, c,
        { { C::SRV_t0, paramsSrv }, { C::SRV_t1, layerDepthSrv }, { C::SRV_t2, in.depthSrv },
          { C::SRV_t3, gfx.GetTextureSRVGpuHandle(m_targets[prev]) }, { C::SRV_t4, velocitySrv },
          { C::UAV_u1, gfx.GetTextureUAVGpuHandle(m_targets[cur]) },
          { C::UAV_u2, gfx.GetTextureUAVGpuHandle(m_targets[kMotion]) } }, groups);

    // Then into the buffer, over the covered pixels only. t4 is rebound: a static SRV of the
    // buffer must not stay set once the buffer turns writable (the debug layer refuses the
    // dispatch).
    Transition(gfx, cl, kMotion, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    velocityTo(RHI::ResourceState::UNORDERED_ACCESS);
    const bool written = motions && RG::DispatchCS(cl, m_writePso, c,
        { { C::SRV_t0, paramsSrv }, { C::SRV_t4, paramsSrv },
          { C::SRV_t5, gfx.GetTextureSRVGpuHandle(m_targets[kMotion]) }, { C::UAV_u0, velocityUav } }, groups);

    m_current      = cur;
    m_historyValid = written;
    return true;
}
