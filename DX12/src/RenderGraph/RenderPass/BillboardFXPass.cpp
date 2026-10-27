#include "RenderGraph/RenderPass/BillboardFXPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform
#include "Resource/ResourceManager.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace DirectX;

// Engine default graphics root-signature slots, shared with WorldUIBillboardPass:
//   b1 space0 - PerView CB (viewProj)
//   t2 space0 - vertex ByteAddressBuffer (the generic per-draw SRV table 0)
//   t0 space2 - bindless Texture2D[] table
// Both constants are absolute graphics root indices (what BindDescriptorTableHandle takes), from
// RHI::Slots::Gfx (Graphics/RootSignatureLayout.h); the local names say t2 carries the billboard
// vertex buffer here.
static constexpr auto kRootSlot_VertexSRV   = RHI::Slots::Gfx::SRV_t2;     // t2 space0
static constexpr auto kRootSlot_BindlessTex = RHI::Slots::Gfx::BindlessTex; // t0 space2 (engine-shared)
// Group-relative index: cl.BindCB forwards to BindConstantBuffer, which adds the group base. An
// absolute root index here binds nothing and logs nothing (RHI::Slots::GfxRel). The sampler is the
// fixed SMP_LINEAR_CLAMP heap slot BillboardFX.ps names, so none is bound.
static constexpr auto kCBRel_PerView        = RHI::Slots::GfxRel::CB_b1;      // -> b1 space0
// b0 root constant word 0; mirror of BillboardFX.ps.hlsl BILLBOARD_FLAG_*.
static constexpr uint32_t kDrawFlagAdditive     = 1u;   // dest ONE (else INV_SRC_ALPHA)
static constexpr uint32_t kDrawFlagDepthTest    = 2u;   // the quad depth-tests: soft-fade it

BillboardFXPass::BillboardFXPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void BillboardFXPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth is only tested: declared as a read, so the plane stays in a state the test accepts.
    // -rgplaneoff declares the write instead, which brings depth to DEPTHSTENCIL.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Read);
    else                                  b.WriteDepthStencil(m_depth);
    // The soft fade reads the opaque depth snapshot through a heap index (FogApplyCB); this
    // declaration is the contract.
    if (m_opaqueDepth.IsValid()) b.ReadSRV(m_opaqueDepth);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds or clears it)
    // and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The froxel fog FogApply samples (VolumetricFogPass barriers it).
    b.ReadPassTextures({ RG::PassTex::kVolFogScattering });
}

void BillboardFXPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::BillboardFX_VS, RHI::ShaderStage::VS, "BillboardFX.vs.hlsl");
    m_shaderLib.Register(ShaderID::BillboardFX_PS, RHI::ShaderStage::PS, "BillboardFX.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    CreateRing(m_main);

    // Warm up the common PSO permutations (additive + alpha, depth-tested).
    m_psoCache.GetOrCreate(BuildPSODesc(BillboardFXBlend::Additive, true));
    m_psoCache.GetOrCreate(BuildPSODesc(BillboardFXBlend::Alpha,    true));
    LOG_SUCCESS("BillboardFXPass: initialized");
}

// One UPLOAD vertex buffer per frame in flight, flat-expanded with no index buffer: the engine root
// signature vertex-pulls from a ByteAddressBuffer.
void BillboardFXPass::CreateRing(Expansion& x)
{
    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(Vertex);
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_gfx->CreateBuffer(bd, x.vertexBuffer[i]))
            x.vbMapped[i] = m_gfx->MapBuffer(x.vertexBuffer[i]);
        else
            LOG_ERROR("BillboardFXPass: VB[%u] creation failed", i);
    }
}

void BillboardFXPass::ViewExpansion::Release(IGraphicsDevice& gfx)
{
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (expansion.vbMapped[i])               gfx.UnmapBuffer(expansion.vertexBuffer[i]);
        if (expansion.vertexBuffer[i].IsValid()) gfx.DestroyBuffer(expansion.vertexBuffer[i]);
    }
    expansion = Expansion{};
}

void BillboardFXPass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
    {
        m_x = &m_main;
        return;
    }
    Expansion& x = view.Data<ViewExpansion>(this).expansion;
    if (!x.vertexBuffer[0].IsValid() && m_gfx) CreateRing(x);
    m_x = &x;
}

void BillboardFXPass::ExpandView(ViewState& view, const XMFLOAT4X4& viewMatrix, const XMFLOAT3& cameraPos)
{
    // A lookup only: the game half must not add to a view's data the render half may be reading.
    if (ViewExpansion* v = view.FindData<ViewExpansion>(this))
        Expand(v->expansion, viewMatrix, cameraPos);
}

PSODesc BillboardFXPass::BuildPSODesc(BillboardFXBlend blend, bool depthTest) const
{
    PSODesc desc;
    desc.vsID        = ShaderID::BillboardFX_VS;
    desc.psID        = ShaderID::BillboardFX_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = true;

    // Depth test as requested, never depth write; reversed-Z (GREATER_EQUAL). dsvFormat stays set
    // even with the test off, so the PSO matches the bound DSV format.
    desc.dss.depthEnable     = depthTest;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;

    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable             = true;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    rt.blendOp                 = RHI::BlendOp::ADD;
    rt.blendOpAlpha           = RHI::BlendOp::ADD;
    rt.srcBlend                = RHI::Blend::SRC_ALPHA;
    rt.srcBlendAlpha          = RHI::Blend::ONE;
    if (blend == BillboardFXBlend::Additive)
    {
        rt.destBlend       = RHI::Blend::ONE;            // SRC_ALPHA*src + dst
        rt.destBlendAlpha = RHI::Blend::ONE;
    }
    else
    {
        rt.destBlend       = RHI::Blend::INV_SRC_ALPHA;  // standard alpha-over
        rt.destBlendAlpha = RHI::Blend::INV_SRC_ALPHA;
    }

    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT; // HDR scene colour
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

// Pushes 6 vertices (2 triangles, CCW) for one camera-facing quad.
void BillboardFXPass::EmitQuad(std::vector<Vertex>& out,
                               const XMFLOAT3& center,
                               const XMFLOAT3& right,
                               const XMFLOAT3& up,
                               float halfW, float halfH,
                               float u0, float v0, float u1, float v1,
                               const float color[4], uint32_t texIdx)
{
    const XMVECTOR c = XMLoadFloat3(&center);
    const XMVECTOR r = XMVectorScale(XMLoadFloat3(&right), halfW);
    const XMVECTOR u = XMVectorScale(XMLoadFloat3(&up),    halfH);

    XMFLOAT3 p[4];
    XMStoreFloat3(&p[0], XMVectorSubtract(XMVectorAdd(c, u), r));      // top-L
    XMStoreFloat3(&p[1], XMVectorAdd(XMVectorAdd(c, u), r));           // top-R
    XMStoreFloat3(&p[2], XMVectorAdd(XMVectorSubtract(c, u), r));      // bot-R
    XMStoreFloat3(&p[3], XMVectorSubtract(XMVectorSubtract(c, u), r)); // bot-L

    auto push = [&](const XMFLOAT3& xyz, float uu, float vv) {
        Vertex vtx{};
        vtx.pos[0] = xyz.x; vtx.pos[1] = xyz.y; vtx.pos[2] = xyz.z;
        vtx.uv[0]  = uu;    vtx.uv[1]  = vv;
        vtx.color[0] = color[0]; vtx.color[1] = color[1];
        vtx.color[2] = color[2]; vtx.color[3] = color[3];
        vtx.texIdx = texIdx;
        out.push_back(vtx);
    };

    push(p[0], u0, v0); push(p[1], u1, v0); push(p[2], u1, v1);
    push(p[0], u0, v0); push(p[2], u1, v1); push(p[3], u0, v1);
}

void BillboardFXPass::ResolveTexture(uint32_t entity, BillboardFXComponent& b)
{
    if (!m_texSys || !m_resMgr || !m_gfx) return;

    auto [it, _inserted] = m_texCache.try_emplace(entity);
    TexCache& entry = it->second;

    if (b.texturePath != entry.path)
    {
        if (entry.handle != Resource::kInvalidTextureHandle)
            m_texSys->Release(entry.handle, *m_gfx);

        entry.path   = b.texturePath;
        entry.handle = b.texturePath.empty()
                     ? Resource::kInvalidTextureHandle
                     : m_texSys->Acquire(b.texturePath, *m_resMgr, *m_gfx);
        b.textureBindlessIdx = -1;
        b.textureGpuHandle   = 0;
    }

    if (b.textureBindlessIdx < 0
        && entry.handle != Resource::kInvalidTextureHandle
        && m_texSys->IsReady(entry.handle))
    {
        if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
        {
            b.textureBindlessIdx = static_cast<int32_t>(tex->handleId);
            b.textureGpuHandle   = m_gfx->GetTextureSRVGpuHandle(*tex);
        }
    }
}

void BillboardFXPass::OnEntityDestroyed(uint32_t entity)
{
    auto it = m_texCache.find(entity);
    if (it == m_texCache.end()) return;
    if (m_texSys && m_gfx && it->second.handle != Resource::kInvalidTextureHandle)
        m_texSys->Release(it->second.handle, *m_gfx);
    m_texCache.erase(it);
}

void BillboardFXPass::OnWorldClear(const RG::WorldClearArgs&)
{
    if (m_texSys && m_gfx)
    {
        for (auto& [_entity, entry] : m_texCache)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, *m_gfx);
    }
    m_texCache.clear();
    m_main.draws.clear();
    m_recs.clear();
}

// Advances one flipbook's playback time -> current frame index.
static void AdvanceAnim(BillboardFXComponent& b, float dt)
{
    const int cols  = (b.columns > 0) ? b.columns : 1;
    const int rows  = (b.rows    > 0) ? b.rows    : 1;
    int total = (b.frameCount > 0) ? b.frameCount : (cols * rows);
    total = std::clamp(total, 1, cols * rows);

    if (total <= 1)
    {
        b.frame = 0;
        return;
    }

    if (b.playing && b.fps > 0.0f)
    {
        b.elapsed += dt;
        const int raw = static_cast<int>(b.elapsed * b.fps);
        switch (b.playback)
        {
        case BillboardFXPlayback::Once:
            b.frame = (raw >= total) ? (total - 1) : raw;
            break;
        case BillboardFXPlayback::PingPong:
        {
            const int period = 2 * total - 2;       // total > 1 here
            const int ph     = raw % period;
            b.frame = (ph < total) ? ph : (period - ph);
            break;
        }
        case BillboardFXPlayback::Loop:
        default:
            b.frame = raw % total;
            break;
        }
    }
    b.frame = std::clamp(b.frame, 0, total - 1);
}

void BillboardFXPass::BuildFrame(World& world, float dt,
                                 const XMFLOAT4X4& viewMatrix,
                                 const XMFLOAT3&   cameraPos)
{
    m_main.draws.clear();
    m_recs.clear();
    if (!enabled || !m_gfx) return;

    auto& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetUploadSlot();
    if (!m_main.vbMapped[frameSlot]) return;

    auto* pool = world.GetPool<BillboardFXComponent>();
    if (!pool || pool->Size() == 0) return;

    // ---- Gather render records (advance anim + resolve textures) ----
    auto& ents = pool->Entities();
    auto& data = pool->Data();
    m_recs.reserve(data.size());

    for (size_t i = 0; i < data.size(); ++i)
    {
        BillboardFXComponent& b = data[i];
        const Entity e = ents[i];

        AdvanceAnim(b, dt);
        ResolveTexture(static_cast<uint32_t>(e), b);

        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) continue;   // no world pose -> nothing to place

        const float alpha = std::clamp(b.tint.w * b.opacity, 0.0f, 1.0f);
        if (alpha <= 0.001f) continue;   // fully transparent (alpha) / no contribution (additive)
        if (b.size <= 0.0f)  continue;

        const int cols  = (b.columns > 0) ? b.columns : 1;
        const int rows  = (b.rows    > 0) ? b.rows    : 1;
        int total = (b.frameCount > 0) ? b.frameCount : (cols * rows);
        total = std::clamp(total, 1, cols * rows);
        const int frame = std::clamp(b.frame, 0, total - 1);
        const int col = frame % cols;
        const int row = frame / cols;

        Rec r;
        r.pos  = { gt->matrix.m[3][0], gt->matrix.m[3][1], gt->matrix.m[3][2] };
        r.half = b.size * 0.5f;
        r.u0 = static_cast<float>(col)     / static_cast<float>(cols);
        r.v0 = static_cast<float>(row)     / static_cast<float>(rows);
        r.u1 = static_cast<float>(col + 1) / static_cast<float>(cols);
        r.v1 = static_cast<float>(row + 1) / static_cast<float>(rows);
        r.color[0] = b.tint.x * b.emissive;
        r.color[1] = b.tint.y * b.emissive;
        r.color[2] = b.tint.z * b.emissive;
        r.color[3] = alpha;
        r.texIdx   = (b.textureBindlessIdx < 0)
                   ? kInvalidTexIdx
                   : static_cast<uint32_t>(b.textureBindlessIdx);
        r.blend     = b.blend;
        r.depthTest = b.depthTest;
        r.face      = b.face;
        m_recs.push_back(r);
    }

    Expand(m_main, viewMatrix, cameraPos);
}

void BillboardFXPass::Expand(Expansion& x, const XMFLOAT4X4& viewMatrix, const XMFLOAT3& cameraPos)
{
    x.draws.clear();
    const uint32_t frameSlot = m_gfx ? m_gfx->GetUploadSlot() : 0u;
    if (m_recs.empty() || !x.vbMapped[frameSlot]) return;

    // World-space camera axes from the row-major view matrix columns (same
    // derivation as WorldSpaceUISystem / WorldUIBillboardPass).
    const float* vm = reinterpret_cast<const float*>(&viewMatrix);
    const XMFLOAT3 camRight { vm[0], vm[4], vm[8]  };
    const XMFLOAT3 camUp    { vm[1], vm[5], vm[9]  };
    const XMFLOAT3 camFwd   { vm[2], vm[6], vm[10] };
    const XMVECTOR camPosV  = XMLoadFloat3(&cameraPos);

    // Back-to-front so alpha-blended sprites composite correctly (harmless for
    // additive, which is order-independent): squared distance to this camera.
    struct Sorted { float dist; uint32_t rec; };
    static thread_local std::vector<Sorted> t_order;
    t_order.clear();
    t_order.reserve(m_recs.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_recs.size()); ++i)
    {
        const XMVECTOR pv = XMLoadFloat3(&m_recs[i].pos);
        t_order.push_back({ XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(pv, camPosV))), i });
    }
    std::sort(t_order.begin(), t_order.end(),
              [](const Sorted& a, const Sorted& b) { return a.dist > b.dist; });

    // ---- Expand to vertices, grouping consecutive same-PSO runs ----
    static thread_local std::vector<Vertex> t_verts;
    t_verts.clear();
    t_verts.reserve(m_recs.size() * 6);

    for (const Sorted& s : t_order)
    {
        const Rec& r = m_recs[s.rec];
        if (t_verts.size() + 6 > kMaxVertices) break;   // ring full: drop the rest

        XMFLOAT3 right = camRight;
        XMFLOAT3 up    = camUp;
        if (r.face == BillboardFXFace::Cylindrical)
        {
            // Yaw to the camera but keep world-up vertical.
            const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            XMVECTOR rr = XMVector3Cross(worldUp, XMLoadFloat3(&camFwd));
            if (XMVectorGetX(XMVector3LengthSq(rr)) < 1e-6f)
                rr = XMLoadFloat3(&camRight);          // looking straight up/down
            XMStoreFloat3(&right, XMVector3Normalize(rr));
            up = { 0.0f, 1.0f, 0.0f };
        }

        const uint32_t start = static_cast<uint32_t>(t_verts.size());
        EmitQuad(t_verts, r.pos, right, up, r.half, r.half,
                 r.u0, r.v0, r.u1, r.v1, r.color, r.texIdx);

        if (!x.draws.empty()
            && x.draws.back().blend == r.blend
            && x.draws.back().depthTest == r.depthTest)
        {
            x.draws.back().count += 6;
        }
        else
        {
            x.draws.push_back({ start, 6u, r.blend, r.depthTest });
        }
    }

    if (t_verts.empty()) { x.draws.clear(); return; }

    std::memcpy(x.vbMapped[frameSlot], t_verts.data(), t_verts.size() * sizeof(Vertex));
}

RHI::CommandList BillboardFXPass::Execute(RHI::CommandList cl)
{
    const Expansion& x = *m_x;
    if (!enabled || x.draws.empty()) return cl;

    // Graph passes run on a worker command list: use the cl. wrapper API throughout (not the gfx.Bind*
    // main-list helpers WorldUIBillboardPass uses on its direct path), and bind root args after the PSO
    // so the root signature is live (as TransparentPass does).
    auto& gfx = *cl.gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (!x.vbMapped[frameSlot]) return cl;

    // Bind the HDR RTV with the GBuffer depth (read-only test, no write).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));

    cl.SetViewport();
    cl.SetScissorRect();
    cl.SetPrimitiveTopology();

    const uint64_t vbHandle    = gfx.GetBufferSRVGpuHandle(x.vertexBuffer[frameSlot]);
    const uint64_t bindlessTbl = gfx.GetBindlessTextureTableGpuHandle();

    // SetGraphicsRootSignature (inside SetPipelineState) resets root args, so
    // re-bind globals on every PSO switch (same defensive pattern as
    // TransparentPass::bindGlobals).
    auto bindGlobals = [&]()
    {
        cl.BindDescriptorHeaps();
        cl.BindCB(kCBRel_PerView, RG::RGSlot::PerView);    // b1: viewProj (first matrix)
        if (vbHandle)        cl.BindDescriptorTableHandle(kRootSlot_VertexSRV, vbHandle);
        if (bindlessTbl)     cl.BindDescriptorTableHandle(kRootSlot_BindlessTex, bindlessTbl);
        // b4: the fog the opaque scene got, and the soft-fade depth.
        FogApply::Bind(cl);
    };

    const RHI::PipelineState* active = nullptr;
    for (const DrawRange& d : x.draws)
    {
        const RHI::PipelineState* pso =
            m_psoCache.GetOrCreate(BuildPSODesc(d.blend, d.depthTest));
        if (!pso || !pso->IsValid()) continue;
        if (pso != active)
        {
            cl.SetPipelineState(*pso);
            active = pso;
            bindGlobals();
            // b0: the PSO state the PS needs for the soft fade and the fog
            // (BillboardFX.ps.hlsl BILLBOARD_FLAG_*). The PSO key is exactly
            // (blend, depthTest), so setting it per PSO switch covers each draw.
            const uint32_t flags = (d.blend == BillboardFXBlend::Additive ? kDrawFlagAdditive : 0u)
                                 | (d.depthTest ? kDrawFlagDepthTest : 0u);
            gfx.SetGraphicsRootConstant(RHI::Slots::Gfx::RootConstants, flags, /*offsetWords=*/0, cl);
        }
        cl.DrawInstanced(d.count, 1, d.startVertex, 0);
    }

    return cl;
}
