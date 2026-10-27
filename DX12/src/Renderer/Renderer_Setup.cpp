// Renderer_Setup.cpp - how a Renderer comes to be and goes: the constructor (pool caps), the
// destructor, Compile (the render graph's passes and the subsystems outside it), CreateConstantBuffers
// and InitSkinningSystems.
// PostSetupClock / g_postSetup ([POSTSETUP]) are file-local: Compile sets the clock and
// InitSkinningSystems books its steps on it.

#include "Diagnostics/Profiler.h"
#include <optional>
#include "System/PathUtil.h"   // Sys::HasArtifactDir - gates the survivor readback
#include "Renderer/Renderer.h"
#include "Renderer/LayoutSpecs.h"     // RegisterLayoutSpecs
#include "Graphics/LayoutCheck.h"     // Layout::LogSummary
#include "Graphics/ShaderLibrary.h"   // ShaderLibrary::Log*Summary
#include "Scene/MeshAssetRegistry.h"
#include "Resource/MeshLibIdentity.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/MemoryPolicy.h"
#include "Graphics/ReSTIRTransientTextures.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"
#include "Graphics/SSR/SSRSubsystem.h"
#include "Graphics/GPUInstanceData.h"

// render passes
#include "Renderer/Preview/PreviewRenderer.h"
#include "Renderer/Preview/PreviewStudioRig.h"
#include "RenderGraph/PassTextureNames.h"
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
#include "PostProcess/ResolveEffects.h"
#include "PostProcess/OverlayEffects.h"
#include "PostProcess/PostProcessRuntime.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"

// engine systems
#include "Module/ModuleRegistry.h"   // the enabled modules' render features
#include "System/Log.h"
#include "System/BootTimeline.h"   // Compile's sub-phase mark
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/TaskSystem.h"
#include "System/EventBus.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace); their layout static_asserts
// sit there and in Renderer.cpp.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;


// ==== Construction and teardown ====
// The pool caps arrive by value and are validated, not trusted: every buffer size, bounds test and
// census reads the members, and a cap the fixed-size inline array cannot honour would be an
// out-of-bounds write. Degrade, never abort (System/Check.h).
// Assigned in the body, not the mem-init list, so the clamps run before any derived value is computed
// and declaration order cannot bite.
Renderer::Renderer(IGraphicsDevice& gfx, const RendererPoolCaps& caps)
    : m_gfx(gfx)
{
    // Register the C++ structs every shader load checks its mirror against (Graphics/LayoutCheck.h)
    // before any pass loads one.
    RendererDetail::RegisterLayoutSpecs();

    m_maxInstances = caps.maxInstances;
    if (!ENGINE_CHECK(m_maxInstances > 0 &&
                      m_maxInstances <= kMaxInstancesUpperBound,
                      "Renderer: maxInstances %u outside (0, %u]; clamping. "
                      "m_instanceSlotToEntity is a fixed array of that bound "
                      "and the cmd-group meta packs command indices into 16 "
                      "bits.",
                      m_maxInstances, kMaxInstancesUpperBound))
        m_maxInstances = kMaxInstancesUpperBound;

    m_maxMaterials = caps.maxMaterials;
    if (!ENGINE_CHECK(m_maxMaterials > 0 &&
                      m_maxMaterials <= kMaxMaterialsUpperBound,
                      "Renderer: maxMaterials %u outside (0, %u]; clamping. "
                      "GBuffer normal.a carries matIdx as a 16-bit UNORM "
                      "integer and DDGISceneAS packs slot+1 into 16 bits.",
                      m_maxMaterials, kMaxMaterialsUpperBound))
        m_maxMaterials = kMaxMaterialsUpperBound;

    // Static lane = everything the dynamic reserve does not claim. Guarded because a preview-sized pool can
    // be smaller than the reserve, and the unsigned wrap would give the static allocator a ~4-billion cap.
    // Default: 32768 - 8192 = 24576.
    m_staticInstanceCap = (m_maxInstances > kDynamicInstanceReserve)
                        ? (m_maxInstances - kDynamicInstanceReserve)
                        : (m_maxInstances / 2u);

    // -shadow_hook_off: the reverse arm of the masked hook shadow - every masked caster back on Shadow.ps
    // (ShadowHookEnabled()).
    m_shadowHookEnabled = !Sys::CommandLine::Get().Has("-shadow_hook_off");
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -matpack_dump: the material binding dump (s_matPackDump).
    s_matPackDump = Sys::CommandLine::Get().Has("-matpack_dump");
#endif
}

Renderer::~Renderer()
{
    if(m_restirBudgetWant) m_gfx.ReleaseGPUProfilerWant();
    // The mesh-descriptor sharing metric, before anything is released: a session ledger, reported while
    // the counters are still the session's (MeshManager's own teardown comes later).
    m_meshMgr.ReportMeshLibDescOwnership();
    // How many shaders each mirrored struct was checked against this run.
    Layout::LogSummary();
    // How many shader closures were checked against the relevance table.
    ShaderLibrary::LogRelevanceSummary();
    // How many surface-material variants were checked against their slots.
    ShaderLibrary::LogMaterialSlotSummary();
    // How the frame plan compared with the live decisions (FramePlan.h).
    if (m_planFrames)
        LOG_INFO("[FRAMEPLAN] summary: frames=%llu changes=%llu fallbacks=%llu vetoes=%llu",
                 static_cast<unsigned long long>(m_planFrames), static_cast<unsigned long long>(m_planChanges),
                 static_cast<unsigned long long>(m_planFallbacks), static_cast<unsigned long long>(m_planVetoes));

    // ---- The in-game PiP's view, handed back before the layer ----
    // The editor's panels close themselves in App::ShutdownAllSystems (EditorLayer::ReleasePreviewViews),
    // which makes "no live view slots at teardown" a real invariant. The PiP has no panel (its only driver
    // is the SetGamePiP call that armed it), so Game.exe with -preview_pip reaches here holding a view.
    // Release it and drain: ServicePreviewReleases will not run again, and a released-but-parked slot
    // would trip the pendingDestroy ENGINE_CHECK in ~PreviewRenderer.
    if (m_preview && m_pipView != Preview::kInvalidView)
    {
        SetPreviewCameraEntity(m_pipView, NullEntity);
        ReleasePreviewView(m_pipView);
        LOG_INFO("[PREVIEW] shutdown: game PiP released (view %u)", m_pipView);
        m_pipView   = Preview::kInvalidView;
        m_pipEntity = NullEntity;
        m_preview->DrainPendingViewDestroys();
    }

    // Safety path for "world outlives renderer": skip listener removal, since m_subscribedWorld may dangle
    // (the scene is usually torn down first).
    m_subscribedWorld         = nullptr;
    m_entityDestroyListenerId = 0;

    // Shutdown worker threads before releasing GPU resources.
    if (m_workersStarted)
        for (auto& w : m_renderWorkers)
            w.Shutdown();

    // Stopping CPU workers does not retire queued GPU work. Raw RT passes have
    // no NRD teardown to incidentally drain it, and their PSOs/root signatures/
    // indirect command signatures are released immediately rather than deferred.
    m_gfx.WaitIdleAndReleaseDeferred();
    LOG_INFO("Renderer: GPU queues drained before pass shutdown");
    // Every pass and system that registered OnShutdown where Compile created
    // it (RenderLifecycle.h), in creation order: after the drain above, before
    // the device goes, each releasing only what it created.
    m_lifecycle.Run(RenderLifecycle::Event::Shutdown);
    // The views' histories (RenderGraph/ViewState.h): after the passes that used them shut down, before
    // the device goes - the secondary views' with the rest of their resources.
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        if (m_secondary[slot].ready) FreeViewResources(slot);
    m_mainView.ReleaseHistories(m_gfx);

    // Triple-buffered CBs: FrameCB::Destroy unmaps + releases each slot.
    m_perObjectCB.Destroy(m_gfx);
    m_lightCB    .Destroy(m_gfx);
    m_fogApplyCB .Destroy(m_gfx);
    m_mediumSet  .Release(m_gfx);
    m_terrainCB  .Destroy(m_gfx);
    m_meshletCullCB.Destroy(m_gfx);

    // Triple-buffered non-CB upload buffers.
    for (uint32_t i = 0; i < kFrameSlots; ++i)
    {
        if (m_instanceBufferMapped[i])  m_gfx.UnmapBuffer(m_instanceBuffer[i]);
        if (m_instanceRemapMapped[i])   m_gfx.UnmapBuffer(m_instanceRemapBuffer[i]);
        if (m_spotShadowVPMapped[i])    m_gfx.UnmapBuffer(m_spotShadowVPBuffer[i]);
        if (m_terrainLayerMapped[i])    m_gfx.UnmapBuffer(m_terrainLayerBuffer[i]);
        if (m_indirectArgMapped[i])     m_gfx.UnmapBuffer(m_indirectArgUpload[i]);
        if (m_drawCountMapped[i])       m_gfx.UnmapBuffer(m_drawCountUpload[i]);
        if (m_cmdGroupMetaMapped[i])    m_gfx.UnmapBuffer(m_cmdGroupMeta[i]);
        if (m_shadowCmdMetaMapped[i])   m_gfx.UnmapBuffer(m_shadowCmdMeta[i]);
        if (m_materialBufferMapped[i])  m_gfx.UnmapBuffer(m_materialBuffer[i]);
        if (m_matFrameRecordMapped[i])  m_gfx.UnmapBuffer(m_matFrameRecord[i]);
    }

    // Release the reflection-probe ring buffers and cube SRV block (the cubemap-array texture itself is
    // freed by the RHI on Renderer teardown).
    m_probeMgr.Shutdown(m_gfx);

    // Material surface-cache atlases (a no-op when the feature was never used: the atlases are allocated
    // on the first page request, not at Compile). The bake pass's own half went with the lifecycle list.
    m_surfaceCache.Shutdown(m_gfx);

    // -dxr_parity's pipelines and buffers, when the run ended before its readback.
    m_dxrParity.Shutdown(m_gfx);

    // Release all renderer-owned texture handles.
    if (m_texSys)
    {
        // Each shared runtime material entry owns exactly one TextureSystem ref
        // per non-empty texture, regardless of how many entities bind it.
        for (auto& [_key, entry] : m_runtimeMaterialResources)
        {
            for (auto& texture : entry.engineTextures)
                if (texture.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(texture.handle, m_gfx);
            for (auto& [_name, texture] : entry.customTextures)
                if (texture.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(texture.handle, m_gfx);
        }

        // Shutdown can occur after OnWorldClear but before the next material
        // sync. Balance the outgoing world's deferred refs in that case.
        for (Resource::TextureHandle handle : m_pendingMatTexReleaseAfterNextSync)
            if (handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(handle, m_gfx);

        // Release terrain textures (heightmap + splatmap + 4 maps per layer, per entity).
        for (auto& [entity, cache] : m_terrainTexCache)
        {
            auto rel = [&](TerrainTexSlot& slot) {
                if (slot.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(slot.handle, m_gfx);
            };
            rel(cache.heightmap);
            rel(cache.splatmap);
            for (auto& l : cache.layers)
            {
                rel(l.albedo);
                rel(l.normal);
                rel(l.arm);
                rel(l.disp);
            }
        }

        // Release water flow-normal map handles (all bodies).
        for (auto& [_e, slots] : m_waterTexCache)
        {
            if (slots.a.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(slots.a.handle, m_gfx);
            if (slots.b.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(slots.b.handle, m_gfx);
        }

        // Release skybox IBL texture handles.
        for (auto& entry : m_skyboxTexCache)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, m_gfx);

        // Release the sky material textures and the BRDF LUT.
        for (auto& [path, entry] : m_skyMaterialTextures)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, m_gfx);
        if (m_brdfLutEntry.handle != Resource::kInvalidTextureHandle)
            m_texSys->Release(m_brdfLutEntry.handle, m_gfx);

        // Release the material preview's studio IBL pair. Engine-lifetime like
        // the BRDF LUT above (OnWorldClear never touches either), so this is
        // the one place they are handed back.
        if (m_previewIrrHandle != Resource::kInvalidTextureHandle)
            m_texSys->Release(m_previewIrrHandle, m_gfx);
        if (m_previewRadHandle != Resource::kInvalidTextureHandle)
            m_texSys->Release(m_previewRadHandle, m_gfx);

        // Release decal material textures; dangling DecalComponent shared_ptrs become inert.
        m_decalMaterialLibrary.Shutdown(*m_texSys, m_gfx);
    }

    // Release the engine-global dissolve noise texture (a renderer-owned RHI texture, not a TextureSystem
    // handle, so independent of m_texSys).
    if (m_dissolveNoiseTex.IsValid())
        m_gfx.DestroyTexture(m_dissolveNoiseTex);

    // ---- The preview asset cache and its pending hand-backs ----
    // OnWorldClear releases every entry in m_previewMeshLibs and drops the hand-back queue; ~Renderer does
    // the same, or a session ending with a preview panel open would keep one MeshLibrary reference (and
    // its VB+IB pair) per cached library.
    // Bare Release, not ReleasePreviewMeshLib, as in OnWorldClear: the hand-back machinery hands slot
    // indices to a MeshDescriptorHeap that is destroyed with us.
    if (m_meshLib && !m_previewMeshLibs.empty())
    {
        for (auto& [path, e] : m_previewMeshLibs)
            if (e.handle.IsValid()) m_meshLib->Release(e.handle, m_gfx);
        LOG_INFO("[PREVIEW] shutdown: released %zu cached preview mesh librar%s",
                 m_previewMeshLibs.size(),
                 m_previewMeshLibs.size() == 1 ? "y" : "ies");
    }
    m_previewMeshLibs.clear();
    // The queue is dropped, not drained, as at a world boundary: its slot indices name free lists inside a
    // MeshDescriptorHeap that dies in this teardown. Report how many were still queued: a queue that never
    // drains is otherwise visible only in a log line that may never have been reached.
    // Empty is not required: an entry queued in the session's last kFramesInFlight+1 frames is by design
    // still here (the drain frees it only once the frames that could have recorded its slots retire). The
    // defect is an overdue entry, one the drain was obliged to free and did not
    // (DrainPreviewMeshLibHandbacks stopped running): assert on the overdue count, report the in-flight ones.
    // Two independent overdue tests, since either alone can be fooled at shutdown: stamp <= GetRetiredFrame()
    // is the drain's own predicate, and the queuedFrame term uses the preview's own frame counter, which
    // still moves if the device counter stalled.
    {
        const uint64_t retired = m_gfx.GetRetiredFrame();
        std::size_t overdue = 0;
        for (const auto& p : m_previewMeshLibHandbacks)
            if (p.stampFrame <= retired ||
                p.queuedFrame + RHI::kFramesInFlight + 1 <= m_previewAssetFrame)
                ++overdue;
        ENGINE_CHECK(overdue == 0,
            "Renderer: %zu preview mesh-library handback(s) were OVERDUE at "
            "shutdown (of %zu still queued) - DrainPreviewMeshLibHandbacks "
            "stopped running (it is called from the top of BuildRenderScene), "
            "so their descriptor slots were never returned to the heap during "
            "this session.",
            overdue, m_previewMeshLibHandbacks.size());
        if (!m_previewMeshLibHandbacks.empty() && overdue == 0)
            LOG_INFO("[PREVIEW] shutdown: %zu mesh-library handback(s) still "
                     "legitimately in flight (queued within the last "
                     "kFramesInFlight+1 frames; retiredFrame=%llu) - dropped "
                     "with the heap they index",
                     m_previewMeshLibHandbacks.size(),
                     static_cast<unsigned long long>(retired));
    }
    m_previewMeshLibHandbacks.clear();
}

namespace
{
// [POSTSETUP]: what the setup outside the render graph costs, step by step (the counterpart of the
// graph's [RGSETUP]). Done() books the time since the previous step to its name. InitSkinningSystems
// books its own steps through g_postSetup, which is set only while Compile runs (main thread).
struct PostSetupClock
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now();
    Clock::time_point mark  = start;
    std::vector<std::pair<const char*, double>> steps;
    void Done(const char* name)
    {
        const Clock::time_point now = Clock::now();
        steps.emplace_back(name, std::chrono::duration<double, std::milli>(now - mark).count());
        mark = now;
    }
};
PostSetupClock* g_postSetup = nullptr;
void PostDone(const char* name) { if (g_postSetup) g_postSetup->Done(name); }

// The lifecycle entry (Renderer/RenderLifecycle.h) of a standalone pass held by a unique_ptr member
// that rebuilds its own shaders on a hot reload. The callback reads the member when it runs, so a pass
// reset after registration is skipped.
template <class T>
RenderLifecycle::Entry& AddReloadable(RenderLifecycle& lifecycle, const char* name,
                                      std::unique_ptr<T>& owner, IGraphicsDevice& gfx)
{
    return lifecycle.Add(name, owner.get())
        .OnReloadShaders([&owner, &gfx] { if (owner) owner->ReloadShaders(gfx); });
}
}

// ==== Compile ====
void Renderer::Compile()
{
    // Every RenderPass constructed from here on must end up in the main graph or in m_lifecycle; the end
    // of Compile checks it.
    const uint64_t passCensusMark = RG::PassCensusMark();
    // The view a history-keeping pass (kView) binds to as it registers, set before any pass exists:
    // RTReflectionPass makes its history in the main graph's first Compile, mid-way through this one.
    m_passes.SetView(m_mainView);

    // The render features (Renderer/RenderFeature.h): every one the generated list names, before anything
    // they take part in.
    RegisterRenderFeatures(m_features);
    // ...and the enabled modules' (a module's feature is not in the generated list: the list is
    // compiled into EngineCore, and naming a module's code there would link the module).
    Module::Registry::Get().RegisterRenderFeatures(m_features);
    m_features.Finalize();

    // Unconditional, and before any pass is handed the arena (RTReflectionPass, the first hand-out, exists only on
    // the RT tier): the arena must be able to idle the GPU before it replaces a heap (TransientAliasArena::Ensure),
    // and a secondary view's pass transients take the graph's shared scratch heap.
    m_transientAliasArena.SetIdleCallback([this] { m_gfx.WaitIdleAndReleaseDeferred(); });
    if (!MemoryPolicy::ViewTransientsCommitted()) m_transientAliasArena.SetViewTwin(&m_graph.ViewScratchArena());

    ReSTIRTransient::ReserveIfEager(m_gfx, m_transientAliasArena);
    InitMeshManager();
    CreateConstantBuffers();

    // ---- Graph-scope GBuffer textures (stable handles across frames) ----
    // R11G11B10_FLOAT albedo: HDR emissive is not clipped at 1; shaders use it linear.
    // isUAV on the decal-targeted RTs so DecalPass blends in place; the graph handles the UAV -> SRV
    // transition.
    m_albedoHandle   = m_graph.CreateTexture("GBuffer0_Albedo",
        { RHI::Format::R11G11B10_FLOAT,     0, 0, false, true  });
    // R16G16B16A16_UNORM (not _FLOAT): .a carries matIdx as an exact 16-bit integer. FP16's 11-bit
    // significand represents integers exactly only up to 2048, and material slots go above that (default
    // cap 4096, bounded at 16 bits by Renderer::kMaxMaterialsUpperBound). UNORM's uniform 1/65535 spacing
    // also beats FP16 for the n*0.5+0.5 normal (FP16 precision falls to 2^-11 near 1.0).
    m_normalHandle   = m_graph.CreateTexture("GBuffer1_Normal",
        { RHI::Format::R16G16B16A16_UNORM,  0, 0, false, true  });
    m_surfaceHandle  = m_graph.CreateTexture("GBuffer2_Surface",
        { RHI::Format::R8G8B8A8_UNORM,     0, 0, false, true  });
    // D32_FLOAT_S8X24: float depth so reversed-Z pays off; D24_UNORM's uniform quantization gave ~0.5 wu
    // of error at 900 m (far SSR holes, shadow / contact artifacts). The stencil plane serves Outline / TAA.
    // Every graphics PSO's dsvFormat must match this format.
    m_depthHandle    = m_graph.CreateTexture("GBuffer_Depth",
        { RHI::Format::D32_FLOAT_S8X24_UINT, 0, 0, true, false });
    m_velocityHandle = m_graph.CreateTexture("GBuffer3_Velocity",
        // isSRV=true: TAA / XeGTAO / SSR read velocity as an SRV outside the graph (direct SRV handles);
        // without it velocitySrv is 0, those passes get no motion vectors and the velocity slot is left
        // unbound (GBV error).
        // isUAV=true: VelocityFillPass writes the sky pixels here in place after the graph (no geometry pass
        // covers depth == 0), which the upscalers need (VelocityFillPass.h).
        { RHI::Format::R16G16_FLOAT,        0, 0, false, /*isUAV*/true, /*isSRV*/true });
    m_emissiveHandle = m_graph.CreateTexture("GBuffer4_Emissive",
        { RHI::Format::R16G16B16A16_FLOAT,  0, 0, false, false });
    // ReSTIR DI / GI and path-tracer results: on demand. Render-sized RGBA16F (DI is two slices), ~66 MB at
    // 1080p and ~265 MB at 4K together, and all three producers are off on the default raster path.
    // Declared here so the passes hold stable handles and LightingPass's reads stay in the graph, but
    // backed only once a producer's EnsureResources (the frame-boundary ServiceEnableGatedResources) first
    // wants it with a live pipeline: a raster session, or a GPU without inline ray tracing, never allocates
    // them. Sticky once backed (RenderGraph::EnsurePhysicalTexture says why).
    // tests/scenarios/preview_teardown pins the texture pool on this (472, not 475).
    m_diResultHandle = m_graph.CreateTexture("RestirDIResult",
        { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true, true, 2 },
        RG::TextureResidency::OnDemand);
    // The compare mode's shadow DI result: the same layout, backed only while -restir_compare=di runs,
    // never composited.
    m_diShadowResultHandle = m_graph.CreateTexture("RestirDIShadowResult",
        { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true, true, 2 },
        RG::TextureResidency::OnDemand);
    m_giResultHandle = m_graph.CreateTexture("RestirGIResult",
        { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true, true },
        RG::TextureResidency::OnDemand);
    // The compare mode's shadow GI result: backed only while -restir_compare=gi runs, never composited.
    m_giShadowResultHandle = m_graph.CreateTexture("RestirGIShadowResult",
        { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true, true },
        RG::TextureResidency::OnDemand);
    m_ptResultHandle = m_graph.CreateTexture("PathTraceResult",
        { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true, true },
        RG::TextureResidency::OnDemand);
    m_reflectionSourceHandle = m_graph.CreateTexture("ReflectionSource_NoTrace",
        { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, false });
    // Depth snapshot after terrain / meshes, before GrassPass. The SSR depth hierarchy is built from it so
    // thin grass blades over the water on screen do not act as Hi-Z blockers (the walker would converge on
    // every blade tip and the fall-through thickness check would kill the ray: a bright sky band
    // truncating reflections behind every grass silhouette). The trace's per-pixel gDepth (origins +
    // finish refinement) stays the main depth, which water / grass do write.
    m_ssrTraceDepthHandle = m_graph.CreateTexture("SSRTraceDepth",
        { RHI::Format::D32_FLOAT_S8X24_UINT, 0, 0, true, false, /*isSRV*/true });
    // ---- Translucent front-layer targets (TranslucentFrontLayerPass) ----
    // GBuffer1/2/3-shaped (same formats; GBuffer1's UNORM alpha is an exact matIdx) so RTReflectionPass's
    // second instance reads them through the same root signature and shaders as the opaque GBuffer. The
    // prepass clears all four every frame it runs (depth to sky = 0, so Classify emits rays only where a
    // translucent surface was stamped). isSRV: the forward fold and the resolve() index derivation read
    // them outside the graph's declared reads. On demand: TranslucentFrontLayerPass::EnsureTargets backs them.
    const RG::TextureResidency frontResidency = TranslucentFrontLayerPass::Residency();
    m_translucentNormalHandle   = m_graph.CreateTexture("TranslucentFront_Normal",
        { RHI::Format::R16G16B16A16_UNORM,   0, 0, false, false, /*isSRV*/true }, frontResidency);
    m_translucentSurfaceHandle  = m_graph.CreateTexture("TranslucentFront_Surface",
        { RHI::Format::R8G8B8A8_UNORM,       0, 0, false, false, /*isSRV*/true }, frontResidency);
    m_translucentVelocityHandle = m_graph.CreateTexture("TranslucentFront_Velocity",
        { RHI::Format::R16G16_FLOAT,         0, 0, false, false, /*isSRV*/true }, frontResidency);
    m_translucentDepthHandle    = m_graph.CreateTexture("TranslucentFront_Depth",
        { RHI::Format::D32_FLOAT_S8X24_UINT, 0, 0, true,  false, /*isSRV*/true }, frontResidency);
    // ---- The HDR scene colour: imported, not owned ----
    // The device's slot 0 target (IGraphicsDevice::GetHdrTexture), which the graph never creates or
    // resizes; RenderAsync binds it with the frame's clear value before every Execute. -rgimportoff: no import.
    if (RG::RenderGraph::ImportsEnabled())
        m_hdrHandle = m_graph.ImportTexture(RG::kHdrSceneTexture,
            { m_gfx.GetHdrSceneColorFormat(), 0, 0, false, false });
    // ---- The SSR scene pyramid and result (imported) ----
    // The SSR group writes them after the graph (ssrCL); RTReflection and SSGI (the pyramid) and Water (the
    // result) read them inside it as last frame's: History imports that their owner transitions. Declared
    // here, before the graph's first Compile; the subsystem is made later (InitSkinningSystems), so
    // RenderAsync binds them before every Execute.
    if (RG::RenderGraph::ImportsEnabled())
    {
        const RG::ImportSpec past{ nullptr, RG::ImportEntry::History, RG::ImportTracking::Owner };
        m_ssrPyramidHandle = m_graph.ImportTexture(RG::PassTex::kSSRScenePyramid,
            { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true }, past);
        m_ssrResultHandle  = m_graph.ImportTexture(RG::PassTex::kSSRResult,
            { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true }, past);
    }
    // ---- Read outside the graph's passes ----
    // TAA and the upscaler inputs, the SSR chain, Hi-Z, the debug wires, the velocity fill and the RR guides
    // take these after (or beside) the graph, so they keep memory of their own (RG::RenderGraph::AliasPlan).
    m_graph.DeclareOutsideUse(m_depthHandle,            "TAA, SSR, HiZ, DebugWire, RRGuide");
    m_graph.DeclareOutsideUse(m_normalHandle,           "TAA, SSR");
    m_graph.DeclareOutsideUse(m_surfaceHandle,          "TAA, SSR");
    m_graph.DeclareOutsideUse(m_velocityHandle,         "TAA, SSR, VelocityFill");
    m_graph.DeclareOutsideUse(m_reflectionSourceHandle, "SSR");
    m_graph.DeclareOutsideUse(m_ssrTraceDepthHandle,    "SSR");

    // ---- Register passes: each owns its ShaderLibrary + PSOCache ----
    // ShadowSystem owns the cascade math; ShadowPass runs standalone (own CL + Texture2DArray).
    m_shadowSystem = std::make_unique<ShadowSystem>();
    m_shadowSystem->Init(m_gfx);
    m_shadowPass = std::make_unique<ShadowPass>();
    // Apply the player-configured CSM resolution BEFORE Init() creates the atlas
    // (EngineSettings / settings.json; set on the Renderer before Compile()).
    m_shadowPass->SetShadowResolution(m_shadowResolution);
    // Meshlet path: the CSM's mesh PSOs exist only on a capable device in a run that did not kill the path
    // (the flags are read here, once).
    ReadMeshletFlags();
    m_shadowPass->SetMeshletCapable(m_gfx.GetCapabilities().meshShader && !m_meshletsKilled);
    m_shadowPass->Init(m_gfx);
    m_shadowPass->SetShadowSystem(m_shadowSystem.get());
    m_passes.Register("Shadow", m_shadowPass);
    // ---- The CSM: imported, transitioned by its owner ----
    // ShadowPass records the cascade array on worker 0's shadowCL, outside the graph and beside its serial
    // window: Fresh (written before the graph's readers) and Owner (the graph records who reads it; the
    // barriers and the device's book stay ShadowPass's).
    if (RG::RenderGraph::ImportsEnabled())
    {
        const uint32_t csm = m_shadowPass->GetShadowResolution();
        m_graph.ImportTexture(RG::PassTex::kShadowCSM,
            { RHI::Format::D32_FLOAT, csm, csm, /*isDepth*/true, false, false, ShadowPass::kCascadeCount },
            { m_shadowPass->CascadeArray(), RG::ImportEntry::Fresh, RG::ImportTracking::Owner });
    }

    // The main graph's passes, in registration order. Each carries its RG::Stage (RenderGraph/Stage.h)
    // and, where the order inside its stage matters and the declarations do not fix it, its RG::Order
    // constraints with the reason (RenderGraph/Order.h). The graph's first sort turns this into the
    // declaration order - the execution order - and logs [RG_ORDER] and [RG_STAGES]. source_gates gate 8
    // holds this list's length and the Renderer's pass members: a new pass comes from a render feature,
    // whose graph passes go in after these (m_features.AddPasses at the end) and say where with their
    // constraints.
    m_frameTextures = { m_depthHandle, m_albedoHandle, m_normalHandle, m_surfaceHandle,
                        m_velocityHandle, m_emissiveHandle, m_diResultHandle };
    FeatureCompileContext featureCtx{ m_graph, m_passes, m_gfx, m_frameTextures };
    {
        auto gbufPass = std::make_unique<GBufferPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle, m_emissiveHandle);
        gbufPass->SetTextureAnisotropy(m_textureAnisotropy);
        // Meshlet path: only the main instance is capable, and only on a device with mesh shaders in a run
        // that did not kill the path; otherwise no AS / MS PSO is ever built (the flags were read before
        // ShadowPass::Init).
        gbufPass->SetMeshletCapable(m_gfx.GetCapabilities().meshShader && !m_meshletsKilled);
        m_gbufferPass = gbufPass.get();
        m_passes.Register("GBuffer", m_gbufferPass);
        m_graph.AddPass(RG::Stage::GBuffer, std::move(gbufPass));
    }
    {
        // Immediately after GBufferPass, and that position is load-bearing: the bake consumes the same opaque
        // DrawList and per-material bindings GBufferPass just used, and its atlas must be back in
        // SHADER_RESOURCE before any later RT pass samples it. It declares no graph texture (own scratch MRTs,
        // hand-written atlas barriers), so it adds no barrier work when idle; with the feature off IsActive()
        // is false and the graph skips it entirely (no command list, timestamp or marker).
        auto pass = std::make_unique<SurfaceCacheBakePass>(m_gbufferPass, &m_surfaceCache);
        m_surfaceCacheBakePass = pass.get();
        m_graph.AddPass(RG::Stage::GBuffer, std::move(pass));
        // Its lazily allocated half (six scratch MRTs and a persistently mapped upload CB) is released at
        // shutdown: the main graph is never Destroy()ed, so nothing else calls its RenderPass::Shutdown.
        m_passes.Register("SurfaceCacheBake", m_surfaceCacheBakePass);
    }
    {
        // Inverted-hull outline as real geometry, into the same GBuffer, before lighting: the rim writes depth
        // + velocity and TAA needs no outline-specific handling. Mutually exclusive with OutlinePass's
        // colour-only hull sub-pass (the outline toggle sync keeps one); both would draw the same rim.
        auto pass = std::make_unique<OutlineGeometryPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle,
            m_velocityHandle, m_emissiveHandle);
        m_outlineGeometryPass = pass.get();
        m_graph.AddPass(RG::Stage::GBuffer, std::move(pass));
        m_passes.RegisterView("OutlineGeometry", m_outlineGeometryPass);   // a secondary view's camera
    }
    {
        // TerrainPass writes into the same GBuffer right after GBufferPass and depth-tests against scene
        // geometry. Self-disables without mesh-shader support or a TerrainComponent.
        auto pass = std::make_unique<TerrainPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle, m_emissiveHandle);
        pass->SetTextureAnisotropy(m_textureAnisotropy);
        m_terrainPass = pass.get();
        m_graph.AddPass(RG::Stage::GBuffer, std::move(pass));

        // ShadowPass renders depth-only terrain into each CSM cascade; as a standalone pass it is handed the
        // TerrainPass directly (not via graph binding). The TerrainCB is rebound every frame in
        // Render_BindFrameResources: it is triple-buffered (one slot per in-flight frame).
        if (m_shadowPass)
            m_shadowPass->SetTerrainPass(m_terrainPass);
    }
    {
        // SSRTraceDepth snapshot: copy the depth after opaque meshes + terrain, before GrassPass stamps its
        // blades. The SSR subsystem builds its Hi-Z march pyramid from this copy, so rays from water pixels
        // pass through the blade band at the grass silhouette instead of being blocked (grass is absent from
        // reflections).
        // The pass writes the snapshot (the copy), so it is declared CopyDst (with -rgplaneoff a ManualBarrier),
        // never a ReadSRV: declared as a read, the DAG would see no producer and call every later reader of
        // SSRTraceDepth (SSGI, the RT reflections, the forward translucents) a stale read. Without planes both
        // sit in DEPTH_READ_SRV at pass entry and the manual barriers below net out to identity, keeping the
        // graph's state tracking coherent.
        m_graph.AddPass(RG::Stage::GBuffer, "SSRTraceDepthSnapshot",
            [depth = m_depthHandle, snap = m_ssrTraceDepthHandle](RG::RenderGraphBuilder& b)
            {
                // With planes enabled the graph moves each texture's depth plane to COPY_SOURCE / COPY_DEST.
                // -rgplaneoff: a declared read plus a manual barrier, and the copy barriers itself.
                if (RG::RenderGraph::PlanesEnabled())
                {
                    b.CopySrc(depth, RG::Plane::Depth);
                    b.CopyDst(snap, RG::Plane::Depth);
                }
                else
                {
                    b.ReadSRV(depth);
                    b.ManualBarrier(snap, RHI::ResourceState::DEPTH_READ_SRV);
                }
                b.SetColorTarget(RG::BuiltinTexture::None);
            },
            [this](RHI::CommandList& cl)
            {
                const RHI::Texture* src = m_graph.GetPhysicalTexture(m_depthHandle);
                const RHI::Texture* dst = m_graph.GetPhysicalTexture(m_ssrTraceDepthHandle);
                if (!src || !dst) return;
                const bool planes = RG::RenderGraph::PlanesEnabled();   // planes: the graph moved them
                if (!planes)
                {
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(src,
                        RHI::ResourceState::DEPTH_READ_SRV, RHI::ResourceState::COPY_SRC), cl);
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(dst,
                        RHI::ResourceState::DEPTH_READ_SRV, RHI::ResourceState::COPY_DST), cl);
                }
                // Depth plane only (mip 0 / slice 0 / plane 0): the stencil plane is never read through this copy.
                m_gfx.CopyTextureSubresource(*src, 0, 0, *dst, 0, 0, cl);
                if (!planes)
                {
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(src,
                        RHI::ResourceState::COPY_SRC, RHI::ResourceState::DEPTH_READ_SRV), cl);
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(dst,
                        RHI::ResourceState::COPY_DST, RHI::ResourceState::DEPTH_READ_SRV), cl);
                }
            });
        // Every view's own copy of its depth: the soft fades and the forward passes read it (FogApplyCB).
        m_graph.MarkViewSafe("SSRTraceDepthSnapshot");
    }
    {
        // GrassPass: GoT-style procedural grass blades. Writes the same GBuffer
        // right after TerrainPass so blades depth-test against the terrain
        // surface they grow from. Self-disables w/o mesh-shader support or
        // when no GrassComponent is armed (dispatchAsGroupCount == 0).
        auto pass = std::make_unique<GrassPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle, m_emissiveHandle);
        m_grassPass = pass.get();
        m_passes.RegisterView("Grass", m_grassPass);   // a secondary view's camera
        m_graph.AddPass(RG::Stage::GBuffer, std::move(pass));
    }
    {
        // Bangs -> face screen-space shadow mark (a DanbaidongRP CharacterFringe port). Runs while RT4 / depth
        // are still RTV / DSV (right after the GBuffer producers); stamps extra.g on NPR pixels the offset
        // fringe proxy covers. Lighting.ps's toon face branch consumes it.
        auto pass = std::make_unique<HairShadowMarkPass>(m_emissiveHandle, m_depthHandle);
        m_hairMarkPass = pass.get();
        m_passes.RegisterView("HairShadowMark", m_hairMarkPass);   // a secondary view's camera
        m_graph.AddPass(RG::Stage::GBuffer, std::move(pass));
    }
    {
        // Compute-only env-cube -> SH projection. Must precede LightingPass (its PS samples the SH buffer).
        auto pass = std::make_unique<SkyIBLPass>();
        m_skyIBLPass = pass.get();
        m_passes.RegisterView("SkyIBL", m_skyIBLPass);   // a secondary view's aerial-perspective LUT
        m_graph.AddPass(RG::Stage::ShadowAndEnvironment, std::move(pass));
    }
    {
        // Spot shadow atlas. Must precede LightingPass + VolumetricFogPass (both sample its depth).
        auto pass = std::make_unique<SpotShadowPass>();
        m_spotShadowPass = pass.get();
        m_graph.AddPass(RG::Stage::ShadowAndEnvironment, std::move(pass));
        // The spot (256 MiB base atlas plus its quality tiers) and point (96 MiB) atlases go with the world
        // that needed them. Both flags gate the release, not just worldSurvives: PlayModeSession::ExitPlay calls
        // OnWorldClear(idsPreserved=true) and then restores the same world, so releasing there would drop and
        // re-allocate both atlases on every Play / Stop. The demand flags need no clearing with them:
        // ExtractShadowAtlasDemand rewrites them from the live world before every ServiceEnableGatedResources,
        // so an old world's demand never outlives its clear.
        m_passes.Register("SpotShadow", m_spotShadowPass);
    }
    {
        // Point-light omnidirectional cube shadow atlas. Must precede LightingPass.
        auto pass = std::make_unique<PointShadowPass>();
        // shadows.pointResolution: the face size, before the graph declares the atlas and Init sizes it.
        pass->SetFaceSize(m_pointShadowResolution);
        m_pointShadowPass = pass.get();
        m_graph.AddPass(RG::Stage::ShadowAndEnvironment, std::move(pass));
        m_passes.Register("PointShadow", m_pointShadowPass);   // the release rule: SpotShadow's, above
    }
    {
        // Per-object (player) high-res directional shadow map. Renders only the
        // player's tagged packets; composited on top of CSM in Lighting.ps.
        // Must precede LightingPass (samples the produced map at t42).
        auto pass = std::make_unique<PerObjectShadowPass>();
        m_perObjShadowPass = pass.get();
        m_graph.AddPass(RG::Stage::ShadowAndEnvironment, std::move(pass));
    }
    {
        // Hair (bangs -> face) shadow map, the Hair variant of the same pass. Renders only hairShadowCaster
        // packets (HairShadowCasterComponent subtree); sampled only by NPR face pixels in Lighting.ps (t43).
        auto pass = std::make_unique<PerObjectShadowPass>(PerObjectShadowPass::Variant::Hair);
        m_hairShadowPass = pass.get();
        m_graph.AddPass(RG::Stage::ShadowAndEnvironment, std::move(pass));
    }
    {
        // DecalPass: GBuffer -> Lighting; UAV-blends into albedo / normal / surface, depth as SRV.
        auto pass = std::make_unique<DecalPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle);
        m_decalPass = pass.get();
        m_passes.RegisterView("Decal", m_decalPass);   // a secondary view's grid and camera
        m_graph.AddPass(RG::Stage::GBufferModify, std::move(pass));
    }
    {
        // SSGIPass - screen-space diffuse GI, the near-field half of the hybrid diffuse GI (DDGI supplies the
        // far field).
        // Registered right after DecalPass, ahead of the whole RT block. The one hard constraint: decals
        // UAV-blend the GBuffer normals its hemispheres and ray origins come from. Everything else is owned or
        // last frame's: it builds its own Hi-Z (RTReflectionPass, later, may borrow it and skip its own
        // build), needs no TLAS / DXR, and the scene-colour pyramid is the previous frame's.
        // Here it stays out from behind the promoteCL <- ddgiCL cross-queue fence: every pass between
        // RTReflectionPass and LightingPass inherits that wait, because the graph chains pass command lists
        // serially, even when it needs nothing DDGI produces.
        // Only debugMode 1 (calibration) reads DDGI here; ahead of the fence that read can land mid-update, so
        // calibration numbers may be a frame stale. Accepted: a bring-up mode, versus paying the fence every
        // shipping frame (farIrr is zeroed when debugMode != 1).
        auto pass = std::make_unique<SSGIPass>(
            m_normalHandle, m_depthHandle, m_velocityHandle,
            m_ssrTraceDepthHandle);
        m_ssgiPass = pass.get();
        static constexpr const char* kAheadOfTheWait =
            "stays ahead of the passes that wait on the compute queue's DDGI / TLAS promote: the graph chains the "
            "command lists serially, so a pass behind a waiter inherits its wait (performance, not correctness)";
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pass),
                        { RG::Before("RTShadowPass", kAheadOfTheWait), RG::Before("RTAOPass", kAheadOfTheWait),
                          RG::Before("RTReflectionPass", kAheadOfTheWait) });
        m_passes.RegisterView("SSGI", m_ssgiPass);   // its history lives in the view
    }
    // ---- RT pass order ----
    // RTShadow and RTAO register before RTReflectionPass. The three are siblings (each reads the post-Decal
    // GBuffer and writes its own target), so their relative order is free. RTR is the only one that reads
    // DDGI probe data, so it is the first pass that waits on the DDGI compute chain, and since the graph
    // chains pass command lists serially everything behind it inherits that stall. The order is kept
    // because it is free; on its own it bought no measurable overlap (see the SetExternalWait calls in
    // RenderAsync).
    // Do not move them to just after GBufferPass: all three must run after Decal, because decals rewrite
    // the GBuffer normals that AO hemispheres and shadow-ray origin offsets are built from - a visual
    // change, not a scheduling one.
    {
        // RTShadowPass - ray-traced sun-shadow mask over the same scene TLAS (shadow rays trace instance-mask
        // bit 1). After Decal (final GBuffer), before Lighting (which min()s the mask into the CSM result at
        // t45). Its cross-queue wait is registered per frame in RenderAsync.
        auto pass = std::make_unique<RTShadowPass>(
            m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle);
        m_rtShadowPass = pass.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pass));
        m_passes.RegisterView("RTShadow", m_rtShadowPass);   // its history lives in the view
    }
    {
        // RTAOPass - ray-traced AO over the same scene TLAS (mask 0x01), denoised by NRD
        // REBLUR_DIFFUSE_OCCLUSION. After Decal (final GBuffer), before Lighting (which samples the resolved AO
        // at gSSAO t18 when Renderer routes SetSSAOHandle here). Replaces XeGTAO's screen-space AO when active.
        auto pass = std::make_unique<RTAOPass>(
            m_normalHandle, m_depthHandle, m_velocityHandle);
        m_rtaoPass = pass.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pass));
        m_passes.RegisterView("RTAO", m_rtaoPass);   // its history lives in the view
    }
    {
        // RTReflectionPass - unified hybrid reflections: a screen-space Hi-Z tier (radiance = previous-frame
        // scene pyramid) + an RT-ray tier over the scene TLAS built by DDGISceneAS (inline RayQuery; the AS
        // build runs even in DDGI-less scenes when RTR is on). After Decal (final GBuffer), before Lighting
        // (which samples its denoised output at t44, the single reflection fold). SSRTraceDepth feeds its Hi-Z
        // build (a grass-free march). Waits on the DDGI promote fence via SetExternalWait, registered per frame
        // in RenderAsync only when the RT tier is active.
        auto pass = std::make_unique<RTReflectionPass>(
            m_normalHandle, m_surfaceHandle, m_depthHandle, m_velocityHandle,
            m_ssrTraceDepthHandle);
        pass->SetAliasArena(&m_transientAliasArena);
        // Last frame's ReSTIR GI for the environment stand-in.
        pass->SetRestirGIResult(m_giResultHandle);
        m_rtReflectionPass = pass.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pass));
        m_passes.RegisterView("RTReflection", m_rtReflectionPass);   // its history lives in the view
    }
    {
        // Translucent front-layer reflections (the UE-Lumen "front layer translucency" shape):
        // TranslucentFrontLayerPass re-draws the forward translucent packets colour-less and depth-writing into
        // private GBuffer-shaped targets (nearest translucent surface wins; opaque occlusion is a PS test
        // against the SSRTraceDepth snapshot), then a second RTReflectionPass instance traces from that stamp:
        // RT-only, denoiser bypassed, ray mask 0x08 so a pane cannot hit itself. The forward shader folds the
        // result at full strength (LightCB fwdReflFrontRtrIdx / fwdReflFrontDepthIdx). Registered right behind
        // the opaque instance: adjacent in the TransientAliasArena timeline (group E aliases group A's bytes)
        // and inside the DDGI promote wait the opaque instance already imposes on everything after it.
        auto pre = std::make_unique<TranslucentFrontLayerPass>(
            m_graph, m_translucentNormalHandle, m_translucentSurfaceHandle,
            m_translucentVelocityHandle, m_translucentDepthHandle,
            m_ssrTraceDepthHandle);
        pre->SetTextureAnisotropy(m_textureAnisotropy);
        m_frontLayerPass = pre.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pre),
                        { RG::After("RTReflectionPass",
                                    "adjacent to the opaque instance in the transient alias arena's timeline (group E "
                                    "aliases group A's bytes; RG.M6 declares the arena) and inside the DDGI promote "
                                    "wait it already imposes") });

        // traceDepth is declared-read only (no Hi-Z is built for this variant).
        auto pass = std::make_unique<RTReflectionPass>(
            m_translucentNormalHandle, m_translucentSurfaceHandle,
            m_translucentDepthHandle, m_translucentVelocityHandle,
            m_ssrTraceDepthHandle, RTReflectionPass::Variant::TranslucentFront);
        pass->SetAliasArena(&m_transientAliasArena);
        m_rtReflectionFrontPass = pass.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pass));
        m_passes.RegisterView("RTReflectionFront", m_rtReflectionFrontPass);   // its history lives in the view
    }
    {
        // XeGTAOPass - screen-space AO, the default source when RTAO is off. Registered in RTAO's slot (after
        // Decal -> post-decal GBuffer normals, before Lighting) rather than in the post-process compute chain:
        // after the graph it could only hand LightingPass the previous frame's AO, which slides on moving
        // silhouettes and camera pans. Mutually exclusive with RTAOPass: RenderAsync picks one and deactivates
        // the other, so only one AO chain dispatches per frame.
        auto pass = std::make_unique<XeGTAOPass>(
            m_normalHandle, m_depthHandle, m_velocityHandle);
        pass->SetAliasArena(&m_transientAliasArena);
        m_xegtaoPass = pass.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pass));
        m_passes.RegisterView("XeGTAO", m_xegtaoPass);   // its history lives in the view
    }
    {
        // Light sampling tables: ordered before the two DI passes that sample them this frame
        // (SetLightTables in RenderAsync) - a bindless read the graph does not see, so an order constraint
        // keeps it.
        auto tables = std::make_unique<LightSamplingPass>(m_graph);
        m_lightSamplingPass = tables.get();
        m_passes.Register("LightSampling", m_lightSamplingPass);
        static constexpr const char* kTablesFirst =
            "builds the light PDF texture and RIS buffer the RTXDI DI core samples this frame - a bindless read, "
            "which the graph does not see (RG.M7 declares buffers)";
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(tables),
                        { RG::Before("RTDI", kTablesFirst), RG::Before("RTDI.Shadow", kTablesFirst) });
        auto di = std::make_unique<RTDIPass>(m_graph, m_albedoHandle, m_normalHandle,
            m_surfaceHandle, m_emissiveHandle, m_depthHandle, m_velocityHandle, m_diResultHandle);
        di->SetAliasArena(&m_transientAliasArena);
        m_rtDiPass = di.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(di));
        // Right after the real one: the same frame's GBuffer, TLAS and lights; its own Classify, NRD and
        // counters.
        auto diShadow = std::make_unique<RTDIPass>(m_graph, m_albedoHandle, m_normalHandle,
            m_surfaceHandle, m_emissiveHandle, m_depthHandle, m_velocityHandle, m_diShadowResultHandle, true);
        diShadow->SetAliasArena(&m_transientAliasArena);
        m_rtDiShadowPass = diShadow.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(diShadow));
        auto gi = std::make_unique<RTGIPass>(m_graph, m_albedoHandle, m_normalHandle,
            m_surfaceHandle, m_depthHandle, m_velocityHandle, m_giResultHandle);
        gi->SetAliasArena(&m_transientAliasArena);
        m_rtGiPass = gi.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(gi));
        // Right after the real one, as RTDI.Shadow: the same frame's GBuffer, TLAS, lights and emitter table;
        // its own Trace, NRD and counters.
        auto giShadow = std::make_unique<RTGIPass>(m_graph, m_albedoHandle, m_normalHandle,
            m_surfaceHandle, m_depthHandle, m_velocityHandle, m_giShadowResultHandle, true);
        giShadow->SetAliasArena(&m_transientAliasArena);
        giShadow->SetSharcSource(m_rtGiPass);   // it queries the main pass's SHaRC cache
        m_rtGiShadowPass = giShadow.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(giShadow),
                        { RG::After("RTGI", "queries the main pass's SHaRC cache this frame (SetSharcSource) - a "
                                            "buffer, which the graph does not see (RG.M7)") });
        auto pt = std::make_unique<PathTracePass>(m_graph, m_albedoHandle, m_normalHandle,
            m_surfaceHandle, m_depthHandle, m_ptResultHandle);
        // The rtxdi mode: temporal reuse reads the motion vectors, and the primary's local lights come from
        // the DI pass declared above.
        pt->SetVelocity(m_velocityHandle);
        pt->SetRestirDIResult(m_diResultHandle);
        // Glass traces its own reflection on this path, from the front-layer
        // prepass's stamp (declared above with the RT reflection front instance).
        if (m_frontLayerPass)
            pt->SetFrontLayer(m_translucentDepthHandle, m_translucentNormalHandle, m_translucentSurfaceHandle,
                              m_translucentVelocityHandle);
        m_pathTracePass = pt.get();
        m_graph.AddPass(RG::Stage::LightingInputs, std::move(pt));
        // ReSTIR and path-trace history names surfaces, lights and instances of one view of one world: a cut,
        // a resize and a world boundary all drop it. OnWorldClear runs mid-frame, so only flags are set; each
        // pass drops its reservoirs and denoiser history at its next Execute. Shutdown runs after the GPU
        // drain: the raw RT passes have no NRD teardown to drain them. The cut, the resize and the world
        // boundary reach their histories through the view. The SHaRC radiance caches (GI's and the path
        // tracer's) start empty in a new world too (loads and the editor's Stop).
        m_passes.Register("RTDI", m_rtDiPass);
        m_passes.Register("RTDI.Shadow", m_rtDiShadowPass);
        m_passes.Register("RTGI", m_rtGiPass);
        m_passes.Register("RTGI.Shadow", m_rtGiShadowPass);
        m_passes.Register("PathTrace", m_pathTracePass);
        auto pass = std::make_unique<LightingPass>(
            m_albedoHandle, m_normalHandle, m_surfaceHandle, m_depthHandle,
            m_emissiveHandle, m_reflectionSourceHandle);
        m_lightingPass = pass.get();
        m_passes.Register("Lighting", m_lightingPass);
        pass->SetPathTraceResult(m_ptResultHandle);
        pass->SetRestirDIResult(m_diResultHandle);
        pass->SetRestirGIResult(m_giResultHandle);
        // No reflection-probe handles go into this pass: the shader samples each probe's cube bindless via
        // cubemapSrvIndex, and the probe StructuredBuffer's per-frame ring SRV goes into LightCB.reflProbeBufIdx
        // in RenderAsync's resolve() block. m_probeMgr owns the cube array and derives every cubemapSrvIndex
        // from it.
        // The cluster probe grid / index handles are wired after ClusterPass::Init (end of Compile).
        m_graph.AddPass(RG::Stage::Lighting, std::move(pass));
    }
    {
        auto pass = std::make_unique<SkyboxPass>(m_depthHandle);
        m_skyboxPass = pass.get();
        m_graph.AddPass(RG::Stage::SkyAndWater, std::move(pass));
        // The star cubemap is a per-world lazy resident: released with the
        // world that needed it, not on a feature-idle timer.
        m_passes.Register("Skybox", m_skyboxPass);
    }
    {
        // WaterPass: forward Fresnel + flow-normal water surface. After Skybox
        // (sky behind the water exists in HDR) and before clouds/fog so both
        // composite above the surface; depth WRITE so they treat it as real
        // geometry. Also stamps GBuffer normal+surface so the post-frame SSR
        // chain traces wavy reflections off the water (composite adds them
        // onto HDR the same frame). Self-skips without a WaterComponent or
        // sky cube.
        auto pass = std::make_unique<WaterPass>(
            m_normalHandle, m_surfaceHandle, m_velocityHandle, m_depthHandle);
        pass->SetTextureAnisotropy(m_textureAnisotropy);
        m_waterPass = pass.get();
        m_graph.AddPass(RG::Stage::SkyAndWater, std::move(pass));
    }
    {
        // WaterSurfacePass: the mesh-driven water path - "a plane + a Water material is water". Consumes
        // DrawFilter::Water (ShadingModel::Water) and shades it through the material's custom shader hook.
        // Registered right after WaterPass and before WaterGBufferRestore, so both water paths share one frame
        // slot and one GBuffer restore.
        // TODO(water_material_domain_plan M7): delete WaterPass once nothing authors a WaterComponent.
        auto pass = std::make_unique<WaterSurfacePass>(
            m_normalHandle, m_surfaceHandle, m_velocityHandle, m_depthHandle,
            m_ssrTraceDepthHandle, m_reflectionSourceHandle);
        pass->SetTextureAnisotropy(m_textureAnisotropy);
        m_waterSurfacePass = pass.get();
        m_graph.AddPass(RG::Stage::SkyAndWater, std::move(pass));
    }
    {
        // Declaration-only no-op pass: WaterPass leaves GBuffer normal + surface in RENDER_TARGET (it stamps
        // them for SSR), but the frame-end invariant - relied on by the out-of-graph consumers (SSR subsystem,
        // XeGTAO, post passes) and by next frame's barrier bookkeeping - is SHADER_RESOURCE. Declaring reads
        // here makes the graph emit the RT -> SR transitions right after the water draw, with its state
        // tracking in sync. The empty Execute is intentional.
        m_graph.AddPass(RG::Stage::SkyAndWater, "WaterGBufferRestore",
            [normal = m_normalHandle, surface = m_surfaceHandle](RG::RenderGraphBuilder& b)
            {
                b.ReadSRV(normal);
                b.ReadSRV(surface);
                b.SetColorTarget(RG::BuiltinTexture::None);
            },
            [](RHI::CommandList&) {});
        // Every view's water leaves its own normal + surface in RENDER_TARGET.
        m_graph.MarkViewSafe("WaterGBufferRestore");
    }
    {
        // Editor-only infinite XZ reference grid: analytic fullscreen composite, manually depth-tested (never
        // writes depth). After water (the water surface occludes it via depth) and before clouds / fog so both
        // composite over it; transparents draw on top later. Disabled unless EditorLayer enables it each
        // frame: inert in Game builds.
        auto pass = std::make_unique<EditorGridPass>(m_depthHandle);
        m_editorGridPass = pass.get();
        m_graph.AddPass(RG::Stage::Atmosphere, std::move(pass));
    }
    {
        // Volumetric clouds: composite OVER skybox, BEFORE volumetric fog
        // (so fog can attenuate cloud god-rays / aerial perspective later).
        auto pass = std::make_unique<CloudPass>(m_depthHandle);
        m_cloudPass = pass.get();
        m_graph.AddPass(RG::Stage::Atmosphere, std::move(pass));
        m_passes.RegisterView("Cloud", m_cloudPass);   // its history lives in the view
    }
    // The height fog (Renderer/Features/HeightFogFeature.cpp) composites here:
    // RG::After("CloudPass"); the Video module's screen overlay right after it (Before("SceneVoxelPass")).
    {
        // Scene occupancy voxelization. Precedes VolumetricFogPass (LightInject samples the grid).
        // Inert with zero AABBs: the shader clears and fog falls back to screen-space shadow.
        auto svp = std::make_unique<SceneVoxelPass>();
        m_sceneVoxelPass = svp.get();
        m_passes.RegisterView("SceneVoxel", m_sceneVoxelPass);   // a secondary view's grid
        m_graph.AddPass(RG::Stage::Atmosphere, std::move(svp));
    }
    {
        // Volumetric fog: scattering between Lighting/Skybox and PP chain. Reads depth, blends HDR.
        auto pass = std::make_unique<VolumetricFogPass>(m_depthHandle);
        pass->SetAliasArena(&m_transientAliasArena);
        // The player's allowance and the scene's .ippc switch as they stand (settings normally apply after
        // Compile; this keeps either order correct).
        pass->SetAllowed(m_volFogAuthored);
        pass->SetEnabled(m_fogSceneConfig);
        m_volFogPass = pass.get();
        m_graph.AddPass(RG::Stage::Atmosphere, std::move(pass));
        m_passes.RegisterView("VolumetricFog", m_volFogPass);   // its history lives in the view
    }
    {
        // Brows/lashes-through-hair overlay (DanbaidongRP CharacterEyelashOutter
        // port). Alpha-blends the tagged lash packets over the lit HDR scene
        // where hair covers them; must run AFTER lighting (needs the lit scene
        // + GBuffer extra/depth as SRVs) and BEFORE TransparentPass (real
        // transparents still draw over the overlay).
        auto pass = std::make_unique<FaceOverlayPass>(m_emissiveHandle, m_depthHandle);
        m_faceOverlayPass = pass.get();
        m_graph.AddPass(RG::Stage::Atmosphere, std::move(pass));
    }
    {
        // NPR lit-scene snapshot: copy the HDR scene after opaque lighting + sky / clouds / fog / video + the
        // face overlay, before transparents / VFX composite over it. NPRStabilizePass applies the NPR exposure /
        // ceiling / shoulder work to this image and passes the (full - lit) overlay delta through with a
        // coverage-scaled share of the correction, so a bright beam or particle crossing an NPR character does
        // not inherit the character's brightness ceiling (a character-shaped hole in the effect).
        // After FaceOverlayPass on purpose: brows / lashes are character-authored pixels and must take the same
        // multiplicative treatment as the face under them. A snapshot before the overlay would add the face's
        // correction (exposure protection / EV from the skin) onto the darker brow colour: brows suddenly very
        // bright, vanishing at overlay alpha 0 (full == lit there).
        m_graph.AddPass(RG::Stage::Atmosphere, "NPRLitSnapshot",
            [](RG::RenderGraphBuilder& b)
            {
                // A scoped read of the HDR scene colour: CopyHdrSceneToScoped needs it in RENDER_TARGET on entry and
                // puts it back there; the pass draws nothing into it.
                // Without the import (-rgimportoff): Preserve, not HdrSceneColor - the plain target is auto-bind +
                // CLEAR (first-HDR-writer semantics) and would wipe the scene to black right before the snapshot.
                // Preserve keeps the serial RENDER_TARGET transition (CopyHdrSceneToScoped's precondition) and skips
                // only the clear.
                if (const RG::RGTextureHandle hdr = b.HdrScene(); hdr.IsValid())
                    b.ReadScoped(hdr, RHI::ResourceState::RENDERTARGET);
                else
                    b.SetColorTarget(RG::BuiltinTexture::HdrSceneColorPreserve);
            },
            [this](RHI::CommandList& cl)
            {
                if (m_nprStabilizePass)
                    m_nprStabilizePass->CopyLitSnapshot(m_gfx, cl);
            },
            // Activity predicate: with NPR stabilisation off (every non-NPR scene) the body is a no-op, and
            // skipping the pass saves a command list, a render-target bind, a timestamp and a marker. The declared
            // HdrSceneColorPreserve target goes with it, which is safe: it skips only the serial RENDER_TARGET
            // transition that exists as the CopyHdrSceneToScoped precondition, and the next active pass
            // transitions from whatever the tracker holds.
            [this]
            {
                return m_nprStabilizePass && m_nprStabilizePass->WantsLitSnapshot();
            });
        m_graph.MarkViewSafe("NPRLitSnapshot");   // into the bound view's snapshot (NPRStabilizePass::BindView)
        // DLSS-RR's colour before transparency (after the fog and the face overlay; TransparentPass, the video
        // quads and the particles follow). NPRLitSnapshot's rule: a scoped read of the HDR import at
        // RENDER_TARGET, or without one HdrSceneColorPreserve's serial transition that CopyHdrSceneToScoped
        // needs, with no clear. Skipped unless RR runs.
        if (!m_rrColorSnapshot) m_rrColorSnapshot = std::make_unique<RRColorSnapshot>();
        m_passes.Register("RRColorSnapshot", m_rrColorSnapshot);
        m_graph.AddPass(RG::Stage::Atmosphere, "RRColorBeforeTransparency",
            [](RG::RenderGraphBuilder& b)
            {
                if (const RG::RGTextureHandle hdr = b.HdrScene(); hdr.IsValid())
                    b.ReadScoped(hdr, RHI::ResourceState::RENDERTARGET);
                else
                    b.SetColorTarget(RG::BuiltinTexture::HdrSceneColorPreserve);
            },
            [this](RHI::CommandList& cl) { m_rrColorSnapshot->Copy(m_gfx, cl); },
            [this] { return m_rrColorSnapshot && m_rrColorSnapshot->Wanted(); });
    }
    {
        // GBuffer1 + the SSRTraceDepth snapshot feed the forward shader's
        // traced-reflection fold (declared reads; opened bindless from LightCB).
        auto pass = std::make_unique<TransparentPass>(m_depthHandle, m_normalHandle,
                                                      m_ssrTraceDepthHandle,
                                                      m_translucentDepthHandle);
        pass->SetTextureAnisotropy(m_textureAnisotropy);
        m_transparentPass = pass.get();
        m_passes.Register("Transparent", m_transparentPass);
        // Forward transparent iterates all probes per pixel: no cluster grid binding and no cube-array
        // binding (the shader opens each probe's cube from cubemapSrvIndex). The probe StructuredBuffer
        // arrives through LightCB.reflProbeBufIdx.
        m_graph.AddPass(RG::Stage::Transparent, std::move(pass));
    }
    // The Video module's world-space quads draw here: RG::After("TransparentPass").
    {
        // Particles after Transparent: depth-test ON, depth-write OFF (additive pre-alpha, no TAA block).
        auto pass = std::make_unique<ParticleRenderPass>(m_depthHandle);
        // The soft fade's opaque depth (read via FogApplyCB).
        pass->SetOpaqueDepthSnapshot(m_ssrTraceDepthHandle);
        m_particleRenderPass = pass.get();
        m_graph.AddPass(RG::Stage::Transparent, std::move(pass));
    }
    {
        // Trail ribbons after particles: the same depth-test-no-write + alpha blend.
        auto pass = std::make_unique<TrailRenderPass>(m_depthHandle);
        pass->SetOpaqueDepthSnapshot(m_ssrTraceDepthHandle);   // the soft fade's opaque depth
        m_trailRenderPass = pass.get();
        m_graph.AddPass(RG::Stage::Transparent, std::move(pass));
        m_passes.RegisterView("TrailRender", m_trailRenderPass);   // a secondary view's scene snapshot
    }
    {
        // Tracer beams (cylindrical billboard, additive); reads depth SRV for soft-particle fade.
        auto pass = std::make_unique<TracerRenderPass>(m_depthHandle);
        m_tracerRenderPass = pass.get();
        m_graph.AddPass(RG::Stage::Transparent, std::move(pass));
    }
    {
        // Animated sprite-sheet billboards (explosions, impacts, glows). Same
        // HDR + read-only-depth phase as transparent/particle/trail/tracer:
        // additive/alpha blend, emissive feeds Bloom. CPU-expanded camera-facing
        // quads with per-effect flipbook UVs (see BillboardFXPass / BillboardFXComponent).
        auto pass = std::make_unique<BillboardFXPass>(m_depthHandle);
        pass->SetOpaqueDepthSnapshot(m_ssrTraceDepthHandle);   // the soft fade's opaque depth
        m_billboardFXPass = pass.get();
        m_graph.AddPass(RG::Stage::Transparent, std::move(pass));
        m_passes.Register("BillboardFX", m_billboardFXPass);   // its sprite-atlas texture refs
    }
    {
        auto pass = std::make_unique<OutlinePass>(m_depthHandle, m_normalHandle);
        m_outlinePass = pass.get();
        m_graph.AddPass(RG::Stage::PostOpaque, std::move(pass));
        m_passes.RegisterView("Outline", m_outlinePass);   // a secondary view's camera and object-ID target
    }
    {
        auto pass = std::make_unique<PickingPass>();
        m_pickingPass = pass.get();
        m_graph.AddPass(RG::Stage::Editor, std::move(pass));
        m_passes.RegisterView("Picking", m_pickingPass);   // a secondary view's id and depth targets
    }
    // The render features' graph passes, stage by stage, after the Renderer's own: each says where in its
    // stage it goes (RenderGraph/Order.h) and the first sort puts it there; one without a constraint goes
    // at its stage's end.
    for (RG::Stage stage : { RG::Stage::GBuffer, RG::Stage::ShadowAndEnvironment, RG::Stage::GBufferModify,
                             RG::Stage::LightingInputs, RG::Stage::Lighting, RG::Stage::SkyAndWater,
                             RG::Stage::Atmosphere, RG::Stage::Transparent, RG::Stage::PostOpaque,
                             RG::Stage::Editor })
        m_features.AddPasses(stage, featureCtx);

    Sys::Boot::Mark("graphSetup");     // graph textures declared, passes constructed, shadow atlas
    // The main graph answers snapshot requests (-rgdump, the pipe's `rg` verb). Building one happens only
    // when asked.
    m_graph.SetSnapshotBoard(&RG::MainGraphSnapshots());
    m_graph.Compile(m_gfx);
    Sys::Boot::Mark("graphCompile");   // the graph's passes Init; the rest of Compile follows

    // What the rest of Compile - the passes OUTSIDE the graph - costs, step by
    // step: the [POSTSETUP] line below (PostSetupClock).
    PostSetupClock postSetup;
    g_postSetup = &postSetup;

    // Atmosphere on/off is driven per frame by SyncSkyboxIBL from the scene's WorldEnvironmentComponent
    // mode (ProceduralAtmosphere -> on). No forced default here: a None / FlatColor / StaticSkybox scene
    // must not resurrect the procedural sky at boot.

    // Shared 1x1 black TextureCube fallback (Renderer.h m_fallbackSkyCube): a valid cube descriptor for
    // DDGI-miss / water reflection when a scene has no sky, so those consumers sample black instead of a
    // stale or unbound slot.
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.width      = 1;
        td.height     = 1;
        td.arraySize = 6;
        td.mipLevels = 1;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
        const uint64_t blackFace = 0;               // 4 x half 0.0 -> black, a = 0
        RHI::SubresourceData faces[6];
        for (int f = 0; f < 6; ++f) faces[f] = RHI::SubresourceData{ &blackFace, 8, 8 };
        if (m_gfx.CreateTexture(td, m_fallbackSkyCube, faces))
            m_fallbackSkyCubeSrv = m_gfx.GetTextureSRVGpuHandle(m_fallbackSkyCube);
        else
            LOG_ERROR("Renderer: fallback sky cube creation failed");
    }

    PostDone("fallbackSkyCube");

    // ---- Post-processing compute passes (outside the graph, sequential) ----
    // Each registers in m_lifecycle (RenderLifecycle.h): nothing walks them the way m_graph walks its
    // passes.
    m_taaPass = std::make_unique<TAAPass>();
    m_taaPass->Init(m_gfx);
    m_passes.Register("TAA", m_taaPass);
    PostDone("TAAPass");

    m_nprStabilizePass = std::make_unique<NPRStabilizePass>();
    m_nprStabilizePass->SetAliasArena(&m_transientAliasArena);
    m_nprStabilizePass->Init(m_gfx);
    m_passes.Register("NPRStabilize", m_nprStabilizePass);
    PostDone("NPRStabilizePass");

    // FSR2 upscaler: replaces TAA when UpscaleMode != Native. The FFX context and output texture are created
    // lazily on the first non-Native frame.
    m_fsr2Pass = std::make_unique<FSR2Pass>();
    m_fsr2Pass->Init(m_gfx);
    // Shutdown destroys its FFX context and output texture while the device
    // lives. A hard cut clears its own temporal accumulation: the motion
    // vectors cannot describe a cut, so the pre-cut frame would smear into the
    // new view. Safe while FSR is inactive (m_reset is read only inside
    // FSR2Pass::Execute, which the worker skips at Native).
    m_passes.Register("FSR2", m_fsr2Pass);
    PostDone("FSR2Pass");

    // DLSS upscaler: replaces TAA in AAMode::DLSS. Allocated unconditionally (a few hundred bytes, no GPU
    // memory until EnsureContext), so the upscale branch never null-checks per hardware. The feature stays
    // dormant unless Streamline reports it supported.
    m_dlssPass = std::make_unique<DLSSPass>();
    m_dlssPass->Init(m_gfx);
    // Its shutdown needs a live device as FSR2's does: it holds an
    // engine-allocated output texture and turns the DLSS feature off.
    m_passes.Register("DLSS", m_dlssPass);
    PostDone("DLSSPass");
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -upscaler_fail=fsr|dlss (harness): that upscaler's context never comes up - what a frame does when one
    // fails at runtime.
    {
        const std::string fail = Sys::CommandLine::Get().Get("-upscaler_fail");
        m_fsr2Pass->failContextForTest = fail == "fsr";
        m_dlssPass->failContextForTest = fail == "dlss";
    }
#endif

    // The render features' passes outside the graph (RenderFeature.h, CreatePasses), among the AA passes;
    // [POSTSETUP] books each feature: the RR raster guides, the sky motion-vector fill and the glass
    // shatter (Renderer/Features/).
    // The post chain exists before the features' CreatePasses, which may add their post effects to it; the
    // Renderer adds its own and finalizes it once every pass exists.
    m_postChain = std::make_unique<PostProcess::PostChain>();
    featureCtx.post = m_postChain.get();
    m_features.CreatePasses(featureCtx, PostDone);
    featureCtx.post = nullptr;

    // The reactive mask FSR 2 / DLSS SR read. Owns no texture until an upscaler that reads it runs (Prepare
    // at the frame boundary).
    m_reactiveMaskPass = std::make_unique<UpscalerReactiveMaskPass>();
    m_reactiveMaskPass->Init(m_gfx);
    m_passes.Register("UpscalerReactiveMask", m_reactiveMaskPass);
    PostDone("UpscalerReactiveMaskPass");

    m_fxaaPass = std::make_unique<FXAAPass>();
    m_fxaaPass->Init(m_gfx);
    m_passes.Register("FXAA", m_fxaaPass);
    PostDone("FXAAPass");

    // Apply default AA mode (sets per-pass enabled flags).
    SetAAMode(m_aaMode);

    m_autoExposurePass = std::make_unique<AutoExposurePass>();
    m_autoExposurePass->Init(m_gfx);
    // Exposure adaptation carries no meaning across a world boundary: the
    // buffer still holds the previous level's adapted value, so without the
    // snap the new scene is tone-mapped with the old exposure and drags to its
    // own target over the full adaptation tau (~1.5 s of visibly wrong image).
    m_passes.Register("AutoExposure", m_autoExposurePass);
    PostDone("AutoExposurePass");
#ifdef WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-energy"))
    {
        m_lumaReducePass = std::make_unique<LumaReducePass>();
        m_lumaReducePass->Init(m_gfx);
        m_passes.Register("LumaReduce", m_lumaReducePass);
        PostDone("LumaReducePass");
    }
#endif

    m_bloomPass = std::make_unique<BloomPass>();
    m_bloomPass->SetAliasArena(&m_transientAliasArena);
    m_bloomPass->Init(m_gfx);
    m_passes.Register("Bloom", m_bloomPass);
    PostDone("BloomPass");

    m_lensFlarePass = std::make_unique<LensFlarePass>();
    m_lensFlarePass->Init(m_gfx);
    m_passes.Register("LensFlare", m_lensFlarePass);
    PostDone("LensFlarePass");

    m_toneMapPass = std::make_unique<ToneMapPass>();
    m_toneMapPass->Init(m_gfx);
    m_passes.Register("ToneMap", m_toneMapPass);
    PostDone("ToneMapPass");

    // XeGTAO is created and Init'd by the render graph (registered next to RTAOPass).

    m_casPass = std::make_unique<CASPass>();
    m_casPass->Init(m_gfx);
    m_passes.Register("CAS", m_casPass);
    PostDone("CASPass");

    m_underwaterPass = std::make_unique<UnderwaterPass>();
    m_underwaterPass->Init(m_gfx);
    m_passes.Register("Underwater", m_underwaterPass);
    PostDone("UnderwaterPass");

    m_dofPass = std::make_unique<DepthOfFieldPass>();
    m_dofPass->Init(m_gfx);
    m_passes.Register("DoF", m_dofPass);
    PostDone("DepthOfFieldPass");

    m_stylizePass = std::make_unique<StylizePass>();
    m_stylizePass->Init(m_gfx);
    m_passes.Register("Stylize", m_stylizePass);
    PostDone("StylizePass");

    // DebugTargetViewPass: the main view shows one render target instead of the lit image
    // (Renderer/DebugTargets.h). It builds nothing until a target is asked for; the registry names every
    // target it can show, and reads -debugtarget.
    m_debugTargetPass = std::make_unique<DebugTargetViewPass>();
    m_debugTargetPass->Init(m_gfx);
    m_passes.Register("DebugTargetView", m_debugTargetPass);
    // The Texture Viewer's pictures: builds nothing until a view is asked for.
    m_textureViews = std::make_unique<TextureViewService>(m_gfx);
    m_lifecycle.Add("TextureViews", m_textureViews.get())
        .OnShutdown([this] { if (m_textureViews) m_textureViews->Shutdown(); })
        .OnReloadShaders([this] { if (m_textureViews) m_textureViews->ReloadShaders(); });
    // The UI Editor's designer pictures: a second UIPass and its targets, built on the first live view.
    m_uiViews = std::make_unique<UIViewService>(m_gfx);
    m_lifecycle.Add("UIViews", m_uiViews.get())
        .OnShutdown([this] { if (m_uiViews) m_uiViews->Shutdown(); })
        .OnReloadShaders([this] { if (m_uiViews) m_uiViews->ReloadShaders(); });
    // The pass capture's copy and picture (Renderer/PassCapture.h): made on request, released here.
    m_lifecycle.Add("PassCapture", &m_passCapture)
        .OnShutdown([this] { ShutdownPassCapture(); });
    m_features.RegisterDebugTargets(m_featureDebugTargets);   // the RR raster guides' rows
    BuildDebugTargetRegistry();
    PostDone("DebugTargetViewPass");

    InitSkinningSystems();

    // PostProcess::PostChain adapters are non-owning; the unique_ptrs keep the passes alive. Built after
    // InitSkinningSystems, which makes the Overlay stage's passes. Each effect says its stage and Order;
    // Finalize orders the chain. The chain consumes the per-frame ResolvedPostProcessSettings from
    // PostProcessResolveSystem (volume blend + override stack); the Renderer points Context::resolved at
    // PostProcess::Runtime each frame.
    // The Resolve stage (PostProcess/ResolveEffects.h): AutoExposure (its metering reads the raw scene, so
    // the exposure is this frame's for every effect after it), NPRStabilize, RRColorAlign, the reactive
    // mask, FSR 2 | DLSS | TAA, FXAA.
    m_postChain->Add(std::make_unique<PostProcess::AutoExposureEffect>(m_autoExposurePass.get()));
    m_postChain->Add(std::make_unique<PostProcess::NPRStabilizeEffect>(m_nprStabilizePass.get()));
    m_postChain->Add(std::make_unique<PostProcess::RRColorAlignEffect>(m_rrColorSnapshot.get()));
    m_postChain->Add(std::make_unique<PostProcess::ReactiveMaskEffect>(m_reactiveMaskPass.get(), m_taaPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::FSR2Effect>(m_fsr2Pass.get()));
    m_postChain->Add(std::make_unique<PostProcess::DLSSEffect>(m_dlssPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::TAAEffect>(m_taaPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::FXAAEffect>(m_fxaaPass.get(), m_taaPass.get()));
    // -energy_after_aa's measurement: only when the harness made LumaReduce (-energy).
    if (m_lumaReducePass)
        m_postChain->Add(std::make_unique<PostProcess::EnergyAfterAAEffect>(m_lumaReducePass.get()));
    m_postChain->Add(std::make_unique<PostProcess::DepthOfFieldEffect>(m_dofPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::CASEffect>(m_casPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::BloomEffect>(m_bloomPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::LensFlareEffect>(m_lensFlarePass.get()));
    m_postChain->Add(std::make_unique<PostProcess::UnderwaterEffect>(m_underwaterPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::StylizeEffect>(m_stylizePass.get()));
    m_postChain->Add(std::make_unique<PostProcess::ToneMapEffect>(m_toneMapPass.get()));
    // The Overlay stage (PostProcess/OverlayEffects.h): over the finished picture on the restore list -
    // wires, world UI, icons, Frame Generation's inputs, the HUD.
    m_postChain->Add(std::make_unique<PostProcess::DebugWireEffect>(m_debugWirePass.get(), m_toneMapPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::WorldUIEffect>(m_worldUIPass.get(), m_toneMapPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::DebugIconsEffect>(m_debugIconPass.get(), m_toneMapPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::FrameGenHudLessEffect>(m_toneMapPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::FrameGenUILayerEffect>(m_uiPass.get()));
    m_postChain->Add(std::make_unique<PostProcess::UIEffect>(m_uiPass.get(), m_toneMapPass.get()));
    m_postChain->Finalize();

    // [POSTSETUP] (PostSetupClock): printed like [RGSETUP], when it adds up.
    {
        g_postSetup = nullptr;
        std::vector<std::pair<const char*, double>>& steps = postSetup.steps;
        const double postTotal = std::chrono::duration<double, std::milli>(
            PostSetupClock::Clock::now() - postSetup.start).count();
        if (postTotal > 100.0)
        {
            std::sort(steps.begin(), steps.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string slowest;
            for (size_t k = 0; k < steps.size() && k < 8; ++k)
            {
                char one[96];
                std::snprintf(one, sizeof(one), "%s%s %.0f", k ? ", " : "", steps[k].first, steps[k].second);
                slowest += one;
            }
            LOG_INFO("[POSTSETUP] %zu steps outside the graph in %.0f ms; slowest (ms): %s",
                     steps.size(), postTotal, slowest.c_str());
        }
    }

    // Every RenderPass this Compile constructed must be in the main graph or in the lifecycle lists
    // (RenderLifecycle.h): one in neither would never be reloaded, reset or shut down, and nothing else
    // would say so.
    {
        uint32_t inGraph = 0, listed = 0, neither = 0;
        for (RG::RenderPass* pass : RG::LivePassesSince(passCensusMark))
        {
            const bool inMainGraph = m_graph.Contains(pass);
            const bool registered  = !inMainGraph && m_lifecycle.Accounts(pass);
            inGraph += inMainGraph ? 1u : 0u;
            listed  += registered ? 1u : 0u;
            if (!ENGINE_CHECK(inMainGraph || registered,
                              "Renderer::Compile: RenderPass '%s' is neither in the main graph nor in the "
                              "lifecycle lists - register it where it is created (Renderer/RenderLifecycle.h)",
                              pass->GetName()))
                ++neither;
        }
        // passes = the RenderPasses Compile constructed: inGraph + listed + neither.
        LOG_INFO("[LIFECYCLE] census: passes=%u inGraph=%u listed=%u neither=%u entries=%zu",
                 inGraph + listed + neither, inGraph, listed, neither, m_lifecycle.Size());
        m_lifecycle.LogOrder();
    }
    RegisterPrepareOrder();
    RegisterServiceOrder();
    // Every pass that keeps a history in the view (kView) was handed the main view as it registered
    // (SetView, the top of Compile) and created its history object in it, empty; the passes still make
    // the GPU resources. [VIEW_STATE] lists them.
    m_passes.LogViews();

    LOG_INFO("Renderer: Compile complete");
}

// The order each prepare stage runs its passes in. It fixes the allocation order, and so the pictures:
// a reorder is an edit here, checked with an A/B. The stages and why each pass prepares there:
// RG::PrepareStage and the passes' OnPrepare.
void Renderer::RegisterPrepareOrder()
{
    using S = RG::PrepareStage;
    m_passes.SetPrepareOrder(S::FrameEarly, { { "XeGTAO", m_xegtaoPass }, { "RTReflection", m_rtReflectionPass },
                                               { "RTReflection.Front", m_rtReflectionFrontPass } });
    m_passes.SetPrepareOrder(S::FrameLate, { { "SurfaceCacheBake", m_surfaceCacheBakePass },
                                              { "NPRStabilize", m_nprStabilizePass.get() } });
    m_passes.SetPrepareOrder(S::HiZ, { { "HiZ", m_hiZPass.get() } });
    m_passes.SetPrepareOrder(S::PostAA, { { "TAA", m_taaPass.get() }, { "FXAA", m_fxaaPass.get() } });
    m_passes.SetPrepareOrder(S::PostPool, { { "DoF", m_dofPass.get() }, { "CAS", m_casPass.get() },
                                             { "Underwater", m_underwaterPass.get() }, { "Stylize", m_stylizePass.get() } });
    m_passes.SetPrepareOrder(S::PostEffects, { { "DoF", m_dofPass.get() }, { "CAS", m_casPass.get() },
                                                { "Bloom", m_bloomPass.get() }, { "NPRStabilize", m_nprStabilizePass.get() },
                                                { "LensFlare", m_lensFlarePass.get() }, { "Underwater", m_underwaterPass.get() },
                                                { "Stylize", m_stylizePass.get() }, { "AutoExposure", m_autoExposurePass.get() } });
    m_passes.SetPrepareOrder(S::PostOutput, { { "ToneMap", m_toneMapPass.get() } });
    m_passes.LogPrepareOrder();
}

// The order each service stage runs its passes in (fixed, like the prepare order). The compare shadows
// serve only with their main pass (the board carries its core).
void Renderer::RegisterServiceOrder()
{
    using S = RG::ServiceStage;
    m_passes.SetServiceOrder(S::Retry, { { "DLSS", m_dlssPass.get() } });
    m_passes.SetServiceOrder(S::Upscalers, { { "FSR2", m_fsr2Pass.get() }, { "DLSS", m_dlssPass.get() } });
    m_passes.SetServiceOrder(S::DirectLight, { { "PathTrace", m_pathTracePass } });
    m_passes.SetServiceOrder(S::Restir, { { "RTDI", m_rtDiPass }, { "RTDI.Shadow", m_rtDiPass ? m_rtDiShadowPass : nullptr },
                                          { "RTGI", m_rtGiPass }, { "RTGI.Shadow", m_rtGiPass ? m_rtGiShadowPass : nullptr } });
    m_passes.SetServiceOrder(S::PathTrace, { { "PathTrace", m_pathTracePass } });
    m_passes.SetServiceOrder(S::StaticGeometry, { { "PathTrace", m_pathTracePass }, { "RTGI", m_rtGiPass } });
    m_passes.SetServiceOrder(S::Lights, { { "Cluster", m_clusterPass.get() }, { "LightSampling", m_lightSamplingPass } });
    m_passes.SetServiceOrder(S::RRReset, { { "DLSS", m_dlssPass.get() } });
    m_passes.SetServiceOrder(S::RTReflectionTier, { { "RTReflection", m_rtReflectionPass },
                                                    { "RTReflection.Front", m_rtReflectionFrontPass } });
    m_passes.SetServiceOrder(S::WorkingSets, { { "RTReflection", m_rtReflectionPass },
                                               { "RRColorSnapshot", m_rrColorSnapshot.get() },
                                               { "UpscalerReactiveMask", m_reactiveMaskPass.get() },
                                               { "RTAO", m_rtaoPass }, { "SSGI", m_ssgiPass }, { "RTShadow", m_rtShadowPass },
                                               { "DebugTargetView", m_debugTargetPass.get() } });
    m_passes.SetServiceOrder(S::Residents, { { "VolumetricFog", m_volFogPass }, { "Skybox", m_skyboxPass },
                                             { "Bloom", m_bloomPass.get() }, { "NPRStabilize", m_nprStabilizePass.get() },
                                             { "LensFlare", m_lensFlarePass.get() }, { "SpotShadow", m_spotShadowPass },
                                             { "PointShadow", m_pointShadowPass } });
    m_passes.LogServiceOrder();
}

RG::PrepareContext Renderer::PrepareContextNow() const
{
    RG::PrepareContext c;
    c.gfx = &m_gfx;
    c.renderW = FrameViewRead().renderWidth;
    c.renderH = FrameViewRead().renderHeight;
    c.viewportW = FrameViewRead().vpWidth;
    c.viewportH = FrameViewRead().vpHeight;
    c.ssao = m_ssaoEnabled;
    return c;
}

// ==== Constant buffers and per-frame GPU buffers ====
void Renderer::CreateConstantBuffers()
{
    // ---- PerViewCB (triple-buffered) ----
    if (!m_perObjectCB.Create(m_gfx, "Renderer.PerViewCB"))
    {
        LOG_ERROR("Renderer: PerViewCB creation failed");
        return;
    }
    {
        FrameViewWrite().vpWidth  = static_cast<uint32_t>(m_gfx.GetWidth());
        FrameViewWrite().vpHeight = static_cast<uint32_t>(m_gfx.GetHeight());
        UploadFrameData(0, FrameViewWrite().vpWidth, FrameViewWrite().vpHeight);
    }

    // ---- TerrainParams CB (triple-buffered) ----
    if (!m_terrainCB.Create(m_gfx, "Renderer.TerrainCB"))
        LOG_ERROR("Renderer: TerrainCB creation failed");

    // ---- MeshletCullCB (b4, triple-buffered) ----
    if (!m_meshletCullCB.Create(m_gfx, "Renderer.MeshletCullCB"))
        LOG_ERROR("Renderer: MeshletCullCB creation failed");
    if (!m_meshletShadowCB.Create(m_gfx, "Renderer.MeshletShadowCB"))
        LOG_ERROR("Renderer: MeshletShadowCB creation failed");

    // ---- InstanceBuffer (GPUInstanceData per instance, UPLOAD heap, triple-buffered) ----
    // The direct-bind buffers; with retained instance slots they double as the dirty-staging ring (same
    // size / stride / mapping, no extra memory).
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(GPUInstanceData);
        desc.stride     = sizeof(GPUInstanceData);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (!m_gfx.CreateBuffer(desc, m_instanceBuffer[i]))
            {
                LOG_ERROR("Renderer: InstanceBuffer[%u] creation failed", i);
                return;
            }
            m_instanceBufferMapped[i] = m_gfx.MapBuffer(m_instanceBuffer[i]);
        }
    }

    // ---- Retained instances: one DEFAULT-heap instance buffer + a per-frame remap ring ----
    // The DEFAULT buffer holds records at stable slots; the remap buffers map draw-local indices to slots,
    // read by the VSes via ResourceDescriptorHeap (heap index published through PerViewCB offset 228;
    // 0 = identity). Requires Resource Binding Tier 3 (implied by the DXR-class hardware the engine
    // requires); without it retained mode disables itself and the per-frame direct-bind path runs.
    {
        bool ok = m_gfx.GetCapabilities().unboundedDescriptorArrays;
        if (!ok)
            LOG_WARNING("Renderer: Resource Binding Tier 3 unavailable — "
                        "retained instance slots disabled (legacy per-frame path)");

        if (ok)
        {
            RHI::GPUBufferDesc desc;
            desc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(GPUInstanceData);
            desc.stride     = sizeof(GPUInstanceData);
            desc.usage      = RHI::Usage::DEFAULT;
            desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            if (!m_gfx.CreateBuffer(desc, m_instanceBufferGPU))
            {
                LOG_ERROR("Renderer: DEFAULT InstanceBuffer creation failed — "
                          "retained instance slots disabled");
                ok = false;
            }
        }
        if (ok)
        {
            RHI::GPUBufferDesc rdesc;
            rdesc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(uint32_t);
            rdesc.stride     = sizeof(uint32_t);
            rdesc.usage      = RHI::Usage::UPLOAD;
            rdesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            for (uint32_t i = 0; i < kFrameSlots && ok; ++i)
            {
                if (!m_gfx.CreateBuffer(rdesc, m_instanceRemapBuffer[i]))
                {
                    LOG_ERROR("Renderer: InstanceRemap[%u] creation failed — "
                              "retained instance slots disabled", i);
                    ok = false;
                    break;
                }
                m_instanceRemapMapped[i]  = m_gfx.MapBuffer(m_instanceRemapBuffer[i]);
                m_instanceRemapHeapIdx[i] = m_gfx.GetBufferSRVHeapIndex(m_instanceRemapBuffer[i]);
                // Index 0 is the shader-side identity sentinel. The remap SRVs allocate long after the
                // engine-lifetime descriptors, so 0 / ~0u here means something is broken: fall back to the direct-bind
                // path rather than mis-map.
                if (m_instanceRemapHeapIdx[i] == 0 || m_instanceRemapHeapIdx[i] == ~0u)
                {
                    LOG_ERROR("Renderer: InstanceRemap[%u] heap index invalid (%u) — "
                              "retained instance slots disabled", i, m_instanceRemapHeapIdx[i]);
                    ok = false;
                }
            }
        }
        if (!ok)
        {
            m_retainInstances = false;
            for (uint32_t i = 0; i < kFrameSlots; ++i) m_instanceRemapHeapIdx[i] = 0;
        }
        else
        {
            LOG_INFO("Renderer: M5 retained instance slots ready "
                     "(static cap %u, dynamic reserve %u, remap heap idx %u/%u/%u)",
                     m_staticInstanceCap, kDynamicInstanceReserve,
                     m_instanceRemapHeapIdx[0], m_instanceRemapHeapIdx[1],
                     m_instanceRemapHeapIdx[2]);
        }
    }

    // ---- Dissolve noise (engine-global, 256^2 R8 tileable FBM, baked once) ----
    // CPU value-noise FBM on a wrapping lattice -> seamless tiling. Same create-with-initial-data path as
    // the font atlas pages; the bindless slot (== handleId) is published via PerViewCB.dissolveNoiseIdx
    // each frame.
    {
        constexpr uint32_t kNoiseDim = 256;
        std::vector<uint8_t> pixels(static_cast<size_t>(kNoiseDim) * kNoiseDim);

        // Deterministic integer lattice hash -> [0,1).
        auto latticeHash = [](uint32_t x, uint32_t y) -> float
        {
            uint32_t h = x * 374761393u + y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= (h >> 16);
            return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
        };
        // Tileable value noise at integer period `p` (lattice wraps at p).
        auto valueNoise = [&latticeHash](float u, float v, uint32_t p) -> float
        {
            const float fx = u * p, fy = v * p;
            const uint32_t x0 = static_cast<uint32_t>(fx) % p;
            const uint32_t y0 = static_cast<uint32_t>(fy) % p;
            const uint32_t x1 = (x0 + 1) % p, y1 = (y0 + 1) % p;
            float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
            tx = tx * tx * (3.0f - 2.0f * tx);   // smoothstep
            ty = ty * ty * (3.0f - 2.0f * ty);
            const float a = latticeHash(x0, y0), b = latticeHash(x1, y0);
            const float c = latticeHash(x0, y1), d = latticeHash(x1, y1);
            return (a + (b - a) * tx) + ((c - a) + (a - b + d - c) * tx) * ty;
        };

        for (uint32_t y = 0; y < kNoiseDim; ++y)
        {
            const float v = (y + 0.5f) / kNoiseDim;
            for (uint32_t x = 0; x < kNoiseDim; ++x)
            {
                const float u = (x + 0.5f) / kNoiseDim;
                // 5-octave FBM, base period 8 -> blobby dissolve fronts with fine crumble detail. Amplitude sum =
                // 1.9375.
                float n = 0.0f, amp = 1.0f;
                uint32_t period = 8;
                for (int oct = 0; oct < 5; ++oct)
                {
                    n      += valueNoise(u, v, period) * amp;
                    amp    *= 0.5f;
                    period *= 2;
                }
                n /= 1.9375f;
                pixels[static_cast<size_t>(y) * kNoiseDim + x] =
                    static_cast<uint8_t>(std::clamp(n * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }

        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.width      = kNoiseDim;
        td.height     = kNoiseDim;
        td.depth      = 1;
        td.format     = RHI::Format::R8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;

        RHI::SubresourceData sub{};
        sub.dataPtr    = pixels.data();
        sub.rowPitch   = kNoiseDim;
        sub.slicePitch = kNoiseDim * kNoiseDim;

        if (!m_gfx.CreateTexture(td, m_dissolveNoiseTex, &sub))
            LOG_ERROR("Renderer: dissolve noise texture creation failed — "
                      "dissolve renders with no effect");
        else
            LOG_INFO("Renderer: dissolve noise baked (%ux%u R8, bindless slot %u)",
                     kNoiseDim, kNoiseDim, m_dissolveNoiseTex.handleId);
    }

    // ---- ExecuteIndirect buffers ----
    {
        const uint64_t argSize = static_cast<uint64_t>(m_maxInstances) * sizeof(IndirectDrawCommand);

        // DEFAULT heap: the GPU-side indirect arg buffer (single: GPU-only, no CPU race; frames serialize on
        // the graphics queue). Retained mode: upload-copied, then zeroed in place. GPU compaction: the
        // CS-written working arg buffer.
        RHI::GPUBufferDesc desc;
        desc.size       = argSize;
        desc.stride     = sizeof(IndirectDrawCommand);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bindFlags = RHI::BindFlag::UNORDERED_ACCESS; // GPU culling writes here
        m_gfx.CreateBuffer(desc, m_indirectArgBuffer);

        // UPLOAD heap: CPU staging (triple-buffered). SHADER_RESOURCE so the compaction CS reads it directly as
        // the pristine args (UPLOAD is GENERIC_READ, a legal CS SRV; saves a per-frame 1 MB DEFAULT copy).
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_indirectArgUpload[i]))
                m_indirectArgMapped[i] = m_gfx.MapBuffer(m_indirectArgUpload[i]);
        }

        // Per-group survivor-command counts, m_maxInstances entries: groups are runs of commands, so they can
        // never outnumber them.
        desc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(uint32_t);
        desc.stride     = sizeof(uint32_t);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
        m_gfx.CreateBuffer(desc, m_drawCountBuffer);

        // Count zero source (UPLOAD, triple-buffered): memset once here, never written again; RenderAsync
        // copies numGroups x 4 bytes of zeros before each compaction dispatch (cheaper and simpler than a clear
        // CS or a ClearUAV descriptor pair).
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_drawCountUpload[i]))
            {
                m_drawCountMapped[i] = m_gfx.MapBuffer(m_drawCountUpload[i]);
                if (m_drawCountMapped[i])
                    std::memset(m_drawCountMapped[i], 0,
                                static_cast<size_t>(desc.size));
            }
        }

        // Readback ring for the post-cull survivor count. kFrameSlots deep so
        // the CPU never waits on the GPU: the slot resolved at the top of a
        // frame was written kFrameSlots frames ago and is long complete.
        m_visibleDrawCmdsEnabled = Sys::HasArtifactDir();
        if (m_visibleDrawCmdsEnabled)
        {
            RHI::GPUBufferDesc rb{};
            rb.size       = static_cast<uint64_t>(kDrawCountReadbackCap) * sizeof(uint32_t);
            rb.stride     = sizeof(uint32_t);
            rb.usage      = RHI::Usage::READBACK;
            rb.bindFlags = RHI::BindFlag::NONE;
            for (uint32_t i = 0; i < kFrameSlots; ++i)
                m_gfx.CreateBuffer(rb, m_drawCountReadback[i]);
        }

        // ---- Post-cull visible-geometry counters (u3) ----
        // Created unconditionally, unlike the survivor-count ring, which only feeds frames.jsonl (so -artifacts
        // gates it): these feed the debug HUD, which a player can turn on in a shipped build. Cost: 64 bytes of
        // GPU heap + 16 bytes x kFrameSlots of readback, one 16-byte CopyBufferRegion and one Map/Unmap per
        // frame, only on frames the counters are enabled.
        // 4 u32s: [0] triangles  [1] instances  [2] commands  [3] unused. Mirrors shaders/InstanceCull.cs.hlsl
        // g_VisStats.
        {
            m_visStatsEnabled = true;
            RHI::GPUBufferDesc vd{};
            vd.size       = kVisStatsElems * sizeof(uint32_t);
            vd.stride     = sizeof(uint32_t);
            vd.usage      = RHI::Usage::DEFAULT;
            vd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
            if (!m_gfx.CreateBuffer(vd, m_visStatsBuffer)) m_visStatsEnabled = false;

            // Zero source. The counters are pure accumulators, so they must be
            // cleared every frame before the dispatch; a CopyBufferRegion from
            // a permanently-zero UPLOAD buffer is the same trick the draw-count
            // buffer already uses.
            RHI::GPUBufferDesc zd = vd;
            zd.usage      = RHI::Usage::UPLOAD;
            zd.bindFlags = RHI::BindFlag::NONE;
            if (m_visStatsEnabled && m_gfx.CreateBuffer(zd, m_visStatsZeroUpload))
            {
                if (void* p = m_gfx.MapBuffer(m_visStatsZeroUpload))
                    std::memset(p, 0, kVisStatsElems * sizeof(uint32_t));
                else
                    m_visStatsEnabled = false;
            }
            else m_visStatsEnabled = false;

            RHI::GPUBufferDesc rd = vd;
            rd.usage      = RHI::Usage::READBACK;
            rd.bindFlags = RHI::BindFlag::NONE;
            for (uint32_t i = 0; i < kFrameSlots && m_visStatsEnabled; ++i)
                if (!m_gfx.CreateBuffer(rd, m_visStatsReadback[i]))
                    m_visStatsEnabled = false;

            if (!m_visStatsEnabled)
                LOG_WARNING("Renderer: visible-geometry counters unavailable "
                            "— the debug HUD will show them as absent");
        }

        LOG_INFO("Renderer: ExecuteIndirect buffers ready (max %u commands)", m_maxInstances);
    }

    // ---- GPU compaction resources (working remap + per-command meta) ----
    // Working remap: lower half [0, m_maxInstances) = the CPU-copied pristine mapping (full-count
    // consumers), upper half [m_maxInstances, 2x) = CS-compacted survivor slots (indirect draws only). Its
    // SRV heap index is published through the main PerViewCB only (byte 228) and read via
    // ResourceDescriptorHeap: the same Resource Binding Tier 3 requirement as the retained instance slots
    // (compaction depends on retained mode anyway).
    {
        bool ok = true;
        RHI::GPUBufferDesc rdesc;
        rdesc.size       = 2ull * m_maxInstances * sizeof(uint32_t);
        rdesc.stride     = sizeof(uint32_t);
        rdesc.usage      = RHI::Usage::DEFAULT;
        rdesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE |
                           RHI::BindFlag::UNORDERED_ACCESS;
        if (!m_gfx.CreateBuffer(rdesc, m_instanceRemapGPU))
        {
            LOG_ERROR("Renderer: working InstanceRemap creation failed — "
                      "GPU compaction disabled");
            ok = false;
        }
        if (ok)
        {
            m_instanceRemapGPUHeapIdx = m_gfx.GetBufferSRVHeapIndex(m_instanceRemapGPU);
            // 0 doubles as the shader-side identity sentinel: the same validity rule as the per-frame remap SRVs.
            if (m_instanceRemapGPUHeapIdx == 0 || m_instanceRemapGPUHeapIdx == ~0u)
            {
                LOG_ERROR("Renderer: working InstanceRemap heap index invalid "
                          "(%u) — GPU compaction disabled", m_instanceRemapGPUHeapIdx);
                ok = false;
            }
        }
        if (ok)
        {
            // Per-command group meta (groupBase << 16 | groupIdx; 0xFFFFFFFF = command in no indirect group). Both
            // indices are bounded by m_maxInstances, which the constructor pins at or below
            // kMaxInstancesUpperBound, so asserting the bound at compile time covers every runtime cap. groupBase
            // gets bits 16..30 only: bit 31 marks a main-view meshlet group, whose commands the cull CS converts to
            // DISPATCH_MESH.
            static_assert(kMaxInstancesUpperBound <= (1u << 15),
                          "cmd-group meta packs groupBase into bits 16..30 (bit 31 = meshlet group)");
            RHI::GPUBufferDesc mdesc;
            mdesc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(uint32_t);
            mdesc.stride     = sizeof(uint32_t);
            mdesc.usage      = RHI::Usage::UPLOAD;
            mdesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            for (uint32_t i = 0; i < kFrameSlots && ok; ++i)
            {
                if (!m_gfx.CreateBuffer(mdesc, m_cmdGroupMeta[i]))
                {
                    LOG_ERROR("Renderer: cmd-group meta[%u] creation failed — "
                              "GPU compaction disabled", i);
                    ok = false;
                    break;
                }
                m_cmdGroupMetaMapped[i] = m_gfx.MapBuffer(m_cmdGroupMeta[i]);
                if (!m_cmdGroupMetaMapped[i]) ok = false;
            }
        }
        if (!ok)
        {
            m_gpuCompaction           = false;
            m_instanceRemapGPUHeapIdx = 0;
        }
        else
        {
            LOG_INFO("Renderer: M6 GPU compaction ready (working remap heap idx %u, "
                     "count buffer %u groups max)",
                     m_instanceRemapGPUHeapIdx, m_maxInstances);
        }
    }

    // ---- Shadow-family per-view cull resources ----
    // Per cull view: a working arg buffer (mode-2 CS output, consumed by the shadow pass's ExecuteIndirect)
    // + a working remap (the VS reads it through the cascade CB's bank-two heap index; no pristine lower
    // half - every surviving command's range is CS-written, so m_maxInstances entries suffice). One shared
    // (view x bucket) count grid + its create-time-memset zero source, and a triple-buffered CPU meta ring
    // for the shadow-bucket routing. Any failure clears m_retainedShadowArgs; the ring path needs none of
    // this.
    {
        // The passes consume these arrays 1:1: the counts must agree, and the three families must not overlap
        // in the sliced buffers.
        static_assert(kShadowCullViews == ShadowPass::kRetainedCullCascades,
                      "shadow cull view count drift vs ShadowPass");
        static_assert(kSpotCullViews  == SpotShadowPass::kUpdateBudget,
                      "spot cull view count drift vs SpotShadowPass");
        static_assert(kPointCullViews == PointShadowPass::kMaxCasters,
                      "point cull view count drift vs PointShadowPass");
        static_assert(kSpotCullViewBase  >= kShadowCullViews &&
                      kPointCullViewBase >= kSpotCullViewBase + kSpotCullViews &&
                      kShadowCullViewsTotal >= kPointCullViewBase + kPointCullViews,
                      "shadow cull family slices alias");
        // Each shadow view and the main view take their own CullingCB from the frame's constants: no pool to
        // outgrow.
        bool ok = true;
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(IndirectDrawCommand);
        desc.stride     = sizeof(IndirectDrawCommand);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
        for (uint32_t v = 0; v < kShadowCullViewsTotal && ok; ++v)
            if (!m_gfx.CreateBuffer(desc, m_shadowWorkingArgs[v])) ok = false;

        desc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(uint32_t);
        desc.stride     = sizeof(uint32_t);
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE |
                          RHI::BindFlag::UNORDERED_ACCESS;
        for (uint32_t v = 0; v < kShadowCullViewsTotal && ok; ++v)
        {
            if (!m_gfx.CreateBuffer(desc, m_shadowWorkingRemap[v])) { ok = false; break; }
            m_shadowRemapHeapIdx[v] = m_gfx.GetBufferSRVHeapIndex(m_shadowWorkingRemap[v]);
            // 0 doubles as the shader-side identity sentinel (gpu_instance.hlsli).
            if (m_shadowRemapHeapIdx[v] == 0 || m_shadowRemapHeapIdx[v] == ~0u)
                ok = false;
        }

        const uint64_t countBytes = static_cast<uint64_t>(kShadowCullViewsTotal) *
                                    ShadowDrawGroup::RetainedCount * sizeof(uint32_t);
        if (ok)
        {
            desc.size       = countBytes;
            desc.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
            if (!m_gfx.CreateBuffer(desc, m_shadowGroupCounts)) ok = false;
        }
        if (ok)
        {
            desc.usage      = RHI::Usage::UPLOAD;
            desc.bindFlags = RHI::BindFlag::NONE;
            if (m_gfx.CreateBuffer(desc, m_shadowCountZeroUpload))
            {
                if (void* p = m_gfx.MapBuffer(m_shadowCountZeroUpload))
                {
                    std::memset(p, 0, static_cast<size_t>(countBytes));
                    m_gfx.UnmapBuffer(m_shadowCountZeroUpload);
                }
                else ok = false;
            }
            else ok = false;
        }
        if (ok)
        {
            RHI::GPUBufferDesc mdesc;
            mdesc.size       = static_cast<uint64_t>(m_maxInstances) * sizeof(uint32_t);
            mdesc.stride     = sizeof(uint32_t);
            mdesc.usage      = RHI::Usage::UPLOAD;
            mdesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            for (uint32_t i = 0; i < kFrameSlots && ok; ++i)
            {
                if (!m_gfx.CreateBuffer(mdesc, m_shadowCmdMeta[i])) { ok = false; break; }
                m_shadowCmdMetaMapped[i] = m_gfx.MapBuffer(m_shadowCmdMeta[i]);
                if (!m_shadowCmdMetaMapped[i]) ok = false;
            }
        }
        if (!ok)
        {
            m_retainedShadowArgs = false;
            for (uint32_t v = 0; v < kShadowCullViewsTotal; ++v)
                m_shadowRemapHeapIdx[v] = 0;
            LOG_ERROR("Renderer: M7 shadow cull resource creation failed — "
                      "retained shadow args disabled (ring path active)");
        }
        else
        {
            LOG_INFO("Renderer: M7/M8 shadow-family cull ready (%u views = %u "
                     "cascades + %u spot slices + %u point cubes, "
                     "cascade remap heap idx %u/%u/%u)",
                     kShadowCullViewsTotal, kShadowCullViews, kSpotCullViews,
                     kPointCullViews, m_shadowRemapHeapIdx[0],
                     m_shadowRemapHeapIdx[1], m_shadowRemapHeapIdx[2]);
        }
    }

    // ---- LightCB (triple-buffered) ----
    if (!m_lightCB.Create(m_gfx, "Renderer.LightCB"))
    {
        LOG_ERROR("Renderer: LightCB creation failed");
        return;
    }
    // The fog for surfaces drawn after the fog composites. Created zeroed (no fog, no soft fade) until
    // WriteFogApplyCB's first frame.
    if (!m_fogApplyCB.Create(m_gfx, "Renderer.FogApplyCB"))
    {
        LOG_ERROR("Renderer: FogApplyCB creation failed");
        return;
    }
    // Without it the path tracers see no fog (UploadMediumSet uploads nothing).
    if (!m_mediumSet.Init(m_gfx))
        LOG_ERROR("Renderer: MediumSet creation failed");
    {
        auto norm3 = [](float x, float y, float z, float o[3]) {
            float l = std::sqrt(x*x + y*y + z*z);
            o[0] = x/l; o[1] = y/l; o[2] = z/l;
        };
        LightCB lb{};
        norm3(1.f, -2.f, 0.5f, lb.lightDir);
        lb.lightColor[0] = 1.f;  lb.lightColor[1] = 0.92f; lb.lightColor[2] = 0.82f;
        lb.cameraPos[0]  = FrameViewWrite().camera.position.x;
        lb.cameraPos[1]  = FrameViewWrite().camera.position.y;
        lb.cameraPos[2]  = FrameViewWrite().camera.position.z;
        // A typed handle field's "absent" is its kind's null, never 0: heap slot 0 is a live bindless-table
        // entry.
        lb.rtReflectionIdx = lb.rtShadowMaskIdx = lb.perObjShadowIdx = lb.hairShadowIdx
                           = lb.cloudShadowIdx = BINDLESS_NULL_TEX2D_FLOAT;
        lb.pointShadowIdx  = BINDLESS_NULL_TEXCUBEARRAY_FLOAT;
        lb.ssrResultIdx    = BINDLESS_NULL_TEX2D_FLOAT;
        lb.fwdReflNormalIdx = lb.fwdReflDepthIdx = lb.fwdReflPrevHdrIdx
                            = lb.fwdReflFrontRtrIdx = lb.fwdReflFrontDepthIdx
                            = lb.fwdAoIdx = BINDLESS_NULL_TEX2D_FLOAT;
        lb.ssgiResultIdx = lb.ptResultIdx = lb.restirGiIdx = lb.nprPtAmbientIdx
                         = BINDLESS_NULL_TEX2D_FLOAT;
        lb.restirDiIdx   = BINDLESS_NULL_TEX2DARRAY_FLOAT;
        lb.faceFrameIdx  = lb.exposureBufIdx = BINDLESS_NULL_STRUCTURED;
        lb.ddgiVolumeBufIdx  = m_gfx.NullOf<RHI::Bindless::Kind::StructuredBuffer>(
                                   static_cast<uint32_t>(sizeof(DDGI::VolumeGPUDesc))).Bits();
        lb.reflProbeGridIdx  = m_gfx.NullOf<RHI::Bindless::Kind::StructuredBuffer>(8u).Bits();
        lb.reflProbeIndexIdx = m_gfx.NullOf<RHI::Bindless::Kind::StructuredBuffer>(4u).Bits();
        lb.reflProbeBufIdx   = m_gfx.NullOf<RHI::Bindless::Kind::StructuredBuffer>(
                                   static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe))).Bits();
        // Seed every slot so the first kFrameSlots frames don't read junk before
        // BuildScene_UploadLights has overwritten the slot the GPU is reading.
        for (uint32_t i = 0; i < kFrameSlots; ++i)
            if (m_lightCB.mapped[i]) *m_lightCB.mapped[i] = lb;
    }

    // ---- SpotShadow VP matrix buffer (triple-buffered) ----
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(SpotShadowPass::kMaxCasters)
                        * sizeof(SpotShadowViewGPU);
        desc.stride     = sizeof(SpotShadowViewGPU);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        DirectX::XMFLOAT4X4 ident;
        DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_spotShadowVPBuffer[i]))
            {
                m_spotShadowVPMapped[i] = m_gfx.MapBuffer(m_spotShadowVPBuffer[i]);
                m_spotShadowVPSrv[i]    = m_gfx.GetBufferSRVGpuHandle(m_spotShadowVPBuffer[i]);
                if (m_spotShadowVPMapped[i])
                {
                    auto* dst = static_cast<SpotShadowViewGPU*>(m_spotShadowVPMapped[i]);
                    for (uint32_t k = 0; k < SpotShadowPass::kMaxCasters; ++k)
                        dst[k] = SpotShadowViewGPU{ ident };
                }
            }
        }
    }

    // ---- Terrain per-layer material table (triple-buffered) ----
    // StructuredBuffer<TerrainLayerGPU> the terrain PS loops over by layerCount. Seeded to a disabled layer
    // (all idx = -1) so a frame before SyncTerrain runs reads benign data rather than junk.
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(kMaxTerrainLayers)
                        * sizeof(RendererDetail::TerrainLayerGPU);
        desc.stride     = sizeof(RendererDetail::TerrainLayerGPU);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            if (m_gfx.CreateBuffer(desc, m_terrainLayerBuffer[i]))
            {
                m_terrainLayerMapped[i] = m_gfx.MapBuffer(m_terrainLayerBuffer[i]);
                m_terrainLayerSrv[i]    = m_gfx.GetBufferSRVGpuHandle(m_terrainLayerBuffer[i]);
                if (m_terrainLayerMapped[i])
                {
                    auto* dst = static_cast<RendererDetail::TerrainLayerGPU*>(m_terrainLayerMapped[i]);
                    for (uint32_t k = 0; k < kMaxTerrainLayers; ++k)
                    {
                        RendererDetail::TerrainLayerGPU empty{};
                        empty.albedoIdx = empty.normalIdx = empty.armIdx = empty.dispIdx = -1;
                        dst[k] = empty;
                    }
                }
            }
        }
    }

    // ---- MaterialBuffer staging (StructuredBuffer<MaterialGPUData>, UPLOAD) ----
    // The CPU-side staging twins of the DEFAULT-heap m_materialBufferGPU below, which the shaders read (a
    // per-pixel g_Materials fetch must not walk write-combined system memory over PCIe). Triple-buffered
    // because the CPU writes a slot while older frames may be in flight, and each stays a complete material
    // table for ProcessProbeBakeQueue's direct read.
    // Slot-indexed: WriteMaterialSlot writes at the persistent slot, never a per-frame compacted cursor, so
    // staging offset == GPU offset and a dirty range is a plain (slot, count) span.
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(m_maxMaterials) * sizeof(Resource::MaterialGPUData);
        desc.stride     = sizeof(Resource::MaterialGPUData);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t s = 0; s < kFrameSlots; ++s)
        {
            if (!m_gfx.CreateBuffer(desc, m_materialBuffer[s]))
            {
                LOG_ERROR("Renderer: MaterialBuffer slot %u creation failed", s);
                return;
            }
            m_materialBufferMapped[s] = m_gfx.MapBuffer(m_materialBuffer[s]);

            // Pre-fill EVERY slot with default PBR values so any of the first
            // kFrameSlots frames reads valid data even before the first scene
            // gather has written its visible materials.
            if (m_materialBufferMapped[s])
            {
                auto* data = static_cast<Resource::MaterialGPUData*>(m_materialBufferMapped[s]);
                for (uint32_t i = 0; i < m_maxMaterials; ++i)
                {
                    data[i] = {};
                    data[i].roughnessMin = 0.0f;  data[i].roughnessMax = 0.5f;
                    data[i].metalnessMin = 0.0f;  data[i].metalnessMax = 0.0f;
                    data[i].reflectance  = 0.5f;  // F0 = 0.16 * 0.5^2 = 0.04
                    data[i].baseColor[0] = data[i].baseColor[1] =
                    data[i].baseColor[2] = data[i].baseColor[3] = 1.0f;
                    // Identity UV transform: `data[i] = {}` zeroes tiling, and a (0,0) tiling collapses every UV to a
                    // single texel.
                    data[i].uvTilingOffset[0] = data[i].uvTilingOffset[1] = 1.0f;
                    data[i].uvTilingOffset[2] = data[i].uvTilingOffset[3] = 0.0f;
                    for (auto& id : data[i].textureHandleIds) id = -1;
                }
            }
        }
        // Fresh staging content -> the DEFAULT-heap copies of the reserved neutral slot 0 must be re-staged
        // (m_matSlot0Staged).
        m_matSlot0Staged = 0;
    }

    // ---- Face frame records ----
    // One float4 per material slot per frame slot, read by the deferred toon at
    // ResourceDescriptorHeap[LightCB.faceFrameIdx] where the slot's record has kMatGpuFlagFrameRecord.
    // Zero-filled once; only face slots are written (by the emit, on the slot's first sight each frame).
    // Failure costs only the feature: faceFrameIdx stays null and the deferred toon falls back to
    // customParams[0..1].
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(m_maxMaterials) * 4u * sizeof(float);
        desc.stride     = 4u * sizeof(float);
        desc.usage      = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t s = 0; s < kFrameSlots; ++s)
        {
            if (!m_gfx.CreateBuffer(desc, m_matFrameRecord[s]))
            {
                LOG_ERROR("Renderer: face frame record buffer %u creation failed", s);
                continue;
            }
            m_matFrameRecordMapped[s] = m_gfx.MapBuffer(m_matFrameRecord[s]);
            if (m_matFrameRecordMapped[s])
                std::memset(m_matFrameRecordMapped[s], 0, static_cast<size_t>(desc.size));
        }
    }

    // ---- MaterialBuffer GPU (StructuredBuffer<MaterialGPUData>, DEFAULT) ----
    // The buffer every per-pixel consumer binds. Kept coherent by the dirty-range blit in RenderAsync (as
    // m_instanceBufferGPU): the retained-slot bookkeeping reports exactly which slots changed, so an
    // unedited scene copies nothing per frame instead of the whole table (m_maxMaterials x
    // sizeof(Resource::MaterialGPUData), a few MB) over PCIe. The record size's authority is the
    // static_assert in SystemHandles.h; the LOG_INFO below prints the real MB from desc.size.
    // Non-fatal: on failure MaterialSRVBuffer() falls back to the UPLOAD twin.
    {
        RHI::GPUBufferDesc desc;
        desc.size       = static_cast<uint64_t>(m_maxMaterials) * sizeof(Resource::MaterialGPUData);
        desc.stride     = sizeof(Resource::MaterialGPUData);
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (!m_gfx.CreateBuffer(desc, m_materialBufferGPU))
        {
            LOG_ERROR("Renderer: DEFAULT MaterialBuffer creation failed — "
                      "falling back to the UPLOAD-heap material bind");
            m_materialBufferGPU = {};
        }
        else
        {
            // Seed it with staging[0]'s default-PBR pre-fill on the first frame's RenderAsync; never-written slots
            // must not read DEFAULT-heap garbage. Seed both upload records (construction time, not a frame half) so
            // whichever is published first carries the full-init request.
            for (auto& mu : m_matUploadBuf) { mu.fullInit = true; mu.ranges.clear(); }
            LOG_INFO("Renderer: DEFAULT material buffer ready (%u slots, %.2f MB)",
                     m_maxMaterials,
                     float(desc.size) / (1024.0f * 1024.0f));
        }
    }

    // Reflection probe pool: non-fatal on partial init.
    m_probeMgr.Init(m_gfx);
    // Its capture pass reloads with the standalone passes (RenderLifecycle.h);
    // the pool's own Shutdown stays in ~Renderer, a member like the rest.
    m_passes.Register("ReflectionProbeCapture", &m_probeMgr.GetCapturePass());

    // DDGI: per-volume resources are made lazily by BuildScene_UpdateDDGI; the trace pass needs DXR.
    // No DXR -> no pass; LightingPass's ddgiVolumeCount == 0 fallback handles it.
    if (m_ddgiMgr.Init(m_gfx))
    {
        if (m_gfx.GetCapabilities().raytracing)
        {
            m_ddgiPass = std::make_unique<DDGIPass>();
            m_ddgiReady = m_ddgiPass->Init(m_gfx);
            if (!m_ddgiReady)
            {
                m_ddgiPass.reset();
                LOG_INFO("Renderer: DDGI pass init failed — DDGI disabled");
            }
            else
            {
                // Its RTPSO + relight CS are not in the render graph: its own
                // ReloadShaders rebuilds the state object from disk.
                m_passes.Register("DDGI", m_ddgiPass);
            }
        }
        else
        {
            LOG_INFO("Renderer: DXR not supported — DDGI disabled (Sky IBL fallback only)");
        }

        // Probe debug viz works without DXR; spheres render dim but visible (+0.02 ambient floor).
        m_ddgiProbeDebugPass = std::make_unique<DDGIProbeDebugPass>();
        if (!m_ddgiProbeDebugPass->Init(m_gfx))
        {
            m_ddgiProbeDebugPass.reset();
            LOG_INFO("Renderer: DDGI probe debug pass init failed");
        }
        else
            m_passes.Register("DDGIProbeDebug", m_ddgiProbeDebugPass);
    }

    // RTXDI integration probe: builds the RTXDI DI / GI / PT pipelines through the engine's bridge and
    // logs one [RTXDI] probe line; a shader reload rebuilds it (ReloadShaders).
    if (Sys::CommandLine::Get().Has("-rtxdi_probe"))
    {
        if (m_gfx.GetCapabilities().inlineRaytracing)
        {
            m_rtxdiProbe = std::make_unique<RtxdiProbe>();
            m_rtxdiProbe->Build(m_gfx, m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight());
            m_lifecycle.Add("RtxdiProbe", m_rtxdiProbe.get())
                .OnShutdown([this] { if (m_rtxdiProbe) m_rtxdiProbe->Release(m_gfx); })
                .OnReloadShaders([this]
                    { if (m_rtxdiProbe) m_rtxdiProbe->ReloadShaders(m_gfx); });
        }
        else LOG_WARNING("[RTXDI] probe skipped: no inline ray tracing");
    }
}


// ==== Post-graph systems (InitSkinningSystems, called once from Compile) ====
void Renderer::InitSkinningSystems()
{
    // Pose / vertex rings, ECS systems, SkinningPass: all owned by m_skin.
    m_skin.Init(m_gfx, m_meshMgr);
    // The pass reloads with the standalone passes; the subsystem stays a member
    // that OnWorldClear orders by hand.
    m_lifecycle.Add("Skinning", &m_skin)
        .Owns(m_skin.GetSkinningPass())
        .OnReloadShaders([this] { if (SkinningPass* skin = m_skin.GetSkinningPass()) skin->ReloadShaders(m_gfx); });
    PostDone("SkinningSystem");

    // Per-material CBV / SRV rings: the first init hook that runs after the CBV/SRV heaps are ready.
    m_customMatCbvRing.Init(m_gfx);
    m_customMatSrvRing.Init(m_gfx);
    // Persistent per-material-slot stores; the rings stay as the flag-off / fallback path (an Init
    // failure -> IsValid() false -> SortAndEmit uses the rings).
    m_customMatCbvStore.Init(m_gfx, m_maxMaterials);
    m_customMatSrvStore.Init(m_gfx);
    PostDone("customMaterialRings+Stores");

    // ---- Interactive water ripples (GPU height-field wave sim) ----
    m_waterRippleSystem = std::make_unique<WaterRippleSystem>();
    m_waterRippleSystem->Init(m_gfx);
    // Ripple slots are keyed by Entity and ids are RECYCLED across worlds: a new
    // entity landing on an old id would inherit the previous world's 512^2 field,
    // and stale m_prevBodyPos would spray phantom splashes on frame 1. Shutdown
    // frees the height-field textures (needs the device alive).
    AddReloadable(m_lifecycle, "WaterRipple", m_waterRippleSystem, m_gfx)
        .OnShutdown([this] { if (m_waterRippleSystem) m_waterRippleSystem->Shutdown(); })
        .OnWorldClear([this](const RenderLifecycle::WorldClearArgs&)
            { if (m_waterRippleSystem) m_waterRippleSystem->OnWorldClear(); });
    PostDone("WaterRippleSystem");

    // ---- Particle system (GPU pool, emit + update compute, billboard render) ----
    m_particleSystem = std::make_unique<ParticleSystem>();
    m_particleSystem->Init(m_gfx);
    // A world boundary drops the ambient volume slots and the emitter texture
    // cache, and the pools too unless the world survives.
    m_lifecycle.Add("ParticleSystem", m_particleSystem.get())
        .OnWorldClear([this](const RenderLifecycle::WorldClearArgs& a)
            { if (m_particleSystem) m_particleSystem->OnWorldClear(m_gfx, a.worldSurvives); });

    m_particleSystem->SetResourceSystems(m_texSys, m_resMgr);

    m_particleSimPass = std::make_unique<ParticleSimPass>();
    m_particleSimPass->Init(m_gfx);
    m_particleSimPass->SetSystem(m_particleSystem.get());
    m_passes.Register("ParticleSim", m_particleSimPass);
    PostDone("ParticleSystem+SimPass");

    // ParticleRenderPass added to graph in Compile(); SetSystem is pure data-store, post-Init fine.
    if (m_particleRenderPass)
    {
        m_particleRenderPass->SetSystem(m_particleSystem.get());
        // Material-driven particle shaders register/reflect against the render
        // pass's library so param reflection and the PSO cache agree on IDs.
        m_particleSystem->SetShaderLibrary(&m_particleRenderPass->GetShaderLibrary());
    }

    // ---- Trail system (GPU ribbon trails) ----
    m_trailSystem = std::make_unique<TrailSystem>();
    m_trailSystem->Init(m_gfx);
    m_trailSystem->SetResourceSystems(m_texSys, m_resMgr);
    m_lifecycle.Add("TrailSystem", m_trailSystem.get())   // its texture cache goes with the world
        .OnWorldClear([this](const RenderLifecycle::WorldClearArgs&)
            { if (m_trailSystem) m_trailSystem->OnWorldClear(m_gfx); });

    m_trailUpdatePass = std::make_unique<TrailUpdatePass>();
    m_trailUpdatePass->Init(m_gfx);
    m_trailUpdatePass->SetSystem(m_trailSystem.get());
    m_passes.Register("TrailUpdate", m_trailUpdatePass);
    PostDone("TrailSystem+UpdatePass");

    if (m_trailRenderPass)
    {
        m_trailRenderPass->SetSystem(m_trailSystem.get());
        // Custom trail shaders register/reflect against the render pass's
        // library so param reflection and the PSO cache agree on IDs.
        m_trailSystem->SetShaderLibrary(&m_trailRenderPass->GetShaderLibrary());
    }

    // ---- Tracer system (cylindrical-billboard thin-laser pool) ----
    m_tracerSystem = std::make_unique<TracerSystem>();
    m_tracerSystem->Init(m_gfx);
    // No OnWorldClear, on purpose: its noise textures (.ivfx `noiseTex=`) are kept across world clears.
    // TracerSystem has no world-clear kill, so tracers alive across a scene switch keep sampling their
    // stored bindless index; releasing the textures would let the deferred destroy recycle the descriptor
    // under them (garbage noise). A handful of small noise .itex held for the renderer's lifetime is the
    // cheap, safe trade.

    m_tracerSimPass = std::make_unique<TracerSimPass>();
    m_tracerSimPass->Init(m_gfx);
    m_tracerSimPass->SetSystem(m_tracerSystem.get());
    m_passes.Register("TracerSim", m_tracerSimPass);
    PostDone("TracerSystem+SimPass");

    if (m_tracerRenderPass)
        m_tracerRenderPass->SetSystem(m_tracerSystem.get());

    // ---- Beam system (procedural tube CS generator) ----
    m_beamSystem = std::make_unique<BeamSystem>();
    m_beamSystem->Init(m_gfx, m_meshMgr.GetDescriptorHeap());

    m_beamSimPass = std::make_unique<BeamSimPass>();
    m_beamSimPass->Init(m_gfx);
    m_beamSimPass->SetSystem(m_beamSystem.get());
    m_passes.Register("BeamSim", m_beamSimPass);
    PostDone("BeamSystem+SimPass");

    // ---- Afterimage system (post-skinning vertex snapshot pool) ----
    m_afterimageSystem = std::make_unique<AfterimageSystem>();
    m_afterimageSystem->Init(m_gfx, m_meshMgr.GetDescriptorHeap());
    m_lifecycle.Add("AfterimageSystem", m_afterimageSystem.get())
        .OnWorldClear([this](const RenderLifecycle::WorldClearArgs&)
            { if (m_afterimageSystem) m_afterimageSystem->OnWorldClear(); });

    m_afterimageCapturePass = std::make_unique<AfterimageCapturePass>();
    m_afterimageCapturePass->Init(m_gfx);
    m_afterimageCapturePass->SetSystem(m_afterimageSystem.get());
    m_passes.Register("AfterimageCapture", m_afterimageCapturePass);
    PostDone("AfterimageSystem+CapturePass");

    // ---- ClusterPass (clustered deferred lighting) ----
    m_clusterPass = std::make_unique<ClusterPass>();
    m_clusterPass->Init(m_gfx);
    // A world boundary resets the light keys: no light of the old world maps into the new one (the stable
    // keys restart with the world's gather counter).
    m_passes.Register("Cluster", m_clusterPass);
    PostDone("ClusterPass");

    // The probe cluster SRVs persist after ClusterPass::Init: wire them to LightingPass once. LightingPass
    // only stores the handles; RenderAsync's per-frame resolve() block turns them into
    // LightCB.reflProbeGridIdx / reflProbeIndexIdx. One wire suffices: the buffers are DEFAULT-heap and
    // ClusterPass never recreates them.
    if (m_lightingPass)
    {
        m_lightingPass->SetReflectionProbeCluster(
            m_clusterPass->GetProbeGridSRVHandle(),
            m_clusterPass->GetProbeIndexSRVHandle());
    }

    // DecalPass reuses ClusterPass cluster AABB SRV; persistent for program lifetime.
    if (m_decalPass)
        m_decalPass->SetClusterAABBSRV(m_clusterPass->GetClusterAABBSRVHandle());

    // ---- CullingPass (GPU frustum culling) ----
    m_cullingPass = std::make_unique<CullingPass>();
    m_cullingPass->Init(m_gfx);
    m_passes.Register("Culling", m_cullingPass);
    PostDone("CullingPass");

    // ---- HiZPass (hierarchical-Z mip chain for occlusion culling) ----
    m_hiZPass = std::make_unique<HiZPass>();
    m_hiZPass->Init(m_gfx);
    m_passes.Register("HiZ", m_hiZPass);
    PostDone("HiZPass");

    // SSR pipeline lives behind a single owning subsystem; see Graphics/SSR/SSRSubsystem.h.
    m_ssrSubsystem = std::make_unique<SSRSubsystem>();
    m_ssrSubsystem->Init(m_gfx);
    m_passes.RegisterView("SSR", m_ssrSubsystem->TemporalPass());   // its history lives in the view
    // Its reload re-fetches all 7 compute shaders, rebuilds the PSOs and resets the temporal history; a
    // hard cut resets the history alone, through the view (SSRTemporalPass::History). A resize is OnResize
    // in BeginFrame: it hands back a new heap index.
    m_lifecycle.Add("SSR", m_ssrSubsystem.get())
        .OnReloadShaders([this]
        {
            if (m_ssrSubsystem) m_ssrSubsystem->ReloadShaders(m_gfx);
            for (SecondaryView& v : m_secondary)
                if (v.ssr) v.ssr->ReloadShaders(m_gfx);
        });

    // Allocate the SSR chain's render targets at the current render resolution. No consumer is wired here
    // on purpose: the only reader, Water.ps, reads LightCB.ssrResultIdx, which BeginFrame's OnResize ->
    // SetSSRResult -> resolve() sequence refreshes every frame; a handle wired at Init would go stale on
    // the first resize.
    m_ssrSubsystem->OnResize(m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight());
    PostDone("SSRSubsystem");

    // ---- DebugWirePass (AABB + frustum wireframe) ----
    m_debugWirePass = std::make_unique<DebugWirePass>();
    m_debugWirePass->Init(m_gfx);
    m_passes.Register("DebugWire", m_debugWirePass);
    PostDone("DebugWirePass");

    // ---- UIPass (HUD / menus): renders on the LDR tonemap output ----
    m_uiPass = std::make_unique<UIPass>();
    m_uiPass->Init(m_gfx);
    m_passes.Register("UI", m_uiPass);
    PostDone("UIPass");

    // ---- WorldUIBillboardPass (world-space UI: HP bars / names) ----
    m_worldUIPass = std::make_unique<WorldUIBillboardPass>();
    m_worldUIPass->Init(m_gfx);
    m_passes.Register("WorldUI", m_worldUIPass);
    PostDone("WorldUIBillboardPass");

    // ---- DebugIconPass (editor-only light/camera gizmo icons) ----
    m_debugIconPass = std::make_unique<DebugIconPass>();
    m_debugIconPass->Init(m_gfx);
    m_passes.Register("DebugIcon", m_debugIconPass);
    PostDone("DebugIconPass");

    m_meshMgr.InitBillboardQuad();
    PostDone("BillboardQuad");

    // m_skin.Init() flips the IsInitialised() flag; nothing more to do here.
    LOG_SUCCESS("Renderer: post-compile systems initialised");
}

