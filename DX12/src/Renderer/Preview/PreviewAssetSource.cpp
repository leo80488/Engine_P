// PreviewAssetSource.cpp - turns a `.meshlib`, `.ipfb`, `.iscn` or Mesh asset ref on disk into
// a flat list of Preview::AssetPart, on the GAME half.
//
// Not a private World: spawning the prefab into a second World would give entity-keyed material
// runtimes, but the preview's by-key material entry point already covers that, and the rest is
// cost - ECS::GuidRegistry is a process singleton with one World* hook,
// MeshDescriptorHeap::OnWorldClear does a FlushAndWait, and a second heap could only serve its
// own World. So the composition is done as data here and handed over as packets.
//
// The three inputs and what each is authoritative for:
//   `.meshlib`          the geometry: one library entry per part, its AABB.
//   sibling `.iscn`     the default material of each library entry (its F-lines are the only
//                       place that binding is recorded) and, for a prefab, each part's placement.
//   `.ipfb`             the node tree and the per-part material overrides.
//
// Overrides are applied as PrefabSerializer::LoadPrefab applies them: by section key, else by
// position (entityMap[sceneIdx + i] = loadedEntities[i], every component block through that
// map). Matching by name would disagree with the thing this previews; LoadPrefab already warns
// when the saved names have drifted.

#include "Renderer/Renderer.h"

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"        // MeshLibRef - RegisterMeshLibMesh's key
#include "ECS/HierarchyOps.h"               // -preview_asset_verify tears its spawn down
#include "Resource/AssetManager.h"          // -preview_asset_verify spawns through it
#include "Scene/TransformSystem.h"          // ... and propagates before comparing
#include "Renderer/Preview/PreviewTypes.h"
#include "Resource/AssetFS.h"
#include "Resource/MaterialOverrideApply.h"  // the prefab's override layer
#include "Resource/MaterialSerializer.h"
#include "Resource/MeshLibIdentity.h"       // MeshAsset refs ("<model>#<keyhex>")
#include "Resource/MeshLibrary.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/ProceduralMesh.h"        // PrimitiveMeshType::Count
#include "Scene/MeshAssetRegistry.h"        // a Mesh item composes from BuildModel's view
#include "Scene/SceneInstanceLoader.h"
#include "System/Check.h"
#include "System/CommandLine.h"             // -preview_meshasset_whole_library
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/ThreadContext.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>   // std::size, for the primitive-name static_assert
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace
{
    std::string LowerExt(const std::string& path)
    {
        const std::size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return {};
        std::string e = path.substr(dot);
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e;
    }

    // Same-stem sibling: `<dir>/<stem>.iscn` next to `<dir>/<stem>.meshlib`, the importer's
    // convention (SceneImporter writes the two together). It is the only way a bare .meshlib finds
    // its entries' materials: MeshLibrary::Entry::defaultMaterialIdx indexes the source model's
    // material table and resolves to nothing at runtime.
    std::string SiblingIscnFor(const std::string& meshlibPath)
    {
        std::filesystem::path p = Sys::U8Path(meshlibPath);
        p.replace_extension(".iscn");
        return Sys::RelativizeAssetPath(Sys::PathToUtf8(p));
    }

    // The comparison key for one mesh reference: canonical path + '#' + meshId. The two sides spell
    // a path differently by design (a live MeshSourcePath is native-separator, un-relativised and
    // keeps a prefab's `../`; a placement is project-relative and lexically normal), so the key
    // uses PreviewLibKey's canonicalisation, case kept: both sides come from the same `.iscn`
    // `file=` token, so a case fold could only make the check looser than the maps it checks.
    // Lexical normalisation is required: RelativizeAssetPath alone gives a `../` part two names.
    std::string MeshKeyOf(const std::string& path, uint32_t meshId)
    {
        return Resource::MeshLibCanonicalPath(path) + '#' + std::to_string(meshId);
    }

    // ---- The prefab's three material layers ----
    // Replayed in LoadPrefab's order, which is the whole correctness claim:
    //   1. `matPath`        the .iscn F-line's `mat=`, already replaced by the node's MaterialRef
    //                       if it has one (ComposePrefabAsset does that).
    //   2. MaterialInline   replaces the component outright: its deserializer overwrites an
    //                       existing MaterialComponent, and the component-block walk applies
    //                       MaterialRef before MaterialInline (registration order, the order
    //                       SavePrefab writes them in).
    //   3. MaterialOverride applied on top by LoadPrefab's post-process loop, so it has the last
    //                       word whatever the line order in the file.
    // A wrong order is invisible in the picture until a prefab carries two layers at once, which is
    // the case the fixture builds.
    void ReplayPlacementMaterial(const Preview::AssetPlacement& pl,
                                 MaterialComponent& out)
    {
        if (!pl.matPath.empty())
        {
            if (!Resource::LoadMaterial(pl.matPath, out, nullptr))
                LOG_WARNING("[PREVIEW] asset part material '%s' failed to load - "
                            "drawing the engine default", pl.matPath.c_str());
        }
        if (!pl.matInlineText.empty())
        {
            // A whole material, not a diff: default-construct first so nothing of the ref survives, as the
            // MaterialInline deserializer's `MaterialComponent mat{}` does.
            MaterialComponent inlineMat{};
            Resource::ApplyMaterialTextInPlace(inlineMat, pl.matInlineText);
            out = std::move(inlineMat);
        }
        if (pl.hasMatOverride)
            Resource::ApplyMaterialOverride(out, pl.matOverride);
    }

    // The cache key for ReplayPlacementMaterial's result. matPath alone is not a key: two parts can
    // share one .imat and differ only in the bag on top. The bag is an unordered_map, so its entries
    // are sorted: two equal overrides must produce one key whatever the hash order.
    std::string PlacementMaterialKey(const Preview::AssetPlacement& pl)
    {
        if (pl.matInlineText.empty() && !pl.hasMatOverride) return pl.matPath;

        std::vector<std::string> parts;
        parts.reserve(pl.matOverride.props.size());
        for (const auto& [key, val] : pl.matOverride.props)
        {
            char buf[192];
            switch (val.type)
            {
            case MaterialOverride::Value::Float:
                snprintf(buf, sizeof(buf), "f:%.9g", val.data.f); break;
            case MaterialOverride::Value::Vec4:
                snprintf(buf, sizeof(buf), "v:%.9g_%.9g_%.9g_%.9g",
                         val.data.v4[0], val.data.v4[1],
                         val.data.v4[2], val.data.v4[3]); break;
            case MaterialOverride::Value::Int:
                snprintf(buf, sizeof(buf), "i:%d", val.data.i); break;
            case MaterialOverride::Value::Bool:
                snprintf(buf, sizeof(buf), "b:%d", val.data.b ? 1 : 0); break;
            case MaterialOverride::Value::TexRef:
                snprintf(buf, sizeof(buf), "t:"); break;
            default:
                buf[0] = '\0'; break;
            }
            std::string entry = key + '=' + buf;
            if (val.type == MaterialOverride::Value::TexRef) entry += val.texPath;
            parts.push_back(std::move(entry));
        }
        std::sort(parts.begin(), parts.end());

        std::string key = pl.matPath;
        key += "\x1f";
        key += pl.matInlineText;
        for (const std::string& s : parts) { key += "\x1f"; key += s; }
        return key;
    }

    // The canonical mesh-file key, the same form as PreviewLibKey and MeshLibrary::HashPath, case
    // kept: m_previewMeshLibs, the library's dedup and the `.ipak` index are case-sensitive, so a
    // case fold could only merge paths the layers below treat as two files.
    std::string NormalisedMeshKey(const std::string& p)
    {
        return Resource::MeshLibCanonicalPath(p);
    }

    // The one spelling m_previewMeshLibs, MeshLibrary::Load and MeshLibrary::HashPath all use:
    // Resource::MeshLibCanonicalPath (lexically normal, then RelativizeAssetPath). It collapses the
    // spellings that reach the map - a browser tile's working-directory-relative or absolute path
    // with native separators, a prefab part's project-relative form - which would otherwise be two
    // keys, two uploads, and an InvalidatePreviewAsset that finds only one. Being HashPath's own
    // function, a spelling the library folds into one slot (a typed `../`) cannot become two
    // entries here.
    // Do not lower-case it: AssetFS looks a packaged asset up by the exact string, and a lower-cased
    // name resolves to nothing inside a `.pak` even though it opens on NTFS.
    std::string PreviewLibKey(const std::string& p)
    {
        return Resource::MeshLibCanonicalPath(p);
    }
}

// One definition, the same as PreviewLibKey's, so there is no second convention to drift from.
std::string Renderer::PreviewAssetKey(const std::string& assetPath)
{
    return Resource::MeshLibCanonicalPath(assetPath);
}

Preview::AssetKind Preview::AssetKindForPath(const std::string& path)
{
    std::string model;
    uint64_t    key = 0;
    if (Resource::ParseMeshAssetRef(path, model, key)) return AssetKind::MeshAsset;
    const std::string ext = LowerExt(path);
    if (ext == ".meshlib") return AssetKind::Mesh;
    if (ext == ".ipfb")    return AssetKind::Prefab;
    if (ext == ".iscn")    return AssetKind::Model;
    return AssetKind::None;
}

// ==== The cacheable half: what the files say, with nothing resolved ====
namespace
{
    void ComposeMeshAsset(const std::string& meshlibPath, uint32_t meshCount,
                          Preview::AssetComposition& comp)
    {
        comp.placements.clear();
        comp.sourceFiles.clear();
        comp.sourceFiles.push_back(meshlibPath);
        if (meshCount == 0) return;

        // Materials from the sibling scene, keyed by (mesh file, meshId) - the composite key
        // ReadMaterialBindings builds its own `byMesh` map with - never by meshId alone: a .iscn can
        // reference several mesh files (an older import emits one .imsh per F-line, each meshId 0), and
        // an id-only key would hand entry N of this library whichever file's binding was read first.
        // Missing or unreadable is not an error: the parts draw with the engine default material, a
        // legitimate picture of a raw mesh.
        const std::string libKey = NormalisedMeshKey(meshlibPath);
        std::unordered_map<uint32_t, std::string> matByMeshId;
        {
            std::vector<SceneInstanceLoader::MaterialBinding> bindings;
            const std::string iscn = SiblingIscnFor(meshlibPath);
            if (!iscn.empty() && Resource::AssetFS::Get().Exists(iscn) &&
                SceneInstanceLoader::ReadMaterialBindings(iscn, bindings))
            {
                comp.sourceFiles.push_back(iscn);
                // Ambiguity is recomputed here as well as read from the binding: ReadMaterialBindings flags
                // several F-lines naming this (file, meshId) with different materials, this loop flags several
                // bindings collapsing onto one composite key once normalised. Either means the entry has no
                // single material and the preview must not pick.
                std::unordered_map<uint32_t, bool> ambiguousId;
                for (const auto& b : bindings)
                {
                    // Only this library's own bindings. A binding naming a
                    // different mesh file addresses a different entry table.
                    if (NormalisedMeshKey(b.meshAbsPath) != libKey) continue;
                    // AMBIGUOUS ENTRIES ARE SKIPPED, not guessed. An Unreal
                    // level import points several F-lines at one library entry
                    // with different materials on purpose; picking one would
                    // show a material the asset does not uniquely have.
                    if (b.ambiguous) { ambiguousId[b.meshId] = true; continue; }
                    if (b.matPath.empty()) continue;
                    auto ins = matByMeshId.emplace(b.meshId, b.matPath);
                    if (!ins.second && ins.first->second != b.matPath)
                        ambiguousId[b.meshId] = true;
                }
                for (const auto& kv : ambiguousId)
                    if (kv.second) matByMeshId.erase(kv.first);
            }
        }

        // ONE PART PER LIBRARY ENTRY, all at the origin. A .meshlib is a
        // container, not a scene: its entries share one coordinate space and
        // the placements that would separate them live in a .iscn. Using that
        // .iscn's placements instead would make a bare-mesh preview of a level
        // import into a preview of the level, which is not what the browser
        // tile is asking for.
        comp.placements.reserve(meshCount);
        for (uint32_t id = 0; id < meshCount; ++id)
        {
            Preview::AssetPlacement p;
            p.meshPath = meshlibPath;
            p.meshId   = id;
            auto it = matByMeshId.find(id);
            if (it != matByMeshId.end()) p.matPath = it->second;
            comp.placements.push_back(std::move(p));
        }
    }

    void ComposePrefabAsset(const std::string& prefabPath,
                            Preview::AssetComposition& comp)
    {
        comp.placements.clear();
        comp.sourceFiles.clear();
        comp.sourceFiles.push_back(prefabPath);

        std::vector<Resource::PrefabNodeView> nodes;
        // The skip tally rides the same parse: a second ReadPrefabNodes call for the counts would be a
        // second answer to the same question.
        if (!Resource::ReadPrefabNodes(prefabPath, nodes, &comp.skipped) ||
            nodes.empty()) return;

        // ---- 1. Expand every SceneRef node ----
        // LoadPrefab maps a spawned .iscn subtree onto prefab node indices
        // [sceneIdx .. sceneIdx + N - 1] in DFS order, so by position a placement's
        // dfsIndex plus the owning node's idx is the prefab node index whose saved
        // MaterialRef overrides it; a section-key match (below) takes precedence.
        std::unordered_map<int, std::string> overrideByNodeIdx;
        for (const Resource::PrefabNodeView& n : nodes)
            if (!n.materialRefPath.empty())
                overrideByNodeIdx.emplace(n.idx, n.materialRefPath);

        // The other two material layers (MaterialInline / MaterialOverride), keyed the same way. A
        // pointer into `nodes` rather than a copy of the bag: `nodes` outlives the loop below and a
        // MaterialOverride is an unordered_map.
        std::unordered_map<int, const Resource::PrefabNodeView*> matLayersByNodeIdx;
        for (const Resource::PrefabNodeView& n : nodes)
            if (n.hasMaterialOverride || !n.materialInlineText.empty())
                matLayersByNodeIdx.emplace(n.idx, &n);

        bool anyScene = false;
        for (const Resource::PrefabNodeView& n : nodes)
        {
            if (n.sceneRefPath.empty()) continue;
            anyScene = true;

            std::vector<SceneInstanceLoader::MeshPlacement> placements;
            if (!SceneInstanceLoader::ReadMeshPlacements(n.sceneRefPath, placements))
                continue;
            comp.sourceFiles.push_back(n.sceneRefPath);

            // Which saved node each part is, by its section key: LoadPrefab's match
            // (Resource::MatchPrefabPartsByKey) over the same saved subtree (parent links) and the same live
            // keys, so the two cannot disagree.
            std::unordered_map<uint32_t, int> nodeOfOrdinal;   // dfsIndex -> prefab node idx
            std::unordered_set<int>           keyedNodes;
            {
                std::unordered_map<int, std::vector<int>> kids;
                for (const Resource::PrefabNodeView& v : nodes) kids[v.parent].push_back(v.idx);
                std::unordered_set<int> subtree;
                std::vector<int>        stack{ n.idx };
                while (!stack.empty())
                {
                    const int i = stack.back();
                    stack.pop_back();
                    if (!subtree.insert(i).second) continue;
                    if (auto k = kids.find(i); k != kids.end()) stack.insert(stack.end(), k->second.begin(), k->second.end());
                }
                std::vector<std::pair<int, uint64_t>> saved;
                for (const Resource::PrefabNodeView& v : nodes)
                    if (v.sectionKey != 0 && subtree.count(v.idx)) saved.emplace_back(v.idx, v.sectionKey);
                if (!saved.empty())
                {
                    uint32_t maxOrd = 0;
                    for (const auto& mp : placements) maxOrd = (std::max)(maxOrd, mp.dfsIndex);
                    std::vector<uint64_t> liveKeys(static_cast<size_t>(maxOrd) + 1, 0);
                    for (const auto& mp : placements)
                        liveKeys[mp.dfsIndex] = SceneInstanceLoader::PlacementSectionKey(mp);
                    const Resource::PrefabPartMatch m = Resource::MatchPrefabPartsByKey(saved, n.idx, liveKeys);
                    for (const auto& [idx, ord] : m.nodeToLive) nodeOfOrdinal[static_cast<uint32_t>(ord)] = idx;
                    for (const auto& s : saved) keyedNodes.insert(s.first);
                }
            }

            const XMMATRIX nodeWorld =
                XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(n.world));

            for (const auto& mp : placements)
            {
                Preview::AssetPlacement p;
                p.meshPath     = mp.meshPath;
                p.meshId       = mp.meshId;
                p.sectionKey   = mp.sectionKey;
                p.matPath      = mp.matPath;
                p.sceneNodeIdx = n.idx;
                p.dfsIndex     = mp.dfsIndex;

                // The prefab's saved override for this part, if it has one: the
                // node its key matched, else the one at its ordinal - unless that
                // node is a keyed part that went elsewhere (or is gone).
                int prefabNodeIdx = n.idx + static_cast<int>(mp.dfsIndex);
                if (const auto k = nodeOfOrdinal.find(mp.dfsIndex); k != nodeOfOrdinal.end())
                    prefabNodeIdx = k->second;
                else if (keyedNodes.count(prefabNodeIdx))
                    prefabNodeIdx = -1;   // no saved node is this part
                auto ov = overrideByNodeIdx.find(prefabNodeIdx);
                if (ov != overrideByNodeIdx.end() && !ov->second.empty())
                    p.matPath = ov->second;

                // And its MaterialInline / MaterialOverride blocks, at the same prefab node index: the only key
                // a prefab's per-part material data is applied by.
                auto ml = matLayersByNodeIdx.find(prefabNodeIdx);
                if (ml != matLayersByNodeIdx.end())
                {
                    p.matInlineText  = ml->second->materialInlineText;
                    p.matOverride    = ml->second->materialOverride;
                    p.hasMatOverride = ml->second->hasMaterialOverride;
                }

                // Replace, not compose, for the .iscn root's own transform: LoadPrefab overwrites the spawned
                // root's LocalTransform with this N-line's, so `mp.world` excludes it
                // (SceneInstanceLoader::ReadMeshPlacements drops it) and this multiply reinstates it exactly
                // once. Composing both would apply an authored root offset (Rossi_Base's (18, 0.9, -1)) twice.
                const XMMATRIX local =
                    XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(mp.world));
                XMFLOAT4X4 m;
                XMStoreFloat4x4(&m, XMMatrixMultiply(local, nodeWorld));
                std::memcpy(p.world, &m, sizeof(float) * 16);
                if (std::find(comp.sourceFiles.begin(), comp.sourceFiles.end(),
                              p.meshPath) == comp.sourceFiles.end())
                    comp.sourceFiles.push_back(p.meshPath);
                comp.placements.push_back(std::move(p));
            }
        }

        // ---- 2. Primitive nodes ----
        // A "MeshHandle: gpuMeshID=N" node is a primitive placement (the DemoLoot shape), the only other
        // geometry a .ipfb can carry: MeshRef is a display hint the loader does not resolve
        // (ComponentSerializers.cpp says so where MeshSourcePath is read). These nodes become placements
        // of the primitive flavour, the same picture the runtime spawns; without them a DemoLoot-shaped
        // prefab composes 0 placements.
        // Not guarded by `!anyScene`: a prefab may carry both a SceneRef and a MeshHandle node
        // (TEST_NAME.ipfb, once its UI nodes are counted), and the two flavours compose into one part
        // list in node order.
        // sceneNodeIdx / dfsIndex = the node's own index and 0: a primitive node expands into exactly
        // itself, which also lets the order self-check (-preview_asset_verify) skip these rather than
        // mis-pair them.
        int primitives = 0;
        for (const Resource::PrefabNodeView& n : nodes)
        {
            if (n.gpuMeshId < 0) continue;
            ++primitives;
            Preview::AssetPlacement p;
            p.primitiveId  = n.gpuMeshId;
            p.matPath      = n.materialRefPath;   // on a MeshHandle node it IS the material
            p.sceneNodeIdx = n.idx;
            p.dfsIndex     = 0;
            if (n.hasMaterialOverride || !n.materialInlineText.empty())
            {
                p.matInlineText  = n.materialInlineText;
                p.matOverride    = n.materialOverride;
                p.hasMatOverride = n.hasMaterialOverride;
            }
            // ALREADY COMPOSED THROUGH THE PARENT CHAIN by ReadPrefabNodes, the
            // same world matrix the SceneRef branch multiplies its .iscn-local
            // placement by. There is no second transform to reinstate here: a
            // MeshHandle node IS the leaf.
            std::memcpy(p.world, n.world, sizeof(float) * 16);
            comp.placements.push_back(std::move(p));
        }
        if (primitives > 0)
            LOG_INFO("[PREVIEW] prefab '%s' places %d primitive mesh(es) "
                     "(MeshHandle/gpuMeshID)%s", prefabPath.c_str(), primitives,
                     anyScene ? " alongside its .iscn parts" : " and no .iscn");

        // ---- What was declined, once per composition ----
        // Only when there is something to say: a clean prefab stays silent, which makes the line's
        // presence meaningful and keeps existing asset packs' output unchanged. On the memoised path, so
        // one line per asset per session, not per frame.
        if (comp.skipped.Total() > 0)
            LOG_INFO("[PREVIEW] prefab '%s' skipped: emitters=%u trails=%u "
                     "lights=%u decals=%u billboards=%u surfaces=%u worldUI=%u "
                     "(counted per NODE - a node carrying ParticleRender + "
                     "ParticleEmit is ONE emitter; none of these are drawn by "
                     "the preview)",
                     prefabPath.c_str(), comp.skipped.emitters,
                     comp.skipped.trails, comp.skipped.lights,
                     comp.skipped.decals, comp.skipped.billboards,
                     comp.skipped.surfaces, comp.skipped.worldUI);
    }

    // A model (.iscn) is exactly the subtree a prefab's SceneRef node spawns, so it composes as
    // ComposePrefabAsset's SceneRef branch does (ReadMeshPlacements, each part at its N-node's world,
    // the root's own transform excluded), with the prefab node at the identity and no override layer.
    void ComposeModelAsset(const std::string& iscnPath, Preview::AssetComposition& comp)
    {
        comp.placements.clear();
        comp.sourceFiles.clear();
        comp.sourceFiles.push_back(iscnPath);

        std::vector<SceneInstanceLoader::MeshPlacement> placements;
        if (!SceneInstanceLoader::ReadMeshPlacements(iscnPath, placements)) return;
        comp.placements.reserve(placements.size());
        for (const SceneInstanceLoader::MeshPlacement& mp : placements)
        {
            Preview::AssetPlacement p;
            p.meshPath   = mp.meshPath;
            p.meshId     = mp.meshId;
            p.sectionKey = mp.sectionKey;
            p.matPath    = mp.matPath;
            p.dfsIndex   = mp.dfsIndex;
            std::memcpy(p.world, mp.world, sizeof(float) * 16);
            if (std::find(comp.sourceFiles.begin(), comp.sourceFiles.end(),
                          p.meshPath) == comp.sourceFiles.end())
                comp.sourceFiles.push_back(p.meshPath);
            comp.placements.push_back(std::move(p));
        }
    }
}

// Takes the reference the way the `.meshlib` composition does: ResolvePreviewMeshEntry registers
// entry 0 and caches the handle in m_previewMeshLibs (one Load per session).
Resource::Handle Renderer::PreviewLibraryHandle(const std::string& meshlibPath)
{
    SceneInstanceLoader::BatchScope _batch(&m_gfx);
    uint32_t slot = RHI::kInvalidBufferIndex, indexCount = 0;
    float lo[3] = { 0.f, 0.f, 0.f }, hi[3] = { 0.f, 0.f, 0.f };
    (void)ResolvePreviewMeshEntry(meshlibPath, 0, slot, indexCount, lo, hi);
    const auto it = m_previewMeshLibs.find(PreviewLibKey(meshlibPath));
    return it != m_previewMeshLibs.end() ? it->second.handle : Resource::Handle{};
}

// The StaticMesh registry's view, built over preview references: the same identity and default
// materials, but no library becomes a world library because a picture was asked for.
bool Renderer::BuildPreviewModelView(const std::string& model, MeshAssetRegistry::Model& view)
{
    bool loadFailed = false;
    MeshAssetRegistry::BuildModel(model, *m_meshLib, [this, &loadFailed](const std::string& lib)
    {
        const Resource::Handle h = PreviewLibraryHandle(lib);
        if (!h.IsValid()) loadFailed = true;
        return h;
    }, view);
    return !loadFailed;
}

bool Renderer::ComposeMeshItemAsset(const std::string& ref, Preview::AssetComposition& comp)
{
    comp.placements.clear();
    comp.sourceFiles.clear();
    std::string model;
    uint64_t    key = 0;
    if (!Resource::ParseMeshAssetRef(ref, model, key))
    {
        // Memoised empty: the thumbnail lane marks it "no preview", the panel
        // says it resolves to nothing.
        LOG_WARNING("[PREVIEW] mesh asset '%s' is not \"<model .iscn>#<16 hex digits>\"", ref.c_str());
        return true;
    }
    comp.sourceFiles.push_back(model);

    MeshAssetRegistry::Model view;
    if (!BuildPreviewModelView(model, view)) return false;   // transient: retried rather than memoised

    const MeshAssetRegistry::Mesh* mesh = MeshAssetRegistry::Find(view, key, std::string());
    if (!mesh)
    {
        LOG_WARNING("[PREVIEW] mesh asset '%s': the model has no mesh with that key%s%s",
                    ref.c_str(), view.error.empty() ? "" : " - ", view.error.c_str());
        return true;
    }
    const std::string lib = Sys::RelativizeAssetPath(mesh->libraryPath);
    comp.sourceFiles.push_back(lib);

    // -preview_meshasset_whole_library: the REVERSE ARM of asset_preview_mesh_single.
    // Every entry of the Mesh's library at the origin - the stacked picture a
    // `.meshlib` tile draws - so the pack proves it can tell one Mesh from that.
    static const bool s_wholeLibrary = Sys::CommandLine::Get().Has("-preview_meshasset_whole_library");
    if (s_wholeLibrary)
    {
        std::unordered_map<uint32_t, std::string> matOf;
        for (const MeshAssetRegistry::Mesh& m : view.meshes)
            for (const MeshAssetRegistry::Section& s : m.sections)
                if (m.libraryPath == mesh->libraryPath) matOf.emplace(s.entry, s.defaultMaterial);
        const uint32_t count = m_meshLib->GetMeshCount(mesh->library);
        for (uint32_t e = 0; e < count; ++e)
        {
            Preview::AssetPlacement p;
            p.meshPath = lib;
            p.meshId   = e;
            if (auto it = matOf.find(e); it != matOf.end()) p.matPath = it->second;
            comp.placements.push_back(std::move(p));
        }
        LOG_WARNING("[PREVIEW] mesh asset '%s': -preview_meshasset_whole_library composes all %u "
                    "entries of '%s' (the reverse arm)", ref.c_str(), count, lib.c_str());
        return true;
    }

    comp.placements.reserve(mesh->sections.size());
    for (const MeshAssetRegistry::Section& s : mesh->sections)
    {
        // At the identity: a Mesh's sections share the mesh's own space.
        Preview::AssetPlacement p;
        p.meshPath = lib;
        p.meshId   = s.entry;
        p.matPath  = s.defaultMaterial;
        comp.placements.push_back(std::move(p));
    }
    return true;
}

// Hands one cached library back. What it must get right:
// - Release, always: every path that forgets an entry comes through here. An erase without the
//   Release leaks a refcount (a whole VB + IB), and since MeshLibrary::Load is path-deduplicated
//   the next load of the path would get the leaked, stale library back.
// - Hand back the descriptors only when this Release destroys the library. Load is refcounted
//   and the preview shares its key space with the live World (a level on Tokyo.meshlib plus a
//   tile previewing it is one library, two references); detaching a shared library zeroes the
//   mesh-descriptor and bindless VB/IB slots the scene draws mid-frame - lost level geometry
//   after a thumbnail refresh, with no validation error. Otherwise this is a bare decrement.
// - Decide that inside the release: MeshLibrary::ReleaseIfLast tests and acts under one lock.
//   An ask-then-release leaves a gap where a Load or Retain (the World spawning a SceneRef onto
//   the same file) makes the answer stale in the dangerous direction.
// - Detach now, free later: the detach is immediate (so the next Load re-reads the file), the
//   free waits in m_previewMeshLibHandbacks until the frames that may have recorded the slots
//   retire.
// - Release, then detach. MeshManager::DetachMeshLib needs only the handle's index, never the
//   library; it does need the VB/IB ID3D12Resource* alive, because
//   MeshDescriptorHeap::DetachBufferSlot erases the entry keyed on that pointer.
//   GraphicsDX12::DestroyBuffer only moves the ComPtr into the deferred-release bucket, so the
//   resources are still alive after ReleaseIfLast; if a concurrent hard sync drains the bucket
//   anyway, DetachBufferSlot's both-ways test declines to erase a registration that no longer
//   names our slot.
bool Renderer::ReleasePreviewMeshLib(const std::string& normalisedPath)
{
    // Game half: this mutates MeshManager's caches and the descriptor heap's registration maps,
    // neither thread-safe and both read by the render half while it records. Legal callers are the
    // game half and the serial-pinned editor host (EditorLayer::OnUIRender inside
    // FramePipeline::RenderHalf, under BuildUI's ThreadCtx::Exempt), never a render thread; the
    // check self-reports.
    if (!ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
            "[SPLITCHECK][RENDERCTX] ReleasePreviewMeshLib ran in the Render "
            "context; the mesh-library hand-back is the GAME half's job (plan "
            "sec.4 M6)."))
        return false;

    auto it = m_previewMeshLibs.find(normalisedPath);
    if (it == m_previewMeshLibs.end()) return false;
    // Copy the key before the erase: `normalisedPath` is often a reference into the node being
    // erased (InvalidatePreviewAsset's clear-all loop passes `m_previewMeshLibs.begin()->first`),
    // and the shared-library log line below reads it.
    const std::string      key = it->first;
    const Resource::Handle h   = it->second.handle;
    m_previewMeshLibs.erase(it);
    if (!m_meshLib || !h.IsValid()) return true;

    bool           destroyed  = false;
    const uint32_t othersLeft = m_meshLib->ReleaseIfLast(h, m_gfx, destroyed);
    if (destroyed)
    {
        PendingMeshLibHandback pending;
        m_meshMgr.DetachMeshLib(h, pending.slots);
        if (!pending.slots.Empty())
        {
            pending.stampFrame  = m_gfx.GetReleaseStampFrame();
            pending.queuedFrame = m_previewAssetFrame;
            m_previewMeshLibHandbacks.push_back(std::move(pending));
        }
    }
    else if (!m_previewMeshLibSharedLogged)
    {
        // Once per session: it is a property of the project layout (a level and a browser on the same
        // file), not of a moment. The count is ReleaseIfLast's refcount after our decrement, i.e.
        // exactly the other holders.
        m_previewMeshLibSharedLogged = true;
        LOG_INFO("[PREVIEW] mesh library '%s' is shared, release only - %u other "
                 "reference(s) (the live world) still hold it, so its mesh "
                 "descriptors and bindless VB/IB slots stay registered "
                 "(logged once per session)",
                 key.c_str(), othersLeft);
    }
    return true;
}

// The deferred half of ReleasePreviewMeshLib, run at the top of BuildRenderScene (see the
// declaration). The predicate is the device's: "kFramesInFlight frames went by" equals "the GPU
// is done" only while one thread owns both halves of the frame. GetReleaseStampFrame() stamps
// one frame beyond wherever the render half is, and GetRetiredFrame() is what BeginFrame's fence
// wait has proved.
void Renderer::DrainPreviewMeshLibHandbacks()
{
    if (m_previewMeshLibHandbacks.empty()) return;
    // Game half, same reason as ReleasePreviewMeshLib: this hands slots back to
    // MeshDescriptorHeap, which is not thread-safe and which the render half reads.
    if (!ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
            "[SPLITCHECK][RENDERCTX] DrainPreviewMeshLibHandbacks ran in the "
            "Render context; the slot hand-back is the GAME half's job (plan "
            "sec.4 M6)."))
        return;
    const uint64_t retired = m_gfx.GetRetiredFrame();
    std::size_t keep = 0;
    for (std::size_t i = 0; i < m_previewMeshLibHandbacks.size(); ++i)
    {
        PendingMeshLibHandback& p = m_previewMeshLibHandbacks[i];
        if (p.stampFrame > retired)
        {
            if (keep != i) m_previewMeshLibHandbacks[keep] = std::move(p);
            ++keep;
            continue;
        }
        // Said once: the deferral is otherwise asserted by nothing (the packets, the picture and the
        // validation layer are identical whether the slots are freed here or at the detach). The number
        // is the deferral actually achieved, so a stamp/retire pair that drifted into "drain
        // immediately" reads as 0 rather than as silence.
        if (!m_previewMeshLibHandbackLogged)
        {
            m_previewMeshLibHandbackLogged = true;
            LOG_INFO("[PREVIEW] mesh library hand-back deferred %llu frame(s) "
                     "before freeing %zu mesh descriptor slot(s) + %u bindless "
                     "buffer slot(s) (logged once per session)",
                     static_cast<unsigned long long>(
                         m_previewAssetFrame >= p.queuedFrame
                             ? m_previewAssetFrame - p.queuedFrame : 0ull),
                     p.slots.meshDescSlots.size(),
                     (p.slots.vbSlot != RHI::kInvalidBufferIndex ? 1u : 0u) +
                     (p.slots.ibSlot != RHI::kInvalidBufferIndex ? 1u : 0u));
        }
        m_meshMgr.ReleaseMeshLibHandback(p.slots);
    }
    m_previewMeshLibHandbacks.resize(keep);
}

// Enters (or stays in) the failed state for one asset. The entry is the retry: while it exists,
// PreviewAssetRetryTick derives the asset's stamp term from the frame counter, so the extract
// re-resolves this asset once every kPreviewAssetRetryFrames frames. Do not bump a counter here
// instead: a counter only a failing resolve moves, feeding a stamp only a moved counter changes,
// lets the extract settle into the idle path on an empty part list (where `allBound` is
// vacuously true) and never look at the file again.
void Renderer::NotePreviewAssetLoadRetry(const std::string& assetPath)
{
    if (assetPath.empty()) return;
    const std::string key = PreviewAssetKey(assetPath);
    auto [it, inserted] = m_previewAssetRetry.try_emplace(key);
    if (inserted)
    {
        it->second.firstFailFrame = m_previewAssetFrame;
        it->second.lastFailFrame  = m_previewAssetFrame;
        it->second.attempts       = 1;
        return;
    }
    // COUNTED PER FRAME, not per call - see PreviewAssetRetry::attempts.
    if (it->second.lastFailFrame == m_previewAssetFrame) return;
    it->second.lastFailFrame = m_previewAssetFrame;
    ++it->second.attempts;
}

// Leaves the failed state. Called from every resolve outcome that is not a retryable load
// failure, including "composes to zero placements" (a DemoLoot-shaped asset is a settled
// answer). Its log line is the only place the retry cadence is observable: one-shot per
// recovery, with the numbers, since a panel that recovers renders like one that was never broken.
void Renderer::ClearPreviewAssetLoadRetry(const std::string& assetPath)
{
    if (assetPath.empty() || m_previewAssetRetry.empty()) return;
    auto it = m_previewAssetRetry.find(PreviewAssetKey(assetPath));
    if (it == m_previewAssetRetry.end()) return;
    const uint32_t attempts = it->second.attempts;
    const uint64_t frames   = m_previewAssetFrame >= it->second.firstFailFrame
                                ? m_previewAssetFrame - it->second.firstFailFrame
                                : 0ull;
    m_previewAssetRetry.erase(it);
    // attempts counts distinct failing frames, so `frames / cadence + 1` bounds it and a pack can
    // gate the cadence from this one line: attempts near `frames` means the retry runs every frame,
    // attempts of 2 with a large `frames` means the retry stopped advancing.
    LOG_INFO("[PREVIEW] asset retry recovered: attempts=%u frames=%llu "
             "cadence=%llu ('%s')",
             attempts, static_cast<unsigned long long>(frames),
             static_cast<unsigned long long>(kPreviewAssetRetryFrames),
             assetPath.c_str());
}

// Enforces the cap. Called at the end of a compose / resolve, so everything the caller touched
// has the highest lastUse values. Evicting a library also drops every memoised composition that
// names it (re-taking the library is what re-registers the parts' mesh-descriptor slots) and
// bumps the memo epoch, so any live asset view re-resolves next frame.
// A view extracted earlier in this frame still holds slots of the evicted library and draws
// before the epoch bump is seen. MeshDescriptorHeap::FreeMesh defers only the slot index and
// zeroes the descriptor at once, so those packets would draw nothing; the deferred hand-back in
// ReleasePreviewMeshLib (m_previewMeshLibHandbacks) is what covers that window.
void Renderer::TrimPreviewMeshLibs(const std::string& keep)
{
    if (m_previewMeshLibs.size() <= m_previewMeshLibCap) return;

    // Oldest first. A handful of entries over the cap in practice, so sorting the whole map is
    // cheaper than an intrusive list, and this runs only on the frame the ceiling is crossed.
    // Nothing the current resolve touched is a candidate (the watermark): an asset needing more than
    // m_previewMeshLibCap distinct libraries would otherwise evict the slots it is registering and
    // reload them next frame, for ever. It stays over the cap and says so instead.
    std::vector<std::pair<uint64_t, std::string>> byAge;
    byAge.reserve(m_previewMeshLibs.size());
    for (const auto& kv : m_previewMeshLibs)
        if (kv.first != keep && kv.second.lastUse <= m_previewMeshLibKeepMark)
            byAge.emplace_back(kv.second.lastUse, kv.first);
    std::sort(byAge.begin(), byAge.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    uint32_t evicted = 0;
    for (const auto& [age, path] : byAge)
    {
        if (m_previewMeshLibs.size() <= m_previewMeshLibCap) break;
        (void)age;
        // Every composition that named it goes too, so the next ask re-composes
        // against a freshly loaded library rather than against slots that were
        // just handed back.
        for (auto cit = m_previewAssetPlacements.begin();
             cit != m_previewAssetPlacements.end(); )
        {
            const bool names =
                cit->first == path ||
                std::find(cit->second.sourceFiles.begin(),
                          cit->second.sourceFiles.end(), path)
                    != cit->second.sourceFiles.end();
            if (names) cit = m_previewAssetPlacements.erase(cit);
            else       ++cit;
        }
        if (ReleasePreviewMeshLib(path)) ++evicted;
    }
    if (evicted)
    {
        ++m_previewAssetMemoEpoch;
        m_previewMeshLibEvicted += evicted;
        // The `name=value` fields at the end are the assertable form; packs match the prose ahead of
        // them, so it stays. The per-call count is 1 for almost any rotation (the watermark keeps the
        // current resolve's entries out), so `sessionEvicted=`, the running total, is what separates
        // "crossed the ceiling once" from "kept crossing it".
        LOG_INFO("[PREVIEW] mesh library cache over the cap of %zu - evicted %u "
                 "least-recently-used librar(ies), %zu remain "
                 "(evicted=%u remain=%zu sessionEvicted=%llu)",
                 m_previewMeshLibCap, evicted, m_previewMeshLibs.size(),
                 evicted, m_previewMeshLibs.size(),
                 static_cast<unsigned long long>(m_previewMeshLibEvicted));
    }
    else if (m_previewMeshLibs.size() > m_previewMeshLibCap &&
             !m_previewMeshLibCapLogged)
    {
        // Every remaining entry belongs to the asset being resolved right now,
        // so the cap loses to correctness. Said once, because it is a property
        // of the ASSET rather than of a moment.
        m_previewMeshLibCapLogged = true;
        LOG_INFO("[PREVIEW] mesh library cache is %zu over the cap of %zu and "
                 "nothing is evictable - the asset being resolved names more "
                 "distinct .meshlib files than the cap allows, so the cap is "
                 "yielding to it (logged once per session)",
                 m_previewMeshLibs.size() - m_previewMeshLibCap,
                 m_previewMeshLibCap);
    }
}

// Drops both caches: the composition memo and the resolved half (m_previewMeshLibs, one
// Resource::Handle per `.meshlib`), or a re-imported `.meshlib` would recompose from the new file
// and draw the old library's entries until a world reload. A prefab invalidation drops every
// library its parts came from as well as its own path, since any of them may have changed and
// re-taking one is cheap (MeshLibrary::Load is path-cached). Every branch releases through
// ReleasePreviewMeshLib.
void Renderer::InvalidatePreviewAsset(const std::string& assetPath)
{
    // The extract's dirty stamp folds this, so an invalidation re-resolves the parts on the next
    // frame - which is what lets the extract skip the resolve on every other frame.
    ++m_previewAssetMemoEpoch;
    if (assetPath.empty())
    {
        m_previewAssetPlacements.clear();
        // Not m_previewMeshLibs.clear(): the map owns one MeshLibrary refcount per entry, and dropping
        // it without the Release would pin a VB+IB pair per library for the life of the process.
        while (!m_previewMeshLibs.empty())
            ReleasePreviewMeshLib(m_previewMeshLibs.begin()->first);
        m_previewMeshLibFailLogged.clear();
        m_previewUnfittedAssets.clear();
        m_previewAssetFits.clear();
        m_previewAssetRetry.clear();
        return;
    }
    // All three per-asset maps are keyed on PreviewAssetKey, the same form m_previewMeshLibs uses:
    // an invalidation in the watcher's spelling must erase what a panel wrote in the browser's.
    const std::string assetKey = PreviewAssetKey(assetPath);
    m_previewUnfittedAssets.erase(assetKey);
    m_previewAssetFits.erase(assetKey);
    // The retry state too: an explicit invalidation means "ask again NOW", and
    // a failed state armed earlier would otherwise hold the answer back for up
    // to kPreviewAssetRetryFrames frames.
    m_previewAssetRetry.erase(assetKey);

    auto it = m_previewAssetPlacements.find(assetKey);
    if (it != m_previewAssetPlacements.end())
    {
        // Copy the paths out first: ReleasePreviewMeshLib does not touch
        // m_previewAssetPlacements, but the composition is erased below and the
        // placement references would dangle if the order were reversed.
        std::vector<std::string> sources;
        sources.reserve(it->second.placements.size());
        for (const Preview::AssetPlacement& p : it->second.placements)
            if (!p.meshPath.empty()) sources.push_back(PreviewLibKey(p.meshPath));
        m_previewAssetPlacements.erase(it);
        for (const std::string& s : sources)
        {
            ReleasePreviewMeshLib(s);
            m_previewMeshLibFailLogged.erase(s);
        }
    }
    // The asset itself may BE a `.meshlib`, in which case it is its own source.
    const std::string self = PreviewLibKey(assetPath);
    ReleasePreviewMeshLib(self);
    m_previewMeshLibFailLogged.erase(self);
}

// The memo: one parse per path per session (or per world reload), or a panel left open would
// re-read a prefab and its .iscn every frame. A failed parse is cached too, so a bad path is not
// re-read and re-logged every frame; InvalidatePreviewAsset is the escape hatch (the editor calls
// it on a re-import and a world reload). A library that would not open is not memoised (below).
const Preview::AssetComposition*
Renderer::ComposePreviewAsset(const std::string& assetPath, Preview::AssetKind kind,
                              bool* outLibraryOpened)
{
    if (outLibraryOpened) *outLibraryOpened = false;

    // The LRU watermark for the trim at the end: nothing stamped after this
    // point is evictable, because it is what this compose is using.
    m_previewMeshLibKeepMark = m_previewMeshLibClock;

    // One spelling for the memo key: the caller's string arrives as a browser tile's
    // working-directory-relative path, a panel's typed one or the watcher's project-relative one,
    // all naming one file.
    const std::string assetKey = PreviewAssetKey(assetPath);
    auto cacheIt = m_previewAssetPlacements.find(assetKey);
    if (cacheIt != m_previewAssetPlacements.end()) return &cacheIt->second;

    Preview::AssetComposition comp;
    const std::string ext = LowerExt(assetPath);
    if (kind == Preview::AssetKind::None) kind = Preview::AssetKindForPath(assetPath);
    if (kind == Preview::AssetKind::Prefab || ext == ".ipfb")
    {
        ComposePrefabAsset(assetPath, comp);
        // A prefab's compose reads no library, but the resolve that follows takes one per SceneRef:
        // trim against the asset path so nothing evicts a library this composition is about to name.
        TrimPreviewMeshLibs(PreviewLibKey(assetPath));
    }
    else if (kind == Preview::AssetKind::Model)
    {
        // Like a prefab's SceneRef: no library is read here; the resolve takes one per part library.
        ComposeModelAsset(assetPath, comp);
        TrimPreviewMeshLibs(PreviewLibKey(assetPath));
    }
    else if (kind == Preview::AssetKind::MeshAsset)
    {
        // The Mesh's identity comes from its loaded library (the file's IDNT, or the provider's
        // synthesis), so this compose opens the library, and one that will not open is retried, not
        // memoised as an empty picture.
        if (!ComposeMeshItemAsset(assetPath, comp))
        {
            NotePreviewAssetLoadRetry(assetPath);
            return nullptr;
        }
        if (outLibraryOpened) *outLibraryOpened = true;
        TrimPreviewMeshLibs(PreviewLibKey(assetPath));
    }
    else
    {
        // The entry count comes from the loaded library, so the load happens here rather than in the
        // composer, through the same cached path every part resolve uses (one Load per session).
        // Batched: this runs inside the asset browser's per-frame thumbnail enqueue loop, and
        // BatchScope (the wrapper SceneInstanceLoader::Load takes) nests - only the outermost scope owns
        // the device.
        const std::string libKey = PreviewLibKey(assetPath);
        uint32_t meshCount = 0;
        bool     libOk     = false;
        {
            SceneInstanceLoader::BatchScope _batch(&m_gfx);
            uint32_t slot = RHI::kInvalidBufferIndex, indexCount = 0;
            float lo[3] = { 0.f, 0.f, 0.f }, hi[3] = { 0.f, 0.f, 0.f };
            (void)ResolvePreviewMeshEntry(assetPath, 0, slot, indexCount, lo, hi);
            if (auto it = m_previewMeshLibs.find(libKey); it != m_previewMeshLibs.end())
            {
                libOk     = it->second.handle.IsValid();
                meshCount = m_meshLib->GetMeshCount(it->second.handle);
            }
        }
        if (!libOk)
        {
            // Not memoised: a library that would not open is the routinely transient failure on this path
            // (an importer rewriting the file, an unfinished copy), and memoising the empty composition would
            // latch "no preview" until a world reload. The retry is this asset's only: bumping the memo epoch
            // (in every target's extract stamp) would de-idle every other asset target every frame.
            // NotePreviewAssetLoadRetry arms a stamp term derived from the frame counter, so it advances
            // every kPreviewAssetRetryFrames frames by itself.
            NotePreviewAssetLoadRetry(assetPath);
            return nullptr;
        }
        // An opened-but-empty library cannot reach here: MeshLibrary::Load refuses meshCount == 0 and
        // returns an invalid handle, so an importer that publishes an empty container first takes the
        // retry branch above, like a half-written file (asset_preview_empty covers it end to end). Do
        // not add a meshCount == 0 branch here without changing MeshLibrary::Load, where the decision
        // lives.
        // Reported as the exact inverse of the retry branch, so a caller that does not resolve
        // (GetPreviewAssetDeps) can disarm the retry this function armed, and nothing else.
        if (outLibraryOpened) *outLibraryOpened = true;
        ComposeMeshAsset(libKey, meshCount, comp);
        TrimPreviewMeshLibs(libKey);
    }
    cacheIt = m_previewAssetPlacements.emplace(assetKey, std::move(comp)).first;
    LOG_INFO("[PREVIEW] asset '%s' composed: %zu placement(s), %zu source file(s)",
             assetPath.c_str(), cacheIt->second.placements.size(),
             cacheIt->second.sourceFiles.size());
    return &cacheIt->second;
}

bool Renderer::GetPreviewAssetDeps(const std::string& assetPath,
                                   Preview::AssetKind kind,
                                   std::vector<std::string>& outFiles,
                                   std::vector<std::string>& outMaterials)
{
    outFiles.clear();
    outMaterials.clear();
    if (assetPath.empty() || kind == Preview::AssetKind::None) return false;
    if (!m_meshLib) return false;
    if (!ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
            "[SPLITCHECK][RENDERCTX] GetPreviewAssetDeps ran in the Render "
            "context; the asset composition is the GAME half's job."))
        return false;

    // This route arms the retry (ComposePreviewAsset's "library would not open" branch, which the
    // thumbnail sidecar reaches as readily as a panel), so it must disarm it, or a tile that failed
    // once and then composed fine leaves a failed entry that keeps the asset re-resolving every
    // kPreviewAssetRetryFrames frames and suppresses the "asset retry recovered" line.
    // And it must disarm no more than that. Do not clear whenever the compose returns non-null: a
    // prefab always composes (its compose reads no library), so a prefab whose part library is
    // missing would lose the retry ResolvePreviewAsset armed and the extract would go idle on an
    // empty picture. libraryOpened is true exactly on the call that opened the asset's own
    // `.meshlib` and found it good.
    bool libraryOpened = false;
    const Preview::AssetComposition* comp =
        ComposePreviewAsset(assetPath, kind, &libraryOpened);
    if (!comp) return false;
    if (libraryOpened) ClearPreviewAssetLoadRetry(assetPath);
    outFiles = comp->sourceFiles;
    for (const Preview::AssetPlacement& p : comp->placements)
        if (!p.matPath.empty() &&
            std::find(outMaterials.begin(), outMaterials.end(), p.matPath)
                == outMaterials.end())
            outMaterials.push_back(p.matPath);

    // ---- The two layers matPath cannot name ----
    // The caller (EditorLayer::ServiceAssetThumbnails) stamps every file in `outFiles` and, for each
    // material in `outMaterials`, the textures CollectDeps finds inside that .imat - the base
    // material only. A texture a prefab's MaterialOverride or MaterialInline block introduces is in
    // the picture, so it joins the dependency set here, or re-importing it would serve the old tile
    // until the browser's residency sweep.
    // They go into outFiles, not outMaterials: the caller runs LoadMaterial on every outMaterials
    // entry, and a texture path there would fail to load on every service pass. `outFiles` is
    // exactly "a file whose content decides this picture".
    auto pushFile = [&outFiles](const std::string& f)
    {
        if (f.empty()) return;
        if (std::find(outFiles.begin(), outFiles.end(), f) == outFiles.end())
            outFiles.push_back(f);
    };
    std::size_t overrideTex = 0, inlineTex = 0;
    for (const Preview::AssetPlacement& p : comp->placements)
    {
        if (p.hasMatOverride)
            for (const auto& [key, val] : p.matOverride.props)
                if (val.type == MaterialOverride::Value::TexRef &&
                    !val.texPath.empty() && val.texPath != "<none>")
                { pushFile(val.texPath); ++overrideTex; }

        // The inline text is parsed, not scanned: a regex over the key=value payload would be a second,
        // diverging reader of the material grammar (the reason ParseMaterialOverrideKV lives outside the
        // serializer). ApplyMaterialTextInPlace onto a default component is the one reader, and the slot
        // table then names the paths.
        if (!p.matInlineText.empty())
        {
            MaterialComponent inlineMat{};
            Resource::ApplyMaterialTextInPlace(inlineMat, p.matInlineText);
            for (uint32_t s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
            {
                const std::string& t = inlineMat.textures[s].name;
                if (t.empty()) continue;
                const std::size_t before = outFiles.size();
                pushFile(t);
                if (outFiles.size() != before) ++inlineTex;
            }
        }
    }
    if (overrideTex || inlineTex)
        LOG_INFO("[PREVIEW] asset deps: files=%zu materials=%zu overrideTex=%zu "
                 "inlineTex=%zu path=%s",
                 outFiles.size(), outMaterials.size(), overrideTex, inlineTex,
                 assetPath.c_str());
    return !comp->placements.empty();
}

// The entry a placement draws: its section key's entry in the preview's library, else the
// meshId hint.
uint32_t Renderer::PlacementEntry(const Preview::AssetPlacement& pl)
{
    if (pl.sectionKey == 0 || pl.meshPath.empty() || !m_meshLib) return pl.meshId;
    const Resource::Handle h = PreviewLibraryHandle(pl.meshPath);
    const int e = h.IsValid() ? m_meshLib->FindEntryBySectionKey(h, pl.sectionKey) : -1;
    return e >= 0 ? static_cast<uint32_t>(e) : pl.meshId;
}

bool Renderer::ResolvePreviewAsset(const std::string& assetPath,
                                   Preview::AssetKind kind,
                                   std::vector<Preview::AssetPart>& outParts,
                                   uint32_t& outDropped,
                                   Resource::PrefabSkipCounts* outSkipped,
                                   const std::vector<std::string>* matOverrides)
{
    outParts.clear();
    outDropped = 0;
    // Zeroed on every exit, including the refusals below: a caller that kept the previous asset's
    // tally after switching to one that does not parse would name skips the panel cannot point at.
    if (outSkipped) *outSkipped = Resource::PrefabSkipCounts{};
    if (assetPath.empty() || kind == Preview::AssetKind::None) return false;
    if (!m_meshLib) return false;

    // Game half: everything below allocates descriptors (RegisterMeshLibMesh), parses assets or
    // resolves custom-shader ids, and the last reallocates a vector a compiling worker may hold an
    // entry pointer into. The check self-reports.
    if (!ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
            "[SPLITCHECK][RENDERCTX] ResolvePreviewAsset ran in the Render "
            "context. The asset composition is the GAME half's job (plan "
            "sec.4 M6)."))
        return false;

    // Reset for this resolve: ResolvePreviewMeshEntry sets it when a library (the asset's own or a
    // part's) would not open, and the exits below turn that into this asset's retry instead of
    // latching an empty picture.
    m_previewMeshLibLoadFailed = false;
    // And the LRU watermark, so the trim at the end cannot evict a library this resolve uses.
    // ComposePreviewAsset re-stamps it to the same value (nothing is touched in between), never a
    // later one.
    m_previewMeshLibKeepMark = m_previewMeshLibClock;

    const Preview::AssetComposition* comp = ComposePreviewAsset(assetPath, kind);
    if (!comp || comp->placements.empty())
    {
        // Both arms: "not failing" must be said as explicitly as "failing". Zero placements without a
        // load failure is the DemoLoot case, a settled answer, and a stale failed state would keep the
        // asset re-resolving every kPreviewAssetRetryFrames frames for ever.
        if (m_previewMeshLibLoadFailed) NotePreviewAssetLoadRetry(assetPath);
        else                           ClearPreviewAssetLoadRetry(assetPath);
        return false;
    }
    const std::vector<Preview::AssetPlacement>& placements = comp->placements;
    // Straight off the memoised composition, the object the "skipped:" log line was emitted from,
    // so the panel and the log cannot disagree.
    if (outSkipped) *outSkipped = comp->skipped;

    // One upload batch for every library this asset's parts come from, as SceneInstanceLoader::Load
    // wraps its own spawn. The compose above takes its own (it may run without reaching here); the
    // two nest harmlessly.
    SceneInstanceLoader::BatchScope _batch(&m_gfx);

    // ---- Resolve each placement ----
    // AABBs are accumulated in the COMPOSED space so the fit below frames what
    // the user will actually see, not the union of un-placed library boxes.
    XMVECTOR bbMin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0.f);
    XMVECTOR bbMax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.f);
    bool     anyBounds = false;

    std::unordered_map<std::string, MaterialComponent> matCache;

    for (const Preview::AssetPlacement& pl : placements)
    {
        if (outParts.size() >= Preview::kMaxPreviewParts)
        {
            ++outDropped;
            continue;
        }

        uint32_t slot = RHI::kInvalidBufferIndex, indexCount = 0;
        float    aabbMin[3] = { 0.f, 0.f, 0.f }, aabbMax[3] = { 0.f, 0.f, 0.f };

        // ---- The primitive flavour ----
        // No load, refcount, LRU or retry clock: MeshManager uploads the five procedural primitives once
        // at Init with persistent=true, so the slot is valid from Renderer::Compile to process exit -
        // which is why this is a second flavour rather than a synthetic library
        // (ResolvePreviewMeshEntry would take a reference nobody can release).
        // The AABB is the unit box, authored rather than measured: every ProceduralMesh.h generator fits
        // [-0.5, 0.5] (BoundingVolume's default extents agree, MeshSpawner relies on it) and a primitive
        // has no stored bounds. It must be non-degenerate, or the merged-bounds fit derives no scale and
        // the panel says "not fitted" for a fittable subject.
        bool resolved = false;
        if (pl.primitiveId >= 0)
        {
            // RANGE-CHECKED HERE rather than trusted from the file: gpuMeshID is
            // a raw integer in the .ipfb text and GetPrimitive indexes a
            // std::array. An out-of-range id is a malformed prefab, which is a
            // dropped part with a line - the same answer a missing library gets.
            if (pl.primitiveId < static_cast<int>(PrimitiveMeshType::Count))
            {
                const MeshManager::GPUMesh& prim =
                    m_meshMgr.GetPrimitive(pl.primitiveId);
                if (prim.meshDescSlot != RHI::kInvalidBufferIndex &&
                    prim.indexCount != 0)
                {
                    slot       = prim.meshDescSlot;
                    indexCount = prim.indexCount;
                    aabbMin[0] = aabbMin[1] = aabbMin[2] = -0.5f;
                    aabbMax[0] = aabbMax[1] = aabbMax[2] =  0.5f;
                    resolved = true;

                    // Name what it resolved to: `gpuMeshID=1` is an opaque integer, and
                    // `parts=1 dropped=0` would also pass if every id mapped onto one
                    // primitive. asset_preview_primitive pins `type=Sphere`, so the enum
                    // order (Cube, Sphere, Cone, Plane, Torus - ProceduralMesh.h) is under
                    // test; the index count proves the slot is a real upload, not an empty
                    // persistent entry. Once per distinct type per session (the bitmask).
                    const uint32_t bit = 1u << pl.primitiveId;
                    if (!(m_previewPrimitiveTypeLogged & bit))
                    {
                        m_previewPrimitiveTypeLogged |= bit;
                        static constexpr const char* kPrimNames[] = {
                            "Cube", "Sphere", "Cone", "Plane", "Torus" };
                        static_assert(
                            static_cast<int>(PrimitiveMeshType::Count) ==
                                static_cast<int>(std::size(kPrimNames)),
                            "PrimitiveMeshType gained a member and this name "
                            "table did not - the log line would name the wrong "
                            "primitive, which is exactly what it exists to "
                            "prevent.");
                        LOG_INFO("[PREVIEW] asset primitive: type=%s id=%d "
                                 "slot=%u indices=%u",
                                 kPrimNames[pl.primitiveId], pl.primitiveId,
                                 slot, indexCount);
                    }
                }
            }
            if (!resolved)
            {
                ++outDropped;
                if (!m_previewPartResolveFailLogged)
                {
                    m_previewPartResolveFailLogged = true;
                    LOG_WARNING("[PREVIEW] asset part primitive gpuMeshID=%d did not "
                                "resolve to a mesh-descriptor slot (out of range, or "
                                "MeshManager::InitPrimitives has not run); it is "
                                "counted in `dropped` and NOT drawn (logged once per "
                                "session)", pl.primitiveId);
                }
                continue;
            }
        }
        else if (!ResolvePreviewMeshEntry(pl.meshPath, PlacementEntry(pl), slot, indexCount,
                                          aabbMin, aabbMax))
        {
            // COUNTED, NOT SILENT. `dropped` is what the panel and the packs
            // read as "the picture is deliberately partial", and a part lost
            // here (the library is still streaming, the file is gone, or the
            // meshId is past the end of it) is exactly as partial as one lost
            // to the part cap - so the invariant parts + dropped == placements
            // has to hold on both routes or the number means nothing.
            ++outDropped;
            if (!m_previewPartResolveFailLogged)
            {
                m_previewPartResolveFailLogged = true;
                LOG_WARNING("[PREVIEW] asset part mesh '%s'#%u did not resolve to a "
                            "mesh-descriptor slot (library missing, still streaming, "
                            "or meshId past the end); it is counted in `dropped` and "
                            "NOT drawn (logged once per session)",
                            pl.meshPath.c_str(), pl.meshId);
            }
            continue;
        }

        Preview::AssetPart part;
        part.meshDescSlot   = slot;
        part.indexCount     = indexCount;
        part.placementIndex = static_cast<uint32_t>(&pl - placements.data());
        std::memcpy(part.world, pl.world, sizeof(float) * 16);

        // ---- The material ----
        // LoadMaterial resolves the whole .imatinst chain and is cached by the serializer, so a repeated
        // path is a map lookup. A path that fails to load leaves the default MaterialComponent, a defined
        // picture: dropping the part would hide geometry because of a material problem.
        // The prefab's MaterialInline / MaterialOverride layers are replayed too, in LoadPrefab's order
        // (ReplayPlacementMaterial), and the cache is keyed by PlacementMaterialKey so two parts sharing
        // one .imat under different bags stay separate.
        // A caller's override (matOverrides, the Mesh Editor's staged slots) replaces the placement's
        // material path for this resolve only; the memoised placement is not touched.
        const std::size_t placementIdx = static_cast<std::size_t>(&pl - placements.data());
        Preview::AssetPlacement plOverride;
        const Preview::AssetPlacement* plMat = &pl;
        if (matOverrides && placementIdx < matOverrides->size() &&
            !(*matOverrides)[placementIdx].empty())
        {
            plOverride         = pl;
            plOverride.matPath = (*matOverrides)[placementIdx];
            plMat              = &plOverride;
        }
        if (!plMat->matPath.empty() || !plMat->matInlineText.empty() || plMat->hasMatOverride)
        {
            const std::string key = PlacementMaterialKey(*plMat);
            auto mit = matCache.find(key);
            if (mit == matCache.end())
            {
                MaterialComponent mc{};
                ReplayPlacementMaterial(*plMat, mc);
                mit = matCache.emplace(key, std::move(mc)).first;
            }
            part.mat = mit->second;
        }

        // ---- Custom-shader ids, before anything hashes the material ----
        // ResolvePreviewCustomPSID/VSID run MaterialReflectionSync::Sync, which mutates the component (it
        // seeds the //@default parameter bag); hashing first would fold the pre-sync material and mint a
        // second render for no visible reason.
        part.customPSID = ResolvePreviewCustomPSID(part.mat);
        part.customVSID = ResolvePreviewCustomVSID(part.mat);

        // Bounds, in composed space.
        {
            const XMMATRIX w =
                XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(part.world));
            // Eight corners, because a rotated placement's transformed
            // min/max pair is not the transformed AABB.
            XMVECTOR pMin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0.f);
            XMVECTOR pMax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.f);
            for (int c = 0; c < 8; ++c)
            {
                const XMVECTOR p = XMVectorSet(
                    (c & 1) ? aabbMax[0] : aabbMin[0],
                    (c & 2) ? aabbMax[1] : aabbMin[1],
                    (c & 4) ? aabbMax[2] : aabbMin[2], 1.f);
                const XMVECTOR t = XMVector3Transform(p, w);
                pMin = XMVectorMin(pMin, t);
                pMax = XMVectorMax(pMax, t);
            }
            bbMin = XMVectorMin(bbMin, pMin);
            bbMax = XMVectorMax(bbMax, pMax);
            // The part's own centre, for the record half's painter ordering.
            // Fitted below with the same matrix everything else is.
            XMFLOAT3 pc;
            XMStoreFloat3(&pc, XMVectorScale(XMVectorAdd(pMin, pMax), 0.5f));
            part.centre[0] = pc.x;
            part.centre[1] = pc.y;
            part.centre[2] = pc.z;
            anyBounds = true;
        }

        outParts.push_back(std::move(part));
    }

    // The trim runs after every part has stamped its library's lastUse, so nothing this asset needs
    // can be evicted.
    TrimPreviewMeshLibs(PreviewLibKey(assetPath));
    // And the retry: a library that would not open moves only this asset's retry tick. The clear is
    // what ends a retry; without it PreviewAssetRetryTick keeps deriving a new term and a healthy
    // asset pays a full resolve every kPreviewAssetRetryFrames frames for ever.
    if (m_previewMeshLibLoadFailed) NotePreviewAssetLoadRetry(assetPath);
    else                            ClearPreviewAssetLoadRetry(assetPath);

    if (outParts.empty()) return false;

    // ---- Framing: fit the merged bounds into the studio camera's unit box ----
    // The rig is authored for a unit-diameter subject at the origin (PreviewStudioRig.h), so this is
    // ResolvePreviewMesh's single-mesh transform: centre on the AABB, then one uniform scale so the
    // longest axis is 1. Uniform, or a sword is squashed into a cube; centred rather than
    // pivot-preserving, or a prefab authored at world (18, 0.9, -1) like Rossi_Base is off screen.
    // Baked into the part transforms rather than the camera: the same picture, one transpose in the
    // record half's instance write, and the studio camera stays a constant, so the A/B fixture path
    // and the shadow VP keep the frustum they were measured against.
    if (anyBounds)
    {
        XMFLOAT3 lo, hi;
        XMStoreFloat3(&lo, bbMin);
        XMStoreFloat3(&hi, bbMax);
        const float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
        const float maxExtent = (std::max)(ex, (std::max)(ey, ez));

        // ---- Both degenerate cases are handled, and both say so ----
        // A non-finite translation is the one that poisons the picture: one part whose library entry has
        // an infinite or NaN AABB (missing bounds are stored as zeros, corrupt ones are not) makes the
        // merged centre non-finite and every part's matrix NaN - an empty panel with a full packet count.
        const bool finiteBox =
            std::isfinite(lo.x) && std::isfinite(lo.y) && std::isfinite(lo.z) &&
            std::isfinite(hi.x) && std::isfinite(hi.y) && std::isfinite(hi.z);
        if (!finiteBox)
        {
            // Marked unfitted here too: this branch skips the fit as completely as the zero-extent one, and
            // the panel's "drawn at its authored size, not fitted" notice is driven by this set.
            m_previewUnfittedAssets.insert(PreviewAssetKey(assetPath));
            m_previewAssetFits.erase(PreviewAssetKey(assetPath));
            if (!m_previewFitNonFiniteLogged)
            {
                m_previewFitNonFiniteLogged = true;
                LOG_WARNING("[PREVIEW] asset '%s': the merged bounds are not finite "
                            "(min %.3f,%.3f,%.3f max %.3f,%.3f,%.3f) - the auto-fit "
                            "is SKIPPED and the parts are drawn at their authored "
                            "size; at least one library entry has a bad AABB "
                            "(logged once per session)",
                            assetPath.c_str(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
            }
        }
        else if (!(maxExtent > 1e-5f))
        {
            // Every part's AABB is zero (a library written without computed
            // bounds). Centring is still correct and safe; the SCALE is not
            // derivable, so the subject is drawn at its authored size and the
            // panel is told, rather than the user being left to wonder why one
            // asset is the wrong size in an otherwise uniform grid.
            m_previewUnfittedAssets.insert(PreviewAssetKey(assetPath));
            if (!m_previewFitZeroExtentLogged)
            {
                m_previewFitZeroExtentLogged = true;
                LOG_WARNING("[PREVIEW] asset '%s': the merged bounds have zero extent "
                            "(no library entry carries a computed AABB) - drawn at "
                            "its AUTHORED size, not fitted (logged once per session)",
                            assetPath.c_str());
            }
        }

        if (finiteBox)
        {
            const float scale = (maxExtent > 1e-5f) ? (1.0f / maxExtent) : 1.0f;
            const XMMATRIX fit = XMMatrixMultiply(
                XMMatrixTranslation(-0.5f * (lo.x + hi.x),
                                    -0.5f * (lo.y + hi.y),
                                    -0.5f * (lo.z + hi.z)),
                XMMatrixScaling(scale, scale, scale));
            // Mesh Editor: the same numbers, for overlays drawn over the
            // picture on the CPU (GetPreviewAssetFit).
            PreviewAssetFit& rec = m_previewAssetFits[PreviewAssetKey(assetPath)];
            rec.centre[0] = 0.5f * (lo.x + hi.x);
            rec.centre[1] = 0.5f * (lo.y + hi.y);
            rec.centre[2] = 0.5f * (lo.z + hi.z);
            rec.scale     = scale;
            for (Preview::AssetPart& p : outParts)
            {
                const XMMATRIX w =
                    XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(p.world));
                XMFLOAT4X4 m;
                XMStoreFloat4x4(&m, XMMatrixMultiply(w, fit));
                std::memcpy(p.world, &m, sizeof(float) * 16);
                // The centre rides through the SAME matrix - a centre left in
                // pre-fit space would sort against a camera that is looking at
                // the fitted one.
                XMFLOAT3 c;
                XMStoreFloat3(&c, XMVector3Transform(
                    XMVectorSet(p.centre[0], p.centre[1], p.centre[2], 1.f), fit));
                p.centre[0] = c.x;
                p.centre[1] = c.y;
                p.centre[2] = c.z;
            }
        }
    }

    return true;
}

// The mesh half of ResolvePreviewMesh, without the fit (see the declaration): the library-load
// and slot-registration rules live only here.
bool Renderer::ResolvePreviewMeshEntry(const std::string& meshlibPath, uint32_t meshId,
                                       uint32_t& outSlot, uint32_t& outIndexCount,
                                       float outAabbMin[3], float outAabbMax[3])
{
    if (meshlibPath.empty() || !m_meshLib) return false;

    // The single normalisation point: the cache key and the string MeshLibrary::Load dedups on use
    // this one spelling, so a browser tile and a prefab part naming one file are one entry, one
    // upload and one thing for InvalidatePreviewAsset to find.
    const std::string key = PreviewLibKey(meshlibPath);

    Resource::Handle lib{};
    if (auto it = m_previewMeshLibs.find(key); it != m_previewMeshLibs.end())
    {
        lib = it->second.handle;
        it->second.lastUse = ++m_previewMeshLibClock;   // LRU stamp
    }
    else
    {
        lib = m_meshLib->Load(key, m_gfx);
        if (lib.IsValid())
        {
            PreviewMeshLibEntry e;
            e.handle  = lib;
            e.lastUse = ++m_previewMeshLibClock;
            m_previewMeshLibs.emplace(key, e);
            m_previewMeshLibFailLogged.erase(key);
            LOG_INFO("[PREVIEW] mesh library '%s': loaded", key.c_str());
        }
        else
        {
            // Not cached: the same invalid handle is what a `.meshlib` being rewritten
            // by an importer produces, and caching it would latch "no preview" until a
            // world reload. NotePreviewAssetLoadRetry bounds the retry (this asset only,
            // once every kPreviewAssetRetryFrames frames). This line and
            // MeshLibrary::Load's own ERROR latch once per path, so a retried open
            // cannot become an error stream that fails a pack's -maxerrors budget.
            if (m_previewMeshLibFailLogged.insert(key).second)
                LOG_INFO("[PREVIEW] mesh library '%s': FAILED (not cached - a "
                         "later resolve retries; logged once per path)", key.c_str());
            m_previewMeshLibLoadFailed = true;
        }
    }
    if (!lib.IsValid()) return false;

    const auto* entry = m_meshLib->GetEntry(lib, meshId);
    if (!entry || entry->indexCount == 0) return false;

    MeshLibRef ref{};
    ref.libHandle = lib;
    ref.meshId    = meshId;
    // Tagged as the preview's: the owner tag changes nothing registered; it lets the shutdown
    // "[MESH] meshlib desc cache" line separate the slots the preview minted from the shared ones.
    const uint32_t slot = m_meshMgr.RegisterMeshLibMesh(
        *m_meshLib, ref, MeshManager::MeshLibOwner::Preview);
    if (slot == RHI::kInvalidBufferIndex) return false;

    outSlot       = slot;
    outIndexCount = entry->indexCount;
    outAabbMin[0] = entry->aabbMin.x;
    outAabbMin[1] = entry->aabbMin.y;
    outAabbMin[2] = entry->aabbMin.z;
    outAabbMax[0] = entry->aabbMax.x;
    outAabbMax[1] = entry->aabbMax.y;
    outAabbMax[2] = entry->aabbMax.z;
    return true;
}

#ifdef WITH_HARNESS
// ==== -preview_asset_verify: the asset composition checked against a real spawn ====
// The asset preview rests on SceneInstanceLoader::ReadMeshPlacements producing, for every part,
// the same (DFS index -> mesh) mapping and world matrix PrefabSerializer::LoadPrefab produces by
// spawning. The picture cannot show a violation (a character either way, identical packet
// counts, a material on the neighbouring part looks like an art decision), so this spawns the
// asset through SceneInstanceLoader::Load, walks Resource::CollectPrefabSubtreeDFS (the function
// LoadPrefab keys its overrides through), compares, and destroys the subtree again.
//   parts            placements the composition produced.
//   mismatches       placements whose dfsIndex does not land on an entity carrying their
//                    (mesh file, meshId): an off-by-one from a skipped mesh ref, a "_volN" /
//                    "_colN" entity or a second root shows up here and nowhere else.
//   matrixMismatches placements whose composed world matrix disagrees with the spawned entity's
//                    GlobalTransform past the tolerance (e.g. the .iscn root's transform
//                    applied twice).
// Game half, editor host, WITH_HARNESS only: it creates and destroys entities.
bool Renderer::VerifyPreviewAssetOrder(const std::string& assetPath,
                                       World& world,
                                       Resource::AssetManager& assetMgr)
{
    if (!m_meshLib)
    {
        LOG_ERROR("[PREVIEW] asset order verify: no MeshLibrary on the renderer");
        return false;
    }

    const std::string ext = LowerExt(assetPath);

    // The entry a placement names: its F-line key's entry when the library has it (what the spawn
    // resolved), else the mesh= hint. Read from the library file, so nothing has to hold it loaded.
    auto entryOf = [](const Preview::AssetPlacement& p) -> uint32_t
    {
        if (p.sectionKey != 0)
            if (const auto t = Resource::MeshLibKeyTableAt(p.meshPath))
                if (const int e = t->Find(p.sectionKey); e >= 0) return static_cast<uint32_t>(e);
        return p.meshId;
    };

    // ---- What the PREVIEW says ----
    // One entry per SceneRef node: the .iscn it spawns, that node's world, and
    // the placements the composition attributed to it. A bare `.iscn` target is
    // the degenerate case - one group, node -1, identity world.
    struct Group
    {
        std::string scenePath;
        int         nodeIdx   = -1;
        XMMATRIX    nodeWorld = XMMatrixIdentity();
        std::vector<const Preview::AssetPlacement*> placements;
    };
    std::vector<Group>                   groups;
    std::vector<Preview::AssetPlacement> ownPlacements;   // the .iscn route

    if (ext == ".ipfb")
    {
        const Preview::AssetComposition* comp =
            ComposePreviewAsset(assetPath, Preview::AssetKind::Prefab);
        if (!comp)
        {
            LOG_ERROR("[PREVIEW] asset order verify: '%s' did not compose",
                      assetPath.c_str());
            return false;
        }
        std::vector<Resource::PrefabNodeView> nodes;
        if (!Resource::ReadPrefabNodes(assetPath, nodes))
        {
            LOG_ERROR("[PREVIEW] asset order verify: '%s' has no readable node tree",
                      assetPath.c_str());
            return false;
        }
        // Grouped by the OWNING NODE, not by the path: two nodes may reference
        // the same .iscn at different placements, and each spawns its own
        // subtree with its own DFS.
        for (const Preview::AssetPlacement& p : comp->placements)
        {
            Group* g = nullptr;
            for (Group& cand : groups)
                if (cand.nodeIdx == p.sceneNodeIdx) { g = &cand; break; }
            if (!g)
            {
                const Resource::PrefabNodeView* nv = nullptr;
                for (const Resource::PrefabNodeView& n : nodes)
                    if (n.idx == p.sceneNodeIdx) { nv = &n; break; }
                if (!nv || nv->sceneRefPath.empty()) continue;
                Group ng;
                ng.scenePath = nv->sceneRefPath;
                ng.nodeIdx   = nv->idx;
                ng.nodeWorld = XMLoadFloat4x4(
                    reinterpret_cast<const XMFLOAT4X4*>(nv->world));
                groups.push_back(std::move(ng));
                g = &groups.back();
            }
            g->placements.push_back(&p);
        }
    }
    else if (ext == ".iscn")
    {
        std::vector<SceneInstanceLoader::MeshPlacement> mps;
        if (!SceneInstanceLoader::ReadMeshPlacements(assetPath, mps))
        {
            LOG_ERROR("[PREVIEW] asset order verify: '%s' did not read",
                      assetPath.c_str());
            return false;
        }
        ownPlacements.reserve(mps.size());
        for (const auto& mp : mps)
        {
            Preview::AssetPlacement p;
            p.meshPath   = mp.meshPath;
            p.meshId     = mp.meshId;
            p.sectionKey = mp.sectionKey;
            p.matPath    = mp.matPath;
            p.dfsIndex   = mp.dfsIndex;
            std::memcpy(p.world, mp.world, sizeof(float) * 16);
            ownPlacements.push_back(std::move(p));
        }
        Group g;
        g.scenePath = assetPath;
        for (const Preview::AssetPlacement& p : ownPlacements)
            g.placements.push_back(&p);
        groups.push_back(std::move(g));
    }
    else
    {
        LOG_ERROR("[PREVIEW] asset order verify: '%s' is neither a .ipfb nor a "
                  ".iscn; nothing to verify", assetPath.c_str());
        return false;
    }

    // ---- What the SPAWN says ----
    uint32_t parts = 0, mismatches = 0, matrixMismatches = 0;
    // 1e-3 per component. The two sides compute the same product in a different
    // ORDER (the reader multiplies local * parentWorld down the tree; the spawn
    // composes GlobalTransform from decomposed TRS), so bit equality is not
    // available and a tolerance is not a loosening - it is what float
    // multiplication in two orders costs. A doubled root transform, the failure
    // this exists for, is metres out.
    constexpr float kTol = 1e-3f;

    for (Group& g : groups)
    {
        SceneInstanceLoader::BatchScope _batch(assetMgr.GetGraphicsDevice());
        SceneInstanceLoader::LoadResult res =
            SceneInstanceLoader::Load(g.scenePath, world, assetMgr, *m_meshLib, this);
        if (!res.success || res.rootEntity == NullEntity)
        {
            LOG_ERROR("[PREVIEW] asset order verify: '%s' failed to spawn",
                      g.scenePath.c_str());
            return false;
        }

        // EXACTLY WHAT LoadPrefab DOES to the spawned root, and the reason
        // ReadMeshPlacements drops the root's own transform: the .iscn root's
        // LocalTransform is REPLACED by the owning prefab N-line's, never
        // composed with it. For a bare `.iscn` target the owning node is the
        // identity, so the root's own placement is replaced by identity too and
        // the comparison stays apples to apples.
        {
            XMVECTOR s, q, t;
            XMMatrixDecompose(&s, &q, &t, g.nodeWorld);
            LocalTransform lt;
            XMStoreFloat3(&lt.translation, t);
            XMStoreFloat4(&lt.rotation, q);
            XMStoreFloat3(&lt.scale, s);
            if (auto* existing = world.GetComponent<LocalTransform>(res.rootEntity))
                *existing = lt;
        }
        TransformSystem::Propagate(world);

        std::vector<Entity> dfs;
        Resource::CollectPrefabSubtreeDFS(world, res.rootEntity, dfs);

        for (const Preview::AssetPlacement* p : g.placements)
        {
            ++parts;
            if (p->dfsIndex >= dfs.size())
            {
                ++mismatches;
                LOG_WARNING("[PREVIEW] asset order verify: dfsIndex %u is past the "
                            "%zu entities '%s' spawned (mesh %s#%u)",
                            p->dfsIndex, dfs.size(), g.scenePath.c_str(),
                            p->meshPath.c_str(), p->meshId);
                continue;
            }
            const Entity e = dfs[p->dfsIndex];
            const MeshLibRef*     ref = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath* src = world.GetComponent<MeshSourcePath>(e);
            const std::string want = MeshKeyOf(p->meshPath, entryOf(*p));
            const std::string got  = (ref && src)
                ? MeshKeyOf(src->path, ref->meshId)
                : std::string("<not a mesh entity>");
            if (want != got)
            {
                ++mismatches;
                LOG_WARNING("[PREVIEW] asset order verify: dfsIndex %u wants '%s' "
                            "but the spawned entity is '%s' ('%s')",
                            p->dfsIndex, want.c_str(), got.c_str(),
                            g.scenePath.c_str());
                continue;   // a matrix compare against the wrong entity says nothing
            }

            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) { ++matrixMismatches; continue; }
            const XMFLOAT4X4 live = gt->matrix;
            const float* a = &live.m[0][0];
            const float* b = p->world;
            bool same = true;
            for (int i = 0; i < 16 && same; ++i)
                same = std::fabs(a[i] - b[i]) <= kTol;
            if (!same)
            {
                ++matrixMismatches;
                LOG_WARNING("[PREVIEW] asset order verify: dfsIndex %u world matrix "
                            "differs - live translation (%.3f, %.3f, %.3f) vs "
                            "composed (%.3f, %.3f, %.3f)",
                            p->dfsIndex, a[12], a[13], a[14], b[12], b[13], b[14]);
            }
        }

        Hierarchy::DestroySubtree(world, res.rootEntity);
    }

    // ---- The prefab's material layers, against LoadPrefab ----
    // A second spawn, through LoadPrefab rather than SceneInstanceLoader: the loop above spawns each
    // SceneRef's `.iscn` alone (it verifies the DFS mapping), so it sees only the .iscn's own `mat=`
    // and none of the prefab's MaterialRef / MaterialInline / MaterialOverride blocks. LoadPrefab is
    // the replay the preview has to agree with.
    // It runs only when the prefab states a layer: a prefab with MaterialRef alone is covered by the
    // composition's `matPath`, so the asset_preview_order targets pay nothing for it.
    // Keyed by the mesh, not the DFS index: the order check above owns dfsIndex, and reusing it would
    // report one failure twice. (meshPath, meshId) is a bijection on any useful fixture; an ambiguous
    // key is reported and skipped, not guessed.
    uint32_t matChecked = 0, matMismatches = 0, matAmbiguous = 0;
    uint32_t matLayered = 0;
    std::vector<uint64_t> matDistinct;
    if (ext == ".ipfb")
    {
        const Preview::AssetComposition* comp =
            ComposePreviewAsset(assetPath, Preview::AssetKind::Prefab);
        bool anyLayer = false;
        if (comp)
            for (const Preview::AssetPlacement& p : comp->placements)
                if (p.hasMatOverride || !p.matInlineText.empty()) { anyLayer = true; break; }

        if (anyLayer)
        {
            SceneInstanceLoader::BatchScope _batch(assetMgr.GetGraphicsDevice());
            const Entity prefabRoot =
                Resource::LoadPrefab(assetPath, world, assetMgr, this, nullptr, nullptr);
            if (prefabRoot == NullEntity)
            {
                LOG_ERROR("[PREVIEW] asset material verify: '%s' did not spawn "
                          "through LoadPrefab", assetPath.c_str());
                return false;
            }
            std::vector<Entity> live;
            Resource::CollectPrefabSubtreeDFS(world, prefabRoot, live);

            std::unordered_map<std::string, const MaterialComponent*> liveByMesh;
            std::unordered_map<std::string, int>                      liveCount;
            for (Entity e : live)
            {
                const MeshLibRef*     ref = world.GetComponent<MeshLibRef>(e);
                const MeshSourcePath* src = world.GetComponent<MeshSourcePath>(e);
                const MaterialComponent* mc = world.GetComponent<MaterialComponent>(e);
                if (!ref || !src || !mc) continue;
                const std::string k = MeshKeyOf(src->path, ref->meshId);
                ++liveCount[k];
                liveByMesh[k] = mc;
            }

            for (const Preview::AssetPlacement& p : comp->placements)
            {
                const std::string k = MeshKeyOf(p.meshPath, entryOf(p));
                auto cnt = liveCount.find(k);
                if (cnt == liveCount.end())
                {
                    ++matMismatches;
                    LOG_WARNING("[PREVIEW] asset material verify: the live prefab has "
                                "no entity for '%s'", k.c_str());
                    continue;
                }
                if (cnt->second != 1)
                {
                    ++matAmbiguous;
                    LOG_WARNING("[PREVIEW] asset material verify: '%s' names %d live "
                                "entities; the material comparison needs a unique "
                                "mesh key, so this part is SKIPPED (not passed)",
                                k.c_str(), cnt->second);
                    continue;
                }

                // THE SAME REPLAY THE RESOLVE USES, by construction: this is the
                // function ResolvePreviewAsset calls, not a second copy of it.
                MaterialComponent previewMat{};
                ReplayPlacementMaterial(p, previewMat);
                const MaterialComponent& liveMat = *liveByMesh[k];

                const ResolvedMaterialRuntime kNoRuntime{};
                const uint64_t wantParams = HashMaterialParams(previewMat, kNoRuntime);
                const uint64_t gotParams  = HashMaterialParams(liveMat,    kNoRuntime);
                const uint64_t wantRt     = BuildPreviewMaterialRuntimeKey(previewMat);
                const uint64_t gotRt      = BuildPreviewMaterialRuntimeKey(liveMat);
                ++matChecked;
                if (p.hasMatOverride || !p.matInlineText.empty()) ++matLayered;
                // The half that does not depend on the live side: mismatches == 0 is vacuously true when both
                // sides ignore the override, and the two replays are separate code. `distinct` counts the
                // different material-param hashes the preview resolved: a fixture whose parts share one .imat
                // and differ only in the bag must produce more than one, and produces one the moment the bag is
                // dropped.
                if (std::find(matDistinct.begin(), matDistinct.end(), wantParams)
                        == matDistinct.end())
                    matDistinct.push_back(wantParams);
                if (wantParams != gotParams || wantRt != gotRt)
                {
                    ++matMismatches;
                    LOG_WARNING("[PREVIEW] asset material verify: '%s' params %llu vs "
                                "%llu, runtime %llu vs %llu (preview vs live)",
                                k.c_str(),
                                static_cast<unsigned long long>(wantParams),
                                static_cast<unsigned long long>(gotParams),
                                static_cast<unsigned long long>(wantRt),
                                static_cast<unsigned long long>(gotRt));
                }
            }

            Hierarchy::DestroySubtree(world, prefabRoot);
            LOG_INFO("[PREVIEW] asset material verify: checked=%u layered=%u "
                     "distinct=%u mismatches=%u ambiguous=%u path=%s",
                     matChecked, matLayered,
                     static_cast<uint32_t>(matDistinct.size()),
                     matMismatches, matAmbiguous, assetPath.c_str());
            // Same rule as the order half: a stage that compared nothing must not report success.
            if (matChecked == 0)
            {
                LOG_ERROR("[PREVIEW] asset material verify compared NOTHING for "
                          "'%s' even though it states a material layer",
                          assetPath.c_str());
                return false;
            }

            // ---- The dependency set, reported ----
            // GetPreviewAssetDeps is otherwise reached only from the asset browser's thumbnail service, i.e.
            // only while the browser shows the asset's folder, which no panel-only pack does. Calling it here
            // costs one memo lookup and makes the set assertable in the run that proves the layers were
            // replayed: a texture the override or inline block introduces must be in it. A prefab with no
            // layer needs no report: its base materials are covered by `matPath`.
            std::vector<std::string> depFiles, depMats;
            GetPreviewAssetDeps(assetPath, Preview::AssetKind::Prefab,
                                depFiles, depMats);
        }
    }

    LOG_INFO("[PREVIEW] asset order verify: parts=%u mismatches=%u "
             "matrixMismatches=%u groups=%zu path=%s",
             parts, mismatches, matrixMismatches, groups.size(),
             assetPath.c_str());
    // Zero comparisons is a failure, not a pass: `mismatches == 0 && matrixMismatches == 0` is
    // vacuously true for a target that composed to nothing (a stale SceneRef path, only primitive
    // placements, an .iscn whose root has no mesh refs), and such an asset is invisible in the
    // picture. This catches it for every target, not only those whose pack pins parts=N.
    if (parts == 0)
    {
        LOG_ERROR("[PREVIEW] asset order verify compared NOTHING for '%s' "
                  "(%zu group(s), 0 placement(s)) - there is no claim to make "
                  "about an asset that composes to no parts",
                  assetPath.c_str(), groups.size());
        return false;
    }
    return mismatches == 0 && matrixMismatches == 0 &&
           matMismatches == 0 && matAmbiguous == 0;
}
#endif   // WITH_HARNESS
