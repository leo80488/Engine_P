#include "RenderGraph/RenderPass/BloomPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"          // placed-resource path (alias arena)
#include "Graphics/TransientAliasArena.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include <cstring>

// Compute root-signature slots from RHI::Slots::Cmp (Graphics/RootSignatureLayout.h):
// S::CB = b0 space2, S::SRV_t0 = t0 space2, S::UAV_u0 = u0 space2.
namespace S = RHI::Slots::Cmp;

struct BloomCB
{
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
    float    filterRadius;
    uint32_t firstDownsample;
    float    pad0;
    float    pad1;
};

void BloomPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::BloomDownsample_CS, RHI::ShaderStage::CS,
                         "Bloom.cs.hlsl", "CSDownsample");
    m_shaderLib.Register(ShaderID::BloomUpsample_CS, RHI::ShaderStage::CS,
                         "Bloom.cs.hlsl", "CSUpsample");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("BloomPass: initialized");
}

// Onto the live members: a failed rebuild keeps the old pipeline.
bool BloomPass::CreatePipelines(IGraphicsDevice& gfx)
{
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::BloomDownsample_CS);
        if (!cs) { LOG_ERROR("BloomPass: BloomDownsample_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_downsamplePSO))
            LOG_ERROR("BloomPass: downsample PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::BloomUpsample_CS);
        if (!cs) { LOG_ERROR("BloomPass: BloomUpsample_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_upsamplePSO))
            LOG_ERROR("BloomPass: upsample PSO failed");
    }
    return true;
}

void BloomPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void BloomPass::DestroyTextures()
{
    if (!m_gfx) return;
    for (int i = 0; i < kSteps; ++i)
        if (m_chain->tex[i].IsValid()) m_gfx->DestroyTexture(m_chain->tex[i]);
    m_chain->placedMask = 0;
}

void BloomPass::BindView(ViewState& view)
{
    m_chain = view.Slot() == 0 ? &m_mainChain : &view.Data<ViewChain>(this).chain;
}

void BloomPass::ViewChain::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture& t : chain.tex)
        if (t.IsValid()) gfx.DestroyTexture(t);
    chain = Chain{};
}

void BloomPass::RebuildTextures(bool allowPlaced)
{
    if (!m_gfx) return;
    DestroyTextures();

    // Chain geometry first (needed for the arena layout pass below).
    {
        uint32_t w = (m_vpW + 1) / 2;
        uint32_t h = (m_vpH + 1) / 2;
        for (int i = 0; i < kSteps; ++i)
        {
            m_chain->w[i] = (w < 1) ? 1 : w;
            m_chain->h[i] = (h < 1) ? 1 : h;
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
    }

    auto descFor = [&](int i)
    {
        RHI::TextureDesc td{};
        td.width      = m_chain->w[i];
        td.height     = m_chain->h[i];
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        return td;
    };

    // Shared alias arena (main-thread rebuilds of the main view's chain only). All 5 levels live from
    // downsample through upsample (the upsample adds into lower levels), so they get distinct offsets
    // and alias only other groups' transients.
    const bool mainChain = m_chain == &m_mainChain;
    bool placed = false;   // in the arena's heap (through IGraphicsDevice)
    uint64_t offsets[kSteps]{};
    uint64_t cursorEnd = 0;
    if (allowPlaced && m_aliasArena && mainChain)
    {
        uint64_t cursor = 0;
        bool ok = true;
        for (int i = 0; i < kSteps; ++i)
        {
            uint64_t sz = 0, al = 0;
            if (!m_gfx->GetTextureAllocationInfo(descFor(i), sz, al)) { ok = false; break; }
            offsets[i] = TransientAliasArena::AlignUp(cursor, al);
            cursor     = offsets[i] + sz;
        }
        if (ok)
        {
            cursorEnd = cursor;
            placed = m_gfx->EnsureAliasArena(*m_aliasArena, cursor);
        }
    }

    char dbg[24];
    for (int i = 0; i < kSteps; ++i)
    {
        RHI::TextureDesc td = descFor(i);
        std::snprintf(dbg, sizeof(dbg), "Bloom[%d]", i);
        td.debugName = dbg;

        bool created = false;
        if (placed)
        {
            if (m_gfx->CreateTexturePlaced(td, *m_aliasArena, offsets[i], m_chain->tex[i]))
            {
                created = true;
                m_chain->placedMask |= uint8_t(1u << i);
            }
        }
        if (!created && !m_gfx->CreateTexture(td, m_chain->tex[i]))
            LOG_ERROR("BloomPass: failed to create bloom[%d]", i);

        m_chain->state[i] = RHI::ResourceState::UNORDERED_ACCESS;
    }
    if (m_aliasArena && mainChain)
    {
        m_chain->placedEpoch = m_aliasArena->Epoch();
        // Publish where group D's late joiners may start. Zero when we fell
        // back to committed textures: none of our bytes are in the heap then,
        // so the joiners are free to pack from 0.
        m_aliasArena->SetGroupDBase(m_chain->placedMask ? cursorEnd : 0);
    }

    m_chain->lastW = m_vpW;
    m_chain->lastH = m_vpH;
    LOG_INFO("BloomPass: rebuilt bloom chain %ux%u%s%s", m_vpW, m_vpH,
             m_chain->placedMask ? " (alias arena)" : "", mainChain ? "" : " (secondary view)");
}

void BloomPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    // Also rebuild when stale against the shared heap (another resident grew it); see
    // TransientAliasArena.h.
    const bool arenaStale = m_aliasArena && m_chain->placedMask &&
                            m_chain->placedEpoch != m_aliasArena->Epoch();
    if (m_vpW != m_chain->lastW || m_vpH != m_chain->lastH || arenaStale)
        RebuildTextures(/*allowPlaced=*/true);   // main thread: placed is OK
}

uint64_t BloomPass::GetBloomSrvHandle() const
{
    if (!m_gfx || !m_chain->tex[0].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_chain->tex[0]);
}

RHI::CommandList BloomPass::Execute(RHI::CommandList cl)
{
    if (!m_downsamplePSO.IsValid() || !m_upsamplePSO.IsValid()) return cl;
    if (m_vpW == 0 || m_vpH == 0) return cl;
    if (m_hdrSrvHandle == 0) return cl;

    // Worker-thread fallback: committed textures only (allowPlaced defaults to false); placed
    // creation belongs to the main-thread PrepareResources.
    if (m_vpW != m_chain->lastW || m_vpH != m_chain->lastH)
        RebuildTextures();
    Chain& c = *m_chain;
    if (!c.tex[0].IsValid()) return cl;

    auto& gfx = *m_gfx;

    // ---- Alias-acquire the placed chain (shared arena residents) ----
    // Cross-queue resident recorded into computeCL. Ordering against the direct-queue groups (RTR /
    // XeGTAO / VolFog) is the existing fence pair (computeCL waits preComputeCL, which covers every
    // graph pass list; restoreCL waits computeCL), so the barrier only has to precede our first use.
    // Null before = any resource; the downsample fully rewrites every level before it is read. State
    // mirrors stay untouched.
    if (c.placedMask)
    {
        for (int i = 0; i < kSteps; ++i)
            if (c.placedMask & (1u << i))
                gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr, &c.tex[i]), cl);
    }

    // One CB per dispatch from the frame's constants: a shared one would hand the next step's CPU
    // write to a dispatch the GPU has not run yet. Returns false when the frame's constants ran out
    // (reported); that step is skipped, its transitions are not.
    auto bindCB = [&](const BloomCB& data) -> bool
    {
        const RHI::Constants<BloomCB> c = AllocConstants<BloomCB>(gfx);
        if (!c) return false;
        *c.cpu = data;
        gfx.SetComputeRootCBVAddress(S::CB, c.gpu, cl);
        return true;
    };

    // Helper to transition a bloom texture
    auto transitionBloom = [&](int i, RHI::ResourceState to)
    {
        if (c.state[i] == to) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(&c.tex[i], c.state[i], to), cl);
        c.state[i] = to;
    };

    // Restore any bloom textures left in SRV state from last frame
    for (int i = 0; i < kSteps; ++i)
        transitionBloom(i, RHI::ResourceState::UNORDERED_ACCESS);

    // --- Downsample phase ---
    gfx.BindComputePipelineState(m_downsamplePSO, cl);

    for (int i = 0; i < kSteps; ++i)
    {
        uint32_t srcW = (i == 0) ? m_vpW    : c.w[i - 1];
        uint32_t srcH = (i == 0) ? m_vpH    : c.h[i - 1];

        // Transition input bloom[i-1] to SRV if needed
        if (i > 0)
            transitionBloom(i - 1, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

        BloomCB cb{};
        cb.srcWidth       = srcW;
        cb.srcHeight      = srcH;
        cb.dstWidth       = c.w[i];
        cb.dstHeight      = c.h[i];
        cb.filterRadius   = 1.0f;
        cb.firstDownsample = (i == 0) ? 1u : 0u;
        if (!bindCB(cb)) continue;

        uint64_t srvHandle = (i == 0) ? m_hdrSrvHandle
                                      : gfx.GetTextureSRVGpuHandle(c.tex[i - 1]);
        gfx.SetComputeDescriptorTable(S::SRV_t0, srvHandle, cl);
        gfx.SetComputeDescriptorTable(S::UAV_u0, gfx.GetTextureUAVGpuHandle(c.tex[i]), cl);

        gfx.DispatchCompute((c.w[i] + 7) / 8, (c.h[i] + 7) / 8, 1, cl);

        // No UAV barrier: the next touch of bloom[i] is always a UAV -> SRV transition (the next
        // iteration's transitionBloom(i-1), or the upsample loop's for the last step), which already
        // synchronises the write -> read.
    }

    // --- Upsample phase (bloom[4] -> bloom[3] -> ... -> bloom[0]) ---
    gfx.BindComputePipelineState(m_upsamplePSO, cl);

    for (int i = kSteps - 1; i > 0; --i)
    {
        // Transition bloom[i] to SRV for reading
        transitionBloom(i, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        // Transition bloom[i-1] to UAV for writing (it may be in SRV from downsample)
        transitionBloom(i - 1, RHI::ResourceState::UNORDERED_ACCESS);

        BloomCB cb{};
        cb.srcWidth      = c.w[i];
        cb.srcHeight     = c.h[i];
        cb.dstWidth      = c.w[i - 1];
        cb.dstHeight     = c.h[i - 1];
        cb.filterRadius  = 1.0f; // Used in upsample
        cb.firstDownsample = 0;
        if (!bindCB(cb)) continue;
        gfx.SetComputeDescriptorTable(S::SRV_t0, gfx.GetTextureSRVGpuHandle(c.tex[i]), cl);
        gfx.SetComputeDescriptorTable(S::UAV_u0, gfx.GetTextureUAVGpuHandle(c.tex[i - 1]), cl);

        gfx.DispatchCompute((c.w[i-1] + 7) / 8, (c.h[i-1] + 7) / 8, 1, cl);
        // No UAV barrier: bloom[i-1] is next touched by transitionBloom to SHADER_RESOURCE_COMPUTE (next
        // iteration, or the bloom[0] transition after the loop), which synchronises.
    }

    // Transition bloom[0] to SRV so ToneMapPass can read it
    transitionBloom(0, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    return cl;
}
