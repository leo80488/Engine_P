// Optional custom-trail input: an immutable copy of the HDR scene taken right before trail drawing.
// t4/t5 are the ribbon geometry; t6 is the existing graphics root slot Gfx::SRV_t6.
#ifndef TRAIL_SCENE_HLSLI
#define TRAIL_SCENE_HLSLI
Texture2D<float4> g_TrailScene : register(t6, space0);

float3 TrailSceneAt(float2 pixel)
{
    uint w, h;
    g_TrailScene.GetDimensions(w, h);
    return g_TrailScene.Load(int3(clamp(int2(pixel), int2(0, 0), int2(w, h) - 1), 0)).rgb;
}

float4 TrailRefractedOutput(TrailPSIn i, float3 rgb, float opacity,
                           float2 offsetPixels, float weight)
{
    // Fogged and soft-faded like every trail (TrailOutput(i, ...)).
    float4 result = TrailOutput(i, rgb, opacity);
    // A signed HDR delta allows pure refraction (opacity = 0) under the premultiplied blend; a zero
    // offset is exactly a no-op, edges included. The scene copy is already fogged; the soft fade eases
    // the distortion out where the ribbon meets the opaque surface, as it does the colour.
    result.rgb += (TrailSceneAt(i.pos.xy + offsetPixels) - TrailSceneAt(i.pos.xy))
                * saturate(weight) * Fog_SoftFade(i.pos) * (1.0 - result.a);
    return result;
}
#endif
