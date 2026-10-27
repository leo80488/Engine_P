#pragma once

// TextureViewService - the Texture Viewer's pictures on the renderer side
// (Renderer/TextureView.h). Owned by the Renderer (Renderer::GetTextureViews()),
// EngineCore, usable from either exe; nothing is built until a view is first asked
// for.
//
// Three contexts, three kinds of access:
//   * GAME half / UI handler / Lua (main thread): Acquire, Request, Release - each
//     only latches an intent under the mutex; Srv / Output / GetStatus read. Nothing
//     here touches what the render half reads.
//   * SERIAL WINDOW (ServiceFrameBoundary, from Renderer::ServiceEnableGatedResources -
//     render lane parked): applies the latched intents, creates / resizes / destroys
//     the output targets (DestroyTexture is frame-stamped, so a descriptor an earlier
//     ImGui::Image captured outlives its draw data), builds the pipeline on first use,
//     and marks the views to draw.
//   * RENDER half (Execute, restoreCL, Phase 7): one dispatch per marked view (at most
//     TextureViewPass::kMaxPerFrame a frame; a new target always draws in the frame it
//     was created, so ImGui never samples an unwritten one).
//
// GPU order: ImGui records into primaryCL, which App makes depend on the renderer's
// last list (restoreCL), so a dispatch recorded here is complete before the same
// frame's ImGui::Image samples the target.

#include "Renderer/TextureView.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class IGraphicsDevice;
class TextureViewPass;

class TextureViewService
{
public:
    static constexpr std::uint32_t kMaxViews  = 64;
    static constexpr std::uint32_t kInvalidId = ~0u;
    static constexpr std::uint32_t kMaxSize   = 4096;   // per output edge

    struct ProbeResult
    {
        bool          armed = false;      // a probe is asked for on this view
        bool          valid = false;      // a value came back for the CURRENT coordinates
        std::uint32_t x = 0, y = 0, mip = 0, face = 0;
        bool          sideB = false;      // read from the comparison's picture B
        float         value[4] = { 0.f, 0.f, 0.f, 0.f };
        std::uint64_t frame = 0;          // the device frame it was recorded on
    };

    explicit TextureViewService(IGraphicsDevice& gfx);
    ~TextureViewService();
    TextureViewService(const TextureViewService&) = delete;
    TextureViewService& operator=(const TextureViewService&) = delete;

    // ---- Main thread ----
    std::uint32_t Acquire();                                   // kInvalidId when all are taken
    void          Request(std::uint32_t id, const TextureView::Desc& desc);
    void          Release(std::uint32_t id);
    // The output's SRV for ImGui::Image; 0 until its target exists. Re-resolve
    // every frame (never cache: a resize recreates it).
    std::uint64_t Srv(std::uint32_t id) const;
    // The output target itself (the harness's CaptureTextureToPNG, in the
    // serial window); nullptr when there is none.
    const RHI::Texture* Output(std::uint32_t id) const;
    TextureView::Status GetStatus(std::uint32_t id) const;
    std::uint32_t LiveCount() const;
    // Total bytes of the output targets (the [POOL]-style leak checks).
    std::uint64_t TargetBytes() const;

    // ---- Probes ----
    // Arm a probe on view @p id: from the next frame boundary on, texel (@p x, @p y) of the
    // view's shown mip - of cube face / slice @p face when >= 0, else the view's own - is
    // read on the GPU every frame (one thread) until StopProbe. What comes back is the
    // value the shaders see (the hardware's BC decode, an sRGB texture's linear value),
    // kFramesInFlight frames later. @p sideB: read the comparison's picture B instead
    // ((x, y) a texel of B's shown mip) while the view compares; A otherwise.
    void RequestProbe(std::uint32_t id, std::uint32_t x, std::uint32_t y, int face = -1, bool sideB = false);
    void StopProbe(std::uint32_t id);

    ProbeResult GetProbe(std::uint32_t id) const;

    // ---- Serial window ----
    void ServiceFrameBoundary();
    void ReloadShaders();
    void Shutdown();

    // ---- Render half ----
    void Execute(RHI::CommandList cl, std::uint64_t frame);

private:
    struct View
    {
        // Game half, under m_mutex.
        bool              live = false;
        bool              releasePending = false;
        bool              hasRequest = false;
        TextureView::Desc requested;
        std::uint64_t     requestedHash = 0;

        // Serial window only (the render half reads, parked meanwhile).
        RHI::Texture      target;
        std::uint64_t     targetBytes = 0;
        TextureView::Desc applied;
        std::uint64_t     appliedHash = 0;
        bool              dirty = false;
        bool              fresh = false;        // target created this boundary: draws this frame, budget or not
        TextureView::SourceKind kind = TextureView::SourceKind::Unsupported;
        std::uint64_t     sourceSrv = 0;
        std::uint32_t     mipW = 0, mipH = 0;
        // The comparison's picture B (applied.compare != Off), resolved the same way.
        TextureView::SourceKind kindB = TextureView::SourceKind::Unsupported;
        std::uint64_t     sourceSrvB = 0;
        std::uint32_t     mipWB = 0, mipHB = 0;

        // Render half writes, main thread reads - under m_statusMutex.
        std::uint64_t     renders = 0;
        std::uint64_t     renderedHash = 0;
        std::uint64_t     lastRenderFrame = 0;
        std::string       why;

        // Probe. Game half (m_mutex): what is asked; serial window: applied; render half
        // (m_statusMutex): the last value that came back.
        bool              probeWanted = false;
        std::uint32_t     probeX = 0, probeY = 0;
        int               probeFace = -1;
        bool              probeSideB = false;
        std::uint64_t     probeSeq = 0;           // bumped when the coordinates change
        bool              probeArmedApplied = false;
        std::uint32_t     probeXApplied = 0, probeYApplied = 0;
        int               probeFaceApplied = -1;
        bool              probeSideBApplied = false;
        std::uint64_t     probeSeqApplied = 0;
        std::uint64_t     probeSeqDone = 0;        // the seq of `probeValue`
        std::uint32_t     probeDoneX = 0, probeDoneY = 0, probeDoneMip = 0, probeDoneFace = 0;
        bool              probeDoneSideB = false;
        float             probeValue[4] = { 0.f, 0.f, 0.f, 0.f };
        std::uint64_t     probeFrame = 0;
    };
    // What each frame slot's readback holds, per view: set when a probe is
    // recorded into slot s, taken (and cleared) when slot s comes round again.
    struct ProbeRecord
    {
        std::uint64_t seq = 0;                     // 0 = nothing recorded
        std::uint32_t x = 0, y = 0, mip = 0, face = 0;
        bool          sideB = false;
        std::uint64_t frame = 0;
    };

    bool EnsureTarget(View& v, std::uint32_t w, std::uint32_t h, std::uint32_t id);
    void DestroyTarget(View& v);

    IGraphicsDevice&                 m_gfx;
    std::unique_ptr<TextureViewPass> m_pass;
    mutable std::mutex               m_mutex;        // the latched intents
    mutable std::mutex               m_statusMutex;  // what the render half reports
    std::vector<View>                m_views;
    bool                             m_anyLive = false;   // serial window's view of it
    std::uint64_t                    m_nextProbeSeq = 1;  // global: a reused view never matches a stale record
    std::vector<ProbeRecord>         m_probeRecords[3];   // [frame slot][view] (RHI::kFramesInFlight)
};
