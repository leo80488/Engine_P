// PathTraceFogApply.ps.hlsl - the reference path tracer's camera fog over the scene
// (PathTraceFogPass). The fog target holds PathTracePass's running means at render size: rgb the
// light the fog scatters toward the camera, a the camera segment's transmittance. Blend
// ONE / INV_SRC_ALPHA: out = rgb + scene x a.
//
// Graphics root signature only: PTFogCB at b2; the target is read through the heap.

cbuffer PTFogCB : register(b2, space0)
{
    uint  FogIdx;   // the fog target's SRV heap index
    uint3 _ptfPad;
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_TARGET
{
    Texture2D<float4> fog = ResourceDescriptorHeap[FogIdx];
    const float4 f = fog.Load(int3(int2(i.pos.xy), 0));
    return float4(f.rgb, 1.0 - f.a);
}
