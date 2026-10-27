[English](ARCHITECTURE.md) | **日本語**

# アーキテクチャー

## ターゲットと所有関係

| ターゲット | 責任 |
| --- | --- |
| EngineCore | Renderer、ECS、資源、Lua と共通システム |
| EditorCore | エディター、インポーター、ImGui |
| EngineJolt | 同梱ソースからビルドする物理ライブラリー |
| EngineModule_* | 選択した追加モジュール |
| Editor / Game | 各ホストのアプリケーション |

EngineCore は WITH_EDITOR なしで一度ビルドします。ホスト差分にはポリシーや editor hook を使います。EditorCore は WITH_EDITOR とエディター用 ImGui 定義を使用します。

main.cpp、App.cpp、App_Render.cpp、モジュール登録コードは各実行ファイルで別々にコンパイルします。App の条件付きメンバーと登録処理が異なるため、共有ライブラリーにまとめると型／所有関係の契約を壊します。PCH も各ターゲットで生成します。

## ソース選択

configure 時に tools/cmake_configure.py がソース所有規則と src/Modules/*.module.json を読み、Sources.cmake と登録コードをビルドツリーへ生成します。元の project を MSBuild で呼ぶ処理ではありません。

src 以下の cpp 追加は次回 configure で検出します。Editor フォルダー外にあるインポーターなどは明示的なルールで分類します。Jolt の feature 定義は利用側と一致させます。

## フレーム

App が起動、終了、外側のループを所有します。FramePipeline が ECS と editor hook を含む各段階を調整し、Renderer が描画、光源、材質、可視性データを準備します。

Render Graph が pass と資源依存を表し、Graphics が DX12 オブジェクト、コマンド記録、送信を担当します。CPU 準備、graph 排程、GPU 実行を区別してください。

## ECS と寿命

component pool とアクセス宣言により安全な並列バッチを構成します。親子 transform の伝播や world 終了も契約の一部です。同じ整数 ID が存在しても、シーン切り替え前の参照を再利用できるとは限りません。

## データ境界

| ルート | 内容 |
| --- | --- |
| engine | デフォルト資源、基本形状、LUT とフォント |
| asset | プロジェクト資源。本版は起動／入力設定のみ |
| shaders | Renderer 内蔵実装 |
| external | 第三者ソースと必須 payload |
| build | 生成ファイル、ビルド成果物、package |

runtime handle は読み込み状態と寿命、GUID はファイルの同一性を扱います。移動時は meta を保持し、native binary はエディター／codec で編集します。

## 設定と拡張

game.json が identity、scene、起動、runtime module を指定します。engine.json は開発設定、settings.json は player options、settings.default.json は初期テンプレートです。CMake のリンク選択と runtime manifest の有効設定は両方合わせます。

Render Graph のアクセス、ECS の宣言、CPU/HLSL layout、world 寿命を維持して拡張してください。project header を共通 PCH に入れないでください。

詳細は [Engine](ENGINE.ja.md)、[Rendering](RENDERING.ja.md)、[Build](BUILD.ja.md) を参照してください。
