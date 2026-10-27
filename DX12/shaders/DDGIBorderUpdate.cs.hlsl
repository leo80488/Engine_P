// Copies interior octahedral edges into the atlas border so bilinear sampling across the
// seams is correct. The edges of the [-1,1]^2 octahedral square meet antipodal points on the
// sphere, so the 1-texel border holds the opposite edge's content, mirrored. Runs per
// (probe, border texel) after Relight updated the interiors.
// Two dispatches (irradiance / depth) share the kernel through DDGIRelight's
// DDGI_RELIGHT_TARGET_* defines: the format differs, the addressing is identical.

#include "DDGICommon.hlsli"

#ifndef DDGI_RELIGHT_TARGET_IRRADIANCE
#define DDGI_RELIGHT_TARGET_IRRADIANCE 1
#endif
#ifndef DDGI_RELIGHT_TARGET_DEPTH
#define DDGI_RELIGHT_TARGET_DEPTH 0
#endif

ConstantBuffer<DDGIVolumeGPU>      g_Vol   : register(b0, space0);
// Per-probe ray count: zero rays means the probe was not relit this frame, so its border is
// already correct. Must be u2 (root param 12, bound once per Execute), not t2: t2 is the ray
// data SRV, still bound from the preceding relight dispatch.
RWStructuredBuffer<uint>           g_RayCount : register(u2, space0);

#if DDGI_RELIGHT_TARGET_IRRADIANCE
RWTexture2D<float4>                g_Atlas : register(u0, space0);
static const uint kProbeSize = DDGI_IRRADIANCE_PROBE_SIZE;
static const uint kProbeStride = DDGI_IRRADIANCE_PROBE_STRIDE;
#else
RWTexture2D<float2>                g_Atlas : register(u0, space0);
static const uint kProbeSize = DDGI_DEPTH_PROBE_SIZE;
static const uint kProbeStride = DDGI_DEPTH_PROBE_STRIDE;
#endif

// Maps a border texel (the +/-1 ring around the interior) to the interior texel it mirrors
// (McGuire 2017 / RTXGI):
//   edge   (one axis outside)  -> mirrored along that edge: (p-1-x) / (p-1-y)
//   corner (both axes outside) -> the diagonally opposite interior corner
// Handle corners first: all four map to direction (0,0,-1), and the edge rules would read
// another border texel (a same-dispatch race) or wrap the uint out of bounds.
uint2 BorderToInterior(int x, int y, uint probeSize)
{
    int p = (int)probeSize;
    const bool outX = (x < 0) || (x >= p);
    const bool outY = (y < 0) || (y >= p);
    if (outX && outY)       return uint2(x < 0 ? p - 1 : 0, y < 0 ? p - 1 : 0);
    if (x < 0)              return uint2(0,                p - 1 - y);
    if (y < 0)              return uint2(p - 1 - x,        0);
    if (x >= p)             return uint2(p - 1,            p - 1 - y);
    if (y >= p)             return uint2(p - 1 - x,        p - 1);
    return uint2(x, y);
}

// The group must cover the full stride x stride tile (8x8 irradiance, 18x18 depth): a smaller
// group leaves the right / bottom border rows unwritten, and bilinear depth taps near them
// blend mean = 0 into the Chebyshev test (probe-cell-shaped blocks in shadow).
#if DDGI_RELIGHT_TARGET_IRRADIANCE
[numthreads(DDGI_IRRADIANCE_PROBE_STRIDE, DDGI_IRRADIANCE_PROBE_STRIDE, 1)]
#else
[numthreads(DDGI_DEPTH_PROBE_STRIDE, DDGI_DEPTH_PROBE_STRIDE, 1)]
#endif
void main(uint3 tid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    // Keep the direct group -> probe mapping; do not drive this kernel over the compacted
    // active-probe list the relight passes use (ExecuteIndirect over it ran ~4x slower). It is the
    // smallest DDGI kernel (a few texel copies per group) and latency-bound, so it needs many groups
    // in flight: a skipped group costs one load and retires, and probeIdx == SV_GroupID.x keeps
    // consecutive groups on consecutive atlas tiles. The ALU-bound relight kernels gain from
    // compaction. Bandwidth is not the limit (~2.8 MB per pass).
    const uint probeIdx = tid.x;
    if (probeIdx >= DDGI_ProbeCountOf(g_Vol))
        return;

    // Zero rays -> Relight skipped this probe (amortization or an inactive volume): its interior
    // is unchanged and the border already mirrors it, so return early.
    if (g_RayCount[probeIdx] == 0u) return;

    // Threads cover the stride x stride tile; only the border ring is written (Relight wrote the
    // interior).
    int x = (int)lid.x; // 0..stride-1
    int y = (int)lid.y;
    if (x >= (int)kProbeStride || y >= (int)kProbeStride) return;

    // The interior is [1..probeSize]; the borders are 0 and probeSize+1.
    int ix = x - 1; // -1, 0..probeSize-1, probeSize
    int iy = y - 1;
    bool isBorder = (ix < 0) || (iy < 0) || (ix >= (int)kProbeSize) || (iy >= (int)kProbeSize);
    if (!isBorder) return;

    uint2 srcLocal = BorderToInterior(ix, iy, kProbeSize);

    // Atlas positions of both ends of the copy.
    uint2 tile = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol, kProbeStride);
    uint2 dstPx = tile + uint2(x, y);
    uint2 srcPx = tile + uint2(1, 1) + srcLocal;

    // The same in both permutations; only g_Atlas's type differs (float4 irradiance vs float2
    // depth moments). A typed UAV load + store: that is why the irradiance atlas is RGBA16F, not
    // R11G11B10_FLOAT - RGBA16F typed UAV loads are required on FL11.0, R11G11B10 needs
    // D3D12_FEATURE_D3D12_OPTIONS::TypedUAVLoadAdditionalFormats.
    g_Atlas[dstPx] = g_Atlas[srcPx];
}
