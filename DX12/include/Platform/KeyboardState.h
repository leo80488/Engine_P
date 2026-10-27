#pragma once
// Platform/KeyboardState.h - the keyboard (and the mouse buttons, by VK) as the window's WndProc saw
// them: the source Input::Update samples instead of polling GetAsyncKeyState.
//
// Owned by Plat::Window (Window::Keyboard()), written by its WndProc and read by Input::Update, both
// on the main thread. The table it hands out has one byte per VK, 1 = down.
//
// How a message source differs from a poll, and how each case is handled:
//   * a key released while another application had focus sends no WM_KEYUP here: the table is
//     cleared on WM_KILLFOCUS and re-read from GetAsyncKeyState on WM_SETFOCUS, so keys held across
//     the focus change come back down and nothing stays stuck;
//   * a key an IME is composing arrives as VK_PROCESSKEY: the window asks ImmGetVirtualKey for the
//     real one, so WASD still moves with a Chinese / Japanese input method on;
//   * a key pressed and released between two frames is latched and reads down for exactly one frame
//     (SnapshotForFrame), so a fast tap still makes its edges;
//   * Shift / Ctrl / Alt also set their left / right VKs (VK_LSHIFT ...);
//   * mouse buttons are the LOGICAL ones (a left-handed swap applies), matching the window's pointer
//     and the Canvas, so gameplay and the UI agree for a left-handed player.
// PrintScreen sends no WM_KEYDOWN, so it never reads down; nothing binds it.
//
// Text: WM_CHAR's UTF-16 (surrogate pairs joined) as UTF-8, bounded. Its only reader is the harness
// (Test.TypedText); a Canvas text field would be the first gameplay one.

#include <cstdint>
#include <string>

namespace Plat
{
    class Window;

    class KeyboardState
    {
    public:
        static constexpr int kNumKeys = 256;

        bool IsDown(int vk) const noexcept { return vk >= 0 && vk < kNumKeys && m_down[vk] != 0; }
        // Down now or pressed since the last snapshot (a tap): a host's look at a key before Input::Update
        // takes the frame's table (the editor's Esc that releases the Game View's capture).
        bool DownOrTapped(int vk) const noexcept
        {
            return vk >= 0 && vk < kNumKeys && (m_down[vk] != 0 || m_tapped[vk] != 0);
        }

        // The frame's table: 1 where a key is down OR was pressed since the last
        // snapshot (a tap shorter than a frame), 0 elsewhere. Clears the taps.
        // Input::Update calls it exactly once per frame.
        void SnapshotForFrame(std::uint8_t out[kNumKeys]) noexcept;
        // The taps since the last snapshot are dropped (the focus gate's branch:
        // an unfocused frame samples nothing, and must not replay them later).
        void DiscardTaps() noexcept;

        // The last scan code seen for @p vk, with 0x100 set for an extended key (0 = never seen). Kept for
        // a binding UI that wants layout-independent keys (AZERTY); the bindings themselves are VKs.
        std::uint16_t ScanCode(int vk) const noexcept
        { return (vk >= 0 && vk < kNumKeys) ? m_scan[vk] : std::uint16_t(0); }

        // UTF-8 typed since the last call (WM_CHAR, control characters dropped).
        std::string ConsumeText();

        // How many up -> down edges the WndProc recorded for @p vk over the window's life. Unlike the table
        // it is not cleared by a focus change or read by a snapshot: the harness reads it to prove a
        // message was (or was not) recorded (Test.KeyEdges).
        std::uint32_t DownEdges(int vk) const noexcept
        { return (vk >= 0 && vk < kNumKeys) ? m_downEdges[vk] : std::uint32_t(0); }

    private:
        // Bounded: only the harness drains the text, and a held key auto-repeats.
        static constexpr std::size_t kMaxText = 4096;

        friend class Window;

        // WM_(SYS)KEYDOWN / WM_(SYS)KEYUP with the message's VK and lParam.
        // @p viaIme: the VK came from ImmGetVirtualKey behind a VK_PROCESSKEY.
        // An IME may swallow that key's WM_KEYUP as well, so such a key is
        // re-checked against GetAsyncKeyState at every snapshot until it is up
        // (and only such a key - everything else is message-driven).
        void OnKey(int vk, bool down, std::uint32_t lParam, bool viaIme = false) noexcept;
        void OnMouseButton(int vk, bool down) noexcept;
        void OnChar(wchar_t unit);
        void Clear() noexcept;
        void ResyncFromAsync() noexcept;
        void Set(int vk, bool down) noexcept;

        std::uint8_t  m_down[kNumKeys] = {};
        std::uint8_t  m_tapped[kNumKeys] = {};
        std::uint8_t  m_imeHeld[kNumKeys] = {};   // down through an IME's VK_PROCESSKEY
        std::uint16_t m_scan[kNumKeys] = {};
        std::uint32_t m_downEdges[kNumKeys] = {};
        wchar_t       m_highSurrogate = 0;
        std::string   m_text;
    };
}
