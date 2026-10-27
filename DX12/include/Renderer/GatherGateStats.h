#pragma once

// Which frames the retained scene caches fell back to a FULL gather, and why.
// Two gates decide it every frame, both on main inside Renderer::BeginFrame: the
// raster candidate cache's patch gate (Renderer::BuildRenderScene, `usePatch`)
// and the DDGI scene-AS retained static gather's (DDGISceneAS::Gather). A full
// gather re-walks the whole scene (11-18 ms on an AIBench spawn frame against
// 0.9 ms patched), so a spawn measurement asks of every row how many frames
// went full and for which reason.
//
// Read by the harness's per-frame record (frames.jsonl gatherFull /
// gatherFullReason / gatherFulls, ddgiFull / ddgiFullReason / ddgiFulls) and
// Lua Profiler.Spawn().
//
// Written and read on main (the gates run in BeginFrame; the harness record is
// an opaque PostRender system, i.e. serial on main). Relaxed atomics anyway, so
// a pipe-thread reader can never tear a row.

#include <atomic>
#include <cstdint>

namespace GatherGateStats
{
    // One bit per term that vetoed the patch. A full frame may carry several.
    enum Reason : uint32_t
    {
        kAdded        = 1u << 0,    // entities entered the scene diff
        kRemoved      = 1u << 1,    // entities left it
        kOther        = 1u << 2,    // dirtyOther: transform / visibility / mesh changes that are not material
        kMat          = 1u << 3,    // matDirty: material changes
        kTexUnsettled = 1u << 4,    // a material resource's textures are still arriving (or resolved this frame)
        kUnresolved   = 1u << 5,    // the previous gather left geometry unresolved
        kSetSig       = 1u << 6,    // raster: the candidate set signature moved
        kPick         = 1u << 7,    // raster: the picking-outline entity changed
        kContinuity   = 1u << 8,    // the dirty feed skipped a frame (or is not valid)
        kInvalid      = 1u << 9,    // the cache was not armed (first frame, a world clear, a full gather that failed)
        kDissolve     = 1u << 10,   // raster: a dissolve subtree is live (per-frame by contract)
        kSurfaceCache = 1u << 11,   // DDGI: the surface-cache epoch moved under -ddgibindfused
        kOff          = 1u << 12,   // the retained path is off for this frame (flags, -candretainab)
    };

    struct Gate
    {
        std::atomic<uint64_t> frame{ 0 };    // the frame the gate last ran in
        std::atomic<uint32_t> full{ 0 };     // 1 = that frame gathered the whole scene
        std::atomic<uint32_t> reason{ 0 };   // Reason bits; 0 on a patch frame
        std::atomic<uint64_t> fulls{ 0 };    // running total of full frames since boot
        std::atomic<uint64_t> runs{ 0 };     // running total of frames the gate ran
        // Raster only: Renderer::BuildRenderScene's wall time (buildUs, microseconds)
        // on buildFrame - the whole scene build, the gate's patch or full gather
        // included.
        std::atomic<uint64_t> buildFrame{ ~0ull };
        std::atomic<uint32_t> buildUs{ 0 };
    };

    inline Gate cand;   // the raster candidate cache (Renderer::BuildRenderScene)
    inline Gate ddgi;   // the DDGI scene-AS static gather (DDGISceneAS::Gather)

    inline void Note(Gate& g, uint64_t frame, bool full, uint32_t reason) noexcept
    {
        g.frame.store(frame, std::memory_order_relaxed);
        g.full.store(full ? 1u : 0u, std::memory_order_relaxed);
        g.reason.store(full ? reason : 0u, std::memory_order_relaxed);
        if (full) g.fulls.fetch_add(1, std::memory_order_relaxed);
        g.runs.fetch_add(1, std::memory_order_relaxed);
    }
}
