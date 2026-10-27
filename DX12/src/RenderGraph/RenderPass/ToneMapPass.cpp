#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include "System/Check.h"
#include <cstring>

// Compute root signature slots (shared by all compute passes) from RHI::Slots::Cmp
// (Graphics/RootSignatureLayout.h); the local names only carry the per-purpose
// comments. The root-index order is deliberately non-monotonic in register space
// (6 -> t5, 7 -> t3).
namespace S = RHI::Slots::Cmp;
static constexpr auto kCBSlot  = S::CB;      // CBV  b0 space2
static constexpr auto kSRV0    = S::SRV_t0;  // SRV  t0 space2 - HDR scene
static constexpr auto kSRV1    = S::SRV_t1;  // SRV  t1 space2 - bloom
static constexpr auto kSRV2    = S::SRV_t2;  // SRV  t2 space2 - exposure buffer
static constexpr auto kUAV0    = S::UAV_u0;  // UAV  u0 space2 - composition buffer
static constexpr auto kUAV1    = S::UAV_u1;  // UAV  u1 space2 - LUT 3D (bake target)
static constexpr auto kSRV5    = S::SRV_t5;  // SRV  t5 space2 - LUT 3D (tonemap read)
static constexpr auto kSRV3    = S::SRV_t3;  // SRV  t3 space2 - lens flare additive

static constexpr uint32_t kLutSize = 32;

// Every ColorGradingParams field but vignetteStrength and filmGrain feeds the LUT bake (the harness
// flags aside): equal to the defaults in all the others, the bake is an identity.
static bool IsIdentityGrading(ColorGradingParams p)
{
    p.vignetteStrength = 0.0f;
    p.filmGrain        = 0.0f;
    p.debugFlags       = 0u;
    return p == ColorGradingParams{};
}

// Name becomes kBaseName.suffix; see GetName() in the header.
void ToneMapPass::SetNameSuffix(const char* suffix)
{
    // An empty suffix would reproduce the main instance's name, and the timestamp
    // lookup is first-match-wins: keep the name and report instead.
    if (!ENGINE_CHECK(suffix && *suffix,
                      "ToneMapPass::SetNameSuffix: empty suffix would duplicate '%s'",
                      kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

// Call only at a quiesced boundary (see the header): ImGui may still sample
// its composition buffer.
void ToneMapPass::Shutdown(IGraphicsDevice& gfx)
{
    if (m_out->tex.IsValid()) gfx.DestroyTexture(m_out->tex);
    for (LutSlot& lut : m_luts)
        if (lut.tex.IsValid()) { gfx.DestroyTexture(lut.tex); lut = LutSlot{}; }
    if (m_pso.IsValid())         gfx.DestroyPipelineState(m_pso);
    if (m_lutPso.IsValid())      gfx.DestroyPipelineState(m_lutPso);
    m_shaderLib.Shutdown(gfx);
    m_cb.Destroy(gfx);

    m_out->lastW = m_out->lastH = 0;
    m_out->state = RHI::ResourceState::UNDEFINED;
    m_out = &m_mainOutput;
    m_gfx = nullptr;
}

void ToneMapPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");

    // Tonemap shader
    m_shaderLib.Register(ShaderID::ToneMap_CS, RHI::ShaderStage::CS,
                         "ToneMap.cs.hlsl", "CSMain");
    // GenerateLUT shader
    m_shaderLib.Register(ShaderID::GenerateLUT_CS, RHI::ShaderStage::CS,
                         "GenerateLUT.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    // Tonemap CB
    m_cb.Create(gfx, "ToneMap.CB");

    // The colour-grading CB is per-frame constants, allocated in BakeLUT.

    // The first grading LUT
    CreateLutSlot(m_luts[0]);

    LOG_SUCCESS("ToneMapPass: initialized (with procedural LUT)");
}

// Creates into the live members; a failed rebuild keeps the old pipeline.
bool ToneMapPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::ToneMap_CS);
    if (!cs) { LOG_ERROR("ToneMapPass: ToneMap_CS not found"); return false; }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("ToneMapPass: tonemap PSO failed"); return false; }

    const RHI::Shader* lutCs = m_shaderLib.GetShader(ShaderID::GenerateLUT_CS);
    if (!lutCs) { LOG_ERROR("ToneMapPass: GenerateLUT_CS not found"); return false; }

    RHI::PipelineStateDesc ld{};
    ld.cs = lutCs;
    if (!gfx.CreatePipelineState(ld, m_lutPso))
    { LOG_ERROR("ToneMapPass: LUT PSO failed"); return false; }
    return true;
}

void ToneMapPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_gfx) return;   // never initialised, or Shutdown already ran
    if (!m_shaderLib.ClearCaches()) return;   // none of its shaders changed
    CreatePipelines(gfx);
    for (LutSlot& lut : m_luts) lut.baked = false;   // they may hold the old GenerateLUT.cs output
}

bool ToneMapPass::CreateLutSlot(LutSlot& slot)
{
    if (!m_gfx) return false;

    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
    td.width      = kLutSize;
    td.height     = kLutSize;
    td.depth      = kLutSize;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, slot.tex))
    {
        LOG_ERROR("ToneMapPass: failed to create LUT 3D texture");
        return false;
    }

    slot.state  = RHI::ResourceState::UNORDERED_ACCESS;
    slot.baked  = false;
    LOG_INFO("ToneMapPass: created %ux%ux%u LUT (R16G16B16A16_FLOAT)", kLutSize, kLutSize, kLutSize);
    return true;
}

void ToneMapPass::ReserveLutSlots(uint32_t count)
{
    for (uint32_t i = 1; i < count && i < kMaxLuts; ++i)
        if (!m_luts[i].tex.IsValid()) CreateLutSlot(m_luts[i]);
}

ToneMapPass::LutSlot* ToneMapPass::LutFor(RHI::CommandList cl)
{
    ++m_lutClock;
    LutSlot* victim = nullptr;
    for (LutSlot& lut : m_luts)
    {
        if (!lut.tex.IsValid()) continue;
        if (lut.baked && lut.params == m_gradingParams) { lut.lastUse = m_lutClock; return &lut; }
        if (!victim || (!lut.baked && victim->baked) || (lut.baked == victim->baked && lut.lastUse < victim->lastUse))
            victim = &lut;
    }
    if (!victim) return nullptr;
    BakeLUT(cl, *victim);
    victim->lastUse = m_lutClock;
    return victim->baked ? victim : nullptr;
}

void ToneMapPass::BakeLUT(RHI::CommandList cl, LutSlot& slot)
{
    auto& gfx = *m_gfx;

    // Grading params as per-frame constants, so a re-bake cannot overwrite what a
    // frame still in flight reads. Allocation failure (reported): the slot stays unbaked
    // and bakes next frame.
    const RHI::Constants<ColorGradingParams> cb = AllocConstants<ColorGradingParams>(gfx);
    if (!cb) return;
    *cb.cpu = m_gradingParams;

    // Ensure LUT is in UAV state
    if (slot.state != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &slot.tex, slot.state, RHI::ResourceState::UNORDERED_ACCESS), cl);
        slot.state = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // Dispatch LUT bake
    gfx.BindComputePipelineState(m_lutPso, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, cb.gpu, cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetTextureUAVGpuHandle(slot.tex), cl);
    gfx.DispatchCompute(kLutSize / 4, kLutSize / 4, kLutSize / 4, cl);

    // Transition to SRV for the tonemap read
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &slot.tex, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    slot.state  = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    slot.params = m_gradingParams;
    slot.baked  = true;
}

void ToneMapPass::RebuildTexture()
{
    if (!m_gfx) return;
    if (m_out->tex.IsValid()) m_gfx->DestroyTexture(m_out->tex);

    RHI::TextureDesc td{};
    td.width      = m_vpW;
    td.height     = m_vpH;
    // SDR -> RGBA8; HDR -> FP16, the only format that holds the >1.0 headroom and is
    // guaranteed to support typed UAV load+store (GlassShatterComposite.cs.hlsl
    // read-modify-writes this texture).
    td.format     = HDRCompositionFormat(m_outputMode);
    // RENDER_TARGET so UIPass can bind it via OMSetRenderTargets to draw HUD/menus on
    // top of the tone-mapped scene; SRV+UAV+RT on one texture costs only an extra RTV.
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE
                  | RHI::BindFlag::UNORDERED_ACCESS
                  | RHI::BindFlag::RENDER_TARGET;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "ToneMapPass.FinalOutput";
    if (!m_gfx->CreateTexture(td, m_out->tex))
        LOG_ERROR("ToneMapPass: failed to create final output texture");

    // GraphicsDX12::CreateTexture forces the initial state to RENDER_TARGET whenever
    // bindFlags include RT (regardless of td.layout). Track that real state; the first
    // Execute transitions RT -> UAV.
    m_out->state = RHI::ResourceState::RENDERTARGET;
    m_out->lastW = m_vpW;
    m_out->lastH = m_vpH;
    m_out->lastMode = m_outputMode;
    LOG_INFO("ToneMapPass: rebuilt final output %ux%u (%s, %s)", m_vpW, m_vpH,
             IsHDR(m_outputMode) ? "R16G16B16A16_FLOAT" : "R8G8B8A8_UNORM",
             HDROutputModeName(m_outputMode));
}

void ToneMapPass::BindView(ViewState& view)
{
    m_out = view.Slot() == 0 ? &m_mainOutput : &view.Data<ViewOutput>(this).out;
}

const RHI::Texture* ToneMapPass::FinalOutputOf(const ViewState& view) const
{
    if (view.Slot() == 0) return m_mainOutput.tex.IsValid() ? &m_mainOutput.tex : nullptr;
    const ViewOutput* v = view.FindData<ViewOutput>(this);
    return v && v->out.tex.IsValid() ? &v->out.tex : nullptr;
}

uint64_t ToneMapPass::GetFinalOutputSrvHandle() const
{
    if (!m_gfx || !m_out->tex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_out->tex);
}

RHI::CommandList ToneMapPass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid()) return cl;
    if (m_vpW == 0 || m_vpH == 0) return cl;

    // Worker-thread safety net for a size change only. Do not test the output mode
    // here: this runs on RenderWorker_Compute, and a rebuild would allocate descriptors
    // off the main thread. The Renderer sets the mode and calls EnsureTexture on the main
    // thread (OnPrepare) before kicking the worker.
    if (m_vpW != m_out->lastW || m_vpH != m_out->lastH)
        RebuildTexture();
    if (!m_out->tex.IsValid()) return cl;

    auto& gfx = *m_gfx;

    // ---- Step 1: Bake LUT if dirty ----
    // Neutral parameters bake an identity that is not one bit for bit (fp16 texels, trilinear between
    // lattice points): no fetch then, and no bake until the parameters leave neutral. A LUT that never
    // baked (its constants failed to allocate) holds garbage: ungraded until a bake lands.
    const bool grade = m_colorGradingEnabled && m_luts[0].tex.IsValid() && !IsIdentityGrading(m_gradingParams);
    LutSlot* lut = grade ? LutFor(cl) : nullptr;
    const bool applyLut = lut != nullptr;

    // ---- Step 2: Tonemap dispatch ----
    if (auto* slot = m_cb.Current(gfx))
    {
        ToneMapPass::ToneMapCB cb{};
        cb.width             = m_vpW;
        cb.ueGrading         = m_ueGrading;
        cb.height            = m_vpH;
        cb.bloomStrength     = (m_bloomSrvHandle != 0) ? m_bloomStrength : 0.0f;
        cb.enableLUT         = applyLut ? 1u : 0u;
        cb.lensFlareStrength = (m_lensFlareSrvHandle != 0) ? m_lensFlareStrength : 0.0f;
        // Report the mode the texture was built for, not the requested one: they differ
        // for one frame when a mode change lands after EnsureTexture, and an HDR mode on
        // an RGBA8 target would clip every HDR value to 1.0.
        cb.hdrMode           = static_cast<uint32_t>(m_out->lastMode);
        // Guard the divide: paperWhite is clamped >0 by the settings layer, but
        // this CB is also reachable before any SetOutputMode call.
        cb.hdrPeakRatio      = (m_paperWhiteNits > 0.0f)
                             ? (m_peakNits / m_paperWhiteNits) : 1.0f;
        cb.debugInjectColumns = m_debug.injectColumns;
        cb.debugFlags        = (m_debug.unguarded ? 1u : 0u) | (m_debug.noDither ? 2u : 0u);
        *slot = cb;
    }

    // Transition final output to UAV
    if (m_out->state != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_out->tex, m_out->state, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_out->state = RHI::ResourceState::UNORDERED_ACCESS;
    }

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRV0, m_hdrSrvHandle, cl);
    // No bloom chain this frame (a view that ran no bloom): t1 still needs a live descriptor, and
    // bloomStrength is 0 then.
    gfx.SetComputeDescriptorTable(kSRV1, m_bloomSrvHandle != 0 ? m_bloomSrvHandle : m_hdrSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kSRV2, m_exposureSrvHandle, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_out->tex), cl);

    // Bind LUT SRV for tonemap read
    if (applyLut)
        gfx.SetComputeDescriptorTable(kSRV5, gfx.GetTextureSRVGpuHandle(lut->tex), cl);

    // Lens flare SRV (t3 space2), bound only when set. Without one lensFlareStrength
    // is 0, so the flare adds nothing (the shader still samples t3 and multiplies by 0).
    if (m_lensFlareSrvHandle != 0)
        gfx.SetComputeDescriptorTable(kSRV3, m_lensFlareSrvHandle, cl);

    gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);

    // Transition final output for next stage
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_out->tex, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_out->state = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

    return cl;
}

void ToneMapPass::TransitionForDisplay(RHI::CommandList& cl)
{
    if (!m_out->tex.IsValid()) return;
    if (m_out->state == RHI::ResourceState::SHADER_RESOURCE) return;

    auto& gfx = *m_gfx;
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_out->tex, m_out->state,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_out->state = RHI::ResourceState::SHADER_RESOURCE;
}

void ToneMapPass::PrepareForCompute(RHI::CommandList& cl)
{
    if (!m_out->tex.IsValid()) return;
    if (m_out->state == RHI::ResourceState::UNORDERED_ACCESS) return;

    auto& gfx = *m_gfx;
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_out->tex, m_out->state,
        RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_out->state = RHI::ResourceState::UNORDERED_ACCESS;
}

void ToneMapPass::OnPrepare(const RG::PrepareContext& c)
{
    ISurface& surface = c.gfx->MainSurface();
    SetOutputMode(surface.GetHDROutputMode(), surface.GetPaperWhiteNits(), surface.GetPeakNits());
    EnsureTexture(c.viewportW, c.viewportH);
}
