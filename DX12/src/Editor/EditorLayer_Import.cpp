// Getting assets into the project: the texture import queue and its popup,
// model imports (prepare on a TaskSystem worker, commit on the main thread; the
// import dialog and its options, reimport and its review, progress, the
// scripted Editor.Import / Editor.Reimport), OS file drops, applying the
// collision bake, and the Unreal manifest import.

#include "Editor/EditorLayer.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/EditorLayerShared.h"
#include "Editor/CameraPreviewWindow.h"
#include "Editor/AssetPreviewWindow.h"
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/TextureViewerWindow.h"
#include "Editor/MaterialPropertyPanel.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/SplitViewportPanel.h"
#include "Editor/ChainPhysicsPreviewWindow.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "Harness/ControlChannelJobs.h" // pipe-requested imports and bakes finish a job; an inert stub in Retail
#include "Editor/EntityRefPicker.h"
#include "Sequencer/SequenceEval.h"
#include "ECS/GuidRegistry.h"
#include "AI/BTNode.h"
#include "Scene/MeshSpawner.h"
#include "Resource/SceneSerializer.h"
#include "Resource/AssetFS.h"
#include "Resource/PostProcessConfig.h"
#include "Resource/EngineSettings.h"
#include "ECS/StaticMeshComponents.h" // StaticMesh owners and their derived sections
#include "Resource/MeshLibIdentity.h" // MeshKeyToHex: the re-import report's keys
#include "Scene/MeshAssetRegistry.h"  // InvalidateModel after an asset-collision edit
#include "Tools/CollisionMeshBaker.h" // Bake render meshes into combined .meshlib
#include "Resource/MeshLibrary.h"     // the loaded libraries a model reimport leaves in place
#include "Resource/AssetHeader.h"
#include "System/TaskSystem.h"        // model imports and the Unreal cook run on workers
#include "System/PathUtil.h"
#include "Physics/PhysicsSystem.h"    // InvalidateMeshShapeCache after re-baking colliders
#include "Scene/SceneInstanceLoader.h"
#include "Resource/ModelImportWrite.h"   // non-destructive model-import write policy
#include "Resource/RetargetBaker.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/AnimationLoader.h"
#include "Renderer/Renderer.h"
#include <chrono>
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/AssetManager.h"
#include "Resource/MaterialImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialOverrideApply.h"
#include "Resource/MaterialLiveOverlay.h"
#include "Resource/AssetRefs.h"
#include "Resource/DecalMaterialSerializer.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"
#include "Resource/TextureImporter.h"
#include "Resource/TextureImportRecord.h"   // the import record ImportTextureFile writes
#include "Resource/AssetGuid.h"        // an import keeps (or mints) its .meta identity
#include "System/Stage.h"              // a long import declares itself to `status`
#include "Resource/ShaderImporter.h"
#include "Resource/SceneImporter.h"
#include "Resource/PmxImporter.h"
#include "Resource/VrmImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/VmdImporter.h"
#include "Audio/AudioImporter.h"
#include "Editor/DebugDrawSystem.h"
#include "Graphics/GraphicsDX12.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>   // the scripted import and bake job results
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>          // the collision apply's per-model buckets
#include <optional>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

using namespace DirectX;


// ==== Texture import ====
void EditorLayer::ProcessPendingTextureImports()
{
    if (!m_textureImportConfirmed)
        return;

    m_textureImportConfirmed = false;
    std::vector<PendingTextureImport> imports = std::move(m_pendingTextureImports);
    m_pendingTextureImports.clear();

    size_t importedCount = 0;
    for (const PendingTextureImport& item : imports)
        if (ImportTextureFile(item).ok)
            ++importedCount;

    LOG_INFO("EditorLayer: texture import finished (%zu/%zu succeeded)",
             importedCount, imports.size());
}

EditorLayer::TextureImportOutcome EditorLayer::ImportTextureFile(const PendingTextureImport& item)
{
    TextureImportOutcome out;
    auto fail = [&](const char* code, std::string message)
    {
        LOG_ERROR("EditorLayer: texture import '%s' -> '%s': %s", item.sourcePath.c_str(),
                  item.outputPath.c_str(), message.c_str());
        out.code    = code;
        out.message = std::move(message);
        return out;
    };

    Sys::Stage::Scope stage("importing", item.sourcePath);
    const std::vector<uint8_t> srcData = ReadFileFully(item.sourcePath);
    if (srcData.empty())
        return fail("read_failed", "could not read the source file");

    Resource::TextureImporter importer;
    Resource::TextureImportSettings settings = item.options;   // maxSize, mipFilter, flipGreen, mips
    settings.format = item.selectedFormat;
    const std::vector<uint8_t> blob = importer.ImportWithSettings(item.sourcePath, srcData, settings);
    if (blob.size() < sizeof(Resource::AssetHeader) + sizeof(Resource::TextureMetadata))
        return fail("import_failed", "the importer produced no texture (the source did not decode)");

    // Atomic: a reader - this engine's own reload, another process - sees the
    // old file or the new one, and a failed write leaves the old one whole.
    Sys::CreateParentDirs(item.outputPath);
    if (!Sys::WriteFileBytesAtomic(item.outputPath, blob.data(), blob.size()))
        return fail("write_failed", "could not write the .itex (the old file, if any, is unchanged)");

    Resource::TextureMetadata meta{};
    std::memcpy(&meta, blob.data() + sizeof(Resource::AssetHeader), sizeof(meta));
    out.ok     = true;
    out.bytes  = blob.size();
    out.width  = meta.width;
    out.height = meta.height;

    // The identity stays with the path: an existing sidecar's guid is kept (a
    // reimport must not orphan the references to it), a first import mints one.
    const ECS::Guid guid = Resource::AssetGuidIndex::Get().EnsureGuid(item.outputPath);
    if (guid.IsValid()) out.guid = guid.ToHex();

    // How it was made, beside the identity. A path with no sidecar (not
    // guid-tracked) keeps no record; its reimport guesses the settings.
    if (guid.IsValid())
    {
        Resource::TextureImportRecord rec;
        rec.source = Sys::RelativizeAssetPath(item.sourcePath);
        for (char& c : rec.source) if (c == '\\') c = '/';
        rec.settings = settings;
        std::string why;
        out.record = Resource::WriteTextureImportRecord(item.outputPath, rec, &why);
        if (!out.record)
            LOG_WARNING("EditorLayer: texture import '%s': its import record was not written (%s) - a Reimport "
                        "will guess the settings", item.outputPath.c_str(), why.c_str());
    }

    out.reload = "not_loaded";
    if (m_textureSys && m_resourceMgr)
    {
        using RR = Resource::TextureSystem::ReloadRequest;
        switch (m_textureSys->RequestReload(item.outputPath, *m_resourceMgr, &out.reloadJobId))
        {
        case RR::Requested: out.reload = "requested"; break;
        case RR::Unchanged: out.reload = "unchanged"; break;
        case RR::NotLoaded: out.reload = "not_loaded"; break;
        }
    }
    m_needsRescan = true;
    LOG_INFO("EditorLayer: imported '%s' -> '%s' as %s (%ux%u, %zu bytes, reload %s; maxSize=%u mipFilter=%s "
             "flipGreen=%d mips=%d record=%d)",
             item.sourcePath.c_str(), item.outputPath.c_str(),
             Resource::TextureImporter::GetFormatName(item.selectedFormat),
             out.width, out.height, out.bytes, out.reload.c_str(), settings.maxSize,
             Resource::TextureImporter::GetMipFilterId(settings.mipFilter), settings.flipGreen ? 1 : 0,
             settings.generateMips ? 1 : 0, out.record ? 1 : 0);
    return out;
}

void EditorLayer::QueueTextureReimport(const std::string& itex, const std::string& sourceOverride)
{
    const Resource::TextureImportRecord rec = Resource::TextureImportRecordFor(itex);
    ScriptedImport s;
    s.reimport             = itex;
    s.sourceOverride       = sourceOverride;
    s.item.outputPath      = itex;
    s.item.sourcePath      = sourceOverride.empty() ? rec.source : sourceOverride;
    s.item.options         = rec.settings;
    s.item.selectedFormat  = rec.settings.format;
    // A guess that could not name the format: what the source's name suggests.
    if (!rec.known && !s.item.sourcePath.empty())
        s.item.selectedFormat = Resource::TextureImporter::SuggestFormat(s.item.sourcePath);
    s.item.suggestedFormat = s.item.selectedFormat;
    m_scriptedImports.push_back(std::move(s));
}

// The importer's output, held between a review and its commit. The
// importers' blobs live here and `outFiles` borrows them: nothing resizes
// these vectors after PrepareModelImport fills them, so the pointers hold.
struct EditorLayer::PreparedModelImport
{
    std::string                              source, outDir, label = "model", importer;
    uint64_t                                 hash = 0;   // the source's bytes (the SR record's)
    Resource::ModelImportOptions             options;    // what it ran with (the IM record's)
    std::vector<Resource::VrmExportedFile>   vrmFiles;
    std::vector<Resource::PmxExportedFile>   pmxFiles;
    Resource::SceneImporter::ImportResult    sceneRes;
    std::vector<uint8_t>                     clips;      // <stem>/<stem>.ianim, the source's animations
    std::vector<Resource::ImportFileOut>     outFiles;
    // What the source says of each Mesh's collision and shape keys (presets,
    // default weights); WriteModelImportFiles keys and merges them.
    std::vector<Resource::ImportedCollision> collision;
    std::vector<Resource::ImportedShapeKeys> shapeKeys;
};

std::shared_ptr<EditorLayer::PreparedModelImport> EditorLayer::PrepareModelImport(const ModelImportRequest& req,
                                                                                  ModelImportOutcome& out)
{
    const std::string& source = req.source;
    const std::string& outDir = req.outDir;
    auto fail = [&](const char* code, std::string message) -> std::shared_ptr<PreparedModelImport>
    {
        LOG_ERROR("EditorLayer: model import '%s' -> '%s': %s", source.c_str(), outDir.c_str(), message.c_str());
        out.code    = code;
        out.message = std::move(message);
        return nullptr;
    };

    // Runs on a worker: no Sys::Stage (main-thread only), nothing of the
    // editor's. ImportModel, the synchronous path, declares the stage.
    const std::vector<uint8_t> srcData = ReadFileFully(source);
    if (srcData.empty())
        return fail("read_failed", "could not read the source file");
    std::string ext = Sys::PathToUtf8(Sys::U8Path(source).extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto prep    = std::make_shared<PreparedModelImport>();
    prep->source = source;
    prep->outDir = outDir;
    prep->hash   = Resource::MeshIdentityHash(std::string_view(reinterpret_cast<const char*>(srcData.data()),
                                                               srcData.size()));
    // The project-relative folder the files land in (what the materials' texture
    // references are spelled against), and the model's name: a re-import keeps
    // its own whatever the source file is called now.
    Resource::ModelImportContext ctx;
    ctx.materialRoot = Sys::RelativizeAssetPath(outDir);
    for (char& c : ctx.materialRoot) if (c == '\\') c = '/';
    ctx.stem = req.stem;
    const std::string stem = !req.stem.empty() ? req.stem : Sys::PathToUtf8(Sys::U8Path(source).stem());
    // The options asked for; else a re-import runs with the ones the model was
    // imported with (its IM record); else the defaults.
    if (req.options)
        ctx.options = *req.options;
    else if (const std::string existing = Sys::PathToUtf8(Sys::U8Path(outDir) / Sys::U8Path(stem) /
                                                          Sys::U8Path(stem + ".iscn"));
             Sys::FileExists(existing) && Resource::ReadImportOptions(existing, ctx.options))
        LOG_INFO("EditorLayer: re-import of '%s' with the model's recorded options: %s", existing.c_str(),
                 Resource::FormatImportOptions(ctx.options).c_str());
    prep->options = ctx.options;
    if (ext == ".vrm")
    {
        // VRM: parse the extension JSON, then delegate the geometry to SceneImporter
        // (the expressions ride along as presets).
        prep->label    = "VRM";
        prep->importer = "vrm";
        Resource::VrmImportResult vrmRes = Resource::VrmImporter::Import(srcData.data(), srcData.size());
        prep->vrmFiles = Resource::VrmImporter::BuildBlobs(source, srcData, vrmRes, &prep->shapeKeys,
                                                           &prep->collision, &ctx);
        for (const auto& f : prep->vrmFiles) prep->outFiles.push_back({ f.relativePath, &f.blob });
    }
    else if (ext == ".pmx")
    {
        // PMX: the custom importer (no Assimp). The large result is built on
        // the heap directly (copy elision); make_unique would stage it on the stack.
        prep->label    = "PMX";
        prep->importer = "pmx";
        auto pmxRes = std::unique_ptr<Resource::PmxImportResult>(
            new Resource::PmxImportResult(Resource::PmxImporter::Import(srcData.data(), srcData.size())));
        if (!pmxRes->success)
            return fail("import_failed", "the PMX importer could not read it");
        // Of the options, morphs and textures apply to a PMX: its normals are the
        // model's own, and it has no normal maps to flip.
        if (!ctx.options.morphs) Resource::PmxImporter::DropMorphs(*pmxRes);
        if (ctx.options.normals == Resource::ModelImportOptions::Normals::Compute)
            LOG_INFO("EditorLayer: normals = compute does not apply to a PMX - its own normals are kept");
        prep->pmxFiles = Resource::PmxImporter::BuildBlobs(*pmxRes, stem, &prep->shapeKeys, &source,
                                                           ctx.options.textures ? &ctx.materialRoot : nullptr);
        for (const auto& f : prep->pmxFiles) prep->outFiles.push_back({ f.relativePath, &f.blob });
    }
    else
    {
        // Every other format: the Assimp-based SceneImporter.
        prep->importer = "assimp";
        prep->sceneRes = Resource::SceneImporter::Import(source, srcData, nullptr, HumanoidConvention::Unknown,
                                                         nullptr, &ctx);
        if (!prep->sceneRes.success)
            return fail("import_failed", "the importer (assimp) could not load it");
        for (const auto& f : prep->sceneRes.files) prep->outFiles.push_back({ f.relativePath, &f.blob });
        prep->collision = std::move(prep->sceneRes.collision);
        prep->shapeKeys = std::move(prep->sceneRes.shapeKeys);
    }
    if (prep->outFiles.empty())
        return fail("import_failed", "the " + prep->label + " importer produced no files");
    // assimp reads some formats leniently: a file that is not a model at all
    // (garbage bytes in an .obj) loads as an EMPTY scene, and the importer then
    // emits an .iscn with nothing in it. No geometry is no model - refused
    // before anything is written.
    if (std::none_of(prep->outFiles.begin(), prep->outFiles.end(), [](const Resource::ImportFileOut& f)
                     { return f.relativePath.ends_with(".meshlib") || f.relativePath.ends_with(".imsh"); }))
        return fail("import_failed", "the " + prep->label + " importer found no geometry in it - "
                                     "the source did not load as a model");
    // The source's animation clips beside the model (skeletal and shape-key
    // channels, named as the model names its targets), so a clip binds without
    // the editor's cook-on-bind, in Game.exe too (it has no importers). The same
    // importer, at the same 30 Hz, as File > Import > Animation.
    if (ctx.options.animations && prep->importer == "assimp" &&
        (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".dae"))
    {
        prep->clips = Resource::AnimationImporter().Import(source, srcData);
        if (!prep->clips.empty())
            prep->outFiles.push_back({ stem + "/" + stem + ".ianim", &prep->clips });
    }
    return prep;
}

EditorLayer::ModelImportOutcome EditorLayer::CommitModelImport(const PreparedModelImport& prep,
                                                              const ModelImportRequest& req)
{
    ModelImportOutcome out;
    out.dryRun = req.dryRun;
    auto fail = [&](const char* code, std::string message)
    {
        LOG_ERROR("EditorLayer: model import '%s' -> '%s': %s", prep.source.c_str(), prep.outDir.c_str(),
                  message.c_str());
        out.code    = code;
        out.message = std::move(message);
        return out;
    };

    // The SR record: where this came from, so Reimport can go back.
    out.source.path = Sys::RelativizeAssetPath(prep.source);
    for (char& c : out.source.path) if (c == '\\') c = '/';
    out.source.hash     = prep.hash;
    out.source.importer = prep.importer;

    // Non-destructive: an existing .imat is kept and the previous .iscn's
    // material bindings are carried over (Resource/ModelImportWrite.h). The
    // source's collision nodes (UCX_ ...) become the Meshes' asset collision, its
    // presets / default weights their src= shape-key records; Meshes renamed in
    // the source keep their keys, and a dry run only reports.
    Resource::ModelWriteOptions wo;
    wo.source          = &out.source;
    wo.renames         = req.renames.empty() ? nullptr : &req.renames;
    wo.geometryRenames = req.geometryRenames;
    wo.dryRun          = req.dryRun;
    wo.importOptions   = &prep.options;   // the IM record and the writer's options
    out.options        = prep.options;
    Resource::ModelImportStats st = Resource::WriteModelImportFiles(
        prep.outDir, prep.outFiles, prep.label.c_str(), prep.collision.empty() ? nullptr : &prep.collision,
        prep.shapeKeys.empty() ? nullptr : &prep.shapeKeys, &wo);
    out.written          = st.written;
    out.failed           = st.failed;
    out.keptMaterials    = st.keptMaterials;
    out.restoredBindings = st.restoredBindings;
    out.collisionMeshes  = st.collisionMeshes;
    out.collisionHulls   = st.collisionHulls;
    out.keptCollision    = st.keptCollision;
    out.orphanCollision  = st.orphanCollision;
    out.keptShapeKeys    = st.keptShapeKeys;
    out.orphanShapeKeys  = st.orphanShapeKeys;
    out.sourcePresets    = st.sourcePresets;
    out.sourceDefaults   = st.sourceDefaults;
    out.textures         = st.textures;
    out.madeCollision    = st.madeCollision;
    out.reusedMaterials  = st.reusedMaterials;
    out.identity         = std::move(st.identity);
    for (const Resource::ImportFileOut& f : prep.outFiles)
    {
        out.files.push_back(f.relativePath);
        if (out.iscn.empty() && f.relativePath.ends_with(".iscn"))
        {
            out.iscn = Sys::RelativizeAssetPath(Sys::PathToUtf8(Sys::U8Path(prep.outDir) / Sys::U8Path(f.relativePath)));
            for (char& c : out.iscn) if (c == '\\') c = '/';
        }
    }
    if (req.dryRun)
    {
        out.ok = true;
        LOG_INFO("EditorLayer: %s re-import of '%s' into '%s' (dry run, nothing written): Meshes %u kept (%zu "
                 "renamed), %zu added, %zu removed; sections %zu added, %zu removed", prep.label.c_str(),
                 prep.source.c_str(), prep.outDir.c_str(), out.identity.meshesKept, out.identity.renamedMeshes.size(),
                 out.identity.addedMeshes.size(), out.identity.removedMeshes.size(),
                 out.identity.addedSections.size(), out.identity.removedSections.size());
        return out;
    }
    m_needsRescan = true;
    if (st.failed)
        return fail("write_failed", std::to_string(st.failed) + " of the model's " +
                                    std::to_string(prep.outFiles.size()) + " files could not be written");
    // The model's identity stays with its path: a reimport keeps the guid its
    // references carry, a first import mints one.
    if (!out.iscn.empty())
        if (const ECS::Guid g = Resource::AssetGuidIndex::Get().EnsureGuid(out.iscn); g.IsValid())
            out.guid = g.ToHex();
    out.ok = true;
    LOG_INFO("EditorLayer: %s imported '%s' -> %d file(s) in '%s' (%d .imat kept, %d binding(s) carried over)",
             prep.label.c_str(), prep.source.c_str(), st.written, prep.outDir.c_str(), st.keptMaterials,
             st.restoredBindings);
    if (out.identity.previous)
        LOG_INFO("EditorLayer:   re-import: Meshes %u kept (%zu renamed), %zu added, %zu removed; sections %zu "
                 "added, %zu removed, %zu slot(s) renamed", out.identity.meshesKept,
                 out.identity.renamedMeshes.size(), out.identity.addedMeshes.size(),
                 out.identity.removedMeshes.size(), out.identity.addedSections.size(),
                 out.identity.removedSections.size(), out.identity.renamedSlots.size());
    return out;
}

EditorLayer::ModelImportOutcome EditorLayer::ImportModel(const ModelImportRequest& req)
{
    ModelImportOutcome out;
    std::shared_ptr<PreparedModelImport> prep;
    {
        Sys::Stage::Scope stage("importing", req.source);   // the main thread is busy: with this
        prep = PrepareModelImport(req, out);
    }
    if (!prep) return out;
    return CommitModelImport(*prep, req);
}

EditorLayer::ModelImportOutcome EditorLayer::ImportModelFile(const std::string& source, const std::string& outDir)
{
    ModelImportRequest req;
    req.source = source;
    req.outDir = outDir;
    return ImportModel(req);
}

// ==== Reimport ====
namespace
{
    bool EqualsIgnoreCaseAscii(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // File > Import > Model's formats.
    bool IsModelSourcePath(const std::string& path)
    {
        std::string ext = Sys::PathToUtf8(Sys::U8Path(path).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static constexpr const char* kExts[] = { ".obj", ".gltf", ".glb", ".pmx", ".vrm", ".fbx",
                                                 ".dae", ".3ds", ".ply", ".stl" };
        return std::any_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; });
    }
}

// ==== Files dropped from the OS ====
void EditorLayer::QueueDroppedFiles(std::vector<std::string> paths, int x, int y)
{
    m_droppedFiles.push_back({ std::move(paths), x, y });
}

void EditorLayer::ProcessDroppedFiles()
{
    if (m_droppedFiles.empty()) return;
    std::vector<DroppedFiles> drops = std::move(m_droppedFiles);
    m_droppedFiles.clear();
    for (const DroppedFiles& d : drops)
    {
        // A model tile the Content Browser drew where they landed (client
        // pixels == ImGui's coordinates: one viewport).
        std::string target;
        for (const ModelTileRect& r : m_modelTileRects)
            if (d.x >= r.x0 && d.x < r.x1 && d.y >= r.y0 && d.y < r.y1)
            {
                target = r.model;
                break;
            }
        const std::string what = HandleDroppedFiles(d.paths, target);
        LOG_INFO("EditorLayer: %zu file(s) dropped at (%d, %d): %s", d.paths.size(), d.x, d.y, what.c_str());
    }
}

std::string EditorLayer::HandleDroppedFiles(const std::vector<std::string>& paths, const std::string& targetModel)
{
    std::vector<std::string> models, images, skipped;
    const std::vector<const char*> imageExts = Resource::TextureImporter().GetSourceExtensions();
    for (const std::string& p : paths)
    {
        std::string ext = Sys::PathToUtf8(Sys::U8Path(p).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (IsModelSourcePath(p)) models.push_back(p);
        else if (std::any_of(imageExts.begin(), imageExts.end(), [&](const char* e) { return ext == e; }))
            images.push_back(p);
        else skipped.push_back(p);
    }
    namespace fs = std::filesystem;
    const std::string dir = !m_currentDir.empty() ? m_currentDir : !m_assetDir.empty() ? m_assetDir : "asset";
    std::string what;
    auto add = [&](const std::string& s) { what += (what.empty() ? "" : "; ") + s; };
    if (!models.empty() && !targetModel.empty())
    {
        // Onto a model: its Reimport From (the review asks first when needed).
        BeginModelReimport(targetModel, models.front());
        add("reimport '" + targetModel + "' from '" + models.front() + "'");
        models.erase(models.begin());
    }
    if (!models.empty())
    {
        // Else the Import Model dialog into the Content Browser's folder, started
        // from the first existing model's recorded options (as File > Import).
        Resource::ModelImportOptions start;
        for (const std::string& src : models)
        {
            const std::string stem = Sys::PathToUtf8(Sys::U8Path(src).stem());
            if (Resource::ReadImportOptions(
                    Sys::PathToUtf8(Sys::U8Path(dir) / Sys::U8Path(stem) / Sys::U8Path(stem + ".iscn")), start))
                break;
        }
        OpenModelImportDialog(models, dir, std::string(), start);
        add("the Import Model dialog: " + std::to_string(models.size()) + " model(s) into '" + dir + "'");
    }
    if (!images.empty())
    {
        // The texture import's review, as File > Import > Texture opens it.
        m_pendingTextureImports.clear();
        for (const std::string& src : images)
        {
            PendingTextureImport item{};
            item.sourcePath      = src;
            item.outputPath      = Sys::PathToUtf8(Sys::U8Path(dir) / (Sys::U8Path(src).stem() += ".itex"));
            item.suggestedFormat = Resource::TextureImporter::SuggestFormat(src);
            item.selectedFormat  = item.suggestedFormat;
            m_pendingTextureImports.push_back(std::move(item));
        }
        m_textureImportConfirmed = false;
        m_openTextureImportPopup = true;
        add("the texture review: " + std::to_string(images.size()) + " image(s) into '" + dir + "'");
    }
    for (const std::string& p : skipped)
        LOG_WARNING("EditorLayer: dropped '%s' - not a model or an image source; skipped", p.c_str());
    if (!skipped.empty()) add(std::to_string(skipped.size()) + " skipped");
    return what.empty() ? std::string("nothing to do") : what;
}

bool EditorLayer::ResolveModelReimport(const std::string& iscnPath, const std::string& sourceOverride,
                                       ModelImportRequest& req, std::string& code, std::string& message)
{
    namespace fs = std::filesystem;
    auto fail = [&](const char* c, std::string m)
    {
        code    = c;
        message = std::move(m);
        return false;
    };
    std::error_code ec;
    const fs::path  model = Sys::U8Path(iscnPath);
    std::string     ext   = Sys::PathToUtf8(model.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".iscn")
        return fail("bad_value", "'" + iscnPath + "' is not a model (.iscn)");
    if (!fs::is_regular_file(model, ec))
        return fail("load_failed", "no such model: " + iscnPath);
    // The importer's layout: <dir>/<stem>/<stem>.iscn. A model moved or renamed
    // out of it would be written somewhere else - and adopt nothing.
    const std::string stem   = Sys::PathToUtf8(model.stem());
    const fs::path    folder = model.parent_path();
    if (!EqualsIgnoreCaseAscii(Sys::PathToUtf8(folder.filename()), stem))
        return fail("bad_layout", "'" + iscnPath + "' is not where its import put it (<dir>/<stem>/<stem>.iscn) - "
                                  "import the source afresh");
    Resource::ModelSourceRecord rec;
    const bool  haveRecord = Resource::ReadModelSource(iscnPath, rec);
    std::string source     = sourceOverride;
    if (source.empty())
    {
        if (!haveRecord)
            return fail("no_source_record", "'" + iscnPath + "' records no source (imported before the SR record "
                                            "existed, or not by a model importer) - name one: Reimport From..., "
                                            "Editor.Reimport(model, {source = ...})");
        source = rec.path;
    }
    if (haveRecord && sourceOverride.empty() && rec.importer == "ue")
        return fail("unsupported_source", "'" + iscnPath + "' is an Unreal scene (its source is the export manifest '" +
                                          source + "') - the Content Browser's Reimport or Import > Unreal Scene "
                                          "cooks it again");
    if (!IsModelSourcePath(source))
        return fail("unsupported_source", "'" + source + "' - a model source is .obj .gltf .glb .pmx .vrm .fbx "
                                          ".dae .3ds .ply or .stl");
    // Where the importer's IO system finds it (a worktree's shared content root too).
    if (!Resource::AssetFS::Get().Exists(source))
        return fail("source_missing", "the source '" + source + "' is not there - name another: Reimport From..., "
                                      "Editor.Reimport(model, {source = ...})");
    req.source = source;
    req.outDir = Sys::PathToUtf8(folder.parent_path());
    req.stem   = stem;
    return true;
}

// What the running engine still holds of the meshes a model import (re)wrote.
// A loaded .meshlib is not re-read in place: MeshLibrary dedups by path, and a
// reimport may change the mesh count and the ids the placed parts name, so
// placed parts keep drawing what they loaded until the scene is opened again.
// The texture reimport swaps in place and says so (reload.jobId); this says
// the opposite, so "succeeded" is not read as "the view shows it".
static nlohmann::json ModelReloadState(World* world, Renderer* renderer, const std::string& dir,
                                       const std::vector<std::string>& files)
{
    using Resource::AssetRefs::NormalizeRefPath;
    std::unordered_set<std::string> libs;
    for (const std::string& f : files)   // as the importer named them, relative to dir
        if (f.size() > 8 && f.ends_with(".meshlib"))
            libs.insert(NormalizeRefPath(dir + "/" + f));
    int users = 0, loaded = 0;
    if (world)
        if (auto* pool = world->GetPool<MeshSourcePath>())
            for (size_t i = 0; i < pool->Data().size(); ++i)
                if (world->IsAlive(pool->Entities()[i]) && libs.count(NormalizeRefPath(pool->Data()[i].path)))
                    ++users;
    if (Resource::MeshLibrary* lib = renderer ? renderer->GetMeshLibrary() : nullptr)
        for (const Resource::Handle h : lib->GetLoadedHandles())
            if (libs.count(NormalizeRefPath(lib->GetSourcePath(h))))
                ++loaded;
    if (!users && !loaded)
        return { { "state", "not_loaded" } };
    return { { "state", "scene_reload_required" }, { "users", users }, { "loadedLibraries", loaded },
             { "why", "the running engine keeps drawing the meshes it loaded from these files - a loaded .meshlib "
                      "is not re-read in place (a reimport may change its mesh ids): open the scene again "
                      "(Editor.OpenScene) or restart to see the reimported meshes" } };
}

// ==== The collision bake's "Use baked as collision" ====
namespace
{
    // One entity's Collider + RigidBody before / after (nullopt = none): a part
    // given a Mesh collider of its own. The body is rebuilt either way.
    class ColliderSetAction : public Editor::Undo::IEditAction
    {
    public:
        ColliderSetAction(EntityHandle h,
                          std::optional<ColliderComponent> colBefore, std::optional<RigidBodyComponent> rbBefore,
                          std::optional<ColliderComponent> colAfter,  std::optional<RigidBodyComponent> rbAfter)
            : m_h(h), m_colBefore(std::move(colBefore)), m_rbBefore(std::move(rbBefore))
            , m_colAfter(std::move(colAfter)), m_rbAfter(std::move(rbAfter)) {}
        void        Apply(Editor::Undo::EditContext& c) override  { Set(c, m_colAfter, m_rbAfter); }
        void        Revert(Editor::Undo::EditContext& c) override { Set(c, m_colBefore, m_rbBefore); }
        const char* Label() const override { return "Use Baked Collision"; }

    private:
        void Set(Editor::Undo::EditContext& c, const std::optional<ColliderComponent>& col,
                 const std::optional<RigidBodyComponent>& rb)
        {
            if (!c.world || !c.world->IsHandleValid(m_h)) return;
            World& w = *c.world;
            const Entity e = m_h.entity;
            if (col)
            {
                if (ColliderComponent* x = w.GetComponent<ColliderComponent>(e)) { *x = *col; x->MarkDirty(); }
                else w.AddComponent<ColliderComponent>(e, *col);
            }
            else if (w.HasComponent<ColliderComponent>(e)) w.RemoveComponent<ColliderComponent>(e);
            if (rb)
            {
                RigidBodyComponent v = *rb;
                v.bodyId = kInvalidPhysicsBodyId;   // a new body; the reaper takes the old one
                if (RigidBodyComponent* x = w.GetComponent<RigidBodyComponent>(e)) *x = v;
                else w.AddComponent<RigidBodyComponent>(e, v);
            }
            else if (w.HasComponent<RigidBodyComponent>(e)) w.RemoveComponent<RigidBodyComponent>(e);
        }
        EntityHandle                        m_h;
        std::optional<ColliderComponent>   m_colBefore;
        std::optional<RigidBodyComponent>  m_rbBefore;
        std::optional<ColliderComponent>   m_colAfter;
        std::optional<RigidBodyComponent>  m_rbAfter;
    };

    // A model file's whole text before / after an edit of its asset collision.
    // Either way round the file is written and `changed` runs: the physics shape
    // caches forget it, Static Mesh owners re-derive, placed instances follow.
    class ModelCollisionEditAction : public Editor::Undo::IEditAction
    {
    public:
        ModelCollisionEditAction(std::string model, std::string before, std::string after,
                                 std::function<void(const std::string&)> changed)
            : m_model(std::move(model)), m_before(std::move(before)), m_after(std::move(after))
            , m_changed(std::move(changed)) {}
        void        Apply(Editor::Undo::EditContext&) override  { Write(m_after); }
        void        Revert(Editor::Undo::EditContext&) override { Write(m_before); }
        const char* Label() const override { return "Model Collision"; }
        size_t      HeapBytes() const override { return m_before.size() + m_after.size(); }

    private:
        void Write(const std::string& text)
        {
            std::string err;
            if (!SceneInstanceLoader::WriteModelText(m_model, text, &err))
            {
                LOG_ERROR("Model collision undo/redo: %s", err.c_str());
                return;
            }
            if (m_changed) m_changed(m_model);
        }
        std::string m_model, m_before, m_after;
        std::function<void(const std::string&)> m_changed;
    };
}

uint32_t EditorLayer::ModelCollisionChanged(const std::string& model)
{
    if (MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr) reg->InvalidateModel(model);
    if (m_physicsSys && m_world) m_physicsSys->InvalidateMeshShapeCache(*m_world);
    return m_world ? SceneInstanceLoader::RefreshAssetCollision(*m_world, model) : 0u;
}

void EditorLayer::ProcessCollisionBake()
{
    if (!m_collisionBakeScripted) return;
    if (m_collisionBake.Tick())   // still baking (the popup may tick it too - harmless)
    {
        Harness::Jobs::Progress(m_collisionBakeJobId, m_collisionBake.Processed(), m_collisionBake.Total());
        return;
    }
    const std::uint64_t job = m_collisionBakeJobId;
    m_collisionBakeScripted = false;
    m_collisionBakeJobId    = 0;
    if (!m_collisionBake.IsComplete() || m_collisionBake.WriteFailed())
    {
        Harness::Jobs::Fail(job, "write_failed", "the bake did not write '" + m_collisionBake.OutputPath() +
                                                 "' (see the log)", {});
        return;
    }
    uint32_t ok = 0, failed = 0, skipped = 0;
    for (const auto& r : m_collisionBake.Results())
        (r.skipped ? skipped : r.success ? ok : failed) += 1;
    const nlohmann::json out = { { "output", m_collisionBake.OutputPath() }, { "baked", ok }, { "failed", failed },
                                 { "skipped", skipped }, { "trisBefore", m_collisionBake.OrigTriTotal() },
                                 { "trisAfter", m_collisionBake.BakedTriTotal() } };
    Harness::Jobs::Succeed(job, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

EditorLayer::BakedCollisionReport EditorLayer::UseBakedAsCollision()
{
    namespace fs = std::filesystem;
    BakedCollisionReport rep;
    if (!m_world) { rep.error = "no world"; return rep; }
    if (GetPlayState() != ViewportPlayState::Stopped)
    {
        rep.error = "the editor is playing - Stop first (a body swapped mid-play would outlive its entity)";
        return rep;
    }
    if (BakePreviewSwapActive() || m_pendingBakePreviewOp != PendingBakePreviewOp::None)
    {
        rep.error = "the bake is previewed in the world - Restore originals first";
        return rep;
    }
    if (!m_collisionBake.IsComplete() || m_collisionBake.WriteFailed())
    {
        rep.error = "no finished bake - Bake first";
        return rep;
    }
    const std::string libPath = m_collisionBake.OutputPath();
    const auto        bakedKeys = Resource::MeshLibKeyTableAt(libPath);   // the bake's own identity

    // (source library, entry) -> baked entry
    std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> bakedMap;
    for (const auto& r : m_collisionBake.Results())
        if (r.success) bakedMap[Resource::AssetRefs::NormalizeRefPath(r.sourcePath)][r.sourceMeshId] = r.bakedMeshId;

    // Every entity the bake covered, by the model it is a part of (the SceneRef
    // root above it; "" = none).
    struct Hit { Entity e; uint32_t baked; std::string lib; uint32_t entry; };
    std::map<std::string, std::vector<Hit>> byModel;
    m_world->ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
    {
        const MeshSourcePath* sp = m_world->GetComponent<MeshSourcePath>(e);
        if (!sp) return;
        const auto pit = bakedMap.find(Resource::AssetRefs::NormalizeRefPath(sp->path));
        if (pit == bakedMap.end()) return;
        const auto mit = pit->second.find(ref.meshId);
        if (mit == pit->second.end()) return;
        std::string model;
        // A Static Mesh's derived section: the owner's model (the section itself is
        // transient - rebuilt from the owner, never saved).
        if (const MeshSectionOf* ms = m_world->GetComponent<MeshSectionOf>(e))
        {
            if (const StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(ms->owner)) model = sm->model;
        }
        else
            for (Entity p = e; p != NullEntity;)
            {
                if (const SceneSourcePath* s = m_world->GetComponent<SceneSourcePath>(p); s && !s->path.empty())
                {
                    model = s->path;
                    break;
                }
                const Parent* par = m_world->GetComponent<Parent>(p);
                p = par ? par->entity : NullEntity;
            }
        byModel[model].push_back({ e, mit->second, sp->path, ref.meshId });
    });
    if (byModel.empty()) { rep.error = "the bake covers nothing in this world"; return rep; }

    // A model's records changed (here, or by its undo / redo).
    auto modelChanged = [this](const std::string& model) -> uint32_t { return ModelCollisionChanged(model); };

    std::vector<DX12Physics::PhysicsSystem::MeshShapeRequest> prewarm;
    SceneInstanceLoader::LibraryIdentityCache libs;
    m_editHistory.BeginMacro("Use Baked as Collision");
    for (auto& [model, hits] : byModel)
    {
        // ---- The model's asset collision, when the editor may write it ----
        std::string why;
        if (!model.empty())
        {
            if (Sys::ThroughReparsePoint(model))
                why = "the model is another checkout's (reached through a junction)";
            else if (SceneInstanceLoader::ModelHasCRecords(model))
                why = "the model still has pre-M6 C records - Game.exe -assettool=collision_migrate first";
            else if (!bakedKeys)
                why = "the bake's library has no keys to name";
        }
        if (!model.empty() && why.empty())
        {
            std::error_code ec;
            const fs::path modelDir = fs::absolute(Sys::U8Path(model), ec).parent_path();
            std::string libRel = Sys::PathToUtf8(fs::absolute(Sys::U8Path(libPath), ec).lexically_relative(modelDir));
            std::replace(libRel.begin(), libRel.end(), '\\', '/');
            std::vector<Resource::MeshBodySetup> setups;   // per Mesh, first-seen order
            for (const Hit& h : hits)
            {
                const Resource::MeshIdentity* id = libs.Get(h.lib);
                if (!id || h.entry >= id->sections.size()) continue;
                const uint32_t o = id->sections[h.entry].ownerMesh;
                const uint64_t bk = bakedKeys->KeyOf(h.baked);
                if (o >= id->meshes.size() || bk == 0) continue;
                const uint64_t mk = id->meshes[o].key;
                auto it = std::find_if(setups.begin(), setups.end(), [&](const auto& s) { return s.meshKey == mk; });
                if (it == setups.end())
                {
                    setups.emplace_back();
                    it = setups.end() - 1;
                    it->meshKey = mk;
                }
                if (std::none_of(it->prims.begin(), it->prims.end(), [&](const auto& p) { return p.sectionKey == bk; }))
                {
                    Resource::CollisionPrim p;
                    p.type       = Resource::CollisionPrim::Type::Mesh;   // a trimesh from the bake: a body prim
                    p.lib        = libRel;
                    p.sectionKey = bk;
                    it->prims.push_back(std::move(p));
                    prewarm.push_back({ libPath, h.baked, false });
                }
            }
            std::string before, after, err;
            if (setups.empty()) why = "no baked section of it has a key";
            else if (!SceneInstanceLoader::ReplaceModelCollision(model, setups, before, after, &err)) why = err;
            else
            {
                rep.models.push_back(model);
                rep.modelMeshes += static_cast<uint32_t>(setups.size());
                rep.refreshed += modelChanged(model);
                m_editHistory.Push(std::make_unique<ModelCollisionEditAction>(
                    model, std::move(before), std::move(after),
                    [modelChanged](const std::string& m) { modelChanged(m); }));
                continue;
            }
        }
        if (!model.empty()) rep.perEntityModels.emplace_back(model, why);

        // ---- Otherwise each part gets a Mesh collider of its own ----
        for (const Hit& h : hits)
        {
            // A derived section is rebuilt from its owner: a collider on it would
            // not survive - only its model's asset collision reaches it.
            if (m_world->HasComponent<MeshSectionOf>(h.e)) { ++rep.skippedSections; continue; }
            std::optional<ColliderComponent>  colBefore;
            std::optional<RigidBodyComponent> rbBefore;
            if (const ColliderComponent* c = m_world->GetComponent<ColliderComponent>(h.e)) colBefore = *c;
            if (const RigidBodyComponent* r = m_world->GetComponent<RigidBodyComponent>(h.e)) rbBefore = *r;
            ColliderComponent col{};
            col.shape         = ColliderComponent::Shape::Mesh;
            col.meshLibPath   = libPath;
            col.meshLibMeshId = h.baked;
            RigidBodyComponent rb = rbBefore ? *rbBefore : RigidBodyComponent{};
            rb.motion = RigidBodyComponent::Motion::Static;   // a trimesh is static-only
            auto action = std::make_unique<ColliderSetAction>(m_world->MakeHandle(h.e), colBefore, rbBefore,
                                                              col, rb);
            m_editHistory.Execute(std::move(action));
            ++rep.perEntity;
            if (colBefore) ++rep.replaced;
            prewarm.push_back({ libPath, h.baked, false });
        }
    }
    m_editHistory.EndMacro();
    // The model writes are saved already; the per-entity colliders are scene edits, in the macro.
    if (rep.perEntity) SyncSceneDirtyToHistory();
    if (m_physicsSys)
    {
        // A re-bake rewrote the library in place: every cached shape of it is stale.
        m_physicsSys->InvalidateMeshShapeCache(*m_world);
        m_physicsSys->PrewarmMeshShapes(prewarm);
    }
    m_lastBakedCollision = rep;
    LOG_INFO("CollisionMeshBaker: use baked as collision - %zu model(s) given asset collision (%u Mesh(es), %u part "
             "collider change(s)), %u entit(ies) a Mesh collider of their own (%u replaced one)",
             rep.models.size(), rep.modelMeshes, rep.refreshed, rep.perEntity, rep.replaced);
    for (const auto& [m, w] : rep.perEntityModels)
        LOG_WARNING("CollisionMeshBaker: '%s' not written (%s) - its parts got colliders of their own", m.c_str(),
                    w.c_str());
    return rep;
}

// A re-import's report: what happened to the model's Meshes and sections (in
// a dry run: would happen). Keys as 16 hex digits.
static nlohmann::json ModelIdentityJson(const Resource::MeshIdentityAdoption& a)
{
    using Resource::MeshKeyToHex;
    using Renamed = Resource::MeshIdentityAdoption::MeshRenamed;
    auto meshes = [](const std::vector<Resource::MeshIdentityAdoption::MeshItem>& v)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : v) arr.push_back({ { "name", m.name }, { "key", MeshKeyToHex(m.key) } });
        return arr;
    };
    auto sections = [](const std::vector<Resource::MeshIdentityAdoption::SectionItem>& v)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : v) arr.push_back({ { "mesh", s.mesh }, { "slot", s.slot }, { "key", MeshKeyToHex(s.key) } });
        return arr;
    };
    nlohmann::json renamed = nlohmann::json::array();
    for (const Renamed& r : a.renamedMeshes)
        renamed.push_back({ { "from", r.from }, { "to", r.to }, { "key", MeshKeyToHex(r.key) },
                            { "how", r.how == Renamed::How::Explicit ? "explicit"
                                   : r.how == Renamed::How::Geometry ? "geometry" : "name" } });
    nlohmann::json slots = nlohmann::json::array();
    for (const auto& r : a.renamedSlots)
        slots.push_back({ { "mesh", r.mesh }, { "from", r.from }, { "to", r.to }, { "key", MeshKeyToHex(r.key) } });
    nlohmann::json d = nlohmann::json::object();
    d["previous"]          = a.previous;
    d["needsReview"]       = a.NeedsReview();
    d["meshes"]            = { { "kept", a.meshesKept }, { "added", meshes(a.addedMeshes) },
                               { "removed", meshes(a.removedMeshes) }, { "renamed", renamed } };
    d["sections"]          = { { "kept", a.sectionsKept }, { "added", sections(a.addedSections) },
                               { "removed", sections(a.removedSections) }, { "renamed", slots } };
    d["unresolvedRenames"] = a.unresolvedRenames;
    return d;
}

// The import options as the IM record names them.
static nlohmann::json ModelOptionsJson(const Resource::ModelImportOptions& opt)
{
    nlohmann::json o = nlohmann::json::object();
    std::string line = Resource::FormatImportOptions(opt);   // "IM k=v k=v\n"
    for (size_t p = 3; p < line.size();)
    {
        size_t q = line.find_first_of(" \n", p);
        if (q == std::string::npos) q = line.size();
        const std::string tok = line.substr(p, q - p);
        p = q + 1;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (k == "smooth")                                               o[k] = std::stod(v);
        else if (k == "morphs" || k == "textures" || k == "flipGreen" ||
                 k == "animations" || k == "meshlets" || k == "lod")     o[k] = v == "1";
        else                                                             o[k] = v;
    }
    return o;
}

void EditorLayer::FinishModelImportJob(std::uint64_t jobId, const ModelImportOutcome& m, const std::string& source,
                                       const std::string& dir, std::vector<std::string> failLog)
{
    if (!jobId) return;
    nlohmann::json o = nlohmann::json::object();
    o["source"] = source;
    o["dir"]    = dir;
    o["iscn"]   = m.iscn;
    o["files"]  = m.files;
    o["stats"]  = { { "written", m.written }, { "failed", m.failed },
                    { "keptMaterials", m.keptMaterials }, { "restoredBindings", m.restoredBindings },
                    { "collisionMeshes", m.collisionMeshes }, { "collisionHulls", m.collisionHulls },
                    { "keptCollision", m.keptCollision }, { "orphanCollision", m.orphanCollision },
                    { "keptShapeKeys", m.keptShapeKeys }, { "orphanShapeKeys", m.orphanShapeKeys },
                    { "sourcePresets", m.sourcePresets }, { "sourceDefaults", m.sourceDefaults },
                    { "textures", m.textures }, { "madeCollision", m.madeCollision },
                    { "reusedMaterials", m.reusedMaterials } };
    o["options"] = ModelOptionsJson(m.options);
    if (!m.source.path.empty())
        o["sourceRecord"] = { { "path", m.source.path }, { "hash", Resource::MeshKeyToHex(m.source.hash) },
                              { "importer", m.source.importer } };
    o["dryRun"] = m.dryRun;
    o["diff"]   = ModelIdentityJson(m.identity);
    if (!m.ok)
    {
        Harness::Jobs::Fail(jobId, m.code, m.message, std::move(failLog), o.dump());
        return;
    }
    if (!m.dryRun)
    {
        o["guid"]   = m.guid;
        o["reload"] = ModelReloadState(m_world, m_renderer, dir, m.files);
    }
    Harness::Jobs::Succeed(jobId, o.dump());
}

// ==== The Content Browser's Reimport, and its review ====
void EditorLayer::BeginModelReimport(const std::string& iscnPath, const std::string& sourceOverride,
                                     std::uint64_t jobId, std::uint32_t requestId, const ModelImportRequest* options)
{
    ModelImportOutcome none;
    auto refuse = [&](const std::string& code, const std::string& message)
    {
        LOG_ERROR("EditorLayer: Reimport '%s': %s", iscnPath.c_str(), message.c_str());
        none.code    = code;
        none.message = message;
        FinishModelImportJob(jobId, none, sourceOverride, std::string(), { message });
    };
    if (ReviewPending())
        return refuse("busy", "another re-import is waiting for its review");

    ModelImportRequest req;
    if (options) req = *options;
    std::string code, message;
    if (!ResolveModelReimport(iscnPath, sourceOverride, req, code, message))
        return refuse(code, message);
    // The prepare runs on a worker; ContinueModelReimport takes it from there.
    StartModelImport(req, jobId, requestId, iscnPath);
}

bool EditorLayer::ReviewPending() const
{
    if (m_reimportReview) return true;
    return std::any_of(m_modelImportTasks.begin(), m_modelImportTasks.end(),
                       [](const std::shared_ptr<ModelImportTask>& t) { return !t->reimport.empty(); });
}

// The review flow's main-thread half: a dry run first. When the report says
// someone should look (a Mesh or a section gone, a geometry guess) the review
// popup holds the prepared import; else it is written at once.
void EditorLayer::ContinueModelReimport(ModelImportTask& t)
{
    const ModelImportRequest& req = t.request;
    ModelImportRequest dry = req;
    dry.dryRun = true;
    ModelImportOutcome report = CommitModelImport(*t.prepared, dry);
    if (report.ok && !report.identity.NeedsReview())
    {
        // Nothing lost and nothing guessed: no question to ask.
        const ModelImportOutcome m = CommitModelImport(*t.prepared, req);
        FinishModelImportJob(t.jobId, m, req.source, req.outDir, m.ok ? std::vector<std::string>{}
                                                                       : std::vector<std::string>{ m.message });
        return;
    }
    if (!report.ok)
    {
        FinishModelImportJob(t.jobId, report, req.source, req.outDir, { report.message });
        return;
    }
    auto r        = std::make_unique<ModelReimportReview>();
    r->prepared   = t.prepared;
    r->request    = req;
    r->model      = t.reimport;
    r->keepRename.assign(report.identity.renamedMeshes.size(), 1);
    r->report     = std::move(report);
    r->jobId      = t.jobId;
    r->requestId  = t.requestId;
    m_reimportReview     = std::move(r);
    m_openReimportReview = true;
    LOG_INFO("EditorLayer: Reimport '%s': %zu Mesh(es) removed, %zu section(s) removed, %zu rename(s) - waiting "
             "for the review", t.reimport.c_str(), m_reimportReview->report.identity.removedMeshes.size(),
             m_reimportReview->report.identity.removedSections.size(),
             m_reimportReview->report.identity.renamedMeshes.size());
}

// ==== Model imports on a worker ====
bool EditorLayer::StartModelImport(const ModelImportRequest& req, std::uint64_t jobId, std::uint32_t requestId,
                                   const std::string& reimport)
{
    // The model it writes, <outDir>/<stem>/<stem>.iscn: one import of it at a time.
    auto modelOf = [](const ModelImportRequest& r)
    {
        const std::string stem = !r.stem.empty() ? r.stem : Sys::PathToUtf8(Sys::U8Path(r.source).stem());
        return Resource::AssetRefs::NormalizeRefPath(Sys::RelativizeAssetPath(
            Sys::PathToUtf8(Sys::U8Path(r.outDir) / Sys::U8Path(stem) / Sys::U8Path(stem + ".iscn"))));
    };
    const std::string model = modelOf(req);
    for (const std::shared_ptr<ModelImportTask>& t : m_modelImportTasks)
        if (modelOf(t->request) == model)
        {
            const std::string why = "'" + model + "' is being imported already";
            LOG_ERROR("EditorLayer: model import '%s': %s", req.source.c_str(), why.c_str());
            ModelImportOutcome none;
            none.code    = "busy";
            none.message = why;
            FinishModelImportJob(jobId, none, req.source, req.outDir, { why });
            return false;
        }
    auto task       = std::make_shared<ModelImportTask>();
    task->request   = req;
    task->reimport  = reimport;
    task->jobId     = jobId;
    task->requestId = requestId;
    task->startedAt = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    task->label     = Sys::PathToUtf8(Sys::U8Path(req.source).filename());
    m_modelImportTasks.push_back(task);
    LOG_INFO("EditorLayer: importing '%s' into '%s' on a worker%s", req.source.c_str(), req.outDir.c_str(),
             reimport.empty() ? "" : " (a re-import: its review follows)");
    TaskSystem::Get().Push([task]()
    {
        // Nothing may escape: the frame loop waits for `finished`.
        try
        {
            task->prepared = PrepareModelImport(task->request, task->failure);
        }
        catch (const std::exception& e)
        {
            task->prepared.reset();
            task->failure.code    = "import_failed";
            task->failure.message = std::string("the importer threw: ") + e.what();
        }
        catch (...)
        {
            task->prepared.reset();
            task->failure.code    = "import_failed";
            task->failure.message = "the importer threw";
        }
        task->finished.store(true, std::memory_order_release);
    }, TaskSystem::TaskPriority::Low);
    return true;
}

// Every frame, on the main thread: the finished prepares, in the order they
// started - committed (the writes, the registry, the jobs), a re-import's
// through its review.
void EditorLayer::ProcessModelImportTasks()
{
    for (size_t i = 0; i < m_modelImportTasks.size();)
    {
        const std::shared_ptr<ModelImportTask> t = m_modelImportTasks[i];
        if (!t->finished.load(std::memory_order_acquire))
        {
            ++i;
            continue;
        }
        m_modelImportTasks.erase(m_modelImportTasks.begin() + static_cast<std::ptrdiff_t>(i));
        if (!t->prepared)
        {
            FinishModelImportJob(t->jobId, t->failure, t->request.source, t->request.outDir,
                                 { t->failure.message });
            continue;
        }
        if (!t->reimport.empty())
        {
            ContinueModelReimport(*t);
            continue;
        }
        std::vector<CapturedLogLine> captured;
        ModelImportOutcome m;
        {
            Logger::ScopedContext ctx(LogSource::Control, t->requestId, &captured, 32);
            m = CommitModelImport(*t->prepared, t->request);
        }
        std::vector<std::string> log;
        for (const CapturedLogLine& l : captured)
            if (l.level == LogLevel::Error || l.level == LogLevel::Warning) log.push_back(l.message);
        FinishModelImportJob(t->jobId, m, t->request.source, t->request.outDir, std::move(log));
    }
}

// What is importing: a small overlay at the bottom right, one row per import.
void EditorLayer::RenderModelImportProgress()
{
    if (m_modelImportTasks.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16.f, vp->WorkPos.y + vp->WorkSize.y - 16.f),
                            ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##model_import_progress", nullptr, kFlags))
    {
        const double now = ImGui::GetTime();
        for (const std::shared_ptr<ModelImportTask>& t : m_modelImportTasks)
        {
            const bool done = t->finished.load(std::memory_order_acquire);
            ImGui::Text("%s %s   %.1f s", done ? "Writing" : "Importing", t->label.c_str(), now - t->startedAt);
            ImGui::ProgressBar(done ? 1.f : -1.f * static_cast<float>(now), ImVec2(280.f, 0.f), "");
        }
    }
    ImGui::End();
}

void EditorLayer::AnswerModelReimportReview(bool accept)
{
    if (!m_reimportReview) return;
    const std::unique_ptr<ModelReimportReview> r = std::move(m_reimportReview);
    if (!accept)
    {
        LOG_INFO("EditorLayer: Reimport '%s' cancelled - nothing written", r->model.c_str());
        ModelImportOutcome m = r->report;
        m.ok      = false;
        m.code    = "cancelled";
        m.message = "the review was cancelled - nothing written";
        FinishModelImportJob(r->jobId, m, r->request.source, r->request.outDir, {});
        return;
    }
    // The geometry's guesses still ticked become the author's word; an unticked
    // one is a removed Mesh and an added one. Name matches redo themselves.
    ModelImportRequest req = r->request;
    req.geometryRenames = false;
    const auto& renamed = r->report.identity.renamedMeshes;
    for (size_t i = 0; i < renamed.size(); ++i)
        if (renamed[i].how == Resource::MeshIdentityAdoption::MeshRenamed::How::Geometry &&
            i < r->keepRename.size() && r->keepRename[i])
            req.renames.push_back({ Resource::MeshKeyToHex(renamed[i].key), renamed[i].to });
    std::vector<CapturedLogLine> captured;
    ModelImportOutcome m;
    {
        Logger::ScopedContext ctx(LogSource::Control, r->requestId, &captured, 32);
        m = CommitModelImport(*r->prepared, req);
    }
    std::vector<std::string> log;
    for (const CapturedLogLine& l : captured)
        if (l.level == LogLevel::Error || l.level == LogLevel::Warning) log.push_back(l.message);
    FinishModelImportJob(r->jobId, m, req.source, req.outDir, std::move(log));
}

void EditorLayer::RenderModelReimportReview()
{
    constexpr const char* kPopupName = "Reimport Model";
    if (m_openReimportReview)
    {
        ImGui::OpenPopup(kPopupName);
        m_openReimportReview = false;
    }
    ImGui::SetNextWindowSize(ImVec2(620.f, 460.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPopupName, nullptr))
        return;
    if (!m_reimportReview)   // answered elsewhere (Editor.ReimportReview)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ModelReimportReview&                  r = *m_reimportReview;
    const Resource::MeshIdentityAdoption& d = r.report.identity;
    using Renamed = Resource::MeshIdentityAdoption::MeshRenamed;

    ImGui::TextUnformatted(r.model.c_str());
    ImGui::TextDisabled("from %s", r.request.source.c_str());
    ImGui::Separator();
    ImGui::Text("Meshes: %u kept, %zu added, %zu removed", d.meshesKept, d.addedMeshes.size(), d.removedMeshes.size());

    ImGui::BeginChild("##reimport_diff", ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 1.6f), true);
    bool anyGuess = false;
    for (const Renamed& m : d.renamedMeshes) anyGuess |= m.how == Renamed::How::Geometry;
    if (anyGuess)
    {
        ImGui::TextUnformatted("Renamed in the source? (the geometry agrees)");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Ticked: the Mesh keeps its identity - scenes, collision and shape keys follow it.");
        ImGui::PopTextWrapPos();
        for (size_t i = 0; i < d.renamedMeshes.size(); ++i)
        {
            if (d.renamedMeshes[i].how != Renamed::How::Geometry) continue;
            bool keep = i < r.keepRename.size() && r.keepRename[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Checkbox("##keep", &keep) && i < r.keepRename.size()) r.keepRename[i] = keep ? 1 : 0;
            ImGui::SameLine();
            ImGui::Text("%s  ->  %s", d.renamedMeshes[i].from.c_str(), d.renamedMeshes[i].to.c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    if (!d.removedMeshes.empty())
    {
        ImGui::TextWrapped("Removed - what refers to them stops resolving; their authored collision and "
                           "shape keys are kept as orphans:");
        for (const auto& m : d.removedMeshes) ImGui::BulletText("%s", m.name.c_str());
        ImGui::Spacing();
    }
    if (!d.removedSections.empty())
    {
        ImGui::TextUnformatted("Material slots removed:");
        for (const auto& s : d.removedSections) ImGui::BulletText("%s / %s", s.mesh.c_str(), s.slot.c_str());
        ImGui::Spacing();
    }
    if (!d.addedMeshes.empty())
    {
        ImGui::TextUnformatted("Added:");
        for (const auto& m : d.addedMeshes) ImGui::BulletText("%s", m.name.c_str());
        ImGui::Spacing();
    }
    if (!d.addedSections.empty())
    {
        ImGui::TextUnformatted("Material slots added:");
        for (const auto& s : d.addedSections) ImGui::BulletText("%s / %s", s.mesh.c_str(), s.slot.c_str());
        ImGui::Spacing();
    }
    for (const Renamed& m : d.renamedMeshes)
        if (m.how != Renamed::How::Geometry)
            ImGui::TextDisabled("Renamed: %s -> %s (%s)", m.from.c_str(), m.to.c_str(),
                                m.how == Renamed::How::Explicit ? "as asked" : "its name matched");
    for (const auto& s : d.renamedSlots)
        ImGui::TextDisabled("Slot renamed: %s / %s -> %s", s.mesh.c_str(), s.from.c_str(), s.to.c_str());
    ImGui::EndChild();

    if (ImGui::Button("Reimport", ImVec2(120.f, 0.f)))
    {
        AnswerModelReimportReview(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.f, 0.f)))
    {
        AnswerModelReimportReview(false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ==== The "Import Model" options dialog ====
void EditorLayer::OpenModelImportDialog(std::vector<std::string> sources, const std::string& outDir,
                                        const std::string& reimport, const Resource::ModelImportOptions& options,
                                        std::uint64_t jobId, std::uint32_t requestId)
{
    if (m_importDialog || m_reimportReview)
    {
        const std::string why = "another import is waiting for its dialog";
        LOG_ERROR("EditorLayer: Import Model: %s", why.c_str());
        ModelImportOutcome none;
        none.code    = "busy";
        none.message = why;
        FinishModelImportJob(jobId, none, sources.empty() ? reimport : sources[0], outDir, { why });
        return;
    }
    auto d       = std::make_unique<ModelImportDialog>();
    d->sources   = std::move(sources);
    d->outDir    = outDir;
    d->reimport  = reimport;
    d->options   = options;
    d->jobId     = jobId;
    d->requestId = requestId;
    m_importDialog     = std::move(d);
    m_openImportDialog = true;
}

void EditorLayer::AnswerModelImportDialog(bool accept)
{
    if (!m_importDialog) return;
    const std::unique_ptr<ModelImportDialog> d = std::move(m_importDialog);
    if (!accept)
    {
        LOG_INFO("EditorLayer: Import Model cancelled - nothing written");
        ModelImportOutcome none;
        none.code    = "cancelled";
        none.message = "the import dialog was cancelled - nothing written";
        FinishModelImportJob(d->jobId, none, d->sources.empty() ? d->reimport : d->sources[0], d->outDir, {});
        return;
    }
    ModelImportRequest opts;
    opts.options = d->options;
    if (!d->reimport.empty())
    {
        BeginModelReimport(d->reimport, std::string(), d->jobId, d->requestId, &opts);
        return;
    }
    namespace fs = std::filesystem;
    for (size_t i = 0; i < d->sources.size(); ++i)
    {
        const std::string& src     = d->sources[i];
        const std::uint64_t job    = i == 0 ? d->jobId : 0;   // a job is one import: the first
        const std::string  stem    = Sys::PathToUtf8(Sys::U8Path(src).stem());
        const fs::path     existing = Sys::U8Path(d->outDir) / Sys::U8Path(stem) / Sys::U8Path(stem + ".iscn");
        std::error_code    ec;
        if (fs::is_regular_file(existing, ec))
        {
            // Over a model the folder has: the reimport flow (the review asks first).
            BeginModelReimport(Sys::PathToUtf8(existing), src, job, d->requestId, &opts);
            continue;
        }
        ModelImportRequest req = opts;
        req.source = src;
        req.outDir = d->outDir;
        StartModelImport(req, job, d->requestId);   // on a worker
    }
}

void EditorLayer::RenderModelImportDialog()
{
    constexpr const char* kPopupName = "Import Model";
    if (m_openImportDialog)
    {
        ImGui::OpenPopup(kPopupName);
        m_openImportDialog = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (!m_importDialog)   // answered elsewhere (Editor.ImportDialog)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ModelImportDialog&            d = *m_importDialog;
    Resource::ModelImportOptions& o = d.options;
    using Opt = Resource::ModelImportOptions;

    if (!d.reimport.empty())
        ImGui::Text("Reimport %s", d.reimport.c_str());
    else
    {
        ImGui::Text("%zu model(s) into %s", d.sources.size(), d.outDir.c_str());
        for (size_t i = 0; i < d.sources.size() && i < 6; ++i) ImGui::BulletText("%s", d.sources[i].c_str());
        if (d.sources.size() > 6) ImGui::TextDisabled("... and %zu more", d.sources.size() - 6);
    }
    ImGui::Separator();

    ImGui::TextUnformatted("Geometry");
    int normals = o.normals == Opt::Normals::Compute ? 1 : 0;
    ImGui::RadioButton("Import normals", &normals, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Compute normals", &normals, 1);
    o.normals = normals ? Opt::Normals::Compute : Opt::Normals::Import;
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Smoothing angle", &o.smoothAngle, 0.f, 180.f, "%.0f deg");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Edges sharper than this stay hard where normals are made:\n"
                          "every mesh with Compute, a mesh without normals either way.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Shape keys");
    ImGui::Checkbox("Import shape keys", &o.morphs);
    ImGui::BeginDisabled(!o.morphs);
    int mn = static_cast<int>(o.morphNormals);
    ImGui::SetNextItemWidth(220.f);
    ImGui::Combo("Shape key normals", &mn, "Auto (the source's, else computed)\0Compute\0None\0");
    o.morphNormals = static_cast<Opt::MorphNormals>(mn);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("Collision");
    int coll = static_cast<int>(o.collision);
    ImGui::SetNextItemWidth(220.f);
    ImGui::Combo("##collision_option", &coll,
                 "Source names (UCX_, -col)\0None\0Box per Mesh\0Convex hull per Mesh\0Complex (its triangles)\0");
    o.collision = static_cast<Opt::Collision>(coll);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Box / Convex / Complex: made for every static Mesh the source\n"
                          "and the author give no collision. What the author edited\n"
                          "in the Mesh Editor is never replaced.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Materials");
    int mats = o.materials == Opt::Materials::Reuse ? 1 : 0;
    ImGui::RadioButton("Create", &mats, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Reuse by name", &mats, 1);
    o.materials = mats ? Opt::Materials::Reuse : Opt::Materials::Create;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A source material whose name exactly one project .imat has\n"
                          "binds that one instead of a new Materials/<name>.imat.");
    ImGui::Checkbox("Import textures", &o.textures);
    ImGui::SameLine();
    ImGui::Checkbox("Flip normal map green (DirectX)", &o.flipGreen);

    ImGui::Spacing();
    ImGui::TextUnformatted("Animation");
    ImGui::Checkbox("Import animations", &o.animations);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The source's clips - skeletal and shape-key channels -\n"
                          "as <name>/<name>.ianim beside the model.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Meshlets");
    ImGui::Checkbox("Build meshlets", &o.meshlets);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clusters for the mesh-shader path, built now and stored\n"
                          "in the .meshlib (static sections only). Without them the\n"
                          "model draws through the vertex shader.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!o.meshlets);
    ImGui::Checkbox("LOD levels", &o.lod);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Simplified levels for distant instances (each half the\n"
                          "triangles), sharing the model's vertices.");

    ImGui::Separator();
    if (ImGui::Button(d.reimport.empty() ? "Import" : "Reimport", ImVec2(120.f, 0.f)))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        AnswerModelImportDialog(true);   // may open the reimport review
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.f, 0.f)))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        AnswerModelImportDialog(false);
        return;
    }
    ImGui::EndPopup();
}

void EditorLayer::ProcessScriptedImports()
{
    if (m_scriptedImports.empty()) return;
    std::vector<ScriptedImport> imports = std::move(m_scriptedImports);
    m_scriptedImports.clear();
    for (const ScriptedImport& s : imports)
    {
        Harness::Jobs::Start(s.jobId);
        std::vector<CapturedLogLine> captured;
        if (s.model)
        {
            // Editor.Import: the request as given. Editor.Reimport: the model's
            // source and folder from its SR record, or the review flow the Content
            // Browser runs, which finishes the job itself.
            ModelImportRequest req = s.modelRequest;
            ModelImportOutcome m;
            {
                Logger::ScopedContext ctx(LogSource::Control, s.requestId, &captured, 32);
                // The options dialog first: it leads to the import or the reimport
                // flow, which finishes the job.
                if (s.dialog)
                {
                    Resource::ModelImportOptions start;
                    if (s.modelRequest.options) start = *s.modelRequest.options;
                    else if (!s.reimport.empty()) Resource::ReadImportOptions(s.reimport, start);
                    if (!s.reimport.empty())
                        OpenModelImportDialog({}, std::string(), s.reimport, start, s.jobId, s.requestId);
                    else
                        OpenModelImportDialog({ s.modelRequest.source }, s.modelRequest.outDir, std::string(), start,
                                              s.jobId, s.requestId);
                    continue;
                }
                if (!s.reimport.empty() && s.review)
                {
                    BeginModelReimport(s.reimport, s.sourceOverride, s.jobId, s.requestId, &s.modelRequest);
                    continue;
                }
                std::string code, message;
                if (!s.reimport.empty() && !ResolveModelReimport(s.reimport, s.sourceOverride, req, code, message))
                {
                    LOG_ERROR("EditorLayer: Reimport '%s': %s", s.reimport.c_str(), message.c_str());
                    m.code    = code;
                    m.message = message;
                }
                else
                {
                    // The prepare runs on a worker; ProcessModelImportTasks commits it
                    // and finishes the job.
                    StartModelImport(req, s.jobId, s.requestId);
                    continue;
                }
            }
            std::vector<std::string> log;
            for (const CapturedLogLine& l : captured)
                if (l.level == LogLevel::Error || l.level == LogLevel::Warning) log.push_back(l.message);
            FinishModelImportJob(s.jobId, m, req.source.empty() ? s.sourceOverride : req.source, req.outDir,
                                 std::move(log));
            continue;
        }
        TextureImportOutcome r;
        {
            Logger::ScopedContext ctx(LogSource::Control, s.requestId, &captured, 32);
            // Editor.Reimport(<.itex>): the source its record (or the guess, or
            // {source}) names must exist; failures use the model reimport's codes.
            std::error_code ec;
            if (!s.reimport.empty() && s.item.sourcePath.empty())
            {
                r.code    = "no_source_record";
                r.message = "'" + s.reimport + "' records no source and no same-named image is beside it - "
                            "Reimport(path, {source = \"<image>\"}) names one";
                LOG_ERROR("EditorLayer: Reimport '%s': %s", s.reimport.c_str(), r.message.c_str());
            }
            else if (!s.reimport.empty() && !std::filesystem::is_regular_file(Sys::U8Path(s.item.sourcePath), ec))
            {
                r.code    = "source_missing";
                r.message = "its source is not there: " + s.item.sourcePath;
                LOG_ERROR("EditorLayer: Reimport '%s': %s", s.reimport.c_str(), r.message.c_str());
            }
            else
                r = ImportTextureFile(s.item);
        }
        nlohmann::json o = nlohmann::json::object();
        o["source"] = s.item.sourcePath;
        o["path"]   = s.item.outputPath;
        o["format"] = Resource::TextureImporter::GetFormatId(s.item.selectedFormat);
        // The rest of what it was imported with (and recorded).
        o["maxSize"]   = s.item.options.maxSize;
        o["mipFilter"] = Resource::TextureImporter::GetMipFilterId(s.item.options.mipFilter);
        o["flipGreen"] = s.item.options.flipGreen;
        o["mips"]      = s.item.options.generateMips;
        if (!r.ok)
        {
            std::vector<std::string> log;
            for (const CapturedLogLine& l : captured)
                if (l.level == LogLevel::Error || l.level == LogLevel::Warning) log.push_back(l.message);
            Harness::Jobs::Fail(s.jobId, r.code, r.message, std::move(log), o.dump());
            continue;
        }
        o["bytes"]  = r.bytes;
        o["width"]  = r.width;
        o["height"] = r.height;
        o["guid"]   = r.guid;
        o["reload"] = { { "state", r.reload }, { "jobId", r.reloadJobId } };
        o["record"] = r.record;
        Harness::Jobs::Succeed(s.jobId, o.dump());
    }
}

void EditorLayer::RenderTextureImportPopup()
{
    constexpr const char* kPopupName = "Review Texture Import";
    if (m_openTextureImportPopup)
    {
        ImGui::OpenPopup(kPopupName);
        m_openTextureImportPopup = false;
    }

    ImGui::SetNextWindowSize(ImVec2(900.f, 560.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPopupName, nullptr, ImGuiWindowFlags_NoResize))
        return;

    ImGui::TextWrapped(
        "Formats were suggested from each filename. Review or override them before any image is decoded, compressed, or written.");
    ImGui::Spacing();

    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##texture_import_review", 3, tableFlags, ImVec2(0.f, 420.f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Suggested", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Import format", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_pendingTextureImports.size(); ++i)
        {
            PendingTextureImport& item = m_pendingTextureImports[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const std::string filename =
                Sys::PathToUtf8(Sys::U8Path(item.sourcePath).filename());
            ImGui::TextUnformatted(filename.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(item.sourcePath.c_str());
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(
                Resource::TextureImporter::GetFormatName(item.suggestedFormat));

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.f);
            const char* preview =
                Resource::TextureImporter::GetFormatName(item.selectedFormat);
            if (ImGui::BeginCombo("##format", preview))
            {
                const int formatCount =
                    static_cast<int>(Resource::TextureImportFormat::Count);
                for (int formatIndex = 0; formatIndex < formatCount; ++formatIndex)
                {
                    const auto format =
                        static_cast<Resource::TextureImportFormat>(formatIndex);
                    const bool selected = item.selectedFormat == format;
                    if (ImGui::Selectable(
                            Resource::TextureImporter::GetFormatName(format), selected))
                    {
                        item.selectedFormat = format;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s",
                            Resource::TextureImporter::GetFormatDescription(format));
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s",
                    Resource::TextureImporter::GetFormatDescription(item.selectedFormat));
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    size_t overwriteCount = 0;
    for (const PendingTextureImport& item : m_pendingTextureImports)
        overwriteCount += std::filesystem::exists(Sys::U8Path(item.outputPath)) ? 1u : 0u;

    if (!m_pendingTextureImports.empty())
    {
        const std::string outputDir = Sys::PathToUtf8(
            Sys::U8Path(m_pendingTextureImports.front().outputPath).parent_path());
        ImGui::TextDisabled("Output: %s", outputDir.c_str());
    }
    if (overwriteCount > 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.2f, 1.f),
                           "(%zu existing .itex file(s) will be replaced)", overwriteCount);
    }

    if (ImGui::Button("Reset All to Suggested"))
    {
        for (PendingTextureImport& item : m_pendingTextureImports)
            item.selectedFormat = item.suggestedFormat;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        m_pendingTextureImports.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();

    const std::string confirmLabel = "Import " +
        std::to_string(m_pendingTextureImports.size()) + " Texture(s)";
    if (ImGui::Button(confirmLabel.c_str()) && !m_pendingTextureImports.empty())
    {
        m_textureImportConfirmed = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ==== Import > Unreal Scene (manifest)... ====
// Cooks a UE scene_manifest.json export (meshes / textures / materials /
// placements) into <assetRoot>/<Scene>/ on a TaskSystem worker while a modal
// shows stage progress.
void EditorLayer::BeginUnrealManifestImport(const std::string& manifestIn, const std::string& outputDir,
                                            const std::string& sceneName)
{
    // One cook at a time; re-open the modal if one is already running.
    if (m_ueImportJob && m_ueImportJob->running.load())
    {
        m_openUeImportPopup = true;
        return;
    }

    const std::string manifest = !manifestIn.empty() ? manifestIn : OpenFileDialog(
        "UE Scene Manifest (*.json)\0*.json\0All Files\0*.*\0\0", nullptr);
    if (manifest.empty())
        return;

    Resource::UnrealSceneImporter::Options options;
    options.manifestPath = manifest;
    options.outputDir = !outputDir.empty()    ? outputDir
                      : !m_currentDir.empty() ? m_currentDir
                      : !m_assetDir.empty()   ? m_assetDir
                                             : "asset";
    options.sceneName = sceneName;   // a Reimport keeps the scene's name

    auto job = std::make_shared<UeImportJob>();
    job->running = true;
    m_ueImportJob = job;
    m_openUeImportPopup = true;

    TaskSystem::Get().Push([job, options]()
    {
        job->result   = Resource::UnrealSceneImporter::Run(options, &job->progress);
        job->running  = false;
        job->finished = true;
    }, TaskSystem::TaskPriority::Low);
}

void EditorLayer::RenderUnrealImportPopup()
{
    if (m_openUeImportPopup)
    {
        ImGui::OpenPopup("Unreal Scene Import");
        m_openUeImportPopup = false;
    }

    if (!ImGui::BeginPopupModal("Unreal Scene Import", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const std::shared_ptr<UeImportJob> job = m_ueImportJob;
    if (!job)
    {
        ImGui::TextUnformatted("No import job.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (job->running.load())
    {
        const int stage = job->progress.stage.load();
        const int done  = job->progress.done.load();
        const int total = job->progress.total.load();

        ImGui::Text("%s  (%d / %d)",
            Resource::UnrealSceneImporter::Progress::StageName(stage), done, total);
        ImGui::ProgressBar(total > 0 ? float(done) / float(total) : 0.0f,
                           ImVec2(420, 0));
        ImGui::TextDisabled("%s", job->progress.GetCurrent().c_str());

        ImGui::Separator();
        if (job->progress.cancel.load())
            ImGui::TextDisabled("Cancelling...");
        else if (ImGui::Button("Cancel"))
            job->progress.cancel = true;
    }
    else if (job->finished.load())
    {
        const auto& r = job->result;
        if (r.ok)
        {
            ImGui::Text("Done: %d textures (%d up-to-date), %d assets -> %d mesh entries,",
                        r.texturesCooked, r.texturesSkipped, r.assetsCooked, r.meshEntries);
            ImGui::Text("%d materials, %d nodes, %d mesh refs.",
                        r.materialsWritten, r.nodesWritten, r.fileRefs);
            // What a re-import kept of what was authored since the last import.
            if (r.keptMaterials || r.restoredBindings || r.keptCollision || r.orphanCollision)
                ImGui::TextDisabled("Kept: %d existing .imat, %d material binding(s), %d authored collision "
                                    "(%d orphaned)", r.keptMaterials, r.restoredBindings, r.keptCollision,
                                    r.orphanCollision);
            if (!job->rescanQueued)
            {
                m_needsRescan     = true;
                job->rescanQueued = true;
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Import failed: %s",
                               r.error.c_str());
        }

        if (!r.warnings.empty())
        {
            ImGui::Separator();
            ImGui::Text("%d warning(s):", (int)r.warnings.size());
            ImGui::BeginChild("ue_import_warnings", ImVec2(560, 160), true);
            for (const std::string& w : r.warnings)
                ImGui::TextWrapped("%s", w.c_str());
            ImGui::EndChild();
        }

        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorLayer::RunImport(AssetType type)
{
    // Build per-type filter strings (OPENFILENAMEA uses NUL separators)
    const char* filter = nullptr;
    const char* outExt = nullptr;

    static const char kFilterTexture[]   = "Image Files\0*.png;*.jpg;*.dds;*.jpeg;*.tga;*.bmp;*.hdr\0All Files\0*.*\0\0";
    static const char kFilterModel[]     = "3D Model Files\0*.obj;*.gltf;*.pmx;*.glb;*.vrm;*.fbx;*.dae;*.3ds;*.ply;*.stl\0All Files\0*.*\0\0";
    static const char kFilterShader[]    = "HLSL Shader\0*.hlsl\0All Files\0*.*\0\0";
    static const char kFilterMaterial[]  = "Material\0*.mat\0All Files\0*.*\0\0";
    static const char kFilterAnimation[] = "Animation Files\0*.fbx;*.gltf;*.glb;*.dae;*.bvh;*.vmd\0All Files\0*.*\0\0";
    static const char kFilterAudio[]     = "Audio Files\0*.wav\0All Files\0*.*\0\0";

    switch (type)
    {
    case AssetType::Texture:   filter = kFilterTexture;   outExt = ".itex";  break;
    case AssetType::Mesh:      filter = kFilterModel;     outExt = nullptr;  break;
    case AssetType::Shader:    filter = kFilterShader;    outExt = ".ishdr"; break;
    case AssetType::Material:  filter = kFilterMaterial;  outExt = ".imat";  break;
    case AssetType::Animation: filter = kFilterAnimation; outExt = nullptr;  break;
    case AssetType::Audio:     filter = kFilterAudio;     outExt = ".aclip"; break;
    default: return;
    }

    const std::vector<std::string> srcPaths =
        OpenFileDialogMulti(filter, m_assetDir.empty() ? nullptr : m_assetDir.c_str());
    if (srcPaths.empty())
        return;  // user cancelled

    namespace fs = std::filesystem;
    const fs::path outDir = !m_currentDir.empty()
        ? Sys::U8Path(m_currentDir)
        : !m_assetDir.empty()
            ? Sys::U8Path(m_assetDir)
            : Sys::U8Path(srcPaths[0]).parent_path();

    // Texture imports are reviewed (the popup) before any source file is read or
    // converted.
    if (type == AssetType::Texture)
    {
        m_pendingTextureImports.clear();
        m_pendingTextureImports.reserve(srcPaths.size());
        for (const std::string& srcPath : srcPaths)
        {
            PendingTextureImport item{};
            item.sourcePath = srcPath;
            item.outputPath = Sys::PathToUtf8(
                outDir / (Sys::U8Path(srcPath).stem() += ".itex"));
            item.suggestedFormat = Resource::TextureImporter::SuggestFormat(srcPath);
            item.selectedFormat = item.suggestedFormat;
            m_pendingTextureImports.push_back(std::move(item));
        }

        m_textureImportConfirmed = false;
        m_openTextureImportPopup = true;
        return;
    }

    // ---- Model ----
    // The options dialog first, then the import Editor.Import runs; over a model
    // this folder already has, the re-import, whose review asks first when
    // something would be lost. The dialog starts from the first existing model's
    // recorded options.
    if (type == AssetType::Mesh)
    {
        Resource::ModelImportOptions start;
        for (const std::string& srcPath : srcPaths)
        {
            const std::string stem = Sys::PathToUtf8(Sys::U8Path(srcPath).stem());
            if (Resource::ReadImportOptions(Sys::PathToUtf8(outDir / Sys::U8Path(stem) / Sys::U8Path(stem + ".iscn")),
                                            start))
                break;
        }
        OpenModelImportDialog(srcPaths, Sys::PathToUtf8(outDir), std::string(), start);
        return;
    }

    for (const std::string& srcPath : srcPaths)
    {

        const std::vector<uint8_t> srcData = ReadFileFully(srcPath);
        if (srcData.empty())
        {
            LOG_ERROR("EditorLayer: failed to read '%s'", srcPath.c_str());
            continue;
        }

        // ---- Animation: single .ianim output via AnimationImporter ----
        if (type == AssetType::Animation)
        {
            if (srcPath.ends_with(".vmd"))
            {
                Resource::VmdImporter imp;
				const std::vector<uint8_t> blob = imp.Import(srcPath, srcData);
                if (blob.empty())
                {
                    LOG_ERROR("EditorLayer: animation import failed for '%s'", srcPath.c_str());
                    continue;
                }
                const std::string outPath =
                    Sys::PathToUtf8(outDir / (Sys::U8Path(srcPath).stem() += ".ianim"));
                if (!WriteFileFully(outPath, blob))
                    LOG_ERROR("EditorLayer: failed to write '%s'", outPath.c_str());
                else
                    LOG_INFO("EditorLayer: imported '%s' → '%s'", srcPath.c_str(), outPath.c_str());
                m_needsRescan = true;
                continue;
            }

            Resource::AnimationImporter imp;
            const std::vector<uint8_t> blob = imp.Import(srcPath, srcData);
            if (blob.empty())
            {
                LOG_ERROR("EditorLayer: animation import failed for '%s'", srcPath.c_str());
                continue;
            }
            const std::string outPath =
                Sys::PathToUtf8(outDir / (Sys::U8Path(srcPath).stem() += ".ianim"));
            if (!WriteFileFully(outPath, blob))
                LOG_ERROR("EditorLayer: failed to write '%s'", outPath.c_str());
            else
                LOG_INFO("EditorLayer: imported '%s' → '%s'", srcPath.c_str(), outPath.c_str());
            m_needsRescan = true;
            continue;
        }

        // ---- Single-file importers (Shader / Material / Audio) ----
        std::vector<uint8_t> blob;
        switch (type)
        {
        case AssetType::Shader:
        {
            Resource::ShaderImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        case AssetType::Material:
        {
            Resource::MaterialImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        case AssetType::Audio:
        {
            Audio::AudioImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        default: break;
        }

        if (blob.empty())
        {
            LOG_ERROR("EditorLayer: import failed for '%s'", srcPath.c_str());
            continue;
        }

        // Checked, not asserted: in Release a null extension would reach
        // path::operator+=.
        if (!ENGINE_CHECK(outExt != nullptr,
                "EditorLayer::RunImport: asset type %d has no output extension ('%s' not written)",
                static_cast<int>(type), srcPath.c_str()))
            continue;
        const std::string outPath =
            Sys::PathToUtf8(outDir / (Sys::U8Path(srcPath).stem() += outExt));

        if (!WriteFileFully(outPath, blob))
        {
            LOG_ERROR("EditorLayer: failed to write '%s'", outPath.c_str());
            continue;
        }

        LOG_INFO("EditorLayer: imported '%s' → '%s'", srcPath.c_str(), outPath.c_str());
        m_needsRescan = true;
    }
}
