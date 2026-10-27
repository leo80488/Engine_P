#pragma once
// Scripting/ScriptVM.h - the Lua VM's thread contract, made checkable. Header-only.
// There is ONE sol::state (ScriptSystem::Initialize), shared by AI, GameFlow and every binding.
// A thread may enter the VM iff it is the VM's OWNER (the thread that created it - main) or it
// holds a Grant: the Scheduler issues one around a system that declared ExclusiveRes::kScriptVM
// while it runs on a worker, like the EventBus grant for kEventBus. kScriptVM is exclusive, so the
// planner runs such systems one at a time.
// -luavmcheck installs a LUA_MASKCALL hook that checks this on every entry into a Lua function,
// from any thread, coroutines included (lua_newthread inherits the hook). Too slow to ship: it
// is the harness tripwire.

#include <atomic>
#include <cstdint>
#include <thread>

namespace Sys::ScriptVM
{
    inline std::atomic<std::thread::id>& OwnerSlot()
    {
        static std::atomic<std::thread::id> s_owner{};
        return s_owner;
    }
    inline void SetOwner(std::thread::id id) { OwnerSlot().store(id, std::memory_order_release); }

    inline int& GrantDepth()
    {
        static thread_local int t_depth = 0;
        return t_depth;
    }

    // RAII: this thread may enter the VM while the grant lives (Scheduler).
    struct Grant
    {
        Grant() { ++GrantDepth(); }
        ~Grant() { --GrantDepth(); }
        Grant(const Grant&)            = delete;
        Grant& operator=(const Grant&) = delete;
    };

    inline bool HeldByThisThread()
    {
        return GrantDepth() > 0 ||
               std::this_thread::get_id() == OwnerSlot().load(std::memory_order_acquire);
    }

    inline std::atomic<uint64_t>& Violations()
    {
        static std::atomic<uint64_t> s_count{0};
        return s_count;
    }
    inline std::atomic<uint64_t>& Checks()
    {
        static std::atomic<uint64_t> s_count{0};
        return s_count;
    }
}
