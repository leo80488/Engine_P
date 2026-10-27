// AssetQuery.cpp - the Lua `Asset` table (contract in the header): what the running engine
// holds of an asset - loaded or not, ready or why it failed, and the entities it is resolved for.
#include "Renderer/AssetQuery.h"

#include "ECS/AnimationComponents.h"      // AnimationSetComponent
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"      // Material / Scene / Prefab / Mesh source paths
#include "Renderer/Renderer.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"
#include "Resource/AssetHeader.h"      // Asset.get: the file's format and version
#include "Resource/AssetManager.h"
#include "Resource/MaterialSerializer.h"  // MaterialsDependingOn
#include "Resource/MeshLibIdentity.h"     // Asset.meshes: key spelling, MESH_IDENT_*
#include "Resource/ModelCollision.h"      // Asset.meshes: each Mesh's asset collision
#include "Resource/ResourceManager.h"
#include "Scene/MeshAssetRegistry.h"      // Asset.meshes
#include "Resource/TextureSystem.h"
#include "Resource/TextureInfo.h"     // Asset.textureInfo
#include "Resource/TextureImportRecord.h"   // Asset.textureInfo(..).import
#include "Graphics/FormatNames.h"
#include "Scripting/LuaDoc.h"
#include "Scripting/LuaEntityId.h"
#include "System/PathUtil.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr size_t kMaxUsers = 256;

    // Separators and case folded: how the engine's file names compare (Windows).
    std::string Fold(std::string s)
    {
        for (char& c : s)
        {
            if (c == '\\') c = '/';
            else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string Canon(const std::string& p)
    {
        std::string c = Sys::RelativizeAssetPath(p);
        for (char& ch : c) if (ch == '\\') ch = '/';
        return c;
    }

    std::string TypeOf(const std::string& path)
    {
        const size_t slash = path.find_last_of('/');
        const size_t dot   = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
        return Fold(path.substr(dot + 1));
    }

    const char* StateName(Resource::ResourceState s)
    {
        switch (s)
        {
        case Resource::ResourceState::Ready:   return "ready";
        case Resource::ResourceState::Loading: return "loading";
        case Resource::ResourceState::Failed:  return "failed";
        default:                               return "not_loaded";
        }
    }

    // The entities whose component of type T carries a path equal to one of @p want (folded).
    template <typename T>
    void UsersByPath(World& w, const std::unordered_set<std::string>& want, std::vector<Entity>& out)
    {
        auto* pool = w.GetPool<T>();
        if (!pool) return;
        const auto& entities = pool->Entities();
        const auto& data     = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
            if (w.IsAlive(entities[i]) && !data[i].path.empty() && want.count(Fold(Canon(data[i].path))))
                out.push_back(entities[i]);
    }

    sol::table Get(sol::this_state ts, World& world, Renderer& renderer, Resource::AssetManager& assets,
                   const std::string& pathIn)
    {
        sol::state_view lua(ts);
        const std::string path = Canon(pathIn);
        const std::string type = TypeOf(path);
        sol::table out = lua.create_table();
        out["path"] = path;
        out["type"] = type;
        // Pak, a search root or the shared content root: where a load would find it.
        out["exists"] = Resource::AssetFS::Get().Exists(path);
        // The header's format and version, read from the file (a loader's own refusal of it is the
        // resource / texture reason below).
        {
            std::vector<std::uint8_t> head;
            Resource::AssetHeader     hdr{};
            if (Resource::AssetFS::Get().ReadRange(path, 0, sizeof(hdr), head) && head.size() == sizeof(hdr))
            {
                std::memcpy(&hdr, head.data(), sizeof(hdr));
                if (const Resource::AssetFormat* f = Resource::FindAssetFormat(hdr.magic))
                {
                    sol::table format = lua.create_table();
                    format["kind"]    = f->kind;
                    format["magic"]   = Resource::MagicText(hdr.magic);
                    format["version"] = hdr.version;
                    format["current"] = f->current;
                    format["minimum"] = f->minimum;
                    format["status"]  = Resource::VersionStatus(*f, hdr.version);
                    if (hdr.version > f->current || hdr.version < f->minimum)
                        format["reason"] = Resource::DescribeHeaderCheck(
                            Resource::CheckHeader(head.data(), head.size(), hdr.magic));
                    out["format"] = format;
                }
            }
        }
        if (const ECS::Guid g = Resource::AssetGuidIndex::Get().GuidForPath(path); g.IsValid())
            out["guid"] = g.ToHex();

        // The ResourceManager's entry, when it holds one - it loads what the
        // TextureSystem asks for (and its like); materials, prefabs and scenes
        // have loaders of their own, so no entry is not "not loaded" for them.
        Resource::ResourceManager* rm = assets.GetResourceManager();
        Resource::ResourceManager::EntryInfo info;
        if (rm && rm->Find(path, info))
        {
            sol::table res = lua.create_table();
            res["state"] = StateName(info.state);
            if (!info.failReason.empty()) res["reason"] = info.failReason;
            out["resource"] = res;
        }
        // A material: the material cache (every resolve reads from it).
        if (type == "imat" || type == "imatinst")
            out["material"] = lua.create_table_with("cached", Resource::IsMaterialCached(path));

        // A texture: the GPU side of it.
        Resource::TextureSystem* texSys = assets.GetTextureSystem();
        if (type == "itex" && texSys && rm)
            if (const Resource::TextureHandle h = texSys->Find(path); h.IsValid())
            {
                using State = Resource::TextureSystem::Status::State;
                const Resource::TextureSystem::Status st = texSys->GetStatus(h, *rm);
                Resource::TextureSystem::Info ti;
                texSys->Describe(h, ti);
                sol::table t = lua.create_table();
                t["state"]  = st.state == State::Ready ? "ready" : st.state == State::Failed ? "failed" : "loading";
                if (!st.reason.empty()) t["reason"] = st.reason;
                t["width"]  = ti.width;
                t["height"] = ti.height;
                t["refs"]   = ti.refCount;
                if (ti.gpuReady)
                {
                    // What the GPU copy is.
                    t["format"]    = RHI::FormatName(ti.format);
                    t["mips"]      = ti.mipLevels;
                    t["arraySize"] = ti.arraySize;
                    t["cube"]      = ti.isCube;
                    t["gpuBytes"]  = ti.gpuBytes;
                    t["revision"]  = ti.revision;
                }
                out["texture"] = t;
            }

        // Who it is resolved for.
        std::vector<Entity> users;
        const std::unordered_set<std::string> self{ Fold(path) };
        if (type == "itex")
            renderer.TextureConsumers(path, users);
        else if (type == "imat" || type == "imatinst")
        {
            // The file itself, and every material whose chain reads it (an
            // instance of it, at any depth) - the serializer's reverse index.
            std::unordered_set<std::string> want = self;
            for (const std::string& m : Resource::MaterialsDependingOn(path)) want.insert(Fold(Canon(m)));
            UsersByPath<MaterialSourcePath>(world, want, users);
        }
        else if (type == "iscn")    UsersByPath<SceneSourcePath>(world, self, users);
        else if (type == "ipfb")    UsersByPath<PrefabSourcePath>(world, self, users);
        else if (type == "meshlib") UsersByPath<MeshSourcePath>(world, self, users);
        else if (type == "ianim")
        {
            if (auto* pool = world.GetPool<AnimationSetComponent>())
            {
                const auto& entities = pool->Entities();
                const auto& sets     = pool->Data();
                for (size_t i = 0; i < sets.size(); ++i)
                    if (world.IsAlive(entities[i]) &&
                        std::any_of(sets[i].clips.begin(), sets[i].clips.end(), [&](const AnimationClipReference& c)
                                    { return !c.path.empty() && Fold(Canon(c.path)) == *self.begin(); }))
                        users.push_back(entities[i]);
            }
        }
        std::sort(users.begin(), users.end());
        users.erase(std::unique(users.begin(), users.end()), users.end());
        sol::table u = lua.create_table();
        for (size_t i = 0; i < users.size() && i < kMaxUsers; ++i) u[i + 1] = LuaId::Out(world, users[i]);
        out["users"]      = u;
        out["usersTotal"] = users.size();
        return out;
    }

    // Asset.textureInfo(path): the file's own description (Resource/TextureInfo.h) -
    // no load, no upload, no reference taken; works on any .itex, loaded or not.
    sol::table TextureInfoTable(sol::this_state ts, const std::string& path)
    {
        sol::state_view lua(ts);
        const Resource::TextureFileInfo i = Resource::DescribeTextureFile(path);
        sol::table t = lua.create_table();
        t["ok"]   = i.ok;
        t["path"] = i.path;
        if (!i.ok)
        {
            t["error"] = i.error;
            return t;
        }
        if (!i.guid.empty()) t["guid"] = i.guid;
        t["fileBytes"]    = i.fileBytes;
        t["packed"]       = i.packed;
        t["width"]        = i.width;
        t["height"]       = i.height;
        t["depth"]        = i.depth;
        t["arraySize"]    = i.arraySize;
        t["mipLevels"]    = i.mipLevels;
        t["dimension"]    = i.dimension;
        t["isCube"]       = i.isCube;
        t["cubeCount"]    = i.cubeCount;
        t["dxgiFormat"]   = i.dxgiFormat;
        t["format"]       = i.formatName;
        t["formatLabel"]  = i.formatLabel;
        t["srgb"]         = i.isSRGB;
        t["compressed"]   = i.isCompressed;
        t["bitsPerPixel"] = i.bitsPerPixel;
        t["blockSize"]    = i.blockSize;
        t["alphaMode"]    = i.alphaMode;
        t["dataBytes"]    = i.dataBytes;
        t["preset"]       = i.preset;
        t["summary"]      = Resource::TextureInfoSummary(i);
        // How it was made: the .meta's import record, or (recorded = false) a guess for a file whose
        // .meta has no record.
        {
            const Resource::TextureImportRecord rec = Resource::TextureImportRecordFor(path);
            sol::table im = lua.create_table();
            im["recorded"]  = rec.recorded;
            im["known"]     = rec.known;
            im["source"]    = rec.source;
            if (rec.known) im["format"] = std::string(Resource::TextureImporter::GetFormatId(rec.settings.format));
            im["maxSize"]   = rec.settings.maxSize;
            im["mipFilter"] = std::string(Resource::TextureImporter::GetMipFilterId(rec.settings.mipFilter));
            im["flipGreen"] = rec.settings.flipGreen;
            im["mips"]      = rec.settings.generateMips;
            t["import"] = im;
        }
        t["headerDimension"] = i.headerDimension;
        if (!i.headerMismatch.empty()) t["headerMismatch"] = i.headerMismatch;
        sol::table mips = lua.create_table();
        for (size_t m = 0; m < i.mips.size(); ++m)
        {
            const Resource::TextureFileInfo::Mip& mi = i.mips[m];
            sol::table r = lua.create_table();
            r["level"]      = m;
            r["width"]      = mi.width;
            r["height"]     = mi.height;
            r["depth"]      = mi.depth;
            r["rowPitch"]   = mi.rowPitch;
            r["slicePitch"] = mi.slicePitch;
            r["bytes"]      = mi.bytes;
            mips[m + 1] = r;
        }
        t["mips"] = mips;
        return t;
    }

    sol::table Loaded(sol::this_state ts, Resource::AssetManager& assets, const sol::optional<sol::table>& query)
    {
        sol::state_view lua(ts);
        std::string wantType, wantState;
        size_t limit = 1000;
        if (query)
        {
            wantType  = Fold(query->get_or<std::string>("type", ""));
            if (!wantType.empty() && wantType[0] == '.') wantType.erase(0, 1);
            wantState = Fold(query->get_or<std::string>("state", ""));
            const int l = query->get_or("limit", 1000);
            if (l < 1) throw sol::error("Asset.loaded: bad_value: limit wants a number >= 1");
            limit = static_cast<size_t>(l);
            if (!wantState.empty() && wantState != "ready" && wantState != "loading" && wantState != "failed" &&
                wantState != "not_loaded")
                throw sol::error("Asset.loaded: bad_value: state is ready, loading, failed or not_loaded");
        }
        sol::table rows = lua.create_table();
        size_t count = 0, n = 0;
        if (Resource::ResourceManager* rm = assets.GetResourceManager())
            for (const Resource::ResourceManager::EntryInfo& e : rm->Snapshot())
            {
                const std::string path = Canon(e.path);
                const std::string type = TypeOf(path);
                const char*       st   = StateName(e.state);
                if (!wantType.empty() && type != wantType) continue;
                if (!wantState.empty() && wantState != st) continue;
                ++count;
                if (n >= limit) continue;
                sol::table r = lua.create_table();
                r["path"]  = path;
                r["type"]  = type;
                r["state"] = st;
                if (!e.failReason.empty()) r["reason"] = e.failReason;
                rows[++n] = r;
            }
        sol::table out = lua.create_table();
        out["rows"]      = rows;
        out["count"]     = count;
        out["truncated"] = count > n;
        return out;
    }
}

void AssetQuery::RegisterLua(lua_State* L, World& world, Renderer& renderer, Resource::AssetManager& assets,
                             std::function<void()> drain)
{
    if (!L) return;
    sol::state_view lua(L);
    sol::table asset = lua["Asset"].get_or_create<sol::table>();
    const std::thread::id mainThread = std::this_thread::get_id();
    World* w = &world;
    Renderer* r = &renderer;
    Resource::AssetManager* a = &assets;
    auto mainOnly = [mainThread](const char* fn)
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error(std::string("Asset.") + fn + ": wrong_thread: call it from the main thread (a "
                             "control-channel run, a scenario driver)");
    };
    LuaDoc::Fn(asset, "Asset", "pakStats", [](sol::this_state ts) -> sol::table
    {
        const Resource::AssetFS& fs = Resource::AssetFS::Get();
        const Resource::AssetFS::Stats s = fs.GetStats();
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["mounted"]       = fs.IsMounted();
        t["version"]       = fs.PakVersion();
        t["buildId"]       = fs.PakBuildId();
        t["contentId"]     = fs.PakContentId();
        t["entries"]       = fs.PakEntryCount();
        t["patches"]       = fs.PatchCount();
        t["residentBytes"] = fs.PakResidentBytes();
        t["sealed"]        = fs.Sealed();
        t["reads"]         = s.pakReads;
        t["bytes"]         = s.pakBytes;
        t["readMs"]        = s.pakReadNs / 1.0e6;
        t["looseReads"]    = s.looseReads;
        t["looseBytes"]    = s.looseBytes;
        t["failures"]      = s.pakFailures;
        t["misses"]        = s.misses;
        sol::table first = lua.create_table();
        int i = 0;
        for (const std::string& p : s.firstMisses) first[++i] = p;
        t["firstMisses"]   = first;
        t["asyncReads"]    = s.asyncReads;
        t["prefetches"]    = s.prefetches;
        t["prefetchBytes"] = s.prefetchBytes;
        return t;
    },
    { .doc = "The mounted pak (version, buildId, contentId, entries, patches, residentBytes, sealed) and every "
             "read since the process started: reads / bytes / readMs from the pak, looseReads / looseBytes, "
             "failures, misses with the first few in firstMisses, asyncReads, prefetches, prefetchBytes." });
    asset.set_function("get", [w, r, a, drain, mainOnly](sol::this_state ts, const std::string& path) -> sol::table
    {
        mainOnly("get");
        if (path.empty()) throw sol::error("Asset.get: bad_value: a path");
        // The renderer's runtime materials are written by the scene build.
        if (drain) drain();
        return Get(ts, *w, *r, *a, path);
    });
    // Asset.textures{sort, limit, state, minBytes, uncompressed, singleMip, users} - every texture
    // the TextureSystem holds, the VRAM audit (the editor's Window > Analysis > Loaded Textures
    // shows the same rows).
    asset.set_function("textures", [r, a, drain, mainOnly](sol::this_state ts, sol::optional<sol::table> query) -> sol::table
    {
        mainOnly("textures");
        sol::state_view lua(ts);
        Resource::TextureSystem*   texSys = a->GetTextureSystem();
        Resource::ResourceManager* rm     = a->GetResourceManager();
        if (!texSys || !rm) throw sol::error("Asset.textures: unavailable: no texture system");
        std::string sort = "bytes", wantState;
        std::size_t limit = 1000;
        std::uint64_t minBytes = 0;
        bool uncompressed = false, singleMip = false, users = false;
        if (query)
        {
            sort      = query->get_or<std::string>("sort", "bytes");
            wantState = Fold(query->get_or<std::string>("state", ""));
            const int l = query->get_or("limit", 1000);
            if (l < 1) throw sol::error("Asset.textures: bad_value: limit wants a number >= 1");
            limit        = static_cast<std::size_t>(l);
            minBytes     = static_cast<std::uint64_t>((std::max)(0.0, query->get_or("minBytes", 0.0)));
            uncompressed = query->get_or("uncompressed", false);
            singleMip    = query->get_or("singleMip", false);
            users        = query->get_or("users", false);
            if (sort != "bytes" && sort != "path" && sort != "refs" && sort != "size")
                throw sol::error("Asset.textures: bad_value: sort is bytes, path, refs or size");
            if (!wantState.empty() && wantState != "ready" && wantState != "loading" && wantState != "failed")
                throw sol::error("Asset.textures: bad_value: state is ready, loading or failed");
        }
        std::unordered_map<std::uint64_t, std::uint32_t> userCounts;
        if (users)
        {
            if (drain) drain();
            r->TextureConsumerCounts(userCounts);
        }
        using State = Resource::TextureSystem::Status::State;
        std::vector<Resource::TextureSystem::EntryView> all = texSys->Snapshot(*rm);
        std::vector<const Resource::TextureSystem::EntryView*> rows;
        std::uint64_t totalBytes = 0;
        sol::table byFormat = lua.create_table();
        for (const Resource::TextureSystem::EntryView& e : all)
        {
            const char* st = e.status.state == State::Ready ? "ready" : e.status.state == State::Failed ? "failed" : "loading";
            const std::string fmt = RHI::FormatName(e.info.format);
            const bool bc = fmt.rfind("BC", 0) == 0;
            if (!wantState.empty() && wantState != st) continue;
            if (e.info.gpuBytes < minBytes) continue;
            if (uncompressed && (bc || e.status.state != State::Ready)) continue;
            if (singleMip && !(e.info.gpuReady && e.info.mipLevels == 1 && (e.info.width > 1 || e.info.height > 1))) continue;
            rows.push_back(&e);
            totalBytes += e.info.gpuBytes;
            if (e.info.gpuReady)
            {
                sol::table f = byFormat[fmt].get_or_create<sol::table>();
                f["count"] = f.get_or("count", 0) + 1;
                f["bytes"] = f.get_or("bytes", 0.0) + static_cast<double>(e.info.gpuBytes);
            }
        }
        std::sort(rows.begin(), rows.end(), [&sort](const auto* x, const auto* y)
        {
            if (sort == "path") return Canon(x->path) < Canon(y->path);
            if (sort == "refs") return x->info.refCount > y->info.refCount;
            if (sort == "size") return uint64_t(x->info.width) * x->info.height > uint64_t(y->info.width) * y->info.height;
            return x->info.gpuBytes > y->info.gpuBytes;
        });
        sol::table list = lua.create_table();
        std::size_t n = 0;
        for (const auto* e : rows)
        {
            if (n >= limit) break;
            sol::table t = lua.create_table();
            t["path"]      = Canon(e->path);
            t["width"]     = e->info.width;
            t["height"]    = e->info.height;
            t["depth"]     = e->info.depth;
            t["format"]    = RHI::FormatName(e->info.format);
            t["mips"]      = e->info.mipLevels;
            t["arraySize"] = e->info.arraySize;
            t["cube"]      = e->info.isCube;
            t["gpuBytes"]  = static_cast<double>(e->info.gpuBytes);
            t["refs"]      = e->info.refCount;
            t["state"]     = e->status.state == State::Ready ? "ready" : e->status.state == State::Failed ? "failed" : "loading";
            if (!e->status.reason.empty()) t["reason"] = e->status.reason;
            if (users)
            {
                const auto it = userCounts.find(Renderer::TextureUserKey(e->path));
                t["users"] = it != userCounts.end() ? it->second : 0u;
            }
            list[++n] = t;
        }
        sol::table out = lua.create_table();
        out["rows"]       = list;
        out["count"]      = rows.size();
        out["truncated"]  = rows.size() > n;
        out["totalBytes"] = static_cast<double>(totalBytes);
        out["byFormat"]   = byFormat;
        return out;
    });
    // Asset.textureInfo(path) - what a .itex file is (format, size, mips, cube, per-mip bytes),
    // read from its DDS header.
    asset.set_function("textureInfo", [](sol::this_state ts, const std::string& path) -> sol::table
    {
        if (path.empty()) throw sol::error("Asset.textureInfo: bad_value: a path");
        return TextureInfoTable(ts, path);
    });
    asset.set_function("loaded", [a, mainOnly](sol::this_state ts, sol::optional<sol::table> query) -> sol::table
    {
        mainOnly("loaded");
        return Loaded(ts, *a, query);
    });
    // Asset.meshLibraries() - every loaded .meshlib and who holds it: the rows the editor's Loaded
    // Geometry window shows (Renderer::DescribeLoadedMeshLibraries).
    asset.set_function("meshLibraries", [w, r, mainOnly](sol::this_state ts) -> sol::table
    {
        mainOnly("meshLibraries");
        sol::state_view lua(ts);
        sol::table rows = lua.create_table();
        int i = 0;
        for (const Renderer::LoadedMeshLibInfo& l : r->DescribeLoadedMeshLibraries(w))
        {
            sol::table t = lua.create_table();
            t["path"]           = l.path;
            t["refs"]           = l.refs;
            t["entries"]        = l.entries;
            t["meshes"]         = l.meshes;
            t["vertices"]       = l.vertices;
            t["indices"]        = l.indices;
            t["stride"]         = l.stride;
            t["vramBytes"]      = static_cast<double>(l.vramBytes);
            t["worldRefs"]      = l.worldRefs;
            t["preview"]        = l.preview;
            t["editorPinned"]   = l.editorPinned;
            t["pendingRelease"] = l.pendingRelease;
            t["users"]          = l.users;
            t["identity"]       = std::string(l.identity);
            // The library's resident meshlets.
            t["meshlets"]         = std::string(l.meshlets);
            t["meshletClusters"]  = l.meshletClusters;
            t["meshletLevels"]    = l.meshletLevels;
            t["meshletMaxLevels"] = l.meshletMaxLevels;
            t["meshletBytes"]     = static_cast<double>(l.meshletBytes);
            rows[++i] = t;
        }
        return rows;
    });
    // Asset.previewComposition(path) - what the asset preview draws for @p path
    // (Renderer::ComposePreviewAsset: an .ipfb / .iscn / .meshlib or a Mesh ref): one row per
    // placement {mesh, entry, hint, key?, material, node, ordinal, primitive?}. `entry` is the one
    // drawn - the F-line's key first - and `material` the part's .imat after a prefab's own
    // MaterialRef. nil while it does not compose (a library still opening: ask again next frame).
    asset.set_function("previewComposition", [r, mainOnly](sol::this_state ts, const std::string& path) -> sol::object
    {
        mainOnly("previewComposition");
        if (path.empty()) throw sol::error("Asset.previewComposition: bad_value: an asset path");
        sol::state_view lua(ts);
        const Preview::AssetComposition* comp = r->ComposePreviewAsset(path, Preview::AssetKindForPath(path));
        if (!comp) return sol::lua_nil;
        sol::table rows = lua.create_table();
        int i = 0;
        for (const Preview::AssetPlacement& p : comp->placements)
        {
            uint32_t entry = p.meshId;
            if (p.sectionKey != 0)
                if (const auto kt = Resource::MeshLibKeyTableAt(p.meshPath))
                    if (const int e = kt->Find(p.sectionKey); e >= 0) entry = static_cast<uint32_t>(e);
            sol::table t = lua.create_table();
            t["mesh"]     = p.meshPath;
            t["entry"]    = entry;
            t["hint"]     = p.meshId;
            if (p.sectionKey != 0) t["key"] = Resource::MeshKeyToHex(p.sectionKey);
            t["material"] = p.matPath;
            t["node"]     = p.sceneNodeIdx;
            t["ordinal"]  = p.dfsIndex;
            if (p.primitiveId >= 0) t["primitive"] = p.primitiveId;
            rows[++i] = t;
        }
        return sol::make_object(lua, rows);
    });
    // Asset.meshes(model) - the Meshes a model's libraries name: what a StaticMesh can place, keyed
    // as it keys them.
    asset.set_function("meshes", [w, r, mainOnly](sol::this_state ts, const std::string& model) -> sol::table
    {
        mainOnly("meshes");
        if (model.empty()) throw sol::error("Asset.meshes: bad_value: a model (.iscn) path");
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        MeshAssetRegistry* reg = r->GetMeshAssets();
        if (!reg) throw sol::error("Asset.meshes: unavailable: no mesh library");
        const MeshAssetRegistry::Model& m = reg->GetModel(model, w->Epoch());
        out["model"] = m.path;
        out["namesSkeleton"] = m.namesSkeleton;
        if (!m.error.empty()) out["error"] = m.error;
        sol::table meshes = lua.create_table();
        int i = 0;
        for (const MeshAssetRegistry::Mesh& mesh : m.meshes)
        {
            sol::table t = lua.create_table();
            t["key"]         = Resource::MeshKeyToHex(mesh.key);
            t["name"]        = mesh.name;
            t["flags"]       = mesh.flags;
            t["hidden"]      = (mesh.flags & Resource::MESH_IDENT_HIDDEN) != 0;
            // Per Mesh (Resource::MeshCountsAsSkinned): the identity's flag; for
            // an index-derived identity, the model naming a skeleton.
            t["skinned"]     = mesh.skinned;
            t["library"]     = Resource::MeshLibCanonicalPath(mesh.libraryPath);
            t["synthesized"] = mesh.synthesized;
            // The Mesh's asset collision (its model's BO / PR records), when it has one.
            if (const Resource::MeshBodySetup* cs = Resource::ModelCollisionAt(model)->Find(mesh.key))
            {
                sol::table c = lua.create_table();
                c["complexity"] = cs->complexity == Resource::MeshBodySetup::Complexity::Complex ? "complex"
                                : cs->complexity == Resource::MeshBodySetup::Complexity::None    ? "none" : "simple";
                c["layer"]   = cs->layer;
                c["trigger"] = cs->trigger;
                c["body"]    = cs->HasBody();
                int nBody = 0, nSection = 0;
                for (const Resource::CollisionPrim& p : cs->prims) (p.InBody() ? nBody : nSection) += 1;
                c["bodyPrims"]    = nBody;
                c["sectionPrims"] = nSection;
                t["collision"] = c;
            }
            sol::table secs = lua.create_table();
            int k = 0;
            for (const MeshAssetRegistry::Section& s : mesh.sections)
            {
                sol::table st = lua.create_table();
                st["entry"]    = s.entry;
                st["key"]      = Resource::MeshKeyToHex(s.key);
                st["slot"]     = s.slot;
                st["material"] = s.defaultMaterial;
                secs[++k] = st;
            }
            t["sections"] = secs;
            meshes[++i] = t;
        }
        out["meshes"] = meshes;
        return out;
    });
}
