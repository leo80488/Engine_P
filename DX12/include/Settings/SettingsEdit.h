#pragma once

// Settings/SettingsEdit.h - one row read from what runs, and one row written the way an
// editor control writes it. The editor's Project Settings controls and the editor's Lua
// Editor.ProjectSetting both go through these, so a control's change is recorded in the
// settings store and applied for that row alone.

#include "Settings/SettingsStore.h"   // Value, the stores, the rows and their contexts

#include <string_view>

namespace Settings
{
    // A row's live value: its capture hook into `s` (a Present row reads the
    // surface's present settings first, as CapturePlayerRows does). False, and
    // `s` untouched, for a row without a capture (Phase::None / Applied).
    bool CapturePlayerRow(const Row& r, Resource::EngineSettings& s, CaptureCtx& c);
    bool CaptureDevRow(const DevRow& r, Resource::EngineDevSettings& s, DevCaptureCtx& c);

    // An editor control's change: `v` recorded in the store as `writer` - a player row in
    // the Player layer (the footer's Save writes settings.json), a developer row in the
    // Runtime layer (never saved; "Save to engine.json" captures what runs and records the
    // Project layer) - then pushed to the row's owner alone (ApplyPlayerRow / ApplyDevRow).
    //  - A player row: recorded as the player's choice (Scalability::RecordPlayerChoice: a
    //    quality level lets go of its group's Player values; a group row's value its level
    //    gives already is no override), and its owner is handed the store's value (a
    //    LastWrite row: the click itself; a Layered row under a -set: or a Runtime value
    //    keeps that one; a Capped row: its allowance, the owner applies the scene's
    //    intent). Every other row whose value the write moved is pushed too: a level's group.
    //  - A developer row: the written value.
    //  - No drain: the controls run inside the ImGui frame, where these setter calls are
    //    safe (the pipe's Runtime writes run at a frame boundary and drain, as ApplyTo does).
    // Returns whether the row has an owner to push to (false: recorded only).
    bool EditPlayerRow(const Row& r, const Value& v, std::string_view writer, ApplyCtx& c);
    bool EditDevRow(const DevRow& r, const Value& v, std::string_view writer, DevApplyCtx& c);
    // A pass row: the Runtime layer, as a developer row's; the store is what the passes
    // read, so recording it is applying it.
    bool EditPassRow(const PassRow& r, const Value& v, std::string_view writer);
}
