#pragma once
// RtxdiProbe - the RTXDI integration probe, built only under -rtxdi_probe.
// Compiles the compute pipelines of shaders/RtxdiProbe.cs.hlsl (RTXDI's ReSTIR
// DI and GI through the engine's bridge) and shaders/RtxdiProbePT.cs.hlsl
// (ReSTIR PT, HLSL 2021 + 16-bit types, only on a device with native 16-bit
// shader ops) on the ReSTIR shell layout; checks that the RTXDI parameter
// structs the C++ library fills sit where the HLSL reads them (reflection vs
// offsetof); runs the library's C++ contexts once. Nothing is dispatched.
// Renderer::ReloadShaders rebuilds it, so an edit to the bridge or a library
// header gets a hot-reload receipt. Each build logs one line packs read:
//   [RTXDI] probe build=N sdk=<version> di=ok gi=ok pt=ok layout=ok ms=...
// (di / gi / pt: ok | failed | skipped:<why>; layout: ok | mismatch).
#include "Graphics/GraphicsStruct.h"
#include <array>
#include <cstdint>

class IGraphicsDevice;

class RtxdiProbe
{
public:
    void Build(IGraphicsDevice& gfx, uint32_t renderWidth, uint32_t renderHeight);
    void ReloadShaders(IGraphicsDevice& gfx);
    void Release(IGraphicsDevice& gfx);
    bool AllOk() const { return m_allOk; }

private:
    RHI::PipelineLayout m_layout;   // the ReSTIR shell layout (ReSTIR::CreateShellLayout)
    std::array<RHI::PipelineState, 3> m_pso;   // DI, GI, PT
    uint32_t m_builds = 0;
    uint32_t m_width = 0, m_height = 0;
    bool m_allOk = false;
};
