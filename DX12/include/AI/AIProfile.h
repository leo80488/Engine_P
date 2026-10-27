#pragma once

// AIProfile - one frame's AI-phase cost and query counts, published by the AI tick systems and
// read by the AI benchmark.
// Why not Tracy or frames.jsonl: the benchmark reports p50/p95/p99 and the worst frame of the AI
// phase alone. Whole-frame cpuMs buries that under render and animation noise, and a .tracy
// capture per configuration is too heavy for a mode x population sweep; this is ~200 bytes per frame.
// Wall vs CPU: wall is how long the main thread was blocked, cpu the work summed over every
// participating thread. cpu / wall is the achieved speedup; cpu - wall is the work moved off the
// main thread.
// Not thread-safe by design: fields are written by the AI phase on the main thread (the parallel
// section accumulates into its own atomic and publishes the total afterwards) and read between frames.

#include <cstdint>

namespace AI
{
    struct AIProfile
    {
        // ---- Per-phase timings (milliseconds) ----
        float perceptionWallMs = 0.f;
        float perceptionCpuMs  = 0.f;   // summed across workers when parallel
        float btWallMs         = 0.f;
        float navWallMs        = 0.f;
        float motorWallMs      = 0.f;
        float lodWallMs        = 0.f;

        // ---- Counts ----
        std::uint32_t aiAgents        = 0;   // entities carrying AIComponent
        std::uint32_t btTicks         = 0;   // trees actually evaluated this frame
        std::uint32_t perceptionAgents = 0;
        std::uint32_t perceptionTicked = 0;
        std::uint32_t perceptionCandidates = 0;
        std::uint32_t perceptionRays  = 0;
        std::uint32_t navAgents       = 0;
        std::uint32_t pathQueries     = 0;
        std::uint32_t pathFailures    = 0;

        float TotalMs() const
        {
            return perceptionWallMs + btWallMs + navWallMs + motorWallMs + lodWallMs;
        }

        // Process-global sink: a measurement side channel that no decision reads, so a singleton rather
        // than a member threaded through five tick systems.
        static AIProfile& Get()
        {
            static AIProfile s_instance;
            return s_instance;
        }
    };
}
