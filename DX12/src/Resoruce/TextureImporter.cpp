#include "Resource/TextureImporter.h"
#include "Resource/AssetHeader.h"
#include "Resource/BCCompressor.h"
#include "System/Log.h"
#include <DirectXTex.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

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

        bool ContainsAny(const std::string& value,
                         std::initializer_list<const char*> keywords)
        {
            return std::any_of(keywords.begin(), keywords.end(),
                [&](const char* keyword)
                {
                    return value.find(keyword) != std::string::npos;
                });
        }

        bool IsFloatFormat(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                   format == DXGI_FORMAT_R32G32B32_FLOAT ||
                   format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        }

        bool IsBCFormat(TextureImportFormat format)
        {
            return format == TextureImportFormat::BC7_SRGB ||
                   format == TextureImportFormat::BC7_Linear ||
                   format == TextureImportFormat::BC5_Linear ||
                   format == TextureImportFormat::BC4_Linear ||
                   format == TextureImportFormat::BC6H_UF16;
        }

        DXGI_FORMAT FinalFormat(TextureImportFormat format)
        {
            switch (format)
            {
            case TextureImportFormat::BC7_SRGB:     return DXGI_FORMAT_BC7_UNORM_SRGB;
            case TextureImportFormat::BC7_Linear:   return DXGI_FORMAT_BC7_UNORM;
            case TextureImportFormat::BC5_Linear:   return DXGI_FORMAT_BC5_UNORM;
            case TextureImportFormat::BC4_Linear:   return DXGI_FORMAT_BC4_UNORM;
            case TextureImportFormat::BC6H_UF16:    return DXGI_FORMAT_BC6H_UF16;
            case TextureImportFormat::R16_UNorm:    return DXGI_FORMAT_R16_UNORM;
            case TextureImportFormat::RG16_Float:   return DXGI_FORMAT_R16G16_FLOAT;
            case TextureImportFormat::RGBA8_Linear: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TextureImportFormat::RGBA16_UNorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
            case TextureImportFormat::RGBA16_Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default:                                return DXGI_FORMAT_UNKNOWN;
            }
        }

        DXGI_FORMAT IntermediateFormat(TextureImportFormat format)
        {
            switch (format)
            {
            case TextureImportFormat::BC7_SRGB:     return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case TextureImportFormat::BC7_Linear:
            case TextureImportFormat::BC5_Linear:
            case TextureImportFormat::BC4_Linear:
            case TextureImportFormat::RGBA8_Linear: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TextureImportFormat::BC6H_UF16:
            case TextureImportFormat::RGBA16_Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case TextureImportFormat::R16_UNorm:    return DXGI_FORMAT_R16_UNORM;
            case TextureImportFormat::RG16_Float:   return DXGI_FORMAT_R16G16_FLOAT;
            case TextureImportFormat::RGBA16_UNorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
            default:                                return DXGI_FORMAT_UNKNOWN;
            }
        }

        // The DirectXTex filter of a TextureMipFilter (the chain and a maxSize resize).
        DirectX::TEX_FILTER_FLAGS FilterFlags(TextureMipFilter filter)
        {
            switch (filter)
            {
            case TextureMipFilter::Box:      return DirectX::TEX_FILTER_BOX;
            case TextureMipFilter::Linear:   return DirectX::TEX_FILTER_LINEAR;
            case TextureMipFilter::Cubic:    return DirectX::TEX_FILTER_CUBIC;
            case TextureMipFilter::Triangle: return DirectX::TEX_FILTER_TRIANGLE;
            case TextureMipFilter::Point:    return DirectX::TEX_FILTER_POINT;
            default:                         return DirectX::TEX_FILTER_DEFAULT;
            }
        }

        DXGI_FORMAT DecompressionFormat(DXGI_FORMAT sourceFormat,
                                        TextureImportFormat outputFormat)
        {
            if (sourceFormat == DXGI_FORMAT_BC6H_UF16 ||
                sourceFormat == DXGI_FORMAT_BC6H_SF16 ||
                outputFormat == TextureImportFormat::BC6H_UF16 ||
                outputFormat == TextureImportFormat::RGBA16_Float)
            {
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            }

            return outputFormat == TextureImportFormat::BC7_SRGB
                ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                : DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    TextureImportFormat TextureImporter::SuggestFormat(const std::string& sourcePath)
    {
        const std::string lower = Lowercase(sourcePath);
        const size_t dot = lower.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string{} : lower.substr(dot);

        if (ext == ".hdr")
            return TextureImportFormat::BC6H_UF16;

        // Precision-sensitive, uncompressed rules must win over generic maps.
        if (ContainsAny(lower, { "brdf", "_brdf" }))
            return TextureImportFormat::RG16_Float;
        if (ContainsAny(lower, { "heightmap", "_h16", "_raw_height", "_disp" }))
            return TextureImportFormat::R16_UNorm;
        if (ContainsAny(lower, { "_sdf", "facelightmap", "_facelightmap",
                                 "facemap", "facelight", "face_shadow", "faceshadow" }))
            return TextureImportFormat::RGBA16_UNorm;
        if (ContainsAny(lower, { "rgbahalf", "ramphalf", "ramp_half",
                                 "ramp_hdr", "_ramphdr" }))
            return TextureImportFormat::RGBA16_Float;
        if (ContainsAny(lower, { "ramp", "_ramp", "lut", "_lut",
                                 "_raw", "_uncompressed", "_lossless" }))
            return TextureImportFormat::RGBA8_Linear;
        // Channel-packed dual-normal maps (Endfield "_HN": tangent normal in RG plus a second,
        // uncorrelated highlight normal in BA). BC7 shares its endpoints/mode across all four channels, so
        // the low mips of two packed uncorrelated signals drift per 4x4 block: decoded normals tilt in
        // blocks and the hair grows dark "shadow blocks" with camera distance. Uncompressed keeps every mip a
        // clean per-channel box average (a shortened normal re-normalizes toward flat, which degrades
        // gracefully). Costs 4x the VRAM of BC7, so only this suffix takes it. Runtime half:
        // EF_TexLodClamped in endfield_common.hlsli.
        if (ContainsAny(lower, { "_hn." }))
            return TextureImportFormat::RGBA8_Linear;
        if (ContainsAny(lower, { "_normal", "_nrm", "_norm", "_n.",
                                 "nmap", "normalmap" }))
            return TextureImportFormat::BC5_Linear;
        if (ContainsAny(lower, { "roughness", "_roughness", "metalness", "_metallic",
                                 "_ao", "ambient", "_height", "_mask", "_bump",
                                 "_gloss", "_opacity", "_specular", "_cavity" }))
            return TextureImportFormat::BC4_Linear;
        if (ContainsAny(lower, { "albedo", "basecolor", "diffuse", "_color",
                                 "_col", "_d.", "_da.", "_spc" }))
            return TextureImportFormat::BC7_SRGB;

        return TextureImportFormat::BC7_Linear;
    }

    std::vector<uint8_t> TextureImporter::Import(const std::string& sourcePath,
                                                  const std::vector<uint8_t>& sourceData)
    {
        TextureImportSettings settings{};
        settings.format = SuggestFormat(sourcePath);
        return ImportInternal(sourcePath, sourceData, settings, true);
    }

    std::vector<uint8_t> TextureImporter::ImportWithSettings(
        const std::string& sourcePath,
        const std::vector<uint8_t>& sourceData,
        const TextureImportSettings& settings)
    {
        return ImportInternal(sourcePath, sourceData, settings, false);
    }

    std::vector<uint8_t> TextureImporter::ImportInternal(
        const std::string& sourcePath,
        const std::vector<uint8_t>& sourceData,
        TextureImportSettings settings,
        bool allowFloatSourcePromotion)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("TextureImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        const auto formatIndex = static_cast<uint8_t>(settings.format);
        if (formatIndex >= static_cast<uint8_t>(TextureImportFormat::Count))
        {
            LOG_ERROR("TextureImporter: invalid import format for '%s'", sourcePath.c_str());
            return {};
        }

        const std::string lowerPath = Lowercase(sourcePath);
        const size_t dot = lowerPath.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string{} : lowerPath.substr(dot);

        DirectX::TexMetadata meta{};
        DirectX::ScratchImage image;
        HRESULT hr = E_FAIL;

        if (ext == ".dds")
        {
            hr = DirectX::LoadFromDDSMemory(sourceData.data(), sourceData.size(),
                                             DirectX::DDS_FLAGS_NONE, &meta, image);
        }
        else if (ext == ".tga")
        {
            hr = DirectX::LoadFromTGAMemory(sourceData.data(), sourceData.size(),
                                             DirectX::TGA_FLAGS_NONE, &meta, image);
        }
        else if (ext == ".hdr")
        {
            hr = DirectX::LoadFromHDRMemory(sourceData.data(), sourceData.size(), &meta, image);
        }
        else
        {
            hr = DirectX::LoadFromWICMemory(sourceData.data(), sourceData.size(),
                                             DirectX::WIC_FLAGS_NONE, &meta, image);
        }

        if (FAILED(hr))
        {
            LOG_ERROR("TextureImporter: decode failed for '%s' (hr=0x%08X)",
                      sourcePath.c_str(), static_cast<unsigned>(hr));
            return {};
        }

        // Non-interactive loads: an unnamed float DDS is HDR. The editor's confirmed selection is always
        // authoritative.
        if (allowFloatSourcePromotion && IsFloatFormat(meta.format) &&
            settings.format != TextureImportFormat::RGBA16_Float &&
            settings.format != TextureImportFormat::RGBA16_UNorm)
        {
            settings.format = TextureImportFormat::BC6H_UF16;
        }

        const DXGI_FORMAT targetFormat = FinalFormat(settings.format);
        const DXGI_FORMAT intermediate = IntermediateFormat(settings.format);

        // A DDS may already be BC-compressed. If it does not match the reviewed
        // output choice, decompress it before channel conversion/recompression.
        if (DirectX::IsCompressed(meta.format) && meta.format != targetFormat)
        {
            DirectX::ScratchImage decompressed;
            const DXGI_FORMAT decompressFormat = DecompressionFormat(meta.format, settings.format);
            hr = DirectX::Decompress(image.GetImages(), image.GetImageCount(),
                                     image.GetMetadata(), decompressFormat, decompressed);
            if (FAILED(hr))
            {
                LOG_ERROR("TextureImporter: decompress failed for '%s' (hr=0x%08X)",
                          sourcePath.c_str(), static_cast<unsigned>(hr));
                return {};
            }
            image = std::move(decompressed);
            meta = image.GetMetadata();
        }

        if (!DirectX::IsCompressed(meta.format))
        {
            // Stamp or strip the metadata tag without changing pixels when an
            // equivalent sRGB/linear format exists.
            const bool wantsSRGB = settings.format == TextureImportFormat::BC7_SRGB;
            const DXGI_FORMAT tagged = wantsSRGB
                ? DirectX::MakeSRGB(meta.format)
                : DirectX::MakeLinear(meta.format);
            if (tagged != meta.format && image.OverrideFormat(tagged))
                meta = image.GetMetadata();

            if (meta.format != intermediate)
            {
                DirectX::ScratchImage converted;
                hr = DirectX::Convert(image.GetImages(), image.GetImageCount(),
                                      image.GetMetadata(), intermediate,
                                      DirectX::TEX_FILTER_DEFAULT,
                                      DirectX::TEX_THRESHOLD_DEFAULT, converted);
                if (FAILED(hr))
                {
                    LOG_ERROR("TextureImporter: format convert failed for '%s' (hr=0x%08X)",
                              sourcePath.c_str(), static_cast<unsigned>(hr));
                    return {};
                }
                image = std::move(converted);
                meta = image.GetMetadata();
            }
        }

        // Flip green, then the size cap, both before the mip chain is built. A source already in the target
        // BC format is copied as is: neither applies to it, and a warning says so.
        const bool uncompressed = !DirectX::IsCompressed(meta.format);
        if (settings.flipGreen)
        {
            if (!uncompressed || meta.format == DXGI_FORMAT_R16_UNORM)
                LOG_WARNING("TextureImporter: '%s' - flipGreen ignored (%s)", sourcePath.c_str(),
                            uncompressed ? "no green channel" : "the source is already the target BC format");
            else
            {
                DirectX::ScratchImage flipped;
                hr = DirectX::TransformImage(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                    [](DirectX::XMVECTOR* out, const DirectX::XMVECTOR* in, size_t width, size_t)
                    {
                        for (size_t i = 0; i < width; ++i)
                            out[i] = DirectX::XMVectorSetY(in[i], 1.f - DirectX::XMVectorGetY(in[i]));
                    }, flipped);
                if (FAILED(hr))
                {
                    LOG_ERROR("TextureImporter: flipGreen failed for '%s' (hr=0x%08X)", sourcePath.c_str(),
                              static_cast<unsigned>(hr));
                    return {};
                }
                image = std::move(flipped);
                meta  = image.GetMetadata();
            }
        }
        if (settings.maxSize > 0 && (meta.width > settings.maxSize || meta.height > settings.maxSize))
        {
            if (!uncompressed || meta.arraySize > 1 || meta.depth > 1)
                LOG_WARNING("TextureImporter: '%s' - maxSize %u ignored (%s)", sourcePath.c_str(), settings.maxSize,
                            uncompressed ? "not a single 2D image" : "the source is already the target BC format");
            else
            {
                // The longest edge to maxSize, the aspect kept; mip 0 only, the chain is rebuilt from the smaller
                // image below.
                const double s = double(settings.maxSize) / double((std::max)(meta.width, meta.height));
                const size_t w = (std::max)(size_t(1), static_cast<size_t>(double(meta.width) * s + 0.5));
                const size_t h = (std::max)(size_t(1), static_cast<size_t>(double(meta.height) * s + 0.5));
                DirectX::ScratchImage resized;
                hr = DirectX::Resize(*image.GetImage(0, 0, 0), w, h, FilterFlags(settings.mipFilter), resized);
                if (FAILED(hr))
                {
                    LOG_ERROR("TextureImporter: resize to %zux%zu failed for '%s' (hr=0x%08X)", w, h,
                              sourcePath.c_str(), static_cast<unsigned>(hr));
                    return {};
                }
                image = std::move(resized);
                meta  = image.GetMetadata();
            }
        }

        const bool needsMips = settings.generateMips && meta.mipLevels == 1 &&
                               !DirectX::IsCompressed(meta.format) &&
                               (meta.width > 1 || meta.height > 1);
        if (needsMips)
        {
            DirectX::ScratchImage mipped;
            hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(),
                                           image.GetMetadata(), FilterFlags(settings.mipFilter),
                                           0, mipped);
            if (SUCCEEDED(hr))
            {
                image = std::move(mipped);
                meta = image.GetMetadata();
            }
            else
            {
                LOG_WARNING("TextureImporter: GenerateMipMaps failed for '%s' (hr=0x%08X)",
                            sourcePath.c_str(), static_cast<unsigned>(hr));
            }
        }

        if (IsBCFormat(settings.format) && meta.format != targetFormat)
        {
            DirectX::ScratchImage compressed;
            hr = BCCompressor::Get().Compress(image.GetImages(), image.GetImageCount(),
                                               image.GetMetadata(), targetFormat, compressed);
            if (FAILED(hr))
            {
                LOG_ERROR("TextureImporter: BC compress (fmt=0x%X) failed for '%s' (hr=0x%08X)",
                          static_cast<unsigned>(targetFormat), sourcePath.c_str(),
                          static_cast<unsigned>(hr));
                return {};
            }
            image = std::move(compressed);
            meta = image.GetMetadata();
        }

        DirectX::Blob ddsBlob;
        hr = DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(),
                                       image.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsBlob);
        if (FAILED(hr))
        {
            LOG_ERROR("TextureImporter: SaveToDDSMemory failed for '%s' (hr=0x%08X)",
                      sourcePath.c_str(), static_cast<unsigned>(hr));
            return {};
        }

        meta = image.GetMetadata();
        TextureMetadata texMeta{};
        texMeta.width     = static_cast<uint32_t>(meta.width);
        texMeta.height    = static_cast<uint32_t>(meta.height);
        texMeta.depth     = static_cast<uint32_t>(meta.depth);
        texMeta.mipLevels = static_cast<uint16_t>(meta.mipLevels);
        texMeta.format    = static_cast<uint16_t>(meta.format);
        texMeta.dimension = static_cast<uint8_t>(meta.dimension);

        AssetHeader header{};
        header.magic        = MAGIC_TEXTURE;
        header.version      = CurrentVersion(MAGIC_TEXTURE);
        header.resourceType = static_cast<uint16_t>(ResourceType::Texture);
        header.metadataSize = sizeof(TextureMetadata);
        header.dataSize     = static_cast<uint32_t>(ddsBlob.GetBufferSize());

        const size_t totalSize = sizeof(AssetHeader) + sizeof(TextureMetadata) +
                                 ddsBlob.GetBufferSize();
        std::vector<uint8_t> blob(totalSize);
        uint8_t* dst = blob.data();
        std::memcpy(dst, &header, sizeof(AssetHeader));
        dst += sizeof(AssetHeader);
        std::memcpy(dst, &texMeta, sizeof(TextureMetadata));
        dst += sizeof(TextureMetadata);
        std::memcpy(dst, ddsBlob.GetBufferPointer(), ddsBlob.GetBufferSize());

        LOG_INFO("TextureImporter: '%s' -> %ux%u %u mips fmt=0x%X preset=%s (%zu B) maxSize=%u mipFilter=%s "
                 "flipGreen=%d mips=%d", sourcePath.c_str(), texMeta.width, texMeta.height, texMeta.mipLevels,
                 static_cast<unsigned>(meta.format), GetFormatName(settings.format), totalSize, settings.maxSize,
                 GetMipFilterId(settings.mipFilter), settings.flipGreen ? 1 : 0, settings.generateMips ? 1 : 0);
        return blob;
    }
}
