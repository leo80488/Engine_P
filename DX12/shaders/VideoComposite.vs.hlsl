// Fullscreen triangle for the NV12 -> RGB composite. Same math as
// CloudComposite.vs.hlsl, kept separate so the composite's geometry can change on its
// own; world-space video quads use VideoQuad.vs.hlsl.

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2u, vid & 2u);
    o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv.y = 1.0 - o.uv.y;
    return o;
}
