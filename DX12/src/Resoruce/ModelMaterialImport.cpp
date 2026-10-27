// Resource/ModelMaterialImport.cpp - a model's materials from its source. See the header.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/ModelMaterialImport.h"

#include "Resource/TextureImporter.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <assimp/texture.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace Resource
{
namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string ExtensionOf(const std::string& name)
    {
        const size_t dot = name.find_last_of('.');
        const size_t sep = name.find_last_of("/\\");
        if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return {};
        return Lower(name.substr(dot));
    }

    std::string StemOf(const std::string& name)
    {
        const size_t sep = name.find_last_of("/\\");
        std::string file = (sep == std::string::npos) ? name : name.substr(sep + 1);
        const size_t dot = file.find_last_of('.');
        return (dot == std::string::npos || dot == 0) ? file : file.substr(0, dot);
    }

    // SceneImporter's SanitiseName rule: spaces and separators become '_', ASCII
    // alphanumerics and "-._" stay, UTF-8 passes through, other ASCII drops.
    std::string SanitiseFileStem(const std::string& raw)
    {
        std::string out;
        for (const unsigned char c : raw)
        {
            if (c == ' ' || c == '/' || c == '\\' || c == ':') out += '_';
            else if (c > 127) out += static_cast<char>(c);
            else if (std::isalnum(c) || c == '-' || c == '.' || c == '_') out += static_cast<char>(c);
        }
        return out.empty() ? std::string("texture") : out;
    }

    // Any image this reads (by the name's extension: DDS / TGA / HDR, else WIC -
    // PNG, JPEG, BMP ...) as one R8G8B8A8 image, mip 0.
    bool DecodeRgba8(const ModelTextureCook::Image& img, DirectX::ScratchImage& out)
    {
        using namespace DirectX;
        const std::string ext = ExtensionOf(img.name);
        TexMetadata  meta{};
        ScratchImage loaded;
        HRESULT hr = E_FAIL;
        if (ext == ".dds")      hr = LoadFromDDSMemory(img.bytes.data(), img.bytes.size(), DDS_FLAGS_NONE, &meta, loaded);
        else if (ext == ".tga") hr = LoadFromTGAMemory(img.bytes.data(), img.bytes.size(), TGA_FLAGS_NONE, &meta, loaded);
        else if (ext == ".hdr") hr = LoadFromHDRMemory(img.bytes.data(), img.bytes.size(), &meta, loaded);
        else                    hr = LoadFromWICMemory(img.bytes.data(), img.bytes.size(), WIC_FLAGS_IGNORE_SRGB, &meta, loaded);
        if (FAILED(hr) || loaded.GetImageCount() == 0) return false;

        const Image* first = loaded.GetImage(0, 0, 0);
        if (!first) return false;
        ScratchImage plain;
        if (IsCompressed(first->format))
        {
            if (FAILED(Decompress(*first, DXGI_FORMAT_R8G8B8A8_UNORM, plain))) return false;
            first = plain.GetImage(0, 0, 0);
        }
        if (first->format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            ScratchImage converted;
            if (FAILED(Convert(*first, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted)))
                return false;
            out = std::move(converted);
            return true;
        }
        return SUCCEEDED(out.InitializeFromImage(*first));
    }

    bool ToDds(const DirectX::ScratchImage& img, std::vector<uint8_t>& out)
    {
        DirectX::Blob blob;
        if (FAILED(DirectX::SaveToDDSMemory(img.GetImages(), img.GetImageCount(), img.GetMetadata(),
                                            DirectX::DDS_FLAGS_NONE, blob)))
            return false;
        const auto* p = static_cast<const uint8_t*>(blob.GetBufferPointer());
        out.assign(p, p + blob.GetBufferSize());
        return true;
    }

    std::vector<uint8_t> ToItex(const std::string& name, const std::vector<uint8_t>& bytes, TextureImportFormat format)
    {
        TextureImporter importer;
        TextureImportSettings settings{};
        settings.format = format;
        return importer.ImportWithSettings(name, bytes, settings);
    }
}

// ==== ModelTextureCook ====
ModelTextureCook::ModelTextureCook(std::string refRoot, std::string stem, std::string sourceDir)
    : m_refRoot(std::move(refRoot)), m_stem(std::move(stem)), m_sourceDir(std::move(sourceDir))
{
    while (!m_refRoot.empty() && (m_refRoot.back() == '/' || m_refRoot.back() == '\\')) m_refRoot.pop_back();
    for (char& c : m_refRoot) if (c == '\\') c = '/';
}

ModelTextureCook::Image ModelTextureCook::FromFile(const std::string& path) const
{
    namespace fs = std::filesystem;
    Image img;
    if (path.empty()) return img;
    std::string norm = path;
    for (char& c : norm) if (c == '\\') c = '/';
    const fs::path given = Sys::U8Path(norm);
    std::vector<fs::path> tries;
    tries.push_back(given.is_absolute() ? given : Sys::U8Path(m_sourceDir) / given);
    tries.push_back(Sys::U8Path(m_sourceDir) / given.filename());   // an author's machine path
    for (const fs::path& p : tries)
    {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        const std::string utf8 = Sys::PathToUtf8(p.lexically_normal());
        if (!Sys::ReadFileBytes(utf8, img.bytes) || img.bytes.empty()) continue;
        img.key  = Lower(utf8);
        img.name = Sys::PathToUtf8(p.filename());
        return img;
    }
    img.key  = norm;
    img.name = Sys::PathToUtf8(given.filename());
    return img;   // no bytes: the caller counts it missing
}

ModelTextureCook::Image ModelTextureCook::FromAssimp(const aiScene& scene, const std::string& path) const
{
    const auto [tex, index] = scene.GetEmbeddedTextureAndIndex(path.c_str());
    if (!tex) return FromFile(path);

    Image img;
    img.key = "*" + std::to_string(index);
    std::string base = tex->mFilename.length ? StemOf(tex->mFilename.C_Str()) : std::string();
    if (base.empty()) base = "embedded_" + std::to_string(index);
    if (tex->mHeight == 0)
    {
        // Compressed (PNG / JPEG ...): mWidth is the byte count, the hint the format.
        const auto* p = reinterpret_cast<const uint8_t*>(tex->pcData);
        img.bytes.assign(p, p + tex->mWidth);
        std::string hint = Lower(tex->achFormatHint);
        if (hint.empty()) hint = "png";
        img.name = base + "." + hint;
        return img;
    }
    // Raw ARGB8888 texels (aiTexel is b, g, r, a).
    DirectX::ScratchImage raw;
    if (FAILED(raw.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, tex->mWidth, tex->mHeight, 1, 1))) return img;
    const DirectX::Image* dst = raw.GetImage(0, 0, 0);
    for (unsigned y = 0; y < tex->mHeight; ++y)
        for (unsigned x = 0; x < tex->mWidth; ++x)
        {
            const aiTexel& t = tex->pcData[y * tex->mWidth + x];
            uint8_t* q = dst->pixels + y * dst->rowPitch + x * 4;
            q[0] = t.r; q[1] = t.g; q[2] = t.b; q[3] = t.a;
        }
    if (ToDds(raw, img.bytes)) img.name = base + ".dds";
    return img;
}

std::string ModelTextureCook::UniqueFileName(const std::string& stem)
{
    const std::string base = SanitiseFileStem(stem);
    std::string name = base;
    for (int n = 1; m_taken.count(Lower(name)); ++n) name = base + "_" + std::to_string(n);
    m_taken[Lower(name)] = 1;
    return name;
}

std::string ModelTextureCook::Emit(const std::string& fileStem, std::vector<uint8_t>&& itex)
{
    const std::string file = UniqueFileName(fileStem) + ".itex";
    m_files.emplace_back(m_stem + "/Textures/" + file, std::move(itex));
    return m_refRoot + "/" + m_stem + "/Textures/" + file;
}

ModelTextureCook::Result ModelTextureCook::Cook(const Image& img, Kind kind, bool flipGreen, bool probeAlpha)
{
    Result r;
    if (!Enabled() || !img.Valid()) return r;
    const std::string doneKey = img.key + "|" + std::to_string(static_cast<int>(kind)) + (flipGreen ? "|f" : "");
    if (const auto it = m_done.find(doneKey); it != m_done.end()) return it->second;

    DirectX::ScratchImage rgba;
    const bool needPixels = probeAlpha || (kind == Kind::Normal && flipGreen);
    if (needPixels && !DecodeRgba8(img, rgba))
    {
        LOG_WARNING("ModelTextureCook: '%s' does not decode - its material keeps the default", img.name.c_str());
        ++m_failed;
        return m_done[doneKey] = r;
    }
    if (probeAlpha)
    {
        const DirectX::Image* p = rgba.GetImage(0, 0, 0);
        for (size_t y = 0; y < p->height && !r.hasAlpha; ++y)
            for (size_t x = 0; x < p->width; ++x)
                if (p->pixels[y * p->rowPitch + x * 4 + 3] < 250) { r.hasAlpha = true; break; }
    }

    std::vector<uint8_t> itex;
    const std::string    stem = StemOf(img.name);
    switch (kind)
    {
    case Kind::Color:
        itex = ToItex(img.name, img.bytes, TextureImportFormat::BC7_SRGB);
        break;
    case Kind::Mask:
        itex = ToItex(img.name, img.bytes, TextureImportFormat::BC4_Linear);
        break;
    case Kind::Normal:
        if (flipGreen)
        {
            // A DirectX-convention map (+Y down the image) -> the OpenGL convention this path draws right
            // (+Y up; see the normal-map note in FillMaterialFromAssimp).
            const DirectX::Image* p = rgba.GetImage(0, 0, 0);
            for (size_t y = 0; y < p->height; ++y)
                for (size_t x = 0; x < p->width; ++x)
                {
                    uint8_t& g = p->pixels[y * p->rowPitch + x * 4 + 1];
                    g = static_cast<uint8_t>(255 - g);
                }
            std::vector<uint8_t> dds;
            if (ToDds(rgba, dds)) itex = ToItex(stem + ".dds", dds, TextureImportFormat::BC5_Linear);
        }
        else
            itex = ToItex(img.name, img.bytes, TextureImportFormat::BC5_Linear);
        break;
    }
    if (itex.empty())
    {
        LOG_WARNING("ModelTextureCook: '%s' did not cook - its material keeps the default", img.name.c_str());
        ++m_failed;
        return m_done[doneKey] = r;
    }
    r.path = Emit(stem, std::move(itex));
    return m_done[doneKey] = r;
}

std::string ModelTextureCook::CookSurface(Channel occlusion, Channel roughness, Channel metalness,
                                          const std::string& name)
{
    if (!Enabled()) return {};
    const Channel ch[3] = { occlusion, roughness, metalness };
    std::string doneKey = "surface";
    for (const Channel& c : ch)
        doneKey += "|" + ((c.img && c.img->Valid()) ? c.img->key + "#" + std::to_string(c.channel) : std::string("-"));
    if (const auto it = m_done.find(doneKey); it != m_done.end()) return it->second.path;

    // Each distinct image decoded once; the first present sets the size.
    std::unordered_map<const Image*, DirectX::ScratchImage> decoded;
    size_t w = 0, h = 0;
    for (const Channel& c : ch)
    {
        if (!c.img || !c.img->Valid() || decoded.count(c.img)) continue;
        DirectX::ScratchImage img;
        if (!DecodeRgba8(*c.img, img))
        {
            LOG_WARNING("ModelTextureCook: '%s' does not decode - its ARM channel takes the default", c.img->name.c_str());
            ++m_failed;
            continue;
        }
        if (w == 0) { w = img.GetMetadata().width; h = img.GetMetadata().height; }
        decoded.emplace(c.img, std::move(img));
    }
    if (w == 0) return m_done[doneKey].path = std::string();
    for (auto& [img, scratch] : decoded)
        if (scratch.GetMetadata().width != w || scratch.GetMetadata().height != h)
        {
            DirectX::ScratchImage resized;
            if (SUCCEEDED(DirectX::Resize(*scratch.GetImage(0, 0, 0), w, h, DirectX::TEX_FILTER_DEFAULT, resized)))
                scratch = std::move(resized);
        }

    DirectX::ScratchImage arm;
    if (FAILED(arm.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1))) return {};
    const DirectX::Image* out = arm.GetImage(0, 0, 0);
    for (int c = 0; c < 3; ++c)
    {
        const auto it = (ch[c].img) ? decoded.find(ch[c].img) : decoded.end();
        const DirectX::Image* src = (it != decoded.end()) ? it->second.GetImage(0, 0, 0) : nullptr;
        const bool usable = src && src->width == w && src->height == h;
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x)
                out->pixels[y * out->rowPitch + x * 4 + c] =
                    usable ? src->pixels[y * src->rowPitch + x * 4 + (ch[c].channel & 3)] : uint8_t(255);
    }
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) out->pixels[y * out->rowPitch + x * 4 + 3] = 255;

    std::vector<uint8_t> dds, itex;
    if (ToDds(arm, dds)) itex = ToItex(name + ".dds", dds, TextureImportFormat::BC7_Linear);
    if (itex.empty())
    {
        LOG_WARNING("ModelTextureCook: the ARM map '%s' did not cook - its material keeps the default", name.c_str());
        ++m_failed;
        return m_done[doneKey].path = std::string();
    }
    return m_done[doneKey].path = Emit(name, std::move(itex));
}

// ==== FillMaterialFromAssimp ====
void FillMaterialFromAssimp(const aiMaterial& src, const aiScene& scene, bool gltf, bool vertexColors,
                            ModelTextureCook& cook, MaterialComponent& mc, ImportedMaterialNotes& notes,
                            bool flipGreen)
{
    // The engine stores colours sRGB (the renderer linearises them); glTF's are linear.
    auto toStored = [gltf](float c)
    {
        c = std::clamp(c, 0.f, 1.f);
        return gltf ? std::pow(c, 1.f / 2.2f) : c;
    };
    bool textured = false;
    auto image = [&](std::initializer_list<aiTextureType> types) -> ModelTextureCook::Image
    {
        for (const aiTextureType t : types)
        {
            aiString     path;
            unsigned int uv = 0;
            if (src.GetTextureCount(t) == 0 || src.GetTexture(t, 0, &path, nullptr, &uv) != AI_SUCCESS) continue;
            if (uv != 0) ++notes.otherUvSet;
            ModelTextureCook::Image img = cook.FromAssimp(scene, path.C_Str());
            if (!img.Valid())
            {
                ++notes.missingImages;
                LOG_WARNING("ModelMaterialImport: texture '%s' does not resolve (looked beside the source)",
                            path.C_Str());
            }
            return img;
        }
        return {};
    };

    // ---- Base colour, opacity, alpha mode ----
    aiColor4D base(1.f, 1.f, 1.f, 1.f);
    if (aiGetMaterialColor(&src, AI_MATKEY_BASE_COLOR, &base) != AI_SUCCESS)
        aiGetMaterialColor(&src, AI_MATKEY_COLOR_DIFFUSE, &base);
    float opacity = gltf ? base.a : 1.f;
    if (!gltf) src.Get(AI_MATKEY_OPACITY, opacity);
    mc.baseColor = { toStored(base.r), toStored(base.g), toStored(base.b), std::clamp(opacity, 0.f, 1.f) };

    aiString alphaMode;
    if (gltf && src.Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS)
    {
        const std::string m = alphaMode.C_Str();
        if (m == "MASK")
        {
            float cutoff = 0.5f;
            src.Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff);
            mc.userBlendMode = BlendMode::Masked;
            mc.alphaRef      = std::clamp(cutoff, 0.f, 1.f);
        }
        else if (m == "BLEND")
            mc.userBlendMode = BlendMode::Alpha;
    }
    else if (!gltf && opacity < 0.999f)
        mc.userBlendMode = BlendMode::Alpha;

    // ---- Transmission (KHR_materials_transmission / _volume / _ior) ----
    // A glTF material that transmits draws Transmissive, whatever its alpha mode: opacity stays its
    // coverage. Its attenuation colour is linear like its base colour; an infinite attenuation
    // distance (glTF's default) is none, 0 here. The thickness is the source's, in its units: a model
    // imported at another scale needs it scaled too. The transmission and thickness textures have no
    // slot: their factors stand alone.
    float transmission = 0.f;
    if (gltf && src.Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission) == AI_SUCCESS && transmission > 0.f)
    {
        mc.userBlendMode = BlendMode::Transmissive;
        mc.transmission  = std::clamp(transmission, 0.f, 1.f);
        float ior = 1.5f;
        if (src.Get(AI_MATKEY_REFRACTI, ior) == AI_SUCCESS && std::isfinite(ior)) mc.ior = std::max(ior, 1.f);
        float thickness = 0.f;
        if (src.Get(AI_MATKEY_VOLUME_THICKNESS_FACTOR, thickness) == AI_SUCCESS && std::isfinite(thickness))
            mc.thickness = std::max(thickness, 0.f);
        float distance = 0.f;
        if (src.Get(AI_MATKEY_VOLUME_ATTENUATION_DISTANCE, distance) == AI_SUCCESS && std::isfinite(distance) &&
            distance > 0.f)
            mc.attenuationDistance = distance;
        aiColor3D attenuation(1.f, 1.f, 1.f);
        if (src.Get(AI_MATKEY_VOLUME_ATTENUATION_COLOR, attenuation) == AI_SUCCESS)
            mc.attenuationColor = { toStored(attenuation.r), toStored(attenuation.g), toStored(attenuation.b), 1.f };
        ++notes.transmissive;
        if (src.GetTextureCount(aiTextureType_TRANSMISSION) > 0) ++notes.transmissionMaps;
    }

    int twoSided = 0;
    if (src.Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided) mc.SetDoubleSided(true);
    if (gltf && vertexColors) mc.flags |= MaterialComponent::USE_VERTEXCOLORS;

    if (!cook.Enabled()) return;

    // ---- Textures ----
    const ModelTextureCook::Image baseImg = image({ aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE });
    if (baseImg.Valid())
        if (const auto r = cook.Cook(baseImg, ModelTextureCook::Kind::Color); !r.path.empty())
        {
            mc.textures[MaterialComponent::BASECOLORMAP].name = r.path;
            textured = true;
        }

    // As the source has it (tests/unit/test_model_materials.py checks it): an OpenGL-convention map -
    // glTF's; Blender's and Maya's FBX / OBJ - already turns the right way through this path (assimp's
    // UV flip and tangent space put +G toward the image's top); flipping G tilts the normal toward the
    // image's bottom. A DirectX-convention source (3ds Max, UE) is the case for @p flipGreen - the
    // import option, not a guess.
    const ModelTextureCook::Image nrmImg = image({ aiTextureType_NORMALS });
    if (nrmImg.Valid())
        if (const auto r = cook.Cook(nrmImg, ModelTextureCook::Kind::Normal, flipGreen); !r.path.empty())
        {
            mc.textures[MaterialComponent::NORMALMAP].name = r.path;
            textured = true;
        }

    if (!gltf)
        if (const ModelTextureCook::Image opImg = image({ aiTextureType_OPACITY }); opImg.Valid())
            if (const auto r = cook.Cook(opImg, ModelTextureCook::Kind::Mask); !r.path.empty())
            {
                mc.textures[MaterialComponent::OPACITYMAP].name = r.path;
                if (mc.userBlendMode == BlendMode::Opaque) { mc.userBlendMode = BlendMode::Masked; mc.alphaRef = 0.5f; }
                textured = true;
            }

    // ---- Metallic / roughness / occlusion -> ARM ----
    // The engine's scalars are a remap range over the map (lerp(min, max, map)): with a map,
    // [0, factor] is factor x map (glTF's rule); without one, [f, f] is the factor whatever the
    // default map holds.
    float metal = 0.f, rough = 1.f;
    const bool haveMetal = src.Get(AI_MATKEY_METALLIC_FACTOR, metal) == AI_SUCCESS;
    const bool haveRough = src.Get(AI_MATKEY_ROUGHNESS_FACTOR, rough) == AI_SUCCESS;
    ModelTextureCook::Image mrImg = image({ aiTextureType_GLTF_METALLIC_ROUGHNESS });
    ModelTextureCook::Image roughImg, metalImg;
    if (!mrImg.Valid())
    {
        roughImg = image({ aiTextureType_DIFFUSE_ROUGHNESS });
        metalImg = image({ aiTextureType_METALNESS });
    }
    const ModelTextureCook::Image occImg = image({ aiTextureType_LIGHTMAP, aiTextureType_AMBIENT_OCCLUSION });
    const bool anySurface = mrImg.Valid() || roughImg.Valid() || metalImg.Valid() || occImg.Valid();
    std::string surface;
    if (anySurface)
    {
        ModelTextureCook::Channel occ{ occImg.Valid() ? &occImg : nullptr, 0 };
        ModelTextureCook::Channel rgh{ mrImg.Valid() ? &mrImg : (roughImg.Valid() ? &roughImg : nullptr), mrImg.Valid() ? 1 : 0 };
        ModelTextureCook::Channel mtl{ mrImg.Valid() ? &mrImg : (metalImg.Valid() ? &metalImg : nullptr), mrImg.Valid() ? 2 : 0 };
        const std::string armName = StemOf((mrImg.Valid() ? mrImg : roughImg.Valid() ? roughImg
                                            : metalImg.Valid() ? metalImg : occImg).name) + "_ARM";
        surface = cook.CookSurface(occ, rgh, mtl, armName);
    }
    const bool hasRoughMap = !surface.empty() && (mrImg.Valid() || roughImg.Valid());
    const bool hasMetalMap = !surface.empty() && (mrImg.Valid() || metalImg.Valid());
    if (!surface.empty())
    {
        mc.textures[MaterialComponent::SURFACEMAP].name = surface;
        textured = true;
    }
    if (haveRough || hasRoughMap)
    {
        const float r = std::clamp(haveRough ? rough : 1.f, 0.f, 1.f);
        mc.roughnessMin = hasRoughMap ? 0.f : r;
        mc.roughnessMax = r;
    }
    if (haveMetal || hasMetalMap)
    {
        const float m = std::clamp(haveMetal ? metal : 1.f, 0.f, 1.f);
        mc.metalnessMin = hasMetalMap ? 0.f : m;
        mc.metalnessMax = m;
    }
    if (!surface.empty() && !hasRoughMap && !haveRough) { mc.roughnessMin = 0.5f; mc.roughnessMax = 0.5f; }
    if (!surface.empty() && !hasMetalMap && !haveMetal) { mc.metalnessMin = 0.f;  mc.metalnessMax = 0.f; }

    // ---- Emissive ----
    aiColor4D em(0.f, 0.f, 0.f, 1.f);
    aiGetMaterialColor(&src, AI_MATKEY_COLOR_EMISSIVE, &em);
    float strength = 1.f;
    src.Get(AI_MATKEY_EMISSIVE_INTENSITY, strength);
    const bool emits = (em.r + em.g + em.b) > 0.f && strength > 0.f;
    const ModelTextureCook::Image emImg = image({ aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR });
    std::string emPath;
    if (emImg.Valid())
        if (const auto r = cook.Cook(emImg, ModelTextureCook::Kind::Color); !r.path.empty()) emPath = r.path;
    if (emits)
    {
        // The default emissive map is black: a factor with no map needs a white one.
        mc.textures[MaterialComponent::EMISSIVEMAP].name =
            emPath.empty() ? std::string("engine/textures/Default_BaseColor.itex") : emPath;
        mc.emissiveColor = { toStored(em.r), toStored(em.g), toStored(em.b), strength };
        textured = textured || !emPath.empty();
    }

    if (textured) ++notes.textured;
}
}
