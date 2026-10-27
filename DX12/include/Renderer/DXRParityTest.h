#pragma once

// DXRParityTest - the DXR pipeline's one caller.
//
// Every ray the engine traces is an inline RayQuery in a compute pass. The DXR state-object path
// (IGraphicsDevice::CreateRaytracingPipeline / DispatchRays) is kept and proved here instead of
// used: with -dxr_parity, once the scene TLAS exists, one grid of camera rays is traced twice
// against it - through a DXIL library (shaders/DXRParity.lib.hlsl) and through an inline-RayQuery
// compute kernel (shaders/DXRParity.cs.hlsl) - and a few frames later the closest hits are
// compared on the CPU: the hit / miss verdict, InstanceID and PrimitiveIndex must match ray for
// ray, T within a relative tolerance. One [DXR] parity line; tests/unit/test_dxr_parity.py reads
// it. Without the flag nothing is made.

#include "Graphics/GraphicsStruct.h"

#include <cstdint>

class IGraphicsDevice;
struct RenderView;

class DXRParityTest
{
public:
    // -dxr_parity, read once.
    static bool Wanted();

    // Once per frame, after the scene acceleration-structure build, on that
    // compute list: records the two traces once the TLAS has existed for
    // kSettleFrames frames, then reads them back and logs when the GPU has run
    // them, and releases everything. @p tlasAddress is 0 when there is no TLAS.
    void Tick(IGraphicsDevice& gfx, RHI::CommandList cmd, uint64_t tlasAddress, const RenderView& view);

    // Releases whatever a run left (the engine quit between record and read).
    void Shutdown(IGraphicsDevice& gfx);

private:
    enum class State : uint8_t { Waiting, Recorded, Done };

    static constexpr uint32_t kWidth         = 64;
    static constexpr uint32_t kHeight        = 64;
    static constexpr uint32_t kRays          = kWidth * kHeight;
    static constexpr uint32_t kSettleFrames  = 60;   // frames with a TLAS before recording (streaming settles)
    static constexpr uint32_t kReadbackDelay = 5;    // > kFrameCount: the recorded list has run

    bool Prepare(IGraphicsDevice& gfx);   // the layout, both pipelines and the buffers
    void Record(IGraphicsDevice& gfx, RHI::CommandList cmd, uint64_t tlasAddress, const RenderView& view);
    void Compare(IGraphicsDevice& gfx);

    State    m_state             = State::Waiting;
    uint32_t m_framesWithTlas    = 0;
    uint32_t m_framesSinceRecord = 0;

    RHI::PipelineLayout     m_layout;
    RHI::PipelineState      m_queryPSO;      // DXRParity.cs.hlsl
    RHI::RaytracingPipeline m_dxr;           // DXRParity.lib.hlsl
    RHI::GPUBuffer          m_rays;          // UPLOAD: kRays rays, written once on the CPU
    RHI::GPUBuffer          m_hits[2];       // DEFAULT UAV: [0] DXR, [1] RayQuery
    RHI::GPUBuffer          m_readback[2];   // READBACK copies of m_hits
};
