// FroxelTemporal.cs.hlsl - temporal reprojection of the UN-integrated froxel grid
// against the previous frame's history. Smooths flicker from low-sample-count light
// injection and amortises quality across frames.
//
//   in : LightingSRV (this frame)     - .rgb = sigma_s x L_in, .a = sigma_t
//        HistorySRV  (previous frame)
//   out: HistoryUAV  (next frame)     - alpha-blended with a neighbourhood clamp
//
// Runs BETWEEN LightInject and Scatter, never after Scatter. A scattered voxel holds
// the integral from the camera to that voxel, a property of the whole ray, so the
// same world position carries a different value once the camera translates and
// reprojecting it is wrong by construction. (sigma_s x L_in, sigma_t) IS a local
// medium property and reprojects exactly; Scatter then integrates the temporally
// stable volume.
//
// VolumetricFogPass's per-view History ping-pongs two textures (tex[2]), so no
// dispatch reads and writes the same resource.

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>   LightingSRV   : register(t0, space2);
Texture3D<float4>   HistorySRV    : register(t1, space2);
RWTexture3D<float4> HistoryUAV    : register(u0, space2);
SamplerState        LinClamp      : register(s0, space2);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;

    float4 current = LightingSRV[DTid];

    // ---- Reproject to the previous frame ----
    // This voxel's world position through the previous frame's view-projection gives
    // its place in the prior history grid. Z must go through the SAME log mapping as
    // the froxel grid: a linear-Z reprojection pulls history from the wrong slice and
    // shows as dark stripes under camera rotation.
    float3 worldPos = FroxelToWorld(DTid, P);
    float4 prevClip = mul(float4(worldPos, 1.0), P.prevViewProj);
    if (prevClip.w <= 0.0)
    {
        HistoryUAV[DTid] = current;
        return;
    }
    float2 prevUV = float2( prevClip.x / prevClip.w * 0.5 + 0.5,
                           -prevClip.y / prevClip.w * 0.5 + 0.5);

    // The grid is radial: the log-distributed slice of the distance to last frame's camera, as
    // [0,1] UVW.z. Planar depth would read history from a slice short of the voxel off the view
    // axis (3.6 slices at 30 degrees over 0.5 - 80 m), past the disocclusion test below at the
    // screen's edges.
    float prevSliceF = ViewDepthToFroxelSlice(
        length(worldPos - P.prevCameraPos), P.froxelNear, P.froxelFar, P.froxelD);
    float  prevW   = prevSliceF / float(P.froxelD);
    float3 prevUVW = float3(prevUV, prevW);

    // Out of bounds (camera moved sideways, voxel left the frustum) -> no history.
    if (any(prevUVW < 0.0) || any(prevUVW > 1.0))
    {
        HistoryUAV[DTid] = current;
        return;
    }

    // ---- Depth disocclusion ----
    // Reject history only for a large Z jump (> 8 slices, ~6% of a 128-slice grid):
    // that catches real disocclusion (an object stepping in front) but forgives the
    // few slices of drift that camera motion and cascade crossings produce, which
    // should be smoothed across.
    float currSliceIdx = float(DTid.z) + 0.5;
    if (abs(prevSliceF - currSliceIdx) > 8.0)
    {
        HistoryUAV[DTid] = current;
        return;
    }

    float4 history = HistorySRV.SampleLevel(LinClamp, prevUVW, 0);

#if FROXEL_RT
    // ---- Variance clamp, exponential average ----
    // One ray-traced sample per froxel and frame: a min / max box is as noisy as the sample and
    // would discard the accumulation, so the history is clamped to the 3x3x3 neighbourhood's
    // mean +- 1.25 standard deviations and the frame enters with rtTemporalAlpha (1 on a cut, a
    // teleport or without a history: P.temporalAlpha).
    float4 m1 = 0, m2 = 0;
    int3 dimRt = int3(P.froxelW, P.froxelH, P.froxelD) - 1;
    [unroll] for (int dz = -1; dz <= 1; ++dz)
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        const float4 n = LightingSRV[clamp(int3(DTid) + int3(dx, dy, dz), int3(0, 0, 0), dimRt)];
        m1 += n;
        m2 += n * n;
    }
    m1 /= 27.0;
    const float4 sigma = sqrt(max(m2 / 27.0 - m1 * m1, 0.0));
    history = clamp(history, m1 - 1.25 * sigma, m1 + 1.25 * sigma);
    HistoryUAV[DTid] = lerp(history, current, P.temporalAlpha >= 1.0 ? 1.0 : P.rtTemporalAlpha);
#else
    // ---- Neighbourhood clamp (anti-ghosting) ----
    // 6-tap axis cross (+-x, +-y, +-z) min/max box around the current voxel, with no
    // expansion: history drifting slightly outside the local range fossilises the
    // "dark stripe" artifact when a reprojection is a fraction of a slice off. A
    // 27-tap box is not needed: (sigma_s x L_in, sigma_t) is a local medium property
    // whose gradients the axis neighbours already resolve, so the cross is as tight at
    // under a quarter of the fetches.
    float4 nMin = current, nMax = current;
    int3 dim = int3(P.froxelW, P.froxelH, P.froxelD) - 1;
    static const int3 kTaps[6] = {
        int3(-1,  0,  0), int3( 1,  0,  0),
        int3( 0, -1,  0), int3( 0,  1,  0),
        int3( 0,  0, -1), int3( 0,  0,  1)
    };
    [unroll] for (int t = 0; t < 6; ++t)
    {
        int3   nc = clamp(int3(DTid) + kTaps[t], int3(0,0,0), dim);
        float4 n  = LightingSRV[nc];
        nMin = min(nMin, n);
        nMax = max(nMax, n);
    }
    history = clamp(history, nMin, nMax);

    // ---- Blend ----
    // Small temporalAlpha (0.05) favours history: very smooth but slow to react.
    // Larger (0.5) reacts faster but flickers more.
    float4 result = lerp(history, current, P.temporalAlpha);
    HistoryUAV[DTid] = result;
#endif
}
