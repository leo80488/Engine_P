#pragma once

// DDGIVolumeManager - owns every per-volume GPU resource for DDGI.
//
// Atlases tile probes as a 2D grid, probeCountX columns x probeCountY*probeCountZ rows:
//   Irradiance atlas (R16G16B16A16_FLOAT): 6x6 octahedral patch + 1-texel border =
//     8x8 per probe, linear irradiance (E/pi); see DDGICommon.hlsli's storage note.
//   Depth atlas (R16G16_FLOAT): 16x16 patch + border = 18x18 per probe; .x = mean
//     distance, .y = its variance (Chebyshev visibility, DDGISampling.hlsli).
// Buffers: ProbeData (relocation offset, classification state, stats), ray data (one
// float4 per [probe][ray]: rgb radiance, a hit distance), and the volume CB (origin /
// extent / probe counts / random rotation / biases ...).
//
// Lifecycle: AllocateOrUpdate creates a volume's resources, or reallocates them when
// the probe counts / raysPerProbe change; Tick runs per frame (random rotation, CB,
// VolumeGPUDesc array); Free releases a slot.
//
// Bindings: no graphics root parameters. Tick() packs a StructuredBuffer<VolumeGPUDesc>;
// Lighting.ps opens it from LightCB.ddgiVolumeBufIdx, and each volume's irradiance /
// depth / probe-data SRV from that element's irradianceBaseIdx / depthBaseIdx /
// probeDataBaseIdx (block base + volume slot) through ResourceDescriptorHeap[].

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/DescriptorHeapAllocator.h"
// RootSignatureLayout.h is a plain-integer header (no D3D types) owning the byte offset
// of every folded bindless index, including the three VolumeGPUDesc fields pinned
// against it by static_assert below.
#include "Graphics/RootSignatureLayout.h"
#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>
#include <vector>

class IGraphicsDevice;
class GraphicsDX12;
struct DDGIVolumeComponent;
struct DDGIVolumeRuntimeComponent;

// DDGI ground-truth diagnostics (see RecordDebugReadback). 0 = compiled out entirely
// (readback, copies, logs). Mirrored in DDGISceneAS.h for the gather counters.
#ifndef DDGI_GT_DIAGNOSTICS
#define DDGI_GT_DIAGNOSTICS 0
#endif

namespace DDGI
{

// Octahedral patch sizes (probe-local). Border adds +1 texel each side.
constexpr uint32_t kIrradianceProbeSize = 6;
constexpr uint32_t kIrradianceProbeStride = kIrradianceProbeSize + 2; // 8
constexpr uint32_t kDepthProbeSize       = 16;
constexpr uint32_t kDepthProbeStride     = kDepthProbeSize + 2;       // 18

// Engine-wide ceiling on simultaneously active DDGI volumes. Binding does not limit
// it: Lighting.ps reads DDGI fully bindless, and HybridReflIntersect{HW,SW} and SSGITrace
// give each of their three DDGI arrays its own register space (space4/5/6) - keep them
// apart: in one space the arrays overlap once wider than their register spacing.
// What the width costs:
//   * VRAM: a dense 32x20x32 @128-ray volume is ~89 MB, eight are ~708 MB.
//   * Shading: the sampler resolves at most the two highest-ranked volumes per pixel,
//     but DDGI_PickVolumes walks every slot.
//   * Three descriptor blocks of this width (AllocateStatic in Init).
constexpr uint32_t kMaxVolumes = 8;
// The descriptor block width the shader walks with `base + volumeSlot`. No root
// signature declares the tables, so this assert is the only C++ tie between the three
// mirrors (here, DDGI_MAX_VOLUMES in DDGICommon.hlsli, Gfx::DDGIMaxVolumes).
static_assert(kMaxVolumes == RHI::Slots::Gfx::DDGIMaxVolumes,
              "DDGI::kMaxVolumes must match RHI::Slots::Gfx::DDGIMaxVolumes "
              "and DDGI_MAX_VOLUMES in DDGICommon.hlsli");

// Mirror of the DDGICommon.hlsli #define: a probe's ray count is stored in buckets of
// DDGI_RAY_BUCKET_COUNT rays.
constexpr uint32_t DDGI_RAY_BUCKET_COUNT = 4;

// GPU-visible descriptor mirroring DDGIVolumeComponent + atlas slot indices.
// 1:1 with HLSL `struct DDGIVolumeGPU` in DDGICommon.hlsli (keep in sync).
struct VolumeGPUDesc
{
    // See DDGIVolumeComponent::perceptualBlendWeight.
    DirectX::XMFLOAT3 origin;       float perceptualBlendWeight = 1.0f;
    // See DDGIVolumeComponent::changeResponseBoost. A new per-volume GPU field takes
    // _padUC2 or grows the struct (update the sizeof static_assert and the HLSL mirror
    // together); padC / _padSun0 / _padSun1 are reserved (see the sun fields).
    DirectX::XMFLOAT3 extent;       float changeResponseBoost = 0.0f;
    DirectX::XMFLOAT3 probeSpacing; float padC = 0;
    uint32_t          probeCountsX, probeCountsY, probeCountsZ;
    uint32_t          raysPerProbe;
    float             hysteresis;
    float             normalBias;
    float             viewBias;
    float             boundaryFadeRatio;

    uint32_t          flags;          // bit 0 enabled, 1 relocation, 2 classification, 3 overwrite atlas, 5 adaptive ray budget

    // ---- Descriptor-block base heap indices ----
    // Lighting.ps opens ResourceDescriptorHeap[NonUniformResourceIndex(base + volumeSlot)];
    // NonUniformResourceIndex is required because the volume slot is a per-pixel loop
    // variable. Each block is one AllocateStatic(kMaxVolumes) made in Init and never moved
    // (create / destroy rewrite the descriptor at a fixed offset: WriteSlotDescriptors /
    // WriteSlotNullDescriptors), yet Tick() recomputes the index every frame from the live
    // GPU handle: a heap index is never cached.
    //
    // The HLSL types must match the SRVs exactly (Resource Binding Tier 3 does not
    // validate):
    //     irradianceBaseIdx -> Texture2D<float4>               (R16G16B16A16_FLOAT)
    //     depthBaseIdx      -> Texture2D<float2>               (R16G16_FLOAT)
    //     probeDataBaseIdx  -> StructuredBuffer<DDGIProbeData> (stride 48)
    //
    // Only pixel shaders open them (DDGISampling.hlsli takes the resources as
    // parameters). A root signature without CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED fails a heap
    // read at PSO creation, not at dxc compile time.
    uint32_t          irradianceBaseIdx   = 0;   // irradiance atlas block base
    uint32_t          depthBaseIdx     = 0;   // depth atlas block base
    uint32_t          probeDataBaseIdx = 0;   // probe-data block base

    DirectX::XMFLOAT4 randomRotation0; // float3x3 packed as 3 float4s; .w unused
    DirectX::XMFLOAT4 randomRotation1;
    DirectX::XMFLOAT4 randomRotation2;

    DirectX::XMFLOAT3 diffuseTint;   float diffuseScale = 1.0f;

    // Trace-time lighting: the trace CS picks a random light per ray from the cluster
    // GPULight buffer (bound by DDGIPass at t7), evaluates direct lighting + a shadow ray
    // and importance-samples by multiplying by lightCount.
    //   lightCount - GPULight entries in the bound buffer.
    //   frameIndex - per-volume execution counter (multi-bounce branch + RNG seed).
    uint32_t          lightCount   = 0;
    uint32_t          frameIndex   = 0;
    // Temporal amortization divisor (see DDGIVolumeComponent::probeUpdateDivisor);
    // 1 = every probe every frame. Tick clamps it to [1, 8].
    uint32_t          probeUpdateDivisor = 1;
    // Scheduling / arbitration, packed:
    //   bits  0..15 : priority     (authored, clamped to [0, 0xFFFF])
    //   bits 16..31 : cascadeLevel (authored)
    // Read by the volume arbitration in DDGISampling.hlsli: of several volumes covering a
    // point, the finest / highest-priority one wins instead of being averaged with the
    // coarse one it is nested in. Mirror: `schedulingPacked` in DDGIVolumeGPU
    // (DDGICommon.hlsli).
    uint32_t          schedulingPacked = 0;

    // Directional sun (mirrors LightCB.lightDir / lightColor); keep byte-for-byte in sync
    // with DDGICommon.hlsli.
    // TODO(DDGI_MultiVolume_Plan M7): the toroidal scroll offset (int3) goes in padC,
    // _padSun0 and _padSun1 - keep them free.
    DirectX::XMFLOAT3 sunDirection{ 0.0f, -1.0f, 0.0f }; float _padSun0 = 0;
    DirectX::XMFLOAT3 sunColor    { 0.0f,  0.0f, 0.0f }; float _padSun1 = 0;

    // Amortization blend-compensation exponent; see
    // DDGIVolumeComponent::updateCompensation.
    float             updateCompensation = 1.0f;
    uint32_t          worldLightGridIndex = ~0u;
    // The sky's radiance scale on a probe ray's miss: LightCB iblStrength, the scale the
    // path tracer, the ReSTIR GI cache, the raster sky fallback and the reflection-probe
    // bake apply to the same cube, so the probes see the sky everything else sees.
    float             skyScale = 1.0f;
    float             _padUC2 = 0;
};

// Pin the folded index fields against RootSignatureLayout.h, the single source
// of truth for every folded offset in the engine. HLSL reads them at these byte
// offsets out of DDGIVolumeGPU (DDGICommon.hlsli); a silent drift feeds
// ResourceDescriptorHeap[] a garbage index, which under HEAP_DIRECTLY_INDEXED
// opens whatever descriptor happens to sit there rather than failing.
static_assert(offsetof(VolumeGPUDesc, irradianceBaseIdx)
                  == RHI::Slots::BindlessIdx::kDDGIVolumeDesc_IrradianceBaseIdx &&
              offsetof(VolumeGPUDesc, depthBaseIdx)
                  == RHI::Slots::BindlessIdx::kDDGIVolumeDesc_DepthBaseIdx &&
              offsetof(VolumeGPUDesc, probeDataBaseIdx)
                  == RHI::Slots::BindlessIdx::kDDGIVolumeDesc_ProbeDataBaseIdx,
              "VolumeGPUDesc fold-batch-3 base-index fields moved — "
              "Lighting.ps.hlsl reads them at these byte offsets "
              "(mirror: DDGICommon.hlsli struct DDGIVolumeGPU)");

// Element stride of the engine-wide StructuredBuffer<DDGIVolumeGPU> - ABI: Init()
// sets the SRV's StructureByteStride from sizeof(VolumeGPUDesc), while the shader
// derives its stride from HLSL `struct DDGIVolumeGPU`. If they disagree, every field
// past the drift is read from the wrong address, including the heap base indices,
// which then feed ResourceDescriptorHeap[] garbage with no validation message. Append
// new fields at the tail so earlier offsets stay put.
static_assert(sizeof(VolumeGPUDesc) == 224,
              "VolumeGPUDesc size changed — HLSL struct DDGIVolumeGPU in "
              "DDGICommon.hlsli must change identically, or Lighting.ps indexes "
              "the volume buffer with the wrong stride");

// Per-probe state stored in ProbeData buffer (written by classify/relocate,
// read by diffuse + indirect-specular sampling). Keep byte-identical to
// DDGIProbeData in DDGICommon.hlsli.
struct ProbeData
{
    DirectX::XMFLOAT3 offset{ 0.0f, 0.0f, 0.0f }; // relocation offset
    uint32_t          state = 0;    // 0 = ACTIVE (default), 1 = INACTIVE.
    float             skyVisibility = 1.0f; // EMA of traced sky-miss ratio.
    // Temporally low-passed relocation push (world units), written only by
    // DDGIProbeRelocate.cs. Zero = no accumulated push, the right cold start.
    DirectX::XMFLOAT3 pushSmooth{ 0.0f, 0.0f, 0.0f };
    // The sky visibility's first moment (mean of miss ? rayDir : 0), written only by
    // DDGIProbeRelocate.cs; with skyVisibility, an L1 projection of "this direction
    // reaches the sky". Zero = no preferred direction, the right cold start.
    DirectX::XMFLOAT3 skyVisDir{ 0.0f, 0.0f, 0.0f };
    float             _padSkyDir = 0.0f;
};
static_assert(sizeof(ProbeData) == 48, "ProbeData must mirror DDGIProbeData");

class DDGIVolumeManager
{
public:
    // ---- Probe watch (runtime diagnostic) ----
    // Reads one probe's ProbeData back from the GPU so a scenario
    // (Engine.DDGIWatchProbe / Engine.DDGIWatchedProbe) or the log (DDGI.WATCH lines) can
    // watch relocation converge. One ProbeData per capture; off unless a watch is set.
    // Cadence: at best one capture per kProbeWatchPeriodFrames (kFrameCount + 3 = 6) -
    // armed at Execute N, the fence counts down over Ticks N+1..N+5, the Map lands on
    // Tick N+6 and Execute N+6 re-arms - and only on frames the volume executes (a
    // Dormant volume captures once per heartbeat). Size scenario wait windows from that,
    // not from kFrameCount.
    struct ProbeWatch
    {
        bool              valid = false;      // at least one capture landed
        int32_t           x = -1, y = -1, z = -1;
        uint32_t          volumeSlot = 0;
        uint32_t          capture = 0;        // monotonic capture counter
        DirectX::XMFLOAT3 offset{ 0.0f, 0.0f, 0.0f };
        uint32_t          stateRaw = 0;       // DDGI_PackProbeState: bits0-1 state, bit2 FRESH, bits8-15 bfSmooth
        float             skyVisibility = 0.0f;
        DirectX::XMFLOAT3 skyVisDir{ 0.0f, 0.0f, 0.0f };   // the sky visibility's first moment
    };
    static constexpr uint32_t kProbeWatchPeriodFrames = RHI::kFramesInFlight + 3;

    // ---- Depth-atlas state ----
    // Depth-atlas state tracked by TransitionVolumeAtlases: UAV (relight / border writes) or
    // SRV (read by the trace's multi-bounce visibility and by Lighting).
    enum class AtlasState : uint8_t { UAV, SRV };

    // ---- Lifetime and volume allocation ----
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Allocate (or reallocate on size change) GPU resources for one volume.
    // Stamps runtime.volumeSlot + the cached probeCounts/raysPerProbe.
    bool AllocateOrUpdate(IGraphicsDevice& gfx,
                          const DDGIVolumeComponent& volume,
                          DDGIVolumeRuntimeComponent& runtime);

    // Free the resources occupied by @p slot. Safe with ~0u (no-op).
    void Free(IGraphicsDevice& gfx, uint32_t slot);

    // Frees every allocated slot not in @p claimedSlots (the frame's slots referenced by
    // live runtime components); the rest are orphans (entity destroyed, scene reload,
    // re-created runtime component). Without it a stale slot and its replacement coexist,
    // and lighting reads one slot's buffer with the other's probe-count layout (out of
    // bounds -> garbage irradiance).
    void FreeUnclaimedSlots(IGraphicsDevice& gfx,
                            const uint32_t* claimedSlots,
                            uint32_t        claimedCount);

    // ---- Probe watch (runtime diagnostic) ----
    // Both only file a REQUEST; Tick validates and latches it (see the
    // threading note by the members). Main thread only.
    void              SetProbeWatch(int32_t x, int32_t y, int32_t z, uint32_t volumeSlot = 0);
    void              ClearProbeWatch();
    const ProbeWatch& GetProbeWatch() const { return m_watchLast; }
    // Called by DDGIPass at the end of its Execute for the volume it ran.
    void RecordProbeWatch(IGraphicsDevice& gfx, RHI::CommandList cmd,
                          uint32_t slot, bool onComputeQueue);

    // ---- Per-frame update and execution ----
    // Per frame: rolls each volume's random rotation, refreshes its CB and packs the
    // engine-wide VolumeGPUDesc array; AllocateOrUpdate must have run for every active
    // volume earlier this frame. @p lightCount = ClusterPass::GetLightCount(): the trace
    // CS picks a random point / spot light in [0, lightCount) per ray. @p sunDirection /
    // @p sunColor are the final LightCB sun (authored directional, or the TOD body),
    // evaluated by the trace CS so DDGI's directional GI matches deferred lighting.
    void Tick(IGraphicsDevice& gfx,
              const DDGIVolumeComponent* const* volumes,
              DDGIVolumeRuntimeComponent* const* runtimes,
              uint32_t volumeCount,
              uint32_t lightCount,
              const DirectX::XMFLOAT3& sunDirection,
              const DirectX::XMFLOAT3& sunColor,
              uint32_t worldLightGridIndex = ~0u,
              float skyScale = 1.0f);   // VolumeGPUDesc::skyScale (LightCB iblStrength)

    // Records that @p slot's probe dispatches executed this frame; Renderer calls it
    // right after DDGIPass::Execute. The volume's frame counter (DDGIVolumeGPU::frameIndex)
    // advances per execution, not per Tick, because every consumer wants executions:
    //   * the amortization interleave updates a probe iff (x + 2y + 3z) % N ==
    //     frameIndex % N; counting skipped frames with a volume executed every M frames
    //     and gcd(M, N) > 1 never traces a subset of probes again (M = N = 4 freezes a
    //     quarter of the grid);
    //   * DDGIPrepareRayCount re-tests INACTIVE probes on a period of 16; a skip cadence of
    //     32 would hit one residue and leave buried probes INACTIVE (black holes);
    //   * the random-rotation jitter and the `frameIndex < 8` burn-in.
    void MarkVolumeExecuted(uint32_t slot);

    // Whether the camera-relevance policy wants @p slot's probe dispatches this frame:
    // false only for a Dormant volume on a non-heartbeat frame (or an unallocated slot);
    // true for an allocated slot with no policy decision yet.
    bool ShouldExecuteVolume(uint32_t slot) const;

    // ---- Diagnostic ground-truth readback ----
    // Diagnostic ground-truth readback, compiled out unless DDGI_GT_DIAGNOSTICS is 1
    // (here, and in DDGISceneAS.h for the gather counters). Tick() arms one volume slot
    // every 600 volume frames; DDGIPass calls this at the end of Execute (after every DDGI
    // dispatch wrote its buffers), and it copies the ray-alloc totals plus strided
    // ray-data / probe-data samples into a READBACK buffer. A few Ticks later they are
    // mapped and logged as one "DDGI.GT[slot]" line: total rays, miss / backface /
    // frontface mix with mean hit distances, probe offset / state / skyVisibility stats.
    // No-op unless armed.
    void RecordDebugReadback(IGraphicsDevice& gfx,
                             RHI::CommandList cmd,
                             uint32_t slot,
                             bool onComputeQueue);

    // ---- Descriptors and per-volume resources ----
    // SRV GPU handle of the packed StructuredBuffer<VolumeGPUDesc> read by Lighting.ps and
    // the DDGI passes; one copy per frame in flight, this returns the current frame's.
    // 0 before Init().
    uint64_t  GetVolumeBufferSrv(IGraphicsDevice& gfx) const;

    // Descriptor-block base GPU handles: each block holds kMaxVolumes SRVs in slot order;
    // a slot without a volume holds a type-correct null SRV (never read: the per-slot
    // `flags & 1u` gate skips it). 0 before Init() succeeds. Lighting.ps does not bind
    // them: Tick() stamps each block's heap index into every VolumeGPUDesc and the shader
    // opens `base + volumeSlot`. RTReflectionPass and SSGIPass bind them as tables on their
    // own root signatures; RTGIPass passes their heap indices.
    uint64_t  GetIrradianceTableGpu()    const { return m_irradianceTableGpu; }
    uint64_t  GetDepthTableGpu()      const { return m_depthTableGpu; }
    uint64_t  GetProbeDataTableGpu()  const { return m_probeDataTableGpu; }

    // Per-volume SRVs. The irradiance atlas is an octahedral Texture2D (RGBA16F,
    // kIrradianceProbeStride^2 texels per probe) sampled bilinearly, the same shape as the
    // depth atlas; see DDGICommon.hlsli's storage note.
    uint64_t  GetIrradianceAtlasSrv        (uint32_t slot) const;
    uint64_t  GetDepthAtlasSrv     (uint32_t slot) const;
    uint64_t  GetProbeDataSrv      (uint32_t slot) const;
    uint64_t  GetIrradianceAtlasUav        (uint32_t slot) const;
    uint64_t  GetDepthAtlasUav     (uint32_t slot) const;
    uint64_t  GetRayDataUav        (uint32_t slot) const;
    uint64_t  GetRayDataSrv        (uint32_t slot) const;
    uint64_t  GetProbeDataUav      (uint32_t slot) const;
    uint64_t  GetVarianceUav       (uint32_t slot) const;
    uint64_t  GetRayCountUav       (uint32_t slot) const;  // per-probe bucket counts + active-probe list
    uint64_t  GetRayAllocUav       (uint32_t slot) const;  // total + ray descriptors
    uint64_t  GetDispatchArgsUav   (uint32_t slot) const;  // D3D12_DISPATCH_ARGUMENTS

    // Underlying GPUBuffer for the dispatch-args buffer. DDGIPass needs the raw
    // ID3D12Resource* to feed ExecuteIndirect's args parameter, so the manager
    // exposes the buffer (not just its UAV handle) for that one call site.
    const RHI::GPUBuffer* GetDispatchArgsBuffer(uint32_t slot) const;

    // The irradiance atlas texture: DDGIPass needs the raw ID3D12Resource* for its
    // per-frame NPSR <-> UAV transitions.
    const RHI::Texture* GetIrradianceAtlasTexture(uint32_t slot) const;

    // The probe-data buffer (DDGIProbeData per probe): DDGIPass needs the raw resource
    // to flip NPSR (the trace's multi-bounce SRV read) -> UAV (relocation CS write) within
    // the frame.
    const RHI::GPUBuffer* GetProbeDataBuffer(uint32_t slot) const;

    // ---- Raw resources for the per-dispatch UAV barriers ----
    // DDGIPass names the one resource each UAV barrier between its dispatches orders,
    // rather than a null-resource (device-wide) UAV barrier that would serialise the
    // queue against unrelated work 8 times per volume per frame.
    const RHI::GPUBuffer* GetRayCountBuffer   (uint32_t slot) const;
    const RHI::GPUBuffer* GetRayAllocBuffer   (uint32_t slot) const;
    const RHI::GPUBuffer* GetRayDataBuffer    (uint32_t slot) const;
    const RHI::Texture*   GetDepthAtlasTexture(uint32_t slot) const;

    // The per-volume CB GPUBuffer of the current frame's ring slot. DDGIPass resolves its
    // GPU VA at bind time through the backend (GraphicsDX12::GetBufferResource): the
    // manager stores no VA because IGraphicsDevice does not expose one.
    const RHI::GPUBuffer* GetVolumeCB(IGraphicsDevice& gfx, uint32_t slot) const;

    // Transitions @p slot's depth atlas to @p newState (no-op for an unallocated slot, or when
    // already in that state with the same SRV mask). Only the depth atlas: the irradiance
    // atlas and probeData follow DDGIPass's own state cycle.
    // @p computeQueueState makes the SRV target NON_PIXEL_SHADER_RESOURCE only - the only
    // SRV state valid on a compute list (the full PIXEL | NON_PIXEL mask trips the debug
    // layer / risks a TDR there); the caller then re-promotes it on the graphics queue
    // before LightingPass binds the atlas.
    void      TransitionVolumeAtlases(IGraphicsDevice& gfx,
                                      RHI::CommandList cmd,
                                      uint32_t slot, AtlasState newState,
                                      bool computeQueueState = false);

    // Graphics queue: promotes each allocated slot's depth atlas, irradiance atlas and
    // probeData buffer from the NON_PIXEL_SHADER_RESOURCE state the compute-queue DDGI
    // leaves to the full PIXEL | NON_PIXEL state LightingPass's pixel shaders need. Call
    // on a graphics list after the cross-queue fence guarantees the DDGI compute work is
    // done. No-op when DDGI ran on the graphics queue.
    void      PromoteAtlasesForGraphicsQueue(IGraphicsDevice& gfx,
                                             RHI::CommandList graphicsCmd);

    // Inverse of PromoteAtlasesForGraphicsQueue: demotes the depth atlas (when in SRV),
    // the irradiance atlas and the probeData buffer from PIXEL | NON_PIXEL back to
    // NON_PIXEL_SHADER_RESOURCE, so the next compute-queue DDGI Execute can transition
    // them. It must run on a graphics list: a compute-queue barrier with StateBefore =
    // PIXEL | NON_PIXEL (0xC0) is rejected, PIXEL_SHADER_RESOURCE (0x80) being invalid on
    // compute. Idempotent per frame (buffersInComputeOnlyState / atlasInComputeOnlyState).
    // Called once per frame before the compute-queue DDGI list.
    void      DemoteForComputeQueue(IGraphicsDevice& gfx,
                                    RHI::CommandList graphicsCmd);

    // Probe count helpers for sizing dispatches.
    uint32_t  GetProbeCount(uint32_t slot) const;
    uint32_t  GetRaysPerProbe(uint32_t slot) const;
    uint32_t  GetDepthAtlasWidth      (uint32_t slot) const;
    uint32_t  GetDepthAtlasHeight     (uint32_t slot) const;

    // Number of currently-allocated volumes (slot count up to kMaxVolumes).
    uint32_t  GetActiveVolumeCount() const { return m_activeCount; }

private:
    // Frame-pipelining depth (RHI::kFramesInFlight): every per-volume and engine-wide
    // UPLOAD buffer is ringed so frame N+1's CPU writes do not trample frame N's pending
    // GPU read.
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct VolumeResources
    {
        bool          allocated = false;
        uint32_t      probeCountsX = 0, probeCountsY = 0, probeCountsZ = 0;
        uint32_t      raysPerProbe = 0;
        AtlasState    atlasState   = AtlasState::SRV; // AllocateOrUpdate sets UAV: the depth atlas is created in UAV
        // True while the depth atlas's SRV state is NON_PIXEL_SHADER_RESOURCE only: its last
        // SRV transition was on a compute queue, or DemoteForComputeQueue narrowed it.
        // PromoteAtlasesForGraphicsQueue lifts it back to the full PIXEL | NON_PIXEL state on
        // the graphics queue.
        bool          atlasInComputeOnlyState = false;
        // Mirrors atlasInComputeOnlyState for the irradiance atlas + probeData buffer, whose
        // state cycle is DDGIPass's, not TransitionVolumeAtlases'. False at creation: both
        // start in the full SHADER_RESOURCE (NPSR|PSR) mask.
        bool          buffersInComputeOnlyState = false;
        // Set by MarkVolumeExecuted() when DDGIPass actually ran this slot's
        // dispatches; consumed and cleared by the NEXT Tick(). See the
        // MarkVolumeExecuted comment for why the frame counter cannot simply
        // advance every Tick.
        bool          executedSinceLastTick = false;
        // Mirror of DDGIVolumeRuntimeComponent::executeThisFrame, published by Tick():
        // Renderer's dispatch loop walks slots, not components, so the relevance policy's
        // skip decision lives here. Defaults true so a volume allocated before the policy
        // first runs still executes.
        bool          executeThisFrame = true;

        RHI::Texture   irradianceAtlas;   // RGBA16F octahedral, 6x6 + 1-texel border per probe; .rgb = linear E/pi
        RHI::Texture   depthAtlas;        // R16G16F
        RHI::GPUBuffer probeDataBuffer;   // RWStructuredBuffer<ProbeData>
        RHI::GPUBuffer rayDataBuffer;     // RWStructuredBuffer<float4> (radiance + dist)
        RHI::GPUBuffer varianceBuffer;    // RWStructuredBuffer<DDGIVarianceData>: per-texel mean-estimator state
        RHI::GPUBuffer rayCountBuffer;    // RWStructuredBuffer<uint>: bucket counts + compacted active-probe list
        RHI::GPUBuffer rayAllocBuffer;    // RWStructuredBuffer<uint>: [0] total rays, [1..] probeIdx | rayIdx << 20
        RHI::GPUBuffer dispatchArgsBuffer;// 32 B: trace + relight/border DISPATCH_ARGUMENTS; UAV <-> INDIRECT_ARGUMENT

        // Per-volume CB, upload-mapped, a subset of VolumeGPUDesc. One per frame in flight:
        // Tick() writes randomRotation + frameIndex every frame and DDGIPass /
        // DDGIProbeDebugPass read it on the GPU the same frame, so a single UPLOAD CB would
        // let frame N+1's CPU write race frame N's GPU read.
        RHI::GPUBuffer volumeCB    [kFrameCount];
        void*          cbvMapped   [kFrameCount] = {};
        uint64_t       cbvGpuAddress[kFrameCount] = {};  // stays 0: DDGIPass gets the VA via GetVolumeCB

        // Cached SRV/UAV handles for hot-path lookups.
        uint64_t       irradianceAtlasSrv         = 0;
        uint64_t       depthAtlasSrv      = 0;
        uint64_t       probeDataSrv       = 0;
        uint64_t       rayDataSrv         = 0;
        uint64_t       irradianceAtlasUav         = 0;
        uint64_t       depthAtlasUav      = 0;
        uint64_t       probeDataUav       = 0;
        uint64_t       rayDataUav         = 0;
        uint64_t       varianceUav        = 0;
        uint64_t       rayCountUav        = 0;
        uint64_t       rayAllocUav        = 0;
        uint64_t       dispatchArgsUav    = 0;
    };

    // Diagnostic ground-truth readback sample sizes (see RecordDebugReadback): 4 ray chunks
    // x 1024 rays (16 B float4 each) + 4 probe chunks x 32 probes (sizeof(ProbeData) each:
    // the copies stride by it, so do not hard-code it) + 16 B of ray-alloc counters.
    static constexpr uint32_t kDbgRayChunks     = 4;
    static constexpr uint32_t kDbgRaysPerChunk  = 1024;
    static constexpr uint32_t kDbgProbeChunks   = 4;
    static constexpr uint32_t kDbgProbesPerChunk= 32;
    static constexpr uint64_t kDbgRayBytes      = uint64_t(kDbgRayChunks) * kDbgRaysPerChunk * 16;
    static constexpr uint64_t kDbgProbeBytes    = uint64_t(kDbgProbeChunks) * kDbgProbesPerChunk * sizeof(ProbeData);
    static constexpr uint64_t kDbgReadbackBytes = 16 + kDbgRayBytes + kDbgProbeBytes;

    void ProcessProbeWatch(IGraphicsDevice& gfx);   // Tick: latch requests, drain captures

    void DestroyVolume(IGraphicsDevice& gfx, VolumeResources& res);
    bool CreateAtlasesForVolume(IGraphicsDevice& gfx,
                                const DDGIVolumeComponent& v,
                                VolumeResources& res);

    void ProcessDebugReadback();

    // Write the slot's real SRVs (AllocateOrUpdate, after CreateAtlasesForVolume) or null
    // SRVs (Free / FreeUnclaimedSlots) into the three blocks.
    void WriteSlotDescriptors  (IGraphicsDevice& gfx, uint32_t slot, const VolumeResources& res);
    void WriteSlotNullDescriptors(IGraphicsDevice& gfx, uint32_t slot);

    // Probe-watch threading: in the threaded Game the render half of frame N runs
    // concurrently with the game half of frame N+1, park-shaped - the game half's
    // RenderSystem parks the render thread, runs BeginFrame -> ... -> Tick, then kicks it. So:
    //   * REQUEST fields are written by the Lua bindings on the main thread at any phase
    //     and read only by Tick (same thread), never by the render thread.
    //   * LATCHED fields are written only inside Tick (render parked) and read by
    //     RecordProbeWatch on the render thread after the kick.
    //   * IN-FLIGHT fields are written by RecordProbeWatch (render) and ProcessProbeWatch
    //     (game, in the next parked window): sequential by construction, no atomics.
    //     RecordProbeWatch writes nothing the Lua getter reads.
    // request (main thread only)
    DirectX::XMINT3 m_watchReqCoord{ -1, -1, -1 };
    uint32_t        m_watchReqSlot  = 0;
    bool            m_watchReqDirty = false;
    // latched (written in Tick, read on the render thread)
    DirectX::XMINT3 m_watchCoord{ -1, -1, -1 };
    uint32_t        m_watchSlot     = 0;
    uint32_t        m_watchIdx      = 0;       // linear probe index, resolved at latch time
    uint32_t        m_watchGen      = 0;       // bumped on every latch/clear
    Microsoft::WRL::ComPtr<ID3D12Resource> m_watchReadback;   // 1 ProbeData, READBACK heap; created in Tick
    // in flight (render writes on arm, game consumes)
    bool            m_watchInFlight    = false;
    uint32_t        m_watchFence       = 0;
    uint32_t        m_watchInFlightGen = 0;    // capture is dropped if != m_watchGen when it lands
    // result (game only)
    uint32_t        m_watchCaptures = 0;
    ProbeWatch      m_watchLast;

    VolumeResources m_volumes[kMaxVolumes];
    uint32_t        m_activeCount = 0;

    // ---- Diagnostic ground-truth readback state (see RecordDebugReadback) ----
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dbgReadback;   // READBACK heap
    uint32_t m_dbgArmedSlot    = 0xFFFFFFFFu;  // slot to capture on next Execute
    uint32_t m_dbgInFlightSlot = 0xFFFFFFFFu;  // slot whose copies are on the GPU
    uint32_t m_dbgFence        = 0;            // Ticks until the Map is safe
    uint32_t m_dbgTotalProbes  = 0;
    uint32_t m_dbgRaysPerProbe = 0;
    uint32_t m_dbgRayChunkCount  [kDbgRayChunks]   = {};
    uint32_t m_dbgProbeChunkCount[kDbgProbeChunks] = {};
    uint32_t m_dbgChunkAnchor    [kDbgRayChunks]   = {};  // probe index each chunk starts at
    uint32_t m_dbgChunkLayerY    [kDbgRayChunks]   = {};  // that anchor's y layer (for the log)

    // Engine-wide volume descriptor buffer (kMaxVolumes entries), an UPLOAD-heap
    // StructuredBuffer<VolumeGPUDesc> rewritten every frame by Tick(). One per frame in
    // flight so frame N+1's CPU write does not overlap frame N's GPU read.
    RHI::GPUBuffer  m_volumeBuffer[kFrameCount];
    void*           m_volumeBufferMapped[kFrameCount] = {};
    uint64_t        m_volumeBufferSrv   [kFrameCount] = {};

    // Contiguous descriptor blocks per resource type (kMaxVolumes slots each), allocated
    // once at Init from the static GPU-visible region; the base GPU handle is stable for
    // the manager's lifetime. Only a slot's SRV is rewritten (WriteSlotDescriptors /
    // WriteSlotNullDescriptors) - no per-frame copy work.
    DescriptorAllocation m_irradianceTable;
    DescriptorAllocation m_depthTable;
    DescriptorAllocation m_probeDataTable;
    uint64_t             m_irradianceTableGpu   = 0;
    uint64_t             m_depthTableGpu     = 0;
    uint64_t             m_probeDataTableGpu = 0;
};

// Atlas dimensions for a probe grid, used by the manager (allocation) and the relight
// / border CS dispatches. Both atlases use the same tiling (DDGI_ProbeAtlasTile() in
// DDGICommon.hlsli: probeCountX columns x probeCountY * probeCountZ rows) and differ
// only in per-probe stride. Keep these four together: a tiling change moves all four,
// DDGI_ProbeAtlasTile and DDGIBorderUpdate as a unit.
inline uint32_t IrradianceAtlasWidth (uint32_t pcx, uint32_t)        { return pcx * kIrradianceProbeStride; }
inline uint32_t IrradianceAtlasHeight(uint32_t,    uint32_t probeRows) { return probeRows * kIrradianceProbeStride; }
inline uint32_t DepthAtlasWidth      (uint32_t pcx, uint32_t)        { return pcx * kDepthProbeStride; }
inline uint32_t DepthAtlasHeight     (uint32_t,    uint32_t probeRows) { return probeRows * kDepthProbeStride; }

} // namespace DDGI
