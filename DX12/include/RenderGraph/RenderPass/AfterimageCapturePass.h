#pragma once

// AfterimageCapturePass - compute pass that copies a slice of the post-skinning vertex output
// (SkinnedVertexRing pos / nrm SRVs) into long-lived snapshot slots of AfterimageSystem's pool.
// Runs right after SkinningPass on the same command list, before any graph pass reads
// SkinnedVertexRing.
// A CS rather than CopyBufferRegion: the ring packs every skinned entity into one buffer, and
// moving it to COPY_SOURCE would stall the whole resource. The CS reads it through its existing
// SRVs and writes the pool UAVs, so the source needs no transition.
// State contract: SkinningPass already issued the ring's UAV barrier (read as SRV here, like
// GBufferPass); the pool buffers stay UAV across frames, and this pass ends with a UAV barrier so
// GBuffer / Transparent can read them.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/RootSignatureLayout.h"

#include <cstdint>

class AfterimageSystem;

class AfterimageCapturePass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "AfterimageCapturePass"; }

    void Setup(RG::RenderGraphBuilder& /*b*/) override {} // no graph textures
    void Init (IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events this pass handles (Renderer::PassSet)

    void SetSystem(AfterimageSystem* sys) { m_system = sys; }

private:
    // Absolute compute root indices (RHI::Slots::Cmp, Graphics/RootSignatureLayout.h) on the shared
    // compute root signature, like SkinningPass. Never Gfx::, whose same-named slots differ.
    static constexpr auto kCBSlot  = RHI::Slots::Cmp::CB;      // ROOT_CBV b0 space2: AfterimageCopyJob
    static constexpr auto kSrcPos  = RHI::Slots::Cmp::SRV_t0;  // DESC_TABLE t0 space2: SkinnedVertexRing pos SRV
    static constexpr auto kSrcNrm  = RHI::Slots::Cmp::SRV_t1;  // DESC_TABLE t1 space2: SkinnedVertexRing nrm SRV
    static constexpr auto kDstPos  = RHI::Slots::Cmp::UAV_u0;  // DESC_TABLE u0 space2: afterimage pool pos UAV
    static constexpr auto kDstNrm  = RHI::Slots::Cmp::UAV_u1;  // DESC_TABLE u1 space2: afterimage pool nrm UAV

    // Same per-job CBV ring slot convention as SkinningPass (256-byte aligned).
    static constexpr uint32_t kMaxJobsPerFrame = 64;
    static constexpr uint32_t kJobSlotBytes    = 256;

    struct alignas(256) JobBufferPool
    {
        uint8_t bytes[kMaxJobsPerFrame * kJobSlotBytes];
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice*   m_gfx     = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // Per-job CBV slots packed into one buffer, bound by byte offset. FrameCB rings it across the
    // frames in flight: otherwise SpawnAfterimage in consecutive frames would overwrite bytes the
    // previous frame's dispatch still reads.
    FrameCB<JobBufferPool> m_jobCB;

    AfterimageSystem* m_system = nullptr;
};
