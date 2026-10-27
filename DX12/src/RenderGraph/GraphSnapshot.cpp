#include "RenderGraph/GraphSnapshot.h"
#include "Graphics/GraphicsStruct.h"   // RHI::ResourceState (StateName)

#include <cstdio>

namespace RG
{
    uint32_t GraphSnapshot::RanCount() const
    {
        uint32_t n = 0;
        for (const Pass& p : passes) n += p.Ran() ? 1u : 0u;
        return n;
    }

    uint32_t GraphSnapshot::BackedCount() const
    {
        uint32_t n = 0;
        for (const Texture& t : textures) n += t.backed ? 1u : 0u;
        return n;
    }

    uint32_t GraphSnapshot::BarrierCount() const
    {
        uint32_t n = 0;
        for (const Pass& p : passes) n += static_cast<uint32_t>(p.barriers.size());
        return n;
    }

    uint32_t GraphSnapshot::WaitCount() const
    {
        uint32_t n = 0;
        for (const Pass& p : passes) n += p.externalWaits;
        return n;
    }

    uint32_t GraphSnapshot::AuxTailCount() const
    {
        uint32_t n = 0;
        for (const Pass& p : passes) n += p.auxTailCl != ~0u ? 1u : 0u;
        return n;
    }

    uint32_t GraphSnapshot::UndeclaredCount() const
    {
        uint32_t n = 0;
        for (const Pass& p : passes) n += static_cast<uint32_t>(p.undeclared.size());
        return n;
    }

    std::string GraphSnapshot::StateName(uint32_t s)
    {
        using RS = RHI::ResourceState;
        switch (static_cast<RS>(s))
        {
        case RS::UNDEFINED:                         return "UNDEFINED";
        case RS::SHADER_RESOURCE:                   return "SHADER_RESOURCE";
        case RS::SHADER_RESOURCE_COMPUTE:           return "SHADER_RESOURCE_COMPUTE";
        case RS::UNORDERED_ACCESS:                  return "UNORDERED_ACCESS";
        case RS::COPY_SRC:                          return "COPY_SRC";
        case RS::COPY_DST:                          return "COPY_DST";
        case RS::RENDERTARGET:                      return "RENDERTARGET";
        case RS::DEPTHSTENCIL:                      return "DEPTHSTENCIL";
        case RS::DEPTHSTENCIL_READONLY:             return "DEPTHSTENCIL_READONLY";
        case RS::SHADING_RATE_SOURCE:               return "SHADING_RATE_SOURCE";
        case RS::VERTEX_BUFFER:                     return "VERTEX_BUFFER";
        case RS::INDEX_BUFFER:                      return "INDEX_BUFFER";
        case RS::CONSTANT_BUFFER:                   return "CONSTANT_BUFFER";
        case RS::INDIRECT_ARGUMENT:                 return "INDIRECT_ARGUMENT";
        case RS::RAYTRACING_ACCELERATION_STRUCTURE: return "RAYTRACING_ACCELERATION_STRUCTURE";
        case RS::PREDICATION:                       return "PREDICATION";
        case RS::VIDEO_DECODE_SRC:                  return "VIDEO_DECODE_SRC";
        case RS::VIDEO_DECODE_DST:                  return "VIDEO_DECODE_DST";
        case RS::VIDEO_DECODE_DPB:                  return "VIDEO_DECODE_DPB";
        case RS::SWAPCHAIN:                         return "SWAPCHAIN";
        case RS::DEPTH_READ_SRV:                    return "DEPTH_READ_SRV";
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%X", s);
        return buf;
    }

    uint32_t GraphSnapshot::UnreadCount() const
    {
        uint32_t n = 0;
        for (const Pass& p : passes) n += p.unread ? 1u : 0u;
        return n;
    }

    SnapshotBoard& MainGraphSnapshots()
    {
        static SnapshotBoard s_board;
        return s_board;
    }
}
