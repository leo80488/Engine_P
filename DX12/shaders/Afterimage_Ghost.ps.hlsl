// Afterimage_Ghost.ps.hlsl - unlit Fresnel-rim ghost PS for snapshot meshes.
//
// Drop-in custom PS for the engine TransparentPass:
//   MaterialComponent.useCustomShader    = true
//   MaterialComponent.customShaderPath   = "shaders/Afterimage_Ghost.ps.hlsl"
//   MaterialComponent.userBlendMode      = BlendMode::Additive
//   MaterialComponent.customShadingModel = ShadingModel::Unlit
//
// Per-snapshot uniforms travel through MaterialCBVRing -> the cbuffer at b8;
// AfterimageSystem updates customParams every frame:
//   GhostColor   .rgb = HDR tint, .a = lifetime fade (0..1)
//   GhostParams  .x   = Fresnel power
//                .y   = base alpha (camera-facing alpha floor)
//                .z   = rim intensity multiplier
//                .w   = unused
//
// The additive PSO is ONE/ONE (src.rgb + dst.rgb, alpha does not scale it), so rgb
// is premultiplied by alpha here: the energy add fades with the silhouette instead
// of blowing out.

#include "material.hlsli"
#include "fog_apply.hlsli"   // FogApplyCB b4 + s3, bound by TransparentPass

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);

cbuffer GhostCB : register(b8, space0)
{
    float4 GhostColor;    // rgb = HDR tint, a = lifetime fade (0..1)
    float4 GhostParams;   // x=fresnelPower, y=baseAlpha, z=rimIntensity, w=_pad
};

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
};

float4 main(PSIn i) : SV_TARGET
{
    const float fresnelPower = max(GhostParams.x, 0.001);
    const float baseAlpha    = saturate(GhostParams.y);
    const float rimIntensity = max(GhostParams.z, 0.0);
    const float lifeFade     = saturate(GhostColor.a);

    // ---- View vector ----
    // The ghost is procedural (no material props) and reads no scene CB, so there is no
    // camera position: V is built from screen-space derivatives instead.
    const float3 N = normalize(i.wn);
    // Caution: ddx/ddy of the world position both lie in the triangle's plane, so
    // their cross is the geometric face normal, not the view direction; the rim below
    // therefore measures how far the interpolated normal bends from the face
    // (abs() absorbs the winding sign).
    const float3 ddxWP = ddx(i.worldPos);
    const float3 ddyWP = ddy(i.worldPos);
    const float3 V = normalize(cross(ddyWP, ddxWP));

    // ---- Fresnel rim ----
    const float NdotV  = saturate(abs(dot(N, V)));
    const float rim    = pow(1.0 - NdotV, fresnelPower);

    // ---- Compose ----
    // baseAlpha keeps the body of the mesh faintly visible; rim adds the bright
    // silhouette. Both attenuate by lifeFade so old snapshots dissolve out.
    const float silhouette = baseAlpha + rim * rimIntensity;
    const float alpha      = saturate(silhouette) * lifeFade;

    if (alpha < 0.002) discard;

    const float3 rgb = GhostColor.rgb * alpha;

    // Fogged at this fragment's depth, like every other additive surface drawn after
    // the fog composites.
    return Fog_ApplyAdditive(float4(rgb, alpha), Fog_Evaluate(i.sv));
}
