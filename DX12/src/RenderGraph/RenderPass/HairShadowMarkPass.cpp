#include "RenderGraph/RenderPass/HairShadowMarkPass.h"
#include "Graphics/ShadingStencil.h"

#include "ECS/HierarchyComponents.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cstring>

using namespace DirectX;

// Root parameter slots - aliases of the shared graphics root signature
// (Graphics/RootSignatureLayout.h).
static constexpr auto kMarkCBSlot      = RHI::Slots::GfxRel::CB_b1;       // CB b1 space0 -> BindConstantBufferAtOffset index 0
static constexpr auto kInstanceBufSlot = RHI::Slots::Gfx::InstanceBuf;     // root SRV t0 space0
static constexpr auto kMeshDescSlot    = RHI::Slots::Gfx::MeshDesc;        // root SRV t1 space0
static constexpr auto kBindlessSlot    = RHI::Slots::Gfx::BindlessBuffers; // desc table bindless g_Buffers[] (PVF fetch)
static constexpr auto kBindlessTexSlot = RHI::Slots::Gfx::BindlessTex;     // t0 space2 - g_AllTextures[]

// GBuffer stencil ref for NPR pixels: the mark lands only on them. Keep the
// shared ShadingStencil value - a private copy that drifts makes this pass draw
// nothing, silently.
static constexpr uint32_t kStencilNPR = ShadingStencil::kNPR;

HairShadowMarkPass::~HairShadowMarkPass()
{
    if (!m_gfxPtr) return;
    m_cb.Destroy(*m_gfxPtr);
}

void HairShadowMarkPass::Setup(RG::RenderGraphBuilder& b)
{
    // The extra target stays an RTV (a masked write is appended) and the depth a
    // DSV (read-only test + stencil EQUAL) - the states the GBuffer producers leave
    // them in, so these declarations are no-op transitions in practice.
    b.WriteRenderTarget(m_extraH);
    // Depth tested and stencil compared: a read of both planes, the stencil's
    // shading-code bits (RenderGraph/StencilBits.h); with -rgplaneoff a plain
    // depth-stencil write. The handle form of SetRenderTargets binds the DSV the
    // declaration asks for.
    if (RG::RenderGraph::PlanesEnabled())
        b.DepthStencil(m_depthH, RG::Depth::Read, RG::Stencil::Read, ShadingStencil::kCodeMask);
    else                                  b.WriteDepthStencil(m_depthH);
}

void HairShadowMarkPass::Init(IGraphicsDevice& gfx)
{
    m_gfxPtr = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::FringeShadowMark_VS, RHI::ShaderStage::VS, "FringeShadowMark.vs.hlsl");
    m_shaderLib.Register(ShaderID::FringeShadowMark_PS, RHI::ShaderStage::PS, "FringeShadowMark.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    PSODesc desc;
    desc.vsID        = ShaderID::FringeShadowMark_VS;
    desc.psID        = ShaderID::FringeShadowMark_PS;
    desc.inputLayout = InputLayoutType::None;   // PVF

    // NONE cull: bangs hair cards are frequently flipped/double-sided; a
    // front-cull hole in the proxy would punch a hole in the shadow.
    desc.rs.cullMode = RHI::CullMode::NONE;

    // Depth: read-only GREATER_EQUAL (reversed-Z) against the GBuffer depth - the
    // depth-pushed proxy passes only where it still floats in FRONT of the visible
    // surface (the face behind the fringe), never on nearer geometry. Stencil: EQUAL
    // 2 (NPR), read-only - never touch PBR / Unlit pixels, whose RT4 may carry other
    // shading-model payloads.
    desc.dss.depthEnable      = true;
    desc.dss.depthWriteMask  = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc        = RHI::ComparisonFunc::GREATER_EQUAL;
    desc.dss.stencilEnable    = true;
    desc.dss.stencilReadMask = ShadingStencil::kCodeMask;   // the code's bits
    desc.dss.stencilWriteMask = 0x00;
    desc.dss.frontFace.stencilFunc    = RHI::ComparisonFunc::EQUAL;
    desc.dss.frontFace.stencilPassOp = RHI::StencilOp::KEEP;
    desc.dss.frontFace.stencilFailOp = RHI::StencilOp::KEEP;
    desc.dss.frontFace.stencilDepthFailOp = RHI::StencilOp::KEEP;
    desc.dss.backFace = desc.dss.frontFace;

    // Write ONLY the GREEN channel of RT4: extra.r (SDF terminator) and extra.a
    // (face marker), written by the face's GBuffer PS, must survive.
    desc.bs.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_GREEN;

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;   // GBuffer RT4 "extra"
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT; // GBuffer depth

    if (!m_psoCache.GetOrCreate(desc))
        LOG_ERROR("HairShadowMarkPass: PSO creation failed");

    if (!m_cb.Create(gfx, "HairShadowMarkPass.CB"))
        LOG_ERROR("HairShadowMarkPass: CB creation failed");

    LOG_SUCCESS("HairShadowMarkPass: initialised (RT4.g screen-space fringe shadow mark)");
}

void HairShadowMarkPass::BindView(ViewState& view)
{
    m_viewCamera = view.Slot() == 0 ? nullptr : &view.Data<ViewCamera>(this).camera;
}

// The main view's math (Renderer::UploadFrameData), on the view's camera.
void HairShadowMarkPass::SetViewCamera(ViewState& view, const XMFLOAT4X4& viewProj, const XMFLOAT4X4& viewMatrix,
                                       const XMFLOAT3& camPos)
{
    if (view.Slot() == 0) return;
    Camera& c = view.Data<ViewCamera>(this).camera;
    c.viewProj = viewProj;
    // Camera world axes = columns of the world->view matrix.
    c.camRight = { viewMatrix._11, viewMatrix._21, viewMatrix._31 };
    c.camUp    = { viewMatrix._12, viewMatrix._22, viewMatrix._32 };
    c.camFwd   = { viewMatrix._13, viewMatrix._23, viewMatrix._33 };
    c.camPos   = camPos;
    const float lightVSx = XMVectorGetX(XMVector3TransformNormal(XMLoadFloat3(&m_lightDir), XMLoadFloat4x4(&viewMatrix)));
    const XMVECTOR camDir = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&camPos), XMLoadFloat3(&m_headCenter)));
    const float camY = XMVectorGetY(camDir);
    const float t = std::clamp((camY - 0.1f) / 0.8f, 0.0f, 1.0f);
    const float camDirFactor = 1.0f - t * t * (3.0f - 2.0f * t);
    c.offsetX = -0.0045f * lightVSx * m_offsetXIn * m_worldScale;
    c.offsetY = -0.0075f * m_offsetYIn * camDirFactor * m_worldScale;
}

void HairShadowMarkPass::UploadCB()
{
    if (!m_gfxPtr) return;
    auto* slot = m_cb.Current(*m_gfxPtr);
    if (!slot) return;

    // The bound view's camera: a secondary view's own, else the main view's (SetFrameParams).
    const Camera mainCamera{ m_pendingVP, m_camRight, m_camUp, m_camPos, m_camFwd, m_offsetX, m_offsetY };
    const Camera& cam = m_viewCamera ? *m_viewCamera : mainCamera;

    // Layout must match FringeMarkCB in FringeShadowMark.vs.hlsl.
    XMMATRIX m = XMLoadFloat4x4(&cam.viewProj);
    XMFLOAT4X4 t;
    XMStoreFloat4x4(&t, XMMatrixTranspose(m));
    std::memcpy(slot->bytes, &t, sizeof(XMFLOAT4X4));

    float row0[4] = { cam.camRight.x, cam.camRight.y, cam.camRight.z, cam.offsetX };
    float row1[4] = { cam.camUp.x,    cam.camUp.y,    cam.camUp.z,    cam.offsetY };
    float row2[4] = { cam.camPos.x,   cam.camPos.y,   cam.camPos.z,   m_depthPush };
    float row3[4] = { cam.camFwd.x,   cam.camFwd.y,   cam.camFwd.z,   0.0f };
    std::memcpy(slot->bytes + 64,  row0, sizeof(row0));
    std::memcpy(slot->bytes + 80,  row1, sizeof(row1));
    std::memcpy(slot->bytes + 96,  row2, sizeof(row2));
    std::memcpy(slot->bytes + 112, row3, sizeof(row3));
    // Byte 124 (row3.w) = dissolve noise bindless index, read as uint by
    // FringeShadowMark.ps (dissolving bangs stop stamping face marks).
    std::memcpy(slot->bytes + 124, &m_dissolveNoiseIdx, sizeof(uint32_t));
    // Byte 128 = instance-remap heap index (FringeShadowMark.vs; 0 = identity).
    std::memcpy(slot->bytes + 128, &m_instanceRemapIdx, sizeof(uint32_t));
}

RHI::CommandList HairShadowMarkPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled) return cl;

    // The bangs packets live in the same lists the shadow passes scan.
    DrawList opaqueDraws      = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    DrawList shadowDraws      = cl.GetContext().GetDrawList(DrawFilter::Shadow);
    DrawList transparentDraws = cl.GetContext().GetDrawList(DrawFilter::Transparent);

    const RHI::PipelineState* pso = nullptr;
    {
        // Rebuild the desc Init built (PSOCache is keyed by the desc hash).
        PSODesc desc;
        desc.vsID        = ShaderID::FringeShadowMark_VS;
        desc.psID        = ShaderID::FringeShadowMark_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cullMode = RHI::CullMode::NONE;
        desc.dss.depthEnable      = true;
        desc.dss.depthWriteMask  = RHI::DepthWriteMask::ZERO;
        desc.dss.depthFunc        = RHI::ComparisonFunc::GREATER_EQUAL;
        desc.dss.stencilEnable    = true;
        desc.dss.stencilReadMask = ShadingStencil::kCodeMask;   // the code's bits
        desc.dss.stencilWriteMask = 0x00;
        desc.dss.frontFace.stencilFunc    = RHI::ComparisonFunc::EQUAL;
        desc.dss.frontFace.stencilPassOp = RHI::StencilOp::KEEP;
        desc.dss.frontFace.stencilFailOp = RHI::StencilOp::KEEP;
        desc.dss.frontFace.stencilDepthFailOp = RHI::StencilOp::KEEP;
        desc.dss.backFace = desc.dss.frontFace;
        desc.bs.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_GREEN;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
        pso = m_psoCache.GetOrCreate(desc);
    }
    if (!pso || !pso->IsValid()) return cl;

    UploadCB();
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(*m_gfxPtr);
    if (!cbBuf.IsValid()) return cl;

    bool bound = false;
    auto drawList = [&](DrawList list, bool transparentSrc)
    {
        for (const DrawPacket& dp : list)
        {
            if (!dp.hairShadowCaster) continue;
            // The artistic fringe mark is still a shadow-caster path. Respect
            // the same material/entity opt-out and shadow-view routing as CSM
            // and the fitted hair shadow map.
            if (!dp.castShadow) continue;
            if ((dp.viewMask & ViewBit::ShadowAny) == 0) continue;
            // Forward-translucent packets carry castShadow 0 (Renderer::
            // ApplyBlendMode) - bangs that should mark are Masked. Under
            // -shadow_translucent_casters pure alpha-blend packets still never
            // cast (matches the shadow passes) and alpha-TEST bangs cast a solid
            // silhouette (the reference marks the full fringe mesh too).
            if (transparentSrc && !dp.permutation.Has(PermutationKey::ALPHA_TEST))
                continue;

            if (!bound)
            {
                cl.SetRenderTargets({ m_extraH }, m_depthH);
                cl.SetViewport();
                cl.SetScissorRect();
                cl.SetPrimitiveTopology();
                cl.SetPipelineState(*pso);
                cl.BindDescriptorHeaps();
                cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
                cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
                if (const uint64_t bindless = cl.GetBindlessTableHandle())
                    cl.BindDescriptorTableHandle(kBindlessSlot, bindless);
                // Bindless texture table for the dissolve-noise fetch in
                // FringeShadowMark.ps (sampler-free Load).
                {
                    const uint64_t texTable = cl.GetDevice().GetBindlessTextureTableGpuHandle();
                    if (texTable)
                        cl.BindDescriptorTableHandle(kBindlessTexSlot, texTable);
                }
                cl.GetDevice().BindConstantBufferAtOffset(
                    kMarkCBSlot, cbBuf, 0, cl);
                cl.gfx->SetStencilRef(kStencilNPR, cl);
                bound = true;
            }

            // Custom-VS (WPO) bangs are drawn with the standard fetch too: the skinned
            // position stream is identical, only the WPO wobble is missing from the proxy,
            // which the soft mark hides.
            cl.SetPVFRootConstants(dp.meshDescriptorIndex, dp.instanceOffset, dp.materialIndex);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    };
    drawList(opaqueDraws,      false);
    drawList(shadowDraws,      false);
    drawList(transparentDraws, true);

    return cl;
}
