#include "PostProcess/PostProcessRuntime.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/ThreadContext.h"

#include <algorithm>

namespace PostProcess
{

Runtime& Runtime::Instance()
{
    static Runtime s_instance;
    return s_instance;
}

Runtime& Runtime::Get()
{
    if (Sys::ThreadCtx::Current() == Sys::ThreadCtx::Render)
    {
        Sys::ThreadCtx::NoteViolation();
        ENGINE_CHECK_ONCE(false,
            "[SPLITCHECK][RENDERCTX] PostProcess::Runtime::Get ran in the Render context: the next "
            "frame's resolve rewrites it while this frame renders. Read "
            "PostProcess::Runtime::Published(<the frame's scene buffer>).");
    }
    return Instance();
}

void Runtime::Publish(uint32_t bufferIndex)
{
    m_published[bufferIndex & 1u] = resolved;
    m_publishedSettled[bufferIndex & 1u] = m_sceneSettled;
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        m_viewPublishedValid[bufferIndex & 1u][slot] = m_viewResolvedValid[slot];
        if (m_viewResolvedValid[slot]) m_viewPublished[bufferIndex & 1u][slot] = m_viewResolved[slot];
    }
}

void Runtime::SetViewResolved(uint32_t slot, const ResolvedPostProcessSettings& r)
{
    if (slot == 0 || slot >= RHI::kMaxViews) return;
    m_viewResolved[slot]      = r;
    m_viewResolvedValid[slot] = true;
}

void Runtime::ClearViewResolved(uint32_t slot)
{
    if (slot != 0 && slot < RHI::kMaxViews) m_viewResolvedValid[slot] = false;
}

const ResolvedPostProcessSettings& Runtime::PublishedView(uint32_t bufferIndex, uint32_t slot)
{
    Runtime& rt = Instance();
    bool shared = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -pp_views_share_main: every view renders with the main camera's blend, as before views had their own
    // (a reverse arm).
    static const bool s_share = Sys::CommandLine::Get().Has("-pp_views_share_main");
    shared = s_share;
#endif
    if (!shared && slot != 0 && slot < RHI::kMaxViews && rt.m_viewPublishedValid[bufferIndex & 1u][slot])
        return rt.m_viewPublished[bufferIndex & 1u][slot];
    return rt.m_published[bufferIndex & 1u];
}

bool Runtime::PublishedSceneSettled(uint32_t bufferIndex)
{
    return Instance().m_publishedSettled[bufferIndex & 1u];
}

const ResolvedPostProcessSettings& Runtime::Published(uint32_t bufferIndex)
{
    return Instance().m_published[bufferIndex & 1u];
}

uint64_t Runtime::PushOverride(PostProcessOverride ov)
{
    ov.id      = m_nextId++;
    ov.elapsed = 0.0f;
    overrides.push_back(std::move(ov));
    return overrides.back().id;
}

void Runtime::RemoveOverride(uint64_t id)
{
    overrides.erase(
        std::remove_if(overrides.begin(), overrides.end(),
                       [id](const PostProcessOverride& o) { return o.id == id; }),
        overrides.end());
}

// Piecewise-linear fade: 0 -> 1 over fadeIn, 1 for hold, 1 -> 0 over fadeOut.
// A negative hold means persistent: 1 after fadeIn until removed.
float EvaluateOverrideEnvelope(const PostProcessOverride& o)
{
    constexpr float kEps = 1.0e-4f;
    const float e = o.elapsed;
    if (e <= 0.0f) return (o.fadeIn > kEps) ? 0.0f : 1.0f;

    if (e < o.fadeIn) return e / std::max(o.fadeIn, kEps);
    float t = e - o.fadeIn;

    if (o.hold < 0.0f) return 1.0f;   // persistent
    if (t < o.hold) return 1.0f;
    t -= o.hold;

    if (t < o.fadeOut) return 1.0f - (t / std::max(o.fadeOut, kEps));
    return 0.0f;
}

bool IsOverrideExpired(const PostProcessOverride& o)
{
    if (o.hold < 0.0f) return false;  // persistent until manually removed
    return o.elapsed >= (o.fadeIn + o.hold + o.fadeOut);
}

} // namespace PostProcess
