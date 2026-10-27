#include "Renderer/Preview/LuaPreviewBindings.h"

#include "ECS/CameraStackComponents.h"   // VirtualCameraComponent
#include "ECS/Components.h"              // CameraComponent
#include "ECS/ECS.h"
#include "Renderer/Preview/PreviewRenderer.h"
#include "Renderer/Renderer.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <string>
#include <vector>

// Preview.* - the in-game picture-in-picture. The feature lives on the
// Renderer; App hands over it and the World.
//
// Lua runs on the game half, which SetGamePiP needs (it acquires a preview
// view and writes the latch; PreviewRenderer's caller check refuses the render
// half). Nothing here renders: ExtractPreviewCameras + ExtractPreviewRequests
// pick the request up at the end of the NEXT BuildRenderScene, so the first
// PiP pixels land one frame later.
//
// Opt-in: a game that never calls SetPiPCamera pays nothing, and
// Preview.ClearPiP() takes the whole feature back out with no recompile.
void Preview::RegisterLuaPreviewBindings(sol::state& state, Renderer& renderer, World& world)
{
    sol::state* lua = &state;
    Renderer* rp = &renderer;
    World*    wp = &world;
    sol::table pv = lua->create_table();

    // Preview.SetPiPCamera(entityOrName [, w, h [, tickInterval]])
    //   entityOrName: an entity id/handle, or an entity NAME (the common
    //   case from a scenario driver, which knows the scene's names and not
    //   its ids). Returns true when the view was acquired.
    pv.set_function("SetPiPCamera",
        [rp, wp](sol::object who, sol::optional<int> w, sol::optional<int> h,
                 sol::optional<int> tick) -> bool
        {
            Entity e = NullEntity;
            if (who.is<std::string>())
            {
                const std::string want = who.as<std::string>();
                // Scanned over the camera pools, not every entity: the World has no name
                // index or full enumeration, and only a camera is usable here. Both pools,
                // because a VCam rig carries VirtualCameraComponent and may not carry
                // CameraComponent.
                auto scan = [&](const std::vector<Entity>& ents)
                {
                    for (Entity cand : ents)
                        if (e == NullEntity && wp->IsAlive(cand) &&
                            wp->GetName(cand) == want) e = cand;
                };
                scan(EntitiesOf(wp->GetPool<CameraComponent>()));
                scan(EntitiesOf(wp->GetPool<VirtualCameraComponent>()));
            }
            else if (who.is<LuaEntity>())
            {
                e = LuaId::ResolveLive(*wp, who.as<LuaEntity>());
            }
            else if (who.is<double>())
            {
                const Entity raw = static_cast<Entity>(who.as<double>());
                if (wp->IsAlive(raw)) e = raw;
            }
            if (e == NullEntity)
            {
                LOG_WARNING("[PREVIEW] Preview.SetPiPCamera: no live entity "
                            "matched the argument; PiP not enabled");
                return false;
            }
            const uint32_t pw = static_cast<uint32_t>(w.value_or(480));
            const uint32_t ph = static_cast<uint32_t>(h.value_or(270));
            const uint32_t pt = static_cast<uint32_t>(tick.value_or(1));
            return rp->SetGamePiP(e, pw, ph, pt);
        });
    pv.set_function("ClearPiP", [rp]() { rp->ClearGamePiP(); });
    // Preview.SetPiPRect(x, y, w, h) - where the inset composites, in back-buffer
    // pixels (Renderer::m_pipRect). w or h <= 0 restores AUTO (bottom-right, 16 px
    // margin).
    pv.set_function("SetPiPRect",
        [rp](int x, int y, int w, int h) { rp->SetGamePiPRect(x, y, w, h); });
    // Read-back for a scenario driver: which entity (0 = off) and how many
    // packets the last secondary cull drew / offered. The counts are what
    // turn "the PiP rendered" into "the PiP rendered THE SCENE".
    pv.set_function("PiPEntity", [rp]() -> unsigned
    { return static_cast<unsigned>(rp->GetGamePiPEntity()); });
    // An optional view id. Absent = whichever view recorded last, what a
    // single-view PiP run wants; with a split panel that is whichever cell the
    // round-robin served last, so a driver that wants a named cell's numbers
    // passes its id.
    auto viewArg = [](sol::optional<int> v) -> uint32_t
    {
        return (v && *v >= 0) ? static_cast<uint32_t>(*v)
                              : Preview::kInvalidView;
    };
    pv.set_function("WorldCullDraws",
        [rp, viewArg](sol::optional<int> v) -> unsigned
        { return rp->GetPreviewWorldCullDraws(viewArg(v)); });
    pv.set_function("WorldCullCandidates",
        [rp, viewArg](sol::optional<int> v) -> unsigned
        { return rp->GetPreviewWorldCullCandidates(viewArg(v)); });
    // How many of the kept packets came from the secondary-view lists -
    // geometry the MAIN camera cannot see. A driver asserting > 0 with a camera
    // pointed the other way is the cheap proof that the PiP is a second view
    // rather than a crop of the first.
    pv.set_function("WorldCullSecondary",
        [rp, viewArg](sol::optional<int> v) -> unsigned
        { return rp->GetPreviewWorldCullSecondary(viewArg(v)); });
    // ---- The secondary view's scene point and spot lights ----
    // Two calls, and the pack needs both. The cost gate (expect.frames.p50Delta)
    // is an in-process A/B: the runner pairs ON and OFF frame windows from one
    // run, so a driver flips the arm at runtime with SetWorldSceneLights (a boot
    // flag cannot alternate windows). WorldClusterLights makes the flip observable
    // per window: it reports what the record half actually BOUND, not what the
    // cull produced, so lights the game half culled but the render half refused on
    // the frame stamp count 0 and a silent no-op cannot pass the gate.
    pv.set_function("SetWorldSceneLights",
        [rp](bool on) { rp->SetPreviewWorldSceneLights(on); });
    pv.set_function("WorldSceneLightsEnabled",
        [rp]() -> bool { return rp->PreviewWorldSceneLightsEnabled(); });
    pv.set_function("WorldClusterLights",
        [rp, viewArg](sol::optional<int> v) -> unsigned
        { return rp->GetPreviewWorldClusterLights(viewArg(v)); });
    // The preview frame counter, which is NOT Engine.Frame(): it advances once
    // per frame the preview layer was serviced, and ViewDesc::tickIntervalFrames
    // counts on it, so a driver asserting a cadence reads this. A run with no
    // preview panel never advances it.
    pv.set_function("PreviewFrame", [rp]() -> double
    { return static_cast<double>(rp->GetPreviewFrame()); });
    (*lua)["Preview"] = pv;
}
