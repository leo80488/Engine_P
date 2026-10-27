#include "Harness/AttachmentDecode.h"

#ifdef WITH_HARNESS

#include <DirectXMath.h>
#include <DirectXPackedVector.h>   // XMConvertHalfToFloat, XMFLOAT3PK (R11G11B10_FLOAT)
#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Harness::Attachments
{
namespace
{
    using Json = nlohmann::json;

    // How one texel's bytes become up to four floats.
    enum class Codec { F32x1, F32x4, F16x4, F16x2, F16x1, UN16x4, UN16x1, UN8x4, UN8x4_BGRA, UN8x1,
                       R11G11B10, R10G10B10A2,
                       // Integers: the value as a float - exact below 2^24.
                       U8x1, U8x2, U16x1, U16x2, U32x1, U32x2 };
    struct Layout
    {
        Codec         codec;
        int           channels;
        std::uint32_t bytes;       // one texel
        const char*   encoding;    // how a channel is stored
        const char*   format;      // the format's name
    };

    // What each attachment is: its semantic (Renderer/DebugTargets.h - it
    // picks the preview), its channels' names, what they mean, how to decode
    // them beyond the format, and its preview's fixed mapping.
    struct Semantics
    {
        std::string              semantic;
        std::vector<std::string> channels;
        std::string              meaning;
        std::string              decode;
        std::string              preview;
    };

    const char* kDepthDecode =
        "perspective: viewZ = near * far / (near + depth * (far - near)); orthographic: viewZ = far - "
        "depth * (far - near); camera.nearZ / farZ below";
    const char* kDepthPreview =
        "grey = 1 - log(viewZ / near) / log(far / near), clamped: white at the near plane, black at "
        "the far plane and the sky; orthographic: grey = depth";
    const char* kVelocityPreview =
        "rgb = 0.5 + clamp(8 * (x, y, 0), -0.5, 0.5): mid grey = no motion, saturated at 1/16 NDC";
    const char* kRadiancePreview = "rgb = sRGB-encoded c / (1 + c) per channel (Reinhard, exposure 1)";

    struct Acc
    {
        double        sum = 0.0;
        double        min = std::numeric_limits<double>::infinity();
        double        max = -std::numeric_limits<double>::infinity();
        std::uint64_t finite = 0, nan = 0, posInf = 0, negInf = 0, zero = 0;
        void Add(float v)
        {
            if (std::isnan(v)) { ++nan; return; }
            if (std::isinf(v)) { (v > 0 ? posInf : negInf)++; return; }
            ++finite;
            sum += v;
            if (v < min) min = v;
            if (v > max) max = v;
            if (v == 0.f) ++zero;
        }
        Json ToJson() const
        {
            Json j{ { "finite", finite }, { "nan", nan }, { "posInf", posInf }, { "negInf", negInf }, { "zero", zero } };
            if (finite)
            {
                j["min"]  = min;
                j["max"]  = max;
                j["mean"] = sum / static_cast<double>(finite);
            }
            else
                j["min"] = j["max"] = j["mean"] = nullptr;
            return j;
        }
    };

    bool LayoutOf(std::uint32_t resourceFormat, std::uint32_t plane, Layout& out)
    {
        switch (static_cast<DXGI_FORMAT>(resourceFormat))
        {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            // Plane 1 is the stencil byte: one byte a texel.
            if (plane == 1) { out = { Codec::U8x1, 1, 1, "uint8", "D32_FLOAT_S8X24_UINT plane 1 (stencil)" }; return true; }
            if (plane != 0) return false;
            out = { Codec::F32x1, 1, 4, "float32", "D32_FLOAT_S8X24_UINT plane 0 (D32_FLOAT)" };
            return true;
        case DXGI_FORMAT_D32_FLOAT:          out = { Codec::F32x1, 1, 4, "float32", "D32_FLOAT" };          return true;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:       out = { Codec::F32x1, 1, 4, "float32", "R32_FLOAT" };          return true;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: out = { Codec::F32x4, 4, 16, "float32", "R32G32B32A32_FLOAT" }; return true;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: out = { Codec::F16x4, 4, 8, "float16", "R16G16B16A16_FLOAT" };  return true;
        case DXGI_FORMAT_R16G16_FLOAT:       out = { Codec::F16x2, 2, 4, "float16", "R16G16_FLOAT" };        return true;
        case DXGI_FORMAT_R16_FLOAT:          out = { Codec::F16x1, 1, 2, "float16", "R16_FLOAT" };           return true;
        case DXGI_FORMAT_R16G16B16A16_UNORM: out = { Codec::UN16x4, 4, 8, "unorm16", "R16G16B16A16_UNORM" }; return true;
        case DXGI_FORMAT_R16_UNORM:          out = { Codec::UN16x1, 1, 2, "unorm16", "R16_UNORM" };          return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:     out = { Codec::UN8x4, 4, 4, "unorm8", "R8G8B8A8_UNORM" };       return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: out = { Codec::UN8x4, 4, 4, "unorm8 (sRGB-encoded)", "R8G8B8A8_UNORM_SRGB" }; return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:     out = { Codec::UN8x4_BGRA, 4, 4, "unorm8, stored BGRA", "B8G8R8A8_UNORM" }; return true;
        case DXGI_FORMAT_R8_UNORM:           out = { Codec::UN8x1, 1, 1, "unorm8", "R8_UNORM" };            return true;
        case DXGI_FORMAT_R11G11B10_FLOAT:    out = { Codec::R11G11B10, 3, 4, "float11/float11/float10 packed in 32 bits", "R11G11B10_FLOAT" }; return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:  out = { Codec::R10G10B10A2, 4, 4, "unorm10/10/10/2 packed in 32 bits", "R10G10B10A2_UNORM" }; return true;
        case DXGI_FORMAT_R8_UINT:            out = { Codec::U8x1, 1, 1, "uint8", "R8_UINT" };               return true;
        case DXGI_FORMAT_R8G8_UINT:          out = { Codec::U8x2, 2, 2, "uint8", "R8G8_UINT" };             return true;
        case DXGI_FORMAT_R16_UINT:           out = { Codec::U16x1, 1, 2, "uint16", "R16_UINT" };            return true;
        case DXGI_FORMAT_R16G16_UINT:        out = { Codec::U16x2, 2, 4, "uint16", "R16G16_UINT" };         return true;
        case DXGI_FORMAT_R32_UINT:           out = { Codec::U32x1, 1, 4, "uint32", "R32_UINT" };            return true;
        case DXGI_FORMAT_R32G32_UINT:        out = { Codec::U32x2, 2, 8, "uint32", "R32G32_UINT" };         return true;
        default: return false;
        }
    }

    float Half(const std::uint8_t* p)
    {
        std::uint16_t h;
        std::memcpy(&h, p, 2);
        return DirectX::PackedVector::XMConvertHalfToFloat(h);
    }
    std::uint16_t U16(const std::uint8_t* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
    std::uint32_t U32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
    float         F32(const std::uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

    void Texel(const Layout& l, const std::uint8_t* p, float v[4])
    {
        switch (l.codec)
        {
        case Codec::F32x1:  v[0] = F32(p); break;
        case Codec::F32x4:  for (int c = 0; c < 4; ++c) v[c] = F32(p + 4 * c); break;
        case Codec::F16x4:  for (int c = 0; c < 4; ++c) v[c] = Half(p + 2 * c); break;
        case Codec::F16x2:  v[0] = Half(p); v[1] = Half(p + 2); break;
        case Codec::F16x1:  v[0] = Half(p); break;
        case Codec::UN16x4: for (int c = 0; c < 4; ++c) v[c] = U16(p + 2 * c) / 65535.f; break;
        case Codec::UN16x1: v[0] = U16(p) / 65535.f; break;
        case Codec::UN8x4:  for (int c = 0; c < 4; ++c) v[c] = p[c] / 255.f; break;
        case Codec::UN8x4_BGRA: v[0] = p[2] / 255.f; v[1] = p[1] / 255.f; v[2] = p[0] / 255.f; v[3] = p[3] / 255.f; break;
        case Codec::UN8x1:  v[0] = p[0] / 255.f; break;
        case Codec::U8x1:   v[0] = static_cast<float>(p[0]); break;
        case Codec::U8x2:   v[0] = static_cast<float>(p[0]); v[1] = static_cast<float>(p[1]); break;
        case Codec::U16x1:  v[0] = static_cast<float>(U16(p)); break;
        case Codec::U16x2:  v[0] = static_cast<float>(U16(p)); v[1] = static_cast<float>(U16(p + 2)); break;
        case Codec::U32x1:  v[0] = static_cast<float>(U32(p)); break;
        case Codec::U32x2:  v[0] = static_cast<float>(U32(p)); v[1] = static_cast<float>(U32(p + 4)); break;
        case Codec::R11G11B10:
        {
            DirectX::PackedVector::XMFLOAT3PK pk;
            pk.v = U32(p);
            DirectX::XMFLOAT3 f;
            DirectX::XMStoreFloat3(&f, DirectX::PackedVector::XMLoadFloat3PK(&pk));
            v[0] = f.x; v[1] = f.y; v[2] = f.z;
            break;
        }
        case Codec::R10G10B10A2:
        {
            const std::uint32_t u = U32(p);
            v[0] = (u & 0x3FFu) / 1023.f;
            v[1] = ((u >> 10) & 0x3FFu) / 1023.f;
            v[2] = ((u >> 20) & 0x3FFu) / 1023.f;
            v[3] = ((u >> 30) & 0x3u) / 3.f;
            break;
        }
        }
    }

    std::vector<std::string> SplitChannels(const std::string& list)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= list.size() && !list.empty())
        {
            const size_t comma = list.find(',', start);
            out.push_back(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    Semantics SemanticsOf(const std::string& name, const DebugTargets::Info* info)
    {
        if (name == "albedo")
            return { "color", { "r", "g", "b" }, "baseColor, linear, no emissive (GBuffer0)",
                     "the stored value", "sRGB-encoded clamp(rgb, 0, 1)" };
        if (name == "normal")
            return { "normal", { "x", "y", "z", "a" },
                     "world-space normal packed to [0,1]; .a an exact 16-bit integer / 65535 (GBuffer1)",
                     "n = 2 * xyz - 1 (unit length where geometry was drawn); a16 = round(a * 65535): "
                     "matIdx = a16 & 0x7FFF, bit 15 = excluded from XeGTAO",
                     "rgb = the stored xyz (0.5 grey = a zero component), no gamma" };
        if (name == "surface")
            return { "surface", { "roughness", "metalness", "ao", "reflectance" },
                     "material surface terms after the material's min/max remap (GBuffer2)", "the stored value",
                     "rgb = roughness, metalness, ao as stored, no gamma" };
        if (name == "depth")
            return { "depth", { "depth" },
                     "reverse-Z device depth: 1 at the near plane, 0 at the far plane - where nothing was drawn "
                     "(the sky) it is exactly 0",
                     kDepthDecode, kDepthPreview };
        if (name == "velocity")
            return { "velocity", { "x", "y" }, "NDC motion vector of the texel (GBuffer3)", "the stored value",
                     kVelocityPreview };
        if (name == "extra")
            return { "data", { "r", "g", "b", "a" },
                     "shading-model scratch - SSS thickness, NPR terms, per model (GBuffer4; the graph calls it "
                     "GBuffer4_Emissive, emissive itself seeds the HDR scene color)",
                     "per shading model: see shaders/GBuffer.ps.hlsl and the model's shader",
                     "rgb = clamp(rgb, 0, 1) as stored, no gamma" };
        if (name == "hdr" || !info)
            return { "radiance", { "r", "g", "b", "a" },
                     "linear scene radiance: lit scene + sky + transparent + in-place effects, before AA / upscale, "
                     "exposure and tonemap (the device's HDR scene target)",
                     "the stored value", kRadiancePreview };

        // Any other registry target: its own words; the preview by what it is.
        Semantics s;
        s.semantic = info->semantic.empty() ? std::string("data") : info->semantic;
        s.channels = SplitChannels(info->channels);
        s.meaning  = info->meaning;
        s.decode   = s.semantic == "depth"  ? kDepthDecode
                   : s.semantic == "normal" ? "n = 2 * xyz - 1" : "the stored value";
        if (s.semantic == "depth")         s.preview = kDepthPreview;
        else if (s.semantic == "velocity") s.preview = kVelocityPreview;
        else if (s.semantic == "radiance") s.preview = kRadiancePreview;
        else if (s.semantic == "color")    s.preview = "sRGB-encoded clamp(rgb, 0, 1)";
        else s.preview = "rgb = clamp(rgb, 0, 1) as stored, no gamma; one channel = grey";
        return s;
    }

    float Clamp01(float x) { return x > 0.f ? (x < 1.f ? x : 1.f) : 0.f; }   // NaN -> 0
    std::uint8_t Byte(float x) { return static_cast<std::uint8_t>(Clamp01(x) * 255.f + 0.5f); }
    float Srgb(float x)
    {
        x = Clamp01(x);
        return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
    }
}

std::vector<std::string> Names()
{
    std::vector<std::string> names = DebugTargets::PublishedNames();
    if (names.empty())
        names = { "albedo", "normal", "surface", "depth", "velocity", "extra", "hdr" };
    return names;
}

bool IsKnown(const std::string& name)
{
    // "Name[N]" is slice N of a texture array (Renderer::GetAttachmentSource
    // picks it; the capture refuses a slice past the end).
    std::string base = name;
    if (const size_t open = name.rfind('['); open != std::string::npos && open > 0 && name.back() == ']')
    {
        const std::string digits = name.substr(open + 1, name.size() - open - 2);
        if (!digits.empty() && digits.size() <= 3 &&
            digits.find_first_not_of("0123456789") == std::string::npos)
            base = name.substr(0, open);
    }
    if (DebugTargets::IsPublishedName(base)) return true;
    for (const std::string& n : Names())
        if (n == base) return true;
    return false;
}

bool Decode(const std::string& name, const IGraphicsDevice::RawTexture& raw, const Camera& camera, Decoded& out,
            const DebugTargets::Info* info)
{
    out = Decoded{};
    Layout layout{};
    if (!LayoutOf(raw.resourceFormat, raw.plane, layout))
    {
        out.error = "format " + std::to_string(raw.resourceFormat) + " (DXGI_FORMAT) plane " +
                    std::to_string(raw.plane) + " is not one this decoder reads";
        return false;
    }
    const std::uint64_t texels = static_cast<std::uint64_t>(raw.width) * raw.height;
    if (raw.bytesPerTexel != layout.bytes || raw.bytes.size() != texels * layout.bytes)
    {
        out.error = "the bytes do not fit the size: " + std::to_string(raw.bytes.size()) + " bytes for " +
                    std::to_string(raw.width) + "x" + std::to_string(raw.height) + " texels of " +
                    std::to_string(layout.bytes) + " (" + layout.format + ")";
        return false;
    }

    Semantics sem = SemanticsOf(name, info);
    const int n = layout.channels;
    // An integer: its preview is value / the type's largest, as grey (the stored values are in the
    // .bin; integers() reads them).
    float intPreview = 0.f;
    switch (layout.codec)
    {
    case Codec::U8x1: case Codec::U8x2:   intPreview = 1.f / 255.f;   sem.preview = "the integer / 255 as grey";        break;
    case Codec::U16x1: case Codec::U16x2: intPreview = 1.f / 65535.f; sem.preview = "the integer / 65535 as grey";      break;
    case Codec::U32x1: case Codec::U32x2: intPreview = 1.f / 4294967295.f;
                                          sem.preview = "the integer / 4294967295 as grey"; break;
    default: break;
    }
    std::vector<Acc> acc(static_cast<size_t>(n));
    out.previewRgba.assign(static_cast<size_t>(texels) * 4u, 0);

    // The depth preview's constants: the camera's own planes.
    const bool   isDepth = sem.semantic == "depth";
    const double nearZ   = std::max(1e-6, static_cast<double>(camera.nearZ));
    const double farZ    = std::max(nearZ * 1.0001, static_cast<double>(camera.farZ));
    const double logSpan = std::log(farZ / nearZ);
    std::uint64_t nonFinitePreview = 0;

    for (std::uint64_t i = 0; i < texels; ++i)
    {
        float v[4] = { 0.f, 0.f, 0.f, 0.f };
        Texel(layout, raw.bytes.data() + i * layout.bytes, v);
        for (int c = 0; c < n; ++c) acc[static_cast<size_t>(c)].Add(v[c]);

        std::uint8_t* px = out.previewRgba.data() + i * 4u;
        px[3] = 255;
        const int shown = std::min(n, 3);
        bool bad = false;
        for (int c = 0; c < shown; ++c) bad = bad || !std::isfinite(v[c]);
        if (bad)
        {
            // NaN / Inf in magenta: the one thing a preview must never hide.
            px[0] = 255; px[1] = 0; px[2] = 255;
            ++nonFinitePreview;
            continue;
        }
        if (intPreview != 0.f)
        {
            for (int c = 0; c < 3; ++c) px[c] = Byte((c < n ? v[c] : v[0]) * intPreview);
        }
        else if (isDepth)
        {
            float g = 0.f;
            if (camera.orthographic)
                g = Clamp01(v[0]);
            else if (v[0] > 0.f)
            {
                const double viewZ = nearZ * farZ / (nearZ + static_cast<double>(v[0]) * (farZ - nearZ));
                g = Clamp01(static_cast<float>(1.0 - std::log(std::max(viewZ, nearZ) / nearZ) / logSpan));
            }
            px[0] = px[1] = px[2] = Byte(g);
        }
        else if (sem.semantic == "color")
        {
            for (int c = 0; c < 3; ++c) px[c] = Byte(Srgb(v[c]));
        }
        else if (sem.semantic == "radiance")
        {
            for (int c = 0; c < 3; ++c)
            {
                const float x = v[c] > 0.f ? v[c] : 0.f;
                px[c] = Byte(Srgb(x / (1.f + x)));
            }
        }
        else if (sem.semantic == "velocity")
        {
            px[0] = Byte(0.5f + std::clamp(8.f * v[0], -0.5f, 0.5f));
            px[1] = Byte(0.5f + std::clamp(8.f * v[1], -0.5f, 0.5f));
            px[2] = 128;
        }
        else if (n == 1)   // a mask / one-channel data: grey
        {
            px[0] = px[1] = px[2] = Byte(v[0]);
        }
        else   // normal, surface, extra, data: the stored values, no gamma
        {
            for (int c = 0; c < 3; ++c) px[c] = c < n ? Byte(v[c]) : 0;
        }
    }

    Json channels = Json::array();
    for (int c = 0; c < n; ++c)
    {
        const bool named = static_cast<size_t>(c) < sem.channels.size() && !sem.channels[static_cast<size_t>(c)].empty();
        channels.push_back({ { "name", named ? sem.channels[static_cast<size_t>(c)] : "c" + std::to_string(c) },
                             { "stats", acc[static_cast<size_t>(c)].ToJson() } });
    }
    out.manifest = {
        { "attachment", name },
        { "semantic", sem.semantic },
        { "view", "main" },
        { "width", raw.width }, { "height", raw.height },
        { "format", layout.format },
        { "dxgiFormat", raw.resourceFormat }, { "planeFormat", raw.planeFormat }, { "plane", raw.plane },
        { "arraySlice", raw.arraySlice },
        { "bytesPerTexel", layout.bytes },
        { "layout", "row-major from the top-left texel, rows packed tight (width * bytesPerTexel bytes), "
                    "little-endian" },
        { "encoding", layout.encoding },
        { "channels", channels },
        { "meaning", sem.meaning },
        { "decode", sem.decode },
        { "preview", { { "mapping", sem.preview }, { "nonFinite", "magenta (255, 0, 255)" },
                       { "nonFiniteTexels", nonFinitePreview } } },
        { "camera", { { "nearZ", camera.nearZ }, { "farZ", camera.farZ }, { "fovY", camera.fovY },
                      { "orthographic", camera.orthographic },
                      { "position", { camera.position[0], camera.position[1], camera.position[2] } },
                      { "forward", { camera.forward[0], camera.forward[1], camera.forward[2] } },
                      { "renderWidth", camera.renderWidth }, { "renderHeight", camera.renderHeight } } } };
    if (info)
    {
        out.manifest["label"] = info->label;
        out.manifest["group"] = info->group;
        if (!info->graphName.empty()) out.manifest["graphName"] = info->graphName;
    }
    return true;
}
}

#endif  // WITH_HARNESS
