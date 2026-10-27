#pragma once

// WorldUIBillboardPass - 3D billboarded UI (HP bars, nameplates, damage numbers,
// world markers) drawn on top of the tonemapped scene.
//
// Reads: entities with WorldSpaceUIComponent + content (WorldUIBar / WorldUIText /
// WorldUIImage / DamageNumber), in BuildLane; the camera right / up vectors, derived
// on the CPU from the view each frame.
// Writes: the composition buffer (SR -> RT -> SR), with its own un-jittered viewProj
// at b1.
//
// Owned by the Renderer (NOT a graph pass), run after ToneMap and before UIPass: world
// UI sits above the 3D scene but below the screen-space HUD.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"
#include "UI/UIDrawList.h"   // GpuTextEffect (shared 96B effects-table entry)

#include <DirectXMath.h>
#include <vector>

class IGraphicsDevice;
class World;
namespace UI { class Font; }

class WorldUIBillboardPass : public RG::PassLifecycle
{
public:
    static constexpr uint32_t kMaxVertices = 65536;

    void Init(IGraphicsDevice& gfx);

    // ---- Build (game side) and draw (render side) ----
    // BuildLane runs on the GAME side and is the only half that reads the World: every
    // visible world-space UI entity becomes lane vertices. It MUST be called after
    // WorldSpaceUISystem::Tick - see the definition. @p timeSec feeds the b1 CB clock
    // (jitter is CPU-side; custom world text shaders read g_wuTimeSec). @p viewSlot: whose
    // lane (0 = the main view; a secondary view's reads its per-view fade / scale / cull and
    // is drawn in its segment).
    void BuildLane(class World& world, uint32_t viewSlot,
                   const DirectX::XMFLOAT4X4& viewProjMatrix,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   uint32_t canvasW, uint32_t canvasH,
                   float timeSec);

    // Execute runs on the RENDER side, reads only the recording view's lane and draws it into @p target:
    // the composition buffer (RENDER_TARGET bind flag; its format follows the HDR output
    // mode, see m_targetFormat). @p entryState is the texture's tracked state; the pass
    // leaves it in SHADER_RESOURCE and the caller updates its own tracker.
    void Execute(RHI::CommandList cl,
                 const RHI::Texture* target,
                 RHI::ResourceState entryState,
                 uint32_t canvasW, uint32_t canvasH);

    void ReloadShaders();
    void ReloadShaders(IGraphicsDevice&) override { ReloadShaders(); }
    uint32_t HandledEvents() const override { return kReloadShaders; }   // lifecycle events (Renderer::PassSet)

    // Format of the Execute() target. Push ToneMapPass::GetFinalOutputFormat() before
    // Execute - see m_targetFormat for why a literal will not do.
    void SetTargetFormat(RHI::Format f) { m_targetFormat = f; }

    uint32_t LastVertexCount() const { return m_lastVertexCount; }

private:
    // 28-byte vertex - pos(12) + uv(8) + col(4) + texIdx(4).
    // texIdx bit layout (mirrored in WorldUI.ps.hlsl / WorldTextFX.hlsli):
    //   [31]     SDF-glyph flag (the page's red channel is a distance field)
    //   [30..24] effects-table index (b2, 0..63 - outline / glow / custom params)
    //   [23..0]  bindless index into t0 space2 (the engine cap, 16384, fits)
    //   0xFFFFFFFF = bar / border sentinel (the PS short-circuits to vertex colour).
    struct WorldUIVertex
    {
        float    pos[3];
        float    uv[2];
        uint32_t col;
        uint32_t texIdx;
    };
    static_assert(sizeof(WorldUIVertex) == 28, "WorldUIVertex must be 28 bytes");

    static constexpr uint32_t kInvalidTexIdx = 0xFFFFFFFFu;
    static constexpr uint32_t kEffectShift   = 24;

    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct alignas(16) WorldUICB
    {
        DirectX::XMFLOAT4X4 viewProj;   // transposed on upload (HLSL column-major)
        float               timeSec = 0.f;
        float               pad[3]  = {};
    };

    static constexpr uint32_t kMaxEffectSlots = 64;
    struct alignas(16) WorldUIEffectsTable { UI::GpuTextEffect fx[kMaxEffectSlots]; };

    PSODesc BuildPSODesc() const;

    // Push a billboarded quad with a bindless texIdx (kInvalidTexIdx for
    // bar/border verts so the PS short-circuits to vertex colour).
    // A secondary view's vertex ring, made on its first lane (the game half: the render lane is idle).
    void EnsureViewRing(uint32_t viewSlot);

    static void EmitQuad(std::vector<WorldUIVertex>& out,
                         const DirectX::XMFLOAT3& worldCenter,
                         const DirectX::XMFLOAT3& cameraRightWS,
                         const DirectX::XMFLOAT3& cameraUpWS,
                         float halfW, float halfH,
                         float u0, float v0, float u1, float v1,
                         uint32_t col, uint32_t texIdx);

public:
    bool enabled = true;

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Custom text-shader slot -> dynamic shader id in THIS m_shaderLib, filled on first
    // use. Per instance: the ids index this library's own registry, so a cache shared
    // between instances would hand one the ids of another library (as in UIPass).
    std::vector<uint32_t> m_slotDynIds;

    // Per-frame UPLOAD-heap vertex buffer, one per frame in flight and view (a secondary view's
    // from its first lane).
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount][RHI::kMaxViews];
    void*            m_vbMapped[kFrameCount][RHI::kMaxViews] = {};

    // CB at b1: un-jittered viewProj (so world UI doesn't dance with TAA
    // jitter) + the UI clock for custom text shaders. Mirrors the cbuffer in
    // WorldUI.vs.hlsl / WorldTextFX.hlsli.
    FrameCB<WorldUICB> m_cb;

    // Text effects table at b2 - the same 96 B GpuTextEffect entries as UIPass
    // (outline / glow widths + colours, customA / B params, runRect). Rebuilt each frame
    // from the visible world texts; slot 0 = no-op.
    FrameCB<WorldUIEffectsTable> m_effectsCB;

    // RTVFormats[0] for the composition buffer this pass blends into. That buffer follows
    // the HDR output mode - RGBA8_UNORM in SDR, FP16 in HDR10 / scRGB
    // (HDRCompositionFormat, Graphics/HDROutput.h) - and the RTV inherits whatever the
    // texture is, so a hardcoded format is a debug-layer error and undefined output with
    // no compile error. The Renderer pushes the live format via SetTargetFormat before
    // Execute. PSODesc::Hash mixes rtvFormats, so the SDR and HDR variants coexist in the
    // PSO cache instead of evicting each other on every mode change.
    RHI::Format      m_targetFormat = RHI::Format::R8G8B8A8_UNORM;

    // ---- The extracted lanes, one per view ----
    // Written by BuildLane on the game side, read by Execute on the render side - two
    // threads, so members, not locals or thread_locals. Capacity is kept across frames
    // (clear(), never shrink), so the steady state does not allocate.
    struct Lane
    {
        std::vector<WorldUIVertex>                                   verts;
        std::vector<std::pair<uint32_t, std::vector<WorldUIVertex>>> buckets;
        std::vector<UI::GpuTextEffect>                               effects;
        uint32_t                                                     bucketVertCount = 0;
        // The extract frame's matrix and clock, carried so the draw half uses the
        // frame the lane was built for rather than whatever is current.
        DirectX::XMFLOAT4X4                                          viewProj{};
        float                                                        timeSec = 0.0f;
    };
    Lane m_lanes[RHI::kMaxViews];

    // Vertices produced by the main view's last build (see FrameStats::worldUIVerts).
    uint32_t m_lastVertexCount = 0;
};
