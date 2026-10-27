#ifndef FROXEL_COMMON_HLSLI
#define FROXEL_COMMON_HLSLI

// Shared header for the volumetric-fog froxel pipeline:
//   FroxelDensity.cs.hlsl     - fills the 3D density texture
//   FroxelLightInject.cs.hlsl - multiplies density by per-light scattering
//   FroxelTemporal.cs.hlsl    - reprojects and blends with history
//   FroxelScatter.cs.hlsl     - front-to-back integration along z
//   VolumetricApply.ps.hlsl   - composites onto the HDR scene (via DepthCommon.hlsli)
// VolumetricRaymarch and VolumetricRaymarchTemporal include it too; height_fog.hlsli
// keeps its own copy of PhaseHG (HeightFog_PhaseHG): change both.
// FROXEL_RT (the ray-traced lighting): FroxelLightInjectRT.cs.hlsl, FroxelIndirectRT.cs.hlsl and
// the FROXEL_RT builds of Density, Temporal and Scatter, which read the appended block of
// FroxelParams.
//
// Coordinate convention:
//   Froxel (x, y, z) -> screen UV (xy) + exponential view-space depth slice (z).
//   z=0 is near, z=Depth-1 is far; depth uses a log(zFar/zNear) distribution.
//   FroxelToWorld places a slice at that distance ALONG the pixel's ray, the radial distance:
//   every build takes it so (Temporal's reprojection and the raymarch's density lookup
//   included), and Scatter resamples the integral onto planar view depth, which the composites'
//   lookups use (VolumetricApply, fog_apply.hlsli).

#include "core/math.hlsli"   // PI

// Filled from FroxelCB, which the consumer must declare with this layout (mirror:
// VolumetricFogPass's FroxelConstants).
struct FroxelParams
{
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 prevViewProj;

    float3 cameraPos;          float nearPlane;
    float farPlane;            float froxelNear;
    float froxelFar;           float temporalAlpha;

    uint   frameIndex;
    uint   froxelW;
    uint   froxelH;
    uint   froxelD;

    float fogDensity;
    float fogScattering;
    float fogAbsorption;
    float anisotropy;          // Henyey-Greenstein g

    float heightFogStart;
    float heightFogFalloff;
    float ambientContribution;
    float _pad0;

    float3 sunDir;             float sunStrength;     // LightCB lightDir: the direction the light travels (toward the ground)
    float3 sunColor;           float _pad1;
    float3 ambientColor;       float _pad2;

    // CSM (3 cascade matrices + per-cascade view-Z splits + sampling params), packed
    // into the same CB as the main params: the engine's compute root signature exposes
    // only b0 space2.
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
    float4x4 shadowMatrix2;
    float4   cascadeSplits;     // .xyz = far view-Z of cascade 0/1/2
    float4   shadowParams;      // x=texelSize, y=volumetric spot/point light count (as float), z=bias, w=strength
    float3   cameraForward;    float _pad3;

    // Voxel occupancy grid bounds (written by SceneVoxelPass, sampled by
    // FroxelLightInject for volumetric-light occlusion).
    float3   voxelGridMin;     float _padVG0;
    float3   voxelGridExtent;  uint  voxelGridDim; // extent = max - min

    // ---- The frame's LightCB and medium set, opened from the heap ----
    // Heap index of the frame's LightCB CBV (IGraphicsDevice::GetBufferCBVHeapIndex;
    // ~0u = none). VolumetricRaymarch reads the cloud shadow there (the map's index and
    // its basis rows) through light_cb.hlsli's LIGHT_CB_AS_STRUCT form, so the shafts
    // and the surfaces share one copy. Mirror: VolumetricFogPass's FroxelConstants.
    // rmMediumIdx: the medium set (Graphics/MediumSet.h, froxel_rt_medium.hlsli) the raymarch's
    // sun crosses on its way to each step; 0 = none, the sun arrives whole.
    uint     lightCbIdx;       uint  rmMediumIdx;        uint2 _padCS0;

    // ---- Scene fog extensions (appended; mirror: FroxelConstants) ----
    // fogAlbedo tints the global scattering (1,1,1 = untinted, bit-identical output).
    // fogEmissive is premultiplied by intensity and scaled by the local extinction in
    // FroxelLightInject. The two counts index the LocalFogVolumes buffer (t5 space2);
    // emissive volumes come first.
    float3   fogAlbedo;        uint  localVolumeCount;
    float3   fogEmissive;      uint  localEmissiveCount;

    // ---- Last frame's camera (appended, mirror: FroxelConstants) ----
    // Temporal reprojects by the distance to it in every build; rtFlags is the FROXEL_RT builds'.
    float3   prevCameraPos;    uint  rtFlags;            // kFroxelRt*

#if FROXEL_RT
    // ---- The ray-traced lighting (the FROXEL_RT builds; appended, mirror: FroxelConstants) ----
    // Heap indices (0 = none): Graphics/MediumSet.h's set, ClusterPass's lights, cluster grid,
    // index list, previous -> current light map and fog scales, the path materials (glass), the
    // density volume, this frame's and the last frame's light reservoirs.
    uint     mediumIdx;        uint  lightsIdx;          uint  lightCount;          uint  lightGridIdx;
    uint     lightIndexIdx;    uint  lightMapIdx;        uint  lightMediumScaleIdx; uint  pathMaterialsIdx;
    uint     densityIdx;       uint  reservoirIdx;       uint  prevReservoirIdx;    uint  candidates;
    uint     alphaTestEnabled; uint  alphaInstanceCount; float rtTemporalAlpha;     float sunMediumScale;
    float    clusterNear;      float clusterFar;         uint  prevLightCount;      uint  volLightsIdx;
    // The indirect light (FroxelIndirectRT.cs.hlsl): its source (kFroxelIndirect*), this frame's and
    // the last frame's grid (RWTexture3D<float4> UAVs), the sky the rays miss into and its scale,
    // the samples' weight, the path tracer's SHaRC cache (its UAVs, capacity and configuration), and
    // the grid the injection reads (the rays' smoothed by Filter; DDGI's is indirectIdx itself).
    // Then the ray-traced shafts' output (the raymarch's half-resolution texture, a UAV) and the scene
    // depth they march to (FroxelLightInjectRT.cs.hlsl's Shafts).
    uint     indirectMode;     uint  indirectIdx;        uint  prevIndirectIdx;     uint  skyIdx;
    float    environmentScale; float indirectAlpha;      uint  sharcCapacity;       uint  sharcHashUav;
    uint     sharcAccumulationUav; uint sharcResolvedUav; float sharcSceneScale;    float sharcRadianceScale;
    uint     indirectReadIdx;  uint  shaftsIdx;          uint  sceneDepthIdx;       uint  _padRt0;
#endif
};

#if FROXEL_RT
// FroxelParams::rtFlags
static const uint kFroxelRtSun        = 1u;   // the sun is lit here (no raymarched shafts this frame)
static const uint kFroxelRtSkipShadow = 2u;   // the raymarch takes its lights: the first rtFlags >> 8
                                             // of volLightsIdx's that hold a shadow slice
static const uint kFroxelRtHistory    = 4u;   // last frame's reservoirs and lighting are this view's
static const uint kFroxelRtGlass      = 8u;   // the TLAS holds a Transmissive instance
static const uint kFroxelRtIndirectHistory = 16u;   // last frame's indirect grid is this view's

// A froxel's light reservoir: (light | M << 16, asuint(W)); light 0xFFFF = none.
static const uint kFroxelRtNoLight = 0xFFFFu;

// FroxelParams::indirectMode. Constant: the constant ambient, no grid.
static const uint kFroxelIndirectConstant = 0u;
static const uint kFroxelIndirectDdgi     = 1u;
static const uint kFroxelIndirectRays     = 2u;

// The indirect grid: one cell per kFroxelIndirectCell froxels. A cell's coordinates are continuous,
// cell k spanning [k, k + 1), as the froxels' (froxel i spans [i, i + 1)).
static const uint3 kFroxelIndirectCell = uint3(4, 4, 2);
uint3 FroxelIndirectDims(FroxelParams p)
{
    return (uint3(p.froxelW, p.froxelH, p.froxelD) + kFroxelIndirectCell - 1u) / kFroxelIndirectCell;
}
// The grid at continuous cell coordinates @p c, trilinear between cell centres and clamped at the
// edges.
float4 FroxelIndirectFetch(RWTexture3D<float4> grid, float3 c, uint3 dims)
{
    const float3 t = clamp(c - 0.5, 0.0, float3(dims) - 1.0);
    const uint3 i0 = min(uint3(t), dims - 1u);
    const uint3 i1 = min(i0 + 1u, dims - 1u);
    const float3 f = t - float3(i0);
    float4 v = 0;
    [unroll] for (uint k = 0; k < 8u; ++k)
    {
        const uint3 o = uint3(k & 1u, (k >> 1) & 1u, k >> 2);
        const float3 w = lerp(1.0 - f, f, float3(o));
        v += grid[uint3(o.x ? i1.x : i0.x, o.y ? i1.y : i0.y, o.z ? i1.z : i0.z)] * (w.x * w.y * w.z);
    }
    return v;
}
#endif

// Global medium extinction at a world position: constant below
// heightFogStart, exponential falloff above it.
float GlobalFogExtinction(FroxelParams p, float3 worldPos)
{
    float h = max(worldPos.y - p.heightFogStart, 0.0);
    return p.fogDensity * exp(-h * p.heightFogFalloff);
}

// Slice index <-> view-space depth (exponential distribution):
//   z=0   -> froxelNear
//   z=D-1 -> froxelFar
float FroxelSliceToViewDepth(float slice, float zNear, float zFar, uint depth)
{
    float t = slice / float(depth);
    return zNear * pow(zFar / zNear, t);
}

float ViewDepthToFroxelSlice(float depth, float zNear, float zFar, uint depthSlices)
{
    return log(depth / zNear) / log(zFar / zNear) * float(depthSlices);
}

// Per-frame sub-voxel jitter, indexed by frameIdx & 15. Sampling a slightly
// different position inside the voxel each frame anti-aliases cone edges and
// shadow boundaries; with temporal reprojection it converges to smooth gradients
// instead of hard rings.
float3 GetFroxelJitter(uint frameIdx)
{
    // 16-tap Halton-(2,3,5) low-discrepancy sequence over the [-0.5, +0.5] cube, a
    // full-voxel range: over the 16-frame cycle each voxel's time-averaged sample
    // integrates its whole volume (a per-voxel box filter), which kills cone-edge
    // aliasing. The full range relies on the temporal pass reprojecting exactly
    // (row-vector, transposed matrices).
    static const float3 kJitter[16] = {
        float3( 0.0000,  0.0000,  0.0000),
        float3(-0.2500,  0.3333, -0.2000),
        float3( 0.2500, -0.3333,  0.2000),
        float3(-0.3750, -0.1111,  0.4000),
        float3( 0.3750,  0.1111, -0.4000),
        float3(-0.1250,  0.4444,  0.1000),
        float3( 0.1250, -0.4444, -0.1000),
        float3(-0.4375, -0.2222,  0.3000),
        float3( 0.4375,  0.2222, -0.3000),
        float3(-0.1875,  0.1111, -0.4400),
        float3( 0.1875, -0.1111,  0.4400),
        float3(-0.3125,  0.4999, -0.1600),
        float3( 0.3125, -0.4999,  0.1600),
        float3(-0.0625, -0.3888,  0.3600),
        float3( 0.0625,  0.3888, -0.3600),
        float3(-0.4999,  0.1666,  0.4800)
    };
    return kJitter[frameIdx & 15u];
}

// Froxel index -> world-space position. `jitter` is in voxel space ([-0.5, +0.5]
// per axis) and offsets the sample inside the voxel for temporal supersampling;
// FroxelToWorld(id, p) gives the un-jittered centre.
float3 FroxelToWorldJittered(uint3 id, FroxelParams p, float3 jitter)
{
    float2 uv     = (float2(id.xy) + 0.5 + jitter.xy) / float2(p.froxelW, p.froxelH);
    // NDC: x [-1, +1], y [+1, -1] (DX top-left origin), z [0, 1]
    float2 ndc    = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float  viewZ  = FroxelSliceToViewDepth(float(id.z) + 0.5 + jitter.z,
                                            p.froxelNear, p.froxelFar, p.froxelD);

    // Reversed-Z: near plane at NDC z=1, far plane at NDC z=0.
    float4 nearH = mul(float4(ndc, 1.0, 1.0), p.invViewProj);
    float4 farH  = mul(float4(ndc, 0.0, 1.0), p.invViewProj);
    float3 nearW = nearH.xyz / nearH.w;
    float3 farW  = farH.xyz  / farH.w;

    float3 dir   = normalize(farW - nearW);
    return p.cameraPos + dir * viewZ;
}

float3 FroxelToWorld(uint3 id, FroxelParams p)
{
    return FroxelToWorldJittered(id, p, float3(0, 0, 0));
}

// World-space position -> UVW in the grids laid out by FroxelToWorld (density, lighting), for
// trilinear sampling: the slice of its distance to the camera. Not for the scattering volume,
// whose slices are planar depth (Scatter).
float3 WorldToFroxelUVW(float3 worldPos, FroxelParams p)
{
    float4 clip   = mul(float4(worldPos, 1.0), p.viewProj);
    if (clip.w <= 0.0) return float3(0, 0, 0);  // behind the camera
    float3 ndc    = clip.xyz / clip.w;
    float2 uv     = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    float  slice  = ViewDepthToFroxelSlice(length(worldPos - p.cameraPos), p.froxelNear, p.froxelFar, p.froxelD);
    return float3(uv, slice / float(p.froxelD));
}

// Henyey-Greenstein phase function (forward / backward scattering):
//   g ->  1: strongly forward (Mie)
//   g ->  0: isotropic
//   g -> -1: strongly backward
float PhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float den = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI * pow(max(den, 1e-3), 1.5));
}

#endif // FROXEL_COMMON_HLSLI
