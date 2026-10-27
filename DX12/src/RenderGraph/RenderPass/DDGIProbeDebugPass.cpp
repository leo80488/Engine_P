#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "ECS/ECS.h"
#include "ECS/DDGIComponents.h"
#include "System/Log.h"

#include <vector>
#include <string>

namespace
{
// Shader compiles go through the shared disk cache (Graphics/ShaderDiskCache.h), the .ishdr
// cache ShaderLibrary uses.
std::vector<uint8_t> CompileShaderFile(const std::string& path,
                                       RHI::ShaderStage stage,
                                       const std::string& entry = "main")
{
    return ShaderDiskCache::LoadOrCompile(path, entry, stage, {},
                                          "DDGIProbeDebugPass");
}

// The pass's own graphics layout (see the header).
constexpr RHI::Slots::GfxParam kPerViewCB    { 0 };   // b0 root CBV: PerViewCB
constexpr RHI::Slots::GfxParam kVolumeCB     { 1 };   // b1 root CBV: the volume CB
constexpr RHI::Slots::GfxParam kConstants    { 2 };   // b2: 4 root constants (radius, debug mode)
constexpr RHI::Slots::GfxParam kAtlasSrv     { 3 };   // t0: the irradiance atlas (PS)
constexpr RHI::Slots::GfxParam kProbeDataSrv { 4 };   // t1: the probe data (VS)
} // namespace

bool DDGIProbeDebugPass::Init(IGraphicsDevice& gfx)
{
    // ---- The pipeline layout ----
    RHI::PipelineLayoutBuilder b("DDGIProbeDebug");
    // b0 PerView CB - the VS builds the impostor quad from cameraPosWS, the PS
    // re-reads viewProj + cameraPosWS for the analytic ray-sphere hit +
    // SV_Depth, so visibility must be ALL.
    b.RootCBV(kPerViewCB, 0)
     .RootCBV(kVolumeCB, 1)                 // b1 the volume CB (VS and PS)
     // b2 root constants (sphereRadius + debugMode) - visible to BOTH the VS
     // (sphereRadius) and the PS (debugMode). VERTEX-only visibility would
     // silently break PSO creation when the PS references b2.
     .Constants(kConstants, 4, 2)
     .Table(kAtlasSrv,     { RHI::SrvRange(0) }, RHI::ShaderVisibility::PS)   // t0 the irradiance atlas
     // t1 ProbeData - the VS reads pd.offset to draw the probe at its
     // relocation-adjusted world position (what trace + sampling use).
     .Table(kProbeDataSrv, { RHI::SrvRange(1) }, RHI::ShaderVisibility::VS);
    // s0 linear CLAMP. MaxLOD stays FLT_MAX (SamplerDesc's default): a zero MaxLOD clamps every
    // sample to mip 0, harmless here only while the probe atlases have one mip (see DDGIPass).
    RHI::SamplerDesc linearClamp{};
    linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = RHI::TextureAddressMode::CLAMP;
    linearClamp.maxAnisotropy = 1;
    b.Sampler(linearClamp, 0, 0, RHI::ShaderVisibility::PS);
    // HEAP_DIRECTLY_INDEXED like every other engine signature, so these shaders follow the same
    // rules as the rest.
    b.Flags(RHI::LayoutFlags::INPUT_ASSEMBLER | RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    {
        LOG_ERROR("DDGIProbeDebugPass: pipeline layout failed");
        return false;
    }

    if (!BuildPipeline(gfx, m_pso))
    {
        LOG_ERROR("DDGIProbeDebugPass: pipeline creation failed");
        return false;
    }

    m_ready = true;
    LOG_INFO("DDGIProbeDebugPass: ready");
    return true;
}

// The one PSO, with the pass's layout. CreatePipelineState retires the handle @p outPSO held
// only on success, so a failed rebuild keeps the old pipeline.
bool DDGIProbeDebugPass::BuildPipeline(IGraphicsDevice& gfx, RHI::PipelineState& outPSO)
{
    const auto vs = CompileShaderFile("shaders/DDGIProbeDebug.vs.hlsl", RHI::ShaderStage::VS);
    const auto ps = CompileShaderFile("shaders/DDGIProbeDebug.ps.hlsl", RHI::ShaderStage::PS);
    if (vs.empty() || ps.empty()) return false;

    // Impostor quads - cull NONE, so no winding ever gets a quad rejected.
    RHI::RasterizerState rs{};
    rs.cullMode = RHI::CullMode::NONE;
    // Reversed-Z scene - "closer than the scene" PASSES so probes occlude
    // properly (GREATER_EQUAL, the engine's convention); depth is read-only.
    RHI::DepthStencilState dss{};
    dss.depthEnable     = true;
    dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;
    RHI::BlendState bs{};   // no blending, every channel written

    RHI::Shader vsh, psh;
    RHI::PipelineStateDesc pd{};
    pd.vs = &vsh; pd.ps = &psh;
    pd.rs = &rs; pd.dss = &dss; pd.bs = &bs;
    pd.pt             = RHI::PrimitiveTopology::TRIANGLELIST;
    pd.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    pd.rtvCount      = 1;
    pd.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    pd.layout         = &m_layout;
    const bool ok = gfx.CreateShader(RHI::ShaderStage::VS, vs.data(), vs.size(), vsh) &&
                    gfx.CreateShader(RHI::ShaderStage::PS, ps.data(), ps.size(), psh) &&
                    gfx.CreatePipelineState(pd, outPSO);
    gfx.DestroyShader(vsh);   // the PSO keeps its own copy of the DXIL
    gfx.DestroyShader(psh);
    return ok;
}

void DDGIProbeDebugPass::Shutdown(IGraphicsDevice& gfx)
{
    gfx.DestroyPipelineState(m_pso);
    gfx.DestroyPipelineLayout(m_layout);
    m_ready = false;
}

bool DDGIProbeDebugPass::ReloadPipelines(IGraphicsDevice& gfx)
{
    if (!m_ready) return false;
    // Hot reload runs after FlushAndWait, so the old PSO can go right away.
    if (!BuildPipeline(gfx, m_pso))
    {
        LOG_ERROR("DDGIProbeDebugPass: PSO rebuild after shader reload failed - keeping the old one");
        return false;
    }
    return true;
}

void DDGIProbeDebugPass::Execute(IGraphicsDevice&             gfx,
                                 RHI::CommandList             cmd,
                                 const RHI::Texture*          depthTex,
                                 const RHI::GPUBuffer&        perViewCB,
                                 DDGI::DDGIVolumeManager&     mgr,
                                 const RHI::DepthView&        view)
{
    if (!m_ready || !enabled) return;

    // An empty slot list means nothing asked for debug spheres: bail before touching any
    // command-list state.
    if (m_debugVolumeSlots.empty()) return;

    // Bind HDR colour + depth (read-only), with the helper LightingPass uses.
    if (depthTex)
        gfx.SetRenderTargetToHdrWithDepth(depthTex, cmd, view);

    gfx.BindPipelineState(m_pso, cmd);   // the PSO and its layout
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cmd);
    gfx.BindDescriptorHeaps(cmd);

    // PerView CB (b0): the VA the rest of the engine uses.
    gfx.SetGraphicsRootCBVAddress(kPerViewCB, gfx.GetBufferGPUAddress(perViewCB), cmd);

    // Root constants at b2: sphereRadius (float) + debugMode (uint).
    // Layout: [radius, debugMode, _pad, _pad].
    union { float f; uint32_t u; } slot[4];
    slot[0].f = sphereRadius;
    slot[1].u = debugMode;
    slot[2].u = 0u;
    slot[3].u = 0u;
    gfx.SetGraphicsRootConstants(kConstants, 4, slot, 0, cmd);

    for (const uint32_t slot : m_debugVolumeSlots)
    {
        if (slot >= DDGI::kMaxVolumes) continue;

        const uint32_t probeCount = mgr.GetProbeCount(slot);
        if (probeCount == 0) continue;

        // Volume CB (b1): this frame's ring slot.
        const RHI::GPUBuffer* cb = mgr.GetVolumeCB(gfx, slot);
        if (!cb) continue;
        const uint64_t cbVA = gfx.GetBufferGPUAddress(*cb);
        if (!cbVA) continue;
        gfx.SetGraphicsRootCBVAddress(kVolumeCB, cbVA, cmd);

        // Irradiance atlas SRV (t0), sampled at the impostor's surface normal via
        // DDGI_SampleProbeIrradiance - a bilinear octahedral tap, so the debug spheres show the stored
        // atlas with no reconstruction step that could hide or invent detail.
        uint64_t shSrv = mgr.GetIrradianceAtlasSrv(slot);
        if (shSrv == 0) continue;
        gfx.SetGraphicsDescriptorTable(kAtlasSrv, shSrv, cmd);

        // ProbeData SRV (t1, VS only). VS reads pd.offset to draw the sphere
        // at the relocation-adjusted world position so the visualization
        // matches where the probe actually traces from.
        if (uint64_t pdSrv = mgr.GetProbeDataSrv(slot))
            gfx.SetGraphicsDescriptorTable(kProbeDataSrv, pdSrv, cmd);

        // Impostor quad: 6 verts per probe (two triangles); the PS resolves the sphere analytically
        // (DDGIProbeDebug.ps.hlsl).
        constexpr uint32_t kVertsPerSphere = 6u;
        gfx.DrawInstanced(kVertsPerSphere, probeCount, 0, 0, cmd);
    }
}
