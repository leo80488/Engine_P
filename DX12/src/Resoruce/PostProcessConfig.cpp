#include "Resource/PostProcessConfig.h"
#include "Resource/AssetFS.h"

#include "Renderer/Renderer.h"
#include "PostProcess/ProfileSystem.h"
#include "PostProcess/PostProcessProfileSerializer.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace DirectX;

namespace
{
    // Tiny line parser: each non-comment line is "Tag key=val key=val ...". A value is one
    // whitespace-free token; a path is percent-encoded.
    struct KV
    {
        std::string tag;
        std::unordered_map<std::string, std::string> map;
    };

    float GetF(const std::unordered_map<std::string, std::string>& m,
               const char* key, float def)
    {
        auto it = m.find(key);
        if (it == m.end()) return def;
        try { return std::stof(it->second); } catch (...) { return def; }
    }
    int GetI(const std::unordered_map<std::string, std::string>& m,
             const char* key, int def)
    {
        auto it = m.find(key);
        if (it == m.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }
    bool GetB(const std::unordered_map<std::string, std::string>& m,
              const char* key, bool def)
    {
        return GetI(m, key, def ? 1 : 0) != 0;
    }
    XMFLOAT3 GetF3(const std::unordered_map<std::string, std::string>& m,
                   const char* key, XMFLOAT3 def)
    {
        auto it = m.find(key);
        if (it == m.end()) return def;
        XMFLOAT3 v = def;
        sscanf_s(it->second.c_str(), "%f_%f_%f", &v.x, &v.y, &v.z);
        return v;
    }

    // Percent-encodes '%' and ' ' so a path survives the whitespace-tokenized line parser (the
    // SceneSerializer convention).
    std::string PercentEncode(const std::string& s)
    {
        std::string o;
        for (char c : s)
        {
            if (c == '%')      o += "%25";
            else if (c == ' ') o += "%20";
            else               o += c;
        }
        return o;
    }
    std::string PercentDecode(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size())
            {
                const std::string h = s.substr(i + 1, 2);
                if (h == "25") { o += '%'; i += 2; continue; }
                if (h == "20") { o += ' '; i += 2; continue; }
            }
            o += s[i];
        }
        return o;
    }
    std::string GetStr(const std::unordered_map<std::string, std::string>& m,
                       const char* key, const std::string& def)
    {
        auto it = m.find(key);
        return (it == m.end()) ? def : PercentDecode(it->second);
    }

    bool ParseLine(const std::string& line, KV& out)
    {
        std::istringstream ls(line);
        if (!(ls >> out.tag)) return false;
        std::string tok;
        while (ls >> tok)
        {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            out.map[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
        return true;
    }
}

// ==== Capture / Apply ====

void Resource::PostProcessConfig::CaptureFrom(const Renderer& rc)
{
    Renderer& r = const_cast<Renderer&>(rc); // accessors are non-const

    // Not captured here: the post-process look is the engine-default PostProcessProfile, saved to its
    // own .ppprofile.

    if (auto* gt = r.GetXeGTAOPass())
    {
        gtaoEffectRadius             = gt->effectRadius;
        gtaoEffectFalloff            = gt->effectFalloffRange;   // holds effectFalloffRange, [0,1]
        gtaoFinalValuePower          = gt->finalValuePower;
        gtaoSampleDistributionPower  = gt->sampleDistributionPower;
        gtaoThinOccluderCompensation = gt->thinOccluderCompensation;
        gtaoDenoiseSigma             = gt->denoiseBlurBeta;      // holds denoiseBlurBeta
        gtaoSliceCount               = gt->sliceCount;
        gtaoStepsPerSlice            = gt->stepsPerSlice;
    }
    // The scene's AO switch, not what runs: the player's features.ssao caps it, and a scene saved
    // while the player has AO off must still ask for AO.
    ssaoEnabled = r.GetSSAOSceneIntent();

    if (auto* ol = r.GetOutlinePass())
    {
        outlinePixels    = ol->outlinePixels;
        outlineColor     = { ol->outlineColor[0], ol->outlineColor[1], ol->outlineColor[2] };
        depthThreshold   = ol->depthThreshold;
        normalThreshold  = ol->normalThreshold;
        outlineStrength  = ol->outlineStrength;
        outlineFadeStart = ol->outlineFadeStart;
        outlineFadeEnd   = ol->outlineFadeEnd;
    }

    if (auto* vf = r.GetVolumetricFogPass())
    {
        volFogEnabled          = vf->IsEnabled();
        fogDensity             = vf->GetDensity();
        fogScattering          = vf->GetScattering();
        fogAbsorption          = vf->GetAbsorption();
        fogAnisotropy          = vf->GetAnisotropy();
        heightFogStart         = vf->GetHeightStart();
        heightFogFalloff       = vf->GetHeightFalloff();
        froxelNear             = vf->GetRangeNear();
        froxelFar              = vf->GetRangeFar();
        fogAmbientContribution = vf->GetAmbientContribution();
        temporalEnabled        = vf->IsTemporalEnabled();
        temporalAlpha          = vf->GetTemporalAlpha();
        // The ambient colour has no getter: fogAmbientColor keeps its current value.
    }

    if (auto* sk = r.GetSkyIBLPass())
    {
        atmosphereEnabled      = sk->IsAtmosphereEnabled();
        skyboxSource           = static_cast<uint32_t>(sk->GetSkyboxSource());
        iblStrength            = sk->GetIBLStrength();
        aerialCompositeEnabled = sk->IsAerialCompositeEnabled();
    }
}

void Resource::ApplyNoConfigSceneIntents(Renderer& r)
{
    const PostProcessConfig defaults;
    const std::string_view writer = "no .ippc (its defaults)";
    r.SetSSAOSceneIntent(defaults.ssaoEnabled, writer);
    r.SetFogSceneConfigIntent(defaults.volFogEnabled, writer);
}

void Resource::PostProcessConfig::ApplyTo(Renderer& r, std::string_view source) const
{
    // Rebuild the engine-default profile in layers, like a post-process volume: the shipping
    // defaults, the player's postProcess.* toggles, then the file's overrides. A property the
    // scene's profile sets wins; one it does not set keeps the player's value.
    if (!engineProfilePath.empty())
    {
        auto& ps  = PostProcess::ProfileSystem::Get();
        auto& def = ps.EngineDefault();
        def = PostProcess::MakeEngineDefaultProfile();
        ps.ApplyAuthoredTo(def);
        PostProcess::LoadProfile(engineProfilePath, def);
    }

    // Scene intent, capped in the Renderer by the player's features.ssao / volumetricFog.
    r.SetSSAOSceneIntent(ssaoEnabled, source);
    r.SetFogSceneConfigIntent(volFogEnabled, source);
    if (auto* gt = r.GetXeGTAOPass())
    {
        gt->effectRadius              = gtaoEffectRadius;
        gt->effectFalloffRange        = gtaoEffectFalloff;
        gt->finalValuePower           = gtaoFinalValuePower;
        gt->sampleDistributionPower   = gtaoSampleDistributionPower;
        gt->thinOccluderCompensation  = gtaoThinOccluderCompensation;
        gt->denoiseBlurBeta           = gtaoDenoiseSigma;
        gt->sliceCount                = gtaoSliceCount;
        gt->stepsPerSlice             = gtaoStepsPerSlice;
    }

    if (auto* ol = r.GetOutlinePass())
    {
        ol->outlinePixels    = outlinePixels;
        ol->outlineColor[0]  = outlineColor.x;
        ol->outlineColor[1]  = outlineColor.y;
        ol->outlineColor[2]  = outlineColor.z;
        ol->depthThreshold   = depthThreshold;
        ol->normalThreshold  = normalThreshold;
        ol->outlineStrength  = outlineStrength;
        ol->outlineFadeStart = outlineFadeStart;
        ol->outlineFadeEnd   = outlineFadeEnd;
    }

    if (auto* vf = r.GetVolumetricFogPass())
    {
        // The on/off switch is SetFogSceneConfigIntent, set before the passes.
        vf->SetDensity(fogDensity);
        vf->SetScattering(fogScattering);
        vf->SetAbsorption(fogAbsorption);
        vf->SetAnisotropy(fogAnisotropy);
        vf->SetHeight(heightFogStart, heightFogFalloff);
        vf->SetRange(froxelNear, froxelFar);
        vf->SetAmbient(fogAmbientColor, fogAmbientContribution);
        vf->SetTemporalEnabled(temporalEnabled);
        vf->SetTemporalAlpha(temporalAlpha);
    }

    if (auto* sk = r.GetSkyIBLPass())
    {
        sk->SetAtmosphereEnabled(atmosphereEnabled);
        sk->SetSkyboxSource(static_cast<SkyIBLPass::SkyboxSource>(skyboxSource));
        sk->SetIBLStrength(iblStrength);
        sk->SetAerialCompositeEnabled(aerialCompositeEnabled);
    }
}

// ==== Save / Load ====

bool Resource::SavePostProcessConfig(const PostProcessConfig& c, const std::string& path)
{
    std::ostringstream ss;
    ss << "# DX12 Engine Post-Process Config (.ippc)\n";
    ss << "PPC version=1\n";

    char buf[512];

    if (!c.engineProfilePath.empty())
        ss << "Profile path=" << PercentEncode(Sys::RelativizeAssetPath(c.engineProfilePath)) << "\n";

    snprintf(buf, sizeof(buf),
        "GTAO enabled=%u radius=%.6f falloff=%.6f finalPower=%.6f"
        " distPower=%.6f thinComp=%.6f denoise=%.6f slices=%u steps=%u\n",
        c.ssaoEnabled ? 1u : 0u,
        c.gtaoEffectRadius, c.gtaoEffectFalloff, c.gtaoFinalValuePower,
        c.gtaoSampleDistributionPower, c.gtaoThinOccluderCompensation,
        c.gtaoDenoiseSigma, c.gtaoSliceCount, c.gtaoStepsPerSlice);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "Outline pixels=%.6f color=%.6f_%.6f_%.6f"
        " depthThr=%.6f normalThr=%.6f strength=%.6f"
        " fadeStart=%.6f fadeEnd=%.6f\n",
        c.outlinePixels,
        c.outlineColor.x, c.outlineColor.y, c.outlineColor.z,
        c.depthThreshold, c.normalThreshold, c.outlineStrength,
        c.outlineFadeStart, c.outlineFadeEnd);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "VolFog enabled=%u density=%.6f scattering=%.6f absorption=%.6f"
        " anisotropy=%.6f heightStart=%.6f heightFalloff=%.6f"
        " froxelNear=%.6f froxelFar=%.6f ambient=%.6f_%.6f_%.6f"
        " ambientContrib=%.6f temporalEnabled=%u temporalAlpha=%.6f\n",
        c.volFogEnabled ? 1u : 0u,
        c.fogDensity, c.fogScattering, c.fogAbsorption, c.fogAnisotropy,
        c.heightFogStart, c.heightFogFalloff,
        c.froxelNear, c.froxelFar,
        c.fogAmbientColor.x, c.fogAmbientColor.y, c.fogAmbientColor.z,
        c.fogAmbientContribution,
        c.temporalEnabled ? 1u : 0u, c.temporalAlpha);
    ss << buf;

    snprintf(buf, sizeof(buf),
        "SkyIBL atmosphereEnabled=%u skyboxSource=%u"
        " iblStrength=%.6f aerialComposite=%u\n",
        c.atmosphereEnabled ? 1u : 0u, c.skyboxSource,
        c.iblStrength,
        c.aerialCompositeEnabled ? 1u : 0u);
    ss << buf;

    std::ofstream f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f) { LOG_ERROR("PostProcessConfig: cannot write '%s'", path.c_str()); return false; }
    const std::string text = ss.str();
    f.write(text.c_str(), static_cast<std::streamsize>(text.size()));
    LOG_SUCCESS("PostProcessConfig: saved '%s' (%zu bytes)", path.c_str(), text.size());
    return true;
}

bool Resource::LoadPostProcessConfig(const std::string& path, PostProcessConfig& out)
{
    std::vector<uint8_t> bytes;
    if (!::Resource::AssetFS::Get().ReadFile(path, bytes))
    { LOG_WARNING("PostProcessConfig: cannot open '%s'", path.c_str()); return false; }

    std::istringstream f(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        KV kv;
        if (!ParseLine(line, kv)) continue;
        const auto& m = kv.map;

        if (kv.tag == "Profile")
        {
            out.engineProfilePath =
                Sys::RelativizeAssetPath(GetStr(m, "path", out.engineProfilePath));
        }
        else if (kv.tag == "GTAO")
        {
            out.ssaoEnabled                 = GetB(m, "enabled",     out.ssaoEnabled);
            out.gtaoEffectRadius            = GetF(m, "radius",      out.gtaoEffectRadius);
            out.gtaoEffectFalloff           = GetF(m, "falloff",     out.gtaoEffectFalloff);
            out.gtaoFinalValuePower         = GetF(m, "finalPower",  out.gtaoFinalValuePower);
            out.gtaoSampleDistributionPower = GetF(m, "distPower",   out.gtaoSampleDistributionPower);
            out.gtaoThinOccluderCompensation= GetF(m, "thinComp",    out.gtaoThinOccluderCompensation);
            out.gtaoDenoiseSigma            = GetF(m, "denoise",     out.gtaoDenoiseSigma);
            out.gtaoSliceCount              = static_cast<uint32_t>(GetI(m, "slices", out.gtaoSliceCount));
            out.gtaoStepsPerSlice           = static_cast<uint32_t>(GetI(m, "steps",  out.gtaoStepsPerSlice));
        }
        else if (kv.tag == "Outline")
        {
            out.outlinePixels    = GetF(m, "pixels",    out.outlinePixels);
            out.outlineColor     = GetF3(m, "color",    out.outlineColor);
            out.depthThreshold   = GetF(m, "depthThr",  out.depthThreshold);
            out.normalThreshold  = GetF(m, "normalThr", out.normalThreshold);
            out.outlineStrength  = GetF(m, "strength",  out.outlineStrength);
            out.outlineFadeStart = GetF(m, "fadeStart", out.outlineFadeStart);
            out.outlineFadeEnd   = GetF(m, "fadeEnd",   out.outlineFadeEnd);
        }
        else if (kv.tag == "VolFog")
        {
            out.volFogEnabled          = GetB(m, "enabled",      out.volFogEnabled);
            out.fogDensity             = GetF(m, "density",      out.fogDensity);
            out.fogScattering          = GetF(m, "scattering",   out.fogScattering);
            out.fogAbsorption          = GetF(m, "absorption",   out.fogAbsorption);
            out.fogAnisotropy          = GetF(m, "anisotropy",   out.fogAnisotropy);
            out.heightFogStart         = GetF(m, "heightStart",  out.heightFogStart);
            out.heightFogFalloff       = GetF(m, "heightFalloff",out.heightFogFalloff);
            out.froxelNear             = GetF(m, "froxelNear",   out.froxelNear);
            out.froxelFar              = GetF(m, "froxelFar",    out.froxelFar);
            out.fogAmbientColor        = GetF3(m, "ambient",     out.fogAmbientColor);
            out.fogAmbientContribution = GetF(m, "ambientContrib", out.fogAmbientContribution);
            out.temporalEnabled        = GetB(m, "temporalEnabled", out.temporalEnabled);
            out.temporalAlpha          = GetF(m, "temporalAlpha",   out.temporalAlpha);
        }
        else if (kv.tag == "SkyIBL")
        {
            out.atmosphereEnabled      = GetB(m, "atmosphereEnabled", out.atmosphereEnabled);
            out.skyboxSource           = static_cast<uint32_t>(GetI(m, "skyboxSource", out.skyboxSource));
            out.iblStrength            = GetF(m, "iblStrength",      out.iblStrength);
            out.aerialCompositeEnabled = GetB(m, "aerialComposite",  out.aerialCompositeEnabled);
        }
    }
    return true;
}
