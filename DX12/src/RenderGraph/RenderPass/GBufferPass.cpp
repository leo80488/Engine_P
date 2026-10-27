#include "RenderGraph/RenderPass/GBufferPass.h"
#include "Graphics/DynamicShaderRegistry.h"   // custom shader names in the PSO line
#include "Graphics/ShadingStencil.h"   // kCodeMask
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/IndirectDrawCommand.h"
#include "Graphics/RootSignatureLayout.h"
#include "ECS/HierarchyComponents.h"   // ViewBit (the per-view filter)
#include "RenderGraph/RenderContext.h"
#include "System/TaskSystem.h"
#include "System/CommandLine.h"
#include "System/ExecutionDomain.h"
#include "System/Log.h"
#include "System/Check.h"
#include "../../shaders/meshlet_shared.hlsli"   // kMeshletDraw* flags, the stats layout

#include <exception>
#include <algorithm>
#include <chrono>

// Root parameter slots - RootSignatureLayout.h is the single source of truth.
// S::  = ABSOLUTE root-parameter index  (BindRootBufferSRV / BindDescriptorTableHandle).
// SR:: = GROUP-RELATIVE index           (BindCB / BindBufferSRV).
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

static bool AllowGBufferIndirect();

GBufferPass::GBufferPass(RG::RGTextureHandle albedo,
                         RG::RGTextureHandle normal,
                         RG::RGTextureHandle surface,
                         RG::RGTextureHandle depth,
                         RG::RGTextureHandle velocity,
                         RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface), m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{}

// The suffix makes the instance name unique (see GetName() in the header).
void GBufferPass::SetNameSuffix(const char* suffix)
{
    // An empty suffix would reproduce the main instance's name (first-match-wins
    // timestamp scan, one shared external-wait key, one merged PIX region): report
    // and keep the name rather than make a duplicate.
    if (!ENGINE_CHECK(suffix && *suffix,
                      "GBufferPass::SetNameSuffix: empty suffix would duplicate '%s'",
                      kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

void GBufferPass::Setup(RG::RenderGraphBuilder& b)
{
    b.WriteRenderTarget(m_albedo);
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteRenderTarget(m_emissive);
    // Depth written, and the whole stencil byte: the clear before the first draw
    // zeroes every field (RenderGraph/StencilBits.h, kClearedBy) and the draws stamp
    // the shading code. With -rgplaneoff it declares a plain depth-stencil write.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Write, RG::Stencil::Write, 0xFF);
    else                                  b.WriteDepthStencil(m_depth);
    // The HDR scene colour is this pass's cleared colour target: Phase A transitions
    // and clears it in this pass's primary CL (imported, as RenderTarget(HdrScene,
    // Load::Clear): a graph barrier, and the bind clears to the import's value).
    // Execute's SetRenderTargetsAndHdr then widens the 1-RT bind to all 6 RTs without
    // clearing again. Do not drop it: the transition would land in a later pass's CL,
    // and this one, submitted first, would draw while HDR is still in last frame's state.
    b.SetColorTarget(RG::BuiltinTexture::HdrSceneColor);
}

void GBufferPass::ReloadShaders(IGraphicsDevice& gfx)
{
    // Drop the compile cache and the PSO map so the next GetOrCreate compiles fresh.
    // The default-permutation PSO is rebuilt here to be warm by the next Execute;
    // dynamic-material PSOs come back lazily as draws hit them.
    // Only the OWNER clears the library: ClearCaches bumps the reload epoch
    // MaterialReflectionSync stamps against, and a borrower clearing it too would
    // double-bump it. The borrower's own PSOCache is still cleared and re-warmed.
    if (OwnsShaderLibrary())
        m_lib->ClearCaches();
    m_psoCache.Clear();
    if (!m_psoCache.GetOrCreate(BuildPSODesc({})))
        LOG_ERROR("GBufferPass: PSO rebuild after shader reload failed");
    // The meshlet path's PSO is gone with the cache; re-warm it so the path
    // latch (MeshletPsoReady) answers for the NEW shaders - a broken edit of
    // SceneMeshlet.*.hlsl turns the path off (reason pso_failed), a fix on.
    if (m_meshletCapable) PrewarmMeshletPSO();
}

// Teardown, also for a SECOND instance. The library half is gated on
// OwnsShaderLibrary(), as Init and ReloadShaders are: ShaderLibrary::Shutdown
// DestroyShader()s every cache entry, which on a borrowed library would take the
// main viewport's DXIL with it. (Dynamic ids live in DynamicShaderRegistry and
// outlive any library.)
// The per-instance PSOCache always goes back. m_linearSamplerIdx cannot - the
// sampler pool is append-only, there is no DestroySampler - so it is only reset to
// -1 for a later Init to re-resolve; CreateSampler's hash dedupe hands back the
// same index, so a second instance costs no sampler slot.
void GBufferPass::Shutdown(IGraphicsDevice& gfx)
{
    DestroyMeshletStatsBuffers(gfx);
    m_psoCache.Shutdown();
    if (OwnsShaderLibrary()) m_ownedLib.Shutdown(gfx);

    if (m_defaultWhite.IsValid())      gfx.DestroyTexture(m_defaultWhite);
    if (m_defaultFlatNormal.IsValid()) gfx.DestroyTexture(m_defaultFlatNormal);
    if (m_defaultBlack.IsValid())      gfx.DestroyTexture(m_defaultBlack);
    if (m_defaultGrey.IsValid())       gfx.DestroyTexture(m_defaultGrey);
    if (m_defaultGreySRGB.IsValid())   gfx.DestroyTexture(m_defaultGreySRGB);
    m_defaultWhiteGpuHandle      = 0;
    m_defaultFlatNormalGpuHandle = 0;
    m_linearSamplerIdx           = -1;
    ClearIndirectDraw();
}

const RHI::Texture& GBufferPass::GetDefaultTexture(ShaderReflect::TextureDefault kind,
                                                   ShaderReflect::TextureColorSpace space) const
{
    using TD = ShaderReflect::TextureDefault;
    const RHI::Texture* t = &m_defaultWhite;
    switch (kind)
    {
    case TD::Black:  t = &m_defaultBlack; break;
    case TD::Grey:   t = space == ShaderReflect::TextureColorSpace::SRGB ? &m_defaultGreySRGB : &m_defaultGrey; break;
    case TD::Normal: t = &m_defaultFlatNormal; break;
    case TD::White:  break;
    }
    return t->IsValid() ? *t : m_defaultWhite;
}

void GBufferPass::Init(IGraphicsDevice& gfx)
{
    // Only the OWNER initialises the library and registers its shaders (a borrower
    // Inits after the owner, so the registry is already populated). Register is
    // idempotent for a fixed ShaderID, but ShaderLibrary::Init is not: it rewrites
    // m_gfx / m_shaderDir outside the library's mutex. Gating both keeps a borrower
    // from writing shared state at all.
    if (OwnsShaderLibrary())
    {
        m_ownedLib.Init(gfx, "shaders/");
        m_ownedLib.Register(ShaderID::GBuffer_VS, RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
        m_ownedLib.Register(ShaderID::GBuffer_PS, RHI::ShaderStage::PS, "GBuffer.ps.hlsl");
        // Meshlet path. Registering is metadata only - nothing compiles until a
        // capable instance asks for the mesh PSO.
        m_ownedLib.Register(ShaderID::SceneMeshlet_AS, RHI::ShaderStage::AS, "SceneMeshlet.as.hlsl");
        m_ownedLib.Register(ShaderID::SceneMeshlet_MS, RHI::ShaderStage::MS, "SceneMeshlet.ms.hlsl");
        m_ownedLib.Register(ShaderID::MeshletDebug_PS, RHI::ShaderStage::PS, "MeshletDebug.ps.hlsl");
    }
    m_psoCache.Init(gfx, *m_lib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc({})))
        LOG_ERROR("GBufferPass: default PSO creation failed");
    else
        LOG_INFO("GBufferPass: default PSO ready");
    if (m_meshletCapable)
    {
        PrewarmMeshletPSO();
        CreateMeshletStatsBuffers(gfx);
    }
#if WITH_HARNESS
    m_forceMeshletGroupCpu = Sys::CommandLine::Get().Has("-meshletgroupcpu");
#endif

    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t white = 0xFFFFFFFFu;
        RHI::SubresourceData init{ &white, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultWhite, &init))
        {
            m_defaultWhiteGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultWhite);
            if (!m_defaultWhiteGpuHandle)
                LOG_ERROR("GBufferPass: GetTextureSRVGpuHandle returned 0 for default white");
        }
        else
            LOG_ERROR("GBufferPass: failed to create default white texture");
    }

    {
        RHI::TextureDesc desc;
        desc.format     = RHI::Format::R8G8B8A8_UNORM;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint32_t flatNormal = 0xFFFF8080u;
        RHI::SubresourceData init{ &flatNormal, 4, 4 };
        if (gfx.CreateTexture(desc, m_defaultFlatNormal, &init))
        {
            m_defaultFlatNormalGpuHandle = gfx.GetTextureSRVGpuHandle(m_defaultFlatNormal);
            if (!m_defaultFlatNormalGpuHandle)
                LOG_ERROR("GBufferPass: GetTextureSRVGpuHandle returned 0 for flat-normal");
        }
        else
            LOG_ERROR("GBufferPass: failed to create flat-normal texture");
    }

    if (OwnsShaderLibrary())
    {
        // The typed fallbacks of an unassigned custom-material slot
        // (GetDefaultTexture). Only the library's OWNER - the main renderer's instance,
        // the one Renderer::PackCustomMaterialBindings asks - makes them: a borrower (each
        // preview graph set builds one) would add three texture-pool slots per build that
        // nothing samples (preview_teardown pins the pool size). A borrower's
        // GetDefaultTexture answers white. R8G8B8A8 little-endian: 0xAABBGGRR.
        auto make1x1 = [&](RHI::Format format, uint32_t rgba, RHI::Texture& out, const char* what) {
            RHI::TextureDesc desc;
            desc.format     = format;
            desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            RHI::SubresourceData init{ &rgba, 4, 4 };
            if (!gfx.CreateTexture(desc, out, &init))
                LOG_ERROR("GBufferPass: failed to create the default %s texture", what);
        };
        make1x1(RHI::Format::R8G8B8A8_UNORM,      0xFF000000u, m_defaultBlack,    "black");
        make1x1(RHI::Format::R8G8B8A8_UNORM,      0xFF808080u, m_defaultGrey,     "grey");
        make1x1(RHI::Format::R8G8B8A8_UNORM_SRGB, 0xFF808080u, m_defaultGreySRGB, "sRGB grey");
    }

    {
        // The material sampler, described by MaterialSampler and shared with the other
        // material passes. Anisotropic (the player setting: default 8, 1 = trilinear, up
        // to 16) to keep the distant grazing-angle detail where sub-pixel geometry
        // (fences, roof tiles, foliage) lives; a negative mip bias gives TAA sharper
        // detail to integrate. The bias is MaterialSampler::kNativeBias until the
        // renderer reports an upscale ratio (SetUpscaleMipBias).
        if (!gfx.CreateSampler(MaterialSampler::Desc(m_textureAnisotropy, m_mipBias), m_linearSamplerIdx))
            LOG_ERROR("GBufferPass: failed to create linear sampler");
    }
}

void GBufferPass::SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW)
{
    MaterialSampler::Rebuild(gfx, m_textureAnisotropy,
                             MaterialSampler::Bias(MaterialSampler::kNativeBias, renderW, displayW),
                             m_linearSamplerIdx, m_mipBias);
}

void GBufferPass::WarmPipelines(const DrawPacket& dp)
{
    // RecordChunk's skips, then its vertex-path PSO.
    if (dp.filter != DrawFilter::Opaque || !dp.renderInMainPass) return;
    if ((dp.viewMask & ViewBit::MainCamera) == 0) return;
    (void)m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, dp.customPSID, dp.customVSID, dp.mirrored != 0));
}

PSODesc GBufferPass::BuildPSODesc(PermutationKey perm, uint32_t customPSID, uint32_t customVSID,
                                  bool mirrored) const
{
    PSODesc desc;
    // Custom shaders go through the same PSO key: PSOCache hashes vsID / psID as
    // uint32_t, so dynamic ids (> ShaderID::Count) coexist with the static enum
    // values. customVSID == 0 keeps GBuffer_VS.
    desc.vsID        = (customVSID != 0)
                        ? static_cast<ShaderID>(customVSID)
                        : ShaderID::GBuffer_VS;
    desc.psID        = (customPSID != 0)
                        ? static_cast<ShaderID>(customPSID)
                        : ShaderID::GBuffer_PS;
    desc.perm        = perm;
    desc.perm.Set(PermutationKey::DEBUG_SHADE, m_debugShade);
    desc.inputLayout = InputLayoutType::None;

    // Log once that a custom id reached the PSO desc: the preview's request line
    // only reports what the game half resolved, and an id dropped here just runs the
    // engine VS with a plausible picture. The pass name is logged because several
    // instances share this function: the main renderer's and one per preview graph
    // set (live / thumbnail / world-cull).
    if ((customVSID != 0 || customPSID != 0) &&
        !m_customPsoLogged.exchange(true, std::memory_order_relaxed))
        // A dynamic id prints as its shader's file name, a static one as its number.
        LOG_INFO("[GBUFFER] custom PSO: pass=%s vsID=%s psID=%s customVS=%s "
                 "customPS=%s", m_name.c_str(),
                 DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.vsID)).c_str(),
                 DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.psID)).c_str(),
                 DynamicShaderRegistry::Get().Label(customVSID).c_str(), DynamicShaderRegistry::Get().Label(customPSID).c_str());

    desc.rs.cullMode         = perm.Has(PermutationKey::DOUBLE_SIDED)
                                ? RHI::CullMode::NONE
                                : RHI::CullMode::BACK;
    // Mirrored (negative-determinant) instances rasterize with reversed winding, so
    // flip the front-face CONVENTION and let BACK keep culling the back face. Flipping
    // the cull mode instead would leave SV_IsFrontFace inverted and break the
    // DOUBLE_SIDED normal/bitangent flip in custom_surface.hlsli. Not a PermutationKey
    // bit: perm bits become DXC defines in the .ishdr cache name, so a bit no shader
    // reads would recompile every variant into identical DXIL. PSODesc::Hash mixes
    // rs.frontCounterClockwise, so a mirrored draw gets its own PSO.
    desc.rs.frontCounterClockwise = mirrored;
    // Global view-mode wireframe (PSOCache hashes rs.fillMode -> distinct PSO).
    desc.rs.fillMode         = m_wireframe ? RHI::FillMode::WIREFRAME
                                            : RHI::FillMode::SOLID;
    desc.rs.depthClipEnable = true;
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z
    // Stencil: each draw writes its shading code (ref set per draw via SetStencilRef),
    // the code's bits only (RenderGraph/StencilBits.h).
    desc.dss.stencilEnable    = true;
    desc.dss.stencilWriteMask = ShadingStencil::kCodeMask;
    desc.dss.stencilReadMask  = 0xFF;
    desc.dss.frontFace.stencilPassOp = RHI::StencilOp::REPLACE;
    desc.dss.frontFace.stencilFunc    = RHI::ComparisonFunc::ALWAYS;
    desc.dss.backFace = desc.dss.frontFace;

    for (int i = 0; i < 6; ++i)
        desc.bs.renderTarget[i].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R11G11B10_FLOAT;        // albedo (base colour only; emissive goes to RT5)
    desc.rtvFormats[1] = RHI::Format::R16G16B16A16_UNORM;     // normal (.a = the exact 16-bit material index)
    desc.rtvFormats[2] = RHI::Format::R8G8B8A8_UNORM;         // surface
    desc.rtvFormats[3] = RHI::Format::R16G16_FLOAT;           // velocity
    desc.rtvFormats[4] = RHI::Format::R16G16B16A16_FLOAT;     // extra (shading-model scratch: SSS thickness, clearcoat, ...)
    desc.rtvFormats[5] = RHI::Format::R16G16B16A16_FLOAT;     // HdrSceneColor (Unreal-style direct emissive write)
    desc.rtvCount      = 6;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}


// Depth-prepass PSO: the SAME vsID and rasterizer state as the matching GBuffer
// PSO, so its depth is bit-identical by construction - otherwise the GBuffer's
// depth test would reject fragments and geometry would vanish. Differences:
// - psID = kNoPixelShader, a true depth-only pipeline (an empty PS would still
//   cost one invocation per fragment, the cost this pass exists to avoid).
// - rtvCount = 0.
// - stencil disabled: the GBuffer pass writes the shading-model stencil on every
//   surviving fragment (equal depth passes GREATER_EQUAL).
// - ALPHA_TEST is never in @p perm: those draws are not depthPrepassEligible,
//   since their clip lives in the removed PS.
PSODesc GBufferPass::BuildDepthPrepassPSODesc(PermutationKey perm, uint32_t customVSID,
                                              bool mirrored) const
{
    PSODesc desc;
    desc.vsID        = (customVSID != 0) ? static_cast<ShaderID>(customVSID)
                                         : ShaderID::GBuffer_VS;
    desc.psID        = PSODesc::kNoPixelShader;
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cullMode = perm.Has(PermutationKey::DOUBLE_SIDED) ? RHI::CullMode::NONE
                                                               : RHI::CullMode::BACK;
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.fillMode         = m_wireframe ? RHI::FillMode::WIREFRAME
                                            : RHI::FillMode::SOLID;
    desc.rs.depthClipEnable = true;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;  // reversed Z
    desc.dss.stencilEnable   = false;

    desc.rtvCount  = 0;
    desc.dsvFormat = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// Meshlet path PSOs: the VS desc with the mesh stages swapped in and NOTHING else
// changed - raster (cull, mirrored front face, wireframe), depth, stencil, the six
// RTs and the PS (GBuffer_PS or the material's custom PS). A custom PS links to the
// MS exactly as to GBuffer.vs: custom_surface.hlsli's MaterialSurfaceVaryings IS
// GBuffer.vs's PSIn. vsID keeps GBuffer_VS but is unused once msID is set: PSOCache
// builds a mesh pipeline from ms / as / ps, and PSODesc::Hash masks perm by those
// stages' masks (it still mixes vsID, a constant here).
PSODesc GBufferPass::BuildMeshletPSODesc(PermutationKey perm, uint32_t customPSID, bool mirrored) const
{
    PSODesc desc = BuildPSODesc(perm, customPSID, 0, mirrored);
    desc.msID = ShaderID::SceneMeshlet_MS;
    desc.asID = ShaderID::SceneMeshlet_AS;
    return desc;
}

// The depth-only twin: BuildDepthPrepassPSODesc + the mesh stages, so a meshlet
// packet's prepass depth comes from the SAME MS as its GBuffer draw - the
// identity the prepass's GREATER_EQUAL safety argument rests on.
PSODesc GBufferPass::BuildMeshletDepthPrepassPSODesc(PermutationKey perm, bool mirrored) const
{
    PSODesc desc = BuildDepthPrepassPSODesc(perm, 0, mirrored);
    desc.msID = ShaderID::SceneMeshlet_MS;
    desc.asID = ShaderID::SceneMeshlet_AS;
    return desc;
}

bool GBufferPass::PrewarmMeshletPSO()
{
    if (!m_meshletCapable) { m_meshletPsoReady.store(false, std::memory_order_release); return false; }
    const auto t0 = std::chrono::steady_clock::now();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildMeshletPSODesc({}, 0, false));
    const bool ok = pso && pso->IsValid();
    m_meshletPsoReady.store(ok, std::memory_order_release);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // The cold cost of the mesh PSO (compile AS + MS + the PSO itself).
    if (ok) LOG_INFO("[MESHLET] pso ready pass=%s ms=%.1f", m_name.c_str(), ms);
    else    LOG_WARNING("[MESHLET] pso failed pass=%s ms=%.1f - the meshlet path stays off", m_name.c_str(), ms);
    return ok;
}

// The path half of eligibility. Opaque only (the caller walks the Opaque list),
// no custom VS (its vertices are not the library's), no dissolve in the batch (its
// clip is per instance, as the depth prepass treats it), no billboard (it
// re-orients in the VS) and no skinning. A custom PS is fine. The geometric half
// was decided at gather: meshletBlocks != 0.
bool GBufferPass::MeshletEligible(const DrawPacket& dp)
{
    return dp.meshletBlocks != 0 &&
           dp.filter == DrawFilter::Opaque &&
           dp.customVSID == 0 &&
           dp.batchHasDissolve == 0 &&
           !dp.permutation.Has(PermutationKey::BILLBOARD) &&
           dp.prevPosElementBase == 0xFFFFFFFFu;        // skinned draws carry a prev-pos base
}

const RHI::PipelineState* GBufferPass::MeshletPSOFor(const DrawPacket& dp, bool depthPrepass)
{
    if (!m_meshletFrame || !MeshletEligible(dp)) return nullptr;
    if (!MeshletDispatchFits(dp.meshletBlocks, dp.instanceCount))
    {
        if (!depthPrepass) m_mlLimitFallback.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return MeshletPso(dp.permutation, dp.customPSID, dp.mirrored != 0, depthPrepass);
}

const RHI::PipelineState* GBufferPass::MeshletPso(PermutationKey perm, uint32_t customPSID, bool mirrored,
                                                  bool depthPrepass)
{
    // The Meshlets view mode swaps every opaque PS for MeshletDebug.ps.
    const uint32_t ps = m_meshletDebugView ? static_cast<uint32_t>(ShaderID::MeshletDebug_PS) : customPSID;
    if (customPSID != 0 && !m_meshletDebugView && MeshletPsBlocked(customPSID))
    {
        if (!depthPrepass) m_mlPsoFallback.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const PSODesc desc = depthPrepass ? BuildMeshletDepthPrepassPSODesc(perm, mirrored)
                                      : BuildMeshletPSODesc(perm, ps, mirrored);
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if ((!pso || !pso->IsValid()) && m_psoCache.IsPending(desc))
    {
        // Built in the background (PSOCache::SetDeferMisses): the VS path draws meanwhile, and nothing
        // is blocked or latched off.
        if (!depthPrepass) m_mlPsoFallback.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if ((!pso || !pso->IsValid()) && customPSID != 0 && !m_meshletDebugView && !depthPrepass)
    {
        // ONE material's PS would not link to the MS (its input signature is
        // not MaterialSurfaceVaryings' - a prefix of it always links). That PS
        // keeps the VS path for the rest of the run; the path stays on.
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(m_meshletBlockedPsMutex);
            first = m_meshletBlockedPs.insert(customPSID).second;
        }
        if (first)
            LOG_WARNING("[MESHLET] custom PS %u does not link to the meshlet MS; its draws keep the VS path",
                        customPSID);
        m_mlPsoFallback.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (!pso || !pso->IsValid())
    {
        // A mesh PSO that fails at record time turns the path off from the next frame
        // (the latch reads MeshletPsoReady); this draw goes through the VS so nothing
        // disappears meanwhile.
        ENGINE_CHECK_ONCE(false, "GBufferPass: meshlet PSO creation failed at record time "
                          "(perm=0x%08X mirrored=%u prepass=%d); the meshlet path is off from the next frame",
                          perm.bits, mirrored ? 1u : 0u, depthPrepass ? 1 : 0);
        m_meshletPsoReady.store(false, std::memory_order_release);
        if (!depthPrepass) m_mlPsoFallback.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return pso;
}

bool GBufferPass::AnyMeshletGroup() const
{
    for (const IndirectGroup& g : m_indirectGroups)
        if (g.meshlet && g.cmdCount != 0) return true;
    return false;
}

void GBufferPass::DrawMeshletGroupOnCpu(RHI::CommandList cl, const IndirectGroup& g, bool depthPrepass,
                                        uint64_t bindlessHdl)
{
    const std::vector<DrawPacket>* all = cl.GetContext().GetAllDrawPackets();
    if (!all) return;
    const uint32_t vsPathPS = m_meshletDebugView ? static_cast<uint32_t>(ShaderID::MeshletDebug_PS) : g.customPSID;
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
        depthPrepass ? BuildDepthPrepassPSODesc(g.perm, g.customVSID, g.mirrored != 0)
                     : BuildPSODesc(g.perm, vsPathPS, g.customVSID, g.mirrored != 0));
    if (!pso || !pso->IsValid()) return;
    cl.SetPipelineState(*pso);
    BindGlobals(cl, bindlessHdl);
    if (!depthPrepass)
    {
        cl.gfx->SetStencilRef(g.stencilRef, cl);
        if (g.customCbvVA)    cl.gfx->BindCustomMaterialCBV(g.customCbvVA, cl);
        if (g.customTexTable) cl.gfx->BindCustomMaterialTextureTable(g.customTexTable, cl);
    }
    // The group's commands are the packets at the same positions (the arg
    // stream is packet-parallel); the per-view remap's LOWER half holds the
    // pristine mapping, so instanceOffset draws every instance.
    const uint32_t base = g.argOffset / static_cast<uint32_t>(sizeof(IndirectDrawCommand));
    for (uint32_t c = 0; c < g.cmdCount && size_t(base) + c < all->size(); ++c)
    {
        const DrawPacket& dp = (*all)[base + c];
        if (!dp.renderInMainPass) continue;
        if ((dp.viewMask & ViewBit::MainCamera) == 0) continue;
        if (!depthPrepass)
        {
            const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
            const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
            const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
            if (bc) cl.BindDescriptorTableHandle(S::SRV_t3, bc);
            if (sm) cl.BindDescriptorTableHandle(S::SRV_t4, sm);
            if (nm) cl.BindDescriptorTableHandle(S::SRV_t5, nm);
        }
        uint32_t constants[4] = {
            dp.meshDescriptorIndex,
            dp.instanceOffset,
            dp.materialIndex,
            m_meshletDebugView ? 0u : dp.prevPosElementBase
        };
        cl.gfx->SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }
    if (!depthPrepass) m_mlGroupFallback.fetch_add(1, std::memory_order_relaxed);
}

void GBufferPass::ResetMeshletCounters()
{
    m_mlPackets.store(0, std::memory_order_relaxed);
    m_mlInstances.store(0, std::memory_order_relaxed);
    m_mlAsGroups.store(0, std::memory_order_relaxed);
    m_mlLimitFallback.store(0, std::memory_order_relaxed);
    m_mlPsoFallback.store(0, std::memory_order_relaxed);
    m_mlPrepassPackets.store(0, std::memory_order_relaxed);
    m_mlIndirectGroups.store(0, std::memory_order_relaxed);
    m_mlIndirectCmds.store(0, std::memory_order_relaxed);
    m_mlGroupFallback.store(0, std::memory_order_relaxed);
}

void GBufferPass::SnapshotMeshletCounters(bool indirect)
{
    MeshletFrameStats s;
    s.active         = m_meshletFrame;
    s.indirect       = indirect;
    s.packets        = m_mlPackets.load(std::memory_order_relaxed);
    s.instances      = m_mlInstances.load(std::memory_order_relaxed);
    s.asGroups       = m_mlAsGroups.load(std::memory_order_relaxed);
    s.limitFallback  = m_mlLimitFallback.load(std::memory_order_relaxed);
    s.psoFallback    = m_mlPsoFallback.load(std::memory_order_relaxed);
    s.prepassPackets = m_mlPrepassPackets.load(std::memory_order_relaxed);
    s.indirectGroups = m_mlIndirectGroups.load(std::memory_order_relaxed);
    s.indirectCmds   = m_mlIndirectCmds.load(std::memory_order_relaxed);
    s.groupFallback  = m_mlGroupFallback.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_meshletStatsMutex);
    m_meshletStatsLast = s;
}

// Meshlet statistics: the amplification shader sums its per-group counters into
// m_mlStatsBuffer (kMeshletStatsCount u32s, a RWStructuredBuffer via
// MeshletCullCB.statsUavIdx). Zeroed at the top of the primary CL, copied to this
// frame slot's readback at the end of the LAST chunk CL (CLs run in chunk order),
// harvested when the slot comes round again.
// STATE: a buffer decays to COMMON at the end of every ExecuteCommandLists, and
// whether the chunk CLs share one with the primary is the graph's business, so
// nothing assumes a state across CLs: the UAV clear promotes COMMON -> UAV
// implicitly and a UAV barrier orders it before the AS's adds; a chunk CL after a
// decay promotes on the first AS write; the last chunk's UAV -> COPY_SRC barrier
// is valid either way.
void GBufferPass::CreateMeshletStatsBuffers(IGraphicsDevice& gfx)
{
    const uint64_t bytes = uint64_t(MeshletShared::kMeshletStatsCount) * sizeof(uint32_t);
    RHI::GPUBufferDesc d{};
    d.size       = bytes;
    d.stride     = sizeof(uint32_t);
    d.usage      = RHI::Usage::DEFAULT;
    d.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    d.miscFlags = RHI::ResourceMiscFlag::UAV_CLEARABLE;   // BeginMeshletStats' ClearUavUint
    bool ok = gfx.CreateBuffer(d, m_mlStatsBuffer);
    RHI::GPUBufferDesc r = d;
    r.usage      = RHI::Usage::READBACK;
    r.bindFlags = RHI::BindFlag::NONE;
    r.miscFlags = RHI::ResourceMiscFlag::NONE;
    for (uint32_t i = 0; ok && i < RHI::kFramesInFlight; ++i)
        ok = gfx.CreateBuffer(r, m_mlStatsReadback[i]);
    if (!ok)
    {
        LOG_WARNING("[MESHLET] statistics buffers unavailable - the meshlet counters will read as absent");
        DestroyMeshletStatsBuffers(gfx);
    }
}

void GBufferPass::DestroyMeshletStatsBuffers(IGraphicsDevice& gfx)
{
    if (m_mlStatsBuffer.IsValid()) gfx.DestroyBuffer(m_mlStatsBuffer);
    for (auto& b : m_mlStatsReadback)
        if (b.IsValid()) gfx.DestroyBuffer(b);
    m_mlStatsBuffer = {};
    for (auto& b : m_mlStatsReadback) b = {};
    for (auto& p : m_mlStatsPending) p.store(false, std::memory_order_relaxed);
}

uint32_t GBufferPass::MeshletStatsUavIndex(IGraphicsDevice& gfx) const
{
    if (!m_mlStatsBuffer.IsValid()) return MeshletShared::kMeshletNoData;
    const uint64_t h = gfx.GetBufferUAVGpuHandle(m_mlStatsBuffer);
    const uint32_t idx = h ? gfx.DescriptorHeapIndexFromGpuHandle(h) : ~0u;
    return idx == ~0u ? MeshletShared::kMeshletNoData : idx;
}

// Render half, before this frame records into its slot: the slot's previous
// copy is complete (the frame ring waited for it), so read it now.
void GBufferPass::HarvestMeshletStats(IGraphicsDevice& gfx)
{
    const uint32_t slot = gfx.GetFrameIndex() % RHI::kFramesInFlight;
    if (!m_mlStatsPending[slot].exchange(false, std::memory_order_acq_rel)) return;
    if (!m_mlStatsReadback[slot].IsValid()) return;
    MeshletGpuStats st;
    if (const uint32_t* p = static_cast<const uint32_t*>(gfx.MapBuffer(m_mlStatsReadback[slot])))
    {
        std::memcpy(st.counters, p, sizeof(st.counters));
        gfx.UnmapBuffer(m_mlStatsReadback[slot]);
        st.valid = true;
        st.frame = m_mlStatsFrame[slot];
        std::lock_guard<std::mutex> lk(m_meshletStatsMutex);
        m_mlGpuStatsLast = st;
    }
}

void GBufferPass::BeginMeshletStats(RHI::CommandList cl)
{
    m_mlStatsArmed = false;
    if (!m_meshletFrame || !m_mlStatsBuffer.IsValid()) return;
    static constexpr uint32_t kZero[4] = {};
    cl.gfx->ClearUavUint(m_mlStatsBuffer, kZero, cl);                   // COMMON -> UNORDERED_ACCESS (implicit)
    cl.gfx->PushBarrier(RHI::GPUBarrier::Memory(&m_mlStatsBuffer), cl);   // the clear before the AS's adds
    m_mlStatsArmed = true;
}

void GBufferPass::EndMeshletStats(RHI::CommandList cl)
{
    if (!m_mlStatsArmed) return;
    m_mlStatsArmed = false;
    const uint32_t slot = cl.gfx->GetFrameIndex() % RHI::kFramesInFlight;
    if (!m_mlStatsReadback[slot].IsValid()) return;
    const uint64_t bytes = uint64_t(MeshletShared::kMeshletStatsCount) * sizeof(uint32_t);
    cl.gfx->PushBarrier(RHI::GPUBarrier::Buffer(&m_mlStatsBuffer, RHI::ResourceState::UNORDERED_ACCESS,
                                                RHI::ResourceState::COPY_SRC), cl);
    cl.gfx->CopyBufferRegion(m_mlStatsBuffer, 0, m_mlStatsReadback[slot], 0, bytes, cl);
    m_mlStatsFrame[slot] = m_meshletFrameNumber;
    m_mlStatsPending[slot].store(true, std::memory_order_release);
}

// The 4th root constant of a meshlet draw (the AS / MeshletDebug.ps read it).
uint32_t GBufferPass::MeshletDrawFlags(const DrawPacket& dp)
{
    uint32_t f = MeshletShared::kMeshletDrawMeshlet;
    // The rasterizer culls nothing on a DOUBLE_SIDED draw, so the cone must not.
    if (dp.permutation.Has(PermutationKey::DOUBLE_SIDED)) f |= MeshletShared::kMeshletDrawNoCone;
    return f;
}

// Depth-only pre-population. Runs after the GBuffer clears and before the main
// loop, writing ONLY into the depth target already bound.
RHI::CommandList GBufferPass::ExecuteDepthPrepass(RHI::CommandList cl, const DrawList& draws)
{
    m_depthPrepassDraws = 0;
    if (!m_depthPrepassEnabled) return cl;

    // Depth target only - no colour attachments for this sequence.
    cl.SetRenderTargetsAndHdr({}, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t bindlessHdl = cl.GetBindlessTableHandle();

    // ---- GPU-driven path ----
    // Must mirror the main loop's path: a per-draw CPU prepass under an
    // ExecuteIndirect GBuffer brings back the thousands of CPU draw calls the
    // GPU-driven path removes (a net CPU loss for no GPU gain). Eligibility is part of
    // the group key, so a group is all-or-nothing and needs no per-command filter.
    if (AllowGBufferIndirect() && m_indirectArgBuffer && !m_indirectGroups.empty())
    {
        // Same single-slot memo as the main loop below - the depth prepass key drops
        // customPSID, so consecutive groups collide even more often.
        uint32_t dpPerm = 0xFFFFFFFFu, dpVS = 0xFFFFFFFFu;
        int      dpMirrored = -1;
        const RHI::PipelineState* dpPSO = nullptr;

        for (size_t gi = 0; gi < m_indirectGroups.size(); ++gi)
        {
            const auto& group = m_indirectGroups[gi];
            if (group.cmdCount == 0)           continue;
            if (!group.depthPrepassEligible)   continue;

            // A meshlet group: its working args are DISPATCH_MESH-shaped - the same mesh
            // stages as its GBuffer draw, so the prepass depth is the GBuffer depth.
            if (group.meshlet)
            {
                const RHI::PipelineState* mp = m_forceMeshletGroupCpu ? nullptr :
                    MeshletPso(group.perm, group.customPSID, group.mirrored != 0, /*depthPrepass=*/true);
                if (!mp) { DrawMeshletGroupOnCpu(cl, group, /*depthPrepass=*/true, bindlessHdl); continue; }
                cl.SetPipelineState(*mp);
                BindGlobals(cl, bindlessHdl);
                cl.BindCB(SR::CB_b4, RG::RGSlot::MeshletCull);
                cl.gfx->ExecuteIndirectDispatchMesh(
                    *m_indirectArgBuffer,
                    group.argOffset,
                    group.cmdCount,
                    m_indirectCountBuffer,
                    gi * sizeof(uint32_t),
                    cl);
                m_depthPrepassDraws += group.cmdCount;
                m_mlPrepassPackets.fetch_add(group.cmdCount, std::memory_order_relaxed);
                continue;
            }

            const RHI::PipelineState* pso;
            if (dpPSO && group.perm.bits == dpPerm && group.customVSID == dpVS &&
                (group.mirrored != 0) == (dpMirrored != 0))
            {
                pso = dpPSO;
            }
            else
            {
                pso = m_psoCache.GetOrCreate(
                    BuildDepthPrepassPSODesc(group.perm, group.customVSID, group.mirrored != 0));
                if (pso && pso->IsValid())
                {
                    dpPerm = group.perm.bits; dpVS = group.customVSID;
                    dpMirrored = group.mirrored != 0; dpPSO = pso;
                }
            }
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            BindGlobals(cl, bindlessHdl);
            // No stencil ref, no material CBV/texture table: nothing but the VS
            // runs, and the GBuffer pass still writes stencil on every surviving
            // fragment.
            cl.gfx->ExecuteIndirectDraw(
                *m_indirectArgBuffer,
                group.argOffset,
                group.cmdCount,
                m_indirectCountBuffer,
                gi * sizeof(uint32_t),
                cl);
            m_depthPrepassDraws += group.cmdCount;
        }
        return cl;
    }

    // ---- CPU fallback (no indirect args this frame) ----
    const RHI::PipelineState* activePSO = nullptr;

    for (size_t di = 0; di < draws.size(); ++di)
    {
        const DrawPacket& dp = draws[di];
        if (!dp.depthPrepassEligible) continue;   // anything that can discard
        if (!dp.renderInMainPass)     continue;
        if ((dp.viewMask & ViewBit::MainCamera) == 0) continue;

        // Meshlet packets pre-pass through the SAME mesh stages their GBuffer
        // draw uses (BuildMeshletDepthPrepassPSODesc) - a VS-made prepass depth
        // under an MS-made GBuffer depth would put the GREATER_EQUAL identity
        // at the mercy of two compilers.
        const RHI::PipelineState* meshPso = MeshletPSOFor(dp, /*depthPrepass=*/true);
        const RHI::PipelineState* pso = meshPso ? meshPso : m_psoCache.GetOrCreate(
            BuildDepthPrepassPSODesc(dp.permutation, dp.customVSID, dp.mirrored != 0));
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            BindGlobals(cl, bindlessHdl);
            if (meshPso) cl.BindCB(SR::CB_b4, RG::RGSlot::MeshletCull);
        }

        // No material textures, no stencil ref, no custom CBV/SRV: nothing but the VS
        // runs. The PVF root constants ARE required - the vertex shader fetches
        // positions through them.
        {
            uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                meshPso ? MeshletDrawFlags(dp) : dp.prevPosElementBase
            };
            cl.gfx->SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
        }
        if (meshPso)
        {
            cl.DispatchMesh(dp.meshletBlocks, dp.instanceCount, 1);
            m_mlPrepassPackets.fetch_add(1, std::memory_order_relaxed);
        }
        else
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        ++m_depthPrepassDraws;
    }
    return cl;
}

void GBufferPass::BindGlobals(RHI::CommandList cl, uint64_t bindlessHandle) const
{
    cl.BindDescriptorHeaps();
    cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
    cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
    if (bindlessHandle)
        cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);
    cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);
    // CB slot 1 (b2 space0) = LightCB. The engine default GBuffer.ps does not read
    // b2, but custom GBuffer surface hooks (e.g. NPR characters: matcap view dir,
    // face-SDF sun direction, view-dependent parallax) need light/camera context in
    // the geometry pass. b2 is VISIBILITY_ALL and otherwise unbound here; binding it
    // is additive (the default path's DXC-stripped b2 read pays nothing).
    cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);

    const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer);
    if (matBuf && matBuf->IsValid())
        cl.BindBufferSRV(SR::SRV_t2, *matBuf);

    if (m_linearSamplerIdx >= 0)
        cl.SetDynamicSampler(m_linearSamplerIdx);   // the material sampler (sampler_heap.hlsli SmpDynamic)

    if (m_defaultWhiteGpuHandle)
    {
        cl.BindDescriptorTableHandle(S::SRV_t3, m_defaultWhiteGpuHandle);
        cl.BindDescriptorTableHandle(S::SRV_t4, m_defaultWhiteGpuHandle);
    }
    if (m_defaultFlatNormalGpuHandle)
        cl.BindDescriptorTableHandle(S::SRV_t5, m_defaultFlatNormalGpuHandle);

    // Bind bindless texture table (all loaded textures indexed by handleId).
    if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
        cl.BindDescriptorTableHandle(S::BindlessTex, texTable); // t0 space2
}

// Records draws[begin..end) onto @p cl (the threading contract is at the declaration).
void GBufferPass::RecordChunk(RHI::CommandList cl, DrawList draws, size_t begin, size_t end, bool lastChunk)
{
    // Every CL binds its render targets even though the primary CL cleared them -
    // D3D12 does not inherit RT state across CLs. SetRenderTargetsAndHdr appends the
    // 6th RT (HdrSceneColor).
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const RHI::PipelineState* activePSO   = nullptr;
    const uint64_t            bindlessHdl = cl.GetBindlessTableHandle();

    for (size_t di = begin; di < end; ++di)
    {
        const DrawPacket& dp = draws[di];

        // The main pass honours VisibilityComponent.RenderInMainPass and viewMask. A
        // shadow-only proxy (e.g. an invisible character) stays in the Opaque list (same
        // sort / batch path) but bails out here before any state binding.
        if (!dp.renderInMainPass) continue;
        if ((dp.viewMask & ViewBit::MainCamera) == 0) continue;

        // Meshlet path: an eligible packet swaps GBuffer.vs for the AS/MS pair;
        // everything below (textures, stencil, root constants) is shared, only the draw
        // call differs.
        const RHI::PipelineState* meshPso = MeshletPSOFor(dp, /*depthPrepass=*/false);
        // The Meshlets view mode draws the VS packets with MeshletDebug.ps too
        // (flat grey: their 4th root constant says "not a meshlet draw").
        const uint32_t vsPathPS = m_meshletDebugView ? static_cast<uint32_t>(ShaderID::MeshletDebug_PS)
                                                     : dp.customPSID;
        const RHI::PipelineState* pso = meshPso ? meshPso :
            m_psoCache.GetOrCreate(BuildPSODesc(dp.permutation, vsPathPS, dp.customVSID,
                                                dp.mirrored != 0));
        if (!pso || !pso->IsValid()) continue;

        if (pso != activePSO)
        {
            cl.SetPipelineState(*pso);
            activePSO = pso;
            BindGlobals(cl, bindlessHdl);
            // MeshletCullCB (b4): the sidecar index and the cull / LOD state the
            // AS reads. Bound with the mesh PSO; a VS PSO never reads b4.
            if (meshPso) cl.BindCB(SR::CB_b4, RG::RGSlot::MeshletCull);
        }

        const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor  : m_defaultWhiteGpuHandle;
        const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap : m_defaultWhiteGpuHandle;
        const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap  : m_defaultFlatNormalGpuHandle;
        if (bc) cl.BindDescriptorTableHandle(S::SRV_t3, bc);
        if (sm) cl.BindDescriptorTableHandle(S::SRV_t4, sm);
        if (nm) cl.BindDescriptorTableHandle(S::SRV_t5, nm);

        // Per-draw stencil ref: the shading code (ShadingStencil: 1 PBR, 2 NPR, 3 Unlit),
        // written by the stencil REPLACE op.
        cl.gfx->SetStencilRef(dp.stencilRef, cl);

        // Bind the per-material custom CBV (b8 space0) when the material ran a packed
        // custom shader this frame. Zero = the standard GBuffer PS, or a custom PS with
        // no cbuffer.
        if (dp.customCbvVA)
            cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl);
        // Bind the per-material custom texture table (t0-t7 space3).
        if (dp.customTexTable)
            cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl);

        // Write all 4 PVF root constants in one call: a 3+1 split is legal D3D12 but hit
        // a driver case where the 4th constant was lost, and one
        // SetGraphicsRoot32BitConstants costs nothing more.
        {
            // [3]: GBuffer.vs's vestigial prevPosInfo. A meshlet draw carries its
            // per-draw flags there (the AS reads the no-cone bit); in the
            // Meshlets view a VS draw carries 0 (MeshletDebug.ps: "not meshlet").
            uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                meshPso ? MeshletDrawFlags(dp) : (m_meshletDebugView ? 0u : dp.prevPosElementBase)
            };
            cl.gfx->SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
        }
        if (meshPso)
        {
            // X = cluster blocks of the entry (all levels), Y = the instances:
            // the AS reads SV_GroupID.y exactly where the VS reads SV_InstanceID.
            cl.DispatchMesh(dp.meshletBlocks, dp.instanceCount, 1);
            m_mlPackets.fetch_add(1, std::memory_order_relaxed);
            m_mlInstances.fetch_add(dp.instanceCount, std::memory_order_relaxed);
            m_mlAsGroups.fetch_add(uint64_t(dp.meshletBlocks) * dp.instanceCount,
                                   std::memory_order_relaxed);
        }
        else
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }
    // The statistics copy rides the LAST chunk: CLs execute in chunk order, so
    // every amplification group of the frame has written by then.
    if (lastChunk) EndMeshletStats(cl);
}

static bool AllowGBufferIndirect()
{
#if WITH_HARNESS
    static const bool s_direct = Sys::CommandLine::Get().Has("-gbufferdirectverify");
    return !s_direct;
#else
    return true;
#endif
}

uint32_t GBufferPass::PrepareRecording(RHI::CommandList cl)
{
    ResetMeshletCounters();   // before any chunk of this frame records
    HarvestMeshletStats(*cl.gfx);
    m_recordPrepared = true;
    m_recordDraws = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    m_recordChunkCount = 1;
    m_recordCLs[0] = cl;
    if (!(AllowGBufferIndirect() && m_indirectArgBuffer && !m_indirectGroups.empty())) {
        m_recordChunkCount = (std::max)(1u, (std::min)(static_cast<uint32_t>(kMaxChunks),
            (std::min)(TaskSystem::Get().GetWorkerCount(),
                static_cast<uint32_t>(m_recordDraws.size() / kMinDrawsPerChunk))));
    }
    for (uint32_t i = 1; i < m_recordChunkCount; ++i) {
        auto extra = cl.gfx->BeginCommandList(RHI::QueueType::GRAPHICS);
        cl.gfx->SetCommandListName(extra, "GBuffer.Chunk");
        extra.gfx = cl.gfx; extra.ctx = cl.ctx;
        cl.gfx->AddCommandListDependency(extra, m_recordCLs[i - 1]);
        m_recordCLs[i] = extra;
    }
    if (Sys::CommandLine::Get().Has("-materialcpuverify"))
        LOG_INFO("[GBUFFER_RECORD_JOBS] frame=%llu draws=%zu chunks=%u direct=%u",
            static_cast<unsigned long long>(Sys::ExecutionDomain::CurrentGeneration()), m_recordDraws.size(),
            m_recordChunkCount, !(AllowGBufferIndirect() && m_indirectArgBuffer && !m_indirectGroups.empty()));
    return m_recordChunkCount - 1;
}

void GBufferPass::ExecuteAdditionalRecording(uint32_t task)
{
    ZoneScopedN("GBuffer.Chunk.Job");
    const size_t chunk = task + 1;
    const size_t begin = m_recordDraws.size() * chunk / m_recordChunkCount;
    const size_t end = m_recordDraws.size() * (chunk + 1) / m_recordChunkCount;
    RecordChunk(m_recordCLs[chunk], m_recordDraws, begin, end, chunk + 1 == m_recordChunkCount);
}

void GBufferPass::FinishRecording()
{
    SnapshotMeshletCounters(AllowGBufferIndirect() && m_indirectArgBuffer && !m_indirectGroups.empty());
    m_recordPrepared = false;
    m_recordDraws = {};
    m_recordChunkCount = 1;
}

RHI::CommandList GBufferPass::Execute(RHI::CommandList cl)
{
    // The primary CL clears the targets and records the first slice; the other chunk
    // CLs (ExecuteAdditionalRecording) bind them and record theirs. Phase A already put
    // the HDR transition and clear into THIS primary CL (Setup's HdrSceneColor colour
    // target); SetRenderTargetsAndHdr below widens that 1-RT bind to the 6-RT set (5
    // GBuffer RTs + HDR) without clearing HDR again. A direct caller (outside
    // RenderGraph) never ran PrepareRecording, so its meshlet counters start here.
    if (!m_recordPrepared) { ResetMeshletCounters(); HarvestMeshletStats(*cl.gfx); }
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    const float black[4]    = { 0.f, 0.f, 0.f, 1.f };
    cl.ClearRenderTarget(m_albedo,   black);
    cl.ClearRenderTarget(m_normal,   black);
    cl.ClearRenderTarget(m_surface,  black);
    cl.ClearRenderTarget(m_velocity, black);
    cl.ClearRenderTarget(m_emissive, black);
    cl.ClearDepthStencil(m_depth);
    // Meshlet statistics: zeroed before any draw of this frame - on the CPU
    // per-draw path, and on the ExecuteIndirect path when it has a meshlet group.
    const bool indirectPath = AllowGBufferIndirect() && m_indirectArgBuffer && !m_indirectGroups.empty();
    if (!indirectPath || AnyMeshletGroup())
        BeginMeshletStats(cl);
    else
        m_mlStatsArmed = false;

    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    if (draws.empty() && !m_indirectArgBuffer) return cl;

    // ---- Optional depth prepass ----
    // Fills depth for the discard-free opaque draws so the 6-MRT pass below early-Z
    // rejects everything they occlude. Rebinds the full RT set after.
    if (m_depthPrepassEnabled && !draws.empty())
    {
        ExecuteDepthPrepass(cl, draws);
        cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    }

    // ---- ExecuteIndirect path (GPU-culled, per-PSO group) ----
    if (AllowGBufferIndirect() && m_indirectArgBuffer && !m_indirectGroups.empty())
    {
        const uint64_t bindlessHdl = cl.GetBindlessTableHandle();

        cl.SetViewport();
        cl.SetScissorRect();
        cl.SetPrimitiveTopology();

        // Single-slot PSO memo keyed on the RAW inputs, not on a hash. PSODesc::Hash
        // walks a ~400 B descriptor (all eight BlendState RT slots) as a serial FNV chain,
        // ~0.5 us per call before GetOrCreate even locks, and this loop is single-threaded.
        // Groups also break on material identity (customCbvVA / customTexTable), so
        // consecutive groups often share a PSO; the memo skips the hash and the lock.
        uint32_t lastPerm = 0xFFFFFFFFu, lastPS = 0xFFFFFFFFu, lastVS = 0xFFFFFFFFu;  // perm compared by .bits
        int      lastMirrored = -1;
        const RHI::PipelineState* lastPSO = nullptr;

        for (size_t gi = 0; gi < m_indirectGroups.size(); ++gi)
        {
            const auto& group = m_indirectGroups[gi];
            if (group.cmdCount == 0) continue;

            // A meshlet group: the cull CS wrote its commands as DISPATCH_MESH(blocks,
            // survivors, 1) - the mesh PSO + b4, the same material binds as a VS group, the
            // same count-buffer slot.
            if (group.meshlet)
            {
                const RHI::PipelineState* mp = m_forceMeshletGroupCpu ? nullptr :
                    MeshletPso(group.perm, group.customPSID, group.mirrored != 0, /*depthPrepass=*/false);
                if (!mp) { DrawMeshletGroupOnCpu(cl, group, /*depthPrepass=*/false, bindlessHdl); continue; }
                cl.SetPipelineState(*mp);
                BindGlobals(cl, bindlessHdl);
                cl.BindCB(SR::CB_b4, RG::RGSlot::MeshletCull);
                cl.gfx->SetStencilRef(group.stencilRef, cl);
                if (group.customCbvVA)
                    cl.gfx->BindCustomMaterialCBV(group.customCbvVA, cl);
                if (group.customTexTable)
                    cl.gfx->BindCustomMaterialTextureTable(group.customTexTable, cl);
                cl.gfx->ExecuteIndirectDispatchMesh(
                    *m_indirectArgBuffer,
                    group.argOffset,
                    group.cmdCount,
                    m_indirectCountBuffer,
                    gi * sizeof(uint32_t),
                    cl);
                m_mlIndirectGroups.fetch_add(1, std::memory_order_relaxed);
                m_mlIndirectCmds.fetch_add(group.cmdCount, std::memory_order_relaxed);
                continue;
            }

            // The Meshlets view mode draws the VS groups with MeshletDebug.ps too
            // (flat grey: their 4th root constant is no meshlet draw's).
            const uint32_t vsPathPS = m_meshletDebugView ? static_cast<uint32_t>(ShaderID::MeshletDebug_PS)
                                                         : group.customPSID;
            const RHI::PipelineState* pso;
            if (lastPSO && group.perm.bits == lastPerm && vsPathPS == lastPS &&
                group.customVSID == lastVS && (group.mirrored != 0) == (lastMirrored != 0))
            {
                pso = lastPSO;
            }
            else
            {
                pso = m_psoCache.GetOrCreate(BuildPSODesc(group.perm, vsPathPS,
                                                          group.customVSID, group.mirrored != 0));
                if (pso && pso->IsValid())
                {
                    lastPerm = group.perm.bits; lastPS = vsPathPS;
                    lastVS   = group.customVSID; lastMirrored = group.mirrored != 0;
                    lastPSO  = pso;
                }
            }
            if (!pso || !pso->IsValid()) continue;

            cl.SetPipelineState(*pso);
            BindGlobals(cl, bindlessHdl);

            cl.gfx->SetStencilRef(group.stencilRef, cl);

            // Bind the group's per-material CBV once before ExecuteIndirect: every command
            // of a group shares the material (and so the packed CBV VA).
            if (group.customCbvVA)
                cl.gfx->BindCustomMaterialCBV(group.customCbvVA, cl);
            // Same for the custom texture table.
            if (group.customTexTable)
                cl.gfx->BindCustomMaterialTextureTable(group.customTexTable, cl);

            // With a count buffer, cmdCount is the group's CAPACITY and the GPU draws the
            // compaction CS's survivor count (<= capacity) read at this group's slot. Without
            // one, cmdCount is exact and culled slots are degenerate zero-instance commands.
            cl.gfx->ExecuteIndirectDraw(
                *m_indirectArgBuffer,
                group.argOffset,
                group.cmdCount,
                m_indirectCountBuffer,
                gi * sizeof(uint32_t),
                cl);
        }
        EndMeshletStats(cl);   // every amplification group of the frame recorded above
        if (!m_recordPrepared) SnapshotMeshletCounters(true);
        return cl;
    }

    if (m_recordPrepared) {
        // All chunks are flat graph work. The graph awaits the whole batch;
        // no recording worker launches children or blocks on a latch.
        RecordChunk(cl, m_recordDraws, 0, m_recordDraws.size() / m_recordChunkCount, m_recordChunkCount == 1);
        return m_recordCLs[m_recordChunkCount - 1];
    }
    // Direct callers (outside RenderGraph) record one list. Parallel work is
    // exposed by PrepareRecording, never by a nested blocking worker dispatch.
    RecordChunk(cl, draws, 0, draws.size(), true);
    SnapshotMeshletCounters(false);
    return cl;
}
