#include "RenderGraph/RenderPass/PTRtxdiCore.h"
#include "Settings/PassSettings.h"     // pass.ptRtxdi.*
#include "System/CommandLine.h"

#include <atomic>
#include <cstring>
#include <iterator>

namespace PTRtxdi
{
namespace
{
std::atomic<bool> g_realtimePreset{false};
std::atomic<uint8_t> g_directLight{static_cast<uint8_t>(DirectLight::Auto)};
constexpr const char* kDirectLightNames[] = { "auto", "nee", "restir_di", "unified" };
static_assert(std::size(kDirectLightNames) == static_cast<size_t>(DirectLight::Count));
}
void SetRealtimePreset(bool on) { g_realtimePreset.store(on, std::memory_order_relaxed); }
bool RealtimePreset() { return g_realtimePreset.load(std::memory_order_relaxed); }
const char* DirectLightName(DirectLight d)
{
    const size_t i = static_cast<size_t>(d);
    return i < std::size(kDirectLightNames) ? kDirectLightNames[i] : "auto";
}
bool ParseDirectLight(const char* token, DirectLight& out)
{
    if (!token) return false;
    for (size_t i = 0; i < std::size(kDirectLightNames); ++i)
        if (std::strcmp(token, kDirectLightNames[i]) == 0) { out = static_cast<DirectLight>(i); return true; }
    return false;
}
void SetDirectLight(DirectLight d) { g_directLight.store(static_cast<uint8_t>(d), std::memory_order_relaxed); }
DirectLight GetDirectLight() { return static_cast<DirectLight>(g_directLight.load(std::memory_order_relaxed)); }
DirectLight EffectiveDirectLight()
{
    // The setting first. raytracing.ptDirectLight also answers -rtxdi_pt_unified=1 (x1's direct
    // light in the PT reservoir) and -rtxdi_pt_di=0|1 (the core's own sample or ReSTIR DI).
    const DirectLight setting = GetDirectLight();
    if (setting != DirectLight::Auto) return setting;
    // Auto: ReSTIR DI on both paths. The core's own sample is ~3.4 ms cheaper than a
    // half-resolution DI pass, but DLSS-RR boils what that hands it; raw full-resolution DI gives
    // RR ~9x less noise on a many-light floor and keeps its energy (with the core's sample RR
    // overshoots by ~20%).
    return DirectLight::RestirDI;
}
bool UsesRestirDI() { return EffectiveDirectLight() == DirectLight::RestirDI; }
} // namespace PTRtxdi

#if ENGINE_WITH_RTXDI
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/RenderPass/RtxdiBridge.h"
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderReflection.h"
#include "Graphics/SharcCache.h"
#include "System/Log.h"
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/PT/ReSTIRPT.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace PTRtxdi
{
namespace
{
// RtxdiBridgePTConstants.hlsli's RAB_PTConstants, member for member.
struct alignas(16) EngineConstants
{
    DirectX::XMFLOAT3 sunDirection; float sunCosRadius;
    DirectX::XMFLOAT3 sunColor; uint32_t skyIdx;
    float environmentScale; float constantEnvironment; uint32_t ptFlags; uint32_t pathMaterialsIdx;
    uint32_t lightAliasIdx, activeLocalLightCount, resultUav, accumUav;
    uint32_t diResultIdx, reservoirUav, neighborOffsetsSrv, counterUav;
    uint32_t spp, frameIndex, mediumIdx, mediaFlags;
    DirectX::XMFLOAT3 prevPrevCamera; float pad2;
    uint32_t sampleIdUav, dupMapUav, pairScratchUav, pairListUav;
    uint32_t pairArgsUav, rgbUav, pairCount, pairCounterUav;
    uint32_t neeCandidates, neeCandidatesX1, stagnancyUav, prevStagnancyUav;   // the smoothed stagnancy
    uint32_t rrAlbedoUav, rrSpecularUav, rrNormalUav, rrDistanceUav;   // the DLSS-RR guides
    // The SHaRC radiance cache (RtxdiBridgeSharc.hlsli).
    uint32_t sharcHashUav, sharcAccumulationUav, sharcResolvedUav, sharcCapacity;
    float sharcSceneScale, sharcRadianceScale; uint32_t sharcUpdateTile, sharcUpdateBounces;
    DirectX::XMFLOAT3 sharcPrevCamera; uint32_t sharcAccumulationFrames;
    uint32_t sharcStaleFrames, sharcFrame, sharcFlags; float sharcMinRoughness;
    uint32_t pairing[4][4];                 // per pairing: t.xy, m.zw
    DirectX::XMFLOAT4X4 prevViewProj;       // transposed, as the bridge's matrices
    // The froxel fog's last lit volume (kRabMediaFroxel): its view-projection as the fog's own
    // constants hold it, that camera, the SRV, the range.
    DirectX::XMFLOAT4X4 froxelViewProj;
    DirectX::XMFLOAT3 froxelCamera; uint32_t froxelHistoryIdx;
    float froxelNear, froxelFar; uint32_t froxelPad[2];
};
static_assert(sizeof(EngineConstants) == 464, "mirror of RAB_PTConstants");
// RtxdiPT.cs.hlsl's cbuffer Params (b0).
struct alignas(16) Constants
{
    RTXDI::BridgeConstants bridge;
    RTXDI_RuntimeParameters runtime;
    RTXDI_PTParameters restirPT;     // g_Const.restirPT
    EngineConstants pt;              // g_PT
};
// ptFlags (RtxdiBridgePTConstants.hlsli).
constexpr uint32_t kDiffuse = 1u, kConstantEnvironment = 2u, kNoSunNee = 4u, kCounters = 8u, kAccumulate = 16u, kDecorrelation = 32u,
                   kDirectOnly = 64u, kDiResult = 128u, kReplayNoRc = 256u, kUnified = 512u, kDupMap = 1024u,
                   kDualMV = 2048u, kRgb = 4096u, kCompaction = 8192u, kRRGuides = 16384u;
// Above the RR corrupt-guide field (bits 17-19).
constexpr uint32_t kGlass = 1u << 20;
constexpr uint32_t kCounterCount = 13;   // RtxdiPT.cs.hlsl's kPTCounterCount (8-10: SHaRC, 11-12: the RR recipe)

enum Entry : uint32_t { Clear, Initial, Temporal, Boiling, Spatial, Shade, DupMap, PairShift, PairArgs, PairReplay,
                        PairResample, DupInputs, DupSmooth, Firefly, EntryCount };
constexpr const char* kEntries[EntryCount] = {
    "RtxdiPTClear", "RtxdiPTInitial", "RtxdiPTTemporal", "RtxdiPTBoiling", "RtxdiPTSpatial", "RtxdiPTShade",
    "RtxdiPTDupMapCount", "RtxdiPTPairShift", "RtxdiPTPairArgs", "RtxdiPTPairReplay", "RtxdiPTPairResample",
    "RtxdiPTDupInputs", "RtxdiPTDupSmooth", "RtxdiPTFirefly" };
constexpr const char* kRegions[EntryCount] = {
    "PT.Rtxdi.Clear", "PT.Rtxdi.Initial", "PT.Rtxdi.Temporal", "PT.Rtxdi.Boiling", "PT.Rtxdi.Spatial", "PT.Rtxdi.Shade",
    "PT.Rtxdi.DupMap", "PT.Rtxdi.PairShift", "PT.Rtxdi.PairArgs", "PT.Rtxdi.PairReplay", "PT.Rtxdi.PairResample",
    "PT.Rtxdi.DupInputs", "PT.Rtxdi.DupSmooth", "PT.Rtxdi.Firefly" };
// The SHaRC radiance cache's kernels (RtxdiPT.cs.hlsl under RAB_SHARC).
enum SharcEntry : uint32_t { SharcClear, SharcUpdate, SharcResolve, SharcEntryCount };
constexpr const char* kSharcEntries[SharcEntryCount] = { "RtxdiPTSharcClear", "RtxdiPTSharcUpdate", "RtxdiPTSharcResolve" };
constexpr const char* kSharcRegions[SharcEntryCount] = { "PT.Sharc.Clear", "PT.Sharc.Update", "PT.Sharc.Resolve" };
// The kernels RAB_MEDIA changes (the ones that weigh a segment) and, with the cache, the cache's
// update; every other kernel compiles to the same code with it and keeps its plain build.
constexpr Entry kMediaEntries[] = { Initial, Temporal, Spatial, Shade, PairShift, PairReplay };
constexpr uint32_t kMediaEntryCount = uint32_t(sizeof(kMediaEntries) / sizeof(kMediaEntries[0]));
// kRabMedia* (RtxdiBridgePTConstants.hlsli), for the log.
constexpr const char* kMediaRules[] = { "absorption", "none", "full", "froxel" };

struct Expect { const char* name; uint32_t offset, size; };
#define PT_CORE_VAR(hlsl, member) { hlsl, uint32_t(offsetof(Constants, member)), uint32_t(sizeof(Constants::member)) }
const Expect kLayout[] = {
    PT_CORE_VAR("g_RabBridge", bridge), PT_CORE_VAR("g_Runtime", runtime),
    PT_CORE_VAR("g_Const", restirPT), PT_CORE_VAR("g_PT", pt),
};
#undef PT_CORE_VAR

bool LayoutMatches(const std::vector<uint8_t>& dxil)
{
    ShaderReflect::Reflection r;
    if (!ShaderReflect::Reflect(dxil.data(), dxil.size(), r)) { LOG_ERROR("[RTXDI] PT core: no reflection"); return false; }
    const ShaderReflect::CBufferLayout* cb = nullptr;
    for (const auto& c : r.cbuffers) if (c.name == "Params") cb = &c;
    if (!cb) { LOG_ERROR("[RTXDI] PT core: cbuffer Params not reflected"); return false; }
    bool ok = true;
    for (const auto& e : kLayout)
    {
        const ShaderReflect::CBufferVar* v = nullptr;
        for (const auto& var : cb->vars) if (var.name == e.name) v = &var;
        if (!v || v->offset != e.offset || v->size != e.size)
        {
            LOG_ERROR("[RTXDI] PT core layout %s: hlsl offset=%u size=%u, c++ offset=%u size=%u", e.name,
                      v ? v->offset : 0u, v ? v->size : 0u, e.offset, e.size);
            ok = false;
        }
    }
    if (cb->sizeBytes != ((sizeof(Constants) + 15) & ~size_t(15)))
    {
        LOG_ERROR("[RTXDI] PT core layout: cbuffer %u bytes, c++ %zu", cb->sizeBytes, sizeof(Constants));
        ok = false;
    }
    return ok;
}

const char* ReuseName(rtxdi::ReSTIRPT_ResamplingMode m)
{
    switch (m)
    {
    case rtxdi::ReSTIRPT_ResamplingMode::None: return "none";
    case rtxdi::ReSTIRPT_ResamplingMode::Temporal: return "temporal";
    case rtxdi::ReSTIRPT_ResamplingMode::Spatial: return "spatial";
    default: return "temporal_spatial";
    }
}

// @p entry's slot among kMediaEntries, or -1.
int MediaSlot(Entry entry)
{
    for (uint32_t i = 0; i < kMediaEntryCount; ++i)
        if (kMediaEntries[i] == entry) return int(i);
    return -1;
}

class RtxdiPTCore final : public Core
{
public:
    RtxdiPTCore()
    {
        // FullSample's "Medium" preset where the engine has no reason to differ. Every knob is a
        // pass row (pass.ptRtxdi.*), read when the core is made; each row also answers its
        // -rtxdi_pt_* flag.
        namespace PS = Settings::Pass;
        const auto& cmd = Sys::CommandLine::Get();
        const std::string reuse = PS::ptRtxdiReuse();
        m_mode = reuse == "none" ? rtxdi::ReSTIRPT_ResamplingMode::None
               : reuse == "temporal" ? rtxdi::ReSTIRPT_ResamplingMode::Temporal
               : reuse == "spatial" ? rtxdi::ReSTIRPT_ResamplingMode::Spatial
               : rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        // 3 vertices (FullSample's Medium), 5 in the real-time preset: paths through glossy
        // vertices, which the cache does not end, reach one it does (within 2% of the reference).
        const bool realtime = RealtimePreset();
        m_bounces = uint32_t(std::clamp(PS::ptRtxdiBouncesGiven() ? PS::ptRtxdiBounces() : (realtime ? 5 : 3), 2, 64));   // the reservoir stores 8-bit lengths
        m_rcLength = uint32_t(std::clamp(PS::ptRtxdiRcLength(), 2, 32));
        m_initialSamples = uint32_t(std::clamp(PS::ptRtxdiInitialSamples(), 1, 8));
        m_spatialSamples = uint32_t(std::clamp(PS::ptRtxdiSpatialSamples(), 1, 8));
        m_radius = std::clamp(PS::ptRtxdiRadius(), 1.f, 128.f);
        m_maxHistory = uint32_t(std::clamp(PS::ptRtxdiHistory(), 1, 64));
        // RTXDI's boiling filter discards reservoirs much brighter than their 16x16 group; where
        // valid paths are rare those are the right samples (under RR it cost a sunlit room 13% of its
        // light, the floor 34%). Off by default for the energy contract; 1 always, -1 while DLSS-RR
        // takes the guides. The strength s (0.2, RTXDI's) sets the bound, (10 / s - 9) x the group's
        // mean.
        m_boiling = std::clamp(PS::ptRtxdiBoiling(), -1, 1);
        m_boilingStrength = std::clamp(PS::ptRtxdiBoilingStrength(), 0.001f, 1.f);
        m_footprint = PS::ptRtxdiReconnection() != "fixed";
        // The final shading is targetFunction x W: every target that reaches it
        // must include the reconnection's visibility, the temporal one too.
        m_temporalVisibility = PS::ptRtxdiTemporalVisibility() != 0;
        // RTXDI's permutation sampling (the first temporal tap XOR-permuted in a 4x4 block by a
        // frame-uniform offset): off by default, as RTXDI ships it. Under RR it decorrelates the
        // temporal history, but a rare bright path then wanders the block and the pairwise pass
        // copies it into a 1000-2000 px blot for a few frames, and every outlier filter that removes
        // those costs 6-13% of the light. 1 always, -1 while DLSS-RR takes the guides.
        m_permutation = std::clamp(PS::ptRtxdiPermutation(), -1, 1);
        // RTXDI's DLSS-RR recipe (its GetDefaultReSTIRPT*Params(true)): a 20-frame history cut where
        // the duplication map finds a sample shared, no age rejection, and decorrelation at the final
        // shading - a reused sample is shaded with the pixel's own preserved initial sample instead,
        // so RR gets independent noise rather than a blot the reuse copied around. On while DLSS-RR
        // takes the guides; 0 / 1 pins it.
        m_rrRecipe = std::clamp(PS::ptRtxdiRrRecipe(), -1, 1);
        // Which pixels decorrelate. RTXDI's DLSS-RR default (its stagnancy rule plus the firefly
        // replacement, a fresh reservoir's replacement clamped to 15x the reused weight) chooses by
        // the sample it would replace, and a reservoir is fresh exactly when this frame's bright
        // initial sample won: that loses ~3% of a room's light. Default: RTXDI's Uniform mode at 0.85
        // (the share its stagnancy rule reaches in a still scene) with neither filter - unbiased.
        // decorrelation=stagnancy (factor 0.4), firefly=1 (strength 0.7) and fresh=clamp are RTXDI's
        // as it ships them. The recipe's history (20, the duplication map's cut, no age rejection)
        // applies unless the history, duplication map or age rejection is given.
        m_decoUniform = PS::ptRtxdiDecorrelation() != "stagnancy";
        m_decoFactor = std::clamp(PS::ptRtxdiDecorrelationFactorGiven() ? PS::ptRtxdiDecorrelationFactor()
                                                                        : (m_decoUniform ? 0.85f : 0.4f), 0.f, 1.f);
        m_firefly = PS::ptRtxdiFirefly() != 0;
        m_fireflyStrength = std::clamp(PS::ptRtxdiFireflyStrength(), 0.001f, 1.f);
        m_fresh = PS::ptRtxdiFresh() == "clamp" ? Fresh::Clamp : Fresh::Replace;
        m_historyPinned = PS::ptRtxdiHistoryGiven();
        m_dupMapPinned = PS::ptRtxdiDuplicationMapGiven();
        m_ageRejectionPinned = cmd.Has("-rtxdi_pt_age_rejection");
        m_useDI = UsesRestirDI();
        // The library's hybrid-shift replay of a path with no reconnection vertex: the bridge
        // refuses it by default (RtxdiBridgePT.hlsli); =1 lets it through for an A/B.
        m_replayNoRc = cmd.GetInt("-rtxdi_pt_replay_no_rc", 0) != 0;
        // The spatial pass's neighbour count must not depend on the samples: RTXDI's disocclusion
        // boost (CalculateNumSamples) takes 8 neighbours only when the canonical sample reconnects at
        // x2 with a short history, so a pixel with a sample lends its MIS weight to 8 mostly empty
        // neighbours while an empty one borrows from 1 - darkening where samples are sparse (-15%).
        // By default the boost equals the plain count (a fixed neighbour set, unbiased);
        // -rtxdi_pt_boost=N sets it for an A/B.
        m_boost = uint32_t(std::clamp(PS::ptRtxdiBoostGiven() ? PS::ptRtxdiBoost() : int(m_spatialSamples), 1, 32));
        // RTXDI's age-based temporal rejection drops a history by its sample's
        // age (non-RR default on); -rtxdi_pt_age_rejection=0 for the A/B.
        m_ageRejection = cmd.GetInt("-rtxdi_pt_age_rejection", 1) != 0;
        // Engine kernels and hooks: the occluder's motion for disoccluded pixels (dualMv, on: -9%
        // noise where a moving object uncovers the floor, a static scene is bit-identical); x1's
        // direct light in the PT reservoir (unified, off: ~3 ms cheaper without the DI pass, but
        // noisier); RTXDI's duplication map outside the DLSS-RR recipe (off: biased and noisier).
        m_unified = EffectiveDirectLight() == DirectLight::Unified;
        m_dupMap = PS::ptRtxdiDuplicationMap() != 0;
        m_dualMv = PS::ptRtxdiDualMv() != 0;
        // The engine's pairwise spatial pass in place of RTXDI's, with compacted random replays and
        // an RGB estimate: faster (spatial stage 6.9 -> 4.4 ms) and less noisy; up to 4 pairings
        // (the spatial sample count). spatial=rtxdi selects the library's pass.
        m_pairwise = PS::ptRtxdiSpatial() != "rtxdi";
        m_compaction = m_pairwise && PS::ptRtxdiCompaction() != 0;
        m_rgb = m_pairwise && PS::ptRtxdiRgb() != 0;
        if (m_pairwise) m_spatialSamples = std::min(m_spatialSamples, 4u);
        // Local-light next-event candidates (power RIS, RTXDI Medium's NEE) at the path's vertices
        // (4 buys -12% FLIP for the time the pairwise pass saves), and at x1 when it is in the
        // reservoir.
        m_neeCandidates = uint32_t(std::clamp(PS::ptRtxdiNeeCandidates(), 1, 32));
        m_neeCandidatesX1 = uint32_t(std::clamp(PS::ptRtxdiNeeCandidatesX1(), 1, 32));
        // The SHaRC radiance cache the paths end in (RtxdiBridgeSharc.hlsli): off unless asked;
        // the real-time preset asks.
        m_sharcWanted = (PS::ptRtxdiSharcGiven() ? PS::ptRtxdiSharc() : (realtime ? 1 : 0)) != 0;
        // The shared -sharc_* flags (Graphics/SharcCache.h): 2^22 entries, scene scale 50, tile 5,
        // 3 update bounces, 20 / 60 frames, radiance x1e4. Fixed-point deposits truncate: upstream's
        // 1e3 loses a few % in a dark scene; 1e4 keeps 1e-4 resolution and still holds 4e5 per
        // channel per entry per frame.
        {
            const SharcCache::Config sharc = SharcCache::FromCommandLine(SharcCache::Config{});
            m_sharcCapacityLog2 = sharc.capacityLog2;
            m_sharcSceneScale = sharc.sceneScale;
            m_sharcTile = sharc.updateTile;
            m_sharcBounces = sharc.updateBounces;
            m_sharcAccumulationFrames = sharc.accumulationFrames;
            m_sharcStaleFrames = sharc.staleFrames;
            m_sharcRadianceScale = sharc.radianceScale;
        }
        m_sharcDiffuseOnly = cmd.GetInt("-sharc_diffuse_only", 0) != 0;   // diagnostic: no specular-cone termination
        // -dlss_rr_corrupt_guide (tools/verify_rr_truth.py): one RR guide filled with noise.
        {
            const std::string g = cmd.Get("-dlss_rr_corrupt_guide", "");
            m_rrCorrupt = g == "albedo" ? 1u : g == "specular" ? 2u : g == "normal" ? 3u : g == "distance" ? 4u : 0u;
        }
        m_sharcFootprintOnly = cmd.GetInt("-sharc_footprint_only", 0) != 0; // diagnostic: no last-vertex fallback
        // A vertex ends a path in the cache only when its outgoing radiance is nearly
        // view-independent (rough, not metal): the cache keeps one value per voxel.
        m_sharcMinRoughness = std::clamp(PS::ptRtxdiSharcMinRoughness(), 0.f, 1.f);
        m_sharcGateLast = PS::ptRtxdiSharcGateLast() != 0;
        char firefly[16] = "off";
        if (m_firefly) std::snprintf(firefly, sizeof(firefly), "%.2f", m_fireflyStrength);
        std::snprintf(m_describe, sizeof(m_describe),
            "reuse=%s bounces=%u rcLength=%u initial=%u spatial=%u boost=%u radius=%.0f history=%u ageRejection=%u boiling=%s reconnection=%s temporalVisibility=%u permutation=%s rrRecipe=%s decorrelation=%s:%.2f firefly=%s fresh=%s di=%u noRcReplay=%s unified=%u dupMap=%u dualMv=%u spatialMode=%s compaction=%u rgb=%u nee=%u neeX1=%u sharc=%u",
            ReuseName(m_mode), m_bounces, m_rcLength, m_initialSamples, m_spatialSamples, m_boost, m_radius, m_maxHistory,
            m_ageRejection ? 1u : 0u, m_boiling < 0 ? "rr" : m_boiling ? "1" : "0", m_footprint ? "footprint" : "fixed", m_temporalVisibility ? 1u : 0u,
            m_permutation < 0 ? "rr" : m_permutation ? "1" : "0", m_rrRecipe < 0 ? "rr" : m_rrRecipe ? "1" : "0",
            m_decoUniform ? "uniform" : "stagnancy", m_decoFactor, firefly,
            m_fresh == Fresh::Clamp ? "clamp" : "replace", m_useDI ? 1u : 0u, m_replayNoRc ? "library" : "refused", m_unified ? 1u : 0u, m_dupMap ? 1u : 0u, m_dualMv ? 1u : 0u,
            m_pairwise ? "pairwise" : "rtxdi", m_compaction ? 1u : 0u, m_rgb ? 1u : 0u, m_neeCandidates, m_neeCandidatesX1,
            m_sharcWanted ? 1u : 0u);
    }

    bool BuildPipelines(IGraphicsDevice& gfx) override
    {
        if (HasPipelines()) return true;
        if (m_pipelinesFailed) return false;
        m_pipelinesFailed = true;   // cleared once every pipeline is built
        const auto start = std::chrono::steady_clock::now();
        if (!m_layout.IsValid() && !ReSTIR::CreateShellLayout(gfx, m_layout, "PTRtxdi"))
        { LOG_ERROR("PathTracePass: RTXDI PT pipeline layout failed"); return false; }
        // Every kernel first (Shade's constants checked against its
        // reflection), then the pipelines together (ComputePipelineBatch). The
        // pairwise replay's indirect dispatch uses the device's signature.
        std::array<std::vector<uint8_t>, EntryCount> dxil;
        bool failed[EntryCount] = {};
        bool ok = true;
        for (uint32_t i = 0; i < EntryCount && ok; ++i)
        {
            dxil[i] = ShaderDiskCache::LoadOrCompile("shaders/RtxdiPT.cs.hlsl", kEntries[i], RHI::ShaderStage::CS, {},
                                                     "PTRtxdi", ShaderDiskCache::CompileFlags{true, true});
            if (dxil[i].empty() || (i == Shade && !LayoutMatches(dxil[i]))) { failed[i] = true; ok = false; }
        }
        if (ok) ok = ComputePipelineBatch::Create(gfx, m_layout, dxil.data(), EntryCount, m_pso.data(), failed);
        if (!ok)
        {
            for (uint32_t i = 0; i < EntryCount; ++i)
                if (failed[i]) LOG_ERROR("PathTracePass: RTXDI PT pipeline %s failed", kEntries[i]);
            for (auto& p : m_pso) gfx.DestroyPipelineState(p);
            return false;
        }
        BuildSharcPipelines(gfx);
        m_pipelinesFailed = false;
        m_pipelineMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }
    bool HasPipelines() const override { return m_pso.back().IsValid(); }
    void ReleasePipelines(IGraphicsDevice& gfx) override
    {
        for (auto& p : m_pso) gfx.DestroyPipelineState(p);
        for (auto& p : m_psoQuery) gfx.DestroyPipelineState(p);
        for (auto& p : m_sharcPso) gfx.DestroyPipelineState(p);
        ReleaseMediaPipelines(gfx);
        gfx.DestroyPipelineLayout(m_layout);
        m_sharc = false; m_sharcLogged = false;
        m_pipelinesFailed = false;
        m_mediaFailed = false;
    }

    bool BuildMediaPipelines(IGraphicsDevice& gfx) override
    {
        if (m_mediaPso.back().IsValid()) return true;
        if (m_mediaFailed || !HasPipelines()) return false;
        m_mediaFailed = true;   // cleared once every pipeline is built
        const auto start = std::chrono::steady_clock::now();
        auto load = [&](const char* entry, std::vector<DxcCompiler::Define> defines) {
            return ShaderDiskCache::LoadOrCompile("shaders/RtxdiPT.cs.hlsl", entry, RHI::ShaderStage::CS, defines,
                                                  "PTRtxdi", ShaderDiskCache::CompileFlags{true, true});
        };
        // Every kernel first (Shade's constants checked against its reflection), then each set's pipelines
        // together; with the cache, its builds of the same kernels and its update.
        std::array<std::vector<uint8_t>, kMediaEntryCount> plain, query;
        std::vector<uint8_t> update;
        bool ok = true;
        for (uint32_t i = 0; i < kMediaEntryCount && ok; ++i)
        {
            const Entry entry = kMediaEntries[i];
            plain[i] = load(kEntries[entry], {{"RAB_MEDIA", "1"}});
            ok = !plain[i].empty() && (entry != Shade || LayoutMatches(plain[i]));
            if (ok && m_sharc)
            {
                query[i] = load(kEntries[entry], {{"RAB_SHARC", "1"}, {"SHARC_QUERY", "1"}, {"RAB_MEDIA", "1"}});
                ok = !query[i].empty() && (entry != Shade || LayoutMatches(query[i]));
            }
        }
        if (ok && m_sharc)
        {
            update = load(kSharcEntries[SharcUpdate], {{"RAB_SHARC", "1"}, {"SHARC_UPDATE", "1"}, {"RAB_MEDIA", "1"}});
            ok = !update.empty();
        }
        ok = ok && ComputePipelineBatch::Create(gfx, m_layout, plain.data(), kMediaEntryCount, m_mediaPso.data());
        if (ok && m_sharc)
            ok = ComputePipelineBatch::Create(gfx, m_layout, query.data(), kMediaEntryCount, m_mediaPsoQuery.data()) &&
                 ComputePipelineBatch::Create(gfx, m_layout, &update, 1, &m_mediaSharcUpdate);
        if (!ok)
        {
            ReleaseMediaPipelines(gfx);
            LOG_ERROR("PathTracePass: RTXDI PT media pipelines failed - the paths cross no fog");
            return false;
        }
        m_mediaFailed = false;
        LOG_INFO("[PT_RTXDI_MEDIA] pipelines=%u sharc=%u ms=%.1f", kMediaEntryCount + (m_sharc ? kMediaEntryCount + 1 : 0),
                 m_sharc ? 1u : 0u, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        return true;
    }
    void ReleaseMediaPipelines(IGraphicsDevice& gfx)
    {
        for (auto& p : m_mediaPso) gfx.DestroyPipelineState(p);
        for (auto& p : m_mediaPsoQuery) gfx.DestroyPipelineState(p);
        gfx.DestroyPipelineState(m_mediaSharcUpdate);
        m_mediaLogKey = ~0u;
    }

    // The SHaRC cache's three kernels and every core kernel again with SHARC_QUERY. A device or
    // tree that cannot run it leaves the core without the cache, saying why ([SHARC] enabled=0
    // reason=...), never silently inert.
    void BuildSharcPipelines(IGraphicsDevice& gfx)
    {
        m_sharc = false;
        m_sharcWhy = "off (-rtxdi_pt_sharc=1 turns it on)";
        if (!m_sharcWanted) return;
        // 64-bit heap atomics, then the unpacked headers (a package carries
        // none - their license: object code only - and loads the compiled
        // kernels from the shader cache it ships). The core's kernels are
        // 16-bit already, so native 16-bit ops are not asked again.
        if (const char* why = SharcCache::InertReason(gfx, false)) { m_sharcWhy = why; return; }
        auto load = [&](const char* entry, std::vector<DxcCompiler::Define> defines) {
            return ShaderDiskCache::LoadOrCompile("shaders/RtxdiPT.cs.hlsl", entry, RHI::ShaderStage::CS, defines,
                                                  "PTRtxdi", ShaderDiskCache::CompileFlags{true, true});
        };
        // Every kernel first (Shade's constants checked against its reflection), then each set's
        // pipelines together (ComputePipelineBatch).
        std::array<std::vector<uint8_t>, EntryCount> query;
        std::array<std::vector<uint8_t>, SharcEntryCount> cache;
        bool ok = true;
        for (uint32_t i = 0; i < EntryCount && ok; ++i)
        {
            query[i] = load(kEntries[i], {{"RAB_SHARC", "1"}, {"SHARC_QUERY", "1"}});
            ok = !query[i].empty() && (i != Shade || LayoutMatches(query[i]));
        }
        if (ok) { cache[SharcClear] = load(kSharcEntries[SharcClear], {{"RAB_SHARC", "1"}}); ok = !cache[SharcClear].empty(); }
        if (ok) { cache[SharcUpdate] = load(kSharcEntries[SharcUpdate], {{"RAB_SHARC", "1"}, {"SHARC_UPDATE", "1"}}); ok = !cache[SharcUpdate].empty(); }
        if (ok) { cache[SharcResolve] = load(kSharcEntries[SharcResolve], {{"RAB_SHARC", "1"}}); ok = !cache[SharcResolve].empty(); }
        ok = ok && ComputePipelineBatch::Create(gfx, m_layout, query.data(), EntryCount, m_psoQuery.data())
                && ComputePipelineBatch::Create(gfx, m_layout, cache.data(), SharcEntryCount, m_sharcPso.data());
        if (!ok)
        {
            for (auto& p : m_psoQuery) gfx.DestroyPipelineState(p);
            for (auto& p : m_sharcPso) gfx.DestroyPipelineState(p);
            m_sharcWhy = "pipeline";
            LOG_ERROR("PathTracePass: RTXDI PT SHaRC pipelines failed - running without the radiance cache");
            return;
        }
        m_sharc = true;
        m_sharcWhy = "selected";
    }
    void ClearCache(const char* reason) override { m_cache.RequestClear(reason ? reason : "world"); }
    SharcCache::Lookup GetSharcLookup(const IGraphicsDevice& gfx) const override
    {
        SharcCache::Lookup lookup;
        if (!m_sharc || !m_cache.IsAllocated()) return lookup;
        lookup.indices = m_cache.GetIndices(gfx);
        lookup.indices.capacity = 1u << m_sharcCapacityLog2;   // as the kernels' sharcCapacity
        lookup.sceneScale = m_sharcSceneScale;
        lookup.radianceScale = m_sharcRadianceScale;
        return lookup;
    }

    bool Allocate(IGraphicsDevice& gfx, uint32_t width, uint32_t height) override
    {
        if (m_allocated && width == m_w && height == m_h) return true;
        gfx.WaitIdleAndReleaseDeferred();
        ReleaseSized(gfx);   // the SHaRC cache is world-space: a resize keeps it
        m_w = width; m_h = height;
        rtxdi::ReSTIRPTStaticParameters sp;
        sp.RenderWidth = width; sp.RenderHeight = height;
        m_ctx = std::make_unique<rtxdi::ReSTIRPTContext>(sp);
        m_ctx->SetResamplingMode(m_mode);
        const RTXDI_ReservoirBufferParameters res = m_ctx->GetReservoirBufferParameters();
        // Slots 0 and 1 (RTXDI's ping-pong); slot 2 holds the unresampled initial sample for the RR
        // recipe's decorrelation.
        m_slots = m_rrRecipe != 0 ? 3 : 2;
        m_stagnancyValid = false;
        RHI::GPUBufferDesc bd{};
        bd.usage = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        bd.stride = sizeof(RTXDI_PackedPTReservoir);
        bd.size = uint64_t(res.reservoirArrayPitch) * m_slots * bd.stride;
        bool ok = gfx.CreateBuffer(bd, m_reservoirs);
        bd.stride = 16; bd.size = uint64_t(width) * height * 2 * bd.stride;   // the bridge's 32 B surfaces
        for (auto& s : m_surfaces) ok = ok && gfx.CreateBuffer(bd, s);
        bd.stride = 4; bd.size = kCounterCount * 4;
        ok = ok && gfx.CreateBuffer(bd, m_counters);
        const uint32_t offsets = 8192;   // RTXDI's neighbour offset table (ImportanceSamplingContext's default)
        m_offsetCount = offsets;
        std::vector<uint8_t> offsetData(size_t(offsets) * 2);
        rtxdi::FillNeighborOffsetBuffer(offsetData.data(), offsets);
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_TYPED;
        bd.format = RHI::Format::R8G8_SNORM; bd.stride = 2; bd.size = offsetData.size();
        ok = ok && gfx.CreateBuffer(bd, m_neighborOffsets, offsetData.data());
        RHI::GPUBufferDesc rb{};
        rb.size = kCounterCount * 4; rb.stride = 4; rb.usage = RHI::Usage::READBACK; rb.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < RHI::kFramesInFlight && ok; ++i)
            ok = gfx.CreateBuffer(rb, m_readback[i]) && (m_mapped[i] = static_cast<const uint32_t*>(gfx.MapBuffer(m_readback[i])));
        if (m_dupMap || m_rrRecipe != 0)
        {
            RHI::TextureDesc td{};
            td.width = width; td.height = height; td.depth = 1; td.mipLevels = 1;
            td.usage = RHI::Usage::DEFAULT;
            td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            td.layout = RHI::ResourceState::UNORDERED_ACCESS;
            td.format = RHI::Format::R32_UINT;
            ok = ok && gfx.CreateTexture(td, m_sampleIds);
            td.format = RHI::Format::R16G16_FLOAT;
            ok = ok && gfx.CreateTexture(td, m_dupMapTex);
            // The smoothed stagnancy, this frame's and the last (decorrelation=stagnancy only).
            td.format = RHI::Format::R16_FLOAT;
            if (m_rrRecipe != 0 && !m_decoUniform)
                for (auto& t : m_stagnancy) ok = ok && gfx.CreateTexture(td, t);
        }
        if (m_pairwise)
        {
            const uint64_t entries = uint64_t(width) * height * m_spatialSamples;
            RHI::GPUBufferDesc pd{};
            pd.usage = RHI::Usage::DEFAULT;
            pd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
            pd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            pd.stride = 16; pd.size = entries * 2 * 16;
            ok = ok && gfx.CreateBuffer(pd, m_pairScratch);
            pd.stride = 4; pd.size = entries * 4;
            ok = ok && gfx.CreateBuffer(pd, m_pairList);
            pd.stride = 4; pd.size = 8;
            ok = ok && gfx.CreateBuffer(pd, m_pairCounter);
            pd.stride = 4; pd.size = 16;
            ok = ok && gfx.CreateBuffer(pd, m_pairArgs);
            if (m_rgb)
            {
                RHI::TextureDesc td{};
                td.width = width; td.height = height; td.depth = 1; td.mipLevels = 1;
                td.usage = RHI::Usage::DEFAULT;
                td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
                td.layout = RHI::ResourceState::UNORDERED_ACCESS;
                td.format = RHI::Format::R32G32B32A32_FLOAT;
                ok = ok && gfx.CreateTexture(td, m_rgbTex);
            }
        }
        // Committed, never aliased: the cache is cross-frame data. A new one
        // starts with a clear pending (SharcCache::Allocate).
        if (m_sharc && !m_cache.IsAllocated())
            ok = ok && m_cache.Allocate(gfx, m_sharcCapacityLog2);
        ok = ok && m_cb.Create(gfx, "PT.Rtxdi");
        if (!ok) { LOG_ERROR("PathTracePass: RTXDI PT allocation failed"); Release(gfx); return false; }
        m_reservoirState = m_offsetsState = m_countersState = RHI::ResourceState::UNDEFINED;
        m_pairScratchState = m_pairListState = m_pairCounterState = m_pairArgsState = RHI::ResourceState::UNDEFINED;
        m_surfaceState.fill(RHI::ResourceState::UNDEFINED);
        m_primed.fill(false);
        m_surfaceSlot = 0; m_history = false; m_hasPrev = false;
        m_allocated = true;
        LOG_INFO("[RTXDI] PT core allocated render=%ux%u reservoirs=%u slots=%u bytes=%llu %s", width, height,
                 res.reservoirArrayPitch, m_slots, static_cast<unsigned long long>(ResidentBytes()), m_describe);
        return true;
    }
    void Release(IGraphicsDevice& gfx) override
    {
        ReleaseSized(gfx);
        m_cache.Release(gfx);
    }
    // Everything sized by the render resolution (the cache is not).
    void ReleaseSized(IGraphicsDevice& gfx)
    {
        for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
        {
            if (m_mapped[i]) gfx.UnmapBuffer(m_readback[i]);
            if (m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
            m_readback[i] = {}; m_mapped[i] = nullptr;
        }
        for (auto* b : { &m_reservoirs, &m_neighborOffsets, &m_counters, &m_surfaces[0], &m_surfaces[1],
                         &m_pairScratch, &m_pairList, &m_pairCounter, &m_pairArgs })
        { if (b->IsValid()) gfx.DestroyBuffer(*b); *b = {}; }
        for (auto* t : { &m_sampleIds, &m_dupMapTex, &m_rgbTex, &m_stagnancy[0], &m_stagnancy[1] })
        { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
        if (m_cb.IsValid()) m_cb.Destroy(gfx);
        m_ctx.reset();
        m_allocated = false;
    }
    bool IsAllocated() const override { return m_allocated; }
    void ResetHistory() override { m_history = false; }

    void Record(const Frame& f) override
    {
        if (!m_allocated || !HasPipelines()) return;
        auto& gfx = *f.gfx;
        auto cl = f.cl;
        auto index = [&](uint64_t h) { return gfx.DescriptorHeapIndexFromGpuHandle(h); };
        const uint32_t cur = m_surfaceSlot, prev = cur ^ 1u;
        const bool history = m_history && !f.reset;
        m_ctx->SetFrameIndex(m_frame);

        Constants k{};
        RTXDI::BridgeConstants& b = k.bridge;
        // Transposed: the HLSL cbuffer packs column-major and the bridge
        // multiplies row vectors (RTDIPass's FillConstants does the same).
        DirectX::XMStoreFloat4x4(&b.invViewProj, DirectX::XMMatrixTranspose(
            DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&f.viewProj))));
        DirectX::XMStoreFloat4x4(&b.worldToView, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&f.view)));
        b.prevWorldToView = m_hasPrev ? m_prevWorldToView : b.worldToView;
        b.camera = f.camera;
        b.prevCamera = m_hasPrev ? m_prevCamera : f.camera;
        b.width = f.width; b.height = f.height;
        b.depthIdx = f.depthIdx; b.normalIdx = f.normalIdx; b.albedoIdx = f.albedoIdx; b.surfaceIdx = f.surfaceIdx;
        b.velocityIdx = f.velocityIdx; b.materialsIdx = f.materialsIdx;
        b.lightsIdx = f.lightsIdx; b.lightCount = f.lightCount;
        b.prevLightsIdx = f.prevLightsIdx ? f.prevLightsIdx : f.lightsIdx;
        b.prevLightCount = f.prevLightsIdx ? f.prevLightCount : f.lightCount;
        b.lightMapIdx = f.lightMapIdx;
        b.surfaceOutUav = index(gfx.GetBufferUAVGpuHandle(m_surfaces[cur]));
        b.prevSurfaceSrv = history ? index(gfx.GetBufferSRVGpuHandle(m_surfaces[prev])) : 0u;
        b.surfaceGridWidth = f.width; b.surfaceGridHeight = f.height;
        b.rayTMax = 10000.f; b.alphaTestEnabled = f.alphaTestEnabled; b.alphaInstanceCount = f.alphaInstanceCount;

        k.runtime.neighborOffsetMask = m_offsetCount - 1;
        k.runtime.activeCheckerboardField = 0;
        k.runtime.frameIndex = m_frame;
        RTXDI_PTParameters& p = k.restirPT;
        p.reservoirBuffer = m_ctx->GetReservoirBufferParameters();
        p.bufferIndices = m_ctx->GetBufferIndices();
        p.initialSampling = m_ctx->GetInitialSamplingParameters();
        p.initialSampling.numInitialSamples = m_initialSamples;
        p.initialSampling.maxBounceDepth = m_bounces;
        p.initialSampling.maxRcVertexLength = m_rcLength;
        p.decorrelation = m_ctx->GetDecorrelationParameters();
        p.reconnection = m_ctx->GetReconnectionParameters();
        p.reconnection.reconnectionMode = m_footprint ? RTXDI_PTReconnectionMode::Footprint : RTXDI_PTReconnectionMode::FixedThreshold;
        // The RR recipe this frame: its third slot exists, and RR takes the guides unless
        // rrRecipe=1 pins it on.
        const bool recipe = m_rrRecipe != 0 && m_slots == 3 && (m_rrRecipe > 0 || f.rrGuideUav[0] != 0);
        const bool stagnancy = recipe && !m_decoUniform;
        const bool dupMap = (recipe && !m_dupMapPinned) || m_dupMap;   // the count and its history cut
        const bool dupInputs = dupMap || recipe;                         // + the recipe's stagnancy
        const uint32_t maxHistory = (recipe && !m_historyPinned) ? 20u : m_maxHistory;
        const bool ageRejection = (recipe && !m_ageRejectionPinned) ? false : m_ageRejection;
        if (recipe)
        {
            // RTXDI's DLSS-RR decorrelation (GetDefaultReSTIRPTDecorrelationParams(true)).
            p.decorrelation.decorrelationMode = m_decoUniform ? RTXDI_PTDecorrelationMode::Uniform
                                                              : RTXDI_PTDecorrelationMode::Stagnancy;
            p.decorrelation.decorrelationFactor = m_decoFactor;
            p.decorrelation.decorrelationStagnancyExponent = 0.5f;
            p.decorrelation.decorrelationEmaFactor = 0.2f;
            p.decorrelation.fireflyReplacementFilterEnable = m_firefly ? 1u : 0u;
            p.decorrelation.fireflyReplacementFilterStrength = m_fireflyStrength;
            p.decorrelation.fireflyReplacementBiasReduction = m_fresh == Fresh::Clamp ? 1u : 0u;
            p.decorrelation.fireflyReplacementMultiplyBound = 15.f;
        }
        else
        {
            p.decorrelation.decorrelationMode = RTXDI_PTDecorrelationMode::None;
            p.decorrelation.decorrelationFactor = 0.f;
            p.decorrelation.fireflyReplacementFilterEnable = 0;
        }
        p.temporalResampling = m_ctx->GetTemporalResamplingParameters();
        p.temporalResampling.maxHistoryLength = maxHistory;
        p.temporalResampling.enableVisibilityBeforeCombine = m_temporalVisibility ? 1u : 0u;
        p.temporalResampling.enablePermutationSampling =
            (m_permutation < 0 ? f.rrGuideUav[0] != 0 : m_permutation != 0) ? 1u : 0u;
        p.temporalResampling.enableAgeBasedRejection = ageRejection ? 1u : 0u;
        p.temporalResampling.duplicationBasedHistoryReduction = dupMap ? 1u : 0u;
        p.hybridShift = m_ctx->GetHybridShiftParameters();
        p.hybridShift.maxBounceDepth = m_bounces;
        p.hybridShift.maxRcVertexLength = m_rcLength;
        p.boilingFilter = m_ctx->GetBoilingFilterParameters();
        p.boilingFilter.boilingFilterStrength = m_boilingStrength;
        p.spatialResampling = m_ctx->GetSpatialResamplingParameters();
        p.spatialResampling.numSpatialSamples = m_spatialSamples;
        p.spatialResampling.numDisocclusionBoostSamples = m_boost;
        p.spatialResampling.samplingRadius = m_radius;
        p.spatialResampling.maxTemporalHistory = maxHistory;
        p.spatialResampling.duplicationBasedHistoryReduction = dupMap ? 1u : 0u;

        EngineConstants& e = k.pt;
        e.sunDirection = f.sunDirection; e.sunCosRadius = f.sunCosRadius;
        e.sunColor = f.sunColor; e.skyIdx = f.skyIdx;
        e.environmentScale = f.environmentScale; e.constantEnvironment = f.constantEnvironment;
        const bool counters = (f.engineFlags & kCounters) != 0;
        const bool useDI = m_useDI && f.diResultIdx != 0;
        const bool temporal = m_mode == rtxdi::ReSTIRPT_ResamplingMode::Temporal || m_mode == rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        const bool spatial = m_mode == rtxdi::ReSTIRPT_ResamplingMode::Spatial || m_mode == rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        const bool direct = (f.engineFlags & kDirectOnly) != 0;
        const bool pairwise = m_pairwise && spatial && !direct;
        const bool sharc = m_sharc && m_cache.IsAllocated();
        e.ptFlags = (f.engineFlags & (kDiffuse | kConstantEnvironment | kNoSunNee | kCounters | kDirectOnly))
                  | (f.accumulate ? kAccumulate : 0u) | (useDI ? kDiResult : 0u) | (m_replayNoRc ? kReplayNoRc : 0u)
                  | (m_unified ? kUnified : 0u) | (dupMap ? kDupMap : 0u) | (m_dualMv ? kDualMV : 0u)
                  | (recipe ? kDecorrelation : 0u)
                  | (pairwise && m_rgb ? kRgb : 0u) | (pairwise && m_compaction ? kCompaction : 0u)
                  | (f.rrGuideUav[0] ? kRRGuides | (m_rrCorrupt << 17) : 0u) | (f.glass ? kGlass : 0u);
        e.neeCandidates = m_neeCandidates; e.neeCandidatesX1 = m_neeCandidatesX1;
        e.rrAlbedoUav = f.rrGuideUav[0]; e.rrSpecularUav = f.rrGuideUav[1];
        e.rrNormalUav = f.rrGuideUav[2]; e.rrDistanceUav = f.rrGuideUav[3];
        if (sharc)
        {
            const SharcCache::Indices cache = m_cache.GetIndices(gfx);
            e.sharcHashUav = cache.hashUav;
            e.sharcAccumulationUav = cache.accumulationUav;
            e.sharcResolvedUav = cache.resolvedUav;
            e.sharcCapacity = 1u << m_sharcCapacityLog2;
            e.sharcSceneScale = m_sharcSceneScale; e.sharcRadianceScale = m_sharcRadianceScale;
            e.sharcUpdateTile = m_sharcTile; e.sharcUpdateBounces = m_sharcBounces;
            e.sharcPrevCamera = m_hasPrev ? m_prevCamera : f.camera;
            e.sharcAccumulationFrames = m_sharcAccumulationFrames; e.sharcStaleFrames = m_sharcStaleFrames;
            e.sharcFrame = m_frame + (f.seed - 1u) * 0x9E3779B9u;
            e.sharcFlags = (m_sharcDiffuseOnly ? 1u : 0u) | (m_sharcFootprintOnly ? 2u : 0u) | (m_sharcGateLast ? 4u : 0u);
            e.sharcMinRoughness = m_sharcMinRoughness;
        }
        if (pairwise)
        {
            e.pairScratchUav = index(gfx.GetBufferUAVGpuHandle(m_pairScratch));
            e.pairListUav = index(gfx.GetBufferUAVGpuHandle(m_pairList));
            e.pairCounterUav = index(gfx.GetBufferUAVGpuHandle(m_pairCounter));
            e.pairArgsUav = index(gfx.GetBufferUAVGpuHandle(m_pairArgs));
            e.rgbUav = m_rgb ? index(gfx.GetTextureUAVGpuHandle(m_rgbTex)) : 0u;
            e.pairCount = m_spatialSamples;
            // Fresh reciprocal pairings every frame: partner = ((p + t) ^ m) - t,
            // |m| < 2^bits with 2^bits ~ the spatial radius.
            const uint32_t bits = uint32_t(std::clamp(std::lround(std::log2(double(m_radius))), 1l, 7l));
            for (uint32_t k = 0; k < m_spatialSamples; ++k)
            {
                uint32_t h = (m_frame * 4u + k) ^ ((f.seed - 1u) * 0x9E3779B9u);
                h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
                const uint32_t mask = (1u << bits) - 1u;
                uint32_t mx = (h >> 12) & mask, my = (h >> 20) & mask;
                if (mx == 0 && my == 0) mx = 1u + (h >> 28) % mask;
                e.pairing[k][0] = h & 63u; e.pairing[k][1] = (h >> 6) & 63u; e.pairing[k][2] = mx; e.pairing[k][3] = my;
            }
        }
        e.pathMaterialsIdx = f.pathMaterialsIdx;
        e.lightAliasIdx = f.lightAliasIdx; e.activeLocalLightCount = f.activeLocalLightCount;
        e.resultUav = f.resultUav; e.accumUav = f.accumUav;
        e.diResultIdx = useDI ? f.diResultIdx : 0u;
        e.reservoirUav = index(gfx.GetBufferUAVGpuHandle(m_reservoirs));
        e.neighborOffsetsSrv = index(gfx.GetBufferSRVGpuHandle(m_neighborOffsets));
        e.counterUav = index(gfx.GetBufferUAVGpuHandle(m_counters));
        // The kernels' random streams hash (pixel, frameIndex): -seed offsets
        // the frame index so two runs are independent; -seed=1 keeps it.
        e.spp = f.spp; e.frameIndex = m_frame + (f.seed - 1u) * 0x9E3779B9u;
        // The fog, when the media builds exist (BuildMediaPipelines).
        const bool media = f.mediumIdx != 0 && m_mediaPso.back().IsValid();
        e.mediumIdx = media ? f.mediumIdx : 0u;
        e.mediaFlags = f.mediaFlags;
        if (f.mediaFlags == 3u && f.froxel.srv)
        {
            e.froxelViewProj = f.froxel.viewProj;
            e.froxelCamera = f.froxel.camera;
            e.froxelHistoryIdx = f.froxel.srv;
            e.froxelNear = f.froxel.froxelNear;
            e.froxelFar = f.froxel.froxelFar;
        }
        {
            const uint32_t rule = std::min(f.mediaFlags, 3u);
            const uint32_t key = (media ? 1u : 0u) | (rule << 1) | (e.froxelHistoryIdx ? 16u : 0u);
            if (key != m_mediaLogKey)
            {
                m_mediaLogKey = key;
                // froxel: the fog's lit volume reached the segments (the froxel rule only).
                LOG_INFO("[PT_RTXDI_MEDIA] fog=%u continuation=%s froxel=%u", media ? 1u : 0u, kMediaRules[rule],
                         e.froxelHistoryIdx ? 1u : 0u);
            }
        }
        e.prevPrevCamera = m_hasPrev ? m_prevPrevCamera : f.camera;
        if (dupInputs)
        {
            e.sampleIdUav = index(gfx.GetTextureUAVGpuHandle(m_sampleIds));
            e.dupMapUav = index(gfx.GetTextureUAVGpuHandle(m_dupMapTex));
        }
        if (stagnancy)
        {
            e.stagnancyUav = index(gfx.GetTextureUAVGpuHandle(m_stagnancy[m_stagnancySlot]));
            e.prevStagnancyUav = history && m_stagnancyValid
                ? index(gfx.GetTextureUAVGpuHandle(m_stagnancy[m_stagnancySlot ^ 1u])) : 0u;
        }
        DirectX::XMFLOAT4X4 viewProjT;
        DirectX::XMStoreFloat4x4(&viewProjT, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&f.viewProj)));
        e.prevViewProj = m_hasPrev ? m_prevViewProj : viewProjT;
        // Built here, copied once: the constant buffer is write-combined.
        *m_cb.Current(gfx) = k;

        auto bufState = [&](RHI::GPUBuffer& buf, RHI::ResourceState& before, RHI::ResourceState after) {
            if (before != after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&buf, before, after), cl); before = after;
        };
        bufState(m_reservoirs, m_reservoirState, RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_neighborOffsets, m_offsetsState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        bufState(m_counters, m_countersState, RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_surfaces[cur], m_surfaceState[cur], RHI::ResourceState::UNORDERED_ACCESS);
        bufState(m_surfaces[prev], m_surfaceState[prev], RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

        namespace S = ReSTIR::ShellSlots;
        // The shell layout, through the first entry's pipeline: every kernel's
        // pipeline (and its SHaRC builds) is built on it, so the arguments hold.
        gfx.BindComputePipelineState(m_pso[Clear], cl);
        gfx.SetComputeRootCBV(S::CB, m_cb.CurrentBuffer(gfx), 0, cl);
        gfx.SetComputeRootCBV(S::CoreCB, m_cb.CurrentBuffer(gfx), 0, cl);
        gfx.SetComputeRootSRVAddress(S::TLAS, f.tlasVA, cl);
        gfx.SetComputeRootSRVAddress(S::Instances, f.instanceVA, cl);
        gfx.SetComputeRootSRVAddress(S::Alpha, f.alphaVA, cl);
        gfx.SetComputeDescriptorTable(S::Buffers, f.buffers, cl);
        gfx.SetComputeDescriptorTable(S::Textures, gfx.GetBindlessTextureTableGpuHandle(), cl);
        gfx.SetComputeDescriptorTable(S::Result, f.resultTable, cl);
        auto uavBarrier = [&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
        // With the cache, every kernel runs its SHARC_QUERY build: initial
        // sampling and the shifts' replays end paths by the same rule. With the
        // fog, the kernels that weigh a segment run their media builds.
        auto pso = [&](Entry entry) -> const RHI::PipelineState& {
            const int slot = media ? MediaSlot(entry) : -1;
            if (slot >= 0) return sharc ? m_mediaPsoQuery[slot] : m_mediaPso[slot];
            return sharc ? m_psoQuery[entry] : m_pso[entry];
        };
        auto run = [&](Entry entry, uint32_t gx, uint32_t gy) {
            const auto t = gfx.BeginGPUTimestamp(cl, kRegions[entry]);
            gfx.BindComputePipelineState(pso(entry), cl);
            gfx.DispatchCompute(gx, gy, 1, cl);
            gfx.EndGPUTimestamp(cl, t);
            uavBarrier();
        };
        const uint32_t gx = (f.width + 7) / 8, gy = (f.height + 7) / 8;
        if (counters)
        {
            gfx.BindComputePipelineState(pso(Clear), cl);
            gfx.DispatchCompute((kCounterCount + 7) / 8, 1, 1, cl);
            uavBarrier();
        }
        if (sharc)
        {
            // SHaRC: clear (first frame, a new world) -> this frame's sparse update -> resolve into the
            // history; the kernels below read the resolved cache (SHaRC Integration.md's order).
            m_cache.Transition(gfx, cl);
            auto runSharc = [&](SharcEntry entry, uint32_t sx, uint32_t sy) {
                const auto t = gfx.BeginGPUTimestamp(cl, kSharcRegions[entry]);
                gfx.BindComputePipelineState(media && entry == SharcUpdate ? m_mediaSharcUpdate : m_sharcPso[entry], cl);
                gfx.DispatchCompute(sx, sy, 1, cl);
                gfx.EndGPUTimestamp(cl, t);
                uavBarrier();
            };
            const uint32_t capacity = 1u << m_sharcCapacityLog2;
            if (const char* why = m_cache.TakeClear())
            {
                runSharc(SharcClear, (capacity + 255) / 256, 1);   // <= 2^23 entries: <= 32768 groups
                LOG_INFO("[SHARC] cleared=%u capacity=%u reason=%s", m_cache.Clears(), capacity, why);
            }
            const uint32_t tiles[2] = { (f.width + m_sharcTile - 1) / m_sharcTile, (f.height + m_sharcTile - 1) / m_sharcTile };
            runSharc(SharcUpdate, (tiles[0] + 7) / 8, (tiles[1] + 7) / 8);
            runSharc(SharcResolve, (capacity + 255) / 256, 1);
        }
        if (!m_sharcLogged)
        {
            m_sharcLogged = true;
            LOG_INFO("[SHARC] enabled=%u reason=%s capacity=%u bytes=%llu sceneScale=%.1f updateTile=%u updateBounces=%u accumulationFrames=%u staleFrames=%u radianceScale=%.0f",
                     sharc ? 1u : 0u, m_sharcWhy, sharc ? 1u << m_sharcCapacityLog2 : 0u,
                     static_cast<unsigned long long>(sharc ? (40ull << m_sharcCapacityLog2) : 0ull), m_sharcSceneScale,
                     m_sharcTile, m_sharcBounces, m_sharcAccumulationFrames, m_sharcStaleFrames, m_sharcRadianceScale);
        }
        run(Initial, gx, gy);
        if (!direct)
        {
            if (temporal && history) run(Temporal, gx, gy);
            if (temporal && history && (m_boiling < 0 ? f.rrGuideUav[0] != 0 : m_boiling != 0) &&
                p.boilingFilter.enableBoilingFilter)
                run(Boiling, (f.width + 15) / 16, (f.height + 15) / 16);
            if (pairwise)
            {
                bufState(m_pairScratch, m_pairScratchState, RHI::ResourceState::UNORDERED_ACCESS);
                bufState(m_pairList, m_pairListState, RHI::ResourceState::UNORDERED_ACCESS);
                bufState(m_pairCounter, m_pairCounterState, RHI::ResourceState::UNORDERED_ACCESS);
                run(PairShift, gx, gy);
                if (m_compaction)
                {
                    bufState(m_pairArgs, m_pairArgsState, RHI::ResourceState::UNORDERED_ACCESS);
                    run(PairArgs, 1, 1);
                    bufState(m_pairArgs, m_pairArgsState, RHI::ResourceState::INDIRECT_ARGUMENT);
                    const auto t = gfx.BeginGPUTimestamp(cl, kRegions[PairReplay]);
                    gfx.BindComputePipelineState(pso(PairReplay), cl);
                    gfx.DispatchComputeIndirect(m_pairArgs, 0, cl);
                    gfx.EndGPUTimestamp(cl, t);
                    uavBarrier();
                }
                run(PairResample, gx, gy);
            }
            else if (spatial) run(Spatial, gx, gy);
        }
        // The final reservoir's duplication inputs and count (the next frame's history cap), then
        // the RR recipe's stagnancy smoothing and firefly flag - both read by Shade's decorrelation.
        if (dupInputs) run(DupInputs, gx, gy);
        if (dupMap) run(DupMap, (f.width + 15) / 16, (f.height + 15) / 16);
        if (stagnancy) run(DupSmooth, gx, gy);
        if (recipe && m_firefly && temporal && history && !direct) run(Firefly, (f.width + 15) / 16, (f.height + 15) / 16);
        run(Shade, gx, gy);
        if (stagnancy) m_stagnancySlot ^= 1u;
        m_stagnancyValid = stagnancy;
        if (counters)
        {
            const uint32_t slot = gfx.GetFrameIndex();
            bufState(m_counters, m_countersState, RHI::ResourceState::COPY_SRC);
            gfx.CopyBuffer(m_counters, m_readback[slot], kCounterCount * 4, cl);
            m_primed[slot] = true;
        }
        m_lastTemporal = temporal && history && !direct;
        m_surfaceSlot ^= 1u;
        m_history = true;
        m_prevPrevCamera = m_hasPrev ? m_prevCamera : f.camera;
        m_prevCamera = f.camera;
        m_prevWorldToView = b.worldToView;
        m_prevViewProj = viewProjT;
        m_hasPrev = true;
        ++m_frame;
    }

    void LogCounters(uint32_t slot) override
    {
        if (slot >= RHI::kFramesInFlight || !m_primed[slot] || !m_mapped[slot]) return;
        const uint32_t* c = m_mapped[slot];
        LOG_INFO("[PT_RTXDI] primaries=%u initial=%u temporal=%u spatial=%u boiled=%u nonfinite=%u shaded=%u diLit=%u sharcEnded=%u sharcDeposits=%u sharcMissed=%u decorrelated=%u fireflies=%u",
                 c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12]);
        if (c[5]) LOG_ERROR("PathTracePass: RTXDI PT nonfinite pixels=%u", c[5]);
    }
    uint64_t ResidentBytes() const override
    {
        if (!m_allocated || !m_ctx) return 0;
        return uint64_t(m_ctx->GetReservoirBufferParameters().reservoirArrayPitch) * m_slots * sizeof(RTXDI_PackedPTReservoir)
             + 2ull * m_w * m_h * 32 + uint64_t(m_offsetCount) * 2 + kCounterCount * 4
             + (m_sampleIds.IsValid() ? uint64_t(m_w) * m_h * 8 : 0ull)
             + (m_stagnancy[0].IsValid() ? uint64_t(m_w) * m_h * 4 : 0ull)
             + (m_pairwise ? uint64_t(m_w) * m_h * m_spatialSamples * 36 + 24 : 0ull)
             + (m_rgb ? uint64_t(m_w) * m_h * 16 : 0ull)
             + m_cache.ResidentBytes();
    }
    double PipelineMs() const override { return m_pipelineMs; }
    const char* Describe() const override { return m_describe; }

private:
    enum class Fresh : uint8_t { Clamp, Replace };   // a fresh reservoir's replacement: RTXDI's 15x clamp, or as is

    RHI::PipelineLayout m_layout;   // the shell layout (ReSTIR::CreateShellLayout), the core's own
    std::array<RHI::PipelineState, EntryCount> m_pso;
    // SHaRC: every kernel's SHARC_QUERY build, the cache's own kernels and its buffers (the
    // shared SharcCache: hash keys, accumulation, resolved; 40 B an entry).
    std::array<RHI::PipelineState, EntryCount> m_psoQuery;
    std::array<RHI::PipelineState, SharcEntryCount> m_sharcPso;
    // The fog: kMediaEntries' RAB_MEDIA builds, their SHARC_QUERY builds and the cache's update
    // (with the cache), made on the first frame with a medium.
    std::array<RHI::PipelineState, kMediaEntryCount> m_mediaPso, m_mediaPsoQuery;
    RHI::PipelineState m_mediaSharcUpdate;
    bool m_mediaFailed = false;
    uint32_t m_mediaLogKey = ~0u;   // the last [PT_RTXDI_MEDIA] fog line's state
    SharcCache m_cache;
    bool m_sharcWanted = false, m_sharc = false, m_sharcLogged = false;
    const char* m_sharcWhy = "off";
    uint32_t m_sharcCapacityLog2 = 22, m_sharcTile = 5, m_sharcBounces = 3, m_sharcAccumulationFrames = 20, m_sharcStaleFrames = 60;
    bool m_sharcDiffuseOnly = false, m_sharcFootprintOnly = false, m_sharcGateLast = false;
    uint32_t m_rrCorrupt = 0;   // -dlss_rr_corrupt_guide: 1-4 = albedo, specular, normal, distance
    float m_sharcMinRoughness = 0.5f;
    float m_sharcSceneScale = 50.f, m_sharcRadianceScale = 1e4f;
    bool m_pipelinesFailed = false;
    double m_pipelineMs = 0.0;
    std::unique_ptr<rtxdi::ReSTIRPTContext> m_ctx;
    RHI::GPUBuffer m_reservoirs, m_neighborOffsets, m_counters;
    RHI::Texture m_sampleIds, m_dupMapTex;   // the duplication map (dupMap=1) and the RR recipe
    RHI::Texture m_stagnancy[2];            // the smoothed stagnancy ping-pong
    uint32_t m_stagnancySlot = 0;
    bool m_stagnancyValid = false;          // m_stagnancy[slot ^ 1] holds last frame's
    // The pairwise spatial pass (spatial=pairwise only).
    RHI::GPUBuffer m_pairScratch, m_pairList, m_pairCounter, m_pairArgs;
    RHI::Texture m_rgbTex;
    RHI::ResourceState m_pairScratchState = RHI::ResourceState::UNDEFINED, m_pairListState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_pairCounterState = RHI::ResourceState::UNDEFINED, m_pairArgsState = RHI::ResourceState::UNDEFINED;
    std::array<RHI::GPUBuffer, 2> m_surfaces;
    RHI::GPUBuffer m_readback[RHI::kFramesInFlight];
    const uint32_t* m_mapped[RHI::kFramesInFlight]{};
    std::array<bool, RHI::kFramesInFlight> m_primed{};
    RHI::ResourceState m_reservoirState = RHI::ResourceState::UNDEFINED, m_offsetsState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_countersState = RHI::ResourceState::UNDEFINED;
    std::array<RHI::ResourceState, 2> m_surfaceState{};
    FrameCB<Constants> m_cb;
    uint32_t m_w = 0, m_h = 0, m_slots = 2, m_offsetCount = 8192;
    bool m_allocated = false;

    // ---- Settings ----
    rtxdi::ReSTIRPT_ResamplingMode m_mode = rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
    uint32_t m_bounces = 3, m_rcLength = 5, m_initialSamples = 1, m_spatialSamples = 1, m_maxHistory = 8;
    float m_radius = 32.f;
    bool m_footprint = true, m_temporalVisibility = true, m_useDI = true;
    int m_permutation = 0;    // 1 always, -1 while DLSS-RR takes the guides
    int m_boiling = 0;        // 1 always, -1 while DLSS-RR takes the guides
    int m_rrRecipe = -1;      // -1 while DLSS-RR takes the guides, 0 / 1 pinned

    bool m_decoUniform = true, m_firefly = false;    // the recipe's parts (not RTXDI's defaults)
    Fresh m_fresh = Fresh::Replace;
    bool m_historyPinned = false, m_dupMapPinned = false, m_ageRejectionPinned = false;
    float m_decoFactor = 0.85f, m_fireflyStrength = 0.7f;
    float m_boilingStrength = 0.2f;   // RTXDI's: discards above (10 / s - 9) x the group mean
    bool m_replayNoRc = false, m_ageRejection = true, m_unified = false, m_dupMap = false, m_dualMv = false;
    bool m_pairwise = false, m_compaction = false, m_rgb = false;
    uint32_t m_neeCandidates = 1, m_neeCandidatesX1 = 8;
    uint32_t m_boost = 1;
    char m_describe[512] = {};

    // ---- History ----
    uint32_t m_surfaceSlot = 0, m_frame = 1;
    bool m_history = false, m_hasPrev = false, m_lastTemporal = false;
    DirectX::XMFLOAT3 m_prevCamera{}, m_prevPrevCamera{};
    DirectX::XMFLOAT4X4 m_prevWorldToView{};   // transposed, as uploaded
    DirectX::XMFLOAT4X4 m_prevViewProj{};      // transposed, as uploaded
};
} // namespace

std::unique_ptr<Core> MakeCore() { return std::make_unique<RtxdiPTCore>(); }
} // namespace PTRtxdi

#else

namespace PTRtxdi
{
std::unique_ptr<Core> MakeCore() { return nullptr; }
} // namespace PTRtxdi

#endif
