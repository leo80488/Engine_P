// The DLSS-RR guides of a raster frame: RRGuidePass, when ray reconstruction denoises a
// ReSTIR DI / GI frame - or, as a diagnostic, beside the path tracer (-rr_raster_guides=1),
// whose own guides it must equal (tools/verify_rr_guides.py --raster).
//
// The GBuffer primary's guides with PathTrace.cs.hlsl's formulas (its flags & 8192 block)
// over the same reads: the diffuse albedo baseColor x (1 - metal), the split-sum specular
// albedo F0 x A + B (EnvBRDFApprox), both in [0,1]; the shading normal and roughness; the sky
// and unlit pixels cleared as the path tracer clears them.
//
// kRRRasterModels: the raster lighting's rules (Lighting.ps.hlsl) where they differ from the
// path tracer's GGX - its roughness floor, 0.045 where the path tracer's is 0.01
// (core/roughness_floor.hlsli); a toon pixel authored with reflectance 0 has no PBR specular
// (the NPR switch gates every reflection source); and a clear coat layers its dielectric film
// (F0 0.04, its own roughness from RT4) over the base, attenuating both base lobes by the
// film's Fresnel. The path tracer shades toon and clear coat as plain GGX over its own floor,
// so the diagnostic beside it runs without the rules.
//
// The specular hit distance is energy-weighted over the specular light the raster frame has
// (the path tracer's rule): ReSTIR DI's specular at its light's distance (RestirDIResult
// slice 1, its alpha the distance under ray reconstruction - kRRDiDistance), the sun's,
// unshadowed, at 10000 (the path tracer's miss distance), the traced reflection (Lighting's
// t44: radiance, confidence) at its hit distance (RTReflectionPass's kept copy), and the
// sky's prefiltered radiance for the rest of the lobe at 10000; 0 where none. (RR's diffuse
// hit distance is ReSTIR GI's connection distance, written by RTGI's Resolve and tagged
// directly.)
#define PT_REFERENCE_GGX 1
#include "brdf.hlsli"
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
#include "material.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "core/color.hlsli"   // Luminance
#include "core/roughness_floor.hlsli"   // RASTER_ / RT_ROUGHNESS_FLOOR

cbuffer Params : register(b0)   // root constants: RRGuidePass.cpp's Params
{
    float4x4 invViewProj;   // jittered, transposed for the row-vector mul
    float3 camera; uint width;
    uint height, albedoIdx, normalIdx, surfaceIdx;
    uint depthIdx, materialsIdx, extraIdx, flags;
    uint albedoUav, specularUav, normalUav, distanceUav;
    float3 sunDirection; uint diResultIdx;   // sunDirection: the way the light travels
    float3 sunColor; uint reflectionIdx;     // reflectionIdx: Lighting's t44 (radiance, confidence)
    uint reflectionDistanceIdx, radianceIdx, radianceMips; float iblStrength;
};
SamplerState gLinearClamp : register(s0);   // RRGuidePass's static sampler
static const uint kRRRasterModels = 1u;
static const uint kRRDiDistance = 2u;
static const uint kRRDiffuseOnly = 4u;
static const float kRRMissDistance = 10000;   // PathTrace.cs.hlsl's continuation TMax

float RRLuminance(float3 v) { return Luminance(max(v, 0)); }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    RWTexture2D<float4> da = ResourceDescriptorHeap[albedoUav];
    RWTexture2D<float4> sa = ResourceDescriptorHeap[specularUav];
    RWTexture2D<float4> nr = ResourceDescriptorHeap[normalUav];
    RWTexture2D<float> hd = ResourceDescriptorHeap[distanceUav];
    Texture2D<float> depthTex = ResourceDescriptorHeap[depthIdx];
    const float depth = depthTex.Load(int3(px, 0));
    if (depth <= 0)
    {
        // The sky: PathTrace.cs.hlsl's cleared guides.
        da[px] = 0; sa[px] = 0; nr[px] = float4(0, 0, 1, 1); hd[px] = 0;
        return;
    }
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[albedoIdx];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[normalIdx];
    Texture2D<float4> surfaceTex = ResourceDescriptorHeap[surfaceIdx];
    const float2 uv = (float2(px) + 0.5) / float2(width, height);
    const float3 p = WorldPosFromDepth(uv, depth, invViewProj);
    const float4 normal = normalTex.Load(int3(px, 0));
    const float3 n = normalize(normal.xyz * 2 - 1);
    const float3 v = normalize(camera - p);
    const float3 albedo = albedoTex.Load(int3(px, 0)).rgb;
    uint model = SHADING_MODEL_STANDARD;
    MaterialGPUData material = (MaterialGPUData)0;
    if (materialsIdx != 0)
    {
        StructuredBuffer<MaterialGPUData> materials = ResourceDescriptorHeap[materialsIdx];
        material = materials[GBufferDecodeMatIdx(normal.a)];
        model = material.customShadingModel;
    }
    if (model == SHADING_MODEL_UNLIT)
    {
        // Its base colour, once: no scattering (the path tracer's direct store).
        da[px] = 0; sa[px] = 0; nr[px] = float4(0, 0, 1, 1); hd[px] = 0;
        return;
    }
    const float4 surf = surfaceTex.Load(int3(px, 0));
    const float roughFloor = (flags & kRRRasterModels) ? RASTER_ROUGHNESS_FLOOR : RT_ROUGHNESS_FLOOR;
    const float rough = max(surf.r, roughFloor), metal = surf.g, reflectance = surf.a;
    const float3 f0 = lerp((.16 * reflectance * reflectance).xxx, albedo, metal);
    const float nv = saturate(dot(n, v));
    const float2 ab = EnvBRDFApprox(rough, nv);
    float3 diffuseAlbedo = albedo * (1 - metal);
    float3 specularAlbedo = (flags & kRRDiffuseOnly) ? float3(0, 0, 0) : f0 * ab.x + ab.y;
    if (flags & kRRRasterModels)
    {
        if (model == SHADING_MODEL_TOON && reflectance <= 1e-4) specularAlbedo = 0;
        else if (model == SHADING_MODEL_CLEARCOAT)
        {
            // RT4.r / .g: the coat's strength and roughness; a face pixel
            // (RT4.a marker) keeps the material's (Lighting.ps.hlsl).
            float coat = saturate(material.clearCoat), coatRough = saturate(material.clearCoatRoughness);
            if (extraIdx != 0)
            {
                Texture2D<float4> extraTex = ResourceDescriptorHeap[extraIdx];
                const float4 extra = extraTex.Load(int3(px, 0));
                if (extra.a <= 0.5) { coat = saturate(extra.r); coatRough = saturate(extra.g); }
            }
            if (coat > 0 && !(flags & kRRDiffuseOnly))
            {
                const float coatAtten = 1 - (0.04 + 0.96 * pow(1 - nv, 5.0)) * coat;
                const float2 cab = EnvBRDFApprox(max(coatRough, RASTER_ROUGHNESS_FLOOR), nv);
                specularAlbedo = specularAlbedo * coatAtten + (0.04 * cab.x + cab.y) * coat;
                diffuseAlbedo *= coatAtten;
            }
        }
    }
    // Engine split-sum convention, linear reflectance, no NRD demodulation. Clamp to [0,1]: a
    // base colour above 1 makes DLSS-RR's model D write non-finite texels at 3x.
    specularAlbedo = saturate(specularAlbedo);
    da[px] = float4(saturate(diffuseAlbedo), 1);
    sa[px] = float4(specularAlbedo, 1);
    nr[px] = float4(n, rough);

    // The specular hit distance: energy x distance over the light we know.
    float energy = 0, moment = 0;
    if (any(specularAlbedo > 0))
    {
        if ((flags & kRRDiDistance) && diResultIdx != 0)
        {
            Texture2DArray<float4> di = ResourceDescriptorHeap[diResultIdx];
            const float4 spec = di.Load(int4(px, 1, 0));
            const float e = RRLuminance(spec.rgb);
            if (e > 0 && spec.a > 0) { energy += e; moment += e * spec.a; }
        }
        if (any(sunColor > 0))
        {
            const float3 l = normalize(-sunDirection);
            float3 spec, kd;
            EvalCookTorrance(n, v, l, f0, rough, metal, albedo, spec, kd);
            const float e = RRLuminance(spec * saturate(dot(n, l)) * sunColor);
            energy += e; moment += e * kRRMissDistance;
        }
        float confidence = 0;
        if (reflectionIdx != 0)
        {
            Texture2D<float4> reflection = ResourceDescriptorHeap[reflectionIdx];
            const float4 traced = reflection.Load(int3(px, 0));
            confidence = saturate(traced.a);
            const float e = RRLuminance(traced.rgb * specularAlbedo) * confidence;
            float d = kRRMissDistance;
            if (reflectionDistanceIdx != 0)
            {
                Texture2D<float> hit = ResourceDescriptorHeap[reflectionDistanceIdx];
                const float t = abs(hit.Load(int3(px, 0)));   // a negative sign marks a moving reflector
                if (t > 0) d = t;
            }
            if (e > 0) { energy += e; moment += e * d; }
        }
        if (radianceIdx != 0 && confidence < 1)
        {
            TextureCube<float4> sky = ResourceDescriptorHeap[radianceIdx];
            const float mip = rough * float(max(radianceMips, 1u) - 1u);
            const float3 radiance = sky.SampleLevel(gLinearClamp, reflect(-v, n), mip).rgb * iblStrength;
            const float e = RRLuminance(radiance * specularAlbedo) * (1 - confidence);
            if (e > 0) { energy += e; moment += e * kRRMissDistance; }
        }
    }
    hd[px] = energy > 1e-20 ? moment / energy : 0;
}
