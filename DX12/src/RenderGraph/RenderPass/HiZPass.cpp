#include "RenderGraph/RenderPass/HiZPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include <cstring>
#include <algorithm>

// Compute root signature slots (space2) - aliases of the shared compute root
// signature (Graphics/RootSignatureLayout.h).
static constexpr auto kCBSlot  = RHI::Slots::Cmp::CB;      // b0 space2
static constexpr auto kSRV0    = RHI::Slots::Cmp::SRV_t0;  // t0 space2 - source mip (mip 0 path: depth SRV)
static constexpr auto kUAV0    = RHI::Slots::Cmp::UAV_u0;  // u0 space2 - dest (mip 0) OR source (reduce)
static constexpr auto kUAV1    = RHI::Slots::Cmp::UAV_u1;  // u1 space2 - dest mip N   (reduce path)
static constexpr auto kUAV2    = RHI::Slots::Cmp::UAV_u2;  // u2 space2 - dest mip N+1 (reduce path)

void HiZPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::HiZGenerate_CS, RHI::ShaderStage::CS,
                         "HiZGenerate.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::HiZReduce_CS, RHI::ShaderStage::CS,
                         "HiZReduce.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("HiZPass: initialized");
}

// Onto the live members: a failed rebuild keeps the old pipeline.
bool HiZPass::CreatePipelines(IGraphicsDevice& gfx)
{
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HiZGenerate_CS);
        if (!cs) { LOG_ERROR("HiZPass: HiZGenerate_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_pso))
        { LOG_ERROR("HiZPass: generate PSO creation failed"); return false; }
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HiZReduce_CS);
        if (!cs) { LOG_ERROR("HiZPass: HiZReduce_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_reducePSO))
        { LOG_ERROR("HiZPass: reduce PSO creation failed"); return false; }
    }
    return true;
}

void HiZPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void HiZPass::EnsureTexture(uint32_t depthW, uint32_t depthH)
{
    if (!m_gfx) return;

    // Round each dimension up to a multiple of 32, NOT to the next power of two:
    // pow2 rounding turns a 1920x1080 render into a 2048x2048 R32_FLOAT (~22 MB)
    // whose mip-0 dispatch mostly writes padding. 32-alignment costs at most 31 rows /
    // columns and keeps the first five mips exactly addressable by the consumer's
    // `pixel >> mip` mapping (InstanceCull.cs.hlsl HiZOcclusionTest).
    auto alignUp32 = [](uint32_t v) -> uint32_t { return (v + 31u) & ~31u; };

    uint32_t w = alignUp32(depthW);
    uint32_t h = alignUp32(depthH);

    // Valid extent tracks the render dims even when the aligned texture size is
    // unchanged (e.g. 1900x1070 and 1920x1080 both round to 1920x1088).
    m_validWidth  = depthW;
    m_validHeight = depthH;

    // GROW-ONLY: alternating render sizes (editor viewport <-> F11 fullscreen, the
    // upscaler's render resolution) would otherwise re-create the texture on every
    // flip, and each re-create leaves the pyramid garbage until the next full
    // generation. A larger texture is harmless: generation covers its full extent
    // (far padding beyond the valid region) and the cull maps taps by the valid dims.
    if (m_hiZTexture.IsValid() && w <= m_hiZWidth && h <= m_hiZHeight) return;
    w = (std::max)(w, m_hiZWidth);
    h = (std::max)(h, m_hiZHeight);

    if (m_hiZTexture.IsValid())
        m_gfx->DestroyTexture(m_hiZTexture);

    m_hiZWidth  = w;
    m_hiZHeight = h;

    m_mipCount = 1;
    { uint32_t dim = std::max(w, h); while (dim > 1) { dim >>= 1; m_mipCount++; } }

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.mipLevels = m_mipCount;
    td.format     = RHI::Format::R32_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_hiZTexture))
        LOG_ERROR("HiZPass: failed to create Hi-Z texture (%ux%u, %u mips)", w, h, m_mipCount);
    else
        LOG_INFO("HiZPass: created Hi-Z %ux%u (%u mips)", w, h, m_mipCount);

    // Fresh texture starts in the creation layout.
    m_texState = RHI::ResourceState::UNORDERED_ACCESS;
}

uint64_t HiZPass::GetHiZSrvHandle() const
{
    if (!m_gfx || !m_hiZTexture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_hiZTexture);
}

// One CB per dispatch (see the declaration).
bool HiZPass::BindCB(IGraphicsDevice& gfx, const HiZCB& cb, RHI::CommandList cl)
{
    const RHI::Constants<HiZCB> c = AllocConstants<HiZCB>(gfx);
    if (!c) return false;
    *c.cpu = cb;
    gfx.SetComputeRootCBVAddress(kCBSlot, c.gpu, cl);
    return true;
}

bool HiZPass::Execute(RHI::CommandList cl, uint64_t depthSrvHandle)
{
    if (!m_pso.IsValid() || !m_reducePSO.IsValid()) return false;
    if (!m_hiZTexture.IsValid() || m_mipCount == 0) return false;

    IGraphicsDevice& gfx = *m_gfx;

    // Last frame's Execute parked the texture in SHADER_RESOURCE for the cull; bring
    // it back to UAV for this frame's generation. (A fresh texture is created in the
    // UAV layout - no barrier then.)
    if (m_texState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hiZTexture,
            m_texState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_texState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // ---- Mip 0: depth SRV -> Hi-Z mip 0 (HiZGenerate_CS) ----
    // The shader point-samples the depth texel at DTid and writes a 1:1 copy.
    {
        const uint32_t w = m_hiZWidth;
        const uint32_t h = m_hiZHeight;

        gfx.BindComputePipelineState(m_pso, cl);

        // src = VALID depth extent (bounds the 1:1 copy; beyond it the
        // shader writes far-plane padding), dst = the full mip 0 extent.
        if (!BindCB(gfx, { m_validWidth, m_validHeight, w, h, 0 }, cl)) return false;
        gfx.SetComputeDescriptorTable(kSRV0, depthSrvHandle, cl);
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, 0), cl);

        gfx.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hiZTexture), cl);
    }

    // ---- Mips 1..N-1: UAV -> UAV reduce (HiZReduce_CS) ----
    // The reduce PSO is bound once; each dispatch swaps only the per-mip UAV tables
    // and the CB. The texture stays in UNORDERED_ACCESS; UAV barriers between
    // dispatches order the reads after the writes.
    // TWO MIPS PER DISPATCH: each mip READS the one before, a true dependency no
    // barrier trick removes, so the shader reduces its 8x8 tile a second time through
    // groupshared (depth_pyramid_reduce.hlsli), halving the dispatches and the UAV
    // barriers - the 4x4 / 2x2 / 1x1 tail is pure launch overhead otherwise.
    gfx.BindComputePipelineState(m_reducePSO, cl);

    for (uint32_t mip = 1; mip < m_mipCount; mip += 2)
    {
        const uint32_t srcW = (std::max)(m_hiZWidth  >> (mip - 1), 1u);
        const uint32_t srcH = (std::max)(m_hiZHeight >> (mip - 1), 1u);
        const uint32_t dstW = (std::max)(m_hiZWidth  >> mip, 1u);
        const uint32_t dstH = (std::max)(m_hiZHeight >> mip, 1u);

        // On an odd tail the second level has no mip to write; u2 is still
        // bound (to mip N) so no root descriptor table is left unset, and
        // writeSecond == 0 keeps the shader from touching it.
        const bool pair = (mip + 1 < m_mipCount);

        // A pyramid cut short is not one: the texture stays in UAV (m_texState
        // says so) and the caller does not count the frame.
        if (!BindCB(gfx, { srcW, srcH, dstW, dstH, pair ? 1u : 0u }, cl)) return false;

        // u0 = source (prev mip, read-only UAV), u1 = mip N, u2 = mip N+1.
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, mip - 1), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, mip), cl);
        gfx.SetComputeDescriptorTable(kUAV2,
            gfx.GetTextureMipUAVGpuHandle(m_hiZTexture, pair ? mip + 1 : mip), cl);

        // Group count follows the mip N extent; the second level's 4x4 output
        // per group covers exactly ceil(dstW/8) x ceil(dstH/8) mip N+1 texels.
        gfx.DispatchCompute((dstW + 7) / 8, (dstH + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hiZTexture), cl);
    }

    // Park in SHADER_RESOURCE: the consumer is next frame's InstanceCull SRV read
    // (t2). Reading an UNORDERED_ACCESS-state texture through an SRV is undefined on
    // hardware even though replays hide it.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hiZTexture,
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_texState = RHI::ResourceState::SHADER_RESOURCE;
    return true;
}
