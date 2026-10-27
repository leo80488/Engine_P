// The ENGINE DEFAULT look for a Water material: what draws when the shading model is
// Water (Unreal-style Single Layer Water, a translucent Surface material, not a domain)
// and no Material Shader is assigned. A custom shader (asset/Shaders/water_surface.hlsl
// is the shipped starting point) replaces it through dp.customPSID, like GBuffer.ps /
// Transparent.ps. It reads the ordinary material fields, no b8 cbuffer: without a custom
// shader there are no reflected parameters.
// The ordinary MATERIAL_TEXTURE_SLOTS indices are repurposed; the Inspector relabels them
// for Water and hides the rest. Keep this list in sync with kWaterSlotNames in
// MaterialPropertyPanel.cpp: no compiler check ties the two.
//   0 BASECOLORMAP  hidden           (the Base Color SWATCH is the deep colour)
//   1 NORMALMAP  -> Flow Normal A    (broad swell)
//   2 SURFACEMAP -> Flow Normal B    (fine detail, counter-scrolling)
//   3 EMISSIVEMAP   hidden
//   4 RAMPMAP    -> Depth ramp       (U = shore..deep; replaces the computed gradient)
//   5 OPACITYMAP -> Foam mask        (.r, sampled at the flow UV, near shore)
// Every slot is optional: unbound reads -1 and falls back to calm water / the computed
// gradient / no foam. The permutation selects the 4-MRT water main in
// custom_surface.hlsli; this file only supplies the hook.

#define MATERIAL_HAS_SURFACE
#include "custom_surface.hlsli"

// Fixed defaults for the knobs a custom shader would expose as b8 params; deliberately
// conservative (a starting point, not a look).
static const float kFlowSpeed     = 0.55;
static const float kNormalTiling  = 0.35;
static const float kDetailTiling  = 2.3;
static const float kNormalStrength= 0.45;
static const float kAbsorbDist    = 6.0;
static const float kShoreFade     = 1.2;
static const float kFresnelF0     = 0.02;
static const float kRippleStrength= 3.0;
static const float kFoamDepth     = 0.8;   // metres of depth over which foam fades out
static const float kFoamStrength  = 0.9;
static const float2 kFlowDir      = float2(0.9438, 0.3304);   // normalized (1, 0.35)

void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)
{
    MaterialGPUData mat = g_Materials[p.materialIndex];

    // ---- Flow normal ----
    // World-XZ tiling so the look does not depend on the plane's UVs or size.
    const float2 flow = kFlowDir * (kFlowSpeed * p.time);
    const float2 uvA  = (p.worldPos.xz - flow) * kNormalTiling;
    const float2 uvB  = (p.worldPos.xz + flow * 0.62) * (kNormalTiling * kDetailTiling) + 0.37;

    // Two independent flow normals. material.hlsli #defines MAT_TEX_ macros only for slots
    // 3..5; the slot list at the top of this file is the authority for 0..2. BC5: sample RG,
    // rebuild Z. An unbound slot (-1) adds zero slope (calm water).
    float2 sA = float2(0.0, 0.0);
    float2 sB = float2(0.0, 0.0);
    {
        const int nA = mat.textureHandleIds[1];   // NORMALMAP  -> swell
        const int nB = mat.textureHandleIds[2];   // SURFACEMAP -> detail
        if (nA >= 0) sA = g_AllTextures[nA].Sample(g_LinearWrap, uvA).rg * 2.0 - 1.0;
        // Fall back to the swell map at the detail tiling when B is unassigned,
        // so a single authored normal still produces the two-layer look.
        if (nB >= 0)      sB = g_AllTextures[nB].Sample(g_LinearWrap, uvB).rg * 2.0 - 1.0;
        else if (nA >= 0) sB = g_AllTextures[nA].Sample(g_LinearWrap, uvB).rg * 2.0 - 1.0;
    }
    const float zA = sqrt(saturate(1.0 - dot(sA, sA)));
    const float zB = sqrt(saturate(1.0 - dot(sB, sB)));

    float2 slope = (sA + sB) * kNormalStrength;
    slope += WaterRippleSlope(p, kRippleStrength);
    const float up = max(zA * zB, 0.05);

    s.normalWS    = normalize(mul(normalize(float3(slope.x, up, slope.y)),
                                  p.tangentToWorld));
    s.roughness   = max(mat.roughnessMin, 0.02);
    s.metallic    = 0.0;
    s.ao          = 1.0;
    s.reflectance = kFresnelF0;

    // ---- Depth-driven body colour + shoreline ----
    const float  depth  = WaterSceneDepthBelow(p);
    const float  absorb = 1.0 - exp(-depth / kAbsorbDist);
    // Body colour. An authored RAMP (slot 4) wins: U = 0 at the shoreline, 1 at full depth.
    // Without one, a lighter, greener shallow tint is derived from the Base Color swatch
    // (the deep colour).
    const float3 deep = mat.baseColor.rgb;
    float3 body;
    {
        const int rampT = mat.textureHandleIds[4];   // RAMPMAP -> depth ramp
        if (rampT >= 0)
        {
            body = g_AllTextures[rampT].SampleLevel(
                       g_LinearWrap, float2(saturate(absorb), 0.5), 0).rgb;
        }
        else
        {
            const float3 shallow = saturate(deep * 3.0 + float3(0.04, 0.14, 0.10));
            body = lerp(shallow, deep, absorb);
        }
    }

    const float2 uvScr  = WaterScreenUV(p) + s.normalWS.xz * 0.03;
    const float3 behind = WaterSceneColor(p, uvScr, body);

    // ---- Hand the result to the ENGINE BRDF ----
    // Ordinary PBR hook: the shared forward path derives the sky / probe reflection, the sun
    // glint and the Fresnel falloff from normalWS / roughness / reflectance
    // (water_surface.hlsl explains the split).
    const float transmit = 1.0 - WaterFresnel(p, s, kFresnelF0);
    s.baseColor = body;
    s.emissive  = behind * (1.0 - saturate(absorb)) * transmit;

    // Foam (slot 5, .r): a shoreline band where the water is shallow, scrolling with the
    // flow so it is not painted on. Mixed into baseColor: foam is scattering water, not a
    // light source.
    {
        const int foamT = mat.textureHandleIds[5];   // OPACITYMAP -> foam mask
        if (foamT >= 0)
        {
            const float band = 1.0 - saturate(depth / max(kFoamDepth, 1e-3));
            if (band > 0.0)
            {
                const float m = g_AllTextures[foamT].Sample(g_LinearWrap, uvA).r;
                s.baseColor = lerp(s.baseColor, float3(1, 1, 1),
                                   saturate(m * band * kFoamStrength));
            }
        }
    }

    s.opacity = saturate(depth / kShoreFade);
}
