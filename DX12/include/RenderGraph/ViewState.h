#pragma once

// RenderGraph/ViewState.h - one view's cross-frame state: its TAA jitter and
// previous view-projection, the camera the Hi-Z pyramid was built from, and every
// temporal pass's history, in one place with one reset path. It lives beside the
// passes, not in Renderer/, because the passes keep their histories in it.
//
// The main view is Renderer::m_mainView (slot 0); a secondary view has a slot of its
// own (Graphics/ViewSlot.h). A pass's history is found by (view, pass instance), never
// kept on the pass, so every view reuses the same pass instances; so is the rest of a
// pass's per-view state (ViewData: its view-sized textures, camera caches, counters).
//
// A history is an object of the pass's own ViewHistory-derived type. The Renderer
// binds the view to every pass that keeps one as it registers
// (RG::PassLifecycle::BindView via PassSet's Register / RegisterView,
// Renderer::Compile, main thread); the pass creates it there with
// History<H>(owner) and keeps the reference. It starts EMPTY: the pass allocates
// its GPU resources into it at its own point in the frame (allocation order
// changes pictures). Find<H>(owner) looks one up from any thread; creating one
// logs [VIEW_STATE] with the reasons that reset it.
//
// Every break goes through Invalidate(reasons), which hands each history the
// reasons it resets on: the hard cut (Renderer::InvalidateTemporalHistory), a
// resize (BeginFrame's recompile), a world clear (Renderer::OnWorldClear), the
// camera's cut. A reason raised on the game side (Renderer::RaiseViewBreak, any
// thread) travels in the FrameView being built; the render half passes BeginFrame
// that frame's reasons, its own (a render-path change, -restir_reset_frame) and
// the camera's cut, and the view applies them ONCE before anything of the frame
// records. FrameBreaks keeps them for passes that read them while recording.
//
// The content cuts (HistoryReset::Cuts) reset every history; the exposure snaps on
// a hard cut or a world clear, not on a camera cut. A resize resets what depends
// on the size, PathChange what depends on the lighting path - the ReSTIR budget
// ladder changes path mid-play, and the fog, the AO or the exposure must not jump.

#include "Graphics/TAAJitterState.h"

#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <typeindex>
#include <utility>
#include <vector>

class IGraphicsDevice;

// Why a view's history breaks (bits). Each history names the ones it resets on
// (ViewHistory::ResetsOn); all of them go through ViewState::Invalidate.
namespace HistoryReset
{
    enum : uint32_t
    {
        HardCut    = 1u << 0,   // the shown content cuts: Play / Stop, a Sequencer seek, the editor's view switch,
                                // -restir_reset_frame
        CameraCut  = 1u << 1,   // the camera does not continue the last frame (historyValid == false)
        Resize     = 1u << 2,   // the view's render or display size changed
        WorldClear = 1u << 3,   // the World was cleared (a scene load, Play / Stop's restore)
        PathChange = 1u << 4,   // the render path changed (the user's choice, the ReSTIR budget ladder, a backend
                                // switch): the lighting changes, the camera and the world do not
    };
    // The view's content cuts: every history resets on them. A resize only resets
    // what depends on the size, a path change what depends on the lighting path.
    constexpr uint32_t Cuts = HardCut | CameraCut | WorldClear;
    // "HardCut,Resize" into @p out (size >= 48).
    void Names(uint32_t bits, char* out, size_t size);
}

// A pass's cross-frame state for one view.
class ViewHistory
{
public:
    virtual ~ViewHistory() = default;
    // A string literal (the census keeps the pointer).
    virtual const char* Name() const = 0;
    // The HistoryReset bits that reset it.
    virtual uint32_t    ResetsOn() const = 0;
    // The view broke for @p reasons (only bits ResetsOn() names): reset what
    // the view tells you about - ViewState::Invalidate calls it on the thread
    // that invalidated. A reason the pass notices by itself (a new size its
    // EnsureTextures compares, a rebuilt context) needs nothing here; the
    // override says where it is noticed.
    virtual void        Invalidate(uint32_t reasons) = 0;
    // The view goes away (or the Renderer shuts down, after the GPU drained):
    // release every GPU resource it holds.
    virtual void        Release(IGraphicsDevice& gfx) = 0;
};

// A pass's per-view state that is not a history - nothing resets it on a cut: its
// view-sized textures and buffers, the camera it cached, its counters. Made in the
// pass's BindView, given back when the view goes.
class ViewData
{
public:
    virtual ~ViewData() = default;
    // The view goes away (or the Renderer shuts down, after the GPU drained):
    // release every GPU resource it holds (deferred destruction).
    virtual void Release(IGraphicsDevice& gfx) = 0;
};

// The camera the Hi-Z pyramid was generated from - the NEXT frame's occlusion
// cull projects with it (the pyramid is a frame old; projecting with the
// current view-projection mis-taps by the camera's per-frame motion). Pos /
// fwd feed the motion gate (Renderer::HiZUsableThisFrame) that disables the
// test for a frame on big jumps historyValid misses.
struct HiZCapture
{
    // Frames since the pyramid was last generated at the current size; 0 =
    // stale / none, the cull skips the occlusion test. Reset on a world clear
    // and whenever generation is skipped.
    uint32_t            framesValid = 0;
    DirectX::XMFLOAT4X4 viewProj{};
    DirectX::XMFLOAT3   camPos{};
    DirectX::XMFLOAT3   camFwd{};
};

class ViewState
{
public:
    // @p slot: the view's slot (Graphics/ViewSlot.h) - 0 the main view.
    explicit ViewState(const char* name, uint32_t slot = 0) : m_name(name), m_slot(slot) {}
    ~ViewState();

    ViewState(const ViewState&)            = delete;
    ViewState& operator=(const ViewState&) = delete;

    const char* Name() const { return m_name; }
    uint32_t    Slot() const { return m_slot; }

    // @p owner's history of type @p H in this view - created (with @p args)
    // and logged the first time. Main thread only (the pass's BindView).
    template <class H, class... Args>
    H& History(const void* owner, Args&&... args)
    {
        if (H* h = Find<H>(owner))
            return *h;
        auto made = std::make_unique<H>(std::forward<Args>(args)...);
        H& ref = *made;
        Add(owner, typeid(H), std::move(made));
        return ref;
    }

    // @p owner's history of type @p H, or null - any thread.
    template <class H>
    H* Find(const void* owner) const
    {
        for (const Entry& e : m_histories)
            if (e.owner == owner && e.type == typeid(H))
                return static_cast<H*>(e.history.get());
        return nullptr;
    }

    size_t HistoryCount() const { return m_histories.size(); }

    // @p owner's per-view state of type @p D in this view - created (with @p args)
    // the first time. Main thread only (the pass's BindView).
    template <class D, class... Args>
    D& Data(const void* owner, Args&&... args)
    {
        for (const DataEntry& e : m_data)
            if (e.owner == owner && e.type == typeid(D))
                return *static_cast<D*>(e.data.get());
        auto made = std::make_unique<D>(std::forward<Args>(args)...);
        D& ref = *made;
        m_data.push_back(DataEntry{ owner, typeid(D), std::move(made) });
        return ref;
    }
    // @p owner's per-view state of type @p D, or null - any thread.
    template <class D>
    D* FindData(const void* owner) const
    {
        for (const DataEntry& e : m_data)
            if (e.owner == owner && e.type == typeid(D))
                return static_cast<D*>(e.data.get());
        return nullptr;
    }
    size_t DataCount() const { return m_data.size(); }

    // ---- Reset path ----
    // The view's history breaks for @p reasons (HistoryReset bits), now, on this
    // thread: every history whose ResetsOn() names one of them gets
    // reasons & ResetsOn(), in registration order; then the view's own state (a world
    // clear drops the Hi-Z capture). One [VIEW_STATE] invalidate line, unless @p quiet
    // (the Sequencer's seeks, every scrubbed frame).
    void Invalidate(uint32_t reasons, bool quiet = false);

    // Render frame @p frame starts (render thread, before anything of the frame
    // records). @p reasons are the frame's breaks: those raised while the game half
    // built it, the render half's own, and CameraCut when the camera does not continue
    // the last frame (RenderCamera::historyValid == false); @p loud are the ones
    // raised with a log line (a cut is loud). Records them (FrameBreaks) and
    // invalidates them at once. A run of camera-cut frames logs its first frame and,
    // when it ends, one line with the count.
    void BeginFrame(uint64_t frame, uint32_t reasons, uint32_t loud);
    // The render frame's breaks (HistoryReset bits): what a pass reads where
    // it records - set by BeginFrame, before the graph forks, so any worker of
    // the frame may read it.
    uint32_t FrameBreaks() const { return m_frameBreaks; }

    // Releases every history's and every ViewData's GPU resources and forgets them
    // (the view going away, or the Renderer's shutdown after the GPU drained and the
    // passes shut down).
    void ReleaseHistories(IGraphicsDevice& gfx);

private:
    struct Entry
    {
        const void*                  owner;
        std::type_index              type;
        std::unique_ptr<ViewHistory> history;
    };
    struct DataEntry
    {
        const void*               owner;
        std::type_index           type;
        std::unique_ptr<ViewData> data;
    };

    void Add(const void* owner, std::type_index type, std::unique_ptr<ViewHistory> history);
    // Invalidate's work; @p frame (or -1) goes into the log line.
    void Apply(uint32_t reasons, bool quiet, int64_t frame);
    // The end of a run of cut frames: its count, when it lasted more than one.
    void EndCutRun();

public:
    // The view's sub-pixel jitter and its unjittered / jittered
    // view-projections, this frame's and the last (TAA, the upscalers, the
    // velocity fill, the RT / SSR reprojection). Advanced once a frame.
    TAAJitterState jitter;
    HiZCapture     hiz;

private:
    std::vector<Entry>     m_histories;
    std::vector<DataEntry> m_data;
    const char*            m_name;
    uint32_t               m_slot = 0;
    uint32_t           m_frameBreaks = 0;
    uint32_t           m_cutRun      = 0;   // consecutive render frames with a camera cut (BeginFrame)
    uint64_t           m_cutRunLast  = 0;   // the run's last frame
};
