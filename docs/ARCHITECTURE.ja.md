[English](ARCHITECTURE.md) | **日本語**

# Architecture

## Engine の全体像

DX12Engine は、real-time renderer、scene と gameplay の runtime、editor を組み合わせた engine です。Editor と Game は engine core を共有します。Editor は content を作成する tool を追加し、Game はその content を単独で実行します。

| 構成要素 | 主な役割 |
| --- | --- |
| Scene と ECS | world に存在する object を表し、それぞれの data を保持 |
| Gameplay systems | input、移動、animation、physics、navigation、behavior を更新 |
| Resources | mesh、texture、material、scene などの asset を読み込み、管理 |
| Renderer | 現在の world と camera から描画に必要な data を準備 |
| Render Graph と Graphics | rendering の処理を整理し、Direct3D 12 を通じて GPU に送信 |
| Editor | 共通の engine を使って scene、asset、settings を作成・確認 |
| AI Harness | 外部 tool による自動 check と evidence の収集 |

## 各部分の連携

scene は world 内の object を記述します。component は位置、見た目、physics の設定などの data を持ち、system が実行中にその data を更新します。Lua は project 固有の behavior を追加し、resource は object が使う mesh や texture などの content を提供します。

各 frame で、renderer は active camera から見える scene の描画準備を行います。Render Graph が選択された rendering pass と resource の処理を整理し、Graphics が Direct3D 12 の処理を担当します。lighting、material、post processing が最終的な画像を作ります。editor も同じ core system を使って content を preview・編集します。

公開版の starter scene は小さな構成です。engine の起動と基本描画を確認するためのもので、より豊かな gameplay や visual feature には project 側で用意する content と設定が必要です。

## 主な設計方針

- **共通の runtime:** Editor と Game が core system を共有し、作成した content を editor 外でも実行できるようにします。
- **役割の分離:** world simulation、resource management、GPU rendering にそれぞれ明確な役割を持たせます。
- **Data-driven な content:** scene、component、material、settings、Lua script で project の多くの動作を記述します。
- **選択可能な integration:** module で追加機能を選択します。一部には別途 SDK や asset が必要です。
- **実行状態の把握:** log、frame record、harness check により、実際に何が動いたかを確認できます。

以下では ownership と source 構成を説明します。個別の subsystem は [Engine Systems](ENGINE.ja.md)、visual feature は [Rendering](RENDERING.ja.md)、自動 check は [AI Harness](AI_HARNESS.ja.md) を参照してください。

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
