#include "PostProcess/PostProcessProfileSerializer.h"
#include "PostProcess/PostProcessProfile.h"

#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace
{
    // Tolerant numeric parsers - a corrupt field yields the caller's default.
    float ParseF(const std::string& s, float def = 0.0f)
    {
        try { return std::stof(s); } catch (...) { return def; }
    }
    uint32_t ParseU(const std::string& s)
    {
        try { return static_cast<uint32_t>(std::stoul(s)); } catch (...) { return 0u; }
    }
    bool ParseB(const std::string& s)
    {
        return ParseF(s, 0.0f) != 0.0f;
    }
    XMFLOAT3 ParseF3(const std::string& s)
    {
        XMFLOAT3 v{ 0.0f, 0.0f, 0.0f };
        sscanf_s(s.c_str(), "%f_%f_%f", &v.x, &v.y, &v.z);
        return v;
    }
}

namespace PostProcess
{

bool SaveProfile(const PostProcessProfile& p, const std::string& path)
{
    std::ostringstream ss;
    ss << "# DX12 Engine Post-Process Profile (.ppprofile)\n";

    char buf[256];
    (void)buf;
#define PP_BOOL(g, m, l, d) \
    if (p.g.m.overrideState) ss << #g "_" #m "=" << (p.g.m.value ? 1 : 0) << "\n";
#define PP_FLOAT(g, m, l, d, a, b) \
    if (p.g.m.overrideState) { snprintf(buf, sizeof(buf), "%.6f", p.g.m.value); ss << #g "_" #m "=" << buf << "\n"; }
#define PP_FLOAT3(g, m, l, dx, dy, dz) \
    if (p.g.m.overrideState) { snprintf(buf, sizeof(buf), "%.6f_%.6f_%.6f", p.g.m.value.x, p.g.m.value.y, p.g.m.value.z); ss << #g "_" #m "=" << buf << "\n"; }
#define PP_COLOR(g, m, l, dr, dg, db) \
    if (p.g.m.overrideState) { snprintf(buf, sizeof(buf), "%.6f_%.6f_%.6f", p.g.m.value.x, p.g.m.value.y, p.g.m.value.z); ss << #g "_" #m "=" << buf << "\n"; }
#define PP_UINT(g, m, l, d, a, b) \
    if (p.g.m.overrideState) ss << #g "_" #m "=" << p.g.m.value << "\n";
#include "PostProcess/PostProcessProperties.inl"

    const std::string text = ss.str();

    Resource::AssetHeader hdr{};
    hdr.magic        = Resource::MAGIC_PPPROFILE;
    hdr.version      = Resource::CurrentVersion(Resource::MAGIC_PPPROFILE);
    hdr.resourceType = static_cast<uint16_t>(Resource::ResourceType::PostProcessProfile);
    hdr.metadataSize = sizeof(Resource::PostProcessProfileMetadata);
    hdr.dataSize     = static_cast<uint32_t>(text.size() + 1); // include null terminator

    Resource::PostProcessProfileMetadata meta{};
    meta.textLength = static_cast<uint32_t>(text.size());

    std::ofstream f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f) { LOG_ERROR("PostProcessProfile: cannot write '%s'", path.c_str()); return false; }
    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), static_cast<std::streamsize>(text.size() + 1));
    LOG_SUCCESS("PostProcessProfile: saved '%s' (%zu bytes text)", path.c_str(), text.size());
    return true;
}

bool LoadProfile(const std::string& path, PostProcessProfile& out)
{
    std::vector<uint8_t> bytes;
    if (!Resource::AssetFS::Get().ReadFile(path, bytes))
    {
        LOG_WARNING("PostProcessProfile: cannot open '%s'", path.c_str());
        return false;
    }
    if (!Resource::ValidateHeader(bytes.data(), bytes.size(), Resource::MAGIC_PPPROFILE))
    {
        LOG_WARNING("PostProcessProfile: bad header '%s'", path.c_str());
        return false;
    }

    const char* payload = reinterpret_cast<const char*>(Resource::GetPayload(bytes.data()));
    const uint32_t len   = Resource::GetMetadata<Resource::PostProcessProfileMetadata>(bytes.data())->textLength;

    std::unordered_map<std::string, std::string> kv;
    {
        std::istringstream fs(std::string(payload, len));
        std::string line;
        while (std::getline(fs, line))
        {
            if (line.empty() || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            // Strip a trailing CR (a file saved with CRLF line endings).
            std::string val = line.substr(eq + 1);
            if (!val.empty() && val.back() == '\r') val.pop_back();
            kv[line.substr(0, eq)] = val;
        }
    }

#define PP_BOOL(g, m, l, d) \
    { auto it = kv.find(#g "_" #m); if (it != kv.end()) { out.g.m.value = ParseB(it->second); out.g.m.overrideState = true; } }
#define PP_FLOAT(g, m, l, d, a, b) \
    { auto it = kv.find(#g "_" #m); if (it != kv.end()) { out.g.m.value = ParseF(it->second, (d)); out.g.m.overrideState = true; } }
#define PP_FLOAT3(g, m, l, dx, dy, dz) \
    { auto it = kv.find(#g "_" #m); if (it != kv.end()) { out.g.m.value = ParseF3(it->second); out.g.m.overrideState = true; } }
#define PP_COLOR(g, m, l, dr, dg, db) \
    { auto it = kv.find(#g "_" #m); if (it != kv.end()) { out.g.m.value = ParseF3(it->second); out.g.m.overrideState = true; } }
#define PP_UINT(g, m, l, d, a, b) \
    { auto it = kv.find(#g "_" #m); if (it != kv.end()) { out.g.m.value = ParseU(it->second); out.g.m.overrideState = true; } }
#include "PostProcess/PostProcessProperties.inl"

    return true;
}

} // namespace PostProcess
