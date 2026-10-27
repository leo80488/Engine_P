#pragma once

// PreviewStudioRig - the one definition of the material preview's lighting and
// framing.
//
// The constants live here, not at the use sites, because the A/B parity harness
// renders the same sphere through the preview graph and through the main pipeline
// from a fixture scene and compares the images: that only means something if both
// sides read bit-identically the same rig. Do not copy these numbers elsewhere.
//
// The preview does not borrow the live scene's sun or time of day: a preview that
// changes with the clock cannot be compared against a golden. Everything here is a
// compile-time constant.
//
// Sun direction: LightCB.lightDir is the direction light TRAVELS, not the direction
// towards the light (Renderer_DDGI documents the flip against
// SkyIBLPass::GetSunDir). A key light above and left of the camera therefore has a
// lightDir pointing down and to the right.

#include <algorithm>   // std::clamp on the orbit pitch
#include <cmath>
#include <cstdint>

namespace Preview
{
    // ---- Framing ----
    // The subject is a unit-diameter primitive centred on the origin (Sphere radius
    // 0.5; Cube/Plane/Torus fit the same unit box), so the camera constants are shared
    // by every shape.
    struct StudioCamera
    {
        float fovY      = 0.60f;   // radians (~34.4 deg), narrow enough to avoid
                                   // wide-angle distortion on a sphere
        float distance  = 2.10f;   // metres from the origin
        float yaw       = 0.6981317f;  // +40 deg, three-quarter view
        float pitch     = 0.3490659f;  // +20 deg, looking slightly down
        float nearZ     = 0.05f;
        float farZ      = 50.0f;
    };
    inline constexpr StudioCamera kStudioCamera{};

    // Camera basis from kStudioCamera plus the panel's orbit deltas (yawOffset,
    // pitchOffset, distanceScale). PreviewRenderer::BuildPreviewCamera and App's
    // parity camera override both call this, so both parity sides share one eye and
    // forward.
    //
    // Left-handed, +Z forward, world up. outForward points from the camera at the
    // origin and outEye = -distance * outForward, so the subject is framed at any
    // orbit. Pitch is clamped short of the poles: at +-90 deg the look-to basis is
    // degenerate against world up and the view matrix comes out NaN.
    // panOffset (optional) shifts the eye and the framed point in world space without
    // changing the forward; nullptr = the unpanned basis.
    inline void StudioCameraBasis(float yawOffset, float pitchOffset,
                                  float distanceScale,
                                  float outEye[3], float outForward[3],
                                  const float* panOffset = nullptr)
    {
        const StudioCamera& sc = kStudioCamera;
        const float yaw   = sc.yaw + yawOffset;
        const float pitch = std::clamp(sc.pitch + pitchOffset, -1.45f, 1.45f);
        const float dist  = std::max(0.35f,
                                     sc.distance * std::max(0.05f, distanceScale));
        outForward[0] = std::cos(pitch) * std::sin(yaw);
        outForward[1] = -std::sin(pitch);
        outForward[2] = std::cos(pitch) * std::cos(yaw);
        outEye[0] = -dist * outForward[0];
        outEye[1] = -dist * outForward[1];
        outEye[2] = -dist * outForward[2];
        if (panOffset)
        {
            outEye[0] += panOffset[0];
            outEye[1] += panOffset[1];
            outEye[2] += panOffset[2];
        }
    }

    // Distance scale used only by the A/B parity run: the subject fills the frame, so
    // the comparison is 100% shaded pixels. The backdrops cannot be matched - the
    // preview's Solid backdrop is its clear colour (SkyboxPass#2 draws nothing for it),
    // while the main pipeline paints the sky cube (with an empty skyboxPath, the
    // radiance cube) - and turning the main sky mode off (None / FlatColor) would also
    // turn off the IBL block the test checks.
    //
    // Arithmetic for the unit-diameter sphere, fovY = 0.60 rad, square viewport: the
    // corner half-angle is atan(sqrt(2) * tan(fovY/2)) = 0.4123 rad, so covering the
    // corners needs d < 0.5 / sin(0.4123) = 1.2475 m. At 0.5 x 2.10 = 1.05 m the
    // sphere clears the corner by ~20%, so a tweak to fovY or the radius fails a pack
    // instead of growing a background wedge. The near plane (0.05) is well in front of
    // the surface (0.55 m), and with no silhouette in frame areaFracMax measures
    // shading disagreement only.
    inline constexpr float kParityDistanceScale = 0.5f;

    // ---- Lights ----
    // dir = TRAVEL direction (see the file header). Not normalised here on purpose:
    // the consumer normalises, and hand-normalised literals drift.
    struct StudioLight
    {
        float dir[3];
        float color[3];   // already multiplied by intensity
    };

    // KEY - above/behind the camera's left shoulder, travelling down-right-forward.
    // The ONE analytic directional light Lighting.ps has (LightCB.lightDir /
    // lightColor). Fill and back are point lights; see below.
    inline constexpr StudioLight kKeyLight{
        { 0.45f, -0.75f, 0.48f },
        { 3.10f,  3.05f,  2.90f }
    };

    // ---- Fill + back: clustered point lights ----
    // Not a constant ambient term: ambient has no direction, so Subsurface
    // transmission and TwoSidedFoliage's transmitted glow would see no back light.
    // They are point lights because Lighting.ps has exactly one analytic directional;
    // everything else arrives through the clustered block (gLights / gLightIndexList /
    // gLightGrid at t13/t14/t15), which PreviewRenderer builds itself (see
    // PreviewLightGrid for why its grid is uniform rather than culled).
    //
    // intensity is not in kKeyLight.color units: AccumClusteredLight applies 1/d^2
    // attenuation inside the range window, so the radiance at the subject is
    // ~color * intensity / distance^2. intensity = distance^2 (9.0 at 3 m) makes
    // `color` the delivered radiance and keeps the two fields independently editable.
    struct StudioPointLight
    {
        float dir[3];      // TRAVEL direction, as StudioLight - the light sits at
                           // -normalize(dir) * distance and shines at the origin.
        float color[3];    // linear radiance delivered at the subject
        float intensity;   // ~distance^2, see above
        float distance;    // metres from the origin
        float radius;      // range window; must comfortably exceed distance + 0.5
    };

    // FILL - opposite side from the key, dimmer and cooler, lifts the terminator.
    inline constexpr StudioPointLight kFillLight{
        { -0.70f, -0.25f, 0.55f },
        {  0.55f,  0.62f,  0.78f },
        9.0f, 3.0f, 12.0f
    };
    // BACK / RIM - behind the subject travelling towards the camera. Required:
    // TwoSidedFoliage transmission and the Subsurface wrap term are invisible without
    // a light on the far side. It sits at ~(-0.30, -1.03, +2.80), opposite the camera
    // eye at ~(-1.27, +0.72, -1.51).
    inline constexpr StudioPointLight kBackLight{
        { 0.10f,  0.35f, -0.95f },
        { 1.20f,  1.15f,  1.30f },
        9.0f, 3.0f, 12.0f
    };

    // ---- Ambient stand-in ----
    // Read only when Feature::kAmbientFill (a compatibility bit, off by default) is
    // set, and nothing sets it. Delete these two together with the bit.
    inline constexpr float kAmbientColor[3]  = { 0.29f, 0.31f, 0.36f };
    inline constexpr float kAmbientIntensity = 1.0f;

    // ---- Studio key-light shadow ----
    // PreviewStudioShadowPass renders the preview draw list from the key light into a
    // 512^2 reversed-Z depth slice and hands LightingPass#2 a one-cascade CSM (see that
    // pass for why cascadeSplits are pinned far and shadowBlendRange is 0). The fit is
    // a fixed ortho box around the unit subject, not a camera-fitted cascade, which is
    // why this is not a ShadowPass instance.
    inline constexpr unsigned kStudioShadowSize   = 512;
    // Half-extent of the ortho box, in metres. The unit primitive fits a 0.5 m sphere
    // and a mesh override is normalised into the same unit box by the draw list, so
    // 1.6 m leaves ~3x margin at 512/3.2 = 160 texels per metre.
    inline constexpr float    kStudioShadowFitRadius = 1.6f;
    // Receiver-side comparison bias in NDC. Reversed-Z: RAISING the reference
    // biases toward LIT. Same units as LightCB.shadowBias on the main path.
    inline constexpr float    kStudioShadowBias      = 0.0015f;
    // Normal-offset strength, in texels of this map. The main path's default is
    // 2.0; a 512^2 map over 3.2 m has ~7x the world texel size of a 4096^2
    // cascade over the same area, so a smaller number gives the same visual push.
    inline constexpr float    kStudioShadowNormalOffset = 1.5f;
    inline constexpr float    kStudioShadowStrength     = 1.0f;

    // ---- Environment ----
    // A shipped engine IBL triple rather than a dedicated studio HDRI (that would be
    // content work). autumn_field_puresky is the most neutral of the three that ship:
    // the others are a dusk and a night sky, which tint every metal preview.
    // tools/package_game.py ships the files of an engine library (engine/ibl/<sky>/) that
    // engine source names by a literal path, so these two are in every package too.
    inline constexpr const char* kIrradiancePath =
        "engine/ibl/autumn_field_puresky/autumn_field_puresky_Irradiance.itex";
    inline constexpr const char* kRadiancePath =
        "engine/ibl/autumn_field_puresky/autumn_field_puresky_Radiance.itex";
    // No kSkyboxPath, deliberately: the preview binds no backdrop, and a parity
    // fixture with an empty skyboxPath is painted with the radiance cube (Renderer_IBL
    // falls back skybox -> radiance -> irradiance), not black. The parity run frames
    // the backdrop out instead (kParityDistanceScale).
    // Mip count of kRadiancePath's prefiltered roughness chain, read off the file
    // (1024x1024, 5 mips). SkyboxComponent::radianceMipLevels defaults to 7 and the
    // main pipeline publishes that into LightCB.iblRadianceMips, while the preview
    // reads the real count off the texture: a parity fixture must use this value or
    // every rough metal pixel disagrees. Renderer::SyncPreviewEnvironment logs the
    // count it read ("[PREVIEW] studio IBL ready: radiance mips N") and the parity
    // pack asserts it, so a re-baked HDRI cannot drift from this constant.
    inline constexpr unsigned kRadianceMipLevels = 5;
    // The engine-global split-sum LUT. t8 has no fallback anywhere, so the indirect
    // path must stay off until this resolves.
    inline constexpr const char* kBRDFLUTPath = "engine/ibl/BRDF_LUT.itex";

    inline constexpr float kIBLStrength      = 1.0f;

    // ==== Secondary-view lights parity fixture ====
    // A second fixture family comparing a different pipeline pair. The constants above
    // describe the synthetic lane (lane 0: material inspector, Solid backdrop, studio
    // rig, camera from StudioCameraBasis) that -preview_parity compares with the main
    // pipeline. This one is the WorldCull lane (lane 1: a second camera on the same
    // world, with a SkyboxPass, a copied LightCB and a borrowed CSM).
    //
    // Three arms, and arm 0 is not optional:
    //   0 baseline  the same world with zero punctual lights: the main-vs-WorldCull
    //               floor, without which arm 1's number says nothing about lights.
    //   1 on        the same world with the punctual lights below.
    //   2 off       the same run with Preview.SetWorldSceneLights(false) (or
    //               -preview_pip_lights=0); must differ from arm 1 by orders of
    //               magnitude.
    //
    // EditorLayer's world builder reads this table to spawn the entities and App reads
    // kLightsParityCamera to pin the main camera to the PiP camera's eye and lens, so
    // both sides use the same numbers.
    //
    // No directional light, structurally: a secondary view borrows the main camera's
    // cascades, so a directional shadow outside the two frusta's intersection would be
    // wrong on one side only. With none, LightCB.lightColor is zero on both sides and
    // the borrowed CSM contributes nothing.
    // TODO(MultiViewport_MaterialPreview_Plan NF-2): per-view CSM for secondary WorldCull views.
    struct LightsParityCamera
    {
        float eye[3];
        float forward[3];
        float fovY;
        float nearZ;
        float farZ;
    };
    // Looking down +Z from behind the origin, slightly above the floor. The floor and
    // the back plane between them cover every pixel: the preview's SkyboxPass#3 forces
    // sun disk, moon and stars off while the main pipeline draws them as authored, so
    // any sky pixel in frame is a disagreement unrelated to lights. At this eye and
    // fov the frustum bottom meets the floor at z = -0.7 and the back plane covers the
    // rest out to the far edge.
    inline constexpr LightsParityCamera kLightsParityCamera{
        { 0.0f, 1.6f, -4.0f },
        { 0.0f, 0.0f,  1.0f },
        0.90f, 0.10f, 100.0f
    };
    // The room: a floor slab and a back wall, both the primitive CUBE. Not planes: a
    // rotated Plane produced no draw packet, so the wall went missing while the
    // comparison still read bit-identical. A closed solid has a non-degenerate AABB and
    // no orientation question, and it is what BaseScene draws with.
    // The floor's top face is at y = 0 and the wall's front face at z = 6.5; both are
    // sized so the frustum never reaches an edge, which keeps sky out of frame.
    inline constexpr float kLightsParityFloorY     = -0.5f;  // centre; top face at 0
    inline constexpr float kLightsParityFloorScale[3] = { 30.0f, 1.0f, 30.0f };
    inline constexpr float kLightsParityFloorZ     = 1.0f;
    inline constexpr float kLightsParityWallScale[3]  = { 30.0f, 24.0f, 1.0f };
    inline constexpr float kLightsParityWallY      = 6.0f;
    inline constexpr float kLightsParityWallZ      = 7.0f;   // centre; front face at 6.5

    // The punctual lights of arm 1: four points and two spots, all inside the camera
    // frustum and well under Preview::kMaxPreviewLights, so the cap drops nothing. The
    // cap's behaviour belongs to the real-scene pack; a dropped light here would make
    // the floor measure the importance heuristic instead of the carrier.
    //
    // castsShadow is false on every one: the preview binds SetSpotShadowAtlas(0,0) /
    // SetPointShadowAtlas(0) and forces shadowSliceIdx to 0xFFFFFFFF, so a
    // shadow-casting light would be shadowed on the main side only. That difference is
    // real and shown in the camera panel's tooltip, but it is not what this fixture
    // measures; folding it into the parity threshold would hide it.
    struct LightsParityLight
    {
        float    position[3];
        float    color[3];
        float    intensity;   // ~distance^2, the same convention StudioPointLight uses
        float    radius;
        bool     spot;
        float    direction[3];  // TRAVEL direction; spot only
        float    spotAngle;     // half-angle, radians; spot only
    };
    inline constexpr LightsParityLight kLightsParityLights[] = {
        // Four points strung between the camera and the back plane, at
        // different heights and hues so a missing one is visible rather than
        // averaged away.
        { { -2.2f, 1.2f, 0.5f }, { 1.00f, 0.35f, 0.25f }, 6.0f, 6.0f, false, { 0.f, 0.f, 0.f }, 0.f },
        { {  2.2f, 1.2f, 0.5f }, { 0.25f, 0.45f, 1.00f }, 6.0f, 6.0f, false, { 0.f, 0.f, 0.f }, 0.f },
        { { -1.4f, 2.6f, 3.4f }, { 0.35f, 1.00f, 0.40f }, 8.0f, 7.0f, false, { 0.f, 0.f, 0.f }, 0.f },
        { {  1.4f, 0.4f, 3.4f }, { 1.00f, 0.90f, 0.40f }, 8.0f, 7.0f, false, { 0.f, 0.f, 0.f }, 0.f },
        // Two spots aimed down at the floor from either side: AccumClusteredLight's cone
        // test is a separate branch from the point path, which points alone leave
        // uncovered.
        { { -3.0f, 3.2f, 2.0f }, { 1.00f, 1.00f, 0.95f }, 16.0f, 9.0f, true,
          {  0.55f, -0.82f, 0.15f }, 0.55f },
        { {  3.0f, 3.2f, 2.0f }, { 0.95f, 1.00f, 1.00f }, 16.0f, 9.0f, true,
          { -0.55f, -0.82f, 0.15f }, 0.55f },
    };
    inline constexpr unsigned kLightsParityLightCount =
        sizeof(kLightsParityLights) / sizeof(kLightsParityLights[0]);
    // The camera ENTITY's name, which -preview_pip resolves by. One spelling,
    // read by the world builder and typed into the packs.
    inline constexpr const char* kLightsParityCameraName = "LightPiPCam";
}
