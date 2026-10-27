#include "System/CommandLine.h"
#include "Renderer/Renderer.h"

#include "System/Log.h"

// engine graphics
#include "Graphics/ReflectionProbeTypes.h"
#include "Graphics/ShadowSystem.h"
#include "RenderGraph/RenderPass/ShadowPass.h"

// render passes
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"

// ECS components + systems
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/ProxyVolumeComponent.h"
#include "ECS/Components.h"            // MeshHandle: a probe's own primitive
#include "ECS/HierarchyComponents.h"   // ViewBit::ReflectionProbe

// UTF-8 filesystem-path helpers
#include "System/PathUtil.h"

// The scene's DDGI / SSGI intent is recorded in the settings store
#include "Settings/SettingsStore.h"

// std::max / std::min / std::fabs / std::sqrt - the DDGI camera-relevance scheduler in
// BuildScene_UpdateDDGI. Included explicitly, not through the PCH, so a PCH change cannot
// break this TU.
#include <algorithm>
#include <cmath>

// STL / DirectXMath
#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// The Renderer's DDGI and reflection-probe code: probe select / upload / bake / export and
// the per-frame DDGI tick (one TU per Renderer subsystem; the class is declared in
// Renderer/Renderer.h).
//
// Helpers for the probe GPU upload.
namespace {

struct RigidFrame { DirectX::XMVECTOR ax, ay, az; DirectX::XMFLOAT3 pos; };

// Orthonormalised (scale/shear-stripped) rotation basis + translation of a
// TRS-ish world matrix. Gram-Schmidt so the resulting world->local is RIGID and
// a ray parameter solved in local space equals the world distance.
RigidFrame ExtractRigidFrame(const DirectX::XMFLOAT4X4& m)
{
    using namespace DirectX;
    RigidFrame f;
    f.pos = { m._41, m._42, m._43 };
    XMVECTOR x = XMVectorSet(m._11, m._12, m._13, 0.0f);
    XMVECTOR y = XMVectorSet(m._21, m._22, m._23, 0.0f);
    const float lx = XMVectorGetX(XMVector3Length(x));
    x = (lx > 1e-6f) ? XMVector3Normalize(x) : XMVectorSet(1, 0, 0, 0);
    y = XMVectorSubtract(y, XMVectorScale(x, XMVectorGetX(XMVector3Dot(x, y))));
    const float ly = XMVectorGetX(XMVector3Length(y));
    y = (ly > 1e-6f) ? XMVector3Normalize(y) : XMVector3Normalize(XMVector3Orthogonal(x));
    f.ax = x;
    f.ay = y;
    f.az = XMVector3Cross(x, y);   // right-handed, already unit (x and y orthonormal)
    return f;
}

// Point at `f.pos` + local-space offset expressed in f's basis, in world space.
DirectX::XMFLOAT3 LocalOffsetToWorld(const RigidFrame& f, const DirectX::XMFLOAT3& off)
{
    using namespace DirectX;
    XMVECTOR w = XMVectorAdd(XMLoadFloat3(&f.pos),
        XMVectorAdd(XMVectorAdd(XMVectorScale(f.ax, off.x), XMVectorScale(f.ay, off.y)),
                    XMVectorScale(f.az, off.z)));
    XMFLOAT3 out; XMStoreFloat3(&out, w); return out;
}

// Build a rigid world->local 3x4 (rows into r0/r1/r2, translation in .w) for a
// volume with orthonormal axes ax/ay/az centred at world-space `centre`.
// HLSL reconstructs float3x4(r0,r1,r2) and does mul(m, float4(worldPos,1)).
void StoreWorldToLocal3x4(DirectX::XMVECTOR ax, DirectX::XMVECTOR ay, DirectX::XMVECTOR az,
                          const DirectX::XMFLOAT3& centre,
                          DirectX::XMFLOAT4& r0, DirectX::XMFLOAT4& r1, DirectX::XMFLOAT4& r2)
{
    using namespace DirectX;
    XMMATRIX l2w;
    l2w.r[0] = XMVectorSetW(ax, 0.0f);
    l2w.r[1] = XMVectorSetW(ay, 0.0f);
    l2w.r[2] = XMVectorSetW(az, 0.0f);
    l2w.r[3] = XMVectorSet(centre.x, centre.y, centre.z, 1.0f);
    XMMATRIX w2lT = XMMatrixTranspose(XMMatrixInverse(nullptr, l2w));
    XMFLOAT4X4 m; XMStoreFloat4x4(&m, w2lT);
    r0 = { m._11, m._12, m._13, m._14 };
    r1 = { m._21, m._22, m._23, m._24 };
    r2 = { m._31, m._32, m._33, m._34 };
}

} // namespace

// The SELECT half of the reflection probes, at the top of BuildRenderScene beside
// ExtractPreviewCameras: the classify below it needs the probe baking this frame, whose
// faces are a secondary volume (NoteSecondaryViewCandidate keeps a DrawFilter::ProbeView
// copy of what the main view drops). Allocates stable per-probe slots, restores baked
// cubemaps from disk, sorts (priority DESC / influence volume ASC / entity ASC) and fills
// the bake queue; BuildScene_UploadProbes packs the GPU records at Phase 5 in the order
// this chose (m_probeSelected).
void Renderer::BuildScene_SelectProbes(World& world)
{
    using namespace Reflection;
    using namespace DirectX;

    // FIRST, before the enqueue sweep below: applied after it, last frame's bake would be
    // enqueued and baked again (see ApplyProbeBakeFeedback).
    ApplyProbeBakeFeedback(world);

    m_probeSelected.clear();
    m_probeSelectOverflowed = false;
    m_probeSelectRan        = false;
    m_probeCullActive       = false;

    // The upload's early outs, tested here so the select stops where the
    // upload always stopped: no buffer, or no probe in the World - nothing is
    // enqueued or extracted then (a request already standing keeps standing).
    if (!m_probeMgr.GetUploadPointer(m_gfx)) return;
    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool)         return;
    const auto& probeEnts = probePool->Entities();
    if (probeEnts.empty())  return;
    auto& probeData = probePool->Data();
    m_probeSelectRan = true;

    // ---- Stable slot allocation ----
    // comp.cubemapSlice is the PERSISTENT per-probe slot (runtime-only): it keys the shared
    // cube-array slice, the bindless per-cube SRV, the bake dest and the disk cache. The
    // StructuredBuffer is written in SORT order, but each entry carries the probe's stable
    // cubemapSrvIndex, so priority reordering never relocates baked content. Pass 1 marks
    // claimed slots; new/colliding probes get a free slot in the gather loop.
    bool slotUsed[kMaxReflectionProbes] = {};
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        if (!world.IsAlive(probeEnts[i])) continue;
        uint32_t s = probeData[i].cubemapSlice;
        if (s < kMaxReflectionProbes)
        {
            if (slotUsed[s]) probeData[i].cubemapSlice = ReflectionProbeComponent::kInvalidSlice;
            else             slotUsed[s] = true;
        }
        else if (s != ReflectionProbeComponent::kInvalidSlice)
        {
            probeData[i].cubemapSlice = ReflectionProbeComponent::kInvalidSlice;
        }
    }
    auto allocSlot = [&]() -> uint32_t {
        for (uint32_t s = 0; s < kMaxReflectionProbes; ++s)
            if (!slotUsed[s]) { slotUsed[s] = true; return s; }
        return ReflectionProbeComponent::kInvalidSlice;
    };

    // ---- Gather eligible probes ----
    struct Entry { size_t poolIdx; Entity e; uint32_t slot; uint8_t priority; float volume; };
    std::vector<Entry> entries;
    entries.reserve(probeEnts.size());

    constexpr uint32_t kMaxProbeDiskLoadsPerFrame = 2;
    uint32_t diskLoadsThisFrame = 0;
    bool overflowed = false;

    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        Entity e = probeEnts[i];
        if (!world.IsAlive(e)) continue;
        if (!world.GetComponent<GlobalTransform>(e)) continue; // needs a transform

        if (entries.size() >= kMaxReflectionProbes) { overflowed = true; break; }

        ReflectionProbeComponent& comp = probeData[i];
        if (comp.cubemapSlice == ReflectionProbeComponent::kInvalidSlice)
            comp.cubemapSlice = allocSlot();
        const uint32_t slot = comp.cubemapSlice;
        if (slot >= kMaxReflectionProbes) { overflowed = true; continue; } // no free slot

        // Disk-cache restore (once/probe, budgeted; keyed by the stable slot).
        if (!comp.IsBaked() && !comp.realtime &&
            comp.bakeDataVersion == ReflectionProbeComponent::kCurrentBakeDataVersion &&
            !comp.bakedCubemapPath.empty() && !comp.cacheLoadAttempted &&
            diskLoadsThisFrame < kMaxProbeDiskLoadsPerFrame &&
            m_probeMgr.GetArrayTexture().IsValid())
        {
            comp.cacheLoadAttempted = true;
            ++diskLoadsThisFrame;
            if (m_gfx.LoadITEXIntoTextureCube(m_probeMgr.GetArrayTexture(), slot,
                                              RHI::ResourceState::SHADER_RESOURCE,
                                              comp.bakedCubemapPath.c_str()))
                comp.SetBaked(true);
            else
                LOG_WARNING("Renderer: probe cubemap '%s' load failed — falling back to bake",
                            comp.bakedCubemapPath.c_str());
        }

        // Sort key: influence volume (small first -> UE "big base + small patch").
        const XMFLOAT3 h = comp.influenceHalfExtents;
        const float vol = (comp.influenceShape == EInfluenceShape::Sphere)
            ? std::max(1e-4f, comp.influenceOuterRadius * comp.influenceOuterRadius
                              * comp.influenceOuterRadius)
            : std::max(1e-4f, h.x * h.y * h.z);

        entries.push_back({ i, e, slot, comp.priority, vol });
    }

    // ---- Sort: priority DESC, influence volume ASC, entity ASC (a total order) ----
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.volume   != b.volume)   return a.volume   < b.volume;
        return a.e < b.e;
    });

    // ---- The bake queue, in sorted order ----
    // The upload packs m_probeSelected in this same order.
    for (const Entry& en : entries)
    {
        ReflectionProbeComponent& comp = probeData[en.poolIdx];
        if (!world.GetComponent<GlobalTransform>(en.e)) continue;
        m_probeSelected.push_back({ en.e, en.slot });

        if (!comp.IsBaked() || comp.NeedsRebake())
            m_probeMgr.EnqueueBake(en.slot);

        if (comp.realtime && comp.IsBaked() && comp.tickIntervalFrames > 0)
        {
            const uint64_t interval = comp.tickIntervalFrames;
            if (FrameViewWrite().currentFrame >= comp.lastBakedFrame + interval)
            {
                comp.RequestRebake();
                m_probeMgr.EnqueueBake(en.slot);
                comp.lastBakedFrame = FrameViewWrite().currentFrame;
            }
        }
    }
    m_probeSelectOverflowed = overflowed;

    // LAST, after the sweep has had its chance to enqueue. Resolves one queued
    // probe out of the World so ProcessProbeBakeQueue needs no World at all.
    ExtractProbeBakeRequest(world);

    // ---- The baking probe as a secondary volume ----
    // Its six faces, each a 90-degree frustum from the capture point to the bake's far plane,
    // together cover exactly the cube of that half-size: the classify keeps a ProbeView copy of
    // anything in it the main view drops or converts (NoteSecondaryViewCandidate, in the
    // scene-build classify), so what the probe captures does not depend on where the main
    // camera looks.
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -probe_mainlists (harness): no probe copies - the faces read the main view's Opaque and
    // Shadow lists (the reverse arm, ProcessProbeBakeQueue; tests/unit/test_probe_cull.py).
    static const bool s_mainLists = Sys::CommandLine::Get().Has("-probe_mainlists");
#else
    constexpr bool s_mainLists = false;
#endif
    if (m_probeBakeReq.valid && !s_mainLists)
    {
        const float r = ReflectionProbeCapturePass::kFarZ;
        const XMFLOAT3& c = m_probeBakeReq.capturePos;
        m_probeCullMin = { c.x - r, c.y - r, c.z - r };
        m_probeCullMax = { c.x + r, c.y + r, c.z + r };
        m_probeCullActive = true;
        m_secondaryCullActive = true;
    }
}

// Phase 5, the UPLOAD half: the GPU records of the probes BuildScene_SelectProbes chose, in
// its order - the proxy AttachmentRef resolved to rigid world->local matrices (no proxy:
// one synthesised from the influence volume), packed as the 192-byte GpuShared::ReflectionProbe
// with a bindless cubemapSrvIndex.
void Renderer::BuildScene_UploadProbes(World& world)
{
    using namespace Reflection;
    using namespace DirectX;

    uint32_t activeCount = 0;
    m_probeMgr.SetActiveProbeCount(0);
    auto* lb = m_lightCB.Upload(m_gfx);

    auto publishCount = [&]() {
        m_probeMgr.SetActiveProbeCount(activeCount);
        if (lb) lb->reflectionProbeCount = activeCount;
    };

    auto* dst = m_probeMgr.GetUploadPointer(m_gfx);
    if (!dst) { publishCount(); return; }

    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool || !m_probeSelectRan) { publishCount(); return; }

    // ---- Pack in sorted order ----
    for (size_t p = 0; p < m_probeSelected.size(); ++p)
    {
        const ProbeSelectEntry& en = m_probeSelected[p];
        ReflectionProbeComponent* compP = probePool->Get(en.e);
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(en.e);
        if (!compP || !gt) continue;
        ReflectionProbeComponent& comp = *compP;

        // Influence frame = probe rotation, centred at the probe entity.
        const RigidFrame infl = ExtractRigidFrame(gt->matrix);
        const XMFLOAT3 capturePosWS = LocalOffsetToWorld(infl, comp.captureOffsetLS);

        // Influence volume (authored).
        const bool     inflSphere = (comp.influenceShape == EInfluenceShape::Sphere);
        const XMFLOAT3 inflExtents = inflSphere
            ? XMFLOAT3{ comp.influenceOuterRadius, comp.influenceInnerRadius, 0.0f }
            : comp.influenceHalfExtents;

        // Proxy: resolve the AttachmentRef; else synthesise from the influence
        // volume (preserves parallax when no explicit proxy is assigned).
        RigidFrame  proxy      = infl;
        uint32_t    proxyShape  = inflSphere ? kProxyShapeSphere : kProxyShapeOrientedBox;
        XMFLOAT3    proxyExt    = inflSphere
            ? XMFLOAT3{ comp.influenceOuterRadius, 0.0f, 0.0f }
            : comp.influenceHalfExtents;
        if (Entity pe = comp.proxyVolume.Resolve(world); pe != NullEntity)
        {
            const ProxyVolumeComponent* pv = world.GetComponent<ProxyVolumeComponent>(pe);
            const GlobalTransform*      pgt = world.GetComponent<GlobalTransform>(pe);
            if (pv && pgt)
            {
                proxy      = ExtractRigidFrame(pgt->matrix);
                proxy.pos  = LocalOffsetToWorld(proxy, pv->centerOffsetLS); // box centre
                proxyShape = static_cast<uint32_t>(pv->shape);
                proxyExt   = (pv->shape == EProxyShape::Sphere)
                                 ? XMFLOAT3{ pv->sphereRadius, 0.0f, 0.0f }
                                 : pv->halfExtents;
            }
        }

        GpuShared::ReflectionProbe& gpu = dst[p];
        StoreWorldToLocal3x4(proxy.ax, proxy.ay, proxy.az, proxy.pos,
                             gpu.worldToProxyR0, gpu.worldToProxyR1, gpu.worldToProxyR2);
        StoreWorldToLocal3x4(infl.ax, infl.ay, infl.az, infl.pos,
                             gpu.worldToInflR0, gpu.worldToInflR1, gpu.worldToInflR2);
        gpu.proxyExtents    = proxyExt;
        gpu.proxyShape      = proxyShape;
        gpu.capturePosWS    = capturePosWS;
        gpu.mipCount        = static_cast<float>(kProbeCubemapMips);
        gpu.inflExtents     = inflExtents;
        gpu.inflShape       = inflSphere ? kInflShapeSphere : kInflShapeBox;
        gpu.fadePos         = comp.fadePos;
        gpu.intensity       = std::max(0.0f, comp.intensity);
        gpu.fadeNeg         = comp.fadeNeg;
        gpu.priority        = comp.priority;
        gpu.cubemapSrvIndex = m_probeMgr.GetCubeBindlessIndex(en.slot);
        // NORMALIZE: the shading scales the capture by local / capture-point irradiance;
        // -probe_norm_off turns it off for the A/B.
        static const bool s_probeNormOff = Sys::CommandLine::Get().Has("-probe_norm_off");
        gpu.flags           = (comp.IsBaked() ? GPU_PROBE_FLAG_BAKED : 0u)
                            | (s_probeNormOff ? 0u : GPU_PROBE_FLAG_NORMALIZE);
        gpu._pad = { 0u, 0u };

        ++activeCount;
    }

    if (m_probeSelectOverflowed)
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            LOG_WARNING("Renderer: more reflection probes in scene than kMaxReflectionProbes=%u — extras ignored",
                        kMaxReflectionProbes);
            s_warned = true;
        }
    }

    publishCount();

    // The slot GetUploadPointer() just packed, not GetBufferSrv() (render half).
    if (m_clusterPass)
        m_clusterPass->SetProbes(m_probeMgr.GetUploadBufferSrv(m_gfx), activeCount);

    // Nothing reflection-probe-shaped is bound to LightingPass / TransparentPass. RenderAsync's
    // resolve() block reads the current ring slot's probe-buffer SRV off m_probeMgr into
    // LightCB.reflProbeBufIdx every frame (a per-frame ring handle kept in a pass member goes
    // stale), and reflection_probe.hlsli opens each probe's cube bindlessly through
    // probe.cubemapSrvIndex (GetCubeBindlessIndex above). m_probeMgr owns the
    // TextureCubeArray; RTReflectionPass takes GetArraySrv() through its own FrameData.
}

// Bake queue API; ProcessProbeBakeQueue bakes one entry per frame (amortizes the 6-face +
// prefilter cost).
void Renderer::BakeProbe(uint32_t cubeSlice)
{
    m_probeMgr.EnqueueBake(cubeSlice);
}

void Renderer::BakeAllProbes()
{
    if (!m_lastWorld) return;
    auto* probePool = m_lastWorld->GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    auto& probeData = probePool->Data();
    auto& probeEnts = probePool->Entities();
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        ReflectionProbeComponent& comp = probeData[i];
        if (comp.cubemapSlice == ReflectionProbeComponent::kInvalidSlice) continue;
        comp.RequestRebake();
        m_probeMgr.EnqueueBake(comp.cubemapSlice);
    }
}

// Export baked (non-realtime) probe cubemaps to .itex beside the .iscene so the next
// LoadScene restores them off disk instead of re-baking. Call right before
// Resource::SaveScene: it mutates ReflectionProbeComponent::bakedCubemapPath, which the
// component serializer then persists.
void Renderer::ExportBakedProbeCubemaps(World& world, const std::string& worldPath)
{
    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    auto& probeData = probePool->Data();
    auto& probeEnts = probePool->Entities();
    if (probeEnts.empty())                       return;
    if (!m_probeMgr.GetArrayTexture().IsValid()) return;

    namespace fs = std::filesystem;
    const fs::path    worldP   = Sys::U8Path(worldPath);
    const std::string stem     = Sys::PathToUtf8(worldP.stem());
    const fs::path    probeDir = worldP.parent_path() / (stem + "_probes");

    // New probes get file indices above the highest already-assigned one, so
    // re-saves keep stable filenames and never clobber each other.
    uint32_t nextIdx = 0;
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        const std::string& p = probeData[i].bakedCubemapPath;
        if (p.empty()) continue;
        const std::string fn = Sys::PathToUtf8(Sys::U8Path(p).stem()); // e.g. "probe_3"
        if (fn.rfind("probe_", 0) == 0)
        {
            uint32_t n = 0;
            if (sscanf_s(fn.c_str() + 6, "%u", &n) == 1)
                nextIdx = std::max(nextIdx, n + 1);
        }
    }

    uint32_t exported = 0, cleared = 0;
    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        if (!world.IsAlive(probeEnts[i])) continue;
        ReflectionProbeComponent& comp = probeData[i];

        // Realtime probes re-bake on an interval; un-baked / unslotted probes have no valid
        // cubemap content. Either way there is nothing to persist: drop any stale path so the next
        // load does not restore garbage.
        if (comp.realtime ||
            !comp.IsBaked() ||
            comp.cubemapSlice == ReflectionProbeComponent::kInvalidSlice)
        {
            if (!comp.bakedCubemapPath.empty()) { comp.bakedCubemapPath.clear(); ++cleared; }
            continue;
        }

        // Assign a stable path on first export; reuse it on subsequent saves.
        // Stored relative to the working dir when possible so the scene stays
        // portable (matches how other asset paths are persisted).
        if (comp.bakedCubemapPath.empty())
        {
            const fs::path file = probeDir / ("probe_" + std::to_string(nextIdx++) + ".itex");
            std::error_code ec;
            const fs::path rel = fs::relative(file, fs::current_path(), ec);
            comp.bakedCubemapPath =
                Sys::PathToUtf8((!ec && !rel.empty()) ? rel : file);
        }

        if (m_gfx.SaveTextureCubeToITEX(m_probeMgr.GetArrayTexture(),
                                        comp.cubemapSlice,
                                        RHI::ResourceState::SHADER_RESOURCE,
                                        comp.bakedCubemapPath.c_str()))
        {
            ++exported;
        }
        else
        {
            // Do not persist a path to a file that was not written: the probe re-bakes on the next
            // load instead.
            LOG_WARNING("Renderer: probe cubemap export failed (entity %u) — will re-bake on load",
                        probeEnts[i]);
            comp.bakedCubemapPath.clear();
        }
    }

    if (exported || cleared)
        LOG_INFO("Renderer::ExportBakedProbeCubemaps — exported %u, cleared %u ('%s')",
                 exported, cleared, probeDir.generic_string().c_str());
}

namespace
{
    // -ddgi_off: DDGI off whatever the scene and the player say - an A/B and measurement
    // kill switch, above both.
    bool ForceDdgiOff()
    {
        static const bool s_off = Sys::CommandLine::Get().Has("-ddgi_off");
        return s_off;
    }
}

// The player's allowances. The scene build applies the rule every frame; these keep
// m_ddgiSettings right until it runs again.
void Renderer::SetDDGIAuthored(bool v)
{
    m_ddgiAuthored = v;
    m_ddgiSettings.ddgiEnabled = v && m_sceneDdgiIntent && !ForceDdgiOff();
}

void Renderer::SetSSGIAuthored(bool v)
{
    m_ssgiAuthored = v;
    m_ddgiSettings.ssgiEnabled = v && m_sceneSsgiIntent;
}

// Settings.where("features.ddgi" / "features.ssgi") shows the scene's intent
// beside the player's allowance, and the store's effective value (Policy::
// Capped) is what BuildScene_UpdateDDGI runs. Recorded when it changes - a
// scene load, an edit of the component - not every frame.
void Renderer::RecordSceneIndirectIntent(bool sceneHasComponent)
{
    const uint8_t now = static_cast<uint8_t>((m_sceneDdgiIntent ? 1u : 0u) | (m_sceneSsgiIntent ? 2u : 0u) |
                                             (sceneHasComponent ? 4u : 0u));
    if (now == m_sceneIntentRecorded) return;
    m_sceneIntentRecorded = now;
    Settings::SettingsStore& st = Settings::SettingsStore::Get();
    const char* writer = sceneHasComponent ? "IndirectLightingSettings"
                                           : "IndirectLightingSettings defaults (the scene has none)";
    if (const Settings::Row* r = st.Find("features.ddgi"))
        st.Write(*r, Settings::Layer::Scene, Settings::Value{ .b = m_sceneDdgiIntent }, writer);
    if (const Settings::Row* r = st.Find("features.ssgi"))
        st.Write(*r, Settings::Layer::Scene, Settings::Value{ .b = m_sceneSsgiIntent }, writer);
}

// CPU-side DDGI bookkeeping: collect volumes, alloc/resize, refresh CBs, publish to LightCB.
// The GPU dispatches (TLAS build + trace + relight) are the render half's (RenderAsync).
void Renderer::BuildScene_UpdateDDGI(World& world)
{
    auto* lb = m_lightCB.Upload(m_gfx);

    // The scene's singleton IndirectLightingSettings is its intent (ddgiEnabled /
    // ssgiEnabled) and its art parameters; the player's features.ddgi / features.ssgi are
    // allowances; what runs is both (Settings::Policy::Capped). A scene without the component
    // gets the component's defaults: DDGI wanted (no objection - with no volume there is
    // nothing to run), SSGI not (it is an opt-in tier), every art parameter at its default, so
    // a previous scene's values do not linger after a load.
    const IndirectLightingSettingsComponent* scene = nullptr;
    if (auto* settingsPool = world.GetPool<IndirectLightingSettingsComponent>())
        if (!settingsPool->Entities().empty()) scene = &settingsPool->Data().front();
    m_ddgiSettings    = scene ? *scene : IndirectLightingSettingsComponent{};
    m_sceneDdgiIntent = m_ddgiSettings.ddgiEnabled;
    m_sceneSsgiIntent = m_ddgiSettings.ssgiEnabled;
    RecordSceneIndirectIntent(scene != nullptr);
    m_ddgiSettings.ddgiEnabled = m_sceneDdgiIntent && m_ddgiAuthored && !ForceDdgiOff();
    m_ddgiSettings.ssgiEnabled = m_sceneSsgiIntent && m_ssgiAuthored;

    // No volume pool -> 0 volumes this frame; LightCB still needs the master toggles.
    // EnsurePool for the runtime component: World serialization skips
    // DDGIVolumeRuntimeComponent (GPU handles, rebuilt on demand), so after a fresh load no
    // entity holds one, and without the pool the auto-promote loop below would never run -
    // DDGI silently off until a volume is added in the editor.
    auto* volPool = world.GetPool<DDGIVolumeComponent>();
    auto* runPool = world.EnsurePool<DDGIVolumeRuntimeComponent>();
    uint32_t activeCount    = 0;

    // Parallel arrays for the manager's Tick + the GC pass below. Declared out here (not
    // inside the volPool block) so the GC still runs when this world has no DDGIVolume pool at
    // all (see FreeUnclaimedSlots below).
    const DDGIVolumeComponent*  vols[DDGI::kMaxVolumes] = {};
    DDGIVolumeRuntimeComponent* runs[DDGI::kMaxVolumes] = {};

    static uint32_t s_ddgiScanLogCounter = 0;
    const bool ddgiScanLogTick = false;

    if (ddgiScanLogTick)
    {
        size_t volEntCount = (volPool ? volPool->Entities().size() : 0);
        LOG_INFO("DDGI scan: volPool=%s entities=%zu",
                 volPool ? "ok" : "null", volEntCount);
    }

    if (volPool && runPool)
    {
        auto& volEnts = volPool->Entities();
        auto& volData = volPool->Data();

        // Promote runtime component for newly-added volumes.
        for (size_t i = 0; i < volEnts.size(); ++i)
        {
            Entity e = volEnts[i];
            if (!world.IsAlive(e)) continue;
            if (!runPool->Get(e))
                world.AddComponent<DDGIVolumeRuntimeComponent>(e, {});
        }

        // Overflow telemetry. The loop below stops at kMaxVolumes before AllocateOrUpdate, so the
        // manager's own "no free volume slot" error never fires and the extra volumes drop
        // silently. Which volumes survive is ECS pool order (swap-remove reshuffles it), so a scene
        // can light differently across runs. Latched: an over-budget scene logs once, not per
        // frame.
        {
            static size_t s_lastOverflowCount = 0;
            const size_t  volCount = volEnts.size();
            if (volCount > DDGI::kMaxVolumes)
            {
                if (volCount != s_lastOverflowCount)
                {
                    LOG_WARNING("DDGI: scene has %zu DDGIVolume components but only %u "
                                "slots exist — %zu will be IGNORED (selection is ECS pool "
                                "order, not distance or priority).",
                                volCount, DDGI::kMaxVolumes,
                                volCount - DDGI::kMaxVolumes);
                    s_lastOverflowCount = volCount;
                }
            }
            else
            {
                s_lastOverflowCount = 0;
            }
        }

        // Alloc/resize on first sight or on a probe-count change.
        std::vector<uint32_t> debugSlots;
        for (size_t i = 0; i < volEnts.size() && activeCount < DDGI::kMaxVolumes; ++i)
        {
            Entity e = volEnts[i];
            if (!world.IsAlive(e)) continue;
            DDGIVolumeRuntimeComponent* rt = runPool->Get(e);
            if (!rt) continue;

            DDGIVolumeComponent& vc = volData[i];
            if (!m_ddgiMgr.AllocateOrUpdate(m_gfx, vc, *rt))
                continue;

            vols[activeCount] = &vc;
            runs[activeCount] = rt;
            activeCount++;

            // EXTRACT the probe-debug lane: the slot index of each volume that wants spheres, gathered
            // inside the walk that already proved the entity alive and the runtime component present,
            // so the debug pass never walks the pools from the render half.
#if WITH_HARNESS
            // -ddgi_probedebug (harness): every volume draws its probe spheres, as the editor's
            // per-volume debugDraw toggle (not serialized, not writable from Lua) - the DDGIProbeDebug
            // list and its depth handling run without the editor.
            static const bool s_probeDebug = Sys::CommandLine::Get().Has("-ddgi_probedebug");
#else
            constexpr bool s_probeDebug = false;
#endif
            if ((vc.debugDraw || s_probeDebug) && rt->volumeSlot < DDGI::kMaxVolumes)
                debugSlots.push_back(rt->volumeSlot);
        }

        // ---- Camera-relevance scheduling ----
        // Grades each volume by how close the camera is and turns that into an effective
        // `probeUpdateDivisor` (plus an outright skip for the dormant tier), so covering a big
        // scene with several volumes does not cost every volume's full ray budget every frame.
        // The divisor is the only GPU-side lever, no shader change: DDGIPrepareRayCount throttles
        // the whole ray pipeline through one `g_RayCount[probeIdx] = 0` write, and both relight
        // paths compensate their EMA for it (blendSpeed * N for irradiance, hysteresis^N for
        // depth), so convergence wall-clock time is kept; what is traded is temporal resolution.
        // Distance is measured to the AABB SURFACE, not the centre: a long thin corridor volume
        // must not be graded "far" just because its centre is.
        {
            XMVECTOR camPosV, camFwdV, camUpV;
            BuildCameraBasis(camPosV, camFwdV, camUpV);   // NOT FrameViewWrite().view - that lags a frame
            XMFLOAT3 cam{};
            XMStoreFloat3(&cam, camPosV);

            for (uint32_t i = 0; i < activeCount; ++i)
            {
                const DDGIVolumeComponent&  v = *vols[i];
                DDGIVolumeRuntimeComponent& r = *runs[i];

                const float dx = (std::max)(0.0f, std::fabs(cam.x - v.origin.x) - v.extent.x);
                const float dy = (std::max)(0.0f, std::fabs(cam.y - v.origin.y) - v.extent.y);
                const float dz = (std::max)(0.0f, std::fabs(cam.z - v.origin.z) - v.extent.z);
                // Normalize each axis independently: a wide but thin floor must
                // not use its 51 m horizontal extent to grade vertical distance.
                // Two probe spacings keep small/coarse volumes conservative.
                auto axisScale = [](float extent, uint32_t count) {
                    const float spacing = 2.0f * extent / float((std::max)(1u, count > 1 ? count-1 : 1u));
                    return (std::max)(1e-3f, (std::max)(extent, 2.0f*spacing));
                };
                float dist = (std::max)(dx/axisScale(v.extent.x,v.probeCountsX),
                    (std::max)(dy/axisScale(v.extent.y,v.probeCountsY), dz/axisScale(v.extent.z,v.probeCountsZ)));

                using Tier = DDGIVolumeRuntimeComponent::RelevanceTier;
                Tier tier = dist <= 0.5f ? Tier::Inside
                          : dist <= 1.5f ? Tier::Near
                          : dist <= 4.0f ? Tier::Far
                                               : Tier::Dormant;

                // Artist override: a higher priority promotes a volume one tier (a hero interior stays
                // sharp even when the camera steps out).
                if (v.priority > 0 && tier != Tier::Inside)
                    tier = static_cast<Tier>(static_cast<uint32_t>(tier) - 1u);

                const Tier prevTier = r.relevanceTier;
                // Hold a 10% band around tier boundaries to prevent repeated
                // wake-up bursts while the camera hovers at a floor boundary.
                auto graded = [&](float d) {
                    Tier t = d <= 0.5f ? Tier::Inside : d <= 1.5f ? Tier::Near : d <= 4.0f ? Tier::Far : Tier::Dormant;
                    if (v.priority > 0 && t != Tier::Inside) t = static_cast<Tier>(static_cast<uint32_t>(t)-1u);
                    return t;
                };
                if (tier != prevTier &&
                    graded(dist * (static_cast<uint32_t>(tier) > static_cast<uint32_t>(prevTier) ? 0.9f : 1.1f)) == prevTier)
                    tier = prevTier;
                if (static_cast<uint32_t>(tier) < static_cast<uint32_t>(prevTier))
                    r.wakeWarmupRemaining = 8;   // full-rate catch-up, see the field comment
                r.relevanceTier = tier;

                uint32_t want = (std::max)(1u, v.probeUpdateDivisor);
                switch (tier)
                {
                    case Tier::Inside:  break;                       // authored rate
                    case Tier::Near:    want = (std::max)(want, 6u); break;
                    case Tier::Far:     want = (std::max)(want, 8u); break;
                    case Tier::Dormant: want = (std::max)(want, 8u); break;
                    default: break;
                }

                // HYSTERESIS CAP - the least obvious constraint here. DDGIRelight computes
                //     blendSpeed = saturate(max(1 - hysteresis, 0.005) * N)
                // so once N >= 1/(1-hysteresis) the EMA saturates at 1.0: a straight overwrite with no
                // temporal filtering while the ray set keeps being re-jittered - a flickering distant
                // volume that looks like a trace bug, and the shader warns about nothing. Half of that
                // limit keeps a real average.
                const float oneMinusH = (std::max)(1e-4f, 1.0f - v.hysteresis);
                const uint32_t nMax = (std::max)(1u,
                    static_cast<uint32_t>(0.5f / oneMinusH));
                r.divisorCappedByHysteresis = (want > nMax);
                want = (std::min)((std::min)(want, nMax), 8u);

                const bool waking = r.wakeWarmupRemaining > 0;
                if (waking)
                {
                    want = 1u;
                    r.wakeWarmupRemaining--;
                }

                // Only switch divisor on an interleave PHASE BOUNDARY, and
                // restart the phase when it changes. The selector is
                // `(x + 2y + 3z) % N == frameIndex % N`; changing N mid-cycle
                // re-partitions the probes against a counter that is already
                // part-way through the old cycle, so some probes are visited
                // twice in a row and others are skipped for up to 2N frames.
                if (want != r.effectiveDivisor)
                {
                    const uint32_t curN = (std::max)(1u, r.effectiveDivisor);
                    if (waking || (r.frameCounter % curN) == 0u)
                    {
                        r.effectiveDivisor = want;
                        r.frameCounter     = 0;   // clean phase restart
                    }
                }

                // Dormant volumes skip the GPU entirely apart from a heartbeat. The heartbeat period is
                // 31 ON PURPOSE: it must be coprime with both the divisor range [1,8] and the
                // INACTIVE-probe retest period of 16 in DDGIPrepareRayCount. A round 32 shares a factor
                // with 16, so every heartbeat would land on the same retest residue and all but 1/16 of
                // the probes buried in geometry would stay INACTIVE forever - which the sampler skips,
                // i.e. permanent probe-shaped black holes.
                constexpr uint32_t kDormantHeartbeat = 31;
                r.executeThisFrame = (tier != Tier::Dormant)
                                  || (r.framesSinceExecute >= kDormantHeartbeat);

                if (r.executeThisFrame) r.framesSinceExecute = 0;
                else                    r.framesSinceExecute++;
            }
        }

        // The trace CS picks a random point/spot light per ray from the cluster buffer;
        // 0 -> sky-miss + multi-bounce only.
        const uint32_t ddgiLightCount =
            m_clusterPass ? m_clusterPass->GetLightCount() : 0u;

        // Directional sun: read back the FINAL LightCB sun. The light gather wrote the authored
        // directional into LightCB.lightDir/lightColor, then SyncSkyboxIBL's TOD override (earlier
        // this frame, inside BuildScene_UploadLights) replaced it with the active day/night body
        // when Time-of-Day is on. This is the same single sun the deferred Lighting.ps applies, so
        // DDGI's directional GI tracks both the authored light and the TOD cycle - even when the
        // sun is not a SunLightTag entity (the cluster buffer alone never sees that override).
        XMFLOAT3 sunDir{ 0.0f, -1.0f, 0.0f };
        XMFLOAT3 sunCol{ 0.0f,  0.0f, 0.0f };
        // The sky a probe ray misses into is scaled like the path tracer's
        // (PathTracePass FrameData::environmentScale) and the raster fallback's:
        // by the same LightCB iblStrength.
        float skyScale = 1.0f;
        if (const auto* lb = m_lightCB.Upload(m_gfx))
        {
            sunDir = { lb->lightDir[0],   lb->lightDir[1],   lb->lightDir[2]   };
            sunCol = { lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
            skyScale = lb->iblStrength;
        }
        m_ddgiMgr.Tick(m_gfx, vols, runs, activeCount, ddgiLightCount, sunDir, sunCol,
            m_clusterPass ? m_clusterPass->GetWorldLightGridIndex(true) : ~0u, skyScale);

        // Publish the probe-debug lane. Done here rather than at the pass so
        // the render half never has to ask the World anything.
        if (m_ddgiProbeDebugPass)
            m_ddgiProbeDebugPass->SetDebugVolumeSlots(std::move(debugSlots));
    }

    // GC orphaned slots - ALWAYS, even when volPool is null (a world with no DDGIVolume pool).
    // Otherwise a scene with zero volumes keeps the previous world's volumes allocated and
    // m_activeCount stale, the DDGI execution gate (the frame plan's ddgi decision:
    // GetActiveVolumeCount() > 0) keeps passing, and the GPU keeps tracing probes for a world
    // without DDGI. With activeCount == 0 this frees every slot and zeroes the count; it also
    // drops slots whose probe-count layout changed. DestroyVolume uses deferred release, so
    // freeing in the build phase is safe.
    uint32_t claimed[DDGI::kMaxVolumes] = {};
    for (uint32_t i = 0; i < activeCount; ++i)
        claimed[i] = runs[i] ? runs[i]->volumeSlot : 0xFFFFFFFFu;
    m_ddgiMgr.FreeUnclaimedSlots(m_gfx, claimed, activeCount);

    // LightCB integration knobs. ddgiVolumeCount == 0 -> the shader keeps Sky IBL.
    if (lb)
    {
        lb->ddgiVolumeCount         = activeCount;
        lb->ddgiEnabled             = (m_ddgiSettings.ddgiEnabled && activeCount > 0) ? 1u : 0u;
        lb->ddgiDiffuseScale        = m_ddgiSettings.ddgiDiffuseScale;
        lb->ddgiAONearFieldStrength = m_ddgiSettings.ddgiAONearFieldStrength;
        // Scene-wide NPR HDR ceiling, min()ed against the per-material value in Lighting.ps's
        // NPR_PASS. 0 (default) = no global ceiling.
        lb->nprMaxBrightness        = m_ddgiSettings.nprMaxBrightness;

        // DDGI is the lowest tier that occludes the sky light. Without it - DDGI off or no volume -
        // and without ReSTIR GI or the path tracer, the sky reaches every surface as if it stood
        // outdoors (a closed room comes out 8-17x the path tracer): a DEBUG quality, never a
        // shipping one. Said once per change, as INFO (packs bound their WARN count, and a sky
        // scene with no volume is legal content), and shown by the Sky Visibility view.
        const RenderPath path = GetRenderPath();
        const bool giPath  = path == RenderPath::ReSTIR_GI || path == RenderPath::ReSTIR_DI_GI || IsPathTrace(path);
        const bool skyLit  = lb->iblRadianceMips > 0 && lb->iblStrength > 0.0f;
        const bool exposed = skyLit && !giPath && lb->ddgiEnabled == 0u;
        if (exposed != m_skyLightUnoccluded)
        {
            m_skyLightUnoccluded = exposed;
            if (exposed)
                LOG_INFO("[SKYLIGHT] unoccluded: no GI tier occludes the sky light here (DDGI %s, %u volume(s), "
                         "path %s) - debug quality; add a DDGI volume or enable DDGI (plan D-IE2)",
                         m_ddgiSettings.ddgiEnabled ? "on" : "off", activeCount, RenderPathName(path));
            else
                LOG_INFO("[SKYLIGHT] occluded: a GI tier owns the sky light again");
        }
    }

    // No LightingPass DDGI wiring. The volume buffer's heap index goes to
    // LightCB.ddgiVolumeBufIdx in RenderAsync's per-frame `resolve()` block - it MUST be
    // resolved there, not here: the buffer is triple-buffered and GetVolumeBufferSrv() returns
    // the CURRENT frame's ring slot, so a value captured elsewhere in the frame can name the
    // wrong slot. The three kMaxVolumes-wide descriptor-block bases are written into every
    // VolumeGPUDesc by DDGIVolumeManager::Tick (called above). ddgiVolumeCount / ddgiEnabled
    // (set above) remain the only gate.
}

// RECORD half - no World. Everything about the probe arrived in m_probeBakeReq, resolved
// on the game side by ExtractProbeBakeRequest; the result goes back through
// m_probeBakeDone.
void Renderer::ProcessProbeBakeQueue(RHI::CommandList colorLastCL)
{
    if (!m_probeBakeReq.valid)                      return;
    if (!m_probeMgr.GetArrayTexture().IsValid())    return;
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    if (!m_materialBuffer[frameSlot].IsValid())     return;

    const uint32_t cubeSlice = m_probeBakeReq.cubeSlice;

    // Build the bake context; lighting state comes from SkyIBLPass when present.
    ReflectionProbeCapturePass::BakeContext ctx{};
    ctx.probeArray       = &m_probeMgr.GetArrayTexture();
    ctx.probePos         = m_probeBakeReq.capturePos;   // the capture origin, offset applied
    if (m_skyIBLPass)
    {
        // CONVENTION FLIP - do not remove. SkyIBLPass stores the vector pointing TOWARDS the body
        // (set from -LightCB.lightDir in Renderer_IBL); BakeContext::sunDir and ProbeCapture.ps use
        // the direction the light TRAVELS, like LightCB.lightDir, because the PS does
        // `L = normalize(-c_sunDir)` as Lighting.ps does `L = normalize(-lightDir)`. Without the
        // flip every probe is lit from the hemisphere opposite the deferred pass (shadows still
        // keyed to the true sun, so it reads as wrongly lit backfaces).
        const DirectX::XMFLOAT3 toSun = m_skyIBLPass->GetSunDir();
        ctx.sunDir      = { -toSun.x, -toSun.y, -toSun.z };
        ctx.sunColor    = m_skyIBLPass->GetSunColor();
        ctx.skySHSrv    = m_skyIBLPass->GetSHSrvHandle();
        ctx.iblStrength = m_skyIBLPass->GetIBLStrength();
    }
    ctx.ambientScale = m_ddgiSettings.reflectionProbeBakeAmbient;

    // CSM sun shadowing for the bake: frame-global cascades from ShadowSystem. The shadow
    // array is always allocated (even with no light); shadowStrength gates whether the capture
    // PS samples it. Cascades are built for the main camera, so probes within the camera's
    // cascade range bake shadowed.
    if (m_shadowPass && m_shadowSystem && m_shadowSystem->HasValidLight())
    {
        ctx.shadowArraySrv = m_shadowPass->GetShadowArrayGpuHandle();
        const DirectX::XMFLOAT4X4* casc = m_shadowSystem->CascadeMatricesTransposed();
        const float*               spl  = m_shadowSystem->CascadeSplits();
        for (int c = 0; c < ShadowSystem::kCascadeCount && c < 4; ++c)
            ctx.cascadeVP[c] = casc[c];
        ctx.cascadeSplits  = { spl[0], spl[1], spl[2], spl[3] };
        ctx.camPos         = FrameViewRead().camera.position;
        ctx.camFwd         = FrameViewRead().view.cameraForward;
        ctx.shadowStrength = ShadowSystem::ShadowStrength();
        ctx.shadowTexel    = 1.0f / static_cast<float>(m_shadowPass->GetShadowResolution());
    }
    ctx.materialBufSrv   = m_gfx.GetBufferSRVGpuHandle(m_materialBuffer[frameSlot]);
    ctx.bindlessTexTable = m_gfx.GetBindlessTextureTableGpuHandle();
    ctx.bindlessBufTable = m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle();
    // The bake reuses GBuffer.vs with draw-local packet offsets, so under retained instances
    // it binds the DEFAULT buffer + this frame's remap idx (carried at byte 228 of the capture
    // CB - see ReflectionProbeCapturePass).
    ctx.instanceBuffer   = RetainedInstancesActive()
        ? &m_instanceBufferGPU
        : &m_instanceBuffer[m_gfx.GetFrameIndex()];
    ctx.instanceRemapIdx = RetainedInstancesActive()
        ? m_instanceRemapHeapIdx[m_gfx.GetFrameIndex()] : 0u;
    ctx.meshDescBuffer   = &m_meshMgr.GetDescriptorHeap().CurrentMeshDescBuffer();

    // ---- Each face is a view ----
    // The face's 90-degree frustum from the capture point to the bake's far plane, through
    // Renderer::CullView with the world-cull preview lane's rules: an entity the BVH has no
    // leaf for is kept (primitives; every entity while GPU culling is off), renderInMainPass
    // and the ReflectionProbe bit are honoured, and every probe's own primitive (the editor's
    // viz ball) is excluded. Lists: Opaque, then the probe's own copies of what the main view
    // dropped or converted (DrawFilter::ProbeView), in the order the capture pass draws them.
    // The vectors live until BakeProbe returns (synchronous record).
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -probecull_legacy (harness): the bake without per-face views - the BVH asked with the
    // influence box around the probe and the capture point, the main Opaque and Shadow lists
    // filtered by it (an entity with no leaf dropped), one list for all six faces, the previous
    // draw's t3 kept. The reverse arm of the faces' CullView (tests/unit/test_probe_cull.py).
    static const bool s_legacyCull = Sys::CommandLine::Get().Has("-probecull_legacy");
#else
    constexpr bool s_legacyCull = false;
#endif
    std::vector<DrawPacket> legacyPackets;
    if (s_legacyCull)
    {
        const DirectX::XMFLOAT3 ih = m_probeBakeReq.influenceHalfExtents;
        const float influenceR = (m_probeBakeReq.influenceShape == EInfluenceShape::Sphere)
            ? m_probeBakeReq.influenceOuterRadius
            : std::sqrt(ih.x * ih.x + ih.y * ih.y + ih.z * ih.z);
        const DirectX::XMFLOAT3& pc = m_probeBakeReq.probePos;
        const DirectX::XMFLOAT3& cc = m_probeBakeReq.capturePos;
        SceneBVH::AABB influenceAabb;
        influenceAabb.min = { (std::min)(pc.x, cc.x) - influenceR, (std::min)(pc.y, cc.y) - influenceR,
                              (std::min)(pc.z, cc.z) - influenceR };
        influenceAabb.max = { (std::max)(pc.x, cc.x) + influenceR, (std::max)(pc.y, cc.y) + influenceR,
                              (std::max)(pc.z, cc.z) + influenceR };
        std::vector<Entity> bvhVisible;
        m_sceneBVH.QueryAABB(influenceAabb, bvhVisible);
        std::sort(bvhVisible.begin(), bvhVisible.end());
        auto filterList = [&](DrawList src)
        {
            for (const DrawPacket& dp : src)
            {
                const uint32_t endSlot = (std::min)(dp.instanceOffset + dp.instanceCount, m_maxInstances);
                for (uint32_t i = dp.instanceOffset; i < endSlot; ++i)
                    if (std::binary_search(bvhVisible.begin(), bvhVisible.end(), EntityForDrawLocalInstance(i)))
                    {
                        legacyPackets.push_back(dp);
                        break;
                    }
            }
        };
        filterList(GetDrawList(DrawFilter::Opaque));
        filterList(GetDrawList(DrawFilter::Shadow));
        for (uint32_t face = 0; face < 6; ++face)
            ctx.faceDraws[face] = DrawList(legacyPackets.data(), legacyPackets.size());
        ctx.legacyTextureBind = true;
    }
    static const char* const kFaceViewNames[6] = { "probe+X", "probe-X", "probe+Y", "probe-Y", "probe+Z", "probe-Z" };
    std::vector<DrawPacket> facePackets[6];
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -probe_mainlists (harness): Opaque and the main view's shadow-only packets, no probe
    // copies (BuildScene_SelectProbes registers no probe volume). The reverse arm of the probe
    // copies (tests/unit/test_probe_cull.py).
    static const bool s_mainLists = Sys::CommandLine::Get().Has("-probe_mainlists");
#else
    constexpr bool s_mainLists = false;
#endif
    ViewCullRequest faceReq;
    faceReq.lists[0]     = DrawFilter::Opaque;
    faceReq.lists[1]     = s_mainLists ? DrawFilter::Shadow : DrawFilter::ProbeView;
    faceReq.secondary[1] = !s_mainLists;
    faceReq.listCount    = 2;
    faceReq.exclude      = m_probeBakeReq.probeMeshes.data();
    faceReq.excludeCount = static_cast<uint32_t>(m_probeBakeReq.probeMeshes.size());
    ViewCullStats faceStats[6];
    if (s_legacyCull)
        for (ViewCullStats& st : faceStats) st.kept = static_cast<uint32_t>(legacyPackets.size());
    for (uint32_t face = 0; face < 6 && !s_legacyCull; ++face)
    {
        RG::SceneView faceView;
        faceView.name = kFaceViewNames[face];
        CullPlanesFrom(ExtractFrustumPlanes(ReflectionProbeCapturePass::FaceViewProj(ctx.probePos, face)),
                       faceView.cull.volume);
        const DirectX::XMFLOAT3 fwd = ReflectionProbeCapturePass::FaceForward(face);
        faceView.cull.eye[0] = ctx.probePos.x; faceView.cull.eye[1] = ctx.probePos.y; faceView.cull.eye[2] = ctx.probePos.z;
        faceView.cull.forward[0] = fwd.x;      faceView.cull.forward[1] = fwd.y;      faceView.cull.forward[2] = fwd.z;
        faceView.cull.viewBit = ViewBit::ReflectionProbe;
#if defined(WITH_HARNESS) && WITH_HARNESS
        // -probe_nocull (harness): every face draws every packet - the reverse
        // arm of the per-face cull, whose faces are the same (it only skips what
        // a face cannot see; tests/unit/test_probe_cull.py).
        static const bool s_noCull = Sys::CommandLine::Get().Has("-probe_nocull");
        if (s_noCull) faceView.cull.volume.kind = RG::CullVolume::Kind::None;
#endif
        CullView(faceView, faceReq, facePackets[face], &faceStats[face]);
        ctx.faceDraws[face] = DrawList(facePackets[face].data(), facePackets[face].size());
    }
    ctx.defaultWhiteSrv    = m_gbufferPass ? m_gbufferPass->GetDefaultWhiteSrvHandle()      : 0;
    ctx.defaultFlatNormSrv = m_gbufferPass ? m_gbufferPass->GetDefaultFlatNormalSrvHandle() : 0;
    // Skybox capture: reuse the SkyboxPass cube VB + the active sky cubemap. Either 0 -> sky
    // draw skipped.
    if (m_skyboxPass)
        ctx.skyCubeVB = &m_skyboxPass->GetCubeVB();
    if (m_skyIBLPass)
        ctx.skyCubemapSrv = m_skyIBLPass->ResolveSkyboxSrvHandle(/*staticFallback=*/0);

    // Dedicated graphics CL sequenced after the main graph (as the Hi-Z stage at Phase 4.5 of
    // RenderAsync is).
    RHI::CommandList bakeCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
    m_gfx.SetCommandListName(bakeCL, "ProbeBake");
    bakeCL.gfx = &m_gfx;
    if (colorLastCL.IsValid())
        m_gfx.AddCommandListDependency(bakeCL, colorLastCL);

    uint32_t bakeRegion = m_gfx.BeginGPUTimestamp(bakeCL, "ProbeBake");
    m_probeMgr.GetCapturePass().BakeProbe(bakeCL, cubeSlice, ctx);
    m_gfx.EndGPUTimestamp(bakeCL, bakeRegion);
    // What -probedump reads back (the count last: release).
    {
        const uint64_t n = m_probeBakeCount.load(std::memory_order_relaxed);
        ProbeBakeRecord& rec = m_probeBakeLog[n % kProbeBakeLog];
        rec.entity = m_probeBakeReq.entity;
        rec.slice  = cubeSlice;
        rec.frame  = FrameViewRead().currentFrame;
        for (uint32_t face = 0; face < 6; ++face)
        {
            rec.kept[face]   = faceStats[face].kept;
            rec.copies[face] = faceStats[face].keptSecondary;
        }
        m_probeBakeCount.store(n + 1, std::memory_order_release);
    }

    // Return path: the four component fields are written on the game side by
    // ApplyProbeBakeFeedback. Only the entity has to travel - "a bake happened"
    // fully determines what gets written.
    m_probeBakeDone = m_probeBakeReq.entity;
    m_probeBakeReq.valid = false;
}

// EXTRACT half (game side): pops the queue and resolves the probe out of the World so the
// record half needs none of it.
void Renderer::ExtractProbeBakeRequest(World& world)
{
    m_probeBakeReq.valid = false;
    if (m_probeMgr.BakeQueueEmpty()) return;

    const uint32_t cubeSlice = m_probeMgr.PeekBake();
    m_probeMgr.PopBake();

    // Resolve slice -> entity -> component (linear scan; N <= 64).
    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    auto& probeData = probePool->Data();
    auto& probeEnts = probePool->Entities();

    for (size_t i = 0; i < probeEnts.size(); ++i)
    {
        if (probeData[i].cubemapSlice != cubeSlice) continue;
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(probeEnts[i]);
        if (!gt) return;
        m_probeBakeReq.entity               = probeEnts[i];
        m_probeBakeReq.cubeSlice            = cubeSlice;
        m_probeBakeReq.probePos             = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
        // The capture point the shading assumes (BuildScene_UploadProbes' capturePosWS: parallax
        // and the NORMALIZE flag).
        m_probeBakeReq.capturePos           = LocalOffsetToWorld(ExtractRigidFrame(gt->matrix),
                                                                 probeData[i].captureOffsetLS);
        m_probeBakeReq.influenceHalfExtents = probeData[i].influenceHalfExtents;
        m_probeBakeReq.influenceShape       = probeData[i].influenceShape;
        m_probeBakeReq.influenceOuterRadius = probeData[i].influenceOuterRadius;
        // The probes' own primitives (the editor's viz balls) - the faces' CullView excludes them.
        m_probeBakeReq.probeMeshes.clear();
        for (Entity pe : probeEnts)
            if (world.HasComponent<MeshHandle>(pe)) m_probeBakeReq.probeMeshes.push_back(pe);
        m_probeBakeReq.valid                = true;

        static bool s_reportedBake = false;
        if (!s_reportedBake)
        {
            s_reportedBake = true;
            LOG_INFO("[SPLITAUDIT][PROBEBAKE] extract: slice=%u entity=%u - the bake "
                     "path is exercised", cubeSlice, (unsigned)probeEnts[i]);
        }
        return;
    }
    // Probe disappeared between enqueue and now: the slice is already popped,
    // so the request simply stays invalid and nothing is recorded.
}

// APPLY half (game side): runs first in BuildScene_SelectProbes, before the enqueue sweep,
// which re-enqueues any probe not yet marked baked - applied after it, the probe baked last
// frame would be baked a second time, every time.
void Renderer::ApplyProbeBakeFeedback(World& world)
{
    if (m_probeBakeDone == NullEntity) return;
    const Entity e = m_probeBakeDone;
    m_probeBakeDone = NullEntity;

    auto* probePool = world.GetPool<ReflectionProbeComponent>();
    if (!probePool) return;
    ReflectionProbeComponent* c = probePool->Get(e);
    if (!c) return;   // destroyed while the bake was in flight

    c->SetBaked(true);
    c->bakeDataVersion = ReflectionProbeComponent::kCurrentBakeDataVersion;
    c->ClearRebakeRequest();
    // Seed realtime tick so freshly-baked realtime probes wait a full interval.
    c->lastBakedFrame = FrameViewWrite().currentFrame;
}
