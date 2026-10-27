// REBLUR OUT_DIFF_HITDIST -> the AO texture Lighting.ps samples (gSSAO t18).
// OUT_DIFF_HITDIST is already a [0,1] occlusion factor (1 = unoccluded); apply the artistic
// power curve and write it out.
//
// Root signature map, must stay in sync with RTAOPass.cpp:
//   [0] ROOT_CBV b0  RTAOConstants
//   [9]  TABLE   t4  gAoIn   (REBLUR OUT_DIFF_HITDIST, R8)
//   [10] TABLE   u4  OutAO   (R8 -> gSSAO t18, 1 = no occlusion)

cbuffer RTAOConstants : register(b0, space0)
{
    float4x4 invViewProj;
    uint     screenW;          uint  screenH;
    float    invScreenW;       float invScreenH;
    float    aoRadius;         float normalBias;
    uint     rayCount;         uint  frameIndex;
    float    nearZ;            float farZ;
    float    aoPower;          uint  alphaTestEnabled;
    uint     alphaInstanceCount; float alphaMipLevel;
};

Texture2D<float>   gAoIn : register(t4, space0);
RWTexture2D<float> OutAO : register(u4, space0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= screenW || pixel.y >= screenH)
        return;

    const float ao = saturate(gAoIn.Load(int3(pixel, 0)));
    OutAO[pixel] = pow(ao, max(aoPower, 1e-3));
}
