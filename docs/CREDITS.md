**English** | [日本語](CREDITS.ja.md)

# Credits & References

## Libraries

Components retain their original ownership. License texts/notices accompany the relevant payload and [THIRD_PARTY_NOTICES.txt](../THIRD_PARTY_NOTICES.txt).

| Component | Role | Upstream |
| --- | --- | --- |
| Jolt | Physics/character simulation | [JoltPhysics](https://github.com/jrouwe/JoltPhysics) |
| ImGui / ImGuizmo | Editor UI and manipulation | [ImGui](https://github.com/ocornut/imgui), [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) |
| ImPlot / node editor | Plots and graph authoring | [ImPlot](https://github.com/epezent/implot), [node editor](https://github.com/thedmd/imgui-node-editor) |
| ImGui test engine / club | Editor testing/utilities | [test engine](https://github.com/ocornut/imgui_test_engine), [imgui_club](https://github.com/ocornut/imgui_club) |
| Lua / sol2 | Scripting | [Lua](https://www.lua.org/), [sol2](https://github.com/ThePhD/sol2) |
| DirectXTex / headers | Texture and DX12 helpers | [DirectXTex](https://github.com/microsoft/DirectXTex), [headers](https://github.com/microsoft/DirectX-Headers) |
| DXC | HLSL compilation | [DXC](https://github.com/microsoft/DirectXShaderCompiler) |
| FreeType | Font rasterization | [FreeType](https://freetype.org/) |
| Assimp | Model import | [Assimp](https://github.com/assimp/assimp) |
| meshoptimizer | Geometry processing | [meshoptimizer](https://github.com/zeux/meshoptimizer) |
| Recast / Detour | Navigation | [Recastnavigation](https://github.com/recastnavigation/recastnavigation) |
| FidelityFX | Upscaling | [GPUOpen](https://gpuopen.com/fidelityfx/) |
| NRD / ShaderMake | Denoising/shader data | [NRD](https://github.com/NVIDIA-RTX/NRD) |
| JSON / zstd | Data and decompression | [JSON](https://github.com/nlohmann/json), [zstd](https://github.com/facebook/zstd) |
| Tracy | Profiling | [Tracy](https://github.com/wolfpld/tracy) |

## Fonts

The minimal set contains Noto Sans regular/semibold, required Traditional Chinese/Japanese faces and Font Awesome Solid. The runtime's historical default filename contains Noto Sans Regular. Font license texts accompany those files.

The original project's full font and environment libraries are not redistributed here.

## Optional integrations

Streamline/DLSS, RTXDI/SHaRC, FFmpeg and ONNX Runtime/face-tracking integrations have their own SDK/runtime/content requirements. Their optional payloads are excluded. The engine's own license cannot replace those vendor terms.

## Techniques and demonstrations

The implementation uses documented approaches including deferred/clustered shading, cascaded shadow maps, temporal reconstruction, hybrid screen-space/ray-query effects, irradiance probes and atmospheric scattering. Source/shader notices identify local attribution and adaptations.

README demonstrations use project content excluded from the repository. They are not redistributable sample-asset packages.

## Distribution

CMake installation and CPack include notices and selected library license texts. Preparing this distribution does not assign a new license to the original engine code.
