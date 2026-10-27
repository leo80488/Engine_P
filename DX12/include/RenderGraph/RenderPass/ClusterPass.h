#pragma once

// ClusterPass - compute pass for clustered deferred shading.
// ClusterBuild computes the per-cluster view-space AABBs when a projection input changes;
// ClusterCull assigns lights and ClusterCullProbes reflection probes to clusters every frame.
// Runs before LightingPass, which reads g_LightGrid and g_LightIndexList. Also owns the light
// record upload, the camera-independent world light grid and the Lights v2 history rings.

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's grid)
#include "Graphics/FrameConstants.h"
#include "Graphics/ResolvedSceneLight.h"
#include <span>
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"
#include "ECS/Components.h"

#include <DirectXMath.h>
#include <atomic>
#include <vector>
#include <cstdint>

class IGraphicsDevice;

class ClusterPass : public RG::PassLifecycle
{
public:
    // Grid constants (must match cluster_common.hlsli). 48x27 is exactly 16:9: square tiles on
    // a 16:9 target (40x40 px at 1080p). Every cluster-sized buffer here and in DecalPass derives
    // from kClusterCount, so changing kTileX / kTileY resizes them all.
    static constexpr uint32_t kTileX   = 48;
    static constexpr uint32_t kTileY   = 27;
    static constexpr uint32_t kSlices  = 24;
    static constexpr uint32_t kClusterCount = kTileX * kTileY * kSlices;
    static constexpr uint32_t kMaxLights        = 4096;
    static constexpr uint32_t kMaxPerCluster    = 32; // must match MAX_PER_CLUSTER in ClusterCull.cs.hlsl
    static constexpr uint32_t kMaxLightIndices  = kClusterCount * kMaxPerCluster;

    // Probe cull: a smaller per-cluster cap, since per-pixel probe loops cost more than light
    // loops. Must match MAX_PROBES_PER_CLUSTER in ClusterCullProbes.cs.hlsl.
    static constexpr uint32_t kMaxProbesPerCluster = 8;
    static constexpr uint32_t kMaxProbeIndices     = kClusterCount * kMaxProbesPerCluster;

    // Resolved light with world-space position/direction (from GlobalTransform).
    using ResolvedLight = ResolvedSceneLight;

    // ---- Lights v2 ----
    // The light list is ordered by creation (LightData::gatherSeq) and holds only ENABLED lights,
    // so adding, removing or toggling one shifts every later index: a reservoir's "light 7" of
    // last frame is another light now. Each upload slot therefore also carries, for the frame
    // that consumes it:
    //   * the index map between the previous upload and this one by stable key
    //     (ResolvedSceneLight::key): [0,kMaxLights) previous -> current,
    //     [kMaxLights,2*kMaxLights) current -> previous, kInvalidLight where a light is gone or
    //     new (RAB_TranslateLightIndex); then at kRemovedOffset the count of REMOVED local lights
    //     (kInvalidLight = more than kMaxRemoved, or one of unknown reach) and per light
    //     float4(position, radius): the receivers it could have lit (its attenuation is exactly 0
    //     at its radius), whose reservoir history was normalized over a target that included it;
    //   * the PREVIOUS upload's records (RAB_LoadLightInfo(i, previousFrame)), copied from the
    //     CPU into this slot's own UPLOAD buffer, since the ring may rewrite the previous slot
    //     while this frame is in flight;
    //   * an alias table over the active local lights by power.
    // The three rings are created on demand (EnsureLightHistory, at the service point), so a
    // frame that needs none allocates nothing.
    static constexpr uint32_t kInvalidLight = 0xFFFFFFFFu;
    static constexpr uint32_t kMaxRemoved = 256;
    static constexpr uint32_t kRemovedOffset = 2 * kMaxLights; // uint index in the map ring
    static constexpr uint32_t kLightMapWords = kRemovedOffset + 4 + 4 * kMaxRemoved;
    struct LightHistory
    {
        uint64_t mapSRV = 0;          // StructuredBuffer<uint> (2*kMaxLights); 0 = no ring yet
        uint64_t prevLightsSRV = 0;   // StructuredBuffer<GPULight> (prevCount rows); 0 = no ring
        uint32_t prevCount = 0;
        bool identity = true;         // the same keys in the same order: nothing to translate
        uint32_t kept = 0, added = 0, removed = 0;
        bool keyed = true;            // every key known (else positional)
    };
    // The alias table: `count` 32-byte rows {light, aliasLight, threshold, pdfLight, pdfAlias,
    // 0,0,0}; pdf = power / total power.
    struct LightAlias { uint64_t srv = 0; uint32_t count = 0; };

    struct ServiceRequest { bool mapAndPrevious = false, alias = false; };

    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // Uploads lights from the CPU; call before Execute(). SetLightsMeta + UploadLightRecords
    // into the game's current upload slot.
    void SetLights(std::span<const ResolvedLight> lights);
    // The two halves of SetLights. META: the count, the active-local-light range and the world
    // light grid - none read shadowSliceIdx, and the game half reads all of them back (DDGI's
    // tick). RECORDS: the per-light GPU rows, shadowSliceIdx included, into an EXPLICIT upload
    // slot, so the render-owner lane can write them after it assigned the shadow slices, for a
    // frame main has moved past.
    void SetLightsMeta(std::span<const ResolvedLight> lights);
    void UploadLightRecords(std::span<const ResolvedLight> lights, uint32_t uploadSlot);

    // Camera inputs of the main view's cluster AABBs; a change to any of them rebuilds the AABBs.
    void SetCamera(const DirectX::XMFLOAT4X4& invProj,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   float nearZ, float farZ,
                   uint32_t screenW, uint32_t screenH,
                   bool orthographic = false);
    // The same for secondary view @p view's grid (its segment, render half). Never through the bound
    // view: the main view's SetCamera runs on the game half.
    void SetViewCamera(ViewState& view, const DirectX::XMFLOAT4X4& invProj,
                       const DirectX::XMFLOAT4X4& viewMatrix, float nearZ, float farZ,
                       uint32_t screenW, uint32_t screenH, bool orthographic);
    // Execute and the grid getters below are the bound view's: the main view keeps its grid here, a
    // secondary view its own in its ViewState (made when the view is first bound, at a frame boundary).
    void BindView(ViewState& view) override;

    // Dispatch compute shaders. Must be called on a COMPUTE or GRAPHICS command list
    // before LightingPass::Execute().
    void Execute(RHI::CommandList cl);

    // SRV handles for LightingPass. The lights buffer is an UPLOAD ring written every frame, so
    // its SRV follows the frame slot; the index list and the grid are DEFAULT heap, their SRVs
    // stable.
    uint64_t GetLightsSRVHandle()    const;
    uint64_t GetLightIndexSRVHandle() const { return m_g->lightIndexSRV; }
    uint64_t GetLightGridSRVHandle() const { return m_g->lightGridSRV; }

    // Cluster AABB SRV, for passes (DecalPass) that reuse this frame's cluster grid instead of
    // dispatching ClusterBuild again.
    uint64_t GetClusterAABBSRVHandle() const { return m_g->aabbSRV; }

    uint64_t GetLightsFingerprint() const; // render-owner, PT-only on-demand hash
    // The lights' fog scales (ResolvedSceneLight::mediumScale), one float a light in the records'
    // order, for the path tracers' medium: this frame's slot as an SRV, 0 when every light's is 1.
    uint64_t GetLightMediumScaleSRVHandle() const;
    uint32_t GetLightCount() const { return m_lightCount; }
    uint32_t GetActiveLocalLightCount() const { return m_activeLocalLightCount; }
    uint32_t GetFirstActiveLocalLightIndex() const { return m_firstActiveLocalLightIndex; }
    uint32_t GetWorldLightGridIndex(bool forUpload = false) const;

    // Reflection-probe input: Renderer pushes the probe StructuredBuffer SRV and the active
    // probe count every frame (BuildScene_UploadProbes). ClusterCullProbes mirrors the light
    // cull and produces the per-cluster probe index list.
    void SetProbes(uint64_t probeBufferSRV, uint32_t probeCount)
    {
        m_probeBufferSRV = probeBufferSRV;
        m_probeCount     = probeCount;
    }
    uint64_t GetProbeIndexSRVHandle() const { return m_g->probeIndexSRV; }
    uint64_t GetProbeGridSRVHandle()  const { return m_g->probeGridSRV;  }
    // Whether this frame's Execute wrote the light / probe grid (render half; the
    // lightComplexity debug target): neither is written with no light / no probe, and the buffer
    // then holds an older frame's.
    bool LightGridWritten() const { return m_g->lightGridWritten; }
    bool ProbeGridWritten() const { return m_g->probeGridWritten; }

    // The render frame's slot (GetFrameIndex), like GetLightsSRVHandle.
    LightHistory GetLightHistory() const;
    LightAlias GetLightAlias() const;
    // Frame boundary only (creates GPU buffers): the rings a consumer needs.
    void EnsureLightHistory(bool mapAndPrevious, bool alias);

    ServiceRequest& Request() { return m_request; }
    // The rings this frame's consumers need: OnService passes them to EnsureLightHistory when the
    // device has inline ray tracing.
    void OnService(RG::ServiceContext& c) override;
    // World clear: no light of the old world maps to the new one. Deferred to
    // the next upload (OnWorldClear runs mid-frame).
    void ResetLightHistory() { m_lightHistoryReset.store(true, std::memory_order_release); }
    uint32_t HandledEvents() const override { return kReloadShaders | kWorldClear | kView; }   // events from Renderer::PassSet
    void OnWorldClear(const RG::WorldClearArgs&) override { ResetLightHistory(); }
    // -lightprepdigest: keys + mapping of the last upload.
    uint64_t GetLightHistoryDigest() const { return m_lightHistoryDigest; }

private:
    // CB layout of ClusterBuild / ClusterCull / ClusterCullProbes (all at b0 space2); viewMatrix
    // is packed at the end.
    struct alignas(16) ClusterCB
    {
        float    invProj[16];     // 64 bytes
        float    nearZ;           // 4
        float    farZ;            // 4
        uint32_t screenW;         // 4
        uint32_t screenH;         // 4
        uint32_t lightCount;      // 4
        uint32_t probeCount;      // 4: read by ClusterCullProbes only
        uint32_t isOrtho;         // 4: ClusterBuild picks the affine z -> NDC formula
        uint32_t probeBufferIdx;  // 4: ClusterCullProbes' heap index of the probe buffer
        float    viewMatrix[16];  // 64 bytes
    }; // total = 160 bytes, fits in 256-byte CB

    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    // One view's cluster grid: the AABBs its projection builds, the light and probe lists culled into
    // them, its CB contents and what the AABBs were built for.
    struct Grid
    {
        RHI::GPUBuffer aabb;         // RWStructuredBuffer<ClusterAABB>, DEFAULT
        RHI::GPUBuffer lightIndex;   // RWStructuredBuffer<uint>, DEFAULT
        RHI::GPUBuffer lightGrid;    // RWStructuredBuffer<LightGridEntry>, DEFAULT
        // Reflection-probe cluster outputs, laid out like the light grid (base offset =
        // clusterIdx * kMaxProbesPerCluster, count in the grid).
        RHI::GPUBuffer probeIndex;   // RWStructuredBuffer<uint>, DEFAULT
        RHI::GPUBuffer probeGrid;    // RWStructuredBuffer<ProbeGridEntry>, DEFAULT
        uint64_t aabbSRV = 0, aabbUAV = 0;
        uint64_t lightIndexSRV = 0, lightIndexUAV = 0;
        uint64_t lightGridSRV = 0, lightGridUAV = 0;
        uint64_t probeIndexSRV = 0, probeIndexUAV = 0;
        uint64_t probeGridSRV = 0, probeGridUAV = 0;
        ClusterCB cb{};
        bool     needsRebuild = true;   // rebuild the AABBs: a projection input changed
        bool     lightGridWritten = false, probeGridWritten = false;   // set by this frame's Execute
        uint32_t lastScreenW = 0;
        uint32_t lastScreenH = 0;
    };
    struct ViewGrid : ViewData
    {
        Grid grid;
        void Release(IGraphicsDevice& gfx) override;
    };

    static constexpr uint32_t kWorldCellsX = 32, kWorldCellsY = 16, kWorldCellsZ = 32;
    static constexpr uint32_t kWorldCellStride = 2, kWorldHeader = 8;
    static constexpr uint32_t kWorldWords = kWorldHeader + kWorldCellsX * kWorldCellsY * kWorldCellsZ * (kWorldCellStride + 32);

    void CreatePipelines(IGraphicsDevice& gfx);
    // @p g's buffers (a failed one is logged and left invalid).
    void CreateGrid(Grid& g);
    void SetCameraOf(Grid& g, const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& viewMatrix,
                     float nearZ, float farZ, uint32_t screenW, uint32_t screenH, bool orthographic,
                     uint32_t lightCount);

    void UpdateWorldLightGrid(std::span<const ResolvedLight> lights);

    void UploadLightHistory(std::span<const ResolvedLight> lights, uint32_t frameSlot, const void* records);

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_buildPSO;
    RHI::PipelineState m_cullPSO;
    RHI::PipelineState m_probeCullPSO;

    // The lights buffer is UPLOAD and written every frame: a ring, one per frame in flight.
    RHI::GPUBuffer m_lightBuffer[kFrameCount];  // StructuredBuffer<GPULight>, UPLOAD ring
    void*          m_lightMapped[kFrameCount] = {};
    uint64_t       m_lightsSRV[kFrameCount]    = {};
    // Word hash of the records each slot received, computed while they are built: reading the
    // write-combined UPLOAD memory back to hash it costs ~1-2 ms per frame.
    uint64_t       m_lightFingerprint[kFrameCount] = {};
    // The fog scales beside the records (GetLightMediumScaleSRVHandle): written only into a slot whose
    // lights do not all scale by 1.
    RHI::GPUBuffer m_mediumScaleBuffer[kFrameCount];   // StructuredBuffer<float>, UPLOAD ring
    void*          m_mediumScaleMapped[kFrameCount] = {};
    uint64_t       m_mediumScaleSRV[kFrameCount]    = {};
    bool           m_mediumScaleUsed[kFrameCount]   = {};

    // Camera-independent conservative light lists for DDGI / reflected hits.
    // Overflow cells use the complete list, never truncate illumination.
    RHI::GPUBuffer m_worldLightBuffer[kFrameCount];
    void* m_worldLightMapped[kFrameCount] = {};
    uint64_t m_worldLightSlotVersion[kFrameCount] = {};
    uint64_t m_worldLightVersion = 0;
    std::vector<uint32_t> m_worldLightKey, m_worldLightGrid;

    // The main view's grid; m_g is the bound view's (BindView).
    Grid  m_mainGrid;
    Grid* m_g = &m_mainGrid;

    RHI::GPUBuffer m_counterBuffer;     // RWStructuredBuffer<uint>, DEFAULT; unused (no shader binds a counter)
    FrameCB<ClusterCB> m_clusterCB;     // UPLOAD, one per frame in flight
    RHI::GPUBuffer m_counterResetBuf;   // 4-byte UPLOAD 0 for a counter reset; unused

    uint64_t m_counterUAV     = 0;

    uint64_t m_probeBufferSRV = 0;     // SRV of GpuShared::ReflectionProbe buffer (set by Renderer)
    uint32_t m_probeCount     = 0;

    void* m_counterResetMapped = nullptr;

    // Lights v2 rings (see EnsureLightHistory) and the CPU-side copy of the
    // last upload they are built from.
    RHI::GPUBuffer m_lightMapBuffer[kFrameCount], m_prevLightBuffer[kFrameCount], m_lightAliasBuffer[kFrameCount];
    void* m_lightMapMapped[kFrameCount] = {};
    void* m_prevLightMapped[kFrameCount] = {};
    void* m_lightAliasMapped[kFrameCount] = {};
    uint64_t m_lightMapSRV[kFrameCount] = {}, m_prevLightSRV[kFrameCount] = {}, m_lightAliasSRV[kFrameCount] = {};
    LightHistory m_lightHistory[kFrameCount];
    uint32_t m_lightAliasCount[kFrameCount] = {};
    std::vector<uint32_t> m_prevKeys;       // keys of the last upload, in list order
    std::vector<uint8_t>  m_prevRecordBytes; // its GPU records (64 B each)
    std::atomic<bool> m_lightHistoryReset{false};
    ServiceRequest m_request;   // what OnService ensures
    uint64_t m_lightHistoryDigest = 0;

    uint32_t m_lightCount = 0;
    uint32_t m_activeLocalLightCount = 0, m_firstActiveLocalLightIndex = 0;
};
