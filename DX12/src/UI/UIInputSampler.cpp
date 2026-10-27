#include "UI/UIInputSampler.h"

#include "UI/UICanvasSystem.h"   // UIInputState
#include "Input/InputSystem.h"
#include "Platform/PointerState.h"

#include <cstdint>

void UI::SampleCanvasInput(Plat::PointerState& ms, UIInputState& uiInput, bool& prevUILeft,
                           float vpMinX, float vpMinY, bool hostOwnsPointer, float scaleX, float scaleY)
{
    const auto mp = ms.PhysicalPos();
    uiInput.mousePos = { (static_cast<float>(mp.first) - vpMinX) * scaleX,
                         (static_cast<float>(mp.second) - vpMinY) * scaleY };
    const bool mouseLeft = ms.PhysicalLeftIsPressed();
    uiInput.mouseLeft   = mouseLeft;
    uiInput.mouseRight  = ms.PhysicalRightIsPressed();
    uiInput.mouseMiddle = ms.PhysicalMiddleIsPressed();
    uiInput.mouseDown   = mouseLeft && !prevUILeft;
    uiInput.mouseUp     = !mouseLeft && prevUILeft;
    prevUILeft          = mouseLeft;
    // Consumed exactly once per frame, here; a second consumer would silently eat the scroll.
    uiInput.wheelDelta  = { ms.ConsumePhysicalHWheel(), ms.ConsumePhysicalWheel() };
    // The host's chrome owns the pointer (the editor's ImGui; never in Game.exe):
    // keep tracking the physical edges, but nothing reaches the Canvas.
    if (hostOwnsPointer)
    {
        uiInput.mousePos  = { -1.0e9f, -1.0e9f };
        uiInput.mouseDown = false;
        uiInput.mouseUp   = false;
    }
    // ---- Keyboard / gamepad navigation edges ----
    // Derived once here, like the mouse edges: two consumers would each use up the same press.
    // Directions come from the RAW devices, not the action layer: there is no rebindable
    // "menu up" GameAction, and raw reads are ungated, so a menu that froze the sim (zero time
    // scale) still responds. Submit / cancel take the action layer first (a rebound UIConfirm
    // works), then OR in the raw defaults so a strange binding cannot make the menu unusable.
    {
        Input& in = Input::Get();
        auto keyEdge = [&in](int vk) { return in.WasKeyPressed(vk); };
        // GamepadDevice directly, bypassing Input::Cmd(): Cmd() is zeroed whenever the sim is not
        // advancing, which is exactly when a menu still needs the pad.
        const GamepadDevice& pad = in.Pad();
        const int padSlot = pad.ActiveSlot();
        auto padEdge = [&pad, padSlot](std::uint16_t m) {
            return padSlot >= 0 && pad.State(padSlot).Pressed(m);
        };

        const bool up    = keyEdge(VK_UP)    || padEdge(PadBtn::DUp);
        const bool down  = keyEdge(VK_DOWN)  || padEdge(PadBtn::DDown);
        const bool left  = keyEdge(VK_LEFT)  || padEdge(PadBtn::DLeft);
        const bool right = keyEdge(VK_RIGHT) || padEdge(PadBtn::DRight);

        uiInput.navY = (down ? 1 : 0) - (up ? 1 : 0);
        uiInput.navX = (right ? 1 : 0) - (left ? 1 : 0);
        uiInput.submit = in.Cmd().Pressed(GameAction::UIConfirm)
                      || keyEdge(VK_RETURN) || keyEdge(VK_SPACE)
                      || padEdge(PadBtn::A);
        uiInput.cancel = in.Cmd().Pressed(GameAction::UICancel)
                      || keyEdge(VK_ESCAPE) || padEdge(PadBtn::B);
    }
}
