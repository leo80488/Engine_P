#include "Gameplay/EventDelivery.h"
#include "ECS/Scheduler.h"
#include "Gameplay/Damage.h"
#include "System/CommandLine.h"
#include "System/EventBus.h"
#include "Diagnostics/Profiler.h"

#include <vector>

namespace Gameplay::EventDelivery
{
    namespace
    {
        // A stage that keeps publishing is cut off here; what it publishes after waits for the next drain.
        constexpr int kMaxStageRounds = 4;

        std::vector<Stage>& Stages()
        {
            static std::vector<Stage> s_stages;
            return s_stages;
        }
    }

    void AddStage(Stage stage)
    {
        Stages().push_back(std::move(stage));
    }

    void Drain(World* world)
    {
        PROF_SCOPE("Events.Drain");
        EventBus::Get().DispatchAll();
        if (!world) return;
        for (int round = 0; round < kMaxStageRounds; ++round)
        {
            bool published = false;
            for (const Stage& stage : Stages()) published = stage(*world) || published;
            if (!published) break;
            EventBus::Get().DispatchAll();
        }
    }

    bool LegacyDispatch()
    {
#if WITH_HARNESS
        static const bool s_legacy = Sys::CommandLine::Get().Has("-eventdelivery_legacy");
        return s_legacy;
#else
        return false;
#endif
    }

    void DrainFromAnimationChain()
    {
        if (LegacyDispatch()) Drain(nullptr);
    }

    void Install(Scheduler& scheduler)
    {
        Damage::Install();
        const auto drain = [](World& world, FrameContext&)
        {
            if (!LegacyDispatch()) Drain(&world);
        };
        scheduler.AddPhaseEdgeHook(TickPhase::GameplayPreLogic, Scheduler::PhaseEdge::Begin, drain);
        scheduler.AddPhaseEdgeHook(TickPhase::BoneAttachment, Scheduler::PhaseEdge::End, drain);
    }
}
