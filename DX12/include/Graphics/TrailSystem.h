#pragma once

// TrailSystem - GPU trail ring buffers and the per-frame append requests that feed them.
//
// Segment pool: one DEFAULT-heap structured buffer of kMaxTrails x kMaxSegmentsPerTrail x
// kMaxPointsPerTrail TrailSegmentGPU records, point-major (see TrailSegmentGPU). One sample =
// one CollectTrails emission with up to kMaxPointsPerTrail point rows (multi-point weapon
// ribbons; single-point trails use row 0 only). A header buffer holds per-trail metadata
// (ring cursor, count, width, colours, ribbon params). Both are GPU-writable so
// TrailUpdatePass can append samples and advance the cursor.
//
// Per frame:
//   CPU: CollectTrails walks the TrailComponent pool, leases slots, gates samples by
//        minSampleDistance and writes append requests into the UPLOAD request buffer.
//   GPU: TrailUpdatePass, one thread per trail: ages the trail's samples, then replays its
//        requests in buffer order (append at the head, advance the cursor, cap the count).
//   GPU: TrailRenderPass; the VS extrudes sample pairs into a ribbon.

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/MaterialCBVRing.h"   // b8 custom-shader param upload ring
#include "Resource/SystemHandles.h"     // Resource::TextureHandle
#include "ECS/ECS.h"

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <DirectXMath.h>

class IGraphicsDevice;
class World;
class ShaderLibrary;
namespace ShaderReflect { struct Reflection; }   // resolve-cache pointer only
namespace Resource { class TextureSystem; class ResourceManager; }

// One ribbon point record, 32 bytes; mirrors TrailSegment in shaders/Trail.hlsli.
// The pool is point-major: a segment slot is one SAMPLE (one CollectTrails emission)
// owning kMaxPointsPerTrail consecutive point records:
//   record = ((trailSlot * kMaxSegmentsPerTrail) + sampleSlot) * kMaxPointsPerTrail + p
// p = 0 (blade base) .. pointCount-1 (blade tip). Rows at or above pointCount are never
// written or read.
struct alignas(16) TrailSegmentGPU
{
    DirectX::XMFLOAT3 position;   // 12 - world-space position of point p of this sample
    float             age;        // 4  - seconds since spawn (CPU substeps start at their ageOffset)
    float             arcLen;     // 4  - metres of this row's polyline since trail (re)start (GPU-accumulated)
    uint32_t          _pad0;      // 4
    uint32_t          _pad1;      // 4
    uint32_t          _pad2;      // 4
};
static_assert(sizeof(TrailSegmentGPU) == 32, "TrailSegmentGPU drift");

// Per-trail metadata, 80 bytes; mirrors TrailHeader in shaders/Trail.hlsli.
struct alignas(16) TrailHeaderGPU
{
    uint32_t          head;               // 4 - next write slot, in SAMPLES (ring cursor)
    uint32_t          count;              // 4 - live sample count (capped at kMaxSegmentsPerTrail)
    float             width;              // 4 - camera-facing half-width (pointCount == 1 only)
    float             maxAge;             // 4
    DirectX::XMFLOAT4 startColor;         // 16
    DirectX::XMFLOAT4 endColor;           // 16
    uint32_t          pointCount;         // 4 - 1..kMaxPointsPerTrail; a zero-initialised 0 reads as 1 in the VS
    uint32_t          textureBindlessIdx; // 4 - 0xFFFFFFFF = untextured
    uint32_t          renderFlags;        // 4 - TrailRenderFlag bits 0-3; bits 8-15 = custom-shader bucket
    uint32_t          subdivisions;       // 4 - Catmull-Rom sub-segments per pair, 1..kMaxSubdivisions
    float             uvTileLength;       // 4 - arc UV: metres per U repeat; 0 = stretch over the live trail
    float             hdrIntensity;       // 4 - rgb multiplier (bloom feed)
    uint32_t          _reserved[2];       // 8
};
static_assert(sizeof(TrailHeaderGPU) == 80, "TrailHeaderGPU drift");

// One append request from the CPU, 128 bytes; mirrors TrailAppendRequest in
// shaders/Trail.hlsli: a 64-byte base block (point 0 and the header params) and a 64-byte
// block with points 1-3 and the multi-point params. flags: TrailRequestFlag bits.
struct alignas(16) TrailAppendRequestGPU
{
    // ---- base block (64 B) ----
    DirectX::XMFLOAT3 position;    // 12 - point 0 (base) world position
    float             dt;          // 4  - frame dt (the CS ages with TrailSystemCB.deltaTime)
    uint32_t          trailSlot;   // 4  - target trail index in the segment pool
    float             width;       // 4
    float             maxAge;      // 4
    uint32_t          flags;       // 4  - TrailRequestFlag bits
    DirectX::XMFLOAT4 startColor;  // 16 - append and refresh requests rewrite the header params
    DirectX::XMFLOAT4 endColor;    // 16
    // ---- multi-point block (64 B) ----
    DirectX::XMFLOAT4 position1;   // 16 - xyz = point 1 world pos; w = uvTileLength
    DirectX::XMFLOAT4 position2;   // 16 - xyz = point 2 world pos; w = hdrIntensity
    DirectX::XMFLOAT4 position3;   // 16 - xyz = point 3 world pos; w = ageOffset (substep age, 0 = newest)
    uint32_t          pointCount;         // 4 - 1..kMaxPointsPerTrail
    uint32_t          textureBindlessIdx; // 4
    uint32_t          renderFlags;        // 4 - same bit layout as the header
    uint32_t          subdivisions;       // 4
};
static_assert(sizeof(TrailAppendRequestGPU) == 128, "TrailAppendRequestGPU drift");

enum TrailRequestFlag : uint32_t
{
    TrailRequest_ResetHeader   = 1u << 0,  // zero head/count before appending (slot reuse)
    TrailRequest_ResetOnly     = 1u << 1,  // clear a freed slot's header, no append
    TrailRequest_RefreshParams = 1u << 2,  // update header params only, no append (idle-edit path)
};

// Per-frame globals (cbuffer), 16 bytes; mirrors TrailSystemParams in shaders/Trail.hlsli.
struct alignas(16) TrailSystemCB
{
    float    deltaTime;     // 4
    uint32_t requestCount;  // 4 - number of append requests this frame
    uint32_t maxSegments;   // 4 - kMaxSegmentsPerTrail, for the shader's wrap math
    uint32_t maxTrails;     // 4
};
static_assert(sizeof(TrailSystemCB) == 16, "TrailSystemCB drift");

class TrailSystem
{
public:
    static constexpr uint32_t kMaxTrails           = 64;
    static constexpr uint32_t kMaxSegmentsPerTrail = 64;   // samples per trail (ring length)
    // Point records per sample. MUST mirror TRAIL_MAX_POINTS in Trail.hlsli.
    static constexpr uint32_t kMaxPointsPerTrail   = 4;
    // Catmull-Rom sub-segments per sample pair. MUST mirror TRAIL_MAX_SUBDIV in Trail.hlsli.
    static constexpr uint32_t kMaxSubdivisions     = 4;
    // Max CPU sub-samples per trail per frame (slerp substepping, pointCount>=2).
    static constexpr uint32_t kMaxCpuSubSteps      = 4;
    // Request ring capacity per frame. Budget proof (re-check it before changing any of these
    // constants): sample appends are capped at kMaxAppendsPerFrame (= kMaxTrails x
    // kMaxCpuSubSteps, the true worst case), so the trailing freed-slot ResetOnly sweep
    // (<= kMaxTrails, slots disjoint from any append / refresh target this frame) always fits.
    // A starved reset leaves a frozen ghost ribbon.
    static constexpr uint32_t kMaxRequestsPerFrame = kMaxTrails * kMaxCpuSubSteps + kMaxTrails; // 320
    static constexpr uint32_t kMaxAppendsPerFrame  = kMaxRequestsPerFrame - kMaxTrails;         // 256
    static_assert(kMaxCpuSubSteps <= kMaxAppendsPerFrame / kMaxTrails,
                  "substep budget: kMaxTrails trails x kMaxCpuSubSteps appends must fit kMaxAppendsPerFrame");
    // Unused: requests live in a structured buffer (stride sizeof(TrailAppendRequestGPU)),
    // not in 256 B CB slots.
    static constexpr uint64_t kRequestSlotStride   = 256;

    struct SlotDraw
    {
        uint32_t slot;
        uint64_t paramVA;   // 0 = shader has no b8 cbuffer (draw without bind)
        bool     ready;
    };
    struct DrawBucket
    {
        uint32_t shaderBucket;  // index into shader-bucket path table (0 = default PS)
        uint32_t taaVariant;    // 0 = normal, 1 = TAA-excluded (stencil 0x40)
        std::vector<SlotDraw> slotDraws;   // empty for the default bucket
    };

    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Texture resolution dependencies (optional; null = no texture support). Same
    // entity-keyed Acquire/Release cache pattern as ParticleSystem.
    void SetResourceSystems(Resource::TextureSystem* texSys,
                            Resource::ResourceManager* resMgr)
    {
        m_texSys = texSys;
        m_resMgr = resMgr;
    }

    // World::Clear() fires no per-entity destroy listeners: release the entity-keyed texture
    // caches here or their handles leak across scene loads.
    void OnWorldClear(IGraphicsDevice& gfx);

    // The trail render pass's shader library: custom trail shaders are registered and
    // reflected against it so their shader ids match the PSO cache.
    void SetShaderLibrary(ShaderLibrary* lib) { m_shaderLib = lib; }

    // Per-frame CPU collection: walks the TrailComponent pool, leases trail slots on first
    // sight, gates samples by minSampleDistance and writes append requests into this frame's
    // upload buffer; GetRequestCount() then holds the number of valid entries. Also builds
    // the draw buckets and the instance-decode budget for TrailRenderPass.
    void CollectTrails(World& world, float dt);

    // Number of append requests populated this frame.
    uint32_t GetRequestCount() const { return m_requestCountThisFrame; }

    // Highest leased slot + 1: the render pass's per-trail draw range, so unused slots at
    // the end of the header buffer are not drawn.
    uint32_t GetMaxActiveSlot() const { return m_maxActiveSlot; }

    // Per-frame instance-decode budget maxima (>= 1), over every component holding a valid
    // slot (frozen / disabled ribbons still draw). TrailRenderPass MUST feed its RenderCB AND
    // its DrawInstanced count from these same values: a mismatch scrambles the VS instance
    // decode.
    uint32_t GetFrameMaxPointCount() const { return m_frameMaxPointCount; }
    uint32_t GetFrameMaxSubdiv()     const { return m_frameMaxSubdiv; }

    // One draw per occupied (custom shader, TAA variant) combination this frame, over
    // components holding a VALID slot only (a slotless component has no header, so counting
    // it would desync bucket and header). The VS filters by both keys, so nothing draws
    // twice. Custom-shader buckets draw ONE TRAIL PER DRAW (the b0 g_slotFilter root constant)
    // so each trail gets its own b8 param CBV; the default bucket is one instanced draw.
    // ready = false: the shader is not compiled / reflected yet this frame, so skip the draw
    // (binding an unpacked b8 would fault).
    const std::vector<DrawBucket>& GetDrawBuckets() const { return m_drawBuckets; }
    // Path for a shader bucket id ("" = engine default Trail.ps).
    const std::string& GetShaderBucketPath(uint32_t bucket) const
    {
        static const std::string s_empty;
        return (bucket < m_shaderBucketPaths.size()) ? m_shaderBucketPaths[bucket] : s_empty;
    }

    // Buffer accessors for passes.
    const RHI::GPUBuffer& GetSegmentBuffer() const { return m_segmentBuffer; }
    const RHI::GPUBuffer& GetHeaderBuffer()  const { return m_headerBuffer; }
    // One request buffer per frame in flight: @p gfx selects the current frame's, since the
    // GPU may still be reading another frame's.
    const RHI::GPUBuffer& GetRequestBuffer(IGraphicsDevice& gfx) const;
    const RHI::GPUBuffer& GetSystemCB(IGraphicsDevice& gfx)      const { return m_systemCB.CurrentBuffer(gfx); }

    uint64_t GetSegmentSRVHandle() const { return m_segSrv; }
    uint64_t GetSegmentUAVHandle() const { return m_segUav; }
    uint64_t GetHeaderSRVHandle()  const { return m_hdrSrv; }
    uint64_t GetHeaderUAVHandle()  const { return m_hdrUav; }

    void UpdateSystemCB(float dt);

private:
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct TrailTexCache
    {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
    };

    struct TrailReflCache
    {
        uint32_t pathHash  = 0;      // FNV32 of the fx path this entry resolved
        uint32_t dynId     = ~0u;    // ShaderLibrary::kInvalidDynShaderID
        uint32_t reflEpoch = 0;
        const ShaderReflect::Reflection* refl = nullptr;
    };

    // Lazy slot allocation for a new TrailComponent. Returns ~0u if the
    // pool is exhausted.
    uint32_t AllocateSlot();
    void     ReleaseSlot(uint32_t slot);

    IGraphicsDevice* m_gfx = nullptr;

    // Main pools (DEFAULT heap, SR+UAV).
    RHI::GPUBuffer m_segmentBuffer;
    RHI::GPUBuffer m_headerBuffer;
    uint64_t       m_segSrv = 0, m_segUav = 0;
    uint64_t       m_hdrSrv = 0, m_hdrUav = 0;

    // Append request buffers (UPLOAD heap, SRV), one per frame in flight, persistently mapped.
    RHI::GPUBuffer m_requestBuffer[kFrameCount];
    void*          m_requestMapped[kFrameCount] = {};
    uint64_t       m_requestSrv[kFrameCount]    = {};

    // Per-frame globals (UPLOAD, CB).
    FrameCB<TrailSystemCB> m_systemCB;

    // Slot allocator: one bit per trail slot (kMaxTrails = 64 fits one uint64_t).
    uint64_t m_slotUsedMask         = 0;
    uint32_t m_maxActiveSlot        = 0;

    // Per-frame instance-decode budget (recomputed in CollectTrails).
    uint32_t m_frameMaxPointCount   = 1;
    uint32_t m_frameMaxSubdiv       = 1;

    // Custom-shader buckets: path -> stable id (0 = "" = default PS). Ids ride in
    // TrailHeaderGPU.renderFlags bits 8-15, so <= 255 distinct paths per session (never freed;
    // bounded by the authored shader count).
    std::unordered_map<std::string, uint32_t> m_shaderBucketByPath;
    std::vector<std::string>                  m_shaderBucketPaths;   // [0] = ""
    std::vector<DrawBucket>                   m_drawBuckets;         // per-frame

    // Custom-shader b8 param plumbing: the material system's MaterialCBVRing and
    // reflection-driven packing, per TRAIL instead of per material.
    ShaderLibrary*  m_shaderLib = nullptr;
    MaterialCBVRing m_paramRing;

    // Texture resolution dependencies (optional; null = no texture support).
    Resource::TextureSystem*   m_texSys = nullptr;
    Resource::ResourceManager* m_resMgr = nullptr;

    // Per-entity texture cache: keeps the acquired TextureHandle alive until the path changes
    // or the component vanishes (swept in CollectTrails).
    std::unordered_map<Entity, TrailTexCache> m_trailTexCache;
    // Custom-shader texture slots ("Tex_*" b8 params): (entity, slot name) -> acquired handle.
    // Swept alongside m_trailTexCache.
    std::unordered_map<Entity, std::unordered_map<std::string, TrailTexCache>>
                                              m_customTexCache;

    // Per-entity dynamic-shader resolve cache, so syncCustomShaderRefl does not pay
    // RegisterDynamic (string key + the library's global mutex) per custom trail per frame for
    // an id that never changes. pathHash catches path edits; refl is re-fetched when the
    // library's reload epoch bumps.
    std::unordered_map<Entity, TrailReflCache> m_trailReflCache;

    uint32_t m_requestCountThisFrame = 0;
};
