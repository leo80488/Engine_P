#pragma once
// PTRtxdiCore - PathTracePass's rtxdi mode: RTXDI's ReSTIR PT (initial path sampling,
// hybrid-shift temporal and spatial reuse, the boiling filter) over the engine path tracer,
// written as RTXDI's RAB_PathTrace (shaders/RtxdiBridgePT.hlsli, RtxdiPT.cs.hlsl). The pass
// keeps its contract (PT.Trace, PathTraceResult, the accumulation, the [PT] line); only what
// fills the result changes. A build without the RTXDI SDK gets no core (MakeCore returns
// nullptr) and the pass reports the mode inert.
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MediumSet.h"
#include "Graphics/SharcCache.h"
#include <DirectXMath.h>
#include <cstdint>
#include <memory>

class IGraphicsDevice;

namespace PTRtxdi
{
// One frame's inputs, from PathTracePass::Execute.
struct Frame
{
    IGraphicsDevice* gfx = nullptr;
    RHI::CommandList cl;
    uint64_t tlasVA = 0, instanceVA = 0, alphaVA = 0, buffers = 0;   // root SRVs; the space1 table
    uint32_t width = 0, height = 0;
    DirectX::XMFLOAT4X4 viewProj{}, view{};                          // current, row-vector convention
    DirectX::XMFLOAT3 camera{};
    uint32_t depthIdx = 0, normalIdx = 0, albedoIdx = 0, surfaceIdx = 0, velocityIdx = 0;
    uint32_t materialsIdx = 0, pathMaterialsIdx = 0;
    uint32_t lightsIdx = 0, lightCount = 0, prevLightsIdx = 0, prevLightCount = 0, lightMapIdx = 0;
    uint32_t lightAliasIdx = 0, activeLocalLightCount = 0;
    DirectX::XMFLOAT3 sunDirection{}, sunColor{};
    float sunCosRadius = 1.f, environmentScale = 1.f, constantEnvironment = 0.f;
    uint32_t skyIdx = 0;
    uint32_t engineFlags = 0;      // PathTracePass flags: 1 Lambertian, 2 constant sky, 4 no sun NEE, 8 counters, 64 direct only
    uint32_t alphaTestEnabled = 0, alphaInstanceCount = 0;
    uint32_t resultUav = 0, accumUav = 0;                            // bindless UAV indices
    uint64_t resultTable = 0;                                        // the shell signature's u0 table (unused by the kernels)
    uint32_t diResultIdx = 0;                                        // RestirDIResult this frame (0 = none)
    uint32_t seed = 1;                                               // -seed: decorrelates runs (1 = the default streams)
    uint32_t spp = 0;
    uint32_t rrGuideUav[4] = {};   // the DLSS-RR guides' UAVs, written by Shade; 0 = RR off
    // The fog the segments cross (Graphics/MediumSet.h's set, 0 = none; it takes the media builds,
    // BuildMediaPipelines) and what a continuation keeps (RtxdiBridgePTConstants.hlsli's kRabMedia*);
    // under kRabMediaFroxel, the froxel fog's last lit volume.
    uint32_t mediumIdx = 0, mediaFlags = 0;
    Media::FroxelVolume froxel;
    bool accumulate = false;
    bool reset = false;                                              // no history this frame
    bool glass = false;                                              // the TLAS holds a Transmissive instance
};

class Core
{
public:
    virtual ~Core() = default;
    // Builds the pipelines on the core's own ReSTIR shell layout (ReSTIR::CreateShellLayout).
    virtual bool BuildPipelines(IGraphicsDevice& gfx) = 0;
    virtual bool HasPipelines() const = 0;
    // Builds the media builds (RAB_MEDIA) of the kernels that weigh a segment, once, after
    // BuildPipelines: a frame with a medium (Frame::mediumIdx) runs them. False when they fail;
    // the frames then cross no fog.
    virtual bool BuildMediaPipelines(IGraphicsDevice& gfx) = 0;
    // Releases the pipelines and the layout (device objects); call before the core is dropped.
    virtual void ReleasePipelines(IGraphicsDevice& gfx) = 0;
    virtual bool Allocate(IGraphicsDevice& gfx, uint32_t width, uint32_t height) = 0;
    virtual void Release(IGraphicsDevice& gfx) = 0;
    virtual bool IsAllocated() const = 0;
    virtual void ResetHistory() = 0;
    virtual void Record(const Frame& f) = 0;
    // The counters of the frame that left the GPU last ([PT_RTXDI]).
    virtual void LogCounters(uint32_t slot) = 0;
    virtual uint64_t ResidentBytes() const = 0;
    virtual double PipelineMs() const = 0;
    virtual const char* Describe() const = 0;   // the settings, for [RESTIR_BACKEND] and [PT_RTXDI_SETTINGS]
    // Forgets the SHaRC radiance cache: on a new world, or new static geometry in the TLAS (the
    // cache may hold light that leaked through a slab whose BLAS was still building). Never on a
    // resize, a camera cut or a history reset: the cache is world-space.
    virtual void ClearCache(const char* reason = "world") { (void)reason; }
    // The SHaRC radiance cache for a reader later in the frame (the froxel fog's indirect rays): a
    // frame the core records clears, updates and resolves it first. Empty without a cache.
    virtual SharcCache::Lookup GetSharcLookup(const IGraphicsDevice& gfx) const { (void)gfx; return {}; }
};

// nullptr when the build has no RTXDI SDK.
std::unique_ptr<Core> MakeCore();
// Where the primary's (x1's) direct light comes from (engine.json raytracing.ptDirectLight, the
// editor's Path Trace (Real-time) section, Lua Settings.setPTDirectLight): the core's own
// next-event sample (power RIS), ReSTIR DI's result (the DI pass then runs raw on the
// path-trace path; the path tracer's consumer denoises it), or x1's light inside the PT
// reservoir (Unified). Auto: ReSTIR DI on the real-time path, the core's own sample elsewhere.
// -rtxdi_pt_di / -rtxdi_pt_unified still win.
enum class DirectLight : uint8_t { Auto = 0, Nee = 1, RestirDI = 2, Unified = 3, Count };
const char* DirectLightName(DirectLight d);
bool ParseDirectLight(const char* token, DirectLight& out);
void SetDirectLight(DirectLight d);
DirectLight GetDirectLight();
// The setting as it resolves now (the CLI pins, then the setting, then Auto's
// rule); never Auto. A core reads it when it is made.
DirectLight EffectiveDirectLight();
// Whether the core takes the primary's local lights from ReSTIR DI
// (RestirDIResult) rather than its own next-event sample: the renderer runs
// the DI pass on the path-trace path only then.
bool UsesRestirDI();
// The real-time PT preset, i.e. the Path Trace (Real-time) render path (-pt_realtime picks it
// at boot; PathTracePass::SetRealtimePreset): the core's defaults become the SHaRC radiance
// cache on and a 5-vertex cap; x1's lights follow DirectLight. Each -rtxdi_pt_* / -sharc_*
// flag still wins. A core reads it when it is made.
void SetRealtimePreset(bool on);
bool RealtimePreset();
} // namespace PTRtxdi
