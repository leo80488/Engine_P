// DDGIRayTrace.cs.hlsl - DDGI probe ray trace: inline RayQuery (DXR 1.1,
// SM 6.5+) in one compute shader, no RTPSO or shader binding table.
//
// Dispatch: numthreads(DDGI_TRACE_GROUP_SIZE,1,1) via ExecuteIndirect with args
// written by DDGIFinalizeIndirect.cs. ONE FLAT INDEX over every ray allocated
// this frame:
//   linearIdx = DTid.x + DTid.y * DDGI_MAX_GROUPS_X * DDGI_TRACE_GROUP_SIZE
//   (probeIdx, rayIdx) is then unpacked from g_RayAlloc[1 + linearIdx].
// The Y dimension only exceeds 1 past 65535 groups (DDGICommon.hlsli).
//
// Output: g_RayData[probeIdx * raysPerProbe + rayIdx] = float4(radiance, dist),
// the layout DDGIRelight reads.
//
// Explicit mip for the bounce-albedo fetch. An inline RayQuery has no ray
// differentials, and a diffuse bounce wants the mean albedo over the footprint
// rather than a point sample, so a fixed coarse level is both cheaper and more
// correct than mip 0. 4 = 1/16 linear scale; raise it if probe-scale colour
// bleeding tracks fine texture detail it should not, lower it if large
// flat-coloured surfaces read as washed out.
#define DDGI_BOUNCE_ALBEDO_MIP 4.0

// Emissive gets the same footprint-mean treatment as the bounce albedo, for a
// stronger reason: emissive is the one term added to the ray OUTSIDE the BRDF, at
// authored strengths of ~10 against night-scene mean irradiance of ~0.01-0.1. At
// mip 0 a probe ray over a neon sign is a per-frame texel lottery - a lit letter
// this frame (clamped to 5.0 by the firefly cap), the dark background next - a
// Bernoulli spike train ~50-100x the mean that the relight EMA turns into slow
// dark/bright breathing, never settling (the dominant night-scene DDGI
// instability). The mip carries the sign's footprint-MEAN emission instead: the
// same expected energy (mips are box averages), a tiny fraction of the variance.
#define DDGI_EMISSIVE_MIP 4.0

// Shading: per-instance albedo + smooth-shaded normal (bindless VB/IB lookup) +
// a shadow ray per light (NdotL x visibility).
//
// Multi-bounce: when frameIndex > 0 and the overwrite flag is clear, a hit also
// samples last frame's DDGI of THIS volume (raw: no tint / scale) as extra
// incoming irradiance.
#include "DDGICommon.hlsli"
#include "DDGISampling.hlsli"
#include "cluster_common.hlsli"
#include "world_light_grid.hlsli"
#include "light_eval.hlsli"   // GPULight; the single punctual-falloff authority

// ---- Bindings ----
// Root signature: DDGIPass.h.
//   b0 space0 - DDGIVolumeGPU
//   t0 space0 - TLAS
//   t1 space0 - sky IBL cube
//   u0 space0 - RayData (output)
//   t3 space0 - per-instance data
//   t4 space0 - irradiance atlas SRV   (multi-bounce read)
//   t5 space0 - depth atlas SRV        (multi-bounce read)
//   t6 space0 - probe data SRV         (multi-bounce read)
//   t7 space0 - cluster GPULight buffer
//   u2 space0 - per-probe ray count;  u3 space0 - ray allocation
//   t0 space1 - bindless ByteAddressBuffer table (16384 slots)
//   t0 space2 - bindless texture table (16384 slots)
//   s0 space0 - linear sampler
ConstantBuffer<DDGIVolumeGPU>      g_Vol      : register(b0, space0);
RaytracingAccelerationStructure    g_TLAS     : register(t0, space0);
TextureCube<float4>                g_SkyIBL   : register(t1, space0);
RWStructuredBuffer<float4>         g_RayData  : register(u0, space0);

// Per-instance data (128 B): the layout is owned by rt_instance_data.hlsli, the
// ONE HLSL declaration shared with HybridReflIntersectHW / RTShadow / RTAO; the
// C++ mirror is DDGIInstanceData in DDGISceneAS.cpp. Do NOT re-declare it here.
// DDGI's own shading uses baseColor x albedoTexIdx (coarse mip) + emissiveTexIdx;
// the material-proxy fields exist for the reflection tracer but must be present
// so the StructuredBuffer stride matches.
#include "rt_instance_data.hlsli"
typedef RTInstanceData DDGIInstanceData;   // DDGI's name for the shared struct
#define DDGI_MaterialUV RTInstanceUV       // DDGI's name for the UV transform

StructuredBuffer<DDGIInstanceData> g_DDGIInstances : register(t3, space0);

// Multi-bounce source: LAST frame's octahedral irradiance atlas, depth atlas and
// probe data of THIS volume, sampled on hit when g_Vol.frameIndex > 0.
Texture2D<float4>                  g_PrevIrradiance : register(t4, space0);
Texture2D<float2>                  g_PrevDepth      : register(t5, space0);
StructuredBuffer<DDGIProbeData>    g_PrevProbeData  : register(t6, space0);

// Cluster GPULight buffer, the same buffer the rasterizer LightingPass reads.
// Local lights are evaluated deterministically after range/cone rejection.
StructuredBuffer<GPULight>         g_Lights         : register(t7, space0);

// Adaptive ray dispatch, descriptors prepared by DDGIRayAllocation.cs.
// g_RayAlloc[0]   = total rays this frame.
// g_RayAlloc[1..] = packed (probeIdx & 0xFFFFF) | (rayIdx << 20).
// (The dispatch args live in the SEPARATE dispatch-args buffer consumed by
// ExecuteIndirect; they are not interleaved here.)
RWStructuredBuffer<uint>           g_RayCount       : register(u2, space0);
RWStructuredBuffer<uint>           g_RayAlloc       : register(u3, space0);

ByteAddressBuffer g_DDGIBuffers[16384] : register(t0, space1);

// Engine-wide bindless texture table (matches GraphicsDX12::kMaxBindlessTextures
// = 16384). Hits sample albedo, surface-cache and emissive textures through it,
// so glowing surfaces (lamps, neon signs, screens) light up the probes.
Texture2D                          g_AllTextures[16384] : register(t0, space2);

SamplerState                       g_LinearSampler : register(s0, space0);

#define DDGI_VERTEX_STRIDE_BYTES 32
#define DDGI_INDEX_BYTES         4

// DXR instance inclusion mask for probe + shadow rays. Instance masks are
// bit-partitioned by DDGISceneAS: bit0 = reflections/RTAO, bit1 = RT-shadow
// caster, bit2 = DDGI. Bit 2 covers every static instance (visible AND
// hidden-from-main-pass) but NEVER skinned characters: their BLAS deforms
// every frame, which destabilises probe relocation / backface "buried"
// classification and makes probes flicker, for negligible GI contribution.
// RTReflectionTrace/RTAO trace 0x01 and do see them; RTShadow traces 0x02.
#define DDGI_RAY_MASK 0x04u

// ---- Helpers ----
// LoadHitTriangleAttributes returns position + normal + UV per triangle vertex;
// false (with defaults) when the instance has no bindless registration.
// Index fetch: primitives use R16_UINT (indexBytes == 2), MeshLibrary R32.
// ByteAddressBuffer loads are 4-byte aligned, so the 16-bit half is picked
// manually.
uint DDGI_LoadIndex(uint ibIdx, uint i, uint indexBytes)
{
    if (indexBytes == 2u)
    {
        const uint byteOff = i * 2u;
        const uint word    = g_DDGIBuffers[ibIdx].Load(byteOff & ~3u);
        return (byteOff & 2u) ? (word >> 16) : (word & 0xFFFFu);
    }
    return g_DDGIBuffers[ibIdx].Load(i * 4u);
}

bool LoadHitTriangleAttributes(uint instId, uint primIdx,
                               out float3 p0, out float3 p1, out float3 p2,
                               out float3 n0, out float3 n1, out float3 n2,
                               out float2 uv0, out float2 uv1, out float2 uv2)
{
    DDGIInstanceData inst = g_DDGIInstances[instId];
    p0 = p1 = p2 = float3(0, 0, 0);
    n0 = n1 = n2 = float3(0, 1, 0);
    uv0 = uv1 = uv2 = float2(0, 0);
    if (inst.vbBindlessIdx == 0xFFFFFFFFu || inst.ibBindlessIdx == 0xFFFFFFFFu)
        return false;

    const uint vbIdx = NonUniformResourceIndex(inst.vbBindlessIdx);
    const uint ibIdx = NonUniformResourceIndex(inst.ibBindlessIdx);

    const uint indexBytes = (inst.indexBytes == 2u) ? 2u : 4u;
    uint i0 = DDGI_LoadIndex(ibIdx, inst.indexStart + primIdx * 3 + 0, indexBytes);
    uint i1 = DDGI_LoadIndex(ibIdx, inst.indexStart + primIdx * 3 + 1, indexBytes);
    uint i2 = DDGI_LoadIndex(ibIdx, inst.indexStart + primIdx * 3 + 2, indexBytes);

    if (inst.normalBufIdx != 0xFFFFFFFFu)
    {
        // De-interleaved streams (procedural primitives + skinned meshes):
        // vb = positions (float3, stride 12), separate normal (float3) +
        // uv0 (float2) streams, each rebased by its per-instance element base
        // (vertexStart = position base; all bases are 0 for primitives).
        const uint nbIdx = NonUniformResourceIndex(inst.normalBufIdx);
        p0 = asfloat(g_DDGIBuffers[vbIdx].Load3((inst.vertexStart + i0) * 12u));
        p1 = asfloat(g_DDGIBuffers[vbIdx].Load3((inst.vertexStart + i1) * 12u));
        p2 = asfloat(g_DDGIBuffers[vbIdx].Load3((inst.vertexStart + i2) * 12u));
        n0 = asfloat(g_DDGIBuffers[nbIdx].Load3((inst.nrmStart + i0) * 12u));
        n1 = asfloat(g_DDGIBuffers[nbIdx].Load3((inst.nrmStart + i1) * 12u));
        n2 = asfloat(g_DDGIBuffers[nbIdx].Load3((inst.nrmStart + i2) * 12u));
        if (inst.uvBufIdx != 0xFFFFFFFFu)
        {
            const uint uvbIdx = NonUniformResourceIndex(inst.uvBufIdx);
            uv0 = asfloat(g_DDGIBuffers[uvbIdx].Load2((inst.uvStart + i0) * 8u));
            uv1 = asfloat(g_DDGIBuffers[uvbIdx].Load2((inst.uvStart + i1) * 8u));
            uv2 = asfloat(g_DDGIBuffers[uvbIdx].Load2((inst.uvStart + i2) * 8u));
        }
        return true;
    }

    // Per-instance stride: must match the BLAS build's stride (DDGIInstanceData).
    // Guard against a zero from an unset slot.
    const uint stride = max(inst.vertexStride, DDGI_VERTEX_STRIDE_BYTES);
    uint v0Off = i0 * stride;
    uint v1Off = i1 * stride;
    uint v2Off = i2 * stride;
    p0 = asfloat(g_DDGIBuffers[vbIdx].Load3(v0Off + 0));
    p1 = asfloat(g_DDGIBuffers[vbIdx].Load3(v1Off + 0));
    p2 = asfloat(g_DDGIBuffers[vbIdx].Load3(v2Off + 0));
    n0 = asfloat(g_DDGIBuffers[vbIdx].Load3(v0Off + 12));
    n1 = asfloat(g_DDGIBuffers[vbIdx].Load3(v1Off + 12));
    n2 = asfloat(g_DDGIBuffers[vbIdx].Load3(v2Off + 12));
    // UV at offset 24 (after pos[0..11] + nrm[12..23]).
    uv0 = asfloat(g_DDGIBuffers[vbIdx].Load2(v0Off + 24));
    uv1 = asfloat(g_DDGIBuffers[vbIdx].Load2(v1Off + 24));
    uv2 = asfloat(g_DDGIBuffers[vbIdx].Load2(v2Off + 24));
    return true;
}

// Probe-ray range: ONE constant for the primary ray and every shadow ray it
// spawns. A hit found within the primary range cannot be shadowed by an occluder
// the primary ray could never have reached, so a longer shadow range buys nothing
// and multiplies the traversal of the frame's most numerous ray type.
//
// Before raising it read the TDR note at the ray.TMax assignment: probe-ray cost
// scales linearly with probe count, and this dispatch can exceed Windows'
// 2-second TDR limit.
#define DDGI_RAY_TMAX 50.0

// Inline shadow ray with parameterised range. Returns 1 = light visible, 0 = blocked.
float ShadowVisibility(float3 origin, float3 dir, float maxDist)
{
    RayDesc r;
    r.Origin    = origin + dir * 0.05; // bias to avoid self-hit
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
           | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, DDGI_RAY_MASK, r);
    // DDGI deliberately keeps RAY_FLAG_FORCE_OPAQUE although alpha-clip instances
    // carry D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE: probe classification is
    // driven by the committed-BACKFACE ratio, and letting rays through foliage would
    // push probes across the relocate CS's 25%/10% hysteresis and re-open the
    // light-leak vector that classifier exists to close. The ray flag outranks the
    // instance flag, so traversal resolves everything in fixed function and this loop
    // body is unreachable; it exists so a flag-precedence surprise degrades to opaque
    // behaviour instead of reporting MISS through every alpha-clip surface.
    [loop] while (q.Proceed()) q.CommitNonOpaqueTriangleHit();
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// PCG hash (HashPcg). Local lights are evaluated deterministically, so nothing in
// this file draws from it.
#include "core/hash.hlsli"

// Evaluate one cluster GPULight at a surface point. Returns the irradiance
// arriving at P (without the Lambertian 1/PI factor: the caller folds that in
// along with the albedo).
float3 EvalLight(GPULight light, float3 P, float3 N)
{
    float3 lightCol = light.color * light.intensity;
    float3 L        = float3(0, 0, 0);
    float  dist     = 0.0;

    if (light.type == 0u)
    {
        // Directional: the stored direction is from-light (same convention as
        // LightCB.lightDir); negate it for the surface-to-light direction.
        L    = -normalize(light.direction);
        // Shadow-ray range tied to the primary ray's (DDGI_RAY_TMAX). An unbounded range
        // makes every directional shadow ray on a sky-facing surface traverse the whole
        // TLAS before missing, which at scale (>= 2048 probes x 64 rays in a 3000+
        // instance scene) exceeds Windows' 2-second TDR limit (DXGI_ERROR_DEVICE_HUNG).
        //
        // This branch is DEAD in this shader: the local-light loop skips type == 0
        // because EvalDirectionalSun owns the sun. It is kept correct so the two
        // evaluators read side by side.
        dist = DDGI_RAY_TMAX;
    }
    else
    {
        // POINT and SPOT: EvalLocalLightAttenuation folds the cone in itself and returns
        // exactly 1 for a non-spot. Keep this a bare `else`, not `else if (type == 1u)`:
        // a type-2 light falling through would reach the NdotL / ShadowVisibility code
        // below with L and dist unassigned.
        //
        // The falloff and spot cone are the deferred path's (light_eval.hlsli: inverse
        // square with a max(d*d, 0.01) floor), so a lamp's indirect bounce falls off on
        // the same curve as its direct term. Near the lamp that is bright: a surface
        // 10 cm away receives ~100x its value at 1 m, and a lamp centimetres from a
        // surface blows out - as it does in direct lighting.
        float3 d = light.position - P;
        dist = length(d);
        if (dist > light.radius) return float3(0, 0, 0);
        L = d / max(dist, 1e-4);
        lightCol *= EvalLocalLightAttenuation(light, L, dist);
    }

    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0, 0, 0);

    if (!any(lightCol > 0.0)) return float3(0, 0, 0);
    float vis = ShadowVisibility(P, L, dist);
    return lightCol * NdotL * vis;
}

// Deterministic directional-sun evaluator. `dirFromLight` uses the same
// from-light convention as LightCB.lightDir / GPULight.direction; `radiance`
// is the sun colour already premultiplied by intensity (== LightCB.lightColor).
// Returns the irradiance arriving at P (the caller folds in the 1/PI Lambert
// term, exactly as for EvalLight). Mirrors the directional branch of EvalLight,
// including its DDGI_RAY_TMAX shadow-ray range. A zero radiance (no sun, or an
// intensity-0 directional in the Unreal-style "no sun -> no light" convention)
// yields zero contribution and skips the shadow ray.
//
// This is the hottest shadow ray in the engine: it fires on EVERY frontface
// probe-ray hit whose normal faces the sun, so its range multiplies straight
// into the frame's total traversal work.
float3 EvalDirectionalSun(float3 dirFromLight, float3 radiance, float3 P, float3 N)
{
    if (dot(radiance, radiance) <= 0.0) return float3(0, 0, 0);
    float3 L = -normalize(dirFromLight);
    float  NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0, 0, 0);
    float vis = ShadowVisibility(P, L, DDGI_RAY_TMAX);
    return radiance * NdotL * vis;
}

// ---- Entry ----
[numthreads(DDGI_TRACE_GROUP_SIZE, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    // Adaptive dispatch: a single 1D index over all rays this frame; decode the
    // (probeIdx, rayIdx) pair from the packed descriptor. The direction comes from
    // DDGI_AdaptiveSampleIdx(rayIdx, adaptiveRays) below, which the relight CS
    // reconstructs from the same inputs.
    // 2D fold: gy is 1 (so DTid.y == 0) unless this frame's ray count needed
    // more than DDGI_MAX_GROUPS_X groups. The row stride MUST equal the X
    // extent DDGIFinalizeIndirect actually dispatched, which is exactly
    // DDGI_MAX_GROUPS_X groups of DDGI_TRACE_GROUP_SIZE threads whenever
    // gy > 1. Both constants live in DDGICommon.hlsli so they cannot drift.
    const uint linearIdx  = DTid.x + DTid.y * (DDGI_MAX_GROUPS_X * DDGI_TRACE_GROUP_SIZE);
    const uint totalRays  = g_RayAlloc[0];
    if (linearIdx >= totalRays) return;

    const uint packed     = g_RayAlloc[1 + linearIdx];
    const uint probeIdx   = packed & 0xFFFFFu;
    const uint rayIdx     = packed >> 20u;
    // adaptiveRays must equal the relight's: both derive the ray direction from
    // DDGI_AdaptiveSampleIdx(rayIdx, adaptiveRays) (Halton(2,3), prefix-uniform;
    // DDGICommon.hlsli). g_RayAlloc already holds only valid (probeIdx, rayIdx) pairs.
    const uint adaptiveRays  = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;

    // Probe origin: base grid + the relocation offset from the previous frame's
    // probe relocation CS. Without the offset, probes the relocation pushed out of
    // nearby walls would still TRACE from inside the wall, producing degenerate
    // close-range hits that re-trigger relocation in a feedback loop. Sampling uses
    // pd.offset too, so the two ends stay consistent.
    int3   coord    = DDGI_ProbeCoord(probeIdx, g_Vol);
    float3 probePos = DDGI_ProbeWorldPos(coord, g_Vol);
    {
        DDGIProbeData pd = g_PrevProbeData[probeIdx];
        probePos += pd.offset;
    }

    // Direction = Halton(2,3) x per-frame random rotation. The adaptive sample index
    // spreads this probe's adaptiveRays rays across the volume's full Halton
    // sequence and the rotation changes per frame, so the EMA reaches the whole
    // sphere (DDGICommon.hlsli explains why a naive prefix is not enough).
    const uint sampleIdx = DDGI_AdaptiveSampleIdx(rayIdx, adaptiveRays, g_Vol);
    float3 dirLocal = DDGI_HaltonSphere(sampleIdx);
    float3 rayDir   = DDGI_ApplyRandomRotation(dirLocal, g_Vol);

    RayDesc ray;
    ray.Origin    = probePos;
    ray.Direction = rayDir;
    // TMin must stay tiny. Probes are not surface points (no self-intersection to
    // dodge), and a large TMin (e.g. 0.05) TUNNELS through any surface closer than it:
    // a probe on the floor plane (a whole grid layer often lands there) would skip
    // the floor's top face, travel inside the slab and commit its BOTTOM face as a
    // backface -> >= 25% backface -> INACTIVE on frame one, with no close frontface
    // signal for relocation to escape with.
    ray.TMin      = 0.001;
    // Trace TMax (DDGI_RAY_TMAX). With thousands of BLAS instances each ray
    // worst-cases hundreds of intersection tests, and the dispatch cost scales
    // linearly with ray count: a range of 200 exceeded Windows' 2-second TDR limit
    // (DXGI_ERROR_DEVICE_HUNG) at 16x8x16 = 2048 probes while 16x7x16 = 1792 ran.
    //
    // 50 keeps coverage well past any reasonable single-volume extent while bounding
    // the BLAS visits per ray; beyond ~50 units most indirect contribution is already
    // attenuated through several bounces. Raise it for very large outdoor scenes only
    // after profiling rules out a TDR - it also lengthens every shadow ray, the more
    // numerous of the two.
    ray.TMax      = DDGI_RAY_TMAX;

    // Backface culling is OFF on purpose: backface hits reveal probes buried in solid
    // geometry or on the wrong side of a thin wall (the indoor/outdoor leak vector).
    // With culling those rays pass through walls into the sky and pollute the probe
    // with whatever is on the other side. Without it they report
    // CommittedTriangleFrontFace() = false and take the "this direction is buried"
    // path (zero radiance, backface-sentinel hitDistance) read by Relight's depth
    // atlas and Relocate's probe classification.
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, DDGI_RAY_MASK, ray);
    // FORCE_OPAQUE is kept on purpose (see ShadowVisibility). Defensive candidate
    // loop, unreachable while that holds.
    [loop] while (q.Proceed()) q.CommitNonOpaqueTriangleHit();

    float3 radiance    = float3(0, 0, 0);
    float  hitDistance = -1.0;

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        // ---- Miss ----
        // x iblStrength (g_Vol.skyScale), the scale every other consumer of this cube
        // applies; without it an iblStrength-4 scene's probes see a quarter of the path
        // tracer's sky.
        float3 sky = g_SkyIBL.SampleLevel(g_LinearSampler, rayDir, 0).rgb * g_Vol.skyScale;
        radiance    = min(sky, DDGI_FireflyCap(10.0, g_Vol.sunColor));
        hitDistance = -1.0;
    }
    else
    {
        // ---- Triangle hit: facing decided from SURFACE NORMALS ----
        // CommittedTriangleFrontFace() trusts triangle WINDING, which is unreliable in
        // authored content: geometry mirrored inside the DCC bakes the flip into the mesh
        // (identity transform, so the TLAS instance winding flag cannot correct it), and
        // meshes authored for cull-off rendering carry arbitrary winding. The raster
        // pipeline lights surfaces by their VERTEX NORMALS, so those are the ground truth
        // for which side is visible; a winding-based test disagreeing with them turns
        // whole regions of valid floor into "buried" (radiance 0 + INACTIVE): probe-shaped
        // black patches on the mirrored half of symmetric scenes.
        //
        // Facing rule: the world-space GEOMETRIC normal (exact for the triangle, stable at
        // silhouettes) ORIENTED by the smooth vertex normal (winding-independent). Winding
        // is the fallback only when the mesh has no bindless attributes or degenerate
        // normals.
        const uint  instId    = q.CommittedInstanceID();
        const uint  primIdx   = q.CommittedPrimitiveIndex();
        const float t         = q.CommittedRayT();
        const float2 bary     = q.CommittedTriangleBarycentrics();
        const float3x4 obj2w  = q.CommittedObjectToWorld3x4();

        DDGIInstanceData inst = g_DDGIInstances[instId];
        float3 albedo   = inst.baseColor.rgb;
        float3 emissive = inst.emissive.rgb;

        float3 hitPos = ray.Origin + ray.Direction * t;

        // Smooth-shaded normal + interpolated UV via bindless triangle
        // attribute fetch, plus the winding-independent facing decision.
        float3 worldN;
        float2 hitUV = float2(0, 0);
        bool   hasUV = false;
        bool   frontFace;
        {
            float3 p0, p1, p2, n0, n1, n2;
            float2 uv0, uv1, uv2;
            bool ok = LoadHitTriangleAttributes(instId, primIdx,
                                                p0, p1, p2, n0, n1, n2,
                                                uv0, uv1, uv2);
            if (ok)
            {
                float u = bary.x;
                float v = bary.y;
                float w = 1.0 - u - v;
                float3 nObj    = n0 * w + n1 * u + n2 * v;
                bool   nValid  = !any(isnan(nObj)) && dot(nObj, nObj) > 1e-4;

                // World-space triangle edges -> exact geometric normal (its sign is still
                // winding-dependent until oriented below).
                float3 w0 = mul(obj2w, float4(p0, 1.0));
                float3 w1 = mul(obj2w, float4(p1, 1.0));
                float3 w2 = mul(obj2w, float4(p2, 1.0));
                float3 gN = cross(w1 - w0, w2 - w0);

                if (nValid)
                {
                    worldN = normalize(mul((float3x3)obj2w, normalize(nObj)));
                    // Orient the geometric normal to agree with the smooth
                    // normal, then classify against the ray. Winding never
                    // enters the decision.
                    if (dot(gN, worldN) < 0.0) gN = -gN;
                    frontFace = dot(gN, rayDir) < 0.0;
                }
                else
                {
                    // Degenerate vertex normals: no orientation reference; fall back to the winding
                    // the BLAS reports.
                    frontFace = q.CommittedTriangleFrontFace();
                    worldN = normalize(gN);
                    if (dot(worldN, rayDir) > 0.0) worldN = -worldN;
                }
                hitUV  = uv0 * w + uv1 * u + uv2 * v;
                hasUV  = true;
            }
            else
            {
                frontFace = q.CommittedTriangleFrontFace();
                worldN = -normalize(rayDir);
            }
        }

        if (!frontFace)
        {
            // ---- Backface hit ----
            // The probe is on the SOLID side of this surface along this ray. Do NOT integrate
            // what is behind the wall: that is how outdoor sky leaks into indoor probes
            // through thin walls.
            //
            // Encoding contract (read by DDGIRelight.cs / DDGIProbeRelocate.cs):
            //   hitDistance > 0       : frontface hit at this distance
            //   hitDistance == -1.0   : miss (no hit)
            //   hitDistance < -1.5    : backface hit, real t = -(hitDistance + 2.0)
            //
            // The -2.0 offset keeps backface values strictly below -1.5, so the miss sentinel
            // (-1.0) sits in an unambiguous gap, while the distance to the wall is preserved
            // for Chebyshev (the depth atlas records the wall as a near occluder).
            g_RayData[probeIdx * g_Vol.raysPerProbe + rayIdx] =
                float4(0.0, 0.0, 0.0, -(t + 2.0));
            return;
        }

        // ---- Material surface cache ----
        // A cached instance carries baseColor = WHITE: the page already contains
        // baseColor x texture, and the hybrid-reflection path multiplies the two
        // (DDGISceneAS::fillMaterialProxy). Without the multiply below every cached
        // instance would bounce at albedo 1.0, the brightest legal value, into the
        // multi-bounce feedback loop.
        //
        // Gated on the flag, not on `albedoTexIdx >= 0`: a non-cached instance's
        // baseColor is its authored tint, and it takes the coarse-mip albedo branch below.
        if ((inst.rtFlags & RT_INST_FLAG_SURFACE_CACHE) != 0u)
        {
            // Emissive mask from the page's surface alpha, as HybridReflIntersectHW does: a
            // cached instance has emissiveTexIdx = -1, which means "do not multiply", not
            // black, so a material authored with emissiveColor (1,1,1,1) would become a
            // full-strength white EMITTER in the probe field - a GI blowout.
            //
            // Fail to zero, like the RTR path: when the page is unreachable (no UV, unbound
            // page) the answer is "no emissive", never "full white emitter".
            float cacheEmisMask = 0.0;
            if (hasUV)
            {
                const float2 pageUV = DDGI_MaterialUV(inst, hitUV);
                if (inst.albedoTexIdx >= 0)
                {
                    // ALBEDO_RAW: the albedo slot holds the material's own FULL-RES texture (raw
                    // mesh UV; baseColor stays the authored tint) instead of the page. Sample it like
                    // the non-cached branch below, at the coarse bounce mip, so incoherent probe rays
                    // do not thrash the texture cache.
                    const bool rawAlb =
                        (inst.rtFlags & RT_INST_FLAG_ALBEDO_RAW) != 0u;
                    const uint aIdx = NonUniformResourceIndex((uint)inst.albedoTexIdx);
                    albedo *= g_AllTextures[aIdx]
                        .SampleLevel(g_LinearSampler,
                                     rawAlb ? hitUV : pageUV,
                                     rawAlb ? DDGI_BOUNCE_ALBEDO_MIP : 0).rgb;
                }
                if (inst.surfaceTexIdx >= 0)
                {
                    const uint sIdx = NonUniformResourceIndex((uint)inst.surfaceTexIdx);
                    cacheEmisMask = g_AllTextures[sIdx]
                        .SampleLevel(g_LinearSampler, pageUV, 0).a;
                }
            }
            emissive *= cacheEmisMask;
        }
        else if (hasUV && inst.albedoTexIdx >= 0)
        {
            // ---- Bounce albedo from the albedo texture ----
            // A non-cached instance's baseColor is only the authored tint: the albedo map
            // carries the colour (a white-tinted wall with a red map bounces red light) and,
            // being almost always darker than 1, keeps the bounce from injecting more energy
            // than the surface reflects into the multi-bounce loop.
            //
            // Coarse mip (DDGI_BOUNCE_ALBEDO_MIP), deliberately:
            //   1. Correctness: a diffuse bounce wants the AVERAGE albedo over the ray's
            //      footprint, not the texel it happened to land on, and an inline RayQuery
            //      has no ray differentials to compute that footprint.
            //   2. Cost: this runs on every hit of every probe ray (order 10^6 per frame)
            //      and those rays are incoherent; point-sampling mip 0 of full-res maps is
            //      near the worst case for the texture cache, in the frame's most expensive
            //      GPU region.
            // The surface-cache branch above samples its page at mip 0: a page is a small
            // atlas rect already at roughly the right scale.
            const uint aIdx = NonUniformResourceIndex((uint)inst.albedoTexIdx);
            albedo *= g_AllTextures[aIdx]
                .SampleLevel(g_LinearSampler, DDGI_MaterialUV(inst, hitUV),
                             DDGI_BOUNCE_ALBEDO_MIP).rgb;
        }

        // Emissive texture modulation (GBuffer.ps convention:
        // emissiveOut = (emissiveColor.rgb * w) * emissiveTex.rgb). The constant part is
        // pre-multiplied on the CPU; here it is multiplied by the texture at the
        // interpolated UV. Cached instances arrive with emissiveTexIdx = -1 (the
        // material's own emissive map cannot be sampled through a page rect), so this is
        // a no-op for them and the constant emissive carries through.
        if (inst.emissiveTexIdx >= 0 && hasUV)
        {
            const uint texIdx = NonUniformResourceIndex((uint)inst.emissiveTexIdx);
            // Coarse mip on purpose (DDGI_EMISSIVE_MIP): mip 0 is a per-frame texel
            // lottery, the dominant night-scene noise source.
            float3 emTex = g_AllTextures[texIdx]
                .SampleLevel(g_LinearSampler, DDGI_MaterialUV(inst, hitUV),
                             DDGI_EMISSIVE_MIP).rgb;
            emissive *= emTex;
        }

        // Direct lighting: directional sun + cluster point/spot lights.
        //
        // The directional sun has a SINGLE source of truth (g_Vol.sunDirection /
        // sunColor, mirrored from LightCB by the CPU), evaluated deterministically for
        // every ray exactly like the deferred Lighting.ps, which also reads the sun from
        // LightCB and skips type == 0 entries in its cluster loop. DDGI's sun therefore
        // matches direct lighting and follows the time-of-day cycle whether or not the
        // sun is a tagged SunLightTag entity (the cluster buffer never sees the
        // time-of-day override).
        float3 directLit = EvalDirectionalSun(g_Vol.sunDirection, g_Vol.sunColor,
                                              hitPos, worldN);

        // Evaluate local lights deterministically: uniformly picking one of hundreds of
        // lights and multiplying by the list size produces rare bright outliers the
        // temporal filter cannot tell from real lighting changes. Range, cone and cosine
        // rejection happen before the visibility trace, so only lights contributing at
        // this hit cast rays.
        uint lightOffset;
        uint candidateCount = WorldLightRange(g_Vol.worldLightGridIndex, hitPos, g_Vol.lightCount, lightOffset);
        [loop] for (uint ci = 0u; ci < candidateCount; ++ci)
        {
            uint li = WorldLightIndex(g_Vol.worldLightGridIndex, lightOffset, ci);
            GPULight light = g_Lights[li];
            if (light.type != 0u)
                directLit += EvalLight(light, hitPos, worldN);
        }
        // `directLit` and the converted multi-bounce sample (prevSafe x PI) are both
        // irradiance (energy per area arriving at the surface); the Lambertian outgoing
        // radiance is (albedo / pi) x total irradiance, one pi divide.
        //
        // Do not add a constant ambient pedestal as a multi-bounce bootstrap: it injects
        // energy from nowhere (a scene with no lights and a black sky converges to lit
        // probes, amplified by the bounce series and by any authored ddgiDiffuseScale
        // above the physical default 1 - the probes store E/pi). With no light the probes
        // must converge to black. The bootstrap is real signal only (sun / local lights
        // on hit, sky IBL on miss, emissive): enclosed scenes converge a few frames
        // slower, dark scenes stay dark.
        float3 incomingIrradiance = directLit;

        // Multi-bounce gate, both conditions must hold:
        //   (1) frameIndex > 0 - there is a previous frame's atlas to read;
        //   (2) the overwrite flag (bit 3) is clear - the relight has done at least one
        //       full overwrite of the atlas, so this reads real radiance, not
        //       uninitialised GPU memory.
        // Without (2) the first trace reads whatever bit pattern the new atlas holds,
        // the relight blends it in, the next trace re-reads the poisoned atlas and the
        // geometric series locks the probes onto the garbage. DDGIRelight's write-site
        // sanitise is the second line of defence.
        if (g_Vol.frameIndex > 0 && (g_Vol.flags & 8u) == 0u)
        {
            float3 viewDir = -rayDir;
            // RAW sample: per-volume diffuseTint/diffuseScale are display knobs and must NOT
            // enter the bounce feedback loop (they would multiply once per bounce and can
            // push the series gain past 1 - brightness runaway).
            float4 prev = DDGI_SampleVolumeRaw(hitPos, worldN, viewDir, g_Vol,
                                               g_PrevIrradiance, g_PrevDepth,
                                               g_PrevProbeData, g_LinearSampler);
            // Physical multi-bounce. The sample is E(N)/pi (the atlas convention,
            // DDGICommon.hlsli) and `incomingIrradiance` is in E units (EvalLight /
            // EvalDirectionalSun), so the conversion is x pi and the feedback gain is 1.0:
            // this loop is the Jacobi iteration of the rendering equation, each round trip
            // already attenuated by albedo x (1/pi) x the cosine integral. The series gain
            // (~ scene albedo < 1) converges; a white room converges slowly and gets bright -
            // that is radiosity, not a bug. Do not bake a damping constant here; a scene that
            // needs one gets an authored knob.
            //
            // The firefly cap works in SAMPLE space (E/pi), before the x pi on purpose: its
            // 3.0 floor was measured there (legit values inside a volume are < 1.5), ~9.4 in
            // E units. DDGI_FireflyCap raises it with the light level: at sun 25 the physical
            // steady state alone is 7.96, so a fixed 3.0 would truncate the feedback loop.
            float3 prevSafe = min(prev.rgb * prev.a, DDGI_FireflyCap(3.0, g_Vol.sunColor));
            incomingIrradiance += prevSafe * PI;
        }

        // Outgoing radiance = self-emission + Lambertian reflection. Emissive is added
        // DIRECTLY (independent of incoming light) so glowing surfaces (lamps, neon
        // signs, screens) reach the probes and bleed colour onto nearby walls. Mirrors
        // GBuffer.ps's "emissive bypasses the BRDF" convention, so direct and indirect
        // render the same emissive intensity.
        //
        // Perceived strength: the probe grid samples lighting at discrete points, so an
        // emissive surface of small solid angle (a lamp shade, a neon sign) registers
        // only on probes close enough for it to fill a meaningful part of the sphere.
        // Densify the probe grid to extend the bleed range; a typical lamp of radius 0.5
        // units reaches probes within ~1-2 units before its contribution drops below the
        // 64-rays-per-probe noise floor.
        radiance    = emissive + albedo * incomingIrradiance * (1.0 / 3.14159265);
        // Firefly cap with a 5.0 floor: a few rays returning radiance > 5 each frame
        // saturate primary colours in the probes, and the EMA never fully discards them.
        // The cap also bounds emissive (a (10, 0, 0) red sign clamps to 5.0 R, plenty for
        // indirect diffuse). DDGI_FireflyCap raises the floor with the light level: a
        // fixed 5.0 eats ~37% of the legitimate first-bounce energy in a sunlit scene.
        radiance    = min(radiance, DDGI_FireflyCap(5.0, g_Vol.sunColor));
        hitDistance = t;
    }

    // Hard NaN/Inf scrub before writing to the ray-data buffer. The min() caps above
    // are not a reliable NaN guard, so any NaN source in this shader (degenerate
    // triangle attributes feeding worldN, multi-bounce feedback amplifying a
    // previous-frame probe corruption, a Chebyshev division by near zero in
    // DDGI_SampleVolumeRaw) could enter the persistent probe state and never leave.
    // Zeroing it at the write boundary breaks the feedback loop; at worst one ray
    // contributes nothing this frame.
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0, 0, 0);

    // Storage layout uses the VOLUME's raysPerProbe as stride (buffer size
    // mirrors that). Adaptive ray count only affects how many indices [0..k)
    // are dispatched this frame; the stride stays at the volume's max.
    g_RayData[probeIdx * g_Vol.raysPerProbe + rayIdx] = float4(radiance, hitDistance);
}
