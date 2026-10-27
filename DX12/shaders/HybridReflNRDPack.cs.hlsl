// Full-screen NRD (REBLUR_SPECULAR) input pack. Runs after IntersectSW /
// IntersectHW and before the NRD dispatch chain; turns the GBuffer and the
// intersect outputs into the four textures REBLUR consumes, plus the depth history:
//
//   u9  OutNrdNormalRough  R10G10B10A2  NRD_FrontEnd_PackNormalAndRoughness
//   u10 OutNrdViewZ        R32F         linear view-space Z (sky -> huge)
//   u11 OutNrdMV           RG16F        pixelUvPrev = pixelUv + mv.xy (UV units)
//   u12 OutNrdSpecIn       RGBA16F      REBLUR_FrontEnd_PackRadianceAndNormHitDist
//   u8  OutDepthHistory    R32F         raw NDC depth: the SW tier's
//                                       screen-radiance-reuse depth ping-pong
//                                       (gPrevDepth)
//
// Untraced pixels (sky / rougher than the trace gate) get zero radiance and zero
// hit distance; REBLUR's roughness/normal edge-stopping keeps them out of traced
// neighbours, and the resolve writes conf 0 there anyway.
//
// The encoding defines must match the NRD.lib build (CMake NRD_NORMAL_ENCODING /
// NRD_ROUGHNESS_ENCODING, the 2 / 1 defaults; NrdDenoiser::Resize checks them).

#include "HybridReflCommon.hlsli"

#define NRD_NORMAL_ENCODING    2   // R10_G10_B10_A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1   // linear roughness
#include "NRD.hlsli"

Texture2D<float>    gDepth      : register(t2,  space0);
Texture2D<float4>   gNormal     : register(t3,  space0);
Texture2D<float4>   gSurface    : register(t4,  space0);   // r = roughness
Texture2D<float2>   gVelocity   : register(t5,  space0);   // NDC delta (unjittered)
Texture2D<float4>   gRawColor   : register(t28, space0);   // intersect: rgb radiance
Texture2D<float>    gHitDist    : register(t29, space0);   // intersect: world-space hit t

RWTexture2D<float>  OutDepthHistory   : register(u8,  space0);
RWTexture2D<float4> OutNrdNormalRough : register(u9,  space0);
RWTexture2D<float>  OutNrdViewZ      : register(u10, space0);
RWTexture2D<float2> OutNrdMV         : register(u11, space0);
RWTexture2D<float4> OutNrdSpecIn     : register(u12, space0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const uint2 pixel = DTid.xy;

    const float depth = gDepth.Load(int3(pixel, 0));

    // Depth history for next frame's SW-tier screen-radiance reuse (see header).
    OutDepthHistory[pixel] = depth;

    // Normal + roughness, written even for untraced pixels: REBLUR's edge-stopping
    // reads the full-screen guide.
    const float3 N         = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    const float  roughness = gSurface.Load(int3(pixel, 0)).r;
    OutNrdNormalRough[pixel] = NRD_FrontEnd_PackNormalAndRoughness(N, roughness, 0.0);

    // Linear view Z (reverse-Z NDC -> positive view-space distance). Sky must land
    // beyond CommonSettings::denoisingRange.
    const float viewZ = (depth > 0.0) ? HR_LinearizeReverseZ(depth) : 1e7;
    OutNrdViewZ[pixel] = viewZ;

    // Motion vector: engine velocity = curNDC - prevNDC, so
    // prevUV = uv - (0.5, -0.5) x ndcDelta. NRD (2D mode, scale 1):
    // pixelUvPrev = pixelUv + mv.xy -> mv = prevUV - uv.
    const float2 ndcDelta = gVelocity.Load(int3(pixel, 0));
    const float2 surfaceMV = float2(-0.5 * ndcDelta.x, 0.5 * ndcDelta.y);

    // Specular radiance + normalized hit distance. Intersect contract: rgb = hit
    // shading or sky-cube miss radiance; abs(gHitDist) = world hit t (rayTMax on miss);
    // a negative sign marks an independently moving reflector. Untraced pixels are
    // zeros.
    const float3 radiance  = gRawColor.Load(int3(pixel, 0)).rgb;
    const float  rawHitT   = gHitDist.Load(int3(pixel, 0));
    const bool   dynamicHit = rawHitT < 0.0;
    const float  hitT       = abs(rawHitT);

    // REBLUR only receives the motion of the primary (mirror) surface, so a skinned
    // or otherwise independently moving reflection hit would reproject the old pose
    // and look one or two frames late. The intersect tiers encode such hits as -hitT;
    // an out-of-bounds previous UV is the documented-safe way to reject that temporal
    // sample while keeping the spatial filtering and the real hit distance. Static
    // reflection pixels keep their surface MV.
    OutNrdMV[pixel] = dynamicHit ? float2(2.0, 2.0) : surfaceMV;

    // hitDistanceParameters (A, B, C; the hlsli helper takes the first three). A comes
    // from the CB and scales with rayTMax (A = rayTMax x 0.075, i.e. 3.0 at the shipped
    // rayTMax 40); B and C are the nrd::ReblurSettings defaults.
    // Must equal what NrdDenoiser hands REBLUR as rs.hitDistanceParameters: this shader
    // normalizes with them and REBLUR denormalizes for its blur radius and
    // virtual-motion reprojection. Both come from one value in RTReflectionPass::Execute;
    // do not fork them.
    // Declared inside main(): a `static const` global cannot be initialized from a
    // cbuffer member.
    const float3 kHitDistParams = float3(hitDistParamA, 0.1, 20.0);

    float normHitDist = 0.0;
    if (depth > 0.0 && hitT > 0.0)
        normHitDist = REBLUR_FrontEnd_GetNormHitDist(hitT, viewZ, kHitDistParams, roughness);

    OutNrdSpecIn[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist, true);
}
