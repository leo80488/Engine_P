// SharcEngine.hlsli - the SHaRC radiance cache as every consumer compiles it (Graphics/SharcCache.h).
//
// SHaRC (NVIDIA-RTX/SHARC v1.8.3; tools/vendor_sharc.py unpacks its unmodified headers into
// shaders/Sharc/) is a world-space hash grid of outgoing radiance. Its consumers - the RTXDI PT
// core (RtxdiBridgeSharc.hlsli) and the ReSTIR GI shell (RTGI.cs.hlsl under RTGI_SHARC) - compile
// their own kernels, each reading the buffer indices from its own constants; this file is what
// they share: the configuration (one cache semantics, never forked) and the parameter / clear /
// resolve bodies.
//
// The bodies are MACROS, not functions: DXC (fast-math float ops) schedules a by-value helper
// differently, so as a function the PT core's initial-sampling kernel compiles to a different
// float-op mix. A macro expands token for token into each consumer, which keeps all 25 RtxdiPT
// kernels bit-identical to their hand-inlined form. Do not turn the bodies into functions.
//
// SHARC_ENABLE_CACHE_RESAMPLING and SHARC_SAMPLE_NUM_THRESHOLD stay each consumer's (its TU
// defines them before this include, or takes upstream's).
//
// Requires -enable-16bit-types (SharcTypes.h stores fp16 radiance; HLSL 2018 and 2021 alike)
// and, for the update's 64-bit hash insert, 64-bit atomics on descriptor-heap resources
// (RHI::DeviceCapabilities::int64HeapAtomics).
#ifndef SHARC_ENGINE_HLSLI
#define SHARC_ENGINE_HLSLI

// Every configuration switch is stated, so an upstream default change cannot move the cache
// silently. Emission is kept out of the voxels and added back exactly at a query: a lamp's
// radiance need not fit the update's fixed-point accumulation, and an emitter's cache entry still
// says how much light it reflects.
#define SHARC_SEPARATE_EMISSIVE 1
#define SHARC_ENABLE_RESPONSIVE_LIGHTING 0
#define SHARC_ENABLE_SH_ENCODING 0
#define SHARC_MATERIAL_DEMODULATION 0
// Fade acceleration: an entry whose luminance fell on each of the last 32 frames weighs its
// history no more than the frame's own samples (SharcCommon.h resolve). Without it, light that is
// gone (a lamp switched off, a slab whose BLAS landed after the cache traced through it) decays
// only by the 20-frame EMA, and multi-bounce feedback holds it ~400 frames in a closed corridor.
#define SHARC_ENABLE_FADE_ACCELERATION 1
#define SHARC_USE_FP16 0
#define HASH_GRID_COMPACT 0
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1
#include "Sharc/SharcCommon.h"

// Fills `p` (a SharcParameters) from a consumer's constants.
#define SHARC_ENGINE_PARAMETERS(p, camera, hashUav, accumulationUav, resolvedUav, capacityValue, sceneScaleValue, radianceScaleValue) \
    p.hashGridParameters.cameraPosition = (camera); \
    p.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE; \
    p.hashGridParameters.sceneScale = (sceneScaleValue); \
    p.hashGridParameters.levelBias = SHARC_GRID_LEVEL_BIAS; \
    p.hashGridData.capacity = (capacityValue); \
    p.hashGridData.hashEntriesBuffer = ResourceDescriptorHeap[(hashUav)]; \
    p.radianceScale = (radianceScaleValue); \
    p.accumulationBuffer = ResourceDescriptorHeap[(accumulationUav)]; \
    p.resolvedBuffer = ResourceDescriptorHeap[(resolvedUav)]

// The edge of the voxel holding `position` (its level follows the camera
// distance: 2^level / sceneScale, HashGridCommon.h).
#define SHARC_ENGINE_VOXEL_SIZE(position, p) \
    HashGridGetVoxelSize(HashGridGetLevel((position), (p).hashGridParameters), (p).hashGridParameters)

// A clear kernel's body, one thread per entry: every buffer SHaRC reads
// starts at 0 (Integration.md).
#define SHARC_ENGINE_CLEAR(index, hashUav, accumulationUav, resolvedUav, capacityValue) \
    if ((index) >= (capacityValue)) return; \
    RWStructuredBuffer<uint64_t> sharcEngineHashes = ResourceDescriptorHeap[(hashUav)]; \
    RWStructuredBuffer<SharcAccumulationData> sharcEngineAccumulation = ResourceDescriptorHeap[(accumulationUav)]; \
    RWStructuredBuffer<SharcPackedData> sharcEngineResolved = ResourceDescriptorHeap[(resolvedUav)]; \
    sharcEngineHashes[(index)] = 0; \
    sharcEngineAccumulation[(index)] = SharcZeroAccumulationData(); \
    sharcEngineResolved[(index)] = SharcZeroPackedData()

// A resolve kernel's body, one thread per entry: this frame's deposits into
// each entry's history; an entry no path refreshed for `staleFrames` frames
// is evicted. `parameters` is evaluated once, after the resolve parameters.
#define SHARC_ENGINE_RESOLVE(index, parameters, prevCamera, accumulationFrames, staleFrames, frame) \
    SharcResolveParameters sharcEngineResolve; \
    sharcEngineResolve.cameraPositionPrev = (prevCamera); \
    sharcEngineResolve.accumulationFrameNum = (accumulationFrames); \
    sharcEngineResolve.responsiveFrameNum = 1;   /* responsive lighting is off */ \
    sharcEngineResolve.staleFrameNumMax = (staleFrames); \
    sharcEngineResolve.frameIndex = (frame); \
    SharcResolveEntry((index), parameters, sharcEngineResolve)
#endif
