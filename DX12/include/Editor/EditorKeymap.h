#pragma once

// EditorKeymap - a keyboard chord ("Ctrl+Shift+S") as the command registry binds it, with the text
// form the keymap file, the menus and the shortcut help use. Header-only: the parser is unit-tested
// without an ImGui context (only ImGuiKey's values are used).

#include "imgui/imgui.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace Editor
{
    struct Chord
    {
        ImGuiKey key   = ImGuiKey_None;
        bool     ctrl  = false;
        bool     shift = false;
        bool     alt   = false;

        bool Empty() const { return key == ImGuiKey_None; }
        bool operator==(const Chord& o) const
        {
            return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
        }
        bool operator!=(const Chord& o) const { return !(*this == o); }
    };

    namespace KeymapDetail
    {
        struct KeyName
        {
            ImGuiKey    key;
            const char* name;   // the first spelling of a key is the one FormatChord writes
        };

        inline const KeyName kKeyNames[] = {
            { ImGuiKey_Delete, "Del" },       { ImGuiKey_Delete, "Delete" },
            { ImGuiKey_Escape, "Esc" },       { ImGuiKey_Escape, "Escape" },
            { ImGuiKey_Enter, "Enter" },      { ImGuiKey_Tab, "Tab" },
            { ImGuiKey_Space, "Space" },      { ImGuiKey_Backspace, "Backspace" },
            { ImGuiKey_Insert, "Ins" },       { ImGuiKey_Insert, "Insert" },
            { ImGuiKey_Home, "Home" },        { ImGuiKey_End, "End" },
            { ImGuiKey_PageUp, "PgUp" },      { ImGuiKey_PageUp, "PageUp" },
            { ImGuiKey_PageDown, "PgDn" },    { ImGuiKey_PageDown, "PageDown" },
            { ImGuiKey_LeftArrow, "Left" },   { ImGuiKey_RightArrow, "Right" },
            { ImGuiKey_UpArrow, "Up" },       { ImGuiKey_DownArrow, "Down" },
            { ImGuiKey_Minus, "-" },          { ImGuiKey_Equal, "=" },
            { ImGuiKey_Comma, "," },          { ImGuiKey_Period, "." },
            { ImGuiKey_Slash, "/" },          { ImGuiKey_GraveAccent, "`" },
        };

        inline bool EqualNoCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
                if (x != y) return false;
            }
            return true;
        }

        inline ImGuiKey KeyFromName(std::string_view name)
        {
            if (name.size() == 1)
            {
                char c = name[0];
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
                if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
            }
            if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
            {
                int n = 0;
                bool digits = true;
                for (std::size_t i = 1; i < name.size(); ++i)
                {
                    if (name[i] < '0' || name[i] > '9') { digits = false; break; }
                    n = n * 10 + (name[i] - '0');
                }
                if (digits && n >= 1 && n <= 12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
            }
            for (const KeyName& k : kKeyNames)
                if (EqualNoCase(name, k.name)) return k.key;
            return ImGuiKey_None;
        }

        inline std::string NameOfKey(ImGuiKey key)
        {
            if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return std::string(1, static_cast<char>('A' + (key - ImGuiKey_A)));
            if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return std::string(1, static_cast<char>('0' + (key - ImGuiKey_0)));
            if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) return "F" + std::to_string(1 + (key - ImGuiKey_F1));
            for (const KeyName& k : kKeyNames)
                if (k.key == key) return k.name;
            return std::string();
        }
    }

    // "Ctrl+Shift+S", "F1", "Del", "Alt+F4" (case-insensitive; modifiers in any order). An empty
    // chord when the text names no key, a key twice, or an unknown modifier.
    inline Chord ParseChord(std::string_view text)
    {
        Chord c;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t plus = text.find('+', start);
            // A trailing '+' after a modifier is the key itself only when nothing follows.
            if (plus == start && plus + 1 == text.size()) plus = std::string_view::npos;
            const std::string_view part = text.substr(start, plus == std::string_view::npos ? std::string_view::npos
                                                                                             : plus - start);
            if (part.empty()) return Chord{};
            if (KeymapDetail::EqualNoCase(part, "Ctrl") || KeymapDetail::EqualNoCase(part, "Control")) c.ctrl = true;
            else if (KeymapDetail::EqualNoCase(part, "Shift")) c.shift = true;
            else if (KeymapDetail::EqualNoCase(part, "Alt")) c.alt = true;
            else
            {
                const ImGuiKey k = KeymapDetail::KeyFromName(part);
                if (k == ImGuiKey_None || c.key != ImGuiKey_None) return Chord{};
                c.key = k;
            }
            if (plus == std::string_view::npos) break;
            start = plus + 1;
        }
        if (c.key == ImGuiKey_None) return Chord{};
        return c;
    }

    // The text ParseChord reads back: "Ctrl+Shift+Alt+<key>"; "" for an empty chord.
    inline std::string FormatChord(const Chord& c)
    {
        if (c.Empty()) return std::string();
        std::string out;
        if (c.ctrl) out += "Ctrl+";
        if (c.shift) out += "Shift+";
        if (c.alt) out += "Alt+";
        return out + KeymapDetail::NameOfKey(c.key);
    }
}
