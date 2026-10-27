#pragma once

// TimelineCanvas - a reusable frame-ruler / lane / playhead canvas.
//
// It holds only view state and geometry: which rows exist, what is drawn in them
// and what a click means are the caller's business.
// TODO(sequencer_system_plan M8): move NotifyTrackEditor onto this canvas.
//
// Techniques carried over from NotifyTrackEditor, each for a reason:
//   - An InvisibleButton covers the lane region. Without an item under the
//     cursor, dragging on empty canvas falls through to ImGui's "drag void =
//     move window" and the whole floating window slides around.
//   - The header column is not covered, so real widgets in it (name fields,
//     mute buttons) still receive input.
//   - Wheel zoom pivots on the cursor's frame, not on the scroll origin, or
//     zooming walks the content off-screen.
//   - Scroll is clamped up front, so a stale zoom level from a previous
//     document can never strand the lanes outside the viewport on frame 1.

#include "imgui/imgui.h"

#include <cstdint>
#include <vector>

namespace Editor
{

class TimelineCanvas
{
public:
    // @p contentHeight is the total pixel height of all rows, for vertical scroll
    // clamping. Returns false if the canvas is degenerate (zero area); End() must
    // still be called then.
    bool Begin(const char* id, float height, int32_t duration, float fps, float contentHeight);
    void End();

    float   FrameToX(float f) const { return lanesX + (f - scrollFrames) * pxPerFrame; }
    float   XToFrame(float x) const { return scrollFrames + (x - lanesX) / pxPerFrame; }
    int32_t SnapFrame(float f) const;
    float   RowY(int row) const     { return lanesY + row * rowHeight - scrollY; }
    int     RowAtY(float y) const;

    // Ruler with adaptive tick spacing, second labels, and optional marker
    // pips. Call after Begin().
    void DrawRuler(const std::vector<int32_t>* markerFrames = nullptr);

    // Shades the region outside [0, durationFrames] and, optionally, outside the
    // work range, so it is obvious which part of the timeline the transport plays.
    void DrawRangeShading(int32_t workStart, int32_t workEnd);

    void DrawPlayhead(int32_t frame);

    // Wheel zoom about the cursor + middle-drag pan. Only acts while the lane
    // region is hovered, so it never steals a scroll meant for the window.
    void HandleZoomPan();

    void ClampScroll(float contentHeight);

    // Narrows the clip to the lane region only. Begin() clips to the whole canvas,
    // header column included: ImGui widgets draw into this same draw list and obey
    // the same clip stack, so a lane clip there would erase the track headers. Lane
    // content pushes this instead, so a section scrolled off the left cannot bleed
    // over the headers.
    void PushLaneClip();
    void PopLaneClip();

    // Narrows the clip to the header column, so a long track name cannot run out
    // over the sections; the two clips give each column its own space.
    void PushHeaderClip();
    void PopHeaderClip();

    // Frame the whole duration in the visible width.
    void FrameAll();

    // ---- Persistent view state (the owner keeps a TimelineCanvas alive) ----
    float scrollFrames = 0.f;    // leftmost visible frame
    float pxPerFrame   = 6.f;
    float scrollY      = 0.f;    // vertical scroll, pixels

    // ---- Style ----
    float headerWidth = 240.f;
    float rulerHeight = 26.f;
    float rowHeight   = 22.f;

    // ---- Per-frame geometry, filled by Begin() ----
    ImDrawList* dl      = nullptr;
    ImVec2      origin  {};      // top-left of the canvas, screen space
    float       canvasW = 0.f;   // header column + lanes
    float       lanesX  = 0.f;   // left edge of the lane region
    float       lanesW  = 0.f;
    float       lanesY  = 0.f;   // top of the first row (below the ruler)
    float       lanesH  = 0.f;
    bool        lanesHovered = false;
    bool        lanesActive  = false;

    int32_t durationFrames = 100;
    float   frameRate      = 30.f;

private:
    float m_contentHeight = 0.f;
};

} // namespace Editor
