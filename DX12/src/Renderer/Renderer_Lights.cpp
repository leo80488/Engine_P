// Renderer_Lights.cpp - the scene's lights for the GPU: the spot shadow casters a frame
// tracks (TrackSpotShadowCasters), BuildScene_PrepareLightData (the light list and its
// digest) and BuildScene_UploadLights (plus the clustered decals).

#include "Graphics/SpotShadowView.h"
#include "Renderer/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "System/HostPolicy.h"

// render passes
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/PerObjectShadowPass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"

// engine systems
#include "System/JobDefaults.h"
#include "System/CommandLine.h" // -spotreducejobs, -spotreduceverify, -spot_shadow_log_every, -verify_spot_shadows, -phaselog_every, -lightprepdigest
#include "System/TaskSystem.h"
#include "System/TaskParallelSort.h"
#include "System/TaskGraph.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <execution>
#include <cmath>
#include <string_view>
#include <type_traits>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace), with their
// layout static_asserts.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;


// Capture BEFORE camera classification so off-screen casters still invalidate their
// lights. Old and new footprints are retained, including removals.
void Renderer::TrackSpotShadowCasters(const std::vector<DrawCandidate>& candidates,
                                      bool retainedSceneDiff, bool matTexUnsettled,
                                      bool laneForm)
{
    ZoneScopedN("BS.TrackSpotCasters");
    ++m_spotCasterStamp;
    m_spotCasterChanges.clear();
    m_spotStaticCasterChanges.clear();
    m_spotForceRefresh = !retainedSceneDiff || matTexUnsettled;
    m_spotStaticForceRefresh = m_spotForceRefresh;
    auto markDirty = [&](const std::vector<Entity>& entities) {
        for (Entity e : entities) {
            if (e >= m_spotCasterState.size()) m_spotCasterState.resize(size_t(e) + 1);
            m_spotCasterState[e].dirty = m_spotCasterStamp;
        }
    };
    markDirty(m_sceneDirty); markDirty(m_sceneMatDirty); markDirty(m_sceneRemoved);
    for (const auto& c : candidates)
    {
        // The material layer of cast-shadow: *mc on main, the cached bit on the lane
        // (ComputeMatBits sets MB_HAS_MATERIAL exactly when mc is non-null, so the two tests ask
        // the same question).
        const bool materialOptsOut = laneForm
            ? ((c.matBits & MB_HAS_MATERIAL) && !(c.matBits & MB_CAST_SHADOW))
            : (c.mc && !c.mc->IsCastingShadow());
        if (!c.entity || !c.castShadow || !(c.viewMask & ViewBit::ShadowAny) ||
            materialOptsOut) continue;
        if (c.entity >= m_spotCasterState.size()) m_spotCasterState.resize(size_t(c.entity) + 1);
        auto& state = m_spotCasterState[c.entity];
        SpotCasterBox box{c.aabbMin, c.aabbMax};
        box.unknown = c.customVSID != 0 ||
            !(box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z) ||
            !std::isfinite(box.min.x + box.min.y + box.min.z + box.max.x + box.max.y + box.max.z);
        const bool animated = c.skinned || c.customVSID || c.dissolveParams.x > 0.0f;
        if (state.seen != m_spotCasterStamp) {
            state.current = box; state.animated = animated; state.seen = m_spotCasterStamp;
            state.hasStatic = false;
        } else {
            // Multiple sections of one entity contribute a union, not just the last section.
            auto& b = state.current;
            b.min = {std::min(b.min.x,box.min.x),std::min(b.min.y,box.min.y),std::min(b.min.z,box.min.z)};
            b.max = {std::max(b.max.x,box.max.x),std::max(b.max.y,box.max.y),std::max(b.max.z,box.max.z)};
            b.unknown |= box.unknown; state.animated |= animated;
        }
        // An entity may contain both custom-VS and ordinary mesh sections.
        // Track the static subset separately so neither classification masks
        // edits/removal of the other subset or inflates its bounds to unknown.
        if (!animated) {
            auto& b = state.currentStatic;
            if (!state.hasStatic) b = box;
            else {
                b.min = {std::min(b.min.x,box.min.x),std::min(b.min.y,box.min.y),std::min(b.min.z,box.min.z)};
                b.max = {std::max(b.max.x,box.max.x),std::max(b.max.y,box.max.y),std::max(b.max.z,box.max.z)};
                b.unknown |= box.unknown;
            }
            state.hasStatic = true;
        }
    }
    bool parallel = Sys::JobSplittingDefaultsEnabled(), verify = false;
#if WITH_HARNESS
    const auto& cmd = Sys::CommandLine::Get();
    parallel = cmd.Has("-spotreducejobs") || Sys::JobSplittingDefaultsEnabled();
    verify = cmd.Has("-spotreduceverify");
#endif
    const size_t count = m_spotCasterState.size();
    const uint32_t lanes = (std::min)(4u, (std::min)(TaskSystem::Get().GetWorkerCount(), static_cast<uint32_t>(count / 2048u)));
    parallel = parallel && lanes >= 2;
    ZoneNamedN(graphZone, "BS.SpotReduce.Graph", parallel);
    ZoneNamedN(serialZone, "BS.SpotReduce.Serial", !parallel);
    auto reduce = [&](size_t begin, size_t end, std::vector<SpotCasterBox>& changes,
                      std::vector<SpotCasterBox>& staticChanges, bool& force, bool& staticForce) {
        auto append = [&](const SpotCasterBox& b) {
            if (b.unknown) force = true;
            else changes.push_back(b);
        };
        auto appendStatic = [&](const SpotCasterBox& b) {
            if (b.unknown) staticForce = true;
            else staticChanges.push_back(b);
        };
        for (size_t index = begin; index < end; ++index)
        {
            auto& state = m_spotCasterState[index];
            if (state.seen != m_spotCasterStamp) {
                if (state.active) {
                    append(state.previous);
                    if (state.previousHasStatic) appendStatic(state.previousStatic);
                }
                state.active = false; continue;
            }
            const bool changed = !state.active || state.animated ||
                state.previousAnimated != state.animated || state.dirty == m_spotCasterStamp ||
                state.previous.unknown != state.current.unknown ||
                std::memcmp(&state.previous.min, &state.current.min, sizeof(XMFLOAT3)) != 0 ||
                std::memcmp(&state.previous.max, &state.current.max, sizeof(XMFLOAT3)) != 0;
            if (changed) {
                if (state.active) append(state.previous);
                append(state.current);
            }
            const bool staticChanged = !state.active || state.dirty == m_spotCasterStamp ||
                state.previousHasStatic != state.hasStatic ||
                (state.hasStatic && (state.previousStatic.unknown != state.currentStatic.unknown ||
                 std::memcmp(&state.previousStatic.min, &state.currentStatic.min, sizeof(XMFLOAT3)) != 0 ||
                 std::memcmp(&state.previousStatic.max, &state.currentStatic.max, sizeof(XMFLOAT3)) != 0));
            if (staticChanged) {
                if (state.active && state.previousHasStatic) appendStatic(state.previousStatic);
                if (state.hasStatic) appendStatic(state.currentStatic);
            }
            state.previousHasStatic = state.hasStatic;
            state.previousStatic = state.currentStatic;
            state.previousAnimated = state.animated;
            state.previous = state.current; state.active = true;
        }
        };
    auto serial = [&] { reduce(0, count, m_spotCasterChanges, m_spotStaticCasterChanges,
                               m_spotForceRefresh, m_spotStaticForceRefresh); };
    auto graph = [&] {
        struct alignas(64) Lane {
            std::vector<SpotCasterBox> changes, staticChanges;
            bool force = false, staticForce = false;
        };
        thread_local std::array<Lane, 4> t_scratch;
        // Bind owner TLS once. Workers only mutate disjoint state ranges and
        // their output shard; no vector growth or World reads in shared state.
        auto* outputs = t_scratch.data();
        for (uint32_t i = 0; i < lanes; ++i) {
            outputs[i].changes.clear(); outputs[i].staticChanges.clear();
            outputs[i].force = outputs[i].staticForce = false;
        }
        auto run = [&](uint32_t i) {
            ZoneScopedN("BS.SpotReduce.Chunk");
            auto& out = outputs[i];
            reduce(count * i / lanes, count * (i + 1) / lanes,
                   out.changes, out.staticChanges, out.force, out.staticForce);
        };
        Sys::CpuTaskScope scope(lanes - 1);
        for (uint32_t i = 1; i < lanes; ++i) scope.Submit([&, i] { run(i); });
        run(0);
        scope.Join();
        ZoneScopedN("BS.SpotReduce.Merge");
        size_t total = 0, staticTotal = 0;
        for (uint32_t i = 0; i < lanes; ++i) {
            total += outputs[i].changes.size(); staticTotal += outputs[i].staticChanges.size();
        }
        m_spotCasterChanges.reserve(total); m_spotStaticCasterChanges.reserve(staticTotal);
        // Contiguous entity ranges preserve the original ascending output order.
        for (uint32_t i = 0; i < lanes; ++i) {
            const auto& out = outputs[i];
            m_spotCasterChanges.insert(m_spotCasterChanges.end(), out.changes.begin(), out.changes.end());
            m_spotStaticCasterChanges.insert(m_spotStaticCasterChanges.end(), out.staticChanges.begin(), out.staticChanges.end());
            m_spotForceRefresh |= out.force; m_spotStaticForceRefresh |= out.staticForce;
        }
    };
#if WITH_HARNESS
    if (parallel && verify) {
        const auto before = m_spotCasterState;
        const bool initialForce = m_spotForceRefresh, initialStaticForce = m_spotStaticForceRefresh;
        serial();
        const auto expectedState = m_spotCasterState;
        const auto expectedChanges = m_spotCasterChanges, expectedStatic = m_spotStaticCasterChanges;
        const bool expectedForce = m_spotForceRefresh, expectedStaticForce = m_spotStaticForceRefresh;
        m_spotCasterState = before;
        m_spotCasterChanges.clear(); m_spotStaticCasterChanges.clear();
        m_spotForceRefresh = initialForce; m_spotStaticForceRefresh = initialStaticForce;
        graph();
        auto sameBox = [](const SpotCasterBox& a, const SpotCasterBox& b) {
            return a.unknown == b.unknown && std::memcmp(&a.min, &b.min, sizeof(a.min)) == 0
                && std::memcmp(&a.max, &b.max, sizeof(a.max)) == 0;
        };
        bool match = expectedForce == m_spotForceRefresh && expectedStaticForce == m_spotStaticForceRefresh
            && expectedChanges.size() == m_spotCasterChanges.size() && expectedStatic.size() == m_spotStaticCasterChanges.size();
        if (match) {
            for (size_t i = 0; i < expectedChanges.size(); ++i) match &= sameBox(expectedChanges[i], m_spotCasterChanges[i]);
            for (size_t i = 0; i < expectedStatic.size(); ++i) match &= sameBox(expectedStatic[i], m_spotStaticCasterChanges[i]);
        }
        for (size_t i = 0; i < count; ++i) {
            const auto& a = expectedState[i]; const auto& b = m_spotCasterState[i];
            match &= sameBox(a.previous,b.previous) && sameBox(a.current,b.current)
                && sameBox(a.previousStatic,b.previousStatic) && sameBox(a.currentStatic,b.currentStatic)
                && a.seen == b.seen && a.dirty == b.dirty && a.active == b.active
                && a.animated == b.animated && a.previousAnimated == b.previousAnimated
                && a.hasStatic == b.hasStatic && a.previousHasStatic == b.previousHasStatic;
        }
        ENGINE_CHECK(match, "spot reduction oracle mismatch");
        LOG_INFO("[SPOT_REDUCE_ORACLE] states=%zu lanes=%u match=%u", count, lanes, match ? 1u : 0u);
    } else
#endif
    if (parallel) graph(); else serial();
}

void Renderer::BuildScene_PrepareLightData(const LightPrepInputs& in, bool clusterMetaDone)
{
    ZoneScopedN("BS.LightData");
    auto* lb = m_lightCB.At(in.uploadSlot);

    // Atlas residency is independent of the bounded per-frame cull/update views.
    const uint32_t frameSlot = in.uploadSlot;
    uint32_t activeCasters = 0, updateCount = 0, spotCastersDenied = 0;
    // This frame's scene revision: each light records the revision at which its casters last
    // changed (per-light, not an all-scene invalidation).
    ++m_spotShadowSceneRevision;
    m_spotShadowCasterCap = SpotShadowPass::kMaxCasters;
    if (m_spotShadowPass && m_spotShadowPass->IsAtlasResident()
        && m_spotShadowVPMapped[frameSlot])
    {
        struct SpotEntry { ResolvedLight* light; XMFLOAT4X4 vp; bool valid; float score = 0; bool dynamic = false; bool dormant = false; };
        std::array<SpotEntry, SpotShadowPass::kMaxCasters> spots{};
        for (auto& L : m_frameLights)
        {
            if (L.type != LightType::Spot || !L.castsShadow ||
                L.radius <= 0.1f || L.spotAngle <= 0.0f) continue;
            if (activeCasters == SpotShadowPass::kMaxCasters)
            { ++spotCastersDenied; continue; }
            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&L.direction));
            XMFLOAT3 d3; XMStoreFloat3(&d3, dir);
            XMVECTOR up = std::fabs(d3.y) > 0.99f ? XMVectorSet(0,0,1,0) : XMVectorSet(0,1,0,0);
            const auto vp = XMMatrixLookToLH(XMLoadFloat3(&L.position), dir, up) *
                XMMatrixPerspectiveFovLH(std::min(L.spotAngle * 2.0f, XM_PI - 0.01f),
                                        1.0f, L.radius, 0.1f);
            auto& entry = spots[activeCasters];
            entry.light = &L;
            XMStoreFloat4x4(&entry.vp, vp);
            // Reserve the same logical slot even while dark. Removing these
            // rows would shift every later light when one lamp switches on.
            entry.dormant = !(L.intensity > 0.0f) ||
                !(std::max({L.color.x, L.color.y, L.color.z}) > 0.0f);
            if (entry.dormant) {
                m_spotShadowPass->SetDormant(activeCasters);
                ++activeCasters;
                continue;
            }
            entry.valid = m_spotShadowPass->IsSliceValid(activeCasters, entry.vp);
            auto intersectsChanges = [&](const std::vector<SpotCasterBox>& changes) {
              for (const auto& b : changes) {
                const float dx = std::max({b.min.x - L.position.x, 0.0f, L.position.x - b.max.x});
                const float dy = std::max({b.min.y - L.position.y, 0.0f, L.position.y - b.max.y});
                const float dz = std::max({b.min.z - L.position.z, 0.0f, L.position.z - b.max.z});
                if (dx*dx + dy*dy + dz*dz > L.radius*L.radius) continue;
                // Homogeneous clip outcodes: conservative for an AABB and
                // valid for reversed Z too (0 <= z <= w). No camera frustum.
                uint32_t outside = 63;
                for (uint32_t corner=0; corner<8; ++corner) {
                    const XMVECTOR p=XMVectorSet(corner&1 ? b.max.x:b.min.x,
                        corner&2 ? b.max.y:b.min.y, corner&4 ? b.max.z:b.min.z,1);
                    XMFLOAT4 q; XMStoreFloat4(&q,XMVector4Transform(p,vp));
                    outside &= (q.x < -q.w ? 1u:0u) | (q.x > q.w ? 2u:0u) |
                        (q.y < -q.w ? 4u:0u) | (q.y > q.w ? 8u:0u) |
                        (q.z < 0 ? 16u:0u);
                    // Near cap is disabled in raster/GPU cull (depth pancake).
                }
                if (outside == 0) return true;
              }
              return false;
            };
            const bool staticAffected = m_spotStaticForceRefresh ||
                intersectsChanges(m_spotStaticCasterChanges);
            const bool affected = staticAffected || m_spotForceRefresh || intersectsChanges(m_spotCasterChanges);
            if (affected || !entry.valid) m_spotLightRevision[activeCasters] = m_spotShadowSceneRevision;
            if (staticAffected || !entry.valid) m_spotLightStaticRevision[activeCasters] = m_spotShadowSceneRevision;
            entry.dynamic = affected;
            const auto& cam = in.view.view.cameraPosition;
            const float dx = cam.x-L.position.x, dy=cam.y-L.position.y, dz=cam.z-L.position.z;
            entry.score = std::sqrt(dx*dx+dy*dy+dz*dz);

            ++activeCasters;
        }
        std::array<uint32_t, SpotShadowPass::kMaxCasters> order{};
        uint32_t awakeCount = 0;
        for (uint32_t i=0; i<activeCasters; ++i)
            if (!spots[i].dormant) order[awakeCount++]=i;
        std::stable_sort(order.begin(), order.begin()+awakeCount, [&](uint32_t a, uint32_t b) {
            // Keep a resident high-quality map through small camera movements.
            const float sa = spots[a].score * (m_spotShadowPass->GetTier(a) ? 0.85f : 1.0f);
            const float sb = spots[b].score * (m_spotShadowPass->GetTier(b) ? 0.85f : 1.0f);
            return sa < sb;
        });
        uint32_t highCount=0, mediumCount=0;
        // shadows.spotMaxResolution: the highest tier the pick may hand out (2 = all three, the
        // default).
        const uint32_t maxTier=m_spotShadowMaxTier;
        for (uint32_t rank=0; rank<awakeCount; ++rank) {
            const uint32_t slice=order[rank];
            const uint32_t oldTier=m_spotShadowPass->GetTier(slice);
            const float distance=spots[slice].score;
            uint32_t tier=0;
            if (maxTier>=2 && highCount<m_spotShadowPass->TierCapacity(2) && distance<(oldTier==2 ? 17.25f : 15.0f))
                { tier=2; ++highCount; }
            else if (maxTier>=1 && mediumCount<m_spotShadowPass->TierCapacity(1) && distance<(oldTier ? 40.25f : 35.0f))
                { tier=1; ++mediumCount; }
            m_spotShadowPass->SetDesiredTier(slice, tier);
        }
        m_spotShadowPass->TrimResidents(activeCasters);
        std::array<bool, SpotShadowPass::kMaxCasters> queued{};
        auto queue = [&](uint32_t slice) {
            auto& entry = spots[slice];
            m_spotShadowPass->QueueSlice(updateCount, slice, entry.vp,
                m_spotLightRevision[slice], m_spotLightStaticRevision[slice]);
            m_spotLightLastUpdate[slice] = m_spotShadowSceneRevision;
            SceneEmitWrite().spotCullVP[updateCount++] = entry.vp;
            entry.valid = queued[slice] = true;
        };
        uint32_t dynamicDemand=0;
        for (uint32_t i=0; i<activeCasters; ++i) dynamicDemand+=spots[i].dynamic;
        // A campus character can intersect 30+ conservative light frusta.
        // Grow the update budget for that demand; static scenes still issue
        // zero views, and ordinary background maintenance uses at most 16.
        const uint32_t frameBudget=std::clamp(dynamicDemand+2u,16u,kSpotCullViews);
        // Reserve two views for bootstrap/background fairness. The remaining
        // views service affected nearby lights every frame instead of rotating
        // through the entire campus. Aging prevents starvation under overload.
        std::stable_sort(order.begin(), order.begin()+awakeCount, [&](uint32_t a, uint32_t b) {
            if (spots[a].dynamic != spots[b].dynamic) return spots[a].dynamic;
            auto priority = [&](uint32_t i) {
                const float age = float(m_spotShadowSceneRevision - m_spotLightLastUpdate[i]);
                return (spots[i].dynamic ? 1000.0f : 0.0f) + std::min(age, 120.0f)*25.0f - spots[i].score;
            };
            return priority(a) > priority(b);
        });
        for (uint32_t rank=0; rank<awakeCount && updateCount<frameBudget-2; ++rank) {
            const uint32_t slice=order[rank];
            if (!spots[slice].valid || m_spotShadowPass->NeedsRefresh(slice,m_spotLightRevision[slice])) queue(slice);
        }
        for (uint32_t n=0; n<activeCasters && updateCount<frameBudget; ++n) {
            const uint32_t slice=m_spotShadowRefreshCursor++ % activeCasters;
            if (!spots[slice].dormant && !queued[slice] && (!spots[slice].valid ||
                m_spotShadowPass->NeedsRefresh(slice,m_spotLightRevision[slice]))) queue(slice);
        }
        static const int s_shadowLogEvery=Sys::CommandLine::Get().GetInt("-spot_shadow_log_every",0);
        static const bool s_verifySpot=Sys::CommandLine::Get().Has("-verify_spot_shadows");
        uint32_t affectedCount=0, affectedUpdated=0, tierCount[3]{};
        for (uint32_t i=0; i<activeCasters; ++i) {
            if (spots[i].dormant) continue;
            affectedCount += spots[i].dynamic;
            affectedUpdated += spots[i].dynamic && queued[i];
            ++tierCount[m_spotShadowPass->GetTier(i)];
            if (s_verifySpot && spots[i].valid) {
                const auto view=m_spotShadowPass->GetView(i);
                ENGINE_CHECK(view.atlasIndex != ~0u && view.resolution == (512u << m_spotShadowPass->GetTier(i)),
                    "Spot shadow atlas/view contract mismatch");
                for (uint32_t j=0; j<i; ++j) if (spots[j].valid) {
                    const auto other=m_spotShadowPass->GetView(j);
                    ENGINE_CHECK(view.atlasIndex!=other.atlasIndex || view.slice!=other.slice,
                        "Spot shadow physical slot shared by different lights");
                }
            }
        }
        if (s_verifySpot && affectedCount<=frameBudget-2)
            ENGINE_CHECK(affectedCount==affectedUpdated, "Spot shadow missed an in-budget dynamic update");
        if (s_shadowLogEvery>0 && m_spotShadowSceneRevision % s_shadowLogEvery==0)
            LOG_INFO("[SPOTQUALITY] resident=%u updates=%u affected=%u serviced=%u force=%u boxes=%zu tiers=%u/%u/%u budget=%u awake=%u dormant=%u staticForce=%u staticBoxes=%zu",
                activeCasters,updateCount,affectedCount,affectedUpdated,m_spotForceRefresh ? 1u:0u,
                m_spotCasterChanges.size(),tierCount[0],tierCount[1],tierCount[2],frameBudget,
                awakeCount,activeCasters-awakeCount,m_spotStaticForceRefresh ? 1u:0u,m_spotStaticCasterChanges.size());
        auto* vpDst = static_cast<SpotShadowViewGPU*>(m_spotShadowVPMapped[frameSlot]);
        uint32_t ready = 0;
        for (uint32_t slice = 0; slice < activeCasters; ++slice)
        {
            const auto& entry = spots[slice];
            if (!entry.valid) continue;
            ++ready;
            vpDst[slice] = m_spotShadowPass->GetView(slice);
            entry.light->shadowSliceIdx = slice;
            for (auto& vl : VolLightsWrite())
                if (vl.type == static_cast<uint32_t>(LightType::Spot) &&
                    std::abs(vl.position.x - entry.light->position.x) < 1e-4f &&
                    std::abs(vl.position.y - entry.light->position.y) < 1e-4f &&
                    std::abs(vl.position.z - entry.light->position.z) < 1e-4f)
                { vl.shadowSliceIdx = slice; break; }
        }
        if (ready > m_spotShadowReadyHighWater)
        {
            m_spotShadowReadyHighWater = ready;
            if (ready == awakeCount)
                LOG_INFO("Spot shadows ready: %u/%u lit lights have valid cached maps", ready, awakeCount);
        }
    }
    if (m_spotShadowPass) m_spotShadowPass->SetActiveCasterCount(updateCount);
    SceneEmitWrite().spotCullCount = updateCount;
    static const int s_spotLogEvery = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
    if (s_spotLogEvery > 0 && (in.frameEpoch % s_spotLogEvery) == 0)
        LOG_INFO("[SPOTCACHE] resident %u updates %u revision %llu", activeCasters, updateCount,
                 static_cast<unsigned long long>(m_spotShadowSceneRevision));
    m_spotShadowCastersDropped = spotCastersDenied;
    if (activeCasters > m_spotShadowCasterHighWater)
    {
        m_spotShadowCasterHighWater = activeCasters;
        LOG_INFO("Spot shadows: %u resident lights, capacity %u, %u updates/frame at %u pixels",
                 activeCasters, m_spotShadowCasterCap, kSpotCullViews, SpotShadowPass::kShadowMapSize);
    }
    if (spotCastersDenied > m_spotShadowCastersDeniedWorst)
    {
        m_spotShadowCastersDeniedWorst = spotCastersDenied;
        LOG_WARNING("Spot shadow atlas full: %u resident, %u unassigned", activeCasters, spotCastersDenied);
    }

    // Point-shadow cube assignment: the first kMaxCasters shadow-casting points. Mirrors the
    // spot path but each caster owns a full cube (6 per-face VPs). shadowSliceIdx is reused as
    // the cube index (a light is point XOR spot, so the field is unambiguous); Lighting.ps
    // branches on light.type. Same residency gate as the spot block above: the 96 MiB cube
    // atlas is lazily allocated, and with no atlas every point light keeps
    // shadowSliceIdx == 0xFFFFFFFF, which Lighting.ps reads as "no shadow".
    if (m_pointShadowPass && m_pointShadowPass->IsAtlasResident())
    {
        const uint32_t kMaxCubes = PointShadowPass::kMaxCasters;
        uint32_t activeCubes = 0;
        uint32_t pointCubesDenied = 0;   // over the cube budget (telemetry, like spotCastersDenied)
        m_pointShadowCasterCap = kMaxCubes;

        // Canonical D3D cube face basis (+X,-X,+Y,-Y,+Z,-Z): forward + up per
        // face. Must match the hardware cube-sampler convention so the SRV
        // samples the same face the rasterizer wrote.
        static const XMFLOAT3 kFaceDir[6] = {
            { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1} };
        static const XMFLOAT3 kFaceUp[6] = {
            { 0, 1, 0}, { 0, 1, 0}, { 0, 0,-1}, { 0, 0, 1}, { 0, 1, 0}, { 0, 1, 0} };

        for (auto& L : m_frameLights)
        {
            // Cap test after the qualifier tests so denials can be counted; the assignment is the
            // same as stopping at the cap.
            if (L.type != LightType::Point || !L.castsShadow) continue;
            if (L.radius <= 0.0f)                             continue;
            if (activeCubes >= kMaxCubes) { ++pointCubesDenied; continue; }

            XMVECTOR eye = XMLoadFloat3(&L.position);
            // Reversed-Z 90 deg perspective (NearZ = radius, FarZ = kNearPlane); matches
            // PointShadowPass + Lighting.ps::SamplePointShadow.
            XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f,
                                L.radius, PointShadowPass::kNearPlane);
            for (uint32_t f = 0; f < 6; ++f)
            {
                XMVECTOR dir  = XMLoadFloat3(&kFaceDir[f]);
                XMVECTOR up   = XMLoadFloat3(&kFaceUp[f]);
                XMMATRIX view = XMMatrixLookToLH(eye, dir, up);
                XMMATRIX vp   = view * proj;
                // PointShadowPass transposes internally; pass un-transposed.
                XMFLOAT4X4 vpUntransposed;
                XMStoreFloat4x4(&vpUntransposed, vp);
                m_pointShadowPass->SetFaceMatrix(activeCubes, f, vpUntransposed);
            }

            // One cull view per CUBE (all 6 faces replay it): the six faces share this bounding
            // sphere, so the conservative volume is the light sphere itself (kPointCullViewBase in
            // Renderer.h says why not per face). Same index, same loop as SetFaceMatrix /
            // SetActiveCasterCount.
            if (activeCubes < kPointCullViews)
                SceneEmitWrite().pointCullSphere[activeCubes] =
                    XMFLOAT4(L.position.x, L.position.y, L.position.z, L.radius);

            L.shadowSliceIdx = activeCubes;
            ++activeCubes;
        }

        m_pointShadowPass->SetActiveCasterCount(activeCubes);
        SceneEmitWrite().pointCullCount = std::min<uint32_t>(activeCubes, kPointCullViews);

        // Point-cube telemetry, like the spot block's.
        m_pointShadowCastersDropped = pointCubesDenied;
        if (activeCubes > m_pointShadowCasterHighWater)
            m_pointShadowCasterHighWater = activeCubes;
        if (pointCubesDenied > m_pointShadowCastersDeniedWorst)
        {
            m_pointShadowCastersDeniedWorst = pointCubesDenied;
            LOG_WARNING("Renderer: point shadow cube budget exhausted — %u/%u "
                        "cubes assigned, %u shadow-casting point lights got no "
                        "cube and fall back to occlusion-only (they still "
                        "light, they just cast nothing). Raise "
                        "PointShadowPass::kMaxCasters.",
                        activeCubes, kMaxCubes, pointCubesDenied);
        }
    }
    else
    {
        // Explicit zero for the same reason as the spot path: without it a pass
        // whose atlas was released (or not yet allocated) keeps last frame's
        // cube count and the retained-arg fan-out still walks it.
        if (m_pointShadowPass)
        {
            m_pointShadowPass->SetActiveCasterCount(0);
            m_pointShadowCasterCap = PointShadowPass::kMaxCasters;
        }
        m_pointShadowCastersDropped = 0;
        SceneEmitWrite().pointCullCount = 0;
    }

    {
        // Upload to ClusterPass
        if (m_clusterPass)
        {
            // Synchronous read of this frame's shared CPU payload; no conversion vector,
            // initialization pass or retained pointer into scene storage. On the lane form META
            // (count / active range / world grid) is main's: DDGI reads it on main before the
            // hand-off.
            if (!clusterMetaDone) m_clusterPass->SetLightsMeta(m_frameLights);
            m_clusterPass->UploadLightRecords(m_frameLights, in.uploadSlot);

            // Cluster camera params from this frame's camera (in.view): the FrameView's view is
            // written by UploadFrameData after the scene build, and a stale one makes light/probe
            // cluster binning lag the GBuffer by a frame during camera motion.
            SetClusterCamera(in.view, nullptr, lb);
        }

        // NPR params are per-material; LightCB.nprMinBrightness = 0 so shadows reach true black.
        // This leaves the scene-wide floor knob dead (the shader's
        // max(nprMat.nprMinBrightness, nprMinBrightness) only sees the per-material value), on
        // purpose: a global shadow floor fights the AutoExposure histogram alignment that
        // per-material floors preserve.
        if (lb) lb->nprMinBrightness = 0.0f;
    }

}

// -lightprepdigest=N: one line per Nth frame hashing everything the spot-caster tracking
// and the light prep leave behind - the tracking's change lists, force flags and
// per-entity state; the spot / point view assignment and revisions, every tier and
// published spot view; the shadow slice of every frame light and volumetric light; the
// LightCB cluster fields and ClusterPass META. Logged from whichever thread ran the prep;
// `where` says which, and the rest of the line is what the two arms compare.
void Renderer::SetClusterCamera(const FrameView& fv, ViewState* view, LightCB* lb)
{
    XMMATRIX viewM, proj;
    BuildCameraViewProjFor(fv, viewM, proj);
    XMMATRIX invP = XMMatrixInverse(nullptr, proj);
    XMFLOAT4X4 invProjF, viewF;
    XMStoreFloat4x4(&invProjF, XMMatrixTranspose(invP));
    XMStoreFloat4x4(&viewF, XMMatrixTranspose(viewM));
    if (view)
        m_clusterPass->SetViewCamera(*view, invProjF, viewF, fv.camera.nearZ, fv.camera.farZ, fv.vpWidth,
                                     fv.vpHeight, fv.camera.orthographic);
    else
        m_clusterPass->SetCamera(invProjF, viewF,
                                 fv.camera.nearZ, fv.camera.farZ,
                                 fv.vpWidth, fv.vpHeight,
                                 fv.camera.orthographic);

    // LightCB's cluster params: Lighting.ps / the forward passes find a pixel's cluster from them.
    if (lb)
    {
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->viewMatrix),
                        XMMatrixTranspose(viewM));
        lb->clusterNearZ      = fv.camera.nearZ;
        lb->clusterFarZ       = fv.camera.farZ;
        lb->clusterLightCount = m_clusterPass->GetLightCount();
    }
}

void Renderer::LogLightPrepDigest(const LightPrepInputs& in, const char* where)
{
#if WITH_HARNESS
    static const int s_every = Sys::CommandLine::Get().GetInt("-lightprepdigest", 0);
    if (s_every <= 0 || in.frameEpoch % static_cast<uint64_t>(s_every) != 0) return;
    auto mix = [](uint64_t& h, const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    auto mixBox = [&](uint64_t& h, const SpotCasterBox& b) {
        mix(h, &b.min, sizeof(b.min));
        mix(h, &b.max, sizeof(b.max));
        const uint8_t u = b.unknown ? 1u : 0u;
        mix(h, &u, 1);
    };
    const uint64_t kSeed = 14695981039346656037ull;
    uint64_t hTrack = kSeed, hShadow = kSeed, hLights = kSeed, hCB = kSeed;
    for (const SpotCasterBox& b : m_spotCasterChanges)       mixBox(hTrack, b);
    for (const SpotCasterBox& b : m_spotStaticCasterChanges) mixBox(hTrack, b);
    const uint8_t force[2] = { m_spotForceRefresh ? 1u : 0u, m_spotStaticForceRefresh ? 1u : 0u };
    mix(hTrack, force, sizeof(force));
    for (const SpotCasterState& st : m_spotCasterState)
    {
        mixBox(hTrack, st.previous);       mixBox(hTrack, st.current);
        mixBox(hTrack, st.previousStatic); mixBox(hTrack, st.currentStatic);
        mix(hTrack, &st.seen, sizeof(st.seen));
        mix(hTrack, &st.dirty, sizeof(st.dirty));
        const uint8_t flags = (st.active ? 1u : 0u) | (st.animated ? 2u : 0u) |
            (st.previousAnimated ? 4u : 0u) | (st.hasStatic ? 8u : 0u) |
            (st.previousHasStatic ? 16u : 0u);
        mix(hTrack, &flags, 1);
    }
    const SceneEmit& se = SceneEmitWrite();
    mix(hShadow, &se.spotCullCount, sizeof(se.spotCullCount));
    mix(hShadow, se.spotCullVP, sizeof(XMFLOAT4X4) * (std::min)(se.spotCullCount, kSpotCullViews));
    mix(hShadow, &se.pointCullCount, sizeof(se.pointCullCount));
    mix(hShadow, se.pointCullSphere, sizeof(XMFLOAT4) * (std::min)(se.pointCullCount, kPointCullViews));
    mix(hShadow, &m_spotShadowSceneRevision, sizeof(m_spotShadowSceneRevision));
    mix(hShadow, &m_spotShadowRefreshCursor, sizeof(m_spotShadowRefreshCursor));
    mix(hShadow, m_spotLightRevision, sizeof(m_spotLightRevision));
    mix(hShadow, m_spotLightStaticRevision, sizeof(m_spotLightStaticRevision));
    mix(hShadow, m_spotLightLastUpdate, sizeof(m_spotLightLastUpdate));
    if (m_spotShadowPass)
        for (uint32_t i = 0; i < SpotShadowPass::kMaxCasters; ++i)
        {
            const uint32_t tier = m_spotShadowPass->GetTier(i);
            const SpotShadowViewGPU v = m_spotShadowPass->GetView(i);
            mix(hShadow, &tier, sizeof(tier));
            mix(hShadow, &v, sizeof(v));
        }
    for (const ResolvedLight& L : m_frameLights) mix(hLights, &L.shadowSliceIdx, sizeof(L.shadowSliceIdx));
    for (const auto& vl : VolLightsWrite())      mix(hLights, &vl.shadowSliceIdx, sizeof(vl.shadowSliceIdx));
    if (const auto* lb = m_lightCB.At(in.uploadSlot))
    {
        float vm[16];
        std::memcpy(vm, lb->viewMatrix, sizeof(vm));
        const float nz = lb->clusterNearZ, fz = lb->clusterFarZ, npr = lb->nprMinBrightness;
        const uint32_t cnt = lb->clusterLightCount;
        mix(hCB, vm, sizeof(vm));
        mix(hCB, &nz, 4); mix(hCB, &fz, 4); mix(hCB, &cnt, 4); mix(hCB, &npr, 4);
    }
    if (m_clusterPass)
    {
        const uint32_t meta[3] = { m_clusterPass->GetLightCount(),
                                   m_clusterPass->GetActiveLocalLightCount(),
                                   m_clusterPass->GetFirstActiveLocalLightIndex() };
        mix(hCB, meta, sizeof(meta));
        // The light history's stable keys and the previous <-> current map.
        const uint64_t keys = m_clusterPass->GetLightHistoryDigest();
        mix(hLights, &keys, sizeof(keys));
    }
    LOG_INFO("[LIGHTPREP_DIGEST] frame=%llu track=%016llx shadow=%016llx lights=%016llx "
             "cb=%016llx changes=%zu staticChanges=%zu states=%zu spotViews=%u pointViews=%u "
             "frameLights=%zu where=%s",
             static_cast<unsigned long long>(in.frameEpoch),
             static_cast<unsigned long long>(hTrack), static_cast<unsigned long long>(hShadow),
             static_cast<unsigned long long>(hLights), static_cast<unsigned long long>(hCB),
             m_spotCasterChanges.size(), m_spotStaticCasterChanges.size(),
             m_spotCasterState.size(), se.spotCullCount, se.pointCullCount,
             m_frameLights.size(), where);
#else
    (void)in; (void)where;
#endif
}

Renderer::LightPrepInputs Renderer::CaptureLightPrepInputs() const
{
    LightPrepInputs in;
    in.view       = FrameViewWrite();
    in.uploadSlot = m_gfx.GetUploadSlot();
    in.frameEpoch = m_materialFrameEpoch;
    return in;
}

void Renderer::BuildScene_UploadLights(World& world, bool dataPrepared)
{
    ZoneScopedN("BS.UploadLights");
    if (m_lightPrepOnLaneThisFrame)
    {
        // Lane form: the lane prepares the light data, after its classify. Main keeps the capture
        // and ClusterPass META - the count, the active range and the world light grid read no
        // shadow slice, and DDGI's tick reads the count and the grid index further down this
        // function's caller, on main.
        m_laneLightIn = CaptureLightPrepInputs();
        if (m_clusterPass) m_clusterPass->SetLightsMeta(m_frameLights);
    }
    else if (!dataPrepared)
    {
        const LightPrepInputs in = CaptureLightPrepInputs();
        BuildScene_PrepareLightData(in, false);
        LogLightPrepDigest(in, "main");
    }
    // Sync IBL from SkyboxComponent -> LightCB + passes.
    SyncSkyboxIBL(world);
    // The render features read the World here (RenderFeature.h, Extract): the
    // height fog's component, a module's video surfaces.
    {
        DirectX::XMMATRIX view, proj;
        BuildCameraViewProj(view, proj);
        FeatureExtractContext featureCtx{ world, m_gfx };
        DirectX::XMStoreFloat4x4(&featureCtx.viewProjNoJitter, DirectX::XMMatrixMultiply(view, proj));
        m_features.Extract(featureCtx);
    }

    // Clustered decals: asset-driven, sharing ClusterPass camera state.
    if (m_decalPass)
    {
        std::vector<DecalPass::ResolvedDecal> resolvedDecals;
        auto* pDecal = world.GetPool<DecalComponent>();
        auto* pGT    = world.GetPool<GlobalTransform>();
        const bool haveTexSys = (m_texSys && m_resMgr);

        if (pDecal && pGT && pDecal->Size() > 0 && haveTexSys)
        {
            // Pump Resolve() + TryPromote() once per asset referenced this frame.
            std::unordered_set<Resource::DecalMaterialAsset*> pumped;
            pumped.reserve(pDecal->Size());

            auto* pMatPool = world.GetPool<MaterialComponent>();

            const auto& ents = pDecal->Entities();
            resolvedDecals.reserve(ents.size());
            for (size_t i = 0; i < ents.size(); ++i)
            {
                Entity e = ents[i];
                DecalComponent* dc  = pDecal->Get(e);
                const GlobalTransform* gt = pGT->Get(e);
                if (!dc || !gt) continue;

                // Unified-material path: an entity-level MaterialComponent with domain == Decal drives the
                // decal (.imat workflow: serialized via MaterialRef/MaterialInline like any surface
                // material; textures resolve through BuildScene_SyncMaterials, which ran earlier this
                // build). Takes precedence over a DecalMaterialAsset library link when both are present.
                const MaterialComponent* mc =
                    pMatPool ? pMatPool->Get(e) : nullptr;
                const bool unified = mc && mc->IsDecalDomain();
                if (!unified && !dc->material) continue;

                DecalPass::ResolvedDecal rd;
                DirectX::XMFLOAT4 assetTint = { 1.f, 1.f, 1.f, 1.f };
                bool baseColorAuthored = false;   // a BaseColor path is set (resident or not)

                if (unified)
                {
                    const ResolvedMaterialRuntime rt = ResolveMaterialRuntime(e);
                    baseColorAuthored = !mc->textures[MaterialComponent::BASECOLORMAP].name.empty();
                    rd.texBaseColor = rt.EngineTex(MaterialComponent::BASECOLORMAP).bindlessIndex;
                    rd.texNormal    = rt.EngineTex(MaterialComponent::NORMALMAP).bindlessIndex;
                    rd.texOpacity   = rt.EngineTex(MaterialComponent::OPACITYMAP).bindlessIndex;
                    // Packed ARM feeds BOTH roughness (.g) and AO (.r) via the
                    // DECAL_SRC_ARM channel-select in DecalApply.cs.
                    const int32_t arm = rt.EngineTex(MaterialComponent::SURFACEMAP).bindlessIndex;
                    rd.texRoughness = arm;
                    rd.texAO        = arm;
                    // No unified slots for these: only DecalMaterialAsset's 9-slot layout has them.
                    rd.texSpecular = rd.texBump = rd.texCavity = rd.texDisplacement = -1;

                    rd.flags          = (mc->decalWriteMask & 0x1Fu) | (1u << 8) /*DECAL_SRC_ARM*/;
                    rd.angleFadeStart = mc->decalAngleFadeStart;
                    rd.sortLayer      = static_cast<float>(mc->decalSortLayer);

                    // Surface materials author baseColor in sRGB (WriteMaterialSlot
                    // uploads a linear cache); decal tint multiplies linear
                    // texels, so convert here for identical response. Alpha =
                    // authored opacity.
                    assetTint = { std::pow(std::max(mc->baseColor.x, 0.f), 2.2f),
                                  std::pow(std::max(mc->baseColor.y, 0.f), 2.2f),
                                  std::pow(std::max(mc->baseColor.z, 0.f), 2.2f),
                                  mc->baseColor.w };

                    // Roughness matches the surface remap lerp(min, max, ARM.g): scalars0.y = roughnessMax
                    // (also the flat value when no ARM is bound), scalars1.y = roughnessMin (the bumpStrength
                    // slot is safe to reuse: texBump is -1 on this path). Specular = reflectance scalar; AO
                    // flat = 1 (texture-driven only).
                    rd.scalars0 = { 1.0f, mc->roughnessMax, mc->reflectance, 1.0f };
                    rd.scalars1 = { mc->normalMapStrength, mc->roughnessMin, 0.f, 0.f };
                }
                else
                {
                    Resource::DecalMaterialAsset* asset = dc->material.get();
                    if (pumped.insert(asset).second)
                    {
                        asset->Resolve(*m_texSys, *m_resMgr, m_gfx);
                        asset->TryPromote(*m_texSys);
                    }

                    // Fill 9 bindless indices + scalars from the asset.
                    using A = Resource::DecalMaterialAsset;
                    baseColorAuthored  = !asset->texPaths[A::SLOT_BASECOLOR].empty();
                    rd.texBaseColor    = asset->texBindless[A::SLOT_BASECOLOR];
                    rd.texNormal       = asset->texBindless[A::SLOT_NORMAL];
                    rd.texOpacity      = asset->texBindless[A::SLOT_OPACITY];
                    rd.texRoughness    = asset->texBindless[A::SLOT_ROUGHNESS];
                    rd.texSpecular     = asset->texBindless[A::SLOT_SPECULAR];
                    rd.texAO           = asset->texBindless[A::SLOT_AO];
                    rd.texBump         = asset->texBindless[A::SLOT_BUMP];
                    rd.texCavity       = asset->texBindless[A::SLOT_CAVITY];
                    rd.texDisplacement = asset->texBindless[A::SLOT_DISPLACEMENT];

                    rd.flags          = asset->flags;
                    rd.angleFadeStart = asset->angleFadeStart;
                    rd.sortLayer      = static_cast<float>(asset->sortLayer);

                    assetTint   = asset->baseColorTint;
                    rd.scalars0 = { asset->opacity, asset->roughness, asset->specular, asset->ao };
                    rd.scalars1 = { asset->normalStrength, asset->bumpStrength,
                                    asset->cavityStrength, asset->displacementScale };
                }

                // A channel with no texture writes its paired scalar as a flat
                // value (DecalApply.cs.hlsl); BaseColor's is the tint, so a
                // texture-less base-colour decal paints its baseColor. A BaseColor
                // that IS authored but not resident (still streaming, or a path
                // that never loads) writes nothing instead of a flat tint box.
                if (baseColorAuthored && rd.texBaseColor < 0)
                    rd.flags &= ~static_cast<uint32_t>(Resource::DecalMaterialAsset::WRITE_BASECOLOR);

                // Skip only when no textures AND no channel with a flat fallback
                // writes (allows pure-scalar decals; Normal is texture-only).
                bool anySlot = rd.texBaseColor >= 0 || rd.texNormal >= 0 ||
                               rd.texOpacity  >= 0 || rd.texRoughness >= 0 ||
                               rd.texSpecular >= 0 || rd.texAO >= 0 ||
                               rd.texBump     >= 0 || rd.texCavity >= 0 ||
                               rd.texDisplacement >= 0;
                const bool scalarOnlyWrite =
                    (rd.flags & (Resource::DecalMaterialAsset::WRITE_BASECOLOR |
                                 Resource::DecalMaterialAsset::WRITE_ROUGHNESS |
                                 Resource::DecalMaterialAsset::WRITE_SPECULAR  |
                                 Resource::DecalMaterialAsset::WRITE_AO)) != 0;
                if (!anySlot && !scalarOnlyWrite) continue;

                // worldToDecal = inverse(world); row-major DX-style (last row = translation).
                XMMATRIX worldMtx = XMLoadFloat4x4(&gt->matrix);
                XMMATRIX inv      = XMMatrixInverse(nullptr, worldMtx);

                XMStoreFloat4x4(&rd.worldToDecal, XMMatrixTranspose(inv));
                rd.boundsCenter = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };

                // OBB-sphere radius: unit cube's half-diagonal = sqrt(3)/2.
                XMVECTOR sx = XMVector3Length(worldMtx.r[0]);
                XMVECTOR sy = XMVector3Length(worldMtx.r[1]);
                XMVECTOR sz = XMVector3Length(worldMtx.r[2]);
                float maxScale = std::max({ XMVectorGetX(sx), XMVectorGetX(sy), XMVectorGetX(sz) });
                rd.boundsRadius = maxScale * 0.8660254f;

                // Decal world-space +Z = normalized row 2 of the world matrix.
                XMVECTOR zAxis = XMVector3Normalize(worldMtx.r[2]);
                XMStoreFloat3(&rd.decalForwardWS, zAxis);

                // Tint: per-instance override wins; .a pre-multiplied with fadeAlpha.
                const bool tintOverridden = (dc->tintOverride.w > 0.f);
                DirectX::XMFLOAT4 baseTint = tintOverridden ? dc->tintOverride : assetTint;
                // Unified path linearizes the MATERIAL tint (sRGB-authored);
                // linearize the sRGB-authored per-instance override the same
                // way or identical RGB values render visibly different.
                if (unified && tintOverridden)
                {
                    baseTint.x = std::pow(std::max(baseTint.x, 0.f), 2.2f);
                    baseTint.y = std::pow(std::max(baseTint.y, 0.f), 2.2f);
                    baseTint.z = std::pow(std::max(baseTint.z, 0.f), 2.2f);
                }
                rd.baseColorTint = { baseTint.x, baseTint.y, baseTint.z, baseTint.w * dc->fadeAlpha };

                resolvedDecals.push_back(rd);
            }
        }

        m_decalPass->SetDecals(resolvedDecals);

        // Camera data: invVP -> apply CS world-pos; invProj + view -> cull CS; camPos ->
        // displacement parallax. MUST be this frame's camera: the apply CS reconstructs world pos
        // from this frame's depth buffer, and the FrameView's view is refreshed only in
        // UploadFrameData, after the scene build (a stale one slides decals by a frame of camera
        // motion).
        SetDecalCamera(FrameViewWrite(), nullptr);
    }
}

void Renderer::SetDecalCamera(const FrameView& fv, ViewState* viewState)
{
    XMMATRIX view, proj;
    BuildCameraViewProjFor(fv, view, proj);
    XMMATRIX invP    = XMMatrixInverse(nullptr, proj);
    XMMATRIX viewProj = view * proj;
    XMMATRIX invVP   = XMMatrixInverse(nullptr, viewProj);
    XMFLOAT4X4 invProjF, invVPF, viewF;
    XMStoreFloat4x4(&invProjF, XMMatrixTranspose(invP));
    XMStoreFloat4x4(&invVPF,   XMMatrixTranspose(invVP));
    XMStoreFloat4x4(&viewF,    XMMatrixTranspose(view));
    // RENDER-resolution dims: the apply CS indexes the render-res depth / GBuffer per pixel;
    // display dims break the uv scale under an upscaler.
    const uint32_t w = fv.renderWidth  ? fv.renderWidth  : fv.vpWidth;
    const uint32_t h = fv.renderHeight ? fv.renderHeight : fv.vpHeight;
    if (viewState)
        m_decalPass->SetViewCamera(*viewState, invProjF, invVPF, viewF, fv.camera.position,
                                   fv.camera.nearZ, fv.camera.farZ, w, h);
    else
        m_decalPass->SetCamera(invProjF, invVPF, viewF, fv.camera.position, fv.camera.nearZ, fv.camera.farZ, w, h);
}
