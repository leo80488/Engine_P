#include "Renderer/ReflectionProbeManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ReflectionProbeTypes.h"
#include "System/Log.h"

#include <cstring>

bool ReflectionProbeManager::Init(IGraphicsDevice& gfx)
{
    using namespace Reflection;

    // ---- Cubemap-array ----
    {
        RHI::TextureDesc td{};
        td.width      = kProbeCubemapSize;
        td.height     = kProbeCubemapSize;
        td.arraySize = kMaxReflectionProbes * 6;
        td.mipLevels = kProbeCubemapMips;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE
                      | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        td.debugName = "ReflectionProbeManager.CubemapArray";

        if (!gfx.CreateTexture(td, m_array))
        {
            LOG_ERROR("ReflectionProbeManager: cubemap-array create failed");
        }
        else
        {
            m_arraySrv = gfx.GetTextureSRVGpuHandle(m_array);
            LOG_INFO("ReflectionProbeManager: cubemap-array ready (%u cubes, %u mips, %ux%u)",
                     kMaxReflectionProbes, kProbeCubemapMips,
                     kProbeCubemapSize, kProbeCubemapSize);
        }
    }

    // ---- Per-cube bindless SRV block ----
    // One TextureCubeArray view (NumCubes=1, First2DArrayFace=slice*6) per cube, in
    // the shader-visible heap's static region. cubemapSrvIndex(slice) =
    // m_cubeSrvBaseIndex + slice; sampled via ResourceDescriptorHeap[idx] in-shader.
    // The views take their format (R16G16B16A16_FLOAT) from the array.
    if (m_array.IsValid())
    {
        if (gfx.AllocateDescriptorBlock(kMaxReflectionProbes, m_cubeSrvBlock))
        {
            // (blockGpuHandle - heapStart) / descSize via the shared helper, which
            // DDGIVolumeManager also uses, so the conversion has one definition.
            m_cubeSrvBaseIndex = gfx.DescriptorHeapIndexFromGpuHandle(m_cubeSrvBlock.gpuBase);
            for (uint32_t s = 0; s < kMaxReflectionProbes; ++s)
                gfx.WriteTextureCubeArraySrv(m_cubeSrvBlock, s, m_array, /*firstCube=*/s, /*numCubes=*/1,
                                             kProbeCubemapMips);
            LOG_INFO("ReflectionProbeManager: %u per-cube bindless SRVs @ base heap index %u",
                     kMaxReflectionProbes, m_cubeSrvBaseIndex);
        }
        else
        {
            LOG_ERROR("ReflectionProbeManager: per-cube bindless SRV block alloc failed");
        }
    }

    // ---- StructuredBuffer<GpuShared::ReflectionProbe> ----
    // Ring of kFrameCount UPLOAD buffers - see the header for why.
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        RHI::GPUBufferDesc desc{};
        desc.size       = static_cast<uint64_t>(kMaxReflectionProbes) * sizeof(GpuShared::ReflectionProbe);
        desc.stride     = sizeof(GpuShared::ReflectionProbe);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!gfx.CreateBuffer(desc, m_buffer[i]))
        {
            LOG_ERROR("ReflectionProbeManager: StructuredBuffer create failed (slot %u)", i);
            return false;
        }
        m_bufferMapped[i] = static_cast<GpuShared::ReflectionProbe*>(gfx.MapBuffer(m_buffer[i]));
        m_bufferSrv[i]    = gfx.GetBufferSRVGpuHandle(m_buffer[i]);

        if (m_bufferMapped[i])
            std::memset(m_bufferMapped[i], 0,
                        kMaxReflectionProbes * sizeof(GpuShared::ReflectionProbe));
    }

    if (!m_capturePass.Init(gfx))
    {
        LOG_ERROR("ReflectionProbeManager: capture pass init failed");
        return false;
    }
    return true;
}

void ReflectionProbeManager::Shutdown(IGraphicsDevice& gfx)
{
    // Released at once (quiesced): Shutdown runs in ~Renderer, after the GPU queues
    // drain.
    if (m_cubeSrvBlock.IsValid()) gfx.ReleaseDescriptorBlock(m_cubeSrvBlock, /*quiesced=*/true);
    m_cubeSrvBaseIndex = 0;

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_bufferMapped[i])      { gfx.UnmapBuffer(m_buffer[i]); m_bufferMapped[i] = nullptr; }
        if (m_buffer[i].IsValid())  { gfx.DestroyBuffer(m_buffer[i]); }
        m_bufferSrv[i] = 0;
    }
}

GpuShared::ReflectionProbe* ReflectionProbeManager::GetUploadPointer(IGraphicsDevice& gfx) const
{
    const uint32_t s = gfx.GetUploadSlot();
    if (s >= kFrameCount) return nullptr;
    return m_bufferMapped[s];
}

uint64_t ReflectionProbeManager::GetBufferSrv(IGraphicsDevice& gfx) const
{
    const uint32_t s = gfx.GetFrameIndex();
    if (s >= kFrameCount) return 0;
    return m_bufferSrv[s];
}

uint64_t ReflectionProbeManager::GetUploadBufferSrv(IGraphicsDevice& gfx) const
{
    const uint32_t s = gfx.GetUploadSlot();
    if (s >= kFrameCount) return 0;
    return m_bufferSrv[s];
}

void ReflectionProbeManager::EnqueueBake(uint32_t cubeSlice)
{
    if (cubeSlice >= Reflection::kMaxReflectionProbes) return;
    for (uint32_t s : m_bakeQueue) if (s == cubeSlice) return;
    m_bakeQueue.push_back(cubeSlice);
}

uint32_t ReflectionProbeManager::PeekBake() const
{
    return m_bakeQueue.empty() ? ~0u : m_bakeQueue.front();
}

void ReflectionProbeManager::PopBake()
{
    if (!m_bakeQueue.empty()) m_bakeQueue.erase(m_bakeQueue.begin());
}
