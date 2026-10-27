#pragma once

// Renderer/PassSet.h - the Renderer's passes, in the render graph or out of it,
// and the hooks it runs on them.
//
// Register adds a pass's lifecycle entry (Renderer/RenderLifecycle.h) from the
// events the pass handles (RG::PassLifecycle::HandledEvents), each calling the
// pass's own override. Their order is registration order, i.e. where
// Renderer::Compile creates the pass; the entry's object is the pass itself, so
// the lifecycle census finds it.
//
// The prepare hooks' order is one table per stage, written in one place
// (Renderer::RegisterPrepareOrder), not registration order: allocation order
// changes pictures (a texture created and never used moved 4% of AIBench's
// pixels by 1-2 LSB), so a reorder must be a visible edit. The service hooks'
// order is likewise one table per stage (Renderer::RegisterServiceOrder).
//
// Register also lists a pass that names kView (it keeps a history in the view,
// RenderGraph/ViewState.h) and hands it the view; registration order is the
// order the histories are created and logged. The view is set before the first
// registration (SetView, the top of Renderer::Compile) and each pass is bound AS
// IT REGISTERS: RTReflectionPass makes its history in its boot OnCompile (the
// main graph's first Compile), long before Compile ends. LogViews prints the
// census once everything registered. A hard cut and a resize reach the
// histories through the view (ViewState::Invalidate), not through the lifecycle
// lists.

#include "Renderer/RenderLifecycle.h"
#include "RenderGraph/PassLifecycle.h"
#include "Graphics/ViewSlot.h"   // a secondary view's segment (Prepare)
#include "System/Check.h"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

class IGraphicsDevice;
class ViewState;

class PassSet
{
public:
    struct Named
    {
        const char*        name = nullptr;
        RG::PassLifecycle* pass = nullptr;
    };

    PassSet(RenderLifecycle& lifecycle, IGraphicsDevice& gfx) : m_lifecycle(lifecycle), m_gfx(gfx) {}
    PassSet(const PassSet&) = delete;
    PassSet& operator=(const PassSet&) = delete;

    // A pass the Renderer holds by pointer for as long as the Renderer lives (a
    // graph pass, a member of a member).
    template <class T>
    RenderLifecycle::Entry& Register(const char* name, T* pass)
    {
        ENGINE_CHECK(pass != nullptr, "PassSet: '%s' is registered without its pass", name ? name : "?");
        RenderLifecycle::Entry& e = m_lifecycle.Add(name, pass);
        RG::PassLifecycle* p = pass;
        IGraphicsDevice& gfx = m_gfx;
        const uint32_t events = p ? p->HandledEvents() : 0u;
        using L = RG::PassLifecycle;
        if (events & L::kShutdown)      e.OnShutdown([p, &gfx] { p->Shutdown(gfx); });
        if (events & L::kReloadShaders) e.OnReloadShaders([p, &gfx] { p->ReloadShaders(gfx); });
        if (events & L::kWorldClear)    e.OnWorldClear([p](const RG::WorldClearArgs& a) { p->OnWorldClear(a); });
        if (events & L::kView)          AddView(name, [p] { return p; });
        return e;
    }

    // A pass held by a unique_ptr member: each hook reads the member when it
    // runs, so a pass reset after registration is skipped.
    template <class T>
    RenderLifecycle::Entry& Register(const char* name, std::unique_ptr<T>& owner)
    {
        RenderLifecycle::Entry& e = m_lifecycle.Add(name, owner.get());
        IGraphicsDevice& gfx = m_gfx;
        const uint32_t events = owner ? static_cast<RG::PassLifecycle&>(*owner).HandledEvents() : 0u;
        using L = RG::PassLifecycle;
        auto pass = [&owner]() -> RG::PassLifecycle* { return owner.get(); };
        if (events & L::kShutdown)      e.OnShutdown([pass, &gfx] { if (auto* p = pass()) p->Shutdown(gfx); });
        if (events & L::kReloadShaders) e.OnReloadShaders([pass, &gfx] { if (auto* p = pass()) p->ReloadShaders(gfx); });
        if (events & L::kWorldClear)
            e.OnWorldClear([pass](const RG::WorldClearArgs& a) { if (auto* p = pass()) p->OnWorldClear(a); });
        if (events & L::kView)          AddView(name, pass);
        return e;
    }

    // ---- the view ----
    // A pass that keeps a history in the view but handles no lifecycle event - a
    // main graph pass: listed for BindView only, no lifecycle entry (the graph owns
    // it). Registered where it is created, like Register; it must name kView.
    void RegisterView(const char* name, RG::PassLifecycle* pass)
    {
        ENGINE_CHECK(pass != nullptr, "PassSet: '%s' is registered for the view without its pass", name ? name : "?");
        if (!pass) return;
        ENGINE_CHECK(pass->HandledEvents() & RG::PassLifecycle::kView,
                     "PassSet: '%s' is registered for the view but does not name kView", name ? name : "?");
        AddView(name, [pass] { return pass; });
    }
    // The view the passes that name kView are bound to as they register: set
    // before the first registration - the main view.
    void SetView(ViewState& view) { m_view = m_bound = &view; }
    // Binds every pass that names kView to @p view (RG::PassLifecycle::BindView):
    // before a view other than the bound one records, and back to the main view
    // after it. Serial, outside any recording.
    void BindView(ViewState& view);
    ViewState* BoundView() const { return m_bound; }
    // One [VIEW_STATE] line: the passes bound and the view's history count
    // (the end of Renderer::Compile). Main thread.
    void LogViews() const;

    // ---- prepare ----
    // Stage @p stage runs these passes in this order (a null pass - a feature
    // this build or device does not create - is left out).
    void SetPrepareOrder(RG::PrepareStage stage, std::initializer_list<Named> passes);
    // In a secondary view's segment only the passes that record any view (kViewSafe) prepare: the
    // others keep the main view's resources, which another size would rebuild every frame.
    void Prepare(RG::PrepareStage stage, RG::PrepareContext ctx)
    {
        ctx.stage = stage;
        const bool secondary = RHI::CurrentRecordingView().slot != 0;
        for (const Named& n : m_prepare[static_cast<size_t>(stage)])
            if (!secondary || (n.pass->HandledEvents() & RG::PassLifecycle::kViewSafe))
                n.pass->OnPrepare(ctx);
    }
    // One [PASSES] line per stage: its passes, in the order they run.
    void LogPrepareOrder() const;

    // ---- service ----
    void SetServiceOrder(RG::ServiceStage stage, std::initializer_list<Named> passes);
    void Service(RG::ServiceStage stage, RG::ServiceContext& ctx)
    {
        ctx.stage = stage;
        for (const Named& n : m_service[static_cast<size_t>(stage)])
            n.pass->OnService(ctx);
    }
    void LogServiceOrder() const;

private:
    // A pass that keeps a history in the view (a unique_ptr member reset after
    // registration reads null and is left out of the census).
    struct ViewPass
    {
        const char*                         name = nullptr;
        std::function<RG::PassLifecycle*()> pass;
    };

    // Lists the pass and binds it to the view now (its BindView creates its
    // history object).
    void AddView(const char* name, std::function<RG::PassLifecycle*()> pass);

    RenderLifecycle& m_lifecycle;
    IGraphicsDevice& m_gfx;
    std::array<std::vector<Named>, static_cast<size_t>(RG::PrepareStage::Count)> m_prepare;
    std::array<std::vector<Named>, static_cast<size_t>(RG::ServiceStage::Count)> m_service;
    std::vector<ViewPass> m_views;
    ViewState*            m_view  = nullptr;   // the main view (SetView)
    ViewState*            m_bound = nullptr;   // the view the passes are bound to now
};
