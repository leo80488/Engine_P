#include "RenderGraph/RenderPass/SurfaceCacheBakePass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "ECS/HierarchyComponents.h"   // ViewBit
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"
#include "Diagnostics/Profiler.h"

#include <algorithm>
#include <cstring>

// Root parameter slots (RootSignatureLayout.h):
// S::  = absolute graphics root-parameter index.
// SR:: = group-relative graphics index (BindCB / BindBufferSRV).
// C::  = compute root-parameter index.
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;
namespace C  = RHI::Slots::Cmp;

namespace
{
    // Mirror of ResolveCB in SurfaceCacheResolve.cs.hlsl.
    struct alignas(16) ResolveCB
    {
        uint32_t pageOriginX, pageOriginY;
        uint32_t pageSizeX,   pageSizeY;
        uint32_t dilateRings;
        uint32_t pad[3];
    };
    static_assert(sizeof(ResolveCB) == 32, "sync with SurfaceCacheResolve.cs.hlsl");
}

// The pass declares nothing to the graph: its render targets are pass-owned
// scratch textures and its atlas barriers are emitted by hand, so the graph has
// nothing to transition. Same shape as PickingPass::Setup.
void SurfaceCacheBakePass::Setup(RG::RenderGraphBuilder& b)
{
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void SurfaceCacheBakePass::Init(IGraphicsDevice& gfx)
{
    if (!m_gbuffer)
    {
        LOG_ERROR("SurfaceCacheBakePass: no GBufferPass — bake disabled");
        return;
    }

    // Register the bake VS into GBufferPass's library (see the header for why), then
    // point our PSOCache at that library. Registration is bookkeeping only: nothing
    // compiles until the first GetOrCreate.
    ShaderLibrary& gbufLib = m_gbuffer->GetShaderLibrary();
    gbufLib.Register(ShaderID::SurfaceCacheBake_VS, RHI::ShaderStage::VS,
                     "SurfaceCacheBake.vs.hlsl");
    m_psoCache.Init(gfx, gbufLib);

    m_csLib.Init(gfx, "shaders/");
    m_csLib.Register(ShaderID::SurfaceCacheResolve_CS, RHI::ShaderStage::CS,
                     "SurfaceCacheResolve.cs.hlsl", "main");
    m_csLib.Register(ShaderID::SurfaceCacheDilate_CS, RHI::ShaderStage::CS,
                     "SurfaceCacheResolve.cs.hlsl", "CSDilate");

    // Plain linear WRAP, no mip bias, no anisotropy: deliberately not GBufferPass's
    // sampler, whose -1.0 bias feeds TAA sub-pixel detail across frames. A bake has no
    // temporal history, so a bias would only alias the page; the natural UV-space
    // derivative (1/pageSize) already picks the mip that matches the page resolution.
    RHI::SamplerDesc sd;
    sd.filter = RHI::Filter::MIN_MAG_MIP_LINEAR;
    if (!gfx.CreateSampler(sd, m_samplerIdx))
        LOG_ERROR("SurfaceCacheBakePass: sampler creation failed");
}

void SurfaceCacheBakePass::ReloadShaders(IGraphicsDevice& gfx)
{
    // GBufferPass owns (and clears) the shared library; we drop only what is ours.
    // m_resourcesReady = false sends the compute PSOs back through EnsureComputePSOs on
    // the next (main-thread) OnPrepare: keeping every PSO / texture creation on that one
    // path keeps the threading argument checkable.
    m_psoCache.Clear();
    m_csLib.ClearCaches();
    m_resolvePSO     = {};
    m_dilatePSO      = {};
    m_resourcesReady = false;
}

bool SurfaceCacheBakePass::EnsureScratch(IGraphicsDevice& gfx)
{
    if (m_scratch[RT_Albedo].IsValid()) return true;
    PROF_SCOPE("SurfaceCacheBake.CreateScratch");   // the lazy scratch MRT creation

    struct { RHI::Format fmt; const char* name; } kSpec[RT_Count] = {
        // RT0 widened to RGBA16F: the real GBuffer RT0 is R11G11B10_FLOAT with no alpha,
        // but MS_EncodeGBuffer writes float4(baseColor, opacity) and the proxy's candidate
        // alpha test needs that opacity.
        { RHI::Format::R16G16B16A16_FLOAT, "SurfaceCacheBake.Albedo"   },
        { RHI::Format::R16G16B16A16_UNORM, "SurfaceCacheBake.Normal"   },
        { RHI::Format::R8G8B8A8_UNORM,     "SurfaceCacheBake.Surface"  },
        { RHI::Format::R16G16_FLOAT,       "SurfaceCacheBake.Velocity" },
        { RHI::Format::R16G16B16A16_FLOAT, "SurfaceCacheBake.Extra"    },
        { RHI::Format::R16G16B16A16_FLOAT, "SurfaceCacheBake.Emissive" },
    };

    for (uint32_t i = 0; i < RT_Count; ++i)
    {
        RHI::TextureDesc td{};
        td.width      = MaterialSurfaceCache::kPageSize;
        td.height     = MaterialSurfaceCache::kPageSize;
        td.mipLevels = 1;
        td.format     = kSpec[i].fmt;
        // SHADER_RESOURCE on all six even though only three are resolved: an SRV
        // costs one descriptor and keeps the set uniform, which matters more than
        // three saved descriptors the first time someone adds a fourth consumer.
        td.bindFlags = RHI::BindFlag::RENDER_TARGET | RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::RENDERTARGET;
        td.clear      = RHI::ClearValue::Color(0, 0, 0, 0);
        td.debugName = kSpec[i].name;
        if (!gfx.CreateTexture(td, m_scratch[i]))
        {
            LOG_ERROR("SurfaceCacheBakePass: scratch RT %u creation failed", i);
            for (uint32_t k = 0; k < i; ++k)
            {
                gfx.DestroyTexture(m_scratch[k]);
                m_scratch[k] = {};
            }
            return false;
        }
        m_scratchState[i] = RHI::ResourceState::RENDERTARGET;
    }

    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(kCBSlotStride) * kCBSlots;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
    if (!gfx.CreateBuffer(bd, m_resolveCB))
    {
        LOG_ERROR("SurfaceCacheBakePass: resolve CB creation failed");
        return false;
    }
    m_resolveCBMapped = gfx.MapBuffer(m_resolveCB);
    if (!m_resolveCBMapped)
    {
        // Fail closed. A zeroed ResolveCB gives gPageSize == 0, so both compute entry points
        // return on their first line (every thread, every page) while the CPU still marks
        // the page baked: a silently empty atlas the proxy publishes as authoritative, far
        // worse than not having the feature.
        LOG_ERROR("SurfaceCacheBakePass: resolve CB map failed — bake disabled");
        return false;
    }
    std::memset(m_resolveCBMapped, 0, bd.size);

    LOG_INFO("SurfaceCacheBakePass: %u scratch MRTs ready (%u^2)",
             (uint32_t)RT_Count, MaterialSurfaceCache::kPageSize);
    return true;
}

bool SurfaceCacheBakePass::EnsureComputePSOs(IGraphicsDevice& gfx)
{
    if (m_resolvePSO.IsValid() && m_dilatePSO.IsValid()) return true;

    const RHI::Shader* resolveCS = m_csLib.GetShader(ShaderID::SurfaceCacheResolve_CS);
    const RHI::Shader* dilateCS  = m_csLib.GetShader(ShaderID::SurfaceCacheDilate_CS);
    if (!resolveCS || !dilateCS)
    {
        LOG_ERROR("SurfaceCacheBakePass: resolve/dilate CS compile failed");
        return false;
    }

    RHI::PipelineStateDesc d{};
    d.cs = resolveCS;
    if (!gfx.CreatePipelineState(d, m_resolvePSO))
    {
        LOG_ERROR("SurfaceCacheBakePass: resolve PSO creation failed");
        return false;
    }
    d.cs = dilateCS;
    if (!gfx.CreatePipelineState(d, m_dilatePSO))
    {
        LOG_ERROR("SurfaceCacheBakePass: dilate PSO creation failed");
        return false;
    }
    return true;
}

void SurfaceCacheBakePass::Shutdown(IGraphicsDevice& gfx)
{
    // Order matters for the CB: unmap BEFORE destroying, or the mapped pointer
    // outlives its resource. Everything here is null/invalid-tolerant so the
    // feature-off path (OnPrepare allocated nothing) is a no-op.
    if (m_resolveCBMapped)
    {
        gfx.UnmapBuffer(m_resolveCB);
        m_resolveCBMapped = nullptr;
    }
    if (m_resolveCB.IsValid())
    {
        gfx.DestroyBuffer(m_resolveCB);
        m_resolveCB = {};
    }
    for (uint32_t i = 0; i < RT_Count; ++i)
    {
        if (!m_scratch[i].IsValid()) continue;
        gfx.DestroyTexture(m_scratch[i]);
        m_scratch[i]      = {};
        m_scratchState[i] = {};
    }
    m_resolvePSO     = {};
    m_dilatePSO      = {};
    m_resourcesReady = false;
}

void SurfaceCacheBakePass::OnPrepare(const RG::PrepareContext& c)
{
    IGraphicsDevice& gfx = *c.gfx;
    m_resourcesReady = false;
    // With the feature off this is the only thing that runs, and it allocates nothing.
    if (!m_cache || !m_cache->HasPendingBakes()) return;
    if (!m_cache->AtlasReady()) return;
    if (!EnsureScratch(gfx))    return;
    if (!EnsureComputePSOs(gfx)) return;
    m_resourcesReady = true;
}

void SurfaceCacheBakePass::WarmPipelines(const DrawPacket& dp)
{
    // Execute's permutation: ALPHA_TEST and DOUBLE_SIDED out.
    PermutationKey perm = dp.permutation;
    perm.Set(PermutationKey::ALPHA_TEST,   false);
    perm.Set(PermutationKey::DOUBLE_SIDED, false);
    (void)m_psoCache.GetOrCreate(BuildBakePSODesc(perm, dp.customPSID));
}

void SurfaceCacheBakePass::WarmShaders()
{
    (void)m_csLib.GetShader(ShaderID::SurfaceCacheResolve_CS);
    (void)m_csLib.GetShader(ShaderID::SurfaceCacheDilate_CS);
}

bool SurfaceCacheBakePass::Prewarm(IGraphicsDevice& gfx)
{
    if (!m_cache || !m_cache->IsEnabled()) return false;
    return EnsureScratch(gfx) && EnsureComputePSOs(gfx);
}

PSODesc SurfaceCacheBakePass::BuildBakePSODesc(PermutationKey perm,
                                               uint32_t customPSID) const
{
    // Start from the real GBuffer desc so blend state, write masks and every future
    // field stay in lockstep. customVSID = 0: the bake VS replaces it either way (see the
    // header). mirrored = false: a mirrored world matrix flips the clip-space winding, a
    // property of the projection this bake does not use; UV winding decides coverage
    // here, and cull is NONE anyway.
    PSODesc desc = m_gbuffer->BuildPSODesc(perm, customPSID, /*customVSID=*/0,
                                           /*mirrored=*/false);

    desc.vsID = ShaderID::SurfaceCacheBake_VS;

    // Widened for opacity (see EnsureScratch).
    desc.rtvFormats[RT_Albedo] = RHI::Format::R16G16B16A16_FLOAT;

    // No depth, no stencil, no depth target. A UV-space raster has no meaningful
    // depth: two surface points that overlap in UV are the SAME texel, and
    // whichever triangle wins is arbitrary either way. Keeping a depth test would
    // just add a DSV the pass has no reason to own. dsvFormat UNKNOWN is part of
    // the PSO hash, so this cannot collide with the screen GBuffer PSO.
    desc.dss.depthEnable      = false;
    desc.dss.depthWriteMask  = RHI::DepthWriteMask::ZERO;
    desc.dss.stencilEnable    = false;
    desc.dsvFormat             = RHI::Format::UNKNOWN;

    // CullMode NONE: UV winding is unrelated to geometric winding; a mirrored or
    // flipped UV shell would be culled wholesale and leave a hole in the page.
    desc.rs.cullMode          = RHI::CullMode::NONE;
    // Never wireframe. GBufferPass::BuildPSODesc honours the global view mode;
    // a wireframe surface cache would be a debug view leaking into RT shading.
    desc.rs.fillMode          = RHI::FillMode::SOLID;

    return desc;
}

// Mirror of GBufferPass::BindGlobals, deliberately a separate copy rather than a
// call into the (private) original: this pass binds its own sampler and must not
// inherit GBufferPass's TAA mip bias, and the bindless / CB / SRV set is fixed by the
// shared root signature, not by GBufferPass.
void SurfaceCacheBakePass::BindGlobals(RHI::CommandList cl, uint64_t bindlessHandle) const
{
    cl.BindDescriptorHeaps();
    cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
    cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
    if (bindlessHandle)
        cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);
    cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);
    // b2 = LightCB. Custom GBuffer surface hooks read it (matcap view dir,
    // face-SDF sun direction), and an unbound b2 there is a GPU-validation
    // error, not a black pixel.
    cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);

    const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer);
    if (matBuf && matBuf->IsValid())
        cl.BindBufferSRV(SR::SRV_t2, *matBuf);

    if (m_samplerIdx >= 0)
        cl.SetDynamicSampler(m_samplerIdx);   // the baked materials' sampler, linear wrap

    const uint64_t white = m_gbuffer->GetDefaultWhiteSrvHandle();
    const uint64_t flat  = m_gbuffer->GetDefaultFlatNormalSrvHandle();
    if (white)
    {
        cl.BindDescriptorTableHandle(S::SRV_t3, white);
        cl.BindDescriptorTableHandle(S::SRV_t4, white);
    }
    if (flat)
        cl.BindDescriptorTableHandle(S::SRV_t5, flat);

    if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
        cl.BindDescriptorTableHandle(S::BindlessTex, texTable);   // t0 space2
}

RHI::CommandList SurfaceCacheBakePass::Execute(RHI::CommandList cl)
{
    m_lastBakeCount = 0;
    if (!m_resourcesReady || !m_cache || !m_gbuffer) return cl;
    if (!m_cache->AtlasReady()) return cl;

    // Schedule: walk the same opaque list GBufferPass just consumed and ask the cache
    // which packets own a page still waiting for texels. The cache owns the budget, so a
    // packet whose page is already baked costs one hash lookup.
    const DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    if (draws.empty()) return cl;

    struct Scheduled { uint32_t packet; uint32_t page; };
    Scheduled sched[kMaxBakesPerFrame];
    uint32_t  schedCount = 0;

    for (size_t di = 0; di < draws.size() && schedCount < kMaxBakesPerFrame; ++di)
    {
        const DrawPacket& dp = draws[di];
        if (!dp.renderInMainPass) continue;
        if ((dp.viewMask & ViewBit::MainCamera) == 0) continue;
        // The Renderer's surface-page acquisition gate refuses batches carrying a dissolve
        // but cannot bind that decision to a packet: dissolve is per-instance and not part
        // of the batch key, so a dissolving and an intact batch can share
        // (meshDescriptorIndex, materialIndex), the only thing this loop matches on. Found
        // first, the dissolving packet's MS_ApplyDissolve (unconditional, not a permutation)
        // would burn holes into a page that outlives the dissolve. Re-test here so both
        // loops agree on the packet.
        if (dp.batchHasDissolve) continue;

        const uint32_t page = m_cache->TakeBakeSlot(dp.meshDescriptorIndex, dp.materialIndex);
        if (page == MaterialSurfaceCache::kInvalidPage) continue;

        sched[schedCount++] = { static_cast<uint32_t>(di), page };
    }
    if (schedCount == 0) return cl;

    const uint64_t bindlessHdl = cl.GetBindlessTableHandle();
    const RHI::Texture& albedoAtlas  = m_cache->GetAlbedoAtlas();
    const RHI::Texture& surfaceAtlas = m_cache->GetSurfaceAtlas();

    // Atlases SR -> UAV for the whole batch. From here to the SR transition at the end
    // this list owns both atlases, whole; DDGI's list waits on it (TakeAtlasWriteList).
    m_atlasWriteCL = cl;
    const RHI::ResourceState atlasEntry = m_cache->GetAtlasState();
    if (atlasEntry != RHI::ResourceState::UNORDERED_ACCESS)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(&albedoAtlas,  atlasEntry,
                                              RHI::ResourceState::UNORDERED_ACCESS));
        cl.PushBarrier(RHI::GPUBarrier::Image(&surfaceAtlas, atlasEntry,
                                              RHI::ResourceState::UNORDERED_ACCESS));
    }

    const RHI::Texture* rtvs[RT_Count];
    for (uint32_t i = 0; i < RT_Count; ++i) rtvs[i] = &m_scratch[i];

    const uint64_t albedoUav  = cl.GetDevice().GetTextureUAVGpuHandle(albedoAtlas);
    const uint64_t surfaceUav = cl.GetDevice().GetTextureUAVGpuHandle(surfaceAtlas);
    const uint64_t srcAlbedo  = cl.GetDevice().GetTextureSRVGpuHandle(m_scratch[RT_Albedo]);
    const uint64_t srcSurface = cl.GetDevice().GetTextureSRVGpuHandle(m_scratch[RT_Surface]);
    const uint64_t srcEmis    = cl.GetDevice().GetTextureSRVGpuHandle(m_scratch[RT_Emissive]);

    constexpr uint32_t kResolved[3] = { RT_Albedo, RT_Surface, RT_Emissive };

    for (uint32_t s = 0; s < schedCount; ++s)
    {
        const DrawPacket& dp = draws[sched[s].packet];

        // Scratch back to RENDERTARGET (a no-op on the first iteration).
        for (uint32_t k : kResolved)
        {
            if (m_scratchState[k] == RHI::ResourceState::RENDERTARGET) continue;
            cl.PushBarrier(RHI::GPUBarrier::Image(&m_scratch[k], m_scratchState[k],
                                                  RHI::ResourceState::RENDERTARGET));
            m_scratchState[k] = RHI::ResourceState::RENDERTARGET;
        }

        // Raster the section into its own UV space.
        cl.SetRenderTargets(RT_Count, rtvs, nullptr);
        const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
        for (uint32_t i = 0; i < RT_Count; ++i)
            cl.ClearRenderTarget(m_scratch[i], zero);
        cl.SetViewport(MaterialSurfaceCache::kPageSize, MaterialSurfaceCache::kPageSize);
        cl.SetScissorRect(MaterialSurfaceCache::kPageSize, MaterialSurfaceCache::kPageSize);
        cl.SetPrimitiveTopology();

        // ALPHA_TEST out: the clip runs against a screen-space fwidth and would punch holes
        // in the page. DOUBLE_SIDED out: it makes the PS take SV_IsFrontFace and flip N/BT
        // on back faces, and "front face" in UV space is decided by UV winding, so a
        // mirrored UV shell would get an inverted normal; with cull NONE the bit does nothing
        // useful here anyway.
        PermutationKey perm = dp.permutation;
        perm.Set(PermutationKey::ALPHA_TEST,   false);
        perm.Set(PermutationKey::DOUBLE_SIDED, false);

        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildBakePSODesc(perm, dp.customPSID));
        if (!pso || !pso->IsValid()) continue;

        cl.SetPipelineState(*pso);
        BindGlobals(cl, bindlessHdl);

        const uint64_t bc = dp.texBaseColor  ? dp.texBaseColor
                                             : m_gbuffer->GetDefaultWhiteSrvHandle();
        const uint64_t sm = dp.texSurfaceMap ? dp.texSurfaceMap
                                             : m_gbuffer->GetDefaultWhiteSrvHandle();
        const uint64_t nm = dp.texNormalMap  ? dp.texNormalMap
                                             : m_gbuffer->GetDefaultFlatNormalSrvHandle();
        if (bc) cl.BindDescriptorTableHandle(S::SRV_t3, bc);
        if (sm) cl.BindDescriptorTableHandle(S::SRV_t4, sm);
        if (nm) cl.BindDescriptorTableHandle(S::SRV_t5, nm);

        // The b8 CBV and the space3 texture table are the custom material: skip them and
        // the hook samples the default-white fill and reads zeroed params, baking a shader
        // that is not the one on screen.
        if (dp.customCbvVA)    cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl);
        if (dp.customTexTable) cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl);

        {
            const uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.prevPosElementBase
            };
            cl.gfx->SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
        }
        // instanceCount forced to 1: every instance of this batch shares the
        // same mesh AND the same material, so they all rasterise to the SAME
        // page texels. Drawing N of them would be N identical overdraws.
        cl.DrawInstanced(dp.vertexOrIndexCount, 1, 0, 0);

        // Resolve + dilate.
        for (uint32_t k : kResolved)
        {
            cl.PushBarrier(RHI::GPUBarrier::Image(&m_scratch[k],
                RHI::ResourceState::RENDERTARGET, RHI::ResourceState::SHADER_RESOURCE));
            m_scratchState[k] = RHI::ResourceState::SHADER_RESOURCE;
        }

        uint32_t px = 0, py = 0;
        m_cache->GetPageOrigin(sched[s].page, px, py);
        // One CB slot per bake, never one shared slot rewritten in the loop: all N
        // dispatches are recorded into the same command list and read the buffer at execute
        // time, so one slot would give every page the last page's rect. The cursor is
        // monotonic across frames (see kCBSlots).
        const uint32_t cbSlot = m_cbCursor;
        m_cbCursor = (m_cbCursor + 1u) % kCBSlots;
        {
            ResolveCB cb{};
            cb.pageOriginX = px;
            cb.pageOriginY = py;
            cb.pageSizeX   = MaterialSurfaceCache::kPageSize;
            cb.pageSizeY   = MaterialSurfaceCache::kPageSize;
            cb.dilateRings = m_dilateRings;
            std::memcpy(static_cast<uint8_t*>(m_resolveCBMapped) + cbSlot * kCBSlotStride,
                        &cb, sizeof(cb));
        }

        constexpr uint32_t kGroup = 8;
        const uint32_t groups = (MaterialSurfaceCache::kPageSize + kGroup - 1) / kGroup;

        auto dispatchResolve = [&](const RHI::PipelineState& pso2)
        {
            cl.gfx->BindComputePipelineState(pso2, cl);
            // BindComputePipelineState re-sets the compute root signature, which invalidates
            // every compute root binding: all six are re-bound per dispatch, not hoisted.
            cl.gfx->SetComputeRootCBV(C::CB, m_resolveCB, cbSlot * kCBSlotStride, cl);
            cl.gfx->SetComputeDescriptorTable(C::SRV_t0, srcAlbedo,  cl);
            cl.gfx->SetComputeDescriptorTable(C::SRV_t1, srcSurface, cl);
            cl.gfx->SetComputeDescriptorTable(C::SRV_t2, srcEmis,    cl);
            cl.gfx->SetComputeDescriptorTable(C::UAV_u0, albedoUav,  cl);
            cl.gfx->SetComputeDescriptorTable(C::UAV_u1, surfaceUav, cl);
            cl.gfx->DispatchCompute(groups, groups, 1, cl);
        };

        dispatchResolve(m_resolvePSO);
        // main writes every page texel (covered ones with data, gutter ones with the
        // defined clear value) and CSDilate overwrites part of the gutter: same UAV, same
        // texels, so the two must not overlap in flight.
        cl.PushBarrier(RHI::GPUBarrier::Memory(nullptr));
        dispatchResolve(m_dilatePSO);
        cl.PushBarrier(RHI::GPUBarrier::Memory(nullptr));

        m_cache->MarkBaked(sched[s].page);
        ++m_lastBakeCount;
    }

    // Atlases back to SR for this frame's RT consumers.
    cl.PushBarrier(RHI::GPUBarrier::Image(&albedoAtlas,
        RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::SHADER_RESOURCE));
    cl.PushBarrier(RHI::GPUBarrier::Image(&surfaceAtlas,
        RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::SHADER_RESOURCE));
    m_cache->SetAtlasState(RHI::ResourceState::SHADER_RESOURCE);

    return cl;
}
