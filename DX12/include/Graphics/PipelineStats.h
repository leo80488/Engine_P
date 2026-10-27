#pragma once

// First-compile observability: running totals since boot that frames.jsonl carries
// on every row (HarnessCaptureSystem). A row where one moves paid for a compile or a
// pipeline creation on that frame - the hitch an "everything is warm after frame N"
// assertion is about (scenario_run.py frames.psoCreatesAfterFrame).
//
// Totals, not per-frame counts, so a -framelog_every=N run that skips rows loses
// nothing: the difference between two rows is what happened between them. Relaxed
// atomics - shaders compile and PSOs build on render workers.

#include <atomic>
#include <cstdint>

namespace PipelineStats
{
    // DXC compiles (DxcCompiler::Compile): shaders built from source because
    // no current .ishdr existed - a cache hit is not a compile.
    inline std::atomic<uint32_t> shaderCompiles{ 0 };

    // Pipeline states the DRIVER built inside the device (IGraphicsDevice::CreatePipelineState
    // and the compute PSO path): a PSO the PSO library did not hold. A library load is
    // not counted, nor the present PSO that SurfaceDX12::BuildPresentPSO creates directly.
    inline std::atomic<uint32_t> psoCreates{ 0 };

    // PSO library lookups that missed (GraphicsDX12's m_psoLibMisses, mirrored
    // here so a reader needs no device).
    inline std::atomic<uint32_t> psoLibMisses{ 0 };

    // ShaderLibrary in-memory cache misses, every library and every thread
    // (ShaderLibrary::GetShaderInternal): an (id, permutation) a library loaded for the
    // first time - the .ishdr load or a DXC compile, then reflection, //@ annotations
    // and the binding checks. A compile is a miss too, so this is >= shaderCompiles. A
    // prefab whose shaders were warmed before its spawn adds none on the spawn's frames.
    inline std::atomic<uint32_t> shaderMisses{ 0 };

    // PSOCache in-memory misses that BUILT a pipeline, every pass and every thread
    // (PSOCache::GetOrCreate): a driver create or a PSO library load - either way a
    // pipeline object made on that frame. psoCreates counts only the driver's half, so a
    // run with a warm PSO library sees 0 there while a pass still builds its first PSO
    // of a material on its record worker. A preloaded prefab's spawn window adds none.
    inline std::atomic<uint32_t> psoCacheMisses{ 0 };
}
