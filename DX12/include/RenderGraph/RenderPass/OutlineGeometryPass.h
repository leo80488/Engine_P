#pragma once

// OutlineGeometryPass - inverted-hull outline drawn as real geometry.
//
// Runs right after GBufferPass, before lighting, and writes the same 6-MRT GBuffer, so the rim
// gets depth and velocity like any other surface and nothing downstream has to know it exists.
// OutlinePass's own inverted-hull sub-pass composites the rim into HDR after lighting, colour
// only: with no velocity TAA cannot reproject the rim, and with no depth its disocclusion
// detector cannot see the rim leave a texel, which leaves an outline-shaped trail.
//
// Unlit without a new shading model: RT5 is the HdrSceneColor emissive seed and emissive never
// passes through the BRDF, so albedo = 0 and reflectance = 0 leave the pixel exactly
// outlineColor (see OutlineGeometry.ps.hlsl).
//
// Distance fade shrinks the extrusion width instead of fading alpha: a GBuffer has no blending,
// and a thin line reads better than a translucent one. It also keeps the depth write safe: at
// the authored 1-2 px widths the silhouette error every later depth reader sees stays sub-pixel.
//
// Resource states on entry (left by GBufferPass): the five colour MRTs + HDR in RENDER_TARGET,
// depth in DEPTHSTENCIL. Unchanged on exit.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's camera)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class OutlineGeometryPass : public RG::RenderPass
{
public:
    OutlineGeometryPass(RG::RGTextureHandle albedo,
                        RG::RGTextureHandle normal,
                        RG::RGTextureHandle surface,
                        RG::RGTextureHandle depth,
                        RG::RGTextureHandle velocity,
                        RG::RGTextureHandle emissive);

    const char* GetName() const override { return "OutlineGeometryPass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's first two early-outs. m_customDrawCount is pushed by
    // the Renderer each frame from the packet set (DrawFilter::Custom); the
    // count is a full-scan equality count while GetDrawList returns only the
    // FIRST contiguous run of that filter, so it is an upper bound - it can
    // leave the pass active with an empty list, never the reverse. -1 means
    // "nobody pushed one" (a non-Renderer host) and keeps the pass active.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (enabled && m_customDrawCount != 0);
    }
    void SetCustomDrawCount(int32_t n) { m_customDrawCount = n; }

    // It records any view: the main view reads the public camera fields below (the Renderer writes them on
    // the game half), a secondary view its own (SetViewCamera, its segment).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    void SetViewCamera(ViewState& view, float viewNearZ, float viewFarZ, uint32_t viewDisplayW,
                       uint32_t viewDisplayH);

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Builds now, on the calling thread (a preload's TaskSystem worker), the fixed outline PSO the
    // first material-outline packet would build (Execute: BuildPSODesc(false)).
    void WarmPipelines();

private:
    struct OutlineCBData
    {
        float    outlinePixels;
        float    depthThreshold;
        float    normalThreshold;
        float    outlineStrength;
        float    outlineColor[3];
        float    outlineFadeStart;
        // Named for what the shader does with them: the pixel space outlinePixels is measured in.
        // Display extents, not the rasterized viewport; they differ under FSR2/DLSS.
        uint32_t outlinePxSpaceW;
        uint32_t outlinePxSpaceH;
        float    outlineFadeEnd;
        float    nearZ;
        float    farZ;
        float    _pad;
    };

    PSODesc BuildPSODesc(bool mirrored) const;

    // A secondary view's camera fields (the main view's are the public members).
    struct ViewCamera : ViewData
    {
        float    nearZ = 0.1f;
        float    farZ  = 1000.0f;
        uint32_t displayWidth  = 0;
        uint32_t displayHeight = 0;
        void Release(IGraphicsDevice&) override {}
    };

public:
    // Master switch. When false this pass draws nothing and OutlinePass's own inverted-hull
    // sub-pass draws the rim instead; exactly one of them may draw it (the Renderer keeps them in
    // sync).
    bool enabled = true;

    // Mirrors OutlinePass's tunables; both read the same OutlineCB layout so the
    // rim looks identical whichever pass draws it.
    float outlinePixels   = 2.0f;
    float outlineColor[3] = { 0.0f, 0.0f, 0.0f };
    float outlineFadeStart = 30.0f;
    float outlineFadeEnd   = 60.0f;
    float nearZ = 0.1f;
    float farZ  = 1000.0f;

    // DISPLAY (post-upscale) extents, pushed every frame by the Renderer, the only place that knows
    // both resolutions. outlinePixels is measured in this space: extruding by render pixels would
    // let FSR2/DLSS multiply the width by the upscale ratio (a 2 px outline 4 px thick at
    // Performance). 0 = not pushed yet; Execute falls back to the render extents, which is correct
    // at native resolution.
    uint32_t displayWidth  = 0;
    uint32_t displayHeight = 0;

private:
    // DrawFilter::Custom packet count, pushed by the Renderer each frame before graph Execute
    // (Renderer::Render_BindFrameResources). -1 = never pushed (see IsActive).
    int32_t m_customDrawCount = -1;
    const ViewCamera* m_viewCamera = nullptr;   // the bound secondary view's; null for the main view

    RG::RGTextureHandle m_albedo, m_normal, m_surface, m_depth, m_velocity, m_emissive;

    IGraphicsDevice* m_gfxPtr = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Mirrors OutlinePass::OutlineCBData through farZ: the shaders share the b2 block.
    FrameCB<OutlineCBData> m_outlineCB;
};
