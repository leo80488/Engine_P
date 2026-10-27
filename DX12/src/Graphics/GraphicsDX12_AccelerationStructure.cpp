#include "Graphics/GraphicsDX12.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "System/Log.h"

#include <vector>

// Ray-tracing acceleration structures behind IGraphicsDevice: DDGISceneAS records
// its BLAS / TLAS work through the RHI. Builds emit no barrier: the caller pushes
// GPUBarrier::Memory on what it built. The compacted-size query keeps its two
// transient buffers on the pool entry and issues its own transitions.

using Microsoft::WRL::ComPtr;

namespace
{
// The RHI values are D3D12's - a translation would be a cast, so check that it is.
static_assert(RHI::kASAlignment == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
              "RHI::kASAlignment is D3D12's acceleration-structure alignment");
static_assert(sizeof(RHI::ASInstance) == sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
              "RHI::ASInstance is D3D12_RAYTRACING_INSTANCE_DESC's 64-byte record");
static_assert(static_cast<uint32_t>(RHI::ASBuildFlags::ALLOW_UPDATE) ==
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE, "ASBuildFlags");
static_assert(static_cast<uint32_t>(RHI::ASBuildFlags::ALLOW_COMPACTION) ==
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION, "ASBuildFlags");
static_assert(static_cast<uint32_t>(RHI::ASBuildFlags::PREFER_FAST_TRACE) ==
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, "ASBuildFlags");
static_assert(static_cast<uint32_t>(RHI::ASBuildFlags::PREFER_FAST_BUILD) ==
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD, "ASBuildFlags");
static_assert(static_cast<uint32_t>(RHI::ASBuildFlags::PERFORM_UPDATE) ==
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE, "ASBuildFlags");
static_assert(RHI::ASInstanceFlag::TRIANGLE_CULL_DISABLE == D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE &&
              RHI::ASInstanceFlag::TRIANGLE_FRONT_CCW == D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE &&
              RHI::ASInstanceFlag::FORCE_OPAQUE == D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE &&
              RHI::ASInstanceFlag::FORCE_NON_OPAQUE == D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE,
              "RHI::ASInstanceFlag is D3D12_RAYTRACING_INSTANCE_FLAGS");

constexpr uint64_t AlignTo(uint64_t value, uint64_t align)
{ return (value + align - 1) & ~(align - 1); }

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ToD3D12(RHI::ASBuildFlags flags)
{ return static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(static_cast<uint32_t>(flags)); }

// A DEFAULT-heap buffer with UAV access, for acceleration-structure memory.
// CREATE_NOT_ZEROED: D3D12 zero-initializes committed resources by default, which
// for BLAS / scratch is serial main-thread CPU time while the GPU idles
// (re-allocating the skinned-BLAS pool after a world clear cost ~47 ms in one
// frame). Safe here: a build writes the whole result buffer before anything reads
// it, and scratch is write-before-read within one build. Unlike depth / stencil, no
// fixed-function unit reads these before a shader or AS build writes them.
bool CreateASBuffer(ID3D12Device* device, uint64_t sizeBytes, D3D12_RESOURCE_STATES initialState,
                    ComPtr<ID3D12Resource>& out)
{
    if (!device || sizeBytes == 0) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment          = 0;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc,
        initialState, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateAccelerationStructure: CreateCommittedResource failed (size=%llu, hr=0x%08X)",
                  (unsigned long long)sizeBytes, (unsigned)hr);
        return false;
    }
    GpuLedger::TrackCommitted(device, out.Get(), Diag::Prof::GpuMemCat::AccelStruct, "RT.AccelStructBuffer");
    return true;
}

// READBACK-heap buffer for the compacted-size query.
bool CreateReadbackBuffer(ID3D12Device* device, uint64_t sizeBytes, ComPtr<ID3D12Resource>& out)
{
    if (!device || sizeBytes == 0) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    const HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
    if (SUCCEEDED(hr)) GpuLedger::TrackCommitted(device, out.Get(), Diag::Prof::GpuMemCat::Readback, "RT.ReadbackBuffer");
    return SUCCEEDED(hr);
}

// The geometry descs a BLAS build or prebuild query reads. Transform3x4 is left
// null - instance transforms come from the TLAS.
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> TranslateGeometry(const RHI::ASTriangles* geoms, uint32_t count)
{
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const RHI::ASTriangles& g = geoms[i];
        D3D12_RAYTRACING_GEOMETRY_DESC d{};
        d.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = g.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                           : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

        auto& tri = d.Triangles;
        tri.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
        tri.VertexCount                = g.vertexCount;
        tri.VertexBuffer.StartAddress  = g.positionAddress;
        tri.VertexBuffer.StrideInBytes = g.positionStride;
        if (g.indexAddress && g.indexCount)
        {
            tri.IndexBuffer = g.indexAddress;
            tri.IndexCount  = g.indexCount;
            tri.IndexFormat = GraphicsDX12::ToDxgiFormat(g.indexFormat);
        }
        else
        {
            tri.IndexBuffer = 0; tri.IndexCount = 0;
            tri.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }
        out.push_back(d);
    }
    return out;
}

} // anonymous namespace

// ==== Prebuild sizes ====

bool GraphicsDX12::GetBLASPrebuildInfo(const RHI::ASTriangles* geoms, uint32_t count,
                                       RHI::ASBuildFlags flags, RHI::ASPrebuildInfo& out) const
{
    out = {};
    if (!m_dxrSupported || !geoms || count == 0) return false;
    ID3D12Device5* dev5 = GetDevice5();
    if (!dev5) return false;

    const auto geoArr = TranslateGeometry(geoms, count);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // Prebuild flags MUST match the build's or the reported sizes are wrong -
    // ALLOW_COMPACTION can change both result and scratch sizes.
    inputs.Flags          = ToD3D12(flags);
    inputs.NumDescs       = count;
    inputs.pGeometryDescs = geoArr.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    out.resultSize  = AlignTo(info.ResultDataMaxSizeInBytes, RHI::kASAlignment);
    out.scratchSize = AlignTo(info.ScratchDataSizeInBytes, RHI::kASAlignment);
    // Only meaningful when ALLOW_UPDATE was in the query's flags; D3D12 reports
    // 0 otherwise. Never fall back to scratchSize on 0 - a refit handed
    // build-sized scratch is fine, but a refit handed ZERO scratch when the
    // driver wanted some is memory corruption. Callers treat 0 as "this BLAS is
    // not refittable" and rebuild instead.
    out.updateScratchSize = AlignTo(info.UpdateScratchDataSizeInBytes, RHI::kASAlignment);
    return out.resultSize > 0;
}

bool GraphicsDX12::GetTLASPrebuildInfo(uint32_t maxInstances, RHI::ASBuildFlags flags,
                                       RHI::ASPrebuildInfo& out) const
{
    out = {};
    if (!m_dxrSupported || maxInstances == 0) return false;
    ID3D12Device5* dev5 = GetDevice5();
    if (!dev5) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags       = ToD3D12(flags);
    inputs.NumDescs    = maxInstances;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    out.resultSize        = AlignTo(info.ResultDataMaxSizeInBytes, RHI::kASAlignment);
    out.scratchSize       = AlignTo(info.ScratchDataSizeInBytes, RHI::kASAlignment);
    out.updateScratchSize = AlignTo(info.UpdateScratchDataSizeInBytes, RHI::kASAlignment);
    return out.resultSize > 0;
}

// ==== Memory ====

bool GraphicsDX12::CreateAccelerationStructure(uint64_t sizeBytes, RHI::ASMemory memory,
                                               RHI::AccelerationStructure& out)
{
    out = {};
    if (!m_dxrSupported || sizeBytes == 0) return false;
    // A RESULT is created in the state it lives in for good; a SCRATCH in COMMON,
    // from which a build promotes it to UNORDERED_ACCESS.
    ComPtr<ID3D12Resource> resource;
    if (!CreateASBuffer(m_device.Get(), sizeBytes,
                        memory == RHI::ASMemory::RESULT ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                                        : D3D12_RESOURCE_STATE_COMMON,
                        resource))
        return false;
    const uint64_t address = resource->GetGPUVirtualAddress();

    std::lock_guard<std::mutex> lock(m_asMutex);
    uint32_t id;
    if (!m_asFreeIds.empty())
    {
        id = m_asFreeIds.back();
        m_asFreeIds.pop_back();
    }
    else
    {
        id = static_cast<uint32_t>(m_asPool.size());
        m_asPool.emplace_back();
    }
    m_asPool[id].resource = std::move(resource);
    out.handleId = id;
    out.type      = RHI::GPUResource::Type::AccelerationStructure;
    out.sizeBytes = sizeBytes;
    out.address   = address;
    out.memory    = memory;
    return true;
}

ID3D12Resource* GraphicsDX12::LookupAccelerationStructure(const RHI::GPUResource& as) const
{
    std::lock_guard<std::mutex> lock(m_asMutex);
    return as.handleId < m_asPool.size() ? m_asPool[as.handleId].resource.Get() : nullptr;
}

// The pool entry's resources leave by value and the id goes back on the free
// list; the resources go to the deferred releases, so they outlive every list
// recorded with them.
void GraphicsDX12::TakeAccelerationStructure(RHI::AccelerationStructure& as,
                                             ComPtr<ID3D12Resource> (&out)[3])
{
    {
        std::lock_guard<std::mutex> lock(m_asMutex);
        if (as.handleId < m_asPool.size())
        {
            AccelStructDX12& e = m_asPool[as.handleId];
            out[0] = std::move(e.resource);
            out[1] = std::move(e.compactSizeUav);
            out[2] = std::move(e.compactSizeReadback);
            m_asFreeIds.push_back(as.handleId);
        }
    }
    as = {};
}

void GraphicsDX12::DestroyAccelerationStructure(RHI::AccelerationStructure& as)
{
    if (!as.IsValid()) return;
    ComPtr<ID3D12Resource> taken[3];
    TakeAccelerationStructure(as, taken);
    for (auto& r : taken)
        if (r) DeferReleaseResource(std::move(r));   // the current backbuffer slot's bucket
}

void GraphicsDX12::DestroyAccelerationStructure(RHI::AccelerationStructure& as, RHI::QueueType queue)
{
    if (!as.IsValid()) return;
    // Keyed on the queue's last signaled fence: work already submitted on it is
    // what can still read the memory (the async-compute AS builds' scratch, which
    // the backbuffer-slot window does not cover - see DeferReleaseResource).
    const uint32_t q  = static_cast<uint32_t>(queue);
    const uint64_t fv = GetLastSignaledFenceValue(q);
    ComPtr<ID3D12Resource> taken[3];
    TakeAccelerationStructure(as, taken);
    for (auto& r : taken)
        if (r) DeferReleaseResource(std::move(r), q, fv);
}

// ==== Builds ====

void GraphicsDX12::BuildBLAS(RHI::CommandList cmd, const RHI::ASTriangles* geoms, uint32_t count,
                             RHI::ASBuildFlags flags, const RHI::AccelerationStructure& dst,
                             const RHI::AccelerationStructure& scratch, uint64_t scratchOffset)
{
    if (!m_dxrSupported || !geoms || count == 0 || !dst.IsValid() || !scratch.IsValid()) return;
    // A refit is only defined if the ORIGINAL build carried ALLOW_UPDATE. Doing
    // it anyway is undefined behaviour, so fail closed rather than corrupt the AS.
    const bool performUpdate = RHI::HasFlag(flags, RHI::ASBuildFlags::PERFORM_UPDATE);
    if (performUpdate && !RHI::HasFlag(flags, RHI::ASBuildFlags::ALLOW_UPDATE)) return;
    // The sub-allocated region must actually lie inside the buffer.
    if (scratchOffset >= scratch.sizeBytes) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    const auto geoArr = TranslateGeometry(geoms, count);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.DestAccelerationStructureData    = dst.address;
    desc.ScratchAccelerationStructureData = scratch.address + scratchOffset;
    desc.Inputs.Type                      = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.DescsLayout               = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // PERFORM_UPDATE is OR'd ON TOP of the original flag set (ALLOW_UPDATE
    // included) - it does not replace it. Source == Dest is an in-place update,
    // which DXR explicitly permits.
    desc.Inputs.Flags                     = ToD3D12(flags);
    if (performUpdate)
        desc.SourceAccelerationStructureData = dst.address;
    desc.Inputs.NumDescs                  = count;
    desc.Inputs.pGeometryDescs            = geoArr.data();

    cl->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
}

void GraphicsDX12::BuildTLAS(RHI::CommandList cmd, const RHI::GPUBuffer& instances, uint32_t instanceCount,
                             RHI::ASBuildFlags flags, const RHI::AccelerationStructure& dst,
                             const RHI::AccelerationStructure& scratch)
{
    if (!m_dxrSupported || !instances.IsValid() || !dst.IsValid() || !scratch.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.DestAccelerationStructureData    = dst.address;
    desc.ScratchAccelerationStructureData = scratch.address;
    desc.Inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    desc.Inputs.Flags       = ToD3D12(flags);
    if (RHI::HasFlag(flags, RHI::ASBuildFlags::PERFORM_UPDATE))
        desc.SourceAccelerationStructureData = dst.address;
    desc.Inputs.NumDescs      = instanceCount;
    desc.Inputs.InstanceDescs = GetBufferGPUAddress(instances);

    cl->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
}

// ==== Compaction ====

bool GraphicsDX12::QueryCompactedSize(RHI::CommandList cmd, const RHI::AccelerationStructure& as)
{
    if (!m_dxrSupported || !as.IsValid()) return false;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return false;

    // The COMPACTED_SIZE postbuild query writes one UINT64 to a GPU-writable (UAV)
    // dest, which a READBACK buffer cannot be: emit into a tiny DEFAULT UAV buffer,
    // then copy it into a READBACK buffer the CPU maps a few frames later. A
    // DEFAULT-heap buffer is created in COMMON whatever state is requested (the debug
    // layer warns), so transition COMMON->UAV before the emit.
    constexpr uint64_t kSizeBytes = sizeof(UINT64);
    ComPtr<ID3D12Resource> uav, readback;
    if (!CreateASBuffer(m_device.Get(), kSizeBytes, D3D12_RESOURCE_STATE_COMMON, uav))
        return false;
    if (!CreateReadbackBuffer(m_device.Get(), kSizeBytes, readback))
        return false;   // nothing recorded yet: the UAV buffer is released here, unreferenced

    {
        D3D12_RESOURCE_BARRIER toUav{};
        toUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav.Transition.pResource   = uav.Get();
        toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &toUav);
        JournalNativeBarriers(cmd, &toUav, 1);   // the frame's barrier journal
    }

    // The caller's UAV barrier on `as` (after its build) makes it safe to read here.
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pbi{};
    pbi.InfoType   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    pbi.DestBuffer = uav->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS srcVA = as.address;
    cl->EmitRaytracingAccelerationStructurePostbuildInfo(&pbi, 1, &srcVA);

    // UAV barrier on the emit dest, then UAV->COPY_SOURCE so it can be copied
    // into the readback buffer. (No transition back - it is transient, released
    // after the CPU read.)
    D3D12_RESOURCE_BARRIER post[2]{};
    post[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    post[0].UAV.pResource          = uav.Get();
    post[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition.pResource   = uav.Get();
    post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    post[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(2, post);
    JournalNativeBarriers(cmd, post, 2);   // the frame's barrier journal

    cl->CopyBufferRegion(readback.Get(), 0, uav.Get(), 0, kSizeBytes);

    // The two buffers are named by the list now, so nothing here may drop them:
    // they move onto the pool entry, and whatever leaves (a query still pending
    // on this structure, or both when the entry is gone) goes to the deferred
    // releases - outside the pool lock.
    ComPtr<ID3D12Resource> leaving[2];
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(m_asMutex);
        if (as.handleId < m_asPool.size())
        {
            AccelStructDX12& e = m_asPool[as.handleId];
            leaving[0] = std::move(e.compactSizeUav);
            leaving[1] = std::move(e.compactSizeReadback);
            e.compactSizeUav      = std::move(uav);
            e.compactSizeReadback = std::move(readback);
            stored = true;
        }
    }
    if (!stored) { leaving[0] = std::move(uav); leaving[1] = std::move(readback); }
    for (auto& r : leaving)
        if (r) DeferReleaseResource(std::move(r));
    return stored;
}

uint64_t GraphicsDX12::ReadCompactedSize(const RHI::AccelerationStructure& as)
{
    ComPtr<ID3D12Resource> uav, readback;
    {
        std::lock_guard<std::mutex> lock(m_asMutex);
        if (!as.IsValid() || as.handleId >= m_asPool.size()) return 0;
        AccelStructDX12& e = m_asPool[as.handleId];
        uav      = std::move(e.compactSizeUav);
        readback = std::move(e.compactSizeReadback);
    }
    uint64_t compactedSize = 0;
    if (readback)
    {
        D3D12_RANGE rr{ 0, sizeof(uint64_t) };
        void* mapped = nullptr;
        if (SUCCEEDED(readback->Map(0, &rr, &mapped)) && mapped)
        {
            compactedSize = *static_cast<const uint64_t*>(mapped);
            D3D12_RANGE wr{ 0, 0 };
            readback->Unmap(0, &wr);
        }
    }
    // Released whatever the outcome (slot-based defer: still referenced by the
    // recent emit / copy list).
    if (uav)      DeferReleaseResource(std::move(uav));
    if (readback) DeferReleaseResource(std::move(readback));
    return compactedSize;
}

bool GraphicsDX12::CompactAccelerationStructure(RHI::CommandList cmd, const RHI::AccelerationStructure& src,
                                                uint64_t compactedSize, RHI::AccelerationStructure& out)
{
    out = {};
    if (!m_dxrSupported || !src.IsValid() || compactedSize == 0) return false;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return false;

    // Right-sized result (DEFAULT UAV, RAYTRACING_ACCELERATION_STRUCTURE state -
    // the same as a built BLAS). Committed-resource addresses are 64 KB-aligned,
    // so the 256 B acceleration-structure alignment is satisfied.
    if (!CreateAccelerationStructure(compactedSize, RHI::ASMemory::RESULT, out))
        return false;

    cl->CopyRaytracingAccelerationStructure(out.address, src.address,
                                            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
    return true;
}
