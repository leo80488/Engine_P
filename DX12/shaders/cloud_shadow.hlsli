#ifndef CLOUD_SHADOW_HLSLI
#define CLOUD_SHADOW_HLSLI

#include "bindless.hlsli"   // typed bindless handles

// cloud_shadow.hlsli - samples the volumetric-cloud shadow map.
// The map is an orthographic grid along the sun direction, baked by CloudShadowBake.cs.hlsl.
// The two basis rows arrive PRE-DIVIDED by the projection extent with the centre folded
// into .w, so a receiver's UV is two dot products:
//     uv = float2(dot(P, R.xyz) + R.w, dot(P, U.xyz) + U.w)
// Both basis vectors are perpendicular to the sun, so the UV is invariant along a sun ray:
// one 2D map shadows every altitude under the deck with no depth term.
// Binding: the map is opened from ResourceDescriptorHeap[] with its handle and basis rows
// from LightCB. Graphics consumers bind LightCB and call SampleCloudShadow();
// VolumetricRaymarch.cs opens the frame's LightCB from the heap (LIGHT_CB_AS_STRUCT) and
// passes its members to CloudShadowSample().
// "No map this frame" (clouds disabled, bake skipped, strength 0) returns 1.0, full sun,
// so call sites need no special case.

// Fade the term out over the outer 10% of the footprint. The map follows the
// camera, so without this the shadow would pop along a hard square edge that
// slides across the world as the camera moves.
float CloudShadowEdgeFade(float2 uv)
{
    float2 d = min(uv, 1.0 - uv);                 // distance to nearest border
    return saturate(min(d.x, d.y) / 0.10);
}

// The projection + fetch with everything passed explicitly; every caller funnels through
// it. 1 = full sun, 0 = fully shadowed.
float CloudShadowCore(float3 worldPos, float4 R, float4 U,
                      Texture2D<float> tex, SamplerState samp)
{
    float2 uv = float2(dot(worldPos, R.xyz) + R.w,
                       dot(worldPos, U.xyz) + U.w);
    float fade = CloudShadowEdgeFade(uv);
    if (fade <= 0.0) return 1.0;
    return lerp(1.0, saturate(tex.SampleLevel(samp, saturate(uv), 0)), fade);
}

// 1 = full sun. @p idx is the map's bindless handle (the Tex2D null when there is no map
// this frame: clouds off, bake skipped or strength 0); @p R / @p U are LightCB's basis rows.
float CloudShadowSample(float3 worldPos, uint idx, float4 R, float4 U,
                        SamplerState samp)
{
    // The null reads 0 (fully shadowed), so "no map" is tested, not read. 0 is tested too:
    // it is what an unwritten LightCB holds, and heap slot 0 is a live bindless-table texture.
    if (idx == BINDLESS_NULL_TEX2D_FLOAT || idx == 0u) return 1.0;

    // The index comes from a constant buffer, so no NonUniformResourceIndex().
    Texture2D<float> tex = BindlessTex2D<float>(idx);
    return CloudShadowCore(worldPos, R, U, tex, samp);
}

#ifndef LIGHT_CB_AS_STRUCT
// The graphics consumers: gCloudShadowIdx / gCloudShadowRight / gCloudShadowUp
// are LightCB's globals (light_cb.hlsli). The sampler is passed in rather than
// assumed by name: consumers live in different shaders whose linear-clamp
// sampler is called different things (gSampler in Lighting.ps, g_Clamp in
// Water.ps), and plain linear is all this needs.
float SampleCloudShadow(float3 worldPos, SamplerState samp)
{
    return CloudShadowSample(worldPos, gCloudShadowIdx, gCloudShadowRight,
                             gCloudShadowUp, samp);
}
#endif

#endif // CLOUD_SHADOW_HLSLI
