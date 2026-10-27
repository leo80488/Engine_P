#pragma once

// FrameCB<T> - kFrameCount-deep ring of persistently mapped UPLOAD constant
// buffers. A single mapped CB written every frame would be overwritten by frame
// N+1's CPU while frame N's GPU still reads it; each slot is a separate physical
// buffer, indexed by the frame slot.
//
// Usage:
//   FrameCB<MyCB> m_cb;
//   Init:    m_cb.Create(gfx, "MyPass.CB");
//   Execute: *m_cb.Current(gfx) = liveData;
//            gfx.SetComputeRootCBV(slot, m_cb.CurrentBuffer(gfx), cl);
//   Destroy: m_cb.Destroy(gfx);
//
// Views (Graphics/ViewSlot.h): the same pass records every view of a frame, so a
// render-side write in a secondary view's segment goes to that view's own ring
// (made while the view is live) and does not overwrite the main view's. The first
// Current() or CurrentBuffer() in a view's segment copies slot 0's value in - the
// main view has finished the frame by then - so a field the view does not set reads
// what the main view's holds, and a buffer bound before its writer runs (a CB the
// graph binds by name) is still the one the writer fills.

#include "Graphics/FrameConstants.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ViewSlot.h"

#include "System/Check.h"

#include <cassert>
#include <cstring>

template <typename T>
struct FrameCB : RHI::ViewSlotResource
{
    // Equals GraphicsDX12::kFrameCount (both are RHI::kFramesInFlight) without
    // including the DX12 header.
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    FrameCB() = default;
    // Registered by address while it lives (ViewSlotResource): it never moves.
    FrameCB(const FrameCB&)            = delete;
    FrameCB& operator=(const FrameCB&) = delete;
    ~FrameCB() override
    {
        if (m_registered) RHI::UnregisterViewSlotResource(this);
    }

    bool Create(IGraphicsDevice& gfx, const char* /*debugName*/ = nullptr)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
            if (!CreateSlot(gfx, buffers[i], mapped[i])) return false;
        if (!m_registered)
        {
            m_registered = true;
            RHI::RegisterViewSlotResource(gfx, this);
        }
        return true;
    }

    void Destroy(IGraphicsDevice& gfx)
    {
        if (m_registered)
        {
            RHI::UnregisterViewSlotResource(this);
            m_registered = false;
        }
        for (uint32_t v = 1; v < RHI::kMaxViews; ++v) ReleaseView(gfx, v);
        for (uint32_t i = 0; i < kFrameCount; ++i) DestroySlot(gfx, buffers[i], mapped[i]);
    }

    bool IsValid() const { return buffers[0].IsValid(); }

    // ---- Slot accessors ----
    // Current*() = the slot of the frame being rendered; Upload*() = the slot of the
    // frame the game side is simulating. They agree while the loop is serial and
    // differ by one once the render thread runs a frame behind, so the wrong one
    // aliases another frame's constants: no crash, nothing from the debug layer,
    // intermittent flicker.
    // Writing from a RenderPass or inside Renderer::Render -> Current(); from
    // Renderer::BeginFrame or any earlier tick phase -> Upload().
    // Current*() need BeginFrame to have run (gfx.GetFrameIndex() is the frame being
    // rendered). In a secondary view's segment they are that view's (see the top).
    T*                    Current      (IGraphicsDevice& gfx)
    {
        const uint32_t s = gfx.GetFrameIndex();
        // ENGINE_CHECK_ONCE, not assert: assert is compiled out of Release, where an
        // out-of-range slot silently reads the wrong ring buffer (flicker, not a crash).
        ENGINE_CHECK_ONCE(s < kFrameCount,
            "[SPLITCHECK][SLOTPARITY] FrameCB slot %u >= %u", s, kFrameCount);
        const RHI::RecordingView v = RHI::CurrentRecordingView();
        if (v.slot == 0 || v.slot >= RHI::kMaxViews || s >= kFrameCount) return mapped[s];
        View& view = m_views[s][v.slot - 1];
        if (!view.mapped)
        {
            ENGINE_CHECK_ONCE(false, "FrameCB: view %u has no ring of its own (it is not live)", v.slot);
            return mapped[s];
        }
        RHI::TakeForScope(view.taken, v.epoch, [&] { std::memcpy(view.mapped, mapped[s], sizeof(T)); });
        return view.mapped;
    }
    const RHI::GPUBuffer& CurrentBuffer(IGraphicsDevice& gfx) const
    {
        const uint32_t s = gfx.GetFrameIndex();
        // ENGINE_CHECK_ONCE, not assert: see Current().
        ENGINE_CHECK_ONCE(s < kFrameCount,
            "[SPLITCHECK][SLOTPARITY] FrameCB slot %u >= %u", s, kFrameCount);
        const RHI::RecordingView v = RHI::CurrentRecordingView();
        if (v.slot == 0 || v.slot >= RHI::kMaxViews || s >= kFrameCount) return buffers[s];
        View& view = m_views[s][v.slot - 1];
        if (!view.mapped)
        {
            ENGINE_CHECK_ONCE(false, "FrameCB: view %u has no ring of its own (it is not live)", v.slot);
            return buffers[s];
        }
        RHI::TakeForScope(view.taken, v.epoch, [&] { std::memcpy(view.mapped, mapped[s], sizeof(T)); });
        return view.buffer;
    }

    // Game-side slot accessors: the frame being simulated. Valid from
    // AcquireUploadSlot (App's Animation phase) on, strictly earlier than Current().
    T*                    Upload      (IGraphicsDevice& gfx)
    {
        const uint32_t s = gfx.GetUploadSlot();
        ENGINE_CHECK_ONCE(s < kFrameCount,
            "[SPLITCHECK][SLOTPARITY] FrameCB upload slot %u >= %u", s, kFrameCount);
        return mapped[s];
    }
    const RHI::GPUBuffer& UploadBuffer(IGraphicsDevice& gfx) const
    {
        const uint32_t s = gfx.GetUploadSlot();
        ENGINE_CHECK_ONCE(s < kFrameCount,
            "[SPLITCHECK][SLOTPARITY] FrameCB upload slot %u >= %u", s, kFrameCount);
        return buffers[s];
    }

    // An EXPLICIT slot: the frame's upload slot carried as a lease (Renderer's
    // m_frameUploadSlot), for game-half work that runs on the render-owner lane
    // after main has moved on. Neither accessor above answers that: Upload() is
    // main's current game slot (the NEXT frame's once main acquires it) and
    // Current() is the render slot, which the lane advances later.
    T*                    At          (uint32_t s)
    {
        ENGINE_CHECK_ONCE(s < kFrameCount,
            "[SPLITCHECK][SLOTPARITY] FrameCB slot %u >= %u", s, kFrameCount);
        return s < kFrameCount ? mapped[s] : nullptr;
    }

    // ---- RHI::ViewSlotResource ----
    void EnsureView(IGraphicsDevice& gfx, uint32_t slot) override
    {
        if (slot == 0 || slot >= RHI::kMaxViews || !IsValid()) return;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            View& view = m_views[i][slot - 1];
            if (!view.mapped && !CreateSlot(gfx, view.buffer, view.mapped)) return;
            view.taken = 0;
        }
    }
    void ReleaseView(IGraphicsDevice& gfx, uint32_t slot) override
    {
        if (slot == 0 || slot >= RHI::kMaxViews) return;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            View& view = m_views[i][slot - 1];
            DestroySlot(gfx, view.buffer, view.mapped);
            view.taken = 0;
        }
    }

private:
    // A secondary view's ring entry, and the scope that last took it (RHI::TakeForScope).
    struct View
    {
        RHI::GPUBuffer buffer;
        T*             mapped = nullptr;
        uint64_t       taken  = 0;
    };

    static bool CreateSlot(IGraphicsDevice& gfx, RHI::GPUBuffer& buffer, T*& ptr)
    {
        RHI::GPUBufferDesc d{};
        // CBs are 256-aligned per the D3D12 spec.
        const uint64_t paddedSize = (sizeof(T) + 255ull) & ~255ull;
        d.size       = paddedSize;
        d.usage      = RHI::Usage::UPLOAD;
        d.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
        if (!gfx.CreateBuffer(d, buffer)) return false;
        ptr = static_cast<T*>(gfx.MapBuffer(buffer));
        if (!ptr) return false;
        // UPLOAD heap memory arrives uninitialized: a field a shader reads before the first
        // CPU write (e.g. a `_pad` slot repurposed into a knob) would read garbage on the
        // first frames. Zero is the neutral value for every such field.
        std::memset(ptr, 0, static_cast<size_t>(paddedSize));
        return true;
    }
    static void DestroySlot(IGraphicsDevice& gfx, RHI::GPUBuffer& buffer, T*& ptr)
    {
        if (ptr)                { gfx.UnmapBuffer(buffer); ptr = nullptr; }
        if (buffer.IsValid())     gfx.DestroyBuffer(buffer);
        buffer = {};
    }

public:
    RHI::GPUBuffer  buffers[kFrameCount];
    T*              mapped [kFrameCount] = {};

private:
    mutable View m_views[kFrameCount][RHI::kMaxViews - 1];   // CurrentBuffer() takes one too
    bool m_registered = false;
};
