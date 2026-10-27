// DDGIRayAllocation.cs.hlsl - packs per-probe ray descriptors into a flat list.
// One thread per probe: reads its bucket count from g_RayCount, atomically claims a
// slice of g_RayAlloc[1..] and writes its (probeIdx | rayIdx << 20) descriptors there,
// then appends the probe to the active-probe list (see DDGICommon.hlsli).
// After this CS:
//   g_RayAlloc[0]   = total rays this frame
//   g_RayAlloc[1..] = flat array of packed (probe, ray) descriptors
// The dispatch args live in the separate buffer DDGIFinalizeIndirect.cs writes.

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU>  g_Vol      : register(b0, space0);
RWStructuredBuffer<uint>       g_RayCount : register(u2, space0);
RWStructuredBuffer<uint>       g_RayAlloc : register(u3, space0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint probeCount = g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ;
    const uint probeIdx   = DTid.x;

    // Do not add an early return: the compaction below uses wave intrinsics, and a
    // returned lane leaves the wave's active mask, silently changing what
    // WaveActiveCountBits / WavePrefixCountBits count. Everything is predicated instead.
    const bool inRange = (probeIdx < probeCount);

    uint rayCount = 0;
    if (inRange)
        rayCount = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;

    // Capacity of the descriptor list: totalProbes * raysPerProbe. A probe whose slice
    // would overflow it is not allocated (cannot happen while each probe's count is
    // <= raysPerProbe, but writing past it would corrupt memory).
    const uint capacity = probeCount * g_Vol.raysPerProbe;

    uint base      = 0;
    bool allocated = false;
    if (rayCount != 0)
    {
        InterlockedAdd(g_RayAlloc[0], rayCount, base);
        allocated = (base + rayCount <= capacity);
    }

    // ---- Compact this probe into the active-probe list ----
    // Gated on `allocated`, after the capacity check: a probe that lost the capacity race
    // has a non-zero ray count but no rays in g_RayAlloc, so the per-probe kernels must not
    // visit it; it keeps last frame's value.
    // One atomic per wave, lanes laid out by prefix count, not one atomic per lane: per-lane
    // atomics retire in random order, and the per-probe kernels then walk the atlas in
    // random tile order (DDGIBorderUpdate ~4x slower). Threads map to consecutive probeIdx,
    // so each wave's probes stay contiguous and ascending on wave32 and wave64 hardware.
    const uint waveActive = WaveActiveCountBits(allocated);
    uint waveBase = 0;
    if (WaveIsFirstLane() && waveActive != 0u)
        InterlockedAdd(g_RayCount[DDGI_ActiveCountSlot(g_Vol)], waveActive, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);

    if (allocated)
    {
        const uint slot = waveBase + WavePrefixCountBits(allocated);
        g_RayCount[DDGI_ActiveListSlot(g_Vol, slot)] = probeIdx;
    }

    // probeIdx fits in 20 bits (<= 1M probes); rayIdx in the upper 12 bits (<= 4096 rays
    // per probe), both well past the engine's limits.
    if (allocated)
    {
        [loop] for (uint r = 0; r < rayCount; ++r)
        {
            g_RayAlloc[1 + base + r] = (probeIdx & 0xFFFFFu) | (r << 20u);
        }
    }
}
