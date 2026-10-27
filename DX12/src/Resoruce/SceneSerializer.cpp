#include "Resource/SceneSerializer.h"
#include "Resource/SceneText.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"   // guid-augmented references: survive a rename
#include "Resource/AssetManager.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceHandle.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/PrefabSerializer.h"   // LoadPrefab: PrefabRef rows are re-instantiated (LoadScene step 8b)
#include "Resource/SceneLoadContext.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialOverrideApply.h"   // shared ApplyMaterialOverride
#include "Resource/MeshLibIdentity.h"   // srcSec= / meshSec= (MeshLibKeyTableAt)
#include "Resource/PostProcessConfig.h"
#include "ECS/GuidRegistry.h"
#include "ECS/FollowComponents.h"   // attachment-target GUID pre-stamp pass
#include "UI/UICanvas.h"           // UI::UIParent: stamp parent GUIDs before save
#include "System/TaskSystem.h"
#include "System/PathUtil.h"
#include "System/Stage.h"          // loading_scene / saving_scene for the control pipe's status
#include "ECS/Components.h"
#include "ECS/MaterialSchema.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/ReflectionProbeComponent.h"   // proxyVolume AttachmentRef pre-stamp
#include "ECS/AnimationComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/PhysicsComponents.h"   // ColliderComponent / RigidBodyComponent (MC override lines)
#include "ECS/FaceShadingComponent.h" // FaceShadingComponent (FS override lines on skinned children)
#include "Physics/ChainPhysicsSystem.h" // ChainPhysicsComponent / BoneRestPose (CPR override lines)
#include "Scene/SceneInstanceLoader.h"
#include "Renderer/Renderer.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DirectX;
using Resource::AnimHandle;

namespace
{
struct NodeInfo { int idx; int parentIdx; Entity entity; };

// N line -> node record.
struct NodeRecord
{
    int   idx    { -1 };
    int   parent { -1 };
    float tx{0}, ty{0}, tz{0};
    float qx{0}, qy{0}, qz{0}, qw{1};
    float sx{1}, sy{1}, sz{1};
    std::string name;
};

// Indented component line -> tag + KVMap.
struct ComponentBlock { int nodeIdx; std::string tag; KVMap kv; };

std::string PercentEncode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s)
    {
        if (c == '%')       r += "%25";
        else if (c == ' ')  r += "%20";
        else                r += c;
    }
    return r;
}

// Strict variant for values that are AUTHORED text rather than paths: a bone name may contain
// '=' or '#', and every line parser here splits on the FIRST '=' and drops '#'-leading lines, so
// the weak encoder above would tear the line in half. Decodes with the same PercentDecode.
std::string PercentEncodeStrict(const std::string& s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string r;
    r.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s)
    {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                          c == '/' || c == '-' || c == ':';
        if (safe) r += static_cast<char>(c);
        else { r += '%'; r += kHex[c >> 4]; r += kHex[c & 0xF]; }
    }
    return r;
}

std::string PercentDecode(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int h = hex(s[i+1]), l = hex(s[i+2]);
            if (h >= 0 && l >= 0) { r += static_cast<char>(h*16+l); i += 2; continue; }
        }
        r += s[i];
    }
    return r;
}

// Collect authored scene entities into @p out, filling @p entityToIdx and each node's parentIdx.
// System-owned SceneTransientTag entities deliberately have no on-disk row; otherwise their
// runtime-only components are absent from the serializer registry and the row comes back from
// LoadScene as an empty, unnamed entity.
void CollectAllEntities(World& world,
                        std::unordered_map<Entity, int>& entityToIdx,
                        std::vector<NodeInfo>& out)
{
    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        if (world.HasComponent<SceneTransientTag>(e)) continue;
        int idx = static_cast<int>(out.size());
        entityToIdx[e] = idx;
        out.push_back({ idx, -1, e });
    }
    for (auto& ni : out)
    {
        const Parent* p = world.GetComponent<Parent>(ni.entity);
        if (p && p->entity != NullEntity)
        {
            auto it = entityToIdx.find(p->entity);
            if (it != entityToIdx.end())
                ni.parentIdx = it->second;
        }
    }
}

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
            else if (key == "tx") out.tx = std::stof(val); else if (key == "ty") out.ty = std::stof(val);
            else if (key == "tz") out.tz = std::stof(val);
            else if (key == "qx") out.qx = std::stof(val); else if (key == "qy") out.qy = std::stof(val);
            else if (key == "qz") out.qz = std::stof(val); else if (key == "qw") out.qw = std::stof(val);
            else if (key == "sx") out.sx = std::stof(val); else if (key == "sy") out.sy = std::stof(val);
            else if (key == "sz") out.sz = std::stof(val);
            else if (key == "name") out.name = PercentDecode(val);
        }
        catch (...) {}
    }
    return out.idx >= 0;
}

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

// ==== Pre-load helpers ====
// LoadScene kicks off async I/O for every loader-backed path BEFORE it clears the world, waits
// for the RM slots to leave Loading, then clears and deserializes: new resources load on workers
// while the old world still renders, and path-hash dedup keeps a texture both worlds use warm across
// the swap (a clip both worlds use is not: see LoadScene step 4).
// Only paths whose extension has a registered IResourceLoader can be batched: textures
// (MaterialOverride 't:'-typed values) and anim clips (AnimationSet paths, AnimRef.path).
// .iscn / .meshlib / .imsh stay on the synchronous path inside SceneInstanceLoader / MeshLibrary.

struct PreloadEntry
{
    std::string            path;
    Resource::ResourceType type;
};

std::vector<PreloadEntry> CollectPreloadPaths(const std::vector<ComponentBlock>& compBlocks)
{
    std::vector<PreloadEntry> out;
    std::unordered_set<uint64_t> seen;

    auto Add = [&](std::string path, Resource::ResourceType type) {
        if (path.empty()) return;
        // FNV-1a of the path: RM normalizes on Load itself, this only dedups within one LoadScene call.
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
        if (!seen.insert(h).second) return;
        out.push_back({ std::move(path), type });
    };

    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "MaterialOverride")
        {
            for (const auto& [rawKey, rawVal] : cb.kv)
            {
                if (rawVal.size() < 2 || rawVal[0] != 't' || rawVal[1] != ':') continue;
                Add(PercentDecode(rawVal.substr(2)), Resource::ResourceType::Texture);
            }
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
                Add(PercentDecode(it->second), Resource::ResourceType::Animation);
        }
        else if (cb.tag == "AnimationSet")
        {
            int count = 0;
            if (auto it = cb.kv.find("n"); it != cb.kv.end())
            {
                try { count = std::clamp(std::stoi(it->second), 0, 128); }
                catch (...) { count = 0; }
            }
            for (int i = 0; i < count; ++i)
            {
                const std::string key = "path" + std::to_string(i);
                if (auto it = cb.kv.find(key); it != cb.kv.end())
                    Add(PercentDecode(it->second), Resource::ResourceType::Animation);
            }
        }
    }
    return out;
}

// Collect .iscn paths (SceneRef components) for a fire-and-forget OS page-cache warmup.
// SceneInstanceLoader::Load still reads them synchronously after Clear, but from RAM instead of
// disk. No RM integration: these extensions have no registered IResourceLoader.
std::vector<std::string> CollectScenePaths(const std::vector<ComponentBlock>& compBlocks)
{
    std::vector<std::string> out;
    std::unordered_set<uint64_t> seen;
    for (const auto& cb : compBlocks)
    {
        if (cb.tag != "SceneRef") continue;
        auto it = cb.kv.find("path");
        if (it == cb.kv.end()) continue;
        std::string path = PercentDecode(it->second);
        if (path.empty()) continue;
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
        if (!seen.insert(h).second) continue;
        out.push_back(std::move(path));
    }
    return out;
}

// Recursive page-cache warmup for one .iscn: opens the file on a worker, parses out the sibling
// references (.meshlib via F file=, .imat via F mat=, .iskel via K file=, .imorph via M file=)
// and prefetches each unique sibling (AssetFS::Prefetch: the IO thread reads its stored bytes, a
// zstd entry undecoded), so every file SceneInstanceLoader's sync read touches is already in the OS
// page cache. No RM integration (no registered IResourceLoader for these extensions). Cost: the
// .iscn text (<= a few hundred KB even for Bistro) + N prefetches.
void EnqueueIscnRecursiveWarmup(const std::string& scenePath)
{
    TaskSystem::Get().Push([scenePath]()
    {
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(scenePath, blob)) return;
        if (blob.empty()) return;
        if (!::Resource::ValidateHeader(blob.data(), blob.size(),
                                        ::Resource::MAGIC_SCENE)) return;

        // Not the bare payload pointer: a NUL-less .iscn (older PmxImporter output) would be read on
        // past the end of the blob.
        const std::string text(::Resource::GetPayloadText(blob.data(), blob.size()));
        const std::filesystem::path sceneDir =
            Sys::U8Path(scenePath).parent_path();

        std::unordered_set<uint64_t> seen;  // dedup per .iscn
        uint32_t enqueued = 0;
        auto pushWarmup = [&seen, &sceneDir, &enqueued](std::string filename) {
            // Trim trailing CR/LF/space (line.find returns up-to-EOL substring).
            while (!filename.empty() &&
                   (filename.back() == '\r' || filename.back() == '\n' ||
                    filename.back() == ' '))
                filename.pop_back();
            if (filename.empty()) return;

            std::string absPath = Sys::PathToUtf8(sceneDir / Sys::U8Path(filename));
            uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : absPath) { h ^= c; h *= 1099511628211ULL; }
            if (!seen.insert(h).second) return;

            ++enqueued;
            ::Resource::AssetFS::Get().Prefetch(absPath);
        };

        // Extract a `key=` token's value up to the next space (or EOL).
        auto extractToken = [](const std::string& line, const char* key) -> std::string {
            const size_t kpos = line.find(key);
            if (kpos == std::string::npos) return {};
            const size_t start = kpos + std::strlen(key);
            const size_t end   = line.find(' ', start);
            return (end == std::string::npos)
                ? line.substr(start)
                : line.substr(start, end - start);
        };

        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.size() < 2 || line[0] == '#') continue;

            // F idx=I file=<name.meshlib|name.imsh> mesh=M [mat=<name.imat>]
            if (line[0] == 'F' && line[1] == ' ')
            {
                pushWarmup(extractToken(line, "file="));
                pushWarmup(extractToken(line, "mat="));
            }
            // K file=<name.iskel>
            else if (line[0] == 'K' && line[1] == ' ')
            {
                pushWarmup(extractToken(line, "file="));
            }
            // M idx=I file=<name.imorph>
            else if (line[0] == 'M' && line[1] == ' ')
            {
                pushWarmup(extractToken(line, "file="));
            }
        }

        LOG_INFO("SceneSerializer: .iscn warmup '%s' queued %u sibling reads",
                 scenePath.c_str(), enqueued);
    });
}

void WaitForHandlesReady(Resource::ResourceManager& rm,
                         const std::vector<Resource::Handle>& handles)
{
    using namespace std::chrono_literals;
    if (handles.empty()) return;

    // [LOAD-PROFILE] This spin-wait blocks the main thread while workers finish CPU decode: the
    // CPU-bound half of the load stall (EditorLayer measures the GPU upload half).
    const auto _t0 = std::chrono::steady_clock::now();

    // The main-thread GPU upload pump must run inside the wait: workers finish file I/O, but the
    // final ID3D12Resource upload happens here, so without the pump textures stay at Loading forever.
    auto lastProgress = std::chrono::steady_clock::time_point{};
    while (true)
    {
        bool anyLoading = false;
        for (Resource::Handle h : handles)
        {
            if (rm.GetState(h) == Resource::ResourceState::Loading)
            {
                anyLoading = true;
                break;
            }
        }
        // How far the wait is, for the control pipe's `status` (Sys::Stage):
        // a full count, so at most every 100 ms.
        if (const auto now = std::chrono::steady_clock::now(); now - lastProgress >= 100ms || !anyLoading)
        {
            lastProgress = now;
            std::size_t loading = 0;
            for (Resource::Handle h : handles)
                if (rm.GetState(h) == Resource::ResourceState::Loading) ++loading;
            Sys::Stage::Progress(handles.size() - loading, handles.size(), "assets");
        }
        rm.ProcessPendingGPUUploads(2.0f);
        if (!anyLoading) break;
        std::this_thread::sleep_for(1ms);
    }

    const double _ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - _t0).count();
    LOG_INFO("[LOAD-PROFILE] WaitForHandlesReady: %.1f ms spin-waiting on %zu handles "
             "(CPU decode wait) <- Phase-1 non-blocking-pump target",
             _ms, handles.size());
}

// ==== Override lines name their part by key ====
// An MC / FS / MO / MR / MRI / MOI line names a part of a model as (src=<library>,
// srcMesh=<entry>), and a collider's mesh as (meshPath=, meshId=). An entry is a POSITION, which a
// re-import that reorders the library moves, so each line also carries the entry's SectionKey
// (srcSec= / meshSec=, read from the library file by Resource::MeshLibKeyTableAt: its IDNT, or the
// positional keys of a file without one) and the replay resolves the KEY first. The entry stays
// as the hint an older binary reads (its readers skip a key they do not know: every one matches
// keys by name, and MO / MOI values need a "<type>:" prefix).

// " <name>=<hex16>" for @p entry of @p libPath, or "" when the file does not read.
std::string SectionKeyToken(const char* name, const std::string& libPath, uint32_t entry)
{
    if (libPath.empty()) return {};
    const auto t = Resource::MeshLibKeyTableAt(libPath);
    const uint64_t k = t ? t->KeyOf(entry) : 0;
    return k ? std::string(" ") + name + "=" + Resource::MeshKeyToHex(k) : std::string();
}

uint64_t ParseSectionKey(const std::string& v)
{
    uint64_t k = 0;
    return Resource::MeshKeyFromHex(v, k) ? k : 0;
}

struct KeyResolveStats { uint32_t moved = 0, missing = 0; };

// The entry a line names: its key's, when the library has that key; else the hint.
uint32_t EntryForKey(const std::string& libPath, uint32_t hint, uint64_t key, KeyResolveStats& st)
{
    if (key == 0 || libPath.empty()) return hint;
    const auto t = Resource::MeshLibKeyTableAt(libPath);
    if (!t) return hint;
    const int e = t->Find(key);
    if (e < 0) { ++st.missing; return hint; }
    if (static_cast<uint32_t>(e) != hint) ++st.moved;
    return static_cast<uint32_t>(e);
}

// The authored fields of a part's collider / body against what the model gave it at spawn
// (ColliderSpawnDefault). Tolerant: an MC line is written at %.4f, so a replayed line differs from
// the model's value in the 5th decimal.
bool NearF(float a, float b) { return std::fabs(a - b) <= 6e-5f + 1e-6f * std::fabs(a); }
bool Near3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
{
    return NearF(a.x, b.x) && NearF(a.y, b.y) && NearF(a.z, b.z);
}
bool SameAuthoredCollider(const ColliderComponent& a, const ColliderComponent& b)
{
    return a.shape == b.shape && Near3(a.halfExtents, b.halfExtents) && NearF(a.radius, b.radius) &&
           NearF(a.halfHeight, b.halfHeight) && Near3(a.offset, b.offset) &&
           Near3(a.rotationEulerDeg, b.rotationEulerDeg) && a.isTrigger == b.isTrigger && a.layer == b.layer &&
           Resource::AssetRefs::NormalizeRefPath(a.meshLibPath) == Resource::AssetRefs::NormalizeRefPath(b.meshLibPath) &&
           a.meshLibMeshId == b.meshLibMeshId &&
           Resource::AssetRefs::NormalizeRefPath(a.assetModel) == Resource::AssetRefs::NormalizeRefPath(b.assetModel) &&
           a.assetMeshKey == b.assetMeshKey;
}
bool SameAuthoredBody(const RigidBodyComponent& a, const RigidBodyComponent& b)
{
    return a.motion == b.motion && NearF(a.mass, b.mass) && NearF(a.linearDamping, b.linearDamping) &&
           NearF(a.angularDamping, b.angularDamping) && NearF(a.friction, b.friction) &&
           NearF(a.restitution, b.restitution) && NearF(a.gravityFactor, b.gravityFactor) &&
           a.lockedAxes == b.lockedAxes;
}

bool ColliderHasMesh(const ColliderComponent& c)
{
    return (c.shape == ColliderComponent::Shape::Mesh || c.shape == ColliderComponent::Shape::ConvexHull) &&
           !c.meshLibPath.empty();
}

// One MC line (see SaveScene for why they exist): the part it names by (library,
// entry) and the collider / body it gives every part with that key.
struct MeshColliderOverride
{
    std::string         sourcePath;
    uint32_t            sourceMeshId = 0;
    uint64_t            sourceSec    = 0;   // srcSec= (0: none)
    uint64_t            meshSec      = 0;   // meshSec=: the collider's mesh
    ColliderComponent   collider;
    RigidBodyComponent  rb;
    bool                hasCollider  = false;
    bool                hasRb        = false;
};

// MC src=<path> srcMesh=N hasCol=1 hasRb=1 <collider fields> <rigidbody fields>.
// A key it does not know (a `$g_` guid token LoadScene has not stripped) is skipped. A malformed
// number throws (std::stoi / std::stof).
void ParseMCLine(const std::string& line, MeshColliderOverride& mco)
{
    std::istringstream ls(line.substr(3));
    std::string token;
    while (ls >> token)
    {
        const auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = token.substr(0, eq);
        const std::string v = token.substr(eq + 1);
        if      (k == "src")          mco.sourcePath = PercentDecode(v);
        else if (k == "srcMesh")      mco.sourceMeshId = static_cast<uint32_t>(std::stoul(v));
        else if (k == "srcSec")       mco.sourceSec    = ParseSectionKey(v);
        else if (k == "meshSec")      mco.meshSec      = ParseSectionKey(v);
        else if (k == "model")        mco.collider.assetModel   = PercentDecode(v);   // the MeshAsset collider's model
        else if (k == "meshKey")      mco.collider.assetMeshKey = ParseSectionKey(v);
        else if (k == "hasCol")       mco.hasCollider = (std::stoi(v) != 0);
        else if (k == "hasRb")        mco.hasRb       = (std::stoi(v) != 0);
        else if (k == "shape")        mco.collider.shape = static_cast<ColliderComponent::Shape>(std::stoi(v));
        else if (k == "halfExtents")
        {
            float x=0,y=0,z=0;
            sscanf_s(v.c_str(), "%f_%f_%f", &x, &y, &z);
            mco.collider.halfExtents = { x, y, z };
        }
        else if (k == "radius")       mco.collider.radius     = std::stof(v);
        else if (k == "halfHeight")   mco.collider.halfHeight = std::stof(v);
        else if (k == "offset")
        {
            float x=0,y=0,z=0;
            sscanf_s(v.c_str(), "%f_%f_%f", &x, &y, &z);
            mco.collider.offset = { x, y, z };
        }
        else if (k == "rotEuler")
        {
            float x=0,y=0,z=0;
            sscanf_s(v.c_str(), "%f_%f_%f", &x, &y, &z);
            mco.collider.rotationEulerDeg = { x, y, z };
        }
        else if (k == "isTrigger")    mco.collider.isTrigger = (std::stoi(v) != 0);
        else if (k == "layer")
            mco.collider.layer =
                static_cast<ColliderComponent::CollisionLayer>(std::stoi(v));
        else if (k == "lockedAxes")
            mco.rb.lockedAxes = static_cast<std::uint8_t>(std::stoul(v));
        else if (k == "meshPath")     mco.collider.meshLibPath   = PercentDecode(v);
        else if (k == "meshId")       mco.collider.meshLibMeshId = static_cast<uint32_t>(std::stoul(v));
        else if (k == "mt")           mco.rb.motion          = static_cast<RigidBodyComponent::Motion>(std::stoi(v));
        else if (k == "mass")         mco.rb.mass            = std::stof(v);
        else if (k == "linDamp")      mco.rb.linearDamping   = std::stof(v);
        else if (k == "angDamp")      mco.rb.angularDamping  = std::stof(v);
        else if (k == "friction")     mco.rb.friction        = std::stof(v);
        else if (k == "restitution")  mco.rb.restitution     = std::stof(v);
        else if (k == "gravity")      mco.rb.gravityFactor   = std::stof(v);
    }
    mco.rb.bodyId = kInvalidPhysicsBodyId;
}

} // anonymous namespace

// ==== Resource::SceneText ====
// The line parsers above and SaveScene's N-line format, public, so the offline document codec
// (SceneDocument) reads and writes exactly what LoadScene reads and SaveScene writes.
namespace Resource::SceneText
{
    bool ParseNodeLine(const std::string& line, NodeLine& out)
    {
        if (line.size() < 2 || line[0] != 'N' || line[1] != ' ') return false;
        NodeRecord nr;
        if (!ParseNLine(line, nr)) return false;
        out.idx    = nr.idx;
        out.parent = nr.parent;
        out.t[0] = nr.tx; out.t[1] = nr.ty; out.t[2] = nr.tz;
        out.q[0] = nr.qx; out.q[1] = nr.qy; out.q[2] = nr.qz; out.q[3] = nr.qw;
        out.s[0] = nr.sx; out.s[1] = nr.sy; out.s[2] = nr.sz;
        out.name = nr.name;
        return true;
    }

    std::string FormatNodeLine(const NodeLine& n)
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "N idx=%d parent=%d tx=%.6f ty=%.6f tz=%.6f"
            " qx=%.6f qy=%.6f qz=%.6f qw=%.6f sx=%.6f sy=%.6f sz=%.6f",
            n.idx, n.parent, n.t[0], n.t[1], n.t[2], n.q[0], n.q[1], n.q[2], n.q[3],
            n.s[0], n.s[1], n.s[2]);
        return std::string(buf) + " name=" + PercentEncode(n.name);
    }

    bool ParseComponentLine(const std::string& line, std::string& tag, KVMap& kv)
    {
        ComponentBlock cb;
        if (!::ParseComponentLine(line, -1, cb)) return false;
        tag = std::move(cb.tag);
        kv  = std::move(cb.kv);
        return true;
    }

    std::string EncodeText(const std::string& s) { return PercentEncode(s); }
    std::string DecodeText(const std::string& s) { return PercentDecode(s); }
}

// ==== Cross-frame asset prewarm ====
// Reuses the parse + collect helpers above. Read-only: it kicks the SAME async loads LoadScene
// later kicks (RM dedups by path), so LoadScene's own WaitForHandlesReady returns at once after
// it. Contract: SceneSerializer.h.
namespace Resource
{
    struct ScenePreloadState
    {
        std::vector<Handle> handles;   // RM-backed texture/anim preloads
        std::size_t         total = 0; // == handles.size(); cached for progress
    };

    ScenePreload BeginScenePreload(const std::string& path, AssetManager& assetMgr)
    {
        std::vector<uint8_t> data;
        if (!::Resource::AssetFS::Get().ReadFile(path, data))          return nullptr;
        if (data.size() < sizeof(AssetHeader))                         return nullptr;
        const auto* hdr = reinterpret_cast<const AssetHeader*>(data.data());
        if (hdr->magic != MAGIC_WORLD)                                 return nullptr;

        // Parse ONLY what CollectPreloadPaths / CollectScenePaths need: the 2-space-indented component
        // lines (grouped under their N node). W / MC / FS / MO / ... lines are skipped: MO can carry a t:
        // texture path, but a missed preload only costs a few frames of fallback texture (LoadScene's own
        // pre-load skips MO lines too).
        std::vector<ComponentBlock> compBlocks;
        int currentNodeIdx = -1;
        {
            // Guid-repair here too: the preloader would otherwise kick off
            // async loads for paths LoadScene is about to repair, which is the
            // one place a moved asset still shows up as a hard load failure.
            std::istringstream stream(AssetRefs::ResolveSceneText(
                std::string(GetPayloadText(data.data(), data.size())), path, nullptr));
            std::string line;
            while (std::getline(stream, line))
            {
                if (line.empty() || line[0] == '#') continue;
                if (line[0] == 'N' && line.size() > 1 && line[1] == ' ')
                {
                    NodeRecord nr;
                    if (ParseNLine(line, nr)) currentNodeIdx = nr.idx;
                }
                else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
                {
                    ComponentBlock cb;
                    if (ParseComponentLine(line, currentNodeIdx, cb))
                        compBlocks.push_back(std::move(cb));
                }
            }
        }

        auto state = std::make_shared<ScenePreloadState>();

        // .iscn OS page-cache warmup (fire-and-forget; no RM handles to wait on).
        for (const std::string& scenePath : CollectScenePaths(compBlocks))
            EnqueueIscnRecursiveWarmup(scenePath);

        // RM-backed async loads (textures / anims): what the progress bar tracks.
        if (auto* rm = assetMgr.GetResourceManager())
        {
            const std::vector<PreloadEntry> preloadList = CollectPreloadPaths(compBlocks);
            state->handles.reserve(preloadList.size());
            for (const PreloadEntry& pe : preloadList)
                state->handles.push_back(rm->Load(pe.path, pe.type));
        }
        state->total = state->handles.size();
        return state;
    }

    bool PollScenePreload(const ScenePreload& pre, AssetManager& assetMgr, float& outProgress)
    {
        if (!pre || pre->total == 0) { outProgress = 1.f; return true; }

        auto* rm = assetMgr.GetResourceManager();
        if (!rm) { outProgress = 1.f; return true; }

        // Main-thread GPU-upload pump: workers finish CPU decode off-thread, but the final
        // ID3D12Resource upload happens here (as in the blocking WaitForHandlesReady). A small budget
        // keeps a poll frame snappy.
        rm->ProcessPendingGPUUploads(2.0f);

        std::size_t ready = 0;
        for (Handle h : pre->handles)
            if (rm->GetState(h) != ResourceState::Loading) ++ready; // Ready/Failed both count as done
        outProgress = static_cast<float>(ready) / static_cast<float>(pre->total);
        return ready >= pre->total;
    }
} // namespace Resource

// ==== SaveScene: generic component serialization via ComponentSerializerRegistry ====
bool Resource::SaveScene(World& world, const std::string& path,
                          const std::string& sceneName,
                          const std::string& postProcessConfigPath,
                          const std::string& navMeshPath,
                          const std::string& sceneScript,
                          std::string* errorCode,
                          const std::vector<std::string>& sceneSystems)
{
    Sys::Stage::Scope stage("saving_scene", path);
    auto& reg = GetComponentRegistry();

    std::unordered_map<Entity, int> entityToIdx;
    std::vector<NodeInfo> nodes;
    CollectAllEntities(world, entityToIdx, nodes);

    if (nodes.empty())
    {
        LOG_WARNING("SceneSerializer: world is empty, nothing to save");
        if (errorCode) *errorCode = "empty_world";
        return false;
    }

    // Identify skinned entity subtrees: only the root gets serialized.
    // Children come from the .iscn on load, so skip them to avoid duplicates.
    std::unordered_set<Entity> skinnedSubtree;

    // The reference root each marked entity belongs to, for the MR / MRI / MOI blocks below.
    // `rootEntity` is the OUTERMOST EMITTED root (the one that gets an N line), because `rootIdx` is
    // only addressable on load through that line. `iscnPath` is non-empty only for a plain .iscn
    // placement; a prefab instance leaves it empty, because the .ipfb already persists MaterialRef
    // for its whole subtree (SavePrefab walks it with no skinnedSubtree equivalent) and a second
    // authority in the .iscene would fight it.
    struct RefRootInfo
    {
        Entity      rootEntity = NullEntity;
        int         rootIdx    = -1;
        std::string iscnPath;          // "" => prefab instance
    };
    std::unordered_map<Entity, RefRootInfo> refRootOf;
    {
        // Iterative pre-order walk marking every DESCENDANT of a skinned root (the root itself stays
        // serialized; its children are restored from the .iscn on load). Not recursive, like the node
        // walk in SceneInstanceLoader::Load: a deep bone hierarchy (~70 levels) on top of the editor/ImGui
        // call stack could overflow the 1 MB stack. `skinnedSubtree` doubles as the visited set, so a
        // malformed cyclic Children graph cannot loop forever; marking is membership-only, so traversal
        // order is irrelevant.
        std::vector<Entity> stack;

        // TWO PASSES, and the order is load-bearing. Whichever root's walk reaches a descendant FIRST
        // claims it, and for a WRAPPED prefab the wrapper (PrefabSourcePath) and the inner .iscn root
        // (SceneSourcePath) are two DIFFERENT entities, the inner one created first. In one pass the inner
        // .iscn root could claim the whole character and emit MR rows for a prefab instance, fighting the
        // .ipfb that stores the same bindings. Claiming prefab subtrees first makes the prefab win
        // whatever order `nodes` is in.
        auto markFrom = [&](const NodeInfo& ni, const std::string& iscnPath)
        {
            stack.push_back(ni.entity);   // root itself is not marked, only descendants
            while (!stack.empty())
            {
                const Entity parent = stack.back();
                stack.pop_back();
                const Children* ch = world.GetComponent<Children>(parent);
                if (!ch) continue;
                for (Entity child : ch->entities)
                    if (skinnedSubtree.insert(child).second)  // visited / cycle guard
                    {
                        refRootOf.emplace(child, RefRootInfo{ ni.entity, ni.idx, iscnPath });
                        stack.push_back(child);
                    }
            }
        };

        // Pass 1: prefab instances.
        for (const NodeInfo& ni : nodes)
        {
            const PrefabSourcePath* pfp = world.GetComponent<PrefabSourcePath>(ni.entity);
            if (!pfp || pfp->path.empty()) continue;
            markFrom(ni, std::string());   // "" => the .ipfb owns this subtree
        }

        // Pass 2: plain .iscn placements no prefab claimed. A root that is ITSELF inside a claimed
        // subtree gets no N line, so its idx could not be resolved on load: skip it and let the prefab
        // root stand as the addressable owner.
        for (const NodeInfo& ni : nodes)
        {
            const SceneSourcePath* ssp = world.GetComponent<SceneSourcePath>(ni.entity);
            if (!ssp || ssp->path.empty()) continue;
            if (world.GetComponent<PrefabSourcePath>(ni.entity)) continue;  // unwrapped prefab root: pass 1 claimed it
            if (skinnedSubtree.count(ni.entity)) continue;                  // inside a wrapped prefab
            markFrom(ni, ssp->path);
        }

        // Prefab instances swallow more than their Children: SavePrefab's CollectLinkedFollowers pulls in
        // every entity that FollowSocket / FollowEntity / SkeletonRef-links INTO the subtree (an equipped
        // weapon, its trail, its spark emitter) and stores them as top-level (parent=-1) nodes in the
        // .ipfb. A follow component attaches them, not parenting, so the Children walk above never
        // reaches them; written again as standalone rows, LoadScene would spawn each twice (from the
        // PrefabRef row in step 8b and from its own row in step 6 / 8). LoadPrefab tags them with
        // PrefabInstanceMember: skip them here, plus whatever hangs under them. The tag counts only while
        // the root is alive and still a prefab reference, so DETACHING an instance (removing
        // PrefabSourcePath) turns its followers back into plain scene data.
        if (auto* memberPool = world.GetPool<PrefabInstanceMember>())
        {
            const auto& memberEnts = memberPool->Entities();
            const auto& memberData = memberPool->Data();
            for (size_t i = 0; i < memberEnts.size(); ++i)
            {
                const Entity member = memberEnts[i];
                const Entity owner  = memberData[i].root;
                if (member == owner) continue;                 // root row always survives
                if (owner == NullEntity || !world.IsAlive(owner)) continue;
                const PrefabSourcePath* opf = world.GetComponent<PrefabSourcePath>(owner);
                if (!opf || opf->path.empty()) continue;       // detached -> save normally

                if (!skinnedSubtree.insert(member).second) continue;
                stack.push_back(member);
                while (!stack.empty())
                {
                    const Entity parent = stack.back();
                    stack.pop_back();
                    const Children* ch = world.GetComponent<Children>(parent);
                    if (!ch) continue;
                    for (Entity child : ch->entities)
                        if (skinnedSubtree.insert(child).second)
                            stack.push_back(child);
                }
            }
        }
    }

    // Stamp a GuidComponent on every UIParent target UP FRONT, before any entity row is written.
    // The entity loop runs in creation order (not hierarchy order) and CvParent stamps the parent's
    // GUID only while serializing the CHILD, so a parent flushed earlier (or one whose UIParent was
    // set without AttachUIParent) would never emit its `Guid:` line and the child could not
    // re-resolve its parent on load. Stamping here makes the round trip order-independent.
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

    // Same problem, same fix, for attachment targets: a character written to the
    // file before the weapon that follows it would never emit its own `Guid:`
    // line, and the weapon's targetGuid would dangle on load.
    if (auto* fsPool = world.GetPool<FollowSocketComponent>())
    {
        auto& fsData = fsPool->Data();
        for (size_t i = 0; i < fsData.size(); ++i)
            if (const Entity t = fsData[i].target.Resolve(world); t != NullEntity)
                ECS::EnsureGuidOn(world, t);
    }
    if (auto* fePool = world.GetPool<FollowEntityComponent>())
    {
        auto& feData = fePool->Data();
        for (size_t i = 0; i < feData.size(); ++i)
            if (const Entity t = feData[i].target.Resolve(world); t != NullEntity)
                ECS::EnsureGuidOn(world, t);
    }
    // Reflection-probe proxy targets: a shared ProxyVolume entity written after a
    // probe that refs it would never emit its `Guid:` line -> the probe's
    // proxyGuid would dangle on load. Stamp every resolved proxy up front.
    if (auto* rpPool = world.GetPool<ReflectionProbeComponent>())
    {
        auto& rpData = rpPool->Data();
        for (size_t i = 0; i < rpData.size(); ++i)
            if (const Entity t = rpData[i].proxyVolume.Resolve(world); t != NullEntity)
                ECS::EnsureGuidOn(world, t);
    }

    std::ostringstream ss;
    ss << "# DX12 Engine World Scene\n";
    ss << "W name=" << PercentEncode(sceneName);
    // Serialised asset paths must be project-root-relative (Sys::RelativizeAssetPath). These three
    // arrive straight from editor file dialogs, which hand back an absolute path.
    if (!postProcessConfigPath.empty())
        ss << " postProcessConfig=" << PercentEncode(Sys::RelativizeAssetPath(postProcessConfigPath));
    if (!navMeshPath.empty())
        ss << " navMesh=" << PercentEncode(Sys::RelativizeAssetPath(navMeshPath));
    if (!sceneScript.empty())
        ss << " sceneScript=" << PercentEncode(Sys::RelativizeAssetPath(sceneScript));
    if (!sceneSystems.empty())
    {
        std::string list;
        for (const std::string& s : sceneSystems)
            if (!s.empty()) list += (list.empty() ? "" : ",") + s;
        if (!list.empty()) ss << " systems=" << PercentEncode(list);
    }
    ss << "\n";

    // MC lines embed TWO percent-encoded paths (src= and meshPath=) plus ~20 numeric fields, hence
    // the size: a truncated line loses its '\n', so the next record concatenates onto it and BOTH are
    // misparsed.
    char buf[2048];

    for (const NodeInfo& ni : nodes)
    {
        const Entity e = ni.entity;
        if (skinnedSubtree.count(e)) continue;

        // Prefab-instance ROOT, saved as a PURE REFERENCE: the N line (transform + name + parent) plus
        // the "PrefabRef: path=.." line and the instance's identity rows. Components AND children are
        // re-instantiated from the .ipfb on load, so editing the prefab file propagates to every scene
        // referencing it. (Removing PrefabSourcePath in the inspector DETACHES the instance: it then saves
        // as plain entity data.)
        const PrefabSourcePath* prefabRef = world.GetComponent<PrefabSourcePath>(e);
        const bool prefabPureRef = (prefabRef && !prefabRef->path.empty());

        // ---- N line: transform + name ----
        const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        float tx=0,ty=0,tz=0, qx=0,qy=0,qz=0,qw=1, sx=1,sy=1,sz=1;
        if (lt)
        {
            tx = lt->translation.x; ty = lt->translation.y; tz = lt->translation.z;
            qx = lt->rotation.x;    qy = lt->rotation.y;    qz = lt->rotation.z; qw = lt->rotation.w;
            sx = lt->scale.x;       sy = lt->scale.y;       sz = lt->scale.z;
        }

        // The one N-line formatter (SceneText.h): the offline document codec writes a node exactly as
        // this does.
        {
            SceneText::NodeLine nl;
            nl.idx = ni.idx; nl.parent = ni.parentIdx;
            nl.t[0] = tx; nl.t[1] = ty; nl.t[2] = tz;
            nl.q[0] = qx; nl.q[1] = qy; nl.q[2] = qz; nl.q[3] = qw;
            nl.s[0] = sx; nl.s[1] = sy; nl.s[2] = sz;
            nl.name = world.GetName(e);
            ss << SceneText::FormatNodeLine(nl) << "\n";
        }

        // ---- Pre-step: auto-create the .imat if needed ----
        // Not for prefab roots: their material comes from the .ipfb.
        if (!prefabPureRef &&
            world.HasComponent<MaterialComponent>(e) && !world.HasComponent<MaterialSourcePath>(e))
        {
            const MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
            namespace fs = std::filesystem;
            const fs::path worldDir = Sys::U8Path(path).parent_path();
            const fs::path matDir   = worldDir / "Materials";
            fs::create_directories(matDir);

            std::string entName = world.GetName(e);
            if (entName.empty()) entName = "entity";
            // Keeps a UTF-8 name and replaces only what Win32 and the cook reject (an [A-Za-z0-9_-]
            // allowlist would turn a 4-character CJK name into 12 underscores).
            std::string safeName = Sys::SanitizeFileName(entName);
            if (safeName.empty()) safeName = "entity";
            std::string matFileName = safeName + "_mat" + std::to_string(ni.idx) + ".imat";
            std::string matAbsPath  = Sys::PathToUtf8(matDir / matFileName);

            SaveMaterial(*mat, matAbsPath);

            // matDir derives from the scene's absolute save path, so matAbsPath is
            // absolute. Write the file there, but store the path the way every
            // other asset reference is stored: project-root-relative.
            const std::string matRefPath = Sys::RelativizeAssetPath(matAbsPath);
            if (!world.HasComponent<MaterialSourcePath>(e))
                world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ matRefPath });
            else
                world.GetComponent<MaterialSourcePath>(e)->path = matRefPath;
        }

        // ---- Pre-step 2: an UNSAVED live material edit becomes an override ----
        // The pre-step above only fires for an entity with NO MaterialSourcePath, never the case for a
        // mesh part an .iscn F-line spawned with `mat=`; without this, a tweak to such a part (one
        // roughness change) is silently dropped on save.
        // The diff is recorded as a MaterialOverride row, not a forked .imat: the load side replays it,
        // and a forked file would be named from the DFS ordinal (the least stable key in this format),
        // one file per part.
        // Only for a DIRTY component: ASSET_DIRTY means "differs from the .imat on disk" (not the
        // GPU-upload bit), so a clean part writes nothing and an untouched prefab re-saves byte-identical.
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

        // ---- Generic component serialization via the registry ----
        for (const auto& [typeIdx, serializer] : reg.All())
        {
            // Prefab root: emit ONLY the PrefabRef line (pure reference) PLUS the instance's identity and
            // organization (Guid / Tag / Persistent / EdFolder). Those are per-instance state, not prefab
            // content: without the Guid every reload would mint a fresh one via BindAndStamp and orphan every
            // serialized reference to the instance, and gameplay tags / save-game markers would be stripped.
            // LoadScene step 8b applies these rows as root overrides, and GuidRegistry::RebuildFromWorld at
            // its end makes the saved Guid authoritative.
            if (prefabPureRef
                && serializer.tag != "PrefabRef"
                && serializer.tag != "Guid"
                && serializer.tag != "Tag"
                && serializer.tag != "Persistent"
                && serializer.tag != "EdFolder")
                continue;
            if (serializer.has(world, e))
                serializer.serialize(world, e, ss);
        }
    }

    // ---- MC lines: collider overrides on skinnedSubtree entities ----
    // Everything under a SceneSourcePath is skipped above because the .iscn re-spawns it on load. A
    // ColliderComponent / RigidBodyComponent added in the editor ("Use baked as collision") does not
    // live in the .iscn: it is an override keyed by (sourcePath, sourceMeshId), saved as one MC line
    // per unique key and replayed on load.
    {
        struct MCEntry { ColliderComponent col; RigidBodyComponent rb; bool hasCol=false; bool hasRb=false; };
        std::unordered_map<std::string, MCEntry> mc;
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            if (!skinnedSubtree.count(e)) continue;

            const ColliderComponent*  col = world.GetComponent<ColliderComponent>(e);
            const RigidBodyComponent* rb  = world.GetComponent<RigidBodyComponent>(e);
            if (!col && !rb) continue;

            const MeshLibRef*      mlr = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath*  sp  = world.GetComponent<MeshSourcePath>(e);
            if (!mlr || !sp || sp->path.empty()) continue;

            // What the MODEL gave this part (a C record, its asset collision) is not an override: only a
            // difference from it is written, so a save does not restate every model-given collider and pin
            // its mesh id.
            if (ColliderIsSpawnDefault(world, e)) continue;

            const std::string key = sp->path + '#' + std::to_string(mlr->meshId);
            auto& slot = mc[key];
            if (col) { slot.col = *col; slot.hasCol = true; }
            if (rb)  { slot.rb  = *rb;  slot.hasRb  = true; }
        }
        for (const auto& [key, entry] : mc)
        {
            const size_t hashPos = key.rfind('#');
            const std::string srcPath = key.substr(0, hashPos);
            const uint32_t srcMeshId = static_cast<uint32_t>(std::stoul(key.substr(hashPos + 1)));

            const auto& c = entry.col;
            const auto& r = entry.rb;
            // Every authored field is written: without offset / rotEuler / isTrigger / layer / lockedAxes an
            // imported Unreal box collider or a hand-made trigger volume would come back centred, unrotated,
            // solid and Auto-layer. A line without one of these keys (an older .iscene) leaves that field at
            // its component default.
            // srcSec= (the part) and meshSec= (the collider's mesh) follow the entry each one keys.
            const std::string srcSec  = SectionKeyToken("srcSec", srcPath, srcMeshId);
            std::string meshSec = (entry.hasCol && ColliderHasMesh(c))
                                ? SectionKeyToken("meshSec", c.meshLibPath, c.meshLibMeshId) : std::string();
            // A MeshAsset collider names its Mesh (the model and the MeshKey).
            if (entry.hasCol && c.shape == ColliderComponent::Shape::MeshAsset)
                meshSec += " model=" + PercentEncode(c.assetModel) + " meshKey=" + MeshKeyToHex(c.assetMeshKey);
            snprintf(buf, sizeof(buf),
                "MC src=%s srcMesh=%u%s hasCol=%u hasRb=%u"
                " shape=%u halfExtents=%.4f_%.4f_%.4f radius=%.4f halfHeight=%.4f"
                " offset=%.4f_%.4f_%.4f rotEuler=%.4f_%.4f_%.4f"
                " isTrigger=%u layer=%u"
                " meshPath=%s meshId=%u%s"
                " mt=%u mass=%.4f linDamp=%.4f angDamp=%.4f"
                " friction=%.4f restitution=%.4f gravity=%.4f lockedAxes=%u\n",
                PercentEncode(srcPath).c_str(), srcMeshId, srcSec.c_str(),
                entry.hasCol ? 1u : 0u, entry.hasRb ? 1u : 0u,
                static_cast<uint32_t>(c.shape),
                c.halfExtents.x, c.halfExtents.y, c.halfExtents.z, c.radius, c.halfHeight,
                c.offset.x, c.offset.y, c.offset.z,
                c.rotationEulerDeg.x, c.rotationEulerDeg.y, c.rotationEulerDeg.z,
                c.isTrigger ? 1u : 0u, static_cast<uint32_t>(c.layer),
                PercentEncode(c.meshLibPath).c_str(), c.meshLibMeshId, meshSec.c_str(),
                static_cast<uint32_t>(r.motion),
                r.mass, r.linearDamping, r.angularDamping,
                r.friction, r.restitution, r.gravityFactor,
                static_cast<uint32_t>(r.lockedAxes));
            ss << buf;
        }
    }

    // ---- FS lines: FaceShading overrides on skinnedSubtree entities ----
    // FaceShadingComponent is authored onto the FACE PART entity of a skinned character
    // (FaceShadingComponent.h), a descendant of a SceneSourcePath root that the entity loop skips (the
    // .iscn re-spawns it), so without this the face's head-axis setup is lost on every reload. Saved
    // as an `FS` line keyed by the SAME (MeshSourcePath, MeshLibRef.meshId) identity as MC and
    // replayed after the subtree is rebuilt. FaceShading on a NON-skinned / root entity goes through
    // the generic registry above.
    {
        std::unordered_set<std::string> fsSeen;
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            if (!skinnedSubtree.count(e)) continue;

            const FaceShadingComponent* fc = world.GetComponent<FaceShadingComponent>(e);
            if (!fc) continue;
            const MeshLibRef*     mlr = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath* sp  = world.GetComponent<MeshSourcePath>(e);
            if (!mlr || !sp || sp->path.empty()) continue;

            const std::string key = sp->path + '#' + std::to_string(mlr->meshId);
            if (!fsSeen.insert(key).second) continue;   // dedup multi-instanced characters

            snprintf(buf, sizeof(buf),
                "FS src=%s srcMesh=%u%s headBone=%s forwardAxis=%d rightAxis=%d horizontalize=%u\n",
                PercentEncode(sp->path).c_str(), mlr->meshId,
                SectionKeyToken("srcSec", sp->path, mlr->meshId).c_str(),
                PercentEncode(fc->headBoneName).c_str(),
                fc->forwardAxis, fc->rightAxis, fc->horizontalize ? 1u : 0u);
            ss << buf;
        }
    }

    // ---- MO lines: MaterialOverride on skinnedSubtree entities ----
    // A material tweak authored on a mesh PART of a skinned character (recolour one piece of
    // clothing, unbind one emissive map) lives on a descendant of a SceneSourcePath root, which the
    // entity loop skips; without this it would vanish on reload. Keyed by the SAME (MeshSourcePath,
    // MeshLibRef.meshId) identity as MC / FS and deduplicated for multi-instanced characters. Values
    // use the f:/v:/i:/b:/t: vocabulary ComponentSerializers writes for the inline MaterialOverride
    // row, so AssetReferenceScanner's prefix special-case covers t: entries here too.
    {
        std::unordered_set<std::string> moSeen;
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            if (!skinnedSubtree.count(e)) continue;

            const MaterialOverride* ovr = world.GetComponent<MaterialOverride>(e);
            if (!ovr || ovr->IsEmpty()) continue;
            const MeshLibRef*     mlr = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath* sp  = world.GetComponent<MeshSourcePath>(e);
            if (!mlr || !sp || sp->path.empty()) continue;

            const std::string key = sp->path + '#' + std::to_string(mlr->meshId);
            if (!moSeen.insert(key).second) continue;

            ss << "MO src=" << PercentEncode(sp->path)
               << " srcMesh=" << mlr->meshId << SectionKeyToken("srcSec", sp->path, mlr->meshId);
            for (const auto& [k, val] : ovr->props)
            {
                ss << ' ' << k << '=';
                switch (val.type)
                {
                case MaterialOverride::Value::Float: ss << "f:" << val.data.f; break;
                case MaterialOverride::Value::Vec4:
                    ss << "v:" << val.data.v4[0] << '_' << val.data.v4[1] << '_'
                       << val.data.v4[2] << '_' << val.data.v4[3];
                    break;
                case MaterialOverride::Value::Int:   ss << "i:" << val.data.i; break;
                case MaterialOverride::Value::Bool:  ss << "b:" << (val.data.b ? 1 : 0); break;
                case MaterialOverride::Value::TexRef:ss << "t:" << PercentEncode(val.texPath); break;
                }
            }
            ss << '\n';
        }
    }

    // ---- MR lines: material REF swaps on .iscn mesh parts ----
    // Re-assigning a mesh part to a DIFFERENT .imat changes MaterialSourcePath on a descendant of a
    // SceneSourcePath root, which the entity loop skips before the registry could emit its
    // MaterialRef line; the .iscn F-line `mat=` is the only other record, and nothing writes a .iscn
    // from the live world. Keyed by the SAME (MeshSourcePath, MeshLibRef.meshId) identity as MC / FS /
    // MO. Written after MO; only the REPLAY order matters, and there MR runs before MO (see LoadScene).
    // ONLY A DIFF IS WRITTEN, and not as an optimisation: every mesh part of an .iscn-spawned model
    // carries the MaterialSourcePath SceneInstanceLoader stamped from its F-line, so an unconditional
    // writer would emit one row PER MESH PART on every save (Tokyo.iscn has 714 F-lines). The baseline
    // is the .iscn's own `mat=`, re-read through SceneInstanceLoader::ReadMaterialBindings (so the two
    // never disagree about resolution) and cached per source model: an untouched level saves
    // byte-identical.
    // KNOWN LIMIT (of MC / FS / MO too): (MeshSourcePath, meshId) is not unique in UNREAL-imported
    // levels, where UnrealSceneImporter points several F-lines at one meshlib entry with different
    // materials. ReadMaterialBindings flags such an entry `ambiguous` and this block skips it rather
    // than repainting every variant. For PMX / assimp characters (one F-line per entry) the key is
    // exact.
    {
        // iscnPath -> ("<normalized mesh path>#<entry>" -> normalized baseline .imat path). An entry
        // ReadMaterialBindings flags ambiguous goes into ambiguousKeys instead, and is skipped.
        struct Baseline
        {
            std::unordered_map<std::string, std::string> matByKey;
            std::unordered_set<std::string>              ambiguousKeys;
            bool                                         valid = false;
        };
        std::unordered_map<std::string, Baseline> baselines;

        auto baselineFor = [&](const std::string& iscnPath) -> const Baseline&
        {
            auto it = baselines.find(iscnPath);
            if (it != baselines.end()) return it->second;

            Baseline b;
            std::vector<SceneInstanceLoader::MaterialBinding> rows;
            b.valid = SceneInstanceLoader::ReadMaterialBindings(iscnPath, rows);
            for (const auto& r : rows)
            {
                // Canonical, as the replay compares (sameSource): the live path and this one are separator- and
                // case-spelled independently. The entry is the one the F-line's key names (what the loader
                // spawned), not its mesh= hint.
                KeyResolveStats unused;
                const uint32_t entry = EntryForKey(r.meshAbsPath, r.meshId, r.sectionKey, unused);
                const std::string key = AssetRefs::NormalizeRefPath(r.meshAbsPath) + '#' + std::to_string(entry);
                if (r.ambiguous) b.ambiguousKeys.insert(key);
                else             b.matByKey.emplace(key, AssetRefs::NormalizeRefPath(r.matPath));
            }
            return baselines.emplace(iscnPath, std::move(b)).first->second;
        };

        std::unordered_set<std::string> mrSeen;
        uint32_t mrWritten = 0;
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            const auto rootIt = refRootOf.find(e);
            if (rootIt == refRootOf.end()) continue;
            // Empty iscnPath => a prefab instance; the .ipfb owns those bindings.
            if (rootIt->second.iscnPath.empty()) continue;

            const MaterialSourcePath* msp = world.GetComponent<MaterialSourcePath>(e);
            if (!msp || msp->path.empty()) continue;
            const MeshLibRef*     mlr = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath* sp  = world.GetComponent<MeshSourcePath>(e);
            if (!mlr || !sp || sp->path.empty()) continue;

            const std::string key = AssetRefs::NormalizeRefPath(sp->path) + '#' + std::to_string(mlr->meshId);

            const Baseline& base = baselineFor(rootIt->second.iscnPath);
            // An unreadable .iscn means we have no baseline to diff against.
            // Writing everything would bloat the file; writing nothing would
            // lose the user's edit. Losing the edit is the recoverable failure,
            // and the read failure is already logged by ReadMaterialBindings.
            if (!base.valid) continue;
            if (base.ambiguousKeys.count(key)) continue;

            const std::string cur = Sys::RelativizeAssetPath(msp->path);
            const auto baseIt = base.matByKey.find(key);
            if (baseIt != base.matByKey.end() && baseIt->second == AssetRefs::NormalizeRefPath(cur))
                continue;   // still the model's own default: nothing to record

            if (!mrSeen.insert(key).second) continue;   // multi-instanced character

            ss << "MR src=" << PercentEncode(sp->path)
               << " srcMesh=" << mlr->meshId << SectionKeyToken("srcSec", sp->path, mlr->meshId)
               // PercentEncode, NOT PercentEncodeStrict. The cook's path scanner (tools/package_game.py)
               // percent-decodes %XX PER BYTE, which turns a strict-encoded non-ASCII path into mojibake that
               // resolve() cannot find, and --strict then ABORTS packaging (237 shipped .imat live under
               // non-ASCII directories). Weak encoding escapes only '%' and ' ', all the whitespace tokenizer
               // needs, and the rule MC / FS / MO use for src=.
               << " mat=" << PercentEncode(cur) << '\n';
            ++mrWritten;
        }
        if (mrWritten)
            LOG_INFO("SceneSerializer: wrote %u material-ref swap(s) as MR lines", mrWritten);
    }


    // ---- MRI / MOI lines: PER-PLACEMENT material overrides ----
    // MR / MO are per-ASSET: keyed by (MeshSourcePath, meshId) and replayed with a world-wide ForEach,
    // so every copy of one model in a scene gets the same value and the writer's dedup DROPS the
    // divergent ones (Unity's prefab/asset default layer). MRI / MOI add the per-placement key: the
    // reference root's own N-line idx, the addressing CPR uses and the only per-placement identity in
    // this format that survives a round trip (an .iscn mesh part gets no Guid and is never written to
    // the .iscene).
    //
    //   MRI root=<N idx> src=<pct MeshSourcePath> srcMesh=<meshId> mat=<pct>
    //   MOI root=<N idx> src=<pct> srcMesh=<meshId> <key>=<f:|v:|i:|b:|t:>...
    //
    // `root` is the OUTERMOST EMITTED ref root: for a wrapped prefab the wrapper, not the inner .iscn
    // root (that one is inside the wrapper's subtree, gets no N line, and its idx would not resolve).
    // Covers PREFAB instances as well as plain .iscn placements without fighting the .ipfb: the
    // baseline is MaterialSpawnDefault, which LoadPrefab re-stamps AFTER applying the prefab's own
    // rows, so nothing the .ipfb authored is a diff.
    // ONLY A DIFF IS WRITTEN: every mesh part carries a MaterialSourcePath from spawn, so an
    // unconditional writer would add one row per part per save (714 for Tokyo).
    {
        uint32_t mriWritten = 0, moiWritten = 0;
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            const auto rootIt = refRootOf.find(e);
            if (rootIt == refRootOf.end()) continue;
            if (rootIt->second.rootIdx < 0) continue;

            const MeshLibRef*     mlr = world.GetComponent<MeshLibRef>(e);
            const MeshSourcePath* sp  = world.GetComponent<MeshSourcePath>(e);
            if (!mlr || !sp || sp->path.empty()) continue;

            // No baseline means the loader never said what it gave this part: treat it as "unchanged"
            // rather than restating every binding.
            const MaterialSpawnDefault* def = world.GetComponent<MaterialSpawnDefault>(e);
            if (!def) continue;

            const std::string head = "root=" + std::to_string(rootIt->second.rootIdx)
                                   + " src=" + PercentEncode(sp->path)
                                   + " srcMesh=" + std::to_string(mlr->meshId)
                                   + SectionKeyToken("srcSec", sp->path, mlr->meshId);

            // ---- MRI: the material REFERENCE differs from the as-spawned one ----
            const MaterialSourcePath* msp = world.GetComponent<MaterialSourcePath>(e);
            if (msp && !msp->path.empty())
            {
                const std::string cur = Sys::RelativizeAssetPath(msp->path);
                if (cur != Sys::RelativizeAssetPath(def->path))
                {
                    // Weak-encoded for the same reason MR above is: see there.
                    ss << "MRI " << head << " mat=" << PercentEncode(cur) << '\n';
                    ++mriWritten;
                }
            }

            // ---- MOI: the override BAG differs from the as-spawned one ----
            const MaterialOverride* ovr = world.GetComponent<MaterialOverride>(e);
            const bool hasNow  = ovr && !ovr->IsEmpty();
            const bool hadThen = !def->ovr.IsEmpty();
            bool bagDiffers = (hasNow != hadThen);
            if (!bagDiffers && hasNow)
            {
                if (ovr->props.size() != def->ovr.props.size()) bagDiffers = true;
                else
                    for (const auto& [k, v] : ovr->props)
                    {
                        const auto it = def->ovr.props.find(k);
                        if (it == def->ovr.props.end()) { bagDiffers = true; break; }
                        const auto& o = it->second;
                        if (o.type != v.type) { bagDiffers = true; break; }
                        using V = MaterialOverride::Value;
                        bool same = true;
                        switch (v.type)
                        {
                        case V::Float:  same = (o.data.f == v.data.f); break;
                        case V::Int:    same = (o.data.i == v.data.i); break;
                        case V::Bool:   same = (o.data.b == v.data.b); break;
                        case V::TexRef: same = (o.texPath == v.texPath); break;
                        case V::Vec4:
                            for (int q = 0; q < 4; ++q)
                                if (o.data.v4[q] != v.data.v4[q]) same = false;
                            break;
                        }
                        if (!same) { bagDiffers = true; break; }
                    }
            }
            // An emptied bag still has to be recorded, or a removed override would silently come back on
            // reload: the line then carries no value tokens, and the replay reads that as "clear".
            if (bagDiffers)
            {
                ss << "MOI " << head;
                if (hasNow)
                {
                    for (const auto& [k, val] : ovr->props)
                    {
                        ss << ' ' << k << '=';
                        switch (val.type)
                        {
                        case MaterialOverride::Value::Float: ss << "f:" << val.data.f; break;
                        case MaterialOverride::Value::Vec4:
                            ss << "v:" << val.data.v4[0] << '_' << val.data.v4[1] << '_'
                               << val.data.v4[2] << '_' << val.data.v4[3];
                            break;
                        case MaterialOverride::Value::Int:   ss << "i:" << val.data.i; break;
                        case MaterialOverride::Value::Bool:  ss << "b:" << (val.data.b ? 1 : 0); break;
                        case MaterialOverride::Value::TexRef:ss << "t:" << PercentEncode(val.texPath); break;
                        }
                    }
                }
                ss << '\n';
                ++moiWritten;
            }
        }
        if (mriWritten || moiWritten)
            LOG_INFO("SceneSerializer: wrote %u per-placement material ref swap(s) and "
                     "%u per-placement override bag(s)", mriWritten, moiWritten);
    }

    // ---- CPR lines: authored rest-pose overrides on prefab instances ----
    // A prefab-instance root is saved as a PURE reference (the entity loop skips every serializer but
    // PrefabRef / Guid / Tag / Persistent / EdFolder), so a tail curl authored on an INSTANCE would
    // vanish on reload. A dedicated line, NOT "ChainPhysics" in that allowlist: allowlisting would
    // bake the instance's ENTIRE ChainPhysicsComponent (shipped prefabs carry 45-64 groups) into the
    // .iscene as a root override, immunising the instance against every later .ipfb edit with no UI
    // indication. Per instance only the rest POSE is wanted, not the physics material.
    // Scope: CPR covers the prefab-instance ROOT only; prefab DESCENDANTS are in skinnedSubtree and
    // never visited. ChainPhysicsComponent lives on the character root (with SkeletonComponent): do
    // not move it onto a mesh child without giving CPR the FS block's (MeshSourcePath, meshId) keying,
    // or this line goes silently dead.
    {
        for (const NodeInfo& ni : nodes)
        {
            const Entity e = ni.entity;
            // Mirror the pure-reference test used above; a DETACHED instance
            // (PrefabSourcePath removed) saves its component normally and must
            // not also get a CPR line, or the two would fight on load.
            const PrefabSourcePath* pfp = world.GetComponent<PrefabSourcePath>(e);
            if (!pfp || pfp->path.empty()) continue;
            const ChainPhysicsComponent* cp = world.GetComponent<ChainPhysicsComponent>(e);
            if (!cp) continue;
            if (cp->restPose.empty() && cp->restPoseEnabled && cp->restPoseWeight == 1.f)
                continue;   // nothing authored -> no line, so existing scenes stay byte-identical

            // KNOWN LIMITATION: the gate above compares against STRUCT DEFAULTS, not the prefab's own
            // values (SaveScene has no cheap way to read the .ipfb's ChainPhysicsComponent). Two narrow
            // consequences:
            //   * If a PREFAB ships a rest pose, every instance re-saves it as an override, and later edits
            //     to the .ipfb's pose stop reaching already-saved scenes (the physics material still
            //     propagates; only the pose is pinned). Detach or re-save the scene to pick the new pose up.
            //   * "This instance has NO pose while the prefab does" is not expressible by clearing the list
            //     (empty == default == no line): untick "Enabled" instead (rpEn=0 is non-default, so it
            //     emits a line and disables the inherited pose).
            // A written line is fully authoritative (rp=0 included), so an instance with any other
            // non-default field can also clear its list.

            ss << "CPR idx=" << ni.idx
               << " rpEn=" << (cp->restPoseEnabled ? 1 : 0)
               << " rpW="  << cp->restPoseWeight
               << " rp="   << cp->restPose.size();
            char qb[96];
            for (size_t i = 0; i < cp->restPose.size(); ++i)
            {
                const auto& r = cp->restPose[i];
                snprintf(qb, sizeof(qb), "%.7g_%.7g_%.7g_%.7g",
                         r.rot.x, r.rot.y, r.rot.z, r.rot.w);
                // PercentEncodeStrict, matching the component serializer's own rN_b encoding: a bone name is
                // authored text, not a path.
                ss << " r" << i << "_b=" << PercentEncodeStrict(r.boneName)
                   << " r" << i << "_q=" << qb
                   << " r" << i << "_w=" << r.weight
                   << " r" << i << "_e=" << (r.enabled ? 1 : 0);
            }
            ss << "\n";
        }
    }

    // Stamp each reference with its target's guid so a later rename or move outside the editor is
    // still recoverable (Resource/AssetRefs.h).
    const std::string text = AssetRefs::AugmentSceneText(ss.str());
    const uint32_t textLen = static_cast<uint32_t>(text.size());
    const uint32_t payloadSz = textLen + 1;

    struct WorldMeta { uint32_t nodeCount; uint32_t textLength; uint32_t _pad[2]; };
    WorldMeta meta{ static_cast<uint32_t>(nodes.size()), textLen, {0, 0} };

    AssetHeader hdr{};
    hdr.magic        = MAGIC_WORLD;
    hdr.version      = CurrentVersion(MAGIC_WORLD);
    hdr.resourceType = 0;
    hdr.metadataSize = sizeof(meta);
    hdr.dataSize     = payloadSz;

    auto f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("SceneSerializer: cannot write '%s'", path.c_str());
        if (errorCode) *errorCode = "open_failed";
        return false;
    }
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLen);
    f.put('\0');
    // A full disk or a vanished share fails HERE, not at open: check it, or "saved" is logged for a
    // truncated file.
    f.flush();
    if (!f)
    {
        LOG_ERROR("SceneSerializer: writing '%s' failed part-way — the file is incomplete",
                  path.c_str());
        if (errorCode) *errorCode = "write_failed";
        return false;
    }

    LOG_SUCCESS("SceneSerializer: saved '%s' — %u entities, %u bytes",
                path.c_str(), static_cast<uint32_t>(nodes.size()), textLen);
    return true;
}

// ==== LoadScene: generic component deserialization via ComponentSerializerRegistry ====
bool Resource::LoadScene(const std::string& path, World& world, AssetManager& assetMgr,
                          Renderer* renderer, AnimationClipSystem* animClipSys,
                          std::string* outSceneName,
                          std::string* outPostProcessConfigPath,
                          std::string* outNavMeshPath,
                          std::string* outSceneScript,
                          std::vector<std::string>* outSceneSystems)
{
    // What the frame loop is blocked on, for the control pipe's `status`.
    Sys::Stage::Scope stage("loading_scene", path);
    auto& reg = GetComponentRegistry();

    // 1. Read the file (pak or disk, via AssetFS).
    std::vector<uint8_t> data;
    if (!::Resource::AssetFS::Get().ReadFile(path, data))
    { LOG_ERROR("SceneSerializer: cannot open '%s'", path.c_str()); return false; }

    const auto sz = data.size();
    if (sz < sizeof(AssetHeader))
    { LOG_ERROR("SceneSerializer: file too small"); return false; }

    if (const std::string why = HeaderRefusal(data.data(), sz, MAGIC_WORLD); !why.empty())
    { LOG_ERROR("SceneSerializer: refusing '%s': %s", path.c_str(), why.c_str()); return false; }

    // 2. Parse the text line by line BEFORE world.Clear(), so resource paths can be pre-scanned and
    //    their async loads kicked off while the old world still renders.
    std::string sceneName            = "Untitled";
    std::string postProcessConfigPath;
    std::string navMeshPath;
    std::string sceneScript;
    std::vector<std::string>    sceneSystems;
    std::vector<NodeRecord>     nodes;
    std::vector<ComponentBlock> compBlocks;
    int currentNodeIdx = -1;

    // Mesh-collider overrides for .iscn-spawned entities (SaveScene says why they exist). One MC
    // line per unique (sourcePath, sourceMeshId); applied AFTER SceneInstanceLoader::Load re-spawns
    // the scene tree.
    std::vector<MeshColliderOverride> mcOverrides;

    // FaceShading overrides for skinned-character face parts (FS lines). Mirrors
    // mcOverrides: keyed by (sourcePath, sourceMeshId), replayed after the .iscn
    // subtree is reconstructed (the face entity is re-spawned, not in the N list).
    struct FaceShadeOverride
    {
        std::string          sourcePath;
        uint32_t             sourceMeshId = 0;
        uint64_t             sourceSec    = 0;
        FaceShadingComponent fsc;
    };
    std::vector<FaceShadeOverride> fsOverrides;

    // MO lines: a MaterialOverride bag authored on a skinned-subtree mesh part,
    // keyed the same way MC/FS are. See the writer for why it needs its own line.
    struct MatOverrideRow
    {
        std::string      sourcePath;
        uint32_t         sourceMeshId = 0;
        uint64_t         sourceSec    = 0;
        MaterialOverride ovr;
    };
    std::vector<MatOverrideRow> moOverrides;

    // MR: a material-REF swap on a .iscn mesh part, keyed exactly like MO.
    // Distinct from MO: MO carries VALUES to layer on top of whatever material
    // the part already has, MR names a DIFFERENT .imat to load in its place.
    struct MatRefRow
    {
        std::string sourcePath;
        uint32_t    sourceMeshId = 0;
        uint64_t    sourceSec    = 0;
        std::string matPath;
    };
    std::vector<MatRefRow> mrOverrides;

    // MRI / MOI: the same payloads keyed additionally by the reference root's N-line idx, i.e. per
    // PLACEMENT rather than per mesh asset.
    struct MatRefInstRow
    {
        int         rootIdx      = -1;
        std::string sourcePath;
        uint32_t    sourceMeshId = 0;
        uint64_t    sourceSec    = 0;
        std::string matPath;
    };
    struct MatOvrInstRow
    {
        int              rootIdx      = -1;
        std::string      sourcePath;
        uint32_t         sourceMeshId = 0;
        uint64_t         sourceSec    = 0;
        MaterialOverride ovr;          // empty = the placement CLEARED its bag
    };
    std::vector<MatRefInstRow> mriOverrides;
    std::vector<MatOvrInstRow> moiOverrides;

    // Authored rest-pose overrides on prefab-instance ROOTS (CPR lines). See the
    // SaveScene block for why this is a dedicated line and not an allowlist entry.
    struct ChainRestPoseOverride
    {
        int                       nodeIdx = -1;
        bool                      enabled = true;
        float                     weight  = 1.f;
        std::vector<BoneRestPose> pose;
    };
    std::vector<ChainRestPoseOverride> cprOverrides;

    // Repair references whose target moved, using the guid each one carries
    // (Resource/AssetRefs.h). Path-first: a healthy scene costs one existence
    // check per reference and nothing more, and the `$g_` tokens are stripped
    // here so not one line handler below has to know this layer exists.
    std::size_t refsRepaired = 0;
    const std::string sceneText = AssetRefs::ResolveSceneText(
        std::string(GetPayloadText(data.data(), data.size())), path, &refsRepaired);
    if (refsRepaired)
        LOG_WARNING("SceneSerializer: '%s' — %zu reference(s) repaired by guid. "
                    "Re-save the scene to make the repair permanent.",
                    path.c_str(), refsRepaired);

    std::istringstream stream(sceneText);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == 'W')
        {
            // W name=<scene-name> [postProcessConfig=<path>] [navMesh=<path>] [sceneScript=<path>]
            //   [systems=<name>,<name>...]
            std::istringstream ls(line.substr(2));
            std::string token;
            while (ls >> token)
            {
                auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = token.substr(0, eq);
                const std::string val = token.substr(eq + 1);
                // Relativize on READ too: an older scene may store absolute paths, which no pak key matches.
                if      (key == "name")              sceneName             = PercentDecode(val);
                else if (key == "postProcessConfig") postProcessConfigPath = Sys::RelativizeAssetPath(PercentDecode(val));
                else if (key == "navMesh")           navMeshPath           = Sys::RelativizeAssetPath(PercentDecode(val));
                else if (key == "sceneScript")       sceneScript           = Sys::RelativizeAssetPath(PercentDecode(val));
                else if (key == "systems")
                {
                    std::istringstream list(PercentDecode(val));
                    std::string s;
                    while (std::getline(list, s, ','))
                        if (!s.empty()) sceneSystems.push_back(s);
                }
            }
        }
        else if (line[0] == 'N' && line.size() > 1 && line[1] == ' ')
        {
            NodeRecord nr;
            if (ParseNLine(line, nr))
            {
                currentNodeIdx = nr.idx;
                nodes.push_back(std::move(nr));
            }
        }
        else if (line.size() > 3 && line[0] == 'M' && line[1] == 'C' && line[2] == ' ')
        {
            MeshColliderOverride mco;
            ParseMCLine(line, mco);
            mcOverrides.push_back(std::move(mco));
        }
        else if (line.size() > 3 && line[0] == 'F' && line[1] == 'S' && line[2] == ' ')
        {
            // FS src=<path> srcMesh=N headBone=<name> forwardAxis=i rightAxis=i horizontalize=0|1
            FaceShadeOverride fso;
            std::istringstream ls(line.substr(3));
            std::string token;
            while (ls >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = token.substr(0, eq);
                const std::string v = token.substr(eq + 1);
                if      (k == "src")           fso.sourcePath          = PercentDecode(v);
                else if (k == "srcMesh")       fso.sourceMeshId        = static_cast<uint32_t>(std::stoul(v));
                else if (k == "srcSec")        fso.sourceSec           = ParseSectionKey(v);
                else if (k == "headBone")      fso.fsc.headBoneName    = PercentDecode(v);
                else if (k == "forwardAxis")   fso.fsc.forwardAxis     = std::stoi(v);
                else if (k == "rightAxis")     fso.fsc.rightAxis       = std::stoi(v);
                else if (k == "horizontalize") fso.fsc.horizontalize   = (std::stoi(v) != 0);
            }
            fsOverrides.push_back(std::move(fso));
        }
        else if (line.size() > 4 && line.compare(0, 4, "MRI ") == 0)
        {
            // MRI root=N src=<path> srcMesh=N mat=<path>
            MatRefInstRow row;
            std::istringstream ls(line.substr(4));
            std::string token;
            while (ls >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = token.substr(0, eq);
                const std::string v = token.substr(eq + 1);
                if      (k == "src") row.sourcePath = PercentDecode(v);
                else if (k == "mat") row.matPath    = PercentDecode(v);
                else if (k == "root")    { try { row.rootIdx      = std::stoi(v); }            catch (...) {} }
                else if (k == "srcMesh") { try { row.sourceMeshId = static_cast<uint32_t>(std::stoul(v)); } catch (...) {} }
                else if (k == "srcSec")  row.sourceSec = ParseSectionKey(v);
            }
            if (row.rootIdx >= 0 && !row.sourcePath.empty() && !row.matPath.empty())
                mriOverrides.push_back(std::move(row));
        }
        else if (line.size() > 4 && line.compare(0, 4, "MOI ") == 0)
        {
            // MOI root=N src=<path> srcMesh=N [<key>=<typed> ...]
            // No value tokens at all is meaningful: the placement CLEARED its bag.
            MatOvrInstRow row;
            std::istringstream ls(line.substr(4));
            std::string token;
            while (ls >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = token.substr(0, eq);
                const std::string v = token.substr(eq + 1);
                if      (k == "src")     { row.sourcePath = PercentDecode(v); continue; }
                else if (k == "root")    { try { row.rootIdx = std::stoi(v); } catch (...) {} continue; }
                else if (k == "srcMesh") { try { row.sourceMeshId = static_cast<uint32_t>(std::stoul(v)); } catch (...) {} continue; }
                else if (k == "srcSec")  { row.sourceSec = ParseSectionKey(v); continue; }

                if (v.size() < 2 || v[1] != ':') continue;
                using V = MaterialOverride::Value;
                const std::string valStr = v.substr(2);
                try
                {
                    switch (v[0])
                    {
                    case 'f': row.ovr.Set(k, V::MakeFloat(std::stof(valStr))); break;
                    case 'v': {
                        float x=0,y=0,z=0,w=0;
                        sscanf_s(valStr.c_str(), "%f_%f_%f_%f", &x,&y,&z,&w);
                        row.ovr.Set(k, V::MakeVec4(x,y,z,w)); } break;
                    case 'i': row.ovr.Set(k, V::MakeInt(std::stoi(valStr))); break;
                    case 'b': row.ovr.Set(k, V::MakeBool(valStr != "0")); break;
                    case 't': row.ovr.Set(k, V::MakeTex(PercentDecode(valStr))); break;
                    default: break;
                    }
                }
                catch (...) { /* one malformed value must not lose the whole row */ }
            }
            if (row.rootIdx >= 0 && !row.sourcePath.empty())
                moiOverrides.push_back(std::move(row));
        }
        else if (line.size() > 3 && line[0] == 'M' && line[1] == 'R' && line[2] == ' ')
        {
            // MR src=<path> srcMesh=N mat=<path>
            MatRefRow row;
            std::istringstream ls(line.substr(3));
            std::string token;
            while (ls >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = token.substr(0, eq);
                const std::string v = token.substr(eq + 1);
                if      (k == "src")     row.sourcePath = PercentDecode(v);
                else if (k == "mat")     row.matPath    = PercentDecode(v);
                else if (k == "srcMesh")
                {
                    try { row.sourceMeshId = static_cast<uint32_t>(std::stoul(v)); }
                    catch (...) { }
                }
                else if (k == "srcSec")  row.sourceSec = ParseSectionKey(v);
            }
            if (!row.sourcePath.empty() && !row.matPath.empty())
                mrOverrides.push_back(std::move(row));
        }
        else if (line.size() > 3 && line[0] == 'M' && line[1] == 'O' && line[2] == ' ')
        {
            // MO src=<path> srcMesh=N <key>=<f:|v:|i:|b:|t:><value> ...
            MatOverrideRow row;
            std::istringstream ls(line.substr(3));
            std::string token;
            while (ls >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = token.substr(0, eq);
                const std::string v = token.substr(eq + 1);
                if      (k == "src")     { row.sourcePath   = PercentDecode(v); continue; }
                else if (k == "srcMesh") { row.sourceMeshId = static_cast<uint32_t>(std::stoul(v)); continue; }
                else if (k == "srcSec")  { row.sourceSec    = ParseSectionKey(v); continue; }

                if (v.size() < 2 || v[1] != ':') continue;
                using V = MaterialOverride::Value;
                const std::string valStr = v.substr(2);
                try
                {
                    switch (v[0])
                    {
                    case 'f': row.ovr.Set(k, V::MakeFloat(std::stof(valStr))); break;
                    case 'v': {
                        float x=0,y=0,z=0,w=0;
                        sscanf_s(valStr.c_str(), "%f_%f_%f_%f", &x,&y,&z,&w);
                        row.ovr.Set(k, V::MakeVec4(x,y,z,w)); } break;
                    case 'i': row.ovr.Set(k, V::MakeInt(std::stoi(valStr))); break;
                    case 'b': row.ovr.Set(k, V::MakeBool(valStr != "0")); break;
                    case 't': row.ovr.Set(k, V::MakeTex(PercentDecode(valStr))); break;
                    default: break;
                    }
                }
                catch (...) { /* one malformed value must not lose the whole row */ }
            }
            if (!row.ovr.IsEmpty()) moOverrides.push_back(std::move(row));
        }
        else if (line.size() > 4 && line.compare(0, 4, "CPR ") == 0)
        {
            // CPR idx=N rpEn=0|1 rpW=f rp=N r0_b=<name> r0_q=x_y_z_w r0_w=f r0_e=0|1 ...
            ChainRestPoseOverride cpr;
            std::unordered_map<std::string, std::string> kv;
            {
                std::istringstream ls(line.substr(4));
                std::string token;
                while (ls >> token)
                {
                    const auto eq = token.find('=');
                    if (eq == std::string::npos) continue;
                    kv[token.substr(0, eq)] = token.substr(eq + 1);
                }
            }
            auto getS = [&](const std::string& k) -> std::string {
                auto it = kv.find(k); return it == kv.end() ? std::string() : it->second;
            };
            auto getI = [&](const std::string& k, int def) -> int {
                auto it = kv.find(k); if (it == kv.end()) return def;
                try { return std::stoi(it->second); } catch (...) { return def; }
            };
            auto getF = [&](const std::string& k, float def) -> float {
                auto it = kv.find(k); if (it == kv.end()) return def;
                try { return std::stof(it->second); } catch (...) { return def; }
            };

            cpr.nodeIdx = getI("idx", -1);
            cpr.enabled = getI("rpEn", 1) != 0;
            cpr.weight  = getF("rpW",  1.f);
            if (!std::isfinite(cpr.weight)) cpr.weight = 1.f;
            cpr.weight  = (cpr.weight < 0.f) ? 0.f : (cpr.weight > 1.f ? 1.f : cpr.weight);
            int n = getI("rp", 0);
            if (n < 0) n = 0;
            if (n > ChainPhysicsComponent::MAX_REST_POSE_BONES)
            {
                LOG_WARNING("SceneSerializer: CPR rest-pose list truncated %d -> %d bones",
                            n, ChainPhysicsComponent::MAX_REST_POSE_BONES);
                n = ChainPhysicsComponent::MAX_REST_POSE_BONES;
            }
            cpr.pose.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
            {
                BoneRestPose& r = cpr.pose[i];
                const std::string pre = "r" + std::to_string(i) + "_";
                const std::string nm = PercentDecode(getS(pre + "b"));
                snprintf(r.boneName, sizeof(r.boneName), "%s", nm.c_str());

                const std::string q = getS(pre + "q");
                float c[4] = { 0.f, 0.f, 0.f, 1.f };
                int   qi = 0, parsed = 0; size_t p = 0;
                while (qi < 4 && p <= q.size())
                {
                    const size_t u = q.find('_', p);
                    const std::string tok = q.substr(p, u == std::string::npos ? std::string::npos : u - p);
                    if (!tok.empty()) { try { c[qi] = std::stof(tok); ++parsed; } catch (...) { c[qi] = (qi == 3) ? 1.f : 0.f; } }
                    ++qi;
                    if (u == std::string::npos) break;
                    p = u + 1;
                }
                // %.7g emits inf/nan for a poisoned value and those tokens pass
                // the tokenizer untouched; normalizing one produces NaN, which
                // ends up in the skin matrix and AVs the BVH build.
                const bool finite = std::isfinite(c[0]) && std::isfinite(c[1])
                                 && std::isfinite(c[2]) && std::isfinite(c[3]);
                const float len2 = c[0]*c[0] + c[1]*c[1] + c[2]*c[2] + c[3]*c[3];
                // parsed != 4 => truncated value; fall back to identity rather
                // than keeping a half-written quaternion.
                if (parsed != 4 || !finite || len2 < 1e-12f) r.rot = { 0.f, 0.f, 0.f, 1.f };
                else
                {
                    const float inv = 1.f / std::sqrt(len2);
                    r.rot = { c[0]*inv, c[1]*inv, c[2]*inv, c[3]*inv };
                }
                // NOT 0 (see the component reader), and finite+clamped: a NaN
                // weight becomes a NaN pose via XMQuaternionSlerp.
                r.weight  = getF(pre + "w", 1.f);
                if (!std::isfinite(r.weight)) r.weight = 1.f;
                r.weight  = (r.weight < 0.f) ? 0.f : (r.weight > 1.f ? 1.f : r.weight);
                r.enabled = getI(pre + "e", 1) != 0;
            }
            if (cpr.nodeIdx >= 0) cprOverrides.push_back(std::move(cpr));
        }
        else if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            ComponentBlock cb;
            if (ParseComponentLine(line, currentNodeIdx, cb))
                compBlocks.push_back(std::move(cb));
        }
    }

    if (outSceneName) *outSceneName = sceneName;

    // 3. Pre-load loader-backed resources BEFORE Clear(): workers run async file I/O while the main
    //    thread pumps GPU uploads, so step 6 finds these paths in RM's path-hash cache instead of
    //    reading the disk (except a clip the old world also used: see step 4).
    //    SceneRef (.iscn) paths have no registered IResourceLoader, so a separate fire-and-forget
    //    pass pulls their bytes into the OS page cache before SceneInstanceLoader's sync read in
    //    step 8: the biggest win for SceneRef-heavy worlds (Bistro); the MaterialOverride / AnimRef
    //    path covers VFX / cutscene-style worlds.
    {
        auto* rm = assetMgr.GetResourceManager();

        // 3a. Recursive page-cache warmup for .iscn, fire-and-forget (not waited on). The outer worker
        //     reads the .iscn, parses F/K/M lines and pushes child warmups for every sibling (.meshlib,
        //     .imat, .iskel, .imorph), so SceneInstanceLoader's sync reads in step 8 hit a warm kernel
        //     page cache instead of cold disk seeks.
        const std::vector<std::string> scenePaths = CollectScenePaths(compBlocks);
        for (const std::string& scenePath : scenePaths)
            EnqueueIscnRecursiveWarmup(scenePath);

        // 3b. RM-backed pre-load for loader-known extensions (textures, anims).
        if (rm)
        {
            const std::vector<PreloadEntry> preloadList = CollectPreloadPaths(compBlocks);
            std::vector<Handle> preloadHandles;
            preloadHandles.reserve(preloadList.size());
            for (const PreloadEntry& pe : preloadList)
                preloadHandles.push_back(rm->Load(pe.path, pe.type));

            if (!preloadHandles.empty() || !scenePaths.empty())
            {
                LOG_INFO("SceneSerializer: pre-loading %zu RM resources + %zu .iscn warmups",
                         preloadHandles.size(), scenePaths.size());
            }
            if (!preloadHandles.empty())
                WaitForHandlesReady(*rm, preloadHandles);
            // preloadHandles drop here. An RM Handle is a POD that holds no ref: a slot stays alive until
            // something Unloads it, and the refcount owners (TextureSystem / AnimationClipSystem) Acquire
            // them in steps 6/7 and inherit the warm cache.
        }
    }

    // 4. Clear the old world. Animation handles are not RAII values, so release
    //    authored bindings and in-flight ECS handles before their components
    //    disappear. A clip whose AnimationClipSystem refcount reaches 0 here is RM-Unloaded at once,
    //    so a clip both worlds use loads again instead of reusing its step-3b slot. A shared texture
    //    stays warm: TextureSystem defers its RM Unload to Tick, which keeps a path re-Acquired by then.
    if (animClipSys) animClipSys->OnWorldClear(world);
    world.Clear();
    // Stale Guid -> Entity mappings die with the old world; step 8.4 rebuilds them from the new
    // GuidComponents.
    ECS::GuidRegistry::Get().Clear();

    // 5. Pre-collect skinned node indices (SceneRef components)
    struct SkinInfo { int nodeIdx = -1; std::string scenePath, animPath; };
    std::unordered_map<int, SkinInfo> skinInfoMap;
    std::unordered_set<int> skinnedIndices;

    // 5b. Prefab-instance rows (PrefabRef): like the skinned rows they get NO plain entity in step
    // 6; step 8b re-instantiates the .ipfb and maps the row idx onto the spawned prefab root.
    std::unordered_map<int, std::string> prefabInfoMap;
    std::unordered_set<int> prefabIndices;

    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
            {
                skinInfoMap[cb.nodeIdx].nodeIdx = cb.nodeIdx;
                skinInfoMap[cb.nodeIdx].scenePath = PercentDecode(it->second);
                skinnedIndices.insert(cb.nodeIdx);
            }
        }
        else if (cb.tag == "AnimRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
            {
                skinInfoMap[cb.nodeIdx].nodeIdx = cb.nodeIdx;
                skinInfoMap[cb.nodeIdx].animPath = PercentDecode(it->second);
            }
        }
        else if (cb.tag == "PrefabRef")
        {
            auto it = cb.kv.find("path");
            if (it != cb.kv.end())
            {
                prefabInfoMap[cb.nodeIdx] = PercentDecode(it->second);
                prefabIndices.insert(cb.nodeIdx);
            }
        }
    }

    // 6. Create non-skinned entities (skinned roots are loaded from .iscn in
    // step 8; prefab-instance roots from their .ipfb in step 8b)
    std::unordered_map<int, Entity> entityMap;
    for (const auto& nr : nodes)
    {
        if (skinnedIndices.count(nr.idx) || prefabIndices.count(nr.idx)) continue;

        Entity e = world.CreateEntity();
        world.SetName(e, nr.name.empty() ? "Entity" : nr.name);
        entityMap[nr.idx] = e;

        LocalTransform lt;
        lt.translation = { nr.tx, nr.ty, nr.tz };
        lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
        lt.scale       = { nr.sx, nr.sy, nr.sz };
        world.AddComponent<LocalTransform>(e, lt);
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
        world.AddComponent<RenderLayer>(e, RenderLayer{});
        world.AddComponent<Children>(e, Children{});

        if (nr.parent >= 0)
        {
            auto it = entityMap.find(nr.parent);
            if (it != entityMap.end())
            {
                world.AddComponent<Parent>(e, Parent{ it->second });
                if (auto* ch = world.GetComponent<Children>(it->second))
                    ch->entities.push_back(e);
            }
        }
    }

    // 7. Apply component blocks for non-skinned entities via the registry.
    //
    // SetLegacyEntityMap installs the saved-idx -> new-Entity remap so AttachmentRef fields saved as
    // raw entity ids (PlayerComponent.cameraEntity, CameraControllerComponent.followTarget,
    // FollowCamera / AimCamera.target) resolve to live entities and BindAndStamp a GUID on the
    // target: the next save is GUID-only.
    ::Resource::SetLegacyEntityMap(&entityMap);

    for (const auto& cb : compBlocks)
    {
        if (cb.tag == "SceneRef" || cb.tag == "AnimRef") continue;
        if (skinnedIndices.count(cb.nodeIdx)) continue;

        auto it = entityMap.find(cb.nodeIdx);
        if (it == entityMap.end()) continue;

        ApplyComponentLine(reg, world, it->second, cb.tag, cb.kv, &assetMgr);
    }

    // Add default material to mesh entities that don't have one
    for (const auto& [idx, e] : entityMap)
    {
        if (world.HasComponent<MeshLibRef>(e) && !world.HasComponent<MaterialComponent>(e))
            world.AddComponent<MaterialComponent>(e, MaterialComponent{});
    }

    // 8. Skinned character reconstruction from SceneRef / AnimRef.
    //
    // One BatchScope for the WHOLE loop: a populated world re-instantiates the same .iscn many times
    // (AIBench = 100x Rossi), and each would re-read and re-decompose byte-identical .meshlib /
    // .imorph payloads. Scoped, not process-wide, so a later load after a re-import decodes fresh
    // (SceneInstanceLoader::BatchScope). The graphics device is passed so ONE buffer-upload batch
    // covers all N characters; Load()'s own batch would drain the graphics queue once per character
    // (13-15 ms each).
    SceneInstanceLoader::BatchScope _skinLoadBatch(assetMgr.GetGraphicsDevice());

    std::size_t charactersDone = 0;
    for (const auto& [idx, info] : skinInfoMap)
    {
        Sys::Stage::Progress(charactersDone++, skinInfoMap.size(), "characters");
        if (info.scenePath.empty()) continue;

        MeshLibrary* meshLib = renderer ? renderer->GetMeshLibrary() : nullptr;
        if (!meshLib)
        {
            LOG_ERROR("SceneSerializer: MeshLibrary not wired — cannot load skinned scene '%s'",
                      info.scenePath.c_str());
            continue;
        }
        SceneInstanceLoader::LoadResult res =
            SceneInstanceLoader::Load(info.scenePath, world, assetMgr, *meshLib, renderer);

        if (res.success && res.rootEntity != NullEntity)
        {
            entityMap[idx] = res.rootEntity;

            // Restore name and LocalTransform from N line
            for (const auto& nr : nodes)
            {
                if (nr.idx != idx) continue;
                if (!nr.name.empty())
                    world.SetName(res.rootEntity, nr.name);

                LocalTransform lt;
                lt.translation = { nr.tx, nr.ty, nr.tz };
                lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
                lt.scale       = { nr.sx, nr.sy, nr.sz };
                if (auto* existing = world.GetComponent<LocalTransform>(res.rootEntity))
                    *existing = lt;
                else
                    world.AddComponent<LocalTransform>(res.rootEntity, lt);
                break;
            }

            // Store source paths
            world.AddComponent<SceneSourcePath>(res.rootEntity, SceneSourcePath{ info.scenePath });

            // Apply component overrides on the skinned root (LightData, Billboard, etc.)
            for (const auto& cb : compBlocks)
            {
                if (cb.nodeIdx != idx) continue;
                if (cb.tag == "SceneRef" || cb.tag == "AnimRef") continue;
                ApplyComponentLine(reg, world, res.rootEntity, cb.tag, cb.kv, &assetMgr);
            }

            // AnimRef (one clip path) becomes the authored AnimationSet's Default clip. AnimationClipSystem
            // owns async binding and queues Default playback without storing a handle in ECS.
            if (!info.animPath.empty() && animClipSys)
            {
                auto* set = world.GetComponent<AnimationSetComponent>(res.rootEntity);
                if (!set)
                {
                    world.AddComponent<AnimationSetComponent>(
                        res.rootEntity, AnimationSetComponent{});
                    set = world.GetComponent<AnimationSetComponent>(res.rootEntity);
                }
                if (!set->Find("Default"))
                    set->Set("Default", info.animPath, true, 1.f);
                if (set->defaultClip.empty()) set->defaultClip = "Default";
                set->playDefaultOnLoad = true;
                animClipSys->QueueAnimatorPlay(
                    world, res.rootEntity, set->defaultClip, 0.f, true);
                world.RemoveComponent<AnimationSourcePath>(res.rootEntity);
            }

            LOG_INFO("SceneSerializer: loaded skinned entity idx=%d scene='%s' anim='%s'",
                     idx, info.scenePath.c_str(), info.animPath.c_str());
        }
        else
        {
            LOG_ERROR("SceneSerializer: failed to load .iscn '%s'", info.scenePath.c_str());
        }
    }

    // 8b. Prefab-instance reconstruction from PrefabRef, the mirror of step 8: the row's entity IS
    // the freshly instantiated prefab root; the N line restores the per-instance name/transform;
    // extra component blocks apply as root overrides. Step 8.5 wires the parent links.
    std::size_t prefabsDone = 0;
    for (const auto& [idx, prefabPath] : prefabInfoMap)
    {
        Sys::Stage::Progress(prefabsDone++, prefabInfoMap.size(), "prefabs");
        // Seed the instance's identity from the Guid this row was saved with, so every node of the
        // instance gets the SAME per-instance Guid as last session (LoadPrefab derives the rest from it).
        // Unseeded, the members would be re-randomised on every load and any ref to a prefab MEMBER (a
        // VFX socketed onto the instance's weapon, a camera targeting it) would dangle after one round
        // trip. A row without a Guid gets a random seed, like a fresh drag-drop instantiation.
        ECS::Guid seed = ECS::kInvalidGuid;
        for (const auto& cb : compBlocks)
            if (cb.nodeIdx == idx && cb.tag == "Guid")
            {
                if (auto it = cb.kv.find("hex"); it != cb.kv.end())
                    seed = ECS::Guid::FromHex(it->second);
                break;
            }

        Entity root = LoadPrefab(prefabPath, world, assetMgr, renderer, animClipSys,
                                 seed.IsValid() ? &seed : nullptr);
        if (root == NullEntity)
        {
            LOG_ERROR("SceneSerializer: failed to instantiate prefab '%s' (idx=%d)",
                      prefabPath.c_str(), idx);
            continue;
        }
        entityMap[idx] = root;

        // Restore per-instance name + LocalTransform from the N line.
        for (const auto& nr : nodes)
        {
            if (nr.idx != idx) continue;
            if (!nr.name.empty())
                world.SetName(root, nr.name);
            LocalTransform lt;
            lt.translation = { nr.tx, nr.ty, nr.tz };
            lt.rotation    = { nr.qx, nr.qy, nr.qz, nr.qw };
            lt.scale       = { nr.sx, nr.sy, nr.sz };
            if (auto* existing = world.GetComponent<LocalTransform>(root))
                *existing = lt;
            else
                world.AddComponent<LocalTransform>(root, lt);
            break;
        }

        // LoadPrefab tags PrefabSourcePath itself; guard for robustness.
        if (!world.HasComponent<PrefabSourcePath>(root))
            world.AddComponent<PrefabSourcePath>(root, PrefabSourcePath{ prefabPath });

        // Extra component blocks (the Guid / Tag / Persistent / EdFolder rows SaveScene writes) apply as
        // root overrides.
        for (const auto& cb : compBlocks)
        {
            if (cb.nodeIdx != idx) continue;
            if (cb.tag == "PrefabRef") continue;
            ApplyComponentLine(reg, world, root, cb.tag, cb.kv, &assetMgr);
        }

        // Per-instance authored rest pose (CPR), applied AFTER the root-override replay above: LoadPrefab
        // has already given the root its ChainPhysicsComponent from the .ipfb, and only the pose fields
        // are patched. The physics material stays owned by the prefab, which is the point of not
        // allowlisting the component.
        for (const auto& cpr : cprOverrides)
        {
            if (cpr.nodeIdx != idx) continue;
            ChainPhysicsComponent* cp = world.GetComponent<ChainPhysicsComponent>(root);
            if (!cp)
            {
                LOG_WARNING("SceneSerializer: CPR override for prefab idx=%d has no "
                            "ChainPhysicsComponent on the instance root - skipped", idx);
                break;
            }
            cp->restPose        = cpr.pose;
            cp->restPoseEnabled = cpr.enabled;
            cp->restPoseWeight  = cpr.weight;
            break;
        }

        LOG_INFO("SceneSerializer: instantiated prefab idx=%d '%s' root=%u",
                 idx, prefabPath.c_str(), root);
    }

    // Post-process: apply MaterialOverride to MaterialComponent, AFTER steps 8 and 8b. Both create
    // or REPLACE MaterialComponents (step 8 rebuilds a skinned subtree from its .iscn, step 8b
    // re-instantiates a prefab, and the default-material pass in between adds one where there was
    // none), so a bag applied earlier would land on a component that is then overwritten, or be
    // skipped because the component does not exist yet.
    // Steps 8/8b also replay overrides onto their own roots; applying twice is harmless: the bag
    // assigns values, it does not accumulate.
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

    // 8.4: tear down the saved-idx remap installed in step 7, then rebuild GuidRegistry from the
    // world so every loaded GuidComponent has a live Guid -> Entity mapping for AttachmentRef
    // resolution, including GUIDs just stamped onto remap targets in steps 7/8.
    ::Resource::SetLegacyEntityMap(nullptr);
    ECS::GuidRegistry::Get().RebuildFromWorld(world);

    // 8.5: re-wire Parent / Children for every node from the saved parentIdx. Step 6 wired the
    // non-skinned entities but could not see the skinned (.iscn) ones, created in step 8. This pass
    // also reconciles the reverse case: an .iscn root reparented under a regular entity in the
    // editor. Idempotent: skips a node whose Parent already matches.
    for (const auto& nr : nodes)
    {
        if (nr.parent < 0) continue;
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

    LOG_SUCCESS("SceneSerializer: loaded '%s' — %zu entities, scene='%s'",
                path.c_str(), nodes.size(), sceneName.c_str());

    // 9. Apply the referenced post-processing config (if any, and a Renderer is available). A
    //    missing file is non-fatal: the AO / fog switches take the file's defaults (below); the
    //    other values keep whatever state the passes were in before the load.
    if (outPostProcessConfigPath) *outPostProcessConfigPath = postProcessConfigPath;
    if (outNavMeshPath)            *outNavMeshPath           = navMeshPath;
    if (outSceneScript)            *outSceneScript           = sceneScript;
    if (outSceneSystems)           *outSceneSystems          = sceneSystems;
    bool appliedConfig = false;
    if (!postProcessConfigPath.empty() && renderer)
    {
        PostProcessConfig cfg;
        if (LoadPostProcessConfig(postProcessConfigPath, cfg))
        {
            cfg.ApplyTo(*renderer, postProcessConfigPath);
            appliedConfig = true;
            LOG_INFO("SceneSerializer: applied post-process config '%s'",
                     postProcessConfigPath.c_str());
        }
    }
    // A scene with no (readable) .ippc states that file's defaults for its AO and fog switches,
    // instead of inheriting the previous scene's.
    if (!appliedConfig && renderer)
        ApplyNoConfigSceneIntents(*renderer);

    // A part's MeshSourcePath against a saved row's `src`, both canonical
    // (AssetRefs::NormalizeRefPath: '/', lower case). The .iscn loader joins the model's folder and
    // file with the platform separator ('\'), while the file can hold '/' (the editor's asset move
    // rewrites every reference that way, AssetReferenceScanner::RewriteReferences), so an exact
    // compare would silently miss EVERY row of such a model in the MC / FS / MR / MRI / MO / MOI
    // blocks below.
    auto sameSource = [](const MeshSourcePath* sp, const std::string& rowNorm)
    { return sp && AssetRefs::NormalizeRefPath(sp->path) == rowNorm; };

    // The entry each row names: its srcSec= key's when the library has it (a re-import may have
    // moved it), else srcMesh=. A spawned part's MeshLibRef.meshId is the entry the .iscn loader
    // resolved (key first too).
    KeyResolveStats keyStats;

    // Apply the MC lines to every .iscn-spawned mesh entity whose (sourcePath, sourceMeshId)
    // matches. Done last, so the SceneInstanceLoader path has fully populated the world;
    // PhysicsSystem's auto-prewarm picks the new Mesh ColliderComponents up on its next Update, no
    // manual Prewarm needed. Matched canonically (sameSource) like the blocks below: an exact compare
    // would drop every authored collider (a level's "Use baked as collision" lines) the first time an
    // editor move re-spells its path with '/'.
    if (!mcOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& mco : mcOverrides)
        {
            const std::string rowNorm = AssetRefs::NormalizeRefPath(mco.sourcePath);
            const uint32_t    want    = EntryForKey(mco.sourcePath, mco.sourceMeshId, mco.sourceSec, keyStats);
            ColliderComponent collider = mco.collider;
            if (mco.hasCollider && mco.meshSec != 0)
                collider.meshLibMeshId = EntryForKey(collider.meshLibPath, collider.meshLibMeshId, mco.meshSec, keyStats);
            world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
            {
                if (ref.meshId != want) return;
                if (!sameSource(world.GetComponent<MeshSourcePath>(e), rowNorm)) return;

                if (mco.hasCollider)
                {
                    if (world.HasComponent<ColliderComponent>(e))
                        *world.GetComponent<ColliderComponent>(e) = collider;
                    else
                        world.AddComponent<ColliderComponent>(e, collider);
                }
                if (mco.hasRb)
                {
                    if (world.HasComponent<RigidBodyComponent>(e))
                    {
                        auto* rb = world.GetComponent<RigidBodyComponent>(e);
                        *rb = mco.rb;
                        rb->bodyId = kInvalidPhysicsBodyId; // force re-create on next physics tick
                    }
                    else
                    {
                        world.AddComponent<RigidBodyComponent>(e, mco.rb);
                    }
                }
                ++applied;
            });
        }
        LOG_INFO("SceneSerializer: applied %u Mesh collider overrides from %zu MC lines",
                 applied, mcOverrides.size());
    }

    // FaceShading overrides: re-attach FaceShadingComponent onto the face part the .iscn just
    // re-spawned, matched by (sourcePath, meshId) like MC. Without it the head-axis driver is missing
    // after reload and the SDF face shadow / nose-spec gate go unwired.
    if (!fsOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& fso : fsOverrides)
        {
            const std::string rowNorm = AssetRefs::NormalizeRefPath(fso.sourcePath);
            const uint32_t    want    = EntryForKey(fso.sourcePath, fso.sourceMeshId, fso.sourceSec, keyStats);
            world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
            {
                if (ref.meshId != want) return;
                if (!sameSource(world.GetComponent<MeshSourcePath>(e), rowNorm)) return;

                FaceShadingComponent fsc = fso.fsc;
                fsc.cachedBoneIdx = ~0u;   // runtime field: force a re-resolve from headBoneName
                if (world.HasComponent<FaceShadingComponent>(e))
                    *world.GetComponent<FaceShadingComponent>(e) = fsc;
                else
                    world.AddComponent<FaceShadingComponent>(e, fsc);
                ++applied;
            });
        }
        LOG_INFO("SceneSerializer: applied %u FaceShading overrides from %zu FS lines",
                 applied, fsOverrides.size());
    }

    // Material REF swaps: re-point the part at the .imat the user assigned, replacing the material
    // the .iscn's own F-line just loaded.
    // MUST RUN BEFORE THE MO BLOCK BELOW: LoadMaterial overwrites the whole MaterialComponent, while
    // MO is a value bag that has to land ON TOP of the final material. Reversed, every per-part tweak
    // is silently reverted by the material load that follows it.
    if (!mrOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& row : mrOverrides)
        {
            const std::string rowNorm = AssetRefs::NormalizeRefPath(row.sourcePath);
            const uint32_t    want    = EntryForKey(row.sourcePath, row.sourceMeshId, row.sourceSec, keyStats);
            world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
            {
                if (ref.meshId != want) return;
                if (!sameSource(world.GetComponent<MeshSourcePath>(e), rowNorm)) return;

                MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
                if (!mat) return;

                // LEADING "::" IS REQUIRED, and this file uses it everywhere for
                // the same reason: Resource/Resource.h declares `class Resource`
                // INSIDE `namespace Resource`, so inside a `Resource::`-qualified
                // function body the name `Resource` finds the CLASS first and
                // `Resource::MaterialLoadInfo` fails to name a type.
                ::Resource::MaterialLoadInfo info;
                if (!::Resource::LoadMaterial(row.matPath, *mat, &info))
                {
                    LOG_WARNING("SceneSerializer: MR line names '%s', which failed to load — "
                                "part keeps the model's default material", row.matPath.c_str());
                    return;
                }
                // Contract at HierarchyComponents.h: every path that loads a
                // material into an entity must publish its load info, or a later
                // re-save has no override set to write and guesses one.
                ApplyMaterialLoadInfo(world, e, info);

                if (auto* msp = world.GetComponent<MaterialSourcePath>(e))
                    msp->path = row.matPath;
                else
                    world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ row.matPath });

                mat->SetDirty();
                ++applied;
            });
        }
        LOG_INFO("SceneSerializer: applied %u material-ref swaps from %zu MR lines",
                 applied, mrOverrides.size());
    }


    // Per-placement material REF swaps. Scoped to ONE reference root's subtree,
    // which is the whole point: MR above matches (MeshSourcePath, meshId) across
    // the entire world, so it repaints every copy of the model; MRI addresses one
    // placement through the root's own N-line idx and leaves its siblings alone.
    //
    // Runs AFTER MR so a placement that overrides the per-asset default wins, and
    // BEFORE the MO/MOI value bags so those layer on top of the final material.
    if (!mriOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& row : mriOverrides)
        {
            const auto rootIt = entityMap.find(row.rootIdx);
            if (rootIt == entityMap.end() || !world.IsAlive(rootIt->second))
            {
                LOG_WARNING("SceneSerializer: MRI names root idx=%d, which no longer "
                            "resolves — per-placement material skipped", row.rootIdx);
                continue;
            }

            // Iterative subtree walk: a character is ~70 levels deep and this runs
            // on the editor/ImGui stack, which is why nothing here recurses.
            const std::string rowNorm = AssetRefs::NormalizeRefPath(row.sourcePath);
            const uint32_t    want    = EntryForKey(row.sourcePath, row.sourceMeshId, row.sourceSec, keyStats);
            std::vector<Entity> stack{ rootIt->second };
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (const Children* ch = world.GetComponent<Children>(e))
                    for (Entity kid : ch->entities) stack.push_back(kid);

                const MeshLibRef* ref = world.GetComponent<MeshLibRef>(e);
                if (!ref || ref->meshId != want) continue;
                if (!sameSource(world.GetComponent<MeshSourcePath>(e), rowNorm)) continue;

                MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
                if (!mat) continue;

                ::Resource::MaterialLoadInfo info;
                if (!::Resource::LoadMaterial(row.matPath, *mat, &info))
                {
                    LOG_WARNING("SceneSerializer: MRI names '%s', which failed to load — "
                                "part keeps its default material", row.matPath.c_str());
                    continue;
                }
                ApplyMaterialLoadInfo(world, e, info);
                if (auto* msp = world.GetComponent<MaterialSourcePath>(e))
                    msp->path = row.matPath;
                else
                    world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ row.matPath });
                mat->SetDirty();
                ++applied;
            }
        }
        LOG_INFO("SceneSerializer: applied %u per-placement material swaps from %zu MRI lines",
                 applied, mriOverrides.size());
    }

    // MaterialOverride on skinned mesh parts: re-attach the bag onto the entity the .iscn just
    // re-spawned, then apply it. Matched by (sourcePath, meshId) like MC / FS. Runs AFTER the generic
    // post-process above, so a part that also carries a scene-level override ends with both,
    // most-specific last.
    if (!moOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& row : moOverrides)
        {
            const std::string rowNorm = AssetRefs::NormalizeRefPath(row.sourcePath);
            const uint32_t    want    = EntryForKey(row.sourcePath, row.sourceMeshId, row.sourceSec, keyStats);
            world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
            {
                if (ref.meshId != want) return;
                if (!sameSource(world.GetComponent<MeshSourcePath>(e), rowNorm)) return;

                if (auto* existing = world.GetComponent<MaterialOverride>(e))
                    *existing = row.ovr;
                else
                    world.AddComponent<MaterialOverride>(e, row.ovr);

                if (auto* mat = world.GetComponent<MaterialComponent>(e))
                {
                    ApplyMaterialOverride(*mat, row.ovr);
                    mat->SetDirty();
                }
                ++applied;
            });
        }
        LOG_INFO("SceneSerializer: applied %u material overrides from %zu MO lines",
                 applied, moOverrides.size());
    }

    // Per-placement override BAGS. Last of the four, so the most specific layer has the final word:
    // MR (asset ref) -> MRI (placement ref) -> MO (asset values) -> MOI (placement values). An MOI
    // row with no value tokens is not malformed: it records a placement that CLEARED its bag, and
    // must remove whatever MO just applied, or a deleted override would silently return.
    if (!moiOverrides.empty())
    {
        uint32_t applied = 0;
        for (const auto& row : moiOverrides)
        {
            const auto rootIt = entityMap.find(row.rootIdx);
            if (rootIt == entityMap.end() || !world.IsAlive(rootIt->second)) continue;

            const std::string rowNorm = AssetRefs::NormalizeRefPath(row.sourcePath);
            const uint32_t    want    = EntryForKey(row.sourcePath, row.sourceMeshId, row.sourceSec, keyStats);
            std::vector<Entity> stack{ rootIt->second };
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (const Children* ch = world.GetComponent<Children>(e))
                    for (Entity kid : ch->entities) stack.push_back(kid);

                const MeshLibRef* ref = world.GetComponent<MeshLibRef>(e);
                if (!ref || ref->meshId != want) continue;
                if (!sameSource(world.GetComponent<MeshSourcePath>(e), rowNorm)) continue;

                if (row.ovr.IsEmpty())
                {
                    // Cleared. Drop the bag AND re-load the material from its file,
                    // because MO may already have layered values onto it.
                    if (world.GetComponent<MaterialOverride>(e))
                        world.RemoveComponent<MaterialOverride>(e);
                    if (auto* mat = world.GetComponent<MaterialComponent>(e))
                    {
                        const MaterialSourcePath* msp = world.GetComponent<MaterialSourcePath>(e);
                        if (msp && !msp->path.empty())
                        {
                            ::Resource::MaterialLoadInfo info;
                            if (::Resource::LoadMaterial(msp->path, *mat, &info))
                                ApplyMaterialLoadInfo(world, e, info);
                        }
                        mat->SetDirty();
                    }
                }
                else
                {
                    if (auto* existing = world.GetComponent<MaterialOverride>(e))
                        *existing = row.ovr;
                    else
                        world.AddComponent<MaterialOverride>(e, row.ovr);
                    if (auto* mat = world.GetComponent<MaterialComponent>(e))
                    {
                        ApplyMaterialOverride(*mat, row.ovr);
                        mat->SetDirty();
                    }
                }
                ++applied;
            }
        }
        LOG_INFO("SceneSerializer: applied %u per-placement override bags from %zu MOI lines",
                 applied, moiOverrides.size());
    }

    if (keyStats.moved > 0 || keyStats.missing > 0)
        LOG_WARNING("SceneSerializer: '%s' - %u override line(s) followed their section key (srcSec= / "
                    "meshSec=) to another entry than their srcMesh= / meshId= hint (a model was re-imported; "
                    "re-saving the scene refreshes the hints), %u key(s) the library does not have (the "
                    "hint was used)", path.c_str(), keyStats.moved, keyStats.missing);

    return true;
}

// ==== StampSceneKeys (contract in the header) ====
namespace
{
    // [value start, value end) of the " <key>=" token at @p at (the value runs
    // to the next space).
    std::pair<size_t, size_t> TokenSpan(const std::string& line, size_t at, size_t keyLen)
    {
        const size_t s = at + keyLen;
        const size_t e = line.find(' ', s);
        return { s, e == std::string::npos ? line.size() : e };
    }

    // " <key>=" in @p line, from @p from; npos when absent.
    size_t FindToken(const std::string& line, const char* key, size_t from = 0)
    {
        return line.find(std::string(" ") + key + "=", from);
    }

    // One (library, entry-hint, key) triple of a line: add, check or refresh.
    //   libAt   - " <libKey>=" (the value is percent-encoded)
    //   entryAt - " <entryKey>=" (a decimal entry index)
    void StampOne(std::string& line, const char* libKey, const char* entryKey, const char* secKey,
                  Resource::SceneKeyReport::Counts& n)
    {
        const size_t libAt   = FindToken(line, libKey);
        const size_t entryAt = FindToken(line, entryKey);
        if (libAt == std::string::npos || entryAt == std::string::npos) return;
        const auto [ls, le] = TokenSpan(line, libAt, std::strlen(libKey) + 2);
        const std::string lib = Resource::AssetRefs::PercentDecode(line.substr(ls, le - ls));
        if (lib.empty()) return;
        ++n.lines;
        const auto [es, ee] = TokenSpan(line, entryAt, std::strlen(entryKey) + 2);
        uint32_t entry = 0;
        try { entry = static_cast<uint32_t>(std::stoul(line.substr(es, ee - es))); }
        catch (...) { ++n.unresolved; return; }
        const auto table = Resource::MeshLibKeyTableAt(lib);
        const size_t secAt = FindToken(line, secKey);
        if (secAt != std::string::npos)
        {
            const auto [ss, se] = TokenSpan(line, secAt, std::strlen(secKey) + 2);
            uint64_t key = 0;
            if (!table || !Resource::MeshKeyFromHex(line.substr(ss, se - ss), key)) { ++n.unresolved; return; }
            const int at = table->Find(key);
            if (at < 0) { ++n.dangling; return; }
            if (static_cast<uint32_t>(at) == entry) { ++n.kept; return; }
            line.replace(es, ee - es, std::to_string(at));   // the hint, from the key
            ++n.hintFixed;
            return;
        }
        const uint64_t key = table ? table->KeyOf(entry) : 0;
        if (key == 0) { ++n.unresolved; return; }
        line.insert(ee, std::string(" ") + secKey + "=" + Resource::MeshKeyToHex(key));
        ++n.added;
    }

    int ShapeOf(const std::string& line)
    {
        const size_t at = FindToken(line, "shape");
        if (at == std::string::npos) return -1;
        const auto [s, e] = TokenSpan(line, at, 7);
        try { return std::stoi(line.substr(s, e - s)); } catch (...) { return -1; }
    }

    bool MeshShape(int shape)
    {
        return shape == static_cast<int>(ColliderComponent::Shape::Mesh) ||
               shape == static_cast<int>(ColliderComponent::Shape::ConvexHull);
    }

    // A scene (.iscene) or prefab (.ipfb) container read for a text edit: the
    // bytes, the header, where the text starts and the text (to its NUL).
    struct SceneContainer
    {
        std::vector<uint8_t> blob;
        Resource::AssetHeader hdr{};
        size_t               payload = 0;
        std::string          text;
    };
    bool ReadSceneContainer(const std::string& path, SceneContainer& c, std::string& why)
    {
        using namespace Resource;
        if (!Sys::ReadFileBytes(path, c.blob) || c.blob.size() < sizeof(AssetHeader) + 8)
            return why = "cannot read '" + path + "'", false;
        std::memcpy(&c.hdr, c.blob.data(), sizeof(c.hdr));
        if (c.hdr.magic != MAGIC_WORLD && c.hdr.magic != MAGIC_PREFAB)
            return why = "'" + path + "' is not a scene (.iscene) or prefab (.ipfb)", false;
        if (c.hdr.metadataSize < 8 || sizeof(AssetHeader) + size_t(c.hdr.metadataSize) > c.blob.size())
            return why = "'" + path + "': truncated metadata", false;
        c.payload = sizeof(AssetHeader) + c.hdr.metadataSize;
        size_t n = (std::min)(static_cast<size_t>(c.hdr.dataSize), c.blob.size() - c.payload);
        if (const void* nul = std::memchr(c.blob.data() + c.payload, 0, n))
            n = static_cast<size_t>(static_cast<const uint8_t*>(nul) - (c.blob.data() + c.payload));
        c.text.assign(reinterpret_cast<const char*>(c.blob.data() + c.payload), n);
        return true;
    }
    // The container as the writer made it: header + metadata (nodeCount kept,
    // textLength at +4 - WorldMeta and PrefabMetadata both), the text, a NUL.
    bool WriteSceneContainer(const std::string& path, const SceneContainer& c, const std::string& out)
    {
        using namespace Resource;
        std::vector<uint8_t> next(c.payload + out.size() + 1, 0);
        std::memcpy(next.data(), c.blob.data(), c.payload);
        AssetHeader hdr = c.hdr;
        hdr.dataSize = static_cast<uint32_t>(out.size() + 1);
        std::memcpy(next.data(), &hdr, sizeof(hdr));
        const uint32_t textLen = static_cast<uint32_t>(out.size());
        std::memcpy(next.data() + sizeof(AssetHeader) + 4, &textLen, sizeof(textLen));
        std::memcpy(next.data() + c.payload, out.data(), out.size());
        return Sys::WriteFileBytesAtomic(path, next.data(), next.size());
    }
}

bool Resource::StampSceneKeys(const std::string& path, bool apply, SceneKeyReport& rep, std::string* outError)
{
    rep = SceneKeyReport{};
    auto fail = [&](std::string why)
    {
        if (outError) *outError = why;
        return false;
    };
    SceneContainer sc;
    std::string    why;
    if (!ReadSceneContainer(path, sc, why)) return fail(why);
    const AssetHeader& hdr  = sc.hdr;
    const std::string& text = sc.text;

    // A prefab's model parts: node idx -> the key of the model's placement at its
    // DFS ordinal (SceneRef node idx + dfsIndex, as LoadPrefab maps them).
    std::unordered_map<int, uint64_t> partKeyOfNode;
    if (hdr.magic == MAGIC_PREFAB)
    {
        int node = -1;
        size_t p = 0;
        while (p < text.size())
        {
            size_t q = text.find('\n', p);
            if (q == std::string::npos) q = text.size();
            const std::string line = text.substr(p, q - p);
            p = q + 1;
            if (line.rfind("N ", 0) == 0)
            {
                const size_t at = FindToken(line, "idx");
                node = -1;
                if (at != std::string::npos)
                    try { node = std::stoi(line.substr(TokenSpan(line, at, 5).first)); } catch (...) {}
                continue;
            }
            const size_t tag = line.find_first_not_of(' ');
            if (node < 0 || tag == std::string::npos || line.compare(tag, 10, "SceneRef: ") != 0) continue;
            const size_t at = FindToken(line, "path");
            if (at == std::string::npos) continue;
            const auto [s, e] = TokenSpan(line, at, 6);
            std::vector<SceneInstanceLoader::MeshPlacement> placements;
            if (!SceneInstanceLoader::ReadMeshPlacements(AssetRefs::PercentDecode(line.substr(s, e - s)), placements))
                continue;
            for (const auto& mp : placements)
                if (const uint64_t k = SceneInstanceLoader::PlacementSectionKey(mp))
                    partKeyOfNode[node + static_cast<int>(mp.dfsIndex)] = k;
        }
    }

    std::string out;
    out.reserve(text.size() + 64);
    size_t b = 0;
    while (b < text.size())
    {
        size_t e = text.find('\n', b);
        const bool last = (e == std::string::npos);
        if (last) e = text.size();
        std::string line = text.substr(b, e - b);
        std::string cr;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); cr = "\r"; }

        const bool mc  = line.rfind("MC ", 0) == 0;
        const bool row = mc || line.rfind("FS ", 0) == 0 || line.rfind("MO ", 0) == 0 ||
                         line.rfind("MR ", 0) == 0 || line.rfind("MRI ", 0) == 0 || line.rfind("MOI ", 0) == 0;
        if (row) StampOne(line, "src", "srcMesh", "srcSec", rep.part);
        // A collider's mesh: the MC line's own, or an inline `Collider:` component.
        const size_t tagEnd = line.find_first_not_of(' ');
        const bool inlineCollider = tagEnd != std::string::npos && line.compare(tagEnd, 10, "Collider: ") == 0;
        if ((mc || inlineCollider) && MeshShape(ShapeOf(line)))
            StampOne(line, "meshPath", "meshId", "meshSec", rep.collider);
        if (!partKeyOfNode.empty() && line.rfind("N ", 0) == 0)
        {
            int idx = -1;
            if (const size_t at = FindToken(line, "idx"); at != std::string::npos)
                try { idx = std::stoi(line.substr(TokenSpan(line, at, 5).first)); } catch (...) {}
            if (const auto k = partKeyOfNode.find(idx); k != partKeyOfNode.end())
            {
                ++rep.prefabPart.lines;
                const std::string want = MeshKeyToHex(k->second);   // (inside Resource::, unqualified)
                if (const size_t at = FindToken(line, "msec"); at != std::string::npos)
                {
                    const auto [s, e] = TokenSpan(line, at, 6);
                    if (line.compare(s, e - s, want) == 0) ++rep.prefabPart.kept;
                    else ++rep.prefabPart.dangling;   // the part moved since: its key is what follows it
                }
                else
                {
                    const size_t nameAt = FindToken(line, "name");
                    line.insert(nameAt == std::string::npos ? line.size() : nameAt, " msec=" + want);
                    ++rep.prefabPart.added;
                }
            }
        }

        out += line;
        out += cr;
        if (!last) out += '\n';
        b = e + 1;
        if (last) break;
    }

    rep.changed = (out != text);
    if (!apply || !rep.changed) return true;
    if (!WriteSceneContainer(path, sc, out))
        return fail("cannot write '" + path + "'");
    rep.written = true;
    return true;
}

// ==== StripColliderEchoes (contract in the header) ====
namespace
{
    // The value of @p key's token on a component line (`  Tag: key=value ...`),
    // percent-decoded; "" when absent.
    std::string ComponentValue(const std::string& line, const char* key)
    {
        const size_t at = FindToken(line, key);
        if (at == std::string::npos) return {};
        const auto [s, e] = TokenSpan(line, at, std::strlen(key) + 2);
        return Resource::AssetRefs::PercentDecode(line.substr(s, e - s));
    }

    // The models a scene / prefab text places: its SceneRef lines, and through its
    // PrefabRef lines each prefab's own (recursively, once each), plus the models
    // its StaticMesh lines draw from.
    void CollectPlacedModels(const std::string& text, std::vector<std::string>& models,
                             std::vector<std::string>& staticModels, std::unordered_set<std::string>& seen,
                             int depth)
    {
        auto addUnique = [&](std::vector<std::string>& v, const std::string& p)
        {
            if (p.empty()) return;
            const std::string k = Resource::AssetRefs::NormalizeRefPath(p);
            if (std::none_of(v.begin(), v.end(), [&](const std::string& x)
                             { return Resource::AssetRefs::NormalizeRefPath(x) == k; }))
                v.push_back(p);
        };
        size_t b = 0;
        while (b < text.size())
        {
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(b, e - b);
            b = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t tag = line.find_first_not_of(' ');
            if (tag == std::string::npos) continue;
            if (line.compare(tag, 10, "SceneRef: ") == 0) addUnique(models, ComponentValue(line, "path"));
            else if (line.compare(tag, 12, "StaticMesh: ") == 0) addUnique(staticModels, ComponentValue(line, "model"));
            else if (line.compare(tag, 11, "PrefabRef: ") == 0 && depth < 16)
            {
                const std::string prefab = ComponentValue(line, "path");
                if (prefab.empty() || !seen.insert(Resource::AssetRefs::NormalizeRefPath(prefab)).second) continue;
                SceneContainer pc;
                std::string    why;
                if (ReadSceneContainer(prefab, pc, why))
                    CollectPlacedModels(pc.text, models, staticModels, seen, depth + 1);
            }
        }
    }
}

bool Resource::StripColliderEchoes(const std::string& path, bool apply, ColliderEchoReport& rep, std::string* outError)
{
    rep = ColliderEchoReport{};
    auto fail = [&](std::string why)
    {
        if (outError) *outError = why;
        return false;
    };
    SceneContainer sc;
    std::string    why;
    if (!ReadSceneContainer(path, sc, why)) return fail(why);
    const std::string& text = sc.text;

    std::vector<std::string>        staticModels;
    std::unordered_set<std::string> seenPrefabs;
    CollectPlacedModels(text, rep.models, staticModels, seenPrefabs, 0);

    // What every placed part gets from its model's C records, by (library, entry).
    struct Slot
    {
        bool                           withoutC = false;   // a part with no C record
        std::vector<ColliderComponent> given;              // what the others get
    };
    std::unordered_map<std::string, Slot> byPart;
    auto partKey = [](const std::string& lib, uint32_t entry)
    { return AssetRefs::NormalizeRefPath(lib) + '#' + std::to_string(entry); };
    SceneInstanceLoader::LibraryIdentityCache libs;
    for (const std::string& model : rep.models)
    {
        std::vector<SceneInstanceLoader::PartColliderDefault> rows;
        if (!SceneInstanceLoader::ReadPartColliderDefaults(model, rows, libs)) continue;
        for (const auto& r : rows)
        {
            Slot& s = byPart[partKey(r.lib, r.entry)];
            if (r.hasCollider) s.given.push_back(r.collider);
            else               s.withoutC = true;
        }
    }
    // The libraries a StaticMesh of the file draws from (its derived sections carry
    // MeshSourcePath + MeshLibRef, so an MC line reaches them too).
    std::unordered_set<std::string> staticLibs;
    for (const std::string& model : staticModels)
    {
        std::vector<uint8_t> mb;
        if (!AssetFS::Get().ReadFile(model, mb) || !ValidateHeader(mb.data(), mb.size(), MAGIC_SCENE)) continue;
        const size_t off = sizeof(AssetHeader) + GetHeader(mb.data())->metadataSize;
        if (off >= mb.size()) continue;
        std::string mt(reinterpret_cast<const char*>(mb.data() + off),
                       (std::min)(static_cast<size_t>(GetHeader(mb.data())->dataSize), mb.size() - off));
        if (const size_t nul = mt.find('\0'); nul != std::string::npos) mt.resize(nul);
        for (const auto& fr : SceneInstanceLoader::ListFileReferences(mt, model))
            if (fr.kind == 'F' && fr.key == "file") staticLibs.insert(AssetRefs::NormalizeRefPath(fr.path));
    }

    RigidBodyComponent spawnBody{};
    spawnBody.motion = RigidBodyComponent::Motion::Static;

    std::string out;
    out.reserve(text.size());
    size_t b = 0;
    while (b < text.size())
    {
        size_t e = text.find('\n', b);
        const bool last = (e == std::string::npos);
        if (last) e = text.size();
        const std::string raw = text.substr(b, e - b);   // with its '\r', if any
        b = e + 1;
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        bool drop = false;
        if (line.rfind("MC ", 0) == 0)
        {
            ++rep.mcLines;
            MeshColliderOverride mco;
            bool parsed = true;
            try { ParseMCLine(line, mco); } catch (...) { parsed = false; }
            if (!parsed) ++rep.unparsed;
            else
            {
                KeyResolveStats ks;
                const uint32_t want = EntryForKey(mco.sourcePath, mco.sourceMeshId, mco.sourceSec, ks);
                ColliderComponent col = mco.collider;
                if (mco.hasCollider && mco.meshSec != 0)
                    col.meshLibMeshId = EntryForKey(col.meshLibPath, col.meshLibMeshId, mco.meshSec, ks);
                const auto it = byPart.find(partKey(mco.sourcePath, want));
                if (staticLibs.count(AssetRefs::NormalizeRefPath(mco.sourcePath))) ++rep.staticMesh;
                else if (it == byPart.end()) ++rep.unplaced;
                else if (it->second.withoutC) ++rep.authored;
                else if (!mco.hasCollider || !mco.hasRb || !SameAuthoredBody(spawnBody, mco.rb) ||
                         std::any_of(it->second.given.begin(), it->second.given.end(),
                                     [&](const ColliderComponent& g) { return !SameAuthoredCollider(g, col); }))
                    ++rep.differs;
                else
                {
                    ++rep.echoes;
                    drop = true;
                }
            }
        }
        if (!drop)
        {
            out += raw;
            if (!last) out += '\n';
        }
        if (last) break;
    }

    rep.changed = (out != text);
    if (!apply || !rep.changed) return true;
    if (!WriteSceneContainer(path, sc, out))
        return fail("cannot write '" + path + "'");
    rep.written = true;
    return true;
}

// ==== ColliderIsSpawnDefault (contract in the header) ====
bool Resource::ColliderIsSpawnDefault(World& world, Entity e, bool* outFromAsset)
{
    const ColliderSpawnDefault* def = world.GetComponent<ColliderSpawnDefault>(e);
    const ColliderComponent*    col = world.GetComponent<ColliderComponent>(e);
    const RigidBodyComponent*   rb  = world.GetComponent<RigidBodyComponent>(e);
    if (!def || !col || !rb) return false;
    if (outFromAsset) *outFromAsset = def->fromAsset;
    return SameAuthoredCollider(def->collider, *col) && SameAuthoredBody(def->rigidBody, *rb);
}
