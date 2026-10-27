// EditorLayer_CreateMenu.cpp - the Create menu's items. Drawn in two places: the main menu bar's
// "Create" and the Hierarchy's "+ Create" button (same list, same behaviour).
//
// Undo: entity items run inside RecordSpawn, which diffs the entity set before and after and
// records every new subtree root as one Create entry (a macro when there are several). The undo
// dispatch layer also sets the scene dirty flag, so an item outside RecordSpawn neither undoes nor
// dirties the scene. Items that only edit an existing singleton (Environment > Flat Color on a
// scene that already has a World Environment) create nothing and so record nothing.
#include "Editor/EditorLayer.h"
#include "Editor/FontAwesomeIcons.h"
#include "Editor/EditorTheme.h"             // Icon() column, MenuSection()

#include "ECS/Components.h"              // LightData, MaterialComponent
#include "ECS/HierarchyComponents.h"     // LocalTransform, GlobalTransform
#include "ECS/LightUtil.h"               // light direction derived from the transform (+Z)
#include "ECS/WorldEnvironment.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/HeightFogComponent.h"
#include "ECS/VolumetricFogComponent.h"
#include "ECS/LocalFogVolumeComponent.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "PostProcess/ProfileSystem.h"   // runtime profile for a new volume
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "ECS/TODComponents.h"           // TODConfig/Output, Sun/MoonLightTag
#include "ECS/DDGIComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/TagComponent.h"
#include "ECS/BillboardComponent.h"
#include "ECS/BillboardFXComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/FollowComponents.h"        // FollowEntityComponent (world-space UI)
#include "Scene/MeshSpawner.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"           // UI::FindOwningCanvas
#include "UI/WorldSpaceUI.h"
#include "Editor/UITemplates.h"          // the Canvas widgets the UI Editor's Palette makes too

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"          // MenuItemEx / BeginMenuEx (icon column)

#include <algorithm>

using namespace DirectX;

void EditorLayer::DrawCreateMenuItems()
{
    // Assets are created in the content browser's current folder and opened
    // immediately. They have their own document history rather than scene undo.
    if (ImGui::BeginMenuEx("Animation", EditorTheme::Icon(FontAwesome::kFilm)))
    {
        if (ImGui::MenuItem("Animation Graph")) CreateAnimationAsset(true);
        if (ImGui::MenuItem("Animation Set")) CreateAnimationAsset(false);
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (!m_world)
    {
        ImGui::TextDisabled("(no active scene)");
        return;
    }

    // Common prologue for every spatial item: a named entity with a transform,
    // spawned in front of the camera (not at the origin, which is off-screen
    // in most scenes).
    auto newSpatial = [this](const char* name) -> Entity
    {
        Entity e = m_world->CreateEntity();
        m_world->SetName(e, name);
        LocalTransform lt{};
        lt.translation = ComputeSpawnPosition();
        m_world->AddComponent<LocalTransform>(e, lt);
        m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
        return e;
    };

    // ---- Empty ----
    if (ImGui::MenuItemEx("Empty Entity", EditorTheme::Icon(FontAwesome::kCircle)))
        RecordSpawn("Create Empty Entity", [&] { m_selectedEntity = newSpatial("Entity"); });
    ImGui::Separator();

    // ---- 3D Object ----
    if (ImGui::BeginMenuEx("3D Object", EditorTheme::Icon(FontAwesome::kCube)))
    {
        // A Static Mesh of the engine's Primitives model; the MeshHandle primitive
        // (m_createMeshCallback) only when that model cannot be read.
        // Editor.CreatePrimitive(kind, {staticMesh = true}) is this item from a script.
        auto createPrimitive = [&](const char* label, int type) {
            if (!ImGui::MenuItem(label)) return;
            RecordSpawn(label, [&] {
                const XMFLOAT3 at = ComputeSpawnPosition();
                const Entity   e  = CreatePrimitiveStaticMesh(type, at);
                if (e != NullEntity) m_selectedEntity = e;
                else if (m_createMeshCallback) m_createMeshCallback(type, at);
            });
        };
        createPrimitive("Cube",   0);
        createPrimitive("Sphere", 1);
        createPrimitive("Cone",   2);
        createPrimitive("Plane",  3);
        createPrimitive("Torus",  4);
        ImGui::Separator();

        // A plane + a Water-domain material IS water; there is no
        // WaterComponent to add. This is the discoverable one-click entry.
        if (ImGui::MenuItem("Water Plane"))
        {
            RecordSpawn("Water Plane", [&] {
                Entity e = MeshSpawner::SpawnWaterPlane(*m_world, ComputeSpawnPosition());
                if (e != NullEntity) m_selectedEntity = e;
            });
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "50 m water plane: Plane mesh + material with Domain = Water,\n"
                "shaded by asset/Shaders/water_surface.hlsl.\n"
                "Scale it with the gizmo; edit the look in the Material inspector.");
        ImGui::EndMenu();
    }

    // ---- Collision ----
    // A child of the selection with a Collider fitted to its mesh and a
    // Kinematic RigidBody (it follows the parent - Unreal's ShapeComponent);
    // with nothing selected, a Static one in front of the camera.
    if (ImGui::BeginMenuEx("Collision", EditorTheme::Icon(FontAwesome::kCube)))
    {
        auto createCollision = [&](const char* label, ColliderComponent::Shape shape) {
            if (!ImGui::MenuItem(label)) return;
            RecordSpawn(label, [&] {
                const Entity e = CreateCollisionEntity(shape, m_selectedEntity);
                if (e != NullEntity) m_selectedEntity = e;
            });
        };
        createCollision("Box Collision",     ColliderComponent::Shape::Box);
        createCollision("Sphere Collision",  ColliderComponent::Shape::Sphere);
        createCollision("Capsule Collision", ColliderComponent::Shape::Capsule);
        ImGui::EndMenu();
    }

    // ---- Lighting & GI ----
    if (ImGui::BeginMenuEx("Lighting & GI", EditorTheme::Icon(FontAwesome::kLightbulb)))
    {
        auto createLight = [&](LightType type, const char* name) {
            RecordSpawn(name, [&] {
                // Position and aim via LocalTransform (not LightData) - spot
                // lights point down, which is what users expect to place first.
                Entity e = newSpatial(name);
                if (type == LightType::Spot)
                    if (auto* lt = m_world->GetComponent<LocalTransform>(e))
                        lt->rotation = LightUtil::RotationAiming({ 0.f, -1.f, 0.f });

                LightData ld;
                ld.type = type;
                if (type == LightType::Point)
                {
                    ld.radius    = 15.f;
                    ld.intensity = 5.f;
                }
                else if (type == LightType::Spot)
                {
                    ld.radius    = 20.f;
                    ld.intensity = 10.f;
                    ld.spotAngle = 0.5236f; // 30 degrees
                }
                m_world->AddComponent<LightData>(e, ld);

                // Billboard icon for the light (transparent forward, unlit).
                BillboardComponent bb;
                bb.mode      = BillboardMode::Transparent;
                bb.worldSize = 1.0f;
                m_world->AddComponent<BillboardComponent>(e, bb);
                m_selectedEntity = e;
            });
        };

        EditorTheme::MenuSection("Lights");
        if (ImGui::MenuItem("Point Light"))       createLight(LightType::Point,       "Point Light");
        if (ImGui::MenuItem("Spot Light"))        createLight(LightType::Spot,        "Spot Light");
        if (ImGui::MenuItem("Directional Light")) createLight(LightType::Directional, "Directional Light");

        EditorTheme::MenuSection("Global Illumination");
        // DDGI volume - regular probe grid, resize via the Inspector.
        if (ImGui::MenuItem("DDGI Volume"))
        {
            RecordSpawn("DDGI Volume", [&] {
                Entity e = newSpatial("DDGI Volume");
                DDGIVolumeComponent vc{};
                // origin is the world-space grid center (read directly, not from
                // the transform) - anchor it to the spawn point too.
                if (const auto* lt = m_world->GetComponent<LocalTransform>(e))
                    vc.origin = lt->translation;
                // 60x30x60 m covers one medium room; 256 probes / 64 rays keep
                // the first DispatchRays light. Scale up in the Inspector.
                vc.extent = { 30.f, 15.f, 30.f };
                vc.probeCountsX = 8; vc.probeCountsY = 4; vc.probeCountsZ = 8;
                vc.raysPerProbe = 64;
                vc.debugDraw = true;   // probe spheres visible by default for feedback
                m_world->AddComponent<DDGIVolumeComponent>(e, vc);
                m_world->AddComponent<DDGIVolumeRuntimeComponent>(e, {});
                m_selectedEntity = e;
            });
        }
        // Reflection probe - a mirror sphere (rough 0 / metal 1) as its debug
        // viz + ReflectionProbeComponent + the "ReflectionProbe" tag.
        if (ImGui::MenuItem("Reflection Probe"))
        {
            RecordSpawn("Reflection Probe", [&] {
                Entity e = MeshSpawner::Spawn(/*Sphere*/ 1, *m_world, ComputeSpawnPosition());
                if (e == NullEntity) return;
                m_world->SetName(e, "Reflection Probe");
                // rough/metal min==max collapses the GBuffer lerp regardless of
                // the surface map.
                if (auto* mat = m_world->GetComponent<MaterialComponent>(e))
                {
                    mat->SetRoughnessMin(0.0f);
                    mat->SetRoughnessMax(0.0f);
                    mat->SetMetalnessMin(1.0f);
                    mat->SetMetalnessMax(1.0f);
                }
                // Default extents (inner 4 / outer 6) - the Renderer queues the
                // first bake automatically.
                m_world->AddComponent<ReflectionProbeComponent>(e, ReflectionProbeComponent{});
                TagComponent tags{};
                tags.Add("ReflectionProbe");
                m_world->AddComponent<TagComponent>(e, tags);
                m_selectedEntity = e;
            });
        }
        ImGui::EndMenu();
    }

    // ---- Environment (Sky / Atmosphere / Time-of-Day) ----
    // Optional-sky authoring: pieces are added/removed explicitly. The
    // renderer keys off WorldEnvironmentComponent.mode (not component
    // presence), so deleting the Sky/ToD entity sticks.
    if (ImGui::BeginMenuEx("Environment", EditorTheme::Icon(FontAwesome::kGlobe)))
    {
        // Find-or-create the WorldEnvironment singleton entity.
        auto ensureEnvEntity = [this]() -> Entity {
            if (auto* p = m_world->GetPool<WorldEnvironmentComponent>();
                p && !p->Entities().empty())
                return p->Entities()[0];
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, "World Environment");
            m_world->AddComponent<WorldEnvironmentComponent>(e, WorldEnvironmentComponent{});
            return e;
        };
        auto setMode = [this, ensureEnvEntity](WorldEnvironmentComponent::Mode m) -> Entity {
            Entity e = ensureEnvEntity();
            if (auto* wec = m_world->GetComponent<WorldEnvironmentComponent>(e))
                wec->mode = m;
            return e;
        };
        // Find-or-create the "Sky" entity carrying AtmosphereComponent.
        auto ensureSkyEntity = [this]() -> Entity {
            if (auto* p = m_world->GetPool<AtmosphereComponent>();
                p && !p->Entities().empty())
                return p->Entities()[0];
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, "Sky");
            m_world->AddComponent<AtmosphereComponent>(e, AtmosphereComponent{});
            return e;
        };

        if (ImGui::MenuItem("World Environment"))
            RecordSpawn("World Environment", [&] { m_selectedEntity = ensureEnvEntity(); });

        EditorTheme::MenuSection("Sky");
        if (ImGui::MenuItem("Procedural Atmosphere"))
            RecordSpawn("Sky - Procedural Atmosphere", [&] {
                setMode(WorldEnvironmentComponent::Mode::ProceduralAtmosphere);
                m_selectedEntity = ensureSkyEntity();
            });
        if (ImGui::MenuItem("Static Skybox (cubemap)"))
            RecordSpawn("Sky - Static Skybox", [&] {
                Entity sky = ensureSkyEntity();
                if (!m_world->HasComponent<SkyboxComponent>(sky))
                    m_world->AddComponent<SkyboxComponent>(sky, SkyboxComponent{});
                setMode(WorldEnvironmentComponent::Mode::StaticSkybox);
                m_selectedEntity = sky;
            });
        if (ImGui::MenuItem("Flat Color (no sky)"))
            RecordSpawn("Sky - Flat Color", [&] {
                m_selectedEntity = setMode(WorldEnvironmentComponent::Mode::FlatColor);
            });
        if (ImGui::MenuItem("None (no sky)"))
            RecordSpawn("Sky - None", [&] {
                m_selectedEntity = setMode(WorldEnvironmentComponent::Mode::None);
            });

        EditorTheme::MenuSection("Time of Day");
        auto createTaggedLight = [&](bool moon) {
            RecordSpawn(moon ? "Moon Light" : "Sun Light", [&] {
                Entity e = newSpatial(moon ? "Moon Light" : "Sun Light");
                // Aim via the transform - LightData has no direction (LightUtil.h).
                if (auto* lt = m_world->GetComponent<LocalTransform>(e))
                    lt->rotation = LightUtil::RotationAiming({ 0.447f, -0.894f, 0.224f });
                LightData ld;
                ld.type      = LightType::Directional;
                ld.color     = moon ? XMFLOAT3{ 0.55f, 0.70f, 1.0f }
                                    : XMFLOAT3{ 1.0f, 0.96f, 0.9f };
                ld.intensity = moon ? 0.2f : 1.0f;
                m_world->AddComponent<LightData>(e, ld);
                if (moon) m_world->AddComponent<MoonLightTag>(e, MoonLightTag{});
                else      m_world->AddComponent<SunLightTag>(e, SunLightTag{});
                m_selectedEntity = e;
            });
        };
        if (ImGui::MenuItem("Sun Light (TOD-driven)"))  createTaggedLight(false);
        if (ImGui::MenuItem("Moon Light (TOD-driven)")) createTaggedLight(true);
        if (ImGui::MenuItem("Time of Day Controller"))
        {
            RecordSpawn("Time of Day", [&] {
                Entity e = NullEntity;
                if (auto* p = m_world->GetPool<TODConfigComponent>();
                    p && !p->Entities().empty())
                    e = p->Entities()[0];
                if (e == NullEntity)
                {
                    e = m_world->CreateEntity();
                    m_world->SetName(e, "Time of Day");
                    TODConfigComponent cfg{};
                    cfg.enabled = true;
                    m_world->AddComponent<TODConfigComponent>(e, cfg);
                    m_world->AddComponent<TODOutputComponent>(e, TODOutputComponent{});
                }
                if (auto* wec = m_world->GetComponent<WorldEnvironmentComponent>(ensureEnvEntity()))
                    wec->enableTimeOfDay = true;
                m_selectedEntity = e;
            });
        }

        EditorTheme::MenuSection("Atmospherics");
        if (ImGui::MenuItem("Volumetric Clouds"))
            RecordSpawn("Volumetric Clouds", [&] {
                Entity sky = ensureSkyEntity();
                if (!m_world->HasComponent<CloudComponent>(sky))
                {
                    CloudComponent c{}; c.enabled = true;
                    m_world->AddComponent<CloudComponent>(sky, c);
                }
                m_selectedEntity = sky;
            });
        if (ImGui::MenuItem("Height Fog"))
            RecordSpawn("Height Fog", [&] {
                Entity sky = ensureSkyEntity();
                if (!m_world->HasComponent<HeightFogComponent>(sky))
                {
                    HeightFogComponent hf{}; hf.enabled = true;
                    m_world->AddComponent<HeightFogComponent>(sky, hf);
                }
                m_selectedEntity = sky;
            });

        EditorTheme::MenuSection("Fog");
        // Global volumetric fog is a singleton: an existing one is selected
        // instead of spawning a second (the renderer only reads the first).
        Entity fogEntity = NullEntity;
        if (auto* p = m_world->GetPool<VolumetricFogComponent>(); p && !p->Entities().empty())
            fogEntity = p->Entities()[0];
        if (ImGui::MenuItem("Volumetric Fog (global)"))
        {
            if (fogEntity != NullEntity)
                m_selectedEntity = fogEntity;
            else
                RecordSpawn("Volumetric Fog", [&] { m_selectedEntity = SpawnVolumetricFogFromLegacy(); });
        }
        if (fogEntity != NullEntity && ImGui::IsItemHovered())
            ImGui::SetTooltip("This scene already has its global fog - selects it.");
        if (ImGui::MenuItem("Local Fog Volume"))
            RecordSpawn("Local Fog Volume", [&] { m_selectedEntity = SpawnLocalFogVolume(); });

        EditorTheme::MenuSection("Post-Process");
        if (ImGui::MenuItem("Post-Process Volume (global)"))
            RecordSpawn("Post-Process Volume", [&] { m_selectedEntity = SpawnPostProcessVolume(true); });
        if (ImGui::MenuItem("Post-Process Volume (box)"))
            RecordSpawn("Post-Process Volume", [&] { m_selectedEntity = SpawnPostProcessVolume(false); });
        ImGui::EndMenu();
    }

    // ---- Effects (VFX) ----
    if (ImGui::BeginMenuEx("Effects", EditorTheme::Icon(FontAwesome::kFire)))
    {
        // Particle entities = a ParticleRenderComponent (look) + one behaviour.
        auto particle = [&](const char* name, auto behaviour) {
            RecordSpawn(name, [&] {
                Entity e = newSpatial(name);
                m_world->AddComponent<ParticleRenderComponent>(e, ParticleRenderComponent{});
                m_world->AddComponent<decltype(behaviour)>(e, behaviour);
                m_selectedEntity = e;
            });
        };
        EditorTheme::MenuSection("Particles");
        if (ImGui::MenuItem("Particle Emitter"))
            particle("Particle Emitter", ParticleEmitterComponent{});
        if (ImGui::MenuItem("Particle Flock (Boids)"))
            particle("Particle Flock", ParticleFlockComponent{});
        if (ImGui::MenuItem("Particle Ambient (camera field)"))
            particle("Particle Ambient", ParticleAmbientComponent{});

        EditorTheme::MenuSection("Geometry FX");
        if (ImGui::MenuItem("Billboard FX (Sprite)"))
        {
            RecordSpawn("Billboard FX", [&] {
                Entity e = newSpatial("Billboard FX");
                // Flipbook defaults - assign a sprite sheet + grid in the
                // Inspector. Additive + emissive so it glows through Bloom.
                BillboardFXComponent b{};
                b.columns  = 4;
                b.rows     = 4;
                b.fps      = 24.0f;
                b.size     = 1.0f;
                b.emissive = 1.0f;
                m_world->AddComponent<BillboardFXComponent>(e, b);
                m_selectedEntity = e;
            });
        }
        if (ImGui::MenuItem("Trail"))
        {
            RecordSpawn("Trail", [&] {
                Entity e = newSpatial("Trail");
                m_world->AddComponent<TrailComponent>(e, TrailComponent{});
                m_selectedEntity = e;
            });
        }
        if (ImGui::MenuItem("Beam (Procedural Tube)"))
        {
            RecordSpawn("Beam", [&] {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "Beam");
                // Identity transform - control points are authored in world space.
                m_world->AddComponent<LocalTransform>(e, LocalTransform{});
                m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

                // Two-point default beam along +X, anchored in front of the camera.
                const XMFLOAT3 spawn = ComputeSpawnPosition();
                BeamComponent bc;
                BeamControlPoint p0, p1;
                p0.position = { spawn.x,       spawn.y, spawn.z };
                p1.position = { spawn.x + 5.f, spawn.y, spawn.z };
                p0.radius   = p1.radius   = 0.3f;
                p0.colorTint = p1.colorTint = { 1.0f, 0.5f, 0.2f, 4.0f };
                bc.controlPoints     = { p0, p1 };
                bc.globalRadiusScale = 1.0f;
                bc.wobbleAmplitude   = 0.0f;
                bc.wobbleSpeed       = 1.0f;
                m_world->AddComponent<BeamComponent>(e, std::move(bc));

                // Opaque inner core; the outer glow is a separate entity created on demand.
                MaterialComponent mc;
                mc.useCustomShader  = true;
                mc.customShaderPath = "shaders/BeamTube_InnerCore.ps.hlsl";
                mc.userBlendMode    = BlendMode::Opaque;
                m_world->AddComponent<MaterialComponent>(e, std::move(mc));
                m_selectedEntity = e;
            });
        }
        ImGui::EndMenu();
    }

    // ---- UI ----
    // Two independent paths that never compose (see the ui-system docs):
    // screen-space Canvas widgets, and world-space billboards that follow an
    // entity. One menu, two labelled sections.
    if (ImGui::BeginMenuEx("UI", EditorTheme::Icon(FontAwesome::kImage)))
    {
        EditorTheme::MenuSection("Screen Space (Canvas)");

        // Parent new elements to the canvas of the selection (walk up from it), not the selection,
        // so successive Create calls make siblings under the canvas instead of nesting. With no
        // canvas node selected: the only canvas when there is exactly one, else none. Only canvases
        // the scene authors count (AuthoredCanvas): a UI Editor stage's or a UI Document instance's
        // would take the widget out of the scene. The widgets are the UI Editor Palette's
        // (Editor::UITemplates), centred on the canvas.
        auto findUIParent = [&]() -> Entity
        {
            const Entity c = UI::FindOwningCanvas(*m_world, m_selectedEntity);
            if (c != NullEntity && AuthoredCanvas(*m_world, c)) return c;
            Entity only = NullEntity;
            int    n    = 0;
            if (const auto* cp = m_world->GetPool<UI::UICanvas>())
                for (const Entity e : cp->Entities())
                    if (AuthoredCanvas(*m_world, e) && n++ == 0) only = e;
            return n == 1 ? only : NullEntity;
        };

        if (ImGui::MenuItem("Canvas (root)"))
            RecordSpawn("UI Canvas", [&] {
                Entity c = m_world->CreateEntity();
                m_world->SetName(c, "Canvas");
                m_world->AddComponent<UI::UICanvas>(c, UI::UICanvas{});
                m_selectedEntity = c;
            });

        const Entity parent = findUIParent();
        const bool   canAdd = (parent != NullEntity);
        if (!canAdd)
            ImGui::TextDisabled(m_world->GetPool<UI::UICanvas>() && m_world->GetPool<UI::UICanvas>()->Size() > 1
                                    ? "(select a node of the canvas to add to)"
                                    : "(create a Canvas first, or select one)");

        for (int i = 0; i < static_cast<int>(Editor::UITemplates::Widget::Count); ++i)
        {
            const auto w = static_cast<Editor::UITemplates::Widget>(i);
            if (ImGui::MenuItem(Editor::UITemplates::Label(w), nullptr, false, canAdd))
            {
                const std::string label = std::string("UI ") + Editor::UITemplates::Label(w);
                RecordSpawn(label.c_str(), [&] {
                    const Entity e = Editor::UITemplates::Create(*m_world, w, parent);
                    if (e != NullEntity) m_selectedEntity = e;
                });
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Editor::UITemplates::Tooltip(w));
        }

        EditorTheme::MenuSection("World Space (on selected entity)");

        // World-space UI follows the SELECTED entity: LocalTransform + Global +
        // FollowEntity + WorldSpaceUI, offset upward by yOffset.
        const bool hasTarget = m_selectedEntity != NullEntity
                            && m_world->IsAlive(m_selectedEntity)
                            && m_world->HasComponent<GlobalTransform>(m_selectedEntity);
        const Entity target = m_selectedEntity;
        auto spawnWorldUIChild = [&](const char* name, float yOffset, XMFLOAT2 baseSize) -> Entity
        {
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, name);
            LocalTransform lt;
            lt.translation = { 0.f, yOffset, 0.f };
            lt.scale       = { 1.f, 1.f, 1.f };
            XMStoreFloat4(&lt.rotation, XMQuaternionIdentity());
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

            // lt.translation carries yOffset; FollowSystem composes it against
            // the target's world matrix.
            FollowEntityComponent fe;
            fe.target.BindAndStamp(*m_world, target);
            m_world->AddComponent<FollowEntityComponent>(e, fe);

            UI::WorldSpaceUIComponent ws;
            ws.baseSize    = baseSize;
            ws.pivot       = { 0.5f, 1.0f };
            ws.scalingMode = UI::ScalingMode::ConstantWorld;
            ws.fadeFar     = 50.0f;
            m_world->AddComponent<UI::WorldSpaceUIComponent>(e, ws);
            return e;
        };
        auto targetName = [&]() -> std::string {
            std::string n = m_world->GetName(target);
            return n.empty() ? std::string("Entity") : n;
        };

        if (ImGui::MenuItem("Text Label", nullptr, false, hasTarget))
            RecordSpawn("World UI Text", [&] {
                Entity e = spawnWorldUIChild("Floating Text", 2.5f, { 1.0f, 0.4f });
                UI::WorldUITextComponent t;
                t.text  = targetName();
                t.color = { 1.f, 1.f, 1.f, 1.f };
                t.scale = 1.0f;
                m_world->AddComponent<UI::WorldUITextComponent>(e, t);
                m_selectedEntity = e;
            });
        if (ImGui::MenuItem("HP Bar", nullptr, false, hasTarget))
            RecordSpawn("World UI HP Bar", [&] {
                Entity e = spawnWorldUIChild("HP Bar", 2.0f, { 1.2f, 0.18f });
                UI::WorldUIBarComponent bar;
                bar.value = 0.7f;
                m_world->AddComponent<UI::WorldUIBarComponent>(e, bar);
                m_selectedEntity = e;
            });
        if (ImGui::MenuItem("Name + HP Bar", nullptr, false, hasTarget))
            RecordSpawn("World UI Name + HP Bar", [&] {
                Entity bar = spawnWorldUIChild("World HP Bar", 2.0f, { 1.4f, 0.20f });
                UI::WorldUIBarComponent bc;
                bc.value = 1.0f;
                m_world->AddComponent<UI::WorldUIBarComponent>(bar, bc);
                Entity label = spawnWorldUIChild("World Name Label", 2.5f, { 1.4f, 0.30f });
                UI::WorldUITextComponent tc;
                tc.text = targetName();
                m_world->AddComponent<UI::WorldUITextComponent>(label, tc);
                m_selectedEntity = bar;
            });
        // Bursts upward with auto-fade + auto-destroy.
        if (ImGui::MenuItem("Damage Number", nullptr, false, hasTarget))
            RecordSpawn("World UI Damage Number", [&] {
                Entity e = spawnWorldUIChild("Damage Number", 1.8f, { 0.6f, 0.4f });
                UI::DamageNumberComponent dn;
                dn.text          = "100";
                dn.lifetime      = 1.5f;
                dn.totalLifetime = 1.5f;
                dn.velocity      = { 0.f, 1.5f, 0.f };
                dn.color         = { 1.0f, 0.4f, 0.3f, 1.0f };
                m_world->AddComponent<UI::DamageNumberComponent>(e, dn);
                m_selectedEntity = e;
            });
        if (!hasTarget)
            ImGui::TextDisabled("(select an entity with a transform first)");
        ImGui::EndMenu();
    }
}

// ==== Fog helpers ====
// Used by the Create menu, the Environment window and the Volumetric Fog inspector.
Entity EditorLayer::SpawnVolumetricFogFromLegacy()
{
    VolumetricFogComponent c{};
    c.enabled = true;
    // Seed from the pass's own settings so adding the component does not change the
    // picture. Pass: sigma_s = d*scattering, sigma_t = d*(scattering + absorption);
    // component: sigma_t = density, sigma_s = sigma_t*albedo.
    if (VolumetricFogPass* vf = m_renderer ? m_renderer->GetVolumetricFogPass() : nullptr)
    {
        const float s  = vf->GetScattering();
        const float a  = vf->GetAbsorption();
        const float st = s + a;
        const float albedo = st > 0.0f ? std::clamp(s / st, 0.0f, 1.0f) : 1.0f;
        c.density          = vf->GetDensity() * st;
        c.albedo           = { albedo, albedo, albedo };
        c.anisotropy       = vf->GetAnisotropy();
        c.baseHeight       = vf->GetHeightStart();
        c.heightFalloff    = vf->GetHeightFalloff();
        c.ambientColor     = vf->GetAmbientColor();
        c.ambientIntensity = vf->GetAmbientContribution();
        c.startDistance    = vf->GetRangeNear();
        c.viewDistance     = vf->GetRangeFar();
        c.godRays          = vf->IsRaymarchEnabled();
        c.temporal         = vf->IsTemporalEnabled();
        c.temporalBlend    = vf->GetTemporalAlpha();
    }
    Entity e = m_world->CreateEntity();
    m_world->SetName(e, "Volumetric Fog");
    m_world->AddComponent<VolumetricFogComponent>(e, c);
    return e;
}

Entity EditorLayer::SpawnLocalFogVolume()
{
    Entity e = m_world->CreateEntity();
    m_world->SetName(e, "Local Fog Volume");
    LocalTransform lt{};
    lt.translation = ComputeSpawnPosition();
    lt.scale       = { 4.0f, 2.0f, 4.0f };   // half-extents: an 8 x 4 x 8 m box
    m_world->AddComponent<LocalTransform>(e, lt);
    m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
    m_world->AddComponent<LocalFogVolumeComponent>(e, LocalFogVolumeComponent{});
    return e;
}

Entity EditorLayer::SpawnPostProcessVolume(bool global)
{
    Entity e = m_world->CreateEntity();
    m_world->SetName(e, global ? "Global Post-Process Volume" : "Post-Process Volume");
    LocalTransform lt{};
    lt.translation = ComputeSpawnPosition();
    if (!global) lt.scale = { 5.0f, 3.0f, 5.0f };   // half-extents: a 10 x 6 x 10 m box
    m_world->AddComponent<LocalTransform>(e, lt);
    m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
    ECS::PostProcessVolumeComponent vc{};
    vc.isGlobal = global;
    // A runtime profile until the artist saves it as a .ppprofile (Inspector).
    vc.profile  = PostProcess::ProfileSystem::Get().CreateRuntime();
    m_world->AddComponent<ECS::PostProcessVolumeComponent>(e, vc);
    return e;
}

void EditorLayer::DrawVolumetricFogStatus()
{
    if (!m_world) return;
    const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);

    // ---- Light shafts: the sun opts in with a Volumetric Light component ----
    // Prefer the TOD-tagged sun; otherwise the first directional light.
    Entity sun = NullEntity;
    if (auto* p = m_world->GetPool<SunLightTag>(); p && !p->Entities().empty())
        sun = p->Entities()[0];
    if (sun == NullEntity)
        if (auto* p = m_world->GetPool<LightData>())
        {
            const auto& ents = p->Entities();
            const auto& data = p->Data();
            for (size_t i = 0; i < ents.size(); ++i)
                if (data[i].type == LightType::Directional) { sun = ents[i]; break; }
        }
    if (sun == NullEntity)
        ImGui::TextDisabled("Light shafts: no directional light in the scene.");
    else
    {
        const auto* vl = m_world->GetComponent<VolumetricLightComponent>(sun);
        const std::string name = m_world->GetName(sun);
        if (vl && vl->enabled)
            ImGui::TextDisabled("Light shafts: '%s' scatters into the fog.", name.c_str());
        else if (vl)
            ImGui::TextColored(kWarn, "Light shafts: the Volumetric Light on '%s' is disabled.",
                               name.c_str());
        else
        {
            ImGui::TextColored(kWarn, "Light shafts: '%s' does not light the fog.", name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Enable on Sun"))
                AddComponentWithUndo(sun, std::type_index(typeid(VolumetricLightComponent)),
                    [](World& w, Entity e) {
                        w.AddComponent<VolumetricLightComponent>(e, VolumetricLightComponent{});
                    });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Adds a Volumetric Light component to the sun (UE's\n"
                                  "'Volumetric Scattering Intensity' lives on the light too).");
        }
    }

    // ---- Local volumes ----
    if (m_renderer)
    {
        const uint32_t total = m_renderer->GetLocalFogVolumeTotal();
        const uint32_t kept  = m_renderer->GetLocalFogVolumeKept();
        if (total == 0)
            ImGui::TextDisabled("Local fog volumes: none (Create > Environment > Local Fog Volume).");
        else
            ImGui::TextDisabled("Local fog volumes: %u active, %u within view distance (max %u).",
                                total, kept, VolumetricFogPass::kMaxLocalFogVolumes);
    }
}
