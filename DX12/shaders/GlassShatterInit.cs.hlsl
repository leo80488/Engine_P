// GlassShatterInit.cs.hlsl - one-shot per-shard physics initialisation, dispatched
// at trigger time. Reads the CPU-baked per-shard centroid + edge metadata from
// g_ShardMeta and writes the initial Shard physics state into the RW shard buffer.
//
// Initial linear velocity = radial-from-impact + jitter, scaled by
// ImpactRadialStrength / (1 + dist*2). The initial spawnDelay grows with the
// distance from the impact, so the cracks visibly propagate outward.
//
// Compute root signature (space2):
//   b0 - ShatterCB
//   t0 - StructuredBuffer<ShardMeta>      (centroidUV, edgeOffset, edgeCount)
//   u0 - RWStructuredBuffer<Shard>

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

struct ShardMeta
{
    float2 centroidUV;
    uint   edgeOffset;
    uint   edgeCount;
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

StructuredBuffer<ShardMeta>   g_ShardMeta  : register(t0, space2);
RWStructuredBuffer<Shard>     g_Shards     : register(u0, space2);

float Hash11(float n)
{
    return frac(sin(n) * 43758.5453);
}

float2 Hash22(float n)
{
    return float2(Hash11(n), Hash11(n + 17.31));
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint id = dtid.x;
    if (id >= ShardCount) return;

    ShardMeta meta = g_ShardMeta[id];

    Shard s;
    s.centroidUV  = meta.centroidUV;
    s.translation = float2(0.0, 0.0);
    s.angle2D     = 0.0;
    s.seed        = Hash11(float(id) * 0.1234);
    s.edgeOffset  = meta.edgeOffset;
    s.edgeCount   = meta.edgeCount;
    s._pad0       = 0.0;
    s._pad1       = 0.0;
    s._pad2       = 0.0;

    // Impact -> shard centroid in UV. The distance falloff softens the radial impulse,
    // so far shards drift and near shards fly.
    const float2 dir  = meta.centroidUV - ImpactPointUV;
    const float  dist = length(dir) + 1.0e-4;
    const float2 unit = dir / dist;

    // To NDC scale: UV span [0,1] = NDC span [-1,1] = scale 2; Y flipped (UV y-down,
    // NDC y-up).
    const float2 unitNDC = float2(unit.x * 2.0, -unit.y * 2.0);
    const float2 radial  = unitNDC * (ImpactRadialStrength / (1.0 + dist * 2.0));

    const float2 jitter = (Hash22(float(id) * 0.713) * 2.0 - 1.0) * 0.3;
    s.linearVelocity = radial + jitter;

    // Angular: random sign, magnitude inversely proportional to distance.
    s.angularVel2D = (Hash11(float(id) + 11.0) * 2.0 - 1.0) * 6.0 / (1.0 + dist * 3.0);

    // Stagger: cracks propagate outward; far shards start later.
    s.spawnDelay = dist * 0.18 + Hash11(float(id) + 7.0) * 0.04;

    s.alpha = 1.0;

    g_Shards[id] = s;
}
