#pragma once

// PostProcessConfig: a POD snapshot of the render features outside the post-process volume
// blend (AO, outline, volumetric fog, sky / IBL), saved as a line-based .ippc text file
// (.iscene / .ipfb conventions). A scene references an .ippc by path to restore them.
// The post-process look (CAS, exposure, bloom, tonemap, grading, lens flare) is not here: it is
// the engine-default PostProcessProfile (.ppprofile) the config names.
//   Save: cfg.CaptureFrom(renderer); SavePostProcessConfig(cfg, path);
//   Load: LoadPostProcessConfig(path, cfg); cfg.ApplyTo(renderer);
// A key missing from the file keeps its C++ default, so files written before a field existed
// still load.

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <string_view>

class Renderer;

namespace Resource
{
    struct PostProcessConfig
    {
        // ---- Engine-default PostProcessProfile (.ppprofile) ----
        // The scene's base post-process look (the volume blend's base); ApplyTo loads it into
        // PostProcess::ProfileSystem::EngineDefault().
        std::string engineProfilePath;

        // ---- XeGTAOPass ----
        bool     ssaoEnabled                = true;  // the scene's AO intent, capped by features.ssao
        float    gtaoEffectRadius           = 0.5f;
        float    gtaoEffectFalloff          = 2.0f;
        float    gtaoFinalValuePower        = 1.5f;
        float    gtaoSampleDistributionPower = 2.0f;
        float    gtaoThinOccluderCompensation = 0.05f;
        float    gtaoDenoiseSigma           = 1.0f;
        uint32_t gtaoSliceCount             = 3;
        uint32_t gtaoStepsPerSlice          = 3;

        // ---- OutlinePass ----
        float             outlinePixels     = 2.0f;
        DirectX::XMFLOAT3 outlineColor      { 0.0f, 0.0f, 0.0f };
        float             depthThreshold    = 0.05f;
        float             normalThreshold   = 0.3f;
        float             outlineStrength   = 1.0f;
        float             outlineFadeStart  = 15.0f;
        float             outlineFadeEnd    = 40.0f;

        // ---- VolumetricFogPass ----
        bool              volFogEnabled          = false;
        float             fogDensity             = 0.02f;
        float             fogScattering          = 0.6f;
        float             fogAbsorption          = 0.02f;
        float             fogAnisotropy          = 0.4f;
        float             heightFogStart         = 0.0f;
        float             heightFogFalloff       = 0.05f;
        float             froxelNear             = 0.5f;
        float             froxelFar              = 80.0f;
        DirectX::XMFLOAT3 fogAmbientColor        { 0.4f, 0.55f, 0.85f };
        float             fogAmbientContribution = 0.4f;
        bool              temporalEnabled        = true;
        float             temporalAlpha          = 0.12f;

        // ---- SkyIBLPass (time of day lives in TODConfigComponent) ----
        bool              atmosphereEnabled      = false;
        uint32_t          skyboxSource           = 0;    // 0 = Atmosphere, 1 = Static
        float             iblStrength            = 0.1f;
        bool              aerialCompositeEnabled = false;

        // Fills this struct from the renderer's live parameters; a pass that does not exist yet
        // (before Init) is skipped.
        void CaptureFrom(const Renderer& r);

        // Pushes the values onto the renderer's passes, skipping missing ones. The AO and fog switches
        // are the scene's intent: the player's features.ssao / volumetricFog cap them, and @p source
        // (the .ippc's path) names them in the settings store's Scene layer.
        void ApplyTo(Renderer& r, std::string_view source = "post-process config") const;
    };

    // For a scene without an .ippc: states the struct defaults (AO on, fog off) as its AO and fog
    // intent, so a previous scene's switches do not linger. Only those two; the other .ippc values
    // carry over from the previous scene.
    void ApplyNoConfigSceneIntents(Renderer& r);

    // Serialize a PostProcessConfig to `path` (.ippc text format).
    bool SavePostProcessConfig(const PostProcessConfig& cfg, const std::string& path);

    // Parses a .ippc file into `out`; missing keys keep their defaults. Returns false only on an
    // I/O failure; corrupt or missing fields are tolerated.
    bool LoadPostProcessConfig(const std::string& path, PostProcessConfig& out);
}
