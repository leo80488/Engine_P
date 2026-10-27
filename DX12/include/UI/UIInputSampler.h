#pragma once
// The Canvas UI's per-frame input: the pointer in CANVAS space, its one-frame edges and
// the wheel, from the ungated physical snapshot; and the keyboard / gamepad navigation
// edges. Derived ONCE per frame, here - a second consumer would use up the same press.

namespace Plat { class PointerState; }

namespace UI
{
    struct UIInputState;

    // @p pointer: the window's pointer (Plat::Window::Pointer()) - its PHYSICAL view,
    // and the wheel, consumed here once per frame.
    // @p uiInput: the frame's Canvas input, written here. @p prevUILeft: last frame's
    // left button (the edge memory, App's).
    // @p vpMinX / @p vpMinY: the viewport's origin in the window (the editor's docked
    // viewport; 0 in Game.exe and in F11). @p hostOwnsPointer: the host's chrome (the
    // editor's ImGui) owns the pointer this frame - edges are tracked, nothing reaches
    // the Canvas. @p scaleX / @p scaleY: Canvas pixels per window pixel (the editor's Game View shows a
    // fixed render size scaled into its panel; 1 everywhere else).
    void SampleCanvasInput(Plat::PointerState& pointer, UIInputState& uiInput, bool& prevUILeft,
                           float vpMinX, float vpMinY, bool hostOwnsPointer, float scaleX = 1.f,
                           float scaleY = 1.f);
}
