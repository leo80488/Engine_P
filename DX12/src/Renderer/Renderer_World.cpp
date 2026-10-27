// Renderer_World.cpp - the Renderer's side of the World's lifecycle: the subscriptions (entity
// destroy, the undo park / restore), the structural sync point, OnWorldClear, the editor world's
// resource capture / restore and the loaded mesh-library listing.

#include "Diagnostics/Profiler.h"
#include <algorithm>
#include <optional>
#include "System/PathUtil.h"
#include "Renderer/Renderer.h"
#include "Settings/SettingsStore.h"      // PublishSettingsSnapshot reads the store
#include "Scene/MeshAssetRegistry.h"   // m_meshAssets (SetMeshLibrary)
#include "Resource/MeshLibIdentity.h"     // DescribeLoadedMeshLibraries: IDNT_FLAG_SYNTHESIZED, MeshLibCanonicalPath

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
#include "Graphics/ShadingStencil.h"   // kNPR - the toon receivers the path tracer lights
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
#include "System/CommandLine.h"   // -phaselog_every gate for [RENDERZONE]
#include "System/Check.h"     // ENGINE_CHECK_ONCE tripwires
#include "System/TaskSystem.h"
#include "System/EventBus.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <bit>                // std::popcount - FinishFramePlan
#include <cstring>
#include <cstdlib>            // _dupenv_s / strtoull - DX12_MATBUF_CHECKSUM
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// ==== World subscription, undo restore, entity destroy ====
void Renderer::SubscribeToWorld(World* world)
{
    if (world == m_subscribedWorld) return;

    if (m_subscribedWorld && m_entityDestroyListenerId != 0)
    {
        m_subscribedWorld->RemoveEntityDestroyListener(m_entityDestroyListenerId);
        m_entityDestroyListenerId = 0;
    }

    m_subscribedWorld = world;

    if (world)
    {
        m_entityDestroyListenerId = world->AddEntityDestroyListener(
            [this](Entity e) { OnEntityDestroyed(e); });
    }
}

void Renderer::ParkEntitiesForUndo(const std::vector<Entity>& ents)
{
    if (!m_subscribedWorld) return;
    World& w = *m_subscribedWorld;
    int n = 0;
    for (Entity e : ents)
        if (w.IsAlive(e) && w.HasComponent<MeshSkinnedComponent>(e))
            { m_skin.ParkForUndo(w, e); ++n; }
    if (n) LOG_INFO("Renderer: parked %d skinned-mesh GPU slot set(s) for undo", n);
}

void Renderer::OnEntitiesRestored(const std::vector<Entity>& ents)
{
    if (!m_subscribedWorld) return;
    World& w = *m_subscribedWorld;
    int nSkinned = 0, nMat = 0;
    for (Entity e : ents)
    {
        if (!w.IsAlive(e)) continue;

        // Static meshlib mesh: force re-resolve of the cached descriptor slot
        // (the meshlib desc cache itself survives a single-entity delete, so the
        // slot re-resolves correctly on the next RegisterMeshLibMesh).
        if (MeshLibRef* ref = w.GetComponent<MeshLibRef>(e))
        {
            ref->cachedDescSlot   = RHI::kInvalidBufferIndex;
            ref->cachedGeneration = 0;
        }

        // Skinned mesh: its descriptor slots were parked (kept alive) across the delete, so the restored
        // component's meshDescriptorIdx is still valid; un-park so a later real delete frees them.
        if (w.HasComponent<MeshSkinnedComponent>(e))
            { m_skin.UnparkForUndo(e); ++nSkinned; }

        // The authored component is restored by value, but renderer-owned runtime
        // bindings were released on delete. Drop any surviving entity binding and
        // let the next material sync bind the restored paths to a shared resource.
        if (MaterialComponent* mc = w.GetComponent<MaterialComponent>(e))
        {
            ReleaseEntityMaterialRuntimeBinding(e);
            mc->SetDirty();
            ++nMat;
        }
    }
    if (nSkinned || nMat)
        LOG_INFO("Renderer: un-parked %d skinned, re-synced %d material(s) after undo",
                 nSkinned, nMat);
}

void Renderer::FreeUndoParkedEntities(const std::vector<Entity>& ents)
{
    for (Entity e : ents)
        m_skin.FreeUndoParked(m_meshMgr, e);
}

void Renderer::OnEntityDestroyed(Entity e)
{
    // While the render-owner lane has a frame in flight, the render-side half is recorded (applied at
    // App's structural sync point) instead of draining the lane first; the game-half state below runs now
    // in both paths.
    // Only a World that quarantines its ids may defer: the record is keyed by the id, and a reissued id
    // would inherit the pending release.
    const bool defer = m_deferEntityDestroys && m_subscribedWorld &&
                       m_subscribedWorld->IdQuarantineEnabled() &&
                       m_renderFrameInFlight && m_renderFrameInFlight();
    if (!defer)
    {
        m_gfx.DrainCpuFrame(static_cast<uint8_t>(Diag::DrainCause::EntityDestroy));
        // The lane is quiescent after the drain. Deferred, these writes wait for the sync point: the lane
        // reads both while it records.
        ApplyEntityDestroySharedState(e);
    }

    // RECORDED, not released here - see m_pendingBindingRelease. The render
    // half holds raw pointers into the maps this would erase from.
    m_pendingBindingRelease.push_back(e);

    // Chain physics is game-half state (the Animation phase on main).
    if (auto* cps = m_skin.GetChainPhysicsSystem())
        cps->OnEntityDestroyed(e);

    // The listener fires BEFORE pool teardown: what the render-side half needs
    // from the components is read here, in both paths.
    const MeshSkinnedComponent* skin = nullptr;
    BeamComponent* beam = nullptr;
    if (m_subscribedWorld)
    {
        skin = m_subscribedWorld->GetComponent<MeshSkinnedComponent>(e);
        if (m_beamSystem)
            if (auto* pool = m_subscribedWorld->GetPool<BeamComponent>())
                beam = pool->Get(e);
    }
    const uint32_t beamSlot = beam ? beam->beamSlot : 0xFFFFFFFFu;
    if (beam) beam->beamSlot = 0xFFFFFFFFu;

    if (!defer)
    {
        ++m_destroyDeferStats.immediate;
        ApplyEntityDestroyRenderSide(e, skin, beamSlot);
        return;
    }
    DeferredEntityDestroy rec;
    rec.e        = e;
    rec.beamSlot = beamSlot;
    if (skin) { rec.hasSkin = true; rec.skin = *skin; }
    m_deferredDestroys.push_back(std::move(rec));
    ++m_destroyDeferStats.deferred;
    m_destroyDeferStats.peak = (std::max)(m_destroyDeferStats.peak, m_deferredDestroys.size());
}

// The render-side half of an entity destroy: state the render-owner lane reads
// while it records (the emit nest's instance slots, skinned descriptor slots +
// buffers, the particle / billboard texture caches, afterimage spawns, beams).
void Renderer::ApplyEntityDestroyRenderSide(Entity e, const MeshSkinnedComponent* skin,
                                            uint32_t beamSlot)
{
    // Stable instance slots: park the entity's slot for deferred reuse (idempotent; the dirty feed's
    // removed list also routes here).
    FreeInstanceSlot(e);
    m_skin.OnEntityDestroyed(e, skin);
    if (m_particleSystem)
        m_particleSystem->OnEntityDestroyed(m_gfx, e);
    if (m_afterimageSystem)
        m_afterimageSystem->OnEntityDestroyed(e);
    if (m_billboardFXPass)
        m_billboardFXPass->OnEntityDestroyed(e);
    if (m_beamSystem && beamSlot != 0xFFFFFFFFu)
        m_beamSystem->Release(beamSlot);
}

// State the lane reads while it records, so written only with the lane quiescent: m_sceneStore.tracked
// (IsStaticLaneInstance in PlanEmit / EmitBatches) and DDGISceneAS's record list (a plain std::vector
// iterated and cleared in BuildOrRefit). Do not write it from main mid-frame: a data race, and a lost
// record lets a recycled id's skinned BLAS be refit against another mesh's topology.
void Renderer::ApplyEntityDestroySharedState(Entity e)
{
    ENGINE_CHECK_ONCE(!(m_renderFrameInFlight && m_renderFrameInFlight()),
        "[DESTROY_DEFER] shared destroy state written while a lane frame is in flight "
        "(entity %u)", static_cast<uint32_t>(e));
    // Retained-scene store accelerator: untrack (the DirtyDiff membership sweep
    // would also catch it; this keeps the removed list accurate for the next
    // scene build, which runs after the sync point either way).
    const uint32_t id = static_cast<uint32_t>(e);
    if (id < m_sceneStore.tracked.size() && m_sceneStore.tracked[id])
    {
        m_sceneStore.tracked[id] = 0;
        m_sceneStore.trackedCount--;
        m_sceneRemovedEvents.push_back(e);
    }
    // Skinned BLAS: a recycled id must not be REFIT against the old topology.
    // Record-only (applied in BuildOrRefit).
    m_ddgiSceneAS.OnEntityDestroyed(e);
}

bool Renderer::RunOrDeferToSyncPoint(std::function<void()> fn,
                                     const std::function<void()>& drain)
{
    if (!(m_deferSyncCalls && m_renderFrameInFlight && m_renderFrameInFlight()))
    {
        if (drain) drain();
        ++m_destroyDeferStats.callsImmediate;
        fn();
        return true;
    }
    m_deferredCalls.push_back({ m_deferredDestroys.size(), std::move(fn) });
    ++m_destroyDeferStats.callsDeferred;
    m_destroyDeferStats.callsPeak = (std::max)(m_destroyDeferStats.callsPeak, m_deferredCalls.size());
    return false;
}

// Runs at the structural sync point (App, right after the Render-phase barrier drained the lane) and
// from OnWorldClear (worldBoundary; the lane is drained there too). Destroy records and mailbox calls
// run in the order they were filed.
void Renderer::ApplyStructuralSyncPoint(bool worldBoundary)
{
    if (m_deferredDestroys.empty() && m_deferredCalls.empty()) return;
    // Swapped out first: the lane is quiescent here, so anything these re-enter
    // (a destroy, another request) takes the immediate path, never the lists.
    std::vector<DeferredEntityDestroy> destroys;
    std::vector<DeferredSyncCall>      calls;
    destroys.swap(m_deferredDestroys);
    calls.swap(m_deferredCalls);
    size_t c = 0;
    for (size_t i = 0; i <= destroys.size(); ++i)
    {
        for (; c < calls.size() && calls[c].beforeDestroy <= i; ++c)
        {
            calls[c].fn();
            ++m_destroyDeferStats.callsApplied;
        }
        if (i == destroys.size()) break;
        const DeferredEntityDestroy& d = destroys[i];
        // The shared half runs for every record, a resurrected id included, as the immediate path did at
        // destroy time: the untrack makes DirtyDiff report the id removed + re-added, and the DDGI record
        // forces a BLAS rebuild for whatever carries the id now.
        ApplyEntityDestroySharedState(d.e);
        // The id is quarantined until after this call, so the only way it is
        // alive again is CreateEntityWithId (editor undo) before the sync point.
        // Its renderer-side state then belongs to the resurrected entity: skip.
        // At a world boundary "alive" means nothing (the World was cleared or
        // adopted): every record is applied, as the immediate path would have.
        if (!worldBoundary && m_subscribedWorld && m_subscribedWorld->IsAlive(d.e))
        {
            ++m_destroyDeferStats.skippedAlive;
            ENGINE_CHECK_ONCE(false,
                "[DESTROY_DEFER] entity %u was recreated before its deferred destroy "
                "was applied - record skipped", static_cast<uint32_t>(d.e));
            continue;
        }
        ApplyEntityDestroyRenderSide(d.e, d.hasSkin ? &d.skin : nullptr, d.beamSlot);
        ++m_destroyDeferStats.applied;
    }
}

void Renderer::SetChainPhysicsSimulationEnabled(bool enabled)
{
    if (auto* cps = m_skin.GetChainPhysicsSystem())
        cps->SetSimulationEnabled(enabled);
}

void Renderer::SetChainPhysicsHold(bool hold)
{
    if (auto* cps = m_skin.GetChainPhysicsSystem())
        cps->SetHold(hold);
}

void Renderer::ResetChainPhysicsSimulation()
{
    if (auto* cps = m_skin.GetChainPhysicsSystem())
        cps->ClearAll();

    // Chain physics writes into AnimationSystem's LocalPose scratch. Clearing
    // particles alone is insufficient for a no-clip skeleton because the rest
    // fallback intentionally keeps its prior cache; invalidate it so the next
    // frame rehydrates the clean clip/rest pose before physics initializes.
    if (auto* anim = m_skin.GetAnimationSystem())
        anim->ClearLocalPoseCache();
}

void Renderer::SetMeshLibrary(Resource::MeshLibrary* meshLib)
{
    m_meshLib    = meshLib;
    m_meshAssets = meshLib ? std::make_unique<MeshAssetRegistry>(*meshLib, m_gfx, this) : nullptr;
}

// Caller contract: before calling, the caller must have
//   1. called IGraphicsDevice::WaitIdleAndReleaseDeferred() (all queues idle, every deferred-release slot
//      drained), and
//   2. recorded nothing into this frame yet.
// Step 1 is checked on entry (a missing sync is an ERROR, and the sync runs here as a fallback); step 2
// is not.
// What follows (MeshManager's descriptor generation bump, the DDGI BLAS cache drop, wiping
// m_runtimeMaterialResources, mesh-library releases) frees or invalidates objects an open command list may
// still reference, and a D3D12 command list holds no refcount. Every caller (SceneManager::ActivateScene,
// EditorLayer's load / new / preview-harness scenes / bake-preview restore, PlayModeSession::ExitPlay)
// sits in that slot. Do not call this from an ImGui handler: a use-after-free that usually appears to
// work.
void Renderer::OnWorldClear(bool idsPreserved, bool worldSurvives)
{
    m_gfx.DrainCpuFrame(static_cast<uint8_t>(Diag::DrainCause::WorldClear));
    // Contract step 1, checked: the hard-sync floor (GetRetiredFrame) must cover every frame submitted
    // so far - the frame being recorded excepted inside one, as WaitForPreviousFrame raises it. A
    // caller that skipped the sync is reported and synced here, before anything below is released.
    {
        const uint64_t stamp = m_gfx.GetReleaseStampFrame();
        const uint64_t need  = m_gfx.IsRecordingFrame() ? stamp - 1 : stamp;
        if (!ENGINE_CHECK(m_gfx.GetRetiredFrame() >= need,
                          "Renderer::OnWorldClear: no hard GPU sync since the last submitted frame: "
                          "retired=%llu needs=%llu - syncing here; call WaitIdleAndReleaseDeferred first",
                          static_cast<unsigned long long>(m_gfx.GetRetiredFrame()),
                          static_cast<unsigned long long>(need)))
            m_gfx.WaitIdleAndReleaseDeferred();
    }
    // Every clear counts, the world-survives one too: a preload's GPU half (shared skinned sources, the
    // world's mesh libraries) does not outlive it.
    ++m_worldClearCount;
    // The registry's handles are world libraries parked below; its views name
    // them, so they go with the world (the epoch change would drop them anyway).
    if (m_meshAssets) m_meshAssets->Clear();
    // -brssplit: the static lanes hold m_candCache and raw MaterialComponent pointers into the world being
    // torn down. Nothing below may run while one is in flight: the cache clear further down would free
    // the storage under them. Idempotent and free when the flag is off.
    JoinStaticSceneEarly();
    m_spotCasterState.clear();
    m_spotCasterChanges.clear();
    m_spotStaticCasterChanges.clear();
    m_spotForceRefresh = true;
    m_spotStaticForceRefresh = true;
    // Deferred destroy work names entity IDs from the world being torn down.
    // World::Clear fires no destroy listeners, so nothing else would drop these,
    // and after a reload the same IDs belong to different entities.
    m_pendingBindingRelease.clear();
    m_spawnMaterialResolve.clear();   // the queue names the old world's entities
    // The render-side destroy records are not dropped the same way: the immediate path released them
    // before this clear, and an idsPreserved clear (Play / Stop) keeps per-entity state keyed by surviving
    // ids. Apply them first, while every subsystem is intact, together with any mailbox call filed before
    // them, in order.
    ApplyStructuralSyncPoint(/*worldBoundary=*/true);

    // Descriptor occupancy at every world boundary: tells "descriptors leak across reloads" from "one
    // world costs more than half the heap"; without it the only symptom is a driver fault on the second
    // load of a heavy scene.
    {
        uint32_t cu = 0, cc = 0, gu = 0, gc = 0;
        m_gfx.GetCbvSrvUavStaticUsage(cu, cc, gu, gc);
        LOG_INFO("Renderer::OnWorldClear(idsPreserved=%d worldSurvives=%d): "
                 "CBV_SRV_UAV static cpu %u/%u, gpu %u/%u",
                 idsPreserved ? 1 : 0, worldSurvives ? 1 : 0, cu, cc, gu, gc);
    }

    // BeginFrame re-subscribes to whichever World binds next.
    SubscribeToWorld(nullptr);

    // A world clear invalidates the mesh descriptor heap's world-scoped bindless buffer slots: drop the
    // cached material-table and exposure-buffer slots so the RTR wiring re-registers them when next needed.
    m_materialBufGpuBindless = RHI::kInvalidBufferIndex;
    m_exposureBufBindless    = RHI::kInvalidBufferIndex;

    // Drop transient gameplay post-process overrides: per-session state that must not bleed across a
    // world reload.
    PostProcess::Runtime::Get().ClearOverrides();

    m_skin.OnWorldClear(idsPreserved, worldSurvives);
    // Every pass and system that registered OnWorldClear where Compile created
    // it (RenderLifecycle.h): the SHaRC and RTXDI world caches, the VFX
    // texture caches and pools, the per-world lazy residents (stars, spot /
    // point atlases), the ripple slots, the light-history keys. Flags, or
    // releases of what each owns.
    m_lifecycle.RunWorldClear({ idsPreserved, worldSurvives });
    // Then the main view's WorldClear, with the frame being built (the render half of the last one has
    // drained): every history, the exposure snap, the Hi-Z capture.
    RaiseViewBreak(HistoryReset::WorldClear);
    // The emit path's previous world matrices (velocity) belong to the old world: an id recycled - or
    // kept, Play / Stop - would take the other world's last pose as its motion. The next emit uses the
    // current matrix (one zero-velocity frame), on the static and the dynamic lane alike.
    std::fill(m_prevWorldValid.begin(), m_prevWorldValid.end(), static_cast<uint8_t>(0));
    // The preview's mesh-override libraries: release them before m_meshMgr.OnWorldClear bumps the
    // descriptor generation that invalidates their slots (hand back the refcount, then let the cache that
    // named it go). Dropping the map without Release would pin every .meshlib ever dragged onto the panel
    // for the life of the process.
    // No DetachMeshLib here: m_meshMgr.OnWorldClear clears the whole descriptor cache and bindless table
    // and bumps the generation, more than ReleasePreviewMeshLib's per-library hand-back; doing both would
    // free the same slots twice.
    if (m_meshLib)
        for (auto& [path, e] : m_previewMeshLibs)
            if (e.handle.IsValid()) m_meshLib->Release(e.handle, m_gfx);
    m_previewMeshLibs.clear();
    m_previewMeshLibFailLogged.clear();
    // The deferred hand-backs are dropped, not drained: they hold slot indices into the tables
    // m_meshMgr.OnWorldClear resets (m_meshFreeList / m_bufferFreeList and the pending buckets), so
    // freeing them now would push indices into buckets wiped a moment later, and keeping them would hand
    // the next world's allocator this world's slots.
    m_previewMeshLibHandbacks.clear();
    // Per-asset retry clocks name paths that will be re-composed from scratch.
    m_previewAssetRetry.clear();
    // And the compositions that named those libraries: parsed file data with no GPU reference, but every
    // mesh-descriptor slot they resolve to is invalidated by m_meshMgr.OnWorldClear; kept, the first frame
    // after a reload would resolve against a dead generation.
    m_previewAssetPlacements.clear();
    m_previewUnfittedAssets.clear();
    m_previewAssetFits.clear();
    ++m_previewAssetMemoEpoch;

    // ---- Camera-preview identities die with the world ----
    // Entity is a bare uint32 id with no generation (World::IsAlive checks only m_alive[e]; EntityHandle has
    // one, Entity does not). A PiP or camera-preview view left on entity 412 across a reload would silently
    // follow whatever 412 is now: ExtractPreviewCameras validates only IsAlive and a camera component.
    // ClearGamePiP's semantics, inline: the pending queue is drained too (a Set queued this frame names an
    // old-world id), and the release goes through ReleasePreviewView so the HDR slot and the WorldCull lane
    // are handed back. Not ClearGamePiP() itself: it only queues, and nothing applies the queue before the
    // next frame's classify.
    // Skipped when the world survives (EditorLayer's collision-baker preview restore): no entity was
    // destroyed, and running this would cancel a Lua / harness SetGamePiP with nothing to re-arm it (the
    // camera panels re-push their selection every Draw; the PiP does not).
    // The asset / composition caches above are not gated: they are keyed by the mesh-descriptor
    // generation, which m_meshMgr.OnWorldClear bumps on this path too.
    if (!worldSurvives)
    {
    m_pipPending = PipPending{};
    // The layer's one-shot refusal diagnostics, per-view "size refused" flags and scheduler priority belong
    // to a tenancy, and a world boundary ends every tenancy. The order against the PiP release below does
    // not matter for the latches (both only write false); this runs first so the log shows the layer-wide
    // reset before the per-view teardown. Nothing depends on that order; do not add something that does
    // without saying so here.
    if (m_preview) m_preview->NoteWorldClear();
    if (m_pipView != Preview::kInvalidView)
    {
        SetPreviewCameraEntity(m_pipView, NullEntity);
        ReleasePreviewView(m_pipView);
        LOG_INFO("[PREVIEW] world clear: game PiP released (view %u, entity %u) - "
                 "entity ids are recycled across a reload, so the view is dropped "
                 "rather than re-pointed", m_pipView,
                 static_cast<unsigned>(m_pipEntity));
        m_pipView = Preview::kInvalidView;
    }
    m_pipEntity = NullEntity;
    m_pipW = m_pipH = 0;
    m_pipRect[0] = m_pipRect[1] = m_pipRect[2] = m_pipRect[3] = 0;
    // Every view's camera identity, not just the PiP's: CameraPreviewWindow pushes its selection every
    // frame, but until its next Draw the resolver would run with the old id. It re-resolves by name after
    // a reload through the World::Epoch() poll at the top of CameraPreviewWindow::Draw.
    for (Entity& e : m_previewCamEntity) e = NullEntity;
    m_secondaryFrusta.clear();
    m_secondaryCullActive = false;
    m_secondaryCands.clear();
    // The probe volume and the probes chosen this frame name the old world's entities; the next
    // BuildScene_SelectProbes rebuilds them.
    m_probeCullActive = false;
    m_probeSelected.clear();
    m_probeSelectRan = false;
    // The one-shot cull diagnostic describes ONE world; a reloaded scene with a
    // new BVH deserves its own line.
    m_previewCullLogged = false;
    // The other two one-shots: a world boundary is a new session, so the first refusal after it must
    // speak again.
    //   m_previewNoFrustumLogged      - names a view that contributed no frustum; it self-heals only when
    //                                   that view contributes one, so the same breakage in the next world
    //                                   would be silent.
    //   m_previewSecondaryFrustaLogged - seeded 0xFFFFFFFF so the first extract always speaks; it compares
    //                                   against the previous count, which would be the old world's.
    for (bool& b : m_previewNoFrustumLogged) b = false;
    m_previewSecondaryFrustaLogged = 0xFFFFFFFFu;
    }   // !worldSurvives
    m_meshMgr.OnWorldClear();  // clears per-library caches + bumps generation
    // The skinned vertex ring's bindless slots survive: registered via
    // MeshDescriptorHeap::RegisterPersistentBuffer at Init.
    // The DDGI BLAS cache is keyed on MeshLibRef and stale after teardown: drop it to force a rebuild.
    // m_gfx is passed so the BLAS result buffers, still referenced by this frame's already-recorded DDGI
    // command list, are deferred-released, not freed inline.
    m_ddgiSceneAS.OnWorldClear(m_gfx, idsPreserved);
    // Surface-cache pages are keyed by (meshDescriptorIndex, materialIndex), both recycled heap slots, so a
    // surviving key would hand the new world's mesh the old world's baked texels. Drops the mapping only:
    // the atlas textures are engine-owned and keep their bindless slots (the proxy stores those indices
    // per instance).
    m_surfaceCache.OnWorldClear();

    // World::Clear() fires no per-entity destroy listeners, so OnEntityDestroyed's Release path is bypassed
    // on reload. Refs must still drop, but an inline Release breaks the same-path cache hit:
    //   inline Release -> refcount 0 -> FreeSlot wipes the path mapping -> the next world's Acquire of
    //   the same path gets a fresh slot and a full GPU re-upload.
    // So the handles go to a deferred-release queue: the next BeginFrame runs SyncMaterialTextures before
    // FlushPendingMatTexReleases, so same-path Acquires bump the refcount 1 -> 2 and the deferred Release
    // drops it back to 1. Shared textures keep their GPU upload across reloads (~800 in Bistro).
    if (m_texSys)
    {
        // One deferred handle per shared runtime-entry texture, not per entity.
        // TextureSystem receives exactly one Acquire/Release for each entry.
        for (auto& [_key, entry] : m_runtimeMaterialResources)
        {
            for (auto& texture : entry.engineTextures)
                if (texture.handle != Resource::kInvalidTextureHandle)
                    m_pendingMatTexReleaseAfterNextSync.push_back(texture.handle);
            for (auto& [_name, texture] : entry.customTextures)
                if (texture.handle != Resource::kInvalidTextureHandle)
                    m_pendingMatTexReleaseAfterNextSync.push_back(texture.handle);
        }

        // Water flow-normal maps: the same Load-then-Release deferral, so a same-path reload keeps the GPU
        // upload warm (per body, keyed on entity).
        auto deferWaterSlot = [&](TerrainTexSlot& s) {
            if (s.handle != Resource::kInvalidTextureHandle)
                m_pendingMatTexReleaseAfterNextSync.push_back(s.handle);
            s = {};
        };
        for (auto& [_e, slots] : m_waterTexCache)
        {
            deferWaterSlot(slots.a);
            deferWaterSlot(slots.b);
        }
        m_waterTexCache.clear();
    }
    m_runtimeMaterialResources.clear();
    // Every outstanding by-key reference is now a phantom: the clear is refcount-blind and every texture
    // was deferred for release, but a holder outside the entity system (the material preview) still
    // believes it owns a reference. A missing key is not enough for it to notice:
    // BuildScene_SyncMaterials re-creates entries for the new world before the holder looks again, so a
    // key the new world also uses is present again, and the holder would release a reference it never
    // re-took, dropping TextureSystem handles under live entities. The epoch makes that case observable
    // (Renderer::MaterialRuntimeEpoch()).
    ++m_matRuntimeEpoch;
    m_entityMaterialRuntimeBindings.clear();
    m_matBoundBits.clear();   // mirror of the bindings map (Renderer.h)
    // Retained material slots key on (matHash, textures, shader ids) from the old world; those keys are
    // meaningless now and their entity ids are about to be recycled. Rebuild the slot map on the next
    // SortAndEmit.
    m_persistentMatSlotsReset = true;

    // MeshLibrary::Load() does not dedupe by path, so a different world allocates fresh VB+IB buffers per
    // .meshlib while the old world's stay resident. Release with one world's lag: the libraries queued at
    // the previous OnWorldClear, not the ones queued now.
    // Do not release inline, even after WaitIdleAndReleaseDeferred: a full multi-queue sync still produced
    // a delayed TDR ~5 s after reload (suspected: stale descriptors in MeshDescriptorHeap's bindless SRV
    // table, still reached by something such as DXR BLAS prefetch until a later RegisterBuffer overwrites
    // them). One world of lag outlives any in-flight pipeline.
    if (m_meshLib)
        for (Resource::Handle h : m_meshLibsPendingRelease)
            m_meshLib->Release(h, m_gfx);
    m_meshLibsPendingRelease = std::move(m_worldMeshLibs);
    m_worldMeshLibs.clear();

    // Prune DDGI BLAS entries whose meshlib slot just reached refCount 0. Without this A -> B -> A cycles
    // leak ~200 MB per cycle: m_blasCache's ComPtr keeps the BLAS resident after its source VB+IB went away.
    m_ddgiSceneAS.PruneStaleBLAS(m_meshLib, m_gfx);

    // Release decal-material textures so TextureSystem refcounts balance across reloads.
    if (m_texSys)
        m_decalMaterialLibrary.Shutdown(*m_texSys, m_gfx);
    // Cached entity IDs would collide with World::Clear's free-list recycling.
    m_decalSpawner.OnWorldClear();
    // BOTH buffers: the render half may still be pointed at the other one, and
    // a world teardown must not leave it holding packets for entities that no
    // longer exist.
    m_drawPacketsBuf[0].clear();
    m_drawPacketsBuf[1].clear();
    // BOTH buffers on a world clear: the render half may still be pointed at
    // the other one, holding jobs for entities that no longer exist.
    m_skin.SkinJobsWrite().clear();
    m_skin.PublishSkinJobs();
    m_skin.SkinJobsWrite().clear();
    // Force a full SAH rebuild of the cull BVH next frame. The BVH caches the previous world's entity ids
    // (m_leafEntities) and topology, and World::Clear recycles those ids onto different geometry.
    // BuildScene_CullBVH otherwise rebuilds only when GetLeafCount() != entityCount, so two worlds with the
    // same leaf count would take the refit path against the stale tree: RefitLeaf updates m_leafAABBs but
    // not m_leafEntities, so FrustumCull returns the old world's ids and the new world's entities are
    // tested-but-not-visible, silently culled.
    m_sceneBVH.MarkRebuild();
    m_lastWorld = nullptr;
    ResetChainPhysicsSimulation();
    // Depth content dies with the world: the view's WorldClear raised earlier zeroes the Hi-Z capture at
    // the next render frame's start, so the old world's occluders never test the new world's draws.

    // Retained-scene store: entity ids recycle in the new world, so a full reset.
    m_sceneStore.tracked.assign(m_sceneStore.tracked.size(), 0);
    m_sceneStore.trackedCount  = 0;
    m_sceneStore.fullInvalidate = true;
    m_sceneRemovedEvents.clear();
    m_sceneMatDirty.clear();
    // Stable instance slots: the per-entity slot map holds the old world's recycled ids. Rebuild
    // everything at the next SortAndEmit (safe mid-frame: in-flight frames' draws execute before the
    // re-upload copies on the same queue).
    m_instanceRetainReset = true;
    // Candidate cache: cached mc pointers + entity ids die with the world.
    m_candCacheValid = false;
    m_candCache.clear();
    m_candEntityToIdx.clear();
    m_candCachePrimCount = 0;
    m_dynCandCacheValid = false;   // the dynamic lane's cache: same reason
    m_dynCandCache.clear();
    // Retained BVH leaves + tag sets: leaf indices and set membership hold this world's entity ids. The
    // gates would catch the reload too (fullInvalidate -> dirtyOther storm; MarkRebuild pins the full BVH
    // path); the explicit reset keeps the invariant local rather than dependent on gate order.
    m_bvhRetainedValid  = false;
    m_bvhSkippedRows    = 0;
    m_bvhWorldAabbCount = 0;
    m_bvhEntityToLeaf.clear();
    m_bvhDynamicLeaves.clear();
    m_tagSetsRetainedValid = false;
    m_playerSetSeedSig = m_hairSetSeedSig = m_faceOverlaySeedSig = 0;
    m_playerShadowSet.clear();
    m_hairShadowSet.clear();
    m_faceOverlayParams.clear();

    LOG_INFO("Renderer::OnWorldClear — all per-entity caches invalidated");
}

// ==== Editor Play snapshot: mesh-library ownership, world-scoped descriptor restore ====
void Renderer::CaptureEditorWorldResources(const World& world)
{
    // From here until RestoreEditorWorldResources the Play snapshot holds value-copies of every skinned
    // component: their GPU buffer handles must never reach the destruction queue (a mid-play Scene.Load
    // runs the idsPreserved=false sweep). Set before any early-out.
    m_skin.SetPlaySnapshotActive(true);

    if (!m_meshLib) return;

    // Defensive recovery for an interrupted Play session. A normal Stop moves
    // these references into m_worldMeshLibs, so this vector starts empty.
    for (Resource::Handle h : m_editorPinnedMeshLibs)
        m_meshLib->Release(h, m_gfx);
    m_editorPinnedMeshLibs.clear();

    std::unordered_set<uint32_t> seen;
    auto pin = [&](Resource::Handle h) {
        if (!h.IsValid() || !seen.insert(h.packed).second)
            return;
        if (m_meshLib->Retain(h))
            m_editorPinnedMeshLibs.push_back(h);
        else
            LOG_WARNING("Renderer: cannot pin stale Editor MeshLibrary handle 0x%08X", h.packed);
    };

    // The ECS pool is the source of truth for what the snapshot will need.
    // Include tracked ownership as a fallback for library-backed renderer data
    // that has no surviving MeshLibRef entity.
    if (const auto* refs = world.GetPool<MeshLibRef>())
        for (const MeshLibRef& ref : refs->Data())
            pin(ref.libHandle);
    for (Resource::Handle h : m_worldMeshLibs)
        pin(h);

    LOG_INFO("Renderer: pinned %zu Editor MeshLibraries for Play snapshot",
             m_editorPinnedMeshLibs.size());
}

void Renderer::RestoreEditorWorldResources(World& world)
{
    // OnWorldClear immediately before AdoptState moved the outgoing Play
    // world's ownership into m_meshLibsPendingRelease. The retained snapshot
    // references now become the restored world's regular ownership; a later
    // scene clear will retire them through the existing one-world-lag path.
    m_worldMeshLibs.insert(m_worldMeshLibs.end(),
                           m_editorPinnedMeshLibs.begin(),
                           m_editorPinnedMeshLibs.end());
    const size_t restoredLibCount = m_editorPinnedMeshLibs.size();
    m_editorPinnedMeshLibs.clear();

    // Force lazy static-mesh resolution even if a very long session ever wraps
    // the MeshManager generation counter back to a component's cached value.
    if (auto* refs = world.GetPool<MeshLibRef>())
        for (MeshLibRef& ref : refs->Data())
        {
            ref.cachedDescSlot = RHI::kInvalidBufferIndex;
            ref.cachedGeneration = 0;
        }

    const uint32_t rebound = m_skin.RebindRestoredWorldDescriptors(world, m_meshMgr);
    // Snapshot handed back to the world; the by-reference reconcile above has
    // already decided every tracked handle's fate.
    m_skin.SetPlaySnapshotActive(false);
    LOG_INFO("Renderer: restored Editor world resources (%zu MeshLibraries, %u skinned descriptors)",
             restoredLibCount, rebound);
}

std::vector<Renderer::LoadedMeshLibInfo> Renderer::DescribeLoadedMeshLibraries(World* world) const
{
    std::vector<LoadedMeshLibInfo> out;
    if (!m_meshLib) return out;
    auto same = [](Resource::Handle a, Resource::Handle b) { return a.packed == b.packed; };
    auto count = [&](const std::vector<Resource::Handle>& v, Resource::Handle h)
    {
        uint32_t n = 0;
        for (Resource::Handle x : v) n += same(x, h) ? 1u : 0u;
        return n;
    };
    // Users: one walk of the MeshLibRef pool, keyed by the whole handle.
    std::unordered_map<uint32_t, uint32_t> usersBySlot;
    if (world)
        if (auto* refs = world->GetPool<MeshLibRef>())
        {
            const auto& ents = refs->Entities();
            const auto& data = refs->Data();
            for (size_t i = 0; i < data.size(); ++i)
                if (data[i].libHandle.IsValid() && world->IsAlive(ents[i]))
                    ++usersBySlot[data[i].libHandle.packed];
        }
    std::unordered_set<uint32_t> previewHeld;
    for (const auto& [key, e] : m_previewMeshLibs)
        if (e.handle.IsValid()) previewHeld.insert(e.handle.packed);

    for (const Resource::Handle h : m_meshLib->GetLoadedHandles())
    {
        LoadedMeshLibInfo r;
        r.handle   = h;
        // The dedup key's spelling, not whichever spelling reached Load first:
        // one library can stand for several (a prefab's `../`).
        r.path     = Resource::MeshLibCanonicalPath(m_meshLib->GetSourcePath(h));
        r.refs     = m_meshLib->RefCount(h);
        r.entries  = m_meshLib->GetMeshCount(h);
        const auto totals = m_meshLib->GetTotals(h);
        r.vertices = totals.vertexCount;
        r.indices  = totals.indexCount;
        r.stride   = m_meshLib->GetVertexStride(h);
        r.vramBytes = uint64_t(r.vertices) * r.stride + uint64_t(r.indices) * 4ull;
        if (const auto id = m_meshLib->GetIdentity(h))
        {
            r.meshes   = static_cast<uint32_t>(id->meshes.size());
            r.identity = (id->flags & Resource::IDNT_FLAG_SYNTHESIZED) ? "synthesized" : "file";
        }
        {
            const Resource::MeshLibrary::MeshletLibraryInfo mi = m_meshLib->GetMeshletInfo(h);
            r.meshlets         = Resource::MeshLibrary::MeshletStateName(mi.state);
            r.meshletClusters  = mi.clusters;
            r.meshletLevels    = mi.levelRecords;
            r.meshletMaxLevels = mi.maxLevels;
            r.meshletBytes     = mi.gpuBytes;
        }
        r.worldRefs      = count(m_worldMeshLibs, h);
        r.editorPinned   = count(m_editorPinnedMeshLibs, h) != 0;
        r.pendingRelease = count(m_meshLibsPendingRelease, h) != 0;
        r.preview        = previewHeld.count(h.packed) != 0;
        if (auto it = usersBySlot.find(h.packed); it != usersBySlot.end()) r.users = it->second;
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const LoadedMeshLibInfo& a, const LoadedMeshLibInfo& b) { return a.path < b.path; });
    return out;
}
