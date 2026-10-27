#include "System/BuildInfo.h"

// tools/gen_build_info.py's values: the generated folder comes first on this file's include path
// (EngineCore.vcxproj), include/System's stand-in after it.
#include "System/BuildInfoValues.h"

#include <cstring>

namespace Sys
{
    const BuildInfo& Build()
    {
        static const BuildInfo s_build{ BUILDINFO_PRODUCT, BUILDINFO_VERSION, BUILDINFO_GIT_SHA, BUILDINFO_DIRTY != 0,
                                        BUILDINFO_BRANCH, BUILDINFO_CONFIG, BUILDINFO_BUILD_ID };
        return s_build;
    }

    const char* ProductTitle()
    {
        const char* product = Build().product;
        return product && *product && std::strcmp(product, "unknown") != 0 ? product : "DX12 Engine";
    }
}
