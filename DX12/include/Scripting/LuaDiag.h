#pragma once

// LuaDiag: de-duplicated diagnostics for the Lua binding layer.
// A binding that fails silently looks like a broken engine to a designer holding only a
// Lua script, but bindings run from OnUpdate: logging every failure floods 60 lines/s and
// pushes the first, most useful message out of the logger's record ring
// (Logger::kRingCapacity) within minutes. So each site warns once.
// Usage: one static site string per failure site, e.g.
//   static constexpr char kSite[] = "Engine.SetMaterialParam/no-material";
// passed to WarnOnce / Fail. The key is compared by address (a pointer hash), and the
// suppressed path, the common one, formats no string.
// Lifetime: ScriptSystem::ClearAll / ClearInstances call ResetOnce(), so each Play session
// warns once per site again.
// Threading: not synchronised. Every Lua-entering system is Opaque to the Scheduler (see
// ScriptSystem.cpp), so this runs on the calling thread; add a lock if that changes.

#include "System/Log.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace LuaDiag
{
    // Returns true the FIRST time it is called for `siteKey`, false after.
    // Exposed for callers that want to gate something other than a log line.
    bool ShouldReport(const void* siteKey);

    // Content-keyed variant, for a "site" that is a runtime name rather than a code location
    // (a BT action name, a material slot name). Costs a string hash and keeps a copy of every
    // key: use it only when the key set is genuinely dynamic.
    bool ShouldReportNamed(const std::string& key);

    // Clear all site memory (new Play session). Called by ScriptSystem.
    void ResetOnce();

    // Log a warning at most once per site.
    inline void WarnOnce(const void* siteKey, const char* fmt, ...)
    {
        if (!ShouldReport(siteKey)) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LOG_WARNING("%s", buf);
    }

    // Content-keyed WarnOnce (see ShouldReportNamed).
    inline void WarnOnceNamed(const std::string& key, const char* fmt, ...)
    {
        if (!ShouldReportNamed(key)) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LOG_WARNING("%s", buf);
    }

    // WarnOnce + return a sentinel, so a guard stays a one-liner:
    //     if (!mc) return LuaDiag::Fail(kSite, false, "...", e);
    template <typename T>
    inline T Fail(const void* siteKey, T sentinel, const char* fmt, ...)
    {
        if (ShouldReport(siteKey))
        {
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            LOG_WARNING("%s", buf);
        }
        return sentinel;
    }
}
