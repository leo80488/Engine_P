#include "Scene/SceneNavMesh.h"

namespace SceneNavMesh
{
namespace
{
    Provider    s_provider;
    std::string s_scenePath;
}

void Install(const Provider& provider)
{
    s_provider = provider;
}

bool HasProvider()
{
    return s_provider.load != nullptr;
}

bool LoadForScene(const std::string& path)
{
    s_scenePath = path;
    return !s_provider.load || s_provider.load(path);
}

std::string Path()
{
    return s_provider.sourcePath ? s_provider.sourcePath() : s_scenePath;
}
}
