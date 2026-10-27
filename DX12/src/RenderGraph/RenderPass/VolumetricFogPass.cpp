#include "Graphics/SpotShadowView.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"          // placed-resource path (alias arena)
#include "Graphics/MediumSet.h"
#include "Graphics/TransientAliasArena.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShaderDiskCache.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"   // the ray-traced injection's shell layout
#include "Settings/PassSettings.h"                  // pass.volumetricFog.rt*
#include "System/Check.h"
#include "System/Log.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

// Root-parameter indices come from Graphics/RootSignatureLayout.h - the local
// names below are aliases that keep this file's vocabulary, never new numbers.
namespace S    = RHI::Slots::Gfx;    // ABSOLUTE graphics root indices (apply pass)
namespace SRel = RHI::Slots::GfxRel; // GROUP-RELATIVE (BindCB/BindSRVByHandle)
namespace C    = RHI::Slots::Cmp;    // ABSOLUTE compute root indices (froxel passes)

// The compute root signature, shared by every compute pass (RHI::Slots::Cmp,
// GraphicsDX12's CreateComputeRootSignature).
static constexpr auto kCBSlot   = C::CB;      // b0 space2
static constexpr auto kSRVT0    = C::SRV_t0;  // t0 space2
static constexpr auto kSRVT1    = C::SRV_t1;  // t1 space2
static constexpr auto kUAV0     = C::UAV_u0;  // u0 space2

// The apply draws use the GRAPHICS root signature, slots as in LightingPass.
static constexpr auto kEnvMapRootSlot = S::SRV_t6;  // t6 space0, the env cube's slot, reused by both apply draws

// FroxelCommon.hlsli's FROXEL_RT rtFlags.
static constexpr uint32_t kRtSun = 1u, kRtSkipShadow = 2u, kRtHistory = 4u, kRtGlass = 8u, kRtIndirectHistory = 16u;
// One froxel's light reservoir (FroxelLightInjectRT.cs.hlsl): uint2.
static constexpr uint32_t kRtReservoirBytes = 8;
// FroxelCommon.hlsli's kFroxelIndirect*: the indirect light's source.
static constexpr uint32_t kIndirectConstant = 0u, kIndirectDdgi = 1u, kIndirectRays = 2u;
// The indirect rays' samples a cell averages at most (FroxelParams::indirectAlpha = 1 / this): at four
// rays a cell a frame (FroxelIndirectRT.cs.hlsl), the last 16 frames.
static constexpr float kIndirectSamples = 64.0f;

VolumetricFogPass::VolumetricFogPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{
}

void VolumetricFogPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
    m_view = &view;
    if (view.Slot() == 0)
        m_v = &m_mainVol;
    else
    {
        m_v = &view.Data<ViewVolumes>(this).vol;
        if (m_gfx) MatchMainVolumes(view);
    }
    m_scatteringImport = m_v->scatteringTex;
}

void VolumetricFogPass::MatchMainVolumes(const ViewState& view)
{
    if (m_mainVol.densityTex.IsValid() && !m_v->densityTex.IsValid())
    {
        Create3DTextures(*m_gfx);
        m_hist->valid = false;   // fresh volumes hold garbage
        LOG_INFO("VolumetricFogPass: view %u froxel volumes allocated (70.3 MiB)", view.Slot());
    }
    else if (!m_mainVol.densityTex.IsValid() && m_v->densityTex.IsValid())
    {
        ReleaseFroxelVolumes();
        LOG_INFO("VolumetricFogPass: view %u froxel volumes released (fog disabled)", view.Slot());
    }
    // The ray-traced lighting's reservoirs and grids while the main view's live (EnsureFroxelResources).
    EnsureRtVolumes(m_rtVolumes && m_v->densityTex.IsValid());

    // The raymarch target at the render size, which is the view's only in its segment (the device's
    // render size while its HDR target is pushed).
    if (RHI::CurrentRecordingView().slot != view.Slot()) return;
    if (m_mainVol.rmCurrentTex.IsValid())
    {
        const uint32_t renderW = m_gfx->GetRenderWidth();
        const uint32_t renderH = m_gfx->GetRenderHeight();
        if (renderW > 0 && renderH > 0 && (renderW != m_v->rmLastFullW || renderH != m_v->rmLastFullH))
            RebuildRaymarchTextures(renderW, renderH);
    }
    else if (m_v->rmCurrentTex.IsValid())
    {
        DestroyRaymarchTextures();
        m_v->rmLastFullW = 0;
        m_v->rmLastFullH = 0;
    }
}

void VolumetricFogPass::ViewVolumes::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &vol.densityTex, &vol.lightingTex, &vol.scatteringTex, &vol.rmCurrentTex })
        if (t->IsValid()) gfx.DestroyTexture(*t);
    vol = Volumes{};
}

void VolumetricFogPass::History::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &tex[0], &tex[1], &rmTex[0], &rmTex[1] })
    {
        if (t->IsValid()) gfx.DestroyTexture(*t);
        *t = {};
    }
    valid = rmValid = false;
    ReleaseRt(gfx);
}

void VolumetricFogPass::History::ReleaseRt(IGraphicsDevice& gfx)
{
    for (RHI::GPUBuffer& b : rtReservoir)
    {
        if (b.IsValid()) gfx.DestroyBuffer(b);
        b = {};
    }
    for (RHI::Texture* t : { &rtIndirect[0], &rtIndirect[1], &rtIndirectSmoothed })
    {
        if (t->IsValid()) gfx.DestroyTexture(*t);
        *t = {};
    }
    rtLast = rtIndirectRan = false;
}

VolumetricFogPass::~VolumetricFogPass()
{
    if (!m_gfx) return;
    // The main view's: a secondary view's volumes and every history are the view's, released with
    // it by ~Renderer before the graph's passes - m_v and m_hist may dangle here.
    for (RHI::Texture* t : { &m_mainVol.densityTex, &m_mainVol.lightingTex, &m_mainVol.scatteringTex,
                             &m_mainVol.rmCurrentTex })
        if (t->IsValid()) m_gfx->DestroyTexture(*t);
    ReleaseRtPipelines(*m_gfx);
    m_paramCB.Destroy(*m_gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_volLightsBufMapped[i])    m_gfx->UnmapBuffer(m_volLightsBuf[i]);
        if (m_volLightsBuf[i].IsValid()) m_gfx->DestroyBuffer(m_volLightsBuf[i]);
        m_volLightsBufMapped[i] = nullptr;
        m_lightsSrv[i]          = 0;
        if (m_localFogBufMapped[i])     m_gfx->UnmapBuffer(m_localFogBuf[i]);
        if (m_localFogBuf[i].IsValid()) m_gfx->DestroyBuffer(m_localFogBuf[i]);
        m_localFogBufMapped[i] = nullptr;
        m_localFogSrv[i]       = 0;
    }
}

// The froxel scattering volume as this pass's import: the graph reads the bound view's copy
// (re-placed when the arena moves) at each Execute.
void VolumetricFogPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_scatteringH = d.Import(RG::PassTex::kVolFogScattering,
        { RHI::Format::R16G16B16A16_FLOAT, kFroxelW, kFroxelH, false, true },
        { &m_scatteringImport, RG::ImportEntry::Stale, RG::ImportTracking::Owner });
}

void VolumetricFogPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth is read as an SRV: the graph emits DEPTHSTENCIL -> DEPTH_READ_SRV before this
    // pass.
    //
    // Do not make HdrSceneColor the colour target (SetColorTarget): the graph would CLEAR
    // it on entry, and a pass that early-outs would leave the HDR black. Execute binds it
    // by hand (SetRenderTargetToHdrWithDepth), as SkyboxPass does.
    b.ReadSRV(m_depth);
    // The DSV is bound with the depth test off (the fullscreen PSO's format match) - a
    // plane the DSV does not use.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::None);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds or
    // clears it) and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The pass textures: the raymarch and the light injection sample the CSM, the spot
    // tiers and the cloud shadow CloudPass baked earlier this frame (the graph brings the
    // spot tiers and the cloud shadow to their read states for this pass; the CSM is its
    // owner's). The froxel volume is this pass's own.
    b.ReadPassTextures({ RG::PassTex::kShadowCSM, RG::PassTex::kShadowSpotAtlas,
                         RG::PassTex::kShadowSpotAtlasMedium, RG::PassTex::kShadowSpotAtlasHigh,
                         RG::PassTex::kCloudShadow });
    // The scene-voxel occupancy, read by the light injection's compute dispatch: brought
    // to SHADER_RESOURCE_COMPUTE in this pass's Phase A and left there.
    b.ReadScoped(b.GetTexture(RG::PassTex::kSceneVoxelOccupancy), RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    b.WriteUAV(m_scatteringH);
}

void VolumetricFogPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // Fully-lit CSM stand-in, built like TransparentPass / ParticleRenderPass /
    // FaceOverlayPass / WaterSurfacePass's. VolumetricRaymarch.cs declares ShadowCascades
    // unconditionally and samples it, but the shadow array can be absent (ShadowPass::Init
    // logs an error and returns on allocation failure); binding nothing leaves the slot on
    // whatever descriptor was there - an undefined read, not a black shadow.
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
            LOG_ERROR("VolumetricFogPass: CSM fallback texture failed");
    }

    // Spot-shadow atlas 1x1 stand-in (t6 space2). SpotShadowPass allocates its 128 MiB
    // atlas lazily, so "no atlas" is the ordinary state of a scene without a
    // shadow-casting spot; both compute dispatches then bind this instead of leaving the
    // slot on the previous root signature's descriptor. Both details are load-bearing, as
    // for the CSM stand-in above: FroxelLightInject.cs / VolumetricRaymarch.cs declare
    // Texture2DArray<float> and index slices up to kMaxCasters-1 (arraySize > 1 for a
    // TEXTURE2DARRAY SRV), and the 0.0 texel is fully lit under reversed Z.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1; td.height = 1;
        td.arraySize = 8;   // > 1 for an ARRAY SRV
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // ONE entry PER SUBRESOURCE: CreateTexture uploads mipLevels * arraySize subresources
        // and INDEXES this pointer; a single-element init with arraySize > 1 reads past it (an
        // access violation inside the upload memcpy, not a validation error).
        RHI::SubresourceData init[8];
        for (auto& sd : init) sd = { &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(td, m_spotShadowAtlasFallbackTex, init))
            m_spotShadowAtlasFallback = gfx.GetTextureSRVGpuHandle(m_spotShadowAtlasFallbackTex);
        else
            LOG_ERROR("VolumetricFogPass: spot-shadow atlas fallback creation failed");
    }
    // Single-identity VP buffer for the spot-shadow VP fallback (t7 space2), shaped like
    // LightingPass's. Its content is moot while shadowSliceIdx stays 0xFFFFFFFF, but the
    // StructuredBuffer<float4x4> slot must name a real descriptor.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(SpotShadowViewGPU);
        bd.stride     = sizeof(SpotShadowViewGPU);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        SpotShadowViewGPU identity;
        DirectX::XMStoreFloat4x4(&identity.vp, DirectX::XMMatrixIdentity());
        if (gfx.CreateBuffer(bd, m_spotShadowVPFallbackBuf, &identity))
            m_spotShadowVPFallback = gfx.GetBufferSRVGpuHandle(m_spotShadowVPFallbackBuf);
        else
            LOG_ERROR("VolumetricFogPass: spot-shadow VP fallback creation failed");
    }

    // ---- Shader registration + PSO compile ----
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::FroxelDensity_CS,     RHI::ShaderStage::CS,
                         "FroxelDensity.cs.hlsl",     "main");
    m_shaderLib.Register(ShaderID::FroxelLightInject_CS, RHI::ShaderStage::CS,
                         "FroxelLightInject.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::FroxelScatter_CS,     RHI::ShaderStage::CS,
                         "FroxelScatter.cs.hlsl",     "main");
    m_shaderLib.Register(ShaderID::FroxelTemporal_CS,    RHI::ShaderStage::CS,
                         "FroxelTemporal.cs.hlsl",    "main");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS,   RHI::ShaderStage::VS,
                         "VolumetricApply.vs.hlsl",   "main");
    m_shaderLib.Register(ShaderID::VolumetricApply_PS,   RHI::ShaderStage::PS,
                         "VolumetricApply.ps.hlsl",   "main");
    m_shaderLib.Register(ShaderID::VolumetricRaymarch_CS,
                         RHI::ShaderStage::CS,
                         "VolumetricRaymarch.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VolumetricRaymarchTemporal_CS,
                         RHI::ShaderStage::CS,
                         "VolumetricRaymarchTemporal.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VolumetricRaymarchApply_PS,
                         RHI::ShaderStage::PS,
                         "VolumetricRaymarchApply.ps.hlsl", "main");

    CreateComputePipelines(gfx);

    // The apply graphics PSOs via PSOCache (the engine's global root signature).
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VolumetricApply_VS;
        desc.psID        = ShaderID::VolumetricApply_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cullMode         = RHI::CullMode::NONE;
        desc.rs.depthClipEnable = false;
        // Composite over HDR using premultiplied scattering and (1-T) alpha.
        desc.dss.depthEnable     = false;
        desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.renderTarget[0];
        bs.blendEnable           = true;
        bs.srcBlend              = RHI::Blend::ONE;
        bs.destBlend             = RHI::Blend::INV_SRC_ALPHA;
        bs.blendOp               = RHI::BlendOp::ADD;
        bs.srcBlendAlpha        = RHI::Blend::ZERO;
        bs.destBlendAlpha       = RHI::Blend::ONE;
        bs.blendOpAlpha         = RHI::BlendOp::ADD;
        bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("VolumetricFogPass: apply PSO failed");
    }

    // Raymarch apply graphics PSO - additive blend (src=ONE, dst=ONE), drawn after the froxel
    // VolumetricApply: the raymarch's in-scatter carries the medium transmittance from the
    // camera already, so the composite only adds it and must not modify the HDR alpha.
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VolumetricApply_VS;
        desc.psID        = ShaderID::VolumetricRaymarchApply_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cullMode         = RHI::CullMode::NONE;
        desc.rs.depthClipEnable = false;
        desc.dss.depthEnable     = false;
        desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.renderTarget[0];
        bs.blendEnable           = true;
        bs.srcBlend              = RHI::Blend::ONE;
        bs.destBlend             = RHI::Blend::ONE;
        bs.blendOp               = RHI::BlendOp::ADD;
        bs.srcBlendAlpha        = RHI::Blend::ZERO;
        bs.destBlendAlpha       = RHI::Blend::ONE;
        bs.blendOpAlpha         = RHI::BlendOp::ADD;
        bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("VolumetricFogPass: raymarch apply PSO failed");
    }

    // ---- 3D textures + descriptors ----
    // Not created here: the froxel volumes (70.3 MiB) are enable-gated in
    // EnsureFroxelResources (main thread, called by the Renderer each frame), like the
    // raymarch 2D set.

    // ---- CBs (one per frame in flight) ----
    if (!m_paramCB.Create(gfx, "VolumetricFogPass.ParamCB"))
        LOG_ERROR("VolumetricFogPass: ParamCB create failed");
    // The apply CB is a block of the frame's constants (WriteApplyCB).

    // Volumetric light StructuredBuffer<GPULight> (UPLOAD), one per frame in flight: the
    // scene lights that opted into volumetric (VolumetricLightComponent).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kMaxVolLights * sizeof(VolLight);
        bd.stride     = sizeof(VolLight);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_volLightsBuf[i]))
            {
                m_volLightsBufMapped[i] = gfx.MapBuffer(m_volLightsBuf[i]);
                m_lightsSrv[i]          = gfx.GetBufferSRVGpuHandle(m_volLightsBuf[i]);
            }
        }
    }

    // Local fog volume StructuredBuffer<LocalFogVolumeGPU> (UPLOAD), the same ring shape.
    // 32 x 96 B x 3 = 9 KiB, always resident: Density and LightInject declare t5
    // unconditionally, and a zero-count frame still has to name a live descriptor there.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kMaxLocalFogVolumes * sizeof(LocalFogVolumeGPU);
        bd.stride     = sizeof(LocalFogVolumeGPU);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_localFogBuf[i]))
            {
                m_localFogBufMapped[i] = gfx.MapBuffer(m_localFogBuf[i]);
                m_localFogSrv[i]       = gfx.GetBufferSRVGpuHandle(m_localFogBuf[i]);
                if (m_localFogBufMapped[i])
                    std::memset(m_localFogBufMapped[i], 0, bd.size);
            }
            else
                LOG_ERROR("VolumetricFogPass: local fog volume buffer %u create failed", i);
        }
    }

    // ---- Samplers ----
    // None to create: the compute passes use the compute signature's static samplers, the
    // two apply shaders the fixed SMP_LINEAR_CLAMP heap slot.

    LOG_SUCCESS("VolumetricFogPass: initialised (%ux%ux%u)",
                kFroxelW, kFroxelH, kFroxelD);
}

// Onto the live members: a failed rebuild keeps the old pipeline.
void VolumetricFogPass::CreateComputePipelines(IGraphicsDevice& gfx)
{
    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag)
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("VolumetricFogPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("VolumetricFogPass: %s PSO failed", tag);
    };
    makeCs(ShaderID::FroxelDensity_CS,             m_densityPSO,       "Density");
    makeCs(ShaderID::FroxelLightInject_CS,         m_lightInjectPSO,   "LightInject");
    makeCs(ShaderID::FroxelScatter_CS,             m_scatterPSO,       "Scatter");
    makeCs(ShaderID::FroxelTemporal_CS,            m_temporalPSO,      "Temporal");
    makeCs(ShaderID::VolumetricRaymarch_CS,        m_rmPSO,            "Raymarch");
    makeCs(ShaderID::VolumetricRaymarchTemporal_CS,m_rmTemporalPSO,    "RaymarchTemporal");
}

void VolumetricFogPass::ReloadShaders(IGraphicsDevice& gfx)
{
    // The ray-traced builds come from the disk cache, which tracks their sources itself: they are
    // rebuilt the next frame the tier runs, and a failed build gets another try.
    ReleaseRtPipelines(gfx);
    m_rtFailed = false;
    if (!m_shaderLib.ClearCaches()) return;   // none of its shaders changed
    m_psoCache.Clear();          // the apply draws: rebuilt at their next Execute
    CreateComputePipelines(gfx);
}

uint8_t VolumetricFogPass::RtMode()
{
    const std::string mode = Settings::Pass::volumetricFogRtLighting();
    return mode == "on" ? 2 : mode == "auto" ? 1 : 0;
}

uint8_t VolumetricFogPass::RtIndirectMode()
{
    const std::string mode = Settings::Pass::volumetricFogRtIndirect();
    return mode == "rays" ? 3 : mode == "ddgi" ? 2 : mode == "auto" ? 1 : 0;
}

bool VolumetricFogPass::RtUsable() const
{
    return m_gfx && m_gfx->GetCapabilities().inlineRaytracing && !m_rtFailed;
}

Media::FroxelVolume VolumetricFogPass::LastLitVolume(const ViewState& view) const
{
    Media::FroxelVolume v;
    const History* hist = view.Find<History>(this);
    if (!hist || !hist->lit || !m_gfx || !hist->tex[hist->readIdx].IsValid()) return v;
    const uint32_t index = m_gfx->DescriptorHeapIndexFromGpuHandle(m_gfx->GetTextureSRVGpuHandle(hist->tex[hist->readIdx]));
    if (index == ~0u) return v;
    v.srv        = index;
    v.viewProj   = hist->prevViewProj;
    v.camera     = hist->prevCameraPos;
    v.froxelNear = EffectiveFroxelNear();
    v.froxelFar  = EffectiveFroxelFar();
    return v;
}

void VolumetricFogPass::SetRt(RtTier tier, const RtInputs& in)
{
    m_rtTier = tier;
    m_rtIn = in;
    if (tier != RtTier::Off && m_gfx && !m_rtInjectPSO.IsValid() && !m_rtFailed)
        BuildRtPipelines(*m_gfx);
}

bool VolumetricFogPass::BuildRtPipelines(IGraphicsDevice& gfx)
{
    m_rtFailed = true;   // cleared once every pipeline is built
    auto load = [&](const char* file, std::vector<DxcCompiler::Define> defines) {
        return ShaderDiskCache::LoadOrCompile(file, "main", RHI::ShaderStage::CS, defines, "VolumetricFogPass");
    };
    auto make = [&](const std::vector<uint8_t>& dxil, const RHI::PipelineLayout* layout, RHI::PipelineState& out) {
        if (dxil.empty()) return false;
        RHI::Shader cs;
        RHI::PipelineStateDesc d{};
        d.cs = &cs;
        d.layout = layout;
        const bool ok = gfx.CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), cs) &&
                        gfx.CreatePipelineState(d, out);
        gfx.DestroyShader(cs);   // the PSO keeps its own copy of the DXIL
        return ok;
    };
    const std::vector<DxcCompiler::Define> rt = { { "FROXEL_RT", "1" } };
    bool ok = m_rtLayout.IsValid() || ReSTIR::CreateShellLayout(gfx, m_rtLayout, "VolumetricFogRT");
    ok = ok && make(load("shaders/FroxelDensity.cs.hlsl", rt), nullptr, m_rtDensityPSO)
            && make(load("shaders/FroxelTemporal.cs.hlsl", rt), nullptr, m_rtTemporalPSO)
            && make(load("shaders/FroxelLightInjectRT.cs.hlsl", {}), &m_rtLayout, m_rtInjectPSO)
            && make(load("shaders/FroxelIndirectRT.cs.hlsl", {}), &m_rtLayout, m_rtIndirectPSO)
            && make(ShaderDiskCache::LoadOrCompile("shaders/FroxelIndirectRT.cs.hlsl", "Filter", RHI::ShaderStage::CS,
                                                   {}, "VolumetricFogPass"), &m_rtLayout, m_rtIndirectFilterPSO);
    if (!ok)
    {
        ReleaseRtPipelines(gfx);
        LOG_ERROR("VolumetricFogPass: ray-traced lighting pipelines failed - the froxels keep the raster lighting");
        return false;
    }
    m_rtFailed = false;
    // The indirect rays' SHaRC query, where the device and the tree can hold the cache: without it
    // they take the sky and the emitters alone.
    const char* why = SharcCache::InertReason(gfx, true);
    if (!why)
    {
        const std::vector<uint8_t> dxil = ShaderDiskCache::LoadOrCompile(
            "shaders/FroxelIndirectRT.cs.hlsl", "main", RHI::ShaderStage::CS, { { "FROXEL_INDIRECT_SHARC", "1" } },
            "VolumetricFogPass", ShaderDiskCache::CompileFlags{ false, true });
        if (!make(dxil, &m_rtLayout, m_rtIndirectSharcPSO)) why = "pipeline";
    }
    LOG_INFO("[VOLFOG_RT_INDIRECT] pipelines=%u sharc=%s", m_rtIndirectSharcPSO.IsValid() ? 2u : 1u, why ? why : "ready");
    // The per-pixel shafts: without them the froxels keep the sun.
    if (!make(ShaderDiskCache::LoadOrCompile("shaders/FroxelLightInjectRT.cs.hlsl", "Shafts", RHI::ShaderStage::CS, {},
                                             "VolumetricFogPass"), &m_rtLayout, m_rtShaftsPSO))
        LOG_WARNING("VolumetricFogPass: the ray-traced shafts' pipeline failed - the froxels keep the sun");
    return true;
}

void VolumetricFogPass::ReleaseRtPipelines(IGraphicsDevice& gfx)
{
    for (RHI::PipelineState* p : { &m_rtDensityPSO, &m_rtInjectPSO, &m_rtTemporalPSO, &m_rtIndirectPSO,
                                   &m_rtIndirectSharcPSO, &m_rtIndirectFilterPSO, &m_rtShaftsPSO })
        gfx.DestroyPipelineState(*p);
    gfx.DestroyPipelineLayout(m_rtLayout);
}

uint32_t VolumetricFogPass::ResolveIndirect(bool& sharc) const
{
    // auto: DDGI where it runs, else the rays into the path tracer's SHaRC cache, else the constant
    // ambient. ddgi / rays: that source alone (the rays without the cache: the sky and the emitters).
    sharc = false;
    const uint8_t mode = RtIndirectMode();
    const bool cache = m_rtIndirectSharcPSO.IsValid() && m_rtIn.sharc.indices.capacity != 0;
    if ((mode == 1 || mode == 2) && m_rtIn.ddgi) return kIndirectDdgi;
    if ((mode == 1 && cache) || mode == 3)
    {
        sharc = cache;
        return kIndirectRays;
    }
    return kIndirectConstant;
}

bool VolumetricFogPass::RtRuns() const
{
    return WantsFog() && Shown() && m_v->densityTex.IsValid() && m_rtTier != RtTier::Off && m_rtInjectPSO.IsValid() &&
           m_rtIn.tlasVA && m_rtIn.instanceVA && m_rtIn.buffers && m_hist && m_hist->rtReservoir[0].IsValid() &&
           m_hist->rtReservoir[1].IsValid();
}

void VolumetricFogPass::EnsureRtVolumes(bool rtOn)
{
    // The bound view's history: 2 x 14.1 MiB of light reservoirs and 2 x 0.5 MiB of indirect grids.
    if (!m_hist) return;
    History& h = *m_hist;
    if (rtOn && !h.rtReservoir[0].IsValid())
    {
        RHI::GPUBufferDesc bd{};
        bd.usage     = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        bd.stride    = kRtReservoirBytes;
        bd.size      = uint64_t(kFroxelW) * kFroxelH * kFroxelD * kRtReservoirBytes;
        bool ok = true;
        for (RHI::GPUBuffer& b : h.rtReservoir) ok = ok && m_gfx->CreateBuffer(bd, b);
        for (RHI::ResourceState& s : h.rtReservoirState) s = RHI::ResourceState::UNDEFINED;
        RHI::TextureDesc td = FroxelDesc();
        td.width  = kIndirectW;
        td.height = kIndirectH;
        td.depth  = kIndirectD;
        td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        for (uint32_t i = 0; i < 2; ++i)
        {
            td.debugName = i ? "VolFog.Indirect[1]" : "VolFog.Indirect[0]";
            ok = ok && m_gfx->CreateTexture(td, h.rtIndirect[i]);
        }
        td.debugName = "VolFog.IndirectSmoothed";
        ok = ok && m_gfx->CreateTexture(td, h.rtIndirectSmoothed);
        h.rtLast        = false;
        h.rtIndirectRan = false;
        if (!ok)
        {
            h.ReleaseRt(*m_gfx);
            LOG_ERROR("VolumetricFogPass: the ray-traced lighting's reservoirs failed");
        }
        else
            LOG_INFO("VolumetricFogPass: ray-traced lighting reservoirs allocated (%.1f MiB)",
                     2.0 * double(bd.size) / (1024.0 * 1024.0));
    }
    else if (!rtOn && h.rtReservoir[0].IsValid())
    {
        h.ReleaseRt(*m_gfx);
        LOG_INFO("VolumetricFogPass: ray-traced lighting reservoirs released");
    }
}

void VolumetricFogPass::EnsureFroxelResources(const FogDemand& demand)
{
    if (!m_gfx) return;
    // The main view's volumes: the frame boundary binds it (a secondary view follows in BindView).
    if (!ENGINE_CHECK_ONCE(m_v == &m_mainVol, "VolumetricFogPass: a secondary view bound at the frame boundary"))
        return;

    // Same formula as WantsFog() / WantsGodRays(), on the game-half demand instead of the
    // render-half SceneFog (see FogDemand).
    const bool fogOn = m_allowed && ((demand.sceneOwned ? demand.sceneEnabled : m_enabled)
                                     || demand.anyLocal);
    const bool rmOn  = fogOn && ((demand.sceneOwned ? demand.sceneGodRays : m_rmEnabled) ||
                                 (demand.rt && Settings::Pass::volumetricFogRtShafts() &&
                                  m_gfx->GetCapabilities().inlineRaytracing));

    // ---- Per-pixel raymarch tier (3 x half-res RGBA16F, 11.9 MiB at 1080p) ----
    // Created and released here on the main thread, not in Execute: descriptor allocation
    // must stay off the recording workers, and with fog or god rays off the composite
    // cannot run, so the textures are freed.
    if (rmOn)
    {
        const uint32_t renderW = m_gfx->GetRenderWidth();
        const uint32_t renderH = m_gfx->GetRenderHeight();
        if (renderW > 0 && renderH > 0 &&
            (renderW != m_v->rmLastFullW || renderH != m_v->rmLastFullH))
            RebuildRaymarchTextures(renderW, renderH);
    }
    else if (m_v->rmCurrentTex.IsValid())
    {
        DestroyRaymarchTextures();
        // Zero the size stamp or a re-enable at the SAME resolution reads
        // "unchanged" and never rebuilds.
        m_v->rmLastFullW = 0;
        m_v->rmLastFullH = 0;
        LOG_INFO("VolumetricFogPass: raymarch half-res released "
                 "(%s, 11.9 MiB @1080p reclaimed)",
                 fogOn ? "god rays disabled" : "fog disabled");
    }

    // ---- The ray-traced lighting's light reservoirs and indirect grids, while its tier may run ----
    // The secondary views follow the main view's (MatchMainVolumes).
    m_rtVolumes = fogOn && demand.rt && m_gfx->GetCapabilities().inlineRaytracing;
    EnsureRtVolumes(m_rtVolumes);

    if (fogOn)
    {
        if (m_v->densityTex.IsValid())
        {
            // Steady state - unless another arena resident grew the shared heap since the trio
            // was placed: rebuild the TRANSIENTS on the current heap (the history is committed and
            // carries real cross-frame content, so it stays). This runs at the frame boundary
            // (main thread, before any recording), so recreating is safe.
            if (m_aliasArena && m_placedMask &&
                m_placedEpoch != m_aliasArena->Epoch())
            {
                for (RHI::Texture* t : { &m_v->densityTex, &m_v->lightingTex, &m_v->scatteringTex })
                    if (t->IsValid()) m_gfx->DestroyTexture(*t);
                m_v->densityTex = {}; m_v->lightingTex = {}; m_v->scatteringTex = {};
                m_placedMask = 0;
                CreateTransient3DTextures(*m_gfx);
            }
            return;
        }
        Create3DTextures(*m_gfx);
        // Fresh volumes hold garbage - force the temporal path to treat the first frame as a
        // reset (the flag a teleport uses).
        if (m_hist) m_hist->valid = false;
        LOG_INFO("VolumetricFogPass: froxel volumes allocated (70.3 MiB, fog enabled)");
        return;
    }

    if (!m_v->densityTex.IsValid()) return;
    ReleaseFroxelVolumes();
    m_placedMask   = 0;   // no live placed transients -> no alias barriers
    LOG_INFO("VolumetricFogPass: froxel volumes released (fog disabled, 70.3 MiB reclaimed)");
}

// DestroyTexture defers internally for kFrameCount frames, so in-flight dispatches finish
// safely; history is rebuilt from scratch on the next enable anyway.
void VolumetricFogPass::ReleaseFroxelVolumes()
{
    for (RHI::Texture* t : { &m_v->densityTex, &m_v->lightingTex, &m_v->scatteringTex })
        if (t->IsValid()) m_gfx->DestroyTexture(*t);
    if (m_hist)   // the froxel history, after the trio
        for (RHI::Texture* t : { &m_hist->tex[0], &m_hist->tex[1] })
            if (t->IsValid()) m_gfx->DestroyTexture(*t);
    m_v->densityTex = {}; m_v->lightingTex = {}; m_v->scatteringTex = {};
    if (m_hist)
    {
        m_hist->tex[0] = {}; m_hist->tex[1] = {};
        m_hist->valid = false;
    }
}

RHI::TextureDesc VolumetricFogPass::FroxelDesc()
{
    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
    td.width      = kFroxelW;
    td.height     = kFroxelH;
    td.depth      = kFroxelD;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    return td;
}

// The main view's are placed in the shared alias arena when there is one (its 42.2 MiB aliases
// the RTR / XeGTAO / Bloom transients; each is fully rewritten before any read), else committed;
// a secondary view's are committed.
void VolumetricFogPass::CreateTransient3DTextures(IGraphicsDevice& gfx)
{
    struct T { const char* tag; RHI::Texture* tex; RHI::ResourceState* st; };
    const T transients[] = {
        { "VolFog.Density",    &m_v->densityTex,    &m_v->densityState },
        { "VolFog.Lighting",   &m_v->lightingTex,   &m_v->lightingState },
        { "VolFog.Scattering", &m_v->scatteringTex, &m_v->scatteringState },
    };
    constexpr int kCount = int(sizeof(transients) / sizeof(transients[0]));

    const bool main = m_v == &m_mainVol;
    bool placed = false;   // in the arena's heap
    uint64_t offsets[kCount]{};
    if (m_aliasArena && main)
    {
        uint64_t cursor = 0;
        bool ok = true;
        for (int i = 0; i < kCount; ++i)
        {
            uint64_t sz = 0, al = 0;
            if (!gfx.GetTextureAllocationInfo(FroxelDesc(), sz, al)) { ok = false; break; }
            offsets[i] = TransientAliasArena::AlignUp(cursor, al);
            cursor     = offsets[i] + sz;
        }
        if (ok)
            placed = gfx.EnsureAliasArena(*m_aliasArena, cursor);
    }

    if (main) m_placedMask = 0;
    for (int i = 0; i < kCount; ++i)
    {
        RHI::TextureDesc td = FroxelDesc();
        td.debugName = transients[i].tag;
        bool created = false;
        if (placed && gfx.CreateTexturePlaced(td, *m_aliasArena, offsets[i], *transients[i].tex))
        {
            created = true;
            m_placedMask |= uint8_t(1u << i);
        }
        if (!created && !gfx.CreateTexture(td, *transients[i].tex))
            LOG_ERROR("VolumetricFogPass: %s 3D texture create failed", transients[i].tag);
        // Keep the CPU state shadow in lockstep with the creation layout: every dispatch
        // sequence returns the volumes to SR_COMPUTE, but a recreate must not inherit a stale
        // mid-frame shadow.
        *transients[i].st = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }
    if (m_aliasArena && main)
        m_placedEpoch = m_aliasArena->Epoch();
}

void VolumetricFogPass::Create3DTextures(IGraphicsDevice& gfx)
{
    CreateTransient3DTextures(gfx);
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr,
                           "VolumetricFogPass: no view bound (PassSet::BindView) - no froxel history"))
        return;

    // Temporal history ping-pong, committed on purpose: it carries last frame's scattering
    // content across the frame boundary (never in the arena).
    for (int i = 0; i < 2; ++i)
    {
        char name[24];
        std::snprintf(name, sizeof(name), "History[%d]", i);
        RHI::TextureDesc td = FroxelDesc();
        td.debugName = name;
        if (!gfx.CreateTexture(td, m_hist->tex[i]))
            LOG_ERROR("VolumetricFogPass: %s 3D texture create failed", name);
        m_hist->state[i] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }
}

void VolumetricFogPass::DestroyRaymarchTextures()
{
    if (!m_gfx) return;
    if (m_v->rmCurrentTex.IsValid())    m_gfx->DestroyTexture(m_v->rmCurrentTex);
    m_v->rmCurrentTex = {};
    if (!m_hist) return;
    for (int i = 0; i < 2; ++i)
        if (m_hist->rmTex[i].IsValid()) m_gfx->DestroyTexture(m_hist->rmTex[i]);
    m_hist->rmTex[0] = {};
    m_hist->rmTex[1] = {};
    m_hist->rmValid = false;
}

void VolumetricFogPass::RebuildRaymarchTextures(uint32_t fullW, uint32_t fullH)
{
    if (!m_gfx) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr,
                           "VolumetricFogPass: no view bound (PassSet::BindView) - no raymarch history"))
        return;
    DestroyRaymarchTextures();

    // Half res matches the 32-step raymarch budget; rounding up keeps odd sizes
    // (1921x1081) at a clean 2x.
    m_v->rmHalfW = (fullW + 1) / 2;
    m_v->rmHalfH = (fullH + 1) / 2;
    if (m_v->rmHalfW == 0 || m_v->rmHalfH == 0) return;

    auto make2D = [&](RHI::Texture& outTex, const char* tag)
    {
        RHI::TextureDesc td{};
        td.width      = m_v->rmHalfW;
        td.height     = m_v->rmHalfH;
        td.depth      = 1;
        td.mipLevels = 1;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        if (!m_gfx->CreateTexture(td, outTex))
            LOG_ERROR("VolumetricFogPass: %s half-res texture create failed", tag);
    };
    make2D(m_v->rmCurrentTex,     "RM.Current");
    make2D(m_hist->rmTex[0],   "RM.History[0]");
    make2D(m_hist->rmTex[1],   "RM.History[1]");

    m_v->rmCurrentState    = RHI::ResourceState::UNORDERED_ACCESS;
    m_hist->rmState[0]  = RHI::ResourceState::UNORDERED_ACCESS;
    m_hist->rmState[1]  = RHI::ResourceState::UNORDERED_ACCESS;
    m_v->rmLastFullW = fullW;
    m_v->rmLastFullH = fullH;
    m_hist->rmValid = false;
    LOG_INFO("VolumetricFogPass: raymarch half-res rebuilt %ux%u", m_v->rmHalfW, m_v->rmHalfH);
}

void VolumetricFogPass::SetVolumetricLights(const std::vector<VolLight>& lights)
{
    m_spotLightCount = static_cast<uint32_t>((std::min)(lights.size(),
                                                        size_t(kMaxVolLights)));
    if (!m_gfx || m_spotLightCount == 0) return;
    const uint32_t s = m_gfx->GetFrameIndex();
    if (s < kFrameCount && m_volLightsBufMapped[s])
    {
        std::memcpy(m_volLightsBufMapped[s], lights.data(),
                    m_spotLightCount * sizeof(VolLight));
    }
}

void VolumetricFogPass::SetSceneFog(const SceneFog& f)
{
    m_scene = f;
}

bool VolumetricFogPass::WriteMediumGlobal(Media::Global& out) const
{
    if (!m_allowed)
        return false;
    // The local volumes scatter with the global medium's asymmetry, density or not.
    out.anisotropy = std::clamp(m_scene.owned ? m_scene.anisotropy : m_anisotropy, -0.95f, 0.95f);
    if (m_scene.owned)
    {
        if (!m_scene.enabled || !(m_scene.density > 0.0f))
            return false;
        // A medium's albedo above 1 creates energy; HG at |g| = 1 is a delta.
        out.density     = m_scene.density;
        out.albedo[0]   = std::clamp(m_scene.albedo.x, 0.0f, 1.0f);
        out.albedo[1]   = std::clamp(m_scene.albedo.y, 0.0f, 1.0f);
        out.albedo[2]   = std::clamp(m_scene.albedo.z, 0.0f, 1.0f);
        out.emission[0] = std::max(m_scene.emissive.x, 0.0f);
        out.emission[1] = std::max(m_scene.emissive.y, 0.0f);
        out.emission[2] = std::max(m_scene.emissive.z, 0.0f);
        out.baseHeight  = m_scene.baseHeight;
        out.falloff     = std::max(m_scene.heightFalloff, 0.0f);
        return true;
    }
    // The pass-owned fog in FroxelDensity's terms: sigma_t = d (s + a), sigma_s = d s.
    const float scatterPlusAbsorb = m_fogScattering + m_fogAbsorption;
    if (!m_enabled || !(m_fogDensity > 0.0f) || !(scatterPlusAbsorb > 0.0f))
        return false;
    const float albedo = std::clamp(m_fogScattering / scatterPlusAbsorb, 0.0f, 1.0f);
    out.density    = m_fogDensity * scatterPlusAbsorb;
    out.albedo[0]  = out.albedo[1] = out.albedo[2] = albedo;
    out.emission[0] = out.emission[1] = out.emission[2] = 0.0f;
    out.baseHeight = m_heightStart;
    out.falloff    = std::max(m_heightFalloff, 0.0f);
    return true;
}

void VolumetricFogPass::LogSceneFogStateIfChanged()
{
    const uint32_t state = (m_scene.owned   ? 1u : 0u)
                         | (m_scene.enabled ? 2u : 0u)
                         | (m_enabled       ? 4u : 0u)
                         | (m_allowed       ? 8u : 0u)
                         | (m_localCount         << 8)
                         | (m_localEmissiveCount << 16);
    if (state == m_loggedFogState) return;
    m_loggedFogState = state;
    // The player's allowance caps all of it.
    const char* capped = m_allowed ? "" : " - all off: the player's Volumetric Fog setting";
    if (m_scene.owned)
        LOG_INFO("VolumetricFogPass: scene-owned fog (global %s), %u local volume(s), %u emissive%s",
                 m_scene.enabled ? "on" : "off", m_localCount, m_localEmissiveCount, capped);
    else
        LOG_INFO("VolumetricFogPass: legacy fog (global %s), %u local volume(s), %u emissive%s",
                 m_enabled ? "on" : "off", m_localCount, m_localEmissiveCount, capped);
}

void VolumetricFogPass::SetLocalFogVolumes(const std::vector<LocalFogVolumeGPU>& volumes,
                                           uint32_t emissiveCount)
{
    uint32_t n = static_cast<uint32_t>((std::min)(volumes.size(),
                                                  size_t(kMaxLocalFogVolumes)));
    const uint32_t s = m_gfx ? m_gfx->GetFrameIndex() : kFrameCount;
    // No slot to write into -> nothing can be read either: report zero volumes
    // rather than letting the shaders walk a stale ring slot.
    if (s >= kFrameCount || !m_localFogBufMapped[s]) n = 0;
    if (n > 0)
        std::memcpy(m_localFogBufMapped[s], volumes.data(), n * sizeof(LocalFogVolumeGPU));
    m_localCount         = n;
    m_localEmissiveCount = (std::min)(emissiveCount, n);
    // Renderer pushes SetSceneFog immediately before this, once per frame.
    LogSceneFogStateIfChanged();
}

void VolumetricFogPass::SetFrameState(const DirectX::XMFLOAT4X4& viewProj,
                                       const DirectX::XMFLOAT4X4& invViewProj,
                                       const DirectX::XMFLOAT4X4& /*prevViewProj*/,
                                       const DirectX::XMFLOAT3&   cameraPos,
                                       float nearPlane, float farPlane,
                                       uint32_t frameIndex)
{
    // @p prevViewProj is ignored: Execute snapshots the current viewProj at its END, so
    // the temporal pass always reprojects against its own previous-frame matrix.
    //
    // First frame: an identity prevViewProj sends every reprojected UV out of bounds, the
    // temporal pass copies the current values into the history for every cell, and the
    // next frame blends against a one-frame snapshot instead of a running average. Seeding
    // it with the current VP makes frame 0's reprojection a no-op.
    if (m_hist && !m_hist->firstFrameDone)
    {
        m_hist->prevViewProj   = viewProj;
        m_hist->prevCameraPos  = cameraPos;
        m_hist->firstFrameDone = true;
    }
    m_v->viewProj      = viewProj;
    m_v->invViewProj   = invViewProj;
    m_v->cameraPos     = cameraPos;
    m_v->nearPlane     = nearPlane;
    m_v->farPlane      = farPlane;
    m_v->frameIndex    = frameIndex;
}

void VolumetricFogPass::SetShadowState(uint64_t shadowSrvHandle,
                                       const DirectX::XMFLOAT4X4 shadowMatrix[3],
                                       const DirectX::XMFLOAT3& cascadeSplits,
                                       float texelSize, float bias, float strength)
{
    m_v->shadowSrv         = shadowSrvHandle;
    for (int i = 0; i < 3; ++i) m_v->shadowMatrix[i] = shadowMatrix[i];
    m_v->cascadeSplits     = cascadeSplits;
    m_v->shadowTexelSize   = texelSize;
    m_v->shadowBias        = bias;
    m_v->shadowStrength    = strength;
}

static void TransitionTex(IGraphicsDevice& gfx,
                          const RHI::Texture& tex,
                          RHI::ResourceState& state,
                          RHI::ResourceState to,
                          RHI::CommandList cl)
{
    if (state == to) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}

void VolumetricFogPass::DispatchDensity(RHI::CommandList cl, const RHI::PipelineState& pso)
{
    if (!pso.IsValid() || !m_v->densityTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_v->densityTex, m_v->densityState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    // Local fog volumes (t5 space2) - always bound, see Init.
    if (const uint64_t lf = m_localFogSrv[gfx.GetFrameIndex() % kFrameCount])
        gfx.SetComputeDescriptorTable(C::SRV_t5, lf, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_v->densityTex), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);

    TransitionTex(gfx, m_v->densityTex, m_v->densityState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchLightInject(RHI::CommandList cl)
{
    if (!m_lightInjectPSO.IsValid() || !m_v->lightingTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_v->lightingTex, m_v->lightingState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_lightInjectPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_v->densityTex), cl);
    // CSM shadow array (sun god rays): ShadowPass leaves it in DEPTH_READ_SRV
    // (DEPTH_READ | PIXEL_SR | NON_PIXEL_SR), so the compute path reads it directly; the
    // stand-in when it is absent.
    {
        const uint64_t shadowSrv = m_v->shadowSrv ? m_v->shadowSrv : m_shadowFallbackHandle;
        if (shadowSrv) gfx.SetComputeDescriptorTable(kSRVT1, shadowSrv, cl);
    }
    // The volumetric lights' GPULight buffer (point + spot).
    {
        const uint64_t lightsSrv = m_lightsSrv[gfx.GetFrameIndex()];
        if (lightsSrv)
            gfx.SetComputeDescriptorTable(C::SRV_t2, lightsSrv, cl);
    }
    // Scene depth (t3 space2): ScreenSpaceShadow in the shader keeps spot / point lights
    // from beaming through walls. Setup's ReadSRV(m_depth) already put it in
    // DEPTH_READ_SRV, so no manual barrier.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(C::SRV_t3,
                                      gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // Scene occupancy (t4 space2), optional: unbound, the shader uses
    // ScreenSpaceShadow-only visibility.
    if (m_v->voxelOccupancySrv)
        gfx.SetComputeDescriptorTable(C::SRV_t4,
                                      m_v->voxelOccupancySrv, cl);
    // Spot-light shadow atlas + VP matrices (t6 / t7 space2). Only sampled for a light
    // with shadowSliceIdx != 0xFFFFFFFF, but the slots must ALWAYS name a live descriptor:
    // without a resident SpotShadowPass atlas (normal in a scene with no shadow-casting
    // spot) bind Init's 1x1 stand-ins.
    {
        const uint64_t atlas = m_spotShadowAtlasSrv ? m_spotShadowAtlasSrv
                                                    : m_spotShadowAtlasFallback;
        const uint64_t vps   = m_spotShadowVPSrv    ? m_spotShadowVPSrv
                                                    : m_spotShadowVPFallback;
        if (atlas) gfx.SetComputeDescriptorTable(C::SpotShadowAtlas, atlas, cl);
        if (vps)   gfx.SetComputeDescriptorTable(C::SpotShadowVP,    vps,   cl);
    }
    // Local fog volumes (t5 space2) - the emissive ones glow here.
    if (const uint64_t lf = m_localFogSrv[gfx.GetFrameIndex() % kFrameCount])
        gfx.SetComputeDescriptorTable(C::SRV_t5, lf, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_v->lightingTex), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);

    TransitionTex(gfx, m_v->lightingTex, m_v->lightingState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

// The indirect grid, before the injection reads it: on the injection's shell layout and bindings,
// this frame's grid at u0 (the kernels open the grids through the heap); @p smooth: then the rays'
// grid through the Filter entry.
void VolumetricFogPass::DispatchIndirectRT(RHI::CommandList cl, const RHI::PipelineState& pso, bool smooth)
{
    RHI::Texture& grid = m_hist->rtIndirect[m_hist->rtIndirectSlot];
    if (!pso.IsValid() || !grid.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;
    namespace RS = ReSTIR::ShellSlots;

    gfx.BindComputePipelineState(pso, cl);
    gfx.SetComputeRootCBV(RS::CB, m_paramCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootCBV(RS::CoreCB, m_paramCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(RS::TLAS, m_rtIn.tlasVA, cl);
    gfx.SetComputeRootSRVAddress(RS::Instances, m_rtIn.instanceVA, cl);
    gfx.SetComputeRootSRVAddress(RS::Alpha, m_rtIn.alphaVA ? m_rtIn.alphaVA : m_rtIn.instanceVA, cl);
    gfx.SetComputeDescriptorTable(RS::Buffers, m_rtIn.buffers, cl);
    gfx.SetComputeDescriptorTable(RS::Textures, gfx.GetBindlessTextureTableGpuHandle(), cl);
    gfx.SetComputeDescriptorTable(RS::Result, gfx.GetTextureUAVGpuHandle(grid), cl);
    gfx.DispatchCompute((kIndirectW + 3) / 4, (kIndirectH + 3) / 4, (kIndirectD + 3) / 4, cl);
    // Filter reads this frame's grid (and next frame's kernel), the injection the smoothed one.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);
    if (smooth && m_rtIndirectFilterPSO.IsValid() && m_hist->rtIndirectSmoothed.IsValid())
    {
        gfx.BindComputePipelineState(m_rtIndirectFilterPSO, cl);
        gfx.DispatchCompute((kIndirectW + 3) / 4, (kIndirectH + 3) / 4, (kIndirectD + 3) / 4, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);
    }
}

void VolumetricFogPass::DispatchShaftsRT(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = *m_gfx;
    namespace RS = ReSTIR::ShellSlots;

    TransitionTex(gfx, m_v->rmCurrentTex, m_v->rmCurrentState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    gfx.BindComputePipelineState(m_rtShaftsPSO, cl);
    gfx.SetComputeRootCBV(RS::CB, m_paramCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootCBV(RS::CoreCB, m_paramCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(RS::TLAS, m_rtIn.tlasVA, cl);
    gfx.SetComputeRootSRVAddress(RS::Instances, m_rtIn.instanceVA, cl);
    gfx.SetComputeRootSRVAddress(RS::Alpha, m_rtIn.alphaVA ? m_rtIn.alphaVA : m_rtIn.instanceVA, cl);
    gfx.SetComputeDescriptorTable(RS::Buffers, m_rtIn.buffers, cl);
    gfx.SetComputeDescriptorTable(RS::Textures, gfx.GetBindlessTextureTableGpuHandle(), cl);
    // The kernel opens its output from the heap (FroxelParams::shaftsIdx); the slot holds it too.
    gfx.SetComputeDescriptorTable(RS::Result, gfx.GetTextureUAVGpuHandle(m_v->rmCurrentTex), cl);
    gfx.DispatchCompute((m_v->rmHalfW + 7) / 8, (m_v->rmHalfH + 7) / 8, 1, cl);
    TransitionTex(gfx, m_v->rmCurrentTex, m_v->rmCurrentState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

// On ReSTIR's shell layout: the froxel constants at b0 (and b1), the TLAS and the instance and
// alpha tables as root SRVs, the bindless tables, the lighting volume at u0. Everything else the
// kernel opens through the heap (FroxelParams' FROXEL_RT block). The dispatches after it bind
// the compute signature again with their pipelines.
void VolumetricFogPass::DispatchLightInjectRT(RHI::CommandList cl)
{
    if (!m_v->lightingTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;
    namespace RS = ReSTIR::ShellSlots;

    TransitionTex(gfx, m_v->lightingTex, m_v->lightingState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    for (uint32_t i = 0; i < 2; ++i)
        if (m_hist->rtReservoirState[i] != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_hist->rtReservoir[i], m_hist->rtReservoirState[i],
                                                    RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_hist->rtReservoirState[i] = RHI::ResourceState::UNORDERED_ACCESS;
        }

    gfx.BindComputePipelineState(m_rtInjectPSO, cl);
    gfx.SetComputeRootCBV(RS::CB, m_paramCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootCBV(RS::CoreCB, m_paramCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(RS::TLAS, m_rtIn.tlasVA, cl);
    gfx.SetComputeRootSRVAddress(RS::Instances, m_rtIn.instanceVA, cl);
    gfx.SetComputeRootSRVAddress(RS::Alpha, m_rtIn.alphaVA ? m_rtIn.alphaVA : m_rtIn.instanceVA, cl);
    gfx.SetComputeDescriptorTable(RS::Buffers, m_rtIn.buffers, cl);
    gfx.SetComputeDescriptorTable(RS::Textures, gfx.GetBindlessTextureTableGpuHandle(), cl);
    gfx.SetComputeDescriptorTable(RS::Result, gfx.GetTextureUAVGpuHandle(m_v->lightingTex), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);
    // The reservoirs are read next frame: a UAV barrier orders this frame's writes before them.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);

    TransitionTex(gfx, m_v->lightingTex, m_v->lightingState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchScatter(RHI::CommandList cl, uint64_t srcSrv)
{
    if (!m_scatterPSO.IsValid() || !m_v->scatteringTex.IsValid() || !srcSrv) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_v->scatteringTex, m_v->scatteringState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_scatterPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0, srcSrv, cl);
    gfx.SetComputeDescriptorTable(kUAV0,  gfx.GetTextureUAVGpuHandle(m_v->scatteringTex), cl);
    // XY only - the shader walks Z.
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, 1, cl);

    // Scatter is the LAST froxel compute stage and its output the only thing the apply PS
    // samples, so it goes straight to pixel SR.
    TransitionTex(gfx, m_v->scatteringTex, m_v->scatteringState,
                  RHI::ResourceState::SHADER_RESOURCE, cl);
}

bool VolumetricFogPass::DispatchTemporal(RHI::CommandList cl, const RHI::PipelineState& pso)
{
    if (!pso.IsValid() || !m_hist) return false;
    History& hist = *m_hist;
    if (!hist.tex[0].IsValid() || !hist.tex[1].IsValid()) return false;
    IGraphicsDevice& gfx = *m_gfx;

    const uint32_t r = hist.readIdx;
    const uint32_t w = hist.writeIdx;

    // Read buffer -> SRV (compute), write buffer -> UAV.
    TransitionTex(gfx, hist.tex[r], hist.state[r],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, hist.tex[w], hist.state[w],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    // t0 = the UN-integrated lighting volume (sigma_s x L_in, sigma_t): integrating first
    // and reprojecting the result is wrong by construction (FroxelTemporal.cs.hlsl's
    // header).
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_v->lightingTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1, gfx.GetTextureSRVGpuHandle(hist.tex[r]), cl);
    gfx.SetComputeDescriptorTable(kUAV0,  gfx.GetTextureUAVGpuHandle(hist.tex[w]), cl);
    gfx.DispatchCompute((kFroxelW + 7) / 8, (kFroxelH + 7) / 8, kFroxelD, cl);

    // Hand the just-written history off to the scatter compute shader.
    TransitionTex(gfx, hist.tex[w], hist.state[w],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);

    hist.valid = true;
    return true;
}

void VolumetricFogPass::DispatchRaymarch(RHI::CommandList cl)
{
    if (!m_rmPSO.IsValid() || !m_v->rmCurrentTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_v->rmCurrentTex, m_v->rmCurrentState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_rmPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    // t0 space2 = FroxelDensity 3D (sigma_s / sigma_t lookup).
    gfx.SetComputeDescriptorTable(kSRVT0, gfx.GetTextureSRVGpuHandle(m_v->densityTex), cl);
    // t1 space2 = CSM array (sun shadowing).
    {
        const uint64_t shadowSrv = m_v->shadowSrv ? m_v->shadowSrv : m_shadowFallbackHandle;
        if (shadowSrv) gfx.SetComputeDescriptorTable(kSRVT1, shadowSrv, cl);
    }
    // t2 space2 = Lights buffer (opted-in volumetric lights).
    {
        const uint64_t lightsSrv = m_lightsSrv[gfx.GetFrameIndex()];
        if (lightsSrv)
            gfx.SetComputeDescriptorTable(C::SRV_t2, lightsSrv, cl);
    }
    // t3 space2 = Scene depth (view-Z clamp + sky detection).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(C::SRV_t3, gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // No t5 here: the cloud shadow map (god rays through cloud gaps) is opened from the
    // heap by the index in the frame's LightCB (FroxelParams.lightCbIdx).
    // t6 / t7 space2 = the SpotShadowPass atlas + per-slice VP buffer, or the 1x1
    // stand-ins when the lazily allocated atlas is not resident (as in
    // DispatchLightInject).
    {
        const uint64_t atlas = m_spotShadowAtlasSrv ? m_spotShadowAtlasSrv
                                                    : m_spotShadowAtlasFallback;
        const uint64_t vps   = m_spotShadowVPSrv    ? m_spotShadowVPSrv
                                                    : m_spotShadowVPFallback;
        if (atlas) gfx.SetComputeDescriptorTable(C::SpotShadowAtlas, atlas, cl);
        if (vps)   gfx.SetComputeDescriptorTable(C::SpotShadowVP,    vps,   cl);
    }
    // u0 space2 = half-res in-scatter output.
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_v->rmCurrentTex), cl);

    gfx.DispatchCompute((m_v->rmHalfW + 7) / 8, (m_v->rmHalfH + 7) / 8, 1, cl);

    TransitionTex(gfx, m_v->rmCurrentTex, m_v->rmCurrentState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
}

void VolumetricFogPass::DispatchRaymarchTemporal(RHI::CommandList cl)
{
    if (!m_rmTemporalPSO.IsValid() || !m_hist) return;
    History& hist = *m_hist;
    if (!hist.rmTex[0].IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    const uint32_t r = hist.rmReadIdx;
    const uint32_t w = hist.rmWriteIdx;

    TransitionTex(gfx, hist.rmTex[r], hist.rmState[r],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, hist.rmTex[w], hist.rmState[w],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_rmTemporalPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_paramCB.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_v->rmCurrentTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1,
        gfx.GetTextureSRVGpuHandle(hist.rmTex[r]), cl);
    // Scene depth (t3 space2): the temporal CS reprojects via the surface anchor
    // reconstructed from scene depth, as the raymarch does.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(C::SRV_t3, gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(hist.rmTex[w]), cl);

    gfx.DispatchCompute((m_v->rmHalfW + 7) / 8, (m_v->rmHalfH + 7) / 8, 1, cl);

    TransitionTex(gfx, hist.rmTex[w], hist.rmState[w],
                  RHI::ResourceState::SHADER_RESOURCE, cl);
    hist.rmValid = true;
}

// The apply CB (near / far + froxel range) as a block of this frame's constants; its
// GPU address, 0 when the frame's constants ran out.
uint64_t VolumetricFogPass::WriteApplyCB()
{
    const RHI::Constants<VolApplyConstants> cb = AllocConstants<VolApplyConstants>(*m_gfx);
    if (!cb) return 0;
    VolApplyConstants c{};
    c.nearZ        = m_v->nearPlane;
    c.farZ         = m_v->farPlane;
    c.froxelNear   = EffectiveFroxelNear();
    c.froxelFar    = EffectiveFroxelFar();
    c.froxelDepth  = kFroxelD;
    *cb.cpu = c;
    return cb.gpu;
}

void VolumetricFogPass::DrawRaymarchApply(RHI::CommandList cl)
{
    PSODesc desc;
    desc.vsID        = ShaderID::VolumetricApply_VS;
    desc.psID        = ShaderID::VolumetricRaymarchApply_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    auto& bs = desc.bs.renderTarget[0];
    bs.blendEnable     = true;
    bs.srcBlend        = RHI::Blend::ONE;
    bs.destBlend       = RHI::Blend::ONE;
    bs.blendOp         = RHI::BlendOp::ADD;
    bs.srcBlendAlpha  = RHI::Blend::ZERO;
    bs.destBlendAlpha = RHI::Blend::ONE;
    bs.blendOpAlpha   = RHI::BlendOp::ADD;
    bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return;

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    cl.BindCBAddress(SRel::CB_b3, m_applyVA);
    cl.BindSRVByHandle(SRel::SRV_t5, m_depth);

    // Half-res raymarch source (post-temporal once the history is primed), in the
    // graphics root signature's t6 space0 (env cube) slot, as the froxel VolumetricApply
    // PS does.
    const uint64_t srv = (EffectiveTemporal() && m_hist && m_hist->rmValid)
        ? m_gfx->GetTextureSRVGpuHandle(m_hist->rmTex[m_hist->rmWriteIdx])
        : m_gfx->GetTextureSRVGpuHandle(m_v->rmCurrentTex);
    cl.BindDescriptorTableHandle(kEnvMapRootSlot, srv);

    cl.DrawInstanced(3, 1, 0, 0);
}

void VolumetricFogPass::DrawApply(RHI::CommandList cl)
{
    PSODesc desc;
    desc.vsID        = ShaderID::VolumetricApply_VS;
    desc.psID        = ShaderID::VolumetricApply_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    auto& bs = desc.bs.renderTarget[0];
    bs.blendEnable     = true;
    bs.srcBlend        = RHI::Blend::ONE;
    bs.destBlend       = RHI::Blend::INV_SRC_ALPHA;
    bs.blendOp         = RHI::BlendOp::ADD;
    bs.srcBlendAlpha  = RHI::Blend::ZERO;
    bs.destBlendAlpha = RHI::Blend::ONE;
    bs.blendOpAlpha   = RHI::BlendOp::ADD;
    bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return;

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // The apply PS samples the 3D scattering texture at t6 (the env-cube root slot,
    // unused by this PS otherwise) and the graph-managed depth at t5; its CB is at b3
    // space0.
    cl.BindCBAddress(SRel::CB_b3, m_applyVA);
    cl.BindSRVByHandle(SRel::SRV_t5, m_depth);   // SRV slot 3 = root param 13 (t5 space0)
    cl.BindDescriptorTableHandle(kEnvMapRootSlot,
        m_gfx->GetTextureSRVGpuHandle(m_v->scatteringTex));

    cl.DrawInstanced(3, 1, 0, 0);
}

bool VolumetricFogPass::ForwardScattering(uint64_t& srv, float& froxelNear, float& froxelFar,
                                          uint32_t& depth) const
{
    // Execute's two early-outs, then what DispatchScatter needs to write the
    // volume the apply samples.
    if (!WantsFog() || !Shown() || !m_gfx) return false;
    if (!m_v->densityTex.IsValid() || !m_densityPSO.IsValid()) return false;
    if (!m_scatterPSO.IsValid() || !m_v->scatteringTex.IsValid() || !m_v->lightingTex.IsValid()) return false;
    srv        = m_gfx->GetTextureSRVGpuHandle(m_v->scatteringTex);
    froxelNear = EffectiveFroxelNear();
    froxelFar  = EffectiveFroxelFar();
    depth      = kFroxelD;
    return srv != 0;
}

RHI::CommandList VolumetricFogPass::Execute(RHI::CommandList cl)
{
    // Volume validity: EnsureFroxelResources creates the volumes the frame fog turns on
    // (before the graph runs), so this only trips for a pass driven without a Renderer
    // (tools) - degrade to "no fog", never crash.
    if (!WantsFog() || !Shown() || !m_v->densityTex.IsValid()) return cl;
    if (!m_densityPSO.IsValid()) return cl;
    if (!m_hist) return cl;   // no view bound: EnsureFroxelResources said so
    History& hist = *m_hist;

    IGraphicsDevice& gfx = *m_gfx;

    // ---- This frame's global medium: the scene component or the pass-owned values ----
    // A scene that owns its fog but has it switched off still runs for its local volumes,
    // with zero global density.
    const bool  owned         = m_scene.owned;
    const bool  temporalOn    = EffectiveTemporal();
    const float temporalAlpha = owned ? m_scene.temporalBlend : m_temporalAlpha;

    // ---- Alias-acquire the placed froxel trio (shared arena residents) ----
    // The heap bytes may last have belonged to RTR / XeGTAO / Bloom (or nobody: a null
    // `before` = any resource). Recorded every frame the fog runs, before any transition;
    // Density / LightInject / Scatter fully rewrite every in-bounds voxel before any read.
    // The state shadows stay as they are: state is tracked per resource THROUGH aliasing
    // barriers.
    if (m_placedMask && m_v == &m_mainVol)
    {
        const RHI::GPUResource* placed[] = {
            &m_v->densityTex, &m_v->lightingTex, &m_v->scatteringTex,
        };
        for (int i = 0; i < int(sizeof(placed) / sizeof(placed[0])); ++i)
            if (m_placedMask & (1u << i))
                gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr, placed[i]), cl);
    }

    // Teleport guard: a camera that moved more than kTeleportThreshold metres since last
    // frame (scene switch, debug-cam jump, cinematic cut) makes the reprojected history
    // wrong, and blending it would ghost. Then, as on a content cut or with no valid
    // history, take only the current frame (alpha 1).
    const DirectX::XMVECTOR prevP = DirectX::XMLoadFloat3(&hist.prevCameraPos);
    const DirectX::XMVECTOR currP = DirectX::XMLoadFloat3(&m_v->cameraPos);
    const float camDelta = DirectX::XMVectorGetX(
        DirectX::XMVector3Length(DirectX::XMVectorSubtract(currP, prevP)));
    constexpr float kTeleportThreshold = 5.0f;

    // ---- The ray-traced lighting this frame (SetRt) ----
    // The two lightings reproject and accumulate differently: a switch between them restarts the
    // history, and last frame's reservoirs count only for the view they lit.
    const bool rt = RtRuns();
    const bool forwardOnly = m_rtTier == RtTier::ForwardOnly;
    const bool rtHistory = rt && hist.rtLast;
    const bool lightingSwitched = rt ? !rtHistory : hist.rtLast;
    // The sun's shafts traced per pixel (pass.volumetricFog.rtShafts) take it from the froxels and the
    // raymarch; else the raymarched shafts keep the sun and the shadow-casting spots in the hybrid tier.
    const bool shafts = WantsGodRays() && m_rmPSO.IsValid() && m_v->rmCurrentTex.IsValid();
    const bool rtShafts = rt && !forwardOnly && Settings::Pass::volumetricFogRtShafts() && m_rtShaftsPSO.IsValid() &&
                          m_v->rmCurrentTex.IsValid();
    const bool rtSun = !rtShafts && !(m_rtTier == RtTier::Hybrid && shafts);
    // The medium the raymarched sun crosses on its way to each step (0: none, it arrives whole).
    const uint32_t rmMediumIdx = Settings::Pass::volumetricFogRaymarchSunTransmittance() ? m_rtIn.mediumIdx : 0u;

    const float effectiveAlpha =
        (hist.valid && !(m_view && (m_view->FrameBreaks() & HistoryReset::Cuts)) && camDelta < kTeleportThreshold &&
         !lightingSwitched)
            ? temporalAlpha : 1.0f;
    // The indirect light's source (ResolveIndirect), and whether last frame's grid serves as its
    // history: written for this view, no cut or teleport since.
    bool indirectSharc = false;
    const uint32_t indirect = rt ? ResolveIndirect(indirectSharc) : kIndirectConstant;
    const bool indirectHistory = rtHistory && m_hist->rtIndirectRan && camDelta < kTeleportThreshold &&
                                 !(m_view && (m_view->FrameBreaks() & HistoryReset::Cuts));

    // Upload FroxelConstants.
    if (auto* paramCBSlot = m_paramCB.Current(gfx))
    {
        FroxelConstants c{};
        std::memcpy(c.viewProj,     &m_v->viewProj,     sizeof(c.viewProj));
        std::memcpy(c.invViewProj,  &m_v->invViewProj,  sizeof(c.invViewProj));
        std::memcpy(c.prevViewProj, &hist.prevViewProj, sizeof(c.prevViewProj));
        c.cameraPos[0] = m_v->cameraPos.x; c.cameraPos[1] = m_v->cameraPos.y; c.cameraPos[2] = m_v->cameraPos.z;
        c.nearPlane    = m_v->nearPlane;
        c.farPlane     = m_v->farPlane;
        c.froxelNear   = EffectiveFroxelNear();
        c.froxelFar    = EffectiveFroxelFar();
        c.temporalAlpha= effectiveAlpha;
        c.frameIndex   = m_v->frameIndex;
        c.froxelW      = kFroxelW;
        c.froxelH      = kFroxelH;
        c.froxelD      = kFroxelD;
        if (owned)
        {
            // HDRP medium: sigma_t = density, sigma_s = sigma_t * albedo ->
            // scattering 1, absorption 0, and the albedo carries the tint.
            c.fogDensity        = m_scene.enabled ? m_scene.density : 0.0f;
            c.fogScattering     = 1.0f;
            c.fogAbsorption     = 0.0f;
            c.anisotropy        = m_scene.anisotropy;
            c.heightFogStart    = m_scene.baseHeight;
            c.heightFogFalloff  = m_scene.heightFalloff;
            c.ambientContribution = m_scene.ambientIntensity;
            c.fogAlbedo[0]   = m_scene.albedo.x;
            c.fogAlbedo[1]   = m_scene.albedo.y;
            c.fogAlbedo[2]   = m_scene.albedo.z;
            c.fogEmissive[0] = m_scene.enabled ? m_scene.emissive.x : 0.0f;
            c.fogEmissive[1] = m_scene.enabled ? m_scene.emissive.y : 0.0f;
            c.fogEmissive[2] = m_scene.enabled ? m_scene.emissive.z : 0.0f;
        }
        else
        {
            // Pass-owned: a disabled fog that only runs for local volumes adds no global medium.
            c.fogDensity        = m_enabled ? m_fogDensity : 0.0f;
            c.fogScattering     = m_fogScattering;
            c.fogAbsorption     = m_fogAbsorption;
            c.anisotropy        = m_anisotropy;
            c.heightFogStart    = m_heightStart;
            c.heightFogFalloff  = m_heightFalloff;
            c.ambientContribution = m_ambientContribution;
            c.fogAlbedo[0] = c.fogAlbedo[1] = c.fogAlbedo[2] = 1.0f;
            // fogEmissive stays zero-initialised.
        }
        c.localVolumeCount   = m_localCount;
        c.localEmissiveCount = m_localEmissiveCount;
        c.sunDir[0] = m_sunDir.x;   c.sunDir[1] = m_sunDir.y;   c.sunDir[2] = m_sunDir.z;
        c.sunStrength = m_sunStrength;
        c.sunColor[0] = m_sunColor.x; c.sunColor[1] = m_sunColor.y; c.sunColor[2] = m_sunColor.z;
        const DirectX::XMFLOAT3& amb = owned ? m_scene.ambientColor : m_ambientColor;
        c.ambientColor[0] = amb.x;
        c.ambientColor[1] = amb.y;
        c.ambientColor[2] = amb.z;

        // CSM (packed into the same CB).
        std::memcpy(c.shadowMatrix0, &m_v->shadowMatrix[0], sizeof(c.shadowMatrix0));
        std::memcpy(c.shadowMatrix1, &m_v->shadowMatrix[1], sizeof(c.shadowMatrix1));
        std::memcpy(c.shadowMatrix2, &m_v->shadowMatrix[2], sizeof(c.shadowMatrix2));
        c.cascadeSplits[0] = m_v->cascadeSplits.x;
        c.cascadeSplits[1] = m_v->cascadeSplits.y;
        c.cascadeSplits[2] = m_v->cascadeSplits.z;
        c.cascadeSplits[3] = 0.0f;
        c.shadowParams[0]  = m_v->shadowTexelSize;
        // .y is repurposed as the spot/point light count for the inject loop
        // (fits as a float since only used as integer cast in shader).
        c.shadowParams[1]  = static_cast<float>(m_spotLightCount);
        c.shadowParams[2]  = m_v->shadowBias;
        c.shadowParams[3]  = m_v->shadowStrength;
        // Camera forward: the third row of viewProj's rotation block is not exactly the
        // world-space forward once composed with the projection, so the Renderer pushes the
        // unit forward (SetCameraForward; default (0, 0, 1)).
        c.cameraForward[0] = m_v->cameraForward.x;
        c.cameraForward[1] = m_v->cameraForward.y;
        c.cameraForward[2] = m_v->cameraForward.z;

        // Voxel occupancy grid. With no grid bound this frame (SceneVoxelPass off) the extent
        // is 0 and the shader treats every voxel-raymarch sample as out of bounds -> visible.
        c.voxelGridMin[0]    = m_v->voxelGridMin.x;
        c.voxelGridMin[1]    = m_v->voxelGridMin.y;
        c.voxelGridMin[2]    = m_v->voxelGridMin.z;
        c.voxelGridExtent[0] = m_v->voxelGridMax.x - m_v->voxelGridMin.x;
        c.voxelGridExtent[1] = m_v->voxelGridMax.y - m_v->voxelGridMin.y;
        c.voxelGridExtent[2] = m_v->voxelGridMax.z - m_v->voxelGridMin.z;
        c.voxelGridDim       = m_v->voxelGridDim;

        // The frame's LightCB, where the raymarch reads the cloud shadow, and the medium its sun crosses.
        c.lightCbIdx         = m_v->lightCbIdx;
        c.rmMediumIdx        = rmMediumIdx;

        // Last frame's camera: Temporal reprojects by the distance to it.
        c.prevCameraPos[0] = hist.prevCameraPos.x;
        c.prevCameraPos[1] = hist.prevCameraPos.y;
        c.prevCameraPos[2] = hist.prevCameraPos.z;

        // The ray-traced lighting's block (the FROXEL_RT builds).
        if (rt)
        {
            auto heapIndex = [&](uint64_t handle) -> uint32_t {
                if (!handle) return 0u;
                const uint32_t i = gfx.DescriptorHeapIndexFromGpuHandle(handle);
                return i == ~0u ? 0u : i;
            };
            // Beside the shafts the raymarch keeps its lights (the volumetric list's shadow-casting
            // ones): the list and its count (rtFlags >> 8) let the injection skip them.
            c.rtFlags = (rtSun ? kRtSun : 0u) | (rtSun || rtShafts ? 0u : kRtSkipShadow) | (rtHistory ? kRtHistory : 0u) |
                        (m_rtIn.glass ? kRtGlass : 0u) | (indirectHistory ? kRtIndirectHistory : 0u) |
                        (std::min(m_spotLightCount, kMaxVolLights) << 8);
            c.volLightsIdx = heapIndex(m_lightsSrv[gfx.GetFrameIndex()]);
            c.mediumIdx           = m_rtIn.mediumIdx;
            c.lightsIdx           = m_rtIn.lightsIdx;
            c.lightCount          = m_rtIn.lightCount;
            c.lightGridIdx        = m_rtIn.lightGridIdx;
            c.lightIndexIdx       = m_rtIn.lightIndexIdx;
            c.lightMapIdx         = m_rtIn.lightMapIdx;
            c.lightMediumScaleIdx = m_rtIn.lightMediumScaleIdx;
            c.pathMaterialsIdx    = m_rtIn.pathMaterialsIdx;
            c.densityIdx          = heapIndex(gfx.GetTextureSRVGpuHandle(m_v->densityTex));
            c.reservoirIdx        = heapIndex(gfx.GetBufferUAVGpuHandle(m_hist->rtReservoir[m_hist->rtReservoirSlot]));
            c.prevReservoirIdx    = heapIndex(gfx.GetBufferUAVGpuHandle(m_hist->rtReservoir[m_hist->rtReservoirSlot ^ 1u]));
            c.candidates          = uint32_t(std::clamp(Settings::Pass::volumetricFogRtCandidates(), 1, 32));
            c.alphaTestEnabled    = m_rtIn.alphaTestEnabled;
            c.alphaInstanceCount  = m_rtIn.alphaInstanceCount;
            c.rtTemporalAlpha     = std::clamp(Settings::Pass::volumetricFogRtTemporalAlpha(), 0.01f, 1.0f);
            c.sunMediumScale      = m_rtIn.sunMediumScale;
            c.clusterNear         = m_rtIn.clusterNear;
            c.clusterFar          = m_rtIn.clusterFar;
            c.prevLightCount      = m_rtIn.prevLightCount;
            // The indirect light: this frame's grid and the last, the sky the rays miss into, the cache.
            c.indirectMode        = indirect;
            if (indirect != kIndirectConstant)
            {
                c.indirectIdx     = heapIndex(gfx.GetTextureUAVGpuHandle(m_hist->rtIndirect[m_hist->rtIndirectSlot]));
                c.prevIndirectIdx = heapIndex(gfx.GetTextureUAVGpuHandle(m_hist->rtIndirect[m_hist->rtIndirectSlot ^ 1u]));
                // The rays' grid smoothed (Filter); DDGI's is smooth already.
                c.indirectReadIdx = indirect == kIndirectRays && m_rtIndirectFilterPSO.IsValid()
                                  ? heapIndex(gfx.GetTextureUAVGpuHandle(m_hist->rtIndirectSmoothed)) : c.indirectIdx;
            }
            c.skyIdx              = m_rtIn.skyIdx;
            c.environmentScale    = m_rtIn.environmentScale;
            c.indirectAlpha       = 1.0f / kIndirectSamples;
            // The traced shafts' output and the depth they march to.
            if (rtShafts)
            {
                c.shaftsIdx = heapIndex(gfx.GetTextureUAVGpuHandle(m_v->rmCurrentTex));
                if (const RHI::Texture* depth = cl.GetContext().GetTexture(m_depth))
                    c.sceneDepthIdx = heapIndex(gfx.GetTextureSRVGpuHandle(*depth));
            }
            if (indirectSharc)
            {
                const SharcCache::Lookup& sharc = m_rtIn.sharc;
                c.sharcCapacity        = sharc.indices.capacity;
                c.sharcHashUav         = sharc.indices.hashUav;
                c.sharcAccumulationUav = sharc.indices.accumulationUav;
                c.sharcResolvedUav     = sharc.indices.resolvedUav;
                c.sharcSceneScale      = sharc.sceneScale;
                c.sharcRadianceScale   = sharc.radianceScale;
            }
        }

        *paramCBSlot = c;
    }

    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(cl, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(cl, r);
    };

    {
        uint32_t r = pBegin("VolFog.Density"); DispatchDensity(cl, rt ? m_rtDensityPSO : m_densityPSO); pEnd(r);
    }
    if (rt)
    {
        if (indirect != kIndirectConstant)
        {
            uint32_t r = pBegin("VolFog.IndirectRT");
            DispatchIndirectRT(cl, indirectSharc ? m_rtIndirectSharcPSO : m_rtIndirectPSO, indirect == kIndirectRays);
            pEnd(r);
        }
        uint32_t r = pBegin("VolFog.LightInjectRT"); DispatchLightInjectRT(cl); pEnd(r);
    }
    else
    {
        uint32_t r = pBegin("VolFog.LightInject"); DispatchLightInject(cl); pEnd(r);
    }

    // ---- Temporal reprojection (optional) ----
    // Blends the just-injected (sigma_s x L_in, sigma_t) grid against the history with a
    // neighbourhood clamp, BEFORE Scatter: only the un-integrated volume is a local voxel
    // property; the integral depends on the whole camera -> voxel path and cannot be
    // reprojected. Scatter integrates whichever volume came out.
    uint64_t scatterSrc = gfx.GetTextureSRVGpuHandle(m_v->lightingTex);
    bool temporalRan = false;
    if (temporalOn)
    {
        uint32_t r = pBegin("VolFog.Temporal");
        temporalRan = DispatchTemporal(cl, rt ? m_rtTemporalPSO : m_temporalPSO);
        pEnd(r);
        if (temporalRan)
            scatterSrc = gfx.GetTextureSRVGpuHandle(hist.tex[hist.writeIdx]);
    }

    {
        uint32_t r = pBegin("VolFog.Scatter"); DispatchScatter(cl, scatterSrc); pEnd(r);
    }

    // ---- Per-pixel raymarch tier (sun + shadow-casting spots) ----
    // Runs AFTER the froxel density is ready (sampled as sigma_t). ONE predicate for the
    // dispatch, the composite below and the history swap: a composite without the PSO term
    // would sample a texture nothing wrote this frame after a failed VolumetricRaymarch.cs
    // compile (the PSO is built outside m_psoCache and stays invalid until ReloadShaders).
    // Not under the ray-traced lighting's full tier (its froxels hold the sun and every light),
    // never when the path tracer owns the camera's fog, and not where the traced shafts run: they
    // write its texture, and its temporal pass and composite take them.
    const bool rmRan = shafts && !forwardOnly && !(rt && rtSun) && !rtShafts;
    // One line on every change while the raymarch runs: whether its sun crosses the medium.
    if (rmRan && (1u | (rmMediumIdx ? 2u : 0u)) != m_rmLogKey)
    {
        m_rmLogKey = 1u | (rmMediumIdx ? 2u : 0u);
        LOG_INFO("[VOLFOG_RM] sunMedium=%u", rmMediumIdx ? 1u : 0u);
    }
    bool rmTemporalRan = false;
    if (rmRan || rtShafts)
    {
        uint32_t r = pBegin(rmRan ? "VolFog.Raymarch" : "VolFog.ShaftsRT");
        if (rmRan) DispatchRaymarch(cl);
        else       DispatchShaftsRT(cl);
        pEnd(r);

        if (temporalOn && m_rmTemporalPSO.IsValid() && hist.rmTex[0].IsValid())
        {
            uint32_t rt = pBegin("VolFog.Raymarch.Temporal");
            DispatchRaymarchTemporal(cl);
            pEnd(rt);
            rmTemporalRan = true;
        }
        else
        {
            // No temporal: promote the current texture to pixel SR for the apply PS.
            TransitionTex(gfx, m_v->rmCurrentTex, m_v->rmCurrentState,
                          RHI::ResourceState::SHADER_RESOURCE, cl);
        }
    }

    // ---- Apply (graphics): composite onto HDR ----
    // Not while the path tracer owns the camera's fog: the volume is lit for the surfaces drawn
    // after it alone (fog_apply.hlsli through ForwardScattering).
    const RHI::Texture* depthTex = forwardOnly ? nullptr : cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));

    // Both apply draws bind this frame's apply CB, written once before them. No block
    // (the frame's constants ran out, reported) - no composite this frame.
    m_applyVA = forwardOnly ? 0 : WriteApplyCB();

    if (m_applyVA)
    {
        uint32_t r = pBegin("VolFog.Apply");
        DrawApply(cl);
        pEnd(r);
    }

    // Then the shafts, additive: the raymarch attenuated each step's light from the camera
    // already, so they go on after the froxel apply's transmittance, not under it.
    if ((rmRan || rtShafts) && m_applyVA)
    {
        uint32_t r = pBegin("VolFog.Raymarch.Apply");
        DrawRaymarchApply(cl);
        pEnd(r);
    }

    // Swap history ping-pong for next frame.
    if (temporalOn)
    {
        hist.readIdx  = hist.writeIdx;
        hist.writeIdx = 1u - hist.writeIdx;
        // Swap the raymarch ping-pong ONLY when the raymarch temporal pass ran: it has three
        // more gates than temporalOn, and hist.rmValid is sticky, so a swap on a skipped frame
        // would make the next DrawRaymarchApply sample a two-frame-old slot as last frame's.
        if (rmTemporalRan)
        {
            hist.rmReadIdx  = hist.rmWriteIdx;
            hist.rmWriteIdx = 1u - hist.rmWriteIdx;
        }
    }

    // Next frame's reprojection and teleport guard (History::prevViewProj); the volume the path
    // tracer's segments read until the next Execute (LastLitVolume).
    hist.prevViewProj  = m_v->viewProj;
    hist.prevCameraPos = m_v->cameraPos;
    hist.lit           = temporalRan;

    // The ray-traced lighting: this frame's reservoirs are next frame's history, for this view.
    hist.rtLast = rt;
    if (rt) hist.rtReservoirSlot ^= 1u;
    hist.rtIndirectRan = rt && indirect != kIndirectConstant;
    if (hist.rtIndirectRan) hist.rtIndirectSlot ^= 1u;
    if (!m_view || m_view->Slot() == 0)   // the [VOLFOG_RT] line is the main view's
    {
        // Silent while the tier is off (0): the raster lighting's runs log as before.
        const uint32_t key = m_rtTier == RtTier::Off ? 0u
            : 1u | (uint32_t(m_rtTier) << 1) | (rt ? 8u : 0u) | (rtSun ? 16u : 0u) | (rmRan ? 32u : 0u) |
              (m_rtIn.glass ? 64u : 0u) | (indirect << 7) | (indirectSharc ? 512u : 0u) | (rtShafts ? 1024u : 0u);
        if (key != m_rtLogKey)
        {
            m_rtLogKey = key;
            static const char* const kTiers[] = { "off", "hybrid", "full", "forwardOnly" };
            // indirect: constant, ddgi, rays (the sky and the emitters), sharc (the rays with the cache).
            const char* indirectName = indirect == kIndirectDdgi ? "ddgi"
                                     : indirect == kIndirectRays ? (indirectSharc ? "sharc" : "rays") : "constant";
            // shafts: 0 none, 1 raymarched, 2 traced per pixel. E: the sun the froxels or the traced
            // shafts take (its colour x its fog scale).
            LOG_INFO("[VOLFOG_RT] tier=%s traced=%u sun=%u shafts=%u composite=%u glass=%u lights=%u indirect=%s "
                     "E=%.6f_%.6f_%.6f",
                     kTiers[uint32_t(m_rtTier)], rt ? 1u : 0u, rt && rtSun ? 1u : 0u, rmRan ? 1u : rtShafts ? 2u : 0u,
                     forwardOnly ? 0u : 1u, m_rtIn.glass ? 1u : 0u, m_rtIn.lightCount, indirectName,
                     m_sunColor.x * m_rtIn.sunMediumScale, m_sunColor.y * m_rtIn.sunMediumScale,
                     m_sunColor.z * m_rtIn.sunMediumScale);
        }
    }

    return cl;
}
