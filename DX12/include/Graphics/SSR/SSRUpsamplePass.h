#pragma once

// SSRUpsamplePass - pass 5 of the Hi-Z SSR pipeline.
//
// Reads the temporal color + variance, runs a variance-driven a-trous bilateral blur and
// writes the final reflection texture at render resolution. Water.ps reads it one
// frame latent (LightCB.ssrResultIdx), LightingPass folds it in FullFold mode, and
// SSRComposite additively blends it into HDR on FullAdditive frames.

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRUpsamplePass
{
public:
    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        float               nearZ;
        float               farZ;
    };

    struct alignas(16) SSRUpsampleCB
    {
        float    invViewProj[16];
        uint32_t screenW;     uint32_t screenH;
        float    invScreenW;  float    invScreenH;
        float    nearZ;       float    farZ;
        uint32_t stepSize;    uint32_t _pad1;   // a-trous hole size (1, 2, ...)
    };

    // A-trous iteration count. Iteration i taps at stepSize = 1 << i; ping-pongs
    // temporal -> m_pingTex -> m_colorTex.
    static constexpr uint32_t kIterations = 2;

    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);
    // Output at render resolution. The inputs are read by UV (SampleLevel), so a
    // lower-resolution temporal input would be upsampled bilaterally; every SSR pass runs at
    // full render res.
    // Frees the textures (deferred); the chain that owns them is going away (a released view's).
    void ReleaseTextures();
    void EnsureTexture(uint32_t renderW, uint32_t renderH);

    void SetCamera(const Camera& cam) { m_cam = cam; }

    uint64_t GetColorSrv() const;
    const RHI::Texture* GetColorTexture() const { return &m_colorTex; }

    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t temporalSrv, uint64_t varianceSrv,
                 uint64_t depthSrv, uint64_t normalSrv, uint64_t surfaceSrv);

private:
    static constexpr uint32_t kCBStride = RHI::kConstantBufferAlignment;
    struct alignas(256) SSRUpsampleCBArray { uint8_t bytes[kCBStride * kIterations]; };

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // One 256-byte CB slot per iteration: both dispatches are recorded into the same
    // command list, so a shared slot would race (the GPU reads the LAST CPU write for every
    // dispatch).
    FrameCB<SSRUpsampleCBArray> m_cb;

    RHI::Texture m_colorTex;       // RGBA16F (SR+UAV), full render res, final
    RHI::Texture m_pingTex;        // RGBA16F (SR+UAV), iteration scratch
    uint32_t m_w = 0, m_h = 0;
    Camera   m_cam{};
};
