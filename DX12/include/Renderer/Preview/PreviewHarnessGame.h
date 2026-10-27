#pragma once
// Preview::HarnessGame - the game-capable -preview_* harness flags: -preview_pip,
// -preview_idle_frames, -preview_meshlib_cap, -preview_ibl_delay, -preview_key_light,
// -preview_pip_lights and -preview_pip_lights_at. Parsed at boot (a bare or malformed
// value refuses the run, exit 12) and pushed into the Renderer from the frame loop's
// tail, once each. App::Run holds one as a local.
// WITH_HARNESS only: the definitions compile to nothing in Retail.

#include <cstdint>
#include <string>

class World;
class Renderer;
struct GameModeContext;

namespace Preview
{
    class HarnessGame
    {
    public:
        explicit HarnessGame(World& world) : m_world(world) {}
        // At boot on both hosts, once the Renderer exists: parses and validates the flags (a bad
        // value quits with Harness::PreconditionFail), pushes -preview_pip_lights at once and
        // latches the rest for AfterFrame.
        void ParseBootFlags(Renderer& renderer);
        // At the frame loop's tail, after ++ctx.frame (the first call sees ctx.frame == 1):
        // pushes each latched value into the Renderer once; -preview_pip_lights_at waits for its
        // frame, -preview_pip for its camera entity.
        void AfterFrame(const GameModeContext& ctx, Renderer& renderer);

    private:
        World& m_world;
        // Latched by ParseBootFlags, drained by AfterFrame.
        int  previewIdleFrames = 0;
        bool previewIdleSent   = false;
        int  previewMeshLibCap     = 0;
        bool previewMeshLibCapSent = false;
        int  previewIblDelay     = 0;
        bool previewIblDelaySent = false;
        float previewKeyLight[3]   = { 0.f, 0.f, 0.f };
        bool  previewKeyLightWant  = false;
        bool  previewKeyLightSent  = false;
        std::string pipName;
        uint32_t    pipW = 480, pipH = 270, pipTick = 1;
        bool        pipSent = false;
        std::uint64_t pipLightsAtFrame = 0;
        bool          pipLightsAtValue = true;
        bool          pipLightsAtArmed = false;
        bool          pipLightsAtSent  = false;
    };
}
