#include "RenderGraph/RenderPass/PickingPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// Root parameter slots: aliases of the shared graphics root signature
// (Graphics/RootSignatureLayout.h).
static constexpr auto kPerViewCBSlot   = RHI::Slots::GfxRel::CB_b1;   // b1 space0 (CB-relative index)
static constexpr auto kInstanceBufSlot = RHI::Slots::Gfx::InstanceBuf;
static constexpr auto kMeshDescSlot    = RHI::Slots::Gfx::MeshDesc;
static constexpr auto kBindlessSlot    = RHI::Slots::Gfx::BindlessBuffers;

PickingPass::~PickingPass()
{
    if (m_gfx)
    {
        ReleaseTargets(*m_gfx, m_main);
        if (m_readbackBuffer.IsValid()) m_gfx->DestroyBuffer(m_readbackBuffer);
        if (m_imageReadback.IsValid())  m_gfx->DestroyBuffer(m_imageReadback);
    }
}

void PickingPass::ReleaseTargets(IGraphicsDevice& gfx, Targets& t)
{
    if (t.id.IsValid())    gfx.DestroyTexture(t.id);
    if (t.depth.IsValid()) gfx.DestroyTexture(t.depth);
    t = Targets{};
}

void PickingPass::ViewTargets::Release(IGraphicsDevice& gfx)
{
    ReleaseTargets(gfx, targets);
}

void PickingPass::BindView(ViewState& view)
{
    m_boundSlot = view.Slot();
    m_t         = m_boundSlot == 0 ? &m_main : &view.Data<ViewTargets>(this).targets;
}

void PickingPass::Setup(RG::RenderGraphBuilder&) {}   // no graph-managed textures

void PickingPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // Free resources from a previous Init (graph recompile on window resize).
    if (m_readbackBuffer.IsValid()) gfx.DestroyBuffer(m_readbackBuffer);
    if (m_imageReadback.IsValid())  gfx.DestroyBuffer(m_imageReadback);
    ReleaseTargets(gfx, m_main);        // force EnsureTextures to recreate
    m_imageBytes = 0;
    m_pickRequested = m_pickPending = false;
    m_imageRequested = m_imagePending = false;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::PickingID_VS, RHI::ShaderStage::VS, "PickingID.vs.hlsl");
    m_shaderLib.Register(ShaderID::PickingID_PS, RHI::ShaderStage::PS, "PickingID.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    if (!m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_ERROR("PickingPass: PSO creation failed");
    else
        LOG_INFO("PickingPass: PSO ready");

    // Single-pixel READBACK buffer (512 = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT).
    RHI::GPUBufferDesc readbackDesc;
    readbackDesc.size       = 512;
    readbackDesc.usage      = RHI::Usage::READBACK;
    readbackDesc.bindFlags = RHI::BindFlag::NONE;
    if (!gfx.CreateBuffer(readbackDesc, m_readbackBuffer))
        LOG_ERROR("PickingPass: readback buffer creation failed");
    else
        LOG_INFO("PickingPass: readback buffer created");
}

// Serially after the graph's textures are (re)created: destroys the main view's textures and
// pre-creates them at the render size, so EnsureTextures in Execute is a no-op during the
// parallel phase.
void PickingPass::OnCompile(IGraphicsDevice& gfx)
{
    ReleaseTargets(gfx, m_main);
    m_pickRequested = false;
    m_pickPending   = false;
    // An image asked before the rebuild is gone with the texture it named; the
    // harness asks again (its buffer is resized on the next RequestImage).
    m_imageRequested = false;
    m_imagePending   = false;

    const uint32_t w = gfx.GetRenderWidth();
    const uint32_t h = gfx.GetRenderHeight();
    if (w > 0 && h > 0)
        EnsureTextures(gfx, m_main, w, h);
}

PSODesc PickingPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::PickingID_VS;
    desc.psID        = ShaderID::PickingID_PS;
    desc.inputLayout = InputLayoutType::None;   // PVF

    // No back-face culling: double-sided transparents (leaves, banners, decals)
    // must be pickable from either side. PickingPass only executes on click
    // frames (see Execute() early-out), so the extra raster cost is negligible.
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = true;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reversed Z

    desc.bs.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = RHI::Format::R32_UINT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT;
    return desc;
}

void PickingPass::EnsureTextures(IGraphicsDevice& gfx, Targets& t, uint32_t w, uint32_t h)
{
    if (w == t.w && h == t.h) return;

    ReleaseTargets(gfx, t);

    // ---- R32_UINT ID texture ----
    {
        RHI::TextureDesc desc;
        desc.width      = w;
        desc.height     = h;
        desc.format     = RHI::Format::R32_UINT;
        desc.bindFlags = RHI::BindFlag::RENDER_TARGET;
        desc.layout     = RHI::ResourceState::RENDERTARGET;
        desc.clear.color[0] = desc.clear.color[1] = desc.clear.color[2] = desc.clear.color[3] = 0.f;
        desc.debugName = "PickingPass.IDTexture";
        if (!gfx.CreateTexture(desc, t.id))
            LOG_ERROR("PickingPass: ID texture creation failed");
        t.idState = RHI::ResourceState::RENDERTARGET;
    }

    // ---- D32_FLOAT depth buffer ----
    {
        RHI::TextureDesc desc;
        desc.width      = w;
        desc.height     = h;
        desc.format     = RHI::Format::D32_FLOAT;
        desc.bindFlags = RHI::BindFlag::DEPTH_STENCIL;
        desc.layout     = RHI::ResourceState::DEPTHSTENCIL;
        desc.clear.depthStencil.depth   = 0.f;  // reversed Z
        desc.clear.depthStencil.stencil = 0;
        desc.debugName = "PickingPass.Depth";
        if (!gfx.CreateTexture(desc, t.depth))
            LOG_ERROR("PickingPass: depth texture creation failed");
    }

    t.w = w;
    t.h = h;
    LOG_INFO("PickingPass: resized textures to %ux%u", w, h);
}

void PickingPass::PrepareBoundView(IGraphicsDevice& gfx, uint32_t width, uint32_t height)
{
    if (m_boundSlot == 0 || !DrawsInBoundView() || width == 0 || height == 0) return;
    EnsureTextures(gfx, *m_t, width, height);
    // An image the buffer cannot be made for is dropped (the harness asks again).
    if (m_imageView == m_boundSlot && m_imageRequested && !EnsureImageReadback(*m_t))
        m_imageRequested = false;
}

void PickingPass::CancelView(uint32_t viewSlot)
{
    if (viewSlot == 0) return;
    if (m_pickView == viewSlot)  CancelPick();
    if (m_imageView == viewSlot) CancelImage();
}

void PickingPass::RequestPick(int pixelX, int pixelY, uint32_t viewSlot)
{
    m_pickX         = pixelX;
    m_pickY         = pixelY;
    m_pickView      = viewSlot;
    m_pickRequested = true;
    // m_pickPending is not set here but by Execute() once the GPU copy is recorded, so
    // Renderer::ResolvePick cannot read stale data on the click's frame.
}

uint32_t PickingPass::ReadPickResult() const
{
    if (!m_readbackBuffer.IsValid() || !m_gfx) return 0;

    void* pData = m_gfx->MapBuffer(m_readbackBuffer);
    if (!pData) return 0;
    const uint32_t result = *static_cast<const uint32_t*>(pData);
	LOG_INFO("Picking POS = ( %u , %u )", m_pickX,m_pickY);
	LOG_INFO("PickingPass: picked instance slot = %u", result);
    m_gfx->UnmapBuffer(m_readbackBuffer);
    return result;
}

// The object-ID image. Sized here, on the main thread (a secondary view's in its segment's
// PrepareBoundView), so Execute (recorded in the parallel phase) never creates a resource.
bool PickingPass::RequestImage(uint32_t viewSlot)
{
    if (!m_gfx) return false;
    if (viewSlot == 0 && !EnsureImageReadback(m_main)) return false;
    m_imageView      = viewSlot;
    m_imageRequested = true;
    m_imagePending   = false;
    return true;
}

bool PickingPass::EnsureImageReadback(const Targets& t)
{
    if (!m_gfx || !t.id.IsValid()) return false;
    uint32_t pitch = 0, rows = 0;
    uint64_t bytes = 0;
    if (!m_gfx->GetTexture2DReadbackFootprint(t.id, pitch, rows, bytes)) return false;
    if (!m_imageReadback.IsValid() || m_imageBytes < bytes)
    {
        if (m_imageReadback.IsValid()) m_gfx->DestroyBuffer(m_imageReadback);
        m_imageBytes = 0;
        RHI::GPUBufferDesc desc;
        desc.size       = bytes;
        desc.usage      = RHI::Usage::READBACK;
        desc.bindFlags = RHI::BindFlag::NONE;
        if (!m_gfx->CreateBuffer(desc, m_imageReadback))
        {
            LOG_ERROR("PickingPass: id image readback buffer (%llu bytes) creation failed",
                      static_cast<unsigned long long>(bytes));
            return false;
        }
        m_imageBytes = bytes;
    }
    return true;
}

bool PickingPass::ReadImage(std::vector<uint32_t>& out, uint32_t& outW, uint32_t& outH)
{
    if (!m_imagePending || !m_imageReadback.IsValid() || !m_gfx) return false;
    const auto* src = static_cast<const uint8_t*>(m_gfx->MapBuffer(m_imageReadback));
    if (!src) return false;
    // Rows sit m_imageRowPitch apart, not width * 4: D3D12 aligns a placed
    // footprint's rows to 256 bytes.
    out.resize(static_cast<size_t>(m_imageW) * m_imageH);
    for (uint32_t y = 0; y < m_imageH; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * m_imageW,
                    src + static_cast<size_t>(y) * m_imageRowPitch, static_cast<size_t>(m_imageW) * 4u);
    m_gfx->UnmapBuffer(m_imageReadback);
    outW = m_imageW;
    outH = m_imageH;
    m_imagePending = false;
    return true;
}

RHI::CommandList PickingPass::Execute(RHI::CommandList cl)
{
    // Picking is request-driven: unless a RequestPick is outstanding for the bound view
    // (m_pickRequested), a previous frame's copy awaits readback (m_pickPending) or an object-ID
    // image is asked of it (RequestImage), there is nothing to draw; an idle pass would pay a full
    // opaque draw-list render, clears and binds (several hundred us) every frame. An image only
    // awaiting its read does not draw.
    if (!DrawsInBoundView())
        return cl;
    const bool pickHere  = m_pickView == m_boundSlot && m_pickRequested;
    const bool imageHere = m_imageView == m_boundSlot && m_imageRequested;

    const uint32_t vpW = cl.GetWidth();
    const uint32_t vpH = cl.GetHeight();
    if (vpW == 0 || vpH == 0) return cl;

    Targets& t = *m_t;
    EnsureTextures(cl.GetDevice(), t, vpW, vpH);
    if (!t.id.IsValid() || !t.depth.IsValid()) return cl;

    // ---- ID texture to RENDER_TARGET if it is in COPY_SRC ----
    if (t.idState == RHI::ResourceState::COPY_SRC)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(&t.id,
            RHI::ResourceState::COPY_SRC, RHI::ResourceState::RENDERTARGET));
        t.idState = RHI::ResourceState::RENDERTARGET;
    }

    // ---- Bind and clear ----
    const RHI::Texture* rtvs[] = { &t.id };
    cl.SetRenderTargets(1, rtvs, &t.depth);

    const float clearZero[4] = { 0.f, 0.f, 0.f, 0.f };
    cl.ClearRenderTarget(t.id, clearZero);
    cl.ClearDepthStencil(t.depth, 0.f, 0);  // reversed Z

    cl.SetViewport(vpW, vpH);
    // The readback below copies exactly ONE pixel, so everything outside a
    // tiny window around it is wasted raster + PS work. Keep the FULL viewport
    // (vertex positions must not move) and narrow only the SCISSOR, which
    // discards the rest of the coverage before the pixel shader runs. Clamped
    // to the viewport; falls back to full-screen when no pick is outstanding -
    // or when the object-ID image wants every pixel this frame.
    if (pickHere && !imageHere
        && m_pickX >= 0 && m_pickX < static_cast<int>(vpW)
        && m_pickY >= 0 && m_pickY < static_cast<int>(vpH))
    {
        const uint32_t left   = static_cast<uint32_t>(m_pickX);
        const uint32_t top    = static_cast<uint32_t>(m_pickY);
        const uint32_t right  = (left + 2 < vpW) ? left + 2 : vpW;   // exclusive
        const uint32_t bottom = (top  + 2 < vpH) ? top  + 2 : vpH;   // exclusive
        cl.GetDevice().SetScissorRect(left, top, right, bottom, cl);
    }
    else
    {
        cl.SetScissorRect(vpW, vpH);
    }
    cl.SetPrimitiveTopology();

    // ---- Draw opaque + transparent packets ----
    // Transparent meshes share the PSO (R32_UINT RTV, reverse-Z GREATER_EQUAL). Drawn after the
    // opaques into the private depth buffer, the frontmost fragment wins - the editors' rule
    // (Blender / Unity / Unreal): clicking a glass cube picks the glass, whatever its alpha.
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid())
        return cl;

    const uint64_t bindlessHandle = cl.GetBindlessTableHandle();

    cl.SetPipelineState(*pso);
    cl.BindDescriptorHeaps();
    cl.BindRootBufferSRV(kInstanceBufSlot, RG::RGSlot::InstanceBuffer);
    cl.BindRootBufferSRV(kMeshDescSlot, RG::RGSlot::MeshDescriptors);
    if (bindlessHandle)
        cl.BindDescriptorTableHandle(kBindlessSlot, bindlessHandle);
    cl.BindCB(kPerViewCBSlot, RG::RGSlot::PerView);

    auto drawList = [&](DrawFilter f)
    {
        for (const DrawPacket& dp : cl.GetContext().GetDrawList(f))
        {
            cl.SetPVFRootConstants(dp.meshDescriptorIndex,
                                   dp.instanceOffset,
                                   dp.materialIndex);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    };
    drawList(DrawFilter::Opaque);
    drawList(DrawFilter::Transparent);

    // ---- Copy the picked pixel / the whole image to readback (if requested) ----
    const bool pickCopy = pickHere
        && m_pickX >= 0 && m_pickX < static_cast<int>(vpW)
        && m_pickY >= 0 && m_pickY < static_cast<int>(vpH);
    if (pickCopy || imageHere)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(&t.id,
            RHI::ResourceState::RENDERTARGET, RHI::ResourceState::COPY_SRC));
        t.idState = RHI::ResourceState::COPY_SRC;
    }
    if (pickCopy)
    {
        cl.CopyTexturePixelToBuffer(t.id,
            static_cast<uint32_t>(m_pickX),
            static_cast<uint32_t>(m_pickY),
            m_readbackBuffer);

        m_pickRequested = false;
        m_pickPending   = true;
    }
    if (imageHere)
    {
        // The buffer was sized in RequestImage (PrepareBoundView); a texture resized since would
        // overrun it - dropped, and the harness asks again.
        IGraphicsDevice& dev = cl.GetDevice();
        uint32_t pitch = 0, rows = 0;
        uint64_t bytes = 0;
        if (dev.GetTexture2DReadbackFootprint(t.id, pitch, rows, bytes)
            && m_imageReadback.IsValid() && bytes <= m_imageBytes)
        {
            dev.CopyTexture2DToBuffer(t.id, m_imageReadback, cl);
            m_imageRowPitch = pitch;
            m_imageW        = t.w;
            m_imageH        = t.h;
            m_imagePending  = true;
        }
        else
            LOG_WARNING("PickingPass: the id image no longer fits its readback buffer (%llu > %llu bytes) - dropped",
                        static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(m_imageBytes));
        m_imageRequested = false;
    }

    return cl;
}
