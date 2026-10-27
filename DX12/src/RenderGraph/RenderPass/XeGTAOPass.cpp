#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/TransientAliasArena.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "System/Check.h"
#include "System/Log.h"
#include <cstring>
#include <cmath>

// Compute root signature slots (shared by all compute passes, space2), from RHI::Slots::Cmp
// (Graphics/RootSignatureLayout.h); the local names only carry the per-purpose comments.
namespace S = RHI::Slots::Cmp;
static constexpr auto kCBSlot     = S::CB;      // b0 space2
static constexpr auto kSRV0       = S::SRV_t0;  // t0 space2
static constexpr auto kSRV1       = S::SRV_t1;  // t1 space2
static constexpr auto kSRV2       = S::SRV_t2;  // t2 space2: Hilbert LUT (main), AO history (temporal)
static constexpr auto kUAV0       = S::UAV_u0;  // u0 space2
static constexpr auto kUAV1       = S::UAV_u1;  // u1 space2
static constexpr auto kUAV2       = S::UAV_u2;  // u2 space2 (XeGTAO prefilter mip 2)
static constexpr auto kUAV3       = S::UAV_u3;  // u3 space2 (XeGTAO prefilter mip 3)
static constexpr auto kUAV4       = S::UAV_u4;  // u4 space2 (XeGTAO prefilter mip 4)

// Intel XeGTAO Hilbert curve, a verbatim port at XE_HILBERT_LEVEL=6 (64x64 pixels, 4096 cells).
// SpatioTemporalNoise drives an R2 low-discrepancy sequence from this spatially decorrelated base
// index, a better noise pattern than plain R2.
static constexpr uint32_t XE_HILBERT_LEVEL = 6u;
static constexpr uint32_t XE_HILBERT_WIDTH = 1u << XE_HILBERT_LEVEL;  // 64
static constexpr uint32_t XE_HILBERT_AREA  = XE_HILBERT_WIDTH * XE_HILBERT_WIDTH; // 4096

// One shared upload buffer with a 256-byte-aligned slice per dispatch, so filling one
// dispatch's CB never overwrites another's before the GPU reads it.
static constexpr uint32_t kCBOffsetMain      = 0;
static constexpr uint32_t kCBOffsetTemporal  = 256;
static constexpr uint32_t kCBOffsetDenoise   = 512;
static constexpr uint32_t kCBOffsetPrefilter = 768;  // one slot: a single dispatch writes every mip

struct alignas(16) PrefilterCB
{
    uint32_t srcSize[2];
    uint32_t dstSize[2];
    float    depthUnpackConsts[2];
    float    effectRadius;
    float    effectFalloffRange;
    float    radiusMultiplier;
    uint32_t srcMip;
    float    _pad0[2];
};

static uint32_t HilbertIndex(uint32_t posX, uint32_t posY)
{
    uint32_t index = 0u;
    for (uint32_t curLevel = XE_HILBERT_WIDTH / 2u; curLevel > 0u; curLevel /= 2u)
    {
        const uint32_t regionX = (posX & curLevel) > 0u ? 1u : 0u;
        const uint32_t regionY = (posY & curLevel) > 0u ? 1u : 0u;
        index += curLevel * curLevel * ((3u * regionX) ^ regionY);
        if (regionY == 0u)
        {
            if (regionX == 1u)
            {
                posX = (XE_HILBERT_WIDTH - 1u) - posX;
                posY = (XE_HILBERT_WIDTH - 1u) - posY;
            }
            const uint32_t t = posX; posX = posY; posY = t;
        }
    }
    return index;
}

XeGTAOPass::XeGTAOPass(RG::RGTextureHandle normal,
                       RG::RGTextureHandle depth,
                       RG::RGTextureHandle velocity)
    : m_normalH(normal), m_depthH(depth), m_velocityH(velocity)
{}

// The AO plane's two slots as graph-tracked imports; BindView made the history before the first Compile.
// Their sources are copies of the bound view's slots, so each view's Execute takes its own.
void XeGTAOPass::DeclareImports(RG::ImportDeclarer& d)
{
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "XeGTAOPass: no view bound before the graph's Compile - its AO is not imported"))
        return;
    SyncImports();
    const RG::RGTextureDesc desc{ RHI::Format::R8_UNORM, 0, 0, false, true };
    m_aoH.cur  = d.Import(RG::PassTex::kXeGTAO, desc,
                          { &m_aoCurImport, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
    m_aoH.prev = d.Import(RG::PassTex::kXeGTAOPrev, desc,
                          { &m_aoPrevImport, RG::ImportEntry::History, RG::ImportTracking::Graph });
}

void XeGTAOPass::SyncImports()
{
    const RHI::Texture* cur  = m_hist ? m_hist->tex.CurSource(0)  : nullptr;
    const RHI::Texture* prev = m_hist ? m_hist->tex.PrevSource(0) : nullptr;
    m_aoCurImport  = cur  ? *cur  : RHI::Texture{};
    m_aoPrevImport = prev ? *prev : RHI::Texture{};
}

void XeGTAOPass::Setup(RG::RenderGraphBuilder& b)
{
    // Same declarations as RTAOPass: the graph transitions the GBuffer inputs, the pass barriers
    // its own AO / linear-depth textures.
    b.ReadSRV(m_normalH);
    b.ReadSRV(m_depthH);
    b.ReadSRV(m_velocityH);
    // The AO history, transitioned by the graph: Cur to UNORDERED_ACCESS here and to
    // SHADER_RESOURCE in its first reader's Phase A (LightingPass); Prev, last frame's AO, is read
    // scoped at SHADER_RESOURCE_COMPUTE by the temporal dispatch.
    b.WriteUAV(m_aoH.cur);
    b.ReadScoped(m_aoH.prev, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
}

void XeGTAOPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::XeGTAO_CS,              RHI::ShaderStage::CS,
                         "XeGTAO.cs.hlsl",                 "CSMain");
    m_shaderLib.Register(ShaderID::XeGTAODenoise_CS,       RHI::ShaderStage::CS,
                         "XeGTAODenoise.cs.hlsl",          "CSMain");
    m_shaderLib.Register(ShaderID::XeGTAODepthLinearize_CS, RHI::ShaderStage::CS,
                         "XeGTAODepthPrefilter.cs.hlsl",  "CSPrefilter");
    m_shaderLib.Register(ShaderID::XeGTAOTemporal_CS,      RHI::ShaderStage::CS,
                         "XeGTAOTemporal.cs.hlsl",         "CSMain");

    CreatePipelines(gfx);

    // Shared CB, one slice per dispatch type (kCBOffset*).
    m_cb.Create(gfx, "XeGTAO.CB");

    // ---- Hilbert LUT (64x64 R16_UINT, immutable) ----
    {
        std::vector<uint16_t> lut(XE_HILBERT_AREA);
        for (uint32_t y = 0; y < XE_HILBERT_WIDTH; ++y)
        for (uint32_t x = 0; x < XE_HILBERT_WIDTH; ++x)
        {
            const uint32_t idx = HilbertIndex(x, y);
            // idx range [0, 4096), always fits in uint16.
            lut[y * XE_HILBERT_WIDTH + x] = static_cast<uint16_t>(idx);
        }

        RHI::TextureDesc td{};
        td.width      = XE_HILBERT_WIDTH;
        td.height     = XE_HILBERT_WIDTH;
        td.format     = RHI::Format::R16_UINT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

        RHI::SubresourceData sub{};
        sub.dataPtr   = lut.data();
        sub.rowPitch  = XE_HILBERT_WIDTH * sizeof(uint16_t); // 128 bytes
        sub.slicePitch = sub.rowPitch * XE_HILBERT_WIDTH;

        if (gfx.CreateTexture(td, m_hilbertLUT, &sub))
            m_hilbertLUTSrv = gfx.GetTextureSRVGpuHandle(m_hilbertLUT);
        else
            LOG_ERROR("XeGTAOPass: failed to create Hilbert LUT");
    }

    // ---- 1x1 zero-velocity fallback (R16G16_FLOAT) ----
    // Bound to the temporal pass's velocity slot when no GBuffer velocity SRV exists (see
    // m_zeroVelocityTex).
    {
        const uint16_t zeroVel[2] = { 0, 0 }; // (0.0h, 0.0h)
        RHI::TextureDesc td{};
        td.width      = 1;
        td.height     = 1;
        td.format     = RHI::Format::R16G16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

        RHI::SubresourceData sub{};
        sub.dataPtr    = zeroVel;
        sub.rowPitch   = sizeof(zeroVel);  // 4 bytes
        sub.slicePitch = sizeof(zeroVel);

        if (gfx.CreateTexture(td, m_zeroVelocityTex, &sub))
            m_zeroVelocitySrv = gfx.GetTextureSRVGpuHandle(m_zeroVelocityTex);
        else
            LOG_ERROR("XeGTAOPass: failed to create zero-velocity fallback");
    }

    LOG_SUCCESS("XeGTAOPass: initialized");
}

// Onto the live members: a failed rebuild keeps the old pipeline.
void XeGTAOPass::CreatePipelines(IGraphicsDevice& gfx)
{
    auto makeCS = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("XeGTAOPass: %s shader not found", tag); return; }
        RHI::PipelineStateDesc pd{};
        pd.cs = cs;
        if (!gfx.CreatePipelineState(pd, out))
            LOG_ERROR("XeGTAOPass: %s PSO creation failed", tag);
    };
    makeCS(ShaderID::XeGTAO_CS,               m_mainPSO,      "XeGTAO_CS");
    makeCS(ShaderID::XeGTAOTemporal_CS,       m_temporalPSO,  "XeGTAOTemporal_CS");
    makeCS(ShaderID::XeGTAODenoise_CS,        m_denoisePSO,   "XeGTAODenoise_CS");
    makeCS(ShaderID::XeGTAODepthLinearize_CS, m_prefilterPSO, "XeGTAODepthPrefilter_CS");
}

void XeGTAOPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void XeGTAOPass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_w->vpW || h != m_w->vpH)
    {
        m_w->vpW = w;
        m_w->vpH = h;
        m_w->texDirty = true;
    }
}

TransientAliasArena* XeGTAOPass::WorkArena() const
{
    return m_aliasArena && m_w != &m_mainWork ? m_aliasArena->ViewTwin() : m_aliasArena;
}

void XeGTAOPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (m_w->texDirty)
    {
        RebuildTextures(/*allowPlaced=*/true);   // main thread: placed allowed
        return;
    }
    // Another arena resident grew the shared heap since our last rebuild: re-place only the
    // transients. The committed history keeps its accumulation; a full RebuildTextures here would
    // pop the AO for a frame.
    const TransientAliasArena* arena = WorkArena();
    if (arena && m_w->placedMask && m_w->placedEpoch != arena->Epoch())
        RebuildTransients(/*allowPlaced=*/true);
}

void XeGTAOPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
    m_view = &view;
    m_w    = view.Slot() == 0 ? &m_mainWork : &view.Data<ViewWork>(this).work;
    SyncImports();
}

void XeGTAOPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &work.linearDepth, &work.aoRaw, &work.aoFinal, &work.edges })
        if (t->IsValid()) gfx.DestroyTexture(*t);
    work = Work{};
}

void XeGTAOPass::History::Release(IGraphicsDevice& gfx)
{
    tex.Release(gfx);   // deferred internally; the AO plane's two, then the depth's
    valid = false;
}

void XeGTAOPass::ReleaseResources()
{
    if (!m_gfx || !m_w->aoFinal.IsValid()) return;   // already released
    for (RHI::Texture* t : { &m_w->linearDepth, &m_w->aoRaw, &m_w->aoFinal, &m_w->edges })
    {
        if (t->IsValid()) m_gfx->DestroyTexture(*t);   // deferred internally
        *t = {};
    }
    // Then the history ping-pongs.
    if (m_hist) m_hist->Release(*m_gfx);
    SyncImports();
    // The Hilbert LUT and the 1x1 zero-velocity texture stay: tiny, size-independent, and the LUT
    // upload is the costly part of a rebuild.
    m_w->texDirty     = true;    // next PrepareResources recreates
    m_w->placedMask = 0;     // no live placed transients -> no alias barriers
    LOG_INFO("XeGTAOPass: working set released (SSAO switched off)");
}

void XeGTAOPass::SetProjectionMatrix(const DirectX::XMFLOAT4X4& projMatrix,
                                     const DirectX::XMFLOAT4X4& viewMatrix,
                                     uint32_t frameCounter)
{
    m_w->projMatrix   = projMatrix;
    m_w->viewMatrix   = viewMatrix;
    m_w->frameCounter = frameCounter;
}

// Placed into the bound view's alias arena when allowed (the frame's prepare only); each texture falls
// back to committed on failure. Placed textures get per-mip UAVs like committed ones. The committed history ping-pongs are not touched.
void XeGTAOPass::RebuildTransients(bool allowPlaced)
{
    if (!m_gfx || m_w->vpW == 0 || m_w->vpH == 0) return;

    RHI::TextureDesc tdR8{};
    tdR8.width      = m_w->vpW;
    tdR8.height     = m_w->vpH;
    tdR8.format     = RHI::Format::R8_UNORM;
    tdR8.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    tdR8.usage      = RHI::Usage::DEFAULT;
    tdR8.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    RHI::TextureDesc tdDepth = tdR8;
    tdDepth.format     = RHI::Format::R32_FLOAT;
    tdDepth.mipLevels = kDepthMipLevels;

    struct T { const char* tag; const RHI::TextureDesc* td; RHI::Texture* tex; };
    const T transients[] = {
        { "XeGTAO.linearDepth", &tdDepth, &m_w->linearDepth },
        { "XeGTAO.aoRaw",       &tdR8,    &m_w->aoRaw },
        { "XeGTAO.edges",       &tdR8,    &m_w->edges },
        { "XeGTAO.aoFinal",     &tdR8,    &m_w->aoFinal },
    };
    constexpr int kCount = int(sizeof(transients) / sizeof(transients[0]));

    TransientAliasArena* const arena = WorkArena();
    bool placed = false;   // in the arena's heap, through IGraphicsDevice
    uint64_t offsets[kCount]{};
    if (allowPlaced && arena)
    {
        uint64_t cursor = 0;
        bool ok = true;
        for (int i = 0; i < kCount; ++i)
        {
            uint64_t sz = 0, al = 0;
            if (!m_gfx->GetTextureAllocationInfo(*transients[i].td, sz, al)) { ok = false; break; }
            offsets[i] = TransientAliasArena::AlignUp(cursor, al);
            cursor     = offsets[i] + sz;
        }
        if (ok)
            placed = m_gfx->EnsureAliasArena(*arena, cursor);
    }

    m_w->placedMask = 0;
    for (int i = 0; i < kCount; ++i)
    {
        if (transients[i].tex->IsValid()) m_gfx->DestroyTexture(*transients[i].tex);
        RHI::TextureDesc td = *transients[i].td;
        td.debugName = transients[i].tag;
        bool created = false;
        if (placed)
        {
            if (m_gfx->CreateTexturePlaced(td, *arena, offsets[i], *transients[i].tex))
            {
                created = true;
                m_w->placedMask |= uint8_t(1u << i);
            }
        }
        if (!created && !m_gfx->CreateTexture(td, *transients[i].tex))
            LOG_ERROR("XeGTAOPass: failed to create %s (%ux%u)",
                      transients[i].tag, m_w->vpW, m_w->vpH);
    }
    if (arena)
        m_w->placedEpoch = arena->Epoch();

    // Creation-layout state mirrors for the transient set (a recreated texture must not inherit a
    // stale mid-frame state).
    m_w->aoRawState       = RHI::ResourceState::UNORDERED_ACCESS;
    m_w->aoFinalState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_w->edgesState       = RHI::ResourceState::UNORDERED_ACCESS;
    m_w->linearDepthState = RHI::ResourceState::UNORDERED_ACCESS;
}

void XeGTAOPass::RebuildTextures(bool allowPlaced)
{
    if (!m_gfx || m_w->vpW == 0 || m_w->vpH == 0) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "XeGTAOPass: no view bound (PassSet::BindView) - no AO"))
        return;
    History& h = *m_hist;

    RebuildTransients(allowPlaced);

    // The temporal ping-pong (RG::HistoryTexture): both planes destroyed and made again at the
    // viewport's size, created UNORDERED_ACCESS; Cur is slot 0 again, nothing written.
    h.tex.Rebuild(*m_gfx, m_w->vpW, m_w->vpH);
    h.valid = false;
    SyncImports();

    m_w->texDirty = false;

    LOG_INFO("XeGTAOPass: created AO textures %ux%u + linear depth", m_w->vpW, m_w->vpH);
}

uint64_t XeGTAOPass::GetAOSrvHandle() const
{
    // The history's newest content: Cur once this frame's Execute ran, Prev until then.
    if (!m_gfx || !m_hist) return 0;
    const RHI::Texture& tex = m_hist->tex.LastWritten(0);
    if (!tex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(tex);
}

uint64_t XeGTAOPass::GetUpcomingAOSrv() const
{
    // Cur is fixed for the whole frame by the history flip in OnPrepare, so the consumer can bind
    // this frame's AO before the pass records.
    //
    // Mirror Execute's early-outs: if it bails, the texture is never written nor moved out of
    // UNORDERED_ACCESS this frame, and Lighting.ps would read garbage in an illegal state. 0 makes
    // the consumer fall back to "no AO".
    if (!m_gfx || !m_hist || m_w->texDirty) return 0;
    if (!m_mainPSO.IsValid() || !m_denoisePSO.IsValid() || !m_prefilterPSO.IsValid())
        return 0;
    const RHI::Texture& tex = m_hist->tex.Cur(0);
    if (!tex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(tex);
}

const RHI::Texture* XeGTAOPass::GetDebugOutput(RHI::ResourceState& state) const
{
    if (!m_w->active || !m_gfx || !m_hist || !m_hist->valid) return nullptr;
    const RHI::Texture& t = m_hist->tex.LastWritten(0);
    if (!t.IsValid()) return nullptr;
    state = m_gfx->QueryTextureState(t);
    return &t;
}

RHI::CommandList XeGTAOPass::Execute(RHI::CommandList cl)
{
    if (!m_mainPSO.IsValid() || !m_denoisePSO.IsValid() ||
        !m_prefilterPSO.IsValid()) return cl;
    if (!m_w->depthSrvHandle || m_w->vpW == 0 || m_w->vpH == 0) return cl;

    // Worker-thread dirty fallback: committed textures only (allowPlaced defaults to false);
    // placed creation belongs to the main-thread PrepareResources.
    if (m_w->texDirty) RebuildTextures();
    if (!m_hist) return cl;   // no view bound: RebuildTextures said so
    if (!m_w->aoRaw.IsValid() || !m_w->aoFinal.IsValid() ||
        !m_w->edges.IsValid() || !m_w->linearDepth.IsValid()) return cl;
    History& h = *m_hist;
    // The history flips at the frame's start (OnPrepare). A second Execute without it would write
    // the slot it wrote last and read the one before as history.
    ENGINE_CHECK_ONCE(!h.tex.WrittenThisFrame(),
        "XeGTAOPass: Execute ran again without the frame's OnPrepare - the AO history did not flip");

    auto& gfx = *m_gfx;

    // ---- Alias-acquire the placed transients (shared arena residents) ----
    // The heap bytes may last have belonged to another resident (RTR, VolFog, Bloom) or nobody
    // (null before = any resource). Recorded every frame the pass runs, before any transition; the
    // full-coverage prefilter / main / denoise writes re-initialize the content before any read.
    // State mirrors stay as they are: state is tracked per resource through aliasing barriers.
    if (m_w->placedMask)
    {
        const RHI::GPUResource* placed[] = {
            &m_w->linearDepth, &m_w->aoRaw, &m_w->edges, &m_w->aoFinal,
        };
        for (int i = 0; i < int(sizeof(placed) / sizeof(placed[0])); ++i)
            if (m_w->placedMask & (1u << i))
                gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr, placed[i]), cl);
    }

    // ---- Projection-derived constants (shared by main + prefilter) ----
    const float* pm = &m_w->projMatrix.m[0][0];
    float depthLinearizeMul = -pm[3 * 4 + 2];
    float depthLinearizeAdd =  pm[2 * 4 + 2];
    if (depthLinearizeMul * depthLinearizeAdd < 0.0f)
        depthLinearizeAdd = -depthLinearizeAdd;
    const float tanHalfFOVY = 1.0f / pm[1 * 4 + 1];
    const float tanHalfFOVX = 1.0f / pm[0 * 4 + 0];

    auto* cbMapped = m_cb.Current(gfx);
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // ---- Fill all CBs up front (different byte offsets in m_cb) ----
    if (cbMapped)
    {
        uint8_t* base = cbMapped->bytes;

        // Main GTAOConstants - offset 0.
        {
            GTAOConstants c{};
            c.ViewportSize[0]      = int32_t(m_w->vpW);
            c.ViewportSize[1]      = int32_t(m_w->vpH);
            c.ViewportPixelSize[0] = 1.0f / float(m_w->vpW);
            c.ViewportPixelSize[1] = 1.0f / float(m_w->vpH);
            c.DepthUnpackConsts[0] = depthLinearizeMul;
            c.DepthUnpackConsts[1] = depthLinearizeAdd;
            c.CameraTanHalfFOV[0]  = tanHalfFOVX;
            c.CameraTanHalfFOV[1]  = tanHalfFOVY;
            c.NDCToViewMul[0]      = tanHalfFOVX *  2.0f;
            c.NDCToViewMul[1]      = tanHalfFOVY * -2.0f;
            c.NDCToViewAdd[0]      = tanHalfFOVX * -1.0f;
            c.NDCToViewAdd[1]      = tanHalfFOVY *  1.0f;
            c.NDCToViewMul_x_PixelSize[0] = c.NDCToViewMul[0] * c.ViewportPixelSize[0];
            c.NDCToViewMul_x_PixelSize[1] = c.NDCToViewMul[1] * c.ViewportPixelSize[1];
            c.EffectRadius             = effectRadius;
            c.EffectFalloffRange       = effectFalloffRange;
            c.RadiusMultiplier         = radiusMultiplier;
            c.FinalValuePower          = finalValuePower;
            c.DenoiseBlurBeta          = denoiseBlurBeta;
            c.SampleDistributionPower  = sampleDistributionPower;
            c.ThinOccluderCompensation = thinOccluderCompensation;
            c.DepthMIPSamplingOffset   = depthMIPSamplingOffset;
            c.NoiseIndex               = int32_t(m_w->frameCounter % 64);
            EffectiveCounts(c.SliceCount, c.StepsPerSlice);   // features.aoQuality
            // Raw copy of the row-major view matrix; HLSL reads it column-major, which gives the
            // world -> view rotation under mul(M, v).
            std::memcpy(c.ViewMatrix, &m_w->viewMatrix.m[0][0], sizeof(c.ViewMatrix));
            std::memcpy(base + kCBOffsetMain, &c, sizeof(c));
        }

        // Temporal CB: AO accumulates across frames by velocity reprojection. historyAlpha = 0 on the
        // first valid frame, after a resize or on a cut, so the shader uses the current value instead of
        // an uninitialised history.
        {
            TemporalCB tc{};
            tc.viewportWidth  = m_w->vpW;
            tc.viewportHeight = m_w->vpH;
            // The frame's content cut (a hard cut, the camera's, a world clear - the view's FrameBreaks):
            // no history this frame.
            const bool cut = m_view && (m_view->FrameBreaks() & HistoryReset::Cuts);
            tc.historyAlpha   = (h.valid && !cut)
                                  ? temporalHistoryAlpha : 0.0f;
            tc.rejectionDiff  = temporalRejectionDiff;
            std::memcpy(base + kCBOffsetTemporal, &tc, sizeof(tc));
        }

        // Denoise CB - offset 512. One final denoise pass, the reference's default DenoisePasses = 1
        // (CSDenoiseLastPass, finalApply = 1). Multi-pass would need an intermediate AO texture: N-1
        // dispatches with finalApply=0 ping-ponging aoRaw and scratch, then one finalApply=1 into
        // aoFinal.
        {
            DenoiseCB cb{};
            cb.viewportWidth     = m_w->vpW;
            cb.viewportHeight    = m_w->vpH;
            cb.depthLinearizeMul = depthLinearizeMul;
            cb.depthLinearizeAdd = depthLinearizeAdd;
            cb.denoiseBlurBeta   = denoiseBlurBeta;
            cb.finalApply        = 1u;
            std::memcpy(base + kCBOffsetDenoise, &cb, sizeof(cb));
        }

        // Prefilter CB - one dispatch writes the whole pyramid from the full-res device depth.
        {
            PrefilterCB pc{};
            pc.srcSize[0]           = m_w->vpW;
            pc.srcSize[1]           = m_w->vpH;
            pc.dstSize[0]           = m_w->vpW;
            pc.dstSize[1]           = m_w->vpH;
            pc.depthUnpackConsts[0] = depthLinearizeMul;
            pc.depthUnpackConsts[1] = depthLinearizeAdd;
            pc.effectRadius         = effectRadius;
            pc.effectFalloffRange   = effectFalloffRange;
            pc.radiusMultiplier     = radiusMultiplier;
            pc.srcMip               = 0;
            std::memcpy(base + kCBOffsetPrefilter, &pc, sizeof(pc));
        }
    }

    // ---- Pass A: 5-mip depth prefilter (single dispatch) ----
    // Writes all 5 linear-depth mips through u0..u4, like Intel's XeGTAO_PrefilterDepths16x16.
    {
        if (m_w->linearDepthState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_w->linearDepth, m_w->linearDepthState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_w->linearDepthState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_prefilterPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, kCBOffsetPrefilter, cl);
        gfx.SetComputeDescriptorTable(kSRV0, m_w->depthSrvHandle, cl);
        // u0..u4 - one UAV per mip level.
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_w->linearDepth, 0), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_w->linearDepth, 1), cl);
        gfx.SetComputeDescriptorTable(kUAV2,
            gfx.GetTextureMipUAVGpuHandle(m_w->linearDepth, 2), cl);
        gfx.SetComputeDescriptorTable(kUAV3,
            gfx.GetTextureMipUAVGpuHandle(m_w->linearDepth, 3), cl);
        gfx.SetComputeDescriptorTable(kUAV4,
            gfx.GetTextureMipUAVGpuHandle(m_w->linearDepth, 4), cl);

        // 8x8 threads per group, each a 2x2 mip-0 quad -> 16x16 px per group.
        gfx.DispatchCompute((m_w->vpW + 15) / 16, (m_w->vpH + 15) / 16, 1, cl);

        // Whole pyramid -> SRV_COMPUTE for the main pass's SampleLevel reads.
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_w->linearDepth, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_w->linearDepthState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    // ---- Pass B: GTAO main ----
    {
        if (m_w->aoRawState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_w->aoRaw, m_w->aoRawState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_w->aoRawState = RHI::ResourceState::UNORDERED_ACCESS;
        }
        if (m_w->edgesState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_w->edges, m_w->edgesState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_w->edgesState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_mainPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, kCBOffsetMain, cl);
        gfx.SetComputeDescriptorTable(kSRV0,
            gfx.GetTextureSRVGpuHandle(m_w->linearDepth), cl);
        if (m_w->normalSrvHandle)
            gfx.SetComputeDescriptorTable(kSRV1, m_w->normalSrvHandle, cl);
        // t2 space2 - Hilbert LUT used by SpatioTemporalNoise.
        if (m_hilbertLUTSrv)
            gfx.SetComputeDescriptorTable(kSRV2, m_hilbertLUTSrv, cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_w->aoRaw), cl);
        gfx.SetComputeDescriptorTable(kUAV1, gfx.GetTextureUAVGpuHandle(m_w->edges), cl);
        gfx.DispatchCompute((m_w->vpW + 7) / 8, (m_w->vpH + 7) / 8, 1, cl);
    }

    // Raw AO + edges -> SRV_COMPUTE for the denoise and temporal reads. Denoise runs first so the
    // temporal variance clip sees a low-variance input: against raw 1-spp blue-noise AO the sigma
    // is wide enough to accept stale history (crevice ghost trails).
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_w->aoRaw, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_w->aoRawState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_w->edges, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_w->edgesState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // ---- Pass C: spatial denoise ----
    // Reads raw AO + edges and writes aoFinal, the denoised input of the temporal pass; the pass
    // output is the temporal history (GetAOSrvHandle).
    {
        if (m_w->aoFinalState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_w->aoFinal, m_w->aoFinalState, RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_w->aoFinalState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_denoisePSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, kCBOffsetDenoise, cl);
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_w->aoRaw), cl);
        gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_w->edges), cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_w->aoFinal), cl);
        // Each thread denoises 2x1 pixels -> halve the X dispatch count.
        const uint32_t halfW = (m_w->vpW + 1u) / 2u;
        gfx.DispatchCompute((halfW + 7u) / 8u, (m_w->vpH + 7u) / 8u, 1, cl);
    }

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_w->aoFinal, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_w->aoFinalState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    // ---- Pass D: temporal accumulation ----
    // Reads the denoised AO, velocity, edges and the previous history; writes this frame's history,
    // the pass output. On the denoised input the variance clip runs tight (gamma 1.0, sigma floor
    // 0.02) and rejectionDiff 0.05 is a real disocclusion signal; edges modulate alpha so
    // disocclusion-prone pixels trust new data faster.
    //
    // Velocity needs no barrier: Setup declares ReadSRV on it, so the graph already moved it to
    // SHADER_RESOURCE, like depth and normals.
    // The history's slots by role (RG::HistoryTexture), states tracked by the device. The graph
    // moves the imported AO plane (Cur to UNORDERED_ACCESS and Prev to SHADER_RESOURCE_COMPUTE in
    // this pass's Phase A, Cur to SHADER_RESOURCE in its first reader's), so the pass moves it only
    // without the import (-rgimportoff); the depth plane, which no other pass reads, it always
    // moves itself.
    const bool graphTracked = m_aoH.cur.IsValid();
    const RHI::Texture& aoCur  = h.tex.Cur(0);
    const RHI::Texture& aoPrev = h.tex.Prev(0);
    const RHI::Texture& zCur   = h.tex.Cur(1);
    const RHI::Texture& zPrev  = h.tex.Prev(1);
    {
        if (!graphTracked)
        {
            // Write target -> UAV.
            RG::TransitionTo(gfx, cl, aoCur, RHI::ResourceState::UNORDERED_ACCESS);
            // Read source (history) -> SRV. On the first frame it is still in its UAV creation state; the
            // shader ignores it at historyAlpha == 0, but the descriptor must come from a legal SRV state.
            RG::TransitionTo(gfx, cl, aoPrev, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        }

        // Prev-depth write target (next frame's read source) -> UAV.
        RG::TransitionTo(gfx, cl, zCur, RHI::ResourceState::UNORDERED_ACCESS);
        // Prev-depth read source (last frame's write) -> SRV. On the first frame it holds uninitialised
        // data, which is harmless: the shader gates the depth rejection on historyValid (alpha 0).
        RG::TransitionTo(gfx, cl, zPrev, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        // linearDepth is already SHADER_RESOURCE_COMPUTE from the prefilter's post-barrier.

        gfx.BindComputePipelineState(m_temporalPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, kCBOffsetTemporal, cl);
        // t0 space2 = denoised AO.
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_w->aoFinal), cl);
        // t1 space2 = packed edge mask, drives the alpha modulation in the shader.
        gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_w->edges), cl);
        gfx.SetComputeDescriptorTable(kSRV2,
            gfx.GetTextureSRVGpuHandle(aoPrev), cl);
        // t3 space2 (root slot 7) = prev-frame linear depth (read source).
        gfx.SetComputeDescriptorTable(S::SRV_t3,
            gfx.GetTextureSRVGpuHandle(zPrev), cl);
        // Velocity -> t4 space2 (root slot 8), the engine-wide convention (TAA uses the same slot).
        // The shader reads gVelocity unconditionally: bind the zero-velocity fallback when no GBuffer
        // velocity exists (an unbound slot 8 is a GPU-Based Validation error every frame; zero velocity
        // means no reprojection).
        gfx.SetComputeDescriptorTable(S::SRV_t4,
            m_w->velocitySrvHandle ? m_w->velocitySrvHandle : m_zeroVelocitySrv, cl);
        // t5 space2 (root slot 6) = current-frame linear depth pyramid; the shader Loads mip 0.
        gfx.SetComputeDescriptorTable(S::SRV_t5,
            gfx.GetTextureSRVGpuHandle(m_w->linearDepth), cl);

        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureUAVGpuHandle(aoCur), cl);
        // u1 space2 = this frame's depth copy -> next frame's prev-depth read source.
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureUAVGpuHandle(zCur), cl);
        gfx.DispatchCompute((m_w->vpW + 7) / 8, (m_w->vpH + 7) / 8, 1, cl);
    }
    // Write buffer -> SHADER_RESOURCE (pixel and non-pixel): Lighting.ps samples it as gSSAO (t18)
    // later this frame on the same graphics queue, with no queue handoff in between to change the
    // state. Next frame's temporal read moves it back to SHADER_RESOURCE_COMPUTE. Imported, the
    // graph does this in LightingPass's Phase A.
    if (!graphTracked)
        RG::TransitionTo(gfx, cl, aoCur, RHI::ResourceState::SHADER_RESOURCE);

    // Prev-depth write buffer -> SRV for next frame's temporal read.
    RG::TransitionTo(gfx, cl, zCur, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    // Cur holds the just-written final AO (what GetAOSrvHandle returns from here); next frame's
    // OnPrepare flips the history, so next frame writes the other slot.
    h.tex.MarkWritten();
    h.valid = true;
    return cl;
}
