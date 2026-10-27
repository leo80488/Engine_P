[English](STATUS.md) | **日本語**

# Status & Limitations

## Distribution

現在の engine/editor source、core dependency、最小 startup content を含みます。最初の scene は camera、directional light、cube です。project script は input map と空 boot manifest に限定しています。

game character、animation、commercial scene、sound library、gameplay script、private project history は除外しています。内部設計計画、歴史的実装提案、backlog、export audit report は documentation tree に含みません。

## Availability

| 領域 | 状態 |
| --- | --- |
| Renderer、ECS、resources、editor | source と必要な startup resource を同梱 |
| Jolt | native CMake source build |
| Nav | 標準で link。game navmesh はなし |
| Material/animation/UI/sequencer tools | source を同梱。content は利用者が用意 |
| FidelityFX / NRD core | 必須 payload を同梱 |
| Media / Video | OFF。FFmpeg runtime payload なし |
| FaceTracking | OFF。ONNX Runtime、face model/mapping なし |
| Streamline / DLSS | OFF。SDK/runtime は別途取得 |
| RTXDI / SHaRC | SDK tree を除外。依存 backend は標準で無効 |
| Tracy | source/header あり。instrumentation は OFF |

## Platform と content の制約

Windows x64、MSVC、実機 DX12 を必要とします。hidden test も desktop/GPU を使用し、WARP や Linux/macOS Renderer は提供しません。

高度な経路には追加の GPU/driver 条件があります。starter の起動成功は、すべての device で ray tracing、mesh shader、HDR、temporal SDK が動作することを保証しません。

sample game や character showcase はありません。一部の authoring preview は除外した autumn IBL を参照するので、自分の environment を用意してください。default material に残る歴史的な ramp filename は必要な material dependency であり、character asset ではありません。

## Build と verification の範囲

CMake は Debug/Release、feature option、CTest startup、install、ZIP package を提供します。package は runtime shader compilation を使う loose-file development distribution で、元 project の Profile/Retail cook/shipping pipeline 全体ではありません。

Windows x64、MSVC 19.44／Windows SDK 10.0.26100.0 で確認しました。

| 検査 | 結果 |
| --- | --- |
| Native CMake Release build | EngineCore、EditorCore、Jolt、Nav、Editor、Game の build 成功 |
| 元 solution/project file なしの reconfigure/rebuild | 成功 |
| CTest | Editor/Game 各 90 frames、strict access check、engine errors 0 |
| 画像確認 | CMake Game capture で starter cube を確認 |
| CMake installation | 成功 |
| Installed Game / Editor | installed root から独立した各 60-frame run が成功 |
| CPack ZIP | 作成成功 |
| Debug、Ninja、optional SDK configuration | 必要な設定を提供。今回の検証では未実行 |

結果はこの Windows/GPU 環境と default module set に対するものです。

startup test は実際の CMake executable、bounded completion、ledger/frame evidence、counter を確認します。元 project の gameplay/rendering regression corpus 全体を置き換えるものではありません。

## Known diagnostics

既存の constant-buffer whole-CBV warning が startup 時に出る場合があります。core prebuilt NRD library は vendor PDB 不在の linker warning を出す場合があり、該当 library object の debug symbol 利用に影響します。test は新しい engine error を許容しません。

## Licensing

third-party component は各自の license を保持します。engine 独自コードの license は作者が決める事項です。[Credits](CREDITS.ja.md)を参照してください。
