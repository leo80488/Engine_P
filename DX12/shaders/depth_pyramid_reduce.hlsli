// depth_pyramid_reduce.hlsli - the shared "two mips per dispatch" reduction core of the
// engine's depth pyramids (Hi-Z occlusion culling + SSR Hi-Z traversal).
//
// Mip N reads mip N-1, a true read-after-write dependency, so one dispatch plus one global
// UAV barrier per mip (~12 of each at 1080p, the 4x4/2x2/1x1 tail pure launch overhead) is the
// floor unless a dispatch produces several mips. Here an 8x8 group reduces its 16x16 source
// window into mip N (8x8, DPR_DST_A), stages the result in groupshared, and reduces once more
// into mip N+1 (4x4, DPR_DST_B): half the dispatches and half the barriers.
//
// FidelityFX SPD (external/FidelityFX/.../gpu/spd/ffx_spd.h) would collapse the whole chain into
// one dispatch, but it needs all 12-13 destination mips bound as one UAV array plus a
// globallycoherent atomic counter, i.e. new root parameters. The shared compute root sig exposes
// u0..u4 space2 as five single-descriptor tables and is frozen, hence this hand-written
// two-level variant.
//
// The includer selects the payload and the operator before including:
//   DPR_TYPE            value type (float for the min-only Hi-Z, float2 for SSR)
//   DPR_SRC             RWTexture2D<DPR_TYPE> holding mip N-1
//   DPR_DST_A           RWTexture2D<DPR_TYPE> receiving mip N
//   DPR_DST_B           RWTexture2D<DPR_TYPE> receiving mip N+1
//   DPRCombine(a, b)    function combining two payloads (min / max+min)
// and must declare these CB fields (same names, any register layout):
//   srcWidth/srcHeight  mip N-1 dimensions
//   dstWidth/dstHeight  mip N dimensions (also the dispatch range)
//   writeSecond         0 on the odd tail of the chain (mip N was the last)

// 8 outputs + 1 halo column/row - see the level-2 fold reach below.
#define DPR_LDS_DIM 9
groupshared DPR_TYPE dpr_lds[DPR_LDS_DIM][DPR_LDS_DIM];

// Reduce the 2x2 (plus odd-dimension remainder) source block that destination
// texel `d` stands for. `d` must already be clamped to the mip N extent.
DPR_TYPE DPR_ReduceSource(uint2 d)
{
    const uint2 mx = uint2(srcWidth - 1u, srcHeight - 1u);
    const uint2 s  = d * 2u;
    const uint2 s0 = min(s,                 mx);
    const uint2 s1 = min(s + uint2(1u, 0u), mx);
    const uint2 s2 = min(s + uint2(0u, 1u), mx);
    const uint2 s3 = min(s + uint2(1u, 1u), mx);

    DPR_TYPE v = DPRCombine(DPRCombine(DPR_SRC[s0], DPR_SRC[s1]),
                            DPRCombine(DPR_SRC[s2], DPR_SRC[s3]));

    // Odd-dimension remainder fold. D3D12 defines mip m as floor(dim / 2^m), so with an odd source
    // the last row/column belongs to no destination texel and would vanish from this and every
    // coarser mip. For a conservative min-pyramid that is the dangerous direction: a lost far
    // occluder raises the tile min, and the cull then judges visible geometry occluded. Fold the
    // orphan band into the last destination texel instead: over-covering a tile is always safe,
    // under-covering never is.
    const bool lastX = (d.x == dstWidth  - 1u) && ((srcWidth  & 1u) != 0u);
    const bool lastY = (d.y == dstHeight - 1u) && ((srcHeight & 1u) != 0u);
    if (lastX)
    {
        v = DPRCombine(v, DPR_SRC[uint2(mx.x, s0.y)]);
        v = DPRCombine(v, DPR_SRC[uint2(mx.x, s2.y)]);
    }
    if (lastY)
    {
        v = DPRCombine(v, DPR_SRC[uint2(s0.x, mx.y)]);
        v = DPRCombine(v, DPR_SRC[uint2(s1.x, mx.y)]);
    }
    if (lastX && lastY)
        v = DPRCombine(v, DPR_SRC[mx]);

    return v;
}

// [numthreads(8, 8, 1)] - the includer owns the entry point so each shader keeps the entry name
// its ShaderLibrary registration expects.
void DPR_Reduce2Mips(uint3 dtid, uint3 gtid, uint3 gid)
{
    const uint2 dstMax = uint2(dstWidth - 1u, dstHeight - 1u);

    // ---- Level 1: mip N-1 -> mip N ----
    // Out-of-range threads reduce the clamped texel instead of bailing, so every LDS slot holds a
    // real mip N value and the level-2 reads below need no bounds logic. Only the store is gated.
    const DPR_TYPE v = DPR_ReduceSource(min(dtid.xy, dstMax));
    dpr_lds[gtid.y][gtid.x] = v;

    if (dtid.x < dstWidth && dtid.y < dstHeight)
        DPR_DST_A[dtid.xy] = v;

    // Halo column/row. The level-2 odd fold reaches mip N texel (2*local + 2), one past the group's
    // 8-wide tile when this group owns the last mip N+1 texel. Recompute that texel from the source:
    // the neighbouring group writes it into DPR_DST_A in this same dispatch, so reading it back
    // would race. Gated on the fold being able to fire at all (mip N dimension odd), so the common
    // even case pays nothing.
    if ((dstWidth & 1u) != 0u && gtid.x == 7u)
        dpr_lds[gtid.y][8] = DPR_ReduceSource(min(uint2(dtid.x + 1u, dtid.y), dstMax));
    if ((dstHeight & 1u) != 0u && gtid.y == 7u)
        dpr_lds[8][gtid.x] = DPR_ReduceSource(min(uint2(dtid.x, dtid.y + 1u), dstMax));
    if ((dstWidth & 1u) != 0u && (dstHeight & 1u) != 0u && gtid.x == 7u && gtid.y == 7u)
        dpr_lds[8][8] = DPR_ReduceSource(min(dtid.xy + uint2(1u, 1u), dstMax));

    GroupMemoryBarrierWithGroupSync();

    // ---- Level 2: mip N -> mip N+1 ----
    // writeSecond == 0 on the odd tail of the chain. It is dispatch-uniform, but it is tested after
    // the barrier so the sync stays in unconditional flow whatever the compiler can prove.
    if (writeSecond == 0u) return;
    if (gtid.x >= 4u || gtid.y >= 4u) return;

    const uint2 dst2 = uint2(max(dstWidth  >> 1u, 1u), max(dstHeight >> 1u, 1u));
    const uint2 d2   = uint2(gid.x * 4u + gtid.x, gid.y * 4u + gtid.y);
    if (d2.x >= dst2.x || d2.y >= dst2.y) return;

    const uint2 l = gtid.xy * 2u;
    DPR_TYPE w = DPRCombine(DPRCombine(dpr_lds[l.y     ][l.x], dpr_lds[l.y     ][l.x + 1u]),
                            DPRCombine(dpr_lds[l.y + 1u][l.x], dpr_lds[l.y + 1u][l.x + 1u]));

    // Same orphan-band rule as DPR_ReduceSource, one level up: LDS index
    // (l + 2) is exactly mip N texel (dstWidth - 1) when this thread owns the
    // last mip N+1 texel of an odd mip N.
    const bool lastX = (d2.x == dst2.x - 1u) && ((dstWidth  & 1u) != 0u);
    const bool lastY = (d2.y == dst2.y - 1u) && ((dstHeight & 1u) != 0u);
    if (lastX)
    {
        w = DPRCombine(w, dpr_lds[l.y     ][l.x + 2u]);
        w = DPRCombine(w, dpr_lds[l.y + 1u][l.x + 2u]);
    }
    if (lastY)
    {
        w = DPRCombine(w, dpr_lds[l.y + 2u][l.x     ]);
        w = DPRCombine(w, dpr_lds[l.y + 2u][l.x + 1u]);
    }
    if (lastX && lastY)
        w = DPRCombine(w, dpr_lds[l.y + 2u][l.x + 2u]);

    DPR_DST_B[d2] = w;
}
