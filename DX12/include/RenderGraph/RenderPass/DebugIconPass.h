#pragma once

// DebugIconPass - editor-only billboard icons (light / camera / audio gizmos), the
// "Billboards" debug-draw bucket: a debug overlay drawn AFTER tonemap, separate from
// the gameplay scene draw (GBuffer / Transparent) and from the gameplay WorldUI pass.
//
// Borrows the WorldUI billboard shaders (WorldUI.vs/ps.hlsl), their 28-byte vertex
// layout and the engine-shared root slots (vertex SRV t2 space0, bindless texture
// table t0 space2). Its data is an immediate AddIcon() queue the editor's
// DebugDrawSystem fills each frame, not ECS components. Nothing is submitted in Game
// builds (DebugDrawSystem is editor-gated), so the queue stays empty and Execute()
// early-outs.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"

#include <DirectXMath.h>
#include <vector>
#include <cstdint>

class IGraphicsDevice;

class DebugIconPass : public RG::PassLifecycle
{
public:
    // 6 verts/icon -> ~1365 icons; Execute drops the rest silently.
    static constexpr uint32_t kMaxVertices = 8192;

    void Init(IGraphicsDevice& gfx);
    void ReloadShaders();
    void ReloadShaders(IGraphicsDevice&) override { ReloadShaders(); }
    uint32_t HandledEvents() const override { return kReloadShaders; }

    // Per-frame immediate API. DebugDrawSystem clears then fills this each
    // editor frame (after Renderer::BeginFrame, before Renderer::Render).
    void ClearIcons() { m_icons.clear(); }

    // @p worldPos: icon centre in world space. @p halfSize: half-extent of the
    // camera-facing quad - a screen fraction while constantScreenSize, else world units.
    // @p texIdx: bindless index into the engine t0 space2 table (a texture's bindless
    // index == its handleId, see Renderer_Scene.cpp). @p color: 0xAABBGGRR multiply tint
    // (0xFFFFFFFF = unmodified).
    void AddIcon(const DirectX::XMFLOAT3& worldPos, float halfSize,
                 uint32_t texIdx, uint32_t color = 0xFFFFFFFFu);

    bool HasIcons() const { return !m_icons.empty(); }

    // Draws the queued icons into @p target (the composition buffer, RENDER_TARGET bind
    // flag) like WorldUIBillboardPass::Execute: @p entryState -> RENDERTARGET, draw,
    // -> SHADER_RESOURCE.
    void Execute(RHI::CommandList            cl,
                 const DirectX::XMFLOAT4X4&  viewProjMatrix,
                 const DirectX::XMFLOAT4X4&  viewMatrix,
                 const RHI::Texture*         target,
                 RHI::ResourceState          entryState,
                 uint32_t                    canvasW,
                 uint32_t                    canvasH);

    // Format of that target: push ToneMapPass::GetFinalOutputFormat() before Execute
    // (m_targetFormat says why a literal will not do).
    void SetTargetFormat(RHI::Format f) { m_targetFormat = f; }

private:
    // Must match the WorldUI vertex layout the shared shaders read.
    struct IconVertex
    {
        float    pos[3];
        float    uv[2];
        uint32_t col;
        uint32_t texIdx;
    };
    static_assert(sizeof(IconVertex) == 28, "IconVertex must match WorldUIVertex (28 bytes)");

    struct IconReq
    {
        DirectX::XMFLOAT3 pos;
        float             halfSize;
        uint32_t          texIdx;
        uint32_t          col;
    };

    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;          // == GraphicsDX12::kFrameCount

    PSODesc BuildPSODesc() const;

public:
    bool enabled = true;
    float maxDistance = 50.0f;

    // Constant screen-size billboards (editor gizmo convention): the world-space quad
    // scales with camera distance so icons keep their on-screen size near or far. When
    // true, AddIcon's `halfSize` is a SCREEN FRACTION (world half = clampedDistance x
    // halfSize); when false, a fixed world half-extent.
    bool  constantScreenSize = true;

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // One ring per view: Execute writes the camera-facing quads of the view that records it.
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount][RHI::kMaxViews];
    void*            m_vbMapped[kFrameCount][RHI::kMaxViews] = {};

    FrameCB<DirectX::XMFLOAT4X4> m_cb;                  // un-jittered viewProj (b1)

    std::vector<IconReq> m_icons;

    // RTVFormats[0] for the composition buffer this pass blends into. That buffer
    // follows the HDR output mode (RGBA8_UNORM in SDR, FP16 in HDR10 / scRGB:
    // HDRCompositionFormat, Graphics/HDROutput.h) and the RTV silently inherits the
    // texture's real format, so a hardcoded one is a debug-layer error and undefined
    // output with no compile error. The Renderer pushes the live format via
    // SetTargetFormat before Execute. PSODesc::Hash mixes rtvFormats, so the SDR and HDR
    // variants coexist in the PSO cache instead of evicting each other on a mode change.
    RHI::Format      m_targetFormat = RHI::Format::R8G8B8A8_UNORM;
};
