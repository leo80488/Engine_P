// Renderer_Meshlet.cpp - the Renderer's half of the meshlet geometry path.
//
//   game half   ReadMeshletFlags (once), the -meshlets_at toggle (BeginFrame),
//               MeshletBlocksForSlot (gather: the geometric half of eligibility)
//               and MeshletPathPreconditions (the SceneEmit latch: the path half).
//   render half WriteMeshletCullCB (b4), GBufferPass::SetMeshletFrame and the
//               [MESHLET] frame line.
//
// GBufferPass owns the draw itself (RecordChunk / the CPU depth prepass).
#include "Renderer/Renderer.h"

#include "ECS/HierarchyComponents.h"   // ViewBit
#include "Graphics/MeshDescriptorHeap.h"
#include "../../shaders/meshlet_cull.hlsli"   // MeshletXformPoint / MeshletMaxScale (explain's LOD)
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/HiZPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void Renderer::ReadMeshletFlags()
{
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    // Read unconditionally: a pack that passes one must never see it reported
    // as unread just because the path was off.
    if (cl.Has("-meshlets"))
    {
        m_meshletsRequested = cl.GetInt("-meshlets", 0) != 0;
        m_meshletsPinned    = true;
    }
    if (cl.Has("-meshletlod"))
    {
        const float t = cl.GetFloat("-meshletlod", m_meshletLodTau);
        m_meshletLodTau    = t < 0.0f ? 0.0f : t;
        m_meshletLodPinned = true;
    }
    if (cl.Has("-meshletshadowlod"))
    {
        const float t = cl.GetFloat("-meshletshadowlod", m_meshletShadowLodTau);
        m_meshletShadowLodTau    = t < 0.0f ? 0.0f : t;
        m_meshletShadowLodPinned = true;
    }
    m_meshletShadowLodAtFrame = -1;
    if (cl.Has("-meshletshadowlod_at"))
    {
        // <frame>:<tau texels>
        const std::string spec = cl.Get("-meshletshadowlod_at");
        const size_t colon = spec.find(':');
        char* endF = nullptr;
        char* endV = nullptr;
        const long long f = colon == std::string::npos ? -1 : std::strtoll(spec.c_str(), &endF, 10);
        const double v = colon == std::string::npos ? -1.0 : std::strtod(spec.c_str() + colon + 1, &endV);
        if (ENGINE_CHECK(colon != std::string::npos && endF == spec.c_str() + colon && f >= 0 &&
                         endV && *endV == '\0' && v >= 0.0,
                         "-meshletshadowlod_at='%s' is not <frame>:<tau >= 0>; ignored", spec.c_str()))
        {
            m_meshletShadowLodAtFrame = f;
            m_meshletShadowLodAtValue = static_cast<float>(v);
        }
    }
    m_meshletLodAtFrame = -1;
    if (cl.Has("-meshletlod_at"))
    {
        // <frame>:<tau>
        const std::string spec = cl.Get("-meshletlod_at");
        const size_t colon = spec.find(':');
        char* endF = nullptr;
        char* endV = nullptr;
        const long long f = colon == std::string::npos ? -1 : std::strtoll(spec.c_str(), &endF, 10);
        const double v = colon == std::string::npos ? -1.0 : std::strtod(spec.c_str() + colon + 1, &endV);
        if (ENGINE_CHECK(colon != std::string::npos && endF == spec.c_str() + colon && f >= 0 &&
                         endV && *endV == '\0' && v >= 0.0,
                         "-meshletlod_at='%s' is not <frame>:<tau >= 0>; ignored", spec.c_str()))
        {
            m_meshletLodAtFrame = f;
            m_meshletLodAtValue = static_cast<float>(v);
        }
    }
    m_meshletsKilled    = cl.Has("-meshletsoff");
    m_meshletVerify     = cl.Has("-meshletverify");
    const int logEvery  = cl.GetInt("-meshletlog_every", 0);
    m_meshletLogEvery   = logEvery > 0 ? static_cast<uint32_t>(logEvery) : 0u;

    // -meshletcull=<mask>: the culling stages from the start (kMeshletCull*:
    // 1 frustum, 2 cone, 4 Hi-Z); -meshletcull_at=<frame>:<mask> switches later.
    if (cl.Has("-meshletcull"))
        m_meshletCullMask = static_cast<uint32_t>(cl.GetInt("-meshletcull", int(m_meshletCullMask))) &
                            (MeshletShared::kMeshletCullFrustum | MeshletShared::kMeshletCullCone |
                             MeshletShared::kMeshletCullHiZ);
    m_meshletCullAtFrame = -1;
    if (cl.Has("-meshletcull_at"))
    {
        const std::string spec = cl.Get("-meshletcull_at");
        const size_t colon = spec.find(':');
        char* endF = nullptr;
        char* endV = nullptr;
        const long long f = colon == std::string::npos ? -1 : std::strtoll(spec.c_str(), &endF, 10);
        const long v = colon == std::string::npos ? -1 : std::strtol(spec.c_str() + colon + 1, &endV, 10);
        if (ENGINE_CHECK(colon != std::string::npos && endF == spec.c_str() + colon && f >= 0 &&
                         endV && *endV == '\0' && v >= 0 && v <= 7,
                         "-meshletcull_at='%s' is not <frame>:<mask 0..7>; ignored", spec.c_str()))
        {
            m_meshletCullAtFrame = f;
            m_meshletCullAtValue = static_cast<uint32_t>(v);
        }
    }

    m_meshletsAtFrame = -1;
    if (cl.Has("-meshlets_at"))
    {
        // <frame>:<0|1>
        const std::string spec = cl.Get("-meshlets_at");
        const size_t colon = spec.find(':');
        char* endF = nullptr;
        char* endV = nullptr;
        const long long f = colon == std::string::npos ? -1 : std::strtoll(spec.c_str(), &endF, 10);
        const long v = colon == std::string::npos ? -1 : std::strtol(spec.c_str() + colon + 1, &endV, 10);
        if (ENGINE_CHECK(colon != std::string::npos && endF == spec.c_str() + colon && f >= 0 &&
                         endV && *endV == '\0' && (v == 0 || v == 1),
                         "-meshlets_at='%s' is not <frame>:<0|1>; ignored", spec.c_str()))
        {
            m_meshletsAtFrame = f;
            m_meshletsAtValue = v != 0;
        }
    }
    LOG_INFO("[MESHLET] flags meshlets=%d pinned=%d killed=%d at=%lld:%d cull=%u cullAt=%lld:%u lod=%.3f lodPinned=%d "
             "lodAt=%lld:%.3f shadowLod=%.3f shadowLodPinned=%d shadowLodAt=%lld:%.3f logEvery=%u meshShader=%d "
             "verify=%d",
             m_meshletsRequested ? 1 : 0, m_meshletsPinned ? 1 : 0, m_meshletsKilled ? 1 : 0,
             static_cast<long long>(m_meshletsAtFrame), m_meshletsAtValue ? 1 : 0,
             m_meshletCullMask, static_cast<long long>(m_meshletCullAtFrame), m_meshletCullAtValue,
             m_meshletLodTau, m_meshletLodPinned ? 1 : 0, static_cast<long long>(m_meshletLodAtFrame),
             m_meshletLodAtValue, m_meshletShadowLodTau, m_meshletShadowLodPinned ? 1 : 0,
             static_cast<long long>(m_meshletShadowLodAtFrame), m_meshletShadowLodAtValue, m_meshletLogEvery,
             m_gfx.GetCapabilities().meshShader ? 1 : 0, m_meshletVerify ? 1 : 0);
}

bool Renderer::MeshletPathPreconditions(const char*& reason) const
{
    if (m_meshletsKilled)                      { reason = "killed";         return false; }
    if (!m_meshletsRequested)                  { reason = "off";            return false; }
    if (!m_gfx.GetCapabilities().meshShader)   { reason = "no_mesh_shader"; return false; }
    if (!m_gbufferPass)                        { reason = "no_pass";        return false; }
    if (!m_gbufferPass->MeshletPsoReady())     { reason = "pso_failed";     return false; }
    reason = "";
    return true;
}

uint16_t Renderer::MeshletBlocksForSlot(uint32_t meshDescSlot, bool skinned, uint32_t customVSID) const
{
    // The geometric half of eligibility: skinned draws do not read the library
    // VB, and a custom VS (WPO) moves vertices the MS would not.
    if (skinned || customVSID != 0) return 0;
    const MeshDescriptorHeap::MeshletInfo info = m_meshMgr.GetDescriptorHeap().GetMeshletInfo(meshDescSlot);
    if (info.blobBufferIdx == MeshDescriptorHeap::kNoMeshlets.blobBufferIdx || info.clusterCount == 0)
        return 0;
    const uint32_t blocks = (info.clusterCount + MeshletShared::kMeshletASGroupSize - 1) /
                            MeshletShared::kMeshletASGroupSize;
    // A dispatch dimension is capped at 65535: such an entry stays on the VS.
    return blocks <= 65535u ? static_cast<uint16_t>(blocks) : uint16_t(0);
}

// An indirect command joins a meshlet group when the path is latched for the
// frame, the compaction CS runs (the conversion lives in it; the in-place cull
// and the CPU zero-out paths keep DRAW), the packet is eligible and fits one
// DispatchMesh with ALL its instances (the CS can only lower Y), and its custom
// PS links to the MS (one that failed keeps the VS: GBufferPass::MeshletPsBlocked).
bool Renderer::MeshletIndirectPacket(const DrawPacket& dp) const
{
    const SceneEmit& se = SceneEmitWrite();
    if (!se.meshletFrameActive || !se.gpuCompactionFrameActive) return false;
    if (!GBufferPass::MeshletEligible(dp)) return false;
    if (!GBufferPass::MeshletDispatchFits(dp.meshletBlocks, dp.instanceCount)) return false;
    return dp.customPSID == 0 || !m_gbufferPass || !m_gbufferPass->MeshletPsBlocked(dp.customPSID);
}

int Renderer::ShadowRetainedGroup(const DrawPacket& dp) const
{
    const int g = ClassifyShadowDrawGroup(dp, ViewBit::ShadowAny);
    if (g < 0) return g;
    return (SceneEmitWrite().shadowMeshletFrameActive && ShadowMeshletEligible(dp))
               ? g + ShadowDrawGroup::MeshletOffset : g;
}

// One CSM cascade's meshlet cull CB: frustum only (no cone - the buckets draw
// back faces and both faces; no Hi-Z - there is no shadow pyramid), the
// statistics, and LOD in shadow texels when the shadow tau > 0.
void Renderer::WriteMeshletShadowBank(uint32_t cascade, const float (&planes)[6][4],
                                      const DirectX::XMFLOAT4X4& cascadeVP)
{
    if (cascade >= 3 || !m_meshletShadowCB.IsValid()) return;
    MeshletShadowBanks* banks = m_meshletShadowCB.Current(m_gfx);
    if (!banks) return;
    MeshletShared::MeshletCullCB& cb = banks->cascade[cascade];
    cb = MeshletShared::MeshletCullCB{};
    for (int p = 0; p < 6; ++p)
        cb.frustumPlanes[p] = { planes[p][0], planes[p][1], planes[p][2], planes[p][3] };
    const FrameView& fv = FrameViewRead();
    cb.cameraPosWS = { fv.view.cameraPosition.x, fv.view.cameraPosition.y, fv.view.cameraPosition.z };
    cb.statsUavIdx = m_shadowPass ? m_shadowPass->MeshletStatsUavIndex(m_gfx) : MeshletShared::kMeshletNoData;
    cb.cullMask    = MeshletShared::kMeshletCullFrustum |
                     (cb.statsUavIdx != MeshletShared::kMeshletNoData ? MeshletShared::kMeshletCullStats : 0u);
    cb.sidecarIdx  = m_meshMgr.GetDescriptorHeap().CurrentMeshletInfoBindless();
    cb.lodOrtho    = 1;
    cb.zNear       = fv.view.nearZ;
    cb.pad0.x      = cascade * MeshletShared::kMeshletStatsCount;   // this cascade's stats slice
    // Orthographic LOD in shadow texels. lodScale = texels per world unit of this
    // cascade: half the map's resolution times the longer of the VP's x / y
    // gradients (row-vector convention: clip.x = dot(world, column 0)); a group's
    // projected error is errorWS * lodScale.
    const auto& m = cascadeVP.m;
    const float gx = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]);
    const float gy = std::sqrt(m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1]);
    const float res = m_shadowPass ? static_cast<float>(m_shadowPass->GetShadowResolution()) : 0.0f;
    cb.lodScale     = 0.5f * res * (gx > gy ? gx : gy);
    cb.lodThreshold = SceneEmitRead().meshletShadowLodTau;
    if (cb.lodThreshold > 0.0f && cb.lodScale > 0.0f)
        cb.cullMask |= MeshletShared::kMeshletCullLod;
}

void Renderer::VerifyMeshletGroups(uint32_t frameSlot)
{
    const SceneEmit& se = SceneEmitWrite();
    const auto* args = static_cast<const IndirectDrawCommand*>(m_indirectArgMapped[frameSlot]);
    const auto* meta = se.gpuCompactionFrameActive ? static_cast<const uint32_t*>(m_cmdGroupMetaMapped[frameSlot])
                                                    : nullptr;
    const auto& packets = DrawPacketsWrite();
    uint32_t meshletGroups = 0, cmds = 0, bad = 0, firstBad = 0xFFFFFFFFu;
    for (const IndirectGroup& g : se.indirectGroups)
    {
        if (g.meshlet) ++meshletGroups;
        const uint32_t base = g.argOffset / static_cast<uint32_t>(sizeof(IndirectDrawCommand));
        for (uint32_t c = 0; c < g.cmdCount; ++c)
        {
            const uint32_t di = base + c;
            if (di >= packets.size()) { ++bad; continue; }
            const DrawPacket& dp = packets[di];
            bool ok = MeshletIndirectPacket(dp) == (g.meshlet != 0);   // the group is homogeneous
            if (g.meshlet)
            {
                ++cmds;
                ok = ok && GBufferPass::MeshletDispatchFits(dp.meshletBlocks, dp.instanceCount);
                ok = ok && args && args[di].startInstanceLocation == dp.meshletBlocks &&
                     args[di].prevPosInfo == GBufferPass::MeshletDrawFlags(dp);
                ok = ok && meta && (meta[di] & 0x80000000u) != 0;
            }
            else
            {
                ok = ok && (!args || args[di].startInstanceLocation == 0);
                ok = ok && (!meta || meta[di] == 0xFFFFFFFFu || (meta[di] & 0x80000000u) == 0);
            }
            if (!ok) { ++bad; if (firstBad == 0xFFFFFFFFu) firstBad = di; }
        }
    }
    // The shadow side: every packet's retained bucket, its meta, and the pristine
    // blocks a CSM meshlet twin's DISPATCH_MESH reads.
    const auto* smeta = se.shadowArgsFrameActive ? static_cast<const uint32_t*>(m_shadowCmdMetaMapped[frameSlot])
                                                 : nullptr;
    uint32_t shadowMeshlet = 0, shadowBad = 0;
    if (smeta)
    {
        const uint32_t metaCount = (std::min)(se.indirectDrawCount, m_maxInstances);
        for (uint32_t di = 0; di < metaCount && di < packets.size(); ++di)
        {
            const DrawPacket& dp = packets[di];
            const int g = ShadowRetainedGroup(dp);
            bool ok = g < 0 ? smeta[di] == 0xFFFFFFFFu : (smeta[di] & 0xFFFFu) == static_cast<uint32_t>(g);
            if (g >= ShadowDrawGroup::MeshletOffset)
            {
                ++shadowMeshlet;
                ok = ok && args && args[di].startInstanceLocation == dp.meshletBlocks;
            }
            if (!ok) ++shadowBad;
        }
    }
    LOG_INFO("[MESHLET_VERIFY] frame=%llu groups=%zu meshletGroups=%u cmds=%u bad=%u firstBad=%d "
             "shadowMeshlet=%u shadowBad=%u match=%d",
             static_cast<unsigned long long>(FrameViewEmit().currentFrame), se.indirectGroups.size(),
             meshletGroups, cmds, bad, firstBad == 0xFFFFFFFFu ? -1 : static_cast<int>(firstBad),
             shadowMeshlet, shadowBad, (bad == 0 && shadowBad == 0) ? 1 : 0);
}

void Renderer::WriteMeshletCullCB()
{
    if (!m_meshletCullCB.IsValid()) return;
    MeshletShared::MeshletCullCB* cb = m_meshletCullCB.Current(m_gfx);
    if (!cb) return;
    *cb = MeshletShared::MeshletCullCB{};
    const FrameView& fv = FrameViewRead();
    for (int p = 0; p < 6; ++p)
        cb->frustumPlanes[p] = { fv.view.frustum[p].normal.x, fv.view.frustum[p].normal.y,
                                 fv.view.frustum[p].normal.z, fv.view.frustum[p].distance };
    cb->cameraPosWS = { fv.view.cameraPosition.x, fv.view.cameraPosition.y, fv.view.cameraPosition.z };
    // The stages the scene build latched for this frame, plus the statistics when
    // the counters exist and LOD when tau > 0.
    cb->statsUavIdx = m_gbufferPass ? m_gbufferPass->MeshletStatsUavIndex(m_gfx) : MeshletShared::kMeshletNoData;
    const float tau = SceneEmitRead().meshletLodTau;
    cb->cullMask    = SceneEmitRead().meshletCullMask |
                      (cb->statsUavIdx != MeshletShared::kMeshletNoData ? MeshletShared::kMeshletCullStats : 0u) |
                      (tau > 0.0f ? MeshletShared::kMeshletCullLod : 0u);
    cb->sidecarIdx  = m_meshMgr.GetDescriptorHeap().CurrentMeshletInfoBindless();
    {
        const ViewMode vm = m_viewMode.load(std::memory_order_relaxed);
        cb->debugMode = vm == ViewMode::Meshlets   ? MeshletShared::kMeshletDebugCluster
                      : vm == ViewMode::MeshletLod ? MeshletShared::kMeshletDebugLevel
                                                   : MeshletShared::kMeshletDebugOff;
    }
    const MeshletLodView lv = GetMeshletLodView();
    cb->lodScale     = lv.lodScale;
    cb->lodThreshold = lv.tau;
    cb->lodOrtho     = lv.ortho;
    cb->zNear        = lv.zNear;
    // Hi-Z: last frame's pyramid with the VP it was captured with - the pyramid,
    // matrix and gate InstanceCull uses this frame (HiZUsableThisFrame), so the
    // instance and the cluster level agree. hiZValid 0 = the AS skips the stage
    // (no pyramid yet, a camera cut or jump, Hi-Z switched off).
    if ((cb->cullMask & MeshletShared::kMeshletCullHiZ) != 0 && HiZUsableThisFrame())
    {
        const uint64_t h   = m_hiZPass->GetHiZSrvHandle();
        const uint32_t idx = h ? m_gfx.DescriptorHeapIndexFromGpuHandle(h) : ~0u;
        if (idx != ~0u)
        {
            cb->hiZValid    = 1;
            cb->hiZWidth    = m_hiZPass->GetHiZValidWidth();
            cb->hiZHeight   = m_hiZPass->GetHiZValidHeight();
            cb->hiZMipCount = m_hiZPass->GetHiZMipCount();
            cb->hiZSrvIdx   = idx;
            // Transposed like every engine matrix (CullingPass::SetViewProj):
            // hiz_cull.hlsli applies it as mul(float4(p, 1), hiZViewProj).
            DirectX::XMFLOAT4X4 t;
            DirectX::XMStoreFloat4x4(&t, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&m_mainView.hiz.viewProj)));
            static_assert(sizeof(cb->hiZViewProj) == sizeof(t), "MeshletCullCB::hiZViewProj is a float4x4");
            std::memcpy(&cb->hiZViewProj, &t, sizeof(t));
        }
    }
}

Renderer::MeshletLodView Renderer::GetMeshletLodView() const
{
    MeshletLodView v;
    const FrameView& fv = FrameViewRead();
    v.tau = SceneEmitRead().meshletLodTau;
    v.camera[0] = fv.view.cameraPosition.x;
    v.camera[1] = fv.view.cameraPosition.y;
    v.camera[2] = fv.view.cameraPosition.z;
    // Pixels per world unit at distance 1 (perspective) / per unit (ortho):
    // renderHeight * P[1][1] / 2 covers both, P[1][1] being 1/tan(fovY/2) or
    // 2/height. The RENDER height, so FSR2 / DLSS measure what is rasterized.
    const float p11 = fv.view.projMatrixNoJitter.m[1][1];
    v.lodScale = 0.5f * static_cast<float>(fv.renderHeight ? fv.renderHeight : fv.vpHeight) * p11;
    v.ortho    = fv.view.projMatrixNoJitter.m[3][3] == 1.0f ? 1u : 0u;
    v.zNear    = fv.view.nearZ;
    return v;
}

void Renderer::LogMeshletFrame()
{
    if (!m_gbufferPass) return;
    const GBufferPass::MeshletFrameStats s = m_gbufferPass->GetMeshletFrameStats();
    const SceneEmit& emit = SceneEmitRead();
    // path: where the meshlet-eligible packets went this frame.
    //   cpu       the CPU per-draw loop drew them through the mesh pipeline
    //   vs        they drew through GBuffer.vs - `reason` says why
    //   indirect  meshlet groups of the ExecuteIndirect path
    const char* path = "vs";
    const char* reason = emit.meshletReason ? emit.meshletReason : "";
    if (emit.meshletFrameActive)
    {
        if (s.indirect)
        {
            if (s.indirectCmds > 0 || s.groupFallback > 0) { path = "indirect"; reason = ""; }
            else if (!emit.gpuCompactionFrameActive) reason = "no_compaction";   // the conversion is the compaction CS's
            else                                     reason = "no_eligible_packet";
        }
        else if (s.packets == 0)  reason = "no_eligible_packet";
        else                      { path = "cpu"; reason = ""; }
    }
    const unsigned long long frame = static_cast<unsigned long long>(FrameViewRead().currentFrame);
    std::string key = std::string(path) + "|" + reason;
    const bool changed = key != m_meshletLastLogKey;
    const bool periodic = m_meshletLogEvery > 0 && frame % m_meshletLogEvery == 0;
    if (!changed && !periodic) return;
    m_meshletLastLogKey = std::move(key);

    // Census of this frame's main-view opaque packets: how many the geometric
    // half admitted (meshletBlocks != 0) and, of the rest, why not - so a
    // "packets=0" line says which gate the content is stuck at.
    uint32_t opaque = 0, geom = 0, noData = 0, skinned = 0, customVS = 0, customPS = 0, dissolve = 0;
    for (const DrawPacket& dp : DrawPacketsRead())
    {
        if (dp.filter != DrawFilter::Opaque || !dp.renderInMainPass ||
            (dp.viewMask & ViewBit::MainCamera) == 0) continue;
        ++opaque;
        if (dp.meshletBlocks != 0)
        {
            ++geom;
            if (dp.customPSID != 0)       ++customPS;   // informational: a custom PS is allowed
            if (dp.batchHasDissolve)      ++dissolve;
        }
        else if (dp.prevPosElementBase != 0xFFFFFFFFu) ++skinned;
        else if (dp.customVSID != 0)                    ++customVS;
        else                                            ++noData;
    }
    // The amplification shader's own counters (~kFramesInFlight frames old).
    using namespace MeshletShared;
    const GBufferPass::MeshletGpuStats g = m_gbufferPass->GetMeshletGpuStats();
    const uint32_t* c = g.counters;
    LOG_INFO("[MESHLET] frame=%llu path=%s reason=%s packets=%u instances=%u asGroups=%llu "
             "prepass=%u limitFallback=%u psoFallback=%u opaque=%u geomEligible=%u noData=%u "
             "skinned=%u customVS=%u customPS=%u dissolve=%u cull=%u statsValid=%d tested=%u "
             "lodRejected=%u cullFrustum=%u cullCone=%u cullHiZ=%u visible=%u tris=%u lvl0=%u lvl1=%u "
             "lvl2=%u lvl3=%u lvl4=%u lvl5=%u lvl6=%u lvl7=%u lod=%.3f hiz=%d statsFrame=%u cmds=%u groups=%u "
             "groupFallback=%u",
             frame, path, *reason ? reason : "-", s.packets, s.instances,
             static_cast<unsigned long long>(s.asGroups), s.prepassPackets,
             s.limitFallback, s.psoFallback, opaque, geom, noData, skinned, customVS, customPS,
             dissolve, emit.meshletCullMask, g.valid ? 1 : 0, c[kMeshletStatTested],
             c[kMeshletStatLodRejected], c[kMeshletStatCullFrustum], c[kMeshletStatCullCone],
             c[kMeshletStatCullHiZ], c[kMeshletStatVisible], c[kMeshletStatVisibleTris],
             c[kMeshletStatLevel0], c[kMeshletStatLevel0 + 1], c[kMeshletStatLevel0 + 2],
             c[kMeshletStatLevel0 + 3], c[kMeshletStatLevel0 + 4], c[kMeshletStatLevel0 + 5],
             c[kMeshletStatLevel0 + 6], c[kMeshletStatLevel0 + 7], emit.meshletLodTau,
             (emit.meshletCullMask & kMeshletCullHiZ) != 0 && HiZUsableThisFrame() ? 1 : 0, g.frame,
             s.indirectCmds, s.indirectGroups, s.groupFallback);

    // The CSM meshlet path: the shadow amplification shader's counters summed over
    // the three cascades, plus each cascade's triangles. Same cadence as [MESHLET].
    if (m_shadowPass)
    {
        const ShadowPass::MeshletShadowStats ss = m_shadowPass->GetMeshletShadowStats();
        uint32_t sum[kMeshletStatsCount] = {};
        for (uint32_t c = 0; c < ShadowPass::kRetainedCullCascades; ++c)
            for (uint32_t k = 0; k < kMeshletStatsCount; ++k) sum[k] += ss.counters[c][k];
        LOG_INFO("[MESHLET_SHADOW] frame=%llu active=%d statsValid=%d statsFrame=%u twins=%u cpuFallback=%u "
                 "tested=%u lodRejected=%u cullFrustum=%u visible=%u tris=%u c0tris=%u c1tris=%u c2tris=%u "
                 "lvl0=%u lvl1=%u lvl2=%u lvl3=%u lod=%.3f",
                 frame, emit.shadowMeshletFrameActive ? 1 : 0, ss.valid ? 1 : 0, ss.frame, ss.twinGroups,
                 ss.cpuFallback, sum[kMeshletStatTested], sum[kMeshletStatLodRejected],
                 sum[kMeshletStatCullFrustum], sum[kMeshletStatVisible], sum[kMeshletStatVisibleTris],
                 ss.counters[0][kMeshletStatVisibleTris], ss.counters[1][kMeshletStatVisibleTris],
                 ss.counters[2][kMeshletStatVisibleTris], sum[kMeshletStatLevel0], sum[kMeshletStatLevel0 + 1],
                 sum[kMeshletStatLevel0 + 2], sum[kMeshletStatLevel0 + 3], emit.meshletShadowLodTau);
    }
}

// Render.explain(id).meshlet.lod: the level the LOD rule picks for the entity
// right now - the AS's own MeshletLodSelected over the library's per-level
// records (one record per level: every cluster of a discrete-LOD level carries
// the same spheres and errors), with the entity's world transform and this
// frame's view. Read-only; the caller drained the frame.
void Renderer::ExplainMeshletLod(Entity e, MeshletLodReport& out) const
{
    out = MeshletLodReport{};
    if (!m_lastWorld || !m_meshLib) return;
    const MeshLibRef*      ref = m_lastWorld->GetComponent<MeshLibRef>(e);
    const GlobalTransform* gt  = m_lastWorld->GetComponent<GlobalTransform>(e);
    if (!ref || !ref->IsValid() || !gt) return;
    std::vector<Resource::MeshLibrary::MeshletLevelInfo> levels;
    if (!m_meshLib->GetMeshletLevels(ref->libHandle, ref->meshId, levels) || levels.empty()) return;
    using namespace MeshletShared;
    const MeshletLodView lv = GetMeshletLodView();
    const DirectX::XMFLOAT4X4& m = gt->matrix;
    const float3 r0{ m.m[0][0], m.m[0][1], m.m[0][2] }, r1{ m.m[1][0], m.m[1][1], m.m[1][2] },
                 r2{ m.m[2][0], m.m[2][1], m.m[2][2] }, t{ m.m[3][0], m.m[3][1], m.m[3][2] };
    const float  s = MeshletMaxScale(r0, r1, r2);
    const float3 cam{ lv.camera[0], lv.camera[1], lv.camera[2] };
    out.valid = true;
    out.tau   = lv.tau;
    for (uint32_t l = 0; l < levels.size(); ++l)
    {
        const auto& L = levels[l];
        MeshletLodReport::Level r;
        r.clusters    = L.clusters;
        r.triangles   = L.triangles;
        r.selfError   = L.selfError;
        r.parentError = L.parentError;
        const float3 sc = MeshletXformPoint(float3{ L.selfCenter[0], L.selfCenter[1], L.selfCenter[2] }, r0, r1, r2, t);
        const float3 pc = MeshletXformPoint(float3{ L.parentCenter[0], L.parentCenter[1], L.parentCenter[2] }, r0, r1, r2, t);
        r.projected = MeshletProjectedError(sc, L.selfRadius * s, L.selfError * s, cam, lv.lodScale, lv.ortho, lv.zNear);
        if (out.selected < 0 &&
            MeshletLodSelected(l, sc, L.selfRadius * s, L.selfError * s, pc, L.parentRadius * s, L.parentError * s,
                               cam, lv.lodScale, lv.tau, lv.ortho, lv.zNear))
            out.selected = static_cast<int32_t>(l);
        out.levels.push_back(r);
    }
}
