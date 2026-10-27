#pragma once

// HairShadowMarkPass - bangs -> face SCREEN-SPACE shadow mark (DanbaidongRP's
// "CharacterFringe" technique). Re-draws the DrawPacket::hairShadowCaster packets
// offset in view space along the light's screen direction plus a small depth push
// (FringeShadowMark.vs.hlsl) and stamps extra.g = 1 into the GBuffer extra target
// (RT4) where the fringe's shadow falls. Lighting.ps's toon FACE branch folds
// extra.g into the SDF face terminator: a crisp, always-contact bangs shadow with
// no shadow-map cost.
//
// Runs right after the GBuffer producers (GBuffer / Terrain / Grass), while the
// extra target is still an RTV and the depth a DSV:
// - depth test GREATER_EQUAL (reversed-Z), no depth write: the pushed proxy marks
//   only surfaces it still floats in front of;
// - stencil EQUAL 2 (NPR), read-only: marks only NPR character pixels (other
//   shading models may use RT4, e.g. SSS);
// - colour write masked to RT4.G: the face's extra.r / extra.a survive.
//
// Alternative / complement: the fitted bangs shadow MAP (the Hair variant of
// PerObjectShadowPass at t43). HairShadowCasterComponent.mode picks the source;
// Lighting.ps takes the min of the active ones.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's camera)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class HairShadowMarkPass : public RG::RenderPass
{
public:
    HairShadowMarkPass(RG::RGTextureHandle extraTarget, RG::RGTextureHandle depth)
        : m_extraH(extraTarget), m_depthH(depth) {}
    ~HairShadowMarkPass();

    const char* GetName() const override { return "HairShadowMarkPass"; }
    void Setup (RG::RenderGraphBuilder& b) override;
    void Init  (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's only early-out (`if (!m_enabled) return cl;`); nothing runs
    // before it, so the skip changes nothing observable.
    bool IsActive() const override
    { return !RG::PassActivityGatesEnabled() || m_enabled; }
    // It records any view: a secondary view's camera and the offsets that follow from it are its own
    // (SetViewCamera, its segment), the rest is the main view's frame.
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame BEFORE graph Execute (UploadFrameData).
    // Enabled == a tagged bangs caster exists, the component mode includes the
    // screen mark, and the sun is valid.
    void SetEnabled(bool e) { m_enabled = e; }
    // viewProj = RAW (untransposed) JITTERED camera VP; offsets are view-space
    // metres, depthPush is metres along the camera ray (all precomputed by the
    // Renderer from component params + light/camera state + caster world scale).
    void SetFrameParams(const DirectX::XMFLOAT4X4& viewProj,
                        const DirectX::XMFLOAT3& camRight,
                        const DirectX::XMFLOAT3& camUp,
                        const DirectX::XMFLOAT3& camPos,
                        const DirectX::XMFLOAT3& camFwd,
                        float offsetX, float offsetY, float depthPush,
                        uint32_t dissolveNoiseIdx = ~0u,
                        uint32_t instanceRemapIdx = 0)
    {
        m_pendingVP = viewProj;
        m_camRight = camRight; m_camUp = camUp; m_camPos = camPos; m_camFwd = camFwd;
        m_offsetX = offsetX; m_offsetY = offsetY; m_depthPush = depthPush;
        m_dissolveNoiseIdx = dissolveNoiseIdx;
        m_instanceRemapIdx = instanceRemapIdx;
    }
    // What a secondary view's offsets are computed from (the main view's frame, beside SetFrameParams):
    // the unit light direction, the head's centre and the component's offset factors and world scale.
    void SetViewInputs(const DirectX::XMFLOAT3& lightDir, const DirectX::XMFLOAT3& headCenter,
                       float offsetX, float offsetY, float worldScale)
    {
        m_lightDir = lightDir; m_headCenter = headCenter;
        m_offsetXIn = offsetX; m_offsetYIn = offsetY; m_worldScale = worldScale;
    }
    // A secondary view's camera (its segment): @p viewProj is its jittered VP, as SetFrameParams'.
    void SetViewCamera(ViewState& view, const DirectX::XMFLOAT4X4& viewProj,
                       const DirectX::XMFLOAT4X4& viewMatrix, const DirectX::XMFLOAT3& camPos);

private:
    static constexpr uint32_t kCBStride = RHI::kConstantBufferAlignment;
    struct CBSlot { uint8_t bytes[kCBStride]; };

    // A view's camera and the offsets that depend on it.
    struct Camera
    {
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT3   camRight{ 1, 0, 0 };
        DirectX::XMFLOAT3   camUp   { 0, 1, 0 };
        DirectX::XMFLOAT3   camPos  { 0, 0, 0 };
        DirectX::XMFLOAT3   camFwd  { 0, 0, 1 };
        float               offsetX = 0.0f;
        float               offsetY = 0.0f;
    };
    struct ViewCamera : ViewData
    {
        Camera camera;
        void Release(IGraphicsDevice&) override {}
    };

    void UploadCB();

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RG::RGTextureHandle m_extraH;   // GBuffer RT4 "extra" (gExtraGBuffer)
    RG::RGTextureHandle m_depthH;   // GBuffer depth (read-only test + stencil)

    // 256B triple-buffered CB (FringeMarkCB in FringeShadowMark.vs.hlsl).
    FrameCB<CBSlot> m_cb;

    DirectX::XMFLOAT4X4 m_pendingVP{};
    DirectX::XMFLOAT3   m_camRight{ 1, 0, 0 };
    DirectX::XMFLOAT3   m_camUp   { 0, 1, 0 };
    DirectX::XMFLOAT3   m_camPos  { 0, 0, 0 };
    DirectX::XMFLOAT3   m_camFwd  { 0, 0, 1 };
    float m_offsetX   = 0.0f;
    float m_offsetY   = 0.0f;
    float m_depthPush = 0.005f;
    // Dissolve noise bindless index (FringeMarkCB byte 124; ~0u = none).
    uint32_t m_dissolveNoiseIdx = ~0u;
    // Instance-remap heap index (FringeMarkCB byte 128; 0 = identity).
    uint32_t m_instanceRemapIdx = 0;
    bool  m_enabled   = false;

    // SetViewInputs'; the bound secondary view's camera (BindView; null for the main view, whose is above).
    DirectX::XMFLOAT3 m_lightDir  { 0, -1, 0 };
    DirectX::XMFLOAT3 m_headCenter{ 0, 0, 0 };
    float             m_offsetXIn  = 0.0f;
    float             m_offsetYIn  = 0.0f;
    float             m_worldScale = 1.0f;
    const Camera*     m_viewCamera = nullptr;

    IGraphicsDevice* m_gfxPtr = nullptr;
};
