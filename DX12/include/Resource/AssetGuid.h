#pragma once
// AssetGuidIndex - stable 128-bit identity for asset files.
//
// The authority is the sidecar, not this map: each tracked asset owns a
// `<file>.meta` beside it holding one guid, which travels with the file through
// `git mv`, an Explorer drag or a merge, and must be committed. This index is a
// rebuildable cache of those sidecars - losing it costs a rescan. Do not turn it
// into a central guid -> path database as the authority: one bad merge or one
// uncommitted file would break every reference at once, with no way to rebuild them.
//
// Built lazily, on the first resolution that misses (path-first resolution means a
// healthy project never misses, so a shipped game normally never builds it): with a
// pak mounted, one read of the baked `asset/asset_guids.txt` (tools/pack_assets.py);
// with loose files, one recursive walk of the sidecars. The editor builds it eagerly
// during its asset scan with stamping on, so new imports get an identity at once.
//
// Stamping is editor-only: a shipped game must never author into asset/.

#include "ECS/Guid.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    // "asset/Foo/bar.itex" -> "asset/Foo/bar.itex.meta". The extension is kept
    // so two assets differing only by extension get distinct sidecars.
    std::string MetaPathFor(const std::string& assetPath);

    // True when `path`'s extension is one the identity layer covers. Sidecars
    // are only written for assets something can actually reference.
    bool IsGuidTrackedAsset(const std::string& path);

    // Parse a `.meta` blob. Returns kInvalidGuid when absent or malformed.
    ECS::Guid ParseMetaGuid(const std::string& metaText);
    // The canonical sidecar body for `g`.
    std::string BuildMetaText(const ECS::Guid& g);

    // ---- Stored references outside the container formats ----
    // Save files and game.json hold asset paths as plain strings, outside the
    // .iscene / .imat text AssetRefs augments; these two functions let such a store
    // carry an identity next to its path.

    // The target's guid as 32-char hex, or "" when it has none. Builds the index if
    // needed: call it only while writing, never on a hot path.
    std::string GuidHexForPath(const std::string& assetPath);

    // Path first: returns `path` untouched when it still resolves, which is the
    // healthy case and costs one existence check with no index build. Only a
    // MISS consults `hexGuid`. `whatForLog` names the thing ("scene", "prefab")
    // in the repair warning.
    std::string ResolveStoredPath(const std::string& path,
                                  const std::string& hexGuid,
                                  const char* whatForLog);

    class AssetGuidIndex
    {
    public:
        static AssetGuidIndex& Get();

        // Builds if not built yet. `assetRoot` is used only by the loose-file walk. Safe
        // from any thread and on a hot path: it returns at once when built.
        void EnsureBuilt(const std::string& assetRoot = "asset");

        // Forces a (re)build. `stampMissing` (editor only) writes a fresh sidecar for every
        // tracked asset that lacks one.
        void Rebuild(const std::string& assetRoot, bool stampMissing);

        // The content directories to walk, in priority order matching AssetFS's search
        // roots: the first root to claim a guid keeps it. An index that walked only
        // "asset" would lose every engine identity, and the repair path would then "fix"
        // a good engine reference into an empty string.
        void SetContentRoots(std::vector<std::string> dirs);
        const std::vector<std::string>& ContentRoots() const { return m_contentRoots; }

        // Drop the index so the next EnsureBuilt rebuilds it.
        void Invalidate();

        // guid -> current project-relative path. Empty when unknown.
        std::string PathForGuid(const ECS::Guid& g) const;
        // path -> guid. kInvalidGuid when untracked or unstamped.
        ECS::Guid   GuidForPath(const std::string& path) const;

        // GuidForPath, writing a sidecar when there is none. Editor-only;
        // returns kInvalidGuid in a shipped game (and logs once).
        ECS::Guid   EnsureGuid(const std::string& path);

        // Move an entry after a rename, without a full rescan. Also moves the
        // sidecar file itself when it did not travel with the asset.
        void Repoint(const std::string& oldPath, const std::string& newPath);

        // Forget one asset (deleted). The sidecar is removed too.
        void Forget(const std::string& path);

        bool        Built() const;
        std::size_t Count() const;
        // Sidecars that carried a guid already claimed by another file. In the
        // editor these are re-stamped (the later file loses); in a game they
        // are only reported, because the fix belongs in source control.
        std::size_t DuplicateCount() const;
        double      LastScanMs() const;
        bool        FromBakedTable() const;

    private:
        AssetGuidIndex() = default;

        // Reads `asset/asset_guids.txt` out of a mounted pak. False when no pak is
        // mounted or the pak was cooked without the table.
        bool LoadBakedTable();
        void WalkAndLoad(const std::string& assetRoot, bool stampMissing);
        void WalkSharedLocked();   // identities of assets read from the shared root
        // Caller must hold m_mutex.
        void InsertLocked(const ECS::Guid& g, const std::string& path);

        mutable std::mutex m_mutex;
        // guid -> path AS ENUMERATED (authored case preserved: this value is
        // written back into containers).
        std::unordered_map<ECS::Guid, std::string> m_byGuid;
        // NormalizeRefPath(path) -> guid.
        std::unordered_map<std::string, ECS::Guid> m_byPath;
        // Empty = walk the single root Rebuild is handed.
        std::vector<std::string> m_contentRoots;
        std::size_t m_duplicates = 0;
        double      m_lastScanMs = 0.0;
        bool        m_built      = false;
        bool        m_baked      = false;
    };
}
