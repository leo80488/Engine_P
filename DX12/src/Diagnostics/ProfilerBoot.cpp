#include "Diagnostics/ProfilerBoot.h"

#include "Diagnostics/ProfilerCollector.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"
#include "Harness/ExitCodes.h"

#include <string>

// -profile*: starts the engine profiler; an unknown or malformed flag refuses the run.
void Diag::Prof::StartFromCommandLine(IGraphicsDevice& backend)
{
    Diag::Prof::CollectorConfig profCfg;
    std::string profError;
    if (!Diag::Prof::ParseCommandLine(profCfg, profError))
    {
        LOG_ERROR("[PROFILER] %s - refusing to run.", profError.c_str());
        PostQuitMessage(Harness::PreconditionFail);
    }
    else
    {
        Diag::Prof::StartCollector(profCfg);
        // The GPU half of the timeline needs the timestamp profiler. A want
        // (not a write to GPUProfiler::enabled) so the editor's Profiler
        // window closing cannot switch it off underneath the capture; held
        // for the life of the process.
        if (profCfg.level != Diag::Prof::Level::Off && profCfg.gpu)
            backend.AddGPUProfilerWant();
    }
}
