// Camera-only motion vectors for SKY pixels.
// GBufferPass clears GBuffer3 (velocity) to zero and only geometry passes fill it;
// SkyboxPass and the cloud composite have no velocity MRT. FSR2 / DLSS take velocity
// at face value, so a zero sky ghosts under camera motion. TAA's own sky synthesis
// (depth <= 0 && velocity ~ 0) ASSIGNS, so once this fills the buffer it simply stops
// firing instead of double-applying. Sky early-outs (SSRTemporal, SSGIResolve) never
// load velocity.
//
// Math: unproject the pixel onto the far plane and project that point through the
// previous frame's view-projection. At infinity parallax is pure rotation; the
// translation parallax of a 1.5-4 km cloud deck is sub-pixel. Both matrices are
// UNJITTERED, like MS_Velocity.
//
// Output must match material_surface.hlsli MS_Velocity: raw NDC delta curNDC - prevNDC,
// not scaled to UV, not y-flipped; each consumer applies its own (0.5, -0.5).
//
// Compute root signature (space2):
//   b0 - VelocityFillCB
//   t0 - depth    (R32_FLOAT plane of the D32_FLOAT_S8X24 main depth)
//   u0 - velocity (R16G16_FLOAT, GBuffer3, read-modify-write)

cbuffer VelocityFillCB : register(b0, space2)
{
    // Inverse of the current UNJITTERED view-projection, transposed for the
    // row-vector convention this engine's HLSL uses (vector on the left).
    float4x4 invViewProjNoJitter;
    // Previous frame's UNJITTERED view-projection, likewise transposed.
    float4x4 prevViewProj;
    uint     width;
    uint     height;
    float2   _pad;
};

Texture2D<float>     gDepth    : register(t0, space2);
RWTexture2D<float2>  gVelocity : register(u0, space2);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height)
        return;

    const int2 px = int2(dtid.xy);

    // Reverse-Z: the far plane is EXACTLY 0, and the sky is the only thing that
    // writes it. Anything greater is geometry and already carries a real
    // velocity that must not be overwritten.
    const float depth = gDepth.Load(int3(px, 0));
    if (depth > 0.0)
        return;

    // Pixel centre -> NDC. NDC y is up, texture y is down.
    const float2 uv     = (float2(px) + 0.5) / float2(width, height);
    const float2 curNDC = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    // Unproject at the far plane (z = 0 under reverse-Z) to a world point.
    const float4 farW = mul(float4(curNDC, 0.0, 1.0), invViewProjNoJitter);
    const float3 wpos = farW.xyz / farW.w;

    // Reproject through the previous frame's camera.
    const float4 prevC = mul(float4(wpos, 1.0), prevViewProj);

    // A degenerate w means the point is behind the previous camera: leave zero
    // velocity (static) rather than reprojecting from garbage.
    if (abs(prevC.w) < 1e-6)
        return;

    gVelocity[px] = curNDC - prevC.xy / prevC.w;
}
