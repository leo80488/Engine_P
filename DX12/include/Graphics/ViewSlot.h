#pragma once

// Graphics/ViewSlot.h - the view the render half is recording. The same pass instances record every
// view of a frame, one view after another; a per-view resource (a FrameCB's buffer, a PassTexture's
// texture) resolves to the recording view's. Slot 0 is the main view, and is what every resource is
// outside a ViewScope - with no secondary view nothing here changes a thing.
//
// A secondary view's resources exist only while the view does: Renderer::AcquireView makes them
// (EnsureViewSlot, serial, outside any recording) and ReleaseView gives them back (ReleaseViewSlot).
// Each per-view resource type registers itself (ViewSlotResource) when it is created.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class IGraphicsDevice;

namespace RHI
{
    // The main view and up to three secondary views.
    inline constexpr uint32_t kMaxViews = 4;

    // The recording view: its slot, and the epoch of the ViewScope that set it (a new one per
    // scope, so a resource can tell "taken in this view's segment" from "taken in an earlier one").
    struct RecordingView
    {
        uint32_t slot  = 0;
        uint64_t epoch = 0;
    };

    namespace ViewSlotDetail
    {
        // slot in the low 8 bits, the scope's epoch above them; one word, read in one load.
        inline std::atomic<uint64_t> g_recording{ 0 };
        inline std::atomic<uint64_t> g_nextEpoch{ 1 };
    }

    // The render half's recording view: slot 0 outside every ViewScope. Any thread of the render half
    // (the views are recorded one after another, so one value serves every worker of a view).
    inline RecordingView CurrentRecordingView()
    {
        const uint64_t w = ViewSlotDetail::g_recording.load(std::memory_order_acquire);
        return { static_cast<uint32_t>(w & 0xFFu), w >> 8 };
    }

    // View @p slot's segment of the render half: per-view resources resolve to it until the scope
    // ends. Set and ended on the render thread, around work that does not overlap another view's.
    class ViewScope
    {
    public:
        explicit ViewScope(uint32_t slot)
            : m_previous(ViewSlotDetail::g_recording.load(std::memory_order_relaxed))
        {
            const uint64_t epoch = ViewSlotDetail::g_nextEpoch.fetch_add(1, std::memory_order_relaxed);
            ViewSlotDetail::g_recording.store((epoch << 8) | (slot & 0xFFu), std::memory_order_release);
        }
        ~ViewScope() { ViewSlotDetail::g_recording.store(m_previous, std::memory_order_release); }

        ViewScope(const ViewScope&)            = delete;
        ViewScope& operator=(const ViewScope&) = delete;

    private:
        uint64_t m_previous;
    };

    // A resource with a copy per secondary view. Registered while it is alive (Register in its
    // creation, Unregister before it goes); it must not move in between.
    class ViewSlotResource
    {
    public:
        virtual ~ViewSlotResource() = default;
        // Makes view @p slot's copy (1 .. kMaxViews - 1). Serial, outside any recording.
        virtual void EnsureView(IGraphicsDevice& gfx, uint32_t slot) = 0;
        // Gives view @p slot's copy back (deferred destruction: frames in flight may still read it).
        virtual void ReleaseView(IGraphicsDevice& gfx, uint32_t slot) = 0;
    };

    namespace ViewSlotDetail
    {
        inline std::mutex                     g_registryMutex;
        inline std::vector<ViewSlotResource*> g_registry;
        // Which secondary slots are live: a resource created while one is gets its copy at once.
        inline uint32_t                       g_liveMask = 0;
    }

    // Adds @p r to the registry and, when a secondary view is live, makes its copies for it.
    inline void RegisterViewSlotResource(IGraphicsDevice& gfx, ViewSlotResource* r)
    {
        std::lock_guard<std::mutex> lock(ViewSlotDetail::g_registryMutex);
        ViewSlotDetail::g_registry.push_back(r);
        for (uint32_t slot = 1; slot < kMaxViews; ++slot)
            if (ViewSlotDetail::g_liveMask & (1u << slot)) r->EnsureView(gfx, slot);
    }
    inline void UnregisterViewSlotResource(ViewSlotResource* r)
    {
        std::lock_guard<std::mutex> lock(ViewSlotDetail::g_registryMutex);
        auto& reg = ViewSlotDetail::g_registry;
        reg.erase(std::remove(reg.begin(), reg.end(), r), reg.end());
    }

    // View @p slot comes (EnsureViewSlot) or goes (ReleaseViewSlot): every registered resource makes
    // or gives back its copy. Serial, outside any recording.
    inline void EnsureViewSlot(IGraphicsDevice& gfx, uint32_t slot)
    {
        if (slot == 0 || slot >= kMaxViews) return;
        std::lock_guard<std::mutex> lock(ViewSlotDetail::g_registryMutex);
        if (ViewSlotDetail::g_liveMask & (1u << slot)) return;
        ViewSlotDetail::g_liveMask |= 1u << slot;
        for (ViewSlotResource* r : ViewSlotDetail::g_registry) r->EnsureView(gfx, slot);
    }
    inline void ReleaseViewSlot(IGraphicsDevice& gfx, uint32_t slot)
    {
        if (slot == 0 || slot >= kMaxViews) return;
        std::lock_guard<std::mutex> lock(ViewSlotDetail::g_registryMutex);
        if (!(ViewSlotDetail::g_liveMask & (1u << slot))) return;
        ViewSlotDetail::g_liveMask &= ~(1u << slot);
        for (ViewSlotResource* r : ViewSlotDetail::g_registry) r->ReleaseView(gfx, slot);
    }
    // The secondary slots that are live (bit per slot).
    inline uint32_t LiveViewSlots()
    {
        std::lock_guard<std::mutex> lock(ViewSlotDetail::g_registryMutex);
        return ViewSlotDetail::g_liveMask;
    }
} // namespace RHI

namespace RHI
{
    namespace ViewSlotDetail
    {
        inline std::mutex g_takeLocks[16];
    }

    // @p taken is a per-view copy's last scope epoch. The first time it is taken in scope @p epoch,
    // @p fill runs once (under a lock, before anyone sees the new epoch) - a copy of slot 0's value,
    // so a field the view's writer does not set reads what the main view's holds.
    template <class Fill>
    inline void TakeForScope(uint64_t& taken, uint64_t epoch, Fill&& fill)
    {
        std::atomic_ref<uint64_t> t(taken);
        if (t.load(std::memory_order_acquire) == epoch) return;
        std::lock_guard<std::mutex> lock(
            ViewSlotDetail::g_takeLocks[(reinterpret_cast<uintptr_t>(&taken) >> 6) & 15u]);
        if (t.load(std::memory_order_relaxed) == epoch) return;
        fill();
        t.store(epoch, std::memory_order_release);
    }
} // namespace RHI
