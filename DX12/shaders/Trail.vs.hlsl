// Trail vertex shader: a ribbon from consecutive sample pairs.
// Instance layout (per-frame adaptive budget): maxPoints / maxSubdiv are the FRAME'S maxima, not the
// compile-time caps, and the CPU derives the DrawInstanced count from the same values, so decode and
// draw never desync:
//   S  = max(maxSubdiv, 1)          // Catmull-Rom sub-segments per pair
//   P1 = max(maxPoints, 2) - 1      // point strips across the blade
//   perTrail   = (maxSegments-1) * P1 * S
//   trailIdx   = iid / perTrail
//   segPairIdx = (iid % perTrail) / (P1*S)   // sample pair (i, i+1), oldest-first
//   strip      = ((iid % perTrail) % (P1*S)) / S
//   sub        = iid % S
//   VertexID (0..5) = one of the 6 verts of the 2-triangle quad.
// iid = SV_InstanceID + g_slotFilter * perTrail on slot-filtered (custom-shader) draws, which issue
// one trail's worth of instances; iid = SV_InstanceID on the unfiltered default-bucket draw.
// Single-point trails (pointCount <= 1) use only strip 0 / sub 0 with the camera-facing miter-join
// expansion; their other instances emit degenerate quads.
// The segment pool is point-major:
//   record = ((trailIdx * maxSegments) + samplePhys) * TRAIL_MAX_POINTS + p
// Bindings (graphics, space0):
//   b2 -> TrailRenderCB
//   t4 -> StructuredBuffer<TrailSegment> gSegments
//   t5 -> StructuredBuffer<TrailHeader>  gHeaders

#include "Trail.hlsli"

cbuffer TrailRenderCB : register(b2, space0)
{
    float4x4 viewProj;
    float3   camForward;    // faces the single-point ribbon toward the camera
    uint     maxSegments;
    uint     maxPoints;     // frame max pointCount (>= 1), for the instance decode
    uint     maxSubdiv;     // frame max subdivisions (>= 1), for the instance decode
    uint     _pad0;
    uint     _pad1;
};

// Root constants (b0 space0, 4 DWORDs, visibility ALL: the engine-shared slot, set per draw like
// the particle bucket base). The variant / bucket filter keeps a trail from drawing in two buckets.
cbuffer TrailDrawCB : register(b0, space0)
{
    uint g_variantFilter;   // 0 = normal, 1 = TAA-excluded (stencil 0x40)
    uint g_shaderBucket;    // custom-shader bucket id (renderFlags bits 8-15)
    uint g_timeBits;        // asfloat = engine time (consumed by FX pixel shaders)
    uint g_slotFilter;      // custom buckets draw one trail per draw (per-trail
                            // b8 params); 0xFFFFFFFF = no slot filter
};

StructuredBuffer<TrailSegment> gSegments : register(t4, space0);
StructuredBuffer<TrailHeader>  gHeaders  : register(t5, space0);

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float  alpha : TEXCOORD0;  // additional per-vertex fade
    float2 uv    : TEXCOORD1;  // U along the trail (head=0), V across (base=0, tip=1)
    nointerpolation uint texIdx : TEXCOORD2;   // bindless; 0xFFFFFFFF = none
    nointerpolation uint flags  : TEXCOORD3;   // TrailHeader.renderFlags
};

// U along the trail for a vertex of point-row `row`.
// Arc mode (renderFlags bit0): U anchors at the head (newest = 0) and is per
// ROW, so a swipe texture fans naturally around the swing pivot (base
// compresses, tip tiles more). uvTileLength > 0 tiles at fixed world length
// (speed-independent); 0 stretches the texture once over the live trail.
// Age mode: uniform across the blade, stretches with speed.
float ComputeU(TrailHeader hdr, uint segBase, uint oldestPhys, uint segsPerTrail,
               uint row, float ageAtVert, float arcAtVert)
{
    if (hdr.renderFlags & 1u)
    {
        // World-anchored (renderFlags bit3): U = ABSOLUTE arc since the trail (re)started, fixed when a
        // sample is laid down, so the pattern stays put in the world (in-place dissolve look).
        // uvTileLength == 0 has no normalizer here (a live-length one would slide again): 1 metre per
        // repeat.
        if (hdr.renderFlags & 8u)
            return arcAtVert / ((hdr.uvTileLength > 0.0) ? hdr.uvTileLength : 1.0);

        const uint headRec = (segBase + ((oldestPhys + hdr.count - 1u) % segsPerTrail))
                             * TRAIL_MAX_POINTS + row;
        const float arcHead = gSegments[headRec].arcLen;
        const float distBehindHead = max(arcHead - arcAtVert, 0.0);
        if (hdr.uvTileLength > 0.0)
            return distBehindHead / hdr.uvTileLength;
        const uint oldestRec = (segBase + oldestPhys) * TRAIL_MAX_POINTS + row;
        const float arcOldest = gSegments[oldestRec].arcLen;
        return distBehindHead / max(arcHead - arcOldest, 1e-4);
    }
    return saturate(ageAtVert / max(hdr.maxAge, 1e-4));
}

// Per vertex: (which segment) x (+ or - perpendicular)
//   vertexID 0 = (A, -perp)
//   vertexID 1 = (B, -perp)
//   vertexID 2 = (A, +perp)
//   vertexID 3 = (A, +perp)
//   vertexID 4 = (B, -perp)
//   vertexID 5 = (B, +perp)
VSOut main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOut o = (VSOut)0;
    o.pos = float4(0, 0, 0, 1);            // default: degenerate

    // Instance decode (see the header comment).
    const uint S  = max(maxSubdiv, 1u);
    const uint P1 = max(maxPoints, 2u) - 1u;
    const uint segsPerTrail  = maxSegments;
    const uint pairsPerTrail = segsPerTrail - 1;
    const uint perTrail   = pairsPerTrail * P1 * S;
    // A slot-filtered draw (custom-shader bucket) issues only ONE trail's worth of instances, so
    // SV_InstanceID starts at 0 for that trail: rebase it onto the filtered slot. Unfiltered draws
    // (default bucket) get base 0.
    const uint instBase   = (g_slotFilter != 0xFFFFFFFFu) ? (g_slotFilter * perTrail) : 0u;
    const uint iid        = instanceID + instBase;
    const uint trailIdx   = iid / perTrail;
    if (g_slotFilter != 0xFFFFFFFFu && trailIdx != g_slotFilter)
        return o;
    const uint rem        = iid % perTrail;
    const uint segPairIdx = rem / (P1 * S);
    const uint rem2       = rem % (P1 * S);
    const uint strip      = rem2 / S;
    const uint sub        = rem2 % S;

    TrailHeader hdr = gHeaders[trailIdx];
    const uint pc = clamp(hdr.pointCount, 1u, TRAIL_MAX_POINTS);
    // Custom-shader trails (bucket != 0): colour is ENTIRELY the shader's job; startColor/endColor are
    // ignored and the shader exposes its own colour params (b8 cbuffer + //@default annotations).
    // hdrIntensity still arrives as a flat intensity in color.rgb, the alpha fade in VSOut.alpha.
    const bool customShader = ((hdr.renderFlags >> 8) & 0xFFu) != 0u;

    // Not enough history to form a ribbon pair.
    if (hdr.count < 2u || segPairIdx >= hdr.count - 1u)
        return o;

    // Draw-bucket filter: this draw renders only trails matching BOTH its TAA variant and its
    // custom-shader bucket.
    if (((hdr.renderFlags >> 2) & 1u) != g_variantFilter)
        return o;
    if (((hdr.renderFlags >> 8) & 0xFFu) != g_shaderBucket)
        return o;

    // A single-point trail owns exactly 1/(P1*S) of the budget.
    if (pc <= 1u && (strip != 0u || sub != 0u))
        return o;

    // Logical (oldest-first) -> physical ring slot: logical 0 lives at
    // (head + maxSegs - count) % maxSegs.
    const uint oldestPhys = (hdr.head + segsPerTrail - hdr.count) % segsPerTrail;
    const uint segBase    = trailIdx * segsPerTrail;

    // This vertex's segment and side: v0/v2/v3 sit on segA (sub-segment start), v1/v4/v5 on segB
    // (end); +side for v2/v3/v5, -side for v0/v1/v4.
    const bool onB   = (vertexID == 1u) || (vertexID == 4u) || (vertexID == 5u);
    const bool plus  = (vertexID == 2u) || (vertexID == 3u) || (vertexID == 5u);

    // Multi-point ribbon (pc >= 2): the surface spans the sampled points directly, with no camera
    // facing and no miter. Adjacent sub-quads and pairs evaluate identical control points at identical
    // t, so their vertices coincide (crack-free by construction).
    if (pc > 1u)
    {
        if (strip >= pc - 1u)
            return o;
        const uint Sdiv = clamp(hdr.subdivisions, 1u, S);
        if (sub >= Sdiv)
            return o;

        // Row for this vertex: -side = strip, +side = strip+1 (base -> tip).
        const uint row = strip + (plus ? 1u : 0u);

        // Catmull-Rom control samples, clamped in LOGICAL space before the ring wrap (end knots doubled, no
        // phantom fetch). Keep the math SIGNED: with uint, segPairIdx-1 at 0 wraps to 0xFFFFFFFF, max()
        // keeps it and the read lands far outside the live window.
        const int  j  = (int)segPairIdx;
        const uint L0 = (uint)max(j - 1, 0);
        const uint L1 = segPairIdx;
        const uint L2 = segPairIdx + 1u;
        const uint L3 = min(segPairIdx + 2u, hdr.count - 1u);

        const TrailSegment s0 = gSegments[(segBase + ((oldestPhys + L0) % segsPerTrail)) * TRAIL_MAX_POINTS + row];
        const TrailSegment s1 = gSegments[(segBase + ((oldestPhys + L1) % segsPerTrail)) * TRAIL_MAX_POINTS + row];
        const TrailSegment s2 = gSegments[(segBase + ((oldestPhys + L2) % segsPerTrail)) * TRAIL_MAX_POINTS + row];
        const TrailSegment s3 = gSegments[(segBase + ((oldestPhys + L3) % segsPerTrail)) * TRAIL_MAX_POINTS + row];

        // Dead sub-segment (both endpoint samples past maxAge) -> degenerate.
        if (s1.age > hdr.maxAge && s2.age > hdr.maxAge)
            return o;

        // Sub-segment parameter: A-verts at sub/Sdiv, B-verts at (sub+1)/Sdiv.
        const float t  = (float)(sub + (onB ? 1u : 0u)) / (float)Sdiv;
        const float t2 = t * t;
        const float t3 = t2 * t;

        // Uniform Catmull-Rom on the 4 control positions of THIS row.
        const float3 P0 = s0.position;
        const float3 P1 = s1.position;
        const float3 P2 = s2.position;
        const float3 P3 = s3.position;
        const float3 wp = 0.5 * ((2.0 * P1)
                        + (-P0 + P2) * t
                        + (2.0 * P0 - 5.0 * P1 + 4.0 * P2 - P3) * t2
                        + (-P0 + 3.0 * P1 - 3.0 * P2 + P3) * t3);

        // age/arc: linear between the endpoint samples; Catmull-Rom on scalars can overshoot, linear keeps
        // the fade monotone.
        const float age  = lerp(s1.age, s2.age, t);
        const float arc  = lerp(s1.arcLen, s2.arcLen, t);
        const float ageT = saturate(age / max(hdr.maxAge, 1e-4));

        o.pos   = mul(float4(wp, 1.0), viewProj);
        o.color = customShader ? float4(1.0, 1.0, 1.0, 1.0)
                               : lerp(hdr.startColor, hdr.endColor, ageT);
        o.color.rgb *= max(hdr.hdrIntensity, 0.0);
        o.alpha = 1.0 - ageT;
        o.uv    = float2(ComputeU(hdr, segBase, oldestPhys, segsPerTrail, row, age, arc),
                         (float)row / (float)(pc - 1u));
        o.texIdx = hdr.textureBindlessIdx;
        o.flags  = hdr.renderFlags;
        return o;
    }

    // Single-point camera-facing ribbon (pc == 1), reading the row-0 records.
    const uint physA = (oldestPhys + segPairIdx)     % segsPerTrail;
    const uint physB = (oldestPhys + segPairIdx + 1) % segsPerTrail;

    TrailSegment sA = gSegments[(segBase + physA) * TRAIL_MAX_POINTS];
    TrailSegment sB = gSegments[(segBase + physB) * TRAIL_MAX_POINTS];

    // Dead segment pair (both past maxAge) -> degenerate.
    if (sA.age > hdr.maxAge && sB.age > hdr.maxAge)
        return o;

    // Miter join: at each hub sample average the perpendiculars of the incoming and outgoing tangents,
    // with length compensation so the ribbon edge stays straight through the seam (constant width
    // across corners). Both quads meeting at a hub compute the SAME perp, so their vertices coincide
    // and the ribbon has no gaps where it bends.
    // Endpoints: the oldest sample (segPairIdx == 0, A is the tail) has no predecessor, so tIn = tOut;
    // the newest (segPairIdx == count-2, B is the head) has no successor, so tOut = tIn. Both reduce
    // to the single-tangent perp.
    float3 hubPos;
    float  hubAge;
    float3 tIn, tOut;
    if (onB)
    {
        hubPos = sB.position;
        hubAge = sB.age;
        tIn    = sB.position - sA.position;
        if (segPairIdx + 1u < hdr.count - 1u)
        {
            const uint physC = (oldestPhys + segPairIdx + 2u) % segsPerTrail;
            TrailSegment sC = gSegments[(segBase + physC) * TRAIL_MAX_POINTS];
            tOut = sC.position - sB.position;
        }
        else
        {
            tOut = tIn; // newest sample: no successor
        }
    }
    else
    {
        hubPos = sA.position;
        hubAge = sA.age;
        tOut   = sB.position - sA.position;
        if (segPairIdx > 0u)
        {
            const uint physP = (oldestPhys + segPairIdx - 1u) % segsPerTrail;
            TrailSegment sP = gSegments[(segBase + physP) * TRAIL_MAX_POINTS];
            tIn = sA.position - sP.position;
        }
        else
        {
            tIn = tOut; // oldest sample: no predecessor
        }
    }

    const float3 camN    = normalize(camForward);
    const float3 perpIn  = normalize(cross(normalize(tIn  + 1e-6), camN) + 1e-6);
    const float3 perpOut = normalize(cross(normalize(tOut + 1e-6), camN) + 1e-6);

    float3 miterPerp;
    float3 miterSum   = perpIn + perpOut;
    const float miterL = length(miterSum);
    if (miterL < 1e-4)
    {
        // ~180 deg turn (perpIn and perpOut antiparallel) is degenerate: pick one; it looks bad either way
        // at that angle.
        miterPerp = perpIn;
    }
    else
    {
        const float3 miterDir = miterSum / miterL;
        // Length compensation: 1 / cos(theta/2), theta the angle between perpIn and perpOut;
        // cos(theta/2) = dot(miterDir, perpIn) since miterDir bisects them. Clamped to 4x so a near-hairpin
        // does not shoot the corner vertex out (miter spike).
        const float factor = clamp(1.0 / max(dot(miterDir, perpIn), 0.25), 1.0, 4.0);
        miterPerp = miterDir * factor;
    }

    // Narrow the ribbon as the segment ages (older = thinner).
    const float ageT  = saturate(hubAge / max(hdr.maxAge, 1e-4));
    const float halfW = hdr.width * (1.0 - 0.6 * ageT);

    float3 worldPos = hubPos + miterPerp * (plus ? halfW : -halfW);
    o.pos = mul(float4(worldPos, 1.0), viewProj);

    // Age-based color + alpha fade (custom shaders own color entirely).
    o.color = customShader ? float4(1.0, 1.0, 1.0, 1.0)
                           : lerp(hdr.startColor, hdr.endColor, ageT);
    o.color.rgb *= max(hdr.hdrIntensity, 0.0);
    o.alpha = 1.0 - ageT;

    // UV: U from the hub sample's age/arc (row 0), V = ribbon side.
    const float hubArc = onB ? sB.arcLen : sA.arcLen;
    o.uv     = float2(ComputeU(hdr, segBase, oldestPhys, segsPerTrail, 0u, hubAge, hubArc),
                      plus ? 1.0 : 0.0);
    o.texIdx = hdr.textureBindlessIdx;
    o.flags  = hdr.renderFlags;

    return o;
}
