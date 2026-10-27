#include "Scripting/ScriptOwner.h"
#include "System/CommandLine.h"

#include <utility>

namespace Scripting
{
    namespace
    {
        std::function<OwnerToken()>                              s_current;
        std::function<void(OwnerToken, std::function<void()>)>   s_run;
    }

    OwnerToken CurrentOwner()
    {
        return s_current ? s_current() : 0;
    }

    void RunAsOwner(OwnerToken owner, std::function<void()> fn)
    {
        if (!fn) return;
        if (LegacyTimers()) owner = 0;
        if (s_run) s_run(owner, std::move(fn));
    }

    void SetOwnerBackend(std::function<OwnerToken()> current,
                         std::function<void(OwnerToken, std::function<void()>)> run)
    {
        s_current = std::move(current);
        s_run     = std::move(run);
    }

    bool LegacyTimers()
    {
#if WITH_HARNESS
        static const bool s_legacy = Sys::CommandLine::Get().Has("-timers_legacy");
        return s_legacy;
#else
        return false;
#endif
    }
}
