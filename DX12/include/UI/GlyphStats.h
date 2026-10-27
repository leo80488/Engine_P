#pragma once

// Glyph baking totals: running totals since boot that frames.jsonl carries on
// every row (HarnessCaptureSystem) and Lua reads through Profiler.Glyphs(). A
// row where one moves baked glyphs on that frame; the difference between two
// reads is what happened between them.
//
// Totals, not per-frame counts, for the reason PipelineStats.h and
// UploadStats.h give: a -framelog_every=N run that skips rows loses nothing.
// Relaxed atomics: the main thread bakes, the TaskSystem's workers render
// cells, and every reader is on some other thread. Never reset.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace UI::GlyphStats
{
    // Cells whose place in the atlas was settled before their pixels were
    // rendered (reserve-first). 0 while every glyph is rendered first.
    inline std::atomic<uint64_t> reserved{ 0 };
    // Cells of real glyphs rendered - FreeType's SDF, or the per-glyph
    // 8SSEDT fallback - on any thread. Tofu boxes are counted apart.
    inline std::atomic<uint64_t> rendered{ 0 };
    // ...of which a TaskSystem worker rendered.
    inline std::atomic<uint64_t> renderedOffMain{ 0 };
    // Glyphs with nothing to draw (a space, a zero-area outline, FGMiraiRen's
    // empty outlines): an advance and no cell.
    inline std::atomic<uint64_t> empty{ 0 };
    // Tofu boxes: one per codepoint no font of the chain has.
    inline std::atomic<uint64_t> tofu{ 0 };
    // ...of which copied the font's first box instead of computing their own:
    // every box of a font is the same cell, so its 8SSEDT runs once a font.
    inline std::atomic<uint64_t> tofuReused{ 0 };
    // Wall time the main thread spent baking, counted once however the calls
    // nest (a prewarm's own glyphs, a fallback font's bake under EnsureGlyph):
    // loading outlines, rendering, packing and waiting for the workers.
    inline std::atomic<uint64_t> mainNs{ 0 };
    // Render time of the cells a worker rendered (the CPU the workers spent).
    inline std::atomic<uint64_t> workerNs{ 0 };
    // Render time of every rendered cell and tofu box, on any thread: the
    // work itself, which moving it between threads does not change.
    inline std::atomic<uint64_t> renderNs{ 0 };
    // Cells that became visible in a later frame than the one that asked for
    // them (BakeMode::Next). 0 while every bake is same-frame.
    inline std::atomic<uint64_t> late{ 0 };
    // Rendered cells whose geometry differed from what was reserved for them:
    // packed again elsewhere, so the atlas layout moved. Keep the name plural
    // wherever it is printed: "geomMismatch=0" contains "match=0", which
    // many packs forbid in their logs (their verify lines' failure).
    inline std::atomic<uint64_t> geomMismatches{ 0 };

    // The next-frame contract (BakeMode::Next). asyncCells: cells handed to
    // the Low-priority tasks that render them while the frame goes on.
    // joinCells: those the next flush found unclaimed and rendered itself (on
    // all workers) - the background tasks had not reached them. joinWaitNs:
    // the main thread's wall time at that flush waiting for cells a task WAS
    // rendering; joinWaitMaxNs the longest such wait, and renderMaxNs the
    // longest single cell's render anywhere - one join waits at most one cell
    // per task, so joinWaitMaxNs stays near renderMaxNs however loaded the
    // Low queue is. The two maxima are high-water marks, not totals.
    inline std::atomic<uint64_t> asyncCells{ 0 };
    inline std::atomic<uint64_t> joinCells{ 0 };
    inline std::atomic<uint64_t> joinWaitNs{ 0 };
    inline std::atomic<uint64_t> joinWaitMaxNs{ 0 };
    inline std::atomic<uint64_t> renderMaxNs{ 0 };

    inline void NoteMax(std::atomic<uint64_t>& hw, uint64_t v) noexcept
    {
        uint64_t cur = hw.load(std::memory_order_relaxed);
        while (v > cur && !hw.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    inline uint64_t ToNs(std::chrono::steady_clock::duration d) noexcept
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        return ns > 0 ? static_cast<uint64_t>(ns) : 0u;
    }

    inline double NsToMs(uint64_t ns) noexcept { return static_cast<double>(ns) / 1.0e6; }

    inline uint64_t Load(const std::atomic<uint64_t>& a) noexcept
    { return a.load(std::memory_order_relaxed); }
}
