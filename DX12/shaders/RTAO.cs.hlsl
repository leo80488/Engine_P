// Ray-traced ambient occlusion: fused trace + NRD (REBLUR_DIFFUSE_OCCLUSION) pack.
//
// Per pixel: rayCount cosine-weighted hemisphere rays (R2 low-discrepancy + per-pixel
// Cranley-Patterson rotation), inline RayQuery against the shared scene TLAS (mask 0x01,
// includes skinned characters). Instead of a screen-space AO factor it writes the REBLUR
// occlusion input quartet directly:
//
//   u0  OutDiffHitDist   R8           REBLUR_FrontEnd_GetNormHitDist (norm AO)
//   u1  OutNormalRough   R10G10B10A2  NRD_FrontEnd_PackNormalAndRoughness
//   u2  OutViewZ         R32F         linear view-space Z (sky -> huge)
//   u3  OutMV            RG16F        pixelUvPrev = pixelUv + mv.xy (UV units)
//
// REBLUR denoises OUT_DIFF_HITDIST to a [0,1] occlusion (1 = unoccluded).
//
// Root signature map, must stay in sync with RTAOPass.cpp:
//   [0] ROOT_CBV b0  RTAOConstants
//   [1] ROOT_SRV t0  TLAS
//   [2] TABLE    t1  gDepth   (reverse-Z hardware depth)
//   [3] TABLE    t2  gNormal  (GBuffer world normal; .a = material index, bit 15 = no-SSAO)
//   [4] TABLE    t3  gVelocity(NDC delta = curNDC - prevNDC)
//   [5] TABLE    u0  OutDiffHitDist
//   [6] TABLE    u1  OutNormalRough
//   [7] TABLE    u2  OutViewZ
//   [8] TABLE    u3  OutMV
//   [9] TABLE    t4  gAoIn        (resolve only)
//  [10] TABLE    u4  OutAo        (resolve only)
//  [11] ROOT_SRV t5          g_AlphaRecords (RTAlphaRecord, 40B)
//  [12] TABLE N  t0.. space1 bindless ByteAddressBuffer (VB/IB -> candidate UVs)
//  [13] TABLE N  t0.. space2 bindless Texture2D (cutout alpha)
//  [14] ROOT_SRV t6          g_Instances    (RTInstanceData, 128B): cutout UV xform
//   s1 space0 - static linear WRAP (material textures; MaxLOD = FLOAT32_MAX)

cbuffer RTAOConstants : register(b0, space0)
{
    float4x4 invViewProj;      // current, jittered: matches the depth buffer (uploaded transposed)
    uint     screenW;          uint  screenH;
    float    invScreenW;       float invScreenH;
    float    aoRadius;         float normalBias;
    uint     rayCount;         uint  frameIndex;
    float    nearZ;            float farZ;
    float    aoPower;          uint  alphaTestEnabled;  // aoPower used by the resolve pass
    uint     alphaInstanceCount; float alphaMipLevel;
};

#define NRD_NORMAL_ENCODING    2   // R10_G10_B10_A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1   // linear roughness
#include "NRD.hlsli"

RaytracingAccelerationStructure g_TLAS   : register(t0, space0);
Texture2D<float>                gDepth   : register(t1, space0);
Texture2D<float4>               gNormal  : register(t2, space0);
Texture2D<float2>               gVelocity: register(t3, space0);

RWTexture2D<float>  OutDiffHitDist : register(u0, space0);
RWTexture2D<float4> OutNormalRough : register(u1, space0);
RWTexture2D<float>  OutViewZ       : register(u2, space0);
RWTexture2D<float2> OutMV          : register(u3, space0);

// ==== Alpha-cutout occluders ====
// RTAO fully replaces XeGTAO as the frame's AO source when active, so without the cutout a
// solid foliage canopy stamps a hard AO disc the raster GBuffer never produces.
#include "rt_alpha_record.hlsli"
#include "rt_instance_data.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferMatIdxNoSsao
StructuredBuffer<RTAlphaRecord> g_AlphaRecords       : register(t5, space0);
// Per-instance data (128 B), same InstanceID() index space as g_AlphaRecords. Bound only so
// the candidate alpha test can apply the material UV transform, which the compact 40 B record
// does not carry. Gated by alphaTestEnabled like g_AlphaRecords: the root SRV is bound
// unconditionally and may be VA 0.
StructuredBuffer<RTInstanceData> g_Instances         : register(t6, space0);
ByteAddressBuffer               g_Buffers[16384]     : register(t0, space1);
Texture2D                       g_AllTextures[16384] : register(t0, space2);
SamplerState                    gSampWrap            : register(s1, space0);

#define RTAO_BINDLESS_LIMIT 16384u

#define RTAO_VISIBLE_MASK 0x01u

// Index fetch: lean clone of HR_LoadIndex / RTS_LoadIndex.
uint RTAO_LoadIndex(uint ibIdx, uint i, uint indexBytes)
{
    if (indexBytes == 2u)
    {
        const uint byteOff = i * 2u;
        const uint word    = g_Buffers[ibIdx].Load(byteOff & ~3u);
        return (byteOff & 2u) ? (word >> 16) : (word & 0xFFFFu);
    }
    return g_Buffers[ibIdx].Load(i * 4u);
}

// Accept this candidate as an occluder? Occlusion is a visibility question, so this follows
// the shadow convention: texture alpha alone, raw compare, and every failure path returns
// true (solid).
bool RTAO_AlphaAccept(uint instId, uint primIdx, float2 bary)
{
    // Must precede any g_AlphaRecords access: the root SRV is bound unconditionally and may be
    // VA 0.
    if (alphaTestEnabled == 0u)       return true;
    if (instId >= alphaInstanceCount) return true;

    const RTAlphaRecord a = g_AlphaRecords[instId];
    // Alpha-blend (translucent) instances: glass and smoke do not occlude ambient light, so
    // reject outright. Check this before the ALPHA_CLIP guard: blend instances are
    // FORCE_NON_OPAQUE (so RTShadow can attenuate through them) and would otherwise take the
    // "not a cutout -> solid" path and darken AO behind every window. Also skips the bindless
    // VB/IB fetch + texture sample.
    if ((a.flags & RT_ALPHA_FLAG_BLEND) != 0u)                           return false;
    if ((a.flags & RT_ALPHA_FLAG_ALPHA_CLIP) == 0u)                       return true;
    if (a.albedoTexIdx < 0 || a.albedoTexIdx >= (int)RTAO_BINDLESS_LIMIT) return true;
    if (a.vbBindlessIdx >= RTAO_BINDLESS_LIMIT ||
        a.ibBindlessIdx >= RTAO_BINDLESS_LIMIT)                           return true;

    const uint vbIdx = NonUniformResourceIndex(a.vbBindlessIdx);
    const uint ibIdx = NonUniformResourceIndex(a.ibBindlessIdx);
    const uint indexBytes = (a.indexBytes == 2u) ? 2u : 4u;
    const uint i0 = RTAO_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 0u, indexBytes);
    const uint i1 = RTAO_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 1u, indexBytes);
    const uint i2 = RTAO_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 2u, indexBytes);

    float2 uv0, uv1, uv2;
    if (a.flags & RT_ALPHA_FLAG_DEINTERLEAVED)
    {
        if (a.uvBufIdx >= RTAO_BINDLESS_LIMIT) return true;
        const uint uvIdx = NonUniformResourceIndex(a.uvBufIdx);
        uv0 = asfloat(g_Buffers[uvIdx].Load2((a.uvStart + i0) * 8u));
        uv1 = asfloat(g_Buffers[uvIdx].Load2((a.uvStart + i1) * 8u));
        uv2 = asfloat(g_Buffers[uvIdx].Load2((a.uvStart + i2) * 8u));
    }
    else
    {
        const uint stride = max(a.vertexStride, 32u);
        uv0 = asfloat(g_Buffers[vbIdx].Load2(i0 * stride + 24u));
        uv1 = asfloat(g_Buffers[vbIdx].Load2(i1 * stride + 24u));
        uv2 = asfloat(g_Buffers[vbIdx].Load2(i2 * stride + 24u));
    }

    const float  w   = 1.0 - bary.x - bary.y;
    // Material UV transform. The compact 40 B alpha record (fetched per traversal candidate)
    // does not carry it, so read it from the 128 B instance table: same InstanceID() index space,
    // and alphaTestEnabled already gates both buffers. Without it a tiled cutout occludes ambient
    // on a different silhouette than it renders with.
    const float2 uv  = RTInstanceUV(g_Instances[instId],
                                    uv0 * w + uv1 * bary.x + uv2 * bary.y);

    // No ray differentials in an inline RayQuery: explicit mip required.
    // .r when the record carries a Masked-mode opacity mask, else .a.
    const float alpha = RTAlphaSampleAlpha(
        a, g_AllTextures[NonUniformResourceIndex((uint)a.albedoTexIdx)],
        gSampWrap, uv, alphaMipLevel);
    return alpha >= a.alphaRef;
}

// hitDistanceParameters: must match nrd::ReblurSettings::hitDistanceParameters set by
// NrdDenoiser for Kind::ReblurDiffuseOcclusion, {A = aoRadius, B = 0, C = 1}, so f == aoRadius
// and a miss (hitDist == aoRadius) normalizes to exactly 1 = unoccluded. The REBLUR_SPECULAR
// defaults {3, 0.1, 20} would crush a miss to ~0.3 and darken the scene with distance.
#define kAOHitDistParams float3(aoRadius, 0.0, 1.0)

#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float RTAO_LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

float3 RTAO_ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

#include "core/hash.hlsli"

#include "core/sampling.hlsli"   // OrthonormalBasis, SampleGGXVNDF, Hammersley2D

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= screenW || pixel.y >= screenH)
        return;

    const float depth = gDepth.Load(int3(pixel, 0));
    const float4 normalRaw = gNormal.Load(int3(pixel, 0));
    const float3 N = normalize(normalRaw.rgb * 2.0 - 1.0);

    // NRD guides, written for every pixel.
    OutNormalRough[pixel] = NRD_FrontEnd_PackNormalAndRoughness(N, 1.0, 0.0);
    const float viewZ = (depth > 0.0) ? RTAO_LinearizeReverseZ(depth) : 1e7;
    OutViewZ[pixel] = viewZ;
    const float2 ndcDelta = gVelocity.Load(int3(pixel, 0));
    OutMV[pixel] = float2(-0.5 * ndcDelta.x, 0.5 * ndcDelta.y);

    // Sky (reverse-Z depth == 0) or a material opted out of SSAO (normal.a bit 15,
    // MAT_FLAG_EXCLUDE_FROM_SSAO; RT1 is UNORM, so .a is an exact 16-bit int) -> unoccluded. Feed
    // the max-distance normalized hit (-> AO 1) so NRD's edge-stopping still has a valid guide
    // sample here.
    if (depth <= 0.0 || GBufferMatIdxNoSsao(normalRaw.a))
    {
        OutDiffHitDist[pixel] = REBLUR_FrontEnd_GetNormHitDist(aoRadius, viewZ, kAOHitDistParams, 1.0);
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);
    const float3 worldPos = RTAO_ReconstructWorldPos(uv, depth);

    float3 T, B;
    OrthonormalBasis(N, T, B);

    const uint nRays = clamp(rayCount, 1u, 8u);
    float avgHit = 0.0;

    [loop]
    for (uint i = 0; i < nRays; ++i)
    {
        const float2 xi = R2PixelFrame2D(pixel, frameIndex * nRays + i);
        const float r   = sqrt(xi.x);
        const float phi = xi.y * 6.2831853;
        const float3 dir = normalize(T * (r * cos(phi)) +
                                     B * (r * sin(phi)) +
                                     N * sqrt(max(0.0, 1.0 - xi.x)));

        RayDesc ray;
        ray.Origin    = worldPos + N * normalBias;
        ray.Direction = dir;
        ray.TMin      = 0.02;
        ray.TMax      = aoRadius;

        // Do not add RAY_FLAG_FORCE_OPAQUE: it outranks the per-instance flag and makes the cutout
        // unreachable. Only the non-opaque instances (alpha-clip and alpha-blend, FORCE_NON_OPAQUE)
        // enter the candidate branch; every other BLAS is FLAG_OPAQUE and resolves in fixed function,
        // and AO rays are bounded by aoRadius, so candidate counts stay small. Not ACCEPT_FIRST_HIT:
        // avgHit needs the nearest occluder distance, and first-hit would return an arbitrary one.
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, RTAO_VISIBLE_MASK, ray);
        [loop]
        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                if (RTAO_AlphaAccept(q.CandidateInstanceID(),
                                     q.CandidatePrimitiveIndex(),
                                     q.CandidateTriangleBarycentrics()))
                    q.CommitNonOpaqueTriangleHit();
            }
        }

        // Miss -> full distance (unoccluded); hit -> occluder distance.
        avgHit += (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
                ? q.CommittedRayT() : aoRadius;
    }
    avgHit /= float(nRays);

    // Normalized hit distance for REBLUR occlusion (roughness 1 = diffuse lobe).
    OutDiffHitDist[pixel] = REBLUR_FrontEnd_GetNormHitDist(avgHit, viewZ, kAOHitDistParams, 1.0);
}
