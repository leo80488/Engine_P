#include "RenderGraph/RenderPass/LumaReducePass.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include "System/CommandLine.h"
#include <cmath>
#include <cstdio>
#include <string>

// Loading the recipe again is what tells a hot reload's receipt that this pass
// let go of the old bytecode. The new shader and PSO replace the members only
// on success (CreatePipelineState retires the old PSO itself), so a failed
// rebuild keeps the old pipeline.
bool LumaReducePass::CreatePipeline(IGraphicsDevice& gfx)
{
    const auto dxil = ShaderDiskCache::LoadOrCompile("shaders/LumaReduce.cs.hlsl", "main",
        RHI::ShaderStage::CS, {}, "LumaReducePass");
    RHI::Shader shader;
    if (dxil.empty() || !gfx.CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), shader))
    { LOG_ERROR("LumaReducePass: shader creation failed"); return false; }
    RHI::PipelineStateDesc pso{};
    pso.cs = &shader;
    if (!gfx.CreatePipelineState(pso, m_pso))
    { LOG_ERROR("LumaReducePass: pipeline creation failed"); gfx.DestroyShader(shader); return false; }
    if (m_shader.IsValid()) gfx.DestroyShader(m_shader);
    m_shader = shader;
    return true;
}

void LumaReducePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shader.IsValid()) CreatePipeline(gfx);
}

void LumaReducePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    if (!CreatePipeline(gfx)) return;
    if (!m_cb.Create(gfx, "LumaReduce.CB"))
    { LOG_ERROR("LumaReducePass: CB creation failed"); return; }
    RHI::GPUBufferDesc desc{};
    desc.size = 2 * kTiles * sizeof(float);
    desc.stride = sizeof(float);
    desc.usage = RHI::Usage::DEFAULT;
    desc.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    if (!gfx.CreateBuffer(desc, m_tiles))
    { LOG_ERROR("LumaReducePass: tile buffer creation failed"); return; }
    desc.usage = RHI::Usage::READBACK;
    desc.bindFlags = RHI::BindFlag::NONE;
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (!gfx.CreateBuffer(desc, m_readback[i]) ||
            !(m_mapped[i] = static_cast<const float*>(gfx.MapBuffer(m_readback[i]))))
        { LOG_ERROR("LumaReducePass: readback creation failed"); return; }
    }
#ifdef WITH_HARNESS
    const auto& commandLine = Sys::CommandLine::Get();
    m_measureAlpha=commandLine.Has("-energy_alpha");
    m_pixelVariance=commandLine.Has("-energy_pixel_variance");
    m_varianceStart=static_cast<uint32_t>(std::max(0,commandLine.GetInt("-energy_variance_start",500)));
    if(commandLine.Has("-energy_variance_fixture"))
    {
        if(!commandLine.Has("-energy_fixture"))
        { LOG_ERROR("LumaReducePass: variance fixture requires known-radiance fixture"); return; }
        m_varianceAlternation=.1f;
    }
    if(m_pixelVariance)
    {
        RHI::GPUBufferDesc moments{};
        moments.size=4096*2*sizeof(float); moments.stride=2*sizeof(float);
        moments.usage=RHI::Usage::DEFAULT;
        moments.bindFlags=RHI::BindFlag::UNORDERED_ACCESS;
        moments.miscFlags=RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if(!gfx.CreateBuffer(moments,m_pixelMoments))
        { LOG_ERROR("LumaReducePass: pixel moments allocation failed"); return; }
    }
    if (commandLine.Has("-energy_until"))
    {
        const int frame = commandLine.GetInt("-energy_until", 0);
        if (frame <= 0)
        { LOG_ERROR("LumaReducePass: -energy_until requires a positive dispatch count"); return; }
        m_stopAfter = static_cast<uint64_t>(frame);
    }
    if (commandLine.Has("-energy_fixture"))
    {
        const float radiance = commandLine.GetFloat("-energy_fixture", -1.f);
        if (!std::isfinite(radiance) || radiance < 0.f)
        { LOG_ERROR("LumaReducePass: -energy_fixture requires finite nonnegative radiance"); return; }
        float pixels[8 * 8 * 4];
        for (uint32_t i = 0; i < 8 * 8; ++i)
        {
            pixels[4 * i] = pixels[4 * i + 1] = pixels[4 * i + 2] =
                radiance*(commandLine.Has("-energy_fixture_gradient")?1.f+float(i%8)/7.f:1.f);
            pixels[4 * i + 3] = 1.f;
        }
        RHI::TextureDesc texture{};
        texture.width = texture.height = 8;
        texture.format = RHI::Format::R32G32B32A32_FLOAT;
        texture.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        texture.layout = RHI::ResourceState::SHADER_RESOURCE;
        RHI::SubresourceData data{pixels, 8 * 4 * sizeof(float), sizeof(pixels)};
        if (!gfx.CreateTexture(texture, m_fixture, &data))
        { LOG_ERROR("LumaReducePass: fixture creation failed"); return; }
        LOG_INFO("LumaReducePass: measurement source = known-radiance fixture %.9f (scene unchanged)", radiance);
    }
#endif
    m_ready = true;
}

void LumaReducePass::Shutdown(IGraphicsDevice& gfx)
{
    m_ready = false; m_momentsInitialized=false; m_varianceSamples=0;
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_mapped[i]) gfx.UnmapBuffer(m_readback[i]);
        m_mapped[i] = nullptr;
        if (m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
    }
    if (m_tiles.IsValid()) gfx.DestroyBuffer(m_tiles);
    if (m_pixelMoments.IsValid()) gfx.DestroyBuffer(m_pixelMoments);
    if (m_fixture.IsValid()) gfx.DestroyTexture(m_fixture);
    m_cb.Destroy(gfx);
    if (m_pso.IsValid()) gfx.DestroyPipelineState(m_pso);
    if (m_shader.IsValid()) gfx.DestroyShader(m_shader);
}

RHI::CommandList LumaReducePass::Execute(RHI::CommandList cl)
{
    if (!m_ready || m_source == ~0u || m_width < 8 || m_height < 8) return cl;
    if (m_stopAfter && m_frame >= m_stopAfter)
    {
        if (!m_stopLogged)
        {
            LOG_INFO("[ENERGY_STATE] enabled=0 dispatches=%llu", static_cast<unsigned long long>(m_frame));
            m_stopLogged = true;
        }
        return cl;
    }
    auto& gfx = *m_gfx;
    if (m_fixture.IsValid())
    {
        m_source = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_fixture));
        m_width = m_height = 8;
    }
    const uint32_t slot = gfx.GetFrameIndex();
    // BeginFrame has waited for THIS slot's fence. Read before overwriting it;
    // a neighbouring slot may still be in flight on the GPU.
    if (m_primed[slot])
    {
        for (uint32_t row = 0; row < 8; ++row)
        {
            // Logger's printf buffer is 1024 bytes. Eight rows keep even very
            // bright FP32 fixed-point values from silently truncating t63.
            std::string fields;
            for (uint32_t x = 0; x < 8; ++x)
            {
                const uint32_t i = row * 8 + x;
                const float value = m_mapped[slot][i];
                if (!std::isfinite(value))
                    LOG_ERROR("LumaReducePass: nonfinite tile=%u frame=%llu", i,
                              static_cast<unsigned long long>(m_sequence[slot]));
                char field[96];
                std::snprintf(field, sizeof(field), " t%02u=%.9f", i, value);
                fields += field;
            }
            LOG_INFO("[ENERGY] frame=%llu source=%u width=%u height=%u row=%u tiles=64%s renderFrame=%llu channel=%s",
                static_cast<unsigned long long>(m_sequence[slot]), m_sourceHistory[slot],
                m_widthHistory[slot], m_heightHistory[slot], row, fields.c_str(),
                static_cast<unsigned long long>(m_renderFrameHistory[slot]),m_measureAlpha?"alpha":"rgb");
        }
    }
    if(m_primed[slot] && m_varianceHistory[slot]>0)
    {
        for(uint32_t row=0;row<8;++row)
        {
            std::string fields;
            for(uint32_t x=0;x<8;++x)
            {
                const uint32_t i=row*8+x; const float value=m_mapped[slot][kTiles+i];
                if(!std::isfinite(value) || value<0) LOG_ERROR("LumaReducePass: invalid pixel variance tile=%u",i);
                char field[96]; std::snprintf(field,sizeof(field)," t%02u=%.12f",i,value); fields+=field;
            }
            LOG_INFO("[PIXEL_VARIANCE] frame=%llu source=%u width=%u height=%u row=%u tiles=64 samples=%u%s",
                static_cast<unsigned long long>(m_sequence[slot]),m_sourceHistory[slot],
                m_widthHistory[slot],m_heightHistory[slot],row,m_varianceHistory[slot],fields.c_str());
        }
    }
    if(m_varianceSource!=m_source || m_varianceWidth!=m_width || m_varianceHeight!=m_height)
    { m_varianceSamples=0; m_varianceSource=m_source; m_varianceWidth=m_width; m_varianceHeight=m_height; }
    const bool varianceActive=m_pixelVariance && m_frame>=m_varianceStart;
    if(varianceActive) ++m_varianceSamples;
    uint32_t varianceUav=0;
    if(varianceActive)
    {
        varianceUav=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetBufferUAVGpuHandle(m_pixelMoments));
        if(!m_momentsInitialized)
            gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_pixelMoments,RHI::ResourceState::UNDEFINED,RHI::ResourceState::UNORDERED_ACCESS),cl);
        else gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_pixelMoments),cl);
        m_momentsInitialized=true;
    }
    *m_cb.Current(gfx) = {m_source,m_width,m_height,varianceUav,m_varianceSamples,varianceActive?1u:0u,m_varianceAlternation,m_measureAlpha?1u:0u};
    const uint32_t zone = gfx.BeginGPUTimestamp(cl, "Energy.Reduce");
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_tiles, m_state, RHI::ResourceState::UNORDERED_ACCESS), cl);
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(RHI::Slots::Cmp::CB, m_cb.CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(RHI::Slots::Cmp::UAV_u0, gfx.GetBufferUAVGpuHandle(m_tiles), cl);
    gfx.DispatchCompute(8, 8, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_tiles, RHI::ResourceState::UNORDERED_ACCESS,
                                          RHI::ResourceState::COPY_SRC), cl);
    m_state = RHI::ResourceState::COPY_SRC;
    gfx.CopyBuffer(m_tiles, m_readback[slot], 2 * kTiles * sizeof(float), cl);
    m_varianceHistory[slot]=varianceActive?m_varianceSamples:0;
    m_sequence[slot] = m_frame++;
    m_renderFrameHistory[slot] = m_renderFrame;
    m_sourceHistory[slot] = m_source;
    m_widthHistory[slot] = m_width;
    m_heightHistory[slot] = m_height;
    m_primed[slot] = true;
    gfx.EndGPUTimestamp(cl, zone);
    return cl;
}
