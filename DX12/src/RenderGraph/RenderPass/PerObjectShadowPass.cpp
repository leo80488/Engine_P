#include "RenderGraph/RenderPass/PerObjectShadowPass.h"
#include "RenderGraph/RenderPass/ShadowCasterShaders.h"   // ShadowCaster: the masked-material shadow hook

#include "ECS/HierarchyComponents.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstring>

using namespace DirectX;

// Root parameter slots: aliases of the shared graphics root signature
// (Graphics/RootSignatureLayout.h).
static constexpr auto kShadowPerViewSlot = RHI::Slots::GfxRel::CB_b1;       // CB b1 space0 -> BindConstantBufferAtOffset index 0
static constexpr auto kInstanceBufSlot   = RHI::Slots::Gfx::InstanceBuf;     // root SRV t0 space0
static constexpr auto kMeshDescSlot      = RHI::Slots::Gfx::MeshDesc;        // root SRV t1 space0
static constexpr auto kBindlessSlot      = RHI::Slots::Gfx::BindlessBuffers; // desc table bindless g_Buffers[]
static constexpr auto kBindlessTexSlot   = RHI::Slots::Gfx::BindlessTex;     // desc table bindless g_AllTextures[] (alpha-test PS)
static constexpr auto kMaterialBufSlot   = RHI::Slots::GfxRel::SRV_t2;       // t2 space0 (SRV-relative index)

PerObjectShadowPass::PerObjectShadowPass(Variant variant)
    : m_variant(variant)
    , m_mapSize(variant == Variant::Hair ? kHairShadowMapSize : kShadowMapSize)
{
    XMStoreFloat4x4(&m_pendingVP, XMMatrixIdentity());
}

PerObjectShadowPass::~PerObjectShadowPass()
{
    if (!m_gfxPtr) return;
    m_casterCB.Destroy(*m_gfxPtr);
    if (m_map.IsValid()) m_gfxPtr->DestroyTexture(m_map);
}

void PerObjectShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // Reuse the CSM shadow shaders: depth-only, one ShadowPerViewCB at b1.
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "Shadow.vs.hlsl");
    // A masked custom material's shadow VS when it has no custom VS (ShadowCasterShaders.h); the
    // material's PS resolves here through the one DynamicShaderRegistry, as in every pass
    // library.
    m_shaderLib.Register(ShaderID::ShadowHook_VS, RHI::ShaderStage::VS, "ShadowHook.vs.hlsl");
    m_shaderLib.Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm)))
        LOG_ERROR("%s: base PSO creation failed", GetName());

    // Pre-create the ALPHA_TEST permutation too: Init runs serially at graph Compile while
    // Execute records on parallel workers, and with two instances of this pass building
    // byte-identical PSODescs a lazy first use on both workers races the pipeline library's
    // StorePipeline on the same name (harmless, but it logs an E_INVALIDARG warning).
    PermutationKey alphaPerm;
    alphaPerm.Set(PermutationKey::ALPHA_TEST, true);
    if (!m_psoCache.GetOrCreate(BuildPSODesc(alphaPerm)))
        LOG_ERROR("%s: alpha-test PSO creation failed", GetName());

    // Same for the base group's no-PS (depth-only) variant: it is chosen per frame from the
    // caster packets, so its first use would land mid-frame on a worker, and it flips back and
    // forth as the player dissolves. Build it up front so no worker creates it live.
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm, 0, true)))
        LOG_ERROR("%s: depth-only PSO creation failed", GetName());

    // ---- Shadow map: single Texture2D<D32_FLOAT> ----
    {
        RHI::TextureDesc td;
        td.width      = m_mapSize;
        td.height     = m_mapSize;
        td.format     = RHI::Format::D32_FLOAT;
        td.bindFlags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.mipLevels = 1;
        td.arraySize = 1;                              // single slice -> Texture2D SRV/DSV
        td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0); // reversed Z
        td.debugName = m_variant == Variant::Hair ? "HairShadowPass.Map"
                                                   : "PerObjectShadowPass.Map";
        if (!gfx.CreateTexture(td, m_map))
            LOG_ERROR("%s: shadow map creation failed", GetName());
    }

    // ---- Caster CB (triple-buffered, 256 B) ----
    if (!m_casterCB.Create(gfx, m_variant == Variant::Hair
                                    ? "HairShadowPass.CasterCB"
                                    : "PerObjectShadowPass.CasterCB"))
        LOG_ERROR("%s: caster CB creation failed", GetName());

    // Linear-wrap sampler for alpha-test PS (matches GBuffer.ps's g_LinearWrap).
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU = RHI::TextureAddressMode::WRAP;
        sd.addressV = RHI::TextureAddressMode::WRAP;
        sd.addressW = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("%s: alpha-test sampler creation failed", GetName());
    }

    LOG_SUCCESS("%s: initialised (Map %ux%u D32)",
                GetName(), m_mapSize, m_mapSize);
}

PSODesc PerObjectShadowPass::BuildPSODesc(PermutationKey perm, uint32_t customVSID,
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
    // No-PS bucket: Shadow.ps's only work for a discard-free draw is the
    // unconditional dissolveParams fetch + dead [branch], and binding a
    // may-discard PS costs the pass its early-Z depth WRITE. The player is
    // the usual dissolve target, so this bucket naturally falls back to
    // Shadow_PS the moment the character starts dissolving.
    if (hookPSID == 0)
        desc.psID    = depthOnly ? PSODesc::kNoPixelShader : ShaderID::Shadow_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

    // NONE cull avoids thin-feature leaks (matches SpotShadowPass). Self-shadow
    // acne is handled on the caster side with a negative slope-scaled bias
    // (reversed-Z: pushes stored depth AWAY from the light).
    desc.rs.cullMode               = RHI::CullMode::NONE;
    desc.rs.depthClipEnable       = false;
    desc.rs.depthBias              = -1;
    desc.rs.slopeScaledDepthBias = -1.5f;   // a touch more than spot: tight map, steep player surfaces
    desc.rs.depthBiasClamp        = 0.0f;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvCount  = 0;
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

uint64_t PerObjectShadowPass::GetShadowSrvHandle() const
{
    if (!m_gfxPtr || !m_map.IsValid()) return 0;
    return m_gfxPtr->GetTextureSRVGpuHandle(m_map);
}

void PerObjectShadowPass::UploadCasterCB()
{
    if (!m_gfxPtr) return;
    auto* slot = m_casterCB.Current(*m_gfxPtr);
    if (!slot) return;

    XMMATRIX m = XMLoadFloat4x4(&m_pendingVP);
    XMFLOAT4X4 t;
    XMStoreFloat4x4(&t, XMMatrixTranspose(m));
    std::memcpy(slot->bytes, &t, sizeof(XMFLOAT4X4));

    // Custom-VS (WPO) time + camera at fixed offset 192 (matches
    // custom_vertex.hlsli CUSTOM_VS_SHADOW ShadowPerViewCB). 256-byte stride.
    float timeParams[4] = { m_cvTime, m_cvDeltaTime,
                            std::sin(m_cvTime), std::cos(m_cvTime) };
    float cameraPos[4]  = { m_cvCameraPos.x, m_cvCameraPos.y, m_cvCameraPos.z, 0.0f };
    std::memcpy(slot->bytes + 192, timeParams, sizeof(timeParams));
    std::memcpy(slot->bytes + 208, cameraPos,  sizeof(cameraPos));
    // Dissolve noise bindless index at offset 224 (Shadow.ps ShadowPerViewCB) + the
    // instance-remap heap index at 228 (Shadow.vs / CUSTOM_VS_SHADOW).
    uint32_t dissolveIdx[4] = { m_dissolveNoiseIdx, m_instanceRemapIdx, 0u, 0u };
    std::memcpy(slot->bytes + 224, dissolveIdx, sizeof(dissolveIdx));
}

RHI::CommandList PerObjectShadowPass::Execute(RHI::CommandList cl)
{
    if (!m_map.IsValid()) return cl;
    // With the map imported the graph transitions it (DEPTHSTENCIL before this pass,
    // DEPTH_READ_SRV before each reader), so these are the pass's own transitions only without an
    // import (-rgimportoff). The flag bookkeeping stays either way.
    const bool graphTracked = m_mapH.IsValid();
    auto transitionMap = [&](RHI::ResourceState before, RHI::ResourceState after)
    {
        if (!graphTracked) cl.PushBarrier(RHI::GPUBarrier::Image(&m_map, before, after));
    };

    // Disabled (no player / feature off / no sun): LightCB.perObjShadowParams.x is 0, so
    // Lighting.ps never samples the map; keep it in SRV state, since LightCB's index still names
    // it. The first frame owes the one-shot DEPTHSTENCIL -> DEPTH_READ_SRV transition; later
    // frames it is already there.
    if (!m_enabled)
    {
        if (m_firstExecution)
        {
            transitionMap(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
            m_firstExecution = false;
        }
        return cl;
    }

    // Enabled: always clear, even when the player has no visible packets (fully culled). A
    // cleared map = no occluder -> SamplePerObjectShadow returns lit -> min(csm, 1) = csm.
    // Skipping the clear would leave stale occluder depth under the repositioned matrix.
    IGraphicsDevice& gfx = *m_gfxPtr;

    // Gather the lists the caster's packets can live in.
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);

    // Which packet tag this instance renders (Player subtree vs bangs subtree).
    // Per-object membership is only a routing tag: the authored material/entity
    // cast-shadow gates and shadow-view mask remain authoritative, exactly as in
    // the CSM/spot/point passes.
    const bool hairVariant = (m_variant == Variant::Hair);
    auto isCaster = [hairVariant](const DrawPacket& dp)
    {
        if (!dp.castShadow) return false;
        if ((dp.viewMask & ViewBit::ShadowAny) == 0) return false;
        return hairVariant ? dp.hairShadowCaster != 0 : dp.perObjectShadowCaster != 0;
    };

    // Depth-only eligibility for the non-alpha group: when every packet it draws is discard-free
    // its PSO drops the pixel shader (see ShadowDrawNeedsPS). Group 1 is ALPHA_TEST and always
    // keeps Shadow_PS. Same filters as the g == 0 drawList below (the transparent list only
    // contributes alpha-test cards, so it cannot affect this group).
    bool group0NoPS = true;
    {
        auto scanList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                if (!isCaster(dp))      continue;
                if (ShadowDrawsPerDraw(dp)) continue;   // per-draw loop, keeps its PS
                if (dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;
                if (ShadowDrawNeedsPS(dp)) group0NoPS = false;
            }
        };
        scanList(opaqueDraws); scanList(shadowDraws);
    }

    // Diagnostic: logs the tagged caster-packet count when it changes.
    {
        auto cnt = [&](DrawList l){ int n=0; for (const DrawPacket& dp : l) if (isCaster(dp)) ++n; return n; };
        const int tagged = cnt(opaqueDraws) + cnt(shadowDraws) + cnt(transparentDraws);
        static int s_lastTagged[2] = { -1, -1 };
        int& last = s_lastTagged[hairVariant ? 1 : 0];
        if (tagged != last) { LOG_INFO("[%s] pass enabled, tagged caster packets = %d", GetName(), tagged); last = tagged; }
    }

    // On subsequent frames the map arrives in DEPTH_READ_SRV (sampled last frame).
    if (!m_firstExecution)
    {
        transitionMap(RHI::ResourceState::DEPTH_READ_SRV, RHI::ResourceState::DEPTHSTENCIL);
    }

    gfx.ClearDepthStencil(m_map, 0.0f, 0, cl); // reversed Z

    UploadCasterCB();

    const RHI::GPUBuffer& casterCBBuf = m_casterCB.CurrentBuffer(gfx);
    if (!casterCBBuf.IsValid())
    {
        transitionMap(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);
        m_firstExecution = false;
        return cl;
    }

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();

    gfx.SetDepthStencilTarget(m_map, cl);
    cl.SetViewport(m_mapSize, m_mapSize);
    cl.SetScissorRect(m_mapSize, m_mapSize);
    cl.SetPrimitiveTopology();

    // ---- Standard (non-custom-VS) draws, split alpha / non-alpha ----
    for (int g = 0; g < 2; ++g)
    {
        PermutationKey perm;
        if (g == 1) perm.Set(PermutationKey::ALPHA_TEST, true);

        const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
            BuildPSODesc(perm, 0, (g == 0) && group0NoPS));
        if (!pso || !pso->IsValid()) continue;

        bool psoBound      = false;
        bool alphaGlobals  = false;

        auto drawList = [&](DrawList list, bool transparentSrc)
        {
            for (const DrawPacket& dp : list)
            {
                if (!isCaster(dp))             continue;       // this variant's casters only
                if (ShadowDrawsPerDraw(dp))        continue;       // custom-VS loop below
                const bool isAlpha = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                if (transparentSrc && !isAlpha) continue;      // pure-blend never casts here
                if (isAlpha != (g == 1))        continue;      // wrong group

                if (!psoBound)
                {
                    cl.SetPipelineState(*pso);
                    cl.BindDescriptorHeaps();
                    cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
                    cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
                    if (bindlessHandle)
                        cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
                    cl.GetDevice().BindConstantBufferAtOffset(
                        kShadowPerViewSlot, casterCBBuf, 0, cl);
                    // Bindless textures + sampler for the dissolve clip
                    // (needed by every group, not just alpha-test).
                    if (cl.gfx)
                    {
                        const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle();
                        if (texTable)
                            cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
                    }
                    if (m_alphaSamplerIdx >= 0) cl.SetDynamicSampler(m_alphaSamplerIdx);   // custom casters' material sampler
                    psoBound = true;
                }

                // ALPHA_TEST PS additionally reads MaterialBuffer (alphaRef).
                if (g == 1 && !alphaGlobals)
                {
                    if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer))
                        if (matBuf->IsValid()) cl.BindBufferSRV(kMaterialBufSlot, *matBuf);
                    alphaGlobals = true;
                }

                cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
                cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
            }
        };
        drawList(opaqueDraws,      false);
        drawList(shadowDraws,      false);
        drawList(transparentDraws, true);
    }

    // ---- Custom-VS (WPO) player draws: depth-only, per draw ----
    {
        const RHI::PipelineState* cvPSO = nullptr;
        bool cvAlphaGlobals = false;
        auto drawCustomVs = [&](DrawList list, bool transparentSrc)
        {
            for (const DrawPacket& dp : list)
            {
                if (!isCaster(dp))             continue;
                if (!ShadowDrawsPerDraw(dp))        continue;
                const bool isAlpha = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                if (transparentSrc && !isAlpha) continue;

                PermutationKey perm;
                if (isAlpha) perm.Set(PermutationKey::ALPHA_TEST, true);

                const uint32_t hookPS = ShadowCaster::HookPS(dp);
                const RHI::PipelineState* pso =
                    m_psoCache.GetOrCreate(BuildPSODesc(perm, dp.customVSID, false, hookPS));
                if ((!pso || !pso->IsValid()) && hookPS != 0)
                {
                    // The hook does not build as a shadow: fall back to the caster's plain shadow shaders
                    // (ShadowCasterShaders.h).
                    ShadowCaster::NoteHookFallback(hookPS, "PerObjectShadowPass");
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
                        kShadowPerViewSlot, casterCBBuf, 0, cl);
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

                if (isAlpha && !cvAlphaGlobals)
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

    transitionMap(RHI::ResourceState::DEPTHSTENCIL, RHI::ResourceState::DEPTH_READ_SRV);

    m_firstExecution = false;
    return cl;
}
