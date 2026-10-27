#include "Scene/SceneTransitionManager.h"

#include "UI/UIDrawList.h"
#include "UI/Font.h"

#include <cstdint>

namespace
{
    inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    inline uint8_t ToByte(float a01) { return static_cast<uint8_t>(Clamp01(a01) * 255.f + 0.5f); }
}

void SceneTransitionManager::EnterPhase(Phase p)
{
    m_phase   = p;
    m_elapsed = 0.f;
    if (m_effect) m_effect->OnPhaseEnter(p);
}

void SceneTransitionManager::Begin(std::string sceneId)
{
    // A transition already owns the screen. An empty id is not refused: dropping it would
    // swallow the load edge (see Begin in the header).
    if (m_phase != Phase::Idle) return;

    // Resolve the effect + timing from the pending params (fade fallback).
    m_params = m_paramsProvider ? m_paramsProvider() : Scene::TransitionParams{};
    m_effect = Scene::CreateTransition(m_params); // never null
    const float scale = (m_params.durationScale > 0.f) ? m_params.durationScale : 1.f;
    m_coverSeconds  = m_effect->CoverSeconds()  * scale;
    m_revealSeconds = m_effect->RevealSeconds() * scale;
    m_effect->OnBegin(m_params);

    m_target        = std::move(sceneId);
    m_cover         = 0.f;
    m_progress      = 0.f;
    m_swapRequested = false;
    EnterPhase(Phase::Cover);
}

void SceneTransitionManager::Tick(float dt, const SceneSwapFn& swapScene)
{
    if (dt < 0.f) dt = 0.f;

    switch (m_phase)
    {
    case Phase::Idle:
        return;

    case Phase::Cover:
    {
        m_elapsed += dt;
        const float t = (m_coverSeconds <= 0.f) ? 1.f : Clamp01(m_elapsed / m_coverSeconds);
        m_cover = t;
        // The prewarm already streams during the cover: show it in the low band so the bar is
        // not dead until fully covered.
        if (m_loadPoll) { float p = 0.f; m_loadPoll(p); m_progress = 0.15f * Clamp01(p); }
        else            { m_progress = 0.15f * t; }
        if (t >= 1.f)
        {
            m_cover         = 1.f; // fully covered: safe to swap once assets are warm
            m_swapRequested = false;
            EnterPhase(Phase::Loading);
        }
        break;
    }

    case Phase::Loading:
    {
        m_cover = 1.f; // hold fully covered across the stall
        if (!m_swapRequested)
        {
            // Pump the async prewarm across frames for REAL progress. Stay in
            // Loading (keep animating the bar) until it reports ready.
            bool ready = true;
            if (m_loadPoll)
            {
                float p = 0.f;
                ready = m_loadPoll(p);
                m_progress = 0.15f + 0.75f * Clamp01(p); // 0.15 -> 0.90 while assets warm
            }
            if (!ready) break;

            // Assets warm (or no poll wired): trigger the blocking swap now. App drains it this same
            // frame behind full cover, so the freeze is hidden.
            if (swapScene && m_target)
            {
                swapScene(*m_target);
                m_target.reset();
            }
            m_swapRequested = true;
            m_progress      = 0.9f;
        }
        else
        {
            // Swap + blocking build finished last frame; new scene is live under
            // full cover. Reveal it.
            m_progress = 1.f;
            EnterPhase(Phase::Reveal);
        }
        break;
    }

    case Phase::Reveal:
    {
        m_elapsed += dt;
        const float t = (m_revealSeconds <= 0.f) ? 1.f : Clamp01(m_elapsed / m_revealSeconds);
        m_cover    = 1.f - t;
        m_progress = 1.f;
        if (t >= 1.f)
        {
            m_cover = 0.f;
            m_phase = Phase::Idle;
            if (m_effect) m_effect->OnEnd();
            m_effect.reset();
        }
        break;
    }
    }
}

void SceneTransitionManager::DrawOverlay(UI::UIDrawList& dl, unsigned int viewportW,
                                         unsigned int viewportH) const
{
    if (m_phase == Phase::Idle && m_cover <= 0.f) return;
    if (viewportW == 0 || viewportH == 0) return;

    using UI::Color32;
    using UI::Vec2;

    const float w = static_cast<float>(viewportW);
    const float h = static_cast<float>(viewportH);

    // 1. The pluggable effect draws the cover/reveal visual; a plain black quad if no effect
    //    is bound, so a transition is never invisible.
    Scene::TransitionFrame f;
    f.phase     = m_phase;
    f.cover     = m_cover;
    f.progress  = m_progress;
    f.viewportW = viewportW;
    f.viewportH = viewportH;
    f.phaseT =
        (m_phase == Phase::Cover)   ? (m_coverSeconds  <= 0.f ? 1.f : Clamp01(m_elapsed / m_coverSeconds))
      : (m_phase == Phase::Reveal)  ? (m_revealSeconds <= 0.f ? 1.f : Clamp01(m_elapsed / m_revealSeconds))
      : (m_phase == Phase::Loading) ? m_progress
      : 0.f;

    if (m_effect)
        m_effect->Render(dl, f);
    else
        dl.AddRectFilled(Vec2{ 0.f, 0.f }, Vec2{ w, h }, Color32(0, 0, 0, ToByte(m_cover)));

    // 2. Shared progress bar + text on top (unless params.showProgressBar opts out). It ramps
    //    in with the cover so it never bleeds over the old/new scene during the sweeps.
    if (!m_params.showProgressBar) return;

    // 2a. A game's own art replaces the built-in entirely, including the cover-ramp fade
    //     below: it controls its own entrance via f.cover. It gets the same frame as the
    //     effect, so the two compose without re-deriving phase timing.
    if (m_overlayHook)
    {
        m_overlayHook(dl, f);
        return;
    }

    const float contentA = Clamp01((m_cover - 0.55f) / 0.45f);
    if (contentA <= 0.f) return;

    const float barW = w * 0.32f;
    const float barH = 8.f;
    const float barX = (w - barW) * 0.5f;
    const float barY = h * 0.78f;

    dl.AddRectFilled(Vec2{ barX, barY }, Vec2{ barX + barW, barY + barH },
                     Color32(50, 50, 55, ToByte(contentA)));                  // track
    const float fillW = barW * Clamp01(m_progress);
    if (fillW > 0.f)
        dl.AddRectFilled(Vec2{ barX, barY }, Vec2{ barX + fillW, barY + barH },
                         Color32(235, 235, 240, ToByte(contentA)));           // fill
    dl.AddRect(Vec2{ barX, barY }, Vec2{ barX + barW, barY + barH },
               Color32(120, 120, 130, ToByte(contentA)), 1.f);               // border

    UI::Font& font = UI::DefaultFont();
    if (font.IsReady())
    {
        std::string provided = m_messageProvider ? m_messageProvider() : std::string();
        const char* msg = provided.empty() ? "Now Loading..." : provided.c_str();
        UI::TextDrawParams tp;               // default sizePx = kTextRefPx
        const Vec2  ts  = font.Measure(msg, tp.sizePx);
        const Vec2  pos{ (w - ts.x) * 0.5f, barY - ts.y - 14.f };
        font.DrawText(dl, pos, Color32(255, 255, 255, ToByte(contentA)), msg, tp);
    }
}
