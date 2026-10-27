// EditorLayer_AssetBrowser.cpp - the Asset Browser panel of EditorLayer: ScanAssets, the
// Tiles / List / Details views, the folder tree, asset and folder file operations and the
// rendered thumbnail tiles. EditorLayer member definitions only (no state of its own); the
// import pipeline (RunImport, the import popups) lives in EditorLayer.cpp.
#include "Editor/EditorLayer.h"
#include "Resource/AssetTypes.generated.h"   // the asset types (AssetTypes.json)
#include "Editor/EditorLayerShared.h"   // OpenFileDialog (Reimport From...)
#include "Editor/AttackEditor.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/AssetPreviewWindow.h"  // the mesh / prefab preview panel
#include "Editor/MeshEditorWindow.h"    // Open in Mesh Editor
#include "Editor/SkeletonEditorWindow.h"   // Open in Skeleton Editor
#include "Editor/TextureViewerWindow.h"    // Open in Texture Viewer
#include "Editor/UIEditorWindow.h"         // Open in UI Editor
#include "Editor/MaterialEditorWindow.h"   // Open in Material Editor
#include "Editor/FontAwesomeIcons.h"
#include "Graphics/GraphicsDX12.h"       // GetTexturePreviewSrvGpuHandle (thumbnail SRV)
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"      // streamed thumbnail Acquire/Release
#include "Resource/AssetGuid.h"          // .meta identities - rename/move survival
#include "ECS/Components.h"              // MaterialComponent (Create Material Instance)
#include "Resource/MaterialSerializer.h" // LoadMaterial / SaveMaterialInstance
#include "Resource/MaterialLiveOverlay.h" // LiveMaterialBypass: tiles read the file, not a live edit
#include "Renderer/Renderer.h"           // GetMaterialShaderLibrary (instance repair)
#include "Resource/AssetRefs.h"          // ToLowerAscii - one definition of the vocabulary
#include "Resource/AssetFS.h"            // the shared content root (a worktree's binary payload)
#include "Resource/MeshLibIdentity.h"    // Mesh items (keys, MESH_IDENT_*)
#include "Scene/MeshAssetRegistry.h"     // a model's Meshes
#include "Scene/SceneInstanceLoader.h"   // ModelSkeletonPath (Open in Skeleton Editor)
#include "System/CommandLine.h"          // -refscan_root (harness fixture root of the reference index)
#include "System/Log.h"
#include "System/PathUtil.h"             // UTF-8 <-> UTF-16 filesystem boundary (non-ASCII paths)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW - "Show in Explorer"; SHFileOperationW - the Recycle Bin

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"        // ImClamp

#include <algorithm>
#include <cctype>
#include "Resource/TextureInfo.h"   // texture tooltip + Details columns
#include "Resource/TextureImportRecord.h"   // Reimport a texture from its record
#include "Renderer/TextureViewService.h"   // cube thumbnails as a cross
#include <chrono>       // clock_cast - file_clock -> system_clock ("Modified" column)
#include <cstdio>
#include <cstring>
#include <ctime>        // localtime_s / strftime - asset browser "Modified" column
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Moves @p paths to the Recycle Bin in one shell operation, so a delete can be taken back
    // from Explorer. A missing path fails the whole move (nothing is deleted). False with @p why
    // when the shell refused.
    bool MoveToRecycleBin(const std::vector<std::string>& paths, std::string& why)
    {
        std::wstring from;   // double-NUL-terminated list of absolute paths
        for (const std::string& p : paths)
        {
            std::error_code ec;
            const std::filesystem::path abs = std::filesystem::absolute(Sys::U8Path(p), ec);
            if (ec) { why = "'" + p + "': " + ec.message(); return false; }
            from += abs.wstring();
            from.push_back(L'\0');
        }
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc  = FO_DELETE;
        op.pFrom  = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        const int rc = SHFileOperationW(&op);
        if (rc != 0 || op.fAnyOperationsAborted)
        {
            why = "the shell refused the move to the Recycle Bin (code " + std::to_string(rc) + ")";
            return false;
        }
        return true;
    }

    // The asset and its .meta sidecar, so a restore from the Recycle Bin brings its guid back.
    std::vector<std::string> WithSidecar(const std::string& asset)
    {
        std::vector<std::string> out{ asset };
        std::error_code ec;
        const std::string meta = Resource::MetaPathFor(asset);
        if (std::filesystem::exists(Sys::U8Path(meta), ec)) out.push_back(meta);
        return out;
    }

    // Last-write time as a time_t; 0 when unavailable (the Details column shows "-").
    // MSVC's C++20 file_clock converts through clock_cast.
    int64_t FileWriteTimeT(const std::filesystem::directory_entry& e)
    {
        std::error_code ec;
        const auto ft = e.last_write_time(ec);
        if (ec) return 0;
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
        return static_cast<int64_t>(std::chrono::system_clock::to_time_t(sys));
    }

    // ASCII case-insensitive suffix test, for telling extensions apart inside one AssetType.
    // Must be case-insensitive: AssetTypeForExt sees a lower-cased extension, so a byte-exact
    // compare would type "Head.MESHLIB" as a Mesh and then silently refuse to drag it.
    bool EndsWithCI(std::string_view s, std::string_view suffix)
    {
        if (s.size() < suffix.size()) return false;
        const size_t off = s.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i])))
                return false;
        return true;
    }

    // Case-insensitive lexicographic compare (ASCII fold only; non-ASCII UTF-8
    // bytes pass through so CJK names keep a stable byte ordering).
    int CompareNameCI(const std::string& a, const std::string& b)
    {
        const size_t n = (std::min)(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
        {
            const int ca = std::tolower(static_cast<unsigned char>(a[i]));
            const int cb = std::tolower(static_cast<unsigned char>(b[i]));
            if (ca != cb) return ca < cb ? -1 : 1;
        }
        if (a.size() == b.size()) return 0;
        return a.size() < b.size() ? -1 : 1;
    }

    // Explorer-style byte count.
    void FormatAssetSize(uint64_t bytes, char* out, size_t cap)
    {
        constexpr uint64_t kKB = 1024ull, kMB = kKB * 1024ull, kGB = kMB * 1024ull;
        if      (bytes >= kGB) std::snprintf(out, cap, "%.2f GB", double(bytes) / double(kGB));
        else if (bytes >= kMB) std::snprintf(out, cap, "%.1f MB", double(bytes) / double(kMB));
        else if (bytes >= kKB) std::snprintf(out, cap, "%.0f KB", double(bytes) / double(kKB));
        else                   std::snprintf(out, cap, "%llu B",  static_cast<unsigned long long>(bytes));
    }

    // Local-time "YYYY-MM-DD HH:MM"; empty string when the stamp is unknown.
    void FormatAssetTime(int64_t stamp, char* out, size_t cap)
    {
        out[0] = '\0';
        if (stamp <= 0) return;
        const std::time_t tt = static_cast<std::time_t>(stamp);
        std::tm tmv {};
        if (localtime_s(&tmv, &tt) == 0)
            std::strftime(out, cap, "%Y-%m-%d %H:%M", &tmv);
    }

    // The same folder in the main checkout, when this checkout reads binary payload it did
    // not check out (the shared content root, Resource/SharedContent.h): a payload-only folder
    // does not exist here and the others lack their textures, models and animations. Empty
    // when there is no shared root or `dir` is not a relative asset/ or engine/ folder.
    // Entries listed from there keep THIS checkout's path spelling: loads resolve them
    // through AssetFS, and file operations on them fail here rather than reach the other tree.
    std::filesystem::path SharedTwin(const std::string& dir)
    {
        const std::string twin = Resource::AssetFS::Get().SharedTwinDir(dir);
        return twin.empty() ? std::filesystem::path() : Sys::U8Path(twin);
    }
}

// Which assets get a RENDERED tile and a Preview action: Preview::AssetKindForPath, the one
// mapping the panel and the composer use too (.meshlib, .ipfb, .iscn, and a Mesh item's
// MeshAssetRef "<model>#<keyhex>").
// Gated on the EXTENSION, not on AssetType: AssetTypeForExt folds .imsh and .meshlib into
// AssetType::Mesh, and a .imsh has no runtime path to a mesh-descriptor slot, so a
// type-keyed gate would queue a tile per .imsh that resolves to zero parts and latches
// "gave up" - a folder of grey squares.
static Preview::AssetKind PreviewKindForPath(const std::string& path)
{
    return Preview::AssetKindForPath(path);
}

// The FILE a preview entry's picture is stamped against in the disk cache: the
// entry's own path, or - for a Mesh item's tile, whose path is a MeshAssetRef -
// its model.
static std::string PreviewSourceFile(const std::string& path)
{
    std::string model;
    uint64_t    key = 0;
    return Resource::ParseMeshAssetRef(path, model, key) ? model : path;
}

// A static member because AssetType is private to EditorLayer; the declaration says why
// the folder tree shares it.
// The row's kind (src/Resoruce/AssetTypes.json) as the browser's type: a new extension of a known kind
// is one row there; a new kind is a row here too.
EditorLayer::AssetType EditorLayer::AssetTypeForExt(const std::string& ext)
{
    static const std::pair<std::string_view, AssetType> kKinds[] = {
        { "Texture", AssetType::Texture },     { "Shader", AssetType::Shader },       { "Mesh", AssetType::Mesh },
        { "Material", AssetType::Material },   { "Scene", AssetType::Scene },         { "Prefab", AssetType::Prefab },
        { "Skeleton", AssetType::Skeleton },   { "Animation", AssetType::Animation }, { "AnimGraph", AssetType::AnimGraph },
        { "AnimSet", AssetType::AnimSet },     { "Audio", AssetType::Audio },         { "Script", AssetType::Script },
        { "Font", AssetType::Font },           { "VFX", AssetType::VFX },             { "Sequence", AssetType::Sequence },
        { "UIAsset", AssetType::UIAsset },     { "FaceMap", AssetType::FaceMap },     { "FaceTake", AssetType::FaceTake },
    };
    const Resource::AssetTypes::Row* row = Resource::AssetTypes::FindByExt(ext);
    if (!row || !row->kind) return AssetType::Unknown;
    for (const auto& [name, type] : kKinds)
        if (name == row->kind) return type;
    return AssetType::Unknown;
}

void EditorLayer::ScanAssets()
{
    // Counted for the thumbnail summary (rescans= / orphansReleased=): the snapshot / restore /
    // orphan-release path below is the one part of the tile lifecycle a churn pack cannot
    // reach by scrolling, so the count shows whether the pack covered it.
    ++m_thumbRescans;

    // ---- Step 1: snapshot preview state keyed by path ----
    // The GPU handles and the visibility timer carry over a rescan so thumbnails do not flicker.
    struct PreviewState
    {
        Resource::TextureHandle thumbnail;
        float                   lastVisibleTime;
        // The rendered tile travels too: a rescan runs every 5 s and on every external file
        // change, and an entry that dropped it would re-render its tile each time.
        RHI::Texture            previewTile;
        uint64_t                previewKey;
        std::string             previewGuid;
        uint32_t                previewGen;
        bool                    previewDirty;
        bool                    previewReady;
        bool                    previewGaveUp;
    };
    std::unordered_map<std::string, PreviewState> saved;
    saved.reserve(m_assets.size());
    for (auto& ae : m_assets)
        saved[ae.path] = { ae.thumbnail, ae.lastVisibleTime,
                           ae.previewTile, ae.previewKey, ae.previewGuid,
                           ae.previewGen, ae.previewDirty, ae.previewReady,
                           ae.previewGaveUp };

    m_assets.clear();

    // Release every handle still in `saved` (its path left the listing).
    auto releaseOrphans = [&]()
    {
        if (m_textureSys && m_gfx)
            for (auto& [path, ps] : saved)
                if (ps.thumbnail != Resource::kInvalidTextureHandle)
                {
                    m_textureSys->Release(ps.thumbnail, *m_gfx);
                    // Counted too: once a tile is in the disk cache, what a rescan orphans is this streamed
                    // handle, not the rendered texture below.
                    ++m_thumbOrphansReleased;
                }
        // An orphaned tile is a texture the browser owns: queue it on the deferred-destroy list
        // the eviction sweep uses. ScanAssets is reachable from an ImGui handler (Refresh, a
        // folder double-click), and a descriptor an earlier ImGui::Image captured must outlive
        // this frame's draw data.
        for (auto& [path, ps] : saved)
        {
            if (!ps.previewTile.IsValid()) continue;
            // Cancel first: CancelPreviewThumbnail drops the queued requests AND voids the in-flight
            // one. A request left flying would copy into a texture already queued for destruction,
            // never publish a result (the copy is skipped once the pool slot is recycled), and be
            // re-sent up to the lane's resend cap. The tile lives until ProcessPendingActions, which
            // runs after the record half is parked, so a copy already recorded has been submitted.
            if (m_renderer && ps.previewKey)
                m_renderer->CancelPreviewThumbnail(ps.previewKey);
            m_pendingTileDestroy.push_back(ps.previewTile);
            ++m_thumbEvicted;
            ++m_thumbOrphansReleased;
        }
        saved.clear();
    };

    if (m_assetDir.empty()) { releaseOrphans(); return; }

    // Build the identity index once, the first time there is a tree. READ-ONLY: an asset is
    // stamped when something first references it (AssetRefs::Augment*Text -> EnsureGuid), so
    // startup never writes thousands of files; "Stamp all asset identities" in the Asset
    // References window does the bulk pass.
    static bool s_guidIndexBuilt = false;
    if (!s_guidIndexBuilt)
    {
        s_guidIndexBuilt = true;
        // Both content roots. The identity index and the reference index have
        // to span exactly what the browser can show, or "who uses this?" says
        // "nobody" about an engine asset the whole project depends on.
        Resource::AssetGuidIndex::Get().SetContentRoots({
            m_assetDir, Sys::EngineDir() + "/engine" });
        Resource::AssetGuidIndex::Get().Rebuild(m_assetDir, /*stampMissing=*/false);
        m_refScanner.SetExtraRoots(RefScanExtraRoots());
    }

    // ---- Step 2: scan the directory, then its shared twin ----
    const std::string& scanDir = m_currentDir.empty() ? m_assetDir : m_currentDir;
    std::error_code ec;
    const std::filesystem::path twin = SharedTwin(scanDir);
    const bool twinDir = !twin.empty() && std::filesystem::is_directory(twin, ec);
    if (!std::filesystem::exists(Sys::U8Path(scanDir), ec) && !twinDir) { releaseOrphans(); return; }

    const auto& typeOf = AssetTypeForExt;

    // `as` is the path the entry is listed under: the entry's own, or - for
    // one found in the shared twin - the same name in this checkout's folder.
    auto addEntry = [&](const std::filesystem::directory_entry& entry, const std::filesystem::path& as, bool shared)
    {
        if (entry.is_directory())
        {
            AssetEntry ae;
            ae.path = Sys::PathToUtf8(as);
            ae.stem = Sys::PathToUtf8(as.filename());
            ae.name = ae.stem;                 // folders have no extension to strip
            ae.type = AssetType::Folder;
            ae.mtime = FileWriteTimeT(entry);
            m_assets.push_back(std::move(ae));
            return;
        }

        if (!entry.is_regular_file()) return;

        std::string ext = Sys::PathToUtf8(as.extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        AssetType t = typeOf(ext);
        const std::string fullName=Sys::PathToUtf8(as);
        if(fullName.ends_with(".attack.json")||fullName.ends_with(".attackset.json"))t=AssetType::Animation;
        if (t == AssetType::Unknown) return;
        // Deleted in this checkout: the shared root still has it, this checkout does not.
        if (shared && !Resource::AssetFS::Get().SharedServes(fullName)) return;

        AssetEntry ae;
        ae.path      = fullName;
        ae.stem      = Sys::PathToUtf8(as.stem());
        ae.name      = Sys::PathToUtf8(as.filename());
        ae.type      = t;
        ae.thumbnail = Resource::kInvalidTextureHandle;

        // List/Details columns. A failed stat leaves 0 (shown as "-") rather than aborting the
        // scan: files can vanish between iterate and stat.
        std::error_code sec;
        const auto sz = entry.file_size(sec);
        ae.sizeBytes  = sec ? 0 : static_cast<uint64_t>(sz);
        ae.mtime      = FileWriteTimeT(entry);

        m_assets.push_back(std::move(ae));
    };

    std::unordered_set<std::string> listed;   // lower-case names found here
    for (const auto& entry : std::filesystem::directory_iterator(Sys::U8Path(scanDir), ec))
    {
        listed.insert(Resource::AssetRefs::ToLowerAscii(Sys::PathToUtf8(entry.path().filename())));
        addEntry(entry, entry.path(), false);
    }
    if (twinDir)
    {
        std::error_code tec;
        for (const auto& entry : std::filesystem::directory_iterator(twin, tec))
            if (listed.insert(Resource::AssetRefs::ToLowerAscii(Sys::PathToUtf8(entry.path().filename()))).second)
                addEntry(entry, Sys::U8Path(scanDir) / entry.path().filename(), true);
    }

    // Sort: folders first, then by type enum, then alphabetically.
    std::sort(m_assets.begin(), m_assets.end(), [](const AssetEntry& a, const AssetEntry& b)
    {
        const bool af = (a.type == AssetType::Folder);
        const bool bf = (b.type == AssetType::Folder);
        if (af != bf) return af > bf;
        if (a.type != b.type) return static_cast<int>(a.type) < static_cast<int>(b.type);
        return a.stem < b.stem;
    });

    // ---- Step 3: restore preview state; remaining `saved` entries are orphans to release. ----
    for (auto& ae : m_assets)
    {
        auto it = saved.find(ae.path);
        if (it != saved.end())
        {
            ae.thumbnail       = it->second.thumbnail;
            ae.lastVisibleTime = it->second.lastVisibleTime;
            ae.previewTile     = it->second.previewTile;
            ae.previewKey      = it->second.previewKey;
            ae.previewGuid     = it->second.previewGuid;
            ae.previewGen      = it->second.previewGen;
            ae.previewDirty    = it->second.previewDirty;
            ae.previewReady    = it->second.previewReady;
            ae.previewGaveUp   = it->second.previewGaveUp;
            saved.erase(it);   // consumed - not an orphan
        }
    }

    // Release handles for paths that disappeared from disk.
    releaseOrphans();

    // Indices in m_assetOrder now refer to a different vector: force a rebuild before any
    // view iterates it.
    m_assetOrderDirty = true;

    // ---- Reconcile the selection with the new listing ----
    // Paths survive a rescan, but the PRIMARY is an index and does not: re-derive it, and
    // drop paths this folder no longer lists.
    if (!m_selectedAssetPaths.empty())
    {
        const std::string primaryPath =
            (m_selectedAsset >= 0 && !m_selectedAssetPaths.empty())
                ? m_selectedAssetPaths.back() : std::string();

        std::erase_if(m_selectedAssetPaths, [this](const std::string& p) {
            return std::none_of(m_assets.begin(), m_assets.end(),
                                [&p](const AssetEntry& a) { return a.path == p; });
        });

        m_selectedAsset = -1;
        if (!primaryPath.empty())
            for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
                if (m_assets[i].path == primaryPath) { m_selectedAsset = i; break; }
        // Primary gone (deleted, moved, filtered out): hand it to a survivor rather than leave
        // an index pointing at an unrelated asset.
        if (m_selectedAsset < 0 && !m_selectedAssetPaths.empty())
            for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
                if (m_assets[i].path == m_selectedAssetPaths.back()) { m_selectedAsset = i; break; }
        m_assetRangeAnchor = m_selectedAsset;
    }

    // "Find in Content Browser" on an Inspector material row: the file is selected once its
    // folder is listed.
    if (!m_revealAssetPath.empty())
    {
        auto key = [](const std::string& p)
        { return Resource::AssetRefs::NormalizeRefPath(Sys::RelativizeAssetPath(p)); };
        const std::string want = key(m_revealAssetPath);
        for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
            if (key(m_assets[i].path) == want)
            {
                m_selectedAssetPaths = { m_assets[i].path };
                m_selectedAsset      = i;
                m_assetRangeAnchor   = i;
                break;
            }
        m_revealAssetPath.clear();
    }
}

// ==== Asset browser - shared item plumbing (Tiles / List / Details) ====
// Every view must behave identically for selection, drag payloads, double-click open and
// thumbnail streaming, so that logic lives here once.

// FontAwesome glyph for an asset type; nullptr when the icon font is missing
// (callers fall back to AssetTypeTag).
const char* EditorLayer::AssetIconChar(AssetType t) const
{
    if (!m_hasFontAwesomeIcons) return nullptr;
    switch (t)
    {
    case AssetType::Texture:   return FontAwesome::kImage;
    case AssetType::Shader:    return FontAwesome::kCode;
    case AssetType::Mesh:      return FontAwesome::kCube;
    case AssetType::Material:  return FontAwesome::kPaintBrush;
    case AssetType::Scene:     return FontAwesome::kCubes;
    case AssetType::Prefab:    return FontAwesome::kCubes;
    case AssetType::Skeleton:  return FontAwesome::kUser;
    case AssetType::Animation: return FontAwesome::kFilm;
    case AssetType::Audio:     return FontAwesome::kVolumeUp;
    case AssetType::Script:    return FontAwesome::kFileText;
    case AssetType::Font:      return FontAwesome::kFont;
    case AssetType::VFX:       return FontAwesome::kFire;
    case AssetType::Sequence:  return FontAwesome::kVideoCamera;
    case AssetType::UIAsset:   return FontAwesome::kTh;
    case AssetType::FaceMap:   return FontAwesome::kUser;
    case AssetType::FaceTake:  return FontAwesome::kFilm;
    case AssetType::Folder:    return FontAwesome::kFolder;
    default:                   return nullptr;
    }
}

ImU32 EditorLayer::AssetTypeColor(AssetType t)
{
    switch (t)
    {
    case AssetType::Texture:   return IM_COL32( 50,  90, 160, 255);  // blue
    case AssetType::Shader:    return IM_COL32(140,  60, 180, 255);  // purple
    case AssetType::Mesh:      return IM_COL32( 60, 140,  60, 255);  // green
    case AssetType::Material:  return IM_COL32(180, 100,  30, 255);  // orange
    case AssetType::Scene:     return IM_COL32( 30, 130, 140, 255);  // teal
    case AssetType::Prefab:    return IM_COL32( 80,  40, 160, 255);  // purple
    case AssetType::Skeleton:  return IM_COL32(200,  80,  80, 255);  // red
    case AssetType::Animation: return IM_COL32( 80, 160, 100, 255);  // green
    case AssetType::Audio:     return IM_COL32(180, 110, 200, 255);  // pink-violet
    case AssetType::Script:    return IM_COL32(100, 150, 200, 255);  // light blue
    case AssetType::Font:      return IM_COL32(190, 140,  50, 255);  // amber
    case AssetType::VFX:       return IM_COL32(200,  70, 120, 255);  // magenta
    case AssetType::Sequence:  return IM_COL32( 60,  60, 160, 255);  // indigo
    case AssetType::UIAsset:   return IM_COL32( 40, 150, 190, 255);  // cyan
    case AssetType::FaceMap:   return IM_COL32(210, 120, 150, 255);  // rose
    case AssetType::FaceTake:  return IM_COL32(150,  90, 120, 255);  // dusky rose
    case AssetType::Folder:    return IM_COL32(180, 150,  30, 255);  // gold
    default:                   return IM_COL32( 70,  70,  70, 255);
    }
}

// Human-readable type shown in the Details "Type" column.
const char* EditorLayer::AssetTypeName(AssetType t)
{
    switch (t)
    {
    case AssetType::Texture:   return "Texture";
    case AssetType::Shader:    return "Shader";
    // The enum names predate this reading and stay; only the label changes. A .meshlib is a
    // model's GEOMETRY package (many sections), a .iscn is the MODEL itself (scene graph +
    // section bindings).
    case AssetType::Mesh:      return "Geometry";
    case AssetType::Material:  return "Material";
    case AssetType::Scene:     return "Model";
    case AssetType::Prefab:    return "Prefab";
    case AssetType::Skeleton:  return "Skeleton";
    case AssetType::Animation: return "Animation";
    case AssetType::Audio:     return "Audio";
    case AssetType::Script:    return "Script";
    case AssetType::Font:      return "Font";
    case AssetType::VFX:       return "VFX";
    case AssetType::Sequence:  return "Sequence";
    case AssetType::UIAsset:   return "UI";
    case AssetType::FaceMap:   return "Face Mapping";
    case AssetType::FaceTake:  return "Face Take";
    case AssetType::Folder:    return "Folder";
    default:                   return "Unknown";
    }
}

// One-letter badge drawn in place of the glyph when FontAwesome failed to load.
const char* EditorLayer::AssetTypeTag(AssetType t)
{
    switch (t)
    {
    case AssetType::Texture:   return "T";
    case AssetType::Shader:    return "S";
    case AssetType::Mesh:      return "M";
    case AssetType::Material:  return "A";
    case AssetType::Scene:     return "N";
    case AssetType::Prefab:    return "P";
    case AssetType::Skeleton:  return "K";
    case AssetType::Animation: return "A";
    case AssetType::Audio:     return "U";
    case AssetType::Script:    return "L";
    case AssetType::Font:      return "F";
    case AssetType::VFX:       return "V";
    case AssetType::Sequence:  return "Q";
    case AssetType::UIAsset:   return "I";
    case AssetType::FaceMap:   return "Y";
    case AssetType::FaceTake:  return "R";
    case AssetType::Folder:    return "/";
    default:                   return "?";
    }
}

// Case-insensitive substring match against the toolbar filter box.
bool EditorLayer::AssetMatchesFilter(const std::string& stem) const
{
    if (m_assetFilter[0] == '\0') return true;

    const size_t needleLen = std::strlen(m_assetFilter);
    const size_t n         = stem.size();
    if (needleLen > n) return false;

    for (size_t i = 0; i + needleLen <= n; ++i)
    {
        size_t j = 0;
        for (; j < needleLen; ++j)
        {
            const int a = std::tolower(static_cast<unsigned char>(stem[i + j]));
            const int b = std::tolower(static_cast<unsigned char>(m_assetFilter[j]));
            if (a != b) break;
        }
        if (j == needleLen) return true;
    }
    return false;
}

// Rebuild the filtered + sorted index list every view iterates. m_assets keeps
// ScanAssets' canonical order (folders, type, name) so a 5 s rescan never
// stomps the sort the user picked in the Details header.
void EditorLayer::RebuildAssetDisplayOrder()
{
    m_assetOrder.clear();
    m_assetOrder.reserve(m_assets.size());
    for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
    {
        // Folders always survive the filter so navigation is never blocked.
        const AssetEntry& ae = m_assets[i];
        if (ae.type != AssetType::Folder && !AssetMatchesFilter(ae.stem)) continue;
        m_assetOrder.push_back(i);
    }

    const int  sortCol = m_assetSortColumn;
    const bool asc     = m_assetSortAsc;
    std::stable_sort(m_assetOrder.begin(), m_assetOrder.end(),
        [this, sortCol, asc](int lhs, int rhs)
        {
            const AssetEntry& a = m_assets[lhs];
            const AssetEntry& b = m_assets[rhs];

            // Explorer rule: folders lead regardless of column or direction.
            const bool af = (a.type == AssetType::Folder);
            const bool bf = (b.type == AssetType::Folder);
            if (af != bf) return af;

            int cmp = 0;
            switch (sortCol)
            {
            case 1: cmp = static_cast<int>(a.type) - static_cast<int>(b.type); break;
            case 2: cmp = (a.sizeBytes < b.sizeBytes) ? -1 : (a.sizeBytes > b.sizeBytes) ? 1 : 0; break;
            case 3: cmp = (a.mtime     < b.mtime)     ? -1 : (a.mtime     > b.mtime)     ? 1 : 0; break;
            case 4: case 5:
            {
                // Dimensions sort by texel count, Format by its label; non-textures sort after textures.
                const Resource::TextureFileInfo* ta = AssetTextureInfo(lhs);
                const Resource::TextureFileInfo* tb = AssetTextureInfo(rhs);
                if (!ta || !tb) { cmp = (ta ? -1 : 0) + (tb ? 1 : 0); break; }
                if (sortCol == 4)
                {
                    const uint64_t pa = uint64_t(ta->width) * ta->height * ta->arraySize;
                    const uint64_t pb = uint64_t(tb->width) * tb->height * tb->arraySize;
                    cmp = pa < pb ? -1 : pa > pb ? 1 : 0;
                }
                else
                    cmp = ta->formatLabel.compare(tb->formatLabel) < 0 ? -1 : ta->formatLabel == tb->formatLabel ? 0 : 1;
                break;
            }
            default: break;
            }
            if (cmp == 0) cmp = CompareNameCI(a.name, b.name);   // name is also the tiebreaker
            return asc ? (cmp < 0) : (cmp > 0);
        });

    // List-view column width tracks the longest visible name, measured here so the
    // per-frame path never re-measures every entry.
    float widest = 0.f;
    for (int idx : m_assetOrder)
        widest = (std::max)(widest, ImGui::CalcTextSize(m_assets[idx].name.c_str()).x);
    m_assetListColW = ImClamp(widest + 46.f, 150.f, 420.f);

    m_assetOrderDirty = false;
}

// Time-stamp an entry's visibility and Acquire a texture's streamed preview (budget-limited;
// a rendered entry only raises previewDirty). `visible` gates both: off-screen entries
// deliberately leave lastVisibleTime alone so the hidden gap keeps growing.
void EditorLayer::StreamAssetThumbnail(int index, bool visible, float now)
{
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return;
    StreamEntryThumbnail(m_assets[index], visible, now);
}

void EditorLayer::StreamEntryThumbnail(AssetEntry& ae, bool visible, float now)
{
    // Rendered tiles: materials (.imat / .imatinst) and every PreviewKindForPath asset - one
    // lane, one budget, one disk cache, a different draw source. PreviewKindForPath says why
    // the asset half is keyed on the extension.
    const bool isRendered = (ae.type == AssetType::Material) ||
                            (PreviewKindForPath(ae.path) != Preview::AssetKind::None);
    if ((ae.type != AssetType::Texture && !isRendered) || !visible) return;
    if (!m_textureSys || !m_resourceMgr || !m_gfx) return;

    ae.lastVisibleTime = now;

    if (isRendered)
    {
        // Nothing is created, loaded or enqueued here: this runs in the ImGui pass, which may not
        // create a texture, touch the renderer's material maps or destroy a descriptor an earlier
        // AddImage captured. It only raises the flag ServiceAssetThumbnails acts on in
        // ProcessPendingActions (the serial window).
        if (!ae.previewReady && !ae.previewDirty && !ae.previewGaveUp &&
            ae.thumbnail == Resource::kInvalidTextureHandle)
            ae.previewDirty = true;
        return;
    }

    if (ae.thumbnail == Resource::kInvalidTextureHandle &&
        m_previewLoadsThisFrame < m_previewLoadBudgetPerFrame)
    {
        ae.thumbnail = m_textureSys->Acquire(ae.path, *m_resourceMgr, *m_gfx);
        ++m_previewLoadsThisFrame;
    }
}

// GPU SRV of an entry's thumbnail; 0 when not (yet) resident. Re-resolved every frame,
// never cached: the descriptor slot goes through the allocator's free list, and ImGui's
// own SRVs come from the same heap.
uint64_t EditorLayer::GetAssetThumbnailSrv(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return 0;
    return GetEntryThumbnailSrv(m_assets[index]);
}

uint64_t EditorLayer::GetEntryThumbnailSrv(const AssetEntry& ae) const
{
    if (!m_textureSys || !m_gfx) return 0;
    const bool isRendered = (ae.type == AssetType::Material) ||
                            (PreviewKindForPath(ae.path) != Preview::AssetKind::None);
    if (ae.type != AssetType::Texture && !isRendered) return 0;
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    // A rendered entry shows its disk-cached tile when one streamed in (same picture, already
    // resident), else its freshly rendered tile; a rendered tile that later gets a cache
    // entry keeps being drawn until the sweep evicts it.
    if (isRendered)
    {
        if (const RHI::Texture* cached = m_textureSys->GetTexture(ae.thumbnail))
            return dx12.GetTexturePreviewSrvGpuHandle(*cached);
        // previewReady, not previewTile.IsValid(): a tile is created BEFORE its
        // first render and is zero-filled until the copy lands, so drawing it
        // early shows a black square where an icon should be.
        if (ae.previewReady && ae.previewTile.IsValid())
            return m_gfx->GetTextureSRVGpuHandle(ae.previewTile);
        return 0;
    }

    const RHI::Texture* tex = m_textureSys->GetTexture(ae.thumbnail);
    if (!tex) return 0;
    // ImGui's backend samples a Texture2D: a cube / array / 3D / MSAA view bound there is a
    // view-type mismatch, silent UB under Resource Binding Tier 3 (DX12_DEBUGLAYER=1
    // DX12_GBV=1 reports it). A cube shows its cross instead, anything else its badge.
    const RHI::TextureDesc& d = tex->desc;
    if (d.type != RHI::TextureDesc::Type::TEXTURE_2D || d.arraySize != 1 || d.sampleCount > 1 ||
        RHI::HasFlag(d.miscFlags, RHI::ResourceMiscFlag::TEXTURECUBE))
        return CubeThumbnailSrv(ae, *tex);   // a cube's cross; 0 (the badge) for anything else
    return dx12.GetTexturePreviewSrvGpuHandle(*tex);
}

uint64_t EditorLayer::CubeThumbnailSrv(const AssetEntry& ae, const RHI::Texture& tex) const
{
    if (TextureView::KindOf(tex.desc) != TextureView::SourceKind::Cube || !m_renderer || !m_textureSys) return 0;
    TextureViewService* views = m_renderer->GetTextureViews();
    if (!views) return 0;
    auto it = m_cubeThumbs.find(ae.path);
    if (it == m_cubeThumbs.end())
    {
        if (m_cubeThumbs.size() >= kMaxCubeThumbs) return 0;
        const std::uint32_t id = views->Acquire();
        if (id == TextureViewService::kInvalidId) return 0;
        it = m_cubeThumbs.emplace(ae.path, CubeThumb{ id, 0 }).first;
    }
    it->second.lastFrame = m_editorFrame;
    // The cross (4 x 3 faces) fitted into a square tile, black around it; a float
    // cube (an IBL, a probe) through HDR + tonemap like the viewer's default.
    constexpr std::uint32_t kPx = 128;
    Resource::TextureSystem::Info ti;
    m_textureSys->Describe(ae.thumbnail, ti);
    TextureView::Desc desc;
    desc.source         = tex;
    desc.sourceRevision = ti.revision;
    desc.width          = kPx;
    desc.height         = kPx;
    desc.cubeLayout     = TextureView::CubeLayout::Cross;
    desc.background     = TextureView::Background::Black;
    desc.filtered       = true;
    const RHI::Format f = tex.desc.format;
    if (f == RHI::Format::R16G16B16A16_FLOAT || f == RHI::Format::R32G32B32A32_FLOAT ||
        f == RHI::Format::R11G11B10_FLOAT || f == RHI::Format::BC6H_UF16 || f == RHI::Format::BC6H_SF16)
        desc.map = TextureView::Map::Hdr;
    desc.uvPerPixel[0] = 1.f / kPx;
    desc.uvPerPixel[1] = 1.f / (kPx * 0.75f);
    desc.uvOrigin[0]   = 0.f;
    desc.uvOrigin[1]   = -(kPx * 0.125f) / (kPx * 0.75f);
    views->Request(it->second.viewId, desc);
    return views->Srv(it->second.viewId);
}

void EditorLayer::SweepCubeThumbnails(bool all)
{
    TextureViewService* views = m_renderer ? m_renderer->GetTextureViews() : nullptr;
    for (auto it = m_cubeThumbs.begin(); it != m_cubeThumbs.end();)
    {
        if (all || it->second.lastFrame + 2 < m_editorFrame)
        {
            if (views) views->Release(it->second.viewId);
            it = m_cubeThumbs.erase(it);
        }
        else
            ++it;
    }
}

const Resource::TextureFileInfo* EditorLayer::AssetTextureInfo(int index)
{
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return nullptr;
    AssetEntry& ae = m_assets[index];
    if (ae.type != AssetType::Texture || !EndsWithCI(ae.path, ".itex")) return nullptr;
    if (!ae.texInfo)
        ae.texInfo = std::make_shared<const Resource::TextureFileInfo>(Resource::DescribeTextureFile(ae.path));
    return ae.texInfo->ok ? ae.texInfo.get() : nullptr;
}

// Small square icon for List/Details rows: streamed thumbnail when resident,
// otherwise the type-colored badge. Consumes size x size of layout so the
// caller can SameLine() the label after it.
void EditorLayer::DrawAssetItemIcon(int index, float size)
{
    const AssetEntry& ae = m_assets[index];
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + size, p0.y + size);
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    if (const uint64_t srv = GetAssetThumbnailSrv(index))
    {
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(srv)), p0, p1);
    }
    else
    {
        dl->AddRectFilled(p0, p1, AssetTypeColor(ae.type), 2.f);
        const char*  text = AssetIconChar(ae.type);
        if (!text) text   = AssetTypeTag(ae.type);
        const ImVec2 sz   = ImGui::CalcTextSize(text);
        dl->PushClipRect(p0, p1, true);   // glyphs are 16 px in an 18 px badge
        dl->AddText(ImVec2(p0.x + (size - sz.x) * 0.5f, p0.y + (size - sz.y) * 0.5f),
                    IM_COL32(255, 255, 255, 230), text);
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(size, size));
}

// The one payload a drag from this item publishes (contract at the declaration): none
// for .imsh, .ishdr and unknown types, ASSET_MOVE for a folder or a multi-selection.
EditorLayer::AssetDragPayload EditorLayer::AssetDragPayloadFor(int index) const
{
    AssetDragPayload out;
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return out;
    const AssetEntry& ae = m_assets[index];

    const char* payloadId = nullptr;
    const char* prefix    = "";
    switch (ae.type)
    {
    // A model (.iscn) dropped in the viewport is spawned.
    case AssetType::Scene:     payloadId = "ISCN_PATH";  prefix = "Spawn: ";     break;
    case AssetType::Prefab:    payloadId = "IPFB_PATH";  prefix = "Prefab: ";    break;
    case AssetType::Texture:   payloadId = "ITEX_PATH";  prefix = "";            break;
    case AssetType::Material:  payloadId = "IMAT_PATH";  prefix = "Material: ";  break;
    case AssetType::Skeleton:  payloadId = "ISKEL_PATH"; prefix = "Skeleton: ";  break;
    case AssetType::AnimGraph: payloadId = "IANIMGRAPH_PATH"; prefix = "Animation Graph: "; break;
    case AssetType::AnimSet: payloadId = "IANIMSET_PATH"; prefix = "Animation Set: "; break;
    case AssetType::Animation:
        payloadId = EndsWithCI(ae.path, ".attack.json") ? "ATTACK_PATH" :
            EndsWithCI(ae.path, ".attackset.json") ? "ATTACKSET_PATH" : "IANIM_PATH";
        prefix = "Animation: "; break;
    case AssetType::Audio:     payloadId = "ACLIP_PATH"; prefix = "Audio: ";     break;
    case AssetType::Script:    payloadId = "ILUA_PATH";  prefix = "Script: ";    break;
    case AssetType::Font:      payloadId = "IFONT_PATH"; prefix = "Font: ";      break;
    case AssetType::VFX:       payloadId = "IVFX_PATH";  prefix = "VFX: ";       break;
    case AssetType::Sequence:  payloadId = "ISEQ_PATH";  prefix = "Sequence: ";  break;
    // A .iui dropped on the UI Editor's designer or Hierarchy is made there.
    case AssetType::UIAsset:   payloadId = "IUI_PATH";   prefix = "UI: ";        break;
    // Face tracking: a mapping drops on FaceTracking's Mapping field; a take is named
    // by FaceTracking.OpenReplay (no field takes it yet).
    case AssetType::FaceMap:   payloadId = "IFACEMAP_PATH"; prefix = "Face Mapping: "; break;
    case AssetType::FaceTake:  payloadId = "IFACE_PATH";    prefix = "Face Take: ";    break;
    case AssetType::Mesh:
        // Only .meshlib is draggable, onto the material preview's mesh override: a .imsh shares
        // the type but has no runtime path to a mesh-descriptor slot, so its drop could only
        // fail. Told apart by the extension, like the Shader case below.
        if (EndsWithCI(ae.path, ".meshlib"))
        {
            payloadId = "IMESHLIB_PATH";
            prefix    = "Mesh: ";
        }
        break;
    case AssetType::Shader:
        // Only .hlsl source is draggable; compiled .ishdr blobs are derived and won't recompile on edit.
        if (ae.path.size() >= 5 &&
            std::string_view(ae.path).substr(ae.path.size() - 5) == ".hlsl")
        {
            payloadId = "IHLSL_PATH";
            prefix    = "Shader: ";
        }
        break;
    default: break;
    }
    // A folder is not droppable onto an Inspector field, but it IS draggable
    // into another folder, so it still gets a source.
    if (!payloadId && ae.type != AssetType::Folder) return out;

    // A real multi-selection moves as a set; anything else keeps its typed
    // single-asset payload so the existing consumers keep working.
    const bool multi = ae.type != AssetType::Folder
                    && IsAssetSelected(index)
                    && m_selectedAssetPaths.size() > 1;

    out.valid  = true;
    out.multi  = multi || ae.type == AssetType::Folder;
    out.id     = out.multi ? "ASSET_MOVE" : payloadId;
    out.prefix = prefix;
    return out;
}

// Drag source for one entry. Call while the item is still ImGui's last-submitted item:
// BeginDragDropSource reads it.
void EditorLayer::BeginAssetDragSource(int index)
{
    const AssetEntry&      ae = m_assets[index];
    const AssetDragPayload pl = AssetDragPayloadFor(index);
    if (!pl.valid) return;

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
    {
        // Exactly one SetDragDropPayload on every path: ImGui keeps one payload per source and a
        // second call replaces the first. AssetDragPayloadFor picked the id; this fills in the
        // data.
        if (pl.multi)
        {
            // Newline-separated so one drag carries the whole selection. A
            // folder drags as just itself.
            std::vector<std::string> moving;
            if (ae.type != AssetType::Folder) moving = SelectedAssetPaths();
            if (moving.empty()) moving.push_back(ae.path);

            std::string blob;
            for (const std::string& p : moving) { blob += p; blob += '\n'; }
            ImGui::SetDragDropPayload(pl.id, blob.c_str(), blob.size() + 1);

            if (ae.type == AssetType::Folder)
                ImGui::Text("%s  %s", FontAwesome::kFolder, ae.name.c_str());
            else
                ImGui::Text("%zu assets", moving.size());
        }
        else
        {
            ImGui::SetDragDropPayload(pl.id, ae.path.c_str(), ae.path.size() + 1);
            ImGui::Text("%s%s", pl.prefix, ae.stem.c_str());
        }
        ImGui::EndDragDropSource();
    }
}

// Selection / folder navigation / open-on-double-click, shared by every view. The click
// state is passed in so callers sample it before the drag-source block perturbs ImGui's
// last-item state.
void EditorLayer::HandleAssetItemActivate(int index, bool clicked, bool doubleClicked)
{
    const AssetEntry& ae = m_assets[index];

    if (doubleClicked)
    {
        if(ae.type==AssetType::AnimGraph){Editor::AnimGraphEditor::Open(ae.path);return;}
        if(ae.type==AssetType::AnimSet){Editor::AnimSetEditor::Open(ae.path);return;}
        if(ae.path.ends_with(".attack.json")||ae.path.ends_with(".attackset.json")){AttackEditor::Open(ae.path);return;}
        // Double-click: .ivfx -> VFX Editor, .iseq -> Sequencer, .imat / .imatinst -> the Material
        // Editor. Without the last, a material no entity uses, such as a new instance, would be
        // unreachable.
        if (ae.type == AssetType::VFX)      { OpenVFXAsset(ae.path);      return; }
        if (ae.type == AssetType::Sequence) { OpenSequenceAsset(ae.path); return; }
        if (ae.type == AssetType::Material)
        {
            Editor::MaterialEditorWindow::Get().RequestOpen(Sys::RelativizeAssetPath(ae.path));
            return;
        }
        // A skeleton (.iskel) opens in the Skeleton Editor, with its model's picture when the
        // same-stem .iscn beside it names it. A model (.iscn) expands into its Meshes instead;
        // its context menu has Open in Skeleton Editor.
        if (ae.type == AssetType::Skeleton)
        {
            Editor::SkeletonEditorWindow::Get().RequestOpen(Sys::RelativizeAssetPath(ae.path));
            return;
        }
        // A UI asset (.iui) opens in a UI Editor tab of its own.
        if (ae.type == AssetType::UIAsset)
        {
            Editor::UIEditorWindow::Get().RequestOpenAsset(Sys::RelativizeAssetPath(ae.path));
            return;
        }
        // A texture opens in the Texture Viewer.
        if (ae.type == AssetType::Texture && EndsWithCI(ae.path, ".itex"))
        {
            Editor::TextureViewerWindow::Get().RequestOpen(Sys::RelativizeAssetPath(ae.path));
            return;
        }
        // A model (.iscn) shows / hides its Meshes, like its arrow.
        if (IsModelExpandable(index))
        {
            const std::string key = Sys::RelativizeAssetPath(ae.path);
            if (!m_expandedModels.erase(key)) m_expandedModels.insert(key);
            return;
        }
        // A .meshlib / .ipfb opens the Asset Preview panel.
        if (PreviewKindForPath(ae.path) != Preview::AssetKind::None)
        {
            m_showAssetPreview = true;
            Editor::AssetPreviewWindow::Get().Show(ae.path);
            return;
        }
    }

    if (!clicked) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (ae.type == AssetType::Folder && !io.KeyCtrl && !io.KeyShift)
    {
        // By value before the rescan invalidates ae; SetCurrentAssetDir also
        // lets the folder tree know to reveal the new location.
        SetCurrentAssetDir(std::string(ae.path));
    }
    else
    {
        // A folder with a modifier held is being SELECTED, not entered: that is how folders are
        // picked for a move.
        SelectAsset(index, io.KeyCtrl  ? SelectMode::Toggle
                         : io.KeyShift ? SelectMode::Range
                                       : SelectMode::Replace);
    }
}

// One List/Details row: small icon + FULL file name (extension included), with
// the same selection / drag / open behaviour as a Tiles tile.
void EditorLayer::DrawAssetRowItem(int index, float iconSize, bool spanAllColumns)
{
    const AssetEntry& ae = m_assets[index];

    // Only submitted rows reach here; with the table clipper that means on-screen, the
    // visibility signal the streamer wants.
    StreamAssetThumbnail(index, true, static_cast<float>(ImGui::GetTime()));

    ImGui::PushID(index);
    if (IsModelExpandable(index))
    {
        // A model expands into its Meshes (sub-rows follow it).
        DrawModelExpandArrow(index, ImGui::GetCursorScreenPos(), iconSize);
        ImGui::Dummy(ImVec2(iconSize, iconSize));
        ImGui::SameLine(0.f, 2.f);
    }
    DrawAssetItemIcon(index, iconSize);
    ImGui::SameLine(0.f, 6.f);

    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowOverlap;
    if (spanAllColumns) flags |= ImGuiSelectableFlags_SpanAllColumns;

    const bool clicked = ImGui::Selectable(ae.name.c_str(),
                                           m_selectedAsset == index || IsAssetSelected(index),
                                           flags, ImVec2(0.f, iconSize));
    WireAssetItemInteractions(index, clicked);
    ImGui::PopID();
}

// Every behaviour an asset item offers, in one place (contract at the declaration).
void EditorLayer::WireAssetItemInteractions(int index, bool clicked)
{
    const bool hovered = ImGui::IsItemHovered();
    const bool dbl     = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    // Record where a model's item was drawn: an OS file dropped on it reimports it.
    if (index >= 0 && index < static_cast<int>(m_assets.size()))
    {
        const std::string& p = m_assets[index].path;
        if (p.size() > 5 && Resource::AssetRefs::ToLowerAscii(p.substr(p.size() - 5)) == ".iscn")
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            m_modelTileRects.push_back({ a.x, a.y, b.x, b.y, p });
        }
    }

    BeginAssetDragSource(index);
    // A folder row/tile is a drop target: this is how assets get moved.
    if (index >= 0 && index < static_cast<int>(m_assets.size())
        && m_assets[index].type == AssetType::Folder)
        AcceptAssetMoveDrop(m_assets[index].path);
    if (hovered && index >= 0 && index < static_cast<int>(m_assets.size()))
    {
        // A texture's tooltip says what it is - "2048x2048 . BC7 sRGB . 12 mips . 5.3 MB".
        if (const Resource::TextureFileInfo* ti = AssetTextureInfo(index))
            ImGui::SetTooltip("%s\n%s", m_assets[index].path.c_str(), Resource::TextureInfoSummary(*ti).c_str());
        else
            ImGui::SetTooltip("%s", m_assets[index].path.c_str());
    }
    HandleAssetItemActivate(index, clicked, dbl);
    RenderAssetItemContextMenu(index);
}

// ==== Models expand into their Meshes ====
bool EditorLayer::IsModelExpandable(int index) const
{
    return index >= 0 && index < static_cast<int>(m_assets.size()) &&
           m_assets[index].type == AssetType::Scene && EndsWithCI(m_assets[index].path, ".iscn");
}

void EditorLayer::RebuildAssetDisplayRows()
{
    m_assetRows.clear();
    m_assetRows.reserve(m_assetOrder.size());
    MeshAssetRegistry* reg   = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
    const uint32_t     epoch = m_world ? m_world->Epoch() : 0u;
    for (int i : m_assetOrder)
    {
        if (i < 0 || i >= static_cast<int>(m_assets.size())) continue;
        m_assetRows.push_back({ i, -1, nullptr });
        if (!reg || !IsModelExpandable(i) ||
            !m_expandedModels.count(Sys::RelativizeAssetPath(m_assets[i].path))) continue;
        // Loads the model's libraries as world libraries - what placing one
        // would do anyway; cached until the world changes.
        const MeshAssetRegistry::Model& model = reg->GetModel(m_assets[i].path, epoch);
        for (int k = 0; k < static_cast<int>(model.meshes.size()); ++k)
            if (!(model.meshes[k].flags & Resource::MESH_IDENT_HIDDEN))
                m_assetRows.push_back({ i, k, &model });
    }
}

const MeshAssetRegistry::Mesh* EditorLayer::ModelMeshAt(const AssetDisplayRow& row) const
{
    if (!row.model || row.mesh < 0 || row.mesh >= static_cast<int>(row.model->meshes.size())) return nullptr;
    return &row.model->meshes[row.mesh];
}

void EditorLayer::DrawModelExpandArrow(int index, const ImVec2& min, float size)
{
    if (!IsModelExpandable(index)) return;
    // Keyed by the canonical spelling (Editor.Browse names models that way).
    const std::string  path     = Sys::RelativizeAssetPath(m_assets[index].path);
    const bool         expanded = m_expandedModels.count(path) != 0;
    const ImVec2       max(min.x + size, min.y + size);
    const bool         hovered  = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                  ImGui::IsMouseHoveringRect(min, max);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, hovered ? IM_COL32(90, 150, 220, 220) : IM_COL32(20, 20, 28, 170), 3.f);
    ImGui::RenderArrow(dl, ImVec2(min.x + size * 0.18f, min.y + size * 0.18f), IM_COL32(230, 230, 240, 255),
                       expanded ? ImGuiDir_Down : ImGuiDir_Right, size * 0.64f / ImGui::GetFontSize());
    if (hovered)
    {
        ImGui::SetTooltip(expanded ? "Hide this model's meshes" : "Show this model's meshes (drag one into the scene)");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (expanded) m_expandedModels.erase(path);
            else          m_expandedModels.insert(path);
        }
    }
}

Entity EditorLayer::PlaceBrowserMesh(const AssetDisplayRow& row)
{
    const MeshAssetRegistry::Mesh* mesh = ModelMeshAt(row);
    if (!mesh || !m_world || row.asset < 0 || row.asset >= static_cast<int>(m_assets.size())) return NullEntity;
    const std::string model = Sys::RelativizeAssetPath(m_assets[row.asset].path);
    const uint64_t    key   = mesh->key;
    const std::string name  = mesh->name;
    Entity e = NullEntity;
    RecordSpawn("Place Mesh", [&] { e = CreateStaticMeshEntity(model, key, name, ComputeSpawnPosition()); });
    if (e != NullEntity)
    {
        m_selectedEntity = e;
        MarkHierarchyDirty();
        LOG_SUCCESS("EditorLayer: placed mesh '%s' of '%s'", name.c_str(), model.c_str());
    }
    return e;
}

// Drag source, tooltip, double-click and right-click of a Mesh item - the
// sub-item twin of WireAssetItemInteractions. Call right after the item.
void EditorLayer::WireModelMeshInteractions(const AssetDisplayRow& row)
{
    const MeshAssetRegistry::Mesh* mesh = ModelMeshAt(row);
    if (!mesh) return;
    // Resource::MeshCountsAsSkinned: per Mesh (a static prop in a character
    // model, or one whose shape keys gave it a one-bone skeleton, is not).
    const bool        skinned = mesh->skinned;
    const bool        hovered = ImGui::IsItemHovered();
    const std::string model   = Sys::RelativizeAssetPath(m_assets[row.asset].path);
    const std::string keyHex  = Resource::MeshKeyToHex(mesh->key);

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
    {
        // "model\nkeyhex\nname" - EditorLayer::ParseStaticMeshRef. Deliberately
        // NOT a *_PATH payload: the folder drop targets read those as a file
        // to move (AcceptAssetMoveDrop).
        const std::string payload = model + "\n" + keyHex + "\n" + mesh->name;
        ImGui::SetDragDropPayload("STATICMESH_REF", payload.c_str(), payload.size() + 1);
        ImGui::Text("Mesh: %s", mesh->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (hovered && !ImGui::IsDragDropActive())
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(mesh->name.c_str());
        std::string slots;
        for (const MeshAssetRegistry::Section& s : mesh->sections)
            slots += (slots.empty() ? "" : ", ") + (s.slot.empty() ? std::string("(unnamed)") : s.slot);
        ImGui::TextDisabled("%zu material slot%s: %s", mesh->sections.size(),
                            mesh->sections.size() == 1 ? "" : "s", slots.c_str());
        ImGui::TextDisabled("key %s  -  %s", keyHex.c_str(), model.c_str());
        if (skinned)
            ImGui::TextDisabled("skinned: placed as a Static Mesh it shows the bind pose");
        if (mesh->synthesized)
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
                               "legacy library: its keys are positional until it is backfilled");
        ImGui::TextDisabled("drag into the scene, or double-click to place it");
        ImGui::EndTooltip();
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) PlaceBrowserMesh(row);
    }
    if (ImGui::BeginPopupContextItem("MeshItemContext"))
    {
        ImGui::TextDisabled("%s", mesh->name.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Place in Scene")) PlaceBrowserMesh(row);
        // The Mesh Editor - view modes, overlays, UVs, default slots.
        if (ImGui::MenuItem("Open in Mesh Editor"))
            Editor::MeshEditorWindow::Get().RequestOpen(model, mesh->key, mesh->name);
        // The Asset Preview panel on this ONE Mesh (Preview::AssetKind::MeshAsset) - its
        // sections in its own space, the model's materials.
        if (ImGui::MenuItem("Preview"))
        {
            m_showAssetPreview = true;
            Editor::AssetPreviewWindow::Get().Show(Resource::MeshAssetRef(model, mesh->key));
        }
        // The Mesh as a model of its own (MeshExtract); the dialog asks where.
        const bool canExtract = !skinned && !(mesh->flags & Resource::MESH_IDENT_HAS_MORPH);
        if (ImGui::MenuItem("Extract Mesh...", nullptr, false, canExtract))
        {
            m_extractDialog          = ExtractDialog{};
            m_extractDialog.open     = true;
            m_extractDialog.model    = model;
            m_extractDialog.key      = mesh->key;
            m_extractDialog.meshName = mesh->name;
            const std::string d = MeshExtract::DefaultDestination(model, mesh->name);
            std::snprintf(m_extractDialog.dest, sizeof(m_extractDialog.dest), "%s", d.c_str());
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(canExtract ? "Write this mesh out as a model of its own (.iscn + .meshlib)"
                                         : "A skinned mesh / one with shape keys cannot be extracted yet");
        if (ImGui::MenuItem("Copy Mesh Reference"))
            ImGui::SetClipboardText((model + "#" + keyHex).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("<model>#<MeshKey> - the pair a Static Mesh stores");
        ImGui::EndPopup();
    }
}

// The "Extract Mesh..." modal, opened by a Mesh item's menu. Drawn at the
// Content Browser window's scope (OpenPopup and BeginPopupModal must share an
// ID stack; the item's context menu is gone by the time it opens).
void EditorLayer::RenderExtractMeshDialog()
{
    static constexpr const char* kId = "Extract Mesh###ExtractMeshDialog";
    if (m_extractDialog.open)
    {
        ImGui::OpenPopup(kId);
        m_extractDialog.open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ExtractDialog& d = m_extractDialog;
    ImGui::Text("Write '%s' out as a model of its own.", d.meshName.c_str());
    ImGui::TextDisabled("%s  #%s", d.model.c_str(), Resource::MeshKeyToHex(d.key).c_str());
    ImGui::Spacing();
    ImGui::SetNextItemWidth(420.f);
    ImGui::InputText("Destination (.iscn)", d.dest, sizeof(d.dest));
    ImGui::TextDisabled("Its .meshlib is written beside it with the same name. The new model keeps the\n"
                        "mesh's key, so a Static Mesh can point at it and draw the same thing.");
    ImGui::Checkbox("Point this scene's Static Meshes of it at the new model", &d.retarget);
    if (!d.lastError.empty())
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", d.lastError.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Extract", ImVec2(120.f, 0.f)))
    {
        MeshExtractOutcome res;
        if (ExtractModelMesh(d.model, d.key, d.dest, d.retarget, /*overwrite=*/false, res))
        {
            LOG_SUCCESS("EditorLayer: extracted '%s' -> '%s' (%u section(s)); %u Static Mesh(es) re-pointed",
                        d.meshName.c_str(), res.file.iscn.c_str(), res.file.sections, res.retargeted);
            d.lastError.clear();
            ImGui::CloseCurrentPopup();
        }
        else
            d.lastError = res.file.error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.f, 0.f)))
    {
        d.lastError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

EditorLayer::AssetEntry* EditorLayer::MeshTileFor(const AssetDisplayRow& row, bool create)
{
    const MeshAssetRegistry::Mesh* mesh = ModelMeshAt(row);
    if (!mesh || row.asset < 0 || row.asset >= static_cast<int>(m_assets.size())) return nullptr;
    const std::string ref = Resource::MeshAssetRef(m_assets[row.asset].path, mesh->key);
    if (!create)
    {
        auto it = m_meshTiles.find(ref);
        return it != m_meshTiles.end() ? &it->second : nullptr;
    }
    auto [it, added] = m_meshTiles.try_emplace(ref);
    if (added)
    {
        it->second.path = ref;
        it->second.stem = it->second.name = mesh->name;
        it->second.type = AssetType::Mesh;
    }
    return &it->second;
}

void EditorLayer::DrawModelMeshTile(const AssetDisplayRow& row, float tileSize, float labelH)
{
    const MeshAssetRegistry::Mesh* mesh = ModelMeshAt(row);
    if (!mesh) return;
    const ImVec2 tileMin = ImGui::GetCursorScreenPos();

    // The tile's picture streams like a folder entry's, visibility included, so an
    // off-screen Mesh item ages out on the same clock - and is not even created while
    // off-screen (the grid submits every tile).
    const float winTop = ImGui::GetWindowPos().y;
    const float winBot = winTop + ImGui::GetWindowHeight();
    const bool  vis    = (tileMin.y + tileSize + labelH > winTop) && (tileMin.y < winBot);
    AssetEntry* tile   = MeshTileFor(row, /*create=*/vis);
    if (tile) StreamEntryThumbnail(*tile, vis, static_cast<float>(ImGui::GetTime()));

    ImGui::PushID("mesh");
    ImGui::PushID(row.asset);
    ImGui::PushID(row.mesh);
    ImGui::InvisibleButton("##meshtile", ImVec2(tileSize, tileSize + labelH));
    const bool hovered = ImGui::IsItemHovered();
    WireModelMeshInteractions(row);
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();

    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const ImVec2 tileMax(tileMin.x + tileSize, tileMin.y + tileSize);
    dl->AddRectFilled(tileMin, ImVec2(tileMax.x, tileMax.y + labelH + 2.f),
                      hovered ? IM_COL32(70, 95, 110, 200) : IM_COL32(28, 40, 48, 200), 4.f);
    if (const uint64_t srv = tile ? GetEntryThumbnailSrv(*tile) : 0)
    {
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(srv)),
                     ImVec2(tileMin.x + 8.f, tileMin.y + 8.f), ImVec2(tileMax.x - 8.f, tileMax.y - 8.f));
    }
    else
    {
        dl->AddRectFilled(ImVec2(tileMin.x + 8.f, tileMin.y + 8.f), ImVec2(tileMax.x - 8.f, tileMax.y - 8.f),
                          IM_COL32(40, 110, 120, 255), 3.f);
        const char*  icon  = m_hasFontAwesomeIcons ? FontAwesome::kCube : "M";
        const ImVec2 isz   = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(tileMin.x + (tileSize - isz.x) * 0.5f, tileMin.y + (tileSize - isz.y) * 0.5f - 6.f),
                    IM_COL32(255, 255, 255, 230), icon);
    }
    char slots[32];
    std::snprintf(slots, sizeof(slots), "%zu slot%s", mesh->sections.size(), mesh->sections.size() == 1 ? "" : "s");
    const ImVec2 ssz = ImGui::CalcTextSize(slots);
    dl->AddRectFilled(ImVec2(tileMin.x + (tileSize - ssz.x) * 0.5f - 3.f, tileMax.y - 9.f - ssz.y),
                      ImVec2(tileMin.x + (tileSize + ssz.x) * 0.5f + 3.f, tileMax.y - 7.f),
                      IM_COL32(10, 20, 24, 150), 2.f);
    dl->AddText(ImVec2(tileMin.x + (tileSize - ssz.x) * 0.5f, tileMax.y - 8.f - ssz.y),
                IM_COL32(200, 230, 235, 220), slots);

    // Label: the mesh name, ellipsized to the tile.
    const std::string& name  = mesh->name;
    const float        maxW  = tileSize - 2.f;
    const ImVec2       lpos(tileMin.x + 1.f, tileMax.y + 2.f);
    if (ImGui::CalcTextSize(name.c_str()).x <= maxW)
        dl->AddText(lpos, IM_COL32(170, 220, 225, 255), name.c_str());
    else
    {
        const float ew = ImGui::CalcTextSize("\xe2\x80\xa6").x;
        int lo = 0, hi = static_cast<int>(name.size());
        while (lo < hi)
        {
            const int mid = (lo + hi + 1) / 2;
            if (ImGui::CalcTextSize(name.c_str(), name.c_str() + mid).x + ew <= maxW) lo = mid;
            else hi = mid - 1;
        }
        const std::string clipped = name.substr(0, static_cast<size_t>(lo)) + "\xe2\x80\xa6";
        dl->AddText(lpos, IM_COL32(170, 220, 225, 255), clipped.c_str());
    }
}

void EditorLayer::DrawModelMeshRowItem(const AssetDisplayRow& row, float iconSize, bool spanAllColumns)
{
    const MeshAssetRegistry::Mesh* mesh = ModelMeshAt(row);
    if (!mesh) return;
    ImGui::PushID("mesh");
    ImGui::PushID(row.asset);
    ImGui::PushID(row.mesh);
    ImGui::Dummy(ImVec2(iconSize, iconSize));   // indent under the model's arrow
    ImGui::SameLine(0.f, 2.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    // Only submitted rows reach here (the views clip), so this row is on screen.
    AssetEntry* tile = MeshTileFor(row);
    if (tile) StreamEntryThumbnail(*tile, true, static_cast<float>(ImGui::GetTime()));
    if (const uint64_t srv = tile ? GetEntryThumbnailSrv(*tile) : 0)
    {
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(srv)), p0, ImVec2(p0.x + iconSize, p0.y + iconSize));
    }
    else
    {
        dl->AddRectFilled(p0, ImVec2(p0.x + iconSize, p0.y + iconSize), IM_COL32(40, 110, 120, 255), 2.f);
        const char*  icon = m_hasFontAwesomeIcons ? FontAwesome::kCube : "M";
        const ImVec2 isz  = ImGui::CalcTextSize(icon);
        dl->PushClipRect(p0, ImVec2(p0.x + iconSize, p0.y + iconSize), true);
        dl->AddText(ImVec2(p0.x + (iconSize - isz.x) * 0.5f, p0.y + (iconSize - isz.y) * 0.5f),
                    IM_COL32(255, 255, 255, 230), icon);
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(iconSize, iconSize));
    ImGui::SameLine(0.f, 6.f);
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowOverlap;
    if (spanAllColumns) flags |= ImGuiSelectableFlags_SpanAllColumns;
    ImGui::Selectable(mesh->name.c_str(), false, flags, ImVec2(0.f, iconSize));
    WireModelMeshInteractions(row);
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

// Per-asset right-click menu: the open / preview / reimport / instance actions, then
// Rename, Delete, Find References. References are path strings: Rename repoints the .meta
// identity and, with "Repoint references" ticked, rewrites what the reference index parses
// (.iscene / .ipfb / .imat / .imatinst); Lua scripts, game.json and save files are not
// indexed, and the menu says so.
void EditorLayer::RenderAssetItemContextMenu(int index)
{
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return;
    // By value: the operations below can flip m_needsRescan, and a later
    // rescan reallocates m_assets out from under a reference.
    const std::string assetPath = m_assets[index].path;
    const std::string assetName = m_assets[index].name;

    if (!ImGui::BeginPopupContextItem("AssetItemContext"))
        return;

    ImGui::TextDisabled("%s", assetName.c_str());
    ImGui::Separator();

    namespace fs = std::filesystem;

    // ---- Preview (mesh / prefab / model) ----
    // Gated on the extension (PreviewKindForPath): a .imsh is AssetType::Mesh too and has no
    // runtime path to a mesh-descriptor slot.
    if (PreviewKindForPath(assetPath) != Preview::AssetKind::None)
    {
        if (ImGui::MenuItem("Preview"))
        {
            m_showAssetPreview = true;
            Editor::AssetPreviewWindow::Get().Show(assetPath);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show this asset in the Asset Preview panel, with\n"
                              "its own materials on the studio rig.\n\n"
                              "Skinned meshes are drawn at their REST (bind) pose;\n"
                              "particles, trails, decals and lights are not drawn.");
        ImGui::Separator();
    }

    if (EndsWithCI(assetPath, ".ianimgraph") || EndsWithCI(assetPath, ".ianimset"))
    {
        const bool graph = EndsWithCI(assetPath, ".ianimgraph");
        if (ImGui::MenuItem(graph ? "Open Animation Graph" : "Open Animation Set"))
        {
            if (graph) Editor::AnimGraphEditor::Open(assetPath);
            else Editor::AnimSetEditor::Open(assetPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }

    // ---- Open in Material Editor ----
    if (EndsWithCI(assetPath, ".imat") || EndsWithCI(assetPath, ".imatinst"))
    {
        if (ImGui::MenuItem("Open in Material Editor"))
        {
            Editor::MaterialEditorWindow::Get().RequestOpen(Sys::RelativizeAssetPath(assetPath));
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Its own window: a preview of the material as edited, every\n"
                              "parameter, its own undo, Save / Save As / Save as Instance.");
        ImGui::Separator();
    }

    // ---- Open in UI Editor ----
    // A .iui edits there; a .ipfb whose root is a UI node opens read-only (Convert to UI
    // Asset makes the .iui). Whether a .ipfb is UI is read once per popup per file.
    {
        static std::string s_uiFor;
        static bool        s_uiTemplate = false;
        const bool iui = EndsWithCI(assetPath, ".iui");
        if (!iui && EndsWithCI(assetPath, ".ipfb") && s_uiFor != assetPath)
        {
            s_uiFor      = assetPath;
            s_uiTemplate = Editor::UIEditorWindow::IsUITemplateFile(assetPath);
        }
        if (iui || (EndsWithCI(assetPath, ".ipfb") && s_uiTemplate))
        {
            if (ImGui::MenuItem(iui ? "Open in UI Editor" : "Open in UI Editor (read-only)"))
            {
                Editor::UIEditorWindow::Get().RequestOpenAsset(Sys::RelativizeAssetPath(assetPath));
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(iui ? "Its own tab: the designer, its own undo, Save / Save As / Revert."
                                      : "A UI template in a read-only tab; Convert to UI Asset writes it as a .iui.");
            ImGui::Separator();
        }
    }

    // ---- Open in Texture Viewer ----
    if (EndsWithCI(assetPath, ".itex"))
    {
        if (ImGui::MenuItem("Open in Texture Viewer"))
        {
            Editor::TextureViewerWindow::Get().RequestOpen(Sys::RelativizeAssetPath(assetPath));
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Each channel alone, any mip, a cube's faces, HDR exposure,\n"
                              "and what the file is: format, size, mips, memory, users.");

        // Reimport through the import it came from: the .meta's import record or, for a texture
        // imported before records existed, a guess (the format its DXGI format names, a
        // same-named image beside it). Read once per popup-open per texture.
        static std::string                   s_trFor;
        static Resource::TextureImportRecord s_tr;
        static bool                          s_trThere = false;
        if (s_trFor != assetPath)
        {
            s_trFor = assetPath;
            s_tr    = Resource::TextureImportRecordFor(Sys::RelativizeAssetPath(assetPath));
            std::error_code ec;
            s_trThere = !s_tr.source.empty() && fs::is_regular_file(Sys::U8Path(s_tr.source), ec);
        }
        ImGui::BeginDisabled(!s_trThere);
        if (ImGui::MenuItem("Reimport"))
        {
            ImGui::CloseCurrentPopup();
            QueueTextureReimport(Sys::RelativizeAssetPath(assetPath), std::string());
            s_trFor.clear();   // the record changes
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (s_tr.source.empty())
                ImGui::SetTooltip("No source is recorded and no same-named image is beside it.\n"
                                  "Reimport From... names one.");
            else if (!s_trThere)
                ImGui::SetTooltip("Its source is not there:\n%s\n\nReimport From... names another.",
                                  s_tr.source.c_str());
            else
                ImGui::SetTooltip("Import it again from\n%s\nas %s%s - swapped in place, same guid.",
                                  s_tr.source.c_str(), Resource::TextureImporter::GetFormatName(s_tr.settings.format),
                                  s_tr.recorded ? "" : "\n(guessed: imported before settings were recorded)");
        }
        if (ImGui::MenuItem("Reimport From..."))
        {
            ImGui::CloseCurrentPopup();
            static const char kFilterImage[] =
                "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr\0All Files\0*.*\0\0";
            const std::string picked = OpenFileDialog(kFilterImage, m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!picked.empty()) QueueTextureReimport(Sys::RelativizeAssetPath(assetPath), picked);
            s_trFor.clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Import this texture again from another image - same name,\n"
                              "same guid, same settings; the source is recorded.");
        if (ImGui::MenuItem("Import Settings..."))
        {
            Editor::TextureViewerWindow::Get().RequestImportSettings(Sys::RelativizeAssetPath(assetPath));
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Its source, format, size cap, mip filter, green flip and\n"
                              "mips - in the Texture Viewer, with Reimport.");
        ImGui::Separator();
    }

    // ---- Open in Skeleton Editor ----
    // A skeleton (.iskel), or a model naming one (its K line, read once per popup-open per
    // model).
    {
        const std::string lower = Resource::AssetRefs::ToLowerAscii(assetPath);
        const bool isIskel = lower.size() > 6 && lower.compare(lower.size() - 6, 6, ".iskel") == 0;
        const bool isIscn  = lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".iscn") == 0;
        static std::string s_skelFor;
        static bool        s_skelHave = false;
        if (isIscn && s_skelFor != assetPath)
        {
            s_skelFor  = assetPath;
            s_skelHave = !SceneInstanceLoader::ModelSkeletonPath(Sys::RelativizeAssetPath(assetPath)).empty();
        }
        if (isIskel || (isIscn && s_skelHave))
        {
            if (ImGui::MenuItem("Open in Skeleton Editor"))
            {
                Editor::SkeletonEditorWindow::Get().RequestOpen(Sys::RelativizeAssetPath(assetPath));
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The skeleton's bones, their categories (IK, 付与, twist, chain\n"
                                  "physics, humanoid roles) and its problems, over the model.");
            ImGui::Separator();
        }
    }

    // ---- Reimport (a model) ----
    // Through the import it came from: the source its SR record names, or one picked
    // (Reimport From... - the record follows it). A change that loses something (a Mesh or a
    // slot gone) or guesses (a rename by geometry) opens the review first; anything else is
    // written at once.
    if (assetPath.size() > 5 && Resource::AssetRefs::ToLowerAscii(assetPath.substr(assetPath.size() - 5)) == ".iscn")
    {
        // Read once per popup-open per model: the record and whether its source is there.
        static std::string                 s_srFor;
        static Resource::ModelSourceRecord s_sr;
        static bool                        s_srHave = false, s_srThere = false;
        if (s_srFor != assetPath)
        {
            s_srFor   = assetPath;
            s_srHave  = Resource::ReadModelSource(assetPath, s_sr);
            std::error_code ec;
            s_srThere = s_srHave && fs::is_regular_file(Sys::U8Path(s_sr.path), ec);
        }
        const bool importBusy = ReviewPending() || m_importDialog != nullptr;
        ImGui::BeginDisabled(!s_srThere || importBusy);
        if (ImGui::MenuItem("Reimport"))
        {
            ImGui::CloseCurrentPopup();
            if (s_sr.importer == "ue")
            {
                // An Unreal scene: its export manifest cooked again, into the scene's own folder under
                // its own name.
                const fs::path model = Sys::U8Path(assetPath);
                BeginUnrealManifestImport(s_sr.path, Sys::PathToUtf8(model.parent_path().parent_path()),
                                          Sys::PathToUtf8(model.stem()));
            }
            else
                BeginModelReimport(assetPath, std::string());
            s_srFor.clear();   // the record may have changed
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (!s_srHave)
                ImGui::SetTooltip("This model records no source (it was imported before the\n"
                                  "source was recorded, or not by a model importer).\n"
                                  "Reimport From... names one.");
            else if (!s_srThere)
                ImGui::SetTooltip("Its source is not there:\n%s\n\nReimport From... names another.",
                                  s_sr.path.c_str());
            else
                ImGui::SetTooltip("Import it again from\n%s\n\nKeys, materials, collision and shape keys\n"
                                  "authored since are kept. A change that removes a\n"
                                  "Mesh or a material slot asks first.", s_sr.path.c_str());
        }
        ImGui::BeginDisabled(importBusy);
        if (ImGui::MenuItem("Reimport From..."))
        {
            ImGui::CloseCurrentPopup();
            static const char kFilterModel[] =
                "3D Model Files\0*.obj;*.gltf;*.pmx;*.glb;*.vrm;*.fbx;*.dae;*.3ds;*.ply;*.stl\0All Files\0*.*\0\0";
            const std::string picked = OpenFileDialog(kFilterModel, m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!picked.empty())
                BeginModelReimport(assetPath, picked);
            s_srFor.clear();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Import this model again from another source file - it keeps\n"
                              "its name, folder and keys; the source is recorded for the\n"
                              "next Reimport.");
        // The import options first, started from the model's recorded ones.
        ImGui::BeginDisabled(!s_srThere || importBusy);
        if (ImGui::MenuItem("Reimport With Options..."))
        {
            ImGui::CloseCurrentPopup();
            Resource::ModelImportOptions start;
            Resource::ReadImportOptions(assetPath, start);
            OpenModelImportDialog({}, std::string(), assetPath, start);
            s_srFor.clear();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Import it again with other options (normals, shape keys,\n"
                              "collision, materials, textures) - the model records them\n"
                              "and the next Reimport uses them again.");
        ImGui::Separator();
    }

    // ---- Create Material Instance ----
    // Gated on the extension: .imat and .imatinst share AssetType::Material and AssetEntry
    // does not keep the extension. Offered for both: an instance of an instance is legal (a
    // per-level or per-variant tweak on top of a shared look).
    {
        const bool isImat =
            assetPath.size() >= 5 &&
            std::string_view(assetPath).substr(assetPath.size() - 5) == ".imat";
        const bool isImatInst =
            assetPath.size() >= 9 &&
            std::string_view(assetPath).substr(assetPath.size() - 9) == ".imatinst";
        if (isImat || isImatInst)
        {
            if (ImGui::MenuItem("Create Material Instance"))
            {
                // assetPath is captured BY VALUE above precisely for this: the
                // call sets m_needsRescan and the rescan reallocates m_assets.
                const std::string made = CreateMaterialInstance(assetPath);
                if (!made.empty())
                {
                    // Open it straight away in the Material Editor: a new asset in a re-sorted grid is
                    // hard to find.
                    Editor::MaterialEditorWindow::Get().RequestOpen(Sys::RelativizeAssetPath(made));
                }
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("New .imatinst that inherits everything from this\n"
                                  "material, opened in the Material Editor: a value you\n"
                                  "change there is overridden; the rest keeps following\n"
                                  "the parent.");
            ImGui::Separator();
        }
    }

    // Referrer count for the Rename/Delete blurbs. Built once per popup-open
    // per asset: warm that is free, cold it is the same sub-second scan
    // "Find References" already pays.
    static int         s_ctxRefCount = -1;
    static std::string s_ctxRefFor;
    if (s_ctxRefFor != assetPath || !m_refScanner.Built())
    {
        s_ctxRefFor = assetPath;
        if (!m_refScanner.Built()) m_refScanner.Rebuild(m_assetDir);
        s_ctxRefCount = static_cast<int>(m_refScanner.FindReferrers(assetPath).size());
    }

    if (ImGui::BeginMenu("Rename"))
    {
        static char s_renameBuf[260] = {};
        static int  s_renameFor = -1;
        static bool s_repointRefs = true;
        if (s_renameFor != index)
        {
            s_renameFor = index;
            strncpy_s(s_renameBuf,
                      Sys::PathToUtf8(Sys::U8Path(assetPath).filename()).c_str(),
                      _TRUNCATE);
        }
        ImGui::SetNextItemWidth(280.f);
        const bool commit = ImGui::InputText("##assetRename", s_renameBuf,
                                             sizeof(s_renameBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Checkbox("Repoint references", &s_repointRefs);
        if (s_ctxRefCount > 0)
            ImGui::TextDisabled("Enter commits. %d indexed reference(s) will be\n"
                                "%s.", s_ctxRefCount,
                                s_repointRefs ? "rewritten to the new path"
                                              : "LEFT DANGLING");
        else
            ImGui::TextDisabled("Enter commits. No indexed references to this asset.");
        ImGui::TextDisabled("Lua scripts, game.json and save files are not\n"
                            "indexed - those keep the old path either way.");
        if (commit && s_renameBuf[0])
        {
            std::error_code ec;
            const fs::path oldP = Sys::U8Path(assetPath);
            const fs::path newP = oldP.parent_path() / Sys::U8Path(std::string(s_renameBuf));
            if (fs::exists(newP, ec))
                LOG_WARNING("Asset rename refused: '%s' already exists",
                            Sys::PathToUtf8(newP).c_str());
            else
            {
                fs::rename(oldP, newP, ec);
                if (ec)
                    LOG_ERROR("Asset rename failed: '%s' -> '%s' (%s)",
                              assetPath.c_str(), Sys::PathToUtf8(newP).c_str(),
                              ec.message().c_str());
                else
                {
                    const std::string newRel =
                        Sys::RelativizeAssetPath(Sys::PathToUtf8(newP));
                    LOG_INFO("Asset renamed: '%s' -> '%s'",
                             assetPath.c_str(), newRel.c_str());
                    // Identity first: the sidecar has to follow the asset
                    // before anything re-derives a guid from the new path,
                    // or the rename would mint a SECOND identity for it and
                    // strand every reference that still holds the first.
                    Resource::AssetGuidIndex::Get().Repoint(assetPath, newRel);
                ClearAssetSelection();
                    if (s_repointRefs)
                        RepointAssetReferences(assetPath, newRel);
                    m_selectedAsset = -1;
                    m_needsRescan   = true;
                    s_ctxRefFor.clear();
                }
            }
            s_renameFor = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    // A multi-selection deletes as a set, but ONLY when the right-clicked item is part of it;
    // right-clicking outside the selection means "this one".
    const bool deleteWholeSelection =
        m_selectedAssetPaths.size() > 1
        && std::find(m_selectedAssetPaths.begin(), m_selectedAssetPaths.end(), assetPath)
               != m_selectedAssetPaths.end();

    if (ImGui::BeginMenu(deleteWholeSelection ? "Delete selection" : "Delete"))
    {
        if (deleteWholeSelection)
            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f),
                               "%zu assets will be deleted.", m_selectedAssetPaths.size());
        ImGui::TextDisabled("Moves the file to the Recycle Bin.");
        if (deleteWholeSelection)
        {
            // Counting every victim's referrers would cost N index queries per frame while the menu
            // is open, so say that the count covers one asset, not the set.
            ImGui::TextDisabled("Use \"Find References\" per asset to check referrers.");
        }
        else if (s_ctxRefCount > 0)
        {
            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f),
                               "%d reference(s) will be left dangling:", s_ctxRefCount);
            // The referrers by name, so the decision is made seeing them.
            const auto refs = m_refScanner.FindReferrers(assetPath);
            const std::size_t shown = (std::min<std::size_t>)(refs.size(), 8);
            for (std::size_t i = 0; i < shown; ++i)
                ImGui::BulletText("%s (%s)", refs[i].file.c_str(), refs[i].key.c_str());
            if (refs.size() > shown) ImGui::TextDisabled("... and %zu more", refs.size() - shown);
        }
        else
            ImGui::TextDisabled("No indexed references to this asset.");
        if (s_ctxRefCount != 0 && ImGui::MenuItem("Show references first"))
        {
            m_refQueryPath         = assetPath;
            m_showReferencesWindow = true;
        }
        if (ImGui::MenuItem("Confirm delete"))
        {
            std::vector<std::string> victims;
            if (deleteWholeSelection) victims = SelectedAssetPaths();
            else                      victims.push_back(assetPath);

            std::size_t deleted = 0;
            for (const std::string& victim : victims)
            {
                std::string why;
                if (!MoveToRecycleBin(WithSidecar(victim), why))
                {
                    LOG_ERROR("Asset delete failed: '%s' (%s)", victim.c_str(), why.c_str());
                    continue;
                }
                LOG_INFO("Asset deleted: '%s'", victim.c_str());
                // The sidecar went to the Recycle Bin with the asset; drop its guid from the index, or
                // a file that later takes the path would inherit it.
                Resource::AssetGuidIndex::Get().Forget(victim);
                ++deleted;
            }
            if (deleted > 1) LOG_SUCCESS("Deleted %zu assets", deleted);
            ClearAssetSelection();
            m_needsRescan = true;
            s_ctxRefFor.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (!std::filesystem::is_directory(Sys::U8Path(assetPath)) && ImGui::MenuItem("View Bytes"))
        OpenByteViewForFile(assetPath);
    if (ImGui::MenuItem("Find References"))
    {
        // Rebuild is incremental (mtime cache): a warm re-click is instant, a cold one sub-second.
        m_refQueryPath         = assetPath;
        m_showReferencesWindow = true;
        m_refScanner.Rebuild(m_assetDir);
    }
    if (ImGui::MenuItem("Show in Explorer"))
    {
        std::error_code ec;
        const fs::path abs = fs::absolute(Sys::U8Path(assetPath), ec);
        if (!ec)
        {
            const std::wstring arg = L"/select," + abs.native();
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(),
                          nullptr, SW_SHOWNORMAL);
        }
    }
    if (ImGui::MenuItem("Copy Path"))
        ImGui::SetClipboardText(assetPath.c_str());

    ImGui::EndPopup();
}

// Rename fix-up (contract at the declaration). The Rebuild BEFORE the rewrite is needed:
// RewriteReferences only touches containers the index knows, so a stale index silently
// misses new referrers. Both rebuilds are incremental (mtime + size): a directory walk,
// not a re-parse.
void EditorLayer::RepointAssetReferences(const std::string& oldRel,
                                         const std::string& newRel)
{
    m_refScanner.Rebuild(m_assetDir);
    const auto rep = m_refScanner.RewriteReferences(oldRel, newRel);
    m_refScanner.Rebuild(m_assetDir);   // RewriteReferences leaves it stale

    if (rep.referencesRewritten > 0)
        LOG_SUCCESS("Rename fix-up: repointed %zu reference(s) across %zu file(s)",
                    rep.referencesRewritten, rep.containersChanged);

    if (!rep.failed.empty())
    {
        // Loud AND actionable: the window lists exactly what still points at
        // the old path, so a partial fix-up cannot be mistaken for a clean one.
        LOG_ERROR("Rename fix-up: %zu container(s) still reference '%s' — "
                  "opening Asset References", rep.failed.size(), oldRel.c_str());
        m_refQueryPath         = oldRel;
        m_showReferencesWindow = true;
    }
}

// "Who uses this asset?" (see the declaration). Double-clicking an .iscene referrer loads
// it through RequestLoadScene (its unsaved-changes and autosave prompts apply); any other
// row copies its path.
void EditorLayer::RenderAssetReferencesWindow()
{
    if (!m_showReferencesWindow) return;
    if (!ImGui::Begin("Asset References", &m_showReferencesWindow))
    {
        ImGui::End();
        return;
    }

    // Query row: editable path + rescan.
    char buf[512];
    strncpy_s(buf, m_refQueryPath.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(-120.f);
    if (ImGui::InputText("##refQuery", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        m_refQueryPath = buf;
    ImGui::SameLine();
    if (ImGui::Button("Rescan"))
        m_refScanner.Rebuild(m_assetDir);

    if (m_refScanner.Built())
        ImGui::TextDisabled("%zu containers indexed, %zu reference edges (%.0f ms scan)",
                            m_refScanner.FileCount(), m_refScanner.EdgeCount(),
                            m_refScanner.LastScanMs());

    // ---- Identity layer status ----
    // The reverse index above rewrites only the containers it parses, and only on an in-editor
    // rename; the guid identities rescue what it cannot touch: a `git mv`, and save files
    // already on a player's disk.
    {
        Resource::AssetGuidIndex& gidx = Resource::AssetGuidIndex::Get();
        ImGui::TextDisabled("%zu asset identities%s (%.0f ms)", gidx.Count(),
                            gidx.FromBakedTable() ? " from baked pak table" : "",
                            gidx.LastScanMs());
        if (gidx.DuplicateCount() > 0)
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                               "%zu DUPLICATE guid(s) - two assets claim one identity. "
                               "Delete the copied .meta and let it re-stamp.",
                               gidx.DuplicateCount());
        if (ImGui::Button("Stamp all asset identities"))
        {
            // Bulk migration. Normally unnecessary (an asset is stamped when first referenced), but
            // running it once makes the whole tree rename-proof before a reorganisation.
            gidx.Rebuild(m_assetDir, /*stampMissing=*/true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes a .meta sidecar for every tracked asset that "
                              "lacks one.\nCommit them: the sidecar IS the identity.");

        // The other half of the migration: sidecars let an asset KEEP its identity, but a move
        // made outside the editor resolves only through references that carry it too.
        ImGui::SameLine();
        if (ImGui::Button("Add GUIDs to references"))
        {
            const auto rep = m_refScanner.BackfillGuids(m_assetDir);
            m_refScanner.Rebuild(m_assetDir);
            if (!rep.failed.empty())
            {
                LOG_ERROR("Guid backfill: %zu container(s) could not be rewritten "
                          "— see the warnings above", rep.failed.size());
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stamps every reference in every .iscene / .ipfb / .imat\n"
                              "with its target's guid. Run this ONCE before reorganising\n"
                              "asset/ outside the editor - without it a git mv or an\n"
                              "Explorer drag still strands its referrers.\n"
                              "Safe to re-run: it rewrites only what changes.");

        // ---- Material-instance repair (dry run, then opt-in apply) ----
        // The apply is a separate button behind a confirm (MaterialInstanceRepair.h): dropping
        // an override mark is a decision made on the artist's behalf.
        ImGui::SameLine();
        if (ImGui::Button("Scan material instances"))
        {
            if (auto* lib = m_renderer ? m_renderer->GetMaterialShaderLibrary() : nullptr)
            {
                m_matRepair = Editor::RepairMaterialInstances(*lib, m_assetDir, /*apply=*/false, m_renderer);
                m_matRepairRan = true;
            }
            else
                LOG_WARNING("Material instance scan: the shader library is not up yet");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Finds override marks a LEGACY .imatinst is carrying by accident.\n"
                "Before marks existed, a custom-shader instance recorded every\n"
                "reflected //@ parameter as an override, pinning values the artist\n"
                "never set. Dry run: reports, writes nothing.");

        if (m_matRepairRan)
        {
            ImGui::Indent();
            ImGui::Text("scanned %d - %d flagged, %d skipped",
                        m_matRepair.scanned, m_matRepair.flagged, m_matRepair.skipped);
            for (const auto& item : m_matRepair.items)
            {
                if (!item.skipReason.empty())
                {
                    ImGui::TextDisabled("skip  %s", item.path.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", item.skipReason.c_str());
                }
                else if (!item.droppable.empty())
                {
                    ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.25f, 1.f),
                                       "%s  (%zu accidental mark(s))%s",
                                       item.path.c_str(), item.droppable.size(),
                                       item.rewritten ? "  [rewritten]" : "");
                    if (ImGui::IsItemHovered())
                    {
                        std::string list;
                        for (const auto& k : item.droppable) { list += "  "; list += k; list += '\n'; }
                        ImGui::SetTooltip("parent: %s\n\nwould drop:\n%s",
                                          item.parentPath.c_str(), list.c_str());
                    }
                }
            }
            if (m_matRepair.flagged > 0)
            {
                if (ImGui::Button("Apply repair (writes .bak)"))
                    ImGui::OpenPopup("ConfirmMatRepair");
                if (ImGui::BeginPopup("ConfirmMatRepair"))
                {
                    ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                                       "Rewrites %d file(s).", m_matRepair.flagged);
                    ImGui::TextDisabled("Each keeps a .imatinst.bak beside it.\n"
                                        "Values do not change - only which of them\n"
                                        "stop being pinned against the master.");
                    if (ImGui::Button("Confirm"))
                    {
                        if (auto* lib = m_renderer ? m_renderer->GetMaterialShaderLibrary() : nullptr)
                        {
                            m_matRepair = Editor::RepairMaterialInstances(
                                *lib, m_assetDir, /*apply=*/true, m_renderer);
                            m_needsRescan = true;
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            ImGui::Unindent();
        }
    }

    ImGui::Separator();

    if (m_refQueryPath.empty())
    {
        ImGui::TextDisabled("Right-click an asset in the browser and pick "
                            "\"Find References\", or type a path above.");
        ImGui::End();
        return;
    }

    const auto refs = m_refScanner.FindReferrers(m_refQueryPath);
    if (refs.empty())
    {
        ImGui::TextWrapped("No references to '%s' in any .iscene / .ipfb / "
                           ".imat under asset/.", m_refQueryPath.c_str());
        ImGui::TextDisabled("(Lua scripts and code-side paths are outside this "
                            "index — absence here is strong, not absolute.)");
        ImGui::End();
        return;
    }

    ImGui::Text("%zu reference(s) to", refs.size());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", m_refQueryPath.c_str());

    if (ImGui::BeginTable("##refTable", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Referencing file");
        ImGui::TableSetupColumn("Via", ImGuiTableColumnFlags_WidthFixed, 220.f);
        ImGui::TableHeadersRow();
        int rowId = 0;
        for (const auto& r : refs)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(rowId++);
            if (ImGui::Selectable(r.file.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (r.file.size() > 7
                        && r.file.compare(r.file.size() - 7, 7, ".iscene") == 0)
                        RequestLoadScene(r.file);   // unsaved-changes modal applies
                    else
                        ImGui::SetClipboardText(r.file.c_str());
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Double-click: %s",
                                  (r.file.size() > 7
                                   && r.file.compare(r.file.size() - 7, 7, ".iscene") == 0)
                                      ? "load this scene" : "copy path");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r.key.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Unreal-style Content Browser action: right-click empty space to import into
// the folder the view is currently showing.
void EditorLayer::RenderAssetEmptySpaceContextMenu()
{
    if (ImGui::BeginPopupContextWindow(
            "AssetGridContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::BeginMenu("Import"))
        {
            RenderAssetImportMenuItems();
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
}

// Toolbar segmented toggle: Tiles | List | Details.
void EditorLayer::RenderAssetViewModeSelector()
{
    struct ModeDesc { AssetViewMode mode; const char* icon; const char* label; const char* tip; };
    static const ModeDesc kModes[] =
    {
        { AssetViewMode::Tiles,   FontAwesome::kTh,     "Tiles",
          "Tiles - large thumbnails" },
        { AssetViewMode::List,    FontAwesome::kThList, "List",
          "List - small icons + full file names" },
        { AssetViewMode::Details, FontAwesome::kTable,  "Details",
          "Details - name / type / size / modified (click headers to sort)" },
    };

    for (int i = 0; i < IM_ARRAYSIZE(kModes); ++i)
    {
        if (i > 0) ImGui::SameLine(0.f, 2.f);
        const bool active = (m_assetViewMode == kModes[i].mode);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        ImGui::PushID(i);
        if (ImGui::SmallButton(m_hasFontAwesomeIcons ? kModes[i].icon : kModes[i].label))
            m_assetViewMode = kModes[i].mode;
        ImGui::PopID();

        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kModes[i].tip);
    }
}

// ==== Folder tree - the content browser's left pane ====

void EditorLayer::SetCurrentAssetDir(const std::string& dir)
{
    if (dir.empty()) return;
    if (Sys::U8Path(dir) == Sys::U8Path(m_currentDir)) return;
    m_currentDir    = dir;
    m_selectedAsset = -1;
    m_needsRescan   = true;   // ScanAssets reads ONE directory; it must re-read
}

void EditorLayer::RebuildAssetFolderTree()
{
    m_folderTree.clear();
    if (m_assetDir.empty()) return;

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(Sys::U8Path(m_assetDir), ec)) return;

    // Iterative (m_folderTree is walked as it grows), not recursive: asset trees get deep,
    // and a stack overflow inside a UI rebuild is not worth two saved lines.
    m_folderTree.push_back({ "asset", m_assetDir, {}, 0 });

    // The engine content root as a second top-level node, so the engine assets a project
    // references stay browsable.
    {
        const std::string engineRoot = Sys::EngineDir() + "/engine";
        if (fs::is_directory(Sys::U8Path(engineRoot), ec))
            m_folderTree.push_back({ "engine", engineRoot, {}, 0 });
    }
    for (std::size_t i = 0; i < m_folderTree.size(); ++i)
    {
        const std::string dir = m_folderTree[i].path;   // by value: push invalidates
        int assets = 0;
        std::vector<std::pair<std::string, std::string>> subdirs;   // (name, path)
        std::unordered_set<std::string> listed;                     // lower-case names

        // This checkout's folder, then its shared twin for what it lacks - the
        // union ScanAssets lists.
        const fs::path twin = SharedTwin(dir);
        for (int pass = 0; pass < (twin.empty() ? 1 : 2); ++pass)
        {
            const bool shared = (pass == 1);
            for (fs::directory_iterator it(shared ? twin : Sys::U8Path(dir),
                                           fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec))
            {
                if (ec) break;
                const std::string name = Sys::PathToUtf8(it->path().filename());
                if (!listed.insert(Resource::AssetRefs::ToLowerAscii(name)).second) continue;
                const fs::path as = shared ? Sys::U8Path(dir) / it->path().filename() : it->path();
                if (it->is_directory(ec))
                {
                    subdirs.emplace_back(name, Sys::PathToUtf8(as));
                }
                else if (it->is_regular_file(ec))
                {
                    // Count exactly what ScanAssets lists (AssetTypeForExt, which also skips .meta
                    // sidecars): a count the folder does not deliver is worse than none.
                    const std::string ext = Resource::AssetRefs::ToLowerAscii(
                        Sys::PathToUtf8(it->path().extension()));
                    if (AssetTypeForExt(ext) != AssetType::Unknown &&
                        (!shared || Resource::AssetFS::Get().SharedServes(Sys::PathToUtf8(as))))
                        ++assets;
                }
            }
            ec.clear();
        }

        std::sort(subdirs.begin(), subdirs.end(),
                  [](const auto& a, const auto& b) { return CompareNameCI(a.first, b.first) < 0; });

        m_folderTree[i].assetCount = assets;
        m_folderTree[i].children.reserve(subdirs.size());
        for (const auto& [name, path] : subdirs)
        {
            m_folderTree[i].children.push_back(static_cast<int>(m_folderTree.size()));
            m_folderTree.push_back({ name, path, {}, 0 });
        }
    }
}

void EditorLayer::DrawAssetFolderNode(int index, bool reveal)
{
    if (index < 0 || index >= static_cast<int>(m_folderTree.size())) return;
    const AssetFolderNode& node = m_folderTree[index];

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    const bool leaf = node.children.empty();
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    const bool isCurrent = !m_currentDir.empty()
                        && Sys::U8Path(node.path) == Sys::U8Path(m_currentDir);
    if (isCurrent) flags |= ImGuiTreeNodeFlags_Selected;
    if (index == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    // Expand ancestors of the folder that was just selected elsewhere. Only on
    // the reveal frame, so a folder the user collapsed by hand stays collapsed.
    if (reveal && !leaf && !isCurrent && !m_currentDir.empty())
    {
        const std::string prefix = node.path + "/";
        std::string cur = m_currentDir;
        for (char& c : cur) if (c == '\\') c = '/';
        std::string pre = prefix;
        for (char& c : pre) if (c == '\\') c = '/';
        if (cur.rfind(pre, 0) == 0) ImGui::SetNextItemOpen(true);
    }

    // The selected icon changes, and tree indices can shift after a rescan.
    // Keep both this node and its descendants anchored to the folder path.
    ImGui::PushID(node.path.c_str());
    const std::string label = std::string(isCurrent ? FontAwesome::kFolderOpen
                                                    : FontAwesome::kFolder)
                            + "  " + node.name;
    const bool open = ImGui::TreeNodeEx("##Folder", flags, "%s", label.c_str());
    // IsItemToggledOpen: clicking the arrow expands, it does not navigate.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        SetCurrentAssetDir(node.path);

    AcceptAssetMoveDrop(node.path);
    RenderFolderContextMenu(node.path, index == 0);

    if (open && !leaf)
    {
        for (int child : node.children) DrawAssetFolderNode(child, reveal);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorLayer::RenderAssetFolderTree()
{
    ImGui::BeginChild("AssetFolderTree", ImVec2(0.f, 0.f), ImGuiChildFlags_None);

    if (m_folderTree.empty())
    {
        ImGui::TextDisabled("(no folders)");
        ImGui::EndChild();
        return;
    }

    const bool reveal = (m_folderRevealedFor != m_currentDir);
    DrawAssetFolderNode(0, reveal);
    m_folderRevealedFor = m_currentDir;

    ImGui::EndChild();
}

void EditorLayer::RenderAssetBrowser()
{
    const float now = static_cast<float>(ImGui::GetTime());

    // Reset per-frame load budget (views stamp visibility as they submit items).
    m_previewLoadsThisFrame = 0;

    // ---- Eviction pass: release items off-screen longer than m_previewEvictDelay; skip never-visible. ----
    if (m_textureSys && m_gfx)
    {
        for (auto& ae : m_assets)
        {
            if (ae.thumbnail == Resource::kInvalidTextureHandle) continue;
            if (ae.lastVisibleTime < 0.f) continue;

            const float hiddenFor = now - ae.lastVisibleTime;
            if (hiddenFor > m_previewEvictDelay)
            {
                m_textureSys->Release(ae.thumbnail, *m_gfx);
                ae.thumbnail       = Resource::kInvalidTextureHandle;
                ae.lastVisibleTime = -1.f;
            }
        }
    }

    if (m_assetOrderDirty) RebuildAssetDisplayOrder();
    RebuildAssetDisplayRows();   // + the Mesh sub-rows of expanded models (cheap: cached views)
    m_modelTileRects.clear();    // this draw's model tiles (an OS file drop's target)

    switch (m_assetViewMode)
    {
    case AssetViewMode::List:    RenderAssetList();    break;
    case AssetViewMode::Details: RenderAssetDetails(); break;
    default:                     RenderAssetGrid();    break;
    }
}

// Placeholder for a view with nothing to draw: tells "folder is empty" apart from "the
// filter hid everything".
void EditorLayer::RenderAssetEmptyHint()
{
    if (!m_assets.empty() && m_assetFilter[0] != '\0')
    {
        ImGui::TextDisabled("No asset matches '%s'", m_assetFilter);
    }
    else
    {
        ImGui::TextDisabled("No converted assets found in '%s'", m_assetDir.c_str());
        ImGui::TextDisabled("(place .itex / .ishdr files here and hit Refresh)");
    }
    RenderAssetEmptySpaceContextMenu();
}

// Windows-Explorer "List" view: small icon + full file name, flowed into as many
// equal-width columns as the panel fits. A table, so ImGui's row clipper keeps folders
// with thousands of entries cheap.
void EditorLayer::RenderAssetList()
{
    constexpr float kIconSize = 18.f;

    if (m_assets.empty() || m_assetRows.empty())
    {
        RenderAssetEmptyHint();
        return;
    }

    const float availW  = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
    const int   columns = ImClamp(static_cast<int>(availW / m_assetListColW), 1, 64);
    const int   total   = static_cast<int>(m_assetRows.size());
    const int   rows    = (total + columns - 1) / columns;

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoPadOuterX;
    if (!ImGui::BeginTable("AssetListView", columns, kFlags))
        return;

    for (int c = 0; c < columns; ++c)
    {
        char colId[16];
        std::snprintf(colId, sizeof(colId), "##lc%d", c);
        ImGui::TableSetupColumn(colId, ImGuiTableColumnFlags_WidthStretch);
    }

    ImGuiListClipper clipper;
    clipper.Begin(rows);
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            ImGui::TableNextRow();
            for (int c = 0; c < columns; ++c)
            {
                const int slot = r * columns + c;
                if (slot >= total) break;
                ImGui::TableSetColumnIndex(c);
                const AssetDisplayRow& row = m_assetRows[slot];
                if (row.mesh >= 0) DrawModelMeshRowItem(row, kIconSize, false);
                else               DrawAssetRowItem(row.asset, kIconSize, false);
            }
        }
    }

    RenderAssetEmptySpaceContextMenu();   // inside the table = its scrolling child
    ImGui::EndTable();
}

// Windows-Explorer "Details" view: a sortable Name / Type / Size / Modified / Dimensions /
// Format table; the name column carries the small icon and the full file name.
void EditorLayer::RenderAssetDetails()
{
    constexpr float kIconSize = 18.f;

    if (m_assets.empty() || m_assetRows.empty())
    {
        RenderAssetEmptyHint();
        return;
    }

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_ScrollY      | ImGuiTableFlags_RowBg        |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable    | ImGuiTableFlags_Sortable      |
        ImGuiTableFlags_Hideable;

    if (!ImGui::BeginTable("AssetDetailsView", 6, kFlags))
        return;

    // Type carries DefaultSort so the first open matches the scan grouping the
    // Tiles view shows (folders, then type, then name).
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch |
                                    ImGuiTableColumnFlags_NoHide, 0.f, 0);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_DefaultSort, 90.f, 1);
    ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed,  80.f, 2);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.f, 3);
    // A texture's size and format, read from its DDS header on first draw.
    ImGui::TableSetupColumn("Dimensions", ImGuiTableColumnFlags_WidthFixed, 110.f, 4);
    ImGui::TableSetupColumn("Format",     ImGuiTableColumnFlags_WidthFixed, 110.f, 5);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Header clicks only move the display order; m_assets keeps the scan order.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
    {
        if (specs->SpecsDirty)
        {
            if (specs->SpecsCount > 0)
            {
                m_assetSortColumn = static_cast<int>(specs->Specs[0].ColumnUserID);
                m_assetSortAsc    = (specs->Specs[0].SortDirection != ImGuiSortDirection_Descending);
            }
            m_assetOrderDirty = true;
            specs->SpecsDirty = false;
        }
    }
    if (m_assetOrderDirty)
    {
        RebuildAssetDisplayOrder();
        RebuildAssetDisplayRows();
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_assetRows.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const AssetDisplayRow& dr = m_assetRows[row];
            const int idx = dr.asset;
            if (idx < 0 || idx >= static_cast<int>(m_assets.size())) continue;

            ImGui::TableNextRow();

            if (dr.mesh >= 0)
            {
                // An expanded model's Mesh item.
                ImGui::TableSetColumnIndex(0);
                DrawModelMeshRowItem(dr, kIconSize, true);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("Mesh");
                ImGui::TableSetColumnIndex(2);
                if (const MeshAssetRegistry::Mesh* m = ModelMeshAt(dr))
                    ImGui::Text("%zu slot%s", m->sections.size(), m->sections.size() == 1 ? "" : "s");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(4);
                ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(5);
                ImGui::TextDisabled("-");
                continue;
            }

            ImGui::TableSetColumnIndex(0);
            DrawAssetRowItem(idx, kIconSize, true);

            const AssetEntry& ae = m_assets[idx];

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(AssetTypeName(ae.type));

            ImGui::TableSetColumnIndex(2);
            if (ae.type == AssetType::Folder)
            {
                ImGui::TextDisabled("-");
            }
            else
            {
                char sizeText[32];
                FormatAssetSize(ae.sizeBytes, sizeText, sizeof(sizeText));
                ImGui::TextUnformatted(sizeText);
            }

            ImGui::TableSetColumnIndex(3);
            char when[32];
            FormatAssetTime(ae.mtime, when, sizeof(when));
            if (when[0] != '\0') ImGui::TextUnformatted(when);
            else                 ImGui::TextDisabled("-");

            const Resource::TextureFileInfo* ti = AssetTextureInfo(idx);
            ImGui::TableSetColumnIndex(4);
            if (ti)
            {
                if (ti->isCube)                 ImGui::Text("Cube %ux%u", ti->width, ti->height);
                else if (ti->dimension == "3D") ImGui::Text("%ux%ux%u", ti->width, ti->height, ti->depth);
                else if (ti->arraySize > 1)     ImGui::Text("%ux%u x%u", ti->width, ti->height, ti->arraySize);
                else                            ImGui::Text("%ux%u", ti->width, ti->height);
            }
            else
                ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(5);
            if (ti)
            {
                ImGui::TextUnformatted(ti->formatLabel.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%u mip%s%s%s", ti->formatName.c_str(), ti->mipLevels,
                                      ti->mipLevels == 1 ? "" : "s",
                                      ti->preset.empty() ? "" : "\nimport preset: ", ti->preset.c_str());
            }
            else
                ImGui::TextDisabled("-");
        }
    }

    RenderAssetEmptySpaceContextMenu();
    ImGui::EndTable();
}

// Tiles view: the large-thumbnail grid.
void EditorLayer::RenderAssetGrid()
{
    constexpr float kTileSize    = 80.f;   // thumbnail/icon square size
    constexpr float kTilePadX   = 10.f;   // horizontal gap between tiles
    constexpr float kTilePadY   = 22.f;   // space below tile for label
    constexpr float kTileStepX  = kTileSize + kTilePadX;
    constexpr float kTileStepY  = kTileSize + kTilePadY + 6.f;

    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const int   columns    = (std::max)(1, static_cast<int>(panelWidth / kTileStepX));

    // Per-frame constants (captured once before any child window changes context).
    const float currentTime = static_cast<float>(ImGui::GetTime());
    constexpr float kVisBuffer = kTileStepY;  // 1-row lookahead beyond viewport edges

    ImGui::BeginChild("AssetGridScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

    if (m_assetRows.empty())
    {
        RenderAssetEmptyHint();
        ImGui::EndChild();
        return;
    }

    int col = 0;
    for (int slot = 0; slot < static_cast<int>(m_assetRows.size()); ++slot)
    {
        const AssetDisplayRow& row = m_assetRows[slot];
        const int i = row.asset;
        if (i < 0 || i >= static_cast<int>(m_assets.size())) continue;

        // An expanded model's Mesh item: its own tile, same flow layout.
        if (row.mesh >= 0)
        {
            DrawModelMeshTile(row, kTileSize, kTilePadY);
            ++col;
            if (col < columns) ImGui::SameLine(0.f, kTilePadX);
            else { col = 0; ImGui::Dummy(ImVec2(0.f, kTilePadY)); }
            continue;
        }
        AssetEntry& ae = m_assets[i];

        // Screen position of this tile, read before any ImGui state mutation.
        ImVec2 tileMin = ImGui::GetCursorScreenPos();

        // ---- Visibility culling + load throttling ----
        {
            const float winTop   = ImGui::GetWindowPos().y;
            const float winBot   = winTop + ImGui::GetWindowHeight();
            const float tileBotY = tileMin.y + kTileSize + kTilePadY;
            const bool  visible  = (tileBotY + kVisBuffer > winTop) &&
                                   (tileMin.y  - kVisBuffer < winBot);
            // Off-screen: lastVisibleTime is left alone, so the hidden gap keeps growing.
            StreamAssetThumbnail(i, visible, currentTime);
        }

        const bool selected = (m_selectedAsset == i) || IsAssetSelected(i);

        // Invisible button for interaction (full tile area including label space)
        ImGui::PushID(i);
        const bool tileClicked = ImGui::InvisibleButton("##tile", ImVec2(kTileSize, kTileSize + kTilePadY));
        // Painting below wants hover for the background tint; the shared
        // wiring derives its own hover off the same (still-last) item.
        const bool hovered     = ImGui::IsItemHovered();
        WireAssetItemInteractions(i, tileClicked);
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImVec2 tileMax = ImVec2(tileMin.x + kTileSize, tileMin.y + kTileSize);

        ImU32 bgColor = selected ? IM_COL32(60, 120, 200, 200)
                      : hovered  ? IM_COL32(80,  80, 100, 180)
                                 : IM_COL32(40,  40,  50, 200);
        dl->AddRectFilled(tileMin, ImVec2(tileMin.x + kTileSize, tileMin.y + kTileSize + kTilePadY + 2.f), bgColor, 4.f);

        // Thumbnail, or the type badge.
        const uint64_t gpuHandle = GetAssetThumbnailSrv(i);
        if (gpuHandle != 0)
        {
            dl->AddImage(
                ImTextureRef(static_cast<ImTextureID>(gpuHandle)),
                ImVec2(tileMin.x + 2.f, tileMin.y + 2.f),
                ImVec2(tileMax.x - 2.f, tileMax.y - 2.f));
        }
        else
        {
            dl->AddRectFilled(ImVec2(tileMin.x + 2.f, tileMin.y + 2.f),
                              ImVec2(tileMax.x - 2.f, tileMax.y - 2.f),
                              AssetTypeColor(ae.type), 2.f);

            // Icon glyph, or the one-letter type tag when the icon font is absent.
            const char* icon = AssetIconChar(ae.type);
            if (!icon) icon  = AssetTypeTag(ae.type);
            ImVec2 iconSize  = ImGui::CalcTextSize(icon);
            ImVec2 iconPos   = ImVec2(
                tileMin.x + (kTileSize - iconSize.x) * 0.5f,
                tileMin.y + (kTileSize - iconSize.y) * 0.5f);
            dl->AddText(iconPos, IM_COL32(255, 255, 255, 230), icon);
        }

        {
            // Label: the stem, ellipsized to the tile width.
            const std::string& stem = ae.stem;
            const float maxLabelW   = kTileSize - 2.f;
            ImVec2 labelPos = ImVec2(tileMin.x + 1.f, tileMax.y + 2.f);

            // Binary search for the longest prefix that fits with the ellipsis.
            const char* textBegin = stem.c_str();
            const char* textEnd   = textBegin + stem.size();
            ImVec2 fullSz = ImGui::CalcTextSize(textBegin, textEnd);
            if (fullSz.x > maxLabelW)
            {
                const char* ellipsis  = "\xe2\x80\xa6";  // UTF-8 horizontal ellipsis (U+2026)
                float       ellipsisW = ImGui::CalcTextSize(ellipsis).x;
                int lo = 0, hi = static_cast<int>(stem.size());
                while (lo < hi)
                {
                    int mid = (lo + hi + 1) / 2;
                    ImVec2 sz = ImGui::CalcTextSize(textBegin, textBegin + mid);
                    if (sz.x + ellipsisW <= maxLabelW) lo = mid;
                    else                               hi = mid - 1;
                }
                std::string clipped = stem.substr(0, lo) + "\xe2\x80\xa6";
                dl->AddText(labelPos, IM_COL32(200, 200, 200, 255), clipped.c_str());
            }
            else
            {
                dl->AddText(labelPos, IM_COL32(200, 200, 200, 255), textBegin, textEnd);
            }
        }

        if (selected)
            dl->AddRect(tileMin, ImVec2(tileMin.x + kTileSize, tileMin.y + kTileSize + kTilePadY + 2.f),
                        IM_COL32(100, 180, 255, 255), 4.f, 0, 2.f);

        // A model expands into its Meshes: the arrow in the corner.
        if (IsModelExpandable(i))
            DrawModelExpandArrow(i, ImVec2(tileMin.x + kTileSize - 20.f, tileMin.y + 4.f), 16.f);

        ++col;
        if (col < columns)
        {
            ImGui::SameLine(0.f, kTilePadX);
        }
        else
        {
            col = 0;
            ImGui::Dummy(ImVec2(0.f, kTilePadY));  // row gap
        }
    }
    // Ensure last row has proper height
    if (col != 0)
        ImGui::Dummy(ImVec2(0.f, kTilePadY));

    // Right-click empty space: import into the folder this grid shows.
    RenderAssetEmptySpaceContextMenu();

    ImGui::EndChild();
}

void EditorLayer::RenderAssetImportMenuItems()
{
    if (ImGui::MenuItem("Texture..."))   RunImport(AssetType::Texture);
    if (ImGui::MenuItem("Model..."))     RunImport(AssetType::Mesh);
    if (ImGui::MenuItem("Shader..."))    RunImport(AssetType::Shader);
    if (ImGui::MenuItem("Material..."))  RunImport(AssetType::Material);
    if (ImGui::MenuItem("Animation...")) RunImport(AssetType::Animation);
    if (ImGui::MenuItem("Audio..."))     RunImport(AssetType::Audio);
    ImGui::Separator();
    if (ImGui::MenuItem("Unreal Scene (manifest)..."))
        BeginUnrealManifestImport();
}

// ==== Asset multi-selection ====
// Tracked by path, not by index: indices do not survive a rescan (see
// m_selectedAssetPaths).

bool EditorLayer::IsAssetSelected(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return false;
    const std::string& p = m_assets[index].path;
    return std::find(m_selectedAssetPaths.begin(), m_selectedAssetPaths.end(), p)
           != m_selectedAssetPaths.end();
}

void EditorLayer::ClearAssetSelection()
{
    m_selectedAssetPaths.clear();
    m_selectedAsset    = -1;
    m_assetRangeAnchor = -1;
}

std::vector<std::string> EditorLayer::SelectedAssetPaths() const
{
    std::vector<std::string> out;
    out.reserve(m_selectedAssetPaths.size());
    // Still there - here, or in the shared twin the browser listed it from. A
    // file operation on a twin-only entry fails on this checkout's path; it
    // never reaches the other tree.
    for (const std::string& p : m_selectedAssetPaths)
        if (Resource::AssetFS::Get().ExistsExact(p)) out.push_back(p);
    return out;
}

void EditorLayer::SelectAsset(int index, SelectMode mode)
{
    if (index < 0 || index >= static_cast<int>(m_assets.size())) return;
    const std::string path = m_assets[index].path;   // by value: rescans invalidate

    switch (mode)
    {
    case SelectMode::Replace:
        m_selectedAssetPaths.assign(1, path);
        m_assetRangeAnchor = index;
        break;

    case SelectMode::Add:
        std::erase(m_selectedAssetPaths, path);
        m_selectedAssetPaths.push_back(path);
        m_assetRangeAnchor = index;
        break;

    case SelectMode::Toggle:
        if (IsAssetSelected(index)) std::erase(m_selectedAssetPaths, path);
        else                        m_selectedAssetPaths.push_back(path);
        m_assetRangeAnchor = index;
        break;

    case SelectMode::Range:
    {
        // Range runs over the DISPLAY order, not m_assets order: the user selects what they see,
        // and the two differ whenever a filter or a Details-view sort is active.
        const auto pos = [&](int idx) {
            const auto it = std::find(m_assetOrder.begin(), m_assetOrder.end(), idx);
            return it == m_assetOrder.end() ? -1
                                            : static_cast<int>(it - m_assetOrder.begin());
        };
        const int a = pos(m_assetRangeAnchor);
        const int b = pos(index);
        if (a < 0 || b < 0)
        {
            m_selectedAssetPaths.assign(1, path);   // no usable anchor
            m_assetRangeAnchor = index;
            break;
        }
        m_selectedAssetPaths.clear();
        for (int i = std::min(a, b); i <= std::max(a, b); ++i)
            m_selectedAssetPaths.push_back(m_assets[m_assetOrder[i]].path);
        // Clicked item is the primary, whichever end it sits at.
        std::erase(m_selectedAssetPaths, path);
        m_selectedAssetPaths.push_back(path);
        // Anchor deliberately unchanged so repeated Shift+clicks re-extend.
        break;
    }
    }

    m_selectedAsset = m_selectedAssetPaths.empty() ? -1 : index;
}

// Same normalization the reference index uses, so "is this path inside that
// folder" answers identically here and there.
static std::string NormalizeForCompare(const std::string& p)
{
    return Editor::AssetReferenceScanner::NormalizePath(p);
}

// ==== Folder operations ====
// An asset that moves takes its identity and its referrers along: the .meta sidecar moves
// with the file, AssetGuidIndex is repointed, and every container naming the old path is
// rewritten. Without the rewrite scenes log a repair on every open; without the sidecar
// the asset is re-stamped later and every existing reference is orphaned.

// Engine content, plus -refscan_root=<dir>: a harness fixture root, so a test moves and
// renames COPIES under tests/_artifacts/ through the browser's own code.
std::vector<std::string> EditorLayer::RefScanExtraRoots() const
{
    std::vector<std::string> roots{ Sys::EngineDir() + "/engine" };
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    if (cl.Has("-refscan_root") && !cl.Get("-refscan_root").empty())
        roots.push_back(cl.Get("-refscan_root"));
    return roots;
}

void EditorLayer::RepointMovedAssets(
    const std::vector<std::pair<std::string, std::string>>& moves)
{
    if (moves.empty()) return;

    for (const auto& [oldPath, newPath] : moves)
        Resource::AssetGuidIndex::Get().Repoint(oldPath, newPath);

    // The index must cover the DESTINATION too: a container moved in this batch is no longer
    // where the index recorded it, so an unindexed destination would leave its own
    // references naming the old location.
    m_refScanner.SetExtraRoots(RefScanExtraRoots());

    // One batch, not a loop: RewriteReferences marks the index stale on the way
    // out, so a per-asset loop would silently no-op after the first one.
    const auto rep = m_refScanner.RewriteReferencesBatch(moves);
    m_refScanner.Rebuild(m_assetDir);

    if (rep.referencesRewritten > 0)
        LOG_SUCCESS("Move: repointed %zu reference(s) across %zu file(s)",
                    rep.referencesRewritten, rep.containersChanged);
    if (!rep.failed.empty())
    {
        LOG_ERROR("Move: %zu container(s) still reference a moved asset - "
                  "opening Asset References", rep.failed.size());
        m_refQueryPath         = moves.front().first;
        m_showReferencesWindow = true;
    }
}

void EditorLayer::CreateAssetFolder(const std::string& parentDir, const std::string& name)
{
    if (parentDir.empty() || name.empty()) return;
    namespace fs = std::filesystem;

    // Reject anything that would escape the folder being viewed. ".." typed
    // into a name field is a mistake, not an instruction.
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos
        || name == "." || name == "..")
    {
        LOG_WARNING("New folder: '%s' is not a valid folder name", name.c_str());
        return;
    }

    std::error_code ec;
    const fs::path dst = Sys::U8Path(parentDir) / Sys::U8Path(name);
    if (fs::exists(dst, ec))
    {
        LOG_WARNING("New folder: '%s' already exists", Sys::PathToUtf8(dst).c_str());
        return;
    }
    if (!fs::create_directory(dst, ec) || ec)
    {
        LOG_ERROR("New folder: could not create '%s' (%s)",
                  Sys::PathToUtf8(dst).c_str(), ec.message().c_str());
        return;
    }
    LOG_SUCCESS("Created folder '%s'", Sys::PathToUtf8(dst).c_str());
    m_needsRescan = true;
}

// The new instance starts with an EMPTY override set on purpose: every value is inherited
// and stays live until the artist overrides it. That is the difference from duplicating
// the material.
std::string EditorLayer::CreateMaterialInstance(const std::string& parentPath)
{
    if (parentPath.empty()) return {};
    namespace fs = std::filesystem;

    // Resolve the parent so the instance is written against its real effective
    // values. A parent that cannot resolve would silently produce an instance of
    // a default white material.
    MaterialComponent parentMat{};
    if (!Resource::LoadMaterial(parentPath, parentMat))
    {
        LOG_ERROR("Create Material Instance: parent '%s' failed to load",
                  parentPath.c_str());
        return {};
    }

    // Name: MI_<stem>.imatinst, then _1, _2, ...: never overwrite, always suffix. A trailing
    // ".imat" is stripped from the stem first, or the name gets a second extension.
    const fs::path parentFsPath = Sys::U8Path(parentPath);
    std::string stem = Sys::PathToUtf8(parentFsPath.stem());
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".imat") == 0)
        stem.erase(stem.size() - 5);
    if (stem.rfind("MI_", 0) != 0) stem = "MI_" + stem;

    const fs::path dir = parentFsPath.parent_path();
    std::error_code ec;
    fs::path dst;
    for (int suffix = 0; suffix < 1000; ++suffix)
    {
        const std::string name = suffix == 0 ? (stem + ".imatinst")
                                             : (stem + "_" + std::to_string(suffix) + ".imatinst");
        dst = dir / Sys::U8Path(name);
        if (!fs::exists(dst, ec)) break;
        dst.clear();
    }
    if (dst.empty())
    {
        LOG_ERROR("Create Material Instance: could not find a free name for '%s'",
                  stem.c_str());
        return {};
    }

    const std::string outPath = Sys::RelativizeAssetPath(Sys::PathToUtf8(dst));
    const std::string parentRef = Sys::RelativizeAssetPath(parentPath);

    // An empty set, explicitly: the EXPLICIT format with nothing marked (nullptr would select
    // the value-diff mode).
    const std::unordered_set<std::string> noOverrides;
    if (!Resource::SaveMaterialInstance(outPath, parentRef, parentMat, &noOverrides))
    {
        LOG_ERROR("Create Material Instance: could not write '%s'", outPath.c_str());
        return {};
    }

    // Stamp explicitly: ScanAssets builds the index with stampMissing=false, and a guid-less
    // instance does not survive its parent being renamed.
    Resource::AssetGuidIndex::Get().EnsureGuid(outPath);

    LOG_SUCCESS("Created material instance '%s' (parent '%s')",
                outPath.c_str(), parentRef.c_str());
    m_needsRescan = true;
    return outPath;
}

std::string EditorLayer::CreateAnimationAsset(bool graph, const std::string& directory)
{
    namespace fs = std::filesystem;
    const std::string dir = directory.empty() ? (m_currentDir.empty() ? m_assetDir : m_currentDir) : directory;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(Sys::U8Path(dir), ec))
    {
        LOG_ERROR("Create Animation Asset: choose an existing content folder");
        return {};
    }
    const std::string stem = graph ? "AnimationGraph" : "AnimationSet";
    const std::string extension = graph ? ".ianimgraph" : ".ianimset";
    std::string path;
    for (unsigned suffix = 1; suffix < 10000; ++suffix)
    {
        const auto candidate = Sys::U8Path(dir) / Sys::U8Path(stem + std::to_string(suffix) + extension);
        if (!fs::exists(candidate, ec) && !ec)
        {
            path = Sys::RelativizeAssetPath(Sys::PathToUtf8(candidate));
            break;
        }
        if (ec) break;
    }
    if (path.empty()) { LOG_ERROR("Create Animation Asset: could not find a free name in %s", dir.c_str()); return {}; }
    auto control = graph ? Editor::AnimGraphEditor::Control : Editor::AnimSetEditor::Control;
    auto result = nlohmann::json::parse(control(nlohmann::json{{"action", "new"}, {"path", path}}.dump()));
    if (result.value("ok", false)) result = nlohmann::json::parse(control(R"({"action":"save"})"));
    if (!result.value("ok", false))
    {
        LOG_WARNING("Create Animation Asset: %s", result.value("error", "Could not create asset").c_str());
        return {};
    }
    Resource::AssetGuidIndex::Get().EnsureGuid(path);
    m_needsRescan = true;
    LOG_SUCCESS("Created animation asset '%s'", path.c_str());
    return path;
}

size_t EditorLayer::MoveAssetsInto(const std::vector<std::string>& srcPaths,
                                   const std::string& destDir)
{
    if (srcPaths.empty() || destDir.empty()) return 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(Sys::U8Path(destDir), ec))
    {
        LOG_WARNING("Move: '%s' is not a folder", destDir.c_str());
        return 0;
    }

    std::vector<std::pair<std::string, std::string>> moves;
    moves.reserve(srcPaths.size());

    for (const std::string& src : srcPaths)
    {
        if (src.empty()) continue;
        const fs::path srcP = Sys::U8Path(src);
        if (fs::is_directory(srcP, ec)) continue;   // folders go via MoveAssetFolder

        const fs::path    dstP = Sys::U8Path(destDir) / srcP.filename();
        const std::string dst  = Sys::PathToUtf8(dstP);
        if (Sys::U8Path(dst) == srcP) continue;     // already there
        if (fs::exists(dstP, ec))
        {
            // No silent overwrite: that file is somebody's asset, and whatever
            // references it would start resolving to this one instead.
            LOG_WARNING("Move: '%s' already exists - skipped", dst.c_str());
            continue;
        }

        fs::rename(srcP, dstP, ec);
        if (ec)
        {
            LOG_ERROR("Move failed: '%s' -> '%s' (%s)",
                      src.c_str(), dst.c_str(), ec.message().c_str());
            continue;
        }
        // The sidecar rides along.
        const std::string srcMeta = Resource::MetaPathFor(src);
        if (fs::exists(Sys::U8Path(srcMeta), ec))
            fs::rename(Sys::U8Path(srcMeta),
                       Sys::U8Path(Resource::MetaPathFor(dst)), ec);

        moves.emplace_back(src, dst);
    }

    if (!moves.empty())
    {
        RepointMovedAssets(moves);
        LOG_SUCCESS("Moved %zu asset(s) into '%s'", moves.size(), destDir.c_str());
        ClearAssetSelection();
        m_needsRescan = true;
    }
    return moves.size();
}

bool EditorLayer::MoveAssetFolder(const std::string& srcDir, const std::string& destPath)
{
    namespace fs = std::filesystem;
    if (srcDir.empty() || destPath.empty()) return false;
    std::error_code ec;

    const fs::path srcP = Sys::U8Path(srcDir);
    const fs::path dstP = Sys::U8Path(destPath);
    if (!fs::is_directory(srcP, ec)) return false;
    if (fs::exists(dstP, ec))
    {
        LOG_WARNING("Move folder: '%s' already exists", destPath.c_str());
        return false;
    }
    // Moving a folder into itself (or its own descendant) either fails halfway
    // or loses the subtree, depending on the platform.
    if ((NormalizeForCompare(destPath) + "/").rfind(NormalizeForCompare(srcDir) + "/", 0) == 0)
    {
        LOG_WARNING("Move folder: cannot move '%s' inside itself", srcDir.c_str());
        return false;
    }

    // Enumerate BEFORE the move: afterwards the old paths are gone and the
    // (old,new) pairs the reference fix-up needs cannot be reconstructed.
    std::vector<std::pair<std::string, std::string>> moves;
    for (fs::recursive_directory_iterator
             it(srcP, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string oldPath = Sys::PathToUtf8(it->path());
        const std::string lower   = Resource::AssetRefs::ToLowerAscii(oldPath);
        if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".meta") == 0)
            continue;                                // sidecars follow their asset
        const fs::path rel = fs::relative(it->path(), srcP, ec);
        if (ec) continue;
        moves.emplace_back(oldPath, Sys::PathToUtf8(dstP / rel));
    }

    fs::rename(srcP, dstP, ec);
    if (ec)
    {
        LOG_ERROR("Move folder failed: '%s' -> '%s' (%s)",
                  srcDir.c_str(), destPath.c_str(), ec.message().c_str());
        return false;
    }

    RepointMovedAssets(moves);
    LOG_SUCCESS("Moved folder '%s' -> '%s' (%zu asset(s) repointed)",
                srcDir.c_str(), destPath.c_str(), moves.size());

    // Follow the folder if the browser was inside it.
    if (NormalizeForCompare(m_currentDir).rfind(NormalizeForCompare(srcDir), 0) == 0)
        SetCurrentAssetDir(destPath);

    m_needsRescan = true;
    return true;
}

bool EditorLayer::DeleteAssetFolder(const std::string& dir)
{
    namespace fs = std::filesystem;
    if (dir.empty()) return false;
    std::error_code ec;
    if (!fs::is_directory(Sys::U8Path(dir), ec)) return false;
    if (Sys::U8Path(dir) == Sys::U8Path(m_assetDir))
    {
        LOG_WARNING("Delete folder: refusing to delete the asset root");
        return false;
    }

    // Enumerate BEFORE the move: afterwards the paths are gone, and every identity inside must be
    // forgotten so no guid passes to some future file that lands on the same path.
    std::vector<std::string> files;
    std::uintmax_t entries = 1;   // the folder itself, as remove_all counted
    for (fs::recursive_directory_iterator
             it(Sys::U8Path(dir), fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) break;
        ++entries;
        if (it->is_regular_file(ec)) files.push_back(Sys::PathToUtf8(it->path()));
    }

    std::string why;
    if (!MoveToRecycleBin({ dir }, why))
    {
        LOG_ERROR("Delete folder failed: '%s' (%s)", dir.c_str(), why.c_str());
        return false;
    }
    for (const std::string& f : files) Resource::AssetGuidIndex::Get().Forget(f);
    LOG_INFO("Deleted folder '%s' (%llu entries)", dir.c_str(),
             static_cast<unsigned long long>(entries));

    if (NormalizeForCompare(m_currentDir).rfind(NormalizeForCompare(dir), 0) == 0)
        SetCurrentAssetDir(m_assetDir);

    ClearAssetSelection();
    m_needsRescan = true;
    return true;
}

void EditorLayer::AcceptAssetMoveDrop(const std::string& destDir)
{
    if (!ImGui::BeginDragDropTarget()) return;

    // A folder also accepts the TYPED single-asset payloads: a drag source publishes one
    // payload, and a single asset must keep its typed id for the viewport and the Inspector, so
    // the folder is the side that has to be permissive.
    static const char* const kTypedAssetPayloads[] = {
        "ISCN_PATH", "IPFB_PATH", "ITEX_PATH", "IMAT_PATH", "ISKEL_PATH",
        "IANIMGRAPH_PATH", "IANIMSET_PATH", "IANIM_PATH", "ACLIP_PATH", "ILUA_PATH", "IFONT_PATH", "IVFX_PATH",
        "ISEQ_PATH", "IHLSL_PATH", "IUI_PATH", "IFACEMAP_PATH", "IFACE_PATH",
    };
    for (const char* id : kTypedAssetPayloads)
    {
        if (const ImGuiPayload* tp = ImGui::AcceptDragDropPayload(id))
        {
            // Typed payloads carry one NUL-terminated path.
            m_pendingMoveSrc.assign(1, std::string(static_cast<const char*>(tp->Data)));
            m_pendingMoveDest = destDir;
            ImGui::EndDragDropTarget();
            return;
        }
    }

    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MOVE"))
    {
        // Newline-separated, so one drag can carry a whole multi-selection.
        const std::string blob(static_cast<const char*>(p->Data),
                               static_cast<size_t>(p->DataSize));
        std::vector<std::string> paths;
        size_t pos = 0;
        while (pos < blob.size())
        {
            const size_t nl  = blob.find('\n', pos);
            const size_t end = (nl == std::string::npos) ? blob.size() : nl;
            std::string one  = blob.substr(pos, end - pos);
            while (!one.empty() && (one.back() == '\0' || one.back() == '\r')) one.pop_back();
            if (!one.empty()) paths.push_back(std::move(one));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        // Deferred: the move invalidates m_assets and the folder tree, and this
        // runs in the middle of drawing both.
        m_pendingMoveSrc  = std::move(paths);
        m_pendingMoveDest = destDir;
    }
    ImGui::EndDragDropTarget();
}

// Runs the queued folder operations once per frame, after the browser has drawn (see the
// declaration).
void EditorLayer::ApplyPendingFolderOps()
{
    if (!m_pendingMoveSrc.empty() && !m_pendingMoveDest.empty())
    {
        const std::vector<std::string> src  = std::move(m_pendingMoveSrc);
        const std::string              dest = std::move(m_pendingMoveDest);
        m_pendingMoveSrc.clear();
        m_pendingMoveDest.clear();

        // A folder dragged onto a folder is a folder move; files are a batch.
        std::error_code ec;
        std::vector<std::string> files;
        for (const std::string& s : src)
        {
            if (std::filesystem::is_directory(Sys::U8Path(s), ec))
                MoveAssetFolder(s, Sys::PathToUtf8(Sys::U8Path(dest)
                                                   / Sys::U8Path(s).filename()));
            else
                files.push_back(s);
        }
        if (!files.empty()) MoveAssetsInto(files, dest);
    }

    if (!m_pendingDeleteFolder.empty())
    {
        const std::string dir = std::move(m_pendingDeleteFolder);
        m_pendingDeleteFolder.clear();
        DeleteAssetFolder(dir);
    }
}

// Right-click menu of a folder (tree node or tile). `isRoot` (the asset/ root) disables
// Rename and Delete: the browser may not move or delete it.
void EditorLayer::RenderFolderContextMenu(const std::string& dir, bool isRoot)
{
    if (dir.empty()) return;
    ImGui::PushID(dir.c_str());
    if (!ImGui::BeginPopupContextItem("FolderCtx"))
    {
        ImGui::PopID();
        return;
    }

    namespace fs = std::filesystem;
    ImGui::TextDisabled("%s", Sys::PathToUtf8(Sys::U8Path(dir).filename()).c_str());
    ImGui::Separator();

    if (ImGui::MenuItem("New Animation Graph")) CreateAnimationAsset(true, dir);
    if (ImGui::MenuItem("New Animation Set")) CreateAnimationAsset(false, dir);

    if (ImGui::BeginMenu("New folder"))
    {
        static char s_newName[128] = "New Folder";
        static std::string s_newFor;
        if (s_newFor != dir) { s_newFor = dir; strncpy_s(s_newName, "New Folder", _TRUNCATE); }
        ImGui::SetNextItemWidth(220.f);
        const bool commit = ImGui::InputText("##newFolderName", s_newName, sizeof(s_newName),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Enter creates it inside this folder.");
        if (commit && s_newName[0])
        {
            CreateAssetFolder(dir, s_newName);
            s_newFor.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    if (isRoot) ImGui::BeginDisabled();
    if (ImGui::BeginMenu("Rename"))
    {
        static char s_renameBuf[260] = {};
        static std::string s_renameFor;
        if (s_renameFor != dir)
        {
            s_renameFor = dir;
            strncpy_s(s_renameBuf,
                      Sys::PathToUtf8(Sys::U8Path(dir).filename()).c_str(), _TRUNCATE);
        }
        ImGui::SetNextItemWidth(260.f);
        const bool commit = ImGui::InputText("##folderRename", s_renameBuf, sizeof(s_renameBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Every asset inside is repointed - references,\n"
                            "identities and sidecars all follow.");
        if (commit && s_renameBuf[0])
        {
            const std::string dest = Sys::PathToUtf8(
                Sys::U8Path(dir).parent_path() / Sys::U8Path(std::string(s_renameBuf)));
            MoveAssetFolder(dir, dest);
            s_renameFor.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Delete folder"))
    {
        // Count first: "delete this folder" with no idea what is in it is how people lose work.
        // Once per opening of the menu, not per frame it stays open.
        static std::string s_countedDir;
        static int         s_countedFrame = -2;
        static std::size_t s_countedFiles = 0;
        const int frame = ImGui::GetFrameCount();
        if (s_countedDir != dir || s_countedFrame != frame - 1)
        {
            std::error_code ec;
            s_countedFiles = 0;
            for (fs::recursive_directory_iterator
                     it(Sys::U8Path(dir), fs::directory_options::skip_permission_denied, ec), end;
                 it != end && s_countedFiles < 10000; it.increment(ec))
            {
                if (ec) break;
                if (it->is_regular_file(ec)) ++s_countedFiles;
            }
            s_countedDir = dir;
        }
        s_countedFrame = frame;
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                           "Moves %zu file(s) to the Recycle Bin.", s_countedFiles);
        ImGui::TextDisabled("References held by other assets are NOT repaired -\n"
                            "the targets stop existing.");
        if (ImGui::MenuItem("Confirm delete folder"))
        {
            // Deferred: this runs inside the traversal drawing the very node
            // it is about to remove.
            m_pendingDeleteFolder = dir;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    if (isRoot) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer"))
    {
        std::error_code ec;
        const fs::path abs = fs::absolute(Sys::U8Path(dir), ec);
        if (!ec)
        {
            const std::wstring arg = abs.native();
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(),
                          nullptr, SW_SHOWNORMAL);
        }
    }
    if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(dir.c_str());

    ImGui::EndPopup();
    ImGui::PopID();
}

// Pins the drag-payload decision without a filesystem scan or an ImGui context.
void EditorLayer::TestOnly_SetAssets(const std::vector<std::string>& paths,
                                     const std::vector<std::string>& selected)
{
    m_assets.clear();
    for (const std::string& p : paths)
    {
        AssetEntry ae;
        ae.path = p;
        ae.name = Sys::PathToUtf8(Sys::U8Path(p).filename());
        ae.stem = Sys::PathToUtf8(Sys::U8Path(p).stem());
        const std::string ext = Resource::AssetRefs::ToLowerAscii(
            Sys::PathToUtf8(Sys::U8Path(p).extension()));
        ae.type = ext.empty() ? AssetType::Folder : AssetTypeForExt(ext);
        m_assets.push_back(std::move(ae));
    }
    m_selectedAssetPaths = selected;
    m_selectedAsset      = selected.empty() ? -1 : 0;
}

// ==== Rendered thumbnail tiles ====
// ServiceAssetThumbnails does the tile work in ProcessPendingActions - the game half's
// serial window, before renderer.BeginFrame, so an enqueue there reaches the SAME frame's
// PreviewRenderer::ExtractRequests. Not in the ImGui pass, which may not create or destroy
// a texture (an earlier AddImage's descriptor is still in this frame's draw data), touch
// the renderer's runtime-material maps (game-half state) or parse a material off disk as a
// folder scrolls: that pass only raises flags, and the invalidation hooks reachable from it
// queue their destroys (EvictAssetPreviewTile).

namespace
{
    // CanonicalMaterialPath is TU-local in MaterialSerializer. Its three steps (relativise,
    // forward slashes, strip a leading "./") are repeated here rather than widening that
    // public surface for a path comparison.
    std::string CanonicalMatPath(const std::string& path)
    {
        std::string c = Sys::RelativizeAssetPath(path);
        for (char& ch : c) if (ch == '\\') ch = '/';
        while (c.size() >= 2 && c[0] == '.' && c[1] == '/') c.erase(0, 2);
        return c;
    }
}

void EditorLayer::ResolveAssetPreviewIdentity(AssetEntry& ae)
{
    if (ae.previewKey != 0) return;

    // GuidForPath, NOT EnsureGuid: EnsureGuid WRITES a .meta, and scrolling past a thousand
    // unstamped materials would author a thousand files into the working tree. An unstamped
    // asset gets a path-derived, memory-only key; it is stamped when something references it.
    // A Mesh item's tile uses its model's guid plus the MeshKey: a disk-cache stem per Mesh
    // that survives a model rename (an unstamped model leaves its Meshes memory-only).
    std::string meshModel;
    uint64_t    meshKey = 0;
    const bool  meshRef = Resource::ParseMeshAssetRef(ae.path, meshModel, meshKey);
    const ECS::Guid g = Resource::AssetGuidIndex::Get().GuidForPath(meshRef ? meshModel : ae.path);
    if (g.IsValid())
    {
        ae.previewGuid = g.ToHex();
        ae.previewKey  = g.lo ^ (g.hi * 0x9E3779B97F4A7C15ull);
        if (meshRef)
        {
            ae.previewGuid += "m" + Resource::MeshKeyToHex(meshKey);
            ae.previewKey  ^= meshKey * 0xC2B2AE3D27D4EB4Full;   // odd: never maps a key to 0
        }
    }
    else
    {
        ae.previewGuid.clear();
        // FNV-1a over the canonical path. Memory-only: nothing keys a disk
        // entry on it, because a path-derived key does not survive a rename and
        // a stale tile that survives a rename is worse than no tile at all.
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : Sys::RelativizeAssetPath(ae.path))
        {
            if (c == '\\') c = '/';
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        ae.previewKey = h;
    }
    if (ae.previewKey == 0) ae.previewKey = 1;   // 0 means "no identity"
}

void EditorLayer::EvictAssetPreviewTile(AssetEntry& ae, bool countIt)
{
    if (m_renderer && ae.previewKey) m_renderer->CancelPreviewThumbnail(ae.previewKey);
    if (ae.previewTile.IsValid())
    {
        // Queued, not destroyed: this is reachable from the ImGui pass through the invalidation
        // hooks, and the SRV an earlier ImGui::Image captured must outlive this frame's draw data.
        // ServiceAssetThumbnails destroys it at the next frame boundary.
        m_pendingTileDestroy.push_back(ae.previewTile);
        ae.previewTile = RHI::Texture{};
        if (countIt) ++m_thumbEvicted;
    }
    if (ae.thumbnail != Resource::kInvalidTextureHandle && m_textureSys && m_gfx)
    {
        m_textureSys->Release(ae.thumbnail, *m_gfx);
        ae.thumbnail = Resource::kInvalidTextureHandle;
        if (countIt) ++m_thumbEvicted;
    }
    ae.previewReady    = false;
    ae.previewGaveUp   = false;
    ae.lastVisibleTime = -1.f;
}

void EditorLayer::InvalidateMaterialThumbnails(const std::string& matPath)
{
    if (matPath.empty()) return;
    auto& cache = Editor::PreviewThumbnailCache::Get();

    // MaterialsDependingOn answers from the serializer's REVERSE INDEX, not its cache, so it
    // works right after a save invalidated the cache: an instance of an edited master is
    // re-tiled too.
    std::vector<std::string> affected = Resource::MaterialsDependingOn(matPath);
    affected.push_back(CanonicalMatPath(matPath));

    // Asset-tile accounting over the whole affected chain: `tiles` = asset tiles the dep
    // index named, `dirty` = those in the current folder (the only ones re-rendered),
    // `outside` = the rest, evicted and redrawn on their folder's next visit.
    std::size_t assetTiles = 0, assetDirty = 0, assetOutside = 0;

    for (const std::string& p : affected)
    {
        // The disk entry goes by GUID and does not need the asset to be in the
        // browser's current folder - which is the usual case, since a save
        // happens from the inspector.
        const ECS::Guid g = Resource::AssetGuidIndex::Get().GuidForPath(p);
        if (g.IsValid()) cache.Evict(g.ToHex());

        const std::string canon = CanonicalMatPath(p);
        // The folder's entries and the Inspector material rows' tiles.
        ForEachPreviewEntry([&](AssetEntry& ae)
        {
            if (ae.type != AssetType::Material) return;
            if (CanonicalMatPath(ae.path) != canon) return;
            ++ae.previewGen;          // void anything in flight
            EvictAssetPreviewTile(ae, /*countIt=*/false);
            ae.previewDirty = true;   // re-render on the next service
        });
        // ---- And the asset tiles that use it ----
        // PreviewThumbnailCache::Store records every dependency of a tile (an asset tile: its own
        // file, every .iscn it expands, every .meshlib a part comes from and every material those
        // parts name); GuidsDependingOn queries that index, the same walk
        // NotifyThumbnailDependencyChanged does for a file change. Without it a tile stays stale
        // until the residency sweep ages it out, which for an on-screen tile is never.
        // Only current-folder entries are re-rendered (`m_assets` holds ONE folder); a tile in
        // another folder is evicted from the cache by guid and redrawn on that folder's next visit.
        for (const std::string& guid : cache.GuidsDependingOn(canon))
        {
            cache.Evict(guid);
            bool here = false;
            // The Mesh item tiles too (ForEachPreviewEntry): their deps name the model's part
            // materials exactly as a model tile's do.
            ForEachPreviewEntry([&](AssetEntry& ae)
            {
                // Material entries are skipped: the loop above dirtied them by path, and a second
                // previewGen bump would void the request the first one just made.
                if (ae.type == AssetType::Material)            return;
                if (PreviewKindForPath(ae.path) ==
                    Preview::AssetKind::None)                  return;
                if (ae.previewGuid != guid)                    return;
                ++ae.previewGen;          // void anything in flight
                EvictAssetPreviewTile(ae, /*countIt=*/false);
                ae.previewDirty = true;
                ++assetDirty;
                here = true;
                // And the memoised composition, which the tile cache knows nothing about (the second
                // half NotifyThumbnailDependencyChanged does for a file change).
                if (m_renderer) m_renderer->InvalidatePreviewAsset(ae.path);
            });
            ++assetTiles;
            if (!here) ++assetOutside;
        }
    }
    // One line, with the frame on it: a pack checks the tile is dirty in the SAME frame as
    // the save by comparing `frame` with the frame the save reported.
    if (assetTiles)
        LOG_INFO("[PREVIEW] material save -> asset tiles: mat='%s' tiles=%zu "
                 "dirty=%zu outside=%zu folderEntries=%zu frame=%llu",
                 matPath.c_str(), assetTiles, assetDirty, assetOutside,
                 m_assets.size(),
                 static_cast<unsigned long long>(m_editorFrame));
}

void EditorLayer::NotifyThumbnailDependencyChanged(
    const std::vector<std::string>& changedPaths)
{
    if (changedPaths.empty()) return;
    auto& cache = Editor::PreviewThumbnailCache::Get();
    std::size_t hits = 0;
    // The cross-folder contract as numbers. `m_assets` holds ONE folder, so a dependency change
    // can only mark that folder's entries; every other tile it touches is evicted from the
    // cache and redrawn on its folder's next visit. Do not widen `m_assets` to the whole scan:
    // an evict-only invalidation would become a re-render of every tile in the project.
    //   dirty         current-folder entries marked for re-render
    //   outside       evictions with no entry here (cross-folder: evicted, NOT re-rendered)
    //   folderEntries what `m_assets` holds, the ceiling `dirty` can reach
    std::size_t dirty   = 0;
    std::size_t outside = 0;

    for (const std::string& changed : changedPaths)
    {
        // Texture and custom-shader changes, through ONE index: the cache records every texture a
        // material resolved and its two custom shader paths in the sidecar's `deps`, so a
        // re-imported .itex and an edited .hlsl are the same lookup. Neither has a dependency
        // stamp anywhere else in the engine.
        for (const std::string& guid : cache.GuidsDependingOn(changed))
        {
            cache.Evict(guid);
            ++hits;
            bool inThisFolder = false;
            // And every Mesh item tile (ForEachPreviewEntry): a re-imported model re-tiles its
            // expanded Meshes too.
            ForEachPreviewEntry([&](AssetEntry& ae)
            {
                // Asset tiles are on the same index: their deps are their own file, every .iscn they
                // expand, every .meshlib a part comes from and every material those parts name, so a
                // re-imported model or an edited part material arrives here by guid.
                const Preview::AssetKind kind = PreviewKindForPath(ae.path);
                if (ae.type != AssetType::Material &&
                    kind == Preview::AssetKind::None) return;
                if (ae.previewGuid != guid) return;
                ++ae.previewGen;
                EvictAssetPreviewTile(ae, /*countIt=*/false);
                ae.previewDirty = true;
                ++dirty;
                inThisFolder = true;
                // And the memoised composition, which the tile cache knows nothing about: a changed
                // .iscn or .ipfb means the placement list itself is stale, not just the picture.
                if (kind != Preview::AssetKind::None && m_renderer)
                    m_renderer->InvalidatePreviewAsset(ae.path);
            });
            if (!inThisFolder) ++outside;
        }
    }
    // Do not change the literal up to "dependency file(s)": preview_churn asserts it verbatim.
    // The name=value fields after it are read by the runner's logLineFields.
    if (hits)
        LOG_INFO("[PREVIEW] thumbnails: %zu tile(s) invalidated by %zu changed "
                 "dependency file(s) - evicted=%zu dirty=%zu outside=%zu "
                 "folderEntries=%zu files=%zu",
                 hits, changedPaths.size(), hits, dirty, outside,
                 m_assets.size(), changedPaths.size());
}

void EditorLayer::ServiceAssetThumbnails()
{
    // Cube tiles not drawn for two frames hand their texture views back.
    SweepCubeThumbnails(false);
    if (!m_gfx) return;

    // ---- 0. Deferred destroys ----
    // Safe here: the frame that queued these has been presented, so no ImGui draw list still
    // names their descriptors.
    for (RHI::Texture& t : m_pendingTileDestroy)
        if (t.IsValid()) m_gfx->DestroyTexture(t);
    m_pendingTileDestroy.clear();

    if (!m_renderer || !m_textureSys || !m_resourceMgr) return;

    auto& cache = Editor::PreviewThumbnailCache::Get();
    // One stat per file per frame - see the memo's comment in the cache.
    Editor::PreviewThumbnailCache::BeginFrame();

    // ---- 1. Boot GC, once ----
    // After the first scan, which builds AssetGuidIndex. The orphan half gets an EMPTY live
    // set on purpose: m_assets holds one folder, and treating it as every guid in the project
    // would delete every other folder's tiles. BootGC reads an empty set as "no information"
    // and applies the size cap alone.
    if (!m_thumbGCDone && Resource::AssetGuidIndex::Get().Built())
    {
        m_thumbGCDone = true;
        cache.BootGC(std::unordered_set<std::string>{},
                     Editor::PreviewThumbnailCache::kDefaultSizeCap);
    }

    // ---- 2. Apply what the render half finished ----
    // The ONLY place previewDirty is cleared: clearing it at enqueue would let a record
    // early-out drop the asset from the queue for the rest of the session.
    m_thumbResultScratch.clear();
    m_renderer->TakePreviewThumbnailResults(m_thumbResultScratch);
    for (const Preview::ThumbResult& r : m_thumbResultScratch)
    {
        // The folder's entries, then the Mesh item tiles; keys are unique.
        AssetEntry* hit = nullptr;
        for (AssetEntry& ae : m_assets)
            if (ae.previewKey == r.assetKey) { hit = &ae; break; }
        if (!hit)
            for (auto& kv : m_meshTiles)
                if (kv.second.previewKey == r.assetKey) { hit = &kv.second; break; }
        if (!hit)   // the Inspector material rows' tiles
            for (auto& kv : m_materialRowTiles)
                if (kv.second.previewKey == r.assetKey) { hit = &kv.second; break; }
        if (!hit) continue;
        AssetEntry& ae = *hit;
        // A result minted before an invalidation must not mark the tile clean. Every
        // invalidation bumps the generation, so a mismatch means "this picture is of the
        // previous version".
        if (ae.previewGen != r.generation) continue;
        ae.previewDirty = false;
        if (r.ok) { ae.previewReady = true; ++m_thumbApplied; }
        else      { ae.previewGaveUp = true; }
    }

    // ---- 3. Drain the readback ring into the disk cache ----
    // The pixels arrive kFrameCount frames after the record, off the ring the render half
    // wrote - never a FlushAndWait.
    // Written inline, not on a TaskSystem worker: one tile is a 64 KiB memcpy, an uncompressed
    // 128^2 SaveToDDSMemory (no BC encode) and two small file writes, a fraction of a
    // millisecond at the lane's one-tile-per-frame ceiling (the churn pack's cpuMs p99
    // assertion holds it). A worker would need the pixels to outlive the call and a
    // thread-safe dep index.
    {
        uint64_t key = 0; uint32_t gen = 0, pitch = 0, w = 0, h = 0;
        const uint8_t* rgba = nullptr;
        while (m_renderer->TakePreviewThumbnailPixels(key, gen, rgba, pitch, w, h))
        {
            auto it = m_thumbPendingInfo.find(key);
            if (it == m_thumbPendingInfo.end()) continue;
            const PendingThumbInfo info = it->second;
            m_thumbPendingInfo.erase(it);
            // Stale: the material was invalidated while this picture was in
            // flight. Writing it would cache the OLD look under stamps taken
            // from the NEW file, which is the one combination nothing can
            // invalidate later.
            if (info.generation != gen)        continue;
            if (info.guidHex.empty())          continue;   // memory-only asset
            if (!rgba || w == 0 || h == 0)     continue;
            cache.Store(info.guidHex, info.srcPath, info.deps, rgba, w, h, pitch);
        }
    }

    // ---- 4. Eviction sweep for rendered tiles ----
    // The browser owns these textures and a destroy may not happen in an ImGui handler, so
    // this sweep runs here, not in RenderAssetBrowser's TextureSystem sweep.
    {
        const float now = static_cast<float>(ImGui::GetTime());
        ForEachPreviewEntry([&](AssetEntry& ae)
        {
            if (ae.type != AssetType::Material &&
                PreviewKindForPath(ae.path) == Preview::AssetKind::None) return;
            // Either kind of residency: RenderAssetBrowser's sweep releases the cached-.itex handle
            // only on frames the panel draws, and a tile can be resident on frames it does not.
            // Releasing both here ages the two halves on one clock.
            const bool hasCached = (ae.thumbnail != Resource::kInvalidTextureHandle);
            if (!ae.previewTile.IsValid() && !hasCached) return;
            if (ae.lastVisibleTime < 0.f)       return;
            if (now - ae.lastVisibleTime <= m_previewEvictDelay) return;
            // NEVER while its request is in flight: the tile handle is inside a
            // command list this frame, and destroying it recycles a pool slot
            // the copy still names.
            if (m_renderer->HasPreviewThumbnailInFlight(ae.previewKey)) return;
            EvictAssetPreviewTile(ae, /*countIt=*/true);
        });

        // A Mesh item tile that left the screen is forgotten, not just released: no rescan ever
        // drops it, and expanding many models must not keep one entry per Mesh ever shown.
        // Erased once off-screen for the eviction delay with nothing in flight; its queued
        // request is cancelled with it (EvictAssetPreviewTile).
        for (auto it = m_meshTiles.begin(); it != m_meshTiles.end();)
        {
            AssetEntry& ae = it->second;
            const bool stale = ae.lastVisibleTime < 0.f || now - ae.lastVisibleTime > m_previewEvictDelay;
            if (!stale || (ae.previewKey && m_renderer->HasPreviewThumbnailInFlight(ae.previewKey)))
            {
                ++it;
                continue;
            }
            EvictAssetPreviewTile(ae, /*countIt=*/true);
            it = m_meshTiles.erase(it);
        }
        // An Inspector material row's tile, the same way - no folder lists it.
        for (auto it = m_materialRowTiles.begin(); it != m_materialRowTiles.end();)
        {
            AssetEntry& ae = it->second;
            const bool stale = ae.lastVisibleTime < 0.f || now - ae.lastVisibleTime > m_previewEvictDelay;
            if (!stale || (ae.previewKey && m_renderer->HasPreviewThumbnailInFlight(ae.previewKey)))
            {
                ++it;
                continue;
            }
            EvictAssetPreviewTile(ae, /*countIt=*/true);
            it = m_materialRowTiles.erase(it);
        }
    }

    // ---- 5. Enqueue: cache hits first, renders second ----
    // Its OWN per-frame counter, not m_previewLoadsThisFrame: that one resets only on frames
    // the panel draws (RenderAssetBrowser) while this runs every frame, so a shared counter
    // would be spent once and never refilled. Both use m_previewLoadBudgetPerFrame.
    int cacheLoads = 0;
    int enqueued   = 0;
    // The lane can refuse itself for the session (HDR output: the tone-map output is FP16
    // and cannot be copied into an R8G8B8A8 tile); then every QueuePreviewThumbnail returns
    // false. Do not treat that as "skip to the next entry": it walks the whole folder every
    // frame, creating a tile and parsing a material per entry. The disk cache is still read;
    // only the render path stops.
    const bool thumbLaneOff = m_renderer->ArePreviewThumbnailsDisabled();
    // The folder's entries, then the Mesh item tiles, then the Inspector material rows' tiles:
    // one walk, one budget, so `break` below means "done for this frame".
    std::vector<AssetEntry*> entries;
    entries.reserve(m_assets.size() + m_meshTiles.size() + m_materialRowTiles.size());
    for (AssetEntry& ae : m_assets)         entries.push_back(&ae);
    for (auto& kv : m_meshTiles)            entries.push_back(&kv.second);
    for (auto& kv : m_materialRowTiles)     entries.push_back(&kv.second);
    for (AssetEntry* entry : entries)
    {
        AssetEntry& ae = *entry;
        const Preview::AssetKind assetKind = PreviewKindForPath(ae.path);
        const bool isMaterialTile = (ae.type == AssetType::Material);
        if ((!isMaterialTile && assetKind == Preview::AssetKind::None) ||
            !ae.previewDirty) continue;
        if (enqueued >= m_thumbEnqueuePerFrame) break;
        ResolveAssetPreviewIdentity(ae);
        if (m_renderer->HasPreviewThumbnailInFlight(ae.previewKey)) continue;
        // The file the disk cache stamps this picture against: a Mesh item's
        // tile has no file of its own, its model is it.
        const std::string srcFile = PreviewSourceFile(ae.path);

        // (a) The disk cache, loaded through the same streaming path as a .itex asset
        //     (TextureSystem::Acquire, the 4-per-frame budget, the 8-second release): no new code,
        //     no new lifetime rule.
        if (!ae.previewGuid.empty())
        {
            // Budget exhausted means SKIP, not render: falling through to (b) would re-render tiles
            // that are fresh on disk. The entry stays dirty and the next frame asks again.
            if (cacheLoads >= m_previewLoadBudgetPerFrame) continue;
            std::string itex;
            if (cache.LookupFresh(ae.previewGuid, srcFile, itex))
            {
                ae.thumbnail = m_textureSys->Acquire(itex, *m_resourceMgr, *m_gfx);
                ++cacheLoads;
                ae.previewDirty = false;
                ae.previewReady = false;   // the cached handle drives the draw
                continue;
            }
        }

        // (b) Render it; the tile is created here, in the serial window.
        // Not when the lane is off: previewGaveUp latches the entry so the visibility sweep cannot
        // re-arm previewDirty next frame. Any invalidation clears the latch, so if the output
        // returns to SDR the lane comes back with the next real change.
        if (thumbLaneOff)
        {
            ae.previewGaveUp = true;
            ae.previewDirty  = false;
            continue;
        }
        if (!ae.previewTile.IsValid())
        {
            RHI::TextureDesc td{};
            td.width      = Preview::kThumbnailSize;
            td.height     = Preview::kThumbnailSize;
            td.format     = RHI::Format::R8G8B8A8_UNORM;
            td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            td.usage      = RHI::Usage::DEFAULT;
            // Stated explicitly: CreateTexture derives the initial state from this field for an
            // SRV-only texture, and the record half's barrier round trip (SHADER_RESOURCE ->
            // COPY_DEST -> COPY_SRC -> SHADER_RESOURCE) assumes it on entry. A mismatch would be
            // reported against the copy, not the creation.
            td.layout     = RHI::ResourceState::SHADER_RESOURCE;
            td.debugName = "AssetBrowser.MaterialTile";
            if (!m_gfx->CreateTexture(td, ae.previewTile, nullptr))
            {
                ae.previewGaveUp = true;
                ae.previewDirty  = false;
                continue;
            }
        }

        // The DISK state, deliberately: a tile shows what the FILE looks like, not what the
        // inspector shows (unsaved edits are not drawn). LoadMaterial resolves the whole
        // .imatinst chain and is cached, so a file is parsed only the first time.
        MaterialComponent mat{};
        Resource::MaterialLoadInfo info{};
        // An asset tile has no single material: its parts' materials are resolved on the game
        // half by ExtractThumbnails, so the parse is skipped (on a .meshlib path it would fail and
        // latch previewGaveUp).
        // LiveMaterialBypass: the FILE, not a Material Editor tab's live version - the picture is
        // cached under the file's stamps.
        const Resource::LiveMaterialBypass fileNotLive;
        if (isMaterialTile && !Resource::LoadMaterial(ae.path, mat, &info))
        {
            ae.previewGaveUp = true;
            ae.previewDirty  = false;
            continue;
        }

        Preview::ThumbRequest req;
        req.assetKey   = ae.previewKey;
        req.generation = ae.previewGen;
        req.mat        = mat;
        req.assetKind  = assetKind;
        if (assetKind != Preview::AssetKind::None) req.assetPath = ae.path;
        req.tile       = ae.previewTile;
        // A refusal consumes the budget: QueuePreviewThumbnail refuses when the FIFO is at
        // Preview::kMaxThumbnailQueue or the lane is off, and neither changes for the next entry,
        // so `continue` would create a tile and parse a material per entry every frame. The entry
        // stays dirty and is asked again once the FIFO drains.
        if (!m_renderer->QueuePreviewThumbnail(req)) break;

        PendingThumbInfo pi;
        pi.srcPath    = srcFile;
        pi.guidHex    = ae.previewGuid;
        pi.generation = ae.previewGen;
        // Bounded: an entry is erased when its readback pixels are consumed, and if the readback
        // ring failed to create they never arrive. Dropping them only costs the disk cache: the
        // tiles still render and display.
        if (m_thumbPendingInfo.size() > 512)
        {
            m_thumbPendingInfo.clear();
            LOG_WARNING("[PREVIEW] thumbnail encode backlog exceeded 512 entries - "
                        "the readback ring is not delivering; tiles will render "
                        "but will not reach the disk cache");
        }
        pi.deps       = Editor::PreviewThumbnailCache::CollectDeps(srcFile, mat);
        // ---- An asset's dependency set ----
        // CollectDeps above stamped only the asset FILE (a .meshlib has no material fields). An
        // asset tile's picture also depends on every .iscn a prefab expands, every .meshlib a part
        // comes from and every material those parts name, with each material's own textures,
        // custom shaders and parent chain. Without these a re-imported model or an edited part
        // material would keep its old tile.
        if (assetKind != Preview::AssetKind::None && m_renderer)
        {
            std::vector<std::string> files, mats;
            m_renderer->GetPreviewAssetDeps(ae.path, assetKind, files, mats);
            for (const std::string& f : files)
                pi.deps.push_back(Editor::PreviewThumbnailCache::Stamp(f));
            for (const std::string& mp : mats)
            {
                pi.deps.push_back(Editor::PreviewThumbnailCache::Stamp(mp));
                MaterialComponent pm{};
                Resource::MaterialLoadInfo pinfo{};
                if (!Resource::LoadMaterial(mp, pm, &pinfo)) continue;
                for (const auto& d : Editor::PreviewThumbnailCache::CollectDeps(mp, pm))
                    pi.deps.push_back(d);
                // ...and that material's own parent chain, on the same terms
                // as the material-tile branch below.
                std::string parent = pinfo.parentPath;
                for (int depth = 0; depth < 8 && !parent.empty(); ++depth)
                {
                    pi.deps.push_back(Editor::PreviewThumbnailCache::Stamp(parent));
                    MaterialComponent gp{};
                    Resource::MaterialLoadInfo ginfo{};
                    if (!Resource::LoadMaterial(parent, gp, &ginfo)) break;
                    parent = ginfo.parentPath;
                }
            }
        }
        // The parent chain, stamped too. CollectDeps records the LEAF file, and the editor's Save
        // hook covers chain members saved from the inspector (MaterialsDependingOn), but an .imat
        // edited OUTSIDE the editor (a git checkout, another tool) reaches neither and would leave
        // a cache entry nothing invalidates. LoadMaterial is cached: each parent is parsed once
        // per session.
        {
            std::string parent = info.parentPath;
            for (int depth = 0; depth < 8 && !parent.empty(); ++depth)
            {
                pi.deps.push_back(Editor::PreviewThumbnailCache::Stamp(parent));
                MaterialComponent pm{};
                Resource::MaterialLoadInfo pinfo{};
                if (!Resource::LoadMaterial(parent, pm, &pinfo)) break;
                parent = pinfo.parentPath;
            }
        }
        m_thumbPendingInfo[ae.previewKey] = std::move(pi);
        ++enqueued;
    }
}

// Shutdown release; the contract is at the declaration.
void EditorLayer::ReleaseThumbnailTiles(IGraphicsDevice& gfx)
{
    std::size_t freed = 0;
    ForEachPreviewEntry([&](AssetEntry& ae)   // + the Mesh item and Inspector row tiles
    {
        if (!ae.previewTile.IsValid()) return;
        // Cancel the lane's requests for this key first (queued and in flight), as the eviction
        // path does.
        if (m_renderer && ae.previewKey)
            m_renderer->CancelPreviewThumbnail(ae.previewKey);
        gfx.DestroyTexture(ae.previewTile);
        ae.previewTile  = {};
        ae.previewReady = false;
        ++freed;
    });
    for (RHI::Texture& t : m_pendingTileDestroy)
        if (t.IsValid()) { gfx.DestroyTexture(t); ++freed; }
    m_pendingTileDestroy.clear();
    if (freed)
        LOG_INFO("[PREVIEW] shutdown: released %zu thumbnail tile texture(s)", freed);
}

void EditorLayer::ReportThumbnailSummary(IGraphicsDevice& gfx) const
{
    // The FINAL pass. A single-pass session reports pass=1, so the line's shape
    // does not depend on whether -preview_churn_passes was given.
    EmitThumbnailPassSummary(gfx, m_thumbChurnPass + 1);
}

void EditorLayer::EmitThumbnailPassSummary(IGraphicsDevice& gfx,
                                           uint32_t passIndex) const
{
    const auto& cache = Editor::PreviewThumbnailCache::Get();
    const uint64_t renderedTotal =
        m_renderer ? m_renderer->GetPreviewThumbnailsRenderedTotal() : 0ull;

    // ---- Per-pass deltas, not process totals ----
    // A two-pass churn asserts opposite numbers per pass (pass 1 renders every material, pass
    // 2 serves them all from disk), which process totals cannot express. Each counter is
    // baselined at the pass boundary instead of reset: PreviewRenderer's is monotonic by
    // contract (preview_edit latches it).
    const uint64_t rendered = renderedTotal - m_thumbPassBase.rendered;
    const uint32_t applied  = m_thumbApplied - m_thumbPassBase.applied;
    const uint32_t hits     = cache.Hits()   - m_thumbPassBase.hits;
    const uint32_t writes   = cache.Writes() - m_thumbPassBase.writes;
    const uint32_t evicted  = m_thumbEvicted - m_thumbPassBase.evicted;
    const uint32_t rescans  = m_thumbRescans - m_thumbPassBase.rescans;
    const uint32_t orphans  = m_thumbOrphansReleased - m_thumbPassBase.orphans;

    // Silent when the lane did nothing: editor packs that open the browser at the asset root
    // (no .imat there) must not see this line in their logs.
    if (rendered == 0 && hits == 0 && writes == 0 && evicted == 0)
        return;

    auto& dx = static_cast<GraphicsDX12&>(gfx);
    const auto cs = dx.GetCbvSrvUavHeapStats();
    const auto rs = dx.GetRtvHeapStats();
    const auto ds = dx.GetDsvHeapStats();
    const auto ss = dx.GetSamplerHeapStats();

    // One line, name=value throughout: the scenario runner parses it (expect.logLineFields).
    // The pool high-waters ride in the SAME line so a churn pack can assert "N tiles AND the
    // pools did not climb" for one run.
    // `applied` (game half, results consumed) and `rendered` (render half, pictures recorded)
    // must agree: a gap means the generation guard is dropping ordinary traffic, not just
    // real invalidations.
    LOG_INFO("[PREVIEW] thumbnails pass=%u rendered=%llu applied=%u cacheHits=%u "
             "evicted=%u diskWrites=%u rescans=%u orphansReleased=%u "
             "cbvUsed=%u cbvFree=%u rtvUsed=%u "
             "rtvFree=%u dsvUsed=%u dsvFree=%u samplerUsed=%u samplerFree=%u "
             "cmdListPeak=%u",
             passIndex, static_cast<unsigned long long>(rendered), applied,
             hits, evicted, writes, rescans, orphans,
             cs.used, cs.freeBlocks, rs.used, rs.freeBlocks,
             ds.used, ds.freeBlocks, ss.used, ss.freeBlocks,
             dx.GetCommandListPeak());
}

void EditorLayer::BeginThumbnailPass(IGraphicsDevice& gfx)
{
    // Emit the finished pass, then re-baseline. Called from the churn driver at
    // a folder-rotation boundary; the LAST pass is emitted by
    // ReportThumbnailSummary at shutdown, so every pass produces exactly one
    // line and the pass numbers are contiguous.
    EmitThumbnailPassSummary(gfx, m_thumbChurnPass + 1);
    ++m_thumbChurnPass;

    const auto& cache = Editor::PreviewThumbnailCache::Get();
    m_thumbPassBase.rendered =
        m_renderer ? m_renderer->GetPreviewThumbnailsRenderedTotal() : 0ull;
    m_thumbPassBase.applied  = m_thumbApplied;
    m_thumbPassBase.hits     = cache.Hits();
    m_thumbPassBase.writes   = cache.Writes();
    m_thumbPassBase.evicted  = m_thumbEvicted;
    m_thumbPassBase.rescans  = m_thumbRescans;
    m_thumbPassBase.orphans  = m_thumbOrphansReleased;
}

// ==== Search over every folder ====

void EditorLayer::RenderAssetSearchResults()
{
    // The catalog walks again now and then while a search is up: an import or a delete elsewhere shows
    // within seconds.
    if (m_assetCatalogAskedAt < 0.0 || ImGui::GetTime() - m_assetCatalogAskedAt > 10.0)
    {
        m_assetCatalog.SetRoots({ m_assetDir, Sys::EngineDir() + "/engine" });
        m_assetCatalog.Refresh();
        m_assetCatalogAskedAt = ImGui::GetTime();
    }
    const auto snap = m_assetCatalog.Current();
    double micros = 0.0;
    constexpr std::size_t kLimit = 200;
    const std::vector<Editor::AssetCatalog::Entry> hits = m_assetCatalog.Search(m_assetFilter, kLimit, &micros);
    if (!snap)
    {
        ImGui::TextDisabled("Cataloguing every folder...");
        return;
    }
    ImGui::TextDisabled("%zu%s match(es) in every folder (%zu entries, walked in %.0f ms off the UI thread; "
                        "searched in %.0f us)", hits.size(), hits.size() >= kLimit ? "+" : "", snap->entries.size(),
                        snap->buildMs, micros);
    const float h = (std::min)(ImGui::GetContentRegionAvail().y * 0.45f, ImGui::GetTextLineHeightWithSpacing() * 12.f);
    if (ImGui::BeginChild("##assetSearchResults", ImVec2(0.f, h), ImGuiChildFlags_Borders))
    {
        for (const Editor::AssetCatalog::Entry& e : hits)
        {
            ImGui::PushID(e.path.c_str());
            if (ImGui::Selectable(e.folder ? (e.name + "/").c_str() : e.name.c_str(), false))
            {
                // Go to it: its folder (or the folder itself), then select it once listed.
                const std::filesystem::path p = Sys::U8Path(e.path);
                if (e.folder) SetCurrentAssetDir(e.path);
                else
                {
                    SetCurrentAssetDir(Sys::PathToUtf8(p.parent_path()));
                    m_pendingAssetSelect = e.path;
                    m_needsRescan = true;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.path.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
            ImGui::TextDisabled("%s", e.path.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
