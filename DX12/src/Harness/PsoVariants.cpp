#include "Harness/PsoVariants.h"

#ifdef WITH_HARNESS

#include "Graphics/PsoManifest.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <string>
#include <vector>

namespace Harness
{
void EnablePsoVariantsIfRequested()
{
    if (Sys::CommandLine::Get().Has("-pso_variants")) Gfx::PsoManifest::EnableRecording();
}

void WritePsoVariantsIfRequested()
{
    auto& cl = Sys::CommandLine::Get();
    if (!cl.Has("-pso_variants")) return;
    std::string file = cl.Get("-pso_variants");
    if (file.empty())
    {
        if (!Sys::HasArtifactDir())
        {
            LOG_WARNING("-pso_variants needs -artifacts=<dir> or a file name; nothing written");
            return;
        }
        file = Sys::ArtifactDir() + "/pso_manifest.bin";
    }
    const Gfx::PsoManifest::Manifest manifest = Gfx::PsoManifest::Recorded();
    const std::vector<std::uint8_t> bytes = Gfx::PsoManifest::Serialize(manifest);
    if (!Sys::WriteFileBytes(file, bytes.data(), bytes.size()))
    {
        LOG_ERROR("-pso_variants: cannot write '%s'", file.c_str());
        return;
    }
    LOG_INFO("[PSO_VARIANTS] records=%zu blobs=%zu bytes=%zu file=%s", manifest.records.size(),
             manifest.blobs.size(), bytes.size(), file.c_str());
}
} // namespace Harness

#endif // WITH_HARNESS
