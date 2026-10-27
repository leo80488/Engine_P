#include "RenderGraph/RenderPass/OutlinePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/RenderTypes.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// Stencil bit for "this pixel was painted by OutlinePass": sub-pass 1 (inverted hull) sets it
// on the rim pixels it writes to HDR, and TAA reads the stencil plane and reduces history weight
// there so the outline does not ghost behind moving meshes. It is the OutlineRim field of
// RenderGraph/StencilBits.h; the lighting passes test only the shading code's bits (kCodeMask).
static constexpr uint8_t kOutlineStencilBit = ShadingStencil::kOutlineRim;   // 0x80

// ==== Root parameter slots (graphics root signature) ====
// All values come from Graphics/RootSignatureLayout.h. RHI::Slots::GfxRel:: is a group-relative
// index (BindCB / BindConstantBuffer add the CBV base); RHI::Slots::Gfx:: is an absolute
// root-parameter index (BindRootBufferSRV / BindDescriptorTableHandle /
// SetGraphicsRootConstant). The two are not interchangeable.
static constexpr auto kPerViewCBSlot   = RHI::Slots::GfxRel::CB_b1;  // BindCB slot 0 -> b1 space0
static constexpr auto kOutlineCBSlot   = RHI::Slots::GfxRel::CB_b2;  // BindConstantBuffer slot 1 -> b2 space0
static constexpr auto kRootConstantsSlot = RHI::Slots::Gfx::RootConstants;
static constexpr auto kInstanceBufSlot = RHI::Slots::Gfx::InstanceBuf;
static constexpr auto kMeshDescSlot    = RHI::Slots::Gfx::MeshDesc;
static constexpr auto kBindlessSlot    = RHI::Slots::Gfx::BindlessBuffers;
static constexpr auto kBindlessTexSlot = RHI::Slots::Gfx::BindlessTex;  // t0 space2: dissolve noise fetch

// SRV descriptor-table slots of the screen-space PS (root params [10..13] -> t2..t5 space0).
static constexpr auto kObjectIDSrvSlot = RHI::Slots::Gfx::SRV_t2; // t2 space0
static constexpr auto kNormalSrvSlot   = RHI::Slots::Gfx::SRV_t3; // t3 space0
static constexpr auto kDepthSrvSlot    = RHI::Slots::Gfx::SRV_t5; // t5 space0

OutlinePass::OutlinePass(RG::RGTextureHandle depth, RG::RGTextureHandle normal)
    : m_depth(depth), m_normal(normal)
{}

OutlinePass::~OutlinePass()
{
    if (!m_gfxPtr) return;
    m_outlineCB.Destroy(*m_gfxPtr);
    if (m_mainObjectId.tex.IsValid()) m_gfxPtr->DestroyTexture(m_mainObjectId.tex);
}

void OutlinePass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
    {
        m_id         = &m_mainObjectId;
        m_viewCamera = nullptr;
        return;
    }
    ViewOutline& v = view.Data<ViewOutline>(this);
    m_id         = &v.objectId;
    m_viewCamera = &v.camera;
}

void OutlinePass::SetViewCamera(ViewState& view, float viewNearZ, float viewFarZ, uint32_t viewDisplayW,
                                uint32_t viewDisplayH)
{
    if (view.Slot() == 0) return;
    ViewCamera& c   = view.Data<ViewOutline>(this).camera;
    c.nearZ         = viewNearZ;
    c.farZ          = viewFarZ;
    c.displayWidth  = viewDisplayW;
    c.displayHeight = viewDisplayH;
}

void OutlinePass::ViewOutline::Release(IGraphicsDevice& gfx)
{
    if (objectId.tex.IsValid()) gfx.DestroyTexture(objectId.tex);
    objectId = ObjectId{};
}

void OutlinePass::Setup(RG::RenderGraphBuilder& b)
{
    // Keep normal in SHADER_RESOURCE so sub-pass 3 can sample it.
    b.ReadSRV(m_normal);
    // Depth is tested through the DSV (sub-passes 1 and 2) and sampled (sub-pass 3); sub-pass 1
    // writes the stencil's rim bit. One declaration per plane: the depth plane read twice in
    // DEPTH_READ_SRV (the read-only DSV's test and the SRV both take it), the stencil plane (the
    // OutlineRim field, RenderGraph/StencilBits.h) written in DEPTH_WRITE, and no barrier inside the
    // pass.
    //
    // -rgplaneoff: ReadSRV + WriteDepthStencil cannot say "a DSV and an SRV of one texture", so the
    // entry state is declared (DEPTHSTENCIL, emitted in Phase A with the tracker kept in step) and
    // Execute hand-rolls DEPTHSTENCIL -> SHADER_RESOURCE -> DEPTHSTENCIL around the composite.
    if (RG::RenderGraph::PlanesEnabled())
    {
        b.ReadSRV(m_depth);
        b.DepthStencil(m_depth, RG::Depth::Read, RG::Stencil::Write, ShadingStencil::kOutlineRim);
    }
    else
        b.ManualBarrier(m_depth, RHI::ResourceState::DEPTHSTENCIL);
    // The HDR scene colour: declared here (Phase A brings it to RENDER_TARGET without binding or
    // clearing it) and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void OutlinePass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    m_outlineCB.Destroy(gfx); // safe-guard in case Init is re-entered
    if (!m_outlineCB.Create(gfx, "OutlinePass.OutlineCB"))
        LOG_ERROR("OutlinePass: OutlineCB creation failed");

    // ObjectID texture, at the current render resolution.
    const uint32_t w = gfx.GetRenderWidth();
    const uint32_t h = gfx.GetRenderHeight();
    RebuildObjectIdTexture(gfx, w, h);

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,            RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::Lighting_VS,           RHI::ShaderStage::VS, "Lighting.vs.hlsl");
    m_shaderLib.Register(ShaderID::OutlineHull_VS,        RHI::ShaderStage::VS, "InvertedHull.vs.hlsl");
    m_shaderLib.Register(ShaderID::OutlineHull_PS,        RHI::ShaderStage::PS, "InvertedHull.ps.hlsl");
    m_shaderLib.Register(ShaderID::OutlineObjectID_PS,    RHI::ShaderStage::PS, "OutlineObjectID.ps.hlsl");
    m_shaderLib.Register(ShaderID::OutlineScreenSpace_PS, RHI::ShaderStage::PS, "OutlineScreenSpace.ps.hlsl");

    m_hullPsoCache.Init(gfx, m_shaderLib);
    m_objectIdPsoCache.Init(gfx, m_shaderLib);
    m_pickingObjectIdPsoCache.Init(gfx, m_shaderLib);
    m_screenSpacePsoCache.Init(gfx, m_shaderLib);

    if (!m_hullPsoCache.GetOrCreate(BuildHullPSODesc()))
        LOG_ERROR("OutlinePass: hull PSO creation failed");
    else
        LOG_INFO("OutlinePass: hull PSO ready");

    if (!m_objectIdPsoCache.GetOrCreate(BuildObjectIdPSODesc()))
        LOG_ERROR("OutlinePass: objectId PSO creation failed");
    else
        LOG_INFO("OutlinePass: objectId PSO ready");

    if (!m_pickingObjectIdPsoCache.GetOrCreate(BuildPickingObjectIdPSODesc()))
        LOG_ERROR("OutlinePass: picking objectId PSO creation failed");
    else
        LOG_INFO("OutlinePass: picking objectId PSO ready");

    if (!m_screenSpacePsoCache.GetOrCreate(BuildScreenSpacePSODesc()))
        LOG_ERROR("OutlinePass: screen-space PSO creation failed");
    else
        LOG_SUCCESS("OutlinePass: initialized (%ux%u)", w, h);
}

void OutlinePass::RebuildObjectIdTexture(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    ObjectId& id = *m_id;
    if (id.tex.IsValid()) gfx.DestroyTexture(id.tex);

    id.w     = 0;
    id.h     = 0;
    id.state = RHI::ResourceState::UNDEFINED;

    if (w == 0 || h == 0) return;

    RHI::TextureDesc td;
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R32_UINT;
    td.bindFlags = RHI::BindFlag::RENDER_TARGET | RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.mipLevels = 1;
    td.layout     = RHI::ResourceState::RENDERTARGET;  // start directly as RTV
    td.debugName = "OutlinePass.ObjectID";

    if (!gfx.CreateTexture(td, id.tex))
    {
        LOG_ERROR("OutlinePass: ObjectID texture creation failed (%ux%u)", w, h);
        return;
    }
    id.state = RHI::ResourceState::RENDERTARGET;
    id.w = w;
    id.h = h;
}

PSODesc OutlinePass::BuildHullPSODesc(bool mirrored) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::OutlineHull_VS;
    desc.psID        = ShaderID::OutlineHull_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cullMode         = RHI::CullMode::FRONT;
    // Mirrored instances rasterize with reversed winding. Flip the front-face
    // CONVENTION so FRONT still culls the near side and the inverted hull keeps
    // showing only the far side (that is what makes it read as an outline).
    // PSOCache hashes rs.frontCounterClockwise, so this mints its own PSO.
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.depthClipEnable = true;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    // Tag rim pixels with kOutlineStencilBit so TAA can identify them and weaken history blending.
    // The write mask touches only that bit, preserving the shading code and the TaaExclude bit; the
    // read mask is irrelevant (stencilFunc = ALWAYS).
    desc.dss.stencilEnable     = true;
    desc.dss.stencilReadMask  = 0xFF;
    desc.dss.stencilWriteMask = kOutlineStencilBit;
    desc.dss.frontFace.stencilFunc    = RHI::ComparisonFunc::ALWAYS;
    desc.dss.frontFace.stencilPassOp = RHI::StencilOp::REPLACE;
    desc.dss.backFace = desc.dss.frontFace;

    // Alpha blending: outline fades with distance to prevent dense-mesh
    // areas (hair) from becoming solid black at a distance.
    auto& rt0 = desc.bs.renderTarget[0];
    rt0.blendEnable           = true;
    rt0.srcBlend              = RHI::Blend::SRC_ALPHA;
    rt0.destBlend             = RHI::Blend::INV_SRC_ALPHA;
    rt0.blendOp               = RHI::BlendOp::ADD;
    rt0.srcBlendAlpha        = RHI::Blend::ONE;
    rt0.destBlendAlpha       = RHI::Blend::ZERO;
    rt0.blendOpAlpha         = RHI::BlendOp::ADD;
    rt0.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

PSODesc OutlinePass::BuildObjectIdPSODesc(bool mirrored) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::GBuffer_VS;
    desc.psID        = ShaderID::OutlineObjectID_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cullMode         = RHI::CullMode::BACK;
    desc.rs.frontCounterClockwise = mirrored;   // see BuildHullPSODesc
    desc.rs.depthClipEnable = true;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;  // GBuffer already wrote depth
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.rtvFormats[0] = RHI::Format::R32_UINT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// Picking ObjectID: BuildObjectIdPSODesc with the depth test off, so the selected entity's
// silhouette is recorded into the mask even where it is occluded. The PS sets bit 31 of the
// written ID so sub-pass 3 paints the edges in pickingOutlineColor (Blender / Unity selection
// look).
PSODesc OutlinePass::BuildPickingObjectIdPSODesc(bool mirrored) const
{
    PSODesc desc = BuildObjectIdPSODesc(mirrored);
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    return desc;
}

PSODesc OutlinePass::BuildScreenSpacePSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::Lighting_VS;
    desc.psID        = ShaderID::OutlineScreenSpace_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;

    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;

    auto& rt = desc.bs.renderTarget[0];
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rt.blendEnable             = true;
    rt.srcBlend                = RHI::Blend::SRC_ALPHA;
    rt.destBlend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp                 = RHI::BlendOp::ADD;
    rt.srcBlendAlpha          = RHI::Blend::ONE;
    rt.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha           = RHI::BlendOp::ADD;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;
    return desc;
}

RHI::CommandList OutlinePass::Execute(RHI::CommandList cl)
{
    DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Custom);
    if (draws.empty()) return cl;

    // Init made the main view's target; a secondary view's is made below, at its first Execute.
    if (!m_id->tex.IsValid() && !m_viewCamera) return cl;

    const RHI::PipelineState* hullPso          = m_hullPsoCache.GetOrCreate(BuildHullPSODesc());
    const RHI::PipelineState* objIdPso         = m_objectIdPsoCache.GetOrCreate(BuildObjectIdPSODesc());
    const RHI::PipelineState* pickingObjIdPso  = m_pickingObjectIdPsoCache.GetOrCreate(BuildPickingObjectIdPSODesc());
    const RHI::PipelineState* ssPso            = m_screenSpacePsoCache.GetOrCreate(BuildScreenSpacePSODesc());
    if (!hullPso || !objIdPso || !pickingObjIdPso || !ssPso) return cl;

    IGraphicsDevice& gfx = *m_gfxPtr;

    const RHI::Texture* depthTex  = cl.GetContext().GetTexture(m_depth);
    const RHI::Texture* normalTex = cl.GetContext().GetTexture(m_normal);
    if (!depthTex || !normalTex) return cl;

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
    const uint32_t renderW = gfx.GetRenderWidth();
    const uint32_t renderH = gfx.GetRenderHeight();

    if (renderW != m_id->w || renderH != m_id->h)
        RebuildObjectIdTexture(gfx, renderW, renderH);
    if (!m_id->tex.IsValid()) return cl;
    ObjectId& id = *m_id;
    const float    camNearZ    = m_viewCamera ? m_viewCamera->nearZ : nearZ;
    const float    camFarZ     = m_viewCamera ? m_viewCamera->farZ  : farZ;
    const uint32_t camDisplayW = m_viewCamera ? m_viewCamera->displayWidth  : displayWidth;
    const uint32_t camDisplayH = m_viewCamera ? m_viewCamera->displayHeight : displayHeight;

    if (auto* slot = m_outlineCB.Current(gfx))
    {
        OutlineCBData cb;
        cb.outlinePixels   = outlinePixels;
        cb.depthThreshold  = depthThreshold;
        cb.normalThreshold = normalThreshold;
        cb.outlineStrength = outlineStrength;
        cb.outlineColor[0] = outlineColor[0];
        cb.outlineColor[1] = outlineColor[1];
        cb.outlineColor[2] = outlineColor[2];
        cb.outlineFadeStart = outlineFadeStart;
        // Display extents, not renderW/H: the hull VS turns outlinePixels into an NDC offset by dividing
        // by these, so the render size would thicken the rim by the upscale ratio under FSR2/DLSS. Falls
        // back to the render size until the Renderer pushes one (identical at native).
        cb.outlinePxSpaceW  = camDisplayW ? camDisplayW : renderW;
        cb.outlinePxSpaceH  = camDisplayH ? camDisplayH : renderH;
        cb.outlineFadeEnd   = outlineFadeEnd;
        cb.nearZ = camNearZ;
        cb.farZ  = camFarZ;
        cb.pickingOutlineColor[0] = pickingOutlineColor[0];
        cb.pickingOutlineColor[1] = pickingOutlineColor[1];
        cb.pickingOutlineColor[2] = pickingOutlineColor[2];
        *slot = cb;
    }

    auto bindGeometryGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
        cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
        if (bindlessHandle)
            cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
        cl.BindCB(kPerViewCBSlot, RG::RGSlot::PerView);
        // Bindless texture table for the dissolve-noise fetch in
        // InvertedHull.ps / OutlineObjectID.ps (sampler-free Load).
        if (cl.gfx)
        {
            if (const uint64_t texTable = cl.gfx->GetBindlessTextureTableGpuHandle())
                cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
        }
    };

    // ---- Sub-pass 1: inverted hull -> HDR RTV + depth DSV (read-only depth test) ----
    // HDR starts in RENDER_TARGET; the DSV is the view the declaration set (depth read-only, stencil
    // writable).
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));
    cl.SetViewport(renderW, renderH);
    cl.SetScissorRect(renderW, renderH);
    cl.SetPrimitiveTopology();

    // hullSuppressed = OutlineGeometryPass owns the hull this frame.
    if (!diagSkipHull && !hullSuppressed)
    {
        cl.SetPipelineState(*hullPso);
        bindGeometryGlobals();
        cl.GetDevice().BindConstantBuffer(kOutlineCBSlot, m_outlineCB.CurrentBuffer(gfx), cl);
        // Packets are sorted with `mirrored` next to the permutation, so the PSO flips at most a couple
        // of times per frame.
        int curHullMirrored = 0;   // hullPso above is the unmirrored variant
        // The stencil ref carries kOutlineStencilBit into REPLACE writes (limited to that bit by the
        // PSO's stencilWriteMask), so every hull pixel that survives the depth test gets bit 7 in the
        // stencil plane. tagStencilForTAA == false makes this a plain inverted hull: ref 0 still
        // REPLACEs within the write mask, writing bit 7 as 0 and leaving the material code in bits 0..6
        // alone, which turns off every outline-specific path in TAA.cs.hlsl at once.
        const uint8_t hullStencilRef = tagStencilForTAA ? kOutlineStencilBit : 0u;
        cl.GetDevice().SetStencilRef(hullStencilRef, cl);

        for (const DrawPacket& dp : draws)
        {
            // The picking outline does not use the inverted hull: it is a pure screen-space silhouette
            // (sub-passes 2+3). A hull with depth disabled draws a solid blob instead of a thin ring, and a
            // depth-tested one is cut off at walls.
            if (dp.isPickingOutline) continue;
            const int wantMirrored = (dp.mirrored != 0) ? 1 : 0;
            if (wantMirrored != curHullMirrored)
            {
                const RHI::PipelineState* pso =
                    m_hullPsoCache.GetOrCreate(BuildHullPSODesc(wantMirrored != 0));
                if (pso && pso->IsValid())
                {
                    cl.SetPipelineState(*pso);
                    bindGeometryGlobals();
                    cl.GetDevice().BindConstantBuffer(kOutlineCBSlot,
                                                      m_outlineCB.CurrentBuffer(gfx), cl);
                    cl.GetDevice().SetStencilRef(hullStencilRef, cl);
                    curHullMirrored = wantMirrored;
                }
            }
            cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
            // Set outlinePixels as the 4th root constant (word offset 3) per draw.
            gfx.SetGraphicsRootConstant(
                kRootConstantsSlot,
                *reinterpret_cast<const uint32_t*>(&dp.outlinePixels), 3, cl);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    }

    if (!diagSkipScreenSpace)
    {
        // ---- Sub-pass 2: object ID -> R32_UINT RTV + depth DSV (depth test only) ----
        // The PSO writes no depth (GBuffer already did). ObjectID goes from its current state to
        // RENDERTARGET.
        if (id.state != RHI::ResourceState::RENDERTARGET)
        {
            cl.PushBarrier(RHI::GPUBarrier::Image(
                &id.tex,
                id.state,
                RHI::ResourceState::RENDERTARGET));
            id.state = RHI::ResourceState::RENDERTARGET;
        }

        const float zeroClear[4] = {};
        gfx.ClearRenderTarget(id.tex, zeroClear, cl);
        const RHI::Texture* rts[] = { &id.tex };
        gfx.SetRenderTargets(1, rts, depthTex, cl, cl.GetContext().DepthView(m_depth));   // the DSV view the declaration set

        cl.SetViewport(renderW, renderH);
        cl.SetScissorRect(renderW, renderH);
        cl.SetPrimitiveTopology();

        // Two variant dimensions, packed into one key so a flip in either rebinds: picking (depth test
        // off, so the silhouette shows through walls) and mirrored (front-face convention). The 4th root
        // constant (word offset 3) tells OutlineObjectID.ps.hlsl whether to OR bit 31 into the written
        // ID; sub-pass 3 keys the picking colour off that bit. -1 = nothing bound yet.
        int curObjIdVariant = -1;
        for (const DrawPacket& dp : draws)
        {
            if (!dp.screenSpaceOutline) continue;
            if (dp.isPickingOutline && !m_pickingOutlineShown) continue;
            const bool wantPicking  = dp.isPickingOutline;
            const bool wantMirrored = (dp.mirrored != 0);
            const int  wantVariant  = (wantPicking ? 1 : 0) | (wantMirrored ? 2 : 0);
            if (wantVariant != curObjIdVariant)
            {
                const RHI::PipelineState* pso =
                    wantMirrored
                        ? (wantPicking
                              ? m_pickingObjectIdPsoCache.GetOrCreate(BuildPickingObjectIdPSODesc(true))
                              : m_objectIdPsoCache.GetOrCreate(BuildObjectIdPSODesc(true)))
                        : (wantPicking ? pickingObjIdPso : objIdPso);
                if (!pso || !pso->IsValid()) continue;
                cl.SetPipelineState(*pso);
                bindGeometryGlobals();
                curObjIdVariant = wantVariant;
            }
            cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
            // Word offset 3 of the root-constants slot: picking-flag for the PS.
            const uint32_t pickingFlag = dp.isPickingOutline ? 1u : 0u;
            gfx.SetGraphicsRootConstant(kRootConstantsSlot, pickingFlag, 3, cl);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }

        // ---- Sub-pass 3: screen-space composite (fullscreen triangle, alpha blend) ----
        // ObjectID RENDERTARGET -> SHADER_RESOURCE. Depth: the graph already holds the depth plane in
        // DEPTH_READ_SRV; under -rgplaneoff it goes DEPTHSTENCIL -> SHADER_RESOURCE here and is restored
        // afterwards.
        cl.PushBarrier(RHI::GPUBarrier::Image(
            &id.tex,
            RHI::ResourceState::RENDERTARGET,
            RHI::ResourceState::SHADER_RESOURCE));
        id.state = RHI::ResourceState::SHADER_RESOURCE;

        const bool planes = RG::RenderGraph::PlanesEnabled();
        if (!planes)
            cl.PushBarrier(RHI::GPUBarrier::Image(
                depthTex,
                RHI::ResourceState::DEPTHSTENCIL,
                RHI::ResourceState::SHADER_RESOURCE));

        // Bind HDR RTV without depth (nullptr = no DSV).
        cl.GetDevice().SetRenderTargetToHdrWithDepth(nullptr, cl);

        cl.SetPipelineState(*ssPso);
        cl.BindDescriptorHeaps();
        cl.GetDevice().BindConstantBuffer(kOutlineCBSlot, m_outlineCB.CurrentBuffer(gfx), cl);

        const uint64_t objIdSrv  = gfx.GetTextureSRVGpuHandle(id.tex);
        const uint64_t normalSrv = gfx.GetTextureSRVGpuHandle(*normalTex);
        const uint64_t depthSrv  = gfx.GetTextureSRVGpuHandle(*depthTex);

        if (objIdSrv)  cl.BindDescriptorTableHandle(kObjectIDSrvSlot, objIdSrv);
        if (normalSrv) cl.BindDescriptorTableHandle(kNormalSrvSlot,   normalSrv);
        if (depthSrv)  cl.BindDescriptorTableHandle(kDepthSrvSlot,    depthSrv);

        cl.DrawFullscreenTriangle();

        // -rgplaneoff: restore depth to DEPTHSTENCIL. The graph's tracker records this pass as leaving
        // it there (ManualBarrier's DEPTHSTENCIL entry state), and the next transition (the post-graph
        // PreCompute, which reads that tracker) starts from it.
        if (!planes)
            cl.PushBarrier(RHI::GPUBarrier::Image(
                depthTex,
                RHI::ResourceState::SHADER_RESOURCE,
                RHI::ResourceState::DEPTHSTENCIL));
    }

    return cl;
}
