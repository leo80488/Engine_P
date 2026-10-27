#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

// CSM shadow sampling for the graphics passes. Three biases fight self-shadow acne:
//   1. Slope-scaled depth bias (caster side, raster state in ShadowPass.cpp): pushes the
//      stored depth by |dz/dx|, |dz/dy|, tuned per cascade via the cull-mode group.
//   2. Normal offset (receiver side, here): shifts the receiver along its normal before
//      the light-space transform, in world meters per cascade x saturate(1 - NdotL).
//      Kills the grazing-angle acne slope scaling misses, without a flat depth bias's
//      peter-panning.
//   3. Receiver-plane depth bias (receiver side, here): ddx/ddy of shadow-space (uv, z)
//      give dz/du, dz/dv on the receiving plane; each PCF tap compares against the
//      predicted depth at its offset (depthCenter + dot(grad, off)), so taps over a
//      sloped surface do not self-collide.
//
// The includer declares before including:
//   - a cbuffer with shadowMatrix[4], cascadeSplits, shadowStrength, shadowBias,
//     gCsmCamPos, gCsmCamFwd, shadowMapTexelSize, shadowBlendRange, shadowFrameIndex,
//     cascadeTexelWorldSize, shadowNormalOffset, lightDir
//   - Texture2DArray<float> gShadowCascades : register(t9, space0)
//   - SamplerComparisonState gShadowSampler : register(s2)

// The math (PCF rotation, receiver-plane depth bias, 16-tap Vogel PCF) is
// shadow_eval.hlsli's, with resources and constants passed in; this file is the graphics
// binding.
#include "shadow_eval.hlsli"
#include "bindless.hlsli"   // typed bindless handles

// The per-pixel PCF rotation on this frame's index (its two cases: shadow_eval.hlsli).
float ShadowRotationAngle(float2 screenPos)
{
    return ShadowRotationAngleFrom(screenPos, shadowFrameIndex);
}

// Extract a single cascade's world-space texel size from the float4 packed
// in LightCB (HLSL forbids dynamic indexing of vector components).
float CascadeTexelWorld(int idx)
{
    return idx == 0 ? cascadeTexelWorldSize.x
         : idx == 1 ? cascadeTexelWorldSize.y
         : idx == 2 ? cascadeTexelWorldSize.z
         :            cascadeTexelWorldSize.w;
}

// Vogel-disk 16-tap PCF, rotated per pixel (Bayer 4x4) and per frame
// (ShadowRotationAngle: a hashed re-seat while temporal AA / DLSS is on). Per-tap depth
// corrected by the receiver-plane gradient; the receiver pre-shifted along N.
float SampleCascadeShadow(int idx, float3 worldPos, float3 N, float2 screenPos)
{
    // ---- 1) Normal offset bias ----
    // Push the receiver along its normal before sampling, by this cascade's world-space texel
    // size (so the visual offset is constant across cascades) x (1 - NdotL): head-on surfaces
    // get no offset (full PCF range), grazing ones the full push, exactly where the
    // slope-scaled caster bias falls short.
    const float3 L          = normalize(-lightDir);
    const float  NdotL      = saturate(dot(N, L));
    const float  texelWorld = CascadeTexelWorld(idx);
    const float  noScale    = shadowNormalOffset * texelWorld * (1.0 - NdotL);
    const float3 offsetPos  = worldPos + N * noScale;

    float4 sp = mul(float4(offsetPos, 1.0), shadowMatrix[idx]);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || sp.z < 0.0 || sp.z > 1.0)
        return 1.0;

    const float depthCenter = sp.z;
    const float texelSize   = shadowMapTexelSize;

    // ---- 2) Receiver-plane depth gradient ----
    // (dz/du, dz/dv) on the surface, once per pixel for every PCF tap. Outside the loop
    // because ddx/ddy of a per-tap quantity is undefined (and the plane is the same).
    const float2 rpdb = ReceiverPlaneDepthBias(float3(uv, depthCenter));

    float  baseAngle   = ShadowRotationAngle(screenPos);

    // shadowBias: the constant receiver-side compare bias (LightCB, 0.0003 NDC) added at
    // every tap. Reversed-Z: raising the reference makes the GREATER_EQUAL compare pass more
    // easily -> biases toward lit. Needed on top of the other two: normal offset scales by
    // (1 - NdotL) and RPDB only tracks the receiving plane, so neither covers the plain
    // rasterization / quantization mismatch between caster grid and receiver (the back-cull
    // acne speckle). RPDB keeps each tap's reference on the centre sample's plane, so it moves
    // with the surface (reversed-Z safe).
    return VogelPcf16(gShadowCascades, gShadowSampler, uv, float(idx), depthCenter, rpdb,
                      shadowBias, 2.5, texelSize, baseAngle);
}

#ifdef PER_OBJECT_SHADOW
// Per-object (player) high-res directional shadow. Like SampleCascadeShadow, but samples
// one Texture2D map fitted to the player AABB along the sun direction (reversed-Z).
// Returns 1.0 (lit) for any receiver outside the player's frustum, so the caller can do
// `shadow = min(csm, perObject)`: inside, the crisp player map wins (self-shadow, ground
// contact); outside it is a no-op.
//
// The includer #defines PER_OBJECT_SHADOW. Uses gShadowSampler (s2) and the LightCB
// perObjShadow* fields (light_cb.hlsli); the map is opened from the heap index LightCB
// carries (perObjShadowIdx), so the graphics root signature spends no table on it.
float SamplePerObjectShadow(float3 worldPos, float3 N, float2 screenPos)
{
    // perObjShadowIdx comes from a CB, so it is wave-uniform: no NonUniformResourceIndex,
    // which would only buy a scalarization loop.
    //
    // The declared type must match the SRV LightingPass hands over exactly: the live
    // PerObjectShadowPass map (D32_FLOAT -> R32_FLOAT Texture2D SRV) and its 1x1 fallback are
    // both Texture2D<float>. Under Resource Binding Tier 3 a mismatch emits no D3D12
    // validation message at all.
    //
    // Opened inside this function, not at file scope, so the read stays behind the caller's
    // gate (perObjShadowParams.x != 0).
    Texture2D<float> perObjShadowTex = BindlessTex2D<float>(perObjShadowIdx);

    // Normal-offset bias, as on the cascade path, sized by the per-object map's world-space
    // texel size x (1 - NdotL).
    const float3 L          = normalize(-lightDir);
    const float  NdotL      = saturate(dot(N, L));
    const float  texelWorld = perObjShadowParams.y;
    const float  noStrength = perObjShadowParams.w;
    const float3 offsetPos  = worldPos + N * (noStrength * texelWorld * (1.0 - NdotL));

    float4 sp = mul(float4(offsetPos, 1.0), perObjShadowMatrix);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;

    // Depth outside the [0,1] slab -> fully lit (no-op for min()).
    if (sp.z < 0.0 || sp.z > 1.0)
        return 1.0;

    // Soft edge fade: ramp toward 1.0 (lit) over the outer ~8% of the map so the per-object
    // -> CSM transition is gradual, not a hard seam where the fitted frustum ends. 0 at or
    // beyond the border (CSM takes over) -> 1 well inside (full per-object).
    float2 edgeDist = min(uv, 1.0 - uv);
    float  edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 12.5); // fade width = 0.08 UV
    if (edgeFade <= 0.0)
        return 1.0;

    const float depthCenter = sp.z;
    const float texelSize   = perObjShadowTexelSize;
    const float biasRcv     = perObjShadowParams.z; // reversed-Z: raises ref -> toward lit

    const float2 rpdb = ReceiverPlaneDepthBias(float3(uv, depthCenter));

    float  baseAngle   = ShadowRotationAngle(screenPos);

    // Radius 2 texels: tighter than CSM, the map is already high-res.
    const float shadow = VogelPcf16(perObjShadowTex, gShadowSampler, uv, depthCenter, rpdb,
                                    biasRcv, 2.0, texelSize, baseAngle);
    // Blend toward lit at the frustum border (edgeFade) so the boundary is soft.
    return lerp(1.0, shadow, edgeFade);
}
#endif // PER_OBJECT_SHADOW

#ifdef HAIR_SHADOW
// Hair (bangs -> face) shadow. Like SamplePerObjectShadow, but samples the small
// bangs-only map (only HairShadowCasterComponent-tagged meshes render into it) with the
// LightCB hairShadow* fields. Returns 1.0 (lit) outside the fitted frustum / border fade.
//
// Deliberately not composited into ComputeShadowFactor: the map is an artistic bangs
// shadow for NPR faces, so Lighting.ps calls it on face pixels only (extra.a marker) and
// min()s it into the SDF face terminator; body pixels keep their CSM / per-object shadows
// and are never darkened twice by the bangs.
//
// The includer #defines HAIR_SHADOW. Uses gShadowSampler (s2); the map is opened from
// LightCB.hairShadowIdx.
float SampleHairShadow(float3 worldPos, float3 N, float2 screenPos)
{
    // Wave-uniform CB index; Texture2D<float> matches both the live hair map (D32_FLOAT ->
    // R32_FLOAT Texture2D SRV) and the shared 1x1 fallback. Opened inside the function so the
    // read stays behind the caller's gate (hairShadowParams.x != 0, NPR face pixels only).
    Texture2D<float> hairShadowTex = BindlessTex2D<float>(hairShadowIdx);

    // Normal-offset bias, as on the per-object path, sized by the hair map's world-space
    // texel size x (1 - NdotL).
    const float3 L          = normalize(-lightDir);
    const float  NdotL      = saturate(dot(N, L));
    const float  texelWorld = hairShadowParams.y;
    const float  noStrength = hairShadowParams.w;
    const float3 offsetPos  = worldPos + N * (noStrength * texelWorld * (1.0 - NdotL));

    float4 sp = mul(float4(offsetPos, 1.0), hairShadowMatrix);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;

    // Depth outside the [0,1] slab -> fully lit (no-op for min()).
    if (sp.z < 0.0 || sp.z > 1.0)
        return 1.0;

    // Soft edge fade toward lit over the outer ~8% of the fitted footprint so
    // the shadow dissolves instead of hard-clipping where the frustum ends.
    float2 edgeDist = min(uv, 1.0 - uv);
    float  edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 12.5); // fade width = 0.08 UV
    if (edgeFade <= 0.0)
        return 1.0;

    const float depthCenter = sp.z;
    const float texelSize   = hairShadowTexelSize;
    const float biasRcv     = hairShadowParams.z; // reversed-Z: raises ref -> toward lit

    const float2 rpdb = ReceiverPlaneDepthBias(float3(uv, depthCenter));

    float  baseAngle   = ShadowRotationAngle(screenPos);

    // Radius 1.5 texels: tight PCF, an anime bangs shadow wants a crisp edge.
    const float shadow = VogelPcf16(hairShadowTex, gShadowSampler, uv, depthCenter, rpdb,
                                    biasRcv, 1.5, texelSize, baseAngle);
    return lerp(1.0, shadow, edgeFade);
}
#endif // HAIR_SHADOW

// Raw CSM cascade term (no shadowStrength lerp, none of the per-object / RT folds):
// picks the cascade by view Z and returns the shadow factor [0,1]. Four cascades: 0..2 are
// the near cascades, 3 the ultra-far terrain cascade (typically out to ~2000 m). Beyond
// cascade 3's far boundary the receiver is lit - too distant for a perceptible shadow.
float ComputeCascadeShadowTerm(float3 worldPos, float3 N, float2 screenPos)
{
    // gCsmCamPos / gCsmCamFwd, not cameraPos / cameraForward: the split table and the four
    // cascade matrices were fitted to one camera, and picking a cascade with another camera's
    // depth samples the wrong slice. The main path writes gCsmCam* equal to cameraPos /
    // cameraForward; a secondary view that borrows the cascades leaves them at the fitting
    // camera's while cameraPos follows its own lens.
    float viewZ = dot(worldPos - gCsmCamPos, gCsmCamFwd);

    // Beyond the ultra-far cascade boundary there is no coverage -> lit (shadow = 1); the
    // per-object map can still contribute in ComputeShadowFactor.
    float shadow = 1.0;
    if (viewZ <= cascadeSplits.w)
    {
        int cascade = 0;
        if      (viewZ > cascadeSplits.z) cascade = 3;
        else if (viewZ > cascadeSplits.y) cascade = 2;
        else if (viewZ > cascadeSplits.x) cascade = 1;

        shadow = SampleCascadeShadow(cascade, worldPos, N, screenPos);

        // Cross-fade across cascade boundaries. Every cascade's near edge uses the same
        // `shadowBlendRange` width regardless of physical extent, which gives visually constant
        // blend bands across the screen.
        if (cascade < 3 && shadowBlendRange > 0.0)
        {
            float splitFar = (cascade == 0) ? cascadeSplits.x
                           : (cascade == 1) ? cascadeSplits.y
                           :                  cascadeSplits.z;
            float blendT   = saturate((viewZ - (splitFar - shadowBlendRange)) / shadowBlendRange);
            if (blendT > 0.0)
                shadow = lerp(shadow, SampleCascadeShadow(cascade + 1, worldPos, N, screenPos), blendT);
        }
    }

    return shadow;
}

float ComputeShadowFactor(float3 worldPos, float3 N, float2 screenPos)
{
    if (shadowStrength <= 0.0) return 1.0;

    float shadow = ComputeCascadeShadowTerm(worldPos, N, screenPos);

#ifdef PER_OBJECT_SHADOW
    // Compose the player's high-res shadow, darkest wins: inside the player's frustum the
    // crisp self-shadow and ground contact; outside, SamplePerObjectShadow returns 1.0.
    if (perObjShadowParams.x != 0.0)
        shadow = min(shadow, SamplePerObjectShadow(worldPos, N, screenPos));
#endif

#ifdef RT_SHADOW_MASK
    // RT sun-shadow mask (RTShadowPass): full-screen, render-res, 1 = lit. Same darkest-wins
    // fold as the per-object map, so overlap with CSM / per-object never double-darkens. The
    // mask is opened from the heap index LightCB carries; rtShadowMaskIdx is wave-uniform, so
    // no NonUniformResourceIndex (it would only buy a scalarization loop).
    //
    // Open it inside the branch: the fallback texture is 1x1 white, and a full-screen Load()
    // against it is out of bounds for every pixel but (0,0). rtShadowEnabled is the gate, so
    // frames without a fresh mask (pass off, no TLAS, no sun) never sample a stale texture.
    if (rtShadowEnabled != 0.0)
    {
        Texture2D<float4> rtShadowMaskTex = BindlessTex2D<float4>(rtShadowMaskIdx);
        shadow = min(shadow, rtShadowMaskTex.Load(int3(int2(screenPos), 0)).a);   // its largest channel
    }
#endif

    return lerp(1.0, shadow, shadowStrength);
}

#ifdef RT_SHADOW_MASK
// The colour BlendMode::Transmissive glass leaves on the sun, where the RT mask darkens it: the
// mask's RGB over its largest channel (the number ComputeShadowFactor folds in), 1 when the pass is
// off or the light that reaches here is grey. Multiply the sun by it wherever ComputeShadowFactor's
// result stands for its visibility.
float3 RTShadowTint(float2 screenPos)
{
    if (rtShadowEnabled == 0.0) return 1.0;
    Texture2D<float4> rtShadowMaskTex = BindlessTex2D<float4>(rtShadowMaskIdx);
    const float4 m = rtShadowMaskTex.Load(int3(int2(screenPos), 0));
    return m.a > (1.0 / 255.0) ? saturate(m.rgb / m.a) : 1.0;
}
#endif

// External-world shadow only: the CSM cascades, skipping the per-object player map and the
// RT sun mask, which both contain the character's own casters (bangs -> face, face -> face,
// body -> face). NPR face pixels use this so the face never receives the character's
// projected self-shadow through a shadow map: its self-shadow is the SDF terminator and the
// bangs' shadow the artistic hair term (screen mark / fitted hair map), both folded in by
// Lighting.ps. While the per-object map is active, ShadowPass excludes the character's
// packets from the CSM, so the cascades are exactly the external channel; with per-object
// off the character is back in the CSM and cannot be separated per caster here.
float ComputeShadowFactorExternalOnly(float3 worldPos, float3 N, float2 screenPos)
{
    if (shadowStrength <= 0.0) return 1.0;
    return lerp(1.0, ComputeCascadeShadowTerm(worldPos, N, screenPos), shadowStrength);
}

#endif // SHADOW_HLSLI
