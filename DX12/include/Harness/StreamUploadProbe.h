#pragma once
// Harness/StreamUploadProbe.h - -streamupload_probe, the streaming texture upload's
// correctness probe.
// It proves that a copy QueueTextureUpload queued for frame F is visible to frame F's
// EARLIEST reader, every frame, untorn - what the dedicated StreamUpload list (begun
// first, depending on nothing) is for - and that a request lands in the frame its Result
// names.
//   Writer A/B: the game half, TickPhase::PreRender (after AcquireUploadSlot, before the
//     hand-off), queues per frame texture A whole with LatestWins - a decoy first, then
//     the real data, which must merge in place (Replaced) - and texture B a 16x16
//     MustDeliver rect walking a 4x4 grid, every texel encoding F.
//   Writer C: TickPhase::GameplayPreLogic of frame G, BEFORE AcquireUploadSlot(G),
//     queues a 16x16 MustDeliver rect of texture C encoding G. It lands in the still-open
//     frame G-1 (Queued) if the render half has not taken G-1 yet (frame overlap), else
//     it is carried over into G (CarriedOver); the probe expects it in whichever frame
//     the Result named.
//   Reader: a graph pass in the GBuffer stage
//     (Renderer/Features/StreamUploadProbeFeature.cpp) copies A, B and C into this slot's
//     READBACK buffers (CopyTexture2DToBufferInPlace).
//   Check: AcquireUploadSlot(F+3) waited for frame F's slot to retire, so the PreRender
//     of F+3 reads frame F's readback: A must be F everywhere, B's rect F, each C rect
//     the frame expected for it, all uniform (torn otherwise). A value from an EARLIER
//     frame is "late" - the order bug the reverse arm reproduces.
//   Flushes: UploadStats::flushes from kFlushFrom on - the probe must not cause one.
//   Creation: texture D and buffer E are created ONCE with initial data (a position
//     pattern), so their copies take the COPY queue - D in COMMON while the ledger says
//     SHADER_RESOURCE. The reader copies them too (D through a barrier FROM that ledger
//     state, the rule the debug layer judges), and every check compares them with the
//     pattern.
// Reverse arm: -streamupload_probe_primary records the queued copies into the PRIMARY
// list (submitted last): every check is then a frame late, and the summary must show
// mismatches.
// The line: [UPLOAD_STREAM_PROBE] checked= mismatches= late= torn= mustChecked=
// mustMismatches= skipped= flushes= queued= replaced= carried= rejected= other=
// fallback= cChecked= cMismatches= cEarly= cCarried= dChecked= dMismatches=
// eMismatches= window= primary= loader= at= - every 120 frames and at shutdown,
// cumulative (logLineFields reads the last one). queued..fallback count A's and B's
// results; cEarly / cCarried C's; dChecked counts the checks of D and E (one each),
// dMismatches / eMismatches the ones that differed from the pattern; loader=1 when D
// and E were made inside a loader worker's upload scope (-streamupload_probe_loader).

#include "ECS/ISystem.h"
#include "Graphics/GraphicsStruct.h"

#include <atomic>
#include <cstdint>
#include <vector>

class IGraphicsDevice;

namespace Harness
{
    // A module's upload probe whose texture the StreamUploadProbe feature's pass reads back too
    // (the Video module's -video_probe). Registered before the renderer compiles
    // (Module::IEngineModule::Initialize): @p requested decides whether the pass exists, @p record
    // runs in it.
    struct UploadReadback
    {
        bool (*requested)() = nullptr;
        void (*record)(IGraphicsDevice& gfx, RHI::CommandList cl) = nullptr;
    };
    void RegisterUploadReadback(UploadReadback readback);
    const std::vector<UploadReadback>& UploadReadbacks();

    class StreamUploadProbe
    {
    public:
        static StreamUploadProbe& Get();
        // -streamupload_probe or -streamupload_probe_primary (harness builds only).
        static bool Requested();

        // The game half, PreRender of frame @p frame: checks the readback the
        // slot's previous frame left, then queues this frame's A and B.
        void GameTick(IGraphicsDevice& gfx, uint64_t frame);
        // The game half, GameplayPreLogic of frame @p frame (before its
        // AcquireUploadSlot): queues C.
        void EarlyTick(IGraphicsDevice& gfx, uint64_t frame);
        // The render half, from the probe's graph pass.
        void RecordReadback(IGraphicsDevice& gfx, RHI::CommandList cl);
        void Report(const char* when) const;

    private:
        static constexpr uint32_t kSize      = 64;    // every texture, RGBA8
        static constexpr uint32_t kRect      = 16;    // B's and C's rects: a 4x4 grid of them
        static constexpr uint32_t kSlots     = 3;     // RHI::kFramesInFlight
        static constexpr uint64_t kCheckFrom = 30;    // warm-up: the scene loads
        static constexpr uint64_t kFlushFrom = 60;
        static constexpr uint32_t kExpected  = 16;    // C rects awaiting their frame's check
        static constexpr uint32_t kBufBytes  = 4096;  // E, created with initial data

        struct Slot
        {
            uint64_t frame  = 0;
            bool     valid  = false;
            bool     aQueued = false, bQueued = false;   // landed in this frame (Queued / Replaced / Fallback)
            uint32_t bX     = 0, bY = 0;
            std::atomic<bool> recorded{ false };         // the reader copied this frame's textures
        };
        // A C rect and the frame whose reader must see it.
        struct Expected
        {
            uint64_t frame = 0;
            uint32_t x = 0, y = 0, value = 0;
            bool     valid = false;
        };

        bool Init(IGraphicsDevice& gfx);
        void Check(IGraphicsDevice& gfx, uint32_t s);
        // A 16x16 rect of a readback at (@p x, @p y): its value, false when torn.
        bool ReadRect(const uint8_t* p, uint32_t x, uint32_t y, uint32_t& value) const;

        bool              m_initTried = false;
        std::atomic<bool> m_ready{ false };
        bool              m_primary = false;
        bool              m_loader  = false;   // D and E made inside a loader worker's upload scope
        RHI::Texture      m_texA, m_texB, m_texC;
        RHI::GPUBuffer    m_readA[kSlots], m_readB[kSlots], m_readC[kSlots];
        RHI::Texture      m_texD;                     // created with initial data
        RHI::GPUBuffer    m_bufE;                     // created with initial data
        RHI::GPUBuffer    m_readD[kSlots], m_readE[kSlots];
        uint32_t          m_rowPitch = 0;
        Slot              m_slots[kSlots];
        Expected          m_expected[kExpected];
        std::vector<uint32_t> m_pixA, m_pixB, m_pixC;

        uint64_t m_checked = 0, m_mismatches = 0, m_late = 0, m_torn = 0;
        uint64_t m_mustChecked = 0, m_mustMismatches = 0, m_skipped = 0;
        uint64_t m_queued = 0, m_replaced = 0, m_carried = 0, m_rejected = 0, m_other = 0, m_fallback = 0;
        uint64_t m_cChecked = 0, m_cMismatches = 0, m_cEarly = 0, m_cCarried = 0;
        uint64_t m_dChecked = 0, m_dMismatches = 0, m_eMismatches = 0;
        uint64_t m_flushBase = 0, m_firstChecked = 0, m_lastChecked = 0, m_lastFrame = 0;
        bool     m_flushBaseTaken = false;
        uint32_t m_detailLogged = 0;
    };

    // The A/B writer's seat in the frame: PreRender runs after the Animation phase's
    // AcquireUploadSlot and before Render's hand-off. App registers it only when
    // Requested(): it runs as a batch of its own.
    class StreamUploadProbeSystem : public SystemInPhase<TickPhase::PreRender>
    {
    public:
        explicit StreamUploadProbeSystem(IGraphicsDevice& gfx) : m_gfx(gfx) {}
        ~StreamUploadProbeSystem() override;
        const char* GetName() const override { return "StreamUploadProbeSystem"; }
        SystemRunMask RunMask() const override { return SystemRunMask::Always; }
        void Update(World& world, const FrameContext& ctx) override;

    private:
        IGraphicsDevice& m_gfx;
    };

    // The C writer's seat: GameplayPreLogic, before the frame's
    // AcquireUploadSlot and - on overlap - beside the render half of the
    // frame before. Registered with the other, only when Requested().
    class StreamUploadProbeEarlySystem : public SystemInPhase<TickPhase::GameplayPreLogic>
    {
    public:
        explicit StreamUploadProbeEarlySystem(IGraphicsDevice& gfx) : m_gfx(gfx) {}
        const char* GetName() const override { return "StreamUploadProbeEarlySystem"; }
        SystemRunMask RunMask() const override { return SystemRunMask::Always; }
        void Update(World& world, const FrameContext& ctx) override;

    private:
        IGraphicsDevice& m_gfx;
    };
}
