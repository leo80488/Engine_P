#include "Scripting/LuaDiag.h"

#include <unordered_set>
#include <string>

namespace LuaDiag
{
namespace
{
    // Keyed by the ADDRESS of a static site string, so lookup is a pointer
    // hash and the suppressed path does no string work at all.
    std::unordered_set<const void*> g_reported;
    // Content-keyed companion for runtime names (BT action names etc.), where
    // no stable code address exists to key on.
    std::unordered_set<std::string> g_reportedNamed;
}

bool ShouldReport(const void* siteKey)
{
    return g_reported.insert(siteKey).second;
}

bool ShouldReportNamed(const std::string& key)
{
    return g_reportedNamed.insert(key).second;
}

void ResetOnce()
{
    g_reported.clear();
    g_reportedNamed.clear();
}
}
