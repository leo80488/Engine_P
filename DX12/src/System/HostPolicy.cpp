#include "System/HostPolicy.h"

#ifdef WITH_HARNESS
#include "System/CommandLine.h"
#endif

namespace Sys
{
    namespace
    {
        // Written once from main.cpp during startup, before any worker thread or
        // frame exists; read-only from then on, so a plain bool needs no ordering.
        bool g_isEditorHost = false;
    }

    void SetEditorHost(bool isEditorHost)
    {
        g_isEditorHost = isEditorHost;
    }

    bool IsEditorHost()
    {
        return g_isEditorHost;
    }

    bool HotReloadEnabled()
    {
        if (g_isEditorHost) return true;
#ifdef WITH_HARNESS
        // Read once, on first use (a frame, long after main parsed the command line).
        static const bool s_flag = Sys::CommandLine::Get().Has("-hotreload");
        return s_flag;
#else
        return false;
#endif
    }
}
