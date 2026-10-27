#pragma once

// ComputePipelineBatch - create several compute pipelines at once, through the RHI.
//
// Creating a pipeline from a large kernel is the driver compiling it: RTGI's RayQuery
// Trace kernel costs ~270 ms on every run, and feeding back the PSO's cached blob does
// not shorten it. Built one after another in the first frame, RTGIPass took ~450 ms
// and RTDIPass ~230 ms. IGraphicsDevice::CreatePipelineStates builds a batch
// concurrently and returns when all are done, so the wall time is the slowest pipeline,
// not the sum.
//
// The caller loads the kernels first (ShaderDiskCache, serially) and keeps the bytes
// alive across the call.

#include "Graphics/GraphicsStruct.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class IGraphicsDevice;

namespace ComputePipelineBatch
{
    // Creates outs[i] from kernels[i], all on @p layout, together. A pipeline an out
    // already held is destroyed first. All or nothing: on any failure every outs[i] is
    // destroyed and false is returned; @p failed, when given, receives @p count flags
    // marking the failed entries (an empty kernel is one).
    bool Create(IGraphicsDevice& gfx, const RHI::PipelineLayout& layout,
                const std::vector<uint8_t>* kernels, size_t count,
                RHI::PipelineState* outs, bool* failed = nullptr);

    // The common shape: @p count entry points of one HLSL source on one layout. Loads
    // each kernel through ShaderDiskCache::LoadOrCompile in order, then creates them
    // together. On any failure it logs "<logTag>: pipeline <entry> failed" per failed
    // entry and destroys every outs[i].
    bool BuildEntries(IGraphicsDevice& gfx, const RHI::PipelineLayout& layout, const char* hlslPath,
                      const char* const* entries, size_t count,
                      RHI::PipelineState* outs, const char* logTag);
}
