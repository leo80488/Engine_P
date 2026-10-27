// AMD FidelityFX Contrast Adaptive Sharpening (ffx_cas.h, CAS 1.0), adapted to HDR.
// Runs post-TAA / pre-tonemap on the HDR scene. Each 8x8 group loads a 10x10 halo tile into
// LDS once (100 texels instead of 9 loads x 64 threads = 576).
// Port notes: no LINEAR sample-halving path (the tile already fits LDS); the amp keeps AMD's
// headroom formula and clamps it so bright HDR pixels (lumaMax > 2) get no sharpening.
// Compute root signature (space2):
//   b0 CASCB { uint width, height; float sharpness; float _pad; }
//   t0 HDR input (R16G16B16A16_FLOAT), u0 HDR output (R16G16B16A16_FLOAT)

#include "core/color.hlsli"   // Luminance

cbuffer CASCB : register(b0, space2)
{
    uint  width;
    uint  height;
    float sharpness;   // [0..1]: 0 = subtle (AMD's minimum), 1 = maximum
    float _pad;
};

Texture2D<float4>   gInput  : register(t0, space2);
RWTexture2D<float4> gOutput : register(u0, space2);

// ---- LDS tile: 8x8 group + 1-pixel halo = 10x10 float4 = 1600 bytes ----
#define TILE       8
#define HALO       1
#define TILE_WH    (TILE + 2 * HALO)       // 10
#define TILE_AREA  (TILE_WH * TILE_WH)     // 100

groupshared float4 g_tile[TILE_WH][TILE_WH];

[numthreads(TILE, TILE, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID,
            uint3 gtid : SV_GroupThreadID,
            uint3 gid  : SV_GroupID)
{
    const int2 maxPx = int2(int(width) - 1, int(height) - 1);

    // ---- Cooperative load: 64 threads fill the 100 slots in up to 2 iterations ----
    // Iteration 0 covers slots 0..63, iteration 1 slots 64..99 (first 36 threads). Taps are
    // clamped to the viewport edge so the border never reads uninitialised texels.
    const int2 groupOrigin = int2(gid.xy) * TILE - HALO;
    const uint linearIdx   = gtid.y * TILE + gtid.x;   // 0..63

    {
        const uint slot = linearIdx;                   // 0..63
        const uint ty   = slot / TILE_WH;
        const uint tx   = slot % TILE_WH;
        const int2 srcPx = clamp(groupOrigin + int2(tx, ty), int2(0, 0), maxPx);
        g_tile[ty][tx]   = gInput.Load(int3(srcPx, 0));
    }
    if (linearIdx < (TILE_AREA - TILE * TILE))         // 100 - 64 = 36
    {
        const uint slot = linearIdx + TILE * TILE;     // 64..99
        const uint ty   = slot / TILE_WH;
        const uint tx   = slot % TILE_WH;
        const int2 srcPx = clamp(groupOrigin + int2(tx, ty), int2(0, 0), maxPx);
        g_tile[ty][tx]   = gInput.Load(int3(srcPx, 0));
    }

    GroupMemoryBarrierWithGroupSync();

    // Out-of-viewport threads return only after the cooperative load: their LDS slots are
    // needed by in-bounds neighbours.
    if (dtid.x >= width || dtid.y >= height) return;

    // ---- 3x3 neighbourhood from LDS ----
    //   a b c
    //   d e f   (e = centre)
    //   g h i
    const int lx = int(gtid.x) + HALO;
    const int ly = int(gtid.y) + HALO;

    const float4 a = g_tile[ly - 1][lx - 1];
    const float4 b = g_tile[ly - 1][lx    ];
    const float4 c = g_tile[ly - 1][lx + 1];
    const float4 d = g_tile[ly    ][lx - 1];
    const float4 e = g_tile[ly    ][lx    ];
    const float4 f = g_tile[ly    ][lx + 1];
    const float4 g = g_tile[ly + 1][lx - 1];
    const float4 h = g_tile[ly + 1][lx    ];
    const float4 i = g_tile[ly + 1][lx + 1];

    // Full 3x3 per-channel min/max; corners included for the range.
    const float3 minRGB = min(min(min(a.rgb, b.rgb), min(c.rgb, d.rgb)),
                              min(min(e.rgb, f.rgb), min(min(g.rgb, h.rgb), i.rgb)));
    const float3 maxRGB = max(max(max(a.rgb, b.rgb), max(c.rgb, d.rgb)),
                              max(max(e.rgb, f.rgb), max(max(g.rgb, h.rgb), i.rgb)));

    // ---- Amp: AMD headroom formula, HDR-safe ----
    // AMD's LDR amp = saturate(min(lumaMin, 2 - lumaMax) / lumaMax), the headroom before clipping
    // at either end. HDR lumaMax > 2 makes the headroom negative; it is clamped to 0, so bright
    // emissives and fireflies get no sharpening instead of being amplified.
    const float  lumaMin = Luminance(minRGB);
    const float  lumaMax = Luminance(maxRGB);

    const float headroom = max(0.0, 2.0 - lumaMax);
    const float rcp      = 1.0 / max(lumaMax, 1e-4);
    float amp = saturate(min(lumaMin, headroom) * rcp);
    amp = sqrt(amp);   // AMD's perceptual curve

    // Peak weight goes from -1/8 (sharpness 0) to -1/5 (sharpness 1); negative because the cross
    // filter subtracts the neighbours.
    const float peak = -1.0 / lerp(8.0, 5.0, sharpness);
    const float w    = amp * peak;
    const float rcpW = 1.0 / (1.0 + 4.0 * w);

    // 5-tap cross sharpen as the centre plus a correction; alpha passes through from the centre.
    //   (b+d+f+h) * w/(1+4w) + e/(1+4w)  ==  e + (b+d+f+h - 4e) * w/(1+4w)
    // Keep the right-hand form: the left one adds two large cancelling terms (~-1.8e + 2.8e at
    // sharpness 0.6), so on a flat patch the fp16 store gave e or e - 1 ulp depending on fp32
    // last bits that vary between identical frames, flipping the whole sky by 1 LSB. Here a flat
    // patch's correction is exactly 0 and the output is exactly e.
    const float3 sharpened = e.rgb
                           + (b.rgb + d.rgb + f.rgb + h.rgb - 4.0 * e.rgb) * (w * rcpW);

    gOutput[int2(dtid.xy)] = float4(max(sharpened, 0.0), e.a);
}
