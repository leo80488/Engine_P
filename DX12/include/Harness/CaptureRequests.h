#pragma once
// Harness/CaptureRequests.h - a picture of the main view, asked for over the pipe while the engine
// runs, so a script can look at the result of what it just did (an Entity.set, a hot-reloaded
// shader) instead of a -screenshot frame fixed before the run.
//
//   capture {"path": "<abs .png>", "afterFrame": N}
//     -> {"ok":true,"verb":"capture","jobId":J,"afterFrame":N}   (pipe thread)
//
// The job (Harness/ControlChannelJobs.h, kind "capture") finishes when HarnessCaptureSystem - in
// TickPhase::PostRender, the safe point every harness capture uses - takes the first frame whose
// index is >= afterFrame (default: the frame after the request). output: {path, source
// "view_output", view "main", afterFrame, renderedFrame (that frame's index: frames.jsonl's and the
// log's `frame`, Engine.Frame() during it), width, height, bytes}.
//
// "view_output" is the ToneMap final output of the main view: no editor UI, no debug overlay
// composited later. One capture is one hard GPU sync (a hitch), so this is for checks, not for
// recording video. The output also says what the picture is of: camera {nearZ, farZ, fovY,
// orthographic, position, forward, renderWidth, renderHeight} (the view the frame was drawn with),
// worldEpoch, exposure {value, ev, mode "auto"|"manual"} (read back from the exposure buffer the
// tonemap multiplied by, exact in both modes), hdrMode, renderSize and outputSize. An attachment's
// output and manifest carry the same.
// TODO(AI_Harness_Next_Plan R16): asynchronous readback, so a capture stops costing a hard GPU sync.
//
// A named camera: capture {"path", "camera": <entity id>, "width", "height"} is what that camera
// sees (in Edit mode the main view is the editor's camera, which no entity names). The service
// points the in-game PiP view at the camera (Renderer::SetGamePiP; a PiP that was on is put back
// afterwards), waits until that view has recorded two pictures since, and captures its output:
// view "camera", viewId, camera. Failure codes: stale_handle / no_camera (the id), unavailable (the
// preview layer is off), view_not_rendered (no picture within 240 frames - another camera panel
// may hold the view lane at another size).
//
// The window as presented: capture {"path", "source": "app_ui", "afterFrame"} is the back buffer
// the frame presented - Editor.exe's whole window with its panels (ImGui), Game.exe's final frame
// - which view_output (the default source) never contains. The copy is recorded by that frame's
// EndFrame and written after its GPU work (GraphicsDX12, one pending at a time: a -ui_screenshot
// in flight makes it wait a frame); output {path, source "app_ui", afterFrame, renderedFrame (the
// frame whose present it is), width, height (the window's), bytes}. capture_failed carries the
// device's reason; an unknown source is `unsupported_source`. The output also lists the editor's
// panels in that picture - windows [{name, label, x, y, w, h, order (back to front), focused,
// collapsed, docked}], client pixels; Game.exe has none (System/UiWindows.h).
// One panel: {"source": "app_ui", "panel": "<label or ImGui name>"} writes the whole window as
// <stem>.window.png and that panel's rectangle of it at `path`; output panel {name, label, rect
// [x, y, w, h] (clamped to the window), visibleFraction, occludedBy [labels of windows drawn over
// it]} - the crop is what was composited there, so a covered panel shows what covers it
// (visibleFraction says how much). no_panel (job): no such panel was laid out in that frame (the
// message lists the ones that were).
//
// An existing preview view: capture {"path", "view": <id>, "lease": <n>, "afterRenders": <count>}
// is that view's own output (a material preview, a camera panel, the PiP) - id and lease from
// Render.views(). The lease is the view's lifetime generation: a view released since, or its slot
// acquired again by another panel, is job error stale_view - never the new tenant's picture.
// afterRenders waits until the view's monotonic picture count reaches it (Render.views()
// `renders` + 1 = a picture recorded after that reading); view_not_rendered after 240 frames
// without one. Output {path, source "view_output", view "preview", viewId, lease, kind, renders,
// afterRenders, afterFrame, capturedFrame, width, height, bytes}.
//
// A render target as data: capture {"path", "attachment":
// "albedo"|"normal"|"surface"|"depth"|"velocity"|"extra"|"hdr", "afterFrame"} reads that main-view
// target back as stored at PostRender (after the render lane is drained: the graph's tracker
// states are final) - a GBuffer target, the depth plane of the depth buffer, or the device's HDR
// scene target - and writes <stem>.bin (the texels), <stem>.json (format, decode rules, meaning,
// per-channel stats with NaN / Inf counts, the camera) and the .png at `path` (a fixed-mapping
// preview, NaN / Inf magenta): Harness/AttachmentDecode.h. Output {path, raw, manifest, source
// "attachment", attachment, view "main", afterFrame, renderedFrame, width, height, format, channels
// (name + stats), nonFiniteTexels}. unavailable: the target is not backed this run;
// capture_failed: the readback or the decode said why.
//
// Right after a pass (Renderer/PassCapture.h): with "afterPass": "<pass instance name>" ("" = after
// the last pass that runs) the texture is the copy the main graph makes on its own command list
// right after that pass, in a later frame - so a texture a later pass overwrites is read as that
// pass left it. "attachment" then also takes any main-graph texture's name (an import's:
// "HdrScene", "XeGTAO.AO"); "stencil" is GBuffer_Depth's plane 1, "hdr" the HDR import. The output
// adds afterPass (the pass it followed) and copiedFrame. unavailable: not a graph texture, an
// owner-tracked import, the pass did not run, or no copy within 12 frames - the reason says.
//
// Needs the capture service: a harness build run with -artifacts (the SDK always passes it);
// without it the verb answers "unavailable".

#include <cstdint>
#include <string>
#include <vector>

namespace Harness::CaptureRequests
{
#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

    struct Request
    {
        std::uint64_t jobId      = 0;
        std::string   path;
        std::uint64_t afterFrame = 0;
        std::uint64_t camera     = 0;          // a packed entity id; 0 = the main view
        std::uint32_t width = 0, height = 0;   // the camera view's size
        bool          appUi      = false;      // source "app_ui": the presented back buffer
        std::int32_t  view       = -1;         // an existing preview view; -1 = none
        std::uint32_t lease      = 0;          // ... that tenancy of its slot
        std::uint64_t afterRenders = 0;        // ... once it has recorded this many pictures
        std::string   attachment;              // a render target as data; "" = a picture
        std::string   panel;                   // app_ui: crop to this editor panel ("" = the window)
        // A capture of a debug target made on request waits for the frame that makes it: not taken
        // before notBefore (afterFrame stays what was asked); produceWaits counts the frames so far.
        std::uint64_t notBefore    = 0;
        std::uint32_t produceWaits = 0;
        // An attachment right after this pass ("afterPass" given; empty string = the last pass that
        // runs), whether the copy was asked for (passCaptureAsked) and the frames waited for it.
        bool          afterPassGiven   = false;
        std::string   afterPass;
        bool          passCaptureAsked = false;
        std::uint32_t passCaptureWaits = 0;
    };

    // The `capture` verb (pipe thread). @p args is what follows "capture";
    // @p currentFrame is the frame the channel last saw.
    std::string ServeVerb(const std::string& args, std::uint64_t currentFrame);

    // `pick {"x": 0..1, "y": 0..1}` (pipe thread): which entity the main view draws at that point of
    // the viewport (0,0 = top left) - a job of kind "pick" HarnessCaptureSystem resolves through the
    // editor's own picking pass: the GPU id buffer, so what is drawn, the frontmost fragment winning,
    // transparent surfaces included. Output {entity (0 = nothing: the sky), name, x, y, pixel [px, py]
    // (display pixels), frame}. Errors: pick_not_rendered (120 frames), pick_lost (the pass was
    // rebuilt or the editor took the slot three times).
    //
    // Either form takes {"view": <id>, "lease": <n>} (both from Render.views()): a secondary view of
    // the main pipeline (Render.addView) instead of the main view - its pixels, its picture. Errors
    // add bad_view (a preview view: no picking pass draws it) and stale_view (released since).
    //
    // `pick {"all": true [, "path": "<absolute .png>"]}` - the object-ID image: the whole id buffer at
    // once. Output {width, height (the internal render size), frame, background {pixels, fraction},
    // unknownPixels (a slot no entity owns), entityCount, entities: [{entity, name, pixels, fraction,
    // box [x0, y0, x1, y1] and center [x, y] as fractions of the view - pick's space - boxPx, color
    // [r, g, b]}] by pixels, the largest first (at most 256; truncated), path}. With "path" it also
    // writes the image: each entity in its legend color, the background black.
    struct PickRequest
    {
        std::uint64_t jobId = 0;
        float         x = 0.f, y = 0.f;
        bool          all = false;            // the object-ID image
        std::string   path;                   // with all: where to write it (optional)
        std::int32_t  view = -1;              // a secondary view's id (-1: the main view)
        std::uint32_t lease = 0;
    };
    std::string ServePickVerb(const std::string& args);
    std::vector<PickRequest> TakePicks();

    // HarnessCaptureSystem (main thread): the service exists / went away.
    void SetServiced(bool serviced);
    // HarnessCaptureSystem, PostRender: the requests whose afterFrame (and notBefore) has come.
    std::vector<Request> TakeDue(std::uint64_t frame);
    // HarnessCaptureSystem, PostRender: a taken request back in the queue (its debug target is made
    // on request, next frame).
    void Requeue(Request r);

#else   // ---- inert stub: Retail has no control channel ----

    struct Request
    {
        std::uint64_t jobId = 0;
        std::string   path;
        std::uint64_t afterFrame = 0;
        std::uint64_t camera = 0;
        std::uint32_t width = 0, height = 0;
        bool          appUi = false;
        std::int32_t  view = -1;
        std::uint32_t lease = 0;
        std::uint64_t afterRenders = 0;
        std::string   attachment;
        std::string   panel;
        std::uint64_t notBefore = 0;
        std::uint32_t produceWaits = 0;
        bool          afterPassGiven = false;
        std::string   afterPass;
        bool          passCaptureAsked = false;
        std::uint32_t passCaptureWaits = 0;
    };
    inline void SetServiced(bool) {}
    inline std::vector<Request> TakeDue(std::uint64_t) { return {}; }
    inline void Requeue(Request) {}
    struct PickRequest
    {
        std::uint64_t jobId = 0;
        float         x = 0.f, y = 0.f;
        bool          all = false;
        std::string   path;
        std::int32_t  view = -1;
        std::uint32_t lease = 0;
    };
    inline std::vector<PickRequest> TakePicks() { return {}; }

#endif
}
