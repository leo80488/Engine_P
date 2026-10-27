#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/ShadowCasterShaders.h"   // masked custom casters' hook shaders
#include "ECS/Components.h"             // ShadowCullMode enum
#include "ECS/HierarchyComponents.h"    // ViewBit
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShadowSystem.h"
#include "RenderGraph/RenderContext.h"
#include "RenderGraph/RenderPass/TerrainPass.h"
#include "System/Log.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "../../shaders/meshlet_shared.hlsli"   // kMeshletStat* layout, kMeshletNoData

#include <DirectXMath.h>
#include <cstring>

using namespace DirectX;

// Root slots: aliases of the shared graphics root signature (Graphics/RootSignatureLayout.h).
// The per-cascade shadow view-projection CB takes the GBuffer pass's PerViewCB slot.
static constexpr auto kShadowPerViewSlot  = RHI::Slots::GfxRel::CB_b1;      // CB slot 0 -> b1 space0
static constexpr auto kMeshletCullSlot    = RHI::Slots::GfxRel::CB_b4;      // the cascade's MeshletCullCB bank
static constexpr auto kInstanceBufSlot    = RHI::Slots::Gfx::InstanceBuf;
static constexpr auto kMeshDescSlot       = RHI::Slots::Gfx::MeshDesc;
static constexpr auto kBindlessSlot       = RHI::Slots::Gfx::BindlessBuffers;
// Alpha-test / dissolve globals shared by every shadow group.
static constexpr auto kBindlessTexSlot    = RHI::Slots::Gfx::BindlessTex;      // t0 space2 - g_AllTextures[]
static constexpr auto kMaterialBufSlot    = RHI::Slots::GfxRel::SRV_t2;        // t2 space0 (SRV-relative index)
// Terrain shadow rendering root slots.
static constexpr auto kTerrainHeightmapRootSlot = RHI::Slots::Gfx::SRV_t2;       // t2 space0 (per-draw SRV table, ALL vis)
static constexpr auto kTerrainParamsCBSlot      = RHI::Slots::GfxRel::CB_b2;     // b2 space0 (CB-relative slot 1)
// The heightmap sampler is the fixed SMP_LINEAR_CLAMP heap slot
// Terrain.shadow.ms names.

ShadowPass::ShadowPass() = default;

ShadowPass::~ShadowPass()
{
    if (m_gfxPtr)
    {
        m_cascadeCBs.Destroy(*m_gfxPtr);
        DestroyArgRing(*m_gfxPtr, m_mainArgs);
        if (m_shadowArray.IsValid())
            m_gfxPtr->DestroyTexture(m_shadowArray);
    }
}

void ShadowPass::Setup(RG::RenderGraphBuilder& /*b*/)
{
    // Records outside the graph on its own command list, so nothing is declared here.
    // With graph imports on, Renderer imports the cascade array as RG::PassTex::kShadowCSM
    // (Owner tracking: the barriers stay this pass's).
}

void ShadowPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm, RHI::CullMode::BACK)))
        LOG_ERROR("ShadowPass: PSO rebuild after shader reload failed");
    BuildTerrainShadowPSO(gfx);
    if (m_meshletCapable) PrewarmMeshletPSO();
}

// Terrain shadow PSO - Terrain_Shadow_AS/MS, no PS, depth-only, with the same
// reversed-Z test and depth bias as the BACK-cull shadow PSO so the cascades stay
// acne-free on terrain.
bool ShadowPass::BuildTerrainShadowPSO(IGraphicsDevice& gfx)
{
    // The capability gate TerrainPass and GrassPass apply: without mesh shaders (or
    // with -force_no_mesh_shader) no AS / MS is compiled and no mesh PSO is built;
    // RenderTerrainShadow then skips on the invalid PSO.
    if (!gfx.GetCapabilities().meshShader)
    {
        m_terrainShadowPSO = {};
        LOG_INFO("ShadowPass: no mesh shaders - terrain does not cast shadows");
        return false;
    }
    const RHI::Shader* as = m_shaderLib.GetShader(ShaderID::Terrain_Shadow_AS);
    const RHI::Shader* ms = m_shaderLib.GetShader(ShaderID::Terrain_Shadow_MS);
    if (!as || !as->IsValid())
    {
        LOG_ERROR("ShadowPass: Terrain_Shadow_AS not found — terrain will not cast shadows");
        return false;
    }
    if (!ms || !ms->IsValid())
    {
        LOG_ERROR("ShadowPass: Terrain_Shadow_MS not found — terrain will not cast shadows");
        return false;
    }

    // Bias follows the reversed-Z convention (BuildPSODesc's BACK case): depth grows
    // toward the light, so a NEGATIVE depthBias pushes the caster away from the
    // receiver. A positive bias moves it closer and covers the terrain in acne.
    //
    // depthClipEnable = false: clamp, not clip, at the near / far planes - the
    // standard for shadow casting.
    RHI::RasterizerState rs{};
    rs.fillMode               = RHI::FillMode::SOLID;
    rs.cullMode               = RHI::CullMode::BACK;
    rs.frontCounterClockwise = false;
    rs.depthClipEnable       = false;
    rs.depthBias              = -50;
    rs.slopeScaledDepthBias = -1.5f;
    rs.depthBiasClamp        = 0.0f;

    RHI::DepthStencilState dss{};
    dss.depthEnable     = true;
    dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed Z
    dss.stencilEnable   = false;

    RHI::BlendState bs{};   // depth-only - no colour writes

    RHI::PipelineStateDesc pd{};
    pd.as          = as;
    pd.ms          = ms;
    pd.ps          = nullptr;     // depth-only
    pd.rs          = &rs;
    pd.dss         = &dss;
    pd.bs          = &bs;
    pd.il          = nullptr;
    pd.pt          = RHI::PrimitiveTopology::TRIANGLELIST;     // ignored for MS PSO
    pd.rtvCount   = 0;
    pd.dsvFormat  = RHI::Format::D32_FLOAT;                   // matches m_shadowArray
    pd.sampleCount = 1;

    if (!gfx.CreatePipelineState(pd, m_terrainShadowPSO))
    {
        LOG_ERROR("ShadowPass: terrain shadow PSO creation failed");
        return false;
    }
    LOG_INFO("ShadowPass: terrain shadow PSO ready");
    return true;
}

// The terrain PSO uses the shared graphics root signature, like the rest of the
// pass, so the per-CL bindings persist.
void ShadowPass::RenderTerrainShadow(RHI::CommandList cl, int cascadeIdx)
{
    if (!m_terrainPass || !m_terrainShadowPSO.IsValid())     return;
    if (cascadeIdx < 0 || cascadeIdx >= kCascadeCount)       return;
    if (!m_gfxPtr)                                           return;
    const RHI::GPUBuffer& cascadeCBBuf = m_cascadeCBs.CurrentBuffer(*m_gfxPtr);
    if (!cascadeCBBuf.IsValid())                             return;
    if (!m_terrainParamsCB || !m_terrainParamsCB->IsValid()) return;

    const auto& tile = m_terrainPass->GetTileBindings();
    if (tile.dispatchAsGroupCount == 0 || tile.heightmapSRV == 0) return;

    cl.SetPipelineState(m_terrainShadowPSO);
    cl.BindDescriptorHeaps();

    // b1: the cascade's ShadowPerViewCB, re-bound in case the draw loop never bound
    // it for this cascade. Cascade i sits at i * kCascadeCBStride in the pool.
    cl.GetDevice().BindConstantBufferAtOffset(kShadowPerViewSlot,
                                              cascadeCBBuf,
                                              static_cast<uint64_t>(cascadeIdx) * kCascadeCBStride,
                                              cl);

    // b2 TerrainCB. ShadowPass records outside the graph, so the graph-published
    // "TerrainParams" BindCB lookup fails here: bind it through the device, like the
    // cascade CB. Renderer rebinds the buffer pointer each frame.
    cl.GetDevice().BindConstantBuffer(kTerrainParamsCBSlot, *m_terrainParamsCB, cl);

    // t2 heightmap SRV, the same descriptor table slot as the colour pass.
    cl.BindDescriptorTableHandle(kTerrainHeightmapRootSlot, tile.heightmapSRV);

    cl.DispatchMesh(tile.dispatchAsGroupCount, 1, 1);
}

void ShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    // ---- Shadow shaders / PSO ----
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "Shadow.vs.hlsl");
    // A masked custom material's shadow VS when it has no custom VS
    // (ShadowCasterShaders.h); the material's PS resolves here through the one
    // DynamicShaderRegistry, as in every pass library.
    m_shaderLib.Register(ShaderID::ShadowHook_VS, RHI::ShaderStage::VS, "ShadowHook.vs.hlsl");
    m_shaderLib.Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_Shadow_AS, RHI::ShaderStage::AS, "Terrain.shadow.as.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_Shadow_MS, RHI::ShaderStage::MS, "Terrain.shadow.ms.hlsl");
    // The CSM meshlet path. Metadata only - nothing compiles unless a capable
    // instance pre-warms the mesh PSO below.
    m_shaderLib.Register(ShaderID::ShadowMeshlet_AS, RHI::ShaderStage::AS, "ShadowMeshlet.as.hlsl");
    m_shaderLib.Register(ShaderID::ShadowMeshlet_MS, RHI::ShaderStage::MS, "ShadowMeshlet.ms.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PermutationKey basePerm;
    if (!m_psoCache.GetOrCreate(BuildPSODesc(basePerm, RHI::CullMode::BACK)))
        LOG_ERROR("ShadowPass: base PSO creation failed");
    else
        LOG_INFO("ShadowPass: base PSO ready");

    // ---- Terrain shadow PSO ----
    BuildTerrainShadowPSO(gfx);

    // ---- CSM meshlet path ----
    if (m_meshletCapable)
    {
        PrewarmMeshletPSO();
        CreateMeshletStatsBuffers(gfx);
    }
#if WITH_HARNESS
    m_forceTwinCpu = Sys::CommandLine::Get().Has("-meshletgroupcpu");
#endif

    // The run-time sampler for custom casters: linear wrap, what their vertex hooks
    // read as g_LinearWrap here. Shadow.ps's own alpha test names the fixed
    // SMP_LINEAR_WRAP heap slot.
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU = RHI::TextureAddressMode::WRAP;
        sd.addressV = RHI::TextureAddressMode::WRAP;
        sd.addressW = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("ShadowPass: failed to create alpha-test wrap sampler");
    }

    // ---- Shadow Texture2DArray (size^2 x kCascadeCount) ----
    // One array resource: a single SRV binding, sampled via float3(uv, sliceIndex)
    // with no dynamic indexing.
    {
        RHI::TextureDesc td;
        td.width      = m_shadowMapSize;
        td.height     = m_shadowMapSize;
        td.format     = RHI::Format::D32_FLOAT;
        td.bindFlags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.mipLevels = 1;
        td.arraySize = kCascadeCount;    // one array slice per cascade
        td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0);  // reversed Z
        td.debugName = "ShadowPass.CascadeArray";

        if (!gfx.CreateTexture(td, m_shadowArray))
        {
            LOG_ERROR("ShadowPass: shadow Texture2DArray creation failed");
            return;
        }
    }

    // IGraphicsDevice::CreateTexture creates the per-slice DSVs (arraySize > 1 with DEPTH_STENCIL).

    // ---- Per-cascade shadow VP constant buffers (UPLOAD) ----
    // Layout (must match Shadow.vs.hlsl + Terrain.shadow.{as,ms}.hlsl):
    //   float4x4 shadowViewProj;           //  64 B  (transposed for HLSL row-vector)
    //   float4   shadowFrustumPlanes[6];   //  96 B  (inward-normal world-space planes)
    // then the tail UploadCascadeCBs writes at bytes 192..231, in a 256-byte slot.
    // Terrain.shadow.as.hlsl reads the planes for per-cascade frustum culling. One
    // triple-buffered FrameCB<> pool, bound via BindConstantBufferAtOffset(slot *
    // kCascadeCBStride).
    if (!m_cascadeCBs.Create(gfx, "ShadowPass.CascadeCBs"))
        LOG_ERROR("ShadowPass: cascade CB pool creation failed");

    // ---- ExecuteIndirect arg buffers (UPLOAD, one per frame in flight) ----
    CreateArgRing(gfx, m_mainArgs);

    LOG_SUCCESS("ShadowPass: initialized — Texture2DArray %ux%ux%d, indirect=%s",
        m_shadowMapSize, m_shadowMapSize, kCascadeCount,
        m_mainArgs.mapped[0] ? "YES" : "NO");
}

void ShadowPass::CreateArgRing(IGraphicsDevice& gfx, ArgRing& ring)
{
    RHI::GPUBufferDesc desc;
    desc.size       = static_cast<uint64_t>(kMaxIndirectCommands) * sizeof(IndirectDrawCommand);
    desc.stride     = sizeof(IndirectDrawCommand);
    desc.usage      = RHI::Usage::UPLOAD;
    desc.bindFlags = RHI::BindFlag::NONE;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (gfx.CreateBuffer(desc, ring.buffer[i]))
            ring.mapped[i] = gfx.MapBuffer(ring.buffer[i]);
    }
}

void ShadowPass::DestroyArgRing(IGraphicsDevice& gfx, ArgRing& ring)
{
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (ring.mapped[i])          gfx.UnmapBuffer(ring.buffer[i]);
        if (ring.buffer[i].IsValid()) gfx.DestroyBuffer(ring.buffer[i]);
        ring.mapped[i] = nullptr;
    }
}

void ShadowPass::ViewArgs::Release(IGraphicsDevice& gfx)
{
    DestroyArgRing(gfx, ring);
}

void ShadowPass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
    {
        m_args = &m_mainArgs;
        return;
    }
    ArgRing& ring = view.Data<ViewArgs>(this).ring;
    if (!ring.buffer[0].IsValid() && m_gfxPtr) CreateArgRing(*m_gfxPtr, ring);
    m_args = &ring;
}

uint64_t ShadowPass::GetShadowArrayGpuHandle() const
{
    if (!m_gfxPtr || !m_shadowArray.IsValid()) return 0;
    return m_gfxPtr->GetTextureSRVGpuHandle(m_shadowArray);
}

void ShadowPass::PrepareUnwritten(RHI::CommandList cl)
{
    if (!m_firstExecution || !m_gfxPtr || !m_shadowArray.IsValid()) return;
    for (int cascade = 0; cascade < kCascadeCount; ++cascade)
        m_gfxPtr->ClearDepthStencilSlice(m_shadowArray, static_cast<uint32_t>(cascade), 0.0f, 0, cl);   // reversed Z
    cl.PushBarrier(RHI::GPUBarrier::Image(
        &m_shadowArray,
        RHI::ResourceState::DEPTHSTENCIL,
        RHI::ResourceState::DEPTH_READ_SRV));
    m_firstExecution = false;
}

void ShadowPass::WarmPipelines(const DrawPacket& dp)
{
    if (!dp.castShadow || (dp.viewMask & ViewBit::ShadowAny) == 0) return;
    const bool alphaTest = dp.permutation.Has(PermutationKey::ALPHA_TEST);
    if (ShadowDrawsPerDraw(dp))
    {
        // The per-draw loop (Execute): a custom VS or a masked custom material's
        // SHADOW_HOOK PS; transparent sources only as alpha-tested cards; alpha test ->
        // NONE, mirrored -> NONE, else the material's shadow cull mode.
        if (dp.filter == DrawFilter::Transparent && !alphaTest) return;
        if (dp.filter == DrawFilter::Custom || IsSecondaryViewFilter(dp.filter)) return;
        RHI::CullMode  cull = RHI::CullMode::BACK;
        PermutationKey perm;
        if (alphaTest)
        {
            cull = RHI::CullMode::NONE;
            perm.Set(PermutationKey::ALPHA_TEST, true);
        }
        else if (dp.mirrored) cull = RHI::CullMode::NONE;
        else switch (static_cast<ShadowCullMode>(dp.shadowCullMode))
        {
            case ShadowCullMode::Front: cull = RHI::CullMode::FRONT; break;
            case ShadowCullMode::None:  cull = RHI::CullMode::NONE;  break;
            default:                    cull = RHI::CullMode::BACK;  break;
        }
        const uint32_t hookPS = ShadowCaster::HookPS(dp);
        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(perm, cull, dp.customVSID, false, hookPS));
        // A hook that does not build as a shadow: the record path's fallback.
        if ((!pso || !pso->IsValid()) && hookPS != 0)
            (void)m_psoCache.GetOrCreate(BuildPSODesc(perm, cull, dp.customVSID));
        return;
    }
    // The indirect groups (Execute's groupPSO table).
    const int g = ClassifyShadowDrawGroup(dp, ViewBit::ShadowAny);
    if (g < 0 || g >= ShadowDrawGroup::Count) return;
    PermutationKey perm;
    RHI::CullMode  cull = RHI::CullMode::BACK;
    switch (g)
    {
    case ShadowDrawGroup::OpaqueFront: cull = RHI::CullMode::FRONT; break;
    case ShadowDrawGroup::OpaqueNone:  cull = RHI::CullMode::NONE;  break;
    case ShadowDrawGroup::AlphaTest:   cull = RHI::CullMode::NONE;
                                       perm.Set(PermutationKey::ALPHA_TEST, true); break;
    default: break;
    }
    const bool noPS = g != ShadowDrawGroup::AlphaTest && !ShadowDrawNeedsPS(dp);
    (void)m_psoCache.GetOrCreate(BuildPSODesc(perm, cull, 0, noPS));
}

PSODesc ShadowPass::BuildPSODesc(PermutationKey perm, RHI::CullMode cullMode,
                                 uint32_t customVSID, bool depthOnly, uint32_t hookPSID) const
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
        // Material custom vertex shader -> its depth-only variant
        // (custom_vertex.hlsli #if CUSTOM_VS_SHADOW). PS stays Shadow_PS.
        desc.vsID = static_cast<ShaderID>(customVSID);
        perm.Set(PermutationKey::CUSTOM_VS_SHADOW, true);
    }
    else
    {
        desc.vsID = ShaderID::Shadow_VS;
    }
    // No-PS bucket: for a non-alpha-test, non-dissolving draw Shadow.ps only fetches
    // InstanceBuffer[].dissolveParams and skips a dead [branch]. A bound (may-discard)
    // PS costs the shadow stack its early-Z depth WRITE, so drop the stage when the
    // group is provably discard-free. Not combined with customVSID: those casters
    // render per-draw and stay conservative.
    if (hookPSID == 0)
        desc.psID    = depthOnly ? PSODesc::kNoPixelShader : ShaderID::Shadow_PS;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;  // PVF

    // Cull mode is caller-supplied (per-material ShadowCullMode):
    //   BACK  - opaque default; the bias fights self-shadow acne
    //   FRONT - closed-mesh characters: only back faces reach the map, so self-shadow
    //           acne is impossible and bias is HARMFUL - the back faces graze the
    //           light, |dz/dx| explodes, and slope-scaled bias pushes the stored depth
    //           past the receiver, so the shadow vanishes on other surfaces
    //   NONE  - single-sided geometry (hair cards, cloth, foliage cards): both sides
    //           rasterize, acne still possible, so a middle-ground bias
    // Alpha test is a second axis: it forces NONE whatever the material says.
    //
    // The caster-side slope-scaled bias is the first line of defence; the receiver
    // adds normal-offset and receiver-plane depth bias (shadow.hlsli), so these values
    // stay conservative and only catch the residual.
    desc.rs.cullMode               = cullMode;
    desc.rs.depthClipEnable       = false;
    desc.rs.depthBiasClamp        = 0.0f;

    switch (cullMode)
    {
    case RHI::CullMode::FRONT:
        // ZERO bias - trust the cull. Back faces mostly GRAZE the light (silhouettes,
        // thin limbs), where max|dz/dx| explodes, so even a -0.25 slope term (unclamped:
        // depthBiasClamp = 0) pushes the stored depth metres past the receiver and the
        // mesh stops casting. The front-back separation IS the object's thickness, orders
        // of magnitude above D32 precision, so no safety bias is needed.
        desc.rs.depthBias              = 0;
        desc.rs.slopeScaledDepthBias = 0.0f;
        break;
    case RHI::CullMode::NONE:
        // Alpha-test cards / single-sided geometry: both sides rasterize, so self-shadow
        // is possible, but the surfaces are thin and the full BACK bias would open
        // visible gaps.
        desc.rs.depthBias              = -30;
        desc.rs.slopeScaledDepthBias = -1.0f;
        break;
    default: // BACK
        desc.rs.depthBias              = -50;
        desc.rs.slopeScaledDepthBias = -1.5f;
        break;
    }

    // Depth test and write, no color output.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvCount  = 0;                           // depth-only
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

PSODesc ShadowPass::BuildMeshletPSODesc(PermutationKey perm, RHI::CullMode cullMode, bool depthOnly) const
{
    PSODesc desc = BuildPSODesc(perm, cullMode, 0, depthOnly);
    desc.asID = ShaderID::ShadowMeshlet_AS;
    desc.msID = ShaderID::ShadowMeshlet_MS;
    return desc;
}

const RHI::PipelineState* ShadowPass::MeshletPso(PermutationKey perm, RHI::CullMode cullMode, bool depthOnly)
{
    const PSODesc desc = BuildMeshletPSODesc(perm, cullMode, depthOnly);
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (pso && pso->IsValid()) return pso;
    // Built in the background (PSOCache::SetDeferMisses): this frame's twins draw through the VS, and the
    // path stays on.
    if (m_psoCache.IsPending(desc)) return nullptr;
    // A record-time failure (a broken shader reload): this frame's twins draw
    // on the CPU through the VS, and the latch drops the path from the next.
    ENGINE_CHECK_ONCE(false, "ShadowPass: meshlet PSO creation failed at record time (perm=0x%08X cull=%d "
                      "noPS=%d); the CSM meshlet path is off from the next frame",
                      perm.bits, static_cast<int>(cullMode), depthOnly ? 1 : 0);
    m_meshletPsoReady.store(false, std::memory_order_release);
    return nullptr;
}

bool ShadowPass::PrewarmMeshletPSO()
{
    PermutationKey basePerm;
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
        BuildMeshletPSODesc(basePerm, RHI::CullMode::BACK, /*depthOnly=*/true));
    const bool ok = pso && pso->IsValid();
    m_meshletPsoReady.store(ok, std::memory_order_release);
    if (ok) LOG_INFO("[MESHLET] pso ready pass=ShadowPass");
    else    LOG_WARNING("[MESHLET] ShadowPass mesh PSO unavailable - the CSM keeps Shadow.vs");
    return ok;
}

void ShadowPass::CreateMeshletStatsBuffers(IGraphicsDevice& gfx)
{
    const uint64_t bytes = uint64_t(kRetainedCullCascades) * MeshletShared::kMeshletStatsCount * sizeof(uint32_t);
    RHI::GPUBufferDesc d;
    d.size       = bytes;
    d.stride     = sizeof(uint32_t);
    d.usage      = RHI::Usage::DEFAULT;
    d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    bool ok = gfx.CreateBuffer(d, m_mlStatsBuffer);
    RHI::GPUBufferDesc z = d;
    z.usage      = RHI::Usage::UPLOAD;
    z.bindFlags = RHI::BindFlag::NONE;
    if (ok) ok = gfx.CreateBuffer(z, m_mlStatsZero);
    if (ok)
    {
        if (void* p = gfx.MapBuffer(m_mlStatsZero)) { std::memset(p, 0, static_cast<size_t>(bytes)); gfx.UnmapBuffer(m_mlStatsZero); }
        else ok = false;
    }
    RHI::GPUBufferDesc r = d;
    r.usage      = RHI::Usage::READBACK;
    r.bindFlags = RHI::BindFlag::NONE;
    for (uint32_t i = 0; ok && i < RHI::kFramesInFlight; ++i)
        ok = gfx.CreateBuffer(r, m_mlStatsReadback[i]);
    if (!ok)
    {
        LOG_WARNING("[MESHLET] CSM statistics buffers unavailable - the shadow meshlet counters will read as absent");
        if (m_mlStatsBuffer.IsValid()) gfx.DestroyBuffer(m_mlStatsBuffer);
        if (m_mlStatsZero.IsValid())   gfx.DestroyBuffer(m_mlStatsZero);
        for (auto& b : m_mlStatsReadback) if (b.IsValid()) gfx.DestroyBuffer(b);
        m_mlStatsBuffer = {};
        m_mlStatsZero   = {};
        for (auto& b : m_mlStatsReadback) b = {};
    }
}

uint32_t ShadowPass::MeshletStatsUavIndex(IGraphicsDevice& gfx) const
{
    if (!m_mlStatsBuffer.IsValid()) return MeshletShared::kMeshletNoData;
    const uint64_t h = gfx.GetBufferUAVGpuHandle(m_mlStatsBuffer);
    const uint32_t idx = h ? gfx.DescriptorHeapIndexFromGpuHandle(h) : ~0u;
    return idx == ~0u ? MeshletShared::kMeshletNoData : idx;
}

// Before this frame records into its slot: that slot's previous copy is done
// (the frame ring waited for it).
void ShadowPass::HarvestMeshletStats(IGraphicsDevice& gfx)
{
    const uint32_t slot = gfx.GetFrameIndex() % RHI::kFramesInFlight;
    if (!m_mlStatsPending[slot]) return;
    m_mlStatsPending[slot] = false;
    if (!m_mlStatsReadback[slot].IsValid()) return;
    MeshletShadowStats st;
    if (const uint32_t* p = static_cast<const uint32_t*>(gfx.MapBuffer(m_mlStatsReadback[slot])))
    {
        std::memcpy(st.counters, p, sizeof(st.counters));
        gfx.UnmapBuffer(m_mlStatsReadback[slot]);
        st.valid       = true;
        st.frame       = m_mlStatsFrame[slot];
        st.twinGroups  = m_mlTwinGroups[slot];
        st.cpuFallback = m_mlCpuFallback[slot];
        std::lock_guard<std::mutex> lk(m_mlStatsMutex);
        m_mlStatsLast = st;
    }
}

// Pack each cascade's ShadowPerViewCB:
//   [  0 ..  63] transposed VP (HLSL row-vector convention)
//   [ 64 .. 159] 6 inward-normal world-space frustum planes from the UN-transposed
//                VP (Gribb-Hartmann); Terrain.shadow.as.hlsl culls with them
//   [192 .. 223] custom-VS time + camera
//   [224 .. 231] dissolve noise index, instance-remap index
void ShadowPass::UploadCascadeCBs()
{
    if (!m_sys || !m_gfxPtr) return;
    auto* pool = m_cascadeCBs.Current(*m_gfxPtr);
    if (!pool) return;
    const DirectX::XMFLOAT4X4* src = m_sys->CascadeMatricesForPass();
    for (int i = 0; i < kCascadeCount; ++i)
    {
        // ---- Matrix ----
        XMMATRIX mat = XMLoadFloat4x4(&src[i]);
        XMFLOAT4X4 transposed;
        XMStoreFloat4x4(&transposed, XMMatrixTranspose(mat));

        uint8_t* dst = pool->slots + i * kCascadeCBStride;
        std::memcpy(dst, &transposed, sizeof(transposed));

        // ---- Frustum planes (from the untransposed VP, Gribb-Hartmann) ----
        // Same convention as RenderTypes.h ExtractFrustumPlanes: inward normals,
        // dot(plane.xyz, P) + plane.w >= 0 -> inside.
        const auto& m = src[i].m;
        auto makePlane = [](float a, float b, float c, float d, float* out) {
            const float invLen = 1.0f / std::sqrt(a*a + b*b + c*c);
            out[0] = a * invLen;
            out[1] = b * invLen;
            out[2] = c * invLen;
            out[3] = d * invLen;
        };
        float planes[6][4];
        // Left:   col[3] + col[0]
        makePlane(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0], planes[0]);
        // Right:  col[3] - col[0]
        makePlane(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0], planes[1]);
        // Bottom: col[3] + col[1]
        makePlane(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1], planes[2]);
        // Top:    col[3] - col[1]
        makePlane(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1], planes[3]);
        // Near:   col[2]
        makePlane(m[0][2],           m[1][2],           m[2][2],           m[3][2],           planes[4]);
        // Far:    col[3] - col[2]
        makePlane(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2], planes[5]);

        std::memcpy(dst + sizeof(transposed), planes, sizeof(planes));

        // ---- Custom-VS (WPO) time + camera at fixed offset 192 ----
        // custom_vertex.hlsli's CUSTOM_VS_SHADOW ShadowPerViewCB reads time at byte 192
        // and camera at 208 (past the 160-byte matrix + planes block), so WPO casters
        // deform exactly as in the GBuffer and the cast shadow matches the lit silhouette.
        float timeParams[4] = { m_cvTime, m_cvDeltaTime,
                                std::sin(m_cvTime), std::cos(m_cvTime) };
        float cameraPos[4]  = { m_cvCameraPos.x, m_cvCameraPos.y, m_cvCameraPos.z, 0.0f };
        std::memcpy(dst + 192, timeParams, sizeof(timeParams));
        std::memcpy(dst + 208, cameraPos,  sizeof(cameraPos));
        // Dissolve noise bindless index at offset 224 (Shadow.ps ShadowPerViewCB)
        // + instance-remap heap index at 228 (Shadow.vs / CUSTOM_VS_SHADOW).
        uint32_t dissolveIdx[4] = { m_dissolveNoiseIdx, m_instanceRemapIdx, 0u, 0u };
        std::memcpy(dst + 224, dissolveIdx, sizeof(dissolveIdx));

        // Bank two (see CascadeCBPool) = bank one with the cascade's per-view WORKING
        // remap heap index at byte 228. Written ONLY while retained args are active this
        // frame: the ring path never binds bank two and must not pay for it. Cascades past
        // kRetainedCullCascades (the terrain-only far cascade) have no cull view; nothing
        // binds their bank-two slot.
        if (m_ra.active && i < static_cast<int>(kRetainedCullCascades) &&
            m_ra.remapHeapIdx[i] != 0)
        {
            uint8_t* dstW = pool->slots + (kCascadeCount + i) * kCascadeCBStride;
            std::memcpy(dstW, dst, kCascadeCBStride);
            const uint32_t workingIdx = m_ra.remapHeapIdx[i];
            std::memcpy(dstW + 228, &workingIdx, sizeof(workingIdx));
        }
    }
}

RHI::CommandList ShadowPass::Execute(RHI::CommandList cl)
{
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    // Forward-translucent packets carry castShadow 0 (Renderer::ApplyBlendMode),
    // so nothing in this list casts unless -shadow_translucent_casters.
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);
    if (opaqueDraws.empty() && shadowDraws.empty() && transparentDraws.empty()) return cl;

    if (!m_shadowArray.IsValid()) return cl;

    UploadCascadeCBs();

    IGraphicsDevice& gfx = *m_gfxPtr;

    // Do not move this check past the entry barrier: an early return there skips the
    // exit barrier, leaving the array in DEPTH_WRITE for Lighting to sample and the
    // next frame's entry barrier claiming a before-state the array is not in. The
    // buffer's validity depends only on the ring's Create(), so testing it here is
    // equivalent.
    const RHI::GPUBuffer& cascadeCBBuf = m_cascadeCBs.CurrentBuffer(gfx);
    if (!cascadeCBBuf.IsValid()) return cl;

    if (!m_firstExecution)
        cl.PushBarrier(RHI::GPUBarrier::Image(
            &m_shadowArray,
            RHI::ResourceState::DEPTH_READ_SRV,
            RHI::ResourceState::DEPTHSTENCIL));

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const uint32_t frameSlot      = gfx.GetFrameIndex();
    const bool useIndirect = (m_args->mapped[frameSlot] != nullptr);

    // ---- CSM meshlet statistics ----
    // Harvest the slot's previous copy, then zero the counters before the first twin
    // of this frame records (the banks carry the UAV index).
    HarvestMeshletStats(gfx);
    const uint32_t statsSlot = frameSlot % RHI::kFramesInFlight;
    const bool meshletStats = m_ra.active && m_ra.meshlet && m_mlStatsBuffer.IsValid() &&
                              m_mlStatsZero.IsValid() && m_mlStatsReadback[statsSlot].IsValid();
    uint32_t twinGroups = 0, cpuFallback = 0;
    if (meshletStats)
    {
        const uint64_t bytes = uint64_t(kRetainedCullCascades) * MeshletShared::kMeshletStatsCount * sizeof(uint32_t);
        gfx.CopyBufferRegion(m_mlStatsZero, 0, m_mlStatsBuffer, 0, bytes, cl);   // COMMON -> COPY_DEST (implicit)
        cl.PushBarrier(RHI::GPUBarrier::Buffer(&m_mlStatsBuffer, RHI::ResourceState::COPY_DST,
                                               RHI::ResourceState::UNORDERED_ACCESS));
    }

    // ---- 4-group classification ----
    // Alpha test is one bucket (its own shader permutation; cull forced NONE whatever
    // the material says - alpha-test cards are single-sided).
    //
    // Other draws honour the per-material ShadowCullMode:
    //   Default (0) -> BACK  : bias-based anti-acne
    //   Front   (1) -> FRONT : closed-mesh characters (no acne)
    //   None    (2) -> NONE  : cloth / single-sided props that need both sides
    enum DrawGroup {
        GRP_OPAQUE_BACK  = 0,
        GRP_OPAQUE_FRONT = 1,
        GRP_OPAQUE_NONE  = 2,
        GRP_ALPHATEST    = 3,
        GRP_COUNT        = 4,
        GRP_SKIP         = -1,   // material opted out of casting shadow
    };

    const bool excludePerObj = m_excludePerObjCasters;
    // Bucket rules live in ClassifyShadowDrawGroup (RenderTypes.h), ONE definition
    // shared with the Renderer's retained shadow-group meta fill, so the ring and
    // retained layouts never disagree. The wrapper adds the only per-FRAME rule:
    // character (per-object) casters render into the high-res PerObjectShadowPass
    // instead and are excluded here.
    //
    // The reason is the SEAM, not double-darkening: min() is idempotent, so a caster
    // in both maps would not darken twice (shadow.hlsli folds the two), but the maps
    // differ in resolution and bias, so the shadow visibly steps where the fitted
    // per-object frustum ends.
    //
    // The retained path applies the same exclusion in the cull CS
    // (kInstanceFlagPerObjShadow).
    static_assert(GRP_OPAQUE_BACK  == ShadowDrawGroup::OpaqueBack &&
                  GRP_OPAQUE_FRONT == ShadowDrawGroup::OpaqueFront &&
                  GRP_OPAQUE_NONE  == ShadowDrawGroup::OpaqueNone &&
                  GRP_ALPHATEST    == ShadowDrawGroup::AlphaTest &&
                  GRP_COUNT        == ShadowDrawGroup::Count &&
                  GRP_SKIP         == ShadowDrawGroup::Skip,
                  "ShadowPass buckets must mirror ShadowDrawGroup");
    auto classify = [excludePerObj](const DrawPacket& dp) -> int {
        if (excludePerObj && dp.perObjectShadowCaster) return GRP_SKIP;
        return ClassifyShadowDrawGroup(dp, ViewBit::ShadowAny);
    };

    // ---- Per-group depth-only eligibility ----
    // A group whose every packet is discard-free renders with NO pixel shader
    // (ShadowDrawNeedsPS, BuildPSODesc's depthOnly). One scan of the three lists -
    // cheaper than the per-cascade custom-VS walk below - and it must run on the
    // retained path too (which skips the ring build). ALPHA_TEST is excluded by
    // construction; spelled out so the intent survives a change to
    // depthPrepassEligible.
    bool groupNoPS[GRP_COUNT];
    for (int g = 0; g < GRP_COUNT; ++g) groupNoPS[g] = true;
    {
        auto scanList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                const int g = classify(dp);
                if (g < 0 || g >= GRP_COUNT) continue;
                if (ShadowDrawNeedsPS(dp)) groupNoPS[g] = false;
            }
        };
        scanList(opaqueDraws); scanList(shadowDraws); scanList(transparentDraws);
        groupNoPS[GRP_ALPHATEST] = false;
    }

    // Retained args (RetainedShadowArgs): the per-cascade cull dispatches Renderer
    // recorded at the head of THIS command list already produced group-compacted
    // per-cascade args + survivor counts, so the CPU ring build is skipped.
    const bool retainedArgs = m_ra.active;

    uint32_t groupOffsets[GRP_COUNT] = {};   // byte offset into indirect buffer
    uint32_t groupCounts [GRP_COUNT] = {};   // command count

    if (useIndirect && !retainedArgs)
    {
        auto* args = static_cast<IndirectDrawCommand*>(m_args->mapped[frameSlot]);

        // Pass 1: count per group (SKIP bucket ignored).
        auto countList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                const int g = classify(dp);
                if (g < 0 || g >= GRP_COUNT) continue;
                ++groupCounts[g];
            }
        };
        countList(opaqueDraws); countList(shadowDraws); countList(transparentDraws);

        // Prefix sum -> byte offsets
        uint32_t running = 0;
        for (int g = 0; g < GRP_COUNT; ++g)
        {
            groupOffsets[g] = running * sizeof(IndirectDrawCommand);
            running += groupCounts[g];
        }
        const uint32_t totalCommands = running;
        // Warn once: an over-budget scene stays over budget, and a line per frame per
        // view would bury the rest of DX12Log.txt.
        if (totalCommands > kMaxIndirectCommands)
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                LOG_WARNING("ShadowPass: %u commands exceed cap %u — the tail of "
                            "each over-running group is dropped and those casters "
                            "produce no shadow. Raise ShadowPass::"
                            "kMaxIndirectCommands.",
                            totalCommands, kMaxIndirectCommands);
            }
        }

        // Pass 2: write per group, using per-group write cursors.
        uint32_t cursors[GRP_COUNT];
        for (int g = 0; g < GRP_COUNT; ++g)
            cursors[g] = groupOffsets[g] / sizeof(IndirectDrawCommand);

        auto writeList = [&](DrawList list) {
            for (const DrawPacket& dp : list)
            {
                const int g = classify(dp);
                if (g < 0 || g >= GRP_COUNT) continue;   // opted-out caster
                uint32_t  c = cursors[g];
                if (c >= kMaxIndirectCommands) continue;
                IndirectDrawCommand& ic = args[c];
                ic.meshDescIdx            = dp.meshDescriptorIndex;
                ic.instanceOffset         = dp.instanceOffset;
                ic.materialIndex          = dp.materialIndex;
                ic.prevPosInfo            = 0;
                ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                ic.instanceCount          = dp.instanceCount;
                ic.startVertexLocation    = 0;
                ic.startInstanceLocation  = 0;
                cursors[g] = c + 1;
            }
        };
        writeList(opaqueDraws); writeList(shadowDraws); writeList(transparentDraws);

        // Re-derive the group counts from what the writer ACTUALLY wrote. The uncapped
        // pass-1 tally as ExecuteIndirectDraw's maxCommands would walk past the last
        // written command on an overrun (and, for a group whose prefix-sum offset is past
        // the cap, start outside the buffer). Point / spot shadows do the same.
        //
        // A no-op without overflow: writeList advances cursors[g] once per written
        // packet, so cursors[g] == groupOffsets[g]/stride + groupCounts[g] already. A
        // group that starts past the cap collapses to 0, and the draw loop's
        // `groupCounts[g] == 0 -> continue` skips it, so its bad offset is never used.
        for (int g = 0; g < GRP_COUNT; ++g)
        {
            const uint32_t base = groupOffsets[g] / sizeof(IndirectDrawCommand);
            groupCounts[g] = (cursors[g] > base) ? (cursors[g] - base) : 0u;
        }
    }

    // ---- Per-group PSO descs (base shader perm + rasterizer cull mode) ----
    struct GroupPSO { PermutationKey perm; RHI::CullMode cull; };
    GroupPSO groupPSO[GRP_COUNT];
    groupPSO[GRP_OPAQUE_BACK].cull   = RHI::CullMode::BACK;
    groupPSO[GRP_OPAQUE_FRONT].cull  = RHI::CullMode::FRONT;
    groupPSO[GRP_OPAQUE_NONE].cull   = RHI::CullMode::NONE;
    groupPSO[GRP_ALPHATEST].cull     = RHI::CullMode::NONE;
    groupPSO[GRP_ALPHATEST].perm.Set(PermutationKey::ALPHA_TEST, true);

    // ---- Render each cascade ----
    // (cascadeCBBuf was validated before the entry barrier.)
    for (int cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        gfx.ClearDepthStencilSlice(m_shadowArray, cascade, 0.0f, 0, cl);  // reversed Z
        gfx.SetDepthStencilSlice(m_shadowArray, cascade, cl);

        cl.SetViewport(m_shadowMapSize, m_shadowMapSize);
        cl.SetScissorRect(m_shadowMapSize, m_shadowMapSize);
        cl.SetPrimitiveTopology();

        // Cascade kFarCascadeIdx is the terrain-only ultra-far cascade (from the near
        // cascades' reach, kShadowFarCap by default, to kFarCascadeFar in view-Z).
        // Dynamic entities are sub-pixel there, so drawing every DrawPacket into the
        // full-size target a fourth time is wasted: skip the draw loop and let the
        // terrain mesh-shader pass below own this slice.
        const bool terrainOnlyCascade = (cascade == kFarCascadeIdx);

        // Retained mode culls per cascade: only cascades with a cull view draw packets
        // (0..2, the same set the ring path renders).
        const bool retainedCascade =
            retainedArgs && cascade < static_cast<int>(kRetainedCullCascades);

        // Retained: the four buckets and their meshlet twins; ring: four.
        const int groupLoopCount = retainedArgs ? static_cast<int>(ShadowDrawGroup::RetainedCount)
                                                : static_cast<int>(GRP_COUNT);
        for (int g = 0; !terrainOnlyCascade && g < groupLoopCount; ++g)
        {
            // A meshlet twin (g >= MeshletOffset) shares its base bucket's
            // permutation, cull mode and PS choice; only who makes the vertices
            // differs.
            const int  baseGroup    = g % static_cast<int>(GRP_COUNT);
            const bool meshletGroup = g >= static_cast<int>(ShadowDrawGroup::MeshletOffset);
            if (retainedArgs)
            {
                // Capacity 0 = the CPU classification put no command in this bucket (the GPU
                // count can only be <= that). remapHeapIdx 0 mirrors the bank-two write gate in
                // UploadCascadeCBs: the compacted ranges resolve ONLY through that cascade's
                // working remap, so no valid index -> no retained draw (unreachable while
                // creation is all-or-nothing).
                if (!retainedCascade || m_ra.groupCap[g] == 0 ||
                    !m_ra.workingArgs[cascade] || !m_ra.countBuffer ||
                    m_ra.remapHeapIdx[cascade] == 0)
                    continue;
            }
            else if (useIndirect && groupCounts[g] == 0) continue;

            // A twin's commands are DISPATCH_MESH only on a meshlet frame; a twin without its
            // mesh PSO (or banks) draws its packets on the CPU through the VS below
            // (cpuTwin) - its args cannot run as DRAW.
            const RHI::PipelineState* meshPso = (meshletGroup && m_ra.meshlet && m_ra.meshletBanks && !m_forceTwinCpu)
                ? MeshletPso(groupPSO[baseGroup].perm, groupPSO[baseGroup].cull, groupNoPS[baseGroup])
                : nullptr;
            const bool cpuTwin = meshletGroup && !meshPso;
            const RHI::PipelineState* pso = meshPso ? meshPso : m_psoCache.GetOrCreate(
                BuildPSODesc(groupPSO[baseGroup].perm, groupPSO[baseGroup].cull, 0, groupNoPS[baseGroup]));
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            cl.BindDescriptorHeaps();
            cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
            cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
            if (bindlessHandle)
                cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
            {
                // Retained group draws bind CB bank two: byte 228 = this
                // cascade's WORKING remap (their compacted commands' instance
                // ranges live in the per-cascade remap buffer). Everything
                // else keeps bank one (pristine remap, full ranges).
                // A CPU-drawn twin reads full pristine ranges: bank one.
                const uint64_t cbSlot = (retainedArgs && !cpuTwin)
                    ? static_cast<uint64_t>(kCascadeCount + cascade)
                    : static_cast<uint64_t>(cascade);
                cl.GetDevice().BindConstantBufferAtOffset(kShadowPerViewSlot,
                                                          cascadeCBBuf,
                                                          cbSlot * kCascadeCBStride,
                                                          cl);
            }
            // The cascade's MeshletCullCB bank for the amplification shader.
            if (meshPso)
                cl.GetDevice().BindConstantBufferAtOffset(kMeshletCullSlot, *m_ra.meshletBanks,
                                                          static_cast<uint64_t>(cascade) *
                                                              sizeof(MeshletShared::MeshletCullCB),
                                                          cl);

            // Bindless g_AllTextures[] + sampler: EVERY group needs them for Shadow.ps's
            // per-instance dissolve clip (noise sample).
            if (cl.gfx)
            {
                const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle();
                if (texTable)
                    cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
            }
            if (m_alphaSamplerIdx >= 0)
                cl.SetDynamicSampler(m_alphaSamplerIdx);   // custom casters' material sampler
            // ALPHA_TEST PS additionally reads MaterialBuffer (alphaRef).
            if (baseGroup == GRP_ALPHATEST)
            {
                if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer))
                    if (matBuf->IsValid())
                        cl.BindBufferSRV(kMaterialBufSlot, *matBuf);
            }

            if (cpuTwin)
            {
                // The twin's members: the base bucket's packets the meshlet
                // path took this frame (ShadowMeshletEligible - the latch was
                // on when they were emitted). Every instance, no cascade cull.
                auto twinDraw = [&](DrawList list) {
                    for (const DrawPacket& dp : list)
                    {
                        if (classify(dp) != baseGroup || !ShadowMeshletEligible(dp)) continue;
                        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
                        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                    }
                };
                twinDraw(opaqueDraws);
                twinDraw(shadowDraws);
                twinDraw(transparentDraws);
                ++cpuFallback;
            }
            else if (retainedArgs)
            {
                // Per-cascade working args + GPU survivor count: cap is the ExecuteIndirect
                // maxCommands, the count buffer slot holds the compaction CS's survivor total
                // for (cascade, group). A twin's commands are DISPATCH_MESH(blocks, kept, 1).
                const uint64_t argOff   = uint64_t(m_ra.groupBase[g]) * sizeof(IndirectDrawCommand);
                const uint64_t countOff = (uint64_t(cascade) * ShadowDrawGroup::RetainedCount + g) *
                                          sizeof(uint32_t);
                if (meshPso)
                {
                    gfx.ExecuteIndirectDispatchMesh(*m_ra.workingArgs[cascade], argOff, m_ra.groupCap[g],
                                                    m_ra.countBuffer, countOff, cl);
                    ++twinGroups;
                }
                else
                    gfx.ExecuteIndirectDraw(*m_ra.workingArgs[cascade], argOff, m_ra.groupCap[g],
                                            m_ra.countBuffer, countOff, cl);
            }
            else if (useIndirect)
            {
                gfx.ExecuteIndirectDraw(m_args->buffer[frameSlot],
                                        groupOffsets[g],
                                        groupCounts[g],
                                        nullptr, 0, cl);
            }
            else
            {
                // Fallback: per-draw path. The SAME classifier makes a packet emit a draw only
                // when its group is the current one.
                auto fallbackDraw = [&](DrawList list) {
                    for (const DrawPacket& dp : list) {
                        if (classify(dp) != g) continue;
                        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
                        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                    }
                };
                fallbackDraw(opaqueDraws);
                fallbackDraw(shadowDraws);
                fallbackDraw(transparentDraws);
            }
        }

        // ---- Custom-VS (WPO) and hook casters - depth-only, per-draw ----
        // Excluded from the indirect groups: each needs its own VS PSO + b8 params, or
        // its material's own PS (a masked custom material whose hook decides coverage).
        // Few in practice (foliage patches, cut-out signs). Renders the SAME deformed
        // geometry as the GBuffer, through the SAME coverage, so the cast shadow tracks
        // the swaying silhouette.
        if (!terrainOnlyCascade)
        {
            const RHI::PipelineState* cvPSO = nullptr;
            bool cvAlphaGlobals = false;
            auto drawCustomVs = [&](DrawList list) {
                for (const DrawPacket& dp : list)
                {
                    if (!ShadowDrawsPerDraw(dp))                         continue;
                    if (!dp.castShadow)                                  continue;
                    if (excludePerObj && dp.perObjectShadowCaster)      continue; // rendered by PerObjectShadowPass
                    if ((dp.viewMask & ViewBit::ShadowAny) == 0)        continue;
                    if (dp.filter == DrawFilter::Transparent &&
                        !dp.permutation.Has(PermutationKey::ALPHA_TEST)) continue;

                    const bool alphaTest = dp.permutation.Has(PermutationKey::ALPHA_TEST);
                    RHI::CullMode cull = RHI::CullMode::BACK;
                    PermutationKey perm;
                    if (alphaTest)
                    {
                        cull = RHI::CullMode::NONE;
                        perm.Set(PermutationKey::ALPHA_TEST, true);
                    }
                    // Mirrored casters have reversed winding; NONE is the one winding-independent
                    // cull mode. Matches ClassifyShadowDrawGroup's bucket routing (RenderTypes.h),
                    // so a caster lands the same way on both paths.
                    else if (dp.mirrored)  cull = RHI::CullMode::NONE;
                    else switch (static_cast<ShadowCullMode>(dp.shadowCullMode))
                    {
                        case ShadowCullMode::Front: cull = RHI::CullMode::FRONT; break;
                        case ShadowCullMode::None:  cull = RHI::CullMode::NONE;  break;
                        default:                    cull = RHI::CullMode::BACK;  break;
                    }

                    const uint32_t hookPS = ShadowCaster::HookPS(dp);
                    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
                        BuildPSODesc(perm, cull, dp.customVSID, false, hookPS));
                    if ((!pso || !pso->IsValid()) && hookPS != 0)
                    {
                        // The hook does not build as a shadow: fall back to the plain caster shaders
                        // (ShadowCasterShaders.h).
                        ShadowCaster::NoteHookFallback(hookPS, "ShadowPass");
                        pso = m_psoCache.GetOrCreate(BuildPSODesc(perm, cull, dp.customVSID));
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
                            kShadowPerViewSlot, cascadeCBBuf,
                            static_cast<uint64_t>(cascade) * kCascadeCBStride, cl);
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

                    // b8 custom params (the WPO tunables the hook reads) +
                    // optional vertex-fetched custom textures (space3).
                    if (dp.customCbvVA)
                        cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl);
                    if (dp.customTexTable)
                        cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl);

                    cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
                    cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
                }
            };
            drawCustomVs(opaqueDraws);
            drawCustomVs(shadowDraws);
            drawCustomVs(transparentDraws);
        }

        // Terrain shadow casting: depth-only mesh-shader pass into the same cascade
        // slice. No-op without a terrain or before its heightmap is GPU-resident.
        RenderTerrainShadow(cl, cascade);
    }

    cl.PushBarrier(RHI::GPUBarrier::Image(
        &m_shadowArray,
        RHI::ResourceState::DEPTHSTENCIL,
        RHI::ResourceState::DEPTH_READ_SRV));

    // Every twin of the frame is recorded: copy the counters out.
    if (meshletStats)
    {
        const uint64_t bytes = uint64_t(kRetainedCullCascades) * MeshletShared::kMeshletStatsCount * sizeof(uint32_t);
        cl.PushBarrier(RHI::GPUBarrier::Buffer(&m_mlStatsBuffer, RHI::ResourceState::UNORDERED_ACCESS,
                                               RHI::ResourceState::COPY_SRC));
        gfx.CopyBufferRegion(m_mlStatsBuffer, 0, m_mlStatsReadback[statsSlot], 0, bytes, cl);
        m_mlStatsPending[statsSlot] = true;
        m_mlStatsFrame[statsSlot]   = m_ra.frame;
        m_mlTwinGroups[statsSlot]   = twinGroups;
        m_mlCpuFallback[statsSlot]  = cpuFallback;
    }

    m_firstExecution = false;
    return cl;
}
