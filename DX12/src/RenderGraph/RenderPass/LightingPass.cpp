#include "Graphics/SpotShadowView.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ReflectionProbeTypes.h" // GpuShared::ReflectionProbe stride (the null probe SRV)
#include "RenderGraph/PassTextureNames.h"
#include <cstring>
#include "System/Log.h"
#include "System/Check.h"

// ==== Root parameter slots (RootSignatureLayout.h is the single source of truth) ====
// S:: = absolute root-parameter index (BindDescriptorTableHandle); SR:: = group-relative index
// (BindCB / BindSRVByHandle). Keep in sync with Lighting.ps.hlsl.
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

// Stencil refs: one engine-wide definition in Graphics/ShadingStencil.h.
static constexpr uint8_t kStencilPBR   = ShadingStencil::kPBR;
static constexpr uint8_t kStencilNPR   = ShadingStencil::kNPR;
static constexpr uint8_t kStencilUnlit = ShadingStencil::kUnlit;

// ==== Construction / RG wiring ====

LightingPass::LightingPass(RG::RGTextureHandle albedo,
                           RG::RGTextureHandle normal,
                           RG::RGTextureHandle surface,
                           RG::RGTextureHandle depth,
                           RG::RGTextureHandle emissive,
                           RG::RGTextureHandle reflectionSource)
    : m_albedo(albedo), m_normal(normal), m_surface(surface), m_depth(depth),
      m_emissive(emissive), m_reflectionSource(reflectionSource)
{}

void LightingPass::BindView(ViewState& view)
{
    m_in = view.Slot() == 0 ? &m_mainIn : &view.Data<ViewInputsData>(this).in;
}

// See the header note on what RenderGraph keys on GetName().
void LightingPass::SetNameSuffix(const char* suffix)
{
    // An empty suffix would silently reproduce the main instance's name (first-match-wins timestamp
    // scan, one shared external-wait key, one merged PIX region): leave the name alone and report.
    if (!ENGINE_CHECK(suffix && *suffix,
                      "LightingPass::SetNameSuffix: empty suffix would duplicate '%s'",
                      kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

void LightingPass::Setup(RG::RenderGraphBuilder& b)
{
    // The three results are ON-DEMAND graph textures (Renderer::Compile): a
    // valid handle can still have no physical texture behind it, which the
    // graph's barrier emitter skips. Nothing samples one then - the shader
    // reads them only through LightCB indices the Renderer sets for an
    // ACTIVE producer, and a producer is never active with its result unbacked.
    if (m_ptResult.IsValid()) b.ReadSRV(m_ptResult);
    if (m_diResult.IsValid()) b.ReadSRV(m_diResult);
    if (m_giResult.IsValid()) b.ReadSRV(m_giResult);
    b.ReadSRV(m_albedo);
    b.ReadSRV(m_normal);
    b.ReadSRV(m_surface);
    b.ReadSRV(m_depth);
    // The DSV is bound for the stencil test (EQUAL, by shading model) with the depth test off: the
    // stencil plane read (the shading code's bits, RenderGraph/StencilBits.h), the depth plane
    // unused by the DSV (sampled above).
    if (RG::RenderGraph::PlanesEnabled())
        b.DepthStencil(m_depth, RG::Depth::None, RG::Stencil::Read, ShadingStencil::kCodeMask);
    b.ReadSRV(m_emissive);
    b.WriteRenderTarget(m_reflectionSource);
    // Preserve = bind HDR without clearing: GBufferPass already wrote the emissive seed and this
    // pass adds on top. The seed's copy into the reflection source (CopyHdrSceneToScoped) is a scoped
    // read at RENDER_TARGET.
    b.ReadScoped(b.HdrScene(), RHI::ResourceState::RENDERTARGET);
    b.SetColorTarget(RG::BuiltinTexture::HdrSceneColorPreserve);
    // The pass textures Lighting.ps samples through the setters and the LightCB indices, declared so
    // the graph's DAG has the edges (their producers barrier them: Owner): the shadow maps, the RT
    // sun mask, the AO (RTAO's or XeGTAO's; one of them runs), the RT reflections, SSGI and the
    // sky's specular cube. The cloud shadow is last frame's: CloudPass runs after this pass.
    b.ReadPassTextures({ RG::PassTex::kShadowCSM, RG::PassTex::kShadowSpotAtlas,
                         RG::PassTex::kShadowSpotAtlasMedium, RG::PassTex::kShadowSpotAtlasHigh,
                         RG::PassTex::kShadowPointCube, RG::PassTex::kShadowPerObject, RG::PassTex::kShadowHair,
                         RG::PassTex::kRTShadowMask, RG::PassTex::kRTAO, RG::PassTex::kXeGTAO,
                         RG::PassTex::kRTReflection, RG::PassTex::kSSGIResult, RG::PassTex::kSkySpecularCube });
    b.ReadPassTextureHistory({ RG::PassTex::kCloudShadow });
}

// ==== PSO build / shader hot reload ====

PSODesc LightingPass::BuildPSODesc(Variant variant) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Lighting_VS;
    desc.psID        = ShaderID::Lighting_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = true;

    // MRT layout (SetRenderTargetsAndHdr appends HDR last): RT0 = reflection-free opaque scene
    // colour, RT1 = final HDR scene colour. Both are pre-seeded with GBufferPass's material emissive
    // (its direct write to RT5), and this pass ADDS diffuse + specular + ambient on top, so
    // SceneColor = lighting + emissive without emissive passing through the BRDF. Each variant
    // (PBR / NPR / Unlit) returns only its lit contribution.
    desc.bs.renderTarget[0].blendEnable        = true;
    desc.bs.renderTarget[0].srcBlend           = RHI::Blend::ONE;
    desc.bs.renderTarget[0].destBlend          = RHI::Blend::ONE;
    desc.bs.renderTarget[0].blendOp            = RHI::BlendOp::ADD;
    desc.bs.renderTarget[0].srcBlendAlpha     = RHI::Blend::ONE;
    desc.bs.renderTarget[0].destBlendAlpha    = RHI::Blend::ONE;
    desc.bs.renderTarget[0].blendOpAlpha      = RHI::BlendOp::ADD;
    desc.bs.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvFormats[1] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 2;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    // Fullscreen triangle: depth off; stencil EQUAL on the shading code's bits only, so a TaaExclude /
    // OutlineRim bit set earlier does not fail the test.
    desc.dss.depthEnable       = false;
    desc.dss.depthWriteMask   = RHI::DepthWriteMask::ZERO;
    desc.dss.stencilEnable     = true;
    desc.dss.stencilReadMask  = ShadingStencil::kCodeMask;
    desc.dss.stencilWriteMask = 0x00;
    desc.dss.frontFace.stencilFunc    = RHI::ComparisonFunc::EQUAL;
    desc.dss.frontFace.stencilPassOp = RHI::StencilOp::KEEP;
    desc.dss.backFace = desc.dss.frontFace;

    if(variant==Variant::CameraPrimary)
    {
        // Complete camera radiance includes emission and sky; replace the
        // GBuffer seed and draw every pixel, including raster background.
        desc.bs.renderTarget[0].blendEnable=false;
        desc.bs.renderTarget[1].blendEnable=false;
        desc.dss.stencilEnable=false;
    }
    switch (variant)
    {
    case Variant::NPR:   desc.perm.Set(PermutationKey::NPR_STENCIL, true); break;
    case Variant::Unlit: desc.perm.Set(PermutationKey::UNLIT,       true); break;
    case Variant::PBR: default: break;
    }
    return desc;
}

void LightingPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::PBR)))
        LOG_ERROR("LightingPass: PBR PSO rebuild after shader reload failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::NPR)))
        LOG_ERROR("LightingPass: NPR PSO rebuild after shader reload failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::Unlit)))
        LOG_ERROR("LightingPass: Unlit PSO rebuild after shader reload failed");
}

// ==== Lifetime: Shutdown and Init (shaders, PSOs, fallback resources) ====

// Frees nothing; see the declaration.
LightingPass::~LightingPass() = default;

// Gives back everything the instance owns (see the declaration): each preview GraphSet build
// makes a new instance, so anything left here would leak once per build.
void LightingPass::Shutdown(IGraphicsDevice& gfx)
{
    // A contract tripwire, not a guard: the hand-backs below are safe either way, but a caller that
    // tears a pass down while the frames it recorded are in flight has a real bug elsewhere.
    // ENGINE_CHECK logs and the teardown proceeds.
    //
    // GetRetiredFrame() = max(m_renderFrame - kFramesInFlight, the hard-sync floor) and
    // m_lastRecordStamp was m_renderFrame + 1 at the last record, so a set torn down N frames after
    // its last record passes when N >= kFramesInFlight + 1 (Preview::kMinIdleTeardownFrames, the
    // floor -preview_idle_frames is clamped to). A pass that never recorded keeps stamp 0 and never
    // trips this.
    const bool quiesced = (m_lastRecordStamp == 0) ||
                          (gfx.GetRetiredFrame() >= m_lastRecordStamp);
    if (m_lastRecordStamp != 0)
        ENGINE_CHECK(quiesced,
            "LightingPass::Shutdown at a boundary that is NOT quiesced: the last "
            "record was stamped frame %llu and the device has only retired %llu. "
            "Shutdown's contract is a quiesced boundary; the descriptor block and "
            "the fallback textures go through the deferred bucket so this is not "
            "itself a use-after-free, but the caller is destroying a pass whose "
            "command lists may still be in flight.",
            static_cast<unsigned long long>(m_lastRecordStamp),
            static_cast<unsigned long long>(gfx.GetRetiredFrame()));
    m_lastRecordStamp = 0;

    m_psoCache.Shutdown();
    m_shaderLib.Shutdown(gfx);

    auto tex = [&](RHI::Texture& t, uint64_t& h) {
        if (t.IsValid()) gfx.DestroyTexture(t);
        h = 0;
    };
    tex(m_shadowFallbackTex,           m_shadowFallbackHandle);
    tex(m_ssaoFallbackTex,             m_ssaoFallbackHandle);
    tex(m_spotShadowAtlasFallbackTex,  m_spotShadowAtlasFallback);
    tex(m_pointShadowAtlasFallbackTex, m_pointShadowAtlasFallback);
    tex(m_perObjShadowFallbackTex,     m_perObjShadowFallback);
    tex(m_rtReflectionFallbackTex,     m_rtReflectionFallback);
    tex(m_rtShadowMaskFallbackTex,     m_rtShadowMaskFallback);

    if (m_spotShadowVPFallbackBuf.IsValid()) gfx.DestroyBuffer(m_spotShadowVPFallbackBuf);
    m_spotShadowVPFallback = 0;
    if (m_skySHFallback.IsValid()) gfx.DestroyBuffer(m_skySHFallback);
    m_skySHFallbackHandle = 0;

    if (m_probeNullBlock.IsValid())
    {
        // Quiesced: free now. GetRetiredFrame() is >= the drain's argument (BeginFrame drains up to
        // m_renderFrame - kFrameCount; a WaitForPreviousFrame hard sync only raises it), so an immediate
        // free is at least as safe as a deferred one. It also puts the three slots back before a
        // rebuild's AllocateStatic(3) asks, so the rebuild reuses the same block; deferring would let a
        // 1- or 2-slot request split it meanwhile (+3 static high-water per preview open/close; the
        // preview_teardown pack gates liveCbvDelta / worldCbvDelta at 0).
        //
        // Not quiesced: the deferred bucket, so the slots cannot go to anyone else while a command list
        // may still name them. ReleaseDescriptorBlock takes the block by value and resets the handle in
        // both cases.
        gfx.ReleaseDescriptorBlock(m_probeNullBlock, quiesced);
    }
    m_reflProbeGridNullSrv   = 0;
    m_reflProbeIndexNullSrv  = 0;
    m_reflProbeBufferNullSrv = 0;
}

void LightingPass::Init(IGraphicsDevice& gfx)
{
    // Fully-lit CSM stand-in (R32_FLOAT, arraySize = kCascadeCount), built like TransparentPass /
    // ParticleRenderPass / FaceOverlayPass / WaterSurfacePass do. Lighting.ps samples
    // gShadowCascades unconditionally, but the shadow array can be absent (ShadowPass::Init returns
    // early on allocation failure; the array is lazy and gated), and an unbound table reads whatever
    // descriptor last occupied the slot: an undefined read, not a black shadow.
    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R32_FLOAT;
        desc.width      = 1;
        desc.height     = 1;
        desc.arraySize = 4;   // Texture2DArray SRV needs > 1
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        // arraySize 4 and texel 0.0 are both load-bearing. The shader declares Texture2DArray, and
        // CreateTexture emits a TEXTURE2DARRAY SRV only when arraySize > 1 (a TEXTURE2D SRV in an array
        // slot is a Tier-3 type mismatch with no validation message). Under the reversed-Z GREATER_EQUAL
        // comparison sampler "fully lit" is depth 0.0 (the shadow passes' clear value, "no occluder");
        // a 1.0 texel reads as an occluder at the near plane, i.e. fully shadowed.
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // One entry per subresource: CreateTexture uploads mipLevels x arraySize subresources from this
        // array; a single-element init with arraySize > 1 reads off the stack.
        RHI::SubresourceData init[4];
        for (auto& sd : init) sd = { &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(desc, m_shadowFallbackTex, init))
            m_shadowFallbackHandle = gfx.GetTextureSRVGpuHandle(m_shadowFallbackTex);
        else
            LOG_ERROR("LightingPass: CSM fallback texture failed");
    }

    // No samplers to create or bind: Lighting.ps names fixed sampler-heap slots -
    // SMP_LINEAR_MIP_POINT_CLAMP for the GBuffer / NPR ramp reads (linear == point at texel centres
    // for 1:1 reads), SMP_LINEAR_WRAP for the IBL cubes, SMP_CMP_LINEAR_BORDER_WHITE_GE for the
    // shadow maps' PCF (reversed-Z GREATER_EQUAL, border = lit).

    // ---- Shaders + PSOs ----
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Lighting_VS, RHI::ShaderStage::VS, "Lighting.vs.hlsl");
    m_shaderLib.Register(ShaderID::Lighting_PS, RHI::ShaderStage::PS, "Lighting.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::PBR)))
        LOG_ERROR("LightingPass: PBR PSO creation failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::NPR)))
        LOG_ERROR("LightingPass: NPR PSO creation failed");
    if (!m_psoCache.GetOrCreate(BuildPSODesc(Variant::Unlit)))
        LOG_ERROR("LightingPass: Unlit PSO creation failed");
    else
        LOG_INFO("LightingPass: PBR + NPR + Unlit PSOs ready");

    // ---- Fallback resources (what a slot or LightCB handle names when its input is absent) ----
    // SSAO 1x1 white -> reads as 1.0 = no occlusion.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint8_t white = 0xFF;
        RHI::SubresourceData init{ &white, 1, 1 };
        if (gfx.CreateTexture(td, m_ssaoFallbackTex, &init))
            m_ssaoFallbackHandle = gfx.GetTextureSRVGpuHandle(m_ssaoFallbackTex);
    }
    // Sky SH 9 x float4 zeros (the shader gates reads on iblUseSH).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 9 * 4 * sizeof(float);
        bd.stride     = 4 * sizeof(float);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        float zeros[9 * 4] = {};
        if (gfx.CreateBuffer(bd, m_skySHFallback, zeros))
            m_skySHFallbackHandle = gfx.GetBufferSRVGpuHandle(m_skySHFallback);
        else
            LOG_ERROR("LightingPass: SH fallback buffer creation failed");
    }
    // Spot-shadow atlas 1x1 stand-in, with the CSM fallback's two rules: the shader declares
    // Texture2DArray and indexes slices up to kMaxCasters-1, so arraySize must be > 1 for a
    // TEXTURE2DARRAY SRV; and fully lit is 0.0, not 1.0, under the reversed-Z comparison sampler.
    // Content is normally moot (the shader samples only when shadowSliceIdx != 0xFFFFFFFF), but
    // normally is not never.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1; td.height = 1;
        td.arraySize = 8;   // > 1 for an ARRAY SRV
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const float one = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // One entry per subresource: CreateTexture uploads mipLevels x arraySize subresources from this
        // array; a single-element init with arraySize > 1 reads off the stack.
        RHI::SubresourceData init[8];
        for (auto& sd : init) sd = { &one, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(td, m_spotShadowAtlasFallbackTex, init))
            m_spotShadowAtlasFallback = gfx.GetTextureSRVGpuHandle(m_spotShadowAtlasFallbackTex);
    }
    // Single-identity VP buffer for the spot-shadow VP fallback.
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
    }
    // Point-shadow cube atlas fallback: a 1x1 TextureCubeArray (2 cubes -> the SRV is
    // TEXTURECUBEARRAY / R32_FLOAT, which the shader's TextureCubeArray<float> must match; under
    // Tier 3 a mismatch gives no validation message, only garbage or a TDR). Content is moot:
    // Lighting.ps samples it only when a point light owns a cube (shadowSliceIdx != 0xFFFFFFFF).
    // GetPointShadowAtlasSrv() hands it to LightCB.pointShadowIdx on frames without a live atlas.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1; td.height = 1;
        td.arraySize = 12;  // 2 cubes: CreateTexture needs arraySize > 6 && % 6 == 0
        td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateTexture(td, m_pointShadowAtlasFallbackTex))
            m_pointShadowAtlasFallback = gfx.GetTextureSRVGpuHandle(m_pointShadowAtlasFallbackTex);
        else
            LOG_ERROR("LightingPass: point-shadow cube fallback creation failed");
    }
    // Per-object (player) + hair shadow 1x1 fallback (Texture2D<float>), shared by both maps:
    // GetPerObjectShadowSrv() / GetHairShadowSrv() hand it back on frames without a live map, so
    // LightCB.perObjShadowIdx / hairShadowIdx always name a real R32_FLOAT Texture2D. Content is
    // normally moot (the shader gates on perObjShadowParams.x / hairShadowParams.x), but if a live
    // map fails to allocate while the enable stays set this texel is compared: under the reversed-Z
    // comparison sampler 0.0 = fully lit, while 1.0 would shadow everything.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1; td.height = 1;
        td.arraySize = 1;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        RHI::SubresourceData init{ &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(td, m_perObjShadowFallbackTex, &init))
            m_perObjShadowFallback = gfx.GetTextureSRVGpuHandle(m_perObjShadowFallbackTex);
        else
            LOG_ERROR("LightingPass: per-object shadow fallback creation failed");
    }
    // RT sun-shadow mask 1x1 WHITE (visibility: 1.0 = fully lit). Not the per-object fallback, whose
    // 0.0 means lit only under the comparison sampler and reads fully shadowed under a plain Load.
    // The real gate is LightCB.rtShadowEnabled (the Renderer sets it only with a live full-res mask);
    // GetRTShadowMaskSrv() hands this one to LightCB.rtShadowMaskIdx on frames RTShadowPass is off.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R8_UNORM;
        td.width      = 1; td.height = 1;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint8_t lit = 0xFF;   // 1.0 = fully lit (min() no-op)
        RHI::SubresourceData init{ &lit, 1, 1 };
        if (gfx.CreateTexture(td, m_rtShadowMaskFallbackTex, &init))
            m_rtShadowMaskFallback = gfx.GetTextureSRVGpuHandle(m_rtShadowMaskFallbackTex);
        else
            LOG_ERROR("LightingPass: RT shadow-mask fallback creation failed");
    }
    // RT reflection 1x1 black (rtConf == 0): when read, the 3-tier composite collapses to the no-RTR
    // math. LightCB.rtReflectionIdx points here whenever RTR produced nothing, and unlike the shadow
    // mask this read is ungated, so the texture must be real black, not merely valid.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.width      = 1; td.height = 1;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint16_t black[4] = { 0, 0, 0, 0 };   // fp16 zeros
        RHI::SubresourceData init{ black, 8, 8 };
        if (gfx.CreateTexture(td, m_rtReflectionFallbackTex, &init))
            m_rtReflectionFallback = gfx.GetTextureSRVGpuHandle(m_rtReflectionFallbackTex);
        else
            LOG_ERROR("LightingPass: RT reflection fallback creation failed");
    }
    // ---- Null SRVs for the probe grid / index lists and the probe buffer ----
    // Three adjacent static heap slots holding structured-buffer nulls whose strides match the HLSL
    // element types exactly:
    //     slot 0 -> stride 8   == sizeof(ProbeGridEntry) { uint offset; uint count; }
    //     slot 1 -> stride 4   == sizeof(uint)
    //     slot 2 -> stride 192 == sizeof(GpuShared::ReflectionProbe)
    // The first two match ClusterPass::Init's m_probeGridBuffer / m_probeIndexBuffer strides. A
    // descriptor of another type behind `StructuredBuffer<T> b = ResourceDescriptorHeap[i]` is
    // undefined behaviour, and Resource Binding Tier 3 reports nothing. The Renderer uses slot 2 only
    // when ReflectionProbeManager's per-frame ring SRV is absent.
    // IGraphicsDevice::CreateNullStructuredSrvBlock writes straight into the shader-visible heap, so
    // no CopyToGpu() step is needed; the block is kept in m_probeNullBlock.
    {
        constexpr uint32_t kProbeGridEntryStride = 8;   // { uint offset; uint count; }
        constexpr uint32_t kProbeIndexStride     = 4;   // uint
        // Taken from the C++ mirror so it can never drift from the struct the
        // HLSL StructuredBuffer element type describes (both are 192 B; the
        // type header static_asserts its own size).
        constexpr uint32_t kProbeStride =
            static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe));

        // Re-Init on an instance that holds a block would strand the old three, so the previous block
        // goes back first, the same way as in Shutdown. Init runs from RenderGraph::Compile, which
        // Renderer::ServicePreviewCompile pins to the game half's serial window with the render half
        // parked, so the stamp test normally passes and the free is immediate; the deferred bucket covers
        // a rebuild that did not wait out the idle grace.
        if (m_probeNullBlock.IsValid())
            gfx.ReleaseDescriptorBlock(m_probeNullBlock,
                                       m_lastRecordStamp == 0 ||
                                       gfx.GetRetiredFrame() >= m_lastRecordStamp);
        const uint32_t strides[3] = { kProbeGridEntryStride, kProbeIndexStride, kProbeStride };
        if (gfx.CreateNullStructuredSrvBlock(strides, 3, m_probeNullBlock))
        {
            m_reflProbeGridNullSrv    = m_probeNullBlock.Gpu(0);
            m_reflProbeIndexNullSrv   = m_probeNullBlock.Gpu(1);
            m_reflProbeBufferNullSrv  = m_probeNullBlock.Gpu(2);
        }
        else
        {
            // The Renderer's BindlessOf checks each handle's slot (kind and stride) and hands LightCB the
            // structured null instead, so the gated reads see an empty buffer rather than remove the device;
            // it should never happen.
            LOG_ERROR("LightingPass: probe null SRV allocation failed "
                      "(ex-t24/t25/t26 bindless fallbacks unavailable)");
        }
    }

    // No AP fallback: SkyIBLPass creates the real 3D LUT and shader gates on aerialMaxDistKm > 0.
}

// ==== Execute: bind the GBuffer SRVs + per-frame handles, draw the stencil-gated variants ====

RHI::CommandList LightingPass::Execute(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = cl.GetDevice();

    // The stamp that makes Shutdown's contract checkable: the device's deferred-bucket counter at
    // the point this pass records. One plain store per record; nothing reads it on this half.
    m_lastRecordStamp = gfx.GetReleaseStampFrame();

    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    const RHI::Texture* reflectionSourceTex =
        cl.GetContext().GetTexture(m_reflectionSource);

    // GBufferPass has cleared HDR and seeded it with material emissive, and no lighting or
    // reflection pass has touched it yet. Copy that seed to RT0, then add reflection-free lighting to
    // RT0 and full lighting to HDR/RT1 in the same fullscreen draws. The PSO has a fixed two-RT
    // contract (with only HDR bound SV_Target0 would land in the wrong resource), so a missing graph
    // texture is a hard failure, not a one-RT fallback.
    if (!reflectionSourceTex || !reflectionSourceTex->IsValid() || !depthTex)
        return cl;

    // CopyHdrSceneToScoped, not SetHdrTextureState(COPY_SRC) + CopyHdrSceneTo +
    // SetHdrTextureState(RENDERTARGET): this body records on a Phase-B ParallelFor worker
    // concurrently with every other pass. The SetHdrTextureState pair mutates the device's single
    // shared HDR state tracker; a pass calling SetRenderTargetsAndHdr / SetRenderTargetToHdrWithDepth
    // inside that window would emit a COPY_SOURCE -> RENDER_TARGET barrier against a resource still
    // in RENDER_TARGET, and this pass's restore would become a no-op, leaving HDR stuck in
    // COPY_SOURCE. The scoped form keeps the round trip inside this command list and never touches
    // the shared tracker.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        reflectionSourceTex,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::COPY_DST), cl);
    gfx.CopyHdrSceneToScoped(*reflectionSourceTex, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        reflectionSourceTex,
        RHI::ResourceState::COPY_DST,
        RHI::ResourceState::RENDERTARGET), cl);

    const RHI::Texture* rtvs[] = { reflectionSourceTex };
    gfx.SetRenderTargetsAndHdr(1, rtvs, depthTex, cl, cl.GetContext().DepthView(m_depth));   // the stencil-test DSV view the declaration set

    auto bindResources = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindCB(SR::CB_b1, RG::RGSlot::LightCB);
        cl.BindSRVByHandle(SR::SRV_t2, m_albedo);
        cl.BindSRVByHandle(SR::SRV_t3, m_normal);
        cl.BindSRVByHandle(SR::SRV_t4, m_surface);
        cl.BindSRVByHandle(SR::SRV_t5, m_depth);

        // ---- IBL + shadow CSM ----
        if (m_iblIrradianceHandle) cl.BindDescriptorTableHandle(S::IBLIrradiance, m_iblIrradianceHandle);
        if (m_iblRadianceHandle)   cl.BindDescriptorTableHandle(S::IBLRadiance,   m_iblRadianceHandle);
        if (m_brdfLutHandle)       cl.BindDescriptorTableHandle(S::BRDFLUT,       m_brdfLutHandle);
        {   // never leave t9 unbound (see the CSM fallback in Init)
            const uint64_t shadowSrv = m_shadowSrvHandle ? m_shadowSrvHandle
                                                         : m_shadowFallbackHandle;
            if (shadowSrv) cl.BindDescriptorTableHandle(S::ShadowSRV, shadowSrv);
        }

        // ---- Clustered lighting ----
        if (m_clusterLightsSRV)    cl.BindDescriptorTableHandle(S::ClusterLights,    m_clusterLightsSRV);
        if (m_clusterIndexListSRV) cl.BindDescriptorTableHandle(S::ClusterIndexList, m_clusterIndexListSRV);
        if (m_clusterGridSRV)      cl.BindDescriptorTableHandle(S::ClusterGrid,      m_clusterGridSRV);

        // ---- Per-material buffer ----
        if (m_materialBufHandle)    cl.BindDescriptorTableHandle(S::MaterialBuf, m_materialBufHandle);

        // ---- Bindless texture table (S::BindlessTex) ----
        // The NPR variant samples the per-material toon ramp (g_AllTextures) by
        // its bindless id. Mirrors GBufferPass::BindGlobals.
        if (const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle())
            cl.BindDescriptorTableHandle(S::BindlessTex, texTable);

        // ---- Emissive GBuffer (t17) ----
        if (const RHI::Texture* emTex = cl.GetContext().GetTexture(m_emissive); emTex && emTex->IsValid())
        {
            if (uint64_t emGpu = cl.GetDevice().GetTextureSRVGpuHandle(*emTex))
                cl.BindDescriptorTableHandle(S::EmissiveSRV, emGpu);
        }

        // ---- SSAO (white fallback when disabled -> ssao = 1.0) ----
        if (m_in->ssao)        cl.BindDescriptorTableHandle(S::SSAOSRV, m_in->ssao);
        else if (m_ssaoFallbackHandle) cl.BindDescriptorTableHandle(S::SSAOSRV, m_ssaoFallbackHandle);

        // ---- Sky SH (always bind something; LightCB.iblUseSH gates the read) ----
        if (m_skySHValid && m_skySHHandle) cl.BindDescriptorTableHandle(S::SkySH, m_skySHHandle);
        else if (m_skySHFallbackHandle)    cl.BindDescriptorTableHandle(S::SkySH, m_skySHFallbackHandle);

        // ---- Aerial perspective ----
        // No fallback: Lighting.ps gates the read on LightCB.aerialMaxDistKm > 0, so binding nothing on
        // a frame without a LUT is defined.
        if (const uint64_t ap = m_in->aerial ? m_in->aerial : m_apHandle)
            cl.BindDescriptorTableHandle(S::AerialPersp, ap);

        // ---- Spot-shadow atlas + per-slice VPs ----
        cl.BindDescriptorTableHandle(S::SpotShadowAtlas,
            m_spotShadowAtlasHandle ? m_spotShadowAtlasHandle : m_spotShadowAtlasFallback);
        cl.BindDescriptorTableHandle(S::SpotShadowVP,
            m_spotShadowVPHandle    ? m_spotShadowVPHandle    : m_spotShadowVPFallback);

        // ---- Not bound here: read through LightCB handles (see RootSignatureLayout.h) ----
        // The graphics root signature is at the D3D12 64-DWORD limit, so these are opened from the
        // descriptor heap:
        // - point / per-object / hair shadows, RT reflection, RT sun mask: typed handles the Renderer
        //   resolves (BindlessOf) once per frame, right before graph.Execute, from the Get*Srv()
        //   accessors, which pick the Init fallbacks on frames without a live resource;
        // - probe grid / index lists: GetReflProbeGridSrv() / GetReflProbeIndexSrv()
        //   (type-correct nulls when absent);
        // - probe buffer: ReflectionProbeManager::GetBufferSrv(gfx) each frame (a 3-slot UPLOAD
        //   ring), GetReflProbeBufferNullSrv() when the manager has none;
        // - probe cubes: each probe's own cubemapSrvIndex (reflection_probe.hlsli);
        // - DDGI: the volume buffer's handle straight off DDGIVolumeManager (the current frame's
        //   ring slot), whose Tick stamps each volume's resource base indices into its
        //   VolumeGPUDesc; `ddgiEnabled != 0 && ddgiVolumeCount > 0` gates every read;
        // - SSR result: Lighting.ps does not read it; Water.ps opens it from
        //   LightCB.ssrResultIdx (WaterPass owns the handle and its fallback).
    };

    auto draw = [&](Variant variant, uint8_t stencilRef)
    {
        const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc(variant));
        if (!pso || !pso->IsValid()) return;
        cl.SetPipelineState(*pso);
        bindResources();
        gfx.SetStencilRef(stencilRef, cl);
        cl.DrawFullscreenTriangle();
    };

    if(m_in->cameraPrimary)
    {
        draw(Variant::CameraPrimary,0);
        return cl;
    }
    draw(Variant::PBR, kStencilPBR);
    // Unconditional, like the other two: the toon model reads each material's own ramp from the
    // bindless table.
    draw(Variant::NPR, kStencilNPR);
    draw(Variant::Unlit, kStencilUnlit);

    return cl;
}
