#pragma once
// ReSTIR DI resampling core interface.
//
// RTDIPass is the SHELL: Classify (eligibility + compaction), Prepare (the indirect args),
// NRD, Resolve, the counters' shell slots, the result texture and every name a pack or the
// Lighting fold reads. A core owns everything between Prepare and NRD - its reservoirs, its
// pipelines, its history - and leaves the shell's DiffIn / SpecIn textures holding the
// NRD-packed demodulated signal of the sample it selected (the contract every core meets).
//
// The engine core (RTDIEngineCore) runs the Initial / Temporal / Spatial / Shade entries of
// RTDI.cs.hlsl on the shell's pipeline layout (ReSTIR::CreateShellLayout) and its shared
// constant block; the RTXDI core brings its own constants (b1) on the same layout.

#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "Graphics/FrameCB.h"
#include "Graphics/GraphicsStruct.h"
#include <cstdint>

class IGraphicsDevice;

namespace RTDI
{
// The constant block every RTDI.cs.hlsl entry reads (cbuffer Params). Layout
// is the shader's; the shell fills its fields, the core its own.
struct alignas(16) Constants
{
    float invViewProj[16], worldToView[16];
    float camera[3]; float rayTMax;
    uint32_t width,height,traceW,traceH;
    uint32_t depthIdx,normalIdx,albedoIdx,surfaceIdx;
    uint32_t extraIdx,velocityIdx,materialsIdx,lightsIdx;
    uint32_t worldGrid,lightCount,alphaInstanceCount,frame;
    uint32_t diffUav,specUav,normalUav,viewZUav;
    uint32_t mvUav,diffSrv,specSrv,flags;
    uint32_t listUav,listSrv,counterUav,argsUav;
    uint32_t candidates,alphaTestEnabled,stageIndex,confidenceUav;
    uint32_t initialUav,initialSrv,previousSrv,temporalUav;
    uint32_t temporalSrv,previousSurfaceSrv,surfaceUav,historyReset;
    uint32_t maxM,spatialRadius,historyWidth,historyHeight;
    uint32_t rawSpecSrv; float mirrorRoughness; uint32_t spatialCandidates,maxAge;
    float historyJitterDelta[2]; uint32_t confidenceSrv,areaLite;
    uint32_t tracePhase,flags2,lightMapIdx,prevLightCount; // lightMapIdx 0 = no light-list translation (Lights v2)
};
static_assert(sizeof(Constants)==368);
// Each dispatch's constants occupy immutable, 256-byte-aligned storage:
// rewriting a single mapped CB after recording would change earlier stages.
struct alignas(256) StageConstants { Constants values; };
static_assert(sizeof(StageConstants)==512);
// Stage 0: Initial / Temporal / first Spatial; 1: the second spatial stage;
// 2: Shade (reads the shaded reservoir).
struct ConstantBlock { StageConstants stages[3]; };

// The GPU profiler keeps region names by pointer: static tables, one per shell instance - the
// compare mode's shadow pass has its own, so its regions never fold into the composited
// pass's.
struct RegionNames
{
    const char *clear, *classify, *prepare, *initialShade, *temporal, *spatial, *spatial1, *shade;
    const char *presample, *regir, *convert, *relax, *reblur, *resolve, *boiling;
};
extern const RegionNames kMainRegions, kShadowRegions;

// What one frame's recording hands the core.
struct CoreFrame
{
    IGraphicsDevice* gfx = nullptr;
    RHI::CommandList cl{};
    // The shell's Prepare output: one 2-D dispatch's group counts, in
    // INDIRECT_ARGUMENT state (IGraphicsDevice::DispatchComputeIndirect).
    const RHI::GPUBuffer* args = nullptr;
    FrameCB<ConstantBlock>* cb = nullptr;         // already written for this frame
    float jitterX = 0.f, jitterY = 0.f;           // this frame's camera jitter
    uint32_t phase = 0;
    uint32_t frame = 0;
    bool reset = false;                           // the shell's history reset
    const RegionNames* regions = &kMainRegions;
};

// What a core reads beyond the shell's constants (the RTXDI core's bridge): the Lights v2
// light lists, the light tables, the view.
struct SceneInputs
{
    uint32_t lightsIdx = 0, lightCount = 0;
    uint32_t prevLightsIdx = 0, prevLightCount = 0;
    uint32_t lightMapIdx = 0;          // 0 = the list kept its keys and order
    // LightSamplingPass's PDF texture (0 = not built this frame).
    uint32_t lightPdfIdx = 0, lightPdfSize = 0, lightPdfMips = 0;
    float view[16] = {};               // world -> view, row-vector (the frame's view matrix)
    float camera[3] = {};
    float rayTMax = 0.f;
    uint32_t renderW = 0, renderH = 0;
};

// What FillConstants needs from the shell this frame.
struct FillContext
{
    uint32_t shellFlags = 0;     // the shell's flag word (CLI + reuse bit)
    uint32_t phase = 0;          // this frame's 2x2 trace phase
    uint32_t frame = 0;
    bool reset = false;          // the shell's history reset
    float jitterX = 0.f, jitterY = 0.f;
    uint32_t traceW = 0, traceH = 0;
    const SceneInputs* scene = nullptr;
};

// The settings the shell forwards (engine.json / CLI).
struct CoreSettings
{
    uint32_t spatialPasses = 1, spatialRadius = 16, secondRadius = 0, spatialK = 3;
    uint32_t maxM = 20, maxAge = 63, candidates = 8;
    bool reuse = true;
    // Which values come from the command line (RTDIPass::Settings).
    bool reusePinned = false, spatialPassesPinned = false, spatialRadiusPinned = false, ageCapPinned = false;
};

class Core
{
public:
    virtual ~Core() = default;
    virtual ReSTIR::Backend Backend() const = 0;
    // First use, from the shell's EnsureResources (never from Execute), on a
    // thread of its own beside the shell's build. Every core's pipelines are
    // built on the shell's layout, so the shell's arguments hold across them.
    virtual bool BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell) = 0;
    virtual bool HasPipelines() const = 0;
    // (Re)allocate the working set for a trace extent; Release really frees it.
    virtual bool Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH) = 0;
    virtual void Release(IGraphicsDevice& gfx) = 0;
    virtual void ReleasePipelines(IGraphicsDevice& gfx) = 0;
    // A secondary view's core records with @p from's pipelines (a core of the same composition, built):
    // it never builds or releases them - @p from's owner does.
    virtual void AdoptPipelines(const Core& from) = 0;
    virtual void ResetHistory() = 0;
    // Returns true when the change needs Allocate again (a reservoir count);
    // @p historyReset when the history no longer applies. @p distance is the
    // shell's ray distance, for the [RTDI_SETTINGS] line.
    virtual bool ApplySettings(const CoreSettings& s, float distance, bool distanceChanged, bool& historyReset) = 0;
    virtual void ApplyBudgetStage(uint32_t stage) = 0;
    virtual bool Reuse() const = 0;
    // The core's fields of the constants (reuse-chain flag bits, reservoir
    // indices, caps, reprojection deltas) - before the shell copies them into
    // the three stages - then its per-stage patches.
    virtual void FillConstants(IGraphicsDevice& gfx, Constants& c, const FillContext& ctx) = 0;
    virtual void PatchStages(IGraphicsDevice& gfx, ConstantBlock& block) = 0;
    // Before the shell's Clear / Classify / Prepare: the core's buffers into
    // the states those entries need (Classify zeroes the engine core's
    // reservoirs and writes its receiver history).
    virtual void PreShell(const CoreFrame& f) = 0;
    // After Prepare: Initial .. Shade (indirect dispatches over the shell's list),
    // leaving the shell's DiffIn / SpecIn holding the selected samples.
    virtual void Record(const CoreFrame& f) = 0;
    virtual const char* InertReason() const = 0;
    // The working set Allocate built and Release frees (the shell re-Allocates when the pass is
    // wanted again).
    virtual bool IsAllocated() const = 0;
    virtual const char* IgnoredFlags() const = 0;
    virtual uint64_t ResidentBytes() const = 0;
    virtual double PipelineMs() const = 0;
    // For the [RTDI] line: what the core's reuse flags were.
    virtual bool Feedback() const = 0;
    virtual bool VisibilityMIS() const = 0;
};
} // namespace RTDI
