// Hit-triangle and alpha-cutout fetch for the RT passes (RTDI, RtxdiBridge, rt_hit_surface).
// HybridReflIntersectHW keeps its own copies of these helpers so RTR's bytecode stays put;
// keep its mirrored HR_AlphaThreshold in step with this one.
#ifndef RT_HIT_FETCH_HLSLI
#define RT_HIT_FETCH_HLSLI
#define HR_VERTEX_STRIDE_BYTES 32   // minimum/fallback stride
#define HR_INDEX_BYTES         4

// DXR instance inclusion mask for reflection rays. DDGISceneAS partitions instance masks:
// bit0 = visible to reflections / RTAO (main-pass-visible statics and skinned characters),
// bit1 = RT-shadow caster, bit2 = DDGI. 0x01 skips hidden instances entirely - the ray
// passes through to whatever is behind, so VisibilityComponent-hidden objects never
// appear in reflections. DDGI traces 0x04 (statics only), RTShadow 0x02.
#define HR_REFLECT_MASK        0x01u

// Index fetch: primitives use R16_UINT (indexBytes == 2), MeshLibrary R32.
// ByteAddressBuffer loads are 4-byte aligned, so the 16-bit half is picked by hand.
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
        // De-interleaved streams (procedural primitives, skinned meshes): vb = positions
        // (float3, stride 12) plus separate normal (float3) and uv0 (float2) streams. Skinned
        // streams live in the shared SkinnedVertexRing arena, rebased by the per-instance
        // element bases (vertexStart = position base; all bases are 0 for primitives).
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
// Runs inside the Proceed() loop for instances DDGISceneAS flags FORCE_NON_OPAQUE; every
// other instance keeps its opaque BLAS and never gets here. Bindless indices are
// range-checked against the heap sizes (kMaxBuffers / kMaxBindlessTextures = 16384): an
// out-of-range descriptor read is undefined and can hang the device. Every failure path
// returns accept (fully solid).
#define HR_BINDLESS_LIMIT 16384u

// RT_ROUGHNESS_FLOOR: the perceptual-roughness floor of a ray-traced surface
// (0.045, the raster lighting's; 0.01 under PT_REFERENCE_GGX).
#include "core/roughness_floor.hlsli"

// Threshold softening. The deferred raster path does not compare raw: custom_surface
// runs the Ben Golus rescale clip((a - ref) / max(fwidth(a), 1e-4) + 0.5), an effective
// threshold of ref - 0.5*fwidth(a), more permissive and TAA-dithered on top. A ray has no
// derivatives, so a raw compare gives thinner cutouts in reflections than on screen; a
// small constant bias is the closest approximation. Tune, do not remove.
#define HR_ALPHA_SOFTEN 0.02

// The softened threshold, never below half the authored one: with a small Alpha Ref
// (foliage at 0.005-0.024) the fixed bias alone goes <= 0 and `alpha >= 0` accepts every
// texel, turning the leaf card into a solid quad for every GI, DI, reflection and
// path-tracer ray. A fully transparent texel (alpha 0) must always fall through, as on
// screen, where fwidth(a) is 0 inside the transparent region.
float HR_AlphaThreshold(float alphaRef)
{
    return max(alphaRef - HR_ALPHA_SOFTEN, 0.5 * alphaRef);
}

// Fetches only the three UVs of a candidate triangle; a candidate test needs no
// positions or normals (HR_LoadHitTriangleAttributes).
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
        // Procedural primitives and skinned: UVs are a separate float2 stream rebased by
        // uvStart. The SkinnedVertexRing holds no UVs (pos / nrm / prevPos only); UVs are
        // skin-invariant and come from the static per-entity uv0 buffer DDGISceneAS forwards.
        if (a.uvBufIdx >= HR_BINDLESS_LIMIT) return false;  // no UV stream -> solid
        const uint uvbIdx = NonUniformResourceIndex(a.uvBufIdx);
        uv0 = asfloat(g_Buffers[uvbIdx].Load2((a.uvStart + i0) * 8u));
        uv1 = asfloat(g_Buffers[uvbIdx].Load2((a.uvStart + i1) * 8u));
        uv2 = asfloat(g_Buffers[uvbIdx].Load2((a.uvStart + i2) * 8u));
    }
    else
    {
        // Interleaved MeshLibrary: pos@0 nrm@12 uv@24 in the first 32 bytes of every layout;
        // only the stride varies. Indices are absolute into the VB view - vertexStart is not
        // added, as in the interleaved branch of HR_LoadHitTriangleAttributes.
        const uint stride = max(a.vertexStride, HR_VERTEX_STRIDE_BYTES);
        uv0 = asfloat(g_Buffers[vbIdx].Load2(i0 * stride + 24u));
        uv1 = asfloat(g_Buffers[vbIdx].Load2(i1 * stride + 24u));
        uv2 = asfloat(g_Buffers[vbIdx].Load2(i2 * stride + 24u));
    }
    const float w = 1.0 - bary.x - bary.y;
    uv = uv0 * w + uv1 * bary.x + uv2 * bary.y;
    return true;
}

// Flags word for an instance, or 0 when the record table is not live this frame. It
// must be the only way g_AlphaRecords is reached: the root SRV is bound unconditionally
// and may be VA 0.
uint HR_AlphaFlags(uint instId)
{
    if (alphaTestEnabled == 0u)       return 0u;
    if (instId >= alphaInstanceCount) return 0u;
    return g_AlphaRecords[instId].flags;
}

// Candidate alpha test at an explicit texture mip; returns true = accept (commit the
// candidate). includeBaseColorAlpha picks the raster path the ray mirrors:
//   true  - GBuffer parity (opacity = baseTex.a * baseColor.a), the reflection ray;
//   false - Shadow.ps parity (texture alpha alone), the sun-shadow ray from a
//           reflection hit.
bool HR_AlphaAcceptMip(uint instId, uint primIdx, float2 bary, bool includeBaseColorAlpha, float mip)
{
    const uint hrFlags = HR_AlphaFlags(instId);

    // Alpha-blend (translucent) instances are FORCE_NON_OPAQUE so RTShadow's candidate loop
    // can attenuate through them, so they reach this loop too. includeBaseColorAlpha tells
    // the two call sites apart:
    //   reflection ray (true)    - commit: a window is visible in a mirror; HR_ShadeHit
    //                              shades it through the material proxy.
    //   ShadowVisibility (false) - pass through: glass casts no hard shadow onto a
    //                              reflected surface, as with main-view RTShadow.
    // Binary rather than a transmittance product on purpose: on a secondary ray inside a
    // reflection the extra fidelity is invisible and the traversal cost is not.
    if ((hrFlags & RT_ALPHA_FLAG_BLEND) != 0u) return includeBaseColorAlpha;

    if ((hrFlags & RT_ALPHA_FLAG_ALPHA_CLIP) == 0u) return true;

    const RTAlphaRecord a = g_AlphaRecords[instId];
    if ((a.flags & RT_ALPHA_FLAG_ALPHA_CLIP) == 0u) return true;
    // No base-color texture bound -> nothing to clip against, as in Shadow.ps. Also covers
    // the transient albedoTexIdx == -1 window during texture streaming or right after
    // OnWorldClear.
    if (a.albedoTexIdx < 0 || a.albedoTexIdx >= (int)HR_BINDLESS_LIMIT) return true;

    float2 uv;
    if (!HR_LoadCandidateUV(a, primIdx, bary, uv)) return true;

    // Material UV transform. The 40 B alpha record does not carry it (it is fetched per
    // traversal candidate), so read it from the 128 B instance table, same InstanceID()
    // index. Only clip candidates with a texture get here, right before a Sample, so the
    // extra 16-byte load is noise. Without it a tiled cutout (fence, foliage card) clips on
    // a different silhouette in reflections than on screen.
    uv = HR_MaterialUV(g_Instances[instId], uv);

    // Explicit LOD: HR_AlphaAccept uses mip 0; the path tracer passes a candidate cone's.
    // .r when the record carries a Masked-mode opacity mask, else .a.
    float alpha = RTAlphaSampleAlpha(
        a, g_AllTextures[NonUniformResourceIndex((uint)a.albedoTexIdx)],
        gSampWrap, uv, mip);
    if (includeBaseColorAlpha) alpha *= g_Instances[instId].baseColor.a;

    return alpha >= HR_AlphaThreshold(a.alphaRef);
}

bool HR_AlphaAccept(uint instId, uint primIdx, float2 bary, bool includeBaseColorAlpha)
{
    return HR_AlphaAcceptMip(instId, primIdx, bary, includeBaseColorAlpha, 0);
}


#endif
