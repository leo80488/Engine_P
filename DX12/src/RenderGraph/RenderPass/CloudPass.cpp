#include "RenderGraph/RenderPass/CloudPass.h"
#include "ECS/CloudComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "System/Check.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>
#include <utility>

using namespace DirectX;

// Slot values come from RHI::Slots (Graphics/RootSignatureLayout.h); the local names say
// what this pass puts in each generic slot of the shared compute root signature.
static constexpr auto kCBSlot   = RHI::Slots::Cmp::CB;      // b0 space2
static constexpr auto kSRVT0    = RHI::Slots::Cmp::SRV_t0;  // t0 space2: base shape noise
static constexpr auto kSRVT1    = RHI::Slots::Cmp::SRV_t1;  // t1 space2: detail erosion noise
static constexpr auto kSRVT2    = RHI::Slots::Cmp::SRV_t2;  // t2 space2: weather map
static constexpr auto kSRVT3    = RHI::Slots::Cmp::SRV_t3;  // t3 space2: scene depth (as VolumetricFogPass)
static constexpr auto kUAV0     = RHI::Slots::Cmp::UAV_u0;  // u0 space2
static constexpr auto kUAV1     = RHI::Slots::Cmp::UAV_u1;  // u1 space2: per-texel march distance

// Graphics root slots for the composite PS, re-purposed the way VolumetricFogPass does for
// its half-res raymarch result.
static constexpr auto kEnvMapRootSlot    = RHI::Slots::Gfx::SRV_t6; // t6 space0
static constexpr auto kCloudDistRootSlot = RHI::Slots::Gfx::SRV_t7; // t7 space0 (second IBL slot)

using CloudConstants = CloudPass::CloudConstants;

static void TransitionTex(IGraphicsDevice& gfx,
                          const RHI::Texture& tex,
                          RHI::ResourceState& state,
                          RHI::ResourceState to,
                          RHI::CommandList cl)
{
    if (state == to) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}

CloudPass::CloudPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

// Destroys the pass's own textures, not the accumulation (History::accum): that is the view's,
// and ~Renderer releases the view histories before the graph's passes are destroyed.
CloudPass::~CloudPass()
{
    if (m_gfx)
    {
        if (m_baseNoiseTex.IsValid())   m_gfx->DestroyTexture(m_baseNoiseTex);
        if (m_detailNoiseTex.IsValid()) m_gfx->DestroyTexture(m_detailNoiseTex);
        if (m_weatherTex.IsValid())     m_gfx->DestroyTexture(m_weatherTex);
        if (m_mainFrame.cloudTex.IsValid())     m_gfx->DestroyTexture(m_mainFrame.cloudTex);
        if (m_mainFrame.cloudDistTex.IsValid()) m_gfx->DestroyTexture(m_mainFrame.cloudDistTex);
        if (m_cloudShadowTex.IsValid()) m_gfx->DestroyTexture(m_cloudShadowTex);
    }
}

void CloudPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
    if (view.Slot() == 0)
    {
        m_mainHist = m_hist;
        m_f        = &m_mainFrame;
    }
    else
        m_f = &view.Data<ViewFrame>(this).frame;
}

void CloudPass::ViewFrame::Release(IGraphicsDevice& gfx)
{
    if (frame.cloudTex.IsValid())     gfx.DestroyTexture(frame.cloudTex);
    if (frame.cloudDistTex.IsValid()) gfx.DestroyTexture(frame.cloudDistTex);
    frame = Frame{};
}

void CloudPass::History::Release(IGraphicsDevice& gfx)
{
    for (auto& t : accum)
    {
        if (t.IsValid()) gfx.DestroyTexture(t);
        t = {};
    }
    valid = false;
}

// The import points at the member, so a map EnsureCloudShadowTexture creates in Execute is
// picked up next frame; that first frame it is created UNORDERED_ACCESS, baked and left there.
// Afterwards the graph moves it to UNORDERED_ACCESS in this pass's Phase A and back to
// SHADER_RESOURCE in the next declared reader's (VolumetricFog or Transparent this frame,
// else Lighting next frame).
void CloudPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_cloudShadowH = d.Import(RG::PassTex::kCloudShadow,
        { RHI::Format::R16_FLOAT, kCloudShadowDim, kCloudShadowDim, false, true },
        { &m_cloudShadowTex, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
}

void CloudPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_depth);                            // raymarch samples scene depth
    // The DSV is bound with the depth test off (the fullscreen PSO's format match): a plane the
    // DSV does not use.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::None);
    // The HDR scene colour: declared so Phase A brings it to RENDER_TARGET (it never binds or
    // clears it); Composite binds it with depth, as SkyboxPass / VolumetricFogPass do.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    // Not BuiltinTexture::HdrSceneColor: that colour target makes the graph clear the HDR scene
    // colour on entry and wipe Lighting + Skybox before the composite draws over them.
    b.SetColorTarget(RG::BuiltinTexture::None);
    b.WriteUAV(m_cloudShadowH);   // the graph transitions the map
}

void CloudPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Shader registration ----
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::CloudNoiseBake_CS,       RHI::ShaderStage::CS,
                         "CloudNoiseBake.cs.hlsl",       "main");
    m_shaderLib.Register(ShaderID::CloudDetailNoiseBake_CS, RHI::ShaderStage::CS,
                         "CloudDetailNoiseBake.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::CloudWeatherBake_CS,     RHI::ShaderStage::CS,
                         "CloudWeatherBake.cs.hlsl",     "main");
    m_shaderLib.Register(ShaderID::CloudRaymarch_CS,        RHI::ShaderStage::CS,
                         "CloudRaymarch.cs.hlsl",        "main");
    m_shaderLib.Register(ShaderID::CloudTemporal_CS,        RHI::ShaderStage::CS,
                         "CloudTemporal.cs.hlsl",        "main");
    m_shaderLib.Register(ShaderID::CloudShadowBake_CS,      RHI::ShaderStage::CS,
                         "CloudShadowBake.cs.hlsl",      "main");
    m_shaderLib.Register(ShaderID::CloudComposite_VS,       RHI::ShaderStage::VS,
                         "CloudComposite.vs.hlsl",       "main");
    m_shaderLib.Register(ShaderID::CloudComposite_PS,       RHI::ShaderStage::PS,
                         "CloudComposite.ps.hlsl",       "main");

    CreateComputePipelines(gfx);

    // Composite graphics PSO (alpha-over: src=ONE, dst=SRC_ALPHA).
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::CloudComposite_VS;
        desc.psID        = ShaderID::CloudComposite_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cullMode         = RHI::CullMode::NONE;
        desc.rs.depthClipEnable = false;
        desc.dss.depthEnable     = false;
        desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
        auto& bs = desc.bs.renderTarget[0];
        bs.blendEnable           = true;
        bs.srcBlend              = RHI::Blend::ONE;
        bs.destBlend             = RHI::Blend::SRC_ALPHA;
        bs.blendOp               = RHI::BlendOp::ADD;
        bs.srcBlendAlpha        = RHI::Blend::ZERO;
        bs.destBlendAlpha       = RHI::Blend::ONE;
        bs.blendOpAlpha         = RHI::BlendOp::ADD;
        bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
        if (!m_psoCache.GetOrCreate(desc))
            LOG_ERROR("CloudPass: composite PSO failed");
    }

    // The three lookup volumes (BakeNoiseOnce) and the shadow map (EnsureCloudShadowTexture) are
    // created lazily: CloudComponent is opt-in and not auto-spawned, so a scene without clouds
    // would carry ~9 MB of textures nothing reads. The shadow map is released when the feature
    // switches off; the volumes are kept for the session, since re-creating them re-runs the
    // 128^3 bake and would stutter on every toggle.

    // The composite's upsample sampler is the fixed SMP_LINEAR_CLAMP heap slot
    // CloudComposite.ps names, so none is created here.

    LOG_SUCCESS("CloudPass: initialised (base %u^3 + detail %u^3 + weather %u^2, "
                "raymarch quarter-res)",
                kBaseNoiseDim, kDetailNoiseDim, kWeatherDim);
}

// Onto the live members: a failed rebuild keeps the old pipeline.
void CloudPass::CreateComputePipelines(IGraphicsDevice& gfx)
{
    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("CloudPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("CloudPass: %s PSO failed", tag);
    };
    makeCs(ShaderID::CloudNoiseBake_CS,       m_noiseBakePSO,   "NoiseBake");
    makeCs(ShaderID::CloudDetailNoiseBake_CS, m_detailBakePSO,  "DetailBake");
    makeCs(ShaderID::CloudWeatherBake_CS,     m_weatherBakePSO, "WeatherBake");
    makeCs(ShaderID::CloudRaymarch_CS,        m_raymarchPSO,    "Raymarch");
    makeCs(ShaderID::CloudTemporal_CS,        m_temporalPSO,    "Temporal");
    makeCs(ShaderID::CloudShadowBake_CS,      m_shadowBakePSO,  "ShadowBake");
}

void CloudPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_shaderLib.ClearCaches()) return;   // none of its shaders changed
    m_psoCache.Clear();          // the composite: rebuilt at its next draw
    CreateComputePipelines(gfx);
    // BakeNoiseOnce keeps the volumes it allocated and bakes them again with
    // the new code; the temporal history holds the old shaders' output.
    m_noiseBaked = false;
    if (m_hist) m_hist->valid = false;
}

void CloudPass::RebuildCloudTexture(uint32_t fullW, uint32_t fullH)
{
    if (!m_gfx || !m_hist) return;   // Execute checks the view is bound
    History& h = *m_hist;
    if (m_f->cloudTex.IsValid())     m_gfx->DestroyTexture(m_f->cloudTex);
    if (m_f->cloudDistTex.IsValid()) m_gfx->DestroyTexture(m_f->cloudDistTex);
    for (auto& t : h.accum)
        if (t.IsValid()) { m_gfx->DestroyTexture(t); t = {}; }

    // Quarter res, rounded up.
    m_f->cloudW = (fullW + 3) / 4;
    m_f->cloudH = (fullH + 3) / 4;
    if (m_f->cloudW == 0 || m_f->cloudH == 0) return;

    RHI::TextureDesc td{};
    td.width      = m_f->cloudW;
    td.height     = m_f->cloudH;
    td.depth      = 1;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "Cloud.Raymarch";
    if (!m_gfx->CreateTexture(td, m_f->cloudTex))
        LOG_ERROR("CloudPass: cloud quarter-res texture create failed");

    // Temporal accumulation ping-pong: same format and size as the raymarch output.
    td.format = RHI::Format::R16G16B16A16_FLOAT;
    for (int i = 0; i < 2; ++i)
    {
        td.debugName = "Cloud.Accumulation";
        if (!m_gfx->CreateTexture(td, h.accum[i]))
            LOG_ERROR("CloudPass: cloud accumulation texture %d create failed", i);
        h.accumState[i] = RHI::ResourceState::UNORDERED_ACCESS;
    }
    h.accumRead  = 0;
    h.accumWrite = 1;
    // A new resolution invalidates every history texel.
    h.valid      = false;

    td.format = RHI::Format::R32_FLOAT;
    td.debugName = "Cloud.MarchDistance";
    if (!m_gfx->CreateTexture(td, m_f->cloudDistTex))
        LOG_ERROR("CloudPass: cloud march-distance texture create failed");

    m_f->cloudState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_f->cloudDistState = RHI::ResourceState::UNORDERED_ACCESS;
    m_f->lastFullW      = fullW;
    m_f->lastFullH      = fullH;
    LOG_INFO("CloudPass: quarter-res rebuilt %ux%u", m_f->cloudW, m_f->cloudH);
}

void CloudPass::SetCamera(const XMFLOAT4X4& invViewProj,
                          const XMFLOAT3&   cameraPos,
                          float nearZ, float farZ)
{
    SetCameraOf(m_mainFrame, m_mainHist, invViewProj, cameraPos, nearZ, farZ);
}

void CloudPass::SetViewCamera(ViewState& view, const XMFLOAT4X4& invViewProj, const XMFLOAT3& cameraPos,
                              float nearZ, float farZ)
{
    if (view.Slot() == 0) return;
    SetCameraOf(view.Data<ViewFrame>(this).frame, &view.History<History>(this), invViewProj, cameraPos,
                nearZ, farZ);
}

void CloudPass::SetCameraOf(Frame& f, History* hist, const XMFLOAT4X4& invViewProj, const XMFLOAT3& cameraPos,
                            float nearZ, float farZ)
{
    f.invViewProj = invViewProj;
    f.cameraPos   = cameraPos;
    f.nearZ       = nearZ;
    f.farZ        = farZ;

    // transpose(VP) for the temporal reprojection = inverse(invViewProj) (see
    // CloudConstants::prevViewProj). Deriving it here instead of passing a second matrix keeps
    // both on the same frame, jitter included.
    const XMMATRIX invVP = XMLoadFloat4x4(&f.invViewProj);
    XMVECTOR det;
    const XMMATRIX vpT = XMMatrixInverse(&det, invVP);
    if (!XMVector4Equal(det, XMVectorZero()))
        XMStoreFloat4x4(&f.viewProjT, vpT);
    else if (hist)
        hist->valid = false;   // singular: no usable reprojection this frame
}

void CloudPass::SetSun(const XMFLOAT3& dir, const XMFLOAT3& color, const XMFLOAT3& skyColor)
{
    m_sunDir   = dir;
    m_sunColor = color;
    // Rec.601 luma.
    m_skyLightLum = std::max(0.f, 0.299f * skyColor.x + 0.587f * skyColor.y + 0.114f * skyColor.z);
}

void CloudPass::SetParams(const CloudComponent& c, float dt)
{
    m_paramsEnabled    = c.enabled;
    m_bottomAlt        = c.bottomAltitude;
    m_topAlt           = c.topAltitude;
    m_coverage         = c.coverage;
    m_density          = c.density;
    m_baseNoiseScale   = c.noiseScale;
    m_detailNoiseScale = c.detailNoiseScale;
    m_detailStrength   = c.detailStrength;
    m_weatherScale     = c.weatherScale;
    m_cloudTypeBias    = c.cloudTypeBias;
    m_anvilBias        = c.anvilBias;
    m_windDir          = c.windDirection;
    m_windSpeed        = c.windSpeed;
    m_phaseFwdG        = c.anisotropy;
    m_phaseBackG       = c.phaseBackG;
    m_phaseBlend       = c.phaseBlend;
    m_silverIntensity  = c.silverIntensity;
    m_silverSpread     = c.silverSpread;
    m_extinction       = c.extinction;
    m_ambient          = c.ambientStrength;
    m_ambientTint      = c.ambientTint;
    m_cloudColor       = c.cloudColor;
    m_maxSteps         = c.maxSteps;
    // Toggling temporal on must not blend against a history the accumulator
    // never wrote while it was off.
    if (c.temporalUpsample && !m_temporalUpsample && m_hist) m_hist->valid = false;
    m_temporalUpsample = c.temporalUpsample;
    m_cloudShadowEnabled  = c.cloudShadowEnabled;
    m_cloudShadowStrength = std::max(0.f, c.cloudShadowStrength);
    m_cloudShadowExtent   = std::max(100.f, c.cloudShadowExtent);
    // Give the memory back the moment the feature is switched off, rather than
    // holding a 512x512 map that nothing reads until the world unloads.
    if (!ShadowWanted()) ReleaseCloudShadowTexture();

    // Accumulate the wind distance (m) so a wind speed change does not jump the shader's offset.
    m_windOffset += m_windSpeed * std::max(0.f, dt);
    // Wrap to avoid float precision drift over long sessions.
    if (m_windOffset > 1.0e6f) m_windOffset -= 1.0e6f;
}

bool CloudPass::UploadCB()
{
    m_cbVA = 0;
    if (!m_gfx || !m_hist) return false;
    const History& h = *m_hist;

    CloudConstants c{};
    std::memcpy(c.invViewProj, &m_f->invViewProj, sizeof(c.invViewProj));
    c.cameraPos[0]     = m_f->cameraPos.x;
    c.cameraPos[1]     = m_f->cameraPos.y;
    c.cameraPos[2]     = m_f->cameraPos.z;
    c.nearZ            = m_f->nearZ;
    c.farZ             = m_f->farZ;
    c.bottomAltitude   = m_bottomAlt;
    c.topAltitude      = std::max(m_topAlt, m_bottomAlt + 1.0f);
    c.coverage         = m_coverage;
    c.density          = m_density;
    c.baseNoiseScale   = m_baseNoiseScale;
    c.detailNoiseScale = m_detailNoiseScale;
    c.detailStrength   = m_detailStrength;
    c.weatherScale     = m_weatherScale;
    c.cloudTypeBias    = m_cloudTypeBias;
    c.anvilBias        = m_anvilBias;
    c.extinction       = m_extinction;

    c.sunDir[0]        = m_sunDir.x;
    c.sunDir[1]        = m_sunDir.y;
    c.sunDir[2]        = m_sunDir.z;
    c.ambientStrength  = m_ambient;
    c.sunColor[0]      = m_sunColor.x;
    c.sunColor[1]      = m_sunColor.y;
    c.sunColor[2]      = m_sunColor.z;
    c.phaseFwdG        = m_phaseFwdG;
    c.ambientTint[0]   = m_ambientTint.x;
    c.ambientTint[1]   = m_ambientTint.y;
    c.ambientTint[2]   = m_ambientTint.z;
    c.phaseBackG       = m_phaseBackG;
    c.cloudColor[0]    = m_cloudColor.x;
    c.cloudColor[1]    = m_cloudColor.y;
    c.cloudColor[2]    = m_cloudColor.z;
    c.phaseBlend       = m_phaseBlend;

    // Normalize wind direction and scale by accumulated offset distance.
    XMFLOAT3 w = m_windDir;
    float wlen = std::sqrt(w.x*w.x + w.y*w.y + w.z*w.z);
    if (wlen > 1e-6f) { w.x /= wlen; w.y /= wlen; w.z /= wlen; }
    c.windOffset[0]    = w.x * m_windOffset;
    c.windOffset[1]    = w.y * m_windOffset;
    c.windOffset[2]    = w.z * m_windOffset;
    c.silverIntensity  = m_silverIntensity;

    c.silverSpread     = m_silverSpread;
    c.frameIndex       = static_cast<float>(h.frameIndex % 8u);
    c.maxSteps         = std::clamp(m_maxSteps, 24.f, 256.f);
    c.maxTraceDist     = kMaxTraceDist;

    c.halfResW = static_cast<float>(m_f->cloudW);
    c.halfResH = static_cast<float>(m_f->cloudH);
    c.fullResW = static_cast<float>(m_f->lastFullW);
    c.fullResH = static_cast<float>(m_f->lastFullH);

    std::memcpy(c.prevViewProj, &h.prevViewProjT, sizeof(c.prevViewProj));

    // A camera teleport reprojects to garbage: the anchor point's previous screen position has
    // no relation to where its cloud was.
    const float dx = m_f->cameraPos.x - h.prevCameraPos.x;
    const float dy = m_f->cameraPos.y - h.prevCameraPos.y;
    const float dz = m_f->cameraPos.z - h.prevCameraPos.z;
    const bool  teleported = (dx*dx + dy*dy + dz*dz) >
                             (kTeleportDist * kTeleportDist);

    // Gate on the accumulator being ABLE to run, not just on the author flag:
    // the raymarch only advances its jitter when something downstream will
    // average it. A failed PSO with temporalOn = 1 would boil every frame.
    const bool temporalRuns = m_temporalUpsample && m_temporalPSO.IsValid()
                            && h.accum[0].IsValid()
                            && h.accum[1].IsValid();
    c.temporalOn    = temporalRuns ? 1.0f : 0.0f;
    c.resetHistory  = (h.valid && !teleported) ? 0.0f : 1.0f;
    c.temporalBlend = kTemporalBlend;
    c.ambientLightLum = m_skyLightLum;

    // ---- Cloud-shadow basis ----
    // Both axes are perpendicular to the sun, so a receiver's two projected components are
    // constant along its sun ray: one 2D map serves every altitude with no depth term.
    {
        XMVECTOR sun = XMVector3Normalize(
            XMVectorSet(m_sunDir.x, m_sunDir.y, m_sunDir.z, 0.f));
        // Any reference that is not parallel to the sun; swap near the poles
        // so the cross product never degenerates at zenith/midnight.
        const XMVECTOR ref = (std::fabs(m_sunDir.y) > 0.99f)
                           ? XMVectorSet(0.f, 0.f, 1.f, 0.f)
                           : XMVectorSet(0.f, 1.f, 0.f, 0.f);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(ref, sun));
        XMVECTOR up    = XMVector3Normalize(XMVector3Cross(sun, right));

        XMFLOAT3 R, U;
        XMStoreFloat3(&R, right);
        XMStoreFloat3(&U, up);

        // Snap the centre to whole texels along both axes; otherwise the footprint slides with the
        // camera and the shadow crawls even over still ground.
        const float texelWorld = (2.0f * m_cloudShadowExtent)
                               / static_cast<float>(kCloudShadowDim);
        const float camR = m_f->cameraPos.x * R.x + m_f->cameraPos.y * R.y + m_f->cameraPos.z * R.z;
        const float camU = m_f->cameraPos.x * U.x + m_f->cameraPos.y * U.y + m_f->cameraPos.z * U.z;
        const float snapR = std::floor(camR / texelWorld + 0.5f) * texelWorld;
        const float snapU = std::floor(camU / texelWorld + 0.5f) * texelWorld;
        // The projection discards the centre's along-sun component, so the centre is built from the
        // two snapped in-plane components only.
        const XMFLOAT3 centre{ R.x * snapR + U.x * snapU,
                               R.y * snapR + U.y * snapU,
                               R.z * snapR + U.z * snapU };

        c.csRight[0] = R.x; c.csRight[1] = R.y; c.csRight[2] = R.z;
        c.csUp[0]    = U.x; c.csUp[1]    = U.y; c.csUp[2]    = U.z;
        c.cloudShadowCenter[0] = centre.x;
        c.cloudShadowCenter[1] = centre.y;
        c.cloudShadowCenter[2] = centre.z;
        c.cloudShadowExtent    = m_cloudShadowExtent;
        c.cloudShadowDim       = static_cast<float>(kCloudShadowDim);
        c.cloudShadowStrength  = m_cloudShadowStrength;

        // Fold the same projection into the two rows LightCB hands consumers:
        // uv = dot(P, row.xyz) + row.w. Pre-dividing here keeps every consumer
        // at two dot products and guarantees they cannot drift from the bake.
        const float inv = 1.0f / (2.0f * m_cloudShadowExtent);
        m_shadowRightRow[0] = R.x * inv;
        m_shadowRightRow[1] = R.y * inv;
        m_shadowRightRow[2] = R.z * inv;
        m_shadowRightRow[3] = 0.5f - snapR * inv;
        m_shadowUpRow[0]    = U.x * inv;
        m_shadowUpRow[1]    = U.y * inv;
        m_shadowUpRow[2]    = U.z * inv;
        m_shadowUpRow[3]    = 0.5f - snapU * inv;
    }

    // This frame's block: the raymarch, temporal and shadow-bake dispatches and the composite
    // draw all bind it.
    const RHI::Constants<CloudConstants> cb = AllocConstants<CloudConstants>(*m_gfx);
    if (!cb) return false;
    *cb.cpu = c;
    m_cbVA  = cb.gpu;
    return true;
}

void CloudPass::BakeNoiseOnce(RHI::CommandList cl)
{
    if (m_noiseBaked) return;
    if (!m_noiseBakePSO.IsValid() || !m_detailBakePSO.IsValid() ||
        !m_weatherBakePSO.IsValid())
        return;
    IGraphicsDevice& gfx = *m_gfx;

    // First enabled frame: allocate the lookup volumes (see Init).
    auto make3D = [&](uint32_t dim, RHI::Texture& out, const char* tag, const char* name) {
        if (out.IsValid()) return;
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
        td.width      = dim;
        td.height     = dim;
        td.depth      = dim;
        td.mipLevels = 1;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = name;
        if (!gfx.CreateTexture(td, out))
            LOG_ERROR("CloudPass: %s 3D texture create failed", tag);
    };
    make3D(kBaseNoiseDim,   m_baseNoiseTex,   "base noise",   "Cloud.BaseNoise");
    make3D(kDetailNoiseDim, m_detailNoiseTex, "detail noise", "Cloud.DetailNoise");
    if (!m_weatherTex.IsValid())
    {
        RHI::TextureDesc td{};
        td.width      = kWeatherDim;
        td.height     = kWeatherDim;
        td.depth      = 1;
        td.mipLevels = 1;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = "Cloud.Weather";
        if (!gfx.CreateTexture(td, m_weatherTex))
            LOG_ERROR("CloudPass: weather texture create failed");
    }
    if (!m_baseNoiseTex.IsValid() || !m_detailNoiseTex.IsValid() ||
        !m_weatherTex.IsValid())
        return;

    auto bake = [&](const RHI::PipelineState& pso, const RHI::Texture& tex,
                    RHI::ResourceState& state,
                    uint32_t gx, uint32_t gy, uint32_t gz)
    {
        TransitionTex(gfx, tex, state, RHI::ResourceState::UNORDERED_ACCESS, cl);
        gfx.BindComputePipelineState(pso, cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(tex), cl);
        gfx.DispatchCompute(gx, gy, gz, cl);
        TransitionTex(gfx, tex, state, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    };

    bake(m_noiseBakePSO,   m_baseNoiseTex,   m_baseNoiseState,
         kBaseNoiseDim / 8, kBaseNoiseDim / 8, kBaseNoiseDim / 8);     // 8x8x8 groups
    bake(m_detailBakePSO,  m_detailNoiseTex, m_detailNoiseState,
         kDetailNoiseDim / 4, kDetailNoiseDim / 4, kDetailNoiseDim / 4); // 4x4x4 groups
    bake(m_weatherBakePSO, m_weatherTex,     m_weatherState,
         kWeatherDim / 8, kWeatherDim / 8, 1);                          // 8x8 groups

    m_noiseBaked = true;
    LOG_INFO("CloudPass: baked base %u^3 + detail %u^3 + weather %u^2",
             kBaseNoiseDim, kDetailNoiseDim, kWeatherDim);
}

void CloudPass::DispatchRaymarch(RHI::CommandList cl)
{
    if (!m_raymarchPSO.IsValid() || !m_f->cloudTex.IsValid()
        || !m_f->cloudDistTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    TransitionTex(gfx, m_f->cloudTex, m_f->cloudState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_f->cloudDistTex, m_f->cloudDistState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_baseNoiseTex, m_baseNoiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_detailNoiseTex, m_detailNoiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_weatherTex, m_weatherState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);

    gfx.BindComputePipelineState(m_raymarchPSO, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, m_cbVA, cl);
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_baseNoiseTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1,
        gfx.GetTextureSRVGpuHandle(m_detailNoiseTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT2,
        gfx.GetTextureSRVGpuHandle(m_weatherTex), cl);
    // t3 space2 = scene depth (sampled to clamp ray exit past opaque geometry)
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (depthTex)
        gfx.SetComputeDescriptorTable(kSRVT3,
            gfx.GetTextureSRVGpuHandle(*depthTex), cl);
    // u0 space2 = quarter-res output; u1 space2 = per-texel march distance
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_f->cloudTex), cl);
    gfx.SetComputeDescriptorTable(kUAV1,
        gfx.GetTextureUAVGpuHandle(m_f->cloudDistTex), cl);

    gfx.DispatchCompute((m_f->cloudW + 7) / 8, (m_f->cloudH + 7) / 8, 1, cl);

    TransitionTex(gfx, m_f->cloudTex, m_f->cloudState,
                  RHI::ResourceState::SHADER_RESOURCE, cl);
    TransitionTex(gfx, m_f->cloudDistTex, m_f->cloudDistState,
                  RHI::ResourceState::SHADER_RESOURCE, cl);

    // Only now is the texture safe to serve to other passes (lens flare).
    m_f->renderedThisFrame = true;
}

// Reproject + accumulate the quarter-res raymarch. Returns true when the history's
// accum[accumWrite] holds this frame's result, the texture consumers should read.
bool CloudPass::DispatchTemporal(RHI::CommandList cl)
{
    if (!m_temporalUpsample) return false;
    if (!m_temporalPSO.IsValid()) return false;
    if (!m_hist) return false;
    History& h = *m_hist;
    if (!h.accum[0].IsValid() || !h.accum[1].IsValid()) return false;
    IGraphicsDevice& gfx = *m_gfx;

    const uint32_t r = h.accumRead;
    const uint32_t w = h.accumWrite;

    TransitionTex(gfx, h.accum[r], h.accumState[r],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, h.accum[w], h.accumState[w],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_temporalPSO, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, m_cbVA, cl);
    // t0 = this frame's raymarch (already SHADER_RESOURCE), t1 = history.
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_f->cloudTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1,
        gfx.GetTextureSRVGpuHandle(h.accum[r]), cl);
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(h.accum[w]), cl);

    gfx.DispatchCompute((m_f->cloudW + 7) / 8, (m_f->cloudH + 7) / 8, 1, cl);

    TransitionTex(gfx, h.accum[w], h.accumState[w],
                  RHI::ResourceState::SHADER_RESOURCE, cl);
    return true;
}

// The shadow is wanted only when the clouds and the feature are on and strength > 0
// (strength 0 bakes all 1.0, the same as no map); otherwise the map is released.
// Not gated on Shown(): wireframe can flip many times a second and would free / re-allocate
// the map on every toggle; Execute's early-out already skips the bake, the part that costs.
bool CloudPass::ShadowWanted() const
{
    return m_enabled && m_paramsEnabled
        && m_cloudShadowEnabled && m_cloudShadowStrength > 0.f;
}

void CloudPass::EnsureCloudShadowTexture()
{
    if (m_cloudShadowTex.IsValid() || !m_gfx) return;

    RHI::TextureDesc td{};
    td.width      = kCloudShadowDim;
    td.height     = kCloudShadowDim;
    td.depth      = 1;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "Cloud.Shadow";
    if (!m_gfx->CreateTexture(td, m_cloudShadowTex))
    {
        LOG_ERROR("CloudPass: cloud shadow map create failed");
        return;
    }
    m_cloudShadowState = RHI::ResourceState::UNORDERED_ACCESS;
    LOG_INFO("CloudPass: cloud shadow map allocated %ux%u", kCloudShadowDim, kCloudShadowDim);
}

void CloudPass::ReleaseCloudShadowTexture()
{
    if (!m_cloudShadowTex.IsValid() || !m_gfx) return;

    // Clear the "we have a map" flag BEFORE the destroy: this can run from SetParams before
    // Renderer reads GetCloudShadowSrvHandle() in the same frame, and a set flag would hand out
    // a descriptor for a texture on its way out.
    m_shadowBakedThisFrame = false;
    // DestroyTexture queues for deferred release (the GPU may still be reading
    // it for up to kFrameCount frames), so no WaitIdle is needed here.
    m_gfx->DestroyTexture(m_cloudShadowTex);
    m_cloudShadowTex   = {};
    m_cloudShadowState = RHI::ResourceState::UNORDERED_ACCESS;
    LOG_INFO("CloudPass: cloud shadow map released (cast-shadow switched off)");
}

// Bake the clouds' shadow along the sun direction. Shares the CB, the noise / weather SRVs
// and SampleDensity (CloudDensity.hlsli) with the raymarch, so the shadow always belongs to
// the cloud on screen.
void CloudPass::DispatchShadowBake(RHI::CommandList cl)
{
    m_shadowBakedThisFrame = false;
    if (!ShadowWanted()) return;                 // switched off: skip the cost
    if (!m_shadowBakePSO.IsValid()) return;
    EnsureCloudShadowTexture();
    if (!m_cloudShadowTex.IsValid()) return;
    IGraphicsDevice& gfx = *m_gfx;

    // An imported map is the graph's to transition (this pass's Phase A, the next reader's);
    // without the import (-rgimportoff) the pass does it.
    const bool graphTracked = m_cloudShadowH.IsValid();
    if (!graphTracked)
        TransitionTex(gfx, m_cloudShadowTex, m_cloudShadowState,
                      RHI::ResourceState::UNORDERED_ACCESS, cl);
    // The noise volumes are already SHADER_RESOURCE_COMPUTE from the raymarch
    // that ran just before; TransitionTex no-ops when the state matches.
    TransitionTex(gfx, m_baseNoiseTex, m_baseNoiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_detailNoiseTex, m_detailNoiseState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_weatherTex, m_weatherState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);

    gfx.BindComputePipelineState(m_shadowBakePSO, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, m_cbVA, cl);
    gfx.SetComputeDescriptorTable(kSRVT0,
        gfx.GetTextureSRVGpuHandle(m_baseNoiseTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT1,
        gfx.GetTextureSRVGpuHandle(m_detailNoiseTex), cl);
    gfx.SetComputeDescriptorTable(kSRVT2,
        gfx.GetTextureSRVGpuHandle(m_weatherTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0,
        gfx.GetTextureUAVGpuHandle(m_cloudShadowTex), cl);

    gfx.DispatchCompute(kCloudShadowDim / 8, kCloudShadowDim / 8, 1, cl);

    // PIXEL|NON_PIXEL: pixel shaders (Lighting, Water, forward surfaces) and VolumetricFog's
    // compute raymarch read it.
    if (!graphTracked)
        TransitionTex(gfx, m_cloudShadowTex, m_cloudShadowState,
                      RHI::ResourceState::SHADER_RESOURCE, cl);
    m_shadowBakedThisFrame = true;
}

uint64_t CloudPass::GetCloudShadowSrvHandle() const
{
    if (!m_shadowBakedThisFrame || !m_gfx || !m_cloudShadowTex.IsValid()) return 0;
    // Only without the import must the pass's own book say SHADER_RESOURCE: the graph brings an
    // imported map to SHADER_RESOURCE for each reader.
    if (!m_cloudShadowH.IsValid() && m_cloudShadowState != RHI::ResourceState::SHADER_RESOURCE) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_cloudShadowTex);
}

void CloudPass::GetCloudShadowRows(float outRight[4], float outUp[4]) const
{
    for (int i = 0; i < 4; ++i)
    {
        outRight[i] = m_shadowRightRow[i];
        outUp[i]    = m_shadowUpRow[i];
    }
}

// What the composite and the lens flare must read. accumWrite is not swapped until the next
// frame's Execute, so this stays correct for the whole frame, including LensFlarePass, which
// runs long after Execute returns.
const RHI::Texture& CloudPass::ResolvedCloudTex() const
{
    return (m_f->accumulatedThisFrame && m_hist) ? m_hist->accum[m_hist->accumWrite] : m_f->cloudTex;
}

void CloudPass::Composite(RHI::CommandList cl)
{
    PSODesc desc;
    desc.vsID        = ShaderID::CloudComposite_VS;
    desc.psID        = ShaderID::CloudComposite_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    auto& bs = desc.bs.renderTarget[0];
    bs.blendEnable           = true;
    bs.srcBlend              = RHI::Blend::ONE;
    bs.destBlend             = RHI::Blend::SRC_ALPHA;
    bs.blendOp               = RHI::BlendOp::ADD;
    bs.srcBlendAlpha        = RHI::Blend::ZERO;
    bs.destBlendAlpha       = RHI::Blend::ONE;
    bs.blendOpAlpha         = RHI::BlendOp::ADD;
    bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return;

    // Bind HDR + depth here (Setup's colour target is None). The DSV only matches the PSO's
    // format: its depth test is off.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    cl.GetDevice().SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));

    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // CB (b3 space0: this frame's CloudConstants block, UploadCB) + full-res depth at t5, both
    // needed by the depth-aware upsample / shell guard. Same pattern as
    // VolumetricFogPass::DrawApply.
    cl.BindCBAddress(RHI::Slots::GfxRel::CB_b3, m_cbVA);
    cl.BindSRVByHandle(RHI::Slots::GfxRel::SRV_t5, m_depth);

    // Cloud quarter-res RGBA16F at t6 space0 (the slot VolumetricFog re-purposes for its
    // half-res raymarch); march distance at t7.
    cl.BindDescriptorTableHandle(kEnvMapRootSlot,
        m_gfx->GetTextureSRVGpuHandle(ResolvedCloudTex()));
    cl.BindDescriptorTableHandle(kCloudDistRootSlot,
        m_gfx->GetTextureSRVGpuHandle(m_f->cloudDistTex));

    cl.DrawInstanced(3, 1, 0, 0);
}

uint64_t CloudPass::GetCloudSrvHandle() const
{
    if (!m_f->renderedThisFrame || !m_gfx || !m_f->cloudTex.IsValid()) return 0;
    if (m_f->cloudState != RHI::ResourceState::SHADER_RESOURCE) return 0;
    if (m_f->accumulatedThisFrame && m_hist &&
        m_hist->accumState[m_hist->accumWrite] != RHI::ResourceState::SHADER_RESOURCE)
        return 0;
    return m_gfx->GetTextureSRVGpuHandle(ResolvedCloudTex());
}

// Runs from the graph's serial Phase A every frame, including the frames IsActive() skips,
// which is what lets this pass have an activity gate (RG::RenderPass::PrepareFrame). The
// flags are read outside the graph: GetCloudSrvHandle by the lens flare after it,
// GetCloudShadowSrvHandle by Renderer before the next frame's graph.
void CloudPass::PrepareFrame()
{
    // A disabled / wireframe / no-component frame must not serve last frame's
    // transmittance to the lens flare.
    m_f->renderedThisFrame    = false;
    m_f->accumulatedThisFrame = false;
    // A frame without clouds must report "no shadow map" so Renderer writes cloudShadowIdx = 0
    // and the world goes back to full sun instead of keeping the last baked overcast. The main
    // view's: a secondary view bakes no shadow.
    if (m_f == &m_mainFrame) m_shadowBakedThisFrame = false;

    if (!m_enabled || !m_paramsEnabled || !Shown())
    {
        // Skipped frames leave prevViewProj / prevCameraPos frozen, so the
        // history is stale by an unknown number of frames of camera motion by
        // the time clouds come back. Drop it rather than reproject against it.
        if (m_hist) m_hist->valid = false;
    }
}

RHI::CommandList CloudPass::Execute(RHI::CommandList cl)
{
    // Also the entry point for a non-graph caller; PrepareFrame already ran for every graph
    // frame and is idempotent.
    PrepareFrame();

    if (!m_enabled || !m_paramsEnabled || !Shown())
        return cl;
    if (!m_gfx) return cl;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "CloudPass: no view bound (PassSet::BindView) - no clouds"))
        return cl;
    History& h = *m_hist;
    IGraphicsDevice& gfx = *m_gfx;

    // First Execute: bake the noise volumes + weather map once.
    BakeNoiseOnce(cl);

    // Rebuild quarter-res buffer on resize.
    const uint32_t fullW = gfx.GetRenderWidth();
    const uint32_t fullH = gfx.GetRenderHeight();
    if (fullW == 0 || fullH == 0) return cl;
    if (fullW != m_f->lastFullW || fullH != m_f->lastFullH || !m_f->cloudTex.IsValid())
        RebuildCloudTexture(fullW, fullH);

    ++h.frameIndex;

    // Ping-pong at the TOP of the frame, not the bottom: accumWrite has to
    // keep identifying this frame's result until the frame is over, because
    // LensFlarePass calls GetCloudSrvHandle() long after Execute returns.
    if (m_temporalUpsample) std::swap(h.accumRead, h.accumWrite);

    // Upload CB + raymarch. UploadCB reads the history's valid / prevViewProjT /
    // prevCameraPos, so all three must still hold LAST frame's values here.
    // No CB (the frame's constants ran out, reported) - nothing below may run.
    if (!UploadCB()) return cl;
    DispatchRaymarch(cl);

    // Reproject + accumulate, then composite whichever texture came out.
    m_f->accumulatedThisFrame = DispatchTemporal(cl);
    Composite(cl);

    // Cloud shadow map. It lags one frame for the readers before this pass (Lighting, Water,
    // WaterSurface), deliberately: baking ahead of LightingPass would need a second pass that
    // duplicates the noise bake and the CB upload. Renderer reads GetCloudShadowSrvHandle() and
    // GetCloudShadowRows() together before the graph, so the basis it publishes describes the map
    // those readers sample and a rotating sun lags a frame instead of sliding the shadows. Do not
    // refresh one of the two without the other. VolumetricFog and Transparent sample the map after
    // this bake: this frame's texels through LightCB's (last frame's) basis.
    if (m_f == &m_mainFrame) DispatchShadowBake(cl);

    // Snapshot for next frame's reprojection. Only a completed accumulation
    // leaves a history worth blending against.
    h.prevViewProjT = m_f->viewProjT;
    h.prevCameraPos = m_f->cameraPos;
    h.valid         = m_f->accumulatedThisFrame;

    return cl;
}
