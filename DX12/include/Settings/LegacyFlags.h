#pragma once

// Settings/LegacyFlags.h - a row's old command-line spellings (RowSpecT::legacyFlag /
// legacyRead).
//
// Syntax of .legacyFlag:
//   "-x"       the flag's own value
//   "-x=<v>"   given in any form, the row takes <v> (read as -set: reads it); the
//              flag's own value is not looked at
//   a|b|...    several spellings: the first one given decides
//
// Two styles, by document:
//   Pass       the pass rows. "-x" is read exactly as the pass read it: a Bool by
//              presence (Has), an Int / Float by GetInt / GetFloat with the row's
//              default as the fallback (a bare or malformed number IS the default;
//              with kSettingLegacyValueOnly it is no value), a String by Get. Not
//              marked read: the row's getter marks it when the pass first asks
//              (Settings/PassSettings.h).
//   Developer  engine.json's rows. "-x" is read as -set: reads a value
//              (RequestFromText + Coerce: the row's range and legal names); a bare
//              Bool is true; a bare Int / Float, or a number (Int, Float, Bool) whose
//              value is malformed or NaN, is Ignored with a warning (the row keeps its
//              own); a name with no value, or one the row does not offer, is Refused
//              like a bad -set:. Every spelling given is marked read at boot.

#include "Settings/SettingsStore.h"

#include <string>
#include <vector>

namespace Settings
{
    struct LegacySpelling
    {
        std::string flag;            // with its '-'
        bool        constant = false;
        std::string text;            // "-x=<v>": <v>
    };
    std::vector<LegacySpelling> SplitLegacy(const char* spec);

    enum class LegacyStyle : uint8_t { Pass, Developer };
    const char* LegacyResultName(LegacyResult r);   // "absent" / "taken" / "ignored" / "refused"

    // The row's old spellings off the command line (.legacyFlag; for a row with a
    // .legacyRead hook the caller calls the hook instead). Taken: `out` holds the value
    // and `writer` the spelling's flag. Ignored: given but no usable value (the pass
    // style's kSettingLegacyValueOnly; the developer style's number, `why` the warning).
    // Refused: a value the row refuses; `why` says which.
    template <class S>
    LegacyResult ReadLegacy(const RowT<S>& r, LegacyStyle style, Value& out, std::string& writer, std::string* why);

    // A row's .legacyFlag as written: every spelling a '-' flag, every constant
    // a value the row takes, not both .legacyFlag and .legacyRead. Empty when
    // well formed; each table checks its rows with it when it is built.
    template <class S>
    std::string LegacySpecError(const RowT<S>& r);
}
