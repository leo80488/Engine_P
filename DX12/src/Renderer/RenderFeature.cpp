#include "Renderer/RenderFeature.h"

#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace
{
    constexpr uint32_t kAllStages = (1u << static_cast<uint32_t>(RG::Stage::Count)) - 1u;
    constexpr uint32_t kGraphStages = StageBit(RG::Stage::GBuffer) | StageBit(RG::Stage::ShadowAndEnvironment) |
                                      StageBit(RG::Stage::GBufferModify) | StageBit(RG::Stage::LightingInputs) |
                                      StageBit(RG::Stage::Lighting) | StageBit(RG::Stage::SkyAndWater) |
                                      StageBit(RG::Stage::Atmosphere) | StageBit(RG::Stage::Transparent) |
                                      StageBit(RG::Stage::PostOpaque) | StageBit(RG::Stage::Editor);

    bool InGraph(RG::Stage s) { return s >= RG::Stage::GBuffer && s <= RG::Stage::Editor; }
}

void FeatureCompileContext::AddGraphPass(std::unique_ptr<RG::RenderPass> pass,
                                         std::initializer_list<RG::Order> order)
{
    // Outside AddPasses the graph is compiled already (CreatePasses) or the
    // pass would carry no stage: either way not a pass for the main graph.
    if (!ENGINE_CHECK(InGraph(stage), "FeatureCompileContext::AddPass outside AddPasses (stage %s): a pass "
                                      "recorded outside the graph is the feature's own, registered with ctx.passes",
                      RG::StageName(stage)))
        return;
    graph.AddPass(stage, std::move(pass), order);
}

void RenderFeatureRegistry::Add(std::unique_ptr<IRenderFeature> feature)
{
    if (!ENGINE_CHECK(feature != nullptr, "RenderFeatureRegistry: a factory returned no feature"))
        return;
    ENGINE_CHECK(!m_finalized, "RenderFeatureRegistry: '%s' added after Finalize", feature->Name());
    m_features.push_back(std::move(feature));
}

void RenderFeatureRegistry::Finalize()
{
    std::stable_sort(m_features.begin(), m_features.end(),
                     [](const std::unique_ptr<IRenderFeature>& a, const std::unique_ptr<IRenderFeature>& b)
                     {
                         if (a->Order() != b->Order()) return a->Order() < b->Order();
                         return std::strcmp(a->Name(), b->Name()) < 0;
                     });
    std::unordered_set<std::string> names;
    std::string census;
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
    {
        ENGINE_CHECK(names.insert(f->Name()).second, "RenderFeatureRegistry: two features are named '%s'", f->Name());
        const uint32_t stages = f->Stages();
        ENGINE_CHECK(stages != 0 && (stages & ~kAllStages) == 0,
                     "RenderFeatureRegistry: '%s' takes part in no stage, or in one past RG::Stage::Count (0x%X)",
                     f->Name(), stages);
        census += " ";
        census += f->Name();
        census += "(";
        const char* sep = "";
        for (uint32_t k = 0; k < static_cast<uint32_t>(RG::Stage::Count); ++k)
            if (stages & (1u << k))
            {
                census += sep;
                census += RG::StageName(static_cast<RG::Stage>(k));
                sep = ",";
            }
        census += ":" + std::to_string(f->Order()) + ")";
    }
    m_finalized = true;
    LOG_INFO("[RENDER_FEATURES] count=%zu%s", m_features.size(), census.c_str());
}

void RenderFeatureRegistry::AddPasses(RG::Stage stage, FeatureCompileContext& ctx)
{
    if (!ENGINE_CHECK(InGraph(stage), "RenderFeatureRegistry::AddPasses: %s is not a stage of the main graph",
                      RG::StageName(stage)))
        return;
    ctx.stage = stage;
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        if (f->Stages() & StageBit(stage))
            f->AddPasses(stage, ctx);
    ctx.stage = RG::Stage::Unstaged;
}

void RenderFeatureRegistry::CreatePasses(FeatureCompileContext& ctx, void (*created)(const char* name))
{
    ctx.stage = RG::Stage::Unstaged;
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
    {
        if (!(f->Stages() & ~kGraphStages))
            continue;   // a graph-only feature's passes went in with AddPasses
        f->CreatePasses(ctx);
        if (created) created(f->Name());
    }
}

void RenderFeatureRegistry::Extract(FeatureExtractContext& ctx)
{
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        f->Extract(ctx);
}

void RenderFeatureRegistry::PrepareGraphStages(FeatureFrameContext& ctx)
{
    for (uint32_t k = static_cast<uint32_t>(RG::Stage::GBuffer); k <= static_cast<uint32_t>(RG::Stage::Editor); ++k)
    {
        const RG::Stage stage = static_cast<RG::Stage>(k);
        for (const std::unique_ptr<IRenderFeature>& f : m_features)
            if (f->Stages() & StageBit(stage))
                f->Prepare(stage, ctx);
    }
}

void RenderFeatureRegistry::Prepare(RG::Stage stage, FeatureFrameContext& ctx)
{
    if (!ENGINE_CHECK_ONCE(!InGraph(stage), "RenderFeatureRegistry::Prepare: %s is a stage of the main graph, "
                                            "whose stages prepare together (PrepareGraphStages)", RG::StageName(stage)))
        return;
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        if (f->Stages() & StageBit(stage))
            f->Prepare(stage, ctx);
}

void RenderFeatureRegistry::PrepareView(FeatureFrameContext& ctx)
{
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        f->PrepareView(ctx);
}

void RenderFeatureRegistry::RegisterDebugTargets(DebugTargetRegistry& registry)
{
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        f->RegisterDebugTargets(registry);
}

bool RenderFeatureRegistry::Has(const char* name) const
{
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        if (std::strcmp(f->Name(), name) == 0)
            return true;
    return false;
}

void RenderFeatureRegistry::Record(RG::Stage stage, FeatureRecordContext& ctx)
{
    if (!ENGINE_CHECK_ONCE(!InGraph(stage), "RenderFeatureRegistry::Record: %s is a stage of the main graph, "
                                            "whose passes record themselves", RG::StageName(stage)))
        return;
    for (const std::unique_ptr<IRenderFeature>& f : m_features)
        if (f->Stages() & StageBit(stage))
            f->Record(stage, ctx);
}
