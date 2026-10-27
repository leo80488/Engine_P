#include "RenderGraph/RenderPass/OutlineGeometryPass.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// ==== Root parameter slots (graphics root signature, the same map as OutlinePass) ====
// Values from Graphics/RootSignatureLayout.h. GfxRel:: = group-relative index (BindCB /
// BindConstantBuffer add the CBV base); Gfx:: = absolute root-parameter index
// (BindRootBufferSRV / BindDescriptorTableHandle / SetGraphicsRootConstant).
static constexpr auto kPerViewCBSlot   = RHI::Slots::GfxRel::CB_b1;   // -> b1 space0
static constexpr auto kOutlineCBSlot   = RHI::Slots::GfxRel::CB_b2;   // -> b2 space0
static constexpr auto kRootConstantsSlot = RHI::Slots::Gfx::RootConstants;
static constexpr auto kInstanceBufSlot = RHI::Slots::Gfx::InstanceBuf;
static constexpr auto kMeshDescSlot    = RHI::Slots::Gfx::MeshDesc;
static constexpr auto kBindlessSlot    = RHI::Slots::Gfx::BindlessBuffers;
static constexpr auto kBindlessTexSlot = RHI::Slots::Gfx::BindlessTex;  // t0 space2: dissolve noise

// Shading-model stencil code written for rim pixels. Matches the codes the
// lighting passes read (1=PBR, 2=NPR, 3=Unlit). Unlit is the honest answer: the
// rim's colour comes entirely from RT5's emissive seed and must never be shaded.
static constexpr uint8_t kUnlitStencilRef = ShadingStencil::kUnlit;

OutlineGeometryPass::OutlineGeometryPass(RG::RGTextureHandle albedo,
                                         RG::RGTextureHandle normal,
                                         RG::RGTextureHandle surface,
                                         RG::RGTextureHandle depth,
                                         RG::RGTextureHandle velocity,
                                         RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface)
    , m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{
}

void OutlineGeometryPass::Setup(RG::RenderGraphBuilder& b)
{
    b.WriteRenderTarget(m_albedo);
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteRenderTarget(m_emissive);
    // Depth written and the stencil's shading code stamped (RenderGraph/StencilBits.h);
    // -rgplaneoff keeps the plain depth-stencil write.
    if (RG::RenderGraph::PlanesEnabled())
        b.DepthStencil(m_depth, RG::Depth::Write, RG::Stencil::Write, ShadingStencil::kCodeMask);
    else
        b.WriteDepthStencil(m_depth);
    // Not SetColorTarget(HdrSceneColor): that binds AND clears, wiping the emissive GBufferPass
    // just wrote. The HDR scene colour is declared here (Phase A brings it to RENDER_TARGET without
    // binding or clearing it); Execute binds the full MRT set via SetRenderTargetsAndHdr, which
    // only binds.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void OutlineGeometryPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::OutlineGeometry_VS, RHI::ShaderStage::VS,
                         "OutlineGeometry.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::OutlineGeometry_PS, RHI::ShaderStage::PS,
                         "OutlineGeometry.ps.hlsl", "main");

    m_psoCache.Init(gfx, m_shaderLib);
    m_outlineCB.Create(gfx, "OutlineGeometry.CB");

    LOG_SUCCESS("OutlineGeometryPass: initialized");
}

void OutlineGeometryPass::WarmPipelines()
{
    (void)m_psoCache.GetOrCreate(BuildPSODesc(false));
}

PSODesc OutlineGeometryPass::BuildPSODesc(bool mirrored) const
{
    PSODesc desc{};
    desc.vsID = ShaderID::OutlineGeometry_VS;
    desc.psID = ShaderID::OutlineGeometry_PS;

    // Inverted hull: render BACK faces only, so the extruded shell shows only
    // where it pokes out past the silhouette.
    desc.rs.cullMode = RHI::CullMode::FRONT;
    // A mirrored instance (odd negative scale) reverses winding, so the front/back
    // convention has to flip with it or the shell renders inside-out. PSOCache
    // hashes this, so each variant mints its own PSO.
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.depthClipEnable       = true;

    // Unlike OutlinePass's hull, depth write is on, so TAA's disocclusion detector notices when the
    // rim leaves a texel. Safe because the width fade keeps the shell at 1-2 px, where the
    // silhouette error every other depth reader sees stays sub-pixel.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;  // reversed Z

    // Shading-model code, same contract as GBufferPass.
    desc.dss.stencilEnable     = true;
    desc.dss.stencilReadMask  = 0xFF;
    desc.dss.stencilWriteMask = ShadingStencil::kCodeMask;   // the shading code's bits only
    desc.dss.frontFace.stencilFunc    = RHI::ComparisonFunc::ALWAYS;
    desc.dss.frontFace.stencilPassOp = RHI::StencilOp::REPLACE;
    desc.dss.backFace = desc.dss.frontFace;

    // No blending: this is a GBuffer write, and the distance fade lives in the extrusion width so
    // none is needed.
    for (int i = 0; i < 6; ++i)
    {
        desc.bs.renderTarget[i].blendEnable = false;
        desc.bs.renderTarget[i].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    }

    // Must match GBufferPass::BuildPSODesc exactly: the encode is shared via MS_EncodeGBuffer, so a
    // format drift would silently corrupt the GBuffer.
    desc.rtvFormats[0] = RHI::Format::R11G11B10_FLOAT;      // albedo
    desc.rtvFormats[1] = RHI::Format::R16G16B16A16_UNORM;   // normal (+ matIdx)
    desc.rtvFormats[2] = RHI::Format::R8G8B8A8_UNORM;       // surface
    desc.rtvFormats[3] = RHI::Format::R16G16_FLOAT;         // velocity
    desc.rtvFormats[4] = RHI::Format::R16G16B16A16_FLOAT;   // extra
    desc.rtvFormats[5] = RHI::Format::R16G16B16A16_FLOAT;   // HdrSceneColor
    desc.rtvCount      = 6;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

RHI::CommandList OutlineGeometryPass::Execute(RHI::CommandList cl)
{
    if (!enabled) return cl;

    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Custom);
    if (draws.empty()) return cl;

    IGraphicsDevice& gfx = *m_gfxPtr;
    const uint32_t renderW = gfx.GetRenderWidth();
    const uint32_t renderH = gfx.GetRenderHeight();

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc(false));
    if (!pso || !pso->IsValid()) return cl;

    // The extrusion is authored in display pixels, so the shader divides by the display extents
    // (render extents would make the rim upscale-ratio thick: 2 px -> 4 px at DLSS Performance).
    // Until the Renderer pushes a display size the render extents are used, which agree at native
    // resolution.
    const uint32_t camDisplayW = m_viewCamera ? m_viewCamera->displayWidth  : displayWidth;
    const uint32_t camDisplayH = m_viewCamera ? m_viewCamera->displayHeight : displayHeight;
    const uint32_t pxSpaceW = camDisplayW ? camDisplayW : renderW;
    const uint32_t pxSpaceH = camDisplayH ? camDisplayH : renderH;

    if (auto* slot = m_outlineCB.Current(gfx))
    {
        OutlineCBData cb{};
        cb.outlinePixels    = outlinePixels;
        cb.outlineColor[0]  = outlineColor[0];
        cb.outlineColor[1]  = outlineColor[1];
        cb.outlineColor[2]  = outlineColor[2];
        cb.outlineFadeStart = outlineFadeStart;
        cb.outlinePxSpaceW  = pxSpaceW;
        cb.outlinePxSpaceH  = pxSpaceH;
        cb.outlineFadeEnd   = outlineFadeEnd;
        cb.nearZ            = m_viewCamera ? m_viewCamera->nearZ : nearZ;
        cb.farZ             = m_viewCamera ? m_viewCamera->farZ  : farZ;
        std::memcpy(slot, &cb, sizeof(cb));
    }

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();

    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
        cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
        cl.BindCB(kPerViewCBSlot, RG::RGSlot::PerView);
        cl.GetDevice().BindConstantBuffer(kOutlineCBSlot, m_outlineCB.CurrentBuffer(gfx), cl);
        if (cl.gfx)
        {
            if (const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle())
                cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
        }
    };

    // Bind only (no clear): GBufferPass already cleared and filled these.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive },
                              m_depth);

    // Viewport, scissor and topology are command-list state, not inherited from the previous pass.
    // Omitting them fails silently: the draws record, nothing errors, nothing rasterizes.
    cl.SetViewport(renderW, renderH);
    cl.SetScissorRect(renderW, renderH);
    cl.SetPrimitiveTopology();

    cl.SetPipelineState(*pso);
    bindGlobals();
    cl.GetDevice().SetStencilRef(kUnlitStencilRef, cl);

    int curMirrored = 0;   // the PSO bound above is the unmirrored variant
    for (const DrawPacket& dp : draws)
    {
        // The picking outline is a screen-space silhouette (OutlinePass sub-passes 2+3), never a hull:
        // drawing it here would put a solid blob into the GBuffer instead of a ring.
        if (dp.isPickingOutline) continue;

        const int wantMirrored = (dp.mirrored != 0) ? 1 : 0;
        if (wantMirrored != curMirrored)
        {
            const RHI::PipelineState* mp = m_psoCache.GetOrCreate(BuildPSODesc(wantMirrored != 0));
            if (mp && mp->IsValid())
            {
                cl.SetPipelineState(*mp);
                bindGlobals();
                cl.GetDevice().SetStencilRef(kUnlitStencilRef, cl);
                curMirrored = wantMirrored;
            }
        }

        cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
        // Root constant [3] carries prevPosElementBase here (the outline width rides
        // GPUInstanceData.dissolveEdgeColor.w), so the VS computes velocity with the same code path as
        // GBuffer.vs.
        gfx.SetGraphicsRootConstant(kRootConstantsSlot, dp.prevPosElementBase, 3, cl);
        cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
    }

    return cl;
}

void OutlineGeometryPass::BindView(ViewState& view)
{
    m_viewCamera = view.Slot() == 0 ? nullptr : &view.Data<ViewCamera>(this);
}

void OutlineGeometryPass::SetViewCamera(ViewState& view, float viewNearZ, float viewFarZ, uint32_t viewDisplayW,
                                        uint32_t viewDisplayH)
{
    if (view.Slot() == 0) return;
    ViewCamera& c   = view.Data<ViewCamera>(this);
    c.nearZ         = viewNearZ;
    c.farZ          = viewFarZ;
    c.displayWidth  = viewDisplayW;
    c.displayHeight = viewDisplayH;
}
