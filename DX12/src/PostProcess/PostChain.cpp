#include "PostProcess/PostChain.h"
#include "Graphics/IGraphicsDevice.h"
#include "PostProcess/ResolvedPostProcessSettings.h"
#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace PostProcess
{

namespace
{
    // An effect's GPU region name, "PostProcess.<effect>" - its bare name would
    // collide with a pass's own region inside it (AutoExposurePass opens
    // "AutoExposure"). Interned for the life of the process: the GPU profiler holds
    // the pointer until the frame's results come back, by which time the effect may
    // have been replaced.
    const char* RegionName(const char* effect)
    {
        static std::mutex             s_mutex;
        static std::set<std::string>  s_names;   // node-based: the strings never move
        std::lock_guard<std::mutex> lk(s_mutex);
        return s_names.insert(std::string("PostProcess.") + (effect ? effect : "?")).first->c_str();
    }

    bool IsPostStage(RG::Stage s) { return s >= RG::Stage::Resolve && s <= RG::Stage::Overlay; }
}

void PostChain::Add(std::unique_ptr<IEffect> effect)
{
    if (!ENGINE_CHECK(effect != nullptr, "PostProcess::PostChain::Add: null effect"))
        return;
    ENGINE_CHECK(!m_finalized, "PostProcess::PostChain::Add: '%s' added after Finalize", effect->GetName());
    m_effects.push_back(std::move(effect));
}

void PostChain::Finalize()
{
    std::stable_sort(m_effects.begin(), m_effects.end(),
                     [](const std::unique_ptr<IEffect>& a, const std::unique_ptr<IEffect>& b)
                     {
                         if (a->GetStage() != b->GetStage()) return a->GetStage() < b->GetStage();
                         if (a->Order() != b->Order()) return a->Order() < b->Order();
                         return std::strcmp(a->GetName(), b->GetName()) < 0;
                     });
    std::string census;
    for (const std::unique_ptr<IEffect>& e : m_effects)
    {
        ENGINE_CHECK(IsPostStage(e->GetStage()), "PostProcess::PostChain: '%s' is in stage %s, not a post stage "
                     "(Resolve ... Overlay)", e->GetName(), RG::StageName(e->GetStage()));
        census += " ";
        census += e->GetName();
        census += "(";
        census += RG::StageName(e->GetStage());
        census += ":" + std::to_string(e->Order()) + ")";
    }
    m_finalized = true;
    LOG_INFO("[POST_CHAIN] effects=%zu%s", m_effects.size(), census.c_str());
}

void PostChain::Execute(Context& ctx, RG::Stage first, RG::Stage last)
{
    // Fall back to engine defaults if the caller did not resolve this frame, so the
    // effects always see a valid contract.
    static const ResolvedPostProcessSettings kDefault{};
    if (!ctx.resolved) ctx.resolved = &kDefault;

    // The Resolve stage ends with its routing: the last effect of the stage that
    // claims the frame hands its image over - asked of every one, run or not.
    const auto routeResolve = [&]
    {
        for (const auto& effect : m_effects)
        {
            uint64_t srv = 0;
            if (ctx.secondaryView && !effect->ViewSafe()) continue;
            if (effect->GetStage() == RG::Stage::Resolve && effect->ClaimsResolve(ctx, srv))
                ctx.hdrSrv = srv;
        }
    };
    bool routed = !(first <= RG::Stage::Resolve && RG::Stage::Resolve <= last);

    for (auto& effect : m_effects)
    {
        const RG::Stage stage = effect->GetStage();
        if (stage < first || stage > last) continue;
        if (!routed && stage > RG::Stage::Resolve) { routeResolve(); routed = true; }
        if (ctx.secondaryView && !effect->ViewSafe()) continue;
        if (!effect->IsEnabled(ctx)) continue;

        // Hand this producer the buffer the chain is NOT currently reading
        // from, then flip. ctx.hdrSrv holds the previous slot's SRV, so
        // writing the other one can never clobber this effect's own input.
        if (effect->UsesSharedOutput() && ctx.poolTex[0] && ctx.poolTex[1])
        {
            const int slot = ctx.poolIndex & 1;
            effect->SetSharedOutput(ctx.poolTex[slot]);
            ctx.poolIndex = slot ^ 1;
        }
        // One GPU region per effect, inside the caller's PostProcessStack region (which
        // carries the draw / dispatch counts) - unless the effect opens its own
        // (OwnsRegions).
        const bool chainRegion = ctx.gfx && !effect->OwnsRegions();
        const uint32_t region = chainRegion ? ctx.gfx->BeginGPUTimestamp(ctx.cl, RegionName(effect->GetName())) : ~0u;
        effect->Execute(ctx);
        if (chainRegion) ctx.gfx->EndGPUTimestamp(ctx.cl, region);
    }
    if (!routed) routeResolve();
}

bool PostChain::RequiresGraphicsQueue(const Context& ctx) const
{
    for (const auto& effect : m_effects)
        if (effect->RequiresGraphicsQueue(ctx)) return true;
    return false;
}

} // namespace PostProcess
