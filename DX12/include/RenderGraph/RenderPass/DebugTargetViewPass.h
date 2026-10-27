#pragma once

// DebugTargetViewPass - paints one render target over the main view's final output
// (Renderer/DebugTargets.h).
//
// Outside the render graph, like GlassShatterPass: the Renderer runs it on the
// frame's restore command list after ToneMapPass::TransitionForDisplay - after the
// whole post-process chain, before the overlays (debug wires, world UI, icons, the
// HUD) - and only while a target is asked for. One compute dispatch over the output
// (shaders/DebugTargetView.cs.hlsl).
//
// Nothing is built until a target is first asked for: EnsurePipeline runs at the
// frame boundary's service point (Renderer::ServiceEnableGatedResources, render lane
// parked), so a run that never shows one pays nothing.
//
// Producers: a target no pass writes (shadingModel, lightComplexity, ...) is made by
// this pass, one compute dispatch each (DebugTargetProduce.cs.hlsl), right before the
// paint and only on the frames that show or capture it; its texture is made at the
// service point. A frame that asks for none adds no graph pass, transient or texture.
//
// State: the output and the source are each moved to what the dispatch needs
// (UNORDERED_ACCESS / a shader-resource state) and put back to the exact state they
// came in with, so the callers' trackers (ToneMapPass, the render graph, the pass
// that owns the source) stay correct without being told anything.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/FrameConstants.h"   // RHI::kFramesInFlight
#include "Renderer/DebugTargets.h"

#include <cstdint>

class IGraphicsDevice;

class DebugTargetViewPass : public RG::PassLifecycle
{
public:
    struct ServiceRequest
    {
        bool          wanted = false;
        std::uint32_t produce = 0, renderW = 0, renderH = 0;
        std::uint32_t materialCapacity = 0;
    };

    // The target to read. @p srv is its SRV (0 = not there this frame: the
    // pass paints the unavailable pattern). @p texture / @p state, when set,
    // let the pass make it readable and put it back; null = the caller did
    // (the device's HDR target, whose state the device tracks).
    struct Source
    {
        const RHI::Texture* texture = nullptr;
        RHI::ResourceState  state   = RHI::ResourceState::SHADER_RESOURCE;
        std::uint64_t       srv     = 0;
        std::uint32_t       width   = 0, height = 0;
        // A uint target: 1 = its channels, 2 = a stencil plane's byte (.y), read through
        // @p uintHandle (a Tex2DUint handle); t0 then holds the fallback. 0 = the float SRV.
        std::uint32_t       uintMode = 0, uintHandle = 0;
    };
    struct Params
    {
        DebugTargets::Mode mode     = DebugTargets::Mode::Raw;   // never Auto here
        int                channel  = 0;
        float              rangeMin = 0.f, rangeMax = 1.f;
        float              exposure = 1.f, scale = 100.f;
        float              nearZ    = 0.1f, farZ = 1000.f;
        bool               orthographic = false;
        std::uint32_t      mask     = 0xFFFFFFFFu;   // Categorical / Bits
    };
    // Whose constants a paint uses: one FrameCB per painter, each written once a frame -
    // the main view's (View) and the pass capture's picture (Preview,
    // Renderer/PassCapture.h), which can paint in the same frame.
    enum class Slot : std::uint8_t { View = 0, Preview = 1 };

    // ---- Producers: the debug targets no pass writes ----
    enum class Produced : std::uint32_t { ShadingModel = 0, LightComplexity, Overdraw, Density, MaterialCost, ShadingCost,
                                          Count };

    struct ProduceInputs
    {
        const RHI::Texture* normal = nullptr;   // GBuffer1 (.a the matIdx)
        RHI::ResourceState  normalState = RHI::ResourceState::SHADER_RESOURCE;
        const RHI::Texture* depth = nullptr;    // the main depth (0 = nothing drawn)
        RHI::ResourceState  depthState = RHI::ResourceState::SHADER_RESOURCE;
        // 0 = the round trip moves the depth plane alone (a depth-stencil texture whose
        // planes the graph tracks apart); -1 every plane.
        int                 depthPlane = -1;
        std::uint64_t       materialSrv   = 0;  // the frame's material table
        std::uint32_t       materialCount = 0;
        // lightComplexity: the camera's near / far (the cluster grid's) and the grids this
        // frame wrote, as bindless StructuredBuffer handles.
        float               nearZ = 0.1f, farZ = 1000.f;
        bool                orthographic = false;
        std::uint32_t       gridFlags = 0;   // kGrid* below
        std::uint32_t       lightGrid = 0, decalGrid = 0, probeGrid = 0;
    };
    static constexpr std::uint32_t kGridLights = 1u, kGridDecals = 2u, kGridProbes = 4u;

    void Init(IGraphicsDevice& gfx) { m_gfx = &gfx; }   // builds nothing
    // Build the pipeline, constants and fallback texture - once; a failure
    // is not retried until ReloadShaders. Main thread, render lane parked.
    bool EnsurePipeline();

    ServiceRequest& Request() { return m_request; }
    // What OnService builds, filled by the Renderer: the pipeline the first frame a
    // target is asked for (wanted), kept once built; the textures of the producers this
    // frame runs (produce, Bit()s of Produced) at the render size; the material cost
    // table (materialCapacity slots) when one of them reads it.
    void OnService(RG::ServiceContext&) override;
    bool IsReady() const { return m_ready; }
    void ReloadShaders(IGraphicsDevice& gfx) override;   // Renderer::ReloadShaders
    uint32_t HandledEvents() const override { return kReloadShaders; }

    // False when nothing was painted (not built, no output).
    bool Execute(RHI::CommandList cl, const Source& src,
                 const RHI::Texture& output, RHI::ResourceState outputState,
                 std::uint32_t outW, std::uint32_t outH, const Params& p, Slot slot = Slot::View);

    // DebugTargetProduce.cs.hlsl. Made on the frames a view shows them or a capture names
    // them (Renderer_DebugTargets.cpp), right before the paint; their textures are made
    // at the service point.
    static constexpr std::uint32_t Bit(Produced k) { return 1u << static_cast<std::uint32_t>(k); }

    // Make @p kind's texture this frame (the render half). The inputs are put
    // back in the states they came in; the texture is left SHADER_RESOURCE.
    // False when nothing was made (not built, no texture yet, an input missing).
    bool Produce(RHI::CommandList cl, Produced kind, const ProduceInputs& in, std::uint64_t frame);
    // @p kind's texture as the last Produce left it, its state and frame (~0 = no
    // frame made it yet); null before one exists.
    const RHI::Texture* GetProduced(Produced kind, RHI::ResourceState& state, std::uint64_t& frame) const;
    // @p kind's pipeline is built (a producer that does not compile leaves the
    // view and the other targets working).
    bool ProducerReady(Produced kind) const
    {
        return m_ready && m_producePso[static_cast<size_t>(kind)].IsValid();
    }
    // Whether a producer in @p produce reads a counter the material pixel shaders write
    // (overdraw, mipDensity) and that counter is made and at its fixed heap slot: then
    // this frame's PSOs take the DEBUG_SHADE permutation. Asked at the service point,
    // after OnService.
    bool DebugShadeReady(std::uint32_t produce) const;
    // The producers that read such a counter. The DEBUG_SHADE permutation writes every
    // counter, so a frame that runs one runs them all - each copies its counter out and
    // zeroes it, and none piles up frames it did not read.
    static std::uint32_t CountedMask();
    // The material cost table - per material slot, the instructions of the pixel shader
    // that draws it - at its fixed heap slot (BINDLESS_DEBUG_COST_TABLE). Made at the
    // service point the first frame a producer that reads it runs (CostTableMask:
    // materialCost); the render half copies the frame's table in (UploadCostTable) before
    // any list reads it.
    static std::uint32_t CostTableMask();
    bool CostTableReady() const { return m_costTable.ready; }
    // @p count costs (one per material slot; the slots past them 0) into the
    // table through @p frameSlot's upload buffer, recorded on @p cl; @p frame is
    // the frame they are for (Produce makes materialCost only from that frame's
    // table). False when the table is not made.
    bool UploadCostTable(RHI::CommandList cl, std::uint32_t frameSlot, const std::uint32_t* costs,
                         std::uint32_t count, std::uint64_t frame);
    // The visible pixels per material slot a materialCost frame counted, read back once
    // its frame is done - @p frameSlot's, which the GPU finished before this frame's
    // render half began (call it then, before this frame's Produce reuses the slot).
    // False when that slot holds none (or holds it no more: each one is taken once).
    bool TakeCoverage(std::uint32_t frameSlot, std::vector<std::uint32_t>& pixels, std::uint64_t& frame);

private:
    struct alignas(16) CBData
    {
        std::uint32_t srcW, srcH, dstW, dstH;
        std::uint32_t mode;
        std::int32_t  channel;
        float         rangeMin, rangeMax;
        float         exposure, scale, nearZ, farZ;
        std::uint32_t orthographic, srcUint, srcHandle, mask;   // the uint source; Categorical / Bits
    };
    static_assert(sizeof(CBData) == 64, "must mirror DebugTargetCB in shaders/DebugTargetView.cs.hlsl");

    struct alignas(16) ProduceCBData
    {
        std::uint32_t w, h, materialCount, gridFlags;
        float         nearZ, farZ;
        std::uint32_t orthographic, coverage;   // materialCost: the coverage buffer's RWStructuredBuffer handle
        std::uint32_t lightGrid, decalGrid, probeGrid, pad1;
    };
    static_assert(sizeof(ProduceCBData) == 48, "must mirror ProduceCB in shaders/DebugTargetProduce.cs.hlsl");
    struct ProducedTarget
    {
        RHI::Texture       tex;
        RHI::ResourceState state = RHI::ResourceState::UNDEFINED;
        std::uint64_t      frame = ~0ull;   // the frame Produce last made it in
        std::uint32_t      failedW = 0, failedH = 0;   // a size it could not be made at (not retried every frame)
        // The counter a DEBUG_SHADE pixel shader writes (ProducerInfo::counter), at its
        // fixed heap slot once counterReady; always UNORDERED_ACCESS.
        RHI::Texture       counter;
        bool               counterReady = false;
    };

    struct CostTable
    {
        RHI::GPUBuffer gpu;                                // DEFAULT StructuredBuffer<uint>, at its fixed slot
        RHI::GPUBuffer upload[RHI::kFramesInFlight];       // the CPU's copy, one per frame in flight
        void*          mapped[RHI::kFramesInFlight] = {};
        std::uint32_t  capacity = 0;                       // elements
        bool           ready = false, failed = false;
        std::uint64_t  frame = ~0ull;                      // the frame UploadCostTable last copied for
    };

    struct Coverage
    {
        RHI::GPUBuffer counts;                             // DEFAULT RWStructuredBuffer<uint>
        RHI::GPUBuffer readback[RHI::kFramesInFlight];
        std::uint64_t  frameOf[RHI::kFramesInFlight] = {}; // the frame each readback holds; 0 = none
        std::uint32_t  handle = 0;                         // counts' RWStructuredBuffer handle
        bool           ready = false;
    };

    bool CreatePipeline(IGraphicsDevice& gfx);

    // The producers' pipelines, constants (one FrameCB each: one dispatch a frame) and textures.
    void EnsureProduced(Produced kind, std::uint32_t w, std::uint32_t h);   // the service point

    // The material cost table. Made once (the material slot capacity is fixed at boot);
    // a failure is not retried.
    void EnsureCostTable(std::uint32_t capacity);          // the service point

    // materialCost's coverage - the visible pixels per material slot - cleared by a UAV
    // clear, counted by CSMaterialCost, copied to the frame slot's READBACK buffer. Made
    // with the cost table; a failure only leaves the report without pixels.
    void EnsureCoverage(std::uint32_t capacity);           // EnsureCostTable's

    ServiceRequest m_request;   // the Renderer's, read by OnService

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<CBData>    m_cb;
    FrameCB<CBData>    m_cbPreview;   // Slot::Preview
    RHI::Texture       m_fallback;   // 1x1, bound as t0 when the target is not there

    CostTable m_costTable;

    Coverage m_coverage;
    RHI::PipelineState     m_producePso[static_cast<size_t>(Produced::Count)];
    FrameCB<ProduceCBData> m_produceCb[static_cast<size_t>(Produced::Count)];
    ProducedTarget         m_produced[static_cast<size_t>(Produced::Count)];
    bool               m_ready  = false;
    bool               m_failed = false;   // EnsurePipeline failed; cleared by ReloadShaders
};
