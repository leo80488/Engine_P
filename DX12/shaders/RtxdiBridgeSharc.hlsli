// RtxdiBridgeSharc.hlsli - the SHaRC radiance cache for the RTXDI PT core.
//
// SHaRC (NVIDIA-RTX/SHARC v1.8.3; tools/vendor_sharc.py unpacks its
// unmodified headers into shaders/Sharc/) is a world-space hash grid of
// outgoing radiance. The PT core keeps one: a sparse camera-rooted update
// pass (RtxdiPTSharcUpdate, SHARC_UPDATE) traces ~1 path per tile through the
// bridge's path tracer and deposits every vertex's next-event light, SHaRC
// propagating it to the vertices before; RtxdiPTSharcResolve merges each
// frame into the history; and every path-tracing kernel of the core, compiled
// with SHARC_QUERY, ends a path at a secondary vertex whose cache entry the
// path's footprint covers (RAB_SharcTerminates): the cached radiance plus the
// vertex's own emission stands for the vertex and everything beyond it.
//
// The rule depends on the path's geometry and lobes only, so initial sampling
// and the hybrid shift's replays apply the same one: a replay that meets it
// before its reconnection vertex ends there with no light (a failed shift,
// as every early end of a replay). Included only under RAB_SHARC: a core
// without the cache compiles none of this, and needs no unpacked headers.
//
// Requires -enable-16bit-types (SHaRC stores fp16 radiance; the PT core's
// kernels compile with it) and, for the update's 64-bit hash insert, 64-bit
// atomics on descriptor-heap resources (RHI::DeviceCapabilities::int64HeapAtomics).
#ifndef RTXDI_BRIDGE_SHARC_HLSLI
#define RTXDI_BRIDGE_SHARC_HLSLI

// The cache's configuration and its clear / resolve bodies, shared with the
// ReSTIR GI shell (SharcEngine.hlsli; Graphics/SharcCache.h owns the buffers).
// SHARC_ENABLE_CACHE_RESAMPLING stays upstream's (= SHARC_UPDATE): an update
// path ends in the cache past the propagation depth, so the cache holds every
// bounce beyond it.
#include "SharcEngine.hlsli"

// [PT_RTXDI] counters (RtxdiPT.cs.hlsl's kPTCounterCount): 8 paths the cache
// ended in initial sampling, 9 update-path vertices deposited, 10 initial
// paths whose footprint allowed the cache but found no entry there.
void PTCountSharcSlot(uint slot, bool value)
{
    if (!(g_PT.ptFlags & kRabPTCounters)) return;
    const uint n = WaveActiveCountBits(value);
    if (WaveIsFirstLane() && n != 0)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[g_PT.counterUav];
        InterlockedAdd(counters[slot], n);
    }
}
void PTCountSharc(bool value) { PTCountSharcSlot(8, value); }

// sharcFlags (-sharc_diffuse_only: a diagnostic arm).
static const uint kRabSharcDiffuseOnly = 1u;   // only a diffuse-lobe segment ends in the cache
static const uint kRabSharcFootprintOnly = 2u;  // the last vertex too only where the footprint allows
static const uint kRabSharcGateLast = 4u;       // the last vertex too only on a diffuse-like surface

SharcParameters RAB_SharcParameters()
{
    SharcParameters p;
    SHARC_ENGINE_PARAMETERS(p, g_RabBridge.camera, g_PT.sharcHashUav, g_PT.sharcAccumulationUav, g_PT.sharcResolvedUav,
                            g_PT.sharcCapacity, g_PT.sharcSceneScale, g_PT.sharcRadianceScale);
    return p;
}

float RAB_SharcVoxelSize(float3 position)
{
    const SharcParameters p = RAB_SharcParameters();
    return SHARC_ENGINE_VOXEL_SIZE(position, p);
}

// Whether the segment from `from` (sampled with `brs`) to its hit at hitT may
// end in the cache: the segment longer than the voxel's diagonal (SHaRC's
// rule against self-lookup), and the lobe wider than the voxel - always for
// the diffuse lobe, for the specular one when its cone (Integration.md:
// 2 t sqrt(a^2 / 2 (1 - a^2)), a = roughness^2) covers it.
bool RAB_SharcTerminates(RAB_Surface from, RTXDI_BrdfRaySample brs, float hitT, float3 hitPosition)
{
    const float voxel = RAB_SharcVoxelSize(hitPosition);
    if (!(hitT > 1.7320508 * voxel)) return false;
    if (brs.properties.IsDiffuse() || (g_PT.ptFlags & kRabPTDiffuse)) return true;
    if (g_PT.sharcFlags & kRabSharcDiffuseOnly) return false;
    const float a = from.material.roughness * from.material.roughness;
    const float a2 = min(a * a, 0.999);
    return 2 * hitT * sqrt(0.5 * a2 / (1 - a2)) > voxel;
}

// Whether a hit's outgoing radiance is close enough to view-independent for
// the cache (which stores one value per voxel, whatever the direction): rough
// and not metal. A glossy or metal vertex is traced on - its next-event light
// exact - and the path ends at a later one.
bool RAB_SharcDiffuseLike(RAB_Surface hit)
{
    // Glass never is: what leaves it is mostly what crosses it, from behind.
    return !hit.material.glass && hit.material.roughness >= g_PT.sharcMinRoughness && hit.material.metal < 0.5;
}

// The cached outgoing radiance at a hit, with the hit's own emission.
bool RAB_SharcQuery(RAB_Surface hit, float3 emission, out float3 radiance)
{
    SharcHitData h;
    h.positionWorld = hit.worldPos;
    h.normalWorld = hit.geoNormal;
    h.emissive = emission;
    radiance = 0;
    return SharcGetCachedRadiance(RAB_SharcParameters(), h, radiance, false) && all(isfinite(radiance));
}
#endif
