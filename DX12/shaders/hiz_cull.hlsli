// hiz_cull.hlsli - the Hi-Z occlusion test, shared by InstanceCull.cs.hlsl (one
// world AABB per instance) and SceneMeshlet.as.hlsl (one per cluster). One copy of
// the math and the thresholds, so the instance and the cluster level can never
// disagree about "occluded".
//
// Matrix convention: hizViewProj is the captured no-jitter view-projection of the
// frame that rendered the pyramid, as uploaded - transposed on the CPU
// (CullingPass::SetViewProj, Renderer::WriteMeshletCullCB) - and applied as a row
// vector, mul(float4(p, 1), hizViewProj). An untransposed matrix still passes
// identity-rotation geometry but culls rotated instances while they are on
// screen; the rotated-90-degree canary in meshlet_hiz_identity guards it.
//
// Reversed-Z: NDC z=1 near, z=0 far; closer geometry has larger z. The pyramid
// stores the min depth per tile = the farthest occluder, which is what a
// conservative test needs. Visible when the AABB's nearest corner is at least as
// close as the tile's farthest known occluder.
//
// hiZWidth / hiZHeight are the mip-0 valid extent (the render dims inside the
// pow2 texture), hiZMipCount the pyramid's mip count.
#ifndef HIZ_CULL_HLSLI
#define HIZ_CULL_HLSLI

bool HiZOcclusionTest(float3 worldMin, float3 worldMax, float4x4 hizViewProj,
                      Texture2D<float> hiZ, uint hiZWidth, uint hiZHeight, uint hiZMipCount)
{
    float2 ssMin = float2( 1e10,  1e10);
    float2 ssMax = float2(-1e10, -1e10);
    float  aabbNearZ = 0.0;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float3 corner = float3(
            (i & 1) ? worldMax.x : worldMin.x,
            (i & 2) ? worldMax.y : worldMin.y,
            (i & 4) ? worldMax.z : worldMin.z);

        float4 clip = mul(float4(corner, 1.0), hizViewProj);
        if (clip.w <= 0.0) return true; // corner behind camera -> visible

        float3 ndc = clip.xyz / clip.w;
        float2 uv  = ndc.xy * float2(0.5, -0.5) + 0.5;
        ssMin = min(ssMin, uv);
        ssMax = max(ssMax, uv);
        aabbNearZ = max(aabbNearZ, ndc.z);
    }

    // The pyramid holds the previous frame's depth and this projection uses that
    // frame's viewProj. A rect reaching outside [0,1] covers screen area the pyramid
    // has no data for - clamping it inward would judge the object by unrelated border
    // texels. Conservative answer: visible.
    if (ssMin.x < 0.0 || ssMin.y < 0.0 || ssMax.x > 1.0 || ssMax.y > 1.0)
        return true;

    // Pick the mip where the projected rect spans <= ~2 texels so the 2x2 corner
    // loads below cover it conservatively. +2 px: the tap window is dilated by 1 pixel
    // each side (the pyramid was rendered with TAA jitter, this projection has none,
    // and sub-pixel misalignment must not push a tiny object's own depth out of its
    // window), and the mip must come from the dilated extent to keep the 2x2 coverage
    // guarantee.
    float2 extentPx = (ssMax - ssMin) * float2(hiZWidth, hiZHeight) + 2.0;
    float  mipF     = ceil(log2(max(max(extentPx.x, extentPx.y), 1.0)));
    uint   mip      = (uint)clamp(mipF, 0.0, (float)(hiZMipCount - 1));

    // Map uv -> texel via mip-0 valid-extent pixel coords, then shift down to the
    // chosen mip. Do not use a per-mip `dim >> mip` instead: the valid extent is not
    // pow2, so it truncates (1351>>9 = 2 while the data spans ceil(1351/512) = 3 rows)
    // and skews every tap toward texel 0 (position/pitch-dependent false culls).
    int2 px0 = int2(min(ssMin * float2(hiZWidth, hiZHeight),
                        float2(hiZWidth - 1, hiZHeight - 1)));
    int2 px1 = int2(min(ssMax * float2(hiZWidth, hiZHeight),
                        float2(hiZWidth - 1, hiZHeight - 1)));
    // 1-pixel dilation (see extentPx above) - clamped to the valid region.
    px0 = max(px0 - 1, int2(0, 0));
    px1 = min(px1 + 1, int2(hiZWidth - 1, hiZHeight - 1));
    int2 t0 = px0 >> mip;
    int2 t1 = px1 >> mip;

    float d00 = hiZ.Load(int3(t0.x, t0.y, mip));
    float d10 = hiZ.Load(int3(t1.x, t0.y, mip));
    float d01 = hiZ.Load(int3(t0.x, t1.y, mip));
    float d11 = hiZ.Load(int3(t1.x, t1.y, mip));
    float farthestOccluder = min(min(d00, d10), min(d01, d11));

    return aabbNearZ >= farthestOccluder;
}

#endif // HIZ_CULL_HLSLI
