#pragma once

// Graphics/PsoPrepare.h - the boot precompile's policy: whether this run builds every recorded
// pipeline before its first scene, from which manifest, on how many threads, and what the loading
// screen says meanwhile. The work itself is IGraphicsDevice::StartPipelinePrecompile; the manifest
// is Graphics/PsoManifest.h's.
//
//   -pso_prepare=auto|force|off   auto: prepare when a manifest ships and this PSO library was not
//                                 prepared from it on this adapter and driver; force: prepare even
//                                 so (a library already holding a pipeline skips its build); off:
//                                 never. The default is auto for a game host, off for the editor
//                                 and for an unattended run (a pack measures its own boot).
//   -pso_manifest=<file>          the manifest to prepare from (default: pso_manifest.bin beside
//                                 the exe, else in game.ipak)
//   -pso_threads=N                worker threads (default: the logical cores less one)
//   -pso_miss=skip|stall          once prepared, a pipeline the library lacks: built in the background
//                                 with its draws skipped meanwhile (skip: a shipped game's default), or
//                                 built where it is asked for (stall: a harness build's default)
//
// App calls Begin after InitPSOLibrary and Poll once a frame; while Active, the boot scene waits
// behind the scene transition's loading screen, which shows Progress and Message.

#include <string>

class IGraphicsDevice;

namespace Gfx::PsoPrepare
{
    // Starts the precompile when the policy and the manifest say so. @p cacheFilePath: the PSO
    // library's file; the index of what it was prepared from lives beside it.
    void Begin(IGraphicsDevice& device, const std::string& cacheFilePath);
    // Once a frame: when the workers are done, saves the library, writes the index and ends.
    void Poll(IGraphicsDevice& device);
    // At shutdown while still running: the pipelines not started are skipped, the built ones saved.
    void Cancel(IGraphicsDevice& device);

    bool  Active();
    float Progress();   // 0..1
    // The loading screen's line, in the player's ui.language ("ja", "zh-*"; English otherwise).
    std::string Message();
}
