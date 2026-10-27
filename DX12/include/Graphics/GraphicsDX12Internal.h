#pragma once

// Internal pool records and root-signature slot constants shared by the GraphicsDX12*.cpp
// sources. Not part of the public IGraphicsDevice surface: kept out of GraphicsDX12.h so callers
// do not see the DX12 pool internals.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <wrl.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "d3d12.h"
#include "Graphics/DescriptorHeapAllocator.h"
#include "Graphics/RootSignatureLayout.h"

// ==== Pool-backed resource records (index == GPUResource::handleId) ====
struct TextureDX12
{
    // Non-null for a placed resource in a shared aliasing heap (CreateTexturePlaced): keeps the heap
    // alive, and DestroyTexture defers its release until after the resource (D3D12 requires the heap
    // to outlive its placed resources). Do not move below `resource`: members destruct in reverse
    // order, so the raw pool clear at shutdown releases the resource first and the heap last (the
    // rule DeferredReleaseList keeps by declaration order).
    Microsoft::WRL::ComPtr<ID3D12Heap>     placedHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES   state = D3D12_RESOURCE_STATE_COMMON;
    // A depth-stencil format has a state per plane: `state` is plane 0 (depth; the whole texture for
    // any other format), `stencilState` plane 1. planes = 2 only for such a format
    // (RHI::HasStencilPlane). A barrier on every plane while the two differ is split per plane
    // (GraphicsDX12::TranslateBarrier).
    D3D12_RESOURCE_STATES   stencilState = D3D12_RESOURCE_STATE_COMMON;
    uint8_t                 planes = 1;
    DescriptorAllocation    srv;        // CBV_SRV_UAV shader-visible heap (native fmt incl. SRGB)
    DescriptorAllocation    previewSrv; // UNORM alias SRV for editor preview (SRGB only)
    DescriptorAllocation    stencilSrv; // stencil-plane view (X32_TYPELESS_G8X24 / X24_TYPELESS_G8)
    // NV12 UV-plane SRV (R8G8_UNORM, PlaneSlice=1); for NV12, srv holds the Y plane (R8_UNORM,
    // PlaneSlice=0). Empty for every other format.
    DescriptorAllocation    uvPlaneSrv;
    DescriptorAllocation    uav;
    DescriptorAllocation    rtv;
    DescriptorAllocation    dsv;
    // Read-only DSVs of a 2D, non-array depth texture, picked by RHI::DepthView: [0]
    // READ_ONLY_DEPTH, [1] READ_ONLY_STENCIL, [2] both ([1] and [2] only with a stencil plane).
    DescriptorAllocation    dsvReadOnly[3];
    // Per-mip UAVs (only when mipLevels > 1 + UAV bind flag).
    std::vector<DescriptorAllocation> mipUavs;
    // Per-array-slice DSVs (only when arraySize > 1 + DEPTH_STENCIL).
    std::vector<DescriptorAllocation> sliceDsvs;
    // Lazy per-(cube, face, mip) RTVs; key = (cubeIdx<<16)|(face<<8)|mip.
    std::unordered_map<uint32_t, DescriptorAllocation> cubeFaceRtvs;
    // Lazy per-(cube, mip) UAVs; key = (cubeIdx<<8)|mip.
    std::unordered_map<uint32_t, DescriptorAllocation> cubeMipUavs;
    // generation: stamped from the device's counter when this record gets its texture, so a queued
    // stream upload naming a reused handle id is told apart from the id's new owner (0 = never
    // stamped). streamTarget: set by the first QueueTextureUpload; UpdateTexture and
    // CopyD3D12ResourceToTexture then fail an ENGINE_CHECK on the texture, because their direct
    // submission runs before this frame's queued copies and would reverse the order of the data.
    uint32_t                generation = 0;
    bool                    streamTarget = false;
    // Set when DrainReleasesUpTo puts this record's id on the free list; the Create* that takes the
    // id writes a fresh record, which clears it. An id pushed twice would go to two textures.
    bool                    onFreeList = false;
};

struct GPUBufferDX12
{
    // Non-null for a buffer suballocated as a placed resource in a shared heap
    // (BeginBufferHeapBatch): keeps the heap alive, and DestroyBuffer defers its release until after
    // the resource. Do not move below `resource`: reverse-order member destruction must release the
    // placed resource first and the heap last (the raw pool-clear shutdown path).
    Microsoft::WRL::ComPtr<ID3D12Heap>     placedHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    // Write-only: do not read this to decide a transition. TranslateBarrier stamps each barrier's
    // `after` state, but nothing consumes it and it is not kept honest: buffer states decay to
    // COMMON at every ExecuteCommandLists boundary. (TextureDX12::state is read and reliable; the two
    // are not equivalent.) Real buffer-state tracking would have to be added deliberately.
    D3D12_RESOURCE_STATES    state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW  ibv{};
    DescriptorAllocation     cbv;
    DescriptorAllocation     srv;   // structured / typed
    DescriptorAllocation     uav;
    DescriptorAllocation     clearUav;   // the raw view a UAV_CLEARABLE buffer clears through
    // Set when DrainReleasesUpTo puts this record's id on the free list; the CreateBuffer that takes
    // the id writes a fresh record, which clears it. An id pushed twice would go to two buffers.
    bool                     onFreeList = false;
};

struct ShaderDX12
{
    std::vector<uint8_t> bytecode;
    // FNV-1a of the bytecode, mixed into ComputePSOName for every stage: PSO disk-library names stay
    // stable across runs (pso_cache.bin does not grow) and change when the DXIL does.
    uint64_t             bytecodeHash = 0;
};

struct PipelineStateDX12
{
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  pso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  rootSignature;
    bool                                          isCompute = false;
    // What a draw with this PSO writes through the bound DSV: the PSO half of the draw-time depth
    // tripwire (CommandListDX12::depthView).
    bool                                          writesDepth   = false;
    bool                                          writesStencil = false;
    uint8_t                                       stencilWriteMask = 0;   // its bits; 0 when it writes none
};

// One pipeline layout. rootSignature is null once destroyed (the slot stays retired).
struct PipelineLayoutDX12
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  rootSignature;
    std::string                                   name;
    uint64_t                                      blobHash  = 0;
    uint32_t                                      blobBytes = 0;
    uint32_t                                      dwords    = 0;
};


// ==== Default (PVF) root-signature slot constants ====
// Backend-local spellings of the shared table in Graphics/RootSignatureLayout.h, the source of
// truth that CreateDefaultRootSignature() builds from; do not give any of them an independent
// value. They are the raw D3D12 parameter indices (`.index` of the typed slots): the backend
// fills D3D12_ROOT_PARAMETER arrays and calls SetGraphicsRoot* with them, the one place a slot
// is a number.
static constexpr UINT kRootConstantsSlot   = RHI::Slots::Gfx::RootConstants.index;
static constexpr UINT kCBVSlotBase         = RHI::Slots::Gfx::CBVBase;         // root 1-7 -> b1-b7
static constexpr UINT kCBVSlotCount        = RHI::Slots::Gfx::CBVCount;
static constexpr UINT kInstanceBufSlot     = RHI::Slots::Gfx::InstanceBuf.index;     // root 8   -> t0 space0
static constexpr UINT kMeshDescSlot        = RHI::Slots::Gfx::MeshDesc.index;        // root 9   -> t1 space0
static constexpr UINT kSRVSlotBase         = RHI::Slots::Gfx::SRVBase;         // root 10-13 -> t2-t5 space0
static constexpr UINT kSRVSlotCount        = RHI::Slots::Gfx::SRVCount;
static constexpr UINT kBindlessSlot        = RHI::Slots::Gfx::BindlessBuffers.index; // root 14  -> t0 space1
static constexpr UINT kIBLSRVSlotBase      = RHI::Slots::Gfx::IBLSRVBase;      // root 15-17 -> t6-t8 space0
static constexpr UINT kIBLSRVSlotCount     = RHI::Slots::Gfx::IBLSRVCount;
static constexpr UINT kShadowSRVSlot       = RHI::Slots::Gfx::ShadowSRV.index;       // root 18  -> t9-t11 space0
static constexpr UINT kShadowSRVCount      = RHI::Slots::Gfx::ShadowSRVCount;
static constexpr UINT kClusterSRVSlotBase  = RHI::Slots::Gfx::ClusterSRVBase;  // root 19-21 -> t13-t15 space0
static constexpr UINT kClusterSRVSlotCount = RHI::Slots::Gfx::ClusterSRVCount;
// Must match kMaxBuffers in MeshDescriptorHeap.h, g_Buffers[] in
// pvf_fetch.hlsli, and g_DDGIBuffers[] in DDGIRayTrace.cs.hlsl.
static constexpr UINT kMaxBindlessBuffers  = RHI::Slots::MaxBindlessBuffers;
