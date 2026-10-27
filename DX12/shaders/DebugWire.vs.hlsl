// DebugWire.vs.hlsl - debug line vertex shader. Reads each vertex from a
// ByteAddressBuffer by SV_VertexID: float3 position + uint RGBA8 colour, 16 bytes.

#include "debug_wire_common.hlsli"

ByteAddressBuffer g_LineVB : register(t2, space0);

DebugWireVSOut main(uint vid : SV_VertexID)
{
    uint byteOff = vid * 16u;
    float3 pos;
    pos.x = asfloat(g_LineVB.Load(byteOff));
    pos.y = asfloat(g_LineVB.Load(byteOff + 4u));
    pos.z = asfloat(g_LineVB.Load(byteOff + 8u));
    uint  packedColor = g_LineVB.Load(byteOff + 12u);

    // RGBA8, R in the low byte.
    float4 color;
    color.r = float((packedColor >>  0) & 0xFF) / 255.0;
    color.g = float((packedColor >>  8) & 0xFF) / 255.0;
    color.b = float((packedColor >> 16) & 0xFF) / 255.0;
    color.a = float((packedColor >> 24) & 0xFF) / 255.0;

    DebugWireVSOut o;
    o.pos   = mul(float4(pos, 1.0), g_viewProj);
    o.color = color;
    return o;
}
