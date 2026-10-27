#pragma once

// RenderContext: the per-pass execution context RenderGraph builds each frame.
// It provides:
//   - the resolved RHI textures of the virtual textures declared at Setup;
//   - the named constant buffers the Renderer supplies before Execute;
//   - the draw list (sorted DrawPackets) for passes that issue draw calls.
// The command list is not here: each pass receives its pre-opened
// RHI::CommandList as an Execute() parameter.
// No DX12 types appear here. Passes MUST NOT cache any handle across frames.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/RenderTypes.h"     // DrawPacket, DrawList, DrawFilter
#include "RenderGraph/SceneView.h"    // GetView

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RG
{
    // ==== RGSlot string bridge (the enum lives in GraphicsStruct.h) ====
    // RGSlotFromName is THE string -> slot map: the string APIs below and in
    // RenderGraph resolve through it once, at registration - never in the
    // per-frame bind path. Unknown name -> RGSlot::Count.
    const char* RGSlotName(RGSlot slot);              // debug / log text
    RGSlot      RGSlotFromName(std::string_view name);

    class RenderContext
    {
    public:
        // ---- Undeclared-access validation hook (RenderGraph installs it) ----
        using AccessValidator = void (*)(void* user, RGTextureHandle h);

        using DepthViewLookup = RHI::DepthView (*)(void* user, RGTextureHandle h);

        // ---- Resolved texture handles ----
        const RHI::Texture*   GetTexture(RGTextureHandle h) const;

        // ---- Named constant buffer handles ----
        // The enum overload is the hot path (array index); the const char* twin is a
        // shim for names without an RGSlot (cold fallback map).
        const RHI::GPUBuffer* GetCB(RGSlot slot) const;
        const RHI::GPUBuffer* GetCB(const char* name) const;

        // ---- Named GPU buffer handles (for root SRV binding) ----
        const RHI::GPUBuffer* GetBuffer(RGSlot slot) const;
        const RHI::GPUBuffer* GetBuffer(const char* name) const;

        // ---- Bindless descriptor table handle (g_Buffers[]) ----
        uint64_t GetBindlessTableHandle() const { return m_bindlessTableHandle; }

        // ---- Draw list: sorted packets from Renderer::BuildRenderScene ----
        // Returns all packets that belong to @p filter (contiguous in sorted order).
        DrawList GetDrawList(DrawFilter filter) const;
        // The whole packet array the filter ranges cut (the indirect args are
        // indexed by position in it).
        const std::vector<DrawPacket>* GetAllDrawPackets() const { return m_drawPackets; }

        // ---- Render dimensions ----
        uint32_t GetWidth()  const { return m_width; }
        uint32_t GetHeight() const { return m_height; }

        // ---- The view this execution draws (RenderGraph/SceneView.h) ----
        // Its ShowFlags, view mode, size and cross-frame state, as the graph's owner
        // set it (RenderGraph::SetView). Outside an Execute: a default view, every
        // flag shown.
        const SceneView& GetView() const { return m_view ? *m_view : DefaultView(); }

        // ---- Builder interface: RenderGraph only, never called from passes ----
        void SetTexture(RGTextureHandle h, const RHI::Texture& tex);
        void SetCB(RGSlot slot, const RHI::GPUBuffer& buffer);
        void SetCB(const char* name, const RHI::GPUBuffer& buffer);
        void SetBuffer(RGSlot slot, const RHI::GPUBuffer& buffer);
        void SetBuffer(const char* name, const RHI::GPUBuffer& buffer);
        void SetBindlessTableHandle(uint64_t handle) { m_bindlessTableHandle = handle; }
        // Rebuilds the filter ranges once the final packet order is settled; the
        // packets stay immutable until recording has joined. Each context has its
        // own view's ranges.
        void SetDrawList(const std::vector<DrawPacket>* packets);
        void SetDimensions(uint32_t w, uint32_t h)  { m_width  = w; m_height = h; }
        void SetView(const SceneView* view)         { m_view = view; }
        void Clear();

        // ---- Undeclared-access validation hook (RenderGraph installs it) ----
        // GetTexture() is the ONE way a pass reaches a graph texture, so it is also
        // the one place to tell whether the pass declared that texture in Setup().
        // RenderGraph installs a thunk here (RG_VALIDATE_ACCESS); null costs nothing.
        void SetAccessValidator(AccessValidator fn, void* user)
        { m_accessValidator = fn; m_accessValidatorUser = user; }

        // ---- Depth views ----
        // The DSV view the recording pass binds @p h with
        // (IGraphicsDevice::SetRenderTargetToHdrWithDepth & co.): its
        // RenderGraphBuilder::DepthStencil declaration and the planes' states after
        // its Phase A barriers. RHI::DepthView{} (the writable DSV, nothing checked)
        // for a texture the pass did not declare so, outside a pass, or under
        // -rgplaneoff. RenderGraph installs the lookup.
        RHI::DepthView DepthView(RGTextureHandle h) const
        { return m_depthViewLookup ? m_depthViewLookup(m_depthViewUser, h) : RHI::DepthView{}; }

        void SetDepthViewLookup(DepthViewLookup fn, void* user)
        { m_depthViewLookup = fn; m_depthViewUser = user; }

    private:
        struct TextureSlot
        {
            RHI::Texture texture;
            bool present = false; // distinguishes a sparse hole from a bound invalid texture
        };

        static const SceneView& DefaultView() { static const SceneView kDefaultView{}; return kDefaultView; }

        AccessValidator m_accessValidator     = nullptr;
        void*           m_accessValidatorUser = nullptr;
        DepthViewLookup m_depthViewLookup     = nullptr;
        void*           m_depthViewUser       = nullptr;

        uint32_t         m_width  = 0;
        uint32_t         m_height = 0;
        const SceneView* m_view   = nullptr;   // the graph's view (RenderGraph::SetView)

        const std::vector<DrawPacket>*                         m_drawPackets = nullptr;
        uint64_t                                               m_bindlessTableHandle = 0;

        std::vector<TextureSlot> m_textures;
        std::array<DrawList, size_t(DrawFilter::Count)> m_drawRanges{};
        // Hot storage: slot-indexed, filled once per frame, read lock-free by
        // concurrently recording passes. CBs and buffers have separate arrays and
        // getters.
        std::array<const RHI::GPUBuffer*, size_t(RGSlot::Count)> m_cbSlots{};
        std::array<const RHI::GPUBuffer*, size_t(RGSlot::Count)> m_bufferSlots{};
        // Cold fallback: only names with no RGSlot land here (editor / debug
        // freedom); the shipping frame never touches these maps.
        std::unordered_map<std::string, const RHI::GPUBuffer*> m_cbFallback;
        std::unordered_map<std::string, const RHI::GPUBuffer*> m_bufferFallback;
    };

} // namespace RG
