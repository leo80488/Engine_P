#include "Scene/SceneDefaults.h"

#include "ECS/Components.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/CameraSystem.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/WorldEnvironment.h"
#include "ECS/CloudComponent.h"
#include "ECS/HeightFogComponent.h"
#include "ECS/TODComponents.h"

namespace
{
    // Creates the "Sky" entity (Atmosphere + Clouds + HeightFog) and the separate "Time of Day"
    // entity. Shared by SpawnDefaultEnvironment and MigrateEnvironment so both produce the same
    // sky.
    void SpawnSkyAndTOD(World& world)
    {
        Entity sky = world.CreateEntity();
        world.SetName(sky, "Sky");
        world.AddComponent<AtmosphereComponent>(sky, AtmosphereComponent{});
        CloudComponent cloud{};
        cloud.enabled = true;                 // clouds on out of the box
        world.AddComponent<CloudComponent>(sky, cloud);
        world.AddComponent<HeightFogComponent>(sky, HeightFogComponent{});   // disabled by default

        Entity tod = world.CreateEntity();
        world.SetName(tod, "Time of Day");
        world.AddComponent<TODConfigComponent>(tod, TODConfigComponent{});   // enabled = false
        world.AddComponent<TODOutputComponent>(tod, TODOutputComponent{});
    }
}

void Scene::SpawnDefaultEnvironment(World& world)
{
    // Environment singleton: its presence makes the scene's sky explicit (`mode` is authoritative,
    // and MigrateEnvironment leaves the scene alone at load).
    Entity env = world.CreateEntity();
    world.SetName(env, "World Environment");
    WorldEnvironmentComponent wec{};
    wec.mode = WorldEnvironmentComponent::Mode::ProceduralAtmosphere;
    wec.iblStrength = kNewSceneSkyStrength;   // a new scene's sky is physical
    world.AddComponent<WorldEnvironmentComponent>(env, wec);

    SpawnSkyAndTOD(world);
}

void Scene::ApplyNewSceneSkyStrength(World& world)
{
    if (auto* p = world.GetPool<WorldEnvironmentComponent>(); p && !p->Data().empty())
        p->Data()[0].iblStrength = kNewSceneSkyStrength;
}

void Scene::MigrateEnvironment(World& world)
{
    // Already carries the singleton: respect it verbatim.
    if (auto* p = world.GetPool<WorldEnvironmentComponent>(); p && !p->Data().empty())
        return;

    using Mode = WorldEnvironmentComponent::Mode;
    WorldEnvironmentComponent env{};

    const AtmosphereComponent* atmos = nullptr;
    if (auto* p = world.GetPool<AtmosphereComponent>(); p && !p->Data().empty())
        atmos = &p->Data()[0];
    bool hasSkybox = false;
    if (auto* p = world.GetPool<SkyboxComponent>(); p && !p->Data().empty())
        hasSkybox = true;

    if (atmos && atmos->atmosphereEnabled)
    {
        env.mode = Mode::ProceduralAtmosphere;   // procedural sky was active
    }
    else if (atmos || hasSkybox)
    {
        env.mode = Mode::StaticSkybox;           // static cubemap / atmosphere-off
    }
    else
    {
        // No sky component at all: such a file was authored when a procedural sky was always drawn,
        // so keep that look (not a black screen) and seed the Sky + ToD entities so its knobs are
        // inspectable.
        env.mode = Mode::ProceduralAtmosphere;
        SpawnSkyAndTOD(world);
    }

    // Keep the file's Time-of-Day enable state.
    if (auto* p = world.GetPool<TODConfigComponent>(); p && !p->Data().empty())
        env.enableTimeOfDay = p->Data()[0].enabled;

    Entity e = world.CreateEntity();
    world.SetName(e, "World Environment");
    world.AddComponent<WorldEnvironmentComponent>(e, env);
}

void Scene::SpawnDefaultWorld(World& world)
{
    // Camera: App's main-camera hint latches onto the first entity with a CameraComponent.
    // Pose lives on LocalTransform, FPS-controller state on CameraControllerComponent.
    Entity cam = world.CreateEntity();
    world.SetName(cam, "Main Camera");
    CameraControllerComponent camCtrl{};
    world.AddComponent<CameraComponent>(cam, CameraComponent{});
    world.AddComponent<CameraControllerComponent>(cam, camCtrl);
    world.AddComponent<LocalTransform>(cam, CameraSystem::MakeTransform(camCtrl, { 4.f, 3.f, 5.f }));
    world.AddComponent<GlobalTransform>(cam, GlobalTransform{});

    Entity light = world.CreateEntity();
    world.SetName(light, "Directional Light");
    LightData ld;
    ld.color     = { 1.f, 0.92f, 0.82f };
    ld.intensity = 1.f;
    ld.type      = LightType::Directional;
    world.AddComponent<LightData>(light, ld);
    // Aim lives on the transform (LightUtil.h): a light without one has no direction.
    LocalTransform lightLT{};
    lightLT.rotation = LightUtil::RotationAiming({ 0.447f, -0.894f, 0.224f });
    world.AddComponent<LocalTransform>(light, lightLT);
    world.AddComponent<GlobalTransform>(light, GlobalTransform{});

    // Default environment: procedural atmosphere + clouds + (disabled) ToD, as an explicit
    // WorldEnvironment.
    Scene::SpawnDefaultEnvironment(world);
}
