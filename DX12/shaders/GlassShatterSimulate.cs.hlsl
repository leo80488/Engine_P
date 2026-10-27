// GlassShatterSimulate.cs.hlsl - per-frame physics integration for every shard,
// every frame the effect is active. Reads / writes the same RW buffer as Init.
//
// Integration:
//   v += g * dt                  (gravity along NDC-y, < 0 falls down)
//   v *= (1 - drag * dt)         (linear damping)
//   p += v * dt
//   w *= (1 - angularDamp * dt)  (angular damping)
//   angle += w * dt
//
// Fade-out: alpha ramps 1 -> 0 over the last 30% of Duration; the composite pass
// skips shards once alpha drops below 0.01.

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

RWStructuredBuffer<Shard> g_Shards : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint id = dtid.x;
    if (id >= ShardCount) return;

    Shard s = g_Shards[id];

    // Two-stage gate:
    //   1. HoldDuration freezes EVERY shard at its rest pose: the composite shows the
    //      frozen scene with crack lines but no shard motion (impact "hit-stop").
    //   2. The per-shard spawnDelay then staggers the release after the hold, so the
    //      cracks visibly propagate outward from the impact.
    if (TimeSinceTrigger < HoldDuration + s.spawnDelay)
    {
        g_Shards[id] = s;
        return;
    }

    const float dt = DeltaTime;

    // Linear: gravity (downward in NDC), then drag, then integrate.
    s.linearVelocity.y += GravityNDC * dt;
    s.linearVelocity   *= max(0.0, 1.0 - AirDrag * dt);
    s.translation      += s.linearVelocity * dt;

    // Angular: damp then integrate.
    s.angularVel2D *= max(0.0, 1.0 - AngularDamping * dt);
    s.angle2D      += s.angularVel2D * dt;

    // Fade: smooth ramp over the last 30% of Duration.
    const float t = saturate(TimeSinceTrigger / max(Duration, 1.0e-4));
    s.alpha = 1.0 - smoothstep(0.7, 1.0, t);

    g_Shards[id] = s;
}
