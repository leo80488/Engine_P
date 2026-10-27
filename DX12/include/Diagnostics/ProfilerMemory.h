#pragma once

// Diag::Prof memory accounting.
//
// GPU RESOURCE LEDGER. The device reports each GPU allocation where it is created
// (Graphics/GpuMemoryLedger.h attaches a private-data object to the resource or heap), and the
// SUBTRACTION happens when D3D12 really destroys the object and releases that private data -
// so every release path is covered without hooking any: the deferred-release drain three
// frames after a Destroy*, a shared heap's last reference, shutdown's wholesale pool clear.
// Memory waiting in the deferred queue therefore counts as live, which is what the GPU has.
//
// Placed resources are COUNTED but add no bytes: their heap is already in the ledger
// (TransientHeap / BufferHeap), and adding both would double count. Committed resources use
// GetResourceAllocationInfo (>= 64 KiB - an 8-byte buffer really costs 64 KiB), heaps their
// SizeInBytes.
//
// The ledger CANNOT see memory third-party SDKs allocate themselves (Streamline / DLSS /
// Frame Generation, FidelityFX), FFmpeg's decoder surfaces, ImGui's own buffers, Tracy's
// query heap, or driver-internal memory (PSOs, root signatures, RT state objects, command
// allocators). The collector reports DXGI's local usage minus the ledger's local categories
// as "unattributed" - expected to be large, and expected to be STABLE.
//
// GAUGES. Per-frame values a subsystem already knows (Lua heap, the Jolt step arena's peak,
// live shader-visible descriptors): SetGauge from any thread, last write wins, snapshotted
// into the frame record at the boundary; a gauge nobody set is absent (NaN), not 0.
//
// All of it is always on in a profiler build (a relaxed atomic add per resource creation /
// release, not per frame), because resources are created long before anyone turns a profiler
// level on. Retail: every call is a no-op.

#include <cstdint>
#include <string>
#include <vector>

namespace Diag::Prof
{
    enum class GpuMemCat : uint8_t
    {
        Texture = 0,      // committed, sampled / UAV
        RenderTarget,     // committed, ALLOW_RENDER_TARGET
        DepthStencil,     // committed, ALLOW_DEPTH_STENCIL
        Buffer,           // committed DEFAULT buffer
        Upload,           // UPLOAD heap (system memory on a discrete GPU)
        Readback,         // READBACK heap (system memory)
        AccelStruct,      // BLAS / TLAS / scratch
        TransientHeap,    // alias heaps placed textures live in (arena, SSR)
        BufferHeap,       // heap-batch heaps placed buffers live in
        QueryHeap,
        DescriptorHeap,   // shader-visible descriptor heaps only (CPU-only ones are system RAM)
        SwapChain,        // back buffers (owned by DXGI; tagged when fetched)
        Other,
        Count
    };
    constexpr int kGpuMemCats = static_cast<int>(GpuMemCat::Count);
    const char* GpuMemCatName(GpuMemCat c) noexcept;
    // Upload / Readback live in system memory on a discrete GPU (DXGI's
    // NON_LOCAL segment); everything else is local VRAM.
    inline bool GpuMemCatIsLocal(GpuMemCat c) noexcept
    {
        return c != GpuMemCat::Upload && c != GpuMemCat::Readback;
    }

    void NoteGpuAlloc(GpuMemCat c, uint64_t bytes) noexcept;
    void NoteGpuFree(GpuMemCat c, uint64_t bytes) noexcept;
    void NoteGpuPlaced(int delta) noexcept;          // +1 created, -1 destroyed
    // Staging bytes copied to the GPU; summed per frame window.
    void NoteUploadBytes(uint64_t bytes) noexcept;

    struct GpuLedger
    {
        uint64_t bytes[kGpuMemCats]{};
        int64_t  count[kGpuMemCats]{};
        int64_t  placed = 0;
        uint64_t localBytes = 0, nonLocalBytes = 0, totalBytes = 0;
    };
    void ReadGpuLedger(GpuLedger& out) noexcept;

    // LIVE GPU OBJECTS: the objects behind the ledger that hold bytes (committed resources,
    // heaps, shader-visible descriptor heaps; a placed resource's bytes are its heap's), with the
    // name the engine created them under, so a report can list the largest.
    // Graphics/GpuMemoryLedger.cpp registers each when it tags it and unregisters it when D3D12
    // destroys it; the registry outlives static destruction (an object may be released after it).
    struct GpuObjectInfo
    {
        std::string name;                    // empty: created without one
        GpuMemCat   cat   = GpuMemCat::Other;
        uint64_t    bytes = 0;
    };
    uint64_t RegisterGpuObject(GpuMemCat c, uint64_t bytes, const char* name) noexcept;   // 0 = not registered
    void     UnregisterGpuObject(uint64_t id) noexcept;
    // The @p n largest live objects (0 = all), largest first.
    void     ReadGpuObjects(std::vector<GpuObjectInfo>& out, size_t n);

    enum class Gauge : uint8_t
    {
        LuaKB = 0,             // lua_gc(LUA_GCCOUNT): the Lua heap
        JoltStepPeakKB,        // the Jolt step arena's high-water mark
        DescriptorsLive,       // CBV/SRV/UAV shader-visible slots in use (not the high-water cursor)
        DescriptorsCapacity,
        Count
    };
    constexpr int kGauges = static_cast<int>(Gauge::Count);
    const char* GaugeName(Gauge g) noexcept;
    void SetGauge(Gauge g, double value) noexcept;
    bool ReadGauge(Gauge g, double& value) noexcept;   // false = never set
}
