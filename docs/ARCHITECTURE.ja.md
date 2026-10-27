[English](ARCHITECTURE.md) | **日本語**

# Architecture

## Targets と所有関係

EngineOptions と EngineDependencies は INTERFACE target であり、追加の runtime binary ではありません。標準の選択 module は Nav です。Editor が editor-only UI と import code を所有し、Game は EditorCore を link しません。

EngineCore は WITH_EDITOR なしで一度コンパイルします。host ごとの差分には host policy または editor hook を使用します。EditorCore は WITH_EDITOR と editor 用 ImGui 定義でコンパイルします。

main.cpp、App.cpp、App_Render.cpp と生成した module registration unit は、Editor と Game で別々にコンパイルします。App の宣言と module registration が host ごとに異なるため、これらを共有 library にまとめると layout の契約を壊します。各 target は独自の PCH を生成します。

Jolt は同梱 source からビルドします。Jolt header を利用するすべての translation unit と feature 定義を一致させる必要があり、CMake がその定義を伝播します。

## Source selection

configure 時に DX12/tools/cmake_configure.py を実行し、source ownership rule と module descriptor から Sources.cmake および registration code を build tree へ生成します。元の project file を parse してコンパイルしたり、MSBuild で元の solution を呼び出したりする処理ではありません。

src/ 以下に追加した cpp は次回 configure で検出します。module の所有範囲は src/Modules/*.module.json に記述します。src/Editor/ 外の editor helper にも明示的な ownership rule を適用します。

## Frame execution

App は startup、shutdown、外側の loop を所有します。FramePipeline が editor hook を含む frame phase を調整します。ECS system が scene data を更新した後、Renderer が GPU 向けの draw、material、light、visibility data を準備します。

Render Graph は pass と resource dependency を表し、Graphics が DX12 object、command recording と submission を扱います。CPU preparation、graph scheduling、GPU submission は別の責任です。

### Runtime layers

Scene/gameplay state は world-facing 側、pass/resource scheduling は Render Graph、device object と command submission は Graphics に属します。Editor hook により、EngineCore に WITH_EDITOR を持ち込まず authoring 処理を接続します。

### Frame ownership と synchronization

render-owner path は game 側の orchestration と render execution を分離します。job と preparation は active frame plan に従って overlap できます。drain は必要な ownership boundary を待ちますが、queue に投入済みの全 GPU work の完了を意味しません。slot の再利用は別途 GPU completion で保護します。

[FramePipeline](../DX12/include/Renderer/FramePipeline.h)、[executor](../DX12/src/Renderer/FramePipeline.cpp)、[App render phases](../DX12/src/App_Render.cpp)を参照してください。world mutation、render snapshot、slot ownership を handoff の適切な側に置きます。

## ECS と lifetime

component pool が storage を提供し、system がアクセスを宣言することで合法な並列 batch を組み立てます。transform hierarchy の伝播と world teardown も契約の一部です。integer index が再利用されていても、cached handle が scene transition を越えて有効になるわけではありません。

## Resource boundaries

| Root | 所有範囲 |
| --- | --- |
| DX12/engine/ | defaults、primitive、LUT、editor font |
| DX12/asset/ | project content。本版では boot/input configuration のみ |
| DX12/shaders/ | built-in Renderer implementation |
| DX12/external/ | third-party source と必須 prebuilt payload |
| DX12/build/ | generated file、compiler output、package |

runtime handle は loading/lifetime、GUID はファイル移動を越える authored identity を扱います。meta sidecar を保持し、native binary format は engine codec で編集します。

## Configuration

game.json は identity、scene、startup、runtime module を宣言します。engine.json は developer/render configuration、settings.json は player option、settings.default.json は clean installation/test template です。Editor preference と layout は local state として扱います。

CMake は link 対象を選択し、project manifest は runtime で有効にする機能を選択します。optional integration の追加時には両方を合わせます。

## Extension points

- engine 機能は EngineCore、editor window/importer は EditorCore、optional integration は module を通して追加します。
- Render Graph access を宣言し、CPU/HLSL layout の一致を維持します。
- ECS access declaration と world lifetime の処理を維持します。
- project header を PCH に入れないでください。external/C source は engine の C++ PCH を使用しません。

[Engine Systems](ENGINE.ja.md)、[Rendering](RENDERING.ja.md)、[Build](BUILD.ja.md)も参照してください。

## Source map

| 領域 | 読み始める場所 |
| --- | --- |
| Application / boot | [main.cpp](../DX12/src/main.cpp)、[App.cpp](../DX12/src/App.cpp) |
| Frame execution | [FramePipeline.h](../DX12/include/Renderer/FramePipeline.h) |
| ECS / scheduling | [ECS](../DX12/include/ECS/ECS.h)、[Scheduler](../DX12/include/ECS/Scheduler.h) |
| Resource loading | [ResourceManager](../DX12/include/Resource/ResourceManager.h)、[AssetFS](../DX12/include/Resource/AssetFS.h) |
| Rendering graph | [RenderGraph](../DX12/include/RenderGraph/RenderGraph.h) |
| Native CMake targets | [DX12/CMakeLists.txt](../DX12/CMakeLists.txt) |

## Feature を追加する場所

| 変更 | 主な接続箇所 |
| --- | --- |
| Component/system | storage、serialization/reflection、scheduler registration、access declaration |
| Render feature | frame-plan selection、resource declaration、shader binding、lifetime |
| Asset format | loader/importer、versioned codec、identity、editor integration |
| Editor tool | EditorCore ownership、undo/redo、world/preview lifetime、host hook |
| Optional module | descriptor ownership、CMake selection、SDK check、runtime manifest |

最小限の関連 extension point から始め、共有動作を変えた場合は両 host で確認します。機能ごとに新しい manager を作るのではなく、既存の ownership boundary を利用します。
