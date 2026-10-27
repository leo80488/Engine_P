#pragma once

// EditorCommands - the editor's actions, defined once. The menus, the command palette, the keyboard
// shortcuts and the shortcut help read the same entry: its id, label, chord, scope and whether it
// can run now. A user's chord overrides (editor_prefs.ini "keymap.<id>") replace the defaults.

#include "Editor/EditorKeymap.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Editor
{
    // Where a command's chord is live.
    enum class CommandScope : std::uint8_t
    {
        Global,     // any time the editor has the keyboard
        Document,   // acts on the active document; its chord yields to a panel that owns its keys
        Panel,      // only while the named panel has focus
    };

    struct Command
    {
        std::string  id;         // stable: "file.save" - keymap overrides, scripts and tests name it
        std::string  category;   // "File" - the palette prefix and the help's group
        std::string  label;      // "Save" (no category)
        std::string  keywords;   // more words the palette search matches
        const char*  icon = nullptr;   // a FontAwesome glyph for the menu's icon column (static text)
        Chord        defaultChord;
        Chord        altChord;   // a second chord that always works too (Ctrl+Shift+Z for redo)
        // The chord is shown, never dispatched: something else reads that key (Alt+F4 is the OS's
        // close, F / End / W / E / R are the viewport's own while it is hovered).
        bool         displayOnly = false;
        CommandScope scope = CommandScope::Global;
        std::string  panel;      // CommandScope::Panel: the panel whose focus it needs
        std::function<void()> run;
        // Optional. False greys the command out; @p why (may be empty) is the tooltip.
        std::function<bool(std::string& why)> enabled;
        std::function<bool()>                 checked;       // optional: a toggle's state
        std::function<std::string()>          dynamicLabel;  // optional: "Undo Move" over "Undo"
        std::function<std::string()>          tooltip;       // optional: the menu item's hover text
        // Optional, for the chord only (menus and the palette ignore it): whether the key is the
        // command's this frame (Delete only while the Hierarchy or the Viewport has focus).
        std::function<bool()>                 chordLive;
    };

    class CommandRegistry
    {
    public:
        static CommandRegistry& Get();

        // ---- Commands ----
        // Adds @p c, or replaces the command with the same id.
        void Register(Command c);
        const Command* Find(std::string_view id) const;
        const std::vector<Command>& All() const { return m_commands; }
        // The label to show: dynamicLabel when set, else label.
        std::string LabelOf(const Command& c) const;
        bool IsEnabled(const Command& c, std::string* why = nullptr) const;
        // Runs @p id when it exists and is enabled; false otherwise (@p why says which).
        bool Execute(std::string_view id, std::string* why = nullptr);

        // ---- Keymap ----
        // The chord in effect: the user's override when there is one, else the default.
        Chord ChordOf(const Command& c) const;
        Chord ChordOf(std::string_view id) const;
        // An empty @p chord unbinds the command; the default chord restores it with ResetChord.
        void  SetUserChord(const std::string& id, const Chord& chord);
        void  ResetChord(const std::string& id);
        bool  HasUserChord(const std::string& id) const { return m_userChords.count(id) != 0; }
        void  ClearUserChords() { m_userChords.clear(); }
        const std::unordered_map<std::string, Chord>& UserChords() const { return m_userChords; }
        // Pairs of command ids sharing a chord where both can be live at once (two Global ones, or a
        // Global and a Document one, or two of the same panel).
        std::vector<std::pair<std::string, std::string>> Conflicts() const;

    private:
        std::vector<Command>                    m_commands;
        std::unordered_map<std::string, Chord>  m_userChords;   // id -> chord (empty = unbound)
    };

    // A chord against this frame's keyboard: its key pressed (no repeat) with exactly its modifiers.
    bool ChordPressed(const Chord& c);
}
