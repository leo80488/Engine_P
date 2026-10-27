// FXAA.cs.hlsl - NVIDIA FXAA 3.11 PC quality preset, HDR-aware compute port
// (reference: the FXAA 3.11 white paper / Fxaa3_11.h).
//   1. 5-tap luma (centre + NSWE).
//   2. Local-contrast early-out below qualityEdgeThreshold.
//   3. 4 corner samples (NW NE SW SE) for the second-derivative edge orientation.
//   4. Pick the orientation (horz / vert) and step toward the steeper side.
//   5. Walk the edge both ways (kStepQuality steps) to find its endpoints.
//   6. Pixel offset from the endpoint distances.
//   7. Subpixel AA: blend toward the 12-sample luma average.
//   8. Bilinear sample at the offset position.
//
// HDR: BT.601 luma (as the reference); the relative threshold handles HDR
// contrast, so no tonemap is needed.
//
// Root signature (compute space2, GraphicsDX12::CreateComputeRootSignature):
//   b0 space2 - FXAACB
//   t0 space2 - input HDR (raw scene or TAA's resolved output)
//   u0 space2 - output UAV (R16G16B16A16_FLOAT)

#include "core/color.hlsli"   // LuminanceRec601

cbuffer FXAACB : register(b0, space2)
{
    uint   width;
    uint   height;
    float  qualitySubpix;
    float  qualityEdgeThreshold;
    float  qualityEdgeThresholdMin;
    float3 _pad0;
};

Texture2D<float4>   gInput  : register(t0, space2);
RWTexture2D<float4> gOutput : register(u0, space2);
SamplerState        gLinear : register(s0, space2);


// Edge-walk step multipliers after the first two one-texel steps (shaped like the
// tail of NVIDIA's FXAA 3.11 quality-preset step tables).
static const float kStepQuality[8] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0 };

[numthreads(8, 8, 1)]
void CSMain(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    float2 invSize = 1.0 / float2(width, height);
    float2 uv      = (float2(id) + 0.5) * invSize;

    // ---- 5-tap centre + cardinals ----
    // The centre's alpha is TAA's previous-frame luma for AutoExposure: forward it
    // unchanged so FXAA+TAA keeps exposure tracking correct.
    float4 sampleM = gInput.SampleLevel(gLinear, uv, 0);
    float3 rgbM    = sampleM.rgb;
    float  alphaM  = sampleM.a;
    float lumaM = LuminanceRec601(rgbM);

    float lumaN = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2( 0.0,        -invSize.y), 0).rgb);
    float lumaS = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2( 0.0,         invSize.y), 0).rgb);
    float lumaE = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2( invSize.x,   0.0      ), 0).rgb);
    float lumaW = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2(-invSize.x,   0.0      ), 0).rgb);

    // ---- Local-contrast early-out ----
    float lumaMin   = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax   = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(qualityEdgeThresholdMin, lumaMax * qualityEdgeThreshold))
    {
        gOutput[id] = float4(rgbM, alphaM);
        return;
    }

    // ---- 4 corner samples for the edge-orientation discriminator ----
    float lumaNW = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2(-invSize.x, -invSize.y), 0).rgb);
    float lumaNE = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2( invSize.x, -invSize.y), 0).rgb);
    float lumaSW = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2(-invSize.x,  invSize.y), 0).rgb);
    float lumaSE = LuminanceRec601(gInput.SampleLevel(gLinear, uv + float2( invSize.x,  invSize.y), 0).rgb);

    // Sums shared by the edge-detection and subpixel-AA stages.
    float lumaDownUp        = lumaN + lumaS;
    float lumaLeftRight     = lumaW + lumaE;
    float lumaLeftCorners   = lumaNW + lumaSW;
    float lumaRightCorners  = lumaNE + lumaSE;
    float lumaTopCorners    = lumaNW + lumaNE;
    float lumaBottomCorners = lumaSW + lumaSE;

    // Second derivatives, per axis.
    float edgeHorz = abs((-2.0 * lumaW) + lumaLeftCorners)
                   + abs((-2.0 * lumaM) + lumaDownUp     ) * 2.0
                   + abs((-2.0 * lumaE) + lumaRightCorners);
    float edgeVert = abs((-2.0 * lumaN) + lumaTopCorners)
                   + abs((-2.0 * lumaM) + lumaLeftRight  ) * 2.0
                   + abs((-2.0 * lumaS) + lumaBottomCorners);

    bool isHorzEdge = (edgeHorz >= edgeVert);

    // ---- Step direction (toward the steepest gradient) ----
    float luma1 = isHorzEdge ? lumaN : lumaW;
    float luma2 = isHorzEdge ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool  is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength   = isHorzEdge ? -invSize.y : -invSize.x;
    float lumaLocalAvg = 0.0;
    if (is1Steepest)
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    else
    {
        stepLength   = -stepLength;
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    // Shift the centre half a pixel toward the steeper neighbour.
    float2 currentUV = uv;
    if (isHorzEdge) currentUV.y += stepLength * 0.5;
    else            currentUV.x += stepLength * 0.5;

    // ---- Walk both directions along the edge ----
    float2 offset = isHorzEdge ? float2(invSize.x, 0.0) : float2(0.0, invSize.y);
    float2 uv1 = currentUV - offset;
    float2 uv2 = currentUV + offset;

    float lumaEnd1 = LuminanceRec601(gInput.SampleLevel(gLinear, uv1, 0).rgb) - lumaLocalAvg;
    float lumaEnd2 = LuminanceRec601(gInput.SampleLevel(gLinear, uv2, 0).rgb) - lumaLocalAvg;

    bool reached1   = abs(lumaEnd1) >= gradientScaled;
    bool reached2   = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        if (reachedBoth) break;

        if (!reached1) lumaEnd1 = LuminanceRec601(gInput.SampleLevel(gLinear, uv1, 0).rgb) - lumaLocalAvg;
        if (!reached2) lumaEnd2 = LuminanceRec601(gInput.SampleLevel(gLinear, uv2, 0).rgb) - lumaLocalAvg;

        reached1 = abs(lumaEnd1) >= gradientScaled;
        reached2 = abs(lumaEnd2) >= gradientScaled;
        reachedBoth = reached1 && reached2;

        if (!reached1) uv1 -= offset * kStepQuality[i];
        if (!reached2) uv2 += offset * kStepQuality[i];
    }

    // ---- Pixel offset from the end-distance asymmetry ----
    float distance1 = isHorzEdge ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorzEdge ? (uv2.x - uv.x) : (uv2.y - uv.y);

    bool  isDir1 = distance1 < distance2;
    float distanceFinal  = min(distance1, distance2);
    float edgeThickness  = distance1 + distance2;
    float pixelOffset    = -distanceFinal / max(edgeThickness, 1e-5) + 0.5;

    // The centre must be on the correct side of the edge: a sign mismatch means
    // the edge ended in the direction of the centre's own luma deviation, so no
    // offset applies.
    bool isCentreSmaller   = lumaM < lumaLocalAvg;
    bool correctVariation  = ((isDir1 ? lumaEnd1 : lumaEnd2) < 0.0) != isCentreSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // ---- Subpixel anti-aliasing ----
    // Weighted average of 12 samples (cardinals x2 + corners) as the sub-pixel luma
    // reference. The further the centre deviates from it, the more likely it IS a
    // sub-pixel feature, which raises the offset.
    float lumaAvg = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight)
                                    + lumaLeftCorners + lumaRightCorners);
    float subPixOffset1 = saturate(abs(lumaAvg - lumaM) / max(lumaRange, 1e-5));
    float subPixOffset2 = (-2.0 * subPixOffset1 + 3.0) * subPixOffset1 * subPixOffset1;
    float subPixOffsetFinal = subPixOffset2 * subPixOffset2 * qualitySubpix;

    finalOffset = max(finalOffset, subPixOffsetFinal);

    // ---- Final bilinear sample at the offset position ----
    float2 finalUV = uv;
    if (isHorzEdge) finalUV.y += finalOffset * stepLength;
    else            finalUV.x += finalOffset * stepLength;

    float3 finalRGB = gInput.SampleLevel(gLinear, finalUV, 0).rgb;
    gOutput[id] = float4(finalRGB, alphaM);  // forward TAA's prev-luma alpha
}
