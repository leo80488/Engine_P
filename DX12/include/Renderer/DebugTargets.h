#pragma once
// Renderer/DebugTargets.h - the main view shows a render target instead of the lit image.
//
// Switchable at any time, in the editor (the viewport toolbar's Buffer menu, Edit or Play) and
// in Game.exe:
//   Render.targets()                    what can be shown, and whether it is now
//   Render.target("normal" [, opts])    show one: opts = { mode, channel, min,
//                                       max, exposure, scale, mask }
//   Render.target(false)                back to the lit image
//   Render.target()                     what is shown
//   -debugtarget=<name>                 start with one
// The picture is painted into the main view's final output (ToneMapPass), so everything that
// reads that output sees it: the editor viewport, Game.exe's window, capture {view_output}.
// capture {"attachment": <any target name>} reads the same target's texels raw
// (Harness/AttachmentDecode.h).
//
// DebugTargetViewPass paints it after the post-process chain and before the overlays (debug
// wires, world UI, icons, the HUD), point-sampled to the view's size. Raw and Channel write the
// stored value, remapped from [min, max] to [0, 1], straight into the output - which holds
// gamma-space values in every output mode (Graphics/HDROutput.h) - so an 8-bit capture's
// pixel / 255 is (value - min) / (max - min): the picture is the data, quantized. Heat,
// Categorical and Bits colour a value, an integer and a bit field with
// shaders/debug_visualize.hlsli's maps. NaN / Inf paint magenta. A target that is not there this
// frame (its feature is off, its producer did not run) paints a grey checkerboard, never the lit
// image.
//
// A uint target (R8 / R16 / R32 UINT, one or two channels) and a depth buffer's stencil plane are
// painted too: the view reads them through a Tex2DUint handle; capture {"attachment"} reads them
// as integers.
//
// The registry (Renderer::ListDebugTargets): a curated table - the GBuffer, the HDR scene target,
// the AO and shadow masks, each with its meaning and default mapping - then every other
// render-graph texture by its graph name (group "Graph"). A graph name that a curated entry covers
// ("GBuffer1_Normal") names that entry. Names are matched case-insensitively. Adding a target a
// pass owns outside the graph: one row in Renderer_DebugTargets.cpp's table plus a const accessor
// on the pass returning the texture and the state its own tracker holds (RTAOPass::GetDebugOutput
// is the pattern).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Renderer;
struct lua_State;

namespace DebugTargets
{
    // How a target's texels become a picture. Mirrored in
    // shaders/DebugTargetView.cs.hlsl (MODE_*).
    enum class Mode : std::uint32_t
    {
        Auto     = 0,   // the target's own default (Info::defaultMode)
        Raw      = 1,   // rgb as stored, remapped from [min, max]
        Channel  = 2,   // one channel as grey, remapped from [min, max]
        Hdr      = 3,   // linear radiance * exposure, Reinhard, gamma 2.2
        Depth    = 4,   // reverse-Z device depth -> view distance, log ramp
                        // (near white, far and sky black)
        Velocity = 5,   // NDC motion: hue = direction, brightness = |mv| * scale
        Signed   = 6,   // signed data (-1..1) * 0.5 + 0.5
        // Colour maps (shaders/debug_visualize.hlsli):
        Heat        = 7,   // one channel, [min, max] -> black, blue, cyan, green, yellow, red, white
        Categorical = 8,   // one channel as an integer (a uint target's own; a float one's value * scale),
                           // & mask -> a fixed colour per value; outside [min, max] black (no category)
        Bits        = 9,   // the same integer as the stencil byte's layout: the low six bits a category,
                           // 0x40 / 0x80 stripes (Graphics/ShadingStencil.h)
        Ev          = 10,  // false colour of log2(luminance * exposure / 0.18), the stops from middle grey, on
                           // the heat ramp over [min, max] (-6 .. +6 stops is a good range)
        Count
    };
    const char* ModeName(Mode m);
    bool        ParseMode(const std::string& s, Mode& out);   // case-insensitive name

    // What the main view is asked to show. An empty name is the lit image.
    struct View
    {
        std::string name;
        Mode        mode     = Mode::Auto;
        int         channel  = -1;      // -1 = the target's default; 0..3 = r g b a
        float       rangeMin = 0.f;
        float       rangeMax = 1.f;
        float       exposure = 1.f;     // Hdr, Ev
        float       scale    = 100.f;   // Velocity: NDC distance -> brightness; Categorical / Bits on a
                                        // float target: value -> integer
        std::uint32_t mask   = 0xFFFFFFFFu;   // Categorical / Bits: the integer's bits that count
        // Which of min / max / scale / mask the asker set. One not set takes the
        // target's own default (Info::defaultMin ...) - the same as the values
        // above for every target that has no other.
        bool        minGiven = false, maxGiven = false, scaleGiven = false, maskGiven = false;
    };

    // One entry of the registry (Renderer::ListDebugTargets).
    //
    // `semantic` is what the texels ARE - it picks the capture manifest's
    // decode notes and preview mapping, independent of the view mode:
    //   color     linear colour (the albedo)     normal    world normal * 0.5 + 0.5
    //   surface   material terms                 depth     reverse-Z device depth
    //   velocity  NDC motion vector              radiance  linear HDR light
    //   mask      a [0, 1] factor (AO, shadow)   data      anything else, as stored
    struct Info
    {
        std::string   name;             // what Render.target and capture {"attachment"} take
        std::string   label;            // the Buffer menu's text
        std::string   group;            // GBuffer / Scene / Lighting / Graph
        std::string   semantic;         // see above
        std::string   meaning;          // what the texels are
        std::string   channels;         // per-channel names, comma-separated
        std::string   graphName;        // its render-graph texture, if it is one
        Mode          defaultMode    = Mode::Raw;
        int           defaultChannel = -1;
        // The mapping a view that does not set them gets: View's own defaults
        // unless the target has better ones (a heat target's range, GBuffer1.a's
        // matIdx scale and mask).
        float         defaultMin = 0.f, defaultMax = 1.f, defaultScale = 100.f;
        std::uint32_t defaultMask = 0xFFFFFFFFu;
        bool          available      = false;   // backed, and written this frame
        std::string   why;                      // why not available
        bool          viewable       = true;    // the view can paint it (not an
                                                // array / integer texture) -
                                                // capture {"attachment"} reads it either way
        std::string   viewWhy;                  // why not viewable
        std::uint32_t width = 0, height = 0;    // 0 = not backed
        std::string   format;                   // e.g. "R16G16B16A16_FLOAT"
    };

    // What the LAST frame painted (Renderer::GetDebugTargetShown): the view
    // with Auto resolved to the target's mode and channel, and whether the
    // target was there. frame = the frame index it was painted in.
    struct Shown
    {
        View          view;
        bool          active    = false;   // a target was asked for
        bool          available = false;
        std::string   why;
        std::uint64_t frame     = 0;
    };

    // The registry's names as the main renderer last published them (every
    // Compile), for code that holds no Renderer - the pipe's capture verb
    // validating {"attachment"}. @p aliases: the graph names curated entries
    // cover ("GBuffer1_Normal" is "normal") - accepted, not listed.
    // Thread-safe. Empty before the first Compile.
    void                     PublishNames(std::vector<std::string> names, std::vector<std::string> aliases);
    std::vector<std::string> PublishedNames();
    bool                     IsPublishedName(const std::string& name);   // a name or an alias, case-insensitive

    // Registers Render.targets and Render.target (DebugTargetsLua.cpp;
    // Lua_API_Reference.md, Render). @p drain finishes the frame's in-flight
    // render work (FramePipeline::DrainFrameWork) and is only callable on the
    // main thread, which is where both functions must be called.
    void RegisterLua(lua_State* L, Renderer& renderer, std::function<void()> drain);
}
