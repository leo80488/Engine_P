// DebugWire.ps.hlsl - solid colour output for debug lines.
// Always-on-top overlay variant (collision / navmesh / AABB): no occlusion test.

#include "debug_wire_common.hlsli"

float4 main(DebugWireVSOut i) : SV_TARGET
{
    return i.color;
}
