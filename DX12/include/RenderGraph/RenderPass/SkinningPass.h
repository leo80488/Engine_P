#pragma once

// SkinningPass - compute pass that skins every visible skinned mesh section,
// before GBufferPass. It binds the frame's PoseRingBuffer slot (bone matrices) as SRV
// and the SkinnedVertexRing pos / nrm / tan slot as UAVs once, then per job the
// rest-pose buffers (pos, nrm, tan), blend weights and morph deltas as SRVs, and
// dispatches ceil(vertexCount / 64) groups. Closing UAV barriers let GBufferPass
// read the ring through the MeshDescriptor stream descriptors.
//
// No graph resources (GPU buffers only). SkinnedMeshSubsystem publishes the jobs
// (SetJobs, SetMorphWeightPool, SetCurPosSlot) before Execute.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/SkinningBuffers.h"
#include "Graphics/RootSignatureLayout.h"
#include "ECS/AnimationComponents.h"   // MorphComponent::MAX (== kMaxMorphTargets, shaders/morph_limits.hlsli)

#include <vector>
#include <cstdint>

// ==== Per-entity dispatch descriptor ====
// Built by SkinnedMeshSubsystem::BuildSkinJobs, consumed by SkinningPass::Execute.
struct SkinDispatchDesc
{
    // Mesh entity this job skins. DDGISceneAS treats the job list as the
    // authoritative "this entity's SkinnedVertexRing slice IS written this frame" set
    // before building a skinned BLAS from it: SkinningOutput offsets can look valid
    // while the dispatch was dropped (e.g. the kMaxSkinJobs cap), and a BLAS over an
    // unwritten ring region is garbage geometry -> DXR traversal TDR.
    uint32_t entity = 0xFFFFFFFFu;   // ECS Entity id
    uint64_t restPosSRVHandle;       // GPU handle for rest position buffer SRV
    uint64_t restNrmSRVHandle;       // GPU handle for rest normal buffer SRV
    uint64_t blendSRVHandle;         // GPU handle for blend-weight buffer SRV
    uint64_t restTanSRVHandle = 0;   // rest tangent float4 SRV (0 = mesh has none)
    uint64_t morphDeltaSRVHandle;    // GPU handle for morph target delta buffer SRV (0 = no morphs)
    uint32_t poseByteOffset;         // byte offset into current frame's PoseRingBuffer
    uint32_t outPosByteOffset;       // byte offset into SkinnedVertexRing pos buffer
    uint32_t outNrmByteOffset;       // byte offset into SkinnedVertexRing nrm buffer
    uint32_t outTanByteOffset = ~0u; // byte offset into SkinnedVertexRing tan buffer (~0u = skip)
    uint32_t vertexCount;
    float    charExtent = 0.f;        // per-skeleton max |restPos|; skinning clamps verts to ~4x this (TDR guard)
    uint32_t morphCount;             // number of morph targets (0 = no morphs)
    // Where the NORMAL deltas start in the delta buffer, in float3 elements: the
    // mesh's full local target count x vertexCount (.imorph v2). 0 = the mesh
    // carries none, and Skin.cs leaves the rest normal as it was.
    uint32_t morphNrmElemOffset = 0;


    // Morph weights live OUT OF LINE: morphWeightOffset is an element index (not
    // bytes) into the pool handed to SkinningPass::SetMorphWeightPool. An inline
    // float[MorphComponent::MAX] array would carry 4 KB per job (MAX = 1024) whether or
    // not the mesh morphs - 2 KB a job already cost BuildSkinJobs ~10 ms at 1500
    // sections - and real rigs do use the cap (shaders/morph_limits.hlsli; ~700 targets
    // seen), so the allocation stays proportional here and in SkinningPass's packed
    // per-job upload.
    uint32_t morphWeightOffset = 0;      // index into the morph-weight pool
    // How many of this job's pool weights are ACTIVE (> kMorphActiveEpsilon) after
    // the facial fold: what SkinningPass uploads, as (local target, weight) pairs, and
    // all Skin.cs walks. 0 = the job does no morph work at all (no upload, no loop),
    // whatever morphCount says.
    uint32_t morphActive = 0;
};

// A morph weight at or below this moves nothing worth a delta fetch. Applied
// once per job on the CPU (BuildSkinJobs counts, SkinningPass compacts) instead of
// per vertex in Skin.cs.hlsl.
constexpr float kMorphActiveEpsilon = 0.001f;

// ==== SkinningPass ====
class SkinningPass : public RG::RenderPass
{
public:
    // Max skinned meshes per frame; Execute drops jobs past it (BuildSkinJobs already
    // enforces it at the source). Public: DDGISceneAS clamps its skinned-BLAS job-set
    // scan to it (a dropped job's ring slice is unwritten and must not become a BLAS).
    // One capacity contract for owner emission, dispatch upload allocation and BLAS
    // admission; the 200-agent AIBench exceeds 2048 mesh parts.
    static constexpr uint32_t kMaxSkinJobs = 4096;

    // CPU-side morph weight storage the jobs index into (SkinDispatchDesc::
    // morphWeightOffset). Borrowed, not owned: the caller guarantees it outlives
    // Execute and is refilled before SetJobs each frame.
    void SetMorphWeightPool(const std::vector<float>* p) { m_morphWeightPool = p; }
    const char* GetName() const override { return "SkinningPass"; }

    void Setup  (RG::RenderGraphBuilder& b) override {}  // no graph-managed textures
    void Init   (IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (standalone: Renderer::ReloadShaders -> SkinnedMeshSubsystem).
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // The pose / vertex rings, borrowed (owned by SkinnedMeshSubsystem).
    void SetBuffers(PoseRingBuffer* pose, SkinnedVertexRing* verts)
    {
        m_poseBuffer = pose;
        m_vertRing   = verts;
    }

    // The jobs SkinnedMeshSubsystem::BuildSkinJobs built; the pointer must stay
    // valid until Execute() returns.
    void SetJobs(const std::vector<SkinDispatchDesc>* jobs) { m_jobs = jobs; }

    // Which SkinnedVertexRing slot this payload's byte offsets were allocated from:
    // the GAME half's slot when BuildSkinJobs ran. Passed as DATA, not derived here,
    // because the two halves do not share a slot counter. Execute compares it with the
    // slot it is about to bind ([SPLITCHECK]).
    void SetCurPosSlot(uint32_t curSlot) { m_curPosSlotExpected = curSlot; }

private:
    // SHARED COMPUTE root signature slots: ABSOLUTE compute root indices from
    // RHI::Slots::Cmp (Graphics/RootSignatureLayout.h); never Gfx::, whose same-named
    // slots have different values. The register order is deliberately non-monotonic
    // (6 -> t5, 7 -> t3, 8 -> t4), not a bug; RootSignatureLayout.h documents it.
    static constexpr auto kCBSlot    = RHI::Slots::Cmp::CB;      // ROOT_CBV b0 space2 - per-job 256B slot
    static constexpr auto kRestPos   = RHI::Slots::Cmp::SRV_t0;  // DESC_TABLE t0 space2
    static constexpr auto kRestNrm   = RHI::Slots::Cmp::SRV_t1;  // DESC_TABLE t1 space2
    static constexpr auto kBlend     = RHI::Slots::Cmp::SRV_t2;  // DESC_TABLE t2 space2
    static constexpr auto kPose         = RHI::Slots::Cmp::SRV_t3;  // DESC_TABLE t3 space2
    static constexpr auto kMorphDeltas  = RHI::Slots::Cmp::SRV_t4;  // DESC_TABLE t4 space2
    static constexpr auto kMorphWeights = RHI::Slots::Cmp::SRV_t5;  // DESC_TABLE t5 space2
    static constexpr auto kRestTan      = RHI::Slots::Cmp::SRV_t6;  // DESC_TABLE t6 space2
    // (job data comes through the root CBV pointing at each job's 256B slot)
    static constexpr auto kOutPos       = RHI::Slots::Cmp::UAV_u0;  // DESC_TABLE u0 space2
    static constexpr auto kOutNrm       = RHI::Slots::Cmp::UAV_u1;  // DESC_TABLE u1 space2
    static constexpr auto kOutTan       = RHI::Slots::Cmp::UAV_u2;  // DESC_TABLE u2 space2

    // Ring depth: one buffer per frame in flight.
    static constexpr uint32_t kFrameCount  = RHI::kFramesInFlight;

    struct DispatchRef
    {
        uint32_t                 cbSlot;   // Phase-1 emission index
        const SkinDispatchDesc*  job;
        uint64_t                 tan;      // resolved rest-tangent handle (dummy substituted)
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

public:
    // SetCurPosSlot's slot (~0u = not set: no check).
    uint32_t m_curPosSlotExpected = 0xFFFFFFFFu;

private:
    const std::vector<float>* m_morphWeightPool = nullptr;

    IGraphicsDevice*   m_gfx     = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // Job upload buffer: kMaxSkinJobs x 256 bytes (one 256B-aligned slot per job);
    // the root CBV points at each slot per dispatch. One per frame in flight, so CPU
    // writes for frame N+1 do not race GPU reads of frame N.
    RHI::GPUBuffer m_jobBuffer[kFrameCount];
    void*          m_jobBufferMapped[kFrameCount] = {};

    // Morph weight upload buffer: ONE flat arena per frame slot
    // (kMorphWeightBufBytes in SkinningPass.cpp), sub-allocated per job; a job's
    // region is exactly the morphCount its CB carries.
    RHI::GPUBuffer m_morphWeightBuf[kFrameCount];
    void*          m_morphWeightMapped[kFrameCount] = {};
    uint64_t       m_morphWeightSRV[kFrameCount]    = {};

    // Phase-2 dispatch order, sorted by source buffers so identical descriptor
    // tables bind once instead of once per job.
    //
    // Reordering is safe because each job writes a DISJOINT range of the vertex ring.
    // But Phase 1 writes each job's 256-byte CB slot and morph-weight region at its
    // EMISSION index, and Phase 2's root CBV must point at that slot - so cbSlot
    // travels with the job; without it every dispatch gets the wrong SkinJobDesc.
    //
    // m_jobs itself is NEVER reordered: DDGISceneAS replicates the kMaxSkinJobs cap as
    // "the first N jobs with vertexCount > 0, in list order" when it decides which
    // skinned BLASes are safe to build.
    std::vector<DispatchRef> m_order;
    uint32_t m_execCount = 0;   // census only: report from a steady-state frame

    // 4-element float4 stand-in bound at t6 for meshes with no authored tangent
    // stream. The shader's g_RestTan read is guarded, but the descriptor table must
    // still hold a live SRV of the right kind: an unbound slot in a non-VOLATILE table
    // is undefined behaviour. Structured-buffer OOB reads return 0, so an undersized
    // dummy is safe.
    RHI::GPUBuffer m_dummyTanBuf;
    uint64_t       m_dummyTanSRV = 0;

    PoseRingBuffer*   m_poseBuffer = nullptr;
    SkinnedVertexRing* m_vertRing  = nullptr;

    const std::vector<SkinDispatchDesc>* m_jobs = nullptr;
};
