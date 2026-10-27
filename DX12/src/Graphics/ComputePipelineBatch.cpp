#include "Graphics/ComputePipelineBatch.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "System/Log.h"

#include <memory>

namespace ComputePipelineBatch
{

bool Create(IGraphicsDevice& gfx, const RHI::PipelineLayout& layout,
            const std::vector<uint8_t>* kernels, size_t count,
            RHI::PipelineState* outs, bool* failed)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (failed) failed[i] = false;
        gfx.DestroyPipelineState(outs[i]);   // a failed entry must not keep the old pipeline
    }
    if (!layout.IsValid()) return false;
    std::vector<RHI::Shader> shaders(count);
    std::vector<RHI::PipelineStateDesc> descs(count);
    bool ok = true;
    for (size_t i = 0; i < count; ++i)
    {
        if (kernels[i].empty() ||
            !gfx.CreateShader(RHI::ShaderStage::CS, kernels[i].data(), kernels[i].size(), shaders[i]))
        {
            if (failed) failed[i] = true;
            ok = false;
            continue;
        }
        descs[i].cs     = &shaders[i];
        descs[i].layout = &layout;
    }
    const bool batched = ok;
    if (ok) ok = gfx.CreatePipelineStates(descs.data(), static_cast<uint32_t>(count), outs);
    for (RHI::Shader& s : shaders) gfx.DestroyShader(s);   // the pipelines keep what they need
    if (!ok)
        for (size_t i = 0; i < count; ++i)
        {
            if (failed && batched && !outs[i].IsValid()) failed[i] = true;
            gfx.DestroyPipelineState(outs[i]);
        }
    return ok;
}

bool BuildEntries(IGraphicsDevice& gfx, const RHI::PipelineLayout& layout, const char* hlslPath,
                  const char* const* entries, size_t count,
                  RHI::PipelineState* outs, const char* logTag)
{
    std::vector<std::vector<uint8_t>> kernels(count);
    bool ok = layout.IsValid();
    for (size_t i = 0; i < count && ok; ++i)
    {
        kernels[i] = ShaderDiskCache::LoadOrCompile(hlslPath, entries[i], RHI::ShaderStage::CS, {}, logTag);
        if (kernels[i].empty()) { LOG_ERROR("%s: pipeline %s failed", logTag, entries[i]); ok = false; }
    }
    if (!ok)
    {
        for (size_t i = 0; i < count; ++i) gfx.DestroyPipelineState(outs[i]);
        return false;
    }
    std::unique_ptr<bool[]> failed(new bool[count]());
    if (Create(gfx, layout, kernels.data(), count, outs, failed.get())) return true;
    for (size_t i = 0; i < count; ++i)
        if (failed[i]) LOG_ERROR("%s: pipeline %s failed", logTag, entries[i]);
    return false;
}

} // namespace ComputePipelineBatch
