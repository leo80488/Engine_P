#ifndef FACE_SDF_HLSLI
#define FACE_SDF_HLSLI

// face_sdf.hlsli - the shared contract for the anime/NPR "SDF face shadow".
//
// The SDF terminator is per light: the threshold (dot(-L_xz, faceFront_xz)) and the
// mirror half of the lightmap sampled (sign of dot(L_xz, faceRight_xz)) both depend
// on the light direction. The GBuffer PS has the face UV but no light list; the
// deferred lighting PS has the lights but no UV. So the GBuffer stores the two raw
// mirror halves and the deferred pass resolves one terminator per light. Keep the
// shared maths here so the two sides cannot drift apart.
//
// ==== RT4 ("extra") channel budget ====
//   .r  face SDF, mirror half used when dot(L_xz, faceRight_xz) >= 0   [v2]
//       (v1 producers put the already-resolved main-light terminator here)
//   .g  bangs/hair screen mark, stamped on face pixels by HairShadowMarkPass
//       (that pass writes green only: do not repurpose it)
//   .b  face SDF, mirror half used when dot(L_xz, faceRight_xz) <  0   [v2]
//       Shared with FaceOverlay's hair mask on disjoint pixels: the hair mask is
//       written by hair materials and read on hair pixels, this one is written by
//       face materials and read only under a face marker. Do not assume .b is free
//       on face pixels.
//   .a  marker / version (below)
//
// ==== Marker versions ====
// Users author their own face shaders (asset/EngineResource/**/char/*.ps.hlsl), so
// the deferred side reads the layout version from the marker:
//
//   0.0  not a face pixel - deferred uses the NdotL toon term.
//   1.0  V1: .r holds a terminator already resolved against the main directional
//        light. Point/spot lights cannot use it (it encodes the sun's yaw) and fall
//        back to the NdotL toon term. Unported face shaders write this.
//   2.0  V2: .r/.b hold the two raw mirror halves; every light resolves its own
//        terminator. The deferred pass reads the parameters from MaterialGPUData, so
//        the material cbuffer (b8 -> customParams) must be:
//            customParams[0].xyz = HeadForward (world face front)
//            customParams[1].xyz = HeadRight   (world face right)
//            customParams[2]     = (shadowOffset, sharpness, strength, uvSet)
//        A v2 producer with another layout shades with garbage. The material packer
//        copies the face shadow into MaterialGPUData::faceShadow from the member a
//        shader names `@engine faceShadow` (else customParams[2]), so that one may sit
//        anywhere; the head frame is still positional.
//   3.0  V2-AZIMUTH: same storage and cbuffer contract as 2.0, but the per-light
//        threshold is the linear atan2 azimuth sweep (see FaceSdfTerminator). For SDFs
//        authored against an atan2/PI reference such as the Arknights: Endfield
//        `calculateAngel` group: a cosine threshold eases near the front/back poles
//        and the shadow "sticks" when the key light passes the nose.
//
// Compare with `>= 1.5` etc., not `==`: RT4 is R16G16B16A16_FLOAT and the deferred
// fetch is a linear Sample, so a pixel on a face/non-face edge can land between two
// marker values.
#define FACE_TERM_NONE          0.0
#define FACE_TERM_BAKED         1.0
#define FACE_TERM_SDF_V2        2.0
#define FACE_TERM_SDF_V2_AZIM   3.0

// Sentinel stored in place of the SDF for texels the area mask (lightmap alpha)
// excludes from the sweep: those are shadowed for every light. Real SDF samples are
// >= 0, so any negative is unambiguous.
// The consumer must branch on it, not push a large negative through the sigmoid: at
// low sharpness (users can dial it to ~0) the sigmoid never reaches 0 and the
// excluded region would light up.
#define FACE_SDF_NO_AREA  (-1.0)

float FaceSdfSigmoid(float x, float center, float sharpness)
{
    return 1.0 / (1.0 + exp(-sharpness * (x - center)));
}

float2 FaceSdfNormalize2(float2 v) { return v / max(1e-5, length(v)); }

// Resolve the terminator (1 = lit) for one light from the two stored halves.
//
//   sdfRight / sdfLeft  RT4 .r / .b - raw halves, or FACE_SDF_NO_AREA
//   faceFrontXZ         normalized head forward flattened to world XZ
//   faceRightXZ         normalized head right   flattened to world XZ
//   dirTowardLight      unit vector from the surface toward the light
//                       (directional: -lightDir; point/spot: normalize(pos - P))
//   shadowOffset/Sharpness/Strength   MaterialGPUData::faceShadow.xyz
//   azimuth             which threshold sweep the SDF was authored against
//
// The `* 10.0` on sharpness and the saturate() on the cosine threshold are part of
// the authored parameter range: keep them so ported materials render identically
// under the main light.
//
//   azimuth = false (marker 2.0, the miHoYo/Genshin form):
//       threshold = saturate(dot(-L_xz, front_xz) * 0.5 + shadowOffset)
//     A cosine of the light yaw in [-0.5,0.5]+offset; 0.5 is the neutral offset.
//
//   azimuth = true (marker 3.0, the Arknights: Endfield `calculateAngel` form):
//       t         = atan2(dot(L_xz, right_xz), dot(-L_xz, front_xz)) / PI
//       threshold = (1 - abs(t)) + shadowOffset
//     Linear in azimuth: 0 with the light dead ahead, 1 dead behind, 0.5 at 90 deg.
//     Not saturated on purpose: a threshold above 1 means no texel can be lit (a
//     light behind the head) and the sigmoid takes it to 0 without a discontinuity.
//     shadowOffset is an additive bias on [0,1] here; its neutral value is 0.
//
// Both modes share the sigmoid, so shadowSharpness means the same in either
// (engine sharpness = 3.454 x a DanbaidongRP/Endfield `sharp`).
float FaceSdfTerminator(float sdfRight, float sdfLeft,
                        float2 faceFrontXZ, float2 faceRightXZ,
                        float3 dirTowardLight,
                        float shadowOffset, float shadowSharpness, float shadowStrength,
                        bool azimuth)
{
    float2 LxzN     = FaceSdfNormalize2(dirTowardLight.xz);
    float  dotRight = dot(LxzN, faceRightXZ);
    float  sdf      = (dotRight < 0.0) ? sdfLeft : sdfRight;

    float lit;
    if (sdf < 0.0)
    {
        lit = 0.0;                      // outside the sweep area -> always shadowed
    }
    else
    {
        float dotFront = dot(-LxzN, faceFrontXZ);
        float threshold;
        if (azimuth)
        {
            // atan2 is well defined here: LxzN is a normalized 2-D direction, so
            // (dotRight, dotFront) is never (0,0).
            float t   = atan2(dotRight, dotFront) * (1.0 / 3.14159265);   // [-1,1]
            threshold = (1.0 - abs(t)) + shadowOffset;
        }
        else
        {
            threshold = saturate(dotFront * 0.5 + shadowOffset);
        }
        lit = FaceSdfSigmoid(sdf, threshold, shadowSharpness * 10.0);
    }
    return lerp(1.0, lit, shadowStrength);   // strength 0 -> always lit
}

// Overload without azimuth: the cosine sweep.
float FaceSdfTerminator(float sdfRight, float sdfLeft,
                        float2 faceFrontXZ, float2 faceRightXZ,
                        float3 dirTowardLight,
                        float shadowOffset, float shadowSharpness, float shadowStrength)
{
    return FaceSdfTerminator(sdfRight, sdfLeft, faceFrontXZ, faceRightXZ,
                             dirTowardLight, shadowOffset, shadowSharpness,
                             shadowStrength, false);
}

#endif // FACE_SDF_HLSLI
