#pragma once
// Harness::RequireValue - the one refusal for a flag given without a value,
// shared by the features that parse their own flags. Callers wrap it in a local
// lambda named needValue, so the flag catalog (gen_catalog) lists those flags
// 'via needValue'.

#include "Harness/ExitCodes.h"
#include "System/CommandLine.h"
#include "System/Log.h"

namespace Harness
{
    // One shape for "this flag needs a value": a BARE -preview_* or -chainpreview_*
    // flag IS A HARD PRECONDITION FAILURE, exit 12. Do not make it an ENGINE_CHECK:
    // an ENGINE_CHECK refuses nothing - it logs, returns false, and the run CONTINUES
    // with the feature off (exit 0, engineErrors 1), so only a pack's own
    // engineErrors budget would turn it red, and the rule "every -preview_* flag
    // takes a value; a bare one is refused" would hold for some flags only.
    //
    // A MALFORMED value is a different thing and stays an ENGINE_CHECK: the flag
    // names a knob whose documented fallback is a default the log reports, not a
    // feature that silently fails to exist.
    inline void RequireValue(const Sys::CommandLine& pcl, const char* flag, const char* usage)
    {
        if (!pcl.Has(flag) || !pcl.Get(flag).empty()) return;
        LOG_ERROR("%s needs a value (%s). Refusing to run: a bare flag "
                  "leaves the feature it names OFF, and the pack would "
                  "measure the null case under its own name.",
                  flag, usage);
        PostQuitMessage(Harness::PreconditionFail);
    }
}
