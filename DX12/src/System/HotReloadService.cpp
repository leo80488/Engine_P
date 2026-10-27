// HotReload::WatchService: see System/HotReloadService.h.

#include "System/HotReloadService.h"
#include "Resource/AssetTypes.generated.h"

#include "ECS/AnimGraphSystem.h"
#include "ECS/ECS.h"
#include "Gameplay/DataTable.h"
#include "Graphics/IGraphicsDevice.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetFS.h"
#include "Resource/MaterialOverrideApply.h"   // ReloadMaterialsAffectedBy
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Scene/GameModeContext.h"
#include "System/HostPolicy.h"                // Sys::HotReloadEnabled
#include "System/HotReloadReceipt.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "UI/UIAsset.h"                       // InvalidateUIAsset (.iui)

#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    std::vector<std::pair<std::string, HotReload::AssetHandler>>& AssetHandlers()
    {
        static std::vector<std::pair<std::string, HotReload::AssetHandler>> s_handlers;
        return s_handlers;
    }

    HotReload::AssetHandler FindAssetHandler(const std::string& ext)
    {
        for (const auto& [e, handler] : AssetHandlers())
            if (e == ext) return handler;
        return nullptr;
    }
}

void HotReload::RegisterAssetHandler(const char* ext, AssetHandler handler)
{
    AssetHandlers().emplace_back(ext, handler);
}

void HotReload::WatchService::Start()
{
    if (!::Resource::AssetFS::Get().IsMounted())
    {
        // Anchored to the ROOTS, not the working directory: engine shaders live
        // with the engine and custom material shaders with the project, and a
        // relative path would depend on where the process was started.
        //
        // Probe before Init in both cases: neither directory is guaranteed (a
        // project may keep no loose custom shaders at all), and Init LOG_ERRORs
        // on a missing one.
        std::error_code dirEc;
        const std::string engineShaders  = Sys::EngineDir()  + "/shaders";
        const std::string projectShaders = Sys::ProjectDir() + "/asset/Shaders";
        if (std::filesystem::is_directory(Sys::U8Path(engineShaders), dirEc))
        {
            if (shaderWatchers[0].Init(engineShaders)) HotReload::NoteWatchedShaderRoot(kShaderWatchPrefix[0]);
            shaderWatchRoot[0] = engineShaders;
        }
        if (std::filesystem::is_directory(Sys::U8Path(projectShaders), dirEc))
        {
            if (shaderWatchers[1].Init(projectShaders)) HotReload::NoteWatchedShaderRoot(kShaderWatchPrefix[1]);
            shaderWatchRoot[1] = projectShaders;
        }
    }

    // The editor always watches; a harness Game.exe does when started with
    // -hotreload (Sys::HotReloadEnabled), without the asset browser and the
    // thumbnail hooks, which are editor-only.
    if (Sys::HotReloadEnabled() && !::Resource::AssetFS::Get().IsMounted())
    {
        // Anchored to the ROOTS like the shader watchers, so with -project this
        // watches the tree the browser shows. engine/ is watched too:
        // engine/textures/Default_mat.imat is a parent of shipped instances.
        // The AssetTypes rows with watch (src/Resoruce/AssetTypes.json).
        std::vector<std::string> exts;
        for (const ::Resource::AssetTypes::Row& row : ::Resource::AssetTypes::kRows)
            if (row.watch) exts.emplace_back(row.ext);
        std::error_code assetDirEc;
        const std::string projectAssets = Sys::ProjectDir() + "/asset";
        const std::string engineAssets  = Sys::EngineDir()  + "/engine";
        if (std::filesystem::is_directory(Sys::U8Path(projectAssets), assetDirEc))
        {
            assetWatchers[0].SetExtensions(exts);
            assetWatchers[0].Init(projectAssets);
        }
        if (std::filesystem::is_directory(Sys::U8Path(engineAssets), assetDirEc))
        {
            assetWatchers[1].SetExtensions(exts);
            assetWatchers[1].Init(engineAssets);
        }
    }
}

void HotReload::WatchService::Poll(const GameModeContext& ctx, IGraphicsDevice& backend, Renderer& renderer)
{
    // Drain EVERY watcher, unconditionally: a short-circuit would let
    // the unpolled one's dedup set grow for the life of the process.
    //
    // The file names are kept in three forms: `shaderChanged` as the
    // watcher reports it (relative to its directory), `shaderChangedRooted`
    // root-relative (see kShaderWatchPrefix) for the receipt and the
    // thumbnail cache, `shaderChangedAbs` absolute for the pre-flight's
    // recipe match. A material's customShaderPath / customVertexShaderPath
    // carry no dependency stamp, so the thumbnail cache records them in its
    // sidecar `deps` and compares them with the rooted list.
    std::vector<std::string> shaderChanged, shaderChangedRooted, shaderChangedAbs;
    for (size_t wi = 0; wi < 2; ++wi)
    {
        auto changed = shaderWatchers[wi].PollChanges();
        for (const std::string& c : changed)
        {
            shaderChangedRooted.push_back(kShaderWatchPrefix[wi] + c);
            shaderChangedAbs.push_back(shaderWatchRoot[wi] + "/" + c);
        }
        shaderChanged.insert(shaderChanged.end(),
                             std::make_move_iterator(changed.begin()),
                             std::make_move_iterator(changed.end()));
    }
    // A shader a tool changed where no watcher looks (e.g. the Material
    // Editor's live graph shader of a harness copy): the same batch, the same
    // receipt.
    for (std::string& p : HotReload::TakeRequestedShaderReloads())
    {
        std::error_code absEc;
        const auto abs = std::filesystem::absolute(Sys::U8Path(p), absEc);
        shaderChangedRooted.push_back(p);
        shaderChangedAbs.push_back(absEc ? p : Sys::PathToUtf8(abs));
        shaderChanged.push_back(std::move(p));
    }
    const size_t changedCount = shaderChanged.size();
    if (changedCount != 0)
    {
        LOG_INFO("Hot-reload: %zu shader file(s) changed", changedCount);
        shaderReloader.Note(shaderChangedRooted, shaderChangedAbs);
    }
    // Pre-flight first: a change that no longer compiles clears nothing,
    // so the passes keep their PSOs; one that compiles is applied here
    // and gets its receipt once its first frame was presented.
    shaderReloader.Tick(ctx.frame, [&]()
    {
        backend.FlushAndWait();
        renderer.ReloadShaders();
        backend.ReloadDeviceShaders();   // the present composite
    });
    if (!shaderChangedRooted.empty() && m_hooks.thumbnailDependencyChanged)
        m_hooks.thumbnailDependencyChanged(shaderChangedRooted);

    // Asset-tree changes. Drained unconditionally for the same
    // dedup-set-growth reason as above.
    std::vector<std::string> assetChanged, assetChangedRooted;
    for (size_t wi = 0; wi < 2; ++wi)
    {
        auto changed = assetWatchers[wi].PollChanges();
        for (const std::string& c : changed)
            assetChangedRooted.push_back(kAssetWatchPrefix[wi] + c);
        assetChanged.insert(assetChanged.end(),
                            std::make_move_iterator(changed.begin()),
                            std::make_move_iterator(changed.end()));
    }
    if (!assetChanged.empty())
    {
        // Under -renderowner the lane may be recording the previous
        // frame: the material reloads and thumbnail cancels below touch
        // renderer state it reads. Rare frames; park it first.
        if (m_hooks.parkRenderLane) m_hooks.parkRenderLane();
        if (m_hooks.externalAssetChange) m_hooks.externalAssetChange(assetChanged.size());

        // Re-resolve the entities a changed material feeds, so an edited master
        // reaches the instances already in the world. Pass the root-relative path:
        // the watcher-relative one matches no material.
        //
        // ReloadMaterialsAffectedBy is scoped on purpose (see its header):
        // each reload SetDirty()s, and a world-wide dirty set disables the
        // renderer's retained-instance fast path for that frame.
        //
        // One receipt per batch (System/HotReloadReceipt.h): the entities
        // re-resolved, and the errors the reload logged.
        std::vector<std::string> materialFiles;
        std::vector<size_t>      materialIdx;
        for (size_t i = 0; i < assetChanged.size(); ++i)
        {
            const std::string& path = assetChanged[i];
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos) continue;
            const std::string ext = path.substr(dot);
            if (ext == ".ianimgraph" || ext == ".ianimset") { renderer.GetAnimGraphSystem()->Reload(path); continue; }
            // A .iui is parsed once and cached (UI/UIAsset.h): the next Engine.SpawnUI
            // reads the edited file. A spawned instance is not re-made here (a
            // UIDocument re-makes its own). The cache checks size / write time too;
            // this also covers an edit that keeps both.
            if (ext == ".iui") { UI::InvalidateUIAsset(assetChangedRooted[i]); continue; }
            // A data table is read again; a rewrite its schema refuses keeps the previous table
            // (Gameplay/DataTable.h). Only a table somebody loaded: an unread one is read when asked for.
            if (ext == ".dtable")
            {
                // The watcher reports lower case; the table is keyed by the path its script named.
                std::string error;
                const std::string loaded = Gameplay::Data::Tables::Get().LoadedPathLike(assetChangedRooted[i]);
                if (!loaded.empty()) Gameplay::Data::Tables::Get().Reload(loaded, error);
                continue;
            }
            // A module's asset type (RegisterAssetHandler).
            if (const AssetHandler handler = FindAssetHandler(ext)) { handler(path); continue; }
            if (ext == ".itex")
            {
                // The texture itself is re-read and swapped in place
                // (same handle and bindless slot), one receipt of kind
                // "texture" per file; the materials that sample it read
                // the same slot and need no re-resolve. Re-resolving them
                // would not re-read anything: TextureSystem and the
                // ResourceManager dedup a loaded .itex by path.
                m_textureSys.RequestReload(assetChangedRooted[i], m_resourceMgr);
                continue;
            }
            if (ext != ".imat" && ext != ".imatinst")
                continue;
            materialFiles.push_back(assetChangedRooted[i]);
            materialIdx.push_back(i);
        }
        if (!materialIdx.empty())
        {
            const std::uint64_t receipt = HotReload::Open("material", materialFiles);
            HotReload::Start(receipt);
            std::vector<CapturedLogLine> captured;
            int rematerialized = 0;
            {
                Logger::ScopedContext capture(Logger::CurrentSource(), Logger::CurrentRequestId(),
                                              &captured, 32);
                for (size_t i : materialIdx)
                    rematerialized += ::Resource::ReloadMaterialsAffectedBy(m_world, assetChangedRooted[i]);
            }
            HotReload::Receipt rc;
            rc.ok = static_cast<std::uint32_t>(rematerialized);
            rc.effectiveFrame = ctx.frame;   // this frame's draw uses them
            for (const CapturedLogLine& l : captured)
                if (l.level == LogLevel::Error) rc.errors.push_back(l.message);
            rc.failed = static_cast<std::uint32_t>(rc.errors.size());
            if (!rc.errors.empty())
            {
                rc.code    = "reload_error";
                rc.message = "re-resolved " + std::to_string(rematerialized) +
                             " entity(ies), with " + std::to_string(rc.errors.size()) + " error(s)";
            }
            else
                rc.message = "in effect from frame " + std::to_string(rc.effectiveFrame) + ": " +
                             std::to_string(rematerialized) + " entity(ies) re-resolved";
            rc.detailJson = "{\"entities\":" + std::to_string(rematerialized) + "}";
            HotReload::Close(receipt, rc);
        }

        // A .itex re-import has no dependency stamp on the material side
        // (MaterialSerializer stamps only chain members), so the thumbnail
        // cache carries the texture paths itself and compares them with this
        // list. Pass the ROOTED list: the watcher's own is relative to the
        // watched directory and never matches a sidecar's "asset/..." dep.
        if (m_hooks.thumbnailDependencyChanged)
            m_hooks.thumbnailDependencyChanged(assetChangedRooted);
    }
}
