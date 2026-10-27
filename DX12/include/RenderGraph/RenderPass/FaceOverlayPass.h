#pragma once

// FaceOverlayPass - brows/lashes visible THROUGH the hair (the DanbaidongRP
// "CharacterEyelashOutter" technique). Re-draws the packets tagged
// DrawPacket::faceOverlay AFTER opaque lighting, alpha-blended over the HDR scene with
// NO depth test; the PS (FaceOverlay.ps.hlsl) limits the redraw to pixels whose
// visible surface is HAIR (the extra.b mask the hair GBuffer PS writes), fades by the
// lash -> hair depth gap (~5 cm max, which also hides the far side of the head) and
// fades out when the camera leaves the front of the face.
//
// Registered right before TransparentPass (real transparents still draw over the
// overlay). Depth is bound read-only like TransparentPass, so the PS can Load() it for
// the gap fade at the same time.
//
// Tag the eyebrow / eyelash entities with FaceOverlayComponent; the meshes keep their
// normal GBuffer draw (visible when not covered) - this pass only adds the
// see-through redraw where hair covers them.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class FaceOverlayPass : public RG::RenderPass
{
public:
    FaceOverlayPass(RG::RGTextureHandle extraTarget, RG::RGTextureHandle depth)
        : m_extraH(extraTarget), m_depthH(depth) {}
    ~FaceOverlayPass();

    const char* GetName() const override { return "FaceOverlayPass"; }
    void Setup (RG::RenderGraphBuilder& b) override;
    void Init  (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's only early-out (`if (!m_enabled) return cl;`).
    bool IsActive() const override
    { return !RG::PassActivityGatesEnabled() || m_enabled; }
    // It records any view: its camera, depth, extra GBuffer and cascades are the bound view's.
    uint32_t HandledEvents() const override { return kViewSafe; }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame BEFORE graph Execute.
    void SetEnabled(bool e) { m_enabled = e; }

    // CSM cascade array SRV (t9 space0), the same handle LightingPass gets
    // (Gfx::ShadowSRV, already in the shared graphics signature). 0 is legal: Execute()
    // binds a 1x1 fully-lit stand-in rather than leaving a sampled table unbound.
    void SetShadowMap(uint64_t srv) { m_shadowSrvHandle = srv; }

    // The face's indirect sources: the sky irradiance cube (t6) and sky SH (t19)
    // LightingPass gets - real or the shared black fallback, never 0 in a frame that
    // renders. DDGI needs no binding (ResourceDescriptorHeap via LightCB). No
    // root-signature cost, like the lit particles' copies.
    void SetIBL(uint64_t irradiance) { m_iblIrradianceHandle = irradiance; }
    void SetSkySH(uint64_t skySH)    { m_skySHHandle = skySH; }

private:
    static constexpr uint32_t kCBStride = RHI::kConstantBufferAlignment;
    struct CBSlot { uint8_t bytes[kCBStride]; };

    void UploadCB(uint32_t rtW, uint32_t rtH);

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RG::RGTextureHandle m_extraH;   // GBuffer RT4 "extra" (extra.b = hair mask), SRV
    RG::RGTextureHandle m_depthH;   // GBuffer depth, read-only DSV + SRV

    FrameCB<CBSlot> m_cb;

    bool  m_enabled     = false;

    uint64_t m_iblIrradianceHandle = 0;   // t6
    uint64_t m_skySHHandle         = 0;   // t19

    // CSM array (t9): the overlay's direct sun term is gated by
    // ComputeShadowFactorExternalOnly (ungated, a brow drawn through hair gets 55-100% of
    // full sunlight whatever occludes it). Falls back to a 1x1 fully-lit array so the
    // table is never left unbound under a shader that samples it.
    uint64_t     m_shadowSrvHandle      = 0;
    RHI::Texture m_shadowFallbackTex;
    uint64_t     m_shadowFallbackHandle = 0;

    IGraphicsDevice* m_gfxPtr = nullptr;
};
