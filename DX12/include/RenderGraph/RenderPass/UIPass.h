#pragma once

// UIPass - uploads a UIDrawList and renders it on top of the composition buffer
// (post-tonemap). Owned by the Renderer (not a RG::RenderPass); runs after every
// graph pass, before the swap-chain composite. Like DebugWirePass: dedicated CL,
// dependency-chained to the previous pass, target SR -> RT, draw, back to SR.
//
// Bindings (engine default root signature):
//   b1 space0 - UICB (canvas pixel size, UI clock)
//   b2 space0 - SDF text effects table
//   t2 space0 - vertex ByteAddressBuffer (UPLOAD, one per frame in flight)
//   t3 space0 - the draw command's texture SRV (or 1x1 white)
//   g_UISmp   - the command's UI sampler (clamp/wrap/mirror x linear/point)

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"
#include "UI/UIDrawList.h"

#include <cstdint>
#include <vector>

class IGraphicsDevice;

class UIPass : public RG::PassLifecycle
{
public:
    // Buffer caps (the buffer is refilled from the CPU each frame, so it is sized once).
    // kMaxIndices is the real cap and the only one Execute() enforces: indices are
    // flat-expanded into the vertex ring (the PVF root signature has no IA index
    // buffer), the ring holds kMaxIndices UIVertex, and expansion stops there.
    // kMaxVertices is not a limit - nothing tests it; the source-vertex ceiling is
    // UIDrawList::kMaxSourceVertices (65536, the uint16 index range). It is only the
    // figure the init log reports: do not add a check against it.
    static constexpr uint32_t kMaxVertices = 32768;
    static constexpr uint32_t kMaxIndices  = 65535;

    void Init(IGraphicsDevice& gfx);

    // The CPU-side draw list. UICanvasSystem populates this each frame.
    UI::UIDrawList& GetDrawList()             { return m_drawList; }
    const UI::UIDrawList& GetDrawList() const { return m_drawList; }
    // View @p viewSlot's HUD list: GetDrawList for the main view (0), else the secondary view's,
    // painted in the UI paint window and drawn in that view's overlay stage.
    UI::UIDrawList& ViewDrawList(uint32_t viewSlot)
    { return viewSlot == 0 || viewSlot >= RHI::kMaxViews ? m_drawList : m_viewLists[viewSlot]; }
    const UI::UIDrawList& ViewDrawList(uint32_t viewSlot) const
    { return viewSlot == 0 || viewSlot >= RHI::kMaxViews ? m_drawList : m_viewLists[viewSlot]; }
    // A secondary view's vertex ring, made on its first HUD (the UI paint window: the render lane
    // is idle). Execute draws nothing for a view without one.
    void EnsureViewRing(uint32_t viewSlot);

    // Renders into @p target, the composition buffer (created with the RENDER_TARGET
    // bind flag; its format follows the HDR output mode, see m_targetFormat).
    // @p entryState is its current tracked state: UIPass transitions entryState ->
    // RENDERTARGET, draws, then RT -> SR, so on return the target is in SHADER_RESOURCE
    // (the caller updates its own tracker). canvasW / canvasH are the target's pixel
    // size (not the swap chain: pass the editor viewport size). The caller spawns and
    // dependency-chains the CL. @p timeSec feeds UICB.g_uiTimeSec for animated custom
    // text shaders. Returns whether it drew: on false no barrier was issued and the
    // target is still in @p entryState (disabled, empty list or canvas, no pipeline or
    // mapped vertex buffer, no vertices).
    bool Execute(RHI::CommandList cl,
                 const RHI::Texture* target,
                 RHI::ResourceState entryState,
                 uint32_t canvasW, uint32_t canvasH,
                 float timeSec = 0.f)
    {
        return Execute(cl, target, entryState, canvasW, canvasH, timeSec, m_drawList);
    }

    // The same, drawing @p list instead of this pass's own list (the UI Editor's view
    // service keeps one list per view). Into the vertex ring of the view that records it.
    bool Execute(RHI::CommandList cl,
                 const RHI::Texture* target,
                 RHI::ResourceState entryState,
                 uint32_t canvasW, uint32_t canvasH,
                 float timeSec,
                 const UI::UIDrawList& list);

    // Hot-reload entry: clears the shader and PSO caches; the next Execute rebuilds.
    void ReloadShaders();
    void ReloadShaders(IGraphicsDevice&) override { ReloadShaders(); }
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events Renderer::PassSet delivers

    // Format of the Execute() target. Push ToneMapPass::GetFinalOutputFormat() before
    // Execute (see m_targetFormat for why a literal will not do).
    void SetTargetFormat(RHI::Format f) { m_targetFormat = f; }

    // Prefix of this instance's diagnostic log lines ("UIPass.Execute: ...").
    // A second instance (the UI Editor's view service) names itself so its lines
    // are not read as the game screen's. Must outlive the pass (a literal).
    void SetLogName(const char* name) { m_logName = name ? name : "UIPass"; }

private:
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct alignas(16) UICB { float w, h; float timeSec; uint32_t pad1; };

    static constexpr uint32_t kMaxEffectSlots = 64;
    struct alignas(16) UIEffectsTable { UI::GpuTextEffect fx[kMaxEffectSlots]; };

    PSODesc BuildPSODesc() const;
    bool    CreateWhiteTexture();

public:
    bool enabled = true;

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Custom text-shader slot -> dynamic shader id in this m_shaderLib, filled on first
    // use. Per instance: the ids index this library's own registry, so a cache shared
    // between two UIPass instances would hand one the other's ids.
    std::vector<uint32_t> m_slotDynIds;

    // Command count of the last Execute; gates the one-line-per-change diagnostic.
    // Per instance for the same reason as m_slotDynIds.
    size_t           m_lastCmdCount = SIZE_MAX;
    const char*      m_logName      = "UIPass";

    UI::UIDrawList   m_drawList;
    UI::UIDrawList   m_viewLists[RHI::kMaxViews];   // [0] unused: the main view's is m_drawList

    RHI::GPUBuffer   m_vertexBuffer[kFrameCount][RHI::kMaxViews];
    void*            m_vbMapped[kFrameCount][RHI::kMaxViews] = {};
    // Vertex UPLOAD buffers, one per frame in flight and view (a secondary view's from its
    // first HUD). m_indexBuffer / m_ibMapped are
    // unused: indices are flat-expanded on the CPU into the VB (the engine root sig has
    // no IA index-buffer slot); they are kept for a possible IA index path.
    RHI::GPUBuffer   m_indexBuffer;
    void*            m_ibMapped = nullptr;

    // Per-pass UI CB (canvas size + UI clock for custom text shaders).
    // Mirrors cbuffer UICB in UI.vs.hlsl / shaders/TextFX.hlsli.
    FrameCB<UICB>    m_cb;

    // SDF text effects table, uploaded from UIDrawList::Effects() each frame and bound
    // at b2 space0. Indexed by the draw command's effectIndex (the PS reads it from the
    // b0 root constants). Element 0 is the no-op effect.
    FrameCB<UIEffectsTable> m_effectsCB;

    // 1x1 white default texture (used when a draw cmd has no texture bound).
    RHI::Texture     m_whiteTex;
    uint64_t         m_whiteTexSrv = 0;

    // UV samplers, indexed by UI::UISamplerId(wrap, pointFilter):
    //   [0..2] linear Clamp/Wrap/Mirror, [3..5] point Clamp/Wrap/Mirror.
    // Set per draw command as the dynamic sampler (g_UISmp). SDF text always uses [0]
    // (clamp-linear).
    int              m_samplers[UI::kUISamplerCount] = { -1, -1, -1, -1, -1, -1 };

    // RTVFormats[0] for the composition buffer this pass blends into. That buffer
    // follows the HDR output mode (RGBA8_UNORM in SDR, FP16 in HDR10 / scRGB;
    // HDRCompositionFormat, Graphics/HDROutput.h), and a hardcoded format here is a
    // debug-layer error and undefined output with no compile error. The Renderer pushes
    // the live format via SetTargetFormat before Execute; PSODesc::Hash mixes
    // rtvFormats, so the SDR and HDR variants coexist in the PSO cache.
    // The custom-text-shader PSOs copy BuildPSODesc() and override only psID, so they
    // get this format too - which is why the composition space stays extended-gamma
    // rather than linear nits (game-asset shaders cannot be reached into).
    RHI::Format      m_targetFormat = RHI::Format::R8G8B8A8_UNORM;
};
