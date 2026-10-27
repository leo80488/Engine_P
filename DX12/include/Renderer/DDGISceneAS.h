#pragma once

// DDGISceneAS - the scene acceleration-structure cache shared by DDGI, the RT screen passes and
// the path tracer.
//
// Owns:
//   - the per-mesh BLAS cache (keyed by library handle + meshId); it survives world reloads,
//     PruneStaleBLAS reclaims stale entries.
//   - a per-entity deformable BLAS pool for skinned meshes, refit or rebuilt every frame from
//     the SkinnedVertexRing.
//   - one scene-wide TLAS built from the gathered instances, and the per-instance record
//     tables the hit shaders index by InstanceID().
//   - a reusable scratch buffer (sized for the largest build seen this run).
//
// Per frame: Gather (game side, reads the World), then BuildOrRefit (render side, records the
// build). The first build after OnWorldClear is a full rebuild; later ones refit the TLAS
// (PERFORM_UPDATE) when only transforms changed.
//
// Builds through IGraphicsDevice's acceleration-structure API on an RHI command list - no DX12
// type. Renderer-internal; no pass touches this class directly.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialRuntime.h"
#include "Graphics/FoliageWind.h"   // Pending::wind
#include <DirectXMath.h>
#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"

#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <atomic>

// DDGI ground-truth diagnostics. 0 = the per-gather counters below are compiled out. Mirrors
// DDGIVolumeManager.h.
#ifndef DDGI_GT_DIAGNOSTICS
#define DDGI_GT_DIAGNOSTICS 0
#endif
#include <cstdint>

class IGraphicsDevice;
class World;
class Renderer;
class MaterialSurfaceCache;
class MeshManager;
class SkinnedMeshSubsystem;
namespace Resource { class MeshLibrary; }

class DDGISceneAS
{
public:
    // Resets TLAS freshness and the retained gather. The BLAS cache survives: MeshLibrary
    // path-dedupes its slots, so the keys stay valid across a reload; PruneStaleBLAS reclaims the
    // entries whose meshlib slot Renderer's two-stage release freed.
    // @p idsPreserved: true only on the Play -> Stop edge, where the same world is restored from a
    // snapshot and entity ids still name the same skinned mesh. It keeps the per-entity skinned BLAS
    // allocations (reallocating them dominated the Stop hitch) while still forcing a full rebuild.
    // Leave it false for real scene changes: ids recycle against other meshes.
    void OnWorldClear(IGraphicsDevice& gfx, bool idsPreserved = false);

    // Per-entity destroy (from Renderer's destroy listener, mid-session). Only records it:
    // ApplyDestroyed, at the top of BuildOrRefit and before the skinned BLAS reuse test, resets the
    // entity's builtOnce so its next build is a full rebuild (the allocation is reused). The reuse
    // test compares only vertex and index counts, so a recycled id on a different mesh with matching
    // counts would otherwise be refit against the old topology - the DXR traversal-hang class of bug.
    // Not applied inline: the listener runs on whatever thread called World::DestroyEntity, and the
    // build half iterates and mutates m_skinnedBlas.
    void OnEntityDestroyed(Entity e);
    void ApplyDestroyed();

    // Defer-releases the BLAS cache entries whose MeshLibrary slot is no longer alive (refCount 0 ->
    // FreeSlot bumped the generation). Without it A -> B -> A world cycling leaks ~200 MB per round
    // trip in Bistro-scale worlds: the cache keeps a BLAS resident after its source VB/IB went away.
    // Call from Renderer::OnWorldClear after the m_meshLibsPendingRelease drain - the moment a slot
    // can drop to refCount 0 and become detectable here.
    // MeshHandle (procedural, sourceKind 1) entries are not pruned: MeshHandle stores a raw uint32 id
    // with no generation, so staleness is not observable, and those BLAS are tiny (cube / sphere).
    void PruneStaleBLAS(Resource::MeshLibrary* meshLib, IGraphicsDevice& gfx);

    // ---- Two halves (game / render thread split) ----
    // Gather is the only half that touches the World: it walks @p world for mesh entities
    // (MeshLibRef or MeshHandle + GlobalTransform) and records the rows BuildOrRefit consumes. Call
    // it on the game side after Renderer::BuildRenderScene - the definition says why that seam and no
    // earlier one.
    // When @p includeSkinned is true (an RT screen feature is active) and @p skin is initialised,
    // skinned meshes also enter the TLAS: each mesh entity that got a skin dispatch this frame gets a
    // per-entity BLAS built in place from its SkinnedVertexRing slice (object-space post-skin
    // positions, float3 stride 12).
    // Instance masks are bit-partitioned: bit0 = reflections / RTAO (main-pass visible), bit1 = RT
    // sun-shadow caster, bit2 = DDGI probe rays (statics only: deforming geometry destabilises probe
    // relocation / backface tests for a negligible GI gain), bit3 = front-layer reflections.
    // Skinned instances never carry bit2.
    void Gather(World&                        world,
                IGraphicsDevice&                 gfx,
                Resource::MeshLibrary*        meshLib,
                MeshManager*                  meshMgr,
                const IMaterialRuntimeLookup* materialRuntime,
                SkinnedMeshSubsystem*         skin           = nullptr,
                bool                          includeSkinned = false);

    // Records the AS build onto @p cmd (a graphics or compute queue list): missing BLASes, skinned
    // BLAS refits / rebuilds, the TLAS refit or rebuild. No World& and no pool access: it consumes
    // what Gather produced. @p allowSkinned filters the already-gathered skinned rows rather than
    // gating the gather, because it depends on command lists that only exist once Render() is under
    // way. Rebuilding in place on this list is safe against earlier frames' traces: the AS chain
    // (asCL -> ddgiCL) fence-waits demoteCL, which the graphics queue orders after frame N-1's RT
    // dispatches.
    // Returns the TLAS GPU VA, or 0 when the build was skipped (no geometry, no DXR, an
    // unrecoverable error). @p cmd carries nested GPU timestamp regions:
    //   AS.StaticBLAS  - cold / new static BLAS builds (budgeted, usually 0)
    //   AS.SkinnedBLAS - per-frame deformable BLAS refit / rebuild
    //   AS.Compact     - BLAS compaction copies
    //   AS.TLAS        - instance upload + top-level build
    uint64_t BuildOrRefit(IGraphicsDevice& gfx,
                          RHI::CommandList cmd,
                          // Scene time driving the foliage-wind TLAS
                          // shear. Must equal PerViewCB.timeParams.x this
                          // frame (Renderer::m_globalTimeSec) or the RT
                          // shadow slides out of phase with its leaves.
                          float            timeSec = 0.0f,
                          bool             allowSkinned = false);

    // True when the cached TLAS resource is allocated and last-built non-empty.
    bool IsValid() const { return m_tlas.as.IsValid() && m_tlas.instanceCount > 0; }

    // Render owner only, after BuildOrRefit. A hash of the published prefix of the instance and
    // alpha record masters (allocated at a power-of-two capacity), a word at a time, computed on
    // demand: PT accumulation depends on these records as well as on the TLAS. Compare it only with
    // this function's previous result. A skinned row's ring-slot fields (which of the
    // SkinnedVertexRing's per-frame buffers it reads) are left out: the same pose reads the same
    // bytes from any slot, and ContentFingerprint covers the pose.
    uint64_t ShadingFingerprint() const;
    uint64_t GetPathMaterialSrv(IGraphicsDevice& gfx) const;
    void ReleasePathMaterials(IGraphicsDevice& gfx);
    // BlendMode::Transmissive rows of the last build (a superset of its TLAS instances): the ReSTIR PT
    // bridge makes glass a path vertex only when there is any.
    uint32_t TransmissiveInstanceCount() const { return m_transmissiveCount; }
    // ReSTIR GI emitter table: every TLAS instance flagged
    // RT_INST_FLAG_GI_EMITTER (it emits at a GI hit), with its transforms,
    // InstanceID, triangle count and a radiance estimate. Built each
    // BuildOrRefit while wanted; the per-emitter area/CDF table beside it is
    // filled on the GPU by RTGI's EmitterArea/EmitterCdf entries.
    void SetGIEmittersWanted(bool wanted) { m_giEmittersWanted = wanted; }
    void ReleaseGIEmitters(IGraphicsDevice& gfx);
    uint32_t GetGIEmitterCount() const { return m_giEmitterCount; }
    uint64_t GetGIEmitterSrv(IGraphicsDevice& gfx) const;
    uint64_t GetGIEmitterTableUav(IGraphicsDevice& gfx) const;
    uint64_t PathMaterialFingerprint() const { return m_pathMaterialHash; }
    uint64_t TlasFingerprint() const { return m_lastInstanceHash; }
    bool TlasFingerprintValid() const { return m_tlasFresh && IsValid() && m_lastHashValid; }
    // The path tracer's view of the TLAS content: the instance descs and, with skinned instances,
    // each one's deformation hash (Pending::deformHash). Unlike TlasFingerprint it stays valid with
    // skinned content as long as every skinned row was hashed, so a character whose pose does not
    // change does not restart the accumulation. Render owner only, after BuildOrRefit.
    uint64_t ContentFingerprint() const { return m_contentHash; }
    bool ContentFingerprintValid() const { return m_tlasFresh && IsValid() && m_contentHashValid; }
    // The path tracer reads ContentFingerprint: Gather hashes the skinned rows' deformation and
    // BuildOrRefit hashes the instance descs with skinned content too. Set on the render lane, read
    // by the next Gather (one frame late, which the PT's restart-on-activation covers).
    void SetDeformFingerprintWanted(bool wanted) { m_deformWanted.store(wanted, std::memory_order_relaxed); }
    bool StaticBlasAllResolved() const { return m_tlasFresh && IsValid() && m_staticBlasAllResolved; }
    // Bumped once each time the static BLAS set goes from building to complete (a world's first
    // ~16-per-frame burst, or static meshes that arrive later): from that frame the TLAS holds every
    // static occluder. World-space caches that stored light traced through a slab whose BLAS had not
    // landed yet (SHaRC) clear on a change.
    // Written by the TLAS build (the render lane), read at the top of the game half.
    uint32_t StaticGeometryEpoch() const { return m_staticGeometryEpoch.load(std::memory_order_acquire); }

    // The TLAS GPU VA, the value the last BuildOrRefit returned; 0 before the first build.
    uint64_t GetTLASAddress() const { return m_tlas.GPUAddress(); }

    // Per-instance data table: closest-hit reads g_DDGIInstances[InstanceID()] for albedo, emissive,
    // bindless VB/IB slots, geometry offsets and the RTR hit-shading material proxy (albedo / surface
    // texture indices, roughness / metalness remap, reflectance). Layout: struct DDGIInstanceData,
    // 128 bytes (the static_assert in DDGISceneAS.cpp is the authority); keep its three mirrors in
    // sync: DDGISceneAS.cpp (CPU), DDGIRayTrace.cs.hlsl, HybridReflIntersectHW.cs.hlsl
    // (HRInstanceData). One resident DEFAULT-heap table (see the record-table members); @p gfx is
    // unused. Returns nullptr before the first BuildOrRefit.
    const RHI::GPUBuffer* GetInstanceBuffer(IGraphicsDevice& gfx) const;
    // Alias of GetInstanceBuffer; nothing calls it.
    const RHI::GPUBuffer* GetMaterialBuffer(IGraphicsDevice& gfx) const { return GetInstanceBuffer(gfx); }
    // SRV GPU handle of the resident instance table.
    uint64_t              GetInstanceSrv(IGraphicsDevice& gfx) const;

    // Per-instance alpha-cutout records (DEFAULT heap, StructuredBuffer<RTAlphaRecord>, 40-byte
    // stride), indexed by InstanceID() like the instance table but a separate compact table, because
    // the RT alpha test runs per traversal candidate rather than per committed hit. One HLSL mirror:
    // shaders/rt_alpha_record.hlsli. Written in BuildOrRefit with the instance table (same index,
    // same guard, same dirty-span upload). Returns 0 before the first BuildOrRefit.
    uint64_t GetAlphaRecordVA(IGraphicsDevice& gfx) const;
    uint64_t GetAlphaRecordSrv(IGraphicsDevice& gfx) const;
    // Number of instances whose alpha record was written this frame. Shaders
    // bound-check InstanceID() against this before touching the table.
    uint32_t                  GetAlphaRecordCount() const { return m_alphaRecordCount; }

    // ---- Translucent (alpha-blend) RT shadow occluders ----
    // When true, alpha-blend instances (glass, water planes, smoke cards) get
    // RHI::ASInstanceFlag::FORCE_NON_OPAQUE + a blend RTAlphaRecord, so RTShadow's candidate loop
    // attenuates through them instead of treating them as solid. When false they stay opaque and no
    // RT consumer pays for the feature.
    // This is the real kill switch and must live here, not in a shader gate: a non-opaque instance
    // makes traversal leave fixed function on every candidate whatever the shader does (as with the
    // alphaClip switch; see the FORCE_NON_OPAQUE comment in BuildOrRefit).
    // Invalidating the retained gather is required: blendShadow is computed in fillMaterialProxy,
    // which the patch path skips, and the toggle marks nothing dirty in the Renderer's SceneDiffView.
    void SetBlendShadowOccluders(bool v)
    {
        if (v == m_blendShadowOccluders) return;   // called every frame
        m_blendShadowOccluders = v;
        m_retainedValid = false;                   // force one full gather
    }
    bool GetBlendShadowOccluders() const { return m_blendShadowOccluders; }

    // ---- Retained gather store ----
    // The retained static gather's last gate decision (Renderer/GatherGateStats.h): whether the last
    // Gather that reached the gate ran the full static gather, and the GatherGateStats::Reason bits
    // that vetoed the patch. LastGateSerial moves once per decision, so a caller can tell "decided
    // this frame" from a Gather that returned before the gate (no DXR).
    bool     LastGateFull()   const { return m_lastGateFull; }
    uint32_t LastGateReason() const { return m_lastGateReason; }
    uint64_t LastGateSerial() const { return m_lastGateSerial; }

private:
    // ---- Per-buffer mirrors ----
    struct BufMirror
    {
        bool                  valid   = false;
        uint64_t              blobGen = 0;   // m_blobGen it mirrors
        uint64_t              serial  = 0;   // m_gatherSerial that wrote it
        std::vector<uint32_t> maskRows;      // rows the dissolve loop rewrote
        std::vector<uint32_t> pageRows;      // rows BindProxy bound to a page
    };

    // BLAS cache key: source kind + identifier.
    struct CacheKey
    {
        uint32_t sourceKind; // 0 = MeshLibRef, 1 = MeshHandle
        uint64_t libOrSlot;  // MeshLibRef: (libGen << 32) | libSlot; MeshHandle: (gen << 32) | slot
        uint32_t meshId;     // MeshLibRef: entry index; MeshHandle: 0
        bool operator==(const CacheKey& o) const noexcept
        { return sourceKind == o.sourceKind && libOrSlot == o.libOrSlot && meshId == o.meshId; }
    };
    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            uint64_t h = k.libOrSlot * 0x9E3779B97F4A7C15ull;
            h ^= (uint64_t)k.meshId * 0xBF58476D1CE4E5B9ull;
            h ^= (uint64_t)k.sourceKind << 56;
            return (size_t)h;
        }
    };

    // ---- What Gather hands to BuildOrRefit ----
    // One row per instance, collected before any AS work so the instance count is known up front
    // (TLAS capacity check) and BLASes are built lazily for unique mesh keys only. A member type, not
    // a function-local or thread_local one: Gather and BuildOrRefit run on different threads.
    struct Pending
    {
        CacheKey key;
        DirectX::XMFLOAT4X4 worldMatrix;
        uint64_t                  positionVA;
        uint32_t                  positionStride;
        uint32_t                  vertexCount;
        uint64_t                  indexVA;
        uint32_t                  indexCount;
        RHI::Format               indexFormat;   // R32_UINT for MeshLibrary, R16_UINT for primitives
        DirectX::XMFLOAT4                  baseColor;     // packed into the per-instance record
        DirectX::XMFLOAT4                  emissive;      // (rgb x strength, 0) - DDGI light contribution
        int32_t                   emissiveTexIdx; // bindless tex pool index, -1 = none
        // RTR hit-shading material proxy (mirrors MaterialComponent fields, with the remap rules of
        // material_surface.hlsli::EvaluateMaterial).
        int32_t                   albedoTexIdx  = -1;
        int32_t                   surfaceTexIdx = -1;
        int32_t                   normalTexIdx  = -1;
        float                     roughnessMin  = 0.0f;
        float                     roughnessMax  = 1.0f;
        float                     metalnessMin  = 0.0f;
        float                     metalnessMax  = 0.0f;
        float                     reflectance   = 0.5f;
        // Material UV transform (uv' = uv*xy + zw), mirror of MaterialComponent::texMulAdd. The
        // identity default matters: an instance that never reaches the material-proxy fill (no
        // MaterialComponent) would otherwise carry (0,0,0,0) and collapse every hit UV to one texel.
        float                     uvTilingOffset[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
        // Bindless slots + offsets so closest-hit can re-fetch positions for the face normal. Zero
        // means "geometry lookup unavailable": closest-hit falls back to a directional-only NdotL
        // approximation.
        uint32_t                  vbBindlessIdx;
        uint32_t                  ibBindlessIdx;
        uint32_t                  vertexStart;
        uint32_t                  indexStart;
        // De-interleaved stream slots (procedural primitives + skinned) + index format; see the
        // DDGIInstanceData comment.
        uint32_t                  normalBufIdx = RHI::kInvalidBufferIndex;
        uint32_t                  uvBufIdx     = RHI::kInvalidBufferIndex;
        uint32_t                  indexBytes   = 4;
        // Foliage wind, so RT sees WPO-swayed geometry: a shear on the TLAS instance transform at emit
        // time, not folded into worldMatrix - worldMatrix is what the retained blob caches, and a
        // time-varying term there would drift and go stale on patch frames (which refresh only
        // sceneDiff.dirtyXform entities). Strength 0 for everything that is not wind foliage.
        FoliageWind::Params       wind{};
        uint8_t                   instanceMask;   // bit0 RTR/RTAO, bit1 RT-shadow caster, bit2 DDGI (instanceMask lambda)
        // Skinned (deformable) instances: the BLAS comes from m_skinnedBlas[entity] instead of
        // m_blasCache[key]; the stream bases address this entity's slice of the shared
        // SkinnedVertexRing arena (elements, not bytes).
        bool                      skinned      = false;
        Entity                    entity       = 0;
        uint32_t                  nrmStart     = 0;
        uint32_t                  uvStart      = 0;
        // Skinned: the hash of what this frame's skin dispatch consumed (skinning matrices, morph
        // weights), 0 when not hashed (Gathered::deformHashed says whether every row was).
        uint64_t                  deformHash   = 0;
        // Custom-shader material whose albedo resolved from no known space3 alias and whose engine
        // BASECOLORMAP is still a placeholder: every proxy field is an engine guess. Published as
        // RT_INST_FLAG_PROXY_UNTRUSTED so the trace substitutes an environment estimate instead of
        // confidently shading the wrong surface. Cleared once the surface cache supplies a baked page.
        bool                      proxyUntrusted = false;
        // Albedo / surface come from a baked material surface-cache page: uvTilingOffset carries the
        // page rect and every material scalar in this record is neutralised to the identity. Published
        // as RT_INST_FLAG_SURFACE_CACHE.
        bool                      surfaceCache   = false;
        // Material shaderType NPR_RAMP / NPR_COLOR, published as RT_INST_FLAG_NPR so reflection hit
        // shading uses the toon ramp composite instead of NdotL Cook-Torrance.
        bool                      npr            = false;
        // Persistent material slot + 1 (0 = unknown this frame), published in rtFlags bits 16..31 so
        // IntersectHW reads the hit material's real fields from the bindless MaterialGPUData table. The
        // slot cap (4096 by default) is bounded at 16 bits by Renderer::kMaxMaterialsUpperBound, so
        // slot+1 always fits.
        // Published only for Toon (the full toon-knob set) and Unlit (exposureCompensation: a reflected
        // unlit surface must keep its authored color under adaptation like the primary view, or a mirror
        // shows it at another brightness). Not for every instance: the lookup is per instance per frame
        // on the gather path, and no other model reads the table from here.
        uint32_t                  matSlot        = 0;
        uint32_t                  pathMaterialIndex = 0; // refreshed after the retained replay
        // ---- The binding half's inputs ----
        // Recorded by the World half (fillMaterialProxy) so the binding half (BindProxy: the material
        // slot + the surface-cache page) can run later - after this frame's PlanEmit, wherever that
        // runs - without the World.
        bool                      hasMaterial    = false; // a MaterialComponent was found
        bool                      rtStdMat       = false; // mc->rtUseStandardMaterial
        bool                      wantsMatSlot   = false; // Toon or Unlit (see matSlot)
        // Cached instance keeps its full-res aliased albedo (RT_INST_FLAG_ALBEDO_RAW).
        bool                      albedoRaw      = false;
        // Alias-resolved full-res albedo (applyCustomProxy), -1 = none. Read by
        // the surface-cache override in fillMaterialProxy (separate lambda).
        int32_t                   aliasAlbedoTexIdx = -1;
        // Material draws an on-screen inverted-hull outline (RT_INST_FLAG_OUTLINE).
        bool                      outline        = false;
        bool                      interleavedTangent = false;
        // ---- RT alpha cutout (RTAlphaRecord) ----
        float                     alphaRef     = 1.0f;  // MaterialComponent::alphaRef
        bool                      alphaClip    = false; // opts into non-opaque traversal
        bool                      doubleSided  = false; // backface hits are legitimate
        // Texture the candidate alpha test samples. Defaults to albedoTexIdx (BASECOLORMAP.a); a Masked
        // material with an authored OPACITYMAP points it at the mask and sets alphaFromRed (sample .r) -
        // see RT_ALPHA_FLAG_OPACITY_R in rt_alpha_record.hlsli.
        int32_t                   alphaTexIdx  = -1;
        bool                      alphaFromRed = false;
        // ---- RT alpha blend (translucent occluder) ----
        // Set for the four forward-transparent blend modes when the material does not alpha-clip (the
        // two are exclusive, so the record's alphaRef slot is unambiguous - see kRTAlphaFlagBlend).
        // blendOpacity is MaterialComponent::GetOpacity() (baseColor.w); the shader multiplies it by the
        // sampled BASECOLORMAP.a, as the forward transparent pass blends.
        bool                      blendShadow  = false;
        float                     blendOpacity = 1.0f;
        // BlendMode::Transmissive (RT_INST_FLAG_TRANSMISSIVE): blendOpacity is then what a covered
        // pixel stops, coverage x (1 - TransmissiveShadowTransmittance); mask bit 0x10 makes it a
        // path tracer vertex and no ray culls its back faces.
        bool                      transmissive = false;
        // Transmissive with blendShadow: kRTAlphaFlagTint and the colour one crossing keeps (bits 8-31).
        uint32_t                  shadowTint   = 0;
    };

    // ---- The gather lane, double-buffered ----
    struct Gathered
    {
        std::vector<Pending> pending;
        // The cached PT material table, shared copy-on-write with m_pathMatCache; null when the PT does
        // not want it.
        std::shared_ptr<const std::vector<Resource::MaterialGPUData>> pathMaterials;
        // Content revision of pathMaterials (0 = none). Changes exactly when a
        // record's bytes change, so PT accumulation resets on a real edit only.
        uint64_t pathMaterialHash = 0;
        uint32_t skinnedCount    = 0;
        bool     deformHashed    = false;   // every skinned row carries Pending::deformHash
        bool     valid           = false;
        bool     usePatch        = false;
        bool     anyDissolve     = false;
        bool     dirtyXformEmpty = true;
    };

    // ---- Acceleration-structure objects ----
    // RHI handles do not release themselves: every drop goes through
    // IGraphicsDevice::DestroyAccelerationStructure / DestroyBuffer (both deferred).
    // A built BLAS and its compaction state machine: built with ALLOW_COMPACTION, a COMPACTED_SIZE
    // query is recorded right after the build; once the frame delay has passed (compactReadyCounter)
    // the size is read and the BLAS copied into a right-sized result. None / Done = nothing to do.
    struct SceneBLAS
    {
        enum class CompactState : uint8_t { None, AwaitingSize, Done };

        RHI::AccelerationStructure as;

        CompactState compactState        = CompactState::None;
        uint64_t     compactReadyCounter = 0;   // m_buildCounter at/after which the query is GPU-ready

        bool     Valid() const      { return as.IsValid(); }
        uint64_t GPUAddress() const { return as.address; }
    };
    // The scene TLAS. Instance staging is multi-buffered: the instance write is a CPU memcpy at
    // record time while the previous frame's TLAS build may still read its slot on the GPU - one
    // upload buffer rewritten every frame races it (torn instance descs -> bad TLAS -> TDR). The
    // write takes the frame index; the build reads the slot written last. Three slots: the engine's
    // kFrameCount pipelining depth.
    struct SceneTLAS
    {
        static constexpr uint32_t kInstanceUploadSlots = 3;
        RHI::AccelerationStructure as;
        RHI::GPUBuffer             instances[kInstanceUploadSlots];   // UPLOAD, RHI::ASInstance records
        uint32_t uploadSlot       = 0;   // slot most recently written
        uint64_t scratchSize      = 0;
        uint32_t instanceCapacity = 0;
        uint32_t instanceCount    = 0;
        uint64_t GPUAddress() const { return as.address; }
    };
    // One TLAS instance on the CPU side; the instance write converts it to RHI::ASInstance field by
    // field. The transform is row-major 3x4 (trailing row [0,0,0,1] implied); triangleCount serves
    // consumers that sample an instance's surface (ReSTIR GI emitters). The fingerprint hashes these
    // raw bytes.
    struct SceneASInstance
    {
        float    transform[3][4] = { {1,0,0,0},{0,1,0,0},{0,0,1,0} };
        uint32_t instanceID    = 0;      // shader-visible (24 bits)
        uint32_t instanceMask  = 0xFF;   // ray mask - 0xFF = all
        uint32_t hitGroupIndex = 0;      // contribution to hit group index
        uint32_t flags         = 0;      // RHI::ASInstanceFlag
        uint64_t blasVA        = 0;      // SceneBLAS::GPUAddress()
        uint32_t triangleCount = 0;
        uint32_t reserved      = 0;
    };

    // ---- Skinned (deformable) BLAS pool ----
    struct SkinnedBLAS
    {
        SceneBLAS blas;
        uint64_t resultSize      = 0;   // prebuild sizes for the counts below
        uint64_t scratchSize     = 0;
        uint32_t vertexCount     = 0;   // geometry the sizes were queried for:
        uint32_t indexCount      = 0;   //   re-query + realloc when these change
        uint64_t lastSeenCounter = 0;   // m_buildCounter when last gathered
        bool     builtOnce       = false; // BLAS contents valid (>=1 build recorded)
        // Pending::deformHash of the pose the last build was recorded with: over the per-frame
        // build budget the BLAS lags its pose, and ContentFingerprint is not valid until it caught up.
        uint64_t builtDeform     = 0;

        // ---- Per-frame refit state (PERFORM_UPDATE) ----
        // Skinned meshes have fixed topology and only moving vertex positions - the case DXR's BLAS
        // update exists for. A refit is several times cheaper than a rebuild, but refitting forever
        // keeps the original split planes while vertices migrate, so node AABBs inflate and traversal
        // cost climbs: hence the periodic forced rebuild.
        uint64_t updateScratchSize  = 0;   // UpdateScratchDataSizeInBytes; 0 = not refittable
        uint64_t scratchOffset      = 0;   // this frame's private scratch region
        uint32_t framesSinceRebuild = 0;   // reset on every full rebuild
        bool     performUpdate      = false; // decided in the sizing loop, used by the build loop
        bool     updatable          = false; // built with ALLOW_UPDATE (so a refit is legal at all)
    };

    // ---- Per-instance record tables (DEFAULT resident + dirty staging) ----
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct RecordCopyRange { uint32_t first, count; };

    // ---- The gather lane, double-buffered ----
    Gathered& GatherWrite() { return m_gatherBuf[m_gatherWrite]; }
    Gathered& GatherRead()  { return m_gatherBuf[m_gatherRead];  }
    // Last statement of Gather.
    void PublishGather() { m_gatherRead = m_gatherWrite; m_gatherWrite ^= 1u; }

    // ---- Late binding ----
    // The binding of one row. Idempotent. @p e is explicit, not p.entity: the fused arm binds inside
    // fillMaterialProxy, before the gather assigns p.entity.
    void BindProxy(Pending& p, Entity e) const;
    // BuildOrRefit's binding pass over the published buffer `bufIdx`.
    void BindGatheredRows(std::vector<Pending>& rows, uint32_t bufIdx);
    // -ddgiproxyverify's census of the binding.
    void ReportProxySlotCensus(const std::vector<Pending>& rows, bool usePatch);

    // ---- Scratch, TLAS and record buffers ----
    // Grow scratch to at least @p needed bytes. Reallocates only on growth.
    void EnsureScratch(IGraphicsDevice& gfx, uint64_t needed);

    // Allocate / grow the TLAS to fit @p maxInstances. Pure resize, no build.
    void EnsureTLAS(IGraphicsDevice& gfx, uint32_t maxInstances);

    // Allocate / grow the per-instance material buffer to fit @p maxInstances.
    void EnsureMaterialBuffer(IGraphicsDevice& gfx, uint32_t maxInstances);

    // Allocate / grow the per-instance alpha-record buffer to fit @p maxInstances.
    void EnsureAlphaBuffer(IGraphicsDevice& gfx, uint32_t maxInstances);

    // Fill this frame's instance-upload slot from @p instances (clamped to the
    // TLAS capacity) and record it as the slot the build reads; the count written.
    uint32_t WriteTLASInstances(IGraphicsDevice& gfx, const SceneASInstance* instances,
                                uint32_t count, uint32_t frameSlot);
    // Release a TLAS's result and upload slots (deferred) and reset it.
    static void ReleaseTLAS(IGraphicsDevice& gfx, SceneTLAS& tlas);

    // ---- Retained gather store ----
    bool     m_lastGateFull   = false;
    uint32_t m_lastGateReason = 0;
    uint64_t m_lastGateSerial = 0;

    // Pending rows are trivially copyable, so the static prefix (meshlib + primitive instances) is
    // kept across frames as a raw byte blob (~2 MB; restoring it costs ~0.3 ms against ~1.3 ms for a
    // re-gather). Transform-only changes are patched in place from the Renderer's SceneDiffView; any
    // membership-affecting event (add / remove / material / tag change, unresolved meshes,
    // diff-stamp gap, RT-features-off gap) falls back to a full re-gather that frame. Skinned rows
    // are re-appended after the static prefix every frame.
    std::vector<uint8_t>  m_retainedStaticBlob;
    uint32_t              m_retainedStaticCount = 0;
    std::vector<uint32_t> m_retainedEntityToIdx;   // dense entity id -> static index
    bool                  m_retainedValid      = false;
    // Set by both gather paths and cleared on world clear, but read by nothing: m_bufMirror decides
    // whether a patch frame can skip the blob replay.
    bool                  m_pendingMirrorsBlob = false;

    // ---- Per-buffer mirrors ----
    // Each of the two gather buffers remembers the blob generation its static prefix mirrors and the
    // gather that wrote it. Invariant, kept by both gather paths: a mirrored buffer's static prefix is
    // the blob plus its binding (split: BindProxy; fused: already in the blob) plus the dissolve
    // masks listed in maskRows. The render half never writes the static prefix (it only erases
    // skinned rows, which sit after it) and is done with a buffer before the gather two frames later.
    // A patch frame whose write buffer the gather wrote exactly two calls ago (no full gather since)
    // catches up instead of replaying the whole blob (~7K rows, ~1.5 MB): it applies the previous
    // gather's transform rows from the blob, restores the rows a page bound (BindProxy writes the page
    // block only when it finds a page, so a page-bound row restarts from the pre-binding record) and
    // the rows a dissolve mask touched, then re-binds every row as the replay does.
    // -ddgimirroroff = always replay; -ddgimirrorverify = both, compared byte for byte on every hit.
    BufMirror             m_bufMirror[2];
    // Static rows whose binding can change while no surface-cache page can
    // bind (the cache is off or has no atlas): the Toon/Unlit material-slot
    // rows. On such a frame a mirror hit re-binds only these plus the buffer's
    // pageRows, instead of all ~7K rows. Rebuilt by every full gather.
    std::vector<uint32_t> m_wantsSlotRows;

    uint64_t              m_gatherSerial = 0;   // +1 per Gather past the DXR check
    uint64_t              m_blobGen      = 0;   // +1 per wholesale blob rewrite
    std::vector<uint32_t> m_prevGatherDirty;    // rows the previous gather re-matrixed
    std::vector<uint32_t> m_curGatherDirty;
    uint64_t              m_mirrorHits = 0, m_mirrorReplays = 0;
    uint64_t              m_mirrorVerified = 0, m_mirrorMismatchRows = 0;
    // ---- Retained gather store ----
    uint32_t              m_retainedLastStamp  = 0; // SceneDiffView.frameStamp continuity
    uint32_t              m_retainedUnresolved = 0; // transient skips -> keep rebuilding
    bool                  m_retainedGather     = true; // flag: false = always full gather
    // MaterialSurfaceCache::GetPublishEpoch() at the last gather. A change moves the set of
    // instances whose proxy should read a baked atlas page, which the Renderer's dirty feed cannot
    // see (the entities are clean, the cache changed), so it forces exactly one full re-gather.
    uint32_t              m_surfaceCacheEpoch  = 0;

    // ---- The gather lane, double-buffered ----
    // Gather (game half) writes one buffer while BuildOrRefit (render half) reads the other.
    // Everything Gather leaves behind travels together, because the gate flags only mean something
    // paired with the rows they describe: frame F+1's usePatch applied to frame F's rows skips the
    // missing-BLAS scan for meshes added in F, and a skinnedCount zeroed under a list that still has
    // skinned rows skips the erase and lets the skinned BLAS build read a ring the game half is
    // rewriting. BuildOrRefit mutates what it reads (erase_if, clearing valid), which is why the two
    // sides must not share a buffer.
    // valid: false until Gather filled the buffer for this frame; BuildOrRefit refuses a stale or
    // never-filled list. skinnedCount: recomputed by BuildOrRefit when allowSkinned drops the skinned
    // rows. usePatch / anyDissolve / dirtyXformEmpty: gate results decided in the gather and read
    // again by the build's unchanged-bytes fast path, passed as values so the render side never
    // re-reads the epoch or the scene diff.
    Gathered m_gatherBuf[2];
    uint32_t m_gatherWrite = 0;
    uint32_t m_gatherRead  = 0;

    // ---- Late binding ----
    // The two proxy fields that depend on this frame's PlanEmit - the raster's material slot and the
    // surface-cache page - are bound in BuildOrRefit, on the thread that records the AS, after this
    // frame's plan and emit wherever those ran. Gather's rows stay pre-binding apart from what an
    // earlier BuildOrRefit left in its buffer, which BindGatheredRows accounts for (the per-buffer
    // pageRows). Gather records the two lookup objects; the Renderer that owns this object owns both.
    const Renderer*             m_bindRenderer     = nullptr;
    const MaterialSurfaceCache* m_bindSurfaceCache = nullptr;

    // -ddgimirrorverify's render half: the incremental binding against a full bind of the blob
    // (atomics: Gather prints the tally line).
    std::atomic<uint64_t> m_bindVerified{0}, m_bindMismatchRows{0}, m_bindRowsBound{0};
    // Entities destroyed since the last build. Applied at the top of
    // BuildOrRefit; see OnEntityDestroyed.
    std::vector<Entity>  m_pendingDestroyed;

    // ---- Acceleration-structure objects ----
    std::unordered_map<CacheKey, SceneBLAS, CacheKeyHash> m_blasCache;

    // Keys whose BLAS waits on its compacted-size readback. The compaction pass examines only these,
    // not the whole cache (in steady state every entry is Done); an entry leaves the list when it
    // reaches Done or leaves the cache.
    std::vector<CacheKey> m_blasCompactPending;

    // ---- Skinned (deformable) BLAS pool ----
    // One BLAS per skinned mesh entity, refit or rebuilt in place every BuildOrRefit from that
    // frame's SkinnedVertexRing slice (PREFER_FAST_BUILD | ALLOW_UPDATE, never compacted). The
    // result buffer is allocated once per entity (the prebuild size depends only on the vertex /
    // index counts, fixed per mesh) and reused. Entries not gathered for kFrameCount+1 consecutive
    // calls (entity destroyed, world cleared, anim-culled, RTR off) are defer-released by
    // BuildOrRefit's prune sweep; OnWorldClear drops the whole pool unless ids are preserved.
    std::unordered_map<Entity, SkinnedBLAS>               m_skinnedBlas;
    // Round-robin start offset into this frame's skinned pending list, where the per-frame skinned
    // BLAS build budget starts. The list is in stable pool order, so a budget that always took the
    // first N entries would never build the tail (missing from the TLAS for good while the prune
    // sweep keeps it alive); rotating makes the budget a latency cap (each section refreshed at least
    // every ceil(N / budget) frames). Never-built entries are exempt from the budget, so a newly
    // spawned character enters the TLAS on its first frame.
    uint32_t                                              m_skinnedBuildCursor = 0;

    // ---- Acceleration-structure objects ----
    SceneTLAS                                             m_tlas;
    RHI::AccelerationStructure                            m_scratch;   // ASMemory::SCRATCH
    uint32_t                                              m_tlasCapacity = 0;
    bool                                                  m_tlasFresh    = false; // false -> next build is non-update
    // Last frame's instance count: DXR PERFORM_UPDATE needs the same instance count and BLAS
    // pointers, so a change (more BLASes finished building, scene churn) forces a full rebuild.
    uint32_t                                              m_lastInstanceCount = 0;
    uint32_t                                              m_transmissiveCount = 0;   // TransmissiveInstanceCount
    // Consecutive PERFORM_UPDATE refits since the last full TLAS rebuild. A
    // refit preserves the split planes chosen when the TLAS was last built from
    // scratch, so traversal quality decays as instances drift; forcing a
    // rebuild every kTLASRebuildInterval refits bounds that decay.
    uint32_t                                              m_tlasRefitCount = 0;
    // FNV-1a fingerprint of last frame's TLAS instance descs (transforms + BLAS VAs). When it
    // matches and no BLAS swapped, the TLAS is byte-identical to a rebuild: skip WriteTLASInstances
    // and the TLAS build (static scene, camera-only motion). 0 = no valid build yet.
    uint64_t                                              m_lastInstanceHash = 0;
    // False when m_lastInstanceHash is a placeholder because the fingerprint was skipped (a skinned
    // instance short-circuits both of its consumers, so hashing would be ~0.065 ms/frame of dead
    // serial work). Consumers must check this before trusting the hash.
    bool                                                  m_lastHashValid    = false;
    // ContentFingerprint: m_lastInstanceHash's bytes plus every skinned row's deformation hash,
    // kept apart from the TLAS skip above, whose skinned rule (always rebuild or refit) stays.
    uint64_t                                              m_contentHash      = 0;
    bool                                                  m_contentHashValid = false;
    std::atomic<bool>                                     m_deformWanted{false};   // SetDeformFingerprintWanted

    // True when the last static-BLAS scan found nothing outstanding, which lets
    // a clean patch frame skip the scan entirely. See the skipStaticBlasScan
    // note in BuildOrRefit.
    bool                                                  m_staticBlasAllResolved = false;
    // Previous build's value of the flag above. The fingerprint-reuse fast path needs "resolved for
    // both frames": on the false -> true frame the newly landed BLAS VAs changed the instance bytes,
    // so that frame must hash. Updated next to m_lastInstanceHash.
    bool                                                  m_staticBlasResolvedPrev = false;
    std::atomic<uint32_t>                                 m_staticGeometryEpoch{0};   // StaticGeometryEpoch

    // Persistent TLAS instance staging: cleared, never resized, because the fingerprint hashes raw
    // bytes (see its use in BuildOrRefit).
    std::vector<SceneASInstance>                          m_instancesScratch;

    // Monotonic BuildOrRefit call counter that times BLAS-compaction readbacks: a COMPACTED_SIZE
    // query recorded in one call is GPU-ready kFrameCount calls later (the swap-chain slot fence
    // guarantees that frame's compute work completed), so the compact-on-a-later-frame machine needs
    // no cross-queue fence.
    uint64_t                                              m_buildCounter = 0;

    // ---- Per-instance record tables (DEFAULT resident + dirty staging) ----
    // The structure of Renderer's retained instance buffer (Renderer::StageInstanceRecord):
    //   - one DEFAULT-heap StructuredBuffer per record type, which every shader binds;
    //   - kFrameCount persistently mapped UPLOAD staging buffers, each a full-size table indexed
    //     like the destination, so a dirty span is {first, count} with src offset == dst offset
    //     (like Renderer::StageMaterialSlot);
    //   - a CPU master array (raw bytes; the record structs are file-local to DDGISceneAS.cpp), a
    //     byte-exact mirror of the DEFAULT buffer. WriteInstances memcmp's each freshly built
    //     record against it and stages only differences, so a static scene writes no
    //     write-combined memory and issues no copies.
    // No per-frame-slot replay: a record changed on frame N reaches the DEFAULT buffer once, every
    // consumer reads the DEFAULT buffer, and nothing binds the staging buffers.
    // Lifetime: as the TLAS - written by BuildOrRefit's list (asCL, compute queue) and read only by
    // consumers ordered after it (ddgiCL waits on asCL; the graph's RTR / RTShadow / RTAO passes wait
    // on promoteCL <- ddgiCL), so one resident table is no weaker than the single in-place-rebuilt
    // TLAS that indexes it.
    RHI::GPUBuffer                                        m_matBuffer[kFrameCount]; // UPLOAD staging
    void*                                                 m_matMapped[kFrameCount] = {};
    // ---- PT material table and ReSTIR GI emitters ----
    // PT reference material table for both primary modes. Ringed UPLOAD storage; dense InstanceID
    // order is written alongside TLAS records, including parallel/filtered builds.
    RHI::GPUBuffer m_pathMaterials[kFrameCount];
    Resource::MaterialGPUData* m_pathMaterialMapped[kFrameCount] = {};
    uint32_t m_pathMaterialCapacity[kFrameCount] = {};
    uint64_t m_pathMaterialHash = 0;
    bool m_pathMaterialsReady = false;
    // The PT material table is cached across frames. Gather rebuilds it in full only when its row
    // order or a global input (texture loads, the material runtime epoch, a custom shader reload, a
    // scene-diff gap) changed; otherwise it rewrites just the rows of material-dirty entities. Each
    // UPLOAD slot remembers the revision and the InstanceID -> row map it holds, so an unchanged slot
    // is not rewritten either.
    std::shared_ptr<std::vector<Resource::MaterialGPUData>> m_pathMatCache;
    std::vector<uint32_t> m_pathMatRowEntity;      // row -> entity id
    std::vector<uint32_t> m_pathMatEntityRow;      // entity id -> first row (~0u)
    uint64_t m_pathMatEntitySig = 0;
    uint64_t m_pathMatTexRevision = 0;
    uint32_t m_pathMatRuntimeEpoch = 0, m_pathMatShaderEpoch = 0;
    uint32_t m_pathMatLastStamp = 0;
    uint64_t m_pathMatRevision = 0;                // bumped when any record's bytes change
    bool m_pathMatValid = false;
    uint64_t m_pathMatSlotRevision[kFrameCount] = {};
    std::vector<uint32_t> m_pathMatSlotRow[kFrameCount]; // InstanceID -> row the slot holds
    // ReSTIR GI emitter list (UPLOAD) and its area/CDF table (DEFAULT, UAV),
    // one pair per frame slot so a slot is reused only after its fence.
    RHI::GPUBuffer m_giEmitters[kFrameCount];
    RHI::GPUBuffer m_giEmitterTable[kFrameCount];
    void* m_giEmitterMapped[kFrameCount] = {};
    uint32_t m_giEmitterCapacity[kFrameCount] = {};
    uint32_t m_giEmitterCount = 0;
    bool m_giEmittersWanted = false;
    std::vector<uint8_t> m_giEmitterScratch;
    // m_giEmitterScratch survives frames: it is rebuilt only when an input changed (generation),
    // and a slot is re-copied only when stale.
    bool m_giEmitterCacheValid = false;
    size_t m_giEmitterCacheInstances = 0;
    uint64_t m_giEmitterGeneration = 0;
    uint64_t m_giEmitterSlotGeneration[kFrameCount] = {};
    // ---- Per-instance record tables (DEFAULT resident + dirty staging) ----
    RHI::GPUBuffer                                        m_matBufferGPU;           // DEFAULT, resident
    uint64_t                                              m_matSrv      = 0;        // SRV of m_matBufferGPU
    uint32_t                                              m_matCapacity = 0;
    std::vector<uint8_t>                                  m_matMaster;              // cap x sizeof(DDGIInstanceData)

    // Per-instance alpha-cutout records (40-byte stride): the same lifetime, staging / master
    // structure, index space and capacity guard as m_matBuffer - an instance is published to the TLAS
    // only when both tables have a slot for it (see the hoisted guard in BuildOrRefit).
    RHI::GPUBuffer                                        m_alphaBuffer[kFrameCount]; // UPLOAD staging
    void*                                                 m_alphaMapped[kFrameCount] = {};
    RHI::GPUBuffer                                        m_alphaBufferGPU;           // DEFAULT, resident
    uint64_t                                              m_alphaSrv       = 0;
    uint32_t                                              m_alphaCapacity  = 0;
    std::vector<uint8_t>                                  m_alphaMaster;            // cap x sizeof(RTAlphaRecord)

    // Dirty spans staged this frame, drained by the CopyBufferRegion loop at
    // the end of the same WriteInstances block. No frameSlot field (the
    // Renderer ranges carry one): these are produced and consumed inside one
    // function on one command list, so they can never straddle two frame slots.
    std::vector<RecordCopyRange>                          m_matCopyRanges;
    std::vector<RecordCopyRange>                          m_alphaCopyRanges;
    // Armed on buffer (re)creation, on capacity growth and on world clear:
    // the masters no longer describe the DEFAULT buffers, so the next write
    // loop stages EVERY record once (collapsing into a single span) instead of
    // trusting a memcmp against stale/zeroed bytes.
    bool                                                  m_recordUploadAll = true;
    uint32_t                                              m_recordCopyPeak  = 0;  // high-water telemetry
    // Records actually written this frame; shaders bound-check InstanceID() against it. Set right
    // after the write loop, not reused from the fingerprint-skip bookkeeping.
    uint32_t                                              m_alphaRecordCount = 0;
    // ---- Translucent (alpha-blend) RT shadow occluders ----
    // See SetBlendShadowOccluders. Toggling this changes the TLAS instance
    // bytes, so the FNV-1a fingerprint forces exactly one rebuild.
    bool                                                  m_blendShadowOccluders = true;
    // ---- Gather diagnostics ----
    // Diagnostic: meshlib instances gathered this frame whose bindless VB/IB slots were unresolved -
    // their hits take DDGIRayTrace's cheap fallback path. Printed in the throttled SceneAS status log.
    uint32_t                                              m_bindlessInvalidCount = 0;
    // Instances whose material proxy resolved a valid emissive texture this gather: each of their
    // hits pays a divergent bindless SampleLevel in DDGIRayTrace. See the fillMaterialProxy
    // diagnostic note.
    uint32_t                                              m_emissiveTexCount = 0;
    // Distinct emissive texture indices this gather. The per-hit SampleLevel in DDGIRayTrace uses
    // NonUniformResourceIndex: a wave whose hits span many distinct textures serializes its fetches,
    // so ~1 shared placeholder (streaming unfinished, resolve frozen) vs hundreds of real textures is
    // a ~10x per-hit cost cliff at identical validity counts.
    std::unordered_set<int32_t>                           m_emissiveTexDistinct;
};
