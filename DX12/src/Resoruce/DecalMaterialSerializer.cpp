#include "Resource/DecalMaterialSerializer.h"
#include "Resource/DecalMaterialAsset.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace Resource
{
    using json = nlohmann::json;

    // File format versions the reader accepts:
    //   v1: "textures" { albedo, normal, orm } + tint / opacity / flags.
    //   v2 (written): a 9-slot "textures" map keyed by SlotKey(), scalar multipliers and
    //       the WRITE_BASECOLOR..WRITE_AO flag bits.
    static constexpr int kFormatVersion = 2;

    static const char* BlendModeToString(DecalMaterialAsset::NormalBlendMode m)
    {
        return (m == DecalMaterialAsset::NormalBlendMode::Lerp) ? "Lerp" : "RNM";
    }
    static DecalMaterialAsset::NormalBlendMode BlendModeFromString(const std::string& s)
    {
        return (s == "Lerp") ? DecalMaterialAsset::NormalBlendMode::Lerp
                             : DecalMaterialAsset::NormalBlendMode::RNM;
    }

    bool SaveDecalMaterial(const DecalMaterialAsset& a, const std::string& path)
    {
        json j;
        j["version"] = kFormatVersion;
        j["name"]    = a.name;

        // Textures: the 9-slot map keyed by SlotKey(). Only non-empty paths are written,
        // so a scalar-only decal material stays lean.
        json textures = json::object();
        for (uint32_t i = 0; i < DecalMaterialAsset::SLOT_COUNT; ++i)
        {
            const auto slot = static_cast<DecalMaterialAsset::TextureSlot>(i);
            if (!a.texPaths[i].empty())
                textures[DecalMaterialAsset::SlotKey(slot)] = a.texPaths[i];
        }
        j["textures"] = std::move(textures);

        j["baseColorTint"] = { a.baseColorTint.x, a.baseColorTint.y,
                               a.baseColorTint.z, a.baseColorTint.w };

        j["scalars"] = {
            {"opacity",           a.opacity},
            {"roughness",         a.roughness},
            {"specular",          a.specular},
            {"ao",                a.ao},
            {"normalStrength",    a.normalStrength},
            {"bumpStrength",      a.bumpStrength},
            {"cavityStrength",    a.cavityStrength},
            {"displacementScale", a.displacementScale}
        };

        j["flags"]           = a.flags;
        j["normalBlendMode"] = BlendModeToString(a.normalBlendMode);
        j["angleFadeStart"]  = a.angleFadeStart;
        j["sortLayer"]       = a.sortLayer;

        auto f = Sys::OpenOfstream(path, std::ios::out);
        if (!f)
        {
            LOG_ERROR("DecalMaterialSerializer: cannot open '%s' for writing", path.c_str());
            return false;
        }
        f << j.dump(2);
        if (!f.good())
        {
            LOG_ERROR("DecalMaterialSerializer: write error on '%s'", path.c_str());
            return false;
        }
        LOG_SUCCESS("DecalMaterialSerializer: saved '%s' ('%s')", path.c_str(), a.name.c_str());
        return true;
    }

    // Read helpers: one reader per format version keeps LoadDecalMaterial short.

    static void LoadV1Compat(DecalMaterialAsset& out, const json& j)
    {
        // v1 -> v2 texture slots: albedo -> SLOT_BASECOLOR, normal -> SLOT_NORMAL,
        // orm -> SLOT_ROUGHNESS. The roughness slot samples .r, so a real ORM texture has to
        // be re-exported or moved to the matching slots.
        if (auto it = j.find("textures"); it != j.end() && it->is_object())
        {
            out.texPaths[DecalMaterialAsset::SLOT_BASECOLOR] = it->value("albedo", std::string{});
            out.texPaths[DecalMaterialAsset::SLOT_NORMAL]    = it->value("normal", std::string{});
            out.texPaths[DecalMaterialAsset::SLOT_ROUGHNESS] = it->value("orm",    std::string{});
        }

        if (auto it = j.find("tint"); it != j.end() && it->is_array() && it->size() == 4)
        {
            out.baseColorTint.x = (*it)[0].get<float>();
            out.baseColorTint.y = (*it)[1].get<float>();
            out.baseColorTint.z = (*it)[2].get<float>();
            out.baseColorTint.w = (*it)[3].get<float>();
        }
        out.opacity        = j.value("opacity", out.opacity);
        out.angleFadeStart = j.value("angleFadeStart", out.angleFadeStart);
        out.sortLayer      = j.value("sortLayer", out.sortLayer);

        // v1 flag bits 0..2 (albedo, normal, rough) map 1:1 to v2 (basecolor, normal,
        // roughness); bit 3 (metal) is dropped: the Specular workflow has no metallic.
        const uint32_t v1Flags = j.value<uint32_t>("flags",
            static_cast<uint32_t>(DecalMaterialAsset::DEFAULT));
        out.flags = (v1Flags & 0x7u);  // mask to bits 0..2 which map 1:1
        // The roughness / specular / ao scalars and normalStrength keep their defaults.
    }

    static void LoadV2(DecalMaterialAsset& out, const json& j)
    {
        if (auto it = j.find("textures"); it != j.end() && it->is_object())
        {
            for (uint32_t i = 0; i < DecalMaterialAsset::SLOT_COUNT; ++i)
            {
                const auto slot = static_cast<DecalMaterialAsset::TextureSlot>(i);
                out.texPaths[i] = it->value(DecalMaterialAsset::SlotKey(slot), std::string{});
            }
        }

        if (auto it = j.find("baseColorTint");
            it != j.end() && it->is_array() && it->size() == 4)
        {
            out.baseColorTint.x = (*it)[0].get<float>();
            out.baseColorTint.y = (*it)[1].get<float>();
            out.baseColorTint.z = (*it)[2].get<float>();
            out.baseColorTint.w = (*it)[3].get<float>();
        }

        if (auto it = j.find("scalars"); it != j.end() && it->is_object())
        {
            out.opacity            = it->value("opacity",           out.opacity);
            out.roughness          = it->value("roughness",         out.roughness);
            out.specular           = it->value("specular",          out.specular);
            out.ao                 = it->value("ao",                out.ao);
            out.normalStrength     = it->value("normalStrength",    out.normalStrength);
            out.bumpStrength       = it->value("bumpStrength",      out.bumpStrength);
            out.cavityStrength     = it->value("cavityStrength",    out.cavityStrength);
            out.displacementScale  = it->value("displacementScale", out.displacementScale);
        }

        out.flags           = j.value("flags",
                                      static_cast<uint32_t>(DecalMaterialAsset::DEFAULT));
        out.normalBlendMode = BlendModeFromString(
            j.value("normalBlendMode", std::string("RNM")));
        out.angleFadeStart  = j.value("angleFadeStart", out.angleFadeStart);
        out.sortLayer       = j.value("sortLayer",      out.sortLayer);
    }

    bool LoadDecalMaterial(DecalMaterialAsset& out, const std::string& path)
    {
        auto f = Sys::OpenIfstream(path, std::ios::in);
        if (!f)
        {
            LOG_ERROR("DecalMaterialSerializer: cannot open '%s' for reading", path.c_str());
            return false;
        }

        json j;
        try { f >> j; }
        catch (const std::exception& e)
        {
            LOG_ERROR("DecalMaterialSerializer: parse error on '%s' — %s", path.c_str(), e.what());
            return false;
        }

        const int version = j.value("version", 0);
        if (version > kFormatVersion)
        {
            LOG_ERROR("DecalMaterialSerializer: '%s' has version %d > supported %d",
                      path.c_str(), version, kFormatVersion);
            return false;
        }

        out.name = j.value("name", out.name);

        if (version <= 1) LoadV1Compat(out, j);
        else              LoadV2     (out, j);

        // Invalidate the runtime caches: the next Resolve() acquires again.
        for (uint32_t i = 0; i < DecalMaterialAsset::SLOT_COUNT; ++i)
        {
            out.texHandles[i]  = kInvalidTextureHandle;
            out.texBindless[i] = -1;
        }

        LOG_SUCCESS("DecalMaterialSerializer: loaded '%s' (v%d, '%s')",
                    path.c_str(), version, out.name.c_str());
        return true;
    }
}
