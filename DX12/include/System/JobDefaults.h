#pragma once
#include "System/CommandLine.h"

namespace Sys {
// User-selected all-jobs preset, independent of WITH_HARNESS. It changes
// defaults only: explicit subsystem rollback switches and safety gates remain.
// -jobdefaultsoff turns the preset off (the subsystems' own defaults, not a
// serial engine).
inline bool JobSplittingDefaultsEnabled()
{
    static const bool s_enabled = !CommandLine::Get().Has("-jobdefaultsoff");
    return s_enabled;
}
}
