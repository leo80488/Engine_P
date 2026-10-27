#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "ECS/Components.h"

#include <cstring>
#include <algorithm>

// Compute root signature slots (shared with Bloom / AutoExposure), aliases of
// Graphics/RootSignatureLayout.h.
static constexpr auto kCBSlot = RHI::Slots::Cmp::CB;      // b0 space2
static constexpr auto kSRV0   = RHI::Slots::Cmp::SRV_t0;  // t0 space2
static constexpr auto kSRV1   = RHI::Slots::Cmp::SRV_t1;  // t1 space2
static constexpr auto kUAV0   = RHI::Slots::Cmp::UAV_u0;  // u0 space2

static constexpr uint32_t kSHCoeffCount  = 9;
static constexpr uint32_t kSHSampleCount = 2048;

// Per-mip GGX importance sample count. For a smooth atmospheric sky 32-256
// samples are indistinguishable from 1024 (Karis, split-sum), so this table costs
// ~8x less prefilter time at near-zero visual difference:
//   mip 0   (roughness ~0)                  - 1 sample, a copy of the source direction
//   mip 1-2 (sharp, high-detail reflections) - 128
//   mip 3   (medium)                         - 64
//   mip 4-6 (rough: wide lobe, tiny target)  - 32
static constexpr uint32_t kPrefilterSampleTable[7] = {
    1,      // mip 0 (128^2)
    128,    // mip 1 ( 64^2)
    128,    // mip 2 ( 32^2)
    64,     // mip 3 ( 16^2)
    32,     // mip 4 (  8^2)
    32,     // mip 5 (  4^2)
    32,     // mip 6 (  2^2)
};
// kPrefilterCBStride is defined as a class-static in SkyIBLPass.h so the
// PrefilterCBPool wrapper can size itself at compile time.

struct alignas(16) PrefilterConstants
{
    uint32_t faceSize;
    uint32_t sampleCount;
    float    roughness;
    uint32_t sourceMipCount;
    uint32_t faceIndex;    // temporal: the cube face this dispatch writes
    uint32_t useCoverage;  // 0 for global sky; reflection probes set 1
    uint32_t _pad1;
    uint32_t _pad2;
};
static_assert(sizeof(PrefilterConstants) == 32,
              "PrefilterConstants must match HLSL");

// AtmosphereConstants is a nested type in SkyIBLPass.h, so the
// FrameCB<AtmosphereConstants> member can be instantiated at class definition. Do
// not redeclare it here: two copies drift (ODR).

struct alignas(16) LutDimConstants
{
    uint32_t lutWidth;
    uint32_t lutHeight;
    uint32_t _pad0;
    uint32_t _pad1;
};

struct alignas(16) SkyViewConstants
{
    float    sunDir[3];         float cameraAltitudeKm;
    uint32_t lutWidth;
    uint32_t lutHeight;
    uint32_t _pad0;
    uint32_t _pad1;
};

struct alignas(16) AerialConstants
{
    float    sunDir[3];          float cameraAltitudeKm;
    float    sunColor[3];        float maxDistKm;
    float    invViewProj[16];
    float    cameraPosWorld[3];  float _pad0;
    float    cameraForward[3];   float _pad1;
    uint32_t lutW;
    uint32_t lutH;
    uint32_t lutD;
    uint32_t _pad2;
};

// kLutCBStride is a class-static in SkyIBLPass.h (sized for AerialConstants).
static constexpr uint32_t kLutCBSlotTrans  = 0;
static constexpr uint32_t kLutCBSlotMS     = 1;
static constexpr uint32_t kLutCBSlotSV     = 2;
static constexpr uint32_t kLutCBSlotAtmo   = 3;
static constexpr uint32_t kLutCBSlotAerial = 4;

void SkyIBLPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SkySHProjection_CS,  RHI::ShaderStage::CS,
                         "SkySHProjection.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::SpecularPrefilter_CS, RHI::ShaderStage::CS,
                         "SpecularPrefilter.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::SkyAtmosphere_CS, RHI::ShaderStage::CS,
                         "SkyAtmosphere.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AtmosphereTransmittance_CS, RHI::ShaderStage::CS,
                         "AtmosphereTransmittance.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AtmosphereMultiScatter_CS, RHI::ShaderStage::CS,
                         "AtmosphereMultiScatter.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AtmosphereSkyView_CS, RHI::ShaderStage::CS,
                         "AtmosphereSkyView.cs.hlsl", "main");
    m_shaderLib.Register(ShaderID::AerialPerspective_CS, RHI::ShaderStage::CS,
                         "AerialPerspective.cs.hlsl", "main");

    if (!CreatePipelines(gfx)) return;

    // SH output buffer - 9 x float4 (.xyz = coefficient, .w = padding).
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = kSHCoeffCount * sizeof(float) * 4;
        bd.stride     = sizeof(float) * 4;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;

        float zeros[kSHCoeffCount * 4] = {};
        if (!gfx.CreateBuffer(bd, m_shBuffer, zeros))
        {
            LOG_ERROR("SkyIBLPass: SH buffer creation failed");
            return;
        }

        m_shUavHandle = gfx.GetBufferUAVGpuHandle(m_shBuffer);
        m_shSrvHandle = gfx.GetBufferSRVGpuHandle(m_shBuffer);
    }

    m_cb.Create(gfx, "SkyIBL.SHConstants");

    // Prefilter per-dispatch CB: one 256-byte slot per (face, mip) pair, so the full
    // bake (6 faces x 7 mips = 42 slots) does not race the GPU; a temporal update uses
    // 7 of them. The FrameCB ring covers the cross-frame race on the pool.
    m_prefilterCB.Create(gfx, "SkyIBL.PrefilterCB");

    // Atmosphere CB - one AtmosphereConstants per frame.
    m_atmosphereCB.Create(gfx, "SkyIBL.AtmosphereCB");
    m_materialSkyCB.Create(gfx, "SkyIBL.MaterialCB");

    // Shared LUT CB pool: 5 x 512-byte slots (Transmittance / MultiScatter /
    // SkyView / [unused Atmo] / Aerial). 512 because AerialConstants (invViewProj +
    // many floats) exceeds the 256-byte slot size.
    m_atmoLutCB.Create(gfx, "SkyIBL.AtmoLutCB");

    CreateSpecularCube(gfx);
    CreateAtmosphereCube(gfx);
    CreateAtmosphereLUTs(gfx);

    LOG_SUCCESS("SkyIBLPass: initialised (SH %u floats, specular %ux%u %u mips)",
                kSHCoeffCount * 4, kSpecularSize, kSpecularSize, kSpecularMips);
}

// Onto the live members: a failed rebuild keeps the old pipeline. False when
// one of the first three shaders is missing (Init stops there).
bool SkyIBLPass::CreatePipelines(IGraphicsDevice& gfx)
{
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SkySHProjection_CS);
        if (!cs) { LOG_ERROR("SkyIBLPass: SkySHProjection_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_pso))
            LOG_ERROR("SkyIBLPass: SH projection PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SpecularPrefilter_CS);
        if (!cs) { LOG_ERROR("SkyIBLPass: SpecularPrefilter_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_prefilterPSO))
            LOG_ERROR("SkyIBLPass: specular prefilter PSO failed");
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SkyAtmosphere_CS);
        if (!cs) { LOG_ERROR("SkyIBLPass: SkyAtmosphere_CS not found"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_atmospherePSO))
            LOG_ERROR("SkyIBLPass: atmosphere PSO failed");
    }
    auto makeCsPso = [&](ShaderID id, RHI::PipelineState& out, const char* tag)
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SkyIBLPass: %s_CS not found", tag); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, out))
            LOG_ERROR("SkyIBLPass: %s PSO failed", tag);
    };
    makeCsPso(ShaderID::AtmosphereTransmittance_CS, m_transmittancePSO, "Transmittance");
    makeCsPso(ShaderID::AtmosphereMultiScatter_CS,  m_multiScatterPSO,  "MultiScatter");
    makeCsPso(ShaderID::AtmosphereSkyView_CS,       m_skyViewPSO,       "SkyView");
    makeCsPso(ShaderID::AerialPerspective_CS,       m_aerialPSO,        "AerialPerspective");
    return true;
}

void SkyIBLPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_shaderLib.ClearCaches()) return;   // none of its shaders changed
    CreatePipelines(gfx);

    // Re-bake what the old shaders produced: the static LUTs (BakeStaticLUTs
    // takes them back to UAV), then - through the same dirty test a sun change
    // trips - the SkyView LUT, the cube, the SH and a full 6-face prefilter.
    m_staticLutsBaked        = false;
    m_prefilterNeedsFullBake = true;
    m_lastBakedSunIntensity  = -1.0f;
    // A custom sky material is a dynamic CS in m_shaderLib: forgetting the
    // path makes the next PrepareMaterialSky rebuild its PSO and reflection.
    m_materialSkyShader.clear();
}

void SkyIBLPass::CreateSpecularCube(IGraphicsDevice& gfx)
{
    // TextureCube (6 array slices) with UAV + 7-mip chain. The RHI creates the
    // TextureCube SRV (TEXTURECUBE misc flag) and the per-mip Texture2DArray UAVs
    // (mipLevels > 1 + UAV flag).
    RHI::TextureDesc td{};
    td.width      = kSpecularSize;
    td.height     = kSpecularSize;
    td.arraySize = 6;
    td.mipLevels = kSpecularMips;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "SkyIBL.Specular";

    if (!gfx.CreateTexture(td, m_specTex))
    {
        LOG_ERROR("SkyIBLPass: specular cubemap create failed");
        return;
    }
    m_specState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_specSrvHandle = gfx.GetTextureSRVGpuHandle(m_specTex);

    // The async reader's copy (AsyncReaderSrv). Created here, in Init's serial
    // window, never lazily on the graph worker that records the prefilter
    // (CreateTexture allocates bindless descriptors).
    if (Sys::CommandLine::Get().Has("-ddgiskysamecube"))
    {
        LOG_INFO("[DDGI_SKY] -ddgiskysamecube: no async copy - DDGI samples the live specular cube");
        return;
    }
    RHI::TextureDesc cd = td;
    cd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    cd.layout     = RHI::ResourceState::SHADER_RESOURCE;
    cd.debugName = "SkyIBL.SpecularAsyncCopy";
    if (!gfx.CreateTexture(cd, m_specCopyTex))
    {
        LOG_ERROR("SkyIBLPass: the specular cube's async copy failed - DDGI samples the live cube (R15)");
        return;
    }
    m_specCopyState = RHI::ResourceState::SHADER_RESOURCE;
    m_specCopySrv   = gfx.GetTextureSRVGpuHandle(m_specCopyTex);
}

// The faces the prefilter wrote since the last copy, every mip, from the
// specular cube into the async reader's copy. The cube is left in the state it was
// found in (its own tracker, m_specState); the copy goes back to SHADER_RESOURCE,
// the state the compute queue samples it in.
void SkyIBLPass::RecordAsyncCopy(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = *m_gfx;
    const RHI::ResourceState specFound = m_specState;

    RHI::GPUBarrier enter[2];
    uint32_t n = 0;
    if (specFound != RHI::ResourceState::COPY_SRC)
        enter[n++] = RHI::GPUBarrier::Image(&m_specTex, specFound, RHI::ResourceState::COPY_SRC);
    if (m_specCopyState != RHI::ResourceState::COPY_DST)
        enter[n++] = RHI::GPUBarrier::Image(&m_specCopyTex, m_specCopyState, RHI::ResourceState::COPY_DST);
    if (n) gfx.PushBarriers(enter, n, cl);

    for (uint32_t face = 0; face < 6; ++face)
    {
        if ((m_specCopyDirty & (1u << face)) == 0) continue;
        for (uint32_t mip = 0; mip < kSpecularMips; ++mip)
            gfx.CopyTextureSubresource(m_specTex, mip, face, m_specCopyTex, mip, face, cl);
    }

    RHI::GPUBarrier leave[2];
    n = 0;
    if (specFound != RHI::ResourceState::COPY_SRC)
        leave[n++] = RHI::GPUBarrier::Image(&m_specTex, RHI::ResourceState::COPY_SRC, specFound);
    leave[n++] = RHI::GPUBarrier::Image(&m_specCopyTex, RHI::ResourceState::COPY_DST,
                                        RHI::ResourceState::SHADER_RESOURCE);
    gfx.PushBarriers(leave, n, cl);

    m_specCopyState = RHI::ResourceState::SHADER_RESOURCE;
    m_specCopyDirty = 0;
}

void SkyIBLPass::RefreshAsyncCopy(RHI::CommandList cl)
{
    // Before the first fill the copy is nobody's (AsyncReaderSrv hands it out
    // only once valid), and DispatchPrefilter fills it; nothing written since
    // the last copy, nothing to do.
    if (!m_specCopyValid || m_specCopyDirty == 0 || !m_specCopyTex.IsValid() || !m_specTex.IsValid())
        return;
    RecordAsyncCopy(cl);
}

uint64_t SkyIBLPass::ResolveSkyboxSrvHandle(uint64_t staticFallback) const
{
    if (m_materialSkyEnabled && m_materialSkyValid) return m_materialSkySrv;
    if (m_skyboxSource == SkyboxSource::Atmosphere
        && m_atmosphereEnabled && m_atmosphereSrvHandle != 0)
        return m_atmosphereSrvHandle;
    return staticFallback;
}

void SkyIBLPass::CreateAtmosphereCube(IGraphicsDevice& gfx)
{
    RHI::TextureDesc td{};
    td.width      = kAtmosphereSize;
    td.height     = kAtmosphereSize;
    td.arraySize = 6;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!gfx.CreateTexture(td, m_atmosphereTex))
    {
        LOG_ERROR("SkyIBLPass: atmosphere cubemap create failed");
        return;
    }
    m_atmosphereState     = RHI::ResourceState::UNORDERED_ACCESS;
    m_atmosphereSrvHandle = gfx.GetTextureSRVGpuHandle(m_atmosphereTex);
}

void SkyIBLPass::DispatchAtmosphere(RHI::CommandList cl)
{
    if (!m_atmospherePSO.IsValid()) return;
    if (!m_atmosphereTex.IsValid()) return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_atmosphereState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_atmosphereTex, m_atmosphereState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_atmosphereState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* slot = m_atmosphereCB.Current(gfx))
    {
        AtmosphereConstants c{};
        // Use the geometric-sun pair (colour=0 below horizon) so the sky
        // produces no scattering at night.
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.sunColor[0] = m_atmosphereSunColor.x;
        c.sunColor[1] = m_atmosphereSunColor.y;
        c.sunColor[2] = m_atmosphereSunColor.z;
        c.faceSize         = kAtmosphereSize;
        c.cameraAltitudeKm = 0.5f;
        *slot = c;
    }

    gfx.BindComputePipelineState(m_atmospherePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmosphereCB.CurrentBuffer(gfx), cl);
    // t0 = SkyView LUT, t1 = Transmittance LUT (see SkyAtmosphere.cs.hlsl).
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_skyViewTex), cl);
    gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_atmosphereTex), cl);

    const uint32_t g = (kAtmosphereSize + 7) / 8;
    gfx.DispatchCompute(g, g, 6, cl);

    // UAV -> SRV (PIXEL | NON_PIXEL in D3D12) for the SH projection + prefilter
    // dispatches that follow, and for SkyboxPass / LightingPass later.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_atmosphereTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_atmosphereState = RHI::ResourceState::SHADER_RESOURCE;
}

void SkyIBLPass::DispatchPrefilter(RHI::CommandList cl, uint64_t sourceSrv)
{
    if (!m_prefilterPSO.IsValid()) return;
    if (!m_specTex.IsValid())      return;
    if (sourceSrv == 0)            return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_specState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_specTex, m_specState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_specState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    gfx.BindComputePipelineState(m_prefilterPSO, cl);
    gfx.SetComputeDescriptorTable(kSRV0, sourceSrv, cl);

    // Temporal schedule: the first bake after a toggle / startup does all 6 faces so
    // the IBL is valid at once; later frames do ONE face, rotating through the cube -
    // 6x cheaper per frame, with a 6-frame convergence latency that is invisible for a
    // slowly changing sky. The face is frame % 6, NOT a count of dispatches since the
    // bootstrap: the bootstrap's frame depends on when the sky material's textures
    // finished loading, and a counter started there would give every later face a
    // load-timing-dependent sky time.
    const uint32_t firstFace   = m_prefilterNeedsFullBake ? 0u
                                                          : static_cast<uint32_t>(m_frameNumber % 6u);
    const uint32_t facesThisRun = m_prefilterNeedsFullBake ? 6u : 1u;
    if (m_prefilterNeedsFullBake)
        LOG_INFO("SkyIBLPass: full 6-face prefilter at frame %llu",
                 static_cast<unsigned long long>(m_frameNumber));

    for (uint32_t fi = 0; fi < facesThisRun; ++fi)
    {
        const uint32_t face = firstFace + fi;

        for (uint32_t mip = 0; mip < kSpecularMips; ++mip)
        {
            const uint32_t faceW = (std::max)(1u, kSpecularSize >> mip);
            const float    roughness = (kSpecularMips <= 1) ? 0.0f
                                      : float(mip) / float(kSpecularMips - 1);

            // Pack the per-dispatch CB (each (face, mip) pair gets a unique
            // 256-byte slot so no writes overlap the GPU's read inside this
            // frame; FrameCB ring handles cross-frame).
            if (auto* pool = m_prefilterCB.Current(gfx))
            {
                PrefilterConstants c{};
                c.faceSize       = faceW;
                c.sampleCount    = (mip < 7) ? kPrefilterSampleTable[mip] : 32u;
                c.roughness      = roughness;
                c.sourceMipCount = 1;
                c.faceIndex      = face;
                const uint32_t slot = fi * kSpecularMips + mip;
                uint8_t* dst = pool->bytes + slot * kPrefilterCBStride;
                std::memcpy(dst, &c, sizeof(c));
                gfx.SetComputeRootCBV(kCBSlot, m_prefilterCB.CurrentBuffer(gfx),
                                      slot * kPrefilterCBStride, cl);
            }

            gfx.SetComputeDescriptorTable(kUAV0,
                gfx.GetTextureMipUAVGpuHandle(m_specTex, mip), cl);

            const uint32_t gx = (faceW + 7) / 8;
            const uint32_t gy = (faceW + 7) / 8;
            // Single face per dispatch (z = 1); shader writes into the face
            // slice selected by PrefilterCB::faceIndex.
            gfx.DispatchCompute(gx, gy, 1, cl);

            gfx.PushBarrier(RHI::GPUBarrier::Memory(nullptr), cl);
        }
    }
    // The faces the async reader's copy now lacks (RefreshAsyncCopy).
    m_specCopyDirty |= (facesThisRun == 6u) ? uint8_t(0x3F) : static_cast<uint8_t>(1u << firstFace);

    if (m_prefilterNeedsFullBake)
        m_prefilterNeedsFullBake = false;

    // Transition to SRV for this frame's lighting.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_specTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_specState = RHI::ResourceState::SHADER_RESOURCE;

    m_lastBakedSrc = sourceSrv;
    m_specValid    = true;

    // The async reader's copy gets its first fill HERE, in this pass's own list.
    // Until it holds a complete cube AsyncReaderSrv hands it to nobody, so nothing can
    // be sampling it now; afterwards only RefreshAsyncCopy writes it, on a list
    // ordered after the reader.
    if (!m_specCopyValid && m_specCopyTex.IsValid())
    {
        m_specCopyDirty = 0x3F;
        RecordAsyncCopy(cl);
        m_specCopyValid = true;
        LOG_INFO("[DDGI_SKY] the specular cube's async copy holds the cube: DDGI samples it, "
                 "refreshed after DDGI's list each frame - a frame late (-ddgiskysamecube: the live cube)");
    }
}

// Creates a 2D R16G16B16A16_FLOAT texture with UAV + SRV views through the RHI,
// left in UNORDERED_ACCESS.
static bool CreateLut2D(IGraphicsDevice& gfx,
                        uint32_t w, uint32_t h,
                        RHI::Texture& outTex,
                        const char* tag)
{
    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    if (!gfx.CreateTexture(td, outTex))
    {
        LOG_ERROR("SkyIBLPass: %s LUT create failed", tag);
        return false;
    }
    return true;
}

void SkyIBLPass::CreateAtmosphereLUTs(IGraphicsDevice& gfx)
{
    CreateLut2D(gfx, kTransmittanceW, kTransmittanceH, m_transmittanceTex, "Transmittance");
    CreateLut2D(gfx, kMultiScatterW,  kMultiScatterH,  m_multiScatterTex,  "MultiScatter");
    CreateLut2D(gfx, kSkyViewW,       kSkyViewH,       m_skyViewTex,       "SkyView");

    CreateAerial(gfx, m_mainAerial);
}

// Aerial perspective: 3D R16G16B16A16_FLOAT (32x32x32) with UAV + SRV. Starts in SHADER_RESOURCE
// so the lighting pass can sample zeros safely on the first frame, before DispatchAerialPerspective
// runs.
void SkyIBLPass::CreateAerial(IGraphicsDevice& gfx, Aerial& a)
{
    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
    td.width      = kAerialW;
    td.height     = kAerialH;
    td.depth      = kAerialD;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;
    if (!gfx.CreateTexture(td, a.tex))
    {
        LOG_ERROR("SkyIBLPass: Aerial Perspective 3D LUT creation failed");
        return;
    }
    a.state = RHI::ResourceState::SHADER_RESOURCE;
    a.srv   = gfx.GetTextureSRVGpuHandle(a.tex);
}

void SkyIBLPass::ViewAerial::Release(IGraphicsDevice& gfx)
{
    if (aerial.tex.IsValid()) gfx.DestroyTexture(aerial.tex);
    aerial = Aerial{};
}

void SkyIBLPass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
    {
        m_a = &m_mainAerial;
        return;
    }
    Aerial& a = view.Data<ViewAerial>(this).aerial;
    if (!a.tex.IsValid() && m_gfx) CreateAerial(*m_gfx, a);
    m_a = &a;
}

void SkyIBLPass::SetViewCameraForAerial(ViewState& view, const DirectX::XMFLOAT3& cameraPosWorld,
                                        const DirectX::XMFLOAT3& cameraForwardWorld,
                                        const float invViewProjRowMajor[16])
{
    if (view.Slot() == 0) return;
    SetAerialCamera(view.Data<ViewAerial>(this).aerial, cameraPosWorld, cameraForwardWorld, invViewProjRowMajor);
}

void SkyIBLPass::DispatchAerialPerspective(RHI::CommandList cl)
{
    Aerial& a = *m_a;
    if (!m_aerialPSO.IsValid() || !a.tex.IsValid()) return;
    if (!m_staticLutsBaked)                               return;

    IGraphicsDevice& gfx = *m_gfx;

    if (a.state != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&a.tex, a.state,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        a.state = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* pool = m_atmoLutCB.Current(gfx))
    {
        AerialConstants c{};
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.sunColor[0] = m_atmosphereSunColor.x;
        c.sunColor[1] = m_atmosphereSunColor.y;
        c.sunColor[2] = m_atmosphereSunColor.z;
        // Real camera altitude (world Y in metres), so the haze matches the viewpoint.
        c.cameraAltitudeKm = std::max(a.cameraPosWorld.y, 0.0f) * 0.001f;
        c.maxDistKm = kAerialMaxKm;
        for (int i = 0; i < 16; ++i) c.invViewProj[i] = a.invViewProj[i];
        c.cameraPosWorld[0] = a.cameraPosWorld.x;
        c.cameraPosWorld[1] = a.cameraPosWorld.y;
        c.cameraPosWorld[2] = a.cameraPosWorld.z;
        c.cameraForward[0] = a.cameraForwardWorld.x;
        c.cameraForward[1] = a.cameraForwardWorld.y;
        c.cameraForward[2] = a.cameraForwardWorld.z;
        c.lutW = kAerialW; c.lutH = kAerialH; c.lutD = kAerialD;
        std::memcpy(pool->bytes + kLutCBSlotAerial * kLutCBStride, &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_aerialPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotAerial * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_multiScatterTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(a.tex), cl);

    // 8x8 threadgroups in xy, z = depth slice: dispatch (4, 4, kAerialD).
    const uint32_t gx = (kAerialW + 7) / 8;
    const uint32_t gy = (kAerialH + 7) / 8;
    gfx.DispatchCompute(gx, gy, kAerialD, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(&a.tex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    a.state = RHI::ResourceState::SHADER_RESOURCE;

    a.valid = true;
}

void SkyIBLPass::BakeStaticLUTs(RHI::CommandList cl)
{
    if (m_staticLutsBaked)         return;
    if (!m_transmittancePSO.IsValid() || !m_multiScatterPSO.IsValid()) return;
    if (!m_transmittanceTex.IsValid() || !m_multiScatterTex.IsValid()) return;

    IGraphicsDevice& gfx = *m_gfx;

    // The first bake finds both LUTs in their creation state (UAV); a re-bake
    // after a shader hot reload finds them in SHADER_RESOURCE.
    if (m_transmittanceState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_transmittanceTex, m_transmittanceState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_transmittanceState = RHI::ResourceState::UNORDERED_ACCESS;
    }
    if (m_multiScatterState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_multiScatterTex, m_multiScatterState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_multiScatterState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // Upload CB slots for Transmittance and MultiScatter dims.
    if (auto* pool = m_atmoLutCB.Current(gfx))
    {
        LutDimConstants t{ kTransmittanceW, kTransmittanceH, 0, 0 };
        LutDimConstants m{ kMultiScatterW,  kMultiScatterH,  0, 0 };
        std::memcpy(pool->bytes + kLutCBSlotTrans * kLutCBStride, &t, sizeof(t));
        std::memcpy(pool->bytes + kLutCBSlotMS    * kLutCBStride, &m, sizeof(m));
    }

    // ---- Transmittance ----
    gfx.BindComputePipelineState(m_transmittancePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotTrans * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_transmittanceTex), cl);
    gfx.DispatchCompute((kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8, 1, cl);

    // Transmittance -> SRV so MultiScatter can read it.
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_transmittanceTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_transmittanceState = RHI::ResourceState::SHADER_RESOURCE;

    // ---- MultiScatter ----
    // The paper's parallel-per-pixel layout: one 1x1x64 threadgroup per LUT texel, so
    // Dispatch = (lutW, lutH, 1). Each threadgroup takes 64 sphere samples and
    // LDS-reduces them to one output pixel.
    gfx.BindComputePipelineState(m_multiScatterPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotMS * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_multiScatterTex), cl);
    gfx.DispatchCompute(kMultiScatterW, kMultiScatterH, 1, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_multiScatterTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_multiScatterState = RHI::ResourceState::SHADER_RESOURCE;

    m_staticLutsBaked = true;
    LOG_SUCCESS("SkyIBLPass: Transmittance + MultiScatter LUTs baked");
}

void SkyIBLPass::DispatchSkyViewLUT(RHI::CommandList cl)
{
    if (!m_skyViewPSO.IsValid() || !m_skyViewTex.IsValid()) return;
    if (!m_staticLutsBaked)                                 return;

    IGraphicsDevice& gfx = *m_gfx;

    if (m_skyViewState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_skyViewTex, m_skyViewState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_skyViewState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* pool = m_atmoLutCB.Current(gfx))
    {
        SkyViewConstants c{};
        c.sunDir[0] = m_atmosphereSunDir.x;
        c.sunDir[1] = m_atmosphereSunDir.y;
        c.sunDir[2] = m_atmosphereSunDir.z;
        c.cameraAltitudeKm = 0.5f; // hard-coded ground-level camera
        c.lutWidth  = kSkyViewW;
        c.lutHeight = kSkyViewH;
        std::memcpy(pool->bytes + kLutCBSlotSV * kLutCBStride, &c, sizeof(c));
    }

    gfx.BindComputePipelineState(m_skyViewPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_atmoLutCB.CurrentBuffer(gfx),
                          kLutCBSlotSV * kLutCBStride, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetTextureSRVGpuHandle(m_transmittanceTex), cl);
    // t1 space2 (root param 2) - MultiScatter LUT.
    gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_multiScatterTex), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_skyViewTex), cl);
    gfx.DispatchCompute((kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8, 1, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_skyViewTex,
                    RHI::ResourceState::UNORDERED_ACCESS,
                    RHI::ResourceState::SHADER_RESOURCE), cl);
    m_skyViewState = RHI::ResourceState::SHADER_RESOURCE;
}

RHI::CommandList SkyIBLPass::Execute(RHI::CommandList cl)
{
    // A secondary view: only its camera's aerial-perspective LUT (BindView).
    if (m_a != &m_mainAerial)
    {
        if (m_pso.IsValid() && m_atmosphereEnabled && m_aerialCompositeEnabled)
        {
            const uint32_t r = m_gfx->BeginGPUTimestamp(cl, "SkyIBL.AerialPerspective");
            DispatchAerialPerspective(cl);
            m_gfx->EndGPUTimestamp(cl, r);
        }
        return cl;
    }

    // No async reader records this frame (SetAsyncReaderActive), so nothing can be
    // sampling the copy: bring it up to date here rather than leave it at whenever
    // DDGI last ran.
    if (!m_asyncReaderActive) RefreshAsyncCopy(cl);

    if (!m_pso.IsValid())      return cl;
    if (!m_shBuffer.IsValid()) return cl;

    // Pure static skybox - do NOTHING: no atmosphere, no SH projection, no specular
    // prefilter. The lighting path uses the scene's irradiance + radiance cubemaps
    // (zero GPU cost here).
    if (!m_atmosphereEnabled && !m_materialSkyEnabled)
        return cl;

    // ---- Dirty check: skip every per-frame dispatch when nothing changed ----
    // The atmosphere cube, SkyView LUT and SH coefficients are PURE functions of the
    // sun direction and intensity: if neither moved since the last bake, every
    // downstream buffer already holds the result, so a paused TOD or a static scene
    // costs ~0 here. A material sky always updates.
    const float cosDelta = DirectX::XMVectorGetX(
        DirectX::XMVector3Dot(
            DirectX::XMLoadFloat3(&m_sunDir),
            DirectX::XMLoadFloat3(&m_lastBakedSunDir)));
    const bool sunDirChanged = cosDelta < 0.99999f;     // ~0.25 deg
    const float curSunMag    = m_sunColor.x + m_sunColor.y + m_sunColor.z;
    const bool sunIntChanged = std::abs(curSunMag - m_lastBakedSunIntensity) > 1e-3f;

    const bool needCubeUpdate = m_materialSkyEnabled || m_prefilterNeedsFullBake || sunDirChanged || sunIntChanged;

    uint64_t sourceSrv = m_materialSkyEnabled ? m_materialSkySrv : m_atmosphereSrvHandle;
    if (sourceSrv == 0) return cl;

    // ---- Fine-grained GPU profiling ----
    // Each sub-step gets its own Begin/EndTimestamp pair, so the editor's GPU Profiler
    // lists per-step costs (SkyIBL.MaterialSky / .SkyViewLUT / .AtmosphereCube /
    // .AerialPerspective / .SHProjection / .Prefilter) instead of one number.
    IGraphicsDevice& gfx = *m_gfx;
    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(cl, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(cl, r);
    };

    // ---- Procedural or material sky (only when the sky changed) ----
    if (needCubeUpdate)
    {
        if (m_materialSkyEnabled)
        {
            uint32_t r = pBegin("SkyIBL.MaterialSky");
            DispatchMaterialSky(cl);
            pEnd(r);
        }
        else
        {
        BakeStaticLUTs(cl);          // one-shot: returns at once after the first bake

        {
            uint32_t r = pBegin("SkyIBL.SkyViewLUT");
            DispatchSkyViewLUT(cl);
            pEnd(r);
        }

        {
            uint32_t r = pBegin("SkyIBL.AtmosphereCube");
            DispatchAtmosphere(cl);
            pEnd(r);
        }
        }

        m_lastBakedSunDir        = m_sunDir;
        m_lastBakedSunIntensity  = curSunMag;
        m_framesSinceCubeUpdate  = 0;
    }
    else
    {
        // BakeStaticLUTs still runs here, so the one-shot bake happens on the first
        // frame even when the sun started at identity.
        BakeStaticLUTs(cl);
        if (m_framesSinceCubeUpdate != 0xFFFFFFFFu)
            m_framesSinceCubeUpdate++;
    }

    // ---- Aerial perspective: EVERY frame while enabled ----
    // The AP LUT bakes per-voxel view rays from InvViewProj, so it depends on the
    // CAMERA, not just the sun: behind the sun-dirty gate it would freeze to an old
    // camera and smear haze the moment the camera turns. 4x4x32 groups x 6 steps is
    // tens of microseconds, cheap enough to run unconditionally.
    if (m_atmosphereEnabled && m_aerialCompositeEnabled)
    {
        uint32_t r = pBegin("SkyIBL.AerialPerspective");
        DispatchAerialPerspective(cl);
        pEnd(r);
    }

    // ---- SH projection - only when the sky cube was re-baked ----
    // SH reads the cube; stale cube -> stale SH. Skipping when nothing changed saves
    // the LDS-reduction dispatch (~0.05 ms).
    if (needCubeUpdate)
    {
        uint32_t r = pBegin("SkyIBL.SHProjection");

        if (auto* slot = m_cb.Current(gfx))
        {
            SkyIBLPass::SHConstants c{};
            c.sampleCount = kSHSampleCount;
            *slot = c;
        }

        if (m_shState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_shBuffer, m_shState,
                            RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_shState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        gfx.BindComputePipelineState(m_pso, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);
        gfx.SetComputeDescriptorTable(kSRV0, sourceSrv, cl);
        gfx.SetComputeDescriptorTable(kUAV0, m_shUavHandle, cl);
        gfx.DispatchCompute(1, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_shBuffer), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_shBuffer, RHI::ResourceState::UNORDERED_ACCESS,
                        RHI::ResourceState::SHADER_RESOURCE), cl);
        m_shState = RHI::ResourceState::SHADER_RESOURCE;
        m_shValid = true;

        pEnd(r);
    }

    // ---- Specular prefilter - temporal: runs until all 6 faces have caught up with
    // the latest cube (~6 frames after a change). After that every face reflects the
    // current cube and the ~0.4 ms prefilter dispatch is skipped.
    const bool needPrefilter = m_prefilterNeedsFullBake
                             || (m_framesSinceCubeUpdate < 6);
    if (needPrefilter)
    {
        // Label the bootstrap (all 6 faces) and the temporal (1 face/frame)
        // runs differently so the editor shows the ~6x cost spike on mode
        // toggle versus the steady-state temporal cost.
        const char* name = m_prefilterNeedsFullBake ? "SkyIBL.Prefilter(Bootstrap 6f)"
                                                    : "SkyIBL.Prefilter(1f)";
        uint32_t r = pBegin(name);
        DispatchPrefilter(cl, sourceSrv);
        pEnd(r);
    }

    return cl;
}


void SkyIBLPass::PrepareMaterialSky(const MaterialComponent* material, float time,
    const std::function<uint64_t(const std::string&)>& texture)
{
    const bool wasEnabled = m_materialSkyEnabled;
    m_materialSkyEnabled = false;
    if (!material || material->customShaderPath.empty() || !m_gfx) return;
    if (m_materialSkyShader != material->customShaderPath)
    {
        if (m_materialSkyPSO.IsValid()) m_gfx->DestroyPipelineState(m_materialSkyPSO);
        m_materialSkyPSO = {};
        m_materialSkyShader = material->customShaderPath;
        m_materialSkyValid = false;
        const uint32_t id = m_shaderLib.RegisterDynamic(m_materialSkyShader.c_str(), RHI::ShaderStage::CS, "main");
        const auto* cs = m_shaderLib.GetDynamicShader(id);
        const auto* reflection = m_shaderLib.GetDynamicReflection(id);
        if (!cs || !reflection) return;
        m_materialSkyReflection = *reflection;
        RHI::PipelineStateDesc desc{}; desc.cs = cs;
        if (!m_gfx->CreatePipelineState(desc, m_materialSkyPSO)) return;
        LOG_INFO("SkyIBL: dynamic custom sky material %s", m_materialSkyShader.c_str());
    }
    if (!m_materialSkyPSO.IsValid()) return;
    if (!m_materialSkyTex.IsValid())
    {
        RHI::TextureDesc td{};
        td.width = td.height = kMaterialSkySize; td.arraySize = 6; td.mipLevels = 1;
        td.format = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = "SkyIBL.MaterialSky";
        if (!m_gfx->CreateTexture(td, m_materialSkyTex)) return;
        m_materialSkySrv = m_gfx->GetTextureSRVGpuHandle(m_materialSkyTex);
    }
    // Upload(), not Current(): this runs on the GAME side (SyncSkyboxIBL, inside
    // BuildRenderScene), before BeginFrame publishes the render slot. Current() there
    // is the PREVIOUS frame's slot, which the GPU may still be reading, and
    // DispatchMaterialSky's CurrentBuffer() would then read a slot written for another
    // frame: the sky time - and with it the cube, its SH and the prefiltered faces -
    // would depend on CPU/GPU lag.
    auto* cb = m_materialSkyCB.Upload(*m_gfx);
    if (!cb) return;
    *cb = {};
    for (const auto& layout : m_materialSkyReflection.cbuffers)
    {
        if (layout.bindPoint != 0 || layout.space != 2 || layout.sizeBytes > sizeof(cb->bytes)) return;
        for (const auto& v : layout.vars)
        {
            if (v.offset + v.size > sizeof(cb->bytes) || v.size > 16) return;
            auto it = material->customParams.find(v.name);
            if (it != material->customParams.end()) std::memcpy(cb->bytes + v.offset, it->second.data(), v.size);
            else if (v.hasDefault) std::memcpy(cb->bytes + v.offset, v.defaultValue.data(), v.size);
            if (v.name == "SkyTimeSize")
            {
                const float values[4] = {time, float(kMaterialSkySize), 0, 0};
                std::memcpy(cb->bytes + v.offset, values, sizeof(values));
            }
        }
    }
    m_materialSkySRV.fill(0);
    for (const auto& b : m_materialSkyReflection.bindings)
    {
        if (b.type != ShaderReflect::ResourceType::Texture) continue;
        if (b.space != 2 || b.bindPoint >= m_materialSkySRV.size()) return;
        auto it = material->customTextures.find(b.name);
        if (it == material->customTextures.end()) return;
        const uint64_t srv = texture(it->second.name);
        if (!srv) return;
        m_materialSkySRV[b.bindPoint] = srv;
    }
    m_materialSkyEnabled = true;
    if (!wasEnabled) m_prefilterNeedsFullBake = true;
}

void SkyIBLPass::DispatchMaterialSky(RHI::CommandList cl)
{
    auto& gfx = *m_gfx;
    if (m_materialSkyState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_materialSkyTex, m_materialSkyState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
    gfx.BindComputePipelineState(m_materialSkyPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_materialSkyCB.CurrentBuffer(gfx), cl);
    const RHI::Slots::CmpParam slots[] = {RHI::Slots::Cmp::SRV_t0, RHI::Slots::Cmp::SRV_t1,
                             RHI::Slots::Cmp::SRV_t2, RHI::Slots::Cmp::SRV_t3,
                             RHI::Slots::Cmp::SRV_t4, RHI::Slots::Cmp::SRV_t5,
                             RHI::Slots::Cmp::SRV_t6, RHI::Slots::Cmp::SRV_t7};
    for (size_t i = 0; i < m_materialSkySRV.size(); ++i)
        if (m_materialSkySRV[i]) gfx.SetComputeDescriptorTable(slots[i], m_materialSkySRV[i], cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(m_materialSkyTex), cl);
    gfx.DispatchCompute((kMaterialSkySize + 7) / 8, (kMaterialSkySize + 7) / 8, 6, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_materialSkyTex, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_materialSkyState = RHI::ResourceState::SHADER_RESOURCE;
    m_materialSkyValid = true;
}
