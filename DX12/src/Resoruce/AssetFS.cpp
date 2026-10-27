#include "Resource/AssetFS.h"
#include "Resource/LoadCounters.h"   // a runtime spawn's census counts its reads
#include "Diagnostics/Profiler.h"
#include "System/BuildInfo.h"     // the exe a version 2 pak was packaged with
#include "System/Check.h"
#include "System/CommandLine.h"   // -verify_pak
#include "System/Log.h"
#include "System/PathUtil.h"   // UTF-8 path aware disk I/O (non-ASCII paths)

#include "zstd/lib/zstd.h"   // zstd entries (external/zstd: the decoder)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace
{
    // Mount reads the header in steps of this size.
    constexpr std::uint64_t kIndexChunk = 64 * 1024;
    // One ::ReadFile at most: its length is a DWORD.
    constexpr std::uint64_t kReadChunk = 16ull * 1024 * 1024;

    // ---- Version 2 (tools/pack_assets.py) ----
    // The header's fields at their offsets in the file.
    struct HeaderV2
    {
        char          magic[4];
        std::uint32_t version;
        std::uint32_t flags;
        std::uint32_t headerSize;
        std::uint8_t  buildId[16];   // the exe's build id, then the TOC's content id
        std::uint64_t tocOffset;
        std::uint64_t tocStoredSize;
        std::uint64_t tocRawSize;
        std::uint32_t tocCrc;
        std::uint32_t dataAlign;
    };
    static_assert(sizeof(HeaderV2) == 64, "HeaderV2 is the file's 64 bytes");
    // A TOC entry: pathOff u32, pathLen u16, method u8, flags u8, offset u64, storedSize u64,
    // rawSize u64, crc32 u32, blockSize u32, firstBlock u32.
    constexpr std::uint64_t kTocEntryBytes = 44;
    constexpr std::uint8_t  kMethodStored  = 0;
    constexpr std::uint8_t  kMethodZstd    = 1;
    constexpr std::uint8_t  kFlagDeleted   = 1;   // a patch's tombstone
    // A TOC this large is a damaged header, not a pak (RossiDuel's is about 0.1 MiB).
    constexpr std::uint64_t kMaxTocBytes = 256ull * 1024 * 1024;

    // CRC-32 (zlib's: reflected 0xEDB88320), eight bytes a step.
    constexpr std::array<std::array<std::uint32_t, 256>, 8> MakeCrcTables()
    {
        std::array<std::array<std::uint32_t, 256>, 8> t{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; ++i)
            for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        return t;
    }
    constexpr std::array<std::array<std::uint32_t, 256>, 8> kCrcTables = MakeCrcTables();

    std::uint32_t Crc32(const std::uint8_t* data, std::uint64_t size)
    {
        std::uint32_t c = 0xFFFFFFFFu;
        while (size >= 8)
        {
            std::uint32_t lo = 0;
            std::uint32_t hi = 0;
            std::memcpy(&lo, data, 4);
            std::memcpy(&hi, data + 4, 4);
            lo ^= c;
            c = kCrcTables[7][lo & 0xFF] ^ kCrcTables[6][(lo >> 8) & 0xFF] ^ kCrcTables[5][(lo >> 16) & 0xFF] ^
                kCrcTables[4][lo >> 24] ^ kCrcTables[3][hi & 0xFF] ^ kCrcTables[2][(hi >> 8) & 0xFF] ^
                kCrcTables[1][(hi >> 16) & 0xFF] ^ kCrcTables[0][hi >> 24];
            data += 8;
            size -= 8;
        }
        while (size-- > 0) c = kCrcTables[0][(c ^ *data++) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    std::string Hex(const std::uint8_t* bytes, std::size_t n)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i)
        {
            s += kDigits[bytes[i] >> 4];
            s += kDigits[bytes[i] & 0xF];
        }
        return s;
    }

    // One event per thread for the pak's overlapped reads: each thread waits on its own.
    struct ReadEvent
    {
        ReadEvent() : handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
        ~ReadEvent() { if (handle) ::CloseHandle(handle); }

        HANDLE handle;
    };
    thread_local ReadEvent t_readEvent;

    // Forward slashes everywhere. The .ipak index is built with normalised
    // keys, so ReadFile callers that hand in either "asset/foo" or "asset\foo"
    // hit the same entry.
    std::string Normalize(std::string s)
    {
        for (char& c : s) if (c == '\\') c = '/';
        while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
            s.erase(0, 2);
        // Imported prefabs can refer to sibling resources through ../.
        // Disk I/O resolves these segments, but a pak hash lookup cannot.
        if (s.find("/.") != std::string::npos)
        {
            s = Sys::PathToUtf8(Sys::U8Path(s).lexically_normal());
            for (char& c : s) if (c == '\\') c = '/';
        }
        return s;
    }
}

namespace Resource
{
    AssetFS& AssetFS::Get()
    {
        static AssetFS s_instance;
        return s_instance;
    }

    void AssetFS::AddSearchRoot(std::string name, std::string dir)
    {
        if (dir.empty()) return;
        for (char& c : dir) if (c == '\\') c = '/';
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
        // Registering a name again replaces its directory instead of shadowing it: one
        // name with two directories would resolve differently depending on order.
        for (Root& r : m_roots)
            if (r.name == name) { r.dir = std::move(dir); return; }
        LOG_INFO("AssetFS: search root '%s' -> '%s' (priority %zu)",
                 name.c_str(), dir.c_str(), m_roots.size());
        // The shared root stays LAST whatever registers after it.
        auto at = m_roots.end();
        for (auto it = m_roots.begin(); it != m_roots.end(); ++it)
            if (it->shared) { at = it; break; }
        m_roots.insert(at, Root{ std::move(name), std::move(dir) });
    }

    void AssetFS::ClearSearchRoots()
    {
        m_roots.clear();
    }

    std::string AssetFS::RemapRelocatedEnginePath(const std::string& path)
    {
        // old prefix -> new prefix. Add a row whenever engine content moves
        // between roots; never remove one, because the stored paths this
        // rescues live on other people's disks.
        static const struct { const char* from; const char* to; } kMoved[] = {
            // Keep the "from" column spelled the old way: a find-and-replace that turns a row
            // into an identity mapping silently disables it, and every stale stored path then
            // fails to resolve without a word.
            { "asset/Default_Texture/",      "engine/textures/" },
            { "asset/font/",                 "engine/font/"     },
            { "asset/IBL/",                  "engine/ibl/"      },
            { "asset/EngineResource/UI/",    "engine/ui/"       },
            { "asset/EngineResource/World/BaseScene.iscene",
                                             "engine/world/BaseScene.iscene" },

            // A project rename (asset/EngineResource/ -> asset/Content/), not an engine move:
            // the tools repointed every container reference, but a path stored where no tool
            // reaches (a save file, game.json, a Lua literal, a scenario fixture) is rescued
            // only here.
            //
            // Keep this row last: the table returns on the first match, and the two rows above
            // are narrower prefixes of it - ahead of them, asset/EngineResource/UI/x would
            // become asset/Content/UI/x, a path that does not exist.
            { "asset/EngineResource/",       "asset/Content/"   },
        };
        const std::string rel = Normalize(path);
        for (const auto& m : kMoved)
        {
            const std::size_t n = std::strlen(m.from);
            // ">=", not ">": a row whose "from" names a single file, not a directory prefix,
            // matches at exactly its own length (a game.json naming
            // asset/EngineResource/World/BaseScene.iscene).
            if (rel.size() >= n && rel.compare(0, n, m.from) == 0)
                return std::string(m.to) + rel.substr(n);
        }
        return {};
    }

    void AssetFS::AddSharedRoot(std::string dir, std::vector<std::string> prefixes,
                                const std::vector<std::string>& masked)
    {
        for (char& c : dir) if (c == '\\') c = '/';
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
        if (dir.empty()) return;
        Root r{ "shared", std::move(dir) };
        r.shared   = true;
        r.prefixes = std::move(prefixes);
        for (std::string& p : r.prefixes) p = FoldKey(Normalize(p));
        for (const std::string& m : masked) r.masked.insert(FoldKey(Normalize(m)));
        // The same folder again replaces its entry; another one goes after it.
        for (Root& existing : m_roots)
            if (existing.shared && FoldKey(existing.dir) == FoldKey(r.dir)) { existing = std::move(r); return; }
        LOG_INFO("AssetFS: search root 'shared' -> '%s' (last; %zu prefix(es), %zu masked path(s))",
                 r.dir.c_str(), r.prefixes.size(), r.masked.size());
        m_roots.push_back(std::move(r));
    }

    const std::string& AssetFS::SharedRootDir() const
    {
        static const std::string kNone;
        for (const Root& r : m_roots)
            if (r.shared) return r.dir;
        return kNone;
    }

    std::string AssetFS::FoldKey(std::string rel)
    {
        for (char& c : rel)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return rel;
    }

    bool AssetFS::Serves(const Root& r, const std::string& rel)
    {
        if (!r.shared) return true;
        const std::string key = FoldKey(rel);
        if (r.masked.count(key)) return false;   // deleted on this branch
        for (const std::string& p : r.prefixes)
            if (key.compare(0, p.size(), p) == 0) return true;
        return r.prefixes.empty();
    }

    std::string AssetFS::ResolveIn(const std::string& path, bool withShared) const
    {
        const std::string rel = Normalize(path);
        std::error_code ec;
        for (const Root& r : m_roots)
        {
            if ((r.shared && !withShared) || !Serves(r, rel)) continue;
            const std::string full = r.dir + "/" + rel;
            if (std::filesystem::exists(Sys::U8Path(full), ec)) return full;
        }

        // Nothing has it: before giving up, retry through the relocation table (a stored
        // asset/font/X.ttf resolves to engine/font/X.ttf).
        const std::string moved = RemapRelocatedEnginePath(rel);
        if (!moved.empty())
            for (const Root& r : m_roots)
            {
                if ((r.shared && !withShared) || !Serves(r, moved)) continue;
                const std::string full = r.dir + "/" + moved;
                if (std::filesystem::exists(Sys::U8Path(full), ec)) return full;
            }
        return {};
    }

    std::string AssetFS::ResolveOnDisk(const std::string& path) const
    {
        return ResolveIn(path, /*withShared*/ true);
    }

    std::string AssetFS::ResolveLocal(const std::string& path) const
    {
        return ResolveIn(path, /*withShared*/ false);
    }

    bool AssetFS::IsSharedPath(const std::string& fullPath) const
    {
        std::string a = fullPath;
        for (char& c : a) if (c == '\\') c = '/';
        for (const Root& r : m_roots)
        {
            const std::string& s = r.dir;
            if (r.shared && a.size() > s.size() && FoldKey(a.substr(0, s.size())) == FoldKey(s) && a[s.size()] == '/')
                return true;
        }
        return false;
    }

    bool AssetFS::SharedServes(const std::string& path) const
    {
        const std::string rel = Normalize(path);
        for (const Root& r : m_roots)
            if (r.shared && Serves(r, rel)) return true;
        return false;
    }

    std::string AssetFS::SharedTwinDir(const std::string& relDir) const
    {
        std::string rel = Normalize(relDir);
        while (!rel.empty() && rel.back() == '/') rel.pop_back();
        if (rel.empty() || Sys::U8Path(rel).is_absolute()) return {};
        const std::string key = FoldKey(rel) + "/";
        for (const Root& r : m_roots)
        {
            if (!r.shared) continue;
            for (const std::string& p : r.prefixes)
                if (key.compare(0, p.size(), p) == 0) return r.dir + "/" + rel;
            return {};
        }
        return {};
    }

    bool AssetFS::ExistsExact(const std::string& path) const
    {
        if (HasInPakExact(path)) return true;
        if (SealedAway(path)) return false;
        const std::string rel = Normalize(path);
        std::error_code ec;
        for (const Root& r : m_roots)
        {
            if (!Serves(r, rel)) continue;
            if (std::filesystem::exists(Sys::U8Path(r.dir + "/" + rel), ec)) return true;
        }
        return m_roots.empty() && std::filesystem::exists(Sys::U8Path(path), ec);
    }

    void AssetFS::EnumerateLoose(const std::string& relDir, std::vector<std::string>& out) const
    {
        std::string base = Normalize(relDir);
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::unordered_set<std::string> seen;
        std::error_code ec;
        // rootDir empty: `base` is walked as given and reported as iterated.
        auto walk = [&](const std::string& rootDir, const Root* r)
        {
            const std::filesystem::path top = Sys::U8Path(rootDir.empty() ? base : rootDir + "/" + base);
            if (!std::filesystem::is_directory(top, ec)) return;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     top, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec)) continue;
                std::string rel = Sys::PathToUtf8(rootDir.empty() ? it->path()
                                                                  : it->path().lexically_relative(Sys::U8Path(rootDir)));
                for (char& c : rel) if (c == '\\') c = '/';
                if (r && !Serves(*r, rel)) continue;
                if (seen.insert(FoldKey(rel)).second) out.push_back(std::move(rel));
            }
        };
        // An absolute folder (a fixture tree) belongs to no root.
        if (m_roots.empty() || Sys::U8Path(base).is_absolute()) { walk(std::string(), nullptr); return; }
        // Two roots on one folder (project and engine are both "." in the
        // repo) are one walk: the second could only find what the first did.
        std::unordered_set<std::string> walked;
        for (const Root& r : m_roots)
            if (walked.insert(FoldKey(r.dir)).second) walk(r.dir, &r);
    }

    std::string AssetFS::RootOwning(const std::string& path) const
    {
        if (HasInPak(path)) return "pak";

        // Ask ResolveOnDisk (which also retries relocated paths) and map its answer back
        // to a root name, so a stale engine path still names its owner.
        const std::string full = ResolveOnDisk(path);
        if (full.empty()) return {};
        for (const Root& r : m_roots)
            if (full.size() > r.dir.size() && full.compare(0, r.dir.size(), r.dir) == 0)
                return r.name;
        return {};
    }

    bool AssetFS::Exists(const std::string& path) const
    {
        if (HasInPak(path)) return true;
        if (SealedAway(path)) return false;
        if (!ResolveOnDisk(path).empty()) return true;
        // No root has it: fall back to the path as given (cwd-relative), the only lookup
        // that works before any root is registered.
        std::error_code ec;
        return std::filesystem::exists(Sys::U8Path(path), ec);
    }

    AssetFS::~AssetFS()
    {
        // The IO thread first: the requests still queued are dropped, their callbacks never run.
        {
            std::lock_guard<std::mutex> lock(m_ioMutex);
            m_ioStop = true;
        }
        m_ioWake.notify_all();
        if (m_ioThread.joinable()) m_ioThread.join();
        // No Unmount: its check logs, and the logger may be gone at static destruction.
        if (m_pak.handle) ::CloseHandle(static_cast<HANDLE>(m_pak.handle));
    }

    bool AssetFS::Mount(const std::string& pakPath)
    {
        Unmount();
        const auto t0 = std::chrono::steady_clock::now();

        HANDLE file = ::CreateFileW(Sys::U8Path(pakPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                LOG_INFO("AssetFS: no pak at '%s' — running disk-only", pakPath.c_str());
            else
                LOG_ERROR("AssetFS: cannot open '%s' (error %lu) — running disk-only", pakPath.c_str(), err);
            return false;
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size))
        {
            LOG_ERROR("AssetFS: read failed on '%s' (error %lu)", pakPath.c_str(), ::GetLastError());
            ::CloseHandle(file);
            return false;
        }
        m_pak.handle   = file;
        m_pak.fileSize = static_cast<std::uint64_t>(size.QuadPart);
        m_pak.path     = pakPath;
        const std::uint64_t fileSize = m_pak.fileSize;

        // The header, read in kIndexChunk steps; freed when Mount returns.
        std::vector<std::uint8_t> head;
        DWORD readError = 0;
        // True once [0, end) is in head; false past the end of the file or on a read error.
        auto need = [&](std::uint64_t end) -> bool
        {
            if (end > fileSize) return false;
            if (head.size() >= end) return true;
            const std::uint64_t at = head.size();
            const std::uint64_t to = std::min(fileSize, std::max(end, at + kIndexChunk));
            head.resize(static_cast<std::size_t>(to));
            if (ReadAt(0, at, head.data() + at, to - at)) return true;
            readError = ::GetLastError();
            head.resize(static_cast<std::size_t>(at));
            return false;
        };
        // Logs the read error behind a failed need(), if there was one.
        auto readFailed = [&]() -> bool
        {
            if (readError == 0) return false;
            LOG_ERROR("AssetFS: read failed on '%s' (error %lu)", pakPath.c_str(), readError);
            return true;
        };

        // Header: 4 bytes magic + u32 version + u32 entry count
        if (!need(12) || head[0] != 'I' || head[1] != 'P' || head[2] != 'A' || head[3] != 'K')
        {
            if (!readFailed())
                LOG_ERROR("AssetFS: '%s' is not a valid .ipak (bad magic)", pakPath.c_str());
            Unmount();
            return false;
        }

        std::uint32_t version = 0;
        std::memcpy(&version, head.data() + 4, 4);
        m_pak.version = version;
        // -verify_pak: every whole-entry read of a version 2 pak checks the entry's CRC32 (a pass
        // over every byte read), so a damaged pak names the entry instead of loading its bytes.
        static const bool s_verifyPak = Sys::CommandLine::Get().Has("-verify_pak");
        if (s_verifyPak) m_verifyEntries = true;
        if (version == 2)
        {
            if (!need(sizeof(HeaderV2)))
            {
                if (!readFailed()) LOG_ERROR("AssetFS: '%s' has a truncated header", pakPath.c_str());
                Unmount();
                return false;
            }
            if (!ReadTocV2(head.data(), 0))
            {
                Unmount();
                return false;
            }
            NoteMounted(m_pak, t0);
            return true;
        }
        // Version 1 (a bare path index) was written by an older packer and is no longer read.
        LOG_ERROR("AssetFS: '%s' is a version %u pak; only version 2 is read - repackage it with "
                  "tools/pack_assets.py", pakPath.c_str(), version);
        Unmount();
        return false;
    }

    bool AssetFS::MountPatch(const std::string& pakPath)
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (!IsMounted())
        {
            LOG_ERROR("AssetFS: MountPatch('%s') with no pak mounted", pakPath.c_str());
            return false;
        }
        if (m_patches.size() >= 0xFFFEu)
        {
            LOG_ERROR("AssetFS: MountPatch('%s'): too many patches", pakPath.c_str());
            return false;
        }
        HANDLE file = ::CreateFileW(Sys::U8Path(pakPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            LOG_ERROR("AssetFS: cannot open the patch '%s' (error %lu)", pakPath.c_str(), ::GetLastError());
            return false;
        }
        LARGE_INTEGER size{};
        ::GetFileSizeEx(file, &size);
        PakFile patch;
        patch.handle   = file;
        patch.fileSize = static_cast<std::uint64_t>(size.QuadPart);
        patch.path     = pakPath;
        patch.version  = 2;
        m_patches.push_back(std::move(patch));
        const auto pak = static_cast<std::uint16_t>(m_patches.size());

        // A refused patch leaves the mounted paks as they were: the TOC goes into a copy of the index.
        std::uint8_t header[sizeof(HeaderV2)] = {};
        std::uint32_t version = 0;
        bool ok = m_patches.back().fileSize >= sizeof(HeaderV2) && ReadAt(pak, 0, header, sizeof(header));
        if (ok) std::memcpy(&version, header + 4, 4);
        if (!ok || std::memcmp(header, "IPAK", 4) != 0 || version != 2)
        {
            LOG_ERROR("AssetFS: '%s' is not a version 2 pak; a patch must be one", pakPath.c_str());
            ok = false;
        }
        std::unordered_map<std::string, Entry> kept;
        if (ok)
        {
            kept = m_index;
            ok = ReadTocV2(header, pak);
        }
        if (!ok)
        {
            if (!kept.empty()) m_index = std::move(kept);
            ::CloseHandle(static_cast<HANDLE>(m_patches.back().handle));
            m_patches.pop_back();
            return false;
        }
        NoteMounted(m_patches.back(), t0);
        return true;
    }

    void AssetFS::NoteMounted(const PakFile& file, std::chrono::steady_clock::time_point started)
    {
        // An estimate: the bucket array, the nodes, and the keys too long for the string's
        // inline buffer.
        const std::size_t inlineChars = std::string().capacity();
        m_indexBytes = m_index.bucket_count() * sizeof(void*);
        for (const auto& kv : m_index)
            m_indexBytes += sizeof(kv) + 2 * sizeof(void*) +
                            (kv.first.capacity() > inlineChars ? kv.first.capacity() + 1 : 0);

        LOG_INFO("AssetFS: mounted '%s' — %zu entries, %.1f MiB (read on demand; index %.0f KiB resident, %.1f ms)",
                 file.path.c_str(), m_index.size(), file.fileSize / (1024.0 * 1024.0), m_indexBytes / 1024.0,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        if (file.version != 2) return;
        LOG_INFO("AssetFS: pak version 2%s, build %s, content %s%s", m_patches.empty() ? "" : " (a patch)",
                 file.buildId.c_str(), file.contentId.c_str(),
                 m_verifyEntries ? ", every entry checked against its CRC" : "");
        // The top pak names the build its content belongs to; zeros = packaged without an id. A base pak
        // under patches is an older build's by design, so only the top one is compared.
        const std::string zeros(16, '0');
        const std::string exeBuild = Sys::Build().buildId;
        if (file.buildId != zeros && exeBuild != zeros && file.buildId != exeBuild)
            LOG_WARNING("AssetFS: '%s' was packaged with build %s; this exe is build %s", file.path.c_str(),
                        file.buildId.c_str(), exeBuild.c_str());
    }

    bool AssetFS::ReadTocV2(const std::uint8_t* header, std::uint16_t pakIndex)
    {
        PakFile& file = PakOf(pakIndex);
        HeaderV2 h{};
        std::memcpy(&h, header, sizeof(h));
        const std::uint64_t fileSize = file.fileSize;
        const char* pak = file.path.c_str();
        if (h.headerSize < sizeof(HeaderV2) || h.tocOffset < h.headerSize || h.tocStoredSize > fileSize ||
            h.tocOffset > fileSize - h.tocStoredSize || h.tocStoredSize < 8)
        {
            LOG_ERROR("AssetFS: '%s': the TOC lies outside the file", pak);
            return false;
        }
        if (h.tocStoredSize != h.tocRawSize)
        {
            LOG_ERROR("AssetFS: '%s' has a compressed TOC; this build reads stored ones", pak);
            return false;
        }
        if (h.tocStoredSize > kMaxTocBytes)
        {
            LOG_ERROR("AssetFS: '%s': a TOC of %llu bytes", pak, static_cast<unsigned long long>(h.tocStoredSize));
            return false;
        }
        std::vector<std::uint8_t> toc(static_cast<std::size_t>(h.tocStoredSize));
        if (!ReadAt(pakIndex, h.tocOffset, toc.data(), toc.size()))
        {
            LOG_ERROR("AssetFS: read failed on '%s' (error %lu)", pak, ::GetLastError());
            return false;
        }
        const std::uint32_t tocCrc = Crc32(toc.data(), toc.size());
        if (tocCrc != h.tocCrc)
        {
            LOG_ERROR("AssetFS: '%s': the TOC fails its CRC check (recorded %08X, read %08X) - the pak is damaged",
                      pak, h.tocCrc, tocCrc);
            return false;
        }

        std::uint32_t entryCount = 0;
        std::uint32_t blockCount = 0;
        std::memcpy(&entryCount, toc.data(), 4);
        std::memcpy(&blockCount, toc.data() + 4, 4);
        const std::uint64_t stringsAt = 8 + static_cast<std::uint64_t>(entryCount) * kTocEntryBytes +
                                        static_cast<std::uint64_t>(blockCount) * 4;
        if (stringsAt > toc.size())
        {
            LOG_ERROR("AssetFS: '%s': truncated TOC", pak);
            return false;
        }
        const std::uint64_t stringBytes = toc.size() - stringsAt;
        const std::uint8_t* blockTable = toc.data() + 8 + static_cast<std::uint64_t>(entryCount) * kTocEntryBytes;
        file.blockStored.resize(blockCount);
        if (blockCount) std::memcpy(file.blockStored.data(), blockTable, blockCount * sizeof(std::uint32_t));
        file.blockStart.assign(blockCount, 0);
        m_index.reserve(m_index.size() + entryCount);
        for (std::uint32_t i = 0; i < entryCount; ++i)
        {
            const std::uint8_t* row = toc.data() + 8 + static_cast<std::uint64_t>(i) * kTocEntryBytes;
            std::uint32_t pathOff = 0;
            std::uint16_t pathLen = 0;
            Entry e{};
            std::memcpy(&pathOff, row, 4);
            std::memcpy(&pathLen, row + 4, 2);
            e.method = row[6];
            const std::uint8_t flags = row[7];
            std::memcpy(&e.offset, row + 8, 8);
            std::memcpy(&e.stored, row + 16, 8);
            std::memcpy(&e.size, row + 24, 8);
            std::memcpy(&e.crc, row + 32, 4);
            std::memcpy(&e.blockSize, row + 36, 4);
            std::memcpy(&e.firstBlock, row + 40, 4);
            e.hasCrc = true;
            e.pak    = pakIndex;
            if (pathOff > stringBytes || pathLen > stringBytes - pathOff)
            {
                LOG_ERROR("AssetFS: '%s': entry %u's key lies outside the string table", pak, i);
                return false;
            }
            std::string key(reinterpret_cast<const char*>(toc.data() + stringsAt + pathOff), pathLen);
            if (flags & kFlagDeleted)
            {
                // A tombstone: the key is gone from the paks under this one.
                if (pakIndex != 0) m_index.erase(Normalize(std::move(key)));
                continue;
            }
            if (e.method == kMethodStored)
            {
                if (e.stored != e.size)
                {
                    LOG_ERROR("AssetFS: '%s': the stored entry '%s' has two sizes", pak, key.c_str());
                    return false;
                }
            }
            else if (e.method == kMethodZstd)
            {
                // Its blocks: inside the table, adding up to its stored size; each one's offset in the file.
                const std::uint64_t blocks = e.blockSize ? (e.size + e.blockSize - 1) / e.blockSize : 0;
                if (e.blockSize == 0 || e.firstBlock > blockCount || blocks > blockCount - e.firstBlock)
                {
                    LOG_ERROR("AssetFS: '%s': entry '%s''s blocks lie outside the block table", pak, key.c_str());
                    return false;
                }
                std::uint64_t at = e.offset;
                for (std::uint64_t b = 0; b < blocks; ++b)
                {
                    file.blockStart[e.firstBlock + b] = at;
                    at += file.blockStored[e.firstBlock + b];
                }
                if (at - e.offset != e.stored)
                {
                    LOG_ERROR("AssetFS: '%s': entry '%s''s blocks do not add up to its size", pak, key.c_str());
                    return false;
                }
            }
            else
            {
                LOG_ERROR("AssetFS: '%s': entry '%s' is stored with method %u; this build reads 0 and 1",
                          pak, key.c_str(), static_cast<unsigned>(e.method));
                return false;
            }
            // Within the data: after the header, before the TOC. Not offset + size > end: a crafted
            // offset wraps that sum.
            if (e.offset < h.headerSize || e.stored > h.tocOffset || e.offset > h.tocOffset - e.stored)
            {
                LOG_ERROR("AssetFS: '%s': entry '%s' lies outside the data", pak, key.c_str());
                return false;
            }
            if (pakIndex == 0)
                m_index.emplace(Normalize(std::move(key)), e);
            else
                m_index.insert_or_assign(Normalize(std::move(key)), e);
        }
        file.buildId   = Hex(h.buildId, 8);
        file.contentId = Hex(h.buildId + 8, 8);
        return true;
    }

    void AssetFS::Unmount()
    {
        // A queued request would read through the handle this closes.
        WaitAsyncIdle();
        // A read in flight reads through this handle, which a later CreateFile may reuse.
        ENGINE_CHECK(m_readsInFlight.load(std::memory_order_acquire) == 0,
                     "AssetFS::Unmount with %u pak read(s) in flight", m_readsInFlight.load());
        if (m_pak.handle) ::CloseHandle(static_cast<HANDLE>(m_pak.handle));
        for (PakFile& patch : m_patches)
            if (patch.handle) ::CloseHandle(static_cast<HANDLE>(patch.handle));
        m_patches.clear();
        m_pak = {};
        m_index = {};
        m_indexBytes = 0;
    }

    bool AssetFS::ReadAt(std::uint16_t pak, std::uint64_t offset, void* dst, std::uint64_t size) const
    {
        HANDLE file = static_cast<HANDLE>(PakOf(pak).handle);
        if (!t_readEvent.handle) return false;   // CreateEventW failed; GetLastError says why
        auto* out = static_cast<std::uint8_t*>(dst);
        while (size > 0)
        {
            const DWORD want = static_cast<DWORD>(std::min(size, kReadChunk));
            OVERLAPPED ov{};
            ov.Offset     = static_cast<DWORD>(offset);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            ov.hEvent     = t_readEvent.handle;
            // An overlapped handle reports the count through GetOverlappedResult, whether the
            // read completed at once or is pending.
            if (!::ReadFile(file, out, want, nullptr, &ov) && ::GetLastError() != ERROR_IO_PENDING)
                return false;
            DWORD got = 0;
            if (!::GetOverlappedResult(file, &ov, &got, TRUE))
                return false;
            if (got != want)
            {
                ::SetLastError(ERROR_HANDLE_EOF);
                return false;
            }
            out    += want;
            offset += want;
            size   -= want;
        }
        return true;
    }

    const AssetFS::IndexItem* AssetFS::FindEntry(const std::string& path) const
    {
        if (m_index.empty()) return nullptr;
        auto it = m_index.find(Normalize(path));
        // The same relocation retry as the loose half: a packed build reading a path out
        // of a player's settings.json faces the same staleness, with no loose fallback.
        if (it == m_index.end())
        {
            const std::string moved = RemapRelocatedEnginePath(path);
            if (!moved.empty()) it = m_index.find(moved);
        }
        return it != m_index.end() ? &*it : nullptr;
    }

    bool AssetFS::ReadEntry(const std::string& key, const Entry& entry, std::uint64_t offset, void* dst,
                            std::uint64_t size) const
    {
        const auto t0 = std::chrono::steady_clock::now();
        m_readsInFlight.fetch_add(1, std::memory_order_acquire);
        const char* why = nullptr;
        const bool ok = entry.method == kMethodZstd ? ReadZstd(entry, offset, dst, size, why)
                                                    : ReadAt(entry.pak, entry.offset + offset, dst, size);
        const DWORD err = ok ? 0 : ::GetLastError();
        m_readsInFlight.fetch_sub(1, std::memory_order_release);
        if (!ok)
        {
            m_pakFailures.fetch_add(1, std::memory_order_relaxed);
            if (offset == 0 && size == entry.size)
                LOG_ERROR("AssetFS: reading '%s' (%llu bytes) from the pak failed (error %lu%s%s)",
                          key.c_str(), static_cast<unsigned long long>(size), err, why ? ", zstd: " : "", why ? why : "");
            else
                LOG_ERROR("AssetFS: reading '%s' (%llu bytes at %llu of %llu) from the pak failed (error %lu%s%s)",
                          key.c_str(), static_cast<unsigned long long>(size), static_cast<unsigned long long>(offset),
                          static_cast<unsigned long long>(entry.size), err, why ? ", zstd: " : "", why ? why : "");
            return false;
        }
        if (m_verifyEntries && entry.hasCrc && offset == 0 && size == entry.size)
        {
            const std::uint32_t crc = Crc32(static_cast<const std::uint8_t*>(dst), size);
            if (crc != entry.crc)
            {
                m_pakFailures.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR("AssetFS: '%s' fails its CRC check (recorded %08X, read %08X) - the pak is damaged",
                          key.c_str(), entry.crc, crc);
                return false;
            }
        }
        m_pakReadNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0).count()),
                              std::memory_order_relaxed);
        m_pakReads.fetch_add(1, std::memory_order_relaxed);
        m_pakBytes.fetch_add(size, std::memory_order_relaxed);
        if (m_recordOrder.load(std::memory_order_relaxed)) NoteFirstUse(key);
        return true;
    }

    bool AssetFS::ReadZstd(const Entry& entry, std::uint64_t offset, void* dst, std::uint64_t size,
                           const char*& why) const
    {
        if (size == 0) return true;
        const PakFile& file = PakOf(entry.pak);
        thread_local std::vector<std::uint8_t> t_packed;
        thread_local std::vector<std::uint8_t> t_block;
        auto* out = static_cast<std::uint8_t*>(dst);
        const std::uint64_t first = offset / entry.blockSize;
        const std::uint64_t last  = (offset + size - 1) / entry.blockSize;
        for (std::uint64_t b = first; b <= last; ++b)
        {
            const std::uint64_t index    = entry.firstBlock + b;
            const std::uint64_t rawStart = b * entry.blockSize;
            const std::uint64_t rawLen   = (std::min)(static_cast<std::uint64_t>(entry.blockSize), entry.size - rawStart);
            t_packed.resize(file.blockStored[index]);
            if (!ReadAt(entry.pak, file.blockStart[index], t_packed.data(), t_packed.size())) return false;
            const std::uint64_t from = (std::max)(offset, rawStart);
            const std::uint64_t to   = (std::min)(offset + size, rawStart + rawLen);
            // A whole block decodes straight into the caller's buffer; one a range cuts, into scratch.
            const bool whole = from == rawStart && to == rawStart + rawLen;
            if (!whole) t_block.resize(rawLen);
            std::uint8_t* target = whole ? out + (rawStart - offset) : t_block.data();
            const std::size_t got = ZSTD_decompress(target, rawLen, t_packed.data(), t_packed.size());
            if (ZSTD_isError(got) || got != rawLen)
            {
                why = ZSTD_isError(got) ? ZSTD_getErrorName(got) : "a block decoded to the wrong size";
                ::SetLastError(ERROR_INVALID_DATA);
                return false;
            }
            if (!whole) std::memcpy(out + (from - offset), t_block.data() + (from - rawStart), to - from);
        }
        return true;
    }

    bool AssetFS::HasInPakExact(const std::string& path) const
    {
        if (m_index.empty()) return false;
        return m_index.find(Normalize(path)) != m_index.end();
    }

    bool AssetFS::HasInPak(const std::string& path) const
    {
        if (m_index.empty()) return false;
        if (m_index.find(Normalize(path)) != m_index.end()) return true;

        // The same relocation retry ReadFile does, so HasInPak never says no to a path
        // ReadFile loads from the pak. A loose build hides the gap (Exists falls through to
        // ResolveOnDisk, which remaps); a packed build has no loose tree, so
        // -scene=asset/EngineResource/World/TitleScene.iscene must find
        // asset/Content/World/TitleScene.iscene here. One extra hash lookup, on the miss
        // path only.
        const std::string moved = RemapRelocatedEnginePath(path);
        return !moved.empty() && m_index.find(moved) != m_index.end();
    }

    bool AssetFS::ReadFile(const std::string& path, std::vector<std::uint8_t>& out) const
    {
        PROF_SCOPE("IO.ReadFile");
        ++LoadCounters::ThisThread().fileReads;
        // Pak path: no allocation except the output copy.
        if (const IndexItem* item = FindEntry(path))
        {
            out.resize(static_cast<std::size_t>(item->second.size));
            // No loose fallback on a failure: the pak wins over every root, and a loose copy
            // would be a file the package does not ship.
            if (ReadEntry(item->first, item->second, 0, out.data(), item->second.size)) return true;
            out.clear();
            return false;
        }

        // Disk fallback (development, or assets added after packing): the search roots,
        // then the path exactly as given - which resolves everything before any root is
        // registered and keeps absolute paths working.
        if (SealedAway(path))
        {
            NoteMiss(path);
            return false;
        }
        const std::string resolved = ResolveOnDisk(path);
        std::ifstream f(Sys::U8Path(resolved.empty() ? path : resolved),
                        std::ios::binary | std::ios::ate);
        if (!f)
        {
            NoteMiss(path);
            return false;
        }
        const std::streamsize sz = f.tellg();
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<std::size_t>(sz));
        if (!f.read(reinterpret_cast<char*>(out.data()), sz))
        {
            NoteMiss(path);
            return false;
        }
        m_looseReads.fetch_add(1, std::memory_order_relaxed);
        m_looseBytes.fetch_add(static_cast<std::uint64_t>(sz), std::memory_order_relaxed);
        NoteLooseUse(path);
        return true;
    }

    AssetFS::Stats AssetFS::GetStats() const
    {
        Stats s;
        s.pakReads   = m_pakReads.load(std::memory_order_relaxed);
        s.pakBytes   = m_pakBytes.load(std::memory_order_relaxed);
        s.pakReadNs  = m_pakReadNs.load(std::memory_order_relaxed);
        s.looseReads = m_looseReads.load(std::memory_order_relaxed);
        s.looseBytes = m_looseBytes.load(std::memory_order_relaxed);
        s.pakFailures = m_pakFailures.load(std::memory_order_relaxed);
        s.misses     = m_misses.load(std::memory_order_relaxed);
        s.asyncReads    = m_asyncReads.load(std::memory_order_relaxed);
        s.prefetches    = m_prefetches.load(std::memory_order_relaxed);
        s.prefetchBytes = m_prefetchBytes.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_missMutex);
        s.firstMisses = m_missSample;
        return s;
    }

    void AssetFS::NoteMiss(const std::string& path) const
    {
        m_misses.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_missMutex);
        if (m_missSample.size() < kMissSample) m_missSample.push_back(path);
    }

    std::uint64_t AssetFS::PakResidentBytes() const
    {
        return m_indexBytes;
    }

    void AssetFS::LogStats(const char* tag) const
    {
        const Stats s = GetStats();
        constexpr double kMiB = 1024.0 * 1024.0;
        // A miss in a sealed package is a probe for an optional file or content the pak lacks: name some.
        std::string first;
        for (const std::string& p : s.firstMisses) first += (first.empty() ? " (first: " : ", ") + p;
        if (!first.empty()) first += ")";
        LOG_INFO("[PAK] %s: entries=%zu resident=%.1f MiB reads=%llu (%.1f MiB, %.1f ms) loose=%llu (%.1f MiB) "
                 "failures=%llu misses=%llu%s", tag, m_index.size(), PakResidentBytes() / kMiB,
                 static_cast<unsigned long long>(s.pakReads), s.pakBytes / kMiB, s.pakReadNs / 1.0e6,
                 static_cast<unsigned long long>(s.looseReads), s.looseBytes / kMiB,
                 static_cast<unsigned long long>(s.pakFailures), static_cast<unsigned long long>(s.misses),
                 first.c_str());
    }

    bool AssetFS::ReadFileText(const std::string& path, std::string& out) const
    {
        // Straight into the string: no vector between the source and the text.
        out.clear();
        const std::optional<std::uint64_t> size = RawSize(path);
        if (!size)
        {
            NoteMiss(path);
            return false;
        }
        out.resize(static_cast<std::size_t>(*size));
        std::size_t got = 0;
        if (!ReadInto(path, out.data(), out.size(), &got))
        {
            out.clear();
            return false;
        }
        out.resize(got);
        return true;
    }

    // ==== Partial and streamed reads ====

    class AssetFS::PakStream final : public AssetFS::Stream
    {
    public:
        PakStream(const AssetFS& fs, const IndexItem& item) : m_fs(fs), m_key(item.first), m_entry(item.second) {}

        std::size_t Read(void* dst, std::size_t size) override
        {
            const std::uint64_t n = (std::min)(static_cast<std::uint64_t>(size), m_entry.size - m_position);
            if (n == 0 || !m_fs.ReadEntry(m_key, m_entry, m_position, dst, n)) return 0;
            m_position += n;
            return static_cast<std::size_t>(n);
        }

        bool Seek(std::uint64_t position) override
        {
            if (position > m_entry.size) return false;
            m_position = position;
            return true;
        }

        std::uint64_t Position() const override { return m_position; }
        std::uint64_t Size() const override { return m_entry.size; }

    private:
        const AssetFS&    m_fs;
        // Copies, not the index's node: a stream outliving Unmount fails its reads, nothing worse.
        const std::string m_key;
        const Entry       m_entry;
        std::uint64_t     m_position = 0;
    };

    namespace
    {
        class LooseStream final : public AssetFS::Stream
        {
        public:
            LooseStream(std::ifstream file, std::uint64_t size) : m_file(std::move(file)), m_size(size) {}

            std::size_t Read(void* dst, std::size_t size) override
            {
                const std::uint64_t n = (std::min)(static_cast<std::uint64_t>(size), m_size - m_position);
                if (n == 0) return 0;
                m_file.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
                const auto got = static_cast<std::uint64_t>((std::max)(m_file.gcount(), std::streamsize(0)));
                m_position += got;
                if (got != n)
                {
                    m_file.clear();
                    m_file.seekg(static_cast<std::streamoff>(m_position));
                }
                return static_cast<std::size_t>(got);
            }

            bool Seek(std::uint64_t position) override
            {
                if (position > m_size) return false;
                m_file.clear();
                m_file.seekg(static_cast<std::streamoff>(position));
                if (!m_file) return false;
                m_position = position;
                return true;
            }

            std::uint64_t Position() const override { return m_position; }
            std::uint64_t Size() const override { return m_size; }

        private:
            std::ifstream m_file;
            std::uint64_t m_size;
            std::uint64_t m_position = 0;
        };

        // The loose half's file for `path`, opened at its start, and its size.
        bool OpenLoose(const AssetFS& fs, const std::string& path, std::ifstream& file, std::uint64_t& size)
        {
            const std::string resolved = fs.ResolveOnDisk(path);
            file.open(Sys::U8Path(resolved.empty() ? path : resolved), std::ios::binary | std::ios::ate);
            if (!file) return false;
            const std::streamsize end = file.tellg();
            if (end < 0) return false;
            size = static_cast<std::uint64_t>(end);
            file.seekg(0, std::ios::beg);
            return static_cast<bool>(file);
        }
    }

    std::optional<std::uint64_t> AssetFS::RawSize(const std::string& path) const
    {
        if (const IndexItem* item = FindEntry(path)) return item->second.size;
        if (SealedAway(path)) return std::nullopt;
        std::error_code ec;
        const std::string resolved = ResolveOnDisk(path);
        const auto size = std::filesystem::file_size(Sys::U8Path(resolved.empty() ? path : resolved), ec);
        if (ec) return std::nullopt;
        return static_cast<std::uint64_t>(size);
    }

    bool AssetFS::ReadRange(const std::string& path, std::uint64_t offset, std::uint64_t size,
                            std::vector<std::uint8_t>& out) const
    {
        PROF_SCOPE("IO.ReadRange");
        ++LoadCounters::ThisThread().fileReads;
        out.clear();
        if (const IndexItem* item = FindEntry(path))
        {
            if (offset > item->second.size) return false;
            const std::uint64_t n = (std::min)(size, item->second.size - offset);
            out.resize(static_cast<std::size_t>(n));
            if (n == 0 || ReadEntry(item->first, item->second, offset, out.data(), n)) return true;
            out.clear();
            return false;
        }
        std::ifstream file;
        std::uint64_t fileSize = 0;
        if (SealedAway(path) || !OpenLoose(*this, path, file, fileSize))
        {
            NoteMiss(path);
            return false;
        }
        if (offset > fileSize) return false;
        const std::uint64_t n = (std::min)(size, fileSize - offset);
        out.resize(static_cast<std::size_t>(n));
        file.seekg(static_cast<std::streamoff>(offset));
        if (n > 0 && !file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)))
        {
            out.clear();
            return false;
        }
        m_looseReads.fetch_add(1, std::memory_order_relaxed);
        m_looseBytes.fetch_add(n, std::memory_order_relaxed);
        NoteLooseUse(path);
        return true;
    }

    std::unique_ptr<AssetFS::Stream> AssetFS::OpenStream(const std::string& path) const
    {
        if (const IndexItem* item = FindEntry(path)) return std::make_unique<PakStream>(*this, *item);
        std::ifstream file;
        std::uint64_t size = 0;
        if (SealedAway(path) || !OpenLoose(*this, path, file, size))
        {
            NoteMiss(path);
            return nullptr;
        }
        NoteLooseUse(path);
        return std::make_unique<LooseStream>(std::move(file), size);
    }

    bool AssetFS::ReadInto(const std::string& path, void* dst, std::size_t capacity, std::size_t* outSize) const
    {
        PROF_SCOPE("IO.ReadInto");
        ++LoadCounters::ThisThread().fileReads;
        if (const IndexItem* item = FindEntry(path))
        {
            if (item->second.size > capacity) return false;
            if (!ReadEntry(item->first, item->second, 0, dst, item->second.size)) return false;
            if (outSize) *outSize = static_cast<std::size_t>(item->second.size);
            return true;
        }
        std::ifstream file;
        std::uint64_t size = 0;
        if (SealedAway(path) || !OpenLoose(*this, path, file, size))
        {
            NoteMiss(path);
            return false;
        }
        if (size > capacity) return false;
        if (size > 0 && !file.read(static_cast<char*>(dst), static_cast<std::streamsize>(size))) return false;
        m_looseReads.fetch_add(1, std::memory_order_relaxed);
        m_looseBytes.fetch_add(size, std::memory_order_relaxed);
        NoteLooseUse(path);
        if (outSize) *outSize = static_cast<std::size_t>(size);
        return true;
    }

    void AssetFS::EnumerateUnder(const std::string& prefix,
                                 std::vector<std::string>& out) const
    {
        // Appends nothing when no pak is mounted: callers fall back to a loose-disk scan.
        const std::string pfx = Normalize(prefix);
        const std::size_t before = out.size();
        for (const auto& kv : m_index)
            if (kv.first.size() >= pfx.size() &&
                kv.first.compare(0, pfx.size(), pfx) == 0)
                out.push_back(kv.first);
        if (out.size() != before) return;

        // Nothing under that prefix: retry it through the (prefix-based) relocation table.
        // A stale directory would otherwise enumerate as an empty vector with no error,
        // which reads as "that folder is empty", not "wrong name".
        const std::string moved = RemapRelocatedEnginePath(pfx);
        if (moved.empty()) return;
        for (const auto& kv : m_index)
            if (kv.first.size() >= moved.size() &&
                kv.first.compare(0, moved.size(), moved) == 0)
                out.push_back(kv.first);
    }

    // ==== Sealed lookups, first-use order ====

    bool AssetFS::SealedAway(const std::string& path) const
    {
        if (!m_sealed || m_index.empty()) return false;
        const std::string key = FoldKey(Normalize(path));
        for (const char* prefix : { "asset/", "engine/", "shaders/" })
            if (key.rfind(prefix, 0) == 0) return true;
        return false;
    }

    void AssetFS::NoteFirstUse(const std::string& key) const
    {
        std::lock_guard<std::mutex> lock(m_orderMutex);
        if (m_orderSeen.insert(key).second) m_order.push_back(key);
    }

    void AssetFS::NoteLooseUse(const std::string& path) const
    {
        if (!m_recordOrder.load(std::memory_order_relaxed)) return;
        const std::string key = Normalize(path);
        if (key.empty() || Sys::U8Path(key).is_absolute()) return;
        NoteFirstUse(key);
    }

    std::vector<std::string> AssetFS::ReadOrder() const
    {
        std::lock_guard<std::mutex> lock(m_orderMutex);
        return m_order;
    }

    // ==== Asynchronous reads ====

    void AssetFS::PushIo(IoRequest request, IoPriority priority) const
    {
        if (m_ioStop) return;
        m_ioQueues[static_cast<std::size_t>(priority)].push_back(std::move(request));
        if (!m_ioThread.joinable()) m_ioThread = std::thread([this] { IoLoop(); });
        m_ioWake.notify_one();
    }

    void AssetFS::ReadAsync(std::string path, IoPriority priority, ReadDone done) const
    {
        std::lock_guard<std::mutex> lock(m_ioMutex);
        PushIo({ std::move(path), std::move(done), false }, priority);
    }

    void AssetFS::Prefetch(std::string path) const
    {
        std::lock_guard<std::mutex> lock(m_ioMutex);
        PushIo({ std::move(path), nullptr, true }, IoPriority::Low);
    }

    void AssetFS::WaitAsyncIdle() const
    {
        std::unique_lock<std::mutex> lock(m_ioMutex);
        if (!m_ioThread.joinable()) return;
        if (std::this_thread::get_id() == m_ioThread.get_id())
        {
            ENGINE_CHECK(false, "AssetFS::WaitAsyncIdle from a ReadAsync callback would wait for itself");
            return;
        }
        m_ioIdle.wait(lock, [this]
        {
            return !m_ioBusy && std::all_of(m_ioQueues.begin(), m_ioQueues.end(),
                                            [](const std::deque<IoRequest>& q) { return q.empty(); });
        });
    }

    void AssetFS::IoLoop() const
    {
        ::SetThreadDescription(::GetCurrentThread(), L"AssetFS IO");
        auto next = [this]() -> std::deque<IoRequest>*
        {
            for (std::size_t p = m_ioQueues.size(); p-- > 0;)
                if (!m_ioQueues[p].empty()) return &m_ioQueues[p];
            return nullptr;
        };
        std::unique_lock<std::mutex> lock(m_ioMutex);
        for (;;)
        {
            m_ioWake.wait(lock, [&] { return m_ioStop || next() != nullptr; });
            if (m_ioStop) return;
            std::deque<IoRequest>* queue = next();
            IoRequest request = std::move(queue->front());
            queue->pop_front();
            m_ioBusy = true;
            lock.unlock();
            if (request.prefetch)
            {
                PrefetchNow(request.path);
                m_prefetches.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                std::vector<std::uint8_t> bytes;
                const bool ok = ReadFile(request.path, bytes);
                m_asyncReads.fetch_add(1, std::memory_order_relaxed);
                try
                {
                    if (request.done) request.done(ok, bytes);
                }
                catch (const std::exception& e)
                {
                    LOG_ERROR("AssetFS: the ReadAsync callback for '%s' threw: %s", request.path.c_str(), e.what());
                }
            }
            lock.lock();
            m_ioBusy = false;
            if (!next()) m_ioIdle.notify_all();
        }
    }

    void AssetFS::PrefetchNow(const std::string& path) const
    {
        constexpr std::uint64_t kPiece = 1ull << 20;
        thread_local std::vector<std::uint8_t> t_piece;
        t_piece.resize(kPiece);
        if (const IndexItem* item = FindEntry(path))
        {
            // The bytes as they lie in the pak: a zstd entry's frames, not decoded.
            const Entry& e = item->second;
            m_readsInFlight.fetch_add(1, std::memory_order_acquire);
            for (std::uint64_t at = 0; at < e.stored;)
            {
                const std::uint64_t n = (std::min)(kPiece, e.stored - at);
                if (!ReadAt(e.pak, e.offset + at, t_piece.data(), n)) break;
                at += n;
                m_prefetchBytes.fetch_add(n, std::memory_order_relaxed);
            }
            m_readsInFlight.fetch_sub(1, std::memory_order_release);
            return;
        }
        std::ifstream file;
        std::uint64_t size = 0;
        if (SealedAway(path) || !OpenLoose(*this, path, file, size)) return;
        while (file.read(reinterpret_cast<char*>(t_piece.data()), static_cast<std::streamsize>(kPiece)) ||
               file.gcount() > 0)
        {
            m_prefetchBytes.fetch_add(static_cast<std::uint64_t>(file.gcount()), std::memory_order_relaxed);
            if (!file) break;
        }
    }
}
