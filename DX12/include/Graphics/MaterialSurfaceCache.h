#pragma once

// MaterialSurfaceCache - Lumen-style surface cache for CUSTOM-SHADER materials.
// Why: every ray-tracing consumer shades a hit from the 128-byte RTInstanceData proxy, which can only
// describe an engine-standard PBR material. A `useCustomShader = 1` material gets its look from a
// pixel shader the ray cannot execute, its maps live in NAME-KEYED space3 slots, and its engine
// BASECOLORMAP holds the importer's default texture - so without this an NPR character reflects as an
// over-bright, featureless mannequin.
// How: the custom PS hook already normalises to engine convention
// (`EvaluateMaterial(MaterialPixelParams, inout SurfaceData)`), so RUNNING THE MATERIAL'S OWN GBUFFER
// PIXEL SHADER yields exactly the data the proxy wants, with no name guessing and no NPR
// re-implementation. Only WHERE it rasterises changes: a bake VS emits
// `SV_Position = float4(uv0 * 2 - 1, 0, 1)` (Y flipped), so the mesh rasterises into its own UV space.
// No cards or capture cameras: character meshes carry a clean UV0 unwrap.
// Owns: the two atlases (albedo + surface, created on first need, so a scene with no custom-shader
// material pays no VRAM); the page allocator (fixed grid, LRU) and the key -> page map; the per-frame
// bake schedule (N pages a frame, never all at once); the entity -> page map that
// DDGISceneAS::fillMaterialProxy joins against.
// Page key = (meshDescriptorIndex, materialIndex): exactly the DrawPacket batch identity, and
// DrawPackets are INSTANCED batches, so no per-entity key can be derived from one. It is also the
// mesh-section x material granularity the bake needs: sections overlap heavily in UV space (one
// character: 69 of 105 section pairs overlap > 5% of the smaller section's UV area), so a
// material-only key would make sections overwrite each other.
// Threading - two threads, in two disjoint windows of the frame:
//   * PRODUCER (main thread) - AcquirePage / MapEntity from Renderer::BuildScene_SortAndEmit or, under
//     -surfacepagesearly, from the ResolveBatchPlan stage just ahead of the emit nest. BeginFrame /
//     OnWorldClear / SetEnabled / SetPagesPerFrame are main too. DDGISceneAS::Gather reads
//     LookupEntity / GetPublishEpoch on main, later in the same Renderer::BeginFrame.
//   * BAKE (a render-graph WORKER; the render-owner lane under -renderowner) - TakeBakeSlot /
//     MarkBaked / SetAtlasState from SurfaceCacheBakePass::Execute.
// The frame BARRIER (DrainFrameWork ahead of Renderer::BeginFrame) retires frame F-1's bake before
// frame F's acquire runs; that, not a single thread, keeps them apart. The two windows also touch
// disjoint Page fields (the bake side never writes lastSeenFrame / inUse / key / contentHash /
// m_keyToPage), which makes the producer-side replay in ResolveBatchPlan bit-identical. No locking,
// deliberately: a mutex would only hide a future violation of the barrier.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;

class MaterialSurfaceCache
{
public:
    // 2048^2 atlas / 128^2 pages = 256 pages, ~32 MB (RGBA16F) + ~16 MB (RGBA8). A character asset has
    // over 100 mesh-section / material pairs; 256 pages keep a whole skinned character resident.
    static constexpr uint32_t kAtlasSize   = 2048;
    static constexpr uint32_t kPageSize    = 128;
    static constexpr uint32_t kPagesPerRow = kAtlasSize / kPageSize;   // 16
    static constexpr uint32_t kPageCount   = kPagesPerRow * kPagesPerRow; // 256
    static constexpr uint32_t kInvalidPage = 0xFFFFFFFFu;
    // Hard cap on the per-frame bake budget. SurfaceCacheBakePass sizes its resolve-CB ring from it, so
    // SetPagesPerFrame clamps to it rather than keeping two independent constants.
    static constexpr uint32_t kMaxPagesPerFrame = 8;

    // A baked page is publishable to the proxy only once the bake has certainly RETIRED on the GPU. The
    // bake runs in the render graph (graphics queue) while the RT consumers that sample the atlas may run
    // on the COMPUTE queue in a LATER frame with no fence between them, so "the CPU recorded the bake" is
    // not "the texels exist". Waiting kFramesInFlight frames is the guarantee the engine's deferred
    // release and upload rings rely on too.
    static constexpr uint64_t kPublishLagFrames = RHI::kFramesInFlight;

    // Per-instance data the proxy needs. `rect` is the page's uvTilingOffset (mul.xy, add.zw); the
    // material's OWN tiling is already baked into the page, which is why the Renderer's emit gate pages
    // only identity-UV materials (MB_IDENTITY_UV).
    // TODO(MaterialSurfaceCache M5): a mesh whose own uv0 leaves [0,1] with identity tiling still bleeds
    // into the neighbouring page; closing it needs a per-mesh UV-bounds field produced at import.
    struct PageBinding
    {
        float   rect[4]      = { 1.0f, 1.0f, 0.0f, 0.0f };
        int32_t albedoTexIdx = -1;
        int32_t surfaceTexIdx = -1;
    };

    // ---- Configuration ----
    // OFF until the developer settings apply (materialSurfaceCache.enabled, on by default). Disabled,
    // every entry point below is a no-op returning "no page": no atlas allocation, no
    // RT_INST_FLAG_SURFACE_CACHE, no proxy change.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v);
    // Pages baked per frame. Each bake re-issues a full mesh-section draw, a real raster cost: amortise,
    // never burst.
    uint32_t GetPagesPerFrame() const { return m_pagesPerFrame; }
    void     SetPagesPerFrame(uint32_t n)
    { m_pagesPerFrame = (n == 0) ? 1u : ((n > kMaxPagesPerFrame) ? kMaxPagesPerFrame : n); }

    // ---- Frame lifecycle ----
    // Called once per frame from Renderer::BeginFrame, BEFORE BuildRenderScene: advances the frame
    // counter, re-arms the bake budget and publishes pages whose bake has aged past kPublishLagFrames.
    void BeginFrame();
    // Scene swap: pages are keyed by meshDescriptorIndex, a MeshDescriptor HEAP SLOT recycled across
    // worlds, so a stale key would hand the new world's mesh the old world's texels. Only the mapping is
    // dropped; the atlas textures survive (engine-owned, their bindless slots must stay stable).
    void OnWorldClear();
    void Shutdown(IGraphicsDevice& gfx);

    // ---- Producer side (Renderer::SortAndEmit) ----
    // Gets (or creates) the page for this batch. Returns kInvalidPage when the cache is off, the atlas
    // could not be created, or every page is in use by a batch already seen THIS frame.
    // @p contentHash invalidates: BOTH halves of the key are RECYCLED HEAP SLOTS (a material slot is
    // reused for another binding key after Renderer's kMatSlotReuseLagEpochs, a mesh-descriptor slot when
    // a mesh is released), so the key can survive while the content behind it changes completely. A
    // mismatch forces a re-bake and un-publishes the page until it lands.
    // An ordinary material edit needs none of this: it changes matHash, hence the material slot, hence
    // the KEY, so a fresh page is allocated and the old one ages out by LRU. Same for a custom texture
    // finishing its async load (HashMatParams folds the resolved bindless indices). Hence no dirty flags.
    uint32_t AcquirePage(IGraphicsDevice& gfx, uint32_t meshDescIdx,
                         uint32_t materialIndex, uint64_t contentHash);
    // Records that @p entityId draws from @p pageIdx this frame. The map is rebuilt every frame
    // (stamped, not cleared), so an entity no longer drawn silently drops back to the alias path.
    void MapEntity(uint32_t entityId, uint32_t pageIdx);
    // Every entity MapEntity stamped THIS frame, once each - the only entities LookupEntity can answer
    // with a page (it refuses a stamp from another frame), so a consumer that re-derives page bindings
    // every frame (DDGISceneAS's retained gather) visits these instead of every row it holds. Cleared by
    // BeginFrame; same thread rules as m_entityPage (written by the producer, read after it).
    const std::vector<uint32_t>& MappedThisFrame() const { return m_mappedThisFrame; }
    // Creates the atlas NOW on the calling (main) thread instead of inside the first AcquirePage: under
    // -brsplanlane AcquirePage runs on the render-owner lane, and a CreateTexture there would interleave
    // bindless handles with main's. False when the feature is off or creation failed; idempotent.
    bool PrepareAtlas(IGraphicsDevice& gfx) { return m_enabled && EnsureAtlas(gfx); }

    // ---- Consumer side (DDGISceneAS::fillMaterialProxy) ----
    // True only when the entity has a page AND that page holds baked texels the GPU has certainly
    // finished writing. Never sample an UNBAKED page: the caller falls back to the alias path.
    bool LookupEntity(uint32_t entityId, PageBinding& out) const;
    // Bumped whenever the set of publishable pages CHANGES (a page becomes usable, is evicted, or the
    // feature is toggled). DDGISceneAS's retained static gather replays a cached blob on unchanged frames
    // and would keep the pre-bake proxy forever; it re-gathers fully when this value moves.
    uint32_t GetPublishEpoch() const { return m_publishEpoch; }

    // ---- Bake side (SurfaceCacheBakePass) ----
    // Any page waiting for texels? Drives the pass's IsActive() gate, which the graph queries in its
    // SERIAL phase before opening a command list.
    bool HasPendingBakes() const { return m_enabled && m_pendingBakes > 0; }
    // If the draw packet with @p meshDescIdx / @p materialIndex owns a page that needs baking and budget
    // is left this frame, returns the page index and consumes one budget slot; else kInvalidPage.
    uint32_t TakeBakeSlot(uint32_t meshDescIdx, uint32_t materialIndex);
    // The bake for @p pageIdx has been RECORDED; publication waits kPublishLagFrames.
    void MarkBaked(uint32_t pageIdx);
    void GetPageOrigin(uint32_t pageIdx, uint32_t& x, uint32_t& y) const;

    // ---- Atlas access ----
    bool AtlasReady() const { return m_albedoAtlas.IsValid() && m_surfaceAtlas.IsValid(); }
    const RHI::Texture& GetAlbedoAtlas()  const { return m_albedoAtlas; }
    const RHI::Texture& GetSurfaceAtlas() const { return m_surfaceAtlas; }
    // The bake pass owns the atlas barriers; the cache just remembers where it
    // left them so the next frame's first transition has the right `before`.
    RHI::ResourceState GetAtlasState() const { return m_atlasState; }
    void SetAtlasState(RHI::ResourceState s) { m_atlasState = s; }

    // ---- Diagnostics ----
    // Monotonic call counters. -surfacepagesearly moves every producer mutation out of the emit nest; the
    // nest brackets itself with these two and ENGINE_CHECK_ONCEs that neither moved. Never reset: the
    // emit side compares a before / after pair.
    uint32_t GetAcquireCallCount()   const { return m_dbgAcquireCalls; }
    uint32_t GetMapEntityCallCount() const { return m_dbgMapEntityCalls; }
    uint32_t GetPagesInUse()   const { return m_pagesInUse; }
    uint32_t GetPagesBaked()   const { return m_pagesBaked; }
    uint32_t GetPendingBakes() const { return m_pendingBakes; }

private:
    struct Page
    {
        uint64_t key           = 0;      // (meshDescIdx << 32) | materialIndex
        uint64_t contentHash   = 0;      // see AcquirePage - recycled-slot guard
        uint64_t lastSeenFrame = 0;      // last AcquirePage hit - LRU key
        uint64_t bakedFrame    = 0;      // frame the bake was RECORDED
        // Frame a bake slot was handed out for this page. Two DrawPackets can share a (meshDesc, material)
        // key while differing in another batch-key field (mirrored, viewMask, stencil ref); without this the
        // bake pass would bake the same page twice in one frame.
        uint64_t claimedFrame  = 0;
        bool     inUse         = false;
        bool     needsBake     = true;
        bool     baked         = false;  // has been rasterised at least once
        bool     usable        = false;  // baked AND aged past kPublishLagFrames
    };

    struct EntitySlot { uint64_t frame = 0; uint32_t page = kInvalidPage; };

    // Creates the two atlases on first use. Returns false (logging once) if creation fails, which
    // disables paging for the session rather than retrying a failing allocation every frame.
    bool EnsureAtlas(IGraphicsDevice& gfx);
    void ResetPages();

    bool     m_enabled       = false;
    uint32_t m_pagesPerFrame = 8;
    uint64_t m_frame         = 0;
    uint32_t m_budgetLeft    = 0;
    uint32_t m_publishEpoch  = 1;   // 0 reserved for "never read"

    RHI::Texture       m_albedoAtlas;    // RGBA16F: rgb = linear albedo, a = opacity
    RHI::Texture       m_surfaceAtlas;   // RGBA8:   r = AO, g = rough, b = metal, a = emissive mask
    RHI::ResourceState m_atlasState = RHI::ResourceState::SHADER_RESOURCE;
    bool               m_atlasFailed = false;

    Page                                  m_pages[kPageCount];
    std::unordered_map<uint64_t, uint32_t> m_keyToPage;

    // Entity id -> page, stamped with the frame it was written so the map is rebuilt (not cleared) every
    // frame. Dense, as entity ids are (same pattern as m_prevWorldDense).
    std::vector<EntitySlot> m_entityPage;
    std::vector<uint32_t>   m_mappedThisFrame;   // see MappedThisFrame()

    uint32_t m_pagesInUse   = 0;
    uint32_t m_pagesBaked   = 0;
    uint32_t m_pendingBakes = 0;
    // Latched when the LRU scan finds every page live this frame. Once true it cannot become false
    // within the frame (see AcquirePage), so every later scan is skipped.
    bool     m_atlasFullThisFrame = false;

    // Per-frame churn counters, plotted from BeginFrame under TRACY_ENABLE and reset there. A healthy
    // steady-state scene settles at 0/0/0 (allocated once, baked once, then hit). Sustained churn is a
    // material whose identity is unstable frame to frame, which shows up as DDGISceneAS falling back to
    // its full static gather (the publish epoch these paths bump invalidates that gather).
    uint32_t m_statAcquired = 0;   // fresh page allocations
    uint32_t m_statEvicted  = 0;   // LRU evictions of an in-use page
    uint32_t m_statPromoted = 0;   // bakes aged past the publish lag
    // Tripwire counters - see GetAcquireCallCount. Monotonic for the process.
    uint32_t m_dbgAcquireCalls   = 0;
    uint32_t m_dbgMapEntityCalls = 0;
};
