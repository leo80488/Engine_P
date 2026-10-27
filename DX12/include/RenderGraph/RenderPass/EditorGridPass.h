#pragma once

// EditorGridPass - editor-only infinite reference grid on the world XZ plane.
//
// One fullscreen ONE/INV_SRC_ALPHA draw between WaterPass and CloudPass: analytic
// ray-plane intersection per pixel (no geometry, truly infinite), AA'd with
// screen-space derivatives, manually depth-tested against the scene depth SRV (never
// writes depth). Clouds/fog composite over it and transparents draw on top, so it
// reads as part of the scene.
//
// Disabled by default; EditorLayer pushes enable + cell size (the editor "unit"
// selector) every frame via Renderer::GetEditorGridPass(). Game builds never enable
// it, so the pass early-outs at zero cost.
//
// Slots as HeightFogPass's: LightCB at b1, this frame's GridConstants at b2 (a block
// of the frame's constants), depth at t5 space0.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

#include <vector>

class EditorGridPass : public RG::RenderPass
{
public:
    // HLSL CB mirror: keep aligned with EditorGrid.ps.hlsl (5 rows).
    struct alignas(16) GridConstants
    {
        float cellSize;         // row 0
        float majorEvery;
        float gridY;
        float fadeDistance;

        float minorColor[4];    // rows 1-4: rgb + peak opacity
        float majorColor[4];
        float axisXColor[4];
        float axisZColor[4];
    };
    static_assert(sizeof(GridConstants) == 80, "EditorGridCB layout drift");

    explicit EditorGridPass(RG::RGTextureHandle depth) : m_depth(depth) {}

    const char* GetName() const override { return "EditorGridPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's early-out.
    bool IsActive() const override;
    // It records any view that shows the editor's overlays: its camera is the bound view's LightCB.
    uint32_t HandledEvents() const override { return kViewSafe; }
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::EditorOverlays); }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Editor-pushed per-frame state ----
    void SetEnabled(bool on)       { m_enabled = on; }
    bool IsEnabled() const         { return m_enabled; }
    // Metres per minor grid cell (the editor unit selector); major lines every 10 cells.
    void SetCellSize(float metres) { m_cellSize = metres; }
    float GetCellSize() const      { return m_cellSize; }

private:
    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    bool  m_enabled  = false;
    float m_cellSize = 1.0f;
};
