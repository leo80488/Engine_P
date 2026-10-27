// SSRComposite.cs.hlsl - additive composite of the standalone SSR chain.
// Resolved SSR is weighted by Fresnel, envBRDF and confidence, then added to HDR. LightingPass
// already dampens IBL by (1 - ssrConf), so the combined specular term stays energy-correct.

cbuffer SSRCompositeCB : register(b0, space2)
{
    float4x4 invViewProj;
    float3   cameraPos;       float _pad0;
    uint     screenW;         uint  screenH;
    uint     useUnifiedConf;  uint  _pad1;
};

Texture2D<float4> gAlbedo             : register(t0, space2);
Texture2D<float4> gNormal             : register(t1, space2);
Texture2D<float4> gSurface            : register(t2, space2);
Texture2D<float>  gDepth              : register(t3, space2);
Texture2D<float4> gSSR                : register(t4, space2);
Texture2D<float2> gBRDFLUT            : register(t5, space2);
Texture2D<float4> gUnifiedReflection  : register(t7, space2);

RWTexture2D<float4> OutHDR : register(u0, space2);
SamplerState gLinClamp     : register(s0, space2);

#include "indirect_specular.hlsli"

#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float3 ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= screenW || dispatchThreadId.y >= screenH)
        return;

    const int2 pixel = int2(dispatchThreadId.xy);
    const float4 ssr = gSSR.Load(int3(pixel, 0));
    const float confidence = saturate(ssr.a);
    if (confidence <= 1e-4)
        return;

    const float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)
        return;

    const float2 uv = (float2(pixel) + 0.5) / float2(screenW, screenH);
    const float3 worldPos = ReconstructWorldPos(uv, depth);
    const float3 normal = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    const float3 viewDir = normalize(cameraPos - worldPos);
    const float nDotV = saturate(dot(normal, viewDir));

    const float3 albedo = gAlbedo.Load(int3(pixel, 0)).rgb;
    const float4 surface = gSurface.Load(int3(pixel, 0));
    const float roughness = max(surface.r, 0.045);
    const float metallic = surface.g;
    const float reflectance = surface.a;

    const float dielectricF0 = 0.16 * reflectance * reflectance;
    const float3 f0 = lerp(
        float3(dielectricF0, dielectricF0, dielectricF0),
        albedo,
        metallic);
    const float2 envBRDF =
        gBRDFLUT.SampleLevel(gLinClamp, float2(nDotV, roughness), 0);
    float3 reflection =
        IndirectSpecular_ApplyEnvBRDF(ssr.rgb, f0, envBRDF) * confidence;

    // Water frames also run the late standalone SSR chain. Suppress its contribution where the
    // same-frame unified reflection pass already covered the opaque surface.
    if (useUnifiedConf != 0)
    {
        const float unifiedConfidence =
            saturate(gUnifiedReflection.Load(int3(pixel, 0)).a);
        reflection *= 1.0 - unifiedConfidence;
    }

    const float4 hdr = OutHDR[pixel];
    OutHDR[pixel] = float4(hdr.rgb + reflection, hdr.a);
}
