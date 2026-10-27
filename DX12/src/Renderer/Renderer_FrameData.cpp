// Renderer_FrameData.cpp - UploadFrameData: the per-frame constant buffers and tables
// written from the frame's extracted scene.

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
#include <bit>
#include <cstring>
#include <cstdlib>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// One view's camera-derived frame data: its matrices and jitter, the b1 constants, its RenderView and
// frustum, LightCB's camera fields and its cascades - the same code for the main view (UploadFrameData,
// game half) and a secondary one (the start of its segment, render half), so the two agree for the same
// camera.
void Renderer::BuildViewFrame(const ViewFrameTarget& t)
{
    FrameView& fv = t.fv;
    ViewState& vs = t.view;

    // FSR, DLSS and RR as this frame's plan has them (m_gamePlan); the render half decides
    // them from the same intent. Their contexts (FSR 2 / DLSS create, resize, release on
    // deselect) are made at the service point, ServiceEnableGatedResources, before the
    // plan's resolve.
    const FramePlan::Decisions& plan = t.plan;

    // The frame's single Streamline serialisation point. App takes this frame's token at the
    // very top of the loop (Reflex needs it before input is sampled, long before BeginFrame);
    // read that one rather than taking a second: two tokens in a frame pair one frame's
    // constants with another frame's tags, and with the CPU kFrameCount-1 frames ahead the
    // mismatch is silent.
    // GAME token, held in a local: UploadFrameData is game-half work describing the camera
    // the game just simulated, so its constants belong to the game's frame. Do not write
    // m_slFrameToken: that member is the render half's, and the two halves can be on
    // different frames.
    void* const slFrameGame = t.primary ? SL::StreamlineBackend::Get().GameFrameToken() : nullptr;

    // Build the view from this view's camera pose (the main view's: position + forward come from
    // the camera entity's GlobalTransform; see App::Run / Renderer::SetCamera). The matrix math
    // lives in BuildCameraBasisFor/BuildCameraViewProjFor so the scene-build consumers
    // (cluster/decal camera CBs) derive identical matrices earlier in the frame. Always @p fv's:
    // a secondary view builds on the render half, where FrameViewWrite() is a later frame's main
    // camera. Reversed-Z: near/far swapped; main depth is D32_FLOAT_S8X24 so the precision win is
    // fully realised. Editor ortho views build a reversed-Z ortho box (the same swapped near/far
    // the CSM light projections use).
    XMVECTOR pos, forward, up;
    BuildCameraBasisFor(fv, pos, forward, up);
    XMMATRIX view, projBase;
    BuildCameraViewProjFor(fv, view, projBase);

    // Save unjittered VP for TAA reprojection (becomes prevViewProj next frame).
    XMMATRIX viewProjNoJitter = XMMatrixMultiply(view, projBase);

    // Halton subpixel jitter (+-0.5 px, 16 positions -> 1 px box filter).
    // Ortho: jitter goes into proj rows _31/_32, a constant NDC offset only when w == viewZ
    // (perspective); under ortho it would shear with depth, so no jitter is wanted for ortho
    // frames.
    const bool taaEnabled = (m_taaPass && m_taaPass->IsEnabled()) && !fv.camera.orthographic;
    // An upscaler still runs in an orthographic view (the render half's resolve has no ortho
    // term), unjittered.
    const bool fsrActive  = t.primary && plan.fsr && !fv.camera.orthographic;
    const bool dlssActive = t.primary && plan.dlssWanted && !fv.camera.orthographic &&
        (!plan.rr || (m_dlssPass && m_dlssPass->IsContextValid()));
    // A raw render target in the main view (the viewport's Buffer menu, Render.target) or a
    // wireframe / lighting / GBuffer debug view shows what was rasterized this frame, not a
    // temporally resolved image, so sub-pixel jitter would make it crawl by up to a pixel
    // every frame. UpdateAAEnabled turns TAA off for the debug view modes, but DLSS and FSR
    // keep running, so the jitter is dropped here; their history re-converges within a few
    // frames of returning to the lit image.
    const bool stableView = m_debugTargetWanted.load(std::memory_order_acquire)
        || m_viewMode == ViewMode::Wireframe || IsLightingDebugView(m_viewMode) || IsGBufferDebugView(m_viewMode);
    const bool wantJitter = (taaEnabled || fsrActive || dlssActive) && !stableView;
    // Advance Halton even when an external upscaler is active: its query may fail on the
    // first frame, before its context exists, and Halton is a safe fallback. A successful
    // query overwrites it below.
    vs.jitter.Advance(wantJitter);
    if (fsrActive && m_fsr2Pass && wantJitter)
    {
        // FSR uses a ceil(8 * (displayW/renderW)^2)-phase Halton. Query the offset at the current
        // frame index; on failure (context not yet built on frame 0) keep the Halton value - a
        // one-frame mismatch is harmless because FSR resets its history on context creation.
        float jx = 0.0f, jy = 0.0f;
        if (m_fsr2Pass->QueryJitter(static_cast<uint32_t>(fv.currentFrame), jx, jy))
            vs.jitter.SetJitterPixelsDirect(jx, jy);
    }
    else if (dlssActive && m_dlssPass && wantJitter)
    {
        // Same idea, but the sequence is ours: Streamline publishes no jitter
        // generator, so DLSSPass owns the ratio-scaled Halton. Overriding the
        // shared jitter state here is what keeps the projection matrix built
        // below and the value handed to DLSS identical by construction.
        float jx = 0.0f, jy = 0.0f;
        if (m_dlssPass->QueryJitter(static_cast<uint32_t>(fv.currentFrame), jx, jy))
            vs.jitter.SetJitterPixelsDirect(jx, jy);
    }
    // Jitter is in pixels at RENDER res (this projection rasterizes into the HDR
    // target, which is render-sized). At Native render == display.
    XMMATRIX proj             = vs.jitter.ApplyJitterToProjection(projBase, fv.renderWidth, fv.renderHeight);
    XMMATRIX viewProjJittered = XMMatrixMultiply(view, proj);

    // Streamline per-frame constants: uploaded once, here, right after the matrices they
    // describe are built and still on the main thread. DLSS, Frame Generation and Reflex all
    // read this block, so it is filled whenever a Streamline frame token exists, not only
    // when DLSS is the active upscaler.
    if (slFrameGame)
    {
        const XMMATRIX curVPNoJitter  = viewProjNoJitter;
        const XMMATRIX prevVPNoJitter = XMLoadFloat4x4(&vs.jitter.GetPrevViewProjNoJitter());

        SL::FrameConstants k{};
        // NOT transposed, unlike everything uploaded to HLSL: Streamline specifies row-major and
        // consumes the matrices on the CPU; the engine transposes only because HLSL packs
        // column-major by default.
        // VERIFY THIS FIRST if DLSS output smears or ghosts under camera rotation: a transposed
        // reprojection matrix produces exactly that and nothing else in the pipeline complains.
        // The development-build DLSS overlay visualises the motion it derives - check it against
        // a slow horizontal pan before tuning anything else.
        XMStoreFloat4x4(&k.cameraViewToClip, projBase);
        XMStoreFloat4x4(&k.clipToCameraView, XMMatrixInverse(nullptr, projBase));
        XMStoreFloat4x4(&k.clipToPrevClip,
                        XMMatrixMultiply(XMMatrixInverse(nullptr, curVPNoJitter), prevVPNoJitter));
        XMStoreFloat4x4(&k.prevClipToClip,
                        XMMatrixMultiply(XMMatrixInverse(nullptr, prevVPNoJitter), curVPNoJitter));

        // Pixels at render res - the same value the projection above was jittered by, so the two
        // cannot disagree.
        k.jitterOffset = { vs.jitter.GetJitterX(), vs.jitter.GetJitterY() };

        // GBuffer3 stores the RAW NDC DELTA (curNDC - prevNDC). Streamline wants motion
        // normalised so a full-screen traversal is 1.0 ("values in [-1,1]"), pointing
        // CURRENT -> PREVIOUS. Three corrections:
        //
        //   magnitude: NDC spans 2.0 across the screen, UV spans 1.0  -> x0.5
        //   direction: engine stores cur-prev, DLSS wants prev-cur    -> negate
        //   y axis:    NDC +y is up, UV +y is down                    -> flip
        //
        // giving (-0.5, +0.5). Cross-checks: the FSR2 scale (-renderW/2, +renderH/2) divided by
        // the render extent (FSR wants the same thing in pixels), and XeGTAOTemporal's decoder of
        // the same buffer:
        //   uvDelta = float2(vel.x, -vel.y) * 0.5;  prevUV = currUV - uvDelta;
        // A wrong scale shows as whole-screen ghosting/smearing under any camera movement.
        k.mvecScale = { -0.5f, 0.5f };

        XMStoreFloat3(&k.cameraPos,   pos);
        XMStoreFloat3(&k.cameraFwd,   XMVector3Normalize(forward));
        XMStoreFloat3(&k.cameraUp,    XMVector3Normalize(up));
        XMStoreFloat3(&k.cameraRight, XMVector3Normalize(XMVector3Cross(up, forward)));

        k.cameraNear = fv.camera.nearZ;
        k.cameraFar  = fv.camera.farZ;
        k.cameraFOV  = fv.camera.fov;
        k.cameraAspectRatio = (fv.renderHeight > 0)
            ? static_cast<float>(fv.renderWidth) / static_cast<float>(fv.renderHeight)
            : 1.0f;

        k.depthInverted          = true;    // reversed-Z everywhere in this engine
        k.cameraMotionIncluded   = true;    // GBuffer velocity is a full clip delta
        k.motionVectors3D        = false;
        k.orthographicProjection = fv.camera.orthographic;
        // Read the pass's pending-reset flag rather than deriving it from
        // context validity. Deriving it inverts the meaning: it would assert
        // reset only on frames where DLSS does not run (and the flag is
        // discarded) and never on the frame after a hard cut, which is the one
        // frame it exists for.
        k.reset = m_dlssPass && m_dlssPass->IsResetPending();
        // The view's reset reasons raised for this frame so far (RaiseViewBreak) reach the
        // history only at the render half's start, after this upload, so the ones DLSS resets on
        // are read here from the frame's pending breaks.
        if (m_dlssPass && (m_pendingViewBreaks.load(std::memory_order_acquire) & DLSSPass::ViewResetReasons()))
            k.reset = true;
        // Camera.HardCutTo invalidates the frame's camera history without necessarily calling
        // InvalidateTemporalHistory (e.g. Lua/camera stack). Carry that discontinuity on the
        // matching Streamline frame token, for SR and RR alike. Do not use PT's accumulation
        // reset: projection jitter resets it every frame, whereas RR must retain history across
        // ordinary camera motion.
        if (m_dlssPass && !fv.camera.historyValid)
        {
            k.reset = true;
            if (plan.rr) LOG_INFO("[DLSS_RR] cameraHistoryReset=1");
        }
        // This frame's evaluate clears only a reset these constants carry: one asked for after
        // this upload (the render half's own hard cuts, an AA-mode switch) waits for the next
        // frame's constants.
        fv.dlssResetSent = k.reset;

        SL::StreamlineBackend::Get().SetConstants(k, slFrameGame);
    }

    PerViewCB cb{};
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                    XMMatrixTranspose(viewProjJittered));
    // Prev unjittered VP (transposed for row-vector HLSL).
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj),
                    XMMatrixTranspose(XMLoadFloat4x4(&vs.jitter.GetPrevViewProjNoJitter())));
    // Current UNJITTERED VP for velocity (real motion only, not TAA jitter).
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.curViewProjNoJitter),
                    XMMatrixTranspose(viewProjNoJitter));
    // Time + camera for custom vertex-shader hooks (custom_vertex.hlsli).
    cb.timeParams[0] = fv.globalTimeSec;
    cb.timeParams[1] = fv.worldDeltaTime;
    cb.timeParams[2] = std::sin(fv.globalTimeSec);
    cb.timeParams[3] = std::cos(fv.globalTimeSec);
    cb.cameraPosWS[0] = fv.camera.position.x;
    cb.cameraPosWS[1] = fv.camera.position.y;
    cb.cameraPosWS[2] = fv.camera.position.z;
    cb.cameraPosWS[3] = 0.0f;
    // Engine-global dissolve noise (bindless slot == handleId; ~0u = missing). The shader's
    // uniform branch also gates on dissolveParams.x > 0, so a failed texture disables the
    // effect instead of sampling garbage.
    cb.dissolveNoiseIdx = m_dissolveNoiseTex.IsValid()
                        ? m_dissolveNoiseTex.handleId
                        : ~0u;
    // The instance-remap heap index the target names (UploadFrameData says which).
    cb.instanceRemapIdx = t.instanceRemapIdx;
    // Temporal dither phase for BlendMode::Dithered (material_bindings.hlsli
    // gFrameIndex @232). The presented-frame counter, so it advances exactly
    // once per TAA history step.
    cb.frameIndex       = static_cast<uint32_t>(fv.currentFrame);
    *t.perView = cb;

    XMStoreFloat4x4(&fv.view.viewMatrix,              view);
    XMStoreFloat4x4(&fv.view.projMatrix,              proj);              // jittered
    XMStoreFloat4x4(&fv.view.viewProjMatrix,          viewProjJittered);  // jittered
    XMStoreFloat4x4(&fv.view.projMatrixNoJitter,      projBase);          // un-jittered (for ImGuizmo)
    XMStoreFloat4x4(&fv.view.viewProjMatrixNoJitter,  viewProjNoJitter);  // un-jittered (for ImGuizmo)
    XMStoreFloat3(&fv.view.cameraPosition,   pos);
    XMStoreFloat3(&fv.view.cameraForward,    forward);
    fv.view.nearZ = fv.camera.nearZ;
    fv.view.farZ  = fv.camera.farZ;

    // Feed scene time + camera to the shadow passes so custom-VS (WPO) casters deform
    // identically to the GBuffer in the CSM / spot / point shadow maps. Runs every frame
    // before any shadow pass executes (BeginFrame -> render). Also carries the dissolve noise
    // bindless index (cascade CB offset 224) so Shadow.ps clips dissolving casters in
    // lockstep with the GBuffer.
    if (t.primary)
    {
        const uint32_t dissolveNoiseIdx = m_dissolveNoiseTex.IsValid()
                                        ? m_dissolveNoiseTex.handleId : ~0u;
        // The shadow family gets the pristine per-frame remap index at CB offset 228, never the
        // working remap: the shadow CL is recorded into an earlier pool slot than the cull CL
        // (pool order == submit order), so at shadow-execute time the working buffer still holds
        // last frame's copy; the UPLOAD remap is written by this frame's SortAndEmit and needs no
        // GPU copy or state management. Full-range draws (custom-VS/terrain/fallback,
        // spot/point/per-object/hair/face) resolve exactly through it. The CSM retained group
        // draws are the one exception: they bind cascade CB bank two, where ShadowPass swaps byte
        // 228 for the cascade's own working remap (filled by the mode-2 cull dispatches recorded
        // at the head of the shadow CL itself: same-CL ordering, no cross-CL hazard).
        const uint32_t instRemapIdxPristine = RetainedInstancesActive()
            ? m_instanceRemapHeapIdx[m_gfx.GetUploadSlot()] : 0u;
        if (m_shadowPass)      m_shadowPass->SetCustomVsTime(fv.globalTimeSec, fv.worldDeltaTime, fv.view.cameraPosition, dissolveNoiseIdx, instRemapIdxPristine);
        if (m_spotShadowPass)  m_spotShadowPass->SetCustomVsTime(fv.globalTimeSec, fv.worldDeltaTime, fv.view.cameraPosition, dissolveNoiseIdx, instRemapIdxPristine);
        if (m_pointShadowPass) m_pointShadowPass->SetCustomVsTime(fv.globalTimeSec, fv.worldDeltaTime, fv.view.cameraPosition, dissolveNoiseIdx, instRemapIdxPristine);
        // Spot/Point honor castShadow + the shadow viewMask; pushed per frame so the compat flag
        // is runtime-flippable.
        if (m_spotShadowPass)  m_spotShadowPass->SetHonorShadowFlags(m_spotPointHonorShadowFlags);
        if (m_pointShadowPass) m_pointShadowPass->SetHonorShadowFlags(m_spotPointHonorShadowFlags);
        if (m_perObjShadowPass) m_perObjShadowPass->SetCustomVsTime(fv.globalTimeSec, fv.worldDeltaTime, fv.view.cameraPosition, dissolveNoiseIdx, instRemapIdxPristine);
        if (m_hairShadowPass)   m_hairShadowPass->SetCustomVsTime(fv.globalTimeSec, fv.worldDeltaTime, fv.view.cameraPosition, dissolveNoiseIdx, instRemapIdxPristine);
    }

    // BoundingFrustum extraction needs standard-Z (CreateFromMatrix hard-codes near -> 0,
    // far -> 1). The render projection is reversed-Z; build a std-Z copy only for the extractor.
    if (fv.camera.orthographic)
    {
        // DirectXCollision's BoundingFrustum is slope-based and cannot express a parallel (ortho)
        // volume. Build a CONSERVATIVE containing perspective cone instead: apex pulled `pull`
        // behind the camera so at every depth d >= 0 its half-extent (pull+d)*halfH/pull >= halfH
        // - slight over-inclusion (over-draw), never false culling. Editor-only path.
        const float aspect = static_cast<float>(fv.vpWidth) / static_cast<float>(fv.vpHeight);
        const float halfH  = std::max(fv.camera.orthoHalfHeight, 0.01f);
        const float pull   = std::max(fv.camera.farZ, 1.0f);
        const float fovY   = 2.0f * std::atan(halfH / pull);
        XMMATRIX stdProj = XMMatrixPerspectiveFovLH(
            fovY, aspect, std::max(pull * 0.01f, 0.05f), pull + fv.camera.farZ);
        DirectX::BoundingFrustum::CreateFromMatrix(fv.view.boundingFrustum, stdProj);

        const XMVECTOR apex = XMVectorSubtract(pos, XMVectorScale(forward, pull));
        XMMATRIX invApexView = XMMatrixInverse(nullptr,
            XMMatrixLookToLH(apex, forward, up));
        fv.view.boundingFrustum.Transform(fv.view.boundingFrustum, invApexView);
    }
    else
    {
        const float aspect = static_cast<float>(fv.vpWidth) / static_cast<float>(fv.vpHeight);
        XMMATRIX stdProj = XMMatrixPerspectiveFovLH(
            fv.camera.fov, aspect, fv.camera.nearZ, fv.camera.farZ);
        DirectX::BoundingFrustum::CreateFromMatrix(fv.view.boundingFrustum, stdProj);

        // Transform from view-space to world-space.
        XMMATRIX invView = XMMatrixInverse(nullptr, view);
        fv.view.boundingFrustum.Transform(fv.view.boundingFrustum, invView);

        // Inflate slightly to avoid edge-case culling of objects at frustum boundary.
        fv.view.boundingFrustum.Near   *= 0.9f;
        fv.view.boundingFrustum.Far    *= 1.1f;
        fv.view.boundingFrustum.TopSlope    *= 1.05f;
        fv.view.boundingFrustum.BottomSlope *= 1.05f;
        fv.view.boundingFrustum.LeftSlope   *= 1.05f;
        fv.view.boundingFrustum.RightSlope  *= 1.05f;
    }

    // Plane form of the unjittered view frustum, for the GPU cull, meshlet, grass and terrain
    // culling.
    fv.view.frustum = ExtractFrustumPlanes(fv.view.viewProjMatrixNoJitter);

    if (t.primary)
    {
        // Update outline pass with current near/far for depth linearization
        if (m_outlinePass)
        {
            m_outlinePass->nearZ = fv.camera.nearZ;
            m_outlinePass->farZ  = fv.camera.farZ;
            // "Outline Width (px)" means DISPLAY pixels. Both hull shaders convert it to an NDC offset
            // by dividing by these extents, so they must be the post-upscale size; the passes only see
            // the render size (gfx.GetRenderWidth() is the HDR target), which would thicken the rim by
            // the upscale ratio. Renderer is the only place that holds both numbers; they are equal
            // at native/DLAA.
            m_outlinePass->displayWidth  = fv.vpWidth;
            m_outlinePass->displayHeight = fv.vpHeight;
        }

        // Geometry-phase outline: mirror OutlinePass's authored look so the rim is identical
        // whichever pass draws it, and enforce mutual exclusion - both passes draw the SAME hull,
        // so running them together double-draws it. OutlinePass keeps sub-passes 2+3 (ObjectID +
        // screen-space + editor picking) either way; only its hull sub-pass steps aside.
        if (m_outlineGeometryPass && m_outlinePass)
        {
            m_outlineGeometryPass->nearZ            = fv.camera.nearZ;
            m_outlineGeometryPass->farZ             = fv.camera.farZ;
            m_outlineGeometryPass->outlinePixels    = m_outlinePass->outlinePixels;
            m_outlineGeometryPass->outlineColor[0]  = m_outlinePass->outlineColor[0];
            m_outlineGeometryPass->outlineColor[1]  = m_outlinePass->outlineColor[1];
            m_outlineGeometryPass->outlineColor[2]  = m_outlinePass->outlineColor[2];
            m_outlineGeometryPass->outlineFadeStart = m_outlinePass->outlineFadeStart;
            m_outlineGeometryPass->outlineFadeEnd   = m_outlinePass->outlineFadeEnd;
            m_outlineGeometryPass->displayWidth     = fv.vpWidth;
            m_outlineGeometryPass->displayHeight    = fv.vpHeight;

            // Not diagSkipHull: that field is the editor's diagnostic checkbox, and two writers would
            // overwrite each other every frame.
            m_outlinePass->hullSuppressed = m_outlineGeometryPass->enabled;
            // The geometry rim writes depth and velocity, so TAA needs no special case for it: drop
            // the stencil tag so the outline path in TAA.cs.hlsl goes inert.
            m_outlinePass->tagStencilForTAA = !m_outlineGeometryPass->enabled;
        }
    }

    // LightCB camera + invViewProj (transposed for HLSL row-vector mul); cache for TAACB.
    if (t.light)
    {
        auto* lb = t.light;
        lb->cameraPos[0] = fv.camera.position.x;
        lb->cameraPos[1] = fv.camera.position.y;
        lb->cameraPos[2] = fv.camera.position.z;
        // The cascade-selection camera, written beside the shading camera with the same value, so
        // the main view's cascade choice equals dot(worldPos - cameraPos, cameraForward). Only a
        // secondary view (which borrows the cascades) lets the two differ.
        lb->csmCamPos[0] = fv.camera.position.x;
        lb->csmCamPos[1] = fv.camera.position.y;
        lb->csmCamPos[2] = fv.camera.position.z;
        {
            // global Lit/Unlit/Wireframe switch; the meshlet views reach
            // Lighting.ps as BaseColor (their colour is the GBuffer albedo).
            const ViewMode vm = m_viewMode.load(std::memory_order_relaxed);
            lb->viewMode = IsMeshletDebugView(vm) ? (uint32_t)ViewMode::BaseColor : (uint32_t)vm;
        }

        // Bindless fold of the AutoExposure buffer, so a FORWARD pixel shader can read the live
        // scene exposure. Deferred surfaces do not need it (NPRStabilizePass counter-scales them
        // from the same buffer afterwards), but a forward draw composites after NPRLitSnapshot,
        // outside that pass's reach. A CPU readback of the exposure value would be frames stale;
        // the buffer costs no root parameter and no stall.
        // Same ~0u clamp and 0-means-unavailable convention as reflProbeBufIdx: an out-of-range
        // ResourceDescriptorHeap[] read removes the device, and heap index 0 is always
        // addressable. A typed handle (StructuredBuffer<float>, stride 4); without the pass it is
        // the stride-4 null, the fixed BINDLESS_NULL_STRUCTURED the forward gate (BindlessNone)
        // knows.
        lb->exposureBufIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
            m_autoExposurePass ? m_gfx.GetBufferSRVGpuHandle(m_autoExposurePass->GetExposureBuffer()) : 0ull,
            "AutoExposure buffer", 4u).Bits();

        // This frame's face frame records: the buffer the emit wrote for this upload slot, read by
        // the deferred toon wherever a material's record carries kMatGpuFlagFrameRecord. A typed
        // handle (stride 16), made every frame from the slot's buffer; a buffer that failed to
        // create hands over the FIXED structured null - the stride-16 null is not a fixed slot,
        // and the toon gates on BindlessNone, which sees only the fixed ones.
        {
            const RHI::GPUBuffer& rec = m_matFrameRecord[t.slot];
            lb->faceFrameIdx = rec.IsValid()
                ? m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(m_gfx.GetBufferSRVGpuHandle(rec), "face frame records", 16u).Bits()
                : BINDLESS_NULL_STRUCTURED;
        }

        XMMATRIX invVP = XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjJittered));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(lb->invViewProj), invVP);

        vs.jitter.CommitFrame(viewProjNoJitter, viewProjJittered, invVP);
    }

    // Cascade shadow matrices.
    if (t.cascades && t.light)
    {
        auto* lb = t.light;

        t.cascades->Update(CascadeInput(fv, view, lb->lightDir));

        if (t.cascades->HasValidLight())
        {
            for (int i = 0; i < ShadowSystem::kCascadeCount; ++i)
            {
                std::memcpy(lb->shadowMatrix[i],
                            &t.cascades->CascadeMatricesTransposed()[i],
                            sizeof(DirectX::XMFLOAT4X4));
                lb->cascadeSplits[i] = t.cascades->CascadeSplits()[i];
            }
            lb->shadowBias         = ShadowSystem::ShadowBias();
            lb->shadowStrength     = ShadowSystem::ShadowStrength();
            lb->cameraForward[0]   = fv.view.cameraForward.x;
            lb->cameraForward[1]   = fv.view.cameraForward.y;
            lb->cameraForward[2]   = fv.view.cameraForward.z;
            // Written only inside this HasValidLight block, like cameraForward, so the pair is
            // exactly as stale (or fresh) as cameraForward and the main view's cascade choice is
            // bit-identical (see csmCamPos above).
            lb->csmCamFwd[0]       = fv.view.cameraForward.x;
            lb->csmCamFwd[1]       = fv.view.cameraForward.y;
            lb->csmCamFwd[2]       = fv.view.cameraForward.z;
            lb->shadowMapTexelSize = 1.0f / static_cast<float>(
                m_shadowPass ? m_shadowPass->GetShadowResolution() : ShadowPass::kShadowMapSize);
            lb->shadowBlendRange   = t.cascades->ShadowBlendRange();
            // Bit 31 selects the hashed PCF rotation (shadow.hlsli ShadowRotationAngle) while
            // temporal AA or an upscaler resolves it; without one, the 4-phase rotation, which
            // scrolls one row per frame, is used. A secondary view keeps the main view's phase (its
            // LightCB is a copy of the main view's this frame): its own cascades' index counts from
            // when it was made.
            if (t.primary)
                lb->shadowFrameIndex = (t.cascades->ShadowFrameIndex() & 0x7FFFFFFFu) | (wantJitter ? 0x80000000u : 0u);

            const float* texelWorld = t.cascades->CascadeTexelWorldSize();
            for (int i = 0; i < ShadowSystem::kCascadeCount; ++i)
                lb->cascadeTexelWorldSize[i] = texelWorld[i];
            lb->shadowNormalOffset = ShadowSystem::ShadowNormalOffset();
        }
    }
}

ShadowSystem::FrameInput Renderer::CascadeInput(const FrameView& fv, const XMMATRIX& view,
                                                const float lightDir[3]) const
{
    ShadowSystem::FrameInput fi{};
    fi.view           = view;
    fi.cameraForward  = fv.view.cameraForward;
    fi.lightDir       = { lightDir[0], lightDir[1], lightDir[2] };
    fi.nearZ          = fv.camera.nearZ;
    fi.farZ           = fv.camera.farZ;
    fi.fov            = fv.camera.fov;
    fi.aspect         = (fv.vpHeight > 0)
                        ? static_cast<float>(fv.vpWidth) / static_cast<float>(fv.vpHeight)
                        : 1.0f;
    fi.shadowMapSize  = m_shadowPass ? m_shadowPass->GetShadowResolution()
                                     : ShadowPass::kShadowMapSize;
    fi.orthographic    = fv.camera.orthographic;
    fi.orthoHalfHeight = fv.camera.orthoHalfHeight;
    // shadows.distance; the default is ShadowSystem's far cap.
    static_assert(kDefaultShadowDistance == ShadowSystem::kShadowFarCap,
                  "Renderer::kDefaultShadowDistance must stay the CSM's historic far cap");
    fi.shadowDistance  = m_shadowDistance;
    return fi;
}

void Renderer::FitViewShadowRetention(const float lightDir[3])
{
    m_viewShadowFitAny = false;
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        ShadowFrustumCompute& vol = m_viewShadowFit[slot];
        const SecondaryFrameView& s = FrameViewWrite().secondary[slot - 1];
        if (!lightDir || s.slot != slot || s.vpWidth == 0 || s.vpHeight == 0)
        {
            vol = ShadowFrustumCompute{};
            continue;
        }
        // The view's frame as its segment builds it (BeginViewSegment): the main one with its camera and
        // sizes. Its cascades are fitted the same way there, from the same camera and light.
        FrameView fv = FrameViewWrite();
        fv.camera = s.camera;
        fv.vpWidth  = fv.renderWidth  = s.vpWidth;
        fv.vpHeight = fv.renderHeight = s.vpHeight;
        XMVECTOR pos, forward, up;
        BuildCameraBasisFor(fv, pos, forward, up);
        XMStoreFloat3(&fv.view.cameraForward, forward);
        XMMATRIX view, proj;
        BuildCameraViewProjFor(fv, view, proj);
        ShadowSystem fit;
        fit.Update(CascadeInput(fv, view, lightDir));
        m_viewShadowFitAny |= vol.Compute(fit);
    }
}

bool Renderer::ShadowRetains(const BoundingBox& bb) const
{
    if (m_shadowFrustum.IsValid() && m_shadowFrustum.IntersectsAny(bb)) return true;
    if (!m_viewShadowRetention) return false;
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        if (m_viewShadowFrustum[slot].IsValid() && m_viewShadowFrustum[slot].IntersectsAny(bb)) return true;
    return false;
}

void Renderer::UploadFrameData(FrameIndex /*frame*/, uint32_t vpW, uint32_t vpH)
{
    ZoneScopedN("UploadFrameData");
    if (vpW == 0 || vpH == 0)  return;
    auto* pvCB = m_perObjectCB.Upload(m_gfx);
    if (!pvCB)                  return;

    FrameViewWrite().vpWidth  = vpW;
    FrameViewWrite().vpHeight = vpH;

    // App has already sized the HDR target from display size, FSR mode and the optional
    // render-scale setting. Read that authoritative size back rather than recomputing it
    // from the display viewport, so picking, jitter and post effects agree with the actual
    // render targets when display.renderScale != 1.
    FrameViewWrite().renderWidth  = m_gfx.GetRenderWidth();
    FrameViewWrite().renderHeight = m_gfx.GetRenderHeight();
    if (FrameViewWrite().renderWidth == 0 || FrameViewWrite().renderHeight == 0)
        ComputeRenderResolution(vpW, vpH, FrameViewWrite().renderWidth, FrameViewWrite().renderHeight);
    FrameViewWrite().editorOverlays = m_mainEditorOverlays;

    ViewFrameTarget main{ FrameViewWrite(), m_mainView, m_gamePlan };
    main.perView  = pvCB;
    main.light    = m_lightCB.Upload(m_gfx);
    main.cascades = m_shadowSystem.get();
    main.slot     = m_gfx.GetUploadSlot();
    main.primary  = true;
    // This frame's instance-remap heap index (0 = identity -> the VSes fall back to
    // draw-local indexing). When this frame's SortAndEmit latched GPU compaction, the main
    // view resolves through the working remap instead: its lower half is a GPU-copied
    // pristine mapping (full-count draws: picking, transparent, outline) and its upper half
    // holds the CS-compacted survivor ranges the working indirect args point at. Every other
    // b1 consumer (shadow cascade/caster/face CBs, probe capture, fringe/hair mark) keeps the
    // pristine UPLOAD remap index: their draws always use full ranges.
    main.instanceRemapIdx = RetainedInstancesActive()
        ? (SceneEmitWrite().gpuCompactionFrameActive
               ? m_instanceRemapGPUHeapIdx
               : m_instanceRemapHeapIdx[m_gfx.GetUploadSlot()])
        : 0u;
    BuildViewFrame(main);
    // The secondary views' cascades, for the casters the next frame's classify keeps (the main
    // view's retention also reads this frame's fit then).
    FitViewShadowRetention(main.light ? main.light->lightDir : nullptr);

    // Per-object (player) high-res directional shadow: a reversed-Z ortho light VP fitted to
    // the player's merged world AABB (computed in BuildRenderScene) along the sun direction,
    // mirroring the ShadowSystem cascade construction. Writes LightCB.perObjShadow* and hands
    // the raw VP to PerObjectShadowPass. Lighting.ps composites it over CSM as
    // shadow = min(csm, perObject); Spot/Point sampling is untouched.
    if (m_lightCB.Upload(m_gfx))
    {
        auto* lb = m_lightCB.Upload(m_gfx);
        bool built = false;
        const bool enable = m_perObjectShadowEnabled && m_hasPlayerShadowCaster
                          && m_shadowSystem && m_shadowSystem->HasValidLight();
        // The [PerObjShadow] line in BuildScene_ComputePlayerShadowSet reports the SET, not this
        // gate: a healthy set can still be switched off here by a missing/degenerate sun. Log
        // which of the four terms it was.
        {
            static int s_e = -1;
            const int e = enable ? 1 : 0;
            if (e != s_e) {
                LOG_INFO("[PerObjShadow1] enable=%d (toggle=%d hasCaster=%d shadowSys=%d validLight=%d)",
                         e, m_perObjectShadowEnabled ? 1 : 0, m_hasPlayerShadowCaster ? 1 : 0,
                         m_shadowSystem ? 1 : 0,
                         (m_shadowSystem && m_shadowSystem->HasValidLight()) ? 1 : 0);
                s_e = e;
            }
        }
        if (enable)
        {
            XMVECTOR lo = XMLoadFloat3(&m_playerShadowAabbMin);
            XMVECTOR hi = XMLoadFloat3(&m_playerShadowAabbMax);
            XMVECTOR c  = XMVectorScale(XMVectorAdd(lo, hi), 0.5f);
            // Bounding-sphere radius of the AABB (rotation-invariant fit), with a
            // small margin for pose extremes and a floor for degenerate AABBs.
            float radius = 0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo)));
            radius = std::max(radius, 0.25f) * 1.1f;

            XMVECTOR lightDirV = XMVectorSet(lb->lightDir[0], lb->lightDir[1], lb->lightDir[2], 0.0f);
            const float lightLen2 = XMVectorGetX(XMVector3LengthSq(lightDirV));

            // Defensive: a non-finite radius (an empty/sentinel AABB) or a degenerate sun direction
            // would trip XMMatrixLook* asserts and produce a garbage matrix; the gate below then falls
            // through to disabled. Log the fit gate and any material change in the fit radius: a
            // growing radius looks like "the shadow slowly faded away" - the ortho fit is sized to the
            // merged caster AABB, so an inflated AABB (a bad bone AABB, a stale WorldAabb, a caster
            // that should not be in the set) spreads the same 4096 map over a larger area and shrinks
            // the character's shadow toward sub-pixel. Nothing else reports it, and the set-level line
            // cannot see it.
            {
                static int   s_ok  = -1;
                static float s_rad = 0.f;
                const int ok = (std::isfinite(radius) && radius > 1e-4f
                             && std::isfinite(lightLen2) && lightLen2 > 1e-8f) ? 1 : 0;
                const bool grew = std::isfinite(radius)
                               && (radius > s_rad * 1.25f || radius < s_rad * 0.8f);
                (void)0;
                if (ok != s_ok || grew) {
                    LOG_INFO("[PerObjShadow2] fitOk=%d radius=%.4f (was %.4f) "
                             "lightDir=(%.3f,%.3f,%.3f) aabb=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                             ok, radius, s_rad,
                             lb->lightDir[0], lb->lightDir[1], lb->lightDir[2],
                             m_playerShadowAabbMin.x, m_playerShadowAabbMin.y, m_playerShadowAabbMin.z,
                             m_playerShadowAabbMax.x, m_playerShadowAabbMax.y, m_playerShadowAabbMax.z);
                    s_ok = ok; s_rad = radius;
                }
            }
            // Backstop: the caster-distance guard in BuildScene_ComputePlayerShadowSet should keep the
            // fit sane, but a 4096x4096 map stretched past this radius is worthless whatever produced
            // it (the character lands on a fraction of a texel). Refuse (warning once); Lighting.ps
            // composites shadow = min(csm, perObject), so dropping out degrades cleanly to CSM.
            constexpr float kMaxFitRadius = 300.0f;
            if (std::isfinite(radius) && radius > kMaxFitRadius)
            {
                static bool s_warnedFit = false;
                if (!s_warnedFit)
                {
                    s_warnedFit = true;
                    LOG_WARNING("PerObjectShadow: fit radius %.1f m exceeds %.0f m — disabling "
                                "the per-object map this frame (CSM still covers it). The caster "
                                "set spans (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f); something in it is "
                                "almost certainly out of place.",
                                radius, kMaxFitRadius,
                                m_playerShadowAabbMin.x, m_playerShadowAabbMin.y, m_playerShadowAabbMin.z,
                                m_playerShadowAabbMax.x, m_playerShadowAabbMax.y, m_playerShadowAabbMax.z);
                }
                radius = 0.0f;   // falls the gate below through to "disabled"
            }

            if (std::isfinite(radius) && radius > 1e-4f && std::isfinite(lightLen2) && lightLen2 > 1e-8f)
            {
                XMVECTOR lightDir = XMVector3Normalize(lightDirV); // from-light (photon travel) dir
                XMFLOAT3 L; XMStoreFloat3(&L, lightDir);

                // Extend the fit along the light's GROUND projection so the character's CAST shadow (much
                // longer than the character at grazing sun) fits inside the map instead of being clipped
                // at the frustum edge (a visible "split"). Shadow length ~ vertical extent x
                // horizontal/|vertical|, capped so a near-horizon sun does not blow the map up to
                // uselessly low resolution.
                const float horiz  = std::sqrt(L.x * L.x + L.z * L.z);
                const float vert   = std::max(std::fabs(L.y), 0.15f);
                const float spread = std::min((2.0f * radius) * (horiz / vert), radius * 8.0f);
                // x1.35 margin: extra ground coverage around the character, and keeps the up-sun edge AND
                // the shadow tip inside the shader's 8% soft-fade border (else the fade would nibble the
                // character's own self-shadow).
                const float fitR   = (radius + spread * 0.5f) * 1.35f;
                XMVECTOR shadowDir = (horiz > 1e-4f)
                    ? XMVector3Normalize(XMVectorSet(L.x, 0.0f, L.z, 0.0f))
                    : XMVectorZero();
                XMVECTOR fitC = XMVectorAdd(c, XMVectorScale(shadowDir, spread * 0.5f));

                // Modest pullback keeps D32 depth precision tight around the player
                // (vs ShadowSystem's 300 m, sized for whole-scene cascades).
                const float pullback = fitR;
                XMVECTOR eye = XMVectorSubtract(fitC, XMVectorScale(lightDir, fitR + pullback));

                XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
                if (std::fabs(L.y) > 0.999f)
                    up = XMVectorSet(1.f, 0.f, 0.f, 0.f);

                // LookToLH with the explicit normalized direction: the shadow cam looks ALONG +lightDir
                // toward fitC. Avoids LookAtLH's (focus - eye) subtraction, which cancels to zero for
                // large player coords.
                XMMATRIX view = XMMatrixLookToLH(eye, lightDir, up);
                XMMATRIX proj = XMMatrixOrthographicLH(
                    2.0f * fitR, 2.0f * fitR,
                    2.0f * fitR + pullback * 2.0f, 0.0f); // reversed Z (near/far swapped)
                XMMATRIX vp   = XMMatrixMultiply(view, proj);

                XMFLOAT4X4 vpT;
                XMStoreFloat4x4(&vpT, XMMatrixTranspose(vp));
                std::memcpy(lb->perObjShadowMatrix, &vpT, sizeof(XMFLOAT4X4));

                const float mapSize    = static_cast<float>(PerObjectShadowPass::kShadowMapSize);
                const float texelWorld = (2.0f * fitR) / mapSize;
                lb->perObjShadowParams[0] = 1.0f;        // enabled
                lb->perObjShadowParams[1] = texelWorld;  // world m/texel -> normal-offset scaling
                lb->perObjShadowParams[2] = 0.0008f;     // receiver bias (NDC; reversed-Z -> toward lit)
                lb->perObjShadowParams[3] = 2.0f;        // normal-offset strength (texels)
                lb->perObjShadowTexelSize = 1.0f / mapSize;

                if (m_perObjShadowPass)
                {
                    XMFLOAT4X4 vpRaw;
                    XMStoreFloat4x4(&vpRaw, vp);
                    m_perObjShadowPass->SetShadowMatrix(vpRaw);
                    m_perObjShadowPass->SetEnabled(true);
                }
                built = true;
            }
        }
        if (!built)
        {
            lb->perObjShadowParams[0] = 0.0f;        // disabled -> shader skips the sample
            if (m_perObjShadowPass) m_perObjShadowPass->SetEnabled(false);
        }
        // Clean separation: when the per-object map renders the character this frame, exclude it
        // from the CSM (no caster in both maps -> min() cannot double-darken). When the per-object
        // map is inactive, the character falls back to CSM casting so it never loses its
        // directional shadow.
        if (m_shadowPass) m_shadowPass->SetExcludePerObjectCasters(built);
        // The retained shadow cull applies the same exclusion in the CS
        // (kInstanceFlagPerObjShadow); Render() reads this mirror when it records the per-cascade
        // dispatches, after this decision is final.
        SceneEmitWrite().perObjCsmExcludeFrame = built;
    }

    // Hair (bangs -> face) directional shadow: the same construction as the per-object block,
    // fitted to the much smaller merged bangs AABB but without the ground-projection spread -
    // the receiver is the face right behind the bangs along the light, not a ground plane, so
    // a plain footprint around the head keeps texel density maximal. Consumed only by NPR
    // face pixels (Lighting.ps SampleHairShadow @ t43); never composited into the scene-wide
    // shadow factor, and the bangs keep casting into CSM / the player map (no exclusion here).
    if (m_lightCB.Upload(m_gfx))
    {
        auto* lb = m_lightCB.Upload(m_gfx);
        bool built = false;
        // mode 0 = screen mark only -> the fitted map stays off (the mark pass below is the
        // source); 1/2 include the map.
        const bool enable = m_hairShadowEnabled && m_hasHairShadowCaster
                          && (m_hairShadowMode != 0)
                          && m_hairShadowPass
                          && m_shadowSystem && m_shadowSystem->HasValidLight();
        if (enable)
        {
            XMVECTOR lo = XMLoadFloat3(&m_hairShadowAabbMin);
            XMVECTOR hi = XMLoadFloat3(&m_hairShadowAabbMax);
            XMVECTOR c  = XMVectorScale(XMVectorAdd(lo, hi), 0.5f);
            float radius = 0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo)));
            const float casterScale = std::max(m_hairShadowWorldScale, 1.0e-4f);
            radius = std::max(radius, 0.05f * casterScale) * 1.1f;

            XMVECTOR lightDirV = XMVectorSet(lb->lightDir[0], lb->lightDir[1], lb->lightDir[2], 0.0f);
            const float lightLen2 = XMVectorGetX(XMVector3LengthSq(lightDirV));

            if (std::isfinite(radius) && radius > 1e-4f && std::isfinite(lightLen2) && lightLen2 > 1e-8f)
            {
                XMVECTOR lightDir = XMVector3Normalize(lightDirV); // from-light (photon travel) dir
                XMFLOAT3 L; XMStoreFloat3(&L, lightDir);

                // x2.5: the footprint must cover where the bangs shadow LANDS on the face (below/behind
                // the fringe, sliding sideways with the light), not just the fringe itself. fitScale = the
                // component's per-character lever when the default clips or wastes texels.
                const float fitR = std::max(radius * 2.5f, 0.15f * casterScale)
                                 * m_hairShadowFitScale;

                // Depth slab: pullback behind the bangs + reach past the whole head so every face
                // receiver lands inside [0,1] (outside -> the shader returns lit, and the shadow would
                // silently vanish).
                const float pullback = fitR;
                XMVECTOR eye = XMVectorSubtract(c, XMVectorScale(lightDir, fitR + pullback));

                XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
                if (std::fabs(L.y) > 0.999f)
                    up = XMVectorSet(1.f, 0.f, 0.f, 0.f);

                XMMATRIX view = XMMatrixLookToLH(eye, lightDir, up);
                XMMATRIX proj = XMMatrixOrthographicLH(
                    2.0f * fitR, 2.0f * fitR,
                    2.0f * fitR + pullback * 2.0f, 0.0f); // reversed Z (near/far swapped)
                XMMATRIX vp   = XMMatrixMultiply(view, proj);

                XMFLOAT4X4 vpT;
                XMStoreFloat4x4(&vpT, XMMatrixTranspose(vp));
                std::memcpy(lb->hairShadowMatrix, &vpT, sizeof(XMFLOAT4X4));

                const float mapSize    = static_cast<float>(PerObjectShadowPass::kHairShadowMapSize);
                const float texelWorld = (2.0f * fitR) / mapSize;
                // Receiver bias derived from a fixed WORLD distance, not a fixed NDC value: NDC bias x
                // slab depth = world bias, and the slab (4 x fitR) scales with fitScale, so a fixed 0.0008
                // NDC would grow past the millimetre bangs -> forehead gap at large fits and erase the
                // shadow. 1.5 mm stays above D32 quantization noise and below the physical gap at any
                // slider setting.
                const float kHairBiasWorld = 0.0015f * casterScale;
                const float slabDepth      = 2.0f * fitR + pullback * 2.0f;
                lb->hairShadowParams[0] = 1.0f;        // enabled
                lb->hairShadowParams[1] = texelWorld;  // world m/texel -> normal-offset scaling
                lb->hairShadowParams[2] = kHairBiasWorld / slabDepth; // receiver bias (NDC; reversed-Z -> toward lit)
                lb->hairShadowParams[3] = 2.0f;        // normal-offset strength (texels)
                lb->hairShadowTexelSize = 1.0f / mapSize;
                lb->hairShadowStrength  = m_hairShadowStrength;

                XMFLOAT4X4 vpRaw;
                XMStoreFloat4x4(&vpRaw, vp);
                m_hairShadowPass->SetShadowMatrix(vpRaw);
                m_hairShadowPass->SetEnabled(true);
                built = true;
            }
        }
        if (!built)
        {
            lb->hairShadowParams[0] = 0.0f;        // disabled -> shader skips the sample
            if (m_hairShadowPass) m_hairShadowPass->SetEnabled(false);
        }
        // Strength is consumed by BOTH hair-shadow sources (map sample AND the
        // screen mark below), so upload it regardless of the map's built state.
        lb->hairShadowStrength = m_hairShadowStrength;
        // No SetExcludePerObjectCasters here: the bangs stay casters in CSM / the player map
        // (their body/ground shadow must not disappear).
    }

    // Hair screen-space fringe mark (a port of DanbaidongRP's CharacterFringe): proxy offsets
    // precomputed on the CPU (reference math, view-space metres) for HairShadowMarkPass.
    // mode 1 = shadow map only -> off.
    if (m_hairMarkPass)
    {
        bool markOn = m_hairShadowEnabled && m_hasHairShadowCaster
                   && (m_hairShadowMode != 1)
                   && m_shadowSystem && m_shadowSystem->HasValidLight()
                   && m_lightCB.Upload(m_gfx);
        if (markOn)
        {
            auto* lb = m_lightCB.Upload(m_gfx);
            XMVECTOR lightDirV = XMVectorSet(lb->lightDir[0], lb->lightDir[1], lb->lightDir[2], 0.0f);
            const float lightLen2 = XMVectorGetX(XMVector3LengthSq(lightDirV));
            if (std::isfinite(lightLen2) && lightLen2 > 1e-8f)
            {
                const XMVECTOR lightDir = XMVector3Normalize(lightDirV);
                const XMMATRIX view = XMLoadFloat4x4(&FrameViewWrite().view.viewMatrix);
                // Camera world axes = columns of the world->view matrix (row-vector convention: view rows
                // are the basis transposed).
                const auto& vm = FrameViewWrite().view.viewMatrix;
                const XMFLOAT3 camRight{ vm._11, vm._21, vm._31 };
                const XMFLOAT3 camUp   { vm._12, vm._22, vm._32 };
                const XMFLOAT3 camFwd  { vm._13, vm._23, vm._33 };
                // Light direction in view space; only X matters (the proxy shifts horizontally against
                // the light's screen direction).
                const float lightVSx = XMVectorGetX(XMVector3TransformNormal(lightDir, view));
                // Camera-above factor (reference camDirFactor): the downward
                // shift fades out as the camera rises above the head, keeping
                // the shadow hugging the face from top-down views.
                const XMVECTOR headC = XMVectorScale(
                    XMVectorAdd(XMLoadFloat3(&m_hairShadowAabbMin),
                                XMLoadFloat3(&m_hairShadowAabbMax)), 0.5f);
                const XMVECTOR camDir = XMVector3Normalize(
                    XMVectorSubtract(XMLoadFloat3(&FrameViewWrite().view.cameraPosition), headC));
                const float camY = XMVectorGetY(camDir);
                const float t = std::clamp((camY - 0.1f) / 0.8f, 0.0f, 1.0f);
                const float camDirFactor = 1.0f - t * t * (3.0f - 2.0f * t); // 1 - smoothstep(0.1,0.9,camY)

                m_hairMarkPass->SetFrameParams(
                    FrameViewWrite().view.viewProjMatrix,                       // jittered - matches GBuffer depth
                    camRight, camUp, FrameViewWrite().view.cameraPosition, camFwd,
                    -0.0045f * lightVSx * m_hairMarkOffsetX * m_hairShadowWorldScale,
                    -0.0075f * m_hairMarkOffsetY * camDirFactor * m_hairShadowWorldScale,
                     0.01f   * m_hairMarkDepthPush * m_hairShadowWorldScale,
                    m_dissolveNoiseTex.IsValid() ? m_dissolveNoiseTex.handleId : ~0u,
                    RetainedInstancesActive()
                        ? m_instanceRemapHeapIdx[m_gfx.GetUploadSlot()] : 0u);
                m_hairMarkPass->SetViewInputs(XMFLOAT3{ XMVectorGetX(lightDir), XMVectorGetY(lightDir),
                                                        XMVectorGetZ(lightDir) },
                                              XMFLOAT3{ XMVectorGetX(headC), XMVectorGetY(headC), XMVectorGetZ(headC) },
                                              m_hairMarkOffsetX, m_hairMarkOffsetY, m_hairShadowWorldScale);
                m_hairMarkPass->SetEnabled(true);
            }
            else
                markOn = false;
        }
        if (!markOn)
            m_hairMarkPass->SetEnabled(false);
    }

    // Face overlay (brows/lashes through hair).
    if (m_faceOverlayPass)
        m_faceOverlayPass->SetEnabled(m_hasFaceOverlay);
}
