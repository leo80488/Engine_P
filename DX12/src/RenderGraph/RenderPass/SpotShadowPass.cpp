#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/ShadowCasterShaders.h"   // ShadowCaster:: hook shaders

#include "ECS/HierarchyComponents.h"    // ViewBit (shadow viewMask)
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "Settings/PassSettings.h"     // pass.spotShadow.*
#include "System/Check.h"
#include "System/Log.h"
#include "System/CommandLine.h"

#include <DirectXMath.h>
#include <cstring>

using namespace DirectX;

// Root parameter slots: aliases of the shared graphics root signature
// (Graphics/RootSignatureLayout.h).
static constexpr auto kShadowPerViewSlot = RHI::Slots::GfxRel::CB_b1;       // CB b1 space0 -> BindCB index 0
static constexpr auto kInstanceBufSlot   = RHI::Slots::Gfx::InstanceBuf;     // root SRV t0 space0: InstanceBuffer
static constexpr auto kMeshDescSlot      = RHI::Slots::Gfx::MeshDesc;        // root SRV t1 space0
static constexpr auto kBindlessSlot      = RHI::Slots::Gfx::BindlessBuffers; // desc table bindless g_Buffers[]
static constexpr auto kBindlessTexSlot   = RHI::Slots::Gfx::BindlessTex;     // t0 space2 - g_AllTextures[]
static constexpr auto kMaterialBufSlot   = RHI::Slots::GfxRel::SRV_t2;       // t2 space0 (SRV-relative index)

// The retained per-slice args use the shared CSM bucket order.
static_assert(SpotShadowPass::kShadowGroupCount == ShadowDrawGroup::RetainedCount,
              "SpotShadowPass retained-arg buckets must mirror ShadowDrawGroup");

SpotShadowPass::SpotShadowPass()
{
    for (uint32_t i = 0; i < kUpdateBudget; ++i)
        XMStoreFloat4x4(&m_pendingVP[i], XMMatrixIdentity());
}

SpotShadowPass::~SpotShadowPass()
{
    if (!m_gfxPtr) return;
    m_casterCBs.Destroy(*m_gfxPtr);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_indirectArgMapped[i])    m_gfxPtr->UnmapBuffer(m_indirectArgBuffer[i]);
        if (m_indirectArgBuffer[i].IsValid()) m_gfxPtr->DestroyBuffer(m_indirectArgBuffer[i]);
        m_indirectArgMapped[i] = nullptr;
    }
    ReleaseAtlas();
}

// The tiers as imports of this pass. The atlas is made at the frame boundary
// (EnsureAtlas via OnService) and the graph reads the member at each Execute, so a
// later allocation needs nothing here. The graph transitions them
// (ImportTracking::Graph): DEPTHSTENCIL for this pass, DEPTH_READ_SRV for each reader.
void SpotShadowPass::DeclareImports(RG::ImportDeclarer& d)
{
    auto tracked = [](const RHI::Texture& t)
    { return RG::ImportSpec{ &t, RG::ImportEntry::Stale, RG::ImportTracking::Graph }; };
    m_atlasH[0] = d.Import(RG::PassTex::kShadowSpotAtlas,
        { RHI::Format::D32_FLOAT, kShadowMapSize, kShadowMapSize, true, false, false, kMaxCasters }, tracked(m_atlas));
    m_atlasH[1] = d.Import(RG::PassTex::kShadowSpotAtlasMedium,
        { RHI::Format::D32_FLOAT, 1024, 1024, true, false, false, kMediumCount }, tracked(m_mediumAtlas));
    m_atlasH[2] = d.Import(RG::PassTex::kShadowSpotAtlasHigh,
        { RHI::Format::D32_FLOAT, 2048, 2048, true, false, false, kHighCount }, tracked(m_highAtlas));
}

void SpotShadowPass::Setup(RG::RenderGraphBuilder& b)
{
    // What this pass draws into: the graph brings the tiers to DEPTHSTENCIL.
    for (RG::RGTextureHandle h : m_atlasH) b.WriteDepthStencil(h);
}

void SpotShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // Shadow.ps and SpotShadow.vs (Shadow.vs plus the static / dynamic instance filter):
    // depth-only, one ShadowPerViewCB at b1 space0.
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "SpotShadow.vs.hlsl");
    // A masked custom material's shadow VS when it has no custom VS
    // (ShadowCasterShaders.h); the material's PS resolves through the one
    // DynamicShaderRegistry, as in every pass library.
    m_shaderLib.Register(ShaderID::ShadowHook_VS, RHI::ShaderStage::VS, "ShadowHook.vs.hlsl");
    m_shaderLib.Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm)))
        LOG_ERROR("SpotShadowPass: base PSO creation failed");

    // The shadow atlas is not created here: its three tiers (512^2 x 256 + 1024^2 x 16 +
    // 2048^2 x 4, D32) are allocated on demand by EnsureAtlas, so a world with no
    // shadow-casting spot light pays nothing. Everything below is small and eager.

    // Caster CB pool (one per frame in flight, 256 B-aligned per slot).
    if (!m_casterCBs.Create(gfx, "SpotShadowPass.CasterCBs"))
        LOG_ERROR("SpotShadowPass: caster CB pool creation failed");

    // Indirect draw command buffers (same layout as ShadowPass / GBuffer), one per
    // frame in flight so ExecuteIndirect cannot race the next frame's CPU writes.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxIndirectCommands) * sizeof(IndirectDrawCommand);
        bd.stride     = sizeof(IndirectDrawCommand);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_indirectArgBuffer[i]))
                m_indirectArgMapped[i] = gfx.MapBuffer(m_indirectArgBuffer[i]);
        }
    }

    // The run-time sampler for custom casters (SetDynamicSampler): linear wrap, what their
    // material shaders read as g_LinearWrap here. Shadow.ps's own alpha test names the
    // fixed SMP_LINEAR_WRAP slot.
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU = RHI::TextureAddressMode::WRAP;
        sd.addressV = RHI::TextureAddressMode::WRAP;
        sd.addressW = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("SpotShadowPass: alpha-test sampler creation failed");
    }

    LOG_SUCCESS("SpotShadowPass: initialised (Atlas %ux%u x %u slices DEFERRED, indirect=%s)",
                kShadowMapSize, kShadowMapSize, kMaxCasters,
                m_indirectArgMapped[0] ? "YES" : "NO");
}

// Allocation only; the header has the residency contract. At most one frame of lag, by
// design: `wanted` comes from Renderer::ExtractShadowAtlasDemand, a LightData scan at
// the top of App::RenderPhase (after the frame's simulation, before the editor's
// actions and UI and the scene build), so a spot light that gains castsShadow or
// spawns after that scan allocates on the next frame, and the frame in between renders
// with no atlas: no shadow, never a wrong one (without an atlas the Renderer leaves
// every shadowSliceIdx at 0xFFFFFFFF, and Lighting.ps / the fog shaders skip the
// sample). A world load follows the same rule: its OnWorldClear released the atlas,
// and the first scan that sees the new world's casters allocates it again.
// Release is per world, never a "no casters for N frames" timer: a light toggled off
// and on, or a caster leaving the light set for a moment, would churn 256 MiB, and a
// frame-counted timer depends on the display refresh rate.
void SpotShadowPass::EnsureAtlas(bool wanted)
{
    if (!m_gfxPtr || !wanted || m_atlas.IsValid() || m_allocFailed) return;

    RHI::TextureDesc td;
    td.width      = kShadowMapSize;
    td.height     = kShadowMapSize;
    td.format     = RHI::Format::D32_FLOAT;
    td.bindFlags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.mipLevels = 1;
    td.arraySize = kMaxCasters;
    td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0); // reversed Z
    td.debugName = "SpotShadowPass.Atlas";
    // _ONCE throttles only the log; m_allocFailed stops the retry. The check stays in
    // this function (not a shared helper) so its static latch belongs to the spot atlas
    // alone; PointShadowPass has its own.
    if (!ENGINE_CHECK_ONCE(m_gfxPtr->CreateTexture(td, m_atlas),
                           "SpotShadowPass: shadow atlas creation failed "
                           "(%ux%u x %u D32, 256 MiB) — spot shadows disabled "
                           "for this world",
                           kShadowMapSize, kShadowMapSize, kMaxCasters))
    {
        m_atlas      = {};
        m_allocFailed = true;
        return;
    }

    // Bounded high-quality working set: 64 MiB medium + 64 MiB high. If either fails,
    // the base atlas still serves every light.
    td.width = td.height = 1024; td.arraySize = kMediumCount;
    td.debugName = "SpotShadowPass.Medium";
    if (!m_gfxPtr->CreateTexture(td, m_mediumAtlas))
        LOG_WARNING("SpotShadowPass: medium tier unavailable; using base maps");
    td.width = td.height = 2048; td.arraySize = kHighCount;
    td.debugName = "SpotShadowPass.High";
    if (!m_gfxPtr->CreateTexture(td, m_highAtlas))
        LOG_WARNING("SpotShadowPass: high tier unavailable; using lower tiers");

    // Optional static backing depth (up to 384 MiB), no SRVs: receivers read the
    // sampled atlases. A failed allocation keeps the full-render path.
    // pass.spotShadow.noMixedCache is read here, at allocation: a change lands with the
    // next world's atlas.
    if (!Settings::Pass::spotShadowNoMixedCache()) {
        auto createStatic = [&](const RHI::Texture& source, RHI::Texture& target, const char* name) {
            if (!source.IsValid()) return true;
            auto desc = source.desc;
            desc.bindFlags = RHI::BindFlag::DEPTH_STENCIL;
            desc.debugName = name;
            return m_gfxPtr->CreateTexture(desc, target);
        };
        m_mixedAvailable = createStatic(m_atlas, m_staticAtlas, "SpotShadowPass.StaticBase") &&
            createStatic(m_mediumAtlas, m_staticMediumAtlas, "SpotShadowPass.StaticMedium") &&
            createStatic(m_highAtlas, m_staticHighAtlas, "SpotShadowPass.StaticHigh");
        if (!m_mixedAvailable) {
            for (auto* atlas : { &m_staticAtlas, &m_staticMediumAtlas, &m_staticHighAtlas }) {
                if (atlas->IsValid()) m_gfxPtr->DestroyTexture(*atlas);
                *atlas = {};
            }
            LOG_WARNING("SpotShadowPass: static cache allocation failed; full redraw fallback");
        }
    }

    // CPU-side state mirror: CreateTexture leaves a DEPTH_STENCIL texture in
    // DEPTH_WRITE, the before-state Execute's m_firstExecution branch assumes. A stale
    // false would make the first barrier after a re-create claim DEPTH_READ_SRV.
    m_firstExecution = true;
    // No caster is assigned against this atlas yet. The Renderer refills both every
    // frame before Execute, but last world's values would let a torn frame draw with them.
    m_activeCount = 0;
    m_ra          = {};

    LOG_INFO("SpotShadowPass: atlas allocated (%.1f MiB, %u shadow-casting spot "
             "light(s) in world)",
             static_cast<double>(static_cast<uint64_t>(kShadowMapSize) * kShadowMapSize
                                 * kMaxCasters * 4ull) / (1024.0 * 1024.0),
             kMaxCasters);
}

// Tier moves occur only when this light is redrawn. Until then consumers keep
// sampling its previous valid map; no shadow disappears while a promotion waits.
void SpotShadowPass::AssignTier(uint32_t slice)
{
    const uint32_t wanted = m_desiredTier[slice];
    if (m_sliceTier[slice] == wanted) return;
    m_sliceTier[slice] = 0;
    const uint32_t capacity = wanted == 2 ? kHighCount : wanted == 1 ? kMediumCount : 0;
    for (uint32_t slot = 0; slot < capacity; ++slot)
    {
        bool used = false;
        for (uint32_t other = 0; other < kMaxCasters; ++other)
            if (m_sliceTier[other] == wanted && m_tierSlot[other] == slot) { used = true; break; }
        if (!used) { m_sliceTier[slice] = wanted; m_tierSlot[slice] = slot; break; }
    }
}

SpotShadowViewGPU SpotShadowPass::GetView(uint32_t slice) const
{
    SpotShadowViewGPU result;
    DirectX::XMStoreFloat4x4(&result.vp, DirectX::XMMatrixTranspose(
        DirectX::XMLoadFloat4x4(&m_cachedVP[slice])));
    const auto& atlas = AtlasFor(slice);
    result.atlasIndex = m_gfxPtr->DescriptorHeapIndexFromGpuHandle(m_gfxPtr->GetTextureSRVGpuHandle(atlas));
    result.slice = PhysicalSlice(slice);
    result.resolution = atlas.desc.width;
    return result;
}

// Called on real scene changes (OnWorldClear; the editor's ExitPlay restores the
// same world and must not drop and re-allocate) and from the destructor.
// DestroyTexture defers the resource and its views kFrameCount frames, so an
// in-flight LightingPass still sampling the atlas finishes safely.
void SpotShadowPass::ReleaseAtlas()
{
    // Cleared unconditionally: a new world gets one fresh allocation attempt even if
    // the previous one failed and left nothing to destroy.
    m_allocFailed = false;

    if (!m_gfxPtr || !m_atlas.IsValid()) return;
    for (auto* atlas : { &m_staticAtlas, &m_staticMediumAtlas, &m_staticHighAtlas }) {
        if (atlas->IsValid()) m_gfxPtr->DestroyTexture(*atlas);
        *atlas = {};
    }
    m_mixedAvailable = false;
    std::fill(std::begin(m_staticValid), std::end(m_staticValid), false);
    if (m_mediumAtlas.IsValid()) m_gfxPtr->DestroyTexture(m_mediumAtlas);
    if (m_highAtlas.IsValid()) m_gfxPtr->DestroyTexture(m_highAtlas);
    m_mediumAtlas = {}; m_highAtlas = {};
    std::fill(std::begin(m_sliceTier), std::end(m_sliceTier), 0u);
    std::fill(std::begin(m_desiredTier), std::end(m_desiredTier), 0u);
    m_gfxPtr->DestroyTexture(m_atlas);   // deferred kFrameCount frames
    m_atlas          = {};
    m_firstExecution = true;
    m_activeCount    = 0;
    std::fill(std::begin(m_cachedValid), std::end(m_cachedValid), false);
    m_ra             = {};
    LOG_INFO("SpotShadowPass: atlas released (%.1f MiB reclaimed, world cleared)",
             static_cast<double>(static_cast<uint64_t>(kShadowMapSize) * kShadowMapSize
                                 * kMaxCasters * 4ull) / (1024.0 * 1024.0));
}

void SpotShadowPass::WarmPipelines(const DrawPacket& dp)
{
    if (!dp.castShadow || (dp.viewMask & ViewBit::ShadowAny) == 0) return;
    const bool alphaTest = dp.permutation.Has(PermutationKey::ALPHA_TEST);
    if (dp.filter == DrawFilter::Transparent && !alphaTest) return;
    PermutationKey perm;
    if (alphaTest) perm.Set(PermutationKey::ALPHA_TEST, true);
    if (ShadowDrawsPerDraw(dp))
    {
        // The per-draw loop: a custom VS, or a masked custom material's SHADOW_HOOK PS,
        // with the record path's fallback.
        const uint32_t hookPS = ShadowCaster::HookPS(dp);
        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(perm, dp.customVSID, false, hookPS));
        if ((!pso || !pso->IsValid()) && hookPS != 0)
            (void)m_psoCache.GetOrCreate(BuildPSODesc(perm, dp.customVSID));
        return;
    }
    if (ClassifyShadowDrawGroup(dp, ViewBit::ShadowAny) == ShadowDrawGroup::Skip) return;
    (void)m_psoCache.GetOrCreate(BuildPSODesc(perm, 0, !alphaTest && !ShadowDrawNeedsPS(dp)));
}

PSODesc SpotShadowPass::BuildPSODesc(PermutationKey perm, uint32_t customVSID,
                                     bool depthOnly, uint32_t hookPSID) const
{
    PSODesc desc;
    if (hookPSID != 0)
    {
        // A masked custom material: its own PS in the SHADOW_HOOK variant decides the
        // coverage (ShadowCasterShaders.h).
        ShadowCaster::SelectHookShaders(desc, perm, customVSID, hookPSID);
    }
    else if (customVSID != 0)
    {
        desc.vsID = static_cast<ShaderID>(customVSID);
        perm.Set(PermutationKey::CUSTOM_VS_SHADOW, true);  // depth-only WPO variant
    }
    else
    {
        desc.vsID = ShaderID::Shadow_VS;
    }
    // No-PS bucket: Shadow.ps's only work for a discard-free draw is the
    // unconditional dissolveParams fetch + dead [branch], and binding a
    // may-discard PS costs the pass its early-Z depth WRITE. Drop the stage
    // when the whole group is provably discard-free.
    if (hookPSID == 0)
        desc.psID    = depthOnly ? PSODesc::kNoPixelShader : ShaderID::Shadow_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

	// Cull NONE: double-sided casters keep thin objects from leaking light. Back-face
	// culling would suit mostly closed meshes but adds acne and still leaks.
	desc.rs.cullMode = RHI::CullMode::NONE;
    // Depth clip on. Without it a triangle with two vertices behind the light (w < 0)
    // is lost at rasterization: a floor tile right under a ceiling-height spot vanished
    // from the map and the spot lit the floor below through the slab. Clipping to
    // 0 <= z <= w cuts such a triangle at the near cap (0.1 m from the light) instead;
    // the pancaking given up only kept casters inside that 0.1 m (a lamp's own housing,
    // which should not shadow its light). The perspective families need this; CSM and
    // the per-object map are orthographic (w == 1) and keep pancaking.
    desc.rs.depthClipEnable = true;

    // Reversed-Z D32_FLOAT: a negative caster bias moves stored depth away from the
    // light. Under a vertical spot the floor's depth slope is zero and the receiver
    // normal offset vanishes at NdotL == 1, and one ULP does not cover camera-depth
    // position reconstruction plus light-space projection (a static floor self-shadows
    // differently as the camera moves): reserve 32 depth ULPs. This is exponent-scaled
    // D32 bias, not a fixed normalized-depth offset (which would leak badly under
    // perspective). Keep the slope term for grazing surfaces; do not stack a fixed
    // receiver bias on top.
    desc.rs.depthBias              = -32;
    desc.rs.slopeScaledDepthBias = -1.0f;
    desc.rs.depthBiasClamp        = 0.0f;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvCount  = 0;
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

uint64_t SpotShadowPass::GetAtlasSrvHandle() const
{
    if (!m_gfxPtr || !m_atlas.IsValid()) return 0;
    return m_gfxPtr->GetTextureSRVGpuHandle(m_atlas);
}

void SpotShadowPass::UploadCasterCBs()
{
    if (!m_gfxPtr) return;
    auto* pool = m_casterCBs.Current(*m_gfxPtr);
    if (!pool) return;
    for (uint32_t i = 0; i < m_activeCount; ++i)
    {
        XMMATRIX m = XMLoadFloat4x4(&m_pendingVP[i]);
        XMFLOAT4X4 t;
        XMStoreFloat4x4(&t, XMMatrixTranspose(m));
        uint8_t* dst = pool->slots + i * kCasterCBStride;
        std::memcpy(dst, &t, sizeof(XMFLOAT4X4));

        // Custom-VS (WPO) time + camera at fixed offsets 192 / 208 (custom_vertex.hlsli
        // CUSTOM_VS_SHADOW ShadowPerViewCB). 256-byte stride.
        float timeParams[4] = { m_cvTime, m_cvDeltaTime,
                                std::sin(m_cvTime), std::cos(m_cvTime) };
        float cameraPos[4]  = { m_cvCameraPos.x, m_cvCameraPos.y, m_cvCameraPos.z, 0.0f };
        std::memcpy(dst + 192, timeParams, sizeof(timeParams));
        std::memcpy(dst + 208, cameraPos,  sizeof(cameraPos));
        // Dissolve noise bindless index at 224 (Shadow.ps ShadowPerViewCB) and the
        // instance-remap heap index at 228 (Shadow.vs / CUSTOM_VS_SHADOW).
        uint32_t dissolveIdx[4] = { m_dissolveNoiseIdx, m_instanceRemapIdx, 0u, 0u };
        std::memcpy(dst + 224, dissolveIdx, sizeof(dissolveIdx));

        // Banks two and three (see CasterCBPool): bank one's bytes with this slice's working
        // remap heap index at 228 (the compacted commands' instance ranges live in that
        // remap) and the dynamic / static filter at 232. Written only while retained args
        // are active, so the ring path pays nothing extra.
        if (m_ra.active && m_ra.remapHeapIdx[i] != 0)
        {
            uint8_t* dstW = pool->slots + (kUpdateBudget + i) * kCasterCBStride;
            std::memcpy(dstW, dst, kCasterCBStride);
            const uint32_t workingIdx = m_ra.remapHeapIdx[i];
            std::memcpy(dstW + 228, &workingIdx, sizeof(workingIdx));
            const uint32_t dynamicFilter = m_mixedAvailable && m_honorShadowFlags ? 2u : 0u;
            std::memcpy(dstW + 232, &dynamicFilter, sizeof(dynamicFilter));
            uint8_t* dstStatic = pool->slots + (2 * kUpdateBudget + i) * kCasterCBStride;
            std::memcpy(dstStatic, dstW, kCasterCBStride);
            const uint32_t staticFilter = 1;
            std::memcpy(dstStatic + 232, &staticFilter, sizeof(staticFilter));
        }
    }
}

RHI::CommandList SpotShadowPass::Execute(RHI::CommandList cl)
{
    ++m_telemetryFrame;
    if (!m_atlas.IsValid()) return cl;
    // With the tiers imported, the graph transitions them (DEPTHSTENCIL before this
    // pass, DEPTH_READ_SRV before each reader), so these barriers run only without an
    // import (-rgimportoff). The m_firstExecution bookkeeping stays either way.
    const bool graphTracked = m_atlasH[0].IsValid();
    auto barrierAtlases = [&](RHI::ResourceState before, RHI::ResourceState after) {
        if (graphTracked) return;
        for (auto* atlas : { &m_atlas, &m_mediumAtlas, &m_highAtlas })
            if (atlas->IsValid()) cl.PushBarrier(RHI::GPUBarrier::Image(atlas, before, after));
    };


    // No shadow-casting spot light this frame: skip the clears. The first caster-less
    // frame still owes a one-shot DEPTHSTENCIL -> SRV transition so LightingPass can
    // sample the zero-initialised atlas (reversed Z: depth 0 = no shadow); after that
    // the atlas stays in SRV until a caster reappears.
    if (m_activeCount == 0)
    {
        if (m_firstExecution)
        {
            barrierAtlases(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
            m_firstExecution = false;
        }
        return cl;
    }

    IGraphicsDevice& gfx = *m_gfxPtr;
    const bool mixed = m_mixedAvailable && m_ra.active && m_honorShadowFlags;
    // Compatibility/failure paths draw the complete scene. Never trust an old
    // static cache after one of those frames, since its revision wasn't serviced.
    if (!mixed) std::fill(std::begin(m_staticValid), std::end(m_staticValid), false);

    // On subsequent frames the atlas arrives in DEPTH_READ_SRV (Lighting.ps
    // sampled it last frame). Transition back to DEPTHSTENCIL for writes.
    if (!m_firstExecution)
    {
        barrierAtlases(RHI::ResourceState::DEPTH_READ_SRV, RHI::ResourceState::DEPTHSTENCIL);
    }

    // Preserve unselected residents; only the update views are cleared and redrawn.
    for (uint32_t s = 0; s < m_activeCount; ++s)
        if (!mixed)
            gfx.ClearDepthStencilSlice(AtlasFor(m_updateSlices[s]), PhysicalSlice(m_updateSlices[s]), 0.0f, 0, cl); // reversed Z

    // Pull the draw lists we need ONCE.
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    // Forward-translucent packets carry castShadow 0 (Renderer::ApplyBlendMode)
    // unless -shadow_translucent_casters.
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);

    if (!mixed && (m_activeCount == 0 ||
        (opaqueDraws.empty() && shadowDraws.empty() && transparentDraws.empty())))
    {
        // No renderable content: transition back and bail.
        barrierAtlases(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
        m_firstExecution = false;
        return cl;
    }

    UploadCasterCBs();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const uint32_t frameSlot      = gfx.GetFrameIndex();
    const bool useIndirect = (m_indirectArgMapped[frameSlot] != nullptr);

    // Honor CAST_SHADOW + the shadow viewMask like CSM (the Renderer's compat flag can
    // turn it off), identically in every loop below (indirect count/write, per-draw
    // fallback, custom-VS).
    const bool honorFlags = m_honorShadowFlags;
    auto skipCaster = [honorFlags](const DrawPacket& dp) -> bool {
        return honorFlags &&
               (!dp.castShadow || (dp.viewMask & ViewBit::ShadowAny) == 0);
    };

    // Per-slice GPU-culled args (see RetainedShadowArgs), gated on m_honorShadowFlags:
    // the cull CS always applies castShadow + the ShadowAny viewMask
    // (ClassifyShadowDrawGroup / requiredViewBits), so only the CPU ring path can
    // ignore them.
    const bool retainedArgs = m_ra.active && m_honorShadowFlags;

    // Ring path depth-only eligibility for the non-alpha group: when every packet it
    // draws is discard-free its PSO drops the pixel shader (ShadowDrawNeedsPS). Group 1
    // is ALPHA_TEST and always keeps Shadow_PS. Same filters as the group-0
    // count/write/fallback loops below.
    bool group0NoPS = true;
    if (!retainedArgs)
    {
        auto scanList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                if (skipCaster(dp))       continue;
                if (ShadowDrawsPerDraw(dp))   continue;   // per-draw loop, keeps its PS
                if (dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                if (ShadowDrawNeedsPS(dp)) group0NoPS = false;
            }
        };
        scanList(opaqueDraws); scanList(shadowDraws);
    }

    // Retained path: the same eligibility, per ShadowDrawGroup bucket. The scan walks
    // the full un-culled packet list while the GPU cull may drop instances and whole
    // commands; that is correct because culling only removes: "no packet in this bucket
    // can discard" over the superset holds for any subset. Never derive a permissive
    // flag from the un-culled list.
    bool groupNoPS[kShadowGroupCount];
    for (uint32_t g = 0; g < kShadowGroupCount; ++g) groupNoPS[g] = true;
    if (retainedArgs)
    {
        auto scanList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                const int g = ClassifyShadowDrawGroup(dp, ViewBit::ShadowAny);
                if (g < 0 || g >= static_cast<int>(kShadowGroupCount)) continue;
                if (ShadowDrawNeedsPS(dp)) groupNoPS[g] = false;
            }
        };
        scanList(opaqueDraws); scanList(shadowDraws); scanList(transparentDraws);
        groupNoPS[ShadowDrawGroup::AlphaTest] = false;
    }

    // Build the indirect arg buffer split alpha / non-alpha (CSM's layout). Skipped on
    // the retained path (per-slice working args replace it).
    uint32_t groupOffsets[2] = { 0, 0 };
    uint32_t groupCounts[2]  = { 0, 0 };

    if (useIndirect && !retainedArgs)
    {
        auto* args = static_cast<IndirectDrawCommand*>(m_indirectArgMapped[frameSlot]);

        // The transparent source contributes only to the alpha-test bucket: pure
        // alpha-blended packets (no alphaRef) would otherwise render solid.
        auto countAlpha = [&](DrawList list, bool wantAlpha, bool transparentSrc) -> uint32_t {
            uint32_t n = 0;
            for (const DrawPacket& dp : list)
            {
                if (skipCaster(dp)) continue;      // caster opted out
                if (ShadowDrawsPerDraw(dp)) continue;  // custom-VS casters render per-draw below
                if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                if (dp.permutation.Has(PermutationKey::ALPHA_TEST) == wantAlpha)
                    ++n;
            }
            return n;
        };
        groupCounts[0] = countAlpha(opaqueDraws, false, false) + countAlpha(shadowDraws, false, false);
        groupCounts[1] = countAlpha(opaqueDraws, true,  false) + countAlpha(shadowDraws, true,  false)
                       + countAlpha(transparentDraws, true, true);

        groupOffsets[0] = 0;
        groupOffsets[1] = groupCounts[0] * sizeof(IndirectDrawCommand);

        uint32_t writeIdx = 0;
        auto writeGroup = [&](DrawList list, bool wantAlpha, bool transparentSrc) {
            for (const DrawPacket& dp : list)
            {
                if (skipCaster(dp)) continue;      // caster opted out
                if (ShadowDrawsPerDraw(dp)) continue;  // custom-VS casters render per-draw below
                if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                bool isAlpha = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                if (isAlpha != wantAlpha) continue;
                if (writeIdx >= kMaxIndirectCommands) break;
                auto& ic = args[writeIdx++];
                ic.meshDescIdx     = dp.meshDescriptorIndex;
                ic.instanceOffset  = dp.instanceOffset;
                ic.materialIndex   = dp.materialIndex;
                ic.prevPosInfo     = 0;
                ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                ic.instanceCount          = dp.instanceCount;
                ic.startVertexLocation    = 0;
                ic.startInstanceLocation  = 0;
            }
        };
        // Group 0 is written first and contiguously from index 0, so the split
        // point is simply the cursor after its two lists.
        writeGroup(opaqueDraws,      false, false);
        writeGroup(shadowDraws,      false, false);
        const uint32_t written0 = writeIdx;
        writeGroup(opaqueDraws,      true,  false);
        writeGroup(shadowDraws,      true,  false);
        writeGroup(transparentDraws, true,  true);

        // The writeIdx cap can truncate a group mid-write, while groupCounts / groupOffsets
        // came from the uncapped countAlpha totals. They go to ExecuteIndirectDraw as
        // maxCommands and a byte offset, so stale values would point past the last written
        // command (and, if group 0 alone overran, start group 1 beyond the buffer).
        // Re-derive both from what was written; a no-op when nothing was dropped.
        const uint32_t requestedCommands = groupCounts[0] + groupCounts[1];
        groupCounts[0]  = written0;
        groupCounts[1]  = writeIdx - written0;
        groupOffsets[0] = 0;
        groupOffsets[1] = written0 * sizeof(IndirectDrawCommand);

        // _ONCE: this is per-frame, and the spot-caster set rarely shrinks on its own: an
        // unlatched line would repeat forever.
        ENGINE_CHECK_ONCE(writeIdx >= requestedCommands,
            "SpotShadowPass: indirect command budget exhausted — %u/%u "
            "commands written, %u caster draws dropped this frame (their spot "
            "shadows are simply missing, with nothing on screen naming this "
            "cap). Raise SpotShadowPass::kMaxIndirectCommands.",
            writeIdx, kMaxIndirectCommands, requestedCommands - writeIdx);
    }

    // Render each active caster into its slice.
    const RHI::GPUBuffer& casterCBBuf = m_casterCBs.CurrentBuffer(gfx);
    if (!casterCBBuf.IsValid()) {
        barrierAtlases(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
        m_firstExecution = false;
        std::fill(std::begin(m_cachedValid), std::end(m_cachedValid), false);
        std::fill(std::begin(m_staticValid), std::end(m_staticValid), false);
        return cl;
    }
    uint32_t staticRebuilds = 0, staticReused = 0;
    for (uint32_t s = 0; s < m_activeCount; ++s)
    {
        const uint32_t resolution = AtlasFor(m_updateSlices[s]).desc.width;
        cl.SetViewport(resolution, resolution);
        cl.SetScissorRect(resolution, resolution);
        cl.SetPrimitiveTopology();

        // Retained: all kShadowGroupCount buckets from this slice's culled args.
        // Ring: the non-alpha / alpha pair.
        const int groupLoopCount = retainedArgs
                                 ? static_cast<int>(kShadowGroupCount) : 2;

        auto drawGroups = [&](bool staticOnly) {
          for (int g = 0; g < groupLoopCount; ++g)
        {
            if (retainedArgs)
            {
                // Capacity 0 = the CPU classification put no command in this bucket; the GPU
                // survivor count can only be <=. A zero remap heap index means this slice got no
                // cull view: the compacted ranges resolve only through that remap, so it must not draw.
                if (m_ra.groupCap[g] == 0 || !m_ra.workingArgs[s] ||
                    !m_ra.countBuffer || m_ra.remapHeapIdx[s] == 0)
                    continue;
            }
            else if (useIndirect && groupCounts[g] == 0) continue;

            // Rasterizer cull stays NONE for every bucket (BuildPSODesc hard-codes it): spot
            // shadows are double-sided, and the ShadowDrawGroup split is reused only for its
            // arg layout, not for CSM's per-bucket cull modes. A meshlet twin
            // (g >= MeshletOffset) is its base bucket here.
            const int  baseGroup  = g % static_cast<int>(ShadowDrawGroup::Count);
            const bool alphaGroup =
                retainedArgs ? (baseGroup == ShadowDrawGroup::AlphaTest) : (g == 1);
            PermutationKey perm;
            if (alphaGroup) perm.Set(PermutationKey::ALPHA_TEST, true);

            const bool noPS = retainedArgs ? groupNoPS[baseGroup]
                                           : ((g == 0) && group0NoPS);
            const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
                BuildPSODesc(perm, 0, noPS));
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            cl.BindDescriptorHeaps();
            cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
            cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
            if (bindlessHandle)
                cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
            // Each caster has one 256-aligned slot in the pool CB: bind by byte offset.
            // Retained draws bind bank two (byte 228 = this slice's working remap), or bank
            // three for a static rebuild; everything else keeps bank one (pristine remap).
            {
                const uint64_t cbSlot = retainedArgs
                    ? static_cast<uint64_t>((staticOnly ? 2 : 1) * kUpdateBudget + s)
                    : static_cast<uint64_t>(s);
                cl.GetDevice().BindConstantBufferAtOffset(kShadowPerViewSlot,
                                                          casterCBBuf,
                                                          cbSlot * kCasterCBStride,
                                                          cl);
            }

            // Bindless g_AllTextures[] + sampler for every group: the per-instance dissolve
            // clip in Shadow.ps samples the noise texture.
            if (cl.gfx)
            {
                const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle();
                if (texTable)
                    cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
            }
            if (m_alphaSamplerIdx >= 0)
                cl.SetDynamicSampler(m_alphaSamplerIdx);   // custom casters' material sampler
            // ALPHA_TEST PS additionally reads MaterialBuffer (alphaRef).
            if (alphaGroup)
            {
                if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer))
                    if (matBuf->IsValid())
                        cl.BindBufferSRV(kMaterialBufSlot, *matBuf);
            }

            if (retainedArgs)
            {
                // Cap = the CPU bucket size (ExecuteIndirect maxCommands); the
                // real count is the GPU survivor total at this SLICE's row of
                // the shared grid. countBase already folds in the spot family
                // base, so the element read is countBase[s] + bucket.
                gfx.ExecuteIndirectDraw(
                    *m_ra.workingArgs[s],
                    uint64_t(m_ra.groupBase[g]) * sizeof(IndirectDrawCommand),
                    m_ra.groupCap[g],
                    m_ra.countBuffer,
                    uint64_t(m_ra.countBase[s] + g) * sizeof(uint32_t),
                    cl);
            }
            else if (useIndirect)
            {
                gfx.ExecuteIndirectDraw(m_indirectArgBuffer[frameSlot],
                                        groupOffsets[g],
                                        groupCounts[g],
                                        nullptr, 0, cl);
            }
            else
            {
                auto fallbackDraw = [&](DrawList list, bool transparentSrc) {
                    for (const DrawPacket& dp : list)
                    {
                        if (skipCaster(dp)) continue;      // caster opted out
                        if (ShadowDrawsPerDraw(dp)) continue;  // rendered by the custom-VS loop below
                        if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                        bool isAlpha = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                        if ((g == 0 && isAlpha) || (g == 1 && !isAlpha)) continue;
                        cl.SetPVFRootConstants(dp.meshDescriptorIndex,
                                               dp.instanceOffset,
                                               dp.materialIndex);
                        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                    }
                };
                fallbackDraw(opaqueDraws,      false);
                fallbackDraw(shadowDraws,      false);
                if (g == 1) fallbackDraw(transparentDraws, true);
            }
        }
        };

        const uint32_t slice = m_updateSlices[s], physical = PhysicalSlice(slice);
        auto& output = AtlasFor(slice);
        if (mixed) {
            auto& backing = StaticAtlasFor(slice);
            if (m_rebuildStatic[s]) {
                gfx.ClearDepthStencilSlice(backing, physical, 0.0f, 0, cl);
                gfx.SetDepthStencilSlice(backing, physical, cl);
                drawGroups(true);
                m_staticValid[slice] = true;
                m_staticRevision[slice] = m_pendingStaticRevision[s];
                ++staticRebuilds;
            } else {
                ++staticReused;
            }
            // Copy clean static depth every update: this also removes the previous pose when a
            // character leaves the light or is deleted. The round trip back to DEPTHSTENCIL is
            // scoped, so the device's state book keeps what the graph left there for the
            // readers after this pass.
            cl.PushBarrierScoped(RHI::GPUBarrier::Image(&backing, RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::COPY_SRC));
            cl.PushBarrierScoped(RHI::GPUBarrier::Image(&output, RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::COPY_DST));
            gfx.CopyTextureSubresource(backing, 0, physical, output, 0, physical, cl);
            cl.PushBarrierScoped(RHI::GPUBarrier::Image(&backing, RHI::ResourceState::COPY_SRC, RHI::ResourceState::DEPTHSTENCIL));
            cl.PushBarrierScoped(RHI::GPUBarrier::Image(&output, RHI::ResourceState::COPY_DST, RHI::ResourceState::DEPTHSTENCIL));
        }
        gfx.SetDepthStencilSlice(output, physical, cl);
        drawGroups(false);

        // Custom-VS (WPO) casters: depth-only, per draw. Excluded from the indirect groups;
        // same spot filtering (alpha split; the transparent source contributes only its
        // alpha-test cards).
        {
            const RHI::PipelineState* cvPSO = nullptr;
            bool cvAlphaGlobals = false;
            auto drawCustomVs = [&](DrawList list, bool transparentSrc) {
                for (const DrawPacket& dp : list)
                {
                    if (skipCaster(dp)) continue;      // caster opted out
                    if (!ShadowDrawsPerDraw(dp)) continue;
                    if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;

                    const bool alphaTest = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                    PermutationKey perm;
                    if (alphaTest) perm.Set(PermutationKey::ALPHA_TEST, true);

                    const uint32_t hookPS = ShadowCaster::HookPS(dp);
                    const RHI::PipelineState* pso =
                        m_psoCache.GetOrCreate(BuildPSODesc(perm, dp.customVSID, false, hookPS));
                    if ((!pso || !pso->IsValid()) && hookPS != 0)
                    {
                        // The hook does not build as a shadow: fall back to the caster's non-hook shaders
                        // (ShadowCasterShaders.h).
                        ShadowCaster::NoteHookFallback(hookPS, "SpotShadowPass");
                        pso = m_psoCache.GetOrCreate(BuildPSODesc(perm, dp.customVSID));
                    }
                    if (!pso || !pso->IsValid()) continue;

                    if (pso != cvPSO)
                    {
                        cl.SetPipelineState(*pso);
                        cl.BindDescriptorHeaps();
                        cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
                        cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
                        if (bindlessHandle)
                            cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
                        cl.GetDevice().BindConstantBufferAtOffset(
                            kShadowPerViewSlot, casterCBBuf,
                            static_cast<uint64_t>(s) * kCasterCBStride, cl);
                        // Bindless textures + sampler for the dissolve clip
                        // (needed by every custom-VS caster, not just alpha-test).
                        if (cl.gfx)
                        {
                            const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle();
                            if (texTable) cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
                        }
                        if (m_alphaSamplerIdx >= 0) cl.SetDynamicSampler(m_alphaSamplerIdx);   // custom casters' material sampler
                        cvPSO = pso;
                        cvAlphaGlobals = false;
                    }

                    if (alphaTest && !cvAlphaGlobals)
                    {
                        if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer))
                            if (matBuf->IsValid()) cl.BindBufferSRV(kMaterialBufSlot, *matBuf);
                        cvAlphaGlobals = true;
                    }

                    if (dp.customCbvVA)
                        cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl);
                    if (dp.customTexTable)
                        cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl);

                    cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
                    cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                }
            };
            drawCustomVs(opaqueDraws,      false);
            drawCustomVs(shadowDraws,      false);
            drawCustomVs(transparentDraws, true);
        }
    }

    barrierAtlases(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
    // -spot_shadow_log_every N logs [SPOTMIXED] every Nth recording frame.
    // m_telemetryFrame increments on Execute's first line, which IsActive skips on frames
    // with no spot casters; the increment deliberately stays out of PrepareFrame(): a
    // skipped frame has nothing to report, and this opt-in read is its only reader.
    static const int s_logEvery = Sys::CommandLine::Get().GetInt("-spot_shadow_log_every", 0);
    if (s_logEvery > 0 && m_telemetryFrame % s_logEvery == 0)
        LOG_INFO("[SPOTMIXED] enabled=%u updates=%u staticRebuilds=%u staticReused=%u",
            mixed ? 1u : 0u, m_activeCount, staticRebuilds, staticReused);

    m_firstExecution = false;
    return cl;
}
