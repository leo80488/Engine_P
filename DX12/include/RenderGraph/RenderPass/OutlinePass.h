#pragma once

// OutlinePass - multi-technique outline for entities with the OUTLINE material flag.
//
// A pre-TAA composite registered after TransparentPass. Outline pixels are written into the HDR
// scene colour at the same jittered NDC as the GBuffer raster, so TAA reprojects and
// history-blends mesh and outline together and the outline never desyncs from the mesh during
// animation. The cost: outline edges share TAA's ~0.5 px softening with the mesh (symmetric, so
// it does not read as a gap).
//
// Execute runs three sub-passes in order:
//   1. Inverted hull (graphics): DrawFilter::Custom packets with front-face culling + clip-space
//      normal extrusion, a pixel-consistent silhouette in HDR. Skipped while OutlineGeometryPass
//      draws the hull (hullSuppressed).
//   2. Object ID (graphics): the same packets into a private R32_UINT texture with back-face
//      culling + depth test; each pixel gets meshDescIdx + 1.
//   3. Screen-space composite (fullscreen triangle, alpha blend): a Roberts cross edge detector
//      over the object ID, GBuffer normal and depth; the edges, alpha-blended onto HDR, draw
//      inner creases and inter-object boundaries.
//
// Resource states: HDR scene RENDER_TARGET on entry (left by TransparentPass) and exit; normal
// SHADER_RESOURCE (LightingPass's ReadSRV, kept by Setup's). Depth: the graph holds the depth
// plane in DEPTH_READ_SRV with the stencil plane writable; under -rgplaneoff it is DEPTHSTENCIL
// on entry and exit (Execute samples it in SHADER_RESOURCE for sub-pass 3, then restores it).

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's camera and target)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class OutlinePass : public RG::RenderPass
{
public:
    OutlinePass(RG::RGTextureHandle depth, RG::RGTextureHandle normal);
    ~OutlinePass();

    const char* GetName() const override { return "OutlinePass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's first early-out (draws.empty()); OutlineGeometryPass::IsActive explains why
    // the pushed count is a safe upper bound. A skipped pass also skips its Setup-time depth
    // declaration (the graph emits a node's entry transitions only when it runs it), so depth can
    // leave the graph in DEPTH_READ_SRV. That is safe: every post-graph depth consumer reads the
    // graph's tracked state (GetTextureState) before transitioning, so nothing downstream assumes
    // DEPTHSTENCIL.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled() || m_customDrawCount != 0;
    }
    void SetCustomDrawCount(int32_t n) { m_customDrawCount = n; }
    // Whether the bound view draws the editor's selection (picking) outline: its EditorOverlays flag.
    void SetPickingOutlineShown(bool shown) { m_pickingOutlineShown = shown; }

    // It records any view: the main view reads the public camera fields below (the Renderer writes them on
    // the game half), a secondary view its own (SetViewCamera, its segment) and its own object-ID target.
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    void SetViewCamera(ViewState& view, float viewNearZ, float viewFarZ, uint32_t viewDisplayW,
                       uint32_t viewDisplayH);
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override
    { return { &m_hullPsoCache, &m_objectIdPsoCache,
               &m_pickingObjectIdPsoCache, &m_screenSpacePsoCache }; }

private:
    struct alignas(16) OutlineCBData
    {
        float    outlinePixels       = 2.0f;
        float    depthThreshold      = 0.0008f;
        float    normalThreshold     = 0.3f;
        float    outlineStrength     = 1.0f;
        float    outlineColor[3]     = {};
        float    outlineFadeStart    = 15.0f;
        // Named for what the shader does with them: the pixel space outlinePixels is measured in.
        // Display extents, not the rasterized viewport; they differ under FSR2/DLSS. Only
        // InvertedHull.vs reads them; the screen-space PS declares them only to keep the b2 layout.
        uint32_t outlinePxSpaceW     = 0;
        uint32_t outlinePxSpaceH     = 0;
        float    outlineFadeEnd      = 40.0f;
        float    nearZ               = 0.1f;
        float    farZ                = 1000.0f;
        // Picking-outline colour, used by sub-pass 3 where the ObjectID has bit 31 set (written by the
        // picking ObjectID PSO).
        float    pickingOutlineColor[3] = {};
    };

    // `mirrored` = this draw's instances have a negative-determinant world matrix, so the
    // front-face convention flips with them (see GBufferPass::BuildPSODesc). Without it the
    // inverted hull culls the wrong side (the outline vanishes or floods the object) and the
    // ObjectID pass writes back-face IDs.
    PSODesc BuildHullPSODesc(bool mirrored = false)            const;
    PSODesc BuildObjectIdPSODesc(bool mirrored = false)        const;
    PSODesc BuildPickingObjectIdPSODesc(bool mirrored = false) const; // depth test off -> silhouette through walls
    PSODesc BuildScreenSpacePSODesc()      const;

    // One view's object-ID target and what it was built for.
    struct ObjectId
    {
        RHI::Texture       tex;
        RHI::ResourceState state = RHI::ResourceState::UNDEFINED;
        uint32_t           w = 0;
        uint32_t           h = 0;
    };
    // A secondary view's camera fields (the main view's are the public members).
    struct ViewCamera
    {
        float    nearZ = 0.1f;
        float    farZ  = 1000.0f;
        uint32_t displayWidth  = 0;
        uint32_t displayHeight = 0;
    };
    struct ViewOutline : ViewData
    {
        ObjectId   objectId;
        ViewCamera camera;
        void Release(IGraphicsDevice& gfx) override;
    };

    void RebuildObjectIdTexture(IGraphicsDevice& gfx, uint32_t w, uint32_t h);

public:
    // ---- Per-frame settings ----
    float outlinePixels   = 2.0f;
    float outlineColor[3] = { 0.0f, 0.0f, 0.0f };
    float depthThreshold  = 0.05f;   // relative linear-depth ratio
    float normalThreshold = 0.3f;
    float outlineStrength = 1.0f;
    float outlineFadeStart = 15.0f;  // distance where outline starts fading
    float outlineFadeEnd   = 40.0f;  // distance where outline fully disappears
    float nearZ            = 0.1f;   // set by Renderer each frame
    float farZ             = 1000.f; // set by Renderer each frame

    // DISPLAY (post-upscale) extents, pushed every frame by the Renderer: the space outlinePixels
    // is measured in for the inverted-hull sub-pass (see OutlineGeometryPass::displayWidth).
    // 0 = not pushed yet; Execute falls back to the render extents.
    uint32_t displayWidth  = 0;
    uint32_t displayHeight = 0;

    // Editor selection outline colour. Sub-pass 3 draws it only for pixels whose ObjectID has bit
    // 31 set; sub-pass 2 sets that bit for picking-outline draws (rendered with depthEnable = false,
    // so the silhouette shows through walls).
    float pickingOutlineColor[3] = { 0.95f, 0.80f, 0.10f }; // Blender-ish yellow

    // Diagnostic toggles (the editor inspector): skip sub-passes to isolate which one causes an
    // artifact such as outline lag during character motion.
    bool diagSkipHull        = false;   // true -> skip sub-pass 1
    bool diagSkipScreenSpace = false;   // true -> skip sub-passes 2 and 3

    // Set by the Renderer while OutlineGeometryPass draws the hull (the same shell, so exactly one
    // of them may run). Separate from diagSkipHull on purpose: that one is the editor's diagnostic
    // checkbox, and with one field the Renderer and the UI would overwrite each other.
    bool hullSuppressed      = false;

    // false -> a plain inverted hull: the hull sub-pass stops stamping kOutlineStencilBit, so the
    // rim is an ordinary depth-tested draw with no identity downstream. The tag exists only so TAA
    // can treat rim pixels differently; every such TAA path is gated on the bit, so this one switch
    // turns them all off. The Renderer clears it while OutlineGeometryPass is enabled.
    bool tagStencilForTAA    = true;

private:
    // DrawFilter::Custom packet count, pushed by the Renderer each frame before graph Execute.
    // -1 = never pushed -> active (see IsActive).
    int32_t m_customDrawCount = -1;
    bool    m_pickingOutlineShown = true;   // SetPickingOutlineShown

    // ---- Graph handles ----
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_normal;

    // ---- Shaders + PSO caches ----
    ShaderLibrary m_shaderLib;
    PSOCache      m_hullPsoCache;
    PSOCache      m_objectIdPsoCache;
    PSOCache      m_pickingObjectIdPsoCache; // depth-test off (x-ray silhouette)
    PSOCache      m_screenSpacePsoCache;

    // ---- ObjectID private render target (R32_UINT) ----
    // RHI::CreateTexture creates its RTV (RENDER_TARGET bind flag).
    // The main view's target; m_id is the bound view's, m_viewCamera a secondary view's camera (null for
    // the main view's).
    ObjectId             m_mainObjectId;
    ObjectId*            m_id         = &m_mainObjectId;
    const ViewCamera*    m_viewCamera = nullptr;

    // ---- OutlineCB ----
    FrameCB<OutlineCBData> m_outlineCB;

    IGraphicsDevice* m_gfxPtr = nullptr;
};
