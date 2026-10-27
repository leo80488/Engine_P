#pragma once

// SSRDepthHierarchyPass - 2-channel depth pyramid for the SSR Hi-Z ray march.
//
// Runs after GBuffer, before SSRTrace. Produces a render-res R16G16_FLOAT pyramid:
// .r = max depth (nearest surface, reverse-Z), .g = min depth (farthest). Separate
// from HiZPass, a single-channel min-depth pyramid sized for occlusion culling: SSR
// needs both channels to pick the right tile wall per ray direction.
//
// Owned by SSRSubsystem; binds the shared compute root signature (space2).

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;

class SSRDepthHierarchyPass
{
public:
    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);

    // Rebuild the pyramid texture when render dimensions change. Keeps the
    // existing resource if dimensions haven't changed.
    // Frees the textures (deferred); the chain that owns them is going away (a released view's).
    void ReleaseTextures();
    // Everything it made (deferred releases): its owner goes (a released view's RT reflections).
    void Shutdown();
    void EnsureTexture(uint32_t depthW, uint32_t depthH);

    // Generates the full mip chain from @p depthSrvHandle (the grass-free trace depth when
    // there is one, else GBuffer depth), which must be in SHADER_RESOURCE.
    void Execute(RHI::CommandList cl, uint64_t depthSrvHandle);

    // SRV of the full pyramid (all mips). SSRTrace samples different mips
    // via Load(int3(xy, mip)).
    uint64_t GetSrvHandle() const;

    const RHI::Texture* GetTexture() const { return &m_texture; }
    uint32_t            GetWidth()   const { return m_width; }
    uint32_t            GetHeight()  const { return m_height; }
    uint32_t            GetMipCount()const { return m_mipCount; }

private:
    // writeSecond: reduce dispatches produce TWO mips (u1 = mip N, u2 =
    // mip N+1); 0 tells the shader mip N was the last one in the chain.
    struct HierCB { uint32_t srcW, srcH, dstW, dstH, writeSecond; };

    // 16 HierCB entries x 256-byte alignment, one per dispatch. Dispatches recorded into
    // the same CL must use distinct CB byte offsets, or every reduce reads the LAST CPU
    // write.
    static constexpr uint32_t kCBStride  = RHI::kConstantBufferAlignment;
    static constexpr uint32_t kMaxMipsCB = 16;
    struct alignas(256) HierCBArray { uint8_t bytes[kCBStride * kMaxMipsCB]; };

    IGraphicsDevice*   m_gfx = nullptr;

    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_mip0PSO;      // depth SRV -> mip 0
    RHI::PipelineState m_reducePSO;    // mip N = reduce(mip N-1)

    RHI::Texture       m_texture;      // R16G16_FLOAT, render res, full mip chain
    uint32_t           m_width    = 0;
    uint32_t           m_height   = 0;
    uint32_t           m_mipCount = 0;

    FrameCB<HierCBArray> m_cb;
};
