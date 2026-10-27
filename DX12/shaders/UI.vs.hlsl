// UI vertex shader: orthographic projection of UI quads / triangles.
// Vertex layout (UIVertex, 20 bytes packed):
//   [0..7]   float2 pos (screen-space pixels, origin top-left)
//   [8..15]  float2 uv  (0..1 if textured)
//   [16..19] uint32 col (RGBA8: byte order R,G,B,A)
// Vertices are fetched by SV_VertexID from a ByteAddressBuffer at t2 space0. The per-draw vertex
// offset comes EXPLICITLY through b0 (g_vertexOffset) and every draw uses StartVertexLocation = 0:
// do not rely on SV_VertexID including StartVertexLocation: on this draw path that made every
// command after the first read from vertex 0.

cbuffer UIDrawConsts : register(b0, space0)
{
    uint g_materialID;    // (PS) 0 = plain, 1 = SDF text
    uint g_effectIndex;   // (PS) index into the effects table
    uint g_vertexOffset;  // (VS) first vertex of this draw command
    uint g_uiPad1;
};

cbuffer UICB : register(b1, space0)
{
    float2 g_canvasSizePx; // viewport in pixels
    float  g_uiTimeSec;    // UI clock (custom text shaders animate off this)
    uint   g_pad1;
};

ByteAddressBuffer g_UIVB : register(t2, space0);

struct VSOut
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut main(uint vid : SV_VertexID)
{
    const uint stride = 20u;
    const uint off    = (vid + g_vertexOffset) * stride;

    float2 px;
    px.x = asfloat(g_UIVB.Load(off + 0u));
    px.y = asfloat(g_UIVB.Load(off + 4u));

    float2 uv;
    uv.x = asfloat(g_UIVB.Load(off + 8u));
    uv.y = asfloat(g_UIVB.Load(off + 12u));

    uint packed = g_UIVB.Load(off + 16u);
    float4 col;
    col.r = float((packed >>  0) & 0xFFu) / 255.0;
    col.g = float((packed >>  8) & 0xFFu) / 255.0;
    col.b = float((packed >> 16) & 0xFFu) / 255.0;
    col.a = float((packed >> 24) & 0xFFu) / 255.0;

    // Orthographic: pixel (0,0) -> NDC (-1,+1), pixel (W,H) -> NDC (+1,-1).
    float2 inv = 1.0 / max(g_canvasSizePx, float2(1.0, 1.0));
    float2 ndc = float2(px.x * inv.x * 2.0 - 1.0,
                        1.0 - px.y * inv.y * 2.0);

    VSOut o;
    o.pos   = float4(ndc, 0.0, 1.0);
    o.uv    = uv;
    o.color = col;
    return o;
}
