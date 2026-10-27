#include "Resource/AssetGuid.h"
#include "Resource/AssetTypes.generated.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"
#include "System/HostPolicy.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace Resource
{
namespace
{
    // The baked table a packed build reads instead of walking sidecars.
    constexpr const char* kBakedTablePath = "asset/asset_guids.txt";

    bool EndsWithCI(const std::string& lower, const char* ext)
    {
        const size_t n = std::strlen(ext);
        return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
    }

    // Forward slashes, original case: the form stored in m_byGuid, because that value
    // is written straight back into containers and into AssetFS lookups, and a pak key
    // is always forward-slashed (the directory walk returns native separators, and
    // "asset\Foo\bar.itex" resolves in no packed build). Unlike
    // AssetRefs::NormalizeRefPath, which also lowercases and is safe only to compare with.
    std::string ToStoredForm(const std::string& path)
    {
        std::string s = path;
        for (char& c : s) if (c == '\\') c = '/';
        while (s.rfind("./", 0) == 0) s.erase(0, 2);
        return s;
    }
}

std::string MetaPathFor(const std::string& assetPath)
{
    return assetPath + ".meta";
}

// The AssetTypes rows with guid (src/Resoruce/AssetTypes.json): assets something can reference.
// .lua and .hlsl have none on purpose: scripts and shaders are authored by path, and a guid in a
// script string would be unreadable.
bool IsGuidTrackedAsset(const std::string& path)
{
    const AssetTypes::Row* row = AssetTypes::FindByPath(AssetRefs::ToLowerAscii(path));
    return row && row->guid;
}

ECS::Guid ParseMetaGuid(const std::string& metaText)
{
    size_t pos = 0;
    while (pos < metaText.size())
    {
        const size_t eol     = metaText.find('\n', pos);
        const size_t lineEnd = (eol == std::string::npos) ? metaText.size() : eol;
        std::string line = metaText.substr(pos, lineEnd - pos);
        if (eol == std::string::npos) pos = metaText.size(); else pos = eol + 1;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        const auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        };
        trim(key); trim(val);
        if (key != "guid") continue;
        // Accept both spellings: ToHex is what we write, but a hand-pasted
        // canonical UUID should not silently become "no identity".
        ECS::Guid g = ECS::Guid::FromHex(val);
        if (!g.IsValid()) g = ECS::Guid::Parse(val);
        return g;
    }
    return ECS::kInvalidGuid;
}

std::string BuildMetaText(const ECS::Guid& g)
{
    return "# DX12 Engine asset meta. Commit this file alongside its asset.\n"
           "# The guid is this asset's identity — references survive a rename\n"
           "# through it. Never edit or copy it by hand.\n"
           "guid = " + g.ToHex() + "\n";
}

// ==== AssetGuidIndex ====

AssetGuidIndex& AssetGuidIndex::Get()
{
    static AssetGuidIndex s_instance;
    return s_instance;
}

bool AssetGuidIndex::Built() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_built;
}

std::size_t AssetGuidIndex::Count() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_byGuid.size();
}

std::size_t AssetGuidIndex::DuplicateCount() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_duplicates;
}

double AssetGuidIndex::LastScanMs() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_lastScanMs;
}

bool AssetGuidIndex::FromBakedTable() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_baked;
}

void AssetGuidIndex::Invalidate()
{
    AssetRefs::ForgetExistenceCache();
    std::lock_guard<std::mutex> lk(m_mutex);
    m_byGuid.clear();
    m_byPath.clear();
    m_duplicates = 0;
    m_built      = false;
    m_baked      = false;
}

void AssetGuidIndex::EnsureBuilt(const std::string& assetRoot)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_built) return;
    }
    // Stamping is the editor's job and happens on its own eager Rebuild; a
    // lazy build triggered from a load path must never write into asset/.
    Rebuild(assetRoot, /*stampMissing=*/false);
}

void AssetGuidIndex::InsertLocked(const ECS::Guid& g, const std::string& rawPath)
{
    const std::string path = ToStoredForm(rawPath);
    const std::string norm = AssetRefs::NormalizeRefPath(path);
    const auto it = m_byGuid.find(g);
    if (it != m_byGuid.end() && AssetRefs::NormalizeRefPath(it->second) != norm)
    {
        ++m_duplicates;
        LOG_ERROR("AssetGuidIndex: guid %s claimed by BOTH '%s' and '%s'. The "
                  "second was almost certainly created by copying a .meta — "
                  "delete one sidecar and let it be re-stamped.",
                  g.ToHex().c_str(), it->second.c_str(), path.c_str());
        return;   // first one enumerated keeps the guid
    }
    m_byGuid[g]    = path;
    m_byPath[norm] = g;
}

bool AssetGuidIndex::LoadBakedTable()
{
    if (!AssetFS::Get().IsMounted()) return false;
    std::string table;
    if (!AssetFS::Get().ReadFileText(kBakedTablePath, table)) return false;

    size_t pos = 0, rows = 0;
    while (pos < table.size())
    {
        const size_t eol     = table.find('\n', pos);
        const size_t lineEnd = (eol == std::string::npos) ? table.size() : eol;
        std::string line = table.substr(pos, lineEnd - pos);
        if (eol == std::string::npos) pos = table.size(); else pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const ECS::Guid g = ECS::Guid::FromHex(line.substr(0, tab));
        if (!g.IsValid()) continue;
        InsertLocked(g, line.substr(tab + 1));
        ++rows;
    }
    m_baked = true;
    LOG_INFO("AssetGuidIndex: loaded %zu identities from the baked pak table", rows);
    return rows > 0;
}

void AssetGuidIndex::WalkAndLoad(const std::string& assetRoot, bool stampMissing)
{
    const std::string root = assetRoot.empty() ? std::string("asset") : assetRoot;
    std::error_code ec;
    std::size_t stamped = 0;

    for (fs::recursive_directory_iterator
             it(Sys::U8Path(root), fs::directory_options::skip_permission_denied, ec), endIt;
         it != endIt; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = Sys::PathToUtf8(it->path());
        if (!IsGuidTrackedAsset(rel)) continue;

        const std::string metaPath = MetaPathFor(rel);
        std::vector<uint8_t> bytes;
        ECS::Guid g = ECS::kInvalidGuid;
        if (Sys::ReadFileBytes(metaPath, bytes))
            g = ParseMetaGuid(std::string(reinterpret_cast<const char*>(bytes.data()),
                                          bytes.size()));

        if (!g.IsValid())
        {
            if (!stampMissing) continue;
            g = ECS::Guid::Generate();
            const std::string meta = BuildMetaText(g);
            if (!Sys::WriteFileBytes(metaPath, meta.data(), meta.size()))
            {
                LOG_WARNING("AssetGuidIndex: cannot write sidecar '%s'", metaPath.c_str());
                continue;
            }
            ++stamped;
        }
        InsertLocked(g, rel);
    }

    if (stamped)
        LOG_SUCCESS("AssetGuidIndex: stamped %zu new asset identities", stamped);
}

// A worktree sharing the main checkout's binary assets (Resource/SharedContent.h).
// Their .meta sidecars are tracked text, so they are HERE - but the walk above
// starts from the asset files, which are not; and an untracked sidecar
// (engine/**/*.meta) exists only in the main checkout. So: every sidecar of the
// union tree whose asset resolves (here or there) and is not yet known gets its
// identity, read from this checkout's sidecar when it has one. Never stamps:
// minting belongs to the checkout that holds the asset.
void AssetGuidIndex::WalkSharedLocked()
{
    const AssetFS& afs = AssetFS::Get();
    if (afs.SharedRootDir().empty()) return;
    std::size_t added = 0;
    for (const char* top : { "asset", "engine" })
    {
        std::vector<std::string> files;
        afs.EnumerateLoose(top, files);
        for (const std::string& rel : files)
        {
            const std::string lower = AssetRefs::ToLowerAscii(rel);
            if (!EndsWithCI(lower, ".meta")) continue;
            const std::string asset = rel.substr(0, rel.size() - 5);
            if (!IsGuidTrackedAsset(asset)) continue;
            if (m_byPath.count(AssetRefs::NormalizeRefPath(ToStoredForm(asset)))) continue;
            if (!afs.ExistsExact(asset)) continue;
            std::vector<uint8_t> bytes;
            if (!afs.ReadFile(rel, bytes)) continue;
            const ECS::Guid g = ParseMetaGuid(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            if (!g.IsValid()) continue;
            InsertLocked(g, asset);
            ++added;
        }
    }
    LOG_INFO("AssetGuidIndex: %zu identities of assets read from the shared root", added);
}

void AssetGuidIndex::SetContentRoots(std::vector<std::string> dirs)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_contentRoots = std::move(dirs);
    }
    Invalidate();   // the index describes the previous set of directories
}

void AssetGuidIndex::Rebuild(const std::string& assetRoot, bool stampMissing)
{
    const auto t0 = std::chrono::steady_clock::now();
    // A rebuild means "the tree may have moved", which is exactly when a
    // memoized "this path exists" turns into a silently skipped repair.
    AssetRefs::ForgetExistenceCache();
    std::lock_guard<std::mutex> lk(m_mutex);

    m_byGuid.clear();
    m_byPath.clear();
    m_duplicates = 0;
    m_baked      = false;

    // A packed build has frozen paths and ships no sidecars; the cook bakes the table
    // instead. A pak without the table falls through to the walk, so it still loads
    // (with no identities).
    if (!LoadBakedTable())
    {
        if (m_contentRoots.empty())
        {
            WalkAndLoad(assetRoot, stampMissing);
        }
        else
        {
            // Priority order: the first root to claim a guid keeps it, which is
            // what InsertLocked already does for duplicates.
            for (const std::string& dir : m_contentRoots)
                WalkAndLoad(dir, stampMissing);
        }
        WalkSharedLocked();
    }

    m_built      = true;
    m_lastScanMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("AssetGuidIndex: %zu identities, %zu duplicate(s) (%.0f ms)",
             m_byGuid.size(), m_duplicates, m_lastScanMs);
}

std::string AssetGuidIndex::PathForGuid(const ECS::Guid& g) const
{
    if (!g.IsValid()) return {};
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_byGuid.find(g);
    return it != m_byGuid.end() ? it->second : std::string{};
}

ECS::Guid AssetGuidIndex::GuidForPath(const std::string& path) const
{
    if (path.empty()) return ECS::kInvalidGuid;
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_byPath.find(AssetRefs::NormalizeRefPath(path));
    return it != m_byPath.end() ? it->second : ECS::kInvalidGuid;
}

ECS::Guid AssetGuidIndex::EnsureGuid(const std::string& path)
{
    if (path.empty() || !IsGuidTrackedAsset(path)) return ECS::kInvalidGuid;

    if (const ECS::Guid existing = GuidForPath(path); existing.IsValid())
        return existing;

    // The asset has to exist. Augmentation runs over whatever a container names,
    // dangling references included; stamping one would litter the tree with asset-less
    // sidecars and, worse, mint an identity mapping the dangling path to itself, so
    // resolution would later "repair" a broken reference into a differently spelled
    // broken one and report it fixed. A path that names nothing stays visibly broken.
    // ExistsExact: a path that exists only after relocation is stale, and minting on it
    // would write path + ".meta" at the old spelling - beside a directory that is not
    // there, and unwritable in a packed build. Exact across every root: a worktree
    // sharing the main checkout's binary assets has the asset there, not here.
    if (!AssetFS::Get().ExistsExact(path))
        return ECS::kInvalidGuid;

    // A sidecar may exist unindexed (imported after the last rebuild): read it before
    // generating, or a re-import would orphan every reference to the existing identity.
    // Through AssetFS: an untracked sidecar (engine/**/*.meta) may exist only in the
    // shared root.
    const std::string metaPath = MetaPathFor(path);
    std::vector<uint8_t> bytes;
    ECS::Guid g = ECS::kInvalidGuid;
    if (AssetFS::Get().ReadFile(metaPath, bytes))
        g = ParseMetaGuid(std::string(reinterpret_cast<const char*>(bytes.data()),
                                      bytes.size()));

    if (!g.IsValid())
    {
        if (!Sys::IsEditorHost())
        {
            // Once: a shipped game hitting this for one asset hits it for
            // hundreds, and the actionable fact is the same every time.
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                LOG_WARNING("AssetGuidIndex: '%s' has no identity and a shipped "
                            "game must not author into asset/ — reference repair "
                            "is unavailable for it (and any other unstamped "
                            "asset). Run \"Stamp all asset identities\" in the "
                            "editor and re-cook.", path.c_str());
            }
            return ECS::kInvalidGuid;
        }
        g = ECS::Guid::Generate();
        const std::string meta = BuildMetaText(g);
        if (!Sys::WriteFileBytes(metaPath, meta.data(), meta.size()))
        {
            LOG_WARNING("AssetGuidIndex: cannot write sidecar '%s'", metaPath.c_str());
            return ECS::kInvalidGuid;
        }
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    InsertLocked(g, path);
    return g;
}

void AssetGuidIndex::Repoint(const std::string& oldPath, const std::string& newPath)
{
    if (oldPath.empty() || newPath.empty()) return;
    AssetRefs::ForgetExistenceCache();

    // The sidecar is supposed to travel with the asset; when the rename went
    // through the editor it has not, so move it here.
    const std::string oldMeta = MetaPathFor(oldPath);
    const std::string newMeta = MetaPathFor(newPath);
    std::error_code ec;
    if (fs::exists(Sys::U8Path(oldMeta), ec) && !fs::exists(Sys::U8Path(newMeta), ec))
        fs::rename(Sys::U8Path(oldMeta), Sys::U8Path(newMeta), ec);

    std::lock_guard<std::mutex> lk(m_mutex);
    const std::string oldNorm = AssetRefs::NormalizeRefPath(oldPath);
    const auto it = m_byPath.find(oldNorm);
    if (it == m_byPath.end()) return;
    const ECS::Guid g = it->second;
    m_byPath.erase(it);
    m_byGuid[g] = ToStoredForm(newPath);
    m_byPath[AssetRefs::NormalizeRefPath(newPath)] = g;
}

std::string GuidHexForPath(const std::string& assetPath)
{
    if (assetPath.empty()) return {};
    AssetGuidIndex& idx = AssetGuidIndex::Get();
    idx.EnsureBuilt();
    const ECS::Guid g = idx.GuidForPath(assetPath);
    return g.IsValid() ? g.ToHex() : std::string{};
}

std::string ResolveStoredPath(const std::string& path, const std::string& hexGuid,
                              const char* whatForLog)
{
    if (path.empty()) return path;
    // ExistsExact, not a relocation-aware test: that would call a stale path healthy,
    // return it unchanged and skip the guid rescue below - the one thing that rewrites
    // the container to the current spelling - so the asset would keep loading
    // (ReadFile relocates) and never be healed. Exact across every root, the shared one
    // included, so a shared asset is not called gone and the first reference does not
    // build the index.
    if (AssetFS::Get().ExistsExact(path))
        return path;   // healthy: no index build, no lookup

    const ECS::Guid g = ECS::Guid::FromHex(hexGuid);
    if (!g.IsValid()) return path;

    AssetGuidIndex& idx = AssetGuidIndex::Get();
    idx.EnsureBuilt();
    const std::string repl = idx.PathForGuid(g);
    if (repl.empty() || repl == path) return path;

    LOG_WARNING("AssetGuid: stored %s '%s' no longer exists; guid %s resolved it "
                "to '%s'", whatForLog ? whatForLog : "reference", path.c_str(),
                hexGuid.c_str(), repl.c_str());
    return repl;
}

void AssetGuidIndex::Forget(const std::string& path)
{
    if (path.empty()) return;
    AssetRefs::ForgetExistenceCache();
    std::error_code ec;
    fs::remove(Sys::U8Path(MetaPathFor(path)), ec);

    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_byPath.find(AssetRefs::NormalizeRefPath(path));
    if (it == m_byPath.end()) return;
    m_byGuid.erase(it->second);
    m_byPath.erase(it);
}

} // namespace Resource
