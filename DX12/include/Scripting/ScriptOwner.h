#pragma once

// Who a callback runs as, for C++ code that starts one on a script's behalf. A script's
// subscriptions, timers and requests belong to the owner whose Lua made them and go with it
// (ScriptSystem); a callback C++ starts later - an asynchronous spawn's onReady - would otherwise
// run as nobody, outlive its requester and leave what it creates owner-less for good.
//
//   const auto owner = Scripting::CurrentOwner();      // while the requesting Lua runs
//   ...later, from C++...
//   Scripting::RunAsOwner(owner, [] { /* calls into Lua */ });
//
// RunAsOwner runs the callback at the next real-time script tick, as that owner; when the owner
// has ended by then the callback is dropped (logged). Main thread only.

#include <cstdint>
#include <functional>

namespace Scripting
{
    using OwnerToken = std::uint64_t;   // 0 = no owner

    // The owner whose Lua is running right now; 0 outside any script.
    OwnerToken CurrentOwner();

    void RunAsOwner(OwnerToken owner, std::function<void()> fn);

    // ScriptSystem installs itself here (and removes itself with nullptrs).
    void SetOwnerBackend(std::function<OwnerToken()> current,
                         std::function<void(OwnerToken, std::function<void()>)> run);

    // True under -timers_legacy (harness builds only): game-domain timers count real time,
    // C++-started callbacks run as no owner and ScriptTimerSystem sits in GameplayScripts again.
    // The lua_timers pack's reverse arm.
    bool LegacyTimers();
}
