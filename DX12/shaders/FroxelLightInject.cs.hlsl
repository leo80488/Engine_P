#include "spot_shadow_view.hlsli"
// FroxelLightInject.cs.hlsl - multiplies the per-froxel scattering coefficient by
// the incoming light at that point. Output:
//   .rgb = density.scattering x sum(L_in x phase x shadow) + emission
//   .a   = density.extinction   (carried through unchanged)
//
// Injects:
//   - Spot / point lights WITHOUT a shadow-atlas slice (the "many cheap lights"
//     tier), occluded by a screen-space march and the SceneVoxel grid.
//   - A constant ambient term (so dark voxels still pick up a small grey).
//   - Emission from the global medium and the emissive local fog volumes.
// The sun and the shadow-casting spots are NOT here: VolumetricRaymarch.cs
// integrates them per pixel against the CSM / spot atlas (see main below).
//
// The cone is light_eval.hlsli's AngularAttenuation, the same function every
// surface uses; the distance falloff is the volumetric window below.

#include "FroxelCommon.hlsli"

#include "cluster_common.hlsli"
#include "light_eval.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>           SrcDensity     : register(t0, space2);
Texture2DArray<float>       ShadowCascades : register(t1, space2);
StructuredBuffer<GPULight>  Lights         : register(t2, space2);
// Scene depth (camera view) for the cheap screen-space shadow march between each
// voxel and the spot/point light it receives; without it every spot beams straight
// through walls.
Texture2D<float>            SceneDepth     : register(t3, space2);
// World-space occupancy grid built by SceneVoxelPass, 128^3 R8_UINT; non-zero =
// "some scene AABB intersects this voxel". Complements SceneDepth by catching
// off-screen occluders, where the screen-space shadow fails.
Texture3D<uint>             SceneOccupancy : register(t4, space2);
// Local fog volumes - only the emissive ones (sorted first) are walked here.
#define LOCAL_FOG_VOLUMES_REGISTER register(t5, space2)
#include "LocalFogVolumes.hlsli"
// Per-spot-light shadow atlas (opt-in via LightData::castsShadow); the layout
// mirrors the graphics root signature's t21/t22 space0 slots.
Texture2DArray<float>       SpotShadowAtlas : register(t6, space2);
StructuredBuffer<SpotShadowView>  SpotShadowVPs   : register(t7, space2);
// s0 space2 is the plain linear sampler shared by all compute passes; the CSM PCF
// lookup needs a COMPARISON sampler, declared at s1.
SamplerComparisonState      ShadowSampler  : register(s1, space2);
SamplerState                LinearSampler  : register(s0, space2);

RWTexture3D<float4>         DstLighting    : register(u0, space2);

// Screen-space shadow: march from the voxel toward the light, project each sample
// into the camera's view and compare its NDC z with the scene depth. A sample behind
// the nearest scene surface (reversed Z: ndc.z < sceneDepth) counts as occluded.
// Returns [0, 1].
//   * Catches only occluders VISIBLE to the camera; geometry just off-screen (or
//     hidden by other occluders) slips through - VoxelOcclusion covers that.
//   * Costs ~kSteps depth taps per voxel per in-range light.
float ScreenSpaceShadow(float3 fromWorld, float3 toWorld)
{
    const int kSteps = 6;
    int   valid    = 0;
    float occluded = 0.0;

    [unroll] for (int i = 1; i <= kSteps; ++i)
    {
        // t in (0,1): skip the endpoints (the voxel itself and the light apex)
        float  t        = float(i) / float(kSteps + 1);
        float3 samplePos = lerp(fromWorld, toWorld, t);

        float4 clip = mul(float4(samplePos, 1.0), P.viewProj);
        if (clip.w <= 0.0) continue; // behind the camera - cannot test

        float3 ndc = clip.xyz / clip.w;
        float2 uv  = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        float sceneZ = SceneDepth.SampleLevel(LinearSampler, uv, 0).r;
        // Reversed Z: larger ndc.z = closer. The sample is behind geometry when its
        // ndc.z is smaller than the scene's depth at that UV.
        if (ndc.z < sceneZ - 1e-4) occluded += 1.0;
        valid += 1;
    }

    if (valid == 0) return 1.0; // whole ray off-screen -> assume visible
    return saturate(1.0 - occluded / float(valid));
}

// World-space voxel occlusion: complements ScreenSpaceShadow with occluders outside
// the camera's view (off-screen walls, objects hidden behind others in the depth
// buffer).
//
// Walks the ray voxel -> light at HALF-voxel steps so a one-voxel wall cannot slip
// between samples, and is binary: any hit blocks the ray. Averaging one hit over 16
// samples would dim by only ~6%, which is invisible.
//
// Returns 1 when the grid is not bound (voxelGridDim == 0 or any extent == 0), so
// the caller gets ScreenSpaceShadow alone.
float VoxelOcclusion(float3 fromWorld, float3 toWorld)
{
    if (P.voxelGridDim == 0u) return 1.0;
    if (any(P.voxelGridExtent <= 0.0)) return 1.0;

    float3 dir  = toWorld - fromWorld;
    float  dist = length(dir);
    if (dist < 1e-4) return 1.0;
    dir /= dist;

    // Voxels are ~cubic (SceneVoxelPass builds them so): extent.x / dim is the edge
    // length. A half-voxel step skips nothing.
    float voxelSize = P.voxelGridExtent.x / float(P.voxelGridDim);
    float stepSize  = max(voxelSize * 0.5, 0.01);
    // 96-step cap = 48 voxels (~24 m at 0.5 m/voxel); longer rays are rare and too
    // costly to march fully.
    int   maxSteps  = min(int(dist / stepSize), 96);

    float3 invExtent = 1.0 / P.voxelGridExtent;

    // i=1 skips the voxel's own cell (no self-occlusion when the fog voxel lies inside
    // a marked AABB), and the walk stops one step short of the light so the light's own
    // cell cannot block it (the Renderer also keeps light entities out of the
    // occupancy AABB list).
    [loop] for (int i = 1; i < maxSteps - 1; ++i)
    {
        float3 samplePos = fromWorld + dir * (float(i) * stepSize);
        float3 uvw       = (samplePos - P.voxelGridMin) * invExtent;
        if (any(uvw < 0.0) || any(uvw > 1.0)) continue;

        int3 vc = clamp(int3(uvw * float(P.voxelGridDim)),
                        0, int(P.voxelGridDim) - 1);
        if (SceneOccupancy.Load(int4(vc, 0)) > 0u)
            return 0.0; // hard shadow: a single hit blocks the ray
    }

    return 1.0;
}

// Shadow-map visibility for a spot light that owns a slice (shadowSliceIdx valid):
// exact occlusion for the slice's authored frustum, instead of the screen-space +
// voxel approximations.
float SpotShadowMapVisibility(float3 worldPos, uint sliceIdx)
{
    const SpotShadowView shadowView = SpotShadowVPs[sliceIdx];
    if (shadowView.atlasIndex == 0xFFFFFFFFu) return 1.0;
    Texture2DArray<float> atlas = ResourceDescriptorHeap[NonUniformResourceIndex(shadowView.atlasIndex)];
    float4 sc = mul(float4(worldPos, 1.0), shadowView.vp);
    // Behind the light's projection plane -> occluded. The shadow frustum matches the
    // spot cone, so light can reach such a voxel only through the light itself.
    if (sc.w <= 0.0) return 0.0;
    sc.xyz /= sc.w;
    float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    // UV / depth outside the shadow frustum = outside the spot's cone. Force occluded
    // so the authored frustum hard-bounds the volumetric contribution; returning 1 here
    // lets light escape the cone at the far / side boundaries (a bottom leak).
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        sc.z < 0.0 || sc.z > 1.0)
        return 0.0;

    // Small receiver bias: the rasterizer's slope-scaled DepthBias handles most acne at
    // caster time, this only covers receiver depth quantisation. A larger one (0.0015)
    // pushes pillar-base voxels above thin occluders, which then light through them.
    const float bias = 0.00025;

    // 2x2 PCF across the froxel's shadow-map footprint. A single tap misses pillars
    // thinner than a texel (it lands on whichever fragment wrote last); four offset taps
    // turn the hard per-texel threshold into a smooth 0 -> 1 ramp at geometry edges,
    // which the temporal pass needs to converge to clean edges around pillar bases.
    //
    // Not called in this shader (the spot loop skips every light that owns a slice), so
    // DXC strips it; it is the froxel-grid counterpart of VolumetricRaymarch's
    // SpotShadowVisibility. The texel size is this light's own tier resolution.
    const float kTexelSize = 1.0 / float(shadowView.resolution);
    float visibility = 0.0;
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        float2 o = (float2(dx, dy) - 0.5) * kTexelSize;
        visibility += atlas.SampleCmpLevelZero(
            ShadowSampler, float3(uv + o, (float)shadowView.slice), sc.z + bias);
    }
    return visibility * 0.25;
}

// The spot-light count rides in shadowParams.y, so FroxelParams keeps its layout.
#define SPOT_LIGHT_COUNT  ((uint)(P.shadowParams.y))

// CSM PCF. ShadowPass leaves the array in DEPTH_READ_SRV (which includes
// NON_PIXEL_SHADER_RESOURCE) and the compute root signature has a comparison
// sampler at s1 space2, so the volumetric pass reads CSM directly. Not called in
// this shader (the sun is VolumetricRaymarch's); kept as the froxel counterpart.
//
// The cascade is not chosen by view-Z (that depends on cameraForward and
// cascadeSplits being right, and fails to "no shadow" when they are not): each
// cascade is probed near -> far and the first whose projected UV lies inside the
// shadow map wins, robust against bad CB data.
float SampleSingleCascade(int cascade, float4x4 m, float3 worldPos)
{
    float4 sp = mul(float4(worldPos, 1.0), m);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;

    // Out-of-cascade signal: the caller falls through to the next cascade.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        sp.z < 0.0 || sp.z > 1.0)
        return -1.0;

    // Reversed-Z: closer surfaces have LARGER depth. Self-shadowing happens when the
    // receiver's z is fractionally BELOW the map's, so push the receiver slightly closer
    // to bias the SampleCmp GREATER_EQUAL compare toward "lit".
    float depth     = sp.z + P.shadowParams.z;
    float texelSize = P.shadowParams.x;

    float shadow = 0.0;
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        float2 o = (float2(dx, dy) - 0.5) * texelSize;
        shadow += ShadowCascades.SampleCmpLevelZero(
            ShadowSampler, float3(uv + o, float(cascade)), depth);
    }
    return shadow * 0.25;
}

// First in-bounds UV wins (near -> far). The surface shader can pick by view-Z
// because its view-Z comes exactly from the depth buffer; a voxel's view-Z comes
// from the log slice distribution and is often wrong near cascade boundaries, and
// picking cascade 0 for a far voxel samples its cleared (1.0) area -> the voxel
// reads fully lit, which shows as frustum-shaped bright/dark banding.
float ComputeSunShadow(float3 worldPos)
{
    float4x4 mats[3] = { P.shadowMatrix0, P.shadowMatrix1, P.shadowMatrix2 };
    [unroll] for (int c = 0; c < 3; ++c)
    {
        float s = SampleSingleCascade(c, mats[c], worldPos);
        if (s >= 0.0) return s;
    }
    return 1.0;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;

    // XY-only temporal sub-voxel jitter: the hard discontinuities (spot cone edge, CSM
    // boundary) are sampled at slightly different positions each frame, and the
    // temporal pass smooths them into gradients. Z is NOT jittered: far froxels are
    // metres deep, and a per-frame Z shift shows as depth flicker temporal reprojection
    // cannot hide.
    float3 jitter = GetFroxelJitter(P.frameIndex);
    jitter.z = 0.0;
    float3 worldPos = FroxelToWorldJittered(DTid, P, jitter);
    float4 dens     = SrcDensity[DTid];

    // View direction at this voxel (camera -> froxel).
    float3 V = normalize(worldPos - P.cameraPos);

    // ---- Directional sun & shadow-casting lights ----
    // Handled by the per-pixel VolumetricRaymarch pass, not this grid: 128 Z slices are
    // too coarse for sharp CSM cascade edges or spot-atlas shadow boundaries, which a
    // full-res raymarch reads exactly. This pass injects only the cheap many-lights
    // tier: opted-in lights without shadow maps.
    float3 sunRadiance = float3(0, 0, 0);

    // ---- Spot / point lights (no-shadow tier only) ----
    float3 spotRadiance = float3(0, 0, 0);
    uint   spotCount    = SPOT_LIGHT_COUNT;
    [loop] for (uint li = 0; li < spotCount; ++li)
    {
        GPULight L = Lights[li];
        if (L.type == 0u) continue;                 // skip directional
        // Lights with a shadow-atlas slice belong to VolumetricRaymarch (per-pixel march +
        // shadow-map lookup, sharp occluders).
        if (L.shadowSliceIdx != 0xFFFFFFFFu) continue;

        float3 toL    = L.position - worldPos;
        float  dist   = length(toL);
        if (dist > L.radius) continue;
        float3 Ldir   = toL / max(dist, 1e-4);

        // Distance attenuation for VOLUMETRIC fog. Surfaces use 1/d^2 (solid-angle
        // falloff); the volumetric path must not: the voxel nearest a light contains the
        // light position, jitter moves the sample in and out of the singularity, and the
        // inverse square turns that into per-frame flicker at the cone apex.
        //
        // A smooth windowed falloff (Frostbite style) instead: ~1 near the light, 0 at the
        // radius, stable everywhere. The density / transmittance integration supplies the
        // physical dropoff along the ray.
        float distNorm  = dist / max(L.radius, 1e-4);
        float distNorm2 = distNorm * distNorm;
        float atten     = saturate(1.0 - distNorm2 * distNorm2);
        atten *= atten;

        // Spot cone: the surfaces' function (inner cone 0.75 x outer; 1 for a point
        // light), so a beam is as wide as the pool of light it casts. The temporal jitter
        // and the reprojection pass resolve the ramp on the 160x90x128 grid; a light that
        // needs a crisp beam has a shadow slice and goes through VolumetricRaymarch.
        const float spotFactor = AngularAttenuation(Ldir, L);
        if (spotFactor <= 0.0) continue;

        // Phase function, view toward the LIGHT (Ldir is voxel -> light).
        float spotCos   = dot(V, Ldir);
        float spotPhase = PhaseHG(spotCos, P.anisotropy);

        // No shadow atlas for these lights (the "many cheap lights" tier); the
        // screen-space + voxel-grid approximations keep beams from leaking through the
        // walls they hit. For a "strictly no occlusion" tier, drop these two calls.
        float visSS = ScreenSpaceShadow(worldPos, L.position);
        float visVX = VoxelOcclusion   (worldPos, L.position);
        float visibility = min(visSS, visVX);

        spotRadiance += L.color * L.intensity * atten * spotFactor * spotPhase * visibility;
    }

    // ---- Ambient (cheap "multi-scatter" stand-in) ----
    float3 ambient = P.ambientColor * P.ambientContribution / (4.0 * PI);

    float3 inLight = sunRadiance + spotRadiance + ambient;

    // ---- Emission (self-illuminated medium): Le x sigma_t ----
    // Global emissive is zero for the pass-owned settings, so this adds +0 there.
    float3 emission = P.fogEmissive * GlobalFogExtinction(P, worldPos);
    [loop] for (uint i = 0; i < P.localEmissiveCount; ++i)
    {
        const LocalFogVolumeGPU v = LocalFogVolumes[i];
        const float w = LocalFogWeight(v, worldPos);
        if (w > 0.0) emission += v.emissive * (v.density * w);
    }

    // Final per-voxel radiance: sigma_s x L_in + Le x sigma_t (extinction passes through)
    DstLighting[DTid] = float4(dens.rgb * inLight + emission, dens.a);
}
