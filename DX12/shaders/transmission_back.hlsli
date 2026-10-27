#ifndef TRANSMISSION_BACK_HLSLI
#define TRANSMISSION_BACK_HLSLI
// Where a Transmissive pixel's background comes from: TransmissionApply.ps.hlsl samples it there,
// TransmissionMotion.cs.hlsl gives the pixel that background's motion. Both must agree, so both
// call this.
//
// @p uv is the pixel's centre, @p offset the prepass's summed screen offset, @p layerZ the nearest
// layer's reverse-Z depth. Returns the background's UV and its scene depth in @p backZ.
float2 TransmissionBackUV(float2 uv, float2 offset, float layerZ, Texture2D<float> sceneDepth,
                          float2 renderSize, out float backZ)
{
    float2 uvBack = uv + offset;
    // Off screen: fade the offset out over the last 5% instead of smearing the border texels.
    const float2 inside = saturate(min(uvBack, 1.0 - uvBack) * 20.0);
    uvBack = lerp(uv, saturate(uvBack), min(inside.x, inside.y));

    // Something nearer than the surface at the refracted pixel is in front of the glass, not
    // behind it: keep the pixel straight behind instead of pulling the foreground in.
    const int2 pxBack = clamp(int2(uvBack * renderSize), int2(0, 0), int2(renderSize) - 1);
    backZ = sceneDepth.Load(int3(pxBack, 0));
    if (backZ > layerZ)
    {
        uvBack = uv;
        backZ  = sceneDepth.Load(int3(int2(uv * renderSize), 0));
    }
    return uvBack;
}
#endif
