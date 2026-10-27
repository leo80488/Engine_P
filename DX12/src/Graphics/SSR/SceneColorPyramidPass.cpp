#include "Graphics/SSR/SceneColorPyramidPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    // Absolute compute root indices: this pass binds the shared compute root signature, so
    // every value comes from RHI::Slots::Cmp (Graphics/RootSignatureLayout.h). Do not take
    // these from Gfx::. Mip 0 is a CopyTextureSubresource (Execute), so there is no SRV slot.
    namespace S = RHI::Slots::Cmp;
    constexpr auto kCBSlot    = S::CB;      // b0 space2
    constexpr auto kUAV0      = S::UAV_u0;  // u0 space2 - reduce src (mip - 1)
    constexpr auto kUAV1      = S::UAV_u1;  // u1 space2 - reduce dst
}

void SceneColorPyramidPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SceneColorPyramidMip0_CS, RHI::ShaderStage::CS,
                         "SceneColorPyramid.cs.hlsl", "CSMain_Mip0");
    m_shaderLib.Register(ShaderID::SceneColorPyramidReduce_CS, RHI::ShaderStage::CS,
                         "SceneColorPyramid.cs.hlsl", "CSMain_Reduce");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SceneColorPyramidMip0_CS);
        if (!cs) { LOG_ERROR("SceneColorPyramidPass: mip0 CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_mip0PSO))
        { LOG_ERROR("SceneColorPyramidPass: mip0 PSO create failed"); return; }
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SceneColorPyramidReduce_CS);
        if (!cs) { LOG_ERROR("SceneColorPyramidPass: reduce CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_reducePSO))
        { LOG_ERROR("SceneColorPyramidPass: reduce PSO create failed"); return; }
    }

    // One 256-byte CB slot per mip: each reduce dispatch writes its own slot and binds it by
    // byteOffset. Do not share one slot across the dispatches of a command list: the CPU
    // overwrites it before the GPU reads it, every dispatch sees the smallest mip's size and
    // early-outs, and mips >= 1 stay zero (black glossy SSR).
    m_cb.Create(gfx, "SceneColorPyramid.CB");

    LOG_SUCCESS("SceneColorPyramidPass: initialized");
}

void SceneColorPyramidPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    auto rebuild = [&](ShaderID id, RHI::PipelineState& pso, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SceneColorPyramidPass::ReloadShaders: %s missing", tag); return; }
        RHI::PipelineStateDesc d{}; d.cs = cs;
        if (!gfx.CreatePipelineState(d, pso))
            LOG_ERROR("SceneColorPyramidPass::ReloadShaders: %s PSO rebuild failed", tag);
    };
    rebuild(ShaderID::SceneColorPyramidMip0_CS,   m_mip0PSO,   "mip0");
    rebuild(ShaderID::SceneColorPyramidReduce_CS, m_reducePSO, "reduce");
}

void SceneColorPyramidPass::ReleaseTextures()
{
    if (!m_gfx) return;
    if (m_texture.IsValid()) m_gfx->DestroyTexture(m_texture);
    m_texture = {};
    m_width = m_height = 0;
}

void SceneColorPyramidPass::EnsureTexture(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_width && h == m_height && m_texture.IsValid()) return;

    if (m_texture.IsValid()) m_gfx->DestroyTexture(m_texture);
    m_width  = w;
    m_height = h;
    m_inSR   = false;   // fresh texture starts in UAV (td.layout below)
    m_built  = false;   // content is garbage until the first Execute()

    m_mipCount = 1;
    { uint32_t dim = std::max(w, h); while (dim > 1) { dim >>= 1; m_mipCount++; } }

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.mipLevels = m_mipCount;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_texture))
        LOG_ERROR("SceneColorPyramidPass: pyramid create failed (%ux%u, %u mips)",
                  w, h, m_mipCount);
    else
        LOG_INFO("SceneColorPyramidPass: pyramid %ux%u (%u mips) ready",
                 w, h, m_mipCount);
}

uint64_t SceneColorPyramidPass::GetSrvHandle() const
{
    if (!m_gfx || !m_texture.IsValid() || !m_built) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_texture);
}

void SceneColorPyramidPass::Execute(RHI::CommandList cl, const RHI::Texture* snapshotTex)
{
    if (!m_reducePSO.IsValid()) return;
    if (!m_texture.IsValid() || m_mipCount == 0 || !m_cb.IsValid()) return;
    if (!snapshotTex || !snapshotTex->IsValid()) return;

    auto& gfx = static_cast<GraphicsDX12&>(*m_gfx);
    auto* mapped = m_cb.Current(gfx);
    if (!mapped) return;
    uint8_t* cbBase = mapped->bytes;
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // Mip 0: a hardware copy of the snapshot (CopyTextureSubresource; the snapshot in COPY_SRC,
    // the pyramid in COPY_DST). Do not write mip 0 from a CS: those writes were silently dropped.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        snapshotTex, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::COPY_SRC), cl);
    // Entry state: SHADER_RESOURCE on every frame but the first after
    // (re)creation (see the state contract in the header).
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_texture,
        m_inSR ? RHI::ResourceState::SHADER_RESOURCE
               : RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::COPY_DST), cl);

    gfx.CopyTextureSubresource(*snapshotTex, /*srcMip*/0, /*srcSlice*/0,
                                m_texture,    /*dstMip*/0, /*dstSlice*/0, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        snapshotTex, RHI::ResourceState::COPY_SRC,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_texture, RHI::ResourceState::COPY_DST,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    // Mips 1..N-1: UAV -> UAV reduce (Karis firefly-weighted average).
    gfx.BindComputePipelineState(m_reducePSO, cl);
    const uint32_t maxMips = std::min(m_mipCount, kMaxMipsCB);
    for (uint32_t mip = 1; mip < maxMips; ++mip)
    {
        const uint32_t srcW = std::max(m_width  >> (mip - 1), 1u);
        const uint32_t srcH = std::max(m_height >> (mip - 1), 1u);
        const uint32_t dstW = std::max(m_width  >> mip, 1u);
        const uint32_t dstH = std::max(m_height >> mip, 1u);

        // Per-mip CB slot at byteOffset = mip * kCBStride, bound at the same offset, so later
        // CPU writes do not clobber the parameters of dispatches already recorded.
        const uint32_t cbOffset = mip * kCBStride;
        HierCB cb{ srcW, srcH, dstW, dstH };
        std::memcpy(cbBase + cbOffset, &cb, sizeof(cb));
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, cbOffset, cl);

        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip - 1), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip), cl);

        gfx.DispatchCompute((dstW + 7) / 8, (dstH + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);
    }

    // Park in SHADER_RESOURCE (state contract): the SSR trace samples it this frame and
    // RTReflectionTrace reuses it next frame for hit-point radiance.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_texture, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_inSR  = true;
    m_built = true;
}
