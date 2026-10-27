#pragma once

// CollisionMeshBaker - runs the source mesh of every MeshLibRef entity through
// meshoptimizer's edge-collapse simplifier and writes all results into one .meshlib in the
// engine's native format, loadable by MeshLibrary like any mesh: collision proxies, far
// LODs, any low-poly version of the scene.
// Vertex stride 32 B (pos + normal + uv) with normal/uv zeroed: collision and LOD callers
// do not need them, and GBuffer.vs synthesises a basis for missing tangents.
// Begin() dispatches the queue to TaskSystem workers, each decoding and simplifying its
// slice into per-item slots (no contention); Tick() polls an atomic counter for the
// progress bar and merges + writes the .meshlib on the call that sees completion:
//
//   BakeJob job;
//   job.Begin(world, opts);
//   while (job.Tick()) { /* paint progress bar */ }
//   // job.IsComplete(): read job.Results() / job.OutputPath()

#include "ECS/ECS.h"
#include "Resource/AssetHeader.h"   // Resource::MeshLibraryEntry (accumulator type)

class DebugWirePass;
namespace DX12Physics { class PhysicsSystem; }

#include <DirectXMath.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Tools::CollisionMesh {

struct BakeOptions
{
    float       targetTriangleRatio = 0.10f;
    float       targetError         = 0.05f;
    uint32_t    minTriangles        = 32;
    bool        skipSkinned         = true;
    std::string outputPath          = "asset/collision/baked_collision.meshlib";
};

struct BakeItemResult
{
    std::string sourcePath;
    uint32_t    sourceMeshId   = 0;     // meshId in the source .meshlib (or 0 for .imsh)
    uint64_t    sourceSectionKey = 0;   // that entry's section key (0 for .imsh)
    uint32_t    bakedMeshId    = ~0u;   // entry index in the OUTPUT .meshlib
    uint32_t    origTriCount   = 0;
    uint32_t    bakedTriCount  = 0;
    uint32_t    origVertCount  = 0;
    uint32_t    bakedVertCount = 0;
    float       actualError    = 0.f;
    bool        success        = false;
    bool        skipped        = false;
    std::string reason;
};

// Per-frame wireframe overlay for every ColliderComponent within `maxDistance` of
// `cameraPos` (0 = all), drawn the way PhysicsSystem builds its shape: a box / sphere /
// capsule as the primitive, a Mesh as its .meshlib slice (bytes from PhysicsSystem's blob
// cache, the decode cached per slice), a ConvexHull and a MeshAsset (a Mesh's asset
// collision) as the physics shape's own triangles (cached per shape). The caller clears
// DebugWirePass first (Renderer::BeginFrame does); lines past DebugWirePass::kMaxVertices
// are dropped silently.
// Coloured by what the body is (0xAABBGGRR): `color` (tan) = static world, blue = a moving
// (Dynamic / Kinematic) body, yellow and dashed = a trigger, red = HurtBox layer, orange =
// AttackBox, purple = any other explicit layer, white = @p selected (drawn over the rest).
constexpr uint32_t kWireStatic   = 0xff60a0e0u;
constexpr uint32_t kWireMoving   = 0xffff9060u;
constexpr uint32_t kWireTrigger  = 0xff30e0ffu;
constexpr uint32_t kWireHurtBox  = 0xff3030ffu;
constexpr uint32_t kWireAttack   = 0xff1080ffu;
constexpr uint32_t kWireLayer    = 0xffff60c0u;
constexpr uint32_t kWireSelected = 0xffffffffu;
void EmitDebugWireframe(World& world,
                        const DirectX::XMFLOAT3& cameraPos,
                        float maxDistance,
                        DebugWirePass& dbg,
                        DX12Physics::PhysicsSystem& physics,
                        uint32_t color = kWireStatic,
                        Entity selected = NullEntity);

// What the last EmitDebugWireframe call drew (the editor's Debug.CollisionOverlay
// and its test): colliders seen, drawn as a primitive / as triangles (hull, mesh,
// asset) and skipped (culled, no shape, nothing to draw), the asset colliders
// among the triangle ones, the triggers drawn dashed, lines pushed for triangle
// shapes, and whether @p selected was drawn.
struct WireStats
{
    uint32_t seen = 0, prims = 0, meshes = 0, assets = 0, skipped = 0, triggers = 0;
    uint32_t triangleLines = 0;
    bool     selectedDrawn = false;
};
const WireStats& LastWireStats();

class BakeJob
{
public:
    struct Item { std::string path; uint32_t meshId; };

    BakeJob()  = default;
    ~BakeJob();

    BakeJob(const BakeJob&)            = delete;
    BakeJob& operator=(const BakeJob&) = delete;

    // Snapshot every renderable MeshLibRef entity into the work queue (dedup
    // by (sourcePath, meshId)), then dispatch the per-item decode+simplify
    // jobs to TaskSystem workers. Returns immediately.
    void Begin(World& world, const BakeOptions& opts);

    // Poll worker progress; when the queue has finished, merge + write the
    // combined .meshlib on the calling (main) thread. Returns true while
    // background work remains, false once IsComplete().
    bool Tick();

    bool         IsRunning()    const { return m_active && !m_completed; }
    bool         IsComplete()   const { return m_completed; }
    uint32_t     Total()        const { return static_cast<uint32_t>(m_queue.size()); }
    uint32_t     Processed()    const { return m_processedCount.load(std::memory_order_acquire); }
    float        Progress()     const
    {
        const uint32_t total = Total();
        return total == 0 ? 1.f : static_cast<float>(Processed()) / static_cast<float>(total);
    }
    const std::vector<BakeItemResult>& Results() const { return m_results; }
    const std::string& OutputPath()     const { return m_opts.outputPath; }
    bool         WriteFailed()  const { return m_writeFailed; }

    uint64_t     OrigTriTotal() const { return m_origTriTotal; }
    uint64_t     BakedTriTotal()const { return m_bakedTriTotal; }

    // Wait for any in-flight workers, then clear state.
    void Reset();

    // Public so the editor can build a (sourcePath, sourceMeshId) -> bakedMeshId map for
    // swap-in preview without re-iterating the world.
    const std::vector<Item>& Queue() const { return m_queue; }

private:
    // Worker-thread output for one queue item. Workers never touch each other's slots, so the
    // parallel phase needs no lock.
    struct PerItemOutput
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<uint32_t>          indices;
        DirectX::XMFLOAT3              aabbMin = { 0, 0, 0 };
        DirectX::XMFLOAT3              aabbMax = { 0, 0, 0 };
        float                          actualError = 0.f;
        uint32_t                       origVertCount = 0;
        uint32_t                       origTriCount  = 0;
        uint64_t                       sourceSectionKey = 0;
        bool                           success       = false;
        std::string                    reason;
    };

    // Runs on a worker thread.
    void DecodeAndSimplify(const Item& item, PerItemOutput& out);
    // Main-thread serial pass that appends one item's output into the shared
    // pools and produces the user-facing BakeItemResult entry.
    void AppendOne(const Item& item, const PerItemOutput& slot, BakeItemResult& outResult);
    // Main-thread: build the final blob and write to disk.
    bool Finalize();

    void WaitForWorkers();

    BakeOptions                              m_opts;
    std::vector<Item>                        m_queue;
    std::vector<PerItemOutput>               m_perItem;          // sized to queue
    std::atomic<uint32_t>                    m_processedCount{ 0 };
    std::atomic<uint32_t>                    m_inFlightChunks{ 0 };
    bool                                     m_active      = false;
    bool                                     m_completed   = false;
    bool                                     m_writeFailed = false;

    // Output pools (32 B/vertex pos+normal+uv).
    std::vector<uint8_t>                     m_mergedVB;
    std::vector<uint32_t>                    m_mergedIB;
    std::vector<Resource::MeshLibraryEntry>  m_entries;

    std::vector<BakeItemResult>              m_results;
    uint64_t                                 m_origTriTotal  = 0;
    uint64_t                                 m_bakedTriTotal = 0;
};

} // namespace Tools::CollisionMesh
