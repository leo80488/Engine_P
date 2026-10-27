#include "Graphics/SSR/SSRDepthHierarchyPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    // Absolute compute root indices: this pass binds the SHARED compute root signature
    // (space2, same slots as HiZPass), so every value comes from RHI::Slots::Cmp. Do not
    // take them from Gfx::.
    namespace S = RHI::Slots::Cmp;
    constexpr auto kCBSlot    = S::CB;      // b0 space2
    constexpr auto kSRV0      = S::SRV_t0;  // t0 space2: depth (mip 0 path)
    constexpr auto kUAV0      = S::UAV_u0;  // u0 space2: dst mip 0 / src mip N-1
    constexpr auto kUAV1      = S::UAV_u1;  // u1 space2: dst mip N
    constexpr auto kUAV2      = S::UAV_u2;  // u2 space2: dst mip N+1
}

void SSRDepthHierarchyPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRDepthHierarchyMip0_CS, RHI::ShaderStage::CS,
                         "SSRDepthHierarchy.cs.hlsl", "CSMain_Mip0");
    m_shaderLib.Register(ShaderID::SSRDepthHierarchyReduce_CS, RHI::ShaderStage::CS,
                         "SSRDepthHierarchy.cs.hlsl", "CSMain_Reduce");

    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRDepthHierarchyMip0_CS);
        if (!cs) { LOG_ERROR("SSRDepthHierarchyPass: mip0 CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_mip0PSO))
        { LOG_ERROR("SSRDepthHierarchyPass: mip0 PSO creation failed"); return; }
    }
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRDepthHierarchyReduce_CS);
        if (!cs) { LOG_ERROR("SSRDepthHierarchyPass: reduce CS not found"); return; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_reducePSO))
        { LOG_ERROR("SSRDepthHierarchyPass: reduce PSO creation failed"); return; }
    }

    // One 256-byte CB slot per dispatch. Reusing a single slot across the dispatches
    // recorded into one CL makes every reduce read the LAST iteration's dims, leaving every
    // middle Hi-Z mip at its 0-init state.
    m_cb.Create(gfx, "SSRDepthHierarchy.CB");

    LOG_SUCCESS("SSRDepthHierarchyPass: initialized");
}

void SSRDepthHierarchyPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    auto rebuild = [&](ShaderID id, RHI::PipelineState& pso, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SSRDepthHierarchyPass::ReloadShaders: %s missing", tag); return; }
        RHI::PipelineStateDesc d{}; d.cs = cs;
        if (!gfx.CreatePipelineState(d, pso))
            LOG_ERROR("SSRDepthHierarchyPass::ReloadShaders: %s PSO rebuild failed", tag);
    };
    rebuild(ShaderID::SSRDepthHierarchyMip0_CS,   m_mip0PSO,   "mip0");
    rebuild(ShaderID::SSRDepthHierarchyReduce_CS, m_reducePSO, "reduce");
}

void SSRDepthHierarchyPass::ReleaseTextures()
{
    if (!m_gfx) return;
    if (m_texture.IsValid()) m_gfx->DestroyTexture(m_texture);
    m_texture = {};
    m_width = m_height = 0;
}

void SSRDepthHierarchyPass::Shutdown()
{
    if (!m_gfx) return;
    ReleaseTextures();
    m_gfx->DestroyPipelineState(m_mip0PSO);
    m_gfx->DestroyPipelineState(m_reducePSO);
    m_cb.Destroy(*m_gfx);
}

void SSRDepthHierarchyPass::EnsureTexture(uint32_t depthW, uint32_t depthH)
{
    if (!m_gfx || depthW == 0 || depthH == 0) return;

    if (depthW == m_width && depthH == m_height && m_texture.IsValid()) return;

    if (m_texture.IsValid())
        m_gfx->DestroyTexture(m_texture);

    m_width  = depthW;
    m_height = depthH;

    m_mipCount = 1;
    { uint32_t dim = std::max(depthW, depthH); while (dim > 1) { dim >>= 1; m_mipCount++; } }

    RHI::TextureDesc td{};
    td.width      = depthW;
    td.height     = depthH;
    td.mipLevels = m_mipCount;
    td.format     = RHI::Format::R16G16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;

    if (!m_gfx->CreateTexture(td, m_texture))
        LOG_ERROR("SSRDepthHierarchyPass: texture creation failed (%ux%u, %u mips)",
                  depthW, depthH, m_mipCount);
    else
        LOG_INFO("SSRDepthHierarchyPass: created pyramid %ux%u (%u mips)",
                 depthW, depthH, m_mipCount);
}

uint64_t SSRDepthHierarchyPass::GetSrvHandle() const
{
    if (!m_gfx || !m_texture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_texture);
}

void SSRDepthHierarchyPass::Execute(RHI::CommandList cl, uint64_t depthSrvHandle)
{
    if (!m_mip0PSO.IsValid() || !m_reducePSO.IsValid()) return;
    if (!m_texture.IsValid() || m_mipCount == 0 || !m_cb.IsValid()) return;
    if (!depthSrvHandle) return;

    auto& gfx = *m_gfx;

    auto* mapped = m_cb.Current(gfx);
    if (!mapped) return;
    uint8_t* cbBase = mapped->bytes;
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // ---- Mip 0: depth SRV -> pyramid mip 0 ----
    {
        const uint32_t w = m_width;
        const uint32_t h = m_height;

        gfx.BindComputePipelineState(m_mip0PSO, cl);

        // Mip 0 uses CB slot 0.
        HierCB cb{ w, h, w, h, 0 };
        std::memcpy(cbBase, &cb, sizeof(cb));
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, 0, cl);
        gfx.SetComputeDescriptorTable(kSRV0, depthSrvHandle, cl);
        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_texture, 0), cl);

        gfx.DispatchCompute((w + 7) / 8, (h + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);
    }

    // ---- Mips 1..N-1: UAV -> UAV reduce ----
    // Two mips per dispatch: mip N reads mip N-1, a true read-after-write dependency no
    // barrier trick removes. The shader reduces its 8x8 tile a second time through
    // groupshared (depth_pyramid_reduce.hlsli, shared with HiZPass), halving the dispatch
    // and global UAV barrier count; one mip per dispatch would make the 4x4/2x2/1x1 tail
    // pure launch overhead.
    gfx.BindComputePipelineState(m_reducePSO, cl);

    uint32_t cbSlot = 1;   // slot 0 belongs to the mip-0 dispatch above
    for (uint32_t mip = 1; mip < m_mipCount && cbSlot < kMaxMipsCB; mip += 2)
    {
        const uint32_t srcW = (std::max)(m_width  >> (mip - 1), 1u);
        const uint32_t srcH = (std::max)(m_height >> (mip - 1), 1u);
        const uint32_t dstW = (std::max)(m_width  >> mip, 1u);
        const uint32_t dstH = (std::max)(m_height >> mip, 1u);

        // On an odd tail the second level has no mip to write; u2 is still
        // bound (to mip N) so no root descriptor table is left unset, and
        // writeSecond == 0 keeps the shader from touching it.
        const bool pair = (mip + 1 < m_mipCount);

        const uint32_t cbOffset = cbSlot++ * kCBStride;
        HierCB cb{ srcW, srcH, dstW, dstH, pair ? 1u : 0u };
        std::memcpy(cbBase + cbOffset, &cb, sizeof(cb));
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, cbOffset, cl);

        gfx.SetComputeDescriptorTable(kUAV0,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip - 1), cl);
        gfx.SetComputeDescriptorTable(kUAV1,
            gfx.GetTextureMipUAVGpuHandle(m_texture, mip), cl);
        gfx.SetComputeDescriptorTable(kUAV2,
            gfx.GetTextureMipUAVGpuHandle(m_texture, pair ? mip + 1 : mip), cl);

        // Group count follows the mip N extent; the second level's 4x4 output
        // per group covers exactly ceil(dstW/8) x ceil(dstH/8) mip N+1 texels.
        gfx.DispatchCompute((dstW + 7) / 8, (dstH + 7) / 8, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_texture), cl);
    }
}
