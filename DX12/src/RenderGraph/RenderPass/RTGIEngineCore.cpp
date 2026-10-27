#include "RenderGraph/RenderPass/RTGIEngineCore.h"
#include "Settings/PassSettings.h"     // pass.rtgi.*
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include <algorithm>
#include <chrono>
#include <iterator>

namespace RTGI
{
EngineCore::EngineCore()
{
    const auto& cmd = Sys::CommandLine::Get();
    // Temporal + spatial resampling is on by default (engine.json restirGiReuse;
    // -gi_reuse / -gi_no_reuse also set it): ApplySettings.
    // Feedback on, as RTXDI: the final (post-spatial) reservoir becomes the history.
    m_feedback = !cmd.Has("-gi_no_spatial_feedback");
    // Reuse: spatial neighbours (pass.rtgi.spatialSamples), the disocclusion
    // boost for young reservoirs (boostSamples, -gi_no_disocclusion_boost),
    // the temporal merge (pairwise MIS; -gi_temporal_1overM selects plain 1/M)
    // and the fresh-sample blend in Shade (freshBlend).
    m_spatialSamples = static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiSpatialSamples(), 1, 16));
    m_boostSamples = static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiBoostSamples(), 1, 32));
    m_freshBlend = std::clamp(::Settings::Pass::rtgiFreshBlend(), 0.f, 1.f);
    // M cap 8, as RTXDI GI's maxHistoryLength (age cap 30, restirGiAgeCap): longer
    // histories (20/60) keep rare bright samples alive long enough to spread into
    // blotches on shadowed surfaces. The age cap and the spatial radius are
    // engine.json rows (ApplySettings; -gi_age_cap / -gi_spatial_radius also set
    // them): the member defaults hold until then.
    m_maxM = static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiMCap(), 1, 63));
    m_reuse = m_reuseRequested;
}

bool EngineCore::BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell)
{
    if (m_pso.back().IsValid()) return true;
    if (m_pipelinesFailed || !shell.IsValid()) return false;
    const auto start = std::chrono::steady_clock::now();
    static constexpr const char* kEntries[] = {"Temporal", "Spatial", "Shade"};
    static_assert(std::size(kEntries) == std::tuple_size_v<decltype(m_pso)>);
    // Concurrently (ComputePipelineBatch), all-or-nothing.
    if (!ComputePipelineBatch::BuildEntries(gfx, shell, "shaders/RTGI.cs.hlsl", kEntries, m_pso.size(),
                                            m_pso.data(), "RTGIPass"))
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
    for (auto& p : m_psoSharc) gfx.DestroyPipelineState(p);
    m_pipelinesFailed = false;
}

void EngineCore::AdoptPipelines(const Core& from)
{
    m_pso = static_cast<const EngineCore&>(from).m_pso;
    m_adopted = true;
}

bool EngineCore::BuildSharcPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell,
                                     const std::vector<DxcCompiler::Define>& defines)
{
    if (HasSharcPipelines()) return true;
    if (!shell.IsValid()) return false;
    // Temporal's history validation re-shades a stored hit the way it was
    // shaded (the reservoir's cached bit), and Temporal and Spatial both keep
    // that bit through their merges (GIMetaKeep): their RTGI_SHARC builds.
    static constexpr const char* kEntries[] = {"Temporal", "Spatial"};
    for (size_t i = 0; i < m_psoSharc.size(); ++i)
    {
        if (m_psoSharc[i].IsValid()) continue;
        const auto shader = ShaderDiskCache::LoadOrCompile("shaders/RTGISharc.cs.hlsl", kEntries[i], RHI::ShaderStage::CS, defines,
                                                           "RTGIPass", ShaderDiskCache::CompileFlags{false, true});
        if (shader.empty() || !ComputePipelineBatch::Create(gfx, shell, &shader, 1, &m_psoSharc[i]))
        {
            LOG_ERROR("RTGIPass: SHaRC pipeline %s failed", kEntries[i]);
            for (auto& p : m_psoSharc) gfx.DestroyPipelineState(p);
            return false;
        }
    }
    return true;
}

void EngineCore::ReleaseSharcPipelines(IGraphicsDevice& gfx)
{
    for (auto& p : m_psoSharc) gfx.DestroyPipelineState(p);
}

bool EngineCore::Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH)
{
    m_tw = traceW; m_th = traceH; m_allocated = true;
    if (!m_reuse) return true;
    RHI::GPUBufferDesc bd{};
    bd.usage = RHI::Usage::DEFAULT; bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    bd.stride = sizeof(ReservoirStorage); bd.size = uint64_t(m_tw) * m_th * bd.stride;
    // Three rotating reservoirs: history, initial/spatial, temporal.
    for (auto& b : m_reservoir)
        if (!gfx.CreateBuffer(bd, b)) { LOG_ERROR("RTGIPass: reservoir allocation failed"); m_allocated = false; return false; }
    bd.stride = sizeof(SurfaceStorage); bd.size = uint64_t(m_tw) * m_th * bd.stride;
    for (auto& b : m_receiver)
        if (!gfx.CreateBuffer(bd, b)) { LOG_ERROR("RTGIPass: receiver allocation failed"); m_allocated = false; return false; }
    m_reservoirState.fill(RHI::ResourceState::UNDEFINED); m_receiverState.fill(RHI::ResourceState::UNDEFINED);
    m_historySlot = 0; m_receiverSlot = 0;
    return true;
}

void EngineCore::Release(IGraphicsDevice& gfx)
{
    for (auto& b : m_reservoir) { if (b.IsValid()) gfx.DestroyBuffer(b); b = {}; }
    for (auto& b : m_receiver) { if (b.IsValid()) gfx.DestroyBuffer(b); b = {}; }
    m_allocated = false;
}

bool EngineCore::ApplySettings(const CoreSettings& s, bool& historyReset)
{
    historyReset = false;
    bool realloc = false;
    m_reuseRequested = s.reuse;
    const bool wantReuse = m_reuseRequested;
    if (wantReuse != m_reuse)
    {
        // Reservoir/receiver buffers exist only with reuse; the shell's
        // OnCompile rebuilds them at the service point (same path as a resize).
        m_reuse = wantReuse;
        realloc = true; historyReset = true;
        LOG_INFO("[RTGI_SETTINGS] reuse=%u feedback=%u", wantReuse ? 1u : 0u, m_feedback ? 1u : 0u);
    }
    // The setting is in half-resolution trace pixels; full resolution doubles
    // it so the reuse footprint in render pixels stays the same. A command-line
    // radius (-gi_spatial_radius, DevPin) is taken as given.
    const uint32_t radius = s.spatialRadiusPinned ? std::min(s.spatialRadius, 32u)
        : std::min(s.spatialRadius * (s.fullResolution ? 2u : 1u), 32u);
    if (radius != m_spatialRadius) { m_spatialRadius = radius; historyReset = true; }
    const uint32_t age = std::clamp(s.maxAge, 1u, 63u);
    if (age != m_maxAge) historyReset = true;
    m_maxAge = age;
    return realloc;
}

void EngineCore::FillConstants(IGraphicsDevice& gfx, Constants& cb, const FillContext& ctx)
{
    auto index = [&](uint64_t h) { return h ? gfx.DescriptorHeapIndexFromGpuHandle(h) : 0u; };
    m_fillFlags = ctx.shellFlags;
    // Budget ladder: stage 1 = one spatial candidate, stage 3 = no validation.
    cb.flags |= (ctx.budgetStage >= 1 ? 8192u : 0u) | (ctx.budgetStage >= 3 ? 64u : 0u);
    if (!(ctx.shellFlags & 16384u))
    { cb.historyJitterDelta[0] = m_historyJitterX - ctx.jitterX; cb.historyJitterDelta[1] = m_historyJitterY - ctx.jitterY; }
    if ((ctx.shellFlags & 8u) && Sys::CommandLine::Get().Has("-restir_reprojection_log"))
        LOG_INFO("[RESTIR_REPROJECT] pass=RTGI frame=%u currentX=%.6f currentY=%.6f previousX=%.6f previousY=%.6f deltaX=%.6f deltaY=%.6f reset=%u",
            ctx.frame, ctx.jitterX, ctx.jitterY, m_historyJitterX, m_historyJitterY, cb.historyJitterDelta[0], cb.historyJitterDelta[1],
            ctx.resetReservoir ? 1u : 0u);
    cb.spatialSamples = m_spatialSamples; cb.boostSamples = m_boostSamples; cb.freshBlend = m_freshBlend;
    // H = last frame's shaded reservoir (history), X = Trace output and the
    // spatial output that Shade reads, Y = Temporal output. X becomes the next
    // H, so spatial reuse compounds across frames (RTXDI). Receivers ping-pong.
    m_H = m_historySlot % 3u; m_X = (m_H + 1u) % 3u; m_Y = (m_H + 2u) % 3u;
    const uint32_t current = m_receiverSlot, previous = current ^ 1u;
    if (ctx.shellFlags & 8u)
    {
        cb.initialUav = index(gfx.GetBufferUAVGpuHandle(m_reservoir[m_X])); cb.initialSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_X]));
        cb.previousSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_H])); cb.temporalUav = index(gfx.GetBufferUAVGpuHandle(m_reservoir[m_Y]));
        cb.temporalSrv = index(gfx.GetBufferSRVGpuHandle(m_reservoir[m_Y])); cb.previousSurfaceSrv = index(gfx.GetBufferSRVGpuHandle(m_receiver[previous]));
        cb.surfaceUav = index(gfx.GetBufferUAVGpuHandle(m_receiver[current])); cb.surfaceSrv = index(gfx.GetBufferSRVGpuHandle(m_receiver[current]));
        cb.historyReset = ctx.resetReservoir; cb.maxM = m_maxM; cb.maxAge = ctx.budgetStage >= 3 ? 1u : m_maxAge; cb.spatialRadius = m_spatialRadius;
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
    if (!(m_fillFlags & 8u)) return;
    auto& gfx = *f.gfx;
    auto cl = f.cl;
    auto bufState = [&](RHI::GPUBuffer& b, RHI::ResourceState& before, RHI::ResourceState after) {
        if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, after), cl); before = after;
    };
    auto barrier = [&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
    auto dispatch = [&](size_t p, const char* name) {
        // With SHaRC, Temporal and Spatial are their RTGI_SHARC builds.
        const RHI::PipelineState& pso = (p < m_psoSharc.size() && f.sharc && HasSharcPipelines()) ? m_psoSharc[p] : m_pso[p];
        const auto timer = gfx.BeginGPUTimestamp(cl, name); gfx.BindComputePipelineState(pso, cl);
        gfx.DispatchCompute((f.traceW + 7) / 8, (f.traceH + 7) / 8, 1, cl); gfx.EndGPUTimestamp(cl, timer);
    };
    const uint32_t X = m_X, Y = m_Y, current = m_receiverSlot;
    bufState(m_reservoir[X], m_reservoirState[X], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    dispatch(0, f.regions->temporal); barrier();
    bufState(m_reservoir[Y], m_reservoirState[Y], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    bufState(m_receiver[current], m_receiverState[current], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    bufState(m_reservoir[X], m_reservoirState[X], RHI::ResourceState::UNORDERED_ACCESS);
    dispatch(1, f.regions->spatial); barrier();
    bufState(m_reservoir[X], m_reservoirState[X], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    dispatch(2, f.regions->shade); barrier();
    m_historySlot = m_feedback ? X : Y;
    m_receiverSlot ^= 1u;
    m_historyJitterX = f.jitterX; m_historyJitterY = f.jitterY;
}

const char* EngineCore::InertReason() const
{
    if (!m_pso.back().IsValid()) return "pipeline";
    if (!m_allocated) return "resources";
    return nullptr;
}

uint64_t EngineCore::ResidentBytes() const
{
    if (!m_allocated || !m_reuse) return 0;
    const uint64_t texels = uint64_t(m_tw) * m_th;
    return texels * (3 * sizeof(ReservoirStorage) + 2 * sizeof(SurfaceStorage));
}
} // namespace RTGI
