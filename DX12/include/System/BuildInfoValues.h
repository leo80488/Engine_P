// The stand-in for tools/gen_build_info.py's output when it has not run (no Python on the build
// machine): the generated BuildInfoValues.h in <BuildRoot><Platform>/<Config>/generated/System/ comes
// first on src/System/BuildInfo.cpp's include path.
#pragma once
#define BUILDINFO_PRODUCT  "Game"
#define BUILDINFO_VERSION  "0.0.0"
#define BUILDINFO_MAJOR    0
#define BUILDINFO_MINOR    0
#define BUILDINFO_PATCH    0
#define BUILDINFO_GIT_SHA  "unknown"
#define BUILDINFO_DIRTY    0
#define BUILDINFO_BRANCH   "unknown"
#define BUILDINFO_CONFIG   "unknown"
#define BUILDINFO_BUILD_ID "0000000000000000"
