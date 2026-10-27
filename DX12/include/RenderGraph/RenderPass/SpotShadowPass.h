#pragma once

// SpotShadowPass: the shared shadow-map atlas for spot lights with
// LightData::castsShadow. Texture2DArray<D32_FLOAT> tiers: 256 base 512^2 slices,
// 16 medium 1024^2 and 4 high 2048^2; a light's logical slice stays stable across
// tier moves. A bounded set of update views (kUpdateBudget) refreshes dirty slices
// each frame:
//   1. The Renderer collects the casting spot lights, assigns slices and builds each
//      light's view-projection (a perspective covering the cone).
//   2. This pass restores cached static depth into the selected slices and draws the
//      dynamic casters; static geometry is redrawn only after invalidation.
//   3. Lighting.ps and VolumetricRaymarch.cs sample the atlas with a comparison
//      sampler; a light with shadowSliceIdx == 0xFFFFFFFF skips it (no shadow).
// Draws with Shadow.ps and SpotShadow.vs (Shadow.vs plus the static / dynamic instance
// filter the caster CB selects at byte 232).
//
// Root-sig layout:
//   b1 space0 - ShadowPerViewCB (shadowViewProj for the current slice)
//   t0 space0 - InstanceBuffer
//   t1 space0 - MeshDescriptors
//   t2 space0 - MaterialBuffer (alpha-test)
//   t0 space1 - bindless g_Buffers[]
//   t0 space2 - bindless g_AllTextures[]

#include "Graphics/FrameConstants.h"
#include "Graphics/SpotShadowView.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IndirectDrawCommand.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class SpotShadowPass : public RG::RenderPass
{
public:
    static constexpr uint32_t kMaxCasters   = 256;
    static constexpr uint32_t kUpdateBudget = 48;
    static constexpr uint32_t kShadowMapSize = 512;
    static constexpr uint32_t kMediumCount = 16, kHighCount = 4;
    static constexpr uint32_t kInvalidSliceIdx = 0xFFFFFFFFu;

    // ---- Per-slice GPU-culled indirect args ----
    static constexpr uint32_t kShadowGroupCount = 8;
    struct RetainedShadowArgs
    {
        bool                  active = false;
        const RHI::GPUBuffer* workingArgs [kUpdateBudget] = {};
        const RHI::GPUBuffer* countBuffer = nullptr;   // (view x bucket) u32 grid
        uint32_t              remapHeapIdx[kUpdateBudget] = {};
        uint32_t              countBase   [kUpdateBudget] = {}; // element base into countBuffer
        uint32_t              groupBase[kShadowGroupCount] = {}; // command slots
        uint32_t              groupCap [kShadowGroupCount] = {};
    };

    struct ServiceRequest { bool wanted = false; };

    SpotShadowPass();
    ~SpotShadowPass();

    const char* GetName() const override { return "SpotShadowPass"; }
    // The three atlas tiers are this pass's imports (RG::PassTex::kShadowSpotAtlas*):
    // created here, transitioned by the graph; Lighting, VolumetricFog and the forward
    // passes declare reading them.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup (RG::RenderGraphBuilder& b) override;
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's two early-outs, including the one-shot DEPTHSTENCIL -> SRV
    // transition the first caster-less frame owes LightingPass (m_firstExecution).
    // A skipped frame does not advance m_telemetryFrame, so -spot_shadow_log_every
    // counts recording frames (see Execute).
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_atlas.IsValid() && (m_activeCount != 0 || m_firstExecution));
    }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame BEFORE graph Execute.
    // activeCount = number of update views this frame (<= kUpdateBudget).
    void SetActiveCasterCount(uint32_t n) { m_activeCount = std::min(n, kUpdateBudget); }
    bool IsSliceValid(uint32_t slice, const DirectX::XMFLOAT4X4& vp) const
    {
        return m_cachedValid[slice] &&
            std::memcmp(&m_cachedVP[slice], &vp, sizeof(vp)) == 0;
    }
    bool NeedsRefresh(uint32_t slice, uint64_t revision) const
    { return m_cachedRevision[slice] != revision || m_desiredTier[slice] != m_sliceTier[slice]; }
    void SetDesiredTier(uint32_t slice, uint32_t tier)
    { m_desiredTier[slice] = tier == 2 && m_highAtlas.IsValid() ? 2 :
                           tier >= 1 && m_mediumAtlas.IsValid() ? 1 : 0; }
    void TrimResidents(uint32_t count) {
        for (uint32_t i=count; i<kMaxCasters; ++i) {
            SetDormant(i);
        }
    }
    // Keep logical slots stable while a light is dark, but release quality slots.
    void SetDormant(uint32_t slice) {
        m_sliceTier[slice] = m_desiredTier[slice] = 0;
        m_cachedValid[slice] = m_staticValid[slice] = false;
    }
    bool CullDynamicOnly(uint32_t view) const {
        return m_honorShadowFlags && m_mixedAvailable && !m_rebuildStatic[view];
    }
    uint32_t TierCapacity(uint32_t tier) const {
        return tier==2 ? (m_highAtlas.IsValid() ? kHighCount:0) :
               tier==1 ? (m_mediumAtlas.IsValid() ? kMediumCount:0) : kMaxCasters;
    }
    uint32_t GetTier(uint32_t slice) const { return m_sliceTier[slice]; }
    SpotShadowViewGPU GetView(uint32_t slice) const;
    // Debug capture of one caster's map (Renderer's "spotShadow[N]" target): logical
    // slice N (a light's LightData.shadowSliceIdx) -> the tier atlas it lives in, with
    // its array slice there in @p physicalSlice. nullptr before the first execution or
    // for a slice that holds no valid map.
    const RHI::Texture* GetDebugSlice(uint32_t logicalSlice, uint32_t& physicalSlice, RHI::ResourceState& state) const
    {
        if (logicalSlice >= kMaxCasters || m_firstExecution || !m_cachedValid[logicalSlice]) return nullptr;
        const RHI::Texture& atlas = AtlasFor(logicalSlice);
        if (!atlas.IsValid()) return nullptr;
        physicalSlice = PhysicalSlice(logicalSlice);
        state = RHI::ResourceState::DEPTH_READ_SRV;
        return &atlas;
    }
    void QueueSlice(uint32_t view, uint32_t slice, const DirectX::XMFLOAT4X4& vp,
                    uint64_t revision, uint64_t staticRevision)
    {
        const uint32_t oldTier = m_sliceTier[slice];
        const bool sameView = IsSliceValid(slice, vp);
        AssignTier(slice);
        m_rebuildStatic[view] = !m_staticValid[slice] || !sameView ||
            oldTier != m_sliceTier[slice] || m_staticRevision[slice] != staticRevision;
        m_pendingStaticRevision[view] = staticRevision;
        m_updateSlices[view] = slice;
        m_pendingVP[view] = vp;
        m_cachedVP[slice] = vp;
        m_cachedValid[slice] = true;
        m_cachedRevision[slice] = revision;
    }
    // Scene time + camera for custom-VS (WPO) casters, the dissolve noise index and the
    // instance-remap heap index; written into each caster CB at byte offsets
    // 192/208/224/228 (custom_vertex.hlsli CUSTOM_VS_SHADOW, Shadow.ps ShadowPerViewCB).
    void SetCustomVsTime(float t, float dt, const DirectX::XMFLOAT3& camPos,
                         uint32_t dissolveNoiseIdx = ~0u,
                         uint32_t instanceRemapIdx = 0)
    { m_cvTime = t; m_cvDeltaTime = dt; m_cvCameraPos = camPos;
      m_dissolveNoiseIdx = dissolveNoiseIdx;
      m_instanceRemapIdx = instanceRemapIdx; }

    // Honor MaterialComponent CAST_SHADOW and the VisibilityComponent shadow viewMask
    // like CSM does (the default). false is the Renderer's compat mode: both are
    // ignored, so an opted-out caster still darkens every spot light.
    void SetHonorShadowFlags(bool b) { m_honorShadowFlags = b; }

    // The Renderer dispatches InstanceCull.cs mode 2 once per slice (the slice's own
    // cone frustum) at the head of the shadow command list and hands the results here.
    // Layout is the shared CSM one: ShadowDrawGroup buckets at fixed command-slot bases
    // inside each slice's working arg buffer, survivor counts in a (view x bucket) grid.
    // Index s is the update-view index (QueueSlice), not an atlas slice or a global
    // cull-view index: the Renderer folds its family base into countBase. Entries past
    // the active caster count stay null.
    // == ShadowDrawGroup::RetainedCount: the four buckets and their meshlet twins. This
    // family's views keep DRAW commands, so a twin draws exactly like its base bucket.
    void SetRetainedShadowArgs(const RetainedShadowArgs& ra) { m_ra = ra; }

    // Consumed by Lighting.ps / VolumetricRaymarch.cs. Returns 0 while the atlas is not
    // resident (see EnsureAtlas): every consumer needs a fallback.
    uint64_t                  GetAtlasSrvHandle() const;
    static constexpr uint32_t GetAtlasSize()       { return kShadowMapSize; }
    static constexpr uint32_t GetMaxCasters()      { return kMaxCasters; }

    // ---- Lazily allocated atlas (256 MiB) ----
    // 512^2 x 256 D32 slices is 256 MiB, so the atlas is created on demand at the frame
    // boundary (Renderer::ServiceEnableGatedResources -> OnService: main thread, after
    // BeginFrame, before any recording, the only point where creating / destroying
    // descriptors is legal) and released per world from OnWorldClear, never on an idle
    // timer.
    // @p wanted must come from a direct LightData pool scan, not this pass's caster
    // count: activeCount comes from a Renderer block that is itself gated on residency,
    // so deriving demand from it would deadlock at zero. EnsureAtlas only allocates;
    // ReleaseAtlas is the only free path (same split as SkyboxPass's stars cubemap).
    void EnsureAtlas(bool wanted);
    void ReleaseAtlas();
    uint32_t HandledEvents() const override { return kWorldClear; }   // events delivered by Renderer::PassSet
    // A new world gives the atlas back; the editor's Play / Stop (which restores the
    // same world two frames later) and a clear that keeps the world do not.
    void OnWorldClear(const RG::WorldClearArgs& a) override { if (!a.worldSurvives && !a.idsPreserved) ReleaseAtlas(); }

    // Service point: allocates when a caster wants the atlas, never releases (that is
    // per world: OnWorldClear).
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override { EnsureAtlas(m_request.wanted); }
    bool IsAtlasResident() const { return m_atlas.IsValid(); }

    // Builds now, on the calling thread (a preload's TaskSystem worker), the PSO(s)
    // Execute would build for @p dp, a material's synthetic packet: its group PSO
    // (alpha-test bucket, or the opaque one with Shadow_PS unless ShadowDrawNeedsPS lets
    // it go; cull is NONE for every bucket) or its per-draw custom-VS PSO. Mirrors the
    // record path's choice; the PSO cache is thread-safe (single flight). Nothing is
    // bound or drawn.
    void WarmPipelines(const DrawPacket& dp);

private:
    static constexpr uint32_t kCasterCBStride = RHI::kConstantBufferAlignment; // D3D12 CBV alignment
    struct CasterCBPool
    {
        // 256-byte slot per caster, padded to the full stride so the FrameCB<> size and the
        // per-caster byte-offset bind line up. Three banks of kUpdateBudget slots: bank one
        // carries the pristine instance-remap heap index at byte 228 and serves the ring
        // path and the per-draw custom-VS loop; bank two is the same bytes with the slice's
        // working remap index (the compacted ranges live in that remap) plus a dynamic-only
        // filter at 232 when mixed caching is active; bank three is the working remap plus a
        // static-only filter, for cache rebuilds.
        uint8_t slots[3 * kUpdateBudget * kCasterCBStride];
    };

    static constexpr uint32_t kMaxIndirectCommands = 32768;
    static constexpr uint32_t kFrameCount          = RHI::kFramesInFlight;

    // @p depthOnly drops the PS (PSODesc::kNoPixelShader): legal only when no packet in
    // the group can discard (ShadowDrawNeedsPS). @p hookPSID != 0: a masked custom
    // material's own PS in its SHADOW_HOOK variant, fed by its custom VS or
    // ShadowHook_VS (ShadowCasterShaders.h).
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customVSID = 0,
                         bool depthOnly = false, uint32_t hookPSID = 0) const;
    void    UploadCasterCBs();
    void AssignTier(uint32_t slice);
    RHI::Texture& AtlasFor(uint32_t slice)
    { return m_sliceTier[slice] == 2 ? m_highAtlas : m_sliceTier[slice] == 1 ? m_mediumAtlas : m_atlas; }
    const RHI::Texture& AtlasFor(uint32_t slice) const
    { return m_sliceTier[slice] == 2 ? m_highAtlas : m_sliceTier[slice] == 1 ? m_mediumAtlas : m_atlas; }
    uint32_t PhysicalSlice(uint32_t slice) const { return m_sliceTier[slice] ? m_tierSlot[slice] : slice; }

    RHI::Texture& StaticAtlasFor(uint32_t slice) {
        return m_sliceTier[slice] == 2 ? m_staticHighAtlas :
               m_sliceTier[slice] == 1 ? m_staticMediumAtlas : m_staticAtlas;
    }

    ServiceRequest m_request;   // filled by the Renderer, read by OnService
    RG::RGTextureHandle m_atlasH[3];   // the tiers' imports: base, medium, high

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // Custom-VS (WPO) caster time + camera (caster CB offsets 192 / 208).
    float             m_cvTime      = 0.0f;
    float             m_cvDeltaTime = 0.0f;
    DirectX::XMFLOAT3 m_cvCameraPos = { 0.0f, 0.0f, 0.0f };
    // Dissolve noise bindless index (caster CB offset 224; ~0u = none).
    uint32_t          m_dissolveNoiseIdx = ~0u;
    // Instance-remap heap index (caster CB offset 228; 0 = identity).
    uint32_t          m_instanceRemapIdx = 0;

    RHI::Texture m_mediumAtlas, m_highAtlas;
    // Matching depth-only backing atlases. Kept in DEPTHSTENCIL between frames;
    // copied into the sampled atlas before drawing this frame's dynamic casters.
    RHI::Texture m_staticAtlas, m_staticMediumAtlas, m_staticHighAtlas;
    bool m_mixedAvailable = false;
    bool m_staticValid[kMaxCasters]{};
    uint64_t m_staticRevision[kMaxCasters]{};
    bool m_rebuildStatic[kUpdateBudget]{};
    uint64_t m_pendingStaticRevision[kUpdateBudget]{};

    uint32_t m_desiredTier[kMaxCasters]{};
    uint32_t m_sliceTier[kMaxCasters]{};
    uint32_t m_tierSlot[kMaxCasters]{};
    RHI::Texture m_atlas;  // Texture2DArray<D32_FLOAT>, kMaxCasters slices; see EnsureAtlas
    // Create-failure latch. Without it EnsureAtlas retries a 256 MiB committed
    // allocation EVERY frame while a caster exists and the atlas does not;
    // ENGINE_CHECK_ONCE throttles only the log line, not the allocation
    // attempt. Cleared by ReleaseAtlas so a new world gets one fresh retry.
    bool m_allocFailed = false;

    // One CB slot per caster: a transposed float4x4 plus the custom-VS / dissolve /
    // remap words, bound with BindConstantBufferAtOffset at `i * kCasterCBStride`.
    // FrameCB keeps one pool per frame in flight, so frame N+1's CPU upload cannot race
    // frame N's GPU read.
    FrameCB<CasterCBPool> m_casterCBs;

    DirectX::XMFLOAT4X4 m_pendingVP[kUpdateBudget]{};
    DirectX::XMFLOAT4X4 m_cachedVP[kMaxCasters]{};
    bool m_cachedValid[kMaxCasters]{};
    uint64_t m_cachedRevision[kMaxCasters]{};
    uint32_t m_updateSlices[kUpdateBudget]{};
    uint32_t            m_activeCount = 0;

    // Indirect draw arg buffers for batching opaque draws per slice (same layout as
    // ShadowPass / GBufferPass). 32768 because Bistro-scale scenes need >20 k spot
    // shadow draws (see ShadowPass.h). A manual ring of kFrameCount buffers (UPLOAD
    // heap, no CB bind flag, so FrameCB<> does not fit).
    RHI::GPUBuffer m_indirectArgBuffer[kFrameCount];
    void*          m_indirectArgMapped[kFrameCount] = {};

    IGraphicsDevice* m_gfxPtr = nullptr;

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer.ps's g_LinearWrap).
    int              m_alphaSamplerIdx = -1;

    // True until the first Execute after each atlas allocation: the atlas starts in its
    // creation state (DEPTHSTENCIL); later frames arrive in DEPTH_READ_SRV.
    bool m_firstExecution = true;
    uint64_t m_telemetryFrame = 0;

    // Honor castShadow + shadow viewMask (see SetHonorShadowFlags).
    bool m_honorShadowFlags = true;

    // Per-frame retained-args state (see RetainedShadowArgs).
    RetainedShadowArgs m_ra;
};
