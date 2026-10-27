#pragma once

// Gameplay time per entity. The game clock (FrameContext::scaledDeltaTime) slows or stops for a
// pause, a hit-stop or a slow-motion shot; one entity can be slowed further on its own, or keep
// real time through all of them (a presentation character behind a pause menu).

#include <functional>

// Runtime state, never serialized. ScriptSystem writes it on the entity a script names and on that
// entity's animated descendant, so both the gameplay root and the animation clock see it: `scale`
// is the slowest live Time.RequestEntityScale request, `realTime` is set by Time.SetRealTime.
struct TimeDilationComponent
{
    float scale    = 1.f;    // 1 = no request
    bool  realTime = false;  // advance on real time: no Time.RequestScale / GameFlow slow-down
};

namespace GameplayTime
{
    // The two clocks one frame runs on; both 0 while the editor is not simulating.
    struct Clocks
    {
        float game = 0.f;   // FrameContext::scaledDeltaTime
        float real = 0.f;   // FrameContext::deltaTime
    };

    // RendererAnimationChainSystem publishes the frame's clocks before the animation chain runs;
    // HitboxSystem reads them later in the same frame. Main thread only.
    void          Publish(const Clocks& clocks);
    const Clocks& Current();

    // An entity's dt: its clock (real or game) times its own scale. No component = the game clock.
    inline float EntityDt(const TimeDilationComponent* dilation, const Clocks& clocks)
    {
        if (!dilation) return clocks.game;
        return (dilation->realTime ? clocks.real : clocks.game) * dilation->scale;
    }

    // True under -gameclock_legacy (harness builds only): the animation chain on real time and a
    // culled entity's clocks frozen. The gameplay_clock pack's reverse arm.
    bool LegacyClock();

    // A timed slow-down of the game clock for code outside the scripting layer (an animation
    // notify's hit-stop). ScriptSystem installs the sink: the request composes with
    // Time.RequestScale (the slowest wins) and expires on real time. Main thread only; returns false
    // while no sink is installed.
    using TimedScaleSink = std::function<void(float scale, float realSeconds)>;
    void SetTimedScaleSink(TimedScaleSink sink);
    bool RequestTimedScale(float scale, float realSeconds);
}
