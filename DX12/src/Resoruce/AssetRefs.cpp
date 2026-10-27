#include "Resource/AssetRefs.h"
#include "Resource/AssetTypes.generated.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"
#include "System/Log.h"
#include "System/PathUtil.h"   // U8Path / PathToUtf8: the only legal narrow <-> wide crossing

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace Resource::AssetRefs
{
namespace
{
    // Positive-only existence memo (see ForgetExistenceCache): one stat per reference per session.
    // Misses are never cached, since an import can make a missing path appear at any moment. A cached
    // "present" goes stale when the asset moves, so AssetGuidIndex clears the memo on every event that
    // moves the tree; in a shipped game nothing moves.
    std::mutex                      g_existMutex;
    std::unordered_set<std::string> g_existPresent;

    bool RefTargetExists(const std::string& path)
    {
        {
            std::lock_guard<std::mutex> lk(g_existMutex);
            if (g_existPresent.find(path) != g_existPresent.end()) return true;
        }
        // AssetFS::Exists, not fs::exists: the latter sees only the process working directory, so with
        // an engine root separate from the project root it would report every engine asset missing and
        // "repair" good references.
        const bool present = AssetFS::Get().Exists(path);
        if (present)
        {
            std::lock_guard<std::mutex> lk(g_existMutex);
            g_existPresent.insert(path);
        }
        return present;
    }

    // The current path of @p g, building the index on first use. The only place resolution touches
    // the index, so a healthy load never pays for the walk. Do not drop the EnsureBuilt: an unbuilt
    // index returns "" for every guid and the repair silently does nothing.
    std::string RepairTargetForGuid(const ECS::Guid& g)
    {
        AssetGuidIndex& idx = AssetGuidIndex::Get();
        idx.EnsureBuilt();
        return idx.PathForGuid(g);
    }

    bool StartsWith(std::string_view s, const char* p)
    {
        const size_t n = std::strlen(p);
        return s.size() >= n && s.compare(0, n, p) == 0;
    }

    void TrimInPlace(std::string& s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    }

    // Shape test shared by every .iscene/.ipfb walker: `  Tag: k=v ...`.
    // `skip` marks lines no walker may touch (comments, virtual folders).
    void SplitSceneLine(std::string_view line, std::string& tag,
                        size_t& bodyStart, bool& skip)
    {
        tag.clear();
        bodyStart = 0;
        skip      = line.empty() || line[0] == '#';
        if (skip) return;
        if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            const size_t colon = line.find(':', 2);
            if (colon != std::string_view::npos)
            {
                tag       = std::string(line.substr(2, colon - 2));
                bodyStart = colon + 1;
                if (tag == "EdFolder") skip = true;   // virtual folders, not assets
            }
        }
    }

    // The MaterialOverride scalar prefixes (f:/v:/i:/b:) are values, not paths;
    // t: is a texture path carrying a type tag. Returns the path portion and
    // whether the t: tag has to be re-emitted, or false when the value is a
    // scalar that must be left alone.
    bool SplitOverrideValue(const std::string& val, std::string& outPath, bool& outTexTag)
    {
        outTexTag = false;
        if (val.size() > 2 && val[1] == ':' &&
            (val[0] == 'f' || val[0] == 'v' || val[0] == 'i' || val[0] == 'b'))
            return false;
        if (val.size() > 2 && val[0] == 't' && val[1] == ':')
        {
            outTexTag = true;
            outPath   = val.substr(2);
        }
        else
            outPath = val;
        return true;
    }

    // Where the path starts inside a trimmed .imat value, honouring the
    // two-token `ctex_<Name> = <uvset> <path>` shape. npos when the line has
    // no path portion at all.
    size_t ImatPathOffset(const std::string& key, std::string_view val)
    {
        if (!StartsWith(key, "ctex_")) return 0;
        const size_t sp = val.find(' ');
        if (sp == std::string_view::npos) return std::string_view::npos;
        size_t p = sp;
        while (p < val.size() && val[p] == ' ') ++p;
        return p < val.size() ? p : std::string_view::npos;
    }

    // Iterate @p text line by line, preserving the exact newline layout. `fn(line, out)` appends
    // the replacement, or returns false to drop the line including its newline: Augment strips the
    // guid lines it re-derives, and leftover blank lines would grow the file on every round trip.
    template <typename F>
    std::string ForEachLine(const std::string& text, F&& fn)
    {
        std::string out;
        out.reserve(text.size() + 64);
        size_t pos = 0;
        while (pos < text.size())
        {
            const size_t eol     = text.find('\n', pos);
            const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol;
            const size_t mark    = out.size();
            if (!fn(std::string_view(text.data() + pos, lineEnd - pos), out))
                out.resize(mark);          // dropped: swallow the newline too
            else if (eol != std::string::npos)
                out.push_back('\n');
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        return out;
    }
} // namespace

// ==== Shared predicates and encodings ====

std::string ToLowerAscii(std::string s)
{
    // ASCII range only: these strings are UTF-8, and ::tolower on a byte >= 0x80 of a multi-byte
    // sequence is locale-dependent.
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string PercentDecode(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size()
            && std::isxdigit(static_cast<unsigned char>(s[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(s[i + 2])))
        {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                return (::tolower(static_cast<unsigned char>(c)) - 'a') + 10;
            };
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        }
        else
            out.push_back(s[i]);
    }
    return out;
}

std::string PercentEncode(std::string_view s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s)
    {
        if      (c == '%') r += "%25";
        else if (c == ' ') r += "%20";
        else               r += c;
    }
    return r;
}

std::string PercentEncodeStrict(std::string_view s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string r;
    r.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s)
    {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                          c == '/' || c == '-' || c == ':';
        if (safe) r += static_cast<char>(c);
        else { r += '%'; r += kHex[c >> 4]; r += kHex[c & 0xF]; }
    }
    return r;
}

std::string NormalizeRefPath(const std::string& raw)
{
    std::string s = PercentDecode(raw);
    for (char& c : s) if (c == '\\') c = '/';
    s = ToLowerAscii(std::move(s));
    while (s.rfind("./", 0) == 0) s.erase(0, 2);

    // Never hand a narrow std::string to fs::path here, nor take generic_string() back off one: those
    // conversions use the ANSI code page, and a non-ASCII asset name (hundreds exist) throws "No
    // mapping for the Unicode character..." in the middle of an asset scan. Sys::U8Path /
    // Sys::PathToUtf8 are the only legal crossing (System/PathUtil.h). Asset paths are usually already
    // lexically normal, so the fast path skips the UTF-8 <-> UTF-16 round trip that would otherwise
    // run per reference on every container load.
    if (s.find("//")   == std::string::npos &&
        s.find("/./")  == std::string::npos &&
        s.find("/../") == std::string::npos)
        return s;

    std::string out = Sys::PathToUtf8(Sys::U8Path(s).lexically_normal());
    for (char& c : out) if (c == '\\') c = '/';
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    return out;
}

// A value is a reference when it ends in an AssetTypes row with ref (src/Resoruce/AssetTypes.json):
// keyed on the extension, not on a list of serializer keys - keys drift with every new component,
// extensions do not.
bool LooksLikeAssetPath(const std::string& lowerValue)
{
    const AssetTypes::Row* row = AssetTypes::FindByPath(lowerValue);
    if (!row || !row->ref) return false;
    return lowerValue.find('/')  != std::string::npos
        || lowerValue.find('\\') != std::string::npos
        || lowerValue.rfind("asset", 0) == 0;
}

bool ValueIsAssetPath(const std::string& value)
{
    return LooksLikeAssetPath(ToLowerAscii(value));
}

bool HasGuidTokens(const std::string& text)
{
    return text.find(kGuidPrefix) != std::string::npos;
}

void ForgetExistenceCache()
{
    std::lock_guard<std::mutex> lk(g_existMutex);
    g_existPresent.clear();
}

// ==== .imat / .imatinst text ====

std::string AugmentImatText(const std::string& text)
{
    AssetGuidIndex& idx = AssetGuidIndex::Get();
    idx.EnsureBuilt();

    return ForEachLine(text, [&](std::string_view line, std::string& out) -> bool {
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) { out.append(line); return true; }

        std::string key(line.substr(0, eq));
        TrimInPlace(key);
        // Drop any guid line already present: Augment is idempotent, and a stale guid is worse than none.
        if (StartsWith(key, kGuidPrefix)) return false;

        out.append(line);

        std::string val(line.substr(eq + 1));
        TrimInPlace(val);
        if (val.empty()) return true;
        const size_t po = ImatPathOffset(key, val);
        if (po == std::string_view::npos) return true;
        const std::string path = val.substr(po);
        if (!ValueIsAssetPath(path)) return true;

        const ECS::Guid g = idx.EnsureGuid(path);
        if (!g.IsValid()) return true;
        out.append("\n");
        out.append(kGuidPrefix);
        out.append(key);
        out.append(" = ");
        out.append(g.ToHex());
        return true;
    });
}

std::string ResolveImatText(const std::string& text, const std::string& forFile,
                            std::size_t* outRepaired)
{
    if (outRepaired) *outRepaired = 0;
    if (!HasGuidTokens(text)) return text;

    // Pass 1: collect key -> guid.
    std::unordered_map<std::string, ECS::Guid> guids;
    {
        size_t pos = 0;
        while (pos < text.size())
        {
            const size_t eol     = text.find('\n', pos);
            const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol;
            const std::string_view line(text.data() + pos, lineEnd - pos);
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos)
            {
                std::string key(line.substr(0, eq));
                TrimInPlace(key);
                if (StartsWith(key, kGuidPrefix))
                {
                    std::string v(line.substr(eq + 1));
                    TrimInPlace(v);
                    const ECS::Guid g = ECS::Guid::FromHex(v);
                    if (g.IsValid()) guids[key.substr(std::strlen(kGuidPrefix))] = g;
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    }
    if (guids.empty()) return text;

    std::size_t repaired = 0;
    std::string out = ForEachLine(text, [&](std::string_view line, std::string& dst) -> bool {
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) { dst.append(line); return true; }

        std::string key(line.substr(0, eq));
        TrimInPlace(key);
        if (StartsWith(key, kGuidPrefix)) return false;   // strip

        const std::string_view rawVal = line.substr(eq + 1);
        size_t vb = 0, ve = rawVal.size();
        while (vb < ve && std::isspace(static_cast<unsigned char>(rawVal[vb])))     ++vb;
        while (ve > vb && std::isspace(static_cast<unsigned char>(rawVal[ve - 1]))) --ve;
        if (ve <= vb) { dst.append(line); return true; }

        const std::string trimmed(rawVal.substr(vb, ve - vb));
        const size_t rel = ImatPathOffset(key, trimmed);
        if (rel == std::string_view::npos) { dst.append(line); return true; }
        const size_t ps = vb + rel;
        const std::string path(rawVal.substr(ps, ve - ps));
        if (!ValueIsAssetPath(path)) { dst.append(line); return true; }

        // Guid lookup first: it is a hash probe, the existence test is a stat.
        const auto git = guids.find(key);
        if (git == guids.end() || RefTargetExists(path)) { dst.append(line); return true; }
        const std::string repl = RepairTargetForGuid(git->second);
        if (repl.empty() || repl == path) { dst.append(line); return true; }

        dst.append(line.substr(0, eq + 1));
        dst.append(rawVal.substr(0, ps));
        dst.append(repl);
        dst.append(rawVal.substr(ve));
        ++repaired;
        LOG_WARNING("AssetRefs: '%s' key '%s' -> '%s' repaired by guid to '%s'",
                    forFile.c_str(), key.c_str(), path.c_str(), repl.c_str());
        return true;
    });

    if (outRepaired) *outRepaired = repaired;
    return out;
}

// ==== .iscene / .ipfb text ====

namespace
{
    // Walk the k=v tokens of one component-line body. `fn(key, rawValue, token, out)` appends the
    // replacement token, or returns false to drop it together with the whitespace emitted before it,
    // so stripping guid tokens leaves no growing gap on each round trip. Non-k=v tokens are copied
    // through untouched.
    template <typename F>
    void ForEachToken(std::string_view body, std::string& out, F&& fn)
    {
        size_t pos = 0;
        while (pos < body.size())
        {
            const size_t mark    = out.size();
            const size_t wsStart = pos;
            while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
            out.append(body.substr(wsStart, pos - wsStart));
            if (pos >= body.size()) break;

            size_t end = pos;
            while (end < body.size() && !std::isspace(static_cast<unsigned char>(body[end]))) ++end;
            const std::string_view token = body.substr(pos, end - pos);
            pos = end;

            const size_t eq = token.find('=');
            if (eq == std::string_view::npos || eq == 0) { out.append(token); continue; }
            if (!fn(token.substr(0, eq), std::string(token.substr(eq + 1)), token, out))
                out.resize(mark);
        }
    }
}

std::string AugmentSceneText(const std::string& text)
{
    AssetGuidIndex& idx = AssetGuidIndex::Get();
    idx.EnsureBuilt();

    return ForEachLine(text, [&](std::string_view line, std::string& out) -> bool {
        std::string tag; size_t bodyStart = 0; bool skip = false;
        SplitSceneLine(line, tag, bodyStart, skip);
        if (skip) { out.append(line); return true; }
        out.append(line.substr(0, bodyStart));

        ForEachToken(line.substr(bodyStart), out,
            [&](std::string_view key, std::string rawVal,
                std::string_view token, std::string& dst) -> bool
        {
            if (StartsWith(key, kGuidPrefix)) return false;   // strip; re-derived below

            std::string val = PercentDecode(rawVal);

            if (key == "data" && tag == "MaterialInline")
            {
                dst.append(key);
                dst.push_back('=');
                dst.append(PercentEncodeStrict(AugmentImatText(val)));
                return true;
            }

            dst.append(token);

            std::string path; bool texTag = false;
            if (!SplitOverrideValue(val, path, texTag)) return true;
            if (!ValueIsAssetPath(path)) return true;
            // EnsureGuid, not GuidForPath: an asset nothing referenced yet has no sidecar, and skipping it
            // would leave the references being authored right now without a guid. Editor-only in effect:
            // EnsureGuid refuses to author into asset/ in a shipped game.
            const ECS::Guid g = idx.EnsureGuid(path);
            if (!g.IsValid()) return true;
            dst.push_back(' ');
            dst.append(kGuidPrefix);
            dst.append(key);
            dst.push_back('=');
            dst.append(g.ToHex());
            return true;
        });
        return true;
    });
}

std::string ResolveSceneText(const std::string& text, const std::string& forFile,
                             std::size_t* outRepaired)
{
    if (outRepaired) *outRepaired = 0;
    if (!HasGuidTokens(text)) return text;

    std::size_t repaired = 0;
    std::string out = ForEachLine(text, [&](std::string_view line, std::string& dst) -> bool {
        std::string tag; size_t bodyStart = 0; bool skip = false;
        SplitSceneLine(line, tag, bodyStart, skip);
        if (skip) { dst.append(line); return true; }

        const std::string_view body = line.substr(bodyStart);

        // Pass 1 over this line only: guid tokens pair with their reference by key, in any order.
        std::unordered_map<std::string, ECS::Guid> guids;
        {
            std::string sink;
            ForEachToken(body, sink, [&](std::string_view key, std::string rawVal,
                                         std::string_view, std::string&) -> bool
            {
                if (!StartsWith(key, kGuidPrefix)) return true;
                const ECS::Guid g = ECS::Guid::FromHex(PercentDecode(rawVal));
                if (g.IsValid())
                    guids[std::string(key.substr(std::strlen(kGuidPrefix)))] = g;
                return true;
            });
        }

        dst.append(line.substr(0, bodyStart));
        ForEachToken(body, dst, [&](std::string_view key, std::string rawVal,
                                    std::string_view token, std::string& out2) -> bool
        {
            if (StartsWith(key, kGuidPrefix)) return false;   // strip

            std::string val = PercentDecode(rawVal);

            if (key == "data" && tag == "MaterialInline")
            {
                std::size_t sub = 0;
                const std::string fixed = ResolveImatText(val, forFile + " (inline)", &sub);
                repaired += sub;
                out2.append(key);
                out2.push_back('=');
                out2.append(PercentEncodeStrict(fixed));
                return true;
            }

            std::string path; bool texTag = false;
            if (!SplitOverrideValue(val, path, texTag)) { out2.append(token); return true; }
            if (!ValueIsAssetPath(path)) { out2.append(token); return true; }

            // Guid lookup first: it is a hash probe, the existence test is a stat.
            const auto git = guids.find(std::string(key));
            if (git == guids.end() || RefTargetExists(path)) { out2.append(token); return true; }
            const std::string repl = RepairTargetForGuid(git->second);
            if (repl.empty() || repl == path) { out2.append(token); return true; }

            out2.append(key);
            out2.push_back('=');
            if (texTag) out2.append("t:");
            out2.append(PercentEncode(repl));
            ++repaired;
            LOG_WARNING("AssetRefs: '%s' %.*s -> '%s' repaired by guid to '%s'",
                        forFile.c_str(), static_cast<int>(key.size()), key.data(),
                        path.c_str(), repl.c_str());
            return true;
        });
        return true;
    });

    if (outRepaired) *outRepaired = repaired;
    return out;
}

// ==== Enumeration ====

namespace
{
    // Read-only line walk: fn(line, 1-based number).
    template <typename F>
    void VisitLines(const std::string& text, F&& fn)
    {
        size_t pos = 0;
        unsigned n = 0;
        while (pos < text.size())
        {
            const size_t eol     = text.find('\n', pos);
            const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol;
            fn(std::string_view(text.data() + pos, lineEnd - pos), ++n);
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    }
}

std::vector<TextRef> ListImatTextRefs(const std::string& text)
{
    // Guid lines pair with their reference by key and may come in any order.
    std::unordered_map<std::string, std::string> guids;
    VisitLines(text, [&](std::string_view line, unsigned) {
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return;
        std::string key(line.substr(0, eq));
        TrimInPlace(key);
        if (!StartsWith(key, kGuidPrefix)) return;
        std::string v(line.substr(eq + 1));
        TrimInPlace(v);
        guids[key.substr(std::strlen(kGuidPrefix))] = v;
    });

    std::vector<TextRef> refs;
    VisitLines(text, [&](std::string_view line, unsigned n) {
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return;
        std::string key(line.substr(0, eq));
        TrimInPlace(key);
        if (key.empty() || StartsWith(key, kGuidPrefix)) return;
        std::string val(line.substr(eq + 1));
        TrimInPlace(val);
        if (val.empty()) return;
        const size_t po = ImatPathOffset(key, val);
        if (po == std::string_view::npos) return;
        std::string path = val.substr(po);
        if (!ValueIsAssetPath(path)) return;
        TextRef r;
        r.key  = key;
        r.path = std::move(path);
        r.line = n;
        if (const auto it = guids.find(key); it != guids.end()) r.guidHex = it->second;
        refs.push_back(std::move(r));
    });
    return refs;
}

std::vector<TextRef> ListSceneTextRefs(const std::string& text)
{
    std::vector<TextRef> refs;
    VisitLines(text, [&](std::string_view line, unsigned n) {
        std::string tag; size_t bodyStart = 0; bool skip = false;
        SplitSceneLine(line, tag, bodyStart, skip);
        if (skip) return;
        const std::string_view body = line.substr(bodyStart);

        std::unordered_map<std::string, std::string> guids;
        std::string sink;
        ForEachToken(body, sink, [&](std::string_view key, std::string rawVal, std::string_view, std::string&) -> bool
        {
            if (StartsWith(key, kGuidPrefix))
                guids[std::string(key.substr(std::strlen(kGuidPrefix)))] = PercentDecode(rawVal);
            return true;
        });

        sink.clear();
        ForEachToken(body, sink, [&](std::string_view key, std::string rawVal, std::string_view, std::string&) -> bool
        {
            if (StartsWith(key, kGuidPrefix)) return true;
            const std::string val = PercentDecode(rawVal);
            if (key == "data" && tag == "MaterialInline")
            {
                for (TextRef r : ListImatTextRefs(val))
                {
                    r.tag            = tag;
                    r.line           = n;
                    r.inlineMaterial = true;
                    refs.push_back(std::move(r));
                }
                return true;
            }
            std::string path; bool texTag = false;
            if (!SplitOverrideValue(val, path, texTag) || !ValueIsAssetPath(path)) return true;
            TextRef r;
            r.key  = std::string(key);
            r.tag  = tag;
            r.path = std::move(path);
            r.line = n;
            if (const auto it = guids.find(r.key); it != guids.end()) r.guidHex = it->second;
            refs.push_back(std::move(r));
            return true;
        });
    });
    return refs;
}

} // namespace Resource::AssetRefs
