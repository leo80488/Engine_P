// Terrain pixel shader: PBR per layer (albedo / normal / ARM) with displacement-driven height
// blending between layers. The geometric normal is rebuilt per pixel from the bicubic heightmap
// (Terrain.hlsli), so shading is smooth inside triangles and a quantised heightmap shows no terraces.

// Debug: set to 1 to replace albedo with a flat colour per layer (0=R, 1=G, 2=B, 3=Y, >=4 white)
// mixed by the final blend weights; normal and ARM are still sampled.
#define TERRAIN_DEBUG_SPLAT 0

#include "Terrain.hlsli"
// Per layer the PS samples:
//   albedo -> blended into the albedo RT
//   normal -> DX-style tangent space, to world through the per-pixel TBN, blended
//   ARM    -> AO (R), roughness (G), metalness (B); written to the surface RT as
//             (roughness, metalness, AO, reflectance)
//   disp   -> one-channel height that biases the blend weights, so layer transitions follow the
//             micro-relief instead of a flat lerp ("height-correlated splat")

// The terrain constant buffer (b2): shared/terrain_params.hlsli is the one definition C++ fills too.
// Per-layer material data lives in the StructuredBuffer below, so adding a layer is a data edit.
#include "shared/terrain_params.hlsli"
ConstantBuffer<TerrainParams> g_terrain : register(b2, space0);

// One element per terrain layer; the PS loops [0, g_terrain.layerCount). Layout MUST match
// RendererDetail::TerrainLayerGPU (48 bytes). With a splatmap the first 4 layers take its RGBA;
// without one each layer is gated by its world-meter height range and degree slope range, both
// smoothstep-faded.
struct TerrainLayerGPU
{
    int   albedoIdx;
    int   normalIdx;
    int   armIdx;
    float tilingScale;
    float minHeight;
    float maxHeight;
    float fadeHeight;
    float minSlopeDeg;
    float maxSlopeDeg;
    float fadeSlopeDeg;
    int   dispIdx;        // displacement map for height-correlated blend (-1 = none)
    float _pad1;
};
#define MAX_TERRAIN_LAYERS 8

// The heightmap SRV (t2 space0) is visible to all stages, so the PS re-samples it for the
// per-pixel normal.
Texture2D<float>  g_HeightMap    : register(t2, space0);
Texture2D<float4> g_Splatmap     : register(t3, space0);
StructuredBuffer<TerrainLayerGPU> g_TerrainLayers : register(t4, space0);
Texture2D         g_AllTextures[]: register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearClamp SmpFixed(SMP_LINEAR_CLAMP)
#define g_LinearWrap SmpDynamic()   // TerrainPass's terrain material sampler (anisotropy + the milder terrain mip bias)

struct PSIn
{
    float4 sv        : SV_Position;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    // No interpolated tangent frame: ComputeTerrainTBN derives one per pixel.
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
};

// The six MRTs: core/gbuffer_codec.hlsli's GBufferOut, through GBufferEncodeNoMaterial.
#include "core/gbuffer_codec.hlsli"

// ==== Per-pixel geometric normal ====
// Re-derives the heightmap surface frame at the pixel's world XZ: a per-vertex normal interpolated
// across steep triangles shows faceted creases at the edges.
float HeightAt(float2 hmUV)
{
    if (g_terrain.hasHeightmap == 0) return g_terrain.worldCenterY;
    // Bicubic, matching the MS exactly, so the normal agrees with the rasterized geometry.
    // h in [0,1] -> Y in [worldCenterY, worldCenterY + heightScale].
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, hmUV, g_terrain.heightmapTexel);
    return g_terrain.worldCenterY + h * g_terrain.heightScale;
}

void ComputeTerrainTBN(float3 worldPos, out float3 N, out float3 T, out float3 B)
{
    if (g_terrain.hasHeightmap == 0)
    {
        N = float3(0, 1, 0);
        T = float3(1, 0, 0);
        B = float3(0, 0, 1);
        return;
    }

    float2 globalNorm = (worldPos.xz - g_terrain.worldOrigin) / max(g_terrain.worldSize, 1e-3);
    float2 hmUV       = g_terrain.heightmapUVOffset + globalNorm * g_terrain.heightmapUVScale;

    // A 4-texel stencil spans a BC7 4x4 block, so pixels on either side of a block boundary get similar
    // gradients; a narrower one jumps every 4 texels and shows dark bands where the slope crosses a
    // block boundary diagonally.
    const float kStencilTexels = 4.0;
    float texelU    = kStencilTexels * g_terrain.heightmapTexel * g_terrain.heightmapUVScale.x;
    float texelV    = kStencilTexels * g_terrain.heightmapTexel * g_terrain.heightmapUVScale.y;
    float worldStep = kStencilTexels * g_terrain.heightmapTexel * g_terrain.worldSize;

    float hL = HeightAt(hmUV + float2(-texelU, 0));
    float hR = HeightAt(hmUV + float2( texelU, 0));
    float hD = HeightAt(hmUV + float2(0, -texelV));
    float hU = HeightAt(hmUV + float2(0,  texelV));

    // Surface y = h(x,z): centred differences build the right-handed TBN.
    N = normalize(float3(hL - hR, 2.0 * worldStep, hD - hU));
    T = normalize(float3(2.0 * worldStep, hR - hL, 0.0));
    B = normalize(cross(T, N));     // T x N points along +Z (matches +V) for DX-style normal maps
}

// ==== Per-map samplers ====
// Albedo and normal use the hex-grid stochastic 3-tap (TerrainStochasticTaps), which removes the
// row/column repetition; ARM and disp are low-frequency and take 1 tap.
// All of them take explicit gradients of the CONTINUOUS base uv and use SampleGrad: the stochastic
// tap UVs are piecewise-constant per cell, so implicit derivatives break at every seam
// (TerrainRotateGrad), and every call sits in main()'s divergent layer loop, where implicit
// derivatives are undefined. SampleGrad costs the same as Sample once the gradients are in registers.
float3 SampleAlbedo(int idx, float2 uv, float2 dxUV, float2 dyUV)
{
    if (idx < 0) return 0.0;
    float2 uv0, uv1, uv2;
    float  w0,  w1,  w2;
    float2 r0, r1, r2;          // rotation cos/sin per tap; also rotates the gradients
    TerrainStochasticTaps(uv, uv0, uv1, uv2, w0, w1, w2, r0, r1, r2);
    return g_AllTextures[idx].SampleGrad(g_LinearWrap, uv0,
               TerrainRotateGrad(r0, dxUV), TerrainRotateGrad(r0, dyUV)).rgb * w0
         + g_AllTextures[idx].SampleGrad(g_LinearWrap, uv1,
               TerrainRotateGrad(r1, dxUV), TerrainRotateGrad(r1, dyUV)).rgb * w1
         + g_AllTextures[idx].SampleGrad(g_LinearWrap, uv2,
               TerrainRotateGrad(r2, dxUV), TerrainRotateGrad(r2, dyUV)).rgb * w2;
}

// DX-style (Y-up) normal map: returns a tangent-space normal in [-1,1], or (0,0,1) for a missing
// map so the macro normal passes through. Each tap's UV was rotated by its rotN, so its sampled
// normal is in a rotated tangent frame: back-rotate (X, Y) by -rotN before blending; Z is invariant.
float3 SampleNormalTS(int idx, float2 uv, float2 dxUV, float2 dyUV)
{
    if (idx < 0) return float3(0, 0, 1);
    float2 uv0, uv1, uv2;
    float  w0,  w1,  w2;
    float2 r0, r1, r2;
    TerrainStochasticTaps(uv, uv0, uv1, uv2, w0, w1, w2, r0, r1, r2);

    // Same rotation for the gradient as for the tap UV it belongs to.
    float3 n0 = g_AllTextures[idx].SampleGrad(g_LinearWrap, uv0,
                    TerrainRotateGrad(r0, dxUV), TerrainRotateGrad(r0, dyUV)).rgb * 2.0 - 1.0;
    float3 n1 = g_AllTextures[idx].SampleGrad(g_LinearWrap, uv1,
                    TerrainRotateGrad(r1, dxUV), TerrainRotateGrad(r1, dyUV)).rgb * 2.0 - 1.0;
    float3 n2 = g_AllTextures[idx].SampleGrad(g_LinearWrap, uv2,
                    TerrainRotateGrad(r2, dxUV), TerrainRotateGrad(r2, dyUV)).rgb * 2.0 - 1.0;

    // Back-rotate by -angle: (cos -sin; sin cos)^-1 = (cos sin; -sin cos)
    n0.xy = float2( r0.x * n0.x + r0.y * n0.y,
                   -r0.y * n0.x + r0.x * n0.y);
    n1.xy = float2( r1.x * n1.x + r1.y * n1.y,
                   -r1.y * n1.x + r1.x * n1.y);
    n2.xy = float2( r2.x * n2.x + r2.y * n2.y,
                   -r2.y * n2.x + r2.x * n2.y);

    // Linear weighted sum; the caller normalizes after blending all layers.
    return n0 * w0 + n1 * w1 + n2 * w2;
}

// ARM: R = AO, G = roughness, B = metalness; the defaults are a matte dielectric. 1 tap, so the
// gradient is the un-rotated base one: the mip implicit derivatives would pick in uniform control
// flow, made explicit because the call sits in the divergent layer loop.
float3 SampleARM(int idx, float2 uv, float2 dxUV, float2 dyUV)
{
    if (idx < 0) return float3(1.0, 0.85, 0.0);
    return g_AllTextures[idx].SampleGrad(g_LinearWrap, uv, dxUV, dyUV).rgb;
}

// One-channel displacement for height-correlated blending; 1 tap; a missing map -> 0 (no bias).
float SampleDisp(int idx, float2 uv, float2 dxUV, float2 dyUV)
{
    if (idx < 0) return 0.0;
    return g_AllTextures[idx].SampleGrad(g_LinearWrap, uv, dxUV, dyUV).r;
}

// ==== Per-layer auto-blend weight ====
// Layer i is fully visible where worldY is in [minHeight, maxHeight] AND the slope (0 = flat,
// 90 deg = cliff) is in [minSlope, maxSlope]; the two gates multiply and each end smoothstep-fades
// over its fade width.
//
//   minHeight     maxHeight
//        v            v
//   +----+------------+----+
//   |fade|  weight=1  |fade|
//   +----+------------+----+
float AutoBlendWeight(TerrainLayerGPU L, float worldY, float slopeDeg)
{
    const float hFade = max(L.fadeHeight, 1e-3);
    const float hLo = smoothstep(L.minHeight - hFade, L.minHeight, worldY);
    const float hHi = 1.0 - smoothstep(L.maxHeight, L.maxHeight + hFade, worldY);
    const float wH  = saturate(hLo * hHi);

    const float sFade = max(L.fadeSlopeDeg, 1e-3);
    const float sLo = smoothstep(L.minSlopeDeg - sFade, L.minSlopeDeg, slopeDeg);
    const float sHi = 1.0 - smoothstep(L.maxSlopeDeg, L.maxSlopeDeg + sFade, slopeDeg);
    const float wS  = saturate(sLo * sHi);

    return wH * wS;
}

GBufferOut main(PSIn i)
{
    // Texture gradients, taken ONCE in uniform control flow. Every layer samples at
    // worldXZ * L.tilingScale and each stochastic tap rotates and offsets that, so only the interpolated
    // world XZ is differentiated; each layer scales it, each tap rotates it (TerrainRotateGrad).
    // Keep this above the fallback early-return and both layer loops: ddx/ddy inside them is divergent
    // (undefined).
    const float2 dxWorldXZ = ddx(i.worldPos.xz);
    const float2 dyWorldXZ = ddy(i.worldPos.xz);

    // Per-pixel geometric TBN from the heightmap.
    float3 N_geom, T_geom, B_geom;
    ComputeTerrainTBN(i.worldPos, N_geom, T_geom, B_geom);

    // Which layers exist, and the blend source.
    const uint n = min(g_terrain.layerCount, (uint)MAX_TERRAIN_LAYERS);

    bool anyLayer = false;
    for (uint a = 0; a < n; ++a)
        if (g_TerrainLayers[a].albedoIdx >= 0) { anyLayer = true; break; }

    // Splatmap (authored weights) covers only the first 4 layers (RGBA). Without one each layer's
    // weight comes from its height/slope gate.
    float4 splat4 = 0;
    if (g_terrain.hasSplatmap != 0)
        splat4 = g_Splatmap.Sample(g_LinearClamp, i.uv);

    if (!(g_terrain.hasSplatmap != 0 || anyLayer))
    {
        // Fallback when there is neither a splatmap nor a layer: slope-debug palette.
        float slope = saturate(1.0 - N_geom.y);
        float3 grass = float3(0.30, 0.46, 0.22);
        float3 rock  = float3(0.45, 0.42, 0.40);
        // Raw NDC delta, as GBuffer.ps; decoders apply (0.5,-0.5).
        float2 cur  = i.curClip.xy  / i.curClip.w;
        float2 prev = i.prevClip.xy / i.prevClip.w;
        return GBufferEncodeNoMaterial(lerp(grass, rock, smoothstep(0.30, 0.65, slope)), N_geom,
                                       0.85, 0.0, 1.0, cur - prev);
    }

    // Per-layer blend. Pass 1 builds each layer's base weight (splatmap RGBA or height/slope gate).
    // With g_terrain.heightBlendEnable the disp map biases it (v = w + disp * strength) and only layers
    // within g_terrain.heightBlendRange of the local maximum survive, so transitions follow the
    // micro-relief. Pass 2 samples and accumulates with the final weights: sum(sample * w) / sum(w),
    // divided once at the end (the normal sum is normalized instead).
    const float  worldY   = i.worldPos.y;
    // Slope from the per-pixel geometric normal: 0 (flat) to 90 (cliff).
    const float  slopeDeg = degrees(acos(saturate(N_geom.y)));
    const float2 worldXZ  = i.worldPos.xz;

    float baseW[MAX_TERRAIN_LAYERS];
    float vBias[MAX_TERRAIN_LAYERS];
    float vMax = -1e9;

    [loop] for (uint li = 0; li < n; ++li)
    {
        TerrainLayerGPU L = g_TerrainLayers[li];

        float w = (g_terrain.hasSplatmap != 0) ? ((li < 4) ? splat4[li] : 0.0)
                                       : AutoBlendWeight(L, worldY, slopeDeg);
        // Mask disabled layers (no albedo bound) so the blend stays normalised.
        w *= (L.albedoIdx >= 0) ? 1.0 : 0.0;
        baseW[li] = w;

        // Displacement bias for active layers only: disp must never lift a layer the splat / gate zeroed.
        // Inactive layers sit far below the max so they never survive the height-blend cutoff.
        float v = -1e9;
        if (w > 0.0)
        {
            v = w;
            if (g_terrain.heightBlendEnable != 0)
            {
                float disp = SampleDisp(L.dispIdx, worldXZ * L.tilingScale,
                                        dxWorldXZ * L.tilingScale,
                                        dyWorldXZ * L.tilingScale);
                v = w + disp * g_terrain.heightBlendStrength;
            }
            vMax = max(vMax, v);
        }
        vBias[li] = v;
    }

    float3 albedoAccum = 0;
    float3 nTSAccum    = 0;
    float3 armAccum    = 0;
    float  wSum        = 0;

    [loop] for (uint li = 0; li < n; ++li)
    {
        if (baseW[li] <= 0.0) continue;

        // Height-correlated cutoff vs plain linear weight.
        float w = (g_terrain.heightBlendEnable != 0)
                ? max(0.0, vBias[li] - vMax + g_terrain.heightBlendRange)
                : baseW[li];
        if (w <= 0.0) continue;

        TerrainLayerGPU L = g_TerrainLayers[li];
        float2 uv   = worldXZ   * L.tilingScale;
        float2 dxUV = dxWorldXZ * L.tilingScale;   // gradients of the CONTINUOUS uv
        float2 dyUV = dyWorldXZ * L.tilingScale;
        wSum += w;
#if TERRAIN_DEBUG_SPLAT
        // Debug: one flat colour per layer (0=R, 1=G, 2=B, 3=Y, >=4 white).
        float3 dbg = (li == 0) ? float3(1, 0, 0)
                   : (li == 1) ? float3(0, 1, 0)
                   : (li == 2) ? float3(0, 0, 1)
                   : (li == 3) ? float3(1, 1, 0)
                               : float3(1, 1, 1);
        albedoAccum += dbg * w;
#else
        albedoAccum += SampleAlbedo(L.albedoIdx, uv, dxUV, dyUV) * w;
#endif
        nTSAccum += SampleNormalTS(L.normalIdx, uv, dxUV, dyUV) * w;
        armAccum += SampleARM(L.armIdx, uv, dxUV, dyUV) * w;
    }

    const float invW = (wSum > 1e-4) ? rcp(wSum) : 0.0;

    // GBuffer output; the TBN is the per-pixel one from ComputeTerrainTBN.
    float3 nTS     = (wSum > 1e-4) ? normalize(nTSAccum) : float3(0, 0, 1);
    float3 N_world = normalize(nTS.x * T_geom + nTS.y * B_geom + nTS.z * N_geom);

    // ARM = AO (R), roughness (G), metalness (B).
    float3 arm = armAccum * invW;

    // Raw NDC delta, as GBuffer.ps; decoders apply (0.5,-0.5).
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;

    // Terrain is material 0 (no per-material data).
    return GBufferEncodeNoMaterial(albedoAccum * invW, N_world, arm.g, arm.b, arm.r, cur - prev);
}
