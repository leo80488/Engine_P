#pragma once

// SSRPass (Hi-Z trace) - pass 2 of the Hi-Z SSR pipeline.
//
// Stochastic GGX ray gen + FidelityFX-SSSR-style Hi-Z traversal, with a Lumen-style
// pixel-precision linear finish trace so converged hit UVs land on the actual mesh
// pixel rather than a Hi-Z cell corner. Runs at full render resolution: a half-res
// trace is unstable without TAA.
//
// Output textures (full render resolution):
//   hitBuffer    .rgb = scene color sampled at hit UV, .a = confidence
//   rayDirPDF    .rgb = world-space reflection direction L, .a = PDF
//   rayLength    world-space ray distance (0 on miss)
//   rayClass     R8_UINT: geometry / sky / edge miss / thickness miss

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

class SSRPass
{
public:
    struct Camera
    {
        DirectX::XMFLOAT4X4 viewProj;     // jittered
        DirectX::XMFLOAT4X4 invViewProj;  // jittered inverse
        DirectX::XMFLOAT3   cameraPos;
        float               nearZ;
        float               farZ;
    };

    struct alignas(16) SSRTraceCB
    {
        float    viewProj[16];
        float    invViewProj[16];
        float    cameraPos[3];        float    nearZ;
        // renderW/H = GBuffer dims; traceW/H = dispatch dims (currently full-res).
        uint32_t renderW;             uint32_t renderH;
        uint32_t hizMipCount;         float    maxRayLength;
        float    farZ;                float    roughnessCutoff;
        uint32_t frameIndex;          uint32_t _pad1;
        float    traceThickness;      uint32_t hizMostDetailedLvl;
        float    coneMipMax;          float    depthBiasFactor;
        uint32_t finishLinearSteps;   uint32_t traceW;
        uint32_t traceH;              uint32_t _pad2[1];
        uint32_t skyRadianceMips;     uint32_t hasSkyRadiance;
        float    roughFadeStart;      float    _pad3;
    };

    void Init(IGraphicsDevice& gfx);
    // Hot-reload entry point: re-fetches the trace CS from disk and rebuilds
    // the compute PSO. Idempotent; safe to call from a filesystem watcher.
    void ReloadShaders(IGraphicsDevice& gfx);

    // (Re)allocate output textures. @p renderW/@p renderH are FULL-resolution
    // dims; internal trace textures currently match them.
    // Frees the textures (deferred); the chain that owns them is going away (a released view's).
    void ReleaseTextures();
    void EnsureTexture(uint32_t renderW, uint32_t renderH);

    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    void SetHiZMipCount(uint32_t m)   { m_hizMipCount = m; }

    void Execute(RHI::CommandList cl,
                 uint64_t normalSrv,
                 uint64_t surfaceSrv,
                 uint64_t depthSrv,
                 uint64_t depthHierSrv,
                 uint64_t hdrPyramidSrv,
                 uint64_t velocitySrv,
                 uint64_t skyRadianceSrv,
                 uint32_t skyRadianceMips,
                 bool hasSkyRadiance);

    uint64_t GetResultSrv()     const;
    uint64_t GetRayDirPDFSrv()  const;
    uint64_t GetRayLengthSrv()  const;
    uint64_t GetRayClassSrv()   const;

    const RHI::Texture* GetResultTexture()    const { return &m_resultTex; }
    const RHI::Texture* GetRayDirPDFTexture() const { return &m_rayDirPDFTex; }
    const RHI::Texture* GetRayLengthTexture() const { return &m_rayLengthTex; }
    const RHI::Texture* GetRayClassTexture() const { return &m_rayClassTex; }

    void SetResultState(RHI::ResourceState s)    { m_resultState    = s; }
    void SetRayDirPDFState(RHI::ResourceState s) { m_rayDirPDFState = s; }
    void SetRayLengthState(RHI::ResourceState s) { m_rayLengthState = s; }
    void SetRayClassState(RHI::ResourceState s) { m_rayClassState = s; }
    void SetAllOutputsState(RHI::ResourceState s)
    { m_resultState = m_rayDirPDFState = m_rayLengthState = m_rayClassState = s; }
    RHI::ResourceState GetResultState() const { return m_resultState; }

    uint32_t GetWidth()        const { return m_w; }
    uint32_t GetHeight()       const { return m_h; }
    uint32_t GetRenderWidth()  const { return m_renderW; }
    uint32_t GetRenderHeight() const { return m_renderH; }

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    RHI::Texture       m_resultTex;     // RGBA16F: hit color + confidence
    RHI::Texture       m_rayDirPDFTex;  // RGBA16F: world L + PDF
    RHI::Texture       m_rayLengthTex;  // R16F: world ray distance
    RHI::Texture       m_rayClassTex;   // R8_UINT: stable SSR_* classification
    RHI::ResourceState m_resultState     = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayDirPDFState  = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayLengthState  = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_rayClassState   = RHI::ResourceState::UNORDERED_ACCESS;
    uint32_t           m_w = 0, m_h = 0;          // trace dispatch dimensions
    uint32_t           m_renderW = 0, m_renderH = 0;

    FrameCB<SSRTraceCB> m_cb;

    Camera   m_cam{};
    uint32_t m_frameIndex   = 0;
    uint32_t m_hizMipCount  = 1;

    // Used ONLY on the LinearFinishTrace fall-through path (budget-exhausted rays that
    // found no clean crossing); a successful finish-trace hit skips the check, its snap is
    // sub-pixel-precise. 0.5 wu is a safe fall-through default: tighter values reject
    // legitimate grazing hits the forward walk failed to converge on within 16 steps. The
    // shader also floors the tolerance at 0.4% of the hit's linear depth: a fixed 0.5 wu at
    // km range sits below D24 quantization + grazing-terrain depth gradients and speckles
    // distant water reflections with sky-fallback holes.
    float    m_traceThickness     = 0.5f;
    uint32_t m_hizMostDetailed    = 0;
    float    m_coneMipMax         = 4.0f;
    // 0: a depth-PROPORTIONAL ray-origin push visibly offsets reflections near contact
    // lines (0.005 ~ 25 cm at 50 m opens water/terrain seam gaps under vertical camera
    // motion). The shader keeps a 0.02 wu absolute floor PLUS a D24-quantization-aware
    // floor (2 quantization steps at the origin's linear depth: sub-mm near, ~1 wu at
    // 900 m) so far surfaces still clear the Hi-Z self-intersection.
    float    m_depthBiasFactor    = 0.0f;
    float    m_maxRayLength       = 100.0f;
    float    m_roughnessCutoff    = 0.7f;
    // The finish trace refines from the Hi-Z hit position with maxSteps/2 backward then
    // maxSteps forward 1-pixel walks. Hi-Z lands within ~1 cell of the true crossing, so 16
    // is plenty; raise it only if Hi-Z is suspected to overshoot more than that.
    uint32_t m_finishLinearSteps  = 16;
};
