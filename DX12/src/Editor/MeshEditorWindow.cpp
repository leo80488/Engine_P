// MeshEditorWindow.cpp - see the header for the contract.

#include "Editor/MeshEditorWindow.h"

#include "ECS/Components.h"                     // MaterialComponent (RequestPreview's payload)
#include "ECS/PhysicsComponents.h"              // ColliderComponent::CollisionLayer
#include "Editor/DrawListRaster.h"
#include "Editor/PreviewPanelMath.h"            // QuantiseBox / BuildCam (shared with the Skeleton Editor)
#include "Editor/SkeletonEditorWindow.h"        // the Skeleton block's "Open in Skeleton Editor"
#include "Graphics/IGraphicsDevice.h"           // CaptureTextureToPNG (-mesheditor_shot)
#include "Graphics/MeshDescriptorHeap.h"         // the shape-key preview's own slots
#include "Math/MathUtils.h"                     // the engine's Euler order and its inverse
#include "Physics/CollisionFit.h"               // box / sphere / capsule / k-DOP / hull fits
#include "Renderer/Preview/PreviewStudioRig.h"  // StudioCameraBasis / kStudioCamera
#include "Renderer/Preview/PreviewTypes.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"               // MESHLIB_FLAG_*
#include "Resource/AssetRefs.h"                 // PercentDecode
#include "Resource/CollisionLibrary.h"          // Save appends new hulls
#include "Resource/MeshLibIdentity.h"           // MeshAssetRef / MeshKeyFromHex / MESH_IDENT_*
#include "Resource/SkeletonAsset.h"             // the bind-pose skeleton overlay
#include "Scene/MeshGeometry.h"
#include "Scene/SceneInstanceLoader.h"          // ReadMaterialBindings / WriteDefaultMaterials
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"                // ImGui::GetCurrentWindow (ImGuizmo's hover window)
#include "imgui/ImGuizmo/ImGuizmo.h"             // the collision gizmo

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>

using namespace DirectX;

namespace Editor
{
namespace
{
    struct ModeEntry { std::uint32_t mode; const char* name; const char* label; };
    // Renderer::ViewMode values; the lighting-bucket isolations (3..11) are
    // the main viewport's diagnosis tools, not an asset's.
    constexpr ModeEntry kModes[] = {
        { 0,  "lit",       "Lit" },
        { 1,  "unlit",     "Unlit" },
        { 2,  "wireframe", "Wireframe" },
        { 12, "basecolor", "Base Color" },
        { 13, "normal",    "World Normal" },
        { 14, "roughness", "Roughness" },
        { 15, "metallic",  "Metallic" },
        { 16, "ao",        "Ambient Occlusion" },
        { 17, "depth",     "Depth" },
    };

    struct ShowEntry { std::uint32_t bit; const char* name; const char* label; };
    constexpr ShowEntry kShows[] = {
        { MeshEditorWindow::kShowBounds,     "bounds",     "Bounds" },
        { MeshEditorWindow::kShowPivot,      "pivot",      "Pivot" },
        { MeshEditorWindow::kShowGrid,       "grid",       "Grid" },
        { MeshEditorWindow::kShowNormals,    "normals",    "Normals" },
        { MeshEditorWindow::kShowTangents,   "tangents",   "Tangents" },
        { MeshEditorWindow::kShowBitangents, "bitangents", "Bitangents" },
        { MeshEditorWindow::kShowSkeleton,   "skeleton",   "Skeleton" },
        { MeshEditorWindow::kShowCollision,  "collision",  "Collision" },
        { MeshEditorWindow::kShowSockets,    "sockets",    "Sockets" },
    };

    struct AddEntry { MeshEditorWindow::CollisionAdd kind; const char* name; const char* label; };
    constexpr AddEntry kAdds[] = {
        { MeshEditorWindow::CollisionAdd::Box,     "box",     "Box" },
        { MeshEditorWindow::CollisionAdd::Sphere,  "sphere",  "Sphere" },
        { MeshEditorWindow::CollisionAdd::Capsule, "capsule", "Capsule" },
        { MeshEditorWindow::CollisionAdd::KDop10X, "kdop10x", "10DOP-X" },
        { MeshEditorWindow::CollisionAdd::KDop10Y, "kdop10y", "10DOP-Y" },
        { MeshEditorWindow::CollisionAdd::KDop10Z, "kdop10z", "10DOP-Z" },
        { MeshEditorWindow::CollisionAdd::KDop18,  "kdop18",  "18DOP" },
        { MeshEditorWindow::CollisionAdd::KDop26,  "kdop26",  "26DOP" },
        { MeshEditorWindow::CollisionAdd::Convex,  "convex",  "Convex hull" },
    };

    constexpr int   kCollisionGizmoId = 0x4D454347;                  // 'MECG' (ImGuizmo::PushID)
    constexpr ImU32 kColBody     = IM_COL32(224, 160, 96, 235);    // the collision overlay's static tan
    constexpr ImU32 kColTrimesh  = IM_COL32(96, 200, 224, 150);    // a section's own triangles
    constexpr ImU32 kColSelected = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kColSocket   = IM_COL32(70, 240, 190, 255);     // the Skeleton Editor's socket green

    constexpr double        kResizeDebounceSeconds = 0.200;
    constexpr std::uint32_t kMaxOverlayLines = 20000;   // sampled, capped
    constexpr std::uint32_t kMaxUvTriangles  = 50000;
    constexpr std::uint64_t kSettleFrames    = 4;       // a shot waits for a picture this old
    constexpr std::uint64_t kShotDeadline    = 400;     // editor frames

    enum StepKind : int
    {
        kStepView = 1, kStepShow, kStepSections, kStepShot, kStepOverlayShot,
        kStepUv, kStepUvShot, kStepOrbit, kStepClose,
    };

    // The render size and the studio camera: shared with the Skeleton Editor
    // (Editor/PreviewPanelMath.h).
    using PreviewPanel::QuantiseBox;
    using PreviewPanel::Cam;
    using PreviewPanel::BuildCam;

    const char* PrimTypeName(Resource::CollisionPrim::Type t)
    {
        switch (t)
        {
        case Resource::CollisionPrim::Type::Box:     return "box";
        case Resource::CollisionPrim::Type::Sphere:  return "sphere";
        case Resource::CollisionPrim::Type::Capsule: return "capsule";
        case Resource::CollisionPrim::Type::Hull:    return "hull";
        case Resource::CollisionPrim::Type::Mesh:    return "mesh";
        }
        return "?";
    }

    // Euler degrees <-> quaternion in the engine's order (XMQuaternionRotationRollPitchYaw:
    // roll Z first, then pitch X, then yaw Y) and its exact inverse (MathUtils).
    XMFLOAT4 QuatOfEuler(const XMFLOAT3& deg) { return MathUtils::EulerDegreesToQuaternion(deg); }
    XMFLOAT3 EulerOfQuat(const XMFLOAT4& q) { return MathUtils::RollPitchYawDegreesFromQuaternion(q); }

    // Where the Mesh Editor puts a model's new hulls: <dir>/<stem>_collision.meshlib
    // (the UE importer's name for a level's collision library), project-relative.
    std::string CollisionLibraryFor(const std::string& model)
    {
        const std::filesystem::path p = Sys::U8Path(model);
        return Sys::RelativizeAssetPath(Sys::PathToUtf8(p.parent_path() / Sys::U8Path(
            Sys::PathToUtf8(p.stem()) + "_collision.meshlib")));
    }

    std::uint64_t Fnv(std::uint64_t h, const void* data, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return h;
    }

    bool Finite3(const XMFLOAT3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    std::string LowerAscii(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool EndsWithCI(const std::string& s, const char* ext)
    {
        const std::string l = LowerAscii(s);
        const std::size_t n = std::strlen(ext);
        return l.size() >= n && l.compare(l.size() - n, n, ext) == 0;
    }

    // A shot's log prefix names its FILE ("[MESHEDITOR] shot lit.png:"): the
    // runner's logLineFields reads the LAST line of a prefix, so one prefix per
    // shot is what lets a pack bound each picture's numbers.
    std::string BaseName(const std::string& p)
    {
        const std::size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    // A harness file name: relative = under the run's diagnostics folder.
    std::string HarnessPath(const std::string& p)
    {
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
        return absolute ? p : Sys::DiagnosticsDir() + "/" + p;
    }

    // "<value>@<frame>" -> value, frame. No '@' = frame 0 (the first frame the
    // subject is open).
    bool SplitAt(const std::string& item, std::string& value, std::uint64_t& at)
    {
        const std::size_t p = item.find_last_of('@');
        if (p == std::string::npos) { value = item; at = 0; return !value.empty(); }
        value = item.substr(0, p);
        const std::string f = item.substr(p + 1);
        if (value.empty() || f.empty() ||
            !std::all_of(f.begin(), f.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            return false;
        at = std::strtoull(f.c_str(), nullptr, 10);
        return true;
    }

    std::vector<std::string> SplitList(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::size_t b = 0;
        while (b <= s.size())
        {
            const std::size_t e = s.find(sep, b);
            const std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
            if (!item.empty()) out.push_back(item);
            if (e == std::string::npos) break;
            b = e + 1;
        }
        return out;
    }

    ImU32 SectionColour(int k)
    {
        static const ImU32 kPalette[] = {
            IM_COL32(90, 200, 255, 255), IM_COL32(255, 170, 60, 255), IM_COL32(120, 230, 120, 255),
            IM_COL32(240, 110, 200, 255), IM_COL32(250, 230, 90, 255), IM_COL32(170, 140, 255, 255),
            IM_COL32(80, 220, 200, 255), IM_COL32(255, 120, 110, 255),
        };
        return kPalette[static_cast<std::size_t>(k) % (sizeof(kPalette) / sizeof(kPalette[0]))];
    }
}

MeshEditorWindow& MeshEditorWindow::Get()
{
    static MeshEditorWindow s_instance;
    return s_instance;
}

// ==== Names ====
const char* MeshEditorWindow::ViewModeName(std::uint32_t mode)
{
    for (const ModeEntry& m : kModes)
        if (m.mode == mode) return m.name;
    return "?";
}

bool MeshEditorWindow::ParseViewMode(const std::string& name, std::uint32_t& out)
{
    const std::string n = LowerAscii(name);
    for (const ModeEntry& m : kModes)
        if (n == m.name) { out = m.mode; return true; }
    if (n == "worldnormal" || n == "normals") { out = 13; return true; }
    return false;
}

bool MeshEditorWindow::ParseShow(const std::string& text, std::uint32_t& out)
{
    const std::string t = LowerAscii(text);
    if (t == "none" || t == "0") { out = 0; return true; }
    if (t == "all") { out = kShowAll; return true; }
    if (t.size() > 2 && t[0] == '0' && t[1] == 'x')
    {
        char* end = nullptr;
        const unsigned long v = std::strtoul(t.c_str() + 2, &end, 16);
        if (!end || *end) return false;
        out = static_cast<std::uint32_t>(v) & kShowAll;
        return true;
    }
    std::uint32_t bits = 0;
    for (const std::string& part : SplitList(t, '+'))
    {
        bool found = false;
        for (const ShowEntry& s : kShows)
            if (part == s.name) { bits |= s.bit; found = true; }
        if (!found) return false;
    }
    out = bits;
    return true;
}

std::string MeshEditorWindow::ShowNames(std::uint32_t bits)
{
    std::string out;
    for (const ShowEntry& s : kShows)
        if (bits & s.bit) out += (out.empty() ? "" : "+") + std::string(s.name);
    return out.empty() ? "none" : out;
}

// ==== The subject ====
void MeshEditorWindow::RequestOpen(const std::string& model, std::uint64_t meshKey,
                                   const std::string& meshName)
{
    m_open        = true;
    m_openQueued  = true;
    m_queuedModel = model;
    m_queuedKey   = meshKey;
    m_queuedName  = meshName;
}

bool MeshEditorWindow::OpenNow(const MeshEditorContext& ctx, const std::string& model,
                               const std::string& mesh, bool discard, std::string& code,
                               std::string& why)
{
    if (m_loaded && Dirty() && !discard)
    {
        code = "dirty";
        why  = "the Mesh Editor has unsaved edits to '" + m_meshName + "' - save them, or pass discard";
        return false;
    }
    std::uint64_t key = 0;
    std::string   name;
    if (!Resource::MeshKeyFromHex(mesh, key)) { key = 0; name = mesh; }
    if (key == 0 && name.empty())
    {
        code = "bad_value";
        why  = "no mesh named";
        return false;
    }
    m_openQueued = false;
    return LoadSubject(ctx, model, key, name, code, why);
}

bool MeshEditorWindow::CloseNow(bool discard, std::string& why)
{
    if (m_loaded && Dirty() && !discard)
    {
        why = "the Mesh Editor has unsaved edits to '" + m_meshName + "'";
        return false;
    }
    if (Dirty()) m_staged = m_saved;
    m_open = false;
    m_askFor = 0;
    LOG_INFO("[MESHEDITOR] closed '%s'%s", m_ref.c_str(), discard ? " (edits discarded)" : "");
    return true;
}

bool MeshEditorWindow::LoadSubject(const MeshEditorContext& ctx, const std::string& model,
                                   std::uint64_t key, const std::string& name,
                                   std::string& code, std::string& why)
{
    auto fail = [&](const char* c, std::string w)
    {
        code = c;
        why  = std::move(w);
        LOG_WARNING("[MESHEDITOR] cannot open '%s' (%s): %s", model.c_str(), c, why.c_str());
        return false;
    };
    if (!ctx.renderer) return fail("no_world", "the editor has no renderer");
    const std::string modelRef = Sys::RelativizeAssetPath(model);
    if (Preview::AssetKindForPath(modelRef) != Preview::AssetKind::Model)
        return fail("bad_value", "'" + model + "' is not a model (.iscn)");
    if (!Resource::AssetFS::Get().Exists(modelRef))
        return fail("load_failed", "'" + modelRef + "' does not exist");

    // The view the picture is composed from (preview references: nothing
    // becomes a world library because the editor looked at it).
    MeshAssetRegistry::Model view;
    if (!ctx.renderer->BuildPreviewModelView(modelRef, view))
        return fail("load_failed", "a library of '" + modelRef + "' would not open" +
                                   (view.error.empty() ? std::string() : " (" + view.error + ")"));
    const MeshAssetRegistry::Mesh* mesh = MeshAssetRegistry::Find(view, key, name);
    if (!mesh)
        return fail("bad_value", "the model has no mesh " +
                                 (key ? Resource::MeshKeyToHex(key) : "'" + name + "'") +
                                 (view.error.empty() ? std::string() : " (" + view.error + ")"));

    // The geometry, from the file (MeshLibrary keeps no CPU copy).
    MeshGeometry::Library lib;
    std::string           gwhy;
    if (!MeshGeometry::ReadLibrary(mesh->libraryPath, lib, gwhy)) return fail("load_failed", gwhy);

    // Which sections this model owns the default of: the ones it has an F-line
    // for (an entry it does not place takes its default from the library's
    // own model file, which is where that slot is edited).
    std::vector<SceneInstanceLoader::MaterialBinding> binds;
    SceneInstanceLoader::ReadMaterialBindings(modelRef, binds);
    std::unordered_set<std::string> owned;
    for (const auto& b : binds)
        // An F-line with `sec=` owns the section its KEY names (its mesh= may be a stale
        // hint naming another); one without, the entry its mesh= names.
        owned.insert(b.sectionKey ? b.meshAbsPath + "#k" + Resource::MeshKeyToHex(b.sectionKey)
                                  : b.meshAbsPath + '#' + std::to_string(b.meshId));

    std::vector<Section> secs;
    secs.reserve(mesh->sections.size());
    std::uint64_t bytes = 0;
    for (const MeshAssetRegistry::Section& s : mesh->sections)
    {
        Section S;
        S.meta     = s;
        S.editable = owned.count(mesh->libraryPath + "#k" + Resource::MeshKeyToHex(s.key)) != 0 ||
                     owned.count(mesh->libraryPath + '#' + std::to_string(s.entry)) != 0;
        MeshGeometry::Section g;
        std::string           dwhy;
        if (MeshGeometry::DecodeSection(lib, s.entry, g, dwhy))
        {
            S.vertices  = static_cast<std::uint32_t>(g.positions.size());
            S.triangles = static_cast<std::uint32_t>(g.indices.size() / 3);
            S.pos.resize(g.positions.size() * 3);
            S.nrm.resize(g.normals.size() * 3);
            std::memcpy(S.pos.data(), g.positions.data(), S.pos.size() * sizeof(float));
            std::memcpy(S.nrm.data(), g.normals.data(), S.nrm.size() * sizeof(float));
            if (!g.tangents.empty())
            {
                S.tan.resize(g.tangents.size() * 4);
                std::memcpy(S.tan.data(), g.tangents.data(), S.tan.size() * sizeof(float));
            }
            for (int c = 0; c < 4; ++c)
                if (!g.uv[c].empty())
                {
                    S.uv[c].resize(g.uv[c].size() * 2);
                    std::memcpy(S.uv[c].data(), g.uv[c].data(), S.uv[c].size() * sizeof(float));
                }
            S.col = std::move(g.colors);
            S.idx = std::move(g.indices);
            const Resource::MeshLibraryEntry e = MeshGeometry::EntryAt(lib, s.entry);
            bytes += std::uint64_t(e.vertexCount) * lib.meta.vertexStride + std::uint64_t(e.indexCount) * 4u;
        }
        else
            LOG_WARNING("[MESHEDITOR] '%s' section %u: %s", modelRef.c_str(), s.entry, dwhy.c_str());
        secs.push_back(std::move(S));
    }

    // A skinned Mesh: its model's bind-pose skeleton, for the Skeleton overlay.
    // "Skinned" is Resource::MeshCountsAsSkinned (the registry's Mesh::skinned): the
    // identity's flag, or - for an index-derived identity, which never carries it (every
    // PMX) - the model naming a skeleton. A static Mesh of a skinned model follows no
    // bone: no skeleton here.
    std::string        skeletonPath = mesh->skinned ? SceneInstanceLoader::ModelSkeletonPath(modelRef) : std::string();
    std::vector<float> joints;
    std::vector<int>   jointParent;
    std::vector<std::string> boneNames;
    std::vector<SceneInstanceLoader::SkinBlendStream> blends;
    {
        if (!skeletonPath.empty())
        {
            auto skel = std::make_unique<SkeletonAsset>();
            std::vector<ClipAsset> clips;
            // With the blend streams: the weight heatmap and the bones that move the Mesh.
            if (SceneInstanceLoader::LoadSkeletonFile(skeletonPath, *skel, clips, &blends))
            {
                const std::uint32_t n = std::min(skel->boneCount, SkeletonAsset::MAX_BONES);
                joints.reserve(size_t(n) * 3);
                jointParent.reserve(n);
                for (std::uint32_t b = 0; b < n; ++b)
                {
                    // inverse(inverseBindPose): bone -> the space the vertices were BOUND in, the
                    // Mesh's own (assimp's offset matrix maps mesh -> bone space; a PMX binds in model
                    // space, its mesh space). bindPose is the SCENE's space instead - off by the mesh
                    // node's transform on an FBX.
                    const XMMATRIX ib = XMLoadFloat4x4(&skel->inverseBindPose[b]);
                    XMVECTOR det;
                    XMFLOAT4X4 m;
                    XMStoreFloat4x4(&m, XMMatrixInverse(&det, ib));
                    joints.insert(joints.end(), { m._41, m._42, m._43 });
                    jointParent.push_back(skel->parentIndex[b]);
                    boneNames.emplace_back(skel->boneNames[b], strnlen(skel->boneNames[b], sizeof(skel->boneNames[b])));
                }
            }
            else
                LOG_WARNING("[MESHEDITOR] '%s': its skeleton '%s' did not load - no Skeleton overlay",
                            modelRef.c_str(), skeletonPath.c_str());

            // The skeleton is in the MODEL's space; the Mesh is shown in its own, the placing
            // node's (an FBX part carries the file's unit scale / axis there; a PMX part sits at
            // the identity). Bring the joints into the Mesh's space through the inverse of the
            // node that places its first section (ReadMeshPlacements: the node's world without
            // the root's own transform - the space the skeleton's root entity is spawned in).
            std::vector<SceneInstanceLoader::MeshPlacement> places;
            if (!joints.empty() && !mesh->sections.empty() &&
                SceneInstanceLoader::ReadMeshPlacements(modelRef, places))
            {
                const std::string libKey = Resource::MeshLibCanonicalPath(mesh->libraryPath);
                for (const SceneInstanceLoader::MeshPlacement& pl : places)
                {
                    if (pl.meshId != mesh->sections.front().entry ||
                        Resource::MeshLibCanonicalPath(pl.meshPath) != libKey)
                        continue;
                    XMVECTOR det;
                    const XMMATRIX inv = XMMatrixInverse(
                        &det, XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(pl.world)));
                    for (std::size_t j = 0; j + 2 < joints.size(); j += 3)
                    {
                        XMFLOAT3 p;
                        XMStoreFloat3(&p, XMVector3TransformCoord(
                            XMVectorSet(joints[j], joints[j + 1], joints[j + 2], 1.f), inv));
                        joints[j] = p.x; joints[j + 1] = p.y; joints[j + 2] = p.z;
                    }
                    break;
                }
            }
        }
    }

    if (!joints.empty())
    {
        float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (std::size_t j = 0; j + 2 < joints.size(); j += 3)
            for (int a = 0; a < 3; ++a)
            {
                lo[a] = std::min(lo[a], joints[j + a]);
                hi[a] = std::max(hi[a], joints[j + a]);
            }
        LOG_INFO("[MESHEDITOR] skeleton: file='%s' bones=%zu joints=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) "
                 "mesh=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)", skeletonPath.c_str(), jointParent.size(),
                 lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], mesh->aabbMin.x, mesh->aabbMin.y, mesh->aabbMin.z,
                 mesh->aabbMax.x, mesh->aabbMax.y, mesh->aabbMax.z);
    }

    // ---- The Mesh's asset collision ----
    // Its model's records for this key (re-read when the file changed), and whether the
    // panel may write them.
    StagedCollision coll;
    coll.setup.meshKey = mesh->key;
    if (const Resource::MeshBodySetup* s = Resource::ModelCollisionAt(modelRef)->Find(mesh->key)) coll.setup = *s;
    coll.pending.assign(coll.setup.prims.size(), {});
    std::string lock;
    std::error_code fec;
    if (!std::filesystem::is_regular_file(Sys::U8Path(modelRef), fec))
        lock = "the model is not a loose file (packed): its collision is read-only";
    else if (Sys::ThroughReparsePoint(modelRef))
        lock = "the model is another checkout's (reached through a junction): its collision is read-only here";
    else if (SceneInstanceLoader::ModelHasCRecords(modelRef))
        lock = "the model still has pre-M6 C records, which win over asset collision on the parts they name - "
               "migrate them first (Game.exe -assettool=collision_migrate)";
    else if (mesh->skinned)
        lock = "a skinned Mesh moves with its skeleton: a static body at the bind pose is not its collision";

    // ---- The Mesh's shape-key records: MK / MP ----
    Resource::MeshShapeKeySetup shapeKeys;
    shapeKeys.meshKey = mesh->key;
    if (const Resource::MeshShapeKeySetup* s = Resource::ModelShapeKeysAt(modelRef)->Find(mesh->key)) shapeKeys = *s;
    std::string shapeKeyLock;
    if (!std::filesystem::is_regular_file(Sys::U8Path(modelRef), fec))
        shapeKeyLock = "the model is not a loose file (packed): its shape-key records are read-only";
    else if (Sys::ThroughReparsePoint(modelRef))
        shapeKeyLock = "the model is another checkout's (reached through a junction): its shape-key records are read-only here";

    // ---- The Mesh's sockets (SO mesh=<key>) ----
    std::vector<Resource::ModelSocket> sockets;
    for (const Resource::ModelSocket& so : Resource::ModelSocketsAt(modelRef)->sockets)
        if (so.meshKey == mesh->key) sockets.push_back(so);
    std::string socketLock;
    if (!std::filesystem::is_regular_file(Sys::U8Path(modelRef), fec))
        socketLock = "the model is not a loose file (packed): its sockets are read-only";
    else if (Sys::ThroughReparsePoint(modelRef))
        socketLock = "the model is another checkout's (reached through a junction): its sockets are read-only here";

    // ---- Commit ----
    // The previous subject's shape-key and weight-heatmap copies go first (their heap
    // slots).
    m_shapeKeys.Release(ctx.gfx, &ctx.renderer->GetMeshManager().GetDescriptorHeap(), ctx.worldEpoch);
    m_weights.Release(ctx.gfx, &ctx.renderer->GetMeshManager().GetDescriptorHeap(), ctx.worldEpoch);
    m_skeletonPath = skeletonPath;
    m_joints       = std::move(joints);
    m_jointParent  = std::move(jointParent);
    m_model            = modelRef;
    m_meshKey          = mesh->key;
    m_meshName         = mesh->name;
    m_ref              = Resource::MeshAssetRef(modelRef, mesh->key);
    m_libraryPath      = mesh->libraryPath;
    m_meshFlags        = mesh->flags;
    m_meshSkinned      = mesh->skinned;
    m_identityFromFile = lib.idFromFile;
    m_sections         = std::move(secs);
    m_stride           = lib.meta.vertexStride;
    m_libFlags         = lib.meta.flags;
    m_geometryBytes    = bytes;
    std::memcpy(m_boundsMin, &mesh->aabbMin, sizeof(m_boundsMin));
    std::memcpy(m_boundsMax, &mesh->aabbMax, sizeof(m_boundsMax));
    m_saved = Staged{};
    for (const Section& s : m_sections) m_saved.materials.push_back(s.meta.defaultMaterial);
    m_saved.collision = std::move(coll);
    m_saved.shapeKeys = std::move(shapeKeys);
    m_saved.sockets   = std::move(sockets);
    m_staged = m_saved;
    m_shapeKeyLock = shapeKeyLock;
    m_socketLock   = socketLock;
    m_selSocket.clear();
    m_socketNameFor.clear();
    m_socketEulerFor.clear();
    m_boneNames     = std::move(boneNames);
    m_undo.clear();
    m_redo.clear();
    m_collisionLock     = lock;
    m_collisionLibrary  = CollisionLibraryFor(modelRef);
    m_collisionSelected = -1;
    m_primShapesKey     = 0;   // rebuilt on the next draw
    m_visible.assign(m_sections.size(), true);
    m_selected  = -1;
    m_uvSection = -1;
    m_uvChannel = 0;
    ResetView();

    // The Mesh's shape keys (placement k = section k).
    {
        std::vector<MeshShapeKeys::SectionIn> in(m_sections.size());
        for (std::size_t k = 0; k < m_sections.size(); ++k)
        {
            const Section& sec = m_sections[k];
            in[k].entry = sec.meta.entry;
            in[k].key   = sec.meta.key;
            in[k].pos   = &sec.pos;
            in[k].nrm   = &sec.nrm;
            in[k].tan   = &sec.tan;
            for (int c = 0; c < 4; ++c) in[k].uv[c] = &sec.uv[c];
            in[k].col   = &sec.col;
            in[k].idx   = &sec.idx;
        }
        std::string swhy;
        if (!m_shapeKeys.Load(modelRef, in, swhy))
            LOG_WARNING("[MESHEDITOR] '%s': its shape keys did not read (%s)", modelRef.c_str(), swhy.c_str());
        m_shapeKeyFilter[0] = '\0';
    }
    // The weight heatmap (part k = section k) and the bones that move the Mesh.
    m_skinBones.clear();
    m_weights.SetTag("[MESHEDITOR]");
    if (!blends.empty())
    {
        std::vector<BoneWeightHeat::SectionIn> in(m_sections.size());
        for (std::size_t k = 0; k < m_sections.size(); ++k)
            in[k] = { m_libraryPath, m_sections[k].meta.entry, m_sections[k].meta.key };
        std::vector<BoneWeightHeat::Stream> streams;
        for (const auto& b : blends) streams.push_back({ b.meshIndex, b.weights.data(), b.weights.size() });
        std::string wwhy;
        if (!m_weights.LoadSections(modelRef, in, streams, wwhy))
            LOG_WARNING("[MESHEDITOR] '%s': its weights did not pair (%s)", modelRef.c_str(), wwhy.c_str());
        const std::vector<std::uint32_t> counts = m_weights.BoneVertexCounts(static_cast<std::uint32_t>(m_boneNames.size()));
        for (std::size_t b = 0; b < counts.size(); ++b)
            if (counts[b] > 0) m_skinBones.push_back({ static_cast<int>(b), m_boneNames[b], counts[b] });
    }
    else
        m_weights.Clear();
    m_boneFilter[0] = '\0';
    m_loaded = true;
    m_open   = true;
    m_error.clear();

    std::uint32_t tris = 0, verts = 0;
    for (const Section& s : m_sections) { tris += s.triangles; verts += s.vertices; }
    std::string uvs = "0";
    if (m_libFlags & Resource::MESHLIB_FLAG_HAS_UV1) uvs += ",1";
    if (m_libFlags & Resource::MESHLIB_FLAG_HAS_UV2) uvs += ",2";
    if (m_libFlags & Resource::MESHLIB_FLAG_HAS_UV3) uvs += ",3";
    std::size_t editable = 0;
    for (const Section& s : m_sections) editable += s.editable ? 1 : 0;
    LOG_INFO("[MESHEDITOR] open: model='%s' key=%s name='%s' sections=%zu editable=%zu tris=%u verts=%u "
             "stride=%u uv=%s tangent=%d color=%d skinned=%d morph=%d bytes=%llu identity=%s bones=%zu "
             "collision=%zu collisionEditable=%d shapeKeys=%zu sockets=%zu skinBones=%zu",
             m_model.c_str(), Resource::MeshKeyToHex(m_meshKey).c_str(), m_meshName.c_str(),
             m_sections.size(), editable, tris, verts, m_stride, uvs.c_str(),
             (m_libFlags & Resource::MESHLIB_FLAG_HAS_TANGENT) ? 1 : 0,
             (m_libFlags & Resource::MESHLIB_FLAG_HAS_COLOR) ? 1 : 0,
             Skinned() ? 1 : 0,
             (m_meshFlags & Resource::MESH_IDENT_HAS_MORPH) ? 1 : 0,
             static_cast<unsigned long long>(m_geometryBytes), m_identityFromFile ? "file" : "synthesized",
             m_jointParent.size(), m_saved.collision.setup.prims.size(), m_collisionLock.empty() ? 1 : 0,
             m_shapeKeys.Count(), m_saved.sockets.size(), m_skinBones.size());
    return true;
}

bool MeshEditorWindow::Skinned() const
{
    return m_meshSkinned;
}

void MeshEditorWindow::ResetView()
{
    m_yaw = m_pitch = 0.f;
    m_dist = 1.f;
    m_pan[0] = m_pan[1] = m_pan[2] = 0.f;
}

// Bit k = section k is drawn, over the Mesh's own sections only (a MeshAsset
// composition has exactly one placement per section; past 63 always drawn).
std::uint64_t MeshEditorWindow::VisibleMask() const
{
    const std::size_t n = m_visible.size();
    std::uint64_t m = (n >= 64) ? ~0ull : ((1ull << n) - 1ull);
    for (std::size_t k = 0; k < n && k < 64; ++k)
        if (!m_visible[k]) m &= ~(1ull << k);
    return m;
}

// ==== Staged slots and the panel's own history ====
void MeshEditorWindow::PushUndo()
{
    m_undo.push_back(m_staged);
    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
}

bool MeshEditorWindow::Undo()
{
    if (m_undo.empty()) return false;
    m_redo.push_back(m_staged);
    m_staged = m_undo.back();
    m_undo.pop_back();
    if (m_collisionSelected >= static_cast<int>(m_staged.collision.setup.prims.size())) m_collisionSelected = -1;
    if (!m_selSocket.empty() && FindSocket(m_selSocket) < 0) m_selSocket.clear();
    m_socketNameFor.clear();
    m_socketEulerFor.clear();
    LOG_INFO("[MESHEDITOR] undo: dirty=%d undo=%zu redo=%zu", Dirty() ? 1 : 0, m_undo.size(), m_redo.size());
    return true;
}

bool MeshEditorWindow::Redo()
{
    if (m_redo.empty()) return false;
    m_undo.push_back(m_staged);
    m_staged = m_redo.back();
    m_redo.pop_back();
    if (m_collisionSelected >= static_cast<int>(m_staged.collision.setup.prims.size())) m_collisionSelected = -1;
    if (!m_selSocket.empty() && FindSocket(m_selSocket) < 0) m_selSocket.clear();
    m_socketNameFor.clear();
    m_socketEulerFor.clear();
    LOG_INFO("[MESHEDITOR] redo: dirty=%d undo=%zu redo=%zu", Dirty() ? 1 : 0, m_undo.size(), m_redo.size());
    return true;
}

// The records' own text (FormatBodySetup: what Save would write, at its
// precision) and every hull's pending points.
bool MeshEditorWindow::SameCollision(const StagedCollision& a, const StagedCollision& b)
{
    if (a.pending.size() != b.pending.size()) return false;
    for (std::size_t i = 0; i < a.pending.size(); ++i)
        if (a.pending[i].size() != b.pending[i].size() ||
            (!a.pending[i].empty() &&
             std::memcmp(a.pending[i].data(), b.pending[i].data(), a.pending[i].size() * sizeof(XMFLOAT3)) != 0))
            return false;
    return Resource::FormatBodySetup(a.setup) == Resource::FormatBodySetup(b.setup);
}

bool MeshEditorWindow::StageMaterial(int section, const std::string& material, std::string& why)
{
    if (!m_loaded) { why = "no mesh is open"; return false; }
    if (section < 0 || section >= static_cast<int>(m_sections.size()))
    {
        why = "no section " + std::to_string(section);
        return false;
    }
    if (!m_sections[static_cast<std::size_t>(section)].editable)
    {
        why = "section " + std::to_string(section) + "'s default material is not this model's to change "
              "(the model has no F-line for it; it comes from the library's own model)";
        return false;
    }
    const std::string mat = material.empty() ? std::string() : Sys::RelativizeAssetPath(material);
    if (!mat.empty())
    {
        if (!EndsWithCI(mat, ".imat") && !EndsWithCI(mat, ".imatinst"))
        {
            why = "'" + material + "' is not a material (.imat / .imatinst)";
            return false;
        }
        if (!Resource::AssetFS::Get().Exists(mat))
        {
            why = "'" + mat + "' does not exist";
            return false;
        }
    }
    std::string& slot = m_staged.materials[static_cast<std::size_t>(section)];
    if (slot == mat) return true;
    PushUndo();
    m_staged.materials[static_cast<std::size_t>(section)] = mat;
    LOG_INFO("[MESHEDITOR] stage: section=%d slot='%s' material='%s' dirty=%d undo=%zu", section,
             m_sections[static_cast<std::size_t>(section)].meta.slot.c_str(), mat.c_str(), Dirty() ? 1 : 0,
             m_undo.size());
    return true;
}

bool MeshEditorWindow::Revert()
{
    if (!Dirty()) return false;
    PushUndo();
    m_staged = m_saved;
    if (m_collisionSelected >= static_cast<int>(m_staged.collision.setup.prims.size())) m_collisionSelected = -1;
    if (!m_selSocket.empty() && FindSocket(m_selSocket) < 0) m_selSocket.clear();
    m_socketNameFor.clear();   // the fields re-seed from the reverted socket
    m_socketEulerFor.clear();
    LOG_INFO("[MESHEDITOR] revert: '%s'", m_ref.c_str());
    return true;
}

// ==== Shape-key records: MK metadata and MP presets, staged ====
Resource::ShapeKeyMeta MeshEditorWindow::ShapeKeyMetaOf(const std::string& name) const
{
    if (const Resource::ShapeKeyMeta* m = m_staged.shapeKeys.FindMeta(name)) return *m;
    Resource::ShapeKeyMeta d;
    d.name = name;
    return d;
}

bool MeshEditorWindow::StageShapeKeyMeta(const Resource::ShapeKeyMeta& meta, std::string& why)
{
    return StageShapeKeyMetaImpl(meta, why, true);
}

bool MeshEditorWindow::StageShapeKeyMetaImpl(const Resource::ShapeKeyMeta& meta, std::string& why, bool pushUndo)
{
    if (!m_loaded) { why = "no mesh is open"; return false; }
    if (!m_shapeKeyLock.empty()) { why = m_shapeKeyLock; return false; }
    if (m_shapeKeys.Find(meta.name) < 0) { why = "the Mesh has no shape key '" + meta.name + "'"; return false; }
    if (!Resource::IsShapeKeyCategory(meta.category))
    {
        why = "category is expression, viseme, body, corrective or empty";
        return false;
    }
    if (!(meta.min >= 0.f && meta.max <= 1.f && meta.min < meta.max))
    {
        why = "the range is inside 0..1, min below max";
        return false;
    }
    if (!(meta.def >= meta.min && meta.def <= meta.max)) { why = "the default is inside the range"; return false; }
    // An edit makes the record the AUTHOR's (no src=: a re-import keeps it and it
    // wins over the source's); the same values leave a source's record the source's.
    Resource::ShapeKeyMeta mine = meta;
    mine.source.clear();
    Resource::MeshShapeKeySetup next = m_staged.shapeKeys;
    auto it = std::find_if(next.meta.begin(), next.meta.end(),
                           [&](const Resource::ShapeKeyMeta& m) { return m.name == meta.name; });
    if (it != next.meta.end())
    {
        Resource::ShapeKeyMeta cur = *it;
        cur.source.clear();
        if (cur == mine) return true;
    }
    if (mine.IsDefault())
    {
        // Back to the defaults: no record - unless the source has one for this
        // target, which a re-import would bring back; an all-default record of the
        // author's keeps it away (FormatShapeKeys writes those).
        if (it != next.meta.end())
        {
            if (it->source.empty()) next.meta.erase(it);
            else                    *it = mine;
        }
    }
    else if (it != next.meta.end()) *it = mine;
    else next.meta.push_back(mine);
    if (next == m_staged.shapeKeys) return true;
    if (pushUndo) PushUndo();
    m_staged.shapeKeys = std::move(next);
    return true;
}

bool MeshEditorWindow::SaveShapeKeyPreset(const std::string& name, std::string& why)
{
    if (!m_loaded) { why = "no mesh is open"; return false; }
    if (!m_shapeKeyLock.empty()) { why = m_shapeKeyLock; return false; }
    if (name.empty()) { why = "a preset needs a name"; return false; }
    // What the picture shows: the weights that count (solo leaves one).
    Resource::ShapeKeyPreset p;
    p.name = name;
    for (std::size_t i = 0; i < m_shapeKeys.Count(); ++i)
    {
        const bool counts = m_shapeKeys.Solo() < 0 || m_shapeKeys.Solo() == static_cast<int>(i);
        if (counts && m_shapeKeys.Weight(i) > 0.001f) p.weights.emplace_back(m_shapeKeys.Name(i), m_shapeKeys.Weight(i));
    }
    Resource::MeshShapeKeySetup next = m_staged.shapeKeys;
    auto it = std::find_if(next.presets.begin(), next.presets.end(),
                           [&](const Resource::ShapeKeyPreset& q) { return q.name == name; });
    if (it != next.presets.end()) *it = std::move(p);
    else next.presets.push_back(std::move(p));
    if (next == m_staged.shapeKeys) return true;
    PushUndo();
    m_staged.shapeKeys = std::move(next);
    return true;
}

bool MeshEditorWindow::DeleteShapeKeyPreset(const std::string& name, std::string& why)
{
    if (!m_loaded) { why = "no mesh is open"; return false; }
    if (!m_shapeKeyLock.empty()) { why = m_shapeKeyLock; return false; }
    Resource::MeshShapeKeySetup next = m_staged.shapeKeys;
    auto it = std::find_if(next.presets.begin(), next.presets.end(),
                           [&](const Resource::ShapeKeyPreset& q) { return q.name == name; });
    if (it == next.presets.end()) { why = "the Mesh has no preset '" + name + "'"; return false; }
    next.presets.erase(it);
    PushUndo();
    m_staged.shapeKeys = std::move(next);
    return true;
}

bool MeshEditorWindow::ApplyShapeKeyPreset(const std::string& name, std::string& why)
{
    const Resource::ShapeKeyPreset* p = m_staged.shapeKeys.FindPreset(name);
    if (!p) { why = "the Mesh has no preset '" + name + "'"; return false; }
    m_shapeKeys.ResetWeights();
    for (const auto& [target, w] : p->weights)
        if (const int i = m_shapeKeys.Find(target); i >= 0) m_shapeKeys.SetWeight(static_cast<std::size_t>(i), w);
    return true;
}

void MeshEditorWindow::ResetShapeKeyWeights()
{
    m_shapeKeys.ResetWeights();
    for (const Resource::ShapeKeyMeta& m : m_staged.shapeKeys.meta)
        if (const int i = m_shapeKeys.Find(m.name); i >= 0 && m.def != 0.f)
            m_shapeKeys.SetWeight(static_cast<std::size_t>(i), m.def);
}

bool MeshEditorWindow::SaveNow(const MeshEditorContext& ctx, std::string& why, SaveReport* out)
{
    SaveReport rep;
    if (out) *out = rep;
    if (!m_loaded) { why = "no mesh is open"; return false; }
    if (ctx.playing)
    {
        why = "stop Play first - a model's defaults and collision are edited against the Edit-mode scene";
        return false;
    }
    if (!Dirty()) return true;
    const bool collisionEdited = CollisionDirty();
    if (collisionEdited && !m_collisionLock.empty())
    {
        why = m_collisionLock;
        return false;
    }
    const bool shapeKeysEdited = ShapeKeysDirty();
    if (shapeKeysEdited && !m_shapeKeyLock.empty())
    {
        why = m_shapeKeyLock;
        return false;
    }
    const bool socketsEdited = SocketsDirty();
    if (socketsEdited && !m_socketLock.empty())
    {
        why = m_socketLock;
        return false;
    }

    // ---- The collision: new hulls into the library, then the Mesh's records ----
    // `written` is the staged state as the file will say it: a hull prim names its
    // library entry instead of carrying points.
    Staged written = m_staged;
    bool   partial = false;   // something reached the file, something did not
    if (collisionEdited)
    {
        StagedCollision& c = written.collision;
        std::vector<Resource::CollisionLibrary::PointSet> hulls;
        std::vector<std::size_t>                          hullPrims;
        for (std::size_t i = 0; i < c.pending.size(); ++i)
            if (!c.pending[i].empty())
            {
                hulls.push_back(c.pending[i]);
                hullPrims.push_back(i);
            }
        if (!hulls.empty())
        {
            std::vector<std::uint64_t> keys;
            std::string                err;
            if (!Resource::CollisionLibrary::AppendHulls(m_collisionLibrary, hulls, m_meshKey, m_meshName, keys, &err,
                                                         &rep.hullsAppended))
            {
                why = "the collision library was not written: " + err;
                return false;
            }
            // lib= is relative to the model: the library sits beside it.
            const std::string libName = Sys::PathToUtf8(Sys::U8Path(m_collisionLibrary).filename());
            for (std::size_t h = 0; h < hullPrims.size(); ++h)
            {
                Resource::CollisionPrim& p = c.setup.prims[hullPrims[h]];
                p.lib        = libName;
                p.sectionKey = keys[h];
                c.pending[hullPrims[h]].clear();
            }
        }
        c.setup.meshKey = m_meshKey;
        // An edited setup is the AUTHOR's: a re-import keeps it, where it would remake the
        // source's / the option's.
        c.setup.source.clear();
        std::string before, after, err;
        if (!SceneInstanceLoader::ReplaceModelCollision(m_model, { c.setup }, before, after, &err))
        {
            why = "the collision records were not written: " + err;
            return false;
        }
        rep.collision = true;
    }

    // ---- The shape-key records ----
    if (shapeKeysEdited)
    {
        Resource::MeshShapeKeySetup sk = m_staged.shapeKeys;
        sk.meshKey = m_meshKey;
        sk.orphan  = false;   // saved from the Mesh itself: it exists
        std::string before, after, err;
        if (!SceneInstanceLoader::ReplaceModelShapeKeys(m_model, { sk }, before, after, &err))
        {
            if (!rep.collision) { why = "the shape-key records were not written: " + err; return false; }
            why = "the collision was saved, the shape-key records were not: " + err;
            partial = true;
        }
        else
            rep.shapeKeys = true;
    }

    // ---- The Mesh's sockets (SO mesh=<key>) ----
    // This Mesh's records only: the skeleton sockets (the Skeleton Editor's) and every
    // other Mesh's stay as the file has them.
    if (socketsEdited)
    {
        std::vector<Resource::ModelSocket> list = m_staged.sockets;
        for (Resource::ModelSocket& so : list)
        {
            so.meshKey = m_meshKey;
            so.bone.clear();
        }
        std::string before, after, err;
        if (!SceneInstanceLoader::ReplaceMeshSockets(m_model, m_meshKey, list, before, after, &err))
        {
            if (!rep.collision && !rep.shapeKeys) { why = "the socket records were not written: " + err; return false; }
            why = "the collision / shape keys were saved, the socket records were not: " + err;
            partial = true;
        }
        else
            rep.sockets = true;
    }

    // ---- The default materials ----
    std::vector<SceneInstanceLoader::DefaultMaterialEdit> edits;
    for (std::size_t k = 0; k < m_sections.size(); ++k)
        if (m_staged.materials[k] != m_saved.materials[k] && m_sections[k].editable)
            edits.push_back({ m_libraryPath, m_sections[k].meta.entry, m_staged.materials[k], m_sections[k].meta.key });
    if (!edits.empty() && !SceneInstanceLoader::WriteDefaultMaterials(m_model, edits, &rep.lines, &why))
    {
        if (!rep.collision && !rep.shapeKeys && !rep.sockets) return false;   // nothing was written
        why = "the collision / shape keys were saved, the default materials were not: " + why;
        partial = true;
    }

    // Everything that drew the old state: the scene's StaticMeshes of this model
    // (re-derived through the model's revision), every preview composition of
    // it, its Content Browser tiles; the collision's shape caches and every
    // placement in the world.
    if (ctx.renderer)
    {
        if (MeshAssetRegistry* reg = ctx.renderer->GetMeshAssets()) reg->InvalidateModel(m_model);
        ctx.renderer->InvalidatePreviewAsset(m_ref);
        ctx.renderer->InvalidatePreviewAsset(m_model);
    }
    if (rep.collision && ctx.collisionChanged) rep.refreshed = ctx.collisionChanged(m_model);
    if (ctx.filesChanged) ctx.filesChanged({ m_model });

    // The file is the truth now: re-read what it says.
    Staged keep = m_staged;                   // what did not reach the file stays staged
    if (rep.collision) keep.collision = written.collision;
    std::string code, lwhy;
    const auto undo = m_undo;
    const auto redo = m_redo;
    const std::vector<bool> visible = m_visible;
    const int selected = m_selected, collisionSelected = m_collisionSelected;
    const std::string selSocket = m_selSocket;
    const std::uint32_t show = m_show;
    const float yaw = m_yaw, pitch = m_pitch, dist = m_dist;
    const float pan[3] = { m_pan[0], m_pan[1], m_pan[2] };
    const MeshShapeKeys::Snapshot preview = m_shapeKeys.Snap();   // the picture's weights survive
    const int weightsBone = m_weights.Bone();                      // and the weight heatmap's bone
    if (!LoadSubject(ctx, m_model, m_meshKey, m_meshName, code, lwhy))
    {
        why = "saved, but re-reading the model failed: " + lwhy;
        return false;
    }
    // Keep the session's view and history across the reload.
    m_undo = undo;
    m_redo = redo;
    if (partial) m_staged = keep;
    if (visible.size() == m_visible.size()) m_visible = visible;
    m_selected = selected < static_cast<int>(m_sections.size()) ? selected : -1;
    m_collisionSelected =
        collisionSelected < static_cast<int>(m_staged.collision.setup.prims.size()) ? collisionSelected : -1;
    m_show = show;
    m_yaw = yaw; m_pitch = pitch; m_dist = dist;
    std::memcpy(m_pan, pan, sizeof(m_pan));
    m_shapeKeys.Restore(preview);
    if (FindSocket(selSocket) >= 0) m_selSocket = selSocket;
    if (weightsBone >= 0) SetWeightsBone(weightsBone);
    if (out) *out = rep;
    LOG_INFO("[MESHEDITOR] save: model='%s' slots=%zu lines=%d collision=%d shapeKeys=%d prims=%zu hullsAppended=%u "
             "refreshed=%u matchesStaged=%d sockets=%d socketRecords=%zu", m_model.c_str(), edits.size(), rep.lines,
             rep.collision ? 1 : 0, rep.shapeKeys ? 1 : 0,
             m_saved.collision.setup.prims.size(), rep.hullsAppended, rep.refreshed,
             SameStaged(m_saved, written) ? 1 : 0, rep.sockets ? 1 : 0, m_saved.sockets.size());
    if (partial) return false;
    return true;
}

// ==== Mesh sockets: the Mesh's SO mesh=<key> records ====
int MeshEditorWindow::FindSocket(const std::string& name) const
{
    for (int i = 0; i < static_cast<int>(m_staged.sockets.size()); ++i)
        if (m_staged.sockets[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
}

bool MeshEditorWindow::SelectSocket(const std::string& name)
{
    if (name.empty()) { m_selSocket.clear(); return true; }
    if (FindSocket(name) < 0) return false;
    m_selSocket     = name;
    m_revealSockets = true;   // its section opens (it starts collapsed)
    return true;
}

std::string MeshEditorWindow::UniqueSocketName(const std::string& base) const
{
    if (FindSocket(base) < 0) return base;
    for (int k = 1; k < 10000; ++k)
    {
        const std::string suffix = "_" + std::to_string(k);
        const std::string n = base.substr(0, std::min<std::size_t>(base.size(), 63 - suffix.size())) + suffix;
        if (FindSocket(n) < 0) return n;
    }
    return base;
}

bool MeshEditorWindow::StageSockets(std::vector<Resource::ModelSocket> next, const char* what, std::string& why,
                                    bool pushUndo)
{
    if (!m_loaded) { why = "no mesh is open"; return false; }
    if (!m_socketLock.empty()) { why = m_socketLock; return false; }
    for (std::size_t i = 0; i < next.size(); ++i)
    {
        if (const std::string p = Resource::SocketNameProblem(next[i].name); !p.empty())
        {
            why = p + " ('" + next[i].name + "')";
            return false;
        }
        Resource::ModelSocket& s = next[i];
        const float v[] = { s.t.x, s.t.y, s.t.z, s.q.x, s.q.y, s.q.z, s.q.w, s.s.x, s.s.y, s.s.z };
        for (float f : v)
            if (!std::isfinite(f)) { why = "socket '" + s.name + "' has a value that is not a number"; return false; }
        for (std::size_t j = 0; j < i; ++j)
            if (next[j].name == next[i].name) { why = "two sockets are named '" + next[i].name + "'"; return false; }
        // The Skeleton Editor's SetSocket rules: a unit rotation, no zero scale.
        const float ql = std::sqrt(s.q.x * s.q.x + s.q.y * s.q.y + s.q.z * s.q.z + s.q.w * s.q.w);
        if (!(ql > 1e-6f)) { why = "socket '" + s.name + "': the rotation has zero length"; return false; }
        s.q = { s.q.x / ql, s.q.y / ql, s.q.z / ql, s.q.w / ql };
        if (s.s.x == 0.f || s.s.y == 0.f || s.s.z == 0.f)
        {
            why = "socket '" + s.name + "': a scale of 0 collapses it";
            return false;
        }
        s.meshKey = m_meshKey;   // this Mesh's, no bone
        s.bone.clear();
        // An edited (or new) socket is the author's: a re-import keeps it (src= is the
        // importer's, remade from the source); it exists, so it is no orphan.
        const int was = FindSocket(s.name);
        if (was < 0 || !(m_staged.sockets[static_cast<std::size_t>(was)] == s))
        {
            s.source.clear();
            s.orphan = false;
        }
    }
    if (next == m_staged.sockets) return true;   // no change, no undo step
    if (pushUndo) PushUndo();
    m_staged.sockets = std::move(next);
    LOG_INFO("[MESHEDITOR] sockets: %s - %zu staged, dirty=%d undo=%zu", what, m_staged.sockets.size(),
             SocketsDirty() ? 1 : 0, m_undo.size());
    return true;
}

bool MeshEditorWindow::AddSocket(const std::string& name, std::string& outName, std::string& why)
{
    const std::string base = name.empty() ? std::string("socket") : name;
    if (const std::string p = Resource::SocketNameProblem(base); !p.empty()) { why = p; return false; }
    if (!name.empty() && FindSocket(name) >= 0) { why = "a socket named '" + name + "' exists"; return false; }
    Resource::ModelSocket s;
    s.name    = UniqueSocketName(base);
    s.meshKey = m_meshKey;   // at the Mesh's pivot (its origin), unrotated
    std::vector<Resource::ModelSocket> next = m_staged.sockets;
    next.push_back(s);
    if (!StageSockets(std::move(next), ("add '" + s.name + "'").c_str(), why)) return false;
    outName         = s.name;
    m_selSocket     = s.name;
    m_revealSockets = true;
    return true;
}

bool MeshEditorWindow::SetSocket(const std::string& name, const Resource::ModelSocket& value, std::string& why)
{
    const int i = FindSocket(name);
    if (i < 0) { why = "no socket named '" + name + "'"; return false; }
    std::vector<Resource::ModelSocket> next = m_staged.sockets;
    next[static_cast<std::size_t>(i)] = value;
    if (!StageSockets(std::move(next), value.name != name ? ("rename '" + name + "' -> '" + value.name + "'").c_str()
                                                          : ("edit '" + name + "'").c_str(), why))
        return false;
    if (m_selSocket == name) m_selSocket = value.name;
    return true;
}

bool MeshEditorWindow::RemoveSocket(const std::string& name, std::string& why)
{
    const int i = FindSocket(name);
    if (i < 0) { why = "no socket named '" + name + "'"; return false; }
    std::vector<Resource::ModelSocket> next = m_staged.sockets;
    next.erase(next.begin() + i);
    if (!StageSockets(std::move(next), ("remove '" + name + "'").c_str(), why)) return false;
    if (m_selSocket == name) m_selSocket.clear();
    return true;
}

void MeshEditorWindow::DrawSocketDetails()
{
    char header[96];
    std::snprintf(header, sizeof(header), "Sockets (%zu)%s###me_sockets", m_staged.sockets.size(),
                  SocketsDirty() ? " *" : "");
    if (m_revealSockets) { ImGui::SetNextItemOpen(true); m_revealSockets = false; }
    if (!ImGui::CollapsingHeader(header)) return;   // collapsed by default (the packs' layout)
    if (!m_socketLock.empty()) ImGui::TextDisabled("%s", m_socketLock.c_str());
    // The picture's Show > Sockets, here as in the Collision section (off by default).
    bool shown = (m_show & kShowSockets) != 0;
    if (ImGui::Checkbox("Show in the view##me_sock_show", &shown))
        m_show = shown ? (m_show | kShowSockets) : (m_show & ~kShowSockets);
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_socketLock.empty());
    if (ImGui::SmallButton("+ Add Socket"))
    {
        std::string name, why;
        if (!AddSocket("", name, why)) m_error = why;
        else m_show |= kShowSockets;   // a socket just added is drawn
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Named attachment points in this Mesh's space (the picture's): a Static Mesh\n"
                          "of it publishes them - Engine.EquipToSocket, VFX and Follow Socket reach them.\n"
                          "Save writes this Mesh's SO records; a model's skeleton sockets are the\n"
                          "Skeleton Editor's.");
    for (int i = 0; i < static_cast<int>(m_staged.sockets.size()); ++i)
    {
        const Resource::ModelSocket& s = m_staged.sockets[static_cast<std::size_t>(i)];
        ImGui::PushID(0x5E000 + i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kColSocket));
        if (ImGui::Selectable(("<> " + s.name).c_str(), s.name == m_selSocket))
            m_selSocket = (s.name == m_selSocket) ? std::string() : s.name;
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    const int sel = SelectedSocket();
    if (sel < 0) return;
    const Resource::ModelSocket cur = m_staged.sockets[static_cast<std::size_t>(sel)];
    ImGui::Separator();
    ImGui::BeginDisabled(!m_socketLock.empty());
    // Name: Enter renames (one step). Seeded per socket NAME: an index could name
    // another socket after an add / remove / revert.
    if (m_socketNameFor != cur.name)
    {
        strncpy_s(m_socketNameEdit, sizeof(m_socketNameEdit), cur.name.c_str(), _TRUNCATE);
        m_socketNameFor = cur.name;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##me_sock_name", m_socketNameEdit, sizeof(m_socketNameEdit),
                         ImGuiInputTextFlags_EnterReturnsTrue) && cur.name != m_socketNameEdit)
    {
        Resource::ModelSocket n = cur;
        n.name = m_socketNameEdit;
        std::string why;
        if (!SetSocket(cur.name, n, why)) m_error = why;
        m_socketNameFor.clear();
    }
    // T / R (RollPitchYaw degrees, the Inspector's order) / S: one widget gesture = one step.
    // The degrees are kept while this socket's rotation is the one they were made from.
    const bool sameQ = m_socketEulerQ.x == cur.q.x && m_socketEulerQ.y == cur.q.y && m_socketEulerQ.z == cur.q.z &&
                       m_socketEulerQ.w == cur.q.w;
    if (m_socketEulerFor != cur.name || !sameQ)
    {
        const XMFLOAT3 e = MathUtils::RollPitchYawDegreesFromQuaternion(cur.q);
        m_socketEuler[0] = e.x; m_socketEuler[1] = e.y; m_socketEuler[2] = e.z;
        m_socketEulerFor = cur.name;
        m_socketEulerQ   = cur.q;
    }
    Resource::ModelSocket n = cur;
    bool edited = false;
    float t[3] = { cur.t.x, cur.t.y, cur.t.z }, sc[3] = { cur.s.x, cur.s.y, cur.s.z };
    if (ImGui::DragFloat3("Position", t, 0.005f)) { n.t = { t[0], t[1], t[2] }; edited = true; }
    if (ImGui::DragFloat3("Rotation", m_socketEuler, 0.5f, -360.f, 360.f, "%.1f"))
    {
        XMStoreFloat4(&n.q, XMQuaternionRotationRollPitchYaw(XMConvertToRadians(m_socketEuler[0]),
                                                             XMConvertToRadians(m_socketEuler[1]),
                                                             XMConvertToRadians(m_socketEuler[2])));
        edited = true;
    }
    if (ImGui::DragFloat3("Scale", sc, 0.01f, 0.001f, 1000.f)) { n.s = { sc[0], sc[1], sc[2] }; edited = true; }
    if (edited)
    {
        std::string why;
        if (!StageSockets([&] { auto v = m_staged.sockets; v[static_cast<std::size_t>(sel)] = n; return v; }(),
                          ("edit '" + cur.name + "'").c_str(), why, /*pushUndo=*/!m_socketDragPushed))
            m_error = why;
        else
        {
            const int now = FindSocket(n.name);   // StageSockets normalised the rotation
            if (now >= 0) m_socketEulerQ = m_staged.sockets[static_cast<std::size_t>(now)].q;
        }
        m_socketDragPushed = true;
    }
    if (!ImGui::IsAnyItemActive()) m_socketDragPushed = false;   // the gesture ended
    if (ImGui::SmallButton("Remove Socket"))
    {
        std::string why;
        if (!RemoveSocket(cur.name, why)) m_error = why;
    }
    ImGui::EndDisabled();
}

// ==== The skeleton and its weights ====
int MeshEditorWindow::BoneIndex(const std::string& name) const
{
    for (int b = 0; b < static_cast<int>(m_boneNames.size()); ++b)
        if (m_boneNames[static_cast<std::size_t>(b)] == name) return b;
    return -1;
}

bool MeshEditorWindow::SetWeightsBone(int bone)
{
    if (bone < -1 || bone >= static_cast<int>(m_boneNames.size())) return false;
    if (bone >= 0 && m_weights.Parts() == 0) return false;   // nothing of this Mesh pairs with its weights
    m_weights.SetBone(bone);
    if (bone >= 0) m_revealSkeleton = true;   // its section opens (it starts collapsed)
    LOG_INFO("[MESHEDITOR] weights: bone %d '%s' (%zu part(s))", bone,
             bone >= 0 ? m_boneNames[static_cast<std::size_t>(bone)].c_str() : "off", m_weights.Parts());
    return true;
}

void MeshEditorWindow::DrawSkeletonDetails(const MeshEditorContext& /*ctx*/)
{
    if (!Skinned() || m_skeletonPath.empty() || m_boneNames.empty()) return;
    char header[96];
    std::snprintf(header, sizeof(header), "Skeleton (%zu bones move it)###me_skeleton", m_skinBones.size());
    if (m_revealSkeleton) { ImGui::SetNextItemOpen(true); m_revealSkeleton = false; }
    if (!ImGui::CollapsingHeader(header)) return;   // collapsed by default (the packs' layout)
    if (ImGui::SmallButton("Open in Skeleton Editor##me_skel_open")) SkeletonEditorWindow::Get().RequestOpen(m_model);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_weights.Bone() < 0);
    if (ImGui::SmallButton("Weights off")) SetWeightsBone(-1);
    ImGui::EndDisabled();
    if (m_weights.Parts() == 0)
        ImGui::TextDisabled("No section of this Mesh pairs with the skeleton's weights (no F-line places it).");
    else
        ImGui::TextDisabled("Click a bone: its weights on the picture (dark blue = none ... red = only it).");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##me_bone_filter", "filter bones...", m_boneFilter, sizeof(m_boneFilter));
    std::string f = m_boneFilter;
    for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    ImGui::BeginChild("##me_bones", ImVec2(0.f, 180.f), true);
    for (const SkinBone& b : m_skinBones)
    {
        if (!f.empty())
        {
            std::string n = b.name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.find(f) == std::string::npos) continue;
        }
        char label[128];
        std::snprintf(label, sizeof(label), "%s   (%u)##b%d", b.name.c_str(), b.vertices, b.bone);
        if (ImGui::Selectable(label, m_weights.Bone() == b.bone))
            SetWeightsBone(m_weights.Bone() == b.bone ? -1 : b.bone);
    }
    ImGui::EndChild();
}

// ==== Collision ====
const char* MeshEditorWindow::CollisionAddName(CollisionAdd k)
{
    for (const AddEntry& a : kAdds)
        if (a.kind == k) return a.name;
    return "?";
}

bool MeshEditorWindow::ParseCollisionAdd(const std::string& s, CollisionAdd& out)
{
    const std::string l = LowerAscii(s);
    for (const AddEntry& a : kAdds)
        if (l == a.name) { out = a.kind; return true; }
    return false;
}

std::string MeshEditorWindow::CollisionLock() const
{
    return m_loaded ? m_collisionLock : std::string("no mesh is open");
}

// The vertices a fit reads: one section's, or every section's (-1).
std::vector<XMFLOAT3> MeshEditorWindow::FitPoints(int section) const
{
    std::vector<XMFLOAT3> pts;
    for (std::size_t k = 0; k < m_sections.size(); ++k)
    {
        if (section >= 0 && static_cast<int>(k) != section) continue;
        const Section& s = m_sections[k];
        for (std::size_t i = 0; i + 2 < s.pos.size(); i += 3) pts.push_back({ s.pos[i], s.pos[i + 1], s.pos[i + 2] });
    }
    return pts;
}

bool MeshEditorWindow::AddCollision(CollisionAdd kind, int section, std::string& why, int* outIndex)
{
    if (outIndex) *outIndex = -1;
    why = CollisionLock();
    if (!why.empty()) return false;
    if (section < -1 || section >= static_cast<int>(m_sections.size()))
    {
        why = "no section " + std::to_string(section);
        return false;
    }
    const std::vector<XMFLOAT3> pts = FitPoints(section);
    if (pts.empty())
    {
        why = "no vertices to fit";
        return false;
    }
    namespace CF = Physics::CollisionFit;
    Resource::CollisionPrim prim;
    std::vector<XMFLOAT3>   hull;
    std::string             err;
    bool                    ok = false;
    switch (kind)
    {
    case CollisionAdd::Box:     ok = CF::FitBox(pts, prim); break;
    case CollisionAdd::Sphere:  ok = CF::FitSphere(pts, prim); break;
    case CollisionAdd::Capsule: ok = CF::FitCapsule(pts, prim); break;
    case CollisionAdd::Convex:  ok = CF::ConvexHull(pts, hull, CF::kDefaultHullVertices, &err); break;
    default:
    {
        const CF::KDop k = kind == CollisionAdd::KDop10X ? CF::KDop::X10
                         : kind == CollisionAdd::KDop10Y ? CF::KDop::Y10
                         : kind == CollisionAdd::KDop10Z ? CF::KDop::Z10
                         : kind == CollisionAdd::KDop18  ? CF::KDop::K18 : CF::KDop::K26;
        std::vector<XMFLOAT3> corners;
        // The corners through the hull builder too: the vertices Jolt keeps.
        ok = CF::FitKDop(pts, k, corners, &err) && CF::ConvexHull(corners, hull, 256, &err);
        break;
    }
    }
    if (!ok)
    {
        why = err.empty() ? std::string("the fit failed") : err;
        return false;
    }
    if (!hull.empty())
    {
        prim      = Resource::CollisionPrim{};
        prim.type = Resource::CollisionPrim::Type::Hull;   // lib / sec come with Save
    }
    PushUndo();
    StagedCollision& c = m_staged.collision;
    c.setup.meshKey = m_meshKey;
    c.setup.prims.push_back(prim);
    c.pending.push_back(std::move(hull));
    m_collisionSelected = static_cast<int>(c.setup.prims.size()) - 1;
    m_show |= kShowCollision;
    if (outIndex) *outIndex = m_collisionSelected;
    LOG_INFO("[MESHEDITOR] collision add: kind=%s section=%d fitPoints=%zu prim=%d type=%s hullPoints=%zu dirty=%d",
             CollisionAddName(kind), section, pts.size(), m_collisionSelected, PrimTypeName(prim.type),
             c.pending.back().size(), Dirty() ? 1 : 0);
    return true;
}

bool MeshEditorWindow::DuplicateCollision(int index, std::string& why, int* outIndex)
{
    if (outIndex) *outIndex = -1;
    why = CollisionLock();
    if (!why.empty()) return false;
    StagedCollision& c = m_staged.collision;
    if (index < 0 || index >= static_cast<int>(c.setup.prims.size()))
    {
        why = "no collision prim " + std::to_string(index);
        return false;
    }
    PushUndo();
    const Resource::CollisionPrim p    = c.setup.prims[static_cast<std::size_t>(index)];
    const std::vector<XMFLOAT3>   pend = c.pending[static_cast<std::size_t>(index)];
    c.setup.prims.push_back(p);
    c.pending.push_back(pend);
    m_collisionSelected = static_cast<int>(c.setup.prims.size()) - 1;
    if (outIndex) *outIndex = m_collisionSelected;
    LOG_INFO("[MESHEDITOR] collision duplicate: prim=%d -> %d", index, m_collisionSelected);
    return true;
}

bool MeshEditorWindow::RemoveCollision(int index, std::string& why)
{
    why = CollisionLock();
    if (!why.empty()) return false;
    StagedCollision& c = m_staged.collision;
    if (index == -1)
    {
        if (c.setup.prims.empty()) { why = "the Mesh has no collision prims"; return false; }
        PushUndo();
        c.setup.prims.clear();
        c.pending.clear();
        m_collisionSelected = -1;
        LOG_INFO("[MESHEDITOR] collision remove: all");
        return true;
    }
    if (index < 0 || index >= static_cast<int>(c.setup.prims.size()))
    {
        why = "no collision prim " + std::to_string(index);
        return false;
    }
    PushUndo();
    c.setup.prims.erase(c.setup.prims.begin() + index);
    c.pending.erase(c.pending.begin() + index);
    if (m_collisionSelected == index) m_collisionSelected = -1;
    else if (m_collisionSelected > index) --m_collisionSelected;
    LOG_INFO("[MESHEDITOR] collision remove: prim=%d left=%zu", index, c.setup.prims.size());
    return true;
}

bool MeshEditorWindow::StageCollision(const Resource::MeshBodySetup& setup, std::string& why)
{
    why = CollisionLock();
    if (!why.empty()) return false;
    const Resource::MeshBodySetup& cur = m_staged.collision.setup;
    if (setup.prims.size() != cur.prims.size())
    {
        why = "the prims are added and removed with MeshEditorCollisionAdd / MeshEditorCollisionRemove";
        return false;
    }
    Resource::MeshBodySetup next = setup;
    next.meshKey = m_meshKey;
    if (next.layer > static_cast<std::uint8_t>(ColliderComponent::CollisionLayer::Sensing))
    {
        why = "layer is 0..8 (Auto, StaticEnv, DynamicProp, Character, Ragdoll, HurtBox, AttackBox, Projectile, Sensing)";
        return false;
    }
    if (!std::isfinite(next.friction) || !std::isfinite(next.restitution))
    {
        why = "friction / restitution must be numbers (negative = the RigidBody default)";
        return false;
    }
    for (std::size_t i = 0; i < next.prims.size(); ++i)
    {
        Resource::CollisionPrim&       p = next.prims[i];
        const Resource::CollisionPrim& o = cur.prims[i];
        const std::string at = "prim " + std::to_string(i) + ": ";
        if (p.type != o.type || p.lib != o.lib || p.sectionKey != o.sectionKey)
        {
            why = at + "its type and source stay (remove it and add another)";
            return false;
        }
        if (!Finite3(p.center) || !std::isfinite(p.rotation.x) || !std::isfinite(p.rotation.y) ||
            !std::isfinite(p.rotation.z) || !std::isfinite(p.rotation.w))
        {
            why = at + "a value is not a number";
            return false;
        }
        const XMVECTOR q = XMLoadFloat4(&p.rotation);
        if (XMVectorGetX(XMVector4LengthSq(q)) < 1e-12f)
        {
            why = at + "the rotation is not a rotation (zero quaternion)";
            return false;
        }
        XMStoreFloat4(&p.rotation, XMQuaternionNormalize(q));
        const bool bad = (p.type == Resource::CollisionPrim::Type::Box &&
                          !(p.halfExtents.x > 0.f && p.halfExtents.y > 0.f && p.halfExtents.z > 0.f &&
                            Finite3(p.halfExtents))) ||
                         ((p.type == Resource::CollisionPrim::Type::Sphere ||
                           p.type == Resource::CollisionPrim::Type::Capsule) &&
                          !(p.radius > 0.f && std::isfinite(p.radius))) ||
                         (p.type == Resource::CollisionPrim::Type::Capsule &&
                          !(p.halfHeight > 0.f && std::isfinite(p.halfHeight)));
        if (bad)
        {
            why = at + "a size must be positive";
            return false;
        }
    }
    if (Resource::FormatBodySetup(next) == Resource::FormatBodySetup(cur)) return true;   // nothing to stage
    PushUndo();
    m_staged.collision.setup = std::move(next);
    LOG_INFO("[MESHEDITOR] collision stage: complexity=%d layer=%u trigger=%d prims=%zu dirty=%d",
             static_cast<int>(m_staged.collision.setup.complexity), m_staged.collision.setup.layer,
             m_staged.collision.setup.trigger ? 1 : 0, m_staged.collision.setup.prims.size(), Dirty() ? 1 : 0);
    return true;
}

bool MeshEditorWindow::SelectCollision(int index)
{
    if (!m_loaded || index < -1 || index >= static_cast<int>(m_staged.collision.setup.prims.size())) return false;
    m_collisionSelected = index;
    return true;
}

MeshEditorWindow::CollisionState MeshEditorWindow::Collision()
{
    EnsureCollisionShapes();
    CollisionState s;
    s.lock    = CollisionLock();
    s.library = m_collisionLibrary;
    s.staged  = m_staged.collision.setup;
    s.saved   = m_saved.collision.setup;
    for (const auto& p : m_staged.collision.pending) s.pending.push_back(static_cast<std::uint32_t>(p.size()));
    for (std::size_t i = 0; i < s.staged.prims.size(); ++i)
        s.shown.push_back(i < m_primShapes.size() ? static_cast<std::uint32_t>(m_primShapes[i].pts.size() / 3) : 0u);
    s.selected     = m_collisionSelected;
    s.dirty        = m_loaded && CollisionDirty();
    s.overlayLines = m_collisionLines;
    return s;
}

// What the overlay draws of each staged prim that is not analytic: a hull's
// edges (its pending points, else its library entry's), a trimesh prim's
// triangles (the Mesh's own section, else the named library's entry), and for
// complexity=complex every section's triangles. Reads files: GAME half only
// (ProcessPendingActions, the scripted surface) - Draw only reads the result.
void MeshEditorWindow::EnsureCollisionShapes()
{
    const StagedCollision& c = m_staged.collision;
    std::uint64_t key = 14695981039346656037ull;
    const std::string text = Resource::FormatBodySetup(c.setup);
    key = Fnv(key, text.data(), text.size());
    key = Fnv(key, m_model.data(), m_model.size());
    for (const auto& p : c.pending)
    {
        const std::uint64_t n = p.size();
        key = Fnv(key, &n, sizeof(n));
        if (!p.empty()) key = Fnv(key, p.data(), p.size() * sizeof(XMFLOAT3));
    }
    if (key == m_primShapesKey && m_primShapes.size() == c.setup.prims.size()) return;
    m_primShapesKey = key;
    m_primShapes.assign(c.setup.prims.size(), PrimShape{});
    m_complexShape = PrimShape{};

    constexpr std::uint32_t kMaxShapeTriangles = 6000;   // per shape; sampled past it
    auto triangles = [&](PrimShape& out, const float* pos, std::size_t verts, const std::vector<std::uint32_t>& idx)
    {
        const std::uint32_t base = static_cast<std::uint32_t>(out.pts.size() / 3);
        out.pts.insert(out.pts.end(), pos, pos + verts * 3);
        const std::size_t tris = idx.size() / 3;
        const std::size_t step = std::max<std::size_t>(1, (tris + kMaxShapeTriangles - 1) / kMaxShapeTriangles);
        for (std::size_t t = 0; t < tris; t += step)
        {
            const std::uint32_t a = idx[t * 3], b = idx[t * 3 + 1], d = idx[t * 3 + 2];
            if (a >= verts || b >= verts || d >= verts) continue;
            out.lines.insert(out.lines.end(), { base + a, base + b, base + b, base + d, base + d, base + a });
        }
    };
    std::map<std::string, std::unique_ptr<MeshGeometry::Library>> libs;   // read once per rebuild
    auto entryOf = [&](const std::string& lib, std::uint64_t sectionKey, MeshGeometry::Section& out) -> bool
    {
        const std::string path = Resource::ResolveModelRelative(m_model, lib);
        auto it = libs.find(path);
        if (it == libs.end())
        {
            auto L = std::make_unique<MeshGeometry::Library>();
            std::string why;
            if (!MeshGeometry::ReadLibrary(path, *L, why))
            {
                LOG_WARNING("[MESHEDITOR] collision: '%s' does not read (%s)", path.c_str(), why.c_str());
                L.reset();
            }
            it = libs.emplace(path, std::move(L)).first;
        }
        if (!it->second) return false;
        const int e = it->second->id.FindSection(sectionKey);
        std::string why;
        return e >= 0 && MeshGeometry::DecodeSection(*it->second, static_cast<std::uint32_t>(e), out, why);
    };

    for (std::size_t i = 0; i < c.setup.prims.size(); ++i)
    {
        const Resource::CollisionPrim& p = c.setup.prims[i];
        PrimShape&                     s = m_primShapes[i];
        if (p.type == Resource::CollisionPrim::Type::Hull)
        {
            std::vector<XMFLOAT3> pts = i < c.pending.size() ? c.pending[i] : std::vector<XMFLOAT3>{};
            MeshGeometry::Section g;
            if (pts.empty() && entryOf(p.lib, p.sectionKey, g)) pts = std::move(g.positions);
            std::vector<XMFLOAT3>      hull;
            std::vector<std::uint32_t> edges;
            if (!pts.empty() && Physics::CollisionFit::ConvexHull(pts, hull, 256, nullptr, &edges))
            {
                for (const XMFLOAT3& v : hull) s.pts.insert(s.pts.end(), { v.x, v.y, v.z });
                s.lines = std::move(edges);
            }
        }
        else if (p.type == Resource::CollisionPrim::Type::Mesh)
        {
            if (p.lib.empty())
            {
                for (const Section& sec : m_sections)
                    if (sec.meta.key == p.sectionKey) triangles(s, sec.pos.data(), sec.pos.size() / 3, sec.idx);
            }
            else if (MeshGeometry::Section g; entryOf(p.lib, p.sectionKey, g))
                triangles(s, reinterpret_cast<const float*>(g.positions.data()), g.positions.size(), g.indices);
        }
    }
    if (c.setup.complexity == Resource::MeshBodySetup::Complexity::Complex)
        for (const Section& sec : m_sections) triangles(m_complexShape, sec.pos.data(), sec.pos.size() / 3, sec.idx);
}

// ==== The collision gizmo ====
const char* MeshEditorWindow::GizmoOpName(GizmoOp op)
{
    switch (op)
    {
    case GizmoOp::Off:       return "off";
    case GizmoOp::Translate: return "translate";
    case GizmoOp::Rotate:    return "rotate";
    case GizmoOp::Scale:     return "scale";
    }
    return "?";
}

bool MeshEditorWindow::ParseGizmoOp(const std::string& s, GizmoOp& out)
{
    const std::string l = LowerAscii(s);
    for (GizmoOp op : { GizmoOp::Off, GizmoOp::Translate, GizmoOp::Rotate, GizmoOp::Scale })
        if (l == GizmoOpName(op)) { out = op; return true; }
    return false;
}

void MeshEditorWindow::PrimDisplayMatrix(const Resource::CollisionPrim& p, const float centre[3], float scale,
                                         XMFLOAT4X4& out)
{
    XMFLOAT3 size = { 1.f, 1.f, 1.f };
    switch (p.type)
    {
    case Resource::CollisionPrim::Type::Box:     size = p.halfExtents; break;
    case Resource::CollisionPrim::Type::Sphere:  size = { p.radius, p.radius, p.radius }; break;
    case Resource::CollisionPrim::Type::Capsule: size = { p.radius, p.halfHeight, p.radius }; break;
    default: break;
    }
    const XMVECTOR q = p.type == Resource::CollisionPrim::Type::Sphere
                     ? XMQuaternionIdentity() : XMQuaternionNormalize(XMLoadFloat4(&p.rotation));
    const XMMATRIX prim = XMMatrixMultiply(XMMatrixMultiply(XMMatrixScaling(size.x, size.y, size.z),
                                                            XMMatrixRotationQuaternion(q)),
                                           XMMatrixTranslation(p.center.x, p.center.y, p.center.z));
    const XMMATRIX fit = XMMatrixMultiply(XMMatrixTranslation(-centre[0], -centre[1], -centre[2]),
                                          XMMatrixScaling(scale, scale, scale));
    XMStoreFloat4x4(&out, XMMatrixMultiply(prim, fit));
}

bool MeshEditorWindow::ApplyGizmoMatrix(int index, GizmoOp op, const XMFLOAT4X4& display, const float centre[3],
                                        float scale)
{
    StagedCollision& c = m_staged.collision;
    if (index < 0 || index >= static_cast<int>(c.setup.prims.size()) || !(scale > 0.f)) return false;
    Resource::CollisionPrim& p = c.setup.prims[static_cast<std::size_t>(index)];
    // Back out of the fitted space (a uniform scale and a translation: exact).
    const float inv = 1.f / scale;
    const XMMATRIX unfit = XMMatrixMultiply(XMMatrixScaling(inv, inv, inv),
                                            XMMatrixTranslation(centre[0], centre[1], centre[2]));
    XMVECTOR s, q, t;
    if (!XMMatrixDecompose(&s, &q, &t, XMMatrixMultiply(XMLoadFloat4x4(&display), unfit))) return false;
    XMFLOAT3 S, T;
    XMStoreFloat3(&S, s);
    XMStoreFloat3(&T, t);
    if (!Finite3(S) || !Finite3(T)) return false;
    constexpr float kMin = Physics::CollisionFit::kMinHalfThickness;
    using Ty = Resource::CollisionPrim::Type;
    switch (op)
    {
    case GizmoOp::Translate:
        p.center = T;
        return true;
    case GizmoOp::Rotate:
        if (p.type == Ty::Sphere) return false;   // a sphere has no orientation
        p.center = T;
        XMStoreFloat4(&p.rotation, XMQuaternionNormalize(q));
        return true;
    case GizmoOp::Scale:
        switch (p.type)
        {
        case Ty::Box:   // a face drag moves the centre too (the opposite face stays)
            p.halfExtents = { (std::max)(S.x, kMin), (std::max)(S.y, kMin), (std::max)(S.z, kMin) };
            p.center      = T;
            return true;
        case Ty::Sphere:
        {
            // One handle moves one axis: the one that moved furthest is the radius.
            float r = S.x;
            for (const float v : { S.y, S.z })
                if (std::fabs(v - p.radius) > std::fabs(r - p.radius)) r = v;
            p.radius = (std::max)(r, kMin);
            p.center = T;
            return true;
        }
        case Ty::Capsule:
            p.radius     = (std::max)(std::fabs(S.x - p.radius) >= std::fabs(S.z - p.radius) ? S.x : S.z, kMin);
            p.halfHeight = (std::max)(S.y, kMin);
            p.center     = T;
            return true;
        default:
            return false;   // a hull's points are fixed; a trimesh has no gizmo
        }
    case GizmoOp::Off:
        break;
    }
    return false;
}

// Every path that ends our drag from this side (the gizmo vanished mid-drag, the
// panel stopped drawing) hands ImGuizmo's global mbUsing / editing ID back, or
// every other gizmo stays dead (the Chain Preview's ReleasePoseGesture).
void MeshEditorWindow::ReleaseGizmo()
{
    if (!m_gizmoUsing) return;
    ImGuizmo::Enable(false);
    ImGuizmo::Enable(true);
    m_gizmoUsing  = false;
    m_gizmoPushed = false;
}

void MeshEditorWindow::DrawCollisionGizmo(Renderer* renderer, float x, float y, float w, float h)
{
    m_gizmoOver = false;
    bool submitted = false;
    const StagedCollision& c = m_staged.collision;
    const int i = m_collisionSelected;
    float centre[3] = { 0.f, 0.f, 0.f }, scale = 1.f;
    const bool want = m_gizmoOp != GizmoOp::Off && m_collisionLock.empty() && (m_show & kShowCollision) != 0 &&
                      i >= 0 && i < static_cast<int>(c.setup.prims.size()) &&
                      c.setup.prims[static_cast<std::size_t>(i)].type != Resource::CollisionPrim::Type::Mesh &&
                      renderer && w > 1.f && h > 1.f && renderer->GetPreviewAssetFit(m_ref, centre, scale);
    if (want)
    {
        using Ty = Resource::CollisionPrim::Type;
        const Resource::CollisionPrim& p = c.setup.prims[static_cast<std::size_t>(i)];
        GizmoOp op = m_gizmoOp;
        if (op == GizmoOp::Scale && p.type == Ty::Hull) op = GizmoOp::Translate;   // a hull moves and turns
        if (op == GizmoOp::Rotate && p.type == Ty::Sphere) op = GizmoOp::Translate;
        // Freeze the fed matrix while hovered / held (the main gizmo's rule): the
        // round trip through the staged prim would drift the hit test.
        if (m_gizmoPrim != i || (!m_gizmoUsing && !m_gizmoOver && !ImGuizmo::IsUsingAny()))
        {
            m_gizmoPrim = i;
            PrimDisplayMatrix(p, centre, scale, m_gizmoMatrix);
        }
        const Cam cam = BuildCam(m_yaw, m_pitch, m_dist, m_pan, w / h);
        XMFLOAT4X4 viewF, projF;
        XMStoreFloat4x4(&viewF, cam.view);
        XMStoreFloat4x4(&projF, cam.proj);
        float matrix[16];
        std::memcpy(matrix, &m_gizmoMatrix, sizeof(matrix));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
        // All four, every call (the main viewport's gizmo sets them too, and
        // ImGuizmo::BeginFrame resets the draw list).
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(x, y, w, h);
        ImGuizmo::SetDrawlist(dl);
        ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());
        ImGuizmo::PushID(kCollisionGizmoId);
        static const float kUnitBounds[6] = { -1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
        const bool bounds = op == GizmoOp::Scale && p.type == Ty::Box;
        const ImGuizmo::OPERATION gop = op == GizmoOp::Translate ? ImGuizmo::TRANSLATE
                                      : op == GizmoOp::Rotate    ? ImGuizmo::ROTATE
                                      : bounds                   ? ImGuizmo::BOUNDS : ImGuizmo::SCALE;
        const bool manipulated = ImGuizmo::Manipulate(reinterpret_cast<const float*>(&viewF),
                                                      reinterpret_cast<const float*>(&projF), gop,
                                                      op == GizmoOp::Translate ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                                                      matrix, nullptr, nullptr, bounds ? kUnitBounds : nullptr);
        // ID-scoped queries (IsUsing compares the current ID): inside PushID.
        const bool usingNow = ImGuizmo::IsUsing();
        m_gizmoOver = ImGuizmo::IsOver() || usingNow;
        ImGuizmo::PopID();
        dl->PopClipRect();
        submitted = true;
        if (manipulated && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            XMFLOAT4X4 result;
            std::memcpy(&result, matrix, sizeof(result));
            // One drag, one undo step: the staged state before its first change.
            std::optional<Staged> before;
            if (!m_gizmoPushed) before = m_staged;
            if (ApplyGizmoMatrix(i, op, result, centre, scale))
            {
                if (before)
                {
                    m_undo.push_back(std::move(*before));
                    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
                    m_redo.clear();
                    m_gizmoPushed = true;
                    ++m_gizmoGestures;
                }
                m_gizmoMatrix = result;
            }
        }
        m_gizmoUsing = usingNow;
        if (!usingNow) m_gizmoPushed = false;
    }
    if (!submitted && m_gizmoUsing) ReleaseGizmo();   // a drag whose gizmo vanished
}

bool MeshEditorWindow::GizmoDrag(int index, GizmoOp op, const XMFLOAT3& v, float deg, std::string& why)
{
    why = CollisionLock();
    if (!why.empty()) return false;
    StagedCollision& c = m_staged.collision;
    if (index < 0 || index >= static_cast<int>(c.setup.prims.size()))
    {
        why = "no collision prim " + std::to_string(index);
        return false;
    }
    using Ty = Resource::CollisionPrim::Type;
    const Resource::CollisionPrim& p = c.setup.prims[static_cast<std::size_t>(index)];
    if (p.type == Ty::Mesh) { why = "a trimesh prim has no gizmo"; return false; }
    if (op == GizmoOp::Off) { why = "no operation"; return false; }
    if (op == GizmoOp::Scale && p.type == Ty::Hull) { why = "a hull's points are fixed: it only moves and turns"; return false; }
    if (op == GizmoOp::Rotate && p.type == Ty::Sphere) { why = "a sphere has no orientation"; return false; }
    if (!Finite3(v) || !std::isfinite(deg)) { why = "a value is not a number"; return false; }

    // The picture's fit: the Mesh's bounds, as the preview frames it (any fit
    // round-trips exactly - it is what the gizmo's matrix goes through).
    const float centre[3] = { 0.5f * (m_boundsMin[0] + m_boundsMax[0]), 0.5f * (m_boundsMin[1] + m_boundsMax[1]),
                              0.5f * (m_boundsMin[2] + m_boundsMax[2]) };
    const float extent = (std::max)({ m_boundsMax[0] - m_boundsMin[0], m_boundsMax[1] - m_boundsMin[1],
                                      m_boundsMax[2] - m_boundsMin[2], 1e-3f });
    const float scale = 1.f / extent;
    XMFLOAT4X4 g;
    PrimDisplayMatrix(p, centre, scale, g);
    XMMATRIX G = XMLoadFloat4x4(&g);
    const XMMATRIX fit = XMMatrixMultiply(XMMatrixTranslation(-centre[0], -centre[1], -centre[2]),
                                          XMMatrixScaling(scale, scale, scale));
    switch (op)
    {
    case GizmoOp::Translate:   // world axes, in the picture's units
        G = XMMatrixMultiply(G, XMMatrixTranslation(v.x * scale, v.y * scale, v.z * scale));
        break;
    case GizmoOp::Rotate:      // about a LOCAL axis, the sizes kept apart (ImGuizmo's local rotation)
    case GizmoOp::Scale:
    {
        XMFLOAT3 size = { 1.f, 1.f, 1.f };
        if (p.type == Ty::Box) size = p.halfExtents;
        else if (p.type == Ty::Sphere) size = { p.radius, p.radius, p.radius };
        else if (p.type == Ty::Capsule) size = { p.radius, p.halfHeight, p.radius };
        XMMATRIX local = XMMatrixScaling(size.x, size.y, size.z);
        if (op == GizmoOp::Rotate)
        {
            const XMVECTOR axis = XMVector3Normalize(XMVectorSet(v.x, v.y, v.z, 0.f));
            if (XMVector3Equal(axis, XMVectorZero()) || XMVector3IsNaN(axis)) { why = "the axis is zero"; return false; }
            local = XMMatrixMultiply(local, XMMatrixRotationAxis(axis, XMConvertToRadians(deg)));
        }
        else
        {
            if (!(v.x > 0.f && v.y > 0.f && v.z > 0.f)) { why = "scale factors must be positive"; return false; }
            local = XMMatrixMultiply(local, XMMatrixScaling(v.x, v.y, v.z));
        }
        const XMVECTOR q = p.type == Ty::Sphere ? XMQuaternionIdentity() : XMQuaternionNormalize(XMLoadFloat4(&p.rotation));
        G = XMMatrixMultiply(XMMatrixMultiply(XMMatrixMultiply(local, XMMatrixRotationQuaternion(q)),
                                              XMMatrixTranslation(p.center.x, p.center.y, p.center.z)), fit);
        break;
    }
    case GizmoOp::Off:
        break;
    }
    XMStoreFloat4x4(&g, G);
    const Staged before = m_staged;
    if (!ApplyGizmoMatrix(index, op, g, centre, scale)) { why = "the result is not a valid prim"; return false; }
    m_undo.push_back(before);
    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
    ++m_gizmoGestures;
    LOG_INFO("[MESHEDITOR] collision gizmo: prim=%d op=%s v=(%.3f,%.3f,%.3f) deg=%.2f gestures=%u", index,
             GizmoOpName(op), v.x, v.y, v.z, deg, m_gizmoGestures);
    return true;
}

// ==== Scripted setters ====
bool MeshEditorWindow::SetViewMode(std::uint32_t mode)
{
    for (const ModeEntry& m : kModes)
        if (m.mode == mode) { m_viewMode = mode; return true; }
    return false;
}

bool MeshEditorWindow::SetVisibleMask(std::uint64_t mask)
{
    if (!m_loaded) return false;
    for (std::size_t k = 0; k < m_visible.size(); ++k)
        m_visible[k] = (k >= 64) || ((mask >> k) & 1ull) != 0;
    return true;
}

bool MeshEditorWindow::Solo(int section)
{
    if (!m_loaded || section >= static_cast<int>(m_sections.size())) return false;
    for (std::size_t k = 0; k < m_visible.size(); ++k)
        m_visible[k] = (section < 0) || static_cast<int>(k) == section;
    return true;
}

bool MeshEditorWindow::Select(int section)
{
    if (!m_loaded || section >= static_cast<int>(m_sections.size())) return false;
    m_selected = section < 0 ? -1 : section;
    return true;
}

bool MeshEditorWindow::SetUv(int channel, int section)
{
    if (channel < 0 || channel > 3 || section >= static_cast<int>(m_sections.size())) return false;
    m_uvChannel = channel;
    m_uvSection = section < 0 ? -1 : section;
    return true;
}

void MeshEditorWindow::SetOrbit(float yaw, float pitch, float distScale)
{
    m_yaw   = yaw;
    m_pitch = std::clamp(pitch, -1.4f, 1.4f);
    m_dist  = std::clamp(distScale, 0.17f, 4.f);
}

// ==== State ====
MeshEditorWindow::StateView MeshEditorWindow::State(Renderer* renderer) const
{
    StateView s;
    s.open     = m_open;
    s.loaded   = m_loaded;
    s.dirty    = m_loaded && Dirty();
    s.model    = m_model;
    s.ref      = m_ref;
    s.meshName = m_meshName;
    s.meshKey  = m_meshKey;
    s.error    = m_error;
    s.viewId   = m_viewId;
    s.viewMode = m_viewMode;
    s.show     = m_show;
    s.visibleMask = VisibleMask();
    s.selected = m_selected;
    s.renderW  = m_renderW;
    s.renderH  = m_renderH;
    s.pendingW = m_pendW;
    s.pendingH = m_pendH;
    if (renderer && m_viewId != ~0u)
    {
        s.lease   = renderer->GetPreviewViewLease(m_viewId);
        s.renders = renderer->GetPreviewViewRenderCount(m_viewId);
        renderer->GetPreviewOutputSize(m_viewId, s.outputW, s.outputH);
    }
    s.stride           = m_stride;
    s.tangents         = (m_libFlags & Resource::MESHLIB_FLAG_HAS_TANGENT) != 0;
    s.colors           = (m_libFlags & Resource::MESHLIB_FLAG_HAS_COLOR) != 0;
    s.uvChannels       = 1u + ((m_libFlags & Resource::MESHLIB_FLAG_HAS_UV1) ? 1u : 0u)
                            + ((m_libFlags & Resource::MESHLIB_FLAG_HAS_UV2) ? 1u : 0u)
                            + ((m_libFlags & Resource::MESHLIB_FLAG_HAS_UV3) ? 1u : 0u);
    s.skinned          = Skinned();
    s.morph            = (m_meshFlags & Resource::MESH_IDENT_HAS_MORPH) != 0;
    s.identityFromFile = m_identityFromFile;
    s.geometryBytes    = m_geometryBytes;
    s.skeleton         = m_skeletonPath;
    s.skeletonBones    = static_cast<std::uint32_t>(m_jointParent.size());
    std::memcpy(s.boundsMin, m_boundsMin, sizeof(s.boundsMin));
    std::memcpy(s.boundsMax, m_boundsMax, sizeof(s.boundsMax));
    s.undoDepth    = m_undo.size();
    s.redoDepth    = m_redo.size();
    s.overlayLines = m_overlayLines;
    s.uvChannel    = m_uvLastChannel;
    s.uvSection    = m_uvLastSection;
    s.uvTriangles  = m_uvTris;
    s.uvDrawn      = m_uvDrawn;
    s.uvMin[0] = m_uvMin[0]; s.uvMin[1] = m_uvMin[1];
    s.uvMax[0] = m_uvMax[0]; s.uvMax[1] = m_uvMax[1];
    s.uvOutside01  = m_uvTris > 0 && (m_uvMin[0] < 0.f || m_uvMin[1] < 0.f || m_uvMax[0] > 1.f || m_uvMax[1] > 1.f);
    s.shapeKeyFile     = m_shapeKeys.FilePath();
    s.shapeKeyNormals  = m_shapeKeys.HasNormals();
    s.shapeKeySections = m_shapeKeys.MorphSections();
    s.shapeKeySolo     = m_shapeKeys.Solo();
    s.shapeKeyHeatmap  = m_shapeKeys.Heatmap();
    s.shapeKeyRevision = m_shapeKeys.Revision();
    s.shapeKeyActive   = m_shapeKeys.Active();
    s.shapeKeyLive     = m_shapeKeys.Live();
    s.socketLock      = m_socketLock;
    s.selectedSocket  = m_selSocket;
    s.socketsDirty    = SocketsDirty();
    s.sockets         = static_cast<std::uint32_t>(m_staged.sockets.size());
    s.overlaySockets  = m_overlaySockets;
    s.weightsBone     = m_weights.Bone();
    s.weightsLive     = m_weights.Live();
    s.weightParts     = m_weights.Parts();
    s.weightsRevision = m_weights.Revision();
    s.skinBones       = static_cast<std::uint32_t>(m_skinBones.size());
    for (std::size_t i = 0; i < m_shapeKeys.Count(); ++i)
        s.shapeKeys.push_back({ m_shapeKeys.Name(i), m_shapeKeys.Weight(i), m_shapeKeys.SectionsOf(i),
                                m_shapeKeys.MaxDelta(i), ShapeKeyMetaOf(m_shapeKeys.Name(i)) });
    s.shapeKeyLock    = m_shapeKeyLock;
    s.shapeKeysDirty  = m_loaded && ShapeKeysDirty();
    s.shapeKeyPresets = m_staged.shapeKeys.presets;
    for (std::size_t k = 0; k < m_sections.size(); ++k)
    {
        const Section& sec = m_sections[k];
        SectionState st;
        st.entry     = sec.meta.entry;
        st.key       = sec.meta.key;
        st.slot      = sec.meta.slot;
        st.material  = k < m_staged.materials.size() ? m_staged.materials[k] : std::string();
        st.saved     = k < m_saved.materials.size() ? m_saved.materials[k] : std::string();
        st.editable  = sec.editable;
        st.visible   = k < m_visible.size() ? static_cast<bool>(m_visible[k]) : true;
        st.triangles = sec.triangles;
        st.vertices  = sec.vertices;
        s.triangles += sec.triangles;
        s.vertices  += sec.vertices;
        s.sections.push_back(std::move(st));
    }
    return s;
}

// ==== The view (AssetPreviewWindow's lifecycle) ====
void MeshEditorWindow::UpdateView(const MeshEditorContext& ctx)
{
    Renderer* renderer = ctx.renderer;
    m_visibleThisFrame = true;
    if (m_releaseQueued) return;   // handed back at the boundary, re-acquired next frame
    if (m_viewId == ~0u)
    {
        // worldCull=false: a SYNTHETIC view - the preview's own instance and
        // material tables, no World.
        m_viewId = renderer->AcquirePreviewView(/*worldCull=*/false);
        if (m_viewId == ~0u) return;
    }
    if (m_harnessW != 0) { m_renderW = m_pendW = m_harnessW; m_renderH = m_pendH = m_harnessH; }
    // Nothing is asked for before DrawViewport measured the picture's box (it runs after
    // this, so a new showing asks one frame later; a showing that starts on the UV Layout
    // tab asks once the Viewport tab is picked). Do not guess a first size: the real one
    // arrives through the resize debounce and re-creates the view - a frame without a
    // picture, every projected point moved.
    if (m_renderW == 0 || m_renderH == 0) return;

    Preview::ViewDesc d{};
    d.w = m_renderW;
    d.h = m_renderH;
    d.source     = Preview::Source::Synthetic;
    d.assetKind  = Preview::AssetKind::MeshAsset;
    d.assetPath  = m_ref;
    d.background = static_cast<Preview::Background>(m_background);
    d.features   = (d.background != Preview::Background::Solid ? Preview::Feature::kSkybox : 0u)
                 | (m_shadow ? Preview::Feature::kStudioShadow : 0u);
    // 0, NOT 1: the HDR slot's optimised clear value is {0,0,0,0}.
    d.clearColor[0] = d.clearColor[1] = d.clearColor[2] = d.clearColor[3] = 0.f;
    d.yawOffset     = m_yaw;
    d.pitchOffset   = m_pitch;
    d.distanceScale = m_dist;
    d.pan[0] = m_pan[0]; d.pan[1] = m_pan[1]; d.pan[2] = m_pan[2];
    // The two reverse arms leave the picture Lit / whole (packs'
    // mesh_editor_viewmodes / _isolate must then fail).
    d.viewMode      = m_harnessViewModeOff ? 0u : m_viewMode;
    // Bits past the Mesh's own sections stay SET: a placement the panel has no
    // row for is not the panel's to hide (the whole-library reverse arm composes
    // more placements than the Mesh has sections, and must show them).
    {
        const std::size_t n = m_visible.size();
        const std::uint64_t mine = (n >= 64) ? ~0ull : ((1ull << n) - 1ull);
        d.partMask = m_harnessMaskOff ? ~0ull : (VisibleMask() | ~mine);
    }
    d.highlightPart = (m_selected >= 0 && m_dimOthers) ? m_selected : -1;
    // Staged slots show before they are saved: placement k = section k.
    if (m_staged.materials != m_saved.materials)
    {
        d.partMaterials.assign(m_sections.size(), std::string());
        for (std::size_t k = 0; k < m_sections.size(); ++k)
            if (m_staged.materials[k] != m_saved.materials[k])
                // An emptied slot draws the engine default: name it, since an
                // empty override means "the asset's own".
                d.partMaterials[k] = m_staged.materials[k].empty() ? std::string("engine/textures/Default_mat.imat")
                                                                   : m_staged.materials[k];
    }
    // The shape-key preview draws the panel's morphed copies (while any weight counts or
    // the heatmap is on; the library's own geometry otherwise). The weight heatmap draws
    // its own copies of the same sections - one of the two, and the heatmap only once its
    // copies exist (while pending or failed, the shape-key preview stays).
    const bool heat = m_weights.Active() && m_weights.HasGpu();
    if (heat) m_weights.Apply(d);
    else      m_shapeKeys.Apply(d);
    renderer->RequestPreview(m_viewId, d, MaterialComponent{});

    // For the harness shots: the render count when the picture last changed.
    char key[512];
    // WHICH copies are drawn is part of the picture (the heatmap's and the shape keys'
    // revisions can coincide): the heatmap's bone, -1 when the shape keys draw.
    std::snprintf(key, sizeof(key), "%s|%u|%u|%u|%llx|%d|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%d|%d|%zu|%llu|%llx|w%d",
                  m_ref.c_str(), d.w, d.h, d.viewMode, static_cast<unsigned long long>(d.partMask),
                  d.highlightPart, d.yawOffset, d.pitchOffset, d.distanceScale, d.pan[0], d.pan[1], d.pan[2],
                  m_background, m_shadow ? 1 : 0, std::hash<std::string>{}([&]
                  {
                      std::string all;
                      for (const std::string& m : d.partMaterials) all += m + "|";
                      return all;
                  }()), static_cast<unsigned long long>(d.partMeshes.empty() ? 0 : d.partMeshRevision),
                  static_cast<unsigned long long>(d.vertexColorParts), heat ? m_weights.Bone() : -1);
    if (m_lastDescKey != key)
    {
        m_lastDescKey   = key;
        m_rendersAtDesc = renderer->GetPreviewViewRenderCount(m_viewId);
    }
}

void MeshEditorWindow::ProcessPendingActions(const MeshEditorContext& ctx)
{
    // AUTO-RELEASE at the frame boundary: this runs BEFORE this frame's Draw,
    // so the flag is the previous frame's answer - "the panel did not draw".
    if (m_viewId != ~0u && !m_visibleThisFrame) m_releaseQueued = true;
    if (!m_visibleThisFrame)
    {
        ReleaseGizmo();   // a gizmo drag in a panel that stopped drawing
        // Its box is measured again when it next draws (a docked tab may come back
        // at another size): UpdateView asks for nothing until then.
        m_renderW = m_renderH = m_pendW = m_pendH = 0;
    }
    m_visibleThisFrame = false;

    // A world reload invalidates the mesh-descriptor slots the composition
    // resolved to.
    if (ctx.hasWorld)
    {
        if (m_worldEpochSeen && ctx.worldEpoch != m_worldEpoch && m_loaded) m_recomposeQueued = true;
        m_worldEpoch     = ctx.worldEpoch;
        m_worldEpochSeen = true;
    }
    if (m_recomposeQueued && ctx.renderer)
    {
        m_recomposeQueued = false;
        if (!m_ref.empty()) ctx.renderer->InvalidatePreviewAsset(m_ref);
    }

    for (; m_pendingUndo > 0; --m_pendingUndo) Undo();
    for (; m_pendingRedo > 0; --m_pendingRedo) Redo();
    if (m_loaded && m_open) EnsureCollisionShapes();   // reads collision libraries: here, never in Draw

    // The shape-key preview's copies - made, rewritten, or handed back with the panel.
    // The heap only takes calls from this half.
    if (ctx.renderer && ctx.gfx)
    {
        MeshDescriptorHeap& heap = ctx.renderer->GetMeshManager().GetDescriptorHeap();
        if (m_loaded && m_open) m_shapeKeys.Update(*ctx.gfx, heap, ctx.worldEpoch);
        else if (m_shapeKeys.HasGpu()) m_shapeKeys.Release(ctx.gfx, &heap, ctx.worldEpoch);
        // The weight heatmap's, the same way.
        if (m_loaded && m_open) m_weights.Update(*ctx.gfx, heap, ctx.worldEpoch);
        else if (m_weights.HasGpu()) m_weights.Release(ctx.gfx, &heap, ctx.worldEpoch);
    }

    if (m_saveQueued)
    {
        m_saveQueued = false;
        std::string why;
        if (!SaveNow(ctx, why))
        {
            m_error = why;
            m_closeAfterSave = false;
            LOG_WARNING("[MESHEDITOR] save failed: %s", why.c_str());
        }
        else if (m_closeAfterSave)
        {
            m_closeAfterSave = false;
            m_open = false;
        }
    }
    if (m_openQueued)
    {
        if (m_loaded && Dirty())
        {
            // Asked once, in the panel; the answer clears or keeps the edits
            // and this runs again.
            if (m_askFor == 0) { m_askFor = 2; m_askOpenPopup = true; }
        }
        else
        {
            m_openQueued = false;
            std::string code, why;
            if (!LoadSubject(ctx, m_queuedModel, m_queuedKey, m_queuedName, code, why)) m_error = why;
        }
    }

    TickHarness(ctx);

    if (!m_releaseQueued) return;
    m_releaseQueued = false;
    if (m_viewId != ~0u && ctx.renderer)
    {
        ctx.renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[MESHEDITOR] released view %u at the frame boundary", m_viewId);
    }
    m_viewId = ~0u;
}

void MeshEditorWindow::ReleaseViewsAtShutdown(Renderer* renderer)
{
    // The device is going away with everything it holds: forget the copies.
    m_shapeKeys.Release(nullptr, nullptr, 0);
    m_weights.Release(nullptr, nullptr, 0);
    m_releaseQueued = false;
    if (m_viewId == ~0u) return;
    if (renderer)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[MESHEDITOR] released view %u at shutdown", m_viewId);
    }
    m_viewId = ~0u;
}

// ==== Draw ====
void MeshEditorWindow::Draw(const MeshEditorContext& ctx)
{
    ImGui::SetNextWindowSize(ImVec2(1040.f, 660.f), ImGuiCond_FirstUseEver);
    const std::string title = m_loaded
        ? "Mesh Editor - " + m_meshName + (Dirty() ? " *" : "") + "###MeshEditor"
        : std::string("Mesh Editor###MeshEditor");
    bool keepOpen = true;
    if (!ImGui::Begin(title.c_str(), &keepOpen))
    {
        // Collapsed is not visible: the view goes back at the boundary.
        m_focused = false;
        ImGui::End();
        return;
    }
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!keepOpen)
    {
        // The X asks first when there is something to lose.
        if (m_loaded && Dirty()) { if (m_askFor == 0) { m_askFor = 1; m_askOpenPopup = true; } }
        else m_open = false;
    }

    if (!ctx.renderer || !ctx.renderer->IsPreviewEnabled())
    {
        ImGui::TextDisabled(ctx.renderer ? "Preview disabled (DX12_PREVIEW=0 or editor_prefs preview.enabled=0)"
                                         : "No renderer attached.");
        ImGui::End();
        return;
    }
    if (!m_loaded)
    {
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "%s", m_error.c_str());
        ImGui::TextDisabled("Open a Mesh: right-click a Mesh item of a model in the Content Browser >\n"
                            "Open in Mesh Editor, or Open in Mesh Editor on a Static Mesh's Inspector.");
        DrawAskModal();
        ImGui::End();
        return;
    }

    UpdateView(ctx);
    DrawToolbar(ctx);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float statsH   = ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    const float availW   = ImGui::GetContentRegionAvail().x;
    const float detailsW = std::clamp(availW * 0.34f, 260.f, 440.f);
    ImGui::BeginChild("##me_left", ImVec2(std::max(64.f, availW - detailsW - style.ItemSpacing.x), -statsH),
                      false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("##me_tabs"))
    {
        if (ImGui::BeginTabItem("Viewport"))  { m_tab = 0; DrawViewport(ctx); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("UV Layout")) { m_tab = 1; DrawUvLayout();    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##me_right", ImVec2(0.f, -statsH), true);
    DrawDetails(ctx);
    ImGui::EndChild();
    DrawStatsLine();
    DrawAskModal();
    ImGui::End();
}

void MeshEditorWindow::DrawToolbar(const MeshEditorContext& ctx)
{
    const bool dirty = Dirty();
    ImGui::BeginDisabled(!dirty || ctx.playing);
    if (ImGui::Button("Save")) m_saveQueued = true;
    ImGui::EndDisabled();
    if (ctx.playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop Play first");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert")) Revert();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::ArrowButton("##me_undo", ImGuiDir_Left)) RequestUndo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo (Ctrl+Z)");
    ImGui::SameLine(0.f, 2.f);
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::ArrowButton("##me_redo", ImGuiDir_Right)) RequestRedo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Redo (Ctrl+Y)");
    ImGui::SameLine();
    if (ImGui::Button("Browse") && ctx.reveal) ctx.reveal(m_model);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the model in the Content Browser");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    int modeIdx = 0;
    for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i)
        if (kModes[i].mode == m_viewMode) modeIdx = i;
    ImGui::SetNextItemWidth(150.f);
    if (ImGui::BeginCombo("##me_view", kModes[modeIdx].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i)
            if (ImGui::Selectable(kModes[i].label, i == modeIdx)) m_viewMode = kModes[i].mode;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("This view's mode only - the main viewport keeps its own.\n"
                          "Ambient Occlusion is the material's AO (the preview runs no SSAO).");
    ImGui::SameLine();
    if (ImGui::Button("Show"))
        ImGui::OpenPopup("##me_show");
    if (ImGui::BeginPopup("##me_show"))
    {
        for (const ShowEntry& s : kShows)
        {
            bool on = (m_show & s.bit) != 0;
            if (ImGui::Checkbox(s.label, &on)) m_show = on ? (m_show | s.bit) : (m_show & ~s.bit);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    static const char* kBgNames[] = { "Solid", "Checker", "Skybox" };
    ImGui::SetNextItemWidth(90.f);
    ImGui::Combo("##me_bg", &m_background, kBgNames, IM_ARRAYSIZE(kBgNames));
    ImGui::SameLine();
    ImGui::Checkbox("Shadow", &m_shadow);
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) ResetView();
    if (!m_error.empty())
        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "%s", m_error.c_str());
}

void MeshEditorWindow::DrawViewport(const MeshEditorContext& ctx)
{
    Renderer* renderer = ctx.renderer;
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Size: quantise, then debounce (a recompile recreates the graph's
    // physical textures).
    if (m_harnessW != 0)
    {
        m_renderW = m_pendW = m_harnessW;
        m_renderH = m_pendH = m_harnessH;
    }
    else
    {
        std::uint32_t w = 0, h = 0;
        QuantiseBox(avail.x, avail.y, w, h);
        const double now = ImGui::GetTime();
        // The FIRST measurement of a showing is the size at once: nothing was drawn at
        // another (UpdateView waited for it). Only a change after it - a drag, the
        // toolbar's error line coming or going - waits out the debounce.
        if (m_renderW == 0 || m_renderH == 0)
        {
            m_renderW = m_pendW = w;
            m_renderH = m_pendH = h;
            m_pendAt  = now;
        }
        else if (w != m_pendW || h != m_pendH) { m_pendW = w; m_pendH = h; m_pendAt = now; }
        else if ((m_pendW != m_renderW || m_pendH != m_renderH) && now - m_pendAt >= kResizeDebounceSeconds)
        {
            m_renderW = m_pendW;
            m_renderH = m_pendH;
        }
    }

    const std::uint64_t srv = (m_viewId != ~0u) ? renderer->GetPreviewSrv(m_viewId) : 0;
    std::uint32_t ow = 0, oh = 0;
    if (m_viewId != ~0u) renderer->GetPreviewOutputSize(m_viewId, ow, oh);
    if (!srv || ow == 0 || oh == 0)
    {
        ImGui::Dummy(ImVec2(std::max(1.f, avail.x), std::max(1.f, avail.y)));
        ImGui::SetCursorPos(ImVec2(12.f, 12.f));
        if (m_viewId == ~0u)
            ImGui::TextDisabled("No preview view is free (camera previews, split views and the other\n"
                                "preview panels hold them all) - close one to see the mesh.");
        else
            ImGui::TextDisabled("Rendering...");
        return;
    }

    // Fit-scaled and centred, never stretched.
    float s = std::min(avail.x / static_cast<float>(ow), avail.y / static_cast<float>(oh));
    if (!(s > 0.f)) s = 1.f;
    const ImVec2 size(static_cast<float>(ow) * s, static_cast<float>(oh) * s);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 origin(start.x + std::max(0.f, (avail.x - size.x) * 0.5f),
                        start.y + std::max(0.f, (avail.y - size.y) * 0.5f));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    m_overlayLines = DrawOverlay(renderer, dl, origin.x, origin.y, size.x, size.y);
    // What the picture is, in its corner.
    std::uint32_t parts = 0, mats = 0, dropped = 0;
    renderer->GetPreviewAssetStats(m_viewId, parts, mats, dropped);
    char badge[128];
    std::snprintf(badge, sizeof(badge), "%s  |  %u of %zu section(s)", kModes[0].label, parts, m_sections.size());
    for (const ModeEntry& m : kModes)
        if (m.mode == m_viewMode)
            std::snprintf(badge, sizeof(badge), "%s  |  %u of %zu section(s)", m.label, parts, m_sections.size());
    dl->AddText(ImVec2(origin.x + 8.f, origin.y + 6.f), IM_COL32(230, 230, 230, 220), badge);
    dl->PopClipRect();

    // ---- The collision gizmo: BEFORE ##me_orbit - ImGuizmo's patched CanActivate
    // needs HoveredId == 0 && ActiveId == 0 on the click frame.
    DrawCollisionGizmo(renderer, origin.x, origin.y, size.x, size.y);
    // Orbit / pan stand aside while the gizmo is hovered or held (the button is
    // DISABLED, so the click that grabs a handle cannot also orbit); a hover
    // while an orbit already runs does not interrupt it.
    const bool gizmoBusy = m_gizmoUsing || (m_gizmoOver && !m_orbitActive);

    // ---- Orbit / pan / zoom, on the panel's OWN state ----
    ImGui::SetCursorScreenPos(origin);
    if (gizmoBusy) ImGui::BeginDisabled();
    ImGui::InvisibleButton("##me_orbit", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    if (gizmoBusy) ImGui::EndDisabled();
    m_orbitActive = ImGui::IsItemActive();
    if (ImGui::IsItemActive())
    {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            m_yaw   += d.x * 0.01f;
            m_pitch  = std::clamp(m_pitch - d.y * 0.01f, -1.4f, 1.4f);
        }
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            // World units per pixel at the framed point.
            const Preview::StudioCamera& sc = Preview::kStudioCamera;
            const float dist = std::max(0.35f, sc.distance * std::max(0.05f, m_dist));
            const float wpp  = 2.f * dist * std::tan(sc.fovY * 0.5f) / std::max(1.f, size.y);
            const Cam   cam  = BuildCam(m_yaw, m_pitch, m_dist, m_pan, size.x / std::max(1.f, size.y));
            XMFLOAT3 r, u;
            XMStoreFloat3(&r, cam.right);
            XMStoreFloat3(&u, cam.up);
            m_pan[0] += (-d.x * r.x + d.y * u.x) * wpp;
            m_pan[1] += (-d.x * r.y + d.y * u.y) * wpp;
            m_pan[2] += (-d.x * r.z + d.y * u.z) * wpp;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) m_dist = std::clamp(m_dist * (1.f - wheel * 0.1f), 0.17f, 4.f);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) ResetView();
        // The gizmo's operation - never mid-drag (switching it while ImGuizmo is
        // using strands its mbUsing: the release becomes unreachable).
        if (!m_gizmoUsing && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOp = GizmoOp::Translate;
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOp = GizmoOp::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOp = GizmoOp::Scale;
        }
        if (ImGui::GetIO().KeyShift)
            ImGui::SetTooltip("Left-drag orbit, right/middle-drag pan, wheel zoom, F frame.\n"
                              "W / E / R: move / rotate / scale the selected collision prim.\n"
                              "Skinned meshes are drawn at their bind pose; shape keys show through\n"
                              "the Shape Keys section's weights.");
    }
}

void MeshEditorWindow::DrawUvLayout()
{
    int channels[4] = { 0, -1, -1, -1 };
    const char* names[] = { "UV0", "UV1", "UV2", "UV3" };
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::BeginCombo("Channel", names[m_uvChannel]))
    {
        for (int c = 0; c < 4; ++c)
        {
            bool present = false;
            for (const Section& s : m_sections) present |= !s.uv[c].empty();
            channels[c] = present ? c : -1;
            if (!present) continue;
            if (ImGui::Selectable(names[c], c == m_uvChannel)) m_uvChannel = c;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const std::string cur = m_uvSection < 0 ? std::string("Every visible section")
                          : std::to_string(m_uvSection) + "  " + m_sections[static_cast<std::size_t>(m_uvSection)].meta.slot;
    ImGui::SetNextItemWidth(240.f);
    if (ImGui::BeginCombo("Section", cur.c_str()))
    {
        if (ImGui::Selectable("Every visible section", m_uvSection < 0)) m_uvSection = -1;
        for (int k = 0; k < static_cast<int>(m_sections.size()); ++k)
        {
            const std::string label = std::to_string(k) + "  " + m_sections[static_cast<std::size_t>(k)].meta.slot;
            if (ImGui::Selectable(label.c_str(), m_uvSection == k)) m_uvSection = k;
        }
        ImGui::EndCombo();
    }
    (void)channels;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float  line  = ImGui::GetTextLineHeightWithSpacing();
    const float  side  = std::max(64.f, std::min(avail.x, avail.y - 2.f * line));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##me_uvcanvas", ImVec2(side, side));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(22, 22, 26, 255));
    dl->PushClipRect(origin, ImVec2(origin.x + side, origin.y + side), true);
    DrawUv(dl, origin.x, origin.y, side);
    dl->PopClipRect();
    ImGui::Text("%u triangle(s)%s   UV (%.3f, %.3f) - (%.3f, %.3f)", m_uvTris,
                m_uvDrawn < m_uvTris ? (" (" + std::to_string(m_uvDrawn) + " drawn)").c_str() : "",
                m_uvMin[0], m_uvMin[1], m_uvMax[0], m_uvMax[1]);
    if (m_uvTris > 0 && (m_uvMin[0] < 0.f || m_uvMin[1] < 0.f || m_uvMax[0] > 1.f || m_uvMax[1] > 1.f))
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "UVs leave 0..1 - the texture tiles (the square is the 0..1 tile).");
}

void MeshEditorWindow::DrawDetails(const MeshEditorContext& ctx)
{
    if (ImGui::CollapsingHeader("Asset", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("%s", m_meshName.c_str());
        ImGui::TextDisabled("model");
        ImGui::SameLine();
        if (ImGui::Selectable(m_model.c_str(), false) && ctx.reveal) ctx.reveal(m_model);
        ImGui::TextDisabled("key   %s", Resource::MeshKeyToHex(m_meshKey).c_str());
        ImGui::TextDisabled("geometry  %s", Sys::RelativizeAssetPath(m_libraryPath).c_str());
        ImGui::TextDisabled("identity  %s", m_identityFromFile ? "the library's own" : "synthesized (legacy library)");
        if (Skinned())
        {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "Skinned: drawn at the bind pose.");
            if (!m_skeletonPath.empty())
            {
                ImGui::TextDisabled("skeleton  %s (%zu bones; Show > Skeleton)", m_skeletonPath.c_str(),
                                    m_jointParent.size());
                // The model in the Skeleton Editor (here, not on the toolbar: a wrapped toolbar would
                // shrink the picture).
                if (ImGui::SmallButton("Open in Skeleton Editor##me_asset_open")) SkeletonEditorWindow::Get().RequestOpen(m_model);
            }
        }
        if (m_meshFlags & Resource::MESH_IDENT_HAS_MORPH && m_shapeKeys.Count() == 0)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "Has shape keys, but its .imorph did not read.");
    }

    char header[64];
    std::snprintf(header, sizeof(header), "Sections (%zu)###me_sections", m_sections.size());
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::SmallButton("Show all")) Solo(-1);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selected < 0);
        if (ImGui::SmallButton("Isolate selected")) Solo(m_selected);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Dim others", &m_dimOthers);

        for (int k = 0; k < static_cast<int>(m_sections.size()); ++k)
        {
            Section& sec = m_sections[static_cast<std::size_t>(k)];
            ImGui::PushID(k);
            bool vis = m_visible[static_cast<std::size_t>(k)];
            if (ImGui::Checkbox("##vis", &vis)) m_visible[static_cast<std::size_t>(k)] = vis;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visible");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(SectionColour(k)));
            char label[256];
            std::snprintf(label, sizeof(label), "%d  %s   (%u tris)", k,
                          sec.meta.slot.empty() ? "(unnamed)" : sec.meta.slot.c_str(), sec.triangles);
            if (ImGui::Selectable(label, m_selected == k, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(std::max(40.f, ImGui::GetContentRegionAvail().x - 44.f), 0.f)))
                m_selected = (m_selected == k) ? -1 : k;
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Solo")) Solo(k);

            // The default material: staged here, written by Save.
            const std::string& staged = m_staged.materials[static_cast<std::size_t>(k)];
            const bool changed = staged != m_saved.materials[static_cast<std::size_t>(k)];
            char buf[512];
            strncpy_s(buf, sizeof(buf), staged.c_str(), _TRUNCATE);
            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            ImGui::BeginDisabled(!sec.editable);
            ImGui::SetNextItemWidth(changed ? -ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x : -FLT_MIN);
            if (ImGui::InputText("##mat", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::string why;
                if (!StageMaterial(k, buf, why)) m_error = why; else m_error.clear();
            }
            ImGui::EndDisabled();
            if (sec.editable && ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IMAT_PATH"))
                {
                    std::string why;
                    if (!StageMaterial(k, static_cast<const char*>(p->Data), why)) m_error = why;
                    else m_error.clear();
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(sec.editable
                    ? "The model's DEFAULT material for this slot: every Static Mesh of it\n"
                      "without an override draws it. Drop a material here; Save writes it."
                    : "This model does not own this slot's default (it has no F-line for\n"
                      "the entry): the library's own model file defines it.");
            if (changed)
            {
                ImGui::SameLine();
                if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.f)))
                {
                    std::string why;
                    StageMaterial(k, m_saved.materials[static_cast<std::size_t>(k)], why);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the saved material");
            }
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            ImGui::PopID();
        }
    }

    DrawShapeKeyDetails();
    DrawCollisionDetails(ctx);
    DrawSocketDetails();          // collapsed by default
    DrawSkeletonDetails(ctx);     // collapsed by default; a skinned Mesh only

    if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::uint32_t tris = 0, verts = 0;
        for (const Section& s : m_sections) { tris += s.triangles; verts += s.vertices; }
        ImGui::Text("%u triangles   %u vertices", tris, verts);
        ImGui::Text("stride %u B   %.2f MB of geometry", m_stride, static_cast<double>(m_geometryBytes) / (1024.0 * 1024.0));
        ImGui::Text("UV0%s%s%s   tangents %s   vertex colour %s",
                    (m_libFlags & Resource::MESHLIB_FLAG_HAS_UV1) ? " UV1" : "",
                    (m_libFlags & Resource::MESHLIB_FLAG_HAS_UV2) ? " UV2" : "",
                    (m_libFlags & Resource::MESHLIB_FLAG_HAS_UV3) ? " UV3" : "",
                    (m_libFlags & Resource::MESHLIB_FLAG_HAS_TANGENT) ? "yes" : "no",
                    (m_libFlags & Resource::MESHLIB_FLAG_HAS_COLOR) ? "yes" : "no");
        ImGui::Text("bounds %.3f x %.3f x %.3f", m_boundsMax[0] - m_boundsMin[0], m_boundsMax[1] - m_boundsMin[1],
                    m_boundsMax[2] - m_boundsMin[2]);
    }
}

void MeshEditorWindow::DrawShapeKeyDetails()
{
    MeshShapeKeys& sk = m_shapeKeys;
    if (sk.Count() == 0) return;
    char header[96];
    std::snprintf(header, sizeof(header), "Shape Keys (%zu)%s###me_shapekeys", sk.Count(), ShapeKeysDirty() ? " *" : "");
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::TextDisabled("%s  %u section(s)%s", sk.FilePath().c_str(), sk.MorphSections(),
                        sk.HasNormals() ? "  normals" : "  no normal deltas");
    if (!m_shapeKeyLock.empty()) ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "%s", m_shapeKeyLock.c_str());
    if (ImGui::SmallButton("Reset")) ResetShapeKeyWeights();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every weight back to its default, solo off");
    ImGui::SameLine();
    ImGui::BeginDisabled(sk.Heatmap() < 0);
    if (ImGui::SmallButton("Heatmap off")) sk.SetHeatmap(-1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto& cats = Resource::ShapeKeyCategories();
    ImGui::SetNextItemWidth(110.f);
    if (ImGui::BeginCombo("##skcat", m_shapeKeyCategory < 0 ? "all categories"
                                                             : cats[static_cast<std::size_t>(m_shapeKeyCategory)].c_str()))
    {
        if (ImGui::Selectable("all categories", m_shapeKeyCategory < 0)) m_shapeKeyCategory = -1;
        for (int c = 0; c < static_cast<int>(cats.size()); ++c)
            if (ImGui::Selectable(cats[static_cast<std::size_t>(c)].c_str(), m_shapeKeyCategory == c)) m_shapeKeyCategory = c;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##skfilter", "search", m_shapeKeyFilter, sizeof(m_shapeKeyFilter));

    std::string filter = m_shapeKeyFilter;
    for (char& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (std::size_t i = 0; i < sk.Count(); ++i)
    {
        const std::string&           name = sk.Name(i);
        const Resource::ShapeKeyMeta meta = ShapeKeyMetaOf(name);
        if (m_shapeKeyCategory >= 0 && meta.category != cats[static_cast<std::size_t>(m_shapeKeyCategory)]) continue;
        const std::string shown = meta.label.empty() ? name : meta.label;
        if (!filter.empty())
        {
            std::string low = name + "|" + meta.label;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.find(filter) == std::string::npos) continue;
        }
        ImGui::PushID(static_cast<int>(i));
        bool solo = sk.Solo() == static_cast<int>(i);
        if (ImGui::Checkbox("##solo", &solo)) sk.SetSolo(solo ? static_cast<int>(i) : -1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo: only this shape counts");
        ImGui::SameLine();
        bool heat = sk.Heatmap() == static_cast<int>(i);
        if (ImGui::Checkbox("##heat", &heat)) sk.SetHeatmap(heat ? static_cast<int>(i) : -1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Heatmap: colour the Mesh by how far this shape moves each vertex\n"
                              "(blue = not at all, red = its largest move, %.4f m)", sk.MaxDelta(i));
        ImGui::SameLine();
        float w = sk.Weight(i);
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::SliderFloat("##w", &w, meta.min, meta.max, (shown + "  %.2f").c_str())) sk.SetWeight(i, w);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s\nmoves %u section(s), up to %.4f m", name.c_str(),
                              meta.category.empty() ? "" : "  -  ", meta.category.c_str(), sk.SectionsOf(i),
                              sk.MaxDelta(i));
        ImGui::SameLine();
        if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0.f))) ImGui::OpenPopup("##skmeta");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Category, default, range, label (saved with the model)");
        if (ImGui::BeginPopup("##skmeta"))
        {
            // Edited on a copy; one undo step when it changes.
            Resource::ShapeKeyMeta m = meta;
            bool changed = false;
            ImGui::BeginDisabled(!m_shapeKeyLock.empty());
            ImGui::TextUnformatted(name.c_str());
            if (ImGui::BeginCombo("category", m.category.empty() ? "(none)" : m.category.c_str()))
            {
                if (ImGui::Selectable("(none)", m.category.empty())) { m.category.clear(); changed = true; }
                for (const std::string& c : cats)
                    if (ImGui::Selectable(c.c_str(), m.category == c)) { m.category = c; changed = true; }
                ImGui::EndCombo();
            }
            changed |= ImGui::SliderFloat("default", &m.def, m.min, m.max, "%.2f");
            float range[2] = { m.min, m.max };
            if (ImGui::DragFloat2("range", range, 0.01f, 0.f, 1.f, "%.2f") && range[0] < range[1])
            {
                m.min = range[0];
                m.max = range[1];
                m.def = std::clamp(m.def, m.min, m.max);
                changed = true;
            }
            char label[64];
            strncpy_s(label, sizeof(label), m.label.c_str(), _TRUNCATE);
            if (ImGui::InputText("label", label, sizeof(label), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                m.label = label;
                changed = true;
            }
            ImGui::EndDisabled();
            if (changed)
            {
                // A drag is one undo step: pushed at its first change.
                std::string why;
                if (!StageShapeKeyMetaImpl(m, why, !m_skMetaDragPushed)) m_error = why;
                else m_skMetaDragPushed = ImGui::IsAnyItemActive();
            }
            if (!ImGui::IsAnyItemActive()) m_skMetaDragPushed = false;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Presets (MP): named weight sets, staged; Save writes them.
    ImGui::Spacing();
    ImGui::TextDisabled("Presets (%zu)", m_staged.shapeKeys.presets.size());
    ImGui::BeginDisabled(!m_shapeKeyLock.empty());
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Save preset").x - ImGui::GetStyle().FramePadding.x * 2.f -
                            ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##presetname", "preset name", m_presetName, sizeof(m_presetName));
    ImGui::SameLine();
    if (ImGui::Button("Save preset"))
    {
        std::string why;
        if (!SaveShapeKeyPreset(m_presetName, why)) m_error = why;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The weights the picture shows now, as a named preset");
    ImGui::EndDisabled();
    std::string toDelete;
    for (const Resource::ShapeKeyPreset& p : m_staged.shapeKeys.presets)
    {
        ImGui::PushID(p.name.c_str());
        if (ImGui::SmallButton("Apply"))
        {
            std::string why;
            if (!ApplyShapeKeyPreset(p.name, why)) m_error = why;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_shapeKeyLock.empty());
        if (ImGui::SmallButton("x")) toDelete = p.name;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (p.source.empty()) ImGui::Text("%s  (%zu)", p.name.c_str(), p.weights.size());
        else ImGui::Text("%s  (%zu)  [%s]", p.name.c_str(), p.weights.size(), p.source.c_str());
        if (ImGui::IsItemHovered())
        {
            std::string t;
            if (!p.source.empty())
                t = "From the source (" + p.source + "): a re-import refreshes it.\n"
                    "Save preset under its name to make it yours.\n\n";
            for (const auto& [n, w] : p.weights) t += n + "  " + std::to_string(w).substr(0, 4) + "\n";
            ImGui::SetTooltip("%s", p.weights.empty() ? "(every weight 0)" : t.c_str());
        }
        ImGui::PopID();
    }
    if (!toDelete.empty())
    {
        std::string why;
        if (!DeleteShapeKeyPreset(toDelete, why)) m_error = why;
    }
}

void MeshEditorWindow::DrawCollisionDetails(const MeshEditorContext& ctx)
{
    (void)ctx;
    StagedCollision& c = m_staged.collision;
    char header[96];
    std::snprintf(header, sizeof(header), "Collision (%zu prim%s)%s###me_collision", c.setup.prims.size(),
                  c.setup.prims.size() == 1 ? "" : "s", CollisionDirty() ? " *" : "");
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) return;
    const bool locked = !m_collisionLock.empty();
    if (locked)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.72f, 0.35f, 1.f));
        ImGui::TextWrapped("%s", m_collisionLock.c_str());
        ImGui::PopStyleColor();
    }
    bool show = (m_show & kShowCollision) != 0;
    if (ImGui::Checkbox("Show in the view", &show)) m_show = show ? (m_show | kShowCollision) : (m_show & ~kShowCollision);
    ImGui::BeginDisabled(locked);
    // The gizmo on the selected prim; never switched mid-drag.
    {
        static const char* kOps[] = { "Off", "Move (W)", "Rotate (E)", "Scale (R)" };
        ImGui::BeginDisabled(m_gizmoUsing);
        for (int k = 0; k < IM_ARRAYSIZE(kOps); ++k)
        {
            if (k) ImGui::SameLine();
            if (ImGui::RadioButton(kOps[k], static_cast<int>(m_gizmoOp) == k)) m_gizmoOp = static_cast<GizmoOp>(k);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The gizmo on the selected prim, in the picture. A box scales by its faces;\n"
                              "a hull only moves and turns; a trimesh prim has none. One drag = one undo step.");
    }

    // A drag is ONE undo step: at its first change the staged state - not yet
    // touched, every widget below edits a copy that is written back after it -
    // goes on the ring; the next drag starts once nothing is active.
    if (!ImGui::IsAnyItemActive()) m_dragPushed = false;
    auto track = [&](bool changed) -> bool
    {
        if (changed && !m_dragPushed)
        {
            PushUndo();
            m_dragPushed = true;
        }
        return changed;
    };
    auto stage = [&](const Resource::MeshBodySetup& next)
    {
        std::string why;
        if (!StageCollision(next, why)) m_error = why;
        else m_error.clear();
    };

    // ---- The body ----
    static const char* kComplexity[] = { "Simple", "Complex", "None" };
    int cx = static_cast<int>(c.setup.complexity);
    ImGui::SetNextItemWidth(140.f);
    if (ImGui::Combo("Complexity", &cx, kComplexity, IM_ARRAYSIZE(kComplexity)))
    {
        Resource::MeshBodySetup next = c.setup;
        next.complexity = static_cast<Resource::MeshBodySetup::Complexity>(cx);
        stage(next);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Simple: the prims below - one body per placed Mesh (a compound for several).\n"
                          "Complex: every section's own triangles, a static trimesh (the prims are kept, unused).\n"
                          "None: no collision (the prims are kept for editing).");
    static const char* kLayers[] = { "Auto", "StaticEnv", "DynamicProp", "Character", "Ragdoll",
                                     "HurtBox", "AttackBox", "Projectile", "Sensing" };
    int layer = c.setup.layer < IM_ARRAYSIZE(kLayers) ? c.setup.layer : 0;
    ImGui::SetNextItemWidth(140.f);
    if (ImGui::Combo("Layer", &layer, kLayers, IM_ARRAYSIZE(kLayers)))
    {
        Resource::MeshBodySetup next = c.setup;
        next.layer = static_cast<std::uint8_t>(layer);
        stage(next);
    }
    bool trig = c.setup.trigger;
    if (ImGui::Checkbox("Trigger", &trig))
    {
        Resource::MeshBodySetup next = c.setup;
        next.trigger = trig;
        stage(next);
    }
    for (int f = 0; f < 2; ++f)
    {
        ImGui::PushID(f);
        const float v  = f == 0 ? c.setup.friction : c.setup.restitution;
        bool        on = v >= 0.f;
        if (ImGui::Checkbox("##on", &on))
        {
            Resource::MeshBodySetup next = c.setup;
            (f == 0 ? next.friction : next.restitution) = on ? (f == 0 ? 0.5f : 0.f) : -1.f;   // the RigidBody's defaults
            stage(next);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override the RigidBody's %s", f == 0 ? "friction" : "restitution");
        ImGui::SameLine();
        if (on)
        {
            float val = f == 0 ? c.setup.friction : c.setup.restitution;
            ImGui::SetNextItemWidth(120.f);
            if (track(ImGui::DragFloat(f == 0 ? "Friction" : "Restitution", &val, 0.01f, 0.f, f == 0 ? 4.f : 1.f, "%.2f")))
                (f == 0 ? c.setup.friction : c.setup.restitution) = (std::max)(val, 0.f);
        }
        else
            ImGui::TextDisabled("%s: the RigidBody's", f == 0 ? "Friction" : "Restitution");
        ImGui::PopID();
    }

    // ---- Add ----
    if (ImGui::Button("Add")) ImGui::OpenPopup("##me_coll_add");
    ImGui::SameLine();
    if (m_selected >= 0 && m_selected < static_cast<int>(m_sections.size()))
        ImGui::TextDisabled("fits section %d (%s)", m_selected,
                            m_sections[static_cast<std::size_t>(m_selected)].meta.slot.c_str());
    else
        ImGui::TextDisabled("fits the whole Mesh (select a section to fit one)");
    if (ImGui::BeginPopup("##me_coll_add"))
    {
        for (const AddEntry& a : kAdds)
        {
            if (a.kind == CollisionAdd::KDop10X || a.kind == CollisionAdd::Convex) ImGui::Separator();
            if (ImGui::MenuItem(a.label))
            {
                std::string why;
                if (!AddCollision(a.kind, m_selected, why)) m_error = why;
                else m_error.clear();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove all", nullptr, false, !c.setup.prims.empty()))
        {
            std::string why;
            RemoveCollision(-1, why);
        }
        ImGui::EndPopup();
    }

    // ---- The prims ----
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Dup").x + ImGui::CalcTextSize("x").x + 4.f * style.FramePadding.x +
                          2.f * style.ItemSpacing.x;
    int removeAt = -1, duplicateAt = -1;
    for (int i = 0; i < static_cast<int>(c.setup.prims.size()); ++i)
    {
        Resource::CollisionPrim& p       = c.setup.prims[static_cast<std::size_t>(i)];
        const std::size_t       pending = c.pending[static_cast<std::size_t>(i)].size();
        ImGui::PushID(1000 + i);
        char label[160];
        std::snprintf(label, sizeof(label), "%d  %s%s%s", i, PrimTypeName(p.type), p.InBody() ? "" : "  (section trimesh)",
                      pending ? "  (new: written by Save)" : "");
        if (ImGui::Selectable(label, m_collisionSelected == i, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(std::max(40.f, ImGui::GetContentRegionAvail().x - buttons), 0.f)))
            m_collisionSelected = (m_collisionSelected == i) ? -1 : i;
        ImGui::SameLine();
        if (ImGui::SmallButton("Dup")) duplicateAt = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");

        if (m_collisionSelected == i)
        {
            using T = Resource::CollisionPrim::Type;
            constexpr float kMin = Physics::CollisionFit::kMinHalfThickness;
            ImGui::Indent();
            Resource::CollisionPrim e = p;
            bool changed = false;
            if (p.type != T::Mesh)
                changed |= track(ImGui::DragFloat3("Center", &e.center.x, 0.005f, -1e4f, 1e4f, "%.3f"));
            if (p.type == T::Box || p.type == T::Capsule || p.type == T::Hull)
            {
                XMFLOAT3 eu = EulerOfQuat(e.rotation);
                if (track(ImGui::DragFloat3("Rotation", &eu.x, 0.5f, -360.f, 360.f, "%.1f")))
                {
                    e.rotation = QuatOfEuler(eu);
                    changed    = true;
                }
            }
            if (p.type == T::Box)
                changed |= track(ImGui::DragFloat3("Half extents", &e.halfExtents.x, 0.005f, kMin, 1e4f, "%.3f"));
            if (p.type == T::Sphere || p.type == T::Capsule)
                changed |= track(ImGui::DragFloat("Radius", &e.radius, 0.005f, kMin, 1e4f, "%.3f"));
            if (p.type == T::Capsule)
                changed |= track(ImGui::DragFloat("Half height", &e.halfHeight, 0.005f, kMin, 1e4f, "%.3f"));
            if (p.type == T::Hull)
            {
                const std::size_t shown = static_cast<std::size_t>(i) < m_primShapes.size()
                                        ? m_primShapes[static_cast<std::size_t>(i)].pts.size() / 3 : 0;
                if (pending) ImGui::TextDisabled("%zu points - appended to the collision library by Save", pending);
                else ImGui::TextDisabled("%s  sec %s  (%zu points)", p.lib.c_str(), Resource::MeshKeyToHex(p.sectionKey).c_str(),
                                         shown);
            }
            if (p.type == T::Mesh)
                ImGui::TextDisabled("%s  sec %s", p.lib.empty() ? "the Mesh's own section" : p.lib.c_str(),
                                    Resource::MeshKeyToHex(p.sectionKey).c_str());
            if (changed)
            {
                e.halfExtents = { (std::max)(e.halfExtents.x, kMin), (std::max)(e.halfExtents.y, kMin),
                                  (std::max)(e.halfExtents.z, kMin) };
                e.radius      = (std::max)(e.radius, kMin);
                e.halfHeight  = (std::max)(e.halfHeight, kMin);
                p = e;
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (duplicateAt >= 0)
    {
        std::string why;
        if (!DuplicateCollision(duplicateAt, why)) m_error = why;
    }
    if (removeAt >= 0)
    {
        std::string why;
        if (!RemoveCollision(removeAt, why)) m_error = why;
    }
    ImGui::EndDisabled();
    if (c.setup.prims.empty())
        ImGui::TextDisabled(c.setup.complexity == Resource::MeshBodySetup::Complexity::Complex
                                ? "No prims: the sections' own triangles are the collision."
                                : "No collision: Add fits a prim to the selected section or the whole Mesh.");
    ImGui::TextDisabled("new hulls go to %s", m_collisionLibrary.c_str());
}

void MeshEditorWindow::DrawStatsLine() const
{
    std::uint32_t tris = 0, verts = 0;
    for (const Section& s : m_sections) { tris += s.triangles; verts += s.vertices; }
    ImGui::TextDisabled("%u tris  |  %u verts  |  stride %u B  |  %.2f MB  |  bounds %.3f x %.3f x %.3f%s%s%s",
                        tris, verts, m_stride, static_cast<double>(m_geometryBytes) / (1024.0 * 1024.0),
                        m_boundsMax[0] - m_boundsMin[0], m_boundsMax[1] - m_boundsMin[1],
                        m_boundsMax[2] - m_boundsMin[2],
                        Skinned() ? "  |  skinned" : "",
                        (m_meshFlags & Resource::MESH_IDENT_HAS_MORPH) ? "  |  shape keys" : "",
                        m_overlayLines ? ("  |  overlay " + std::to_string(m_overlayLines) + " lines").c_str() : "");
}

void MeshEditorWindow::DrawAskModal()
{
    const char* id = "Unsaved mesh edits##mesheditor";
    if (m_askOpenPopup) { ImGui::OpenPopup(id); m_askOpenPopup = false; }
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("'%s' has unsaved edits (material slots, collision).", m_meshName.c_str());
    ImGui::TextDisabled(m_askFor == 1 ? "Save them before closing?" : "Save them before opening another mesh?");
    if (ImGui::Button("Save"))
    {
        m_saveQueued = true;
        if (m_askFor == 1) m_closeAfterSave = true;
        m_askFor = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard"))
    {
        m_staged = m_saved;
        m_undo.clear();
        m_redo.clear();
        if (m_askFor == 1) m_open = false;
        m_askFor = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        if (m_askFor == 2) m_openQueued = false;
        m_askFor = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ==== Overlays (CPU, projected like ChainPreviewOverlay) ====
std::uint32_t MeshEditorWindow::DrawOverlay(Renderer* renderer, ImDrawList* dl, float x, float y,
                                           float w, float h) const
{
    m_collisionLines = 0;
    if (!m_show || !renderer || w <= 0.f || h <= 0.f) return 0;
    float centre[3], scale = 1.f;
    if (!renderer->GetPreviewAssetFit(m_ref, centre, scale)) return 0;   // not composed yet
    const Cam cam = BuildCam(m_yaw, m_pitch, m_dist, m_pan, w / h);
    // Mesh space -> the fitted unit box -> clip.
    const XMMATRIX fit = XMMatrixMultiply(XMMatrixTranslation(-centre[0], -centre[1], -centre[2]),
                                          XMMatrixScaling(scale, scale, scale));
    const XMMATRIX mvp = XMMatrixMultiply(fit, cam.viewProj);
    auto project = [&](float px, float py, float pz, ImVec2& out) -> bool
    {
        const XMVECTOR c = XMVector4Transform(XMVectorSet(px, py, pz, 1.f), mvp);
        const float cw = XMVectorGetW(c);
        if (cw <= 1e-6f) return false;
        const float nx = XMVectorGetX(c) / cw, ny = XMVectorGetY(c) / cw;
        out = ImVec2(x + (nx * 0.5f + 0.5f) * w, y + (1.f - (ny * 0.5f + 0.5f)) * h);
        return true;
    };
    std::uint32_t lines = 0;
    auto line = [&](const float* a, const float* b, ImU32 col, float thick)
    {
        ImVec2 pa, pb;
        if (!project(a[0], a[1], a[2], pa) || !project(b[0], b[1], b[2], pb)) return;
        dl->AddLine(pa, pb, col, thick);
        ++lines;
    };

    const float* lo = m_boundsMin;
    const float* hi = m_boundsMax;
    const float  unit = 1.f / std::max(scale, 1e-6f);   // one unit-box length in mesh units

    if (m_show & kShowGrid)
    {
        const float cx = 0.5f * (lo[0] + hi[0]), cz = 0.5f * (lo[2] + hi[2]);
        const float half = 0.75f * std::max(hi[0] - lo[0], hi[2] - lo[2]) + 0.1f * unit;
        const int   n = 10;
        for (int i = 0; i <= n; ++i)
        {
            const float t = -half + 2.f * half * static_cast<float>(i) / n;
            const float a[3] = { cx + t, lo[1], cz - half }, b[3] = { cx + t, lo[1], cz + half };
            const float c[3] = { cx - half, lo[1], cz + t }, d[3] = { cx + half, lo[1], cz + t };
            const ImU32 col = (i == n / 2) ? IM_COL32(150, 150, 150, 170) : IM_COL32(110, 110, 110, 110);
            line(a, b, col, 1.f);
            line(c, d, col, 1.f);
        }
    }
    if (m_show & kShowBounds)
    {
        const ImU32 col = IM_COL32(255, 210, 70, 230);
        for (int e = 0; e < 12; ++e)
        {
            // The 12 edges: 4 along each axis.
            const int axis = e / 4, k = e % 4;
            float a[3], b[3];
            for (int c = 0; c < 3; ++c)
            {
                const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
                if (c == axis) { a[c] = lo[c]; b[c] = hi[c]; }
                else if (c == o1) a[c] = b[c] = (k & 1) ? hi[c] : lo[c];
                else if (c == o2) a[c] = b[c] = (k & 2) ? hi[c] : lo[c];
            }
            line(a, b, col, 1.5f);
        }
        // The bounding sphere, as its silhouette circle at the centre's depth.
        const float sc[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
        const float r = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                         (hi[2] - lo[2]) * (hi[2] - lo[2]));
        ImVec2 pc;
        const XMVECTOR c = XMVector4Transform(XMVectorSet(sc[0], sc[1], sc[2], 1.f), mvp);
        const float cw = XMVectorGetW(c);
        if (cw > 1e-6f && project(sc[0], sc[1], sc[2], pc))
        {
            const float focalPx = 0.5f * h / std::tan(Preview::kStudioCamera.fovY * 0.5f);
            dl->AddCircle(pc, r * scale * focalPx / cw, IM_COL32(255, 210, 70, 120), 64, 1.f);
            ++lines;
        }
    }
    if (m_show & kShowPivot)
    {
        const float L = 0.2f * unit;
        const float o[3] = { 0.f, 0.f, 0.f };
        const float ax[3] = { L, 0.f, 0.f }, ay[3] = { 0.f, L, 0.f }, az[3] = { 0.f, 0.f, L };
        line(o, ax, IM_COL32(240, 70, 70, 255), 2.f);
        line(o, ay, IM_COL32(80, 220, 80, 255), 2.f);
        line(o, az, IM_COL32(80, 130, 255, 255), 2.f);
    }

    // The staged sockets: a diamond each and its axes, the selected one larger, named.
    m_overlaySockets = 0;
    if (m_show & kShowSockets)
    {
        const int sel = SelectedSocket();
        const float L = 0.12f * unit;
        for (int i = 0; i < static_cast<int>(m_staged.sockets.size()); ++i)
        {
            const XMFLOAT4X4 m = m_staged.sockets[static_cast<std::size_t>(i)].LocalOffset();
            const float o[3] = { m._41, m._42, m._43 };
            ImVec2 q;
            if (!project(o[0], o[1], o[2], q)) continue;
            const bool isSel = i == sel;
            const float r = isSel ? 6.5f : 4.5f;
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y),
                              isSel ? kColSelected : kColSocket);
            dl->AddQuad(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y),
                        IM_COL32(0, 0, 0, 200), 1.f);
            const float rows[3][3] = { { m._11, m._12, m._13 }, { m._21, m._22, m._23 }, { m._31, m._32, m._33 } };
            const ImU32 cols[3] = { IM_COL32(240, 70, 70, 255), IM_COL32(80, 220, 80, 255), IM_COL32(80, 130, 255, 255) };
            for (int a = 0; a < 3; ++a)
            {
                const float len = std::sqrt(rows[a][0] * rows[a][0] + rows[a][1] * rows[a][1] + rows[a][2] * rows[a][2]);
                if (len < 1e-8f) continue;
                const float e[3] = { o[0] + rows[a][0] / len * L, o[1] + rows[a][1] / len * L, o[2] + rows[a][2] / len * L };
                line(o, e, cols[a], isSel ? 2.f : 1.5f);
            }
            if (isSel)
            {
                const char* text = m_staged.sockets[static_cast<std::size_t>(i)].name.c_str();
                dl->AddText(ImVec2(q.x + 9.f, q.y + 5.f), kColSocket, text);
            }
            ++m_overlaySockets;
        }
    }

    if ((m_show & kShowSkeleton) && !m_jointParent.empty())
    {
        for (std::size_t b = 0; b < m_jointParent.size(); ++b)
        {
            const int p = m_jointParent[b];
            if (p < 0 || static_cast<std::size_t>(p) >= m_jointParent.size()) continue;
            line(&m_joints[size_t(p) * 3], &m_joints[b * 3], IM_COL32(255, 150, 40, 235), 1.5f);
        }
    }

    const std::uint32_t vecBits = m_show & (kShowNormals | kShowTangents | kShowBitangents);
    if (vecBits)
    {
        // Sampled across the visible sections to stay under the cap; only
        // vertices facing the camera, since a line through the mesh reads as
        // clutter.
        std::uint64_t total = 0;
        for (std::size_t k = 0; k < m_sections.size(); ++k)
            if (m_visible[k]) total += m_sections[k].vertices;
        const std::uint32_t kinds = ((vecBits & kShowNormals) ? 1u : 0u) + ((vecBits & kShowTangents) ? 1u : 0u) +
                                    ((vecBits & kShowBitangents) ? 1u : 0u);
        const std::uint64_t budget = kMaxOverlayLines / std::max(1u, kinds);
        const std::uint64_t step   = std::max<std::uint64_t>(1, (total + budget - 1) / std::max<std::uint64_t>(1, budget));
        // The eye in mesh space: the fit is a uniform scale about the centre.
        XMFLOAT3 eyeU;
        XMStoreFloat3(&eyeU, cam.eye);
        const float eye[3] = { eyeU.x / scale + centre[0], eyeU.y / scale + centre[1], eyeU.z / scale + centre[2] };
        const float L = 0.035f * unit;
        std::uint64_t v = 0;
        for (std::size_t k = 0; k < m_sections.size(); ++k)
        {
            if (!m_visible[k]) continue;
            const Section& s = m_sections[k];
            for (std::uint32_t i = 0; i < s.vertices; ++i, ++v)
            {
                if (v % step) continue;
                const float* p = &s.pos[size_t(i) * 3];
                const float* n = &s.nrm[size_t(i) * 3];
                const float toEye[3] = { eye[0] - p[0], eye[1] - p[1], eye[2] - p[2] };
                if (n[0] * toEye[0] + n[1] * toEye[1] + n[2] * toEye[2] <= 0.f) continue;
                if (vecBits & kShowNormals)
                {
                    const float q[3] = { p[0] + n[0] * L, p[1] + n[1] * L, p[2] + n[2] * L };
                    line(p, q, IM_COL32(70, 200, 255, 200), 1.f);
                }
                if (!s.tan.empty() && (vecBits & (kShowTangents | kShowBitangents)))
                {
                    const float* t = &s.tan[size_t(i) * 4];
                    if (vecBits & kShowTangents)
                    {
                        const float q[3] = { p[0] + t[0] * L, p[1] + t[1] * L, p[2] + t[2] * L };
                        line(p, q, IM_COL32(255, 90, 90, 200), 1.f);
                    }
                    if (vecBits & kShowBitangents)
                    {
                        // B = cross(N, T) * w (the sign the shaders use).
                        const float b[3] = { (n[1] * t[2] - n[2] * t[1]) * t[3], (n[2] * t[0] - n[0] * t[2]) * t[3],
                                             (n[0] * t[1] - n[1] * t[0]) * t[3] };
                        const float q[3] = { p[0] + b[0] * L, p[1] + b[1] * L, p[2] + b[2] * L };
                        line(p, q, IM_COL32(110, 240, 110, 200), 1.f);
                    }
                }
            }
        }
    }

    // ---- Collision: the STAGED prims, in the Mesh's space ----
    if ((m_show & kShowCollision) && m_loaded)
    {
        const StagedCollision& c      = m_staged.collision;
        const std::uint32_t    before = lines;
        const bool none = c.setup.complexity == Resource::MeshBodySetup::Complexity::None;
        auto dim = [&](ImU32 col) { return none ? ((col & 0x00FFFFFFu) | 0x50000000u) : col; };
        auto seg = [&](XMVECTOR a, XMVECTOR b, ImU32 col, float thick)
        {
            XMFLOAT3 fa, fb;
            XMStoreFloat3(&fa, a);
            XMStoreFloat3(&fb, b);
            line(&fa.x, &fb.x, col, thick);
        };
        constexpr float kTau = 6.28318531f;
        for (std::size_t i = 0; i < c.setup.prims.size(); ++i)
        {
            if (lines - before > 2 * kMaxOverlayLines) break;
            const Resource::CollisionPrim& p = c.setup.prims[i];
            const bool  sel   = static_cast<int>(i) == m_collisionSelected;
            const ImU32 col   = sel ? kColSelected : dim(p.InBody() ? kColBody : kColTrimesh);
            const float thick = sel ? 2.f : 1.25f;
            const XMMATRIX M = XMMatrixMultiply(XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&p.rotation))),
                                                XMMatrixTranslation(p.center.x, p.center.y, p.center.z));
            auto P = [&](float px, float py, float pz) { return XMVector3TransformCoord(XMVectorSet(px, py, pz, 1.f), M); };
            switch (p.type)
            {
            case Resource::CollisionPrim::Type::Box:
            {
                const float he[3] = { p.halfExtents.x, p.halfExtents.y, p.halfExtents.z };
                for (int e = 0; e < 12; ++e)
                {
                    const int axis = e / 4, k = e % 4, o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
                    float a[3], b[3];
                    a[axis] = -he[axis];
                    b[axis] = he[axis];
                    a[o1] = b[o1] = (k & 1) ? he[o1] : -he[o1];
                    a[o2] = b[o2] = (k & 2) ? he[o2] : -he[o2];
                    seg(P(a[0], a[1], a[2]), P(b[0], b[1], b[2]), col, thick);
                }
                break;
            }
            case Resource::CollisionPrim::Type::Sphere:
            {
                const float r = p.radius;
                constexpr int N = 32;
                for (int k = 0; k < N; ++k)
                {
                    const float a0 = kTau * k / N, a1 = kTau * (k + 1) / N;
                    const float c0 = r * std::cos(a0), s0 = r * std::sin(a0), c1 = r * std::cos(a1), s1 = r * std::sin(a1);
                    seg(P(c0, s0, 0.f), P(c1, s1, 0.f), col, thick);
                    seg(P(c0, 0.f, s0), P(c1, 0.f, s1), col, thick);
                    seg(P(0.f, c0, s0), P(0.f, c1, s1), col, thick);
                }
                break;
            }
            case Resource::CollisionPrim::Type::Capsule:
            {
                // Along its local Y: the cylinder's end rings, four side lines and
                // the caps' half circles in the XY and ZY planes.
                const float r = p.radius, hh = p.halfHeight;
                constexpr int N = 24;
                for (int k = 0; k < N; ++k)
                {
                    const float a0 = kTau * k / N, a1 = kTau * (k + 1) / N;
                    for (const float y : { -hh, hh })
                        seg(P(r * std::cos(a0), y, r * std::sin(a0)), P(r * std::cos(a1), y, r * std::sin(a1)), col, thick);
                }
                for (int k = 0; k < 4; ++k)
                {
                    const float a = 0.25f * kTau * k;
                    seg(P(r * std::cos(a), -hh, r * std::sin(a)), P(r * std::cos(a), hh, r * std::sin(a)), col, thick);
                }
                for (int k = 0; k < N / 2; ++k)
                {
                    const float a0 = 0.5f * kTau * k / (N / 2), a1 = 0.5f * kTau * (k + 1) / (N / 2);
                    const float c0 = r * std::cos(a0), s0 = r * std::sin(a0), c1 = r * std::cos(a1), s1 = r * std::sin(a1);
                    seg(P(c0, hh + s0, 0.f), P(c1, hh + s1, 0.f), col, thick);
                    seg(P(0.f, hh + s0, c0), P(0.f, hh + s1, c1), col, thick);
                    seg(P(c0, -hh - s0, 0.f), P(c1, -hh - s1, 0.f), col, thick);
                    seg(P(0.f, -hh - s0, c0), P(0.f, -hh - s1, c1), col, thick);
                }
                break;
            }
            case Resource::CollisionPrim::Type::Hull:
            case Resource::CollisionPrim::Type::Mesh:
            {
                if (i >= m_primShapes.size()) break;   // built on the game half, next frame
                const PrimShape& s = m_primShapes[i];
                const std::size_t n = s.pts.size() / 3;
                for (std::size_t k = 0; k + 1 < s.lines.size(); k += 2)
                {
                    if (s.lines[k] >= n || s.lines[k + 1] >= n) continue;
                    const float* a = &s.pts[std::size_t(s.lines[k]) * 3];
                    const float* b = &s.pts[std::size_t(s.lines[k + 1]) * 3];
                    seg(P(a[0], a[1], a[2]), P(b[0], b[1], b[2]), col, thick);
                }
                break;
            }
            }
        }
        if (c.setup.complexity == Resource::MeshBodySetup::Complexity::Complex)
        {
            const PrimShape&  s = m_complexShape;
            const std::size_t n = s.pts.size() / 3;
            for (std::size_t k = 0; k + 1 < s.lines.size() && lines - before <= 2 * kMaxOverlayLines; k += 2)
                if (s.lines[k] < n && s.lines[k + 1] < n)
                    line(&s.pts[std::size_t(s.lines[k]) * 3], &s.pts[std::size_t(s.lines[k + 1]) * 3], kColTrimesh, 1.f);
        }
        m_collisionLines = lines - before;
    }
    return lines;
}

// ==== UV layout ====
void MeshEditorWindow::DrawUv(ImDrawList* dl, float x, float y, float size) const
{
    const int c = m_uvChannel;
    m_uvLastChannel = c;
    m_uvLastSection = m_uvSection;
    m_uvTris = m_uvDrawn = 0;
    float lo[2] = { 0.f, 0.f }, hi[2] = { 1.f, 1.f };
    float dataLo[2] = { FLT_MAX, FLT_MAX }, dataHi[2] = { -FLT_MAX, -FLT_MAX };
    auto wanted = [&](std::size_t k)
    {
        return (m_uvSection < 0 ? static_cast<bool>(m_visible[k]) : static_cast<int>(k) == m_uvSection) &&
               !m_sections[k].uv[c].empty();
    };
    // Vertex i's UV in channel c. -mesheditor_uv_off (the uv pack's reverse
    // arm) reads the POSITION's x/z instead: a layout drawn from the wrong
    // stream, which every number the pack bounds has to notice.
    auto uvOf = [&](const Section& s, std::uint32_t i, float out[2])
    {
        if (m_harnessUvOff) { out[0] = s.pos[size_t(i) * 3]; out[1] = s.pos[size_t(i) * 3 + 2]; return; }
        out[0] = s.uv[c][size_t(i) * 2];
        out[1] = s.uv[c][size_t(i) * 2 + 1];
    };
    for (std::size_t k = 0; k < m_sections.size(); ++k)
    {
        if (!wanted(k)) continue;
        const Section& s = m_sections[k];
        m_uvTris += s.triangles;
        for (std::size_t i = 0; i < s.idx.size(); ++i)
        {
            float uv[2];
            uvOf(s, s.idx[i], uv);
            for (int a = 0; a < 2; ++a)
            {
                dataLo[a] = std::min(dataLo[a], uv[a]);
                dataHi[a] = std::max(dataHi[a], uv[a]);
            }
        }
    }
    if (m_uvTris == 0)
    {
        m_uvMin[0] = m_uvMin[1] = m_uvMax[0] = m_uvMax[1] = 0.f;
        dl->AddText(ImVec2(x + 8.f, y + 8.f), IM_COL32(160, 160, 160, 255), "No UVs in this channel.");
        return;
    }
    m_uvMin[0] = dataLo[0]; m_uvMin[1] = dataLo[1];
    m_uvMax[0] = dataHi[0]; m_uvMax[1] = dataHi[1];
    // The view: 0..1 and the data, square, with a margin.
    for (int a = 0; a < 2; ++a) { lo[a] = std::min(lo[a], dataLo[a]); hi[a] = std::max(hi[a], dataHi[a]); }
    const float span = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 1.04f;
    const float ox = 0.5f * (lo[0] + hi[0]) - 0.5f * span, oy = 0.5f * (lo[1] + hi[1]) - 0.5f * span;
    auto map = [&](float u, float v) { return ImVec2(x + (u - ox) / span * size, y + (v - oy) / span * size); };

    // The 0..1 tile and its quarters (v down, as a texture is addressed).
    for (int i = 1; i < 4; ++i)
    {
        const float t = static_cast<float>(i) * 0.25f;
        dl->AddLine(map(t, 0.f), map(t, 1.f), IM_COL32(255, 255, 255, 40), 1.f);
        dl->AddLine(map(0.f, t), map(1.f, t), IM_COL32(255, 255, 255, 40), 1.f);
    }
    dl->AddRect(map(0.f, 0.f), map(1.f, 1.f), IM_COL32(255, 255, 255, 170), 0.f, 0, 1.5f);

    const std::uint32_t step = std::max(1u, (m_uvTris + kMaxUvTriangles - 1) / kMaxUvTriangles);
    std::uint32_t t = 0;
    for (std::size_t k = 0; k < m_sections.size(); ++k)
    {
        if (!wanted(k)) continue;
        const Section& s = m_sections[k];
        const ImU32 col = (SectionColour(static_cast<int>(k)) & 0x00FFFFFFu) | 0xC8000000u;
        for (std::size_t i = 0; i + 2 < s.idx.size(); i += 3, ++t)
        {
            if (t % step) continue;
            float a[2], b[2], d[2];
            uvOf(s, s.idx[i], a);
            uvOf(s, s.idx[i + 1], b);
            uvOf(s, s.idx[i + 2], d);
            dl->AddTriangle(map(a[0], a[1]), map(b[0], b[1]), map(d[0], d[1]), col, 1.f);
            ++m_uvDrawn;
        }
    }
}

long long MeshEditorWindow::WriteUvPng(const std::string& path, std::uint32_t px, std::string& why)
{
    if (!m_loaded) { why = "no mesh is open"; return -1; }
    if (!ImGui::GetCurrentContext()) { why = "no ImGui context"; return -1; }
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    const float s = static_cast<float>(px);
    dl.PushClipRect(ImVec2(0.f, 0.f), ImVec2(s, s), false);
    DrawUv(&dl, 0.f, 0.f, s);   // over black: `lit` counts the tile outline and the triangles
    std::uint32_t tris = 0;
    const long long lit = RasterizeDrawListToPng(dl, px, px, path, &tris, &why);
    if (lit >= 0)
        LOG_INFO("[MESHEDITOR] uv shot %s: file='%s' lit=%lld channel=%d section=%d tris=%u drawn=%u "
                 "umin=%.4f vmin=%.4f umax=%.4f vmax=%.4f outside01=%d", BaseName(path).c_str(), path.c_str(), lit,
                 m_uvLastChannel, m_uvLastSection,
                 m_uvTris, m_uvDrawn, m_uvMin[0], m_uvMin[1], m_uvMax[0], m_uvMax[1],
                 (m_uvMin[0] < 0.f || m_uvMin[1] < 0.f || m_uvMax[0] > 1.f || m_uvMax[1] > 1.f) ? 1 : 0);
    return lit;
}

long long MeshEditorWindow::WriteOverlayPng(Renderer* renderer, const std::string& path, std::string& why)
{
    if (!m_loaded || !renderer || m_viewId == ~0u) { why = "no view"; return -1; }
    if (!ImGui::GetCurrentContext()) { why = "no ImGui context"; return -1; }
    std::uint32_t w = 0, h = 0;
    if (!renderer->GetPreviewOutputSize(m_viewId, w, h) || !w || !h) { why = "no picture yet"; return -1; }
    EnsureCollisionShapes();
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    dl.PushClipRect(ImVec2(0.f, 0.f), ImVec2(static_cast<float>(w), static_cast<float>(h)), false);
    const std::uint32_t lines = DrawOverlay(renderer, &dl, 0.f, 0.f, static_cast<float>(w), static_cast<float>(h));
    std::uint32_t tris = 0;
    const long long lit = RasterizeDrawListToPng(dl, w, h, path, &tris, &why);
    if (lit >= 0)
        LOG_INFO("[MESHEDITOR] overlay shot %s: file='%s' lit=%lld lines=%u collisionLines=%u show=%s size=%ux%u",
                 BaseName(path).c_str(), path.c_str(), lit, lines, m_collisionLines, ShowNames(m_show).c_str(), w, h);
    return lit;
}

// ==== Harness ====
bool MeshEditorWindow::HarnessConfigure(std::string& why)
{
    const auto& cl = Sys::CommandLine::Get();
    static const char* kFlags[] = {
        "-mesheditor_size", "-mesheditor_view", "-mesheditor_show", "-mesheditor_sections",
        "-mesheditor_shot", "-mesheditor_overlay_shot", "-mesheditor_uv", "-mesheditor_uv_shot",
        "-mesheditor_orbit", "-mesheditor_close_at", "-mesheditor_viewmode_off", "-mesheditor_mask_off",
        "-mesheditor_uv_off",
    };
    const bool hasOpen = cl.Has("-mesheditor_open");
    for (const char* f : kFlags)
        if (cl.Has(f) && !hasOpen)
        {
            why = std::string(f) + " without -mesheditor_open=<model.iscn>#<key|name> has no subject";
            return false;
        }
    if (!hasOpen) return true;

    m_harnessOpen = Resource::AssetRefs::PercentDecode(cl.Get("-mesheditor_open"));
    if (m_harnessOpen.empty() || m_harnessOpen.find('#') == std::string::npos)
    {
        why = "-mesheditor_open needs <model.iscn>#<MeshKey hex | mesh name>";
        return false;
    }
    m_harnessViewModeOff = cl.Has("-mesheditor_viewmode_off");
    m_harnessMaskOff     = cl.Has("-mesheditor_mask_off");
    m_harnessUvOff       = cl.Has("-mesheditor_uv_off");

    if (cl.Has("-mesheditor_size"))
    {
        const std::string v = cl.Get("-mesheditor_size");
        unsigned w = 0, h = 0;
        if (sscanf_s(v.c_str(), "%ux%u", &w, &h) != 2 || w < 64 || h < 64 || w > 2048 || h > 2048)
        {
            why = "-mesheditor_size needs <w>x<h> (64..2048)";
            return false;
        }
        m_harnessW = w;
        m_harnessH = h;
    }

    auto list = [&](const char* flag, auto&& each) -> bool
    {
        if (!cl.Has(flag)) return true;
        const std::string v = cl.Get(flag);
        if (v.empty()) { why = std::string(flag) + " needs a value"; return false; }
        for (const std::string& item : SplitList(v, ','))
        {
            std::string value;
            std::uint64_t at = 0;
            if (!SplitAt(item, value, at)) { why = std::string(flag) + ": '" + item + "' is not <value>@<frame>"; return false; }
            if (!each(value, at)) { if (why.empty()) why = std::string(flag) + ": bad value '" + value + "'"; return false; }
        }
        return true;
    };
    auto push = [&](int kind, std::uint64_t at, std::string text, std::uint32_t value = 0, std::uint64_t mask = 0)
    {
        Step s;
        s.kind  = kind;
        s.at    = at;
        s.text  = std::move(text);
        s.value = value;
        s.mask  = mask;
        m_steps.push_back(std::move(s));
    };

    if (!list("-mesheditor_view", [&](const std::string& v, std::uint64_t at)
        {
            std::uint32_t mode = 0;
            if (!ParseViewMode(v, mode)) return false;
            push(kStepView, at, v, mode);
            return true;
        })) return false;
    if (!list("-mesheditor_show", [&](const std::string& v, std::uint64_t at)
        {
            std::uint32_t bits = 0;
            if (!ParseShow(v, bits)) return false;
            push(kStepShow, at, v, bits);
            return true;
        })) return false;
    if (!list("-mesheditor_sections", [&](const std::string& v, std::uint64_t at)
        {
            // all | solo:<i> | select:<i> | none (select none) | mask:<hex>
            if (v == "all" || v == "none") { push(kStepSections, at, v); return true; }
            const std::size_t c = v.find(':');
            if (c == std::string::npos) return false;
            const std::string what = v.substr(0, c), arg = v.substr(c + 1);
            if (what != "solo" && what != "select" && what != "mask") return false;
            char* end = nullptr;
            const unsigned long long n = std::strtoull(arg.c_str(), &end, what == "mask" ? 16 : 10);
            if (arg.empty() || !end || *end) return false;
            push(kStepSections, at, what, static_cast<std::uint32_t>(n), n);
            return true;
        })) return false;
    if (!list("-mesheditor_shot", [&](const std::string& v, std::uint64_t at)
        { push(kStepShot, at, HarnessPath(v)); return true; })) return false;
    if (!list("-mesheditor_overlay_shot", [&](const std::string& v, std::uint64_t at)
        { push(kStepOverlayShot, at, HarnessPath(v)); return true; })) return false;
    if (!list("-mesheditor_uv", [&](const std::string& v, std::uint64_t at)
        {
            int ch = -1, sec = -1;
            if (sscanf_s(v.c_str(), "%d:%d", &ch, &sec) < 1 || ch < 0 || ch > 3) return false;
            push(kStepUv, at, v, static_cast<std::uint32_t>(ch), static_cast<std::uint64_t>(static_cast<std::int64_t>(sec)));
            return true;
        })) return false;
    if (!list("-mesheditor_uv_shot", [&](const std::string& v, std::uint64_t at)
        { push(kStepUvShot, at, HarnessPath(v)); return true; })) return false;
    if (!list("-mesheditor_orbit", [&](const std::string& v, std::uint64_t at)
        {
            // <yaw>:<pitch>:<dist>[:<panx>:<pany>:<panz>] (radians, the distance
            // scale, the pan in the fitted unit box).
            float y = 0.f, p = 0.f, d = 1.f, px = 0.f, py = 0.f, pz = 0.f;
            const int n = sscanf_s(v.c_str(), "%f:%f:%f:%f:%f:%f", &y, &p, &d, &px, &py, &pz);
            if (n != 3 && n != 6) return false;
            push(kStepOrbit, at, v);
            return true;
        })) return false;
    if (cl.Has("-mesheditor_close_at"))
    {
        const std::string v = cl.Get("-mesheditor_close_at");
        if (v.empty() || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        {
            why = "-mesheditor_close_at needs an editor frame";
            return false;
        }
        push(kStepClose, std::strtoull(v.c_str(), nullptr, 10), "close");
    }
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
    LOG_INFO("[MESHEDITOR] harness: open='%s' steps=%zu size=%ux%u viewmodeOff=%d maskOff=%d", m_harnessOpen.c_str(),
             m_steps.size(), m_harnessW, m_harnessH, m_harnessViewModeOff ? 1 : 0, m_harnessMaskOff ? 1 : 0);
    return true;
}

void MeshEditorWindow::TickHarness(const MeshEditorContext& ctx)
{
    if (m_harnessOpen.empty() || m_harnessFailed) return;
    if (!m_harnessOpened)
    {
        m_harnessOpened = true;
        const std::size_t hash = m_harnessOpen.find_last_of('#');
        std::string code, why;
        if (!OpenNow(ctx, m_harnessOpen.substr(0, hash), m_harnessOpen.substr(hash + 1), true, code, why))
        {
            m_harnessFailed = true;
            LOG_ERROR("-mesheditor_open='%s' cannot open the Mesh Editor (%s): %s", m_harnessOpen.c_str(),
                      code.c_str(), why.c_str());
            return;
        }
    }
    // Picture freshness for the shots: rendered since the description last
    // changed, and no new render for kSettleFrames frames (streamed textures
    // re-render the view once they arrive).
    const std::uint64_t renders = (ctx.renderer && m_viewId != ~0u)
                                ? ctx.renderer->GetPreviewViewRenderCount(m_viewId) : 0;
    if (renders != m_lastRenders) { m_lastRenders = renders; m_lastRendersFrame = ctx.editorFrame; }
    bool settled = m_viewId != ~0u && renders > m_rendersAtDesc &&
                   ctx.editorFrame >= m_lastRendersFrame + kSettleFrames;

    for (Step& s : m_steps)
    {
        if (s.done || ctx.editorFrame < s.at) continue;
        // A step that changes the PICTURE unsettles it for the rest of this
        // tick: the new description reaches the view in this frame's Draw, so
        // a shot on the same editor frame must wait for its render, not take
        // the previous one.
        if (s.kind == kStepView || s.kind == kStepSections || s.kind == kStepOrbit) settled = false;
        switch (s.kind)
        {
        case kStepView:
            SetViewMode(s.value);
            LOG_INFO("[MESHEDITOR] harness view -> %s (frame %llu)", ViewModeName(s.value),
                     static_cast<unsigned long long>(ctx.editorFrame));
            s.done = true;
            break;
        case kStepShow:
            SetShow(s.value);
            s.done = true;
            break;
        case kStepSections:
            if (s.text == "all") Solo(-1);
            else if (s.text == "none") Select(-1);
            else if (s.text == "solo") Solo(static_cast<int>(s.value));
            else if (s.text == "select") Select(static_cast<int>(s.value));
            else if (s.text == "mask") SetVisibleMask(s.mask);
            LOG_INFO("[MESHEDITOR] harness sections -> %s%s mask=%llx selected=%d", s.text.c_str(),
                     (s.text == "solo" || s.text == "select") ? (":" + std::to_string(s.value)).c_str() : "",
                     static_cast<unsigned long long>(VisibleMask()), m_selected);
            s.done = true;
            break;
        case kStepOrbit:
        {
            float y = 0.f, p = 0.f, d = 1.f, px = 0.f, py = 0.f, pz = 0.f;
            sscanf_s(s.text.c_str(), "%f:%f:%f:%f:%f:%f", &y, &p, &d, &px, &py, &pz);
            SetOrbit(y, p, d);
            SetPan(px, py, pz);
            s.done = true;
            break;
        }
        case kStepUv:
            SetUv(static_cast<int>(s.value), static_cast<int>(static_cast<std::int64_t>(s.mask)));
            s.done = true;
            break;
        case kStepUvShot:
        {
            std::string why;
            if (WriteUvPng(s.text, 512, why) < 0)
            {
                m_harnessFailed = true;
                LOG_ERROR("-mesheditor_uv_shot '%s' failed: %s", s.text.c_str(), why.c_str());
            }
            s.done = true;
            break;
        }
        case kStepShot:
        case kStepOverlayShot:
        {
            if (!settled || !ctx.renderer)
            {
                if (ctx.editorFrame > s.at + kShotDeadline)
                {
                    s.done = true;
                    m_harnessFailed = true;
                    LOG_ERROR("[MESHEDITOR] shot '%s' at editor frame %llu: the view never settled "
                              "(renders=%llu sinceDesc=%llu)", s.text.c_str(),
                              static_cast<unsigned long long>(s.at), static_cast<unsigned long long>(renders),
                              static_cast<unsigned long long>(m_rendersAtDesc));
                }
                break;
            }
            s.done = true;
            std::uint32_t parts = 0, mats = 0, dropped = 0;
            ctx.renderer->GetPreviewAssetStats(m_viewId, parts, mats, dropped);
            if (s.kind == kStepShot)
            {
                const RHI::Texture* tex = ctx.renderer->GetPreviewOutputTexture(m_viewId);
                const bool ok = tex && ctx.gfx &&
                                ctx.gfx->CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, s.text.c_str());
                if (!ok) m_harnessFailed = true;
                LOG_INFO("[MESHEDITOR] shot %s: file='%s' written=%d mode=%s parts=%u materials=%u mask=%llx "
                         "selected=%d frame=%llu renders=%llu", BaseName(s.text).c_str(), s.text.c_str(), ok ? 1 : 0,
                         ViewModeName(m_viewMode), parts, mats, static_cast<unsigned long long>(VisibleMask()),
                         m_selected, static_cast<unsigned long long>(ctx.editorFrame),
                         static_cast<unsigned long long>(renders));
            }
            else
            {
                std::string why;
                if (WriteOverlayPng(ctx.renderer, s.text, why) < 0)
                {
                    m_harnessFailed = true;
                    LOG_ERROR("-mesheditor_overlay_shot '%s' failed: %s", s.text.c_str(), why.c_str());
                }
            }
            break;
        }
        case kStepClose:
        {
            std::string why;
            CloseNow(true, why);
            s.done = true;
            break;
        }
        default:
            s.done = true;
            break;
        }
    }
}

} // namespace Editor
