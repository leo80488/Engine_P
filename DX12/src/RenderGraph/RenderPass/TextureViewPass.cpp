#include "RenderGraph/RenderPass/TextureViewPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <chrono>
#include <cmath>
#include <cstring>

// Compute root signature slots (shared with every compute pass, space2).
static constexpr auto kCBSlot = RHI::Slots::Cmp::CB;
static constexpr auto kUAV0   = RHI::Slots::Cmp::UAV_u0;   // u0: the view's output (CSMain)
static constexpr auto kUAV1   = RHI::Slots::Cmp::UAV_u1;   // u1: the probe results (ProbeCS)

// shaders/TextureView.cs.hlsl FLAG_* / BG_*
static constexpr std::uint32_t kFlagSrgb       = 1u << 0;
static constexpr std::uint32_t kFlagTile       = 1u << 1;
static constexpr std::uint32_t kFlagFiltered   = 1u << 2;
static constexpr std::uint32_t kFlagOneChannel = 1u << 3;
static constexpr std::uint32_t kFlagTonemap    = 1u << 4;
static constexpr std::uint32_t kFlagNoSource   = 1u << 5;
static constexpr std::uint32_t kBgChecker      = 0;
static constexpr std::uint32_t kBgSolid        = 1;

bool TextureViewPass::EnsurePipeline()
{
    if (m_ready) return true;
    if (m_failed || !m_gfx) return false;
    IGraphicsDevice& gfx = *m_gfx;
    const auto t0 = std::chrono::steady_clock::now();

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TextureView_CS, RHI::ShaderStage::CS, "TextureView.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::TextureViewProbe_CS, RHI::ShaderStage::CS, "TextureView.cs.hlsl", "ProbeCS");

    RHI::GPUBufferDesc d{};
    d.size       = std::uint64_t(RHI::kFramesInFlight) * kSlotsPerFrame * kSlotBytes;
    d.usage      = RHI::Usage::UPLOAD;
    d.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
    if (!CreatePipelines() || !gfx.CreateBuffer(d, m_ring) ||
        !(m_ringMapped = static_cast<std::uint8_t*>(gfx.MapBuffer(m_ring))))
    {
        LOG_ERROR("TextureViewPass: not built (pipeline or constants failed) - Texture Viewer pictures stay "
                  "empty until the next shader reload");
        m_failed = true;
        return false;
    }
    std::memset(m_ringMapped, 0, static_cast<std::size_t>(d.size));

    // The probe results (one float4 per texture view) in a UAV the probes write, plus
    // kFramesInFlight READBACK copies of it.
    RHI::GPUBufferDesc pd{};
    pd.size       = std::uint64_t(kProbeSlots) * 16;
    pd.stride     = 16;
    pd.usage      = RHI::Usage::DEFAULT;
    pd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    pd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    bool probesOk = gfx.CreateBuffer(pd, m_probeScratch);
    for (std::uint32_t i = 0; probesOk && i < RHI::kFramesInFlight; ++i)
    {
        RHI::GPUBufferDesc rd{};
        rd.size  = std::uint64_t(kProbeSlots) * 16;
        rd.usage = RHI::Usage::READBACK;
        probesOk = gfx.CreateBuffer(rd, m_probeReadback[i]) &&
                   (m_probeMapped[i] = static_cast<const float*>(gfx.MapBuffer(m_probeReadback[i]))) != nullptr;
    }
    if (!probesOk)
        LOG_WARNING("TextureViewPass: the probe buffers failed - the viewer's value read-out stays empty");

    m_ready = true;
    LOG_INFO("TextureViewPass: built in %.1f ms (the first texture view asked for)",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

// Onto the live members: a failed rebuild keeps the old pipelines.
bool TextureViewPass::CreatePipelines()
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::TextureView_CS);
    if (!cs)
    {
        LOG_ERROR("TextureViewPass: TextureView.cs.hlsl did not compile");
        return false;
    }
    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!m_gfx->CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("TextureViewPass: pipeline failed");
        return false;
    }
    // The probe's: its failure costs the value read-out only.
    if (const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::TextureViewProbe_CS))
    {
        RHI::PipelineStateDesc qd{};
        qd.cs = ps;
        if (!m_gfx->CreatePipelineState(qd, m_probePso))
            LOG_WARNING("TextureViewPass: the probe pipeline failed - no value read-out");
    }
    return true;
}

void TextureViewPass::ReloadShaders()
{
    m_failed = false;   // the next EnsurePipeline tries again
    if (m_ready && m_shaderLib.ClearCaches()) CreatePipelines();
}

void TextureViewPass::Shutdown()
{
    if (!m_gfx) return;
    if (m_ringMapped) { m_gfx->UnmapBuffer(m_ring); m_ringMapped = nullptr; }
    if (m_ring.IsValid()) m_gfx->DestroyBuffer(m_ring);
    m_ring = {};
    for (std::uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_probeMapped[i]) { m_gfx->UnmapBuffer(m_probeReadback[i]); m_probeMapped[i] = nullptr; }
        if (m_probeReadback[i].IsValid()) m_gfx->DestroyBuffer(m_probeReadback[i]);
        m_probeReadback[i] = {};
    }
    if (m_probeScratch.IsValid()) m_gfx->DestroyBuffer(m_probeScratch);
    m_probeScratch = {};
    m_probeState   = RHI::ResourceState::UNDEFINED;
    if (m_probePso.IsValid()) m_gfx->DestroyPipelineState(m_probePso);
    if (m_pso.IsValid()) m_gfx->DestroyPipelineState(m_pso);
    m_ready = false;
}

std::uint32_t TextureViewPass::WriteConstants(std::uint32_t slot, const Job& job, std::uint32_t outW,
                                              std::uint32_t outH, std::uint32_t probeX, std::uint32_t probeY,
                                              std::uint32_t probeIndex)
{
    IGraphicsDevice& gfx = *m_gfx;
    const TextureView::Desc& d = job.desc;
    // The heap index is recomputed here, at record time, every time.
    const std::uint32_t srcIndex = job.sourceSrv ? gfx.DescriptorHeapIndexFromGpuHandle(job.sourceSrv) : ~0u;
    const bool have = srcIndex != ~0u && job.kind != TextureView::SourceKind::Unsupported && job.mipW && job.mipH;

    const std::uint32_t frame = gfx.GetFrameIndex() % RHI::kFramesInFlight;
    const std::uint32_t at    = (frame * kSlotsPerFrame + slot) * kSlotBytes;
    CBData c{};
    c.dstW = outW;
    c.dstH = outH;
    c.mipW = job.mipW;
    c.mipH = job.mipH;
    c.uvOrigin[0]   = d.uvOrigin[0];
    c.uvOrigin[1]   = d.uvOrigin[1];
    c.uvPerPixel[0] = d.uvPerPixel[0];
    c.uvPerPixel[1] = d.uvPerPixel[1];
    c.srcIndex    = have ? srcIndex : 0xFFFFFFFFu;
    c.srcKind     = static_cast<std::uint32_t>(job.kind);
    c.mip         = d.mip;
    c.slice       = d.slice;
    c.channelMask = d.channels & TextureView::kChRGBA;
    c.mapMode     = static_cast<std::uint32_t>(d.map);
    c.flags       = (job.srgb ? kFlagSrgb : 0u) | (d.tile ? kFlagTile : 0u) | (d.filtered ? kFlagFiltered : 0u) |
                    (job.oneChannel ? kFlagOneChannel : 0u) | (d.tonemap ? kFlagTonemap : 0u) |
                    (have ? 0u : kFlagNoSource);
    c.exposure   = std::exp2(d.exposureEV);
    c.rangeMin   = d.rangeMin;
    c.rangeMax   = d.rangeMax;
    c.cubeLayout = static_cast<std::uint32_t>(d.cubeLayout);
    float bg = 0.f;
    switch (d.background)
    {
    case TextureView::Background::Checker: c.background = kBgChecker; break;
    case TextureView::Background::Black:   c.background = kBgSolid; bg = 0.f; break;
    case TextureView::Background::Grey:    c.background = kBgSolid; bg = 0.5f; break;
    case TextureView::Background::White:   c.background = kBgSolid; bg = 1.f; break;
    }
    c.bgColor[0] = c.bgColor[1] = c.bgColor[2] = bg;
    c.bgColor[3] = 1.f;
    c.probeX     = probeX;
    c.probeY     = probeY;
    c.probeIndex = probeIndex;
    // Picture B: its heap index is recomputed at record time like A's.
    const std::uint32_t srcIndexB = (d.compare != TextureView::Compare::Off && job.sourceSrvB)
                                        ? gfx.DescriptorHeapIndexFromGpuHandle(job.sourceSrvB) : ~0u;
    const bool haveB = have && srcIndexB != ~0u && job.kindB != TextureView::SourceKind::Unsupported &&
                       job.mipWB && job.mipHB;
    c.mipWB       = job.mipWB;
    c.mipHB       = job.mipHB;
    c.srcIndexB   = haveB ? srcIndexB : 0xFFFFFFFFu;
    c.srcKindB    = static_cast<std::uint32_t>(job.kindB);
    c.mipB        = d.compareMip;
    c.sliceB      = d.compareSlice;
    c.flagsB      = (job.srgbB ? kFlagSrgb : 0u) | (job.oneChannelB ? kFlagOneChannel : 0u);
    c.compareMode = haveB ? static_cast<std::uint32_t>(d.compare) : 0u;
    c.splitX      = d.splitX;
    c.diffScale   = d.diffScale;
    std::memcpy(m_ringMapped + at, &c, sizeof(c));
    return at;
}

bool TextureViewPass::Execute(RHI::CommandList cl, std::uint32_t slot, const Job& job)
{
    if (!m_ready || !m_gfx || !m_pso.IsValid() || !m_ringMapped || slot >= kMaxPerFrame || !job.output ||
        !job.output->IsValid())
        return false;
    IGraphicsDevice& gfx = *m_gfx;
    const std::uint32_t outW = job.output->desc.width, outH = job.output->desc.height;
    if (outW == 0 || outH == 0) return false;
    const std::uint32_t at = WriteConstants(slot, job, outW, outH, 0, 0, 0);

    gfx.PushBarrier(RHI::GPUBarrier::Image(job.output, RHI::ResourceState::SHADER_RESOURCE,
                                           RHI::ResourceState::UNORDERED_ACCESS), cl);
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_ring, at, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(*job.output), cl);
    gfx.DispatchCompute((outW + 7) / 8, (outH + 7) / 8, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(job.output, RHI::ResourceState::UNORDERED_ACCESS,
                                           RHI::ResourceState::SHADER_RESOURCE), cl);
    return true;
}

std::uint32_t TextureViewPass::ExecuteProbes(RHI::CommandList cl, const Probe* probes, std::uint32_t count)
{
    if (!m_ready || !m_gfx || !m_probePso.IsValid() || !m_probeScratch.IsValid() || !m_ringMapped || count == 0)
        return 0;
    const std::uint32_t frame = m_gfx->GetFrameIndex() % RHI::kFramesInFlight;
    if (!m_probeReadback[frame].IsValid()) return 0;
    IGraphicsDevice& gfx = *m_gfx;
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_probeScratch, m_probeState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_probeState = RHI::ResourceState::UNORDERED_ACCESS;
    gfx.BindComputePipelineState(m_probePso, cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetBufferUAVGpuHandle(m_probeScratch), cl);
    std::uint32_t done = 0;
    for (std::uint32_t i = 0; i < count && done < kMaxProbesPerFrame; ++i)
    {
        const Probe& p = probes[i];
        if (p.index >= kProbeSlots) continue;
        const std::uint32_t at = WriteConstants(kMaxPerFrame + done, p.job, 1, 1, p.x, p.y, p.index);
        gfx.SetComputeRootCBV(kCBSlot, m_ring, at, cl);
        gfx.DispatchCompute(1, 1, 1, cl);
        ++done;
    }
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_probeScratch, RHI::ResourceState::UNORDERED_ACCESS,
                                            RHI::ResourceState::COPY_SRC), cl);
    m_probeState = RHI::ResourceState::COPY_SRC;
    gfx.CopyBuffer(m_probeScratch, m_probeReadback[frame], std::uint64_t(kProbeSlots) * 16, cl);
    return done;
}

const float* TextureViewPass::ProbeResults(std::uint32_t frameSlot) const
{
    return frameSlot < RHI::kFramesInFlight ? m_probeMapped[frameSlot] : nullptr;
}
