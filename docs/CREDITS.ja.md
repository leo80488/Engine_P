[English](CREDITS.md) | **日本語**

# Credits & References

## Libraries

各 component は原作者の所有権を維持します。license text/notice は該当 payload と [THIRD_PARTY_NOTICES.txt](../THIRD_PARTY_NOTICES.txt) にあります。

| Component | 用途 | Upstream |
| --- | --- | --- |
| Jolt | Physics/character simulation | [JoltPhysics](https://github.com/jrouwe/JoltPhysics) |
| ImGui / ImGuizmo | Editor UI と manipulation | [ImGui](https://github.com/ocornut/imgui), [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) |
| ImPlot / node editor | plot と graph authoring | [ImPlot](https://github.com/epezent/implot), [node editor](https://github.com/thedmd/imgui-node-editor) |
| ImGui test engine / club | Editor test と utility | [test engine](https://github.com/ocornut/imgui_test_engine), [imgui_club](https://github.com/ocornut/imgui_club) |
| Lua / sol2 | Scripting | [Lua](https://www.lua.org/), [sol2](https://github.com/ThePhD/sol2) |
| DirectXTex / headers | texture 処理と DX12 helper | [DirectXTex](https://github.com/microsoft/DirectXTex), [headers](https://github.com/microsoft/DirectX-Headers) |
| DXC | HLSL compilation | [DXC](https://github.com/microsoft/DirectXShaderCompiler) |
| FreeType | Font rasterization | [FreeType](https://freetype.org/) |
| Assimp | Model import | [Assimp](https://github.com/assimp/assimp) |
| meshoptimizer | Geometry processing | [meshoptimizer](https://github.com/zeux/meshoptimizer) |
| Recast / Detour | Navigation | [Recastnavigation](https://github.com/recastnavigation/recastnavigation) |
| FidelityFX | Upscaling | [GPUOpen](https://gpuopen.com/fidelityfx/) |
| NRD / ShaderMake | Denoising/shader data | [NRD](https://github.com/NVIDIA-RTX/NRD) |
| JSON / zstd | data 処理と decompression | [JSON](https://github.com/nlohmann/json), [zstd](https://github.com/facebook/zstd) |
| Tracy | Profiling | [Tracy](https://github.com/wolfpld/tracy) |

## Fonts

最小構成として Noto Sans regular/semibold、必要な Traditional Chinese/Japanese face、Font Awesome Solid を含みます。runtime の旧 default filename の内容は Noto Sans Regular です。各 font に license text を添付しています。

元 project の font collection 全体と environment library は、本版では再配布していません。

## Optional integrations

Streamline/DLSS、RTXDI/SHaRC、FFmpeg、ONNX Runtime/face-tracking integration は独自の SDK/runtime/content を必要とします。optional payload は除外しています。engine 自身の license が vendor の条件を置き換えることはありません。

## Techniques と demonstrations

実装には deferred/clustered shading、cascaded shadow map、temporal reconstruction、screen-space/ray-query hybrid effect、irradiance probe、atmospheric scattering などの手法を使用します。個別の attribution/adaptation は source/shader notice に記載しています。

README の demonstration は repository に含まれない project content を使用しており、再配布可能な sample-asset package ではありません。

## Distribution

CMake install と CPack は notice と選択した library license text を含みます。この source distribution の整理によって engine 独自コードに新しい license を設定していません。
