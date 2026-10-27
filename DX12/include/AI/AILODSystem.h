#pragma once

// AILODSystem - rewrites AIComponent::tickInterval each frame from the distance to the camera,
// so distant AI ticks rarely without per-agent budgeting.
// Tiers:
//   Boss      - entities marked with SetBoss(), always bossInterval
//   Combat    - within combatRadius (also: no camera, or no GlobalTransform)
//   Distant   - within distantRadius
//   Offscreen - beyond distantRadius
// The split uses camera position only; there is no frustum test.

#include "ECS/ECS.h"
#include <unordered_set>

namespace AI
{
    class AILODSystem
    {
    public:
        struct Tiers
        {
            float bossInterval     = 1.f / 30.f;
            float combatInterval   = 1.f / 10.f;
            float distantInterval  = 1.f;
            float offscreenInterval = 5.f;

            float combatRadius   = 25.f;
            float distantRadius  = 80.f;
        };

        // Marks an entity as boss / hero: it uses bossInterval while alive. The caller clears the mark
        // (ClearBoss) on death.
        void SetBoss(Entity e)   { m_bosses.insert(e); }
        void ClearBoss(Entity e) { m_bosses.erase(e); }

        // Camera entity for distance queries. NullEntity disables distance LOD (every AI then uses
        // combatInterval).
        void SetCameraEntity(Entity e) { m_camera = e; }

        // Rewrites tickInterval of every AIComponent.
        void Update(World& world);

        Tiers tiers{};

        // Master switch. When false, Update() leaves every tickInterval as authored. Turn it off for
        // measurement: with it on, an agent past combatRadius drops to 1 Hz and past distantRadius to
        // 0.2 Hz, so a population sweep measures the agents' spread around the camera rather than their
        // count (a per-agent cost can read 10x too low).
        bool enabled = true;

    private:
        Entity                    m_camera = NullEntity;
        std::unordered_set<Entity> m_bosses;
    };
}
