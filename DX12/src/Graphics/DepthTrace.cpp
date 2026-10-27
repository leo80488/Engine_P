#include "Graphics/DepthTrace.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    // Pool slots this trace can name: the backend's own command-list capacity, so a
    // heavy frame cannot lose its tail from the roster.
    constexpr uint32_t kSlots = GraphicsDX12::GetCommandListCapacity();

    // Written by the thread that opened the list, read by whichever worker pushes a
    // barrier on it. Atomic so a pointer is never torn; relaxed because the Phase A /
    // Phase B handoff already orders the roster line.
    std::atomic<const char*> g_label[kSlots];
    std::atomic<uint8_t>     g_touchedDepth[kSlots];
    std::atomic<uint64_t>    g_frame{0};

    const char* StateName(RHI::ResourceState s)
    {
        using RS = RHI::ResourceState;
        switch (s)
        {
        case RS::UNDEFINED:               return "UNDEFINED";
        case RS::SHADER_RESOURCE:         return "SHADER_RESOURCE";
        case RS::SHADER_RESOURCE_COMPUTE: return "SRV_COMPUTE";
        case RS::UNORDERED_ACCESS:        return "UAV";
        case RS::COPY_SRC:                return "COPY_SRC";
        case RS::COPY_DST:                return "COPY_DST";
        case RS::RENDERTARGET:            return "RENDERTARGET";
        case RS::DEPTHSTENCIL:            return "DEPTHSTENCIL";
        case RS::DEPTHSTENCIL_READONLY:   return "DEPTHSTENCIL_RO";
        case RS::DEPTH_READ_SRV:          return "DEPTH_READ_SRV";
        default:                          return "other";
        }
    }

    const char* LabelOf(uint32_t clId)
    {
        if (clId >= kSlots) return "?";
        const char* n = g_label[clId].load(std::memory_order_relaxed);
        return n ? n : "?";
    }
}

namespace GfxTrace
{
    bool Enabled()
    {
        // Read once and cached, like GraphicsDX12's DX12_DEBUGLAYER / DX12_DRED gates. Off
        // by default in every config: a diagnostic, not a validator.
        static const bool s_on = []
        {
            char buf[8]{};
            const DWORD n = ::GetEnvironmentVariableA("DX12_DEPTHTRACE", buf, sizeof(buf));
            return n != 0 && n < sizeof(buf) && buf[0] != '0';
        }();
        return s_on;
    }

    void BeginFrame(uint64_t frameIndex)
    {
        if (!Enabled()) return;
        g_frame.store(frameIndex, std::memory_order_relaxed);
        for (uint32_t i = 0; i < kSlots; ++i)
        {
            g_label[i].store(nullptr, std::memory_order_relaxed);
            g_touchedDepth[i].store(0, std::memory_order_relaxed);
        }
    }

    void Label(const RHI::CommandList& cl, const char* name)
    {
        if (!Enabled() || !cl.IsValid() || !name) return;
        const uint32_t id = cl.internalId;
        if (id >= kSlots) return;
        g_label[id].store(name, std::memory_order_relaxed);
        LOG_INFO("[DEPTHTRACE] f=%llu roster cl=%u name=%s",
                 static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)),
                 id, name);
    }

    void ImageBarrier(uint32_t clId, const char* name,
                      RHI::ResourceState before, RHI::ResourceState after,
                      uint32_t d3dBefore, uint32_t d3dAfter, int plane)
    {
        if (!Enabled()) return;
        const char* n = name;
        // Substring, not equality: the preview layer's graph copy is named
        // "<p>GBuffer_Depth", and the full name in the log tells the two apart.
        if (!n || !std::strstr(n, "GBuffer_Depth")) return;
        if (clId < kSlots) g_touchedDepth[clId].store(1, std::memory_order_relaxed);
        if (plane >= 0)
            LOG_INFO("[DEPTHTRACE] f=%llu cl=%u pass=%s tex=%s before=%s(0x%X) after=%s(0x%X) plane=%d",
                     static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)),
                     clId, LabelOf(clId), n,
                     StateName(before), d3dBefore,
                     StateName(after),  d3dAfter, plane);
        else
            LOG_INFO("[DEPTHTRACE] f=%llu cl=%u pass=%s tex=%s before=%s(0x%X) after=%s(0x%X)",
                     static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)),
                     clId, LabelOf(clId), n,
                     StateName(before), d3dBefore,
                     StateName(after),  d3dAfter);
    }

    void Edges(uint32_t clId, const uint32_t* deps, uint32_t count)
    {
        if (!Enabled()) return;
        if (clId >= kSlots || !g_touchedDepth[clId].load(std::memory_order_relaxed)) return;
        char line[512];
        int  w = 0;
        for (uint32_t i = 0; i < count && w < static_cast<int>(sizeof(line)) - 48; ++i)
            w += snprintf(line + w, sizeof(line) - static_cast<size_t>(w), "%s%u(%s)",
                          w ? "," : "", deps[i], LabelOf(deps[i]));
        LOG_INFO("[DEPTHTRACE] f=%llu edges cl=%u(%s) waits_on=[%s]",
                 static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)),
                 clId, LabelOf(clId), w ? line : "");
    }

    void SubmitOrder(const uint32_t* order, uint32_t count)
    {
        if (!Enabled() || !order) return;
        const unsigned long long f = g_frame.load(std::memory_order_relaxed);

        // Depth touchers first: the line ordering defects are read from.
        char line[1024];
        int  w = 0;
        for (uint32_t i = 0; i < count && w < static_cast<int>(sizeof(line)) - 64; ++i)
        {
            const uint32_t id = order[i];
            if (id >= kSlots || !g_touchedDepth[id].load(std::memory_order_relaxed)) continue;
            w += snprintf(line + w, sizeof(line) - static_cast<size_t>(w), "%s%u(%s)",
                          w ? " -> " : "", id, LabelOf(id));
        }
        if (w) LOG_INFO("[DEPTHTRACE] f=%llu submit(depth): %s", f, line);

        // Then the whole order, chunked so no single line is truncated.
        w = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            w += snprintf(line + w, sizeof(line) - static_cast<size_t>(w),
                          "%s%u", w ? "," : "", order[i]);
            if (w > static_cast<int>(sizeof(line)) - 16)
            {
                LOG_INFO("[DEPTHTRACE] f=%llu submit(all): %s", f, line);
                w = 0;
            }
        }
        if (w) LOG_INFO("[DEPTHTRACE] f=%llu submit(all): %s", f, line);
    }
}
