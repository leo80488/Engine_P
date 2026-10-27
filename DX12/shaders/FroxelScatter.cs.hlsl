// FroxelScatter.cs.hlsl - front-to-back integration along each column's ray, the
// last froxel compute pass, so the apply pass always gets a freshly integrated
// volume. SrcLighting is the (sigma_s x L_in, sigma_t) grid after temporal
// reprojection. Each (x, y) thread walks all `froxelD` slices and accumulates:
//   .rgb = integral of T(s) x L(s) ds     (in-scattered light reaching the camera)
//   .a   = T(zEnd) = exp(-integral sigma_t ds) (transmittance camera -> this slice)
//
// The apply pass samples it trilinearly and composites:
//   final = scene.rgb x T + scattered.rgb

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>     SrcLighting    : register(t0, space2);
RWTexture3D<float4>   DstScattering  : register(u0, space2);

// A cell's piece of length @p len: its transmittance and the weight of its source term (the
// transmittance's integral over the piece). No floor on sigma_t: a column of empty air must pass
// everything, or its fp16 transmittance comes out short of 1 in front of the sky.
void ScatterPiece(float sigmaT, float len, out float T, out float weight)
{
    T = exp(-sigmaT * len);
    weight = sigmaT > 1e-6 ? (1.0 - T) / sigmaT : len;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH) return;

    float3 accum = float3(0, 0, 0);
    float  trans = 1.0;

    // The grid is radial, the lookups planar (VolumetricApply, fog_apply.hlsli read the slice of
    // a pixel's planar view depth): slice k of the output holds the integral to the planar depth
    // that ends slice k, the radial distance D(k + 1) / cos(theta) along this column (theta: its
    // angle to the view axis). The radial cells are walked once; each output takes the committed
    // cells plus the part of the next one it reaches. No XY jitter here: a column's cells must
    // sample the same UVs every frame for the integral to stay monotonic; LightInject's jitter
    // supplies the per-frame variation in the source lighting.
    const float3 dir      = normalize(FroxelToWorld(uint3(DTid.xy, 0u), P) - P.cameraPos);
    const float  cosTheta = max(dot(dir, normalize(P.cameraForward)), 1e-3);
    uint  cell  = 0u;
    float rDone = FroxelSliceToViewDepth(0.0, P.froxelNear, P.froxelFar, P.froxelD);
    [loop] for (uint k = 0; k < P.froxelD; ++k)
    {
        const float target = FroxelSliceToViewDepth(float(k + 1u), P.froxelNear, P.froxelFar, P.froxelD) / cosTheta;
        // Every cell the target passes, whole.
        [loop] while (cell < P.froxelD)
        {
            const float rEnd = FroxelSliceToViewDepth(float(cell + 1u), P.froxelNear, P.froxelFar, P.froxelD);
            if (rEnd > target) break;
            const float4 c = SrcLighting[uint3(DTid.xy, cell)];
            float Ti, wi;
            ScatterPiece(c.a, rEnd - rDone, Ti, wi);
            accum += trans * c.rgb * wi;
            trans *= Ti;
            rDone = rEnd;
            ++cell;
        }
        // The part of the cell it ends in (none past the grid's end).
        float3 outAccum = accum;
        float  outTrans = trans;
        if (cell < P.froxelD && target > rDone)
        {
            const float4 c = SrcLighting[uint3(DTid.xy, cell)];
            float Ti, wi;
            ScatterPiece(c.a, target - rDone, Ti, wi);
            outAccum += trans * c.rgb * wi;
            outTrans *= Ti;
        }
        DstScattering[uint3(DTid.xy, k)] = float4(outAccum, outTrans);
    }
}
