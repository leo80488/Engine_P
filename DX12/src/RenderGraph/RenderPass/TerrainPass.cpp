#include "RenderGraph/RenderPass/TerrainPass.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

// Root parameter slots, from Graphics/RootSignatureLayout.h (the shared graphics
// root signature). Absolute root indices (BindDescriptorTableHandle takes absolute):
//   [10] descriptor table, t2 space0 <- per-draw SRV slot 0 (heightmap, ALL vis)
//   [11] descriptor table, t3 space0 <- per-draw SRV slot 1 (splatmap, ALL vis)
//   [12] descriptor table, t4 space0 <- per-draw SRV slot 2 (layer table, ALL vis)
//   [27] descriptor table, t0 space2 <- bindless g_AllTextures[] (PIXEL vis)
// Params 10..13 (t2..t5 space0) are the generic per-draw SRV tables the root
// signature declares, so the layer StructuredBuffer needs no root-sig change.
static constexpr auto kHeightmapRootSlot = RHI::Slots::Gfx::SRV_t2;
static constexpr auto kSplatmapRootSlot  = RHI::Slots::Gfx::SRV_t3;
static constexpr auto kLayerBufRootSlot  = RHI::Slots::Gfx::SRV_t4;
static constexpr auto kBindlessTexSlot   = RHI::Slots::Gfx::BindlessTex;

// Group-relative indices: BindCB adds Gfx::CBVBase. Passing an absolute root index
// to it binds nothing.
static constexpr auto kPerViewCBSlot       = RHI::Slots::GfxRel::CB_b1;
static constexpr auto kTerrainParamsCBSlot = RHI::Slots::GfxRel::CB_b2;

TerrainPass::TerrainPass(RG::RGTextureHandle albedo,
                         RG::RGTextureHandle normal,
                         RG::RGTextureHandle surface,
                         RG::RGTextureHandle depth,
                         RG::RGTextureHandle velocity,
                         RG::RGTextureHandle emissive)
    : m_albedo(albedo), m_normal(normal), m_surface(surface)
    , m_depth(depth), m_velocity(velocity), m_emissive(emissive)
{}

void TerrainPass::Setup(RG::RenderGraphBuilder& b)
{
    // Same writes as GBufferPass: terrain shares the deferred GBuffer. This pass is
    // registered after GBufferPass (Renderer::Compile), so it runs strictly after it and
    // the depth test against scene geometry is correct.
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
    // The HDR scene colour, the sixth target SetRenderTargetsAndHdr appends: declared
    // here, bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
}

bool TerrainPass::BuildPSO(IGraphicsDevice& gfx, bool wireframe, RHI::PipelineState& outPso)
{
    const RHI::Shader* as = m_shaderLib.GetShader(ShaderID::Terrain_AS);
    const RHI::Shader* ms = m_shaderLib.GetShader(ShaderID::Terrain_MS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Terrain_PS);
    if (!as || !as->IsValid())
    { LOG_ERROR("TerrainPass: Terrain_AS not found"); return false; }
    if (!ms || !ms->IsValid())
    { LOG_ERROR("TerrainPass: Terrain_MS not found"); return false; }
    if (!ps || !ps->IsValid())
    { LOG_ERROR("TerrainPass: Terrain_PS not found"); return false; }

    RHI::RasterizerState rs{};
    rs.fillMode         = wireframe ? RHI::FillMode::WIREFRAME : RHI::FillMode::SOLID;
    rs.cullMode         = RHI::CullMode::BACK;
    rs.frontCounterClockwise = false;
    rs.depthClipEnable = true;

    RHI::DepthStencilState dss{};
    dss.depthEnable     = true;
    dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;   // reversed-Z (matches GBufferPass)
    // Stencil: REPLACE with the PBR shading-model id (1) so LightingPass's PBR pass
    // (stencilFunc EQUAL, ref 1) shades terrain pixels. Without it all three lighting
    // branches skip terrain (stencil 0 != 1/2/3), the HDR target keeps last frame's
    // contents and the screen "stacks".
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
    pd.as = as;
    pd.ms = ms;
    pd.ps = ps;
    pd.rs  = &rs;
    pd.dss = &dss;
    pd.bs  = &bs;
    pd.il  = nullptr;
    pd.pt  = RHI::PrimitiveTopology::TRIANGLELIST;   // ignored for mesh-shader PSO
    pd.rtvCount    = 6;
    pd.rtvFormats[0] = RHI::Format::R11G11B10_FLOAT;       // albedo
    pd.rtvFormats[1] = RHI::Format::R16G16B16A16_UNORM;    // normal (.a = exact 16-bit matIdx)
    pd.rtvFormats[2] = RHI::Format::R8G8B8A8_UNORM;        // surface
    pd.rtvFormats[3] = RHI::Format::R16G16_FLOAT;          // velocity
    pd.rtvFormats[4] = RHI::Format::R16G16B16A16_FLOAT;    // extra (shading-model scratch)
    pd.rtvFormats[5] = RHI::Format::R16G16B16A16_FLOAT;    // HdrSceneColor (direct emissive write)
    pd.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;     // matches GBufferPass
    pd.sampleCount   = 1;

    if (!gfx.CreatePipelineState(pd, outPso))
    {
        LOG_ERROR("TerrainPass: PSO creation failed");
        return false;
    }
    return true;
}

void TerrainPass::Init(IGraphicsDevice& gfx)
{
    // Mesh shaders are an optional backend feature: without runtime support m_pso stays
    // invalid and Execute is a silent no-op.
    m_meshShaderAvailable = gfx.GetCapabilities().meshShader;
    if (!m_meshShaderAvailable)
    {
        LOG_WARNING("TerrainPass: device reports no mesh-shader support — pass disabled");
        return;
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Terrain_AS, RHI::ShaderStage::AS, "Terrain.as.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_MS, RHI::ShaderStage::MS, "Terrain.ms.hlsl");
    m_shaderLib.Register(ShaderID::Terrain_PS, RHI::ShaderStage::PS, "Terrain.ps.hlsl");

    if (!BuildPSO(gfx, /*wireframe*/false, m_pso))
        return;
    // Wireframe variant for the global view-mode switch. Non-fatal if it fails:
    // Execute falls back to the solid PSO.
    BuildPSO(gfx, /*wireframe*/true, m_psoWire);

    // The heightmap (MS) and splatmap (PS, sampled by the tile-local UV in [0,1]) use
    // the fixed SMP_LINEAR_CLAMP heap slot: clamp keeps the four-tap derivative stencil
    // from wrapping to the opposite edge of the heightmap.
    //
    // The pass's run-time sampler (SetDynamicSampler) is anisotropic-wrap for the layer
    // albedos: sampled at worldXZ x per-layer tilingScale, the UV runs over many integer
    // periods and must wrap to tile. Anisotropic filtering keeps grazing-angle detail
    // crisp (MaterialSampler, as in GBufferPass); terrain keeps its milder native bias,
    // kTerrainNativeBias, under the same upscale rule.
    if (!gfx.CreateSampler(MaterialSampler::Desc(m_textureAnisotropy, m_mipBias), m_wrapSamplerIdx))
        LOG_ERROR("TerrainPass: failed to create anisotropic-wrap sampler");

    LOG_SUCCESS("TerrainPass: initialized");
}

void TerrainPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_meshShaderAvailable) return;
    m_shaderLib.ClearCaches();
    BuildPSO(gfx, /*wireframe*/false, m_pso);
    BuildPSO(gfx, /*wireframe*/true,  m_psoWire);
}

// Keep in lockstep with Execute's early-outs. Both read state settled before the
// graph runs (Init, and the Renderer's SetActiveTile).
bool TerrainPass::IsActive() const
{
    if (!m_meshShaderAvailable || !m_pso.IsValid())                   return false;
    if (m_tile.dispatchAsGroupCount == 0 || m_tile.heightmapSRV == 0) return false;
    return true;
}

RHI::CommandList TerrainPass::Execute(RHI::CommandList cl)
{
    if (!m_meshShaderAvailable || !m_pso.IsValid())                  return cl;
    if (m_tile.dispatchAsGroupCount == 0 || m_tile.heightmapSRV == 0) return cl;

    // GBufferPass already cleared the RTs (HDR RT5 included); only re-attach them.
    cl.SetRenderTargetsAndHdr({ m_albedo, m_normal, m_surface, m_velocity, m_emissive }, m_depth);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.SetPipelineState((m_wireframe && m_psoWire.IsValid()) ? m_psoWire : m_pso);
    cl.BindDescriptorHeaps();

    // CBs: the slot is CB-relative; BindCB(0, ...) -> root b1, (1, ...) -> root b2.
    cl.BindCB(kPerViewCBSlot, RG::RGSlot::PerView);
    cl.BindCB(kTerrainParamsCBSlot, RG::RGSlot::TerrainParams);

    // Heightmap SRV at t2 space0 (required).
    cl.BindDescriptorTableHandle(kHeightmapRootSlot, m_tile.heightmapSRV);

    // Splatmap SRV at t3 space0 (optional: the PS branches on g_hasSplatmap). Unset,
    // the slot still needs a valid descriptor for D3D12 validation: reuse the heightmap
    // as a benign placeholder.
    const uint64_t splatBind = m_tile.splatmapSRV ? m_tile.splatmapSRV
                                                  : m_tile.heightmapSRV;
    cl.BindDescriptorTableHandle(kSplatmapRootSlot, splatBind);

    // Per-layer material table at t4 space0 (the PS loops over it by layerCount). Unset,
    // bind a benign valid descriptor (splatmap / heightmap) so D3D12 validation never
    // sees an empty table; the PS guards on layerCount.
    const uint64_t layerBind = m_tile.layerBufferSRV ? m_tile.layerBufferSRV
                                                     : splatBind;
    cl.BindDescriptorTableHandle(kLayerBufRootSlot, layerBind);

    // Bindless texture array for layer albedo lookups (PS only).
    if (const uint64_t bindlessTex = cl.gfx->GetBindlessTextureTableGpuHandle();
        bindlessTex != 0)
    {
        cl.BindDescriptorTableHandle(kBindlessTexSlot, bindlessTex);
    }

    // The layer albedos' wrap sampler; the clamp one is a fixed heap slot.
    if (m_wrapSamplerIdx >= 0)
        cl.SetDynamicSampler(m_wrapSamplerIdx);   // Terrain.ps's material sampler

    // Terrain has no MaterialComponent behind it, so the material routing never finds
    // this code: it is stamped here, by name.
    cl.gfx->SetStencilRef(ShadingStencil::kPBR, cl);

    // DispatchMesh dispatches AS thread groups when an AS is bound; the AS
    // performs frustum culling per sub-tile and calls its own DispatchMesh
    // to emit only the surviving MS groups (see Terrain.as.hlsl).
    cl.DispatchMesh(m_tile.dispatchAsGroupCount, 1, 1);

    return cl;
}

// The material sampler's LOD bias for the current render / display widths
// (Graphics/MaterialSampler.h).
void TerrainPass::SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW)
{
    MaterialSampler::Rebuild(gfx, m_textureAnisotropy,
                             MaterialSampler::Bias(MaterialSampler::kTerrainNativeBias, renderW, displayW),
                             m_wrapSamplerIdx, m_mipBias);
}
