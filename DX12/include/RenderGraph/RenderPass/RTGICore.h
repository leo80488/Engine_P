#pragma once
// ReSTIR GI resampling core interface.
//
// RTGIPass is the SHELL: Trace (cosine + emitter RIS candidates and their hit
// shading, which writes the engine's 40-byte GIReservoir - the neutral sample
// every core consumes), NRD, Resolve, the coverage partition and every name a
// pack or the Lighting fold reads. A core owns the reuse in between -
// Temporal / Spatial / Shade, its history validation, its reservoirs - and
// leaves the shell's DiffIn holding the NRD-packed pre-albedo radiance.

#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/GraphicsStruct.h"
#include <cstddef>
#include <cstdint>
#include <vector>

class IGraphicsDevice;

namespace RTGI
{
// cbuffer Params of RTGI.cs.hlsl.
struct alignas(16) Constants
{
    float invViewProj[16], worldToView[16];
    float camera[3], rayTMax;
    float sunDirection[3]; uint32_t frame;
    float sunColor[3]; uint32_t flags;
    uint32_t width,height,traceW,traceH;
    uint32_t depthIdx,normalIdx,albedoIdx,surfaceIdx;
    uint32_t velocityIdx,materialsIdx,lightsIdx,lightCount;
    uint32_t alphaTestEnabled,alphaInstanceCount,counterUav,seed;
    uint32_t diffUav,normalUav,viewZUav,mvUav;
    uint32_t diffSrv,coverageUav,coverageSrv,volumeIdx;
    uint32_t irradianceIdx,ddgiDepthIdx,probeIdx,volumeCount;
    float constantRadiance; uint32_t worldGrid,tier,rrDistanceUav;   // rrDistanceUav: RR's diffuse hit distance (Resolve)
    uint32_t initialUav,initialSrv,previousSrv,temporalUav;
    uint32_t temporalSrv,previousSurfaceSrv,surfaceUav,surfaceSrv;
    uint32_t historyReset,maxM,maxAge,spatialRadius;
    float historyJitterDelta[2],reprojectionPadding[2];
    uint32_t emitterIdx,emitterTableIdx,emitterCount,emitterCandidates;
    uint32_t initialCandidates,hitExactLights,hitLightSamples,localEmitterLimit;
    uint32_t spatialSamples,boostSamples; float freshBlend; uint32_t padding3;
    // SHaRC radiance cache: declared in HLSL only under RTGI_SHARC, so kernels built
    // without it read only the first 416 bytes; zero while the cache does not run.
    uint32_t sharcHashUav,sharcAccumulationUav,sharcResolvedUav,sharcCapacity;
    float sharcSceneScale,sharcRadianceScale,sharcMaxVoxel; uint32_t sharcFlags;
    float sharcPrevCamera[3]; uint32_t sharcFrame;
    uint32_t sharcUpdateTile,sharcUpdateBounces,sharcAccumulationFrames,sharcStaleFrames;
    uint32_t skyIdx; float environmentScale,sharcDepositMax; uint32_t sharcPad0;
};
static_assert(sizeof(Constants)==496 && offsetof(Constants,sharcHashUav)==416,
    "cbuffer Params of RTGI.cs.hlsl: 416 bytes, then the RTGI_SHARC tail");
// The bytes the kernels without SHaRC declare (the -restir_cb_digest scope without the cache).
constexpr size_t kConstantsOffBytes=offsetof(Constants,sharcHashUav);
// sharcFlags (RTGI.cs.hlsl kGISharc*).
constexpr uint32_t kSharcQuery=1u, kSharcMulti=2u, kSharcForceMiss=4u, kSharcUpdate=8u, kSharcBound=16u;

struct FillContext
{
    uint32_t shellFlags = 0;     // the shell's flag word (CLI + reuse bit)
    uint32_t frame = 0;
    bool resetReservoir = false; // the shell's reservoir reset
    float jitterX = 0.f, jitterY = 0.f;
    uint32_t budgetStage = 0;
};

// GPU region names, static per shell instance (the profiler keeps the
// pointer): the compare mode's shadow pass (RTGI.Shadow) has its own, so its
// regions never fold into the composited pass's.
struct RegionNames
{
    const char *clear, *emitterArea, *emitterCdf, *trace, *temporal, *spatial, *shade;
    const char *prepare, *boiling, *final, *relax, *reblur, *resolve;
    const char *sharcClear, *sharcUpdate, *sharcResolve;   // SHaRC (the main pass only)
};
extern const RegionNames kMainRegions, kShadowRegions;

struct CoreFrame
{
    IGraphicsDevice* gfx = nullptr;
    RHI::CommandList cl{};
    uint32_t traceW = 0, traceH = 0;
    float jitterX = 0.f, jitterY = 0.f;
    const RegionNames* regions = &kMainRegions;
    // SHaRC runs this frame: the core's history validation re-shades through the
    // cache - Record picks its BuildSharcPipelines entries.
    bool sharc = false;
};

struct CoreSettings
{
    bool reuse = true;
    uint32_t spatialRadius = 8;   // engine.json, half-resolution trace pixels
    uint32_t maxAge = 30;
    bool fullResolution = false;  // the shell traces at full render resolution
    // A command-line radius (-gi_spatial_radius) is taken as given at full
    // resolution, not doubled.
    bool spatialRadiusPinned = false;
};

class Core
{
public:
    virtual ~Core() = default;
    virtual ReSTIR::Backend Backend() const = 0;
    // First use, on a thread of its own beside the shell's build; on the
    // shell's layout (ReSTIR::CreateShellLayout), so the shell's arguments
    // hold across the core's pipelines.
    virtual bool BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell) = 0;
    virtual bool HasPipelines() const = 0;
    virtual bool Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH) = 0;
    virtual void Release(IGraphicsDevice& gfx) = 0;
    virtual void ReleasePipelines(IGraphicsDevice& gfx) = 0;
    // A secondary view's core records with @p from's pipelines (a core of the same backend, built; not
    // its SHaRC builds - a view runs no cache): it never builds or releases them - @p from's owner does.
    virtual void AdoptPipelines(const Core& from) = 0;
    // Returns true when the reservoirs must be (re)allocated; sets @p historyReset
    // when the reuse history is invalid.
    virtual bool ApplySettings(const CoreSettings& s, bool& historyReset) = 0;
    virtual bool Reuse() const = 0;
    virtual void FillConstants(IGraphicsDevice& gfx, Constants& c, const FillContext& ctx) = 0;
    // Before the shell's Clear / Trace (Trace writes the core's initial reservoir).
    virtual void PreShell(const CoreFrame& f) = 0;
    // After Trace: Temporal / Spatial / Shade.
    virtual void Record(const CoreFrame& f) = 0;
    virtual const char* InertReason() const = 0;
    virtual bool IsAllocated() const = 0;   // see RTDI::Core::IsAllocated
    virtual const char* IgnoredFlags() const = 0;
    virtual uint64_t ResidentBytes() const = 0;
    virtual double PipelineMs() const = 0;
    virtual bool Feedback() const = 0;
    virtual uint32_t SpatialRadius() const = 0;
    // SHaRC: the core's entry that re-shades a GI hit (its history validation),
    // built once more with the cache - @p defines and 16-bit types
    // (ShaderDiskCache::CompileFlags{false, true}, recorded for hot reload).
    // Both cores have one (the engine core's Temporal, the RTXDI core's Prepare);
    // a core without one (the default) leaves the cache off.
    virtual bool BuildSharcPipelines(IGraphicsDevice& /*gfx*/, const RHI::PipelineLayout& /*shell*/,
                                     const std::vector<DxcCompiler::Define>& /*defines*/) { return false; }
    virtual bool HasSharcPipelines() const { return false; }
    virtual void ReleaseSharcPipelines(IGraphicsDevice& /*gfx*/) {}
};
} // namespace RTGI
