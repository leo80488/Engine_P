#pragma once
// Harness/HarnessCaptureSystem.h - the per-frame observability record (frames.jsonl)
// and the harness captures: one-shot screenshots and dumps, the pipe's capture and pick
// jobs. Every number it writes is already computed each frame; recording it makes a
// frame that drew nothing distinguishable on disk from a good one (the Hi-Z false-cull
// signature: black window, zero ERROR lines, exit 0).
// Runs at TickPhase::PostRender, after Render on the calling thread, where this frame's
// CPU-side stats are final. Without -artifacts= Update() returns at once: no file, no
// formatting, no allocation.
// Never throw: the Scheduler's serial path has no try/catch, so an escaping exception
// would skip ShutdownAllSystems / SavePSOLibrary and lose the PSO cache. Streams have no
// exception masks; every failure degrades to "stop writing".

#include "ECS/ISystem.h"
#include "Graphics/HDROutput.h"   // HDROutputMode (cached for the display record)

#include "System/UiWindows.h"      // the panels in an app_ui picture

#include <nlohmann/json_fwd.hpp>   // AddMainViewMetadata

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class Renderer;
class IGraphicsDevice;
namespace PassCapture { struct Status; }                // Renderer/PassCapture.h
namespace Harness::CaptureRequests { struct Request; }  // Harness/CaptureRequests.h

namespace Harness
{
    /// Values App knows and the harness cannot reach. A singleton rather than a
    /// FrameContext field: FrameContext reaches every system, and only this reader wants it.
    struct FrameProbe
    {
        /// CPU frame time in ms, written by App just before the PostRender dispatch, so it
        /// describes THIS frame and leaves the harness's own write cost out of the editor's
        /// CPU Frame Time readout.
        float cpuFrameMs = 0.0f;

        // Defined inline and outside every WITH_HARNESS guard: App feeds cpuFrameMs to
        // EditorLayer::SetCPUFrameTime, so it must exist in a build without the harness
        // (its .cpp is excluded in Retail).
        static FrameProbe& Get()
        {
            static FrameProbe s_probe;
            return s_probe;
        }
    };

    class HarnessCaptureSystem : public SystemInPhase<TickPhase::PostRender>
    {
    public:
        HarnessCaptureSystem(Renderer& renderer, IGraphicsDevice& gfx);
        ~HarnessCaptureSystem() override;

        // DeclareAccess is deliberately not overridden: ISystem's default OpaqueAccess() makes
        // the Scheduler run this system alone, on the main thread. PostRender allows parallel
        // systems, so narrowing it to Read<>() declarations would let the Scheduler run
        // Update's file I/O on a TaskSystem worker. Do not narrow it.
        void Update(World& world, const FrameContext& ctx) override;
        // At the end of a run with -artifacts: the glyph bake's [FONT_BAKE] summary,
        // while the log is still open.
        void OnUnregister(World& world) override;
        const char* GetName() const override { return "HarnessCaptureSystem"; }

        // RunMask::Always, not Play: a run that never reaches Play (an editor session, a
        // failed precondition) still records its frames - "how far did it get" is the question.
        SystemRunMask RunMask() const override { return SystemRunMask::Always; }

        // True when the constructor refused its configuration: a bare -screenshot2 /
        // -preview_screenshot2, a bare or malformed -rgdump / -ui_render, a second slot without
        // -at_frame2 or with -at_frame2 equal to -at_frame, a roundtrip folder with no *.json (or
        // an output it cannot write), a failed -settings_storetest. App reads it right after
        // registering the system and posts PostQuitMessage(Harness::PreconditionFail) (exit 12)
        // before a frame is drawn, so a pack never fails later on a missing file with a message
        // about the wrong thing.
        bool ConfigRefused() const { return m_configRefused; }

        // Called by FramePipeline::RenderHalf under -renderowner BEFORE posting @p frame to the
        // lane, so a due -ui_screenshot or pipe app_ui capture is requested while the lane is
        // idle and the lane's EndFrame(frame) records it - the frame the serial path captures.
        // Update (PostRender) then finds the shot done or pending.
        void ServiceUiShotsBeforePost(std::uint64_t frame)
        {
            m_backBufferBeforePost = true;
            ServiceUiShots(frame);
            ServiceAppUi(frame, /*mayRequest=*/true);
        }

    private:
        struct UiRender
        {
            std::string   asset, path;
            std::uint32_t w = 1920, h = 1080;
            bool          spawned = false, done = false, steady = false;
            std::uint32_t root = 0;       // Entity
            std::uint32_t view = ~0u;
            std::uint64_t startedAt = 0, steadySince = 0;
        };

        struct UiShot { std::string path; std::uint64_t at = 0; bool done = false; };

        struct CameraCapture
        {
            std::uint64_t jobId = 0;
            std::string   path;
            std::uint64_t afterFrame = 0, packedId = 0;
            std::uint32_t camera = 0;               // the raw entity
            std::uint32_t width = 0, height = 0;
            bool          armed = false;
            std::uint64_t armedFrame = 0;
            bool          baselineKnown = false;    // rendersAtArm read (the view exists)
            std::uint64_t rendersAtArm = 0, rendersSeen = 0, lastRenderFrame = 0;
            // The PiP to put back: its camera (0 = it was off), size, tick.
            std::uint32_t prevCamera = 0, prevW = 0, prevH = 0, prevTick = 1;
        };

        struct AppUiCapture
        {
            std::uint64_t jobId = 0, afterFrame = 0;
            std::string   path;
            std::string   panel;            // crop to this editor panel ("" = the window)
            std::string   shotPath;         // what the device writes: path, or <stem>.window.png for a panel
            bool          requested = false;
            std::uint64_t requestedFrame = 0, dueFrame = 0;
            std::vector<Sys::UiWindows::Window> windows;   // the panels laid out in that frame
        };

        struct ViewCapture
        {
            std::uint64_t jobId = 0, afterFrame = 0, afterRenders = 0, dueFrame = 0;
            std::string   path;
            std::uint32_t view = 0, lease = 0;
            bool          started = false;
        };

        struct PickJob
        {
            std::uint64_t jobId = 0;
            float         x = 0.f, y = 0.f;
            std::string   path;                 // an image's PNG (optional)
            bool          started = false, requested = false;
            std::uint64_t requestedFrame = 0;
            std::uint32_t px = 0, py = 0;
            int           attempts = 0;
            std::int32_t  view = -1;            // a secondary view's id (-1: the main view)
            std::uint32_t lease = 0;
        };

        static constexpr std::uint64_t kDefaultDrawGrace = 60;

        void WriteFrameRecord(std::uint64_t frame);

        // -screenshot=<path> / -dumpstate=<path> / -dumpflow=<path> (and -preview_screenshot),
        // all fired once at -at_frame=N (default: the last frame of a -frames=N run). One
        // trigger, so the picture, the world that produced it and the flow state gating it
        // describe the same moment.
        // PostRender is a legal capture point: GraphicsDX12::CanHardSyncNow() admits it (the
        // frame's work is submitted), and ToneMapPass::TransitionForDisplay has left the final
        // texture in SHADER_RESOURCE, the state CaptureViewportToPNG assumes.
        void ServiceOneShots(World& world, std::uint64_t frame);

        // The second capture slot (-screenshot2 / -preview_screenshot2 at -at_frame2), with its
        // own frame and latch. expect.imagePair compares two files of ONE run, so a "before vs
        // after" claim (a panel opened, then torn down; a feature switched off, then on) needs
        // two moments in one process. The state / flow dumps stay on the first trigger only:
        // they describe one moment together.
        void ServiceSecondShot(std::uint64_t frame);

        // -rgdump=<file.json>[@<frame>]: one frame as it executed - the main render graph and
        // the device's frame (command lists, submission, barrier journal, observed edges) -
        // written through Harness/RenderGraphDump.h. Asks RG::FrameCaptures() on the given frame
        // (default 60) and writes the first capture published after the ask (normally the next
        // frame's; a frame later with the render half on its own lane). Its own latch, not the
        // -at_frame one-shot: the capture is built on the render half, so ask and write are
        // different frames.
        void ServiceRgDump(std::uint64_t frame);

        // -probedump[=<dir>]: after every reflection probe bake, that slice's six faces
        // (mip 0) are read back at the next frame boundary and written to <artifacts>/<dir>
        // (default "probes") as attachments probe<entity>_s<slice>_f<face>.bin / .json / .png
        // (the engine_tools.attachments format); one [PROBE_DUMP] line per bake.
        void ServiceProbeDumps(std::uint64_t frame);

        // -ui_screenshot=<png>@<frame>[,...]: the WHOLE editor window as presented (ImGui
        // included), which no other capture sees (-screenshot is the ToneMap output). Editor
        // host only: GraphicsDX12::EndFrame records the copy before the back buffer's
        // RENDER_TARGET -> PRESENT barrier (RequestBackBufferCapture).
        void ServiceUiShots(std::uint64_t frame);

        // The pipe's `capture` verb (Harness/CaptureRequests.h): turns the requests whose
        // afterFrame has come into jobs. At PostRender the capture is THIS frame's ToneMap
        // output, so a job's renderedFrame is @p frame.
        void ServiceCaptureRequests(World& world, std::uint64_t frame);

        // Named-camera captures (CaptureRequests.h), one at a time over several frames: point
        // the PiP view at the camera, wait until the view has recorded two pictures since,
        // capture, restore the PiP.
        void ServiceCameraCaptures(World& world, std::uint64_t frame);

        // The pipe's app_ui captures (CaptureRequests.h), front first: ask the device for the
        // NEXT presented back buffer, then take its outcome (TakeBackBufferShot) once that frame
        // is presented. The ask is made while that frame's EndFrame is still ahead - before the
        // post under -renderowner (@p mayRequest, from ServiceUiShotsBeforePost), at PostRender
        // on the serial path - so a job's renderedFrame is the frame it was asked in.
        void ServiceAppUi(std::uint64_t frame, bool mayRequest);

        // Captures of an EXISTING preview view by (id, lease) (CaptureRequests.h), front first:
        // stale_view once the slot no longer holds that tenancy; otherwise wait for a picture
        // (and afterRenders) and take the view's output here at PostRender.
        void ServiceViewCaptures(std::uint64_t frame);

        // -ui_render=<x.iui>[:<W>x<H>][:<out.png>]: a .iui spawned off screen (a canvas root
        // gets UICanvasOffscreen so the game screen never draws it; a rect root has no canvas),
        // drawn by a Renderer UI view at W x H design px, zoom 1, on the designer's checker
        // backdrop, and written to a PNG once its picture is final (Status::current with no
        // image loading, or after 120 frames of waiting, then 4 more frames). It is the view a
        // UI Editor tab draws through, so a tab at the same size, zoom 1 and pan 0 shows the
        // same picture. A failure is an ERROR (the pack's verdict).
        void ServiceUiRender(World& world, std::uint64_t frame);

        // The pipe's `pick` verb (CaptureRequests.h): asks the picking pass for one point of
        // the main view as the harness's pick (the editor leaves it alone) and resolves it once
        // its readback arrives. One at a time.
        void ServicePicks(World& world, std::uint64_t frame);

        // `pick {"all": true}`: the object-ID image - the picking pass's whole id buffer, read
        // back once and turned into a legend (and, with a path, a palette PNG). One at a time,
        // beside the point picks.
        void ServicePickImages(World& world, std::uint64_t frame);
        // A pick job's view: the main view (slot 0, its display size) or a secondary view of the
        // main pipeline still under the lease the job named (slot, its last picture's size; 0 x 0
        // before its first). False: the job failed (bad_view, stale_view) - the caller pops it.
        bool PickView(const PickJob& p, std::uint32_t& slot, std::uint32_t& w, std::uint32_t& h);

        // capture {"attachment"} (CaptureRequests.h): the main view's render target read back
        // AS STORED at PostRender and written as .bin + .json + a fixed-mapping .png
        // (Harness/AttachmentDecode.h). Finishes its job at once.
        void ServiceAttachment(World& world, std::uint64_t jobId, const std::string& attachment,
                               const std::string& path, std::uint64_t afterFrame, std::uint64_t frame,
                               const PassCapture::Status* afterPass = nullptr);
        // capture {"attachment", "afterPass"}: the copy the main graph makes right after the
        // pass (Renderer/PassCapture.h) - asked for once, the job requeued until the frame that
        // publishes it (12 frames at most), then read back as ServiceAttachment reads a target.
        void ServiceAfterPassCapture(World& world, const CaptureRequests::Request& r, std::uint64_t frame);

        // What a main-view picture was taken of: the camera the frame was drawn with, the
        // world epoch, the exposure its tonemap used (read back, exact in both modes), the
        // debug target shown, the HDR mode and the sizes. Merged into a view_output capture's
        // job output and an attachment's manifest. Synchronous: call it at PostRender, once per
        // capture.
        void AddMainViewMetadata(World& world, nlohmann::json& into);

        // The preview capture both slots share. @p flag names the command-line flag in the
        // log and the ledger, so a failure says which slot asked.
        void CapturePreviewShot(const std::string& path, std::uint64_t frame,
                                const char* flag);

        // -provenance_settings: the settings JSON of the last row written to
        // settings.resolved.jsonl (set only when the write succeeded); a row is appended only
        // when it differs.
        std::string m_lastResolvedSettings;
        // Resource::SpawnStats::syncNs at the previous row (frames.jsonl
        // spawnSyncMs is the difference).
        std::uint64_t m_lastSpawnNs = 0;
        std::string m_lastResolvedEngine;   // engine.resolved.jsonl

        UiRender m_uiRender;

        Renderer&        m_renderer;
        IGraphicsDevice& m_gfx;
        std::ofstream m_out;
        // Set by the constructor, read once by App (see ConfigRefused()).
        bool          m_configRefused = false;
        bool          m_enabled      = false;
        bool          m_opened       = false;
        bool          m_writeFailed  = false;   // latched: stop retrying
        int           m_every        = 1;       // -framelog_every=N
        // -require_draws[=N]: from frame N on (default kDefaultDrawGrace), a frame that
        // submits zero draw packets fails the run. The grace period is required: the first
        // frames legitimately draw nothing while the boot scene loads. The check catches a
        // renderer that stops drawing, or never starts once the world is up (the Hi-Z
        // false-cull signature).
        bool          m_requireDraws = false;
        std::uint64_t m_drawGrace    = 0;
        std::uint32_t m_emptyFrames  = 0;       // frames with zero draw packets
        bool          m_drawFailureReported = false;
        std::uint64_t m_written      = 0;

        std::string   m_screenshotPath;
        std::string   m_dumpStatePath;
        std::string   m_dumpFlowPath;
        // -preview_screenshot=<path>: the material-preview target, on the one-shot trigger of
        // the three above for the reason -dumpflow is: a preview PNG from another frame
        // describes another moment.
        std::string   m_previewShotPath;
        std::uint64_t m_atFrame       = 0;
        bool          m_oneShotsDone  = false;
        // -screenshot_every=N -screenshot_count=C: after the -screenshot one-shot, C-1 more
        // every N frames as <stem>.<frame><ext>; each moves m_atFrame to its own frame.
        std::uint32_t m_captureEvery = 0, m_captureRemaining = 0;
        // -screenshot2=<path> -at_frame2=<n>: its own path, frame and latch, independent of
        // the first capture.
        std::string   m_screenshot2Path;
        // -preview_screenshot2=<path>: the preview half of the second slot. Shares m_atFrame2
        // and m_secondShotDone, so both pictures of the second moment are coherent.
        std::string   m_previewShot2Path;
        std::uint64_t m_atFrame2      = 0;
        bool          m_secondShotDone = false;
        // -rgdump (see ServiceRgDump): the file, the frame it asks on, the
        // board sequence it waits past, and its latch.
        std::string   m_rgDumpPath;
        std::uint64_t m_rgDumpAt        = 60;
        std::uint64_t m_rgDumpSeq       = 0;
        std::uint64_t m_rgDumpAskedOn   = 0;
        bool          m_rgDumpAsked     = false;
        bool          m_rgDumpDone      = false;
        // -probedump (see ServiceProbeDumps): the folder, and the bake count
        // already written.
        std::string   m_probeDumpDir;
        std::uint64_t m_probeDumped     = 0;

        // -ui_screenshot entries (app frames).
        std::vector<UiShot> m_uiShots;

        // The display configuration last passed to RunLedger::NoteDisplay (m_displayNoted: one
        // was). Update compares it every frame rather than latching the first frame's, which the
        // swap chain may not have settled on; the cached copy keeps that check off the ledger's
        // lock.
        bool          m_displayNoted  = false;
        HDROutputMode m_lastHdrMode   = HDROutputMode::SDR;
        std::uint32_t m_lastOutputW   = 0;
        std::uint32_t m_lastOutputH   = 0;
        std::uint32_t m_lastRenderW   = 0;
        std::uint32_t m_lastRenderH   = 0;

        // Named-camera captures (ServiceCameraCaptures), front first.
        std::vector<CameraCapture> m_cameraCaptures;

        // app_ui captures (ServiceAppUi), front first.
        std::vector<AppUiCapture> m_appUiCaptures;

        // Existing-view captures (ServiceViewCaptures), front first.
        std::vector<ViewCapture> m_viewCaptures;
        // Set once FramePipeline::RenderHalf asks before the post (-renderowner): PostRender
        // then only takes outcomes - an ask there could land on the next frame.
        bool m_backBufferBeforePost = false;

        // `pick` jobs (ServicePicks; the object-ID images ServicePickImages),
        // front first.
        std::vector<PickJob> m_picks, m_pickImages;
    };
}
