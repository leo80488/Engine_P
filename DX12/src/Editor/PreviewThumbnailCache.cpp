// PreviewThumbnailCache.cpp - see the header for the directory ruling and the
// key.

#include "Editor/PreviewThumbnailCache.h"

#include "ECS/Components.h"
#include "Resource/AssetFS.h"     // Stamp() resolves through the search roots
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <DirectXTex.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace Editor
{
namespace
{
    constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
    constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

    // ONE-FRAME STAT MEMO. A dependency set is up to sixteen paths and a stamp is
    // TWO filesystem calls, so a frame with four lookups and four dependency sets
    // makes up to 256 syscalls, mostly for the same few engine default textures
    // (without the memo a material churn's cpuMs p99 is ~22 ms). Cleared by
    // BeginFrame every frame, so the answer is never stale across frames.
    // Single-threaded: every caller is the game half's serial window.
    std::unordered_map<std::string, PreviewThumbnailCache::Dep> g_statMemo;

    std::string NormaliseKey(std::string p)
    {
        for (char& c : p)
        {
            if (c == '\\') c = '/';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return p;
    }

    std::uint64_t WriteTimeOf(const fs::path& p)
    {
        std::error_code ec;
        const auto t = fs::last_write_time(p, ec);
        if (ec) return 0;
        return static_cast<std::uint64_t>(t.time_since_epoch().count());
    }

    void FnvBytes(std::uint64_t& h, const void* p, std::size_t n)
    {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrime; }
    }
    void FnvStr(std::uint64_t& h, const std::string& s)
    {
        FnvBytes(h, s.data(), s.size());
        h ^= 0xFFu; h *= kFnvPrime;   // separator, so "ab"+"c" != "a"+"bc"
    }
    std::string Hex64(std::uint64_t v)
    {
        static const char* kHex = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 15; i >= 0; --i) { out[i] = kHex[v & 0xFull]; v >>= 4; }
        return out;
    }

    // "<guid>_<stamp>" -> the two halves. The stamp is always 16 hex characters
    // (Hex64), so the split is on the LAST underscore and is unambiguous even
    // though a guid hex string never contains one.
    bool SplitStem(const std::string& stem, std::string& guid, std::string& stamp)
    {
        const std::size_t u = stem.rfind('_');
        if (u == std::string::npos || u == 0 || u + 1 >= stem.size()) return false;
        guid  = stem.substr(0, u);
        stamp = stem.substr(u + 1);
        return !guid.empty() && !stamp.empty();
    }
}

void PreviewThumbnailCache::BeginFrame()
{
    g_statMemo.clear();
}

// ==== the 2D .itex encoder ====
// The layout is TextureImporter's: AssetHeader { magic = MAGIC_TEXTURE,
// version = ASSET_VERSION, resourceType = Texture, metadataSize, dataSize } +
// TextureMetadata + a DirectXTex DDS blob, so a cached tile streams through
// TextureSystem::Acquire and the shipping TextureLoader like any other texture.
bool SavePreviewTileToITEX(const std::uint8_t* rgba, std::uint32_t w,
                           std::uint32_t h, std::uint32_t rowPitch,
                           const std::string& path)
{
    if (!rgba || w == 0 || h == 0) return false;
    // ROW PITCH IS NOT w*4. The source is a D3D12 READBACK buffer whose rows are
    // aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256): a straight memcpy of
    // w*h*4 bytes reads the image skewed, and it looks plausible enough at 128^2
    // to ship.
    const std::uint32_t tight = w * 4u;
    if (rowPitch < tight) return false;

    DirectX::ScratchImage img;
    if (FAILED(img.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1)))
        return false;
    const DirectX::Image* dst = img.GetImage(0, 0, 0);
    if (!dst || !dst->pixels) return false;
    for (std::uint32_t y = 0; y < h; ++y)
        std::memcpy(dst->pixels + y * dst->rowPitch, rgba + y * rowPitch, tight);

    DirectX::Blob dds;
    if (FAILED(DirectX::SaveToDDSMemory(*dst, DirectX::DDS_FLAGS_NONE, dds)))
        return false;

    Resource::TextureMetadata meta{};
    meta.width     = w;
    meta.height    = h;
    meta.depth     = 1;
    meta.mipLevels = 1;
    meta.format    = static_cast<std::uint16_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    meta.dimension = static_cast<std::uint8_t>(DirectX::TEX_DIMENSION_TEXTURE2D);

    Resource::AssetHeader header{};
    header.magic        = Resource::MAGIC_TEXTURE;
    header.version      = Resource::CurrentVersion(Resource::MAGIC_TEXTURE);
    header.resourceType = static_cast<std::uint16_t>(Resource::ResourceType::Texture);
    header.metadataSize = sizeof(Resource::TextureMetadata);
    header.dataSize     = static_cast<std::uint32_t>(dds.GetBufferSize());

    // WRITTEN TO A TEMP AND RENAMED. A tile is read back by
    // TextureSystem::Acquire on a worker thread as soon as the sidecar says it
    // is fresh, and a half-written .itex parses as a truncated DDS - one that
    // stays on disk and is served on every later run. rename() is atomic on
    // NTFS for a same-directory move.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(Sys::U8Path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(&header), sizeof(header));
        f.write(reinterpret_cast<const char*>(&meta),   sizeof(meta));
        f.write(reinterpret_cast<const char*>(dds.GetBufferPointer()),
                static_cast<std::streamsize>(dds.GetBufferSize()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(Sys::U8Path(tmp), Sys::U8Path(path), ec);
    if (ec)
    {
        fs::remove(Sys::U8Path(path), ec);
        fs::rename(Sys::U8Path(tmp), Sys::U8Path(path), ec);
    }
    if (ec) { fs::remove(Sys::U8Path(tmp), ec); return false; }
    return true;
}

// ==== PreviewThumbnailCache ====
PreviewThumbnailCache& PreviewThumbnailCache::Get()
{
    static PreviewThumbnailCache s_inst;
    return s_inst;
}

bool PreviewThumbnailCache::SetDirOverride(const std::string& dir)
{
    if (dir.empty()) return false;
    // TOO LATE ONCE THE DIRECTORY IS LIVE. Moving it after an entry has been
    // written would strand that entry and silently make the session look cold.
    // The flag is parsed before anything touches the cache, so this only fires on
    // a caller ordering mistake.
    if (m_dirResolved) return false;

    std::string d = dir;
    for (char& c : d) if (c == '\\') c = '/';
    // Relative is relative to the artifacts directory (what -artifacts= moves): a
    // pack writes its tiles inside its own artifacts folder instead of into (and
    // clearing) the developer's real cache under WritableDataDir(). ArtifactDir()
    // falls back to WritableDataDir() when no -artifacts was given, so the flag
    // still means something outside the runner.
    const bool absolute = (d.size() >= 2 && d[1] == ':') ||
                          (!d.empty() && d[0] == '/');
    m_dir = absolute ? d : (Sys::ArtifactDir() + "/" + d);
    m_dirResolved = true;
    std::error_code ec;
    fs::create_directories(Sys::U8Path(m_dir), ec);
    LOG_INFO("[PREVIEW] thumbnail cache dir -> '%s' (-preview_thumb_cache_dir)",
             m_dir.c_str());
    return true;
}

const std::string& PreviewThumbnailCache::Dir()
{
    if (!m_dirResolved)
    {
        m_dir = Sys::WritableDataDir() + "/cache/thumbnails";
        m_dirResolved = true;
        std::error_code ec;
        fs::create_directories(Sys::U8Path(m_dir), ec);
        if (ec)
            LOG_WARNING("[PREVIEW] thumbnail cache dir '%s' could not be created "
                        "(%s) - tiles will render every session",
                        m_dir.c_str(), ec.message().c_str());
    }
    return m_dir;
}

std::string PreviewThumbnailCache::ItexPath(const std::string& guidHex,
                                            const std::string& stampHex) const
{
    return m_dir + "/" + guidHex + "_" + stampHex + ".itex";
}
std::string PreviewThumbnailCache::JsonPath(const std::string& guidHex,
                                            const std::string& stampHex) const
{
    return m_dir + "/" + guidHex + "_" + stampHex + ".json";
}

std::string PreviewThumbnailCache::StampHash(const Dep& src,
                                             const std::vector<Dep>& deps)
{
    std::uint64_t h = kFnvOffset;
    const std::uint32_t rv = kRenderVersion;
    FnvBytes(h, &rv, sizeof(rv));
    FnvStr  (h, NormaliseKey(src.path));
    FnvBytes(h, &src.mtime, sizeof(src.mtime));
    FnvBytes(h, &src.size,  sizeof(src.size));
    for (const Dep& d : deps)
    {
        FnvStr  (h, NormaliseKey(d.path));
        FnvBytes(h, &d.mtime, sizeof(d.mtime));
        FnvBytes(h, &d.size,  sizeof(d.size));
    }
    return Hex64(h);
}

PreviewThumbnailCache::Dep PreviewThumbnailCache::Stamp(const std::string& path)
{
    Dep d;
    d.path = path;
    if (path.empty()) return d;
    const auto memo = g_statMemo.find(path);
    if (memo != g_statMemo.end()) return memo->second;

    // ---- resolve through the search roots, not the working directory ----
    // Every dep is a ROOT-RELATIVE path ("engine/textures/x.itex",
    // "asset/Shaders/y.hlsl", "asset/.../M.imat"). A bare fs::last_write_time
    // resolves against the PROCESS working directory, so under -project=<elsewhere>
    // every dep would stamp {0,0} - no information, so no entry would ever be fresh.
    // AssetFS knows where "asset/" and "engine/" actually are.
    std::string full = Resource::AssetFS::Get().ResolveOnDisk(path);
    // Nothing claimed it - fall back to the path AS GIVEN, the only thing that
    // works before any root is registered (AssetFS::Exists does the same). An
    // absolute path lands here too: ResolveOnDisk cannot match one against a root,
    // and using it directly is correct.
    if (full.empty()) full = path;

    std::error_code ec;
    const fs::path p = Sys::U8Path(full);
    d.mtime = WriteTimeOf(p);
    const auto sz = fs::file_size(p, ec);
    d.size = ec ? 0 : static_cast<std::uint64_t>(sz);
    g_statMemo.emplace(path, d);
    return d;
}

std::vector<PreviewThumbnailCache::Dep>
PreviewThumbnailCache::CollectDeps(const std::string& matPath,
                                   const MaterialComponent& mat)
{
    std::vector<Dep> out;
    out.reserve(16);
    auto add = [&out](const std::string& p)
    {
        if (p.empty()) return;
        for (const Dep& d : out) if (d.path == p) return;
        out.push_back(Stamp(p));
    };

    // 1. The material file itself. LoadMaterial resolves the whole .imatinst ->
    //    .imat chain and MaterialSerializer already stamps every chain member,
    //    but the CACHE cannot see those stamps - so the leaf is recorded here
    //    and the chain is covered by the first signal (the editor's save hook
    //    calls MaterialsDependingOn and evicts by guid).
    add(matPath);

    // 2. THE TEXTURES (the engine stamps them nowhere). A re-imported .itex must
    //    invalidate every tile that sampled it, and the material's dependency
    //    stamps only cover chain members.
    for (int i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
        add(mat.textures[i].name);
    for (const auto& kv : mat.customTextures)
        add(kv.second.name);

    // 3. THE CUSTOM SHADER SOURCES (not stamped anywhere either), WHENEVER
    //    NON-EMPTY, not only when `useCustomShader` is set: that toggle can flip
    //    without touching either file and the paths outlive it. An EXTRA dep costs
    //    one re-render; a MISSING one caches a wrong picture nothing can invalidate.
    add(mat.customShaderPath);
    add(mat.customVertexShaderPath);
    return out;
}

void PreviewThumbnailCache::UnhookDeps(const std::string& guidHex)
{
    auto old = m_byGuid.find(guidHex);
    if (old == m_byGuid.end()) return;
    for (const std::string& k : old->second)
    {
        auto it = m_depIndex.find(k);
        if (it != m_depIndex.end())
        {
            it->second.erase(guidHex);
            if (it->second.empty()) m_depIndex.erase(it);
        }
    }
}

void PreviewThumbnailCache::EnsureIndex()
{
    if (m_indexed) return;
    m_indexed = true;
    Dir();
    // ONE directory scan per session. LookupFresh has to find "the file(s) for
    // this guid" and the name carries a stamp hash it cannot compute without the
    // sidecar it is trying to find - so either every lookup scans the directory
    // (hundreds of times a second during a churn) or the mapping is learned once
    // and kept true by Store / Evict / ClearAll. This is the second.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(Sys::U8Path(m_dir), ec))
    {
        std::error_code fec;
        if (!e.is_regular_file(fec)) continue;
        if (Sys::PathToUtf8(e.path().extension()) != ".json") continue;
        std::string guid, stamp;
        if (!SplitStem(Sys::PathToUtf8(e.path().stem()), guid, stamp)) continue;
        m_files[guid].push_back(stamp);
    }
}

void PreviewThumbnailCache::IndexDeps(const std::string& guidHex,
                                      const std::vector<Dep>& deps)
{
    // Unhook the previous rows first, or a material that DROPPED a texture keeps
    // being invalidated by it.
    UnhookDeps(guidHex);
    std::vector<std::string> keys;
    keys.reserve(deps.size());
    for (const Dep& d : deps)
    {
        if (d.path.empty()) continue;
        const std::string k = NormaliseKey(d.path);
        m_depIndex[k].insert(guidHex);
        keys.push_back(k);
    }
    m_byGuid[guidHex] = std::move(keys);
}

std::vector<std::string>
PreviewThumbnailCache::GuidsDependingOn(const std::string& depPath) const
{
    std::vector<std::string> out;
    const auto it = m_depIndex.find(NormaliseKey(depPath));
    if (it == m_depIndex.end()) return out;
    out.assign(it->second.begin(), it->second.end());
    return out;
}

bool PreviewThumbnailCache::LookupFresh(const std::string& guidHex,
                                        const std::string& srcPath,
                                        std::string& outItexPath)
{
    if (guidHex.empty()) return false;
    EnsureIndex();
    const auto fit = m_files.find(guidHex);
    if (fit == m_files.end() || fit->second.empty()) return false;

    // A COPY: the loop drops index rows for files that turned out not to exist.
    const std::vector<std::string> candidates = fit->second;
    std::vector<std::string> keep;
    keep.reserve(candidates.size());
    bool hit = false;
    std::string hitStamp;

    for (const std::string& stamp : candidates)
    {
        if (hit) { keep.push_back(stamp); continue; }
        const std::string itex = ItexPath(guidHex, stamp);
        const std::string json = JsonPath(guidHex, stamp);
        std::error_code ec;
        if (!fs::exists(Sys::U8Path(itex), ec))
            continue;    // sidecar with no tile - dropped from the index here

        std::ifstream f(Sys::U8Path(json), std::ios::binary);
        if (!f) continue;
        nlohmann::json j;
        try { f >> j; }
        catch (...) { continue; }   // truncated / corrupt = MISS, never a hit
        keep.push_back(stamp);

        // renderVersion FIRST: a pipeline change invalidates the whole cache and
        // there is no per-file evidence of it.
        if (j.value("renderVersion", 0u) != kRenderVersion) continue;

        const Dep src = Stamp(srcPath);
        // mtime 0 is "no information" - regenerate rather than trust it.
        if (src.mtime == 0) continue;
        if (j.value("srcMtime", 0ull) != src.mtime) continue;
        if (j.value("srcSize",  0ull) != src.size)  continue;

        std::vector<Dep> deps;
        bool fresh = true;
        if (j.contains("deps") && j["deps"].is_array())
        {
            for (const auto& e : j["deps"])
            {
                Dep d;
                d.path  = e.value("path", std::string());
                d.mtime = e.value("mtime", 0ull);
                d.size  = e.value("size",  0ull);
                deps.push_back(d);
                if (d.path.empty()) continue;
                const Dep now = Stamp(d.path);
                // MTIME 0 IS A MISMATCH ON EITHER SIDE, stated rather than left to the
                // equality: a dep that does not resolve stamps {0,0} NOW, and a sidecar
                // written while it did not resolve stored {0,0} THEN - `now == stored` would
                // keep the entry "fresh" for the life of the cache with no idea what it
                // depends on. Either zero means "no information", and no information has to
                // mean regenerate.
                if (now.mtime == 0 || d.mtime == 0) { fresh = false; break; }
                if (now.mtime != d.mtime || now.size != d.size) { fresh = false; break; }
            }
        }
        if (!fresh) continue;

        IndexDeps(guidHex, deps);
        outItexPath = itex;
        hitStamp = stamp;
        hit = true;
        ++m_hits;
    }
    fit->second = std::move(keep);
    if (!hit) return false;

    // ---- LRU: a HIT touches the file, or the sort in BootGC is FIFO ----
    // BootGC orders by the tile's last_write_time, and a hit never writes it -
    // so a tile used every day would be evicted before an unused one written an
    // hour later. Touched ONCE PER SESSION per guid: "used this session" is all
    // the resolution an LRU across sessions needs, and it keeps a churn's hundreds
    // of hits down to one write syscall per material.
    if (m_touched.insert(guidHex).second)
    {
        std::error_code tec;
        fs::last_write_time(Sys::U8Path(outItexPath),
                            fs::file_time_type::clock::now(), tec);
    }

    NoteStamp(guidHex, hitStamp);
    return true;
}

// The content-addressing evidence (see m_lastStamp): a guid whose tile file
// CHANGED NAME since this session last wrote or served it logs one line, from
// whichever of Store / LookupFresh sees the new stamp first; the pack asserts it.
void PreviewThumbnailCache::NoteStamp(const std::string& guidHex,
                                      const std::string& stampHex)
{
    auto ls = m_lastStamp.find(guidHex);
    if (ls != m_lastStamp.end() && ls->second != stampHex)
        LOG_INFO("[PREVIEW] thumbnail tile rebound: guid=%s stamp=%s -> %s",
                 guidHex.c_str(), ls->second.c_str(), stampHex.c_str());
    m_lastStamp[guidHex] = stampHex;
}

bool PreviewThumbnailCache::Store(const std::string& guidHex,
                                  const std::string& srcPath,
                                  const std::vector<Dep>& deps,
                                  const std::uint8_t* rgba, std::uint32_t w,
                                  std::uint32_t h, std::uint32_t rowPitch)
{
    if (guidHex.empty()) return false;
    EnsureIndex();

    // THE NAME IS THE STAMP STATE (see the header): a fixed name that is rewritten
    // would be re-read from ResourceManager's path cache with the OLD pixels. Two
    // dependency states that hash the same produce the same picture, so reusing
    // the file then is correct rather than stale.
    const Dep src = Stamp(srcPath);
    const std::string stamp = StampHash(src, deps);
    if (!SavePreviewTileToITEX(rgba, w, h, rowPitch, ItexPath(guidHex, stamp)))
        return false;

    nlohmann::json j;
    j["renderVersion"] = kRenderVersion;
    j["stamp"]         = stamp;
    j["srcMtime"]      = src.mtime;
    j["srcSize"]       = src.size;
    j["deps"]          = nlohmann::json::array();
    for (const Dep& d : deps)
        j["deps"].push_back({ { "path", d.path }, { "mtime", d.mtime },
                              { "size", d.size } });

    // SIDECAR LAST, and via a temp+rename like the .itex: LookupFresh trusts the
    // sidecar to describe a COMPLETE tile, so a sidecar that lands before its
    // .itex is the one ordering that can serve a truncated picture.
    const std::string json = JsonPath(guidHex, stamp);
    const std::string tmp  = json + ".tmp";
    {
        std::ofstream f(Sys::U8Path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string text = j.dump();
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(Sys::U8Path(tmp), Sys::U8Path(json), ec);
    if (ec)
    {
        fs::remove(Sys::U8Path(json), ec);
        fs::rename(Sys::U8Path(tmp), Sys::U8Path(json), ec);
    }
    if (ec) { fs::remove(Sys::U8Path(tmp), ec); return false; }

    // ONE PAIR PER GUID IN THE STEADY STATE. The older generations are removed
    // now that the new one is complete on disk - the reverse order (delete then
    // write) would leave a window with no tile at all.
    auto& present = m_files[guidHex];
    for (const std::string& old : present)
    {
        if (old == stamp) continue;
        std::error_code rec;
        fs::remove(Sys::U8Path(ItexPath(guidHex, old)), rec);
        fs::remove(Sys::U8Path(JsonPath(guidHex, old)), rec);
    }
    present.assign(1, stamp);

    IndexDeps(guidHex, deps);
    NoteStamp(guidHex, stamp);
    ++m_writes;
    return true;
}

void PreviewThumbnailCache::Evict(const std::string& guidHex)
{
    if (guidHex.empty() || !m_dirResolved) return;
    EnsureIndex();
    bool went = false;
    auto fit = m_files.find(guidHex);
    if (fit != m_files.end())
    {
        for (const std::string& stamp : fit->second)
        {
            std::error_code ec;
            went |= fs::remove(Sys::U8Path(ItexPath(guidHex, stamp)), ec);
            went |= fs::remove(Sys::U8Path(JsonPath(guidHex, stamp)), ec);
        }
        m_files.erase(fit);
    }
    UnhookDeps(guidHex);
    m_byGuid.erase(guidHex);
    m_touched.erase(guidHex);
    // m_lastStamp IS NOT ERASED. It records "what this guid's tile was called
    // last time", and an Evict is exactly the moment that is about to change -
    // dropping it here would delete the evidence just before it is needed.
    if (went) ++m_evicts;
}

std::size_t PreviewThumbnailCache::ClearAll()
{
    Dir();
    // COLLECTED FIRST, DELETED AFTER. Removing entries while a
    // directory_iterator is walking the same directory is unspecified - the
    // walk may skip files or revisit them - and "the cache was not actually
    // emptied" is exactly the failure a cold pack cannot detect.
    std::vector<fs::path> victims;
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(Sys::U8Path(m_dir), ec))
        {
            std::error_code fec;
            if (!e.is_regular_file(fec)) continue;
            victims.push_back(e.path());
        }
    }
    std::size_t n = 0;
    for (const fs::path& p : victims)
    {
        std::error_code rec;
        if (fs::remove(p, rec)) ++n;
    }
    m_depIndex.clear();
    m_byGuid.clear();
    m_files.clear();
    m_lastStamp.clear();
    m_touched.clear();
    m_indexed = true;   // the directory is empty and this session made it so
    LOG_INFO("[PREVIEW] thumbnail cache cleared: %zu file(s) under '%s'",
             n, m_dir.c_str());
    return n;
}

void PreviewThumbnailCache::BootGC(const std::unordered_set<std::string>& liveGuids,
                                   std::uint64_t sizeCap)
{
    Dir();
    EnsureIndex();

    // ---- phase 1: walk, collecting only - nothing is deleted in this loop ----
    // Removing entries while a directory_iterator is walking the same directory
    // is unspecified behaviour - the walk may skip files it has not seen or
    // revisit ones it has - and the symptom is a GC that silently leaves some of
    // what it was asked to remove, which nothing downstream can notice.
    struct Row
    {
        fs::path      path;
        std::string   guid, stamp, ext;
        std::uint64_t mtime = 0, bytes = 0;
    };
    std::vector<Row> rows;
    std::vector<fs::path> junk;   // .tmp leftovers of an interrupted write
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(Sys::U8Path(m_dir), ec))
        {
            std::error_code fec;
            if (!e.is_regular_file(fec)) continue;
            const std::string ext = Sys::PathToUtf8(e.path().extension());
            if (ext == ".tmp") { junk.push_back(e.path()); continue; }
            if (ext != ".itex" && ext != ".json") continue;
            Row r;
            r.path = e.path();
            r.ext  = ext;
            if (!SplitStem(Sys::PathToUtf8(e.path().stem()), r.guid, r.stamp))
            {
                // A "<guid>.itex" without a stamp (or anything else that does not parse)
                // can never be served by LookupFresh, which only ever asks for
                // "<guid>_<stamp>". Leaving it is a leak.
                junk.push_back(e.path());
                continue;
            }
            std::error_code sec;
            const auto szv = e.file_size(sec);
            r.bytes = sec ? 0ull : static_cast<std::uint64_t>(szv);
            r.mtime = WriteTimeOf(e.path());
            rows.push_back(std::move(r));
        }
    }

    // ---- phase 2: decide ----
    // Pair the two halves of each entry. A .json with no .itex and a .itex with
    // no .json are both DEAD: LookupFresh needs both, so neither can ever be
    // served.
    struct Pair { bool itex = false, json = false; std::uint64_t mtime = 0, bytes = 0; };
    std::unordered_map<std::string, Pair> pairs;   // "<guid>_<stamp>" -> halves
    for (const Row& r : rows)
    {
        Pair& p = pairs[r.guid + "_" + r.stamp];
        if (r.ext == ".itex") { p.itex = true; p.mtime = r.mtime; }
        else                  { p.json = true; }
        p.bytes += r.bytes;
    }

    std::size_t orphans = 0, halves = 0;
    std::vector<fs::path> victims;
    struct Live { std::string guid, stamp; std::uint64_t mtime, bytes; };
    std::vector<Live> live;
    std::uint64_t total = 0;

    for (const Row& r : rows)
    {
        const Pair& p = pairs[r.guid + "_" + r.stamp];
        if (!p.itex || !p.json) { victims.push_back(r.path); continue; }
        // ORPHAN: no asset in the project claims this guid any more. An EMPTY
        // liveGuids means "the index was not built" - no information - so the orphan
        // pass is skipped entirely rather than deleting the whole cache.
        if (!liveGuids.empty() && liveGuids.find(r.guid) == liveGuids.end())
        {
            victims.push_back(r.path);
            continue;
        }
        if (r.ext != ".itex") continue;    // count each entry once
        total += p.bytes;
        live.push_back({ r.guid, r.stamp, p.mtime, p.bytes });
    }
    // Count entries, not files: each contributes one .itex and one .json.
    for (const auto& kv : pairs)
    {
        if (!kv.second.itex || !kv.second.json) { ++halves; continue; }
        const std::size_t u = kv.first.rfind('_');
        const std::string guid = (u == std::string::npos) ? kv.first
                                                          : kv.first.substr(0, u);
        if (!liveGuids.empty() && liveGuids.find(guid) == liveGuids.end())
            ++orphans;
    }

    std::size_t lru = 0;
    if (total > sizeCap)
    {
        // LEAST-RECENTLY-USED first, down to HALF the cap: trimming to exactly
        // the cap makes the next session's first write trigger another full
        // sweep. The sort key is the .itex's last_write_time, which LookupFresh
        // bumps on a hit (once per session) - without that bump this would be
        // FIFO by CREATION.
        std::sort(live.begin(), live.end(),
                  [](const Live& a, const Live& b) { return a.mtime < b.mtime; });
        const std::uint64_t target = sizeCap / 2;
        for (const Live& e : live)
        {
            if (total <= target) break;
            victims.push_back(Sys::U8Path(ItexPath(e.guid, e.stamp)));
            victims.push_back(Sys::U8Path(JsonPath(e.guid, e.stamp)));
            total -= (e.bytes < total) ? e.bytes : total;
            ++lru;
        }
    }

    // ---- phase 3: delete, with the walk finished ----
    std::size_t removed = 0;
    for (const fs::path& p : junk)    { std::error_code ec; if (fs::remove(p, ec)) ++removed; }
    for (const fs::path& p : victims) { std::error_code ec; if (fs::remove(p, ec)) ++removed; }

    // The index was built from the directory this walk just changed; rebuild it
    // rather than patch it - it is one scan and this runs once per session.
    if (removed)
    {
        m_files.clear();
        m_indexed = false;
        EnsureIndex();
    }

    if (orphans || lru || halves || !junk.empty())
        LOG_INFO("[PREVIEW] thumbnail cache GC: %zu orphan(s), %zu half-written, "
                 "%zu stray file(s), %zu LRU eviction(s), %zu file(s) removed, "
                 "%llu B remaining",
                 orphans, halves, junk.size(), lru, removed,
                 static_cast<unsigned long long>(total));
}

} // namespace Editor
