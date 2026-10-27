#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/PSOCache.h"
#include "ECS/ParticleComponent.h"   // ParticleBlendMode
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

// Root-parameter indices come from Graphics/RootSignatureLayout.h; the local names below are
// aliases that keep this file's vocabulary, never new numbers.
namespace S    = RHI::Slots::Gfx;      // ABSOLUTE graphics root indices
namespace SRel = RHI::Slots::GfxRel;   // GROUP-RELATIVE (BindConstantBuffer)
namespace C    = RHI::Slots::Cmp;      // ABSOLUTE compute root indices

// Compute root signature slots (RHI::Slots::Cmp), shared with SkinningPass (SkinningPass.h
// has the full table).
static constexpr auto kCSCBSlot        = C::CB;      // b0 space2 (ROOT_CBV)
static constexpr auto kCSPoolUAVSlot   = C::UAV_u0;  // DESC_TABLE u0 space2
static constexpr auto kCSSnapUAVSlot    = C::UAV_u1; // DESC_TABLE u1 space2 (boid snapshot: snapshot writes, steer reads)
static constexpr auto kCSGridCountUAVSlot = C::UAV_u2; // DESC_TABLE u2 space2 (boid grid cell counts)
static constexpr auto kCSGridItemsUAVSlot = C::UAV_u3; // DESC_TABLE u3 space2 (boid grid cell item lists)
static constexpr auto kCSMeshDescSlot  = C::MeshDesc;        // DESC_TABLE t1 space0 (MeshDescriptors SRV)
static constexpr auto kCSBindlessSlot  = C::BindlessBuffers; // DESC_TABLE t0 space1 (bindless g_Buffers[])

// Graphics root signature slots (see RHI::Slots::Gfx).
static constexpr auto kGfxPoolSRVSlot    = S::SRV_t4;     // DESC_TABLE t4 space0
static constexpr auto kGfxBindlessTexSlot= S::BindlessTex;// DESC_TABLE t0 space2, bindless texture array

// Stencil bit stamped on TAA-excluded particle pixels; TAA reads it
// (TAAPass::kParticleVfxBitMask, the same ShadingStencil bit) to floor the history blend and
// keep VFX sharp. It sits clear of the shading codes (0x00..0x3F) and OutlinePass's 0x80.
static constexpr uint32_t kParticleTAAExcludeBit = ShadingStencil::kTaaExclude;   // 0x40

// Forward-lighting root slots for LIT buckets (the shared graphics root signature, as
// TransparentPass). The CB entries are group-relative (SRel::) because they go through
// BindCB / BindConstantBuffer; the tables are absolute (S::) because they go through
// BindDescriptorTableHandle. particle_surface.hlsli names fixed sampler-heap slots, so none
// is bound: SMP_LINEAR_WRAP for the texture and the IBL, SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE
// for the CSM (GREATER_EQUAL for reverse-Z; LESS_EQUAL would shadow wherever the light
// reaches), SMP_LINEAR_CLAMP for the fog.
static constexpr auto kGfxLightCBSlot     = SRel::CB_b2;      // ROOT_CBV b2 space0 (BindCB)
static constexpr auto kGfxRenderCBb3Slot  = SRel::CB_b3;      // ROOT_CBV b3 space0 (ParticleRenderCB)
static constexpr auto kIBLIrradianceSlot  = S::IBLIrradiance; // DESC_TABLE t6 space0
static constexpr auto kSkySHSRVSlot       = S::SkySH;         // DESC_TABLE t19 space0

// ==== ParticleSimPass ====

void ParticleSimPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::ParticleEmit_CS,   RHI::ShaderStage::CS, "ParticleEmit.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::ParticleUpdate_CS, RHI::ShaderStage::CS, "ParticleUpdate.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::ParticleBoidSnapshot_CS, RHI::ShaderStage::CS, "ParticleBoidSnapshot.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::ParticleBoidGridClear_CS, RHI::ShaderStage::CS, "ParticleBoidGridClear.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::ParticleBoidGridBuild_CS, RHI::ShaderStage::CS, "ParticleBoidGridBuild.cs.hlsl", "CSMain");
    m_shaderLib.Register(ShaderID::ParticleBoidSteer_CS,    RHI::ShaderStage::CS, "ParticleBoidSteer.cs.hlsl",    "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("ParticleSimPass: initialised");
}

// Onto the live members: a failed rebuild keeps the old pipeline.
bool ParticleSimPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* emitCS   = m_shaderLib.GetShader(ShaderID::ParticleEmit_CS);
    const RHI::Shader* updateCS = m_shaderLib.GetShader(ShaderID::ParticleUpdate_CS);
    if (!emitCS || !updateCS)
    {
        LOG_ERROR("ParticleSimPass: compute shader lookup failed");
        return false;
    }

    {
        RHI::PipelineStateDesc d{};
        d.cs = emitCS;
        if (!gfx.CreatePipelineState(d, m_emitPSO))
            LOG_ERROR("ParticleSimPass: emit PSO failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = updateCS;
        if (!gfx.CreatePipelineState(d, m_updatePSO))
            LOG_ERROR("ParticleSimPass: update PSO failed");
    }

    // The boid PSOs (optional, flocking scenes only). A failed compile leaves a PSO invalid and
    // Execute skips the flock work.
    if (const RHI::Shader* snapCS = m_shaderLib.GetShader(ShaderID::ParticleBoidSnapshot_CS))
    {
        RHI::PipelineStateDesc d{};
        d.cs = snapCS;
        if (!gfx.CreatePipelineState(d, m_boidSnapshotPSO))
            LOG_ERROR("ParticleSimPass: boid snapshot PSO failed");
    }
    if (const RHI::Shader* clearCS = m_shaderLib.GetShader(ShaderID::ParticleBoidGridClear_CS))
    {
        RHI::PipelineStateDesc d{};
        d.cs = clearCS;
        if (!gfx.CreatePipelineState(d, m_boidGridClearPSO))
            LOG_ERROR("ParticleSimPass: boid grid-clear PSO failed");
    }
    if (const RHI::Shader* buildCS = m_shaderLib.GetShader(ShaderID::ParticleBoidGridBuild_CS))
    {
        RHI::PipelineStateDesc d{};
        d.cs = buildCS;
        if (!gfx.CreatePipelineState(d, m_boidGridBuildPSO))
            LOG_ERROR("ParticleSimPass: boid grid-build PSO failed");
    }
    if (const RHI::Shader* steerCS = m_shaderLib.GetShader(ShaderID::ParticleBoidSteer_CS))
    {
        RHI::PipelineStateDesc d{};
        d.cs = steerCS;
        if (!gfx.CreatePipelineState(d, m_boidSteerPSO))
            LOG_ERROR("ParticleSimPass: boid steer PSO failed");
    }
    return true;
}

void ParticleSimPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void ParticleSimPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_emitPSO.IsValid() || !m_updatePSO.IsValid()) return;
    // The pool is lazily resident (ParticleSystem::EnsurePoolResources): a world with no emitter
    // has no pool, and on the frame a release lands the buffer is already gone. Do not drop this
    // early-out: every access below would bind or barrier a destroyed resource.
    if (!m_sys->GetParticlePool().IsValid()) return;
    auto& gfx = *m_gfx;

    // ---- Emit: one dispatch per emitter ----
    const uint32_t emitterCount = m_sys->GetEmitterCount();
    if (emitterCount > 0)
    {
        gfx.BindComputePipelineState(m_emitPSO, cl);
        gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAVHandle(), cl);

        // Mesh-shape sampling bindings; the shader reads them only for shapeType=MESH.
        if (m_meshDescBuffer && m_meshDescBuffer->IsValid())
        {
            const uint64_t meshDescSrv = m_gfx->GetBufferSRVGpuHandle(*m_meshDescBuffer);
            gfx.SetComputeDescriptorTable(kCSMeshDescSlot, meshDescSrv, cl);
        }
        if (m_bindlessTableHandle)
            gfx.SetComputeDescriptorTable(kCSBindlessSlot, m_bindlessTableHandle, cl);

        for (uint32_t i = 0; i < emitterCount; ++i)
        {
            const uint32_t spawnCount = m_sys->GetEmitterSpawnCount(i);
            if (spawnCount == 0) continue;

            // Per-emitter 256B-aligned CBV offset.
            const uint32_t cbOffset = static_cast<uint32_t>(
                i * ParticleSystem::kEmitterSlotStride);
            gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetEmitterBuffer(), cbOffset, cl);

            gfx.DispatchCompute((spawnCount + 63) / 64, 1, 1, cl);
        }

        // UAV barrier: emit writes must land before update reads the same slots.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetParticlePool()), cl);
    }

    // ---- Boids (flocking scenes only) ----
    // Snapshot a stable {pos,vel,valid} view of the pool, build a uniform grid over the flock
    // (clear the counters -> atomic scatter), then steer reading neighbours from the grid (no
    // read-while-write race). UAV-only, with UAV memory barriers; flock-free scenes pay nothing.
    // Agents only occupy [GetBoidSimulatedSlotBase(), GetBoidSimulatedSlotCount()) and the
    // kernels derive their slot as BoidParams::dispatchBase + thread id, so dispatching that span
    // covers every agent and nothing else. Both ends are monotonic, so the span never stops
    // covering a slot an agent still sits in - the steer shrink-kill relies on it.
    const uint32_t flockBase = m_sys->GetBoidSimulatedSlotBase();
    const uint32_t flockEnd  = m_sys->GetBoidSimulatedSlotCount();
    const uint32_t flockSpan = (flockEnd > flockBase) ? (flockEnd - flockBase) : 0u;
    const uint32_t flockGroups = (flockSpan + 63) / 64;
    // The boid working set is lazily resident too, independently of the pool (a scene can have
    // emitters but no flock): check the snapshot buffer rather than assume IsFlockActive()
    // implies it.
    if (m_sys->IsFlockActive() && flockGroups > 0
        && m_sys->GetBoidSnapshot().IsValid()
        && m_boidSnapshotPSO.IsValid() && m_boidGridClearPSO.IsValid()
        && m_boidGridBuildPSO.IsValid() && m_boidSteerPSO.IsValid())
    {
        const uint32_t cellGroups = (ParticleSystem::GetBoidGridCells() + 63) / 64;

        const uint64_t poolUav      = m_sys->GetPoolUAVHandle();
        const uint64_t snapUav      = m_sys->GetBoidSnapshotUAVHandle();
        const uint64_t cellCountUav = m_sys->GetBoidCellCountUAVHandle();
        const uint64_t cellItemsUav = m_sys->GetBoidCellItemsUAVHandle();

        // 1) Snapshot: read gPool (u0) -> write gSnap (u1).
        gfx.BindComputePipelineState(m_boidSnapshotPSO, cl);
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetBoidParamsCB(), 0, cl);
        gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, poolUav, cl);
        gfx.SetComputeDescriptorTable(kCSSnapUAVSlot, snapUav, cl);
        gfx.DispatchCompute(flockGroups, 1, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetBoidSnapshot()), cl);

        // 2) Grid clear: zero per-cell counters (u2).
        gfx.BindComputePipelineState(m_boidGridClearPSO, cl);
        gfx.SetComputeDescriptorTable(kCSGridCountUAVSlot, cellCountUav, cl);
        gfx.DispatchCompute(cellGroups, 1, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetBoidCellCount()), cl);

        // 3) Grid build: scatter agents into cells (read gSnap u1, atomic u2, write u3).
        gfx.BindComputePipelineState(m_boidGridBuildPSO, cl);
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetBoidParamsCB(), 0, cl);
        gfx.SetComputeDescriptorTable(kCSSnapUAVSlot, snapUav, cl);
        gfx.SetComputeDescriptorTable(kCSGridCountUAVSlot, cellCountUav, cl);
        gfx.SetComputeDescriptorTable(kCSGridItemsUAVSlot, cellItemsUav, cl);
        gfx.DispatchCompute(flockGroups, 1, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetBoidCellCount()), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetBoidCellItems()), cl);

        // 4) Steer: read gSnap (u1) + grid (u2/u3), read/write gPool (u0).
        gfx.BindComputePipelineState(m_boidSteerPSO, cl);
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetBoidParamsCB(), 0, cl);
        gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, poolUav, cl);
        gfx.SetComputeDescriptorTable(kCSSnapUAVSlot, snapUav, cl);
        gfx.SetComputeDescriptorTable(kCSGridCountUAVSlot, cellCountUav, cl);
        gfx.SetComputeDescriptorTable(kCSGridItemsUAVSlot, cellItemsUav, cl);
        gfx.DispatchCompute(flockGroups, 1, 1, cl);

        // Steer writes must land before the update pass reads the pool.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetParticlePool()), cl);
    }

    // ---- Update: one dispatch over the live pool range ----
    // Bounded by the slots that can hold a particle, not the whole 1M-slot pool: the kernel maps
    // thread id 1:1 to a pool slot, and a dead slot above the bound cannot come alive without a
    // spawn, which raises the bound first (CollectEmitters runs before this pass).
    // gParams.particleCount stays the full capacity: the ambient path divides it to derive the
    // slice width.
    gfx.BindComputePipelineState(m_updatePSO, cl);
    gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(), 0, cl);
    gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAVHandle(), cl);

    const uint32_t simSlots = m_sys->GetSimulatedSlotCount();
    if (simSlots > 0)
        gfx.DispatchCompute((simSlots + 63) / 64, 1, 1, cl);

    // Final UAV barrier so the subsequent render pass sees a consistent pool.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetParticlePool()), cl);
}

// ==== ParticleRenderPass ====

ParticleRenderPass::ParticleRenderPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void ParticleRenderPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth-test against GBuffer depth, no depth write: the depth plane read, the stencil plane
    // written (the TAA-exclude variant stamps its TaaExclude bit, RenderGraph/StencilBits.h).
    // Under -rgplaneoff the whole depth-stencil is declared written.
    if (RG::RenderGraph::PlanesEnabled())
        b.DepthStencil(m_depth, RG::Depth::Read, RG::Stencil::Write, ShadingStencil::kTaaExclude);
    else                                  b.WriteDepthStencil(m_depth);
    // The soft fade reads the opaque depth snapshot through a heap index (FogApplyCB); the
    // declaration is the contract.
    if (m_opaqueDepth.IsValid()) b.ReadSRV(m_opaqueDepth);
    // The HDR scene colour: declared so the graph brings it to RENDER_TARGET (it never binds or
    // clears it); Execute binds it, as TransparentPass does.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // What particle_surface.hlsli samples (bound by handle; their owners barrier them): the CSM
    // and the per-object map for lit buckets that receive shadows, and the froxel fog
    // (FogApply).
    b.ReadPassTextures({ RG::PassTex::kShadowCSM, RG::PassTex::kShadowPerObject, RG::PassTex::kVolFogScattering });
}

void ParticleRenderPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Particle_VS, RHI::ShaderStage::VS, "Particle.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::Particle_PS, RHI::ShaderStage::PS, "Particle.ps.hlsl", "main");
    m_psoCache.Init(gfx, m_shaderLib);

    // Warm the default PSO (engine VS/PS, additive blend). Per-emitter custom
    // shaders + blend modes add further cached permutations at draw time.
    if (!m_psoCache.GetOrCreate(BuildParticlePSODesc(
            0, 0, static_cast<uint32_t>(ParticleBlendMode::Additive),
            /*taaExclude=*/false, /*lit=*/false, /*receiveShadow=*/false)))
    {
        LOG_ERROR("ParticleRenderPass: default PSO creation failed");
        return;
    }

    // Per-frame render CB (root CBV b3 space0).
    if (!m_renderCB.Create(gfx, "ParticleRenderPass.RenderCB"))
        LOG_ERROR("ParticleRenderPass: render CB create failed");

    // 1x1 fully-lit CSM stand-in (see m_shadowFallbackTex): a missing shadow map reads as "no
    // shadow", not garbage.
    {
        RHI::TextureDesc td;
        td.format     = RHI::Format::R32_FLOAT;
        td.width      = 1;
        td.height     = 1;
        td.arraySize = 4;   // Texture2DArray SRV needs > 1
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        // arraySize 4 and texel 0.0 are both load-bearing (as in LightingPass::Init's fallbacks):
        // CreateTexture emits a TEXTURE2DARRAY SRV only when arraySize > 1, and a TEXTURE2D SRV in
        // the shader's Texture2DArray slot is a Tier-3 type mismatch with no validation message.
        // Under the reversed-Z GREATER_EQUAL comparison sampler fully lit is depth 0.0 (the shadow
        // clear, no occluder); 1.0 would read as an occluder at the near plane, fully shadowed.
        const float lit = 0.0f;   // reversed-Z: 0 = no occluder = fully lit
        // One entry per subresource: CreateTexture uploads mipLevels x arraySize subresources and
        // indexes this array; a single element with arraySize > 1 walks off the stack.
        RHI::SubresourceData init[4];
        for (auto& sd : init) sd = { &lit, sizeof(float), sizeof(float) };
        if (gfx.CreateTexture(td, m_shadowFallbackTex, init))
            m_shadowFallbackHandle = gfx.GetTextureSRVGpuHandle(m_shadowFallbackTex);
        else
            LOG_ERROR("ParticleRenderPass: CSM fallback texture failed");
    }

    LOG_SUCCESS("ParticleRenderPass: initialised");
}

// Triangle strip, no input layout; blends as TransparentPass does.
PSODesc ParticleRenderPass::BuildParticlePSODesc(uint32_t customVSID,
                                                 uint32_t customPSID,
                                                 uint32_t blendMode,
                                                 bool     taaExclude,
                                                 bool     lit,
                                                 bool     receiveShadow) const
{
    PSODesc desc;
    desc.vsID = (customVSID != 0) ? static_cast<ShaderID>(customVSID)
                                  : ShaderID::Particle_VS;
    desc.psID = (customPSID != 0) ? static_cast<ShaderID>(customPSID)
                                  : ShaderID::Particle_PS;
    desc.inputLayout = InputLayoutType::None;   // procedural billboard (SV_VertexID)
    // PARTICLE_LIT -> the PS compiles its forward-lit branch; the VS ignores the define. A
    // distinct PSO through the hashed permutation.
    desc.perm.Set(PermutationKey::PARTICLE_LIT, lit);
    // Sun shadow is a second opt-in, meaningful only on a lit bucket (the unlit path is an
    // emissive composite with no direct term): `lit &&` keeps receiveShadow without lit from
    // minting a pointless extra PSO.
    desc.perm.Set(PermutationKey::PARTICLE_SHADOW, lit && receiveShadow);
    desc.perm.Set(PermutationKey::DEBUG_SHADE, m_debugShade);

    desc.rs.fillMode         = RHI::FillMode::SOLID;
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = true;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;  // reversed-Z

    // TAA-exclude: stamp kParticleTAAExcludeBit on every depth-passing particle pixel (no depth
    // write; only the stencil bit, like OutlinePass), so TAA keeps them sharp. Cull NONE -> both
    // faces. The ref is set per draw (SetStencilRef).
    if (taaExclude)
    {
        desc.dss.stencilEnable     = true;
        desc.dss.stencilReadMask  = 0xFF;
        desc.dss.stencilWriteMask = kParticleTAAExcludeBit;
        desc.dss.frontFace.stencilFunc          = RHI::ComparisonFunc::ALWAYS;
        desc.dss.frontFace.stencilPassOp       = RHI::StencilOp::REPLACE;
        desc.dss.frontFace.stencilFailOp       = RHI::StencilOp::KEEP;
        desc.dss.frontFace.stencilDepthFailOp = RHI::StencilOp::KEEP;
        desc.dss.backFace = desc.dss.frontFace;
    }
    else
    {
        desc.dss.stencilEnable   = false;
    }

    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable             = true;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    switch (static_cast<ParticleBlendMode>(blendMode))
    {
    case ParticleBlendMode::Alpha:
        rt.srcBlend        = RHI::Blend::SRC_ALPHA;
        rt.destBlend       = RHI::Blend::INV_SRC_ALPHA;
        rt.srcBlendAlpha  = RHI::Blend::ONE;
        rt.destBlendAlpha = RHI::Blend::INV_SRC_ALPHA;
        break;
    case ParticleBlendMode::Premultiplied:
        rt.srcBlend        = RHI::Blend::ONE;
        rt.destBlend       = RHI::Blend::INV_SRC_ALPHA;
        rt.srcBlendAlpha  = RHI::Blend::ONE;
        rt.destBlendAlpha = RHI::Blend::INV_SRC_ALPHA;
        break;
    case ParticleBlendMode::Multiply:
        rt.srcBlend        = RHI::Blend::DEST_COLOR;
        rt.destBlend       = RHI::Blend::INV_SRC_ALPHA;
        rt.srcBlendAlpha  = RHI::Blend::ONE;
        rt.destBlendAlpha = RHI::Blend::INV_SRC_ALPHA;
        break;
    case ParticleBlendMode::Additive:
    default:
        // Additive, src multiplied by alpha so fading particles dim.
        rt.srcBlend        = RHI::Blend::SRC_ALPHA;
        rt.destBlend       = RHI::Blend::ONE;
        rt.srcBlendAlpha  = RHI::Blend::ONE;
        rt.destBlendAlpha = RHI::Blend::ONE;
        break;
    }
    rt.blendOp       = RHI::BlendOp::ADD;
    rt.blendOpAlpha = RHI::BlendOp::ADD;

    desc.topology      = RHI::PrimitiveTopology::TRIANGLESTRIP;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    desc.sampleCount   = 1;
    return desc;
}

bool ParticleRenderPass::IsActive() const
{
    return !RG::PassActivityGatesEnabled()
        || (m_sys != nullptr && m_renderCB.IsValid()
            && m_sys->GetParticlePool().IsValid());
}

RHI::CommandList ParticleRenderPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_renderCB.IsValid()) return cl;
    // Lazily resident pool (see ParticleSimPass::Execute): no pool, no slice to draw from. Also
    // the release frame, where ReleaseMainPool already zeroed every bucket's drawCount.
    if (!m_sys->GetParticlePool().IsValid()) return cl;

    auto& gfx = *m_gfx;

    // Pack the camera data into the CB once (shared by every bucket; a bucket's slice offset
    // travels in the b0 bucketBase root constant). Transposed for the HLSL row-vector convention.
    using namespace DirectX;
    if (auto* slot = m_renderCB.Current(gfx))
    {
        RenderCB cb{};
        XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                        XMMatrixTranspose(vp));
        cb.camRight[0]  = m_camRight.x;
        cb.camRight[1]  = m_camRight.y;
        cb.camRight[2]  = m_camRight.z;
        cb.camUp[0]     = m_camUp.x;
        cb.camUp[1]     = m_camUp.y;
        cb.camUp[2]     = m_camUp.z;
        cb.particleSize = m_particleSize;
        cb.gTime        = m_time;
        *slot = cb;
    }

    // Bind the HDR RTV with GBuffer depth (read-only depth test). RTV and viewport are OM / RS
    // state: set once, they survive SetPipelineState across buckets.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));   // the DSV the declared planes need
    cl.SetViewport();
    cl.SetScissorRect();

    const RHI::GPUBuffer&      renderCBBuf = m_renderCB.CurrentBuffer(gfx);
    const uint64_t             poolSrv     = m_sys->GetPoolSRVHandle();
    const uint64_t             texTable    = gfx.GetBindlessTextureTableGpuHandle();

    // One draw per used bucket, each over its own contiguous pool slice with its own (customVS,
    // customPS, blend) PSO. SetPipelineState rebinds the shared root signature, which resets the
    // root arguments, so the CBVs, tables and the bucketBase constant are rebound in the loop.
    const ParticleSystem::ParticleBucket* buckets = m_sys->GetBuckets();
    const uint32_t bucketCount = m_sys->GetBucketCount();
    const uint32_t sliceLen    = ParticleSystem::GetParticlesPerBucket();

    for (uint32_t b = 0; b < bucketCount; ++b)
    {
        const auto& bk = buckets[b];
        if (!bk.used) continue;   // not referenced by any enabled emitter this frame
        // Draw only the bucket's active window [bucketBase, bucketBase + drawCount), not the whole
        // slice: draw cost follows the live count, not the slice capacity.
        const uint32_t drawCount = (bk.drawCount < sliceLen) ? bk.drawCount : sliceLen;
        if (drawCount == 0) continue;   // used but never spawned -> no live particles
        // Material-driven bucket whose b8 is not packed yet (shader still compiling, ring full):
        // drawing would read an unbound b8.
        if (!bk.paramReady) continue;

        // Custom shaders -> ids in this pass's library, re-registered only when the producer ids
        // change (BucketDynIds).
        static_assert(sizeof(m_bucketDynIds) / sizeof(m_bucketDynIds[0])
                          == ParticleSystem::kMaxParticleBuckets,
                      "pass-side cache must cover every bucket");
        BucketDynIds& ids = m_bucketDynIds[b];
        if (ids.producerVs != bk.dynVsId || ids.producerPs != bk.dynPsId)
        {
            ids.producerVs = bk.dynVsId;
            ids.producerPs = bk.dynPsId;
            ids.vsID = ids.psID = 0;
            if (!bk.customVertexPath.empty())
            {
                const uint32_t id = m_shaderLib.RegisterDynamic(
                    bk.customVertexPath.c_str(), RHI::ShaderStage::VS, "main");
                if (id != ShaderLibrary::kInvalidDynShaderID) ids.vsID = id;
            }
            if (!bk.customSurfacePath.empty())
            {
                const uint32_t id = m_shaderLib.RegisterDynamic(
                    bk.customSurfacePath.c_str(), RHI::ShaderStage::PS, "main");
                if (id != ShaderLibrary::kInvalidDynShaderID) ids.psID = id;
            }
        }
        const uint32_t vsID = ids.vsID, psID = ids.psID;

        const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
            BuildParticlePSODesc(vsID, psID, bk.blendMode, bk.taaExclude != 0,
                                 bk.lit != 0, bk.receiveShadow != 0));
        if (!pso || !pso->IsValid()) continue;   // broken custom shader -> skip this bucket only

        cl.SetPipelineState(*pso);
        // PrimitiveTopology is dynamic per-draw state in D3D12: set it each draw.
        cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLESTRIP);
        // TAA-exclude buckets stamp the stencil bit (PSO writes it, ref carries
        // the value). SetStencilRef is dynamic OM state, set per excluded bucket.
        if (bk.taaExclude)
            gfx.SetStencilRef(kParticleTAAExcludeBit, cl);

        // Root arguments again (SetPipelineState reset them). b0 word 0 = this bucket's first pool
        // slot (the VS rebases gPool), word 1 its ParticleBlendMode: the PS applies the soft fade
        // and the fog per blend state (ParticleBucketCB).
        gfx.SetGraphicsRootConstant(S::RootConstants, bk.bucketBase, /*offsetWords=*/0, cl);
        gfx.SetGraphicsRootConstant(S::RootConstants, bk.blendMode,  /*offsetWords=*/1, cl);
        // b4: the fog the opaque scene got, and the soft-fade depth.
        FogApply::Bind(cl);
        gfx.BindConstantBuffer(kGfxRenderCBb3Slot, renderCBBuf, cl);    // b3 camera CB (b2 is LightCB)
        cl.BindDescriptorTableHandle(kGfxPoolSRVSlot, poolSrv);         // t4 pool SRV
        if (texTable)
            cl.BindDescriptorTableHandle(kGfxBindlessTexSlot, texTable);      // t0 space2 bindless
        // Material-driven b8 params (root CBV Gfx::CustomMatCBV, the material system's slot). 0 =
        // nothing to bind.
        if (bk.paramCbvVA)
            gfx.BindCustomMaterialCBV(bk.paramCbvVA, cl);

        // LIT buckets: forward sun + ambient IBL. LightCB is a named global CB (b2); irradiance (t6)
        // and sky SH (t19) come from the Renderer (TransparentPass's handles). Unlit buckets bind
        // none of it.
        if (bk.lit)
        {
            cl.BindCB(kGfxLightCBSlot, RG::RGSlot::LightCB);                          // b2
            if (m_iblIrradianceHandle)
                cl.BindDescriptorTableHandle(kIBLIrradianceSlot, m_iblIrradianceHandle);  // t6
            if (m_skySHHandle)
                cl.BindDescriptorTableHandle(kSkySHSRVSlot, m_skySHHandle);       // t19

            // Sun shadow only for buckets that also opted in: a lit bucket without receiveShadow never
            // declares t9, and one with it declares t9 unconditionally, so the bind must not be skipped.
            // Gfx::ShadowSRV is already in the shared graphics signature.
            if (bk.receiveShadow)
            {
                // Falls back rather than skipping: this bucket's PS declares t9.
                const uint64_t shadowSrv = m_shadowSrvHandle ? m_shadowSrvHandle
                                                             : m_shadowFallbackHandle;
                if (shadowSrv) cl.BindDescriptorTableHandle(S::ShadowSRV, shadowSrv);  // t9
            }
        }

        // Draw the active window; dead slots within it self-discard in the VS.
        gfx.DrawInstanced(4, drawCount, 0, 0, cl);
    }

    return cl;
}
