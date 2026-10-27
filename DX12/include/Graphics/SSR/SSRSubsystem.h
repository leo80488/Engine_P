#pragma once

// SSRSubsystem - single owner of the Hi-Z SSR pipeline (trace / resolve / temporal /
// upsample / composite / depth hierarchy / scene-color pyramid) and of its barrier and
// command-list orchestration:
//
//   Init()       - create all sub-passes
//   OnResize()   - propagate render-target dimensions
//   Render()     - submit the trace chain (Renderer phase 4.6) + composite (phase 4.7),
//                  chaining off the caller's last graph command list.
//
// The per-pass classes are private implementation details. Callers use the
// subsystem-level resize, render and output-SRV contracts; the temporal history resets
// through the view (SSRTemporalPass::History).

#include "Graphics/GraphicsStruct.h"
#include "RenderGraph/RenderGraph.h"

#include <memory>
#include <cstdint>
#include <vector>
#include <utility>
#include <wrl.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;
class GraphicsDX12;
class SSRPass;
class SSRResolvePass;
class SSRTemporalPass;
class SSRUpsamplePass;
class SSRCompositePass;
class SSRDepthHierarchyPass;
class SceneColorPyramidPass;

class SSRSubsystem
{
public:
    // Per-frame inputs gathered by Renderer. Plain POD, no resource ownership transfer.
    struct FrameContext
    {
        RG::RenderGraph*       graph         = nullptr;

        // GBuffer attachments (RG-managed). Render reads their state with
        // graph->GetTextureState and transitions them back to it after use, so downstream
        // passes still see the state they expect.
        RG::RGTextureHandle    albedoHandle{};
        RG::RGTextureHandle    normalHandle{};
        RG::RGTextureHandle    surfaceHandle{};
        RG::RGTextureHandle    depthHandle{};
        RG::RGTextureHandle    velocityHandle{};
        // Optional grass-free depth snapshot (copied before GrassPass). When valid, the Hi-Z
        // march pyramid is built from THIS depth so thin grass blades don't block reflection
        // rays; per-pixel reads (trace origins, finish refinement, resolve / temporal /
        // upsample bilateral) keep using depthHandle. Invalid -> the pyramid uses depthHandle.
        RG::RGTextureHandle    traceDepthHandle{};
        // Lighting's opaque scene-color MRT with emissive but without the same-frame traced
        // reflection (t44). The preferred source for the history pyramid; invalid falls back to
        // final HDR.
        RG::RGTextureHandle    reflectionSourceHandle{};

        // Jittered camera state. invViewProj is derived internally.
        DirectX::XMFLOAT4X4    viewProj{};
        DirectX::XMFLOAT4X4    prevViewProjJittered{};
        DirectX::XMFLOAT3      cameraPos{};
        float                  nearZ         = 0.0f;
        float                  farZ          = 0.0f;

        // BRDF LUT for the composite's Fresnel x envBRDF.
        uint64_t               brdfLutSrv    = 0;

        // Sky-hit radiance for the legacy chain's trace. The renderer always supplies a valid
        // cube SRV; hasSkyRadiance tells a real sky from the black fallback cube.
        uint64_t               skyRadianceSrv = 0;
        uint32_t               skyRadianceMips = 1;
        bool                   hasSkyRadiance = false;

        // RTReflectionPass's denoised output (.a = conf), non-zero only in FullAdditive mode
        // with the unified pass active. The composite then scales its additive term by
        // (1 - conf) so opaque pixels Lighting.ps already folded are not added twice.
        uint64_t               unifiedConfSrv = 0;

        // The render frame (FrameView::currentFrame): the temporal pass restarts a history
        // that skipped frames.
        uint64_t               renderFrame = 0;
    };

    // How much of the legacy chain runs next to the unified hybrid reflections.
    enum class LegacyMode
    {
        // Snapshot + scene-color pyramid only, every reflection frame: the pyramid feeds the
        // NEXT frame's unified trace in RTReflectionPass (its screen tier and its RT
        // screen-radiance reuse).
        PyramidOnly,
        // + depth-hier / trace / resolve / temporal / upsample / composite, for water frames:
        // WaterPass stamps its GBuffer normal/roughness AFTER the unified pass ran, so water
        // still needs the late screen-space chain. The composite is modulated by
        // ctx.unifiedConfSrv.
        FullAdditive,
        // + everything EXCEPT the composite: the fallback when the unified pass is unavailable
        // (no DXR / init failure). Renderer binds the upsample output as LightingPass's RT
        // reflection input (t44), so the same single fold applies one frame latent; color and
        // conf are latent together, so there is no split-frame blend.
        FullFold,
    };

    SSRSubsystem();
    ~SSRSubsystem();

    SSRSubsystem(const SSRSubsystem&)            = delete;
    SSRSubsystem& operator=(const SSRSubsystem&) = delete;

    // Create + Init all sub-passes. Call once during Renderer::Compile().
    void Init(IGraphicsDevice& gfx);

    // Hot-reload entry point: re-fetches every SSR compute shader from disk and rebuilds
    // the PSOs. Wipes temporal history so the first post-reload frame does not blend stale
    // accumulator state.
    void ReloadShaders(IGraphicsDevice& gfx);

    // The temporal pass, whose history lives in the view: the Renderer registers it with
    // PassSet::RegisterView, and a hard cut clears the accumulator through the view.
    RG::PassLifecycle* TemporalPass() const;

    // (Re)allocates every internal texture for the new render resolution. Safe to call
    // every frame; the sub-passes early-out if the dimensions are unchanged. Returns the
    // final color SRV (upsample output, or the raw trace before the upsample exists). It
    // can change on any call, so the caller re-derives LightCB.ssrResultIdx from it every
    // frame.
    uint64_t OnResize(uint32_t renderW, uint32_t renderH);
    // Frees every internal texture (deferred): a released view's chain, before it is retired. The alias
    // heap stays until the chain is destroyed, after the frames that may read its placed textures.
    void ReleaseTextures();

    // Last-built HDR scene-color pyramid, the texture SSRTrace cone-samples, parked in
    // SHADER_RESOURCE outside the build (SceneColorPyramidPass state contract).
    // RTReflectionTrace reuses it for hit-point screen radiance so the RT tier matches
    // SSR's color by construction. Returns 0 until the first SSR frame has built the chain.
    uint64_t GetSceneColorPyramidSrv() const;
    uint32_t GetSceneColorPyramidMips() const;
    // The textures behind GetSceneColorPyramidSrv and GetFinalColorSrv, for the main
    // graph's History imports (RG::PassTex::kSSRScenePyramid / kSSRResult). Null before
    // Init.
    const RHI::Texture* SceneColorPyramidTexture() const;
    const RHI::Texture* FinalColorTexture() const;

    // Submits the legacy chain per @p mode (see LegacyMode). Both internal command lists
    // depend on @p colorLastCL.
    //
    // @p depthLastCL is the caller's running post-graph GBuffer_Depth chain (see the
    // depthLastCL block above Phase 4.5 in Renderer::RenderAsync). Both internal lists
    // hand-roll a GBuffer_Depth round trip whose StateBefore is the graph tracker as it
    // reads at record time, so they must be ordered behind whatever last moved that
    // tracker; Hi-Z rewrites it to DEPTHSTENCIL just before this call. IN/OUT: advanced to
    // the last list that really touched depth, which in PyramidOnly mode is neither.
    //
    // Returns the last list it recorded (invalid when it recorded none). Every one of them
    // moves the device's HDR tracker (the snapshot copy and the end state, PyramidOnly
    // included), so a later list that transitions HDR off that tracker (the probe-debug
    // list, preComputeCL) must be ordered behind it: the caller makes it the new
    // colorLastCL. Without that edge only EndFrame's pool-index tie-break orders SSR.trace
    // -> PreCompute in PyramidOnly mode.
    RHI::CommandList Render(IGraphicsDevice& gfx,
                            const FrameContext& ctx,
                            RHI::CommandList colorLastCL,
                            RHI::CommandList& depthLastCL,
                            LegacyMode mode = LegacyMode::FullAdditive);

    // Final = upsample output (raw trace before the upsample exists). LightingPass binds it
    // in FullFold mode; Water.ps reads it through LightCB.ssrResultIdx.
    uint64_t GetFinalColorSrv() const;

private:
    // ---- Transient scratch aliasing ----
    // The resolve snapshot and color (both full-res RGBA16F, never alive at the same time)
    // share one DEFAULT heap via CreatePlacedResource + aliasing barriers, saving one
    // full-res RGBA16F (8 bytes per pixel: ~16 MB at a 1080p render res). Validate any
    // change to it with the D3D12 debug layer + GPU validation: the aliasing contract (an
    // aliasing barrier on each owner switch, tracked state kept, content re-initialized by
    // a full write; see Render) is checked there at runtime, and a mistake is a GPU hang or
    // corruption, not a compile error.
    static constexpr bool kSSREnableScratchAliasing = true;

    // Ensures m_aliasHeap fits one full-res RGBA16F texture (snapshot and color are both
    // placed at offset 0: same size, lifetime-disjoint). Returns true if usable. Recreates
    // it on a resolution change, retiring the old heap.
    bool EnsureAliasHeap(GraphicsDX12& gfx, uint32_t w, uint32_t h);
    // Decrement retired-heap countdowns; release those whose placed resources
    // have drained. Call once per Render.
    void TickAliasHeap();

    std::unique_ptr<SSRPass>               m_trace;
    std::unique_ptr<SSRResolvePass>        m_resolve;
    std::unique_ptr<SSRTemporalPass>       m_temporal;
    std::unique_ptr<SSRUpsamplePass>       m_upsample;
    std::unique_ptr<SSRCompositePass>      m_composite;
    std::unique_ptr<SSRDepthHierarchyPass> m_depthHier;
    std::unique_ptr<SceneColorPyramidPass> m_sceneColorPyr;

    uint32_t m_frameIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12Heap> m_aliasHeap;
    uint64_t m_aliasHeapSize = 0;
    uint32_t m_aliasHeapW = 0, m_aliasHeapH = 0;
    // Which placed resource is live in the alias heap. PyramidOnly frames
    // leave the snapshot active (the resolve color is only re-activated by a
    // legacy-mode resolve dispatch); the aliasing barrier is emitted only on
    // an actual owner switch.
    bool m_snapshotActive = false;
    // Heaps retired on resolution change, freed only after their placed
    // resources' kFrameCount-deep deferred release has drained (countdown).
    std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Heap>, uint32_t>> m_retiredHeaps;
};
