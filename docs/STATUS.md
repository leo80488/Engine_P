**English** | [日本語](STATUS.ja.md)

# Status & Limitations

## Distribution

The repository contains the current engine/editor source, core dependencies and minimal startup content. The first scene has a camera, directional light and cube. Project scripts are limited to input maps and an empty boot manifest.

Game characters, animations, commercial scenes, sound libraries, gameplay scripts and private project history are excluded. Internal design plans, historical implementation proposals, backlogs and export audit reports are absent from the documentation tree.

## Availability

| Area | Status |
| --- | --- |
| Renderer, ECS, resources, editor | Source and necessary startup resources included |
| Jolt | Native CMake source build |
| Nav | Linked by default; no game navmesh |
| Material/animation/UI/sequencer tools | Source included; use your own content |
| FidelityFX / NRD core | Required payload included |
| Media / Video | Off; no FFmpeg runtime payload |
| FaceTracking | Off; no ONNX Runtime or face models/mappings |
| Streamline / DLSS | Off; SDK/runtime acquired separately |
| RTXDI / SHaRC | SDK trees excluded; dependent backends absent by default |
| Tracy | Source/headers present; instrumentation off |

## Platform and content limits

Windows x64, MSVC and hardware DX12 are required. Hidden tests still need a desktop/GPU. This build provides no WARP or Linux/macOS renderer.

Advanced paths have additional GPU/driver requirements. A successful starter scene does not certify ray tracing, mesh shaders, HDR or temporal SDKs on every device.

There is no sample game or character showcase. Some authoring previews reference the excluded autumn IBL set; supply your own environment. The default material's historically named ramp texture is a required material dependency, not a character asset.

## Build and verification scope

CMake provides Debug/Release, feature options, CTest startup, installation and ZIP packaging. The package is a loose-file development distribution with runtime shader compilation, not the original project's full Profile/Retail cook/shipping pipeline.

Verified on Windows x64 with MSVC 19.44 / Windows SDK 10.0.26100.0:

| Check | Result |
| --- | --- |
| Native CMake Release build | EngineCore, EditorCore, Jolt, Nav, Editor and Game built successfully |
| Reconfigure/rebuild without upstream solution/project files | Passed |
| CTest | Editor and Game both passed, 90 frames each, strict access checks, zero engine errors |
| Visual inspection | The starter cube renders in the CMake-built Game capture |
| CMake installation | Passed |
| Installed Game and Editor | Both passed independent 60-frame runs from the installed root |
| CPack ZIP | Created successfully |
| Debug, Ninja and optional SDK configurations | Provided where applicable, not executed in this validation |

These results apply to this Windows/GPU setup and the default module set.

Startup tests check actual CMake executables, bounded completion, ledger/frame evidence and counters. They do not represent the original project's complete gameplay/rendering test corpus.

## Known diagnostics

The renderer can emit existing constant-buffer whole-CBV warnings at startup. Core prebuilt NRD libraries may also emit linker warnings for absent vendor PDB files; this affects debug-symbol availability for those library objects. New engine errors are not accepted by the tests.

## Licensing

Third-party components retain their licenses. The engine's original-code license remains an owner decision. See [Credits](CREDITS.md).
