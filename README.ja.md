[English](README.md) | **日本語** | [繁體中文](README.zh-TW.md)

# DX12Engine

C++20 と Direct3D 12 によるリアルタイムエンジン／エディターです。ネイティブ CMake でビルドし、DXC で Shader Model 6.6 の HLSL を DXIL にコンパイルします。

現在のエンジン実装と最小起動シーンを収録しています。キャラクター、アニメーション集、商用環境アセット、元のゲームプロジェクトは含みません。

## デモ

元のゲーム用コンテンツによる過去のデモです。映像のアセットは収録していません。

- [レンダリング](https://youtu.be/6CPXLe-J1sw)
- [エディター](https://youtu.be/mi7Asi6O87Y)
- [シーン](https://youtu.be/0zyT00WBlOE)
- [キャラクター](https://youtu.be/juu7YkzafQs)

## 機能

- Bindless、GPU カリング、間接描画、Deferred G-buffer と clustered lighting。
- 各種シャドウ、DXR ray query、ハイブリッド反射、DDGI、reflection probes。
- NPR、アウトライン、材質グラフ、地形、草、水面と GPU エフェクト。
- 大気、雲、ボリューム効果、TAA／FXAA／FidelityFX、HDR とポスト処理。
- ECS、アクセス宣言付きスケジューラー、シーングラフ、非同期リソース読み込み。
- GPU スキニング、モーフ、IK、リターゲット、Jolt、ナビゲーションと AI。
- Lua、オーディオ、UI ドキュメント、シーケンサー、入力とカメラ。
- Docking エディター、Undo/Redo、Edit/Play、Inspector とアセット編集ツール。
- 終了条件付きテストと任意のローカル制御チャネル。

標準では Nav を有効にし、追加 SDK が必要なモジュールを無効にしています。ソースがある機能と、すぐ使用できる機能の違いは[状態と制約](docs/STATUS.ja.md)を参照してください。

## ビルド

Windows x64、実機 DX12 GPU、Visual Studio 2022 の C++ デスクトップ開発、Windows SDK、CMake 3.24+、Python 3.10+ が必要です。

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
ctest --preset vs-x64-release
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
```

CMake が EngineCore、EditorCore、モジュール、Jolt を直接ビルドします。元の solution を呼ぶラッパーではありません。

## ドキュメント

| 文書 | 内容 |
| --- | --- |
| [アーキテクチャー](docs/ARCHITECTURE.ja.md) | レイヤー、所有関係、フレーム実行 |
| [レンダリング](docs/RENDERING.ja.md) | パイプライン、材質、照明、エフェクト |
| [エンジンシステム](docs/ENGINE.ja.md) | ECS、資源、アニメーション、物理、Lua、エディター |
| [ビルド](docs/BUILD.ja.md) | CMake、依存関係、テストとインストール |
| [クレジット](docs/CREDITS.ja.md) | ライブラリーとフォント |
| [状態と制約](docs/STATUS.ja.md) | 配布範囲と検証範囲 |

利用者向けに書き直した文書です。内部設計計画、開発バックログ、移行レポートは収録していません。

`DX12/src` と `include` が実装、`shaders` が内蔵シェーダー、`engine` が最小資源、`asset` が起動／入力設定、`external` が依存ライブラリーです。出力先の `DX12/build/` は Git に入りません。

第三者ライセンスは [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) を参照してください。この配布作業ではエンジン独自コードのライセンスを新しく設定していません。
