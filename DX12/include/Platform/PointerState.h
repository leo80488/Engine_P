#pragma once
// Platform/PointerState.h - the mouse as the window's WndProc saw it.
//
// Owned by Plat::Window (Window::Pointer()), written only by its WndProc on the
// main thread, read on the main thread. Two views:
//   PHYSICAL - recorded for every message, before any capture layer looks at it.
//     The retained Canvas UI reads it (UI::SampleCanvasInput): it needs the
//     pointer precisely while ImGui or the Canvas owns the gesture.
//   GAMEPLAY - presses, moves and the wheel are dropped while a capture
//     predicate says the UI wants the mouse; RELEASES are never dropped, so a
//     gesture captured mid-drag cannot leave a button latched. Game.exe's OS
//     mouse look reads it (OsMouseLook).
// Raw motion (WM_INPUT, absolute samples already turned into deltas by the
// window) accumulates until ConsumeRawDelta.

#include <utility>

namespace Plat
{
    class Window;

    class PointerState
    {
    public:
        // ---- Physical (ungated) ----
        std::pair<int, int> PhysicalPos() const noexcept { return { m_physX, m_physY }; }
        bool PhysicalLeftIsPressed()   const noexcept { return m_physLeft; }
        bool PhysicalRightIsPressed()  const noexcept { return m_physRight; }
        bool PhysicalMiddleIsPressed() const noexcept { return m_physMiddle; }
        bool PhysicalX1IsPressed()     const noexcept { return m_physX1; }
        bool PhysicalX2IsPressed()     const noexcept { return m_physX2; }
        // Horizontal wheel notches (WM_MOUSEHWHEEL, positive = right) since the
        // last call; same one-consumer rule as the vertical wheel.
        float ConsumePhysicalHWheel() noexcept
        {
            const float v = m_physHWheel;
            m_physHWheel = 0.f;
            return v;
        }
        // Wheel notches (one detent = 1.0, positive = away from the user) since the
        // last call. Exactly ONE caller per frame - the code that derives the other UI
        // input edges - or a second one silently eats the first one's scroll.
        float ConsumePhysicalWheel() noexcept
        {
            const float v = m_physWheel;
            m_physWheel = 0.f;
            return v;
        }

        // ---- Gameplay (capture-gated presses) ----
        std::pair<int, int> GetPos() const noexcept { return { m_x, m_y }; }
        bool LeftIsPressed()  const noexcept { return m_left; }
        bool RightIsPressed() const noexcept { return m_right; }
        bool IsInWindow()     const noexcept { return m_inWindow; }
        // Gameplay wheel notches since the last call (no reader yet; kept beside the
        // gameplay buttons for a camera that wants it).
        float ConsumeWheel() noexcept
        {
            const float v = m_wheel;
            m_wheel = 0.f;
            return v;
        }

        // ---- Raw motion ----
        // Accumulated WM_INPUT counts since the last call (unaccelerated).
        std::pair<int, int> ConsumeRawDelta() noexcept
        {
            const std::pair<int, int> d{ m_rawDX, m_rawDY };
            m_rawDX = m_rawDY = 0;
            return d;
        }

    private:
        friend class Window;

        // Physical.
        void OnPhysicalMove(int x, int y) noexcept   { m_physX = x; m_physY = y; }
        void OnPhysicalLeft(bool down, int x, int y) noexcept  { m_physLeft = down;  m_physX = x; m_physY = y; }
        void OnPhysicalRight(bool down, int x, int y) noexcept { m_physRight = down; m_physX = x; m_physY = y; }
        void OnPhysicalWheel(int delta) noexcept { m_physWheel += static_cast<float>(delta) / 120.f; }
        void OnPhysicalHWheel(int delta) noexcept { m_physHWheel += static_cast<float>(delta) / 120.f; }
        void OnPhysicalMiddle(bool down) noexcept { m_physMiddle = down; }
        void OnPhysicalX(int which, bool down) noexcept { (which == 1 ? m_physX1 : m_physX2) = down; }

        // Gameplay.
        void OnMove(int x, int y) noexcept { m_x = x; m_y = y; }
        void OnEnter() noexcept { m_inWindow = true; }
        void OnLeave() noexcept { m_inWindow = false; }
        // Buttons only: the gameplay position moves with WM_MOUSEMOVE alone
        // (OsMouseLook's delta depends on it).
        void OnLeft(bool down) noexcept  { m_left = down; }
        void OnRight(bool down) noexcept { m_right = down; }
        void OnWheel(int delta) noexcept { m_wheel += static_cast<float>(delta) / 120.f; }

        // Raw.
        void OnRawDelta(int dx, int dy) noexcept { m_rawDX += dx; m_rawDY += dy; }

        // Focus lost: every button reads released, in both views (a button held
        // through an alt-tab sends its WM_*BUTTONUP to the other application).
        void ReleaseAll() noexcept
        {
            m_physLeft = m_physRight = m_physMiddle = m_physX1 = m_physX2 = false;
            m_left = m_right = false;
        }

        int   m_physX = 0, m_physY = 0;
        bool  m_physLeft = false, m_physRight = false, m_physMiddle = false;
        bool  m_physX1 = false, m_physX2 = false;
        float m_physWheel = 0.f;
        float m_physHWheel = 0.f;

        int   m_x = 0, m_y = 0;
        bool  m_left = false, m_right = false;
        bool  m_inWindow = false;
        float m_wheel = 0.f;

        int   m_rawDX = 0, m_rawDY = 0;
    };
}
