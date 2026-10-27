#include "Platform/BackgroundPolicy.h"

namespace Plat
{
BackgroundPolicy& CurrentBackgroundPolicy()
{
    static BackgroundPolicy s_policy;
    return s_policy;
}
}
