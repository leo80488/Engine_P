#pragma once
// Diagnostics/GpuCounters.h - what command lists recorded through the graphics device's own entry
// points (the profiler's pass cost table). Counted on each command list by GraphicsDX12, summed onto
// a GPUProfiler region - a render graph pass gets every list it recorded, its chunk lists included -
// and written with the region's span to the profiler's passes.jsonl. Plain counts, no dependencies.
#include <cstdint>

namespace Diag::Prof
{
    struct GpuCounters
    {
        uint32_t draws          = 0;   // DrawInstanced + DrawIndexedInstanced
        uint32_t drawsIndirect  = 0;   // ExecuteIndirect draw calls (each up to its max count on the GPU)
        uint32_t dispatches     = 0;   // Dispatch, and DispatchComputeIndirect's ExecuteIndirect calls
        uint32_t dispatchesIndirect = 0;   // of those, the indirect ones
        uint32_t dispatchesMesh = 0;   // DispatchMesh, and its ExecuteIndirect calls
        uint32_t dispatchesRays = 0;   // DispatchRays
        uint32_t psoBinds       = 0;   // SetPipelineState
        uint32_t barriers       = 0;   // D3D12_RESOURCE_BARRIERs the device issued
        // GetNativeCommandList calls: a recorder holding the native list can
        // record what the device does not count, so the counts may be short.
        uint32_t nativeTakes    = 0;
        bool     valid          = false;   // a region the device counted

        GpuCounters& operator+=(const GpuCounters& o) noexcept
        {
            draws += o.draws;               drawsIndirect += o.drawsIndirect;
            dispatches += o.dispatches;     dispatchesMesh += o.dispatchesMesh;
            dispatchesIndirect += o.dispatchesIndirect;
            dispatchesRays += o.dispatchesRays;
            psoBinds += o.psoBinds;         barriers += o.barriers;
            nativeTakes += o.nativeTakes;   valid = valid || o.valid;
            return *this;
        }
        // What was counted since @p begin (a snapshot of the same list).
        GpuCounters Since(const GpuCounters& begin) const noexcept
        {
            GpuCounters d;
            d.draws          = draws - begin.draws;
            d.drawsIndirect  = drawsIndirect - begin.drawsIndirect;
            d.dispatches     = dispatches - begin.dispatches;
            d.dispatchesIndirect = dispatchesIndirect - begin.dispatchesIndirect;
            d.dispatchesMesh = dispatchesMesh - begin.dispatchesMesh;
            d.dispatchesRays = dispatchesRays - begin.dispatchesRays;
            d.psoBinds       = psoBinds - begin.psoBinds;
            d.barriers       = barriers - begin.barriers;
            d.nativeTakes    = nativeTakes - begin.nativeTakes;
            d.valid          = true;
            return d;
        }
    };
}
