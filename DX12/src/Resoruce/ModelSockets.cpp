// Resource/ModelSockets.cpp - the SO records. See the header for the grammar.

#include "Resource/ModelSockets.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"
#include "Resource/MeshLibIdentity.h"   // MeshKeyFromHex / MeshKeyToHex / MeshLibCanonicalPath
#include "System/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    bool ParseFloats(const std::string& v, float* out, int n)
    {
        std::size_t b = 0;
        for (int i = 0; i < n; ++i)
        {
            const std::size_t e = (i + 1 < n) ? v.find(',', b) : v.size();
            if (e == std::string::npos || e < b) return false;
            const std::string one = v.substr(b, e - b);
            if (one.empty()) return false;
            char* end = nullptr;
            const float f = std::strtof(one.c_str(), &end);
            if (!end || *end || !std::isfinite(f)) return false;
            out[i] = f;
            b = e + 1;
        }
        return true;
    }

    // The shortest text that reads back as exactly @p f: a saved socket re-read is
    // the socket that was saved (no drift over save / load, no false "dirty"),
    // and a plain value stays plain ("0.1", not "0.100000001").
    std::string Num(float f)
    {
        char b[32];
        for (int digits = 6; digits <= 9; ++digits)
        {
            std::snprintf(b, sizeof(b), "%.*g", digits, static_cast<double>(f));
            if (std::strtof(b, nullptr) == f) break;
        }
        return b;
    }

    // The key=value tokens of one record, after its two-letter prefix.
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
}

XMFLOAT4X4 ModelSocket::LocalOffset() const
{
    const XMMATRIX m = XMMatrixMultiply(
        XMMatrixMultiply(XMMatrixScaling(s.x, s.y, s.z), XMMatrixRotationQuaternion(XMLoadFloat4(&q))),
        XMMatrixTranslation(t.x, t.y, t.z));
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, m);
    return out;
}

bool ModelSocket::SetFromOffset(const XMFLOAT4X4& m)
{
    XMVECTOR sc, rot, tr;
    if (!XMMatrixDecompose(&sc, &rot, &tr, XMLoadFloat4x4(&m))) return false;
    XMStoreFloat3(&s, sc);
    XMStoreFloat4(&q, XMQuaternionNormalize(rot));
    XMStoreFloat3(&t, tr);
    return true;
}

bool ModelSocket::operator==(const ModelSocket& o) const
{
    return name == o.name && bone == o.bone && meshKey == o.meshKey && t.x == o.t.x && t.y == o.t.y &&
           t.z == o.t.z && q.x == o.q.x && q.y == o.q.y && q.z == o.q.z && q.w == o.q.w && s.x == o.s.x &&
           s.y == o.s.y && s.z == o.s.z && source == o.source && orphan == o.orphan;
}

const ModelSocket* ModelSockets::Find(const std::string& name) const
{
    for (const ModelSocket& s : sockets)
        if (s.IsSkeletonSocket() && s.name == name) return &s;
    return nullptr;
}

std::size_t ModelSockets::SkeletonSocketCount() const
{
    return static_cast<std::size_t>(
        std::count_if(sockets.begin(), sockets.end(), [](const ModelSocket& s) { return s.IsSkeletonSocket(); }));
}

std::string SocketNameProblem(const std::string& name)
{
    if (name.empty()) return "a socket needs a name";
    if (name.size() > 63) return "a socket name is at most 63 bytes (SocketComponent's name[64])";
    return {};
}

void ParseModelSockets(const std::string& text, ModelSockets& out, std::vector<std::string>* warnings)
{
    out.sockets.clear();
    auto warn = [&](unsigned n, const std::string& why)
    {
        if (warnings) warnings->push_back("line " + std::to_string(n) + ": " + why);
    };
    std::istringstream ss(text);
    std::string        line;
    unsigned           n = 0;
    while (std::getline(ss, line))
    {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!(line.size() > 3 && line[0] == 'S' && line[1] == 'O' && line[2] == ' ')) continue;

        ModelSocket s;
        bool bad = false;
        for (const auto& [k, v] : Fields(line))
        {
            if (k == "name")        s.name = AssetRefs::PercentDecode(v);
            else if (k == "bone")   s.bone = AssetRefs::PercentDecode(v);
            else if (k == "mesh")   { if (!MeshKeyFromHex(v, s.meshKey) || s.meshKey == 0) bad = true; }
            else if (k == "t")      { if (!ParseFloats(v, &s.t.x, 3)) bad = true; }
            else if (k == "q")      { if (!ParseFloats(v, &s.q.x, 4)) bad = true; }
            else if (k == "s")      { if (!ParseFloats(v, &s.s.x, 3)) bad = true; }
            else if (k == "src")    s.source = AssetRefs::PercentDecode(v);
            else if (k == "orphan") s.orphan = (v == "1");
        }
        // A skeleton socket names its bone, a mesh socket its Mesh - never both.
        if (bad || !SocketNameProblem(s.name).empty() || (s.meshKey == 0) == s.bone.empty())
        {
            warn(n, "malformed SO record - skipped");
            continue;
        }
        // A zero-length quaternion is not a rotation: identity, said once. One that
        // is unit to float precision is kept bit for bit: what a Save wrote reads
        // back as the same socket (the Skeleton Editor compares them).
        const float ql = std::sqrt(s.q.x * s.q.x + s.q.y * s.q.y + s.q.z * s.q.z + s.q.w * s.q.w);
        if (ql < 1e-6f) { s.q = { 0.f, 0.f, 0.f, 1.f }; warn(n, "zero rotation - identity used"); }
        else if (std::fabs(ql - 1.f) > 1e-5f) { s.q.x /= ql; s.q.y /= ql; s.q.z /= ql; s.q.w /= ql; }

        auto same = [&](const ModelSocket& o) { return o.meshKey == s.meshKey && o.name == s.name; };
        const auto it = std::find_if(out.sockets.begin(), out.sockets.end(), same);
        if (it != out.sockets.end()) *it = std::move(s);   // a repeat: the last one wins
        else                         out.sockets.push_back(std::move(s));
    }
}

std::shared_ptr<const ModelSockets> ModelSocketsAt(const std::string& iscnPath)
{
    struct Cached { std::shared_ptr<const ModelSockets> data; bool loose = false; uint64_t sig0 = 0, sig1 = 0; };
    static std::mutex                              s_mutex;
    static std::unordered_map<std::string, Cached> s_cache;
    static const auto                              s_empty = std::make_shared<const ModelSockets>();
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

    auto parsed = std::make_shared<ModelSockets>();
    std::vector<uint8_t> blob;
    if (AssetFS::Get().ReadFile(iscnPath, blob) && blob.size() > sizeof(AssetHeader) &&
        ValidateHeader(blob.data(), blob.size(), MAGIC_SCENE))
    {
        const auto*  hdr  = reinterpret_cast<const AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        ParseModelSockets(std::string(payl, n), *parsed);
    }
    Cached fresh{ parsed, loose, sig0, sig1 };
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[k] = fresh;
    return parsed;
}

std::string FormatSocket(const ModelSocket& s)
{
    if (!SocketNameProblem(s.name).empty()) return {};
    std::string out = "SO ";
    if (s.meshKey) out += "mesh=" + MeshKeyToHex(s.meshKey) + " ";
    out += "name=" + AssetRefs::PercentEncodeStrict(s.name);
    if (!s.meshKey) out += " bone=" + AssetRefs::PercentEncodeStrict(s.bone);
    out += " t=" + Num(s.t.x) + "," + Num(s.t.y) + "," + Num(s.t.z);
    out += " q=" + Num(s.q.x) + "," + Num(s.q.y) + "," + Num(s.q.z) + "," + Num(s.q.w);
    out += " s=" + Num(s.s.x) + "," + Num(s.s.y) + "," + Num(s.s.z);
    if (!s.source.empty()) out += " src=" + AssetRefs::PercentEncodeStrict(s.source);
    if (s.orphan) out += " orphan=1";
    out += "\n";
    return out;
}

std::string FormatSockets(const std::vector<ModelSocket>& list)
{
    std::string out;
    for (const ModelSocket& s : list) out += FormatSocket(s);
    return out;
}
}
