#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

using namespace DirectX;

// Engine-shared root signature slots (as WorldUIBillboardPass), from RHI::Slots
// (Graphics/RootSignatureLayout.h):
//   b1 space0 - PerView CB (GROUP-RELATIVE index, GfxRel::CB_b1)
//   t2 space0 - vertex ByteAddressBuffer (ABSOLUTE root index)
//   t0 space2 - bindless Texture2D[] table (ABSOLUTE root index)
// The linear-clamp sampler is the fixed SMP_LINEAR_CLAMP heap slot.
static constexpr auto kRootSlot_VertexSRV   = RHI::Slots::Gfx::SRV_t2;     // t2 space0
static constexpr auto kRootSlot_BindlessTex = RHI::Slots::Gfx::BindlessTex; // t0 space2 (engine-shared)

void DebugIconPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // Reuse the WorldUI billboard shaders: same vertex layout + root slots.
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::WorldUI_VS, RHI::ShaderStage::VS, "WorldUI.vs.hlsl");
    m_shaderLib.Register(ShaderID::WorldUI_PS, RHI::ShaderStage::PS, "WorldUI.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(IconVertex);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
            for (uint32_t v = 0; v < RHI::kMaxViews; ++v)
            {
                if (gfx.CreateBuffer(bd, m_vertexBuffer[i][v]))
                    m_vbMapped[i][v] = gfx.MapBuffer(m_vertexBuffer[i][v]);
                else
                    LOG_ERROR("DebugIconPass: VB[%u][%u] creation failed", i, v);
            }
    }

    m_cb.Create(gfx, "DebugIcon.CB");
    // No sampler to create: WorldUI.ps samples the fixed SMP_LINEAR_CLAMP heap slot.

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("DebugIconPass: initialized");
    else
        LOG_ERROR("DebugIconPass: PSO creation failed");
}

PSODesc DebugIconPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::WorldUI_VS;
    desc.psID        = ShaderID::WorldUI_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;

    // Always-on-top overlay: depth off so icons read clearly even behind geometry
    // (the "overlay" gizmo convention).
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;

    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable             = true;
    rt.srcBlend                = RHI::Blend::SRC_ALPHA;
    rt.destBlend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp                 = RHI::BlendOp::ADD;
    rt.srcBlendAlpha          = RHI::Blend::ONE;
    rt.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha           = RHI::BlendOp::ADD;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = m_targetFormat;   // the live composition format (SetTargetFormat)
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;
    return desc;
}

void DebugIconPass::AddIcon(const XMFLOAT3& worldPos, float halfSize,
                            uint32_t texIdx, uint32_t color)
{
    m_icons.push_back({ worldPos, halfSize, texIdx, color });
}

void DebugIconPass::Execute(RHI::CommandList cl,
                            const XMFLOAT4X4& viewProjMatrix,
                            const XMFLOAT4X4& viewMatrix,
                            const RHI::Texture* target,
                            RHI::ResourceState entryState,
                            uint32_t canvasW, uint32_t canvasH)
{
    // INVARIANT: every early-out MUST precede the first resource barrier (the transition
    // to RENDERTARGET), so the pass is either a clean no-op or a complete transition pair,
    // never a half-transition that desyncs the caller's render-target state tracker. Do
    // not mutate GPU state before the precondition checks.
    if (!enabled || m_icons.empty() || !m_gfx || !target || !target->IsValid()) return;
    if (canvasW == 0 || canvasH == 0) return;

    auto& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    const uint32_t viewSlot = RHI::CurrentRecordingView().slot;
    if (viewSlot >= RHI::kMaxViews || !m_vbMapped[frameSlot][viewSlot]) return;

    const PSODesc psoDesc = BuildPSODesc();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(psoDesc);
    if (!pso || !pso->IsValid()) return;

    // World-space camera axes = columns of the row-major DX view matrix.
    const float* vm = reinterpret_cast<const float*>(&viewMatrix);
    const XMFLOAT3 cameraRightWS{ vm[0], vm[4], vm[8]  };
    const XMFLOAT3 cameraUpWS   { vm[1], vm[5], vm[9]  };
    const XMFLOAT3 cameraFwdWS  { vm[2], vm[6], vm[10] };

    // Camera world position, reconstructed from the orthonormal view basis:
    // C = -(t_x*R + t_y*U + t_z*F), where t = view-space translation (vm[12..14]).
    const XMFLOAT3 cameraPosWS{
        -(vm[12] * cameraRightWS.x + vm[13] * cameraUpWS.x + vm[14] * cameraFwdWS.x),
        -(vm[12] * cameraRightWS.y + vm[13] * cameraUpWS.y + vm[14] * cameraFwdWS.y),
        -(vm[12] * cameraRightWS.z + vm[13] * cameraUpWS.z + vm[14] * cameraFwdWS.z) };
    const XMVECTOR cameraPos = XMLoadFloat3(&cameraPosWS);

    static thread_local std::vector<IconVertex> t_verts;
    t_verts.clear();
    t_verts.reserve(m_icons.size() * 6);

    const XMVECTOR right0 = XMLoadFloat3(&cameraRightWS);
    const XMVECTOR up0    = XMLoadFloat3(&cameraUpWS);

    // Distance clamp for constant-screen sizing: keeps very-near icons from
    // collapsing to zero and very-far ones from becoming enormous world quads.
    constexpr float kMinDist = 1.5f;
    constexpr float kMaxDist = 250.0f;

    for (const IconReq& ic : m_icons)
    {
        if (t_verts.size() + 6 > kMaxVertices) break;

        const XMVECTOR center = XMLoadFloat3(&ic.pos);
        const XMVECTOR offset = XMVectorSubtract(center, cameraPos);
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(offset));
        if (distanceSq > maxDistance * maxDistance ||
            XMVectorGetX(XMVector3Dot(offset, XMLoadFloat3(&cameraFwdWS))) <= 0.f)
            continue;

        float half = ic.halfSize;
        if (constantScreenSize)
        {
            // World half-extent that subtends a constant on-screen size:
            // half = distance x screenFraction (perspective -> constant pixels).
            float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(center, cameraPos)));
            dist = std::clamp(dist, kMinDist, kMaxDist);
            half = dist * ic.halfSize;
        }
        const XMVECTOR right  = XMVectorScale(right0, half);
        const XMVECTOR up     = XMVectorScale(up0,    half);

        XMFLOAT3 p[4];
        XMStoreFloat3(&p[0], XMVectorSubtract(XMVectorAdd(center, up), right));     // top-L
        XMStoreFloat3(&p[1], XMVectorAdd(XMVectorAdd(center, up), right));          // top-R
        XMStoreFloat3(&p[2], XMVectorAdd(XMVectorSubtract(center, up), right));     // bot-R
        XMStoreFloat3(&p[3], XMVectorSubtract(XMVectorSubtract(center, up), right));// bot-L

        auto pushV = [&](const XMFLOAT3& xyz, float u, float v) {
            IconVertex vv{};
            vv.pos[0] = xyz.x; vv.pos[1] = xyz.y; vv.pos[2] = xyz.z;
            vv.uv[0]  = u;     vv.uv[1]  = v;
            vv.col    = ic.col;
            vv.texIdx = ic.texIdx;
            t_verts.push_back(vv);
        };
        // CCW, matching WorldUI EmitQuad winding (PSO is cull-NONE anyway).
        pushV(p[0], 0.f, 0.f);
        pushV(p[1], 1.f, 0.f);
        pushV(p[2], 1.f, 1.f);
        pushV(p[0], 0.f, 0.f);
        pushV(p[2], 1.f, 1.f);
        pushV(p[3], 0.f, 1.f);
    }

    if (t_verts.empty()) return;

    std::memcpy(m_vbMapped[frameSlot][viewSlot], t_verts.data(), t_verts.size() * sizeof(IconVertex));

    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target, entryState, RHI::ResourceState::RENDERTARGET), cl);
    }

    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.topLeftX = 0; vp.topLeftY = 0;
    vp.width  = static_cast<float>(canvasW);
    vp.height = static_cast<float>(canvasH);
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    gfx.SetViewport(vp, cl);
    gfx.SetScissorRect(0, 0, canvasW, canvasH, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // Un-jittered viewProj into our b1 stub (engine stores CB matrices transposed).
    if (auto* slot = m_cb.Current(gfx))
    {
        XMMATRIX m = XMLoadFloat4x4(&viewProjMatrix);
        XMStoreFloat4x4(slot, XMMatrixTranspose(m));
    }
    gfx.BindConstantBuffer(RHI::Slots::GfxRel::CB_b1, m_cb.CurrentBuffer(gfx), cl); // b1 space0

    const uint64_t vbHandle = gfx.GetBufferSRVGpuHandle(m_vertexBuffer[frameSlot][viewSlot]);
    if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);

    if (const uint64_t bindlessTbl = gfx.GetBindlessTextureTableGpuHandle())
        gfx.BindDescriptorTableGpuHandle(kRootSlot_BindlessTex, bindlessTbl, cl);

    gfx.DrawInstanced(static_cast<uint32_t>(t_verts.size()), 1, 0, 0, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}

void DebugIconPass::ReloadShaders()
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
    m_psoCache.GetOrCreate(BuildPSODesc());
}
