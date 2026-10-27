// Forward water surface: Fresnel sky reflection + flow normals, deliberately light.
// No refraction copy: a Fresnel blend between an absorption-tinted body colour and the
// sky cube (SkyIBLPass atmosphere cube or the static skybox), a Blinn sun glint, and
// GBuffer stamps that let SSRComposite add SSR later. "Flow" = two tangent-space normal
// maps counter-scrolling along g_flowDir (A = broad swell at g_normalTiling, B = fine
// detail at 2.3x tiling against the flow), whiteout-blended; they come from
// WaterComponent::normalMapA/BPath, and a 1x1 flat-normal fallback gives calm water
// while they load.
// Water depth is ANALYTIC (water plane Y - terrain heightmap Y at the pixel's XZ), so no
// scene-depth SRV is needed and the pass can depth-test against the GBuffer depth (the
// engine has no read-only DSV; TracerPass shows the SRV alternative).
// Bindings: b1 PerView (VS), b2 WaterCB, b3 LightCB; space0 t2 terrain heightmap, t3 sky
// cube, t4/t5 flow normal maps A/B, t6 ripple field, t9 shadow cascades; samplers from
// the sampler heap. The prev-frame SSR result has no register: it is opened bindless
// from LightCB.ssrResultIdx.

#define LIGHT_CB_REGISTER b3
#include "light_cb.hlsli"
#include "cloud_shadow.hlsli"   // SampleCloudShadow (bindless via LightCB)
#include "Terrain.hlsli"   // TerrainSampleHeightBicubic - must match Terrain.ms

// CSM shadow sampling, so the sun glint vanishes where terrain occludes the sun.
// shadow.hlsli needs these declared first (its CB fields come from light_cb.hlsli).
// PER_OBJECT_SHADOW: a character that ShadowPass excludes from the cascades while its
// per-object map is active still shadows the water. Free: light_cb.hlsli always carries
// the per-object matrix / params / texel size and bindless perObjShadowIdx, and
// SamplePerObjectShadow returns 1.0 outside its fitted frustum.
#define PER_OBJECT_SHADOW
Texture2DArray<float>  gShadowCascades : register(t9, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#include "bindless.hlsli"   // typed bindless handles
#define gShadowSampler SmpCmp(SMP_CMP_LINEAR_BORDER_WHITE_GE)
#include "shadow.hlsli"

cbuffer WaterCB : register(b2, space0)
{
    float2 g_waterOrigin;
    float  g_waterSize;
    float  g_waterHeight;

    float4 g_deepColor;
    float4 g_shallowColor;

    float2 g_flowDir;
    float  g_flowSpeed;
    float  g_normalTiling;

    float  g_normalStrength;
    float  g_absorbDist;
    float  g_shoreFade;
    float  g_fresnelF0;

    float  g_specPower;
    float  g_reflStrength;
    float  g_waterTime;
    uint   g_gridQuads;

    float2 g_terrainOrigin;
    float  g_terrainSize;
    float  g_terrainBaseY;

    float  g_terrainHeightScale;
    uint   g_hasTerrain;
    float  g_hmTexel;
    float  g_waterRoughness;   // written to GBuffer2.r - drives the SSR cone

    float2 g_hmUVOffset;
    float2 g_hmUVScale;

    // row 9: interactive ripple field
    float  g_rippleNormalStrength;
    float  g_rippleTexel;      // 1 / rippleResolution (gradient stencil step)
    uint   g_hasRipple;        // 0: flat fallback bound, no perturbation
    float  g_ripplePad;

    // row 10: ripple sim window footprint (worldXZ -> field uv)
    float2 g_rippleWinOrigin;
    float  g_rippleWinSize;
    float  g_ripplePad2;
};

Texture2D<float>  g_HeightMap   : register(t2,  space0);
TextureCube       g_SkyCube     : register(t3,  space0);
Texture2D         g_NormalA     : register(t4,  space0);
Texture2D         g_NormalB     : register(t5,  space0);
// Interactive ripple height field, bound at the IBL-irradiance descriptor slot (water
// never samples IBL cubes; WaterPass kRippleRootSlot). .r / .g hold height(t) /
// height(t-1) from the WaterRippleSystem integrator; only .r is read here.
Texture2D<float2> g_RippleField : register(t6,  space0);
// No register for the prev-frame SSR result: it is opened where it is used, from
// BindlessTex2D<float4>(ssrResultIdx) in LightCB (see the SSR dampening block).
#define g_Clamp SmpFixed(SMP_LINEAR_CLAMP)
#define g_Wrap SmpDynamic()   // WaterPass's anisotropic wrap sampler

struct PSIn
{
    float4 sv       : SV_Position;
    float3 worldPos : POSITIONWS;
    float4 curClip  : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
};

struct PSOut
{
    float4 normal   : SV_TARGET0;  // GBuffer1: N*0.5+0.5, a=0 - SSR trace input
    float4 surface  : SV_TARGET1;  // GBuffer2: (rough, metal, AO, 0.5)
    float2 velocity : SV_TARGET2;  // GBuffer3: NDC delta - TAA/SSR temporal
    float4 color    : SV_TARGET3;  // HDR scene color (alpha-blended shoreline)
};

// ==== Flow normals from the two authored normal maps ====
// Map A scrolls WITH the flow at the base tiling (broad swell); map B scrolls AGAINST it
// at 2.3x tiling (fine detail). Whiteout blend in tangent space, then tangent -> world for
// a +Y plane (U -> +X, V -> +Z): world = (ts.x, ts.z, ts.y).

float3 WaterFlowNormal(float2 xz)
{
    float2 flowOff = g_flowDir * (g_waterTime * g_flowSpeed);

    float2 uvA = (xz - flowOff)        * g_normalTiling;
    float2 uvB = (xz + flowOff * 0.62) * (g_normalTiling * 2.3) + 0.37;

    // Sample RG only and rebuild Z: the importer packs normal maps as BC5 (two channels),
    // as in GBuffer.ps / Transparent.ps. RGBA fallbacks work too (rg = 0.5,0.5 -> z = 1).
    float2 sA = g_NormalA.Sample(g_Wrap, uvA).rg * 2.0 - 1.0;
    float2 sB = g_NormalB.Sample(g_Wrap, uvB).rg * 2.0 - 1.0;
    float  zA = sqrt(saturate(1.0 - dot(sA, sA)));
    float  zB = sqrt(saturate(1.0 - dot(sB, sB)));

    // Whiteout blend; g_normalStrength scales the combined slope.
    float2 slope = (sA + sB) * g_normalStrength;
    float  up    = max(zA * zB, 0.05);

    return normalize(float3(slope.x, up, slope.y));
}

float TerrainYAt(float2 xz)
{
    float2 norm = (xz - g_terrainOrigin) / max(g_terrainSize, 1e-3);
    if (g_hasTerrain == 0 ||
        norm.x < 0.0 || norm.x > 1.0 || norm.y < 0.0 || norm.y > 1.0)
        return g_waterHeight - g_absorbDist * 4.0;   // off-tile -> deep
    float2 uv = g_hmUVOffset + norm * g_hmUVScale;
    // Bicubic: must match the surface Terrain.ms.hlsl rasterizes, or the shoreline alpha
    // fades out where the rendered terrain is still underwater (the filters differ by
    // ~secondDifference/6).
    float  h  = TerrainSampleHeightBicubic(g_HeightMap, g_Clamp, uv, g_hmTexel);
    return g_terrainBaseY + h * g_terrainHeightScale;
}

PSOut main(PSIn i)
{
    const float3 wp = i.worldPos;
    const float3 V  = normalize(cameraPos - wp);

    const bool underwater = (V.y < 0.0);          // camera under the plane
    float3 N = WaterFlowNormal(wp.xz);

    // ---- Interactive ripples: bend N by the height-field gradient ----
    // WaterRippleSystem integrates a 2D wave equation into g_RippleField (.r = surface height
    // in metres); central differences give the slope, folded into the flow normal. A body
    // without ripples has g_hasRipple == 0 and skips the taps.
    if (g_hasRipple != 0)
    {
        // The ripple field is a local high-detail WINDOW, not the whole body: map worldXZ
        // through the window footprint and perturb only inside it (a stone makes a LOCAL ripple).
        const float2 ruv = (wp.xz - g_rippleWinOrigin) / max(g_rippleWinSize, 1e-3);
        if (all(ruv == saturate(ruv)))
        {
            const float  e   = g_rippleTexel;
            const float hC = g_RippleField.SampleLevel(g_Clamp, ruv,             0).r;
            const float hL = g_RippleField.SampleLevel(g_Clamp, ruv - float2(e, 0), 0).r;
            const float hR = g_RippleField.SampleLevel(g_Clamp, ruv + float2(e, 0), 0).r;
            const float hD = g_RippleField.SampleLevel(g_Clamp, ruv - float2(0, e), 0).r;
            const float hU = g_RippleField.SampleLevel(g_Clamp, ruv + float2(0, e), 0).r;
            // Height-difference gradient -> slope, scaled DIRECTLY by the strength knob. Do not
            // divide by worldSize: ripples would vanish on large bodies and blow up on small ones.
            const float2 slope = -float2(hR - hL, hU - hD) * g_rippleNormalStrength;

            // Where the ripple is active (disturbed height or strong slope) FADE
            // the constant flow-normal detail so the rings read clearly instead
            // of fighting the ever-scrolling flow ("the whole surface moves").
            const float act  = saturate(abs(hC) * 6.0 + length(slope) * 0.5);
            float2 flowTilt  = N.xz * (1.0 - 0.85 * act);
            N = normalize(float3(flowTilt.x + slope.x, N.y, flowTilt.y + slope.y));
        }
    }

    if (underwater) N = float3(N.x, -N.y, N.z);

    // ---- Analytic water depth -> absorption + shoreline fade ----
    const float depth   = max(g_waterHeight - TerrainYAt(wp.xz), 0.0);
    const float absorb  = 1.0 - exp(-depth / max(g_absorbDist, 1e-3));
    float3 body = lerp(g_shallowColor.rgb, g_deepColor.rgb, absorb);
    const float alpha = saturate(depth / max(g_shoreFade, 1e-3));

    // Discard fully faded shoreline pixels so they stamp no depth or GBuffer normals
    // (terrain stays authoritative at the waterline).
    clip(alpha - 0.02);

    // ---- Sun visibility (CSM x cloud shadow) ----
    // Resolved once and shared by the glint and the sun-aligned dimming of the cube
    // reflection, so a glint never blazes off water that lies under a cloud. The bias
    // normal is the plane's up (the flow normal is too high-frequency for the offset).
    const float  sunShadow = ComputeShadowFactor(wp, float3(0, 1, 0), i.sv.xy)
                           * SampleCloudShadow(wp, g_Clamp);
    const float3 L = normalize(-lightDir);   // FROM-light convention -> negate

    // ---- Fresnel sky reflection ----
    const float NdotV = saturate(dot(N, V));
    const float F = g_fresnelF0 + (1.0 - g_fresnelF0) * pow(1.0 - NdotV, 5.0);

    float3 R = reflect(-V, N);
    float3 sky;
    if (underwater)
    {
        // Looking up at the underside: the reflected ray points back into the water (TIR-ish)
        // and the cube's below-horizon content is undefined, so reflect the absorption colour.
        sky = g_deepColor.rgb * g_reflStrength;
    }
    else
    {
        R.y = max(R.y, 0.02);                      // keep off the lower hemisphere
        R   = normalize(R);
        sky = g_SkyCube.SampleLevel(g_Clamp, R, 0).rgb * g_reflStrength;

        // A cube cannot be occluded by scene geometry, so at a low sun its bright sun streak
        // crosses water the terrain already shadows. For the sun-aligned part the path
        // eye -> surface -> sun is what the CSM measures, so the receiver's sun visibility applies
        // exactly. The exponent sets the width over the sun halo: lower it if the streak leaks,
        // raise it if shadows darken too much sky.
        const float sunAlign = pow(saturate(dot(R, L)), 8.0);
        sky *= lerp(1.0, sunShadow, sunAlign);
    }

    // ---- SSR dampening (prev frame, same convention as Lighting.ps) ----
    // SSRComposite later adds ssr.rgb x F x envBRDF x conf using the normal / roughness this
    // writes, so the sky term makes room. Not a linear (1 - conf): conf rarely reaches 1
    // while the sky is often 10-100x brighter than the reflected scene, so the bleed drowns
    // the reflection; smoothstep treats a moderately confident hit as fully occluding and
    // keeps the soft fade near conf ~0 (true misses).
    // The SSR result is the PREVIOUS frame's trace on the previous camera's grid: sample at
    // this point's prev-frame UV (exact at the contact line, where surface and reflection
    // share a depth). Loading at the current pixel bands every confidence edge under motion.
    // Off-screen prev -> the current pixel.
    // The texture is opened inside the gate, so nothing is fetched when it fails.
    // ssrResultIdx is a LightCB value, uniform across the draw: no NonUniformResourceIndex.
    // Do not cache it: SSRSubsystem::OnResize recreates SSRUpsamplePass::m_colorTex on a
    // render-resolution change, so a cached index would name a freed descriptor; the Renderer
    // re-derives it every frame. With SSR off it names WaterPass's 1x1 black texture:
    // .a == 0 -> skyVis == 1.
    float ssrConf = 0.0;
    if (!underwater && i.prevClip.w > 1e-3)
    {
        Texture2D<float4> ssrResultTex = BindlessTex2D<float4>(ssrResultIdx);
        float2 prevNdc = i.prevClip.xy / i.prevClip.w;
        float2 prevUV  = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
        ssrConf = all(saturate(prevUV) == prevUV)
            ? saturate(ssrResultTex.SampleLevel(g_Clamp, prevUV, 0).a)
            : saturate(ssrResultTex.Load(int3(int2(i.sv.xy), 0)).a);
    }
    const float skyVis = 1.0 - smoothstep(0.05, 0.55, ssrConf);
    sky *= skyVis;

    // ---- Sun glint (Blinn, Fresnel-weighted, shadowed by sunShadow) ----
    const float3 H = normalize(L + V);
    const float  NdotH = saturate(dot(N, H));
    const float  VdotH = saturate(dot(V, H));
    const float  Fspec = g_fresnelF0 + (1.0 - g_fresnelF0) * pow(1.0 - VdotH, 5.0);
    float3 spec = lightColor * (pow(NdotH, g_specPower)
                  * Fspec * (g_specPower + 8.0) * (1.0 / 25.13))
                  * sunShadow;

    float3 color = lerp(body, sky, F) + spec;

    PSOut o;
    // GBuffer stamps for the post-frame SSR trace: the animated flow normal and mirror-ish
    // roughness. surface.a = 0.5 -> composite F0 = 0.04 (water).
    o.normal  = float4(N * 0.5 + 0.5, 0.0);
    o.surface = float4(g_waterRoughness, 0.0, 1.0, 0.5);

    // Motion vectors, so TAA and the SSR temporal reproject water pixels with the WATER's
    // motion, not the submerged terrain's. Raw NDC delta (curNDC - prevNDC), as GBuffer.ps
    // writes and every decoder assumes (TAA_Reproject.hlsli: prevNDC = curNDC - velocity;
    // SSRTemporal / XeGTAOTemporal apply the (0.5,-0.5) NDC -> UV scale). Do not pre-scale
    // here: it would halve X and flip Y after decode.
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;
    o.velocity = cur - prev;

    o.color   = float4(color, alpha);
    return o;
}
