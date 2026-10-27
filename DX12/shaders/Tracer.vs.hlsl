// Tracer vertex shader: cylindrical-billboard quads from StructuredBuffer<Tracer>.
// One instance per pool slot, 4 verts each (TRIANGLESTRIP). A dead tracer (lifetime <= 0) emits a
// degenerate quad the rasterizer drops before the PS, as Particle.vs.hlsl does.
// sideDir = normalize(cross(beamAxis, toCamera)), with a fallback when the beam is nearly parallel
// to the view ray.
// Bindings (graphics, space0):
//   b2 -> TracerRenderCB (viewProj, cameraPos, time, fadeRange...)
//   t4 -> descriptor table -> StructuredBuffer<Tracer> gPool

#include "Tracer.hlsli"

cbuffer TracerRenderCB : register(b2, space0)
{
    float4x4 viewProj;       // jittered (for SV_POSITION)
    float3   cameraPos;
    float    time;
    float    nearZ;
    float    farZ;
    float    fadeRange;      // soft-particle fade distance, view-space metres
    float    coreSharpness;  // PS triangular falloff exponent
    float    noiseTiling;    // along-beam noise tile count
    float    scrollSpeed;    // along-beam noise UV/s
    float    noiseFloor;     // 0..1 minimum noise multiplier
    float    _pad;
};

StructuredBuffer<Tracer> gPool : register(t4, space0);

struct VSOut
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;     // x = side (0..1), y = along-beam (0..1)
    float4 color     : COLOR0;
    float  lifeAlpha : TEXCOORD1;     // 0..1, fades to 0 over the life
    nointerpolation uint noiseIdx : TEXCOORD2;  // bindless index, 0xFFFFFFFFu = procedural
    float  spawnTime : TEXCOORD3;     // for noise-scroll continuity across frames
};

// Triangle-strip quad along the beam axis:
//   v0 = (-side, start),  v1 = (+side, start)
//   v2 = (-side, end),    v3 = (+side, end)
// x = side (-0.5..0.5), y = along the beam (0..1).
static const float2 kQuadOffsets[4] =
{
    float2(-0.5, 0.0),
    float2( 0.5, 0.0),
    float2(-0.5, 1.0),
    float2( 0.5, 1.0),
};

VSOut main(uint vID : SV_VertexID, uint iID : SV_InstanceID)
{
    VSOut o = (VSOut)0;

    Tracer t = gPool[iID];

    if (t.lifetime <= 0.0 || t.maxLifetime <= 0.0)
    {
        // Degenerate quad: the rasterizer drops it.
        o.pos       = float4(0, 0, 0, 1);
        o.color     = float4(0, 0, 0, 0);
        o.noiseIdx  = 0xFFFFFFFFu;
        return o;
    }

    const float2 q = kQuadOffsets[vID];

    // Point on the beam centreline at this vertex's along-beam parameter.
    float3 axisPos = lerp(t.startPos, t.endPos, q.y);

    float3 beamSeg = t.endPos - t.startPos;
    float  beamLen = length(beamSeg);
    float3 beamAxis = beamLen > 1e-4 ? beamSeg / beamLen : float3(0, 1, 0);

    float3 toCam = cameraPos - axisPos;
    float  toCamLen = length(toCam);
    float3 toCamN = toCamLen > 1e-4 ? toCam / toCamLen : float3(0, 0, 1);

    // Cylindrical billboard: sideDir is perpendicular to both the beam and the view.
    float3 sideDir = cross(beamAxis, toCamN);
    float  sideLen = length(sideDir);

    // Degenerate when the view ray is nearly parallel to the beam.
    if (sideLen < 1e-3)
    {
        // Any axis perpendicular to beamAxis is a stable fallback.
        float3 up = abs(beamAxis.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
        sideDir = cross(beamAxis, up);
        sideLen = length(sideDir);
    }
    sideDir = sideLen > 1e-4 ? sideDir / sideLen : float3(1, 0, 0);

    float3 worldPos = axisPos + sideDir * (q.x * t.width);

    o.pos = mul(float4(worldPos, 1.0), viewProj);

    o.uv = float2(q.x + 0.5, q.y);
    o.color     = t.color;
    o.lifeAlpha = saturate(t.lifetime / t.maxLifetime);
    o.noiseIdx  = t.noiseTexBindlessIdx;
    o.spawnTime = t.spawnTime;

    return o;
}
