#include "Scripting/ScriptManifest.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace Scripting
{
namespace
{
    int StageRank(BootScriptKind kind)
    {
        switch (kind)
        {
            case BootScriptKind::Global:  return 0;
            case BootScriptKind::Service: return 1;
            case BootScriptKind::System:  return 2;
            case BootScriptKind::UI:      return 3;
        }
        return 4;
    }

    // ASCII fold is enough: manifest paths are project-relative ASCII by
    // convention, and the check only has to catch case-typo duplicates on a
    // case-insensitive filesystem.
    std::string FoldCase(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    bool HasDotDotSegment(const std::string& p)
    {
        size_t start = 0;
        while (start <= p.size())
        {
            const size_t slash = p.find('/', start);
            const size_t end = (slash == std::string::npos) ? p.size() : slash;
            if (end - start == 2 && p[start] == '.' && p[start + 1] == '.')
                return true;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return false;
    }

    bool IsAbsolutePath(const std::string& p)
    {
        return !p.empty() && (p.front() == '/' || (p.size() >= 2 && p[1] == ':'));
    }

    bool EndsWithLua(const std::string& p)
    {
        return p.size() > 4 && p.compare(p.size() - 4, 4, ".lua") == 0;
    }
}

const char* BootScriptKindName(BootScriptKind kind)
{
    switch (kind)
    {
        case BootScriptKind::Global:  return "global";
        case BootScriptKind::Service: return "service";
        case BootScriptKind::System:  return "system";
        case BootScriptKind::UI:      return "ui";
    }
    return "?";
}

bool LoadScriptManifest(const std::string& manifestPath,
                        bool pakOnly,
                        ScriptManifest& out,
                        std::vector<std::string>& errors)
{
    out = {};

    auto& afs = ::Resource::AssetFS::Get();

    std::string src;
    if (!afs.ReadFileText(manifestPath, src))
    {
        errors.push_back("script manifest unreadable: " + manifestPath);
        return false;
    }

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(src);
    }
    catch (const std::exception& ex)
    {
        errors.push_back(std::string("script manifest JSON parse failed: ") + ex.what());
        return false;
    }

    if (!j.contains("version") || !j["version"].is_number_integer()
        || j["version"].get<int64_t>() != 1)
    {
        errors.push_back("script manifest `version` must be the integer 1");
        return false;
    }
    out.version = 1;

    if (!j.contains("entries") || !j["entries"].is_array())
    {
        errors.push_back("script manifest `entries` must be an array");
        return false;
    }

    std::unordered_set<std::string> seenPaths;    // case-folded
    std::unordered_set<std::string> seenKindStem; // "<kind>/<folded stem>"

    size_t idx = 0;
    for (const auto& e : j["entries"])
    {
        const std::string where = "entry #" + std::to_string(idx++);
        if (!e.is_object())
        {
            errors.push_back(where + ": not an object");
            continue;
        }

        BootScriptEntry entry;

        if (!e.contains("kind") || !e["kind"].is_string())
        {
            errors.push_back(where + ": missing `kind`");
            continue;
        }
        const std::string kindStr = e["kind"].get<std::string>();
        if      (kindStr == "global")  entry.kind = BootScriptKind::Global;
        else if (kindStr == "service") entry.kind = BootScriptKind::Service;
        else if (kindStr == "system")  entry.kind = BootScriptKind::System;
        else if (kindStr == "ui")      entry.kind = BootScriptKind::UI;
        else
        {
            errors.push_back(where + ": unknown kind '" + kindStr + "'");
            continue;
        }

        if (!e.contains("path") || !e["path"].is_string())
        {
            errors.push_back(where + " (" + kindStr + "): missing `path`");
            continue;
        }
        entry.path = e["path"].get<std::string>();
        if (entry.path.empty())
        {
            errors.push_back(where + " (" + kindStr + "): empty `path`");
            continue;
        }
        if (entry.path.find('\\') != std::string::npos)
        {
            errors.push_back(entry.path + ": path must use forward slashes");
            continue;
        }
        if (IsAbsolutePath(entry.path))
        {
            errors.push_back(entry.path + ": absolute paths are not allowed");
            continue;
        }
        if (HasDotDotSegment(entry.path))
        {
            errors.push_back(entry.path + ": `..` segments are not allowed");
            continue;
        }
        if (!EndsWithLua(entry.path))
        {
            errors.push_back(entry.path + ": boot entries must reference a .lua file");
            continue;
        }

        const size_t slash = entry.path.rfind('/');
        const std::string base =
            (slash == std::string::npos) ? entry.path : entry.path.substr(slash + 1);
        entry.stem = base.substr(0, base.size() - 4);
        if (entry.stem.empty())
        {
            errors.push_back(entry.path + ": empty filename stem");
            continue;
        }

        if (e.contains("order"))
        {
            const int64_t raw = e["order"].is_number_integer() ? e["order"].get<int64_t>()
                                                               : int64_t{ 0 };
            if (!e["order"].is_number_integer())
                LOG_WARNING("ScriptManifest: %s (%s) `order` must be an integer — using 0",
                            entry.path.c_str(), kindStr.c_str());
            else if (raw < INT32_MIN || raw > INT32_MAX)
                // A silent int64->int32 wrap would move the entry to the FRONT
                // of its stage; malformed order degrades to 0 like every other
                // bad field, never to a surprise position.
                LOG_WARNING("ScriptManifest: %s (%s) `order`=%lld is outside int32 — using 0",
                            entry.path.c_str(), kindStr.c_str(), static_cast<long long>(raw));
            else
            {
                entry.order    = static_cast<int32_t>(raw);
                entry.hasOrder = true;
            }
        }

        if (e.contains("enabled"))
        {
            if (e["enabled"].is_boolean())
                entry.enabled = e["enabled"].get<bool>();
            else
                LOG_WARNING("ScriptManifest: %s (%s) `enabled` must be a boolean — keeping enabled",
                            entry.path.c_str(), kindStr.c_str());
        }

        if (e.contains("set"))
        {
            if (entry.kind != BootScriptKind::System)
            {
                errors.push_back(entry.path + ": `set` is only legal on system entries");
                continue;
            }
            if (!e["set"].is_string())
            {
                errors.push_back(entry.path + ": `set` must be a string");
                continue;
            }
            entry.systemSet = e["set"].get<std::string>();
        }

        if (e.contains("scope"))
        {
            if (entry.kind != BootScriptKind::System)
            {
                errors.push_back(entry.path + ": `scope` is only legal on system entries");
                continue;
            }
            const std::string scope = e["scope"].is_string() ? e["scope"].get<std::string>()
                                                             : std::string{};
            if (scope != "engine" && scope != "scene")
            {
                errors.push_back(entry.path + ": `scope` must be \"engine\" or \"scene\"");
                continue;
            }
            entry.sceneScoped = scope == "scene";
            entry.hasScope    = true;
        }

        // Case-folded duplicate checks: two case-variant spellings of one
        // file would run the same chunk twice with two registry ids and
        // doubled Engine.Subscribe handlers.
        if (!seenPaths.insert(FoldCase(entry.path)).second)
        {
            errors.push_back(entry.path + ": duplicate path (case-insensitive) — a chunk must not run twice");
            continue;
        }
        if (!seenKindStem.insert(kindStr + "/" + FoldCase(entry.stem)).second)
        {
            errors.push_back(entry.path + ": duplicate (" + kindStr + ", " + entry.stem
                             + ") registration id");
            continue;
        }

        if (entry.enabled)
        {
            const bool present = pakOnly ? afs.HasInPakExact(entry.path)
                                         : afs.Exists(entry.path);
            if (!present)
            {
                errors.push_back(entry.path + (pakOnly ? ": not in the mounted pak"
                                                       : ": file not found"));
                continue;
            }
        }

        out.entries.push_back(std::move(entry));
    }

    std::stable_sort(out.entries.begin(), out.entries.end(),
                     [](const BootScriptEntry& a, const BootScriptEntry& b)
                     {
                         const int ra = StageRank(a.kind), rb = StageRank(b.kind);
                         if (ra != rb)           return ra < rb;
                         if (a.order != b.order) return a.order < b.order;
                         return a.stem < b.stem;
                     });

    return true;
}
}
