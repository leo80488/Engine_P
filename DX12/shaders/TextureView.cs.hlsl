// The Texture Viewer's picture (TextureViewPass).
// One thread per output pixel of a panel-owned RGBA16F target that ImGui draws with ImGui::Image.
// The output holds GAMMA-SPACE display values (the composition space, HDROutput.h: 1.0 = paper
// white), like DebugTargetView.
//
// The source is read BINDLESS through the texture's own default SRV: ResourceDescriptorHeap[SrcIndex]
// typed by SrcKind, which the CPU derives from the texture's desc with the predicate CreateTexture
// used to build that view. Reading a descriptor through another view type is silent UB under
// Resource Binding Tier 3, so the kinds never mix:
//   2D      Texture2D<float4>       Load(texel, mip)         (exact)
//   2DArray Texture2DArray<float4>  Load(texel, slice, mip)  (exact)
//   3D      Texture3D<float4>       Load(texel, slice, mip)  (exact)
//   Cube    TextureCube<float4>     SampleLevel(dir, mip) at the texel CENTRE (a linear sampler
//           there returns that texel), so a single face, the cross and the panorama need no
//           extra view.
// The compute root signature is CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
// (GraphicsDX12::CreateComputeRootSignature). Its space2 bindings:
//   b0 - TextureViewCB (a slot of the pass's constant ring)
//   u0 - RWTexture2D<float4> the view's output              (CSMain)
//   u1 - RWStructuredBuffer<float4> the probe results        (ProbeCS)
//   s0 - the static linear-clamp sampler
//
// ProbeCS: ONE thread reads ONE texel of the shown mip / slice (the value the shaders see, hardware
// BC decode included) into ProbeOut[ProbeIndex]; the pass copies it to a readback slot read
// kFramesInFlight frames later. A probe of picture B is written with B in A's fields.
//
// COMPARE: a second source B, read like A through ITS default SRV, drawn with A's mapping, channels
// and uv (each samples its own texels, so two sizes line up). Split: B right of SplitX; Diff:
// |A - B| x DiffScale of the mapped values. A wave can straddle the split, so the heap index is
// NonUniformResourceIndex.
//
// Keep this file pure ASCII: offline dxc here reads sources as CP932.

cbuffer TextureViewCB : register(b0, space2)
{
    uint2  DstSize;       // the output's size in pixels
    uint2  MipSize;       // the shown mip's size in texels
    float2 UvOrigin;      // uv of the output's pixel (0,0) CORNER
    float2 UvPerPixel;    // uv step per output pixel
    uint   SrcIndex;      // heap index of the source's default SRV (0xFFFFFFFF = none)
    uint   SrcKind;       // KIND_* below
    uint   Mip;
    uint   Slice;         // array slice / 3D slice / cube face (face layout)
    uint   ChannelMask;   // bit0 R, bit1 G, bit2 B, bit3 A
    uint   MapMode;       // MAP_* below
    uint   Background;    // BG_* below
    uint   Flags;         // FLAG_* below
    float  Exposure;      // HDR: 2^EV
    float  RangeMin;      // Range
    float  RangeMax;
    uint   CubeLayout;    // LAYOUT_* below
    float4 BgColor;       // Solid backgrounds
    uint2  ProbeTexel;    // ProbeCS: the texel of the shown mip
    uint   ProbeIndex;    // ProbeCS: where in ProbeOut
    uint   ProbePad;
    uint2  MipSizeB;      // compare: B's shown mip, in texels
    uint   SrcIndexB;     // compare: B's default SRV (0xFFFFFFFF = none)
    uint   SrcKindB;
    uint   MipB;
    uint   SliceB;
    uint   FlagsB;        // compare: B's own FLAG_SRGB / FLAG_ONECHANNEL (the rest are Flags')
    uint   CompareMode;   // CMP_* below
    float  SplitX;        // Split: B from this fraction of DstSize.x on
    float  DiffScale;     // Diff: |A - B| x DiffScale
    float2 ComparePad;
};

RWTexture2D<float4>        Dst      : register(u0, space2);
RWStructuredBuffer<float4> ProbeOut : register(u1, space2);
SamplerState        Linear  : register(s0, space2);

static const uint KIND_2D      = 0;
static const uint KIND_2DARRAY = 1;
static const uint KIND_CUBE    = 2;
static const uint KIND_3D      = 3;

static const uint MAP_STORED = 0;   // as stored: an sRGB texture re-encoded to its stored bytes
static const uint MAP_LINEAR = 1;   // the decoded (linear) value the shaders read
static const uint MAP_HDR    = 2;   // exposure + optional tonemap, then gamma 2.2
static const uint MAP_NORMAL = 3;   // tangent-space normal from RG, Z rebuilt
static const uint MAP_RANGE  = 4;   // [RangeMin, RangeMax] -> [0, 1]

static const uint BG_CHECKER = 0;
static const uint BG_SOLID   = 1;

static const uint FLAG_SRGB       = 1u << 0;   // the source format is sRGB
static const uint FLAG_TILE       = 1u << 1;   // repeat the image (3x3 preview)
static const uint FLAG_FILTERED   = 1u << 2;   // bilinear inside the mip instead of point
static const uint FLAG_ONECHANNEL = 1u << 3;   // R-only format: grey from R
static const uint FLAG_TONEMAP    = 1u << 4;   // HDR: x / (1 + x) before the gamma
static const uint FLAG_NOSOURCE   = 1u << 5;   // nothing to show: the unavailable pattern

static const uint LAYOUT_FACE  = 0;
static const uint LAYOUT_CROSS = 1;
static const uint LAYOUT_PANO  = 2;

static const uint CMP_OFF   = 0;
static const uint CMP_SPLIT = 1;
static const uint CMP_DIFF  = 2;

static const float3 kBad = float3(1.0, 0.0, 1.0);   // NaN / Inf: magenta
#include "core/math.hlsli"   // PI

bool Bad(float v) { return isnan(v) || isinf(v); }

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    // step(c, k) = 1 where c <= k: the linear segment.
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}

// D3D cube face (0..5 = +X -X +Y -Y +Z -Z) and its uv (v down) -> direction.
float3 FaceDir(uint face, float2 uv)
{
    const float s = uv.x * 2.0 - 1.0;
    const float t = uv.y * 2.0 - 1.0;
    switch (face)
    {
    case 0:  return float3( 1.0, -t, -s);
    case 1:  return float3(-1.0, -t,  s);
    case 2:  return float3( s,  1.0,  t);
    case 3:  return float3( s, -1.0, -t);
    case 4:  return float3( s, -t,  1.0);
    default: return float3(-s, -t, -1.0);
    }
}

float3 Checker(uint2 px)
{
    const uint c = ((px.x >> 3) ^ (px.y >> 3)) & 1u;
    return c ? float3(0.24, 0.24, 0.24) : float3(0.16, 0.16, 0.16);
}

// One picture's source: A (the view's texture) or B (compare).
struct Src
{
    uint  index;   // heap index of its default SRV
    uint  kind;    // KIND_*
    uint  mip;
    uint  slice;   // array slice / 3D slice / cube face (face layout)
    uint2 size;    // the shown mip, in texels
    uint  flags;   // FLAG_*: the view's, with this source's own SRGB / ONECHANNEL
};

Src SrcA()
{
    Src s;
    s.index = SrcIndex;
    s.kind  = SrcKind;
    s.mip   = Mip;
    s.slice = Slice;
    s.size  = MipSize;
    s.flags = Flags;
    return s;
}

Src SrcB()
{
    Src s;
    s.index = SrcIndexB;
    s.kind  = SrcKindB;
    s.mip   = MipB;
    s.slice = SliceB;
    s.size  = MipSizeB;
    s.flags = (Flags & ~(FLAG_SRGB | FLAG_ONECHANNEL)) | FlagsB;
    return s;
}

// The texel of the shown mip nearest to uv (uv in [0,1)).
int2 TexelOf(float2 uv, uint2 size)
{
    return clamp(int2(floor(uv * float2(size))), int2(0, 0), int2(size) - 1);
}

// The texel CENTRE of the shown mip nearest to uv, as a uv again.
float2 SnapToCentre(float2 uv, uint2 size)
{
    return (float2(TexelOf(uv, size)) + 0.5) / float2(size);
}

float4 Fetch2D(Src s, float2 uv)
{
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(s.index)];
    if (s.flags & FLAG_FILTERED) return t.SampleLevel(Linear, uv, float(s.mip));
    return t.Load(int3(TexelOf(uv, s.size), int(s.mip)));
}

float4 Fetch2DArray(Src s, float2 uv)
{
    Texture2DArray<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(s.index)];
    if (s.flags & FLAG_FILTERED) return t.SampleLevel(Linear, float3(uv, float(s.slice)), float(s.mip));
    return t.Load(int4(TexelOf(uv, s.size), int(s.slice), int(s.mip)));
}

float4 Fetch3D(Src s, float2 uv)
{
    Texture3D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(s.index)];
    return t.Load(int4(TexelOf(uv, s.size), int(s.slice), int(s.mip)));
}

float4 FetchCube(Src s, float3 dir)
{
    TextureCube<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(s.index)];
    return t.SampleLevel(Linear, dir, float(s.mip));
}

// Where output pixel px lands in source s: false = outside the image (background).
bool Sample(Src s, uint2 px, out float4 v)
{
    v = float4(0.0, 0.0, 0.0, 0.0);
    float2 uv = UvOrigin + (float2(px) + 0.5) * UvPerPixel;
    const bool filtered = (s.flags & FLAG_FILTERED) != 0;

    if (s.kind == KIND_CUBE)
    {
        if (CubeLayout == LAYOUT_CROSS)
        {
            // Horizontal cross, 4 x 3 cells over uv [0,1]^2:
            //        +Y
            //    -X  +Z  +X  -Z
            //        -Y
            const float2 cell = uv * float2(4.0, 3.0);
            if (any(cell < 0.0) || cell.x >= 4.0 || cell.y >= 3.0) return false;
            const int2 c = int2(floor(cell));
            int face = -1;
            if (c.y == 0 && c.x == 1) face = 2;
            else if (c.y == 2 && c.x == 1) face = 3;
            else if (c.y == 1) face = (c.x == 0) ? 1 : (c.x == 1) ? 4 : (c.x == 2) ? 0 : 5;
            if (face < 0) return false;
            const float2 fuv = frac(cell);
            v = FetchCube(s, FaceDir(uint(face), filtered ? fuv : SnapToCentre(fuv, s.size)));
            return true;
        }
        if (CubeLayout == LAYOUT_PANO)
        {
            if (any(uv < 0.0) || any(uv >= 1.0)) return false;
            // Longitude along x (-pi..pi, +Z at the centre), latitude along y (+Y up).
            const float phi   = (uv.x * 2.0 - 1.0) * PI;
            const float theta = (0.5 - uv.y) * PI;
            v = FetchCube(s, float3(sin(phi) * cos(theta), sin(theta), cos(phi) * cos(theta)));
            return true;
        }
        if (s.flags & FLAG_TILE) uv = frac(uv);
        if (any(uv < 0.0) || any(uv >= 1.0)) return false;
        v = FetchCube(s, FaceDir(s.slice, filtered ? uv : SnapToCentre(uv, s.size)));
        return true;
    }

    if (s.flags & FLAG_TILE) uv = frac(uv);
    if (any(uv < 0.0) || any(uv >= 1.0)) return false;
    if (s.kind == KIND_2DARRAY)  v = Fetch2DArray(s, uv);
    else if (s.kind == KIND_3D)  v = Fetch3D(s, uv);
    else                         v = Fetch2D(s, uv);
    return true;
}

// NaN / Inf in any shown channel.
bool BadShown(float4 v)
{
    return ((ChannelMask & 1u) != 0 && Bad(v.r)) || ((ChannelMask & 2u) != 0 && Bad(v.g)) ||
           ((ChannelMask & 4u) != 0 && Bad(v.b)) || ((ChannelMask & 8u) != 0 && Bad(v.a));
}

// A read value -> its display value (MAP_*); alpha stays the value's.
float4 MapValue(float4 v, uint flags)
{
    float4 m = v;
    if (MapMode == MAP_STORED)
    {
        if (flags & FLAG_SRGB) m.rgb = LinearToSrgb(v.rgb);
    }
    else if (MapMode == MAP_HDR)
    {
        float3 x = max(v.rgb, 0.0) * Exposure;
        if (Flags & FLAG_TONEMAP) x = x / (1.0 + x);
        m.rgb = pow(saturate(x), 1.0 / 2.2);
    }
    else if (MapMode == MAP_NORMAL)
    {
        const float2 xy = v.rg * 2.0 - 1.0;
        const float  z  = sqrt(saturate(1.0 - dot(xy, xy)));
        m.rgb = float3(xy, z) * 0.5 + 0.5;
    }
    else if (MapMode == MAP_RANGE)
    {
        float d = RangeMax - RangeMin;
        if (abs(d) < 1e-30) d = 1e-30;
        m = saturate((v - RangeMin) / d);
    }
    // MAP_LINEAR: the value as the shaders read it.
    return m;
}

// The shown channels of a mapped value. One alone: grey (UE's convention);
// alpha with colour: coverage over the background - unless diff, where there
// is nothing for alpha to cover (a difference shows its colour).
float3 Compose(float4 m, float3 bg, uint flags, bool diff)
{
    const bool r = (ChannelMask & 1u) != 0, g = (ChannelMask & 2u) != 0;
    const bool b = (ChannelMask & 4u) != 0, a = (ChannelMask & 8u) != 0;
    const uint shown = (r ? 1u : 0u) + (g ? 1u : 0u) + (b ? 1u : 0u) + (a ? 1u : 0u);
    if (shown == 0)
        return diff ? float3(0.0, 0.0, 0.0) : bg;
    if (flags & FLAG_ONECHANNEL)
        return r ? saturate(m.rrr) : (a ? saturate(m.aaa) : float3(0.0, 0.0, 0.0));
    if (shown == 1)
    {
        const float c = r ? m.r : g ? m.g : b ? m.b : m.a;
        return saturate(c).xxx;
    }
    const float3 rgb = saturate(m.rgb) * float3(r ? 1.0 : 0.0, g ? 1.0 : 0.0, b ? 1.0 : 0.0);
    return (a && !diff) ? lerp(bg, rgb, saturate(m.a)) : rgb;
}

[numthreads(1, 1, 1)]
void ProbeCS(uint3 id : SV_DispatchThreadID)
{
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    if (SrcIndex != 0xFFFFFFFFu && (Flags & FLAG_NOSOURCE) == 0)
    {
        const int2 t = int2(min(ProbeTexel, MipSize - 1u));
        if (SrcKind == KIND_CUBE)
            v = FetchCube(SrcA(), FaceDir(Slice, (float2(t) + 0.5) / float2(MipSize)));
        else if (SrcKind == KIND_2DARRAY)
        {
            Texture2DArray<float4> a = ResourceDescriptorHeap[SrcIndex];
            v = a.Load(int4(t, int(Slice), int(Mip)));
        }
        else if (SrcKind == KIND_3D)
        {
            Texture3D<float4> a = ResourceDescriptorHeap[SrcIndex];
            v = a.Load(int4(t, int(Slice), int(Mip)));
        }
        else
        {
            Texture2D<float4> a = ResourceDescriptorHeap[SrcIndex];
            v = a.Load(int3(t, int(Mip)));
        }
    }
    ProbeOut[ProbeIndex] = v;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstSize.x || id.y >= DstSize.y) return;

    const float3 bg = (Background == BG_CHECKER) ? Checker(id.xy) : BgColor.rgb;
    if (Flags & FLAG_NOSOURCE)
    {
        const uint c = ((id.x >> 4) ^ (id.y >> 4)) & 1u;
        Dst[id.xy] = float4(c ? float3(0.25, 0.25, 0.25) : float3(0.16, 0.16, 0.16), 1.0);
        return;
    }

    const bool haveB = CompareMode != CMP_OFF && SrcIndexB != 0xFFFFFFFFu;
    if (haveB && CompareMode == CMP_DIFF)
    {
        const Src a = SrcA();
        const Src b = SrcB();
        float4 va, vb;
        const bool inA = Sample(a, id.xy, va);
        const bool inB = Sample(b, id.xy, vb);
        if (!inA || !inB)
        {
            Dst[id.xy] = float4(bg, 1.0);
            return;
        }
        if (BadShown(va) || BadShown(vb))
        {
            Dst[id.xy] = float4(kBad, 1.0);
            return;
        }
        const float4 d = abs(MapValue(va, a.flags) - MapValue(vb, b.flags)) * DiffScale;
        Dst[id.xy] = float4(Compose(d, bg, a.flags, true), 1.0);
        return;
    }

    // One source: A, or B right of the split.
    Src s = SrcA();
    if (haveB && CompareMode == CMP_SPLIT && float(id.x) + 0.5 >= SplitX * float(DstSize.x))
        s = SrcB();
    float4 v;
    if (!Sample(s, id.xy, v))
    {
        Dst[id.xy] = float4(bg, 1.0);
        return;
    }
    // NaN / Inf in any shown channel: magenta, whatever the mapping.
    if (BadShown(v))
    {
        Dst[id.xy] = float4(kBad, 1.0);
        return;
    }
    Dst[id.xy] = float4(Compose(MapValue(v, s.flags), bg, s.flags, false), 1.0);
}
