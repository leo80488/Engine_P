#include "RenderGraph/RenderContext.h"
#include "System/Check.h"
#include "System/Log.h"

#include <iterator>

namespace
{
    // Index-paired with RG::RGSlot - the static_assert below makes a new slot
    // fail the build instead of silently resolving to "unknown name".
    constexpr const char* kRGSlotNames[] = {
        "PerView",
        "LightCB",
        "TerrainParams",
        "SkyCB",
        "InstanceBuffer",
        "MeshDescriptors",
        "MaterialBuffer",
        "MeshletCull",
        "FogApply",
    };
    static_assert(std::size(kRGSlotNames) == size_t(RG::RGSlot::Count),
                  "kRGSlotNames must mirror RGSlot exactly (add new names at the end)");
}

namespace RG
{
    const char* RGSlotName(RGSlot slot)
    {
        return slot < RGSlot::Count ? kRGSlotNames[size_t(slot)] : "<invalid RGSlot>";
    }

    RGSlot RGSlotFromName(std::string_view name)
    {
        // Linear scan over a dozen short literals: this runs at registration time
        // (a handful of calls per frame), never in the per-draw path.
        for (size_t i = 0; i < size_t(RGSlot::Count); ++i)
            if (name == kRGSlotNames[i])
                return RGSlot(i);
        return RGSlot::Count;
    }

    const RHI::Texture* RenderContext::GetTexture(RGTextureHandle h) const
    {
        // Undeclared-access check (the graph installs the thunk). A pass that
        // reaches a graph texture it never declared in Setup() gets NO barrier from
        // the graph, so its correctness silently depends on the state the previous
        // pass left - the "reordering passes breaks lighting" class of bug.
        if (m_accessValidator)
            m_accessValidator(m_accessValidatorUser, h);

        if (!h.IsValid() || h.id >= m_textures.size() || !m_textures[h.id].present)
        {
            LOG_ERROR("RenderContext: Texture not found for handle %u", h.id);
            return nullptr;
        }
        return &m_textures[h.id].texture;
    }

    const RHI::GPUBuffer* RenderContext::GetCB(RGSlot slot) const
    {
        if (!ENGINE_CHECK_ONCE(slot < RGSlot::Count,
                               "RenderContext::GetCB: invalid slot %u", unsigned(slot)))
            return nullptr;
        const RHI::GPUBuffer* cb = m_cbSlots[size_t(slot)];
        // Missing binding is a wiring error (Renderer registers before Execute);
        // _ONCE because this sits inside the per-frame recording path.
        ENGINE_CHECK_ONCE(cb != nullptr, "RenderContext: CB '%s' not bound", RGSlotName(slot));
        return cb;
    }

    const RHI::GPUBuffer* RenderContext::GetCB(const char* name) const
    {
        const RGSlot slot = RGSlotFromName(name);
        if (slot != RGSlot::Count)
            return GetCB(slot);

        auto it = m_cbFallback.find(name);
        if (it == m_cbFallback.end())
        {
            LOG_ERROR("RenderContext: CB '%s' not found", name);
            return nullptr;
        }
        return it->second;
    }

    DrawList RenderContext::GetDrawList(DrawFilter filter) const
    {
        const size_t index = static_cast<size_t>(filter);
        return index < m_drawRanges.size() ? m_drawRanges[index] : DrawList{};
    }

    void RenderContext::SetDrawList(const std::vector<DrawPacket>* packets)
    {
        m_drawPackets = packets;
        m_drawRanges.fill(DrawList{});
        if (!packets || packets->empty()) return;
        size_t first = 0;
        while (first < packets->size())
        {
            const DrawFilter filter = (*packets)[first].filter;
            size_t last = first + 1;
            while (last < packets->size() && (*packets)[last].filter == filter) ++last;
            const size_t index = static_cast<size_t>(filter);
            if (index < m_drawRanges.size() && m_drawRanges[index].empty())
                m_drawRanges[index] = DrawList{ packets->data() + first, last - first };
            first = last;
        }
    }

    void RenderContext::SetTexture(RGTextureHandle h, const RHI::Texture& tex)
    {
        if (!ENGINE_CHECK_ONCE(h.IsValid(), "RenderContext::SetTexture: invalid handle")) return;
        if (h.id >= m_textures.size()) m_textures.resize(size_t(h.id) + 1);
        m_textures[h.id].texture = tex;
        m_textures[h.id].present = true;
    }

    const RHI::GPUBuffer* RenderContext::GetBuffer(RGSlot slot) const
    {
        if (!ENGINE_CHECK_ONCE(slot < RGSlot::Count,
                               "RenderContext::GetBuffer: invalid slot %u", unsigned(slot)))
            return nullptr;
        const RHI::GPUBuffer* buf = m_bufferSlots[size_t(slot)];
        ENGINE_CHECK_ONCE(buf != nullptr, "RenderContext: Buffer '%s' not bound", RGSlotName(slot));
        return buf;
    }

    const RHI::GPUBuffer* RenderContext::GetBuffer(const char* name) const
    {
        const RGSlot slot = RGSlotFromName(name);
        if (slot != RGSlot::Count)
            return GetBuffer(slot);

        auto it = m_bufferFallback.find(name);
        if (it == m_bufferFallback.end())
        {
            LOG_ERROR("RenderContext: Buffer '%s' not found", name);
            return nullptr;
        }
        return it->second;
    }

    void RenderContext::SetCB(RGSlot slot, const RHI::GPUBuffer& buffer)
    {
        if (ENGINE_CHECK_ONCE(slot < RGSlot::Count,
                              "RenderContext::SetCB: invalid slot %u", unsigned(slot)))
            m_cbSlots[size_t(slot)] = &buffer;
    }

    void RenderContext::SetCB(const char* name, const RHI::GPUBuffer& buffer)
    {
        const RGSlot slot = RGSlotFromName(name);
        if (slot != RGSlot::Count) { SetCB(slot, buffer); return; }

        // Unknown name - keep working (editor / debug freedom), but say so once:
        // this path hashes a string every frame, and the fix is one line in RGSlot
        // + kRGSlotNames.
        static bool s_warnedCB = false;
        if (!s_warnedCB)
        {
            s_warnedCB = true;
            LOG_WARNING("RenderContext::SetCB: '%s' has no RGSlot — using cold string fallback", name);
        }
        m_cbFallback[name] = &buffer;
    }

    void RenderContext::SetBuffer(RGSlot slot, const RHI::GPUBuffer& buffer)
    {
        if (ENGINE_CHECK_ONCE(slot < RGSlot::Count,
                              "RenderContext::SetBuffer: invalid slot %u", unsigned(slot)))
            m_bufferSlots[size_t(slot)] = &buffer;
    }

    void RenderContext::SetBuffer(const char* name, const RHI::GPUBuffer& buffer)
    {
        const RGSlot slot = RGSlotFromName(name);
        if (slot != RGSlot::Count) { SetBuffer(slot, buffer); return; }

        static bool s_warnedBuffer = false;
        if (!s_warnedBuffer)
        {
            s_warnedBuffer = true;
            LOG_WARNING("RenderContext::SetBuffer: '%s' has no RGSlot — using cold string fallback", name);
        }
        m_bufferFallback[name] = &buffer;
    }

    void RenderContext::Clear()
    {
        m_width               = 0;
        m_height              = 0;
        m_view                = nullptr;
        m_drawPackets         = nullptr;
        m_bindlessTableHandle = 0;
        m_textures.clear(); // capacity survives; no old bindings survive a new execution
        m_drawRanges.fill(DrawList{});
        m_accessValidator = nullptr;
        m_accessValidatorUser = nullptr;
        m_cbSlots.fill(nullptr);
        m_bufferSlots.fill(nullptr);
        m_cbFallback.clear();
        m_bufferFallback.clear();
    }

} // namespace RG
