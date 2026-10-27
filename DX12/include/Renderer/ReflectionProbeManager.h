#pragma once

// ReflectionProbeManager - owns the probe pool's GPU resources:
//   - TextureCubeArray (kMaxReflectionProbes x 6 faces x kProbeCubemapMips) that
//     every probe samples via its slice index.
//   - StructuredBuffer<GpuShared::ReflectionProbe> - UPLOAD heap, persistently mapped,
//     rewritten each frame with active probe positions + AABB / influence data.
//   - ReflectionProbeCapturePass instance (PSO + temp cube + depth buffer used to
//     render each bake request).
//   - Bake queue - FIFO of slice indices waiting for a (re)bake. BuildScene pushes
//     dirty slices; Render() pops at most one per frame.
//
// Orchestration (walking ECS, packing probes, driving a bake, setting up the
// BakeContext) lives in Renderer because it coordinates with ClusterPass,
// SkyIBLPass, SceneBVH and the current frame's DrawList; this class only holds the
// GPU state and the bake queue.

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ReflectionProbeTypes.h"   // GpuShared::ReflectionProbe
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include <cstdint>
#include <vector>

class IGraphicsDevice;

class ReflectionProbeManager
{
public:
    // Ring depth (RHI::kFramesInFlight): BuildScene_UploadProbes rewrites every
    // visible slot each frame (no dirty gate), so a single UPLOAD buffer would race
    // the GPU still sampling frame N's data in LightingPass / ClusterPass /
    // TransparentPass.
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    // Create the cubemap array + StructuredBuffer and initialise the capture pass.
    // Returns false if a StructuredBuffer or the capture pass fails; a cube-array or
    // SRV-block failure is only logged. Either way the caller can proceed without
    // probes.
    bool Init(IGraphicsDevice& gfx);

    // Tear down GPU resources (unmap + destroy each ring slot). Idempotent.
    void Shutdown(IGraphicsDevice& gfx);

    // Raw UPLOAD-buffer pointer for per-frame packing: the slot GetUploadSlot()
    // names (the GAME half's). Writes to dst[slice] are visible to the GPU this
    // frame. Returns nullptr if Init() did not create the StructuredBuffer.
    GpuShared::ReflectionProbe* GetUploadPointer(IGraphicsDevice& gfx) const;

    // Set / get the number of slots the shaders will read. BuildScene sets
    // this after packing; the cluster pass + lighting shader honour it.
    void     SetActiveProbeCount(uint32_t n) { m_activeCount = n; }
    uint32_t GetActiveProbeCount() const     { return m_activeCount; }

    // Enqueue a slice for baking. Deduped - slices already in the queue are
    // ignored. Out-of-range slices are dropped.
    void EnqueueBake(uint32_t cubeSlice);

    // Peek the next pending bake slice without removing it. Returns
    // ~0u when empty. Paired with PopBake() when the caller has confirmed
    // it can service the request this frame.
    uint32_t PeekBake() const;

    // Remove the front slice from the queue. No-op on empty queue.
    void PopBake();

    bool BakeQueueEmpty() const { return m_bakeQueue.empty(); }

    // GPU accessors - valid after Init() returns.
    RHI::Texture&       GetArrayTexture()         { return m_array; }
    const RHI::Texture& GetArrayTexture()   const { return m_array; }
    uint64_t            GetArraySrv()       const { return m_arraySrv;  }

    // Absolute shader-visible-heap index of the per-cube bindless SRV for `slice`
    // (a TextureCubeArray view with NumCubes=1 over the shared array). Packed into
    // GpuShared::ReflectionProbe::cubemapSrvIndex; shaders sample it via
    // ResourceDescriptorHeap[NonUniformResourceIndex(idx)]. Returns 0 if the block
    // failed to allocate or the slice is out of range (a valid-but-wrong index; the
    // BAKED gate prevents sampling unbaked probes).
    uint32_t GetCubeBindlessIndex(uint32_t slice) const
    {
        return (m_cubeSrvBlock.IsValid() && slice < Reflection::kMaxReflectionProbes)
                   ? (m_cubeSrvBaseIndex + slice) : 0u;
    }
    // Per-frame SRV of the probe StructuredBuffer for the ring slot matching
    // gfx.GetFrameIndex(): the RENDER half's accessor (LightingPass /
    // TransparentPass resolve it inside Renderer::Render).
    uint64_t            GetBufferSrv(IGraphicsDevice& gfx) const;
    // The same ring's SRV for the slot GetUploadPointer() packs (GetUploadSlot): the
    // GAME half's accessor, which BuildScene_UploadProbes hands to ClusterPass with
    // that frame's count. Do not use GetBufferSrv() there: it names the previous
    // frame's slot - last frame's table under this frame's count, in a slot the CPU
    // may rewrite before the GPU culls with it.
    uint64_t            GetUploadBufferSrv(IGraphicsDevice& gfx) const;

    // Capture pass instance - exposed so ProcessProbeBakeQueue can call BakeProbe()
    // on it with a context it builds from the scene state.
    ReflectionProbeCapturePass& GetCapturePass()  { return m_capturePass; }

private:
    RHI::Texture                m_array;
    uint64_t                    m_arraySrv    = 0;

    // Per-cube bindless SRV block (static region of the shader-visible heap):
    // kMaxReflectionProbes contiguous TextureCubeArray views, one per cube slice.
    // cubemapSrvIndex(slice) = m_cubeSrvBaseIndex + slice.
    RHI::DescriptorBlock        m_cubeSrvBlock;
    uint32_t                    m_cubeSrvBaseIndex = 0;

    // Ring of kFrameCount UPLOAD StructuredBuffers - one physical buffer + mapped
    // pointer + SRV per frame slot.
    RHI::GPUBuffer                  m_buffer[kFrameCount];
    GpuShared::ReflectionProbe* m_bufferMapped[kFrameCount] = {};
    uint64_t                        m_bufferSrv[kFrameCount]    = {};

    uint32_t                    m_activeCount = 0;
    std::vector<uint32_t>       m_bakeQueue;
    ReflectionProbeCapturePass  m_capturePass;
};
