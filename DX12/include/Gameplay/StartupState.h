#pragma once
// game.json "startup_state": the flow-driven boot, where a flow state chooses the first world.

#include <string>

namespace Gameplay
{
    class GameFlowStateMachine;

    // Requests @p bootState (the manifest's startup_state; empty = none) when this
    // run boots through the flow. true = the request was ACCEPTED: that state's
    // on_enter owns the first Scene.Load and the caller must not push
    // startup_scene. Call after the flow's definitions are finalized
    // (RegisterTickSystems), so IsDefined answers from a validated graph.
    bool RequestStartupState(GameFlowStateMachine& gameFlow, const std::string& bootState);
}
