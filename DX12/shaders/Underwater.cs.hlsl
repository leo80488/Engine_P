// Underwater screen distortion + tint, pre-tonemap on the HDR scene (like CAS).
// Two layered time-animated sine waves wobble the frame (fake refraction), then the
// colour is tinted toward the water colour. The Stack swaps ctx.hdrSrv to u0.
// No sampler: bilinear is done by hand from edge-clamped Loads, so the pass shares
// the plain post-process compute root signature (space2):
//   b0 - UnderwaterCB
//   t0 - HDR input  (R16G16B16A16_FLOAT)
//   u0 - HDR output (R16G16B16A16_FLOAT)

cbuffer UnderwaterCB : register(b0, space2)
{
    uint   width;
    uint   height;
    float  time;        // seconds (accumulated)
    float  strength;    // wobble amplitude in UV units (e.g. 0.01)

    float  scale;       // wave spatial frequency
    float  speed;       // wave temporal speed
    float  tintAmount;  // 0..1 blend toward the tinted colour
    float  _pad0;

    float3 tint;        // water colour multiply (e.g. 0.45, 0.75, 0.85)
    float  _pad1;
};

Texture2D<float4>   gInput  : register(t0, space2);
RWTexture2D<float4> gOutput : register(u0, space2);

float4 LoadClamped(int2 p, int2 maxPx)
{
    return gInput.Load(int3(clamp(p, int2(0, 0), maxPx), 0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height) return;

    const int2   maxPx = int2(int(width) - 1, int(height) - 1);
    const float2 dims  = float2(width, height);
    const float2 uv    = (float2(dtid.xy) + 0.5) / dims;

    // Two layered waves on perpendicular axes -> organic, non-repeating wobble.
    float2 offset;
    offset.x = sin(uv.y * scale + time * speed)            * strength;
    offset.y = cos(uv.x * scale * 1.3 + time * speed * 0.9) * strength;
    // A faster, finer ripple on top for surface detail.
    offset.x += sin(uv.y * scale * 2.7 + time * speed * 1.7) * strength * 0.35;
    offset.y += cos(uv.x * scale * 3.1 + time * speed * 1.3) * strength * 0.35;

    // Fractional source pixel -> manual bilinear (edge-clamped).
    const float2 srcPx = (uv + offset) * dims - 0.5;
    const int2   p0    = int2(floor(srcPx));
    const float2 f     = srcPx - float2(p0);

    const float4 c00 = LoadClamped(p0 + int2(0, 0), maxPx);
    const float4 c10 = LoadClamped(p0 + int2(1, 0), maxPx);
    const float4 c01 = LoadClamped(p0 + int2(0, 1), maxPx);
    const float4 c11 = LoadClamped(p0 + int2(1, 1), maxPx);

    float4 c = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);

    // Water tint: blend the colour toward (colour * tint).
    c.rgb = lerp(c.rgb, c.rgb * tint, saturate(tintAmount));

    gOutput[int2(dtid.xy)] = c;
}
