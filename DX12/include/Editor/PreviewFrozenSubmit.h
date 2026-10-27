// PreviewFrozenSubmit.h - the ONE place a suspended cell's last picture is SUBMITTED to
// ImGui, shared by SplitViewportPanel and CameraPreviewWindow.
//
// It draws as well as logs, so the logged rect IS the submitted one: under -hidden no
// pixel can be captured (io.DisplaySize is 0, no draw data reaches a swap chain), so
// what the panel SUBMITTED is the only observable. A pack asserts what can fail:
// submit != cell (the picture keeps the size it was rendered at, e.g. 640x360 in a
// 480x270 cell - never stretched into the rect the cell moved to) and out == lane.
// `submitW == outW` is true by construction and is not asserted. kFrozenTint is the
// single tint passed and reported, for the same reason.
//
// Logs on CHANGE of the whole tuple, not once: a cell resized while suspended submits a
// different rect, the event a pack must see. (A one-shot latch suits only the fixed
// refusal wording - the panels' m_sizeRefusedLogged.)
#pragma once

#include "imgui/imgui.h"

#include <cstdint>

namespace Editor
{
    // The grey a suspended cell's last picture is drawn with. ONE constant: a pack asserts
    // `tint=0.45` off the log line, and the panel that drew it must be the one that
    // reported it.
    inline constexpr ImVec4 kFrozenTint{ 0.45f, 0.45f, 0.45f, 1.f };

    // SUBMIT a suspended view's frozen picture and log the rect it was submitted at (on
    // change). @p prefix names the panel ("[PREVIEW] split:" / "[PREVIEW] camera panel:"):
    // both call sites share one line shape so they cannot drift apart. @p tex is the
    // view's last SRV, already wrapped - the caller has checked there IS one. @p outW /
    // @p outH are the size the texture was ACTUALLY rendered at
    // (Renderer::GetPreviewOutputSize), independent of the cell's want: THIS is what gets
    // submitted. @p cellW / @p cellH: what the cell wants now. @p laneW / @p laneH: the
    // WorldCull lane's current size.
    // Editor-only, main thread, inside the ImGui frame; reads no World and no render state.
    void SubmitFrozenImage(const char* prefix, std::uint32_t viewId,
                           ImTextureRef tex,
                           std::uint32_t outW, std::uint32_t outH,
                           std::uint32_t cellW, std::uint32_t cellH,
                           std::uint32_t laneW, std::uint32_t laneH);
}
