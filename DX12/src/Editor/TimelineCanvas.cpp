#include "Editor/TimelineCanvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Editor
{

namespace
{
    constexpr float kMinPxPerFrame = 0.05f;
    constexpr float kMaxPxPerFrame = 60.f;

    // Pick a tick step (in frames) that keeps labels at least kMinLabelPx
    // apart, from a 1-2-5-10 ladder so the numbers stay round at every zoom.
    int32_t NiceStep(float pxPerFrame, float minLabelPx)
    {
        const float wanted = minLabelPx / std::max(pxPerFrame, 1e-4f);
        int32_t step = 1;
        while (step < wanted)
        {
            if      (step * 2 >= wanted) { step *= 2; break; }
            else if (step * 5 >= wanted) { step *= 5; break; }
            else                           step *= 10;
        }
        return std::max(1, step);
    }
}

bool TimelineCanvas::Begin(const char* id, float height, int32_t duration, float fps,
                           float contentHeight)
{
    durationFrames = std::max(1, duration);
    frameRate      = (fps > 0.f) ? fps : 30.f;
    m_contentHeight = contentHeight;

    ImGui::BeginChild(id, ImVec2(0, height), false, ImGuiWindowFlags_NoScrollbar);

    dl     = ImGui::GetWindowDrawList();
    origin = ImGui::GetCursorScreenPos();

    const float availW = ImGui::GetContentRegionAvail().x;
    canvasW = availW;
    const float availH = std::max(1.f, ImGui::GetContentRegionAvail().y);
    lanesX = origin.x + headerWidth;
    lanesW = std::max(1.f, availW - headerWidth);
    lanesY = origin.y + rulerHeight;
    lanesH = std::max(1.f, availH - rulerHeight);

    // Up front, before anything is drawn: a stale zoom carried over from a
    // previous document must never leave the lanes off-screen on frame 1.
    ClampScroll(m_contentHeight);

    // Cover the ruler + lanes with an InvisibleButton. Two jobs: it gives
    // dragging something to grab (otherwise ImGui treats a drag on empty canvas
    // as "move the window"), and it gives us an honest hovered/active state.
    // The header column stays uncovered so its real widgets keep working.
    ImGui::SetCursorScreenPos(ImVec2(lanesX, origin.y));
    ImGui::InvisibleButton("##tlLanes", ImVec2(lanesW, rulerHeight + lanesH),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    lanesHovered = ImGui::IsItemHovered();
    lanesActive  = ImGui::IsItemActive();
    ImGui::SetCursorScreenPos(origin);   // restore for header widgets

    // Clip to the whole canvas, header column included: ImGui widgets draw into this
    // draw list and honour this clip stack, so a lane-only clip would cull every
    // track header. Lane content narrows the clip itself via PushLaneClip.
    dl->PushClipRect(origin, ImVec2(origin.x + canvasW, origin.y + rulerHeight + lanesH), true);
    return lanesW > 1.f && lanesH > 1.f;
}

void TimelineCanvas::End()
{
    if (dl) dl->PopClipRect();
    ImGui::EndChild();
    dl = nullptr;
}

void TimelineCanvas::PushLaneClip()
{
    if (!dl) return;
    dl->PushClipRect(ImVec2(lanesX, origin.y),
                     ImVec2(lanesX + lanesW, origin.y + rulerHeight + lanesH), true);
}

void TimelineCanvas::PopLaneClip()
{
    if (dl) dl->PopClipRect();
}

void TimelineCanvas::PushHeaderClip()
{
    if (!dl) return;
    dl->PushClipRect(ImVec2(origin.x, origin.y),
                     ImVec2(lanesX - 2.f, origin.y + rulerHeight + lanesH), true);
}

void TimelineCanvas::PopHeaderClip()
{
    if (dl) dl->PopClipRect();
}

int32_t TimelineCanvas::SnapFrame(float f) const
{
    return static_cast<int32_t>(std::lround(f));
}

int TimelineCanvas::RowAtY(float y) const
{
    if (rowHeight <= 0.f) return -1;
    const float rel = y - lanesY + scrollY;
    if (rel < 0.f) return -1;
    return static_cast<int>(rel / rowHeight);
}

void TimelineCanvas::ClampScroll(float contentHeight)
{
    pxPerFrame = std::clamp(pxPerFrame, kMinPxPerFrame, kMaxPxPerFrame);

    // Leave a screenful of slack past the end so the last section is reachable
    // without the view snapping back.
    const float visibleFrames = lanesW / pxPerFrame;
    const float maxScroll = std::max(0.f, static_cast<float>(durationFrames) - visibleFrames * 0.5f);
    scrollFrames = std::clamp(scrollFrames, -visibleFrames * 0.1f, maxScroll);

    const float maxY = std::max(0.f, contentHeight - lanesH);
    scrollY = std::clamp(scrollY, 0.f, maxY);
}

void TimelineCanvas::FrameAll()
{
    if (lanesW <= 1.f) return;
    pxPerFrame   = std::clamp(lanesW / std::max(1.f, static_cast<float>(durationFrames)),
                              kMinPxPerFrame, kMaxPxPerFrame);
    scrollFrames = 0.f;
}

void TimelineCanvas::HandleZoomPan()
{
    if (!lanesHovered) return;
    ImGuiIO& io = ImGui::GetIO();

    if (io.MouseWheel != 0.f)
    {
        if (io.KeyShift || io.KeyCtrl)
        {
            // Zoom pivots on the cursor: zooming about the scroll origin walks whatever
            // you were looking at off the screen.
            const float pivotFrame = XToFrame(io.MousePos.x);
            pxPerFrame = std::clamp(pxPerFrame * std::pow(1.15f, io.MouseWheel),
                                    kMinPxPerFrame, kMaxPxPerFrame);
            scrollFrames = pivotFrame - (io.MousePos.x - lanesX) / pxPerFrame;
        }
        else
        {
            scrollY -= io.MouseWheel * rowHeight * 3.f;
        }
        ClampScroll(m_contentHeight);
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
        scrollFrames -= d.x / pxPerFrame;
        scrollY      -= d.y;
        ClampScroll(m_contentHeight);
    }
}

void TimelineCanvas::DrawRangeShading(int32_t workStart, int32_t workEnd)
{
    PushLaneClip();
    const ImU32 kOutside = IM_COL32(0, 0, 0, 90);
    const float y0 = origin.y;
    const float y1 = origin.y + rulerHeight + lanesH;

    // Before frame 0 / after the duration.
    const float x0 = FrameToX(0.f);
    const float x1 = FrameToX(static_cast<float>(durationFrames));
    if (x0 > lanesX)          dl->AddRectFilled(ImVec2(lanesX, y0), ImVec2(x0, y1), kOutside);
    if (x1 < lanesX + lanesW) dl->AddRectFilled(ImVec2(x1, y0), ImVec2(lanesX + lanesW, y1), kOutside);

    // Outside the work range - the sub-span the transport loops. Without one, a
    // small tweak in a long cinematic means re-scrubbing all of it.
    if (workEnd > workStart && (workStart > 0 || workEnd < durationFrames))
    {
        const float wx0 = FrameToX(static_cast<float>(workStart));
        const float wx1 = FrameToX(static_cast<float>(workEnd));
        const ImU32 kDim = IM_COL32(0, 0, 0, 55);
        if (wx0 > x0) dl->AddRectFilled(ImVec2(std::max(lanesX, x0), y0), ImVec2(wx0, y1), kDim);
        if (wx1 < x1) dl->AddRectFilled(ImVec2(wx1, y0), ImVec2(std::min(lanesX + lanesW, x1), y1), kDim);
        dl->AddLine(ImVec2(wx0, y0), ImVec2(wx0, y1), IM_COL32(255, 200, 90, 160), 1.f);
        dl->AddLine(ImVec2(wx1, y0), ImVec2(wx1, y1), IM_COL32(255, 200, 90, 160), 1.f);
    }
    PopLaneClip();
}

void TimelineCanvas::DrawRuler(const std::vector<int32_t>* markerFrames)
{
    PushLaneClip();
    const float y0 = origin.y;
    const float y1 = origin.y + rulerHeight;
    dl->AddRectFilled(ImVec2(lanesX, y0), ImVec2(lanesX + lanesW, y1), IM_COL32(32, 32, 36, 255));

    const int32_t step  = NiceStep(pxPerFrame, 60.f);
    const int32_t minor = std::max(1, step / 5);

    const int32_t first = static_cast<int32_t>(std::floor(XToFrame(lanesX)));
    const int32_t last  = static_cast<int32_t>(std::ceil (XToFrame(lanesX + lanesW)));

    for (int32_t f = (first / minor) * minor; f <= last; f += minor)
    {
        const float x = FrameToX(static_cast<float>(f));
        if (x < lanesX - 2.f || x > lanesX + lanesW + 2.f) continue;
        const bool major = (f % step) == 0;
        dl->AddLine(ImVec2(x, major ? y0 + 4.f : y1 - 6.f), ImVec2(x, y1),
                    major ? IM_COL32(150, 150, 160, 200) : IM_COL32(90, 90, 100, 160), 1.f);
        if (major)
        {
            // Frame number on top, seconds underneath: cinematics are cut on
            // frames but reviewed in seconds, and converting in your head at
            // 24 vs 30 fps is exactly where mistakes happen.
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%d", f);
            dl->AddText(ImVec2(x + 3.f, y0 + 2.f), IM_COL32(210, 210, 220, 230), buf);
            std::snprintf(buf, sizeof(buf), "%.2fs", static_cast<double>(f) / frameRate);
            dl->AddText(ImVec2(x + 3.f, y0 + 12.f), IM_COL32(130, 130, 140, 200), buf);
        }
    }

    if (markerFrames)
        for (int32_t mf : *markerFrames)
        {
            const float x = FrameToX(static_cast<float>(mf));
            if (x < lanesX || x > lanesX + lanesW) continue;
            dl->AddTriangleFilled(ImVec2(x - 5.f, y1 - 8.f), ImVec2(x + 5.f, y1 - 8.f),
                                  ImVec2(x, y1), IM_COL32(250, 210, 100, 230));
        }

    dl->AddLine(ImVec2(lanesX, y1), ImVec2(lanesX + lanesW, y1), IM_COL32(70, 70, 80, 255), 1.f);
    PopLaneClip();
}

void TimelineCanvas::DrawPlayhead(int32_t frame)
{
    PushLaneClip();
    const float x = FrameToX(static_cast<float>(frame));
    if (x < lanesX - 8.f || x > lanesX + lanesW + 8.f) { PopLaneClip(); return; }
    const float y0 = origin.y;
    const float y1 = origin.y + rulerHeight + lanesH;
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 90, 90, 220), 1.5f);
    dl->AddTriangleFilled(ImVec2(x - 6.f, y0), ImVec2(x + 6.f, y0), ImVec2(x, y0 + 9.f),
                          IM_COL32(255, 90, 90, 255));
    PopLaneClip();
}

} // namespace Editor
