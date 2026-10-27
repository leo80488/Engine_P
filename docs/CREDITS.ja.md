[English](CREDITS.md) | **日本語**

# クレジットと参考情報

## 第三者ライブラリー

原作者が権利を保持します。詳細な upstream 一覧は [英語版](CREDITS.md)、license/notices は各 directory と [THIRD_PARTY_NOTICES.txt](../THIRD_PARTY_NOTICES.txt) にあります。

| 用途 | 使用ライブラリー |
| --- | --- |
| Physics | Jolt |
| Editor | ImGui、ImGuizmo、ImPlot、node editor、ImGui test engine/club |
| Script/data | Lua、sol2、nlohmann/json、zstd |
| Graphics | DirectX headers、DirectXTex、DXC、FidelityFX、NRD/ShaderMake |
| Import/font | Assimp、FreeType |
| Geometry/navigation | meshoptimizer、Recast/Detour |
| Profiling | Tracy |

## フォント

Noto Sans regular/semibold、必要な TC/JP faces、Font Awesome Solid を最小構成として含みます。旧 default filename の内容は Noto Sans Regular です。対応する OFL／license を添付しています。元 project の全 font/environment library は含みません。

## 任意 SDK と参考技術

Streamline/DLSS、RTXDI/SHaRC、FFmpeg、ONNX Runtime/face tracking の追加 payload は含まれません。使用する場合は各 SDK の条件を確認してください。

Deferred/clustered shading、cascaded shadow、temporal reconstruction、screen-space/ray-query hybrid、irradiance probe、atmosphere などの技術を利用します。個別の attribution/adaptation は source/shader notice に残しています。

README のデモ映像は収録しない game content を使ったもので、再配布 asset package ではありません。

CMake install/CPack は第三者 notices と license を含みます。本作業では engine 独自コードの新しい license を設定していません。
