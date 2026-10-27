#pragma once
// System/Check.h - runtime invariant checks that survive Release.
//
// Why not assert(): it compiles to nothing under NDEBUG, and every build that actually
// runs (the harness's, a player's) is Release, so an assert checks only where it is
// least needed - and its failure mode is silence. RenderGraph's RG_VALIDATE_ACCESS is
// compiled in all configurations for the same reason. assert() also aborts, which is
// wrong for a renderer / editor: a null mesh or an out-of-range slot should log loudly
// and let the frame continue degraded, not kill the process and lose an unsaved scene.
// So a check returns its result and the call site decides.
//
// Usage:
//   if (!ENGINE_CHECK(mesh != nullptr, "null mesh for entity %u", e))
//       return;                                  // recover, do not crash
//
//   const bool ok = ENGINE_CHECK(slot < kMaxSlots, "slot %u >= %u", slot, kMaxSlots);
//
//   // Per-frame / per-draw code: report once per call site, never spam.
//   if (!ENGINE_CHECK_ONCE(tex.IsValid(), "material %s missing albedo", name))
//       tex = fallbackWhite;
//
// Which one:
//   ENGINE_CHECK       cold paths (load, init, resize, serialization, editor commands):
//                      a log line per failure is what you want.
//   ENGINE_CHECK_ONCE  hot paths (any per-frame or per-object loop): a broken invariant
//                      would otherwise write thousands of identical lines per second and
//                      push the real cause out of the log.
//   static_assert      whenever the condition is known at compile time; it costs nothing.
//
// Cost when the condition holds: the branch only; the message is formatted on failure.
// Neither macro is gated on NDEBUG or _DEBUG: do not move one back behind a config check.

#include "System/Log.h"

#include <cstdarg>
#include <cstdio>

namespace EngineCheck
{
    // Out-of-line-ish reporter: Logger::Log is variadic and C varargs cannot be
    // forwarded, so the message is formatted here and handed over as "%s". This
    // also keeps the failure path off the caller's hot instruction cache.
    inline void ReportFailure(const char* file, int line, const char* cond,
                              const char* format, ...)
    {
        char msg[512];
        va_list args;
        va_start(args, format);
        vsnprintf(msg, sizeof(msg), format, args);
        va_end(args);

        Logger::Get().Log(LogLevel::Error, file, line,
                          "CHECK FAILED: (%s) - %s", cond, msg);
    }
}

// Evaluates to true when @p cond holds. When it does not: logs an ERROR line
// carrying the source location, the literal condition text, and the formatted
// message, then evaluates to false. Usable anywhere a bool expression is.
#define ENGINE_CHECK(cond, format, ...)                                        \
    ((cond) ? true                                                             \
            : (::EngineCheck::ReportFailure(__FILE__, __LINE__, #cond,         \
                                            (format), ##__VA_ARGS__),          \
               false))

// Same, but each CALL SITE reports at most once per process run. The lambda
// gives every expansion its own closure type, hence its own static flag.
// The flag is constant-initialised, so there is no thread-safe-static guard on
// the fast path; a simultaneous first failure on two threads may log twice,
// which is harmless and cheaper than an atomic.
#define ENGINE_CHECK_ONCE(cond, format, ...)                                   \
    ((cond) ? true                                                             \
            : ([&]() -> bool {                                                 \
                  static bool s_reported = false;                              \
                  if (!s_reported)                                             \
                  {                                                            \
                      s_reported = true;                                       \
                      ::EngineCheck::ReportFailure(__FILE__, __LINE__, #cond,  \
                                                   (format), ##__VA_ARGS__);   \
                  }                                                            \
                  return false;                                                \
              }()))
