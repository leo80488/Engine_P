// HybridReflIntersectHW.cs.hlsl - hardware ray-traced intersection stage of the
// hybrid-reflection pipeline (AMD FidelityFX Hybrid-Reflections scheme):
//   ClassifyTiles -> PrepareArgsSW -> IntersectSW (indirect) -> PrepareArgsHW ->
//   IntersectHW (THIS FILE, indirect) -> NRDPack -> [NRD REBLUR] -> NRDResolve
//
// One thread per ray in g_HWRayList (rays the screen-space tier could not resolve). The ray
// is RE-DERIVED deterministically from the same GBuffer inputs and the same
// HR_SampleReflectionDir helper the classifier / SW pass used: identical inputs, identical
// helper, bit-identical direction (AMD HSR deferred-raytracing contract; see
// HybridReflCommon.hlsli).
//
// Hit shading:
//   1) SCREEN-REUSE FIRST: project the hit to the CURRENT frame; if the hit surface is
//      visible on screen (depth-validated), fetch its lit radiance via HR_FetchPrevRadiance
//      (motion-vector fetch into the prev-frame scene-color pyramid, the SAME source the SW
//      tier reads, so RT- and screen-delivered content match by construction).
//   2) Otherwise ANALYTIC material-proxy shading at the hit (HR_ShadeHit): albedo/surface/
//      emissive textures, Cook-Torrance sun (1 shadow ray, mask 0x02) + analytic local
//      lights, DDGI irradiance + sky diffuse ambient, reflection-probe / sky specular via
//      split-sum EnvBRDFApprox, at parity with Lighting.ps.
//   3) Miss with a real sky cube -> sky light at TMax, conf 1 (REBLUR needs the lobe's miss
//      fraction to carry real environment energy).
//   Single-sided backface -> the environment stand-in (no see-through leak).
//
// Estimator: 1 sample/pixel, the raw hit radiance IS the sample - do NOT bake F or the VNDF
// weight into rgb here; NRDResolve/Lighting apply the BRDF magnitude via Fresnel/envBRDF.
//
// Root signature / register map: HybridReflCommon.hlsli (single source of truth). This file
// declares only the registers it touches; t5/t20/t21/s0 are owned by HybridReflFetch.hlsli.

#include "HybridReflCommon.hlsli"
#include "bindless.hlsli"   // typed bindless handles
#include "HybridReflFetch.hlsli"   // gVelocity(t5)/gPrevHDR(t20)/gPrevDepth(t21)/gSampClamp(s0)
#include "DDGICommon.hlsli"
#include "DDGISampling.hlsli"
#include "cluster_common.hlsli"
#include "world_light_grid.hlsli"
#include "light_eval.hlsli"   // SurfaceDistance/AngularAttenuation: the one local-light falloff authority
#include "material_layout.generated.hlsli"  // MATERIAL_GPU_DATA_STRIDE + MAT_OFF_*
                                   // GENERATED from the C++ struct: this shader
                                   // reads the material table as a RAW buffer and
                                   // is the only code that needs byte offsets.
                                   // See HR_LoadNprKnobs below.
#include "brdf.hlsli"              // Cook-Torrance + EnvBRDFApprox (hit shading)
#include "indirect_specular.hlsli"

// Reflection probes at analytic RT hits: the probe buffer is opened from the heap
// (HybridReflConstants.probeBufferIdx), each probe's cubemap by its own index.
#include "reflection_probe.hlsli"
// Prefiltered mip count of every probe cubemap - mirrors Reflection::kProbeCubemapMips
// (ReflectionProbeTypes.h). Keep in sync.
#define HR_PROBE_CUBEMAP_MIPS 7

// ==== Bindings (register map: HybridReflCommon.hlsli) ====
RaytracingAccelerationStructure    g_TLAS          : register(t0, space0);

// Per-instance data (128 B) - layout owned by rt_instance_data.hlsli, the ONE HLSL
// declaration shared with DDGIRayTrace / RTShadow / RTAO. The C++ mirror is
// DDGIInstanceData in DDGISceneAS.cpp. Do NOT re-declare it here.
#include "rt_instance_data.hlsli"
typedef RTInstanceData HRInstanceData;   // short name used throughout this file
#define HR_MaterialUV RTInstanceUV       // short name of the UV transform

StructuredBuffer<HRInstanceData>   g_Instances     : register(t1, space0);

// RT_INST_FLAG_* for an instance. Unlike HR_AlphaFlags there is no availability
// gate: g_Instances is the buffer HR_ShadeHit already indexes unconditionally,
// so if it were not live nothing in this shader would work anyway.
uint HR_InstFlags(uint instId)
{
    return g_Instances[instId].rtFlags;
}

Texture2D<float>                   gDepth          : register(t2, space0);
Texture2D<float4>                  gNormal         : register(t3, space0);
Texture2D<float4>                  gSurface        : register(t4, space0);   // r = roughness
StructuredBuffer<GPULight>         gLights         : register(t6, space0);

StructuredBuffer<DDGIVolumeGPU>    gDDGIVolumes                     : register(t7,  space0);
Texture2D<float4>                  gDDGIIrradiance[DDGI_MAX_VOLUMES]   : register(t0, space4);
Texture2D<float2>                  gDDGIDepth[DDGI_MAX_VOLUMES]     : register(t0, space5);
StructuredBuffer<DDGIProbeData>    gDDGIProbeData[DDGI_MAX_VOLUMES] : register(t0, space6);

// SkyIBL SH coefficients (9 x float4, the buffer Lighting.ps reads at t19). sky_sh.hlsli
// binds it at t19 space0 for the graphics signature; this one declares it at t22, and
// HR_EvalSkySH runs sky_sh_eval.hlsli's evaluator on it.
StructuredBuffer<float4>           gSkySH          : register(t22, space0);
TextureCube<float4>                gSkyCube        : register(t23, space0);
// The SAME cosine-convolved irradiance cube Lighting.ps diffuse-samples (real cube in
// static-skybox scenes, shared black fallback otherwise): the hit's ambient walks the
// IDENTICAL iblUseSH ? SH : irradiance branch as the main view.
TextureCube<float4>                gSkyIrradiance  : register(t24, space0);

RWTexture2D<float4>                OutRawColor     : register(u0, space0);   // rgb radiance, a conf
RWTexture2D<float>                 OutHitDist      : register(u1, space0);   // world-space hit t
RWStructuredBuffer<uint>           g_HWRayList     : register(u3, space0);   // packed pixels (HR_PackRay)
RWStructuredBuffer<uint>           g_RayCounter    : register(u4, space0);   // see HR_CTR_*

ByteAddressBuffer                  g_Buffers[16384]     : register(t0, space1);
Texture2D                          g_AllTextures[16384] : register(t0, space2);

SamplerState                       gSampWrap       : register(s1, space0);

// Per-instance alpha-cutout records - same InstanceID() index space as g_Instances, but a
// compact 40-byte table because it is read per traversal CANDIDATE, not per committed hit.
// See rt_alpha_record.hlsli.
#include "rt_alpha_record.hlsli"

StructuredBuffer<RTAlphaRecord>    g_AlphaRecords  : register(t32, space0);
// Shared radiance-space environment hand-off. MUST come after the DDGI / probe / sky
// RESOURCE declarations above (t7..t23), not just after their helper headers: it samples
// those bindings directly.
#include "hybrid_refl_env.hlsli"

// ==== Hit-attribute fetch ====
// A clone of DDGIRayTrace's LoadHitTriangleAttributes. Vertex layout in the bindless VB:
// pos float3 @0, normal float3 @12, uv float2 @24 across ALL layouts; only the STRIDE varies
// per library (32 base, 48 with tangent, PMX/scene imports write their own), so walk with
// inst.vertexStride - a hard-coded 32 reads garbage normals/UVs on every other mesh. Indices
// are 16- or 32-bit (inst.indexBytes) and absolute into the VB view (only indexStart
// offsets the IB walk).
#define HR_VERTEX_STRIDE_BYTES 32   // minimum/fallback stride
#define HR_INDEX_BYTES         4

// Bit0 of DDGISceneAS's instance masks: bit0 = visible to reflections/RTAO (main-pass-visible
// statics + skinned characters), bit1 = RT-shadow caster, bit2 = DDGI, bit3 = bit0 minus the
// forward translucents. Hidden instances carry none of bits 0/1/3, so a ray passes through
// them to whatever is behind and VisibilityComponent-hidden objects never appear in
// reflections. The reflection rays here trace the CB's rayMask (0x08), not this mask; DDGI
// traces 0x04 (no deforming geometry), RTShadow and the sun-shadow ray 0x02.
#define HR_REFLECT_MASK        0x01u

// Index fetch - primitives use R16_UINT (indexBytes == 2), MeshLibrary R32.
// ByteAddressBuffer loads are 4-byte aligned; pick the 16-bit half manually.
uint HR_LoadIndex(uint ibIdx, uint i, uint indexBytes)
{
    if (indexBytes == 2u)
    {
        const uint byteOff = i * 2u;
        const uint word    = g_Buffers[ibIdx].Load(byteOff & ~3u);
        return (byteOff & 2u) ? (word >> 16) : (word & 0xFFFFu);
    }
    return g_Buffers[ibIdx].Load(i * 4u);
}

bool HR_LoadHitTriangleAttributes(uint instId, uint primIdx,
                                  out float3 p0, out float3 p1, out float3 p2,
                                  out float3 n0, out float3 n1, out float3 n2,
                                  out float2 uv0, out float2 uv1, out float2 uv2)
{
    HRInstanceData inst = g_Instances[instId];
    p0 = p1 = p2 = float3(0, 0, 0);
    n0 = n1 = n2 = float3(0, 1, 0);
    uv0 = uv1 = uv2 = float2(0, 0);
    if (inst.vbBindlessIdx == 0xFFFFFFFFu || inst.ibBindlessIdx == 0xFFFFFFFFu)
        return false;

    const uint vbIdx = NonUniformResourceIndex(inst.vbBindlessIdx);
    const uint ibIdx = NonUniformResourceIndex(inst.ibBindlessIdx);

    const uint indexBytes = (inst.indexBytes == 2u) ? 2u : 4u;
    uint i0 = HR_LoadIndex(ibIdx, inst.indexStart + primIdx * 3 + 0, indexBytes);
    uint i1 = HR_LoadIndex(ibIdx, inst.indexStart + primIdx * 3 + 1, indexBytes);
    uint i2 = HR_LoadIndex(ibIdx, inst.indexStart + primIdx * 3 + 2, indexBytes);

    if (inst.normalBufIdx != 0xFFFFFFFFu)
    {
        // De-interleaved streams (procedural primitives + skinned meshes):
        // vb = positions (float3, stride 12), separate normal (float3) +
        // uv0 (float2) streams. Skinned streams live in the shared
        // SkinnedVertexRing arena - rebase by the per-instance element bases
        // (vertexStart = position base; all bases are 0 for primitives).
        const uint nbIdx = NonUniformResourceIndex(inst.normalBufIdx);
        p0 = asfloat(g_Buffers[vbIdx].Load3((inst.vertexStart + i0) * 12u));
        p1 = asfloat(g_Buffers[vbIdx].Load3((inst.vertexStart + i1) * 12u));
        p2 = asfloat(g_Buffers[vbIdx].Load3((inst.vertexStart + i2) * 12u));
        n0 = asfloat(g_Buffers[nbIdx].Load3((inst.nrmStart + i0) * 12u));
        n1 = asfloat(g_Buffers[nbIdx].Load3((inst.nrmStart + i1) * 12u));
        n2 = asfloat(g_Buffers[nbIdx].Load3((inst.nrmStart + i2) * 12u));
        if (inst.uvBufIdx != 0xFFFFFFFFu)
        {
            const uint uvbIdx = NonUniformResourceIndex(inst.uvBufIdx);
            uv0 = asfloat(g_Buffers[uvbIdx].Load2((inst.uvStart + i0) * 8u));
            uv1 = asfloat(g_Buffers[uvbIdx].Load2((inst.uvStart + i1) * 8u));
            uv2 = asfloat(g_Buffers[uvbIdx].Load2((inst.uvStart + i2) * 8u));
        }
        return true;
    }

    const uint stride = max(inst.vertexStride, HR_VERTEX_STRIDE_BYTES);
    uint v0Off = i0 * stride;
    uint v1Off = i1 * stride;
    uint v2Off = i2 * stride;
    p0 = asfloat(g_Buffers[vbIdx].Load3(v0Off + 0));
    p1 = asfloat(g_Buffers[vbIdx].Load3(v1Off + 0));
    p2 = asfloat(g_Buffers[vbIdx].Load3(v2Off + 0));
    n0 = asfloat(g_Buffers[vbIdx].Load3(v0Off + 12));
    n1 = asfloat(g_Buffers[vbIdx].Load3(v1Off + 12));
    n2 = asfloat(g_Buffers[vbIdx].Load3(v2Off + 12));
    uv0 = asfloat(g_Buffers[vbIdx].Load2(v0Off + 24));
    uv1 = asfloat(g_Buffers[vbIdx].Load2(v1Off + 24));
    uv2 = asfloat(g_Buffers[vbIdx].Load2(v2Off + 24));
    return true;
}

// ==== Alpha-cutout candidate test ====
// Runs inside the Proceed() loop for instances DDGISceneAS flags
// D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE; every other instance keeps its BLAS
// FLAG_OPAQUE and never reaches here. Bindless indices are range-checked against the heap
// sizes (kMaxBuffers / kMaxBindlessTextures = 16384): an out-of-range descriptor-table read
// is undefined and can hang the device (seen on a world reload). Every failure path returns
// "accept", i.e. a fully solid occluder.
#define HR_BINDLESS_LIMIT 16384u

// Effective-threshold softening. The deferred raster path does NOT do a raw compare:
// custom_surface.hlsli runs the Ben Golus rescale clip((a - ref) / max(fwidth(a), 1e-4) + 0.5),
// an effective threshold of ref - 0.5*fwidth(a), more permissive and TAA-dithered on top. A
// ray has no derivatives, so a raw compare gives systematically THINNER cutouts in
// reflections than on screen; a small constant bias is the closest available approximation.
// Tune, do not remove.
#define HR_ALPHA_SOFTEN 0.02

// Never below half the authored threshold: with a small Alpha Ref (foliage authored at
// 0.005-0.024) the fixed bias alone accepts every texel - solid leaf cards in reflections.
// Mirror of rt_hit_fetch.hlsli's HR_AlphaThreshold.
float HR_AlphaThreshold(float alphaRef)
{
    return max(alphaRef - HR_ALPHA_SOFTEN, 0.5 * alphaRef);
}

// Fetch ONLY the three UVs of a CANDIDATE triangle. HR_LoadHitTriangleAttributes
// above also pulls positions + normals, which a candidate test does not need.
bool HR_LoadCandidateUV(RTAlphaRecord a, uint primIdx, float2 bary, out float2 uv)
{
    uv = float2(0, 0);
    if (a.vbBindlessIdx >= HR_BINDLESS_LIMIT || a.ibBindlessIdx >= HR_BINDLESS_LIMIT)
        return false;

    const uint vbIdx = NonUniformResourceIndex(a.vbBindlessIdx);
    const uint ibIdx = NonUniformResourceIndex(a.ibBindlessIdx);
    const uint indexBytes = (a.indexBytes == 2u) ? 2u : 4u;
    const uint i0 = HR_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 0u, indexBytes);
    const uint i1 = HR_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 1u, indexBytes);
    const uint i2 = HR_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 2u, indexBytes);

    float2 uv0, uv1, uv2;
    if (a.flags & RT_ALPHA_FLAG_DEINTERLEAVED)
    {
        // Procedural primitives + skinned: UVs are a separate float2 stream
        // rebased by uvStart. Skinned meshes have no UVs in the SkinnedVertexRing
        // (pos/nrm/prevPos only) but UVs are skin-invariant and come from the
        // static per-entity uv0 buffer, which DDGISceneAS forwards here.
        if (a.uvBufIdx >= HR_BINDLESS_LIMIT) return false;  // no UV stream -> solid
        const uint uvbIdx = NonUniformResourceIndex(a.uvBufIdx);
        uv0 = asfloat(g_Buffers[uvbIdx].Load2((a.uvStart + i0) * 8u));
        uv1 = asfloat(g_Buffers[uvbIdx].Load2((a.uvStart + i1) * 8u));
        uv2 = asfloat(g_Buffers[uvbIdx].Load2((a.uvStart + i2) * 8u));
    }
    else
    {
        // Interleaved MeshLibrary: pos@0 nrm@12 uv@24 in the first 32 bytes of
        // EVERY layout; only the stride varies. Indices are absolute into the VB
        // view - vertexStart is NOT added, matching the interleaved branch of
        // HR_LoadHitTriangleAttributes above.
        const uint stride = max(a.vertexStride, HR_VERTEX_STRIDE_BYTES);
        uv0 = asfloat(g_Buffers[vbIdx].Load2(i0 * stride + 24u));
        uv1 = asfloat(g_Buffers[vbIdx].Load2(i1 * stride + 24u));
        uv2 = asfloat(g_Buffers[vbIdx].Load2(i2 * stride + 24u));
    }
    const float w = 1.0 - bary.x - bary.y;
    uv = uv0 * w + uv1 * bary.x + uv2 * bary.y;
    return true;
}

// The flags word for an instance, or 0 when the record table is not live this frame. MUST
// be the only way g_AlphaRecords is reached: the root SRV is bound unconditionally and may
// be VA 0.
uint HR_AlphaFlags(uint instId)
{
    if (alphaTestEnabled == 0u)       return 0u;
    if (instId >= alphaInstanceCount) return 0u;
    return g_AlphaRecords[instId].flags;
}

// Returns true = accept (commit the candidate as a hit). includeBaseColorAlpha selects the
// raster path the ray mirrors:
//   true  - GBuffer parity (s.opacity = baseTex.a * mat.baseColor.a) for the REFLECTION ray.
//   false - Shadow.ps parity (texture alpha alone, no baseColor.a) for the SUN-SHADOW ray
//           fired at a reflection hit point.
bool HR_AlphaAccept(uint instId, uint primIdx, float2 bary, bool includeBaseColorAlpha)
{
    const uint hrFlags = HR_AlphaFlags(instId);

    // Alpha-BLEND (translucent) instances are FORCE_NON_OPAQUE so RTShadow's
    // candidate loop can attenuate through them, so they reach THIS loop too.
    // The two call sites want opposite answers, and includeBaseColorAlpha
    // discriminates them:
    //   REFLECTION ray (true)    - COMMIT. A window has to be visible in a
    //                              mirror; HR_ShadeHit shades it through the
    //                              material proxy.
    //   ShadowVisibility (false) - PASS THROUGH. Glass must not cast a hard
    //                              shadow onto a reflected surface, matching
    //                              main-view RTShadow.
    // Binary rather than a transmittance product on purpose: this is a
    // secondary ray inside a reflection, where the extra fidelity is invisible
    // and the traversal cost is not.
    if ((hrFlags & RT_ALPHA_FLAG_BLEND) != 0u) return includeBaseColorAlpha;

    if ((hrFlags & RT_ALPHA_FLAG_ALPHA_CLIP) == 0u) return true;

    const RTAlphaRecord a = g_AlphaRecords[instId];
    if ((a.flags & RT_ALPHA_FLAG_ALPHA_CLIP) == 0u) return true;
    // No base-color texture bound -> nothing to clip against, same as Shadow.ps.
    // Also covers the transient albedoTexIdx == -1 window during texture
    // streaming / right after OnWorldClear.
    if (a.albedoTexIdx < 0 || a.albedoTexIdx >= (int)HR_BINDLESS_LIMIT) return true;

    float2 uv;
    if (!HR_LoadCandidateUV(a, primIdx, bary, uv)) return true;

    // Material UV transform. The compact 40 B alpha record deliberately does
    // NOT carry it (it is fetched per traversal CANDIDATE), so read it from
    // the 128 B instance table - same InstanceID() index space. Only
    // candidates that passed the ALPHA_CLIP flag AND have a texture bound
    // reach this line, and they are about to do a texture Sample, so one
    // extra 16-byte load is noise. Without it a tiled cutout (fence, foliage
    // card) clips on a different silhouette in reflections than on screen.
    uv = HR_MaterialUV(g_Instances[instId], uv);

    // Mip 0 - an inline RayQuery has no ray differentials, so the mip MUST be
    // explicit. Matches the committed-hit albedo sample in HR_ShadeHit.
    // .r when the record carries a Masked-mode opacity mask, else .a.
    float alpha = RTAlphaSampleAlpha(
        a, g_AllTextures[NonUniformResourceIndex((uint)a.albedoTexIdx)],
        gSampWrap, uv, 0);
    if (includeBaseColorAlpha) alpha *= g_Instances[instId].baseColor.a;

    return alpha >= HR_AlphaThreshold(a.alphaRef);
}

// ==== Sky SH diffuse at the hit ====
// E(N)/pi: sky_sh_eval.hlsli's evaluator, the one Lighting.ps runs, on this signature's t22.
#include "sky_sh_eval.hlsli"
float3 HR_EvalSkySH(float3 N)
{
    return EvalSH2From(gSkySH, N);
}

// ==== Shadow ray ====
// Clone of DDGIRayTrace::ShadowVisibility (TDR-tuned TMax).
float ShadowVisibility(float3 origin, float3 dir, float maxDist)
{
    RayDesc r;
    r.Origin    = origin + dir * 0.05;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = maxDist;

    // RAY_FLAG_FORCE_OPAQUE is deliberately absent so alpha-clip instances (which
    // carry D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE) report candidates
    // and get their cutout tested. ACCEPT_FIRST_HIT_AND_END_SEARCH is kept: any
    // COMMITTED occluder ends the search, which is exactly the shadow semantic.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    // Sun-shadow rays at reflection HIT points trace the SHADOW-CASTER bit
    // (0x02), not the reflection-visibility bit: non-casting objects must not
    // shadow reflected surfaces they do not shadow on screen, and hidden
    // shadow-only casters must keep shadowing inside mirrors too.
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02u, r);
    [loop]
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            // Shadow parity: texture alpha only, no baseColor.a (Shadow.ps).
            if (HR_AlphaAccept(q.CandidateInstanceID(),
                               q.CandidatePrimitiveIndex(),
                               q.CandidateTriangleBarycentrics(),
                               /*includeBaseColorAlpha*/false))
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// A hit's sky visibility where nothing else gives one: one cosine-distributed ray from the
// hit, traced with the reflection's own mask (rayMask 0x08: forward-translucent glass is not
// in it, as it is not in the GI shell's rays), 1 when it leaves the scene. The hash changes
// every frame, so the reflection denoiser averages it like the shadowed-light pick.
float HR_SkyVisibilityRay(float3 hitPos, float3 N, uint hash)
{
    const float u1 = float(hash & 0xffffu) * (1.0 / 65536.0);
    const float u2 = float(hash >> 16) * (1.0 / 65536.0);
    const float r = sqrt(u1), phi = 6.28318530718 * u2;
    const float3 t = normalize(abs(N.y) < 0.999 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
    const float3 b = cross(N, t);
    RayDesc ray;
    ray.Origin    = hitPos + N * 0.01;
    ray.Direction = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u1)));
    ray.TMin      = 0.0;
    ray.TMax      = 1.0e4;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, rayMask, ray);
    [loop]
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),
                           q.CandidateTriangleBarycentrics(), /*includeBaseColorAlpha*/true))
            q.CommitNonOpaqueTriangleHit();
    }
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// Local lights at a reflection hit are SHADOWED by ReSTIR GI's Tier 1 rule for secondary
// hits (rt_shade_diffuse.hlsli): a cell with up to HR_HIT_EXACT_LIGHTS lights is summed
// exactly, one shadow ray per light that reaches the hit; a busier cell draws ONE light in
// proportion to its unshadowed contribution (the exact normaliser, so the expectation is the
// shadowed sum) and traces one ray - noise the reflection denoiser removes. The pick hashes
// the hit position with the frame, so it converges in the history. unifiedFlags bit 3
// (HR_FLAG_UNSHADOWED_HIT_LIGHTS, -rtr_unshadowed_hit_lights): the unshadowed sum, for A/B.
static const uint HR_HIT_EXACT_LIGHTS = 8u;
static const uint HR_FLAG_UNSHADOWED_HIT_LIGHTS = 8u;
#include "core/hash.hlsli"
// The light's visibility from the hit along l (normalised), the light dist
// away: ShadowVisibility's caster mask and alpha test, ending 2 mm short of
// the light (its origin is already 5 cm along l), as DI / GI's local rays do.
float HR_LocalLightVisibility(float3 hitPos, float3 l, float dist)
{
    const float tMax = dist - 0.052;
    if (tMax <= 0.0) return 1.0;
    return ShadowVisibility(hitPos, l, tMax);
}

// Analytic point/spot incident radiance WITHOUT a shadow ray; HR_ShadeHit adds the
// visibility (HR_LocalLightVisibility) - unshadowed, a light on the floor above lights the
// reflected room below straight through the slab. Returns the attenuated incident radiance
// (color x intensity x atten x spot) and the direction to the light in `L`; the caller
// applies NdotL and the Cook-Torrance BRDF so the hit gets diffuse AND specular, like the
// clustered-light loop in Lighting.ps. Attenuation is Lighting.ps's model (quadratic window
// x inverse-square + smoothstep spot cone).
float3 IncidentLocalLight(GPULight light, float3 P, out float3 L)
{
    L = float3(0, 1, 0);
    if (light.type != 1u && light.type != 2u)
        return float3(0, 0, 0);   // directional = the sun, handled separately

    float3 d    = light.position - P;
    float  dist = length(d);
    if (dist > light.radius) return float3(0, 0, 0);
    L = d / max(dist, 1e-6);

    // Distance window x inverse square x cone, from the ONE implementation
    // (light_eval.hlsli). Keep distance and cone as two separate factors,
    // ((color*intensity) * att) * spot: folding them into one
    // EvalLocalLightAttenuation() call reassociates the product and moves the
    // result by ~1 ULP (harmless, but it breaks the DXIL's bit-identity).
    return light.color * light.intensity
         * SurfaceDistanceAttenuation(dist, light.radius)
         * AngularAttenuation(L, light);
}

// ==== Analytic material-proxy hit shading ====
// Identity below the knee, rational shoulder above it (NPRStabilize.cs.hlsl's SoftShoulder,
// duplicated verbatim; asymptote start+range).
float HR_SoftShoulder(float value, float start, float range)
{
    if (value <= start) return value;
    float d = value - start;
    return start + d * range / (range + d);
}

// NPR toon knobs from the REAL MaterialGPUData record via the bindless buffer table
// (HybridReflConstants.materialBufIdx + the slot riding rtFlags bits 16..31), at the byte
// offsets of material_layout.generated.hlsli (tools/gen_material_layout.py emits it from the
// C++ struct). Do not approximate the knobs with constants: authored values (such as
// nprDirectLightScale 0.25 on body/face, a sun intensity-coupled toward luminance 1) make a
// reflected character 4-6x too bright otherwise.
struct HRNprKnobs
{
    float  minBrightness;      // Lighting.ps's smooth luminance floor
    float  shadowThreshold;    // ramp-U sigmoid center
    float  shadowSmooth;       // -> sharpness, ToonNdlSharp (toon_eval.hlsli) formula
    float  maxBrightness;      // absolute exposed-space soft cap (0=none), NPRStabilize
    float3 diffuseRampColor;   // no-ramp-texture lit end
    float3 shadowRampColor;    // no-ramp-texture shadow end
    int    rampTexIdx;         // toonRampTex - the material's ramp texture
    float  shadowStrength;     // lerp(1, form, s)
    float  mainLightLerp;      // lerp(coupledLight, white, t)
    float  directOcclusion;    // directOcc = lerp(1-p, 1, ao)
    float  directScale;        // nprDirectLightScale
    float  additionalScale;    // nprAdditionalLightScale (punctuals)
    float  ambientScale;       // nprAmbientLightScale
    float  specularScale;      // nprSpecularLightScale
    float  colorWeight;        // NPRCoupleSceneLight chroma weight
    float  intensityWeight;    // NPRCoupleSceneLight luminance weight
    float  referenceLum;       // NPRCoupleSceneLight reference luminance
    float  exposureBiasEV;     // NPRStabilize exp2(clamp(ev, -2, 2))
    float  shoulderStart;      // NPRStabilize exposed-space artistic shoulder
    float  shoulderRange;
    float  shoulderStrength;
    float  exposureComp;       // NPRStabilize protection (NPR limit = 1.0 = fully immune)
};

HRNprKnobs HR_LoadNprKnobs(uint rtFlags)
{
    HRNprKnobs k;
    // Shipped MaterialComponent defaults - used when the slot or the table is
    // unavailable (entity emitted nothing this frame, or buffer unregistered).
    k.minBrightness    = 0.0;
    k.shadowThreshold  = 0.5;
    k.shadowSmooth     = 0.05;
    k.maxBrightness    = 0.0;
    k.diffuseRampColor = float3(1.0, 1.0, 1.0);
    k.shadowRampColor  = float3(0.02, 0.02, 0.03);
    k.rampTexIdx       = -1;
    k.shadowStrength   = 1.0;
    k.mainLightLerp    = 0.0;
    k.directOcclusion  = 0.1;
    k.directScale      = 1.0;
    k.additionalScale  = 0.35;
    k.ambientScale     = 0.75;
    k.specularScale    = 0.75;
    k.colorWeight      = 1.0;
    k.intensityWeight  = 1.0;
    k.referenceLum     = 1.0;
    k.exposureBiasEV   = 0.0;
    k.shoulderStart    = 1.0;
    k.shoulderRange    = 0.75;
    k.shoulderStrength = 1.0;
    k.exposureComp     = 0.0;

    const uint slotPlus1 = rtFlags >> 16u;
    if (slotPlus1 == 0u || materialBufIdx >= 16384u)
        return k;
    // EVERY NUMBER BELOW IS GENERATED (material_layout.generated.hlsli). Do NOT write a literal
    // offset or stride here: appending a field changes the STRIDE (every material past slot 0
    // is read from the wrong place), and inserting one in the MIDDLE keeps the stride (a size
    // assert still passes) but shifts every later offset by four bytes. Either way a reflected
    // NPR character's toon knobs are garbage, with no build error and no runtime diagnostic.
    // tools/gen_material_layout.py emits the header from the struct, cross-checked against
    // that header's own static_asserts; EngineCore.vcxproj regenerates it when SystemHandles.h
    // changes and ci_check.py fails on drift.
    const uint base = (slotPlus1 - 1u) * MATERIAL_GPU_DATA_STRIDE;

    k.minBrightness    = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprMinBrightness));
    k.shadowThreshold  = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprShadowThreshold));
    k.shadowSmooth     = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprShadowSmooth));
    k.diffuseRampColor = asfloat(g_Buffers[materialBufIdx].Load3(base + MAT_OFF_nprDiffuseRampColor));
    k.maxBrightness    = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprMaxBrightness));
    k.shadowRampColor  = asfloat(g_Buffers[materialBufIdx].Load3(base + MAT_OFF_nprShadowRampColor));
    // The packer resolves which custom texture is the ramp.
    k.rampTexIdx       = asint(g_Buffers[materialBufIdx].Load(base + MAT_OFF_toonRampTex));
    k.shadowStrength   = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprShadowStrength));
    k.mainLightLerp    = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprMainLightLerp));
    k.directOcclusion  = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprDirectOcclusion));
    k.directScale      = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprDirectLightScale));
    k.additionalScale  = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprAdditionalLightScale));
    k.ambientScale     = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprAmbientLightScale));
    k.specularScale    = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprSpecularLightScale));
    k.colorWeight      = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprSceneLightColorWeight));
    k.intensityWeight  = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprSceneLightIntensityWeight));
    k.referenceLum     = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprReferenceLightLuminance));
    k.exposureBiasEV   = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprExposureBiasEV));
    k.shoulderStart    = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprShoulderStart));
    k.shoulderRange    = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprShoulderRange));
    k.shoulderStrength = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_nprShoulderStrength));
    k.exposureComp     = asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_exposureCompensation));
    return k;
}

// Current scene exposure from the AutoExposure buffer over bindless - the same prev-frame
// value NPRStabilize reads (both run before AutoExposure in the post chain). 1.0 when the
// buffer is unavailable, which makes every counter-scale below the identity rather than a
// black or blown frame.
float HR_SceneExposure()
{
    if (exposureBufIdx >= 16384u) return 1.0;
    const float e = asfloat(g_Buffers[exposureBufIdx].Load(0));
    return (isfinite(e) && e >= 1e-4) ? e : 1.0;
}

// exposureCompensation alone, for hits that do NOT want the full toon-knob load (UNLIT).
// Same slot decode and the same GENERATED offset contract as HR_LoadNprKnobs - read the
// warning there before touching either.
float HR_LoadExposureComp(uint rtFlags)
{
    const uint slotPlus1 = rtFlags >> 16u;
    if (slotPlus1 == 0u || materialBufIdx >= 16384u) return 0.0;
    const uint base = (slotPlus1 - 1u) * MATERIAL_GPU_DATA_STRIDE;
    return asfloat(g_Buffers[materialBufIdx].Load(base + MAT_OFF_exposureCompensation));
}

// Shades a committed hit from its material proxy, matching Lighting.ps's per-pixel shading
// term for term (minus SSAO, which does not exist at an off-screen hit), so analytic RT hits
// do not render darker or flatter than the same content delivered by the screen tier.
//   R      = the incoming ray direction (view dir at the hit = -R).
//   flipN  = the ray hit the BACK face of double-sided geometry (foliage/hair/fence cards).
//            The authored normals point out of the front face, so lighting them unflipped
//            gives a fully back-lit, near-black card.
float3 HR_ShadeHit(uint instId, uint primIdx, float2 bary, float3x4 obj2w,
                   float3 hitPos, float3 R, bool flipN = false)
{
    HRInstanceData inst = g_Instances[instId];
    float3 albedo   = inst.baseColor.rgb;
    float3 emissive = inst.emissive.rgb;
    const bool hitCached = (inst.rtFlags & RT_INST_FLAG_SURFACE_CACHE) != 0u;
    // NPR (stencil-2 toon) instances: on screen their sun diffuse is
    //   albedo*(1-metal) * ramp(form) * lightColor * (1/PI) * vis
    // The ramp REPLACES NdotL and its U=0 end is an authored NON-BLACK floor (Lighting.ps
    // TOON_RAMP_V_DIRECT_SHADOW band), so an NdotL-gated Cook-Torrance term would render the
    // reflected character's backlit side pitch black where the primary view shows the warm
    // ramp floor.
    const bool hitNPR    = (inst.rtFlags & RT_INST_FLAG_NPR) != 0u;

    // ---- Interpolated world normal + UV ----
    float3 worldN;
    // Smooth (pre-normal-map) shading normal, kept for the outline
    // silhouette test - bumpy mapped normals would speckle the rim.
    float3 vertexN = float3(0.0, 0.0, 1.0);
    float2 rawHitUV = float2(0, 0);
    float2 hitUV = float2(0, 0);
    bool   hasUV = false;
    {
        float3 p0, p1, p2, n0, n1, n2;
        float2 uv0, uv1, uv2;
        bool ok = HR_LoadHitTriangleAttributes(instId, primIdx,
                                               p0, p1, p2, n0, n1, n2,
                                               uv0, uv1, uv2);
        if (ok)
        {
            float u = bary.x;
            float v = bary.y;
            float w = 1.0 - u - v;
            float3 nObj = normalize(n0 * w + n1 * u + n2 * v);
            if (any(isnan(nObj)) || dot(nObj, nObj) < 1e-4)
                nObj = normalize(cross(p1 - p0, p2 - p0));
            worldN = normalize(mul((float3x3)obj2w, nObj));
            if (flipN) worldN = -worldN;
            vertexN = worldN;   // smooth pre-normal-map normal (outline test)
            // Material UV transform applied ONCE, right where the barycentric
            // UV is formed, so every material fetch below (emissive, albedo,
            // surface) tiles exactly like the GBuffer's MS_MakeParams.
            rawHitUV = uv0 * w + uv1 * u + uv2 * v;
            hitUV  = HR_MaterialUV(inst, rawHitUV);
            hasUV  = true;

            // ---- Normal mapping ----
            // The analytic path applies the normal map itself: shading on the
            // interpolated VERTEX normal alone gives flat-looking metal in mirrors
            // wherever screen reuse (an already-shaded pixel) cannot deliver it.
            // The tangent frame is derived PER-TRIANGLE from the positions and UVs
            // fetched above, so no tangent stream is needed (the VB layout is not
            // uniform: 32 B strides carry no tangent at all).
            // Derived from the RAW mesh UVs on purpose: the material UV transform only
            // scales/offsets the same parameterisation, so it changes the tangent's
            // LENGTH, not its direction, and T/B are normalised anyway.
            if (inst.normalTexIdx >= 0)
            {
                const float3 e1 = p1 - p0, e2 = p2 - p0;
                const float2 d1 = uv1 - uv0, d2 = uv2 - uv0;
                const float  det = d1.x * d2.y - d2.x * d1.y;
                if (abs(det) > 1e-12)
                {
                    const float3 tObj = (e1 * d2.y - e2 * d1.y) / det;
                    float3 T = normalize(mul((float3x3)obj2w, tObj));
                    // Gram-Schmidt against the (already flipped) shading normal
                    // so the basis stays orthonormal on skewed UVs.
                    T = normalize(T - worldN * dot(worldN, T));
                    if (all(isfinite(T)) && dot(T, T) > 1e-6)
                    {
                        // Bitangent HANDEDNESS. A bare cross(worldN, T) imposes one
                        // fixed handedness, which is wrong for mirrored UV islands
                        // (a negative authored tangent.w, common in imported packs)
                        // and for negative-determinant (mirrored) instance transforms.
                        // Both make the normal map's green channel read inverted, so
                        // bumps reflect as dents. The true dV direction, from the same
                        // triangle UVs T came from, picks the sign; it covers both
                        // cases because this bitangent rides through obj2w like T and
                        // worldN do, so no separate determinant term is needed. The
                        // raster path gets the same information from the authored
                        // tangent.w x InstanceHandednessSign (gpu_instance.hlsli).
                        const float3 bObj = (e2 * d1.x - e1 * d2.x) / det;
                        const float3 Bw   = mul((float3x3)obj2w, bObj);
                        // Handedness is a property of the GEOMETRIC frame, so
                        // measure it against the UNflipped normal. The flipN
                        // (back-face) case then falls out of cross(worldN, T)
                        // on its own, matching custom_surface.hlsli's
                        // `if (!frontFace) { N = -N; BT = -BT; }`.
                        const float3 nGeo  = flipN ? -worldN : worldN;
                        const float  bSign = (dot(cross(nGeo, T), Bw) < 0.0) ? -1.0 : 1.0;
                        const float3 B     = cross(worldN, T) * bSign;
                        const uint   nIdx = NonUniformResourceIndex((uint)inst.normalTexIdx);
                        // Same unpack as MS_SampleNormalTS: RG stored, B derived.
                        // normalStrength is NOT carried in the 128 B table, so this
                        // applies it at 1.0: a material that dials the raster
                        // strength down still reflects at full strength.
                        // A cached instance uses hitUV for its atlas page, but the
                        // normal map is the material's own texture. Surface-cache
                        // acquisition only accepts identity texMulAdd, so rawHitUV
                        // is exactly the raster normal-map UV and cannot wander
                        // into a neighbouring atlas page.
                        const float2 normalUV = hitCached ? rawHitUV : hitUV;
                        const float2 rg = g_AllTextures[nIdx]
                                            .SampleLevel(gSampWrap, normalUV, 0).rg * 2.0 - 1.0;
                        const float  nz = sqrt(saturate(1.0 - dot(rg, rg)));
                        // rows = T, B, N - matches MS_ApplyNormalMap.
                        const float3 mapped =
                            normalize(mul(float3(rg, nz), float3x3(T, B, worldN)));
                        if (all(isfinite(mapped)))
                            worldN = mapped;
                    }
                }
            }
        }
        else
        {
            // Already faces the ray - flipping would point it away.
            worldN = -normalize(R);
            vertexN = worldN;
        }
    }

    if (inst.emissiveTexIdx >= 0 && hasUV)
    {
        const uint texIdx = NonUniformResourceIndex((uint)inst.emissiveTexIdx);
        emissive *= g_AllTextures[texIdx].SampleLevel(gSampWrap, hitUV, 0).rgb;
    }

    // ---- Material proxy at the hit (material_surface.hlsli parity) ----
    // No SURFACEMAP bound -> the GBuffer path samples a white default
    // surface map (surf = 1,1,1): ao 1, roughness = roughnessMax,
    // metalness = metalnessMax. Mirror that exactly so flat-authored
    // materials shade identically here and in the main view.
    float hitAO    = 1.0;
    float hitRough = inst.roughnessMax;
    float hitMetal = inst.metalnessMax;
    // Emissive modulation for surface-cached instances. FAILS TO ZERO on purpose: a cached
    // instance has emissiveTexIdx = -1, and if the page sample is never reached (no UV, or the
    // surface page is unbound) the constant emissive would pass through at full strength,
    // which for a material authoring emissiveColor = (1,1,1,1) is a pure-white glow. "No
    // emissive" is the safe failure; "full white emitter" is not.
    float       cacheEmisMask = hitCached ? 0.0 : 1.0;
    if (hasUV)
    {
        if (inst.albedoTexIdx >= 0)
        {
            // ALBEDO_RAW: the albedo is the material's own FULL-RESOLUTION
            // texture (space3 alias), sampled at the raw mesh UV: the 128^2
            // surface-cache page kept for surface/emissive-mask is 16x
            // under-sampled per axis vs a 2048^2 character texture, and a mirror
            // would lose all texture detail. baseColor stays the authored tint
            // for these instances (the raw texture does not contain the multiply
            // the page bakes in).
            const float2 albedoUV =
                ((inst.rtFlags & RT_INST_FLAG_ALBEDO_RAW) != 0u) ? rawHitUV
                                                                 : hitUV;
            const uint aIdx = NonUniformResourceIndex((uint)inst.albedoTexIdx);
            albedo *= g_AllTextures[aIdx].SampleLevel(gSampWrap, albedoUV, 0).rgb;
        }
        if (inst.surfaceTexIdx >= 0)
        {
            const uint sIdx = NonUniformResourceIndex((uint)inst.surfaceTexIdx);
            const float4 surf4 = g_AllTextures[sIdx].SampleLevel(gSampWrap, hitUV, 0);
            // ---- Surface-cache emissive mask ----
            // A cached instance arrives with emissiveTexIdx = -1, because the
            // material's own emissive map cannot be sampled through a page rect,
            // and -1 in this proxy means "no texture, do not multiply", NOT black:
            // the constant emissive.rgb would pass at FULL strength. Materials that
            // author emissiveColor = (1,1,1,1) are fine on screen only because the
            // engine slot holds the BLACK Default_Emissive.itex; reflected without a
            // substitute they become a pure-white glow. The bake writes an emissive
            // luminance mask into the page's alpha (SurfaceCacheResolve.cs.hlsl
            // SC_PackSurface): the quantity the engine slot supplies, measured from
            // the material's real output, and free because this sample is taken
            // anyway.
            // NPR pages keep their baked emissive mask too. The endfield shaders
            // route light-oriented decals through s.emissive (hair aniso shine, lip
            // highlight, rim), so the bake freezes ONE head/light orientation, but
            // zeroing it removes the hair's sheen and the reflected character reads
            // flat. A plausibly placed frozen highlight beats a missing one; the
            // frozen-direction error is bounded by the page refresh (pages rebake
            // as sections re-enter the frame).
            if (hitCached) cacheEmisMask = surf4.a;
            const float3 surf = surf4.rgb;
            hitAO    = surf.r;
            hitRough = lerp(inst.roughnessMin, inst.roughnessMax, surf.g);
            hitMetal = lerp(inst.metalnessMin, inst.metalnessMax, surf.b);
        }
    }
    // Applied here, ONCE, after every path that could have resolved the mask -
    // including the ones that could not (see the fail-to-zero note above).
    emissive *= cacheEmisMask;

    // Shading-model routing (Lighting.ps parity): UNLIT pixels on screen return flat albedo
    // (+ the emissive seed via additive blend) with ZERO lighting; re-lighting them here would
    // make static lamps / glowing screens reflect sun-lit-or-shadowed PBR instead of their
    // constant on-screen color. Packed by DDGISceneAS into emissive.a (0 = Standard); the sun
    // shadow ray + local-light loop are skipped, the ambient terms are discarded at the
    // combine below.
    const bool hitUnlit = (uint)(inst.emissive.a + 0.5) == 1u;  // SHADING_MODEL_UNLIT

    // Real per-material toon knobs for NPR hits (defaults when unavailable).
    HRNprKnobs npr = (HRNprKnobs)0;
    if (hitNPR)
        npr = HR_LoadNprKnobs(inst.rtFlags);

    hitRough = max(hitRough, 0.045);   // Lighting.ps's BRDF stability floor
    const float  f0s    = 0.16 * inst.reflectance * inst.reflectance;
    const float3 F0h    = lerp(float3(f0s, f0s, f0s), albedo, hitMetal);
    const float3 Vh     = -R;   // view dir at the hit = back along the ray
    const float  NdotVh = saturate(dot(worldN, Vh));

    // ---- Direct light: sun (1 shadow ray) + analytic point/spot ----
    // Full Cook-Torrance (diffuse + specular), mirroring Lighting.ps's directional +
    // clustered-light loops: a Lambert-only form drops every highlight and makes RT-delivered
    // content read darker/flatter than the same content via SSR.
    float3 directLit = float3(0, 0, 0);
    {
        const float3 Ls    = -normalize(sunDirection);
        const float  NdotL = saturate(dot(worldN, Ls));
        if (!hitUnlit && dot(sunColor, sunColor) > 0.0)
        {
            if (hitNPR)
            {
                // Lighting.ps toon-block parity (every Toon pixel), driven by the
                // REAL material record. The ramp REPLACES NdotL (its U=0 end is the
                // authored non-black shadow floor, so the backlit side keeps the
                // on-screen minimum), while nprDirectLightScale and
                // NPRCoupleSceneLight keep a physically bright sun from blowing the
                // reflection out (typical body knobs: scale 0.25, intensity weight
                // 0.5; without them the reflection is flat full-brightness albedo).
                // The shadow ray is a plain multiplier, like directVisibility on
                // screen; full shadow falls to ambient + the minBrightness floor at
                // the return, where the screen's own floor lives too.
                const float vis  = ShadowVisibility(hitPos, Ls, 100.0);
                const float hl   = dot(worldN, Ls) * 0.5 + 0.5;   // RAW dot - no clamp
                const float sharp = max((1.0 - saturate(
                    max(npr.shadowSmooth, 0.001) * 2.0)) * 1.5, 0.06);
                const float form = saturate(1.0 / (1.0 + pow(100000.0,
                    -3.0 * sharp * (hl - npr.shadowThreshold))));
                const float shadowArea = lerp(1.0, form,
                                              saturate(npr.shadowStrength));
                // Authored ramp texture (V=0.875 = direct-shadow band) when
                // bound; else the material's two-color ramp - the same
                // fallback as Lighting.ps's toon block and the forward toon.
                float3 rampCol;
                if (npr.rampTexIdx >= 0)
                    rampCol = g_AllTextures[NonUniformResourceIndex(npr.rampTexIdx)]
                                  .SampleLevel(gSampClamp,
                                               float2(shadowArea, 0.875), 0).rgb;
                else
                    rampCol = lerp(npr.shadowRampColor, npr.diffuseRampColor,
                                   shadowArea);
                // NPRCoupleSceneLight + main-light lerp (Lighting.ps's toonLightColor).
                const float sunLum = max(Luminance(max(sunColor, 0.0)), 1e-5);
                const float3 chroma = lerp(float3(1.0, 1.0, 1.0),
                    max(sunColor, 0.0) / sunLum, saturate(npr.colorWeight));
                const float toonLum = lerp(max(npr.referenceLum, 1e-4), sunLum,
                                           saturate(npr.intensityWeight));
                const float3 toonLight = lerp(chroma * toonLum,
                    float3(1.0, 1.0, 1.0), saturate(npr.mainLightLerp));
                const float directOcc = lerp(1.0 - saturate(npr.directOcclusion),
                                             1.0, hitAO);
                directLit += albedo * (1.0 - hitMetal) * rampCol * toonLight
                           * (max(npr.directScale, 0.0) / PI) * directOcc * vis;

                // Sun toon SPECULAR - Lighting.ps toon-block parity. The
                // screen's nprSunSpecUngated is the RAW-radiance GGX sun
                // specular (accumulated in the PBR loop before the toon
                // override), entering the composite x directVisibility x
                // directOcc x nprSpecularLightScale. Do not drop it: with
                // typical character knobs (body threshold 0 -> near-flat
                // diffuse ramp, directScale 0.25) the highlights ARE most of
                // the visible NPR shading - hair shine, metal trims, leather.
                // The stepped spec-ramp stylization and the face glint are
                // approximated by the plain GGX lobe (correct energy, softer
                // edge).
                if (NdotL > 0.0 && vis > 0.0)
                {
                    float3 specNpr, kDNpr;
                    EvalCookTorrance(worldN, Vh, Ls, F0h, hitRough, hitMetal,
                                     albedo, specNpr, kDNpr);
                    directLit += specNpr * sunColor * (NdotL * vis) * directOcc
                               * max(npr.specularScale, 0.0);
                }
            }
            else if (NdotL > 0.0)
            {
                const float vis = ShadowVisibility(hitPos, Ls, 100.0);   // TDR-tuned clamp, matches DDGI
                if (vis > 0.0)
                {
                    float3 spec, kD;
                    EvalCookTorrance(worldN, Vh, Ls, F0h, hitRough, hitMetal, albedo, spec, kD);
                    directLit += (kD * albedo / PI + spec) * sunColor * (NdotL * vis);
                }
            }
        }
    }

    uint lightOffset;
    const uint nLights = hitUnlit ? 0u : WorldLightRange(worldLightGridIndex, hitPos, lightCount, lightOffset);
    // Visibility: exact up to HR_HIT_EXACT_LIGHTS, else one pick.
    const bool shadowLocal = (unifiedFlags & HR_FLAG_UNSHADOWED_HIT_LIGHTS) == 0u;
    const bool exactLights = !shadowLocal || nLights <= HR_HIT_EXACT_LIGHTS;
    uint   pickHash = HashPcg(asuint(hitPos.x) ^ HashPcg(asuint(hitPos.y)
                    ^ HashPcg(asuint(hitPos.z) ^ (frameIndex * 0x9E3779B9u))));
    float  wsum = 0.0, wsel = 0.0, dsel = 0.0;
    float3 csel = float3(0, 0, 0), lsel = float3(0, 1, 0);
    for (uint ci = 0; ci < nLights; ++ci)
    {
        uint li = WorldLightIndex(worldLightGridIndex, lightOffset, ci);
        GPULight Lgt = gLights[li];
        if (Lgt.type == 0u) continue;
        float3 Ldir;
        const float3 inc   = IncidentLocalLight(Lgt, hitPos, Ldir);
        const float  NdotL = saturate(dot(worldN, Ldir));
        if (NdotL <= 0.0 || dot(inc, inc) <= 0.0) continue;
        float3 spec, kD;
        EvalCookTorrance(worldN, Vh, Ldir, F0h, hitRough, hitMetal, albedo, spec, kD);
        // NPR: punctual diffuse enters the toon composite at the authored
        // nprAdditionalLightScale while punctual SPECULAR rides
        // nprSpecularLightScale (Lighting.ps routes nprPunctualSpec through
        // toonSpec); one shared scale would flatten highlights and overdrive
        // diffuse at the same time.
        float3 contribution;
        if (hitNPR)
            contribution = (kD * albedo / PI * max(npr.additionalScale, 0.0)
                          + spec * max(npr.specularScale, 0.0)) * inc * NdotL;
        else
            contribution = (kD * albedo / PI + spec) * inc * NdotL;
        const float dist = length(Lgt.position - hitPos);
        if (exactLights)
        {
            float vis = 1.0;
            [branch] if (shadowLocal) vis = HR_LocalLightVisibility(hitPos, Ldir, dist);
            directLit += contribution * vis;
            continue;
        }
        // Streaming RIS, one pick in proportion to the unshadowed luminance.
        const float w = Luminance(contribution);
        if (!(w > 0.0)) continue;
        wsum += w;
        pickHash = HashPcg(pickHash);
        if (float(pickHash >> 8) * (1.0 / 16777216.0) * wsum < w)
        { wsel = w; csel = contribution; lsel = Ldir; dsel = dist; }
    }
    if (!exactLights && wsel > 0.0)
        directLit += csel * (wsum / wsel) * HR_LocalLightVisibility(hitPos, lsel, dsel);

    // ---- Indirect light: read the DDGI probe field (no second bounce ray) ----
    // Multi-volume weighted blend, same pattern as Lighting.ps. DDGI_SampleVolume returns
    // E(N)/pi scaled by the PER-VOLUME diffuseTint/diffuseScale only; the GLOBAL LightCB
    // ddgiDiffuseScale that Lighting.ps applies on top is applied HERE via the CB, so
    // reflections of DDGI-lit content match the screen at ANY scale.
    const float3 Rh = reflect(R, worldN);   // R = incoming ray dir
    float3 ddgiIrr  = float3(0, 0, 0);
    float  coverage = 0.0;
    float3 ddgiSpecular = float3(0, 0, 0);
    float  ddgiSpecCoverage = 0.0;
    float  skyVisibility = 1.0;
    // The SPECULAR probe sample in the loop below feeds ONLY
    //   giW = smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
    //                    INDIRECT_SPECULAR_DDGI_ROUGH_END, hitRough)
    //         * ddgiSpecCoverage
    // and then `lerp(skyTerm, ddgiSpecular, giW)`. smoothstep is EXACTLY 0 at or below its lower
    // edge (0.4), so for any smoother hit the sample is multiplied by zero: lerp(x, y, 0) == x
    // bit-exactly for finite y. Skipping it saves one DDGI_SampleVolume (an 8-probe gather: a
    // depth-atlas and an irradiance-atlas tap per probe) per picked volume, up to 2 (the
    // DDGI_PickVolumes winners), on every such hit.
    // Only the specular sample is gated. The diffuse and sky-visibility samples feed ddgiIrr /
    // skyVisibility, which are used at every roughness, and must keep running.
    const bool needDDGISpec = (hitRough > INDIRECT_SPECULAR_DDGI_ROUGH_START);
    if (ddgiVolumeCount > 0u)
    {
        // Shared arbitration, see DDGISampling.hlsli. Unlike Lighting.ps, which keeps
        // coverage separate, THIS shader's downstream contract wants coverage folded
        // into the value; that is done below on the shared resolve.
        DDGIVolumePick pick = DDGI_PickVolumes(hitPos, gDDGIVolumes);

        float4 bestDiff   = float4(0, 0, 0, 0);
        float4 secondDiff = float4(0, 0, 0, 0);
        float4 bestSpec   = float4(0, 0, 0, 0);
        float4 secondSpec = float4(0, 0, 0, 0);
        float2 bestSky    = float2(1.0, 0.0);
        float2 secondSky  = float2(1.0, 0.0);

        [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
        {
            if (!DDGI_IsPicked(pick, vi)) continue;
            DDGIVolumeGPU vol = gDDGIVolumes[vi];
            const bool isBest = (vi == (uint)pick.bestSlot);

            float2 sv;
            float4 s = DDGI_SampleVolumeWithSky(hitPos, worldN, -R, vol,
                                         gDDGIIrradiance[vi], gDDGIDepth[vi],
                                         gDDGIProbeData[vi], gSampWrap, sv);
            if (isBest) bestDiff = s; else secondDiff = s;

            if (needDDGISpec)
            {
                float4 rs = DDGI_SampleVolume(hitPos, Rh, Vh, vol,
                                               gDDGIIrradiance[vi], gDDGIDepth[vi],
                                               gDDGIProbeData[vi], gSampWrap);
                if (isBest) bestSpec = rs; else secondSpec = rs;
            }


            if (isBest) bestSky = sv; else secondSky = sv;
        }

        const float4 diffResolved = DDGI_CombineWinners(bestDiff, secondDiff, pick);
        if (diffResolved.a > 1e-4)
        {
            coverage = diffResolved.a;
            // Coverage folded into the value - THIS shader's downstream
            // contract, unlike Lighting.ps which applies it later. Kept
            // deliberately; do not "unify" it without following ddgiIrr to its
            // consumer.
            ddgiIrr  = diffResolved.rgb * coverage;
        }

        const float4 specResolved = DDGI_CombineWinners(bestSpec, secondSpec, pick);
        if (specResolved.a > 1e-4)
        {
            ddgiSpecular     = specResolved.rgb * ddgiDiffuseScale;
            ddgiSpecCoverage = specResolved.a;
        }

        const float2 skyResolved = DDGI_CombineSkyVisibility(bestSky, secondSky, pick);
        if (skyResolved.y > 1e-4)
            skyVisibility = lerp(1.0, skyResolved.x, saturate(skyResolved.y));
    }

    // Hit ambient mirrors Lighting.ps's diffuse-IBL split: DDGI portion x global scale, with the
    // near-field AO DECOUPLING (DDGI already encodes probe-scale visibility, so full baked AO on
    // it stacks the same occlusion twice), + sky diffuse for the (1 - coverage) remainder x full
    // AO. Sky diffuse walks the IDENTICAL branch as Lighting.ps: SH in atmosphere mode, else the
    // dedicated cosine-convolved irradiance cube (bound real-or-black by the renderer, so the
    // unconditional sample is safe, as in the main view). Do not approximate it with the
    // radiance cube's top mip: that under-estimates irradiance and makes shadowed/backlit
    // content reflected via the RT tier darker than on screen.
    const float ddgiAOh  = lerp(1.0, hitAO, saturate(ddgiAONearFieldStrength));
    // DDGI's coverage / skyVisibility occlude this hit's sky terms when volumes exist. Without
    // volumes, under ReSTIR GI the main view still occludes ITS sky (Lighting.ps: skyVisibility
    // = 1 - the GI rays' coverage), so the hit traces its own visibility; otherwise a reflected
    // interior takes the open sky, diffuse here and in the glossy sky term below (too bright,
    // tinted blue). (An if, not ?: - under -HV 2018 both operands of ?: are evaluated, and the
    // ray would be traced for every hit.)
    float hitSkyVis = 1.0;
    if (ddgiVolumeCount == 0u && restirGiPrevIdx != 0u)
        hitSkyVis = HR_SkyVisibilityRay(hitPos, worldN, HashPcg(pickHash ^ 0x2545F491u));
    const float skyDiffW = iblStrength * (1.0 - coverage) * hitSkyVis;
    float3 hitAmbient = ddgiIrr * ddgiDiffuseScale * ddgiAOh;
    if (hasSkySH != 0u)
        hitAmbient += HR_EvalSkySH(worldN) * skyDiffW * hitAO;
    else
        hitAmbient += gSkyIrradiance.SampleLevel(gSampWrap, worldN, 0).rgb
                    * skyDiffW * hitAO;

    // ---- Combine (Lighting.ps parity, minus SSAO) ----
    // Indirect diffuse: kD-conserved albedo x E(N) (AO already folded per source above; SSAO
    // does not exist at an off-screen hit - hitAO is the surface map's R).
    // Indirect specular: probe tier first, the prefiltered sky-radiance cube fills the rest,
    // x split-sum EnvBRDFApprox; without it glossy/metallic content reflects near-black.
    // iblStrength gates it like Lighting.ps's sky-specular tier; the hit's direct + emissive
    // terms are NOT gated, same as the main view.
    const float3 Fibl  = FresnelSchlickRoughness(NdotVh, F0h, hitRough);
    const float3 kDibl = (1.0 - Fibl) * (1.0 - hitMetal);
    // NPR composite scales ambient by nprAmbientLightScale and the specular
    // sum by nprSpecularLightScale - authored per material (0.75 defaults).
    const float nprAmbScale  = hitNPR ? max(npr.ambientScale,  0.0) : 1.0;
    const float nprSpecScale = hitNPR ? max(npr.specularScale, 0.0) : 1.0;
    float3 indirect = kDibl * albedo * hitAmbient * nprAmbScale;
    {
        // Indirect specular at the hit - Lighting.ps parity (probe tier first, sky
        // fills the rest). The sky IBL alone would give content whose environment is
        // a local interior (where artists place reflection probes) the wrong color vs
        // the screen, and the screen/RT tier switch would turn that mismatch into
        // temporal color pumping.
        IndirectSpecularProbeAccum probes = IndirectSpecular_MakeProbeAccum();
        // Cap matches kMaxReflectionProbes: Lighting.ps walks the full per-cluster
        // list (any of the 64 pool slots), and a lower cap makes hits inside the
        // higher-numbered probes reflect the wrong (sky) environment. The
        // Coverage >= 0.999 early-out keeps the real cost bounded.
        const uint nProbes = min(probeCount, 64u);
        for (uint pi = 0; pi < nProbes && probes.coverage < 0.999; ++pi)
        {
            StructuredBuffer<ReflectionProbe> probeBuf = BindlessSBuf<ReflectionProbe>(probeBufferIdx);
            ReflectionProbe probe = probeBuf[pi];
            float w = ComputeProbeWeight(hitPos, probe);
            if (w <= 1e-3) continue;
            // Per-probe intensity scales radiance, NOT coverage weight -
            // same knob split as Lighting.ps.
            float4 probeSample = SampleProbe(probe, hitPos, Rh, hitRough, gSampClamp);
            IndirectSpecular_AddProbe(probes, probeSample, w, probe.intensity);
        }
        float3 skyTerm = float3(0, 0, 0);
        if (hasSkyCube != 0u)
        {
            // ESTIMATOR PARITY: gate by iblStrength unconditionally, like the
            // fallback in Lighting.ps
            //   skyTerm = skySpecular * (iblStrength * skyVisibility * horizon)
            // Do not ungate it inside the traced band. Lighting.ps writes
            //   Target0 = color - tracedSpecularDelta * transmittance
            //   tracedSpecularDelta = specIBL - fallbackSpecIBL
            // and Target0 IS the source of next frame's gPrevHDR pyramid, so the
            // screen-radiance cache has the traced reflection REMOVED and carries
            // the iblStrength-gated fallback instead. An ungated term synthesises
            // a specular bounce the cache omits, making an analytic hit up to
            // 1/iblStrength brighter than the same surface via screen reuse (~10x
            // at iblStrength 0.1), worst exactly where ssrConfidenceThreshold
            // sends unstable rays to the HW tier, so the two estimators alternate
            // on neighbouring pixels.
            // The TRUE sky-miss branch in main() and the SW tier's skyLanding are
            // gated the same way: a ray that sees the sky receives sky light, not
            // the visible skybox's radiance.
            const float mipH  = hitRough * float(max(skyCubeMips, 1u) - 1u);
            skyTerm = gSkyCube.SampleLevel(gSampWrap, Rh, mipH).rgb
                    * iblStrength
                    * skyVisibility * hitSkyVis
                    * IndirectSpecular_HorizonVisibility(Rh);
        }
        // Lumen-style rough-specular fallback: as roughness rises the specular
        // lobe converges to diffuse GI, so CROSSFADE the env radiance toward the
        // DDGI irradiance sampled along the reflection (ddgiSpecular, E/pi scale -
        // for a diffuse environment that IS the prefiltered radiance). UE's
        // CombineRoughSpecular semantics: a roughness-driven lerp between two
        // full-weight estimators of the same integral. Rough hits under a dark
        // sky/probe env but DDGI-lit ambient regain the indirect sheen their
        // on-screen image shows. Do not use a max() floor instead: it can only
        // lift, and on DDGI-lit content it ADDS specular energy Lighting.ps never
        // gives the same surface on screen.
        const float giW = smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
                                     INDIRECT_SPECULAR_DDGI_ROUGH_END,
                                     hitRough) * ddgiSpecCoverage;
        const float3 distant = lerp(skyTerm, ddgiSpecular, giW);
        const float3 envRad = IndirectSpecular_ResolveProbeFallback(probes, distant);
        const float2 eb = EnvBRDFApprox(hitRough, NdotVh);
        indirect += IndirectSpecular_ApplyEnvBRDF(envRad, F0h, eb)
                  * IndirectSpecular_SpecularOcclusion(hitAO, NdotVh, hitRough)
                  * nprSpecScale;
    }

    // Flat environment ambient (WorldEnvironment None / FlatColor): the renderer zeroes
    // flatAmbient in sky modes, so this is a no-op for lit-sky scenes, like Lighting.ps (no kD
    // there either). Without it, reflected content in FlatColor scenes is direct-light-only and
    // goes black in shadow.
    indirect += albedo * flatAmbient * hitAO * nprAmbScale;

    if (hitUnlit)
    {
        // Exposure protection, mirroring NPRStabilize's Unlit branch: an unlit
        // pixel is 100% authored, so exposureCompensation 1.0 cancels the
        // tonemap exposure multiply outright. Without it the same material holds
        // its authored brightness on screen but drifts with adaptation in a
        // mirror (unlit brows / lashes / iris next to already-immune Toon skin).
        return (albedo + emissive) *
               lerp(1.0, rcp(HR_SceneExposure()),
                    saturate(HR_LoadExposureComp(inst.rtFlags)));
    }

    float3 outCol = emissive + directLit + indirect;
    if (hitNPR)
    {
        // Lighting.ps's smooth luminance floor (albedo-tinted fill of the
        // authored min brightness, typically 0.2-0.3 on characters). Same formula
        // as the primary path: adds floorLum^2/(lum+floorLum) of luminance,
        // exactly floorLum at lum=0, hue blending toward albedo, no per-channel
        // crease.
        {
            const float minB = max(npr.minBrightness, 0.0);
            const float floorLum =
                Luminance(albedo) * minB;
            if (floorLum > 0.0)
            {
                const float lum = Luminance(outCol);
                outCol += albedo * minB * (floorLum / (lum + floorLum));
            }
        }
        // NPRStabilize parity: per-material EV bias, then the exposed-space
        // max-RGB artistic shoulder and SOFT absolute ceilings (per-material
        // min()ed with the scene-wide value, knee at half the cap). Exposure
        // comes from the AutoExposure GPU buffer over bindless - the same
        // prev-frame value NPRStabilize reads (it runs before AutoExposure in
        // the post chain). Without this block the reflected character misses
        // the compression the primary view gets and reads BRIGHTER in mirrors
        // under strong light.
        float exposure = 1.0;
        if (exposureBufIdx < 16384u)
        {
            const float e = asfloat(g_Buffers[exposureBufIdx].Load(0));
            if (isfinite(e) && e >= 1e-4) exposure = e;
        }
        // Exposure protection first, same order as NPRStabilize (protection ->
        // EV bias -> shoulder/ceilings). NPR materials take the FULL range:
        // exposureComp 1.0 cancels the tonemap exposure entirely, so the
        // reflected character stays as adaptation-immune as the primary view.
        outCol *= lerp(1.0, rcp(exposure), saturate(npr.exposureComp));
        outCol *= exp2(clamp(npr.exposureBiasEV, -2.0, 2.0));
        float3 exposed = outCol * exposure;
        const float signal = max(exposed.r, max(exposed.g, exposed.b));
        float s = signal;
        s = lerp(s, HR_SoftShoulder(s, max(npr.shoulderStart, 1e-4),
                                       max(npr.shoulderRange, 1e-4)),
                 saturate(npr.shoulderStrength));
        float maxSig = 3.402823466e+38;
        if (npr.maxBrightness > 0.0)     maxSig = min(maxSig, npr.maxBrightness);
        if (sceneNprMaxBrightness > 0.0) maxSig = min(maxSig, sceneNprMaxBrightness);
        if (maxSig < 3.402823466e+37)
            s = HR_SoftShoulder(s, 0.5 * maxSig, 0.5 * maxSig);
        if (signal > 1e-5) exposed *= s / signal;
        outCol = exposed / exposure;
    }

    // ---- Toon outline approximation ----
    // On screen the outline is an inverted-hull DrawCandidate the TLAS never sees, so it never
    // reaches a reflection. Grazing-incidence ink from the RAY's own viewpoint is the classic
    // RT stand-in: darken hits whose SMOOTH normal is near-perpendicular to the ray, which IS
    // the silhouette in the mirror image. Width is angular (not the raster's pixel-constant
    // outlinePixels) but reads as the same ink line at character scales. Gated on the
    // material's own outline flag, so only content that draws outlines on screen gets them in
    // mirrors.
    if ((inst.rtFlags & RT_INST_FLAG_OUTLINE) != 0u)
    {
        const float ndv  = saturate(dot(vertexN, Vh));
        const float ink  = 1.0 - smoothstep(0.10, 0.28, ndv);
        outCol *= lerp(1.0, 0.12, ink);
    }
    return outCol;
}

// ==== Entry ====
// One thread per ray in g_HWRayList (ExecuteIndirect-driven; group grid split 2D per the
// HR_GROUPS_PER_ROW convention in HybridReflCommon).
[numthreads(32, 1, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
    const uint rayIdx = (Gid.y * HR_GROUPS_PER_ROW + Gid.x)
                      * HR_INTERSECT_GROUP_SIZE + GTid.x;
    if (rayIdx >= g_RayCounter[HR_CTR_HW_HIST])
        return;

    const uint2 pixel = HR_UnpackRay(g_HWRayList[rayIdx]);
    if (pixel.x >= screenW || pixel.y >= screenH)
        return;

    const float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)   // reverse-Z: sky is EXACTLY 0.0 - defensive, the
    {                   // classifier never lists sky pixels
        OutRawColor[pixel] = float4(0, 0, 0, 0);
        OutHitDist[pixel]  = 0.0;
        return;
    }

    // ---- Re-derive the EXACT ray the classifier / SW pass produced ----
    // Determinism contract (HybridReflCommon): identical GBuffer inputs + identical helper =>
    // bit-identical direction. Never fork this per pass.
    const float2 uv       = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);
    const float3 worldPos = HR_ReconstructWorldPos(uv, depth);
    const float3 N        = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    const float3 V        = normalize(cameraPos - worldPos);

    const float roughness   = gSurface.Load(int3(pixel, 0)).r;
    const float roughFactor = HR_RoughFactor(roughness);
    // ---- Multi-sample estimator ----
    // HR_RAYS_PER_PIXEL == 1 (default): one iteration, seed == frameIndex, and the averages
    // below divide by 1. Raising it trades traversal cost for a 1/sqrt(N) cut in the 1-spp
    // variance REBLUR has to clean up.
    // COST WARNING: this is the TDR-budgeted tier (see the tMax leash below). N spp is
    // N x traversal for every pixel the screen-space tier could not resolve - raise it one
    // step at a time and watch for device removal on the heaviest view, not on a test scene.
    float3 accumRadiance = float3(0, 0, 0);
    float  accumConf     = 0.0;
    float  bestHitT      = 0.0;
    float  bestConf      = -1.0;
    bool   bestDynamic   = false;

    [loop]
    for (uint sampleIdx = 0; sampleIdx < HR_RAYS_PER_PIXEL; ++sampleIdx)
    {
        const float3 R = HR_SampleReflectionDir(
        V, N, roughness, pixel, frameIndex + sampleIdx * 977u);

        // Glossy rays get a shorter leash: distant glossy reflections are lobe-blurred into
        // the probe/sky tiers anyway, and the saved traversal keeps the full roughness band
        // inside the TDR budget of a mirror-only trace.
        const float tMax = rayTMax * lerp(1.0, 0.6, roughFactor);

        RayDesc ray;
        ray.Origin    = worldPos + N * normalBias;
        ray.Direction = R;
        ray.TMin      = 0.01;
        ray.TMax      = tMax;   // hard TDR-budget clamp

        // Backface culling stays OFF: a backface hit means the mirror looks at geometry
        // from inside/behind, and handling it (instead of culling and hitting whatever is
        // BEHIND the wall) avoids see-through leaks; see the backface branch below.
        //
        // RAY_FLAG_FORCE_OPAQUE is deliberately absent: alpha-clip instances carry
        // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE, so traversal reports
        // CANDIDATE_NON_OPAQUE_TRIANGLE for them and ONLY for them; every other
        // instance's BLAS geometry is FLAG_OPAQUE and resolves entirely in fixed-function.
        // Inclusion mask = cbuffer rayMask, 0x08 on both instances: forward-translucent
        // panes are not in it, so the opaque trace never commits glass as solid and the
        // front-layer trace's rays never see the pane they start on.
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, rayMask, ray);
        [loop]
        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                // GBuffer parity: baseTex.a * baseColor.a.
                if (HR_AlphaAccept(q.CandidateInstanceID(),
                                   q.CandidatePrimitiveIndex(),
                                   q.CandidateTriangleBarycentrics(),
                                   /*includeBaseColorAlpha*/true))
                    q.CommitNonOpaqueTriangleHit();
            }
        }

        float3 radiance = float3(0, 0, 0);
        float  conf     = 0.0;
        float  hitT     = 0.0;
        bool   dynamicHit = false;

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            // Unlike the mirror pixel's GBuffer velocity, the TLAS record
            // identifies deforming geometry directly. Carry this fact to the
            // NRD pack stage through the sign of hitT; its magnitude remains
            // the real world-space distance.
            dynamicHit = (HR_InstFlags(q.CommittedInstanceID()) &
                          RT_INST_FLAG_SKINNED) != 0u;
            if (q.CommittedTriangleFrontFace())
            {
                hitT = q.CommittedRayT();
                const float3 hitPos = ray.Origin + R * hitT;

                // ---- Screen-radiance reuse FIRST (UE / AMD hybrid style) ----
                // CURRENT-frame validated + motion-vector fetched: project the hit
                // with THIS frame's viewProj, verify the hit IS the surface the depth
                // buffer shows there (thickness-tested: an occluded hit must not
                // steal the occluder's color), then let HR_FetchPrevRadiance do the
                // mv-decoded prev-pyramid fetch (the SAME source the SW tier reads,
                // so tier colors match by construction). Off-screen / occluded /
                // first-frame hits fall through to the analytic material-proxy path.
                // DEFORMING reflectors never screen-reuse. The fetch is prev-frame
                // data: pose N-1 shading pasted onto pose-N BLAS geometry, the
                // moving-surface branch of HR_FetchPrevRadiance has no z-oracle to
                // catch self-disocclusion, and the depthThicknessBase (0.2 wu) gate
                // lets a mirror-facing hit steal the CAMERA-facing side's color
                // through any limb thinner than the tolerance. In the mirror band
                // NRDResolve displays this raw trace with no denoiser pass over it,
                // so a reuse/analytic patchwork reads as per-frame tearing of the
                // reflected character. One estimator for the whole deforming
                // reflector: skinned hits always take the analytic path below,
                // pose-consistent by construction, and the surface cache page
                // carries the material's real look.
                bool reused = false;
                const float4 prj = HR_ProjectToScreen(hitPos);
                if (!dynamicHit && hasPrevHdr != 0u && prj.w > 1e-3 &&
                    all(prj.xy > 0.0) && all(prj.xy < 1.0) && prj.z > 0.0)
                {
                    const float2 screenDim = float2(screenW, screenH);
                    const int2 sPx = int2(clamp(prj.xy * screenDim,
                                                float2(0.0, 0.0), screenDim - 1.0));
                    const float surfD = gDepth.Load(int3(sPx, 0));
                    if (surfD > 0.0)
                    {
                        const float linHit  = HR_LinearizeReverseZ(saturate(prj.z));
                        const float linSurf = HR_LinearizeReverseZ(surfD);
                        if (abs(linHit - linSurf) <
                            max(depthThicknessBase, 0.02 * linSurf))
                        {
                            float3 rad;
                            bool screenDynamic;
                            if (HR_FetchPrevRadiance(hitPos, prj.xy, roughFactor,
                                                     hitT, rad, screenDynamic))
                            {
                                radiance = rad;
                                reused   = true;
                                dynamicHit = dynamicHit || screenDynamic;
                            }
                        }
                    }
                }

                // Screen reuse above is the ONLY tier that carries a custom
                // shader's real look, because it reads an already-shaded pixel.
                // The analytic path below re-derives the surface from the 128-byte
                // proxy, and for a custom-shader material that proxy is a
                // name-alias GUESS. When even the guess fails
                // (RT_INST_FLAG_PROXY_UNTRUSTED: no space3 alias matched and the
                // engine slot is still a placeholder) shading it gives the
                // material's baseColor times a 2x2 WHITE default texture, an
                // over-bright featureless mannequin - far worse than no character
                // at all, and it poisons DDGI through the same table.
                // Substitute the SAME environment estimate the single-sided
                // backface path uses, in RADIANCE space so REBLUR integrates it
                // and the fold weight stays stable. Once the material surface
                // cache bakes a page for this material the flag clears and this
                // branch stops firing.
                bool envSubstituted = false;
                if (!reused)
                {
                    if ((HR_InstFlags(q.CommittedInstanceID()) &
                         RT_INST_FLAG_PROXY_UNTRUSTED) != 0u)
                    {
                        float envW;
                        radiance       = HR_MissEnvRadiance(worldPos, N, V, R,
                                                            roughness, envW, true);
                        conf           = envW;
                        // No real surface to report - park hitT at the leash so
                        // REBLUR's virtual-motion reprojection treats it as a far
                        // reflector rather than a zero-distance hit.
                        hitT           = rayTMax;
                        envSubstituted = true;
                    }
                    else
                    {
                        radiance = HR_ShadeHit(q.CommittedInstanceID(),
                                               q.CommittedPrimitiveIndex(),
                                               q.CommittedTriangleBarycentrics(),
                                               q.CommittedObjectToWorld3x4(),
                                               hitPos, R);
                    }
                }

                // Solid front-face hit = FULL-coverage sample, conf 1 (a distance
                // falloff here would wrongly blend the probe tier into solid
                // hits; NRDResolve owns the probe/sky hand-off).
                if (!envSubstituted)
                    conf = 1.0;
            }
            else
            {
                // ---- Backface ----
                // DOUBLE-SIDED geometry (foliage/hair/fence cards) legitimately
                // presents its back face to a reflection ray: the raster path
                // disables culling for it, so it is visible from both sides on
                // screen. Do not report conf 0 for it: relia 0 makes NRDResolve
                // write OutDenoised = 0, which Lighting.ps reads as rtConf 0 and
                // replaces with the probe/flat-ambient specular, so the card does
                // NOT APPEAR in the mirror (about half of every double-sided card
                // cluster). Single-sided geometry takes the environment stand-in
                // below: a backface there means the ray looked at a wall from
                // inside, and committing traced radiance would leak THROUGH it.
                // Read the two-sidedness from the INSTANCE record, not only from
                // the alpha table: HR_AlphaFlags returns 0 whenever
                // `alphaTestEnabled` is 0, and that CB flag is the AND of three
                // unrelated resources (alpha record VA, bindless buffer table,
                // bindless texture table). A frame where one of them is
                // momentarily unavailable (world reload, OnWorldClear, a texture
                // table rebind) would turn every two-sided material single-sided
                // and drop much of the hair and cloth out of reflections. The
                // alpha-table bit stays as a fallback for instances without the
                // instance flag.
                if ((HR_InstFlags(q.CommittedInstanceID()) & RT_INST_FLAG_DOUBLE_SIDED) ||
                    (HR_AlphaFlags(q.CommittedInstanceID()) & RT_ALPHA_FLAG_DOUBLE_SIDED))
                {
                    hitT = q.CommittedRayT();
                    const float3 bfHitPos = ray.Origin + R * hitT;
                    radiance = HR_ShadeHit(q.CommittedInstanceID(),
                                           q.CommittedPrimitiveIndex(),
                                           q.CommittedTriangleBarycentrics(),
                                           q.CommittedObjectToWorld3x4(),
                                           bfHitPos, R, /*flipN*/true);
                    conf = 1.0;
                }
                else
                {
                    // Single-sided backface: the ray is looking at a wall from
                    // inside. Committing traced radiance would leak through the
                    // wall, but conf 0 is NOT the way to express that.
                    // conf is the t44 fold alpha, which REBLUR never filters, and
                    // above mirrorRoughness the ray direction is a per-frame VNDF
                    // sample, so "did THIS ray hit a backface?" is a coin flip
                    // that re-rolls every frame. Written into conf it makes
                    // Lighting pick between the traced tier and the probe/sky tier
                    // per pixel per frame: salt-and-pepper that no REBLUR setting
                    // can touch.
                    // conf 0 means "let Lighting substitute the probe/flat-ambient
                    // tier", so substitute that SAME environment estimate HERE, in
                    // radiance space, with a stable alpha: identical intent, but
                    // REBLUR integrates it and the fold weight does not flicker.
                    // The SW tier follows the same contract (hybrid_refl_env.hlsli).
                    float envW;
                    radiance = HR_MissEnvRadiance(worldPos, N, V, R, roughness, envW, true);
                    conf     = envW;
                    // Env stand-in has no surface - park hitT at the ray leash so
                    // REBLUR treats it as a far reflector (matches the miss path
                    // below) instead of a zero-distance hit, which would poison
                    // the virtual-motion reprojection.
                    hitT     = rayTMax;
                }
            }
        }
        else
        {
            // ---- TRUE miss ----
            if (hasSkyCube != 0u)
            {
                // The sky LIGHT: sky radiance x iblStrength, the convention every
                // other sky term uses (Lighting.ps's fallback above the traced band,
                // DDGI's probe misses, ReSTIR GI and the path tracer at every vertex;
                // UE's SkyLight intensity scales the sky's diffuse AND specular
                // contribution, Lumen's reflection misses included). The visible
                // skybox is drawn unscaled; a reflection of the sky is sky light. Do
                // not reflect the FULL sky over part of the band: a floor whose
                // roughness straddles that edge takes a 1/iblStrength brightness step
                // per texel. The SW tier's sky landing and HR_MissEnvRadiance match
                // this exactly.
                // HR_SkyMissMip prefilters the cube over the TRACED lobe; mip 0 at
                // every roughness makes each jittered rough ray read a near-sharp sky,
                // the largest source-side variance injector on rough metal that
                // reflects sky or an opening. EXACTLY 0.0 at roughness <=
                // mirrorRoughness. MUST stay identical to the SW tier's sky-landing
                // sample.
                // HR_ClampEnvRadiance applies too: the low mips of the cube contain
                // the SUN DISK, and un-clamped it turns escape rays into 64-cap
                // fireflies REBLUR smears into glowing patches indoors (see the
                // helper's note).
                radiance = HR_ClampEnvRadiance(
                               gSkyCube.SampleLevel(gSampWrap, R,
                                                    HR_SkyMissMip(roughness)).rgb)
                         * iblStrength;
                conf     = 1.0;
            }
            else
            {
                // No sky cube (None/FlatColor environments). Same radiance-space
                // rule as the backface branch: hand off the probe/DDGI estimate
                // instead of writing a black conf-0 sample. envW carries the
                // fallback COVERAGE, so a FlatColor scene with no probes still
                // reports ~0 and lets Lighting's flat-ambient fill own the pixel,
                // but as a smooth spatial function rather than a per-ray coin flip.
                float envW;   // a TRUE miss: the ray escaped, nothing occludes it
                radiance = HR_MissEnvRadiance(worldPos, N, V, R, roughness, envW, false);
                conf     = envW;
            }
            // Sky sits at max range in BOTH miss flavors: REBLUR treats "sky at TMax" as a far
            // reflector, which keeps its history stable on mostly-miss lobes. UNLEASHED base
            // rayTMax, matching the SW tier's sky-landing hitDist exactly; the leashed tMax would
            // step the packed normHitDist by up to 1.67x whenever the hit counter flips a
            // sky-reflecting tile between FULL_SW and FULL_HW.
            hitT = rayTMax;
        }

        // Firefly cap at the SHARED write site so EVERY branch (analytic, screen-reuse,
        // sky-miss) clips to one consistent dynamic range: capping only the hit branch makes
        // >64 HDR sky content (sun disk in the prefiltered cube) pump against capped geometry
        // hits as the VNDF jitter alternates outcomes along silhouettes. Mirror pixels stay
        // uncapped (deterministic ray = temporally stable radiance). Same cap + gate as the SW
        // tier's write site.
        if (roughness > mirrorRoughness)
            // Hue-preserving + roughness-aware (HR_ClampTracedRadiance): a
            // per-channel min desaturates bright samples toward white, and a
            // different cap per estimator (picked per pixel) defeats REBLUR's
            // sigma clamp. Mirrors stay uncapped via the guard above.
            radiance = HR_ClampTracedRadiance(radiance, roughFactor);

        // min()/max() do not strip NaN - scrub before the write; this feeds the
        // NRD accumulator (same rule as DDGIRayTrace's write boundary).
        if (any(isnan(radiance)) || any(isinf(radiance)))
        {
            radiance = float3(0, 0, 0);
            conf     = 0.0;
            hitT     = 0.0;
        }

        accumRadiance += radiance;
        accumConf     += conf;
        // hitDist is PICKED, never averaged: REBLUR's virtual-motion
        // reprojection needs a consistent field, and the mean of a near hit
        // and a sky miss describes no real reflector (same rule as the SW
        // tier's dominant-tier pick).
        if (conf > bestConf)
        {
            bestConf    = conf;
            bestHitT    = hitT;
            bestDynamic = dynamicHit;
        }
    }

    const float  invSamples = 1.0 / float(HR_RAYS_PER_PIXEL);
    const float3 radiance   = accumRadiance * invSamples;
    const float  conf       = accumConf * invSamples;
    // R16F preserves this sign exactly. NRDPack consumes it as a private
    // dynamic-reflector marker and restores abs(hitT) before passing distance
    // to REBLUR, so no denoiser distance contract changes.
    const float  hitT       = (bestDynamic && bestHitT > 0.0)
                            ? -bestHitT : bestHitT;

    OutRawColor[pixel] = float4(radiance, conf);
    OutHitDist[pixel]  = hitT;
}
