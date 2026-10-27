// DebugWireDepth.ps.hlsl - debug line PS with a manual occlusion test.
//
// DebugWirePass draws after tonemap and upscale so TAA/FSR2 never see the lines:
// they have no motion vectors, and drawn before TAA they would be reprojected by
// the geometry behind them and ghost. The target is display-res LDR while scene
// depth is render-res; D3D12 cannot pair mismatched RTV/DSV sizes, so hardware
// depth testing is off and the comparison happens here.
//
// Occluded pixels are faded, not discarded: a light behind a wall stays findable
// while the occluder still pins the gizmo to the world.

#include "debug_wire_common.hlsli"

Texture2D<float> g_SceneDepth : register(t3, space0);

float4 main(DebugWireVSOut i) : SV_TARGET
{
    float4 c = i.color;

    // zw == 0: no depth SRV bound this frame; behave like the overlay variant.
    if (g_targetSize.z > 0.0 && g_targetSize.w > 0.0)
    {
        // Display-res pixel centre (i.pos.xy) -> normalised -> render-res depth texel.
        float2 uv = i.pos.xy / max(g_targetSize.xy, float2(1.0, 1.0));
        int2   px = int2(saturate(uv) * (g_targetSize.zw - float2(1.0, 1.0)));
        float  sceneZ = g_SceneDepth.Load(int3(px, 0));

        // Reverse-Z: larger is nearer, far plane is 0 and the sky writes exactly 0, so
        // it never occludes. The bias is relative to the line's own depth: reverse-Z puts
        // almost all precision near the camera, so a fixed epsilon would z-fight up close
        // and over-bias in the distance.
        float bias = g_params.y * max(i.pos.z, 1e-6);
        if (sceneZ > i.pos.z + bias)
            c.a *= g_params.x;
    }
    return c;
}
