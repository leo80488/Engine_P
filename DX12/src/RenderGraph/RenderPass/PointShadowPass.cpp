#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/ShadowCasterShaders.h"   // ShadowCaster hook-PS helpers

#include "ECS/HierarchyComponents.h"    // ViewBit (shadow viewMask)
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Check.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

using namespace DirectX;

// The cube atlas as this pass's import. Allocated at the frame boundary (EnsureAtlas, from
// OnService); the graph reads the member at each Execute and transitions it
// (ImportTracking::Graph).
void PointShadowPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_atlasH = d.Import(RG::PassTex::kShadowPointCube,
        { RHI::Format::D32_FLOAT, m_faceSize, m_faceSize, true, false, false, kFaceCount },
        { &m_atlas, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
}

// Root parameter slots - aliases of the shared graphics root signature
// (Graphics/RootSignatureLayout.h).
static constexpr auto kShadowPerViewSlot = RHI::Slots::GfxRel::CB_b1;       // CB b1 space0 -> BindCB index 0
static constexpr auto kInstanceBufSlot   = RHI::Slots::Gfx::InstanceBuf;     // root SRV t0 space0 (unused; indirect draw handles it)
static constexpr auto kMeshDescSlot      = RHI::Slots::Gfx::MeshDesc;        // root SRV t1 space0
static constexpr auto kBindlessSlot      = RHI::Slots::Gfx::BindlessBuffers; // desc table bindless g_Buffers[]
static constexpr auto kBindlessTexSlot   = RHI::Slots::Gfx::BindlessTex;     // t0 space2 - g_AllTextures[]
static constexpr auto kMaterialBufSlot   = RHI::Slots::GfxRel::SRV_t2;       // t2 space0 (SRV-relative index)

// The retained per-cube args use the shared CSM bucket order.
static_assert(PointShadowPass::kShadowGroupCount == ShadowDrawGroup::RetainedCount,
              "PointShadowPass retained-arg buckets must mirror ShadowDrawGroup");

PointShadowPass::PointShadowPass()
{
    for (uint32_t i = 0; i < kFaceCount; ++i)
        XMStoreFloat4x4(&m_pendingVP[i], XMMatrixIdentity());
}

PointShadowPass::~PointShadowPass()
{
    if (!m_gfxPtr) return;
    m_faceCBs.Destroy(*m_gfxPtr);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_indirectArgMapped[i])           m_gfxPtr->UnmapBuffer(m_indirectArgBuffer[i]);
        if (m_indirectArgBuffer[i].IsValid()) m_gfxPtr->DestroyBuffer(m_indirectArgBuffer[i]);
        m_indirectArgMapped[i] = nullptr;
    }
    if (m_atlas.IsValid()) m_gfxPtr->DestroyTexture(m_atlas);
}

void PointShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // Reuse the CSM / Spot shadow shaders: depth-only with a single ShadowPerViewCB at b1 space0,
    // all a cube face needs.
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "Shadow.vs.hlsl");
    // A masked custom material's shadow VS when it has no custom VS (ShadowCasterShaders.h); the
    // material's PS resolves through the one DynamicShaderRegistry, as in every pass library.
    m_shaderLib.Register(ShaderID::ShadowHook_VS, RHI::ShaderStage::VS, "ShadowHook.vs.hlsl");
    m_shaderLib.Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm)))
        LOG_ERROR("PointShadowPass: base PSO creation failed");

    // ---- Cube shadow atlas: NOT created here ----
    // 1024^2 x 24 slices x D32 = 96 MiB, allocated on demand by EnsureAtlas (OnService); a world
    // with no shadow-casting point light pays nothing. Everything below is small and stays eager.

    // ---- Per-face CB pool (256 B per face in each of two banks, per frame in flight) ----
    if (!m_faceCBs.Create(gfx, "PointShadowPass.FaceCBs"))
        LOG_ERROR("PointShadowPass: face CB pool creation failed");

    // ---- Indirect draw command buffer (same layout as SpotShadowPass) ----
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

    // The run-time sampler for custom casters (SetDynamicSampler): linear wrap, what their material
    // shaders read as g_LinearWrap here. Shadow.ps's own alpha test names the fixed SMP_LINEAR_WRAP
    // slot.
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU = RHI::TextureAddressMode::WRAP;
        sd.addressV = RHI::TextureAddressMode::WRAP;
        sd.addressW = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("PointShadowPass: alpha-test sampler creation failed");
    }

    LOG_SUCCESS("PointShadowPass: initialised (Cube %ux%u x %u cubes DEFERRED, indirect=%s)",
                m_faceSize, m_faceSize, kMaxCasters,
                m_indirectArgMapped[0] ? "YES" : "NO");
}

// Allocates only; ReleaseAtlas is the sole free path. One-frame lag by design, as in
// SpotShadowPass::EnsureAtlas: `wanted` comes from Renderer::ExtractShadowAtlasDemand at the top
// of the game half, so a caster spawned on frame N allocates on N+1 (the same gap follows a world
// load, whose OnWorldClear released the atlas). Frame N renders with no shadow, not a wrong one:
// BuildScene_UploadLights leaves shadowSliceIdx at 0xFFFFFFFF and Lighting.ps skips
// SamplePointShadow.
void PointShadowPass::EnsureAtlas(bool wanted)
{
    if (!m_gfxPtr || !wanted || m_atlas.IsValid() || m_allocFailed) return;

    // D32_FLOAT array of 6*kMaxCasters slices; the TEXTURECUBE misc flag makes
    // the RHI build a TextureCubeArray<float> SRV + per-face DSVs.
    RHI::TextureDesc td;
    td.width      = m_faceSize;
    td.height     = m_faceSize;
    td.format     = RHI::Format::D32_FLOAT;
    td.bindFlags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.mipLevels = 1;
    td.arraySize = kFaceCount;                       // 6 * kMaxCasters
    td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
    td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0); // reversed Z
    td.debugName = "PointShadowPass.CubeAtlas";
    // _ONCE throttles the LOG only; m_allocFailed stops the retry. The check lives in this
    // function on purpose: an ENGINE_CHECK_ONCE latch is a static local at the call site, so a
    // helper shared with SpotShadowPass would let one atlas's failure silence the other's.
    if (!ENGINE_CHECK_ONCE(m_gfxPtr->CreateTexture(td, m_atlas),
                           "PointShadowPass: cube shadow atlas creation failed "
                           "(%ux%u x %u faces D32, %.0f MiB) — point shadows "
                           "disabled for this world",
                           m_faceSize, m_faceSize, kFaceCount,
                           static_cast<double>(static_cast<uint64_t>(m_faceSize) * m_faceSize
                                               * kFaceCount * 4ull) / (1024.0 * 1024.0)))
    {
        m_atlas       = {};
        m_allocFailed = true;
        return;
    }

    // CPU-side state mirror: CreateTexture leaves a DEPTH_STENCIL-bound texture in DEPTH_WRITE,
    // the before-state Execute's m_firstExecution branch declares. A stale false would make the
    // first barrier after a re-create claim DEPTH_READ_SRV.
    m_firstExecution = true;
    m_activeCount    = 0;
    m_ra             = {};

    LOG_INFO("PointShadowPass: cube atlas allocated (%.1f MiB, shadow-casting "
             "point light(s) in world)",
             static_cast<double>(static_cast<uint64_t>(m_faceSize) * m_faceSize
                                 * kFaceCount * 4ull) / (1024.0 * 1024.0));
}

// Per-world release (see OnWorldClear in the header). DestroyTexture defers the resource and
// its views kFrameCount frames, so an in-flight LightingPass read of the cube finishes safely.
void PointShadowPass::ReleaseAtlas()
{
    // Unconditional: a new world deserves one fresh attempt even if the last
    // allocation failed and left nothing to destroy.
    m_allocFailed = false;

    if (!m_gfxPtr || !m_atlas.IsValid()) return;
    m_gfxPtr->DestroyTexture(m_atlas);   // deferred kFrameCount frames
    m_atlas          = {};
    m_firstExecution = true;
    m_activeCount    = 0;
    m_ra             = {};
    LOG_INFO("PointShadowPass: cube atlas released (%.1f MiB reclaimed, world cleared)",
             static_cast<double>(static_cast<uint64_t>(m_faceSize) * m_faceSize
                                 * kFaceCount * 4ull) / (1024.0 * 1024.0));
}

void PointShadowPass::WarmPipelines(const DrawPacket& dp)
{
    if (!dp.castShadow || (dp.viewMask & ViewBit::ShadowAny) == 0) return;
    const bool alphaTest = dp.permutation.Has(PermutationKey::ALPHA_TEST);
    if (dp.filter == DrawFilter::Transparent && !alphaTest) return;
    PermutationKey perm;
    if (alphaTest) perm.Set(PermutationKey::ALPHA_TEST, true);
    if (ShadowDrawsPerDraw(dp))
    {
        // The per-draw loop: a custom VS, or a masked custom material's SHADOW_HOOK PS, with the
        // record path's fallback.
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

PSODesc PointShadowPass::BuildPSODesc(PermutationKey perm, uint32_t customVSID,
                                      bool depthOnly, uint32_t hookPSID) const
{
    PSODesc desc;
    if (hookPSID != 0)
    {
        // A masked custom material: its own PS in the SHADOW_HOOK variant decides the coverage
        // (ShadowCasterShaders.h).
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
    // No-PS bucket: Shadow.ps's only work for a discard-free draw is the unconditional
    // dissolveParams fetch + a dead [branch], and binding a may-discard PS costs the pass its
    // early-Z depth write - six faces per cube here, so the win multiplies. Drop the stage when
    // the whole group is provably discard-free.
    if (hookPSID == 0)
        desc.psID    = depthOnly ? PSODesc::kNoPixelShader : ShaderID::Shadow_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

    // CullMode::None: thin/open occluders must still cast on all 6 faces.
    desc.rs.cullMode = RHI::CullMode::NONE;
    // Depth clip ON, as in SpotShadowPass: each face is a 90 deg perspective, and with depth clip
    // off a triangle with two vertices behind a face's plane through the light (w < 0) is lost -
    // a floor or ceiling tile around a lamp drops out of the faces that should hold it.
    desc.rs.depthClipEnable = true;

    // Slope-scale + constant bias (reversed-Z, D32_FLOAT, GREATER_EQUAL), as in SpotShadowPass:
    // all bias on the caster side, so the receiver-side compare in Lighting.ps stays unbiased
    // (avoids leaks).
    desc.rs.depthBias              = -1;
    desc.rs.slopeScaledDepthBias = -1.0f;
    desc.rs.depthBiasClamp        = 0.0f;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvCount  = 0;
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

uint64_t PointShadowPass::GetCubeAtlasSrvHandle() const
{
    if (!m_gfxPtr || !m_atlas.IsValid()) return 0;
    return m_gfxPtr->GetTextureSRVGpuHandle(m_atlas);
}

void PointShadowPass::UploadFaceCBs()
{
    if (!m_gfxPtr) return;
    auto* pool = m_faceCBs.Current(*m_gfxPtr);
    if (!pool) return;
    const uint32_t activeFaces = m_activeCount * kFacesPerCube;
    for (uint32_t i = 0; i < activeFaces; ++i)
    {
        XMMATRIX m = XMLoadFloat4x4(&m_pendingVP[i]);
        XMFLOAT4X4 t;
        XMStoreFloat4x4(&t, XMMatrixTranspose(m));
        uint8_t* dst = pool->slots + i * kFaceCBStride;
        std::memcpy(dst, &t, sizeof(XMFLOAT4X4));

        // Custom-VS (WPO) time + camera at fixed offset 192 (CUSTOM_VS_SHADOW).
        float timeParams[4] = { m_cvTime, m_cvDeltaTime,
                                std::sin(m_cvTime), std::cos(m_cvTime) };
        float cameraPos[4]  = { m_cvCameraPos.x, m_cvCameraPos.y, m_cvCameraPos.z, 0.0f };
        std::memcpy(dst + 192, timeParams, sizeof(timeParams));
        std::memcpy(dst + 208, cameraPos,  sizeof(cameraPos));
        // Dissolve noise bindless index at offset 224 (Shadow.ps ShadowPerViewCB) and the
        // instance-remap heap index at 228 (Shadow.vs / CUSTOM_VS_SHADOW).
        uint32_t dissolveIdx[4] = { m_dissolveNoiseIdx, m_instanceRemapIdx, 0u, 0u };
        std::memcpy(dst + 224, dissolveIdx, sizeof(dissolveIdx));

        // Second CB bank (see FaceCBPool): bank one with the owning cube's WORKING remap heap index at
        // byte 228 - the compacted commands' instance ranges live in that remap, not the pristine
        // one. Written only while retained args are active, so the ring path pays nothing extra.
        const uint32_t cube = i / kFacesPerCube;
        if (m_ra.active && cube < kMaxCasters && m_ra.remapHeapIdx[cube] != 0)
        {
            uint8_t* dstW = pool->slots + (kFaceCount + i) * kFaceCBStride;
            std::memcpy(dstW, dst, kFaceCBStride);
            const uint32_t workingIdx = m_ra.remapHeapIdx[cube];
            std::memcpy(dstW + 228, &workingIdx, sizeof(workingIdx));
        }
    }
}

RHI::CommandList PointShadowPass::Execute(RHI::CommandList cl)
{
    if (!m_atlas.IsValid()) return cl;
    // With the atlas imported the graph transitions it (DEPTHSTENCIL before this pass,
    // DEPTH_READ_SRV before each reader), so transitionAtlas barriers only without an import
    // (-rgimportoff). The m_firstExecution bookkeeping runs either way.
    const bool graphTracked = m_atlasH.IsValid();
    auto transitionAtlas = [&](RHI::ResourceState before, RHI::ResourceState after)
    {
        if (!graphTracked) cl.PushBarrier(RHI::GPUBarrier::Image(&m_atlas, before, after));
    };

    // No shadow-casting point lights -> skip the clear and the draws. The first frame still owes
    // one DEPTHSTENCIL -> SRV transition so LightingPass can bind the cleared cube (reversed-Z 0 ==
    // no shadow).
    if (m_activeCount == 0)
    {
        if (m_firstExecution)
        {
            transitionAtlas(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
            m_firstExecution = false;
        }
        return cl;
    }

    IGraphicsDevice& gfx = *m_gfxPtr;

    // Checked BEFORE the entry barrier: a bail between the entry and exit barriers would leave the
    // atlas in DEPTH_WRITE for Lighting to sample and give the next frame's entry barrier a wrong
    // before-state. The bail leaves the atlas exactly as the no-active-lights path does.
    const RHI::GPUBuffer& faceCBBuf = m_faceCBs.CurrentBuffer(gfx);
    if (!faceCBBuf.IsValid())
    {
        if (m_firstExecution)
        {
            transitionAtlas(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
            m_firstExecution = false;
        }
        return cl;
    }

    // On subsequent frames the atlas arrives in DEPTH_READ_SRV (sampled last
    // frame). Transition back to DEPTHSTENCIL for writes.
    if (!m_firstExecution)
    {
        transitionAtlas(RHI::ResourceState::DEPTH_READ_SRV, RHI::ResourceState::DEPTHSTENCIL);
    }

    // Always clear every active face - stale depth would corrupt the compare.
    for (uint32_t s = 0; s < m_activeCount * kFacesPerCube; ++s)
        gfx.ClearDepthStencilSlice(m_atlas, s, 0.0f, 0, cl); // reversed Z

    // Pull the draw lists we need ONCE (shared across all faces).
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);

    // ---- Per-cube GPU-culled args (see RetainedShadowArgs) ----
    // Gated on m_honorShadowFlags: the cull CS always applies castShadow + the ShadowAny viewMask
    // (ClassifyShadowDrawGroup / requiredViewBits), so ignoring both works only on the CPU ring
    // path.
    const bool retainedArgs = m_ra.active && m_honorShadowFlags;

    if (opaqueDraws.empty() && shadowDraws.empty() && transparentDraws.empty())
    {
        transitionAtlas(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
        m_firstExecution = false;
        return cl;
    }

    UploadFaceCBs();

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const uint32_t frameSlot      = gfx.GetFrameIndex();
    const bool useIndirect = (m_indirectArgMapped[frameSlot] != nullptr);

    // Honor CAST_SHADOW + the shadow viewMask as CSM does, unless the compat flag turns it off.
    // Applied identically in every loop below.
    const bool honorFlags = m_honorShadowFlags;
    auto skipCaster = [honorFlags](const DrawPacket& dp) -> bool {
        return honorFlags &&
               (!dp.castShadow || (dp.viewMask & ViewBit::ShadowAny) == 0);
    };

    // Depth-only eligibility for the non-alpha group: when every packet it draws is discard-free
    // its PSO drops the pixel shader (see ShadowDrawNeedsPS). Group 1 is ALPHA_TEST and always
    // keeps Shadow_PS. Same filters as the group-0 count / write / fallback loops below.
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

    // Retained path: the same eligibility per ShadowDrawGroup bucket. This scan walks the FULL
    // un-culled packet list while the GPU cull may drop instances and whole commands from the
    // bucket drawn. That stays correct because culling only removes: "no packet in this bucket
    // can discard" over the superset holds for any subset. Never derive a permissive flag this way.
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

    // Build the indirect args once, split alpha / non-alpha - the same layout as SpotShadowPass -
    // and replay them for every cube face. Skipped on the retained path (the per-cube working
    // args replace it).
    uint32_t groupOffsets[2] = { 0, 0 };
    uint32_t groupCounts[2]  = { 0, 0 };

    if (useIndirect && !retainedArgs)
    {
        auto* args = static_cast<IndirectDrawCommand*>(m_indirectArgMapped[frameSlot]);

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

        // The writeIdx cap can truncate a group mid-write, and groupCounts / groupOffsets (from the
        // uncapped countAlpha totals) go straight to ExecuteIndirectDraw as maxCommands and a byte
        // offset: left stale they point past the last written command (or, if group 0 alone
        // overran, start group 1 beyond the buffer). Re-derive both from what was written; a no-op
        // when nothing was dropped.
        const uint32_t requestedCommands = groupCounts[0] + groupCounts[1];
        groupCounts[0]  = written0;
        groupCounts[1]  = writeIdx - written0;
        groupOffsets[0] = 0;
        groupOffsets[1] = written0 * sizeof(IndirectDrawCommand);

        // _ONCE: this is per-frame, and the point-shadow caster set rarely shrinks on its own - an
        // un-latched line would repeat forever.
        ENGINE_CHECK_ONCE(writeIdx >= requestedCommands,
            "PointShadowPass: indirect command budget exhausted — %u/%u "
            "commands written, %u caster draws dropped this frame (their cube "
            "shadows are simply missing, with nothing on screen naming this "
            "cap). Raise PointShadowPass::kMaxIndirectCommands.",
            writeIdx, kMaxIndirectCommands, requestedCommands - writeIdx);
    }

    // ---- Render each active cube's 6 faces into its slices ----
    // (faceCBBuf was validated before the entry barrier.)
    for (uint32_t face = 0; face < m_activeCount * kFacesPerCube; ++face)
    {
        gfx.SetDepthStencilSlice(m_atlas, face, cl);

        cl.SetViewport(m_faceSize, m_faceSize);
        cl.SetScissorRect(m_faceSize, m_faceSize);
        cl.SetPrimitiveTopology();

        // Retained: the CSM buckets and their meshlet twins from this cube's culled args. Ring: the
        // non-alpha / alpha pair. The cube index is the cull-view owner of all 6 of its faces.
        const uint32_t cube = face / kFacesPerCube;
        const int groupLoopCount = retainedArgs
                                 ? static_cast<int>(kShadowGroupCount) : 2;

        for (int g = 0; g < groupLoopCount; ++g)
        {
            if (retainedArgs)
            {
                // Capacity 0 = the CPU classification put no command in this bucket; the GPU survivor count
                // can only be <=. A zero remap heap index means this cube got no cull view (never while
                // m_ra.active - creation is all-or-nothing), and the compacted ranges resolve only through
                // that remap, so do not draw without one.
                if (cube >= kMaxCasters || m_ra.groupCap[g] == 0 ||
                    !m_ra.workingArgs[cube] || !m_ra.countBuffer ||
                    m_ra.remapHeapIdx[cube] == 0)
                    continue;
            }
            else if (useIndirect && groupCounts[g] == 0) continue;

            // Rasterizer cull stays NONE for every bucket: cube shadows draw double-sided, and the
            // ShadowDrawGroup split is reused only for its arg layout, not for CSM's per-bucket cull
            // modes. A meshlet twin (g >= MeshletOffset) draws as its base bucket.
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

            // Each face occupies one 256-aligned slot inside the pool CB.
            // Retained group draws bind bank two (byte 228 = this CUBE's
            // working remap); everything else keeps bank one (pristine remap,
            // full instance ranges).
            {
                const uint64_t cbSlot = retainedArgs
                    ? static_cast<uint64_t>(kFaceCount + face)
                    : static_cast<uint64_t>(face);
                cl.GetDevice().BindConstantBufferAtOffset(kShadowPerViewSlot,
                                                          faceCBBuf,
                                                          cbSlot * kFaceCBStride,
                                                          cl);
            }

            // Bindless g_AllTextures[] for EVERY group: the per-instance dissolve clip in Shadow.ps samples
            // the noise texture (through the fixed SMP_LINEAR_WRAP slot).
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
                // real count is the GPU survivor total at this CUBE's slice of
                // the shared grid. countBase already folds in the point family
                // base, so the element read is countBase[cube] + bucket.
                gfx.ExecuteIndirectDraw(
                    *m_ra.workingArgs[cube],
                    uint64_t(m_ra.groupBase[g]) * sizeof(IndirectDrawCommand),
                    m_ra.groupCap[g],
                    m_ra.countBuffer,
                    uint64_t(m_ra.countBase[cube] + g) * sizeof(uint32_t),
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

        // ---- Custom-VS (WPO) casters - depth-only, per-draw, into this face ----
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
                        // The hook PSO does not build as a shadow: fall back to the caster's non-hook shadow
                        // shaders (ShadowCasterShaders.h).
                        ShadowCaster::NoteHookFallback(hookPS, "PointShadowPass");
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
                            kShadowPerViewSlot, faceCBBuf,
                            static_cast<uint64_t>(face) * kFaceCBStride, cl);
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

    transitionAtlas(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);

    m_firstExecution = false;
    return cl;
}
