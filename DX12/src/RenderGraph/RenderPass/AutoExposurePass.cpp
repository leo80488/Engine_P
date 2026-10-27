#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ViewSlot.h"   // a view's staging float, the main view's readback
#include "Settings/PassSettings.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include <algorithm>
#include <cstring>
#include <cmath>

// Aliases of the shared compute root signature (Graphics/RootSignatureLayout.h).
static constexpr auto kCBSlot      = RHI::Slots::Cmp::CB;
static constexpr auto kSRV0        = RHI::Slots::Cmp::SRV_t0;  // HDR SRV  (histogram build only)
static constexpr auto kSRV1        = RHI::Slots::Cmp::SRV_t1;  // GBuffer normal/material id
static constexpr auto kSRV2        = RHI::Slots::Cmp::SRV_t2;  // material StructuredBuffer
static constexpr auto kSRV3        = RHI::Slots::Cmp::SRV_t3;  // reverse-Z depth
static constexpr auto kUAV0        = RHI::Slots::Cmp::UAV_u0;  // histogram UAV
static constexpr auto kUAV1        = RHI::Slots::Cmp::UAV_u1;  // exposure UAV
static constexpr auto kUAV2        = RHI::Slots::Cmp::UAV_u2;  // exposure texture UAV

static constexpr uint32_t kHistBins = 256;
// The histogram buffer holds one more uint after the bins: the frame's count of
// metered pixels with a non-finite luma (AutoExposure.cs.hlsl NONFINITE_SLOT).
static constexpr uint32_t kHistSlots = kHistBins + 1;
// The exposure buffer: [0] the exposure, [1] that count as a float.
static constexpr uint32_t kExposureSlots = 2;

void AutoExposurePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::HistogramBuild_CS,   RHI::ShaderStage::CS,
                         "AutoExposure.cs.hlsl", "CSHistogramBuild");
    m_shaderLib.Register(ShaderID::HistogramAverage_CS, RHI::ShaderStage::CS,
                         "AutoExposure.cs.hlsl", "CSHistogramAverage");

    if (!CreatePipelines(gfx)) return;

    // Histogram: 256 bins + the non-finite count, uint (UAV only)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kHistSlots * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;

        uint32_t zeros[kHistSlots] = {0};
        if (!gfx.CreateBuffer(bd, m_histogramBuffer, zeros))
            LOG_ERROR("AutoExposurePass: histogram buffer failed");
        else
            // DEFAULT buffers created with initial data leave the upload path
            // in VERTEX_AND_CONSTANT_BUFFER when they have no SRV flag.
            m_histogramState = RHI::ResourceState::VERTEX_BUFFER;
    }

    CreateExposureBuffer(m_initExposure, m_initExposureState);

    // Per-dispatch CB
    m_cb.Create(gfx, "AutoExposure.CB");

    // Manual-exposure staging, one per frame in flight: the disabled path's CopyBuffer reads one each
    // frame, so the next CPU write must not hit a buffer the GPU is still copying. A float per view: a
    // later view's write in the same frame must not reach the copy an earlier view recorded.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(float) * RHI::kMaxViews;
        bd.stride     = sizeof(float);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_manualStaging[i]))
                m_manualStagingMapped[i] = gfx.MapBuffer(m_manualStaging[i]);
        }
    }

    // Debug readback ring: GPU -> CPU copies of the exposure buffer (the exposure and the non-finite
    // count) for the editor HUD and the harness. Allocated up front (8 bytes x frames), written only
    // while a caller keeps renewing the readback TTL.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kExposureSlots * sizeof(float);
        bd.stride     = sizeof(float);
        bd.usage      = RHI::Usage::READBACK;
        bd.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_readback[i]))
                m_readbackMapped[i] = gfx.MapBuffer(m_readback[i]);
        }
    }

    // The histogram's readback ring: allocated with the exposure's, filled only while the readout is on.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kHistSlots * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::READBACK;
        bd.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_histReadback[i]))
                m_histReadbackMapped[i] = gfx.MapBuffer(m_histReadback[i]);
        }
    }

    LOG_SUCCESS("AutoExposurePass: initialized");
}

// Onto the live members: a failed rebuild keeps the old pipeline.
bool AutoExposurePass::CreatePipelines(IGraphicsDevice& gfx)
{
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HistogramBuild_CS);
        if (!cs) { LOG_ERROR("AutoExposurePass: HistogramBuild_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_buildPSO))
            LOG_ERROR("AutoExposurePass: build PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::HistogramAverage_CS);
        if (!cs) { LOG_ERROR("AutoExposurePass: HistogramAverage_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_averagePSO))
            LOG_ERROR("AutoExposurePass: average PSO failed");
    }
    return true;
}

void AutoExposurePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

// Exposure: 2 x float, [0] the exposure ToneMap reads, [1] the non-finite count (UAV + SRV).
bool AutoExposurePass::CreateExposureBuffer(RHI::GPUBuffer& out, RHI::ResourceState& state)
{
    RHI::GPUBufferDesc bd{};
    bd.size       = kExposureSlots * sizeof(float);
    bd.stride     = sizeof(float);
    bd.usage      = RHI::Usage::DEFAULT;
    bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;

    float initialExposure[kExposureSlots] = { 1.0f, 0.0f };
    if (!m_gfx->CreateBuffer(bd, out, initialExposure))
    {
        LOG_ERROR("AutoExposurePass: exposure buffer failed");
        return false;
    }
    // CreateBuffer(initialData) transitions SRV-capable DEFAULT buffers
    // to the combined shader-resource state after its upload copy.
    state = RHI::ResourceState::SHADER_RESOURCE;
    return true;
}

void AutoExposurePass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
    if (m_hist->exposure.IsValid()) return;
    // The first view adopts the buffer Init made, so its allocation point does not move.
    if (m_initExposure.IsValid())
    {
        m_hist->exposure      = m_initExposure;
        m_hist->exposureState = m_initExposureState;
        m_initExposure        = {};
    }
    // A later view gets its own (Renderer::ServiceViews binds it on the main thread first), and its
    // first measurement is its exposure: there is nothing to adapt from.
    else if (m_gfx && CreateExposureBuffer(m_hist->exposure, m_hist->exposureState))
        m_hist->snap = true;
}

void AutoExposurePass::History::Release(IGraphicsDevice& gfx)
{
    if (exposure.IsValid()) gfx.DestroyBuffer(exposure);
    exposure = {};
    if (exposureTex.IsValid()) gfx.DestroyTexture(exposureTex);
    exposureTex = {};
}

bool AutoExposurePass::CreateExposureTexture(History& h)
{
    RHI::TextureDesc td{};
    td.width     = 1;
    td.height    = 1;
    td.format    = RHI::Format::R32_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage     = RHI::Usage::DEFAULT;
    td.layout    = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    td.debugName = "AutoExposure.ExposureTex";
    const float one = 1.0f;
    const RHI::SubresourceData initial{ &one, sizeof(float), sizeof(float) };
    if (!m_gfx->CreateTexture(td, h.exposureTex, &initial))
    {
        LOG_ERROR("AutoExposurePass: exposure texture failed");
        return false;
    }
    h.exposureTexState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    return true;
}

bool AutoExposurePass::ReadExposureTexture(float& value, std::string& error)
{
    if (!m_gfx || !m_hist || !m_hist->exposureTex.IsValid())
    {
        error = "no exposure texture: no upscaler takes the engine's exposure";
        return false;
    }
    m_gfx->DrainCpuFrame();   // the tracked state is the render lane's until then
    IGraphicsDevice::RawTexture raw;
    if (!m_gfx->ReadTextureRaw(m_hist->exposureTex, m_hist->exposureTexState, 0, raw))
    {
        error = raw.error;
        return false;
    }
    if (raw.bytes.size() < sizeof(float))
    {
        error = "the exposure texture read back empty";
        return false;
    }
    std::memcpy(&value, raw.bytes.data(), sizeof(float));
    return true;
}

void AutoExposurePass::OnPrepare(const RG::PrepareContext& c)
{
    if (!c.upscaler || !m_gfx || !m_hist || m_hist->exposureTex.IsValid()) return;
    if (!Settings::Pass::upscalerEngineExposure()) return;
    CreateExposureTexture(*m_hist);
}

void AutoExposurePass::RecordExport(RHI::CommandList cl, History& h)
{
    if (!h.exposureTex.IsValid() || !m_averagePSO.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;
    auto* slot = m_cb.Current(gfx);
    if (!slot) return;
    AutoExposureCB cb{};
    cb.debugFlags = kFlagExportOnly;
    *slot = cb;

    if (h.exposureState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&h.exposure, h.exposureState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        h.exposureState = RHI::ResourceState::UNORDERED_ACCESS;
    }
    gfx.PushBarrier(RHI::GPUBarrier::Image(&h.exposureTex, h.exposureTexState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    gfx.BindComputePipelineState(m_averagePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    // The histogram table is not read in this mode; a buffer in a legal state stands in for it.
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(h.exposure), cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetBufferUAVGpuHandle(h.exposure), cl);
    gfx.SetComputeDescriptorTable(kUAV2, gfx.GetTextureUAVGpuHandle(h.exposureTex), cl);
    gfx.DispatchCompute(1, 1, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(&h.exposureTex, RHI::ResourceState::UNORDERED_ACCESS,
                                           RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    h.exposureTexState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&h.exposure, RHI::ResourceState::UNORDERED_ACCESS,
                                            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    h.exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
}

float AutoExposurePass::GetLastExposureEV() const
{
    return (m_lastExposure > 1e-6f) ? std::log2(m_lastExposure) : 0.0f;
}

void AutoExposurePass::PrepareForCompute(RHI::CommandList graphicsCl)
{
    if (!m_gfx || !graphicsCl.IsValid() || !m_hist || !m_hist->exposure.IsValid()) return;
    History& h = *m_hist;
    if (h.exposureState == RHI::ResourceState::SHADER_RESOURCE_COMPUTE) return;

    m_gfx->PushBarrier(RHI::GPUBarrier::Buffer(
        &h.exposure, h.exposureState,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), graphicsCl);
    h.exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
}

// Optional debug readback, folded into the transition that happens anyway:
// UAV -> COPY_SRC -> SHADER_RESOURCE_COMPUTE costs one extra barrier and an 8-byte copy, only while
// the editor panel is open or the harness records frames. The main view's exposure only. A frame that
// holds the exposure copies it too, so the readout follows a snap that lands in a held frame.
void AutoExposurePass::RecordReadback(RHI::CommandList cl, History& h, bool primary)
{
    IGraphicsDevice& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    const bool wantReadback = primary && (m_debugReadbackTtl > 0u);
    if (primary && m_debugReadbackTtl > 0u) --m_debugReadbackTtl;
    if (!wantReadback || !m_readbackMapped[frameSlot]) return;

    gfx.PushBarrier(RHI::GPUBarrier::Buffer(
        &h.exposure, h.exposureState, RHI::ResourceState::COPY_SRC), cl);
    gfx.CopyBuffer(h.exposure, m_readback[frameSlot], kExposureSlots * sizeof(float), cl);
    h.exposureState = RHI::ResourceState::COPY_SRC;
    m_readbackPrimed[frameSlot] = true;

    // Read the oldest slot, kFrameCount - 1 frames back: the GPU has retired that copy, no fence
    // needed.
    const uint32_t oldest = (frameSlot + 1u) % kFrameCount;
    if (m_readbackPrimed[oldest] && m_readbackMapped[oldest])
    {
        float v[kExposureSlots] = {};
        std::memcpy(v, m_readbackMapped[oldest], sizeof(v));
        if (std::isfinite(v[0]) && v[0] > 0.0f) m_lastExposure = v[0];
        // The count is written as a float value (exact below 2^24).
        if (std::isfinite(v[1]) && v[1] >= 0.0f) m_lastNonFinite = static_cast<int64_t>(v[1]);
    }
}

void AutoExposurePass::RecordHistogramReadback(RHI::CommandList cl, float minLog, float maxLog)
{
    IGraphicsDevice& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (!m_histReadbackMapped[frameSlot]) return;

    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_histogramBuffer, RHI::ResourceState::UNORDERED_ACCESS,
                                            RHI::ResourceState::COPY_SRC), cl);
    gfx.CopyBuffer(m_histogramBuffer, m_histReadback[frameSlot], kHistSlots * sizeof(uint32_t), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_histogramBuffer, RHI::ResourceState::COPY_SRC,
                                            RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_histPrimed[frameSlot] = true;

    // The oldest slot, kFrameCount - 1 frames back: its copy has retired.
    const uint32_t oldest = (frameSlot + 1u) % kFrameCount;
    if (m_histPrimed[oldest] && m_histReadbackMapped[oldest])
    {
        std::memcpy(m_lastHistogram.data(), m_histReadbackMapped[oldest], kHistBins * sizeof(uint32_t));
        m_lastHistMinLog = minLog;
        m_lastHistMaxLog = maxLog;
        m_lastHistogramValid = true;
    }
}

RHI::CommandList AutoExposurePass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_hist || !m_hist->exposure.IsValid()) return cl;
    History& h = *m_hist;

    IGraphicsDevice& gfx = *m_gfx;

    // NPRStabilizePass bound the exposure SRV at t5 earlier on this shared command list, and the
    // compute root signature's ranges are DATA_STATIC_WHILE_SET_AT_EXECUTE: detach it before this pass
    // moves the exposure buffer to COPY_DST / UAV. The histogram shader does not read t5, so the HDR
    // SRV is a valid neutral stand-in. Both paths below re-state the buffer, so this precedes the
    // enabled / disabled split. No pipeline is bound yet: this relies on the shared compute layout
    // being bound, which every private-layout dispatch before it puts back (FSR 2, DLSS, the RR
    // passes - IGraphicsDevice::RestoreDefaultComputeRootSignature).
    if (m_hdrSrvHandle)
        gfx.SetComputeDescriptorTable(RHI::Slots::Cmp::SRV_t5, m_hdrSrvHandle, cl);

    // ---- Disabled path: upload the manual exposure into the buffer ToneMap reads and return; no
    // histogram dispatches, only a 4-byte copy.
    // Ahead of the PSO check on purpose: this path needs no PSO, so a histogram shader that failed to
    // compile must not take manual exposure down and leave ToneMap reading a stale buffer.
    const bool primary = RHI::CurrentRecordingView().slot == 0;
    if (!m_enabled)
    {
        const uint32_t frameSlot = gfx.GetFrameIndex();
        const uint64_t offset    = sizeof(float) * RHI::CurrentRecordingView().slot;
        // The copy is skipped, not just the memcpy: copying from an upload
        // buffer that was never mapped would push uninitialised memory into
        // the exposure value.
        const float manual = m_legacyBias ? m_manualExposure : m_manualExposure * std::exp2(m_evBias);
        if (m_manualStagingMapped[frameSlot])
        {
            std::memcpy(static_cast<uint8_t*>(m_manualStagingMapped[frameSlot]) + offset, &manual,
                        sizeof(float));

            if (h.exposureState != RHI::ResourceState::COPY_DST)
            {
                gfx.PushBarrier(RHI::GPUBarrier::Buffer(
                    &h.exposure, h.exposureState, RHI::ResourceState::COPY_DST), cl);
                h.exposureState = RHI::ResourceState::COPY_DST;
            }
            gfx.CopyBufferRegion(m_manualStaging[frameSlot], offset, h.exposure, 0, sizeof(float), cl);

            gfx.PushBarrier(RHI::GPUBarrier::Buffer(
                &h.exposure, RHI::ResourceState::COPY_DST,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
            h.exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        }
        RecordExport(cl, h);
        // Manual exposure is a CPU value; report it directly rather than
        // paying for a readback to learn what we just uploaded. The main view's.
        if (primary)
        {
            m_lastExposure = manual;
            m_lastNonFinite = -1;   // nothing was metered
        }
        // A manual frame leaves no adapted history worth keeping.
        h.snap = true;
        return cl;
    }

    if (!m_buildPSO.IsValid() || !m_averagePSO.IsValid()) return cl;
    if (m_vpW == 0 || m_vpH == 0 || m_hdrSrvHandle == 0) return cl;

    // Held: keep the exposure as it is. A pending snap still meters once.
    if (IsHolding() && !h.snap)
    {
        RecordReadback(cl, h, primary);
        if (h.exposureState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&h.exposure, h.exposureState,
                                                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
            h.exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        }
        RecordExport(cl, h);
        return cl;
    }

    // Weighted metering needs scene metadata.  These are stable graph/material
    // resources in normal rendering; keep the prior exposure if a capture/debug
    // path invokes the pass without them instead of binding incomplete roots.
    if (!m_normalSrv || !m_depthSrv || !m_materialSrv) return cl;

    // Guard the divide. AutoExposureEffect sanitizes the pair, but this pass is
    // also driven directly (tools, capture paths) and an inverted or degenerate
    // range produces Inf here, which locks the meter on one constant forever.
    const float logRange = (m_maxLogLuma - m_minLogLuma >= 0.1f)
                         ? (m_maxLogLuma - m_minLogLuma) : 0.1f;
    const float maxLogLuma = m_minLogLuma + logRange;

    // Snap: full rate, so a level load or a hard cut lands on the new exposure immediately instead of
    // dragging through the normal tau - and every frame until the scene has settled, so it ends on the
    // streamed picture's exposure (kSnapWaitFrames at most).
    const bool  snapping   = h.snap;
    const float rateBright = snapping ? 1.0f : m_adaptationRate;
    const float rateDark   = snapping ? 1.0f : m_adaptationRateDark;
    if (snapping)
    {
        if (m_sceneSettled || h.snapWait + 1 >= kSnapWaitFrames)
        {
            LOG_INFO("[AE_SNAP] view=%u waited=%u settled=%d", RHI::CurrentRecordingView().slot, h.snapWait,
                     m_sceneSettled ? 1 : 0);
            h.snap = false;
            h.snapWait = 0;
        }
        else
            ++h.snapWait;
    }

    if (auto* slot = m_cb.Current(gfx))
    {
        AutoExposurePass::AutoExposureCB cb{};
        cb.width           = m_vpW;
        cb.height          = m_vpH;
        cb.minLogLuma      = m_minLogLuma;
        cb.invLogLumaRange = 1.0f / logRange;
        cb.adaptationRate  = rateBright;
        cb.lowPercent      = m_lowPercent;
        cb.highPercent     = m_highPercent;
        cb.minExposure     = m_minExposure;
        cb.maxExposure     = m_maxExposure;
        cb.evBias          = m_evBias;
        cb.keyValue        = m_keyValue;
        cb.centerWeight    = m_centerWeight;
        cb.skyWeight       = m_skyWeight;
        cb.nprWeight       = m_nprWeight;
        cb.highlightWeight = m_highlightWeight;
        cb.adaptationRateDark = rateDark;
        // Dispatch-uniform values hoisted off the per-pixel path.
        cb.hiStart         = std::exp2(maxLogLuma - 2.0f);
        cb.hiEnd           = std::exp2(maxLogLuma);
        cb.metaWidth       = m_metaW ? m_metaW : m_vpW;
        cb.metaHeight      = m_metaH ? m_metaH : m_vpH;
        cb.materialCount   = m_materialCount ? m_materialCount : 1u;
        cb.yStride         = m_yStride ? m_yStride : 1u;
#if defined(WITH_HARNESS) && WITH_HARNESS
        // -ae_inject_nan=<n>: the first n columns of every sampled row meter as NaN - rejected from the
        // histogram like a real NaN and counted, so frames.jsonl nanPixels must move.
        static const uint32_t s_injectNaN = static_cast<uint32_t>(
            (std::max)(0, Sys::CommandLine::Get().GetInt("-ae_inject_nan", 0)));
        cb.debugNonFinite  = s_injectNaN;
#endif
        cb.debugFlags      = (m_legacyBias ? kFlagLegacyBias : 0u) | (h.exposureTex.IsValid() ? kFlagExportTexture : 0u);
        *slot = cb;
    }

    // The initial-data upload leaves this UAV-only buffer in
    // VERTEX_AND_CONSTANT_BUFFER; make the first dispatch legal, then retain
    // UAV state across frames (the average kernel clears every bin).
    if (m_histogramState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(
            &m_histogramBuffer, m_histogramState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_histogramState = RHI::ResourceState::UNORDERED_ACCESS;
    }
    else
    {
        // Ensure histogram clears from last frame are visible to this build.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_histogramBuffer), cl);
    }

    if (h.exposureState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&h.exposure, h.exposureState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        h.exposureState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // Own GPU timestamp zone, separate from the PostProcessStack region.
    const uint32_t gpuZone = gfx.BeginGPUTimestamp(cl, "AutoExposure");

    // --- Histogram Build ---
    gfx.BindComputePipelineState(m_buildPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, cbBuf, cl);
    gfx.SetComputeDescriptorTable(kSRV0,  m_hdrSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kSRV1,  m_normalSrv, cl);
    gfx.SetComputeDescriptorTable(kSRV2,  m_materialSrv, cl);
    gfx.SetComputeDescriptorTable(kSRV3,  m_depthSrv, cl);
    gfx.SetComputeDescriptorTable(kUAV0,  gfx.GetBufferUAVGpuHandle(m_histogramBuffer), cl);
    // Each group is 16 wide and covers 16 sampled rows (16 x yStride source rows). x is not strided:
    // a wave already reads contiguous cache lines, so striding x would fetch the same lines for fewer
    // samples.
    const uint32_t rowsPerGroup = 16u * (m_yStride ? m_yStride : 1u);
    gfx.DispatchCompute((m_vpW + 15) / 16,
                        (m_vpH + rowsPerGroup - 1) / rowsPerGroup, 1, cl);

    // UAV barrier before average
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_histogramBuffer), cl);
    if (primary && m_debugReadbackTtl > 0u) RecordHistogramReadback(cl, m_minLogLuma, maxLogLuma);
    // --- Histogram Average ---
    gfx.BindComputePipelineState(m_averagePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, cbBuf, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(m_histogramBuffer), cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetBufferUAVGpuHandle(h.exposure),  cl);
    const bool exportTex = h.exposureTex.IsValid();
    if (exportTex)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&h.exposureTex, h.exposureTexState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        gfx.SetComputeDescriptorTable(kUAV2, gfx.GetTextureUAVGpuHandle(h.exposureTex), cl);
    }
    gfx.DispatchCompute(1, 1, 1, cl);

    // UAV barrier on exposure so ToneMap can read it safely
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&h.exposure), cl);
    if (exportTex)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&h.exposureTex, RHI::ResourceState::UNORDERED_ACCESS,
                                               RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        h.exposureTexState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    RecordReadback(cl, h, primary);

    // Transition exposure buffer to SRV for ToneMapPass
    if (h.exposureState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&h.exposure, h.exposureState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        h.exposureState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    gfx.EndGPUTimestamp(cl, gpuZone);
    return cl;
}
