// TransmissionApply.ps.hlsl - the background behind every BlendMode::Transmissive surface,
// refracted, blurred and tinted, written into the HDR scene before TransparentPass draws those
// surfaces' own light on top. A fullscreen triangle (TransmissionApplyPass).
//
// Inputs: the transmission prepass's targets (custom_surface.hlsli's transmission main) - the
// summed screen-UV offset (.xy), the summed angular spread of the rough lobe (.z) and the layer
// count (.w); the product of the layers' tints; the nearest layer's reverse-Z depth - and the
// scene as the opaque pass, the sky and the fog left it, with its mip chain. A pixel no layer
// covers is discarded.

#include "bindless.hlsli"
#include "core/depth.hlsli"     // LinearizeReverseZ
#include "sampler_heap.hlsli"
#include "transmission_back.hlsli"

cbuffer TransmissionApplyCB : register(b2, space0)
{
    uint   gParamsIdx;       // Texture2D<float4>: offset.xy, spread, layers
    uint   gTintIdx;         // Texture2D<float4>: rgb tint
    uint   gLayerDepthIdx;   // Texture2D<float>:  the nearest layer's reverse-Z depth
    uint   gSceneDepthIdx;   // Texture2D<float>:  the opaque depth, reverse-Z (sky 0)
    uint   gSourceIdx;       // Texture2D<float4>: the scene before translucency, all mips
    float  gSourceMaxMip;
    float  gNearZ;
    float  gFarZ;
    float2 gRenderSize;
    float  gPixelAngle;      // 2 tan(fovY / 2) / render height: one pixel's angle at the eye
    float  gBlurScale;       // pass.transmission.blurScale
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_TARGET
{
    const int2   px     = int2(i.pos.xy);
    const float4 params = BindlessTex2D<float4>(gParamsIdx).Load(int3(px, 0));
    if (params.w <= 0.0)
        discard;
    const float3 tint   = BindlessTex2D<float4>(gTintIdx).Load(int3(px, 0)).rgb;
    const float  layerZ = BindlessTex2D<float>(gLayerDepthIdx).Load(int3(px, 0));
    Texture2D<float> sceneDepth = BindlessTex2D<float>(gSceneDepthIdx);

    const float2 uv = (float2(px) + 0.5) / gRenderSize;
    float backZ;
    const float2 uvBack = TransmissionBackUV(uv, params.xy, layerZ, sceneDepth, gRenderSize, backZ);

    // A rough lobe blurs what it shows by its spread times the distance from the surface to
    // the background, measured in pixels at the background's depth. The sky is at the far plane.
    const float zLayer    = LinearizeReverseZ(layerZ, gNearZ, gFarZ);
    const float zBack     = LinearizeReverseZ(backZ, gNearZ, gFarZ);
    const float footprint = params.z * gBlurScale * max(zBack - zLayer, 0.0)
                          / max(gPixelAngle * zBack, 1e-6);
    const float mip = clamp(log2(max(footprint, 1.0)), 0.0, gSourceMaxMip);
    const float3 background = BindlessTex2D<float4>(gSourceIdx)
        .SampleLevel(SmpFixed(SMP_LINEAR_CLAMP), uvBack, mip).rgb;
    return float4(background * tint, 1.0);
}
