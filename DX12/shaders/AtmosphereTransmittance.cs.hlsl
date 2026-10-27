// TransmittanceLUT (256x64, R16G16B16A16_FLOAT), baked once (again after a shader hot reload).
// uv.x -> altitude, uv.y -> cos(sun zenith) (via ParamsToLutUv); value -> rgb transmittance
// from that altitude to the atmosphere top along that direction (0 when the ground blocks it).
// 40-step midpoint integration of the extinction coefficient along a straight line.

#include "AtmosphereCommon.hlsli"

cbuffer TransmittanceCB : register(b0, space2)
{
    uint lutWidth;
    uint lutHeight;
    uint _pad0;
    uint _pad1;
};

RWTexture2D<float4> DstLUT : register(u0, space2);

static const int kTransmittanceSteps = 40;

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= lutWidth || DTid.y >= lutHeight) return;

    float2 uv = (float2(DTid.xy) + 0.5) / float2(lutWidth, lutHeight);

    float h, mu;
    LutUvToParams(uv, h, mu);

    // Start on the x=0 plane at y = kGroundR + h, direction in the x/y plane with mu = cos(zenith).
    float  r  = kGroundR + h;
    float3 p0 = float3(0.0, r, 0.0);
    float3 rd = float3(sqrt(saturate(1.0 - mu * mu)), mu, 0.0);

    float tMax = DistanceToAtmosphereTop(p0, rd);
    if (tMax < 0.0)
    {
        DstLUT[DTid.xy] = float4(0, 0, 0, 1); // ray hits the ground first
        return;
    }

    float dt = tMax / float(kTransmittanceSteps);
    float3 opticalDepth = 0;
    [loop] for (int i = 0; i < kTransmittanceSteps; ++i)
    {
        float3 p = p0 + rd * (dt * (float(i) + 0.5));
        float  alt = length(p) - kGroundR;
        AtmoMedium m = SampleAtmosphere(alt);
        opticalDepth += m.extinction * dt;
    }

    float3 T = exp(-opticalDepth);
    DstLUT[DTid.xy] = float4(T, 1.0);
}
