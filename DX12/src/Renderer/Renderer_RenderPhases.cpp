// Renderer_RenderPhases.cpp - the phases RenderAsync calls as functions:
// Render_BindFrameResources, Render_ComputePrepass and the shadow family's
// GPU cull (RecordShadowFamilyCull). RenderAsync's other phases stay inline
// (Renderer_RenderAsync.cpp): extracting them would need a context struct for
// its locals.

#include "Diagnostics/Profiler.h"
#include <optional>
#include "System/PathUtil.h"
#include "Renderer/Renderer.h"
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
#include "Graphics/ShadingStencil.h"   // kNPR: the toon receivers the path tracer lights
#include "ECS/BeamComponent.h"
#include "ECS/VFXSpawnRequests.h"   // unified VFX Lane-B mailboxes (Tracer/Decal/Afterimage/Mesh)
#include "ECS/HierarchyComponents.h"
#include "ECS/FaceShadingSystem.h"

// resource system
#include "Resource/MaterialSerializer.h"

// engine systems
#include "System/Log.h"
#include "System/ThreadContext.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"   // -restir_reset_frame, -showoff, -skinrecord* flags
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
#include <bit>
#include <cstring>
#include <cstdlib>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// Binds the frame's constant buffers, buffers, draw list and main view, and
// the passes' per-frame shadow / cluster / fog inputs. Runs before anything of
// the frame records.
void Renderer::Render_BindFrameResources(uint32_t frameSlot)
{
    // The render frame starts in the main view: the frame's breaks - the reasons
    // raised while the game half built it (FrameView::viewBreaks), the camera's cut
    // and the render half's own below - are applied once, before anything of the
    // frame records (ViewState::BeginFrame), and kept as its FrameBreaks: XeGTAO
    // and the fog read them where they record, the Hi-Z gate and the ReSTIR /
    // path-trace block read them; SSGI's and the RT shadow / AO denoisers'
    // histories reset here.
    uint32_t breaks = FrameViewRead().viewBreaks;
    uint32_t loud   = FrameViewRead().viewBreaksLoud;
    if (!FrameViewRead().camera.historyValid)
        breaks |= HistoryReset::CameraCut;
    // The render half's own reasons are OR-ed into this frame's breaks directly
    // (raised through the usual path, they would wait for the next hand-off): a
    // render-path change - SetRenderPath, the budget ladder and the backend switch at
    // the top of RenderAsync - is a PathChange (it resets the histories that depend on
    // the lighting path, not the ones every content cut resets; the ladder changes
    // path mid-play) ...
    if (m_renderPathChanged.exchange(false, std::memory_order_acquire))
    {
        breaks |= HistoryReset::PathChange;
        loud   |= HistoryReset::PathChange;
    }
    // ... and -restir_reset_frame=N (diagnostic) is a HardCut: every temporal
    // history restarts at render frame N, so a capture at N+k depends on frames
    // N..N+k only, not on when each pass's pipelines and resources came up
    // (tools/restir_identity.py: bit-identity across a refactor). DLSS hears these
    // a frame late: its constants were uploaded by this frame's game half
    // (DLSSPass keeps the reset for the next frame's).
    static const int s_resetFrame = Sys::CommandLine::Get().GetInt("-restir_reset_frame", -1);
    if (s_resetFrame >= 0 && FrameViewRead().currentFrame == static_cast<uint64_t>(s_resetFrame))
    {
        breaks |= HistoryReset::HardCut;
        loud   |= HistoryReset::HardCut;
    }
    m_mainView.BeginFrame(FrameViewRead().currentFrame, breaks, loud);
    BindViewInputs(frameSlot, DrawPacketsRead(), "main", m_mainView, /*primary=*/true, RG::kShowAll);
}

// The view's half of the frame's bindings: what the graph and the passes read for the view being
// recorded - its CBs (the recording view's rings), its draw list and counts, its SceneView - for the
// main view (Render_BindFrameResources) and a secondary one (Render_BindViewResources) alike.
void Renderer::BindViewInputs(uint32_t frameSlot, const std::vector<DrawPacket>& drawList, const char* name,
                              ViewState& state, bool primary, uint32_t show)
{
    m_graph.BindConstantBuffer("PerView",     m_perObjectCB.CurrentBuffer(m_gfx));
    m_graph.BindConstantBuffer("LightCB",     m_lightCB.CurrentBuffer(m_gfx));
    // Written later this frame (WriteFogApplyCB in RenderAsync), before the graph
    // records.
    m_graph.BindConstantBuffer("FogApply",    m_fogApplyCB.CurrentBuffer(m_gfx));
    if (m_terrainCB.IsValid())
        m_graph.BindConstantBuffer("TerrainParams", m_terrainCB.CurrentBuffer(m_gfx));
    if (m_skyboxPass)
        m_graph.BindConstantBuffer("SkyCB", m_skyboxPass->GetSkyCB(m_gfx));
    // VolumetricFog's apply CB, the clouds', the height fog's, the editor grid's
    // and the grass's are blocks of the frame's constants that each pass writes
    // and binds itself.
    // Meshlet path: this frame's MeshletCullCB, and the SceneEmit latch the
    // GBuffer pass records with (render half - the latch is the read buffer's).
    if (primary && m_meshletCullCB.IsValid())
    {
        WriteMeshletCullCB();
        m_graph.BindConstantBuffer("MeshletCull", m_meshletCullCB.CurrentBuffer(m_gfx));
    }
    if (m_gbufferPass)
    {
        m_gbufferPass->SetMeshletFrame(primary && SceneEmitRead().meshletFrameActive,
                                       static_cast<uint32_t>(FrameViewRead().currentFrame));
        m_gbufferPass->SetMeshletDebugView(IsMeshletDebugView(m_viewMode.load(std::memory_order_relaxed)));
    }
    // WaterPass binds a per-body 256B CBV at b2 itself (BindConstantBufferAtOffset),
    // so WaterParams is not a graph CB.
    // Retained mode draws from the single DEFAULT-heap buffer (stable slots +
    // per-frame remap); otherwise this frame's UPLOAD slot is bound.
    m_graph.BindBuffer("InstanceBuffer",      RetainedInstancesActive()
                                                  ? m_instanceBufferGPU
                                                  : m_instanceBuffer[frameSlot]);
    m_graph.BindBuffer("MeshDescriptors",     m_meshMgr.GetDescriptorHeap().CurrentMeshDescBuffer());
    // Material table: the single DEFAULT-heap buffer (dirty-blitted from the
    // UPLOAD staging twins). Every graph consumer of "MaterialBuffer" -
    // GBufferPass, TransparentPass, FaceOverlayPass - reads it per pixel, so it
    // must not live on the UPLOAD heap.
    m_graph.BindBuffer("MaterialBuffer",      MaterialSRVBuffer(frameSlot));

    // ShadowPass is a standalone pass outside the graph, so the per-frame terrain
    // CB pointer is rebound into it every frame.
    if (m_shadowPass && m_terrainCB.IsValid())
        m_shadowPass->SetTerrainParamsCB(&m_terrainCB.CurrentBuffer(m_gfx));
    m_graph.SetBindlessTableHandle(m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());
    m_graph.SetDrawList(&drawList);

    // Draw counts, pushed here because the packet set is final and a render-graph
    // Execute() runs on a worker that must not query the Renderer. The water count
    // drives WaterSurfacePass's IsActive() and, through HasWater(), whether the
    // screen-space SSR chain is promoted this frame: water stamps the GBuffer after
    // the unified reflection pass has run, so water pixels still need it. The
    // front-layer count is the predicate the pass's own draw loop applies
    // (IsFrontLayerPacket), so IsActive() and "drew something" cannot disagree.
    // One walk for every counter: a loop each would add full passes over
    // DrawPacketsRead() every frame.
    // Custom / Transparent are upper bounds: RenderContext::GetDrawList hands out
    // only the first contiguous run of a filter, while this counts every packet
    // carrying it. An over-count leaves a pass active with an empty list; an
    // under-count (the dangerous direction) is impossible.
    {
        uint32_t waterDraws = 0, frontDraws = 0, customDraws = 0, transparentDraws = 0, nprDraws = 0;
        for (const DrawPacket& dp : drawList)
        {
            if (dp.filter == DrawFilter::Water)       ++waterDraws;
            if (dp.filter == DrawFilter::Custom)      ++customDraws;
            if (dp.filter == DrawFilter::Transparent) ++transparentDraws;
            if (TranslucentFrontLayerPass::IsFrontLayerPacket(dp)) ++frontDraws;
            // Not the baking probe's copies: they are drawn by the bake alone and must not
            // switch on a main-view reader.
            if (dp.stencilRef == ShadingStencil::kNPR && dp.renderInMainPass &&
                dp.filter != DrawFilter::ProbeView) ++nprDraws;
        }
        // Toon receivers on screen: the path tracer lights them (RecordNprLighting).
        if (primary) m_nprDrawCount = nprDraws;
        if (m_waterSurfacePass)   m_waterSurfacePass->SetWaterDrawCount(waterDraws);
        if (m_frontLayerPass)     m_frontLayerPass->SetDrawCount(frontDraws);
        if (m_outlinePass)        m_outlinePass->SetCustomDrawCount(int32_t(customDraws));
        if (m_outlineGeometryPass) m_outlineGeometryPass->SetCustomDrawCount(int32_t(customDraws));
        if (m_transparentPass)    m_transparentPass->SetTransparentDrawCount(int32_t(transparentDraws));
    }

    // Global view-mode application (Lit / Unlit / Wireframe). Deferred geometry
    // colour is handled in Lighting.ps via LightCB.viewMode, transparent in its
    // forward shader. Here only the CPU-side state Wireframe needs:
    // FILL_MODE_WIREFRAME on the geometry passes, and the background-filling
    // passes hidden so the wire reads as flat lines on a dark background. Set
    // every frame, so leaving Wireframe needs no transition logic.
    {
        const bool wire = (m_viewMode == ViewMode::Wireframe);
        // Ortho editor views hide the perspective-only composites: the skybox
        // VS w=0 trick divides by zero under ortho, and cloud/fog raymarch
        // from a single cameraPos origin (wrong ray set for parallel views).
        //
        // Lighting-debug views hide them too, for a different reason: those
        // passes write into HdrSceneColor AFTER LightingPass, so a sky or a fog
        // layer would paint over the isolated bucket and make a leak test read
        // as "there is light here" when the lighting pass output was black.
        const bool hideEnv = wire || FrameViewRead().camera.orthographic
                          || IsLightingDebugView(m_viewMode)
                          || IsGBufferDebugView(m_viewMode);
        if (m_gbufferPass)     m_gbufferPass->SetWireframe(wire);
        if (m_terrainPass)     m_terrainPass->SetWireframe(wire);
        if (m_grassPass)       m_grassPass->SetWireframe(wire);
        if (m_transparentPass) m_transparentPass->SetWireframe(wire);
        // The DEBUG_SHADE permutation: this frame shows or captures a debug target the
        // material pixel shaders count into.
        const bool debugShade = FrameViewRead().debugShade;
        if (m_gbufferPass)        m_gbufferPass->SetDebugShade(debugShade);
        if (m_transparentPass)    m_transparentPass->SetDebugShade(debugShade);
        if (m_particleRenderPass) m_particleRenderPass->SetDebugShade(debugShade);
        if (m_waterSurfacePass)   m_waterSurfacePass->SetDebugShade(debugShade);
        // Mesh-driven water is real geometry, so it gets a wireframe PSO like the
        // other geometry passes rather than being suppressed.
        if (m_waterSurfacePass) m_waterSurfacePass->SetWireframe(wire);

        // What the main view shows (RenderGraph/SceneView.h). The sky, the clouds and
        // both fogs (the height fog is a render feature's pass) hide with the
        // environment, the planar water in wireframe; every pass that declares one of
        // the flags reads Shown() from here on. The path tracer's camera primary hides
        // the environment again once it is known (in RenderAsync).
        RG::SceneView view;
        view.name         = name;
        view.viewMode     = static_cast<uint8_t>(m_viewMode.load());
        view.orthographic = FrameViewRead().camera.orthographic;
        view.width        = m_gfx.GetRenderWidth();
        view.height       = m_gfx.GetRenderHeight();
        view.state        = &state;
        // Where it looks - the frame's frustum, the camera, the MainCamera bit.
        // Descriptive only: the main view's CPU coarse cull is BuildScene_CullBVH on
        // the game half, its per-instance cull the GPU's.
        CullPlanesFrom(FrameViewRead().view.frustum, view.cull.volume);
        {
            const DirectX::XMFLOAT3& eye = FrameViewRead().view.cameraPosition;
            const DirectX::XMFLOAT3& fwd = FrameViewRead().view.cameraForward;
            view.cull.eye[0] = eye.x; view.cull.eye[1] = eye.y; view.cull.eye[2] = eye.z;
            view.cull.forward[0] = fwd.x; view.cull.forward[1] = fwd.y; view.cull.forward[2] = fwd.z;
        }
        view.cull.viewBit = ViewBit::MainCamera;
        if (hideEnv) view.show.HideAll(RG::kShowEnvironment);
        if (wire)    view.show.Hide(RG::ShowFlag::Water);
        // A secondary view shows what its owner asked for (SecondaryFrameView::show); the main view hides
        // the editor's overlays while it is the Game View.
        if (!primary) view.show.HideAll(RG::kShowAll & ~show);
        if (primary && !FrameViewRead().editorOverlays) view.show.Hide(RG::ShowFlag::EditorOverlays);
#if defined(WITH_HARNESS) && WITH_HARNESS
        // -showoff=<flag>[,<flag>...] (harness): the main view hides these as
        // well - the reverse arm of the flags (test_view_showflags.py). Read once;
        // a name that is no flag is an error that lists them.
        static const uint32_t s_showOff = []
        {
            const std::string spec = Sys::CommandLine::Get().Get("-showoff");
            uint32_t bits = 0;
            size_t start = 0;
            while (start < spec.size())
            {
                size_t end = spec.find(',', start);
                if (end == std::string::npos) end = spec.size();
                const std::string name = spec.substr(start, end - start);
                RG::ShowFlag f{};
                if (RG::ShowFlagFromName(name, f))
                    bits |= static_cast<uint32_t>(f);
                else if (!name.empty())
                    LOG_ERROR("-showoff: '%s' is not a ShowFlag (%s)", name.c_str(),
                              RG::ShowFlagList(RG::kShowAll, ", ").c_str());
                start = end + 1;
            }
            if (bits)
                LOG_INFO("[RG_VIEW] -showoff: the main view also hides %s", RG::ShowFlagList(bits).c_str());
            return bits;
        }();
        view.show.HideAll(s_showOff);
#endif
        m_graph.SetView(view);
        // The selection outline is an editor overlay.
        if (m_outlinePass) m_outlinePass->SetPickingOutlineShown(view.show.Has(RG::ShowFlag::EditorOverlays));
    }

    if (m_shadowPass && m_lightingPass)
        m_lightingPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());
    // Water's sun glint is direct light: the same CSM array lets terrain shadows
    // kill the glint where the sun is occluded.
    if (m_shadowPass && m_waterPass)
        m_waterPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());
    // Translucent surfaces receive the sun shadow; without it every glass /
    // foliage-card / lit-VFX material renders as if the sun were unoccluded. No
    // root-signature cost: Gfx::ShadowSRV is already in the shared graphics
    // signature.
    if (m_shadowPass && m_transparentPass)
        m_transparentPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());
    // Only lit particle buckets that opted into receiveShadow bind it: an emitter
    // without the flag compiles a PS that never declares t9.
    if (m_shadowPass && m_particleRenderPass)
        m_particleRenderPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());
    // Brow/lash overlay: cascades only (ComputeShadowFactorExternalOnly), since
    // the per-object map holds the character's own casters and this overlay
    // exists to draw THROUGH them.
    if (m_shadowPass && m_faceOverlayPass)
        m_faceOverlayPass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());
    if (m_shadowPass && m_waterSurfacePass)
        m_waterSurfacePass->SetShadowMap(m_shadowPass->GetShadowArrayGpuHandle());

    if (m_clusterPass && m_lightingPass)
    {
        m_lightingPass->SetClusterSRVs(
            m_clusterPass->GetLightsSRVHandle(),
            m_clusterPass->GetLightIndexSRVHandle(),
            m_clusterPass->GetLightGridSRVHandle());
        // The reflection-probe lists of the same grid: this view's (ClusterPass is bound to it).
        m_lightingPass->SetReflectionProbeCluster(m_clusterPass->GetProbeGridSRVHandle(),
                                                  m_clusterPass->GetProbeIndexSRVHandle());
    }
    // Water is forward-shaded, so it needs the same cluster buffers to see
    // point/spot lights (the deferred pass cannot light it).
    if (m_clusterPass && m_waterSurfacePass)
        m_waterSurfacePass->SetClusterSRVs(
            m_clusterPass->GetLightsSRVHandle(),
            m_clusterPass->GetLightIndexSRVHandle(),
            m_clusterPass->GetLightGridSRVHandle());
    // So are glass, posters and every other translucent surface
    // (MS_ForwardLighting's clustered loop), with the spot shadows LightingPass
    // gets (the point-cube atlas rides LightCB.pointShadowIdx).
    if (m_clusterPass && m_transparentPass)
        m_transparentPass->SetClusterSRVs(
            m_clusterPass->GetLightsSRVHandle(),
            m_clusterPass->GetLightIndexSRVHandle(),
            m_clusterPass->GetLightGridSRVHandle());
    if (m_transparentPass)
        m_transparentPass->SetSpotShadowVP(m_spotShadowPass ? m_spotShadowVPSrv[frameSlot] : 0);
    // The water surface's local lights take the same spot shadows.
    if (m_waterSurfacePass)
        m_waterSurfacePass->SetSpotShadowVP(m_spotShadowPass ? m_spotShadowVPSrv[frameSlot] : 0);

    // Spot shadow atlas + per-slice VP buffer, for both Lighting and
    // VolumetricFog. The VP buffer is per frame slot: feed this frame's SRV handle.
    if (m_spotShadowPass && m_lightingPass)
        m_lightingPass->SetSpotShadowAtlas(
            m_spotShadowPass->GetAtlasSrvHandle(),
            m_spotShadowVPSrv[frameSlot]);
    if (m_spotShadowPass && m_volFogPass)
        m_volFogPass->SetSpotShadowAtlas(
            m_spotShadowPass->GetAtlasSrvHandle(),
            m_spotShadowVPSrv[frameSlot]);

    // Point-light cube shadow atlas, for Lighting.ps. The shadow maps are bindless,
    // not root parameters: the handles set here are read back later in RenderAsync
    // and converted into LightCB heap indices, so this must stay ahead of that
    // resolve block (it does: this runs before the frame records).
    if (m_pointShadowPass && m_lightingPass)
        m_lightingPass->SetPointShadowAtlas(m_pointShadowPass->GetCubeAtlasSrvHandle());

    // Per-object (player) high-res shadow map, for Lighting.ps. Always hand over
    // the live SRV (always valid, in DEPTH_READ_SRV after the pass); the shader
    // gates the sample on LightCB.perObjShadowParams.x.
    if (m_perObjShadowPass && m_lightingPass)
        m_lightingPass->SetPerObjectShadowMap(m_perObjShadowPass->GetShadowSrvHandle());

    // Hair (bangs -> face) shadow map, for Lighting.ps (face pixels only). Same
    // contract: the shader gates on hairShadowParams.x.
    if (m_hairShadowPass && m_lightingPass)
        m_lightingPass->SetHairShadowMap(m_hairShadowPass->GetShadowSrvHandle());

    // MaterialBuffer SRV for LightingPass's per-material NPR params. Lighting.ps
    // copies whole MaterialGPUData records on the SSS / NPR paths, so this
    // resolves the DEFAULT-heap buffer (MaterialSRVBuffer), not staging.
    if (m_lightingPass && MaterialSRVBuffer(frameSlot).IsValid())
        m_lightingPass->SetMaterialBuffer(
            m_gfx.GetBufferSRVGpuHandle(MaterialSRVBuffer(frameSlot)));

    // AO source defaults to none. Both producers run inside the graph ahead of
    // LightingPass, and which one owns gSSAO is known only once the TLAS state is
    // resolved: RenderAsync rebinds it in Phase 2b.9 (RTAO) or 2b.9b (XeGTAO). 0
    // here makes a frame where neither is active fall through to the shared lit
    // fallback instead of a stale SRV.
    if (m_lightingPass)
        m_lightingPass->SetSSAOHandle(0);

    // Per-frame state for the volumetric fog pass: the scene's medium, local volumes, sun and
    // lights once (the main view), the camera and cascades for each view (its LightCB, the bound
    // view's inputs). The scene-authored fog FIRST: WantsFog() below (voxel gating), the pass's
    // Execute / IsActive and the participating medium all read what this publishes. The path
    // tracers' medium is uploaded once a frame too (its reach is about the main camera): a second
    // upload would overwrite the ring slot the main view's dispatches read.
    if (primary)
    {
        if (m_volFogPass)
        {
            const VolFogSceneState& fs = VolFogSceneRead();
            m_volFogPass->SetSceneFog(fs.fog);
            m_volFogPass->SetLocalFogVolumes(fs.locals, fs.emissiveCount);
        }
        UploadMediumSet();
    }
    if (m_volFogPass && m_lightCB.Current(m_gfx))
    {
        auto* lb = m_lightCB.Current(m_gfx);

        // Matrices uploaded transposed for HLSL row-vector mul(pos, matrix).
        // invViewProj pairs with the un-jittered VP: voxel-derived worldPos must be
        // jitter-stable for reprojection.
        using namespace DirectX;
        DirectX::XMFLOAT4X4 vp{},  invVp{}, prevVp{};
        XMMATRIX vpLoaded      = XMLoadFloat4x4(&FrameViewRead().view.viewProjMatrixNoJitter);
        XMMATRIX prevLoaded    = XMLoadFloat4x4(&state.jitter.GetPrevViewProjNoJitter());
        XMMATRIX invVpNoJitter = XMMatrixInverse(nullptr, vpLoaded);
        XMStoreFloat4x4(&vp,     XMMatrixTranspose(vpLoaded));
        XMStoreFloat4x4(&prevVp, XMMatrixTranspose(prevLoaded));
        XMStoreFloat4x4(&invVp,  XMMatrixTranspose(invVpNoJitter));

        DirectX::XMFLOAT3 camPos { lb->cameraPos[0], lb->cameraPos[1], lb->cameraPos[2] };
        // Per-frame index for the froxel temporal jitter (GetFroxelJitter's Halton
        // cycle). A secondary view takes the main view's (recorded before it this frame).
        static uint32_t s_volFogFrame = 0;
        const uint32_t volFogFrame = primary ? s_volFogFrame++ : s_volFogFrame - 1;
        m_volFogPass->SetFrameState(vp, invVp, prevVp, camPos,
                                     FrameViewRead().view.nearZ, FrameViewRead().view.farZ,
                                     volFogFrame);

        DirectX::XMFLOAT3 sunDir   { lb->lightDir[0],   lb->lightDir[1],   lb->lightDir[2] };
        DirectX::XMFLOAT3 sunColor { lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
        // Sun only contributes when a directional light has VolumetricLightComponent.
        const float sunStrength = m_sunVolumetric ? m_sunVolumetricScale : 0.0f;
        if (primary) m_volFogPass->SetSun(sunDir, sunColor, sunStrength);

        DirectX::XMFLOAT3 camFwd { lb->cameraForward[0], lb->cameraForward[1], lb->cameraForward[2] };
        m_volFogPass->SetCameraForward(camFwd);
        // (The camera-stack hard cut - the LiveCamera's !historyValid: HardCutTo,
        // a first frame, a cinematic shot boundary - reaches the fog and XeGTAO
        // through the main view's FrameBreaks, set at the top of this function.)

        if (m_shadowPass)
        {
            DirectX::XMFLOAT4X4 sm[3];
            std::memcpy(&sm[0], lb->shadowMatrix[0], sizeof(sm[0]));
            std::memcpy(&sm[1], lb->shadowMatrix[1], sizeof(sm[1]));
            std::memcpy(&sm[2], lb->shadowMatrix[2], sizeof(sm[2]));
            DirectX::XMFLOAT3 splits { lb->cascadeSplits[0],
                                      lb->cascadeSplits[1],
                                      lb->cascadeSplits[2] };
            m_volFogPass->SetShadowState(m_shadowPass->GetShadowArrayGpuHandle(),
                                         sm, splits,
                                         lb->shadowMapTexelSize,
                                         lb->shadowBias,
                                         lb->shadowStrength);
        }

        // Per-frame volumetric-only light list (filtered by VolumetricLightComponent above).
        if (primary) m_volFogPass->SetVolumetricLights(VolLightsRead());

        // Tier-2 triangle-precision occupancy grid for spotlight shadowing without
        // shadow maps, camera-snapped to voxel size so sub-voxel drift does not force a
        // re-voxelise. Each view's, around its camera, from its own list (the bound
        // SceneVoxelPass grid). The ray-traced lighting traces its lights instead: not while it
        // runs for the bound view (last frame's tier: this frame's plan comes later; a switch costs
        // one frame).
        if (m_sceneVoxelPass && m_volFogPass->WantsFog() && !m_volFogPass->RtRanLastFrame())
        {
            using namespace DirectX;
            constexpr float    kHalfExtent = 32.0f;            // +-32 m cube
            constexpr uint32_t kDim        = SceneVoxelPass::kGridDim;
            constexpr float    kVoxelSize  = (2.0f * kHalfExtent) / float(kDim);

            auto snap = [](float v) {
                return std::floor(v / kVoxelSize + 0.5f) * kVoxelSize;
            };
            const XMFLOAT3 centre {
                snap(FrameViewRead().view.cameraPosition.x),
                snap(FrameViewRead().view.cameraPosition.y),
                snap(FrameViewRead().view.cameraPosition.z)
            };
            const XMFLOAT3 gMin { centre.x - kHalfExtent,
                                  centre.y - kHalfExtent,
                                  centre.z - kHalfExtent };
            const XMFLOAT3 gMax { centre.x + kHalfExtent,
                                  centre.y + kHalfExtent,
                                  centre.z + kHalfExtent };
            m_sceneVoxelPass->SetGridBounds(gMin, gMax);

            // Shader bindings: InstanceBuffer + MeshDescriptors SRVs + bindless
            // g_Buffers[] table. The voxelize CS reads InstanceBuffer[instanceOffset] with
            // a CPU-provided per-dispatch value; PushDraw below hands it the resolved
            // stable slot, so the CS needs no remap awareness.
            m_sceneVoxelPass->SetSceneBindings(
                m_gfx.GetBufferSRVGpuHandle(RetainedInstancesActive()
                                                ? m_instanceBufferGPU
                                                : m_instanceBuffer[frameSlot]),
                m_gfx.GetBufferSRVGpuHandle(m_meshMgr.GetDescriptorHeap().CurrentMeshDescBuffer()),
                m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());

            // Per-mesh dispatches from opaque packets only; the CPU-side WorldAabb cull
            // bounds the cost to the grid neighbourhood (avoids scene-wide dispatch
            // jitter).
            m_sceneVoxelPass->ResetDraws();
            // Bounds come from InstAabbRead() (flattened by SortAndEmit on the game half),
            // indexed by the draw-local instance index the packet carries. Do not go
            // through the instance slot: the dynamic-overflow fallback aliases several
            // instances onto slot 0, so a slot lookup could cull on another entity's
            // bounds.
            for (const DrawPacket& dp : drawList)
            {
                if (dp.filter != DrawFilter::Opaque) continue;
                if (dp.vertexOrIndexCount < 3)       continue;
                const uint32_t triCount = dp.vertexOrIndexCount / 3u;
                for (uint32_t inst = 0; inst < dp.instanceCount; ++inst)
                {
                    const uint32_t dl = dp.instanceOffset + inst;
                    // Resolve the packet's draw-local index to the instance-buffer slot (identity
                    // when retained mode is off).
                    const uint32_t instSlot = SlotForDrawLocalInstance(dl);
                    if (dl < InstAabbRead().size())
                    {
                        // min.x > max.x is the lane's "no bounds" encoding (no entity, no WorldAabb,
                        // degenerate box).
                        const InstAabb& wa = InstAabbRead()[dl];
                        if (wa.min.x <= wa.max.x)
                        {
                            if (wa.max.x < gMin.x || wa.min.x > gMax.x) continue;
                            if (wa.max.y < gMin.y || wa.min.y > gMax.y) continue;
                            if (wa.max.z < gMin.z || wa.min.z > gMax.z) continue;
                        }
                    }
                    m_sceneVoxelPass->PushDraw(dp.meshDescriptorIndex,
                                               instSlot,
                                               triCount);
                }
            }

            // Reported once: otherwise "the lane was empty so nothing was culled" and "the
            // cull worked" look the same (the pass just dispatches more), and nothing else
            // reads SceneVoxelPass's draw count.
            static bool s_reportedVoxelLane = false;
            if (!s_reportedVoxelLane)
            {
                s_reportedVoxelLane = true;
                uint32_t withBounds = 0;
                for (const InstAabb& a : InstAabbRead())
                    if (a.min.x <= a.max.x) ++withBounds;
                LOG_INFO("[SPLITAUDIT][VOXELAABB] lane %zu instances, %u with bounds, "
                         "%u opaque packets", InstAabbRead().size(), withBounds,
                         static_cast<uint32_t>(DrawPacketsRead().size()));
            }

            m_volFogPass->SetVoxelOcclusion(
                m_sceneVoxelPass->GetOccupancySrvHandle(), gMin, gMax, kDim);
        }
        else
        {
            // Disabled: force the "no grid bound" branch and clear queued draws so the
            // empty bail fires.
            if (m_sceneVoxelPass) m_sceneVoxelPass->ResetDraws();
            m_volFogPass->SetVoxelOcclusion(
                0, DirectX::XMFLOAT3{0,0,0}, DirectX::XMFLOAT3{0,0,0}, 0);
        }
    }
}

// The compute prepasses recorded before the graph (skinning, afterimage
// capture, particle / ripple / trail / tracer / beam sims) and the VFX render
// passes' frame data.
void Renderer::Render_ComputePrepass()
{
    // ---- Phase 0: skinning CS ----
    // Writes the SkinnedVertexRing UAVs, trailing UAV barrier for GBuffer.
    // Afterimage capture runs on the same CL right after SkinningPass, so it sees
    // the freshly skinned vertices; it emits its own UAV barrier on the snapshot
    // pool before any later pass reads it.
    if (m_skin.GetSkinningPass() && m_skin.IsInitialised() && !m_skin.SkinJobsRead().empty())
    {
        RHI::CommandList skinCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(skinCL, "Skinning");
        skinCL.gfx = &m_gfx;
        // Published so RenderAsync can give the compute-queue scene-AS build an
        // explicit dependency on it: the scene AS reads this frame's skinned
        // vertex-ring slices, and command-list allocation order does not imply it.
        m_frameSkinCL = skinCL;
        auto* skinPass = m_skin.GetSkinningPass();
        auto* capturePass = m_afterimageCapturePass && m_afterimageSystem &&
            !m_afterimageSystem->GetCaptureJobs().empty() ? m_afterimageCapturePass.get() : nullptr;
        auto record = [skinPass, capturePass, skinCL]() mutable {
            ZoneScopedN("SkinRecord.Work");
            // The skinning CL is recorded outside the render graph, which opens every other
            // pass's GPU timestamp region; without its own region the dispatch would be
            // missing from the profiler's GPU busy time (a union of regions) and its pass
            // table. Begin and end on this thread, around the recording.
            const uint32_t region = skinCL.gfx->BeginGPUTimestamp(skinCL, "SkinningPass");
            skinPass->Execute(skinCL);
            skinCL.gfx->EndGPUTimestamp(skinCL, region);
            if (capturePass) capturePass->Execute(skinCL);
        };
        // Initial coarse-work cutoff, not a hardware optimum: paired captures benefit
        // at 1500 jobs, but not reliably at 15 jobs.
        static const bool s_enabled = Sys::CommandLine::Get().Has("-skinrecordjobs") || Sys::JobSplittingDefaultsEnabled();
        static const bool s_disabled = Sys::CommandLine::Get().Has("-skinrecordoff");
        bool graph = s_enabled || m_skin.SkinJobsRead().size() >= 128;
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_verify = Sys::CommandLine::Get().Has("-skinrecordverify");
#endif
        if (s_disabled) graph = false; // Explicit rollback wins over force.
        if (graph)
        {
            ZoneScopedN("SkinRecord.Graph");
            // One coarse job. Pass scratch and mapped CB rings belong to this
            // recording until WaitSkinWorker; published skin/afterimage inputs
            // stay stable for the whole Render call. No worker CL allocation.
            m_renderWorkers[1].Kick(std::move(record));
        }
        else
        {
            ZoneScopedN("SkinRecord.Legacy");
            record();
        }
#if defined(WITH_HARNESS) && WITH_HARNESS
        if ((s_enabled || s_verify) && FrameViewRead().currentFrame % 120 == 0)
            LOG_INFO("[SKIN_RECORD] jobs=%zu graph=%u afterimages=%zu",
                     m_skin.SkinJobsRead().size(), graph ? 1u : 0u,
                     m_afterimageSystem ? m_afterimageSystem->GetCaptureJobs().size() : 0u);
#endif
        // skinCL is submitted by EndFrame in allocation order (before GBuffer CLs)
    }

    // ---- Phase 0.5: Particle sim CS (emit+update) before render passes; trailing UAV barrier.
    if (m_particleSimPass && m_particleSystem)
    {
        // Feed mesh-shape sampling; no-op for non-mesh emitters.
        m_particleSimPass->SetMeshDescriptorBinding(
            &m_meshMgr.GetDescriptorHeap().CurrentMeshDescBuffer(),
            m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());

        RHI::CommandList pCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(pCL, "ParticleSim");
        pCL.gfx = &m_gfx;
        m_particleSimPass->Execute(pCL);
    }

    // ---- Phase 0.6: interactive water ripple sim (height-field wave step) ----
    // Runs before the render graph so WaterPass samples the freshly written field
    // (transitioned to SHADER_RESOURCE inside Execute). Self-skips when no
    // ripple-enabled water body is active this frame.
    if (m_waterRippleSystem)
    {
        RHI::CommandList rCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(rCL, "WaterRipple");
        rCL.gfx = &m_gfx;
        m_waterRippleSystem->Execute(rCL);
    }

    // ---- Phase 0.6: Trail update compute -----------------------------------
    if (m_trailUpdatePass && m_trailSystem)
    {
        RHI::CommandList tCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(tCL, "TrailUpdate");
        tCL.gfx = &m_gfx;
        m_trailUpdatePass->Execute(tCL);
    }

    // ---- Phase 0.7: Tracer emit+age CS (same UAV-barrier pattern as ParticleSimPass).
    if (m_tracerSimPass && m_tracerSystem)
    {
        RHI::CommandList trCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(trCL, "TracerSim");
        trCL.gfx = &m_gfx;
        m_tracerSimPass->Execute(trCL);
    }

    // ---- Phase 0.8: beam tube CS - fills shared PVF UAVs; trailing UAV barrier for SRV reads.
    if (m_beamSimPass && m_beamSystem)
    {
        RHI::CommandList bCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(bCL, "BeamSim");
        bCL.gfx = &m_gfx;
        m_beamSimPass->Execute(bCL);
    }

    Render_BindVfxFrameData();
}

void Renderer::Render_BindVfxFrameData()
{
    // Particle render frame data: derive cam right/up from view matrix rows (row-major).
    if (m_particleRenderPass)
    {
        const auto& vm = FrameViewRead().view.viewMatrix;
        DirectX::XMFLOAT3 camRight = { vm._11, vm._21, vm._31 };
        DirectX::XMFLOAT3 camUp    = { vm._12, vm._22, vm._32 };
        m_particleRenderPass->SetFrameData(
            FrameViewRead().view.viewProjMatrix, camRight, camUp, 1.0f, FrameViewRead().globalTimeSec);
        // The render pass pulls per-(shader, blend) buckets from m_particleSystem in
        // Execute; no per-config push is needed.
    }

    // ---- Trail render pass frame data --------------------------------------
    if (m_trailRenderPass)
    {
        m_trailRenderPass->SetFrameData(FrameViewRead().view.viewProjMatrix, FrameViewRead().view.cameraForward,
                                        FrameViewRead().globalTimeSec);
    }

    // ---- Tracer render pass frame data -------------------------------------
    if (m_tracerRenderPass)
    {
        m_tracerRenderPass->SetFrameData(
            FrameViewRead().view.viewProjMatrix,
            FrameViewRead().camera.position,
            FrameViewRead().globalTimeSec,
            FrameViewRead().camera.nearZ,
            FrameViewRead().camera.farZ);
    }
}

// GPU cull for the shadow family (CSM cascades, spot slices, point cubes):
// zero the count grid, move the per-view buffers to UAV, dispatch
// InstanceCull.cs mode 2 once per view (its own frustum planes + ViewBit + the
// per-object exclusion decided in UploadFrameData), then hand the args /
// counts to INDIRECT_ARGUMENT and the remaps to NON_PIXEL_SHADER_RESOURCE for
// ShadowPass's draws later in the same command list. All DEFAULT buffers
// decayed to COMMON at the last ECL boundary (as in the main-view cull); the
// count copy promotes its target to COPY_DEST implicitly. Returns false when
// ShadowPass must fall back to its CPU arg ring.
bool Renderer::RecordShadowFamilyCull(RHI::CommandList cl, uint32_t frameSlot)
{
    // Latch guarantees the buffers/meta exist; these are the hard runtime
    // dependencies of the recording itself.
    if (!m_cullingPass || !m_shadowSystem) return false;

    const uint32_t cmdCount = std::min<uint32_t>(SceneEmitRead().indirectDrawCount, m_maxInstances);
    uint32_t totalShadowCmds = 0;
    for (int g = 0; g < ShadowDrawGroup::RetainedCount; ++g)
        totalShadowCmds += SceneEmitRead().shadowGroupCap[g];
    // No casters (or no packets): every cap is 0 and ShadowPass skips every group,
    // so the stale buffers are never referenced and there is nothing to record.
    if (cmdCount == 0 || totalShadowCmds == 0) return true;

    // LOAD-BEARING early-return: returning false makes ShadowPass fall back to
    // its CPU arg ring (see the comment at the RecordShadowFamilyCull call
    // site). These guards are also the ONLY protection against an invalid
    // handle reaching TranslateBarrier's BUFFER case, which indexes
    // m_bufferPool with no IsValid() and no bounds check.
    for (uint32_t v = 0; v < kShadowCullViewsTotal; ++v)
        if (!m_shadowWorkingArgs[v].IsValid() || !m_shadowWorkingRemap[v].IsValid())
            return false;
    if (!m_shadowGroupCounts.IsValid()) return false;

    uint32_t region = m_gfx.BeginGPUTimestamp(cl, "ShadowCull");

    // Zero every view's per-bucket counts in one copy (zero source is
    // memset once at creation, never written after).
    const uint64_t countBytes = uint64_t(kShadowCullViewsTotal) *
                                ShadowDrawGroup::RetainedCount * sizeof(uint32_t);
    m_gfx.CopyBufferRegion(m_shadowCountZeroUpload, 0,
                           m_shadowGroupCounts, 0, countBytes, cl);

    {
        // ResourceState::UNDEFINED is the enumerator that maps to
        // D3D12_RESOURCE_STATE_COMMON, the state the working buffers decayed to at
        // the last ECL boundary.
        RHI::GPUBarrier pre[1 + 2 * kShadowCullViewsTotal];
        pre[0] = RHI::GPUBarrier::Buffer(&m_shadowGroupCounts,
                                         RHI::ResourceState::COPY_DST,
                                         RHI::ResourceState::UNORDERED_ACCESS);
        for (uint32_t v = 0; v < kShadowCullViewsTotal; ++v)
        {
            pre[1 + v * 2 + 0] = RHI::GPUBarrier::Buffer(&m_shadowWorkingArgs[v],
                                     RHI::ResourceState::UNDEFINED,
                                     RHI::ResourceState::UNORDERED_ACCESS);
            pre[1 + v * 2 + 1] = RHI::GPUBarrier::Buffer(&m_shadowWorkingRemap[v],
                                     RHI::ResourceState::UNDEFINED,
                                     RHI::ResourceState::UNORDERED_ACCESS);
        }
        m_gfx.PushBarriers(pre, _countof(pre), cl);
    }

    // One dispatch per cascade view: the same matrix source UploadCascadeCBs reads
    // (final: UploadFrameData ran inside BeginFrame) and the same Gribb-Hartmann
    // plane convention (RenderTypes.h, as the cascade CB block Terrain.shadow.as
    // culls with).
    const DirectX::XMFLOAT4X4* cascVP = m_shadowSystem->CascadeMatricesForPass();
    const uint64_t pristineRemapSrv =
        m_gfx.GetBufferSRVGpuHandle(m_instanceRemapBuffer[frameSlot]);
    const uint64_t pristineArgsSrv =
        m_gfx.GetBufferSRVGpuHandle(m_indirectArgUpload[frameSlot]);
    const uint64_t shadowMetaSrv =
        m_gfx.GetBufferSRVGpuHandle(m_shadowCmdMeta[frameSlot]);

    // One dispatch = one view slice. `view` is the global slice index into the
    // sliced working buffers and into the count grid (countBase = view *
    // ShadowDrawGroup::RetainedCount); the passes recompute the same index from the
    // same family bases, so an off-by-one would silently draw another view's
    // commands. Keep both sides reading the same constants.
    // meshletDispatch (CSM views only): the meshlet twin buckets' survivors are
    // written as DISPATCH_MESH for ShadowMeshlet.as.
    auto dispatchView = [&](uint32_t view, const float (&planes)[6][4],
                            uint32_t requiredViewBits, bool excludePerObj, bool dynamicOnly = false,
                            bool meshletDispatch = false)
    {
        m_cullingPass->SetFrustumPlanes(planes);
        m_cullingPass->ExecuteCompactShadow(cl,
            m_instanceBufferGPU,   // latch implies retained instances
            m_meshMgr.GetDescriptorHeap().CurrentMeshAABBBuffer(),
            m_shadowWorkingArgs[view],
            cmdCount,
            pristineRemapSrv,
            pristineArgsSrv,
            shadowMetaSrv,
            m_shadowWorkingRemap[view],
            m_shadowGroupCounts,
            /*countBase=*/ view * ShadowDrawGroup::RetainedCount,
            requiredViewBits,
            excludePerObj, dynamicOnly, meshletDispatch);
    };

    // A frame without the CSM (FramePlan::Decisions::csm: the path tracer owns it
    // and nothing reads the cascades) culls none of its views; their counts stay
    // at the zero copied above and ShadowPass does not run.
    const uint32_t csmViews = m_plan.csm ? kShadowCullViews : 0u;
    for (uint32_t v = 0; v < csmViews; ++v)
    {
        const FrustumPlanes fp = ExtractFrustumPlanes(cascVP[v]);
        float planes[6][4];
        for (int p = 0; p < 6; ++p)
        {
            planes[p][0] = fp[p].normal.x;
            planes[p][1] = fp[p].normal.y;
            planes[p][2] = fp[p].normal.z;
            planes[p][3] = fp[p].distance;
        }
        // The shadow PSOs disable depth clip (pancaking): casters on the sun side of
        // the ortho volume clamp onto the nearest depth and still occlude, and the
        // ring path never volume-tests them (long sunset shadows from beyond-the-box
        // structures depend on it). Plane 5 is the clip z'>w boundary = that sun-side
        // plane (reversed-Z light ortho maps the light plane to z'=1), so culling by it
        // would drop exactly the casters pancaking preserves: neutralize it (0*p+1 >= 0
        // always passes). The XY planes still cull (XY clipping is unaffected by
        // depth-clip-disable) and plane 4 (the geometric far side) stays: those
        // casters clamp to depth 0, which can never darken a receiver inside the
        // volume.
        planes[5][0] = 0.0f; planes[5][1] = 0.0f;
        planes[5][2] = 0.0f; planes[5][3] = 1.0f;
        // The cascade's meshlet twins as DISPATCH_MESH, culled per cluster by
        // ShadowMeshlet.as with these planes (the bank it reads at b4).
        const bool meshletCsm = SceneEmitRead().shadowMeshletFrameActive;
        if (meshletCsm) WriteMeshletShadowBank(v, planes, cascVP[v]);
        dispatchView(v, planes,
                     /*requiredViewBits=*/ ViewBit::ShadowCascade0 << v,
                     /*excludePerObj=*/ SceneEmitRead().perObjCsmExcludeFrame,
                     /*dynamicOnly=*/ false, meshletCsm);
    }

    // ---- Spot slices ----
    // One view per atlas slice, frustum = that spot's own cone VP (the exact
    // matrix the pass binds; spotCullVP is written beside SetShadowMatrix).
    // requiredViewBits is ShadowAny, not a cascade bit: that is the gate
    // SpotShadowPass::skipCaster applies on the CPU ring path, so the culled list
    // is a strict subset of what the ring would draw. excludePerObj is false: only
    // the CSM hands the player to PerObjectShadowPass.
    // Plane 5 (the geometric near cap, kNearPlane 0.1 from the light) is
    // neutralized: the spot PSO clips there itself (depth clip on - a triangle
    // crossing the light's plane must be cut at the cap, not dropped), so keeping
    // such casters is merely conservative. Plane 4 (the far cap at the light
    // radius) stays: past the radius the light contributes nothing and the PSO
    // clips a caster beyond it anyway, so dropping it changes no shadow lookup.
    for (uint32_t s = 0; s < SceneEmitRead().spotCullCount; ++s)
    {
        const FrustumPlanes fp = ExtractFrustumPlanes(SceneEmitRead().spotCullVP[s]);
        float planes[6][4];
        for (int p = 0; p < 6; ++p)
        {
            planes[p][0] = fp[p].normal.x;
            planes[p][1] = fp[p].normal.y;
            planes[p][2] = fp[p].normal.z;
            planes[p][3] = fp[p].distance;
        }
        planes[5][0] = 0.0f; planes[5][1] = 0.0f;
        planes[5][2] = 0.0f; planes[5][3] = 1.0f;
        dispatchView(kSpotCullViewBase + s, planes,
                     /*requiredViewBits=*/ ViewBit::ShadowAny,
                     /*excludePerObj=*/ false,
                     m_spotShadowPass && m_spotShadowPass->CullDynamicOnly(s));
    }

    // ---- Point cubes ----
    // One view per cube, shared by all six faces (see kPointCullViewBase).
    // CompactShadowCommand is plane-only, so the light sphere is expressed as the
    // 6 axis-aligned planes of its bounding box; with FrustumTestWorldAABB's
    // positive-vertex selection that is an exact conservative AABB-vs-AABB overlap
    // test against [c-r, c+r]^3, which is what a cube shadow needs.
    for (uint32_t c = 0; c < SceneEmitRead().pointCullCount; ++c)
    {
        const DirectX::XMFLOAT4& sph = SceneEmitRead().pointCullSphere[c];
        const float r = sph.w;
        // dot(n, p) + d >= 0 <=> inside. Normals point inward (as in
        // ExtractFrustumPlanes), already unit length.
        const float planes[6][4] = {
            {  1.0f,  0.0f,  0.0f, r - sph.x },   // p.x >= cx - r
            { -1.0f,  0.0f,  0.0f, r + sph.x },   // p.x <= cx + r
            {  0.0f,  1.0f,  0.0f, r - sph.y },
            {  0.0f, -1.0f,  0.0f, r + sph.y },
            {  0.0f,  0.0f,  1.0f, r - sph.z },
            {  0.0f,  0.0f, -1.0f, r + sph.z },
        };
        dispatchView(kPointCullViewBase + c, planes,
                     /*requiredViewBits=*/ ViewBit::ShadowAny,
                     /*excludePerObj=*/ false);
    }

    {
        // Two separate pushes on purpose: the UAV flush must be its own
        // ResourceBarrier call. Merging the arrays would put the UAV barriers
        // and the transitions in one call, where D3D12 leaves their relative
        // order unspecified.
        RHI::GPUBarrier uavDone[1 + 2 * kShadowCullViewsTotal];
        uavDone[0] = RHI::GPUBarrier::Memory(&m_shadowGroupCounts);
        for (uint32_t v = 0; v < kShadowCullViewsTotal; ++v)
        {
            uavDone[1 + v * 2 + 0] = RHI::GPUBarrier::Memory(&m_shadowWorkingArgs[v]);
            uavDone[1 + v * 2 + 1] = RHI::GPUBarrier::Memory(&m_shadowWorkingRemap[v]);
        }
        m_gfx.PushBarriers(uavDone, _countof(uavDone), cl);

        // Args + counts feed ShadowPass's ExecuteIndirect; the remaps are
        // read by Shadow.vs via ResourceDescriptorHeap (non-pixel).
        // SHADER_RESOURCE_COMPUTE is the only enumerator mapping to bare
        // NON_PIXEL_SHADER_RESOURCE; SHADER_RESOURCE would widen it to
        // PIXEL|NON_PIXEL.
        RHI::GPUBarrier post[1 + 2 * kShadowCullViewsTotal];
        post[0] = RHI::GPUBarrier::Buffer(&m_shadowGroupCounts,
                      RHI::ResourceState::UNORDERED_ACCESS,
                      RHI::ResourceState::INDIRECT_ARGUMENT);
        for (uint32_t v = 0; v < kShadowCullViewsTotal; ++v)
        {
            post[1 + v * 2 + 0] = RHI::GPUBarrier::Buffer(&m_shadowWorkingArgs[v],
                                      RHI::ResourceState::UNORDERED_ACCESS,
                                      RHI::ResourceState::INDIRECT_ARGUMENT);
            post[1 + v * 2 + 1] = RHI::GPUBarrier::Buffer(&m_shadowWorkingRemap[v],
                                      RHI::ResourceState::UNORDERED_ACCESS,
                                      RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        }
        m_gfx.PushBarriers(post, _countof(post), cl);
    }

    m_gfx.EndGPUTimestamp(cl, region);
    return true;
}
