// Default trail pixel shader: ribbon colour x optional texture. The input struct and the
// premultiplied output contract (TrailOutput()) live in TrailFX.hlsli, shared with the built-in FX
// and user custom shaders.

#include "TrailFX.hlsli"

float4 main(TrailPSIn i) : SV_TARGET
{
    const float4 tex = TrailSampleTexture(i.texIdx, i.uv);
    const float3 rgb = i.color.rgb * tex.rgb;
    const float  a   = i.color.a * i.alpha * tex.a;
    if (a < 0.004) discard;
    return TrailOutput(i, rgb, a);
}
