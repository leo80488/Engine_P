#pragma once

// ViewBudget - what the secondary views may cost in video memory, against the OS's budget
// (Graphics/VideoMemory.h).
//
// Admission: a view is made only when the process's usage plus the view's estimated cost stays under 90% of the
// budget. Ladder: when usage passes 95% of the budget with views alive, the views render at half size (stage 1),
// then are suspended - their resources, and the heaps they shared, given back (stage 2) - one step per 60
// frames; they come back, one step at a time, after usage has stayed well under the budget for 300 frames.

#include "Graphics/VideoMemory.h"

#include <cstdint>

namespace ViewBudget
{
    enum class Stage : uint8_t { Normal = 0, HalfSize = 1, Suspended = 2 };

    // May a view of @p width x @p height (0: 1920 x 1080) be made now? @p readyViews: the secondary views whose
    // resources exist (the first one also grows the shared alias heaps). False refuses it and logs why.
    bool Admit(uint32_t width, uint32_t height, const VideoMemory::Info& vm, uint32_t readyViews);

    // Once per frame boundary, main thread: moves the ladder from the last sample. @p readyViews as above.
    Stage Step(const VideoMemory::Info& vm, uint32_t readyViews);

    // The ladder's stage (read from any thread).
    Stage Current();

    // A requested view dimension at the current stage: halved from stage 1 on.
    uint32_t Scaled(uint32_t pixels);

    uint32_t Refusals();   // views Admit refused since boot
}
