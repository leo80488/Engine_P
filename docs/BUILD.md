**English** | [日本語](BUILD.ja.md)

# Build

## Prerequisites

- Windows x64, a logged-in desktop and a hardware DX12 GPU suitable for the engine's Shader Model 6.6 path.
- Visual Studio 2022, MSVC v143, Desktop development with C++ and a Windows SDK.
- CMake 3.24+ and Python 3.10+.
- Optional Ninja, run from an x64 Visual Studio developer shell.

Hidden runs still create a window and require a GPU. This build has no GPU-free headless or WARP fallback.

## Configure and build

From the repository root:

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
cmake --build --preset vs-x64-debug
```

The final line builds Debug if needed. If Python resolves to a Store alias, select an installed interpreter:

```powershell
cmake --preset vs-x64 -DPython3_EXECUTABLE="C:/path/to/python.exe"
```

The VS preset selects x64 tools and writes to `DX12/build/ci/cmake/vs-x64/`. Executables are in `bin/Release/` or `bin/Debug/` below it. Open this CMake project in Visual Studio, or its generated solution in the build directory. Its debugger working directory is `DX12/`.

### Ninja

From an x64 Native Tools for VS 2022 shell with Ninja on PATH:

```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
```

Ninja uses the compiler environment of the shell; it cannot select a Visual Studio architecture itself.

### Manual configure

```powershell
cmake -S . -B DX12/build/ci/cmake/custom -G "Visual Studio 17 2022" -A x64 -T host=x64
cmake --build DX12/build/ci/cmake/custom --config Release --parallel 12
```

CMake compiles the engine and Jolt directly. Configure-time Python generates source ownership/module registration; build-time generators maintain material layout and build identity.

## Dependencies

Core headers, libraries and DLLs are included. Configure does not download optional SDKs or run a package manager.

| Dependency | Expected layout / role |
| --- | --- |
| DirectX headers, DirectXTex, DXC | `DX12/external/`; DXC DLLs are staged beside each app |
| Jolt | `external/Jolt/`; compiled as EngineJolt |
| FreeType | Headers/library under external and `DX12/freetype.dll` |
| Assimp | `external/assimp/` import libraries/runtime; Editor only |
| NRD | `Include/`, `_Bin/<config>/NRD.lib`, `_Build/_deps/shadermake-build/<config>/ShaderMakeBlob.lib` |
| FidelityFX | `FidelityFX/api/include`, `upscalers/include`, loader import library and DLLs in `FidelityFX/signedbin` |
| Geometry/navigation | meshoptimizer and Recast/Detour headers plus matching Debug/Release libraries |
| UI/scripting | ImGui, ImPlot, node editor, Lua, sol2, JSON, zstd and Tracy headers |

Override core roots with `ENGINE_NRD_SDK_DIR` or `ENGINE_FFX_SDK_DIR`. Their layouts must match the table. Missing required files stop configuration with the exact path.

From `DX12/`, `python tools/fetch_deps.py --verify` checks the hash-pinned payload subset. Those files are tracked in Git. The older manifest's download URLs are not a complete public dependency bootstrap service.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `ENGINE_BUILD_EDITOR` | ON | Editor and EditorCore |
| `ENGINE_BUILD_GAME` | ON | Game host |
| `ENGINE_ENABLE_HARNESS` | ON | Bounded runs and ledgers |
| `ENGINE_ENABLE_CONTROL_CHANNEL` | ON | Opt-in local channel, enabled at runtime explicitly |
| `ENGINE_ENABLE_TRACY` | OFF | Tracy client/instrumentation |
| `ENGINE_ENABLE_NAV` | ON | Navigation module |
| `ENGINE_ENABLE_MEDIA` | OFF | Requires FFmpeg headers/libs/DLLs |
| `ENGINE_ENABLE_VIDEO` | OFF | Requires FFmpeg payload |
| `ENGINE_ENABLE_FACE_TRACKING` | OFF | Enables Media; needs ONNX Runtime and content |
| `ENGINE_ENABLE_STREAMLINE` | OFF | Requires separately acquired SDK/runtime |
| `BUILD_TESTING` | ON | CTest startup tests |

Rebuild after changing a compile option. A module enabled only in `game.json` cannot supply code excluded at link time. Optional SDK builds are outside the default verification scope.

Native configurations are Debug and Release. They do not model the upstream project's full Profile/Retail cook/shipping matrix.

## Run

```powershell
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
& .\build\ci\cmake\vs-x64\bin\Release\Game.exe -scene=BaseScene -settings=settings.default.json
```

The starter has a camera, directional light and cube. F11 switches full viewport/docking editor. Generated runtime state is ignored by Git.

## Test

See [AI Harness](AI_HARNESS.md) for runnable examples, Lua scenario authoring, evidence acceptance and local control integration.

```powershell
ctest --preset vs-x64-release
```

CTest launches the actual CMake-built executables. It checks bounded completion, process/ledger exit codes, frame evidence, error/access counters and unread flags. Outputs go to `DX12/tests/_artifacts/cmake/`.

The test wrapper uses Python's standard library. Inspect screenshots/logs to confirm the intended picture; a startup test does not certify every renderer mode or optional SDK.

## Install and package

```powershell
cmake --install DX12/build/ci/cmake/vs-x64 --config Release --prefix DX12/build/package/standalone
cmake --build --preset vs-x64-release --target package
```

Installation includes apps, required runtime DLLs, shaders, minimal content, defaults and notices. Asset identity sidecars are retained. Run installed apps from the installed root.

CPack writes ZIPs under `DX12/build/package/`. These are loose-file development builds with runtime shader compilation, not the original project's cooked Retail product. The matching x64 Visual C++ runtime must be installed separately on machines without it.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Compiler/x64 configure error | VS C++ workload and x64 preset |
| Python opens Store | Explicit `Python3_EXECUTABLE` |
| Missing NRD/FFX file | SDK root and exact directory layout |
| Link failure | Debug/Release flavor and complete vendor libraries |
| Missing runtime DLL | Post-build staging; do not mix SDK versions |
| Missing shader/resource | Working directory or complete installed tree |
| Slow first build | Jolt, PCHs and all engine/editor sources must compile |
| Unexpected scene | game.json and `-scene`; old game content is excluded |
| Black/overexposed view | Current lighting/materials and that run's evidence |
| No run.json | Startup failed before a valid harness result |
