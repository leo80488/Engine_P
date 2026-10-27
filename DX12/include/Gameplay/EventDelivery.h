#pragma once

// Where queued C++ events (EventBus) reach their subscribers - among them the bridges that hand
// them to Lua. Two drain points, both outside every SystemSet so no game-flow state can starve
// them, both in every EngineMode:
//   - the start of TickPhase::GameplayPreLogic: what the previous frame's render phases and this
//     frame's Input phase (a game-flow transition, the focus edge) published;
//   - the end of TickPhase::BoneAttachment: what the frame's gameplay, AI, physics, animation and
//     hit-box systems published. Lua reads the Lua-bus copies in the next GameplayLogic.
// Delivery is in publish order across event types (EventBus). A C++ event is never dropped for a
// paused owner here: the Lua bus decides per handler (ScriptSystem::DispatchLuaEvents skips a
// handler whose owner's set is suspended, and does not queue the event for it).
//
// After the events are delivered the drain runs its stages, with the world: work that turns delivered
// events into more of them (the damage pipeline makes Damaged / Died of a HitboxHit). What a stage
// publishes is delivered in the same drain, so Lua sees the cause and its consequence together.
//
// The audio events keep their own earlier drain (AudioTickSystem); the drains above deliver whatever
// is still queued. Physics contacts have none: PhysicsSystem::PostAllSteps only publishes them.

#include <functional>

class Scheduler;
class World;

namespace Gameplay::EventDelivery
{
    // Adds the two drains to @p scheduler's phase edges, and the damage pipeline's stage.
    void Install(Scheduler& scheduler);

    // A stage runs after each delivery, in the order added; it returns true when it published
    // events (they are delivered at once, then the stages run again, at most four rounds).
    using Stage = std::function<bool(World&)>;
    void AddStage(Stage stage);

    // Delivers everything queued, now; the stages run when @p world is given.
    void Drain(World* world = nullptr);

    // True under -eventdelivery_legacy (harness builds only): the drain is the animation chain's
    // tail again and the two drains above do nothing, the stages included. The event_order pack's
    // reverse arm.
    bool LegacyDispatch();

    // The animation chain's tail: delivers under -eventdelivery_legacy, otherwise does nothing.
    void DrainFromAnimationChain();
}
