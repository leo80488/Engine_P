#pragma once

// TODSystems.h: the Time-of-Day pipeline as small systems, one concern each:
//   TODTickSystem            - advances TODConfig.timeOfDay
//   TODEvaluationSystem      - Config -> Output (sun / moon direction, colour, active body)
//   TODSunSyncSystem         - Output -> LightData of the SunLightTag entity
//   TODMoonSyncSystem        - Output -> LightData of the MoonLightTag entity
//   TODAtmosphereSyncSystem  - Output -> atmosphere / sky consumers (a no-op: AtmosphereComponent
//                              stays author-owned)
//   TODLightSyncSystem       - Output -> TODLightComponent lamps
// The Renderer runs them in this order inside SyncSkyboxIBL (called from BuildScene_UploadLights),
// and only when a TODConfigComponent exists (opt-in, no auto-spawn; deleting the Time of Day entity
// sticks). FindConfig / FindOutput return the first entity carrying the component: the engine has
// no singleton API, so a single instance is by convention.

#include "ECS/ECS.h"

struct TODConfigComponent;
struct TODOutputComponent;

namespace TODUtil
{
    // First entity's TODConfigComponent (nullptr if no TOD entity exists).
    TODConfigComponent* FindConfig(World& w);
    // First entity's TODOutputComponent (nullptr if absent; the TODConfig deserializer and the editor
    // create menu both attach one).
    TODOutputComponent* FindOutput(World& w);
}

// Classes with a static Update, called per frame by the Renderer: no registration framework.

class TODTickSystem
{
public:
    static void Update(World& w, float deltaSeconds);
};

class TODEvaluationSystem
{
public:
    static void Update(World& w);
};

class TODSunSyncSystem
{
public:
    static void Update(World& w);
};

class TODMoonSyncSystem
{
public:
    static void Update(World& w);
};

class TODAtmosphereSyncSystem
{
public:
    static void Update(World& w);
};

// TODLightSyncSystem: drives every TODLightComponent. Local lights switch on as the sun sets and
// off as it rises, and the linked bulb material's emissive follows the same blend, so glow and
// light never disagree. Runs from the renderer's TOD block like its siblings, which makes it work
// in Edit mode (a Lua script cannot: ScriptLogicSystem is SystemRunMask::Play).
// @p deltaSeconds drives the warm-up fade: the same raw wall-clock dt TODTickSystem gets, so the
// fade and the clock agree.
class TODLightSyncSystem
{
public:
    static void Update(World& w, float deltaSeconds);
};
