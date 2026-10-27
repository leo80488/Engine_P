#pragma once
// System/BootTimeline.h - where startup time goes, in milliseconds.
//
// Every scenario pack pays a full engine boot (boot + scene load + shutdown is about
// 60% of a pack's process time), and the log only resolves seconds. This records named
// marks as milliseconds since the process was created (GetProcessTimes), so the first
// phase includes the loader and static initialisation; run.json publishes them as
// `bootPhasesMs`.
//
// Tiny and dependency-free (no logger, no allocation after the first call): marks are
// taken before the logger exists and on the shutdown path after most systems are gone.
// The first mark of a name wins, so a mark in a function that can run twice records the
// first time.
//
// Threading: marks are taken on the main thread in practice; the fixed array is
// published through an atomic count, so a concurrent reader (RunLedger at shutdown, the
// [BOOT] log line) sees only complete entries.

#include <cstddef>

namespace Sys::Boot
{
    struct Phase
    {
        const char* name;   // a string literal
        double      ms;     // since process creation
    };

    // Record `name` now. A name already recorded is ignored.
    void Mark(const char* name);

    // Called with each new mark's name, on the marking thread, right after it is recorded (the boot
    // splash's bar). One listener; nullptr removes it. It must not block or mark.
    using MarkListener = void (*)(const char* name);
    void SetMarkListener(MarkListener listener);
    // More listeners beside the splash's (the main window shows itself at "firstFrame"); false when all
    // slots are taken. Remove one before the state it reads goes away.
    bool AddMarkListener(MarkListener listener);
    void RemoveMarkListener(MarkListener listener);

    // Milliseconds since process creation.
    double NowMs();

    // Copy up to @p max recorded phases, in the order they were marked.
    std::size_t Snapshot(Phase* out, std::size_t max);

    constexpr std::size_t kMaxPhases = 32;
}
