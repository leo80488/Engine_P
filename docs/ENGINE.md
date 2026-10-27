**English** | [日本語](ENGINE.ja.md)

# Engine Systems

## ECS and scheduling

The ECS stores components in pools and uses compact entity references. Systems declare their data access, allowing the scheduler to form legal parallel batches. Scene teardown invalidates world-owned handles and cached pointers; reacquire them after transitions.

Hierarchy propagation is separate from storage. Parenting affects world transforms, serialization and editor operations. A renderer-facing transform is propagated frame state, not necessarily the local Transform most recently edited.

## Native resources

GUIDs and `.meta` sidecars identify authored files; runtime handles track loading and lifetime. Async work can include decoding, upload and finalization. Receiving a handle does not mean the resource is ready.

| Format | Role |
| --- | --- |
| `.iscene` | Entity/component world scene |
| `.iscn`, `.meshlib` | Model hierarchy and geometry |
| `.ipfb` | Prefab |
| `.itex` | Texture container |
| `.imat`, `.imatinst`, `.imatgraph` | Material, instance and graph |
| `.iskel`, `.ianim` | Skeleton and animation |
| `.iui` | UI document |
| `.iseq` | Sequencer asset |
| `.ipak` | Packaged resource container |

Native formats have versioned headers/metadata and may contain binary payloads. Use the editor or native codec, not whole-file text replacement. Import pipelines cover textures/models, PMX/VMD, VRM, animation/skeleton data and Unreal scene inputs. Source import capability does not include those assets.

### Resource loading lifecycle

ResourceManager uses `NotLoaded`, `Loading`, `Ready` and `Failed` states. Unload during loading/failure also invalidates the handle as described below. A load request returns a handle before readiness. Some resources require deferred GPU finalization; downstream texture/mesh systems can also have their own GPU-readiness state. Check the state needed by the consumer rather than assuming a successful file read makes a draw ready.

| Reference | Validity / responsibility |
| --- | --- |
| Asset GUID + `.meta` | Persistent authored identity across file moves |
| Resource Handle | Slot generation; unload/recycle makes old handles stale |
| Borrowed `Resource*` | Short-lived query result; do not retain across unload or frames |
| Entity/world reference | Session/world lifetime; reacquire after scene changes |

See [ResourceManager.h](../DX12/include/Resource/ResourceManager.h). An unload during loading invalidates the slot, so a late worker result cannot populate a new resource that reused it.

## Content and scenes

`engine/` contains defaults, primitive geometry, a BRDF LUT, starter scene and required fonts. `asset/` is project-owned content. This edition includes only an empty boot manifest and Keys/Pad/InputContexts configuration, so no original gameplay service or scene script boots.

`game.json` registers scene names and selects startup. A scene can reference scripts and scene-scoped systems. Game-flow states and transitions sit above that registry; a state may request the first world instead of startup loading it immediately.

The starter contains no player controller, enemy setup, quests or title screen. Register your own scene before loading names from another project. Keep resource identity sidecars when moving files.

## Settings

| File | Purpose |
| --- | --- |
| `game.json` | Identity, scenes, startup and runtime modules |
| `engine.json` | Developer/render configuration |
| `settings.json` | Player options |
| `settings.default.json` | Clean installation/test defaults |
| `input_profiles.json` | Input profiles |
| `editor_prefs.default.ini`, `imgui.default.ini` | Seeds for local editor state |

Runtime settings have typed rows and policy layers. A value may come from scene state, an override or a command-line pin. A saved JSON value alone does not prove which renderer setting is currently active.

## Input and character control

Input provides keyboard/mouse and controller state, action bindings and mapping contexts. Keys/Pad expose stable Lua names. InputContexts layers gameplay/menu mappings; pushes and pops must remain balanced.

Character control separates decisions/intent from physical movement. Locomotion and animation drivers consume that state. The source includes these systems, but no character is created by the starter scene.

## Cameras

Camera components/controllers support editor navigation, runtime views and camera selection. The Edit camera and the playing world's selected camera are distinct. A screenshot or preview should identify which view produced it.

## Animation and retargeting

Animation covers GPU skinning, morphs, state/blend systems, notifies, IK, sockets and secondary chain motion. Skeleton names and transforms matter: successfully loading a clip does not establish compatibility with a different skeleton.

Humanoid tools map and bake motion onto a target rig. Retargeting does not make arbitrary clips interchangeable at runtime. The repository contains no model or animation library.

## Physics

Jolt supplies rigid-body and character-controller primitives. The engine manages collision layers, contacts/events, scene lifetime and transform synchronization. CMake builds Jolt with the feature definitions consumed by the engine.

Collision shapes and visible meshes are separate authoring data. Replacing render geometry does not automatically produce suitable collision.

## Navigation and AI

Nav uses Recast/Detour and is linked by default. No project navmesh is included; bake or import one for your scene.

AI provides behavior, intent and update/LOD machinery. Original action-library scripts are excluded, with their script modules disabled. Enabling one without the script content is not a valid project setup.

## Lua

Lua/sol2 provide bindings, component access, callbacks, services and systems. `asset/scripts/script_manifest.json` declares boot entries; the starter array is empty. Core input configuration is loaded independently by binding initialization.

Check the current source/runtime schema for accepted names and types. Preserve an SDK entity reference's session/world information rather than reusing its integer ID after a scene switch.

## Audio and optional media

Audio integrates XAudio2/X3DAudio with resource loading, mixing and spatial emitters/listeners. Supply your own audio data; game music and sound libraries are not shipped.

Media/Video cover decoding and presentation. FaceTracking covers camera/video/network sources, mappings and runtime integration. They are disabled by default and require matching FFmpeg/ONNX Runtime payloads and authored content. Face models/mappings are not included.

## Sequencer and timelines

Sequencer assets own timeline evaluation and target resolution. The editor authors tracks, while runtime evaluation participates in the frame lifecycle. World changes invalidate resolved targets and need rebinding. Animation notifies and effect/audio events should respect that lifetime.

## UI and fonts

UI documents represent authored hierarchies instead of hardcoding every interface into a scene. Runtime text uses a font library; editor UI uses Noto Sans, CJK faces and Font Awesome.

The historical default path `engine/font/FGMiraiRen.ttf` contains Noto Sans Regular in this edition, accompanied by OFL. This avoids copying the original project's broader font collection.

## Editor

The docking editor includes Outliner, Inspector, Content Browser, viewport tools, undo/redo and Edit/Play behavior. F11 switches between docking and full viewport.

| Area | Tools |
| --- | --- |
| Scenes | Hierarchy, transforms, properties and undo/redo |
| Materials | Materials/instances, graph authoring, previews |
| Resources | Texture Viewer, mesh/model and skeleton inspection |
| Animation | Sets, retargeting, secondary motion and timelines |
| Presentation | UI documents, fonts, VFX and sequencer |
| Diagnostics | Graphics settings, render graph, profiler and debug targets |

Asynchronous authoring operations may return a job before the result is visible. Wait for completion before inspecting/saving dependent state. Stopping Play should restore the editing world rather than persist simulation accidentally.

## Diagnostics and extension

Bounded runs write a ledger, per-frame evidence, log and optional captures/state. CTest checks these records as well as exit codes. The local control channel is opt-in at runtime, evaluates engine bindings and is not a sandbox. A responding status endpoint alone does not prove frame progression.

Maintain ownership, reflection/serialization, CPU/GPU layouts and lifetime contracts when extending a system. Add independent fixtures for new behavior; the starter startup tests do not exercise a complete game.
