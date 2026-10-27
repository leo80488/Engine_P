#pragma once

// AssetFS - read-only virtual filesystem: an optional mounted .ipak, then loose
// search roots.
//
// .ipak format (little-endian; written by tools/pack_assets.py, whose docstring has the layout):
//   version 2: a 64-byte header (the exe's build id, the TOC's offset, sizes and CRC32), the
//            entries' bytes (each distinct content once, large ones 4 KiB aligned), then the
//            TOC: per entry a key, offset, stored and raw size, method (0 stored, 1 zstd in
//            256 KiB blocks, each its own frame with a checksum) and the raw bytes' CRC32.
//            Mount checks the TOC's CRC; -verify_pak checks every entry's on each whole read.
//            A read decodes only the blocks it touches. A patch pak (MountPatch) is a version 2
//            pak over the others: its entries replace theirs, its tombstones delete keys.
//   version 1 (magic "IPAK", version u32, a bare path index) is refused with an error.
//
// Mount() reads the header and the index, keyed by forward-slash path; ReadFile reads
// an entry from the file at its offset when asked. The pak's bytes stay in the OS file
// cache, which the system reclaims, not in this process (Task Manager's "cached" grows,
// the process's private bytes do not). The handle is overlapped and every read is
// positional, so reads from several threads run at once. ReadFile looks in the pak
// first, then on disk, so the editor (no game.ipak, loose files) needs no call-site
// changes. RawSize, ReadRange and OpenStream read a file in part, from the same two
// sources: a header probe, a video demuxer. Threads: Mount, Unmount and root registration must not overlap a read;
// reads share only the immutable index and the handle, so they need no lock. ReadAsync and Prefetch queue
// requests for one IO thread of the filesystem's own.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Resource
{
    class AssetFS
    {
    public:
        static AssetFS& Get();

        AssetFS() = default;
        ~AssetFS();

        // Opens pakPath and reads its index. False if the file is missing or malformed;
        // the caller decides whether that is fatal (Game.exe: a missing pak is; Editor.exe
        // runs without one during development).
        bool Mount(const std::string& pakPath);

        // A patch over the mounted paks (P6, patch_*.ipak beside game.ipak): its entries replace
        // theirs, its tombstones remove keys; the top patch names the build. Version 2 only, after
        // Mount and before reads begin. False, with an ERROR and nothing changed, when the file is
        // missing or a pak Mount would refuse.
        bool MountPatch(const std::string& pakPath);
        std::size_t PatchCount() const { return m_patches.size(); }

        // Closes the mounted paks (if any). Subsequent ReadFile calls fall back to disk
        // only. No read may be in flight.
        void Unmount();

        bool IsMounted() const { return m_pak.handle != nullptr; }
        std::size_t PakEntryCount() const { return m_index.size(); }
        // The mounted pak's version (0 = none; only 2 mounts), the build id of the exe it was
        // packaged with and its content id (16 hex digits each).
        std::uint32_t      PakVersion() const { return m_pak.version; }
        const std::string& PakBuildId() const { return m_pak.buildId; }
        const std::string& PakContentId() const { return m_pak.contentId; }

        // Whole-entry reads of a version 2 pak are checked against the entry's CRC32: a damaged
        // pak fails the read with an ERROR instead of handing out its bytes. Off by default (a
        // pass over every byte read); -verify_pak turns it on at Mount.
        void SetVerifyEntries(bool on) { m_verifyEntries = on; }

        // Sealed: a path under asset/, engine/ or shaders/ that the mounted pak lacks is not looked
        // for on disk, so a loose tree cannot hide what the pak misses (-pak_sealed, harness builds).
        // No effect without a pak.
        void SetSealed(bool on) { m_sealed = on; }
        bool Sealed() const { return m_sealed; }

        // True if `path` is in the mounted pak after slash normalisation and after the
        // relocation table, so a pre-move spelling finds the entry it was renamed to - what
        // almost every caller means by "can this be loaded?", matching ReadFile. False if
        // no pak is mounted or the path is disk-only.
        bool HasInPak(const std::string& path) const;

        // The literal question: is this exact key in the pak, without relocation. Only for
        // callers that detect a stale spelling. The identity layer has two: the mint gate
        // must not accept a path that exists only after relocation (it would write a .meta
        // beside a directory that is not there), and ResolveStoredPath's healthy
        // short-circuit must not swallow a stale path, or the guid repair that rewrites the
        // container to the current spelling never runs and the path is never healed.
        bool HasInPakExact(const std::string& path) const;

        // ---- Search roots (loose fallback) ----
        // Named content roots (engine, project) for the loose half of ReadFile, tried in
        // registration order: the first registered wins a collision, and registering a name
        // again replaces its directory. With none registered a path resolves as given,
        // against the cwd. The pak wins over every root: a packaged build is sealed.
        void AddSearchRoot(std::string name, std::string dir);
        void ClearSearchRoots();
        std::size_t SearchRootCount() const { return m_roots.size(); }
        // Name of the root that resolved `path`, or "" (pak entries answer
        // "pak"). For diagnostics and for the editor to show what owns an asset.
        std::string RootOwning(const std::string& path) const;

        // The first on-disk location of `path` across the search roots, or ""
        // when no root has it. Does NOT consult the pak.
        std::string ResolveOnDisk(const std::string& path) const;

        // ---- The shared root (Resource/SharedContent.h) ----
        // A worktree that checked out only its text assets reads each binary one it lacks
        // from the main checkout. That root is always the last one, serves only paths under
        // `prefixes` ("asset/", "engine/"), never a path in `masked` (deleted on this
        // branch), and is read-only: code that writes, touches or rewrites a file it
        // resolved must resolve it with ResolveLocal, which never answers from the shared
        // root - a worktree's run must not change the main checkout. Another folder
        // registers a further shared root after it (a test's temporary one), each with its
        // own prefixes and mask; the same folder replaces its entry.
        void AddSharedRoot(std::string dir, std::vector<std::string> prefixes,
                           const std::vector<std::string>& masked);
        const std::string& SharedRootDir() const;           // the first one; "" = none
        std::string ResolveLocal(const std::string& path) const;
        bool IsSharedPath(const std::string& fullPath) const;   // a ResolveOnDisk answer from the shared root
        // Would the shared root answer this root-relative path (its prefix, not
        // masked)? No disk access - for listings that already found the file there.
        bool SharedServes(const std::string& path) const;
        // The same folder under the first shared root ("<root>/<relDir>"), when
        // that root serves the folder's prefix; "" otherwise (no shared root, an
        // absolute folder, one outside asset/ and engine/). A listing of a
        // worktree's folder is this checkout's entries plus the twin's others: a
        // folder holding only binary payload exists only there.
        std::string SharedTwinDir(const std::string& relDir) const;

        // Exists without the relocation retry: THIS spelling, in the pak or
        // under any root (what the identity layer's mint gate needs).
        bool ExistsExact(const std::string& path) const;

        // Every file under `relDir` ("asset", "engine/textures") that the loose
        // roots hold, as root-relative forward-slash paths; the first root wins
        // a collision, so the shared root adds only what this checkout lacks.
        // The loose-disk counterpart of EnumerateUnder, for scans that must see
        // a shared worktree's whole tree (the asset catalog, the identity index).
        void EnumerateLoose(const std::string& relDir, std::vector<std::string>& out) const;

        // Compatibility shim for content that moved between roots, for a path stored where
        // tooling cannot rewrite it (a player's settings.json, a save file, a Lua string
        // literal) and the identity layer does not cover (it tracks guid types only; .ttf is
        // not one). Only when nothing resolves a path is it rewritten through this table and
        // retried, so it costs nothing until a lookup fails. "" when no row matches.
        static std::string RemapRelocatedEnginePath(const std::string& path);

        // True when the pak or any search root has `path`. The one existence test asset
        // code should use: a raw fs::exists only sees the cwd.
        bool Exists(const std::string& path) const;

        // Read a file into `out`. Prefers the pak when mounted; then the search
        // roots in order; then the path as given (cwd-relative). Returns false
        // only if none of those has the file.
        bool ReadFile(const std::string& path, std::vector<uint8_t>& out) const;

        // ReadFile returning the bytes as a std::string. The Lua loaders use it
        // (sol::state::safe_script on the source), so scripts come from the pak in a
        // packed build.
        bool ReadFileText(const std::string& path, std::string& out) const;

        // The whole of `path` into @p dst, which holds @p capacity bytes (RawSize first): from the
        // pak (decoded) or the file straight into the caller's buffer, no vector between them.
        // @p outSize gets the byte count. False when no source has the path, it is larger than
        // @p capacity, or the read fails.
        bool ReadInto(const std::string& path, void* dst, std::size_t capacity,
                      std::size_t* outSize = nullptr) const;

        // ---- Partial and streamed reads ----
        // The bytes a ReadFile of `path` returns, without reading them: the pak entry's size,
        // else the loose file's. Empty when no source has the path.
        std::optional<std::uint64_t> RawSize(const std::string& path) const;

        // Up to @p size bytes of `path` from @p offset into `out`, fewer where the file ends.
        // False when no source has the path, @p offset is past its end or the read fails.
        bool ReadRange(const std::string& path, std::uint64_t offset, std::uint64_t size,
                       std::vector<std::uint8_t>& out) const;

        // A file read in pieces, at positions its reader picks.
        class Stream
        {
        public:
            virtual ~Stream() = default;

            // Up to @p size bytes at the position, which advances past them; 0 at the end or on
            // an error.
            virtual std::size_t   Read(void* dst, std::size_t size) = 0;
            // False past the end, and the position stays.
            virtual bool          Seek(std::uint64_t position) = 0;
            virtual std::uint64_t Position() const = 0;
            virtual std::uint64_t Size() const = 0;
        };
        // The pak entry, else the loose file; nullptr when no source has the path. A pak
        // stream reads through the mounted pak: destroy it before Unmount.
        std::unique_ptr<Stream> OpenStream(const std::string& path) const;

        // Append every mounted-pak entry whose normalised (forward-slash) path
        // begins with `prefix` to `out`. No-op when no pak is mounted. Used to
        // DISCOVER directory-scanned assets (e.g. asset/scripts/systems/*.lua) in
        // packed builds, where there are no loose files for a directory_iterator
        // to enumerate. Content is still read via ReadFile/ReadFileText.
        void EnumerateUnder(const std::string& prefix, std::vector<std::string>& out) const;

        // ---- Asynchronous reads ----
        // One IO thread serves the requests, the highest priority first and equal ones in order; it
        // starts with the first request. A callback runs on the IO thread: hand the bytes to another
        // thread there, do not parse them (every request behind it waits).
        enum class IoPriority : std::uint8_t { Low, Normal, High };
        using ReadDone = std::function<void(bool ok, std::vector<std::uint8_t>& bytes)>;
        // ReadFile on the IO thread, then @p done with what it read.
        void ReadAsync(std::string path, IoPriority priority, ReadDone done) const;
        // The bytes of `path` as they lie in their source into the OS file cache, at Low priority: a
        // zstd entry is read, not decoded. A later ReadFile then reads memory. Not counted as a use.
        void Prefetch(std::string path) const;
        // Returns once every request queued before the call has finished. Never from a callback.
        void WaitAsyncIdle() const;

        // ---- First-use order (-pak_order) ----
        // While on, every key read for the first time - a pak entry, or a root-relative loose file -
        // is appended to ReadOrder: the order a package lays its pak out in (tools/pso_record.py
        // records it, tools/package_game.py --pak-order uses it).
        void RecordReadOrder(bool on) { m_recordOrder.store(on, std::memory_order_relaxed); }
        std::vector<std::string> ReadOrder() const;

        // ---- Read statistics ----
        // Totals since the process started, from every thread.
        struct Stats
        {
            std::uint64_t pakReads   = 0;
            std::uint64_t pakBytes   = 0;
            std::uint64_t pakReadNs  = 0;   // spent reading pak entries
            std::uint64_t looseReads = 0;
            std::uint64_t looseBytes = 0;
            std::uint64_t pakFailures = 0;  // a pak entry whose read failed (an I/O error)
            std::uint64_t misses     = 0;   // ReadFile found the path nowhere
            std::vector<std::string> firstMisses;   // the first kMissSample of them, in order
            std::uint64_t asyncReads    = 0;   // ReadAsync requests served
            std::uint64_t prefetches    = 0;   // Prefetch requests served
            std::uint64_t prefetchBytes = 0;   // the bytes they brought in
        };
        static constexpr std::size_t kMissSample = 8;
        Stats GetStats() const;
        // Bytes the mounted pak keeps in memory: its index (an estimate of the hash map's
        // buckets, nodes and keys).
        std::uint64_t PakResidentBytes() const;
        // Logs "[PAK] <tag>: ..." - the totals and the pak's resident bytes.
        void LogStats(const char* tag) const;

    private:
        struct Entry
        {
            std::uint64_t offset = 0;
            std::uint64_t size   = 0;       // raw: what a read hands out
            std::uint32_t crc    = 0;
            bool          hasCrc = false;   // version 2
            std::uint64_t stored     = 0;   // the bytes in the pak (== size when stored)
            std::uint32_t blockSize  = 0;   // zstd: the raw size of each block but the last
            std::uint32_t firstBlock = 0;   // zstd: its first block in its pak's block table
            std::uint8_t  method     = 0;   // 0 stored, 1 zstd
            std::uint16_t pak        = 0;   // 0 the base pak, i the i-th patch
        };
        using IndexItem = std::pair<const std::string, Entry>;
        struct PakFile
        {
            void*         handle   = nullptr;   // a Win32 HANDLE, overlapped; nullptr = none mounted
            std::uint64_t fileSize = 0;
            std::string   path;
            std::uint32_t version  = 0;
            std::string   buildId;               // version 2: hex
            std::string   contentId;
            // zstd blocks: each one's stored size and its offset in the file.
            std::vector<std::uint32_t> blockStored;
            std::vector<std::uint64_t> blockStart;
        };
        struct Root
        {
            std::string name;
            std::string dir;
            bool                             shared = false;   // AddSharedRoot
            std::vector<std::string>         prefixes;         // lowercase, forward slash
            std::unordered_set<std::string>  masked;           // lowercase, forward slash
        };
        struct IoRequest
        {
            std::string path;
            ReadDone    done;               // ReadAsync's; empty for a prefetch
            bool        prefetch = false;
        };
        class PakStream;
        static std::string FoldKey(std::string rel);
        static bool        Serves(const Root& r, const std::string& rel);
        std::string        ResolveIn(const std::string& path, bool withShared) const;
        void               NoteMiss(const std::string& path) const;
        // The pak an entry's @p pak names: 0 the base, i the i-th patch.
        const PakFile&     PakOf(std::uint16_t pak) const { return pak == 0 ? m_pak : m_patches[pak - 1u]; }
        PakFile&           PakOf(std::uint16_t pak) { return pak == 0 ? m_pak : m_patches[pak - 1u]; }
        // Reads @p size bytes at @p offset of pak @p pak into @p dst. False on an I/O error or a
        // short read, with GetLastError set.
        bool               ReadAt(std::uint16_t pak, std::uint64_t offset, void* dst, std::uint64_t size) const;
        // A version 2 TOC after @p header (the file's first 64 bytes) into the index, for pak @p pak:
        // the base's entries go in, a patch's replace or (tombstones) remove. False, with an ERROR,
        // on a TOC Mount must refuse.
        bool               ReadTocV2(const std::uint8_t* header, std::uint16_t pak);
        // Mount's and MountPatch's end: the index's resident estimate and the "mounted" lines.
        void               NoteMounted(const PakFile& file, std::chrono::steady_clock::time_point started);
        // A zstd entry's raw bytes [offset, offset + size): the blocks they touch, each decoded. @p why:
        // zstd's reason on a bad block.
        bool               ReadZstd(const Entry& entry, std::uint64_t offset, void* dst, std::uint64_t size,
                                    const char*& why) const;
        // The pak's entry for `path`, after the relocation retry; nullptr when it has none.
        const IndexItem*   FindEntry(const std::string& path) const;
        // @p size bytes from @p offset into the entry @p key: counted in the statistics, an
        // ERROR when the read fails.
        bool               ReadEntry(const std::string& key, const Entry& entry, std::uint64_t offset,
                                     void* dst, std::uint64_t size) const;
        // Sealed (SetSealed) and `path` under asset/, engine/ or shaders/: no loose root may answer it.
        bool               SealedAway(const std::string& path) const;
        // RecordReadOrder: @p key appended the first time it is read; a loose read's path, when it is
        // root-relative (an absolute path is no pak key).
        void               NoteFirstUse(const std::string& key) const;
        void               NoteLooseUse(const std::string& path) const;
        // The IO thread: serves the queues until the destructor stops it.
        void               IoLoop() const;
        void               PrefetchNow(const std::string& path) const;
        // Queues @p request (m_ioMutex held) and starts the IO thread on the first one.
        void               PushIo(IoRequest request, IoPriority priority) const;

        PakFile                                  m_pak;
        std::vector<PakFile>                     m_patches;   // MountPatch, bottom first
        std::unordered_map<std::string, Entry>   m_index;
        std::uint64_t                            m_indexBytes = 0;   // PakResidentBytes
        // ReadFile's pak reads under way: Unmount closes the handle they read through.
        mutable std::atomic<std::uint32_t>       m_readsInFlight{ 0 };
        bool                                     m_verifyEntries = false;   // SetVerifyEntries
        bool                                     m_sealed = false;          // SetSealed
        // Ordered; first match wins. Empty = paths resolve against the cwd.
        std::vector<Root>                        m_roots;

        // ---- Read statistics (Stats) ----
        mutable std::atomic<std::uint64_t> m_pakReads{ 0 };
        mutable std::atomic<std::uint64_t> m_pakBytes{ 0 };
        mutable std::atomic<std::uint64_t> m_pakReadNs{ 0 };
        mutable std::atomic<std::uint64_t> m_looseReads{ 0 };
        mutable std::atomic<std::uint64_t> m_looseBytes{ 0 };
        mutable std::atomic<std::uint64_t> m_pakFailures{ 0 };
        mutable std::atomic<std::uint64_t> m_misses{ 0 };
        mutable std::mutex                 m_missMutex;   // guards m_missSample
        mutable std::vector<std::string>   m_missSample;

        // ---- First-use order (RecordReadOrder) ----
        mutable std::atomic<bool>               m_recordOrder{ false };
        mutable std::mutex                      m_orderMutex;   // guards m_orderSeen and m_order
        mutable std::unordered_set<std::string> m_orderSeen;
        mutable std::vector<std::string>        m_order;

        // ---- Asynchronous reads ----
        mutable std::mutex                           m_ioMutex;   // guards the queues, m_ioBusy, m_ioStop, m_ioThread
        mutable std::condition_variable              m_ioWake;    // a request queued, or m_ioStop
        mutable std::condition_variable              m_ioIdle;    // the queues empty and nothing being served
        mutable std::array<std::deque<IoRequest>, 3> m_ioQueues;  // by IoPriority
        mutable bool                                 m_ioBusy = false;
        mutable bool                                 m_ioStop = false;
        mutable std::thread                          m_ioThread;
        mutable std::atomic<std::uint64_t>           m_asyncReads{ 0 };
        mutable std::atomic<std::uint64_t>           m_prefetches{ 0 };
        mutable std::atomic<std::uint64_t>           m_prefetchBytes{ 0 };
    };
}
