// TextureInfo.cpp - .itex header description without a device; the contract is in the header.
#include "Resource/TextureInfo.h"

#include "Graphics/FormatNames.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"
#include "Resource/AssetHeader.h"
#include "System/PathUtil.h"

#include <DirectXTex.h>

#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // AssetHeader (24) + TextureMetadata (20) + "DDS " (4) + DDS_HEADER (124)
    // + DDS_HEADER_DXT10 (20) = 192; room for a larger metadata block.
    constexpr size_t kHeadBytes = 4096;

    struct Located
    {
        bool                  packed = false;
        std::filesystem::path disk;
        uint64_t              fileBytes = 0;
        int64_t               stamp = 0;
    };

    struct CacheEntry
    {
        uint64_t                  fileBytes = 0;
        int64_t                   stamp     = 0;
        Resource::TextureFileInfo info;
    };
    std::mutex                                  g_cacheMutex;
    std::unordered_map<std::string, CacheEntry> g_cache;   // folded canonical path

    std::string Canon(const std::string& p)
    {
        std::string c = Sys::RelativizeAssetPath(p);
        for (char& ch : c) if (ch == '\\') ch = '/';
        return c;
    }

    std::string Fold(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    const char* AlphaModeName(DirectX::TEX_ALPHA_MODE m)
    {
        switch (m)
        {
        case DirectX::TEX_ALPHA_MODE_STRAIGHT:      return "straight";
        case DirectX::TEX_ALPHA_MODE_PREMULTIPLIED: return "premultiplied";
        case DirectX::TEX_ALPHA_MODE_OPAQUE:        return "opaque";
        case DirectX::TEX_ALPHA_MODE_CUSTOM:        return "custom";
        default:                                    return "unknown";
        }
    }

    // The TextureImporter format id a DXGI format comes out of
    // (TextureImporter::GetFormatId spellings; ImportInternal's table).
    const char* PresetFor(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_BC7_UNORM_SRGB:     return "BC7_SRGB";
        case DXGI_FORMAT_BC7_UNORM:          return "BC7_Linear";
        case DXGI_FORMAT_BC5_UNORM:          return "BC5_Linear";
        case DXGI_FORMAT_BC4_UNORM:          return "BC4_Linear";
        case DXGI_FORMAT_BC6H_UF16:          return "BC6H_UF16";
        case DXGI_FORMAT_R16_UNORM:          return "R16_UNorm";
        case DXGI_FORMAT_R16G16_FLOAT:       return "RG16_Float";
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return "RGBA8_Linear";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16_UNorm";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16_Float";
        default:                             return "";
        }
    }

    // Where the file is and its (size, stamp) - the cache key's validity - without
    // reading it. A pak entry is immutable: stamp -1, size unknown until read.
    bool Locate(const std::string& path, Located& out)
    {
        Resource::AssetFS& fs = Resource::AssetFS::Get();
        if (fs.HasInPak(path))
        {
            out.packed = true;
            out.stamp  = -1;
            return true;
        }
        std::string onDisk = fs.ResolveOnDisk(path);
        if (onDisk.empty()) onDisk = path;
        std::error_code ec;
        out.disk = Sys::U8Path(onDisk);
        const auto size = std::filesystem::file_size(out.disk, ec);
        if (ec) return false;
        const auto when = std::filesystem::last_write_time(out.disk, ec);
        if (ec) return false;
        out.fileBytes = size;
        out.stamp     = static_cast<int64_t>(when.time_since_epoch().count());
        return true;
    }

    // The first kHeadBytes of a located file.
    bool ReadHead(const std::string& path, Located& loc, std::vector<uint8_t>& head)
    {
        if (loc.packed)
        {
            Resource::AssetFS& fs = Resource::AssetFS::Get();
            const std::optional<std::uint64_t> size = fs.RawSize(path);
            if (!size || !fs.ReadRange(path, 0, kHeadBytes, head)) return false;
            loc.fileBytes = *size;
            return true;
        }
        std::ifstream f(loc.disk, std::ios::binary);
        if (!f) return false;
        head.resize(static_cast<size_t>((std::min<uint64_t>)(loc.fileBytes, kHeadBytes)));
        f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(f.gcount()));
        return true;
    }

    Resource::TextureFileInfo Parse(const std::string& canon, const std::vector<uint8_t>& head,
                                    uint64_t fileBytes, bool packed)
    {
        using namespace Resource;
        TextureFileInfo out;
        out.path      = canon;
        out.fileBytes = fileBytes;
        out.packed    = packed;
        if (const ECS::Guid g = AssetGuidIndex::Get().GuidForPath(canon); g.IsValid())
            out.guid = g.ToHex();

        if (head.size() < sizeof(AssetHeader))
        {
            out.error = "too small for an asset header";
            return out;
        }
        AssetHeader hdr{};
        std::memcpy(&hdr, head.data(), sizeof(hdr));
        if (hdr.magic != MAGIC_TEXTURE)
        {
            out.error = "not a .itex (magic)";
            return out;
        }
        if (hdr.version > CurrentVersion(MAGIC_TEXTURE) || hdr.version < MinimumVersion(MAGIC_TEXTURE))
        {
            out.error = "unsupported .itex version " + std::to_string(hdr.version) + " (this engine reads " +
                        std::to_string(MinimumVersion(MAGIC_TEXTURE)) + ".." +
                        std::to_string(CurrentVersion(MAGIC_TEXTURE)) + ")";
            return out;
        }
        const uint64_t payloadAt = sizeof(AssetHeader) + uint64_t(hdr.metadataSize);
        if (payloadAt + hdr.dataSize > fileBytes)
        {
            out.error = "truncated: the header promises " + std::to_string(payloadAt + hdr.dataSize) +
                        " bytes, the file has " + std::to_string(fileBytes);
            return out;
        }
        if (hdr.metadataSize >= sizeof(TextureMetadata) && head.size() >= sizeof(AssetHeader) + sizeof(TextureMetadata))
        {
            TextureMetadata tm{};
            std::memcpy(&tm, head.data() + sizeof(AssetHeader), sizeof(tm));
            out.headerDimension = tm.dimension;
        }
        if (payloadAt >= head.size())
        {
            out.error = "metadata block larger than expected";
            return out;
        }

        DirectX::TexMetadata md{};
        const HRESULT hr = DirectX::GetMetadataFromDDSMemory(head.data() + payloadAt,
                                                             (std::min<uint64_t>)(hdr.dataSize, head.size() - payloadAt),
                                                             DirectX::DDS_FLAGS_NONE, md);
        if (FAILED(hr))
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "bad DDS header (hr=0x%08X)", static_cast<unsigned>(hr));
            out.error = buf;
            return out;
        }

        out.width     = static_cast<uint32_t>(md.width);
        out.height    = static_cast<uint32_t>(md.height);
        out.depth     = static_cast<uint32_t>(md.depth);
        out.arraySize = static_cast<uint32_t>(md.arraySize);
        out.mipLevels = static_cast<uint32_t>(md.mipLevels);
        out.isCube    = md.IsCubemap();
        out.cubeCount = out.isCube ? out.arraySize / 6 : 0;
        switch (md.dimension)
        {
        case DirectX::TEX_DIMENSION_TEXTURE1D: out.dimension = "1D"; break;
        case DirectX::TEX_DIMENSION_TEXTURE3D: out.dimension = "3D"; break;
        default:                               out.dimension = out.isCube ? "Cube" : "2D"; break;
        }

        out.dxgiFormat   = static_cast<uint32_t>(md.format);
        const char* n    = RHI::DxgiFormatName(out.dxgiFormat);
        out.formatName   = n ? n : ("DXGI " + std::to_string(out.dxgiFormat));
        out.formatLabel  = RHI::DxgiFormatLabel(out.dxgiFormat);
        out.isSRGB       = DirectX::IsSRGB(md.format);
        out.isCompressed = DirectX::IsCompressed(md.format);
        out.bitsPerPixel = static_cast<uint32_t>(DirectX::BitsPerPixel(md.format));
        out.blockSize    = out.isCompressed ? 4u : 1u;
        out.alphaMode    = AlphaModeName(md.GetAlphaMode());
        out.preset       = PresetFor(md.format);

        uint32_t w = out.width, h = out.height, d = out.depth;
        for (uint32_t m = 0; m < out.mipLevels; ++m)
        {
            TextureFileInfo::Mip mip;
            mip.width  = w;
            mip.height = h;
            mip.depth  = d;
            size_t rowPitch = 0, slicePitch = 0;
            if (SUCCEEDED(DirectX::ComputePitch(md.format, w, h, rowPitch, slicePitch)))
            {
                mip.rowPitch   = rowPitch;
                mip.slicePitch = slicePitch;
                mip.bytes      = uint64_t(slicePitch) * d;
            }
            out.dataBytes += mip.bytes * (md.dimension == DirectX::TEX_DIMENSION_TEXTURE3D ? 1u : out.arraySize);
            out.mips.push_back(mip);
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
            d = (std::max)(1u, d / 2);
        }

        // What the AssetHeader's TextureMetadata says, when it disagrees.
        const uint32_t hd = out.headerDimension;
        const bool agrees = (hd == 6 && out.isCube) ||
                            (hd == 3 && (out.dimension == "2D" || out.dimension == "Cube")) ||   // DirectXTex's TEXTURE2D
                            (hd == 2 && out.dimension == "2D") ||
                            (hd == 4 && out.dimension == "3D") ||
                            (hd == 1 && out.dimension == "1D");
        if (!agrees)
            out.headerMismatch = "TextureMetadata.dimension=" + std::to_string(hd) + " but the DDS header says " +
                                 out.dimension;
        out.ok = true;
        return out;
    }
}

namespace Resource
{
    TextureFileInfo DescribeTextureFile(const std::string& pathIn)
    {
        const std::string canon = Canon(pathIn);
        TextureFileInfo missing;
        missing.path  = canon;
        missing.error = "missing";
        Located loc;
        if (canon.empty() || !Locate(canon, loc)) return missing;

        const std::string key = Fold(canon);
        {
            std::lock_guard<std::mutex> lk(g_cacheMutex);
            auto it = g_cache.find(key);
            if (it != g_cache.end() && it->second.stamp == loc.stamp &&
                (loc.packed || it->second.fileBytes == loc.fileBytes))
                return it->second.info;
        }
        std::vector<uint8_t> head;
        if (!ReadHead(canon, loc, head)) return missing;
        TextureFileInfo info = Parse(canon, head, loc.fileBytes, loc.packed);
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        g_cache[key] = CacheEntry{ loc.fileBytes, loc.stamp, info };
        return info;
    }

    std::string FormatByteSize(uint64_t bytes)
    {
        char buf[48];
        if (bytes < 1024ull)                     std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
        else if (bytes < 1024ull * 1024)         std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        else if (bytes < 1024ull * 1024 * 1024)  std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        else                                     std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }

    std::string TextureInfoSummary(const TextureFileInfo& i)
    {
        if (!i.ok) return {};
        std::string s;
        if (i.isCube) s += "Cube ";
        else if (i.dimension == "3D") s += "3D ";
        s += std::to_string(i.width) + "x" + std::to_string(i.height);
        if (i.dimension == "3D") s += "x" + std::to_string(i.depth);
        if (i.isCube && i.cubeCount > 1) s += " x" + std::to_string(i.cubeCount);
        else if (!i.isCube && i.arraySize > 1) s += " x" + std::to_string(i.arraySize);
        constexpr const char* kDot = " · ";   // U+00B7 MIDDLE DOT
        s += kDot + i.formatLabel;
        s += kDot + std::to_string(i.mipLevels) + (i.mipLevels == 1 ? " mip" : " mips");
        s += kDot + FormatByteSize(i.dataBytes);
        return s;
    }
}
