#include "Renderer/Preview/PreviewRenderer.h"

#include "Graphics/DynamicShaderRegistry.h"   // custom shader names in the logs
#include "Graphics/GraphicsDX12.h"
#include "Graphics/DescriptorHeapAllocator.h"   // the 8-slot space3 fallback block
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GPUInstanceData.h"
#include "Graphics/MaterialCBVRing.h"    // MaterialCBVStore::kSlotStride - the b8 slice budget
#include "Graphics/MeshManager.h"
#include "Graphics/PermutationKey.h"
#include "Graphics/ShadingStencil.h"
#include "ECS/MaterialReflectionSync.h"  // IsReservedCBuffer - b8 is not always the material's
#include "RenderGraph/RenderPass/ClusterPass.h"   // kClusterCount - the grid shape Lighting.ps indexes
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/PreviewStudioShadowPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "Renderer/Preview/PreviewStudioRig.h"
#include "Resource/MaterialSerializer.h" // Resource::MaterialContentGeneration
#include "Resource/ProceduralMesh.h"     // PrimitiveMeshType
#include "Resource/SystemHandles.h"      // Resource::MaterialGPUData
#include "Harness/RunLedger.h"           // DeviceLost() - is the device already gone
#include "System/Check.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost - never #ifdef WITH_EDITOR
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>    // FLT_MAX - the sanitised depth key
#include <cmath>
#include <chrono>    // graph compile / destroy timings on the log lines
#include <cstring>   // std::memcpy into the mapped exposure buffer
#include <string>
#include <vector>    // the asset path's per-part depth keys

// PreviewRenderer: the preview layer's secondary views, drawn by second instances of
// the shipping passes. The header states the three graphs, the four rules this file
// enforces and the thread each entry point runs on.

// The HDR slot supply is a device limit: slot 0 is the main viewport's, the
// thumbnail lane owns one (m_thumbTarget) and the slots from kFirstViewHdrSlot on are
// the renderer's secondary views', so live views get kFirstViewHdrSlot - 2.
// This is the only TU that sees both constants. An inequality, so the supply may
// carry headroom; 4 + 1 <= 6 - 1 is a tie, so raising kMaxPreviews requires raising
// kFirstViewHdrSlot (and kMaxHdrTargets with it).
static_assert(Preview::kMaxPreviews + 1 <= GraphicsDX12::kFirstViewHdrSlot - 1,
              "Preview::kMaxPreviews live views plus the M3 thumbnail lane's own "
              "target must fit below GraphicsDX12::kFirstViewHdrSlot (slot 0 is the "
              "main viewport, the slots above it the secondary views'). Raise it first.");
static_assert(Preview::kMaxPreviewsPerFrame <= Preview::kMaxPreviews,
              "The per-frame scheduling cap cannot exceed the storage cap.");

// The private material table: the engine-reserved neutral slot 0, then one record
// per drawn part. Slot 0 is not padding: draws with no material (terrain, water without
// a material, overflow) encode matIdx 0 into GBuffer normal.a and Lighting /
// NPRStabilize / AutoExposure dereference it, so a table read by the same shaders keeps
// the layout. The single-material path writes slot 1 (kPreviewMatSlot), which keeps the
// A/B parity fixtures bit-identical; an asset view writes slots 1..N, one per packet.
static constexpr uint32_t kPreviewMatSlots  = 1 + Preview::kMaxPreviewParts;
static constexpr uint32_t kPreviewMatSlot   = 1;

namespace
{
    using namespace DirectX;

    // The one "is this an asset draw" test. BuildDrawList and the instance /
    // material writes all ask it, so an asset view whose parts have not resolved yet
    // (the library is streaming) cannot take the asset branch in one place and the
    // single-material branch - which reads m_packets[0] and slot 1 - in another. The
    // path is part of the test: a kind with no path is the panel's "nothing selected"
    // state and takes the primitive route.
    bool IsAssetDraw(const Preview::Request& req)
    {
        return req.view.assetKind != Preview::AssetKind::None &&
               !req.view.assetPath.empty();
    }

    // The four ViewDesc shapes, mapped onto the persistent primitive pool.
    // MeshManager::InitPrimitives uploads them with persistent=true, so their
    // mesh-descriptor slots survive every world reload and the preview keeps drawing
    // across a Play -> Stop.
    int PrimitiveIndexFor(Preview::Shape s)
    {
        switch (s)
        {
        case Preview::Shape::Cube:  return static_cast<int>(PrimitiveMeshType::Cube);
        case Preview::Shape::Plane: return static_cast<int>(PrimitiveMeshType::Plane);
        case Preview::Shape::Torus: return static_cast<int>(PrimitiveMeshType::Torus);
        case Preview::Shape::Sphere:
        default:                    return static_cast<int>(PrimitiveMeshType::Sphere);
        }
    }

    // The clustered-light record lives in PreviewTypes.h (with its 64-byte
    // static_assert against cluster_common.hlsli's GPULight) because
    // Renderer::ExtractPreviewSceneLights packs scene lights into it on the game half.
    using PreviewGPULight      = Preview::GPULight;
    using PreviewLightGridEntry = Preview::LightGridEntry;

    // Resolve a rig point light into its GPU record. The light SITS at
    // -normalize(dir) * distance and shines at the origin; see the note on
    // StudioPointLight for why `intensity` is roughly distance^2.
    PreviewGPULight MakeRigPointLight(const Preview::StudioPointLight& L)
    {
        PreviewGPULight g{};
        XMFLOAT3 d;
        XMStoreFloat3(&d, XMVector3Normalize(
            XMVectorSet(L.dir[0], L.dir[1], L.dir[2], 0.f)));
        g.position    = { -d.x * L.distance, -d.y * L.distance, -d.z * L.distance };
        g.radius      = L.radius;
        g.color       = { L.color[0], L.color[1], L.color[2] };
        g.intensity   = L.intensity;
        // Direction is read only by the spot cone test; a point light leaves it
        // at the travel direction so a future spot variant has it already.
        g.direction    = d;
        g.spotAngle    = 0.f;
        g.type           = 1u;            // LightType::Point
        // No shadow slice: AccumClusteredLight would SamplePointShadow against
        // LightCB.pointShadowIdx, which here is LightingPass#2's 12-slice 1x1 fallback - a
        // defined white that costs a SampleCmp per light per pixel. 0xFFFFFFFF skips it.
        // Renderer::ExtractPreviewSceneLights applies the same rule to a secondary view's
        // scene lights, so a shadow-casting lamp appears unshadowed there (stated in
        // WriteWorldLightCB's exception list and the camera panel's tooltip).
        g.shadowSliceIdx = 0xFFFFFFFFu;
        return g;
    }
}

PreviewRenderer::PreviewRenderer(IGraphicsDevice& gfx, Renderer& owner)
    : m_gfx(gfx), m_owner(owner)
{
    // Unique pass names per graph instance: GetGPUTimestampResult is a
    // first-match-wins name scan and PIX markers key off the same string, so
    // "GBufferPassPreview" and "GBufferPassPreviewThumb" must differ or the profiler
    // reports the union of both.
    m_thumb.suffix = "PreviewThumb";
    // The WorldCull set has its own name and its own constant-buffer lane (see
    // GraphSet::perViewCB). m_live and m_thumb share lane 0; ProcessQueue allows at most
    // one of them per frame by testing m_laneRenders[kLaneLive] before taking a
    // thumbnail. m_world is the second picture of the per-frame budget, so it needs its
    // own rings or both dispatches read whichever wrote last.
    m_world.suffix    = "PreviewWorld";
    m_world.source    = Preview::Source::WorldCull;
    m_live.perViewCB  = &m_perViewCB;
    m_live.lightCB    = &m_lightCB;
    m_thumb.perViewCB = &m_perViewCB;
    m_thumb.lightCB   = &m_lightCB;
    m_world.perViewCB = &m_worldPerViewCB;
    m_world.lightCB   = &m_worldLightCB;
    m_live.lane       = kLaneLive;
    m_thumb.lane      = kLaneLive;   // SHARED with m_live - see m_laneRenders
    m_world.lane      = kLaneWorld;
    LOG_INFO("[PREVIEW] PreviewRenderer created (maxViews=%u, perFrame=%u, "
             "thumbnails %u^2)",
             Preview::kMaxPreviews, Preview::kMaxPreviewsPerFrame,
             Preview::kThumbnailSize);
}

// Out of line, and it has to be: Target holds a unique_ptr<ToneMapPass> and the
// header only forward-declares that type.
PreviewRenderer::~PreviewRenderer()
{
    // Idle the device first. The immediate release of m_customTexTable below and the
    // pass Shutdowns DestroyGraphSet reaches need an idle GPU, and nothing on the exit
    // path provides one before ~GraphicsDX12 (App::ShutdownAllSystems joins the render
    // thread but never fences the GPU). The wait also drains every deferred bucket, so
    // the teardowns below start from a clean free list.
    //
    // The wait throws on a removed device (WaitForPreviousFrame -> ThrowIfFailed), and
    // an exception escaping a destructor is std::terminate, so it is caught, as
    // ~GraphicsDX12 does. When the ledger already says the device is lost the wait is
    // skipped: it could only throw. RunLedger is an inert stub outside WITH_HARNESS
    // (DeviceLost() is false), so Retail relies on the catch.
    if (!Harness::RunLedger::Get().DeviceLost())
    {
        try
        {
            m_gfx.WaitIdleAndReleaseDeferred();
        }
        catch (...)
        {
            // Silent and not rethrown: the Logger may already be torn down on some exit
            // paths, and the device-lost diagnosis belongs to whoever lost it.
        }
    }

    // The WorldCull graph's compile count, as one greppable line. A camera view
    // compiles once plus once per committed resize; a size that stopped being quantised
    // (or ServiceCompile disagreeing with PrepareFrame) recreates seven physical
    // textures every frame behind a correct picture, invisible to any other gate.
    // Emitted only when the set was built, so a pack bounding it fails if the feature
    // never ran.
    if (m_worldCompileCount > 0)
        LOG_INFO("[PREVIEW] world graph compiles=%u", m_worldCompileCount);

    // The session ledger, as one greppable line. Unconditional (this class exists
    // only once something asked for a preview), so a pack can always gate on it.
    //   renders        ViewsRenderedTotal: live pictures, thumbnails excluded.
    //   thumbs         pictures the one-shot lane recorded.
    //   worldClears    world boundaries this layer was told about.
    //   postClear      renders since the last of them - the world-churn gate: a panel
    //                  frozen on its pre-reload picture still shows healthy `renders`.
    //                  0 when no world boundary happened.
    //   hdrSlots       the slot mask (0x1 = only the main viewport's, everything came
    //                  back), so a pack can assert the value beside the CHECK below.
    //   rebuilds / rebuildMs  GraphSet builds after a teardown and their total cost.
    //   rebuildsPerHr / rebuildMsPerHr  the same per wall-clock hour of this object's
    //                  life, the form the idle-teardown grace trades against. Read them
    //                  off an ordinary editor session (a pack forces its own cadence);
    //                  a session under 1 s reports 0.
    {
        const uint64_t total = m_viewsRenderedTotal.load(std::memory_order_relaxed);
        const double sessionSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_sessionT0).count();
        const double hours = sessionSec / 3600.0;
        const double perHr = (sessionSec >= 1.0)
            ? (static_cast<double>(m_graphRebuilds) / hours) : 0.0;
        const double msPerHr = (sessionSec >= 1.0) ? (m_graphRebuildMs / hours) : 0.0;
        LOG_INFO("[PREVIEW] session totals: renders=%llu thumbs=%llu "
                 "worldClears=%u postClear=%llu hdrSlots=%u hdrSlotsPeak=%u "
                 "rebuilds=%u "
                 "rebuildMs=%.2f sessionSec=%.1f rebuildsPerHr=%.1f "
                 "rebuildMsPerHr=%.1f",
                 static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(
                     m_thumbsRenderedTotal.load(std::memory_order_relaxed)),
                 m_worldClearCount,
                 static_cast<unsigned long long>(
                     m_worldClearCount ? (total - m_rendersAtLastClear) : 0ull),
                 m_hdrSlotsUsed, m_hdrSlotsPeak,
                 m_graphRebuilds, m_graphRebuildMs, sessionSec,
                 perHr, msPerHr);
    }

    // The split / camera-lane summary, as one greppable line. Every field is a
    // number: logLineFields reads bounds off one line with a name=value scan.
    //   views     high-water mark of concurrently live WorldCull views (acquires, not
    //             pictures: a size-refused cell counts here).
    //   rendered  distinct views that ever recorded a picture; views=4 rendered=1 is
    //             three cells that hold a view and show a refusal or starve.
    //   size      the lane's fixed size; 0x0 = nothing ever latched.
    //   renders   lane-1 records; frames = preview frames with a camera view live. With
    //             N views at tick 1, renders ~ frames and each view gets ~frames / N.
    //   cadence   the largest tickIntervalFrames any view asked for.
    //   spread    max - min of the per-view render counts over views that rendered:
    //             the fairness gate ("within +-1" is not a min/max bound).
    //   mindraws / minsec  minimum over rendered views of their last record's
    //             kept-packet and secondary-lane counts, so one cell showing nothing is
    //             caught; minsec 0 on a rotated camera means two frusta's
    //             intersection plus sky.
    //   v0..v3    per-view render counts (the cadence measurement); d0..d3 their last
    //             kept-packet counts.
    // Every number is per session, so renders == v0 + v1 + v2 + v3.
    static_assert(Preview::kMaxPreviews == 4,
                  "the split line prints v0..v3 by hand; raise it with "
                  "Preview::kMaxPreviews or a view silently stops being "
                  "reported and the cadence gate stops covering it");
    if (m_worldCullPeakViews > 0)
    {
        uint32_t minDraws = 0xFFFFFFFFu, minSec = 0xFFFFFFFFu;
        uint32_t maxDraws = 0;
        uint32_t minR = 0xFFFFFFFFu, maxR = 0;
        uint32_t rendered = 0;
        for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
        {
            if (m_worldViewRenders[i] == 0) continue;
            ++rendered;
            if (m_worldViewLastDraws[i] < minDraws) minDraws = m_worldViewLastDraws[i];
            if (m_worldViewLastDraws[i] > maxDraws) maxDraws = m_worldViewLastDraws[i];
            if (m_worldViewLastSec[i]   < minSec)   minSec   = m_worldViewLastSec[i];
            if (m_worldViewRenders[i] < minR) minR = m_worldViewRenders[i];
            if (m_worldViewRenders[i] > maxR) maxR = m_worldViewRenders[i];
        }
        if (minDraws == 0xFFFFFFFFu) minDraws = 0;
        if (minSec   == 0xFFFFFFFFu) minSec   = 0;
        if (minR     == 0xFFFFFFFFu) minR     = 0;
        // maxdraws and drawspread: "the four cells culled different frusta" is only
        // expressible as a floor on a spread the engine computes. Four cameras at one point
        // on distinct yaws with drawspread 0 means one frustum was reused for all of them.
        LOG_INFO("[PREVIEW] split: views=%u rendered=%u size=%ux%u renders=%u "
                 "frames=%llu cadence=%u spread=%u mindraws=%u maxdraws=%u "
                 "drawspread=%u minsec=%u v0=%u v1=%u v2=%u v3=%u "
                 "d0=%u d1=%u d2=%u d3=%u",
                 m_worldCullPeakViews, rendered, m_loggedWorldW, m_loggedWorldH,
                 m_worldCullRenders,
                 static_cast<unsigned long long>(m_worldCullFrames),
                 m_worldCullMaxTick ? m_worldCullMaxTick : 1u,
                 maxR - minR, minDraws, maxDraws, maxDraws - minDraws, minSec,
                 m_worldViewRenders[0], m_worldViewRenders[1],
                 m_worldViewRenders[2], m_worldViewRenders[3],
                 m_worldViewLastDraws[0], m_worldViewLastDraws[1],
                 m_worldViewLastDraws[2], m_worldViewLastDraws[3]);
    }
    // The secondary-view light summary, a separate line so a reader finds the lights
    // without reading past the split line. Emitted only when a world record reached the
    // writer.
    //   renders   world records that reached WriteWorldClusterLights with the feature
    //             on; stale = how many refused the game half's push on the frame stamp.
    //   minlights / maxlights  range of bound light counts; a max equal to the cap
    //             means Preview::kMaxPreviewLights may be limiting.
    //   gridwrites  records that had to rewrite the 243 KiB uniform grid. Ceiling: live
    //             world views x kFramesInFlight (each view owns a grid ring); a number
    //             climbing past that means the count oscillates (cap flicker).
    //   shadowed  session max of the per-record shadowed tally; must be 0, since no
    //             punctual shadow atlas is bound in a secondary view.
    //   nonfinite total source lights dropped for a NaN / infinite position, radius or
    //             score.
    if (m_worldLightRenders > 0)
    {
        const uint32_t minUsed =
            (m_worldLightMinUsed == 0xFFFFFFFFu) ? 0u : m_worldLightMinUsed;
        LOG_INFO("[PREVIEW] world lights summary: renders=%u stale=%u "
                 "minlights=%u maxlights=%u cap=%u gridwrites=%u shadowed=%u "
                 "nonfinite=%u",
                 m_worldLightRenders, m_worldLightStaleRefusals,
                 minUsed, m_worldLightMaxUsed, kMaxPreviewLights,
                 m_worldLightGridRewrites, m_worldLightShadowedMax,
                 m_worldLightNonFiniteTot);
        // The alarm. An individual stale refusal is legitimate (a camera that went
        // invalid between the halves, a view acquired between them); every record refusing
        // is the m_previewFrame off-by-one - a correct-looking picture with no point lights.
        if (m_worldLightStaleRefusals == m_worldLightRenders)
            LOG_WARNING("[PREVIEW] world lights: EVERY world record this session "
                        "(%u) refused the game half's push on the frame stamp, so "
                        "no secondary view was ever lit by a scene light. That is "
                        "the SetViewSceneLights / WriteWorldClusterLights stamp "
                        "disagreeing, not a scene without lights.",
                        m_worldLightRenders);
    }
    // The idle-teardown summary, as one greppable line. Emitted before the shutdown
    // teardown below, so it reports idle teardowns only.
    //   sets / tonemaps  what the idle sweep gave back.
    //   cycles           sweeps that gave back anything (an open/close cycle is 1).
    //   live/thumb/world  how many times each set was given back.
    //   live*Delta       pool high-water at the last teardown of the live set minus the
    //                    first: the stability gate. Samples come from identical
    //                    build/destroy cycles, so non-zero is a recycling failure.
    //                    `used` is a monotonic cursor FreeStatic never decrements, flat
    //                    exactly when the free list is reused. 0 when the live set was
    //                    destroyed fewer than twice.
    //   worldCbvDelta    the same for the WorldCull set.
    // Silent when the sweep never fired.
    if (m_setsDestroyed || m_toneMapsDestroyed)
    {
        auto delta = [&](int k, int pool) -> int
        {
            if (m_teardownCount[k] < 2) return 0;
            return static_cast<int>(m_teardownLast[k][pool]) -
                   static_cast<int>(m_teardownFirst[k][pool]);
        };
        LOG_INFO("[PREVIEW] teardown summary: cycles=%u sets=%u tonemaps=%u "
                 "live=%u thumb=%u world=%u liveCbvDelta=%d liveRtvDelta=%d "
                 "liveDsvDelta=%d liveSamplerDelta=%d worldCbvDelta=%d",
                 m_teardownCycles, m_setsDestroyed, m_toneMapsDestroyed,
                 m_teardownCount[kTearLive], m_teardownCount[kTearThumb],
                 m_teardownCount[kTearWorld],
                 delta(kTearLive, 0), delta(kTearLive, 1),
                 delta(kTearLive, 2), delta(kTearLive, 3),
                 delta(kTearWorld, 0));
    }

    // Leak tripwires, before anything is released: they check what the callers
    // failed to hand back. ENGINE_CHECK (reported, never fatal), and every preview pack
    // denies "CHECK FAILED", so a panel that stops releasing its view turns the corpus
    // red.
    {
        uint32_t stillLive = 0, stillParked = 0;
        for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
        {
            if (m_targets[i].live)           ++stillLive;
            if (m_targets[i].pendingDestroy) ++stillParked;
        }
        // A live slot is a fault: every holder releases its view on the way out (the
        // editor panels via EditorLayer::ReleasePreviewViews in App::ShutdownAllSystems,
        // the in-game PiP at the top of ~Renderer, each followed by a drain), so a live slot
        // here is a holder that stopped releasing.
        ENGINE_CHECK(stillLive == 0,
            "PreviewRenderer: %u view slot(s) are still LIVE at teardown. Every "
            "holder is supposed to hand its view back on the way out (the four "
            "editor panels via EditorLayer::ReleasePreviewViews, the PiP via "
            "~Renderer); one of them did not.", stillLive);
        // Parked is a different fault: those slots were released and the serial-window
        // drain never collected them.
        ENGINE_CHECK(stillParked == 0,
            "PreviewRenderer: %u view slot(s) are still parked in pendingDestroy "
            "at teardown - ServiceReleases stopped draining the queue before the "
            "process ended.", stillParked);
    }

    for (Target& t : m_targets)
    {
        // Dropped, not released: ~Renderer's body hands every runtime texture back to
        // the TextureSystem once, refcount-independently, before its members are destroyed,
        // so ReleasePreviewMaterialRuntime here would release them twice.
        t.heldMatKey = 0;
        t.heldPartKeys.clear();   // same rule: dropped, never released
        DestroyTarget(t);
        // And the ToneMapPass DestroyTarget keeps: at shutdown there is no ImGui frame
        // left to outlive, so no grace is needed.
        if (t.toneMap) { t.toneMap->Shutdown(m_gfx); t.toneMap.reset(); }
    }
    // The thumbnail lane's own target, on the same terms. Its tile textures are
    // the EDITOR's and are NOT touched here: this class only ever borrows them.
    m_thumbTarget.heldMatKey = 0;
    m_thumbTarget.heldPartKeys.clear();
    DestroyTarget(m_thumbTarget);
    if (m_thumbTarget.toneMap)
    {
        m_thumbTarget.toneMap->Shutdown(m_gfx);
        m_thumbTarget.toneMap.reset();
    }
    m_thumbQueue.clear();
    m_thumbPending.clear();
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_thumbReadbackMapped[i])
        {
            m_gfx.UnmapBuffer(m_thumbReadback[i]);
            m_thumbReadbackMapped[i] = nullptr;
        }
        if (m_thumbReadback[i].IsValid()) m_gfx.DestroyBuffer(m_thumbReadback[i]);
    }

    // The HDR slot ledger, after every DestroyTarget. Bit 0 is the main viewport's and
    // never handed out, so 1 = everything came back. A set bit is a device HDR target
    // that outlived its owner - the one allocation here the deferred bucket cannot clean
    // up by itself, because DestroyHdrTarget is what queues it.
    ENGINE_CHECK(m_hdrSlotsUsed == 1u,
        "PreviewRenderer: HDR slot mask is 0x%X at teardown, expected 0x1 (bit 0 "
        "= the main viewport). Bits above 0 are preview targets whose "
        "DestroyTarget never ran.", m_hdrSlotsUsed);

    // The private instance / material tables and the custom b8 ring: persistently
    // mapped UPLOAD buffers. DestroyBuffer is the deferred path, so this is safe wherever
    // the Renderer dies.
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_instanceMapped[i]) { m_gfx.UnmapBuffer(m_instanceBuf[i]); m_instanceMapped[i] = nullptr; }
        if (m_instanceBuf[i].IsValid()) m_gfx.DestroyBuffer(m_instanceBuf[i]);
        if (m_materialMapped[i]) { m_gfx.UnmapBuffer(m_materialBuf[i]); m_materialMapped[i] = nullptr; }
        if (m_materialBuf[i].IsValid()) m_gfx.DestroyBuffer(m_materialBuf[i]);
        if (m_customCbvMapped[i]) { m_gfx.UnmapBuffer(m_customCbv[i]); m_customCbvMapped[i] = nullptr; }
        if (m_customCbv[i].IsValid()) m_gfx.DestroyBuffer(m_customCbv[i]);
        m_customCbvVA[i] = 0;
    }
    // The three graphs. The idle sweep gives them back while the process runs; this
    // is a session that ends with panels open. Each holds seven physical textures and up
    // to five passes with their own resources (LightingPass fallbacks and PSOs, the
    // SkyboxPass cube VB and SkyCB ring, the studio shadow pass's 512^2 x2 depth array),
    // which would otherwise leak into the D3D12 live-object report.
    DestroyGraphSet(m_live,  "live");
    DestroyGraphSet(m_thumb, "thumb");
    DestroyGraphSet(m_world, "world");

    // The 1536-descriptor space3 block (see the member comment). Released
    // immediately (quiesced), which is safe only here, after the device was idled at the
    // top of this destructor.
    if (m_customTexTable.IsValid())
        m_gfx.ReleaseDescriptorBlock(m_customTexTable, /*quiesced=*/true);
    m_customTexTableGpu = m_customTexTableCpu = 0;
    m_customTexTableInc = 0;

    // The per-lane clustered-light rings and the checker cube (an 8x8x6 texture and
    // its descriptor).
    for (uint32_t ln = 0; ln < kLaneCount; ++ln)
    {
        for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
        {
            if (m_clusterLightMapped[ln][i]) { m_gfx.UnmapBuffer(m_clusterLightBuf[ln][i]); m_clusterLightMapped[ln][i] = nullptr; }
            if (m_clusterLightBuf[ln][i].IsValid()) m_gfx.DestroyBuffer(m_clusterLightBuf[ln][i]);
            if (m_clusterIndexMapped[ln][i]) { m_gfx.UnmapBuffer(m_clusterIndexBuf[ln][i]); m_clusterIndexMapped[ln][i] = nullptr; }
            if (m_clusterIndexBuf[ln][i].IsValid()) m_gfx.DestroyBuffer(m_clusterIndexBuf[ln][i]);
            // The grid ring is per view on the world lane, so the teardown walks
            // kMaxPreviews of them; the studio lane's unused ones are invalid handles, skipped by
            // the same tests.
            for (uint32_t gv = 0; gv < Preview::kMaxPreviews; ++gv)
            {
                if (m_clusterGridMapped[ln][gv][i]) { m_gfx.UnmapBuffer(m_clusterGridBuf[ln][gv][i]); m_clusterGridMapped[ln][gv][i] = nullptr; }
                if (m_clusterGridBuf[ln][gv][i].IsValid()) m_gfx.DestroyBuffer(m_clusterGridBuf[ln][gv][i]);
            }
        }
        m_clusterReady[ln] = false;
    }
    if (m_checkerCube.IsValid()) m_gfx.DestroyTexture(m_checkerCube);
    m_checkerCubeSrv = 0;

    m_perViewCB.Destroy(m_gfx);
    m_lightCB.Destroy(m_gfx);
    m_fogApplyNoneCB.Destroy(m_gfx);
    m_shadowViewCB.Destroy(m_gfx);
    m_worldPerViewCB.Destroy(m_gfx);
    m_worldLightCB.Destroy(m_gfx);

    // Freed although process-lifetime: Renderer can be torn down and rebuilt (world
    // reload, device reset), and a lazily constructed layer must not leak a texture and
    // a mapped UPLOAD buffer per cycle. DestroyTexture / DestroyBuffer are deferred, so
    // this is safe wherever the Renderer dies.
    if (m_neutralBlack.IsValid())    m_gfx.DestroyTexture(m_neutralBlack);
    if (m_neutralExposure.IsValid()) m_gfx.DestroyBuffer(m_neutralExposure);
    m_neutralExposureMapped = nullptr;
    m_neutralBlackSrv = m_neutralExposureSrv = 0;
    m_neutralReady = m_neutralBlackCleared = false;
    // The world-exposure ring, on the same argument.
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_worldExposure[i].IsValid()) m_gfx.DestroyBuffer(m_worldExposure[i]);
        m_worldExposure[i]       = {};
        m_worldExposureMapped[i] = nullptr;
        m_worldExposureSrv[i]    = 0;
    }
    m_worldExposureReady = false;
}

PreviewRenderer::Target* PreviewRenderer::Get(uint32_t viewId)
{
    if (viewId >= Preview::kMaxPreviews) return nullptr;
    return &m_targets[viewId];
}

const PreviewRenderer::Target* PreviewRenderer::Get(uint32_t viewId) const
{
    if (viewId >= Preview::kMaxPreviews) return nullptr;
    return &m_targets[viewId];
}

// ==== Latch writers: editor UI half or game half ====
// PreviewLatchCallerOk: who may write a Target's latch (AcquireView, ReleaseView,
// SetRequest, RequestWorldCullLaneSize). Two legal callers with different
// justifications: Editor.exe's UI (MaterialPreviewWidget::Draw and the other panels
// inside OnUIRender, under a Render ThreadCtx scope, safe because the render half is
// parked), and the game half of Game.exe (Renderer::ApplyPendingGamePiP, which applies what
// SetGamePiP queued, with the render half parked by FramePipeline::DrainFrameWork). The render half
// of a threaded Game.exe must never call them, which is what ThreadCtx reports; both
// halves of the disjunction are checked.
static bool PreviewLatchCallerOk()
{
    // Editor UI half: OnUIRender is ThreadCtx::Render, legal only because the editor
    // builds its UI while the render half is parked (on the serial path, or under
    // -renderowner after the drain and before the post).
    if (Sys::IsEditorHost()) return true;
    // Game half of a shipped game: any context that is not the render one.
    // Current(), not Level() - Level is the tripwire's verbosity knob, Current
    // is the context this thread is actually in.
    return Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render;
}

uint32_t PreviewRenderer::AcquireView(bool worldCull)
{
    // See PreviewLatchCallerOk. Sys::IsEditorHost(), never #ifdef WITH_EDITOR:
    // EngineCore compiles once for both exes, so the macro would take the shipped-game
    // branch in both.
    ENGINE_CHECK_ONCE(PreviewLatchCallerOk(),
        "PreviewRenderer::AcquireView called from the RENDER half of a non-editor "
        "host. The latch belongs to the game half there (Renderer::SetGamePiP); "
        "the editor's own path is legal only because Editor.exe is pinned serial.");

    // Several WorldCull views may be live at once; the two hazards of sharing the one
    // m_world GraphSet are handled elsewhere:
    //   * size - a RenderGraph has one size, so SetRequest (WorldCullSizeAllowed) fixes
    //     it with the first accepted request and refuses other sizes with one line;
    //   * lane - ProcessQueue records at most one lane-1 candidate per frame and
    //     round-robins the rest, so the record-side ENGINE_CHECK never fires in Release.
    // The bound on N is Preview::kMaxPreviews (4 live HDR slots).

    // Any free slot will do: each view owns its latch in Target, and the cost of
    // several live views is bounded by ProcessQueue's scheduler and its per-lane guard.
    for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
    {
        Target& t = m_targets[i];
        if (t.live || t.pendingDestroy) continue;
        t.live          = true;
        t.lease.fetch_add(1, std::memory_order_relaxed);   // a new tenancy (ViewLease)
        t.pendingCreate = true;   // PrepareFrame owns the device work (rule 3)
        t.reqW = t.reqH = 0;
        t.hasOutput.store(false, std::memory_order_relaxed);
        // Arm the flag: a re-acquired view whose new ViewDesc equals the stale one in the
        // latch would otherwise never mint a generation, and the panel would show
        // "rendering..." forever.
        t.needsRender   = true;
        // generation is not reset: it is monotonic for the life of the pool slot, so a
        // result still in flight for the previous tenant can never match the new one's.
        // Resetting it is the ABA hole this counter closes.
        t.lastRenderedGeneration = t.generation;
        // Drop any stale latch so the first SetRequest is a fresh picture. ReleaseView
        // already did this; it matters only for a slot never released cleanly. Not
        // t.generation - see above.
        t.req    = Preview::Request{};
        t.result = Preview::Result{};
        t.lastTickFrame = 0;
        m_viewSizeRefused[i] = false;
        // Synthetic is assigned, not defaulted: a slot reused after a WorldCull tenant
        // must not inherit its lane (the release path clears it too, but a slot never
        // released cleanly would stay counted in the wrong lane).
        m_slotSource[i] = Preview::Source::Synthetic;
        m_sourceRefusedLogged[i] = false;
        if (worldCull)
        {
            // The slot's lane, latched once and read by everything.
            m_slotSource[i] = Preview::Source::WorldCull;
            ++m_worldCullLive;
            if (m_worldCullLive > m_worldCullPeakViews)
                m_worldCullPeakViews = m_worldCullLive;
            // The one-shot diagnostics belong to a tenancy, per view: a re-acquired view
            // (panel reopened, Lua PiP toggled) must log its "first attempt", empty-cull and
            // "world cull:" lines again, and acquiring a second cell must not silence the first.
            m_worldCullAttempted[i]   = false;
            m_worldCullEmptyLogged[i] = false;
            m_worldFirstLogged[i]     = false;
            m_worldCullLoggedDraws[i] = 0xFFFFFFFFu;
            // The leafless-kept re-emit latches belong to the tenancy for the same reason.
            m_worldLoggedLeaflessPrimKept[i]  = 0xFFFFFFFFu;
            m_worldLoggedLeaflessOtherKept[i] = 0xFFFFFFFFu;
            // m_worldViewRenders / LastDraws / LastSec are not reset: they feed the shutdown
            // "split:" line, whose renders / frames count the whole session, so a reset would
            // print a per-tenancy numerator over a per-session denominator. The one-shot log
            // latches above stay per tenancy.
            // This view's own cull counters are reset, so a fresh tenancy does not inherit the
            // previous one's counts; they are per view, so a live sibling cell keeps its own.
            m_worldCullDraws[i].store(0, std::memory_order_relaxed);
            m_worldCullCandidates[i].store(0, std::memory_order_relaxed);
            m_worldCullSecondary[i].store(0, std::memory_order_relaxed);
        }
        m_allParkedStreak = 0;
        LOG_INFO("[PREVIEW] view %u acquired%s", i,
                 worldCull ? " (WorldCull)" : "");
        return i;
    }
    // Exhaustion is a refusal, not a fault: the split panel takes all four slots at
    // 2x2 and a fifth panel then asks, and callers already show "no free view". It is a
    // warning, once per fill, because preview packs assert engineErrors 0.
    //
    // Still a fault: every slot parked awaiting its deferred teardown with none live,
    // for kParkedStreakFault consecutive attempts. That is ServiceReleases no longer
    // draining - it never clears by itself and no panel can report it. App drains the
    // queue between ProcessPendingActions and OnUIRender, so one such attempt is only an
    // ordering accident; a streak is a stopped drain.
    constexpr uint32_t kParkedStreakFault = 120;   // ~2 s at 60 Hz
    uint32_t parked = 0;
    for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
        if (!m_targets[i].live && m_targets[i].pendingDestroy) ++parked;
    m_allParkedStreak = (parked == Preview::kMaxPreviews)
                      ? (m_allParkedStreak + 1) : 0u;
    ENGINE_CHECK_ONCE(m_allParkedStreak < kParkedStreakFault,
        "PreviewRenderer::AcquireView: all %u view slots have been parked in "
        "pendingDestroy with none live for %u consecutive acquire attempts - "
        "ServiceReleases has stopped draining the teardown queue, so no view "
        "will ever be handed out again. This is the leak signature, not the busy "
        "one.",
        Preview::kMaxPreviews, m_allParkedStreak);
    if (!m_slotsExhaustedLogged)
    {
        m_slotsExhaustedLogged = true;
        LOG_WARNING("[PREVIEW] all %u view slots are in use; this acquire is "
                    "refused and the panel will show 'no free view' until one is "
                    "released. Four live cells in the split panel is the whole "
                    "pool (Preview::kMaxPreviews), so this is reachable by "
                    "configuration - raise kMaxPreviews AND kMaxHdrTargets "
                    "together (the static_assert at the top of this file) if it "
                    "needs to be more.",
                    Preview::kMaxPreviews);
    }
    return Preview::kInvalidView;
}

void PreviewRenderer::ReleaseView(uint32_t viewId)
{
    // The same latch rule as AcquireView / SetRequest; this one voids everything in
    // flight (bumps the generation, clears live / hasOutput / req.valid / req.inFlight,
    // hands the WorldCull lane back). Both legal callers pass the check:
    //   * Renderer::OnWorldClear and ApplyPendingGamePiP - game half, inside
    //     BuildRenderScene's extract window with the render half parked;
    //   * CameraPreviewWindow / MaterialPreviewWidget teardown - OnUIRender, which is
    //     ThreadCtx::Render but only in the serial-pinned Editor.exe.
    ENGINE_CHECK_ONCE(PreviewLatchCallerOk(),
        "PreviewRenderer::ReleaseView called from the RENDER half of a non-editor "
        "host. See the note on AcquireView: the release voids everything in "
        "flight for this view, so it belongs to the game half there.");

    Target* t = Get(viewId);
    if (!t || !t->live) return;
    t->live           = false;
    t->pendingCreate  = false;
    t->pendingDestroy = true;
    // Void everything in flight now rather than at the deferred teardown: the
    // returning result was computed for a view that no longer exists, and applying it
    // would stamp "done" onto the next tenant of this id.
    ++t->generation;
    t->hasOutput.store(false, std::memory_order_relaxed);
    t->needsRender = true;   // a future tenant of this id starts dirty
    t->req.valid    = false;
    t->req.inFlight = false;
    m_viewSizeRefused[viewId] = false;
    // The scheduler's preference dies with the view it names, or the next tenant of
    // the slot inherits a double share of the lane. ProcessQueue's expiry would only
    // catch it a frame later.
    if (m_priorityView.load(std::memory_order_relaxed) == viewId)
        m_priorityView.store(Preview::kInvalidView, std::memory_order_relaxed);
    // A slot came back, so the "all slots are in use" line may print again the next
    // time the pool fills: one line per occurrence, not per session or per frame.
    m_slotsExhaustedLogged = false;
    if (IsWorldCullSlot(viewId))
    {
        // Hand the lane back and re-arm the one-shot diagnostics here as well as at
        // acquire: a release never followed by an acquire must not leave stale counts
        // readable through LastWorldCullDraws().
        m_slotSource[viewId] = Preview::Source::Synthetic;
        m_sourceRefusedLogged[viewId] = false;
        if (m_worldCullLive != 0) --m_worldCullLive;
        m_worldCullAttempted[viewId]   = false;
        m_worldCullEmptyLogged[viewId] = false;
        m_worldFirstLogged[viewId]     = false;
        m_zeroSizeLogged[viewId]       = false;
        m_worldCullLoggedDraws[viewId] = 0xFFFFFFFFu;
        m_worldCullDraws[viewId].store(0, std::memory_order_relaxed);
        m_worldCullCandidates[viewId].store(0, std::memory_order_relaxed);
        m_worldCullSecondary[viewId].store(0, std::memory_order_relaxed);
        // Same rule for the four leafless counters and the two re-emit latches.
        m_worldCullLeaflessPrim[viewId].store(0, std::memory_order_relaxed);
        m_worldCullLeaflessOther[viewId].store(0, std::memory_order_relaxed);
        m_worldCullLeaflessPrimKept[viewId].store(0, std::memory_order_relaxed);
        m_worldCullLeaflessOtherKept[viewId].store(0, std::memory_order_relaxed);
        m_worldLoggedLeaflessPrimKept[viewId]  = 0xFFFFFFFFu;
        m_worldLoggedLeaflessOtherKept[viewId] = 0xFFFFFFFFu;
        // The lane size belongs to the tenancy and is released only when the last
        // WorldCull view goes. Releasing it while another cell holds it would let the next
        // acquire fix a size the live cells' targets are not, and their records would bail
        // silently on `set.w != t->w`. A new tenancy at another size costs one recompile.
        if (m_worldCullLive == 0)
        {
            m_worldSizeW = m_worldSizeH = 0;
            m_worldSizeOwner = Preview::kInvalidView;
            // The refusal latches go back with the size they described, so the next
            // tenancy's refusals are logged too.
            m_worldSizeRefusedLogged = false;
            m_moveRefusedCount       = 0;
        }
    }
    LOG_INFO("[PREVIEW] view %u released (teardown deferred to the next serial window)", viewId);
}

void PreviewRenderer::SetRequest(uint32_t viewId, const Preview::ViewDesc& v,
                                 const MaterialComponent& mat)
{
    ENGINE_CHECK_ONCE(PreviewLatchCallerOk(),
        "PreviewRenderer::SetRequest called from the RENDER half of a non-editor "
        "host. See the note on AcquireView: a Game-side PiP routes requests "
        "through the game half.");

    Target* t = Get(viewId);
    if (!t || !t->live) return;

    // Refuse a request whose source disagrees with the slot's acquire-time lane.
    // Past this line t->req.view.source == SlotSource(viewId) holds by construction, so
    // LaneIdle, the live count, RequestWorldCullLaneSize's ownership predicate, the
    // scheduler's pickBest and the record route all read one field.
    // A refusal touches nothing, not even the material copy: half-latching a request
    // from the wrong lane leaves a description the target can never render, and letting
    // it through lets a GraphSet be judged idle while a view draws through it. Logged per
    // view: one wrong cell must not silence the others.
    if (v.source != SlotSource(viewId))
    {
        if (!m_sourceRefusedLogged[viewId])
        {
            m_sourceRefusedLogged[viewId] = true;
            LOG_ERROR("[PREVIEW] view %u was acquired as a %s slot and sent a %s "
                      "request - REFUSED. The lane's live count, its size "
                      "ownership, its idle teardown and its record route all key "
                      "on the ACQUIRE flag, so this view would be judged against "
                      "a lane it is not part of. Release and re-acquire the view "
                      "with the source the panel means.",
                      viewId,
                      SlotSource(viewId) == Preview::Source::WorldCull
                          ? "WorldCull" : "Synthetic",
                      v.source == Preview::Source::WorldCull
                          ? "WorldCull" : "Synthetic");
        }
        return;
    }

    // The dirty-edge regulator has exactly two edges: the description changed
    // (colour picker, resize), or the view is explicitly dirty (needsRender: just
    // acquired or visible again). There is deliberately no "lane is idle, so refresh"
    // term: idle is the settled steady state, so it would re-render every other frame
    // forever and break the zero-idle-cost rule. A record early-out needs no term either:
    // bail() clears inFlight and leaves the generation ahead, so the next
    // ExtractRequests re-sends by itself.
    // The material copy is refreshed unconditionally, before any early-out: the game
    // half decides whether the material changed (HashMaterialParams needs the resolved
    // runtime, unreachable from this render-side context), so this half only copies and
    // ExtractRequests mints the generation.
    t->req.mat = mat;

    // A WorldCull view is dirty every frame by nature (the world moves, its ViewDesc
    // does not); the tick regulator (tickIntervalFrames, applied in ExtractRequests)
    // bounds it. The dirty-edge test is for Synthetic views, whose picture is a pure
    // function of the ViewDesc and the material. SlotSource is the file's one spelling
    // of "which lane is this view" (equal to v.source past the refusal above).
    const bool worldCull = (SlotSource(viewId) == Preview::Source::WorldCull);

    // The WorldCull size is the lane's: every live WorldCull view draws through the
    // one m_world GraphSet. The first accepted request fixes it and a request at another
    // size is refused here, the one place that sees every caller. Moving a fixed size is
    // RequestWorldCullLaneSize's job, called before the new request. A refusal leaves the
    // previous, correctly sized latch in place (a no-op, not a corruption), and the flag
    // lets the panel say why.
    if (worldCull)
    {
        // A zero extent is refused by WorldCullSizeAllowed before its own log line, so
        // warn here (a cell measured before its rectangle existed, a pin that parsed to
        // nothing). Once per view: a panel asks every frame, and one broken cell must not
        // silence the next.
        if ((v.w == 0 || v.h == 0) && !m_zeroSizeLogged[viewId])
        {
            m_zeroSizeLogged[viewId] = true;
            LOG_WARNING("[PREVIEW] view %u asked for a %ux%u WorldCull picture - "
                        "a zero extent cannot size a RenderGraph, so the request "
                        "is REFUSED and the view is suspended until it asks for "
                        "a real size. Nothing else about the view is wrong.",
                        viewId, v.w, v.h);
        }

        // The only decision point for a WorldCull request. Panels must not pre-check
        // before acquiring: that turns a sole panel's resize into a refusal.
        if (!WorldCullSizeAllowed(v.w, v.h))
        {
            // Refused means suspended: an accepted view keeps req.valid, its camera and its
            // target, and would otherwise keep minting generations, winning lane-1 turns and
            // rendering at the old size unseen. Clearing inFlight cancels the hand-over on its
            // way, and ExtractOneRequest will not re-arm a refused view. A request at the lane's
            // size resumes it (the accept branch clears the flag).
            m_viewSizeRefused[viewId] = true;
            t->req.inFlight = false;
            return;
        }
        m_viewSizeRefused[viewId] = false;
        if (m_worldSizeW != v.w || m_worldSizeH != v.h)
        {
            // The first fix of this tenancy: WorldCullSizeAllowed accepts a mismatch only
            // when nothing is fixed yet, and a lane move was already applied by
            // RequestWorldCullLaneSize (m_worldSizeW already equals v.w there).
            m_worldSizeW = m_loggedWorldW = v.w;
            m_worldSizeH = m_loggedWorldH = v.h;
            m_worldSizeOwner = viewId;
            // The refusal latches describe the old size; re-arm them for the new one.
            m_worldSizeRefusedLogged = false;
            m_moveRefusedCount       = 0;
            LOG_INFO("[PREVIEW] split: WorldCull lane size fixed at %ux%u by "
                     "view %u - every live camera view shares one GraphSet, so "
                     "a later request at another size is refused.",
                     v.w, v.h, viewId);
        }
        // The tick the shutdown cadence line reports. Max rather than last,
        // because the claim it supports is "no view asked to run faster".
        const uint32_t tick = (v.tickIntervalFrames == 0) ? 1u : v.tickIntervalFrames;
        if (tick > m_worldCullMaxTick) m_worldCullMaxTick = tick;
    }

    const bool descChanged = !t->req.valid || (t->req.view != v);
    if (!descChanged && !t->needsRender && !worldCull) return;

    // Keep inFlight if it is set: a request landing between ExtractRequests and
    // ProcessQueue must not clear it, or the next extract hands the view over again
    // while the first hand-over is on its way back, and the record publishes the new
    // generation for a picture never drawn (a frozen panel). Only the generation moves;
    // the extract re-arms after the apply.
    t->needsRender   = false;
    t->req.valid      = true;
    t->req.viewId     = viewId;
    t->req.view       = v;
    // A WorldCull view mints its generation in ExtractRequests, on the tick
    // boundary, so a 60 Hz UI thread cannot outrun a tickInterval of 3.
    if (!worldCull || descChanged)
        t->req.generation = ++t->generation;
}

void PreviewRenderer::SetViewCamera(uint32_t viewId, const RenderCamera& cam,
                                    bool valid)
{
    Target* t = Get(viewId);
    if (!t || !t->live) return;
    t->req.cam      = cam;
    t->req.camValid = valid;
}

// WorldCullSizeAllowed: does a WorldCull request match the lane's one size?
// SetRequest is the only caller. Logs the refusal once per tenancy (this and
// RequestWorldCullLaneSize are the two places that see every caller - panel cell,
// PiP and harness). Moving a fixed size is not decided here: RequestWorldCullLaneSize
// is the one mechanism.
bool PreviewRenderer::WorldCullSizeAllowed(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    if (m_worldSizeW == 0) return true;                 // nothing fixed yet
    if (w == m_worldSizeW && h == m_worldSizeH) return true;
    if (!m_worldSizeRefusedLogged)
    {
        m_worldSizeRefusedLogged = true;
        LOG_WARNING("[PREVIEW] split: WorldCull size %ux%u refused - the lane is "
                    "already fixed at %ux%u by view %u and %u camera view(s) are "
                    "live. Every live camera view "
                    "shares ONE GraphSet and a RenderGraph has ONE size, so a "
                    "second size would recompile seven physical textures every "
                    "frame and every mismatched record would bail in silence. "
                    "Close the other camera view, or match its size.",
                    w, h, m_worldSizeW, m_worldSizeH, m_worldSizeOwner,
                    m_worldCullLive);
    }
    return false;
}

// RequestWorldCullLaneSize: the one way to move an already fixed lane size.
// The predicate is "the caller owns the whole lane" (it holds every live WorldCull
// view and hands them all over in this call), not "the caller is the sole view": the
// one-size rule exists so no target is left at the old size while the graph moves
// (its records would bail silently on set.w != t->w), and only this form lets a split
// panel with two to four live cells resize.
// The move is atomic, which keeps it to one recompile: every live view's latch
// (req.view.w/h) and extract snapshot (reqW/reqH) move here, before any can be
// extracted again - ServiceCompile sizes the graph from the first live view's
// snapshot. inFlight is cleared because a hand-over on its way was measured for the
// old size.
bool PreviewRenderer::RequestWorldCullLaneSize(uint32_t w, uint32_t h,
                                               const uint32_t* views,
                                               uint32_t count)
{
    // The latch-writer tripwire, as on AcquireView / ReleaseView / SetRequest: this
    // writes every live view's req.view, reqW/reqH, inFlight and needsRender. See
    // PreviewLatchCallerOk.
    ENGINE_CHECK_ONCE(PreviewLatchCallerOk(),
        "PreviewRenderer::RequestWorldCullLaneSize called from the RENDER half "
        "of a non-editor host. See the note on AcquireView: this moves EVERY "
        "live WorldCull view's latch at once, so it belongs to the game half "
        "there (Renderer::SetGamePiP).");

    if (w == 0 || h == 0) return false;
    // Nothing fixed yet: the first SetRequest fixes it, and that is where the
    // "lane size fixed at" line belongs. Not an error, and not a move.
    if (m_worldSizeW == 0) return true;
    if (w == m_worldSizeW && h == m_worldSizeH) return true;

    // How many of the live WorldCull views the caller named, de-duplicated so a caller
    // passing one id twice cannot "own" a lane it holds one view of.
    bool seen[Preview::kMaxPreviews] = {};
    uint32_t mine = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t v = (views != nullptr) ? views[i] : Preview::kInvalidView;
        if (v >= Preview::kMaxPreviews) continue;
        if (!IsWorldCullSlot(v) || seen[v]) continue;
        seen[v] = true;
        ++mine;
    }
    if (mine == 0 || mine != m_worldCullLive)
    {
        // Its own latch, not m_worldSizeRefusedLogged: a panel refused the move then has
        // each per-cell request refused too, and a shared latch would swallow whichever line
        // came second (the line every _size pack asserts). Keyed on the requested size, so a
        // second panel refused at another size is logged too; see m_moveRefusedSizes for why
        // it is a set.
        if (NoteMoveRefusal(w, h))
        {
            LOG_WARNING("[PREVIEW] split: WorldCull lane move to %ux%u refused - "
                        "the lane is fixed at %ux%u by view %u, %u camera "
                        "view(s) are live and this caller holds only %u of them. "
                        "Moving the size while somebody else's target stays at "
                        "the old one leaves that target bailing in silence on "
                        "every record, so the move is only legal for a caller "
                        "that owns the whole lane. Close the other camera view, "
                        "or match its size.",
                        w, h, m_worldSizeW, m_worldSizeH, m_worldSizeOwner,
                        m_worldCullLive, mine);
        }
        return false;
    }

    uint32_t owner = Preview::kInvalidView;
    for (uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
    {
        if (!seen[v]) continue;
        if (owner == Preview::kInvalidView) owner = v;
        Target& t = m_targets[v];
        // The latch and the snapshot together, so ServiceCompile and PrepareFrame cannot
        // disagree for a frame (which would read as a second recompile).
        t.req.view.w    = w;
        t.req.view.h    = h;
        t.reqW          = w;
        t.reqH          = h;
        t.req.inFlight  = false;
        t.needsRender   = true;
        m_viewSizeRefused[v] = false;
    }
    m_worldSizeW = m_loggedWorldW = w;
    m_worldSizeH = m_loggedWorldH = h;
    m_worldSizeOwner = owner;
    // Both refusal latches described the old size.
    m_worldSizeRefusedLogged = false;
    m_moveRefusedCount       = 0;
    LOG_INFO("[PREVIEW] split: WorldCull lane size re-fixed at %ux%u by view %u "
             "- the caller holds all %u live camera view(s) and moved every one "
             "of them in the same call, so no target is left at the old size and "
             "the graph recompiles exactly once.", w, h, owner, m_worldCullLive);
    return true;
}

// NoteMoveRefusal: true the first time this size is refused (see
// m_moveRefusedSizes). Bounded by kMaxPreviews: a full set goes silent rather than
// wrapping like a ring, so two panels drifting through sizes cannot log per frame.
bool PreviewRenderer::NoteMoveRefusal(uint32_t w, uint32_t h)
{
    const uint32_t key = (w << 16) | (h & 0xFFFFu);
    for (uint32_t i = 0; i < m_moveRefusedCount; ++i)
        if (m_moveRefusedSizes[i] == key) return false;
    if (m_moveRefusedCount >= Preview::kMaxPreviews) return false;
    m_moveRefusedSizes[m_moveRefusedCount++] = key;
    return true;
}

bool PreviewRenderer::IsViewSizeRefused(uint32_t viewId) const
{
    return viewId < Preview::kMaxPreviews && m_viewSizeRefused[viewId];
}

bool PreviewRenderer::IsWorldCullView(uint32_t viewId) const
{
    const Target* t = Get(viewId);
    // Suspended is not live for this question: an accepted-then-refused view keeps
    // req.valid and its camera, and would otherwise keep contributing a secondary
    // frustum (duplicate classify candidates) and an armed auto-exposure readback for a
    // view that records nothing. Reads the slot's source (equal to the latch's, since
    // SetRequest refuses a mismatch), which also answers for a live view whose first
    // request has not landed. `t` is non-null only for an in-range viewId.
    return t && t->live && t->req.valid &&
           SlotSource(viewId) == Preview::Source::WorldCull &&
           !m_viewSizeRefused[viewId];
}

bool PreviewRenderer::GetViewRequestSize(uint32_t viewId, uint32_t& outW,
                                         uint32_t& outH) const
{
    const Target* t = Get(viewId);
    if (!t || !t->live || !t->req.valid) return false;
    if (t->req.view.w == 0 || t->req.view.h == 0) return false;
    outW = t->req.view.w;
    outH = t->req.view.h;
    return true;
}

void PreviewRenderer::ApplyFeedback()
{
    // The one-shot lane first and unconditionally: its result is independent of
    // the live one and must be applied even on a frame the live lane is idle.
    ApplyThumbFeedback();

    // Every live view: each carries its own result.
    for (Target& tgt : m_targets)
    {
        if (tgt.result.viewId == Preview::kInvalidView) continue;

        const Preview::Result r = tgt.result;
        tgt.result = {};

        // The round trip is over whatever the verdict: a result that is dropped
        // for being stale must still clear the flag, or the lane latches in
        // flight forever and the panel freezes on its last good frame.
        tgt.req.inFlight = false;

        // Dropped on purpose: released and re-acquired, or superseded mid-flight.
        // hasOutput is not touched: the picture was drawn and is on the SRV, merely
        // superseded. lastRenderedGeneration staying behind makes the next ExtractRequests
        // hand the newer request over.
        if (r.generation != tgt.generation) continue;
        if (!r.ok) continue;

        // hasOutput was already published by the render half at record time (see
        // GetSrv) - this is the game half's book-keeping only.
        tgt.lastRenderedGeneration = r.generation;
    }
}

void PreviewRenderer::ExtractRequests()
{
    // The one-shot lane, first and unconditionally: the asset browser produces
    // tiles whether or not the material inspector has a panel open, and the
    // live lane's early-outs below must not gate it.
    ExtractThumbnails();

    // Every live view, each through its own latch.
    for (uint32_t vi = 0; vi < Preview::kMaxPreviews; ++vi)
        ExtractOneRequest(vi, m_targets[vi]);
}

// Takes the view id as well as the target so the route asks SlotSource(), the one
// lane query, instead of re-deriving the lane from the latch.
void PreviewRenderer::ExtractOneRequest(uint32_t viewId, Target& target)
{
    Target* t = &target;
    if (!t->req.valid) return;
    if (!t->live) { t->req.valid = false; return; }

    // WorldCull views take their own route: no material runtime, custom-shader ids,
    // mesh override or parameter hash - they draw the scene's packets, already resolved
    // by the main path. They need a camera (filled in by Renderer::ExtractPreviewCameras
    // just before this call) and a tick.
    if (SlotSource(viewId) == Preview::Source::WorldCull)
    {
        // No camera, no request: a default RenderCamera sits at the world origin looking
        // down -Z and would draw a plausible picture of the wrong thing.
        if (!t->req.camValid)
        {
            t->req.inFlight = false;
            return;
        }
        // A size-refused view is suspended: it keeps req.valid, its camera and its last
        // picture (the panel may show it greyed), but mints no generation and hands nothing
        // over, so it takes no lane turn. SetRequest clears the flag when a request at the
        // lane's size arrives.
        if (m_viewSizeRefused[t->req.viewId])
        {
            t->req.inFlight = false;
            return;
        }
        if (t->req.inFlight) return;

        // The tick regulator: tickIntervalFrames 1 = every frame (a focused window, the
        // in-game PiP); higher lets an unfocused panel get out of the way. It counts preview
        // frames (advanced by ProcessQueue), so an interval means the same whatever the game
        // is doing.
        const uint32_t interval = (t->req.view.tickIntervalFrames == 0)
                                ? 1u : t->req.view.tickIntervalFrames;
        // m_previewFrame + 1: this extract runs on the game half of frame F and
        // ProcessQueue increments the counter on the render half of the same frame, so
        // m_previewFrame still names F-1 here. The stale value makes an interval of 1 mean
        // every other frame.
        const uint64_t thisTick = m_previewFrame + 1;
        if (thisTick < t->lastTickFrame + interval &&
            t->req.generation == t->lastRenderedGeneration)
            return;

        t->req.generation = ++t->generation;
        t->reqW = t->req.view.w;
        t->reqH = t->req.view.h;
        t->req.inFlight = true;
        return;
    }

    // Game half only, because:
    //   - the runtime-material maps may only change inside this window (the render half
    //     holds raw pointers into them);
    //   - ShaderLibrary::RegisterDynamic reallocates a vector a compiling worker may
    //     hold an entry pointer into;
    //   - HashMaterialParams needs the resolved runtime, which the record half must not
    //     resolve itself.

    // The OnWorldClear self-heal. OnWorldClear wipes m_runtimeMaterialResources and
    // defers every texture release refcount-blind - on every world Load / New /
    // SceneManager::ActivateScene and every editor Play -> Stop - so the reference this
    // view holds can stop existing (symptom: the preview turns white).
    // Two signals. A missing key is one. The epoch is the other, and the only one that
    // sees this case: ExtractRequests runs after BuildScene_SyncMaterials re-created
    // entries for the new world, so if it uses the same material the key is present
    // again but names an entry whose real holders are scene entities. Releasing it later
    // would hand their TextureSystem handles back (a permanently white object, one
    // unbalanced release). The heal for an epoch change is to forget the key without
    // releasing it.
    const uint32_t matEpoch = m_owner.MaterialRuntimeEpoch();
    if (t->heldMatKey != 0 && t->heldMatEpoch != matEpoch)
    {
        // No ReleasePreviewMaterialRuntime: the reference is already gone, and releasing
        // it would decrement somebody else's.
        t->heldMatKey        = 0;
        t->lastTexturesReady = false;
        t->needsRender       = true;
        if (!m_epochHealLogged)
        {
            m_epochHealLogged = true;
            LOG_INFO("[PREVIEW] material runtime epoch changed (world clear) - "
                     "forgetting the held key WITHOUT releasing it and "
                     "re-acquiring (logged once per session)");
        }
    }
    if (t->heldMatKey != 0 && !m_owner.HasPreviewMaterialRuntime(t->heldMatKey))
    {
        t->heldMatKey       = 0;
        t->lastTexturesReady = false;
        t->needsRender      = true;
        if (!m_selfHealLogged)
        {
            m_selfHealLogged = true;
            LOG_INFO("[PREVIEW] material runtime was dropped by a world clear - "
                     "re-acquiring (logged once per session)");
        }
    }

    // An asset view takes the multi-part route. It shares the two heals above (both
    // ownership schemes live in the runtime-material table) and the dirty-edge tail, and
    // nothing else: an asset has no single material, mesh override or custom-shader id.
    if (t->req.view.assetKind != Preview::AssetKind::None &&
        !t->req.view.assetPath.empty())
    {
        ExtractAssetParts(*t, matEpoch);

        if (t->needsRender)
        {
            t->needsRender    = false;
            t->req.generation = ++t->generation;
        }
        if (t->req.inFlight) return;
        if (t->req.generation == t->lastRenderedGeneration) return;
        t->reqW = t->req.view.w;
        t->reqH = t->req.view.h;
        t->req.inFlight = true;
        return;
    }
    // A view that was an asset view and no longer is hands its references back here,
    // not at the switch: the switch is a ViewDesc change the extract sees exactly once.
    // Through DropAssetScheme, so every scheme transition applies the same epoch rule.
    if (!t->heldPartKeys.empty() || !t->heldAssetPath.empty() ||
        !t->req.parts.empty())
    {
        DropAssetScheme(*t, matEpoch);
        t->needsRender = true;
    }
    CheckSchemeExclusive(*t, "ExtractOneRequest (material route)");

    // Custom shader ids, before anything keys the material. Registration reaches all
    // nine libraries through the one fan-out, from the one thread allowed to do it -
    // never through GetMaterialShaderLibrary()->RegisterDynamic, which breaks parity.
    // Before the runtime key: ResolvePreviewCustomPSID/VSID run
    // MaterialReflectionSync::Sync, which adds a customTextures entry per space3 binding
    // the reflection declares and the material lacks, and BuildPreviewMaterialRuntimeKey
    // folds every customTextures name and path. A key taken before the fill names a
    // different material from the next extract's, so the panel would acquire a second
    // runtime and re-stream its textures from zero. Sync fills every declared slot in
    // one call, so this order makes the material reach a fixed point before anything
    // hashes it. The material hash below still sees the same material as the record.
    t->req.customPSID = m_owner.ResolvePreviewCustomPSID(t->req.mat);
    t->req.customVSID = m_owner.ResolvePreviewCustomVSID(t->req.mat);

    // ---- Runtime-material ownership ----
    const uint64_t wantKey = m_owner.BuildPreviewMaterialRuntimeKey(t->req.mat);
    if (t->heldMatKey != wantKey)
    {
        // ACQUIRE BEFORE RELEASE, exactly as SyncMaterialTextures does: a
        // material that shares texture paths with the outgoing one then moves
        // the TextureSystem refcount 1 -> 2 -> 1 instead of touching zero,
        // which would drop the path mapping and force a full GPU re-upload.
        const uint64_t previous = t->heldMatKey;
        t->heldMatKey = m_owner.AcquirePreviewMaterialRuntime(t->req.mat);
        if (previous) m_owner.ReleasePreviewMaterialRuntime(previous);
        t->lastTexturesReady = false;
        t->needsRender       = true;   // a new texture set is a new picture
    }
    // Stamped whatever happened above, so the key and the epoch it was taken
    // under can never drift apart.
    t->heldMatEpoch     = matEpoch;
    t->req.matRuntimeKey = t->heldMatKey;

    // The mesh override, resolved on this half: MeshLibrary::Load uploads buffers and
    // MeshManager::RegisterMeshLibMesh allocates a mesh-descriptor slot, both
    // descriptor-heap work. A changed result is a new picture: the slot can appear
    // frames after the request (the library streams) while the ViewDesc stays the same.
    {
        uint32_t slot = 0xFFFFFFFFu, indexCount = 0;
        float    fitScale = 1.f, fitOffset[3] = { 0.f, 0.f, 0.f };
        if (!t->req.view.meshPath.empty())
            m_owner.ResolvePreviewMesh(t->req.view.meshPath, t->req.view.meshId,
                                       slot, indexCount, fitScale, fitOffset);
        if (slot != t->req.meshDescSlot || indexCount != t->req.meshIndexCount)
            t->needsRender = true;
        t->req.meshDescSlot   = slot;
        t->req.meshIndexCount = indexCount;
        t->req.meshFitScale   = fitScale;
        t->req.meshFitOffset[0] = fitOffset[0];
        t->req.meshFitOffset[1] = fitOffset[1];
        t->req.meshFitOffset[2] = fitOffset[2];
    }

    // The material edge. The hash and the GPU record must see the same runtime: the
    // key resolved here is the one RecordRequest re-resolves for WriteMaterialSlot, so the
    // record in slot 1 cannot disagree with the hash that decided to write it.
    const Renderer::ResolvedMaterialRuntime rt =
        m_owner.ResolvePreviewMaterialRuntime(t->heldMatKey);
    const uint64_t matHash = m_owner.HashMaterialParams(t->req.mat, rt);
    if (matHash != t->req.matHash)
    {
        t->req.matHash = matHash;
        t->needsRender = true;
    }

    // Streaming readiness: draw early, re-render later. A live panel showing the white
    // / flat-normal fallbacks for a frame or two reads as loading; a grey box until every
    // texture streams reads as broken. A thumbnail does the opposite and waits on this
    // flag, since it would bake the white version to disk.
    const bool ready = m_owner.IsPreviewMaterialRuntimeReady(t->heldMatKey);
    if (ready != t->lastTexturesReady)
    {
        t->lastTexturesReady = ready;
        t->needsRender       = true;   // the real textures are a new picture
    }
    t->req.texturesReady = ready;

    // Anything above may have armed the flag; mint one generation for all of
    // it. Doing this here rather than per-edge keeps a frame that changed the
    // material AND finished streaming to a single redraw.
    if (t->needsRender)
    {
        t->needsRender   = false;
        t->req.generation = ++t->generation;
    }

    // A hand-over already in flight is not disturbed: its result carries the
    // generation the record snapshotted, so a generation minted above makes it stale on
    // arrival and the next extract re-sends.
    if (t->req.inFlight) return;

    // Already on screen. With the mint edges above this is the dirty-edge regulator:
    // an unchanged panel costs a few hash lookups per frame and no GPU work.
    if (t->req.generation == t->lastRenderedGeneration) return;

    t->reqW = t->req.view.w;
    t->reqH = t->req.view.h;
    t->req.inFlight = true;
}

// DropMaterialScheme / DropAssetScheme: the one place a Target's ownership scheme
// is handed back. A Target holds runtime-material references either as heldMatKey
// (single-material path) or as heldPartKeys (asset path); both lanes switch between
// them - the live extract when a panel moves to an asset and back, the thumbnail
// lane on every tile of a folder that mixes materials and .meshlib / .ipfb assets.
// Every transition goes through these, or the outgoing scheme leaks a reference that
// pins a material's texture set for the session.
// The epoch rule: past an epoch bump OnWorldClear has already destroyed the entries
// refcount-blind, so the keys are forgotten, never released.
void PreviewRenderer::DropMaterialScheme(Target& t, uint32_t matEpoch)
{
    if (t.heldMatKey == 0) return;
    if (t.heldMatEpoch == matEpoch && m_owner.HasPreviewMaterialRuntime(t.heldMatKey))
        m_owner.ReleasePreviewMaterialRuntime(t.heldMatKey);
    t.heldMatKey         = 0;
    t.lastTexturesReady  = false;
    t.req.matRuntimeKey  = 0;
}

void PreviewRenderer::DropAssetScheme(Target& t, uint32_t matEpoch)
{
    if (t.heldPartKeys.empty() && t.heldAssetPath.empty() && t.req.parts.empty())
        return;
    const bool epochOk = (t.heldMatEpoch == matEpoch);
    if (epochOk)
        for (uint64_t k : t.heldPartKeys)
            if (m_owner.HasPreviewMaterialRuntime(k))
                m_owner.ReleasePreviewMaterialRuntime(k);
    t.heldPartKeys.clear();
    t.heldAssetPath.clear();
    t.req.parts.clear();
    t.req.partMaterialCount = 0;
    t.req.partsDropped      = 0;
    t.assetExtractStamp     = 0;
}

// The tripwire for the two schemes: a transition that forgets to drop one is
// otherwise silent (the texture stays resident, the picture correct, only VRAM
// moves).
void PreviewRenderer::CheckSchemeExclusive(const Target& t, const char* where) const
{
    ENGINE_CHECK_ONCE(t.heldMatKey == 0 || t.heldPartKeys.empty(),
        "PreviewRenderer: %s left BOTH runtime-material ownership schemes "
        "populated (heldMatKey=%llu, %zu held part key(s)). They are exclusive "
        "by construction; one of them is now leaked.",
        where, static_cast<unsigned long long>(t.heldMatKey),
        t.heldPartKeys.size());
}

// Logs the asset extract's full-resolve vs skipped counts once per 100-call
// window, so an idle panel's cost is a number in the log.
void PreviewRenderer::NoteAssetExtract(bool full)
{
    if (full) ++m_assetExtractFull; else ++m_assetExtractSkipped;
    if (++m_assetExtractWindow < 100) return;
    LOG_INFO("[PREVIEW] asset extract: window=%u fullResolves=%u skipped=%u",
             m_assetExtractWindow, m_assetExtractFull, m_assetExtractSkipped);
    m_assetExtractWindow  = 0;
    m_assetExtractFull    = 0;
    m_assetExtractSkipped = 0;
}

// ExtractAssetParts: the game half of an asset view or asset tile. Game half only, like
// the single-material extract: it resolves mesh-descriptor slots, registers custom
// shaders and takes refcounted runtime-material references - one per distinct material
// key, acquire-before-release on a change, all handed back when the view switches asset
// or closes.
void PreviewRenderer::ExtractAssetParts(Target& t, uint32_t matEpoch)
{
    Preview::Request& r = t.req;

    // The two heals, as on the single path. Epoch: the table was wiped refcount-blind
    // by an OnWorldClear, so forget the held keys without releasing them.
    if (!t.heldPartKeys.empty() && t.heldMatEpoch != matEpoch)
    {
        t.heldPartKeys.clear();
        t.needsRender = true;
        if (!m_epochHealLogged)
        {
            m_epochHealLogged = true;
            LOG_INFO("[PREVIEW] material runtime epoch changed (world clear) - "
                     "forgetting the asset's held keys WITHOUT releasing them and "
                     "re-acquiring (logged once per session)");
        }
    }
    // MISSING KEY: the same signal the single path uses, applied to the set. One
    // missing key means the whole table went, so the set is dropped whole.
    for (uint64_t k : t.heldPartKeys)
    {
        if (m_owner.HasPreviewMaterialRuntime(k)) continue;
        t.heldPartKeys.clear();
        t.needsRender = true;
        if (!m_selfHealLogged)
        {
            m_selfHealLogged = true;
            LOG_INFO("[PREVIEW] material runtime was dropped by a world clear - "
                     "re-acquiring the asset's materials (logged once per session)");
        }
        break;
    }
    // A different asset is a full hand-back; the acquire loop below takes the new set
    // from scratch. Acquire-before-release matters for the same asset (a slider drag),
    // which keeps its keys here, so this simple order is correct.
    if (t.heldAssetPath != r.view.assetPath)
    {
        for (uint64_t k : t.heldPartKeys) m_owner.ReleasePreviewMaterialRuntime(k);
        t.heldPartKeys.clear();
        t.heldAssetPath = r.view.assetPath;
        t.needsRender   = true;
    }
    // Whichever lane got here, the single-material reference is no longer this
    // Target's; hand it back here (the thumbnail lane has no ViewDesc change to hook).
    // Before the epoch is re-stamped: DropMaterialScheme picks release-or-forget by
    // comparing heldMatEpoch with the current epoch, and stamping first would make a key
    // taken under a dead epoch look live.
    DropMaterialScheme(t, matEpoch);
    CheckSchemeExclusive(t, "ExtractAssetParts");
    t.heldMatEpoch = matEpoch;

    // The idle path. Everything below is a function of (asset path, asset kind,
    // runtime-material epoch, composition-memo epoch, material-content generation, this
    // asset's retry tick, the Mesh Editor fields) plus per-part texture readiness, and
    // only readiness can move without a stamp term moving. So an unchanged request
    // refreshes the readiness bits, arms needsRender if any flipped, and returns. A full
    // resolve re-runs ResolvePreviewMeshEntry, ResolvePreviewCustomPSID/VSID (whose
    // MaterialReflectionSync::Sync mutates the material) and HashMaterialParams per part;
    // the stamp keeps an idle panel at a few hash lookups.
    //   - The retry tick is per asset path (PreviewAssetRetryTick), so a missing
    //     .meshlib wakes only the targets showing it. It is derived from the clock -
    //     1 + frame / kPreviewAssetRetryFrames while the asset is failed, 0 otherwise -
    //     because a failed asset has an empty part vector (allBound vacuously true) and
    //     no resolve would ever move a failure-driven term. The entry is erased on the
    //     first success.
    //   - The material-content generation (Resource::MaterialContentGeneration, bumped by
    //     InvalidateMaterialsDependingOn and ClearMaterialCache on every save) re-resolves
    //     the parts after an .imat is rewritten: the identity terms do not move on a save.
    //     Global rather than per part: a save of an unrelated material costs one extra
    //     resolve, and per-part hashes would need the file bytes.
    //   - A part with no runtime reference forces the full path: Acquire returns 0 while
    //     the TextureSystem is not wired (early boot, mid-teardown), and that has to
    //     converge rather than latch.
    const uint64_t stamp = [&]
    {
        uint64_t h = 1469598103934665603ull;
        auto fold = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        fold(std::hash<std::string>{}(r.view.assetPath));
        fold(static_cast<uint64_t>(r.view.assetKind));
        fold(matEpoch);
        fold(m_owner.PreviewAssetMemoEpoch());
        fold(Resource::MaterialContentGeneration());
        fold(m_owner.PreviewAssetRetryTick(r.view.assetPath));
        // Mesh Editor: which placements are drawn and which one is picked -
        // both reshape the part list, so a change is a full resolve.
        fold(r.view.partMask);
        fold(static_cast<uint64_t>(static_cast<uint32_t>(r.view.highlightPart)));
        fold(r.view.partMaterials.size());
        for (const std::string& m : r.view.partMaterials) fold(std::hash<std::string>{}(m));
        // The shape-key preview's own slots, their data revision and the heatmap's
        // vertex-colour parts all reshape what a part draws.
        fold(r.view.partMeshes.size());
        for (const Preview::ViewDesc::PartMesh& pm : r.view.partMeshes)
        {
            fold(pm.placement);
            fold((static_cast<uint64_t>(pm.meshDescSlot) << 32) | pm.indexCount);
        }
        fold(r.view.partMeshRevision);
        fold(r.view.vertexColorParts);
        return h;
    }();

    // Not "&& !parts.empty()": an asset that composes to zero parts (DemoLoot) is as
    // permanent as a resolved one - the memo cannot change without bumping
    // PreviewAssetMemoEpoch, which is in the stamp - and requiring parts would re-resolve
    // it every frame.
    bool allBound = true;
    for (const Preview::AssetPart& p : r.parts)
        if (p.matRuntimeKey == 0) { allBound = false; break; }

    if (stamp == t.assetExtractStamp && allBound)
    {
        for (Preview::AssetPart& p : r.parts)
        {
            const bool ready = m_owner.IsPreviewMaterialRuntimeReady(p.matRuntimeKey);
            if (ready != p.texturesReady)
            {
                p.texturesReady = ready;
                t.needsRender   = true;   // the real textures are a new picture
            }
        }
        if (t.req.viewId < Preview::kMaxPreviews) NoteAssetExtract(/*full=*/false);
        return;
    }
    t.assetExtractStamp = stamp;
    if (t.req.viewId < Preview::kMaxPreviews) NoteAssetExtract(/*full=*/true);

    // ---- Resolve the asset ----
    std::vector<Preview::AssetPart> parts;
    uint32_t dropped = 0;
    Resource::PrefabSkipCounts skipped{};   // prefab nodes the preview does not draw, by kind
    m_owner.ResolvePreviewAsset(r.view.assetPath, r.view.assetKind, parts, dropped,
                                &skipped,
                                r.view.partMaterials.empty() ? nullptr : &r.view.partMaterials);

    // Mesh Editor: the shape-key preview's own geometry. Right after the resolve and
    // before any filter: a part keeps its placement, material and fitted matrix; only its
    // mesh slot changes, to the panel-owned copy the Mesh Editor morphs.
    for (const Preview::ViewDesc::PartMesh& pm : r.view.partMeshes)
        for (Preview::AssetPart& p : parts)
            if (p.placementIndex == pm.placement && pm.meshDescSlot != 0xFFFFFFFFu && pm.indexCount > 0)
            {
                p.meshDescSlot = pm.meshDescSlot;
                p.indexCount   = pm.indexCount;
            }

    // Mesh Editor: solo / isolate and the picked section. After the resolve, so the
    // framing stays the whole asset's (the fit is baked into every part's matrix); a
    // hidden part is not counted as dropped (that means "could not be drawn").
    // The dim is a plain grey material, not a scaled base colour: a custom surface shader
    // (every UE-imported material instance) computes its own albedo and ignores the
    // factor. The engine default material in grey, custom shaders dropped, reads as
    // context for any picked material.
    if (r.view.partMask != ~0ull || r.view.highlightPart >= 0)
    {
        std::vector<Preview::AssetPart> kept;
        kept.reserve(parts.size());
        for (Preview::AssetPart& p : parts)
        {
            if (p.placementIndex < 64 && !(r.view.partMask & (1ull << p.placementIndex)))
                continue;
            if (r.view.highlightPart >= 0 &&
                p.placementIndex != static_cast<uint32_t>(r.view.highlightPart))
            {
                MaterialComponent grey{};
                grey.SetBaseColor({ 0.30f, 0.30f, 0.32f, 1.f });
                p.mat        = grey;
                p.customPSID = 0;
                p.customVSID = 0;
            }
            kept.push_back(std::move(p));
        }
        parts = std::move(kept);
    }
    // The shape-key heatmap: a white material that multiplies in the vertex colour the
    // panel's copy carries. After the dim, which it overrides.
    if (r.view.vertexColorParts != 0)
        for (Preview::AssetPart& p : parts)
            if (p.placementIndex < 64 && (r.view.vertexColorParts & (1ull << p.placementIndex)))
            {
                MaterialComponent heat{};
                heat.SetBaseColor({ 1.f, 1.f, 1.f, 1.f });
                heat.SetUseVertexColors(true);
                p.mat        = heat;
                p.customPSID = 0;
                p.customVSID = 0;
            }

    // One reference per distinct material. Acquires first, releases second, in
    // separate loops as SyncMaterialTextures does: a material carried over from the
    // previous set must never reach refcount 0 in between, or its TextureSystem handles
    // are handed back and re-uploaded for nothing.
    std::vector<uint64_t> want;
    want.reserve(parts.size());
    for (Preview::AssetPart& p : parts)
    {
        const uint64_t key = m_owner.BuildPreviewMaterialRuntimeKey(p.mat);
        p.matRuntimeKey = key;
        const bool alreadyWanted =
            std::find(want.begin(), want.end(), key) != want.end();
        const bool alreadyHeld =
            std::find(t.heldPartKeys.begin(), t.heldPartKeys.end(), key)
                != t.heldPartKeys.end();
        if (!alreadyWanted && !alreadyHeld)
        {
            // Zero is the one legitimate disagreement: AcquirePreviewMaterialRuntime returns 0
            // without registering anything while the TextureSystem or ResourceManager is not
            // wired (headless or mid-teardown). The part keeps matRuntimeKey 0 and draws with
            // GBufferPass's white / flat-normal fallbacks, like a still-streaming material - not
            // a fault, and not a held reference.
            const uint64_t got = m_owner.AcquirePreviewMaterialRuntime(p.mat);
            if (got == 0)
            {
                p.matRuntimeKey = 0;
            }
            else
            {
                // Any OTHER disagreement is a real inconsistency: the two
                // derivations are the same function, so a differing key means
                // the part references a runtime nobody holds.
                ENGINE_CHECK_ONCE(got == key,
                    "PreviewRenderer: AcquirePreviewMaterialRuntime returned key "
                    "%llu for a material whose BuildPreviewMaterialRuntimeKey is "
                    "%llu - the asset part would reference a runtime nobody holds.",
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(key));
                p.matRuntimeKey = got;
                want.push_back(got);
                t.needsRender = true;
            }
        }
        else if (!alreadyWanted)
        {
            want.push_back(key);
        }
        p.texturesReady = p.matRuntimeKey != 0 &&
                          m_owner.IsPreviewMaterialRuntimeReady(p.matRuntimeKey);
    }
    for (std::size_t i = 0; i < t.heldPartKeys.size(); )
    {
        const uint64_t k = t.heldPartKeys[i];
        if (std::find(want.begin(), want.end(), k) == want.end())
        {
            m_owner.ReleasePreviewMaterialRuntime(k);
            t.heldPartKeys.erase(t.heldPartKeys.begin() + static_cast<std::ptrdiff_t>(i));
            t.needsRender = true;
        }
        else ++i;
    }
    for (uint64_t k : want)
        if (std::find(t.heldPartKeys.begin(), t.heldPartKeys.end(), k)
            == t.heldPartKeys.end())
            t.heldPartKeys.push_back(k);

    // The dirty edge: HashMaterialParams decides whether the material's pixels differ,
    // folded with the rest of a packet - mesh slot, placement matrix, shader ids and
    // streaming state. Without the matrix moved parts would never re-render; without the
    // streaming bit the white-fallback first draw would be the last.
    uint64_t h = 1469598103934665603ull;
    auto fold = [&h](uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ull;
    };
    fold(parts.size());
    fold(dropped);
    // A morphed copy is rewritten under the same slot number.
    fold(r.view.partMeshRevision);
    for (const Preview::AssetPart& p : parts)
    {
        const Renderer::ResolvedMaterialRuntime rt =
            m_owner.ResolvePreviewMaterialRuntime(p.matRuntimeKey);
        fold(m_owner.HashMaterialParams(p.mat, rt));
        fold((static_cast<uint64_t>(p.meshDescSlot) << 32) | p.indexCount);
        fold((static_cast<uint64_t>(p.customPSID) << 32) | p.customVSID);
        fold(p.texturesReady ? 1ull : 0ull);
        for (int i = 0; i < 16; ++i)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &p.world[i], sizeof(uint32_t));
            fold(bits);
        }
    }
    if (h != r.matHash)
    {
        r.matHash     = h;
        t.needsRender = true;
    }

    r.parts             = std::move(parts);
    r.partMaterialCount = static_cast<uint32_t>(want.size());
    r.partsDropped      = dropped;
    r.skipped           = skipped;
    // Over-cap is reported, never silent: the picture is partial by construction and
    // the panel says so; this is the log half, once per session.
    if (dropped > 0 && !m_assetOverCapLogged)
    {
        m_assetOverCapLogged = true;
        LOG_WARNING("[PREVIEW] asset '%s' has more parts than the preview's "
                    "cap of %u - %u part(s) are NOT drawn. The picture is "
                    "partial by construction, not broken.",
                    r.view.assetPath.c_str(), Preview::kMaxPreviewParts, dropped);
    }
}

void PreviewRenderer::GetAssetStats(uint32_t viewId, uint32_t& outParts,
                                    uint32_t& outMaterials,
                                    uint32_t& outDropped,
                                    Resource::PrefabSkipCounts* outSkipped) const
{
    outParts = outMaterials = outDropped = 0;
    if (outSkipped) *outSkipped = Resource::PrefabSkipCounts{};
    const Target* t = Get(viewId);
    if (!t || !t->live) return;
    outParts     = static_cast<uint32_t>(t->req.parts.size());
    outMaterials = t->req.partMaterialCount;
    outDropped   = t->req.partsDropped;
    // Sourced where partsDropped is: the request the record half draws, not a
    // re-parse of the file.
    if (outSkipped) *outSkipped = t->req.skipped;
}

// The shutdown drain: the first loop of ServiceReleases and nothing else (see the
// header for why).
void PreviewRenderer::DrainPendingViewDestroys()
{
    for (Target& t : m_targets)
    {
        if (!t.pendingDestroy) continue;
        t.pendingDestroy = false;
        DestroyTarget(t);
    }
}

void PreviewRenderer::ServiceReleases()
{
    for (Target& t : m_targets)
    {
        if (!t.pendingDestroy) continue;
        t.pendingDestroy = false;
        DestroyTarget(t);
    }

    // The thumbnail lane's runtime-material reference. The lane swaps its one
    // reference in ExtractThumbnails only on a new key, so the last material of a burst
    // would stay pinned in TextureSystem for the session. Released here, once the queue
    // has drained, in the same game-half serial window DestroyTarget uses - not
    // mid-burst, so a folder of materials sharing a texture does not push its refcount
    // through zero once per tile. Epoch-guarded like DestroyTarget: after an
    // OnWorldClear the key names nothing (or a new world's entry), and releasing it would
    // decrement somebody else's.
    if ((m_thumbTarget.heldMatKey != 0 || !m_thumbTarget.heldPartKeys.empty()) &&
        m_thumbQueue.empty() &&
        !m_thumbReq.valid && !m_thumbReq.inFlight && m_thumbAssetKey == 0)
    {
        const bool epochOk =
            (m_thumbTarget.heldMatEpoch == m_owner.MaterialRuntimeEpoch());
        if (epochOk && m_thumbTarget.heldMatKey != 0)
            m_owner.ReleasePreviewMaterialRuntime(m_thumbTarget.heldMatKey);
        // And the asset tile's set, on the same terms.
        if (epochOk)
            for (uint64_t k : m_thumbTarget.heldPartKeys)
                m_owner.ReleasePreviewMaterialRuntime(k);
        m_thumbTarget.heldMatKey = 0;
        m_thumbTarget.heldPartKeys.clear();
        m_thumbTarget.heldAssetPath.clear();
        m_thumbTarget.req.parts.clear();
        m_thumbTarget.assetExtractStamp = 0;   // nothing is held any more
    }

    // The idle sweep, last: the loop above has serviced this frame's queued teardowns,
    // so a view released last frame is already not live when the sweep counts. Running
    // it first would cost every teardown an extra frame of grace.
    ServiceIdleTeardown();
}

// ==== Idle teardown ====
// A compiled preview GraphSet is given back after the idle grace with no live view
// and rebuilt lazily on the next request. The sweep runs from ServiceReleases in
// App's game-half serial window (between EditorLayer::ProcessPendingActions and the
// device frame, render half parked by FramePipeline::RenderPhaseBarrier), the window
// ServiceCompile uses: destroying a graph runs every pass's Shutdown and hands seven
// physical textures to the deferred bucket, which must never happen on or beside a
// graph worker (Phase B records on ParallelFor whatever the render mode).

void PreviewRenderer::SetIdleTeardownFrames(uint32_t frames)
{
    const uint32_t want = frames ? frames : Preview::kIdleTeardownFrames;
    m_idleTeardownFrames = (want < Preview::kMinIdleTeardownFrames)
                         ? Preview::kMinIdleTeardownFrames : want;
    LOG_INFO("[PREVIEW] idle teardown grace = %u frames (default %u)",
             m_idleTeardownFrames, Preview::kIdleTeardownFrames);
}

const float* PreviewRenderer::KeyLightDir() const
{
    return m_keyLightOverridden ? m_keyLightOverride : Preview::kKeyLight.dir;
}

// Harness override of the studio key light's travel direction (see the header for
// why the shipped constant does not move).
bool PreviewRenderer::SetKeyLightDirOverride(const float dir[3])
{
    if (!dir) return false;
    const float lenSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    // A ZERO VECTOR IS REFUSED rather than normalised into a NaN: both consumers
    // normalise, and WriteStudioShadowView's own length test would then simply
    // stop publishing a shadow view - a silent "the shadow disappeared" that
    // reads exactly like the bug this override exists to look for.
    if (!(lenSq > 1e-8f)) return false;
    m_keyLightOverride[0] = dir[0];
    m_keyLightOverride[1] = dir[1];
    m_keyLightOverride[2] = dir[2];
    m_keyLightOverridden  = true;
    LOG_INFO("[PREVIEW] studio key light -> travel (%.3f, %.3f, %.3f) "
             "(-preview_key_light; shipped %.3f, %.3f, %.3f)",
             dir[0], dir[1], dir[2],
             Preview::kKeyLight.dir[0], Preview::kKeyLight.dir[1],
             Preview::kKeyLight.dir[2]);
    return true;
}

// Latches the studio shadow pass's alpha test; the value is pushed to the passes
// on the render half (see the header).
void PreviewRenderer::SetStudioShadowAlphaTest(bool on)
{
    if (m_studioShadowAlphaTest == on) return;
    m_studioShadowAlphaTest = on;
    // And re-arm every live view: neither the ViewDesc nor the material moves when this
    // lever flips (both arms install the same synthesised material), so without this the
    // dirty-edge regulator keeps the old picture and the two arms capture bit-identical
    // images - which reads as "the clip is a visual no-op".
    // Game half, like SyncPreviewEnvironment's copy of this loop: the caller is
    // EditorLayer::TickHarnessPreview inside ProcessPendingActions, the window
    // ExtractRequests reads needsRender in.
    uint32_t redirtied = 0;
    for (Target& t : m_targets)
    {
        if (!t.live) continue;
        t.needsRender = true;
        ++redirtied;
    }
    LOG_INFO("[PREVIEW] studio shadow alpha test -> %s (-preview_shadow_alpha), "
             "%u view(s) redirtied", on ? "ON (shipped)" : "OFF", redirtied);
}

bool PreviewRenderer::LaneIdle(const GraphSet& set) const
{
    if (&set == &m_thumb)
    {
        // The one-shot lane is idle when there is nothing queued, nothing in
        // flight and nothing staged. m_thumbPending is in the test because a
        // staged readback still names this set's tone-map output through the
        // copy that produced it.
        return m_thumbQueue.empty() && !m_thumbReq.valid && !m_thumbReq.inFlight &&
               m_thumbAssetKey == 0 && m_thumbPending.empty() &&
               !m_thumbTarget.live && !m_thumbTarget.pendingDestroy;
    }
    const bool wantWorld = (&set == &m_world);
    for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
    {
        const Target& t = m_targets[i];
        if (!t.live && !t.pendingCreate && !t.pendingDestroy) continue;
        // The slot's acquire-time lane: true from the first frame of a tenancy, even
        // before a request is latched.
        const bool isWorld = (m_slotSource[i] == Preview::Source::WorldCull);
        if (isWorld == wantWorld) return false;
    }
    return true;
}

void PreviewRenderer::DestroyGraphSet(GraphSet& set, const char* kind)
{
    if (!set.compiled) return;
    const TeardownKind kindIdx = (&set == &m_thumb) ? kTearThumb
                               : (&set == &m_world) ? kTearWorld : kTearLive;

    const auto t0 = std::chrono::steady_clock::now();
    const RG::RenderGraph::DestroyStats st = set.graph.Destroy(m_gfx);
    const double destroyMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    set.compiled    = false;
    set.gbuffer     = nullptr;
    set.lighting    = nullptr;
    set.shadow      = nullptr;
    set.skybox      = nullptr;
    set.transparent = nullptr;
    set.passCount   = 0;
    set.hAlbedo = set.hNormal = set.hSurface = set.hDepth =
        set.hVelocity = set.hEmissive = set.hReflectionSource = set.hHdr = {};
    set.w = set.h = 0;
    set.idleFrames = 0;
    ++m_setsDestroyed;

    // The space3 block is deliberately not touched here (see the header's
    // m_customTexTable comment). Its descriptors were stamped from the dead
    // GBufferPass's 1x1 default-white, but no draw binds a stale one: PrefillSpace3White
    // re-stamps the eight slots a draw is about to bind, from the live pass, right before
    // the packer writes them. Do not bulk re-stamp here: that rewrites all 1536
    // descriptors - every ring slot - inside the device frame while another lane's custom
    // draws from frames N-1 / N-2 still sample them as root-36 tables.
    m_customPackLoggedBytes = 0xFFFFFFFFu;
    m_customPackLoggedTex   = 0xFFFFFFFFu;

    const auto cs = m_gfx.GetCbvSrvUavHeapStats();
    const auto rs = m_gfx.GetRtvHeapStats();
    const auto ds = m_gfx.GetDsvHeapStats();
    const auto ss = m_gfx.GetSamplerHeapStats();
    const uint32_t now[4] = { cs.used, rs.used, ds.used, ss.used };
    if (m_teardownCount[kindIdx]++ == 0)
        for (int i = 0; i < 4; ++i) m_teardownFirst[kindIdx][i] = now[i];
    for (int i = 0; i < 4; ++i) m_teardownLast[kindIdx][i] = now[i];

    // One line, name=value throughout (expect.logLineFields). `freed` and `passes` are
    // what the teardown did; the pool block is what it may have done. `used` is a
    // monotonic cursor FreeStatic never decrements, flat exactly when the free list is
    // reused - hence the stability gate compares teardown N with teardown 1.
    LOG_INFO("[PREVIEW] graph set destroyed (%s) freed=%u textures passes=%u "
             "sets=%u ms=%.2f cbvUsed=%u cbvFree=%u rtvUsed=%u rtvFree=%u "
             "dsvUsed=%u dsvFree=%u samplerUsed=%u cmdListPeak=%u",
             kind, st.textures, st.passes, m_setsDestroyed, destroyMs,
             cs.used, cs.freeBlocks, rs.used, rs.freeBlocks,
             ds.used, ds.freeBlocks, ss.used, m_gfx.GetCommandListPeak());
}

void PreviewRenderer::ServiceIdleTeardown()
{
    bool any = false;

    // ---- 1. the three GraphSets ----
    struct Entry { GraphSet* set; const char* kind; };
    const Entry sets[3] = { { &m_live, "live" }, { &m_thumb, "thumb" },
                            { &m_world, "world" } };
    for (const Entry& e : sets)
    {
        if (!e.set->compiled) { e.set->idleFrames = 0; continue; }
        if (!LaneIdle(*e.set)) { e.set->idleFrames = 0; continue; }
        if (++e.set->idleFrames < m_idleTeardownFrames) continue;
        DestroyGraphSet(*e.set, e.kind);
        any = true;
    }

    // ---- 2. the per-view ToneMapPass ----
    // Separate from the sets: a Target's ToneMapPass owns the texture ImGui sampled, so
    // the grace here is the proof that the draw data naming that descriptor has retired,
    // not hysteresis.
    for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
    {
        Target& t = m_targets[i];
        if (t.live || t.pendingCreate || t.pendingDestroy || !t.toneMap)
        {
            t.idleFrames = 0;
            continue;
        }
        if (++t.idleFrames < m_idleTeardownFrames) continue;
        t.toneMap->Shutdown(m_gfx);
        t.toneMap.reset();
        t.toneMapInited = false;
        t.idleFrames    = 0;
        ++m_toneMapsDestroyed;
        any = true;
        LOG_INFO("[PREVIEW] view %u tone-map released (idle %u frames) "
                 "tonemaps=%u", i, m_idleTeardownFrames, m_toneMapsDestroyed);
    }

    // The thumbnail lane's own target, on the same terms.
    if (!m_thumbTarget.live && !m_thumbTarget.pendingCreate &&
        !m_thumbTarget.pendingDestroy && m_thumbTarget.toneMap &&
        LaneIdle(m_thumb))
    {
        if (++m_thumbTarget.idleFrames >= m_idleTeardownFrames)
        {
            m_thumbTarget.toneMap->Shutdown(m_gfx);
            m_thumbTarget.toneMap.reset();
            m_thumbTarget.toneMapInited = false;
            m_thumbTarget.idleFrames    = 0;
            ++m_toneMapsDestroyed;
            any = true;
            LOG_INFO("[PREVIEW] thumb tone-map released (idle %u frames) "
                     "tonemaps=%u", m_idleTeardownFrames, m_toneMapsDestroyed);
        }
    }
    else
    {
        m_thumbTarget.idleFrames = 0;
    }

    if (any) ++m_teardownCycles;
}

void PreviewRenderer::DestroyTarget(Target& t)
{
    // The runtime-material reference goes back here, in ServiceReleases - the game
    // half's serial window, the only context allowed to mutate the runtime-material maps -
    // never in ReleaseView, which runs inside an ImGui handler on a render-side context.
    // (The destructor zeroes heldMatKey first: ~Renderer has already handed every
    // runtime texture back.)
    // Only if the reference still exists: ServiceReleases can run after an OnWorldClear
    // wiped the table and before ExtractRequests noticed (a panel closed during a world
    // load), so the epoch is checked here too - releasing a phantom decrements an entry
    // whose real holders are live scene entities.
    const bool epochOk = (t.heldMatEpoch == m_owner.MaterialRuntimeEpoch());
    if (t.heldMatKey != 0 && epochOk)
    {
        m_owner.ReleasePreviewMaterialRuntime(t.heldMatKey);
    }
    t.heldMatKey = 0;
    // And the asset view's set, under the same epoch guard: exactly one reference per
    // distinct material goes back.
    if (epochOk)
        for (uint64_t k : t.heldPartKeys) m_owner.ReleasePreviewMaterialRuntime(k);
    t.heldPartKeys.clear();
    t.heldAssetPath.clear();
    t.req.parts.clear();
    t.req.partMaterialCount = 0;
    t.req.partsDropped      = 0;
    t.assetExtractStamp     = 0;   // a reused slot re-resolves from scratch
    t.lastTexturesReady = false;

    if (t.hdrSlot != 0)
    {
        m_gfx.DestroyHdrTarget(t.hdrSlot);
        m_hdrSlotsUsed &= ~(1u << t.hdrSlot);
        t.hdrSlot = 0;
    }
    t.w = t.h = 0;
    t.reqW = t.reqH = 0;
    t.hasOutput.store(false, std::memory_order_relaxed);
    // t.toneMap and its output texture are kept: the descriptor ImGui captured points
    // into that texture and ImGui's draw data for the frame that closed the panel is
    // still queued, so freeing it on the close - even in this deferred window - is a
    // use-after-free. hasOutput is false from here on, so nothing new samples it.
    // ServiceIdleTeardown shuts it down once the slot has been idle for the grace, the
    // proof this function cannot have; reopening inside the grace costs one
    // EnsureTexture.
}

// ==== The one-shot thumbnail lane ====
// Game half (the editor's serial window) unless a comment says otherwise.
namespace
{
    // How many frames one queued thumbnail may sit at the head waiting for its
    // textures. A thumbnail is BAKED TO DISK, so unlike the live panel it must
    // not draw the engine's white fallback - a cached white tile is wrong for
    // the life of the cache entry, and nothing would ever invalidate it. Past
    // the cap the request is dropped with one line and the editor's dirty flag
    // is left set, so the eviction sweep asks again later.
    constexpr uint16_t kThumbStreamRetryCap = 120;
    // Ceiling on staged-but-unclaimed readback pixel sets. The lane produces at
    // most one per frame and the editor drains every frame, so this is 0 or 1
    // in the steady state; the cap exists so an editor that stops draining (a
    // modal, a long import) cannot grow a 64 KiB-per-entry list without bound.
    constexpr std::size_t kThumbPendingCap = 4;
}

bool PreviewRenderer::QueueThumbnail(const Preview::ThumbRequest& r)
{
    if (m_thumbDisabled)          return false;
    if (!r.tile.IsValid())        return false;
    if (r.assetKey == 0)          return false;
    if (m_thumbQueue.size() >= Preview::kMaxThumbnailQueue) return false;
    // A duplicate is not harmless: two entries for one asset render twice, and the
    // second result arrives with a generation the editor already applied, so a tile
    // invalidated between them would be marked clean by a render that predates the
    // invalidation.
    if (HasThumbnailInFlight(r.assetKey)) return false;
    m_thumbQueue.push_back(r);
    return true;
}

bool PreviewRenderer::HasThumbnailInFlight(uint64_t assetKey) const
{
    if (m_thumbReq.valid && m_thumbAssetKey == assetKey) return true;
    for (const Preview::ThumbRequest& q : m_thumbQueue)
        if (q.assetKey == assetKey) return true;
    return false;
}

void PreviewRenderer::CancelThumbnail(uint64_t assetKey)
{
    for (auto it = m_thumbQueue.begin(); it != m_thumbQueue.end(); )
        it = (it->assetKey == assetKey) ? m_thumbQueue.erase(it) : it + 1;
    // The in-flight request is voided too. Left flying, the record half would copy into
    // a texture the editor already handed to DestroyTexture - not a use-after-free (the
    // destroy is deferred), but a request that can never publish, re-sent up to its cap.
    // No result is published for the voided request: the editor clears its dirty flag
    // only at apply, so the asset stays dirty and is enqueued again. A FAILED result
    // would latch the editor's "gave up" flag and stop the asset from ever being tiled
    // again after an ordinary eviction or rescan.
    if (m_thumbReq.valid && m_thumbAssetKey == assetKey)
    {
        m_thumbReq = Preview::Request{};
        m_thumbAssetKey = 0;
        m_thumbTile = RHI::Texture{};
        m_thumbResends = 0;
    }
}

void PreviewRenderer::TakeThumbnailResults(std::vector<Preview::ThumbResult>& out)
{
    if (m_thumbDone.empty()) return;
    out.insert(out.end(), m_thumbDone.begin(), m_thumbDone.end());
    m_thumbDone.clear();
}

bool PreviewRenderer::TakeThumbnailPixels(uint64_t& outAssetKey,
                                          uint32_t& outGeneration,
                                          const uint8_t*& outRgba,
                                          uint32_t& outRowPitch,
                                          uint32_t& outWidth, uint32_t& outHeight)
{
    if (m_thumbPending.empty()) return false;
    m_thumbHanded = std::move(m_thumbPending.front());
    m_thumbPending.pop_front();
    outAssetKey   = m_thumbHanded.assetKey;
    outGeneration = m_thumbHanded.generation;
    outRgba       = m_thumbHanded.bytes.data();
    outRowPitch   = m_thumbRowPitch;
    outWidth      = Preview::kThumbnailSize;
    outHeight     = Preview::kThumbnailSize;
    return true;
}

// Device-frame thread, serial (PrepareFrame). READBACK buffers are always
// committed and are never part of an upload batch, so there is no mid-frame
// FlushUploadAndWait here - the rule EnsureNeutralInputs keeps for the same
// reason.
void PreviewRenderer::EnsureThumbReadback()
{
    if (m_thumbReadbackReady) return;
    // Latched failure: a partial failure (slot 0 created and mapped, slot 1 refused)
    // would otherwise leak slot 0's buffer and mapping on every retry - a 64 KiB
    // READBACK buffer per frame behind a warning that prints once.
    if (m_thumbReadbackFailed) return;
    if (!m_thumbTile.IsValid()) return;
    if (!m_gfx.GetTexture2DReadbackFootprint(m_thumbTile, m_thumbRowPitch,
                                             m_thumbRows, m_thumbReadbackBytes))
        return;   // the tile is not resident yet; retry next frame

    bool ok = true;
    uint32_t created = 0;
    for (uint32_t i = 0; i < RHI::kFramesInFlight && ok; ++i)
    {
        RHI::GPUBufferDesc bd{};
        bd.size  = m_thumbReadbackBytes;
        bd.usage = RHI::Usage::READBACK;
        ok = m_gfx.CreateBuffer(bd, m_thumbReadback[i]);
        if (ok)
        {
            ++created;
            m_thumbReadbackMapped[i] = m_gfx.MapBuffer(m_thumbReadback[i]);
        }
        ok = ok && m_thumbReadbackMapped[i] != nullptr;
    }
    if (!ok)
    {
        // UNWIND WHAT WAS CREATED, then latch. DestroyBuffer is the deferred
        // release path, so this is safe in the device-frame serial window, and
        // unmapping is implicit in the destroy (the mapped pointer must not be
        // used after it either way, which is what zeroing it states).
        for (uint32_t i = 0; i < created && i < RHI::kFramesInFlight; ++i)
        {
            m_thumbReadbackMapped[i] = nullptr;
            if (m_thumbReadback[i].IsValid()) m_gfx.DestroyBuffer(m_thumbReadback[i]);
            m_thumbReadback[i] = RHI::GPUBuffer{};
            m_thumbReadbackPrimed[i] = false;
        }
        m_thumbReadbackFailed = true;
        LOG_WARNING("[PREVIEW] thumbnail readback ring unavailable (%u/%u slot(s) "
                    "created before the failure; all handed back) - tiles will "
                    "render and display but will NOT be written to the disk cache "
                    "for the rest of this session",
                    created, RHI::kFramesInFlight);
        return;
    }
    m_thumbReadbackReady = true;
    LOG_INFO("[PREVIEW] thumbnail readback ring ready (%u slots, rowPitch %u, "
             "%llu B each)", RHI::kFramesInFlight, m_thumbRowPitch,
             static_cast<unsigned long long>(m_thumbReadbackBytes));
}

void PreviewRenderer::ServiceThumbReadback()
{
    if (!m_thumbReadbackReady) return;
    const uint32_t cur = m_gfx.GetFrameIndex();
    if (cur >= RHI::kFramesInFlight) return;

    // Read slot `cur`, not (cur+1) % kFramesInFlight: it is the only fence-covered
    // slot. GraphicsDX12::BeginFrame called WaitSlotRetired(cur), which fences the
    // previous tenant of slot `cur` (frame F-3 with kFramesInFlight == 3). Slot
    // (cur+1) % 3 was written by frame F-2 with no fence, and its CopyTexture2DToBuffer
    // may still run while we memcpy - a torn image encoded to disk under a valid stamp
    // set and never invalidated. Costs one more frame of latency. The read must precede
    // this frame's record overwriting the slot, so PrepareFrame calls this at its top.
    // AutoExposurePass's (slot+1) read of its exposure scalar is an accepted exception
    // (a smoothed float on a debug path), not a precedent.
    const uint32_t slot = cur;
    if (!m_thumbReadbackPrimed[slot] || !m_thumbReadbackMapped[slot]) return;
    if (m_thumbPending.size() >= kThumbPendingCap) return;   // leave it primed
    m_thumbReadbackPrimed[slot] = false;

    PendingPixels pp;
    pp.assetKey   = m_thumbReadbackKey[slot];
    pp.generation = m_thumbReadbackGen[slot];
    pp.bytes.resize(static_cast<std::size_t>(m_thumbReadbackBytes));
    std::memcpy(pp.bytes.data(), m_thumbReadbackMapped[slot],
                static_cast<std::size_t>(m_thumbReadbackBytes));
    m_thumbPending.push_back(std::move(pp));
}

// Game half, from ExtractRequests. Pops at most one request per frame; with
// ProcessQueue's one-picture-per-frame rule that keeps the lane hitch-free.
void PreviewRenderer::ExtractThumbnails()
{
    // The lane is off (an HDR output mode; see RecordRequest's tile block). The latch
    // is torn down here, on the game half that owns it; the render half only sets the
    // bool. A latch left valid makes HasThumbnailInFlight true for that asset forever,
    // freezing the browser's eviction sweep and enqueue loop on it while every other
    // dirty entry creates a tile and re-parses its material each frame.
    if (m_thumbDisabled)
    {
        m_thumbQueue.clear();
        if (m_thumbReq.valid || m_thumbAssetKey != 0)
        {
            m_thumbReq      = Preview::Request{};
            m_thumbAssetKey = 0;
            m_thumbTile     = RHI::Texture{};
            m_thumbResends  = 0;
        }
        return;
    }
    if (m_thumbReq.inFlight) return;

    if (m_thumbReq.valid)
    {
        // The held runtime key must still be the one acquired. A resend re-arms the same
        // latch with the same matRuntimeKey, skipping the acquire block below. After an
        // OnWorldClear (world load, Play -> Stop) that key names nothing or a new world's
        // entry, and the record half would fall back to white / flat-normal defaults and
        // bake a white tile to disk under a stamp nothing invalidates. Drop the request
        // instead: nothing is published, the editor's dirty flag is still set, and the
        // browser's sweep re-enqueues the asset through the full extract path.
        const uint32_t matEpochNow = m_owner.MaterialRuntimeEpoch();
        const uint64_t heldKey     = m_thumbTarget.heldMatKey;
        // An asset tile holds a set, not a key: the single-key test would read
        // heldMatKey == 0 and drop every asset resend (a tile refused once while the graph
        // compiled would stay dirty forever). The same questions, asked of the set.
        const bool isAsset = (m_thumbReq.view.assetKind != Preview::AssetKind::None);
        bool held = isAsset ? !m_thumbTarget.heldPartKeys.empty()
                            : (heldKey != 0 &&
                               m_owner.HasPreviewMaterialRuntime(heldKey) &&
                               m_owner.IsPreviewMaterialRuntimeReady(heldKey));
        if (isAsset)
            for (uint64_t k : m_thumbTarget.heldPartKeys)
                if (!m_owner.HasPreviewMaterialRuntime(k) ||
                    !m_owner.IsPreviewMaterialRuntimeReady(k))
                { held = false; break; }

        if (m_thumbTarget.heldMatEpoch != matEpochNow || !held)
        {
            LOG_WARNING("[PREVIEW] thumbnail for key %016llx dropped on resend: its "
                        "runtime material is gone or not ready (epoch %u -> %u). "
                        "Nothing was published; the browser's sweep will ask again.",
                        static_cast<unsigned long long>(m_thumbAssetKey),
                        m_thumbTarget.heldMatEpoch, matEpochNow);
            // Forget without releasing on an epoch change: the entry is gone, and decrementing
            // it would decrement somebody else's (as in DestroyTarget).
            if (m_thumbTarget.heldMatEpoch != matEpochNow)
            {
                m_thumbTarget.heldMatKey = 0;
                m_thumbTarget.heldPartKeys.clear();
                m_thumbTarget.heldAssetPath.clear();
                m_thumbTarget.assetExtractStamp = 0;
            }
            m_thumbReq      = Preview::Request{};
            m_thumbAssetKey = 0;
            m_thumbTile     = RHI::Texture{};
            m_thumbResends  = 0;
            return;
        }

        // A record early-out (graph not compiled, target not created, a size mismatch
        // mid-recompile): re-send the same request rather than pop a new one, which keeps the
        // FIFO in order. Bounded, so a permanent refusal cannot spin.
        if (++m_thumbResends > kThumbStreamRetryCap)
        {
            LOG_WARNING("[PREVIEW] thumbnail for key %016llx was refused by the "
                        "record half %u times; dropping it (the browser's sweep "
                        "will ask again)",
                        static_cast<unsigned long long>(m_thumbAssetKey),
                        m_thumbResends);
            m_thumbDone.push_back(Preview::ThumbResult{ m_thumbAssetKey,
                                                        m_thumbReq.generation, false });
            m_thumbReq = Preview::Request{};
            m_thumbAssetKey = 0;
            m_thumbTile = RHI::Texture{};
            m_thumbResends = 0;
            return;
        }
        m_thumbReq.inFlight = true;
        return;
    }

    if (m_thumbQueue.empty()) return;
    Preview::ThumbRequest r = m_thumbQueue.front();
    m_thumbQueue.pop_front();
    if (!r.tile.IsValid()) return;   // the editor withdrew the tile; drop it

    // Do not bake an unlit tile either: a thumbnail is written to disk under a stamp
    // only a source change invalidates, so a tile baked while the studio IBL was the 1x1
    // black fallback stays wrong. (The live panel draws at once and re-renders on the IBL
    // edge, because a blank panel reads as broken.)
    // Bounded by its own cap, and it does not give up: the studio HDRI is engine-shipped
    // and lands within a frame or two of the first SyncPreviewEnvironment, so the cap only
    // covers "it is never coming", and then the tile the renderer can draw is baked
    // rather than the asset refused. 30 frames is well under the 120-frame streaming cap,
    // and the request goes back to the front, so a folder keeps its order.
    constexpr uint16_t kThumbIblWaitCap = 30;
    if (!m_iblEverReady && r.retries < kThumbIblWaitCap)
    {
        ++r.retries;
        m_thumbQueue.push_front(std::move(r));
        return;
    }

    Target& t = m_thumbTarget;

    // The two OnWorldClear signals the live lane uses (epoch, missing key): the table
    // is wiped refcount-blind on every world load and Play -> Stop, so forget the key
    // without releasing it in both cases.
    const uint32_t matEpoch = m_owner.MaterialRuntimeEpoch();
    if (t.heldMatKey != 0 &&
        (t.heldMatEpoch != matEpoch || !m_owner.HasPreviewMaterialRuntime(t.heldMatKey)))
        t.heldMatKey = 0;

    // An asset tile (.meshlib or .ipfb): the same lane, budget and disk cache with a
    // different draw source. It goes through ExtractAssetParts, the per-part ownership
    // code the live lane uses, so a tile and a panel of one prefab hold references
    // identically. The stricter readiness rule applies: no part may still be streaming,
    // or a white tile is baked to disk under a stamp nothing invalidates.
    if (r.assetKind != Preview::AssetKind::None && !r.assetPath.empty())
    {
        t.req.view.assetKind = r.assetKind;
        t.req.view.assetPath = r.assetPath;
        t.needsRender = false;               // this lane has no dirty-edge gate
        ExtractAssetParts(t, matEpoch);

        // Zero parts is permanent and must not wait: an asset resolving to no part (an
        // .ipfb placing only primitives, a .meshlib of empty entries, an unparsable path)
        // would otherwise sit at the front of the one-tile-per-frame FIFO for
        // kThumbStreamRetryCap frames. Nothing changes without the files changing, which
        // invalidates the tile through NotifyThumbnailDependencyChanged, so it is dropped at
        // once with a FAILED result (latching AssetEntry::previewGaveUp so rescans do not
        // re-enqueue it). One line per asset.
        const bool haveParts = !t.req.parts.empty();
        if (!haveParts)
        {
            if (m_assetNoPartsLogged.insert(r.assetPath).second)
                LOG_INFO("[PREVIEW] asset thumbnail '%s' resolves to NO drawable "
                         "part; no tile is baked and the entry is marked "
                         "'no preview' rather than retried (a source change "
                         "re-arms it through the dependency index)",
                         r.assetPath.c_str());
            m_thumbDone.push_back(Preview::ThumbResult{ r.assetKey, r.generation, false });
            return;
        }

        bool allReady = true;
        for (const Preview::AssetPart& p : t.req.parts)
            if (!p.texturesReady) { allReady = false; break; }

        if (!allReady)
        {
            if (r.retries < kThumbStreamRetryCap)
            {
                ++r.retries;
                m_thumbQueue.push_front(std::move(r));
            }
            else
            {
                LOG_WARNING("[PREVIEW] asset thumbnail for key %016llx gave up after "
                            "%u frames (textures still streaming); not baking a "
                            "partial tile (the browser's sweep will ask again)",
                            static_cast<unsigned long long>(r.assetKey),
                            static_cast<unsigned>(r.retries));
                m_thumbDone.push_back(Preview::ThumbResult{ r.assetKey, r.generation, false });
            }
            return;
        }

        m_thumbAssetKey = r.assetKey;
        m_thumbTile     = r.tile;
        m_thumbResends  = 0;

        // The parts and counts already live on t.req (ExtractAssetParts wrote them), so
        // the hand-over keeps that request: moving them into a fresh Request would leave
        // t.req a moved-from vector the next extract compares against.
        t.req.valid      = true;
        t.req.inFlight   = true;
        t.req.viewId     = Preview::kMaxPreviews;
        t.req.generation = r.generation;
        t.req.mat            = MaterialComponent{};
        t.req.matRuntimeKey  = 0;
        t.req.customPSID     = 0;
        t.req.customVSID     = 0;
        t.req.texturesReady  = true;
        // The tile ViewDesc: the asset fields kept, everything else pinned as the
        // material tile pins it (see the note there).
        const Preview::AssetKind    keepKind = t.req.view.assetKind;
        const std::string           keepPath = t.req.view.assetPath;
        t.req.view = Preview::ViewDesc{};
        t.req.view.w = t.req.view.h = Preview::kThumbnailSize;
        t.req.view.shape     = Preview::Shape::Sphere;
        t.req.view.assetKind = keepKind;
        t.req.view.assetPath = keepPath;
        // heldAssetPath tracks the path the references were taken for, and the
        // ViewDesc round trip above must not make the next extract think the
        // asset changed.
        t.heldAssetPath = keepPath;

        m_thumbReq = t.req;
        t.reqW = t.reqH = Preview::kThumbnailSize;
        return;
    }

    // A material tile behind an asset tile: the lane reuses one Target, so the asset
    // tile's per-part references are still held; hand them back through DropAssetScheme
    // before the material scheme takes over.
    DropAssetScheme(t, matEpoch);
    CheckSchemeExclusive(t, "ExtractThumbnails (material tile)");

    // Game half only: RegisterDynamic reallocates a vector a compiling worker may hold
    // an entry pointer into. Before the key, as in ExtractOneRequest: Sync adds the
    // reflection's declared-but-unauthored space3 slots and
    // BuildPreviewMaterialRuntimeKey folds them, so a pre-sync key would mean a second
    // acquire and a second stream for one picture.
    r.customPSID = m_owner.ResolvePreviewCustomPSID(r.mat);
    r.customVSID = m_owner.ResolvePreviewCustomVSID(r.mat);

    const uint64_t wantKey = m_owner.BuildPreviewMaterialRuntimeKey(r.mat);
    if (t.heldMatKey != wantKey)
    {
        // ACQUIRE BEFORE RELEASE, as SyncMaterialTextures does: two materials
        // that share a texture path then move its refcount 1 -> 2 -> 1 rather
        // than through zero, which would drop the mapping and force a full GPU
        // re-upload on the very next tile.
        const uint64_t previous = t.heldMatKey;
        t.heldMatKey = m_owner.AcquirePreviewMaterialRuntime(r.mat);
        if (previous) m_owner.ReleasePreviewMaterialRuntime(previous);
    }
    t.heldMatEpoch  = matEpoch;
    r.matRuntimeKey = t.heldMatKey;

    r.texturesReady = m_owner.IsPreviewMaterialRuntimeReady(t.heldMatKey);

    // Do not bake a white thumbnail: a tile is written to disk under a key only a
    // source change invalidates, so a tile baked while a texture streams stays wrong.
    // (The live panel draws at once with the fallbacks and re-renders on the readiness
    // edge.) Requeued at the front, not the back: the reference this lane holds drives
    // the load, and rotating to another material would release it and restart the
    // stream, so a back-requeue over a full folder never converges.
    if (!r.texturesReady)
    {
        if (r.retries < kThumbStreamRetryCap)
        {
            ++r.retries;
            m_thumbQueue.push_front(std::move(r));
        }
        else
        {
            LOG_WARNING("[PREVIEW] thumbnail for key %016llx gave up after %u "
                        "frames waiting for its textures; not baking a white "
                        "tile (the browser's sweep will ask again)",
                        static_cast<unsigned long long>(r.assetKey),
                        static_cast<unsigned>(r.retries));
            m_thumbDone.push_back(Preview::ThumbResult{ r.assetKey, r.generation, false });
        }
        return;
    }

    // ---- Hand it over ----
    m_thumbAssetKey = r.assetKey;
    m_thumbTile     = r.tile;
    m_thumbResends  = 0;

    m_thumbReq = Preview::Request{};
    m_thumbReq.valid      = true;
    m_thumbReq.inFlight   = true;
    m_thumbReq.viewId     = Preview::kMaxPreviews;   // the thumbnail lane's id
    m_thumbReq.generation = r.generation;
    m_thumbReq.mat            = std::move(r.mat);
    m_thumbReq.matRuntimeKey  = r.matRuntimeKey;
    m_thumbReq.customPSID     = r.customPSID;
    m_thumbReq.customVSID     = r.customVSID;
    m_thumbReq.texturesReady  = true;
    // The ViewDesc: pinned square, sphere, no orbit offsets, black background - a tile
    // cached on disk must not depend on where a user left the inspector's camera.
    // Default-constructed on purpose: features 0 (no studio shadow, no backdrop and above
    // all no Feature::kSceneLighting, or a tile lit by the live scene is baked to disk),
    // Background::Solid, and no meshPath so the primitive is drawn. Do not copy the live
    // panel's ViewDesc here; if that ever changes, this line must filter it.
    m_thumbReq.view = Preview::ViewDesc{};
    m_thumbReq.view.w = m_thumbReq.view.h = Preview::kThumbnailSize;
    m_thumbReq.view.shape = Preview::Shape::Sphere;

    // The extract snapshot, kept consistent with the pinned tile size.
    t.reqW = t.reqH = Preview::kThumbnailSize;
}

void PreviewRenderer::ApplyThumbFeedback()
{
    if (m_thumbResult.assetKey == 0) return;
    const Preview::ThumbResult r = m_thumbResult;
    m_thumbResult = {};
    // The round trip is over: release the latch so the next extract pops a new
    // head. Guarded on the identity AND the generation - a result for an asset
    // the lane has since moved on from (cancelled, re-queued after an
    // invalidation) must not clear the latch of the one now in flight.
    if (m_thumbReq.valid && m_thumbAssetKey == r.assetKey &&
        m_thumbReq.generation == r.generation)
    {
        m_thumbReq = Preview::Request{};
        m_thumbAssetKey = 0;
        m_thumbTile = RHI::Texture{};
        m_thumbResends = 0;
    }
    // The dirty flag lives on the editor and is cleared by it, only when it takes
    // this result.
    m_thumbDone.push_back(r);
}

// ResolveRequestedSize: the one source of truth for a view's pixel size (see the
// header). The extract snapshot (reqW/reqH) wins: ServiceCompile sizes the graph and
// PrepareFrame the HDR target from it, and RecordRequest's guard compares the two;
// the latch is the bootstrap for frames before the first hand-over.
bool PreviewRenderer::ResolveRequestedSize(const Target& t,
                                           uint32_t& w, uint32_t& h) const
{
    w = t.reqW ? t.reqW : t.req.view.w;
    h = t.reqH ? t.reqH : t.req.view.h;
    return w != 0 && h != 0;
}

// ==== Game half, serial window ====
// ServiceCompile is called by App beside ProcessPendingActions.
bool PreviewRenderer::ServiceCompile(GBufferPass* mainGBuffer,
                                     TransparentPass* mainTransparent)
{
    // The checker backdrop cube is built here, not in Init: CreateTexture with
    // initialData ends in FlushUploadAndWait, a hard sync anywhere inside
    // BeginFrame..EndFrame, and this game-half serial window is outside the device frame.
    // 8x8x6 RGBA8 = 1.5 KiB and one descriptor, once per session - and only when a live
    // view asks for Background::Checker, which this sees a frame before the record half
    // reads it. A failed create degrades to the solid clear colour.
    bool anyChecker = false;
    const Target* liveSynth = nullptr;   // the first live Synthetic view
    const Target* owedSynth = nullptr;   // the Synthetic view owed a picture (see below)
    const Target* liveWorld = nullptr;   // the WorldCull view m_world serves
    for (uint32_t vi = 0; vi < Preview::kMaxPreviews; ++vi)
    {
        const Target& tv = m_targets[vi];
        if (!tv.live || !tv.req.valid) continue;
        if (tv.req.view.background == Preview::Background::Checker) anyChecker = true;
        // The slot's lane (the loop is indexed for exactly this).
        if (SlotSource(vi) == Preview::Source::WorldCull)
        {
            // First wins, safely: SetRequest refuses any WorldCull request that does not match
            // the lane's fixed size (WorldCullSizeAllowed), so every live WorldCull latch carries
            // the same w/h. Do not delete that refusal: this line would then recompile the graph
            // every frame, silently.
            if (!liveWorld) liveWorld = &tv;
        }
        else
        {
            if (!liveSynth) liveSynth = &tv;
            // Owed = a generation not yet recorded, picked the way ProcessQueue's pickBest
            // orders the lane (oldest lastTickFrame, lowest id on a tie).
            const bool owed = tv.needsRender || tv.req.generation != tv.lastRenderedGeneration;
            if (owed && (!owedSynth || tv.lastTickFrame < owedSynth->lastTickFrame))
                owedSynth = &tv;
        }
    }
    if (mainGBuffer && anyChecker) EnsureCheckerCube();

    // The thumbnail graph, pinned to kThumbnailSize. Built whenever there is thumbnail
    // work, independent of the live graph: a browser full of tiles with no inspector open
    // still needs it, and an inspector with no browser work must not build it. Nothing
    // exists until something asks.
    if (mainGBuffer &&
        (!m_thumbQueue.empty() || m_thumbReq.valid || m_thumb.compiled))
    {
        // The thumbnail set gets no TransparentPass#2, by scope: a 128^2 tile of a glass
        // ball is a grey disc whichever path draws it, and the forward instance costs three
        // more samplers and a second borrowed library. A translucent material's tile is drawn
        // opaque through the GBuffer route; its live panel is not.
        CompileGraphSet(m_thumb, mainGBuffer, nullptr,
                        Preview::kThumbnailSize, Preview::kThumbnailSize);
    }

    // The WorldCull graph: compiled lazily the first time a camera view exists and
    // never otherwise, which keeps Game.exe's `previewViews == 0` idle assertion true for
    // a game that never calls SetGamePiP.
    if (mainGBuffer && liveWorld)
    {
        uint32_t ww = 0, wh = 0;
        if (ResolveRequestedSize(*liveWorld, ww, wh))
            CompileGraphSet(m_world, mainGBuffer, mainTransparent, ww, wh);
    }

    // Nothing to build until a panel has asked for a size: a session that never opens
    // the inspector has no graph, textures, PSOs or descriptors.
    // The size follows the owed view: m_live has one size, so it is compiled for the
    // Synthetic view owed a picture and stays put while none is. Synthetic views render
    // only on an edge, so two panels at two sizes (the Mesh Editor beside an Asset
    // Preview) take turns - a resize is 3-8 ms and rebuilds no PSO - instead of one
    // starving, and an idle lane never recompiles.
    const Target* t = owedSynth ? owedSynth : liveSynth;
    if (!t) return m_live.compiled;
    if (!owedSynth && m_live.compiled) return true;
    // The same field PrepareFrame builds the target from (ResolveRequestedSize), so
    // the graph and the target cannot be sized from different sources.
    // The guard in RecordRequest (set.w != t->w) is load-bearing: sizes can still move
    // after this serial window (a debounced resize commits, a view is re-acquired), and it
    // is the only thing between a stale graph and a draw into physical textures of the
    // wrong size, which the debug layer does not report. Do not delete it.
    uint32_t w = 0, h = 0;
    if (!ResolveRequestedSize(*t, w, h)) return m_live.compiled;

    return CompileGraphSet(m_live, mainGBuffer, mainTransparent, w, h);
}

bool PreviewRenderer::CompileGraphSet(GraphSet& set, GBufferPass* mainGBuffer,
                                      TransparentPass* mainTransparent,
                                      uint32_t w, uint32_t h)
{
    // Borrowing: the preview's passes compile through the main instances'
    // ShaderLibraries, so a custom material's shaders compile once, reload once and are
    // the same blobs in the panel, the thumbnail and the main viewport; dynamic ids come
    // from the one DynamicShaderRegistry.
    if (!ENGINE_CHECK_ONCE(mainGBuffer != nullptr,
            "PreviewRenderer::ServiceCompile called before the main GBufferPass "
            "exists; the preview graph cannot borrow its ShaderLibrary and will "
            "not be built."))
        return false;
    if (w == 0 || h == 0) return set.compiled;

    if (!set.compiled)
    {
        // Local aliases (references into `set`) so the body reads like
        // Renderer::Compile.
        auto& m_hAlbedo   = set.hAlbedo;
        auto& m_hNormal   = set.hNormal;
        auto& m_hSurface  = set.hSurface;
        auto& m_hDepth    = set.hDepth;
        auto& m_hVelocity = set.hVelocity;
        auto& m_hEmissive = set.hEmissive;
        auto& m_hReflectionSource = set.hReflectionSource;
        auto& m_graph     = set.graph;
        auto& m_gbuffer   = set.gbuffer;
        auto& m_lighting  = set.lighting;

        // Graph-scope textures, mirroring Renderer::Compile with the same formats. Two are
        // load-bearing:
        //   * GBuffer1 is UNORM, not FLOAT: its alpha carries the material index as an exact
        //     16-bit integer, which FP16's 11-bit significand cannot hold above 2048;
        //   * depth has a stencil plane: Lighting.ps selects its shading model (PBR=1 /
        //     NPR=2 / Unlit=3) by stencil ref, so plain D32_FLOAT would give every material
        //     the PBR variant.
        const std::string p = set.suffix;   // "Preview" / "PreviewThumb" / "PreviewWorld"
        m_hAlbedo   = m_graph.CreateTexture((p + "GBuffer0_Albedo").c_str(),
            { RHI::Format::R11G11B10_FLOAT,      0, 0, false, false });
        m_hNormal   = m_graph.CreateTexture((p + "GBuffer1_Normal").c_str(),
            { RHI::Format::R16G16B16A16_UNORM,   0, 0, false, false });
        m_hSurface  = m_graph.CreateTexture((p + "GBuffer2_Surface").c_str(),
            { RHI::Format::R8G8B8A8_UNORM,       0, 0, false, false });
        m_hDepth    = m_graph.CreateTexture((p + "GBuffer_Depth").c_str(),
            { RHI::Format::D32_FLOAT_S8X24_UINT, 0, 0, true,  false });
        m_hVelocity = m_graph.CreateTexture((p + "GBuffer3_Velocity").c_str(),
            { RHI::Format::R16G16_FLOAT,         0, 0, false, false });
        m_hEmissive = m_graph.CreateTexture((p + "GBuffer4_Emissive").c_str(),
            { RHI::Format::R16G16B16A16_FLOAT,   0, 0, false, false });
        // LightingPass's SECOND render target. Its PSO has a fixed two-RT
        // contract, so this is not optional even though the preview never reads
        // the reflection-source copy: drawing with only HDR bound would route
        // SV_Target0 into the wrong resource.
        m_hReflectionSource = m_graph.CreateTexture((p + "ReflectionSource").c_str(),
            { RHI::Format::R16G16B16A16_FLOAT,   0, 0, false, false });
        // The slot's HDR target, imported under the main graph's name
        // (RG::kHdrSceneTexture) so the shared pass classes find it in Setup; RecordRequest
        // binds it before every Execute.
        if (RG::RenderGraph::ImportsEnabled())
            set.hHdr = m_graph.ImportTexture(RG::kHdrSceneTexture,
                { m_gfx.GetHdrSceneColorFormat(), 0, 0, false, false });

        // PreviewStudioShadowPass first: GBufferPass declares BuiltinTexture::HdrSceneColor
        // as bind + clear, so anything after it touching the HDR target would clear over its
        // own work; first also means LightingPass reads a map written earlier in the same
        // command-list sequence.
        // Not in the thumbnail set: a tile's ViewDesc is default-constructed with features 0
        // (ExtractThumbnails' hand-over), so Feature::kStudioShadow is never set and the pass
        // could never run there - it would cost a 512^2 x2 D32 array (2 MB) and a PSO for
        // nothing. Every consumer guards on set.shadow (the roster line, the view,
        // WriteStudioShadowView's wantShadow, the SetExternalWait target, the tone-map tail),
        // so a tile that ever wants a shadow finds set.shadow == nullptr rather than a
        // silently dead pass.
        if (&set != &m_thumb)
        {
            auto sp = std::make_unique<PreviewStudioShadowPass>();
            sp->SetSharedShaderLibrary(mainGBuffer->GetShaderLibrary());
            sp->SetNameSuffix(set.suffix);
            set.shadow    = sp.get();
            ++set.passCount;
            m_graph.AddPass(std::move(sp));
        }
        {
            auto gb = std::make_unique<GBufferPass>(m_hAlbedo, m_hNormal, m_hSurface,
                                                    m_hDepth, m_hVelocity, m_hEmissive);
            // Both before Init(), which Compile() runs: Init wires the PSOCache to the library
            // (the owner also registers its two static shaders there, which a borrower must not
            // repeat), and the name keys timestamp regions and PIX markers that
            // GetGPUTimestampResult resolves first-match-wins.
            gb->SetSharedShaderLibrary(mainGBuffer->GetShaderLibrary());
            gb->SetNameSuffix(set.suffix);
            m_gbuffer = gb.get();
            ++set.passCount;
            m_graph.AddPass(std::move(gb));
        }
        // SkyboxPass#2, the backdrop: after GBufferPass (whose HdrSceneColor declaration
        // cleared the target) and before LightingPass (additive on top, Preserve). Its depth
        // test - GREATER_EQUAL against the far plane (0) under reversed-Z - keeps it behind
        // the subject without a stencil test or a depth pre-pass. Self-gating twice: Execute
        // early-outs on a zero env map, and the view hides it (ShowFlag::Sky,
        // ApplyPreviewView) for a Solid backdrop or a wire / debug view mode - only the view
        // saves the command list, the barrier and the PIX region.
        {
            auto sk = std::make_unique<SkyboxPass>(m_hDepth);
            sk->SetNameSuffix(set.suffix);
            set.skybox    = sk.get();
            ++set.passCount;
            m_graph.AddPass(std::move(sk));
        }
        {
            auto lp = std::make_unique<LightingPass>(m_hAlbedo, m_hNormal, m_hSurface,
                                                     m_hDepth, m_hEmissive,
                                                     m_hReflectionSource);
            lp->SetNameSuffix(set.suffix);
            m_lighting = lp.get();
            ++set.passCount;
            m_graph.AddPass(std::move(lp));
        }
        // TransparentPass#2, the real forward path. It borrows TransparentPass's library,
        // not GBufferPass's: a custom PS's SV_Target count comes from its permutation bits,
        // so a translucent material's shader is a 1-RTV forward binary, and resolving it
        // against the 6-RTV GBuffer library would bind the wrong PSO shape.
        // Its three ReadSRV declarations all name the preview's normal target. On the main
        // pipeline they are GBuffer1, the SSRTraceDepth snapshot and the translucent
        // front-layer depth, which the forward shader opens bindless from
        // LightCB.fwdRefl*Idx - null here. The declarations only keep the texture
        // shader-readable across the pass, so naming one texture three times is correct.
        if (mainTransparent)
        {
            auto tp = std::make_unique<TransparentPass>(m_hDepth, m_hNormal,
                                                        m_hNormal, m_hNormal);
            tp->SetSharedShaderLibrary(mainTransparent->GetShaderLibrary());
            tp->SetNameSuffix(set.suffix);
            set.transparent    = tp.get();
            ++set.passCount;
            m_graph.AddPass(std::move(tp));
        }

        m_graph.SetFixedDimensions(w, h);
        // Timed: the idle-teardown grace trades against this number. `ms=` rides after the
        // size so packs asserting "... graph compiled <W>x<H>" verbatim still match.
        // The upload stats are reset here and reported as flushes= / flushMs=: every pass
        // Init() that creates a resource with initialData ends in a blocking
        // FlushUploadAndWait. The only other reader (EditorLayer::LoadSceneInternal) resets
        // and reads in one straight-line block at a different point of the serial window, so
        // the two cannot interleave.
        m_gfx.ResetUploadStats();
        const auto t0 = std::chrono::steady_clock::now();
        m_graph.Compile(m_gfx);
        const double compileMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        const uint32_t flushes = m_gfx.GetUploadFlushCount();
        const double   flushMs = m_gfx.GetUploadStallMs();
        set.w = w;
        set.h = h;
        set.compiled = true;
        // A build past the first is a rebuild (`compiled` goes false only in
        // DestroyGraphSet); reported per wall-clock hour on the session-totals line.
        if (set.builds++ > 0)
        {
            ++m_graphRebuilds;
            m_graphRebuildMs += compileMs;
        }
        // Separate messages: preview_resize asserts the live one verbatim ("preview graph
        // compiled 256x256"), and a shared wording would let the thumbnail graph satisfy it
        // in a run where the panel never compiled.
        if (&set == &m_thumb)
            LOG_INFO("[PREVIEW] thumbnail graph compiled %ux%u ms=%.2f (second "
                     "GBuffer/Lighting instance, pinned; borrows the same main "
                     "ShaderLibrary) flushes=%u flushMs=%.2f",
                     w, h, compileMs, flushes, flushMs);
        else if (&set == &m_world)
            // A third wording, for the same reason as the thumbnail one.
            LOG_INFO("[PREVIEW] world graph compiled %ux%u ms=%.2f (WorldCull draw "
                     "source; third GBuffer/Lighting instance, borrows the same "
                     "main ShaderLibrary) flushes=%u flushMs=%.2f",
                     w, h, compileMs, flushes, flushMs);
        else
            LOG_INFO("[PREVIEW] preview graph compiled %ux%u ms=%.2f "
                     "(GBufferPassPreview borrows the main ShaderLibrary; "
                     "LightingPassPreview owns its own static one) "
                     "flushes=%u flushMs=%.2f",
                     w, h, compileMs, flushes, flushMs);
        // The pass roster as one assertable line, separate from the verbatim-asserted line
        // above. It reports the optional passes by presence; whether they run is the view's
        // business (ApplyPreviewView) and changes per frame.
        LOG_INFO("[PREVIEW] %s pass roster: shadow=%d skybox=%d transparent=%d "
                 "passes=%u",
                 (&set == &m_thumb) ? "thumbnail"
                                    : ((&set == &m_world) ? "world" : "preview"),
                 set.shadow ? 1 : 0, set.skybox ? 1 : 0, set.transparent ? 1 : 0,
                 set.passCount);
        if (&set == &m_world) ++m_worldCompileCount;
        return true;
    }

    if (w != set.w || h != set.h)
    {
        // A recompile destroys and recreates every physical texture, which is why the
        // widget quantises the panel size to four values and debounces it for 200 ms. The
        // thumbnail set is pinned to kThumbnailSize and never reaches this branch.
        set.graph.SetFixedDimensions(w, h);
        set.graph.MarkDirty();
        // Upload stats as on the first build. A resize recreates only the physical
        // textures (the passes survive, so no Init() and no initialData upload): ~3-8 ms and
        // 0 flushes against 45-70 ms and 12-15 flushes for a first build. That is why a
        // resize is cheap and a rebuild after an idle teardown is not, and why the
        // `recompiled` line is gated at max 2 flushes: a recompile that flushes is a pass
        // whose resources moved into the resize path.
        m_gfx.ResetUploadStats();
        const auto t0 = std::chrono::steady_clock::now();
        set.graph.Compile(m_gfx);
        const double recompileMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        set.w = w;
        set.h = h;
        LOG_INFO("[PREVIEW] %s graph recompiled at %ux%u ms=%.2f "
                 "flushes=%u flushMs=%.2f",
                 (&set == &m_thumb) ? "thumbnail"
                                    : ((&set == &m_world) ? "world" : "preview"),
                 w, h, recompileMs, m_gfx.GetUploadFlushCount(),
                 m_gfx.GetUploadStallMs());
        if (&set == &m_world) ++m_worldCompileCount;
    }
    return true;
}

void PreviewRenderer::ReloadShaders(IGraphicsDevice& gfx)
{
    // Nothing to reload before a graph exists; the guard keeps the log honest. All
    // three instances, or a camera / split view keeps PSOs built from the old bytecode.
    // They borrow the same GBuffer library, and GBufferPass gates the library's
    // ClearCaches on OwnsShaderLibrary, so each borrower clears only its own PSOCache and
    // the owner's reload epoch moves exactly once.
    if (m_live.compiled)  m_live.graph.ReloadShaders(gfx);
    if (m_thumb.compiled) m_thumb.graph.ReloadShaders(gfx);
    if (m_world.compiled) m_world.graph.ReloadShaders(gfx);

    // Each view's ToneMapPass is outside those graphs and owns its library.
    for (Target& t : m_targets)
        if (t.toneMap && t.toneMapInited) t.toneMap->ReloadShaders(gfx);
    if (m_thumbTarget.toneMap && m_thumbTarget.toneMapInited)
        m_thumbTarget.toneMap->ReloadShaders(gfx);

    // And re-arm every live view: a reloaded shader is a new picture (a custom shader
    // edited on disk, the Material Editor's live graph shader) but no ViewDesc or
    // material moved, so the dirty-edge regulator would keep the old one. Main thread,
    // before the frame's extract reads it.
    for (Target& t : m_targets)
        if (t.live) t.needsRender = true;
}

void PreviewRenderer::SetEnvironment(uint64_t irradianceCube, uint64_t radianceCube,
                                     uint32_t radianceMips, uint64_t brdfLut)
{
    m_iblIrradiance   = irradianceCube;
    m_iblRadiance     = radianceCube;
    m_iblRadianceMips = radianceMips ? radianceMips : 1u;
    m_brdfLut         = brdfLut;

    // The IBL readiness edge, like Target::lastTexturesReady one level up: the studio
    // HDRI streams, so "both cubes resolved" can land after a view drew, and an unchanged
    // view mints no generation by itself. Game half (called from
    // Renderer::SyncPreviewEnvironment inside BuildRenderScene), the thread
    // ExtractRequests reads needsRender on. Both cubes, not either: Renderer zeroes both
    // when one is missing.
    if ((irradianceCube != 0) && (radianceCube != 0) && !m_iblEverReady)
    {
        m_iblEverReady = true;
        uint32_t live = 0, redirtied = 0;
        for (Target& t : m_targets)
        {
            if (!t.live) continue;
            ++live;
            // Armed, not minted: ExtractOneRequest mints one generation for everything that
            // armed the flag this frame, so a frame that finished the IBL and changed the
            // material redraws once.
            t.needsRender = true;
            ++redirtied;
        }
        // Pending thumbnails have not drawn, so they need no arming - ExtractThumbnails
        // refuses to bake while the IBL is not ready. Reported so the two halves read as one
        // statement.
        const uint32_t thumbsPending =
            static_cast<uint32_t>(m_thumbQueue.size()) +
            ((m_thumbReq.valid || m_thumbReq.inFlight) ? 1u : 0u);
        // 1 = a picture had already been recorded when the IBL landed (the race this edge
        // exists for); a pack forcing it with -preview_ibl_delay asserts it.
        const uint32_t afterFirstRender =
            (m_viewsRenderedTotal.load(std::memory_order_relaxed) > 0) ? 1u : 0u;
        LOG_INFO("[PREVIEW] studio IBL ready -> %u view(s) redirtied: "
                 "redirtied=%u views=%u thumbsPending=%u afterFirstRender=%u",
                 redirtied, redirtied, live, thumbsPending, afterFirstRender);
    }
}

// ==== Device-frame thread, serial, before the graph Execute ====
bool PreviewRenderer::EnsureSceneBuffers()
{
    // The custom-shader net is checked first and outside the ready latch, so it is
    // retried: its descriptor block needs the preview GBufferPass (for the 1x1
    // default-white), which exists only after ServiceCompile, and latching it with the
    // buffers would leave it null forever - fatal for a custom draw.
    EnsureCustomShaderNet();
    if (m_sceneBuffersReady) return true;

    // UPLOAD + persistently mapped, kFramesInFlight deep - no initialData, so
    // no mid-frame FlushUploadAndWait (the same rule EnsureNeutralInputs keeps).
    // A single buffer written every frame would be overwritten by frame N+1's
    // CPU while frame N's GPU still reads it.
    bool ok = true;
    for (uint32_t i = 0; i < RHI::kFramesInFlight && ok; ++i)
    {
        {
            RHI::GPUBufferDesc bd{};
            // kMaxPreviewParts rows: a packet's instanceOffset indexes this table, so a
            // multi-part asset needs a row per part; the single-material path writes row 0.
            bd.size       = sizeof(GPUInstanceData) * Preview::kMaxPreviewParts;
            bd.stride     = sizeof(GPUInstanceData);
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            ok = m_gfx.CreateBuffer(bd, m_instanceBuf[i]);
            if (ok) m_instanceMapped[i] = m_gfx.MapBuffer(m_instanceBuf[i]);
            ok = ok && m_instanceMapped[i] != nullptr;
        }
        if (ok)
        {
            // Sized with sizeof(Resource::MaterialGPUData), never a literal: the stride has
            // changed several times and ci_check only guards the HLSL side.
            RHI::GPUBufferDesc bd{};
            bd.size       = sizeof(Resource::MaterialGPUData) * kPreviewMatSlots;
            bd.stride     = sizeof(Resource::MaterialGPUData);
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            ok = m_gfx.CreateBuffer(bd, m_materialBuf[i]);
            if (ok) m_materialMapped[i] = m_gfx.MapBuffer(m_materialBuf[i]);
            ok = ok && m_materialMapped[i] != nullptr;
        }
        if (ok)
        {
            // SLOT 0 IS NOT PADDING and MaterialGPUData{} is not a legal record.
            // The engine pre-fills every slot of its own table with these exact
            // values (Renderer::CreateConstantBuffers) and a private table read
            // by the SAME shaders has to match: a zeroed textureHandleIds is a
            // VALID bindless index that samples an unrelated texture instead of
            // the white fallback, and a (0,0) uvTiling collapses every UV onto
            // one texel.
            auto* mats = static_cast<Resource::MaterialGPUData*>(m_materialMapped[i]);
            for (uint32_t s = 0; s < kPreviewMatSlots; ++s)
            {
                mats[s] = {};
                mats[s].roughnessMin = 0.0f;  mats[s].roughnessMax = 0.5f;
                mats[s].metalnessMin = 0.0f;  mats[s].metalnessMax = 0.0f;
                mats[s].reflectance  = 0.5f;
                mats[s].baseColor[0] = mats[s].baseColor[1] =
                mats[s].baseColor[2] = mats[s].baseColor[3] = 1.0f;
                mats[s].uvTilingOffset[0] = mats[s].uvTilingOffset[1] = 1.0f;
                mats[s].uvTilingOffset[2] = mats[s].uvTilingOffset[3] = 0.0f;
                for (auto& id : mats[s].textureHandleIds) id = -1;
            }
        }
    }

    if (ok) ok = m_perViewCB.Create(m_gfx, "PreviewRenderer.PerViewCB");
    if (ok) ok = m_lightCB.Create(m_gfx, "PreviewRenderer.LightCB");
    // Created zeroed (FrameCB::Create) and never written: "no fog".
    if (ok) ok = m_fogApplyNoneCB.Create(m_gfx, "PreviewRenderer.FogApplyNoneCB");
    // The shadow pass's own b1, in the same all-or-nothing latch: a partial failure
    // leaves m_sceneBuffersReady false and the preview declines to draw.
    if (ok) ok = m_shadowViewCB.Create(m_gfx, "PreviewRenderer.ShadowViewCB");
    // Lane 1's rings - the WorldCull set's own PerViewCB / LightCB: a FrameCB has one
    // slot per frame, so two pictures in one frame need two rings. Created in the same
    // all-or-nothing latch (2 x 3 x 256 B is not worth a second lazy path); a session
    // with no camera view pays 1.5 KiB of UPLOAD heap.
    if (ok) ok = m_worldPerViewCB.Create(m_gfx, "PreviewRenderer.WorldPerViewCB");
    if (ok) ok = m_worldLightCB.Create(m_gfx, "PreviewRenderer.WorldLightCB");

    if (!ok)
    {
        LOG_ERROR("[PREVIEW] private scene buffers failed to create - the preview "
                  "will not draw");
        return false;
    }
    m_sceneBuffersReady = true;
    LOG_INFO("[PREVIEW] private scene buffers ready (%u instance rows, %u material "
             "slots @ %zu B stride)", Preview::kMaxPreviewParts, kPreviewMatSlots,
             sizeof(Resource::MaterialGPUData));
    return true;
}

// EnsureClusterBuffers: the clustered-light triple (lights, index list, grid) for
// one lane. See the header's member comment for why the grid is uniform and why that
// is conservative rather than approximate.
bool PreviewRenderer::EnsureClusterBuffers(uint32_t lane)
{
    if (lane >= kLaneCount)    return false;
    if (m_clusterReady[lane])  return true;
    if (m_clusterFailed[lane]) return false;

    constexpr uint32_t kClusters = ClusterPass::kClusterCount;
    bool ok = true;
    for (uint32_t i = 0; i < RHI::kFramesInFlight && ok; ++i)
    {
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = sizeof(PreviewGPULight) * kMaxPreviewLights;
            bd.stride     = sizeof(PreviewGPULight);
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            ok = m_gfx.CreateBuffer(bd, m_clusterLightBuf[lane][i]);
            if (ok) m_clusterLightMapped[lane][i] = m_gfx.MapBuffer(m_clusterLightBuf[lane][i]);
            ok = ok && m_clusterLightMapped[lane][i] != nullptr;
        }
        if (ok)
        {
            // One entry per light: every cluster shares offset 0, so the index list is the
            // identity permutation of the light array (the main path's full list is 3.8 MiB).
            RHI::GPUBufferDesc bd{};
            bd.size       = sizeof(uint32_t) * kMaxPreviewLights;
            bd.stride     = sizeof(uint32_t);
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            ok = m_gfx.CreateBuffer(bd, m_clusterIndexBuf[lane][i]);
            if (ok) m_clusterIndexMapped[lane][i] = m_gfx.MapBuffer(m_clusterIndexBuf[lane][i]);
            ok = ok && m_clusterIndexMapped[lane][i] != nullptr;
        }
        if (ok)
        {
            // 31104 clusters x 8 B = 243 KiB per slot, 729 KiB per ring - the whole cost of the
            // clustered path here (the main pipeline's working set is ~8.3 MiB with its cluster
            // AABBs and full index list).
            RHI::GPUBufferDesc bd{};
            bd.size       = sizeof(PreviewLightGridEntry) * kClusters;
            bd.stride     = sizeof(PreviewLightGridEntry);
            // That size is per ring; the world lane owns four (one per view, see GridViews),
            // so 2.9 MiB.
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            // One grid ring per view on the world lane: the grid is memoised on its count, so a
            // shared buffer would be rewritten on every record by cells with different counts.
            for (uint32_t gv = 0; gv < GridViews(lane) && ok; ++gv)
            {
                ok = m_gfx.CreateBuffer(bd, m_clusterGridBuf[lane][gv][i]);
                if (ok) m_clusterGridMapped[lane][gv][i] = m_gfx.MapBuffer(m_clusterGridBuf[lane][gv][i]);
                ok = ok && m_clusterGridMapped[lane][gv][i] != nullptr;
            }
        }
        // Mark every grid slot unwritten (0xFFFFFFFF never equals a light count), so the
        // first write always happens. Every view's slot, so a lane whose GridViews() grows
        // does not inherit a value that reads as already written.
        for (uint32_t gv = 0; gv < Preview::kMaxPreviews; ++gv)
            m_clusterGridCount[lane][gv][i] = 0xFFFFFFFFu;
    }

    if (!ok)
    {
        // Hard latch, per lane: the loop can fail part way (slot 0 created and mapped,
        // slot 1 refused), and a retry next frame would overwrite slot 0's handles and leak
        // ~250 KiB per frame. Per lane so the warning names which died: lane 0 is the studio
        // fill and back, lane 1 the secondary view's scene lights.
        m_clusterFailed[lane] = true;
        LOG_WARNING("[PREVIEW] clustered-light buffers failed to create for lane "
                    "%u (%s); those lights are unavailable this session (the key "
                    "light / the sun and the IBL are unaffected).",
                    lane, lane == kLaneWorld ? "world scene lights"
                                             : "studio fill+back");
        return false;
    }
    m_clusterReady[lane] = true;
    return true;
}

uint32_t PreviewRenderer::WriteClusterLights(uint32_t lane, uint32_t frameSlot,
                                             const Preview::Request& req)
{
    // Not a create: the buffers are made in PrepareFrame's serial window
    // (PrepareTarget, beside EnsureSceneBuffers), and this render-half path must not
    // allocate. Not ready means PrepareFrame has not run yet or the allocation failed;
    // both render the key light plus the IBL.
    if (lane >= kLaneCount)                 return 0;
    if (!m_clusterReady[lane])              return 0;
    if (frameSlot >= RHI::kFramesInFlight)  return 0;
    if (!m_clusterLightMapped[lane][frameSlot] || !m_clusterIndexMapped[lane][frameSlot] ||
        !m_clusterGridMapped[lane][kStudioGridView][frameSlot]) return 0;

    // With scene lighting the rig is off: that toggle means "this material under my
    // scene", and the scene's point lights are not reachable here (no World, and the main
    // ClusterPass's SRVs are another graph's per-frame state). The studio fill and back
    // over the scene's sun would match neither, so it is the scene's sun + IBL alone.
    if (req.view.features & Preview::Feature::kSceneLighting) return 0;
    // kKeyLightOnly is the explicit off switch (the rollback lever and the control arm
    // of the back-light measurement; see the bit's comment).
    if (req.view.features & Preview::Feature::kKeyLightOnly)  return 0;

    auto* lights = static_cast<PreviewGPULight*>(m_clusterLightMapped[lane][frameSlot]);
    uint32_t n = 0;
    lights[n++] = MakeRigPointLight(Preview::kFillLight);
    lights[n++] = MakeRigPointLight(Preview::kBackLight);
    // The rig is two lights against Preview::kMaxPreviewLights, so this cannot fire
    // today; the cap is shared with the world lane, and a rig that outgrew it would write
    // past the end of a mapped UPLOAD buffer silently.
    ENGINE_CHECK_ONCE(n <= kMaxPreviewLights,
        "PreviewRenderer::WriteClusterLights: the studio rig produced %u lights "
        "against a buffer sized for %u.", n, kMaxPreviewLights);

    auto* idx = static_cast<uint32_t*>(m_clusterIndexMapped[lane][frameSlot]);
    for (uint32_t i = 0; i < n; ++i) idx[i] = i;

    // The uniform grid, rewritten only when the count changes: 243 KiB of identical
    // bytes per frame would dominate this path, and the preview renders on a dirty edge.
    // One grid ring on this lane (GridViews(kLaneLive) == 1): every panel gets the same
    // two-light rig, so nothing is per view; kStudioGridView keeps the indexing shape the
    // same on both lanes.
    if (m_clusterGridCount[lane][kStudioGridView][frameSlot] != n)
    {
        auto* grid = static_cast<PreviewLightGridEntry*>(
            m_clusterGridMapped[lane][kStudioGridView][frameSlot]);
        for (uint32_t c = 0; c < ClusterPass::kClusterCount; ++c)
        {
            grid[c].offset = 0;
            grid[c].count  = n;
        }
        m_clusterGridCount[lane][kStudioGridView][frameSlot] = n;
    }

    if (!m_clusterLoggedOnce)
    {
        m_clusterLoggedOnce = true;
        // The numbers a pack gates on: rig lights that reached the GPU and the cluster
        // count. "uniform" is in the text because the grid is not culled.
        LOG_INFO("[PREVIEW] studio rig clustered lights: %u point light(s), "
                 "uniform grid over %u clusters", n, ClusterPass::kClusterCount);
    }
    return n;
}

// WriteWorldClusterLights: the secondary view's scene lights, render half.
// The cull ran on the game half (Renderer::ExtractPreviewSceneLights), where
// m_frameLights lives and ExtractPreviewCameras' exact frustum is at hand; the light
// list is World-derived and the render half must never read World. Only the render
// half knows which WorldCull view got the lane's turn this frame, so it picks that
// view's cached set and copies it into the lane's UPLOAD slot (one write per frame;
// the per-view cull is views x lights x 6 dot products).
// Not a create, like WriteClusterLights. Every refusal returns 0: the scene's sun
// plus the IBL.
uint32_t PreviewRenderer::WriteWorldClusterLights(uint32_t frameSlot,
                                                  const Preview::Request& req)
{
    constexpr uint32_t lane = kLaneWorld;
    if (req.viewId >= Preview::kMaxPreviews) return 0;

    // What a pack reads, zeroed first: Preview.WorldClusterLights() must report 0 in
    // an OFF window and every refusal below returns early, so the report is cleared
    // before any of them and written only on success.
    m_viewLightBound[req.viewId].store(0, std::memory_order_relaxed);
    m_worldLightLastBound.store(0, std::memory_order_relaxed);

    if (!m_clusterReady[lane])              return 0;
    if (frameSlot >= RHI::kFramesInFlight)  return 0;
    if (!m_clusterLightMapped[lane][frameSlot] || !m_clusterIndexMapped[lane][frameSlot] ||
        !m_clusterGridMapped[lane][req.viewId][frameSlot]) return 0;

    // The one off switch: the process-wide runtime toggle the cost pack alternates
    // between its in-process ON and OFF windows (a boot flag cannot alternate windows).
    // Set from the game half only, inside the parked extract window - see
    // SetWorldSceneLightsEnabled and Renderer::ApplyPendingGamePiP.
    if (!m_worldSceneLightsEnabled) return 0;

    ++m_worldLightRenders;

    // The stale-push refusal. ProcessQueue incremented m_previewFrame this frame on
    // this half; the game half stamped m_previewFrame + 1 before that, so the two agree
    // exactly when this frame's game half published a set for this view. A mismatch (the
    // camera went invalid, the view was acquired between the halves, the push was
    // skipped) means last frame's set is the wrong answer: legitimate and silent. A
    // mismatch on every world record is a stamp off-by-one, which the shutdown summary
    // reports.
    if (m_viewLightStamp[req.viewId] != m_previewFrame)
    {
        ++m_worldLightStaleRefusals;
        return 0;
    }

    uint32_t n = m_viewLightCount[req.viewId];
    // The count is bounded by the buffer, checked before the memcpy. The game half
    // clamps too; this is the check that stands next to the pointer.
    if (!ENGINE_CHECK_ONCE(n <= kMaxPreviewLights,
            "PreviewRenderer::WriteWorldClusterLights: view %u pushed %u lights "
            "against a lane buffer sized for %u; clamping.",
            req.viewId, n, kMaxPreviewLights))
        n = kMaxPreviewLights;
    if (n == 0) return 0;

    auto* lights = static_cast<PreviewGPULight*>(m_clusterLightMapped[lane][frameSlot]);
    std::memcpy(lights, m_viewLights[req.viewId], sizeof(PreviewGPULight) * n);

    // The identity index list: every cluster names offset 0, count n.
    auto* idx = static_cast<uint32_t*>(m_clusterIndexMapped[lane][frameSlot]);
    for (uint32_t i = 0; i < n; ++i) idx[i] = i;

    // The uniform grid, memoised per (view, slot): its content is a pure function of
    // the count, compared inside this view's own ring. A ring shared by the lane would be
    // rewritten on every record by cells with different counts, and keying a shared
    // buffer on (view, count) would let one view skip a rewrite another view already
    // overwrote - binding a grid with the wrong light count.
    if (m_clusterGridCount[lane][req.viewId][frameSlot] != n)
    {
        auto* grid = static_cast<PreviewLightGridEntry*>(
            m_clusterGridMapped[lane][req.viewId][frameSlot]);
        for (uint32_t c = 0; c < ClusterPass::kClusterCount; ++c)
        {
            grid[c].offset = 0;
            grid[c].count  = n;
        }
        m_clusterGridCount[lane][req.viewId][frameSlot] = n;
        ++m_worldLightGridRewrites;
    }

    m_viewLightBound[req.viewId].store(n, std::memory_order_relaxed);
    m_worldLightLastBound.store(n, std::memory_order_relaxed);
    if (n < m_worldLightMinUsed) m_worldLightMinUsed = n;
    if (n > m_worldLightMaxUsed) m_worldLightMaxUsed = n;

    // One line per change, the feature's witness, name=value for logLineFields.
    // `shadowed=` is counted off the packed records (Renderer::ExtractPreviewSceneLights
    // tallies shadowSliceIdx != 0xFFFFFFFF), so the packs' `shadowed=0` is falsifiable: it
    // must stay 0 while the preview binds SetSpotShadowAtlas(0,0) /
    // SetPointShadowAtlas(0), since a real slice would index a 1x1 fallback per light per
    // pixel. `nonfinite=` is normally 0. Keep `shadowed=%u grid=uniform clusters=%u`
    // contiguous: a pack greps for that literal.
    if (!m_viewLightLogged[req.viewId] ||
        m_viewLightLoggedUsed[req.viewId]     != n ||
        m_viewLightLoggedDropped[req.viewId]  != m_viewLightDropped[req.viewId] ||
        m_viewLightLoggedShadowed[req.viewId] != m_viewLightShadowed[req.viewId] ||
        m_viewLightLoggedNonFin[req.viewId]   != m_viewLightNonFinite[req.viewId])
    {
        m_viewLightLogged[req.viewId]         = true;
        m_viewLightLoggedUsed[req.viewId]     = n;
        m_viewLightLoggedDropped[req.viewId]  = m_viewLightDropped[req.viewId];
        m_viewLightLoggedShadowed[req.viewId] = m_viewLightShadowed[req.viewId];
        m_viewLightLoggedNonFin[req.viewId]   = m_viewLightNonFinite[req.viewId];
        LOG_INFO("[PREVIEW] world clustered lights: view=%u used=%u scene=%u "
                 "culled=%u dropped=%u nonfinite=%u cap=%u shadowed=%u "
                 "grid=uniform clusters=%u",
                 req.viewId, n, m_viewLightScene[req.viewId],
                 m_viewLightCulled[req.viewId], m_viewLightDropped[req.viewId],
                 m_viewLightNonFinite[req.viewId], kMaxPreviewLights,
                 m_viewLightShadowed[req.viewId], ClusterPass::kClusterCount);
    }
    // Session scope for the shutdown summary, over records that bound lights; a
    // refusal above contributes nothing.
    if (m_viewLightShadowed[req.viewId] > m_worldLightShadowedMax)
        m_worldLightShadowedMax = m_viewLightShadowed[req.viewId];
    m_worldLightNonFiniteTot += m_viewLightNonFinite[req.viewId];
    return n;
}

void PreviewRenderer::SetViewSceneLights(uint32_t viewId,
                                         const Preview::GPULight* lights,
                                         uint32_t n, uint32_t scene,
                                         uint32_t culled, uint32_t dropped,
                                         uint32_t shadowed, uint32_t nonFinite)
{
    if (viewId >= Preview::kMaxPreviews) return;
    if (n > kMaxPreviewLights) n = kMaxPreviewLights;
    if (n && lights)
        std::memcpy(m_viewLights[viewId], lights, sizeof(Preview::GPULight) * n);
    m_viewLightCount[viewId]   = n;
    m_viewLightScene[viewId]   = scene;
    m_viewLightCulled[viewId]  = culled;
    m_viewLightDropped[viewId] = dropped;
    // Clamped the same way as `n`: `shadowed` counts surviving records, so a larger
    // value is a caller bug the log would report as more shadowed lights than lights.
    m_viewLightShadowed[viewId]  = (shadowed > n) ? n : shadowed;
    m_viewLightNonFinite[viewId] = nonFinite;
    // m_previewFrame + 1: ProcessQueue increments m_previewFrame on the render half of
    // this same frame F, so it still names F-1 here, and WriteWorldClusterLights compares
    // against the incremented value. The same correction as the tick regulator in
    // ExtractOneRequest.
    m_viewLightStamp[viewId] = m_previewFrame + 1;
}

uint32_t PreviewRenderer::LiveWorldCullViewCount() const
{
    uint32_t n = 0;
    for (uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
        if (IsWorldCullView(v)) ++n;
    return n;
}

uint32_t PreviewRenderer::LastWorldClusterLights(uint32_t viewId) const
{
    // kInvalidView (or anything out of range) = whichever world view recorded last,
    // the convention Preview.WorldCullDraws uses. Last rather than max: a max over the
    // per-view array would report an OFF window as on for every view but the one that
    // just refused.
    if (viewId >= Preview::kMaxPreviews)
        return m_worldLightLastBound.load(std::memory_order_relaxed);
    return m_viewLightBound[viewId].load(std::memory_order_relaxed);
}

// EnsureCheckerCube: the checker backdrop (Background::Checker) as a cubemap, not
// a fullscreen pass. SkyboxPass#2 already draws a cube behind the subject with the
// right depth test, so a checker TextureCube makes "checkerboard" and "skybox" one
// code path with a different SRV - no new shader, PSO or pass.
void PreviewRenderer::EnsureCheckerCube()
{
    if (m_checkerTried) return;
    m_checkerTried = true;

    constexpr uint32_t kFace = 8;
    // Two mid-greys rather than black/white: the backdrop exists to make a
    // TRANSLUCENT subject's coverage readable, and a full-contrast board reads
    // as the picture instead of as the background. SkyboxPass's trilinear
    // sampler blurs an 8x8 face across the frame, so this arrives as a soft
    // gradient checker rather than as eight hard squares.
    const uint32_t kA = 0xFF6E6E6Eu;   // ABGR8 (R8G8B8A8_UNORM little-endian)
    const uint32_t kB = 0xFF3A3A3Au;
    uint32_t face[kFace * kFace];
    for (uint32_t y = 0; y < kFace; ++y)
        for (uint32_t x = 0; x < kFace; ++x)
            face[y * kFace + x] = (((x ^ y) & 1u) != 0u) ? kA : kB;

    RHI::TextureDesc td;
    td.width      = kFace;
    td.height     = kFace;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.mipLevels = 1;
    td.arraySize = 6;
    td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
    td.debugName = "PreviewCheckerCube";

    RHI::SubresourceData init[6];
    for (uint32_t f = 0; f < 6; ++f)
        init[f] = RHI::SubresourceData{ face, kFace * 4u, kFace * kFace * 4u };

    if (m_gfx.CreateTexture(td, m_checkerCube, init))
    {
        m_checkerCubeSrv = m_gfx.GetTextureSRVGpuHandle(m_checkerCube);
        LOG_INFO("[PREVIEW] checker backdrop cube ready (%ux%u x6)", kFace, kFace);
    }
    else
    {
        LOG_WARNING("[PREVIEW] checker backdrop cube creation failed; "
                    "Background::Checker falls back to the solid clear colour.");
    }
}

namespace
{
    // ViewDesc::viewMode: does it hide the backdrop? The main viewport's rule
    // (Renderer::Render_BindFrameResources, minus its ortho term - the studio
    // camera is always perspective).
    bool PreviewHidesBackdrop(uint32_t viewMode)
    {
        using VM = Renderer::ViewMode;
        if (viewMode >= static_cast<uint32_t>(VM::Count)) return false;
        const VM m = static_cast<VM>(viewMode);
        return m == VM::Wireframe || Renderer::IsLightingDebugView(m) ||
               Renderer::IsGBufferDebugView(m);
    }
}

// ApplyPreviewView: what this frame's preview view shows (RG::SceneView). Render
// half, immediately before Execute. The optional passes declare a ShowFlag - the
// studio shadow Shadows, the backdrop Sky, the forward pass Transparent - and SetView
// hides the ones this request does not want: a hidden pass costs no command list,
// barrier or PIX region, which its own early-out would still pay.
//   - Producer-complete: every hideable pass is a pure consumer or writes only its
//     own target. The shadow pass writes its own depth array, and LightingPass gets
//     handle 0 (its fully-lit 1x1 fallback) when it is off; SkyboxPass writes HDR but
//     GBufferPass declares (and clears) it; TransparentPass produces nothing anyone
//     reads. GBufferPass and LightingPass declare no flag and always run, so the
//     HdrSceneColor clear stays in the frame. A view with nothing to draw never
//     reaches Execute (a size-refused view is suspended).
//   - Sticky: written in full every frame before Execute, never reset on an
//     early-out.
void PreviewRenderer::ApplyPreviewView(GraphSet& set, const Preview::Request& req,
                                       bool wantForward)
{
    if (set.passCount == 0) return;

    RG::SceneView view;
    view.name     = (&set == &m_thumb) ? "thumbnail" : ((&set == &m_world) ? "world" : "preview");
    view.viewMode = static_cast<uint8_t>(req.view.viewMode);
    view.width    = set.w;
    view.height   = set.h;
    const bool wantSky = (req.view.features & Preview::Feature::kSkybox) != 0
                      && set.skybox != nullptr;

    // The WorldCull set answers differently: the studio shadow never runs (a secondary
    // view borrows the main CSM); Transparent always runs (the world's draw list carries
    // per-packet DrawFilters, and a scene with no translucent geometry just gives the pass
    // nothing to draw); the sky runs whatever the view mode, because a PiP without its sky
    // reads as broken.
    if (set.source == Preview::Source::WorldCull)
    {
        view.show.Hide(RG::ShowFlag::Shadows);
        view.show.Show(RG::ShowFlag::Sky, wantSky);
        view.show.Show(RG::ShowFlag::Transparent, set.transparent != nullptr);
        set.graph.SetView(view);
        return;
    }

    // m_studioShadowValid is part of the test: RecordRequest clears it when
    // WriteStudioShadowView could not publish this frame's light VP, and
    // PreviewStudioShadowPass::Execute then returns - but only after the graph opened a
    // command list, bound its target, emitted barriers and opened a PIX region. Hiding it
    // here makes a failed VP cost nothing.
    const bool wantShadow = (req.view.features & Preview::Feature::kStudioShadow) != 0
                         && set.shadow != nullptr
                         && m_studioShadowValid
                         && set.shadow->GetShadowArrayGpuHandle() != 0;

    view.show.Show(RG::ShowFlag::Shadows, wantShadow);
    // The backdrop also hides for every mode but Lit / Unlit - the main
    // viewport's rule (Render_BindFrameResources): a wire or a GBuffer channel
    // reads against a flat background, not the backdrop.
    view.show.Show(RG::ShowFlag::Sky, wantSky && !PreviewHidesBackdrop(req.view.viewMode));
    view.show.Show(RG::ShowFlag::Transparent, wantForward);
    set.graph.SetView(view);
}

// The custom-shader safety net (see the members' comment in the header).
// Built EAGERLY, not on the first custom material, because "the first custom
// material" is exactly the frame on which getting it wrong removes the device.
void PreviewRenderer::EnsureCustomShaderNet()
{
    if (!m_customCbv[0].IsValid())
    {
        bool ok = true;
        for (uint32_t i = 0; i < RHI::kFramesInFlight && ok; ++i)
        {
            RHI::GPUBufferDesc bd{};
            // MaterialCBVStore::kSlotStride, not a pinned 256: the budget the main packer
            // treats as the normal b8 size (larger falls back to its per-frame ring), taken from
            // the same place. WriteCustomParamCBV reports a reflected layout that does not fit
            // instead of writing past it. kMaxPreviewParts slices: each packet binds root 35 at
            // its own slice, because a prefab's parts do not share a b8 layout and would otherwise
            // all read whichever was packed last.
            bd.size       = MaterialCBVStore::kSlotStride * Preview::kMaxPreviewParts;
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
            ok = m_gfx.CreateBuffer(bd, m_customCbv[i]);
            if (ok) m_customCbvMapped[i] = m_gfx.MapBuffer(m_customCbv[i]);
            ok = ok && m_customCbvMapped[i] != nullptr;
            if (ok)
            {
                // UPLOAD memory arrives uninitialised and every //@ parameter of a custom shader
                // reads this block, so zero it. Persistently mapped: WriteCustomParamCBV rewrites a
                // slice per recorded frame.
                std::memset(m_customCbvMapped[i], 0,
                            MaterialCBVStore::kSlotStride * Preview::kMaxPreviewParts);
                m_customCbvVA[i] = m_gfx.GetBufferGPUAddress(m_customCbv[i]);
            }
        }
        if (!ok || !m_customCbvVA[0])
            LOG_WARNING("[PREVIEW] zero-filled b8 CBV ring unavailable - custom-shader "
                        "materials will be skipped rather than drawn unbound");
    }
    // Needs the preview GBufferPass (the 1x1 default-white PrefillSpace3White stamps
    // with), so it is retried rather than latched: it first succeeds the frame after
    // ServiceCompile built a graph. A graph teardown does not come back here: the block
    // outlives every GraphSet, and the slots a draw binds are re-stamped from the live
    // pass by PrefillSpace3White right before the packer writes them, per (frameSlot,
    // part), on every custom draw (see the header). Once the pass exists a failure
    // repeats identically, so it is latched off (m_customTexTableFailed).
    if (m_customTexTableGpu == 0 && AnyGBuffer() && !m_customTexTableFailed)
    {
        // kFramesInFlight x kMaxPreviewParts blocks of eight. The packer writes each dirty
        // frame's real space3 textures into them, so a shared block would rewrite descriptors
        // an in-flight frame samples (the main packer's MaterialSRVRing rule); per part
        // because root 36 is a per-draw table and two custom materials of one asset must not
        // overwrite each other's slots. One contiguous static range indexed through
        // Space3BlockOffset(frameSlot, part), because a shader-visible descriptor table must
        // be contiguous. Allocated once per session.
        constexpr uint32_t kBlock = GraphicsDX12::kCustomMatTextureSlots;
        constexpr uint32_t kTotal =
            kBlock * RHI::kFramesInFlight * Preview::kMaxPreviewParts;
        // The block is kept: the record half needs only the handles, but
        // ~PreviewRenderer gives the allocation (1536 shader-visible descriptors) back.
        RHI::DescriptorBlock block;
        m_gfx.AllocateDescriptorBlock(kTotal, block);
        // The default-white is a readiness predicate, not a fill source: the only reader
        // of these descriptors is a custom draw, and PrefillSpace3White stamps that draw's
        // eight slots from the live pass on the frame it is recorded. Without a default-white
        // the per-draw prefill cannot work either, so the net is not ready.
        const uint64_t src = m_gfx.GetTextureSRVCpuHandle(AnyGBuffer()->GetDefaultWhiteTexture());
        if (block.IsValid() && src)
        {
            m_customTexTable    = block;
            m_customTexTableGpu = block.gpuBase;
            m_customTexTableCpu = block.cpuBase;
            m_customTexTableInc = block.increment;
            LOG_INFO("[PREVIEW] custom-shader net ready (b8 CBV ring + %u x %u x %u "
                     "space3 slots; PrefillSpace3White stamps the eight a draw "
                     "binds, M2.3 packs them)",
                     RHI::kFramesInFlight, Preview::kMaxPreviewParts, kBlock);
        }
        else
        {
            // Hand the block back before latching: AllocateDescriptorBlock can succeed while
            // the source SRV is null, and abandoning the allocation leaks it.
            if (block.IsValid()) m_gfx.ReleaseDescriptorBlock(block, /*quiesced=*/true);
            m_customTexTableFailed = true;
            // ENGINE_CHECK_ONCE, so the report is one line; the latch above is
            // what actually stops the retry.
            (void)ENGINE_CHECK_ONCE(false,
                "[PREVIEW] space3 fallback table unavailable - custom-shader "
                "materials will be SKIPPED rather than drawn with an unbound "
                "root 36 (T6: that is a device hang, not a black texture)");
        }
    }
}

// Byte offset of one part's eight-descriptor space3 block inside one frame slot.
// Parts are the inner axis, so a frame slot's blocks are contiguous.
uint64_t PreviewRenderer::Space3BlockOffset(uint32_t frameSlot, uint32_t part) const
{
    const uint64_t index =
        static_cast<uint64_t>(frameSlot) * Preview::kMaxPreviewParts + part;
    return index * GraphicsDX12::kCustomMatTextureSlots * m_customTexTableInc;
}

// Byte offset of one part's kSlotStride (256 B) b8 slice inside a frame slot's
// buffer.
uint64_t PreviewRenderer::CustomCbvSliceOffset(uint32_t part) const
{
    return static_cast<uint64_t>(part) * MaterialCBVStore::kSlotStride;
}

// The space3 floor, kept here because the packer writes only the slots a
// reflection declares. This ring slot last held another material's textures: switch
// from a material binding t0..t6 to one binding only t0 and t1..t6 keep descriptors
// the deferred release queue may already have handed back. Sampling a freed
// descriptor on a custom draw is a DEVICE_HUNG with no page fault and no debug-layer
// message.
void PreviewRenderer::PrefillSpace3White(uint32_t frameSlot, uint32_t part)
{
    if (!m_customTexTableCpu || !m_customTexTableInc || !AnyGBuffer()) return;
    if (part >= Preview::kMaxPreviewParts) return;
    const uint64_t src = m_gfx.GetTextureSRVCpuHandle(AnyGBuffer()->GetDefaultWhiteTexture());
    if (!src) return;
    // Only this part's eight slots: filling all kMaxPreviewParts blocks would be 512
    // descriptor copies per frame for blocks nothing is about to bind.
    const uint64_t dst0 = m_customTexTableCpu + Space3BlockOffset(frameSlot, part);
    for (uint32_t s = 0; s < GraphicsDX12::kCustomMatTextureSlots; ++s)
        m_gfx.CopyCbvSrvUavDescriptors(dst0 + s * m_customTexTableInc, src, 1);
}

void PreviewRenderer::EnsureNeutralInputs()
{
    if (m_neutralReady) return;

    // No initialData anywhere in this function: it runs from PrepareFrame, inside
    // BeginFrame..EndFrame, and the DEFAULT + initialData path of CreateTexture and
    // CreateBuffer ends in FlushUploadAndWait - a CPU-blocking device sync mid-frame, on
    // the first frame a panel opens.
    if (!m_neutralBlack.IsValid())
    {
        // A RENDER_TARGET, created empty. Its contents are defined once on the preview
        // command list (RecordRequest, m_neutralBlackCleared) - one clear on a list we
        // already own instead of a staging buffer, a copy and a fence wait. The optimised
        // clear value defaults to {0,0,0,0}, the colour it is cleared to, so there is no
        // MISMATCHINGCLEARVALUE warning either.
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bindFlags = RHI::BindFlag::RENDER_TARGET | RHI::BindFlag::SHADER_RESOURCE;
        td.debugName = "PreviewRenderer.NeutralBlack";
        if (m_gfx.CreateTexture(td, m_neutralBlack, nullptr))
            m_neutralBlackSrv = m_gfx.GetTextureSRVGpuHandle(m_neutralBlack);
        else
            LOG_ERROR("[PREVIEW] neutral black texture creation failed");
    }
    if (!m_neutralExposure.IsValid())
    {
        // An UPLOAD structured buffer, persistently mapped and written from the
        // CPU. Binding an UPLOAD buffer as a StructuredBuffer SRV is the shipped
        // shape here, not a new trick: BeamSystem's control points and
        // SkinningBuffers' pose ring do exactly this (UPLOAD + SHADER_RESOURCE +
        // MapBuffer + GetBufferSRVGpuHandle). An UPLOAD resource is created in
        // GENERIC_READ, which already includes both shader-resource states, so
        // there is no barrier and no copy - and therefore no flush.
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(float);
        bd.stride     = sizeof(float);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (m_gfx.CreateBuffer(bd, m_neutralExposure, nullptr))
        {
            m_neutralExposureMapped = m_gfx.MapBuffer(m_neutralExposure);
            if (m_neutralExposureMapped)
            {
                const float one = 1.0f;
                std::memcpy(m_neutralExposureMapped, &one, sizeof(one));
                m_neutralExposureSrv = m_gfx.GetBufferSRVGpuHandle(m_neutralExposure);
            }
            else
            {
                // Handle deliberately left with a 0 SRV: an unmapped buffer has
                // UNDEFINED contents, and a tone map fed an undefined exposure
                // is a wrong picture rather than a missing one.
                LOG_ERROR("[PREVIEW] neutral exposure buffer map failed - preview disabled");
            }
        }
        else
        {
            LOG_ERROR("[PREVIEW] neutral exposure buffer creation failed");
        }
    }

    // The per-frame-slot scene-exposure ring: the neutral buffer's shape (UPLOAD,
    // persistently mapped, StructuredBuffer SRV - no barrier, copy or flush), one per
    // frame in flight because the value changes every frame. A failure leaves
    // m_worldExposureReady false and the world view tone-maps at the neutral 1.0 (a wrong
    // brightness, not a wrong picture).
    if (!m_worldExposureReady && !m_worldExposure[0].IsValid())
    {
        uint32_t made = 0;
        for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
        {
            RHI::GPUBufferDesc bd{};
            bd.size       = sizeof(float);
            bd.stride     = sizeof(float);
            bd.usage      = RHI::Usage::UPLOAD;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            if (!m_gfx.CreateBuffer(bd, m_worldExposure[i], nullptr)) break;
            m_worldExposureMapped[i] = m_gfx.MapBuffer(m_worldExposure[i]);
            if (!m_worldExposureMapped[i]) break;
            const float one = 1.0f;
            std::memcpy(m_worldExposureMapped[i], &one, sizeof(one));
            m_worldExposureSrv[i] = m_gfx.GetBufferSRVGpuHandle(m_worldExposure[i]);
            if (!m_worldExposureSrv[i]) break;
            ++made;
        }
        m_worldExposureReady = (made == RHI::kFramesInFlight);
        if (!m_worldExposureReady)
            LOG_WARNING("[PREVIEW] scene-exposure ring unavailable (%u/%u slots) - "
                        "a WorldCull view will tone-map at a neutral 1.0 exposure "
                        "instead of tracking the viewport", made,
                        RHI::kFramesInFlight);
    }

    // Ready only when both SRVs exist: with a 0 handle SetComputeDescriptorTable
    // leaves the root slot alone, and the tone-map dispatch would read whatever it last
    // held.
    m_neutralReady = (m_neutralBlackSrv != 0) && (m_neutralExposureSrv != 0);
    if (m_neutralReady)
        LOG_INFO("[PREVIEW] neutral tone-map inputs ready (no mid-frame upload flush)");
}

uint32_t PreviewRenderer::ClaimHdrSlot()
{
    // Slot 0 belongs to the main viewport for the life of the process, so
    // the search starts at 1; the slots from kFirstViewHdrSlot on are the
    // secondary views'.
    for (uint32_t s = 1; s < GraphicsDX12::kFirstViewHdrSlot; ++s)
        if ((m_hdrSlotsUsed & (1u << s)) == 0)
        {
            m_hdrSlotsUsed |= (1u << s);
            m_hdrSlotsPeak |= m_hdrSlotsUsed;   // see the member
            return s;
        }
    ENGINE_CHECK_ONCE(false,
        "PreviewRenderer: no free device HDR slot (kMaxHdrTargets=%u, "
        "slot 0 is the main viewport). The preview will not render.",
        GraphicsDX12::kMaxHdrTargets);
    return 0;
}

void PreviewRenderer::PrepareFrame()
{
    // Drain the readback ring first and outside every guard: this is the device-frame
    // thread with this frame's index current, the one place the retired-slot rule holds,
    // and the last thumbnails of a burst are recorded on frames where the queue then
    // empties, so behind a guard their pixels would never reach the disk encoder.
    ServiceThumbReadback();

    // The thumbnail target, on the same terms as the live one and with the same
    // ordering: created here, in the serial window, never in an ImGui handler
    // and never inside a graph Execute (rule 3).
    if (m_thumbReq.valid && m_thumbReq.inFlight)
    {
        PrepareTarget(m_thumbTarget, Preview::kMaxPreviews,
                      Preview::kThumbnailSize, Preview::kThumbnailSize);
        EnsureThumbReadback();
    }

    // Every live view with a hand-over in flight.
    for (uint32_t vi = 0; vi < Preview::kMaxPreviews; ++vi)
    {
        Target& t = m_targets[vi];
        if (!t.live || !t.req.valid || !t.req.inFlight) continue;

        // The size ServiceCompile built the graph at (ResolveRequestedSize).
        uint32_t w = 0, h = 0;
        if (!ResolveRequestedSize(t, w, h)) continue;
        PrepareTarget(t, vi, w, h);
    }
}

// The device half of one target. `viewId` is only used to name the ToneMapPass
// instance; Preview::kMaxPreviews (one past the last live id) is the thumbnail
// lane's, which is why it is not a valid index into m_targets.
void PreviewRenderer::PrepareTarget(Target& target, uint32_t viewId,
                                    uint32_t w, uint32_t h)
{
    Target* t = &target;

    if (t->pendingCreate || t->hdrSlot == 0)
    {
        const uint32_t slot = ClaimHdrSlot();
        if (slot == 0) return;
        t->hdrSlot      = slot;
        t->pendingCreate = false;
        t->w = t->h = 0;   // force the create below
    }

    if (t->w != w || t->h != h)
    {
        // CreateHdrTarget destroys the slot's previous contents through the
        // DEFERRED release bucket, so calling it mid-frame is safe (see its
        // comment) - and this is the serial window regardless, which is what
        // keeps CreateTexture's descriptor-heap allocation off a graph worker.
        m_gfx.CreateHdrTarget(t->hdrSlot, w, h);
        t->w = w;
        t->h = h;
        t->hasOutput.store(false, std::memory_order_relaxed);
    }

    EnsureNeutralInputs();
    // Same window, same reason: CreateBuffer + MapBuffer on the device-frame
    // thread, before any graph worker exists, and with no initialData so the
    // UPLOAD path never reaches FlushUploadAndWait mid-frame.
    EnsureSceneBuffers();
    // The clustered-light triple, created here and not where it is used:
    // WriteClusterLights runs inside RecordRequest on the render half, and buffer and
    // descriptor creation on a graph worker mid-frame is forbidden (header rule 3). Its
    // m_clusterReady guard renders key light + IBL until this has run.
    // Only this target's lane: the rings (~732 KiB each) stay until ~PreviewRenderer (the
    // idle sweep destroys GraphSets, not these), so a session with only a material
    // inspector must not pay for a WorldCull ring. The thumbnail lane (viewId ==
    // kMaxPreviews) shares m_live's lane 0, hence the range test in the expression.
    EnsureClusterBuffers(
        (viewId < Preview::kMaxPreviews && IsWorldCullSlot(viewId)) ? kLaneWorld
                                                                   : kLaneLive);

    if (!t->toneMap)
    {
        t->toneMap = std::make_unique<ToneMapPass>();
        // Unique name before Init: GetGPUTimestampResult is a first-match-wins name scan
        // and PIX markers / timestamp regions key off the same string, so two instances
        // sharing "ToneMapPass" would report the union of both.
        const std::string suffix = (viewId >= Preview::kMaxPreviews)
            ? std::string("PreviewThumb")
            : ("Preview" + std::to_string(viewId));
        t->toneMap->SetNameSuffix(suffix.c_str());
    }
    if (!t->toneMapInited)
    {
        t->toneMap->Init(m_gfx);
        t->toneMapInited = true;
    }

    // Main-thread-only setters, pushed here so the size-only safety net inside
    // ToneMapPass::Execute never fires. The output mode is read from the device, not
    // pinned to SDR: in an HDR output mode ImGui composes into an extended-gamma FP16
    // buffer. The mode is compared, not just pushed: EnsureTexture also rebuilds on a mode
    // change, leaving a new texture in RENDERTARGET state with undefined contents, so
    // hasOutput must drop or the next ImGui frame samples it.
    ISurface& surface = m_gfx.MainSurface();
    const HDROutputMode modeBefore = t->toneMap->GetOutputMode();
    t->toneMap->SetOutputMode(surface.GetHDROutputMode(), surface.GetPaperWhiteNits(), surface.GetPeakNits());
    if (t->toneMap->GetOutputMode() != modeBefore)
        t->hasOutput.store(false, std::memory_order_relaxed);
    t->toneMap->EnsureTexture(w, h);
}

// ==== Render half helpers ====
// Everything below reads the request snapshot and the values the game half pushed
// in; nothing reaches into World or editor state.
namespace
{
    using namespace DirectX;

    // The studio framing, resolved against the request's orbit deltas.
    struct PreviewCamera
    {
        XMVECTOR eye;
        XMVECTOR forward;
        XMMATRIX view;
        XMMATRIX viewProj;
        float    nearZ, farZ;
    };

    PreviewCamera BuildPreviewCamera(const Preview::ViewDesc& v)
    {
        const Preview::StudioCamera& sc = Preview::kStudioCamera;
        // One definition of the basis (PreviewStudioRig.h), shared with the parity fixture
        // that drives the main pipeline's camera from the same call; the clamps (pitch short
        // of the poles, a distance floor) live there too.
        float e[3], f[3];
        // Mesh Editor pan (ViewDesc::pan). nullptr when unpanned, so every other caller
        // gets the unpanned basis unchanged.
        const bool panned = v.pan[0] != 0.f || v.pan[1] != 0.f || v.pan[2] != 0.f;
        Preview::StudioCameraBasis(v.yawOffset, v.pitchOffset, v.distanceScale, e, f,
                                   panned ? v.pan : nullptr);
        const XMVECTOR forward = XMVectorSet(f[0], f[1], f[2], 0.f);
        const XMVECTOR eye     = XMVectorSet(e[0], e[1], e[2], 0.f);

        PreviewCamera c{};
        c.eye     = eye;
        c.forward = forward;
        c.view    = XMMatrixLookToLH(eye, forward, XMVectorSet(0.f, 1.f, 0.f, 0.f));
        c.nearZ   = sc.nearZ;
        c.farZ    = sc.farZ;
        // REVERSED-Z, exactly as Renderer::BuildCameraViewProj builds it: near
        // and far are SWAPPED and the depth test is GREATER_EQUAL. Passing them
        // the ordinary way round here would put the sphere behind the far plane
        // and the preview would be empty with nothing to explain it.
        const float aspect = (v.h != 0) ? static_cast<float>(v.w) / static_cast<float>(v.h)
                                        : 1.0f;
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(sc.fovY, aspect, c.farZ, c.nearZ);
        c.viewProj = XMMatrixMultiply(c.view, proj);
        return c;
    }

    // Is the scene's published lighting usable for the preview? The test is the IBL
    // pair, not the sun: a scene may have no directional light (an interior lit by
    // probes), but one with no sky publishes zeroes for both cubes - a None/FlatColor
    // WorldEnvironment, or any scene before its .itex cubes stream in. Falling back to
    // the studio rig there keeps the toggle from turning the preview black.
    bool SceneLightingUsable(const PreviewRenderer::SceneLighting& s)
    {
        return s.irradiance != 0 && s.radiance != 0 && s.radianceMips != 0;
    }
}

// WriteStudioShadowView: the key light's reversed-Z ortho VP. Mirrors the
// per-object shadow fit in Renderer.cpp - LookToLH along the travel direction,
// XMMatrixOrthographicLH with near and far swapped - because shadow.hlsli's receiver
// sampling is written against that construction; a second convention would offset
// the shadow from its caster silently.
bool PreviewRenderer::WriteStudioShadowView(const Preview::Request& req)
{
    m_studioShadowValid = false;
    if (!m_shadowViewCB.IsValid()) return false;
    auto* cb = m_shadowViewCB.Current(m_gfx);
    if (!cb) return false;

    // The key light, always - the studio rig's, or the scene's sun under
    // Feature::kSceneLighting. WriteLightCB writes LightCB.lightDir from the same
    // choice, and both read this one expression.
    const bool sceneLit = (req.view.features & Preview::Feature::kSceneLighting) != 0
                       && SceneLightingUsable(m_sceneLighting);
    // KeyLightDir() is the shipped constant unless a harness run moved it. The same
    // expression WriteLightCB reads: the shadow view and LightCB.lightDir must agree, or
    // the map is cast from one direction and sampled from another.
    const float* dirSrc = sceneLit ? m_sceneLighting.sunDir : KeyLightDir();
    XMVECTOR lightDirV = XMVectorSet(dirSrc[0], dirSrc[1], dirSrc[2], 0.f);
    if (XMVectorGetX(XMVector3LengthSq(lightDirV)) < 1e-8f) return false;
    const XMVECTOR lightDir = XMVector3Normalize(lightDirV);

    XMFLOAT3 L; XMStoreFloat3(&L, lightDir);
    // World up is degenerate against a straight-down sun; the per-object fit
    // uses the same escape.
    XMVECTOR up = (std::fabs(L.y) > 0.999f) ? XMVectorSet(1.f, 0.f, 0.f, 0.f)
                                            : XMVectorSet(0.f, 1.f, 0.f, 0.f);

    const float fitR    = Preview::kStudioShadowFitRadius;
    const float pullback = fitR;
    // Eye BEHIND the subject along the light, looking ALONG the travel
    // direction. LookToLH rather than LookAtLH: the latter's (focus - eye)
    // subtraction cancels for a subject at the origin.
    const XMVECTOR eye = XMVectorScale(lightDir, -(fitR + pullback));
    const XMMATRIX view = XMMatrixLookToLH(eye, lightDir, up);
    const XMMATRIX proj = XMMatrixOrthographicLH(2.0f * fitR, 2.0f * fitR,
                                                 2.0f * fitR + pullback * 2.0f,
                                                 0.0f);   // reversed Z
    const XMMATRIX vp = XMMatrixMultiply(view, proj);

    // The CB the pass binds at b1: a full PerViewCB, zeroed first, because
    // GBuffer.vs.hlsl reads the tail rows too - byte 228 is gInstanceRemapIdx and 0 means
    // identity (preview packets index the instance table directly); stale contents would
    // put a remap heap index there.
    *cb = {};
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->viewProj), XMMatrixTranspose(vp));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->prevViewProj), XMMatrixTranspose(vp));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->curViewProjNoJitter),
                    XMMatrixTranspose(vp));
    cb->timeParams[3]    = 1.f;      // cos(0); time is pinned, as in WritePerViewCB
    // Byte 208 is the panel camera, as in WritePerViewCB. Only a masked custom
    // material's hook draw reads it here (the SHADOW_HOOK variant's p.viewDirWS, through
    // ShadowPerViewCB's same offset); the game's shadow passes give the hook the main
    // camera for the same reason - the coverage a view-dependent hook computes must be
    // the one the GBuffer drew. GBuffer_VS and Shadow_VS declare the slot and never read
    // it.
    {
        XMFLOAT3 eye;
        XMStoreFloat3(&eye, BuildPreviewCamera(req.view).eye);
        cb->cameraPosWS[0] = eye.x;
        cb->cameraPosWS[1] = eye.y;
        cb->cameraPosWS[2] = eye.z;
    }
    cb->dissolveNoiseIdx = ~0u;
    cb->instanceRemapIdx = 0;
    cb->frameIndex       = 0;

    XMFLOAT4X4 vpT;
    XMStoreFloat4x4(&vpT, XMMatrixTranspose(vp));
    std::memcpy(m_studioShadowMatrix, &vpT, sizeof(float) * 16);
    m_studioShadowTexelWorld =
        (2.0f * fitR) / static_cast<float>(PreviewStudioShadowPass::kSize);
    m_studioShadowValid = true;
    return true;
}

void PreviewRenderer::WritePerViewCB(GraphSet& set, const Preview::Request& req)
{
    auto* cb = set.perViewCB->Current(m_gfx);
    if (!cb) return;
    *cb = {};

    const PreviewCamera cam = BuildPreviewCamera(req.view);

    // TRANSPOSED for HLSL's row-vector mul(pos, matrix), like every other
    // producer of this struct.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->viewProj),
                    XMMatrixTranspose(cam.viewProj));
    // prev == cur: the preview camera does not move within a frame and there is
    // no temporal consumer here, so velocity is exactly zero rather than
    // whatever last frame's matrix happened to be.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->prevViewProj),
                    XMMatrixTranspose(cam.viewProj));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->curViewProjNoJitter),
                    XMMatrixTranspose(cam.viewProj));

    // Time is pinned to zero: a custom vertex shader swaying with timeParams would
    // make the preview and the cached thumbnail non-deterministic (the same reason the
    // preview does not borrow the scene's time-of-day).
    cb->timeParams[0] = cb->timeParams[1] = 0.f;
    cb->timeParams[2] = 0.f;   // sin(0)
    cb->timeParams[3] = 1.f;   // cos(0)

    XMFLOAT3 eye; XMStoreFloat3(&eye, cam.eye);
    cb->cameraPosWS[0] = eye.x;
    cb->cameraPosWS[1] = eye.y;
    cb->cameraPosWS[2] = eye.z;
    cb->cameraPosWS[3] = 0.f;

    // Byte 224 - the engine-global dissolve noise. ~0u is the engine's own "missing"
    // encoding; the preview's instances zero dissolveParams, so the PS branch that would
    // sample it never runs.
    cb->dissolveNoiseIdx = ~0u;
    // Byte 228 - the cross-layout ABI. 0 means identity: the VS's ResolveInstanceSlot
    // returns the linear index unchanged. Preview packets index the instance table
    // directly (instanceOffset = part), so identity is the correct mapping and needs no
    // private remap buffer.
    cb->instanceRemapIdx = 0;
    // Byte 232 - the dither phase. Pinned, again for determinism: a rotating
    // phase would make two runs of the same preview differ by a 4x4 Bayer
    // pattern on any Dithered material.
    cb->frameIndex = 0;
}

void PreviewRenderer::WriteLightCB(GraphSet& set, const Preview::Request& req,
                                  const LightingPass* lighting,
                                  uint32_t clusterLightCount)
{
    auto* lb = set.lightCB->Current(m_gfx);
    if (!lb) return;
    // Zeroed first, then every field the shader can reach is written below. Zero is
    // not a safe default here: a zero bindless index is a real descriptor (heap slot 0),
    // and Lighting.ps opens several - rtReflectionIdx with no gate at all - so a
    // forgotten field adds an unrelated texture to every preview pixel, silently.
    *lb = {};

    const PreviewCamera cam = BuildPreviewCamera(req.view);

    // ---- The key light ----
    // lightDir is the direction light travels (Renderer_DDGI documents the flip against
    // SkyIBLPass::GetSunDir), and lightColor is colour x intensity pre-multiplied, as
    // SetLightCBSun hands it over on the main path. Under Feature::kSceneLighting it is the
    // live scene's sun, in the same convention: SetLightCBSun already folded time-of-day
    // and atmospheric transmittance into the colour the game half snapshotted, so this is
    // the sun the viewport uses, not a second resolve of it.
    const bool sceneLit = (req.view.features & Preview::Feature::kSceneLighting) != 0
                       && SceneLightingUsable(m_sceneLighting);
    {
        const float* srcDir = sceneLit ? m_sceneLighting.sunDir
                                       : KeyLightDir();  // harness-overridable, see SetKeyLightDirOverride
        XMFLOAT3 d;
        XMStoreFloat3(&d, XMVector3Normalize(
            XMVectorSet(srcDir[0], srcDir[1], srcDir[2], 0.f)));
        lb->lightDir[0] = d.x; lb->lightDir[1] = d.y; lb->lightDir[2] = d.z;
    }
    {
        const float* srcCol = sceneLit ? m_sceneLighting.sunColor
                                       : Preview::kKeyLight.color;
        lb->lightColor[0] = srcCol[0];
        lb->lightColor[1] = srcCol[1];
        lb->lightColor[2] = srcCol[2];
    }

    XMFLOAT3 eye; XMStoreFloat3(&eye, cam.eye);
    lb->cameraPos[0] = eye.x; lb->cameraPos[1] = eye.y; lb->cameraPos[2] = eye.z;
    XMFLOAT3 fwd; XMStoreFloat3(&fwd, cam.forward);
    lb->cameraForward[0] = fwd.x; lb->cameraForward[1] = fwd.y; lb->cameraForward[2] = fwd.z;
    // The studio rig fits its single cascade to this camera (the splits below are all
    // pinned to farZ * 4, so every pixel resolves to cascade 0), so the
    // cascade-selection camera is the shading camera. Written explicitly: that the
    // zeroed default would pick the same cascade is only a coincidence of the pinned
    // splits.
    lb->csmCamPos[0] = eye.x; lb->csmCamPos[1] = eye.y; lb->csmCamPos[2] = eye.z;
    lb->csmCamFwd[0] = fwd.x; lb->csmCamFwd[1] = fwd.y; lb->csmCamFwd[2] = fwd.z;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->invViewProj),
                    XMMatrixTranspose(XMMatrixInverse(nullptr, cam.viewProj)));

    // Indirect. iblRadianceMips doubles as the gate on the whole indirect block, so it
    // is 0 whenever either cube or the BRDF LUT is missing: t8 (gBRDFLUT) has no fallback
    // anywhere, and the block does not test for it.
    const uint64_t irrSrc  = sceneLit ? m_sceneLighting.irradiance : m_iblIrradiance;
    const uint64_t radSrc  = sceneLit ? m_sceneLighting.radiance   : m_iblRadiance;
    const uint32_t mipsSrc = sceneLit ? m_sceneLighting.radianceMips : m_iblRadianceMips;
    const bool indirectOn = (irrSrc != 0) && (radSrc != 0) && (m_brdfLut != 0);
    lb->iblRadianceMips   = indirectOn ? mipsSrc : 0u;
    lb->iblStrength       = sceneLit ? m_sceneLighting.iblStrength
                                     : Preview::kIBLStrength;
    // SH ONLY ON THE SCENE PATH, and only when the scene itself is using it.
    // The studio triple is two static .itex cubes with no SH source at all, so
    // a 1 here would send Lighting.ps to gSkySH - which LightingPass::Init
    // fills with NINE ZEROED float4s - and every studio preview would lose its
    // diffuse IBL. Reading the scene's own iblUseSH keeps the toggle honest in
    // both directions.
    lb->iblUseSH          = (sceneLit && m_sceneLighting.skyUseSH) ? 1u : 0u;
    // The ambient term. On the studio path it is off by default: the rig's fill and
    // back are real clustered point lights (clusterLightCount below), so a constant
    // ambient would double-count them. Feature::kAmbientFill restores the constant
    // stand-in (a debug lever; nothing in the tree sets it). Lighting.ps applies it to
    // the PBR and NPR accumulators alike.
    // On the scene-lighting path the value comes from the scene, as Renderer_IBL
    // computes it: zero in every sky mode, the WorldEnvironment's flat ambient in
    // None/FlatColor, so the toggle reproduces the viewport.
    const bool ambientFill = (req.view.features & Preview::Feature::kAmbientFill) != 0;
    if (sceneLit)
    {
        lb->ambientColor[0]  = m_sceneLighting.ambientColor[0];
        lb->ambientColor[1]  = m_sceneLighting.ambientColor[1];
        lb->ambientColor[2]  = m_sceneLighting.ambientColor[2];
        lb->ambientIntensity = m_sceneLighting.ambientIntensity;
    }
    else
    {
        lb->ambientColor[0]  = ambientFill ? Preview::kAmbientColor[0] : 0.f;
        lb->ambientColor[1]  = ambientFill ? Preview::kAmbientColor[1] : 0.f;
        lb->ambientColor[2]  = ambientFill ? Preview::kAmbientColor[2] : 0.f;
        lb->ambientIntensity = ambientFill ? Preview::kAmbientIntensity : 0.f;
    }

    // The required zeroes: each switches off a subsystem the preview does not
    // instantiate, listed explicitly rather than left to the memset because this is the
    // list a new pass author has to extend. clusterLightCount is the exception: the
    // preview builds its own gLights / gLightIndexList / gLightGrid triple instead of a
    // ClusterPass, and 0 switches the clustered block off (buffers failed, or the
    // scene-lighting toggle is on).
    lb->clusterLightCount    = clusterLightCount;
    lb->reflectionProbeCount = 0;   // no probe manager
    lb->aerialMaxDistKm      = 0.f; // no aerial-perspective LUT (its "fallback" is dead code)
    lb->ddgiEnabled          = 0;
    lb->ddgiVolumeCount      = 0;
    lb->ddgiDiffuseScale     = 0.f;
    lb->ddgiAONearFieldStrength = 0.f;
    lb->ssgiIntensity        = 0.f;
    lb->ssgiResultIdx        = BINDLESS_NULL_TEX2D_FLOAT;   // "no SSGI"
    lb->rtShadowEnabled      = 0.f;
    lb->perObjShadowParams[0] = 0.f;
    lb->hairShadowParams[0]   = 0.f;

    // The studio key-light shadow as a one-cascade CSM. shadowStrength is the master
    // gate (ComputeShadowFactor returns 1.0 at 0), so the rest is written only when the
    // pass rendered this frame. shadow.hlsli picks a cascade with
    // `viewZ > cascadeSplits.z/.y/.x` tests and gates on `viewZ <= cascadeSplits.w`, so
    // all four splits beyond the far plane resolve every pixel to cascade 0 with the gate
    // open. shadowBlendRange 0: a non-zero value would cross-fade into slice 1, which
    // this pass leaves empty. shadowFrameIndex is pinned to 0 like PerViewCB::frameIndex:
    // the main path rotates four Bayer phases, which would make every preview run differ.
    if (m_studioShadowValid)
    {
        std::memcpy(lb->shadowMatrix[0], m_studioShadowMatrix, sizeof(float) * 16);
        // Slices 1..3 get the same matrix rather than zero: they are unreachable (see the
        // splits), but a zero matrix is a divide-by-zero in SampleCascadeShadow's
        // perspective divide if a future edit makes one reachable.
        for (int i = 1; i < 4; ++i)
            std::memcpy(lb->shadowMatrix[i], m_studioShadowMatrix, sizeof(float) * 16);
        const float kFar = Preview::kStudioCamera.farZ * 4.0f;
        lb->cascadeSplits[0] = lb->cascadeSplits[1] =
        lb->cascadeSplits[2] = lb->cascadeSplits[3] = kFar;
        for (int i = 0; i < 4; ++i)
            lb->cascadeTexelWorldSize[i] = m_studioShadowTexelWorld;
        lb->shadowBias         = Preview::kStudioShadowBias;
        lb->shadowStrength     = Preview::kStudioShadowStrength;
        lb->shadowMapTexelSize = 1.0f / static_cast<float>(PreviewStudioShadowPass::kSize);
        lb->shadowBlendRange   = 0.0f;
        lb->shadowFrameIndex   = 0u;
        lb->shadowNormalOffset = Preview::kStudioShadowNormalOffset;
    }
    else
    {
        lb->shadowStrength   = 0.f;   // ComputeShadowFactor returns 1.0 immediately
        lb->shadowBlendRange = 0.f;
        lb->shadowFrameIndex = 0u;
    }
    (void)set;
    lb->nprMinBrightness     = 0.f; // the NPR floor is per material; the scene floor stays 0
    lb->nprMaxBrightness     = 0.f;
    // This view's mode (the Mesh Editor sets it); 0 = Lit for every other caller, and
    // out of range = Lit. The main viewport's mode is a different field of a different
    // buffer.
    lb->viewMode = (req.view.viewMode < static_cast<uint32_t>(Renderer::ViewMode::Count))
                 ? req.view.viewMode : 0u;
    // The meshlet views are a main-view tool (the preview's GBufferPass never
    // takes the mesh path): Lit here, never a value Lighting.ps has no case for.
    if (Renderer::IsMeshletDebugView(static_cast<Renderer::ViewMode>(lb->viewMode))) lb->viewMode = 0u;
    lb->waterGlobals[0] = lb->waterGlobals[1] = lb->waterGlobals[2] = lb->waterGlobals[3] = 0;
    lb->waterRTSize[0]  = static_cast<float>(req.view.w);
    lb->waterRTSize[1]  = static_cast<float>(req.view.h);
    lb->cloudShadowIdx  = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();   // no clouds
    lb->fwdReflNormalIdx = lb->fwdReflDepthIdx = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();   // nulls of their kind: no forward reflections
    lb->fwdReflStrength  = 0.f;
    lb->fwdReflPrevHdrIdx = lb->fwdReflFrontRtrIdx = lb->fwdReflFrontDepthIdx
                          = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();
    lb->fwdAoIdx         = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();

    // viewMatrix / clusterNearZ / clusterFarZ, unconditionally. The main path writes
    // them inside `if (m_clusterPass)`, but viewMatrix has readers outside the cluster
    // gate, one on by default: DanbaidongRP's NPR depth rim does
    // `normalize(mul(N, (float3x3)viewMatrix))` (nprRimWidth 2.5 / nprRimStrength 0.3 are
    // the component defaults), and a zero matrix makes that NaN on every Toon pixel.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->viewMatrix),
                    XMMatrixTranspose(cam.view));
    lb->clusterNearZ = cam.nearZ;
    lb->clusterFarZ  = cam.farZ;

    // Bindless indices, each resolved to a type-correct resource: the shader opens
    // ResourceDescriptorHeap[index] directly, so an unwritten field names heap slot 0 -
    // an addressable descriptor of the wrong type. LightingPass#2's Init built a real 1x1
    // fallback for each, which its accessors return when no live resource is set.
    auto heapIdx = [&](uint64_t srv) -> uint32_t
    {
        if (!srv) return 0u;
        const uint32_t idx = m_gfx.DescriptorHeapIndexFromGpuHandle(srv);
        // ~0u would read far outside the heap and remove the device. Slot 0 is
        // always addressable: wrong contents, but survivable - the same clamp
        // Renderer's own resolve() applies.
        return (idx == ~0u) ? 0u : idx;
    };
    if (lighting)
    {
        // rtReflectionIdx is read ungated (Lighting.ps loads it before any branch), so it
        // must name a real black texture. The pass is an argument: each graph instance owns
        // its own 1x1 fallbacks, and the live pass's indices in the thumbnail dispatch's
        // LightCB would name descriptors that graph never transitioned (or, after a live
        // resize, freed ones). The typed, checked handle, as Renderer writes it
        // (bindless.hlsli's BindlessTex2D opens it).
        lb->rtReflectionIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetRTReflectionSrv(), "preview RT reflection").Bits();
        // The shadow family's typed handles, as Renderer writes them.
        lb->rtShadowMaskIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetRTShadowMaskSrv(), "preview RT shadow mask").Bits();
        lb->pointShadowIdx    = m_gfx.BindlessOf<RHI::Bindless::Kind::TexCubeArrayFloat>(
                                    lighting->GetPointShadowAtlasSrv(), "preview point shadow atlas").Bits();
        lb->perObjShadowIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetPerObjectShadowSrv(), "preview per-object shadow").Bits();
        lb->hairShadowIdx     = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetHairShadowSrv(), "preview hair shadow").Bits();
        // Type-correct NULL StructuredBuffer SRVs (strides 8 / 4 / 192). Read
        // only inside reflectionProbeCount > 0, which is 0 above - but a
        // dimension mismatch on an opened descriptor is UB that Resource
        // Binding Tier 3 never reports, so they are still named correctly.
        lb->reflProbeGridIdx  = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(lighting->GetReflProbeGridSrv(),
                                    "preview probe cluster grid", 8u).Bits();
        lb->reflProbeIndexIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(lighting->GetReflProbeIndexSrv(),
                                    "preview probe index list", 4u).Bits();
        lb->reflProbeBufIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(lighting->GetReflProbeBufferNullSrv(),
                                    "preview probe buffer", static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe))).Bits();
    }
    // ddgiVolumeBufIdx is gated off by ddgiEnabled / ddgiVolumeCount, and ssrResultIdx
    // is read only by Water.ps (no water here): the nulls of their kinds, not heap 0.
    lb->ddgiVolumeBufIdx = m_gfx.NullOf<RHI::Bindless::Kind::StructuredBuffer>(static_cast<uint32_t>(sizeof(DDGI::VolumeGPUDesc))).Bits();
    lb->ssrResultIdx     = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();

    // ---- Exposure ----
    // custom_surface.hlsli gates MaterialGPUData::exposureCompensation on
    // `exposureBufIdx != 0`, so leaving this at 0 would make that material knob
    // work in the viewport and silently do nothing in the preview. The buffer
    // is the neutral 1-float 1.0 the tone map already uses.
    lb->exposureBufIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(m_neutralExposureSrv, "preview neutral exposure", 4u).Bits();
}

// ==== The WorldCull half ====
// Everything here runs on the render half, inside RecordRequest, and reaches the live
// scene only through render-safe Renderer accessors (the BVH and the frame's
// DrawLists, as ProcessProbeBakeQueue uses them from this phase). No World, ECS pool
// or component.
namespace
{
    using namespace DirectX;

    // The secondary camera's view + reversed-Z projection, built the way
    // Renderer::BuildCameraViewProj builds the main one - near and far SWAPPED,
    // GREATER_EQUAL depth - because the preview graph's depth state is the
    // engine's and a second convention here is an empty picture with nothing to
    // explain it.
    struct WorldCamera
    {
        XMVECTOR eye, forward;
        XMMATRIX view, proj, viewProj;
        float    nearZ, farZ;
    };

    WorldCamera BuildWorldCamera(const RenderCamera& cam, uint32_t w, uint32_t h)
    {
        WorldCamera c{};
        c.eye     = XMLoadFloat3(&cam.position);
        c.forward = XMVector3Normalize(XMLoadFloat3(&cam.forward));
        // The same degenerate-up guard the main path applies: a rolled cinematic
        // up that has drifted parallel to forward makes XMMatrixLookToLH produce
        // NaNs, and every pixel downstream of it.
        XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&cam.up));
        if (XMVector3IsNaN(up) ||
            std::abs(XMVectorGetX(XMVector3Dot(up, c.forward))) > 0.999f)
            up = XMVectorSet(0.f, 1.f, 0.f, 0.f);

        const float aspect = (h != 0) ? static_cast<float>(w) / static_cast<float>(h)
                                      : 1.0f;
        c.view  = XMMatrixLookToLH(c.eye, c.forward, up);
        c.nearZ = cam.nearZ;
        c.farZ  = cam.farZ;
        if (cam.orthographic)
        {
            const float halfH = (std::max)(cam.orthoHalfHeight, 0.01f);
            c.proj = XMMatrixOrthographicLH(halfH * aspect * 2.f, halfH * 2.f,
                                            cam.farZ, cam.nearZ);
        }
        else
        {
            c.proj = XMMatrixPerspectiveFovLH(cam.fov, aspect, cam.farZ, cam.nearZ);
        }
        c.viewProj = XMMatrixMultiply(c.view, c.proj);
        return c;
    }
}

bool PreviewRenderer::BuildWorldDrawList(const Preview::Request& req)
{
    m_worldPackets.clear();
    if (!req.camValid) return false;
    // One line per view, the first time a cull is attempted: otherwise "drew nothing"
    // and "was never asked to draw" are the same absence in the log (a camera that did
    // not resolve vs. a cull that kept nothing). Per view, so each of four live cells
    // gets its camera line.
    const uint32_t vid = (req.viewId < Preview::kMaxPreviews) ? req.viewId : 0u;
    if (!m_worldCullAttempted[vid])
    {
        m_worldCullAttempted[vid] = true;
        LOG_INFO("[PREVIEW] world cull first attempt: view %u cam (%.2f, %.2f, "
                 "%.2f) fwd (%.2f, %.2f, %.2f) fov %.3f near %.2f far %.1f %ux%u",
                 req.viewId,
                 req.cam.position.x, req.cam.position.y, req.cam.position.z,
                 req.cam.forward.x, req.cam.forward.y, req.cam.forward.z,
                 req.cam.fov, req.cam.nearZ, req.cam.farZ,
                 req.view.w, req.view.h);
    }

    // One call, and the whole second cull is behind it: Renderer owns the BVH, the
    // frame's DrawLists and the draw-local -> entity remap, all private (see
    // Renderer::CullWorldForPreview for the filter and the transparent re-sort).
    // The recording view is published before the call because NoteWorldCullCounts is a
    // callback from inside it; render half only, one recording thread, so a plain member.
    m_worldCullRecordingView = vid;
    m_owner.CullWorldForPreview(req.cam, req.view.w, req.view.h, req.viewBit,
                                m_worldPackets);

    m_worldCullDraws[vid].store(static_cast<uint32_t>(m_worldPackets.size()),
                                std::memory_order_relaxed);
    m_worldCullLastView.store(vid, std::memory_order_relaxed);

    // An empty list is drawn, not refused: a camera pointed at open sky culls to
    // nothing every frame, and refusing would freeze the panel on a stale picture
    // presented as live. Drawing it gives the correct picture (sky, cleared background).
    // The real failures - a broken viewMask, a stale BVH, a camera at the origin - keep
    // their evidence: this one-shot warning and the "[PREVIEW] world cull:" line's
    // draws=0, which the packs' logLineFields bounds turn red.
    if (m_worldPackets.empty() && !m_worldCullEmptyLogged[vid])
    {
        m_worldCullEmptyLogged[vid] = true;
        LOG_WARNING("[PREVIEW] world cull kept 0 of %u candidate packet(s) for "
                    "view %u - that secondary view draws the sky and nothing "
                    "else. If that is not what the camera is pointed at, check "
                    "the camera (logged above), the viewMask bit and the BVH.",
                    m_worldCullCandidates[vid].load(std::memory_order_relaxed),
                    req.viewId);
    }
    return true;
}

void PreviewRenderer::WriteWorldPerViewCB(GraphSet& set, const Preview::Request& req)
{
    auto* cb = set.perViewCB->Current(m_gfx);
    if (!cb) return;
    *cb = {};

    const WorldCamera c = BuildWorldCamera(req.cam, req.view.w, req.view.h);

    // Transposed for HLSL's row-vector mul(pos, matrix). prev == cur: this view has no
    // temporal consumer (no TAA, velocity fill or upscaler), so exact-zero velocity is
    // correct and cheaper than a history matrix.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->viewProj),
                    XMMatrixTranspose(c.viewProj));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->prevViewProj),
                    XMMatrixTranspose(c.viewProj));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb->curViewProjNoJitter),
                    XMMatrixTranspose(c.viewProj));

    // Time is the scene's, unlike the studio path: a WorldCull view draws the same
    // entities as the main viewport, and a custom vertex shader swaying with timeParams
    // must stay in phase with the viewport's copy. Nothing about this view is cached.
    const Renderer::PreviewFrameSnapshot fs = m_owner.PreviewFrameState();
    const float tSec = fs.globalTimeSec;
    cb->timeParams[0] = tSec;
    cb->timeParams[1] = 0.f;          // dt: the preview does not integrate anything
    cb->timeParams[2] = std::sin(tSec);
    cb->timeParams[3] = std::cos(tSec);

    XMFLOAT3 eye; XMStoreFloat3(&eye, c.eye);
    cb->cameraPosWS[0] = eye.x;
    cb->cameraPosWS[1] = eye.y;
    cb->cameraPosWS[2] = eye.z;
    cb->cameraPosWS[3] = 0.f;

    cb->dissolveNoiseIdx = fs.dissolveNoiseIdx;
    // Byte 228 - the cross-layout ABI, and the one field that sets a WorldCull view
    // apart from every other b1 producer here: the scene's packets carry draw-local
    // instance offsets resolved through the remap buffer this names. It must be the
    // pristine per-frame remap, never the working half, whose lower range is a GPU copy
    // this command list does not wait on and whose upper range holds compacted survivor
    // spans the preview's full-range draws would index past - the rule the shadow family
    // follows (Renderer's instRemapIdxPristine).
    cb->instanceRemapIdx = fs.pristineRemapIdx;
    // The dither phase. The scene's, so a Dithered material in the PiP dithers
    // in lockstep with the same object in the main viewport instead of holding
    // one static Bayer phase against a moving one.
    cb->frameIndex = fs.frameIndex;
}

void PreviewRenderer::WriteWorldLightCB(GraphSet& set, const Preview::Request& req,
                                        const LightingPass* lighting,
                                        uint32_t clusterLightCount)
{
    auto* lb = set.lightCB->Current(m_gfx);
    if (!lb) return;

    // Start from the main frame's LightCB, whole: a secondary view of the same world
    // wants the same sun, IBL strength, sky SH, fog constants, NPR floors and exposure
    // buffer - forty-odd fields whose right value is whatever the main path computed this
    // frame. Rebuilding them here would be a second BuildScene_UploadLights that drifts
    // silently when a field is added, so the copy is the default and the overrides below
    // are the exception list. With no main LightCB this frame the struct starts zeroed and
    // only those overrides are written.
    const RendererDetail::LightCB* mainCB = m_owner.PreviewFrameState().mainLightCB;
    if (!mainCB) { *lb = {}; }
    else         { *lb = *mainCB; }
    // The nulls of their kinds, not 0.
    lb->restirDiIdx    = BINDLESS_NULL_TEX2DARRAY_FLOAT;
    lb->ptResultIdx    = lb->restirGiIdx = BINDLESS_NULL_TEX2D_FLOAT;
    lb->renderPathMode = 0u;

    const WorldCamera c = BuildWorldCamera(req.cam, req.view.w, req.view.h);

    // ---- 1. View-dependent: re-derived from this camera ----
    XMFLOAT3 eye; XMStoreFloat3(&eye, c.eye);
    lb->cameraPos[0] = eye.x; lb->cameraPos[1] = eye.y; lb->cameraPos[2] = eye.z;
    XMFLOAT3 fwd; XMStoreFloat3(&fwd, c.forward);
    lb->cameraForward[0] = fwd.x; lb->cameraForward[1] = fwd.y; lb->cameraForward[2] = fwd.z;
    // csmCamPos / csmCamFwd are deliberately not overridden: the copy holds the main
    // camera, which the borrowed cascade matrices and splits were fitted to, so
    // shadow.hlsli picks the cascade the pixel lives in. This camera's values would index
    // the split table with a depth along another axis from another origin: misplaced
    // seams, or every pixel in cascade 0 or off the cascadeSplits.w gate.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->invViewProj),
                    XMMatrixTranspose(XMMatrixInverse(nullptr, c.viewProj)));
    // viewMatrix has readers outside the cluster gate, one on by default
    // (DanbaidongRP's NPR depth rim, normalize(mul(N, (float3x3)viewMatrix))), so it is
    // written whatever clusterLightCount is.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->viewMatrix),
                    XMMatrixTranspose(c.view));
    lb->clusterNearZ = c.nearZ;
    lb->clusterFarZ  = c.farZ;
    lb->waterRTSize[0] = static_cast<float>(req.view.w);
    lb->waterRTSize[1] = static_cast<float>(req.view.h);

    // ---- 2. What a secondary view does not get ----
    // Screen-space or main-view-shaped resources: produced for the main viewport's pixels
    // at its size and left in the main graph's state, so naming one here is a wrong-state
    // read of a wrongly sized texture.
    //   * Reflection probes: gReflectionProbeGrid is a froxel grid for the main camera's
    //     frustum; zero switches the block off, so a secondary view gets no
    //     parallax-corrected probe reflections.
    //   * SSGI / SSR / RT reflection / RT shadow mask / SSAO / aerial perspective / cloud
    //     shadow / the forward-reflection set: screen-space and main-view-sized.
    //   * DDGI: would be correct, but the preview graph binds no DDGI volume resources
    //     and its LightingPass has no fallback for them.
    // The clustered block is not in that list: the preview builds its own grid for this
    // view (WriteWorldClusterLights), as lane 0 does for the studio rig:
    //   * a frustum-culled, importance-capped subset of the scene's punctual lights,
    //     chosen on the game half against this view's frustum and capped at
    //     Preview::kMaxPreviewLights;
    //   * over a uniform grid - every cluster names every light, equivalent to a real
    //     froxel cull because AccumClusteredLight opens with
    //     `if (dist > light.radius) return;`;
    //   * with no punctual shadows: every record has shadowSliceIdx 0xFFFFFFFF because
    //     RecordWorldCullSetup binds SetSpotShadowAtlas(0,0) / SetPointShadowAtlas(0), so
    //     a shadow-casting lamp appears unshadowed here (also stated in the camera
    //     panel's tooltip);
    //   * directionals dropped: AccumClusteredLight returns on type == 0, and the sun
    //     comes through the LightCB copy.
    // Zero whenever WriteWorldClusterLights refused (sun + IBL only).
    lb->clusterLightCount    = clusterLightCount;
    lb->reflectionProbeCount = 0;
    lb->aerialMaxDistKm      = 0.f;
    // No transmission apply runs in this graph, whatever the main frame drew: its
    // Transmissive surfaces take the coverage fallback.
    lb->transmissionApplied  = 0u;
    lb->ddgiEnabled          = 0;
    lb->ddgiVolumeCount      = 0;
    lb->ddgiDiffuseScale     = 0.f;
    lb->ddgiAONearFieldStrength = 0.f;
    lb->ddgiVolumeBufIdx     = m_gfx.NullOf<RHI::Bindless::Kind::StructuredBuffer>(static_cast<uint32_t>(sizeof(DDGI::VolumeGPUDesc))).Bits();   // null: no DDGI
    lb->ssgiIntensity        = 0.f;
    lb->ssgiResultIdx        = BINDLESS_NULL_TEX2D_FLOAT;   // "no SSGI"
    lb->ssrResultIdx         = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();   // null: no SSR
    lb->rtShadowEnabled      = 0.f;
    lb->cloudShadowIdx       = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();   // no clouds
    lb->fwdReflNormalIdx = lb->fwdReflDepthIdx = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();   // nulls of their kind: no forward reflections
    lb->fwdReflStrength  = 0.f;
    lb->fwdReflPrevHdrIdx = lb->fwdReflFrontRtrIdx = lb->fwdReflFrontDepthIdx
                          = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();
    lb->fwdAoIdx         = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();
    lb->perObjShadowParams[0] = 0.f;
    lb->hairShadowParams[0]   = 0.f;
    lb->viewMode              = 0;   // Lit: the editor's debug view modes are the main view's

    // ---- 3. The bindless indices, re-pointed at this graph's fallbacks ----
    // The copied values name the main graph's textures. rtReflectionIdx is read with no
    // gate in Lighting.ps, so a copied value would add an unrelated, wrongly-stated
    // texture to every secondary pixel. LightingPass#3's Init built a type-correct 1x1
    // for each.
    auto heapIdx = [&](uint64_t srv) -> uint32_t
    {
        if (!srv) return 0u;
        const uint32_t idx = m_gfx.DescriptorHeapIndexFromGpuHandle(srv);
        return (idx == ~0u) ? 0u : idx;
    };
    if (lighting)
    {
        // The typed, checked handle, as Renderer writes it (bindless.hlsli's BindlessTex2D
        // opens it).
        lb->rtReflectionIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetRTReflectionSrv(), "preview RT reflection").Bits();
        // The shadow family's typed handles, as Renderer writes them.
        lb->rtShadowMaskIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetRTShadowMaskSrv(), "preview RT shadow mask").Bits();
        lb->pointShadowIdx    = m_gfx.BindlessOf<RHI::Bindless::Kind::TexCubeArrayFloat>(
                                    lighting->GetPointShadowAtlasSrv(), "preview point shadow atlas").Bits();
        lb->perObjShadowIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetPerObjectShadowSrv(), "preview per-object shadow").Bits();
        lb->hairShadowIdx     = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                    lighting->GetHairShadowSrv(), "preview hair shadow").Bits();
        lb->reflProbeGridIdx  = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(lighting->GetReflProbeGridSrv(),
                                    "preview probe cluster grid", 8u).Bits();
        lb->reflProbeIndexIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(lighting->GetReflProbeIndexSrv(),
                                    "preview probe index list", 4u).Bits();
        lb->reflProbeBufIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(lighting->GetReflProbeBufferNullSrv(),
                                    "preview probe buffer", static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe))).Bits();
    }
    // And the water pair: waterGlobals[0] and [1] are the main frame's
    // refraction-source and scene-depth snapshot bindless indices, which
    // water_common.hlsli opens by heap index with no gate. There is no water in a
    // secondary view (the WorldCull filter has no Water form), so 0 - "unbound", as for
    // the indices above - is correct. waterGlobals[2] (the ripple body count) gates a
    // table only Water.ps reads.
    lb->waterGlobals[0] = 0u;
    lb->waterGlobals[1] = 0u;

    // ---- 3b. Exposure: the scene's, not neutral ----
    // custom_surface.hlsli's forward Unlit / emissive branch divides by gExposure when
    // exposureBufIdx != 0, on the contract that the tone map multiplies it back, so both
    // ends read the same buffer: this preview-owned per-frame-slot scalar, written with
    // the scene's exposure (the tone-map tail reads m_worldToneMapExposureSrv). With auto
    // exposure off, Renderer::PreviewFrameState hands over the resolved manualExposure
    // (exact, no lag); with it on, AutoExposurePass's readback, kFramesInFlight-1 frames
    // behind - `exact` says which. PreviewFrameState records why the scene's GPU exposure
    // buffer cannot be bound directly. With no reading, or with the per-frame buffer
    // failed to create (m_worldExposureReady false), both ends use the neutral buffer;
    // the second case warns.
    {
        const uint32_t slot = m_gfx.GetFrameIndex();
        const Renderer::PreviewFrameSnapshot fs = m_owner.PreviewFrameState();
        const float    e    = fs.sceneExposure;
        uint64_t srv = m_neutralExposureSrv;
        bool     neutral = true;
        if (m_worldExposureReady && slot < RHI::kFramesInFlight &&
            m_worldExposureMapped[slot] && e > 0.0f)
        {
            std::memcpy(m_worldExposureMapped[slot], &e, sizeof(float));
            srv = m_worldExposureSrv[slot];
            neutral = false;
        }
        lb->exposureBufIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(srv, "preview exposure", 4u).Bits();
        m_worldToneMapExposureSrv = srv;   // the tone-map tail reads this
        // Two lines, and the second is the one a pack gates: the first record of a session
        // almost always precedes any exposure reading (AutoExposurePass has not run yet), so
        // a line on the first record would always say `neutral=1 exact=0`. The second fires
        // on the first record that binds a real reading.
        if (!m_worldExposureLogged)
        {
            m_worldExposureLogged = true;
            LOG_INFO("[PREVIEW] world exposure first: exposure=%.4f exact=%d "
                     "neutral=%d ready=%d", neutral ? 1.0f : e,
                     fs.sceneExposureExact ? 1 : 0, neutral ? 1 : 0,
                     m_worldExposureReady ? 1 : 0);
        }
        // On every change, not once: the question is whether the PiP follows the viewport.
        // Rate-limited by the value itself, so a settled scene logs one line and a ramp one
        // per step; logLineFields takes the last matching line, i.e. the exposure the
        // captured picture used. `exact=1`: the resolved manualExposure the main ToneMap
        // uses, no lag; `exact=0`: AutoExposurePass's readback ring, kFramesInFlight-1 frames
        // behind. `neutral` and `ready` are printed, not literal: they are implied on this
        // branch (`!neutral` is the condition, m_worldExposureReady keeps `neutral` clear),
        // and printing the real values turns the packs red if that ever stops holding.
        if (!neutral && std::fabs(e - m_worldExposureLastLogged) > 1e-4f)
        {
            m_worldExposureLastLogged = e;
            LOG_INFO("[PREVIEW] world exposure: exposure=%.4f exact=%d neutral=%d "
                     "ready=%d", e, fs.sceneExposureExact ? 1 : 0,
                     neutral ? 1 : 0, m_worldExposureReady ? 1 : 0);
        }
        // The warning is for the defect only: "no reading yet" is every session's opening
        // state (INFO above), while a per-frame buffer that failed to create pins the whole
        // session at 1.0.
        if (!m_worldExposureReady && !m_worldExposureNeutralWarned)
        {
            m_worldExposureNeutralWarned = true;
            LOG_WARNING("[PREVIEW] the WorldCull view is rendering at the NEUTRAL "
                        "1.0 exposure because the preview's per-frame exposure "
                        "buffer failed to create. Its brightness will not track "
                        "the main viewport's, and a forward Unlit or emissive "
                        "material divides by an exposure nothing multiplies back. "
                        "(logged once per session)");
        }
    }

    // ---- 4. The CSM: borrowed, not rebuilt ----
    // The cascade matrices, splits, texel sizes, bias, strength, blend range and frame
    // index come from the copy, and the caller binds the atlas (SetShadowMap). The
    // cascades are fitted to the main camera, so a secondary camera is shadowed only
    // where the two frusta overlap (cascadeSplits.w gates the rest); re-fitting would
    // need a second ShadowSystem and four more 2K slices. Nothing is overridden: since
    // csmCamPos / csmCamFwd keep the main camera, the main shadowBlendRange puts the
    // cross-fade band where the viewport has it.
    if (!m_owner.PreviewMainShadowSnapshot().valid)
        lb->shadowStrength = 0.f;   // ComputeShadowFactor returns 1.0 immediately
}

// The whole render-half setup for a WorldCull view: draw list, constants,
// bindings and the lighting pass's inputs. Everything the synthetic path does
// between the preconditions and the HDR push, done for the other draw source.
bool PreviewRenderer::RecordWorldCullSetup(GraphSet& set, const Preview::Request& req)
{
    // Cleared before anything can bail: a stale handle would hand ToneMapPass#3 an
    // exposure the LightCB did not name.
    m_worldToneMapExposureSrv = 0;
    if (!BuildWorldDrawList(req)) return false;

    WriteWorldPerViewCB(set, req);

    // The scene's clustered point and spot lights, written before the LightCB: the
    // count returned is LightCB.clusterLightCount, and a count larger than the lane's
    // light buffer walks off the end of an UPLOAD StructuredBuffer - one writer, one
    // count. The frame slot is read once, so the write and the SRV bind below use the
    // same slot.
    const uint32_t clusterSlot        = m_gfx.GetFrameIndex();
    const uint32_t worldClusterLights = WriteWorldClusterLights(clusterSlot, req);
    WriteWorldLightCB(set, req, set.lighting, worldClusterLights);

    // ---- Graph bindings: the scene's buffers, not the private ones ----
    set.graph.BindConstantBuffer("PerView", set.perViewCB->CurrentBuffer(m_gfx));
    set.graph.BindConstantBuffer("LightCB", set.lightCB->CurrentBuffer(m_gfx));
    // No fog in a preview (the zero block; see the member).
    set.graph.BindConstantBuffer("FogApply", m_fogApplyNoneCB.CurrentBuffer(m_gfx));
    if (set.skybox)
        set.graph.BindConstantBuffer("SkyCB", set.skybox->GetSkyCB(m_gfx));

    const Renderer::PreviewFrameSnapshot fs = m_owner.PreviewFrameState();
    const RHI::GPUBuffer* instBuf = fs.instanceBuffer;
    const RHI::GPUBuffer* matBuf  = fs.materialBuffer;
    // NOT A DEGRADATION. Root 8 unbound is a VS that fetches garbage transforms;
    // root 10 / 28 unbound is a shader reading an unbound StructuredBuffer. The
    // frames before the scene's buffers exist simply do not draw.
    if (!instBuf || !instBuf->IsValid() || !matBuf || !matBuf->IsValid())
        return false;

    set.graph.BindBuffer("InstanceBuffer", *instBuf);
    set.graph.BindBuffer("MeshDescriptors",
                       m_owner.GetMeshManager().GetDescriptorHeap().CurrentMeshDescBuffer());
    set.graph.BindBuffer("MaterialBuffer", *matBuf);
    // Root 14 (t0 space1) - the bindless buffer table. Unbound is a device removal,
    // not a black frame.
    set.graph.SetBindlessTableHandle(
        m_owner.GetMeshManager().GetDescriptorHeap().GetBufferTableGpuHandle());
    // Borrowed pointer - m_worldPackets must outlive Execute, which is why it is
    // a member. It is a SEPARATE member from m_packets so a synthetic record and
    // a world record in the same frame (the two-lane budget) cannot overwrite
    // each other's list.
    set.graph.SetDrawList(&m_worldPackets);
    // ROOT 28 (t12 space0) - the SECOND binding of the same table. Lighting.ps
    // decodes matIdx out of GBuffer1's alpha and indexes gMaterials THERE, not
    // at t2; binding only root 10 gives a correct G-buffer and silently wrong
    // lighting for every runtime-branching shading model.
    set.lighting->SetMaterialBuffer(m_gfx.GetBufferSRVGpuHandle(*matBuf));

    // The environment is always the scene's: a WorldCull view is a second camera on
    // the same world, with no studio rig and no kSceneLighting toggle. t6/t7 must never
    // be unbound; the shared 1x1 black cube is the defined degradation (dead metals), and
    // that substitution lives in Renderer::SyncSkyboxIBL, not LightingPass, so a path
    // driving a LightingPass instance directly applies it itself.
    const uint64_t fallbackCube = m_owner.GetFallbackSkyCubeSrv();
    const uint64_t irr = m_sceneLighting.irradiance ? m_sceneLighting.irradiance
                                                    : fallbackCube;
    const uint64_t rad = m_sceneLighting.radiance   ? m_sceneLighting.radiance
                                                    : fallbackCube;
    const bool indirectOn = (m_sceneLighting.irradiance != 0) &&
                            (m_sceneLighting.radiance   != 0) && (m_brdfLut != 0);
    set.lighting->SetIBL(irr, rad, indirectOn ? m_sceneLighting.radianceMips : 0u,
                         m_sceneLighting.iblStrength);
    set.lighting->SetBRDFLUT(m_brdfLut);
    set.lighting->SetSkySH(m_sceneLighting.skyUseSH ? m_sceneLighting.skySH : 0ull,
                           m_sceneLighting.skyUseSH);

    // The borrowed CSM. The atlas is a world-space resource written earlier this
    // frame by the main ShadowPass and left in SHADER_RESOURCE; this graph's first pass
    // waits on the chained colorLastCL, which follows the main graph (SetExternalWait in
    // RecordRequest), so the read is ordered. 0 routes LightingPass back to its own
    // 1x1x4 fully-lit fallback.
    const Renderer::PreviewMainShadow sh = m_owner.PreviewMainShadowSnapshot();
    set.lighting->SetShadowMap(sh.valid ? sh.atlasSrv : 0ull);
    // Everything else keeps LightingPass#3's own neutral fallbacks - see
    // WriteWorldLightCB's exception list for why each is off rather than
    // borrowed.
    set.lighting->SetSpotShadowAtlas(0, 0);
    set.lighting->SetPointShadowAtlas(0);
    set.lighting->SetPerObjectShadowMap(0);
    set.lighting->SetHairShadowMap(0);
    // The clustered triple, all three or none: LightingPass::Execute skips each
    // unbound SRV individually, so a non-zero clusterLightCount with one unbound is an
    // undefined descriptor read (plausible pollution, no debug-layer message). The grid
    // is this view's own: the light and index rings are per (lane, slot), rewritten every
    // record; the memoised grid is per (lane, view, slot), so the bind must name the view
    // the write did.
    uint64_t hLights = 0, hIndex = 0, hGrid = 0;
    if (worldClusterLights > 0 && m_clusterReady[kLaneWorld] &&
        req.viewId < Preview::kMaxPreviews)
    {
        hLights = m_gfx.GetBufferSRVGpuHandle(m_clusterLightBuf[kLaneWorld][clusterSlot]);
        hIndex  = m_gfx.GetBufferSRVGpuHandle(m_clusterIndexBuf[kLaneWorld][clusterSlot]);
        hGrid   = m_gfx.GetBufferSRVGpuHandle(m_clusterGridBuf[kLaneWorld][req.viewId][clusterSlot]);
    }
    set.lighting->SetClusterSRVs(hLights, hIndex, hGrid);
    // All three handles or none, checked after the GetBufferSRVGpuHandle calls that
    // can return 0 (a check on m_clusterReady alone is implied by a non-zero count and
    // could never fire).
    ENGINE_CHECK_ONCE(worldClusterLights == 0 || (hLights && hIndex && hGrid),
        "PreviewRenderer::RecordWorldCullSetup: view %u reported %u clustered "
        "lights but the lane-%u triple resolved to (%llu, %llu, %llu) - "
        "LightCB.clusterLightCount would open the block against an unbound "
        "t13/t14/t15.",
        req.viewId, worldClusterLights, kLaneWorld,
        static_cast<unsigned long long>(hLights),
        static_cast<unsigned long long>(hIndex),
        static_cast<unsigned long long>(hGrid));
    set.lighting->SetSSAOHandle(0);
    set.lighting->SetAerialPerspective(0, 0.f);
    set.lighting->SetReflectionProbeCluster(0, 0);
    set.lighting->SetRTReflection(0);
    set.lighting->SetRTShadowMask(0);

    // ---- TransparentPass#3, mirroring the deferred half ----
    if (set.transparent)
    {
        set.transparent->SetIBL(irr, rad,
                                indirectOn ? m_sceneLighting.radianceMips : 0u,
                                m_sceneLighting.iblStrength);
        set.transparent->SetBRDFLUT(m_brdfLut);
        set.transparent->SetSkySH(m_sceneLighting.skyUseSH ? m_sceneLighting.skySH : 0ull);
        set.transparent->SetShadowMap(sh.valid ? sh.atlasSrv : 0ull);
        // MS_ForwardLighting has a clustered point / spot loop that opens the triple when
        // LightCB.clusterLightCount is non-zero, so this instance gets exactly the deferred
        // half's triple. Preview lights carry no shadow slice, so the spot-shadow views stay
        // the pass's own fallback.
        set.transparent->SetClusterSRVs(hLights, hIndex, hGrid);
        set.transparent->SetSpotShadowVP(0);
    }

    // The backdrop: the scene's sky. SkyboxPass#3 draws the main viewport's radiance
    // cube, plus its sun disk, moon and stars from a snapshot of the main pass's CPU
    // inputs (SkyboxPass::SkyState, a copyable POD) - borrowed the way the CSM is - written
    // into this pass's own SkyCB slot; nothing reads the main pass's buffer.
    // WorldCull only: the studio path in RecordRequest keeps a hard-off sun, because a sun
    // disk across a studio-lit subject would leak the scene into a deliberately
    // scene-independent picture. The star cube is borrowed, not baked (1024^2 x6 RGBA16F
    // is 48 MiB): SkyState carries the main pass's baked SRV, reported as 0 until baked.
    if (set.skybox)
    {
        set.skybox->SetEnvMap(m_sceneLighting.skyboxCube ? m_sceneLighting.skyboxCube
                                                         : rad);
        const SkyboxPass::SkyState sky = m_owner.PreviewSkyState();
        if (sky.valid)
        {
            set.skybox->ApplySkyState(sky);
        }
        else
        {
            // No main SkyboxPass (a graph built without one): a zeroed SkyCB would carry a sun
            // of colour 0 at direction (0,0,0), which normalises to NaN in the shader.
            set.skybox->SetSun({ 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f }, 0.01f, 0.f);
            set.skybox->SetMoon(0, false, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f }, 0.01f);
            set.skybox->SetStars(0.f, 0.f);
        }
        // Shown whatever the view mode (ApplyPreviewView's WorldCull branch). Logged per
        // discrete shape, not once per session (see m_worldSkyLoggedShape): a scene driven
        // into night after the lane came up must report its moon and stars. The intensities
        // ride along but are not part of the key - they move every frame in a day/night cycle.
        const int skyShape = (sky.valid ? 1 : 0)
                           | ((sky.moonVisible && sky.moonTex) ? 2 : 0)
                           | (sky.starsCubeSrv ? 4 : 0);
        if (skyShape != m_worldSkyLoggedShape)
        {
            m_worldSkyLoggedShape = skyShape;
            LOG_INFO("[PREVIEW] world sky: valid=%d sunI=%.2f moon=%d stars=%.2f "
                     "starsCube=%d", sky.valid ? 1 : 0, sky.sunDiskIntensity,
                     (sky.moonVisible && sky.moonTex) ? 1 : 0, sky.starIntensity,
                     sky.starsCubeSrv ? 1 : 0);
        }
    }

    // The studio shadow pass never runs for this source (ApplyPreviewView
    // hides it), so its CB must not be left pointing at a stale slot.
    if (set.shadow) set.shadow->SetViewCB(nullptr);
    m_studioShadowValid = false;
    return true;
}


// BuildOnePacket: one packet, for the single-material path or one part of an asset
// view, so the shading model derivation, permutation bits, b8 / space3 packing and
// the three per-draw texture handles are written once. `part` indexes the private
// instance table and the custom rings; `matSlot` indexes the private material table
// (part + 1: slot 0 is the engine-reserved neutral record). Do not conflate them:
// that is how slot 0 gets overwritten.
bool PreviewRenderer::BuildOnePacket(const MaterialComponent& mc,
                                     uint64_t matRuntimeKey,
                                     uint32_t meshDescSlot, uint32_t indexCount,
                                     uint32_t customPSID, uint32_t customVSID,
                                     uint32_t part, uint32_t matSlot,
                                     uint32_t frameSlot,
                                     const ShaderReflect::Reflection* reflPS,
                                     bool wantForward,
                                     DrawPacket& dp)
{
    const Renderer::ResolvedMaterialRuntime rt =
        m_owner.ResolvePreviewMaterialRuntime(matRuntimeKey);

    dp = DrawPacket{};
    dp.meshDescriptorIndex = meshDescSlot;
    dp.instanceOffset      = part;
    dp.instanceCount       = 1;
    dp.vertexOrIndexCount  = indexCount;
    // Slot 1 for the single path, 1..N for an asset. Slot 0 is the engine-reserved
    // neutral record, read by the same shaders that dereference matIdx 0 for terrain.
    dp.materialIndex       = matSlot;
    // Which pass draws it: RenderContext::GetDrawList routes on DrawFilter, so this
    // byte picks GBufferPass#2 (deferred) or TransparentPass#2 (the real forward path).
    // The caller resolves `wantForward` from the blend mode and whether a forward
    // instance exists: a translucent material in a graph without TransparentPass#2 (the
    // thumbnail set) is drawn opaque.
    dp.filter              = wantForward ? DrawFilter::Transparent
                                         : DrawFilter::Opaque;
    // THE SHADING MODEL, through the same one-line derivation the main path
    // uses. This single byte is what selects the PBR / NPR / Unlit compile-time
    // variant of Lighting.ps - Aniso, Subsurface, ClearCoat and TwoSided all
    // RefFor() to kPBR and branch at runtime off the material record instead.
    dp.stencilRef          = ShadingStencil::RefFor(mc.customShadingModel);
    dp.viewMask            = 0xFFFFFFFFu;
    dp.renderInMainPass    = 1;
    // PreviewStudioShadowPass skips a packet whose castShadow is 0, as ShadowPass
    // does, and a forward (translucent) subject is not a caster - the engine's shadow path
    // excludes alpha-blended geometry too.
    dp.castShadow          = wantForward ? 0u : 1u;
    // An asset part's placement matrix may have a negative determinant (a mirrored
    // prop, an Unreal import with negative scale), which needs the winding flipped. The
    // single-material path draws identity and keeps 0; BuildDrawList overwrites this from
    // the sign of det(part.world) for asset parts.
    dp.mirrored            = 0;
    dp.prevPosElementBase  = 0xFFFFFFFFu;     // static mesh: no skinned prev-pos ring
    dp.depthPrepassEligible = 0;              // the preview never runs a prepass
    dp.batchHasDissolve    = 0;
    dp.customPSID          = customPSID;
    dp.customVSID          = customVSID;

    // Custom-shader bindings: bound only on a custom draw, the only draw whose PSO can
    // reach b8 / space3. Refusing to draw is the correct failure: an unbound space3 table
    // on a custom shader is a DEVICE_HUNG with no page fault or debug-layer message, so
    // an empty panel and one log line beats a TDR.
    // The gate is either stage: customVSID is resolved independently (ResolveCustomVSID
    // reads customVertexShaderPath), so a VS-only material, or one whose PS failed to
    // compile, has customPSID == 0 and a real customVSID - and a custom vertex shader may
    // declare b8 and space3 like a pixel one (custom_vertex.hlsli; wind_vertex.vs.hlsl
    // does).
    if (dp.customPSID != 0 || dp.customVSID != 0)
    {
        const bool netReady = frameSlot < RHI::kFramesInFlight &&
                              m_customCbvMapped[frameSlot] != nullptr &&
                              m_customCbvVA[frameSlot] != 0 &&
                              m_customTexTableGpu != 0 && m_customTexTableInc != 0;
        if (!netReady)
        {
            ENGINE_CHECK_ONCE(false,
                "PreviewRenderer: custom-shader material (PS %u / VS %u) has no b8 "
                "CBV (%s) or no space3 table (%s); refusing to draw it "
                "rather than leaving a root slot unbound on a shader that reads it.",
                dp.customPSID, dp.customVSID,
                (frameSlot < RHI::kFramesInFlight && m_customCbvVA[frameSlot]) ? "ok" : "MISSING",
                m_customTexTableGpu ? "ok" : "MISSING");
            return false;
        }

        // Space3 floor first, packer second: the packer writes only the slots the
        // reflection declares, and this ring slot is kFramesInFlight frames old.
        PrefillSpace3White(frameSlot, part);
        // Zero the whole slice, not just the reflected cbuffer's size: the packer memsets
        // exactly cbSize bytes, and the rest of the 256 B slice holds the previous material's
        // leftovers, which a shorter layout would read.
        uint8_t* cbvCpu = static_cast<uint8_t*>(m_customCbvMapped[frameSlot])
                        + CustomCbvSliceOffset(part);
        std::memset(cbvCpu, 0, MaterialCBVStore::kSlotStride);

        // The one packer, in its caller-owned destination mode: `slot` stays
        // CustomPackDest::kNoSlot, which keeps it out of m_customMatCbvStore /
        // m_customMatSrvStore and every per-slot array (game-half state keyed on a persistent
        // material slot; the preview has none). reflPS goes in as `customRefl` and customVSID
        // as the VS id, and the packer looks the vertex reflection up itself in the borrowed
        // GBuffer library, as the main path does - so a VS-only material's b8 layout is found.
        // (The caller still resolves reflVS for WriteMaterialSlot's packRefl fallback.)
        const uint64_t blockOff = Space3BlockOffset(frameSlot, part);
        Renderer::CustomPackDest dest;
        dest.cbvCpu      = cbvCpu;
        dest.cbvGpu      = m_customCbvVA[frameSlot] + CustomCbvSliceOffset(part);
        dest.cbvCapacity = MaterialCBVStore::kSlotStride;
        dest.texCpu      = m_customTexTableCpu + blockOff;
        dest.texGpu      = m_customTexTableGpu + blockOff;
        dest.texInc      = m_customTexTableInc;
        m_owner.PackCustomMaterialBindings(dest, &mc, rt, reflPS, customVSID, true);

        // A declared but unpacked b8 cbuffer is a refusal, not a zero fill. outCbBytes
        // reports the reflected size whether or not the pack landed, so "declares nothing"
        // (0) and "could not write it" (non-zero with no VA - a layout larger than the 256 B
        // slice, which the packer reported) are separable. The second must not draw: a zeroed
        // "Tex_*" member is bindless index 0, a real unrelated texture, not the -1 the
        // shader's white fallback keys on.
        if (dest.outCbBytes > 0 && dest.outCbvVA == 0)
        {
            ENGINE_CHECK_ONCE(false,
                "PreviewRenderer: custom-shader material (PS %u / VS %u) declares a "
                "%u B b8 cbuffer the preview's %u B slice cannot hold; refusing to "
                "draw rather than binding an all-zero slice.",
                dp.customPSID, dp.customVSID, dest.outCbBytes,
                static_cast<uint32_t>(MaterialCBVStore::kSlotStride));
            return false;
        }

        // The VA is bound even when nothing was packed (no b8 cbuffer, a legitimate custom
        // shader): root 35 is in the shared root signature and the PSO can reach it; the
        // slice is zeroed above, so what it reads is defined.
        dp.customCbvVA    = m_customCbvVA[frameSlot] + CustomCbvSliceOffset(part);
        dp.customTexTable = m_customTexTableGpu + blockOff;

        // One line per distinct result, so a pack can gate on what the packer
        // actually produced rather than on the picture alone. The resolved count
        // climbs while textures stream, which is why this is not a hard latch.
        if (dest.outCbBytes != m_customPackLoggedBytes ||
            dest.outTexResolved != m_customPackLoggedTex)
        {
            m_customPackLoggedBytes = dest.outCbBytes;
            m_customPackLoggedTex   = dest.outTexResolved;
            LOG_INFO("[PREVIEW] custom bindings packed: b8 %u bytes, %u/%u space3 "
                     "textures resolved", dest.outCbBytes, dest.outTexResolved,
                     GraphicsDX12::kCustomMatTextureSlots);
        }
    }

    // Permutation: a deliberate subset of Renderer::ApplyBlendMode, which cannot be
    // called here - it reads FrameViewWrite() (the game half's view) and routes the
    // transparent / water modes at passes this graph lacks. The two agree on everything
    // that reaches the GBuffer-shaped opaque path, the domain the borrowed ShaderLibrary
    // is valid for.
    PermutationKey perm{};
    perm.Set(PermutationKey::DOUBLE_SIDED,
             (mc.flags & MaterialComponent::DOUBLE_SIDED) != 0);
    switch (mc.userBlendMode)
    {
    case BlendMode::Masked:
    case BlendMode::Dithered:
        perm.Set(PermutationKey::ALPHA_TEST, true);
        break;
    case BlendMode::Opaque:
        perm.Set(PermutationKey::ALPHA_TEST, mc.IsAlphaTestEnabled());
        break;
    case BlendMode::Additive:
        // The forward blend bits: the subset of ApplyBlendMode that
        // TransparentPass::BuildPSODesc reads.
        perm.Set(PermutationKey::ALPHA_BLEND,    true);
        perm.Set(PermutationKey::ADDITIVE_BLEND, wantForward);
        perm.Set(PermutationKey::ALPHA_TEST,     mc.IsAlphaTestEnabled());
        break;
    case BlendMode::Premultiplied:
        perm.Set(PermutationKey::ALPHA_BLEND,        true);
        perm.Set(PermutationKey::PREMULTIPLIED_BLEND, wantForward);
        perm.Set(PermutationKey::ALPHA_TEST,          mc.IsAlphaTestEnabled());
        break;
    case BlendMode::Multiply:
        perm.Set(PermutationKey::ALPHA_BLEND,   true);
        perm.Set(PermutationKey::MULTIPLY_BLEND, wantForward);
        perm.Set(PermutationKey::ALPHA_TEST,     mc.IsAlphaTestEnabled());
        break;
    case BlendMode::Transmissive:
        perm.Set(PermutationKey::ALPHA_BLEND,        true);
        perm.Set(PermutationKey::TRANSMISSIVE_BLEND, wantForward);
        perm.Set(PermutationKey::ALPHA_TEST,         mc.IsAlphaTestEnabled());
        break;
    default:
        // Alpha and water. Water has no second WaterSurfacePass instance and never reaches
        // Lighting.ps, so it keeps the opaque route; plain Alpha gets ALPHA_BLEND with none of
        // the three mode bits (TransparentPass's default SRC_ALPHA path).
        perm.Set(PermutationKey::ALPHA_BLEND, wantForward);
        perm.Set(PermutationKey::ALPHA_TEST,  mc.IsAlphaTestEnabled());
        break;
    }
    dp.permutation = perm;
    // A masked custom material's studio shadow runs its own hook, as the game's shadow
    // passes do (Renderer::ShadowHookEnabled, RenderGraph/RenderPass/ShadowCasterShaders.h),
    // so the panel shows the shadow the scene will cast. A forward subject casts nothing
    // here (castShadow above), so the bit is moot for it.
    dp.shadowHookPS = (dp.customPSID != 0 && perm.Has(PermutationKey::ALPHA_TEST) &&
                       m_owner.ShadowHookEnabled()) ? 1u : 0u;

    // Per-draw textures, resolved by key, so a material that belongs to no entity still
    // gets its textures. A zero handle is not an error: GBufferPass substitutes its 1x1
    // white / flat-normal, which is what a still-streaming texture should look like.
    dp.texBaseColor  = rt.EngineTex(MaterialComponent::BASECOLORMAP).gpuHandle;
    dp.texSurfaceMap = rt.EngineTex(MaterialComponent::SURFACEMAP).gpuHandle;
    dp.texNormalMap  = rt.EngineTex(MaterialComponent::NORMALMAP).gpuHandle;
    return true;
}

// WantsForward: is this material drawn by TransparentPass#2 rather than
// GBufferPass#2? Needs both a forward blend mode and a forward instance in this graph
// (the thumbnail set has none; its translucent tiles are drawn opaque).
bool PreviewRenderer::WantsForward(const MaterialComponent& mc,
                                   uint64_t matRuntimeKey,
                                   const GraphSet& set) const
{
    const bool forwardBlend =
        mc.userBlendMode == BlendMode::Alpha         ||
        mc.userBlendMode == BlendMode::Additive      ||
        mc.userBlendMode == BlendMode::Premultiplied ||
        mc.userBlendMode == BlendMode::Multiply      ||
        mc.userBlendMode == BlendMode::Transmissive;
    // Water never reaches Lighting.ps and has no second WaterSurfacePass instance, so a
    // water material stays on the opaque route whatever its blend mode. And it says so:
    // the panel shows the downgrade (MaterialPreviewWidget's amber line), and this is the
    // engine-side witness the corpus gates, emitted inside the deciding branch so line and
    // behaviour cannot disagree. Logged once per (material, blend), not per call:
    // WantsForward runs several times per frame (once per part), a material edit that
    // changes water or blend is worth a new line, and keying on the water / not-water edge
    // would flood for an asset mixing water and non-water parts.
    const bool water = mc.IsWaterShadingModel();
    const uint32_t blend = static_cast<uint32_t>(mc.userBlendMode);
    if (water)
    {
        bool known = false;
        for (const WaterDowngradeKey& k : m_waterDowngradeLogged)
            if (k.matRuntimeKey == matRuntimeKey && k.blend == blend)
            { known = true; break; }
        if (!known)
        {
            m_waterDowngradeLogged.push_back(
                WaterDowngradeKey{ matRuntimeKey, blend });
            LOG_INFO("[PREVIEW] water material downgraded: drawn on the OPAQUE "
                     "route - no WaterSurfacePass#2, no refraction, no SSR "
                     "(blend %u, forward instance %s)", blend,
                     set.transparent ? "present" : "absent");
        }
    }
    return forwardBlend && set.transparent != nullptr && !water;
}

bool PreviewRenderer::BuildDrawList(const Preview::Request& req, uint32_t frameSlot,
                                    const GraphSet& set,
                                    const ShaderReflect::Reflection* reflPS,
                                    bool wantForward)
{
    m_packets.clear();

    // The asset path: one packet per resolved part, each with its own instance row,
    // material slot and custom-shader slice. An early return rather than branches through
    // the body below: every single-material field (mat, matRuntimeKey, customPSID,
    // meshDescSlot) is meaningless here, and the two share BuildOnePacket.
    if (IsAssetDraw(req))
    {
        // An asset view with no parts draws nothing - never the primitive sphere, which
        // would read as "the prefab looks like this". Reachable: an .ipfb placing only
        // primitives (DemoLoot), or a mesh library not streamed in yet.
        // Except when the view hid them: a part mask that leaves no part (the Skeleton
        // Editor's Show > Mesh off, a Mesh Editor isolate hiding every section) asks for the
        // backdrop alone, and that picture must be rendered - a bail would freeze the last
        // picture while an orbit moves the panel's overlay. An empty draw list is a valid
        // frame: GBufferPass (the HDR clear) and LightingPass always run, the sky draws the
        // backdrop, and every per-part write below walks req.parts.
        if (req.parts.empty())
        {
            const bool hidden = req.view.partMask != ~0ull;
            if (hidden && m_assetLoggedPackets != 0)
            {
                m_assetLoggedPackets = 0;   // the next shape with parts logs its line again
                LOG_INFO("[PREVIEW] asset hidden: every part masked out by the view - the backdrop alone "
                         "path=%s", req.view.assetPath.c_str());
            }
            return hidden;
        }

        uint32_t built = 0;
        for (uint32_t i = 0; i < req.parts.size() &&
                             i < Preview::kMaxPreviewParts; ++i)
        {
            const Preview::AssetPart& part = req.parts[i];
            if (part.indexCount == 0 || part.meshDescSlot == RHI::kInvalidBufferIndex)
                continue;

            // Per part, not per asset: a prefab mixes an opaque body with an alpha-blended hair
            // card, and each must reach the right pass. Whether TransparentPass#2 runs at all is
            // per graph, so the caller ORs the parts into wantForward.
            const bool partForward = WantsForward(part.mat, part.matRuntimeKey, set);

            const ShaderReflect::Reflection* const partReflPS =
                part.customPSID ? m_owner.LookupCustomShaderReflection(part.customPSID, &part.mat)
                                : nullptr;

            DrawPacket dp{};
            if (!BuildOnePacket(part.mat, part.matRuntimeKey,
                                part.meshDescSlot, part.indexCount,
                                part.customPSID, part.customVSID,
                                /*part=*/i, /*matSlot=*/i + 1, frameSlot,
                                partReflPS, partForward, dp))
                continue;   // BuildOnePacket has already reported why

            // Mirrored, from the placement's own determinant. GBufferPass declares
            // frontCounterClockwise, so a negative-determinant placement (an Unreal import
            // mirrored along one axis - the "-1 scale" half of a symmetric street) reverses the
            // winding and renders inside out. The main path derives the byte the same way
            // (Renderer_Scene's mirrored candidate field), and it is PSO state, so it belongs on
            // the packet. det of the 4x4 equals det of the upper 3x3 for an affine matrix (last
            // column 0,0,0,1), which every placement here is.
            {
                const XMMATRIX w =
                    XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(part.world));
                const float det = XMVectorGetX(XMMatrixDeterminant(w));
                dp.mirrored = (det < 0.f) ? 1u : 0u;
            }

            m_packets.push_back(dp);
            ++built;
        }

        // Grouped by filter, and not for looks: RenderContext::GetDrawList does not
        // filter, it returns the first contiguous run whose `filter` matches (the main path's
        // SortAndBatch sorts on filter first). A part-ordered list interleaves filters, so
        // GBufferPass#2 would get only the packets before the first translucent one - a prefab
        // with one alpha-blended part in the middle draws as a floating head while `packets=`
        // still counts every packet.
        // Within a depth-sorted filter, back to front: alpha blending is order dependent, and
        // authored order is a stable wrong picture no assertion catches. Same painter's key as
        // Renderer_Scene's SortAndBatch (depth descending), with the part index as the
        // tiebreaker since a part has no entity. Depth is measured along the studio camera's
        // forward from its eye, through PreviewStudioRig::StudioCameraBasis (the basis
        // WritePerViewCB uses), so an orbit re-sorts the parts.
        // Stable: within a non-depth-sorted filter parts keep their authored (.iscn F-line)
        // order, which keeps the opaque picture reproducible.
        {
            float eye[3] = { 0.f, 0.f, 0.f }, fwd[3] = { 0.f, 0.f, 1.f };
            Preview::StudioCameraBasis(req.view.yawOffset, req.view.pitchOffset,
                                       req.view.distanceScale, eye, fwd);
            // instanceOffset IS the part index (BuildOnePacket set it), so the
            // packet can find its own centre after the sort has moved it.
            std::vector<float> depth(req.parts.size(), 0.f);
            for (std::size_t p = 0; p < req.parts.size(); ++p)
            {
                const Preview::AssetPart& ap = req.parts[p];
                const float d = (ap.centre[0] - eye[0]) * fwd[0]
                              + (ap.centre[1] - eye[1]) * fwd[1]
                              + (ap.centre[2] - eye[2]) * fwd[2];
                // Sanitised at the source: a non-finite centre is reachable (a corrupt library
                // AABB makes the merged bounds non-finite, the auto-fit is skipped and the centres
                // stay NaN), and a NaN key breaks the comparator's strict weak ordering -
                // std::stable_sort may then read out of bounds. -FLT_MAX sorts it first in the
                // descending key (farthest), the harmless end for a part nobody can place.
                depth[p] = std::isfinite(d) ? d : -FLT_MAX;
            }
            std::stable_sort(m_packets.begin(), m_packets.end(),
                [&depth](const DrawPacket& a, const DrawPacket& b)
                {
                    if (a.filter != b.filter)
                        return static_cast<uint8_t>(a.filter) <
                               static_cast<uint8_t>(b.filter);
                    if (!IsDepthSortedFilter(a.filter)) return false;
                    const float da = (a.instanceOffset < depth.size())
                                   ? depth[a.instanceOffset] : 0.f;
                    const float db = (b.instanceOffset < depth.size())
                                   ? depth[b.instanceOffset] : 0.f;
                    if (da != db) return da > db;          // painter's order
                    return a.instanceOffset < b.instanceOffset;
                });
        }

        // One line when the shape changes, with both numbers: `packets` says the
        // multi-packet path ran, `materials` that each part resolved its own material record.
        // A pack gates on both.
        if (built != m_assetLoggedPackets ||
            req.partMaterialCount != m_assetLoggedMaterials)
        {
            m_assetLoggedPackets   = built;
            m_assetLoggedMaterials = req.partMaterialCount;
            LOG_INFO("[PREVIEW] asset: packets=%u materials=%u parts=%u dropped=%u "
                     "path=%s", built, req.partMaterialCount,
                     static_cast<uint32_t>(req.parts.size()), req.partsDropped,
                     req.view.assetPath.c_str());
        }
        return built > 0;
    }

    // The mesh override, or the primitive. ExtractRequests resolved ViewDesc::meshPath
    // on the game half; what arrives is a slot and an index count, or the unresolved pair.
    // The fallback is not an error: a failed load, a library still streaming and no
    // override all draw the primitive. Primitives are persistent
    // (MeshManager::InitPrimitives, persistent=true), so their descriptor slots survive
    // every world reload.
    uint32_t drawDescSlot = req.meshDescSlot;
    uint32_t drawIndexCount = req.meshIndexCount;
    if (drawIndexCount == 0 || drawDescSlot == RHI::kInvalidBufferIndex)
    {
        const MeshManager::GPUMesh& mesh =
            m_owner.GetMeshManager().GetPrimitive(PrimitiveIndexFor(req.view.shape));
        if (mesh.indexCount == 0 || mesh.meshDescSlot == RHI::kInvalidBufferIndex)
            return false;
        drawDescSlot   = mesh.meshDescSlot;
        drawIndexCount = mesh.indexCount;
    }

    DrawPacket dp{};
    if (!BuildOnePacket(req.mat, req.matRuntimeKey, drawDescSlot, drawIndexCount,
                        req.customPSID, req.customVSID,
                        /*part=*/0, /*matSlot=*/kPreviewMatSlot, frameSlot,
                        reflPS, wantForward, dp))
        return false;

    m_packets.push_back(dp);
    return true;
}

// ProcessQueue: render half, Renderer::Render Phase 4.3. Records up to
// Preview::kMaxPreviewsPerFrame pictures in a stated priority order:
//   1. the dirty Synthetic view (material inspector, asset preview): its dirtiness is
//      a user edit someone is watching;
//   2. the WorldCull views (camera, split cells, PiP), round-robin - dirty every
//      frame by nature, so without rotation the lowest id would starve the rest;
//   3. the thumbnail FIFO's head, last: a tile is speculative work.
// The schedule:
//   * pickBest runs once per lane and picks at most one candidate each: lane 0
//     (Synthetic) and lane 1 (WorldCull). A lane is a set of constant-buffer rings
//     with one FrameCB slot per frame, so two records through it in one frame both
//     read whichever wrote last.
//   * The pick is priority, then oldest lastTickFrame, then lowest id. Every record
//     stamps lastTickFrame, so N views at tick 1 land on frames i, i+N, i+2N.
//   * A bail costs the view its turn (lastTickFrame = m_previewFrame), or a
//     persistently bailing view would stay oldest and starve the other N-1.
//   * A view whose tickIntervalFrames has not come round is not a candidate (gated
//     in ExtractOneRequest).
// RecordRequest's ENGINE_CHECK on a second record through one lane is a backstop: the
// scheduler never asks for one, and Release must not log a failure for a state only
// the scheduler could produce. m_live and m_thumb share lane 0, so a Synthetic
// picture and a tile never record in the same frame.
// Every picture is one command list, chained: each record takes the previous list as
// its colorLastCL, so the caller's single AddCommandListDependency(primaryCL,
// previewCL) covers all of them.
RHI::CommandList PreviewRenderer::ProcessQueue(RHI::CommandList colorLastCL)
{
    m_viewsRendered = 0;
    m_viewDraws     = 0;
    ++m_previewFrame;
    // Republished for SetPriorityView, which runs on the UI half and must not read
    // m_previewFrame (plain storage owned by this half). The preference stamps itself
    // with this value and the expiry below compares the two.
    m_previewFramePub.store(m_previewFrame, std::memory_order_relaxed);
    for (uint32_t& lr : m_laneRenders) lr = 0;

    RHI::CommandList tail{};          // the newest list recorded this frame
    RHI::CommandList chain = colorLastCL;

    // The cadence line's denominator: preview frames with at least one camera view
    // live, counted even on frames nothing was drawn.
    if (m_worldCullLive != 0) ++m_worldCullFrames;

    // ---- 1. the Synthetic lane, then 2. the camera views ----
    // At most one candidate per lane (see above): with N camera views on lane 1 and two
    // Synthetic panels (material inspector, asset preview) on lane 0 both collisions are
    // reachable. m_priorityView is the hovered split cell, and the preference is a double
    // share, not a veto: the hovered cell is preferred only when it did not render on the
    // previous preview frame, so it gets every other turn (~50% of frames for one hovered
    // cell of four at tick 1, the others ~17% each). An unconditional preference would
    // freeze the other cells on their last picture.
    uint32_t priority = m_priorityView.load(std::memory_order_relaxed);
    // The preference expires: valid for the preview frame it was pushed on plus one. A
    // panel re-asserts it every frame it draws (including "nothing hovered"), so this
    // only drops a preference whose panel stopped drawing (collapsed tab, F11, closed
    // cell); left latched, the next tenant of that slot would inherit a double share.
    if (priority != Preview::kInvalidView &&
        m_priorityFrame.load(std::memory_order_relaxed) + 1 < m_previewFrame)
    {
        priority = Preview::kInvalidView;
        m_priorityView.store(Preview::kInvalidView, std::memory_order_relaxed);
    }
    if (priority < Preview::kMaxPreviews &&
        m_targets[priority].lastTickFrame + 2 > m_previewFrame)
        priority = Preview::kInvalidView;
    auto pickBest = [&](Preview::Source src) -> uint32_t
    {
        uint32_t best = Preview::kInvalidView;
        for (uint32_t i = 0; i < Preview::kMaxPreviews; ++i)
        {
            const Target& t = m_targets[i];
            if (!t.live || !t.req.valid || !t.req.inFlight) continue;
            // A suspended view is not a candidate. ExtractOneRequest already refuses to arm
            // one; this also covers a view refused after its hand-over was armed, which would
            // otherwise record once at the old size.
            if (m_viewSizeRefused[i]) continue;
            // The slot's lane.
            if (m_slotSource[i] != src) continue;
            if (best == Preview::kInvalidView) { best = i; continue; }
            if (i == priority) { best = i; continue; }
            if (best == priority) continue;
            if (t.lastTickFrame < m_targets[best].lastTickFrame) best = i;
        }
        return best;
    };
    uint32_t order[2];
    uint32_t orderCount = 0;
    const uint32_t synth = pickBest(Preview::Source::Synthetic);
    if (synth != Preview::kInvalidView) order[orderCount++] = synth;
    const uint32_t world = pickBest(Preview::Source::WorldCull);
    if (world != Preview::kInvalidView) order[orderCount++] = world;

    for (uint32_t oi = 0; oi < orderCount; ++oi)
    {
        if (m_viewsRendered >= Preview::kMaxPreviewsPerFrame) break;
        const uint32_t viewId = order[oi];
        Target* lt = &m_targets[viewId];
        // The slot's lane, the field pickBest selected on.
        const bool isWorld = (SlotSource(viewId) == Preview::Source::WorldCull);
        GraphSet& set = isWorld ? m_world : m_live;
        std::vector<DrawPacket>& packets = isWorld ? m_worldPackets : m_packets;

        // Snapshot; everything below reads it. A request landing between ExtractRequests
        // and here moves the latch's generation, and publishing the latch's generation would
        // stamp the new generation "done" on a picture drawn from the old description; the
        // Result carries what was drawn. Not const on purpose: WriteMaterialSlot const_casts
        // the material to refresh its sRGB -> linear cache (RefreshLinearCache clears
        // GPU_LINEAR_CACHE_DIRTY in place), which is undefined behaviour on an object
        // declared const.
        Preview::Request req = lt->req;

        const RHI::CommandList cl =
            RecordRequest(set, *lt, req, lt->req.inFlight, chain, nullptr, 0);
        if (!cl.IsValid())
        {
            // A bail still costs this view its turn: the scheduler picks the oldest-rendered
            // candidate, so a view that bails persistently would be re-picked every frame and the
            // other camera views on the lane would never render. It does not delay the retry:
            // bail() cleared inFlight and left the generation ahead, and ExtractOneRequest's tick
            // gate applies only when they are equal, so the next extract re-sends at once; only
            // the order changes. m_worldViewRenders is not touched: the split line counts
            // pictures, not attempts.
            lt->lastTickFrame = m_previewFrame;
            continue;
        }
        // Join the chain as soon as a list exists, empty packet list or not: a recorded
        // list left out of `tail` and `chain` would never be submitted or depended on, and its
        // result never published. An empty list is a legitimate picture (a camera pointed at
        // open sky draws the sky).
        tail  = cl;
        chain = cl;

        // The snapshot, not the latch: see above.
        lt->result = Preview::Result{ req.viewId, req.generation, true };
        lt->lastTickFrame = m_previewFrame;
        m_lastRenderedView.store(req.viewId, std::memory_order_relaxed);
        // Guarded: a WorldCull view of open sky records a real picture with an empty
        // packet list. 0 is not a stencil code, which is what "nothing drawn" means here.
        m_lastRenderedStencil.store(
            packets.empty() ? 0u : static_cast<uint32_t>(packets[0].stencilRef),
            std::memory_order_relaxed);
        m_lastRenderedForward.store(
            (!packets.empty() && packets[0].filter == DrawFilter::Transparent) ? 1u : 0u,
            std::memory_order_relaxed);
        ++m_viewsRendered;
        // Monotonic twin (see ViewsRenderedTotal()), incremented only where a picture is
        // known to be recorded, so "the edit produced a re-render" cannot be satisfied by a
        // bail. Thumbnails are not counted here (they have their own counter):
        // -preview_edit_at latches this number at the edit and asserts the next increment is
        // the live re-render.
        m_viewsRenderedTotal.fetch_add(1, std::memory_order_relaxed);
        if (req.viewId < Preview::kMaxPreviews)
            m_viewRenderCount[req.viewId].fetch_add(1, std::memory_order_relaxed);
        m_viewDraws += static_cast<uint32_t>(packets.size());

        if (isWorld)
        {
            // One line per change in the count, not per frame. The pack asserts "N > 0 and
            // N <= M": a secondary view that culls everything and one that culls nothing are both
            // bugs no mean-luma band shows.
            const uint32_t drew = static_cast<uint32_t>(packets.size());
            // The per-view cadence accounting the shutdown line reports.
            const uint32_t vid = (req.viewId < Preview::kMaxPreviews)
                               ? req.viewId : 0u;
            // This view's own counts (the line names view=%u beside them).
            const uint32_t cand = m_worldCullCandidates[vid].load(std::memory_order_relaxed);
            const uint32_t sec  = m_worldCullSecondary[vid].load(std::memory_order_relaxed);
            // The conservative branch's four leafless counters, same slot.
            const uint32_t lfPrim =
                m_worldCullLeaflessPrim[vid].load(std::memory_order_relaxed);
            const uint32_t lfOther =
                m_worldCullLeaflessOther[vid].load(std::memory_order_relaxed);
            const uint32_t lfPrimKept =
                m_worldCullLeaflessPrimKept[vid].load(std::memory_order_relaxed);
            const uint32_t lfOtherKept =
                m_worldCullLeaflessOtherKept[vid].load(std::memory_order_relaxed);
            ++m_worldViewRenders[vid];
            ++m_worldCullRenders;
            m_worldViewLastDraws[vid] = drew;
            m_worldViewLastSec[vid]   = sec;
            // One line per change in the count or the size: the size is the only per-view
            // evidence that a resize reached the raster (on a scene whose draw count never moves,
            // a resized view and a refused one would otherwise log the same). The two ...Kept
            // counters are in the predicate too: the runner reads the last matching line, and a
            // scene whose draw count never moves would leave a frame-1 value standing. See the
            // members.
            if (!m_worldFirstLogged[vid] || drew != m_worldCullLoggedDraws[vid] ||
                lt->w != m_worldViewLoggedW[vid] || lt->h != m_worldViewLoggedH[vid] ||
                lfPrimKept  != m_worldLoggedLeaflessPrimKept[vid] ||
                lfOtherKept != m_worldLoggedLeaflessOtherKept[vid])
            {
                m_worldFirstLogged[vid]     = true;
                m_worldCullLoggedDraws[vid] = drew;
                m_worldViewLoggedW[vid]     = lt->w;
                m_worldViewLoggedH[vid]     = lt->h;
                m_worldLoggedLeaflessPrimKept[vid]  = lfPrimKept;
                m_worldLoggedLeaflessOtherKept[vid] = lfOtherKept;
                // name=value pairs, not prose: logLineFields reads bounds off one line with a
                // name=number scan. `draws`, `candidates` and `secondary` are what the packs gate on;
                // the rest identify the view. `secondary` counts kept packets from the secondary-view
                // lists - geometry the main camera cannot see - so a PiP pointed away from the player
                // proves it draws its own frustum. Append new fields, never reorder: the runner does
                // not care about order, but a reordered line makes the diff unreadable.
                LOG_INFO("[PREVIEW] world cull: draws=%u candidates=%u "
                         "secondary=%u view=%u w=%u h=%u hdrSlot=%u viewBit=%u "
                         "leaflessPrim=%u leaflessOther=%u leaflessPrimKept=%u "
                         "leaflessOtherKept=%u",
                         drew, cand, sec, req.viewId, lt->w, lt->h, lt->hdrSlot,
                         req.viewBit, lfPrim, lfOther, lfPrimKept, lfOtherKept);
            }
            continue;
        }
        {
        // Once per size, and load-bearing twice: packs assert this line (a per-frame
        // counter cannot prove "at least once" through frames.jsonl's fieldEquals, which
        // compares every frame), and the second emission - after a resize recreated the graph
        // textures, the tone-map output and the HDR slot - is the only evidence that the
        // recompile produced a picture. Both variants start with "[PREVIEW] first render".
        // `stencil` is asserted exactly by preview_material / preview_material_npr (PBR=1 /
        // NPR=2): the shading model is a compile-time permutation of Lighting.ps selected by
        // this byte, and a wrong one renders a plausible picture no image gate catches.
        if (!m_firstRenderLogged || lt->w != m_loggedW || lt->h != m_loggedH)
        {
            const bool first = !m_firstRenderLogged;
            m_firstRenderLogged = true;
            m_loggedW = lt->w;
            m_loggedH = lt->h;
            // The custom shaders by name (a dynamic id is a hash); 0 = none.
            LOG_INFO("[PREVIEW] first render%s: view %u %ux%u hdrSlot %u gen %u "
                     "shape %u stencil %u customPS %s customVS %s texReady %d",
                     first ? "" : " at a new size",
                     req.viewId, lt->w, lt->h, lt->hdrSlot, req.generation,
                     static_cast<unsigned>(req.view.shape),
                     m_packets.empty()
                         ? 0u : static_cast<unsigned>(m_packets[0].stencilRef),
                     DynamicShaderRegistry::Get().Label(req.customPSID).c_str(),
                     DynamicShaderRegistry::Get().Label(req.customVSID).c_str(), req.texturesReady ? 1 : 0);
        }
        }
    }

    // ---- 3. The thumbnail FIFO's turn ----
    // An invalid list, not colorLastCL, means "nothing recorded": the caller must not
    // splice the preview into its colorLastCL chain (see the header), and returning the
    // input unchanged would invite exactly that.
    if (m_viewsRendered >= Preview::kMaxPreviewsPerFrame) return tail;
    // And lane 0 must still be free: with kMaxPreviewsPerFrame == 2 a frame that
    // rendered one picture still has budget, but if it was a Synthetic view, lane 0 -
    // m_live's and m_thumb's shared rings - is spent, and a tile would make both
    // dispatches read whichever wrote last. Refusing here keeps at most one of {live,
    // thumbnail} plus the world view per frame, so the record-side ENGINE_CHECK never
    // fires in Release.
    if (m_laneRenders[kLaneLive] != 0) return tail;
    if (!m_thumbReq.valid || !m_thumbReq.inFlight) return tail;

    Preview::Request treq = m_thumbReq;
    const uint64_t   tkey = m_thumbAssetKey;
    const uint32_t   tgen = treq.generation;
    // `chain`, not colorLastCL: a live picture may already have been recorded
    // this frame and the tile's list must sit AFTER it, or the two lists race
    // for the same graph textures with no fence between them.
    const RHI::CommandList tcl =
        RecordRequest(m_thumb, m_thumbTarget, treq, m_thumbReq.inFlight,
                      chain, &m_thumbTile, tkey);
    // A record early-out cleared inFlight and left the request valid, so the next
    // ExtractRequests re-sends it. It must not publish a result: the editor clears its
    // dirty flag only at apply, and applying a picture nobody drew drops the asset from
    // the queue for the session.
    if (!tcl.IsValid()) return tail;
    // A render whose COPY was skipped (an HDR output mode) leaves the tile
    // zero-filled; publishing success would mark a black square ready and cache
    // it. m_thumbDisabled is latched in that branch, so the queue drains and
    // this cannot repeat.
    if (!m_thumbTileCopied) return tcl;

    m_thumbResult = Preview::ThumbResult{ tkey, tgen, true };
    m_thumbsRenderedTotal.fetch_add(1, std::memory_order_relaxed);
    if (!m_thumbFirstLogged)
    {
        m_thumbFirstLogged = true;
        LOG_INFO("[PREVIEW] first thumbnail render: key %016llx %ux%u hdrSlot %u "
                 "gen %u stencil %u customPS %s",
                 static_cast<unsigned long long>(tkey),
                 m_thumbTarget.w, m_thumbTarget.h, m_thumbTarget.hdrSlot, tgen,
                 m_packets.empty()
                     ? 0u : static_cast<unsigned>(m_packets[0].stencilRef),
                 DynamicShaderRegistry::Get().Label(treq.customPSID).c_str());
    }
    return tcl;
}

// The shared record body. See the header for the contract; @p inFlight is the
// caller's latch flag and is cleared on every early-out.
RHI::CommandList PreviewRenderer::RecordRequest(GraphSet& set, Target& target,
                                                Preview::Request& req,
                                                bool& inFlight,
                                                RHI::CommandList colorLastCL,
                                                const RHI::Texture* tile,
                                                uint64_t tileAssetKey)
{
    Target* t = &target;
    // Early-out clears inFlight: a record that never flew must not look like it did,
    // or the lane sits in flight until something else disturbs it. It must not clear the
    // request itself - the game half owns `valid`. Retry is automatic: the generation is
    // still ahead of lastRenderedGeneration, so the next ExtractRequests re-sends.
    auto bail = [&]() -> RHI::CommandList { inFlight = false; return RHI::CommandList{}; };

    if (t->hdrSlot == 0 || t->w == 0)        return bail();
    if (!t->toneMap || !t->toneMapInited)    return bail();
    // The neutral pair is a HARD precondition, not a nicety: with a 0 handle
    // SetComputeDescriptorTable silently leaves the root slot alone and the
    // tone map reads whatever was there (nothing, on a fresh list).
    if (!m_neutralReady)                     return bail();

    // The preview's own HDR slot must have a live SRV, or the tone map would be
    // dispatched against handle 0 in the same way.
    if (m_gfx.GetHdrTargetSrvGpuHandle(t->hdrSlot) == 0) return bail();

    // "The editor is serial" is not the condition: RenderGraph records in parallel at
    // passCount >= 4 whatever the render mode, and PushHdrTarget hands over a shared state
    // tracker, so this must run outside every graph's Phase B. Phase 4.3 runs after
    // m_graph.Execute returned; the check states it.
    if (!ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == 0,
            "PreviewRenderer::ProcessQueue entered at HDR scope depth %d - the "
            "preview target hand-over must happen outside every push scope.",
            m_gfx.HdrScopeDepth()))
        return bail();

    // The per-frame scheduling cap, enforced: a broken scheduler (two dispatches
    // sharing one FrameCB slot) produces no crash and no debug-layer message, only
    // intermittent flicker.
    if (!ENGINE_CHECK_ONCE(m_viewsRendered < Preview::kMaxPreviewsPerFrame,
            "PreviewRenderer: %u views already rendered this frame, cap is %u.",
            m_viewsRendered, Preview::kMaxPreviewsPerFrame))
        return bail();

    // And the per-lane cap. A lane is a set of constant-buffer rings (perViewCB /
    // lightCB / shadowViewCB / customCbv / the space3 block / the cluster buffers) and a
    // FrameCB has one slot per frame in flight, so a second record through one lane in a
    // frame makes both dispatches read whichever wrote last. Counted per lane, not per
    // GraphSet: m_live and m_thumb are two GraphSets on one lane. Raising
    // kMaxPreviewsPerFrame without adding a lane trips here.
    if (!ENGINE_CHECK_ONCE(set.lane < kLaneCount && m_laneRenders[set.lane] == 0,
            "PreviewRenderer: a second record through lane %u (GraphSet '%s') in "
            "one frame would share its single FrameCB slot (T1). Refused.",
            set.lane, set.suffix ? set.suffix : "?"))
        return bail();

    // The graph is built in the game half's serial window (ServiceCompile); executing
    // it uncompiled would make RenderGraph compile itself here, on the render half - so
    // bail, never build lazily.
    if (!set.compiled || !set.gbuffer || !set.lighting) return bail();
    if (set.w != t->w || set.h != t->h)                 return bail();   // recompile owed
    if (!m_sceneBuffersReady)                     return bail();

    // ---- The scene ----
    // Per-frame CPU writes into this frame's ring slots, then the graph bindings, all
    // before the push and none touching the device's HDR state.
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    if (frameSlot >= RHI::kFramesInFlight) return bail();

    // The lane is spent from here on: counted at the first constant-buffer write.
    // Everything above is a precondition touching no ring; everything below writes one
    // (WriteWorldPerViewCB / WriteWorldLightCB, or BuildDrawList's b8 / space3 block, then
    // the instance, material and per-view rings). Counting it later - past the
    // PushHdrTarget refusal, the HDR-scope check, BeginCommandList exhaustion or
    // RecordWorldCullSetup's missing-buffer bail, all after writes - would admit a second
    // record through the lane in the same frame, sharing one FrameCB slot (intermittent
    // flicker, no error).
    if (set.lane < kLaneCount) ++m_laneRenders[set.lane];

    // The fork: a WorldCull view shares only the preconditions, the HDR push/pop
    // bracket and the tone-map tail with the synthetic path; its setup lives in
    // RecordWorldCullSetup.
    const bool worldCull = (set.source == Preview::Source::WorldCull);
    bool wantForward = false;
    if (worldCull)
    {
        if (!RecordWorldCullSetup(set, req)) return bail();
    }
    else
    {

    // The two custom-shader reflections, resolved once and used by BuildDrawList's b8
    // writer and the material packer below. The fallback matches the main packer's
    // (`packRefl = reflPS ? reflPS : reflVS`): DXC strips each stage's reflection to the
    // members that stage uses, so a VS-only material (or one whose PS has not compiled)
    // has its b8 layout only in the vertex reflection, and a null customRefl would pack
    // it as declaring no parameters.
    const ShaderReflect::Reflection* const reflPS =
        req.customPSID ? m_owner.LookupCustomShaderReflection(req.customPSID, &req.mat) : nullptr;
    const ShaderReflect::Reflection* const reflVS =
        req.customVSID ? m_owner.LookupCustomShaderReflection(req.customVSID, &req.mat) : nullptr;
    const ShaderReflect::Reflection* const packRefl = reflPS ? reflPS : reflVS;

    // Deferred or forward: one decision, read by the draw filter, the permutation
    // bits, the shadow-caster bit and the view's Transparent flag (ApplyPreviewView).
    // Needs a forward blend mode and a TransparentPass#2 in this graph (the thumbnail set
    // has none). For an asset it is an OR over the parts: the draw filter is per packet,
    // but the view's Transparent flag is per graph, so the pass must run whenever any
    // part needs it.
    wantForward = WantsForward(req.mat, req.matRuntimeKey, set);
    if (IsAssetDraw(req))
    {
        wantForward = false;
        for (const Preview::AssetPart& p : req.parts)
            if (WantsForward(p.mat, p.matRuntimeKey, set)) { wantForward = true; break; }
    }

    // One line per route change: the only evidence separating "the forward instance
    // drew it" from "the opaque route drew it plausibly" - against a black backdrop the
    // blend difference is a brightness change no luma band can attribute.
    if (wantForward != m_forwardLogged || !m_forwardEverLogged)
    {
        m_forwardEverLogged = true;
        m_forwardLogged     = wantForward;
        LOG_INFO("[PREVIEW] draw route: %s (blend %u, forward instance %s)",
                 wantForward ? "TransparentPass#2 (forward)" : "GBufferPass#2 (deferred)",
                 static_cast<unsigned>(req.mat.userBlendMode),
                 set.transparent ? "present" : "absent");
    }

    if (!BuildDrawList(req, frameSlot, set, reflPS, wantForward)) return bail();

    {
        // One instance row per packet. Transposed, because the VS does mul(pos, world)
        // with row vectors; prevWorld == world so velocity is exactly zero; dissolveParams
        // stays zero because the PS branches on dissolveParams.x > 0 and a stray value burns
        // holes.
        auto* insts = static_cast<GPUInstanceData*>(m_instanceMapped[frameSlot]);

        if (IsAssetDraw(req))
        {
            // The placement matrices the game half composed, already fitted into the studio
            // camera's unit box. A packet reads row instanceOffset (= the part index), so this
            // walks parts, not packets; a part that failed to build leaves a row nothing indexes.
            for (uint32_t i = 0; i < req.parts.size() &&
                                 i < Preview::kMaxPreviewParts; ++i)
            {
                const Preview::AssetPart& p = req.parts[i];
                GPUInstanceData& inst = insts[i];
                inst = {};
                const XMMATRIX world =
                    XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(p.world));
                XMStoreFloat4x4(&inst.world,     XMMatrixTranspose(world));
                XMStoreFloat4x4(&inst.prevWorld, XMMatrixTranspose(world));
                inst.meshDescIdx = p.meshDescSlot;
                inst.materialIdx = i + 1;      // slot 0 stays the neutral record
                inst.viewMask    = 0xFFFFFFFFu;
                inst.instFlags   = kInstanceFlagMainPass;
            }
        }
        else
        {
            GPUInstanceData* inst = insts;
            *inst = {};
            // Identity unless a mesh override asked to be fitted. The primitive path keeps a
            // literal identity - a scale of 1 and a translate of 0 is not bit-identical through
            // XMMatrixMultiply - so the A/B parity comparison stays exact.
            XMMATRIX world = XMMatrixIdentity();
            if (req.meshIndexCount != 0 && req.meshDescSlot != RHI::kInvalidBufferIndex)
            {
                // Centre the AABB on the origin, THEN scale it into the unit box
                // the studio camera is framed for. Translate-before-scale,
                // because the offset is in the mesh's own units.
                world = XMMatrixMultiply(
                    XMMatrixTranslation(req.meshFitOffset[0], req.meshFitOffset[1],
                                        req.meshFitOffset[2]),
                    XMMatrixScaling(req.meshFitScale, req.meshFitScale, req.meshFitScale));
            }
            XMStoreFloat4x4(&inst->world,     XMMatrixTranspose(world));
            XMStoreFloat4x4(&inst->prevWorld, XMMatrixTranspose(world));
            inst->meshDescIdx = m_packets[0].meshDescriptorIndex;
            inst->materialIdx = m_packets[0].materialIndex;
            inst->viewMask    = 0xFFFFFFFFu;
            inst->instFlags   = kInstanceFlagMainPass;
        }
    }

    {
        // The one material packer, never hand-packed: the schema fields, the reconciled
        // alphaRef, the sRGB -> linear cache, the identity-UV default, the MATERIAL_GPU_FLAGS
        // bitmask and the translucent Unlit -> Standard narrowing all live there. The runtime
        // handed here is re-resolved from the same key ExtractRequests hashed with, so the
        // record and the key that names it cannot disagree.
        auto* mats = static_cast<Resource::MaterialGPUData*>(m_materialMapped[frameSlot]);
        if (IsAssetDraw(req))
        {
            // One record per part at slot part+1, through the same packer, so each is
            // byte-identical to what the main path writes for that material.
            for (uint32_t i = 0; i < req.parts.size() &&
                                 i < Preview::kMaxPreviewParts; ++i)
            {
                const Preview::AssetPart& p = req.parts[i];
                const Renderer::ResolvedMaterialRuntime prt =
                    m_owner.ResolvePreviewMaterialRuntime(p.matRuntimeKey);
                const ShaderReflect::Reflection* pPS =
                    p.customPSID ? m_owner.LookupCustomShaderReflection(p.customPSID, &p.mat) : nullptr;
                const ShaderReflect::Reflection* pVS =
                    p.customVSID ? m_owner.LookupCustomShaderReflection(p.customVSID, &p.mat) : nullptr;
                m_owner.WriteMaterialSlot(mats, i + 1, &p.mat, prt, pPS ? pPS : pVS);
            }
        }
        else
        {
            const Renderer::ResolvedMaterialRuntime rt =
                m_owner.ResolvePreviewMaterialRuntime(req.matRuntimeKey);
            // packRefl, not the PS reflection: see where it is resolved above.
            m_owner.WriteMaterialSlot(mats, kPreviewMatSlot, &req.mat, rt, packRefl);
        }
    }

    // The clustered fill + back lights, written before the LightCB: the count returned
    // is LightCB.clusterLightCount, and a count larger than the light buffer walks off
    // the end of an UPLOAD StructuredBuffer.
    const uint32_t clusterLights = WriteClusterLights(kLaneLive, frameSlot, req);

    // The studio key-light shadow: the VP must exist before WriteLightCB (which
    // publishes shadowMatrix[0] and shadowStrength from it) and ApplyPreviewView (which
    // hides the pass without it). A failure is not a bail: the preview renders
    // unshadowed.
    const bool wantShadow = (req.view.features & Preview::Feature::kStudioShadow) != 0
                         && set.shadow != nullptr;
    if (!wantShadow || !WriteStudioShadowView(req))
        m_studioShadowValid = false;

    WritePerViewCB(set, req);
    WriteLightCB(set, req, set.lighting, clusterLights);
    if (!m_shadowLogged && set.shadow)
    {
        m_shadowLogged = true;
        LOG_INFO("[PREVIEW] studio shadow state: want=%d valid=%d srv=%s strength=%.2f",
                 wantShadow ? 1 : 0, m_studioShadowValid ? 1 : 0,
                 set.shadow->GetShadowArrayGpuHandle() ? "ok" : "MISSING",
                 m_studioShadowValid ? Preview::kStudioShadowStrength : 0.f);
    }

    // ---- Graph bindings (per-instance state, re-set every Execute) ----
    set.graph.BindConstantBuffer("PerView", set.perViewCB->CurrentBuffer(m_gfx));
    set.graph.BindConstantBuffer("LightCB", set.lightCB->CurrentBuffer(m_gfx));
    // No fog in a preview (the zero block; see the member).
    set.graph.BindConstantBuffer("FogApply", m_fogApplyNoneCB.CurrentBuffer(m_gfx));
    // SkyboxPass#2's own b2, bound unconditionally: RenderContext::GetCB is an
    // ENGINE_CHECK, so an unbound slot is a CHECK FAILED line (an engine error to every
    // pack) the moment the view shows the pass. It is the pass's own ring, so this
    // rebinds state it owns.
    if (set.skybox)
        set.graph.BindConstantBuffer("SkyCB", set.skybox->GetSkyCB(m_gfx));
    // Root 8 (t0 space0) - the private instance table (one row per part).
    set.graph.BindBuffer("InstanceBuffer",  m_instanceBuf[frameSlot]);
    // Root 9 (t1 space0) - the shared MeshDescriptorHeap: per-scene state, never
    // copied; a primitive's descriptor slot indexes this one buffer.
    set.graph.BindBuffer("MeshDescriptors",
                       m_owner.GetMeshManager().GetDescriptorHeap().CurrentMeshDescBuffer());
    // Root 10 (t2 space0) - the private material table, for the GBuffer family.
    set.graph.BindBuffer("MaterialBuffer",  m_materialBuf[frameSlot]);
    // Root 14 (t0 space1) - the bindless buffer table. Unbound is a device removal,
    // not a black frame: the VS fetches every vertex attribute through g_Buffers[].
    set.graph.SetBindlessTableHandle(
        m_owner.GetMeshManager().GetDescriptorHeap().GetBufferTableGpuHandle());
    // Borrowed pointer - m_packets must outlive Execute, which is why it is a
    // member and not a local.
    set.graph.SetDrawList(&m_packets);
    // Root 28 (t12 space0) - the second binding of the same table. Lighting.ps decodes
    // matIdx from GBuffer1's alpha and indexes gMaterials there, not at t2; binding only
    // root 10 gives a correct G-buffer and silently wrong lighting (Aniso / SSS /
    // ClearCoat / NPR read the wrong record, with no bounds error and no debug-layer
    // message).
    set.lighting->SetMaterialBuffer(m_gfx.GetBufferSRVGpuHandle(m_materialBuf[frameSlot]));

    // ---- The studio environment ----
    // t6/t7 must never be unbound. The shared 1x1 black cube substitution lives in
    // Renderer::SyncSkyboxIBL, not LightingPass, so a path driving a LightingPass instance
    // directly applies it itself. Black is a defined degradation (dead metals).
    {
        // The studio triple, or the live scene's under Feature::kSceneLighting. Repeated
        // from WriteLightCB rather than shared because the consumers want different things
        // (handles here, a mips count and strength there); the one thing that must not
        // diverge - which environment this frame uses - is the same expression in both.
        const bool sceneLitEnv =
            (req.view.features & Preview::Feature::kSceneLighting) != 0
            && SceneLightingUsable(m_sceneLighting);
        const uint64_t envIrr  = sceneLitEnv ? m_sceneLighting.irradiance : m_iblIrradiance;
        const uint64_t envRad  = sceneLitEnv ? m_sceneLighting.radiance   : m_iblRadiance;
        const uint32_t envMips = sceneLitEnv ? m_sceneLighting.radianceMips : m_iblRadianceMips;
        const float    envStr  = sceneLitEnv ? m_sceneLighting.iblStrength
                                             : Preview::kIBLStrength;

        const uint64_t fallbackCube = m_owner.GetFallbackSkyCubeSrv();
        const uint64_t irr = envIrr ? envIrr : fallbackCube;
        const uint64_t rad = envRad ? envRad : fallbackCube;
        const bool indirectOn = (envIrr != 0) && (envRad != 0) && (m_brdfLut != 0);
        set.lighting->SetIBL(irr, rad, indirectOn ? envMips : 0u, envStr);
        set.lighting->SetBRDFLUT(m_brdfLut);
        // Everything else keeps LightingPass::Init's 1x1 neutral fallbacks (the CSM
        // cascade array - texel 0.0 = unshadowed under reversed-Z - white SSAO, the spot atlas
        // + identity VPs, zeroed sky SH, white NPR ramp); zeroing them explicitly keeps a
        // previous frame's state from leaking in. The studio shadow is handle 0 whenever the
        // pass is not running this frame (LightingPass's fully-lit 1x1x4 fallback), and is
        // re-resolved every frame.
        set.lighting->SetShadowMap(
            m_studioShadowValid ? set.shadow->GetShadowArrayGpuHandle() : 0ull);
        set.lighting->SetSpotShadowAtlas(0, 0);
        set.lighting->SetPointShadowAtlas(0);
        set.lighting->SetPerObjectShadowMap(0);
        set.lighting->SetHairShadowMap(0);
        // The clustered triple, all three or none: LightingPass::Execute skips each
        // unbound SRV, and the shader would index a bound grid with no light buffer.
        if (clusterLights > 0 && m_clusterReady[kLaneLive])
            set.lighting->SetClusterSRVs(
                m_gfx.GetBufferSRVGpuHandle(m_clusterLightBuf[kLaneLive][frameSlot]),
                m_gfx.GetBufferSRVGpuHandle(m_clusterIndexBuf[kLaneLive][frameSlot]),
                m_gfx.GetBufferSRVGpuHandle(m_clusterGridBuf[kLaneLive][kStudioGridView][frameSlot]));
        else
            set.lighting->SetClusterSRVs(0, 0, 0);
        set.lighting->SetSSAOHandle(0);
        (void)0;
        set.lighting->SetSkySH(0, false);
        set.lighting->SetAerialPerspective(0, 0.f);
        set.lighting->SetReflectionProbeCluster(0, 0);
        set.lighting->SetRTReflection(0);
        set.lighting->SetRTShadowMask(0);

        // ---- The studio shadow pass's own inputs ----
        if (set.shadow)
        {
            set.shadow->SetViewCB(m_studioShadowValid
                                      ? &m_shadowViewCB.CurrentBuffer(m_gfx)
                                      : nullptr);
            // Pushed every frame beside the view CB: the idle sweep can tear a set down and
            // rebuild it, and a value handed over once would revert to the pass default.
            set.shadow->SetAlphaTestEnabled(m_studioShadowAlphaTest);
        }

        // TransparentPass#2's inputs: SyncSkyboxIBL pushes these four at the main
        // instance every frame; this instance takes them from the sources the LightingPass
        // block used, so a translucent preview and an opaque one agree about the environment.
        if (set.transparent)
        {
            set.transparent->SetIBL(irr, rad, indirectOn ? envMips : 0u, envStr);
            set.transparent->SetBRDFLUT(m_brdfLut);
            // SH only on the scene path and only when the scene uses it, as WriteLightCB's
            // iblUseSH; a 0 handle binds nothing, which is safe because LightCB.iblUseSH is 0 in
            // lockstep.
            const bool sceneLitFwd =
                (req.view.features & Preview::Feature::kSceneLighting) != 0;
            set.transparent->SetSkySH(
                (sceneLitFwd && m_sceneLighting.skyUseSH) ? m_sceneLighting.skySH : 0ull);
            set.transparent->SetShadowMap(
                m_studioShadowValid ? set.shadow->GetShadowArrayGpuHandle() : 0ull);
            // The same triple as the deferred half: the forward loop opens it whenever
            // LightCB.clusterLightCount is non-zero.
            set.transparent->SetSpotShadowVP(0);
            if (clusterLights > 0 && m_clusterReady[kLaneLive])
                set.transparent->SetClusterSRVs(
                    m_gfx.GetBufferSRVGpuHandle(m_clusterLightBuf[kLaneLive][frameSlot]),
                    m_gfx.GetBufferSRVGpuHandle(m_clusterIndexBuf[kLaneLive][frameSlot]),
                    m_gfx.GetBufferSRVGpuHandle(m_clusterGridBuf[kLaneLive][kStudioGridView][frameSlot]));
            else
                set.transparent->SetClusterSRVs(0, 0, 0);
        }

        // The backdrop: SkyboxPass#2 draws whichever cube this names and nothing for 0
        // (its Execute early-outs), so Solid, a failed checker cube and a sky-less scene all
        // degrade to the flat clear colour. The view (ApplyPreviewView) also keeps the pass
        // from opening a command list for Solid and for every view mode but Lit / Unlit.
        if (set.skybox)
        {
            uint64_t backdrop = 0;
            switch (req.view.background)
            {
            case Preview::Background::Checker: backdrop = m_checkerCubeSrv; break;
            case Preview::Background::Skybox:
                backdrop = ((req.view.features & Preview::Feature::kSceneLighting)
                            && m_sceneLighting.skyboxCube)
                         ? m_sceneLighting.skyboxCube : rad;
                break;
            case Preview::Background::Solid:
            default: backdrop = 0; break;
            }
            set.skybox->SetEnvMap(backdrop);
            // The analytic sun disk, moon and stars are off: the studio SkyCB would otherwise
            // carry a zeroed sun direction and paint a disk at an arbitrary point of the
            // backdrop.
            set.skybox->SetSun({ 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f }, 0.01f, 0.f);
            set.skybox->SetMoon(0, false, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 0.f }, 0.01f);
            set.skybox->SetStars(0.f, 0.f);
        }
    }
    }   // end of the synthetic setup

    // Mesh Editor: FILL_MODE_WIREFRAME for this one record. Written on every record,
    // false included: the m_live passes serve the material inspector, the Asset Preview
    // and the Mesh Editor in turn, so a skipped reset would leave the next view drawing
    // wires. WorldCull ViewDescs never set a mode, so they write false. The studio shadow
    // pass keeps its solid PSO (LightingPass returns the wire colour before any shadow
    // term).
    {
        const bool wire = (req.view.viewMode ==
                           static_cast<uint32_t>(Renderer::ViewMode::Wireframe));
        if (set.gbuffer)     set.gbuffer->SetWireframe(wire);
        if (set.transparent) set.transparent->SetWireframe(wire);
    }

    // ---- Push, run the graph, pop ----
    // The push and the pop bracket Execute from outside: inside Execute the graph raises
    // m_parallelPassRecording for Phase B, and PushHdrTarget refuses while it is set (it
    // hands over a device-level state tracker that concurrently recording passes read).
    // The condition is "not inside any graph's Phase B", and Phase 4.3 is that place.
    // A refused push must not fall through: the binding stays on slot 0, the main
    // viewport, and everything below resolves through Hdr() - carrying on would clear the
    // live scene mid-frame.
    if (!m_gfx.PushHdrTarget(t->hdrSlot)) return bail();
    if (!ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == 1,
            "PreviewRenderer: HDR scope depth is %d immediately after a "
            "successful PushHdrTarget(%u); expected 1. Refusing to touch the "
            "HDR target - it may still be the main viewport's.",
            m_gfx.HdrScopeDepth(), t->hdrSlot))
    {
        m_gfx.PopHdrTarget();
        return bail();
    }

    // The background does not come from Execute's clearColor argument on this path:
    // SetRenderTargetToHdr ignores it (it drives the swap-chain clear) and clears from
    // the binding's stored colour, so the colour goes into the pushed binding first - get
    // it wrong and the background is black and the colour control does nothing.
    // GBufferPass#2 declares BuiltinTexture::HdrSceneColor as bind + clear, which is why
    // the graph needs no sky to have a defined backdrop. The target's optimised clear
    // value is {0,0,0,0} (CreateHdrTarget) and D3D12 compares all four components, so the
    // default background is {0,0,0,0}, which keeps the
    // CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE perf warning away; another colour costs
    // one warning per rendered frame. With the graph's HDR import the colour travels with
    // BindImport below; the slot's stored copy serves only the -rgimportoff arm.
    if (!set.hHdr.IsValid())
        m_gfx.SetHdrClearColor(req.view.clearColor);

    // The view, last and immediately before Execute (sticky, set on the serial
    // thread). It hides only pure consumers or writers of their own target; GBufferPass
    // (the HdrSceneColor clear declarer) and LightingPass declare no flag. Before the
    // external wait, which must name a pass that is going to run.
    ApplyPreviewView(set, req, wantForward);

    // ---- Chain the graph after the main one ----
    // The wait lands on the pass that is first this frame, resolved from the view, not
    // hard-coded: PreviewStudioShadowPass runs before GBufferPass when shown, and
    // RenderGraph re-homes a wait off an inactive pass onto the next active one but never
    // onto an earlier one. The world view reads main-graph resources (the borrowed CSM
    // atlas, the star cube), as does the scene-lighting toggle, and the edge costs a
    // fence the driver inserts anyway.
    // Immediately before Execute, after every bail: RenderGraph clears m_externalWaits
    // only inside Execute, so a wait registered before a bail would leave a recycled
    // CommandList handle latched for the next Execute.
    if (colorLastCL.IsValid())
    {
        const bool shadowRuns = set.shadow != nullptr && set.shadow->Shown();
        // Naming a hidden pass would still work (re-homed onto the next active one) but
        // costs a once-per-session warning that reads like a bug; ask for the pass that runs.
        set.graph.SetExternalWait(shadowRuns ? set.shadow->GetName()
                                             : set.gbuffer->GetName(),
                                  colorLastCL);
    }

    // The pushed slot's target and the view's background, for this Execute only.
    if (set.hHdr.IsValid())
        set.graph.BindImport(set.hHdr, m_gfx.GetHdrTexture(t->hdrSlot), req.view.clearColor);
    const RHI::CommandList graphLastCL = set.graph.Execute(m_gfx, req.view.clearColor);

    RHI::CommandList previewCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
    m_gfx.SetCommandListName(previewCL, "Preview");
    if (!previewCL.IsValid())
    {
        // BeginCommandList returns an invalid handle when the 256-slot pool is exhausted
        // (and has logged). Pop before bailing: an unbalanced push leaves the main viewport
        // pointed at the preview's texture for the rest of the frame.
        m_gfx.PopHdrTarget();
        return bail();
    }
    previewCL.gfx = &m_gfx;
    // AFTER the graph, whose passes wrote the HDR target this list samples. The
    // graph's own tail is the correct predecessor; colorLastCL is the fallback
    // for the (unreachable today) case of an all-inactive graph, whose Execute
    // returns an INVALID list by contract - hence the IsValid test rather than
    // an unconditional edge.
    if (graphLastCL.IsValid())
        m_gfx.AddCommandListDependency(previewCL, graphLastCL);
    else if (colorLastCL.IsValid())
        m_gfx.AddCommandListDependency(previewCL, colorLastCL);

    const uint32_t region = m_gfx.BeginGPUTimestamp(previewCL, "PreviewToneMap");

    // First frame only: the 1x1 neutral black was created without initialData (see
    // EnsureNeutralInputs), so its contents are defined here, on a list we own; it stays
    // in SHADER_RESOURCE for the rest of the run.
    if (!m_neutralBlackCleared)
    {
        const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
        m_gfx.ClearRenderTarget(m_neutralBlack, zero, previewCL);
        m_gfx.PushBarrier(RHI::GPUBarrier::Image(&m_neutralBlack,
                              RHI::ResourceState::RENDERTARGET,
                              RHI::ResourceState::SHADER_RESOURCE), previewCL);
        m_neutralBlackCleared = true;
    }

    // Hand the slot to the tone-map dispatch in the state it reads from, inside the
    // push scope on purpose: the call resolves through Hdr(), so after the Pop it would
    // re-state the main viewport's target - the most dangerous line in this file (every
    // external caller checks HdrScopeDepth() == 0). Recorded on previewCL, which the
    // graph's tail precedes, so the barrier lands after the last write and before the
    // first read.
    m_gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE_COMPUTE, previewCL);
    m_gfx.PopHdrTarget();

    ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == 0,
                      "PreviewRenderer::ProcessQueue leaked an HDR push scope (depth=%d).",
                      m_gfx.HdrScopeDepth());

    // Read the slot explicitly rather than through GetHdrSceneSrvGpuHandle():
    // the scope is closed, so that accessor now answers for the main viewport.
    t->toneMap->SetHdrSrvHandle(m_gfx.GetHdrTargetSrvGpuHandle(t->hdrSlot));
    t->toneMap->SetBloomSrvHandle(m_neutralBlackSrv);
    // The same buffer the LightCB named: the neutral 1.0 for a Synthetic view (a
    // studio render has no scene exposure); the scene's for a WorldCull view, so the PiP's
    // brightness follows the viewport and the forward Unlit divide is multiplied back.
    // Anything else breaks the divide / multiply pair silently.
    t->toneMap->SetExposureSrvHandle(
        (worldCull && m_worldToneMapExposureSrv) ? m_worldToneMapExposureSrv
                                                 : m_neutralExposureSrv);
    t->toneMap->SetLensFlareSrvHandle(m_neutralBlackSrv);
    t->toneMap->SetBloomStrength(0.0f);
    t->toneMap->SetLensFlareStrength(0.0f);
    t->toneMap->Execute(previewCL);
    // Execute leaves the output in SHADER_RESOURCE_COMPUTE and ImGui samples it as a
    // pixel-shader resource. The main viewport gets this from Renderer's restore CL;
    // nothing else does it for the preview.
    t->toneMap->TransitionForDisplay(previewCL);

    // ---- The tile copy and the readback ring ----
    // Thumbnail path only. The tile is the per-asset, SRV-only texture the editor created
    // in its serial window, in SHADER_RESOURCE on entry and exit by contract, so the round
    // trip SHADER_RESOURCE -> COPY_DEST -> COPY_SRC -> SHADER_RESOURCE is stated here;
    // nothing else tracks it. The tone-map output's state goes through its own accessors
    // (Get/SetFinalOutputState): CopyTextureSubresource emits no barriers, and
    // ToneMapPass's next Execute reads m_finalOutputState to pick its transition - a stale
    // tracker is a wrong-state read reported on the next frame's dispatch.
    m_thumbTileCopied = false;
    if (tile && tile->IsValid())
    {
        const RHI::Texture* out = t->toneMap->GetFinalOutputTexture();
        const bool formatOk = out && out->IsValid() &&
                              out->desc.format == RHI::Format::R8G8B8A8_UNORM;
        if (!formatOk)
        {
            // An HDR output mode makes the tone-map output FP16 (HDRCompositionFormat), and a
            // copy between formats is undefined; a thumbnail is not worth a converting blit, so
            // the lane declines. LOG_WARNING, not ENGINE_CHECK_ONCE: an HDR output mode is a
            // legitimate configuration, and a CHECK FAILED line counts as an engine error in
            // every pack.
            if (!m_thumbDisabled)
                LOG_WARNING("[PREVIEW] thumbnails need an SDR (R8G8B8A8_UNORM) "
                            "tone-map output to copy from; this session's HDR "
                            "output mode produces a different format, so "
                            "asset-browser material tiles are disabled for it "
                            "(the inspector's live preview is unaffected).");
            // The latch is unwound by ExtractThumbnails on the game half at the top of the
            // next frame, not here: m_thumbQueue and m_thumbAssetKey are the game half's, so the
            // render half only sets this bool. The unwind must happen - a latch left valid makes
            // HasThumbnailInFlight true for this asset forever, freezing the browser's eviction
            // sweep and enqueue loop while every other material in the folder gets a tile texture
            // and a LoadMaterial each frame.
            m_thumbDisabled = true;
        }
        else
        {
            const RHI::ResourceState outWas = t->toneMap->GetFinalOutputState();
            RHI::GPUBarrier pre[2] = {
                RHI::GPUBarrier::Image(out, outWas, RHI::ResourceState::COPY_SRC),
                RHI::GPUBarrier::Image(tile, RHI::ResourceState::SHADER_RESOURCE,
                                             RHI::ResourceState::COPY_DST),
            };
            m_gfx.PushBarriers(pre, 2, previewCL);
            t->toneMap->SetFinalOutputState(RHI::ResourceState::COPY_SRC);

            // WHOLE subresource, no source box: the target is exactly the tile
            // size, which is the whole reason kThumbnailSize is pinned.
            m_gfx.CopyTextureSubresource(*out, 0, 0, *tile, 0, 0, previewCL);

            // And on into the kFramesInFlight-deep readback ring, copied from the tile rather
            // than the tone-map output: one more barrier, and the encoder's pixels stay
            // bit-identical to what ImGui shows.
            const uint32_t rbSlot = frameSlot;
            if (m_thumbReadbackReady && rbSlot < RHI::kFramesInFlight &&
                m_thumbReadback[rbSlot].IsValid())
            {
                RHI::GPUBarrier mid = RHI::GPUBarrier::Image(
                    tile, RHI::ResourceState::COPY_DST, RHI::ResourceState::COPY_SRC);
                m_gfx.PushBarrier(mid, previewCL);
                m_gfx.CopyTexture2DToBuffer(*tile, m_thumbReadback[rbSlot], previewCL);
                m_thumbReadbackPrimed[rbSlot] = true;
                m_thumbReadbackKey[rbSlot]    = tileAssetKey;
                m_thumbReadbackGen[rbSlot]    = req.generation;
                RHI::GPUBarrier post[2] = {
                    RHI::GPUBarrier::Image(tile, RHI::ResourceState::COPY_SRC,
                                                 RHI::ResourceState::SHADER_RESOURCE),
                    RHI::GPUBarrier::Image(out, RHI::ResourceState::COPY_SRC, outWas),
                };
                m_gfx.PushBarriers(post, 2, previewCL);
            }
            else
            {
                RHI::GPUBarrier post[2] = {
                    RHI::GPUBarrier::Image(tile, RHI::ResourceState::COPY_DST,
                                                 RHI::ResourceState::SHADER_RESOURCE),
                    RHI::GPUBarrier::Image(out, RHI::ResourceState::COPY_SRC, outWas),
                };
                m_gfx.PushBarriers(post, 2, previewCL);
            }
            t->toneMap->SetFinalOutputState(outWas);
            m_thumbTileCopied = true;
        }
    }

    m_gfx.EndGPUTimestamp(previewCL, region);

    // The lane was already counted at the first constant-buffer write (see there).
    inFlight = false;
    // hasOutput is published here, on the render half at record time, so the first UI built
    // after this record already shows the picture: FramePipeline::RenderHalfAsync makes
    // previewCL a predecessor of primaryCL (which ImGui records into), so the ImGui pass
    // samples finished pixels. Publishing from ApplyFeedback (the next frame's game half)
    // would cost the serial path, whose UI follows this record in the same frame, a frame.
    t->hasOutput.store(true, std::memory_order_release);
    return previewCL;
}

uint64_t PreviewRenderer::GetSrv(uint32_t viewId) const
{
    const Target* t = Get(viewId);
    if (!t || !t->toneMap) return 0;
    if (!t->hasOutput.load(std::memory_order_acquire)) return 0;
    // Re-resolved, never cached: the descriptor slot goes through the allocator's free
    // list, and ImGui's own SRVs come from the same heap.
    return t->toneMap->GetFinalOutputSrvHandle();
}

bool PreviewRenderer::GetOutputSize(uint32_t viewId, uint32_t& outW,
                                    uint32_t& outH) const
{
    const Target* t = Get(viewId);
    if (!t || !t->toneMap) return false;
    if (!t->hasOutput.load(std::memory_order_acquire)) return false;
    if (t->w == 0 || t->h == 0) return false;
    // Target::w/h is what PrepareFrame created, not what the latch asked for; they
    // differ between a resize request and the recompile that services it.
    outW = t->w;
    outH = t->h;
    return true;
}

const RHI::Texture* PreviewRenderer::GetOutputTexture(uint32_t viewId) const
{
    const Target* t = Get(viewId);
    if (!t || !t->toneMap) return nullptr;
    if (!t->hasOutput.load(std::memory_order_acquire)) return nullptr;
    const RHI::Texture* tex = t->toneMap->GetFinalOutputTexture();
    return (tex && tex->IsValid()) ? tex : nullptr;
}
