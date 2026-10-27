#pragma once

// PropertyRow - what every property row shares wherever it is drawn: the Inspector's reflected fields
// and Project Settings' rows. Right after the row's widget, PropertyRowChrome adds a marker on the
// row's left edge while its value differs from the default, and a right-click menu: copy / paste the
// value as text, reset it to the default, copy the row's path, copy the Lua call that sets it. A
// RowFilter (the panel's search box, "modified only") decides which rows are drawn at all.
// IPropertySource lists rows of any system the same way (Project Settings' search over every row).
// UI thread.

#include <cstddef>
#include <string>

namespace EditorUI
{
    struct PropertyRowInfo
    {
        std::string path;          // "RigidBody.linDamp", "display.vsync"
        std::string valueText;     // the value as text: what Copy puts on the clipboard
        std::string luaSetter;     // the Lua call that writes this value; empty = none offered
        bool        modified = false;   // the value differs from the default
        bool        writable = true;    // Paste and Reset offered
    };

    enum class RowAction { None, Paste, Reset };

    // Call right after the row's last widget. Paste hands the clipboard text in @p pasted; the caller
    // converts it, writes it and reports the change as its widget would.
    RowAction PropertyRowChrome(const PropertyRowInfo& row, std::string& pasted);

    // The rows a search shows: the text matched, without case, against the label, the path and the
    // tooltip; with modifiedOnly, only rows whose value differs from the default.
    struct RowFilter
    {
        std::string text;
        bool        modifiedOnly = false;

        bool Active() const { return !text.empty() || modifiedOnly; }
        bool Matches(const char* label, const char* path, const char* tip) const;
        bool Shows(const char* label, const char* path, const char* tip, bool modified) const
        {
            return (!modifiedOnly || modified) && Matches(label, path, tip);
        }
    };

    // The filter the row drawers read while a panel draws (the Inspector sets its own around its
    // components); nullptr = every row.
    void             SetActiveRowFilter(const RowFilter* filter);
    const RowFilter* ActiveRowFilter();

    // A literal of the text form as Lua source: a number or boolean as is, a "a_b_c" vector as {a, b, c},
    // anything else as a quoted string.
    std::string LuaLiteral(const std::string& valueText, bool isString);

    // A list of rows any system can offer to a generic drawer: enumerate, label, path, tooltip, whether
    // the value is the default, and the row's own control (which writes through the system's own path).
    class IPropertySource
    {
    public:
        virtual ~IPropertySource() = default;

        virtual std::size_t RowCount() const = 0;
        virtual const char* Label(std::size_t row) const = 0;
        virtual const char* Path(std::size_t row) const = 0;
        virtual const char* Tooltip(std::size_t row) const = 0;
        virtual bool        Modified(std::size_t row) const = 0;
        // Draws the row's control and its chrome; true when the value changed.
        virtual bool        DrawRow(std::size_t row) = 0;
    };

    // Draws the rows of @p source that @p filter shows (all when null); returns how many it drew.
    std::size_t DrawPropertySource(IPropertySource& source, const RowFilter* filter);
}
