#pragma once

// Renderer/RenderLifecycle.h - the renderer's lifecycle lists. The code that creates a
// pass or system says in one place what it does at shutdown, shader reload and world
// clear, and the Renderer runs the lists. A system writes its entry:
//
//     m_lifecycle.Add("TrailSystem", m_trailSystem.get())
//         .OnWorldClear([this](const RenderLifecycle::WorldClearArgs&)
//             { if (m_trailSystem) m_trailSystem->OnWorldClear(m_gfx); });
//
// A pass overrides RG::PassLifecycle's hooks and names them in HandledEvents();
// Renderer::PassSet::Register builds its entry:
//
//     m_taaPass = std::make_unique<TAAPass>();
//     m_taaPass->Init(m_gfx);
//     m_passes.Register("TAA", m_taaPass);   // reload; its history: the view
//
// Hard cuts and resizes are not events here: view histories break through
// ViewState::Invalidate (RenderGraph/ViewState.h), which also carries the world clear
// to them. WorldClear entries are for the world's own state.
//
// Written out by hand, because a list cannot say it:
//   * the main graph reloads after every entry, then the preview graph (GBufferPass#2
//     borrows the main library) - Renderer::ReloadShaders;
//   * shutdown runs after the GPU drain - ~Renderer;
//   * member subsystems whose OnWorldClear is ordered against the renderer's own
//     per-world state (mesh manager, skinning, DDGI scene, surface cache, the preview)
//     - Renderer::OnWorldClear;
//   * ServiceEnableGatedResources: its calls take arguments derived from the render
//     path and the settings. A pass that decides its own resources is a feature module.
//
// Order: an event runs its entries in registration (= creation) order. Each entry
// touches only its own object - a reload rebuilds its own library and PSOs (only graph
// passes borrow another's library), a world clear sets its object's flags or frees what
// it owns. The [LIFECYCLE] lines after Renderer::Compile print each event's order.
//
// Check: every RG::RenderPass that Renderer::Compile constructs must be in the main
// graph or accounted for here (an entry's object, or a pass an entry Owns()), or Compile
// fails an ENGINE_CHECK naming it (RG::LivePassesSince). Standalone passes that are not
// RenderPasses (CAS, SSR, the UI passes, ...) are not counted: register them the same way.
//
// Threading: entries are added in Compile's serial window and never after. Events run
// in serial windows (the destructor, ReloadShaders, OnWorldClear), where the lists are
// only read.

#include "RenderGraph/PassLifecycle.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace RG { class RenderPass; }

class RenderLifecycle
{
public:
    enum class Event : uint8_t
    {
        Shutdown,       // ~Renderer, after the GPU drain: release what the object created
        ReloadShaders,  // Renderer::ReloadShaders, before the main graph's own walk
        WorldClear,     // Renderer::OnWorldClear (RunWorldClear: it takes arguments)
        Count
    };

    // The events before WorldClear take no arguments.
    static constexpr size_t kPlainEvents = static_cast<size_t>(Event::WorldClear);

    // Renderer::OnWorldClear's arguments, as it received them (the passes'
    // OnWorldClear takes the same struct, RG::PassLifecycle).
    using WorldClearArgs = RG::WorldClearArgs;

    class Entry
    {
    public:
        Entry& OnShutdown(std::function<void()> fn)      { return Set(Event::Shutdown, std::move(fn)); }
        Entry& OnReloadShaders(std::function<void()> fn) { return Set(Event::ReloadShaders, std::move(fn)); }
        Entry& OnWorldClear(std::function<void(const WorldClearArgs&)> fn)
        {
            m_worldClear = std::move(fn);
            return *this;
        }
        // A RenderPass the entry's object creates and drives itself (the
        // skinning subsystem's SkinningPass): accounted for by the census.
        Entry& Owns(const RG::RenderPass* pass);

    private:
        friend class RenderLifecycle;
        Entry& Set(Event e, std::function<void()> fn);

        std::string m_name;
        const void* m_object = nullptr;
        std::function<void()> m_plain[kPlainEvents];
        std::function<void(const WorldClearArgs&)> m_worldClear;
        std::vector<const RG::RenderPass*> m_owned;
    };

    static const char* EventName(Event e);

    // @p object identifies the entry: the pass or system itself, so the census
    // can find it. @p name is what the [LIFECYCLE] lines print.
    Entry& Add(const char* name, const void* object);

    void Run(Event e);   // every event but WorldClear
    void RunWorldClear(const WorldClearArgs& args);

    // The object of an entry, or a pass an entry Owns().
    bool Accounts(const void* object) const;
    size_t Count(Event e) const;
    size_t Size() const { return m_entries.size(); }

    // One [LIFECYCLE] line per event: its entries, in the order they run.
    void LogOrder() const;

private:
    bool Has(const Entry& entry, Event e) const;

    std::deque<Entry> m_entries;   // a deque: Add's reference stays valid through the next Add
    std::deque<Entry> m_dropped;   // -lifecycle_drop (harness): built, never run
};
