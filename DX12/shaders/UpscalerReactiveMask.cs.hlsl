// The reactive mask FSR 2 and DLSS SR read (UpscalerReactiveMaskPass).
// Two kinds of pixel write colour but no depth or velocity, so a temporal resolve
// reprojects the background behind them and smears them:
//   0x40 - particles / trails that opted out of TAA (excludeFromTAA; stamped by
//          ParticlePasses / TrailPasses);
//   0x80 - the outline rim (OutlinePass::tagStencilForTAA).
// TAA reads the bits from the stencil plane and floors its current-frame weight
// (particleVfxMinAlpha / outlineMinAlpha). The upscalers do not read stencil, so
// this writes the same floors as FSR's `reactive` (higher = less history) and DLSS's
// BiasCurrentColor (lerp(history, current, bias)). The weights are TAAPass's own
// members, so a pixel is treated alike under every resolve and tuning stays in one place.
//
// Compute root signature (space2):
//   b0 - ReactiveMaskCB
//   t0 - the stencil plane of the main depth (X32_TYPELESS_G8X24_UINT: .y)
//   u0 - the mask (R8_UNORM, render resolution)

cbuffer ReactiveMaskCB : register(b0, space2)
{
    uint  width;
    uint  height;
    uint  particleBit;      // 0x40; 0 disables the term
    uint  outlineBit;       // 0x80; 0 disables the term
    float particleWeight;   // TAAPass::particleVfxMinAlpha
    float outlineWeight;    // TAAPass::outlineMinAlpha
    float2 _pad;
};

Texture2D<uint2>    gStencil  : register(t0, space2);
RWTexture2D<float>  gReactive : register(u0, space2);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height)
        return;

    const uint stencil = gStencil.Load(int3(dtid.xy, 0)).y;
    float reactive = 0.0;
    if ((stencil & particleBit) != 0) reactive = max(reactive, particleWeight);
    if ((stencil & outlineBit)  != 0) reactive = max(reactive, outlineWeight);
    gReactive[dtid.xy] = reactive;
}
