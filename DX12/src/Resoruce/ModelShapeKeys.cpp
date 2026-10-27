// Resource/ModelShapeKeys.cpp - the MK / MP records. See the header for the grammar.

#include "Resource/ModelShapeKeys.h"

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

namespace Resource
{
namespace
{
    bool ParseFloat(const std::string& v, float& out)
    {
        if (v.empty()) return false;
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        if (!end || *end || !std::isfinite(f)) return false;
        out = f;
        return true;
    }

    std::string Num(float f)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.6g", static_cast<double>(f));
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

const std::vector<std::string>& ShapeKeyCategories()
{
    static const std::vector<std::string> kCategories = { "expression", "viseme", "body", "corrective" };
    return kCategories;
}

bool IsShapeKeyCategory(const std::string& c)
{
    if (c.empty()) return true;
    const auto& k = ShapeKeyCategories();
    return std::find(k.begin(), k.end(), c) != k.end();
}

const ShapeKeyMeta* MeshShapeKeySetup::FindMeta(const std::string& name) const
{
    for (const ShapeKeyMeta& m : meta)
        if (m.name == name) return &m;
    return nullptr;
}

const ShapeKeyPreset* MeshShapeKeySetup::FindPreset(const std::string& name) const
{
    for (const ShapeKeyPreset& p : presets)
        if (p.name == name) return &p;
    return nullptr;
}

void ParseModelShapeKeys(const std::string& text, ModelShapeKeys& out, std::vector<std::string>* warnings)
{
    out.meshes.clear();
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
        const bool mk = line.size() > 3 && line[0] == 'M' && line[1] == 'K' && line[2] == ' ';
        const bool mp = line.size() > 3 && line[0] == 'M' && line[1] == 'P' && line[2] == ' ';
        if (!mk && !mp) continue;

        uint64_t key = 0;
        bool     orphan = false, bad = false;
        ShapeKeyMeta   meta;
        ShapeKeyPreset preset;
        for (const auto& [k, v] : Fields(line))
        {
            if (k == "mesh")
            {
                if (!MeshKeyFromHex(v, key)) bad = true;
            }
            else if (k == "name")   (mk ? meta.name : preset.name) = AssetRefs::PercentDecode(v);
            else if (k == "orphan") orphan = (v == "1");
            else if (k == "src")    (mk ? meta.source : preset.source) = v;
            else if (mk && k == "cat")
            {
                meta.category = v;
                if (!IsShapeKeyCategory(v)) { warn(n, "unknown category '" + v + "' - ignored"); meta.category.clear(); }
            }
            else if (mk && k == "default") { if (!ParseFloat(v, meta.def)) bad = true; }
            else if (mk && k == "min")     { if (!ParseFloat(v, meta.min)) bad = true; }
            else if (mk && k == "max")     { if (!ParseFloat(v, meta.max)) bad = true; }
            else if (mk && k == "label")   meta.label = AssetRefs::PercentDecode(v);
            else if (mp && k == "w")
            {
                for (size_t b = 0; b <= v.size();)
                {
                    const size_t e = std::min(v.find(',', b), v.size());
                    const std::string pair = v.substr(b, e - b);
                    b = e + 1;
                    if (pair.empty()) continue;
                    const size_t colon = pair.rfind(':');
                    float w = 0.f;
                    if (colon == std::string::npos || !ParseFloat(pair.substr(colon + 1), w)) { bad = true; break; }
                    preset.weights.emplace_back(AssetRefs::PercentDecode(pair.substr(0, colon)), w);
                }
            }
        }
        const std::string& name = mk ? meta.name : preset.name;
        if (bad || key == 0 || name.empty())
        {
            warn(n, std::string("malformed ") + (mk ? "MK" : "MP") + " record - skipped");
            continue;
        }
        if (mk)
        {
            meta.min = std::clamp(meta.min, 0.f, 1.f);
            meta.max = std::clamp(meta.max, 0.f, 1.f);
            if (meta.min >= meta.max) { meta.min = 0.f; meta.max = 1.f; warn(n, "empty range - 0..1 used"); }
            meta.def = std::clamp(meta.def, meta.min, meta.max);
        }
        MeshShapeKeySetup& s = out.meshes[key];
        s.meshKey = key;
        s.orphan  = s.orphan || orphan;
        if (mk)
        {
            auto it = std::find_if(s.meta.begin(), s.meta.end(), [&](const ShapeKeyMeta& m) { return m.name == meta.name; });
            if (it != s.meta.end()) *it = std::move(meta);   // a repeat: the last one wins
            else                    s.meta.push_back(std::move(meta));
        }
        else
        {
            for (auto& [t, w] : preset.weights) w = std::clamp(w, 0.f, 1.f);
            auto it = std::find_if(s.presets.begin(), s.presets.end(),
                                   [&](const ShapeKeyPreset& p) { return p.name == preset.name; });
            if (it != s.presets.end()) *it = std::move(preset);
            else                       s.presets.push_back(std::move(preset));
        }
    }
}

std::shared_ptr<const ModelShapeKeys> ModelShapeKeysAt(const std::string& iscnPath)
{
    struct Cached { std::shared_ptr<const ModelShapeKeys> data; bool loose = false; uint64_t sig0 = 0, sig1 = 0; };
    static std::mutex                              s_mutex;
    static std::unordered_map<std::string, Cached> s_cache;
    static const auto                              s_empty = std::make_shared<const ModelShapeKeys>();
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

    auto parsed = std::make_shared<ModelShapeKeys>();
    std::vector<uint8_t> blob;
    if (AssetFS::Get().ReadFile(iscnPath, blob) && blob.size() > sizeof(AssetHeader) &&
        ValidateHeader(blob.data(), blob.size(), MAGIC_SCENE))
    {
        const auto*  hdr  = reinterpret_cast<const AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        ParseModelShapeKeys(std::string(payl, n), *parsed);
    }
    Cached fresh{ parsed, loose, sig0, sig1 };
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[k] = fresh;
    return parsed;
}

std::string FormatShapeKeys(const MeshShapeKeySetup& setup)
{
    std::string out;
    const std::string mesh = "mesh=" + MeshKeyToHex(setup.meshKey);
    const std::string tail = setup.orphan ? " orphan=1\n" : "\n";
    for (const ShapeKeyMeta& m : setup.meta)
    {
        // An all-default record says nothing - unless the AUTHOR wrote it: then it
        // is "the defaults, not the source's" (the Mesh Editor keeps one when an
        // imported default is reset), and a re-import must not bring the source's back.
        if (m.name.empty() || (m.IsDefault() && !m.source.empty())) continue;
        out += "MK " + mesh + " name=" + AssetRefs::PercentEncodeStrict(m.name);
        if (!m.category.empty()) out += " cat=" + m.category;
        if (m.def != 0.f)        out += " default=" + Num(m.def);
        if (m.min != 0.f)        out += " min=" + Num(m.min);
        if (m.max != 1.f)        out += " max=" + Num(m.max);
        if (!m.label.empty())    out += " label=" + AssetRefs::PercentEncodeStrict(m.label);
        if (!m.source.empty())   out += " src=" + AssetRefs::PercentEncodeStrict(m.source);
        out += tail;
    }
    for (const ShapeKeyPreset& p : setup.presets)
    {
        if (p.name.empty()) continue;
        out += "MP " + mesh + " name=" + AssetRefs::PercentEncodeStrict(p.name) + " w=";
        for (size_t i = 0; i < p.weights.size(); ++i)
            out += (i ? "," : "") + AssetRefs::PercentEncodeStrict(p.weights[i].first) + ":" + Num(p.weights[i].second);
        if (!p.source.empty()) out += " src=" + AssetRefs::PercentEncodeStrict(p.source);
        out += tail;
    }
    return out;
}
}
