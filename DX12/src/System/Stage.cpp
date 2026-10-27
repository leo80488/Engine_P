#include "System/Stage.h"

#include "System/BootTimeline.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace Sys::Stage
{
    namespace
    {
        struct Entry
        {
            std::uint64_t id = 0;
            const char*   name = "?";
            std::string   detail;
            const char*   what = nullptr;
            double        sinceMs = 0.0;
            double        lastProgressMs = 0.0;
            std::uint64_t done = 0, total = 0;
        };

        struct State
        {
            std::mutex         mutex;
            std::vector<Entry> stack;
            std::uint64_t      nextId = 1;
        };

        State& S()
        {
            static State s_instance;
            return s_instance;
        }
    }

    // A Scope removes the entry IT pushed (by id), not simply the top: should
    // a loader ever run on another thread, interleaved scopes still unwind to
    // the right state.
    Scope::Scope(const char* name, std::string detail)
    {
        const double now = Sys::Boot::NowMs();
        State& s = S();
        std::lock_guard<std::mutex> lk(s.mutex);
        m_id = s.nextId++;
        Entry e;
        e.id      = m_id;
        e.name    = name ? name : "?";
        e.detail  = std::move(detail);
        e.sinceMs = now;
        s.stack.push_back(std::move(e));
    }

    Scope::~Scope()
    {
        State& s = S();
        std::lock_guard<std::mutex> lk(s.mutex);
        const auto it = std::find_if(s.stack.begin(), s.stack.end(),
                                     [this](const Entry& e) { return e.id == m_id; });
        if (it != s.stack.end()) s.stack.erase(it);
    }

    void Progress(std::uint64_t done, std::uint64_t total, const char* what)
    {
        const double now = Sys::Boot::NowMs();
        State& s = S();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (s.stack.empty()) return;
        Entry& e = s.stack.back();
        e.done           = done;
        e.total          = total;
        e.what           = what;
        e.lastProgressMs = now;
    }

    Snapshot Current()
    {
        Snapshot out;
        State& s = S();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (s.stack.empty()) return out;
        const Entry& e = s.stack.back();
        out.active         = true;
        out.name           = e.name;
        out.detail         = e.detail;
        out.what           = e.what ? e.what : "";
        out.sinceMs        = e.sinceMs;
        out.lastProgressMs = e.lastProgressMs;
        out.done           = e.done;
        out.total          = e.total;
        out.depth          = static_cast<std::uint32_t>(s.stack.size());
        return out;
    }
}
