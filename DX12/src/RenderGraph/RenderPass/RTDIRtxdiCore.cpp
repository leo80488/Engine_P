#include "RenderGraph/RenderPass/RTDIRtxdiCore.h"

#if ENGINE_WITH_RTXDI
#include "RenderGraph/RenderPass/RTDIEngineCore.h"
#include "RenderGraph/RenderPass/RtxdiBridge.h"
#include "Settings/PassSettings.h"     // pass.rtdi.rtxdi*
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderReflection.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include <Rtxdi/ImportanceSamplingContext.h>
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/ReGIR/ReGIR.h>
#include <Rtxdi/LightSampling/RISBufferSegmentAllocator.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace RTDI
{
namespace
{
// RtxdiDI.cs.hlsl's cbuffer RtxdiParams (b1), member for member; BuildPipelines
// checks every offset against the shader's reflection.
struct alignas(16) RtxdiConstants
{
    RTXDI::BridgeConstants bridge;
    RTXDI_RuntimeParameters runtime;
    RTXDI_LightBufferParameters lightBuffer;
    RTXDI_RISBufferSegmentParameters localRIS;
    RTXDI_RISBufferSegmentParameters environmentRIS;
    RTXDI_ReservoirBufferParameters reservoirs;
    RTXDI_DIBufferIndices bufferIndices;
    RTXDI_DIInitialSamplingParameters initial;
    RTXDI_DITemporalResamplingParameters temporal;
    RTXDI_BoilingFilterParameters boiling;
    RTXDI_DISpatialResamplingParameters spatial;
    RTXDI_ShadingParameters shading;
    ReGIR_Parameters regir;
    uint32_t reservoirUav, risUav, neighborOffsetsSrv, rtxdiFlags;
    uint32_t regirLightSlots, localTiles, pad0, pad1;
};
constexpr uint32_t kFlagTemporal = 1u << 0, kFlagSpatial = 1u << 1, kFlagInitialVisibility = 1u << 2;

enum Entry : uint32_t { PresampleLocal, PresampleReGIR, Initial, Temporal, Spatial, Shade, ToEngine, FromEngine, Boiling, EntryCount };
constexpr const char* kEntries[EntryCount] = {
    "RtxdiPresampleLocal", "RtxdiPresampleReGIR", "RtxdiInitial", "RtxdiTemporal",
    "RtxdiSpatial", "RtxdiShade", "RtxdiToEngine", "EngineToRtxdi", "RtxdiBoiling" };

struct Expect { const char* name; uint32_t offset, size; };
#define RTXDI_CORE_VAR(hlsl, member) { hlsl, uint32_t(offsetof(RtxdiConstants, member)), uint32_t(sizeof(RtxdiConstants::member)) }
const Expect kLayout[] = {
    RTXDI_CORE_VAR("g_RabBridge", bridge), RTXDI_CORE_VAR("g_Runtime", runtime),
    RTXDI_CORE_VAR("g_LightBuffer", lightBuffer), RTXDI_CORE_VAR("g_LocalRIS", localRIS),
    RTXDI_CORE_VAR("g_EnvironmentRIS", environmentRIS), RTXDI_CORE_VAR("g_Reservoirs", reservoirs),
    RTXDI_CORE_VAR("g_DIBuffers", bufferIndices), RTXDI_CORE_VAR("g_Initial", initial),
    RTXDI_CORE_VAR("g_Temporal", temporal), RTXDI_CORE_VAR("g_Boiling", boiling),
    RTXDI_CORE_VAR("g_Spatial", spatial), RTXDI_CORE_VAR("g_Shading", shading),
    RTXDI_CORE_VAR("g_ReGIR", regir), RTXDI_CORE_VAR("g_ReservoirUav", reservoirUav),
    RTXDI_CORE_VAR("g_RtxdiFlags", rtxdiFlags), RTXDI_CORE_VAR("g_RegirLightSlots", regirLightSlots),
    RTXDI_CORE_VAR("g_LocalTiles", localTiles),
};
#undef RTXDI_CORE_VAR

bool LayoutMatches(const std::vector<uint8_t>& dxil)
{
    ShaderReflect::Reflection r;
    if (!ShaderReflect::Reflect(dxil.data(), dxil.size(), r)) { LOG_ERROR("[RTXDI] DI core: no reflection"); return false; }
    const ShaderReflect::CBufferLayout* cb = nullptr;
    for (const auto& c : r.cbuffers) if (c.name == "RtxdiParams") cb = &c;
    if (!cb) { LOG_ERROR("[RTXDI] DI core: cbuffer RtxdiParams not reflected"); return false; }
    bool ok = true;
    for (const auto& e : kLayout)
    {
        const ShaderReflect::CBufferVar* v = nullptr;
        for (const auto& var : cb->vars) if (var.name == e.name) v = &var;
        if (!v || v->offset != e.offset || v->size != e.size)
        {
            LOG_ERROR("[RTXDI] DI core layout %s: hlsl offset=%u size=%u, c++ offset=%u size=%u", e.name,
                      v ? v->offset : 0u, v ? v->size : 0u, e.offset, e.size);
            ok = false;
        }
    }
    return ok;
}

// -rtxdi_di_bias=off|basic|pairwise|raytraced (temporal and spatial).
uint32_t BiasMode(const std::string& v, uint32_t fallback)
{
    if (v == "off") return RTXDI_BIAS_CORRECTION_OFF;
    if (v == "basic") return RTXDI_BIAS_CORRECTION_BASIC;
    if (v == "pairwise") return RTXDI_BIAS_CORRECTION_PAIRWISE;
    if (v == "raytraced") return RTXDI_BIAS_CORRECTION_RAY_TRACED;
    return fallback;
}
const char* BiasName(uint32_t m)
{
    switch (m)
    {
    case RTXDI_BIAS_CORRECTION_OFF: return "off";
    case RTXDI_BIAS_CORRECTION_BASIC: return "basic";
    case RTXDI_BIAS_CORRECTION_PAIRWISE: return "pairwise";
    case RTXDI_BIAS_CORRECTION_RAY_TRACED: return "raytraced";
    default: return "?";
    }
}

void FillReGIR(const rtxdi::ReGIRContext& ctx, ReGIR_Parameters& out)
{
    const auto st = ctx.GetReGIRStaticParameters();
    const auto dyn = ctx.GetReGIRDynamicParameters();
    out = {};
    out.commonParams.localLightSamplingFallbackMode = static_cast<uint32_t>(dyn.fallbackSamplingMode);
    out.commonParams.centerX = dyn.center.x; out.commonParams.centerY = dyn.center.y; out.commonParams.centerZ = dyn.center.z;
    out.commonParams.risBufferOffset = ctx.GetReGIRCellOffset();
    out.commonParams.lightsPerCell = st.lightsPerCell;
    out.commonParams.cellSize = dyn.regirCellSize;
    out.commonParams.samplingJitter = std::max(0.f, dyn.regirSamplingJitter * 2.f);
    out.commonParams.localLightPresamplingMode = static_cast<uint32_t>(dyn.presamplingMode);
    out.commonParams.numRegirBuildSamples = dyn.regirNumBuildSamples;
    out.gridParams.cellsX = st.gridParameters.gridSize.x;
    out.gridParams.cellsY = st.gridParameters.gridSize.y;
    out.gridParams.cellsZ = st.gridParameters.gridSize.z;
    const auto onion = ctx.GetReGIROnionCalculatedParameters();
    const size_t layers = std::min<size_t>(onion.regirOnionLayers.size(), RTXDI_ONION_MAX_LAYER_GROUPS);
    const size_t rings = std::min<size_t>(onion.regirOnionRings.size(), RTXDI_ONION_MAX_RINGS);
    for (size_t i = 0; i < layers; ++i) out.onionParams.layers[i] = onion.regirOnionLayers[i];
    for (size_t i = 0; i < rings; ++i) out.onionParams.rings[i] = onion.regirOnionRings[i];
    out.onionParams.numLayerGroups = static_cast<uint32_t>(layers);
    out.onionParams.cubicRootFactor = onion.regirOnionCubicRootFactor;
    out.onionParams.linearFactor = onion.regirOnionLinearFactor;
}

class RtxdiCore final : public Core
{
public:
    RtxdiCore(uint32_t shellFlags, const ReSTIR::DIComposition& composition)
        : m_shellFlags(shellFlags), m_composition(composition)
    {
        const auto& cmd = Sys::CommandLine::Get();
        if (composition.initial == ReSTIR::Backend::Engine || composition.reuse == ReSTIR::Backend::Engine)
            m_engine = std::make_unique<EngineCore>(shellFlags);
        // The core's knobs are pass rows (pass.rtdi.rtxdi*).
        const std::string local = Settings::Pass::rtdiRtxdiLocal();
        m_localMode = local == "uniform" ? ReSTIRDI_LocalLightSamplingMode_UNIFORM
                    : local == "power" ? ReSTIRDI_LocalLightSamplingMode_POWER_RIS
                    : ReSTIRDI_LocalLightSamplingMode_REGIR_RIS;
        // Ray-traced bias correction by default: Basic assumes every reused sample is
        // visible and loses ~3% energy on occluded multi-light scenes (ray-traced ~+0.3%;
        // tools/restir_backend_ab.py).
        const std::string bias = Settings::Pass::rtdiRtxdiBias();
        m_temporalBias = BiasMode(bias, RTXDI_BIAS_CORRECTION_RAY_TRACED);
        m_spatialBias = BiasMode(bias, RTXDI_BIAS_CORRECTION_RAY_TRACED);
        m_initialVisibility = Settings::Pass::rtdiRtxdiInitialVisibility() != 0;
        m_permutation = Settings::Pass::rtdiRtxdiPermutation() != 0;
        m_boilingRequested = Settings::Pass::rtdiRtxdiBoiling() != 0;
        // The engine core's flags the RTXDI half does not read (the names RTDIEngineCore /
        // RTDIPass read: gen_catalog.py --flag) are reported as ignored=, never silently
        // dropped. The settings among them (the second radius; a command-line age cap or
        // two-pass request) join at ApplySettings (ComposeIgnored).
        static constexpr const char* kEngineReuseFlags[] = {
            "-di_unshadowed_reuse", "-di_permutation", "-di_area_lite", "-di_no_boiling_filter",
            "-di_keep_invisible", "-di_no_spatial_feedback", "-di_no_inverse_visibility",
            "-di_visibility_confidence", "-di_spatial_integrate" };
        static constexpr const char* kEngineInitialFlags[] = { "-di_no_initial_visibility" };
        auto ignore = [&](std::string& into, const char* f) { if (cmd.Has(f)) { if (!into.empty()) into += ' '; into += f; } };
        if (composition.reuse == ReSTIR::Backend::Rtxdi)
            for (const char* f : kEngineReuseFlags) ignore(m_ignoredReuse, f);
        if (composition.initial == ReSTIR::Backend::Rtxdi)
            for (const char* f : kEngineInitialFlags) ignore(m_ignoredInitial, f);
        ComposeIgnored(nullptr);
    }

    ReSTIR::Backend Backend() const override
    {
        return m_composition.initial == ReSTIR::Backend::Rtxdi && m_composition.reuse == ReSTIR::Backend::Rtxdi
            ? ReSTIR::Backend::Rtxdi
            : m_composition.initial == ReSTIR::Backend::Rtxdi ? ReSTIR::Backend::Hybrid : ReSTIR::Backend::Rtxdi;
    }

    bool BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell) override
    {
        if (HasPipelines()) return true;
        if (m_pipelinesFailed || !shell.IsValid()) return false;
        const auto start = std::chrono::steady_clock::now();
        if (m_engine && !m_engine->BuildPipelines(gfx, shell)) { m_pipelinesFailed = true; return false; }
        // Every kernel first (Initial's constants checked against its
        // reflection), then the pipelines together (ComputePipelineBatch).
        std::array<std::vector<uint8_t>, EntryCount> dxil;
        bool failed[EntryCount] = {};
        bool ok = true;
        for (uint32_t i = 0; i < EntryCount && ok; ++i)
        {
            dxil[i] = ShaderDiskCache::LoadOrCompile("shaders/RtxdiDI.cs.hlsl", kEntries[i], RHI::ShaderStage::CS, {}, "RTDIRtxdi");
            if (dxil[i].empty() || (i == Initial && !LayoutMatches(dxil[i]))) { failed[i] = true; ok = false; }
        }
        if (ok) ok = ComputePipelineBatch::Create(gfx, shell, dxil.data(), EntryCount, m_pso.data(), failed);
        if (!ok)
        {
            for (uint32_t i = 0; i < EntryCount; ++i)
                if (failed[i]) LOG_ERROR("RTDIPass: RTXDI pipeline %s failed", kEntries[i]);
            for (auto& p : m_pso) gfx.DestroyPipelineState(p);
            m_pipelinesFailed = true;
            return false;
        }
        m_pipelineMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
            + (m_engine ? m_engine->PipelineMs() : 0.0);
        return true;
    }
    bool HasPipelines() const override { return m_pso.back().IsValid() && (!m_engine || m_engine->HasPipelines()); }
    void ReleasePipelines(IGraphicsDevice& gfx) override
    {
        if (!m_adopted)
            for (auto& p : m_pso) gfx.DestroyPipelineState(p);
        m_pso = {};
        m_adopted = false;
        m_pipelinesFailed = false;
        if (m_engine) m_engine->ReleasePipelines(gfx);
    }
    void AdoptPipelines(const Core& from) override
    {
        const auto& other = static_cast<const RtxdiCore&>(from);
        m_pso = other.m_pso;
        m_adopted = true;
        if (m_engine && other.m_engine) m_engine->AdoptPipelines(*other.m_engine);
    }

    bool Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH) override
    {
        Release(gfx);
        m_tw = traceW; m_th = traceH;
        rtxdi::ImportanceSamplingContext_StaticParameters sp;
        sp.renderWidth = traceW; sp.renderHeight = traceH;
        sp.regirStaticParams.mode = rtxdi::ReGIRMode::Onion;
        m_isc = std::make_unique<rtxdi::ImportanceSamplingContext>(sp);
        ApplyResamplingMode();
        const RTXDI_ReservoirBufferParameters res = m_isc->GetReSTIRDIContext().GetReservoirBufferParameters();

        RHI::GPUBufferDesc bd{};
        bd.usage = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        bd.stride = sizeof(RTXDI_PackedDIReservoir);
        bd.size = uint64_t(res.reservoirArrayPitch) * rtxdi::c_NumReSTIRDIReservoirBuffers * bd.stride;
        bool ok = gfx.CreateBuffer(bd, m_reservoirs);
        bd.stride = 16; bd.size = uint64_t(traceW) * traceH * 2 * bd.stride;   // 32 B per cell
        for (auto& s : m_surfaces) ok = ok && gfx.CreateBuffer(bd, s);
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_TYPED;
        bd.format = RHI::Format::R32G32_UINT; bd.stride = 8;
        bd.size = uint64_t(std::max(1u, m_isc->GetRISBufferSegmentAllocator().GetTotalSizeInElements())) * 8;
        ok = ok && gfx.CreateBuffer(bd, m_ris);
        const uint32_t offsets = m_isc->GetNeighborOffsetCount();
        std::vector<uint8_t> offsetData(size_t(offsets) * 2);
        rtxdi::FillNeighborOffsetBuffer(offsetData.data(), offsets);
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.format = RHI::Format::R8G8_SNORM; bd.stride = 2; bd.size = offsetData.size();
        ok = ok && gfx.CreateBuffer(bd, m_neighborOffsets, offsetData.data());
        ok = ok && m_cb.Create(gfx, "RTDI.Rtxdi");
        if (ok && m_engine) ok = m_engine->Allocate(gfx, traceW, traceH);
        if (!ok) { LOG_ERROR("RTDIPass: RTXDI core allocation failed"); Release(gfx); return false; }
        m_reservoirState = m_risState = RHI::ResourceState::UNDEFINED;
        m_surfaceState.fill(RHI::ResourceState::UNDEFINED);
        m_offsetsState = RHI::ResourceState::UNDEFINED;
        m_surfaceSlot = 0; m_surfaceHistory = false; m_hasPrevView = false;
        m_allocated = true;
        LOG_INFO("[RTXDI] DI core allocated composition=%s trace=%ux%u reservoirs=%u ris=%u regirSlots=%u local=%s bias=%s/%s",
                 ReSTIR::CompositionName(m_composition), traceW, traceH, res.reservoirArrayPitch,
                 m_isc->GetRISBufferSegmentAllocator().GetTotalSizeInElements(),
                 m_isc->GetReGIRContext().GetReGIRLightSlotCount(), LocalName(), BiasName(m_temporalBias), BiasName(m_spatialBias));
        return true;
    }
    void Release(IGraphicsDevice& gfx) override
    {
        for (auto* b : { &m_reservoirs, &m_ris, &m_neighborOffsets, &m_surfaces[0], &m_surfaces[1] })
        { if (b->IsValid()) gfx.DestroyBuffer(*b); *b = {}; }
        if (m_cb.IsValid()) m_cb.Destroy(gfx);
        m_isc.reset();
        if (m_engine) m_engine->Release(gfx);
        m_allocated = false;
    }
    void ResetHistory() override { m_surfaceHistory = false; if (m_engine) m_engine->ResetHistory(); }

    bool ApplySettings(const CoreSettings& s, float distance, bool distanceChanged, bool& historyReset) override
    {
        bool realloc = false;
        historyReset = false;
        if (m_engine) realloc = m_engine->ApplySettings(s, distance, distanceChanged, historyReset);
        // The settings carry the -di_* command-line pins.
        const uint32_t passes = std::min(s.spatialPasses, 2u);
        const uint32_t radius = std::min(s.spatialRadius, 64u);
        const uint32_t k = std::clamp(s.spatialK, 1u, 8u);
        const uint32_t m = std::clamp(s.maxM, 1u, 63u);
        const bool reuse = s.reuse;
        const uint32_t candidates = std::clamp(s.candidates, 1u, 32u);
        ComposeIgnored(&s);
        if (passes != m_spatialPasses || radius != m_spatialRadius || k != m_spatialK || m != m_maxM || reuse != m_reuse)
        {
            historyReset = true;
            LOG_INFO("[RTDI_SETTINGS] backend=%s reuse=%u passes=%u radius=%u k=%u mCap=%u",
                     ReSTIR::CompositionName(m_composition), reuse ? 1u : 0u, passes, radius, k, m);
        }
        m_spatialPasses = passes; m_spatialRadius = radius; m_spatialK = k; m_maxM = m; m_reuse = reuse;
        m_candidates = candidates;
        ApplyResamplingMode();
        return realloc;
    }
    void ApplyBudgetStage(uint32_t stage) override { m_budgetStage = stage; if (m_engine) m_engine->ApplyBudgetStage(stage); }
    bool Reuse() const override { return RtxdiReuse() ? m_reuse : (m_engine && m_engine->Reuse()); }

    void FillConstants(IGraphicsDevice& gfx, Constants& c, const FillContext& ctx) override
    {
        if (m_engine) m_engine->FillConstants(gfx, c, ctx);
        if (RtxdiReuse())
        {
            // The engine core's reprojection deltas (RTDIEngineCore::FillConstants):
            // RAB_ScreenSpaceMotion reprojects a trace cell exactly as its Temporal does.
            c.historyJitterDelta[0] = c.historyJitterDelta[1] = 0;
            if (!(ctx.shellFlags & 16384u))
            { c.historyJitterDelta[0] = m_historyJitterX - ctx.jitterX; c.historyJitterDelta[1] = m_historyJitterY - ctx.jitterY; }
            c.historyJitterDelta[0] += float(int(ctx.phase & 1u) - int(m_historyPhase & 1u));
            c.historyJitterDelta[1] += float(int((ctx.phase >> 1) & 1u) - int((m_historyPhase >> 1) & 1u));
            c.historyReset = ctx.reset || !m_surfaceHistory;
            c.historyWidth = ctx.traceW; c.historyHeight = ctx.traceH;
        }
        if (!m_engine) c.candidates = m_candidates;
        m_jitterX = ctx.jitterX; m_jitterY = ctx.jitterY; m_phase = ctx.phase;
        if (!m_isc || !m_cb.IsValid()) return;

        auto index = [&](uint64_t h) { return gfx.DescriptorHeapIndexFromGpuHandle(h); };
        auto& di = m_isc->GetReSTIRDIContext();
        di.SetFrameIndex(ctx.frame);
        const SceneInputs scene = ctx.scene ? *ctx.scene : SceneInputs{};
        // Built here, then copied once: the constant buffer is write-combined.
        RtxdiConstants k{};

        RTXDI::BridgeConstants& b = k.bridge;
        std::memcpy(&b.invViewProj, c.invViewProj, sizeof(b.invViewProj));
        std::memcpy(&b.worldToView, c.worldToView, sizeof(b.worldToView));
        std::memcpy(&b.prevWorldToView, m_hasPrevView ? m_prevWorldToView : c.worldToView, sizeof(b.prevWorldToView));
        b.camera = {c.camera[0], c.camera[1], c.camera[2]};
        b.prevCamera = m_hasPrevView ? DirectX::XMFLOAT3{m_prevCamera[0], m_prevCamera[1], m_prevCamera[2]} : b.camera;
        b.width = c.width; b.height = c.height;
        b.depthIdx = c.depthIdx; b.normalIdx = c.normalIdx; b.albedoIdx = c.albedoIdx; b.surfaceIdx = c.surfaceIdx;
        b.velocityIdx = c.velocityIdx; b.materialsIdx = c.materialsIdx; b.lightsIdx = c.lightsIdx; b.lightCount = c.lightCount;
        // The previous light list (ClusterPass's light history ring; the current one
        // when the ring is not there yet, or the set did not change) and the shell's
        // key map.
        b.prevLightsIdx = scene.prevLightsIdx ? scene.prevLightsIdx : c.lightsIdx;
        b.prevLightCount = scene.prevLightsIdx ? scene.prevLightCount : c.lightCount;
        b.lightMapIdx = c.lightMapIdx;
        b.lightPdfIdx = scene.lightPdfIdx; b.lightPdfSize = scene.lightPdfSize; b.lightPdfMips = scene.lightPdfMips;
        const uint32_t cur = m_surfaceSlot, prev = cur ^ 1u;
        b.surfaceOutUav = index(gfx.GetBufferUAVGpuHandle(m_surfaces[cur]));
        b.prevSurfaceSrv = m_surfaceHistory ? index(gfx.GetBufferSRVGpuHandle(m_surfaces[prev])) : 0u;
        b.surfaceGridWidth = m_tw; b.surfaceGridHeight = m_th;
        b.rayTMax = c.rayTMax; b.alphaTestEnabled = c.alphaTestEnabled; b.alphaInstanceCount = c.alphaInstanceCount;

        k.runtime = di.GetRuntimeParams();
        k.lightBuffer.localLightBufferRegion.firstLightIndex = 0;
        k.lightBuffer.localLightBufferRegion.numLights = c.lightCount;
        k.localRIS = m_isc->GetLocalLightRISBufferSegmentParams();
        k.environmentRIS = m_isc->GetEnvironmentLightRISBufferSegmentParams();
        k.reservoirs = di.GetReservoirBufferParameters();
        k.bufferIndices = di.GetBufferIndices();
        k.initial = di.GetInitialSamplingParameters();
        k.initial.numLocalLightSamples = m_candidates;
        k.initial.numInfiniteLightSamples = 0;   // the sun is not a DI light (CSM / RT sun shadows)
        k.initial.numEnvironmentSamples = 0;     // the sky reaches the frame through IBL / DDGI
        k.initial.numBrdfSamples = 0;            // engine lights are not geometry a BRDF ray can hit
        k.initial.enableInitialVisibility = m_initialVisibility ? 1u : 0u;
        // Without this frame's PDF texture power RIS and ReGIR have nothing to
        // draw from: uniform over the list (counted in the [RTXDI] line).
        const bool tables = scene.lightPdfIdx != 0;
        k.initial.localLightSamplingMode = static_cast<ReSTIRDI_LocalLightSamplingMode>(
            tables ? m_localMode : ReSTIRDI_LocalLightSamplingMode_UNIFORM);
        k.temporal = di.GetTemporalResamplingParameters();
        k.temporal.maxHistoryLength = m_maxM;
        k.temporal.biasCorrectionMode = static_cast<ReSTIRDI_TemporalBiasCorrectionMode>(m_temporalBias);
        k.temporal.enablePermutationSampling = m_permutation ? 1u : 0u;
        k.boiling = di.GetBoilingFilterParameters();
        m_boiling = k.boiling.enableBoilingFilter != 0 && m_boilingRequested;
        k.spatial = di.GetSpatialResamplingParameters();
        k.spatial.numSamples = m_budgetStage >= 1 ? 1u : m_spatialK;
        k.spatial.samplingRadius = float(m_spatialRadius);
        k.spatial.biasCorrectionMode = static_cast<ReSTIRDI_SpatialBiasCorrectionMode>(m_spatialBias);
        k.shading = di.GetShadingParameters();
        auto& regir = m_isc->GetReGIRContext();
        auto dyn = regir.GetReGIRDynamicParameters();
        dyn.center = {c.camera[0], c.camera[1], c.camera[2]};
        regir.SetDynamicParameters(dyn);
        FillReGIR(regir, k.regir);
        k.reservoirUav = index(gfx.GetBufferUAVGpuHandle(m_reservoirs));
        k.risUav = index(gfx.GetBufferUAVGpuHandle(m_ris));
        k.neighborOffsetsSrv = index(gfx.GetBufferSRVGpuHandle(m_neighborOffsets));
        const bool spatial = m_spatialPasses > 0 && m_budgetStage < 2;
        k.rtxdiFlags = (m_reuse ? kFlagTemporal : 0u) | (m_reuse && spatial ? kFlagSpatial : 0u)
                     | (m_initialVisibility ? kFlagInitialVisibility : 0u);
        k.regirLightSlots = regir.GetReGIRLightSlotCount();
        k.localTiles = k.localRIS.tileCount;
        m_tables = tables;
        m_regir = tables && m_localMode == ReSTIRDI_LocalLightSamplingMode_REGIR_RIS;
        m_localTiles = k.localRIS.tileCount; m_localTileSize = k.localRIS.tileSize;
        m_regirSlots = k.regirLightSlots;
        m_spatialOn = spatial;
        *m_cb.Current(gfx) = k;

        std::memcpy(m_prevWorldToView, c.worldToView, sizeof(m_prevWorldToView));
        std::memcpy(m_prevCamera, c.camera, sizeof(m_prevCamera));
        m_hasPrevView = true;
    }
    void PatchStages(IGraphicsDevice& gfx, ConstantBlock& block) override { if (m_engine) m_engine->PatchStages(gfx, block); }

    void PreShell(const CoreFrame& f) override
    {
        if (m_engine) m_engine->PreShell(f);
        auto& gfx = *f.gfx; auto cl = f.cl;
        auto bufState = [&](RHI::GPUBuffer& b, RHI::ResourceState& before, RHI::ResourceState after) {
            if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, after), cl); before = after;
        };
        bufState(m_reservoirs, m_reservoirState, RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_ris, m_risState, RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_neighborOffsets, m_offsetsState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        const uint32_t cur = m_surfaceSlot, prev = cur ^ 1u;
        bufState(m_surfaces[cur], m_surfaceState[cur], RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_surfaces[prev], m_surfaceState[prev], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    }

    void Record(const CoreFrame& f) override
    {
        auto& gfx = *f.gfx;
        auto cl = f.cl;
        gfx.SetComputeRootCBV(ReSTIR::ShellSlots::CoreCB, m_cb.CurrentBuffer(gfx), 0, cl);
        auto uavBarrier = [&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
        auto indirect = [&](Entry e, const char* name) {
            const auto t = gfx.BeginGPUTimestamp(cl, name);
            gfx.BindComputePipelineState(m_pso[e], cl);
            gfx.DispatchComputeIndirect(*f.args, 0, cl);
            gfx.EndGPUTimestamp(cl, t); uavBarrier();
        };
        auto direct = [&](Entry e, uint32_t threads, const char* name) {
            const auto t = gfx.BeginGPUTimestamp(cl, name);
            gfx.BindComputePipelineState(m_pso[e], cl);
            gfx.DispatchCompute((threads + 255u) / 256u, 1, 1, cl);
            gfx.EndGPUTimestamp(cl, t); uavBarrier();
        };
        const bool rtxdiInitial = m_composition.initial == ReSTIR::Backend::Rtxdi;
        if (rtxdiInitial)
        {
            if (m_tables && m_localMode != ReSTIRDI_LocalLightSamplingMode_UNIFORM)
                direct(PresampleLocal, m_localTiles * m_localTileSize, f.regions->presample);
            if (m_regir && m_regirSlots) direct(PresampleReGIR, m_regirSlots, f.regions->regir);
            indirect(Initial, f.regions->initialShade);
        }
        else
        {
            m_engine->RecordInitial(f);
            gfx.SetComputeRootCBV(ReSTIR::ShellSlots::CoreCB, m_cb.CurrentBuffer(gfx), 0, cl);
        }
        if (!RtxdiReuse())
        {
            // hybrid: RTXDI's candidates into the engine core's Initial slot,
            // then its visibility-aware reuse and Shade. With the reuse off the
            // engine core has no reuse chain (its Initial shades by itself,
            // and its reservoirs are not allocated): RTXDI's selection shades.
            if (m_engine->Reuse())
            {
                indirect(ToEngine, f.regions->convert);
                m_engine->RecordReuse(f);
            }
            else indirect(Shade, f.regions->shade);
        }
        else
        {
            if (!rtxdiInitial) indirect(FromEngine, f.regions->convert);
            if (m_reuse) indirect(Temporal, f.regions->temporal);
            if (m_reuse && m_boiling)
            {
                // RTXDI's boiling filter averages 16x16 thread groups: the whole
                // trace grid, not the compacted list.
                const auto t = gfx.BeginGPUTimestamp(cl, f.regions->boiling);
                gfx.BindComputePipelineState(m_pso[Boiling], cl);
                gfx.DispatchCompute((m_tw + 15u) / 16u, (m_th + 15u) / 16u, 1, cl);
                gfx.EndGPUTimestamp(cl, t); uavBarrier();
            }
            if (m_reuse && m_spatialOn) indirect(Spatial, f.regions->spatial);
            indirect(Shade, f.regions->shade);
            m_historyJitterX = f.jitterX; m_historyJitterY = f.jitterY; m_historyPhase = f.phase;
        }
        // The surfaces this frame stored are next frame's previous ones.
        m_surfaceSlot ^= 1u;
        m_surfaceHistory = true;
    }

    const char* InertReason() const override
    {
        if (!HasPipelines()) return "pipeline";
        if (!m_allocated) return "resources";
        if (m_engine) if (const char* why = m_engine->InertReason()) return why;
        return nullptr;
    }
    bool IsAllocated() const override { return m_allocated; }
    const char* IgnoredFlags() const override { return m_ignored.c_str(); }
    uint64_t ResidentBytes() const override
    {
        uint64_t bytes = 0;
        if (m_allocated && m_isc)
        {
            bytes += uint64_t(m_isc->GetReSTIRDIContext().GetReservoirBufferParameters().reservoirArrayPitch)
                   * rtxdi::c_NumReSTIRDIReservoirBuffers * sizeof(RTXDI_PackedDIReservoir);
            bytes += uint64_t(m_isc->GetRISBufferSegmentAllocator().GetTotalSizeInElements()) * 8;
            bytes += uint64_t(m_isc->GetNeighborOffsetCount()) * 2;
            bytes += 2ull * m_tw * m_th * 32;
        }
        if (m_engine) bytes += m_engine->ResidentBytes();
        return bytes;
    }
    double PipelineMs() const override { return m_pipelineMs; }
    bool Feedback() const override { return RtxdiReuse() ? false : (m_engine && m_engine->Feedback()); }
    bool VisibilityMIS() const override { return RtxdiReuse() ? false : (m_engine && m_engine->VisibilityMIS()); }

private:
    bool RtxdiReuse() const { return m_composition.reuse == ReSTIR::Backend::Rtxdi; }
    // The ignored= report, always in this order: the engine reuse flags, the second
    // radius, a command-line age cap, the engine initial flags, a command-line
    // two-pass request.
    void ComposeIgnored(const CoreSettings* s)
    {
        m_ignored = m_ignoredReuse;
        auto add = [this](const std::string& f) { if (!m_ignored.empty()) m_ignored += ' '; m_ignored += f; };
        if (RtxdiReuse() && Settings::Pass::rtdiSpatialRadius2Given()) add("-di_spatial_radius2");
        if (RtxdiReuse() && s && s->ageCapPinned) add("-di_age_cap");
        if (!m_ignoredInitial.empty()) add(m_ignoredInitial);
        if (RtxdiReuse() && s && s->spatialPassesPinned && s->spatialPasses == 2) add("-di_spatial_passes=2");
    }
    const char* LocalName() const
    {
        return m_localMode == ReSTIRDI_LocalLightSamplingMode_UNIFORM ? "uniform"
             : m_localMode == ReSTIRDI_LocalLightSamplingMode_POWER_RIS ? "power" : "regir";
    }
    void ApplyResamplingMode()
    {
        if (!m_isc) return;
        auto mode = rtxdi::ReSTIRDI_ResamplingMode::None;
        if (m_reuse) mode = m_spatialPasses > 0 ? rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial
                                                : rtxdi::ReSTIRDI_ResamplingMode::Temporal;
        m_isc->GetReSTIRDIContext().SetResamplingMode(mode);
    }

    uint32_t m_shellFlags = 0;
    ReSTIR::DIComposition m_composition;
    std::unique_ptr<EngineCore> m_engine;
    std::unique_ptr<rtxdi::ImportanceSamplingContext> m_isc;
    std::array<RHI::PipelineState, EntryCount> m_pso;
    bool m_pipelinesFailed = false;
    bool m_adopted = false;   // m_pso is another core's (AdoptPipelines)
    double m_pipelineMs = 0.0;
    RHI::GPUBuffer m_reservoirs, m_ris, m_neighborOffsets;
    std::array<RHI::GPUBuffer, 2> m_surfaces;
    RHI::ResourceState m_reservoirState = RHI::ResourceState::UNDEFINED, m_risState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_offsetsState = RHI::ResourceState::UNDEFINED;
    std::array<RHI::ResourceState, 2> m_surfaceState{};
    FrameCB<RtxdiConstants> m_cb;
    uint32_t m_tw = 0, m_th = 0;
    bool m_allocated = false;
    // Settings (engine.json / CLI).
    uint32_t m_candidates = 8, m_spatialPasses = 1, m_spatialRadius = 16, m_spatialK = 3, m_maxM = 20;
    bool m_reuse = true;
    uint32_t m_localMode = ReSTIRDI_LocalLightSamplingMode_REGIR_RIS;
    uint32_t m_temporalBias = RTXDI_BIAS_CORRECTION_RAY_TRACED, m_spatialBias = RTXDI_BIAS_CORRECTION_RAY_TRACED;
    bool m_initialVisibility = true, m_permutation = true, m_boilingRequested = true, m_boiling = true;
    uint32_t m_budgetStage = 0;
    std::string m_ignored, m_ignoredReuse, m_ignoredInitial;   // the report; the flags given (ctor)
    // This frame's decisions (FillConstants -> Record).
    bool m_tables = false, m_regir = false, m_spatialOn = true;
    uint32_t m_localTiles = 0, m_localTileSize = 0, m_regirSlots = 0;
    // History.
    uint32_t m_surfaceSlot = 0;
    bool m_surfaceHistory = false;
    float m_jitterX = 0, m_jitterY = 0, m_historyJitterX = 0, m_historyJitterY = 0;
    uint32_t m_phase = 0, m_historyPhase = 0;
    float m_prevWorldToView[16] = {}, m_prevCamera[3] = {};
    bool m_hasPrevView = false;
};
} // namespace

std::unique_ptr<Core> MakeRtxdiCore(uint32_t shellFlags, const ReSTIR::DIComposition& composition)
{
    if (composition.initial != ReSTIR::Backend::Rtxdi && composition.reuse != ReSTIR::Backend::Rtxdi) return nullptr;
    return std::make_unique<RtxdiCore>(shellFlags, composition);
}
} // namespace RTDI

#else

namespace RTDI
{
std::unique_ptr<Core> MakeRtxdiCore(uint32_t, const ReSTIR::DIComposition&) { return nullptr; }
} // namespace RTDI

#endif
