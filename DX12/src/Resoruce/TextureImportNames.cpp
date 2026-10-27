// TextureImportNames.cpp - the texture import vocabulary: the names of the formats and mip filters
// (TextureImporter.h). It lives in EngineCore, not with the importer (EditorCore), because the import
// record (Resource/TextureImportRecord.h) and Lua Asset.textureInfo(path).import spell them in both
// exes.
#include "Resource/TextureImporter.h"

#include <cctype>
#include <string>

namespace Resource
{
    namespace
    {
        std::string Lowercase(std::string value)
        {
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }
    }

    const char* TextureImporter::GetFormatName(TextureImportFormat format)
    {
        switch (format)
        {
        case TextureImportFormat::BC7_SRGB:     return "BC7 sRGB (Color)";
        case TextureImportFormat::BC7_Linear:   return "BC7 Linear (RGBA)";
        case TextureImportFormat::BC5_Linear:   return "BC5 Linear (RG / Normal)";
        case TextureImportFormat::BC4_Linear:   return "BC4 Linear (Single Channel)";
        case TextureImportFormat::BC6H_UF16:    return "BC6H UF16 (HDR)";
        case TextureImportFormat::R16_UNorm:    return "Uncompressed R16 UNorm";
        case TextureImportFormat::RG16_Float:   return "Uncompressed RG16 Float";
        case TextureImportFormat::RGBA8_Linear: return "Uncompressed RGBA8 Linear";
        case TextureImportFormat::RGBA16_UNorm: return "Uncompressed RGBA16 UNorm";
        case TextureImportFormat::RGBA16_Float: return "Uncompressed RGBA16 Float";
        default:                                return "Unknown";
        }
    }

    const char* TextureImporter::GetFormatId(TextureImportFormat format)
    {
        switch (format)
        {
        case TextureImportFormat::BC7_SRGB:     return "BC7_SRGB";
        case TextureImportFormat::BC7_Linear:   return "BC7_Linear";
        case TextureImportFormat::BC5_Linear:   return "BC5_Linear";
        case TextureImportFormat::BC4_Linear:   return "BC4_Linear";
        case TextureImportFormat::BC6H_UF16:    return "BC6H_UF16";
        case TextureImportFormat::R16_UNorm:    return "R16_UNorm";
        case TextureImportFormat::RG16_Float:   return "RG16_Float";
        case TextureImportFormat::RGBA8_Linear: return "RGBA8_Linear";
        case TextureImportFormat::RGBA16_UNorm: return "RGBA16_UNorm";
        case TextureImportFormat::RGBA16_Float: return "RGBA16_Float";
        default:                                return "Unknown";
        }
    }

    bool TextureImporter::ParseFormatId(const std::string& id, TextureImportFormat& out)
    {
        for (uint8_t i = 0; i < static_cast<uint8_t>(TextureImportFormat::Count); ++i)
        {
            const auto f = static_cast<TextureImportFormat>(i);
            if (id == GetFormatId(f)) { out = f; return true; }
        }
        return false;
    }

    const char* TextureImporter::GetMipFilterId(TextureMipFilter filter)
    {
        switch (filter)
        {
        case TextureMipFilter::Default:  return "default";
        case TextureMipFilter::Box:      return "box";
        case TextureMipFilter::Linear:   return "linear";
        case TextureMipFilter::Cubic:    return "cubic";
        case TextureMipFilter::Triangle: return "triangle";
        case TextureMipFilter::Point:    return "point";
        default:                         return "default";
        }
    }

    bool TextureImporter::ParseMipFilterId(const std::string& id, TextureMipFilter& out)
    {
        const std::string l = Lowercase(id);
        for (uint8_t i = 0; i < static_cast<uint8_t>(TextureMipFilter::Count); ++i)
        {
            const auto f = static_cast<TextureMipFilter>(i);
            if (l == GetMipFilterId(f)) { out = f; return true; }
        }
        return false;
    }

    const char* TextureImporter::GetFormatDescription(TextureImportFormat format)
    {
        switch (format)
        {
        case TextureImportFormat::BC7_SRGB:     return "Four-channel color texture; sampled as sRGB.";
        case TextureImportFormat::BC7_Linear:   return "Four-channel linear data or generic texture.";
        case TextureImportFormat::BC5_Linear:   return "Two-channel linear data; intended for normal maps.";
        case TextureImportFormat::BC4_Linear:   return "One-channel linear data such as roughness, AO, or masks.";
        case TextureImportFormat::BC6H_UF16:    return "Unsigned high-dynamic-range RGB texture.";
        case TextureImportFormat::R16_UNorm:    return "Lossless 16-bit single channel; heightmaps/displacement.";
        case TextureImportFormat::RG16_Float:   return "Uncompressed two-channel half float; BRDF/LUT data.";
        case TextureImportFormat::RGBA8_Linear: return "Uncompressed 8-bit linear RGBA; ramps and lookup data.";
        case TextureImportFormat::RGBA16_UNorm: return "Uncompressed 16-bit RGBA; SDF/face lightmaps.";
        case TextureImportFormat::RGBA16_Float: return "Uncompressed half-float RGBA; HDR ramps/overbright data.";
        default:                                return "";
        }
    }
}
