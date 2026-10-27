// Writes the inverted-hull rim into the 6-MRT GBuffer. The rim is unlit without a
// shading model of its own: RT5 (sceneCol) is the HdrSceneColor emissive seed and,
// per the GBuffer contract, emissive never passes through the BRDF, so albedo = 0
// kills the diffuse term, reflectance = 0 the specular term, and the pixel is
// exactly outlineColor whatever the lighting pass does.
//
// Encoding goes through MS_EncodeGBuffer so this pass can never desync from the RT
// layout (the same guarantee GBuffer.ps.hlsl's header describes).

#include "material_surface.hlsli"
#include "dissolve.hlsli"

// PushConstants / PerViewCB / InstanceBuffer / g_AllTextures / g_Materials come
// from material_bindings.hlsli (via material_surface.hlsli): this pass shares
// GBuffer.ps's binding contract on purpose, writing the same targets through the
// same encode.

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    uint     outlinePxSpaceW;   // display extents (unused here; keeps the b2 layout)
    uint     outlinePxSpaceH;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
};

// Must match OutlineGeometry.vs.hlsl VSOut.
struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 wn       : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float4 curClip  : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
    nointerpolation uint instanceSlot : INSTSLOT;
};

GBufferOut main(PSIn i)
{
    // Per-instance dissolve: the rim must vanish with the surface it outlines, or a
    // dissolving character keeps a floating silhouette.
    const float4 dparams = InstanceBuffer[i.instanceSlot].dissolveParams;
    [branch]
    if (dparams.x > 0.0 && gDissolveNoiseIdx != 0xFFFFFFFFu)
        DissolveClipLoad(g_AllTextures[gDissolveNoiseIdx], dparams, i.uv);

    // NDC motion vector, both ends unjittered. Same convention as GBuffer.ps.
    const float2 curNDC  = i.curClip.xy  / max(i.curClip.w,  1e-6);
    const float2 prevNDC = i.prevClip.xy / max(i.prevClip.w, 1e-6);
    const float2 velocity = curNDC - prevNDC;

    SurfaceData s;
    s.baseColor   = float3(0, 0, 0);   // no diffuse response
    s.opacity     = 1.0;
    s.normalWS    = normalize(i.wn);   // real normal: RT1 feeds XeGTAO / SSR decoders
    s.roughness   = 1.0;
    s.metallic    = 0.0;
    s.ao          = 1.0;
    s.reflectance = 0.0;               // no specular response
    s.emissive    = outlineColor;      // RT5: bypasses the BRDF, so this is the pixel
    s.extra       = float4(0, 0, 0, 0);
    // Must be set: this is the only manual `SurfaceData s;` in the tree (every other
    // path seeds from MS_DefaultSurface(), which zeroes every field), and
    // MS_EncodeGBuffer reads directAdditive unconditionally. Left unset it writes an
    // uninitialised float into GBuffer RT4.a, which the deferred NPR pass decodes as a
    // stylized direct-additive mask - and it compiles clean.
    // When you add a field to SurfaceData, set it here too, or switch to
    // `SurfaceData s = MS_DefaultSurface(g_Materials[materialIndex]);` + overrides.
    s.directAdditive = 0.0;
    s.shadingModel = 0;
    // Same rule: MS_EncodeGBuffer reads these whenever the outlined material's shading
    // model is Anisotropic or Subsurface (the outline shares that material's index); an
    // uninitialised anisotropyDirWS would put a garbage angle into RT4.r on an
    // anisotropic character's rim. The values are inert (an outline pixel is
    // emissive-only) but must be defined.
    s.anisotropy        = 0.0;
    s.anisotropyDirWS   = float3(1, 0, 0);
    s.subsurfaceOpacity = 1.0;
    s.subsurfaceMask    = 0.0;
    s.clearCoat         = 0.0;
    s.clearCoatRoughness= 0.0;
    s.transmission      = 0.0;
    s.thickness         = 0.0;

    GBufferOut o = MS_EncodeGBuffer(s, materialIndex, velocity);

    // Force the XeGTAO-exclude bit (RT1.a bit 15). MS_EncodeGBuffer derives it from
    // the material's flag, which belongs to the character, not to its rim: an unlit
    // line must never be darkened by ambient occlusion.
    const uint matIdxRaw = (uint)round(o.normal.a * 65535.0) | 0x8000u;
    o.normal.a = float(matIdxRaw) / 65535.0;

    return o;
}
