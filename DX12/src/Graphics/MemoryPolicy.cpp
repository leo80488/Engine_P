#include "Graphics/MemoryPolicy.h"

#include "System/CommandLine.h"

namespace MemoryPolicy
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    namespace
    {
        bool Flag(const char* name) { return Sys::CommandLine::Get().Has(name); }
    }

    bool EagerTransients()
    {
        static const bool s_on = Flag("-mem_eager_transients");
        return s_on;
    }

    bool ViewTransientsCommitted()
    {
        static const bool s_on = Flag("-mem_view_committed");
        return s_on;
    }

    bool KeepViewHeaps()
    {
        static const bool s_on = Flag("-mem_keep_view_heaps");
        return s_on;
    }

    bool IgnoreViewBudget()
    {
        static const bool s_on = Flag("-mem_no_view_budget");
        return s_on;
    }

    bool NoResidencyPriority()
    {
        static const bool s_on = Flag("-mem_no_residency_priority");
        return s_on;
    }
#else
    bool EagerTransients() { return false; }
    bool ViewTransientsCommitted() { return false; }
    bool KeepViewHeaps() { return false; }
    bool IgnoreViewBudget() { return false; }
    bool NoResidencyPriority() { return false; }
#endif
}
