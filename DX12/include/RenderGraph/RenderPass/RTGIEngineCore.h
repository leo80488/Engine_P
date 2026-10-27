#pragma once
// The engine's ReSTIR GI reuse core: RTGI.cs.hlsl's Temporal (with its history
// re-trace / re-shade validation), Spatial and Shade, the 40-byte reservoirs and
// the 32-byte receiver history (RTGIPass is the shell around it; see RTGICore.h).

#include "RenderGraph/RenderPass/RTGICore.h"
#include <array>

namespace RTGI
{
class EngineCore final : public Core
{
public:
    // Reads its own knobs: the pass.rtgi.* rows (spatial / boost sample counts, the
    // fresh-sample blend, the M cap) and -gi_no_spatial_feedback. Reuse, the age cap
    // and the spatial radius arrive with ApplySettings.
    EngineCore();
    ReSTIR::Backend Backend() const override { return ReSTIR::Backend::Engine; }
    bool BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell) override;
    bool HasPipelines() const override { return m_pso.back().IsValid(); }
    bool Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH) override;
    void Release(IGraphicsDevice& gfx) override;
    void ReleasePipelines(IGraphicsDevice& gfx) override;
    void AdoptPipelines(const Core& from) override;
    bool ApplySettings(const CoreSettings& s, bool& historyReset) override;
    bool Reuse() const override { return m_reuse; }
    void FillConstants(IGraphicsDevice& gfx, Constants& c, const FillContext& ctx) override;
    void PreShell(const CoreFrame& f) override;
    void Record(const CoreFrame& f) override;
    const char* InertReason() const override;
    bool IsAllocated() const override { return m_allocated; }
    const char* IgnoredFlags() const override { return ""; }
    uint64_t ResidentBytes() const override;
    double PipelineMs() const override { return m_pipelineMs; }
    bool Feedback() const override { return m_feedback; }
    uint32_t SpatialRadius() const override { return m_spatialRadius; }
    // SHaRC: Temporal (ValidateHistory re-shades the way a sample was shaded) AND
    // Spatial from RTGISharc.cs.hlsl. Every pass that rewrites a reservoir's metadata
    // must keep its cached bit (GIMetaKeep): a pass that drops it makes the next
    // validation re-shade every cached sample analytically (~17k luminance resets a frame).
    bool BuildSharcPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell,
                             const std::vector<DxcCompiler::Define>& defines) override;
    bool HasSharcPipelines() const override { return m_psoSharc[0].IsValid() && m_psoSharc[1].IsValid(); }
    void ReleaseSharcPipelines(IGraphicsDevice& gfx) override;

private:
    struct ReservoirStorage { float position[3]; uint32_t normal; float radiance[3], weight, target; uint32_t metadata; };
    struct SurfaceStorage { float position[3]; uint32_t normal, geoNormal, material, coverageCount, albedo; };
    static_assert(sizeof(ReservoirStorage) == 40 && offsetof(ReservoirStorage, metadata) == 36);
    static_assert(sizeof(SurfaceStorage) == 32);
    std::array<RHI::PipelineState, 3> m_pso; // Temporal, Spatial, Shade
    std::array<RHI::PipelineState, 2> m_psoSharc;   // SHaRC: Temporal, Spatial
    bool m_pipelinesFailed = false;
    bool m_adopted = false;   // m_pso is another core's (AdoptPipelines)
    double m_pipelineMs = 0.0;
    std::array<RHI::GPUBuffer, 3> m_reservoir;
    std::array<RHI::GPUBuffer, 2> m_receiver;
    std::array<RHI::ResourceState, 3> m_reservoirState{};
    std::array<RHI::ResourceState, 2> m_receiverState{};
    uint32_t m_tw = 0, m_th = 0;
    uint32_t m_historySlot = 0, m_receiverSlot = 0;
    uint32_t m_maxM = 8, m_maxAge = 60, m_spatialRadius = 8;
    uint32_t m_spatialSamples = 3, m_boostSamples = 8;
    float m_freshBlend = 0.f;
    bool m_reuseRequested = true, m_reuse = true, m_feedback = true, m_allocated = false;
    float m_historyJitterX = 0, m_historyJitterY = 0;
    uint32_t m_fillFlags = 0;
    uint32_t m_H = 0, m_X = 1, m_Y = 2;
};
} // namespace RTGI
