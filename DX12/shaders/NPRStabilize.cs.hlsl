// Final opaque-material exposure protection + NPR HDR shoulder. Runs after every
// graphics HDR contribution and before temporal AA / upscaling.
//
// The pass sees two images of the same frame:
//   g_input (t0) - the complete HDR pixel: opaque lighting + emissive + clouds /
//                  fog / video / face overlay / transparents / VFX.
//   g_lit   (t4) - the lit-scene snapshot taken right after FaceOverlayPass
//                  (NPRLitSnapshot graph pass): opaque lighting + emissive +
//                  clouds / fog / video + the character-authored face overlay.
//                  The overlay must be inside this image: outside it, the
//                  recombine adds the face's correction onto the darker brow/lash
//                  colour ("brows suddenly bright").
// All NPR treatment (EV bias, shoulder, ceilings) and the exposure protection are
// computed on the lit part only; the overlay delta (full - lit) passes through with
// a coverage-scaled share of the correction. Do not clamp the whole pixel by the
// material under it: a bright beam or fog bank crossing an NPR character would be
// clamped inside the silhouette only, a character-shaped hole in the effect.

#include "material.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx

cbuffer StabilizeCB : register(b0, space2)
{
    uint  g_width;
    uint  g_height;
    float g_sceneMaxBrightness;
    float g_exposureProtectionLimit;

    float g_manualExposure;
    uint  g_useManualExposure;
    uint  g_useLitSnapshot;
    float _pad0;
};

Texture2D<float4>                  g_input     : register(t0, space2);
Texture2D<float4>                  g_normal    : register(t1, space2);
StructuredBuffer<MaterialGPUData>  g_materials : register(t2, space2);
Texture2D<float>                   g_depth     : register(t3, space2);
Texture2D<float4>                  g_lit       : register(t4, space2);
StructuredBuffer<float>            g_exposure  : register(t5, space2);
RWTexture2D<float4>                g_output    : register(u0, space2);

#include "core/color.hlsli"   // Luminance

// Identity below the knee; rational shoulder above it.  The asymptote is
// start+range, unlike a hard clamp, so highlight ordering and hue survive.
float SoftShoulder(float value, float start, float range)
{
    if (value <= start) return value;
    float d = value - start;
    return start + d * range / (range + d);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height) return;

    int2 px = int2(id.xy);
    float4 src = g_input.Load(int3(px, 0));
    float3 full = max(src.rgb, 0.0);
    float3 outColor = full;

    // Reverse-Z clear is zero.  Sky has no material and must remain entirely
    // under the scene exposure/tonemap contract.
    if (g_depth.Load(int3(px, 0)) > 1e-7)
    {
        // Without a snapshot (first frame after a resize), treat the full pixel.
        float3 lit = (g_useLitSnapshot != 0u)
            ? max(g_lit.Load(int3(px, 0)).rgb, 0.0) : full;

        float4 normalRaw = g_normal.Load(int3(px, 0));
        uint matIdx = GBufferDecodeMatIdx(normalRaw.a);
        uint materialCount, materialStride;
        g_materials.GetDimensions(materialCount, materialStride);
        matIdx = min(matIdx, max(materialCount, 1u) - 1u);
        // matIdx 0 is the reserved neutral record (draws with no material: terrain,
        // WaterComponent water, overflow): shaderType 0, exposureCompensation 0, so both
        // branches below are identity for it by construction.
        MaterialGPUData mat = g_materials[matIdx];

        float exposure = (g_useManualExposure != 0u)
            ? g_manualExposure : g_exposure[0];
        if (!isfinite(exposure) || exposure < 1e-4) exposure = 1.0;

        // Keyed on the shading model, not on shaderType.
        const bool isNPR   = (mat.customShadingModel == SHADING_MODEL_TOON);
        const bool isUnlit = (mat.customShadingModel == SHADING_MODEL_UNLIT);

        // Exposure protection. NPR and unlit materials may go to full exposure exclusion
        // (exposureCompensation 1.0 -> the counter-scale exactly cancels the tonemap
        // multiply, so the ACES input is fully authored and never moves with adaptation);
        // this is safe because the pass reads the GPU exposure buffer directly, not a
        // frames-stale CPU readback. An unlit surface has no lighting term, so there is no
        // physical exposure to opt out of; capping it makes unlit face parts (brows /
        // lashes / iris / eye shadow) swim with adaptation while the Toon skin holds still.
        // A PBR surface keeps the g_exposureProtectionLimit cap: it should resist
        // adaptation, not opt out of physical exposure. Only the limit widens for unlit:
        // the shoulder / EV-bias / ceiling block below stays gated on isNPR (toon-authored
        // knobs).
        float protLimit = (isNPR || isUnlit) ? 1.0
                                             : saturate(g_exposureProtectionLimit);
        float protection = min(saturate(mat.exposureCompensation), protLimit);
        float3 treated = lit * lerp(1.0, rcp(exposure), protection);
        if (isNPR)
        {
            treated *= exp2(clamp(mat.nprExposureBiasEV, -2.0, 2.0));

            // Work in exposed HDR, because the style failure occurs at the ACES input, and
            // guard by max RGB rather than luminance (skin can have a safe luma while its red
            // channel is deep in ACES). Both limiters share this one signal and space, so the
            // authored ceiling bounds every channel and does not drift with auto-exposure.
            float3 exposed = treated * exposure;
            float  signal  = max(exposed.r, max(exposed.g, exposed.b));
            float  s       = signal;

            // Artistic shoulder. NOTE with the shipped defaults (start 1.0,
            // range 0.75, strength 1.0) every NPR material is asymptotically
            // capped at exposed max-RGB 1.75; author strength 0 to opt out.
            float start = max(mat.nprShoulderStart, 1e-4);
            float range = max(mat.nprShoulderRange, 1e-4);
            s = lerp(s, SoftShoulder(s, start, range),
                     saturate(mat.nprShoulderStrength));

            // Absolute ceilings, also soft (knee at half, asymptote at the authored value),
            // applied after the artistic shoulder. Do not use a hard proportional rescale: it
            // maps every over-ceiling pixel to one flat plateau (a C1 crease, a gradient-dead
            // region, seams at material boundaries).
            float maxSig = 3.402823466e+38;
            if (mat.nprMaxBrightness > 0.0)
                maxSig = min(maxSig, mat.nprMaxBrightness);
            if (g_sceneMaxBrightness > 0.0)
                maxSig = min(maxSig, g_sceneMaxBrightness);
            if (maxSig < 3.402823466e+37)
                s = SoftShoulder(s, 0.5 * maxSig, 0.5 * maxSig);

            if (signal > 1e-5) exposed *= s / signal;
            treated = exposed / exposure;
        }

        // Recombine: the correction belongs to the opaque surface, not to whatever
        // composited over it. Additive overlays (full >= lit in luma) leave the surface
        // fully visible -> full share. Darkening overlays (alpha-blended smoke/fog
        // replacing the surface) scale the share by fullLuma / litLuma as a coverage
        // estimate, so a fully covered surface carries ~none of its correction into the
        // covering medium.
        float3 corr = treated - lit;
        float  k = 1.0;
        float  fullLum = Luminance(full);
        float  litLum  = Luminance(lit);
        if (fullLum < litLum) k = fullLum / max(litLum, 1e-5);
        outColor = max(full + corr * k, 0.0);
    }

    g_output[px] = float4(outColor, src.a);
}
