#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"     // DX12 helpers: descriptor-block fill, native lists
#include "ECS/DDGIComponents.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <cmath>

using namespace DirectX;

namespace DDGI
{

// ==== Init / Shutdown ====

bool DDGIVolumeManager::Init(IGraphicsDevice& gfx)
{
    // Engine-wide volume desc buffer (kMaxVolumes entries) in an UPLOAD heap, so the CPU
    // rewrites it each frame without staging copies; one per frame in flight, written
    // every frame by Tick().
    RHI::GPUBufferDesc desc{};
    desc.size       = uint64_t(kMaxVolumes) * sizeof(VolumeGPUDesc);
    desc.stride     = sizeof(VolumeGPUDesc);
    desc.usage      = RHI::Usage::UPLOAD;
    desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    desc.debugName = "DDGI.VolumeDescs";
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (!gfx.CreateBuffer(desc, m_volumeBuffer[i]))
        {
            LOG_ERROR("DDGIVolumeManager: volume buffer create failed (slot %u)", i);
            return false;
        }
        m_volumeBufferMapped[i] = gfx.MapBuffer(m_volumeBuffer[i]);
        m_volumeBufferSrv[i]    = gfx.GetBufferSRVGpuHandle(m_volumeBuffer[i]);
        if (m_volumeBufferMapped[i])
            std::memset(m_volumeBufferMapped[i], 0, desc.size);
    }

    // The three descriptor blocks (kMaxVolumes contiguous SRVs each: irradiance / depth /
    // probe data), every slot pre-filled with a type-correct null SRV: the shader never
    // reads an unallocated slot, but a bound table must hold only valid descriptors.
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    DescriptorHeapAllocator& alloc = dx12.GetCbvSrvUavAllocator();

    constexpr uint32_t kProbeDataStride = sizeof(ProbeData);
    constexpr DXGI_FORMAT kDepthFormat  = DXGI_FORMAT_R16G16_FLOAT;
    // The irradiance atlas is RGBA16F, not R11G11B10_FLOAT: the border kernel does a typed
    // UAV load of it, and RGBA16F is in the format set every FL11.0 device supports for
    // that. See DDGICommon.hlsli.
    constexpr DXGI_FORMAT kIrrFormat    = DXGI_FORMAT_R16G16B16A16_FLOAT;

    auto allocAndNullFill = [&](DescriptorAllocation& outAlloc,
                                uint64_t&             outGpu,
                                auto                  fillNullFn)
    {
        outAlloc = alloc.AllocateStatic(kMaxVolumes);
        if (!outAlloc.IsValid())
        {
            LOG_ERROR("DDGIVolumeManager: descriptor table allocation failed (kMaxVolumes=%u)", kMaxVolumes);
            return false;
        }
        const UINT descSize = alloc.GetDescriptorSize();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = outAlloc.GetGpuCpuHandle();
        for (uint32_t i = 0; i < kMaxVolumes; ++i)
        {
            fillNullFn(cpu);
            cpu.ptr += descSize;
        }
        outGpu = outAlloc.GetGpuHandle().ptr;
        return true;
    };

    if (!allocAndNullFill(m_irradianceTable, m_irradianceTableGpu,
            [&](D3D12_CPU_DESCRIPTOR_HANDLE h) { dx12.CreateNullTexture2DSRVAtCpu(kIrrFormat, h); }))
        return false;
    if (!allocAndNullFill(m_depthTable, m_depthTableGpu,
            [&](D3D12_CPU_DESCRIPTOR_HANDLE h) { dx12.CreateNullTexture2DSRVAtCpu(kDepthFormat, h); }))
        return false;
    if (!allocAndNullFill(m_probeDataTable, m_probeDataTableGpu,
            [&](D3D12_CPU_DESCRIPTOR_HANDLE h) { dx12.CreateNullStructuredBufferSRVAtCpu(kProbeDataStride, h); }))
        return false;

    LOG_INFO("DDGIVolumeManager: ready (max %u volumes; Irradiance/Depth/ProbeData tables allocated)", kMaxVolumes);
    // Build marker: grep DX12Log.txt for it to confirm the running exe is the expected
    // DDGI build.
    LOG_INFO("DDGIVolumeManager: [stability-build 2026-05-14d] adaptive off + rotation amplitude scaled to ray count");
    return true;
}

void DDGIVolumeManager::Shutdown(IGraphicsDevice& gfx)
{
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated)
            DestroyVolume(gfx, m_volumes[i]);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_volumeBuffer[i].IsValid())
        {
            if (m_volumeBufferMapped[i]) gfx.UnmapBuffer(m_volumeBuffer[i]);
            gfx.DestroyBuffer(m_volumeBuffer[i]);
            m_volumeBufferMapped[i] = nullptr;
            m_volumeBufferSrv[i]    = 0;
        }
    }
    if (m_irradianceTable.IsValid())   m_irradianceTable.Free();
    if (m_depthTable.IsValid())     m_depthTable.Free();
    if (m_probeDataTable.IsValid()) m_probeDataTable.Free();
    m_irradianceTableGpu = m_depthTableGpu = m_probeDataTableGpu = 0;
    m_activeCount = 0;
    m_dbgReadback.Reset();
    m_watchReadback.Reset();
    m_watchInFlight = false;
    m_dbgArmedSlot = m_dbgInFlightSlot = 0xFFFFFFFFu;
}

// Writes the slot's three SRVs at offset @p slot of the blocks once its resources
// exist; the blocks' GPU handles never change.
void DDGIVolumeManager::WriteSlotDescriptors(IGraphicsDevice& gfx, uint32_t slot,
                                             const VolumeResources& res)
{
    if (slot >= kMaxVolumes) return;
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    const UINT descSize = dx12.GetCbvSrvUavAllocator().GetDescriptorSize();
    auto offsetCpu = [&](D3D12_CPU_DESCRIPTOR_HANDLE base, uint32_t s)
    {
        base.ptr += SIZE_T(s) * descSize;
        return base;
    };

    if (m_irradianceTable.IsValid())
        dx12.CreateTexture2DSRVAtCpu(res.irradianceAtlas,
            offsetCpu(m_irradianceTable.GetGpuCpuHandle(), slot));
    if (m_depthTable.IsValid())
        dx12.CreateTexture2DSRVAtCpu(res.depthAtlas,
            offsetCpu(m_depthTable.GetGpuCpuHandle(), slot));
    if (m_probeDataTable.IsValid())
        dx12.CreateStructuredBufferSRVAtCpu(res.probeDataBuffer,
            offsetCpu(m_probeDataTable.GetGpuCpuHandle(), slot));
}

void DDGIVolumeManager::WriteSlotNullDescriptors(IGraphicsDevice& gfx, uint32_t slot)
{
    if (slot >= kMaxVolumes) return;
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    const UINT descSize = dx12.GetCbvSrvUavAllocator().GetDescriptorSize();
    auto offsetCpu = [&](D3D12_CPU_DESCRIPTOR_HANDLE base, uint32_t s)
    {
        base.ptr += SIZE_T(s) * descSize;
        return base;
    };

    constexpr uint32_t kProbeDataStride = sizeof(ProbeData);
    constexpr DXGI_FORMAT kDepthFormat  = DXGI_FORMAT_R16G16_FLOAT;
    constexpr DXGI_FORMAT kIrrFormat    = DXGI_FORMAT_R16G16B16A16_FLOAT;

    if (m_irradianceTable.IsValid())
        dx12.CreateNullTexture2DSRVAtCpu(kIrrFormat,
            offsetCpu(m_irradianceTable.GetGpuCpuHandle(), slot));
    if (m_depthTable.IsValid())
        dx12.CreateNullTexture2DSRVAtCpu(kDepthFormat,
            offsetCpu(m_depthTable.GetGpuCpuHandle(), slot));
    if (m_probeDataTable.IsValid())
        dx12.CreateNullStructuredBufferSRVAtCpu(kProbeDataStride,
            offsetCpu(m_probeDataTable.GetGpuCpuHandle(), slot));
}

// ==== Allocation ====

bool DDGIVolumeManager::CreateAtlasesForVolume(IGraphicsDevice& gfx,
                                               const DDGIVolumeComponent& v,
                                               VolumeResources& res)
{
    const uint32_t pcx = v.probeCountsX;
    const uint32_t pcy = v.probeCountsY;
    const uint32_t pcz = v.probeCountsZ;
    if (pcx == 0 || pcy == 0 || pcz == 0)
    {
        LOG_ERROR("DDGIVolumeManager: zero probe count not allowed (%u,%u,%u)", pcx, pcy, pcz);
        return false;
    }
    const uint32_t totalProbes = pcx * pcy * pcz;
    const uint32_t pcyPcz      = pcy * pcz;

    // ---- Irradiance atlas (RGBA16F octahedral) ----
    // kIrradianceProbeStride^2 texels per probe: a 6x6 patch plus a 1-texel mirrored
    // border so hardware bilinear is legal across the seam. Zero-init: a new volume reads
    // black until the first relight.
    //
    // Created in SHADER_RESOURCE, not UNORDERED_ACCESS like the depth atlas below; getting
    // it wrong trips a state mismatch on the first frame (or silently corrupts on a driver
    // that does not validate). The two atlases follow different state machines:
    //   * depthAtlas - TransitionVolumeAtlases tracks its state in r.atlasState, so it may
    //     start anywhere; it starts at UAV because the relight touches it first.
    //   * this atlas - DDGIPass does not track it: it assumes kSHReadState at the top of
    //     every Execute (its first touch is the trace's SRV read) and issues a bare
    //     StateBefore = kSHReadState transition after the trace. A texture has no
    //     implicit promotion / decay, so the creation state must make that true on frame 1.
    // On the compute queue buffersInComputeOnlyState starts false, so DemoteForComputeQueue
    // narrows NPSR|PSR -> NPSR on frame 1 before the DDGI list runs - exactly kSHReadState
    // there.
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.width      = IrradianceAtlasWidth (pcx, pcyPcz);
        td.height     = IrradianceAtlasHeight(pcx, pcyPcz);
        td.arraySize = 1;
        td.mipLevels = 1;
        td.usage      = RHI::Usage::DEFAULT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;   // see the note above
        td.debugName = "DDGI.IrradianceAtlas";
        // RGBA16F = 8 bytes per pixel.
        std::vector<uint8_t> zeros(uint64_t(td.width) * td.height * 8, 0);
        RHI::SubresourceData init{ zeros.data(), td.width * 8u, 0u };
        if (!gfx.CreateTexture(td, res.irradianceAtlas, &init))
        {
            LOG_ERROR("DDGIVolumeManager: irradiance atlas create failed (%ux%u)",
                      td.width, td.height);
            return false;
        }
        res.irradianceAtlasSrv = gfx.GetTextureSRVGpuHandle(res.irradianceAtlas);
        res.irradianceAtlasUav = gfx.GetTextureUAVGpuHandle(res.irradianceAtlas);
    }

    // ---- Depth atlas (R16G16F) ----
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.format     = RHI::Format::R16G16_FLOAT;
        td.width      = DepthAtlasWidth (pcx, pcyPcz);
        td.height     = DepthAtlasHeight(pcx, pcyPcz);
        td.arraySize = 1;
        td.mipLevels = 1;
        td.usage      = RHI::Usage::DEFAULT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = "DDGI.DepthAtlas";
        // R16G16F = 4 bytes per pixel.
        std::vector<uint8_t> zeros(uint64_t(td.width) * td.height * 4, 0);
        RHI::SubresourceData init{ zeros.data(), td.width * 4u, 0u };
        if (!gfx.CreateTexture(td, res.depthAtlas, &init))
        {
            LOG_ERROR("DDGIVolumeManager: depth atlas create failed (%ux%u)",
                      td.width, td.height);
            return false;
        }
        res.depthAtlasSrv = gfx.GetTextureSRVGpuHandle(res.depthAtlas);
        res.depthAtlasUav = gfx.GetTextureUAVGpuHandle(res.depthAtlas);
    }

    // ---- Probe data buffer (RWStructuredBuffer<ProbeData>) ----
    // Value-initialised: every probe starts ACTIVE at offset zero with skyVisibility = 1
    // (the safe outdoor fallback until the first trace updates the EMA); a byte-zero
    // buffer would black out the sky fallback during warm-up.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = uint64_t(totalProbes) * sizeof(ProbeData);
        bd.stride     = sizeof(ProbeData);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        std::vector<ProbeData> initial(totalProbes);
        bd.debugName = "DDGI.ProbeData";
        if (!gfx.CreateBuffer(bd, res.probeDataBuffer, initial.data()))
        {
            LOG_ERROR("DDGIVolumeManager: probe data buffer create failed");
            return false;
        }
        res.probeDataSrv = gfx.GetBufferSRVGpuHandle(res.probeDataBuffer);
        res.probeDataUav = gfx.GetBufferUAVGpuHandle(res.probeDataBuffer);
    }

    // ---- Ray data buffer (RWStructuredBuffer<float4>) ----
    // One float4 per (probe, ray): rgb = traced radiance, a = hit distance.
    {
        const uint64_t rayCount = uint64_t(totalProbes) * v.raysPerProbe;
        RHI::GPUBufferDesc bd{};
        bd.size       = rayCount * sizeof(float) * 4;
        bd.stride     = sizeof(float) * 4;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        bd.debugName = "DDGI.RayData";
        if (!gfx.CreateBuffer(bd, res.rayDataBuffer))
        {
            LOG_ERROR("DDGIVolumeManager: ray data buffer create failed");
            return false;
        }
        res.rayDataSrv = gfx.GetBufferSRVGpuHandle(res.rayDataBuffer);
        res.rayDataUav = gfx.GetBufferUAVGpuHandle(res.rayDataBuffer);
    }

    // ---- Ray count buffer (RWStructuredBuffer<uint>) ----
    // Per-probe bucket counts for the trace's ExecuteIndirect path and the relight's ray
    // loop, rewritten every frame by PrepareRayCount: the full budget for a probe that
    // updates this frame, 0 to skip it; only the opt-in adaptive budget (flags bit 5)
    // lowers it by variance.
    {
        // The buffer also carries the compacted active-probe list the three per-probe kernels
        // are ExecuteIndirect'd over; kept here, it costs no descriptor and no root parameter
        // (g_RayCount is bound at u2 in prepare / allocation / finalize / relight / border):
        //   [0 .. P-1]   per-probe bucket count
        //   [P]          active probe count      (reset by PrepareRayCount)
        //   [P+1 .. 2P]  compacted probe indices (written by RayAllocation)
        // Mirror: DDGI_ActiveCountSlot / DDGI_ActiveListSlot in DDGICommon.hlsli.
        const uint32_t maxBuckets = v.raysPerProbe / DDGI_RAY_BUCKET_COUNT;
        const uint64_t rayCountEntries = uint64_t(totalProbes) * 2ull + 1ull;
        RHI::GPUBufferDesc bd{};
        bd.size       = rayCountEntries * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        // Bucket counts start at MAX (full budget on frame one); the counter and
        // list tail start at zero. PrepareRayCount rewrites both regions every
        // frame before anything reads them, so this only has to be sane.
        std::vector<uint32_t> initial(size_t(rayCountEntries), 0u);
        std::fill(initial.begin(), initial.begin() + totalProbes, maxBuckets);
        bd.debugName = "DDGI.RayCount";
        if (!gfx.CreateBuffer(bd, res.rayCountBuffer, initial.data()))
        {
            LOG_ERROR("DDGIVolumeManager: ray count buffer create failed");
            return false;
        }
        res.rayCountUav = gfx.GetBufferUAVGpuHandle(res.rayCountBuffer);
    }

    // ---- Ray allocation buffer ----
    // [0] = total rays (atomic counter during allocation); [1..] = packed ray descriptors
    // (probeIdx | rayIdx << 20). Stays in UAV for the whole frame; the trace CS reads it
    // as a UAV.
    {
        const uint64_t maxRays = uint64_t(totalProbes) * v.raysPerProbe;
        const uint64_t entries = 1 + maxRays;
        RHI::GPUBufferDesc bd{};
        bd.size       = entries * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        bd.debugName = "DDGI.RayAlloc";
        if (!gfx.CreateBuffer(bd, res.rayAllocBuffer))
        {
            LOG_ERROR("DDGIVolumeManager: ray alloc buffer create failed");
            return false;
        }
        res.rayAllocUav = gfx.GetBufferUAVGpuHandle(res.rayAllocBuffer);
    }

    // ---- Dispatch args buffer ----
    // 32 B = two D3D12_DISPATCH_ARGUMENTS, 16 B apart:
    //   byte  0 (uint 0..2) - the trace's ExecuteIndirect
    //   byte 16 (uint 4..6) - Relight.Irr / Relight.Dep / Border, over the compacted
    //                         active-probe list
    // The finalize CS writes both; DDGIPass passes the byte offset to ExecuteIndirect.
    // D3D12 has no bind flag for indirect args: any UAV-capable buffer can transition to
    // INDIRECT_ARGUMENT.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = 32;
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        bd.debugName = "DDGI.DispatchArgs";
        if (!gfx.CreateBuffer(bd, res.dispatchArgsBuffer))
        {
            LOG_ERROR("DDGIVolumeManager: dispatch args buffer create failed");
            return false;
        }
        res.dispatchArgsUav = gfx.GetBufferUAVGpuHandle(res.dispatchArgsBuffer);
    }

    // ---- Variance buffer (RWStructuredBuffer<DDGIVarianceData>) ----
    // 48 bytes per (probe, irradiance texel): the multi-scale mean estimator state (mean /
    // shortMean / variance / vbbr / inconsistency). Must be zero-initialised: DDGIRelight.cs
    // seeds it lazily on the first overwrite pass, and a non-zero start can reach NaN /
    // negative variance.
    {
        constexpr uint32_t kVarianceStride = 48;
        const uint64_t entries = uint64_t(totalProbes) * kIrradianceProbeSize * kIrradianceProbeSize;
        RHI::GPUBufferDesc bd{};
        bd.size       = entries * kVarianceStride;
        bd.stride     = kVarianceStride;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        std::vector<uint8_t> zeros(bd.size, 0);
        bd.debugName = "DDGI.Variance";
        if (!gfx.CreateBuffer(bd, res.varianceBuffer, zeros.data()))
        {
            LOG_ERROR("DDGIVolumeManager: variance buffer create failed");
            return false;
        }
        res.varianceUav = gfx.GetBufferUAVGpuHandle(res.varianceBuffer);
    }

    // ---- Volume CB ring (UPLOAD heap, persistently mapped) ----
    // One per frame in flight: Tick() rewrites randomRotation + frameIndex every frame, and
    // a single CB would race the GPU's in-flight DDGIPass read of an earlier frame.
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        // One 256-byte CBV (the CBV alignment); VolumeGPUDesc is smaller.
        RHI::GPUBufferDesc bd{};
        bd.size       = 256;
        bd.stride     = 0;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
        bd.debugName = "DDGI.VolumeCB";
        if (!gfx.CreateBuffer(bd, res.volumeCB[f]))
        {
            LOG_ERROR("DDGIVolumeManager: volume CB create failed (slot %u)", f);
            return false;
        }
        res.cbvMapped[f]     = gfx.MapBuffer(res.volumeCB[f]);
        res.cbvGpuAddress[f] = 0; // unused: DDGIPass resolves the CB's VA through the backend (GetVolumeCB)
        if (res.cbvMapped[f]) std::memset(res.cbvMapped[f], 0, 256);
    }

    res.probeCountsX = pcx;
    res.probeCountsY = pcy;
    res.probeCountsZ = pcz;
    res.raysPerProbe = v.raysPerProbe;
    res.allocated    = true;
    res.atlasState   = AtlasState::UAV; // the depth atlas was created in UAV
    return true;
}

void DDGIVolumeManager::DestroyVolume(IGraphicsDevice& gfx, VolumeResources& res)
{
    if (!res.allocated) return;
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        if (res.cbvMapped[f])      { gfx.UnmapBuffer(res.volumeCB[f]); res.cbvMapped[f] = nullptr; }
        if (res.volumeCB[f].IsValid()) gfx.DestroyBuffer(res.volumeCB[f]);
    }
    if (res.varianceBuffer.IsValid())      gfx.DestroyBuffer (res.varianceBuffer);
    if (res.dispatchArgsBuffer.IsValid())  gfx.DestroyBuffer (res.dispatchArgsBuffer);
    if (res.rayAllocBuffer.IsValid())      gfx.DestroyBuffer (res.rayAllocBuffer);
    if (res.rayCountBuffer.IsValid())      gfx.DestroyBuffer (res.rayCountBuffer);
    if (res.rayDataBuffer.IsValid())       gfx.DestroyBuffer (res.rayDataBuffer);
    if (res.probeDataBuffer.IsValid())  gfx.DestroyBuffer (res.probeDataBuffer);
    if (res.depthAtlas.IsValid())       gfx.DestroyTexture(res.depthAtlas);
    if (res.irradianceAtlas.IsValid())  gfx.DestroyTexture(res.irradianceAtlas);
    res = {};
}

bool DDGIVolumeManager::AllocateOrUpdate(IGraphicsDevice& gfx,
                                         const DDGIVolumeComponent& volume,
                                         DDGIVolumeRuntimeComponent& runtime)
{
    // Reuse the slot when the component has one and the probe layout is unchanged;
    // otherwise pick a new slot.
    auto sameLayout = [&](const VolumeResources& r) {
        return r.allocated
            && r.probeCountsX == volume.probeCountsX
            && r.probeCountsY == volume.probeCountsY
            && r.probeCountsZ == volume.probeCountsZ
            && r.raysPerProbe == volume.raysPerProbe;
    };

    // Stale-slot guard. FreeUnclaimedSlots() frees by slot index and cannot reach the
    // runtime components, so a volume whose slot was reclaimed (scene churn, a frame with
    // more than kMaxVolumes volumes, a re-created runtime component) still carries the old
    // volumeSlot. The free-slot search below runs only for volumeSlot >= kMaxVolumes, so
    // without this reset the volume would adopt whichever slot was since handed to another:
    // two entities would share it (Tick's gpuDesc[slot] write is last-writer-wins) or
    // DestroyVolume would tear down the owner's atlases.
    if (runtime.volumeSlot < kMaxVolumes && !m_volumes[runtime.volumeSlot].allocated)
        runtime.volumeSlot = 0xFFFFFFFFu;

    if (runtime.volumeSlot < kMaxVolumes && sameLayout(m_volumes[runtime.volumeSlot]))
    {
        // Sync runtime cache for resize-detection on next frame.
        runtime.probeCountX  = volume.probeCountsX;
        runtime.probeCountY  = volume.probeCountsY;
        runtime.probeCountZ  = volume.probeCountsZ;
        runtime.raysPerProbe = volume.raysPerProbe;
        return true;
    }

    // Free old slot if size changed.
    if (runtime.volumeSlot < kMaxVolumes && m_volumes[runtime.volumeSlot].allocated)
        DestroyVolume(gfx, m_volumes[runtime.volumeSlot]);

    // Find first free slot if needed.
    if (runtime.volumeSlot >= kMaxVolumes)
    {
        for (uint32_t i = 0; i < kMaxVolumes; ++i)
            if (!m_volumes[i].allocated) { runtime.volumeSlot = i; break; }
    }
    if (runtime.volumeSlot >= kMaxVolumes)
    {
        LOG_ERROR("DDGIVolumeManager: no free volume slot (max=%u)", kMaxVolumes);
        return false;
    }

    if (!CreateAtlasesForVolume(gfx, volume, m_volumes[runtime.volumeSlot]))
        return false;

    // Publish this slot's SRVs into the multi-volume tables. The shader sees a
    // stable GPU handle into each table; only the descriptor at offset
    // `volumeSlot` changes when this slot's resources rotate.
    WriteSlotDescriptors(gfx, runtime.volumeSlot, m_volumes[runtime.volumeSlot]);

    runtime.probeCountX  = volume.probeCountsX;
    runtime.probeCountY  = volume.probeCountsY;
    runtime.probeCountZ  = volume.probeCountsZ;
    runtime.raysPerProbe = volume.raysPerProbe;
    runtime.frameCounter = 0;
    // Force the next 3 relight passes to overwrite the atlas (no lerp): the only reliable
    // flush of whatever memory the texture was born into (initialData uploads work on most
    // drivers, but not reliably enough to depend on).
    runtime.firstFramesRemaining = 3;

    m_activeCount = 0;
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated) m_activeCount++;

    // Log the full authored config, not just the grid: two loads of the same scene with
    // equal probe count and ray budget can trace at very different cost depending on where
    // the probes are (origin / extent decide whether rays start inside geometry) and which
    // features are on; with all of it logged, the comparison is a diff.
    LOG_INFO("DDGIVolumeManager: volume slot %u allocated (%ux%ux%u probes, %u rays, updiv=%u) "
             "origin=(%.3f, %.3f, %.3f) extent=(%.3f, %.3f, %.3f) "
             "hyst=%.4f jit=%.3f nb=%.3f vb=%.3f reloc=%d classify=%d",
             runtime.volumeSlot, volume.probeCountsX, volume.probeCountsY,
             volume.probeCountsZ, volume.raysPerProbe, volume.probeUpdateDivisor,
             volume.origin.x, volume.origin.y, volume.origin.z,
             volume.extent.x, volume.extent.y, volume.extent.z,
             volume.hysteresis, volume.rotationJitterScale,
             volume.normalBias, volume.viewBias,
             volume.enableRelocation ? 1 : 0, volume.enableClassification ? 1 : 0);
    return true;
}

void DDGIVolumeManager::Free(IGraphicsDevice& gfx, uint32_t slot)
{
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated) return;
    WriteSlotNullDescriptors(gfx, slot);   // unbind before resources die
    DestroyVolume(gfx, m_volumes[slot]);
    m_activeCount = 0;
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated) m_activeCount++;
}

void DDGIVolumeManager::FreeUnclaimedSlots(IGraphicsDevice& gfx,
                                           const uint32_t* claimedSlots,
                                           uint32_t        claimedCount)
{
    bool claimed[kMaxVolumes] = {};
    for (uint32_t i = 0; i < claimedCount; ++i)
    {
        uint32_t s = claimedSlots[i];
        if (s < kMaxVolumes) claimed[s] = true;
    }
    for (uint32_t s = 0; s < kMaxVolumes; ++s)
    {
        if (m_volumes[s].allocated && !claimed[s])
        {
            LOG_INFO("DDGIVolumeManager: freeing orphaned slot %u", s);
            WriteSlotNullDescriptors(gfx, s);
            DestroyVolume(gfx, m_volumes[s]);
        }
    }
    m_activeCount = 0;
    for (uint32_t i = 0; i < kMaxVolumes; ++i)
        if (m_volumes[i].allocated) m_activeCount++;
}

// ==== Per-frame Tick: pack the VolumeGPUDesc array ====

namespace
{
// Per-frame rotation of the probe ray set: van der Corput on (frame, slot), amplitude
// scaled to the Halton point spacing.
// Do not smooth it into a slow drift: a fresh orientation every frame is high-frequency
// jitter the relight EMA filters, while a slow drift evolves at the low frequency the
// EMA passes through, and the whole scene visibly breathes.
// Amplitude: the jitter only fills the gaps between the fixed Halton directions, whose
// angular radius for N directions is ~sqrt(pi / N) (~6.4 deg at 256 rays). A fixed
// 0.3 rad (~17 deg) is ~3x oversized at 256 rays and sweeps rays across radiance edges
// (sky / wall) every frame - noise for no coverage, forcing a high hysteresis.
void GenerateRandomRotation(uint32_t frame, uint32_t slot, uint32_t raysPerProbe,
                            float jitterScale, float out[9])
{
    auto vdc = [](uint32_t i, uint32_t base) {
        float r = 0.0f, f = 1.0f / float(base);
        while (i > 0) { r += f * float(i % base); i /= base; f /= float(base); }
        return r;
    };
    const uint32_t k  = frame * 73u + slot * 19u + 1u;
    // kAmp ~ the Halton gap radius for raysPerProbe directions, clamped so a degenerate
    // ray count cannot make it 0 or huge.
    float kAmp = std::sqrt(3.14159265f / float(raysPerProbe < 1u ? 1u : raysPerProbe));
    if (kAmp < 0.05f) kAmp = 0.05f;
    if (kAmp > 0.35f) kAmp = 0.35f;
    // Per-volume stability knob (DDGIVolumeComponent::rotationJitterScale): scales the
    // amplitude down for steadier sun lighting (hard shadows flicker as the jitter sweeps
    // samples across lit / shadow edges). Applied after the clamp so it can reach 0 (a
    // static ray set).
    float js = jitterScale; if (js < 0.0f) js = 0.0f; else if (js > 1.0f) js = 1.0f;
    kAmp *= js;
    const float    ax = vdc(k, 2) * kAmp;
    const float    ay = vdc(k, 3) * kAmp;
    const float    az = vdc(k, 5) * kAmp;

    const float cx = std::cos(ax), sx = std::sin(ax);
    const float cy = std::cos(ay), sy = std::sin(ay);
    const float cz = std::cos(az), sz = std::sin(az);

    // R = Rz * Ry * Rx (intrinsic XYZ, composed left to right).
    const float r00 =  cy * cz;
    const float r01 =  sx * sy * cz - cx * sz;
    const float r02 =  cx * sy * cz + sx * sz;
    const float r10 =  cy * sz;
    const float r11 =  sx * sy * sz + cx * cz;
    const float r12 =  cx * sy * sz - sx * cz;
    const float r20 = -sy;
    const float r21 =  sx * cy;
    const float r22 =  cx * cy;
    out[0]=r00; out[1]=r01; out[2]=r02;
    out[3]=r10; out[4]=r11; out[5]=r12;
    out[6]=r20; out[7]=r21; out[8]=r22;
}
} // namespace

void DDGIVolumeManager::Tick(IGraphicsDevice& gfx,
                             const DDGIVolumeComponent* const* volumes,
                             DDGIVolumeRuntimeComponent* const* runtimes,
                             uint32_t volumeCount,
                             uint32_t lightCount,
                             const XMFLOAT3& sunDirection,
                             const XMFLOAT3& sunColor,
                             uint32_t worldLightGridIndex,
                             float skyScale)
{
    const uint32_t frameSlot = gfx.GetUploadSlot();
    if (frameSlot >= kFrameCount || !m_volumeBufferMapped[frameSlot]) return;
    auto* gpuDesc = static_cast<VolumeGPUDesc*>(m_volumeBufferMapped[frameSlot]);
    std::memset(gpuDesc, 0, kMaxVolumes * sizeof(VolumeGPUDesc));

#if DDGI_GT_DIAGNOSTICS
    // Drain a completed ground-truth readback (armed below, recorded by
    // DDGIPass at the end of its Execute). One call per frame.
    ProcessDebugReadback();
#endif
    ProcessProbeWatch(gfx);   // runtime probe watch (Engine.DDGIWatchProbe); no-op unless set

    // ---- Descriptor-block base heap indices ----
    // Lighting.ps opens ResourceDescriptorHeap[NonUniformResourceIndex(base + volumeSlot)].
    // The blocks never move, but the index is recomputed every frame: a heap index is never
    // cached.
    // A failed block allocation gives a 0 handle, for which DescriptorHeapIndexFromGpuHandle
    // answers ~0u; ResourceDescriptorHeap[] has no bounds check and ~0u can remove the
    // device, so clamp to 0 (a real, wrong-contents descriptor), as Renderer's resolve()
    // lambda does for the LightCB indices. Unreachable in practice: Tick returns early
    // without Init, and Init fails if any block cannot allocate.
    auto blockBase = [&gfx](uint64_t tableGpu) -> uint32_t
    {
        const uint32_t idx = gfx.DescriptorHeapIndexFromGpuHandle(tableGpu);
        return (idx == ~0u) ? 0u : idx;
    };
    const uint32_t irradianceBase   = blockBase(m_irradianceTableGpu);
    const uint32_t depthBase     = blockBase(m_depthTableGpu);
    const uint32_t probeDataBase = blockBase(m_probeDataTableGpu);

    // Slot-indexed packing: each entry sits at gpuDesc[r.volumeSlot], not gpuDesc[written],
    // so the shader's index space matches the slot-indexed descriptor blocks. Unused slots
    // stay zeroed (flags & 1u == 0); the shader walks 0..DDGI_MAX_VOLUMES and gates each on
    // flags & 1u.
    uint32_t written = 0;
    for (uint32_t i = 0; i < volumeCount; ++i)
    {
        if (!volumes[i] || !runtimes[i]) continue;
        const DDGIVolumeComponent& v = *volumes[i];
        DDGIVolumeRuntimeComponent& r = *runtimes[i];
        if (r.volumeSlot >= kMaxVolumes || !m_volumes[r.volumeSlot].allocated) continue;

        VolumeResources& res = m_volumes[r.volumeSlot];

        // Advance the frame counter only when this slot's dispatches ran (MarkVolumeExecuted,
        // called after DDGIPass::Execute): an unconditional increment desynchronises the
        // interleave phase and freezes a subset of probes (see MarkVolumeExecuted in the
        // header). Then roll a fresh rotation sized to the ray count.
        const bool ranLastFrame = res.executedSinceLastTick;
        res.executedSinceLastTick = false;
        if (ranLastFrame)
            r.frameCounter++;
        GenerateRandomRotation(r.frameCounter, r.volumeSlot, v.raysPerProbe,
                               v.rotationJitterScale, r.randomRotation);

        // Build on the stack, not in the mapped buffer: gpuDesc is a write-combined UPLOAD
        // mapping, where the field stores would be scattered partial-line WC writes and the
        // probeSpacing min() / CB memcpy would read back from WC memory. On the stack it stays
        // in L1, and the two memcpys at the end are sequential full-line WC stores.
        VolumeGPUDesc d{};
        d.worldLightGridIndex = worldLightGridIndex;
        d.origin       = v.origin;
        // Clamped: the sampler lerps between the linear and perceptual means,
        // so anything outside [0,1] EXTRAPOLATES past both valid endpoints.
        d.perceptualBlendWeight = v.perceptualBlendWeight < 0.0f ? 0.0f
                                : (v.perceptualBlendWeight > 1.0f ? 1.0f
                                                                  : v.perceptualBlendWeight);
        d.extent       = v.extent;
        const XMFLOAT3 cellCount{ float(v.probeCountsX > 1 ? v.probeCountsX - 1 : 1),
                                  float(v.probeCountsY > 1 ? v.probeCountsY - 1 : 1),
                                  float(v.probeCountsZ > 1 ? v.probeCountsZ - 1 : 1) };
        d.probeSpacing = { (2.0f * v.extent.x) / cellCount.x,
                           (2.0f * v.extent.y) / cellCount.y,
                           (2.0f * v.extent.z) / cellCount.z };
        d.probeCountsX = v.probeCountsX;
        d.probeCountsY = v.probeCountsY;
        d.probeCountsZ = v.probeCountsZ;
        d.raysPerProbe = v.raysPerProbe;
        d.hysteresis   = v.hysteresis;
        // Clamped: negative would SLOW the blend below the authored rate,
        // which is not what the knob means.
        d.changeResponseBoost = (v.changeResponseBoost > 0.0f)
                              ? v.changeResponseBoost : 0.0f;
        // Spacing-relative bias folded in on the CPU (no GPU field, no shader change). See
        // DDGIVolumeComponent::biasSpacingScale: a fixed absolute bias stops working as spacing
        // grows (0.25 units is 1/14 of a 3.5 m cell, too small for the Chebyshev
        // self-occlusion guard).
        {
            const float minSpacing = (std::min)(d.probeSpacing.x,
                                     (std::min)(d.probeSpacing.y, d.probeSpacing.z));
            const float extra = (std::max)(0.0f, v.biasSpacingScale) * minSpacing;
            d.normalBias = v.normalBias + extra;
            d.viewBias   = v.viewBias   + extra;
        }
        // Clamped: the deserializer takes the serialized value verbatim, and
        // DDGI_VolumeFadeWeight divides by it. Zero turns the AABB fade into a
        // hard step (visible seam at the volume boundary) and anything above
        // 0.5 makes the fade band swallow the whole volume, so the centre never
        // reaches full weight.
        d.boundaryFadeRatio = v.boundaryFadeRatio < 0.01f ? 0.01f
                            : (v.boundaryFadeRatio > 0.5f ? 0.5f
                                                          : v.boundaryFadeRatio);

        uint32_t flags = 0;
        flags |= 1u; // bit 0: enabled
        if (v.enableRelocation)     flags |= 2u;  // bit 1
        if (v.enableClassification) flags |= 4u;  // bit 2
        // bit 5: adaptive per-probe ray budget (opt-in; the component's field comment says why
        // it defaults off and what to watch for).
        if (v.enableAdaptiveRayBudget) flags |= 32u;
        if (r.firstFramesRemaining > 0)
        {
            flags |= 8u;                          // bit 3: overwrite atlas
            // Same reasoning as frameCounter: the burn-in budget is measured in
            // EXECUTED frames. Decrementing it on a frame the volume was
            // skipped would spend the overwrite window without ever writing an
            // atlas, so the volume would come back with hysteresis already
            // engaged over uninitialised state.
            if (ranLastFrame)
                r.firstFramesRemaining--;
        }
        d.flags = flags;

        // Block bases are identical in every entry (they name the block; the shader adds its
        // slot). Written per entry because the memset above zeroes the array and only claimed
        // slots are filled.
        d.irradianceBaseIdx   = irradianceBase;
        d.depthBaseIdx     = depthBase;
        d.probeDataBaseIdx = probeDataBase;

        d.randomRotation0 = { r.randomRotation[0], r.randomRotation[1], r.randomRotation[2], 0.0f };
        d.randomRotation1 = { r.randomRotation[3], r.randomRotation[4], r.randomRotation[5], 0.0f };
        d.randomRotation2 = { r.randomRotation[6], r.randomRotation[7], r.randomRotation[8], 0.0f };

        d.diffuseTint  = v.diffuseTint;
        d.diffuseScale = v.diffuseScale;
        d.lightCount   = lightCount;
        d.frameIndex   = r.frameCounter;
        // Temporal amortization divisor, clamped so shaders can assume >= 1 without a
        // per-thread max(), and to 8: beyond that a probe updates below ~7 Hz and lighting
        // changes visibly stagger across the interleave phases.
        // The source is the runtime component's effectiveDivisor (the camera-relevance policy
        // in Renderer::BuildScene_UpdateDDGI, which already floors it at the authored value and
        // applies the hysteresis cap); the authored value is the fallback before the policy
        // first runs.
        const uint32_t wantDiv = (r.effectiveDivisor > 0u) ? r.effectiveDivisor
                                                           : v.probeUpdateDivisor;
        d.probeUpdateDivisor = wantDiv < 1u  ? 1u
                             : (wantDiv > 8u ? 8u : wantDiv);
        // Blend-compensation exponent for the divisor, clamped to [0,1] so the shader's pow()
        // stays in the documented response <-> noise trade.
        d.updateCompensation = v.updateCompensation < 0.0f ? 0.0f
                             : (v.updateCompensation > 1.0f ? 1.0f
                                                            : v.updateCompensation);

        // Mirror the policy's skip decision into per-slot state: Renderer's
        // dispatch loop iterates SLOTS and has no component pointers there.
        res.executeThisFrame = r.executeThisFrame;

        // Shading-side arbitration inputs (see VolumeGPUDesc::schedulingPacked).
        // priority is a signed int on the component; negatives would wrap the
        // 16-bit field, so clamp rather than cast.
        {
            const int      pClamped = v.priority < 0 ? 0
                                    : (v.priority > 0xFFFF ? 0xFFFF : v.priority);
            const uint32_t casc     = v.cascadeLevel > 0xFFFFu ? 0xFFFFu
                                                               : v.cascadeLevel;
            d.schedulingPacked = (uint32_t(pClamped) & 0xFFFFu) | (casc << 16);
        }
        d.sunDirection = sunDirection;
        d.sunColor     = sunColor;
        d.skyScale     = (std::isfinite(skyScale) && skyScale > 0.0f) ? skyScale : 0.0f;

        // Throttled per-volume state log. `lightCount` gates the trace's Monte-Carlo light
        // pick + shadow ray (DDGIRayTrace.cs): at 0 every ray skips a second traversal, which
        // can change DDGI.Trace cost several-fold on an identical scene - the first thing to
        // check when two loads of a scene trace at very different cost.
        {
            static uint32_t s_volLogTick = 0;
            if ((++s_volLogTick % 600u) == 0)
                LOG_DIAG(ENGINE_LOG_DDGI_VOLUME,
                         "DDGIVolume[%u]: rays=%u updiv=%u lightCount=%u frameIdx=%u flags=0x%X "
                         "probes=%ux%ux%u sun=(%.3f,%.3f,%.3f) sunCol=(%.2f,%.2f,%.2f)",
                         r.volumeSlot, d.raysPerProbe, d.probeUpdateDivisor,
                         d.lightCount, d.frameIndex, d.flags,
                         d.probeCountsX, d.probeCountsY, d.probeCountsZ,
                         d.sunDirection.x, d.sunDirection.y, d.sunDirection.z,
                         d.sunColor.x, d.sunColor.y, d.sunColor.z);
        }

#if DDGI_GT_DIAGNOSTICS
        // Arm the ground-truth readback for this volume, by frameCounter: a (re)created
        // volume's first capture comes at frame 150 (~2.5 s, past burn-in / overwrite, still
        // the post-load steady state), then every 600 frames (~10 s). Offset from the telemetry
        // tick so the two lines interleave in the log.
        if ((r.frameCounter % 600u) == 150u && m_dbgInFlightSlot == 0xFFFFFFFFu)
            m_dbgArmedSlot = r.volumeSlot;
#endif

        // The per-volume CB carries the same data (the DDGI passes bind only the CB, not the
        // engine-wide buffer, to keep their root signatures simple); write the current frame's
        // ring slot.
        if (res.cbvMapped[frameSlot])
            std::memcpy(res.cbvMapped[frameSlot], &d, sizeof(VolumeGPUDesc));

        // Publish the finished descriptor into the engine-wide buffer, indexed by volumeSlot
        // (not `written`, see above).
        std::memcpy(&gpuDesc[r.volumeSlot], &d, sizeof(VolumeGPUDesc));

        written++;
    }
    (void)gfx;
}

// ==== Probe watch: one probe's ProbeData, read back once per kProbeWatchPeriodFrames ====
// See the threading note by the members in the header: the Lua entry points only file a
// request; Tick (game thread, render parked) validates and latches it; the render
// thread reads the latched fields and arms the copy; the next parked Tick drains it. A
// generation stamp drops a capture of a previous or cleared watch, so re-targeting
// never reports the old probe's bytes as the new probe's first capture.

void DDGIVolumeManager::SetProbeWatch(int32_t x, int32_t y, int32_t z, uint32_t volumeSlot)
{
    m_watchReqCoord = { x, y, z };
    m_watchReqSlot  = volumeSlot < kMaxVolumes ? volumeSlot : 0u;
    m_watchReqDirty = true;
}

void DDGIVolumeManager::ClearProbeWatch()
{
    m_watchReqCoord = { -1, -1, -1 };
    m_watchReqDirty = true;
}

void DDGIVolumeManager::ProcessProbeWatch(IGraphicsDevice& gfx)
{
    // ---- 1. Drain a landed capture (armed by the render thread last frame) ----
    if (m_watchInFlight && m_watchReadback)
    {
        if (m_watchFence > 0)
        {
            m_watchFence--;
        }
        else if (m_watchInFlightGen != m_watchGen)
        {
            // Belongs to a watch re-targeted or cleared while pending; the copy has completed
            // (fence), so the buffer may be re-armed. Nothing is published.
            m_watchInFlight = false;
        }
        else
        {
            void* mapped = nullptr;
            const D3D12_RANGE readRange{ 0, sizeof(ProbeData) };
            if (SUCCEEDED(m_watchReadback->Map(0, &readRange, &mapped)) && mapped)
            {
                ProbeData pd;
                std::memcpy(&pd, mapped, sizeof(ProbeData));
                const D3D12_RANGE noWrite{ 0, 0 };
                m_watchReadback->Unmap(0, &noWrite);

                m_watchLast.valid         = true;
                m_watchLast.capture       = ++m_watchCaptures;
                m_watchLast.offset        = pd.offset;
                m_watchLast.stateRaw      = pd.state;
                m_watchLast.skyVisibility = pd.skyVisibility;
                m_watchLast.skyVisDir     = pd.skyVisDir;

                // Every 6th capture = ~6 * kProbeWatchPeriodFrames = 36+ frames.
                if ((m_watchCaptures % 6u) == 1u)
                {
                    const uint32_t enc = (pd.state >> 8) & 0xFFu;
                    const float    bf  = enc ? float(enc - 1u) / 254.0f : -1.0f;
                    LOG_INFO("DDGI.WATCH[%u] probe (%d,%d,%d) #%u: off=(%.4f,%.4f,%.4f) |off|=%.4f "
                             "state=%u fresh=%u bf=%.3f sky=%.3f skyDir=(%.3f,%.3f,%.3f)",
                             m_watchSlot, m_watchLast.x, m_watchLast.y, m_watchLast.z, m_watchCaptures,
                             pd.offset.x, pd.offset.y, pd.offset.z,
                             std::sqrt(pd.offset.x * pd.offset.x + pd.offset.y * pd.offset.y
                                     + pd.offset.z * pd.offset.z),
                             pd.state & 0x3u, (pd.state >> 2) & 1u, bf, pd.skyVisibility,
                             pd.skyVisDir.x, pd.skyVisDir.y, pd.skyVisDir.z);
                }
            }
            m_watchInFlight = false;
        }
    }

    // ---- 2. Latch a pending request (validated here, on the game thread) ----
    if (!m_watchReqDirty) return;
    if (m_watchReqCoord.x < 0)
    {
        m_watchCoord    = { -1, -1, -1 };
        ++m_watchGen;                       // any pending capture is dropped on landing
        m_watchLast     = {};
        m_watchCaptures = 0;
        m_watchReqDirty = false;
        LOG_INFO("DDGI.WATCH: watch cleared");
        return;
    }
    const uint32_t slot = m_watchReqSlot;
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated)
        return;                             // volume not up yet: retry next Tick
    const VolumeResources& res = m_volumes[slot];
    const DirectX::XMINT3  c   = m_watchReqCoord;
    if (c.y < 0 || c.z < 0
        || c.x >= int32_t(res.probeCountsX) || c.y >= int32_t(res.probeCountsY)
        || c.z >= int32_t(res.probeCountsZ))
    {
        LOG_WARNING("DDGI.WATCH: probe (%d,%d,%d) is outside volume slot %u (%ux%ux%u) — request dropped",
                    c.x, c.y, c.z, slot, res.probeCountsX, res.probeCountsY, res.probeCountsZ);
        m_watchReqDirty = false;
        return;
    }
    if (!m_watchReadback)
    {
        auto& dx12 = static_cast<GraphicsDX12&>(gfx);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 256;   // one ProbeData (48 B), padded to the buffer minimum
        static_assert(sizeof(ProbeData) <= 256, "the probe watch readback holds one ProbeData");
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dx12.GetDevice()->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&m_watchReadback))))
        {
            LOG_WARNING("DDGI.WATCH: readback buffer create failed — request dropped");
            m_watchReqDirty = false;
            return;
        }
    }
    m_watchCoord    = c;
    m_watchSlot     = slot;
    m_watchIdx      = uint32_t(c.x) + uint32_t(c.y) * res.probeCountsX
                    + uint32_t(c.z) * res.probeCountsX * res.probeCountsY;
    ++m_watchGen;
    m_watchLast     = {};
    m_watchLast.x = c.x; m_watchLast.y = c.y; m_watchLast.z = c.z;
    m_watchLast.volumeSlot = slot;
    m_watchCaptures = 0;
    m_watchReqDirty = false;
    LOG_INFO("DDGI.WATCH: watching probe (%d,%d,%d) of volume slot %u", c.x, c.y, c.z, slot);
}

void DDGIVolumeManager::RecordProbeWatch(IGraphicsDevice& gfx, RHI::CommandList cmdList,
                                         uint32_t slot, bool onComputeQueue)
{
    // Render thread. Reads only the LATCHED fields, writes only the in-flight
    // trio; never touches m_watchLast / the request.
    if (m_watchCoord.x < 0 || slot != m_watchSlot || m_watchInFlight || !m_watchReadback) return;
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated) return;

    // Take the native list only when a watch records: taking it marks the caller's GPU
    // region native.
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12GraphicsCommandList* cmd = dx12.GetNativeCommandList(cmdList);
    ID3D12Resource* probeRes = dx12.GetBufferResource(m_volumes[slot].probeDataBuffer);
    if (!cmd || !probeRes) return;

    // Same end-of-Execute state contract as RecordDebugReadback: probeData is in its
    // shader-read state (NPSR on the compute queue, NPSR|PSR on graphics - DDGIPass's
    // kSHReadState).
    const D3D12_RESOURCE_STATES probeRead = onComputeQueue
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
         | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER t{};
    t.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource   = probeRes;
    t.Transition.StateBefore = probeRead;
    t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &t);
    GfxDiag::NoteNativeBarriers(cmd, &t, 1);
    cmd->CopyBufferRegion(m_watchReadback.Get(), 0, probeRes,
                          uint64_t(m_watchIdx) * sizeof(ProbeData), sizeof(ProbeData));
    std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    cmd->ResourceBarrier(1, &t);
    GfxDiag::NoteNativeBarriers(cmd, &t, 1);

    m_watchInFlightGen = m_watchGen;
    m_watchFence       = kFrameCount + 2;   // same completion guarantee as the GT readback
    m_watchInFlight    = true;
}

// ==== Diagnostic ground-truth readback ====
// Measures on the GPU how many rays the trace dispatched, what they hit (miss /
// backface / frontface, at what distance) and where the probes are (offset / state /
// sky). These differ only if the traversed scene or the ray origins differ, so one log
// line separates the candidate causes of a trace-cost change.

void DDGIVolumeManager::RecordDebugReadback(IGraphicsDevice& gfx,
                                            RHI::CommandList cmdList,
                                            uint32_t slot,
                                            bool onComputeQueue)
{
#if !DDGI_GT_DIAGNOSTICS
    (void)gfx; (void)cmdList; (void)slot; (void)onComputeQueue;
    return;
#else
    if (m_dbgArmedSlot != slot) return;
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated) { m_dbgArmedSlot = 0xFFFFFFFFu; return; }
    m_dbgArmedSlot = 0xFFFFFFFFu;

    // Take the native list only when the readback is armed.
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12GraphicsCommandList* cmd = dx12.GetNativeCommandList(cmdList);
    if (!cmd) return;
    VolumeResources& res = m_volumes[slot];
    ID3D12Resource* allocRes = dx12.GetBufferResource(res.rayAllocBuffer);
    ID3D12Resource* rayRes   = dx12.GetBufferResource(res.rayDataBuffer);
    ID3D12Resource* probeRes = dx12.GetBufferResource(res.probeDataBuffer);
    if (!allocRes || !rayRes || !probeRes) return;

    if (!m_dbgReadback)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = kDbgReadbackBytes;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dx12.GetDevice()->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&m_dbgReadback))))
        {
            LOG_WARNING("DDGI.GT: readback buffer create failed — diagnostics off");
            return;
        }
    }

    const uint32_t totalProbes = res.probeCountsX * res.probeCountsY * res.probeCountsZ;
    const uint32_t rays        = res.raysPerProbe;
    if (totalProbes == 0 || rays == 0) return;

    // End-of-Execute states: rayAlloc + rayData are in UAV (implicit promotion; DDGIPass
    // never transitions them); probeData is in its shader-read state (NPSR on the compute
    // queue, NPSR|PSR on graphics - DDGIPass's kSHReadState).
    const D3D12_RESOURCE_STATES probeRead = onComputeQueue
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
         | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto transition = [](ID3D12Resource* r, D3D12_RESOURCE_STATES b, D3D12_RESOURCE_STATES a)
    {
        D3D12_RESOURCE_BARRIER t{};
        t.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource   = r;
        t.Transition.StateBefore = b;
        t.Transition.StateAfter  = a;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return t;
    };
    D3D12_RESOURCE_BARRIER toCopy[3] = {
        transition(allocRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(rayRes,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(probeRes, probeRead,                             D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cmd->ResourceBarrier(3, toCopy);
    GfxDiag::NoteNativeBarriers(cmd, toCopy, 3);

    // [0..16): ray-alloc counters ([0] = total rays this frame).
    cmd->CopyBufferRegion(m_dbgReadback.Get(), 0, allocRes, 0, 16);

    // Ray + probe samples: 4 chunks anchored on four different y layers at mid-z, x =
    // 0..31. The probe index is x + y*countsX + z*countsX*countsY (DDGI_ProbeCoord), so
    // quartile-of-index anchors would all land on y = 0. Layer picks y = countsY*(2c+1)/8
    // -> {1,5,9,13} for countsY = 16, ground level to above the rooflines. Chunk c's rays
    // start at its anchor probe's first ray (1024 rays = 8 probes @128 rays, 4 @256).
    uint64_t dstOff = 16;
    for (uint32_t c = 0; c < kDbgRayChunks; ++c)
    {
        const uint32_t yc = std::min(res.probeCountsY - 1,
                                     (res.probeCountsY * (2 * c + 1)) / (2 * kDbgRayChunks));
        const uint32_t pc = yc * res.probeCountsX
                          + (res.probeCountsZ / 2) * res.probeCountsX * res.probeCountsY;
        m_dbgChunkAnchor[c] = pc;
        m_dbgChunkLayerY[c] = yc;
        const uint64_t ray0  = uint64_t(pc) * rays;
        const uint64_t avail = uint64_t(totalProbes) * rays - ray0;
        const uint32_t n     = uint32_t(std::min<uint64_t>(kDbgRaysPerChunk, avail));
        m_dbgRayChunkCount[c] = n;
        if (n)
            cmd->CopyBufferRegion(m_dbgReadback.Get(), dstOff, rayRes, ray0 * 16, uint64_t(n) * 16);
        dstOff += uint64_t(kDbgRaysPerChunk) * 16;
    }
    for (uint32_t c = 0; c < kDbgProbeChunks; ++c)
    {
        const uint32_t pc = m_dbgChunkAnchor[c]; // same y-layer anchors as the ray chunks
        const uint32_t n  = std::min(kDbgProbesPerChunk, totalProbes - pc);
        m_dbgProbeChunkCount[c] = n;
        if (n)
            cmd->CopyBufferRegion(m_dbgReadback.Get(), dstOff, probeRes,
                                  uint64_t(pc) * sizeof(ProbeData), uint64_t(n) * sizeof(ProbeData));
        dstOff += uint64_t(kDbgProbesPerChunk) * sizeof(ProbeData);
    }

    D3D12_RESOURCE_BARRIER back[3] = {
        transition(allocRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(rayRes,   D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(probeRes, D3D12_RESOURCE_STATE_COPY_SOURCE, probeRead),
    };
    cmd->ResourceBarrier(3, back);
    GfxDiag::NoteNativeBarriers(cmd, back, 3);

    m_dbgInFlightSlot  = slot;
    m_dbgFence         = kFrameCount + 2; // swap-chain slot fence guarantees completion by then
    m_dbgTotalProbes   = totalProbes;
    m_dbgRaysPerProbe  = rays;
#endif // DDGI_GT_DIAGNOSTICS
}

void DDGIVolumeManager::ProcessDebugReadback()
{
#if DDGI_GT_DIAGNOSTICS
    if (m_dbgInFlightSlot == 0xFFFFFFFFu || !m_dbgReadback) return;
    if (m_dbgFence > 0) { m_dbgFence--; return; }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, kDbgReadbackBytes };
    if (FAILED(m_dbgReadback->Map(0, &readRange, &mapped)) || !mapped)
    {
        m_dbgInFlightSlot = 0xFFFFFFFFu;
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped);

    uint32_t allocHdr[4];
    std::memcpy(allocHdr, base, 16);

    // Ray classification, mirroring DDGIRayTrace.cs's sentinels:
    //   w == -1.0 : miss;  w < -1.5 : backface (t = -w - 2);  w > 0 : frontface.
    // Kept per chunk (one y layer each), so the log shows the vertical structure
    // (street-level burial vs open-air upper layers) instead of one aggregate.
    uint32_t missC[kDbgRayChunks] = {}, backC[kDbgRayChunks] = {},
             frontC[kDbgRayChunks] = {}, nC[kDbgRayChunks] = {};
    double   tFrontC[kDbgRayChunks] = {}, tBackC[kDbgRayChunks] = {};
    // Radiance luminance split by hit class. Front-hit luminance is the
    // direct-shading discriminator: a sunlit hit carries ~sunCol*albedo/PI
    // (O(1)); a hit whose sun-eval/shadow-ray path was skipped carries only
    // multibounce+emissive (O(0.1)). Miss luminance sanity-checks the sky IBL.
    double   lumFrontC[kDbgRayChunks] = {}, lumMissC[kDbgRayChunks] = {};
    uint32_t totalRaysSampled = 0, zero = 0;
    for (uint32_t c = 0; c < kDbgRayChunks; ++c)
    {
        const float* rays = reinterpret_cast<const float*>(base + 16 + uint64_t(c) * kDbgRaysPerChunk * 16);
        for (uint32_t i = 0; i < m_dbgRayChunkCount[c]; ++i)
        {
            const float r = rays[i * 4 + 0], g = rays[i * 4 + 1], b = rays[i * 4 + 2];
            const float w = rays[i * 4 + 3];
            const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            nC[c]++; totalRaysSampled++;
            if      (w == -1.0f) { missC[c]++;  lumMissC[c] += lum; }
            else if (w <  -1.5f) { backC[c]++;  tBackC[c]  += double(-w) - 2.0; }
            else if (w >   0.0f) { frontC[c]++; tFrontC[c] += double(w); lumFrontC[c] += lum; }
            else                 zero++;
        }
    }

    // Probe stats, also per layer chunk.
    uint32_t inactC[kDbgProbeChunks] = {}, movedC[kDbgProbeChunks] = {}, pnC[kDbgProbeChunks] = {};
    double   offC[kDbgProbeChunks] = {}, skyC[kDbgProbeChunks] = {};
    double   offMax = 0.0;
    const uint8_t* probeBase = base + 16 + kDbgRayBytes;
    for (uint32_t c = 0; c < kDbgProbeChunks; ++c)
    {
        const uint8_t* p = probeBase + uint64_t(c) * kDbgProbesPerChunk * sizeof(ProbeData);
        for (uint32_t i = 0; i < m_dbgProbeChunkCount[c]; ++i)
        {
            ProbeData pd;
            std::memcpy(&pd, p + uint64_t(i) * sizeof(ProbeData), sizeof(ProbeData));
            pnC[c]++;
            const double off = std::sqrt(double(pd.offset.x) * pd.offset.x
                                       + double(pd.offset.y) * pd.offset.y
                                       + double(pd.offset.z) * pd.offset.z);
            offC[c] += off;
            offMax   = std::max(offMax, off);
            if (off > 0.01) movedC[c]++;
            if (pd.state != 0) inactC[c]++;
            skyC[c] += double(pd.skyVisibility);
        }
    }

    const D3D12_RANGE noWrite{ 0, 0 };
    m_dbgReadback->Unmap(0, &noWrite);

    const uint32_t expectRays = m_dbgTotalProbes * m_dbgRaysPerProbe;
    LOG_INFO("DDGI.GT[%u]: totalRays=%u (expect %u) zero=%u | per-layer rays m/b/f (tF,tB) + probes inact/moved/|off|/sky:",
             m_dbgInFlightSlot, allocHdr[0], expectRays, zero);
    for (uint32_t c = 0; c < kDbgRayChunks; ++c)
    {
        LOG_INFO("DDGI.GT[%u]  y=%2u: rays n=%4u m=%4u b=%4u f=%4u (tF=%.2f tB=%.2f lumF=%.3f lumM=%.3f) | "
                 "probes n=%2u inact=%2u moved=%2u |off|=%.3f sky=%.2f",
                 m_dbgInFlightSlot, m_dbgChunkLayerY[c],
                 nC[c], missC[c], backC[c], frontC[c],
                 frontC[c] ? tFrontC[c] / frontC[c] : 0.0,
                 backC[c]  ? tBackC[c]  / backC[c]  : 0.0,
                 frontC[c] ? lumFrontC[c] / frontC[c] : 0.0,
                 missC[c]  ? lumMissC[c]  / missC[c]  : 0.0,
                 pnC[c], inactC[c], movedC[c],
                 pnC[c] ? offC[c] / pnC[c] : 0.0,
                 pnC[c] ? skyC[c] / pnC[c] : 0.0);
    }
    (void)offMax;

    m_dbgInFlightSlot = 0xFFFFFFFFu;
#endif // DDGI_GT_DIAGNOSTICS
}

// ==== Accessors ====

uint64_t DDGIVolumeManager::GetIrradianceAtlasSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].irradianceAtlasSrv : 0; }
uint64_t DDGIVolumeManager::GetDepthAtlasSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].depthAtlasSrv : 0; }
uint64_t DDGIVolumeManager::GetProbeDataSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].probeDataSrv : 0; }
uint64_t DDGIVolumeManager::GetRayDataSrv(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayDataSrv : 0; }
uint64_t DDGIVolumeManager::GetIrradianceAtlasUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].irradianceAtlasUav : 0; }
uint64_t DDGIVolumeManager::GetDepthAtlasUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].depthAtlasUav : 0; }
uint64_t DDGIVolumeManager::GetRayDataUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayDataUav : 0; }
uint64_t DDGIVolumeManager::GetProbeDataUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].probeDataUav : 0; }
uint64_t DDGIVolumeManager::GetVarianceUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].varianceUav : 0; }
uint64_t DDGIVolumeManager::GetRayCountUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayCountUav : 0; }
uint64_t DDGIVolumeManager::GetRayAllocUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].rayAllocUav : 0; }
uint64_t DDGIVolumeManager::GetDispatchArgsUav(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].dispatchArgsUav : 0; }
const RHI::GPUBuffer* DDGIVolumeManager::GetDispatchArgsBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].dispatchArgsBuffer : nullptr; }
const RHI::Texture* DDGIVolumeManager::GetIrradianceAtlasTexture(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].irradianceAtlas : nullptr; }
const RHI::GPUBuffer* DDGIVolumeManager::GetProbeDataBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].probeDataBuffer : nullptr; }
// Raw resources for DDGIPass's per-dispatch UAV barriers (see the header).
const RHI::GPUBuffer* DDGIVolumeManager::GetRayCountBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].rayCountBuffer : nullptr; }
const RHI::GPUBuffer* DDGIVolumeManager::GetRayAllocBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].rayAllocBuffer : nullptr; }
const RHI::GPUBuffer* DDGIVolumeManager::GetRayDataBuffer(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].rayDataBuffer : nullptr; }
const RHI::Texture* DDGIVolumeManager::GetDepthAtlasTexture(uint32_t s) const
{ return (s < kMaxVolumes && m_volumes[s].allocated) ? &m_volumes[s].depthAtlas : nullptr; }

void DDGIVolumeManager::MarkVolumeExecuted(uint32_t s)
{
    if (s < kMaxVolumes && m_volumes[s].allocated)
        m_volumes[s].executedSinceLastTick = true;
}

bool DDGIVolumeManager::ShouldExecuteVolume(uint32_t s) const
{
    return (s < kMaxVolumes && m_volumes[s].allocated)
         ? m_volumes[s].executeThisFrame
         : false;
}
const RHI::GPUBuffer* DDGIVolumeManager::GetVolumeCB(IGraphicsDevice& gfx, uint32_t s) const
{
    if (s >= kMaxVolumes || !m_volumes[s].allocated) return nullptr;
    const uint32_t f = gfx.GetFrameIndex();
    return (f < kFrameCount) ? &m_volumes[s].volumeCB[f] : nullptr;
}

void DDGIVolumeManager::TransitionVolumeAtlases(IGraphicsDevice& gfx,
                                                RHI::CommandList cmd,
                                                uint32_t slot, AtlasState newState,
                                                bool computeQueueState)
{
    if (slot >= kMaxVolumes || !m_volumes[slot].allocated) return;
    VolumeResources& r = m_volumes[slot];

    // The SRV target mask: full SHADER_RESOURCE (PIXEL | NON_PIXEL) is legal only on
    // graphics queues; compute lists get NON_PIXEL_SHADER_RESOURCE, the only SRV state
    // valid on D3D12_COMMAND_LIST_TYPE_COMPUTE.
    const RHI::ResourceState srvTargetState = computeQueueState
        ? RHI::ResourceState::SHADER_RESOURCE_COMPUTE
        : RHI::ResourceState::SHADER_RESOURCE;

    // Skip when the recorded state already matches and the queue restriction agrees:
    // re-issuing it would make StateBefore == StateAfter, which D3D12 flags.
    if (r.atlasState == newState
        && (newState == AtlasState::UAV || r.atlasInComputeOnlyState == computeQueueState))
        return;

    // StateBefore is the recorded state: UAV, or SRV with the NON_PIXEL-only mask while
    // atlasInComputeOnlyState is set, else the full mask.
    const RHI::ResourceState before = (r.atlasState == AtlasState::UAV)
        ? RHI::ResourceState::UNORDERED_ACCESS
        : (r.atlasInComputeOnlyState
            ? RHI::ResourceState::SHADER_RESOURCE_COMPUTE
            : RHI::ResourceState::SHADER_RESOURCE);
    const RHI::ResourceState after = (newState == AtlasState::UAV)
        ? RHI::ResourceState::UNORDERED_ACCESS
        : srvTargetState;

    // Only the depth atlas: the irradiance atlas and probeData follow DDGIPass's own
    // kSHReadState <-> UAV pair.
    RHI::GPUBarrier b = RHI::GPUBarrier::Image(&r.depthAtlas, before, after);
    gfx.PushBarrier(b, cmd);
    r.atlasState = newState;
    r.atlasInComputeOnlyState = (newState == AtlasState::SRV) ? computeQueueState : false;
}

void DDGIVolumeManager::PromoteAtlasesForGraphicsQueue(IGraphicsDevice& gfx,
                                                       RHI::CommandList graphicsCmd)
{
    // Per allocated slot, NON_PIXEL_SHADER_RESOURCE -> SHADER_RESOURCE (PIXEL | NON_PIXEL);
    // the caller has already waited on the cross-queue fence. Three resources: the depth
    // atlas (gated on atlasState), and the irradiance atlas + probeData buffer (gated on
    // buffersInComputeOnlyState; LightingPass samples probeData for the relocation
    // offset). The irradiance atlas stays grouped with probeData, not with the depth atlas:
    // depth follows TransitionVolumeAtlases' atlasState machine, the irradiance atlas
    // DDGIPass's kSHReadState <-> UAV pair. Do not merge the two lifecycles.
    for (uint32_t s = 0; s < kMaxVolumes; ++s)
    {
        VolumeResources& r = m_volumes[s];
        if (!r.allocated) continue;

        if (r.atlasInComputeOnlyState)
        {
            if (r.atlasState == AtlasState::SRV)
            {
                RHI::GPUBarrier b = RHI::GPUBarrier::Image(&r.depthAtlas,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                    RHI::ResourceState::SHADER_RESOURCE);
                gfx.PushBarrier(b, graphicsCmd);
            }
            r.atlasInComputeOnlyState = false;
        }

        if (r.buffersInComputeOnlyState)
        {
            // Irradiance atlas: DDGIPass left it in kSHReadState, NON_PIXEL only on the compute
            // queue; promote to NON_PIXEL | PIXEL for the lighting PS.
            RHI::GPUBarrier shB = RHI::GPUBarrier::Image(&r.irradianceAtlas,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                RHI::ResourceState::SHADER_RESOURCE);
            gfx.PushBarrier(shB, graphicsCmd);

            // ProbeData buffer: same state cycle as the irradiance atlas.
            RHI::GPUBarrier pdB = RHI::GPUBarrier::Buffer(&r.probeDataBuffer,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                RHI::ResourceState::SHADER_RESOURCE);
            gfx.PushBarrier(pdB, graphicsCmd);

            r.buffersInComputeOnlyState = false;
        }
    }
}

void DDGIVolumeManager::DemoteForComputeQueue(IGraphicsDevice& gfx,
                                              RHI::CommandList graphicsCmd)
{
    // Inverse of PromoteAtlasesForGraphicsQueue (see the header). The per-resource
    // compute-only flags make re-entry a no-op; on the first frame after allocation the
    // depth atlas is in UAV, so its barrier is skipped while the irradiance / probeData
    // barriers fire.
    for (uint32_t s = 0; s < kMaxVolumes; ++s)
    {
        VolumeResources& r = m_volumes[s];
        if (!r.allocated) continue;

        // Depth atlas: only demote when currently in SRV state with the full
        // PSR|NPSR mask. UAV state doesn't have PSR, so nothing to do.
        if (!r.atlasInComputeOnlyState && r.atlasState == AtlasState::SRV)
        {
            RHI::GPUBarrier b = RHI::GPUBarrier::Image(&r.depthAtlas,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarrier(b, graphicsCmd);
            r.atlasInComputeOnlyState = true;
        }

        if (!r.buffersInComputeOnlyState)
        {
            RHI::GPUBarrier shB = RHI::GPUBarrier::Image(&r.irradianceAtlas,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarrier(shB, graphicsCmd);

            RHI::GPUBarrier pdB = RHI::GPUBarrier::Buffer(&r.probeDataBuffer,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarrier(pdB, graphicsCmd);

            r.buffersInComputeOnlyState = true;
        }
    }
}

uint32_t DDGIVolumeManager::GetProbeCount(uint32_t s) const
{
    if (s >= kMaxVolumes || !m_volumes[s].allocated) return 0;
    const VolumeResources& r = m_volumes[s];
    return r.probeCountsX * r.probeCountsY * r.probeCountsZ;
}
uint32_t DDGIVolumeManager::GetRaysPerProbe(uint32_t s) const
{ return s < kMaxVolumes ? m_volumes[s].raysPerProbe : 0; }
uint32_t DDGIVolumeManager::GetDepthAtlasWidth(uint32_t s) const
{ return s < kMaxVolumes && m_volumes[s].allocated
       ? DepthAtlasWidth(m_volumes[s].probeCountsX,
                         m_volumes[s].probeCountsY * m_volumes[s].probeCountsZ)
       : 0; }
uint32_t DDGIVolumeManager::GetDepthAtlasHeight(uint32_t s) const
{ return s < kMaxVolumes && m_volumes[s].allocated
       ? DepthAtlasHeight(m_volumes[s].probeCountsX,
                          m_volumes[s].probeCountsY * m_volumes[s].probeCountsZ)
       : 0; }

// ==== Per-frame accessors ====

uint64_t DDGIVolumeManager::GetVolumeBufferSrv(IGraphicsDevice& gfx) const
{
    const uint32_t s = gfx.GetFrameIndex();
    return (s < kFrameCount) ? m_volumeBufferSrv[s] : 0;
}

} // namespace DDGI
