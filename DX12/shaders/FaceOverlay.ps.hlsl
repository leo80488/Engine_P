// FaceOverlay.ps.hlsl - brows/lashes-through-hair overlay PS.
//
// Port of DanbaidongRP's CharacterEyelashOutter fragment. Drawn over the lit HDR
// scene with alpha blend and NO depth test; three gates keep it sane:
//
//   1. HAIR-MASK CLIP - the pixel's visible surface must be hair: the hair
//      GBuffer PS writes extra.b = 1 (asset/Shaders/Hair.ps.hlsl); anything else (the
//      visible lash itself, skin, background) clips. The reference's
//      `kToonFlagHairMask` clip.
//   2. HAIR-DISTANCE FADE - alpha fades to 0 as the gap between the lash and the
//      hair covering it grows past ~maxHairDist (reference: 5 cm,
//      `1 - saturate(depthDiff * 20)`). This also kills the OTHER side's lashes
//      showing through the whole head (gap ~ head width >> 5 cm), which is why no
//      depth test is safe.
//   3. FRONT-VIEW FADE - the overlay only appears while the camera faces the lash
//      (its geometric normal ~ the face front), like the reference's
//      dot(faceFront, camDir) fade but without head axes.
//
// Shading: a half-lambert toon tint of the material's base map, gated by the sun's
// CSM visibility, plus the face's isotropic ambient - at the typical overlay alpha
// (~0.3-0.5) a full BRDF is invisible.

#include "material.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint packedOverlayParams;
};

#define LIGHT_CB_REGISTER b2
#include "light_cb.hlsli"   // lightDir, lightColor, cameraPos, cameraForward, invViewProj, ambient / IBL / DDGI fields

// Per-frame viewport params, uploaded by FaceOverlayPass (triple-buffered).
cbuffer FaceOverlayCB : register(b3, space0)
{
    float2 invRTSize;       // 1 / viewport size (screen UV from SV_POSITION)
    float2 overlayPadding;
};

StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);
Texture2D<float4> gExtraTex  : register(t3, space0);   // GBuffer RT4 (extra.b = hair mask)
Texture2D<float>  gDepthTex  : register(t4, space0);   // scene depth (reversed-Z)
Texture2D         g_AllTextures[] : register(t0, space2);  // bindless table
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearWrap SmpFixed(SMP_LINEAR_WRAP)

// CSM, so the overlay's direct sun term can be gated. Cascades only:
// PER_OBJECT_SHADOW and RT_SHADOW_MASK are deliberately NOT defined - both contain
// the character's OWN casters, and this overlay exists to draw a brow THROUGH the
// hair that would occlude it (same reasoning and function as the NPR face path in
// Lighting.ps.hlsl). No root-signature cost: Gfx::ShadowSRV (t9..t11) and s2 are in
// the shared graphics signature; FaceOverlayPass binds them.
Texture2DArray<float>  gShadowCascades : register(t9, space0);
#define gShadowSampler SmpCmp(SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE)
#include "shadow.hlsli"

// The indirect term: the sky's irradiance (SH at t19, or the cube at t6 when
// iblUseSH = 0) and DDGI through ResourceDescriptorHeap (LightCB's volume buffer
// index) - the face's own sources in Lighting.ps. No root-signature cost: t6 / t19 /
// s1 are in the shared graphics signature; FaceOverlayPass binds them like the lit
// particles.
#include "sky_sh.hlsli"         // gSkySH (t19) + EvalSH2Isotropic
#include "DDGISampling.hlsli"   // DDGI_PickVolumes / DDGI_SampleVolumeAmbientIso
TextureCube  gIrradiance : register(t6, space0);
#define gIBLSampler SmpFixed(SMP_LINEAR_WRAP)

// The face's ISOTROPIC ambient, Lighting.ps's toon path for an active toon receiver
// in miniature: DDGI's sphere average weighted by the two arbitration winners'
// coverage, the sky's sphere average (SH band 0, or the irradiance cube's six-axis
// mean) x iblStrength for the rest, plus the flat ambient (None / FlatColor; 0 in
// the sky modes). No AO (the overlay has none) and no SSGI (toon receivers opt out
// of it there too).
float3 FaceOverlayAmbientIso(float3 P, float3 N, float3 V)
{
    float3 ddgi = float3(0, 0, 0);
    float  cov  = 0.0;
    if (ddgiEnabled != 0 && ddgiVolumeCount > 0)
    {
        StructuredBuffer<DDGIVolumeGPU> vols = BindlessSBuf<DDGIVolumeGPU>(ddgiVolumeBufIdx);
        const DDGIVolumePick pick = DDGI_PickVolumes(P, vols);
        float4 best = float4(0, 0, 0, 0), second = float4(0, 0, 0, 0);
        [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
        {
            if (!DDGI_IsPicked(pick, vi)) continue;
            const DDGIVolumeGPU vol = vols[vi];
            Texture2D<float4> irr = ResourceDescriptorHeap[NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
            Texture2D<float2> dep = ResourceDescriptorHeap[NonUniformResourceIndex(vol.depthBaseIdx + vi)];
            StructuredBuffer<DDGIProbeData> pd =
                ResourceDescriptorHeap[NonUniformResourceIndex(vol.probeDataBaseIdx + vi)];
            const float4 s = DDGI_SampleVolumeAmbientIso(P, N, V, vol, irr, dep, pd, gIBLSampler);
            if (vi == (uint)pick.bestSlot) best = s; else second = s;
        }
        const float4 r = DDGI_CombineWinners(best, second, pick);
        ddgi = r.rgb * ddgiDiffuseScale;
        cov  = r.a;
    }
    float3 sky = float3(0, 0, 0);
    [branch] if (iblRadianceMips > 0)   // the sky modes; None / FlatColor bind black anyway
    {
        sky = (iblUseSH != 0) ? EvalSH2Isotropic()
            : (gIrradiance.SampleLevel(gIBLSampler, float3( 1, 0, 0), 0).rgb
             + gIrradiance.SampleLevel(gIBLSampler, float3(-1, 0, 0), 0).rgb
             + gIrradiance.SampleLevel(gIBLSampler, float3( 0, 1, 0), 0).rgb
             + gIrradiance.SampleLevel(gIBLSampler, float3( 0,-1, 0), 0).rgb
             + gIrradiance.SampleLevel(gIBLSampler, float3( 0, 0, 1), 0).rgb
             + gIrradiance.SampleLevel(gIBLSampler, float3( 0, 0,-1), 0).rgb) * (1.0 / 6.0);
        sky *= iblStrength;
    }
    return ddgi * cov + sky * saturate(1.0 - cov) + ambientColor * ambientIntensity;
}

struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float3 wn       : NORMAL;
    float2 uv       : TEXCOORD0;
};

#include "core/depth.hlsli"   // WorldPosFromDepth
float3 ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

float4 main(PSIn i) : SV_Target0
{
    int3 px = int3(i.sv.xy, 0);

    // Per-draw FaceOverlayComponent settings. Keeping these in the fourth root
    // DWORD lets multiple tagged meshes use independent values in one frame.
    float overlayAlpha = float(packedOverlayParams & 0xFFFFu) / 65535.0f;
    float maxHairDist01 = float((packedOverlayParams >> 16u) & 0xFFFFu) / 65535.0f;
    float maxHairDist = lerp(0.005f, 0.5f, maxHairDist01);
    float invMaxHairDist = rcp(maxHairDist);

    // ---- 1) hair-mask clip: the visible surface here must be hair ----
    // RT4.b serves two disjoint populations: HAIR materials write this mask, FACE
    // materials their second SDF mirror half (channel budget in face_sdf.hlsli). A raw
    // SDF in [0,1] clears the 0.5 test on many face texels, so the face marker in .a
    // disambiguates; without it the overlay leaks onto bare face wherever the SDF
    // reads high.
    float4 extra = gExtraTex.Load(px);
    clip((extra.a > 0.5) ? -1.0 : (extra.b - 0.5));

    // ---- 2) lash -> covering-hair distance fade ----
    float sceneDepth = gDepthTex.Load(px);          // the hair's depth (reversed-Z)
    float2 screenUV  = i.sv.xy * invRTSize;
    float3 hairWS    = ReconstructWorldPos(screenUV, sceneDepth);
    // + = this lash fragment is BEHIND the hair surface (the covered case).
    float depthDiff  = dot(i.worldPos - hairWS, cameraForward);
    float alpha      = overlayAlpha * (1.0 - saturate(depthDiff * invMaxHairDist));
    // A fragment in FRONT of the visible surface is a false positive (mask from
    // adjacent strands): fade it out just as fast.
    alpha *= 1.0 - saturate(-depthDiff * invMaxHairDist);

    // ---- 3) front-view fade ----
    float3 V = normalize(cameraPos - i.worldPos);
    float3 N = normalize(i.wn);
    alpha *= smoothstep(0.0, 0.25, dot(N, V));

    // ---- base map + simple toon tint ----
    MaterialGPUData mat = g_Materials[materialIndex];
    float4 baseTex = float4(1, 1, 1, 1);
    // Custom-shader materials (every NPR brow/lash part: Face.ps, EndfieldBrow...)
    // carry their real base map as the FIRST space3 texture (_BaseMap at t0 by
    // convention); their ENGINE slot 0 may be the white import default (the Endfield
    // manifest import leaves it so). Prefer the custom slot; plain materials fall back
    // to the engine BASECOLORMAP.
    int texBaseColor = (mat.customTextureIds[0] >= 0) ? mat.customTextureIds[0]
                                                      : mat.textureHandleIds[0];
    if (texBaseColor >= 0)
        baseTex = g_AllTextures[NonUniformResourceIndex(texBaseColor)].Sample(
            g_LinearWrap, MaterialUV(mat, i.uv));   // material UV transform parity

    float3 albedo = baseTex.rgb * mat.baseColor.rgb;
    alpha        *= baseTex.a * mat.baseColor.a;

    float  halfLambert = saturate(dot(N, normalize(-lightDir)) * 0.5 + 0.5);

    // @vis-linear — the sun term is DIRECT and must vanish in shadow.
    // ComputeShadowFactorExternalOnly, not ComputeShadowFactor, as Lighting.ps does for
    // face pixels: the per-object player map and the RT sun mask contain the
    // CHARACTER'S OWN casters, and a brow drawn through its own hair must not be
    // darkened by that hair.
    const float sunVis = ComputeShadowFactorExternalOnly(i.worldPos, N, i.sv.xy);

    // The `lerp(0.55, 1, halfLambert)` wrap is authored (it stops a backlit brow going
    // flat): keep it. All of it is multiplied by lightColor, so all of it is DIRECT sun
    // energy and must be gated by sunVis, or a brow in a cast shadow gets 55-100% of
    // full sunlight.
    float3 direct  = albedo * lightColor * lerp(0.55, 1.0, halfLambert) * sunVis;
    // The face's ambient, scaled as the NPR composite scales the face's
    // (nprAmbientLightScale, 0.75 by default). The flat ambient alone is 0 in every
    // sky-mode scene: a brow in shadow would draw black through the hair while the skin
    // around it takes the sky and DDGI.
    float3 ambient = albedo * FaceOverlayAmbientIso(i.worldPos, N, V) * max(mat.nprAmbientLightScale, 0.0);
    float3 color   = direct + ambient;

    return float4(color, saturate(alpha));
}
