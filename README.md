**English** | [日本語](README.ja.md)

# DX12Engine

A C++20 / Direct3D 12 engine and editor with a native CMake build. Shaders target Shader Model 6.6 and are compiled to DXIL with DXC.

This source distribution contains the current engine implementation and a small built-in startup scene. Game characters, animation libraries, commercial environments and the original game project are excluded.

## Demo

Earlier demonstrations, using project content that is not included here:

<table>
<tr>
<td><a href="https://youtu.be/6CPXLe-J1sw"><img width="400" src="https://img.youtube.com/vi/6CPXLe-J1sw/hqdefault.jpg" alt="DX12Engine rendering demo"></a></td>
<td><a href="https://youtu.be/mi7Asi6O87Y"><img width="400" src="https://img.youtube.com/vi/mi7Asi6O87Y/hqdefault.jpg" alt="DX12Engine editor demo"></a></td>
</tr>
<tr>
<td><a href="https://youtu.be/0zyT00WBlOE"><img width="400" src="https://img.youtube.com/vi/0zyT00WBlOE/hqdefault.jpg" alt="DX12Engine scene demo"></a></td>
<td><a href="https://youtu.be/juu7YkzafQs"><img width="400" src="https://img.youtube.com/vi/juu7YkzafQs/hqdefault.jpg" alt="DX12Engine character demo"></a></td>
</tr>
</table>

## Features

**Rendering**

- Deferred G-buffer, bindless resources, GPU culling and indirect draw submission.
- Clustered lighting, cascaded directional shadows, local-light and per-object shadow paths.
- Hardware ray queries, RT shadows/AO, hybrid reflections, DDGI and reflection probes.
- ReSTIR implementations, NPR character rendering, outlines, decals and custom material shaders.
- Atmosphere, clouds, volumetric effects, terrain, grass, water and interactive ripples.
- TAA, FXAA, FidelityFX upscaling, HDR and a post-processing stack.
- Optional Streamline integration; its SDK/runtime is acquired separately.

**Engine**

- ECS with declared system access, dependency-aware scheduling and scene hierarchy.
- Native resource formats, GUID identity, async loading, hot reload and pak support.
- GPU skinning, morphs, IK, animation systems, humanoid retargeting and chain physics.
- Jolt physics, character control, navigation, AI, input, cameras, audio and Lua/sol2.
- UI documents, material graphs, sequencer timelines and layered settings.
- Bounded diagnostic runs with frame evidence and an opt-in local control channel.

**Editor**

- Docking viewport, Outliner, Inspector and Content Browser with undo/redo and Edit/Play.
- Material/texture tools, animation/skeleton authoring, UI editing, previews and render-graph inspection.
- Project configuration and explicit optional engine modules.

These are implementation areas. Nav is enabled by default; optional SDK modules are off. See [Status](docs/STATUS.md) for availability and tested scope.

## Build

Install Visual Studio 2022 with the C++ desktop workload, a Windows SDK, CMake 3.24+ and Python 3.10+.

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
ctest --preset vs-x64-release
```

From the repository root:

```powershell
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
```

CMake builds EngineCore, EditorCore, selected module libraries and Jolt directly from source. It does not invoke the original project solution as a wrapper. Core dependencies are included; optional SDKs are not downloaded automatically.

Full instructions, options, installation and packaging: [Build](docs/BUILD.md).

## Documentation

Start with [Getting Started](docs/GETTING_STARTED.md) for the first editor/scene/Game workflow, then use Architecture for ownership and Rendering/Engine Systems for implementation details.

| Document | Contents |
| --- | --- |
| [Getting Started](docs/GETTING_STARTED.md) | First launch, scene authoring, registration and Game verification |
| [Architecture](docs/ARCHITECTURE.md) | Targets, ownership, frame execution and resource boundaries |
| [Rendering](docs/RENDERING.md) | Pipeline, lighting, materials, effects and pass source index |
| [Engine Systems](docs/ENGINE.md) | ECS, resources, animation, physics, scripting, UI and editor |
| [Build](docs/BUILD.md) | CMake, dependencies, tests, installation and troubleshooting |
| [Credits & References](docs/CREDITS.md) | Libraries, fonts and implementation references |
| [Status & Limitations](docs/STATUS.md) | Distribution scope and verification limits |

These are user-facing documents. Internal design plans, development backlogs and migration reports are not included.

## Repository layout

| Path | Contents |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json` | Public build entry points |
| `docs/` | English and Japanese user documentation |
| `DX12/src/`, `DX12/include/` | Engine and editor implementation |
| `DX12/shaders/` | Built-in renderer shaders |
| `DX12/engine/` | Minimal built-in resources and editor fonts |
| `DX12/asset/` | Empty boot manifest and core input configuration |
| `DX12/external/` | Bundled third-party dependencies |
| `DX12/cmake/`, `DX12/tools/` | Dependencies, generators and utilities |
| `DX12/build/`, `DX12/log/`, `DX12/tests/_artifacts/` | Ignored local outputs |

## Licenses

Third-party components retain their licenses; see [Credits](docs/CREDITS.md) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). This distribution does not assign a new license to the engine's original code.
