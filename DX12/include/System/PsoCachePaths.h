#pragma once
// Where the PSO disk library is read from and written to; the policy and its reasons
// are in the .cpp.

#include <string>

namespace Sys
{
    // Fills @p psoCacheWritePath with the writable root's pso_cache.bin (written at exit)
    // and @p psoCacheShippedPath with the install root's pre-warmed copy (read when the
    // writable one is absent). The harness flag -pso_cache_dir replaces both.
    void ResolvePsoCachePaths(std::string& psoCacheWritePath, std::string& psoCacheShippedPath);
}
