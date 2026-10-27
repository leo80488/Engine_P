#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"
#include "System/Check.h"   // ENGINE_CHECK_ONCE: the lazy star cube create retries every frame

#include <cstring>

// Slot values come from RHI::Slots; the local names stay because three of these
// slots are repurposed and the RHI name would describe the wrong resource.
// Cube vertex ByteAddressBuffer (t2 space0). Group-relative: BindBufferSRV adds Gfx::SRVBase.
static constexpr auto kCubeVBSlot     = RHI::Slots::GfxRel::SRV_t2; // -> root param 10 -> t2
// Root parameter slot for environment cubemap (t6 space0).
static constexpr auto kEnvMapRootSlot = RHI::Slots::Gfx::SRV_t6;
// The moon Texture2D reuses the gEmissive slot (t17 space0, root 29): Skybox runs
// after Lighting, and Lighting rebinds gEmissive next frame.
static constexpr auto kMoonTexRootSlot = RHI::Slots::Gfx::EmissiveSRV;
// The stars cubemap reuses root 30 (t18 space0), Lighting's SSAO slot: Skybox does
// not read SSAO, and Lighting rebinds it next frame.
static constexpr auto kStarsTexRootSlot = RHI::Slots::Gfx::SSAOSRV;

// Compute root sig slots used by the bake pass (space2).
static constexpr auto kComputeCBSlot = RHI::Slots::Cmp::CB;
static constexpr auto kComputeUAV0   = RHI::Slots::Cmp::UAV_u0;

struct alignas(16) StarsBakeCBData
{
    uint32_t cubeSize;
    float    starDensity;
    float    starBrightnessBake;
    uint32_t _pad0;
};

// Pre-expanded unit cube: 36 float3 positions in -1..+1 (6 faces x 2 triangles x 3 verts).
// The vertex is the cubemap sampling direction (w=0 strips the camera translation).
static const float kCubePositions[36][3] =
{
    // +X face
    { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1},
    { 1,-1,-1}, { 1, 1, 1}, { 1,-1, 1},
    // -X face
    {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1},
    {-1,-1, 1}, {-1, 1,-1}, {-1,-1,-1},
    // +Y face
    {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1},
    {-1, 1, 1}, { 1, 1,-1}, {-1, 1,-1},
    // -Y face
    {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1},
    {-1,-1,-1}, { 1,-1, 1}, {-1,-1, 1},
    // +Z face
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1},
    {-1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    // -Z face
    { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1},
    { 1,-1,-1}, {-1, 1,-1}, { 1, 1,-1},
};

SkyboxPass::SkyboxPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

// See the header: RenderGraph keys three things on GetName().
void SkyboxPass::SetNameSuffix(const char* suffix)
{
    // An empty suffix would reproduce the main instance's name (first-match-wins
    // timestamp scan, one shared external-wait key, one merged PIX region): keep the
    // name and report instead of making a duplicate.
    if (!ENGINE_CHECK(suffix && *suffix,
                      "SkyboxPass::SetNameSuffix: empty suffix would duplicate '%s'",
                      kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

void SkyboxPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth is only tested, so it is declared a read: the plane stays in a state the
    // test accepts. With -rgplaneoff it is declared a write and the graph brings depth
    // to DEPTHSTENCIL.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Read);
    else                                  b.WriteDepthStencil(m_depth);
    // No builtin colour target: the HDR scene colour is declared (the graph brings it to
    // RENDER_TARGET, never binds or clears it) and Execute binds it with depth.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

// Also the teardown of a second (preview) instance. It owns everything it uses (a
// library of three fixed shaders, no dynamic ids), so nothing here is gated.
// ReleaseStarsResources is null-tolerant; the lazily created stars cubemap
// (1024^2 x6 RGBA16F) is the only large release.
void SkyboxPass::Shutdown(IGraphicsDevice& gfx)
{
    ReleaseStarsResources();

    m_psoCache.Shutdown();
    m_shaderLib.Shutdown(gfx);
    m_skyCB.Destroy(gfx);

    if (m_cubeVB.IsValid()) gfx.DestroyBuffer(m_cubeVB);
    if (m_starsBakePSO.IsValid()) gfx.DestroyPipelineState(m_starsBakePSO);
    // UNMAP BEFORE DESTROY. DestroyBuffer defers the resource, but the mapped
    // pointer is ours and a stale one outliving the Unmap is a write into freed
    // address space the debug layer cannot see.
    if (m_starsBakeCBMapped)
    {
        gfx.UnmapBuffer(m_starsBakeCB);
        m_starsBakeCBMapped = nullptr;
    }
    if (m_starsBakeCB.IsValid()) gfx.DestroyBuffer(m_starsBakeCB);

    m_gfx              = nullptr;
}

void SkyboxPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;   // EnsureStarsResources creates/releases the star cube later

    // No sampler: Skybox.ps filters the env and star cubes with the fixed
    // SMP_LINEAR_WRAP heap slot (the cube face math handles the wrap-around).

    // Upload the cube vertex buffer (ByteAddressBuffer, 36 x float3).
    {
        RHI::GPUBufferDesc d;
        d.size       = sizeof(kCubePositions);
        d.usage      = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(d, m_cubeVB, kCubePositions))
            LOG_ERROR("SkyboxPass: failed to create cube vertex buffer");
    }

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Skybox_VS,    RHI::ShaderStage::VS, "Skybox.vs.hlsl");
    m_shaderLib.Register(ShaderID::Skybox_PS,    RHI::ShaderStage::PS, "Skybox.ps.hlsl");
    m_shaderLib.Register(ShaderID::StarsBake_CS, RHI::ShaderStage::CS,
                         "StarsBake.cs.hlsl",   "CSMain");
    m_psoCache.Init(gfx, m_shaderLib);

    // Star bake compute PSO and constant buffer.
    {
        CreateStarsBakePipeline(gfx);

        // The 48 MiB cubemap is not created here: EnsureStarsResources creates it on the
        // first night frame (see kStarsCubeSize in the header). Only the 256 B bake CB is
        // eager: it costs nothing, and the lazy path stays one resource.
        RHI::GPUBufferDesc bd{};
        bd.size       = (sizeof(StarsBakeCBData) + 255) & ~255u;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
        if (gfx.CreateBuffer(bd, m_starsBakeCB))
            m_starsBakeCBMapped = gfx.MapBuffer(m_starsBakeCB);
    }

    // SkyCB: the per-frame UPLOAD CB (b2) for the sun, moon and star parameters.
    if (!m_skyCB.Create(gfx, "SkyboxPass.SkyCB"))
        LOG_ERROR("SkyboxPass: SkyCB creation failed");

    if (!m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_ERROR("SkyboxPass: PSO creation failed");
    else
        LOG_INFO("SkyboxPass: PSO ready");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
void SkyboxPass::CreateStarsBakePipeline(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::StarsBake_CS);
    if (!cs) return;
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_starsBakePSO))
        LOG_ERROR("SkyboxPass: stars bake PSO failed");
}

void SkyboxPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_shaderLib.ClearCaches()) return;   // none of its shaders changed
    m_psoCache.Clear();          // the skybox draw: rebuilt at its next Execute
    CreateStarsBakePipeline(gfx);
    m_starsBaked = false;        // the cube holds the old StarsBake.cs output
}

// Pure CPU state plus the baked star cube's SRV handle (a stable resource view,
// not per-frame state).
SkyboxPass::SkyState SkyboxPass::GetSkyState(IGraphicsDevice& gfx) const
{
    SkyState s;
    s.valid            = true;
    s.sunDir           = m_sunDir;
    s.sunColor         = m_sunColor;
    s.sunDiskCos       = m_sunDiskCos;
    s.sunDiskIntensity = m_sunDiskIntensity;
    s.moonTex          = m_moonTexHandle;
    s.moonVisible      = m_moonVisible;
    s.moonDir          = m_moonDir;
    s.moonColor        = m_moonColor;
    s.moonDiskCos      = m_moonDiskCos;
    s.starIntensity    = m_starIntensity;
    s.starTime         = m_starTime;
    s.starDensity      = m_starDensity;
    s.starBrightness   = m_starBrightness;
    // Only when baked: an allocated-but-unbaked cube holds undefined content, and the
    // owner's own Execute does not bind it either (the starsReady interlock).
    if (m_starsBaked && m_starsCubemap.IsValid())
        s.starsCubeSrv = gfx.GetTextureSRVGpuHandle(m_starsCubemap);
    return s;
}

void SkyboxPass::ApplySkyState(const SkyState& s)
{
    if (!s.valid) return;
    m_sunDir           = s.sunDir;
    m_sunColor         = s.sunColor;
    m_sunDiskCos       = s.sunDiskCos;
    m_sunDiskIntensity = s.sunDiskIntensity;
    m_moonTexHandle    = s.moonTex;
    m_moonVisible      = s.moonVisible;
    m_moonDir          = s.moonDir;
    m_moonColor        = s.moonColor;
    m_moonDiskCos      = s.moonDiskCos;
    m_starIntensity    = s.starIntensity;
    m_starTime         = s.starTime;
    m_starDensity      = s.starDensity;
    m_starBrightness   = s.starBrightness;
    m_borrowedStarsSrv = s.starsCubeSrv;
}

void SkyboxPass::EnsureStarsResources()
{
    if (!m_gfx) return;

    const bool want = (m_starIntensity > 0.0f);

    if (want)
    {
        if (m_starsCubemap.IsValid()) return;

        RHI::TextureDesc td{};
        td.width      = kStarsCubeSize;
        td.height     = kStarsCubeSize;
        td.arraySize = 6;
        td.mipLevels = 1;
        // RGBA16F: alpha holds the per-star twinkle phase (0 = no star, >0 = phase in
        // (0, 1]); the runtime shader modulates each star's brightness with it.
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        // ONCE: retried every frame the night sky is up, so a persistent
        // failure would otherwise write one line per frame.
        if (!ENGINE_CHECK_ONCE(m_gfx->CreateTexture(td, m_starsCubemap),
                               "SkyboxPass: stars cubemap create failed (%ux%u x6 "
                               "RGBA16F) — starfield disabled",
                               kStarsCubeSize, kStarsCubeSize))
        {
            return;
        }
        // A fresh UAV-state texture holds garbage: Execute re-runs the bake.
        m_starsCubemapState = RHI::ResourceState::UNORDERED_ACCESS;
        m_starsBaked        = false;
        LOG_INFO("SkyboxPass: stars cubemap allocated (%.1f MiB, night sky active)",
                 static_cast<double>(static_cast<uint64_t>(kStarsCubeSize) * kStarsCubeSize
                                     * 6ull * 8ull) / (1024.0 * 1024.0));
        return;
    }

    // Daylight does not release: the cube is per-world sticky (ReleaseStarsResources).
    // A day/night cycle crosses zero intensity every in-game dawn, so an idle timer
    // would free 48 MiB and re-bake every game day, and a frame-counted timer depends
    // on the display refresh rate. A scene that never renders night pays nothing.
}

void SkyboxPass::ReleaseStarsResources()
{
    // Called on a real scene change (OnWorldClear without worldSurvives) and at
    // Shutdown; the bake-preview clear keeps the world and its sky rendering.
    if (!m_gfx || !m_starsCubemap.IsValid()) return;
    m_gfx->DestroyTexture(m_starsCubemap);   // deferred kFrameCount frames
    m_starsCubemap      = {};
    m_starsCubemapState = RHI::ResourceState::UNORDERED_ACCESS;
    m_starsBaked        = false;
    LOG_INFO("SkyboxPass: stars cubemap released (%.1f MiB reclaimed, world cleared)",
             static_cast<double>(static_cast<uint64_t>(kStarsCubeSize) * kStarsCubeSize
                                 * 6ull * 8ull) / (1024.0 * 1024.0));
}

PSODesc SkyboxPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID       = ShaderID::Skybox_VS;
    desc.psID       = ShaderID::Skybox_PS;
    desc.inputLayout = InputLayoutType::None;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = true;

    // Reversed-Z: the skybox VS outputs NDC z=0 (far plane). GREATER_EQUAL passes
    // 0 >= 0 only where no opaque geometry has written a closer (>0) depth.
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;

    desc.bs.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    return desc;
}

RHI::CommandList SkyboxPass::Execute(RHI::CommandList cl)
{
    if (!Shown()) return cl;   // the view hides the sky: keep the background black
    if (!m_envMapGpuHandle) return cl; // no cubemap assigned yet
    if (!m_cubeVB.IsValid()) return cl;

    auto& gfx = cl.GetDevice();

    // One-time star cubemap bake: the first Execute after the cube is created (or a
    // shader reload) dispatches the procedural star generator into all 6 faces and
    // leaves the cube in SHADER_RESOURCE; later frames only sample.
    if (!m_starsBaked && m_starsBakePSO.IsValid() && m_starsCubemap.IsValid())
    {
        if (m_starsBakeCBMapped)
        {
            StarsBakeCBData c{};
            c.cubeSize           = kStarsCubeSize;
            c.starDensity        = m_starDensity;
            c.starBrightnessBake = m_starBrightness;
            std::memcpy(m_starsBakeCBMapped, &c, sizeof(c));
        }

        // A re-bake after a shader hot reload finds the cube in SHADER_RESOURCE.
        if (m_starsCubemapState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_starsCubemap, m_starsCubemapState,
                RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_starsCubemapState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_starsBakePSO, cl);
        gfx.SetComputeRootCBV(kComputeCBSlot, m_starsBakeCB, cl);
        gfx.SetComputeDescriptorTable(kComputeUAV0,
            gfx.GetTextureUAVGpuHandle(m_starsCubemap), cl);

        // One thread per (texel, face): 8x8 thread groups, z = 6.
        gfx.DispatchCompute((kStarsCubeSize + 7) / 8,
                            (kStarsCubeSize + 7) / 8, 6, cl);

        // UAV -> SRV for skybox sampling.
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_starsCubemap,
            RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), cl);
        m_starsCubemapState = RHI::ResourceState::SHADER_RESOURCE;
        m_starsBaked = true;
        LOG_INFO("SkyboxPass: stars cubemap baked (%ux%u × 6 faces)",
                 kStarsCubeSize, kStarsCubeSize);
    }

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(BuildPSODesc());
    if (!pso || !pso->IsValid()) return cl;

    // Bind HDR RTV + depth (no clear), the DSV as Setup declared it (a read:
    // read-only where the depth plane sits in a DEPTH_READ state).
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));

    // Upload the latest sun / moon / star parameters into SkyCB (b2).
    if (auto* slot = m_skyCB.Current(gfx))
    {
        SkyCBData d{};
        d.sunDir[0] = m_sunDir.x;     d.sunDir[1] = m_sunDir.y;     d.sunDir[2] = m_sunDir.z;
        d.sunDiskSize = m_sunDiskCos;
        d.sunColor[0] = m_sunColor.x; d.sunColor[1] = m_sunColor.y; d.sunColor[2] = m_sunColor.z;
        d.sunDiskIntensity = m_sunDiskIntensity;
        d.moonDir[0]    = m_moonDir.x;   d.moonDir[1] = m_moonDir.y;   d.moonDir[2] = m_moonDir.z;
        d.moonDiskCos   = m_moonDiskCos;
        d.moonColor[0]  = m_moonColor.x; d.moonColor[1] = m_moonColor.y; d.moonColor[2] = m_moonColor.z;
        d.moonVisible   = (m_moonVisible && m_moonTexHandle) ? 1.0f : 0.0f;
        // The PS reads gStarsTex (t18) only when StarIntensity > 0: this interlock keeps
        // it off the slot until a TextureCube is bound there (t18 otherwise still holds
        // Lighting's SSAO Texture2D; sampling that as a cube is undefined). Ready = its own
        // cube is baked, or a borrowed one is set: a preview instance binds the main pass's
        // baked cube, which stays SHADER_RESOURCE for the life of the world and is written
        // earlier in the frame (the main graph's tail precedes previewCL). Costs at most one
        // frame of stars at the start of dusk, where the day/night blend is still ~0.
        const bool starsReady = (m_starsBaked && m_starsCubemap.IsValid()) ||
                                (m_borrowedStarsSrv != 0);
        d.starIntensity = starsReady ? m_starIntensity : 0.0f;
        d.starTime      = m_starTime;
        d.starDensity   = m_starDensity;
        d.starBrightness= m_starBrightness;
        *slot = d;
    }

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);
    cl.BindCB(RHI::Slots::GfxRel::CB_b1, RG::RGSlot::PerView); // b1: viewProj for the cube transform
    cl.BindCB(RHI::Slots::GfxRel::CB_b2, RG::RGSlot::SkyCB);   // b2: sun / moon / star params
    cl.BindBufferSRV(kCubeVBSlot, m_cubeVB); // t2: cube ByteAddressBuffer
    cl.BindDescriptorTableHandle(kEnvMapRootSlot, m_envMapGpuHandle); // t6: TextureCube
    // Moon texture at t17, gEmissive's slot (Lighting rebinds gEmissive next frame).
    // Bound whenever there is one; the shader samples it only when moonVisible is set.
    if (m_moonTexHandle)
        cl.BindDescriptorTableHandle(kMoonTexRootSlot, m_moonTexHandle);
    // Stars cubemap at t18, Lighting's SSAO slot (Skybox does not read SSAO; Lighting
    // rebinds it next frame).
    if (m_starsBaked && m_starsCubemap.IsValid())
    {
        cl.BindDescriptorTableHandle(kStarsTexRootSlot,
            cl.GetDevice().GetTextureSRVGpuHandle(m_starsCubemap));
    }
    else if (m_borrowedStarsSrv)   // borrowed: the main instance's baked cube
    {
        cl.BindDescriptorTableHandle(kStarsTexRootSlot, m_borrowedStarsSrv);
    }

    cl.DrawInstanced(36, 1, 0, 0);
    return cl;
}
