// VERBATIM port of Intel's XeGTAO main pass.
// Source: github.com/GameTechDev/XeGTAO (SPDX MIT, Filip Strugar / Intel 2016-2021),
// Source/Rendering/Shaders/XeGTAO.hlsli :: XeGTAO_MainPass.
// Engine adaptations:
//   * Compute root signature space2: b0 CB, t0 prefiltered linear depth (5 mips),
//     t1 GBuffer world normals, t2 Hilbert LUT, s0 linear, u0 AO, u1 edge mask.
//   * Normals are GBuffer world-space (packed *0.5+0.5), rotated to view space by
//     ViewMatrix (uploaded row-major, read column-major, so mul(M, v) is world -> view).
//   * Output is R8_UNORM visibility + R8_UNORM edges, not the reference's packed-uint
//     bent-normal format (there is no bent-normal path).

// GTAOConstants: field order matches XeGTAOPass::GTAOConstants on the C++ side;
// Intel's XeGTAO.h layout plus a ViewMatrix tail.
cbuffer GTAOCB : register(b0, space2)
{
    int2   ViewportSize;
    float2 ViewportPixelSize;

    float2 DepthUnpackConsts;
    float2 CameraTanHalfFOV;

    float2 NDCToViewMul;
    float2 NDCToViewAdd;

    float2 NDCToViewMul_x_PixelSize;
    float  EffectRadius;
    float  EffectFalloffRange;

    float  RadiusMultiplier;
    float  Padding0;
    float  FinalValuePower;
    float  DenoiseBlurBeta;

    float  SampleDistributionPower;
    float  ThinOccluderCompensation;
    float  DepthMIPSamplingOffset;
    int    NoiseIndex;

    uint   SliceCount;
    uint   StepsPerSlice;
    float2 _pad1;

    // Engine extension: world -> view transform for the GBuffer normal.
    float4x4 ViewMatrix;
};

Texture2D<float>    gLinearDepth : register(t0, space2);   // 5-mip linearized depth
Texture2D<float4>   gNormals     : register(t1, space2);   // packed world normals
Texture2D<uint>     gHilbertLUT  : register(t2, space2);   // 64x64 R16_UINT Hilbert curve
RWTexture2D<float>  gOutput      : register(u0, space2);   // R8_UNORM visibility
RWTexture2D<float>  gEdges       : register(u1, space2);   // R8_UNORM packed edges
SamplerState        gLinear      : register(s0, space2);

#define XE_GTAO_PI              3.1415926535897932384626433832795
#define XE_GTAO_PI_HALF         1.5707963267948966192313216916398
#define XE_GTAO_DEPTH_MIP_LEVELS 5

// ==== Helpers, verbatim from XeGTAO.hlsli ====
float3 XeGTAO_ComputeViewspacePosition(const float2 screenPos, const float viewspaceDepth)
{
    float3 ret;
    ret.xy = (NDCToViewMul * screenPos.xy + NDCToViewAdd) * viewspaceDepth;
    ret.z  = viewspaceDepth;
    return ret;
}

float XeGTAO_FastSqrt(float x)
{
    return asfloat(0x1fbd1df5 + (asint(x) >> 1));
}

float XeGTAO_FastACos(float inX)
{
    const float PI      = 3.141593;
    const float HALF_PI = 1.570796;
    float x   = abs(inX);
    float res = -0.156583 * x + HALF_PI;
    res *= XeGTAO_FastSqrt(1.0 - x);
    return (inX >= 0) ? res : PI - res;
}

// Edge mask from viewspace depth gradients: XeGTAO_CalculateEdges / XeGTAO_PackEdges
// from the reference.
float4 XeGTAO_CalculateEdges(float centerZ, float leftZ, float rightZ,
                             float topZ,   float bottomZ)
{
    float4 edgesLRTB = float4(leftZ, rightZ, topZ, bottomZ) - centerZ;
    float slopeLR = (edgesLRTB.y - edgesLRTB.x) * 0.5;
    float slopeTB = (edgesLRTB.w - edgesLRTB.z) * 0.5;
    float4 slopeAdj = edgesLRTB + float4(slopeLR, -slopeLR, slopeTB, -slopeTB);
    edgesLRTB = min(abs(edgesLRTB), abs(slopeAdj));
    return saturate(1.25 - edgesLRTB / (centerZ * 0.011));
}

float XeGTAO_PackEdges(float4 edgesLRTB)
{
    edgesLRTB = round(saturate(edgesLRTB) * 2.9);
    return dot(edgesLRTB, float4(64.0/255.0, 16.0/255.0, 4.0/255.0, 1.0/255.0));
}

// Verbatim XeGTAO_SpatioTemporalNoise (vaGTAO.hlsl): the 64x64 Hilbert LUT gives a
// spatially decorrelated base index, and temporalIndex offsets the R2 low-discrepancy
// sequence. 288 x (temporalIndex % 64) is the reference's best empirical decorrelation
// for XE_HILBERT_LEVEL = 6.
float2 XeGTAO_SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = gHilbertLUT.Load(int3(int2(pixCoord) % 64, 0)).x;
    index += 288u * (temporalIndex % 64u);
    // Golden-ratio-plane irrationals (Martin Roberts).
    return float2(frac(0.5 + float(index) * float2(0.75487766624669276005,
                                                   0.5698402909980532659114)));
}

// ==== Main pass: XeGTAO_MainPass ====
// Core math is 1:1 with the reference; only the inputs (linear depth pyramid + GBuffer
// world normal) differ.
[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 pixCoord = dtid.xy;
    if (int(pixCoord.x) >= ViewportSize.x || int(pixCoord.y) >= ViewportSize.y)
        return;

    float2 normalizedScreenPos = (float2(pixCoord) + 0.5.xx) * ViewportPixelSize;

    // Viewspace Z at the center: mip 0 of the linearized depth pyramid.
    float viewspaceZ = gLinearDepth.Load(int3(pixCoord, 0));

    // Edges from the 4 cardinal neighbours of the same linear-depth texture (as the
    // reference); coordinates clamp to the viewport.
    int2 pxL = int2(max(int(pixCoord.x) - 1, 0),                pixCoord.y);
    int2 pxR = int2(min(int(pixCoord.x) + 1, ViewportSize.x-1), pixCoord.y);
    int2 pxT = int2(pixCoord.x, max(int(pixCoord.y) - 1, 0));
    int2 pxB = int2(pixCoord.x, min(int(pixCoord.y) + 1, ViewportSize.y-1));
    float pixLZ = gLinearDepth.Load(int3(pxL, 0));
    float pixRZ = gLinearDepth.Load(int3(pxR, 0));
    float pixTZ = gLinearDepth.Load(int3(pxT, 0));
    float pixBZ = gLinearDepth.Load(int3(pxB, 0));

    float4 edgesLRTB = XeGTAO_CalculateEdges(viewspaceZ, pixLZ, pixRZ, pixTZ, pixBZ);
    gEdges[pixCoord] = XeGTAO_PackEdges(edgesLRTB);

    // Sky: linearization writes FLT_MAX for cleared depth; full visibility.
    if (viewspaceZ >= 1e20)
    {
        gOutput[pixCoord] = 1.0;
        return;
    }

    // Move center pixel slightly toward camera to avoid imprecision artifacts.
    viewspaceZ *= 0.99999;

    const float3 pixCenterPos = XeGTAO_ComputeViewspacePosition(normalizedScreenPos, viewspaceZ);
    const float3 viewVec      = normalize(-pixCenterPos);

    // View-space normal from the GBuffer world normal (rgb = world*0.5+0.5). ViewMatrix is
    // uploaded row-major and read column-major, so mul((float3x3)ViewMatrix, worldN) is the
    // world -> view rotation.
    // normal.a is an exact 16-bit integer on the UNORM RT1 (GBuffer.ps.hlsl): matIdx in bits
    // 0..14, bit 15 = MAT_FLAG_EXCLUDE_FROM_SSAO (material opted out, e.g. character skin):
    // full visibility, no horizon loop. Edges are already written, so the spatial denoise
    // keeps consistent neighbourhood info across the excluded region.
    float4 normalRaw    = gNormals.Load(int3(pixCoord, 0));
    if (((uint)(normalRaw.a * 65535.0 + 0.5) & 0x8000u) != 0u)
    {
        gOutput[pixCoord] = 1.0;
        return;
    }
    float3 worldN       = normalize(normalRaw.rgb * 2.0 - 1.0);
    float3 viewspaceNormal = normalize(mul((float3x3)ViewMatrix, worldN));

    // Effect parameters, verbatim.
    const float effectRadius             = EffectRadius * RadiusMultiplier;
    const float sampleDistributionPower  = SampleDistributionPower;
    const float thinOccluderCompensation = ThinOccluderCompensation;
    const float falloffRange             = EffectFalloffRange * effectRadius;
    const float falloffFrom              = effectRadius * (1.0 - EffectFalloffRange);
    const float falloffMul               = -1.0 / falloffRange;
    const float falloffAdd               = falloffFrom / falloffRange + 1.0;

    float visibility = 0;

    // Hilbert-driven R2 noise: a spatially decorrelated, temporally rotated pair in
    // [0,1)^2. With TAA integrating ~8 history frames it converges to smoother AO than plain
    // R2 with a per-frame offset.
    const float2 localNoise  = XeGTAO_SpatioTemporalNoise(pixCoord, uint(NoiseIndex));
    const float  noiseSlice  = localNoise.x;
    const float  noiseSample = localNoise.y;

    const float pixelTooCloseThreshold = 1.3;
    const float2 pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ.xx * NDCToViewMul_x_PixelSize;
    float screenspaceRadius = effectRadius / pixelDirRBViewspaceSizeAtCenterZ.x;

    visibility += saturate((10.0 - screenspaceRadius) / 100.0) * 0.5;

    const float minS = pixelTooCloseThreshold / screenspaceRadius;

    [loop] for (uint slice = 0; slice < SliceCount; slice++)
    {
        float sliceK = (float(slice) + noiseSlice) / float(SliceCount);
        float phi    = sliceK * XE_GTAO_PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        float2 omega = float2(cosPhi, -sinPhi);
        omega *= screenspaceRadius;

        const float3 directionVec      = float3(cosPhi, sinPhi, 0);
        const float3 orthoDirectionVec = directionVec - dot(directionVec, viewVec) * viewVec;
        const float3 axisVec           = normalize(cross(orthoDirectionVec, viewVec));
        float3 projectedNormalVec      = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);

        float signNorm                 = sign(dot(orthoDirectionVec, projectedNormalVec));
        float projectedNormalVecLength = length(projectedNormalVec);
        float cosNorm                  = saturate(dot(projectedNormalVec, viewVec) / projectedNormalVecLength);
        float n                        = signNorm * XeGTAO_FastACos(cosNorm);

        const float lowHorizonCos0 = cos(n + XE_GTAO_PI_HALF);
        const float lowHorizonCos1 = cos(n - XE_GTAO_PI_HALF);
        float horizonCos0          = lowHorizonCos0;
        float horizonCos1          = lowHorizonCos1;

        [loop] for (uint step = 0; step < StepsPerSlice; step++)
        {
            const float stepBaseNoise = float(slice + step * StepsPerSlice) * 0.6180339887498948482;
            float stepNoise = frac(noiseSample + stepBaseNoise);

            float s = (float(step) + stepNoise) / float(StepsPerSlice);
            s       = pow(s, sampleDistributionPower);
            s      += minS;

            float2 sampleOffset = s * omega;
            float  sampleOffsetLength = length(sampleOffset);

            // Pyramid mip selection, verbatim: larger offsets pick a coarser mip so the sample
            // footprint roughly matches a texel (no undersampling noise).
            float mipLevel = clamp(log2(sampleOffsetLength) - DepthMIPSamplingOffset,
                                   0.0, float(XE_GTAO_DEPTH_MIP_LEVELS));

            // Snap to the pixel centre (reference behaviour): precise sample direction, no
            // sub-texel bilinear blends on flat mips.
            sampleOffset = round(sampleOffset) * ViewportPixelSize;

            float2 sampleScreenPos0 = normalizedScreenPos + sampleOffset;
            float  SZ0              = gLinearDepth.SampleLevel(gLinear, sampleScreenPos0, mipLevel);
            float3 samplePos0       = XeGTAO_ComputeViewspacePosition(sampleScreenPos0, SZ0);

            float2 sampleScreenPos1 = normalizedScreenPos - sampleOffset;
            float  SZ1              = gLinearDepth.SampleLevel(gLinear, sampleScreenPos1, mipLevel);
            float3 samplePos1       = XeGTAO_ComputeViewspacePosition(sampleScreenPos1, SZ1);

            float3 sampleDelta0 = samplePos0 - pixCenterPos;
            float3 sampleDelta1 = samplePos1 - pixCenterPos;
            float  sampleDist0  = length(sampleDelta0);
            float  sampleDist1  = length(sampleDelta1);

            float3 sampleHorizonVec0 = sampleDelta0 / sampleDist0;
            float3 sampleHorizonVec1 = sampleDelta1 / sampleDist1;

            float falloffBase0 = length(float3(sampleDelta0.x, sampleDelta0.y,
                                               sampleDelta0.z * (1.0 + thinOccluderCompensation)));
            float falloffBase1 = length(float3(sampleDelta1.x, sampleDelta1.y,
                                               sampleDelta1.z * (1.0 + thinOccluderCompensation)));
            float weight0 = saturate(falloffBase0 * falloffMul + falloffAdd);
            float weight1 = saturate(falloffBase1 * falloffMul + falloffAdd);

            float shc0 = dot(sampleHorizonVec0, viewVec);
            float shc1 = dot(sampleHorizonVec1, viewVec);

            shc0 = lerp(lowHorizonCos0, shc0, weight0);
            shc1 = lerp(lowHorizonCos1, shc1, weight1);

            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }

        projectedNormalVecLength = lerp(projectedNormalVecLength, 1.0, 0.05);

        float h0 = -XeGTAO_FastACos(horizonCos1);
        float h1 =  XeGTAO_FastACos(horizonCos0);

        float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
        float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
        float localVisibility = projectedNormalVecLength * (iarc0 + iarc1);
        visibility += localVisibility;
    }

    visibility /= float(SliceCount);
    visibility  = pow(visibility, FinalValuePower);
    visibility  = max(0.03, visibility);

    gOutput[pixCoord] = visibility;
}
