#include "RenderGraph/RenderPass/LightSamplingPass.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/ShaderDiskCache.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
// The pass's pipeline layout (Init): one root CBV, the per-stage block.
constexpr RHI::Slots::CmpParam kRootCB{ 0 };
}

void LightSamplingPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_log = Sys::CommandLine::Get().Has("-light_tables_log");
    m_corrupt = Sys::CommandLine::Get().Has("-light_tables_corrupt"); // the self-check's negative control
    if (!gfx.GetCapabilities().inlineRaytracing) return;
    // One root CBV (the per-stage 256-byte block); everything else is bindless.
    RHI::PipelineLayoutBuilder layout("LightSampling");
    layout.RootCBV(kRootCB, 0).Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(layout.Desc(), m_layout))
    { LOG_ERROR("LightSamplingPass: root signature failed"); return; }
    // The constants are created with the tables (EnsureResources): a frame
    // that never wants them allocates nothing, so no later descriptor shifts.
    m_pipelinesFailed = false;
}

bool LightSamplingPass::BuildPipelines()
{
    if (m_pso.back().IsValid()) return true;
    if (m_pipelinesFailed || !m_layout.IsValid() || !m_gfx) return false;
    const auto start = std::chrono::steady_clock::now();
    static constexpr const char* kEntryPoints[] = { "BuildPdf", "BuildMip", "Presample", "HistogramClear", "HistogramCount" };
    for (size_t i = 0; i < m_pso.size(); ++i)
    {
        const auto dxil = ShaderDiskCache::LoadOrCompile("shaders/LightSampling.cs.hlsl", kEntryPoints[i], RHI::ShaderStage::CS, {}, "LightSamplingPass");
        RHI::Shader cs;
        RHI::PipelineStateDesc pd{};
        pd.cs = &cs; pd.layout = &m_layout;
        const bool ok = !dxil.empty() && m_gfx->CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), cs) &&
                        m_gfx->CreatePipelineState(pd, m_pso[i]);
        m_gfx->DestroyShader(cs);   // the PSO keeps its own copy of the DXIL
        if (!ok)
        {
            LOG_ERROR("LightSamplingPass: pipeline %s failed", kEntryPoints[i]);
            for (auto& pso : m_pso) m_gfx->DestroyPipelineState(pso);
            m_pipelinesFailed = true;
            return false;
        }
    }
    LOG_INFO("LightSamplingPass: pipelines built on first use (%.0f ms)",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

void LightSamplingPass::Release(IGraphicsDevice& gfx)
{
    if (m_pdf.IsValid()) gfx.DestroyTexture(m_pdf);
    m_pdf = {};
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_mapped[i]) gfx.UnmapBuffer(m_readback[i]);
        if (m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
        m_readback[i] = {}; m_mapped[i] = nullptr; m_primed[i] = false;
    }
    for (auto* b : { &m_ris, &m_histogram }) { if (b->IsValid()) gfx.DestroyBuffer(*b); *b = {}; }
    m_pdfState = m_risState = m_histogramState = RHI::ResourceState::UNDEFINED;
}

void LightSamplingPass::EnsureResources(bool wanted)
{
    m_wanted = wanted;
    if (!m_gfx) return;
    if (!wanted) { if (m_pdf.IsValid()) Release(*m_gfx); return; }
    if (!BuildPipelines() || m_pdf.IsValid()) return;
    if (!m_cb.IsValid() && !m_cb.Create(*m_gfx, "LightTables.Constants")) { LOG_ERROR("LightSamplingPass: constants failed"); return; }
    auto& gfx = *m_gfx;
    RHI::TextureDesc td{};
    td.width = td.height = kPdfSize; td.mipLevels = kPdfMips;
    td.format = RHI::Format::R32_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "LightTables.Pdf";
    if (!gfx.CreateTexture(td, m_pdf)) { LOG_ERROR("LightSamplingPass: PDF texture failed"); return; }
    m_pdfState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::GPUBufferDesc bd{};
    bd.usage = RHI::Usage::DEFAULT;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    bd.stride = 8; bd.size = uint64_t(kEntries) * 8;
    if (!gfx.CreateBuffer(bd, m_ris)) { LOG_ERROR("LightSamplingPass: RIS buffer failed"); Release(gfx); return; }
    if (m_log)
    {
        // counts[kMaxLights] then pdf[kMaxLights] (float), from the same frame.
        bd.stride = 4; bd.size = 2ull * kMaxLights * 4;
        if (!gfx.CreateBuffer(bd, m_histogram)) { LOG_ERROR("LightSamplingPass: histogram failed"); Release(gfx); return; }
        bd.usage = RHI::Usage::READBACK; bd.bindFlags = RHI::BindFlag::NONE; bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
            if (!gfx.CreateBuffer(bd, m_readback[i]) || !(m_mapped[i] = static_cast<const uint32_t*>(gfx.MapBuffer(m_readback[i]))))
            { LOG_ERROR("LightSamplingPass: histogram readback failed"); Release(gfx); return; }
    }
    LOG_INFO("[LIGHT_TABLES] allocated=1 pdf=%ux%u mips=%u risEntries=%u bytes=%llu", kPdfSize, kPdfSize, kPdfMips, kEntries,
        static_cast<unsigned long long>(uint64_t(kPdfSize) * kPdfSize * 4 * 4 / 3 + uint64_t(kEntries) * 8));
}

void LightSamplingPass::Shutdown(IGraphicsDevice& gfx)
{
    Release(gfx);
    m_cb.Destroy(gfx);
    for (auto& pso : m_pso) gfx.DestroyPipelineState(pso);
    gfx.DestroyPipelineLayout(m_layout);
    m_active = false;
}

void LightSamplingPass::ReloadShaders(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred();
    const bool wanted = m_wanted;
    Shutdown(gfx); Init(gfx);
    EnsureResources(wanted);
}

void LightSamplingPass::SetFrameData(uint32_t lightsIdx, uint32_t lightCount, uint32_t frame, bool active)
{
    m_lightsIdx = lightsIdx; m_lightCount = std::min(lightCount, kMaxLights); m_frame = frame;
    m_active = active && m_wanted && m_pdf.IsValid() && m_ris.IsValid() && m_pso.back().IsValid() && lightsIdx;
}

LightSamplingPass::Tables LightSamplingPass::GetTables() const
{
    if (!m_gfx || !m_pdf.IsValid() || !m_ris.IsValid()) return {};
    auto index = [&](uint64_t h) -> uint32_t {
        if (!h) return 0u; const uint32_t i = m_gfx->DescriptorHeapIndexFromGpuHandle(h); return i == ~0u ? 0u : i; };
    Tables t;
    t.pdfIdx = index(m_gfx->GetTextureSRVGpuHandle(m_pdf));
    t.risIdx = index(m_gfx->GetBufferSRVGpuHandle(m_ris));
    t.pdfSize = kPdfSize; t.pdfMips = kPdfMips; t.tileSize = kTileSize; t.tileCount = kTileCount;
    return t;
}

void LightSamplingPass::ReadHistogram(uint32_t slot)
{
    // Chi-square of this frame's RIS draws against the PDF they were drawn
    // from (both written by the same frame's GPU work).
    const uint32_t* counts = m_mapped[slot];
    float pdf[kMaxLights];
    std::memcpy(pdf, counts + kMaxLights, sizeof(pdf));
    double chi2 = 0.0, maxRel = 0.0;
    uint32_t bins = 0, active = 0, zeroHits = 0, total = 0;
    for (uint32_t i = 0; i < kMaxLights; ++i)
    {
        total += counts[i];
        if (!(pdf[i] > 0.f)) { zeroHits += counts[i]; continue; }
        ++active;
        const double expected = double(kEntries) * pdf[i];
        if (expected < 5.0) continue;
        ++bins;
        const double d = double(counts[i]) - expected;
        chi2 += d * d / expected;
        maxRel = std::max(maxRel, std::abs(d) / expected);
    }
    LOG_INFO("[LIGHT_TABLES] frame=%u lights=%u active=%u entries=%u chi2=%.2f dof=%u chi2PerDof=%.4f maxRelErr=%.4f zeroHits=%u",
        m_readbackFrame[slot], m_lightCount, active, total, chi2, bins ? bins - 1 : 0u,
        bins > 1 ? chi2 / double(bins - 1) : 0.0, maxRel, zeroHits);
}

RHI::CommandList LightSamplingPass::Execute(RHI::CommandList cl)
{
    if (!m_active) return cl;
    IGraphicsDevice& gfx = cl.GetDevice();
    const uint32_t slot = gfx.GetFrameIndex();
    if (m_log && m_primed[slot] && m_frame % 30 == 0) ReadHistogram(slot);
    auto index = [&](uint64_t h) { return gfx.DescriptorHeapIndexFromGpuHandle(h); };
    Constants c{};
    c.lightsIdx = m_lightsIdx; c.lightCount = m_lightCount; c.frame = m_frame; c.pdfSize = kPdfSize;
    c.mipCount = kPdfMips; c.tileSize = kTileSize; c.tileCount = kTileCount; c.entries = kEntries;
    c.tableFlags = m_corrupt ? 1u : 0u;
    c.risUav = index(gfx.GetBufferUAVGpuHandle(m_ris));
    c.histogramUav = m_histogram.IsValid() ? index(gfx.GetBufferUAVGpuHandle(m_histogram)) : 0u;
    for (uint32_t m = 0; m < kPdfMips; ++m) c.pdfUav[m] = index(gfx.GetTextureMipUAVGpuHandle(m_pdf, m));
    auto& block = *m_cb.Current(gfx);
    for (uint32_t s = 0; s < kStages; ++s) { block.stages[s].values = c; block.stages[s].values.stage = s; }

    // A global UAV barrier: every stage reads what the one before it wrote.
    auto uavBarrier = [&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
    auto dispatch = [&](size_t pso, uint32_t stage, uint32_t x, uint32_t y) {
        gfx.BindComputePipelineState(m_pso[pso], cl);   // the PSO and its layout
        gfx.SetComputeRootCBV(kRootCB, m_cb.CurrentBuffer(gfx), stage * sizeof(Stage), cl);
        gfx.DispatchCompute(x, y, 1, cl);
    };
    const auto timer = gfx.BeginGPUTimestamp(cl, "LightTables");
    if (m_pdfState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_pdf, m_pdfState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_pdfState = RHI::ResourceState::UNORDERED_ACCESS;
    if (m_risState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_ris, m_risState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_risState = RHI::ResourceState::UNORDERED_ACCESS;
    dispatch(0, 0, kPdfSize / 8, kPdfSize / 8); uavBarrier();
    for (uint32_t m = 1; m < kPdfMips; ++m)
    {
        const uint32_t size = std::max(kPdfSize >> m, 1u);
        dispatch(1, m, (size + 7) / 8, (size + 7) / 8); uavBarrier();
    }
    dispatch(2, kPdfMips, kEntries / 64, 1); uavBarrier();
    if (m_histogram.IsValid())
    {
        if (m_histogramState != RHI::ResourceState::UNORDERED_ACCESS)
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_histogram, m_histogramState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_histogramState = RHI::ResourceState::UNORDERED_ACCESS;
        dispatch(3, kPdfMips + 1, kMaxLights / 64, 1); uavBarrier();
        dispatch(4, kPdfMips + 1, kEntries / 64, 1); uavBarrier();
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_histogram, m_histogramState, RHI::ResourceState::COPY_SRC), cl);
        m_histogramState = RHI::ResourceState::COPY_SRC;
        gfx.CopyBuffer(m_histogram, m_readback[slot], 2ull * kMaxLights * 4, cl);
        m_primed[slot] = true; m_readbackFrame[slot] = m_frame;
    }
    // Consumers read both as SRVs.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_pdf, m_pdfState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_pdfState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_ris, m_risState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_risState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    gfx.EndGPUTimestamp(cl, timer);
    return cl;
}
