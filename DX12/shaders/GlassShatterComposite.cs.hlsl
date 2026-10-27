// GlassShatterComposite.cs.hlsl - per-pixel polygon test against every shard.
//
// Each thread handles one output pixel and writes back into the Tonemap output UAV
// in place:
//
//   pixelNDC = (uv * 2 - 1) with y flipped
//   for each shard s (in buffer order):
//       if s.alpha < eps : skip
//       shardCentreNDC = (s.centroidUV * 2 - 1) with y flipped
//       livePos = shardCentreNDC + s.translation
//       local   = pixelNDC - livePos
//       unrot   = R(-angle2D) x local
//       if forall edges e: dot(e.normal, unrot) <= e.d : "inside"
//           sourceUV = s.centroidUV + (unrot.x*0.5, -unrot.y*0.5)
//           col      = ShatterSourceTex.Sample(LinearClamp, sourceUV)
//           col      = lerp(col, CrackColor, edgeProximity) + rim sheen
//           output   = lerp(currentTonemap, col, s.alpha); stop
//
// Order: the first shard in the buffer that covers a pixel wins (is on top). With
// ~80 shards cut from the same source frame overlap is rare, so order matters
// mostly when shards drift across each other late in the effect.
//
// Compute root signature (space2):
//   b0 - ShatterCB
//   t0 - StructuredBuffer<ShardEdge>          (UPLOAD heap, always GENERIC_READ)
//   t1 - Texture2D<float4>     ShatterSourceTex (frozen scene)
//   s0 - SamplerState LinearClamp
//   u0 - RWTexture2D<float4>  TonemapOutput (in place)
//   u1 - RWStructuredBuffer<Shard> (read-only access; bound as a UAV to avoid
//                                    UAV -> SRV transitions on a buffer the engine
//                                    does not fully state-track across frames)

cbuffer ShatterCB : register(b0, space2)
{
    float  TimeSinceTrigger;
    float  DeltaTime;
    float  Duration;
    uint   ShardCount;

    float2 ImpactPointUV;
    float  ImpactRadialStrength;
    float  GravityNDC;

    float  AirDrag;
    float  AngularDamping;
    float  CrackWidth;
    float  HoldDuration;

    float3 CrackColor;
    float  _pad0;

    uint   Width;
    uint   Height;
    uint   _pad1;
    uint   _pad2;
};

struct Shard
{
    float2 centroidUV;
    float2 translation;
    float  angle2D;
    float  angularVel2D;
    float2 linearVelocity;
    float  spawnDelay;
    float  alpha;
    float  seed;
    uint   edgeOffset;
    uint   edgeCount;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

struct ShardEdge
{
    float2 normal;   // outward in shard-local pre-rotation frame; UV scale
    float  d;        // dot(normal, vert) along the boundary; inside <=> dot(n,p) <= d
    float  _pad;
};

StructuredBuffer<ShardEdge> g_Edges          : register(t0, space2);
Texture2D<float4>           g_ShatterSource  : register(t1, space2);
SamplerState                g_LinearClamp    : register(s0, space2);
RWTexture2D<float4>         g_Output         : register(u0, space2);
RWStructuredBuffer<Shard>   g_Shards         : register(u1, space2);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    const float2 res    = float2(Width, Height);
    const float2 invRes = 1.0 / res;
    const float2 uv     = (float2(dtid.xy) + 0.5) * invRes;

    // Pixel in NDC (y-up).
    const float2 ndc = float2(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0));

    // Start from the current tonemap output: the live scene shows where no shard is.
    float4 outCol = g_Output[int2(dtid.xy)];

    // First shard (by buffer index) that covers this pixel wins; the rest are
    // skipped. Close enough for the visual; a true painter's sort would need a depth
    // surrogate (e.g. translation magnitude).
    for (uint si = 0; si < ShardCount; ++si)
    {
        const Shard s = g_Shards[si];
        if (s.alpha < 0.01) continue;
        // No spawnDelay gate: shards render from frame 0 (cracks visible during the
        // hold). The sim leaves translation=0 / angle=0 until release, so pre-release
        // shards composite at their rest pose and the screen looks frozen-with-cracks
        // until the breakup begins.

        // Shard centroid in NDC (y-up).
        const float2 cNDC0 = float2(s.centroidUV.x * 2.0 - 1.0,
                                    -(s.centroidUV.y * 2.0 - 1.0));
        const float2 cNDC  = cNDC0 + s.translation;

        // Pixel in the shard's centroid-local NDC (post-translation, pre-rotation).
        const float2 local = ndc - cNDC;

        // Inverse rotation by -angle2D into the shard's pre-rotation local frame, where
        // the polygon edges are stored.
        const float ca = cos(-s.angle2D);
        const float sa = sin(-s.angle2D);
        const float2 unrotNDC = float2(local.x * ca - local.y * sa,
                                       local.x * sa + local.y * ca);

        // Edges are stored in UV scale (the Voronoi cell was cut in UV space). NDC-local
        // -> UV-local: scale 0.5 and flip Y (NDC y-up, UV y-down).
        const float2 unrotUV = float2(unrotNDC.x * 0.5, -unrotNDC.y * 0.5);

        // Polygon-inside test: the pixel must be on the inside of every edge. The largest
        // signed distance (positive = outside) gives the edge falloff for the crack
        // highlight.
        bool  inside     = true;
        float maxOutside = -1.0e9;
        for (uint ei = 0; ei < s.edgeCount; ++ei)
        {
            const ShardEdge e = g_Edges[s.edgeOffset + ei];
            const float     v = dot(e.normal, unrotUV) - e.d;
            maxOutside = max(maxOutside, v);
            if (v > 0.0) { inside = false; break; }
        }
        if (!inside) continue;

        // Source UV in the original screen: pre-rotation local -> absolute UV via the
        // untransformed centroid.
        const float2 sourceUV = saturate(s.centroidUV + unrotUV);

        const float3 col = g_ShatterSource.SampleLevel(
            g_LinearClamp, sourceUV, 0).rgb;

        // Crack highlight: maxOutside is in [-Inf, 0] inside the cell. Pixels near an edge
        // (maxOutside near 0) are tinted with CrackColor over a feathered width =
        // CrackWidth (UV units).
        const float edgeProx = saturate(1.0 - (-maxOutside) / max(CrackWidth, 1.0e-5));
        float3 shardCol = lerp(col, CrackColor, edgeProx * 0.85);

        // Subtle rim sheen on the inside of the crack.
        shardCol += (edgeProx * edgeProx) * 0.3 * float3(0.7, 0.85, 1.0);

        outCol.rgb = lerp(outCol.rgb, shardCol, s.alpha);
        outCol.a   = 1.0;
        break;
    }

    g_Output[int2(dtid.xy)] = outCol;
}
