#pragma once

// The names of the pass textures a render graph imports: the producer declares the import
// under one of these (RenderGraph::ImportTexture, or ImportDeclarer::Import from
// RenderPass::DeclareImports) and its readers look it up by the same constant in Setup
// (RenderGraphBuilder::GetTexture). Names say what the texture is, not which pass instance
// holds it: the player's and the hair's per-object maps are two instances of one class. A
// graph without the producer (a preview graph) has no such name, and its readers declare
// nothing.

namespace RG::PassTex
{
    // ShadowPass's cascade array (D32, one slice per cascade): recorded on
    // worker 0's shadowCL, outside and beside the graph - Fresh, Owner.
    inline constexpr const char* kShadowCSM = "Shadow.CSM";

    // SpotShadowPass's three tiers (D32 arrays; each spot light's slice and
    // tier reach the shaders through the spot VP buffer's atlasIndex).
    inline constexpr const char* kShadowSpotAtlas       = "Shadow.SpotAtlas";         // 512^2 x 256
    inline constexpr const char* kShadowSpotAtlasMedium = "Shadow.SpotAtlasMedium";   // 1024^2 x 16
    inline constexpr const char* kShadowSpotAtlasHigh   = "Shadow.SpotAtlasHigh";     // 2048^2 x 4
    // PointShadowPass's cube atlas (D32, 24 slices = 4 cubes).
    inline constexpr const char* kShadowPointCube = "Shadow.PointCube";
    // PerObjectShadowPass: the player's map, and the hair / bangs map of its
    // Variant::Hair instance (HairShadowPass).
    inline constexpr const char* kShadowPerObject = "Shadow.PerObject";
    inline constexpr const char* kShadowHair      = "Shadow.Hair";

    // The RT sun shadow mask (RTShadowPass), the AO the lighting reads - one
    // of RTAO's or XeGTAO's (the slot is shared; one producer runs) - and the
    // two RT reflection outputs (the opaque one and the translucent front).
    inline constexpr const char* kRTShadowMask      = "RTShadow.Mask";
    inline constexpr const char* kRTAO              = "RTAO.AO";
    inline constexpr const char* kXeGTAO            = "XeGTAO.AO";   // the history slot this frame writes
    // The slot written last (ImportDeclarer::ImportHistory's name + ".Prev").
    inline constexpr const char* kXeGTAOPrev        = "XeGTAO.AO.Prev";
    inline constexpr const char* kRTReflection      = "RTReflection.Denoised";
    inline constexpr const char* kRTReflectionFront = "RTReflectionFront.Denoised";
    // SSGIPass's result and the Hi-Z it builds (the opaque RTR traces it).
    inline constexpr const char* kSSGIResult = "SSGI.Result";
    inline constexpr const char* kSSGIHiZ    = "SSGI.HiZ";

    // CloudPass's shadow map: LightingPass, WaterPass and WaterSurfacePass run
    // before CloudPass and read LAST frame's (ReadHistory); the passes after
    // it read this frame's.
    inline constexpr const char* kCloudShadow = "Cloud.Shadow";
    // SceneVoxelPass's occupancy volume (R8_UINT 128^3), VolumetricFog's
    // occlusion; VolumetricFogPass's froxel scattering volume (an arena
    // placement), the forward passes' fog; SkyIBLPass's specular cube.
    inline constexpr const char* kSceneVoxelOccupancy = "SceneVoxel.Occupancy";
    inline constexpr const char* kVolFogScattering    = "VolFog.Scattering";
    inline constexpr const char* kSkySpecularCube     = "SkyIBL.SpecularCube";

    // The SSR subsystem's scene-colour pyramid and its result, written AFTER
    // the graph: History imports, read as last frame's (ReadHistory).
    inline constexpr const char* kSSRScenePyramid = "SSR.ScenePyramid";
    inline constexpr const char* kSSRResult       = "SSR.Result";
}
