// DLSS-RR's colour before transparency, in the space of its input (RRColorSnapshot in
// RRGuidePass.h).
//
// The snapshot is a copy of the HDR scene taken inside the render graph; RR's input colour is
// NPRStabilize's output, which counter-scales every exposure-protected pixel (a toon
// surface's authored ACES input: x rcp(exposure), plus its shoulder and ceilings). Left as it
// is, the snapshot would disagree with the input on exactly those pixels and RR would read
// the difference as a transparency layer. NPRStabilize's correction belongs to the opaque
// surface under every layer, so the snapshot gets the same per-pixel delta (stabilized -
// unstabilized) the input got; the transparents' share of the input stays as it was.
cbuffer Params : register(b0)   // root constants: RRColorSnapshot::Align
{
    uint width, height, hdrIdx, stabilizedIdx, snapUav;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    Texture2D<float4> hdr = ResourceDescriptorHeap[hdrIdx];
    Texture2D<float4> stabilized = ResourceDescriptorHeap[stabilizedIdx];
    const float3 delta = stabilized.Load(int3(px, 0)).rgb - max(hdr.Load(int3(px, 0)).rgb, 0);
    if (all(delta == 0)) return;
    RWTexture2D<float4> snap = ResourceDescriptorHeap[snapUav];
    const float4 v = snap[px];
    snap[px] = float4(max(v.rgb + delta, 0), v.a);
}
