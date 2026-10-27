[English](README.md) | **日本語**

# DX12Engine

native CMake build に対応する C++20 / Direct3D 12 engine と editor です。Shader Model 6.6 を対象とし、DXC で HLSL を DXIL にコンパイルします。

現在の engine implementation と小さな built-in startup scene を提供します。game character、animation library、commercial environment、元の game project は含みません。

## Demo

本 repository に含まれない project content を使用した過去の demonstration です。

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

- Deferred G-buffer、bindless resource、GPU culling、indirect draw submission。
- Clustered lighting、cascaded directional shadow、local-light/per-object shadow path。
- Hardware ray query、RT shadow/AO、hybrid reflection、DDGI、reflection probe。
- ReSTIR implementation、NPR character rendering、outline、decal、custom material shader。
- Atmosphere、cloud、volumetric effect、terrain、grass、water、interactive ripple。
- TAA、FXAA、FidelityFX upscaling、HDR、post-processing stack。
- Streamline は optional integration。SDK/runtime は別途取得します。

**Engine**

- Access declaration、dependency-aware scheduling、scene hierarchy を備える ECS。
- Native resource format、GUID identity、async loading、hot reload、pak support。
- GPU skinning、morph、IK、animation system、humanoid retargeting、chain physics。
- Jolt physics、character control、navigation、AI、input、camera、audio、Lua/sol2。
- UI document、material graph、Sequencer timeline、layered settings。
- Frame evidence を残す bounded diagnostic run と opt-in local control channel。

**Editor**

- Docking viewport、Outliner、Inspector、Content Browser、undo/redo、Edit/Play。
- Material/texture tool、animation/skeleton authoring、UI editing、preview、Render Graph inspection。
- Project configuration と明示的な optional engine module。

これらは実装領域の説明です。標準で Nav を有効にし、optional SDK module は無効にしています。提供範囲と検証済み範囲は [Status](docs/STATUS.ja.md) を確認してください。

## Build

Visual Studio 2022 の C++ desktop workload、Windows SDK、CMake 3.24+、Python 3.10+ を用意します。

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
ctest --preset vs-x64-release
```

repository root から実行します。

```powershell
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
```

CMake は EngineCore、EditorCore、選択した module library、Jolt を source から直接コンパイルし、元の solution を呼ぶ wrapper ではありません。core dependency は同梱し、optional SDK は自動 download しません。

完全な手順、option、install/package は [Build](docs/BUILD.ja.md) にあります。

## Documentation

ownership は Architecture、実装詳細は Rendering / Engine Systems を参照してください。

| Document | 内容 |
| --- | --- |
| [Architecture](docs/ARCHITECTURE.ja.md) | target、ownership、frame execution、resource boundary |
| [Rendering](docs/RENDERING.ja.md) | pipeline、lighting、material、effect、pass source index |
| [Engine Systems](docs/ENGINE.ja.md) | ECS、resource、animation、physics、scripting、UI、editor |
| [AI Harness](docs/AI_HARNESS.ja.md) | bounded run、Lua scenario、evidence、障害調査、local control |
| [Build](docs/BUILD.ja.md) | CMake、dependency、test、install、troubleshooting |
| [Credits & References](docs/CREDITS.ja.md) | library、font、implementation reference |
| [Status & Limitations](docs/STATUS.ja.md) | distribution と verification の範囲 |

利用者向け文書です。内部 design plan、development backlog、migration report は含みません。

## Repository layout

| Path | 内容 |
| --- | --- |
| CMakeLists.txt、CMakePresets.json | 公開 build entry point |
| docs/ | 完全な English/Japanese user documentation |
| DX12/src/、DX12/include/ | engine/editor implementation |
| DX12/shaders/ | built-in Renderer shader |
| DX12/engine/ | 最小 built-in resource と editor font |
| DX12/asset/ | 空 boot manifest と core input configuration |
| DX12/external/ | 同梱 third-party dependency |
| DX12/cmake/、DX12/tools/ | dependency、generator、utility |
| DX12/build/、DX12/log/、DX12/tests/_artifacts/ | Git で無視する local output |

## Licenses

third-party component は各自の license を維持します。[Credits](docs/CREDITS.ja.md)と [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) を確認してください。この distribution では engine 独自コードに新しい license を設定していません。
