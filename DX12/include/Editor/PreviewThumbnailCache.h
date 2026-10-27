#pragma once

// PreviewThumbnailCache - the disk half of the asset browser's preview tiles: the
// material tiles and the mesh / prefab asset tiles, one guid-keyed namespace.
// EDITOR ONLY, structurally: it lives in EditorCore next to TextureImporter
// (whose .itex container it reuses) and Game.exe never links it; a shipped game
// must never author into a cache directory or into asset/.
//
// Location: Sys::WritableDataDir() + "/cache/thumbnails/". NOT under asset/ or
// engine/: tools/package_game.py ships ENGINE_CONTENT_ROOT ("engine") WHOLE and
// cooks every .itex under asset/, so a cache there would put hundreds of MB of
// thumbnails into the package (or fail --strict). NOT LogDir() / DiagnosticsDir():
// -artifacts= repoints those per run. -artifacts does NOT move WritableDataDir,
// so THE CACHE SURVIVES A SCENARIO RUN: a cold run clears it with
// -preview_thumb_cache=clear.
//
// The key is GUID + STAMPS, not the path (a renamed or moved material keeps its
// tile). Each entry is two files:
//   <guid>_<stampHash>.itex   the 128^2 RGBA8 tile, in the engine's own container
//   <guid>_<stampHash>.json   { srcMtime, srcSize, deps: [{path,mtime,size}...],
//                               renderVersion, stamp }
// and an entry is FRESH only when every one of those still matches. `deps` covers
// the three invalidation signals - the material chain, the textures it resolves,
// its custom shader sources - because the engine stamps neither textures nor
// shader sources as material dependencies.
//
// The filename carries the stamp hash (content addressing) because the tile is
// read back through TextureSystem::Acquire, i.e. ResourceManager's PATH CACHE,
// which TextureSystem::Release never Unloads: a fixed <guid>.itex rewritten after
// an invalidation would be served with the OLD PIXELS for the life of the process,
// and an invalidation hook would reach into the shipping loader. A new dependency
// state is a new path. The hash is FNV-1a over kRenderVersion + the source stamp
// + every dep stamp, exactly the tuple LookupFresh compares, so an equal hash means
// an equal picture. Store deletes the older files for the guid: the steady state
// is one pair per material.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct MaterialComponent;

namespace Editor
{
    // Writes a 2D tile as .itex in the container TextureImporter writes (AssetHeader
    // + TextureMetadata + a SaveToDDSMemory blob; the tree has only
    // SaveTextureCubeToITEX), so the read side is the existing TextureLoader and there
    // is no new load path. @p rowPitch is the source stride in bytes, NOT assumed to be
    // w*4: the pixels come out of a D3D12 READBACK buffer, whose rows are aligned to 256.
    bool SavePreviewTileToITEX(const std::uint8_t* rgba, std::uint32_t w,
                               std::uint32_t h, std::uint32_t rowPitch,
                               const std::string& path);

    class PreviewThumbnailCache
    {
    public:
        struct Dep
        {
            std::string   path;
            std::uint64_t mtime = 0;
            std::uint64_t size  = 0;
        };

        // BUMP THIS whenever the preview pipeline changes what a tile looks like
        // (a new studio rig, a shadow pass, a different tone map). Without it, every
        // tile baked by an older pipeline stays on disk and is served forever - the
        // cache has no other way to know the renderer moved. When unsure whether the
        // pixels moved, bump: it costs one rebake per material on the next launch.
        static constexpr std::uint32_t kRenderVersion = 3;

        // Default GC ceiling. 512 MiB of 64 KiB tiles is ~8000 materials, well
        // past any project this editor opens; the cap exists because a disk
        // cache with no GC is a folder that only grows.
        static constexpr std::uint64_t kDefaultSizeCap = 512ull * 1024ull * 1024ull;

        static PreviewThumbnailCache& Get();

        // Drops the one-frame stat memo. Called once per frame from
        // EditorLayer::ServiceAssetThumbnails, before anything stamps: a file is stat'ed
        // at most once per frame and never across frames, so freshness is exact and a
        // folder of materials sharing the engine defaults costs a handful of syscalls.
        static void BeginFrame();

        // The absolute cache directory, created on first use.
        const std::string& Dir();
        // -preview_thumb_cache_dir=<dir> (harness): point the cache somewhere else
        // BEFORE anything reads it, so a scenario pack keeps its tiles inside its own
        // artifacts directory instead of writing into - and clearing - the developer's
        // real cache under WritableDataDir(). A relative path is relative to
        // Sys::ArtifactDir() (what -artifacts= moves); an absolute one is used as given.
        // Refused (returns false) once the directory has been resolved: a move after the
        // fact would strand every entry this session wrote.
        bool SetDirOverride(const std::string& dir);
        std::string ItexPath(const std::string& guidHex,
                             const std::string& stampHex) const;
        std::string JsonPath(const std::string& guidHex,
                             const std::string& stampHex) const;

        // -preview_thumb_cache=clear: deletes every file in the directory and drops the
        // in-memory indexes. Returns how many files went.
        std::size_t ClearAll();

        // Boot sweep: deletes entries whose guid is not in @p liveGuids (an orphan: the
        // asset was deleted or its sidecar changed; an empty set skips this pass), the
        // leftovers of an interrupted write (a .json with no .itex, a .itex with no .json,
        // a stale .tmp), then, if the total is still over @p sizeCap, the
        // LEAST-RECENTLY-USED entries until it is under half the cap. Call ONCE, after the
        // browser has built AssetGuidIndex.
        void BootGC(const std::unordered_set<std::string>& liveGuids,
                    std::uint64_t sizeCap = kDefaultSizeCap);

        // True when some <guid>_<stamp>.itex exists and every stamp in its sidecar still
        // matches what is on disk right now; @p outItexPath is filled on a hit. A missing
        // or unreadable sidecar is a MISS, never a hit: "no information" has to mean
        // "regenerate", or a truncated write serves a stale picture forever. Handles
        // several files per guid (a crash between Store's write and its cleanup can leave
        // two).
        bool LookupFresh(const std::string& guidHex, const std::string& srcPath,
                         std::string& outItexPath);

        // Writes both files. @p deps is what LookupFresh will re-check.
        bool Store(const std::string& guidHex, const std::string& srcPath,
                   const std::vector<Dep>& deps,
                   const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                   std::uint32_t rowPitch);

        // Deletes one entry's files and forgets its reverse-index rows.
        void Evict(const std::string& guidHex);

        // Every guid whose sidecar lists @p depPath (the texture and shader-source
        // signals). The index is filled by Store and LookupFresh, so it covers every entry
        // this session has touched; entries it has NOT touched are covered by the stamp
        // comparison in LookupFresh itself, the path a cold session takes.
        std::vector<std::string> GuidsDependingOn(const std::string& depPath) const;

        // The dependency set of one material: its own file, every texture path it
        // resolves (engine slots AND custom slots) and its two custom shader paths. Done
        // here rather than by extending MaterialSerializer's own dependency stamps: far
        // cheaper, and it keeps the change off the main load path.
        static std::vector<Dep> CollectDeps(const std::string& matPath,
                                            const MaterialComponent& mat);

        // Stamps one file (mtime + size, memoised for the frame). RESOLVED THROUGH THE
        // ASSET SEARCH ROOTS (Resource::AssetFS::ResolveOnDisk), not against the process
        // working directory: a dep is a ROOT-RELATIVE path ("engine/textures/x.itex",
        // "asset/Shaders/y.hlsl"), and under -project=<elsewhere> the cwd is not that
        // root. mtime 0 means "no information", and every comparison treats it as a
        // MISMATCH - the safe direction.
        static Dep Stamp(const std::string& path);

        // Counters for the exit summary line.
        std::uint32_t Hits()   const { return m_hits; }
        std::uint32_t Writes() const { return m_writes; }
        std::uint32_t Evicts() const { return m_evicts; }

    private:
        PreviewThumbnailCache() = default;
        void IndexDeps(const std::string& guidHex, const std::vector<Dep>& deps);
        void UnhookDeps(const std::string& guidHex);
        // One directory scan per session learns which <guid>_<stamp> pairs are on disk;
        // Store / Evict / ClearAll keep it true. Cheaper and far more predictable than a
        // per-lookup scan.
        void EnsureIndex();
        // FNV-1a over kRenderVersion + the source stamp + every dep stamp - the
        // file-name half of the content addressing. 16 lowercase hex.
        static std::string StampHash(const Dep& src, const std::vector<Dep>& deps);

        // Records @p stampHex for @p guidHex and logs the rebind when it moved.
        void NoteStamp(const std::string& guidHex, const std::string& stampHex);

        std::string m_dir;
        bool        m_dirResolved = false;
        bool        m_indexed     = false;
        // guid -> the stamp hashes present on disk for it (normally exactly 1).
        std::unordered_map<std::string, std::vector<std::string>> m_files;
        // guid -> the stamp hash this session last WROTE or SERVED for it. Purely an
        // observation record, never consulted for freshness, and - unlike every other row
        // - it deliberately SURVIVES Evict. A guid whose stamp hash differs from the one
        // recorded here has had its dependency state change, so its tile is a DIFFERENT
        // FILE and ResourceManager's path cache (which TextureSystem::Release never
        // Unloads) cannot hand back the old pixels. One log line says so, and a pack
        // asserts it.
        std::unordered_map<std::string, std::string> m_lastStamp;

        // Guids whose tile file has already had its mtime bumped this session.
        // BootGC's LRU sorts on that mtime, so a HIT has to touch it or a daily-used
        // tile is evicted before an unused newer one; once per session avoids a write
        // syscall per hit, and "used this session" is all the resolution an LRU over
        // sessions needs.
        std::unordered_set<std::string> m_touched;
        // dep path (lowercased, forward slashes) -> guids that name it.
        std::unordered_map<std::string, std::unordered_set<std::string>> m_depIndex;
        // guid -> its dep paths, so Evict can unhook the rows above.
        std::unordered_map<std::string, std::vector<std::string>> m_byGuid;
        std::uint32_t m_hits   = 0;
        std::uint32_t m_writes = 0;
        std::uint32_t m_evicts = 0;
    };
}
