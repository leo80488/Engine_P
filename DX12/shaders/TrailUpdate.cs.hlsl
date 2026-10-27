// Trail update: ONE THREAD PER TRAIL. Each thread owns one trail slot exclusively:
//   A) ages every live point record of its trail by dt, then
//   B) replays this frame's append requests for its slot in buffer order (the CPU writes a
//      trail's substeps contiguously, oldest first).
// Do not parallelize per request: a trail can get several requests per frame (CPU slerp
// sub-sampling emits up to kMaxCpuSubSteps), and head/count would become a cross-thread
// read-modify-write. Correctness needs (a) the CPU writing substeps contiguous oldest-first,
// (b) this shader scanning in buffer order, (c) a dispatch of exactly ceil(maxTrails/64) groups.
// Bindings (space2):
//   b0 -> TrailSystemParams CB
//   t0 -> StructuredBuffer<TrailAppendRequest> gRequests  (UPLOAD SRV)
//   u0 -> RWStructuredBuffer<TrailSegment> gSegments      (point-major records)
//   u1 -> RWStructuredBuffer<TrailHeader>  gHeaders

#include "Trail.hlsli"

cbuffer SystemCB : register(b0, space2)
{
    TrailSystemParams gParams;
};

StructuredBuffer<TrailAppendRequest>   gRequests : register(t0, space2);
RWStructuredBuffer<TrailSegment>       gSegments : register(u0, space2);
RWStructuredBuffer<TrailHeader>        gHeaders  : register(u1, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint tid = id.x;
    if (tid >= gParams.maxTrails) return;

    TrailHeader hdr = gHeaders[tid];

    // A: age my existing samples. Runs BEFORE the request replay so samples appended this frame are
    // not double-aged. Bounded: 64 samples x 4 rows = 256 iterations worst case.
    {
        const uint pc    = clamp(hdr.pointCount, 1u, TRAIL_MAX_POINTS);
        const uint count = min(hdr.count, gParams.maxSegments); // corrupt-header insurance
        const uint oldest = (hdr.head + gParams.maxSegments - count) % gParams.maxSegments;
        for (uint s = 0; s < count; ++s)
        {
            const uint physical = (oldest + s) % gParams.maxSegments;
            const uint sampleBase = (tid * gParams.maxSegments + physical) * TRAIL_MAX_POINTS;
            for (uint p = 0; p < pc; ++p)
                gSegments[sampleBase + p].age += gParams.deltaTime;
        }
    }

    // Trim expired history before UVs stretch across the live ribbon, keeping one boundary sample so
    // the last quad fades smoothly to zero. Untrimmed, slow or temporally budgeted trails would
    // normalize against dead records and map only part of the mask onto the visible sheet.
    while (hdr.count > 1u)
    {
        const uint nextOldest = (hdr.head + gParams.maxSegments - hdr.count + 1u) % gParams.maxSegments;
        if (gSegments[(tid * gParams.maxSegments + nextOldest) * TRAIL_MAX_POINTS].age < hdr.maxAge)
            break;
        --hdr.count;
    }
    if (hdr.count == 1u)
    {
        const uint last = (hdr.head + gParams.maxSegments - 1u) % gParams.maxSegments;
        if (gSegments[(tid * gParams.maxSegments + last) * TRAIL_MAX_POINTS].age >= hdr.maxAge)
            hdr.count = 0u;
    }

    // B: replay this frame's requests for my slot, in buffer order.
    for (uint r = 0; r < gParams.requestCount; ++r)
    {
        TrailAppendRequest req = gRequests[r];
        if (req.trailSlot != tid) continue;

        // Slot reuse: zero the ring so the recycled slot starts clean.
        if (req.flags & 1u) { hdr.head = 0; hdr.count = 0; }
        // Freed slot with no new owner: clear it so nothing renders; no append.
        if (req.flags & 2u) { hdr.head = 0; hdr.count = 0; continue; }

        const uint pc = clamp(req.pointCount, 1u, TRAIL_MAX_POINTS);

        // Self-heal BEFORE the param refresh: a ring built with another pointCount interleaves rows
        // meaninglessly, so restart the ribbon (a backstop under the CPU lastPointCount latch).
        if (hdr.count > 0 && clamp(hdr.pointCount, 1u, TRAIL_MAX_POINTS) != pc)
        {
            hdr.head = 0; hdr.count = 0;
        }

        // Refresh the per-frame render params, also for RefreshParams-only requests (that flag exists so
        // idle-edited trails reach the GPU).
        hdr.width              = req.width;
        hdr.maxAge             = req.maxAge;
        hdr.startColor         = req.startColor;
        hdr.endColor           = req.endColor;
        hdr.pointCount         = pc;
        hdr.textureBindlessIdx = req.textureBindlessIdx;
        hdr.renderFlags        = req.renderFlags;
        hdr.subdivisions       = req.subdivisions;
        hdr.uvTileLength       = req.position1.w;
        hdr.hdrIntensity       = req.position2.w;

        if (req.flags & 4u) continue;   // RefreshParams: no append

        // Append one record per point row at the ring head. arcLen accumulates per ROW from the previous
        // sample (add before subtract: no uint underflow at head == 0).
        for (uint p = 0; p < pc; ++p)
        {
            const float3 pos = (p == 0) ? req.position :
                               (p == 1) ? req.position1.xyz :
                               (p == 2) ? req.position2.xyz : req.position3.xyz;

            float prevArc = 0.0;
            if (hdr.count > 0)
            {
                const uint prevSample = (hdr.head + gParams.maxSegments - 1) % gParams.maxSegments;
                const uint prevRec = (tid * gParams.maxSegments + prevSample) * TRAIL_MAX_POINTS + p;
                prevArc = gSegments[prevRec].arcLen + length(pos - gSegments[prevRec].position);
            }

            const uint rec = (tid * gParams.maxSegments + hdr.head) * TRAIL_MAX_POINTS + p;
            TrailSegment seg;
            seg.position = pos;
            seg.age      = req.position3.w;   // ageOffset: 0 for the newest substep
            seg.arcLen   = prevArc;
            seg._pad0 = 0; seg._pad1 = 0; seg._pad2 = 0;
            gSegments[rec] = seg;
        }

        hdr.head  = (hdr.head + 1) % gParams.maxSegments;
        hdr.count = min(hdr.count + 1, gParams.maxSegments);
    }

    gHeaders[tid] = hdr;   // single final write
}
