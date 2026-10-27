#pragma once

// Settings/SetFlags.h - -set:<path>=<value>, the command line's writer of any settings
// row. settings.json's boot load applies the player overrides and reports every bad
// one; engine.json's boot load applies the developer ones; Settings::ApplyPassCommandLine
// the pass ones; the boot peeks read both files' rows, so a peek and the load after it
// agree.

#include "Settings/SettingsStore.h"   // Value, RowT

#include <string>
#include <vector>

namespace Settings
{
    // One override: its row, the coerced value, and the flag in the row's own
    // spelling ("-set:display.vsync") - the store's writer name.
    template <class S>
    struct SetFlag
    {
        const RowT<S>* row = nullptr;
        Value          value;
        std::string    flag;
    };

    struct SetFlags
    {
        std::vector<SetFlag<Resource::EngineSettings>>    player;      // settings.json's rows
        std::vector<SetFlag<Resource::EngineDevSettings>> developer;   // engine.json's rows
        std::vector<SetFlag<Resource::PassSettings>>      pass;        // the pass rows
    };

    // Every -set: on the command line, in argv order, by the document its path names (any
    // case: CommandLine lower-cases its keys and the rows are camelCase, so Args() is read
    // and each one is asked for through Has(), which keeps it off unreadFlags). A path no
    // row has, a missing value, or a value the row refuses (Coerce) goes to @p errors when
    // given and is skipped. Never logs: the boot peeks call it before anything else is up.
    // Before them, in this order: -scalability=<level> (every group's level row, writer
    // "-scalability") and -scalability:<group>=<level> (writer "-scalability:<group>";
    // Settings/Scalability.h), then -lighting_tier=<name> - the rows the tier's preset
    // writes (Settings/LightingTier.h), each with "-lighting_tier" as its writer. An
    // unknown level, group or tier goes to @p errors.
    SetFlags ParseSetFlags(std::vector<std::string>* errors);
}
