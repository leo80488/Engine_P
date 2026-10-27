// Renderer_Frame.cpp - the game half's frame start: BeginFrame (with the
// DX12_MATBUF_CHECKSUM diagnostic only it uses) and the preparation it hands on
// (PrepareReflectionBindings, FinishFramePreparation, PrepareFrameOnLane).

#include "Diagnostics/Profiler.h"
#include <optional>
#include "System/PathUtil.h"
#include "Renderer/Renderer.h"
#include "Renderer/GatherGateStats.h"   // the DDGI gate's per-frame record
#include "Settings/SettingsStore.h"
#include "Scene/MeshAssetRegistry.h"
#include "Resource/MeshLibIdentity.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/DepthTrace.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"
#include "Graphics/SSR/SSRSubsystem.h"
#include "Graphics/ReflectionProbeTypes.h"

// render passes
#include "Renderer/Preview/PreviewRenderer.h"
#include "Renderer/Preview/PreviewStudioRig.h"
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/SurfaceCacheBakePass.h"
#include "RenderGraph/RenderPass/TerrainPass.h"
#include "RenderGraph/RenderPass/GrassPass.h"
#include "RenderGraph/RenderPass/WaterPass.h"
#include "RenderGraph/RenderPass/WaterSurfacePass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/PickingPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/NPRStabilizePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/LumaReducePass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/FSR2Pass.h"
#include "RenderGraph/RenderPass/DLSSPass.h"
#include "RenderGraph/RenderPass/UpscalerReactiveMaskPass.h"
#include "RenderGraph/RenderPass/RRGuidePass.h"
#include "RenderGraph/RenderPass/FXAAPass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/UnderwaterPass.h"
#include "RenderGraph/RenderPass/DepthOfFieldPass.h"
#include "RenderGraph/RenderPass/StylizePass.h"
#include "RenderGraph/RenderPass/DebugTargetViewPass.h"
#include "Renderer/TextureViewService.h"
#include "Renderer/UIViewService.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/SceneVoxelPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/OutlineGeometryPass.h"
#include "RenderGraph/RenderPass/SkinningPass.h"
#include "Graphics/WaterRippleSystem.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/RTReflectionPass.h"
#include "RenderGraph/RenderPass/TranslucentFrontLayerPass.h"
#include "RenderGraph/RenderPass/RTShadowPass.h"
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/RTDIPass.h"
#include "RenderGraph/RenderPass/LightSamplingPass.h"
#include "RenderGraph/RenderPass/RtxdiProbe.h"
#include "RenderGraph/RenderPass/RTGIPass.h"
#include "RenderGraph/RenderPass/RTAOPass.h"
#include "RenderGraph/RenderPass/SSGIPass.h"
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/PerObjectShadowPass.h"
#include "RenderGraph/RenderPass/HairShadowMarkPass.h"
#include "RenderGraph/RenderPass/FaceOverlayPass.h"
#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "RenderGraph/RenderPass/TracerPasses.h"
#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "RenderGraph/RenderPass/AfterimageCapturePass.h"
#include "RenderGraph/RenderPass/TrailPasses.h"
#include "RenderGraph/RenderPass/BillboardFXPass.h"
#include "RenderGraph/RenderPass/CullingPass.h"
#include "RenderGraph/RenderPass/HiZPass.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/EditorGridPass.h"

// post-process
#include "PostProcess/PostChain.h"
#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/PostProcessRuntime.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/IKRigSystem.h"
#include "ECS/Components.h"
#include "Graphics/ShadingStencil.h"
#include "ECS/BeamComponent.h"
#include "ECS/VFXSpawnRequests.h"   // unified VFX Lane-B mailboxes (Tracer/Decal/Afterimage/Mesh)
#include "ECS/HierarchyComponents.h"
#include "ECS/FaceShadingSystem.h"

// resource system
#include "Resource/MaterialSerializer.h"

// engine systems
#include "System/Log.h"
#include "System/ThreadContext.h" // Extract/Render context for the split tripwire
#include "System/JobDefaults.h"
#include "System/CommandLine.h"   // -phaselog_every, -reflectionbindlessverify, -materialcpuverify
#include "System/Check.h"     // ENGINE_CHECK / ENGINE_CHECK_ONCE tripwires
#include "System/TaskSystem.h"
#include "System/EventBus.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdlib>            // _dupenv_s / strtoull - DX12_MATBUF_CHECKSUM
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// ==== DX12_MATBUF_CHECKSUM diagnostic ====
// DX12_MATBUF_CHECKSUM=<frame> logs [MATBUF] with an FNV-1a hash of the material table
// at that frame, so a refactor that must not change material bytes can be compared
// before vs after.
// It hashes the CPU-visible staging table m_materialBuffer[GetUploadSlot()], not the
// DEFAULT-heap table the shaders read: the MaterialUpload twins carry only dirty spans,
// while the staging buffer is kept a complete material table (ProcessProbeBakeQueue
// reads it directly), and the DEFAULT table would need a GPU readback.
// Unset: one function-local-static compare per BeginFrame (parsed once with _dupenv_s;
// getenv is a deprecation error in this warning set). Set: the byte walk over
// write-combined UPLOAD memory spikes the sampled frame, so it samples one frame - never
// enable it in a perf measurement.
// Thread: BeginFrame is the game half and GetUploadSlot() its slot, so the game half owns
// the buffer; the render half only reads it through the copy engine.
namespace
{
    // Frame index at which to sample, 0 = feature off. Empty / unparsable /
    // "0" all parse to 0, so frame 0 cannot be sampled - by design, the value
    // is the literal frame number, never a multiplier.
    uint64_t MatBufChecksumFrame()
    {
        static const uint64_t s_frame = []() -> uint64_t
        {
            char*              buf = nullptr;
            size_t             len = 0;
            unsigned long long f   = 0;
            if (_dupenv_s(&buf, &len, "DX12_MATBUF_CHECKSUM") == 0 && buf)
            {
                f = std::strtoull(buf, nullptr, 10);
                std::free(buf);
            }
            return static_cast<uint64_t>(f);
        }();
        return s_frame;
    }
} // namespace

// App's ladder asks whether this diagnostic is armed: -brslane refuses under it,
// because the checksum is taken on main inside BeginFrame, before the emit half has
// written the table it hashes. A predicate rather than a copy of the getenv in App,
// so the two cannot disagree.
bool Renderer::MatBufChecksumArmed() { return MatBufChecksumFrame() != 0; }

namespace
{

    // FNV-1a 64, a plain byte loop on purpose: the value must be comparable across builds
    // and across a refactor, and it runs at most once per process. Fold continues an
    // existing hash so a value can span disjoint regions (the custom-CBV checksum folds one
    // slot index + one slot's bytes at a time).
    constexpr uint64_t kMatBufFnvBasis = 14695981039346656037ull;

    uint64_t MatBufFnv1a64Fold(uint64_t h, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            h ^= static_cast<uint64_t>(p[i]);
            h *= 1099511628211ull;                    // FNV prime
        }
        return h;
    }

    uint64_t MatBufFnv1a64(const void* data, size_t bytes)
    {
        return MatBufFnv1a64Fold(kMatBufFnvBasis, data, bytes);
    }
}

void Renderer::PrepareReflectionBindings()
{
    ZoneScopedN("Renderer.PrepareReflectionBindings");
    // BeginFrame runs with RenderMain parked. Keep registration on this side
    // of publication: RegisterBuffer mutates MeshDescriptorHeap's dedup/free
    // lists, also used by animation. Render sees only the published indices.
    auto& view = FrameViewWrite();
    view.reflectionMaterialSlot = RHI::kInvalidBufferIndex;
    view.reflectionExposureSlot = RHI::kInvalidBufferIndex;
    if (!m_rtReflectionPass) return;

    auto& heap = m_meshMgr.GetDescriptorHeap();
    if (MaterialGpuBufferActive())
    {
        if (m_materialBufGpuBindless == RHI::kInvalidBufferIndex)
            m_materialBufGpuBindless = heap.RegisterBuffer(m_materialBufferGPU);
        view.reflectionMaterialSlot = m_materialBufGpuBindless;
    }
    if (m_autoExposurePass && m_autoExposurePass->GetExposureBuffer().IsValid())
    {
        if (m_exposureBufBindless == RHI::kInvalidBufferIndex)
            m_exposureBufBindless = heap.RegisterBuffer(m_autoExposurePass->GetExposureBuffer());
        view.reflectionExposureSlot = m_exposureBufBindless;
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_verify = Sys::CommandLine::Get().Has("-reflectionbindlessverify");
    if (s_verify)
    {
        // Read-only resource lookup cannot hide invalidation bugs by repairing
        // a missing registration. Verification stays in the owner phase.
        const uint32_t material = MaterialGpuBufferActive()
            ? heap.FindRegisteredBuffer(m_materialBufferGPU) : RHI::kInvalidBufferIndex;
        const uint32_t exposure = m_autoExposurePass
            ? heap.FindRegisteredBuffer(m_autoExposurePass->GetExposureBuffer()) : RHI::kInvalidBufferIndex;
        const bool match = material == view.reflectionMaterialSlot &&
                           exposure == view.reflectionExposureSlot;
        ENGINE_CHECK(match, "reflection bindless publication differs from resource lookup");
        if (FrameViewWrite().currentFrame % 120 == 0 || !match)
            LOG_INFO("[REFLECTION_BINDLESS_ORACLE] material=%u exposure=%u match=%u",
                     material, exposure, match ? 1u : 0u);
    }
#endif
}

void Renderer::BeginFrame(World& world, FrameIndex frame, float dt , uint32_t vpW, uint32_t vpH,
                          Sys::FrameTaskContext* cpuFrame)
{
    // One frame, one preparer. m_laneEmitOwed is the -brslane form: the lane's
    // FinishFramePreparation clears it, and the Animation-boundary drain -brslane requires
    // joins the lane before this line, so finding it set means the lane never finished
    // frame F - and F+1 would emit into the buffer F's render half is still reading.
    if (m_cpuFrame || m_pendingPublication || m_laneEmitOwed)
        throw std::logic_error("Renderer::BeginFrame before prior preparation publication");
    m_cpuFrame = cpuFrame;
    struct ResetCpuFrame { Sys::FrameTaskContext*& ptr; ~ResetCpuFrame() { ptr = nullptr; } } resetCpuFrame{m_cpuFrame};

    // Covers the scene gather, instance + light upload and culling setup, otherwise the
    // largest unattributed part of the Render phase.
    ZoneScopedN("Renderer.BeginFrame");

    // This frame's emit lease, latched once. Both values move under the emit path once
    // main runs ahead of a lane still finishing frame F:
    //   m_emitIdx         - the scene-payload buffer this frame owns. Equals m_sceneWrite
    //                       on the default path; only AdvanceSceneWrite, which runs last,
    //                       separates them.
    //   m_frameUploadSlot - the upload slot every mapped pointer in BuildScene_SortAndEmit
    //                       derives from.
    // Latched here rather than at the publish: on a frame published from another thread the
    // previous AdvanceSceneWrite already moved m_sceneWrite, so this picks the buffer main
    // is about to build into.
    m_emitIdx         = m_sceneWrite;
    m_frameUploadSlot = m_gfx.GetUploadSlot();
    // And who owes this frame's emit half, latched beside the lease for the same reason:
    // BuildScene_PlanEmit's `framePreparation`, BuildScene_SortAndEmit's deferral and the
    // publish at the end of this function must answer one question asked once, not three
    // reads of a flag App could flip.
    m_emitDeferred = m_brsLaneEnabled;

    // Resolve the post-cull survivor count from the slot recorded kFrameSlots frames ago,
    // before anything is recorded this frame. Gated: without -artifacts= there is no
    // frames.jsonl to feed, so a normal Game.exe allocates no readback heap, records no
    // copy and maps nothing here.
    m_visibleDrawCmdsValid = false;
    if (m_visibleDrawCmdsEnabled)
    {
        m_frameCounterForReadback.fetch_add(1, std::memory_order_relaxed);
        const uint32_t rbSlot = m_gfx.GetUploadSlot();
        if (rbSlot < kFrameSlots && m_drawCountReadbackFrame[rbSlot] != 0 &&
            m_drawCountReadback[rbSlot].IsValid())
        {
            if (const uint32_t* p =
                    static_cast<const uint32_t*>(m_gfx.MapBuffer(m_drawCountReadback[rbSlot])))
            {
                uint32_t sum = 0;
                const uint32_t n = m_drawCountReadbackGroups[rbSlot];
                for (uint32_t i = 0; i < n; ++i) sum += p[i];
                m_visibleDrawCmds      = sum;
                m_visibleDrawCmdsValid = true;
                m_gfx.UnmapBuffer(m_drawCountReadback[rbSlot]);
            }
            // Cleared unconditionally: a frame that records no copy must not
            // let this slot re-serve its old contents when it next comes
            // round. Absent data has to read as absent.
            m_drawCountReadbackFrame[rbSlot] = 0;
        }
    }

    // Same discipline, separate ring: the visible-geometry counters are not
    // gated on -artifacts (the debug HUD is their consumer), and they carry
    // their own frame counter because m_frameCounterForReadback above only
    // advances inside the artifacts gate.
    m_visStatsValid = false;
    if (m_visStatsEnabled)
    {
        m_visStatsFrameCounter.fetch_add(1, std::memory_order_relaxed);
        const uint32_t vsSlot = m_gfx.GetUploadSlot();
        if (vsSlot < kFrameSlots && m_visStatsReadbackFrame[vsSlot] != 0 &&
            m_visStatsReadback[vsSlot].IsValid())
        {
            if (const uint32_t* p =
                    static_cast<const uint32_t*>(m_gfx.MapBuffer(m_visStatsReadback[vsSlot])))
            {
                m_visTriangles  = p[0];
                m_visInstances  = p[1];
                m_visCommands   = p[2];
                m_visStatsValid = true;
                m_gfx.UnmapBuffer(m_visStatsReadback[vsSlot]);
            }
            // Cleared unconditionally: a frame that records no copy must not let this slot
            // re-serve stale numbers when it next comes round.
            m_visStatsReadbackFrame[vsSlot] = 0;
        }
    }

	FrameViewWrite().deltaTime    = dt;
    // The world clocks' step this frame (BeginWorldClock); dt when the host never asked.
    const float worldDt = std::exchange(m_frameWorldDtSet, false) ? m_frameWorldDt : dt;
    FrameViewWrite().worldDeltaTime = worldDt;
    FrameViewWrite().worldStepped   = std::exchange(m_frameWorldStepped, false);
    m_lastWorld    = &world;
    FrameViewWrite().currentFrame = frame;
    // -meshlets_at=<frame>:<0|1> (and the lod / shadowlod / cull switches below) land on
    // the game half before this frame's SceneEmit latch, so frame `frame` is the first one
    // rendered with the new state.
    if (m_meshletsAtFrame >= 0 && static_cast<int64_t>(frame) >= m_meshletsAtFrame)
    {
        m_meshletsRequested = m_meshletsAtValue;
        LOG_INFO("[MESHLET] toggle frame=%llu meshlets=%d", static_cast<unsigned long long>(frame),
                 m_meshletsRequested ? 1 : 0);
        m_meshletsAtFrame = -1;
    }
    if (m_meshletLodAtFrame >= 0 && static_cast<int64_t>(frame) >= m_meshletLodAtFrame)
    {
        m_meshletLodTau = m_meshletLodAtValue;
        LOG_INFO("[MESHLET] toggle frame=%llu lod=%.3f", static_cast<unsigned long long>(frame), m_meshletLodTau);
        m_meshletLodAtFrame = -1;
    }
    if (m_meshletShadowLodAtFrame >= 0 && static_cast<int64_t>(frame) >= m_meshletShadowLodAtFrame)
    {
        m_meshletShadowLodTau = m_meshletShadowLodAtValue;
        LOG_INFO("[MESHLET] toggle frame=%llu shadowlod=%.3f", static_cast<unsigned long long>(frame),
                 m_meshletShadowLodTau);
        m_meshletShadowLodAtFrame = -1;
    }
    if (m_meshletCullAtFrame >= 0 && static_cast<int64_t>(frame) >= m_meshletCullAtFrame)
    {
        m_meshletCullMask = m_meshletCullAtValue;
        LOG_INFO("[MESHLET] toggle frame=%llu cull=%u", static_cast<unsigned long long>(frame), m_meshletCullMask);
        m_meshletCullAtFrame = -1;
    }
    // Everything from here to the end of BeginFrame is EXTRACTION: it reads the World and
    // legitimately writes it back (material dirty flags, gatherSeq, beam slots, mailbox
    // drains). The scope lets the render-context tripwire tell "reads the World on purpose"
    // from "reads the World from the render half"; at DX12_VERIFY_RENDERCTX=2 it lists the
    // structural World writes made here.
    Sys::ThreadCtx::Scope _extractCtx(Sys::ThreadCtx::Extract);
    // The pre-scene-build part of the game half as one profiler stage (closed right before
    // BuildRenderScene).
    std::optional<Diag::Prof::Scope> _bfPre;
    { static Diag::Prof::NameSlot s_bfPre{ "BF.PreScene" };
      _bfPre.emplace(s_bfPre, Diag::Prof::Cat::Work, Diag::Prof::Level::Stats); }
    // Re-target entity-destroy listener on World change; steady state = pointer compare.
    SubscribeToWorld(&world);
    // Recompile graph (recreates GBuffers) on resize so they sync with PickingPass + scaling.
    const uint32_t winW    = m_gfx.GetWidth();
    const uint32_t winH    = m_gfx.GetHeight();
    const uint32_t renderW = m_gfx.GetRenderWidth();
    const uint32_t renderH = m_gfx.GetRenderHeight();
    if (winW != m_lastWindowW || winH != m_lastWindowH ||
        renderW != m_lastRenderW || renderH != m_lastRenderH)
    {
        m_graph.MarkDirty();
        m_graph.Compile(m_gfx);   // explicit recompile so Execute() never recompiles during parallel recording
        // The main view's Resize, raised for this frame: TAA, ReSTIR and the path tracer reset
        // at its render start; the rest notice the size themselves.
        RaiseViewBreak(HistoryReset::Resize);
        m_lastWindowW = winW;
        m_lastWindowH = winH;
        m_lastRenderW = renderW;
        m_lastRenderH = renderH;
    }
    // The material samplers' LOD bias follows the upscale ratio (Graphics/MaterialSampler.h).
    // Set here, beside the resize recompile, because the recording workers read the passes'
    // sampler indices. Only a temporal upscaler that really runs (FSR 2, DLSS, DLSS-RR, as
    // this frame's plan resolves it in PlanGameResolve) rebuilds display detail from render
    // samples. TAA under display.renderScale < 1, or standing in for an upscaler that did not
    // come up, resolves at render size and is stretched afterwards, so it keeps the native
    // bias; so does an upscaler's first frame, before its context exists.
    const FramePlan::Decisions resolve = PlanGameResolve(vpW, vpH);
    const uint32_t biasDisplayW = (resolve.fsrResolved || resolve.dlssResolved) ? vpW : renderW;
    if (renderW != m_mipBiasRenderW || biasDisplayW != m_mipBiasDisplayW)
        ApplyUpscaleMipBias(renderW, biasDisplayW);

    // SSR resize: EnsureTexture RECREATES the upsample color target on a render-resolution
    // change, freeing the old descriptor and handing back a new heap slot. It is the one
    // folded resource whose heap index is not lifetime-stable, so this order is load-bearing:
    //     OnResize (may recreate)  ->  SetSSRResult (fresh handle)
    //  -> resolve() block (index re-derived)  ->  graph.Execute (WaterPass records)
    // all within this frame. Never cache the index: a stale one makes the water surface
    // sample a released or wrong-sized texture.
    // WaterPass is the only consumer; LightingPass does not read the SSR result (unified
    // hybrid reflections cover it).
    if (m_ssrSubsystem)
    {
        const uint64_t srv = m_ssrSubsystem->OnResize(renderW, renderH);
        // Disabled -> pass 0 so WaterPass falls back to its own 1x1 black texture and the
        // (1 - conf) sky dampening drains to a no-op.
        if (m_waterPass)
            m_waterPass->SetSSRResult(m_ssrEnabled ? srv : 0);
    }

    // MeshDescriptorHeap::BeginFrame and the animation chain run in TickAnimationChain
    // (Animation phase) before this: the heap is on a fresh slot, and the
    // SkinningOutputComponent offsets and PoseRingBuffer / VertexRing writes are in place
    // for this frame's SkinningPass.

    // Unified VFX "Lane B" mailbox drain. VFXSpawnSystem (BoneAttachment phase) deposits
    // spawn requests for the Renderer-owned backends the pure-ECS layer cannot reach
    // (Tracer/Afterimage GPU pools, Decal material library, runtime Mesh load). Drain them
    // before the afterimage/tracer BeginFrame passes consume their pending-spawn queues, so
    // the effects land this frame.
    {
        // Tracer: imperative GPU ring (Spawn enqueues; tracer BeginFrame drains).
        if (m_tracerSystem)
            world.ForEach<PendingTracerSpawns>([&](Entity, PendingTracerSpawns& mb){
                for (auto& r : mb.reqs)
                {
                    // `noiseTex=` path form: resolve to a bindless index here (TextureSystem is
                    // Renderer-owned). Async load: a spawn that lands before the texture is ready falls back
                    // to the procedural noise; later spawns pick the texture up.
                    uint32_t noiseIdx = r.noiseTexBindless;
                    if (!r.noiseTexPath.empty() && m_texSys && m_resMgr)
                    {
                        auto [it, inserted] = m_tracerNoiseTexCache.try_emplace(
                            r.noiseTexPath, Resource::kInvalidTextureHandle);
                        if (inserted)
                            it->second = m_texSys->Acquire(r.noiseTexPath, *m_resMgr, m_gfx);
                        if (it->second != Resource::kInvalidTextureHandle &&
                            m_texSys->IsReady(it->second))
                            if (const RHI::Texture* tex = m_texSys->GetTexture(it->second))
                                noiseIdx = tex->handleId;
                    }
                    m_tracerSystem->Spawn(r.start, r.end, r.color, r.width, r.lifetime, noiseIdx);
                }
                mb.reqs.clear();
            });

        // Afterimage: one Spawn per skinned sub-mesh belonging to the spawner character
        // (resolved via SkeletonRef; a skin without one matches by identity). The target handle
        // guards against a recycled spawner.
        if (m_afterimageSystem)
            world.ForEach<PendingAfterimageSpawns>([&](Entity, PendingAfterimageSpawns& mb){
                for (auto& r : mb.reqs) {
                    if (!world.IsHandleValid(r.target)) continue;
                    const Entity root = r.target.entity;
                    if (auto* pSkin = world.GetPool<MeshSkinnedComponent>()) {
                        const auto& ents = pSkin->Entities();
                        for (size_t i = 0; i < ents.size(); ++i) {
                            const Entity me = ents[i];
                            Entity owner = me;
                            if (auto* ref = world.GetComponent<SkeletonRef>(me)) owner = ref->entity;
                            if (owner == root)
                                m_afterimageSystem->Spawn(me, r.lifetime, r.color);
                        }
                    }
                }
                mb.reqs.clear();
            });

        // Decal: collect-then-spawn. SpawnAtSurface creates entities, so drain the mailbox into
        // a local list first to keep the pool view stable.
        {
            std::vector<PendingDecalSpawns::Req> decals;
            world.ForEach<PendingDecalSpawns>([&](Entity, PendingDecalSpawns& mb){
                for (auto& r : mb.reqs) decals.push_back(std::move(r));
                mb.reqs.clear();
            });
            for (auto& r : decals) {
                auto mat = m_decalMaterialLibrary.Find(r.materialName);
                if (!mat) {
                    LOG_WARNING("VFX decal: material '%s' not found in DecalMaterialLibrary",
                                r.materialName.c_str());
                    continue;
                }
                m_decalSpawner.SpawnAtSurface(world, mat, r.position, r.normal,
                                              r.size, r.depth, r.lifetime, r.fadeOutDuration, r.rollZ);
            }
        }

        // Mesh VFX: resolve path -> GPU mesh + patch the pre-created entity.
        DrainMeshVFXSpawns(world);
    }

    // Resolve pending afterimage spawns + update slot lifetimes. Must run after
    // BuildSkinJobs so SkinningOutputComponent offsets are the freshly-allocated
    // ones for this frame's ring slot.
    if (m_afterimageSystem)
        m_afterimageSystem->BeginFrame(world, m_meshMgr.GetDescriptorHeap(),
                                       m_skin.GetVertexRing(), worldDt);

    // Mirror this frame's viewport/render dimensions before BuildRenderScene, so every
    // BuildScene_* consumer (cluster/decal camera CBs, grass/terrain cull, billboards) sees
    // this frame's sizes rather than last frame's (zeros on frame 0): UploadFrameData runs
    // after the scene build. The FrameView camera is already this frame's (App::Run calls
    // SetCamera before the Render phase), so scene-build code can derive fresh matrices via
    // BuildCameraViewProj().
    if (vpW != 0 && vpH != 0)
    {
        FrameViewWrite().vpWidth  = vpW;
        FrameViewWrite().vpHeight = vpH;
        FrameViewWrite().renderWidth  = m_gfx.GetRenderWidth();
        FrameViewWrite().renderHeight = m_gfx.GetRenderHeight();
        if (FrameViewWrite().renderWidth == 0 || FrameViewWrite().renderHeight == 0)
            ComputeRenderResolution(vpW, vpH, FrameViewWrite().renderWidth, FrameViewWrite().renderHeight);
    }

    // Shadow-caster retention frusta come from ShadowSystem's cascade matrices, so they
    // describe the cascades actually rendered, the ultra-far 400..2000 m one included; do not
    // reintroduce a separate split formula.
    // The matrices are last frame's: ShadowSystem::Update stays in UploadFrameData because the
    // sun direction is final only after BuildRenderScene (the light gather writes
    // LightCB.lightDir; SyncSkyboxIBL's TOD override and TODSun/MoonSyncSystem aim the light
    // entities, all downstream of here). Updating it here would read a kFrameCount-stale ring
    // slot - zeros for the first frames after a load, which drops CSM entirely. One frame of
    // camera motion is small against a cascade radius.
    if (m_shadowSystem)
        m_shadowFrustum.Compute(*m_shadowSystem);
    // The secondary views' cascades, fitted the same frame (FitViewShadowRetention).
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        m_viewShadowFrustum[slot] = m_viewShadowFit[slot];
    m_viewShadowRetention = m_viewShadowFitAny;
    // Advance global time before BuildRenderScene so grass/water publish time = T_n and
    // prevTime = T_n - dt_n = exactly the T_{n-1} frame n-1 published; otherwise the TAA
    // wind-velocity window mismatches the actual pose delta under uneven frame pacing.
    FrameViewWrite().globalTimeSec += worldDt;
    // Surface-cache frame boundary. MUST run before BuildRenderScene: the batch loop in
    // SortAndEmit stamps entity->page mappings with the cache's frame counter, and
    // DDGISceneAS later this frame trusts only a mapping stamped with the CURRENT frame (an
    // entity that stopped being drawn must drop back to the alias proxy, not keep a stale
    // page). Also re-arms the per-frame bake budget and promotes pages whose bake has
    // retired on the GPU.
    m_surfaceCache.BeginFrame();
    // [SCEPOCH] (-phaselog_every): does the emit half move the surface-cache publish epoch?
    // Deferring BuildScene_SortAndEmit to the render-owner lane makes the DDGISceneAS gather
    // below read the previous frame's epoch, because MaterialSurfaceCache::AcquirePage, the
    // only epoch writer inside the scene build, sits in SortAndEmit's per-batch emit nest and
    // cannot be hoisted out. The deferral is bit-exact only on frames where the emit half does
    // not move the epoch. Cost: one uint32 read per frame and a tally; no behaviour change.
    //   preBuild  = the epoch BuildScene_SortAndEmit would inherit
    //   postBuild = what DDGISceneAS::Gather reads
    // movedFrames counts frames where they differ, i.e. where a lane-deferred emit would hand
    // Gather a stale epoch and re-arm the retained gather a frame late. beginBumps measures
    // MaterialSurfaceCache::BeginFrame's page-promotion loop, which also moves the epoch and
    // is not affected by the deferral, so movedFrames can be attributed to the emit nest.
    const uint32_t scEpochPreBuild = m_surfaceCache.GetPublishEpoch();
    _bfPre.reset();   // closes BF.PreScene: everything before the scene build
    {
        // The scene build's wall time with its frame (GatherGateStats.h; Lua
        // Profiler.Spawn().gather.ms): what a spawn frame's full gather costs against the patch
        // frames around it.
        const auto tBuild0 = std::chrono::steady_clock::now();
        BuildRenderScene(world);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - tBuild0).count();
        GatherGateStats::cand.buildUs.store(static_cast<uint32_t>(us), std::memory_order_relaxed);
        GatherGateStats::cand.buildFrame.store(FrameViewWrite().currentFrame, std::memory_order_relaxed);
    }
    // The frame plan's stage A for the scene-AS gather below, taken after the scene build so
    // the DDGI volume count is this frame's, as the render half's read of it will be.
    const FramePlan::Decisions gatherPlan = PlanGameStageA();
    {
        const uint32_t scEpochPostBuild = m_surfaceCache.GetPublishEpoch();
        static const int s_scEvery = [] {
            const auto& cl = Sys::CommandLine::Get();
            return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
        }();
        if (s_scEvery > 0)
        {
            static uint64_t s_scFrames = 0, s_scMovedWindow = 0, s_scMovedTotal = 0;
            static uint64_t s_scBumpsWindow = 0, s_scGatherWindow = 0;
            static uint32_t s_scEpochAtLastEmit = 0;
            if (scEpochPostBuild != scEpochPreBuild)
            {
                ++s_scMovedWindow;
                ++s_scMovedTotal;
                s_scBumpsWindow += (scEpochPostBuild - scEpochPreBuild);
            }
            // beginBumps: epoch motion produced between the previous frame's
            // post-build read and this frame's pre-build read, i.e. everything
            // OUTSIDE the scene build (BeginFrame's promotion loop, SetEnabled,
            // OnWorldClear). Reported so movedFrames cannot be misread as "the
            // epoch is unstable" when the instability is upstream of the cut.
            const uint32_t beginBumps =
                (s_scEpochAtLastEmit != 0 && scEpochPreBuild >= s_scEpochAtLastEmit)
                    ? (scEpochPreBuild - s_scEpochAtLastEmit) : 0u;
            s_scEpochAtLastEmit = scEpochPostBuild;
            static uint64_t s_scBeginBumpsWindow = 0;
            s_scBeginBumpsWindow += beginBumps;
            if (gatherPlan.ddgi || gatherPlan.tlas) ++s_scGatherWindow;
            if (++s_scFrames % static_cast<uint64_t>(s_scEvery) == 0)
            {
                LOG_INFO("[SCEPOCH] frame=%llu epoch=%u movedFrames=%llu/%d "
                         "emitBumps=%llu beginBumps=%llu gatherFrames=%llu "
                         "movedTotal=%llu",
                         static_cast<unsigned long long>(frame), scEpochPostBuild,
                         static_cast<unsigned long long>(s_scMovedWindow), s_scEvery,
                         static_cast<unsigned long long>(s_scBumpsWindow),
                         static_cast<unsigned long long>(s_scBeginBumpsWindow),
                         static_cast<unsigned long long>(s_scGatherWindow),
                         static_cast<unsigned long long>(s_scMovedTotal));
                s_scMovedWindow = s_scBumpsWindow = s_scBeginBumpsWindow = s_scGatherWindow = 0;
            }
        }
    }

    // DX12_MATBUF_CHECKSUM (see the section at the top of this file). The table is final
    // here: BuildScene_SortAndEmit is the only writer of m_materialBuffer[GetUploadSlot()]
    // (WriteMaterialSlot under the needsBaseWrite gate, plus the one-off reserved slot 0
    // stage), its queued writes are finished below, and nothing before Render()'s dirty-span
    // blit writes a material slot. So this is the earliest point the bytes are complete and
    // the latest still on the game half.
    // Range = [0, m_persistentMatSlotNext): the high-water of slots ever handed out (slot 0
    // is the reserved neutral record). Slots above it were never written, so hashing up to
    // m_maxMaterials would only fold in the default-PBR pre-fill. The stride comes from
    // sizeof, never a literal, and is logged so a stride change cannot make two runs look
    // equal.
    if (const uint64_t matSumFrame = MatBufChecksumFrame();
        matSumFrame != 0 && frame == matSumFrame)
    {
        if (m_cpuFrame) m_cpuFrame->Finish(); // drain the queued material writes before hashing
        const uint32_t stride = static_cast<uint32_t>(sizeof(Resource::MaterialGPUData));
        const uint32_t slots  = m_persistentMatSlotNext;
        const void*    src    = m_materialBufferMapped[m_gfx.GetUploadSlot()];
        // src null == material staging creation failed; report 0 rather than
        // skipping the line, so a missing checksum always means "frame never
        // reached" and never "buffer was gone".
        const uint64_t sum    = src ? MatBufFnv1a64(src, size_t(slots) * size_t(stride))
                                    : 0ull;

        // Companion field: the custom-material b8 CBV store checksum. The b8/space3 packer does
        // not write the material table, so fnv64 alone does not cover it; this hashes what the
        // packer writes: the b8 bytes of every persistent material slot whose packed content is
        // valid.
        // Gate on the published VA, not m_matSlotCustomPacked: that flag is also 1 for a
        // non-custom key (it means "no further work", not "holds packed bytes"), and a non-custom
        // slot's 256 B in the store are never written - MaterialCBVStore's UPLOAD buffer is not
        // zero-filled at Init, so hashing them would fold in whatever the OS handed back. The slot
        // index is folded in with the bytes so the same bytes at a different slot stay observable.
        // customcb and customcbslots are 0 when no slot was hashed: the persistent store is off or
        // unavailable (the packer's output lived in the per-frame ring and is gone), or nothing is
        // packed.
        uint64_t customSum   = kMatBufFnvBasis;
        uint32_t customSlots = 0;
        // The condition is BuildScene_SortAndEmit's own `persistentCustom`,
        // repeated rather than referenced: with the store valid but the
        // persistent path off, the VAs above point into the per-frame RING and
        // SlotCpu(s) would name store bytes nobody ever wrote.
        if (m_persistentCustomMatStore && m_retainMaterialSlots &&
            m_customMatCbvStore.IsValid())
        {
            for (uint32_t s = 0; s < slots; ++s)
            {
                // Gate on "this slot's VA IS the store's slot VA", not merely
                // "non-zero": a slot that fell back to the per-frame RING (an
                // oversize b8, or SRV-store exhaustion) also carries a non-zero
                // VA, and hashing its store bytes would hash bytes the packer
                // never wrote - a false PASS for a regression on the ring path.
                if (s >= m_matCustomCbvVA.size() ||
                    m_matCustomCbvVA[s] != m_customMatCbvStore.SlotVA(s))
                    continue;
                customSum = MatBufFnv1a64Fold(customSum, &s, sizeof(s));
                // Whole kSlotStride slice. Bytes [cbSize, kSlotStride) are never
                // written by the packer, so after slot REUSE by a smaller-cbSize
                // key they carry the previous key's tail: the value is stable
                // under the pinned determinism args, but it is sensitive to slot
                // reuse history, not only to content. Compare warm-to-warm.
                customSum = MatBufFnv1a64Fold(customSum,
                                              m_customMatCbvStore.SlotCpu(s),
                                              MaterialCBVStore::kSlotStride);
                // The space3 half has no byte image to hash (descriptors), so
                // fold the handle the packer SELECTED for the slot: proves the
                // same SRV block was chosen, which is where both DEVICE_HUNG
                // traps (default-white prefill, bind-by-bindPoint) live.
                if (s < m_matCustomTexHandle.size())
                    customSum = MatBufFnv1a64Fold(customSum, &m_matCustomTexHandle[s],
                                                  sizeof(m_matCustomTexHandle[s]));
                ++customSlots;
            }
        }
        if (customSlots == 0) customSum = 0ull;

        LOG_INFO("[MATBUF] frame=%llu slots=%u stride=%u fnv64=0x%016llx "
                 "customcb=0x%016llx customcbslots=%u",
                 static_cast<unsigned long long>(frame), slots, stride,
                 static_cast<unsigned long long>(sum),
                 static_cast<unsigned long long>(customSum), customSlots);
    }

    // DDGI scene-AS gather: the only half of DDGISceneAS that reads the World. It must run
    // after BuildRenderScene: the retained-gather gate compares the surface cache's publish
    // epoch, which MaterialSurfaceCache::AcquirePage bumps on page eviction and on a
    // content-hash un-publish, and AcquirePage's one caller, BuildScene_SortAndEmit, runs
    // inside BuildRenderScene. Gathering earlier would defer every eviction by a frame (a
    // frame of RT/DDGI shading against an overwritten page).
    // includeSkinned uses the TLAS demand alone; the command-list half of that condition is
    // not known yet here and is applied as a filter in BuildOrRefit.
    // DDGI and the TLAS demand come from the frame plan's stage A (the frame's intent), which
    // the render half's build reads too, so gather and build cannot disagree about a setting
    // changed after CaptureFrameIntent.
    if (gatherPlan.ddgi || gatherPlan.tlas)
    {
        PROF_SCOPE("BF.DDGIGather");
        const uint64_t gateSerial = m_ddgiSceneAS.LastGateSerial();
        m_ddgiSceneAS.Gather(world, m_gfx,
                             m_meshLib, &m_meshMgr,
                             this, &m_skin,
                             /*includeSkinned=*/gatherPlan.tlas);
        // The retained gate's decision, with this frame's number (GatherGateStats.h,
        // frames.jsonl ddgiFull / ddgiFullReason). Nothing when Gather returned
        // before the gate.
        if (m_ddgiSceneAS.LastGateSerial() != gateSerial)
            GatherGateStats::Note(GatherGateStats::ddgi, frame, m_ddgiSceneAS.LastGateFull(),
                                  m_ddgiSceneAS.LastGateReason());
    }
    { PROF_SCOPE("BF.UploadFrameData"); UploadFrameData(frame, vpW, vpH); }

    // Feed this frame's camera to the Aerial Perspective LUT. Must run after UploadFrameData
    // so the LightCB ring slot it mirrors is fresh; an earlier read gets a kFrameCount-stale
    // slot (zeros on the first frames -> NaN rays).
    if (m_skyIBLPass && m_lightCB.Upload(m_gfx))
    {
        auto* lb = m_lightCB.Upload(m_gfx);
        DirectX::XMFLOAT3 apCamPos{ lb->cameraPos[0], lb->cameraPos[1], lb->cameraPos[2] };
        DirectX::XMFLOAT3 apCamFwd{ lb->cameraForward[0], lb->cameraForward[1], lb->cameraForward[2] };
        m_skyIBLPass->SetCameraForAerial(apCamPos, apCamFwd, lb->invViewProj);
    }

    // Same for the volumetric cloud pass: push the camera from the freshly written LightCB
    // slot (a stale slot drags cloud ray reconstruction behind camera motion).
    // vpW/vpH gate: UploadFrameData early-returns on a zero-sized viewport without writing
    // the slot, so skip the push then and keep the last camera.
    if (vpW != 0 && vpH != 0 && m_cloudPass && m_lightCB.Upload(m_gfx))
    {
        auto* lb = m_lightCB.Upload(m_gfx);
        DirectX::XMFLOAT4X4 cloudInvVP;
        std::memcpy(&cloudInvVP, lb->invViewProj, sizeof(cloudInvVP));
        DirectX::XMFLOAT3 cloudCamPos{ lb->cameraPos[0], lb->cameraPos[1], lb->cameraPos[2] };
        m_cloudPass->SetCamera(cloudInvVP, cloudCamPos, FrameViewWrite().camera.nearZ, FrameViewWrite().camera.farZ);
    }

    // This frame's HDR scene-color clear (set by SyncSkyboxIBL from the WorldEnvironment
    // mode): sky modes -> black (the sky pass paints the background); None/FlatColor -> the
    // scene's flat background colour. It travels with the frame (FrameView::hdrClearColor,
    // bound with the graph's HDR import), so the device's slot copy is written only for the
    // -rgimportoff arm, which has no import.
    if (!RG::RenderGraph::ImportsEnabled())
    {
        ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == 0,
                          "Renderer SetHdrClearColor: HDR access inside an HDR push scope (depth=%d)",
                          m_gfx.HdrScopeDepth());
        m_gfx.SetHdrClearColor(FrameViewWrite().hdrClearColor);
    }

    // Tracer CPU bookkeeping: must run before TracerSimPass::Execute reads it in Render().
    if (m_tracerSystem)
        m_tracerSystem->BeginFrame(worldDt, static_cast<uint32_t>(frame), FrameViewWrite().globalTimeSec);

    // Beam system: upload control points + params before BeamSimPass::Execute.
    if (m_beamSystem)
    {
        // Patch global time into every active beam's params so wobble runs everywhere.
        m_beamSystem->BeginFrame(FrameViewWrite().globalTimeSec);
    }

    if (m_particleSystem)
    {
        // Re-wire resource systems each frame: InitSkinningSystems (which creates the
        // ParticleSystem) runs during Compile(), before App calls
        // SetTextureSystem/SetResourceManager, so a one-time wire at init would capture nullptrs
        // and emitter textures would never resolve. BillboardFXPass does the same.
        m_particleSystem->SetResourceSystems(m_texSys, m_resMgr);
        // Pass this frame's resolved camera position: ambient wrap volumes follow it (editor
        // fly-cam in edit mode, VCam stack in play). SetCamera ran before BeginFrame, so the
        // FrameView camera is fresh.
        m_particleSystem->CollectEmitters(world, worldDt, static_cast<uint32_t>(frame), FrameViewWrite().camera.position);
        m_particleSystem->UpdateSystemCB(worldDt, static_cast<uint32_t>(frame));

        // Resolve pending mesh-shape emitters -> bindless mesh-desc + world matrix.
        for (const auto& req : m_particleSystem->GetPendingMeshResolves())
        {
            const Entity srcE = req.sourceEntity;
            if (srcE == NullEntity) continue;

            DirectX::XMFLOAT4X4 worldMat;
            DirectX::XMStoreFloat4x4(&worldMat, DirectX::XMMatrixIdentity());
            if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(srcE))
                worldMat = gt->matrix;

            uint32_t descSlot   = 0xFFFFFFFFu;
            uint32_t indexCount = 0;

            // Try MeshLibRef path (scene meshes via MeshLibrary).
            if (MeshLibRef* mlr = world.GetComponent<MeshLibRef>(srcE);
                mlr && mlr->IsValid() && m_meshLib)
            {
                descSlot = m_meshMgr.RegisterMeshLibMesh(*m_meshLib, *mlr);
                if (const auto* entry = m_meshLib->GetEntry(mlr->libHandle, mlr->meshId))
                    indexCount = entry->indexCount;
            }
            // Fallback: primitive MeshHandle (cube/sphere/cone etc).
            else if (const MeshHandle* mh = world.GetComponent<MeshHandle>(srcE);
                     mh && mh->IsValid()
                     && mh->gpuMeshID < static_cast<uint32_t>(PrimitiveMeshType::Count))
            {
                const auto& mesh = m_meshMgr.GetPrimitive(mh->gpuMeshID);
                descSlot   = mesh.meshDescSlot;
                indexCount = mesh.indexCount;
            }

            if (descSlot != RHI::kInvalidBufferIndex && indexCount >= 3)
            {
                m_particleSystem->PatchMeshEmitter(
                    req.emitterSlotIndex, descSlot, indexCount, worldMat);
            }
        }
    }

    // Trail system: CPU-side per-frame bookkeeping.
    if (m_trailSystem)
    {
        m_trailSystem->SetResourceSystems(m_texSys, m_resMgr);
        m_trailSystem->CollectTrails(world, worldDt);
        m_trailSystem->UpdateSystemCB(worldDt);
    }

    // Animated billboard FX: advance flipbooks and expand camera-facing quads. Runs here
    // because world, dt and the FrameView view filled by UploadFrameData are all ready; the
    // pass draws in-graph into HDR with read-only depth.
    if (m_billboardFXPass)
    {
        m_billboardFXPass->SetResourceSystems(m_texSys, m_resMgr);
        m_billboardFXPass->BuildFrame(world, worldDt,
                                      FrameViewWrite().view.viewMatrix,
                                      FrameViewWrite().view.cameraPosition);
        // The secondary views' expansions: each faces its own camera.
        for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        {
            const SecondaryFrameView& s = FrameViewWrite().secondary[slot - 1];
            SecondaryView& v = m_secondary[slot];
            if (s.slot != slot || !v.ready || !v.state) continue;
            FrameView fv = FrameViewWrite();
            fv.camera   = s.camera;
            fv.vpWidth  = s.vpWidth;
            fv.vpHeight = s.vpHeight;
            DirectX::XMMATRIX view, proj;
            BuildCameraViewProjFor(fv, view, proj);
            DirectX::XMFLOAT4X4 viewF;
            DirectX::XMStoreFloat4x4(&viewF, view);
            m_billboardFXPass->ExpandView(*v.state, viewF, s.camera.position);
        }
    }

    // Pre-register even while reflections are disabled: the render-side
    // readiness/TLAS decision may change, but must never allocate heap slots.
    // At most two world-scoped slots, reused until OnWorldClear invalidates.
    { PROF_SCOPE("BF.ReflectionBindings"); PrepareReflectionBindings(); }

    // End of the game half's scene build: hand the draw list to the render half and move the
    // game half onto the other buffer. Last in BeginFrame because every game-half writer of
    // the list (BuildRenderScene's clear, SortAndEmit's push_backs, the shadow-group
    // classification that indexes back into it) runs above.
    // -brslane hand-off: only the index half of the publish runs here, on main.
    // AdvanceSceneWrite names the buffer the frame in flight owns (m_emitIdx already points at
    // it) and moves main onto the other one, so SetCamera(F+1) and BuildGameView(F+1), which
    // run inside the window the moved Animation barrier opens, write where the lane is not
    // reading. The seed copy is correct because UploadFrameData stays on main: the FrameView
    // is final for F here. m_sceneRead is not touched: its only writer is the lane's
    // PublishSceneRead(pkt.emitIdx), the last step of the lane's preparation.
    if (m_emitDeferred) {
        const uint32_t advanced = AdvanceSceneWrite();
        ENGINE_CHECK_ONCE(advanced == m_emitIdx,
            "-brslane hand-off: AdvanceSceneWrite published index %u but this "
            "frame's emit lease is %u - the lane is about to finish a payload "
            "main has already moved off", advanced, m_emitIdx);
        // The skin payload's index half moves at the same instant. It is a renderer member, not a
        // packet field, because App builds the packet and the skin cursor is the subsystem's;
        // both are ordered by this hand-off.
        m_laneSkinIdx  = m_skin.AdvanceSkinJobsWrite();
        m_laneEmitOwed = true;
        return;
    }
    if (m_cpuFrame) {
        // Keep both scene indices and the skin payload unpublished while App
        // extracts independent UI/debug data. Workers own their packing inputs;
        // no GPU consumer can run until FinishFramePreparation and packet release.
        m_pendingPublication = m_cpuFrame;
        return;
    }
    PublishRenderScene();
    // The skin payload is published at the same instant. BuildSkinJobs filled it in the
    // Animation phase; publishing it there would hand the render half the next frame's jobs
    // while it is still drawing this one.
    m_skin.PublishSkinJobs();
}

void Renderer::FinishFramePreparation(Sys::FrameTaskContext& cpuFrame)
{
    ZoneScopedN("Renderer.FramePrepare.CommitBoundary");
    // Which half of the seam finished this frame, read BEFORE the publish
    // clears the latch. The oracle at the bottom compares it against the run's
    // shape, so "the lane was supposed to do this and main did" is an assertion
    // rather than something a reader has to notice in a log.
    const bool finishedOnLane = m_laneEmitOwed;
    // The leased scene index, read before the publish. The emit-half oracle asserts that the
    // payload now visible to the render half is this frame's; comparing against m_emitIdx
    // would be true by construction (PublishSceneRead is called with it), so both terms come
    // from elsewhere and earlier. Lane arm: m_laneLeaseEmitIdx, what App put in the
    // FramePacket on main at the hand-off. Default arm: m_sceneWrite, the buffer the game half
    // built into, sampled before AdvanceSceneWrite moves it.
    const uint32_t leasedEmitIdx = finishedOnLane ? m_laneLeaseEmitIdx : m_sceneWrite;
    // The lease check against the frame in flight: once main has run ahead,
    // FrameViewWrite() and m_gfx.GetUploadSlot() describe F+1, so assert that the payload
    // about to be published is the one this context leased (FrameViewEmit(),
    // m_frameUploadSlot). On the default path the two pairs are equal.
    if (m_pendingPublication != &cpuFrame ||
        FrameViewEmit().currentFrame != cpuFrame.Generation() ||
        m_frameUploadSlot != cpuFrame.UploadSlot() || !m_gfx.IsUploadSlotWritable())
        throw std::logic_error("Frame publication has a stale context/generation/upload lease");
    // Errors leave publication closed; the context drains accepted work before
    // stack unwind releases inputs. Render must never consume a partial commit.
    cpuFrame.Finish();
    if (m_laneEmitOwed)
    {
        // -brslane: the index half already ran on main at the hand-off; only the payload half is
        // left, and the two indices must differ. Equal indices mean the hand-off did not advance,
        // which would put main's SetCamera(F+1) and this publish in the same buffer.
        ENGINE_CHECK_ONCE(m_emitIdx != m_sceneWrite,
                          "-brslane: FinishFramePreparation is publishing index %u "
                          "while main is still building into it - AdvanceSceneWrite "
                          "did not run at the hand-off", m_emitIdx);
        PublishSceneRead(m_emitIdx);
        m_skin.PublishSkinJobsRead(m_laneSkinIdx);
        m_laneEmitOwed = false;
    }
    else
    {
        // Default path: preparation finished on main, so the index half runs here too (under
        // -brslane it runs at the hand-off and only PublishSceneRead stays). The check pins the
        // equality that makes the two forms equivalent.
        ENGINE_CHECK_ONCE(m_emitIdx == m_sceneWrite,
                          "FinishFramePreparation: emit index %u is not the write index %u",
                          m_emitIdx, m_sceneWrite);
        PublishRenderScene();
        m_skin.PublishSkinJobs();
    }
    m_pendingPublication = nullptr;
    if (Sys::CommandLine::Get().Has("-materialcpuverify"))
        LOG_INFO("[FRAME_PREPARE_PUBLISH] frame=%llu slot=%u match=1",
            static_cast<unsigned long long>(cpuFrame.Generation()), cpuFrame.UploadSlot());
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -brslaneverify=N: digest here, not at the end of the emit half. The COMMIT stage inside
    // cpuFrame.Finish() above writes the material table, so an earlier digest would hash
    // however much of it a worker had written.
    const bool selfTestFrame = m_brsLaneSelfTestFrame >= 0 &&
        cpuFrame.Generation() == static_cast<uint64_t>(m_brsLaneSelfTestFrame);
    if (m_brsLaneVerifyEvery > 0 &&
        (++m_brsLaneVerifyFrames % static_cast<uint64_t>(m_brsLaneVerifyEvery) == 0 ||
         selfTestFrame))   // the control must fire on ITS frame, not on a multiple
        ReportEmitHalfDigest(cpuFrame.Generation(), finishedOnLane, leasedEmitIdx);
#else
    (void)finishedOnLane; (void)leasedEmitIdx;
#endif
}

// -brslane: the whole lane-side half of the frame.
// Called by FramePipeline::RenderOwnerLoop on the render-owner lane, inside its ThreadCtx::Render
// scope and device-execution token, before RenderHalfAsync starts, so the lane's
// OwnerTaskQueue::RunUntil has not begun and nothing here is a recursive pump.
// The FrameTaskContext is created here, not handed over: it binds to its creating thread
// and CheckOwner() throws for every other one, and the lane's queue is the one whose
// continuations the lane can pump.
// Not here: UploadFrameData, DDGISceneAS::Gather and stages 9-13 of BuildRenderScene stay
// on main (UploadFrameData there keeps the FrameView final before AdvanceSceneWrite);
// AcquirePersistentMatSlot / AcquirePage / MapEntity run on main inside ResolveBatchPlan,
// which -brslane forces on.
// Not on main: SweepPersistentMatSlots (the persistent material slot GC) is the last step
// of BuildScene_EmitBatches, so under -brslane it runs here and mutates the slot table
// main otherwise keeps. Safe by program position, not ownership: the only other toucher is
// ResolveBatchPlan in BeginFrame(F+1), and the moved Animation barrier joins this lane
// first - one thread at a time, ordered by the drain.
void Renderer::PrepareFrameOnLane(Sys::OwnerTaskQueue& queue, uint64_t frame,
                                  uint32_t uploadSlot, uint32_t emitIdx)
{
    ZoneScopedN("Renderer.PrepareFrameOnLane");
    // Three things main promised at the hand-off, checked on the thread about to act on
    // them. The lease terms are the packet's; re-reading them off the renderer here would
    // compare a value with itself.
    if (!m_laneEmitOwed)
        throw std::logic_error("-brslane: PrepareFrameOnLane without a deferred emit half");
    if (m_frameUploadSlot != uploadSlot || m_emitIdx != emitIdx)
        throw std::logic_error("-brslane: the packet's emit lease is not this frame's");
    // Kept from the PACKET, not re-read off the renderer: FinishFramePreparation
    // compares the published index against it, and a copy of m_emitIdx taken
    // here would put that comparison back where it started.
    m_laneLeaseEmitIdx = emitIdx;

    Sys::FrameTaskContext laneFrame(queue, frame, uploadSlot);
    // m_cpuFrame is what QueueFrameMaterialWrites submits through and
    // m_pendingPublication is what FinishFramePreparation and RenderAsync gate
    // on; both are main's on the default path and the lane's here. Nulled on
    // EVERY exit, including a throwing oracle, so nothing downstream can
    // compare against a context whose storage has gone.
    m_cpuFrame           = &laneFrame;
    m_pendingPublication = &laneFrame;
    struct LaneFrameGuard {
        Renderer* r; Sys::FrameTaskContext* ctx;
        ~LaneFrameGuard() {
            r->m_cpuFrame = nullptr;
            if (r->m_pendingPublication == ctx) r->m_pendingPublication = nullptr;
        }
    } laneFrameGuard{ this, &laneFrame };

    // -brsplanlane: the plan half runs here too when main handed it over. The per-frame lane
    // flags are cleared on every exit, after the publish that reads them; PlanThrowGuard
    // below, like BuildScene_SortAndEmit's, closes the scopes a throwing plan started.
    struct LanePlanFlagGuard {
        Renderer* r;
        ~LanePlanFlagGuard() {
            r->m_planOnLaneThisFrame      = false;
            r->m_classifyOnLaneThisFrame  = false;
            r->m_lightPrepOnLaneThisFrame = false;
            r->m_laneAssembleThisFrame    = false;
        }
    } lanePlanFlagGuard{ this };
    // Lane assembly: main handed over only the rows it produced this frame;
    // AssembleLaneCandidates copies the cached static rows in before anything reads the list.
    // Only together with the lane tracking + classify (both need the whole list).
    if (m_laneAssembleThisFrame &&
        (!m_classifyOnLaneThisFrame || !m_lightPrepOnLaneThisFrame))
        throw std::logic_error("M9.2: a lane assembly without the lane tracking + classify");
    // The list everything below reads: the lane's assembled one, or main's.
    std::vector<DrawCandidate>& laneList =
        m_laneAssembleThisFrame ? AssembleLaneCandidates() : m_candScratch;
    // What main handed over ahead of the plan, in main's order: the spot-caster tracking over
    // the unclassified rows, the classify, then the light prep that consumes the tracking.
    // Main hands the classify over only with the plan, and the light work only with the
    // classify; any other combination is an edit that broke the pairing.
    if (m_lightPrepOnLaneThisFrame && !m_classifyOnLaneThisFrame)
        throw std::logic_error("M9.4: a lane light prep without a lane classify");
    if (m_classifyOnLaneThisFrame)
    {
        if (!m_planOnLaneThisFrame)
            throw std::logic_error("M9.4: a lane classify without a lane plan");
        if (m_lightPrepOnLaneThisFrame)
        {
            ZoneScopedN("BRS.SpotCasters.Lane");
            PROF_SCOPE("BRS.SpotCasters.Lane");
            TrackSpotShadowCasters(laneList, m_laneSpotRetainedDiff,
                                   m_laneSpotMatTexUnsettled, /*laneForm=*/true);
        }
        RunLaneClassify(laneList);
        if (m_lightPrepOnLaneThisFrame)
        {
            ZoneScopedN("BRS.LightData.Lane");
            PROF_SCOPE("BRS.LightData.Lane");
            // The captured slot IS the lease main latched in BeginFrame(F):
            // both were GetUploadSlot() inside the same BuildRenderScene.
            if (m_laneLightIn.uploadSlot != m_frameUploadSlot)
                throw std::logic_error("M9.4: the light prep's upload slot is not the frame's lease");
            BuildScene_PrepareLightData(m_laneLightIn, /*clusterMetaDone=*/true);
            LogLightPrepDigest(m_laneLightIn, "lane");
        }
    }
    if (m_planOnLaneThisFrame)
    {
        struct PlanThrowGuard {
            Renderer* r;
            bool      armed = true;
            ~PlanThrowGuard() {
                if (!armed) return;
                r->m_emitCtx.planScope.reset();
                r->m_emitCtx.materialTailScope.reset();
                r->m_emitCtx.earlyMaterialScope.reset();
                r->FinishSortEmitZones();
            }
        } planThrowGuard{ this };
        ZoneScopedN("BRS.PlanEmit.Lane");
        PROF_SCOPE("BRS.PlanEmit.Lane");
        const bool atlasBefore = m_surfaceCache.AtlasReady();
        const auto t0 = std::chrono::steady_clock::now();
        BuildScene_PlanEmit(laneList);
        m_lanePlanMs = static_cast<float>(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
        planThrowGuard.armed = false;
        // PrepareLanePlan's prediction is a superset of the page gate, so the
        // atlas can only ever be created on main. If this fires, a gate term
        // changed without the prediction and a CreateTexture ran on the lane.
        ENGINE_CHECK_ONCE(atlasBefore || !m_surfaceCache.AtlasReady(),
            "[BRSPLANLANE] the surface-cache atlas was created ON THE LANE - "
            "PrepareLanePlan's page-gate prediction missed a candidate");
    }

    // Half two, the emit. laneList is the vector BuildScene_PlanEmit sorted an index
    // permutation over; its next writer is BuildRenderScene(F+1) for m_candScratch, strictly
    // after the Animation-boundary drain that joins this lane (ordered by program position,
    // not timing), or this lane's next assembly for m_candLaneScratch.
    RunEmitHalf(laneList);
    FinishFramePreparation(laneFrame);
}
