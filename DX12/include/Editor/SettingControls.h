#pragma once

// Editor/SettingControls.h - a Project Settings control for one settings row:
// the row's kind picks the widget, its range / labels / values the choices,
// its title / tip the label and the tooltip, kSettingRestart a "(restart)"
// note. The value shown is what runs (the row's capture); a change goes through
// Settings::EditPlayerRow / EditDevRow like every other control: recorded in
// the settings store and applied for that row alone.

#include "Settings/SettingsEdit.h"

#include <functional>
#include <string>

namespace EditorUI
{
    // The store's writer name for every Project Settings control.
    inline constexpr const char* kProjectSettingsWriter = "Project Settings";

    // The label a row's control shows: its title, else its key.
    template <class S> const char* SettingTitle(const Settings::RowT<S>& r);
    // "title##path": the ImGui label of the row's widget - a hook's too - so two
    // rows with one title on a page keep their own ids.
    template <class S> std::string SettingLabel(const Settings::RowT<S>& r);

    // Draws the row's default control over `v` and then its notes; true when
    // the user changed it this frame (`v` holds the new value).
    //   Bool          a checkbox
    //   Int / Float   a drag clamped to [lo, hi] (AlwaysClamp: text entry
    //                 cannot hand in NaN or an out-of-range value)
    //   Enum          a combo of its labels (the value is the index)
    //   Choice        a combo of its values, named by its labels when it has
    //                 one label per value
    //   String        a combo of its legal names when it has them, else a text
    //                 field that writes on Enter
    template <class S> bool SettingControl(const Settings::RowT<S>& r, Settings::Value& v);

    // The tooltip and the "(restart)" note of the control just drawn (the
    // default control calls it; a hook calls it after its own widget).
    template <class S> void SettingNotes(const Settings::RowT<S>& r);

    // One row on a page, the whole cycle: read what runs (the row's capture - a
    // player row without one shows the settings store's value, a developer row its
    // default), draw it (`draw`, else SettingControl) and write a change as
    // kProjectSettingsWriter. True when it changed. A path with no row draws a
    // disabled note instead.
    using PlayerDraw = std::function<bool(const Settings::Row&, Settings::Value&)>;
    using DevDraw    = std::function<bool(const Settings::DevRow&, Settings::Value&)>;
    // Each row also carries the shared row chrome (Editor/PropertyRow.h): the modified marker and
    // the right-click menu (copy / paste the value, reset to the C++ default, copy the path, copy the
    // Settings.set call), and is skipped while the active row filter does not show it.
    bool PlayerSetting(const char* path, Settings::CaptureCtx& read, Settings::ApplyCtx& write,
                       const PlayerDraw& draw = {});
    bool DevSetting(const char* path, Renderer& r, const DevDraw& draw = {});
    // A pass row: the value shown is the store's effective one (what the passes
    // read) and a change goes to the Runtime layer (Settings::EditPassRow).
    bool PassSetting(const Settings::PassRow& r);

    // Whether a row's value differs from its C++ default (the modified marker, the filter).
    template <class S> bool SettingModified(const Settings::RowT<S>& r, const Settings::Value& v);
}
