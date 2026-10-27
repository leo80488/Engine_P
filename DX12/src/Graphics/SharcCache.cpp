#include "Graphics/SharcCache.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>
#include <filesystem>

SharcCache::Config SharcCache::FromCommandLine(const Config& defaults)
{
    // Every consumer reads every flag, so none is reported unread on either path.
    const auto& cmd = Sys::CommandLine::Get();
    Config c;
    c.capacityLog2 = uint32_t(std::clamp(cmd.GetInt("-sharc_capacity_log2", int(defaults.capacityLog2)), 16, 23));
    c.sceneScale = std::clamp(cmd.GetFloat("-sharc_scene_scale", defaults.sceneScale), 1.f, 1000.f);
    c.updateTile = uint32_t(std::clamp(cmd.GetInt("-sharc_update_tile", int(defaults.updateTile)), 1, 16));
    c.updateBounces = uint32_t(std::clamp(cmd.GetInt("-sharc_update_bounces", int(defaults.updateBounces)), 1, 8));
    c.accumulationFrames = uint32_t(std::clamp(cmd.GetInt("-sharc_accumulation_frames", int(defaults.accumulationFrames)), 1, 1024));
    c.staleFrames = uint32_t(std::clamp(cmd.GetInt("-sharc_stale_frames", int(defaults.staleFrames)), 8, 1024));
    // Fixed-point deposits truncate: upstream's 1e3 loses a few % of a dark scene's radiance; 1e4
    // keeps 1e-4 resolution.
    c.radianceScale = std::clamp(cmd.GetFloat("-sharc_radiance_scale", defaults.radianceScale), 1.f, 1e6f);
    return c;
}

const char* SharcCache::InertReason(const IGraphicsDevice& gfx, bool needNative16)
{
    const RHI::DeviceCapabilities caps = gfx.GetCapabilities();
    if (needNative16 && !caps.native16BitShaderOps) return "no native 16-bit shader ops";
    if (!caps.int64HeapAtomics) return "no 64-bit atomics on descriptor-heap resources";
    // A source tree needs the unpacked headers; a package carries none (their
    // license: object code only) and loads the compiled kernels from the
    // shader cache it ships.
    if (!std::filesystem::exists("shaders/Sharc/SharcCommon.h") && !Resource::AssetFS::Get().IsMounted())
        return "sharc headers not unpacked (py -3.12 tools/vendor_sharc.py)";
    return nullptr;
}

bool SharcCache::Allocate(IGraphicsDevice& gfx, uint32_t capacityLog2)
{
    if (IsAllocated() && capacityLog2 == m_capacityLog2) return true;
    Release(gfx);
    const uint64_t capacity = 1ull << capacityLog2;
    RHI::GPUBufferDesc d{};
    d.usage = RHI::Usage::DEFAULT;
    d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    d.stride = 8; d.size = capacity * 8;          // hash keys (uint64)
    bool ok = gfx.CreateBuffer(d, m_hash);
    d.stride = 16; d.size = capacity * 16;        // SharcAccumulationData / SharcPackedData
    ok = ok && gfx.CreateBuffer(d, m_accumulation);
    ok = ok && gfx.CreateBuffer(d, m_resolved);
    if (!ok)
    {
        LOG_ERROR("SharcCache: allocation of 2^%u entries failed", capacityLog2);
        Release(gfx);
        return false;
    }
    m_capacityLog2 = capacityLog2;
    m_hashState = m_accumulationState = m_resolvedState = RHI::ResourceState::UNDEFINED;
    m_clearReason = "allocate";   // every buffer SHaRC reads starts at 0
    return true;
}

void SharcCache::Release(IGraphicsDevice& gfx)
{
    for (auto* b : { &m_hash, &m_accumulation, &m_resolved })
    { if (b->IsValid()) gfx.DestroyBuffer(*b); *b = {}; }
    m_capacityLog2 = 0;
}

const char* SharcCache::TakeClear()
{
    const char* reason = m_clearReason;
    if (reason) { m_clearReason = nullptr; ++m_clears; }
    return reason;
}

void SharcCache::Transition(IGraphicsDevice& gfx, RHI::CommandList cl)
{
    auto state = [&](RHI::GPUBuffer& b, RHI::ResourceState& before) {
        if (before != RHI::ResourceState::UNORDERED_ACCESS)
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b, before, RHI::ResourceState::UNORDERED_ACCESS), cl);
        before = RHI::ResourceState::UNORDERED_ACCESS;
    };
    state(m_hash, m_hashState);
    state(m_accumulation, m_accumulationState);
    state(m_resolved, m_resolvedState);
}

SharcCache::Indices SharcCache::GetIndices(const IGraphicsDevice& gfx) const
{
    Indices i;
    if (!IsAllocated()) return i;
    i.hashUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetBufferUAVGpuHandle(m_hash));
    i.accumulationUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetBufferUAVGpuHandle(m_accumulation));
    i.resolvedUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetBufferUAVGpuHandle(m_resolved));
    i.capacity = Capacity();
    return i;
}
