#pragma once

// Graphics/MeshPsoStream.h - the pipeline state stream every mesh-shader PSO is built from. One
// type for the device's create and the boot precompile's replay: a library LoadPipeline matches the
// stored pipeline's stream desc, so both must hand D3D the same subobjects in the same order.

#include "d3dx12.h"

struct MeshPsoStream
{
    CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE        rootSig;
    CD3DX12_PIPELINE_STATE_STREAM_AS                    as;
    CD3DX12_PIPELINE_STATE_STREAM_MS                    ms;
    CD3DX12_PIPELINE_STATE_STREAM_PS                    ps;
    CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC            blend;
    CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER            raster;
    CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL         depth;
    CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT  dsFormat;
    CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS rtFormats;
    CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC           sample;
    CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK           sampleMask;
    CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY    topology;
};
