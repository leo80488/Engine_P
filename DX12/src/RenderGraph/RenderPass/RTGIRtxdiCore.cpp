#include "RenderGraph/RenderPass/RTGIRtxdiCore.h"

#if ENGINE_WITH_RTXDI
#include "RenderGraph/RenderPass/RtxdiBridge.h"
#include "Settings/PassSettings.h"     // pass.rtgi.*
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderReflection.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/GI/ReSTIRGI.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace RTGI
{
namespace
{
// RtxdiGI.cs.hlsl's cbuffer RtxdiParams (b1), member for member; BuildPipelines
// checks every offset against the shader's reflection.
struct alignas(16) RtxdiGIConstants
{
    RTXDI::BridgeConstants bridge;
    RTXDI_RuntimeParameters runtime;
    RTXDI_ReservoirBufferParameters reservoirs;
    RTXDI_GIBufferIndices bufferIndices;
    RTXDI_GITemporalResamplingParameters temporal;
    RTXDI_BoilingFilterParameters boiling;
    RTXDI_GISpatialResamplingParameters spatial;
    uint32_t reservoirUav, neighborOffsetsSrv, flags, pad;
};
static_assert(sizeof(RtxdiGIConstants) == 480, "mirror of cbuffer RtxdiParams (RtxdiGI.cs.hlsl)");
constexpr uint32_t kFlagTemporal = 1u << 0, kFlagSpatial = 1u << 1, kFlagUnshadowed = 1u << 2,
                   kFlagBoiling = 1u << 3, kFlagValidate = 1u << 4, kFlagFinalTemporal = 1u << 5;
// RTXDI's neighbour offset table (ImportanceSamplingContext's default size).
constexpr uint32_t kNeighborOffsets = 8192;

enum Entry : uint32_t { Prepare, Temporal, Boiling, Spatial, ToEngine, Shade, EntryCount };
constexpr const char* kEntries[EntryCount] = {
    "RtxdiGIPrepare", "RtxdiGITemporal", "RtxdiGIBoiling", "RtxdiGISpatial", "RtxdiGIToEngine", "Shade" };

struct Expect { const char* name; uint32_t offset, size; };
#define RTXDI_GI_VAR(hlsl, member) { hlsl, uint32_t(offsetof(RtxdiGIConstants, member)), uint32_t(sizeof(RtxdiGIConstants::member)) }
const Expect kLayout[] = {
    RTXDI_GI_VAR("g_RabBridge", bridge), RTXDI_GI_VAR("g_Runtime", runtime),
    RTXDI_GI_VAR("g_Reservoirs", reservoirs), RTXDI_GI_VAR("g_GIBuffers", bufferIndices),
    RTXDI_GI_VAR("g_Temporal", temporal), RTXDI_GI_VAR("g_Boiling", boiling),
    RTXDI_GI_VAR("g_Spatial", spatial), RTXDI_GI_VAR("g_ReservoirUav", reservoirUav),
    RTXDI_GI_VAR("g_NeighborOffsetsSrv", neighborOffsetsSrv), RTXDI_GI_VAR("g_RtxdiGIFlags", flags),
};
#undef RTXDI_GI_VAR

bool LayoutMatches(const std::vector<uint8_t>& dxil)
{
    ShaderReflect::Reflection r;
    if (!ShaderReflect::Reflect(dxil.data(), dxil.size(), r)) { LOG_ERROR("[RTXDI] GI core: no reflection"); return false; }
    const ShaderReflect::CBufferLayout* cb = nullptr;
    for (const auto& c : r.cbuffers) if (c.name == "RtxdiParams") cb = &c;
    if (!cb) { LOG_ERROR("[RTXDI] GI core: cbuffer RtxdiParams not reflected"); return false; }
    bool ok = true;
    for (const auto& e : kLayout)
    {
        const ShaderReflect::CBufferVar* v = nullptr;
        for (const auto& var : cb->vars) if (var.name == e.name) v = &var;
        if (!v || v->offset != e.offset || v->size != e.size)
        {
            LOG_ERROR("[RTXDI] GI core layout %s: hlsl offset=%u size=%u, c++ offset=%u size=%u", e.name,
                      v ? v->offset : 0u, v ? v->size : 0u, e.offset, e.size);
            ok = false;
        }
    }
    return ok;
}

// -rtxdi_gi_bias=off|basic|raytraced (temporal and spatial; RTXDI GI has no
// pairwise mode).
bool ParseBias(const std::string& v, RTXDI_GIBiasCorrectionMode& out)
{
    if (v == "off") { out = RTXDI_GIBiasCorrectionMode::Off; return true; }
    if (v == "basic") { out = RTXDI_GIBiasCorrectionMode::Basic; return true; }
    if (v == "raytraced") { out = RTXDI_GIBiasCorrectionMode::Raytraced; return true; }
    return false;
}
const char* BiasName(RTXDI_GIBiasCorrectionMode m)
{
    switch (m)
    {
    case RTXDI_GIBiasCorrectionMode::Off: return "off";
    case RTXDI_GIBiasCorrectionMode::Basic: return "basic";
    case RTXDI_GIBiasCorrectionMode::Raytraced: return "raytraced";
    default: return "?";
    }
}

class RtxdiCore final : public Core
{
public:
    RtxdiCore()
    {
        const auto& cmd = Sys::CommandLine::Get();
        // The reuse switches and history caps the engine core reads, with the same
        // meaning (pass.rtgi.* and engine.json's rows; reuse, the radius and the age
        // cap arrive with ApplySettings).
        m_spatialSamples = static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiSpatialSamples(), 1, 32));
        m_freshBlend = std::clamp(::Settings::Pass::rtgiFreshBlend(), 0.f, 1.f);
        m_maxM = static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiMCap(), 1, 63));
        m_reuse = m_reuseRequested;
        // The target: the engine core's (with its visibility from the receiver,
        // the default - the two cores then target the same integrand) or
        // RTXDI's own setup (unshadowed, visibility in the ray-traced bias
        // correction and the final shading).
        m_unshadowed = ::Settings::Pass::rtgiRtxdiTarget() == "unshadowed";
        // Basic is exact with the visible target (every target it evaluates
        // holds the visibility); ray-traced adds rays that test it again.
        m_bias = m_unshadowed ? RTXDI_GIBiasCorrectionMode::Raytraced : RTXDI_GIBiasCorrectionMode::Basic;
        if (::Settings::Pass::rtgiRtxdiBiasGiven())
        {
            const std::string bias = ::Settings::Pass::rtgiRtxdiBias();
            if (!ParseBias(bias, m_bias)) LOG_ERROR("[RTXDI] -rtxdi_gi_bias=%s: expected off|basic|raytraced", bias.c_str());
        }
        // RTXDI GI defaults that bias (its header calls the fallback tap "locally
        // biased, usually darker") are opt-in here, like the PT core's boiling filter.
        // The boiling filter is the shell's -gi_boiling_filter, as for the engine core.
        m_fallback = ::Settings::Pass::rtgiRtxdiFallback() != 0;
        m_permutation = ::Settings::Pass::rtgiRtxdiPermutation() != 0;
        // The engine core's reuse flags this core does not read: reported as
        // ignored=, never silently dropped (the boost samples are a pass row:
        // set at all, Given()).
        const struct { const char* name; bool given; } kEngineReuseFlags[] = {
            { "-gi_no_spatial_feedback", cmd.Has("-gi_no_spatial_feedback") },
            { "-gi_boost_samples", ::Settings::Pass::rtgiBoostSamplesGiven() },
            { "-gi_no_disocclusion_boost", cmd.Has("-gi_no_disocclusion_boost") },
            { "-gi_temporal_1overM", cmd.Has("-gi_temporal_1overM") },
            { "-gi_no_inverse_visibility", cmd.Has("-gi_no_inverse_visibility") },
            { "-gi_no_random_age_cap", cmd.Has("-gi_no_random_age_cap") } };
        for (const auto& f : kEngineReuseFlags)
            if (f.given) { if (!m_ignored.empty()) m_ignored += ' '; m_ignored += f.name; }
    }

    ReSTIR::Backend Backend() const override { return ReSTIR::Backend::Rtxdi; }

    bool BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell) override
    {
        if (HasPipelines()) return true;
        if (m_pipelinesFailed || !shell.IsValid()) return false;
        const auto start = std::chrono::steady_clock::now();
        // Every kernel first (Temporal's constants checked against its
        // reflection), then the pipelines together (ComputePipelineBatch).
        std::array<std::vector<uint8_t>, EntryCount> dxil;
        bool failed[EntryCount] = {};
        bool ok = true;
        for (uint32_t i = 0; i < EntryCount && ok; ++i)
        {
            // Shade is the engine core's own entry: the shell shades both cores.
            const char* file = i == Shade ? "shaders/RTGI.cs.hlsl" : "shaders/RtxdiGI.cs.hlsl";
            dxil[i] = ShaderDiskCache::LoadOrCompile(file, kEntries[i], RHI::ShaderStage::CS, {}, i == Shade ? "RTGIPass" : "RTGIRtxdi");
            if (dxil[i].empty() || (i == Temporal && !LayoutMatches(dxil[i]))) { failed[i] = true; ok = false; }
        }
        if (ok) ok = ComputePipelineBatch::Create(gfx, shell, dxil.data(), EntryCount, m_pso.data(), failed);
        if (!ok)
        {
            for (uint32_t i = 0; i < EntryCount; ++i)
                if (failed[i]) LOG_ERROR("RTGIPass: RTXDI pipeline %s failed", kEntries[i]);
            for (auto& p : m_pso) gfx.DestroyPipelineState(p);
            m_pipelinesFailed = true;
            return false;
        }
        m_pipelineMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }
    bool HasPipelines() const override { return m_pso.back().IsValid(); }
    void ReleasePipelines(IGraphicsDevice& gfx) override
    {
        if (!m_adopted)
            for (auto& p : m_pso) gfx.DestroyPipelineState(p);
        m_pso = {};
        m_adopted = false;
        gfx.DestroyPipelineState(m_psoSharcPrepare);
        m_pipelinesFailed = false;
    }
    void AdoptPipelines(const Core& from) override
    {
        m_pso = static_cast<const RtxdiCore&>(from).m_pso;
        m_adopted = true;
    }
    // SHaRC: Prepare's history validation is the shell's ValidateHistory, so its
    // RTGI_SHARC build re-shades through the cache exactly as the engine core's
    // Temporal does (the cached bit rides in RTXDI's reservoir as the low bit of
    // position.x, RtxdiGI.cs.hlsl).
    bool BuildSharcPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell,
                             const std::vector<DxcCompiler::Define>& defines) override
    {
        if (m_psoSharcPrepare.IsValid()) return true;
        if (!shell.IsValid()) return false;
        std::vector<DxcCompiler::Define> d = defines;
        d.push_back({"RTGI_SHARC", "1"});
        // 16-bit types (SHaRC's fp16 storage) on the engine's HLSL, recorded for hot reload.
        const auto dxil = ShaderDiskCache::LoadOrCompile("shaders/RtxdiGI.cs.hlsl", kEntries[Prepare], RHI::ShaderStage::CS, d,
                                                         "RTGIRtxdi", ShaderDiskCache::CompileFlags{false, true});
        if (dxil.empty() || !ComputePipelineBatch::Create(gfx, shell, &dxil, 1, &m_psoSharcPrepare))
        {
            LOG_ERROR("RTGIPass: RTXDI SHaRC pipeline %s failed", kEntries[Prepare]);
            gfx.DestroyPipelineState(m_psoSharcPrepare);
            return false;
        }
        return true;
    }
    bool HasSharcPipelines() const override { return m_psoSharcPrepare.IsValid(); }
    void ReleaseSharcPipelines(IGraphicsDevice& gfx) override { gfx.DestroyPipelineState(m_psoSharcPrepare); }

    bool Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH) override
    {
        Release(gfx);
        m_tw = traceW; m_th = traceH; m_allocated = true;
        if (!m_reuse) return true;   // no reuse: Trace's own estimate goes to NRD (as the engine core)
        rtxdi::ReSTIRGIStaticParameters sp;
        sp.RenderWidth = traceW; sp.RenderHeight = traceH;
        sp.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off;
        m_ctx = std::make_unique<rtxdi::ReSTIRGIContext>(sp);
        const RTXDI_ReservoirBufferParameters res = m_ctx->GetReservoirBufferParameters();
        RHI::GPUBufferDesc bd{};
        bd.usage = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        // The shell's 40-byte reservoir: Trace's output, then the final one
        // the engine's Shade reads (ToEngine).
        bd.stride = kEngineReservoirBytes; bd.size = uint64_t(traceW) * traceH * bd.stride;
        bool ok = gfx.CreateBuffer(bd, m_initial);
        // The receivers (GISurface, 32 B): this frame's and the previous one.
        bd.stride = kSurfaceBytes; bd.size = uint64_t(traceW) * traceH * bd.stride;
        for (auto& s : m_receiver) ok = ok && gfx.CreateBuffer(bd, s);
        bd.stride = sizeof(RTXDI_PackedGIReservoir);
        bd.size = uint64_t(res.reservoirArrayPitch) * rtxdi::c_NumReSTIRGIReservoirBuffers * bd.stride;
        ok = ok && gfx.CreateBuffer(bd, m_reservoirs);
        std::vector<uint8_t> offsetData(size_t(kNeighborOffsets) * 2);
        rtxdi::FillNeighborOffsetBuffer(offsetData.data(), kNeighborOffsets);
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_TYPED;
        bd.format = RHI::Format::R8G8_SNORM; bd.stride = 2; bd.size = offsetData.size();
        ok = ok && gfx.CreateBuffer(bd, m_neighborOffsets, offsetData.data());
        ok = ok && m_cb.Create(gfx, "RTGI.Rtxdi");
        if (!ok) { LOG_ERROR("RTGIPass: RTXDI GI core allocation failed"); Release(gfx); return false; }
        m_initialState = m_reservoirState = m_offsetsState = RHI::ResourceState::UNDEFINED;
        m_receiverState.fill(RHI::ResourceState::UNDEFINED);
        m_receiverSlot = 0; m_history = false; m_hasPrevView = false;
        LOG_INFO("[RTXDI] GI core allocated trace=%ux%u reservoirs=%u target=%s bias=%s fallback=%u permutation=%u",
                 traceW, traceH, res.reservoirArrayPitch, m_unshadowed ? "unshadowed" : "visible", BiasName(m_bias),
                 m_fallback ? 1u : 0u, m_permutation ? 1u : 0u);
        return true;
    }
    void Release(IGraphicsDevice& gfx) override
    {
        for (auto* b : { &m_initial, &m_receiver[0], &m_receiver[1], &m_reservoirs, &m_neighborOffsets })
        { if (b->IsValid()) gfx.DestroyBuffer(*b); *b = {}; }
        if (m_cb.IsValid()) m_cb.Destroy(gfx);
        m_ctx.reset();
        m_allocated = false;
    }

    bool ApplySettings(const CoreSettings& s, bool& historyReset) override
    {
        historyReset = false;
        bool realloc = false;
        m_reuseRequested = s.reuse;
        const bool wantReuse = m_reuseRequested;
        if (wantReuse != m_reuse)
        {
            // The reservoirs exist only with reuse; the shell's OnCompile
            // rebuilds them at the service point (as for the engine core).
            m_reuse = wantReuse;
            realloc = true; historyReset = true;
            LOG_INFO("[RTGI_SETTINGS] reuse=%u feedback=1 backend=rtxdi", wantReuse ? 1u : 0u);
        }
        // As the engine core's: a command-line radius is taken as given.
        const uint32_t radius = s.spatialRadiusPinned ? std::min(s.spatialRadius, 32u)
            : std::min(s.spatialRadius * (s.fullResolution ? 2u : 1u), 32u);
        if (radius != m_spatialRadius) { m_spatialRadius = radius; historyReset = true; }
        const uint32_t age = std::clamp(s.maxAge, 1u, 63u);
        if (age != m_maxAge) historyReset = true;
        m_maxAge = age;
        return realloc;
    }
    bool Reuse() const override { return m_reuse; }

    void FillConstants(IGraphicsDevice& gfx, Constants& cb, const FillContext& ctx) override
    {
        auto index = [&](uint64_t h) { return h ? gfx.DescriptorHeapIndexFromGpuHandle(h) : 0u; };
        m_fillFlags = ctx.shellFlags;
        // Budget ladder: stage 3 = no validation and no reuse visibility (the
        // shell's no-validation bit, which VisibleFrom reads), stage 1 = one
        // spatial sample (below).
        if (ctx.budgetStage >= 3) cb.flags |= 64u;
        if (!(ctx.shellFlags & 16384u))
        { cb.historyJitterDelta[0] = m_historyJitterX - ctx.jitterX; cb.historyJitterDelta[1] = m_historyJitterY - ctx.jitterY; }
        cb.spatialSamples = m_spatialSamples; cb.boostSamples = 0; cb.freshBlend = m_freshBlend;
        if (!(ctx.shellFlags & 8u) || !m_ctx || !m_cb.IsValid()) return;
        const uint32_t current = m_receiverSlot, previous = current ^ 1u;
        cb.initialUav = index(gfx.GetBufferUAVGpuHandle(m_initial)); cb.initialSrv = index(gfx.GetBufferSRVGpuHandle(m_initial));
        cb.previousSrv = 0; cb.temporalUav = 0; cb.temporalSrv = 0;
        cb.surfaceUav = index(gfx.GetBufferUAVGpuHandle(m_receiver[current])); cb.surfaceSrv = index(gfx.GetBufferSRVGpuHandle(m_receiver[current]));
        cb.previousSurfaceSrv = index(gfx.GetBufferSRVGpuHandle(m_receiver[previous]));
        cb.maxM = m_maxM; cb.maxAge = m_maxAge; cb.spatialRadius = m_spatialRadius;

        // Temporal + spatial (the spatial output is the next history, as the
        // engine core's feedback), or temporal alone (-gi_no_spatial, radius 0).
        m_spatialOn = !(ctx.shellFlags & 16u) && m_spatialRadius > 0;
        const auto mode = m_spatialOn ? rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial : rtxdi::ReSTIRGI_ResamplingMode::Temporal;
        const bool modeChanged = mode != m_ctx->GetResamplingMode();
        m_ctx->SetFrameIndex(ctx.frame);
        m_ctx->SetResamplingMode(mode);
        // A new history: the shell's reset, a fresh allocation, or the buffer
        // roles moved with the mode.
        m_reset = ctx.resetReservoir || !m_history || modeChanged;
        cb.historyReset = m_reset ? 1u : 0u;
        m_validate = !(cb.flags & 64u) && !m_reset;
        m_boiling = (ctx.shellFlags & 1024u) != 0;

        RtxdiGIConstants k{};
        RTXDI::BridgeConstants& b = k.bridge;
        std::memcpy(&b.invViewProj, cb.invViewProj, sizeof(b.invViewProj));
        std::memcpy(&b.worldToView, cb.worldToView, sizeof(b.worldToView));
        std::memcpy(&b.prevWorldToView, m_hasPrevView ? m_prevWorldToView : cb.worldToView, sizeof(b.prevWorldToView));
        b.camera = {cb.camera[0], cb.camera[1], cb.camera[2]};
        b.prevCamera = m_hasPrevView ? DirectX::XMFLOAT3{m_prevCamera[0], m_prevCamera[1], m_prevCamera[2]} : b.camera;
        b.width = cb.width; b.height = cb.height;
        b.depthIdx = cb.depthIdx; b.normalIdx = cb.normalIdx; b.albedoIdx = cb.albedoIdx; b.surfaceIdx = cb.surfaceIdx;
        b.velocityIdx = cb.velocityIdx; b.materialsIdx = cb.materialsIdx; b.lightsIdx = cb.lightsIdx; b.lightCount = cb.lightCount;
        b.prevLightsIdx = cb.lightsIdx; b.prevLightCount = cb.lightCount;
        b.surfaceGridWidth = m_tw; b.surfaceGridHeight = m_th;
        b.rayTMax = cb.rayTMax; b.alphaTestEnabled = cb.alphaTestEnabled; b.alphaInstanceCount = cb.alphaInstanceCount;

        k.runtime.neighborOffsetMask = kNeighborOffsets - 1;
        k.runtime.activeCheckerboardField = 0;
        k.runtime.frameIndex = ctx.frame;
        k.reservoirs = m_ctx->GetReservoirBufferParameters();
        k.bufferIndices = m_ctx->GetBufferIndices();
        k.temporal = m_ctx->GetTemporalResamplingParameters();
        k.temporal.maxHistoryLength = m_maxM;
        k.temporal.maxReservoirAge = ctx.budgetStage >= 3 ? 1u : m_maxAge;
        k.temporal.biasCorrectionMode = m_bias;
        k.temporal.enableFallbackSampling = m_fallback ? 1u : 0u;
        k.temporal.enablePermutationSampling = m_permutation ? 1u : 0u;
        k.boiling = m_ctx->GetBoilingFilterParameters();
        k.boiling.enableBoilingFilter = m_boiling ? 1u : 0u;
        k.spatial = m_ctx->GetSpatialResamplingParameters();
        k.spatial.numSamples = ctx.budgetStage >= 1 ? 1u : m_spatialSamples;
        k.spatial.samplingRadius = float(m_spatialRadius);
        k.spatial.biasCorrectionMode = m_bias;
        k.reservoirUav = index(gfx.GetBufferUAVGpuHandle(m_reservoirs));
        k.neighborOffsetsSrv = index(gfx.GetBufferSRVGpuHandle(m_neighborOffsets));
        k.flags = kFlagTemporal | (m_spatialOn ? kFlagSpatial : 0u) | (m_unshadowed ? kFlagUnshadowed : 0u)
                | (m_boiling ? kFlagBoiling : 0u) | (m_validate ? kFlagValidate : 0u)
                | (!m_spatialOn && !m_boiling ? kFlagFinalTemporal : 0u);
        *m_cb.Current(gfx) = k;

        std::memcpy(m_prevWorldToView, cb.worldToView, sizeof(m_prevWorldToView));
        std::memcpy(m_prevCamera, cb.camera, sizeof(m_prevCamera));
        m_hasPrevView = true;
    }

    void PreShell(const CoreFrame& f) override
    {
        if (!(m_fillFlags & 8u) || !m_ctx) return;
        auto& gfx = *f.gfx; auto cl = f.cl;
        auto bufState = [&](RHI::GPUBuffer& b, RHI::ResourceState& before, RHI::ResourceState after) {
            if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, after), cl); before = after;
        };
        // Trace writes the initial reservoir and this frame's receivers.
        const uint32_t current = m_receiverSlot, previous = current ^ 1u;
        bufState(m_initial, m_initialState, RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_receiver[current], m_receiverState[current], RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_receiver[previous], m_receiverState[previous], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        bufState(m_reservoirs, m_reservoirState, RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_neighborOffsets, m_offsetsState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    }

    void Record(const CoreFrame& f) override
    {
        if (!(m_fillFlags & 8u) || !m_ctx) return;
        auto& gfx = *f.gfx;
        auto cl = f.cl;
        gfx.SetComputeRootCBV(ReSTIR::ShellSlots::CoreCB, m_cb.CurrentBuffer(gfx), 0, cl);
        auto bufState = [&](RHI::GPUBuffer& b, RHI::ResourceState& before, RHI::ResourceState after) {
            if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, after), cl); before = after;
        };
        auto barrier = [&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
        auto dispatch = [&](Entry e, const char* name, uint32_t group = 8) {
            const auto t = gfx.BeginGPUTimestamp(cl, name);
            // With SHaRC, Prepare is its RTGI_SHARC build.
            gfx.BindComputePipelineState(e == Prepare && f.sharc && m_psoSharcPrepare.IsValid() ? m_psoSharcPrepare : m_pso[e], cl);
            gfx.DispatchCompute((f.traceW + group - 1) / group, (f.traceH + group - 1) / group, 1, cl);
            gfx.EndGPUTimestamp(cl, t); barrier();
        };
        const uint32_t current = m_receiverSlot;
        // The history validated in place (before RTXDI's temporal reads it),
        // the coverage history, Trace's output into RTXDI's initial slot; the
        // stage that ends the chain writes the final reservoir for Shade.
        dispatch(Prepare, f.regions->prepare);
        bufState(m_receiver[current], m_receiverState[current], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        dispatch(Temporal, f.regions->temporal);
        if (m_boiling) dispatch(Boiling, f.regions->boiling, 16);
        if (m_spatialOn) dispatch(Spatial, f.regions->spatial);
        else if (m_boiling) dispatch(ToEngine, f.regions->final);
        bufState(m_initial, m_initialState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        dispatch(Shade, f.regions->shade);
        // This frame's receivers and final reservoirs are the next history.
        m_receiverSlot ^= 1u;
        m_history = true;
        m_historyJitterX = f.jitterX; m_historyJitterY = f.jitterY;
    }

    const char* InertReason() const override
    {
        if (!HasPipelines()) return "pipeline";
        if (!m_allocated) return "resources";
        return nullptr;
    }
    bool IsAllocated() const override { return m_allocated; }
    const char* IgnoredFlags() const override { return m_ignored.c_str(); }
    uint64_t ResidentBytes() const override
    {
        if (!m_allocated || !m_ctx) return 0;
        const uint64_t texels = uint64_t(m_tw) * m_th;
        return texels * (kEngineReservoirBytes + 2ull * kSurfaceBytes)
             + uint64_t(m_ctx->GetReservoirBufferParameters().reservoirArrayPitch) * rtxdi::c_NumReSTIRGIReservoirBuffers
               * sizeof(RTXDI_PackedGIReservoir)
             + uint64_t(kNeighborOffsets) * 2;
    }
    double PipelineMs() const override { return m_pipelineMs; }
    bool Feedback() const override { return m_spatialOn; }
    uint32_t SpatialRadius() const override { return m_spatialRadius; }

private:
    static constexpr uint32_t kEngineReservoirBytes = 40;   // GIReservoir (restir_gi_reservoir.hlsli)
    static constexpr uint32_t kSurfaceBytes = 32;           // GISurface
    std::array<RHI::PipelineState, EntryCount> m_pso;
    RHI::PipelineState m_psoSharcPrepare;   // SHaRC: Prepare's RTGI_SHARC build
    bool m_pipelinesFailed = false;
    bool m_adopted = false;   // m_pso is another core's (AdoptPipelines)
    double m_pipelineMs = 0.0;
    std::unique_ptr<rtxdi::ReSTIRGIContext> m_ctx;
    RHI::GPUBuffer m_initial, m_reservoirs, m_neighborOffsets;
    std::array<RHI::GPUBuffer, 2> m_receiver;
    RHI::ResourceState m_initialState = RHI::ResourceState::UNDEFINED, m_reservoirState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_offsetsState = RHI::ResourceState::UNDEFINED;
    std::array<RHI::ResourceState, 2> m_receiverState{};
    FrameCB<RtxdiGIConstants> m_cb;
    uint32_t m_tw = 0, m_th = 0;
    bool m_allocated = false;
    // Settings (engine.json and pass.rtgi.*).
    uint32_t m_maxM = 8, m_maxAge = 60, m_spatialRadius = 8, m_spatialSamples = 3;
    float m_freshBlend = 0.f;
    bool m_reuseRequested = true, m_reuse = true;
    bool m_unshadowed = false, m_fallback = false, m_permutation = false;
    RTXDI_GIBiasCorrectionMode m_bias = RTXDI_GIBiasCorrectionMode::Basic;
    std::string m_ignored;
    // This frame's decisions (FillConstants -> Record).
    uint32_t m_fillFlags = 0;
    bool m_spatialOn = true, m_validate = true, m_boiling = false, m_reset = true;
    // History.
    uint32_t m_receiverSlot = 0;
    bool m_history = false;
    float m_historyJitterX = 0, m_historyJitterY = 0;
    float m_prevWorldToView[16] = {}, m_prevCamera[3] = {};
    bool m_hasPrevView = false;
};
} // namespace

std::unique_ptr<Core> MakeRtxdiCore() { return std::make_unique<RtxdiCore>(); }
} // namespace RTGI

#else

namespace RTGI
{
std::unique_ptr<Core> MakeRtxdiCore() { return nullptr; }
} // namespace RTGI

#endif
