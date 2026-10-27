#include "Harness/PakOrder.h"

#ifdef WITH_HARNESS

#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <string>
#include <vector>

namespace Harness
{
void EnablePakOrderIfRequested()
{
    // -pak_order[=<file>]: record the order assets are first read in, the pak layout of a package.
    if (Sys::CommandLine::Get().Has("-pak_order")) Resource::AssetFS::Get().RecordReadOrder(true);
}

void WritePakOrderIfRequested()
{
    auto& cl = Sys::CommandLine::Get();
    if (!cl.Has("-pak_order")) return;
    std::string file = cl.Get("-pak_order");
    if (file.empty())
    {
        if (!Sys::HasArtifactDir())
        {
            LOG_WARNING("-pak_order needs -artifacts=<dir> or a file name; nothing written");
            return;
        }
        file = Sys::ArtifactDir() + "/pak_order.txt";
    }
    const std::vector<std::string> order = Resource::AssetFS::Get().ReadOrder();
    std::string text;
    for (const std::string& key : order)
    {
        text += key;
        text += '\n';
    }
    if (!Sys::WriteFileBytes(file, text.data(), text.size()))
    {
        LOG_ERROR("-pak_order: cannot write '%s'", file.c_str());
        return;
    }
    LOG_INFO("[PAK_ORDER] keys=%zu file=%s", order.size(), file.c_str());
}
} // namespace Harness

#endif // WITH_HARNESS
