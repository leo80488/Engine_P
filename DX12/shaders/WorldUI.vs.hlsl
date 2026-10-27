// Billboarded UI quads in world space (bindless texture path). The CPU billboards;
// the VS only transforms world -> clip.
// Vertex layout (28 bytes, pulled from the ByteAddressBuffer at t2 space0):
//   [0..11]  float3  worldPos
//   [12..19] float2  uv
//   [20..23] uint32  packed RGBA8 colour (post-fade)
//   [24..27] uint32  bindless texIdx (0xFFFFFFFF = bar/border sentinel)

cbuffer WorldUIDrawConsts : register(b0, space0)
{
    // First vertex of this draw range: custom-shader text buckets draw from mid-buffer,
    // and SV_VertexID including StartVertexLocation is NOT portable.
    uint g_wuVertexOffset;
    uint g_wuPad0;
    uint g_wuPad1;
    uint g_wuPad2;
};

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 g_viewProj;    // un-jittered
    float    g_wuTimeSec;   // UI clock (custom text shaders animate off this)
    float3   g_wuCbPad;
};

ByteAddressBuffer g_VB : register(t2, space0);

struct VSOut
{
    float4 pos                          : SV_POSITION;
    float2 uv                           : TEXCOORD0;
    nointerpolation uint texIdx         : TEXCOORD1;
    float4 color                        : COLOR0;
};

VSOut main(uint vid : SV_VertexID)
{
    const uint stride = 28u;
    const uint off = (vid + g_wuVertexOffset) * stride;

    float3 wp;
    wp.x = asfloat(g_VB.Load(off + 0u));
    wp.y = asfloat(g_VB.Load(off + 4u));
    wp.z = asfloat(g_VB.Load(off + 8u));

    float2 uv;
    uv.x = asfloat(g_VB.Load(off + 12u));
    uv.y = asfloat(g_VB.Load(off + 16u));

    uint packed = g_VB.Load(off + 20u);
    float4 col;
    col.r = float((packed >>  0) & 0xFFu) / 255.0;
    col.g = float((packed >>  8) & 0xFFu) / 255.0;
    col.b = float((packed >> 16) & 0xFFu) / 255.0;
    col.a = float((packed >> 24) & 0xFFu) / 255.0;

    uint texIdx = g_VB.Load(off + 24u);

    VSOut o;
    o.pos    = mul(float4(wp, 1.0), g_viewProj);
    o.uv     = uv;
    o.texIdx = texIdx;
    o.color  = col;
    return o;
}
