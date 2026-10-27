#include <chrono>
#include "RenderGraph/RenderPass/SkinningPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/SkeletonAsset.h"   // the OTHER two caps (static_asserts below)
#include "System/Log.h"
#include "System/Check.h"

#include <cstring>
#include <algorithm>

// GPU-side constant buffer layout; must match Skin.cs.hlsl SkinJobDesc.
// Morph weights live in a separate ByteAddressBuffer, not in the CB.
struct SkinJobCB
{
    uint32_t poseByteOffset;
    uint32_t vertexCount;
    uint32_t outPosByteOffset;
    uint32_t outNrmByteOffset;
    uint32_t morphCount;             // 0 = no morphs
    uint32_t morphWeightByteOffset;  // byte offset into the morph weight buffer
    // RESERVED, kept so SkinJobDesc stays 48 bytes. The previous position is read
    // from the previous ring slot through MeshDescriptor::prevPosition.
    uint32_t _reservedPrevPosSrc;
    uint32_t _reservedOutPrevPos;   // reserved (0)
    float    charExtent;             // per-skeleton max |restPos| (skinning TDR clamp; 0 = disabled)
    // Where the morph NORMAL deltas start in the delta buffer, in float3 elements
    // (SkinDispatchDesc::morphNrmElemOffset); 0 = none.
    uint32_t morphNrmElemOffset;
    // RESERVED (see above): keeps SkinJobCB 48 bytes, so the layout does not shift
    // under the asserts.
    uint32_t _reservedPrevMorphWeightByteOffset;
    uint32_t outTanByteOffset;       // 0xFFFFFFFF = mesh has no tangent stream
};
static_assert(sizeof(SkinJobCB) == 48, "SkinJobCB must match Skin.cs.hlsl SkinJobDesc");

// THE ONE PLACE ALL FOUR MORPH CAPS MEET. Everything else sees only two of them
// at a time, which is how they drift apart. ENGINE_MAX_MORPH_TARGETS arrives here
// through AnimationComponents.h -> shaders/morph_limits.hlsli, the same text
// Skin.cs.hlsl #includes.
static_assert(MorphComponent::MAX == SkeletonAsset::MAX_MORPH_TARGETS,
              "morph weight pool cap != skeleton morph-NAME table cap: slots would "
              "exist on the GPU that no name can address");
static_assert(MorphComponent::MAX == MorphClipAsset::MAX_MORPHS,
              "morph weight pool cap != morph CLIP channel cap");
static_assert(MorphComponent::MAX == ENGINE_MAX_MORPH_TARGETS,
              "C++ morph cap != the ENGINE_MAX_MORPH_TARGETS Skin.cs.hlsl clamps on");

// Per-job morph-weight upload - VARIABLE stride.
//
// Regions are packed BACK-TO-BACK, each sized to the job's own pair count rounded
// up to kMorphWeightAlignFloats. A fixed MorphComponent::MAX stride would cost
// 8 MB per frame slot (24 MB across the ring) of write-combined UPLOAD memory at
// MAX = 1024, for a payload that is empty for most jobs and a few hundred floats
// for the rest (the heaviest imported rig: 698 targets, 19192 floats for the whole
// character). The GPU side (SkinJobDesc::morphWeightByteOffset) and the CPU pool
// (SkinDispatchDesc::morphWeightOffset) are per-job offsets into shared buffers
// too.
//
// A region holds only the job's ACTIVE weights, as (uint local target, float
// weight) pairs - SkinDispatchDesc::morphActive of them - and the CB's morphCount
// is that pair count: Skin.cs walks the active list instead of every local target
// per vertex, and a job with no active weight gets morphCount 0 and no region.
//
// THE INVARIANT THE SHADER RELIES ON: a job's region is exactly the morphCount
// written into its CB. Skin.cs.hlsl loops to morphCount and the next region belongs
// to a DIFFERENT mesh, so morphCount must be the CLAMPED count, never the requested
// one - slot->morphCount is assigned from `curN` below.
static constexpr uint32_t kMorphWeightAlignFloats = 16;   // 64-byte regions

// Total per-frame-slot capacity, fixed (created once, never resized mid-frame)
// and generous: 1,048,576 floats = 524,288 (target, weight) pairs, e.g. 2048 jobs x
// 256 active weights. Overflow corrupts nothing: the overflowing job's morphCount
// is clamped to what is left, with an ENGINE_CHECK_ONCE naming it.
static constexpr uint32_t kMorphWeightBufBytes = 4u * 1024u * 1024u;

void SkinningPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SkinCS, RHI::ShaderStage::CS, "Skin.cs.hlsl", "SkinCS");

    if (!CreatePipelines(gfx)) return;

    // Job buffer: one 256-byte slot per job; the root CBV points at each slot's
    // offset, so UPLOAD with no bind flags. One per frame in flight, so per-frame
    // writes do not race in-flight GPU reads.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxSkinJobs) * 256u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_jobBuffer[i]))
                m_jobBufferMapped[i] = gfx.MapBuffer(m_jobBuffer[i]);
            if (!m_jobBufferMapped[i])
                LOG_ERROR("SkinningPass: failed to map job buffer[%u]", i);
        }
    }

    // Morph weight upload buffer: one flat kMorphWeightBufBytes arena per frame
    // slot, sub-allocated per job in Execute.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kMorphWeightBufBytes;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_morphWeightBuf[i]))
            {
                m_morphWeightMapped[i] = gfx.MapBuffer(m_morphWeightBuf[i]);
                m_morphWeightSRV[i]    = gfx.GetBufferSRVGpuHandle(m_morphWeightBuf[i]);
            }
            if (!m_morphWeightMapped[i])
                LOG_WARNING("SkinningPass: failed to map morph weight buffer[%u] (morphs disabled)", i);
        }
    }

    // Dummy rest-tangent SRV; the header says why an unbound t6 is not an option.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 4ull * 16ull;
        bd.stride     = 16;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, m_dummyTanBuf))
        {
            if (void* p = gfx.MapBuffer(m_dummyTanBuf))
            {
                const float ident[16] = { 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1 };
                std::memcpy(p, ident, sizeof(ident));
            }
            m_dummyTanSRV = gfx.GetBufferSRVGpuHandle(m_dummyTanBuf);
        }
        if (m_dummyTanSRV == 0)
            LOG_WARNING("SkinningPass: dummy tangent SRV unavailable");
    }

    LOG_SUCCESS("SkinningPass: initialised");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool SkinningPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SkinCS);
    if (!cs)
    {
        LOG_ERROR("SkinningPass: SkinCS shader not found");
        return false;
    }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("SkinningPass: compute PSO creation failed");
        return false;
    }
    return true;
}

void SkinningPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

RHI::CommandList SkinningPass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid())                        return cl;
    if (!m_jobs || m_jobs->empty())              return cl;
    if (!m_poseBuffer || !m_vertRing)            return cl;
    if (!m_gfx)                                  return cl;

    auto& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (!m_jobBufferMapped[frameSlot])           return cl;

    // ---- Phase 1 (CPU): upload all jobs (256-byte slots) + morph weights ----
    uint32_t totalJobs = 0;
    uint32_t morphJobs = 0;   // census only
    // Bump allocator over this frame slot's morph-weight arena. Regions are
    // packed in EMISSION order, which is also the order the CB slots are
    // written, so a job's offset is decided at the moment its CB is filled.
    uint32_t morphHeadBytes = 0;
    m_order.clear();

    for (const SkinDispatchDesc& job : *m_jobs)
    {
        if (job.vertexCount == 0) continue;
        if (totalJobs >= kMaxSkinJobs)
        {
            LOG_WARNING("SkinningPass: exceeded kMaxSkinJobs (%u) — remaining jobs skipped", kMaxSkinJobs);
            break;
        }

        // Write SkinJobCB at its 256-byte slot. No memset: all 12 SkinJobCB fields are
        // assigned below, and Skin.cs.hlsl cannot address the 208-byte alignment tail (its
        // cbuffer is 48 bytes, static_assert above). m_jobBuffer is write-combined UPLOAD
        // memory: zeroing would store 1500 jobs x 256 B per frame for ~72 KB of real data.
        m_order.push_back({ totalJobs, &job,
                            job.restTanSRVHandle != 0 ? job.restTanSRVHandle : m_dummyTanSRV });

        // ---- Sub-allocate this job's morph-weight region ----
        // curN is what the SHADER is told, so every clamp lands here before
        // slot->morphCount is written: a morphCount larger than the region reads the NEXT
        // JOB's weights.
        uint32_t curN        = 0;   // active (local, weight) pairs this job actually gets
        uint32_t regionBytes = 0;   // arena bytes it consumes (curN pairs rounded up)
        if (m_morphWeightMapped[frameSlot])
        {
            // No mapping -> nothing is uploaded; curN = 0 keeps the shader from reading
            // whatever is resident. No ACTIVE weight -> the same: no region, no morph loop.
            curN = (std::min)((std::min)(job.morphActive, job.morphCount), MorphComponent::MAX);
            const uint32_t avail = kMorphWeightBufBytes - morphHeadBytes;
            regionBytes =
                ((curN * 2u + kMorphWeightAlignFloats - 1) / kMorphWeightAlignFloats)
                * kMorphWeightAlignFloats * 4u;
            if (regionBytes > avail)
            {
                ENGINE_CHECK_ONCE(false,
                    "[SkinningPass] morph-weight arena full (%u B per frame slot): entity %u "
                    "has %u active morph weights, only %u fit — the rest are dropped this "
                    "frame. Raise kMorphWeightBufBytes.",
                    kMorphWeightBufBytes, job.entity, curN, avail / 8u);
                curN        = avail / 8u;   // may be 0
                regionBytes = avail;        // consume the tail either way
            }
        }

        SkinJobCB* slot = reinterpret_cast<SkinJobCB*>(
            static_cast<uint8_t*>(m_jobBufferMapped[frameSlot]) + totalJobs * 256u);
        slot->poseByteOffset        = job.poseByteOffset;
        slot->vertexCount           = job.vertexCount;
        slot->outPosByteOffset      = job.outPosByteOffset;
        slot->outNrmByteOffset      = job.outNrmByteOffset;
        slot->morphCount            = curN;
        slot->morphWeightByteOffset = morphHeadBytes;
        slot->_reservedPrevPosSrc   = 0xFFFFFFFFu;
        slot->_reservedOutPrevPos   = 0;
        slot->charExtent            = job.charExtent;
        // Only with weights to scale them: no targets looped, no normals read.
        slot->morphNrmElemOffset                 = curN > 0 ? job.morphNrmElemOffset : 0u;
        slot->_reservedPrevMorphWeightByteOffset = 0;
        // A mesh with no authored tangent (or no ring allocation) opts out; the vertex
        // factory then keeps its bind-pose / synthesised basis.
        slot->outTanByteOffset      = (job.restTanSRVHandle != 0) ? job.outTanByteOffset
                                                                  : 0xFFFFFFFFu;

        if (m_morphWeightMapped[frameSlot] && curN > 0)
        {
            auto* pairs = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_morphWeightMapped[frameSlot])
                                                      + morphHeadBytes);
            // WRITE ONLY WHAT THE SHADER READS: the active weights of the job's
            // pool slice, as (local target, weight) pairs, in local order. The
            // alignment tail past curN pairs is never read (the shader loops to
            // morphCount), so it stays untouched.
            //
            // Bounds-checked rather than trusted: a stale offset would upload
            // garbage weights, which shows up as a silently deformed face. A pair
            // the scan cannot fill (the slice disagrees with morphActive) is
            // written as weight 0 - it moves nothing.
            const std::vector<float>* pool = m_morphWeightPool;
            const size_t   poolN = pool ? pool->size() : 0;
            const uint32_t n     = (std::min)(job.morphCount, MorphComponent::MAX);
            uint32_t       k     = 0;
            if (pool && job.morphWeightOffset + n <= poolN)
            {
                const float* w = pool->data() + job.morphWeightOffset;
                for (uint32_t l = 0; l < n && k < curN; ++l)
                    if (w[l] > kMorphActiveEpsilon)
                    {
                        pairs[k * 2u]     = l;
                        std::memcpy(&pairs[k * 2u + 1u], &w[l], sizeof(float));
                        ++k;
                    }
            }
            for (; k < curN; ++k)
            {
                pairs[k * 2u]     = 0u;
                pairs[k * 2u + 1u] = 0u;   // 0.0f
            }
        }
        // Consume the region unconditionally: its offset is already in the CB, so the
        // next job must start past it even if the copy was skipped.
        morphHeadBytes += regionBytes;
        if (job.morphDeltaSRVHandle != 0) ++morphJobs;
        ++totalJobs;
    }

    if (totalJobs == 0) return cl;

    // THE TWO HALVES MUST AGREE ON THE SLOT. Every byte offset in this payload
    // (outPos / outNrm / outTanByteOffset) was allocated by the GAME half out of its
    // ring slot, and GBuffer.vs reads them back through UploadPosBindlessIdx(), also
    // the game half's slot; this dispatch binds gfx.GetFrameIndex(). Serial scheduling
    // makes the two equal. If a scheduling change breaks that, a skinned mesh silently
    // reads a buffer nobody wrote (no validation layer sees it) - so say so.
    if (m_curPosSlotExpected != 0xFFFFFFFFu && m_curPosSlotExpected != frameSlot)
    {
        ENGINE_CHECK_ONCE(false,
            "[SPLITCHECK][SkinningPass] frame slot disagreement: jobs allocated "
            "out of ring slot %u, dispatch binds slot %u",
            m_curPosSlotExpected, frameSlot);
    }

    // ---- Phase 2 (GPU): bind shared resources once, dispatch per job ----
    gfx.BindComputePipelineState(m_pso, cl);

    gfx.SetComputeDescriptorTable(kPose,   m_poseBuffer->SRVHandleForSlot(frameSlot), cl);
    gfx.SetComputeDescriptorTable(kOutPos, m_vertRing->PosUAVHandleForSlot(frameSlot),       cl);
    gfx.SetComputeDescriptorTable(kOutNrm, m_vertRing->NrmUAVHandleForSlot(frameSlot),       cl);
    gfx.SetComputeDescriptorTable(kOutTan, m_vertRing->TanUAVHandleForSlot(frameSlot),       cl);
    if (m_morphWeightSRV[frameSlot] != 0)
        gfx.SetComputeDescriptorTable(kMorphWeights, m_morphWeightSRV[frameSlot], cl);

    // SORT BY SOURCE, THEN BIND ONLY ON CHANGE.
    //
    // The per-job descriptor tables name the mesh's REST buffers, and clones share
    // them (the shared-source path in SkinnedMeshSubsystem,
    // MeshSkinnedComponent::sharedSourceKey): 100 copies of one character are 1500 jobs
    // but only ~15 distinct (restPos, restNrm, blend, restTan, morphDelta) tuples. The
    // job list is character-major, so consecutive jobs always differ and a plain "same
    // as last?" test catches nothing - the sort makes the redundancy adjacent. The
    // dispatches are unchanged; only repeated binds go.
    std::sort(m_order.begin(), m_order.end(),
              [](const DispatchRef& a, const DispatchRef& b)
    {
        if (a.job->restPosSRVHandle != b.job->restPosSRVHandle)
            return a.job->restPosSRVHandle < b.job->restPosSRVHandle;
        if (a.job->restNrmSRVHandle != b.job->restNrmSRVHandle)
            return a.job->restNrmSRVHandle < b.job->restNrmSRVHandle;
        if (a.job->blendSRVHandle != b.job->blendSRVHandle)
            return a.job->blendSRVHandle < b.job->blendSRVHandle;
        if (a.tan != b.tan) return a.tan < b.tan;
        if (a.job->morphDeltaSRVHandle != b.job->morphDeltaSRVHandle)
            return a.job->morphDeltaSRVHandle < b.job->morphDeltaSRVHandle;
        return a.cbSlot < b.cbSlot;   // stable within a group
    });

    // 0 is a meaningful handle here ("mesh has no morph deltas"), so the
    // "nothing bound yet" sentinel must be a value no descriptor can take.
    constexpr uint64_t kUnbound = ~0ull;
    uint64_t lastPos = kUnbound, lastNrm = kUnbound, lastBlend = kUnbound;
    uint64_t lastTan = kUnbound, lastMorph = kUnbound;
    uint32_t tableBinds = 0;

    for (const DispatchRef& ref : m_order)
    {
        const SkinDispatchDesc& job = *ref.job;

        // Root CBV -> this job's 256-byte slot in the upload buffer. ref.cbSlot, NOT the
        // loop index: the slot was written in Phase-1 emission order, which this loop does
        // not follow.
        gfx.SetComputeRootCBV(kCBSlot, m_jobBuffer[frameSlot], ref.cbSlot * 256u, cl);

        if (job.restPosSRVHandle != lastPos)
        { gfx.SetComputeDescriptorTable(kRestPos, job.restPosSRVHandle, cl); lastPos = job.restPosSRVHandle; ++tableBinds; }
        if (job.restNrmSRVHandle != lastNrm)
        { gfx.SetComputeDescriptorTable(kRestNrm, job.restNrmSRVHandle, cl); lastNrm = job.restNrmSRVHandle; ++tableBinds; }
        if (job.blendSRVHandle != lastBlend)
        { gfx.SetComputeDescriptorTable(kBlend,   job.blendSRVHandle,   cl); lastBlend = job.blendSRVHandle; ++tableBinds; }
        if (ref.tan != lastTan)
        { gfx.SetComputeDescriptorTable(kRestTan, ref.tan, cl);              lastTan = ref.tan;             ++tableBinds; }

        // Bound only when the mesh HAS morph deltas. A job without them keeps the
        // previous table bound, which the shader cannot read: Skin.cs.hlsl loops to
        // morphCount, and morphCount is 0 for exactly these jobs.
        if (job.morphDeltaSRVHandle != 0 && (job.morphDeltaSRVHandle != lastMorph))
        { gfx.SetComputeDescriptorTable(kMorphDeltas, job.morphDeltaSRVHandle, cl); lastMorph = job.morphDeltaSRVHandle; ++tableBinds; }

        gfx.DispatchCompute((job.vertexCount + 63) / 64, 1, 1, cl);
    }

    // Report from a STEADY-STATE frame, not the first: the first Execute lands
    // mid-load with only part of the population registered and understates the bind
    // count by more than half.
    if (++m_execCount == 300u)
    {
        const uint32_t before = totalJobs * 4u + morphJobs;
        LOG_INFO("SkinningPass: BIND CENSUS %u jobs -> descriptor-table binds %u -> %u "
                 "(%u saved); dispatches %u, root CBVs %u (unchanged)",
                 totalJobs, before, tableBinds,
                 before > tableBinds ? before - tableBinds : 0u,
                 totalJobs, totalJobs);
    }

    // UAV barriers: every Skin.cs write is visible before GBufferPass reads the ring.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_vertRing->PosBufferForSlot(frameSlot)), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_vertRing->NrmBufferForSlot(frameSlot)), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_vertRing->TanBufferForSlot(frameSlot)), cl);

    return cl;
}
