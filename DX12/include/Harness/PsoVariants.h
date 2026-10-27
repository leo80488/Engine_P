#pragma once
// Harness/PsoVariants.h - -pso_variants[=<file>]: every pipeline this run built or loaded, written as a
// PSO manifest (Graphics/PsoManifest.h) when the run ends - what a package's boot precompile replays
// (Graphics/PsoPrepare.h). The file defaults to <artifacts>/pso_manifest.bin; a run without -artifacts
// must name one. tools/pso_manifest.py merges runs into the manifest a package ships. Logs
// `[PSO_VARIANTS] records= blobs= bytes= file=`.

namespace Harness
{
    // RunLedger::Begin: the recorder must be on before the device builds its first pipeline.
    void EnablePsoVariantsIfRequested();
    // RunLedger::Finalize, after the teardown (the recorder forgets nothing).
    void WritePsoVariantsIfRequested();
}
