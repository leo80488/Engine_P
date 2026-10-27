#include "RenderGraph/RenderPass/GrassPass.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <cstring>

// Root parameter slots from Graphics/RootSignatureLayout.h (the single source of
// truth for the shared graphics root signature).
// ABSOLUTE root index [10], the t2 space0 descriptor table (the heightmap, ALL
// visibility): BindDescriptorTableHandle takes absolute indices.
static constexpr auto kHeightmapRootSlot = RHI::Slots::Gfx::SRV_t2;

// GROUP-RELATIVE indices: BindCB adds Gfx::CBVBase, so an absolute root index
// binds NOTHING. (The heightmap sampler is the fixed SMP_LINEAR_CLAMP heap slot
// Grass.ms names.)
static constexpr auto kPerViewCBSlot     = RHI::Slots::GfxRel::CB_b1;
static constexpr auto kGrassParamsCBSlot = RHI::Slots::GfxRel::CB_b2;

GrassPass::GrassPass(RG::RGTextureHandle albedo,
                     RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle depth,
                     RG::RGTextureHandle velocity,
                     RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface)
    , m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{}

void GrassPass::Setup(RG::RenderGraphBuilder& b)
{
    // Same writes as GBufferPass / TerrainPass: grass shares the deferred GBuffer
    // and depth-tests against scene + terrain (Renderer::Compile registers this pass
    // right after TerrainPass).
    b.WriteRenderTarget(m_albedo);
    b.WriteRenderTarget(m_normal);
    b.WriteRenderTarget(m_surface);
    b.WriteRenderTarget(m_velocity);
    b.WriteRenderTarget(m_emissive);
    // Depth written, the stencil's shading code stamped (RenderGraph/StencilBits.h);
    // with -rgplaneoff a plain depth-stencil write.
    if (RG::RenderGraph::PlanesEnabled())
        b.DepthStencil(m_depth, RG::Depth::Write, RG::Stencil::Write, ShadingStencil::kCodeMask);
    else
        b.WriteDepthStencil(m_depth);
    // The HDR scene colour, the sixth target SetRenderTargetsAndHdr appends:
    // declared here, bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
}

bool GrassPass::BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso)
{
    const RHI::Shader* as = m_shaderLib.GetShader(ShaderID::Grass_AS);
    const RHI::Shader* ms = m_shaderLib.GetShader(ShaderID::Grass_MS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Grass_PS);
    if (!as || !as->IsValid()) { LOG_ERROR("GrassPass: Grass_AS not found"); return false; }
    if (!ms || !ms->IsValid()) { LOG_ERROR("GrassPass: Grass_MS not found"); return false; }
    if (!ps || !ps->IsValid()) { LOG_ERROR("GrassPass: Grass_PS not found"); return false; }

    RHI::RasterizerState rs{};
    rs.fillMode = wireframe ? RHI::FillMode::WIREFRAME : RHI::FillMode::SOLID;
    // Blades are single-sided ribbons seen from both sides; the MS turns each
    // vertex normal toward the camera (before its up-blend), so there is no PS-side
    // SV_IsFrontFace flip - exactly one mechanism.
    rs.cullMode = RHI::CullMode::NONE;
    rs.frontCounterClockwise = false;
    rs.depthClipEnable = true;

    RHI::DepthStencilState dss{};
    dss.depthEnable     = true;
    dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed-Z
    // Stencil REPLACE ref 1 (PBR): LightingPass's PBR draw (EQUAL 1) must shade
    // grass pixels, as it does terrain.
    dss.stencilEnable      = true;
    dss.stencilReadMask   = 0xFF;
    dss.stencilWriteMask  = ShadingStencil::kCodeMask;   // the code's bits
    dss.frontFace.stencilPassOp = RHI::StencilOp::REPLACE;
    dss.frontFace.stencilFunc    = RHI::ComparisonFunc::ALWAYS;
    dss.backFace = dss.frontFace;

    RHI::BlendState bs{};
    for (int i = 0; i < 6; ++i)
        bs.renderTarget[i].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    RHI::PipelineStateDesc pd{};
    pd.as  = as;
    pd.ms  = ms;
    pd.ps  = ps;
    pd.rs  = &rs;
    pd.dss = &dss;
    pd.bs  = &bs;
    pd.il  = nullptr;
    pd.pt  = RHI::PrimitiveTopology::TRIANGLELIST;   // ignored for mesh-shader PSO
    pd.rtvCount      = 6;
    pd.rtvFormats[0] = RHI::Format::R11G11B10_FLOAT;       // albedo
    pd.rtvFormats[1] = RHI::Format::R16G16B16A16_UNORM;    // normal (.a = exact 16-bit matIdx)
    pd.rtvFormats[2] = RHI::Format::R8G8B8A8_UNORM;        // surface
    pd.rtvFormats[3] = RHI::Format::R16G16_FLOAT;          // velocity
    pd.rtvFormats[4] = RHI::Format::R16G16B16A16_FLOAT;    // extra
    pd.rtvFormats[5] = RHI::Format::R16G16B16A16_FLOAT;    // HdrSceneColor (emissive seed)
    pd.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    pd.sampleCount   = 1;

    if (!gfx.CreatePipelineState(pd, outPso))
    {
        LOG_ERROR("GrassPass: PSO creation failed");
        return false;
    }
    return true;
}

void GrassPass::Init(IGraphicsDevice& gfx)
{
    m_meshShaderAvailable = gfx.GetCapabilities().meshShader;
    if (!m_meshShaderAvailable)
    {
        LOG_WARNING("GrassPass: device reports no mesh-shader support — pass disabled");
        return;
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Grass_AS, RHI::ShaderStage::AS, "Grass.as.hlsl");
    m_shaderLib.Register(ShaderID::Grass_MS, RHI::ShaderStage::MS, "Grass.ms.hlsl");
    m_shaderLib.Register(ShaderID::Grass_PS, RHI::ShaderStage::PS, "Grass.ps.hlsl");

    // Always-needed resources FIRST: a shader compile failure below must not leave
    // the fallback heightmap missing, or a hot reload that fixes the shader would
    // still have no heightmap descriptor to bind.

    // 1x1 R16_UNORM zero heightmap so root slot 10 always has a valid Texture2D
    // descriptor (flat-field mode, or the heightmap still loading).
    {
        RHI::TextureDesc td{};
        td.format     = RHI::Format::R16_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        const uint16_t zero = 0;
        RHI::SubresourceData init{ &zero, 2, 2 };
        if (gfx.CreateTexture(td, m_fallbackHeightmap, &init))
            m_fallbackHeightmapSRV = gfx.GetTextureSRVGpuHandle(m_fallbackHeightmap);
        else
            LOG_ERROR("GrassPass: fallback heightmap creation failed");
    }

    if (!BuildPSO(gfx, /*wireframe*/false, m_pso))
        return;
    BuildPSO(gfx, /*wireframe*/true, m_psoWire);   // non-fatal

    LOG_SUCCESS("GrassPass: initialized");
}

void GrassPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_meshShaderAvailable) return;
    m_shaderLib.ClearCaches();
    BuildPSO(gfx, /*wireframe*/false, m_pso);
    BuildPSO(gfx, /*wireframe*/true,  m_psoWire);
}

// Keep in lockstep with Execute's early-outs. Both read state settled before
// the graph runs (Init, and Renderer's BuildScene_SyncGrass).
bool GrassPass::IsActive() const
{
    if (!m_meshShaderAvailable || !m_pso.IsValid()) return false;
    if (m_field.dispatchAsGroupCount == 0)          return false;
    return true;
}

void GrassPass::BindView(ViewState& view)
{
    m_viewCamera = view.Slot() == 0 ? nullptr : &view.Data<ViewCamera>(this);
}

void GrassPass::SetViewCamera(ViewState& view, const float cameraPos[3], const float frustumPlanes[6][4])
{
    if (view.Slot() == 0) return;
    ViewCamera& c = view.Data<ViewCamera>(this);
    std::memcpy(c.cameraPos,     cameraPos,     sizeof(c.cameraPos));
    std::memcpy(c.frustumPlanes, frustumPlanes, sizeof(c.frustumPlanes));
}

RHI::CommandList GrassPass::Execute(RHI::CommandList cl)
{
    if (!m_meshShaderAvailable || !m_pso.IsValid()) return cl;
    if (m_field.dispatchAsGroupCount == 0)          return cl;

    // This frame's CB (pushed by Renderer in BuildScene_SyncGrass), in a block of
    // the frame's constants; none left (reported) -> no draw.
    const RHI::Constants<GrassCBData> cb = cl.AllocConstants<GrassCBData>();
    if (!cb) return cl;
    GrassCBData data = m_pendingCB;
    if (m_viewCamera)
    {
        std::memcpy(data.cameraPos,     m_viewCamera->cameraPos,     sizeof(data.cameraPos));
        std::memcpy(data.frustumPlanes, m_viewCamera->frustumPlanes, sizeof(data.frustumPlanes));
    }
    *cb.cpu = data;

    // GBufferPass already cleared the RTs; just (re)attach them.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.SetPipelineState((m_wireframe && m_psoWire.IsValid()) ? m_psoWire : m_pso);
    cl.BindDescriptorHeaps();

    cl.BindCB(kPerViewCBSlot, RG::RGSlot::PerView);
    cl.BindCBAddress(kGrassParamsCBSlot, cb.gpu);

    const uint64_t hmSRV = m_field.heightmapSRV ? m_field.heightmapSRV
                                                : m_fallbackHeightmapSRV;
    if (!hmSRV) return cl;
    cl.BindDescriptorTableHandle(kHeightmapRootSlot, hmSRV);

    // Grass has NO MaterialComponent behind it (like TerrainPass): stamp PBR.
    cl.gfx->SetStencilRef(ShadingStencil::kPBR, cl);

    cl.DispatchMesh(m_field.dispatchAsGroupCount, 1, 1);
    return cl;
}
