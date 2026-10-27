// DDGIProbeDebug.vs.hlsl - one camera-facing impostor quad per probe.
// Paired with the PS's analytic ray-sphere intersection: a round silhouette and smooth
// normal at any radius, so anything jagged on a probe sphere is a data problem, not
// tessellation.
// Host: DrawInstanced(6, probeCount, 0, 0); SV_InstanceID = linear probe index. The
// quad circumscribes the sphere's perspective silhouette (r / sqrt(1 - (r/d)^2),
// slightly larger than r) so no sphere pixel is clipped at grazing distances.

#include "DDGICommon.hlsli"

cbuffer PerViewCB : register(b0, space0)
{
    float4x4 g_ViewProj;             // current, jittered (matches scene depth)
    float4x4 g_PrevViewProj;         // unused here
    float4x4 g_CurViewProjNoJitter;  // unused here
    float4   g_TimeParams;           // unused here
    float4   g_CameraPosWS;          // xyz = camera world position
};

ConstantBuffer<DDGIVolumeGPU> g_Vol : register(b1, space0);

// ProbeData SRV: the VS adds pd.offset so the view matches the relocated positions the
// trace and sampling use.
StructuredBuffer<DDGIProbeData> g_ProbeData : register(t1, space0);

cbuffer DebugConstants : register(b2, space0)
{
    float g_SphereRadius;
    float _padR0, _padR1, _padR2;
};

struct VSOut
{
    float4 pos      : SV_POSITION;
    float3 worldPos : WORLD_POS;    // quad surface point; the PS builds the eye ray from it
    nointerpolation float3 center   : SPHERE_CENTER;
    nointerpolation uint   probeIdx : PROBE_IDX;
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // Probe centre from the instance index, plus the relocation offset, so the sphere sits
    // where the trace origin and DDGI_SampleVolume put the probe.
    int3 coord = DDGI_ProbeCoord(iid, g_Vol);
    float3 center = DDGI_ProbeWorldPos(coord, g_Vol);
    {
        DDGIProbeData pd = g_ProbeData[iid];
        center += pd.offset;
    }

    // Camera-facing basis. Degenerate when looking straight along Y at the probe: fall
    // back to X as the right vector.
    float3 toCam = g_CameraPosWS.xyz - center;
    float  d     = length(toCam);
    float3 fwd   = (d > 1e-4) ? toCam / d : float3(0, 0, 1);
    float3 right = cross(float3(0, 1, 0), fwd);
    if (dot(right, right) < 1e-6) right = float3(1, 0, 0);
    right = normalize(right);
    float3 up = cross(fwd, right);

    // Quad half-size = perspective silhouette radius r/sqrt(1-(r/d)^2). Camera at or
    // inside the sphere -> k <= 0: make the quad huge; the PS intersection still resolves
    // per pixel.
    const float r = max(g_SphereRadius, 1e-4);
    const float k = d * d - r * r;
    const float halfSize = (k > 1e-6) ? (r * d * rsqrt(k)) : (r * 10.0);

    // Two CCW triangles: (-1,-1)(1,-1)(1,1) + (-1,-1)(1,1)(-1,1).
    static const float2 kCorner[6] = {
        float2(-1, -1), float2(1, -1), float2(1, 1),
        float2(-1, -1), float2(1,  1), float2(-1, 1),
    };
    float2 c = kCorner[vid];
    float3 worldPos = center + (right * c.x + up * c.y) * halfSize;

    VSOut o;
    o.pos      = mul(float4(worldPos, 1.0), g_ViewProj);
    o.worldPos = worldPos;
    o.center   = center;
    o.probeIdx = iid;
    return o;
}
