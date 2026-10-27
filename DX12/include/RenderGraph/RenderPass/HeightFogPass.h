#pragma once

// HeightFogPass - UE-style analytic exponential height fog.
//
// One fullscreen ONE / INV_SRC_ALPHA draw between CloudPass and VideoPass: HDR then
// holds lit geometry (+ aerial perspective), skybox, water and clouds, so all of
// them - the sky at the horizon included - get fogged; the froxel
// VolumetricFogPass later composes its near-range detail OVER this far fog, and
// video overlays stay unfogged.
//
// Authored on HeightFogComponent (Sky entity); Renderer_IBL's SyncSkyboxIBL
// pushes it here each frame. The shader reads the sun direction / colour straight
// from LightCB. Bindings: LightCB at b1, this frame's HeightFogConstants at b2 (a
// block of the frame's constants), depth at t5 space0.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "ECS/HeightFogComponent.h"

#include <vector>

class HeightFogPass : public RG::RenderPass
{
public:
    // HLSL CB mirror - keep aligned with HeightFogApply.ps.hlsl (3 rows).
    struct alignas(16) HeightFogConstants
    {
        float fogDensity;            // row 0
        float fogHeightFalloff;
        float fogHeight;
        float startDistance;

        float fogColor[3];           // row 1
        float maxOpacity;

        float sunInscatterIntensity; // row 2
        float anisotropy;
        float _pad0;
        float _pad1;
    };
    static_assert(sizeof(HeightFogConstants) == 48, "HeightFogCB layout drift");

    explicit HeightFogPass(RG::RGTextureHandle depth) : m_depth(depth) {}

    const char* GetName() const override { return "HeightFogPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's two early-outs. The constants are built for this pass's
    // own draw on every frame that draws, so skipping leaves nothing stale.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_enabled && Shown() && m_gfx != nullptr);
    }
    // A view that hides the height fog (RenderGraph/SceneView.h) - the main view in
    // wireframe, a debug view mode, an ortho camera, the path tracer's camera
    // primary - skips it and its forward fog.
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::HeightFog); }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Renderer-pushed per-frame state ----
    // It records any view: the camera is the bound view's LightCB, the fog the scene's.
    uint32_t HandledEvents() const override { return kViewSafe; }

    void SetEnabled(bool on)             { m_enabled = on; }
    bool IsEnabled() const               { return m_enabled; }
    void SetParams(const HeightFogComponent& c) { m_params = c; }
    const HeightFogComponent& GetParams() const { return m_params; }
    // The froxel fog holds this fog to @p metres along each camera ray (IFroxelHeightFog): the
    // composite and its forward fog start no nearer. 0: the component's own start.
    void SetStartFloor(float metres) { m_startFloor = metres; }

    // The constants Execute composites with this frame, for the surfaces drawn
    // AFTER it (Renderer::WriteFogApplyCB -> fog_apply.hlsli). False when Execute
    // adds no fog: disabled, not shown by the view (Shown()), or a density the shader
    // returns zero for. Render half, after the frame's RenderGraph::SetView.
    bool ForwardConstants(HeightFogConstants& out) const;

private:
    // Execute's constants and ForwardConstants' - one function, one clamp.
    HeightFogConstants BuildConstants() const;

    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    bool m_enabled        = false;

    HeightFogComponent m_params;
    float m_startFloor = 0.0f;   // SetStartFloor
};
