**English** | [日本語](ARCHITECTURE.ja.md)

# Architecture


## Targets and ownership

```text
Editor.exe -- EditorCore: editor windows, importers, ImGui
     |------- EngineCore: renderer, ECS, resources, scripting
     |               `-- EngineJolt
     `------- selected EngineModule_* libraries
Game.exe ---- EngineCore + selected EngineModule_* libraries
```

EngineCore is compiled without `WITH_EDITOR`. Host differences use host policy or editor hooks. EditorCore compiles editor code with `WITH_EDITOR` and its ImGui definitions.

`main.cpp`, `App.cpp`, `App_Render.cpp` and the generated module registration unit are compiled separately for Editor and Game. App declarations and module registration differ between hosts, so compiling those files once in a shared library would break their layout contract. Each target builds its own PCH.

Jolt is built from source. Its feature definitions must agree with every consumer of Jolt headers; CMake propagates them consistently.

## Source selection

Configure runs `DX12/tools/cmake_configure.py`. The generator reads source-ownership rules and module descriptors to write `Sources.cmake` and registration code in the build tree. It does not invoke MSBuild or parse original project files to compile targets.

New `.cpp` files under `src/` are discovered on CMake's next configure. Module ownership comes from `src/Modules/*.module.json`. Editor helpers outside `src/Editor/` have explicit ownership rules.

## Frame execution

App owns startup, shutdown and the outer loop. FramePipeline coordinates frame phases, including editor hooks. ECS systems update scene data before the renderer prepares GPU-facing draw, material, light and visibility data.

Render Graph describes passes and resource dependencies. Graphics creates DX12 objects, records commands and submits device work. CPU preparation, graph scheduling and GPU submission are separate responsibilities.

## ECS and lifetime

Component pools provide storage; systems declare access for legal parallel batches. Transform hierarchy propagation and world teardown are part of that contract. A cached handle does not survive a scene transition merely because its integer index is reused.

## Resource boundaries

| Root | Ownership |
| --- | --- |
| `DX12/engine/` | Defaults, primitives, LUTs and editor fonts |
| `DX12/asset/` | Project content; only boot/input configuration in this edition |
| `DX12/shaders/` | Built-in renderer implementations |
| `DX12/external/` | Third-party source and required prebuilt payload |
| `DX12/build/` | Generated files, compiler outputs and packages |

Runtime handles track loading/lifetime; GUIDs identify authored resources across moves. Keep `.meta` sidecars and use the engine codec for native binary formats.

## Configuration

`game.json` declares identity, scenes, startup and runtime modules. `engine.json` contains developer/render configuration. `settings.json` stores player options; `settings.default.json` is the clean installation/test template. Editor preferences and layout are local state.

CMake selects what is linked. The project manifest selects what is enabled. Both must agree when adding an optional integration.

## Extension points

- Add engine functionality in EngineCore, editor windows/importers in EditorCore and integrations through modules.
- Declare Render Graph access and maintain CPU/HLSL layout agreement.
- Preserve ECS access declarations and world-lifetime handling.
- Keep project headers out of the PCH; external and C sources do not consume the engine C++ PCH.

See [Engine Systems](ENGINE.md), [Rendering](RENDERING.md) and [Build](BUILD.md).
