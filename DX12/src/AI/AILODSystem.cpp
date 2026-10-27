#include "AI/AILODSystem.h"
#include "AI/AIComponents.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform (camera pose)

#include <DirectXMath.h>

namespace AI
{
    static float DistanceSq(const DirectX::XMFLOAT4X4& m,
                            const DirectX::XMFLOAT3&   p)
    {
        const float dx = m._41 - p.x;
        const float dy = m._42 - p.y;
        const float dz = m._43 - p.z;
        return dx*dx + dy*dy + dz*dz;
    }

    void AILODSystem::Update(World& world)
    {
        // Disabled: every agent keeps its authored tickInterval. Do not restore a default here: the
        // caller that switched LOD off owns the interval.
        if (!enabled) return;

        // No camera, or a camera without GlobalTransform: every agent takes the combat tier (haveCamera
        // stays false).
        DirectX::XMFLOAT3 camPos{ 0.f, 0.f, 0.f };
        bool haveCamera = false;
        if (m_camera != NullEntity)
        {
            if (auto* gt = world.GetComponent<GlobalTransform>(m_camera))
            {
                camPos     = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
                haveCamera = true;
            }
        }

        const float combatRSq  = tiers.combatRadius  * tiers.combatRadius;
        const float distantRSq = tiers.distantRadius * tiers.distantRadius;

        ZoneScopedN("AI.LOD");

        auto* pool = world.GetPool<AIComponent>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        const size_t n = ents.size();

        uint32_t nCombat = 0, nDistant = 0, nOffscreen = 0, nBoss = 0;
        for (size_t i = 0; i < n; ++i)
        {
            AIComponent& ai = data[i];
            const Entity e  = ents[i];

            if (m_bosses.count(e))
            {
                ai.tickInterval = tiers.bossInterval;
                ++nBoss;
                continue;
            }
            if (!haveCamera)
            {
                ai.tickInterval = tiers.combatInterval;
                ++nCombat;
                continue;
            }

            const auto* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt)
            {
                ai.tickInterval = tiers.combatInterval;
                ++nCombat;
                continue;
            }
            const float d2 = DistanceSq(gt->matrix, camPos);
            if      (d2 <= combatRSq)  { ai.tickInterval = tiers.combatInterval;  ++nCombat; }
            else if (d2 <= distantRSq) { ai.tickInterval = tiers.distantInterval; ++nDistant; }
            else                       { ai.tickInterval = tiers.offscreenInterval; ++nOffscreen; }
        }

#ifdef TRACY_ENABLE
        // The tier split is the key number in any AI measurement: an agent past combatRadius ticks at
        // 1 Hz instead of 10 Hz, a 10x difference in BT cost that reads as "the AI is cheap". A benchmark
        // must place its agents inside combatRadius or report which tier it measured.
        TracyPlot("AI.LOD.Combat",    static_cast<int64_t>(nCombat));
        TracyPlot("AI.LOD.Distant",   static_cast<int64_t>(nDistant));
        TracyPlot("AI.LOD.Offscreen", static_cast<int64_t>(nOffscreen));
        TracyPlot("AI.LOD.Boss",      static_cast<int64_t>(nBoss));
#endif
    }
}
