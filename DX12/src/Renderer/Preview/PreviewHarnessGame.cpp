// Preview::HarnessGame - see Renderer/Preview/PreviewHarnessGame.h.

#include "Renderer/Preview/PreviewHarnessGame.h"

#if defined(WITH_HARNESS)

#include "ECS/CameraStackComponents.h"   // VirtualCameraComponent
#include "ECS/Components.h"              // CameraComponent
#include "ECS/ECS.h"
#include "Harness/ExitCodes.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetRefs.h"
#include "Scene/GameModeContext.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

void Preview::HarnessGame::ParseBootFlags(Renderer& renderer)
{
    // ---- -preview_idle_frames=<N> ----
    // Shortens the preview layer's idle-teardown grace (shipped: 300 frames, 5 s at 60 Hz) so a
    // pack reaches the sweep within a few hundred frames. Not editor-gated: a Game.exe PiP has the
    // same lane and teardown. PreviewRenderer clamps it up to Preview::kMinIdleTeardownFrames,
    // below which the sweep could hand back descriptors an in-flight command list still names.
    // A hard precondition (exit 12), not an ENGINE_CHECK: an ENGINE_CHECK only logs and the run
    // continues with the shipped grace, so the teardown pack would never reach its sweep.
    if (Sys::CommandLine::Get().Has("-preview_idle_frames"))
    {
        previewIdleFrames = Sys::CommandLine::Get().GetInt("-preview_idle_frames", 0);
        if (previewIdleFrames <= 0)
        {
            LOG_ERROR("-preview_idle_frames needs a positive value "
                      "(-preview_idle_frames=<N>). Refusing to run: a bare or "
                      "zero flag leaves the shipped grace in place and the run "
                      "would silently measure the no-teardown case under the "
                      "teardown pack's name.");
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- -preview_meshlib_cap=<N> ----
    // Lowers the preview mesh-library LRU cap so a folder scan crosses it. A hard precondition on
    // a bare or zero value: an accepted flag that did nothing would leave the pack asserting the
    // eviction line against a run that never evicted.
    if (Sys::CommandLine::Get().Has("-preview_meshlib_cap"))
    {
        previewMeshLibCap =
            Sys::CommandLine::Get().GetInt("-preview_meshlib_cap", 0);
        if (previewMeshLibCap <= 0)
        {
            LOG_ERROR("-preview_meshlib_cap needs a positive value "
                      "(-preview_meshlib_cap=<N>). Refusing to run: a bare or "
                      "zero flag leaves the shipped cap of 24 in place, which "
                      "no corpus workload reaches, so the pack would measure "
                      "the no-eviction case under the eviction pack's name.");
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- -preview_ibl_delay=<N frames> ----
    // Holds the resolved studio IBL pair back from the preview layer for N SyncPreviewEnvironment
    // calls, so a pack takes the "HDRI landed after the first render" branch on purpose. Same
    // hard precondition as its neighbours: a bare or zero flag would leave the run in the ordinary
    // branch while the pack asserts the edge.
    if (Sys::CommandLine::Get().Has("-preview_ibl_delay"))
    {
        previewIblDelay =
            Sys::CommandLine::Get().GetInt("-preview_ibl_delay", 0);
        if (previewIblDelay <= 0)
        {
            LOG_ERROR("-preview_ibl_delay needs a positive value "
                      "(-preview_ibl_delay=<N frames>). Refusing to run: a bare "
                      "or zero flag publishes the studio IBL on its ordinary "
                      "clock, so the readiness-edge assertions would pass or "
                      "fail on disk timing rather than on the code.");
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- -preview_key_light=<x,y,z> ----
    // Moves the studio key light for this run only. The shipped Preview::kKeyLight
    // (PreviewStudioRig.h) sits ~30 deg off the panel's default camera axis, so every point the
    // studio shadow occludes already has N.L <= 0 and the shadow is a null test (mean-luma delta
    // 0.0000-0.0003 over six orbits). Changing the constant would re-bless the preview_fixtures
    // bands, the parity floors and a golden, so a pack that needs a visible shadow overrides it here.
    // Three comma-separated floats, the direction light TRAVELS (StudioLight::dir's convention;
    // the consumers normalise). A malformed or zero-length value is a hard precondition, not a
    // fallback: keeping the shipped light would measure the null test.
    if (Sys::CommandLine::Get().Has("-preview_key_light"))
    {
        const std::string spec = Sys::CommandLine::Get().Get("-preview_key_light");
        int   parsed = 0;
        std::size_t at = 0;
        while (parsed < 3 && at <= spec.size())
        {
            const std::size_t comma = spec.find(',', at);
            const std::string one   = (comma == std::string::npos)
                                    ? spec.substr(at) : spec.substr(at, comma - at);
            try { previewKeyLight[parsed] = std::stof(one); ++parsed; }
            catch (...) { parsed = -1; break; }
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        const float lenSq = (parsed == 3)
            ? previewKeyLight[0] * previewKeyLight[0]
            + previewKeyLight[1] * previewKeyLight[1]
            + previewKeyLight[2] * previewKeyLight[2] : 0.f;
        if (parsed != 3 || !(lenSq > 1e-8f))
        {
            LOG_ERROR("-preview_key_light needs three comma-separated floats "
                      "with a non-zero length (-preview_key_light=<x,y,z>, the "
                      "TRAVEL direction). Refusing to run: the shipped key light "
                      "puts the studio shadow where N.L is already zero, so a "
                      "run that kept it would measure the null test.");
            PostQuitMessage(Harness::PreconditionFail);
        }
        else previewKeyLightWant = true;
    }

    // ---- -preview_pip=<camera>[:<w>x<h>[:<tick>]] ----
    // Game-capable, not editor-gated: the in-game picture-in-picture is a Game.exe feature and
    // its budget is measured on Game.exe. It is the harness's spelling of Preview.SetPiPCamera
    // (the same call, the same game half), so a pack needs no Lua driver to switch it on.
    // A bare flag is an error: a pack that asked for a PiP and got none would measure the null
    // case under the PiP pack's name.
    const std::string pipSpec = Sys::CommandLine::Get().Get("-preview_pip");
    if (Sys::CommandLine::Get().Has("-preview_pip") && pipSpec.empty())
    {
        LOG_ERROR("-preview_pip needs a value "
                  "(-preview_pip=<cameraEntityName>[:<w>x<h>[:<tick>]]). "
                  "Refusing to run: a bare flag enables nothing and the pack "
                  "would measure the no-PiP case under the PiP pack's name.");
        PostQuitMessage(Harness::PreconditionFail);
    }
    if (!pipSpec.empty())
    {
        // <name>[:<w>x<h>[:<tick>]]: the name ends at the first ':' (an entity name cannot contain
        // ':'); the size and tick fields are optional.
        std::size_t c1 = pipSpec.find(':');
        pipName = (c1 == std::string::npos) ? pipSpec : pipSpec.substr(0, c1);
        if (c1 != std::string::npos)
        {
            const std::string rest = pipSpec.substr(c1 + 1);
            const std::size_t c2 = rest.find(':');
            const std::string dims = (c2 == std::string::npos) ? rest : rest.substr(0, c2);
            const std::size_t xpos = dims.find('x');
            ENGINE_CHECK(xpos != std::string::npos && xpos > 0 &&
                         xpos + 1 < dims.size(),
                         "-preview_pip size field '%s' is not <w>x<h>; using the "
                         "480x270 default.", dims.c_str());
            if (xpos != std::string::npos && xpos > 0 && xpos + 1 < dims.size())
            {
                pipW = static_cast<uint32_t>(std::strtoul(dims.substr(0, xpos).c_str(), nullptr, 10));
                pipH = static_cast<uint32_t>(std::strtoul(dims.substr(xpos + 1).c_str(), nullptr, 10));
            }
            if (c2 != std::string::npos)
                pipTick = static_cast<uint32_t>(std::strtoul(rest.substr(c2 + 1).c_str(), nullptr, 10));
        }
        if (pipW == 0 || pipH == 0) { pipW = 480; pipH = 270; }
        if (pipTick == 0) pipTick = 1;
        // Percent-decoded like -preview_camera: entity names contain spaces and the .iscene format
        // encodes them.
        pipName = Resource::AssetRefs::PercentDecode(pipName);
        // An empty name passes the bare-flag check: "-preview_pip=:480x270" has a non-empty value
        // and an empty name, which never resolves and would measure the no-PiP case.
        if (pipName.empty())
        {
            LOG_ERROR("-preview_pip='%s' parses to an EMPTY camera name. "
                      "Refusing to run: nothing will ever resolve and the run "
                      "measures the no-PiP case.", pipSpec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        LOG_INFO("PiP budget: camera '%s' at %ux%u, tick %u",
                 pipName.c_str(), pipW, pipH, pipTick);
    }

    // ---- -preview_pip_lights=<0|1> ----
    // Boot-time default for the secondary view's scene point and spot lights. `0` is the OFF arm
    // of a single-window A/B (the parity pack's third arm, and the circuit breaker's command-line
    // form). A cost driver's runtime toggle is Lua's Preview.SetWorldSceneLights(): p50Delta pairs
    // windows inside one process and a boot flag is set once. Game.exe too: the cost pack is a
    // Game.exe run.
    // Any value but 0 or 1 is exit 12: it would change nothing and the pack would measure the
    // default arm under the off arm's name.
    bool previewPipLights = true;
    if (Sys::CommandLine::Get().Has("-preview_pip_lights"))
    {
        const std::string v = Sys::CommandLine::Get().Get("-preview_pip_lights");
        if (v != "0" && v != "1")
        {
            LOG_ERROR("-preview_pip_lights needs a value of exactly 0 or 1 "
                      "(-preview_pip_lights=<0|1>); got '%s'. Refusing to run: "
                      "a flag that silently keeps the default arm makes the pack "
                      "measure the wrong side of its own A/B.", v.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
        {
            previewPipLights = (v == "1");
            LOG_INFO("[PREVIEW] -preview_pip_lights=%d: secondary-view scene "
                     "point/spot lights are %s for this run.",
                     previewPipLights ? 1 : 0,
                     previewPipLights ? "ON" : "OFF");
        }
    }
    // Latched on the Renderer until the preview layer exists: the layer is built lazily by the
    // first SetGamePiP, frames later, and a push into a missing layer would silently do nothing.
    renderer.SetPreviewWorldSceneLights(previewPipLights);

    // ---- -preview_pip_lights_at=<frame>:<0|1> ----
    // The runtime form of -preview_pip_lights, at an app frame, for a pack that captures the same
    // view on both sides of the breaker in one process.
    // Not a Lua driver: on an editor host App does not tick an -exec= scenario in Edit mode, and
    // -play on a pack whose fixture is an authored-in-code parity World means a snapshot/restore
    // across the thing measured (a driver without -play asserts nothing and captures the same
    // picture twice). The other one-shots on this clock are flags too (-preview_edit_at,
    // -preview_close_at, -preview_camera_close_at).
    // It is also the only caller that writes from the unsafe side: AfterFrame runs after Present,
    // before the next RenderPhaseBarrier, where a direct write into PreviewRenderer would race.
    // SetPreviewWorldSceneLights only records an intent; ApplyPendingGamePiP applies it inside the
    // parked window.
    if (Sys::CommandLine::Get().Has("-preview_pip_lights_at"))
    {
        const std::string spec = Sys::CommandLine::Get().Get("-preview_pip_lights_at");
        const std::size_t colon = spec.find(':');
        const std::string fs = (colon == std::string::npos) ? std::string()
                                                            : spec.substr(0, colon);
        const std::string vs = (colon == std::string::npos) ? std::string()
                                                            : spec.substr(colon + 1);
        const bool digits = !fs.empty() &&
            fs.find_first_not_of("0123456789") == std::string::npos;
        if (!digits || (vs != "0" && vs != "1"))
        {
            LOG_ERROR("-preview_pip_lights_at needs <frame>:<0|1> "
                      "(-preview_pip_lights_at=184:1); got '%s'. Refusing to "
                      "run: a toggle that silently did not happen leaves the "
                      "second capture a duplicate of the first, and every "
                      "imagePair over the two passes having compared nothing.",
                      spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
        {
            pipLightsAtFrame = std::strtoull(fs.c_str(), nullptr, 10);
            pipLightsAtValue = (vs == "1");
            pipLightsAtArmed = true;
            LOG_INFO("[PREVIEW] -preview_pip_lights_at: secondary-view scene "
                     "point/spot lights go %s at app frame %llu.",
                     pipLightsAtValue ? "ON" : "OFF",
                     static_cast<unsigned long long>(pipLightsAtFrame));
        }
    }
}

void Preview::HarnessGame::AfterFrame(const GameModeContext& ctx, Renderer& renderer)
{
    // ---- push the idle-teardown override, once ----
    // Pushed from the loop rather than at parse time; latched on the Renderer until the preview
    // layer is lazily constructed. A latch, not `ctx.frame == 0`: the counter is already 1 on the
    // first call, so a frame test would never match and the flag would be inert.
    if (previewIdleFrames > 0 && !previewIdleSent)
    {
        previewIdleSent = true;
        renderer.SetPreviewIdleTeardownFrames(
            static_cast<std::uint32_t>(previewIdleFrames));
    }
    // ---- push the preview mesh-library LRU cap, once ----
    // Same latch. The shipped cap of 24 is never reached by the corpus (the Tokyo scan peaks at 2),
    // so this flag is the only test of the eviction path (release, detach mesh-descriptor and
    // bindless VB/IB slots, drop the memos, bump the epoch, queue the deferred hand-back).
    if (previewMeshLibCap > 0 && !previewMeshLibCapSent)
    {
        previewMeshLibCapSent = true;
        renderer.SetPreviewMeshLibCap(
            static_cast<std::size_t>(previewMeshLibCap));
    }
    // ---- delay the studio IBL publication ----
    // Same latch; it must land before the first SyncPreviewEnvironment. That call runs inside
    // BuildRenderScene only once a preview layer exists, which needs an opened panel, and every
    // route that opens one is post-boot, i.e. after this has run at least once.
    if (previewIblDelay > 0 && !previewIblDelaySent)
    {
        previewIblDelaySent = true;
        renderer.SetPreviewIblDelayFrames(
            static_cast<std::uint32_t>(previewIblDelay));
    }
    // ---- move the studio key light, once ----
    // Same latch; it may land before any panel has opened: Renderer holds the value until
    // AcquirePreviewView builds the layer.
    if (previewKeyLightWant && !previewKeyLightSent)
    {
        previewKeyLightSent = true;
        ENGINE_CHECK(renderer.SetPreviewKeyLightDir(previewKeyLight[0],
                                                    previewKeyLight[1],
                                                    previewKeyLight[2]),
                     "-preview_key_light: the renderer refused the direction; "
                     "the run keeps the shipped studio key light.");
    }

    // ---- -preview_pip_lights_at: move the breaker at an app frame ----
    // One-shot and latched. `>=` rather than `==` so a frame the loop skipped (a resize, a device
    // reset) cannot drop it: the pack asserts a picture taken after this, so landing late is
    // recoverable and never landing is not.
    if (pipLightsAtArmed && !pipLightsAtSent && ctx.frame >= pipLightsAtFrame)
    {
        pipLightsAtSent = true;
        renderer.SetPreviewWorldSceneLights(pipLightsAtValue);
        LOG_INFO("[PREVIEW] -preview_pip_lights_at fired at app frame %llu: "
                 "secondary-view scene point/spot lights are now %s.",
                 static_cast<unsigned long long>(ctx.frame),
                 pipLightsAtValue ? "ON" : "OFF");
    }

    // ---- turn the in-game PiP on, once, from frame 2 ----
    // Not during Init: the scene loads asynchronously and the camera entity does not exist until
    // it has settled. It retries every frame until the entity appears; the pack's
    // "previewViews >= 1 after warm-up" assertion fails if it never does.
    if (!pipSent && !pipName.empty() && ctx.frame >= 2)
    {
        Entity found = NullEntity;
        auto scan = [&](const std::vector<Entity>& ents)
        {
            for (Entity cand : ents)
                if (found == NullEntity && m_world.IsAlive(cand) &&
                    m_world.GetName(cand) == pipName) found = cand;
        };
        scan(EntitiesOf(m_world.GetPool<CameraComponent>()));
        scan(EntitiesOf(m_world.GetPool<VirtualCameraComponent>()));
        if (found != NullEntity)
        {
            pipSent = true;
            LOG_INFO("-preview_pip='%s' resolved to entity %u; enabling the PiP.",
                     pipName.c_str(), static_cast<unsigned>(found));
            ENGINE_CHECK(renderer.SetGamePiP(found, pipW, pipH, pipTick),
                         "-preview_pip: SetGamePiP refused (preview layer "
                         "disabled, or the request was unusable); the run "
                         "will measure the no-PiP case.");
        }
        // A name that never resolves must fail loudly: retrying forever in silence turns a typo in a
        // pack argument into a green run of the null case. 120 frames is ~2 s at 60 Hz; every preview
        // pack runs 200+ and the scene has settled long before.
        else if (ctx.frame >= 120)
        {
            ENGINE_CHECK_ONCE(false,
                "-preview_pip='%s' never resolved to a live camera entity by "
                "frame 120. The PiP is OFF and this run is measuring the "
                "no-PiP case. Check the entity name (percent-encode spaces) "
                "and that the scene actually contains it.",
                pipName.c_str());
        }
    }
}

#endif // WITH_HARNESS
