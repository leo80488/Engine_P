// NPR stylization stack (HDR, pre-tonemap). Independently toggled steps in classic
// NPR order, in one pass so they compose without extra textures:
//   Pixelate      - snap the working coordinate to a block grid (pixelateOn)
//   1. Kuwahara   - painterly edge-preserving smoothing (lowest-variance quadrant),
//                   then the CartoonStreet outline / highlight (streetEnabled)
//   2. Posterize  - quantize each channel to N levels
//   3. Dither     - 4x4 Bayer ordered dithering to N levels
//   4. Halftone   - luminance-driven screen-tone dots (rotated screen)
//   5. Crosshatch - multi-angle ink hatching by luminance (B&W sketch)
// Kuwahara works on the raw HDR colour; steps 2-5 work on saturated 0..1 colour,
// and their result is written when any of them is on.
// Compute root signature (space2):
//   b0    - StylizeCB
//   t0    - HDR colour (R16G16B16A16_FLOAT)
//   t1-t3 - street outline depth, normal, materials
//   u0    - HDR output

cbuffer StylizeCB : register(b0, space2)
{
    uint width;
    uint height;
    uint kuwaharaOn;
    uint posterizeOn;

    uint halftoneOn;
    uint ditherOn;
    uint crosshatchOn;
    uint kuwaharaRadius;

    float posterizeLevels;
    float halftoneCell;
    float halftoneAngle;
    float ditherLevels;

    float crosshatchDensity;
    float crosshatchThickness;
    uint  pixelateOn;
    uint  pixelSize;     // block size in pixels
    uint streetEnabled;
    float streetDepthWidth, streetNormalWidth, streetDepthExponent;
    float streetNormalExponent, streetDistancePower, streetDepthMax, streetHighlightU;
    float streetHighlightV, streetNear, streetFar;
    uint streetMaterialCount;
    float3 streetOutlineColor; float streetPad0;
    float3 streetHighlightColor; float streetPad1;
};

Texture2D<float4>   gColor  : register(t0, space2);
RWTexture2D<float4> gOutput : register(u0, space2);
#include "material.hlsli"
Texture2D<float> gStreetDepth : register(t1, space2);
Texture2D<float4> gStreetNormal : register(t2, space2);
StructuredBuffer<MaterialGPUData> gStreetMaterials : register(t3, space2);
#include "SourceOutline.hlsli"

#include "core/color.hlsli"   // Luminance

// ==== 1. Kuwahara ====
float3 Kuwahara(int2 px, int2 maxPx, int R)
{
    int2 qoff[4] = { int2(-R, -R), int2(0, -R), int2(-R, 0), int2(0, 0) };
    float3 bestMean = gColor.Load(int3(px, 0)).rgb;
    float  bestVar  = 1e20;

    [loop]
    for (int q = 0; q < 4; ++q)
    {
        float3 sum  = float3(0, 0, 0);
        float  sumL = 0.0;
        float  sumL2 = 0.0;
        int    cnt = 0;
        for (int dy = 0; dy <= R; ++dy)
        for (int dx = 0; dx <= R; ++dx)
        {
            int2   s = clamp(px + qoff[q] + int2(dx, dy), int2(0, 0), maxPx);
            float3 c = gColor.Load(int3(s, 0)).rgb;
            float  l = Luminance(c);
            sum += c; sumL += l; sumL2 += l * l; cnt++;
        }
        float  inv  = 1.0 / max(float(cnt), 1.0);
        float3 mean = sum * inv;
        float  var  = sumL2 * inv - (sumL * inv) * (sumL * inv);
        if (var < bestVar) { bestVar = var; bestMean = mean; }
    }
    return bestMean;
}

// ==== 2. Posterize ====
float3 Posterize(float3 c, float levels)
{
    float n = max(levels, 2.0);
    return floor(c * (n - 1.0) + 0.5) / (n - 1.0);
}

// ==== 3. Dither (4x4 Bayer ordered) ====
float Bayer4(int2 p)
{
    const float m[16] = {
         0.0 / 16.0,  8.0 / 16.0,  2.0 / 16.0, 10.0 / 16.0,
        12.0 / 16.0,  4.0 / 16.0, 14.0 / 16.0,  6.0 / 16.0,
         3.0 / 16.0, 11.0 / 16.0,  1.0 / 16.0,  9.0 / 16.0,
        15.0 / 16.0,  7.0 / 16.0, 13.0 / 16.0,  5.0 / 16.0
    };
    return m[(p.y & 3) * 4 + (p.x & 3)];
}
float3 Dither(float3 c, float levels, int2 p)
{
    float n = max(levels, 2.0);
    float t = Bayer4(p);
    return floor(c * (n - 1.0) + t) / (n - 1.0);
}

// ==== 4. Halftone (rotated screen dots) ====
float3 Halftone(float3 c, float2 pixel, float cell, float angleDeg)
{
    float  L  = Luminance(c);
    float  a  = radians(angleDeg);
    float  ca = cos(a), sa = sin(a);
    float2 rp = float2(pixel.x * ca - pixel.y * sa, pixel.x * sa + pixel.y * ca);
    float2 cp = (frac(rp / cell) - 0.5) * cell;            // px offset from cell centre
    float  dist     = length(cp);
    float  radius   = (1.0 - L) * cell * 0.5 * 1.5;        // darker -> bigger dot
    float  coverage = 1.0 - smoothstep(radius - 0.7, radius + 0.7, dist);
    return lerp(float3(1, 1, 1), c, coverage);            // paper white, ink = colour
}

// ==== 5. Crosshatch (luminance ink hatching) ====
float HatchLine(float2 pixel, float angleDeg, float freq, float thickness)
{
    float a     = radians(angleDeg);
    float coord = pixel.x * cos(a) + pixel.y * sin(a);
    float v     = abs(frac(coord * freq) - 0.5) * 2.0;     // 0 at line centre, 1 between
    return smoothstep(thickness, thickness + 0.15, v);     // 0 = ink, 1 = paper
}
float3 Crosshatch(float3 c, float2 pixel, float density, float thickness)
{
    float L = Luminance(c);
    float h = 1.0;
    if (L < 0.85) h = min(h, HatchLine(pixel,   0.0, density, thickness));
    if (L < 0.65) h = min(h, HatchLine(pixel,  90.0, density, thickness));
    if (L < 0.45) h = min(h, HatchLine(pixel,  45.0, density, thickness));
    if (L < 0.25) h = min(h, HatchLine(pixel, 135.0, density, thickness));
    return float3(h, h, h);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    const int2 maxPx = int2(int(width) - 1, int(height) - 1);

    // Pixelation snaps the working coordinate to the block centre, so the whole
    // chain below reads and patterns per block and every pixel of a block ends up
    // identical (blocky output).
    int2   workPx    = int2(id.xy);
    float2 workPixel = float2(id.xy);
    if (pixelateOn != 0)
    {
        int bs = max(int(pixelSize), 1);
        workPx    = clamp((workPx / bs) * bs + (bs / 2), int2(0, 0), maxPx);
        workPixel = float2(workPx);
    }
    const int2   px    = workPx;
    const float2 pixel = workPixel;

    const float4 src   = gColor.Load(int3(px, 0));
    float3 c = (kuwaharaOn != 0) ? Kuwahara(px, maxPx, int(kuwaharaRadius)) : src.rgb;
    if (streetEnabled != 0) {
        float2 uv = (float2(px)+0.5)/float2(width,height);
        c = SourceHighlight(uv, SourceOutline(uv,c));
    }

    const bool tonal = (posterizeOn | ditherOn | halftoneOn | crosshatchOn) != 0;

    float3 s = saturate(c);
    if (posterizeOn  != 0) s = Posterize(s, posterizeLevels);
    if (ditherOn     != 0) s = Dither(s, ditherLevels, px);
    if (halftoneOn   != 0) s = Halftone(s, pixel, halftoneCell, halftoneAngle);
    if (crosshatchOn != 0) s = Crosshatch(s, pixel, crosshatchDensity, crosshatchThickness);

    // Write this thread's own pixel (when pixelating, the reads came from the block
    // centre, so every pixel of a block gets the same value).
    gOutput[int2(id.xy)] = float4(tonal ? s : c, src.a);
}
