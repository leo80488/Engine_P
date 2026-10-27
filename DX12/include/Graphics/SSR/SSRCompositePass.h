#pragma once

// SSRCompositePass - pass 6 of the Hi-Z SSR pipeline, run on FullAdditive (water)
// frames only. Additive blend of resolved SSR into the HDR scene color, weighted by
// Fresnel x envBRDF x confidence. Pairs with Water.ps, which dampens its sky reflection
// by the SSR confidence (LightCB.ssrResultIdx), so the combined specular term stays
// energy-correct. On these frames Lighting.ps does not read the SSR result; it folds it
// (as its RT reflection input) only in FullFold mode, where this pass does not run.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRCompositePass
{
public:
    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3   cameraPos;
    };

    struct alignas(16) SSRCompositeCB
    {
        float    invViewProj[16];
        float    cameraPos[3];   float    _pad0;
        uint32_t screenW;        uint32_t screenH;
        uint32_t useUnifiedConf; uint32_t _pad1;
    };

    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx);

    void SetCamera(const Camera& cam) { m_cam = cam; }

    void Execute(RHI::CommandList cl,
                 uint32_t w, uint32_t h,
                 uint64_t albedoSrv, uint64_t normalSrv, uint64_t surfaceSrv,
                 uint64_t depthSrv,  uint64_t ssrSrv,    uint64_t hdrUav,
                 uint64_t brdfLutSrv);

    // @p srv: SRV of RTReflectionPass's denoised output (.a = conf), 0 = none. When
    // non-zero the composite binds it at t7 and scales its additive term by (1 - conf), so
    // opaque pixels Lighting.ps already folded do not receive the reflection twice.
    void SetUnifiedConfSrv(uint64_t srv) { m_unifiedConfSrv = srv; }

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<SSRCompositeCB> m_cb;

    Camera   m_cam{};
    uint64_t m_unifiedConfSrv  = 0;
};
