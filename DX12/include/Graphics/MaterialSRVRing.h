#pragma once

// MaterialSRVRing - per-frame descriptor ring for custom-material texture tables; MaterialCBVRing's
// counterpart for CBV_SRV_UAV descriptors. BeginFrame resets the cursor into the current frame's
// slice of a static allocation, and Allocate(count) hands out a contiguous run of shader-visible
// descriptors.
// Usage:
//     ring.Init(gfx);
//     ring.BeginFrame(frameIdx);
//     auto slice = ring.Allocate(4);
//     // copy each texture's SRV into slice.cpu + i * slice.descIncBytes
//     dx12.BindCustomMaterialTextureTable(slice.gpu, cmdList);

#include "Graphics/FrameConstants.h"
#include <cstdint>
#include <d3d12.h>

#include "Graphics/DescriptorHeapAllocator.h"

class IGraphicsDevice;

// MaterialSRVStore - persistent per-material-slot custom-texture descriptor blocks: one static
// shader-visible allocation, bump-carved into blocks of GraphicsDX12::kCustomMatTextureSlots
// descriptors. A block stays with its material slot for the renderer's life (never freed) and is
// rewritten only when the slot is reassigned to a new MaterialBindingKey, which the Renderer defers
// past frames-in-flight. Pool exhausted -> the caller falls back to the per-frame MaterialSRVRing.
class MaterialSRVStore
{
public:
    struct Block
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};          // CopyDescriptors destination
        uint64_t                    gpu = 0;        // bind target
        uint32_t                    descIncBytes = 0;
    };

    bool Init(IGraphicsDevice& gfx, uint32_t maxTables = 1024);
    void Shutdown();
    bool IsValid() const { return m_maxTables != 0; }

    // Bump-allocates one block; ~0u once the pool is exhausted (warned once).
    uint32_t AllocateBlock();
    Block    GetBlock(uint32_t blockIdx) const;

    uint32_t GetUsedTables() const { return m_next; }
    uint32_t GetCapacityTables() const { return m_maxTables; }

private:
    DescriptorAllocation m_alloc;
    uint32_t m_maxTables     = 0;
    uint32_t m_next          = 0;
    uint32_t m_descIncBytes  = 0;
    bool     m_exhaustWarned = false;
};

class MaterialSRVRing
{
public:
    struct Slice
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;  // starting CPU handle (for CopyDescriptors)
        uint64_t                    gpu;  // starting GPU handle (bind target)
        uint32_t                    descIncBytes;  // advance between slots
    };

    // Reserves @p maxMatsPerFrame tables of GraphicsDX12::kCustomMatTextureSlots descriptors per frame,
    // for each of kFramesInFlight frames (512 x 8 = 4096 per frame), enough for scenes with several
    // multi-material characters.
    bool Init(IGraphicsDevice& gfx, uint32_t maxMatsPerFrame = 512);
    void Shutdown();

    void BeginFrame(uint32_t frameIdx);

    // Allocates @p count contiguous descriptors from the current frame's slice; {} when full.
    Slice Allocate(uint32_t count);

    uint32_t GetUsedDescriptors() const { return m_cursor; }
    uint32_t GetCapacityDescriptors() const { return m_descPerFrame; }

    // Handle of the default white-texture descriptor for unused custom-texture slots. Nothing sets it,
    // and Allocate does not read it.
    D3D12_CPU_DESCRIPTOR_HANDLE GetFallbackCpuHandle() const { return m_fallbackCpu; }
    void SetFallbackCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE h) { m_fallbackCpu = h; }

private:
    static constexpr uint32_t kFramesInFlight = RHI::kFramesInFlight;

    DescriptorAllocation      m_frames[kFramesInFlight]{};
    uint32_t                  m_descPerFrame   = 0;
    uint32_t                  m_descIncBytes   = 0;
    uint32_t                  m_cursor         = 0;
    uint32_t                  m_current        = 0;
    bool                      m_overflowWarned = false;
    D3D12_CPU_DESCRIPTOR_HANDLE m_fallbackCpu  = {};
};
