#pragma once

// TextureViewPass - paints one Texture Viewer picture into a panel-owned target
// (Renderer/TextureView.h) with shaders/TextureView.cs.hlsl, one dispatch per view
// that changed.
//
// Outside the render graph, like DebugTargetViewPass: TextureViewService runs it on
// the frame's restore command list, right before the debug target view (the same
// known-legal point), and only for a view whose description, size or source contents
// changed since its last dispatch. A frame with no dirty view records nothing; a run
// that never opens the viewer builds nothing.
//
// The source is read bindless through its own default SRV (the heap index is
// recomputed every recording, never cached - IGraphicsDevice.h), typed by
// TextureView::KindOf; the output moves SHADER_RESOURCE -> UNORDERED_ACCESS ->
// SHADER_RESOURCE around the dispatch, so ImGui samples it as usual.
//
// Several views can draw in one frame (the viewer's tab, the Content Browser's cube
// thumbnails), so the constants are a ring of 256-byte slots per frame in flight -
// kMaxPerFrame pictures then kMaxProbesPerFrame probes - bound by offset (the compute
// root CBV at b0 space2).
//
// Probes: ProbeCS reads one texel of a view's shown mip / slice (the value the
// shaders see, the hardware's BC decode included) into a UAV with one float4 per
// view; the batch is copied into this frame's READBACK slot. The service reads slot
// GetFrameIndex() at the top of the next Execute that lands on it: the slot
// BeginFrame just fenced (LumaReducePass's rule).

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Renderer/TextureView.h"

#include <cstdint>

class IGraphicsDevice;

class TextureViewPass
{
public:
    static constexpr std::uint32_t kMaxPerFrame       = 16;
    static constexpr std::uint32_t kMaxProbesPerFrame = 8;
    static constexpr std::uint32_t kProbeSlots        = 64;   // = TextureViewService::kMaxViews

    struct Job
    {
        const RHI::Texture*     output = nullptr;   // RGBA16F, SHADER_RESOURCE | UNORDERED_ACCESS
        std::uint64_t           sourceSrv = 0;      // the source's default SRV (GPU handle); 0 = none
        TextureView::SourceKind kind = TextureView::SourceKind::Unsupported;
        std::uint32_t           mipW = 0, mipH = 0; // the shown mip's size
        bool                    srgb = false;       // the source format is sRGB
        bool                    oneChannel = false; // R-only source format
        TextureView::Desc       desc;
        // Compare: picture B (desc.compare != Off), resolved like the source.
        std::uint64_t           sourceSrvB = 0;
        TextureView::SourceKind kindB = TextureView::SourceKind::Unsupported;
        std::uint32_t           mipWB = 0, mipHB = 0;
        bool                    srgbB = false, oneChannelB = false;
    };

    struct Probe
    {
        Job           job;                  // output unused
        std::uint32_t x = 0, y = 0;         // a texel of job.desc's mip (and slice / face)
        std::uint32_t index = 0;            // the result's place (< kProbeSlots)
    };

    void Init(IGraphicsDevice& gfx) { m_gfx = &gfx; }   // builds nothing
    // Build the pipelines, the constant ring and the probe buffers - once; a
    // failure is not retried until ReloadShaders. Serial window (lane parked).
    bool EnsurePipeline();
    bool IsReady() const { return m_ready; }
    void ReloadShaders();
    void Shutdown();

    // Record one picture into @p slot (0 .. kMaxPerFrame-1) of this frame's
    // constant ring. False when nothing was recorded.
    bool Execute(RHI::CommandList cl, std::uint32_t slot, const Job& job);

    // Record up to kMaxProbesPerFrame probes and copy the results into THIS
    // frame's readback slot. Returns how many were recorded (0 = no copy made).
    std::uint32_t ExecuteProbes(RHI::CommandList cl, const Probe* probes, std::uint32_t count);
    // kProbeSlots float4 as a frame slot's copy left them: valid for the slot
    // BeginFrame fenced (GetFrameIndex() on the render half), read BEFORE this
    // frame's ExecuteProbes overwrites it.
    const float* ProbeResults(std::uint32_t frameSlot) const;

private:
    struct alignas(16) CBData
    {
        std::uint32_t dstW, dstH, mipW, mipH;
        float         uvOrigin[2], uvPerPixel[2];
        std::uint32_t srcIndex, srcKind, mip, slice;
        std::uint32_t channelMask, mapMode, background, flags;
        float         exposure, rangeMin, rangeMax;
        std::uint32_t cubeLayout;
        float         bgColor[4];
        // ProbeCS's texel and result index: a 16-byte row appended at the end.
        std::uint32_t probeX, probeY, probeIndex, probePad;
        // Picture B of a comparison: three rows appended.
        std::uint32_t mipWB, mipHB, srcIndexB, srcKindB;
        std::uint32_t mipB, sliceB, flagsB, compareMode;
        float         splitX, diffScale, comparePad[2];
    };
    static_assert(sizeof(CBData) == 160, "must mirror TextureViewCB in shaders/TextureView.cs.hlsl");
    static constexpr std::uint32_t kSlotBytes     = 256;
    static constexpr std::uint32_t kSlotsPerFrame = kMaxPerFrame + kMaxProbesPerFrame;

    bool CreatePipelines();
    // The job's constants into ring slot @p slot of this frame; its byte offset,
    // or ~0u when the job has nothing to read (no source).
    std::uint32_t WriteConstants(std::uint32_t slot, const Job& job, std::uint32_t outW, std::uint32_t outH,
                                 std::uint32_t probeX, std::uint32_t probeY, std::uint32_t probeIndex);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    RHI::PipelineState m_probePso;
    RHI::GPUBuffer     m_ring;              // kFramesInFlight * kSlotsPerFrame slots
    std::uint8_t*      m_ringMapped = nullptr;
    RHI::GPUBuffer     m_probeScratch;      // kProbeSlots float4, UAV
    RHI::ResourceState m_probeState = RHI::ResourceState::UNDEFINED;
    RHI::GPUBuffer     m_probeReadback[RHI::kFramesInFlight];
    const float*       m_probeMapped[RHI::kFramesInFlight] = {};
    bool               m_ready  = false;
    bool               m_failed = false;    // EnsurePipeline failed; cleared by ReloadShaders
};
