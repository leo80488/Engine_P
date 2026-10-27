#pragma once

// ShadowPass - the sun's cascaded shadow map.
//
// Renders the opaque DrawPackets into cascades 0..2 and the terrain alone into the
// ultra-far cascade 3 (kCascadeCount = 4), one slice each of a single NxNx4 D32_FLOAT
// Texture2DArray (N = GetShadowResolution(), setting shadows.csmResolution, default
// kShadowMapSize 4096). The RHI creates a DSV per slice, so each cascade is cleared
// and rendered independently. Renderer records it on its own command list outside
// the graph; the main graph imports the array as RG::PassTex::kShadowCSM.
//
// LightingPass samples the array's SRV at Gfx::ShadowSRV (t9 space0) via
// float3(uv, sliceIdx). The wired ShadowSystem (SetShadowSystem) supplies the
// per-cascade view-projections each frame; each cascade's UPLOAD CB is bound at
// b1 space0 (PerViewCB's slot) while the shadows draw.
//
// Root signature slots used (Graphics/RootSignatureLayout.h):
//   b0      PushConstants (meshDescIdx, instanceOffset, materialIndex)
//   b1      ShadowPerViewCB (per-cascade shadowViewProj)
//   t0      InstanceBuffer
//   t1      MeshDescriptors
//   t9      Shadow Texture2DArray SRV (produced by this pass, consumed by LightingPass)

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IndirectDrawCommand.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's arg ring)

#include <atomic>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class ShadowSystem;
class TerrainPass;

class ShadowPass : public RG::RenderPass
{
public:
    // Cascades 0..2 are the near CSM cascades; cascade 3 is the terrain-only
    // ultra-far cascade out to ShadowSystem::kFarCascadeFar in view-Z (split layout:
    // ShadowSystem; terrain-only render path: Execute).
    static constexpr int      kCascadeCount  = 4;
    static constexpr int      kFarCascadeIdx = kCascadeCount - 1;
    // Default CSM resolution per cascade: 4096^2 x 4 cascades x 4 B (D32) = 256 MB
    // (2048 would be 64 MB). Receiver biases are resolution-adaptive, so this is a
    // safe knob. The runtime value is GetShadowResolution(): consumers read that,
    // not this constant.
    static constexpr uint32_t kShadowMapSize = 4096;

    // Cascades that consume per-cascade GPU-culled args. Cascade kFarCascadeIdx is
    // terrain-only (no packet draws), so only 0..2 get cull views.
    static constexpr uint32_t kRetainedCullCascades = 3;
    struct RetainedShadowArgs
    {
        bool                  active = false;
        const RHI::GPUBuffer* workingArgs[kRetainedCullCascades] = {};
        const RHI::GPUBuffer* countBuffer = nullptr; // (cascade x group) u32 grid
        uint32_t              remapHeapIdx[kRetainedCullCascades] = {};
        // The retained layout: the four buckets and their meshlet twins
        // (ShadowDrawGroup::MeshletOffset).
        uint32_t              groupBase[ShadowDrawGroup::RetainedCount] = {};  // command slots
        uint32_t              groupCap [ShadowDrawGroup::RetainedCount] = {};
        // meshlet: the twins hold DISPATCH_MESH commands this frame; meshletBanks = the
        // per-cascade MeshletCullCB (b4 at cascade * 256); frame tags the statistics.
        bool                  meshlet = false;
        const RHI::GPUBuffer* meshletBanks = nullptr;
        uint32_t              frame = 0;
    };

    struct MeshletShadowStats
    {
        bool     valid = false;
        uint32_t frame = 0;                 // the frame that recorded them
        uint32_t counters[kRetainedCullCascades][16] = {};
        uint32_t twinGroups = 0;            // meshlet twin draws recorded (all cascades)
        uint32_t cpuFallback = 0;           // twin draws taken on the CPU (mesh PSO unavailable)
    };

    ShadowPass();
    ~ShadowPass();

    const char* GetName() const override { return "ShadowPass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx)      override;
    // The pass events Renderer::PassSet delivers; it records any view's cascades (its arg ring is the view's).
    uint32_t HandledEvents() const override { return kReloadShaders | kView; }
    // The main view keeps the indirect-arg ring below; a secondary view its own in its ViewState (made when the
    // view is first bound, at a frame boundary): its Execute writes this frame slot's args after the main
    // view's, before the GPU has read those. The cascade array itself is shared - each view redraws it.
    void BindView(ViewState& view) override;

    // Wire the TerrainPass that owns the active terrain's heightmap SRV +
    // dispatch tile count. Set once after Compile(); ShadowPass dispatches
    // a depth-only mesh-shader pass per cascade using these bindings.
    // Pass nullptr (or never call) to disable terrain shadow casting.
    void SetTerrainPass(TerrainPass* tp) { m_terrainPass = tp; }

    // The per-frame TerrainParams CB owned by Renderer (rebound each frame). Required
    // with SetTerrainPass for terrain shadows: ShadowPass records outside the graph,
    // so the graph-published "TerrainParams" binding is not visible here and
    // RenderTerrainShadow binds it directly, like the cascade CBs.
    void SetTerrainParamsCB(const RHI::GPUBuffer* cb) { m_terrainParamsCB = cb; }

    // Runtime per-cascade resolution (default kShadowMapSize), player-tunable via
    // settings.json "shadows.csmResolution". Set it BEFORE Init(): the Texture2DArray
    // is created there and never resized, so a change takes effect at the next
    // Compile() / restart. 0 restores the default.
    void     SetShadowResolution(uint32_t s) { m_shadowMapSize = s ? s : kShadowMapSize; }
    uint32_t GetShadowResolution() const     { return m_shadowMapSize; }

    // Wire the ShadowSystem that supplies per-cascade lightVP matrices.
    // Must be called once after Init(), before Execute(); a secondary view's segment points it at the
    // view's fit and back.
    void SetShadowSystem(const ShadowSystem* sys) { m_sys = sys; }
    const ShadowSystem* GetShadowSystem() const { return m_sys; }

    // When true, packets tagged DrawPacket::perObjectShadowCaster are SKIPPED in the
    // CSM, so a character is not in both maps (the shadow would step where the
    // per-object frustum ends). Renderer sets it to "PerObjectShadowPass is active
    // this frame", so a character falls back to CSM casting whenever the per-object
    // map cannot render it. The CSM still RECEIVES on those pixels (world -> character
    // shadows).
    void SetExcludePerObjectCasters(bool b) { m_excludePerObjCasters = b; }

    // GPU SRV handle of the cascade Texture2DArray; LightingPass binds it at
    // Gfx::ShadowSRV (kShadowSRVSlot).
    uint64_t GetShadowArrayGpuHandle() const;
    // The cascade array itself, for the main graph's import (RG::PassTex::kShadowCSM):
    // the member's address, valid as long as this pass is - the graph reads it at
    // every Execute.
    const RHI::Texture* CascadeArray() const { return &m_shadowArray; }
    // A frame that skips Execute (FramePlan's csm = false): an array no Execute has
    // written yet is still in its creation state (DEPTHSTENCIL) with undefined
    // contents. This clears every cascade to "no occluder" and leaves it where
    // Execute does (DEPTH_READ_SRV), once; after any Execute it does nothing.
    void PrepareUnwritten(RHI::CommandList cl);
    // The cascade array as the debug-target registry reads it (csm0..csm2 =
    // its slices); nullptr until the first Execute. Between frames it sits in
    // DEPTH_READ_SRV (Execute's last transition).
    const RHI::Texture* GetDebugOutput(RHI::ResourceState& state) const
    {
        if (!m_shadowArray.IsValid() || m_firstExecution) return nullptr;
        state = RHI::ResourceState::DEPTH_READ_SRV;
        return &m_shadowArray;
    }

    // The shader library, so the Renderer can RegisterDynamic the same custom vertex
    // shaders here (compiled in CUSTOM_VS_SHADOW mode) as in the GBuffer pass: the
    // dynamic IDs stay in lockstep, so a DrawPacket's customVSID resolves to the same
    // shader in this pass's PSOCache.
    ShaderLibrary& GetShaderLibrary() { return m_shaderLib; }

    // Per-frame scene time + camera for custom-VS (WPO) casters, the engine-global
    // dissolve noise bindless index and the instance-remap heap index (@p
    // instanceRemapIdx, 0 = identity). Written into each cascade CB at bytes
    // 192/208/224/228 so the shadow VS deforms like the GBuffer VS and Shadow.ps
    // clips dissolving casters. Set by Renderer.
    void SetCustomVsTime(float t, float dt, const DirectX::XMFLOAT3& camPos,
                         uint32_t dissolveNoiseIdx = ~0u,
                         uint32_t instanceRemapIdx = 0)
    { m_cvTime = t; m_cvDeltaTime = dt; m_cvCameraPos = camPos;
      m_dissolveNoiseIdx = dissolveNoiseIdx;
      m_instanceRemapIdx = instanceRemapIdx; }

    // ---- Shadow-family retained args ----
    // Renderer-owned per-frame consumption state, pushed BEFORE every Execute.
    // active = true -> the group draws read the per-cascade working arg buffers
    // (compacted by InstanceCull.cs mode 2, recorded by Renderer at the head of THIS
    // pass's command list) with GPU survivor counts; the per-frame UPLOAD arg ring is
    // not touched. active = false -> the CPU ring path, the permanent fallback.
    // groupBase / groupCap follow the ShadowDrawGroup bucket order (== this pass's
    // DrawGroup enum); caps are the CPU-classified per-bucket command counts (the
    // ExecuteIndirect capacity; the GPU count is always <= cap).
    void SetRetainedShadowArgs(const RetainedShadowArgs& ra) { m_ra = ra; }

    // ---- CSM meshlet path ----
    // A meshlet twin bucket draws through ShadowMeshlet.as/.ms with its base bucket's
    // permutation, cull mode and PS choice. CAPABLE: set by the Renderer before Init
    // on a device with mesh shaders (and no -meshletsoff); only then are the mesh PSOs
    // pre-warmed and the statistics created.
    void SetMeshletCapable(bool v) { m_meshletCapable = v; }
    // The pre-warmed mesh PSO; the Renderer's shadow latch requires it.
    bool MeshletPsoReady() const { return m_meshletPsoReady.load(std::memory_order_acquire); }
    // The CSM statistics UAV's heap index for the banks (kMeshletNoData when absent).
    uint32_t MeshletStatsUavIndex(IGraphicsDevice& gfx) const;

    // The last CSM statistics read back (~kFramesInFlight frames old): 16
    // counters per cascade (meshlet_shared.hlsli kMeshletStat* layout).
    MeshletShadowStats GetMeshletShadowStats() const
    {
        std::lock_guard<std::mutex> lk(m_mlStatsMutex);
        return m_mlStatsLast;
    }

    // ---- PSO warm-up ----
    // Builds NOW, on the calling thread (a preload's TaskSystem worker), the PSO(s)
    // Execute would build for @p dp, a material's synthetic packet: its indirect-group
    // PSO (ClassifyShadowDrawGroup's bucket, with Shadow_PS unless ShadowDrawNeedsPS
    // lets the group drop it) or its per-draw custom-VS / hook PSO. Mirrors the record
    // path's choice; the PSO cache is thread-safe (single flight). Nothing is bound or
    // drawn.
    void WarmPipelines(const DrawPacket& dp);

private:
    static constexpr uint32_t kCascadeCBStride = RHI::kConstantBufferAlignment; // D3D12 CBV alignment
    // Two banks of kCascadeCount slots: bank two carries each cascade's WORKING remap
    // heap index (m_cascadeCBs).
    struct CascadeCBPool
    {
        uint8_t slots[2 * kCascadeCount * kCascadeCBStride];
    };

    // Sized for Bistro's > 20 k shadow casters: 32 k x 32 B = 1 MB per arg buffer.
    // Past the cap the tail of each over-running group is dropped (Execute warns once).
    static constexpr uint32_t kMaxIndirectCommands = 32768;
    static constexpr uint32_t kFrameCount          = RHI::kFramesInFlight;

    // The PSO desc of a shadow draw with permutation @p perm (alpha test on/off) and
    // rasterizer @p cullMode; PSOCache hashes the full desc, so each combination gets
    // its own PSO. @p customVSID != 0 swaps Shadow_VS for a material's custom VS in
    // its depth-only CUSTOM_VS_SHADOW variant, so World-Position-Offset reaches the
    // shadow map. @p depthOnly drops the PS (PSODesc::kNoPixelShader) - legal ONLY when
    // no packet in the group can discard (ShadowDrawNeedsPS). @p hookPSID != 0: a
    // masked custom material's own PS in its SHADOW_HOOK variant, fed by its custom VS
    // or ShadowHook_VS (ShadowCasterShaders.h).
    PSODesc BuildPSODesc(PermutationKey perm, RHI::CullMode cullMode,
                         uint32_t customVSID = 0, bool depthOnly = false,
                         uint32_t hookPSID = 0) const;
    // BuildPSODesc's exact state (cull, depth bias, depth clip off, reversed-Z test)
    // with ShadowMeshlet.as/.ms instead of Shadow.vs, so a meshlet twin writes the
    // depth its VS bucket would.
    PSODesc BuildMeshletPSODesc(PermutationKey perm, RHI::CullMode cullMode, bool depthOnly) const;
    // A twin bucket's mesh PSO, or nullptr (the twin's packets then draw on the
    // CPU through the VS: its working args are DISPATCH_MESH-shaped).
    const RHI::PipelineState* MeshletPso(PermutationKey perm, RHI::CullMode cullMode, bool depthOnly);
    bool PrewarmMeshletPSO();
    void CreateMeshletStatsBuffers(IGraphicsDevice& gfx);
    void HarvestMeshletStats(IGraphicsDevice& gfx);

    void    UploadCascadeCBs();    // fills this frame's cascade CBs from ShadowSystem's matrices

    // Build (or rebuild) the terrain shadow PSO. Called from Init and from
    // ReloadShaders so the depth-bias state stays in sync with the main
    // shadow PSOs.
    bool BuildTerrainShadowPSO(IGraphicsDevice& gfx);
    // Per-cascade terrain dispatch: binds the cascade's CB at b1 (the draw loop may
    // not have, with no DrawPackets) plus the terrain resources, then DispatchMesh,
    // on the shared graphics root signature the rest of the pass uses. No-op without
    // m_terrainPass, the terrain PSO, the TerrainParams CB or a GPU-resident heightmap.
    void RenderTerrainShadow(RHI::CommandList cl, int cascadeIdx);

    bool                  m_meshletCapable = false;
    std::atomic<bool>     m_meshletPsoReady{ false };
    // -meshletgroupcpu (harness): every twin takes the CPU fallback, as a twin
    // whose mesh PSO is unavailable would (GBufferPass reads the same flag).
    bool                  m_forceTwinCpu = false;
    RHI::GPUBuffer        m_mlStatsBuffer;                       // 3 x 16 u32, UAV
    RHI::GPUBuffer        m_mlStatsZero;                         // UPLOAD zeros
    RHI::GPUBuffer        m_mlStatsReadback[RHI::kFramesInFlight];
    bool                  m_mlStatsPending[RHI::kFramesInFlight] = {};
    uint32_t              m_mlStatsFrame[RHI::kFramesInFlight] = {};
    uint32_t              m_mlTwinGroups[RHI::kFramesInFlight] = {};
    uint32_t              m_mlCpuFallback[RHI::kFramesInFlight] = {};
    mutable std::mutex    m_mlStatsMutex;
    MeshletShadowStats    m_mlStatsLast;

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // Custom-VS (WPO) caster time + camera, written into every cascade CB at bytes
    // 192 / 208 (SetCustomVsTime, UploadCascadeCBs).
    float               m_cvTime      = 0.0f;
    float               m_cvDeltaTime = 0.0f;
    DirectX::XMFLOAT3   m_cvCameraPos = { 0.0f, 0.0f, 0.0f };
    // Dissolve noise bindless index, written at cascade CB offset 224 (~0u = none).
    uint32_t            m_dissolveNoiseIdx = ~0u;
    // Instance-remap heap index, written at cascade CB offset 228 (0 = identity).
    uint32_t            m_instanceRemapIdx = 0;

    // Single Texture2DArray depth resource (m_shadowMapSize^2 x kCascadeCount). The
    // RHI creates per-slice DSVs for arraySize > 1 with DEPTH_STENCIL;
    // IGraphicsDevice::SetDepthStencilSlice / ClearDepthStencilSlice use them.
    RHI::Texture m_shadowArray;

    // Actual per-cascade resolution used to create m_shadowArray (see
    // SetShadowResolution). Immutable after Init(); LightCB.shadowMapTexelSize
    // (the PCF kernel step) is derived from this exact value in Renderer.
    uint32_t m_shadowMapSize = kShadowMapSize;

    // Per-cascade constant buffers (UPLOAD heap; float4x4 shadowViewProj + 6 frustum
    // planes, then the WPO / dissolve / remap tail at bytes 192..231), one 256-byte
    // slot per cascade in one triple-buffered FrameCB pool, so CPU writes do not race
    // GPU draws still reading last frame's data. The pool has two banks: slot
    // [kCascadeCount + i] duplicates cascade i except byte 228, which carries the
    // cascade's per-view WORKING remap heap index instead of the pristine one.
    // Retained group draws bind bank two (their compacted commands' instance ranges
    // live in the per-cascade remap); custom-VS / terrain / fallback draws keep bank
    // one (full pristine ranges).
    FrameCB<CascadeCBPool> m_cascadeCBs;

    // Per-frame retained-args state (SetRetainedShadowArgs).
    RetainedShadowArgs m_ra;

    // Source of cascade matrices. Non-owning; lifetime managed by Renderer.
    const ShadowSystem* m_sys = nullptr;

    // The device, needed by the destructor.
    IGraphicsDevice* m_gfxPtr = nullptr;

    // The array is created in DEPTHSTENCIL; after the first Execute (or
    // PrepareUnwritten) it sits in DEPTH_READ_SRV between frames.
    bool m_firstExecution = true;

    // Skip per-object (character) casters this frame: the high-res
    // PerObjectShadowPass renders them. Set per frame by the Renderer.
    bool m_excludePerObjCasters = false;

    // ExecuteIndirect arg ring for the batched shadow draws (UPLOAD heap, filled per
    // frame, one buffer per frame in flight; no CB bind flag, so not a FrameCB<>),
    // kMaxIndirectCommands commands each.
    struct ArgRing
    {
        RHI::GPUBuffer buffer[kFrameCount];
        void*          mapped[kFrameCount] = {};
    };
    struct ViewArgs : ViewData
    {
        ArgRing ring;
        void Release(IGraphicsDevice& gfx) override;
    };
    void CreateArgRing(IGraphicsDevice& gfx, ArgRing& ring);
    static void DestroyArgRing(IGraphicsDevice& gfx, ArgRing& ring);
    // The main view's ring; m_args is the bound view's (BindView).
    ArgRing  m_mainArgs;
    ArgRing* m_args = &m_mainArgs;

    // ---- Terrain shadow casting (mesh-shader path) ----
    // The terrain is dispatched per cascade after the DrawPacket draws, with a
    // depth-only PSO (Terrain_Shadow_AS/MS, no PS) and the same per-cascade
    // ShadowPerViewCB the rest of the pass binds at b1.
    TerrainPass*           m_terrainPass             = nullptr;   // non-owning
    const RHI::GPUBuffer*  m_terrainParamsCB         = nullptr;   // owned by Renderer
    RHI::PipelineState     m_terrainShadowPSO;

    // Run-time sampler for custom casters' vertex hooks (their g_LinearWrap).
    int                    m_alphaSamplerIdx         = -1;        // linear-wrap
};
