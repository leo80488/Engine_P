#include "Gameplay/GameplayTime.h"
#include "System/CommandLine.h"

#include <utility>

namespace GameplayTime
{
    namespace
    {
        Clocks         s_clocks;
        TimedScaleSink s_timedScaleSink;
    }

    void Publish(const Clocks& clocks)
    {
        s_clocks = clocks;
    }

    const Clocks& Current()
    {
        return s_clocks;
    }

    void SetTimedScaleSink(TimedScaleSink sink)
    {
        s_timedScaleSink = std::move(sink);
    }

    bool RequestTimedScale(float scale, float realSeconds)
    {
        if (!s_timedScaleSink) return false;
        s_timedScaleSink(scale, realSeconds);
        return true;
    }

    bool LegacyClock()
    {
#if WITH_HARNESS
        // -gameclock_legacy: the reverse arm of the gameplay_clock pack. The animation chain runs on
        // real time (a Time.RequestScale pause no longer holds an attack) and a culled entity's
        // clocks stand still, the two behaviours the pack proves gone.
        static const bool s_legacy = Sys::CommandLine::Get().Has("-gameclock_legacy");
        return s_legacy;
#else
        return false;
#endif
    }
}
