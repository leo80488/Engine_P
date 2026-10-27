#pragma once
// DepthTrace - DX12_DEPTHTRACE=1: command-list ordering trace for the GBuffer depth
// buffer.
//
// Several command lists touch GBuffer_Depth after the RenderGraph (Hi-Z, SSR trace +
// composite, the DDGI probe-debug list, the pre-compute transition list). Each bakes the
// tracker's state into a barrier's StateBefore at record time, which is correct only if
// the GPU runs them in record order; without AddCommandListDependency edges only
// EndFrame's Kahn-sort tie-break by pool index holds that, and when it slips the debug
// layer reports a 'GBuffer_Depth' before-state mismatch naming only a list ('cmd28').
// This trace logs which pass owns which list id, every GBuffer_Depth transition with
// its list id, and EndFrame's final submission order.
//
// Cost when unset: one cached bool per call site; the environment is read once.
// Usage: set DX12_DEPTHTRACE=1, run, grep DX12Log.txt for "[DEPTHTRACE]".

#include "Graphics/GraphicsStruct.h"

#include <cstdint>

namespace GfxTrace
{
    // DX12_DEPTHTRACE=1 (any value but "0"). Cached; safe from any thread.
    bool Enabled();

    // Called once per device frame from GraphicsDX12::BeginFrame. Resets the
    // per-command-list label table, which is indexed by pool slot and so only valid
    // within one frame.
    void BeginFrame(uint64_t frameIndex);

    // Names a command list: logs a roster line and records the name for every later
    // barrier line. @p name must outlive the frame (a pass's GetName() or a literal,
    // never a temporary).
    void Label(const RHI::CommandList& cl, const char* name);

    // Every image barrier funnels through here from GraphicsDX12::PushBarrier /
    // PushBarriers, one call per D3D12 barrier; only textures whose name contains
    // "GBuffer_Depth" are logged. The caller passes the translated D3D12 state bits too,
    // to match debug-layer messages that quote them in hex
    // (GraphicsDX12::ToD3D12ResourceState is private). @p name is the D3D12 object's
    // name (a texture desc's debugName is borrowed only for CreateTexture); @p plane is the
    // depth-stencil plane a plane barrier moves (-1: every plane, the line then has no
    // plane=).
    void ImageBarrier(uint32_t clId, const char* name,
                      RHI::ResourceState before, RHI::ResourceState after,
                      uint32_t d3dBefore, uint32_t d3dAfter, int plane);

    // EndFrame's final topological submission order: logs the whole order, plus a
    // shorter line with only the lists that emitted a GBuffer_Depth barrier this frame
    // (the one that answers "did Hi-Z run before SSR").
    void SubmitOrder(const uint32_t* order, uint32_t count);

    // One command list's declared dependency edges, logged only for lists that touched
    // GBuffer_Depth this frame. The submission order alone cannot tell a declared chain
    // from a tie-break that happens to give the same order, so the edges are printed too.
    void Edges(uint32_t clId, const uint32_t* deps, uint32_t count);
}
