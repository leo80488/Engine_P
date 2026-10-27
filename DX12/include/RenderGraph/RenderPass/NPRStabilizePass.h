#pragma once

// NPRStabilizePass - HDR appearance protection before AA.
//
// Lighting.ps owns the energy contract. This pass runs after every graphics contribution but
// before TAA/FSR and applies one exposure-space, hue-preserving shoulder + soft absolute
// ceiling to the character-authored part of each NPR pixel (opaque lighting, GBuffer-seeded
// emissive, clouds/fog/video and the face overlay), read from the NPRLitSnapshot copy taken
// right after FaceOverlayPass. Effects composited after that point (transparents, particles,
// trails, tracers, billboards) pass through with a coverage-scaled share of the correction
// instead of being clamped by the material underneath. Exposure is read from the GPU
// exposure buffer.

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's targets)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

class IGraphicsDevice;
class TransientAliasArena;

class NPRStabilizePass : public RG::PassLifecycle
{
public:
    struct ServiceRequest { uint32_t w = 0, h = 0; };

    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // RG::PrepareStage FrameLate: the lit-scene snapshot (the NPRLitSnapshot lambda and the
    // post-chain wiring read the ready flag it arms). PostEffects: the working set, after Bloom
    // (which publishes the group-D base this pass places on).
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.stage == RG::PrepareStage::FrameLate) PrepareLitSnapshot(*c.gfx);
        else                                        PrepareResources(c.renderW, c.renderH);
    }

    // The service point: a group-D resident at the device's render size.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override { PrepareResources(m_request.w, m_request.h); }
    // The events Renderer::PassSet delivers to this pass; it records any view (its targets are the view's).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    // The main view keeps its output and lit snapshot below; a secondary view its own in its ViewState.
    // Every call after this one is that view's.
    void BindView(ViewState& view) override;
    void PrepareResources(uint32_t w, uint32_t h);

    // Shared transient-alias heap (group D's late joiner, see TransientAliasArena.h); null keeps
    // the output committed. Set once by the Renderer at construction; placement happens only on
    // the main thread inside PrepareResources.
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena = arena; }

    void SetInputs(uint64_t hdrSrv, uint64_t normalSrv, uint64_t depthSrv,
                   uint64_t materialSrv, uint64_t exposureSrv,
                   uint64_t litSnapshotSrv)
    {
        m_hdrSrv      = hdrSrv;
        m_normalSrv   = normalSrv;
        m_depthSrv    = depthSrv;
        m_materialSrv = materialSrv;
        m_exposureSrv = exposureSrv;
        m_litSrv      = litSnapshotSrv;
    }

    // ---- Lit-scene snapshot ----
    // The NPR treatment applies only to the character-authored part of the pixel: clamping the
    // full HDR would ceiling-clamp a bright beam inside an NPR character's silhouette.
    // PrepareLitSnapshot runs on the main thread before the graph executes (creates / resizes the
    // committed snapshot, arms the ready flag). CopyLitSnapshot is recorded by the NPRLitSnapshot
    // graph pass (a worker) right after FaceOverlayPass, via CopyHdrSceneToScoped: the overlay is
    // character-authored and must be inside the treated image. GetLitSnapshotSrv returns 0 unless
    // this frame's snapshot was armed, so the shader falls back to full-pixel treatment for at
    // most the one frame after a resize.
    void PrepareLitSnapshot(IGraphicsDevice& gfx);
    void CopyLitSnapshot(IGraphicsDevice& gfx, RHI::CommandList cl);
    // CopyLitSnapshot's early-out predicate, so the NPRLitSnapshot graph pass can skip itself
    // instead of opening a command list for a no-op. PrepareLitSnapshot sets the flag on the main
    // thread before the graph executes, so it is settled by Phase A.
    bool WantsLitSnapshot() const { return m_t->litSnapshotReady && m_t->litSnapshot.IsValid(); }
    uint64_t GetLitSnapshotSrv() const;
    void SetSceneMaxBrightness(float value) { m_sceneMaxBrightness = value; }
    void SetManualExposure(float value, bool enabled)
    {
        m_manualExposure    = value;
        m_useManualExposure = enabled;
    }

    // True only when the output texture was written for this frame.
    bool Execute(RHI::CommandList cl);

    uint64_t GetOutputSrvHandle() const;
    const RHI::Texture& GetOutputTexture() const { return m_t->output; }

private:
    struct alignas(16) StabilizeCB
    {
        uint32_t width;
        uint32_t height;
        float    sceneMaxBrightness;
        float    exposureProtectionLimit;

        float    manualExposure;
        uint32_t useManualExposure;
        uint32_t useLitSnapshot;
        float    _pad0;
    };

    // One view's output and lit-scene snapshot.
    struct Target
    {
        RHI::Texture       output;
        RHI::ResourceState outputState = RHI::ResourceState::UNORDERED_ACCESS;
        // Arena placement state (the main view's only). placed drives the per-frame aliasing barrier;
        // the epoch AND base stamps drive re-placement (the base moves when Bloom's footprint changes,
        // which FSR toggling can do without changing our size).
        bool     placed      = false;
        uint32_t placedEpoch = 0;
        uint64_t placedBase  = 0;
        uint32_t width  = 0;
        uint32_t height = 0;
        bool     dirty  = true;

        // Lit-scene snapshot. Committed: it is written on the graphics queue inside the graph and
        // read on the async-compute post chain, so it must not live in a per-CL transient alias
        // group. litSnapshotReady is written only on the main thread in PrepareLitSnapshot before
        // the graph workers fork, then read by the copy lambda and the input wiring (single writer
        // before the fork, no race). litSnapshotState is touched only inside CopyLitSnapshot (one
        // graph pass per frame).
        RHI::Texture       litSnapshot;
        RHI::ResourceState litSnapshotState = RHI::ResourceState::COPY_DST;
        uint32_t           litW = 0;
        uint32_t           litH = 0;
        bool               litSnapshotReady = false;
    };
    struct ViewTarget : ViewData
    {
        Target target;
        void Release(IGraphicsDevice& gfx) override;
    };

    // allowPlaced is opt-in: Execute's lazy safety rebuild runs on RenderWorker_Compute and must
    // never create a placed resource (TransientAliasArena rule 3), so it takes the default and
    // gets a committed texture. Only the main view's output is ever placed.
    void RebuildTexture(bool allowPlaced = false);
    bool CreatePipelines(IGraphicsDevice& gfx);

    ServiceRequest m_request;   // the size OnService prepares

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<StabilizeCB> m_cb;

    // The main view's targets; m_t is the bound view's (BindView).
    Target  m_main;
    Target* m_t = &m_main;

    TransientAliasArena* m_aliasArena  = nullptr;

    uint64_t m_hdrSrv = 0;
    uint64_t m_normalSrv = 0;
    uint64_t m_depthSrv = 0;
    uint64_t m_materialSrv = 0;
    uint64_t m_exposureSrv = 0;
    uint64_t m_litSrv = 0;

    float m_sceneMaxBrightness = 0.0f;
    float m_manualExposure = 1.0f;
    bool  m_useManualExposure = false;
};
