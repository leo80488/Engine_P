#ifndef NOMINMAX
#define NOMINMAX
#endif

// ModelCollision.cpp - see Resource/ModelCollision.h.

#include "Resource/ModelCollision.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"
#include "Resource/MeshLibIdentity.h"   // MeshKeyFromHex / MeshKeyToHex
#include "System/PathUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace Resource
{
namespace
{
    bool ParseFloat(const std::string& v, float& out)
    {
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        if (end == v.c_str()) return false;
        out = f;
        return true;
    }

    // "x_y_z[_w]" -> @p n floats.
    bool ParseVec(const std::string& v, float* out, int n)
    {
        const char* p = v.c_str();
        for (int i = 0; i < n; ++i)
        {
            char* end = nullptr;
            out[i] = std::strtof(p, &end);
            if (end == p) return false;
            p = end;
            if (i + 1 < n)
            {
                if (*p != '_') return false;
                ++p;
            }
        }
        return *p == '\0';
    }

    // The tokens of one record after its two-letter prefix: key -> value.
    std::unordered_map<std::string, std::string> Tokens(const std::string& line)
    {
        std::unordered_map<std::string, std::string> kv;
        std::istringstream ss(line.substr(3));
        std::string tok;
        while (ss >> tok)
        {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            kv.emplace(tok.substr(0, eq), tok.substr(eq + 1));
        }
        return kv;
    }

    std::string F(float v)
    {
        char b[32];
        // + 0.f turns a -0 (a basis change's negated zero) into 0: the same value, written plainly.
        std::snprintf(b, sizeof(b), "%.6g", v + 0.f);
        return b;
    }
    std::string V3(const DirectX::XMFLOAT3& v) { return F(v.x) + "_" + F(v.y) + "_" + F(v.z); }
    std::string V4(const DirectX::XMFLOAT4& v) { return F(v.x) + "_" + F(v.y) + "_" + F(v.z) + "_" + F(v.w); }
    bool IsIdentity(const DirectX::XMFLOAT4& q) { return q.x == 0.f && q.y == 0.f && q.z == 0.f && q.w == 1.f; }
    bool IsZero(const DirectX::XMFLOAT3& v) { return v.x == 0.f && v.y == 0.f && v.z == 0.f; }
}

bool MeshBodySetup::HasBody() const
{
    if (complexity == Complexity::None) return false;
    return std::any_of(prims.begin(), prims.end(), [](const CollisionPrim& p) { return p.InBody(); });
}

std::vector<uint64_t> MeshBodySetup::SectionTrimeshes(const std::vector<uint64_t>& allSections) const
{
    if (complexity == Complexity::None) return {};
    if (complexity == Complexity::Complex) return allSections;
    std::vector<uint64_t> out;
    for (const CollisionPrim& p : prims)
        if (!p.InBody() && p.sectionKey != 0 &&
            std::find(out.begin(), out.end(), p.sectionKey) == out.end())
            out.push_back(p.sectionKey);
    return out;
}

void ParseModelCollision(const std::string& text, ModelCollision& out, std::vector<std::string>* warnings)
{
    out.meshes.clear();
    auto warn = [&](size_t lineNo, const std::string& why)
    {
        if (warnings) warnings->push_back("line " + std::to_string(lineNo) + ": " + why);
    };
    size_t lineNo = 0;
    size_t b = 0;
    while (b < text.size())
    {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(b, e - b);
        b = e + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool bo = line.rfind("BO ", 0) == 0;
        const bool pr = line.rfind("PR ", 0) == 0;
        if (!bo && !pr) continue;

        const auto kv = Tokens(line);
        uint64_t key = 0;
        const auto mk = kv.find("mesh");
        if (mk == kv.end() || !MeshKeyFromHex(mk->second, key) || key == 0)
        {
            warn(lineNo, "no mesh=<MeshKey>");
            continue;
        }
        MeshBodySetup& setup = out.meshes[key];
        setup.meshKey = key;

        if (bo)
        {
            if (const auto it = kv.find("complexity"); it != kv.end())
            {
                if      (it->second == "simple")  setup.complexity = MeshBodySetup::Complexity::Simple;
                else if (it->second == "complex") setup.complexity = MeshBodySetup::Complexity::Complex;
                else if (it->second == "none")    setup.complexity = MeshBodySetup::Complexity::None;
                else warn(lineNo, "unknown complexity '" + it->second + "' (simple is used)");
            }
            if (const auto it = kv.find("layer"); it != kv.end())
                setup.layer = static_cast<uint8_t>(std::min(std::max(std::atoi(it->second.c_str()), 0), 255));
            if (const auto it = kv.find("trig"); it != kv.end()) setup.trigger = it->second != "0";
            if (const auto it = kv.find("friction"); it != kv.end()) ParseFloat(it->second, setup.friction);
            if (const auto it = kv.find("restitution"); it != kv.end()) ParseFloat(it->second, setup.restitution);
            if (const auto it = kv.find("orphan"); it != kv.end()) setup.orphan = it->second != "0";
            if (const auto it = kv.find("src"); it != kv.end()) setup.source = AssetRefs::PercentDecode(it->second);
            continue;
        }

        CollisionPrim p;
        const auto ty = kv.find("type");
        const std::string type = ty == kv.end() ? std::string() : ty->second;
        if      (type == "box")     p.type = CollisionPrim::Type::Box;
        else if (type == "sphere")  p.type = CollisionPrim::Type::Sphere;
        else if (type == "capsule") p.type = CollisionPrim::Type::Capsule;
        else if (type == "hull")    p.type = CollisionPrim::Type::Hull;
        else if (type == "mesh")    p.type = CollisionPrim::Type::Mesh;
        else { warn(lineNo, "unknown PR type '" + type + "'"); continue; }

        bool ok = true;
        if (const auto it = kv.find("c"); it != kv.end()) ok &= ParseVec(it->second, &p.center.x, 3);
        if (const auto it = kv.find("q"); it != kv.end()) ok &= ParseVec(it->second, &p.rotation.x, 4);
        if (const auto it = kv.find("he"); it != kv.end()) ok &= ParseVec(it->second, &p.halfExtents.x, 3);
        if (const auto it = kv.find("r"); it != kv.end()) ok &= ParseFloat(it->second, p.radius);
        if (const auto it = kv.find("hh"); it != kv.end()) ok &= ParseFloat(it->second, p.halfHeight);
        if (const auto it = kv.find("name"); it != kv.end()) p.name = AssetRefs::PercentDecode(it->second);
        if (const auto it = kv.find("lib"); it != kv.end()) p.lib = AssetRefs::PercentDecode(it->second);
        if (const auto it = kv.find("sec"); it != kv.end()) ok &= MeshKeyFromHex(it->second, p.sectionKey);
        if (!ok) { warn(lineNo, "a malformed value"); continue; }
        if ((p.type == CollisionPrim::Type::Hull || p.type == CollisionPrim::Type::Mesh) && p.sectionKey == 0)
        {
            warn(lineNo, "a " + type + " prim needs sec=<SectionKey>");
            continue;
        }
        if (p.type == CollisionPrim::Type::Hull && p.lib.empty())
        {
            warn(lineNo, "a hull prim needs lib=<collision library>");
            continue;
        }
        setup.prims.push_back(std::move(p));
    }
}

std::shared_ptr<const ModelCollision> ModelCollisionAt(const std::string& iscnPath)
{
    struct Cached { std::shared_ptr<const ModelCollision> data; bool loose = false; uint64_t sig0 = 0, sig1 = 0; };
    static std::mutex                              s_mutex;
    static std::unordered_map<std::string, Cached> s_cache;
    static const auto                              s_empty = std::make_shared<const ModelCollision>();
    if (iscnPath.empty()) return s_empty;

    namespace fs = std::filesystem;
    const std::string k = MeshLibCanonicalPath(iscnPath);
    std::error_code ec;
    const fs::path p = Sys::U8Path(iscnPath);
    const bool loose = fs::is_regular_file(p, ec);
    uint64_t sig0 = 0, sig1 = 0;
    if (loose)
    {
        sig0 = fs::file_size(p, ec);
        sig1 = static_cast<uint64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (const auto it = s_cache.find(k); it != s_cache.end())
            if (!it->second.loose || (it->second.sig0 == sig0 && it->second.sig1 == sig1))
                return it->second.data;
    }

    auto parsed = std::make_shared<ModelCollision>();
    std::vector<uint8_t> blob;
    if (AssetFS::Get().ReadFile(iscnPath, blob) && blob.size() > sizeof(AssetHeader) &&
        ValidateHeader(blob.data(), blob.size(), MAGIC_SCENE))
    {
        const auto*  hdr  = reinterpret_cast<const AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        ParseModelCollision(std::string(payl, n), *parsed);
    }
    Cached fresh{ parsed, loose, sig0, sig1 };
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[k] = fresh;
    return parsed;
}

std::string ResolveModelRelative(const std::string& iscnPath, const std::string& lib)
{
    if (lib.empty()) return {};
    return Sys::RelativizeAssetPath(
        Sys::PathToUtf8((Sys::U8Path(iscnPath).parent_path() / Sys::U8Path(lib)).lexically_normal()));
}

std::string FormatBodySetup(const MeshBodySetup& s)
{
    std::string out = "BO mesh=" + MeshKeyToHex(s.meshKey) + " complexity=" +
                      (s.complexity == MeshBodySetup::Complexity::Complex ? "complex"
                       : s.complexity == MeshBodySetup::Complexity::None  ? "none" : "simple");
    if (s.layer != 0) out += " layer=" + std::to_string(s.layer);
    if (s.trigger) out += " trig=1";
    if (s.friction >= 0.f) out += " friction=" + F(s.friction);
    if (s.restitution >= 0.f) out += " restitution=" + F(s.restitution);
    if (!s.source.empty()) out += " src=" + AssetRefs::PercentEncodeStrict(s.source);
    if (s.orphan) out += " orphan=1";
    out += "\n";
    for (const CollisionPrim& p : s.prims)
    {
        out += "PR mesh=" + MeshKeyToHex(s.meshKey) + " type=";
        switch (p.type)
        {
        case CollisionPrim::Type::Box:
            out += "box c=" + V3(p.center) + " he=" + V3(p.halfExtents);
            if (!IsIdentity(p.rotation)) out += " q=" + V4(p.rotation);
            break;
        case CollisionPrim::Type::Sphere:
            out += "sphere c=" + V3(p.center) + " r=" + F(p.radius);
            break;
        case CollisionPrim::Type::Capsule:
            out += "capsule c=" + V3(p.center) + " r=" + F(p.radius) + " hh=" + F(p.halfHeight);
            if (!IsIdentity(p.rotation)) out += " q=" + V4(p.rotation);
            break;
        case CollisionPrim::Type::Hull:
            out += "hull lib=" + AssetRefs::PercentEncode(p.lib) + " sec=" + MeshKeyToHex(p.sectionKey);
            if (!IsZero(p.center)) out += " c=" + V3(p.center);
            if (!IsIdentity(p.rotation)) out += " q=" + V4(p.rotation);
            break;
        case CollisionPrim::Type::Mesh:
            out += "mesh";
            if (!p.lib.empty()) out += " lib=" + AssetRefs::PercentEncode(p.lib);
            out += " sec=" + MeshKeyToHex(p.sectionKey);
            break;
        }
        if (!p.name.empty()) out += " name=" + AssetRefs::PercentEncodeStrict(p.name);
        out += "\n";
    }
    return out;
}
}   // namespace Resource
