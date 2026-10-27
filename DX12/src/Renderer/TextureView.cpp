// TextureView.cpp - see the header.
#include "Renderer/TextureView.h"

#include <cctype>
#include <cstring>

namespace TextureView
{
    SourceKind KindOf(const RHI::TextureDesc& d)
    {
        // The SAME predicate GraphicsDX12::CreateTexture builds the default SRV
        // with (GraphicsDX12_Resources.cpp): reading that descriptor through any
        // other view type is silent UB under Resource Binding Tier 3.
        if (d.sampleCount > 1) return SourceKind::Unsupported;
        if (d.type == RHI::TextureDesc::Type::TEXTURE_1D) return SourceKind::Unsupported;
        if (d.type == RHI::TextureDesc::Type::TEXTURE_3D) return SourceKind::Tex3D;
        const bool cube = RHI::HasFlag(d.miscFlags, RHI::ResourceMiscFlag::TEXTURECUBE);
        if (cube && d.arraySize == 6) return SourceKind::Cube;
        if (cube && d.arraySize > 6)  return SourceKind::Unsupported;   // a TEXTURECUBEARRAY view
        if (d.arraySize > 1)          return SourceKind::Tex2DArray;
        return SourceKind::Tex2D;
    }

    const char* KindName(SourceKind k)
    {
        switch (k)
        {
        case SourceKind::Tex2D:      return "2D";
        case SourceKind::Tex2DArray: return "2DArray";
        case SourceKind::Cube:       return "Cube";
        case SourceKind::Tex3D:      return "3D";
        default:                     return "unsupported";
        }
    }

    bool IsSrgbFormat(RHI::Format f)
    {
        using F = RHI::Format;
        switch (f)
        {
        case F::R8G8B8A8_UNORM_SRGB: case F::B8G8R8A8_UNORM_SRGB:
        case F::BC1_UNORM_SRGB: case F::BC2_UNORM_SRGB: case F::BC3_UNORM_SRGB: case F::BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    bool IsOneChannelFormat(RHI::Format f)
    {
        using F = RHI::Format;
        switch (f)
        {
        case F::R8_UNORM: case F::R8_SNORM: case F::R16_UNORM: case F::R16_SNORM: case F::R16_FLOAT:
        case F::R32_FLOAT: case F::BC4_UNORM: case F::BC4_SNORM:
        case F::D32_FLOAT: case F::D16_UNORM: case F::D24_UNORM_S8_UINT: case F::D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
        }
    }

    std::uint64_t Hash(const Desc& d)
    {
        // FNV-1a over the fields one at a time (no padding bytes).
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void* p, std::size_t n)
        {
            const unsigned char* b = static_cast<const unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        auto u32 = [&](std::uint32_t v) { mix(&v, sizeof(v)); };
        auto f32 = [&](float v) { mix(&v, sizeof(v)); };
        u32(d.source.handleId);
        u32(d.source.desc.width); u32(d.source.desc.height); u32(d.source.desc.mipLevels);
        u32(d.source.desc.arraySize); u32(static_cast<std::uint32_t>(d.source.desc.format));
        mix(&d.sourceRevision, sizeof(d.sourceRevision));
        u32(d.width); u32(d.height); u32(d.mip); u32(d.slice);
        u32(static_cast<std::uint32_t>(d.cubeLayout)); u32(d.channels); u32(static_cast<std::uint32_t>(d.map));
        f32(d.exposureEV); u32(d.tonemap ? 1u : 0u); f32(d.rangeMin); f32(d.rangeMax);
        u32(static_cast<std::uint32_t>(d.background)); u32(d.filtered ? 1u : 0u); u32(d.tile ? 1u : 0u);
        f32(d.uvOrigin[0]); f32(d.uvOrigin[1]); f32(d.uvPerPixel[0]); f32(d.uvPerPixel[1]);
        if (d.compare != Compare::Off)
        {
            u32(static_cast<std::uint32_t>(d.compare));
            u32(d.compareSource.handleId);
            u32(d.compareSource.desc.width); u32(d.compareSource.desc.height);
            u32(d.compareSource.desc.mipLevels); u32(static_cast<std::uint32_t>(d.compareSource.desc.format));
            mix(&d.compareRevision, sizeof(d.compareRevision));
            u32(d.compareMip); u32(d.compareSlice); f32(d.splitX); f32(d.diffScale);
        }
        return h ? h : 1ull;   // 0 means "nothing asked"
    }

    namespace
    {
        std::string Lower(std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
    }

    const char* MapName(Map m)
    {
        switch (m)
        {
        case Map::Stored: return "stored";
        case Map::Linear: return "linear";
        case Map::Hdr:    return "hdr";
        case Map::Normal: return "normal";
        case Map::Range:  return "range";
        }
        return "stored";
    }

    bool ParseMap(const std::string& s, Map& out)
    {
        const std::string l = Lower(s);
        for (Map m : { Map::Stored, Map::Linear, Map::Hdr, Map::Normal, Map::Range })
            if (l == MapName(m)) { out = m; return true; }
        return false;
    }

    const char* BackgroundName(Background b)
    {
        switch (b)
        {
        case Background::Checker: return "checker";
        case Background::Black:   return "black";
        case Background::Grey:    return "grey";
        case Background::White:   return "white";
        }
        return "checker";
    }

    bool ParseBackground(const std::string& s, Background& out)
    {
        std::string l = Lower(s);
        if (l == "gray") l = "grey";
        for (Background b : { Background::Checker, Background::Black, Background::Grey, Background::White })
            if (l == BackgroundName(b)) { out = b; return true; }
        return false;
    }

    const char* CubeLayoutName(CubeLayout l)
    {
        switch (l)
        {
        case CubeLayout::Face:     return "face";
        case CubeLayout::Cross:    return "cross";
        case CubeLayout::Panorama: return "pano";
        }
        return "face";
    }

    bool ParseCubeLayout(const std::string& s, CubeLayout& out)
    {
        const std::string l = Lower(s);
        if (l == "face")                        { out = CubeLayout::Face;     return true; }
        if (l == "cross")                       { out = CubeLayout::Cross;    return true; }
        if (l == "pano" || l == "panorama")     { out = CubeLayout::Panorama; return true; }
        return false;
    }

    const char* CompareName(Compare c)
    {
        switch (c)
        {
        case Compare::Off:   return "off";
        case Compare::Split: return "split";
        case Compare::Diff:  return "diff";
        }
        return "off";
    }

    bool ParseCompare(const std::string& s, Compare& out)
    {
        const std::string l = Lower(s);
        for (Compare c : { Compare::Off, Compare::Split, Compare::Diff })
            if (l == CompareName(c)) { out = c; return true; }
        return false;
    }

    std::string ChannelsName(std::uint32_t mask)
    {
        std::string s;
        if (mask & kChR) s += 'r';
        if (mask & kChG) s += 'g';
        if (mask & kChB) s += 'b';
        if (mask & kChA) s += 'a';
        return s;
    }

    bool ParseChannels(const std::string& s, std::uint32_t& out)
    {
        if (s.empty()) return false;
        std::uint32_t m = 0;
        for (char c : Lower(s))
        {
            if (c == 'r') m |= kChR;
            else if (c == 'g') m |= kChG;
            else if (c == 'b') m |= kChB;
            else if (c == 'a') m |= kChA;
            else return false;
        }
        out = m;
        return true;
    }

    const char* FaceName(std::uint32_t face)
    {
        static const char* kNames[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
        return face < 6 ? kNames[face] : "?";
    }
}
