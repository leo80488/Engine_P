#pragma once

// BeamSimPass - runs the beam-tube generation compute shader once per active beam each frame.
// Standalone (manual BeginCommandList) like ParticleSimPass and SkinningPass: it writes UAV
// buffers that the GBuffer / Transparent passes read as PVF SRVs.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class IGraphicsDevice;
class BeamSystem;

class BeamSimPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override { (void)gfx; }
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events this pass handles (Renderer::PassSet)

    void SetSystem(BeamSystem* sys) { m_sys = sys; }

    void Execute(RHI::CommandList cl);

private:
    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_genPSO;

    BeamSystem*        m_sys = nullptr;
};
