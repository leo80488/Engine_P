#include "Platform/KeyboardState.h"

#include <Windows.h>

#include <cstring>
#include <utility>

namespace Plat
{
void KeyboardState::Set(int vk, bool down) noexcept
{
    if (vk <= 0 || vk >= kNumKeys)
        return;
    // A tap is an up -> down transition; auto-repeat (down -> down) is not.
    if (down && !m_down[vk])
    {
        m_tapped[vk] = 1;
        ++m_downEdges[vk];
    }
    m_down[vk] = down ? 1 : 0;
}

void KeyboardState::OnKey(int vk, bool down, std::uint32_t lParam, bool viaIme) noexcept
{
    if (vk <= 0 || vk >= kNumKeys)
        return;
    m_imeHeld[vk] = (down && viaIme) ? std::uint8_t(1) : std::uint8_t(0);
    const std::uint16_t scan     = static_cast<std::uint16_t>((lParam >> 16) & 0xFF);
    const bool          extended = (lParam & (1u << 24)) != 0;
    const std::uint16_t scanExt  = static_cast<std::uint16_t>(scan | (extended ? 0x100 : 0));

    // The messages carry only the generic VK for the modifiers, but a binding may name a side
    // (VK_LSHIFT ...). Shift's side is in the scan code, Ctrl's and Alt's in the extended bit.
    int side = 0;
    int left = 0, right = 0;
    switch (vk)
    {
    case VK_SHIFT:
        left = VK_LSHIFT; right = VK_RSHIFT;
        side = (MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? VK_RSHIFT : VK_LSHIFT;
        break;
    case VK_CONTROL:
        left = VK_LCONTROL; right = VK_RCONTROL;
        side = extended ? VK_RCONTROL : VK_LCONTROL;
        break;
    case VK_MENU:
        left = VK_LMENU; right = VK_RMENU;
        side = extended ? VK_RMENU : VK_LMENU;
        break;
    default:
        break;
    }

    if (side != 0)
    {
        Set(side, down);
        m_scan[side] = scanExt;
        // The generic key is down while either side is.
        Set(vk, m_down[left] != 0 || m_down[right] != 0);
    }
    else
    {
        Set(vk, down);
    }
    m_scan[vk] = scanExt;
}

void KeyboardState::OnMouseButton(int vk, bool down) noexcept
{
    Set(vk, down);
}

void KeyboardState::OnChar(wchar_t unit)
{
    char32_t cp = 0;
    if (unit >= 0xD800 && unit <= 0xDBFF)
    {
        m_highSurrogate = unit;   // the low half is the next WM_CHAR
        return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF)
    {
        if (m_highSurrogate == 0)
            return;               // an unpaired low surrogate: not a character
        cp = 0x10000u + ((static_cast<char32_t>(m_highSurrogate) - 0xD800u) << 10)
                      + (static_cast<char32_t>(unit) - 0xDC00u);
        m_highSurrogate = 0;
    }
    else
    {
        m_highSurrogate = 0;
        cp = static_cast<char32_t>(unit);
    }
    // Control characters (Backspace, Tab, Enter, Escape, Ctrl+letter) are keys, not text.
    if (cp < 0x20 || cp == 0x7F)
        return;
    if (m_text.size() >= kMaxText)
        return;

    if (cp < 0x80)
        m_text.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        m_text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        m_text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        m_text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        m_text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        m_text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        m_text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        m_text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        m_text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        m_text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string KeyboardState::ConsumeText()
{
    std::string out;
    out.swap(m_text);
    return out;
}

void KeyboardState::SnapshotForFrame(std::uint8_t out[kNumKeys]) noexcept
{
    for (int vk = 0; vk < kNumKeys; ++vk)
    {
        // A key that went down through an IME may never see its WM_KEYUP:
        // trust the keyboard itself for it until it is released.
        if (m_imeHeld[vk] && !(GetAsyncKeyState(vk) & 0x8000))
        {
            m_imeHeld[vk] = 0;
            m_down[vk]    = 0;
        }
        out[vk] = (m_down[vk] | m_tapped[vk]) ? std::uint8_t(1) : std::uint8_t(0);
    }
    std::memset(m_tapped, 0, sizeof(m_tapped));
}

void KeyboardState::DiscardTaps() noexcept
{
    std::memset(m_tapped, 0, sizeof(m_tapped));
}

void KeyboardState::Clear() noexcept
{
    std::memset(m_down, 0, sizeof(m_down));
    std::memset(m_tapped, 0, sizeof(m_tapped));
    std::memset(m_imeHeld, 0, sizeof(m_imeHeld));
    m_highSurrogate = 0;
}

void KeyboardState::ResyncFromAsync() noexcept
{
    // Focus just came back: what is held RIGHT NOW is the truth, whatever
    // messages went to another application meanwhile. GetAsyncKeyState reports
    // the PHYSICAL mouse buttons; the table holds the logical ones the messages
    // carry, so undo a left-handed swap.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    for (int vk = 1; vk < kNumKeys; ++vk)
    {
        int physical = vk;
        if (swapped && vk == VK_LBUTTON)      physical = VK_RBUTTON;
        else if (swapped && vk == VK_RBUTTON) physical = VK_LBUTTON;
        m_down[vk] = (GetAsyncKeyState(physical) & 0x8000) ? std::uint8_t(1) : std::uint8_t(0);
    }
    std::memset(m_tapped, 0, sizeof(m_tapped));
    std::memset(m_imeHeld, 0, sizeof(m_imeHeld));
    m_highSurrogate = 0;
}
}   // namespace Plat
