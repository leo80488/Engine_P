// Denoiser I/O for the path-traced glass reflection under DLSS-RR
// (PathTracePass::RecordFrontReflection).
//
// Under RR the image is one sample per frame and RR has no guides for the translucent layer
// TransparentPass composites over it, so the FrontReflection entry's per-frame sample (rgb
// radiance, a = first-segment hit distance) is denoised on the pane's own surface before the
// forward fold reads it - the UE Lumen front-layer shape: REBLUR_SPECULAR on the
// TranslucentFront_* stamp.
//
//   Pack     stamp + sample -> NRD inputs (REBLUR_SPECULAR, path-traced kind):
//            normal/roughness R10G10B10A2, viewZ R32F (no pane -> beyond the
//            denoising range), MV RG16F (UV delta), radiance + normalized hit
//            distance RGBA16F
//   Resolve  NRD output -> the front tier's texture: rgb = pre-Fresnel radiance,
//            a = 1 where a pane was stamped (0 elsewhere), which
//            custom_surface.hlsli's MS_FetchFrontLayerReflection folds
//
// PathTracePass's root signature (b0 + a directly indexed heap); every resource is opened
// from the indices below.

#define NRD_NORMAL_ENCODING    2   // R10_G10_B10_A2_UNORM (NrdDenoiser::Resize checks the build)
#define NRD_ROUGHNESS_ENCODING 1   // linear roughness
#include "NRD.hlsli"

cbuffer FrontNrdParams : register(b0)
{
    uint  width, height, depthIdx, normalIdx;
    uint  surfaceIdx, velocityIdx, noisyIdx, normalRoughUav;
    uint  viewZUav, mvUav, specInUav, specOutIdx;
    uint  frontUav; float nearZ, farZ, hitDistA;
};

// Reverse-Z NDC -> positive view distance for the finite perspective the
// renderer builds (XMMatrixPerspectiveFovLH(fov, aspect, farZ, nearZ)).
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float FrontLinearizeReverseZ(float z)
{
    return LinearizeReverseZ(z, nearZ, farZ);
}

[numthreads(8,8,1)]
void Pack(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    Texture2D<float>    depthTex   = ResourceDescriptorHeap[depthIdx];
    RWTexture2D<float4> outNormal  = ResourceDescriptorHeap[normalRoughUav];
    RWTexture2D<float>  outViewZ   = ResourceDescriptorHeap[viewZUav];
    RWTexture2D<float2> outMV      = ResourceDescriptorHeap[mvUav];
    RWTexture2D<float4> outSpec    = ResourceDescriptorHeap[specInUav];
    const float depth = depthTex.Load(int3(px, 0));
    if (depth <= 0.0)
    {
        // No pane: beyond CommonSettings::denoisingRange, which NRD skips.
        outNormal[px] = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 0, 1), 1.0, 0.0);
        outViewZ[px]  = 1e7;
        outMV[px]     = float2(0, 0);
        outSpec[px]   = float4(0, 0, 0, 0);
        return;
    }
    Texture2D<float4> normalTex   = ResourceDescriptorHeap[normalIdx];
    Texture2D<float4> surfaceTex  = ResourceDescriptorHeap[surfaceIdx];
    Texture2D<float2> velocityTex = ResourceDescriptorHeap[velocityIdx];
    Texture2D<float4> noisyTex    = ResourceDescriptorHeap[noisyIdx];
    const float3 N = normalize(normalTex.Load(int3(px, 0)).xyz * 2.0 - 1.0);
    // The trace's own roughness floor (PathTrace.cs.hlsl FrontReflection).
    const float rough = max(surfaceTex.Load(int3(px, 0)).r, 0.045);
    outNormal[px] = NRD_FrontEnd_PackNormalAndRoughness(N, rough, 0.0);
    const float viewZ = FrontLinearizeReverseZ(depth);
    outViewZ[px] = viewZ;
    // The stamp's velocity is the engine's NDC delta (cur - prev), as the
    // GBuffer's: NRD 2D mode wants prevUV - uv.
    const float2 ndcDelta = velocityTex.Load(int3(px, 0));
    outMV[px] = float2(-0.5 * ndcDelta.x, 0.5 * ndcDelta.y);
    const float4 noisy = noisyTex.Load(int3(px, 0));
    const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(noisy.a, viewZ,
                                  float3(hitDistA, 0.1, 20.0), rough);
    outSpec[px] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(noisy.rgb, normHitDist, true);
}

[numthreads(8,8,1)]
void Resolve(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    Texture2D<float>    depthTex = ResourceDescriptorHeap[depthIdx];
    RWTexture2D<float4> front    = ResourceDescriptorHeap[frontUav];
    if (depthTex.Load(int3(px, 0)) <= 0.0) { front[px] = float4(0, 0, 0, 0); return; }
    Texture2D<float4> specOut = ResourceDescriptorHeap[specOutIdx];
    const float3 radiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specOut.Load(int3(px, 0))).rgb;
    front[px] = float4(all(isfinite(radiance)) ? max(radiance, 0.0) : float3(0, 0, 0), 1.0);
}
