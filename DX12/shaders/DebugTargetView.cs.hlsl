// DebugTargetView.cs.hlsl - paint one render target over the main view's final
// output (DebugTargetViewPass; Renderer/DebugTargets.h).
//
// One thread per output pixel. The target is POINT-sampled at the output
// pixel's centre mapped into its own size, so a render-res target in a
// display-res view shows its texels, never a filtered blend of them.
//
// The output holds gamma-space values in every output mode (HDROutput.h: 1.0
// is paper white), so Raw / Channel write the stored value remapped from
// [RangeMin, RangeMax] as it is: an 8-bit capture of this output reads back
// value = RangeMin + pixel / 255 * (RangeMax - RangeMin).
//
// The colour maps are debug_visualize.hlsli's, shared with the render graph
// panel's preview.
//
// Compute root signature (space2):
//   b0 - DebugTargetCB
//   t0 - Texture2D<float4> the target (any float / unorm / depth SRV: a
//        one-channel format reads (r, 0, 0, 1)); the fallback while the
//        target is a uint one, read through SrcHandle instead
//   u0 - RWTexture2D<float4> the main view's final output

#include "bindless.hlsli"          // typed bindless handles
#include "debug_visualize.hlsli"

cbuffer DebugTargetCB : register(b0, space2)
{
    uint2 SrcSize;
    uint2 DstSize;
    uint  Mode;        // MODE_* below (Auto is resolved on the CPU)
    int   Channel;     // Channel / Heat / Categorical / Bits: 0..3
    float RangeMin;
    float RangeMax;
    float Exposure;    // Hdr
    float Scale;       // Velocity: NDC distance -> brightness; Categorical / Bits on a float target: value -> integer
    float NearZ;       // Depth
    float FarZ;
    uint  Orthographic;   // Depth: an orthographic camera's depth is linear
    uint  SrcUint;     // 0: the float target at t0; 1: a uint target (SrcHandle); 2: a stencil plane (SrcHandle, .y)
    uint  SrcHandle;   // the uint target's Tex2DUint handle
    uint  Mask;        // Categorical / Bits: the integer's bits that count
};

Texture2D<float4>   Src : register(t0, space2);
RWTexture2D<float4> Dst : register(u0, space2);

// DebugTargets::Mode
static const uint MODE_RAW         = 1;
static const uint MODE_CHANNEL     = 2;
static const uint MODE_HDR         = 3;
static const uint MODE_DEPTH       = 4;
static const uint MODE_VELOCITY    = 5;
static const uint MODE_SIGNED      = 6;
static const uint MODE_HEAT        = 7;
static const uint MODE_CATEGORICAL = 8;
static const uint MODE_BITS        = 9;
static const uint MODE_EV          = 10;
static const uint MODE_UNAVAILABLE = 15;   // the target is not there this frame

float Remap(float v)
{
    float d = RangeMax - RangeMin;
    if (abs(d) < 1e-30) d = 1e-30;
    return saturate((v - RangeMin) / d);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstSize.x || id.y >= DstSize.y) return;

    if (Mode == MODE_UNAVAILABLE || SrcSize.x == 0 || SrcSize.y == 0)
    {
        const uint check = ((id.x >> 4) ^ (id.y >> 4)) & 1u;
        Dst[id.xy] = float4(check ? float3(0.25, 0.25, 0.25) : float3(0.16, 0.16, 0.16), 1.0);
        return;
    }

    const uint2 sp = min(uint2((float2(id.xy) + 0.5) * float2(SrcSize) / float2(DstSize)), SrcSize - 1u);
    // A uint target: its integers, and as floats for the modes that remap a value.
    // A stencil plane's byte is the view's .y.
    float4 v;
    uint4  u = uint4(0u, 0u, 0u, 1u);
    [branch] if (SrcUint != 0u)
    {
        const uint2 r = BindlessTex2D<uint2>(SrcHandle).Load(int3(sp, 0));
        u = SrcUint == 2u ? uint4(r.y, 0u, 0u, 1u) : uint4(r.x, r.y, 0u, 1u);
        v = float4(u);
    }
    else
        v = Src.Load(int3(sp, 0));
    float3 o = float3(0.0, 0.0, 0.0);

    [branch] if (Mode == MODE_CHANNEL)
    {
        const float x = v[clamp(Channel, 0, 3)];
        o = DV_Bad(x) ? DV_kBad : Remap(x).xxx;
    }
    else if (Mode == MODE_HDR)
    {
        if (DV_Bad3(v.rgb)) o = DV_kBad;
        else
        {
            const float3 x = max(v.rgb, 0.0) * Exposure;
            o = pow(x / (1.0 + x), 1.0 / 2.2);
        }
    }
    else if (Mode == MODE_EV)
    {
        // Stops from middle grey (0.18) of the exposed luminance, on the heat ramp over [RangeMin, RangeMax].
        if (DV_Bad3(v.rgb)) o = DV_kBad;
        else
        {
            const float luma = dot(max(v.rgb, 0.0) * Exposure, float3(0.2126, 0.7152, 0.0722));
            o = DV_Heat(Remap(log2(max(luma, 1e-6) / 0.18)));
        }
    }
    else if (Mode == MODE_DEPTH)
    {
        o = DV_Depth(v.r, NearZ, FarZ, Orthographic != 0);
    }
    else if (Mode == MODE_VELOCITY)
    {
        const float2 mv = v.xy;
        if (DV_Bad(mv.x) || DV_Bad(mv.y)) o = DV_kBad;
        else
        {
            const float len = length(mv);
            const float h   = len > 0.0 ? atan2(mv.y, mv.x) * (0.5 / 3.14159265) + 0.5 : 0.0;
            o = DV_Hue(h) * saturate(len * Scale);
        }
    }
    else if (Mode == MODE_SIGNED)
    {
        o = DV_Bad3(v.rgb) ? DV_kBad : saturate(v.rgb * 0.5 + 0.5);
    }
    else if (Mode == MODE_HEAT)
    {
        const float x = v[clamp(Channel, 0, 3)];
        o = DV_Bad(x) ? DV_kBad : DV_Heat(Remap(x));
    }
    else if (Mode == MODE_CATEGORICAL || Mode == MODE_BITS)
    {
        // The integer: a uint target's own, a float target's value * Scale
        // (GBuffer1.a's matIdx is a UNORM16: Scale 65535), then Mask.
        const int   c = clamp(Channel, 0, 3);
        const float x = v[c];
        if (SrcUint == 0u && (DV_Bad(x) || x < 0.0)) o = DV_kBad;
        else
        {
            const uint n = (SrcUint != 0u ? u[c] : (uint)round(x * Scale)) & Mask;
            // Categorical: a value outside [RangeMin, RangeMax] is no category - black
            // (e.g. shadingModel's 255 where nothing was drawn).
            if (Mode == MODE_BITS) o = DV_StencilBits(n, id.xy);
            else o = ((float)n < RangeMin || (float)n > RangeMax) ? float3(0.0, 0.0, 0.0) : DV_Categorical(n);
        }
    }
    else   // MODE_RAW
    {
        o = DV_Bad3(v.rgb) ? DV_kBad : float3(Remap(v.r), Remap(v.g), Remap(v.b));
    }

    Dst[id.xy] = float4(o, 1.0);
}
