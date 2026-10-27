#pragma once

// MaterialSerializer - save/load MaterialComponent as the .imat / .imatinst asset.
//
// Blob layout (same as MaterialImporter output):
//   [AssetHeader (24 B)] [MaterialMetadata (16 B)] [key=value text payload (null-terminated)]
//
// The text holds meta / flag keys (shaderType, blendMode, domain, castShadow, outline, ...),
// one line per MaterialSchema row (colors as "r g b a"), texTiling / texOffset ("u v"),
// tex_<SLOT>, the custom-shader bag (cparam_ / ctex_ / sswitch_) and the format markers
// matfmt, imatfmt and cparamFmt; an instance adds `parent` and `unset` lines. BuildTextImpl in
// the .cpp is the one writer. A tex_ key that names no slot (tex_METALLICMAP, tex_ROUGHNESSMAP,
// tex_OCCLUSIONMAP) is ignored on load: those channels are packed in tex_SURFACEMAP
// (AO / roughness / metalness).

#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <cstddef>
#include <utility>

struct MaterialComponent;

namespace Resource
{
    // Strict authoring check: validates every requested key / value in @p values on a copy of
    // @p current before any editor history, live material or file changes; on failure sets
    // @p error and returns false. LoadMaterial and the text appliers stay permissive.
    bool ValidateMaterialEdit(const MaterialComponent& current,
        const std::vector<std::pair<std::string, std::string>>& values, std::string& error);
    // What a material FILE authored, as opposed to what its resolved MaterialComponent holds.
    // After the parent merge an overridden value and an inherited one are the same bytes, so
    // re-saving an instance or showing "overridden vs inherited" needs this record.
    struct MaterialLoadInfo
    {
        // "" when the file is a standalone master.
        std::string parentPath;

        // Keys the LEAF file authored: schema names ("roughnessMin"), meta keys ("blendMode",
        // "domain"), "tex_<SLOT>", "cparam_<Name>", "ctex_<Name>", "sswitch_<Name>". Passed back
        // into SaveMaterialInstance it reproduces the file exactly, so re-saving is idempotent.
        std::unordered_set<std::string> overrideKeys;

        // `unset = <key>` lines: inherited custom params / textures / static switches removed outright.
        std::vector<std::string> unsetKeys;

        // The file carried `imatfmt = 2`: its keys are an authored set. False for a value-diff
        // instance, whose key set only approximates what the author meant (see CollectAuthoredKeys).
        bool explicitFormat = false;

        // False when some link in the parent chain could not be resolved. The material still
        // loaded (deltas applied onto default state), so the caller must NOT save over it as if
        // it were a normal material.
        bool parentResolved = true;
    };
    // Process-local cache diagnostics. A cache entry is one fully-resolved,
    // authored .imat/.imatinst snapshot; callers still receive their own
    // MaterialComponent copy, so runtime texture handles and per-entity edits
    // are never shared through the cache.
    struct MaterialCacheStats
    {
        std::uint64_t hits          = 0;
        std::uint64_t misses        = 0;
        std::uint64_t invalidations = 0;
        std::size_t   entries       = 0;
    };

    // Write @p mat to @p path as a STANDALONE material (every key, no `parent` line).
    // Returns true on success.
    // Refused (logs an error, returns false) when @p path currently holds an INSTANCE, unless
    // @p allowFlattenInstance is true: a standalone dump over an .imatinst severs the parent
    // link permanently and silently while the file keeps rendering. Pass true only for a
    // deliberate "break instance link".
    bool SaveMaterial(const MaterialComponent& mat, const std::string& path,
                      bool allowFlattenInstance = false);

    // Read the .imat or .imatinst at @p path into @p mat: an instance resolves its parent chain
    // first and merges its deltas on top. Marks mat dirty. Returns true on success, also when
    // the parent chain is broken (see MaterialLoadInfo::parentResolved).
    // @p out (optional) receives what the file itself authored. Anything that will later
    // RE-SAVE this material as an instance must keep it, or the re-save has no override set
    // and falls back to the value diff.
    bool LoadMaterial(const std::string& path, MaterialComponent& mat,
                      MaterialLoadInfo* out = nullptr);

    // Drop every cached snapshot and the reverse dependency index. Saves use
    // InvalidateMaterialsDependingOn instead; this hook is for tools that replace material
    // assets outside the serializers or remount a package.
    void ClearMaterialCache();
    MaterialCacheStats GetMaterialCacheStats();

    // Drop only the cached materials that READ @p changedPath: the file itself and every
    // instance inheriting from it, at any depth. Returns how many entries were dropped.
    // SaveMaterial / SaveMaterialInstance call this: unrelated entries stay cached and the
    // reverse index stays intact for the hot-reload lookup that follows the save.
    std::size_t InvalidateMaterialsDependingOn(const std::string& changedPath);

    // Monotonic counter of "some material file's content changed", bumped only by
    // ClearMaterialCache and InvalidateMaterialsDependingOn (every save and every live-overlay
    // change goes through the latter).
    // A resolved MaterialComponent is a value: nothing tells its holder the .imat was rewritten.
    // Entities are re-applied by ReloadMaterialsAffectedBy, but the asset preview holds its
    // parts' materials outside the ECS, so it folds this number into its extract stamp and
    // re-resolves on the next frame after a save. Lock-free: a stale read costs one late frame,
    // never a wrong picture, because the value only increases.
    std::uint64_t MaterialContentGeneration();

    // Every material whose resolved snapshot read @p changedPath, including every instance
    // inheriting from it at any depth: the scoping primitive for hot reload. Reloading every
    // material instead calls SetDirty() on each, and the renderer's retained-instance gate needs
    // m_sceneMatDirty empty, so a whole-world reload switches off the candidate cache and the
    // DDGI/RT retained gather for that frame.
    // Answers from the reverse index: every material whose chain fully resolved since the last
    // ClearMaterialCache. Paths come back canonical (forward slashes, project-root-relative).
    std::vector<std::string> MaterialsDependingOn(const std::string& changedPath);

    // @p path spelled the way the cache and the reverse index key it, when the two differ only
    // in letter case (Windows names one file either way; the asset watcher reports LOWERCASED
    // paths). @p path canonicalized when neither knows it in any case.
    std::string MaterialIndexSpelling(const std::string& path);

    // Whether the material cache holds a resolved copy of @p path (in any
    // letter case) - diagnostics: the Lua `Asset` table.
    bool IsMaterialCached(const std::string& path);

    // ---- Material instances (.imatinst) ----
    // A material INSTANCE references a parent material (an .imat or another .imatinst) and
    // stores ONLY the parameters it overrides, so many instances share one master and edits to
    // the master propagate. The file reuses the .imat container (MAGIC_MATERIAL) plus a leading
    // `parent = <path>` key; LoadMaterial resolves it transparently.

    // Save @p mat as an .imatinst at @p path inheriting from @p parentPath. Returns false when
    // the parent chain does not fully resolve, the file cannot be written, or @p parentPath is
    // @p path or has it among its ancestors (either would load as a default white material).
    // @p overrideKeys is the EXPLICIT override set (see MaterialLoadInfo): exactly those keys
    // are written and the file is stamped `imatfmt = 2`. A key is an override because the
    // author marked it, not because its value differs today, so a value pinned to what the
    // parent holds now survives a later parent edit, and a custom-shader instance does not pin
    // the ~20 reflected parameters nobody touched.
    // @p unsetKeys (explicit mode only) removes inherited `cparam_` / `ctex_` / `sswitch_`
    // entries; there is no other way to express removal.
    // @p overrideKeys nullptr: value-diff mode, writing the keys whose value differs from the
    // resolved parent, plus `domain` always.
    bool SaveMaterialInstance(const std::string& path, const std::string& parentPath,
                              const MaterialComponent& mat,
                              const std::unordered_set<std::string>* overrideKeys = nullptr,
                              const std::vector<std::string>*        unsetKeys    = nullptr);

    // Returns the parent material path of an instance file (forward-slashed), or
    // "" if @p path is a standalone material (or unreadable). Lets the editor
    // detect instances and show the inherited-vs-overridden state.
    std::string GetMaterialParent(const std::string& path);

    // Build an .imat binary blob in memory (for BuildBlobs pipelines); the caller appends the
    // returned bytes to its file list.
    std::vector<uint8_t> BuildMaterialBlob(const MaterialComponent& mat);

    // The file bytes @p in (read from @p path) re-encoded at CurrentVersion(MAGIC_MATERIAL) into @p out,
    // every header field but the version kept (flags, reserved, resource type, the metadata's other
    // words). A file the migration chain leaves unchanged gets only its version number; one the chain
    // changes (a text that predates matfmt 3) is written through the material writer after its fold,
    // and the result is read back and compared with the original. False with @p why for a file it
    // cannot lift: a bad header, an instance whose merge with its parent the fold would change (an
    // instance the fold leaves alone gets only its number), a round trip that differs. The upgrade command of the asset tool (Harness/AssetToolUpgrade.h) calls it.
    bool UpgradeMaterialBlob(const std::string& path, const std::vector<uint8_t>& in,
                             std::vector<uint8_t>& out, std::string& why);

    // ---- In-memory text round-trip (inline scene serialization) ----
    // Entities WITHOUT a MaterialSourcePath (.imat ref) persist their material INLINE in the
    // .iscn through these: the same key=value text as the .imat payload, one format
    // (BuildTextImpl / ApplyMaterialText).
    std::string BuildMaterialText(const MaterialComponent& mat);
    // Full (clearBag) apply of a key=value text onto mat; marks it dirty.
    void ApplyMaterialTextInPlace(MaterialComponent& mat, const std::string& text);
    // The same text applied as an INSTANCE DELTA: the keys present overwrite and the custom
    // parameter / texture bag is kept. Uses the loader's own parser, so a value means what it
    // will mean when a file carrying it loads (Editor.CreateMaterialInstance's meta keys).
    // Marks mat dirty.
    void ApplyMaterialTextDelta(MaterialComponent& mat, const std::string& text);
}
