// SkyAtmosphere.cs.hlsl - Hillaire 2020 sky cubemap generator. Each face texel's direction becomes
// (azimuth, elevation) to sample the SkyViewLUT written by AtmosphereSkyView.cs.hlsl.
// Root signature:
//   [0] CBV b0 space2 - AtmosphereCB
//   [1] SRV t0 space2 - SkyViewLUT
//   [2] SRV t1 space2 - TransmittanceLUT (bound, not read here)
//   [4] UAV u0 space2 - output RWTexture2DArray<float4>

#include "AtmosphereCommon.hlsli"

cbuffer AtmosphereCB : register(b0, space2)
{
    float3 SunDir;          float _pad0;
    float3 SunColor;        uint  FaceSize;
    float  CameraAltitudeKm;
    float  _pad1;
    float  _pad2;
    float  _pad3;
};

Texture2D<float4>         SkyViewLUT       : register(t0, space2);
Texture2D<float4>         TransmittanceLUT : register(t1, space2);
RWTexture2DArray<float4>  DstFace          : register(u0, space2);
SamplerState              LinClamp         : register(s0, space2);

float3 FaceUVToDir(uint face, float2 uv)
{
    float3 d;
    [branch] switch (face)
    {
        case 0: d = float3( 1.0, -uv.y, -uv.x); break;
        case 1: d = float3(-1.0, -uv.y,  uv.x); break;
        case 2: d = float3( uv.x,  1.0,  uv.y); break;
        case 3: d = float3( uv.x, -1.0, -uv.y); break;
        case 4: d = float3( uv.x, -uv.y,  1.0); break;
        default: d = float3(-uv.x, -uv.y, -1.0); break;
    }
    return normalize(d);
}

float3 SampleSky(float3 rd)
{
    float elevation = asin(clamp(rd.y, -1.0, 1.0));
    float azimuth   = atan2(rd.z, rd.x);
    if (azimuth < 0.0) azimuth += 2.0 * PI;

    float2 uv = float2(azimuth / (2.0 * PI), SkyViewElevationToV(elevation));
    return SkyViewLUT.SampleLevel(LinClamp, uv, 0).rgb;
}

// No sun disk here: SkyboxPass (Skybox.ps.hlsl) draws it at pixel resolution. The cubemap holds
// scattering only, which SH projection and the specular pre-filter want (a sharp sun would
// overwhelm both).
// SunColor already is the solar illuminance OUTSIDE the atmosphere, in engine light units:
// Renderer_IBL hands the authored colour to SetAtmosphereSun, and only SetLightCBSun multiplies
// by the transmittance for LightCB.lightColor. Scale the sky by SunColor as-is, the number that
// lights the scene, like aerial perspective and the cloud pass do. Do NOT divide by the
// transmittance: extinction would apply twice and diverge as the sun sets (T -> 0).
// Use the magnitude (channel max), not the tinted rgb: the SkyView LUT makes its own orange/blue
// gradient, and the authored sunset warmth on top would double-count it and crush blue.
// Twilight floor: TOD zeroes SunColor the instant the geometric sun drops below the horizon
// (TODSystems.cpp), which would snap the cubemap to black; 0.25 is the night sky level.
static const float kTwilightIlluminance = 0.25;

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= FaceSize || DTid.y >= FaceSize) return;

    float2 uv  = (float2(DTid.xy) + 0.5) / float(FaceSize) * 2.0 - 1.0;
    float3 dir = FaceUVToDir(DTid.z, uv);

    float3 sky = SampleSky(dir);

    // The SkyView LUT is normalised to solar illuminance 1, so this is its only scale; it tracks the
    // Sun Intensity slider linearly, in lockstep with LightCB.lightColor (the same value x
    // transmittance).
    float solarIlluminance = max(max(SunColor.r, SunColor.g), SunColor.b);
    solarIlluminance = max(solarIlluminance, kTwilightIlluminance);

    float3 color = sky * solarIlluminance;
    DstFace[uint3(DTid.xy, DTid.z)] = float4(color, 1.0);
}
