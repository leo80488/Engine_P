// RTShadow.cs.hlsl - ray-traced sun shadow, fused trace + NRD SIGMA input pack.
//
// One inline-RayQuery shadow ray per pixel toward the sun, jittered inside the
// sun's angular cone (R2 low-discrepancy + per-pixel Cranley-Patterson rotation)
// so SIGMA integrates a soft penumbra. Writes the SIGMA input set directly:
//   u0  OutPenumbra      R16F         SIGMA_FrontEnd_PackPenumbra(hitT, tanAng)
//   u1  OutNormalRough   R10G10B10A2  NRD_FrontEnd_PackNormalAndRoughness
//   u2  OutViewZ         R32F         linear view-space Z (sky -> huge)
//   u3  OutMV            RG16F        pixelUvPrev = pixelUv + mv.xy (UV units)
//   u5  OutTranslucency  RGBA8        SIGMA_FrontEnd_PackTranslucency(hitT, t.xxx)
//
// Translucent occluders: the denoiser is SIGMA_SHADOW_TRANSLUCENCY. Alpha-blend
// instances (glass, water planes, smoke cards) carry RT_ALPHA_FLAG_BLEND and are
// never committed: their transmittance multiplies into a running product while
// traversal continues, so a 30%-opaque window darkens its shadow by 30% instead
// of casting a solid silhouette. The scalar visibility rides IN_TRANSLUCENCY
// (grey t.xxx) and gets the same spatial+temporal filter as the binary shadow,
// because SIGMA's blur seeds the whole float4 SIGMA_TYPE from that texture when
// TRANSLUCENCY==1. The resolve therefore reads .y (translucent shadow), not .x
// (binary shadow, still 0 for a translucent-only pixel).
//
// Shadow rays trace instance-mask bit 1 (0x02) = "casts RT sun shadow"
// (VisibilityComponent::CastsShadow && MaterialComponent::IsCastingShadow),
// which DDGISceneAS also grants to skinned characters.
//
// Root signature map - must stay in sync with RTShadowPass.cpp:
//   [0] ROOT_CBV  b0  RTShadowConstants (rt_shadow_common.hlsli)
//   [1] ROOT_SRV  t0  TLAS
//   [2] TABLE     t1  gDepth      (reverse-Z hardware depth)
//   [3] TABLE     t2  gNormal     (GBuffer world normal, *2-1)
//   [4] TABLE     t3  gSurface    (r = roughness)
//   [5] TABLE     t4  gVelocity   (NDC delta = curNDC - prevNDC)
//   [6] TABLE     u0  OutPenumbra
//   [7] TABLE     u1  OutNormalRough
//   [8] TABLE     u2  OutViewZ
//   [9] TABLE     u3  OutMV
//  [10] TABLE     t5  gShadowIn      (resolve only)
//  [11] TABLE     t6  gDepthResolve  (resolve only)
//  [12] TABLE     u4  OutFinal       (resolve only)
//  [13] ROOT_SRV  t7          g_AlphaRecords (RTAlphaRecord, 40B)
//  [14] TABLE N   t0.. space1 bindless ByteAddressBuffer (VB/IB -> candidate UVs)
//  [15] TABLE N   t0.. space2 bindless Texture2D (cutout alpha)
//  [16] TABLE     u5  OutTranslucency
//  [17] ROOT_SRV  t8          g_Instances    (RTInstanceData, 128B) - cutout UV xform
//   s1 space0 - static linear WRAP (material textures; MaxLOD = FLOAT32_MAX)

#include "rt_shadow_common.hlsli"

#define NRD_NORMAL_ENCODING    2   // R10_G10_B10_A2_UNORM
#define NRD_ROUGHNESS_ENCODING 1   // linear roughness
#include "NRD.hlsli"

RaytracingAccelerationStructure g_TLAS   : register(t0, space0);
Texture2D<float>                gDepth   : register(t1, space0);
Texture2D<float4>               gNormal  : register(t2, space0);
Texture2D<float4>               gSurface : register(t3, space0);
Texture2D<float2>               gVelocity: register(t4, space0);

RWTexture2D<float>  OutPenumbra    : register(u0, space0);
RWTexture2D<float4> OutNormalRough : register(u1, space0);
RWTexture2D<float>  OutViewZ       : register(u2, space0);
RWTexture2D<float2> OutMV          : register(u3, space0);
// SIGMA_SHADOW_TRANSLUCENCY IN_TRANSLUCENCY: .x = lit bit, .yzw = transmittance.
RWTexture2D<float4> OutTranslucency: register(u5, space0);

// ==== Alpha-cutout and blend occluders ====
// Alpha-clip instances and blend occluders with a nonzero opacity carry
// D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE (set by DDGISceneAS), so
// traversal reports them as candidates and the cutout or the opacity is tested
// before committing. Everything else keeps its BLAS FLAG_OPAQUE and never
// reaches the candidate branch.
#include "rt_alpha_record.hlsli"
#include "rt_instance_data.hlsli"
StructuredBuffer<RTAlphaRecord> g_AlphaRecords       : register(t7, space0);
// Per-instance data (128 B), same InstanceID() index space as g_AlphaRecords.
// Bound only so the candidate alpha test can apply the material UV transform,
// which the compact 40 B record does not carry. Gated by alphaTestEnabled like
// g_AlphaRecords: the root SRV is bound unconditionally and may be VA 0.
StructuredBuffer<RTInstanceData> g_Instances         : register(t8, space0);
ByteAddressBuffer               g_Buffers[16384]     : register(t0, space1);
Texture2D                       g_AllTextures[16384] : register(t0, space2);
SamplerState                    gSampWrap            : register(s1, space0);

#define RTS_BINDLESS_LIMIT 16384u

#define RTSHADOW_CASTER_MASK 0x02u

// Candidate classification (see RTS_ClassifyCandidate).
#define RTS_CAND_SOLID 0u   // commit - a real occluder, ends traversal
#define RTS_CAND_SKIP  1u   // decline - cutout hole, traversal continues
#define RTS_CAND_BLEND 2u   // never commit - attenuate, traversal continues

// Cost leash for the translucent path. Every blend layer costs a bindless
// VB/IB fetch + a texture SampleLevel, and rayTMax (150 wu) is the only other
// bound (see the TDR note in RTShadowPass.h). The layer cap keeps the worst case
// a constant instead of scene-dependent; it stops accumulating, it does not
// darken (zeroing would turn a stack of faint smoke cards black). The eps
// early-out is the "opaque enough" exit.
#define RTS_MAX_BLEND_LAYERS   8u
#define RTS_TRANSMITTANCE_EPS  0.02

// Index fetch - primitives use R16_UINT (indexBytes == 2), MeshLibrary R32.
// ByteAddressBuffer loads are 4-byte aligned; pick the 16-bit half manually.
// Clone of HR_LoadIndex (HybridReflIntersectHW.cs.hlsl); the RTR and DDGI
// traces carry the same fetch.
uint RTS_LoadIndex(uint ibIdx, uint i, uint indexBytes)
{
    if (indexBytes == 2u)
    {
        const uint byteOff = i * 2u;
        const uint word    = g_Buffers[ibIdx].Load(byteOff & ~3u);
        return (byteOff & 2u) ? (word >> 16) : (word & 0xFFFFu);
    }
    return g_Buffers[ibIdx].Load(i * 4u);
}

// Interpolated UV for a reported candidate. false = the geometry lookup is
// unusable (streaming, bad bindless slot); callers decide what that means.
bool RTS_FetchCandidateUV(RTAlphaRecord a, uint primIdx, float2 bary, out float2 uv)
{
    uv = float2(0.0, 0.0);
    if (a.vbBindlessIdx >= RTS_BINDLESS_LIMIT ||
        a.ibBindlessIdx >= RTS_BINDLESS_LIMIT) return false;

    const uint vbIdx = NonUniformResourceIndex(a.vbBindlessIdx);
    const uint ibIdx = NonUniformResourceIndex(a.ibBindlessIdx);
    const uint indexBytes = (a.indexBytes == 2u) ? 2u : 4u;
    const uint i0 = RTS_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 0u, indexBytes);
    const uint i1 = RTS_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 1u, indexBytes);
    const uint i2 = RTS_LoadIndex(ibIdx, a.indexStart + primIdx * 3u + 2u, indexBytes);

    float2 uv0, uv1, uv2;
    if (a.flags & RT_ALPHA_FLAG_DEINTERLEAVED)
    {
        // Procedural primitives + skinned. Skinned meshes have no UVs in the
        // SkinnedVertexRing (pos/nrm/prevPos only), but UVs are skin-invariant
        // and come from the static per-entity uv0 buffer.
        if (a.uvBufIdx >= RTS_BINDLESS_LIMIT) return false;
        const uint uvIdx = NonUniformResourceIndex(a.uvBufIdx);
        uv0 = asfloat(g_Buffers[uvIdx].Load2((a.uvStart + i0) * 8u));
        uv1 = asfloat(g_Buffers[uvIdx].Load2((a.uvStart + i1) * 8u));
        uv2 = asfloat(g_Buffers[uvIdx].Load2((a.uvStart + i2) * 8u));
    }
    else
    {
        // Interleaved MeshLibrary: pos@0 nrm@12 uv@24 in the first 32 bytes of
        // EVERY layout; only the stride varies. Indices are absolute.
        const uint stride = max(a.vertexStride, 32u);
        uv0 = asfloat(g_Buffers[vbIdx].Load2(i0 * stride + 24u));
        uv1 = asfloat(g_Buffers[vbIdx].Load2(i1 * stride + 24u));
        uv2 = asfloat(g_Buffers[vbIdx].Load2(i2 * stride + 24u));
    }

    const float w = 1.0 - bary.x - bary.y;
    uv = uv0 * w + uv1 * bary.x + uv2 * bary.y;
    return true;
}

// Classify a reported non-opaque candidate; a blend layer's opacity goes to outAlpha.
//
// CUTOUT (RT_ALPHA_FLAG_ALPHA_CLIP) mirrors Shadow.ps.hlsl: alpha =
// BASECOLORMAP.a alone (baseColor.a is not multiplied in for shadows), "no
// texture bound" = fully opaque, and a raw compare against alphaRef - the
// raster shadow clip has no fwidth rescale, so no softening bias here either
// (the reflection path deliberately differs; see HR_ALPHA_SOFTEN). Every
// cutout failure path returns SOLID.
//
// BLEND (RT_ALPHA_FLAG_BLEND) returns this layer's opacity in outAlpha and
// never returns SOLID: the record's alphaRef slot carries
// MaterialComponent::GetOpacity() (baseColor.w) and the sampled texel only
// modulates it. An untextured glass pane carries all of its opacity in
// baseColor.w, and a transient albedoTexIdx == -1 during texture streaming
// must not flash a solid black shadow, so a failed lookup falls back to
// opacity-only.
uint RTS_ClassifyCandidate(uint instId, uint primIdx, float2 bary, out float outAlpha)
{
    outAlpha = 1.0;

    // Kill-switch / table-not-live guard. Must precede any g_AlphaRecords
    // access: the root SRV is bound unconditionally and may be VA 0.
    if (alphaTestEnabled == 0u)       return RTS_CAND_SOLID;
    if (instId >= alphaInstanceCount) return RTS_CAND_SOLID;

    const RTAlphaRecord a = g_AlphaRecords[instId];
    const bool isBlend = (a.flags & RT_ALPHA_FLAG_BLEND) != 0u;
    if (!isBlend && (a.flags & RT_ALPHA_FLAG_ALPHA_CLIP) == 0u)
        return RTS_CAND_SOLID;

    // For a blend record this is the opacity, for a cutout the clip threshold;
    // the two flags are mutually exclusive by construction.
    const float refOrOpacity = a.alphaRef;

    // Additive / Multiply arrive with opacity 0 (they add or modulate light
    // rather than absorbing it). Bail before any memory traffic: no index fetch,
    // no UV fetch, no texture sample.
    if (isBlend && refOrOpacity <= 0.0)
    {
        outAlpha = 0.0;
        return RTS_CAND_BLEND;
    }

    const bool texOk = (a.albedoTexIdx >= 0) &&
                       (a.albedoTexIdx < (int)RTS_BINDLESS_LIMIT);
    float2 uv;
    const bool geoOk = texOk && RTS_FetchCandidateUV(a, primIdx, bary, uv);
    if (!geoOk)
    {
        if (isBlend) { outAlpha = saturate(refOrOpacity); return RTS_CAND_BLEND; }
        return RTS_CAND_SOLID;
    }

    // Material UV transform. The compact 40 B alpha record (fetched per traversal
    // candidate) does not carry it, so read it from the 128 B instance table: same
    // InstanceID() index space, and alphaTestEnabled already gates both buffers.
    // Without it a tiled cutout (fence, foliage card) casts a shadow with a
    // different silhouette than it renders with.
    uv = RTInstanceUV(g_Instances[instId], uv);

    // No ray differentials in an inline RayQuery: the mip must be explicit.
    // mip 0 = the authored silhouette; alphaMipLevel raises it to trade crispness
    // for texture-cache misses. Channel select: .r when the record carries a
    // Masked-mode opacity mask (RT_ALPHA_FLAG_OPACITY_R), else .a. A blend record
    // never carries OPACITY_R (DDGISceneAS gates it off), so it reads
    // BASECOLORMAP.a, which is what the forward transparent pass blends with.
    const float alpha = RTAlphaSampleAlpha(
        a, g_AllTextures[NonUniformResourceIndex((uint)a.albedoTexIdx)],
        gSampWrap, uv, alphaMipLevel);

    if (isBlend)
    {
        outAlpha = saturate(alpha * refOrOpacity);
        return RTS_CAND_BLEND;
    }
    // exact complement of Shadow.ps's clip()
    return (alpha >= refOrOpacity) ? RTS_CAND_SOLID : RTS_CAND_SKIP;
}

// What a blend candidate keeps per channel: a Transmissive record's colour (RT_ALPHA_FLAG_TINT) scaled
// by the texel's share of its coverage, any other one's 1 - alpha.
float3 RTS_CandidateKept(uint instId, float candAlpha)
{
    const RTAlphaRecord a = g_AlphaRecords[instId];
    if ((a.flags & RT_ALPHA_FLAG_TINT) == 0u || !(a.alphaRef > 0.0)) return (1.0 - candAlpha).xxx;
    const float texel = saturate(candAlpha / a.alphaRef);
    return 1.0 - texel * (1.0 - RTAlphaTint(a));
}

#include "core/hash.hlsli"

#include "core/sampling.hlsli"   // OrthonormalBasis, SampleGGXVNDF, Hammersley2D

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= screenW || pixel.y >= screenH)
        return;

    // NRD guide channels, written for every pixel: SIGMA edge-stopping reads the
    // full-screen guides, sky included.
    const float depth = gDepth.Load(int3(pixel, 0));
    const float3 N    = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    const float  rough= gSurface.Load(int3(pixel, 0)).r;
    OutNormalRough[pixel] = NRD_FrontEnd_PackNormalAndRoughness(N, rough, 0.0);
    OutViewZ[pixel] = (depth > 0.0) ? RTS_LinearizeReverseZ(depth) : 1e7;
    const float2 ndcDelta = gVelocity.Load(int3(pixel, 0));
    OutMV[pixel] = float2(-0.5 * ndcDelta.x, 0.5 * ndcDelta.y);

    // Toward-sun direction: LightCB stores lightDir FROM the light.
    const float3 L = normalize(-sunDirection);

    // distanceToOccluder feeds SIGMA_FrontEnd_PackPenumbra:
    //   >= NRD_FP16_MAX -> fully lit (sky / ray miss),
    //   0               -> fully shadowed (NRD contract: NoL <= 0 must be 0, or
    //                      SIGMA's spatial filter leaks "lit" across shadowed
    //                      creases),
    //   a real hit      -> the occluder distance.
    // Reverse-Z sky = exactly 0.
    //
    // transmittance is the IN_TRANSLUCENCY payload: 1 = nothing in the way,
    // 0 = fully blocked, in between = translucent layers only. Sky and untraced
    // pixels keep 1.0 so SIGMA_FrontEnd_PackTranslucency emits (1,1,1,1).
    float distanceToOccluder = NRD_FP16_MAX;
    float transmittance      = 1.0;
    float3 colour            = 1.0;   // the transmittance per channel
    if (depth > 0.0)
    {
        if (dot(N, L) <= 0.0)
        {
            // Back-facing the sun: contract-shadowed (Lighting's NdotL zeroes the direct
            // sun here anyway, but SIGMA must see it as shadowed).
            distanceToOccluder = 0.0;
            transmittance      = 0.0;
            colour             = 0.0;
        }
        else
        {
            const float2 uv = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);
            const float3 worldPos = RTS_ReconstructWorldPos(uv, depth);

            // Jitter the ray inside the sun's angular cone (uniform disk x tan halfAngle
            // on an ONB around L). 1 spp/frame; SIGMA's stabilization integrates the
            // penumbra across frames.
            float3 dir = L;
            if (sunTanHalfAngle > 0.0)
            {
                const float2 xi = R2PixelFrame2D(pixel, frameIndex);
                const float  r    = sqrt(xi.x) * sunTanHalfAngle;
                const float  phi  = xi.y * 6.2831853;
                float3 T, B;
                OrthonormalBasis(L, T, B);
                dir = normalize(L + (r * cos(phi)) * T + (r * sin(phi)) * B);
            }

            RayDesc r;
            r.Origin    = worldPos + N * normalBias;
            r.Direction = dir;
            r.TMin      = 0.02;
            r.TMax      = rayTMax;

            // Do not add RAY_FLAG_FORCE_OPAQUE: it outranks the per-instance flag, makes
            // the alpha cutout unreachable, and every foliage card would cast a solid
            // silhouette that the min() in shadow.hlsli forces over the correct raster CSM
            // cutout. Only the alpha-clip and alpha-blend instances carry
            // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE and enter the candidate
            // branch; every other BLAS is FLAG_OPAQUE and resolves in fixed function.
            // ACCEPT_FIRST_HIT_AND_END_SEARCH ends the search on a commit, and a commit
            // means a solid occluder: the shadow semantic and the cheap early exit the TDR
            // budget wants. Translucent candidates are never committed, so traversal keeps
            // reporting them and the transmittance product accumulates across layers.
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
                   | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
            q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, RTSHADOW_CASTER_MASK, r);

            // Nearest translucent layer: the penumbra radius SIGMA gets when nothing
            // solid committed.
            float nearestBlend = NRD_FP16_MAX;
            uint  blendLayers  = 0u;

            [loop]
            while (q.Proceed())
            {
                if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
                    continue;

                float candAlpha;
                const uint cls = RTS_ClassifyCandidate(
                    q.CandidateInstanceID(),
                    q.CandidatePrimitiveIndex(),
                    q.CandidateTriangleBarycentrics(),
                    candAlpha);

                if (cls == RTS_CAND_SOLID)
                {
                    q.CommitNonOpaqueTriangleHit();   // ends traversal
                }
                else if (cls == RTS_CAND_BLEND)
                {
                    // Beer-Lambert-ish layer product. prod(1 - a_i) is commutative, so the
                    // arbitrary order DXR reports candidates in does not change the result,
                    // which is what makes accumulating during traversal legal.
                    transmittance *= (1.0 - candAlpha);
                    colour        *= RTS_CandidateKept(q.CandidateInstanceID(), candAlpha);
                    nearestBlend   = min(nearestBlend, q.CandidateTriangleRayT());
                    ++blendLayers;

                    if (transmittance <= RTS_TRANSMITTANCE_EPS)
                    {
                        transmittance = 0.0;   // opaque enough - stop tracing
                        colour        = 0.0;
                        break;
                    }
                    if (blendLayers >= RTS_MAX_BLEND_LAYERS)
                        break;                 // cost leash - keep what we have
                }
                // RTS_CAND_SKIP: cutout hole - decline, traversal continues.
            }

            if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                distanceToOccluder = q.CommittedRayT();   // -> penumbra radius
                transmittance      = 0.0;                 // solid wins outright
                colour             = 0.0;
            }
            else if (transmittance < 1.0)
            {
                // Translucent-only occlusion. Reporting the real distance is correct because
                // SIGMA_SHADOW_TRANSLUCENCY takes its lit bit from IN_TRANSLUCENCY.x, not from
                // the penumbra sentinel, so the glass gets a physically-sized penumbra and a
                // partial shadow.
                distanceToOccluder = nearestBlend;
            }
            // full miss -> distanceToOccluder stays NRD_FP16_MAX, transmittance 1
        }
    }

    // SIGMA penumbra encode: distance-to-occluder x tan(angular radius).
    // distanceToOccluder >= NRD_FP16_MAX -> fully lit (no penumbra).
    OutPenumbra[pixel] = SIGMA_FrontEnd_PackPenumbra(distanceToOccluder, sunTanHalfAngle);
    // IN_TRANSLUCENCY: .x = lit bit (distanceToOccluder >= NRD_FP16_MAX), .yzw = the
    // transmittance per channel: grey but for Transmissive glass (RT_ALPHA_FLAG_TINT).
    OutTranslucency[pixel] =
        SIGMA_FrontEnd_PackTranslucency(distanceToOccluder, saturate(colour));
}
