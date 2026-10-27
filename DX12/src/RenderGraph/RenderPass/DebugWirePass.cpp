#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include <cstring>

using namespace DirectX;

// Engine-shared root signature slots, from RHI::Slots (Graphics/RootSignatureLayout.h):
//   b1 space0 - our own DebugWireCB (GROUP-RELATIVE index, GfxRel::CB_b1)
//   t2 space0 - line vertex ByteAddressBuffer (ABSOLUTE root index)
//   t3 space0 - scene depth SRV for the manual occlusion test (ABSOLUTE)
static constexpr auto kRootSlot_VertexSRV = RHI::Slots::Gfx::SRV_t2; // t2 space0
static constexpr auto kRootSlot_DepthSRV  = RHI::Slots::Gfx::SRV_t3; // t3 space0

void DebugWirePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::DebugWire_VS, RHI::ShaderStage::VS, "DebugWire.vs.hlsl");
    m_shaderLib.Register(ShaderID::DebugWire_PS, RHI::ShaderStage::PS, "DebugWire.ps.hlsl");
    m_shaderLib.Register(ShaderID::DebugWireDepth_PS, RHI::ShaderStage::PS,
                         "DebugWireDepth.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    m_cb.Create(gfx, "DebugWire.CB");

    // The 108 MiB of UPLOAD rings is NOT created here but by EnsureRings(), which Clear()
    // calls on the first frame that builds debug wires.

    // Warm both variants: plain overlay + occlusion-fade gizmo.
    const bool okOverlay = m_psoCache.GetOrCreate(BuildPSODesc(false)) != nullptr;
    const bool okDepth   = m_psoCache.GetOrCreate(BuildPSODesc(true))  != nullptr;
    if (okOverlay && okDepth)
        LOG_SUCCESS("DebugWirePass: initialized");
    else
        LOG_ERROR("DebugWirePass: PSO creation failed");
}

void DebugWirePass::ReloadShaders()
{
    if (m_shaderLib.ClearCaches()) m_psoCache.Clear();
}

PSODesc DebugWirePass::BuildPSODesc(bool depthAware) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::DebugWire_VS;
    // Overlay variant: collision / navmesh / AABB wireframes must stay visible
    // through geometry. Depth-aware variant: same geometry, but the PS fades
    // pixels the scene depth buffer says are occluded.
    desc.psID        = depthAware ? ShaderID::DebugWireDepth_PS
                                  : ShaderID::DebugWire_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::LINELIST;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;

    // No DSV is bound at all: the target is the display-res tonemap output and
    // the depth buffer is render-res, which D3D12 cannot pair. Occlusion is a
    // pixel-shader comparison against a depth SRV instead.
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;

    // Alpha blend for semi-transparent lines.
    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable          = true;
    rt.srcBlend             = RHI::Blend::SRC_ALPHA;
    rt.destBlend            = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp              = RHI::BlendOp::ADD;
    rt.srcBlendAlpha       = RHI::Blend::ONE;
    rt.destBlendAlpha      = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha        = RHI::BlendOp::ADD;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = m_targetFormat;   // the live composition format (SetTargetFormat)
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;

    return desc;
}

void DebugWirePass::EnsureRings()
{
    if (m_ringsCreated || !m_gfx) return;
    m_ringsCreated = true;   // set first: a failed create must not retry every frame

    auto& gfx = *m_gfx;

    // UPLOAD heap vertex buffers (CPU-writable each frame), one per frame in flight so
    // the CPU's frame N+1 write does not stomp the GPU's frame N read. One buffer per
    // bucket: both draws start at vertex 0 and never rely on StartVertexLocation
    // reaching SV_VertexID.
    auto createRing = [&](uint32_t vertexCap, RHI::GPUBuffer (&bufs)[kFrameCount],
                          void* (&mapped)[kFrameCount])
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCap) * sizeof(LineVertex);
        bd.stride     = 0; // raw buffer
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, bufs[i]))
                mapped[i] = gfx.MapBuffer(bufs[i]);
        }
    };
    createRing(kMaxVertices,      m_vertexBuffer,      m_vertexMapped);
    createRing(kMaxDepthVertices, m_depthVertexBuffer, m_depthVertexMapped);

    LOG_INFO("DebugWirePass: vertex rings allocated on first use (%.0f MiB upload)",
             double(uint64_t(kMaxVertices + kMaxDepthVertices) * sizeof(LineVertex)
                    * kFrameCount) / (1024.0 * 1024.0));
}

void DebugWirePass::Clear()
{
    // First debug-wire frame commits the rings (no-op afterwards).
    EnsureRings();

    m_vertexCount      = 0;
    m_depthVertexCount = 0;
    // Cache this frame's mapped vertex pointer ONCE so AddLine's hot path (once per
    // wireframe edge: millions on a full-scene collision overlay, where the call was the
    // top self-CPU cost) avoids the per-call virtual GetFrameIndex(). The frame slot is
    // stable from BeginFrame to Execute, and Clear() runs at the top of every debug-wire
    // build (Renderer::BuildScene_DebugWireframes) before any AddLine that frame.
    m_curVerts      = nullptr;
    m_curDepthVerts = nullptr;
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetFrameIndex();
    if (frameSlot < kFrameCount)
    {
        m_curVerts      = static_cast<LineVertex*>(m_vertexMapped[frameSlot]);
        m_curDepthVerts = static_cast<LineVertex*>(m_depthVertexMapped[frameSlot]);
    }
}

void DebugWirePass::AddLine(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t color)
{
    // Hot path: once per wireframe edge. m_curVerts is cached by Clear().
    LineVertex* verts = m_curVerts;
    if (!verts || m_vertexCount + 2 > kMaxVertices) return;
    verts[m_vertexCount++] = { a.x, a.y, a.z, color };
    verts[m_vertexCount++] = { b.x, b.y, b.z, color };
}

void DebugWirePass::AddLineDepthTested(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t color)
{
    // Separate buffer, also filled from vertex 0 (see kMaxDepthVertices).
    LineVertex* verts = m_curDepthVerts;
    if (!verts || m_depthVertexCount + 2 > kMaxDepthVertices) return;
    verts[m_depthVertexCount++] = { a.x, a.y, a.z, color };
    verts[m_depthVertexCount++] = { b.x, b.y, b.z, color };
}

void DebugWirePass::AddAABB(const XMFLOAT3& mn, const XMFLOAT3& mx, uint32_t color)
{
    // 8 corners
    XMFLOAT3 c[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
        {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
        {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
        {mn.x, mx.y, mx.z}, {mx.x, mx.y, mx.z},
    };
    // 12 edges
    AddLine(c[0], c[1], color); AddLine(c[2], c[3], color);
    AddLine(c[4], c[5], color); AddLine(c[6], c[7], color);
    AddLine(c[0], c[2], color); AddLine(c[1], c[3], color);
    AddLine(c[4], c[6], color); AddLine(c[5], c[7], color);
    AddLine(c[0], c[4], color); AddLine(c[1], c[5], color);
    AddLine(c[2], c[6], color); AddLine(c[3], c[7], color);
}

void DebugWirePass::AddOBB(const XMFLOAT4X4& worldMatrix,
                           const XMFLOAT3& halfExtents, uint32_t color)
{
    const XMMATRIX M = XMLoadFloat4x4(&worldMatrix);
    XMFLOAT3 c[8];
    for (int i = 0; i < 8; ++i)
    {
        const float sx = (i & 1) ? halfExtents.x : -halfExtents.x;
        const float sy = (i & 2) ? halfExtents.y : -halfExtents.y;
        const float sz = (i & 4) ? halfExtents.z : -halfExtents.z;
        XMStoreFloat3(&c[i], XMVector3TransformCoord(XMVectorSet(sx, sy, sz, 1.f), M));
    }
    // 12 edges: endpoints differ in exactly one corner-index bit
    // (x = bit0, y = bit1, z = bit2).
    static const int kEdges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},   // X edges
        {0,2},{1,3},{4,6},{5,7},   // Y edges
        {0,4},{1,5},{2,6},{3,7},   // Z edges
    };
    for (auto& edge : kEdges) AddLine(c[edge[0]], c[edge[1]], color);
}

void DebugWirePass::AddCross(const XMFLOAT3& pos, float size, uint32_t color)
{
    // 3 axis-aligned line segments through the centre: 6 vertices.
    AddLine({ pos.x - size, pos.y, pos.z }, { pos.x + size, pos.y, pos.z }, color);
    AddLine({ pos.x, pos.y - size, pos.z }, { pos.x, pos.y + size, pos.z }, color);
    AddLine({ pos.x, pos.y, pos.z - size }, { pos.x, pos.y, pos.z + size }, color);
}

void DebugWirePass::AddFrustum(const XMFLOAT3 corners[8], uint32_t color)
{
    // Near plane edges (corners 0-3)
    AddLine(corners[0], corners[1], color);
    AddLine(corners[1], corners[3], color);
    AddLine(corners[3], corners[2], color);
    AddLine(corners[2], corners[0], color);
    // Far plane edges (corners 4-7)
    AddLine(corners[4], corners[5], color);
    AddLine(corners[5], corners[7], color);
    AddLine(corners[7], corners[6], color);
    AddLine(corners[6], corners[4], color);
    // Connecting edges
    AddLine(corners[0], corners[4], color);
    AddLine(corners[1], corners[5], color);
    AddLine(corners[2], corners[6], color);
    AddLine(corners[3], corners[7], color);
}

void DebugWirePass::AddCapsule(const XMFLOAT3& a, const XMFLOAT3& b,
                               float radius, uint32_t color, int segments)
{
    // Axis from A to B.
    XMVECTOR vA   = XMLoadFloat3(&a);
    XMVECTOR vB   = XMLoadFloat3(&b);
    XMVECTOR axis = XMVectorSubtract(vB, vA);
    float    len  = XMVectorGetX(XMVector3Length(axis));
    if (len < 1e-6f) return;
    axis = XMVectorScale(axis, 1.f / len);

    // Build orthonormal basis (axis, u, v).
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    if (std::abs(XMVectorGetX(XMVector3Dot(axis, up))) > 0.99f)
        up = XMVectorSet(1, 0, 0, 0);
    XMVECTOR u = XMVector3Normalize(XMVector3Cross(axis, up));
    XMVECTOR v = XMVector3Cross(axis, u);

    const float step = DirectX::XM_2PI / static_cast<float>(segments);

    // Circle at A
    for (int i = 0; i < segments; ++i)
    {
        float a0 = step * i, a1 = step * (i + 1);
        XMVECTOR p0 = XMVectorAdd(vA, XMVectorAdd(XMVectorScale(u, cosf(a0) * radius), XMVectorScale(v, sinf(a0) * radius)));
        XMVECTOR p1 = XMVectorAdd(vA, XMVectorAdd(XMVectorScale(u, cosf(a1) * radius), XMVectorScale(v, sinf(a1) * radius)));
        XMFLOAT3 f0, f1; XMStoreFloat3(&f0, p0); XMStoreFloat3(&f1, p1);
        AddLine(f0, f1, color);
    }
    // Circle at B
    for (int i = 0; i < segments; ++i)
    {
        float a0 = step * i, a1 = step * (i + 1);
        XMVECTOR p0 = XMVectorAdd(vB, XMVectorAdd(XMVectorScale(u, cosf(a0) * radius), XMVectorScale(v, sinf(a0) * radius)));
        XMVECTOR p1 = XMVectorAdd(vB, XMVectorAdd(XMVectorScale(u, cosf(a1) * radius), XMVectorScale(v, sinf(a1) * radius)));
        XMFLOAT3 f0, f1; XMStoreFloat3(&f0, p0); XMStoreFloat3(&f1, p1);
        AddLine(f0, f1, color);
    }
    // Connecting lines (4 rails along the cylinder)
    for (int i = 0; i < 4; ++i)
    {
        float ang = step * (i * segments / 4);
        XMVECTOR off = XMVectorAdd(XMVectorScale(u, cosf(ang) * radius), XMVectorScale(v, sinf(ang) * radius));
        XMFLOAT3 fa, fb;
        XMStoreFloat3(&fa, XMVectorAdd(vA, off));
        XMStoreFloat3(&fb, XMVectorAdd(vB, off));
        AddLine(fa, fb, color);
    }

    // Hemisphere arcs at each endpoint (without them the wireframe reads as a
    // cylinder). Two great-circle half-arcs per hemisphere (in the axis-u and axis-v
    // planes) give the capsule silhouette from any viewing angle. Pole at A = A +
    // (-axis)*radius; pole at B = B + axis*radius.
    const int   halfSegs = std::max(2, segments / 2);
    const float halfStep = DirectX::XM_PI / static_cast<float>(halfSegs);

    auto emitHemisphere = [&](XMVECTOR centre, XMVECTOR outward)
    {
        for (int planeIdx = 0; planeIdx < 2; ++planeIdx)
        {
            XMVECTOR tangent = (planeIdx == 0) ? u : v;
            for (int i = 0; i < halfSegs; ++i)
            {
                const float p0 = halfStep * static_cast<float>(i);
                const float p1 = halfStep * static_cast<float>(i + 1);
                XMVECTOR pos0 = XMVectorAdd(centre,
                    XMVectorAdd(XMVectorScale(outward, sinf(p0) * radius),
                                 XMVectorScale(tangent, cosf(p0) * radius)));
                XMVECTOR pos1 = XMVectorAdd(centre,
                    XMVectorAdd(XMVectorScale(outward, sinf(p1) * radius),
                                 XMVectorScale(tangent, cosf(p1) * radius)));
                XMFLOAT3 f0, f1;
                XMStoreFloat3(&f0, pos0);
                XMStoreFloat3(&f1, pos1);
                AddLine(f0, f1, color);
            }
        }
    };

    emitHemisphere(vA, XMVectorNegate(axis));  // dome away from B
    emitHemisphere(vB, axis);                  // dome away from A
}

void DebugWirePass::AddSphere(const XMFLOAT3& center, float radius,
                              uint32_t color, int segments)
{
    if (segments < 4) segments = 4;

    const XMVECTOR c    = XMLoadFloat3(&center);
    const float    step = DirectX::XM_2PI / static_cast<float>(segments);

    // Three orthogonal great circles (XY, YZ, XZ), each a closed loop of `segments`
    // lines. From any view angle at least one circle's silhouette is on-edge, so the
    // sphere reads as round rather than slabby.
    const XMVECTOR basis[3][2] = {
        { XMVectorSet(1,0,0,0), XMVectorSet(0,1,0,0) },   // XY plane
        { XMVectorSet(0,1,0,0), XMVectorSet(0,0,1,0) },   // YZ plane
        { XMVectorSet(0,0,1,0), XMVectorSet(1,0,0,0) },   // XZ plane
    };

    for (int axisIdx = 0; axisIdx < 3; ++axisIdx)
    {
        const XMVECTOR& u = basis[axisIdx][0];
        const XMVECTOR& v = basis[axisIdx][1];
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = step * static_cast<float>(i);
            const float a1 = step * static_cast<float>(i + 1);
            XMVECTOR p0 = XMVectorAdd(c, XMVectorAdd(
                XMVectorScale(u, cosf(a0) * radius),
                XMVectorScale(v, sinf(a0) * radius)));
            XMVECTOR p1 = XMVectorAdd(c, XMVectorAdd(
                XMVectorScale(u, cosf(a1) * radius),
                XMVectorScale(v, sinf(a1) * radius)));
            XMFLOAT3 f0, f1;
            XMStoreFloat3(&f0, p0);
            XMStoreFloat3(&f1, p1);
            AddLine(f0, f1, color);
        }
    }
}

void DebugWirePass::Execute(RHI::CommandList     cl,
                            const XMFLOAT4X4&    viewProjNoJitter,
                            const RHI::Texture*  target,
                            RHI::ResourceState   entryState,
                            uint32_t             canvasW,
                            uint32_t             canvasH,
                            uint64_t             sceneDepthSrv,
                            uint32_t             depthW,
                            uint32_t             depthH)
{
    // INVARIANT (as in DebugIconPass): every early-out must precede the first resource
    // barrier, so this pass is either a clean no-op or a complete transition pair, never
    // a half-transition that desyncs the caller's tracker.
    if (!enabled || (m_vertexCount == 0 && m_depthVertexCount == 0)) return;
    if (!m_gfx || !target || !target->IsValid())                     return;
    if (canvasW == 0 || canvasH == 0)                                return;

    const RHI::PipelineState* psoOverlay = m_psoCache.GetOrCreate(BuildPSODesc(false));
    if (!psoOverlay || !psoOverlay->IsValid()) return;

    // No depth SRV this frame -> the depth-aware bucket degrades to a plain overlay
    // rather than sampling a null descriptor.
    const bool haveDepth = (sceneDepthSrv != 0) && depthW > 0 && depthH > 0;
    const RHI::PipelineState* psoDepth = haveDepth
        ? m_psoCache.GetOrCreate(BuildPSODesc(true))
        : nullptr;
    if (!psoDepth || !psoDepth->IsValid()) psoDepth = psoOverlay;

    auto& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (frameSlot >= kFrameCount) return;

    // Engine stores CB matrices transposed (HLSL packs cbuffer matrices
    // column-major, the VS uses the row-vector `mul(pos, viewProj)` form).
    if (WireCB* slot = m_cb.Current(gfx))
    {
        XMStoreFloat4x4(&slot->viewProj,
                        XMMatrixTranspose(XMLoadFloat4x4(&viewProjNoJitter)));
        slot->targetSize[0] = static_cast<float>(canvasW);
        slot->targetSize[1] = static_cast<float>(canvasH);
        slot->targetSize[2] = haveDepth ? static_cast<float>(depthW) : 0.f;
        slot->targetSize[3] = haveDepth ? static_cast<float>(depthH) : 0.f;
        slot->params[0]     = kXRayAlpha;
        // Relative depth bias (see DebugWireDepth.ps.hlsl). Large enough to survive the
        // half-pixel jitter offset between the un-jittered line projection and the jittered
        // depth buffer.
        slot->params[1]     = 2.0e-3f;
        slot->params[2]     = 0.f;
        slot->params[3]     = 0.f;
    }

    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target, entryState, RHI::ResourceState::RENDERTARGET), cl);
    }

    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.topLeftX = 0; vp.topLeftY = 0;
    vp.width      = static_cast<float>(canvasW);
    vp.height     = static_cast<float>(canvasH);
    vp.minDepth  = 0.f; vp.maxDepth = 1.f;
    gfx.SetViewport(vp, cl);
    gfx.SetScissorRect(0, 0, canvasW, canvasH, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::LINELIST, cl);

    // BindPipelineState re-issues SetGraphicsRootSignature, which invalidates every
    // root argument: re-bind the CB + tables after EACH PSO switch, not once up front.
    // Each bucket has its own vertex buffer and both start at vertex 0: the VS indexes
    // purely by SV_VertexID, so nothing here depends on StartVertexLocation being folded
    // into it.
    auto drawBucket = [&](const RHI::PipelineState& pso, const RHI::GPUBuffer& vb,
                          uint32_t count, bool bindDepth)
    {
        if (count == 0) return;
        const uint64_t vbSrv = gfx.GetBufferSRVGpuHandle(vb);
        if (!vbSrv) return;
        gfx.BindPipelineState(pso, cl);
        // BindConstantBuffer's `slot` is 0-based relative to kCBVSlotBase (b1=0, b2=1,
        // ...), so CB_b1 maps to b1. Do not pass 1: that binds b2, leaves the shader's b1
        // undefined and transforms every vertex off-screen.
        gfx.BindConstantBuffer(RHI::Slots::GfxRel::CB_b1, m_cb.CurrentBuffer(gfx), cl);
        gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbSrv, cl);
        if (bindDepth)
            gfx.BindDescriptorTableGpuHandle(kRootSlot_DepthSRV, sceneDepthSrv, cl);
        gfx.DrawInstanced(count, 1, 0, 0, cl);
    };

    drawBucket(*psoOverlay, m_vertexBuffer[frameSlot],      m_vertexCount,      false);
    drawBucket(*psoDepth,   m_depthVertexBuffer[frameSlot], m_depthVertexCount, haveDepth);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}
