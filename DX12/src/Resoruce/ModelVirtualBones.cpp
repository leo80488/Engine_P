// Resource/ModelVirtualBones.cpp - the VB records. See the header for the grammar and the rules.

#include "Resource/ModelVirtualBones.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"
#include "Resource/MeshLibIdentity.h"   // MeshLibCanonicalPath
#include "Resource/SkeletonAsset.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>

using namespace DirectX;

namespace Resource
{
namespace
{
    std::vector<std::pair<std::string, std::string>> Fields(const std::string& line)
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::istringstream ss(line.substr(3));
        std::string tok;
        while (ss >> tok)
        {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) out.emplace_back(tok, std::string());
            else                         out.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
        }
        return out;
    }

    std::string Utf8Prefix(const std::string& s, std::size_t maxBytes)
    {
        if (s.size() <= maxBytes) return s;
        std::size_t n = maxBytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
        return s.substr(0, n);
    }

    uint32_t Fnv32(const char* s)
    {
        uint32_t h = 2166136261u;
        for (const char* p = s; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
        return h;
    }

    std::string NameOf(const SkeletonAsset& s, uint32_t b)
    {
        return std::string(s.boneNames[b], strnlen(s.boneNames[b], sizeof(s.boneNames[b])));
    }
}

const ModelVirtualBone* ModelVirtualBones::Find(const std::string& name) const
{
    for (const ModelVirtualBone& v : bones)
        if (v.name == name) return &v;
    return nullptr;
}

std::string VirtualBoneNameProblem(const std::string& name)
{
    if (name.empty()) return "a virtual bone needs a name";
    if (name.size() > 63) return "a virtual bone name is at most 63 bytes (a bone name's 64)";
    return {};
}

std::string DefaultVirtualBoneName(const std::string& from, const std::string& to)
{
    return Utf8Prefix("VB " + from + "_" + to, 63);
}

void ParseModelVirtualBones(const std::string& text, ModelVirtualBones& out, std::vector<std::string>* warnings)
{
    out.bones.clear();
    std::istringstream ss(text);
    std::string        line;
    unsigned           n = 0;
    while (std::getline(ss, line))
    {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!(line.size() > 3 && line[0] == 'V' && line[1] == 'B' && line[2] == ' ')) continue;
        ModelVirtualBone v;
        for (const auto& [k, val] : Fields(line))
        {
            if (k == "name")        v.name = AssetRefs::PercentDecode(val);
            else if (k == "from")   v.from = AssetRefs::PercentDecode(val);
            else if (k == "to")     v.to   = AssetRefs::PercentDecode(val);
            else if (k == "orphan") v.orphan = (val == "1");
        }
        if (!VirtualBoneNameProblem(v.name).empty() || v.from.empty() || v.to.empty())
        {
            if (warnings) warnings->push_back("line " + std::to_string(n) + ": malformed VB record - skipped");
            continue;
        }
        const auto it = std::find_if(out.bones.begin(), out.bones.end(),
                                     [&](const ModelVirtualBone& o) { return o.name == v.name; });
        if (it != out.bones.end()) *it = std::move(v);   // a repeat: the last one wins
        else                       out.bones.push_back(std::move(v));
    }
}

std::shared_ptr<const ModelVirtualBones> ModelVirtualBonesAt(const std::string& iscnPath)
{
    struct Cached { std::shared_ptr<const ModelVirtualBones> data; bool loose = false; uint64_t sig0 = 0, sig1 = 0; };
    static std::mutex                              s_mutex;
    static std::unordered_map<std::string, Cached> s_cache;
    static const auto                              s_empty = std::make_shared<const ModelVirtualBones>();
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
    auto parsed = std::make_shared<ModelVirtualBones>();
    std::vector<uint8_t> blob;
    if (AssetFS::Get().ReadFile(iscnPath, blob) && blob.size() > sizeof(AssetHeader) &&
        ValidateHeader(blob.data(), blob.size(), MAGIC_SCENE))
    {
        const auto*  hdr  = reinterpret_cast<const AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        ParseModelVirtualBones(std::string(payl, n), *parsed);
    }
    Cached fresh{ parsed, loose, sig0, sig1 };
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[k] = fresh;
    return parsed;
}

std::string FormatVirtualBone(const ModelVirtualBone& vb)
{
    if (!VirtualBoneNameProblem(vb.name).empty()) return {};
    std::string out = "VB name=" + AssetRefs::PercentEncodeStrict(vb.name) + " from=" +
                      AssetRefs::PercentEncodeStrict(vb.from) + " to=" + AssetRefs::PercentEncodeStrict(vb.to);
    if (vb.orphan) out += " orphan=1";
    return out + "\n";
}

std::string FormatVirtualBones(const std::vector<ModelVirtualBone>& list)
{
    std::string out;
    for (const ModelVirtualBone& v : list) out += FormatVirtualBone(v);
    return out;
}

uint64_t VirtualBonesSignature(const std::vector<ModelVirtualBone>& vbs)
{
    if (vbs.empty()) return 0;
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    auto mix = [&h](const std::string& s)
    {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xFFu;
        h *= 1099511628211ull;
    };
    for (const ModelVirtualBone& v : vbs)
    {
        if (v.orphan) continue;   // not applied: does not change the skeleton
        mix(v.name);
        mix(v.from);
        mix(v.to);
    }
    return h ? h : 1ull;
}

uint32_t ApplyVirtualBones(SkeletonAsset& skel, const std::vector<ModelVirtualBone>& vbs,
                           std::vector<std::string>* warnings)
{
    const uint32_t real = skel.RealBoneCount();
    skel.realBoneCount  = real;
    if (vbs.empty()) return 0;
    auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(w); };

    // The real bones' REST model matrices: FK over restPoseLocal (a root's local at rest is its
    // restPoseLocal). Each bone's parent first, whatever the index order: a shipped skeleton can list a
    // child before its parent, and a forward scan would read an uncomputed parent and give a VB there
    // a zero bind pose (a NaN inverse).
    std::vector<XMFLOAT4X4> restModel(real);
    std::vector<uint8_t>    done(real, 0);
    std::vector<uint32_t>   chain;
    for (uint32_t b = 0; b < real; ++b)
    {
        chain.clear();
        for (int32_t c = static_cast<int32_t>(b); c >= 0 && static_cast<uint32_t>(c) < real && !done[c] &&
                                                   chain.size() <= real;
             c = skel.parentIndex[c])
            chain.push_back(static_cast<uint32_t>(c));
        for (std::size_t i = chain.size(); i-- > 0;)
        {
            const uint32_t x = chain[i];
            const int32_t  p = skel.parentIndex[x];
            XMMATRIX m = XMLoadFloat4x4(&skel.restPoseLocal[x]);
            if (p >= 0 && static_cast<uint32_t>(p) < real && static_cast<uint32_t>(p) != x && done[p])
                m = XMMatrixMultiply(m, XMLoadFloat4x4(&restModel[p]));
            XMStoreFloat4x4(&restModel[x], m);
            done[x] = 1;
        }
    }
    auto findReal = [&](const std::string& name) -> int
    {
        for (uint32_t b = 0; b < real; ++b)
            if (NameOf(skel, b) == name) return static_cast<int>(b);
        return -1;
    };

    uint32_t added = 0;
    for (const ModelVirtualBone& v : vbs)
    {
        if (v.orphan) continue;   // a re-import lost its bone: kept in the file, not applied
        const std::string who = "virtual bone '" + v.name + "'";
        if (const std::string p = VirtualBoneNameProblem(v.name); !p.empty()) { warn(who + ": " + p); continue; }
        const int from = findReal(v.from), to = findReal(v.to);
        if (from < 0 || to < 0)
        {
            warn(who + ": the skeleton has no bone '" + (from < 0 ? v.from : v.to) + "' (a virtual bone's bones are real "
                 "bones) - not applied");
            continue;
        }
        if (from == to) { warn(who + ": from and to are the same bone - not applied"); continue; }
        bool clash = false;
        for (uint32_t b = 0; b < skel.boneCount && !clash; ++b) clash = NameOf(skel, b) == v.name;
        if (clash) { warn(who + ": a bone of that name exists - not applied"); continue; }
        bool pair = false;
        for (const auto& d : skel.virtualBones)
            pair = pair || (d.from == static_cast<uint32_t>(from) && d.to == static_cast<uint32_t>(to));
        if (pair) { warn(who + ": another virtual bone already joins '" + v.from + "' to '" + v.to + "' - not applied"); continue; }
        if (skel.boneCount >= SkeletonAsset::MAX_BONES)
        {
            warn(who + ": the skeleton is full (" + std::to_string(SkeletonAsset::MAX_BONES) + " bones) - not applied");
            continue;
        }
        {
            // A rest that cannot be inverted (a zero scale) would give a NaN bind.
            XMVECTOR detTo, detFrom;
            XMMatrixInverse(&detTo, XMLoadFloat4x4(&restModel[static_cast<uint32_t>(to)]));
            XMMatrixInverse(&detFrom, XMLoadFloat4x4(&restModel[static_cast<uint32_t>(from)]));
            if (!(std::fabs(XMVectorGetX(detTo)) > 1e-12f) || !(std::fabs(XMVectorGetX(detFrom)) > 1e-12f))
            {
                warn(who + ": the rest pose of '" + v.from + "' or '" + v.to + "' is degenerate (zero scale) - not applied");
                continue;
            }
        }

        const uint32_t b = skel.boneCount++;
        skel.parentIndex[b] = from;
        std::memset(skel.boneNames[b], 0, sizeof(skel.boneNames[b]));
        std::memcpy(skel.boneNames[b], v.name.data(), (std::min)(v.name.size(), sizeof(skel.boneNames[b]) - 1));
        skel.nameToIndex[Fnv32(skel.boneNames[b])] = b;
        skel.grantSource[b]    = -1;   // never a grant (the default 0 would name bone 0)
        skel.grantRatio[b]     = 0.f;
        skel.transformOrder[b] = 0;
        skel.boneRestAABBs[b]  = SkeletonAsset::BoneAABB{};   // no skin: empty
        // Bind = to's rest model matrix; rest = to's rest place under from.
        const XMMATRIX to4 = XMLoadFloat4x4(&restModel[static_cast<uint32_t>(to)]);
        const XMMATRIX fr4 = XMLoadFloat4x4(&restModel[static_cast<uint32_t>(from)]);
        XMVECTOR det;
        XMStoreFloat4x4(&skel.bindPose[b], to4);
        XMStoreFloat4x4(&skel.inverseBindPose[b], XMMatrixInverse(&det, to4));
        XMStoreFloat4x4(&skel.restPoseLocal[b], XMMatrixMultiply(to4, XMMatrixInverse(&det, fr4)));
        SkeletonAsset::VirtualBoneDef def;
        def.index = b;
        def.from  = static_cast<uint32_t>(from);
        def.to    = static_cast<uint32_t>(to);
        XMVECTOR rs, rr, rt;
        if (XMMatrixDecompose(&rs, &rr, &rt, XMLoadFloat4x4(&skel.restPoseLocal[b])))
        {
            XMStoreFloat3(&def.restPos, rt);
            XMStoreFloat4(&def.restRot, XMQuaternionNormalize(rr));
            XMStoreFloat3(&def.restScl, rs);
        }
        skel.virtualBones.push_back(def);
        ++added;
    }
    return added;
}
}
