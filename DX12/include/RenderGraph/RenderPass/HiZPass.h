#pragma once

// HiZPass - Hierarchical-Z mip chain generation.
//
// Reads the GBuffer depth and builds a min-depth mip chain for GPU occlusion
// culling (next frame's InstanceCull). Mip 0 is a 1:1 copy dispatch; the rest of
// the chain is produced two mips per dispatch (a groupshared second level, see
// depth_pyramid_reduce.hlsli). Owned by Renderer, not a RenderGraph pass.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"

class IGraphicsDevice;

class HiZPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // lifecycle events (Renderer::PassSet)

    // Rebuild the Hi-Z texture when render dimensions change.
    void EnsureTexture(uint32_t depthW, uint32_t depthH);
    // The prepare point (PrepareStage::HiZ, on the frames the pyramid generates):
    // the texture at the depth's size.
    void OnPrepare(const RG::PrepareContext& c) override { EnsureTexture(c.renderW, c.renderH); }

    // Generate the full mip chain from the GBuffer depth.
    // @p depthSrvHandle: GPU SRV handle of the depth texture (after the GBuffer pass).
    // Returns false when nothing was recorded (invalid PSOs / no texture) - the
    // caller must NOT count the frame as generated (ViewState::hiz.framesValid).
    bool Execute(RHI::CommandList cl, uint64_t depthSrvHandle);

    // SRV of the Hi-Z mip 0 (full-res min-depth). For occlusion culling.
    uint64_t GetHiZSrvHandle() const;

    const RHI::Texture* GetHiZTexture() const { return &m_hiZTexture; }

    // Pyramid dimensions for the culling shader's Load()-based sampling.
    uint32_t GetHiZWidth()    const { return m_hiZWidth;  }
    uint32_t GetHiZHeight()   const { return m_hiZHeight; }
    uint32_t GetHiZMipCount() const { return m_mipCount;  }
    // VALID data extent (= depth render dims). The pyramid texture is the render
    // dims rounded up to a multiple of 32 and grow-only, so only its top-left
    // validW x validH region holds real depth (padding = far); the cull shader's
    // uv -> texel mapping MUST use these, not the texture size.
    uint32_t GetHiZValidWidth()  const { return m_validWidth;  }
    uint32_t GetHiZValidHeight() const { return m_validHeight; }

private:
    // writeSecond: reduce dispatches produce TWO mips (u1 = mip N, u2 =
    // mip N+1); 0 tells the shader mip N was the last one in the chain.
    struct HiZCB { uint32_t srcW, srcH, dstW, dstH, writeSecond; };

    bool CreatePipelines(IGraphicsDevice& gfx);

    // One CB PER DISPATCH, from the frame's constants: Execute records
    // 1 + ceil((mipCount-1)/2) dispatches into a CL that runs long after recording,
    // so one reused CB would hand every dispatch the last CPU write (the final 1x1
    // reduce's dims) - each mip would write only texel (0,0) and the stale pyramid
    // would cause false occlusion culls. Writes @p cb into a fresh block and binds it;
    // false when the frame's constants ran out (the allocator reports it).
    bool BindCB(IGraphicsDevice& gfx, const HiZCB& cb, RHI::CommandList cl);

    IGraphicsDevice*   m_gfx = nullptr;
    RHI::PipelineState m_pso;          // mip 0 - depth SRV -> UAV
    RHI::PipelineState m_reducePSO;    // mip N>0 - UAV -> UAV
    ShaderLibrary      m_shaderLib;

    // Hi-Z texture (R32_FLOAT, render dims aligned to 32, full mip chain,
    // SRV+UAV)
    RHI::Texture m_hiZTexture;
    // Tracked resource state. Generation works in UNORDERED_ACCESS; Execute ends in
    // SHADER_RESOURCE so next frame's InstanceCull SRV read is legal. Reading the
    // UAV-state texture through an SRV is undefined on the live GPU (RenderDoc
    // replays, which serialize states, hide it): intermittent stale reads, i.e.
    // per-frame cull flicker.
    RHI::ResourceState m_texState = RHI::ResourceState::UNORDERED_ACCESS;
    uint32_t     m_hiZWidth  = 0;
    uint32_t     m_hiZHeight = 0;
    uint32_t     m_mipCount  = 0;
    uint32_t     m_validWidth  = 0;   // depth render dims - see GetHiZValidWidth
    uint32_t     m_validHeight = 0;
};
