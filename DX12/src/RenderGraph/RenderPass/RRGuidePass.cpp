#include "RenderGraph/RenderPass/RRGuidePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/ShaderDiskCache.h"
#include "System/Log.h"

#include <cstring>
#include <vector>

namespace
{
    // Both private compute pipeline layouts here - the colour snapshot's align
    // and the guides' - are one parameter of 32-bit constants.
    constexpr RHI::Slots::CmpParam kRootConstants{ 0 };

    // RRGuides.cs.hlsl's cbuffer Params (b0), as root constants: 44 DWORDs.
    struct Params
    {
        DirectX::XMFLOAT4X4 invViewProj;   // transposed for the row-vector HLSL
        float    camera[3];  uint32_t width;
        uint32_t height, albedoIdx, normalIdx, surfaceIdx;
        uint32_t depthIdx, materialsIdx, extraIdx, flags;
        uint32_t albedoUav, specularUav, normalUav, distanceUav;
        float    sunDirection[3]; uint32_t diResultIdx;
        float    sunColor[3];     uint32_t reflectionIdx;
        uint32_t reflectionDistanceIdx, radianceIdx, radianceMips; float iblStrength;
    };
    static_assert(sizeof(Params) == 44 * 4, "RRGuides.cs.hlsl's Params is 44 DWORDs");

    // One compute pipeline on @p layout; @p pso is replaced only on success, so
    // a failed rebuild keeps the old pipeline (CreatePipelineState's rule).
    bool MakePipeline(IGraphicsDevice& gfx, const RHI::PipelineLayout& layout, const std::vector<uint8_t>& dxil,
                      RHI::PipelineState& pso)
    {
        RHI::Shader cs;
        if (!gfx.CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), cs)) return false;
        RHI::PipelineStateDesc pd{};
        pd.cs = &cs;
        pd.layout = &layout;
        const bool ok = gfx.CreatePipelineState(pd, pso);
        gfx.DestroyShader(cs);
        return ok;
    }
}

// ==== RRColorSnapshot ====
void RRColorSnapshot::Prepare(IGraphicsDevice& gfx, bool wanted)
{
    m_wanted = wanted;
    m_written.store(false, std::memory_order_relaxed);
    if (!wanted) { Release(gfx); return; }
    const uint32_t w = gfx.GetRenderWidth(), h = gfx.GetRenderHeight();
    if (w == 0 || h == 0) return;
    if (m_tex.IsValid() && w == m_w && h == m_h) return;
    Release(gfx);
    // The HDR scene target's format and size: CopyHdrSceneToScoped is a
    // full-resource copy (NPRStabilizePass's lit snapshot, the same rule).
    RHI::TextureDesc td{};
    td.width = w; td.height = h;
    td.format = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;   // Align
    td.usage = RHI::Usage::DEFAULT;
    td.layout = RHI::ResourceState::COPY_DST;
    td.debugName = "RRColor.BeforeTransparency";
    if (!gfx.CreateTexture(td, m_tex))
    {
        LOG_ERROR("RRColorSnapshot: allocation failed (%ux%u)", w, h);
        return;
    }
    m_state = td.layout;
    m_w = w; m_h = h;
    LOG_INFO("[RR_COLOR] allocated=1 width=%u height=%u bytes=%llu", w, h, static_cast<unsigned long long>(w) * h * 8ull);
}

void RRColorSnapshot::Release(IGraphicsDevice& gfx)
{
    if (!m_tex.IsValid()) return;
    gfx.DestroyTexture(m_tex);
    m_tex = {};
    m_w = m_h = 0;
    m_written.store(false, std::memory_order_relaxed);
    LOG_INFO("[RR_COLOR] released=1");
}

void RRColorSnapshot::Copy(IGraphicsDevice& gfx, RHI::CommandList cl)
{
    if (!Wanted()) return;
    // A full-resource copy: the HDR target's size and format must be the
    // snapshot's (Prepare allocates R16G16B16A16_FLOAT at the render size).
    uint32_t hdrW = 0, hdrH = 0;
    if (!gfx.GetHdrSceneSize(hdrW, hdrH)) return;
    const RHI::Format hdrFormat = gfx.GetHdrSceneColorFormat();
    if (hdrW != m_w || hdrH != m_h || hdrFormat != RHI::Format::R16G16B16A16_FLOAT)
    {
        static std::atomic<bool> s_warned{ false };
        if (!s_warned.exchange(true))
            LOG_WARNING("RRColorSnapshot: the HDR target is %ux%u (RHI format %d), the snapshot %ux%u (RHI format %d) - "
                        "copy declined", hdrW, hdrH, static_cast<int>(hdrFormat), m_w, m_h,
                        static_cast<int>(RHI::Format::R16G16B16A16_FLOAT));
        return;
    }
    if (m_state != RHI::ResourceState::COPY_DST)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_tex, m_state, RHI::ResourceState::COPY_DST), cl);
    gfx.CopyHdrSceneToScoped(m_tex, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_tex, RHI::ResourceState::COPY_DST,
                                           RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_state = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    m_written.store(true, std::memory_order_release);
}

void RRColorSnapshot::Shutdown(IGraphicsDevice& gfx)
{
    Release(gfx);
    gfx.DestroyPipelineState(m_alignPso);
    gfx.DestroyPipelineLayout(m_alignLayout);
}

bool RRColorSnapshot::BuildAlignPipeline(IGraphicsDevice& gfx)
{
    if (!m_alignLayout.IsValid())
    {
        RHI::PipelineLayoutBuilder b("RRColorAlign");
        b.Constants(kRootConstants, 5, 0)
         .Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
        if (!gfx.CreatePipelineLayout(b.Desc(), m_alignLayout))
        {
            LOG_ERROR("RRColorSnapshot: pipeline layout creation failed");
            return false;
        }
    }
    auto shader = ShaderDiskCache::LoadOrCompile("shaders/RRColorAlign.cs.hlsl", "main", RHI::ShaderStage::CS, {}, "RRColorAlign");
    if (shader.empty())
    {
        LOG_ERROR("RRColorSnapshot: RRColorAlign.cs.hlsl did not compile");
        return false;
    }
    // Replaced only on success: a failed rebuild keeps the old pipeline.
    if (!MakePipeline(gfx, m_alignLayout, shader, m_alignPso))
    {
        LOG_ERROR("RRColorSnapshot: pipeline creation failed");
        return false;
    }
    return true;
}

void RRColorSnapshot::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_alignPso.IsValid()) return;
    if (!BuildAlignPipeline(gfx))
        LOG_ERROR("RRColorSnapshot: RRColorAlign rebuild after shader reload failed - keeping the old pipeline");
}

bool RRColorSnapshot::Align(IGraphicsDevice& gfx, RHI::CommandList cl, uint64_t hdrSrv, uint64_t stabilizedSrv)
{
    if (!hdrSrv || !stabilizedSrv || !m_tex.IsValid() || !m_written.load(std::memory_order_acquire)) return false;
    const bool ok = m_alignPso.IsValid() || (!m_alignFailed && BuildAlignPipeline(gfx));
    if (!ok) { m_alignFailed = true; return false; }
    if (m_state != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_tex, m_state, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_state = RHI::ResourceState::UNORDERED_ACCESS;
    const uint32_t c[5] = { m_w, m_h, gfx.DescriptorHeapIndexFromGpuHandle(hdrSrv),
                            gfx.DescriptorHeapIndexFromGpuHandle(stabilizedSrv),
                            gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_tex)) };
    gfx.BindComputePipelineState(m_alignPso, cl);
    gfx.SetComputeRootConstants(kRootConstants, 5, c, 0, cl);
    gfx.DispatchCompute((m_w + 7) / 8, (m_h + 7) / 8, 1, cl);
    gfx.RestoreDefaultComputeRootSignature(cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_tex, m_state, RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_state = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    if (!m_alignLogged)
    {
        LOG_INFO("[RR_COLOR] aligned=1 width=%u height=%u (NPRStabilize's correction on the snapshot)", m_w, m_h);
        m_alignLogged = true;
    }
    return true;
}

// ==== RRGuidePass ====
bool RRGuidePass::BuildPipeline(IGraphicsDevice& gfx)
{
    if (!m_layout.IsValid())
    {
        RHI::PipelineLayoutBuilder b("RRGuides");
        b.Constants(kRootConstants, sizeof(Params) / 4, 0);
        // s0: the linear-clamp sampler for the sky's prefiltered radiance cube.
        RHI::SamplerDesc linearClamp{};   // MIN_MAG_MIP_LINEAR, MaxLOD FLT_MAX
        linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = RHI::TextureAddressMode::CLAMP;
        linearClamp.comparisonFunc = RHI::ComparisonFunc::NONE;   // D3D12's 0: no comparison
        b.Sampler(linearClamp, 0)
         .Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
        if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
        {
            LOG_ERROR("RRGuidePass: pipeline layout creation failed");
            return false;
        }
    }
    auto shader = ShaderDiskCache::LoadOrCompile("shaders/RRGuides.cs.hlsl", "main", RHI::ShaderStage::CS, {}, "RRGuides");
    if (shader.empty())
    {
        LOG_ERROR("RRGuidePass: RRGuides.cs.hlsl did not compile");
        return false;
    }
    // Replaced only on success: a failed rebuild keeps the old pipeline.
    if (!MakePipeline(gfx, m_layout, shader, m_pso))
    {
        LOG_ERROR("RRGuidePass: pipeline creation failed");
        return false;
    }
    return true;
}

// Hot reload runs after FlushAndWait. Not built yet: the first Execute
// compiles the new source.
void RRGuidePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_pso.IsValid()) return;
    if (!BuildPipeline(gfx))
        LOG_ERROR("RRGuidePass: RRGuides rebuild after shader reload failed - keeping the old pipeline");
}

bool RRGuidePass::Prepare(IGraphicsDevice& gfx, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return false;
    if (IsAllocated() && width == m_width && height == m_height) return true;
    Release(gfx);
    for (uint32_t i = 0; i < 4; ++i)
    {
        RHI::TextureDesc desc{};
        desc.width = width; desc.height = height;
        desc.format = i == 3 ? RHI::Format::R32_FLOAT : RHI::Format::R16G16B16A16_FLOAT;
        desc.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        desc.usage = RHI::Usage::DEFAULT; desc.layout = RHI::ResourceState::UNORDERED_ACCESS;
        desc.debugName = "RRGuidePass.Guide";
        if (!gfx.CreateTexture(desc, m_guides[i]))
        {
            LOG_ERROR("RRGuidePass: guide %u allocation failed (%ux%u)", i, width, height);
            Release(gfx);
            return false;
        }
        m_states[i] = desc.layout;
    }
    m_width = width; m_height = height;
    LOG_INFO("[RR_RASTER_GUIDES] allocated=1 width=%u height=%u bytes=%llu", width, height,
             static_cast<unsigned long long>(width) * height * 28ull);
    return true;
}

void RRGuidePass::Release(IGraphicsDevice& gfx)
{
    if (!IsAllocated()) return;
    for (auto& guide : m_guides)
    {
        if (guide.IsValid()) gfx.DestroyTexture(guide);
        guide = {};
    }
    m_width = m_height = 0;
    m_wrote = false;
    LOG_INFO("[RR_RASTER_GUIDES] released=1");
}

void RRGuidePass::Shutdown(IGraphicsDevice& gfx)
{
    Release(gfx);
    gfx.DestroyPipelineState(m_pso);
    gfx.DestroyPipelineLayout(m_layout);
}

bool RRGuidePass::Execute(RHI::CommandList cl, IGraphicsDevice& gfx, const Inputs& in)
{
    m_wrote = false;
    if (!IsAllocated() || in.width != m_width || in.height != m_height) return false;
    if (in.depthIdx == 0 || in.albedoIdx == 0 || in.normalIdx == 0 || in.surfaceIdx == 0) return false;
    if (!m_pso.IsValid())
    {
        if (m_pipelineFailed) return false;
        if (!BuildPipeline(gfx)) { m_pipelineFailed = true; return false; }
    }
    Params p{};
    DirectX::XMStoreFloat4x4(&p.invViewProj, DirectX::XMMatrixTranspose(
        DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&in.viewProj))));
    std::memcpy(p.camera, &in.camera, sizeof(p.camera));
    p.width = in.width; p.height = in.height;
    p.albedoIdx = in.albedoIdx; p.normalIdx = in.normalIdx; p.surfaceIdx = in.surfaceIdx;
    p.depthIdx = in.depthIdx; p.materialsIdx = in.materialsIdx; p.extraIdx = in.extraIdx; p.flags = in.flags;
    std::memcpy(p.sunDirection, &in.sunDirection, sizeof(p.sunDirection));
    std::memcpy(p.sunColor, &in.sunColor, sizeof(p.sunColor));
    p.diResultIdx = in.diResultIdx;
    p.reflectionIdx = in.reflectionIdx; p.reflectionDistanceIdx = in.reflectionDistanceIdx;
    p.radianceIdx = in.radianceIdx; p.radianceMips = in.radianceMips; p.iblStrength = in.iblStrength;
    uint32_t* uav[4] = { &p.albedoUav, &p.specularUav, &p.normalUav, &p.distanceUav };
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (m_states[i] != RHI::ResourceState::UNORDERED_ACCESS)
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_guides[i], m_states[i], RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_states[i] = RHI::ResourceState::UNORDERED_ACCESS;
        *uav[i] = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_guides[i]));
    }
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootConstants(kRootConstants, sizeof(Params) / 4, &p, 0, cl);
    gfx.DispatchCompute((in.width + 7) / 8, (in.height + 7) / 8, 1, cl);
    gfx.RestoreDefaultComputeRootSignature(cl);
    for (uint32_t i = 0; i < 4; ++i)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_guides[i], m_states[i], RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_states[i] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }
    if (!m_logged)
    {
        LOG_INFO("[RR_RASTER_GUIDES] executed=1 width=%u height=%u flags=%u materials=%u extra=%u diResult=%u "
                 "reflection=%u reflectionDistance=%u sky=%u",
                 in.width, in.height, in.flags, in.materialsIdx ? 1u : 0u, in.extraIdx ? 1u : 0u, in.diResultIdx ? 1u : 0u,
                 in.reflectionIdx ? 1u : 0u, in.reflectionDistanceIdx ? 1u : 0u, in.radianceIdx ? 1u : 0u);
        m_logged = true;
    }
    m_wrote = true;
    return true;
}
