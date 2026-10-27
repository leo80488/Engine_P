#include "Resource/PrefabSerializer.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"   // guid-augmented references: survive a rename
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialOverrideApply.h"   // shared ApplyMaterialOverride
#include "Resource/ComponentSerializers.h"
#include "Resource/MeshLibIdentity.h"   // msec= (MeshLibKeyTableAt)
#include "Resource/PrefabTemplate.h"    // a runtime spawn's bytes + parsed records
#include "ECS/Components.h"
#include "ECS/MaterialSchema.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/FollowComponents.h"
#include "ECS/GuidRegistry.h"          // EnsureGuidOn - UIParent GUID pre-stamp
#include "UI/UICanvas.h"               // UIParent / UIRect - the Canvas hierarchy
#include "Scene/SceneInstanceLoader.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"   // ENGINE_CHECK_ONCE - the declined-tag drift guard
#include "System/Log.h"
#include "System/PathUtil.h"
#include "Diagnostics/Profiler.h"   // Prefab.* profiler stages

#include <DirectXMath.h>
#include <algorithm>
#include <cstring>   // std::memcpy - ReadPrefabNodes composes matrices into float[16]
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace
{
// ==== Percent-encode / decode (space <-> %20, percent <-> %25) ====
std::string PercentEncode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
    {
        if      (c == '%')  r += "%25";
        else if (c == ' ')  r += "%20";
        else                r += static_cast<char>(c);
    }
    return r;
}

std::string PercentDecode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            try { r += static_cast<char>(std::stoi(hex, nullptr, 16)); }
            catch (...) { r += s[i]; }
            i += 3;
        }
        else r += s[i++];
    }
    return r;
}

// ==== Subtree collection (DFS pre-order, parents before children) ====
struct NodeInfo
{
    int    idx        { -1 };
    int    parentIdx  { -1 };
    Entity entity     { NullEntity };
};

// Canvas UI hierarchy, parent -> children. UI nodes are not in the scene graph (no Parent /
// Children, no transform); UI::UIParent points child -> parent, so every walk needs this
// reverse index, built once per save. Without it, saving a Canvas root writes a one-node .ipfb
// and still reports success.
// Sibling order matches UICanvasSystem's (siblingIndex, then entity id), so the saved order is
// the on-screen order.
using UiChildMap = std::unordered_map<Entity, std::vector<Entity>>;

UiChildMap BuildUiChildMap(World& world)
{
    UiChildMap map;
    auto* parentPool = world.GetPool<UI::UIParent>();
    if (!parentPool) return map;

    auto& ents = parentPool->Entities();
    auto& data = parentPool->Data();
    for (size_t i = 0; i < data.size(); ++i)
    {
        const Entity parent = data[i].parent;
        if (parent != NullEntity && world.IsAlive(parent))
            map[parent].push_back(ents[i]);
    }

    auto* rectPool = world.GetPool<UI::UIRect>();
    for (auto& [parent, kids] : map)
    {
        std::sort(kids.begin(), kids.end(),
            [rectPool](Entity a, Entity b)
            {
                const auto* ra = rectPool ? rectPool->Get(a) : nullptr;
                const auto* rb = rectPool ? rectPool->Get(b) : nullptr;
                const int sa = ra ? ra->siblingIndex : 0;
                const int sb = rb ? rb->siblingIndex : 0;
                if (sa != sb) return sa < sb;
                return a < b;
            });
    }
    return map;
}

void CollectNodes(World& world, Entity e, int parentIdx,
                  std::unordered_map<Entity, int>& entityToIdx,
                  std::vector<NodeInfo>& out,
                  const UiChildMap* uiChildren = nullptr)
{
    // Iterative pre-order DFS: a recursive walk overflows the 1 MB stack on a deep Children tree
    // (a 70-deep skinned bone chain). entityToIdx doubles as the visited set, so shared subtrees
    // and cycles are skipped. Children are pushed in reverse so they pop in ascending order: the
    // idx assignment is a plain pre-order.
    std::vector<std::pair<Entity, int>> stack;
    stack.emplace_back(e, parentIdx);
    while (!stack.empty())
    {
        const Entity cur       = stack.back().first;
        const int    curParent = stack.back().second;
        stack.pop_back();
        if (!world.IsAlive(cur)) continue;
        if (entityToIdx.count(cur)) continue;  // already collected (shared subtree / cycle)
        // A transient descendant is runtime output, not authored content - a
        // StaticMesh owner's derived sections (rebuilt from its StaticMesh
        // line on load), a Sequencer spawnable. SaveScene already omits them;
        // written here they would load back as orphan parts without geometry
        // (the MeshRef line never recreates a MeshLibRef). The root is kept:
        // saving a transient entity itself is the caller's decision.
        if (cur != e && world.HasComponent<SceneTransientTag>(cur)) continue;
        const int myIdx = static_cast<int>(out.size());
        entityToIdx[cur] = myIdx;
        out.push_back({ myIdx, curParent, cur });
        const Children* ch = world.GetComponent<Children>(cur);
        if (ch)
            for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it)
                stack.emplace_back(*it, myIdx);
        // Canvas children live in a separate hierarchy (see BuildUiChildMap).
        // Pushed reversed for the same reason as above: the stack pops them back
        // into ascending order, keeping DFS pre-order and therefore idx order.
        if (uiChildren)
        {
            auto uit = uiChildren->find(cur);
            if (uit != uiChildren->end())
                for (auto it = uit->second.rbegin(); it != uit->second.rend(); ++it)
                    stack.emplace_back(*it, myIdx);
        }
    }
}

// Iterative pre-order DFS over the Children tree (and the Canvas hierarchy when @p uiChildren
// is set), appending each entity to @p out. Recursion would overflow on a deep skeleton; the
// visited set makes a cyclic Children graph safe.
void CollectSubtreeDFS(World& world, Entity root, std::vector<Entity>& out,
                       const UiChildMap* uiChildren = nullptr)
{
    std::unordered_set<Entity> visited;
    std::vector<Entity> stack;
    stack.push_back(root);
    while (!stack.empty())
    {
        const Entity ent = stack.back();
        stack.pop_back();
        if (!visited.insert(ent).second) continue;  // cycle / shared-node guard
        out.push_back(ent);
        const Children* ch = world.GetComponent<Children>(ent);
        if (ch)
            for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it)
                stack.push_back(*it);
        if (uiChildren)
        {
            auto uit = uiChildren->find(ent);
            if (uit != uiChildren->end())
                for (auto it = uit->second.rbegin(); it != uit->second.rend(); ++it)
                    stack.push_back(*it);
        }
    }
}

// Extends the prefab set with every entity outside it that references into it (FollowSocket,
// FollowEntity, SkeletonRef), iterating to a fixed point because a new member can bring its own
// dependents. New roots get parent=-1 (siblings of the original root).
void CollectLinkedFollowers(World& world,
                            std::unordered_map<Entity, int>& entityToIdx,
                            std::vector<NodeInfo>& nodes)
{
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (Entity e : world.GetEntities())
        {
            if (!world.IsAlive(e)) continue;
            if (entityToIdx.count(e)) continue;

            bool linksIn = false;
            if (auto* fs = world.GetComponent<FollowSocketComponent>(e);
                fs && entityToIdx.count(fs->target.Resolve(world))) linksIn = true;
            if (!linksIn)
                if (auto* fe = world.GetComponent<FollowEntityComponent>(e);
                    fe && entityToIdx.count(fe->target.Resolve(world))) linksIn = true;
            if (!linksIn)
                if (auto* sr = world.GetComponent<SkeletonRef>(e);
                    sr && entityToIdx.count(sr->entity)) linksIn = true;

            if (linksIn)
            {
                CollectNodes(world, e, /*parentIdx=*/-1, entityToIdx, nodes);
                changed = true;
            }
        }
    }
}

// The `m=` matrix older files carry in a Follow block: 16 underscore-separated floats. A
// missing or unparsable value keeps the identity's.
DirectX::XMFLOAT4X4 ParseMatrix(const std::string& s)
{
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
    float* p = reinterpret_cast<float*>(&m);
    size_t start = 0;
    for (int i = 0; i < 16; ++i)
    {
        size_t end = s.find('_', start);
        if (end == std::string::npos) end = s.size();
        try { p[i] = std::stof(s.substr(start, end - start)); } catch (...) {}
        if (end == s.size()) break;
        start = end + 1;
    }
    return m;
}

// A file without socketHash addresses the socket by raw index: derive the hash now, while the
// target's socket list is at hand, so a later socket removal cannot retarget the attachment to
// its neighbour. A socket with an empty name hashes to 0 and stays index-addressed.
void UpgradeLegacySocketHash(World& world, Entity target, FollowSocketComponent& fs)
{
    if (fs.socketNameHash != 0u) return;
    const SocketComponent* sc = world.GetComponent<SocketComponent>(target);
    // A raw index names one of the target's OWN sockets (FollowSystem's rule).
    if (!sc || fs.socketIndex >= sc->AuthoredCount()) return;
    fs.socketNameHash  = SocketComponent::HashName(sc->sockets[fs.socketIndex].name);
    fs.cachedSocketGen = sc->generation;
}

// Older files keep the attach offset in the Follow block as `m=<16 floats>` while the entity's
// LocalTransform holds a baked world pose. The offset is already in socket / target space, so it
// becomes the entity's LocalTransform and the world pose is dropped.
void MigrateLegacyFollowOffset(World& world, Entity e, const std::string& mStr)
{
    using namespace DirectX;
    const XMFLOAT4X4 m = ParseMatrix(mStr);

    XMVECTOR vScl, vRot, vTrn;
    if (!XMMatrixDecompose(&vScl, &vRot, &vTrn, XMLoadFloat4x4(&m)))
        return;   // singular offset - leave whatever LocalTransform loaded

    LocalTransform lt;
    XMStoreFloat3(&lt.translation, vTrn);
    XMStoreFloat4(&lt.rotation,    vRot);
    XMStoreFloat3(&lt.scale,       vScl);   // old `m=` offsets are unit-scale
    world.AddComponent<LocalTransform>(e, lt);
}

// ==== N line -> NodeRecord (transform, name, part key) ====
struct NodeRecord
{
    int   idx    { -1 };
    int   parent { -1 };
    float tx{0}, ty{0}, tz{0};
    float qx{0}, qy{0}, qz{0}, qw{1};
    float sx{1}, sy{1}, sz{1};
    std::string name;
    uint64_t    msec = 0;   // the part's SectionKey (0: none)
};

bool ParseNLine(const std::string& line, NodeRecord& out)
{
    std::istringstream ss(line.substr(2));
    std::string token;
    while (ss >> token)
    {
        const auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = token.substr(0, eq);
        const std::string val = token.substr(eq + 1);
        if (val.empty()) continue;
        try
        {
            if      (key == "idx")    out.idx    = std::stoi(val);
            else if (key == "parent") out.parent = std::stoi(val);
            else if (key == "tx")     out.tx     = std::stof(val);
            else if (key == "ty")     out.ty     = std::stof(val);
            else if (key == "tz")     out.tz     = std::stof(val);
            else if (key == "qx")     out.qx     = std::stof(val);
            else if (key == "qy")     out.qy     = std::stof(val);
            else if (key == "qz")     out.qz     = std::stof(val);
            else if (key == "qw")     out.qw     = std::stof(val);
            else if (key == "sx")     out.sx     = std::stof(val);
            else if (key == "sy")     out.sy     = std::stof(val);
            else if (key == "sz")     out.sz     = std::stof(val);
            else if (key == "name")   out.name   = PercentDecode(val);
            else if (key == "msec")   { uint64_t k = 0; if (Resource::MeshKeyFromHex(val, k)) out.msec = k; }
        }
        catch (...) {}
    }
    return out.idx >= 0;
}

// ==== Component line -> tag + KVMap ====
// Format: "  Tag: key=val key=val ..."
struct ComponentBlock { int nodeIdx; std::string tag; KVMap kv; };

// The records of one parse; `text` is the resolved text they came from, which a reuse compares
// (see SpawnParsedPrefabs).
struct ParsedPrefab
{
    std::string                 text;
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
};

bool ParseComponentLine(const std::string& line, int currentNodeIdx, ComponentBlock& out)
{
    if (line.size() < 4 || line[0] != ' ' || line[1] != ' ') return false;
    auto colonPos = line.find(':', 2);
    if (colonPos == std::string::npos) return false;

    out.nodeIdx = currentNodeIdx;
    out.tag     = line.substr(2, colonPos - 2);
    out.kv.clear();

    size_t start = colonPos + 1;
    if (start < line.size() && line[start] == ' ') ++start;

    std::istringstream ss(line.substr(start));
    std::string token;
    while (ss >> token)
    {
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        out.kv[token.substr(0, eq)] = token.substr(eq + 1);
    }
    return true;
}

// A runtime spawn's parsed records, by path, for one resolution of the
// prefab's text (LoadPrefab inside the spawn template; a preload). Main thread.
std::unordered_map<std::string, ParsedPrefab>& SpawnParsedPrefabs()
{
    static std::unordered_map<std::string, ParsedPrefab> s_parsed;
    return s_parsed;
}

// The text of a validated blob with its references repaired by the guid each
// one carries (the `$g_` tokens stripped before any line handler sees them).
// Any thread: the asset guid index is locked.
std::string ResolvePrefabText(const std::vector<uint8_t>& data, const std::string& path)
{
    std::size_t refsRepaired = 0;
    std::string text = [&] {
        PROF_SCOPE("Prefab.ResolveRefs");   // a stat per guid-carrying reference
        return Resource::AssetRefs::ResolveSceneText(
            std::string(Resource::GetPayloadText(data.data(), data.size())), path, &refsRepaired);
    }();
    if (refsRepaired)
        LOG_WARNING("PrefabSerializer: '%s' — %zu reference(s) repaired by guid. "
                    "Re-save the prefab to make the repair permanent.",
                    path.c_str(), refsRepaired);
    return text;
}

// The N lines and indented component blocks of a resolved text. Pure.
void ParsePrefabRecords(const std::string& prefabText, std::vector<NodeRecord>& nodes,
                        std::vector<ComponentBlock>& compBlocks)
{
    int currentNodeIdx = -1;
    std::istringstream stream(prefabText);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == 'N' && line.size() > 1 && line[1] == ' ')
        {
            NodeRecord nr;
            if (ParseNLine(line, nr))
            {
                currentNodeIdx = nr.idx;
                nodes.push_back(std::move(nr));
            }
        }
        else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            ComponentBlock cb;
            if (ParseComponentLine(line, currentNodeIdx, cb))
                compBlocks.push_back(std::move(cb));
        }
    }
}

// LoadPrefab's steps 1 and 2: read and validate the blob (through the spawn
// template when @p fromTemplate), repair the references by guid, parse the N
// lines and component blocks (the template's records for this resolution of
// the text when @p fromTemplate). false = it did not read (logged).
bool ReadAndParsePrefab(const std::string& path, bool fromTemplate,
                        std::vector<NodeRecord>& nodes, std::vector<ComponentBlock>& compBlocks)
{
    // 1. Read and validate blob. A runtime spawn reads it through its template
    // (Resource/PrefabTemplate.h): the bytes of the last read, while the file's
    // stamp holds.
    std::vector<uint8_t> data;
    if (!(fromTemplate ? ::Resource::PrefabTemplate::ReadFile(path, data)
                       : ::Resource::AssetFS::Get().ReadFile(path, data)))
    {
        LOG_ERROR("PrefabSerializer: cannot open '%s'", path.c_str());
        return false;
    }

    if (!Resource::ValidateHeader(data.data(), data.size(), Resource::MAGIC_PREFAB))
    {
        LOG_ERROR("PrefabSerializer: invalid .ipfb header in '%s'", path.c_str());
        return false;
    }

    // 2. Parse N lines and indented component blocks
    const std::string prefabText = ResolvePrefabText(data, path);

    // A runtime spawn takes the records its template parsed from THIS
    // resolution of the text: the references above are re-resolved every
    // time (a moved target repairs to another text, then a re-parse).
    // Copies: the per-instance Guid remap rewrites them.
    if (fromTemplate)
    {
        const auto& parsedMap = SpawnParsedPrefabs();
        const auto it = parsedMap.find(path);
        if (it != parsedMap.end() && it->second.text == prefabText)
        {
            nodes      = it->second.nodes;
            compBlocks = it->second.compBlocks;
            return true;
        }
    }
    ParsePrefabRecords(prefabText, nodes, compBlocks);
    if (fromTemplate)
        SpawnParsedPrefabs()[path] = ParsedPrefab{ prefabText, nodes, compBlocks };
    return true;
}

} // anonymous namespace

// ==== SavePrefab: components through ComponentSerializerRegistry ====
bool Resource::SavePrefab(Entity root, World& world, const std::string& path,
                          std::vector<Entity>* savedNodes)
{
    if (savedNodes) savedNodes->clear();
    auto& reg = GetComponentRegistry();

    // Stamp a GuidComponent on every UIParent target before anything is written, as SceneSerializer
    // does: the CvParent serializer stamps the parent's GUID lazily while writing the child, but the
    // node loop runs parents first, so an already written parent would never emit its `Guid:` line
    // and the child's parentGuid would dangle on load. LoadPrefab's remap covers parentGuid through
    // its "*Guid" suffix rule.
    if (auto* uiParentPool = world.GetPool<UI::UIParent>())
    {
        auto& upData = uiParentPool->Data();
        for (size_t i = 0; i < upData.size(); ++i)
        {
            const Entity par = upData[i].parent;
            if (par != NullEntity && world.IsAlive(par))
                ECS::EnsureGuidOn(world, par);
        }
    }

    const UiChildMap uiChildren = BuildUiChildMap(world);

    std::unordered_map<Entity, int> entityToIdx;
    std::vector<NodeInfo> nodes;
    CollectNodes(world, root, -1, entityToIdx, nodes, &uiChildren);

    if (nodes.empty())
    {
        LOG_ERROR("PrefabSerializer: entity %u is not alive", root);
        return false;
    }

    // Auto-include entities that reference anything in the saved subtree, such as trails following
    // a model bone via FollowSocket. They become extra top-level nodes (parent=-1), and their
    // cross-refs are written as prefab-local indices below.
    const size_t coreCount = nodes.size();
    CollectLinkedFollowers(world, entityToIdx, nodes);
    if (nodes.size() > coreCount)
        LOG_INFO("PrefabSerializer: auto-included %zu linked entities (followers)",
                 nodes.size() - coreCount);

    std::ostringstream ss;
    ss << "# DX12 Engine Prefab\n";
    char buf[512];

    for (const NodeInfo& ni : nodes)
    {
        const Entity e = ni.entity;

        // --- N line: transform + name ---
        const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        float tx=0,ty=0,tz=0, qx=0,qy=0,qz=0,qw=1, sx=1,sy=1,sz=1;
        if (lt)
        {
            tx = lt->translation.x; ty = lt->translation.y; tz = lt->translation.z;
            qx = lt->rotation.x;    qy = lt->rotation.y;    qz = lt->rotation.z; qw = lt->rotation.w;
            sx = lt->scale.x;       sy = lt->scale.y;       sz = lt->scale.z;
        }

        snprintf(buf, sizeof(buf),
            "N idx=%d parent=%d"
            " tx=%.6f ty=%.6f tz=%.6f"
            " qx=%.6f qy=%.6f qz=%.6f qw=%.6f"
            " sx=%.6f sy=%.6f sz=%.6f",
            ni.idx, ni.parentIdx,
            tx, ty, tz, qx, qy, qz, qw, sx, sy, sz);
        // A model part's SectionKey: LoadPrefab checks the DFS ordinal its blocks apply by against it
        // (MatchPrefabPartsByKey). Written before name=, which some tools read to the end of the line.
        const uint64_t partKey = PrefabPartKeyOf(world, e);
        ss << buf;
        if (partKey) ss << " msec=" << MeshKeyToHex(partKey);
        ss << " name=" << PercentEncode(world.GetName(e)) << "\n";

        // --- Pre-step: auto-create .imat if entity has MaterialComponent but no MaterialSourcePath ---
        if (world.HasComponent<MaterialComponent>(e) && !world.HasComponent<MaterialSourcePath>(e))
        {
            const MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
            namespace fs = std::filesystem;
            const fs::path prefabDir = Sys::U8Path(path).parent_path();
            const fs::path matDir    = prefabDir / "Materials";
            fs::create_directories(matDir);

            std::string entName = world.GetName(e);
            if (entName.empty()) entName = "entity";
            // Same rule as SceneSerializer; Sys::SanitizeFileName says what it rejects and why.
            std::string safeName = Sys::SanitizeFileName(entName);
            if (safeName.empty()) safeName = "entity";

            std::string matFileName = safeName + "_mat" + std::to_string(ni.idx) + ".imat";
            std::string matAbsPath  = Sys::PathToUtf8(matDir / matFileName);

            SaveMaterial(*mat, matAbsPath);
            // Written to the absolute location (matDir comes from the prefab's absolute save path) but
            // referenced project-relative.
            world.AddComponent<MaterialSourcePath>(
                e, MaterialSourcePath{ Sys::RelativizeAssetPath(matAbsPath) });
        }

        // ---- Pre-step 2: an unsaved live material edit becomes an override ----
        // A part spawned from an .iscn F-line with `mat=` has a MaterialSourcePath, so the pre-step
        // above skips it and only "MaterialRef: path=" is written: without this, a tweak on one part is
        // lost on reload. The diff goes into a MaterialOverride row rather than a forked .imat: the load
        // side replays it, and a forked file would be named by the unstable DFS ordinal, one file per
        // part. Only for a dirty component (ASSET_DIRTY = differs from the .imat on disk, not the
        // GPU-upload bit), so an untouched prefab re-saves byte-identical.
        if (const MaterialComponent* liveMat = world.GetComponent<MaterialComponent>(e))
        {
            const MaterialSourcePath* src = world.GetComponent<MaterialSourcePath>(e);
            if (src && !src->path.empty() && liveMat->IsAssetDirty())
            {
                MaterialOverride diff;
                if (BuildMaterialOverride(*liveMat, src->path, diff) && !diff.IsEmpty())
                {
                    // Supersedes any bag the entity carried: the diff is measured
                    // against the FILE, so it already accounts for the old override.
                    if (auto* existing = world.GetComponent<MaterialOverride>(e))
                        *existing = std::move(diff);
                    else
                        world.AddComponent<MaterialOverride>(e, std::move(diff));
                }
            }
        }

        // --- Generic component serialization via registry ---
        for (const auto& [typeIdx, serializer] : reg.All())
        {
            // Never embed a PrefabRef in a .ipfb: saving a prefab from a tagged instance would bake the
            // instance's old source path into the new file, and every instantiation would tag its root with
            // it. The instance tag is scene-file-only state.
            if (serializer.tag == "PrefabRef") continue;
            // Never embed EdFolder either: the outliner folder is this scene's organisation, not part of
            // the prefab. Baked in, every instance in any scene would reappear in (and create) the folder
            // the prefab was authored under.
            if (serializer.tag == "EdFolder") continue;
            // `Guid` is written on purpose: not the instance's identity but the prefab-local name that
            // intra-prefab references (targetGuid=, parentGuid=, script Entity vars) are written against,
            // which tells LoadPrefab "another node of mine" from "something in the scene". LoadPrefab
            // remaps each one per instance, so they cannot collide across instances.
            if (serializer.has(world, e))
                serializer.serialize(world, e, ss);
        }

        // ---- Cross-entity references ----
        // Saved under "Prefab*" tags so LoadPrefab's registry walk ignores them; its fixup pass
        // resolves them once every entity exists. A target inside the prefab is written as its
        // prefab-local idx (bound with BindAndStamp on load); one outside (a weapon prefab whose owner
        // lives in the scene) as target=-1 - the load still creates the component with a null target,
        // so the authored socket survives and equipment code rebinds it at runtime. The attach offset is
        // the entity's own LocalTransform (written by the registry walk), so there is no `m=`;
        // LoadPrefab folds an older file's `m=` back in.
        if (auto* fs = world.GetComponent<FollowSocketComponent>(e))
        {
            auto it = entityToIdx.find(fs->target.Resolve(world));
            const int tgtIdx = (it != entityToIdx.end()) ? it->second : -1;
            ss << "  PrefabFollowSocket: target=" << tgtIdx
               << " socket=" << fs->socketIndex
               << " socketHash=" << fs->socketNameHash << "\n";
        }
        if (auto* fe = world.GetComponent<FollowEntityComponent>(e))
        {
            auto it = entityToIdx.find(fe->target.Resolve(world));
            const int tgtIdx = (it != entityToIdx.end()) ? it->second : -1;
            ss << "  PrefabFollowEntity: target=" << tgtIdx << "\n";
        }
        if (auto* sr = world.GetComponent<SkeletonRef>(e))
        {
            // SkeletonRef is a hard skinning dependency: without a target the follower cannot render, so
            // only an in-set target is written.
            if (auto it = entityToIdx.find(sr->entity); it != entityToIdx.end())
                ss << "  PrefabSkeletonRef: target=" << it->second << "\n";
        }
    }

    // --- Write binary blob ---
    // Stamp each reference with its target's guid so a later rename or move
    // outside the editor is still recoverable (Resource/AssetRefs.h).
    const std::string text       = AssetRefs::AugmentSceneText(ss.str());
    const uint32_t    textLength = static_cast<uint32_t>(text.size());
    const uint32_t    payloadSz  = textLength + 1;

    PrefabMetadata meta{};
    meta.nodeCount  = static_cast<uint32_t>(nodes.size());
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_PREFAB;
    hdr.version      = CurrentVersion(MAGIC_PREFAB);
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Unknown);
    hdr.metadataSize = sizeof(PrefabMetadata);
    hdr.dataSize     = payloadSz;

    auto f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("PrefabSerializer: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLength);
    const char nul = '\0';
    f.write(&nul, 1);

    if (!f.good())
    {
        LOG_ERROR("PrefabSerializer: write error for '%s'", path.c_str());
        return false;
    }

    LOG_INFO("PrefabSerializer: saved '%s' — %u nodes, %u bytes",
             path.c_str(), static_cast<uint32_t>(nodes.size()), textLength);
    if (savedNodes)
    {
        savedNodes->reserve(nodes.size());
        for (const NodeInfo& ni : nodes) savedNodes->push_back(ni.entity);
    }
    return true;
}

// ==== LoadPrefab: components through ComponentSerializerRegistry ====
Entity Resource::LoadPrefab(const std::string& path, World& world, AssetManager& assetMgr,
                            Renderer* renderer, AnimationClipSystem* animClipSys,
                            const ECS::Guid* instanceSeed)
{
    auto& reg = GetComponentRegistry();

    // The spawn's sequential stages on the engine profiler's timeline. The BatchScope's flush and
    // staging release run inside Prefab.Tail: the scope ends with the skinned branch, before this
    // chain does.
    static ::Diag::Prof::NameSlot s_stRead{ "Prefab.ReadParse" }, s_stGuid{ "Prefab.GuidRemap" },
        s_stLoad{ "Prefab.InstanceLoad" }, s_stPost{ "Prefab.PostLoad" },
        s_stComp{ "Prefab.Components" }, s_stFix{ "Prefab.Fixups" },
        s_stOvr{ "Prefab.MatOverride" }, s_stTail{ "Prefab.Tail" }, s_stPlain{ "Prefab.Plain" };
    ::Diag::Prof::StageChain stage;
    stage.Next(s_stRead);

    // 1-2. Read, validate and parse (ReadAndParsePrefab; a runtime spawn
    // through its template).
    const bool fromTemplate = ::Resource::PrefabTemplate::Active();
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    if (!ReadAndParsePrefab(path, fromTemplate, nodes, compBlocks))
        return NullEntity;

    if (nodes.empty())
    {
        LOG_ERROR("PrefabSerializer: no nodes in '%s'", path.c_str());
        return NullEntity;
    }

    // 2b. Per-instance GUID remap.
    // A .ipfb bakes every captured node's Guid (SavePrefab runs the whole registry). Handed out
    // verbatim, every instance would share one set: a second copy overwrites the first in
    // GuidRegistry, and both instances' AttachmentRefs (follow targets, camera targets, script
    // Entity vars) resolve to whichever registered last. So the parsed text is rewritten before any
    // entity exists, and no duplicate is ever live. Only Guids defined in this file (a `Guid:`
    // block) are remapped; one that points outward is left alone. The mapping is a pure function of
    // one seed (see LoadPrefab's declaration).
    stage.Next(s_stGuid);
    {
        int rootIdx = -1;                      // lowest idx with parent == -1
        for (const auto& nr : nodes)
            if (nr.parent < 0 && (rootIdx < 0 || nr.idx < rootIdx)) rootIdx = nr.idx;

        const ECS::Guid seed = (instanceSeed && instanceSeed->IsValid())
                             ? *instanceSeed
                             : ECS::Guid::Generate();

        // The instance the file was saved from (Editor.SavePrefab {link = true} makes the source that
        // instance, in place) keeps the identities it was saved with: its root Guid is the one baked
        // into the file, and a scene row or save game hands it back as the seed. Deriving would re-key
        // every member on the first reload after the link and break outside references to them. Any
        // other seed (another instance, a paste, a spawn) derives, so two instances never share an
        // identity.
        ECS::Guid bakedRoot = ECS::kInvalidGuid;
        for (const auto& cb : compBlocks)
            if (cb.tag == "Guid" && cb.nodeIdx == rootIdx)
                if (auto it = cb.kv.find("hex"); it != cb.kv.end())
                {
                    bakedRoot = ECS::Guid::FromHex(it->second);
                    break;
                }
        const bool savedFromThis = instanceSeed && instanceSeed->IsValid() && bakedRoot.IsValid()
                                && *instanceSeed == bakedRoot;

        std::unordered_map<ECS::Guid, std::string> remap;   // baked -> per-instance hex
        bool rootHasGuid = false;
        for (const auto& cb : compBlocks)
        {
            if (savedFromThis) break;            // identity: nothing to remap
            if (cb.tag != "Guid") continue;
            auto it = cb.kv.find("hex");
            if (it == cb.kv.end()) continue;
            const ECS::Guid baked = ECS::Guid::FromHex(it->second);
            if (!baked.IsValid() || remap.count(baked)) continue;
            if (cb.nodeIdx == rootIdx) rootHasGuid = true;
            // The root takes the seed ITSELF: LoadScene re-applies the row's
            // saved `Guid:` to the instance root afterwards, and if the root
            // ended up on a derived value instead, every intra-prefab reference
            // to the root would point at a Guid nothing owns.
            remap[baked] = (cb.nodeIdx == rootIdx)
                         ? seed.ToHex()
                         : ECS::Guid::Derive(baked, seed).ToHex();
        }

        if (!remap.empty())
        {
            auto substitute = [&remap](std::string& hex) -> bool
            {
                const ECS::Guid g = ECS::Guid::FromHex(hex);   // tolerates dashes / case
                if (!g.IsValid()) return false;
                auto it = remap.find(g);
                if (it == remap.end()) return false;
                hex = it->second;
                return true;
            };

            for (auto& cb : compBlocks)
                for (auto& [key, val] : cb.kv)
                {
                    // The identity block itself.
                    if (cb.tag == "Guid" && key == "hex") { substitute(val); continue; }
                    // Every entity-ref field. ComponentSerializers.cpp fixes the convention in
                    // WriteAttachmentRef: a ref always writes "<name>Guid=<hex32>" - targetGuid,
                    // followTargetGuid, proxyGuid, parentGuid, t<i>Guid, ...
                    if (key.size() > 4 && key.compare(key.size() - 4, 4, "Guid") == 0)
                    { substitute(val); continue; }
                    // Script exposed vars are the one ref that does NOT follow
                    // that convention: "var.<name>=e:<hex32>" (typechar 'e' ==
                    // ScriptVarType::Entity, ScriptExposedVar.h).
                    if (cb.tag == "Script" && key.rfind("var.", 0) == 0 &&
                        val.size() > 2 && val[0] == 'e' && val[1] == ':')
                    {
                        std::string hex = val.substr(2);
                        if (substitute(hex)) val = "e:" + hex;
                    }
                }

            // If members carry identities but the root does not, nothing would persist the seed (SaveScene
            // writes the root's Guid row and LoadScene feeds it back), so every load would re-randomise
            // every member: stamp the seed onto the root. Appended after the substitution loop on purpose:
            // the seed itself must not be substituted (it can be one of the baked Guids).
            if (!rootHasGuid && rootIdx >= 0)
            {
                ComponentBlock rootGuid;
                rootGuid.nodeIdx   = rootIdx;
                rootGuid.tag       = "Guid";
                rootGuid.kv["hex"] = seed.ToHex();
                compBlocks.push_back(std::move(rootGuid));
            }
        }
    }

    // 3. Detect a skinned prefab (SceneRef block). sceneIdx is the N line that owns the SceneRef,
    // so the prefab root can be a wrapper (the character under an empty) and the .iscn root need
    // not be idx 0.
    // Several SceneRefs appear when CollectLinkedFollowers included a follower with its own .iscn
    // (a character + an equipped weapon). The lowest idx drives animation binding and the prefab
    // root; the rest load into the same entityMap so their idx ranges stay consistent.
    struct SceneRefEntry { int idx; std::string path; };
    std::vector<SceneRefEntry> sceneRefs;
    std::string animPath;
    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
                sceneRefs.push_back({ cb.nodeIdx, PercentDecode(it->second) });
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end()) animPath = PercentDecode(it->second);
        }
    }
    std::sort(sceneRefs.begin(), sceneRefs.end(),
              [](const SceneRefEntry& a, const SceneRefEntry& b) { return a.idx < b.idx; });
    const std::string scenePath = sceneRefs.empty() ? std::string{} : sceneRefs.front().path;
    const int         sceneIdx  = sceneRefs.empty() ? -1            : sceneRefs.front().idx;

    // ---- Skinned character flow ----
    if (!scenePath.empty() && renderer)
    {
        // Covers the primary Load plus every secondary SceneRef below, and
        // nests harmlessly inside the world-load batch when a prefab is
        // instantiated during LoadWorld. See SceneInstanceLoader::BatchScope.
        SceneInstanceLoader::BatchScope _skinLoadBatch(assetMgr.GetGraphicsDevice());

        MeshLibrary* meshLib = renderer->GetMeshLibrary();
        if (!meshLib)
        {
            LOG_ERROR("PrefabSerializer: MeshLibrary not wired on renderer — cannot load scene '%s'",
                      scenePath.c_str());
            return NullEntity;
        }
        stage.Next(s_stLoad);
        auto res = SceneInstanceLoader::Load(scenePath, world, assetMgr, *meshLib, renderer);
        stage.Next(s_stPost);
        if (!res.success || res.rootEntity == NullEntity)
        {
            LOG_ERROR("PrefabSerializer: failed to load .iscn '%s'", scenePath.c_str());
            return NullEntity;
        }

        // Apply the transform and name of the N line that owns the SceneRef block. Falls back to node
        // 0: an unwrapped prefab, whose root is the .iscn root.
        const int sceneIdxLocal = (sceneIdx >= 0) ? sceneIdx : 0;
        for (const auto& nr : nodes)
        {
            if (nr.idx != sceneIdxLocal) continue;
            LocalTransform lt;
            lt.translation = { nr.tx, nr.ty, nr.tz };
            lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
            lt.scale       = { nr.sx, nr.sy, nr.sz };
            if (auto* existing = world.GetComponent<LocalTransform>(res.rootEntity))
                *existing = lt;
            if (!nr.name.empty())
                world.SetName(res.rootEntity, nr.name);
            break;
        }

        // Collect loaded entities in DFS order for index-based component application
        std::vector<Entity> loadedEntities;
        CollectSubtreeDFS(world, res.rootEntity, loadedEntities);

        // idx -> entity map. The .iscn DFS fills [sceneIdxLocal, sceneIdxLocal+N-1]; every N line
        // outside that range is an extra entity (a wrapper empty, a trail on a bone via FollowSocket...)
        // created in the loop below.
        std::unordered_map<int, Entity> entityMap;
        for (size_t i = 0; i < loadedEntities.size(); ++i)
            entityMap[sceneIdxLocal + static_cast<int>(i)] = loadedEntities[i];

        // ---- The DFS ordinal checked against the part's section key ----
        // (MatchPrefabPartsByKey, PrefabSerializer.h.) A saved part the key finds at another ordinal
        // takes its blocks there. A keyed part the model no longer has takes none, and neither does a
        // saved non-part node that now sits on a part another node claimed; both leave the map, so they
        // are not re-created as extras.
        std::unordered_set<int> keyRemapped, keyDropped;
        auto remapPartsByKey = [&](int first, const std::vector<Entity>& live, const std::string& modelPath)
        {
            // The SceneRef node's saved subtree, by the saved parent links.
            std::unordered_map<int, std::vector<int>> kids;
            for (const NodeRecord& nr : nodes) kids[nr.parent].push_back(nr.idx);
            std::unordered_set<int> subtree;
            std::vector<int> stack{ first };
            while (!stack.empty())
            {
                const int i = stack.back();
                stack.pop_back();
                if (!subtree.insert(i).second) continue;
                if (auto it = kids.find(i); it != kids.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
            }
            std::vector<std::pair<int, uint64_t>> saved;
            for (const NodeRecord& nr : nodes)
                if (nr.msec && subtree.count(nr.idx)) saved.emplace_back(nr.idx, nr.msec);
            if (saved.empty()) return;   // a prefab saved without section keys: ordinals only

            std::vector<uint64_t> liveKeys(live.size(), 0);
            for (size_t i = 0; i < live.size(); ++i) liveKeys[i] = PrefabPartKeyOf(world, live[i]);
            const PrefabPartMatch m = MatchPrefabPartsByKey(saved, first, liveKeys);

            std::vector<char> claimed(live.size(), 0);
            for (const auto& [idx, ord] : m.nodeToLive) claimed[static_cast<size_t>(ord)] = 1;
            int displaced = 0;
            for (const NodeRecord& nr : nodes)
            {
                const int ord = nr.idx - first;
                if (nr.msec || !subtree.count(nr.idx) || ord < 0 || ord >= static_cast<int>(live.size())) continue;
                if (liveKeys[static_cast<size_t>(ord)] == 0 || !claimed[static_cast<size_t>(ord)]) continue;
                entityMap.erase(nr.idx);
                keyDropped.insert(nr.idx);
                ++displaced;
            }
            for (const auto& [idx, ord] : m.nodeToLive)
            {
                entityMap[idx] = live[static_cast<size_t>(ord)];
                if (idx - first != ord) keyRemapped.insert(idx);
            }
            for (int idx : m.goneNodes)
            {
                entityMap.erase(idx);
                keyDropped.insert(idx);
            }
            if (m.moved > 0 || !m.goneNodes.empty() || displaced > 0)
                LOG_WARNING("PrefabSerializer: '%s' - %d of %zu part(s) of '%s' were found by their section key at "
                            "another place than the prefab saved them (the model was re-imported; their data follows "
                            "them - re-save the prefab to re-anchor it), %zu the model no longer has (their data is "
                            "dropped), %d other node(s) displaced", path.c_str(), m.moved, saved.size(),
                            modelPath.c_str(), m.goneNodes.size(), displaced);
        };
        remapPartsByKey(sceneIdxLocal, loadedEntities, scenePath);

        // ---- Integrity check on the DFS-ordinal key ----
        // Every component block, MaterialRef included, applies by position, so a re-import that adds,
        // removes or reorders a mesh entry silently shifts materials onto neighbours. The saved N line
        // has a name and SceneInstanceLoader names each part after its material (F-line `name=`), so the
        // two compare for free. Only a warning: renaming in the outliner is legitimate, and refusing or
        // auto-repairing would be worse than a drifted material. Silent when all saved names are
        // identical (files written before F-line `name=` store "<model>_mesh" for every part): they
        // carry no information.
        {
            int  compared = 0, mismatched = 0;
            bool savedNamesDistinct = false;
            std::string firstSaved;
            std::string firstMismatch;
            for (const NodeRecord& nr : nodes)
            {
                const int local = nr.idx - sceneIdxLocal;
                if (local < 0 || local >= static_cast<int>(loadedEntities.size())) continue;
                if (nr.name.empty()) continue;
                if (firstSaved.empty())            firstSaved = nr.name;
                else if (nr.name != firstSaved)    savedNamesDistinct = true;

                const auto mapped = entityMap.find(nr.idx);   // after the key remap above
                if (mapped == entityMap.end()) continue;
                ++compared;
                const std::string& live = world.GetName(mapped->second);
                if (!live.empty() && live != nr.name)
                {
                    ++mismatched;
                    if (firstMismatch.empty())
                        firstMismatch = "'" + nr.name + "' -> '" + live + "'";
                }
            }
            if (savedNamesDistinct && mismatched > 0)
                LOG_WARNING("PrefabSerializer: '%s' — %d/%d node name(s) do not match the "
                            "respawned .iscn (first: %s). The prefab's per-part data "
                            "(materials, colliders) is applied BY POSITION, so the model "
                            "was very likely re-imported with a different mesh order; "
                            "re-save the prefab to re-anchor it.",
                            path.c_str(), mismatched, compared, firstMismatch.c_str());
        }

        // Secondary SceneRefs: spawn each remaining .iscn and merge its DFS entities into entityMap.
        // Without this the other model's nodes become broken wrappers: sockets on an entity with no
        // SkeletonComponent, identity socket transforms, and an Inf WorldAabb that crashes SceneBVH.
        for (size_t s = 1; s < sceneRefs.size(); ++s)
        {
            const SceneRefEntry& srf = sceneRefs[s];
            auto resN = SceneInstanceLoader::Load(srf.path, world, assetMgr, *meshLib, renderer);
            if (!resN.success || resN.rootEntity == NullEntity)
            {
                LOG_WARNING("PrefabSerializer: secondary scene '%s' failed to load — idx=%d skipped",
                            srf.path.c_str(), srf.idx);
                continue;
            }

            // N-line override (transform + name), the same convention as the primary.
            for (const auto& nr : nodes)
            {
                if (nr.idx != srf.idx) continue;
                LocalTransform lt;
                lt.translation = { nr.tx, nr.ty, nr.tz };
                lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
                lt.scale       = { nr.sx, nr.sy, nr.sz };
                if (auto* existing = world.GetComponent<LocalTransform>(resN.rootEntity))
                    *existing = lt;
                if (!nr.name.empty())
                    world.SetName(resN.rootEntity, nr.name);
                break;
            }

            std::vector<Entity> nLoaded;
            CollectSubtreeDFS(world, resN.rootEntity, nLoaded);

            for (size_t i = 0; i < nLoaded.size(); ++i)
                entityMap[srf.idx + static_cast<int>(i)] = nLoaded[i];
            remapPartsByKey(srf.idx, nLoaded, srf.path);

            // Append to loadedEntities so the post-process MaterialOverride
            // loop reaches secondary-scene entities too.
            loadedEntities.insert(loadedEntities.end(), nLoaded.begin(), nLoaded.end());

            if (!world.HasComponent<SceneSourcePath>(resN.rootEntity))
                world.AddComponent<SceneSourcePath>(resN.rootEntity, SceneSourcePath{ srf.path });
        }

        std::vector<NodeRecord> sortedNodes = nodes;
        std::sort(sortedNodes.begin(), sortedNodes.end(),
                  [](const NodeRecord& a, const NodeRecord& b) { return a.idx < b.idx; });

        for (const NodeRecord& nr : sortedNodes)
        {
            if (entityMap.count(nr.idx)) continue;       // already in (loaded scene)
            if (keyDropped.count(nr.idx)) continue;      // a part the model no longer has

            const Entity e = world.CreateEntity();
            world.SetName(e, nr.name.empty() ? "prefab_extra" : nr.name);

            LocalTransform lt;
            lt.translation = { nr.tx, nr.ty, nr.tz };
            lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
            lt.scale       = { nr.sx, nr.sy, nr.sz };
            world.AddComponent<LocalTransform> (e, lt);
            world.AddComponent<GlobalTransform>(e, GlobalTransform{});
            world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
            world.AddComponent<RenderLayer>    (e, RenderLayer{});
            world.AddComponent<Children>       (e, Children{});

            if (nr.parent >= 0)
            {
                auto pit = entityMap.find(nr.parent);
                if (pit != entityMap.end())
                {
                    world.AddComponent<Parent>(e, Parent{ pit->second });
                    if (Children* pc = world.GetComponent<Children>(pit->second))
                        pc->entities.push_back(e);
                }
            }
            entityMap[nr.idx] = e;
        }

        // Apply component blocks via the registry: both loaded-scene entities and the new extras.
        stage.Next(s_stComp);
        for (const auto& cb : compBlocks)
        {
            if (cb.tag == "SceneRef" || cb.tag == "AnimRef") continue;
            // SavePrefab skips EdFolder; a file that still carries one must not drag its authoring folder
            // into every scene. An instance belongs where it is placed.
            if (cb.tag == "EdFolder") continue;
            auto it = entityMap.find(cb.nodeIdx);
            if (it == entityMap.end()) continue;

            ApplyComponentLine(reg, world, it->second, cb.tag, cb.kv, &assetMgr);
        }

        // ---- Fix up cross-entity references (trails -> bones, etc.) ----
        stage.Next(s_stFix);
        auto resolveTargetIdx = [&](const KVMap& kv) -> Entity
        {
            auto it = kv.find("target");
            if (it == kv.end()) return NullEntity;
            try
            {
                const int targetIdx = std::stoi(it->second);
                auto m = entityMap.find(targetIdx);
                return (m != entityMap.end()) ? m->second : NullEntity;
            }
            catch (...) { return NullEntity; }
        };

        for (const auto& cb : compBlocks)
        {
            auto entIt = entityMap.find(cb.nodeIdx);
            if (entIt == entityMap.end()) continue;
            const Entity e = entIt->second;

            if (cb.tag == "PrefabFollowSocket")
            {
                // target=-1 (saved with the target outside the prefab) resolves to NullEntity. The component
                // is still created so the authored socket persists; runtime equip rebinds the target.
                const Entity tgt = resolveTargetIdx(cb.kv);
                FollowSocketComponent fs;
                if (tgt != NullEntity) fs.target.BindAndStamp(world, tgt);
                if (auto it = cb.kv.find("socket"); it != cb.kv.end())
                    try { fs.socketIndex = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
                if (auto it = cb.kv.find("socketHash"); it != cb.kv.end())
                    try { fs.socketNameHash = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
                if (tgt != NullEntity) UpgradeLegacySocketHash(world, tgt, fs);
                if (auto it = cb.kv.find("m"); it != cb.kv.end())
                    MigrateLegacyFollowOffset(world, e, it->second);
                world.AddComponent<FollowSocketComponent>(e, fs);
            }
            else if (cb.tag == "PrefabFollowEntity")
            {
                const Entity tgt = resolveTargetIdx(cb.kv);
                FollowEntityComponent fe;
                if (tgt != NullEntity) fe.target.BindAndStamp(world, tgt);
                if (auto it = cb.kv.find("m"); it != cb.kv.end())
                    MigrateLegacyFollowOffset(world, e, it->second);
                world.AddComponent<FollowEntityComponent>(e, fe);
            }
            else if (cb.tag == "PrefabSkeletonRef")
            {
                const Entity tgt = resolveTargetIdx(cb.kv);
                if (tgt == NullEntity) continue;  // hard dep - skip if unresolved
                world.AddComponent<SkeletonRef>(e, SkeletonRef{ tgt });
            }
        }

        // Post-process: apply MaterialOverride to MaterialComponent
        stage.Next(s_stOvr);
        for (Entity ent : loadedEntities)
        {
            const auto* ovr = world.GetComponent<MaterialOverride>(ent);
            auto* mat = world.GetComponent<MaterialComponent>(ent);
            if (ovr && mat && !ovr->IsEmpty())
            {
                ApplyMaterialOverride(*mat, *ovr);
                mat->SetDirty();
            }

            // Re-stamp the save-time baseline. SceneInstanceLoader recorded the
            // .iscn's own `mat=`; the .ipfb has now had the last word, so THIS is
            // what the entity was actually given. Without the re-stamp every
            // prefab-authored material would look like a user edit to SaveScene
            // and be re-emitted as a per-placement override in every scene.
            if (auto* def = world.GetComponent<MaterialSpawnDefault>(ent))
            {
                if (const auto* sp = world.GetComponent<MaterialSourcePath>(ent))
                    def->path = sp->path;
                def->ovr = ovr ? *ovr : MaterialOverride{};
            }
        }

        // Store source paths on root
        stage.Next(s_stTail);
        if (!world.HasComponent<SceneSourcePath>(res.rootEntity))
            world.AddComponent<SceneSourcePath>(res.rootEntity, SceneSourcePath{ scenePath });

        // An old single AnimRef migrates to an authored AnimationSet.
        if (!animPath.empty() && animClipSys)
        {
            auto* set = world.GetComponent<AnimationSetComponent>(res.rootEntity);
            if (!set)
            {
                world.AddComponent<AnimationSetComponent>(
                    res.rootEntity, AnimationSetComponent{});
                set = world.GetComponent<AnimationSetComponent>(res.rootEntity);
            }
            if (!set->Find("Default"))
                set->Set("Default", animPath, true, 1.f);
            if (set->defaultClip.empty()) set->defaultClip = "Default";
            set->playDefaultOnLoad = true;
            animClipSys->QueueAnimatorPlay(
                world, res.rootEntity, set->defaultClip, 0.f, true);
            world.RemoveComponent<AnimationSourcePath>(res.rootEntity);
        }

        // Final pass: re-wire Parent/Children from saved parentIdx for every
        // node. This handles two cases SceneInstanceLoader can't:
        //   1. The .iscn root has a wrapper parent (sceneIdx > 0).
        //   2. Extras created with a parent pointing into the .iscn DFS
        //      (e.g. a trail node parented under a specific bone).
        // Idempotent for the in-.iscn relationships SceneInstanceLoader
        // already wired.
        for (const NodeRecord& nr : nodes)
        {
            if (nr.parent < 0) continue;
            // A part the key found elsewhere stays where the model put it.
            if (keyRemapped.count(nr.idx)) continue;
            auto eit = entityMap.find(nr.idx);
            if (eit == entityMap.end()) continue;
            auto pit = entityMap.find(nr.parent);
            if (pit == entityMap.end()) continue;

            const Entity child  = eit->second;
            const Entity parent = pit->second;

            Parent* p = world.GetComponent<Parent>(child);
            if (p && p->entity == parent) continue;
            if (p) p->entity = parent;
            else   world.AddComponent<Parent>(child, Parent{ parent });

            Children* ch = world.GetComponent<Children>(parent);
            if (!ch)
            {
                Children newCh; newCh.entities.push_back(child);
                world.AddComponent<Children>(parent, std::move(newCh));
            }
            else
            {
                auto& v = ch->entities;
                if (std::find(v.begin(), v.end(), child) == v.end())
                    v.push_back(child);
            }
        }

        // Prefab root = the entity whose saved parentIdx is -1: the .iscn root for an unwrapped
        // prefab, the wrapper entity otherwise.
        Entity prefabRoot = res.rootEntity;
        for (const NodeRecord& nr : nodes)
        {
            if (nr.parent != -1) continue;
            auto it = entityMap.find(nr.idx);
            if (it != entityMap.end()) { prefabRoot = it->second; break; }
        }

        // Mark the instance root with its source .ipfb so SaveScene stores a
        // PrefabRef line instead of duplicating the whole tree (editing the
        // prefab then propagates to every scene that references it).
        if (prefabRoot != NullEntity && !world.HasComponent<PrefabSourcePath>(prefabRoot))
            world.AddComponent<PrefabSourcePath>(prefabRoot, PrefabSourcePath{ path });

        // Tag every other entity this instantiation created as a member of that root. SaveScene skips
        // members wholesale (see PrefabInstanceMember), including the follow-linked siblings (equipped
        // weapon / trail / emitter) that CollectLinkedFollowers stores as parent=-1 nodes, which a
        // Children walk cannot see and would save twice. loadedEntities joins entityMap: nodes the .iscn
        // gained since the .ipfb was written have no N-line index but are still ours.
        {
            std::unordered_set<Entity> members;
            for (const auto& [idx, ent] : entityMap) members.insert(ent);
            members.insert(loadedEntities.begin(), loadedEntities.end());
            for (Entity ent : members)
            {
                if (ent == prefabRoot || ent == NullEntity) continue;
                if (world.HasComponent<PrefabInstanceMember>(ent)) continue;
                world.AddComponent<PrefabInstanceMember>(ent, PrefabInstanceMember{ prefabRoot });
            }
        }

        LOG_SUCCESS("PrefabSerializer: loaded skinned prefab '%s' — root=%u sceneIdx=%d",
                    path.c_str(), prefabRoot, sceneIdxLocal);
        return prefabRoot;
    }

    // ---- Standard (non-skinned) prefab ----
    stage.Next(s_stPlain);
    std::sort(nodes.begin(), nodes.end(),
              [](const NodeRecord& a, const NodeRecord& b) { return a.idx < b.idx; });

    std::unordered_map<int, Entity> entityMap;
    entityMap.reserve(nodes.size());
    Entity rootEntity = NullEntity;

    for (const NodeRecord& nr : nodes)
    {
        const Entity e = world.CreateEntity();
        world.SetName(e, nr.name.empty() ? "prefab_node" : nr.name);
        entityMap[nr.idx] = e;
        if (rootEntity == NullEntity) rootEntity = e;

        LocalTransform lt;
        lt.translation = { nr.tx, nr.ty, nr.tz };
        lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
        lt.scale       = { nr.sx, nr.sy, nr.sz };

        world.AddComponent<LocalTransform> (e, lt);
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
        world.AddComponent<RenderLayer>    (e, RenderLayer{});
        world.AddComponent<Children>       (e, Children{});

        if (nr.parent >= 0)
        {
            auto it = entityMap.find(nr.parent);
            if (it != entityMap.end())
            {
                world.AddComponent<Parent>(e, Parent{ it->second });
                if (Children* pc = world.GetComponent<Children>(it->second))
                    pc->entities.push_back(e);
            }
        }
    }

    // Apply all component blocks via registry
    for (const auto& cb : compBlocks)
    {
        // Same EdFolder drop as the SceneRef branch: older .ipfb files reach this plain path too.
        if (cb.tag == "EdFolder") continue;
        auto it = entityMap.find(cb.nodeIdx);
        if (it == entityMap.end()) continue;

        ApplyComponentLine(reg, world, it->second, cb.tag, cb.kv, &assetMgr);
    }

    // ---- Pass 2: fix up cross-entity references ----
    // PrefabFollowSocket / PrefabFollowEntity / PrefabSkeletonRef hold prefab-local indices in
    // their `target=N` field, resolved against the freshly built entityMap.
    auto resolveTargetIdx = [&](const KVMap& kv) -> Entity
    {
        auto it = kv.find("target");
        if (it == kv.end()) return NullEntity;
        try
        {
            const int targetIdx = std::stoi(it->second);
            auto m = entityMap.find(targetIdx);
            return (m != entityMap.end()) ? m->second : NullEntity;
        }
        catch (...) { return NullEntity; }
    };

    for (const auto& cb : compBlocks)
    {
        auto entIt = entityMap.find(cb.nodeIdx);
        if (entIt == entityMap.end()) continue;
        const Entity e = entIt->second;

        if (cb.tag == "PrefabFollowSocket")
        {
            // target=-1 -> component created with a null target; equip code rebinds.
            const Entity tgt = resolveTargetIdx(cb.kv);
            FollowSocketComponent fs;
            if (tgt != NullEntity) fs.target.BindAndStamp(world, tgt);
            if (auto it = cb.kv.find("socket"); it != cb.kv.end())
                try { fs.socketIndex = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
            if (auto it = cb.kv.find("socketHash"); it != cb.kv.end())
                try { fs.socketNameHash = static_cast<uint32_t>(std::stoul(it->second)); } catch (...) {}
            if (tgt != NullEntity) UpgradeLegacySocketHash(world, tgt, fs);
            if (auto it = cb.kv.find("m"); it != cb.kv.end())
                MigrateLegacyFollowOffset(world, e, it->second);
            world.AddComponent<FollowSocketComponent>(e, fs);
        }
        else if (cb.tag == "PrefabFollowEntity")
        {
            const Entity tgt = resolveTargetIdx(cb.kv);
            FollowEntityComponent fe;
            if (tgt != NullEntity) fe.target.BindAndStamp(world, tgt);
            if (auto it = cb.kv.find("m"); it != cb.kv.end())
                MigrateLegacyFollowOffset(world, e, it->second);
            world.AddComponent<FollowEntityComponent>(e, fe);
        }
        else if (cb.tag == "PrefabSkeletonRef")
        {
            const Entity tgt = resolveTargetIdx(cb.kv);
            if (tgt == NullEntity) continue;  // hard dep - skip if unresolved
            world.AddComponent<SkeletonRef>(e, SkeletonRef{ tgt });
        }
    }

    // Post-process: apply MaterialOverride to MaterialComponent
    for (const auto& [idx, e] : entityMap)
    {
        const auto* ovr = world.GetComponent<MaterialOverride>(e);
        auto* mat = world.GetComponent<MaterialComponent>(e);
        if (ovr && mat && !ovr->IsEmpty())
        {
            ApplyMaterialOverride(*mat, *ovr);
            mat->SetDirty();
        }
    }

    // Add default material to mesh entities that don't have one
    for (const auto& [idx, e] : entityMap)
    {
        if (world.HasComponent<MeshLibRef>(e) && !world.HasComponent<MaterialComponent>(e))
            world.AddComponent<MaterialComponent>(e, MaterialComponent{});
    }

    // Mark the instance root with its source .ipfb (see the skinned path above)
    // so SaveScene stores a reference, not a copy of the tree.
    if (rootEntity != NullEntity && !world.HasComponent<PrefabSourcePath>(rootEntity))
        world.AddComponent<PrefabSourcePath>(rootEntity, PrefabSourcePath{ path });

    // Ownership tag for every non-root entity, the same contract as the skinned path (see
    // PrefabInstanceMember).
    for (const auto& [idx, e] : entityMap)
    {
        if (e == rootEntity || e == NullEntity) continue;
        if (world.HasComponent<PrefabInstanceMember>(e)) continue;
        world.AddComponent<PrefabInstanceMember>(e, PrefabInstanceMember{ rootEntity });
    }

    LOG_SUCCESS("PrefabSerializer: loaded '%s' — %u nodes, root=%u",
                path.c_str(), static_cast<uint32_t>(nodes.size()), rootEntity);
    return rootEntity;
}

// ==== Spawn preparation and the preview's readers ====
namespace
{
    // The models a prefab's SceneRef blocks name (lowest node first) and the
    // materials its MaterialRef blocks name (canonical, unique). Pure.
    void CollectSpawnRefs(const std::vector<ComponentBlock>& compBlocks, Resource::PrefabSpawnRefs& out);
}

// PreparePrefabForSpawn's parse, done off the main thread (see ParsePrefabForSpawn).
struct Resource::PrefabSpawnParse
{
    std::string                 text;
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    PrefabSpawnRefs             refs;
};

std::shared_ptr<Resource::PrefabSpawnParse> Resource::ParsePrefabForSpawn(const std::string& path,
                                                                          const std::vector<uint8_t>& bytes)
{
    PROF_SCOPE("Prefab.ParseOnLoader");
    if (!::Resource::ValidateHeader(bytes.data(), bytes.size(), ::Resource::MAGIC_PREFAB))
    {
        LOG_ERROR("PrefabSerializer: invalid .ipfb header in '%s'", path.c_str());
        return nullptr;
    }
    auto p  = std::make_shared<PrefabSpawnParse>();
    p->text = ResolvePrefabText(bytes, path);
    ParsePrefabRecords(p->text, p->nodes, p->compBlocks);
    CollectSpawnRefs(p->compBlocks, p->refs);
    return p;
}

void Resource::PutParsedPrefabForSpawn(const std::string& path, PrefabSpawnParse& parsed, PrefabSpawnRefs& out)
{
    out = parsed.refs;
    SpawnParsedPrefabs()[path] = ParsedPrefab{ std::move(parsed.text), std::move(parsed.nodes),
                                               std::move(parsed.compBlocks) };
}

bool Resource::PreparePrefabForSpawn(const std::string& path, PrefabSpawnRefs& out)
{
    PROF_SCOPE("Prefab.PreparePreload");
    out = PrefabSpawnRefs{};
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    if (!ReadAndParsePrefab(path, /*fromTemplate*/ ::Resource::PrefabTemplate::Enabled(), nodes, compBlocks))
        return false;
    CollectSpawnRefs(compBlocks, out);
    return true;
}

namespace
{
void CollectSpawnRefs(const std::vector<ComponentBlock>& compBlocks, Resource::PrefabSpawnRefs& out)
{
    std::vector<std::pair<int, std::string>> scenes;
    for (const ComponentBlock& cb : compBlocks)
    {
        const auto it = cb.kv.find("path");
        if (it == cb.kv.end()) continue;
        if (cb.tag == "SceneRef")
            scenes.emplace_back(cb.nodeIdx, PercentDecode(it->second));
        else if (cb.tag == "MaterialRef")
        {
            // The MaterialRef deserializer's canonical form.
            const std::string m = Sys::RelativizeAssetPath(Sys::PathToUtf8(
                Sys::U8Path(PercentDecode(it->second)).lexically_normal()));
            if (!m.empty() && std::find(out.materials.begin(), out.materials.end(), m) == out.materials.end())
                out.materials.push_back(m);
        }
    }
    std::sort(scenes.begin(), scenes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& s : scenes)
        if (std::find(out.scenes.begin(), out.scenes.end(), s.second) == out.scenes.end())
            out.scenes.push_back(std::move(s.second));
}
}   // namespace

// A thin forward to CollectSubtreeDFS, the walk LoadPrefab uses, so the -preview_asset_verify
// self-check and the thing it checks are one walk.
void Resource::CollectPrefabSubtreeDFS(World& world, Entity root,
                                       std::vector<Entity>& out)
{
    CollectSubtreeDFS(world, root, out);
}

// ReadPrefabNodes shares ParseNLine / ParseComponentLine and AssetRefs::ResolveSceneText with
// LoadPrefab, so a prefab that loads and one that previews cannot disagree about its text.
bool Resource::ReadPrefabNodes(const std::string& path,
                               std::vector<PrefabNodeView>& out,
                               PrefabSkipCounts* outSkipped)
{
    out.clear();
    if (outSkipped) *outSkipped = PrefabSkipCounts{};

    std::vector<uint8_t> data;
    if (!::Resource::AssetFS::Get().ReadFile(path, data))
    {
        LOG_WARNING("PrefabSerializer::ReadPrefabNodes: cannot open '%s'", path.c_str());
        return false;
    }
    if (!ValidateHeader(data.data(), data.size(), MAGIC_PREFAB))
    {
        LOG_WARNING("PrefabSerializer::ReadPrefabNodes: invalid .ipfb header in '%s'",
                    path.c_str());
        return false;
    }

    // No repair count and no warning: this read is for a picture, and LoadPrefab reports the same
    // repairs where a user can act on them. nullptr keeps the resolution identical.
    const std::string prefabText = AssetRefs::ResolveSceneText(
        std::string(GetPayloadText(data.data(), data.size())), path, nullptr);

    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> blocks;
    {
        std::istringstream stream(prefabText);
        std::string        line;
        int currentNodeIdx = -1;
        while (std::getline(stream, line))
        {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == 'N' && line.size() > 1 && line[1] == ' ')
            {
                NodeRecord nr;
                if (ParseNLine(line, nr))
                {
                    currentNodeIdx = nr.idx;
                    nodes.push_back(std::move(nr));
                }
            }
            else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
            {
                ComponentBlock cb;
                if (ParseComponentLine(line, currentNodeIdx, cb))
                    blocks.push_back(std::move(cb));
            }
        }
    }
    if (nodes.empty()) return false;

    std::sort(nodes.begin(), nodes.end(),
              [](const NodeRecord& a, const NodeRecord& b) { return a.idx < b.idx; });

    std::unordered_map<int, size_t> pos;
    out.reserve(nodes.size());
    for (const NodeRecord& nr : nodes)
    {
        PrefabNodeView v;
        v.idx        = nr.idx;
        v.parent     = nr.parent;
        v.name       = nr.name;
        v.sectionKey = nr.msec;
        pos[nr.idx] = out.size();
        out.push_back(std::move(v));
    }

    // ---- The component lines a preview reads ----
    for (const ComponentBlock& cb : blocks)
    {
        auto it = pos.find(cb.nodeIdx);
        if (it == pos.end()) continue;
        PrefabNodeView& v = out[it->second];
        if (cb.tag == "SceneRef")
        {
            auto p = cb.kv.find("path");
            if (p != cb.kv.end()) v.sceneRefPath = PercentDecode(p->second);
        }
        else if (cb.tag == "MaterialRef")
        {
            auto p = cb.kv.find("path");
            if (p != cb.kv.end()) v.materialRefPath = PercentDecode(p->second);
        }
        else if (cb.tag == "MaterialInline")
        {
            // `data=` is PercentEncodeStrict'd by the writer (it is a whole
            // material text, newlines and all), and PercentDecode is the
            // inverse of both encoders.
            auto p = cb.kv.find("data");
            if (p != cb.kv.end()) v.materialInlineText = PercentDecode(p->second);
        }
        else if (cb.tag == "MaterialOverride")
        {
            // The shared parser, not a copy (see ParseMaterialOverrideKV).
            ParseMaterialOverrideKV(cb.kv, v.materialOverride);
            v.hasMaterialOverride = !v.materialOverride.IsEmpty();
        }
        else if (cb.tag == "MeshRef")
        {
            v.hasMeshRef = true;
        }
        else if (cb.tag == "MeshHandle")
        {
            auto p = cb.kv.find("gpuMeshID");
            if (p != cb.kv.end())
            {
                try { v.gpuMeshId = std::stoi(p->second); } catch (...) {}
            }
        }
    }

    // ---- What the walk declined ----
    // A second pass over `blocks` reading cb.tag only: no payload, no PrefabNodeView field (see
    // PrefabSkipCounts for which tags belong). The tag table is checked against
    // ComponentSerializerRegistry on first use - one ENGINE_CHECK_ONCE, one registry scan per
    // process, and on failure the full registered tag list - so a renamed serializer reports
    // instead of counting zero (the asset_preview_skipped fixture uses the same literals and
    // cannot catch it).
    if (outSkipped)
    {
        enum Kind : uint32_t
        {
            kEmitter = 1u << 0, kTrail    = 1u << 1, kLight   = 1u << 2,
            kDecal   = 1u << 3, kBillboard = 1u << 4, kSurface = 1u << 5,
            kWorldUI = 1u << 6
        };
        struct DeclinedTag { const char* tag; uint32_t bit; };
        static constexpr DeclinedTag kDeclinedTags[] = {
            // VFX. A node normally carries several of these at once, which is
            // why the fold below is per NODE.
            { "ParticleRender",  kEmitter },  { "ParticleEmit",    kEmitter },
            { "ParticleFlock",   kEmitter },  { "ParticleAmbient", kEmitter },
            // The combined emitter tag: still read from older files, never written.
            { "ParticleEmitter", kEmitter },
            { "Trail",           kTrail   },  { "Beam",            kTrail   },
            { "LightData",       kLight   },  { "VolumetricLight", kLight   },
            { "SunLight",        kLight   },  { "MoonLight",       kLight   },
            { "TODLight",        kLight   },
            { "Decal",           kDecal   },
            { "Billboard",       kBillboard }, { "BillboardFX",    kBillboard },
            { "Grass",           kSurface },  { "Terrain",         kSurface },
            { "Water",           kSurface },
            { "WorldUI",         kWorldUI },  { "WorldUIBar",      kWorldUI },
            { "WorldUIImage",    kWorldUI },  { "WorldUIText",     kWorldUI },
        };

        // ONE registry scan per process. The flag is a plain static rather than
        // an atomic for the same reason ENGINE_CHECK_ONCE's is: a simultaneous
        // first call on two threads scans twice and logs twice, which is
        // harmless, and ReadPrefabNodes is a cold path anyway.
        static bool s_tagTableChecked = false;
        if (!s_tagTableChecked)
        {
            s_tagTableChecked = true;
            const ComponentSerializerRegistry& reg = GetComponentRegistry();
            std::string missing;
            for (const DeclinedTag& d : kDeclinedTags)
                if (!reg.FindByTag(d.tag))
                {
                    if (!missing.empty()) missing += ", ";
                    missing += d.tag;
                }
            if (!ENGINE_CHECK_ONCE(missing.empty(),
                    "the prefab preview's declined-kind table names %s, which "
                    "no component serializer registers any more - the asset "
                    "panel's 'not previewed' tally silently counts ZERO for "
                    "that kind. Fix the spelling in PrefabSerializer.cpp "
                    "(kDeclinedTags) against ComponentSerializers.cpp.",
                    missing.c_str()))
            {
                std::string tags;
                for (const auto& kvp : reg.All())
                {
                    if (!tags.empty()) tags += ", ";
                    tags += kvp.second.tag;
                }
                LOG_WARNING("Registered component serializer tags (%zu): %s",
                            reg.All().size(), tags.c_str());
            }
        }

        std::unordered_map<int, uint32_t> kindBitsByNode;
        for (const ComponentBlock& cb : blocks)
        {
            // The same membership test as the walk: a block whose nodeIdx names no N line is a malformed
            // file, not a declined node; counting it would report a skip the panel cannot point at.
            if (pos.find(cb.nodeIdx) == pos.end()) continue;
            uint32_t bit = 0;
            for (const DeclinedTag& d : kDeclinedTags)
                if (cb.tag == d.tag) { bit = d.bit; break; }
            if (bit) kindBitsByNode[cb.nodeIdx] |= bit;
        }
        for (const auto& kv : kindBitsByNode)
        {
            const uint32_t b = kv.second;
            if (b & kEmitter)   ++outSkipped->emitters;
            if (b & kTrail)     ++outSkipped->trails;
            if (b & kLight)     ++outSkipped->lights;
            if (b & kDecal)     ++outSkipped->decals;
            if (b & kBillboard) ++outSkipped->billboards;
            if (b & kSurface)   ++outSkipped->surfaces;
            if (b & kWorldUI)   ++outSkipped->worldUI;
        }
    }

    // ---- Compose the world transforms ----
    // Ascending idx suffices: SavePrefab writes parents before children (a DFS from the root), so a
    // parent's matrix is always done. A parent index that runs forward (a hand-edited file)
    // composes against identity - a defined fallback, not uninitialised memory.
    using namespace DirectX;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const NodeRecord& nr = nodes[i];
        PrefabNodeView&   v  = out[i];
        const XMMATRIX local = XMMatrixAffineTransformation(
            XMVectorSet(nr.sx, nr.sy, nr.sz, 0.f),
            XMVectorZero(),
            XMVectorSet(nr.qx, nr.qy, nr.qz, nr.qw),
            XMVectorSet(nr.tx, nr.ty, nr.tz, 0.f));
        XMMATRIX parentWorld = XMMatrixIdentity();
        if (nr.parent >= 0)
        {
            auto pit = pos.find(nr.parent);
            if (pit != pos.end() && pit->second < i)
                parentWorld = XMLoadFloat4x4(
                    reinterpret_cast<const XMFLOAT4X4*>(out[pit->second].world));
        }
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixMultiply(local, parentWorld));
        std::memcpy(v.world, &m, sizeof(float) * 16);
    }
    return true;
}

// ==== A prefab's model parts by section key (see the header) ====
Resource::PrefabPartMatch Resource::MatchPrefabPartsByKey(const std::vector<std::pair<int, uint64_t>>& saved,
                                                          int first, const std::vector<uint64_t>& liveKeys)
{
    PrefabPartMatch m;
    std::unordered_map<uint64_t, std::vector<int>> liveByKey;   // key -> live ordinals, DFS order
    for (size_t i = 0; i < liveKeys.size(); ++i)
        if (liveKeys[i]) liveByKey[liveKeys[i]].push_back(static_cast<int>(i));
    std::vector<std::pair<int, uint64_t>> ordered = saved;       // saved order = idx order
    std::sort(ordered.begin(), ordered.end());
    std::unordered_map<uint64_t, size_t> used;
    for (const auto& [idx, key] : ordered)
    {
        const auto it = liveByKey.find(key);
        size_t&    k  = used[key];
        if (it == liveByKey.end() || k >= it->second.size())
        {
            m.goneNodes.push_back(idx);
            continue;
        }
        const int ord = it->second[k++];
        if (ord != idx - first) ++m.moved;
        m.nodeToLive.emplace_back(idx, ord);
    }
    return m;
}

uint64_t Resource::PrefabPartKeyOf(World& world, Entity e)
{
    const MeshLibRef*     ref = world.GetComponent<MeshLibRef>(e);
    const MeshSourcePath* sp  = world.GetComponent<MeshSourcePath>(e);
    if (!ref || !sp || sp->path.empty()) return 0;
    const auto t = MeshLibKeyTableAt(sp->path);
    return t ? t->KeyOf(ref->meshId) : 0;
}
