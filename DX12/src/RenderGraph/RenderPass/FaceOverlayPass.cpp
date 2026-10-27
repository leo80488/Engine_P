#include "RenderGraph/RenderPass/FaceOverlayPass.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// Root parameter slots (RootSignatureLayout.h). Two index spaces are in play; do not
// mix them:
//   S::  = ABSOLUTE root-parameter index: SetGraphicsRoot32BitConstants,
//          BindRootBufferSRV, BindDescriptorTableHandle.
//   SR:: = GROUP-RELATIVE index: BindCB / BindConstantBufferAtOffset
//          (+CBVBase -> b1..b7), BindBufferSRV / BindSRVByHandle (+SRVBase -> t2..t5).
// Samplers: FaceOverlay.ps names fixed sampler-heap slots - SMP_LINEAR_WRAP for the
// lash base map and the face's ambient (the sky irradiance cube, the DDGI atlases),
// SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE for the CSM (GREATER_EQUAL: reverse-Z engine-wide;
// LESS_EQUAL would invert every test and shadow exactly the lit pixels). Nothing to
// create or bind.
namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

FaceOverlayPass::~FaceOverlayPass()
{
    if (!m_gfxPtr) return;
    m_cb.Destroy(*m_gfxPtr);
}

void FaceOverlayPass::Setup(RG::RenderGraphBuilder& b)
{
    // extra (RT4) and depth are SRV reads; both already sit in shader-read states after
    // LightingPass, so these are no-op transitions. Depth is also bound as a DSV with the
    // depth test off: declared (a plane the DSV does not use), so the bind takes the
    // read-only DSV while the depth plane sits in DEPTH_READ_SRV. Under -rgplaneoff it is
    // the one writable DSV, legal in DEPTH_READ_SRV because this PSO never writes depth
    // (D3D12's DEPTH_READ rules).
    b.ReadSRV(m_extraH);
    b.ReadSRV(m_depthH);
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depthH, RG::Depth::None);
    // SetColorTarget NONE, NOT BuiltinTexture::HdrSceneColor: that declaration makes the
    // graph bind-AND-CLEAR the HDR target before this pass (it is meant for the frame's
    // FIRST HDR writer, GBufferPass), wiping the lit scene to black every frame. Execute
    // binds HDR + depth (never written) itself (SetRenderTargetToHdrWithDepth), like
    // TransparentPass. The HDR scene colour is declared - Phase A brings it to
    // RENDER_TARGET, never binds or clears it - and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The CSM FaceOverlay.ps samples (bound in Execute; ShadowPass barriers it) - no
    // per-object map or RT mask, on purpose.
    b.ReadPassTextures({ RG::PassTex::kShadowCSM });
}

void FaceOverlayPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::FaceOverlay_VS, RHI::ShaderStage::VS, "FaceOverlay.vs.hlsl");
    m_shaderLib.Register(ShaderID::FaceOverlay_PS, RHI::ShaderStage::PS, "FaceOverlay.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PSODesc desc;
    desc.vsID        = ShaderID::FaceOverlay_VS;
    desc.psID        = ShaderID::FaceOverlay_PS;
    desc.inputLayout = InputLayoutType::None;   // PVF

    // Lash/brow cards are thin and frequently mirrored: never cull.
    desc.rs.cullMode = RHI::CullMode::NONE;

    // NO depth test (ZTest Always): the whole point is drawing where the hair occludes
    // the lash; the PS's hair-mask clip + depth-gap fade replace the test. A DSV is still
    // bound (see Setup), so dsvFormat must match.
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;

    // Straight alpha blend over the lit HDR scene (matches TransparentPass).
    auto& rt = desc.bs.renderTarget[0];
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rt.blendEnable    = true;
    rt.srcBlend       = RHI::Blend::SRC_ALPHA;
    rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp        = RHI::BlendOp::ADD;
    rt.srcBlendAlpha = RHI::Blend::ONE;
    rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha  = RHI::BlendOp::ADD;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;   // HdrSceneColor
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT; // GBuffer depth (bound, never written)

    if (!m_psoCache.GetOrCreate(desc))
        LOG_ERROR("FaceOverlayPass: PSO creation failed");

    if (!m_cb.Create(gfx, "FaceOverlayPass.CB"))
        LOG_ERROR("FaceOverlayPass: CB creation failed");

    // 1x1 fully-lit CSM stand-in; its shape must match `Texture2DArray<float>`:
    // R32_FLOAT, arraySize > 1 (arraySize 1 yields a TEXTURE2D SRV, a type mismatch
    // against Texture2DArray - UB under Resource Binding Tier 3, with no validation
    // message).
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1;
        td.height     = 1;
        td.arraySize = 4;   // Texture2DArray SRV needs > 1
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        // arraySize 4 + texel 0.0 are BOTH load-bearing (the same two rules as the
        // per-object and point-cube fallbacks in LightingPass::Init): CreateTexture emits a
        // TEXTURE2DARRAY SRV only when arraySize > 1. And under the reversed-Z GREATER_EQUAL
        // comparison sampler "fully lit" is depth 0.0 (the shadow passes' clear value, "no
        // occluder"); a 1.0 texel is an occluder at the near plane: FULLY SHADOWED.
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // ONE entry PER SUBRESOURCE: CreateTexture uploads
        // mipLevels * arraySize subresources and indexes this array;
        // a single-element init with arraySize > 1 walks off the stack.
        RHI::SubresourceData init[4];
        for (auto& sd : init) sd = { &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(td, m_shadowFallbackTex, init))
            m_shadowFallbackHandle = gfx.GetTextureSRVGpuHandle(m_shadowFallbackTex);
        else
            LOG_ERROR("FaceOverlayPass: CSM fallback texture failed");
    }

    LOG_SUCCESS("FaceOverlayPass: initialised (brows/lashes-through-hair overlay)");
}

void FaceOverlayPass::UploadCB(uint32_t rtW, uint32_t rtH)
{
    if (!m_gfxPtr) return;
    auto* slot = m_cb.Current(*m_gfxPtr);
    if (!slot) return;

    // Layout must match FaceOverlayCB in FaceOverlay.ps.hlsl.
    float data[4] = {
        rtW ? 1.0f / static_cast<float>(rtW) : 0.0f,
        rtH ? 1.0f / static_cast<float>(rtH) : 0.0f,
        0.0f,
        0.0f,
    };
    std::memcpy(slot->bytes, data, sizeof(data));
}

RHI::CommandList FaceOverlayPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled) return cl;

    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);

    PSODesc desc;
    desc.vsID        = ShaderID::FaceOverlay_VS;
    desc.psID        = ShaderID::FaceOverlay_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cullMode = RHI::CullMode::NONE;
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    auto& rt = desc.bs.renderTarget[0];
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rt.blendEnable    = true;
    rt.srcBlend       = RHI::Blend::SRC_ALPHA;
    rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp        = RHI::BlendOp::ADD;
    rt.srcBlendAlpha = RHI::Blend::ONE;
    rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha  = RHI::BlendOp::ADD;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return cl;

    UploadCB(cl.GetWidth(), cl.GetHeight());
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(*m_gfxPtr);
    if (!cbBuf.IsValid()) return cl;

    bool bound = false;
    auto drawList = [&](DrawList list)
    {
        for (const DrawPacket& dp : list)
        {
            if (!dp.faceOverlay) continue;

            if (!bound)
            {
                // HDR RTV + read-only depth DSV, as TransparentPass, so the
                // depth state (DEPTH_READ_SRV) stays coherent while the
                // PS Load()s the same depth as an SRV.
                const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depthH);
                cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl,
                    cl.GetContext().DepthView(m_depthH));   // the read-only DSV view (see Setup)
                cl.SetViewport();
                cl.SetScissorRect();
                cl.SetPrimitiveTopology();
                cl.SetPipelineState(*pso);
                cl.BindDescriptorHeaps();
                cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
                cl.BindRootBufferSRV(S::MeshDesc, RG::RGSlot::MeshDescriptors);
                if (const uint64_t bindless = cl.GetBindlessTableHandle())
                    cl.BindDescriptorTableHandle(S::BindlessBuffers, bindless);
                {
                    if (const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle())
                        cl.BindDescriptorTableHandle(S::BindlessTex, texTable);
                    // b3 space0: FaceOverlayCB (this pass).
                    cl.GetDevice().BindConstantBufferAtOffset(SR::CB_b3, cbBuf, 0, cl);
                }
                cl.BindCB(SR::CB_b1, RG::RGSlot::PerView);   // b1: GBuffer camera CB
                cl.BindCB(SR::CB_b2, RG::RGSlot::LightCB);   // b2
                if (const RHI::GPUBuffer* matBuf = cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer))
                    if (matBuf->IsValid()) cl.BindBufferSRV(SR::SRV_t2, *matBuf);   // t2
                cl.BindSRVByHandle(SR::SRV_t3, m_extraH);   // t3: extra.b hair mask
                cl.BindSRVByHandle(SR::SRV_t4, m_depthH);   // t4: scene depth
                // CSM: the overlay's direct sun term is shadow-gated, so the
                // shadow array is bound on every draw, never conditionally.
                {
                    const uint64_t shadowSrv = m_shadowSrvHandle ? m_shadowSrvHandle
                                                                 : m_shadowFallbackHandle;
                    if (shadowSrv) cl.BindDescriptorTableHandle(S::ShadowSRV, shadowSrv);
                }
                // The face's ambient: sky irradiance cube (t6), sky SH (t19).
                // Renderer hands the real handles or the shared black cube.
                if (m_iblIrradianceHandle) cl.BindDescriptorTableHandle(S::IBLIrradiance, m_iblIrradianceHandle);
                if (m_skySHHandle)         cl.BindDescriptorTableHandle(S::SkySH, m_skySHHandle);
                bound = true;
            }

            // FaceOverlay reuses the fourth PVF root DWORD (normally
            // prevPosInfo) for this packet's packed alpha/max-distance. Write
            // all four atomically so no stale per-draw value survives.
            const uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.faceOverlayParams
            };
            cl.GetDevice().SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    };
    drawList(opaqueDraws);
    drawList(shadowDraws);
    drawList(transparentDraws);

    return cl;
}
