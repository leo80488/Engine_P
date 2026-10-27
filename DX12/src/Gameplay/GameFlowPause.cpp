// GameFlowPause.cpp - the pause layer of the game flow: requests that stop the game clock and
// suspend a template's SystemSets over whatever state is current, without leaving it. One layer
// serves a project with no states at all, a state graph, and every script that needs the game to
// hold still (a settings screen, a controls screen, the menu key).

#include "Gameplay/GameFlowState.h"
#include "System/CommandLine.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <cstdio>

namespace Gameplay
{

bool PauseLegacyHostMode()
{
#if WITH_HARNESS
    // -pause_legacy: the menu key pauses the host (EngineMode::Paused, which stops Lua) again.
    // The pause_contract pack's reverse arm.
    static const bool s_legacy = Sys::CommandLine::Get().Has("-pause_legacy");
    return s_legacy;
#else
    return false;
#endif
}

// ==== Requests ====

GameFlowStateMachine::PauseId GameFlowStateMachine::RequestPause(std::string reason, std::uint64_t owner)
{
    const PauseId id = ++m_nextPauseId;
    m_pauseRequests.push_back(PauseRequest{ id, std::move(reason), owner });
    return id;
}

bool GameFlowStateMachine::ReleasePause(PauseId id)
{
    const auto it = std::find_if(m_pauseRequests.begin(), m_pauseRequests.end(),
                                 [id](const PauseRequest& r) { return r.id == id; });
    if (it == m_pauseRequests.end()) return false;
    m_pauseRequests.erase(it);
    return true;
}

std::size_t GameFlowStateMachine::ReleasePausesOwnedBy(std::uint64_t owner)
{
    return std::erase_if(m_pauseRequests, [owner](const PauseRequest& r) { return r.owner == owner; });
}

std::size_t GameFlowStateMachine::ReleaseScriptPauses()
{
    return std::erase_if(m_pauseRequests,
                         [](const PauseRequest& r) { return r.owner != kHostPauseOwner; });
}

std::vector<std::string> GameFlowStateMachine::PauseReasons() const
{
    std::vector<std::string> out;
    out.reserve(m_pauseRequests.size());
    for (const PauseRequest& r : m_pauseRequests) out.push_back(r.reason);
    return out;
}

// ==== The template ====

void GameFlowStateMachine::ConfigurePause(PauseTemplate pauseTemplate)
{
    m_pauseTemplate = std::move(pauseTemplate);
    m_pauseDirty = true;
}

const GameFlowStateMachine::PauseTemplate& GameFlowStateMachine::GetPauseTemplate() const noexcept
{
    return m_pauseTemplate;
}

bool GameFlowStateMachine::IsPauseSuspended(SystemSetId set) const
{
    for (const std::string& name : m_pauseTemplate.suspends)
        if (SystemSetId(name) == set) return true;
    return false;
}
// ==== Applying ====

void GameFlowStateMachine::ApplyPause()
{
    const bool want = !m_pauseRequests.empty();
    if (want == m_pauseApplied && !m_pauseDirty) return;
    const bool changed = want != m_pauseApplied;
    m_pauseDirty   = false;
    m_pauseApplied = want;
    RefreshActiveSets();
    if (!changed) return;

    std::string reasons;
    for (const PauseRequest& r : m_pauseRequests)
    {
        if (!reasons.empty()) reasons += ", ";
        reasons += r.reason;
    }
    LOG_INFO("[PAUSE] %s (%zu request(s)%s%s)", want ? "paused" : "resumed", m_pauseRequests.size(),
             reasons.empty() ? "" : ": ", reasons.c_str());
    EventBus::Get().Publish(PauseChangedEvent{ want, m_pauseRequests.size(), std::move(reasons) });
}

// ==== The menu key ====

bool GameFlowStateMachine::OnMenuKey()
{
    if (PauseLegacyHostMode()) return false;
    // A state graph that handles the trigger owns the key: its nearest 'pause' edge decides.
    if (Dispatch("pause")) return true;
    if (!m_pauseTemplate.menuKey) return true;

    const auto it = std::find_if(m_pauseRequests.begin(), m_pauseRequests.end(),
                                 [](const PauseRequest& r) { return r.owner == kHostPauseOwner; });
    if (it != m_pauseRequests.end()) m_pauseRequests.erase(it);
    else RequestPause("menu key", kHostPauseOwner);
    return true;
}

} // namespace Gameplay
