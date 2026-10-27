#pragma once
// -profile*: start the engine profiler from the command line.

class IGraphicsDevice;

namespace Diag::Prof
{
    // A malformed or unknown -profile* flag refuses the run before a frame is drawn
    // (PostQuitMessage(PreconditionFail)) - a typo would otherwise profile nothing and exit 0. A valid
    // one starts the collector, and a GPU capture holds a GPU-profiler want on @p backend for the
    // process.
    void StartFromCommandLine(IGraphicsDevice& backend);
}
