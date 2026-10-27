#pragma once
// Pass helpers: this frame's constants, the pipeline, the bindings, the dispatch or draw and
// a GPU zone behind one call each, for compute post effects and fullscreen draws. They record
// through the RHI::CommandList facade and nothing else: barriers and resource states stay
// with the pass, which owns them.

#include "Graphics/IGraphicsDevice.h"   // AllocConstants<T>, the facade's device
#include "Graphics/PSOCache.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShaderLibrary.h"     // ShaderID

#include <cstdint>
#include <initializer_list>

namespace RG
{
// ==== GPU zone ====
// BeginGPUTimestamp / EndGPUTimestamp as a scope: the profiler region, its Tracy zone and the
// capture marker, closed on every way out. For a sub-scope inside a pass and for the serial
// call sites outside the graph; the graph's own per-pass region must not use it (Tracy fixes
// a zone's parent at begin - IGraphicsDevice::BeginGPUTimestampNoTracy).
class GpuScope
{
public:
    GpuScope(RHI::CommandList cl, const char* name)
        : m_cl(cl), m_region(cl.GetDevice().BeginGPUTimestamp(cl, name)) {}
    ~GpuScope() { m_cl.GetDevice().EndGPUTimestamp(m_cl, m_region); }
    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    RHI::CommandList m_cl;
    uint32_t         m_region;
};
} // namespace RG

#define RG_GPU_SCOPE_CAT2(a, b) a##b
#define RG_GPU_SCOPE_CAT(a, b)  RG_GPU_SCOPE_CAT2(a, b)
#define RG_GPU_SCOPE(cl, name)  ::RG::GpuScope RG_GPU_SCOPE_CAT(rgGpuScope_, __LINE__){ (cl), (name) }

namespace RG
{
// ==== One compute dispatch ====

// The thread groups of one dispatch.
struct Groups
{
    uint32_t x = 1, y = 1, z = 1;
};

// ceil(w / tile) x ceil(h / tile) x 1: a post effect's 2D grid of tiles.
constexpr Groups Groups2D(uint32_t w, uint32_t h, uint32_t tile)
{
    return { (w + tile - 1u) / tile, (h + tile - 1u) / tile, 1u };
}

// One descriptor table of the compute root signature. A zero handle binds
// nothing (IGraphicsDevice::SetComputeDescriptorTable), so an optional input
// stays optional.
struct ComputeTable
{
    RHI::Slots::CmpParam slot;
    uint64_t             gpuHandle;
};

// One dispatch on the shared compute root signature, recorded as pipeline, this frame's
// constants at Cmp::CB, the tables, the dispatch. False, with nothing recorded, when the
// pipeline is not valid or the constant allocator is exhausted (it reported that itself): the
// pass then wrote nothing this frame.
template <class T>
bool DispatchCS(RHI::CommandList cl, const RHI::PipelineState& cs, const T& constants,
                std::initializer_list<ComputeTable> tables, Groups groups)
{
    if (!cs.IsValid()) return false;
    const RHI::Constants<T> cb = cl.AllocConstants<T>();
    if (!cb) return false;
    *cb.cpu = constants;
    cl.SetComputePipelineState(cs);
    cl.BindComputeCBAddress(RHI::Slots::Cmp::CB, cb.gpu);
    for (const ComputeTable& t : tables)
        cl.BindComputeTable(t.slot, t.gpuHandle);
    cl.Dispatch(groups.x, groups.y, groups.z);
    return true;
}

// ==== One fullscreen draw ====

enum class FullscreenBlend : uint8_t
{
    Opaque,              // no blending: the draw replaces the target
    PremultipliedOver,   // colour ONE / INV_SRC_ALPHA, alpha ZERO / ONE: the target keeps its alpha
};

// Where a fullscreen draw renders. DrawFullscreen binds the target itself and gives the
// pipeline the formats of what it bound, so the two cannot disagree. The only kind is the
// scene colour with a depth target (IGraphicsDevice::SetRenderTargetToHdrWithDepth); add
// another with its first user.
struct FullscreenTarget
{
    RGTextureHandle depth{};

    static FullscreenTarget HdrWithDepth(RGTextureHandle depth)
    {
        FullscreenTarget t;
        t.depth = depth;
        return t;
    }
};

struct FullscreenDraw
{
    ShaderID         ps;
    FullscreenTarget target;
    FullscreenBlend  blend   = FullscreenBlend::Opaque;
    // A GPU zone around the draw (RG_GPU_SCOPE) when set.
    const char*      gpuZone = nullptr;
    // The fullscreen-triangle VS (SV_VertexID only, despite the file's name). The pass's
    // ShaderLibrary registers it with its PS.
    ShaderID         vs      = ShaderID::VolumetricApply_VS;
};

// A graph constant buffer (LightCB, ...) and a graph texture for the draw, on
// the shared graphics root signature's group-relative slots.
struct GraphCB
{
    RHI::Slots::GfxCB slot;
    RGSlot            cb;
};
struct GraphSRV
{
    RHI::Slots::GfxSRV slot;
    RGTextureHandle    texture;
};

// The pipeline DrawFullscreen builds for @p draw on targets of these formats.
// A pass warms it at Init, before its depth texture exists in the graph; the
// draw itself takes the formats from what it binds, so a stale warm-up costs a
// first-use compile, never a wrong pipeline.
PSODesc FullscreenPipelineDesc(const FullscreenDraw& draw, RHI::Format colorFormat,
                               RHI::Format depthFormat);

namespace detail
{
bool DrawFullscreen(RHI::CommandList cl, PSOCache& cache, const FullscreenDraw& draw,
                    RHI::Slots::GfxCB constantsSlot, uint64_t constantsVA,
                    std::initializer_list<GraphCB> cbs, std::initializer_list<GraphSRV> srvs);
}

// One fullscreen triangle: this frame's constants at constantsSlot, the pipeline the cache
// holds for this draw on the target's formats, the target, the heaps, the graph CBs and
// textures, the draw. False, with nothing recorded, when the allocator is exhausted, the
// target's depth is not in the graph or the pipeline cannot be built.
template <class T>
bool DrawFullscreen(RHI::CommandList cl, PSOCache& cache, const FullscreenDraw& draw,
                    RHI::Slots::GfxCB constantsSlot, const T& constants,
                    std::initializer_list<GraphCB> cbs = {}, std::initializer_list<GraphSRV> srvs = {})
{
    const RHI::Constants<T> cb = cl.AllocConstants<T>();
    if (!cb) return false;
    *cb.cpu = constants;
    return detail::DrawFullscreen(cl, cache, draw, constantsSlot, cb.gpu, cbs, srvs);
}
} // namespace RG
