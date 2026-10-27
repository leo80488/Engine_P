#include "Renderer/RenderLifecycle.h"

#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>

const char* RenderLifecycle::EventName(Event e)
{
    switch (e)
    {
    case Event::Shutdown:      return "shutdown";
    case Event::ReloadShaders: return "reload";
    case Event::WorldClear:    return "worldclear";
    default:                   return "?";
    }
}

RenderLifecycle::Entry& RenderLifecycle::Entry::Set(Event e, std::function<void()> fn)
{
    const size_t i = static_cast<size_t>(e);
    if (ENGINE_CHECK(i < kPlainEvents, "RenderLifecycle: event %s takes arguments - use OnWorldClear", EventName(e)))
        m_plain[i] = std::move(fn);
    return *this;
}

RenderLifecycle::Entry& RenderLifecycle::Entry::Owns(const RG::RenderPass* pass)
{
    if (pass) m_owned.push_back(pass);
    return *this;
}

RenderLifecycle::Entry& RenderLifecycle::Add(const char* name, const void* object)
{
#ifdef WITH_HARNESS
    // -lifecycle_drop=<name> (the lifecycle_census pack's reverse arm): the
    // entry is built but never listed - its events do not run and the census
    // does not count it - which is exactly a registration someone forgot.
    static const std::string s_drop = Sys::CommandLine::Get().Get("-lifecycle_drop");
    if (!s_drop.empty() && name && s_drop == name)
    {
        LOG_WARNING("[LIFECYCLE] -lifecycle_drop: '%s' is left out of the lists", name);
        return m_dropped.emplace_back();
    }
#endif
    // One entry per object: a second would run each of its events twice.
    ENGINE_CHECK(!object || !Accounts(object), "RenderLifecycle: '%s' is registered twice", name ? name : "?");
    Entry& e = m_entries.emplace_back();
    e.m_name   = name ? name : "?";
    e.m_object = object;
    return e;
}

bool RenderLifecycle::Has(const Entry& entry, Event e) const
{
    if (e == Event::WorldClear) return static_cast<bool>(entry.m_worldClear);
    const size_t i = static_cast<size_t>(e);
    return i < kPlainEvents && static_cast<bool>(entry.m_plain[i]);
}

void RenderLifecycle::Run(Event e)
{
    const size_t i = static_cast<size_t>(e);
    if (!ENGINE_CHECK(i < kPlainEvents, "RenderLifecycle::Run(%s): use RunWorldClear", EventName(e)))
        return;
    for (Entry& entry : m_entries)
        if (entry.m_plain[i]) entry.m_plain[i]();
}

void RenderLifecycle::RunWorldClear(const WorldClearArgs& args)
{
    for (Entry& entry : m_entries)
        if (entry.m_worldClear) entry.m_worldClear(args);
}

bool RenderLifecycle::Accounts(const void* object) const
{
    if (!object) return false;
    for (const Entry& entry : m_entries)
    {
        if (entry.m_object == object) return true;
        if (std::find(entry.m_owned.begin(), entry.m_owned.end(), object) != entry.m_owned.end()) return true;
    }
    return false;
}

size_t RenderLifecycle::Count(Event e) const
{
    size_t n = 0;
    for (const Entry& entry : m_entries)
        n += Has(entry, e) ? 1u : 0u;
    return n;
}

void RenderLifecycle::LogOrder() const
{
    // The logger formats into 1 KiB, so a long list continues on another line.
    constexpr size_t kLineBudget = 760;
    for (size_t i = 0; i < static_cast<size_t>(Event::Count); ++i)
    {
        const Event e = static_cast<Event>(i);
        const size_t count = Count(e);
        std::string names;
        bool continued = false;
        for (const Entry& entry : m_entries)
        {
            if (!Has(entry, e)) continue;
            if (!names.empty() && names.size() + entry.m_name.size() + 1 > kLineBudget)
            {
                LOG_INFO("[LIFECYCLE] %s %zu%s:%s", EventName(e), count, continued ? " (cont.)" : "", names.c_str());
                names.clear();
                continued = true;
            }
            names += ' ';
            names += entry.m_name;
        }
        LOG_INFO("[LIFECYCLE] %s %zu%s:%s", EventName(e), count, continued ? " (cont.)" : "",
                 names.empty() ? " (none)" : names.c_str());
    }
}
