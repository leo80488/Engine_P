#ifndef PUNCTUAL_SHADOW_EVAL_HLSLI
#define PUNCTUAL_SHADOW_EVAL_HLSLI
// punctual_shadow_eval.hlsli - spot and point light shadows with every resource and
// constant passed in. No register, no heap: the binding is punctual_shadow.hlsli (the
// graphics signature: the spot views at t22, LightCB, gShadowSampler at s2), which opens the
// atlases and calls these.
#include "spot_shadow_view.hlsli"
#include "spot_shadow_filter.hlsli"

// The atlas is the one shadowView.atlasIndex names; the caller has already returned 1.0 for
// a view without one (atlasIndex == 0xFFFFFFFF). Reads the view's resolution so receiver
// bias stays correct when the resolution changes.
float SpotShadowFromAtlas(SpotShadowView shadowView, Texture2DArray<float> atlas,
                          float3 worldPos, float3 worldDx, float3 worldDy, float3 N, float3 lightPos,
                          float spotHalfAngle, float3 cameraPos, float shadowNormalOffset)
{
    // Keep the slope-dependent normal offset, plus a small world-space
    // reconstruction allowance even at NdotL == 1. Caster D32 ULP bias alone
    // cannot cover inverse camera VP -> world -> light VP roundoff at every
    // view angle. A fixed NDC bias would grow quadratically in world space.
    const float3 Lvec  = lightPos - worldPos;
    const float  dist  = max(length(Lvec), 1e-4);
    const float  NdotL = saturate(dot(N, Lvec / dist));
    // World size of one shadow texel at this receiver: the map spans the full cone
    // (fov = 2 * halfAngle) across shadowView.resolution texels at `dist`.
    const uint shadowWidth = shadowView.resolution;
    const float  texelWorld = (2.0 * dist * tan(spotHalfAngle)) / float(shadowWidth);
    const float reconstructionScale = max(length(worldPos - cameraPos),
        max(max(abs(worldPos.x), abs(worldPos.y)), abs(worldPos.z)));
    // Sixteen FP32 epsilons of the reconstruction scale, bounded in meters.
    // Minimum 0.5 mm covers near-camera quantization; cap at 2 mm to keep
    // the allowance below the thickness of the school's thin door panels.
    const float receiverAllowance = clamp(reconstructionScale * 1.9073486e-6,
                                           0.0005, 0.002);
    // Toward the light changes only comparison depth, not shadow UV. Unlike
    // a normal-only offset this also works with normal-mapped receivers.
    const float3 offsetPos = worldPos
        + N * (shadowNormalOffset * texelWorld * (1.0 - NdotL))
        + (Lvec / dist) * min(receiverAllowance, dist * 0.01);

    float4 sc = mul(float4(offsetPos, 1.0), shadowView.vp);
    if (sc.w <= 0.0) return 1.0;
    sc.xyz /= sc.w;
    // Project derivatives of the GEOMETRIC receiver, evaluated before the
    // divergent clustered-light loop. Derivatives inside that loop can mix
    // different lights across a quad and cause view-dependent flashing.
    float4 sx = mul(float4(worldDx, 0.0), shadowView.vp);
    float4 sy = mul(float4(worldDy, 0.0), shadowView.vp);
    float3 dx = (sx.xyz - sc.xyz * sx.w) / sc.w;
    float3 dy = (sy.xyz - sc.xyz * sy.w) / sc.w;
    dx.xy *= float2(0.5, -0.5);
    dy.xy *= float2(0.5, -0.5);
    float det = dx.x * dy.y - dx.y * dy.x;
    float2 depthGradient = 0.0;
    if (abs(det) > max(1e-20, length(dx.xy) * length(dy.xy) * 1e-4))
        depthGradient = float2(dy.y * dx.z - dx.y * dy.z,
                               dx.x * dy.z - dy.x * dx.z) / det;
    // Silhouettes/degenerate receiver planes cannot provide a useful fit.
    if (any(!isfinite(depthGradient)) ||
        max(abs(depthGradient.x), abs(depthGradient.y)) / float(shadowWidth) > max(sc.z, 1e-6) * 0.25)
        depthGradient = 0.0;
    float2 uv = sc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (any(uv < 0.0) || any(uv > 1.0) || sc.z < 0.0 || sc.z > 1.0)
        return 1.0;
    // Reversed-Z GREATER_EQUAL: the world-space offset above raises the
    // reference depth. Do not additionally add a constant NDC receiver bias.
    // Preserve the same tent footprint/weights, but predict depth at all 16
    // actual texel centers (not the nine bilinear sample locations).
    return FilterSpotShadowPlane(atlas, shadowView.slice, shadowWidth, uv, sc.z, depthGradient);
}

// Must match PointShadowPass.h (kNearPlane). The face size is the atlas's own
// (settings.json shadows.pointResolution sizes it at boot).
#define POINT_SHADOW_NEAR 0.05

// Omnidirectional (cube) shadow of one point light: `atlas` holds one cube per
// active caster (R32_FLOAT TEXTURECUBEARRAY), cubeIdx picks it; `cmp` is the
// reversed-Z GREATER_EQUAL comparison sampler.
float PointShadowFromCube(TextureCubeArray<float> atlas, SamplerComparisonState cmp,
                          float3 worldPos, float3 N, float3 lightPos, float radius, uint cubeIdx)
{
    float3 v    = worldPos - lightPos;
    float  dist = length(v);
    if (dist >= radius) return 1.0;   // outside range -> lit (attenuation already culls)

    // Normal-offset bias (world space, scales with the face's texel size at this
    // distance) to fight self-shadow acne. All depth bias is on the caster side
    // (rasterizer slope-scale), matching SpotShadow - this is the only receiver
    // nudge, and it pushes ALONG the normal so it never leaks past occluders.
    // Scale by (1 - NdotL) like SampleCascadeShadow (shadow.hlsli): head-on
    // surfaces get zero offset (tight contact), grazing get the full push.
    uint   faceW, faceH, faceN;
    atlas.GetDimensions(faceW, faceH, faceN);
    float  texelW  = 2.0 * dist / float(max(faceW, 1u));
    float3 Ldir    = -v / dist;                          // toward the light
    float  noScale = saturate(1.0 - dot(N, Ldir));
    float3 sp      = worldPos + N * texelW * 2.0 * noScale;
    float3 vv      = sp - lightPos;

    // View-space depth on the selected cube face == dominant axis magnitude.
    // Reversed-Z NDC for PerspectiveFovLH(NearZ = radius, FarZ = POINT_SHADOW_NEAR):
    //   ndc(Vz) = fRange * (1 - radius / Vz),  fRange = near / (near - radius)
    float  Vz       = max(abs(vv.x), max(abs(vv.y), abs(vv.z)));
    Vz = max(Vz, 1e-3);
    float  fRange   = POINT_SHADOW_NEAR / (POINT_SHADOW_NEAR - radius);
    float  refDepth = saturate(fRange * (1.0 - radius / Vz));

    return atlas.SampleCmpLevelZero(cmp, float4(vv, (float)cubeIdx), refDepth);
}

#endif // PUNCTUAL_SHADOW_EVAL_HLSLI
