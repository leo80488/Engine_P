#pragma once

// SceneColorPyramidPass - pre-filtered HDR mip chain for SSR cone fetch.
//
// Built each frame from the race-free HDR snapshot (owned by SSRResolvePass). The trace
// samples it at a mip level derived from roughness x ray length, so glossy reflections
// get pre-integrated radiance: the main cure for rough-surface SSR noise.
//
// Owned by SSRSubsystem; runs between the HDR -> snapshot copy and the trace dispatch.
// Binds the shared compute root signature (space2).

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class SceneColorPyramidPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);

    // Frees the textures (deferred); the chain that owns them is going away (a released view's).
    void ReleaseTextures();
    void EnsureTexture(uint32_t w, uint32_t h);

    // Generates the full mip chain from the HDR snapshot. @p snapshotTex must be in
    // SHADER_RESOURCE on entry; this method transitions it through COPY_SRC and back. Mip 0
    // is a CopyTextureSubresource (native copy), not a CS: the CS mip-0 path silently
    // dropped its writes despite a valid PSO, texture and binding.
    //
    // State contract: the pyramid is in SHADER_RESOURCE at ALL times outside this method
    // (Execute goes through COPY_DST / UAV internally and back to SR at the end), so
    // consumers outside the SSR chain (RTReflectionTrace reuses last frame's pyramid for
    // hit-point screen radiance) read it without cross-pass state juggling.
    void Execute(RHI::CommandList cl, const RHI::Texture* snapshotTex);

    // 0 until the first Execute() has filled the chain: a freshly created texture is
    // garbage and must not be sampled.
    uint64_t            GetSrvHandle() const;
    const RHI::Texture* GetTexture()  const { return &m_texture; }
    uint32_t            GetWidth()    const { return m_width; }
    uint32_t            GetHeight()   const { return m_height; }
    uint32_t            GetMipCount() const { return m_mipCount; }

private:
    struct HierCB { uint32_t srcW, srcH, dstW, dstH; };

    // Each mip dispatch reads its own 256-byte CB slot: dispatches recorded into one CL
    // that share a slot would all read the LAST CPU write.
    static constexpr uint32_t kCBStride  = RHI::kConstantBufferAlignment;
    static constexpr uint32_t kMaxMipsCB = 16;
    struct alignas(256) HierCBArray { uint8_t bytes[kCBStride * kMaxMipsCB]; };

    IGraphicsDevice*   m_gfx = nullptr;

    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_mip0PSO;
    RHI::PipelineState m_reducePSO;

    RHI::Texture       m_texture;        // RGBA16F, render res, full mip chain
    uint32_t           m_width    = 0;
    uint32_t           m_height   = 0;
    uint32_t           m_mipCount = 0;
    bool               m_inSR     = false; // true = texture parked in SHADER_RESOURCE
    bool               m_built    = false; // first Execute() completed (content valid)

    FrameCB<HierCBArray> m_cb;
};
