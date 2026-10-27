// PreviewFrozenSubmit.cpp - see the header for why this function DRAWS the frozen
// picture rather than handing a rect back to the caller.

#include "Editor/PreviewFrozenSubmit.h"

#include "System/Log.h"

#include <cstdint>
#include <vector>

namespace Editor
{
namespace
{
    // The log-on-change latch. One record per (panel, view), and the panel is
    // identified by the prefix POINTER because both call sites pass a string
    // literal with static storage - the values are compared, never the string.
    struct FrozenRecord
    {
        const char*   prefix = nullptr;
        std::uint32_t viewId = 0;
        std::uint32_t outW = 0, outH = 0;
        std::uint32_t cellW = 0, cellH = 0;
        std::uint32_t laneW = 0, laneH = 0;
        bool          logged = false;
    };

    // A VECTOR, not an array indexed by viewId: the camera panel and a split cell can hold
    // the SAME view id at different times and their tuples must not overwrite each other.
    // At most kMaxPreviews x 2 entries exist and this runs once per frame per suspended
    // cell, so a linear scan is the cheapest correct thing.
    std::vector<FrozenRecord> g_frozen;
}

void SubmitFrozenImage(const char* prefix, std::uint32_t viewId,
                       ImTextureRef tex,
                       std::uint32_t outW, std::uint32_t outH,
                       std::uint32_t cellW, std::uint32_t cellH,
                       std::uint32_t laneW, std::uint32_t laneH)
{
    // THE SUBMISSION, and the ONLY one. Submitting the cell's own size is the regression
    // the log line catches; it cannot happen without editing this expression (no call-site
    // argument to get wrong).
    const ImVec2 submit(static_cast<float>(outW), static_cast<float>(outH));
    ImGui::ImageWithBg(tex, submit,
                       ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                       ImVec4(0.f, 0.f, 0.f, 0.f),
                       kFrozenTint);

    FrozenRecord* rec = nullptr;
    for (FrozenRecord& r : g_frozen)
        if (r.prefix == prefix && r.viewId == viewId) { rec = &r; break; }
    if (!rec)
    {
        g_frozen.push_back(FrozenRecord{ prefix, viewId, 0, 0, 0, 0, 0, 0, false });
        rec = &g_frozen.back();
    }

    const bool changed = !rec->logged   ||
                         rec->outW  != outW  || rec->outH  != outH ||
                         rec->cellW != cellW || rec->cellH != cellH ||
                         rec->laneW != laneW || rec->laneH != laneH;
    if (changed)
    {
        rec->logged = true;
        rec->outW = outW;   rec->outH = outH;
        rec->cellW = cellW; rec->cellH = cellH;
        rec->laneW = laneW; rec->laneH = laneH;
        // name=value pairs for expect.logLineFields; submitW/submitH are read back off the ImVec2
        // just passed to ImageWithBg, so the line reports the submission (which relations a pack
        // asserts: see the header).
        LOG_INFO("%s frozen submit view=%u submitW=%.0f submitH=%.0f outW=%u "
                 "outH=%u cellW=%u cellH=%u laneW=%u laneH=%u tint=%.2f",
                 prefix, viewId, submit.x, submit.y, outW, outH,
                 cellW, cellH, laneW, laneH, kFrozenTint.x);
    }
}

} // namespace Editor
