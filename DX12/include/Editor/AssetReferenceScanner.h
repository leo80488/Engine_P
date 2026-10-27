#pragma once
// Reverse reference index over the asset tree: answers "who uses this asset?". References are
// path strings, so a rename or delete strands every referrer unless they are repointed; this index
// turns "afraid to reorganize the asset directory" into "check, then act".
// RewriteReferences / RewriteReferencesBatch repoint every indexed reference, so an in-editor
// rename or move carries its referrers. A move outside the editor (git mv, Explorer, a merge) is
// recoverable only through the guid token a reference carries (BackfillGuids).
// Scanned: the AssetHeader containers wrapping a NUL-terminated text payload (.iscene =
// MAGIC_WORLD, .ipfb = MAGIC_PREFAB, .imat / .imatinst = MAGIC_MATERIAL, .iscn = MAGIC_SCENE; a
// 16-byte metadata block each, payload at offset 40) and the JSON assets (.ianimgraph, .ianimset,
// .attack.json, .attackset.json, .imatgraph, .imatfunc, .iui). A model's (.iscn) F / K / M lines
// name files relative to its folder in a positional grammar, so it is read with the loader's own
// SceneInstanceLoader::ListFileReferences, rewritten with relative paths, and re-based when the
// model itself moves.
// References are recognized by value (Resource::AssetRefs: a known engine extension plus a path
// separator or asset/ prefix), not by a key whitelist: serializer keys drift, extensions do not.
// Special cases: MaterialOverride's `t:` prefix, .imat's two-token `ctex_` values, and
// MaterialInline's percent-encoded embedded .imat text (decoded and re-scanned).
// Editor-only (EditorCore). Synchronous rebuild with an mtime + size per-file cache: ~1.3k files
// cold in well under a second, near-instant warm.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Editor
{

class AssetReferenceScanner
{
public:
    struct Referrer
    {
        std::string file;   // referring container, project-relative
        std::string key;    // serializer key that carried the reference
    };

    struct RewriteReport
    {
        size_t containersChanged   = 0;   // containers actually rewritten on disk
        size_t referencesRewritten = 0;   // individual values repointed
        // Containers the index said hold a reference but that could not be rewritten (unreadable,
        // pak-only, or the value no longer matched). Non-empty: the caller must not report a clean rename.
        std::vector<std::string> failed;
    };

    struct BackfillReport
    {
        size_t containersScanned  = 0;
        size_t containersChanged  = 0;
        size_t referencesStamped  = 0;
        std::vector<std::string> failed;
    };

    // (Re)build the index over `assetRoot` (recursive). Unchanged files
    // (mtime+size) keep their cached rows; changed/new files re-parse.
    void Rebuild(const std::string& assetRoot);

    // Extra directories to index besides the one passed to Rebuild. Engine
    // content has containers of its own (the UI widget kit is .ipfb, the base
    // scene is .iscene), and a "who references this?" answer that cannot see
    // them reports an engine asset as unreferenced right before you delete it.
    void SetExtraRoots(std::vector<std::string> dirs) { m_extraRoots = std::move(dirs); }

    // Every container that references @p assetPath, in any separator style, case or
    // percent-encoding (normalized like the index keys).
    std::vector<Referrer> FindReferrers(const std::string& assetPath) const;

    // Repoints every indexed reference to @p oldPath at @p newPath, in place. Only indexed containers
    // are touched and an unbuilt index returns an empty report, so Rebuild() first, ideally right
    // before: a stale index misses files that began referencing oldPath after the scan.
    // Each container is re-emitted through TransformContainer so both length fields stay consistent; a
    // byte search-and-replace corrupts them once the path length changes.
    // Leaves the index stale (Built() == false): Rebuild() before querying.
    RewriteReport RewriteReferences(const std::string& oldPath,
                                    const std::string& newPath);

    // Migration: stamps every reference in every container with its target's guid, so a move made
    // outside the editor stays recoverable. Otherwise a reference gains its guid only when its
    // container is re-saved (Resource::AssetRefs), and stamping the .meta sidecars alone is not enough:
    // the reference has to carry the identity. Rebuilds both indexes first (stamping missing
    // sidecars), rewrites only containers whose text changes, and is idempotent. JSON assets and models
    // (.iscn) are skipped.
    BackfillReport BackfillGuids(const std::string& assetRoot);

    // Literal prefix substitution across every container under @p assetRoot. RewriteReferences /
    // Batch match normalized paths, so they cannot repair a reference whose stored spelling never
    // normalizes to the indexed one, such as
    //     asset/EngineResource\GirlsFrontline%20BastiDefault%201.1\x.imsh
    // (mixed separators and percent-encoding in one token). This rewrites only the prefix bytes and
    // leaves the rest of the token (separators, encoding, case) as it was; TransformContainer patches
    // both length fields. Models (.iscn) are skipped.
    // @p oldPrefix must be specific enough that no unrelated value starts with it: pass a full
    // directory prefix, never a bare word.
    RewriteReport RewriteRawPrefix(const std::string& assetRoot,
                                   const std::string& oldPrefix,
                                   const std::string& newPrefix);

    // Repoints many assets in one pass (a folder move, a multi-asset drag); @p moves holds
    // (oldPath, newPath) pairs. Not a loop over RewriteReferences, which leaves the index stale so the
    // second call does nothing: this rebuilds once, rewrites each affected container once, and re-bases
    // every moved model's own references.
    RewriteReport RewriteReferencesBatch(
        const std::vector<std::pair<std::string, std::string>>& moves);

    bool     Built() const       { return m_built; }
    size_t   FileCount() const   { return m_files.size(); }
    size_t   EdgeCount() const   { return m_edges; }
    double   LastScanMs() const  { return m_lastScanMs; }

    // Index-key normalization: percent-decode, '\' -> '/', lowercase, lexically_normal. Public so UI
    // code can show what it queried.
    static std::string NormalizePath(const std::string& raw);

private:
    struct FileEntry
    {
        int64_t  mtime = 0;    // fs::last_write_time ticks
        uint64_t size  = 0;
        // As enumerated, before NormalizePath lowercased it. The map key is
        // only good for lookups; writes need the on-disk spelling.
        std::string diskPath;
        // (normalized referenced path, key) pairs found in this file.
        std::vector<std::pair<std::string, std::string>> refs;
    };

    // Which grammar a container's text is in, passed to every transform: the .imat `key = value`
    // rules, the .iscn model grammar (positional F / K / M lines with paths relative to the file's
    // folder), or the .iscene / .ipfb component lines.
    struct TextKind
    {
        bool        imatRules = false;
        bool        iscnRules = false;
        std::string diskPath;          // the container itself (an .iscn resolves against it)
    };

    void ParseContainer(const std::string& relPath, FileEntry& entry);

    // Container surgery shared by every in-place edit: reads the file, hands the text payload and its
    // TextKind to @p transform, and re-emits with AssetHeader::dataSize and the metadata textLength
    // both matching (either one wrong silently truncates the payload); JSON assets are written back as
    // text. Writes via .tmp + rename so an interrupted write cannot truncate an asset. @p transform
    // returns how many values it changed; zero leaves the file untouched. Returns false when the file
    // cannot be read, parsed or written.
    static bool TransformContainer(
        const std::string& diskPath,
        const std::function<size_t(std::string& text, const TextKind& kind)>& transform,
        size_t& outHits);

    // Repoint one container's references. `oldNorm` is already normalized;
    // `newValue` is written verbatim (project-relative, forward slashes).
    static bool RewriteContainer(const std::string& diskPath,
                                 const std::string& oldNorm,
                                 const std::string& newValue,
                                 size_t& outHits);

    std::unordered_map<std::string, FileEntry> m_files;  // key: normalized container path
    // normalized referenced path -> referrers (rebuilt from m_files on each Rebuild)
    std::unordered_map<std::string, std::vector<Referrer>> m_index;
    // The root the index was last built over: RewriteReferencesBatch must rebuild before it acts,
    // without asking the caller for the root again.
    std::string m_assetRoot;
    std::vector<std::string> m_extraRoots;
    size_t m_edges     = 0;
    bool   m_built     = false;
    double m_lastScanMs = 0.0;
};

} // namespace Editor
