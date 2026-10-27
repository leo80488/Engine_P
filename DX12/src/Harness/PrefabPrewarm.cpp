// -prefab_prewarm=<path.ipfb>@<frame> - see Harness/PrefabPrewarm.h.

#include "Harness/PrefabPrewarm.h"

#ifdef WITH_HARNESS

#include "ECS/ECS.h"
#include "ECS/FrameContext.h"
#include "Resource/PrefabSpawn.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <cstdlib>

namespace Harness
{
    bool PrefabPrewarmSystem::Requested()
    {
        static const bool s_on = Sys::CommandLine::Get().Has("-prefab_prewarm");
        return s_on;
    }

    PrefabPrewarmSystem::PrefabPrewarmSystem(Resource::AssetManager& assetMgr, Renderer& renderer,
                                             Resource::AnimationClipSystem& clips)
        : m_assetMgr(assetMgr), m_renderer(renderer), m_clips(clips)
    {
        const std::string spec = Sys::CommandLine::Get().Get("-prefab_prewarm");
        const std::size_t at   = spec.rfind('@');
        if (at == std::string::npos || at == 0 || at + 1 >= spec.size())
        {
            LOG_ERROR("-prefab_prewarm needs <path.ipfb>@<frame> (got '%s'); nothing is prewarmed",
                      spec.c_str());
            return;
        }
        char* end = nullptr;
        const unsigned long long f = std::strtoull(spec.c_str() + at + 1, &end, 10);
        if (!end || *end != '\0')
        {
            LOG_ERROR("-prefab_prewarm: '%s' is not a frame number; nothing is prewarmed",
                      spec.c_str() + at + 1);
            return;
        }
        m_path    = spec.substr(0, at);
        m_atFrame = static_cast<std::uint64_t>(f);
        m_armed   = true;
        LOG_INFO("[PREFAB_PREWARM] armed: '%s' at frame %llu", m_path.c_str(),
                 static_cast<unsigned long long>(m_atFrame));
    }

    void PrefabPrewarmSystem::Update(World& world, const FrameContext& ctx)
    {
        if (!m_armed || ctx.frameIndex < m_atFrame) return;
        m_armed = false;   // once
        const Entity root = Resource::SpawnPrefab(m_path, world, m_assetMgr, &m_renderer, &m_clips,
                                                  nullptr, "harness prewarm");
        LOG_INFO("[PREFAB_PREWARM] frame=%llu path=%s root=%u (kept: every later spawn of it is warm)",
                 static_cast<unsigned long long>(ctx.frameIndex), m_path.c_str(), root);
    }
}

#endif  // WITH_HARNESS
