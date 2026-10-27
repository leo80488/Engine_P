#include "Graphics/MediumSet.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include "System/WordHash.h"

#include <algorithm>
#include <cstring>

namespace Media
{
namespace
{
    constexpr uint32_t kWordsPerRecord = 6;   // float4 words in the set and in each local volume
    constexpr uint32_t kRecordBytes    = kWordsPerRecord * 16;
    static_assert(sizeof(Set) == kRecordBytes && sizeof(Local) == kRecordBytes,
                  "medium_set.hlsli reads six float4 a record");
}

bool HasMedium(const Set& set)
{
    return set.global.density > 0.f || set.height.density > 0.f || set.localCount > 0;
}

bool SetBuffer::Init(IGraphicsDevice& gfx)
{
    RHI::GPUBufferDesc bd{};
    bd.size      = static_cast<uint64_t>(kRecordBytes) * (1 + kMaxLocals);
    bd.stride    = 16;
    bd.usage     = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    bd.debugName = "MediumSet";
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (!gfx.CreateBuffer(bd, m_buffer[i]))
        {
            LOG_ERROR("MediumSet: buffer %u create failed", i);
            Release(gfx);
            return false;
        }
        m_mapped[i] = gfx.MapBuffer(m_buffer[i]);
    }
    return true;
}

void SetBuffer::Release(IGraphicsDevice& gfx)
{
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_mapped[i]) gfx.UnmapBuffer(m_buffer[i]);
        if (m_buffer[i].IsValid()) gfx.DestroyBuffer(m_buffer[i]);
        m_buffer[i] = {};
        m_mapped[i] = nullptr;
    }
    m_fingerprint = 0;
}

uint32_t SetBuffer::Upload(IGraphicsDevice& gfx, uint32_t slot, Set set, std::span<const Local> locals)
{
    set.localCount = static_cast<uint32_t>(std::min<size_t>(locals.size(), kMaxLocals));
    if (!HasMedium(set) || slot >= RHI::kFramesInFlight || !m_mapped[slot])
    {
        m_fingerprint = 0;
        return 0;
    }
    auto* dst = static_cast<uint8_t*>(m_mapped[slot]);
    std::memcpy(dst, &set, sizeof(set));
    if (set.localCount)
        std::memcpy(dst + kRecordBytes, locals.data(), set.localCount * sizeof(Local));
    m_fingerprint = Sys::WordHash64(dst, kRecordBytes * (1 + set.localCount)) | 1ull;
    const uint32_t index = gfx.GetBufferSRVHeapIndex(m_buffer[slot]);
    return index == ~0u ? 0u : index;
}
}
