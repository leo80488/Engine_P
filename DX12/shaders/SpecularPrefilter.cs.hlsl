// GGX importance-sampled specular IBL prefilter (split-sum approximation).
// Writes one mip of one face of the output cube (RWTexture2DArray, 6 slices) from a
// source TextureCube of radiance; the caller derives Roughness from the mip level.
// Dispatch((FaceSize + 7) / 8, (FaceSize + 7) / 8, 1) per (face, mip).
// Callers: SkyIBLPass (sky IBL) and ReflectionProbeCapturePass (UseCoverage = 1).
// Shared compute root signature (RHI::Slots::Cmp):
//   [0] CBV b0 space2 - PrefilterCB
//   [1] SRV t0 space2 - source TextureCube<float4>
//   [4] UAV u0 space2 - output RWTexture2DArray<float4> (one mip)
//   static sampler s0 space2 - linear clamp

#include "core/math.hlsli"   // PI

cbuffer PrefilterCB : register(b0, space2)
{
    uint  FaceSize;      // output face edge (e.g. 128 >> mip)
    uint  SampleCount;   // GGX samples per texel; the caller's table gives 1 at mip 0, 32-128 above
    float Roughness;     // mip-based roughness in [0, 1]
    uint  SourceMipCount;// mip count of the source cube (clamps the LOD bias; both callers pass 1)
    uint  FaceIndex;     // the cube face (array slice) this dispatch writes (0..5)
    uint  UseCoverage;   // 1 = source alpha is probe geometry coverage, 0 = ordinary sky IBL
    uint  _pad1;
    uint  _pad2;
};

TextureCube<float4>       SrcEnv    : register(t0, space2);
RWTexture2DArray<float4>  DstFace   : register(u0, space2);
SamplerState              LinClamp  : register(s0, space2);

// ==== GGX sampling helpers ====
#include "core/sampling.hlsli"   // TangentFrameRefZ, Hammersley2D

float3 ImportanceSampleGGX(float2 xi, float3 N, float a)
{
    float phi      = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    // Tangent-space basis aligned with N.
    float3 T, B;
    TangentFrameRefZ(N, T, B);
    return normalize(T * H.x + B * H.y + N * H.z);
}

float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float d  = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (PI * d * d + 1e-7);
}

// Cube face + uv -> direction, D3D cubemap face order:
//   0: +X   1: -X   2: +Y   3: -Y   4: +Z   5: -Z
float3 FaceUVToDir(uint face, float2 uv)
{
    // uv in [-1, +1]
    float3 d;
    [branch] switch (face)
    {
        case 0: d = float3( 1.0, -uv.y, -uv.x); break;  // +X
        case 1: d = float3(-1.0, -uv.y,  uv.x); break;  // -X
        case 2: d = float3( uv.x,  1.0,  uv.y); break;  // +Y
        case 3: d = float3( uv.x, -1.0, -uv.y); break;  // -Y
        case 4: d = float3( uv.x, -uv.y,  1.0); break;  // +Z
        default: d = float3(-uv.x, -uv.y, -1.0); break; // -Z
    }
    return normalize(d);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= FaceSize || DTid.y >= FaceSize) return;

    // One face per dispatch (FaceIndex), so SkyIBLPass can refresh one face per frame.
    uint face = FaceIndex;
    // Pixel center UV in [-1, +1].
    float2 uv = (float2(DTid.xy) + 0.5) / float(FaceSize) * 2.0 - 1.0;
    float3 N  = FaceUVToDir(face, uv);

    // Split-sum assumption: V = R = N.
    float3 R = N;
    float3 V = N;

    float a = max(Roughness * Roughness, 1e-3);

    float3 radianceSum = float3(0, 0, 0);
    float  coverageSum = 0.0;
    float  weightSum   = 0.0;

    // Roughness ~0 (mip 0): all the weight is at the mirror direction, so read the
    // source texel instead of running the sample loop.
    if (Roughness < 1e-3)
    {
        const float4 source = SrcEnv.SampleLevel(LinClamp, N, 0);
        const float sourceCoverage = (UseCoverage != 0u) ? saturate(source.a) : 1.0;
        radianceSum = source.rgb * sourceCoverage;
        coverageSum = sourceCoverage;
        weightSum   = 1.0;
    }
    else
    {
        for (uint i = 0; i < SampleCount; ++i)
        {
            float2 xi    = Hammersley2D(i, SampleCount);
            float3 H     = ImportanceSampleGGX(xi, N, a);
            float3 L     = normalize(2.0 * dot(V, H) * H - V);
            float  NdotL = saturate(dot(N, L));
            if (NdotL <= 0.0) continue;

            // Filtered importance sampling (Krivanek): choose the source LOD from the
            // sample's solid angle vs a texel's, so sparse high-roughness lobes do not alias.
            float NdotH = saturate(dot(N, H));
            float VdotH = saturate(dot(V, H));
            float pdf   = D_GGX(NdotH, a) * NdotH / (4.0 * VdotH) + 1e-6;
            float saTex = 4.0 * PI / (6.0 * float(FaceSize) * float(FaceSize));
            float saSam = 1.0 / (float(SampleCount) * pdf);
            float lod   = 0.5 * log2(saSam / saTex);
            lod = clamp(lod, 0.0, float(max(int(SourceMipCount) - 1, 0)));

            const float4 source = SrcEnv.SampleLevel(LinClamp, L, lod);
            const float coverage = (UseCoverage != 0u) ? saturate(source.a) : 1.0;
            radianceSum += source.rgb * (coverage * NdotL);
            coverageSum += coverage * NdotL;
            weightSum   += NdotL;
        }
    }

    // RGB is radiance conditioned on hitting captured geometry; alpha is the
    // GGX-filtered geometry coverage of the lobe. The runtime can then fall through
    // from the probe to the directional sky without dimming RGB twice.
    const float coverage = coverageSum / max(weightSum, 1e-4);
    const float3 radiance = (coverageSum > 1e-4)
                          ? (radianceSum / coverageSum)
                          : float3(0.0, 0.0, 0.0);
    DstFace[uint3(DTid.xy, face)] = float4(radiance, saturate(coverage));
}
