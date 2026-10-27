#include "RenderGraph/RenderPass/TransparentPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/SpotShadowView.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/TaskSystem.h"

// Root slots shared with GBufferPass / SkyboxPass (RootSignatureLayout.h is the
// single source of truth).
// S::  = absolute root-parameter index  (BindRootBufferSRV / BindDescriptorTableHandle).
// SR:: = group-relative index           (BindCB / BindBufferSRV).
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

TransparentPass::TransparentPass(RG::RGTextureHandle depth,
                                 RG::RGTextureHandle gbufNormal,
                                 RG::RGTextureHandle traceDepth,
                                 RG::RGTextureHandle frontDepth)
    : m_depth(depth), m_gbufNormal(gbufNormal), m_traceDepth(traceDepth)
    , m_frontDepth(frontDepth) {}

void TransparentPass::Setup(RG::RenderGraphBuilder& b)
{
    // Planes on: depth is only tested, a read that keeps the plane in a state the test
    // takes. -rgplaneoff: a depth-stencil write, so the graph brings depth to
    // DEPTHSTENCIL.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Read);
    else                                  b.WriteDepthStencil(m_depth);
    // GBuffer1 normal + the SSRTraceDepth snapshot: the traced-reflection fold
    // (custom_surface.hlsli MS_FetchTracedReflection) opens both bindless from
    // LightCB.fwdReflNormalIdx / fwdReflDepthIdx, so nothing is bound here - but the
    // reads must be declared, or the graph has no reason to keep them shader-readable
    // across this pass. The declaration is the contract, the index only the address.
    b.ReadSRV(m_gbufNormal);
    b.ReadSRV(m_traceDepth);
    // The translucent front-layer depth (MS_FetchFrontLayerReflection's identity
    // test): same contract, declared here, named by LightCB.
    b.ReadSRV(m_frontDepth);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds
    // or clears it) and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The pass textures the forward surface (custom_surface.hlsli MS_ForwardLighting)
    // samples, bound or named by LightCB and declared for the graph's DAG (their
    // producers barrier them): the shadow maps except the per-object ones (water only),
    // the RT sun mask, the AO, both RT reflections, SSGI, this frame's cloud shadow, the
    // froxel fog (FogApply, also for the beam and afterimage shaders drawn here) and
    // the sky's specular cube.
    b.ReadPassTextures({ RG::PassTex::kShadowCSM, RG::PassTex::kShadowSpotAtlas,
                         RG::PassTex::kShadowSpotAtlasMedium, RG::PassTex::kShadowSpotAtlasHigh,
                         RG::PassTex::kShadowPointCube, RG::PassTex::kRTShadowMask,
                         RG::PassTex::kRTAO, RG::PassTex::kXeGTAO,
                         RG::PassTex::kRTReflection, RG::PassTex::kRTReflectionFront, RG::PassTex::kSSGIResult,
                         RG::PassTex::kCloudShadow, RG::PassTex::kVolFogScattering, RG::PassTex::kSkySpecularCube });
}

void TransparentPass::SetNameSuffix(const char* suffix)
{
    // An empty suffix would reproduce the main instance's name (first-match-wins
    // timestamp scan, one shared external-wait key, one merged PIX region): keep the
    // name and report instead.
    if (!ENGINE_CHECK(suffix && *suffix,
                      "TransparentPass::SetNameSuffix: empty suffix would duplicate '%s'",
                      kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

// Teardown for a second instance, with GBufferPass's borrow gate: a preview's
// forward instance borrows the main TransparentPass's library and must not shut it
// down.
void TransparentPass::Shutdown(IGraphicsDevice& gfx)
{
    m_psoCache.Shutdown();
    if (OwnsShaderLibrary()) m_ownedLib.Shutdown(gfx);

    if (m_defaultWhite.IsValid())      gfx.DestroyTexture(m_defaultWhite);
    if (m_defaultFlatNormal.IsValid()) gfx.DestroyTexture(m_defaultFlatNormal);
    if (m_shadowFallbackTex.IsValid()) gfx.DestroyTexture(m_shadowFallbackTex);
    if (m_spotShadowVPFallbackBuf.IsValid()) gfx.DestroyBuffer(m_spotShadowVPFallbackBuf);
    m_defaultWhiteGpuHandle      = 0;
    m_defaultFlatNormalGpuHandle = 0;
    m_shadowFallbackHandle       = 0;
    m_spotShadowVPFallback       = 0;

    m_linearSamplerIdx = -1;
}

void TransparentPass::Init(IGraphicsDevice& gfx)
{
    // Only the owner initialises the library and registers the two static shaders (a
    // borrower Inits after the owner, so the registry is already populated).
    // ShaderLibrary::Register is idempotent for a fixed ShaderID, but Init is not (it
    // rewrites m_gfx / m_shaderDir outside the library's mutex), so a borrower must not
    // call either.
    if (OwnsShaderLibrary())
    {
        m_ownedLib.Init(gfx, "shaders/");
        m_ownedLib.Register(ShaderID::GBuffer_VS,     RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
        m_ownedLib.Register(ShaderID::Transparent_PS, RHI::ShaderStage::PS, "Transparent.ps.hlsl");
    }
    m_psoCache.Init(gfx, *m_lib);

    // Warm up the base alpha-blend PSO immediately.
    PermutationKey basePerm;
    basePerm.Set(PermutationKey::ALPHA_BLEND, true);
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm)))
        LOG_ERROR("TransparentPass: base PSO creation failed");
    else
        LOG_INFO("TransparentPass: base PSO ready");

    // 1x1 white fallback for base-color and surface-map slots.
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t white = 0xFFFFFFFFu;
        RHI::SubresourceData init{ &white, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultWhite, &init))
            m_defaultWhiteGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultWhite);
        else
            LOG_ERROR("TransparentPass: default white texture failed");
    }

    // 1x1 flat-normal fallback (128,128,255,255).
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flat = 0xFFFF8080u;
        RHI::SubresourceData init{ &flat, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultFlatNormal, &init))
            m_defaultFlatNormalGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultFlatNormal);
        else
            LOG_ERROR("TransparentPass: flat-normal texture failed");
    }

    // The CSM comparison sampler is the fixed SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE heap
    // slot custom_surface.hlsli names (GREATER_EQUAL: depth is reverse-Z engine-wide; a
    // LESS_EQUAL would shadow exactly the lit pixels). Unlike a root sampler, a sampler
    // the shader names cannot be left unbound, so nothing is created here.

    // 1x1 fully-lit CSM stand-in for frames where ShadowPass has no array yet
    // (GetShadowArrayGpuHandle returns 0 until m_shadowArray is valid). The forward
    // BRDF samples t9 unconditionally, and an unbound table is UB that Resource Binding
    // Tier 3 does not report.
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R32_FLOAT;
        desc.width      = 1;
        desc.height     = 1;
        desc.arraySize = 4;   // Texture2DArray SRV needs > 1
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        // arraySize 4 and texel 0.0 are both load-bearing (as for LightingPass::Init's
        // per-object and point-cube fallbacks): CreateTexture emits a TEXTURE2DARRAY SRV
        // only when arraySize > 1, and a TEXTURE2D SRV in the shader's Texture2DArray slot
        // is a Tier-3 type mismatch with no validation message. Under the reversed-Z
        // GREATER_EQUAL comparison, depth 0.0 (the shadow clear value) is "no occluder"; a
        // 1.0 texel would read as fully shadowed.
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // One entry per subresource: CreateTexture uploads mipLevels * arraySize
        // subresources from this array; a single-element init would read off the stack.
        RHI::SubresourceData init[4];
        for (auto& sd : init) sd = { &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(desc, m_shadowFallbackTex, init))
            m_shadowFallbackHandle = gfx.GetTextureSRVGpuHandle(m_shadowFallbackTex);
        else
            LOG_ERROR("TransparentPass: CSM fallback texture failed");
    }

    // The spot-shadow VP fallback (t22): one entry whose atlasIndex is ~0 ("no atlas",
    // SampleSpotShadow returns lit), built like LightingPass's, so the table is never
    // unbound under the forward loop.
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
            LOG_ERROR("TransparentPass: spot-shadow VP fallback failed");
    }

    // The material sampler (SetDynamicSampler, aniso wrap; MaterialSampler.h has the
    // description and bias rule). It matches GBufferPass so alpha-blended surfaces
    // (glass, hair cards, mesh particles) agree with opaque TAA integration. The IBL
    // and fog samplers are fixed heap slots the shader names.
    if (!gfx.CreateSampler(MaterialSampler::Desc(m_textureAnisotropy, m_mipBias), m_linearSamplerIdx))
        LOG_ERROR("TransparentPass: linear sampler failed");
}

void TransparentPass::WarmPipelines(const DrawPacket& dp)
{
    if (dp.filter != DrawFilter::Transparent) return;
    (void)m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID, dp.customVSID, dp.mirrored != 0));
}

PSODesc TransparentPass::BuildPSODesc(PermutationKey perm, uint32_t customPSID, uint32_t customVSID,
                                      bool mirrored) const
{
    PSODesc desc;
    desc.vsID        = (customVSID != 0)
                        ? static_cast<ShaderID>(customVSID)
                        : ShaderID::GBuffer_VS;
    desc.psID        = (customPSID != 0)
                        ? static_cast<ShaderID>(customPSID)
                        : ShaderID::Transparent_PS;
    desc.perm        = perm;
    desc.perm.Set(PermutationKey::DEBUG_SHADE, m_debugShade);
    desc.inputLayout = InputLayoutType::None;  // PVF

    // Cull back faces by default; DOUBLE_SIDED -> NONE.
    desc.rs.cullMode         = perm.Has(PermutationKey::DOUBLE_SIDED)
                                ? RHI::CullMode::NONE
                                : RHI::CullMode::BACK;
    // Mirrored instances reverse winding: flip the front-face convention, not the cull
    // mode (SV_IsFrontFace must stay meaningful). See GBufferPass.
    desc.rs.frontCounterClockwise = mirrored;
    // Global view-mode wireframe (PSOCache hashes rs.fillMode -> distinct PSO).
    desc.rs.fillMode         = m_wireframeMode ? RHI::FillMode::WIREFRAME
                                                : RHI::FillMode::SOLID;
    desc.rs.depthClipEnable = true;

    // Depth test on, depth write off - transparent objects don't update depth.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    // Blend state, selected by permutation.
    auto& rt = desc.bs.renderTarget[0];
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rt.blendEnable             = true;

    if (perm.Has(PermutationKey::ADDITIVE_BLEND))
    {
        // Additive: src*ONE + dst*ONE
        rt.srcBlend       = RHI::Blend::ONE;
        rt.destBlend      = RHI::Blend::ONE;
        rt.blendOp        = RHI::BlendOp::ADD;
        rt.srcBlendAlpha = RHI::Blend::ONE;
        rt.destBlendAlpha= RHI::Blend::ONE;
        rt.blendOpAlpha  = RHI::BlendOp::ADD;
    }
    else if (perm.Has(PermutationKey::PREMULTIPLIED_BLEND))
    {
        // Premultiplied alpha: src*ONE + dst*INV_SRC_ALPHA
        rt.srcBlend       = RHI::Blend::ONE;
        rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blendOp        = RHI::BlendOp::ADD;
        rt.srcBlendAlpha = RHI::Blend::ONE;
        rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blendOpAlpha  = RHI::BlendOp::ADD;
    }
    else if(perm.Has(PermutationKey::MULTIPLY_BLEND))
    {
        // Multiply: src*DST_COLOR + dst*INV_SRC_ALPHA
        //   alpha=1 -> src*dst (full multiply)
        //   alpha=0 -> dst (scene untouched)
        //   between -> lerp(dst, src*dst, alpha)
        // DEST_COLOR/ZERO would discard dst and make the alpha slider inert.
        rt.srcBlend       = RHI::Blend::DEST_COLOR;
        rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blendOp        = RHI::BlendOp::ADD;
        rt.srcBlendAlpha = RHI::Blend::ONE;
        rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blendOpAlpha  = RHI::BlendOp::ADD;
    }
    else if (perm.Has(PermutationKey::TRANSMISSIVE_BLEND))
    {
        // Transmissive: the surface's own light over the background TransmissionApply
        // already refracted and tinted. src*ONE + dst*INV_SRC_ALPHA; the shader's alpha is 0
        // when the apply ran, a coverage fallback when it did not.
        rt.srcBlend       = RHI::Blend::ONE;
        rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blendOp        = RHI::BlendOp::ADD;
        rt.srcBlendAlpha = RHI::Blend::ONE;
        rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blendOpAlpha  = RHI::BlendOp::ADD;
    }
    else
    {
        // Alpha blend (default): src*SRC_ALPHA + dst*INV_SRC_ALPHA
        rt.srcBlend       = RHI::Blend::SRC_ALPHA;
        rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
        rt.blendOp        = RHI::BlendOp::ADD;
        rt.srcBlendAlpha = RHI::Blend::ONE;
        rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
        rt.blendOpAlpha  = RHI::BlendOp::ADD;
    }

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

RHI::CommandList TransparentPass::Execute(RHI::CommandList cl)
{
    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    return RecordRange(cl, draws, nullptr);
}

uint32_t TransparentPass::PrepareRecording(RHI::CommandList cl)
{
    ZoneScopedN("Transparent.PrepareRecording");
    m_recordChunkCount = 1;
    m_recordDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    m_recordPSOs.clear();
    const uint32_t count = static_cast<uint32_t>(m_recordDraws.size());
    uint32_t chunks = (std::min)(kMaxRecordChunks, count / 256u);
#if WITH_HARNESS
    const auto& cmd = Sys::CommandLine::Get();
    if (cmd.Has("-transparentserial")) return 0;
    const int forced = cmd.GetInt("-transparentchunks", 0);
    if (forced > 0) chunks = (std::min)(count, (std::min)(kMaxRecordChunks, static_cast<uint32_t>(forced)));
#endif
    if (chunks < 2 || TaskSystem::Get().GetWorkerCount() < 2) return 0;

    // Resolve the shared cache before any chunk records. Pointers live until
    // the graph joins; shader reload cannot run concurrently with recording.
    struct Entry { uint32_t perm, ps, vs; bool mirrored; const RHI::PipelineState* pso; };
    std::vector<Entry> unique;
    unique.reserve(16);
    m_recordPSOs.reserve(count);
    for (const DrawPacket& dp : m_recordDraws)
    {
        const RHI::PipelineState* pso = nullptr;
        bool found = false;
        for (const auto& e : unique)
            if (e.perm == dp.permutation.bits && e.ps == dp.customPSID &&
                e.vs == dp.customVSID && e.mirrored == (dp.mirrored != 0))
            { pso = e.pso; found = true; break; }
        if (!found)
        {
            pso = m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID, dp.customVSID, dp.mirrored != 0));
            unique.push_back({dp.permutation.bits, dp.customPSID, dp.customVSID, dp.mirrored != 0, pso});
        }
        m_recordPSOs.push_back(pso);
    }
    m_recordChunkCount = chunks;
    for (uint32_t i = 0; i < chunks; ++i)
    {
        auto& c = m_recordChunks[i];
        c.begin = static_cast<uint32_t>(uint64_t(count) * i / chunks);
        c.end = static_cast<uint32_t>(uint64_t(count) * (i + 1) / chunks);
        c.cl = i == 0 ? cl : cl.gfx->BeginCommandList(RHI::QueueType::GRAPHICS);
        c.cl.gfx = cl.gfx;
        c.cl.ctx = cl.ctx;
        c.gpuRegion = ~0u;
        if (i > 0)
        {
            // GPU painter order is independent of CPU task completion order.
            cl.gfx->AddCommandListDependency(c.cl, m_recordChunks[i - 1].cl);
            c.gpuRegion = cl.gfx->BeginGPUTimestampNoTracy(c.cl, "Transparent.Chunk");
            cl.gfx->BeginEventMarker(c.cl, "Transparent.Chunk");
            cl.gfx->SetCommandListName(c.cl, "Transparent.Chunk");
        }
    }
    static const int s_logEvery = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
    static uint64_t s_logFrame = 0;
    if (s_logEvery > 0 && ++s_logFrame % s_logEvery == 0)
        LOG_INFO("[TRANSPARENT_JOBS] draws=%u chunks=%u ordered=1", count, chunks);
    return chunks - 1;
}

RHI::CommandList TransparentPass::ExecutePrepared(RHI::CommandList cl)
{
    if (m_recordChunkCount == 1) return Execute(cl);
    const auto& c = m_recordChunks[0];
    RecordRange(cl, m_recordDraws.subspan(c.begin, c.end - c.begin), m_recordPSOs.data() + c.begin);
    return m_recordChunks[m_recordChunkCount - 1].cl;
}

void TransparentPass::ExecuteAdditionalRecording(uint32_t task)
{
    const auto& c = m_recordChunks[task + 1];
    c.cl.gfx->BeginTracyGPUZone(c.cl, "Transparent.Chunk", c.gpuRegion);
    try {
        RecordRange(c.cl, m_recordDraws.subspan(c.begin, c.end - c.begin), m_recordPSOs.data() + c.begin);
    } catch (...) {
        c.cl.gfx->EndTracyGPUZone(c.gpuRegion);
        throw;
    }
    c.cl.gfx->EndTracyGPUZone(c.gpuRegion);
}

void TransparentPass::FinishRecording()
{
    for (uint32_t i = 1; i < m_recordChunkCount; ++i)
    {
        const auto& c = m_recordChunks[i];
        c.cl.gfx->EndGPUTimestampNoTracy(c.cl, c.gpuRegion);
        c.cl.gfx->EndEventMarker(c.cl);
    }
    m_recordDraws = {};
    m_recordPSOs.clear();
    m_recordChunkCount = 1;
}

RHI::CommandList TransparentPass::RecordRange(RHI::CommandList cl, DrawList draws,
                                             const RHI::PipelineState* const* psos)
{
    ZoneScopedN("Transparent.RecordChunk");
    ZoneValue(draws.size());
    if (draws.empty()) return cl;

    // Bind the HDR RTV with the GBuffer depth (read-only depth test, no write).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const RHI::PipelineState* activePSO = nullptr;

    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
        cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);

        // CB slot 0 (b1 space0) = PerView - used by GBuffer.vs.hlsl.
        cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);
        // CB slot 1 (b2 space0) = LightCB - used by Transparent.ps.hlsl.
        cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);
        // b4 = the fog the opaque scene got: every forward main, the beam glow and the
        // afterimage apply it.
        FogApply::Bind(cl);

        // Material buffer at t2 space0 (slot 0 of BindBufferSRV).
        const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer);
        if (matBuf && matBuf->IsValid())
            cl.BindBufferSRV(SR::SRV_t2, *matBuf);

        // The material sampler; the IBL and CSM comparison samplers are fixed heap slots
        // custom_surface.hlsli names.
        if (m_linearSamplerIdx >= 0) cl.SetDynamicSampler(m_linearSamplerIdx);   // sampler_heap.hlsli SmpDynamic

        // IBL resources.
        if (m_iblIrradianceHandle) cl.BindDescriptorTableHandle(S::IBLIrradiance, m_iblIrradianceHandle);
        if (m_iblRadianceHandle)   cl.BindDescriptorTableHandle(S::IBLRadiance,   m_iblRadianceHandle);
        if (m_brdfLutHandle)       cl.BindDescriptorTableHandle(S::BRDFLUT,       m_brdfLutHandle);

        // Sky SH coefficients, the buffer the deferred LightingPass reads. Unset (0) means
        // LightCB.iblUseSH is 0 too, so the shader takes the irradiance-cubemap branch and
        // never reads gSkySH.
        if (m_skySHHandle) cl.BindDescriptorTableHandle(S::SkySH, m_skySHHandle);

        // CSM, bound unconditionally: the forward BRDF samples t9 on every draw, so this
        // falls back to the 1x1 fully-lit array instead of leaving the table unbound.
        {
            const uint64_t shadowSrv = m_shadowSrvHandle ? m_shadowSrvHandle
                                                         : m_shadowFallbackHandle;
            if (shadowSrv) cl.BindDescriptorTableHandle(S::ShadowSRV, shadowSrv);
        }

        // Clustered point / spot lights: the triple LightingPass and WaterSurfacePass bind,
        // all three or none (SetClusterSRVs); then the spot-shadow views or the "no atlas"
        // fallback.
        if (m_clusterLightsSrv && m_clusterIndexSrv && m_clusterGridSrv)
        {
            cl.BindDescriptorTableHandle(S::ClusterLights,    m_clusterLightsSrv);
            cl.BindDescriptorTableHandle(S::ClusterIndexList, m_clusterIndexSrv);
            cl.BindDescriptorTableHandle(S::ClusterGrid,      m_clusterGridSrv);
        }
        {
            const uint64_t vps = m_spotShadowVPHandle ? m_spotShadowVPHandle : m_spotShadowVPFallback;
            if (vps) cl.BindDescriptorTableHandle(S::SpotShadowVP, vps);
        }

        // Reflection probes bind nothing: reflection_probe.hlsli samples each cube through
        // ResourceDescriptorHeap[NonUniformResourceIndex(probe.cubemapSrvIndex)], and
        // custom_surface.hlsli opens the probe buffer from LightCB.reflProbeBufIdx
        // (reflectionProbeCount == 0 skips the loop).

        // Seed t3, t4, t5 with defaults so they are never unbound.
        if (m_defaultWhiteGpuHandle)
        {
            cl.BindDescriptorTableHandle(S::SRV_t3, m_defaultWhiteGpuHandle);
            cl.BindDescriptorTableHandle(S::SRV_t4, m_defaultWhiteGpuHandle);
        }
        if (m_defaultFlatNormalGpuHandle)
            cl.BindDescriptorTableHandle(S::SRV_t5, m_defaultFlatNormalGpuHandle);

        // Bind bindless texture table (for emissive map sampling via textureHandleIds).
        if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
            cl.BindDescriptorTableHandle(S::BindlessTex, texTable);
    };


    // Transparent draws retain painter order. All PSOs built here share the
    // graphics root signature, so changing a PSO does not invalidate globals.
    // Cache only within this Execute call: shader reload cannot stale pointers.
    struct CachedPSO { uint32_t perm, ps, vs; bool mirrored; const RHI::PipelineState* pso; };
    std::vector<CachedPSO> framePSOs;
    if (!psos) framePSOs.reserve(16);
    bool globalsBound = false;
    uint64_t lastBC = 0, lastSM = 0, lastNM = 0, lastCB = 0, lastTex = 0;
    for (const DrawPacket& dp : draws)
    {
        const RHI::PipelineState* pso = psos ? *psos++ : nullptr;
        bool cached = psos != nullptr;
        if (!cached) for (const auto& entry : framePSOs)
            if (entry.perm == dp.permutation.bits && entry.ps == dp.customPSID &&
                entry.vs == dp.customVSID && entry.mirrored == (dp.mirrored != 0))
            { pso = entry.pso; cached = true; break; }
        if (!cached)
        {
            pso = m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID, dp.customVSID, dp.mirrored != 0));
            framePSOs.push_back({dp.permutation.bits, dp.customPSID, dp.customVSID, dp.mirrored != 0, pso});
        }
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            if (!globalsBound) { bindGlobals(); globalsBound = true; }
        }

        // Per-draw texture slots (base color t3, surface t4, normal t5).
        {
            const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
            const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
            const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
            if (bc && bc != lastBC) { cl.BindDescriptorTableHandle(S::SRV_t3, bc); lastBC = bc; }
            if (sm && sm != lastSM) { cl.BindDescriptorTableHandle(S::SRV_t4, sm); lastSM = sm; }
            if (nm && nm != lastNM) { cl.BindDescriptorTableHandle(S::SRV_t5, nm); lastNM = nm; }
        }

        // Custom material CBV (b8 space0) and custom texture table (t0..t3 space3), as in
        // GBufferPass, so one material can drive opaque and additive draws with the same
        // custom PS.
        if (dp.customCbvVA && dp.customCbvVA != lastCB)
        { cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl); lastCB = dp.customCbvVA; }
        if (dp.customTexTable && dp.customTexTable != lastTex)
        { cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl); lastTex = dp.customTexTable; }

        // Write all 4 PVF root constants (as GBufferPass does). The 4th,
        // prevPosElementBase, drives the VS's skinned previous-position fetch for TAA
        // velocity (GBuffer.vs.hlsl and custom_vertex.hlsli read it), and root constants
        // persist across draws, so omitting it would feed a stale value. One 4-constant
        // call, not 3+1, avoids the driver corner case noted in GBufferPass.
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
void TransparentPass::SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW)
{
    MaterialSampler::Rebuild(gfx, m_textureAnisotropy,
                             MaterialSampler::Bias(MaterialSampler::kNativeBias, renderW, displayW),
                             m_linearSamplerIdx, m_mipBias);
}
