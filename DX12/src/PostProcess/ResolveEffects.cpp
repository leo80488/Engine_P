#include "PostProcess/ResolveEffects.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/NPRStabilizePass.h"
#include "RenderGraph/RenderPass/RRGuidePass.h"            // RRColorSnapshot
#include "RenderGraph/RenderPass/UpscalerReactiveMaskPass.h"
#include "RenderGraph/RenderPass/FSR2Pass.h"
#include "RenderGraph/RenderPass/DLSSPass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/FXAAPass.h"
#include "Renderer/FrameResources.h"
#include "Renderer/FrameView.h"
#include "System/Check.h"
#include "System/CommandLine.h"

#include <DirectXMath.h>

namespace PostProcess
{

namespace
{
    // An upscaler is selected this frame (FSR 2 or DLSS): TAA's reactive mask
    // is made for it, FXAA steps aside.
    bool UpscalerSelected(const Context& ctx) { return ctx.plan && (ctx.plan->fsr || ctx.plan->dlss); }
    // The plan resolves through an upscaler (its output is display-sized and
    // the post pool was made at that size): TAA and FXAA do not claim.
    bool Upscaled(const Context& ctx) { return ctx.plan && (ctx.plan->fsrResolved || ctx.plan->dlssResolved); }
    bool Orthographic(const Context& ctx) { return ctx.view && ctx.view->camera.orthographic; }
    // The plan's TAA stand-in: TAA resolves this frame for an upscaler whose
    // context is down, although the AA mode leaves the pass disabled.
    bool TAAStandIn(const Context& ctx) { return ctx.plan && ctx.plan->taaStandIn; }

    // A GPU region around one pass's Execute (RAII), under the pass's own name.
    struct Region
    {
        Context& ctx;
        uint32_t id;
        Region(Context& c, const char* name) : ctx(c), id(c.gfx->BeginGPUTimestamp(c.cl, name)) {}
        ~Region() { ctx.gfx->EndGPUTimestamp(ctx.cl, id); }
    };
}

// ==== NPRStabilize ====
bool NPRStabilizeEffect::IsEnabled(const Context& ctx) const
{
    return m_pass != nullptr && ctx.frame != nullptr;
}

void NPRStabilizeEffect::Execute(Context& ctx)
{
    // Stabilize the COMPLETE opaque HDR pixel before temporal history or an
    // upscaler sees it: GBuffer-seeded emissive and later graphics overlays included,
    // which Lighting.ps alone cannot observe. Do not bypass it under RR: a toon
    // surface's authored ACES input needs the rcp(exposure) counter-scale before ANY
    // resolve (bypassed, the tonemap's exposure multiply left toon surfaces at ~2% of
    // their DLAA brightness; tools/verify_rr_toon.py checks it). It writes a private
    // target: the HDR scene stays untouched, and the meter (Context::meterSrv) keeps
    // reading it.
    const auto& exposure = ctx.resolved->autoExposure;
    m_pass->SetInputs(ctx.hdrSrv, ctx.normalSrv, ctx.depthSrv, ctx.materialSrv,
                      ctx.resources.exposureBuffer, m_pass->GetLitSnapshotSrv());
    m_pass->SetSceneMaxBrightness(ctx.frame->nprMaxBrightness);
    m_pass->SetManualExposure(exposure.manualExposure, !exposure.enabled);
    bool stabilized = false;
    {
        Region r(ctx, "NPRStabilize");
        stabilized = m_pass->Execute(ctx.cl);
    }
    if (stabilized)
    {
        ctx.hdrSrv     = m_pass->GetOutputSrvHandle();
        ctx.hdrTex     = &m_pass->GetOutputTexture();
        ctx.stabilized = true;
    }
}

// ==== RRColorAlign ====
bool RRColorAlignEffect::IsEnabled(const Context& ctx) const
{
    return m_snapshot && ctx.stabilized && ctx.frame && ctx.frame->rrColorBeforeTransparency;
}

void RRColorAlignEffect::Execute(Context& ctx)
{
    // RR's input space: the colour before transparency gets the correction
    // NPRStabilize gave the frame (the scene before it, the stabilized after).
    Region r(ctx, "RRColorAlign");
    m_snapshot->Align(*ctx.gfx, ctx.cl, ctx.meterSrv, ctx.hdrSrv);
}

// ==== ReactiveMask ====
bool ReactiveMaskEffect::IsEnabled(const Context& ctx) const
{
    return UpscalerSelected(ctx) && m_pass && m_pass->Ready() && m_taa && ctx.view;
}

void ReactiveMaskEffect::Execute(Context& ctx)
{
    // The stencil bits TAA floors its history weight on (0x40 particles that
    // opted out of TAA, 0x80 the outline rim) with TAA's own weights; Prepare
    // allocated it at the frame boundary only while FSR 2 or DLSS SR runs.
    Region r(ctx, "ReactiveMask");
    ctx.resources.reactiveMask = m_pass->Execute(
        ctx.cl, *ctx.gfx, ctx.stencilSrv, ctx.view->renderWidth, ctx.view->renderHeight,
        m_taa->particleVfxMinAlpha, m_taa->outlineMinAlpha);
}

// ==== FSR 2 ====
bool FSR2Effect::IsEnabled(const Context& ctx) const
{
    return !ctx.resolveTaken && ctx.plan && ctx.plan->fsr && m_pass && m_pass->IsContextValid()
        && ctx.depthTex && ctx.velocityTex && ctx.view && ctx.frame;
}

void FSR2Effect::Execute(Context& ctx)
{
    ctx.resolveTaken = true;
    ENGINE_CHECK_ONCE(ctx.gfx->HdrScopeDepth() == 0,
                      "Renderer FSR2 HDR scene input: HDR access inside an "
                      "HDR push scope (depth=%d)", ctx.gfx->HdrScopeDepth());
    const FrameView& fv = *ctx.view;
    // The HDR, depth and velocity are compute-readable already (the pre-compute
    // list); FFX queries the actual D3D12 state, so those transitions are what make
    // the dispatch safe. camera.fov is the vertical FOV in radians - exactly FSR's
    // cameraFovAngleVertical.
    m_pass->SetCameraParams(fv.camera.nearZ, fv.camera.farZ, fv.camera.fov, fv.deltaTime * 1000.0f);
    m_pass->SetJitter(ctx.frame->jitter.x, ctx.frame->jitter.y);
    // Engine velocity is currentNDC - previousNDC. FSR adds its motion vector to
    // currentUV to find previousUV, so the scale to previousUV - currentUV is
    // (-renderW / 2, +renderH / 2).
    m_pass->SetMotionVectorScale(-static_cast<float>(fv.renderWidth)  * 0.5f,
                                  static_cast<float>(fv.renderHeight) * 0.5f);
    // hdrTex null = the HDR scene; FSR2Pass resolves them for FFX.
    m_pass->SetSrcTexturesComputeRead(ctx.hdrTex, ctx.depthTex, ctx.velocityTex);
    m_pass->SetReactiveMask(ctx.resources.reactiveMask);
    m_pass->SetExposureTexture(ctx.resources.exposureTex);
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -fsr2_keep_ffx_layout (the fsr2_bare_post pack's reverse arm): FFX's root
    // signature stays bound, and AutoExposure's t5 lands on it.
    static const bool s_keepFfxLayout = Sys::CommandLine::Get().Has("-fsr2_keep_ffx_layout");
    m_pass->restoreEngineLayout = !s_keepFfxLayout;
#endif
    Region r(ctx, "FSR2");
    m_pass->Execute(ctx.cl, *ctx.gfx);
}

bool FSR2Effect::ClaimsResolve(const Context& ctx, uint64_t& srv) const
{
    if (!m_pass || !ctx.plan || !ctx.plan->fsrResolved) return false;
    srv = m_pass->GetOutputSrvGpuHandle(*ctx.gfx);
    return true;
}

// ==== DLSS ====
bool DLSSEffect::IsEnabled(const Context& ctx) const
{
    return !ctx.resolveTaken && ctx.plan && ctx.plan->dlss && m_pass && m_pass->IsContextValid()
        && ctx.depthTex && ctx.velocityTex && ctx.view && ctx.frame;
}

void DLSSEffect::Execute(Context& ctx)
{
    ctx.resolveTaken = true;
    ENGINE_CHECK_ONCE(ctx.gfx->HdrScopeDepth() == 0,
                      "Renderer DLSS HDR scene input: HDR access inside an "
                      "HDR push scope (depth=%d)", ctx.gfx->HdrScopeDepth());
    const FrameView&      fv = *ctx.view;
    const FrameResources& fr = *ctx.frame;
    // Whether this frame's constants (UploadFrameData) carried a reset: the
    // evaluate clears only that one.
    m_pass->SetConstantsCarryReset(fv.dlssResetSent);
    // hdrTex null = the HDR scene; DLSSPass resolves them for Streamline.
    m_pass->SetSrcTexturesComputeRead(ctx.hdrTex, ctx.depthTex, ctx.velocityTex);
    m_pass->SetColorSrvIndex(ctx.gfx->DescriptorHeapIndexFromGpuHandle(ctx.hdrTex
        ? ctx.gfx->GetTextureSRVGpuHandle(*ctx.hdrTex) : ctx.gfx->GetHdrSceneSrvGpuHandle()));
    // DLSS SR's BiasCurrentColor hint (the pass tags it only for SR; null = no hint
    // this frame).
    m_pass->SetBiasCurrentColor(fr.rr ? nullptr : ctx.resources.reactiveMask);
    m_pass->SetExposureTexture(ctx.resources.exposureTex);
    if (fr.rrSetGuides)
    {
        // RRGuidePass's raster guides when this frame wrote them for DLSS (a raster
        // path, -rr_raster_guides=2); the path tracer's own otherwise - FrameResources
        // holds the choice.
        m_pass->SetRRGuides(fr.rrGuides[0], fr.rrGuides[1], fr.rrGuides[2], fr.rrGuides[3]);
        m_pass->SetRRDiffuseHitDistance(fr.rrDiffuseDistance);
        m_pass->SetRRColorBeforeTransparency(fr.rrColorBeforeTransparency);
        // DLSSD options are viewport state, not frame-token state. Set the matrices on
        // the render worker immediately before the matching evaluate, never from the game
        // half one frame ahead.
        DirectX::XMFLOAT4X4 inverseView;
        DirectX::XMStoreFloat4x4(&inverseView, DirectX::XMMatrixInverse(nullptr,
            DirectX::XMLoadFloat4x4(&fv.view.viewMatrix)));
        m_pass->UpdateRRView(fv.view.viewMatrix, inverseView);
    }
    if (!(fr.rr && Sys::CommandLine::Get().Has("-dlss_rr_raw")))
    {
        Region r(ctx, fr.rr ? "DLSS.RR" : "DLSS");
        m_pass->Execute(ctx.cl, *ctx.gfx, fr.slFrameToken);
    }
}

bool DLSSEffect::ClaimsResolve(const Context& ctx, uint64_t& srv) const
{
    if (!m_pass || !ctx.plan || !ctx.plan->dlssResolved || ctx.plan->fsrResolved) return false;
    srv = m_pass->GetOutputSrvGpuHandle(*ctx.gfx);
    return true;
}

bool DLSSEffect::RequiresGraphicsQueue(const Context& ctx) const
{
    // Streamline issues its own barriers for the tagged resources and some target
    // PIXEL_SHADER_RESOURCE (0x80) - ILLEGAL on a compute list: D3D12 errors the
    // list, Close() returns E_INVALIDARG, the frame drops (a black screen).
    // FidelityFX stays compute-legal; NVIDIA's own integrations run DLSS on the
    // direct queue. The whole list switches, not the order: NPRStabilize before the
    // upscaler and the post effects after it record as on every other path, without
    // the async overlap.
    return ctx.plan && ctx.plan->dlss;
}

// ==== TAA ====
bool TAAEffect::IsEnabled(const Context& ctx) const
{
    return !ctx.resolveTaken && m_pass && (m_pass->IsEnabled() || TAAStandIn(ctx)) && ctx.view && ctx.frame &&
           !Orthographic(ctx);
}

void TAAEffect::Execute(Context& ctx)
{
    using namespace DirectX;
    ctx.resolveTaken = true;
    const FrameView&      fv = *ctx.view;
    const FrameResources& fr = *ctx.frame;
    XMFLOAT4X4 prevVP;
    XMStoreFloat4x4(&prevVP, XMMatrixTranspose(XMLoadFloat4x4(&fr.prevViewProjNoJitter)));
    // Unjittered current inverse VP (transposed for row-vector HLSL). Lets TAA
    // synthesise jitter-free sky / cloud motion vectors - the skybox + volumetric
    // cloud composite write none, so without this clouds reproject with zero motion
    // and lag behind camera bob at low fps. Matches the GBuffer geometry-velocity
    // convention.
    XMFLOAT4X4 invVPNoJitter;
    XMStoreFloat4x4(&invVPNoJitter, XMMatrixTranspose(XMMatrixInverse(nullptr,
        XMLoadFloat4x4(&fr.viewProjNoJitter))));

    m_pass->SetHdrSrvHandle(ctx.hdrSrv);
    m_pass->SetDepthSrvHandle(ctx.depthSrv);
    m_pass->SetGBufferSrvHandle(ctx.surfaceSrv);
    m_pass->SetVelocitySrvHandle(ctx.velocitySrv);
    m_pass->SetOutlineStencilSrvHandle(ctx.stencilSrv);
    // TAA runs in the HDR domain at render resolution (= display at Native, 1.0x).
    m_pass->SetViewportSize(fv.renderWidth, fv.renderHeight);
    m_pass->SetFrameData(invVPNoJitter, prevVP, m_pass->historyWeight,
                         /*hasHistory=*/true, fv.deltaTime);
    m_pass->SetJitter(fr.jitter.x, fr.jitter.y);
    m_pass->SetStandIn(TAAStandIn(ctx));
    Region r(ctx, "TAA");
    m_pass->Execute(ctx.cl);
}

bool TAAEffect::ClaimsResolve(const Context& ctx, uint64_t& srv) const
{
    // Only ADOPT a non-zero handle: IsEnabled() and the CONFIG gate that owns the
    // buffers are deliberately not the same predicate (the gate is stickier), so they
    // can disagree for a frame. A 0 here would feed the post effects a null SRV;
    // falling through costs one un-antialiased frame. FSR or DLSS selected with its
    // context down is the plan's TAA stand-in: the history is prepared for it and TAA
    // claims.
    if (Upscaled(ctx) || !m_pass || !(m_pass->IsEnabled() || TAAStandIn(ctx)) || Orthographic(ctx)) return false;
    const uint64_t h = m_pass->GetResolvedSrvHandle();
    if (!h) return false;
    srv = h;
    return true;
}

// ==== FXAA ====
bool FXAAEffect::IsEnabled(const Context& ctx) const
{
    return !UpscalerSelected(ctx) && m_pass && m_pass->IsEnabled() && ctx.view;
}

void FXAAEffect::Execute(Context& ctx)
{
    // TAA's resolved buffer in FXAA+TAA mode, the AA input in FXAA-only mode.
    const uint64_t input = (m_taa && m_taa->IsEnabled() && !Orthographic(ctx))
        ? m_taa->GetResolvedSrvHandle() : ctx.hdrSrv;
    m_pass->SetInputSrvHandle(input);
    // FXAA runs on HDR pre-tonemap, at render resolution.
    m_pass->SetViewportSize(ctx.view->renderWidth, ctx.view->renderHeight);
    Region r(ctx, "FXAA");
    m_pass->Execute(ctx.cl);
}

bool FXAAEffect::ClaimsResolve(const Context& ctx, uint64_t& srv) const
{
    // Over TAA's when both run (FXAA+TAA); a live handle only, as TAA's.
    if (Upscaled(ctx) || !m_pass || !m_pass->IsEnabled()) return false;
    const uint64_t h = m_pass->GetResolvedSrvHandle();
    if (!h) return false;
    srv = h;
    return true;
}

} // namespace PostProcess
