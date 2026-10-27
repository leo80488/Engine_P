[English](STATUS.md) | **日本語**

# 状態と制約

## 配布内容

現行エンジン／エディター、core dependencies、camera/light/cube の最小 scene を含みます。project script は input maps と空 manifest のみです。キャラクター、animation、商用 scene、音源、gameplay、元 Git history は含みません。

内部設計提案、歴史的実装計画、backlog、export audit report を文書ツリーに入れていません。

## 機能の範囲

| 項目 | 標準構成 |
| --- | --- |
| Renderer/ECS/resource/editor | Source と起動資源を同梱 |
| Jolt | CMake source build |
| Nav | 有効。game navmesh なし |
| Material/animation/UI/sequencer tools | Source のみ。content は各自用意 |
| NRD/FidelityFX | 必須 payload を同梱 |
| Media/Video/FaceTracking/Streamline | 無効。追加 runtime/SDK/model は別途 |
| RTXDI/SHaRC | SDK tree 非同梱 |
| Tracy | Source/headers あり、instrumentation 無効 |

## 制約

Windows x64、MSVC と実機 DX12 が必要です。hidden test も desktop/GPU を使います。WARP や Linux/macOS renderer は提供しません。

ray tracing、mesh shader、HDR、temporal SDK は追加条件を持ちます。sample game や character showcase はありません。authoring preview の一部が参照する autumn IBL は利用者が用意します。

## 検証

CMake は Debug/Release、CTest、install、ZIP を提供します。runtime shader compilation を使う development 配布で、元 project の Profile/Retail shipping 全体ではありません。

Windows x64、MSVC 19.44／Windows SDK 10.0.26100.0 で確認しました。

| 検査 | 結果 |
| --- | --- |
| Native CMake Release | Core、EditorCore、Jolt、Nav、Editor、Game のビルド成功 |
| 元 solution/project なしの再 configure/build | 成功 |
| CTest | Editor/Game 各 90 frames、strict access、engine errors 0 |
| 画像確認 | CMake Game の cube を確認 |
| CMake install | 成功 |
| Installed Game / Editor | installed root から各 60 frames 成功 |
| CPack ZIP | 作成成功 |
| Debug、Ninja、任意 SDK | 今回は未実行 |

標準 module 構成と、この Windows/GPU 環境に対する結果です。

startup test は bounded completion、ledger/frame evidence と counters を確認します。元 project の全 rendering/gameplay corpus の代わりではありません。

既存の constant-buffer whole-CBV warning や、NRD vendor PDB 不在による linker warning が出る場合があります。新しい engine error は許容していません。

第三者 license を維持します。engine 独自コードの license は作者が決める事項です。
