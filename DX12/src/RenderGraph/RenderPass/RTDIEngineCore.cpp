#include "RenderGraph/RenderPass/RTDIEngineCore.h"
#include "Settings/PassSettings.h"     // pass.rtdi.spatialRadius2
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include <algorithm>
#include <chrono>
#include <iterator>

namespace RTDI
{
EngineCore::EngineCore(uint32_t shellFlags) : m_shellFlags(shellFlags)
{
    const auto& cmd = Sys::CommandLine::Get();
    // The spatial passes and radius, the M cap, reuse and the candidates are engine.json rows
    // (-di_* command-line values pin them): ApplySettings brings them; the defaults hold until
    // then.
    m_secondRadius = static_cast<uint32_t>(std::clamp(
        Settings::Pass::rtdiSpatialRadius2Given() ? Settings::Pass::rtdiSpatialRadius2() : 0, 0, 64));
    // Reuse (temporal + spatial resampling) is the ReSTIR path proper and is on
    // by default; engine.json restirDiReuse chooses it at runtime.
    // -di_visibility_confidence turns it on unless the setting is a
    // command-line "off" (ApplySettings).
    m_visibilityConfidence = cmd.Has("-di_visibility_confidence");
    // RTXDI feeds the post-spatial (shaded) reservoir back as next frame's history, and so does
    // this core; -di_no_spatial_feedback keeps the temporal-only history instead.
    m_feedback = !cmd.Has("-di_no_spatial_feedback");
    m_boilingFilter = !cmd.Has("-di_no_boiling_filter");
    // RTXDI StoreVisibilityInDIReservoir(discardIfInvisible) on the carried
    // reservoir, and enableInitialVisibility on the Initial winner. Both keep M
    // and remove only the occluded sample; on by default with reuse.
    m_discardInvisible = !cmd.Has("-di_keep_invisible");
    m_initialVisibility = !cmd.Has("-di_no_initial_visibility");
    // Permutation sampling is an opt-in A/B: no better for the NRD output on the occluded
    // multi-light fixture, and it defeats the same-receiver shortcut of the visibility-aware
    // temporal merge.
    m_permutation = cmd.Has("-di_permutation");
    // Area-lite: an opt-in experiment, not adopted (off by default).
    m_areaLite = cmd.Has("-di_area_lite");
    // Visibility-aware reuse (p_hat*V targets in every MIS term) is the default;
    // -di_unshadowed_reuse switches to RTXDI's default unshadowed-target reuse.
    m_visibilityMIS = !cmd.Has("-di_unshadowed_reuse");
    m_inverseVisibility = !cmd.Has("-di_no_inverse_visibility");
    // The integrated signal is scratch only, never history.
    if (m_shellFlags & 4096u) m_feedback = false;
    // Initial candidates: engine.json restirDiCandidates (ApplySettings).
    // -di_debug=1 (calibration): one candidate, no reuse.
    if (std::clamp(cmd.GetInt("-di_debug", 0), 0, 3) == 1)
    {
        m_candidatesOverride = 1;
        m_candidates = 1;
        m_reuseOverride = 0;
    }
    m_reuse = m_reuseOverride >= 0 ? m_reuseOverride != 0 : (m_visibilityConfidence || m_reuseRequested);
}

bool EngineCore::BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell)
{
    if (m_pso.back().IsValid()) return true;
    if (m_pipelinesFailed || !shell.IsValid()) return false;
    const auto start = std::chrono::steady_clock::now();
    static constexpr const char* kEntries[] = {"Initial", "Temporal", "Spatial", "Shade"};
    static_assert(std::size(kEntries) == std::tuple_size_v<decltype(m_pso)>);
    // Built concurrently (ComputePipelineBatch), all or nothing.
    if (!ComputePipelineBatch::BuildEntries(gfx, shell, "shaders/RTDI.cs.hlsl", kEntries, m_pso.size(),
                                            m_pso.data(), "RTDIPass"))
    {
        m_pipelinesFailed = true;
        return false;
    }
    m_pipelineMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

void EngineCore::ReleasePipelines(IGraphicsDevice& gfx)
{
    if (!m_adopted)
        for (auto& p : m_pso) gfx.DestroyPipelineState(p);
    m_pso = {};
    m_adopted = false;
    m_pipelinesFailed = false;
}

void EngineCore::AdoptPipelines(const Core& from)
{
    m_pso = static_cast<const EngineCore&>(from).m_pso;
    m_adopted = true;
}

bool EngineCore::Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH)
{
    m_tw = traceW; m_th = traceH; m_allocated = true;
    if (!m_reuse) return true;
    RHI::GPUBufferDesc bd{};
    bd.usage = RHI::Usage::DEFAULT;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    bd.stride = sizeof(ReservoirStorage); bd.size = uint64_t(m_tw) * m_th * bd.stride;
    // Three rotating reservoirs (history, initial/spatial, temporal); a
    // fourth only when a second spatial stage must not overwrite a
    // temporal-only history.
    for (uint32_t i = 0; i < (m_spatialPasses == 2 && !m_feedback ? 4u : 3u); ++i)
        if (!gfx.CreateBuffer(bd, m_reservoir[i])) { LOG_ERROR("RTDIPass: reservoir allocation failed"); m_allocated = false; return false; }
    bd.stride = 16; bd.size = uint64_t(m_tw) * m_th * bd.stride;
    for (auto& b : m_receiver)
        if (!gfx.CreateBuffer(bd, b)) { LOG_ERROR("RTDIPass: receiver history allocation failed"); m_allocated = false; return false; }
    m_reservoirState.fill(RHI::ResourceState::UNDEFINED);
    m_receiverState.fill(RHI::ResourceState::UNDEFINED);
    m_historySlot = 0; m_receiverSlot = 0;
    return true;
}

void EngineCore::Release(IGraphicsDevice& gfx)
{
    m_historyPhase = m_phase = 0;
    for (auto& b : m_reservoir) { if (b.IsValid()) gfx.DestroyBuffer(b); b = {}; }
    for (auto& b : m_receiver) { if (b.IsValid()) gfx.DestroyBuffer(b); b = {}; }
    m_allocated = false;
}

bool EngineCore::ApplySettings(const CoreSettings& s, float distance, bool distanceChanged, bool& historyReset)
{
    // The settings carry the -di_* command-line pins; rules that apply only to a pinned value read
    // the pin bits.
    uint32_t p = std::min(s.spatialPasses, 2u);
    if (m_shellFlags & 4096u) p = 1; // The visibility-integration diagnostic has one stage.
    // Two command-line passes default the radius to 8 unless it is a
    // command-line value too.
    const uint32_t radiusDefault = s.spatialPassesPinned && !s.spatialRadiusPinned && p == 2 ? 8u : s.spatialRadius;
    const uint32_t r = std::min(radiusDefault, 64u);
    const uint32_t r2 = static_cast<uint32_t>(std::clamp(
        Settings::Pass::rtdiSpatialRadius2Given() ? Settings::Pass::rtdiSpatialRadius2() : static_cast<int>(r), 0, 64));
    const uint32_t k = std::clamp(s.spatialK, 1u, 8u);
    const uint32_t m = std::clamp(s.maxM, 1u, 63u);
    const uint32_t age = std::clamp(s.maxAge, 1u, 63u);
    m_reuseRequested = s.reuse;
    // -di_visibility_confidence turns reuse on unless the setting is a command-line "off" (a
    // pinned off wins over it).
    const bool requested = (m_visibilityConfidence && !(s.reusePinned && !s.reuse)) || m_reuseRequested;
    const bool wantReuse = m_reuseOverride >= 0 ? m_reuseOverride != 0 : requested;
    const bool reuseChanged = wantReuse != m_reuse;
    // Reservoir and receiver buffers exist only with reuse; the shell's
    // OnCompile (re)builds them at the same service point as a spatial-pass change.
    bool realloc = reuseChanged;
    historyReset = false;
    if (distanceChanged || p != m_spatialPasses || r != m_spatialRadius || r2 != m_secondRadius
        || k != m_spatialK || m != m_maxM || age != m_maxAge || reuseChanged)
    {
        if (wantReuse && !m_feedback && (p == 2) != (m_spatialPasses == 2)) realloc = true;
        historyReset = true;
        LOG_INFO("[RTDI_SETTINGS] reuse=%u distance=%.3f passes=%u radius=%u radius2=%u k=%u mCap=%u ageCap=%u feedback=%u discardInvisible=%u initialVisibility=%u boiling=%u areaLite=%u",
            wantReuse ? 1u : 0u, distance, p, r, r2, k, m, age, m_feedback ? 1u : 0u, m_discardInvisible ? 1u : 0u,
            m_initialVisibility ? 1u : 0u, m_boilingFilter ? 1u : 0u, m_areaLite ? 1u : 0u);
    }
    m_reuse = wantReuse;
    m_spatialPasses = p; m_spatialRadius = r; m_secondRadius = r2;
    m_spatialK = k; m_maxM = m; m_maxAge = age;
    // A candidate count is a constant of the next Initial: no history reset.
    const uint32_t candidates = m_candidatesOverride >= 0 ? static_cast<uint32_t>(m_candidatesOverride)
        : std::clamp(s.candidates, 1u, 32u);
    if (candidates != m_candidates)
    {
        m_candidates = candidates;
        LOG_INFO("[RTDI_SETTINGS] candidates=%u", m_candidates);
    }
    return realloc;
}

void EngineCore::FillConstants(IGraphicsDevice& gfx, Constants& cb, const FillContext& ctx)
{
    auto index = [&](uint64_t h) { return gfx.DescriptorHeapIndexFromGpuHandle(h); };
    m_fillFlags = ctx.shellFlags;
    m_phase = ctx.phase;
    m_jitterX = ctx.jitterX; m_jitterY = ctx.jitterY;
    // Budget ladder: stage 1 = one spatial candidate, stage 2 = no spatial.
    cb.flags |= (m_budgetStage >= 1 ? 8192u : 0u) | (m_budgetStage >= 2 ? 16u : 0u);
    if (ctx.shellFlags & 8u)
    {
        // The final (post-spatial) selection traces the one visibility ray.
        // Temporal still traces when its output is the history the
        // visibility-confidence path re-tests (feedback off).
        if (m_spatialPasses > 0 && (m_feedback || !(ctx.shellFlags & (1u << 24)))) cb.flags |= 1u << 25;
        if (m_discardInvisible) cb.flags |= 1u << 26;
        if (m_initialVisibility) cb.flags |= 1u << 29;
        // Permutation sampling: -di_permutation, and always under ray reconstruction at full
        // resolution (flags2 bit 3, shell flag 4). The temporal reuse's history is correlated frame
        // to frame and RR keeps that as detail: at full-resolution DI permutation cut the flickering
        // pixels 79% and halved the highlight cv99. At half resolution it made RR worse, so not there.
        if (m_permutation || ((cb.flags2 & 8u) && (ctx.shellFlags & 4u))) cb.flags |= 1u << 30;
        if (m_visibilityMIS)
        {
            cb.flags |= (1u << 31) | (1u << 29); // requires Initial visibility
            cb.flags &= ~(1u << 26);             // nothing occluded is ever selected
            if (m_inverseVisibility) cb.flags |= 1u << 15;
        }
        if (m_boilingFilter && m_feedback) cb.flags |= 1u << 28;
    }
    cb.areaLite = (m_areaLite && (ctx.shellFlags & 8u)) ? 1u : 0u;
    if (!(ctx.shellFlags & 16384u))
    { cb.historyJitterDelta[0] = m_historyJitterX - m_jitterX; cb.historyJitterDelta[1] = m_historyJitterY - m_jitterY; }
    // A history texel q was traced at full pixel 2q+previousPhase; today's at
    // 2tp+phase. The previous trace coordinate gains (phase-previousPhase)/2,
    // folded here into the full-pixel delta RTPreviousTraceCoordinate divides.
    cb.historyJitterDelta[0] += float(int(m_phase & 1u) - int(m_historyPhase & 1u));
    cb.historyJitterDelta[1] += float(int((m_phase >> 1) & 1u) - int((m_historyPhase >> 1) & 1u));
    if ((ctx.shellFlags & 8u) && Sys::CommandLine::Get().Has("-restir_reprojection_log"))
        LOG_INFO("[RESTIR_REPROJECT] pass=RTDI frame=%u currentX=%.6f currentY=%.6f previousX=%.6f previousY=%.6f deltaX=%.6f deltaY=%.6f reset=%u",
            ctx.frame, m_jitterX, m_jitterY, m_historyJitterX, m_historyJitterY, cb.historyJitterDelta[0], cb.historyJitterDelta[1], cb.historyReset);
    cb.candidates = m_candidates;
    // Reservoir rotation: H = last frame's shaded reservoir (the history), X =
    // Initial output and first spatial output, Y = Temporal output. The
    // reservoir Shade reads becomes the next H, so spatial reuse compounds
    // across frames as in RTXDI. Receivers ping-pong independently.
    m_H = m_historySlot % 3u; m_X = (m_H + 1u) % 3u; m_Y = (m_H + 2u) % 3u;
    m_secondStage = (ctx.shellFlags & 8u) && m_spatialPasses == 2;
    m_second = m_feedback ? m_Y : 3u;
    m_shaded = m_spatialPasses == 0 ? m_Y : (m_secondStage ? m_second : m_X);
    if (ctx.shellFlags & 8u)
    {
        const uint32_t current = m_receiverSlot, previous = current ^ 1u;
        cb.initialUav = index(gfx.GetBufferUAVGpuHandle(m_reservoir[m_X]));
        cb.initialSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_X]));
        cb.previousSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_H]));
        cb.temporalUav = index(gfx.GetBufferUAVGpuHandle(m_reservoir[m_Y]));
        cb.temporalSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_Y]));
        cb.previousSurfaceSrv = index(gfx.GetBufferSRVGpuHandle(m_receiver[previous]));
        cb.surfaceUav = index(gfx.GetBufferUAVGpuHandle(m_receiver[current]));
        cb.historyReset = ctx.reset; cb.maxM = m_maxM; cb.maxAge = m_maxAge; cb.spatialCandidates = m_spatialK; cb.spatialRadius = m_spatialRadius;
        cb.historyWidth = ctx.traceW; cb.historyHeight = ctx.traceH;
    }
}

void EngineCore::PatchStages(IGraphicsDevice& gfx, ConstantBlock& block)
{
    auto index = [&](uint64_t h) { return gfx.DescriptorHeapIndexFromGpuHandle(h); };
    if (m_fillFlags & 8u) block.stages[2].values.initialSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_shaded]));
    if (m_secondStage)
    {
        auto& stage = block.stages[1].values;
        stage.stageIndex = 1; stage.spatialRadius = m_secondRadius;
        stage.temporalSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_X]));
        stage.initialUav = index(gfx.GetBufferUAVGpuHandle(m_reservoir[m_second]));
    }
}

void EngineCore::PreShell(const CoreFrame& f)
{
    if (!(m_fillFlags & 8u)) return;
    auto& gfx = *f.gfx;
    auto cl = f.cl;
    auto bufState = [&](RHI::GPUBuffer& b, RHI::ResourceState& before, RHI::ResourceState after) {
        if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, after), cl); before = after;
    };
    const uint32_t current = m_receiverSlot, previous = current ^ 1u;
    bufState(m_reservoir[m_X], m_reservoirState[m_X], RHI::ResourceState::UNORDERED_ACCESS);
    bufState(m_reservoir[m_Y], m_reservoirState[m_Y], RHI::ResourceState::UNORDERED_ACCESS);
    bufState(m_reservoir[m_H], m_reservoirState[m_H], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    bufState(m_receiver[current], m_receiverState[current], RHI::ResourceState::UNORDERED_ACCESS);
    bufState(m_receiver[previous], m_receiverState[previous], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
}

void EngineCore::Record(const CoreFrame& f)
{
    RecordInitial(f);
    if (!(m_fillFlags & 8u)) return;
    RecordReuse(f);
}

void EngineCore::RecordInitial(const CoreFrame& f)
{
    auto& gfx = *f.gfx;
    auto cl = f.cl;
    const auto timer = gfx.BeginGPUTimestamp(cl, f.regions->initialShade);
    gfx.BindComputePipelineState(m_pso[0], cl); gfx.DispatchComputeIndirect(*f.args, 0, cl);
    gfx.EndGPUTimestamp(cl, timer);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);
}

void EngineCore::RecordReuse(const CoreFrame& f)
{
    auto& gfx = *f.gfx;
    auto cl = f.cl;
    auto bufState = [&](RHI::GPUBuffer& b, RHI::ResourceState& before, RHI::ResourceState after) {
        if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, after), cl); before = after;
    };
    auto uavBarrier = [&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
    auto indirect = [&](size_t p, const char* name) {
        const auto t = gfx.BeginGPUTimestamp(cl, name);
        gfx.BindComputePipelineState(m_pso[p], cl);
        gfx.DispatchComputeIndirect(*f.args, 0, cl);
        gfx.EndGPUTimestamp(cl, t); uavBarrier();
    };
    const uint32_t X = m_X, Y = m_Y, second = m_second, shaded = m_shaded;
    bufState(m_reservoir[X], m_reservoirState[X], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    indirect(1, f.regions->temporal);
    bufState(m_reservoir[Y], m_reservoirState[Y], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    if (m_spatialPasses > 0)
    {
        bufState(m_reservoir[X], m_reservoirState[X], RHI::ResourceState::UNORDERED_ACCESS);
        indirect(2, f.regions->spatial);
        bufState(m_reservoir[X], m_reservoirState[X], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    }
    if (m_secondStage)
    {
        bufState(m_reservoir[second], m_reservoirState[second], RHI::ResourceState::UNORDERED_ACCESS);
        gfx.SetComputeRootCBV(ReSTIR::ShellSlots::CB, f.cb->CurrentBuffer(gfx), sizeof(StageConstants), cl);
        indirect(2, f.regions->spatial1);
        bufState(m_reservoir[second], m_reservoirState[second], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    }
    gfx.SetComputeRootCBV(ReSTIR::ShellSlots::CB, f.cb->CurrentBuffer(gfx), 2 * sizeof(StageConstants), cl);
    indirect(3, f.regions->shade);
    // Next frame's history: the shaded reservoir, or the temporal output when spatial feedback is
    // off.
    m_historySlot = m_feedback ? shaded : Y;
    m_receiverSlot ^= 1u;
    m_historyJitterX = f.jitterX; m_historyJitterY = f.jitterY;
    m_historyPhase = f.phase;
}

const char* EngineCore::InertReason() const
{
    if (!m_pso.back().IsValid()) return "pipeline";
    if (!m_allocated) return "resources";
    return nullptr;
}

uint64_t EngineCore::ResidentBytes() const
{
    uint64_t bytes = 0;
    if (m_allocated && m_reuse)
    {
        const uint64_t texels = uint64_t(m_tw) * m_th;
        for (const auto& b : m_reservoir) if (b.IsValid()) bytes += texels * sizeof(ReservoirStorage);
        bytes += 2 * texels * 16;
    }
    return bytes;
}
} // namespace RTDI
