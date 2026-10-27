// TextureAnalysis.cpp - CPU texel statistics and stored-texel read-out; the contract is in the header.
#include "Resource/TextureAnalysis.h"
#include "Resource/ItexDecode.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "System/CommandLine.h"   // -texviewer_stats_dxtex_half (a reverse arm)
#include "System/PathUtil.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Resource
{
    bool DecodeItexBlob(const std::uint8_t* blob, std::size_t size, DirectX::TexMetadata& metadata,
                        DirectX::ScratchImage& image, std::string& error)
    {
        if (!blob || size == 0)
        {
            error = "empty file";
            return false;
        }
        if (!ValidateHeader(blob, size, MAGIC_TEXTURE, error))
            return false;
        const AssetHeader* hdr = GetHeader(blob);
        const HRESULT hr = DirectX::LoadFromDDSMemory(GetPayload(blob), hdr->dataSize, DirectX::DDS_FLAGS_NONE,
                                                      &metadata, image);
        if (FAILED(hr))
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "LoadFromDDSMemory failed (hr=0x%08X)", static_cast<unsigned>(hr));
            error = buf;
            return false;
        }
        return true;
    }
}

namespace
{
    std::string Fold(std::string s)
    {
        for (char& c : s)
        {
            if (c == '\\') c = '/';
            else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    // ==== The decoded-file cache ====
    struct Decoded
    {
        std::uint64_t         fileBytes = 0;
        std::int64_t          stamp = 0;
        DirectX::TexMetadata  md{};
        DirectX::ScratchImage img;
        std::uint64_t         bytes = 0;
    };
    struct Cache
    {
        std::mutex mutex;
        std::list<std::string> lru;   // front = most recent
        std::unordered_map<std::string, std::pair<std::shared_ptr<Decoded>, std::list<std::string>::iterator>> map;
        std::uint64_t bytes = 0;
        std::uint64_t cap = 256ull * 1024 * 1024;
    };
    Cache& TheCache()
    {
        static Cache s_cache;
        return s_cache;
    }

    // (size, stamp) of the file behind @p path; a pak entry is stamp -1.
    bool Stat(const std::string& path, std::uint64_t& size, std::int64_t& stamp)
    {
        Resource::AssetFS& fs = Resource::AssetFS::Get();
        if (fs.HasInPak(path)) { size = 0; stamp = -1; return true; }
        std::string disk = fs.ResolveOnDisk(path);
        if (disk.empty()) disk = path;
        std::error_code ec;
        const std::filesystem::path p = Sys::U8Path(disk);
        const auto s = std::filesystem::file_size(p, ec);
        if (ec) return false;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return false;
        size  = s;
        stamp = static_cast<std::int64_t>(t.time_since_epoch().count());
        return true;
    }

    std::shared_ptr<const Decoded> GetDecoded(const std::string& path, std::string& error)
    {
        std::uint64_t size = 0;
        std::int64_t  stamp = 0;
        if (!Stat(path, size, stamp)) { error = "missing"; return nullptr; }
        const std::string key = Fold(path);
        Cache& c = TheCache();
        {
            std::lock_guard<std::mutex> lk(c.mutex);
            auto it = c.map.find(key);
            if (it != c.map.end())
            {
                const auto& d = it->second.first;
                if (d->stamp == stamp && (stamp == -1 || d->fileBytes == size))
                {
                    c.lru.splice(c.lru.begin(), c.lru, it->second.second);
                    return d;
                }
                c.bytes -= d->bytes;
                c.lru.erase(it->second.second);
                c.map.erase(it);
            }
        }
        std::vector<std::uint8_t> blob;
        if (!Resource::AssetFS::Get().ReadFile(path, blob)) { error = "missing"; return nullptr; }
        auto d = std::make_shared<Decoded>();
        d->fileBytes = size;
        d->stamp     = stamp;
        if (!Resource::DecodeItexBlob(blob.data(), blob.size(), d->md, d->img, error)) return nullptr;
        d->bytes = d->img.GetPixelsSize();
        std::lock_guard<std::mutex> lk(c.mutex);
        c.lru.push_front(key);
        c.map[key] = { d, c.lru.begin() };
        c.bytes += d->bytes;
        while (c.bytes > c.cap && c.lru.size() > 1)
        {
            const std::string& victim = c.lru.back();
            auto v = c.map.find(victim);
            if (v != c.map.end())
            {
                c.bytes -= v->second.first->bytes;
                c.map.erase(v);
            }
            c.lru.pop_back();
        }
        return d;
    }

    // The image of (mip, slice): an array slice / cube face, or a 3D slice.
    const DirectX::Image* ImageOf(const Decoded& d, std::uint32_t mip, std::uint32_t slice)
    {
        if (d.md.dimension == DirectX::TEX_DIMENSION_TEXTURE3D) return d.img.GetImage(mip, 0, slice);
        return d.img.GetImage(mip, slice, 0);
    }

    // IEEE binary16 -> binary32 with specials kept: DirectXTex's conversion reads exponent 31 as an
    // ordinary number (a half NaN becomes 98304, +Inf 65536), which would hide exactly the NaN / Inf
    // texels the viewer must count exactly.
    float HalfToFloat(std::uint16_t h)
    {
        const std::uint32_t sign = std::uint32_t(h >> 15) << 31;
        std::uint32_t exp  = (h >> 10) & 0x1Fu;
        std::uint32_t mant = h & 0x3FFu;
        std::uint32_t f;
        if (exp == 0x1Fu)
            f = sign | 0x7F800000u | (mant << 13);            // Inf / NaN
        else if (exp != 0)
            f = sign | ((exp + 112u) << 23) | (mant << 13);   // normal
        else if (mant == 0)
            f = sign;                                          // +-0
        else
        {
            exp = 113u;                                        // subnormal: normalise
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            f = sign | (exp << 23) | ((mant & 0x3FFu) << 13);
        }
        float out;
        std::memcpy(&out, &f, sizeof(out));
        return out;
    }

    // The half-float formats, converted by hand (see HalfToFloat); false = not one.
    bool HalfToFloatImage(const DirectX::Image& src, DirectX::ScratchImage& out, std::string& error)
    {
        std::uint32_t ch = 0;
        switch (src.format)
        {
        case DXGI_FORMAT_R16_FLOAT:          ch = 1; break;
        case DXGI_FORMAT_R16G16_FLOAT:       ch = 2; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: ch = 4; break;
        default: return false;
        }
        if (FAILED(out.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, src.width, src.height, 1, 1)))
        {
            error = "cannot allocate the RGBA32F copy";
            return true;
        }
        const DirectX::Image& dst = *out.GetImage(0, 0, 0);
        for (std::size_t y = 0; y < src.height; ++y)
        {
            const std::uint16_t* s = reinterpret_cast<const std::uint16_t*>(src.pixels + y * src.rowPitch);
            float*               d = reinterpret_cast<float*>(dst.pixels + y * dst.rowPitch);
            for (std::size_t x = 0; x < src.width; ++x)
            {
                d[x * 4 + 0] = HalfToFloat(s[x * ch]);
                d[x * 4 + 1] = ch > 1 ? HalfToFloat(s[x * ch + 1]) : 0.f;
                d[x * 4 + 2] = ch > 2 ? HalfToFloat(s[x * ch + 2]) : 0.f;
                d[x * 4 + 3] = ch > 3 ? HalfToFloat(s[x * ch + 3]) : 1.f;
            }
        }
        return true;
    }

    // @p src as stored (an sRGB format read as its linear twin: the stored bytes
    // / 255, never gamma-decoded) converted to RGBA32F.
    bool ToFloat(const DirectX::Image& src, DirectX::ScratchImage& out, std::string& error)
    {
        DirectX::Image view = src;
        view.format = DirectX::MakeLinear(src.format);
        error.clear();
        // The reverse arm of the texviewer_stats pack: DirectXTex's own half conversion, which loses
        // NaN / Inf, so the pack's exact counts must go red.
        static const bool s_dxtexHalf = Sys::CommandLine::Get().Has("-texviewer_stats_dxtex_half");
        if (!s_dxtexHalf && HalfToFloatImage(view, out, error)) return error.empty();
        HRESULT hr = S_OK;
        if (DirectX::IsCompressed(view.format))
            hr = DirectX::Decompress(view, DXGI_FORMAT_R32G32B32A32_FLOAT, out);
        else if (view.format == DXGI_FORMAT_R32G32B32A32_FLOAT)
            hr = out.InitializeFromImage(view);
        else
            hr = DirectX::Convert(view, DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT,
                                  DirectX::TEX_THRESHOLD_DEFAULT, out);
        if (FAILED(hr))
        {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "cannot convert to RGBA32F (hr=0x%08X)", static_cast<unsigned>(hr));
            error = buf;
            return false;
        }
        return true;
    }

    // How many channels a format stores (a channel it does not store reads as
    // 0 / 1 and would only pollute the statistics).
    std::uint32_t StoredChannels(DXGI_FORMAT f)
    {
        switch (DirectX::MakeLinear(f))
        {
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
            return 2;
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_B8G8R8X8_UNORM:
            return 3;
        default:
            return 4;
        }
    }

    std::string Hex(const std::uint8_t* p, std::size_t n)
    {
        static const char* kDigits = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i)
        {
            s += kDigits[p[i] >> 4];
            s += kDigits[p[i] & 15];
        }
        return s;
    }

    // Mean per channel of one converted image (finite values only).
    std::array<double, 4> MeanOf(const DirectX::Image& f)
    {
        std::array<double, 4> sum{};
        std::array<std::uint64_t, 4> n{};
        for (std::size_t y = 0; y < f.height; ++y)
        {
            const float* row = reinterpret_cast<const float*>(f.pixels + y * f.rowPitch);
            for (std::size_t x = 0; x < f.width; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const float v = row[x * 4 + c];
                    if (std::isfinite(v)) { sum[c] += v; ++n[c]; }
                }
        }
        for (int c = 0; c < 4; ++c) sum[c] = n[c] ? sum[c] / double(n[c]) : 0.0;
        return sum;
    }
}

namespace Resource
{
    void SetTextureAnalysisCacheBytes(std::uint64_t cap)
    {
        std::lock_guard<std::mutex> lk(TheCache().mutex);
        TheCache().cap = cap;
    }

    std::uint64_t TextureAnalysisCacheBytes()
    {
        std::lock_guard<std::mutex> lk(TheCache().mutex);
        return TheCache().bytes;
    }

    void ClearTextureAnalysisCache()
    {
        Cache& c = TheCache();
        std::lock_guard<std::mutex> lk(c.mutex);
        c.map.clear();
        c.lru.clear();
        c.bytes = 0;
    }

    TextureStats AnalyzeTexture(const std::string& pathIn, const AnalyzeOptions& o)
    {
        const auto t0 = std::chrono::steady_clock::now();
        TextureStats s;
        s.mip   = o.mip;
        s.slice = o.slice;
        std::string path = Sys::RelativizeAssetPath(pathIn);
        for (char& c : path) if (c == '\\') c = '/';
        const std::shared_ptr<const Decoded> d = GetDecoded(path, s.error);
        if (!d) return s;
        const DirectX::TexMetadata& md = d->md;
        if (o.mip >= md.mipLevels)
        {
            s.error = "mip " + std::to_string(o.mip) + " of " + std::to_string(md.mipLevels);
            return s;
        }
        const bool is3D = md.dimension == DirectX::TEX_DIMENSION_TEXTURE3D;
        const std::uint32_t depthAtMip = static_cast<std::uint32_t>((std::max<std::size_t>)(1, md.depth >> o.mip));
        const std::uint32_t slices = is3D ? depthAtMip : static_cast<std::uint32_t>(md.arraySize);
        const bool allSlices = is3D && o.slice == ~0u;
        if (!allSlices && o.slice >= slices)
        {
            s.error = "slice " + std::to_string(o.slice) + " of " + std::to_string(slices);
            return s;
        }
        const DXGI_FORMAT fmt = md.format;
        s.channels = StoredChannels(fmt);
        s.hasAlpha = DirectX::HasAlpha(fmt) && s.channels == 4;
        const DirectX::FORMAT_TYPE type = DirectX::FormatDataType(DirectX::MakeLinear(fmt));

        // Convert the asked slice(s) to RGBA32F.
        std::vector<DirectX::ScratchImage> conv;
        const std::uint32_t z0 = allSlices ? 0u : o.slice, z1 = allSlices ? depthAtMip : o.slice + 1u;
        for (std::uint32_t z = z0; z < z1; ++z)
        {
            const DirectX::Image* img = ImageOf(*d, o.mip, z);
            if (!img) { s.error = "no image for mip " + std::to_string(o.mip) + " slice " + std::to_string(z); return s; }
            DirectX::ScratchImage f;
            if (!ToFloat(*img, f, s.error)) return s;
            conv.push_back(std::move(f));
        }
        const DirectX::Image& first = *conv.front().GetImage(0, 0, 0);
        s.width  = static_cast<std::uint32_t>(first.width);
        s.height = static_cast<std::uint32_t>(first.height);
        s.depth  = static_cast<std::uint32_t>(conv.size());

        // Pass 1: min / max / mean / stddev / NaN / Inf, alpha, the normal check.
        std::array<double, 4> sum{}, sum2{};
        std::array<std::uint64_t, 4> finite{};
        for (int c = 0; c < 4; ++c) { s.min[c] = HUGE_VAL; s.max[c] = -HUGE_VAL; }
        bool alphaOpaque = true, alphaBinary = true;
        const bool normal = o.normalCheck || s.channels == 2;
        std::uint64_t outside = 0, zNeg = 0;
        double lenDev = 0.0;
        std::uint64_t normalTexels = 0;
        for (const DirectX::ScratchImage& sc : conv)
        {
            const DirectX::Image& f = *sc.GetImage(0, 0, 0);
            for (std::size_t y = 0; y < f.height; ++y)
            {
                const float* row = reinterpret_cast<const float*>(f.pixels + y * f.rowPitch);
                for (std::size_t x = 0; x < f.width; ++x)
                {
                    const float* v = row + x * 4;
                    for (int c = 0; c < 4; ++c)
                    {
                        const float vc = v[c];
                        if (std::isnan(vc)) { ++s.nan[c]; continue; }
                        if (std::isinf(vc)) { ++s.inf[c]; continue; }
                        s.min[c] = (std::min)(s.min[c], double(vc));
                        s.max[c] = (std::max)(s.max[c], double(vc));
                        sum[c] += vc;
                        sum2[c] += double(vc) * vc;
                        ++finite[c];
                    }
                    if (s.hasAlpha && std::isfinite(v[3]))
                    {
                        const float a = v[3];
                        if (a < 1.f - 1.5f / 255.f) alphaOpaque = false;
                        if (a > 1.5f / 255.f && a < 1.f - 1.5f / 255.f) alphaBinary = false;
                    }
                    if (normal && std::isfinite(v[0]) && std::isfinite(v[1]))
                    {
                        const bool snorm = type == DirectX::FORMAT_TYPE_SNORM;
                        const double nx = snorm ? v[0] : v[0] * 2.0 - 1.0;
                        const double ny = snorm ? v[1] : v[1] * 2.0 - 1.0;
                        ++normalTexels;
                        if (s.channels == 2)
                        {
                            if (nx * nx + ny * ny > 1.0 + 1e-3) ++outside;
                        }
                        else if (std::isfinite(v[2]))
                        {
                            const double nz = snorm ? v[2] : v[2] * 2.0 - 1.0;
                            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
                            lenDev += std::fabs(len - 1.0);
                            if (nz < 0.0) ++zNeg;
                            if (nx * nx + ny * ny > 1.0 + 1e-3) ++outside;
                        }
                    }
                }
            }
        }
        s.texels = std::uint64_t(s.width) * s.height * s.depth;
        for (int c = 0; c < 4; ++c)
        {
            if (!finite[c]) { s.min[c] = s.max[c] = 0.0; continue; }
            s.mean[c]   = sum[c] / double(finite[c]);
            s.stddev[c] = std::sqrt((std::max)(0.0, sum2[c] / double(finite[c]) - s.mean[c] * s.mean[c]));
        }
        s.alpha = !s.hasAlpha ? "none" : alphaOpaque ? "opaque" : alphaBinary ? "binary" : "translucent";
        s.monochrome = true;
        for (int c = 0; c < 4; ++c)
            if (s.nan[c] || s.inf[c] || s.min[c] != s.max[c]) s.monochrome = false;
        if (normal && normalTexels)
        {
            s.normalChecked     = true;
            s.normalOutsideDisc = double(outside) / double(normalTexels);
            s.normalLengthDev   = s.channels >= 3 ? lenDev / double(normalTexels) : 0.0;
            s.normalZNegative   = s.channels >= 3 ? double(zNeg) / double(normalTexels) : 0.0;
        }

        // Pass 2: the histograms.
        for (int c = 0; c < 4; ++c)
        {
            if (type == DirectX::FORMAT_TYPE_UNORM)      { s.histLo[c] = 0.0;  s.histHi[c] = 1.0; }
            else if (type == DirectX::FORMAT_TYPE_SNORM) { s.histLo[c] = -1.0; s.histHi[c] = 1.0; }
            else
            {
                s.histLo[c] = s.min[c];
                s.histHi[c] = s.max[c] > s.min[c] ? s.max[c] : s.min[c] + 1.0;
            }
        }
        for (const DirectX::ScratchImage& sc : conv)
        {
            const DirectX::Image& f = *sc.GetImage(0, 0, 0);
            for (std::size_t y = 0; y < f.height; ++y)
            {
                const float* row = reinterpret_cast<const float*>(f.pixels + y * f.rowPitch);
                for (std::size_t x = 0; x < f.width; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        const float v = row[x * 4 + c];
                        if (!std::isfinite(v)) continue;
                        const double t = (v - s.histLo[c]) / (s.histHi[c] - s.histLo[c]);
                        const int bin = std::clamp(static_cast<int>(t * 256.0), 0, 255);
                        ++s.histogram[c][bin];
                    }
            }
        }

        // The mip chain: each mip's mean against mip 0's.
        if (o.mipChain && !allSlices)
        {
            for (std::uint32_t m = 0; m < md.mipLevels; ++m)
            {
                const std::uint32_t z = is3D ? 0u : o.slice;
                const DirectX::Image* img = ImageOf(*d, m, z);
                DirectX::ScratchImage f;
                std::string why;
                if (!img || !ToFloat(*img, f, why)) break;
                s.mipMeans.push_back(MeanOf(*f.GetImage(0, 0, 0)));
            }
            for (const auto& mm : s.mipMeans)
                for (std::uint32_t c = 0; c < s.channels && c < 4; ++c)
                    s.mipMaxDrift = (std::max)(s.mipMaxDrift, std::fabs(mm[c] - s.mipMeans.front()[c]));
        }
        s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        s.ok = true;
        return s;
    }

    StoredTexel ReadStoredTexel(const std::string& pathIn, std::uint32_t mip, std::uint32_t slice,
                                std::uint32_t x, std::uint32_t y)
    {
        StoredTexel t;
        std::string path = Sys::RelativizeAssetPath(pathIn);
        for (char& c : path) if (c == '\\') c = '/';
        const std::shared_ptr<const Decoded> d = GetDecoded(path, t.error);
        if (!d) return t;
        if (mip >= d->md.mipLevels) { t.error = "no mip " + std::to_string(mip); return t; }
        const DirectX::Image* img = ImageOf(*d, mip, slice);
        if (!img) { t.error = "no slice " + std::to_string(slice); return t; }
        if (x >= img->width || y >= img->height)
        {
            t.error = "texel (" + std::to_string(x) + ", " + std::to_string(y) + ") outside " +
                      std::to_string(img->width) + "x" + std::to_string(img->height);
            return t;
        }
        const DXGI_FORMAT lin = DirectX::MakeLinear(img->format);
        DirectX::Image one{};
        std::uint32_t pick = 0;   // the texel's index inside the converted image
        if (DirectX::IsCompressed(lin))
        {
            // The 4x4 block that holds the texel, decoded alone.
            const std::size_t blockBytes = DirectX::BitsPerPixel(lin) * 16 / 8;
            t.block  = true;
            t.blockX = x / 4;
            t.blockY = y / 4;
            const std::uint8_t* p = img->pixels + std::size_t(t.blockY) * img->rowPitch + std::size_t(t.blockX) * blockBytes;
            t.raw = Hex(p, blockBytes);
            one.width = 4; one.height = 4; one.format = lin;
            one.rowPitch = blockBytes; one.slicePitch = blockBytes;
            one.pixels = const_cast<std::uint8_t*>(p);
            pick = (y % 4) * 4 + (x % 4);
        }
        else
        {
            const std::size_t bpp = (std::max<std::size_t>)(1, DirectX::BitsPerPixel(lin) / 8);
            const std::uint8_t* p = img->pixels + std::size_t(y) * img->rowPitch + std::size_t(x) * bpp;
            t.raw = Hex(p, bpp);
            one.width = 1; one.height = 1; one.format = lin;
            one.rowPitch = bpp; one.slicePitch = bpp;
            one.pixels = const_cast<std::uint8_t*>(p);
        }
        DirectX::ScratchImage f;
        if (!ToFloat(one, f, t.error)) return t;
        const DirectX::Image& fi = *f.GetImage(0, 0, 0);
        const float* texel = reinterpret_cast<const float*>(fi.pixels + (pick / fi.width) * fi.rowPitch) + (pick % fi.width) * 4;
        for (int c = 0; c < 4; ++c) t.value[c] = texel[c];
        t.ok = true;
        return t;
    }
}
