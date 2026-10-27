#pragma once
// Game.exe's camera look from the OS mouse: right button held = look, and the delta is
// this frame's movement in pixels (the editor viewport's ImGui MouseDelta convention), so
// CameraSystem's sensitivity feels the same in both hosts. Global namespace, like the
// Input module's other types (Input itself is a class).

namespace Plat { class PointerState; }

class OsMouseLook
{
public:
    // @p pointer: the window's pointer - its gameplay (capture-gated) view, and its raw
    // motion, consumed here every frame.
    // @p uiCaptures: the game UI wants the mouse this frame (no look).
    // @p relative: the cursor is captured (hidden and clipped - GameFlow's cursor
    // "captured"). Then look is always on (no right button) and comes from raw motion,
    // because a pointer-position delta stops dead at the clip rectangle's edge.
    void Sample(Plat::PointerState& pointer, bool uiCaptures, bool relative, float& mouseLookDX,
                float& mouseLookDY, bool& mouseLookHeld);

private:
    // Previous-frame OS mouse position; m_prevMouseValid guards the first frame
    // so a cold start does not produce a huge jump.
    int  m_prevMouseX{ 0 };
    int  m_prevMouseY{ 0 };
    bool m_prevMouseValid{ false };
};
