#pragma once

// DDGIProbeDebugPass - draws one sphere per probe at its relocated world position (an
// impostor quad; the PS resolves the sphere analytically and writes SV_Depth). The sphere's
// normal samples the volume's irradiance atlas through the octahedral mapping, so each sphere
// shows what a surface facing that way receives: a red wall lights the +X side of every probe
// red - the classic DDGI debug view.
// Standalone pass owned by the Renderer (manual Execute, like DebugWirePass / SkinningPass).
// Draws into HDR with a read-only depth test, so probes and scene geometry occlude each other.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"

#include <cstdint>
#include <vector>

namespace DDGI { class DDGIVolumeManager; }
class World;

class DDGIProbeDebugPass : public RG::PassLifecycle
{
public:
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override;
    bool BuildPipeline(IGraphicsDevice& gfx, RHI::PipelineState& outPSO);
    bool ReloadPipelines(IGraphicsDevice& gfx);   // false: not ready, or a pipeline failed
    void ReloadShaders(IGraphicsDevice& gfx) override { ReloadPipelines(gfx); }
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events from Renderer::PassSet

    // Draws the probes of the volumes in the SetDebugVolumeSlots list (those with debugDraw set).
    // @p depthTex is the GBuffer depth (tested, never written); @p perViewCB carries viewProj and
    // cameraPosWS; @p view says how the DSV binds (read-only where a plane is in a DEPTH_READ
    // state).
    void Execute(IGraphicsDevice&             gfx,
                 RHI::CommandList             cmd,
                 const RHI::Texture*          depthTex,
                 const RHI::GPUBuffer&        perViewCB,
                 DDGI::DDGIVolumeManager&     mgr,
                 const RHI::DepthView&        view = {});

    // ---- Extracted volume lane ----
    // The game side extracts the slots of the volumes with debugDraw set; the render side reads
    // only these values, never the World or entity generations.
    void SetDebugVolumeSlots(std::vector<uint32_t>&& slots)
    { m_debugVolumeSlots = std::move(slots); }
    bool HasDebugVolumes() const { return !m_debugVolumeSlots.empty(); }

    // User-facing toggle (DebugWirePass conventions) and the sphere radius per probe (world
    // units), small by default so a 16^3 grid does not hide the scene.
    bool   enabled       = true;
    float  sphereRadius  = 0.10f;
    // Diagnostic visualisation mode (g_DebugMode in DDGIProbeDebug.ps.hlsl):
    //   0 = the stored irradiance toward the surface normal (what lighting samples; default)
    //   1 = grid-coord colour (verify probe placement)
    //   2 = mean irradiance (+Y / -Y average) as heat; zero everywhere -> relight is not writing
    //   3 = directional contrast (max - min over six axis taps); unchanged under a moving sun ->
    //       the atlas is not being rewritten
    uint32_t debugMode = 0;

private:
    // The pass's own small graphics layout, separate from the engine's default rasterisation
    // signature:
    //   [0] ROOT_CBV           b0 space0  (PerViewCB - viewProj, cameraPosWS)
    //   [1] ROOT_CBV           b1 space0  (DDGIVolumeGPU - origin / spacing / ...)
    //   [2] ROOT_CONSTANTS x4  b2 space0  (sphereRadius, debugMode, pad, pad)
    //   [3] DESC_TABLE 1 SRV   t0 space0  (irradiance atlas, PS)
    //   [4] DESC_TABLE 1 SRV   t1 space0  (probe data, VS)
    //   static sampler s0 space0 (linear clamp, PS)
    RHI::PipelineLayout m_layout;

    // PSO is fixed (single triangle list, depth-test, no blend) so we can
    // hold one handle rather than going through PSOCache.
    RHI::PipelineState m_pso;

    ShaderLibrary m_shaderLib;

    bool m_ready = false;

    std::vector<uint32_t> m_debugVolumeSlots;
};
