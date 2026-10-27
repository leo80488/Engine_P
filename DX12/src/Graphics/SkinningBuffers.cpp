#include "Graphics/SkinningBuffers.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Check.h"
#include "System/Log.h"

#include <cstring>

// ==== PoseRingBuffer ====

void PoseRingBuffer::Init(IGraphicsDevice& gfx)
{
    const uint64_t bufSize = static_cast<uint64_t>(kMaxBonesPerFrame) * sizeof(DirectX::XMFLOAT4X4);

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = bufSize;
        bd.stride     = sizeof(DirectX::XMFLOAT4X4);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW; // ByteAddressBuffer

        bd.debugName = "PoseRingBuffer";
        if (!gfx.CreateBuffer(bd, m_buffers[i]))
        {
            LOG_ERROR("PoseRingBuffer: failed to create buffer slot %u", i);
            continue;
        }

        m_mapped[i]     = gfx.MapBuffer(m_buffers[i]);
        m_srvHandles[i] = gfx.GetBufferSRVGpuHandle(m_buffers[i]);

        if (!m_mapped[i])
            LOG_ERROR("PoseRingBuffer: MapBuffer failed for slot %u", i);
        if (!m_srvHandles[i])
            LOG_WARNING("PoseRingBuffer: SRV handle is 0 for slot %u - check bindFlags", i);
    }

    // The shadow is indexed only by slots Alloc() hands out, capped at kMaxBonesPerFrame, so ONE
    // frame's worth is enough; it need not be kFramesInFlight-deep like the GPU buffers.
    m_shadow.resize(kMaxBonesPerFrame);

    LOG_SUCCESS("PoseRingBuffer: initialised (%u KB × %u frames)",
                static_cast<uint32_t>(bufSize / 1024), kFramesInFlight);
}

void PoseRingBuffer::BeginFrame(uint64_t frameIndex)
{
    // High-water watch: the head being reset is the frame that just finished. One warning per
    // threshold per session, naming the constant to raise (like MeshDescriptorHeap::NotePoolPressure);
    // overflow itself falls back to the rest pose, so this warns BEFORE a character silently drops
    // to it.
    if (m_writeHead > m_peakBones) m_peakBones = m_writeHead;
    if (!m_warned90 && m_writeHead >= (kMaxBonesPerFrame / 10) * 9)
    {
        m_warned90 = true;
        LOG_WARNING("PoseRingBuffer: %u/%u bone slots used this frame (>=90%%). "
                    "Raise PoseRingBuffer::kMaxBonesPerFrame — beyond the cap, "
                    "skeletons fall back to bind pose.",
                    m_writeHead, kMaxBonesPerFrame);
    }
    else if (!m_warned75 && m_writeHead >= (kMaxBonesPerFrame / 4) * 3)
    {
        m_warned75 = true;
        LOG_INFO("PoseRingBuffer: %u/%u bone slots used this frame (>=75%%). "
                 "Headroom is shrinking; PoseRingBuffer::kMaxBonesPerFrame is the cap.",
                 m_writeHead, kMaxBonesPerFrame);
    }

    m_frameSlot = static_cast<uint32_t>(frameIndex % kFramesInFlight);
    m_writeHead = 0;
    m_flushedThisFrame = false;
}

uint32_t PoseRingBuffer::Alloc(uint32_t boneCount)
{
    // Check capacity BEFORE advancing the head: a slot past the cap would put the CPU pose write
    // beyond the buffer end. Return the ~0u sentinel; callers keep the skeleton on the rest-pose
    // fallback.
    if (static_cast<uint64_t>(m_writeHead) + boneCount > kMaxBonesPerFrame)
    {
        ENGINE_CHECK_ONCE(false,
            "PoseRingBuffer ring overflow (need %u, head %u, cap %u) — "
            "increase kMaxBonesPerFrame; overflow falls back to rest pose",
            boneCount, m_writeHead, kMaxBonesPerFrame);
        return ~0u;
    }
    ENGINE_CHECK_ONCE(!m_flushedThisFrame,
        "PoseRingBuffer::Alloc after FlushToGPU with no BeginFrame between — this "
        "slice will reach the shadow but NOT the GPU, rendering stale poses. Move "
        "the writer before Renderer::TickAnimationChain's flush.");

    const uint32_t slot = m_writeHead;
    m_writeHead += boneCount;
    return slot;
}

DirectX::XMFLOAT4X4* PoseRingBuffer::MapSlice(uint32_t slot, uint32_t /*boneCount*/)
{
    // nullptr when unmapped is deliberate: callers read it as "ring unavailable, use the rest
    // pose", which beats a shadow write that never reaches the GPU.
    if (!m_mapped[m_frameSlot]) return nullptr;
    if (slot >= m_shadow.size())  return nullptr;
    return m_shadow.data() + slot;
}

void PoseRingBuffer::FlushToGPU()
{
    m_flushedThisFrame = true;
    if (!m_mapped[m_frameSlot] || m_writeHead == 0) return;
    // ONE sequential write into write-combined memory, the pattern WC is built for.
    std::memcpy(m_mapped[m_frameSlot], m_shadow.data(),
                static_cast<size_t>(m_writeHead) * sizeof(DirectX::XMFLOAT4X4));
    m_flushedThisFrame = true;
}

uint64_t PoseRingBuffer::UploadSRVHandle() const
{
    return m_srvHandles[m_frameSlot];
}

uint64_t PoseRingBuffer::SRVHandleForSlot(uint32_t slot) const
{
    return m_srvHandles[slot];
}

const DirectX::XMFLOAT4X4* PoseRingBuffer::ReadMapped(uint32_t byteOffset) const
{
    if (!m_mapped[m_frameSlot]) return nullptr;
    const uint32_t slot = byteOffset / sizeof(DirectX::XMFLOAT4X4);
    if (slot >= m_shadow.size()) return nullptr;
    return m_shadow.data() + slot;
}

// ==== SkinnedVertexRing ====

void SkinnedVertexRing::Init(IGraphicsDevice& gfx, uint32_t vertexBudget, const char* source)
{
    // Fixed at startup, before allocation and bindless registration: never grow the rings while
    // vertex / BLAS references are in flight.
    m_vertexBudget = vertexBudget;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        posBindlessIdx[i]     = RHI::kInvalidBufferIndex;
        nrmBindlessIdx[i]     = RHI::kInvalidBufferIndex;
        tanBindlessIdx[i]     = RHI::kInvalidBufferIndex;
    }

    // Position buffer: float3 per slot = 12 bytes, one slot per skinned vertex.
    const uint64_t posBufSize = static_cast<uint64_t>(m_vertexBudget) * sizeof(float) * 3;
    // Normal buffer: float3 per vertex = 12 bytes. One slot per vertex.
    const uint64_t nrmBufSize = static_cast<uint64_t>(m_vertexBudget) * sizeof(float) * 3;
    // Tangent buffer: float4 per vertex = 16 bytes (xyz direction, w handedness). Skinning must
    // rotate the tangent too: with a bind-pose tangent under a posed normal, GBuffer.vs's
    // Gram-Schmidt keeps T in the right plane but leaves its azimuth off by the bone rotation,
    // corrupting every tangent-space decode on a skinned mesh (normal maps, and anisotropic hair
    // highlights, which are built from T).
    const uint64_t tanBufSize = static_cast<uint64_t>(m_vertexBudget) * sizeof(float) * 4;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        // Position buffer - UAV (CS writes) + SRV (VS reads). Structured float3 (stride 12), not raw,
        // so CreateBuffer makes the matching UAV.
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = posBufSize;
            bd.stride     = sizeof(float) * 3; // structured float3
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::NONE; // structured, not raw

            bd.debugName = "SkinnedVertexRing.Position";
            if (!gfx.CreateBuffer(bd, m_posBuffers[i]))
            {
                LOG_ERROR("SkinnedVertexRing: failed to create pos buffer slot %u", i);
                continue;
            }

            m_posUAVHandles[i] = gfx.GetBufferUAVGpuHandle(m_posBuffers[i]);
            m_posSRVHandles[i] = gfx.GetBufferSRVGpuHandle(m_posBuffers[i]);
        }

        // Normal buffer - UAV (CS writes) + SRV (VS reads), structured float3
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = nrmBufSize;
            bd.stride     = sizeof(float) * 3;
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::NONE;

            bd.debugName = "SkinnedVertexRing.Normal";
            if (!gfx.CreateBuffer(bd, m_nrmBuffers[i]))
            {
                LOG_ERROR("SkinnedVertexRing: failed to create nrm buffer slot %u", i);
                continue;
            }

            m_nrmUAVHandles[i] = gfx.GetBufferUAVGpuHandle(m_nrmBuffers[i]);
            m_nrmSRVHandles[i] = gfx.GetBufferSRVGpuHandle(m_nrmBuffers[i]);
        }

        // Tangent buffer - UAV (CS writes) + SRV (VS reads), structured float4
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = tanBufSize;
            bd.stride     = sizeof(float) * 4;
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::NONE;

            bd.debugName = "SkinnedVertexRing.Tangent";
            if (!gfx.CreateBuffer(bd, m_tanBuffers[i]))
                LOG_ERROR("SkinnedVertexRing: failed to create tangent buffer slot %u", i);
            else
            {
                m_tanUAVHandles[i] = gfx.GetBufferUAVGpuHandle(m_tanBuffers[i]);
                m_tanSRVHandles[i] = gfx.GetBufferSRVGpuHandle(m_tanBuffers[i]);
            }
        }

        // No prev-position buffer: last frame's positions are read from the previous ring slot
        // (MeshDescriptor::prevPosition).
    }

    // The total is the number that matters: DEFAULT-heap VRAM resident from startup on every scene,
    // empty or not. Capacity is quoted in characters, since a vertex count is no budget anyone
    // reasons about at a glance.
    const uint64_t totalBytes = (posBufSize + nrmBufSize + tanBufSize) * kFramesInFlight;
    LOG_SUCCESS("SkinnedVertexRing: initialised (pos %u KB, nrm %u KB, tan %u KB x %u frames "
                "= %llu MiB RESIDENT; budget %u verts/frame ~= %u characters at 70k verts, from %s)",
                static_cast<uint32_t>(posBufSize / 1024),
                static_cast<uint32_t>(nrmBufSize / 1024),
                static_cast<uint32_t>(tanBufSize / 1024),
                kFramesInFlight,
                static_cast<unsigned long long>(totalBytes / (1024ull * 1024ull)),
                m_vertexBudget,
                static_cast<uint32_t>(m_vertexBudget / 70573u), source);
}

void SkinnedVertexRing::BeginFrame(uint64_t frameIndex)
{
    // High-water watch on the frame that just finished (as in PoseRingBuffer::BeginFrame). All three
    // rings are 1:1 with skinned vertices, so the position ring stands for them.
    const uint32_t posSlots = m_posByteHead / uint32_t(sizeof(float) * 3);
    if (posSlots > m_peakPosVerts) m_peakPosVerts = posSlots;
#ifdef TRACY_ENABLE
    // Plotted every frame: the warnings latch after firing once, so a capture taken later has
    // nothing else to show how close the ring is to freezing meshes in the rest pose.
    TracyPlot("Skin.PosRingPct",
              static_cast<int64_t>(100.0 * posSlots / m_vertexBudget));
    TracyPlot("Skin.PosRingSlots", static_cast<int64_t>(posSlots));
#endif
    if (!m_warned90 && posSlots >= (m_vertexBudget / 10) * 9)
    {
        m_warned90 = true;
        LOG_WARNING("SkinnedVertexRing: %u/%u position slots used this frame (>=90%%). "
                    "Raise -skinvertexbudget — beyond the cap, "
                    "meshes fall back to the REST POSE (they stop animating, with no "
                    "other symptom). One position slot per skinned vertex.",
                    posSlots, m_vertexBudget);
    }
    else if (!m_warned75 && posSlots >= (m_vertexBudget / 4) * 3)
    {
        m_warned75 = true;
        LOG_INFO("SkinnedVertexRing: %u/%u position slots used this frame (>=75%%). "
                 "Headroom is shrinking; -skinvertexbudget "
                 "is the budget (%u verts, 1 position slot each).",
                 posSlots, m_vertexBudget, m_vertexBudget);
    }

    m_frameSlot       = static_cast<uint32_t>(frameIndex % kFramesInFlight);
    m_posByteHead     = 0;
    m_nrmByteHead     = 0;
    m_tanByteHead     = 0;
}

// Overflow policy of the three Alloc* below: capacity is checked BEFORE the head advances, since
// an offset past the cap would make SkinningCS write (and the GBuffer VS read) beyond the ring.
// On overflow they return ~0u and callers drop the mesh to the rest pose for the frame.

uint32_t SkinnedVertexRing::AllocPosition(uint32_t vertexCount)
{
    const uint64_t need = static_cast<uint64_t>(vertexCount) * sizeof(float) * 3; // 12 bytes/vertex
    if (m_posByteHead + need > static_cast<uint64_t>(m_vertexBudget) * sizeof(float) * 3)
    {
        ENGINE_CHECK_ONCE(false,
            "SkinnedVertexRing position ring overflow (need %u, head %u, cap %u) — "
            "increase -skinvertexbudget; overflow falls back to rest pose",
            static_cast<uint32_t>(need), m_posByteHead,
            static_cast<uint32_t>(m_vertexBudget * sizeof(float) * 3));
        return ~0u;
    }
    const uint32_t byteOffset = m_posByteHead;
    m_posByteHead += static_cast<uint32_t>(need);
    return byteOffset;
}

uint32_t SkinnedVertexRing::AllocNormal(uint32_t vertexCount)
{
    const uint64_t need = static_cast<uint64_t>(vertexCount) * sizeof(float) * 3; // 12 bytes/vertex
    if (m_nrmByteHead + need > static_cast<uint64_t>(m_vertexBudget) * sizeof(float) * 3)
    {
        ENGINE_CHECK_ONCE(false,
            "SkinnedVertexRing normal ring overflow (need %u, head %u, cap %u) — "
            "increase -skinvertexbudget; overflow falls back to rest pose",
            static_cast<uint32_t>(need), m_nrmByteHead,
            static_cast<uint32_t>(m_vertexBudget * sizeof(float) * 3));
        return ~0u;
    }
    const uint32_t byteOffset = m_nrmByteHead;
    m_nrmByteHead += static_cast<uint32_t>(need);
    return byteOffset;
}

uint32_t SkinnedVertexRing::AllocTangent(uint32_t vertexCount)
{
    const uint64_t need = static_cast<uint64_t>(vertexCount) * sizeof(float) * 4; // 16 bytes/vertex
    if (m_tanByteHead + need > static_cast<uint64_t>(m_vertexBudget) * sizeof(float) * 4)
    {
        ENGINE_CHECK_ONCE(false,
            "SkinnedVertexRing tangent ring overflow (need %u, head %u, cap %u) — "
            "increase -skinvertexbudget; overflow falls back to rest pose",
            static_cast<uint32_t>(need), m_tanByteHead,
            static_cast<uint32_t>(m_vertexBudget * sizeof(float) * 4));
        return ~0u;
    }
    const uint32_t byteOffset = m_tanByteHead;
    m_tanByteHead += static_cast<uint32_t>(need);
    return byteOffset;
}

uint64_t SkinnedVertexRing::PosUAVHandleForSlot(uint32_t s) const { return m_posUAVHandles[s]; }
uint64_t SkinnedVertexRing::NrmUAVHandleForSlot(uint32_t s) const { return m_nrmUAVHandles[s]; }
uint64_t SkinnedVertexRing::TanUAVHandleForSlot(uint32_t s) const { return m_tanUAVHandles[s]; }
uint64_t SkinnedVertexRing::UploadTanSRVHandle()     const { return m_tanSRVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::PosSRVHandleForSlot(uint32_t slot) const { return m_posSRVHandles[slot % kFramesInFlight]; }
uint64_t SkinnedVertexRing::UploadPosSRVHandle()     const { return m_posSRVHandles[m_frameSlot]; }
uint64_t SkinnedVertexRing::UploadNrmSRVHandle()     const { return m_nrmSRVHandles[m_frameSlot]; }

const RHI::GPUBuffer& SkinnedVertexRing::UploadPosBuffer() const { return m_posBuffers[m_frameSlot]; }
const RHI::GPUBuffer& SkinnedVertexRing::UploadNrmBuffer() const { return m_nrmBuffers[m_frameSlot]; }
const RHI::GPUBuffer& SkinnedVertexRing::UploadTanBuffer() const { return m_tanBuffers[m_frameSlot]; }
const RHI::GPUBuffer& SkinnedVertexRing::PosBufferForSlot(uint32_t s) const { return m_posBuffers[s]; }
const RHI::GPUBuffer& SkinnedVertexRing::NrmBufferForSlot(uint32_t s) const { return m_nrmBuffers[s]; }
const RHI::GPUBuffer& SkinnedVertexRing::TanBufferForSlot(uint32_t s) const { return m_tanBuffers[s]; }
