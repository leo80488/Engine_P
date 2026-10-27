#pragma once

// ShadowSystem - CPU-side cascaded shadow map (CSM) math: cascade splits, per-cascade light
// view-projection matrices, texel-grid stabilization. Owns no GPU resources: ShadowPass owns the
// Texture2DArray, the per-cascade upload CBs, the indirect buffer, the PSO and barrier state.
// Renderer::UploadFrameData calls Update once per frame, before any consumer reads the shadow
// state, and copies CascadeMatricesTransposed(), CascadeSplits() etc. into LightCB when
// HasValidLight(). The System owns data and logic, its paired Pass the rendering (as with
// particles and trails).

#include <DirectXMath.h>
#include <cstdint>

class IGraphicsDevice;

class ShadowSystem
{
public:
    static constexpr int   kCascadeCount = 4;
    static constexpr float kLambda       = 0.85f;
    // Cascades 0..2 are the "near" cascades: a practical log + uniform split between the camera
    // near plane and FrameInput::shadowDistance (the player's shadows.distance, whose default is
    // kShadowFarCap).
    static constexpr float kShadowFarCap = 400.0f;
    // Cascade 3 is an "ultra-far" cascade for terrain self-shadow (mountain silhouettes onto the
    // plain at sunrise / sunset, distant peaks shadowing each other): from where cascade 2 ends out
    // to kFarCascadeFar in view-Z. Raise it for bigger worlds.
    static constexpr float kFarCascadeFar = 2000.0f;
    static constexpr float kPullBack     = 300.0f;

    struct FrameInput
    {
        DirectX::XMMATRIX view;
        DirectX::XMFLOAT3 cameraForward;
        DirectX::XMFLOAT3 lightDir;        // caller pulls from LightCB
        float             nearZ  = 0.1f;
        float             farZ   = 1000.0f;
        float             fov    = DirectX::XM_PIDIV4;
        float             aspect = 1.0f;
        uint32_t          shadowMapSize = 4096;
        float             shadowDistance = kShadowFarCap;   // the near cascades' reach (m)
        // Ortho editor views: cascade sub-frusta become ortho boxes (fov unused).
        bool              orthographic    = false;
        float             orthoHalfHeight = 10.0f;
    };

    void Init(IGraphicsDevice& /*gfx*/) {}         // reserved hook; holds no GPU state
    void Update(const FrameInput& in);

    const float*                CascadeSplits()             const { return m_splits; }
    const DirectX::XMFLOAT4X4*  CascadeMatricesTransposed() const { return m_matricesForLightCB; }
    const DirectX::XMFLOAT4X4*  CascadeMatricesForPass()    const { return m_matricesForPass; }
    // World-space metres per shadow-map texel for each cascade. Consumers scale normal / receiver
    // bias by it so both look the same in every cascade, whatever its physical extent.
    const float*                CascadeTexelWorldSize()     const { return m_texelWorld; }
    uint32_t                    ShadowFrameIndex()          const { return m_shadowFrameIndex; }
    float                       ShadowBlendRange()          const { return m_shadowBlendRange; }
    bool                        HasValidLight()             const { return m_hasLight; }

    static constexpr float ShadowBias()     { return 0.0003f; }
    static constexpr float ShadowStrength() { return 1.0f; }
    // Receiver-side normal offset, in shadow-map texels of world; 0 disables. 1.5 pushes a
    // grazing-angle receiver about one and a half texels along its normal before the light-space
    // sample: enough to clear quantization acne without a visible peter-pan gap on most geometry.
    // Lighting.ps reads it as LightCB.shadowNormalOffset.
    static constexpr float ShadowNormalOffset() { return 1.5f; }

private:
    float               m_splits[kCascadeCount]             {};
    float               m_texelWorld[kCascadeCount]         {};
    DirectX::XMFLOAT4X4 m_matricesForLightCB[kCascadeCount] {};
    DirectX::XMFLOAT4X4 m_matricesForPass   [kCascadeCount] {};
    uint32_t            m_shadowFrameIndex = 0;
    float               m_shadowBlendRange = 0.0f;
    bool                m_hasLight         = false;
};
