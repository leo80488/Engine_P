#include "ECS/PlayModeController.h"

// State-transition table (docs/design/editor_runtime_mode_architecture.md):
//
//          RequestPlay        RequestPause     RequestStop      RequestStep
//   Edit    -> Playing*        (ignored)        (ignored)        (ignored)
//   Playing (ignored)          -> Paused        -> Edit**        (ignored)
//   Paused  -> Playing         (ignored)        -> Edit**        -> Stepping
//   Stepping(treated as Paused at top of frame, then as Paused above)
//
//   *  fires onEnterPlay   ** fires onExitPlay
//
// Edit <-> Playing is a world snapshot / restore + lifecycle dispatch; Playing <-> Paused only
// freezes the GameClock and never touches the world or the callbacks. Step advances exactly one
// fixed tick: the frame it is requested for runs as Stepping (Play mask active), and the NEXT
// BeginFrame demotes it back to Paused.

void PlayModeController::BeginFrame(float realDt)
{
    // (1) A Stepping frame ran last frame: demote it to Paused so the single tick does not repeat.
    //     Do this BEFORE processing a new request so a held Step still produces one tick per click.
    if (m_mode == EngineMode::Stepping)
        m_mode = EngineMode::Paused;

    // (2) Apply the queued request as an edge transition.
    const Request req = m_pending;
    m_pending = Request::None;
    switch (req)
    {
        case Request::Play:
            if (m_mode == EngineMode::Edit)
            {
                // Edit -> Playing: snapshot the pristine editor world first.
                if (onEnterPlay) onEnterPlay();
                m_clock.Reset();
                m_mode = EngineMode::Playing;
            }
            else if (m_mode == EngineMode::Paused)
            {
                // Resume: no snapshot, the play world is already live.
                m_mode = EngineMode::Playing;
            }
            break;

        case Request::Pause:
            if (m_mode == EngineMode::Playing)
                m_mode = EngineMode::Paused;
            break;

        case Request::Stop:
            if (m_mode == EngineMode::Playing ||
                m_mode == EngineMode::Paused)
            {
                // -> Edit: restore the editor-world snapshot.
                if (onExitPlay) onExitPlay();
                m_mode = EngineMode::Edit;
            }
            break;

        case Request::Step:
            // Single-step only makes sense from a paused play session.
            if (m_mode == EngineMode::Paused)
                m_mode = EngineMode::Stepping;
            break;

        case Request::None:
        default:
            break;
    }

    // (3) Resolve this frame's simulation delta and advance the GameClock. Stepping forces one fixed
    //     tick; otherwise the sim runs at real wall-clock dt. While the sim is not advancing (Edit /
    //     Paused) the scheduler's run mask skips it and the clock does not move.
    m_effectiveDt = (m_mode == EngineMode::Stepping) ? kFixedStep : realDt;
    if (IsSimAdvancing(m_mode))
        m_clock.Advance(static_cast<double>(m_effectiveDt));
}
