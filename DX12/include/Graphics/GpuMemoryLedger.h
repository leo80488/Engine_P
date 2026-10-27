#pragma once

// GPU memory ledger hooks - the D3D12 half of Diagnostics/ProfilerMemory.h.
//
// Call one right after a successful creation. Each adds the bytes to the ledger and
// attaches a small private-data object to the D3D12 object; D3D12 releases private
// data when the object is really destroyed, and that object's destructor subtracts
// the bytes. No release path needs a hook. An object that holds bytes is also
// registered, under @p name, in Diag::Prof's live-object list (what
// Profiler.MemoryTop and the profile summary's memory.top list).
//
// A no-op in a build without the profiler (Retail).

#include "Diagnostics/ProfilerMemory.h"

#include <cstdint>

struct ID3D12Device;
struct ID3D12Object;
struct ID3D12Resource;
struct ID3D12Heap;
struct ID3D12DescriptorHeap;
struct ID3D12QueryHeap;
struct D3D12_RESOURCE_DESC;

namespace GpuLedger
{
    // Any object, explicit size.
    void Track(ID3D12Object* obj, Diag::Prof::GpuMemCat cat, uint64_t bytes, const char* name = nullptr) noexcept;
    // A committed resource: its real footprint (GetResourceAllocationInfo).
    void TrackCommitted(ID3D12Device* dev, ID3D12Resource* res, Diag::Prof::GpuMemCat cat,
                        const char* name = nullptr) noexcept;
    // A committed texture: category from its flags (depth-stencil / render target / texture).
    void TrackTexture(ID3D12Device* dev, ID3D12Resource* res, const char* name = nullptr) noexcept;
    // A heap: its SizeInBytes.
    void TrackHeap(ID3D12Heap* heap, Diag::Prof::GpuMemCat cat, const char* name = nullptr) noexcept;
    // A placed resource: counted, no bytes (its heap already has them), not listed.
    void TrackPlaced(ID3D12Resource* res) noexcept;
    // A shader-visible descriptor heap: descriptors x increment size.
    void TrackDescriptorHeap(ID3D12Device* dev, ID3D12DescriptorHeap* heap, const char* name = nullptr) noexcept;

    // What this thread allocates from now on is also counted to secondary view @p slot (0: to none), until
    // the next call - the view's segment and its setup (Renderer). Returns the slot it replaces.
    uint32_t SetViewAttribution(uint32_t slot) noexcept;
    // The bytes view @p slot's live allocations hold (placed resources count none: their heap does).
    uint64_t ViewBytes(uint32_t slot) noexcept;

    // Logs "[LEDGER] residual: ..." - what the ledger still holds, by category and the largest names. Called after
    // the app (and so the device) is destroyed: bytes left are an object something kept alive.
    void LogResidual();
}
