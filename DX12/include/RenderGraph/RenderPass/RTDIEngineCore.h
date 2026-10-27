#pragma once
// The engine's ReSTIR DI resampling core: RTDI.cs.hlsl's Initial / Temporal / Spatial / Shade
// entries, its 20-byte reservoirs and receiver history, and the visibility-aware reuse chain
// (RTDIPass is the shell around it; see RTDICore.h).

#include "RenderGraph/RenderPass/RTDICore.h"
#include <array>

namespace RTDI
{
class EngineCore final : public Core
{
public:
    // Reads its own -di_* flags; @p shellFlags is the shell's parsed flag word
    // (the shader contract: reuse / no-spatial / reject-all / integrate bits).
    explicit EngineCore(uint32_t shellFlags);
    ReSTIR::Backend Backend() const override { return ReSTIR::Backend::Engine; }
    bool BuildPipelines(IGraphicsDevice& gfx, const RHI::PipelineLayout& shell) override;
    bool HasPipelines() const override { return m_pso.back().IsValid(); }
    bool Allocate(IGraphicsDevice& gfx, uint32_t traceW, uint32_t traceH) override;
    void Release(IGraphicsDevice& gfx) override;
    void ReleasePipelines(IGraphicsDevice& gfx) override;
    void AdoptPipelines(const Core& from) override;
    void ResetHistory() override {}
    bool ApplySettings(const CoreSettings& s, float distance, bool distanceChanged, bool& historyReset) override;
    void ApplyBudgetStage(uint32_t stage) override { m_budgetStage = stage; }
    bool Reuse() const override { return m_reuse; }
    void FillConstants(IGraphicsDevice& gfx, Constants& c, const FillContext& ctx) override;
    void PatchStages(IGraphicsDevice& gfx, ConstantBlock& block) override;
    void PreShell(const CoreFrame& f) override;
    void Record(const CoreFrame& f) override;
    const char* InertReason() const override;
    bool IsAllocated() const override { return m_allocated; }
    const char* IgnoredFlags() const override { return ""; }
    uint64_t ResidentBytes() const override;
    double PipelineMs() const override { return m_pipelineMs; }
    bool Feedback() const override { return m_feedback; }
    bool VisibilityMIS() const override { return m_visibilityMIS; }

    uint32_t SpatialPasses() const { return m_spatialPasses; }

    // The two halves of Record (a DI core is {candidates} x {reuse}), for the RTXDI core's mixed
    // compositions: Initial alone (engine candidates), or everything after it (engine reuse) over
    // an Initial reservoir someone else wrote where the shell constants' initialUav points (in UAV
    // state after PreShell, as Initial leaves it).
    void RecordInitial(const CoreFrame& f);
    void RecordReuse(const CoreFrame& f);

private:
    struct ReservoirStorage { uint32_t reference, seed; float weight, target; uint32_t metadata; };
    static_assert(sizeof(ReservoirStorage) == 20);
    std::array<RHI::PipelineState, 4> m_pso; // Initial, Temporal, Spatial, Shade
    bool m_pipelinesFailed = false;
    bool m_adopted = false;   // m_pso is another core's (AdoptPipelines)
    double m_pipelineMs = 0.0;
    std::array<RHI::GPUBuffer, 4> m_reservoir;
    std::array<RHI::GPUBuffer, 2> m_receiver;
    std::array<RHI::ResourceState, 4> m_reservoirState{};
    std::array<RHI::ResourceState, 2> m_receiverState{};
    uint32_t m_tw = 0, m_th = 0;
    uint32_t m_shellFlags = 0;      // the shell's parsed CLI flag word
    uint32_t m_budgetStage = 0;
    uint32_t m_historySlot = 0, m_receiverSlot = 0;
    uint32_t m_maxM = 20, m_maxAge = 63, m_spatialK = 3, m_candidates = 8;
    uint32_t m_spatialPasses = 1, m_spatialRadius = 16, m_secondRadius = 0;
    int m_reuseOverride = -1;       // -di_debug=1: 0 (no reuse); -1 follow the settings
    int m_candidatesOverride = -1;  // -di_debug=1: 1; -1 follow the settings
    bool m_visibilityConfidence = false;   // -di_visibility_confidence: reuse on unless pinned off
    bool m_reuseRequested = true, m_reuse = true;
    bool m_feedback = true, m_boilingFilter = true, m_discardInvisible = true, m_initialVisibility = true;
    bool m_permutation = false, m_areaLite = false, m_visibilityMIS = true, m_inverseVisibility = true;
    bool m_allocated = false;
    float m_jitterX = 0, m_jitterY = 0, m_historyJitterX = 0, m_historyJitterY = 0;
    uint32_t m_historyPhase = 0;
    uint32_t m_fillFlags = 0;       // the shell flags of this frame's FillConstants
    // This frame's rotation, decided by FillConstants for Record.
    uint32_t m_H = 0, m_X = 1, m_Y = 2, m_shaded = 1, m_second = 2, m_phase = 0;
    bool m_secondStage = false;
};
} // namespace RTDI
