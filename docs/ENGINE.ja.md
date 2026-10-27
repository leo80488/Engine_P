[English](ENGINE.md) | **日本語**

# エンジンシステム

## ECS とスケジューラー

component pool と compact entity reference を用い、アクセス宣言から並列バッチを構成します。world 終了後は参照や pointer を取り直します。親子 transform の伝播は component storage と分離して管理します。

## リソース

GUID/meta は authored file の identity、runtime handle は読み込み状態と寿命を表します。decode、upload、finalize が終わる前の handle を ready と扱わないでください。

| 形式 | 用途 |
| --- | --- |
| iscene | world scene |
| iscn / meshlib | model hierarchy / geometry |
| ipfb / itex | prefab / texture |
| imat / imatinst / imatgraph | material / instance / graph |
| iskel / ianim | skeleton / animation |
| iui / iseq / ipak | UI / sequence / package |

native 形式は version header と binary payload を持つ場合があります。codec／editor で編集し、meta を保持します。texture/model、PMX/VMD、VRM、Unreal scene などの import 実装はありますが、そのゲーム用データは含まれません。

## Scene と設定

engine に内蔵 defaults、primitive、LUT、scene、font を置き、asset は利用者の content とします。本版の asset は空 manifest と Keys/Pad/InputContexts のみです。

game.json が scene registry、startup、runtime modules を指定します。game-flow state が scene load の時機を管理する場合もあります。starter は player、enemy、quest、title を作成しません。

engine.json は開発／render 設定、settings.json は player options、settings.default.json は初期／test profile、input_profiles.json は入力 profile です。runtime override や command-line pin があるため、保存 JSON だけで有効値を判断しないでください。

## 入力、キャラクター、カメラ

入力は keyboard/mouse、controller、action binding と context を提供します。context の push/pop は対応させます。キャラクターの意図、物理移動、locomotion と animation を分離します。

Edit camera と Play 中の camera は別です。capture 時は view/camera の選択を確認します。

## Animation と physics

GPU skinning、morph、blend/state、notify、IK、socket、chain motion を実装しています。clip の load 成功は skeleton 互換性を保証しません。humanoid retarget は target rig に合わせる authoring/bake の処理です。

Jolt 連携では collision layer、contact/event、scene lifetime、transform 同期を扱います。visible mesh と collision は別のデータです。CMake は Jolt と利用側の feature 定義を一致させます。

## Navigation と AI

Nav は Recast/Detour を使用し、標準でリンクします。ゲーム用 navmesh は含まないので自分の scene 用に用意します。AI は behavior、intent、update/LOD を提供しますが、元の action scripts と demo modules は含みません。

## Lua と audio

Lua/sol2 は component access、callback、service/system を提供します。script_manifest.json の entries は空です。基本入力設定は binding 初期化で別途読み込みます。field/type と参照の world lifetime を確認してください。

Audio は XAudio2/X3DAudio の mixing と spatial playback を統合します。音源は利用者が用意します。Media/Video、FaceTracking は追加 payload と content を要するため標準で無効です。

## Sequencer と UI

sequence は timeline、評価と target resolution を所有します。world 切り替え時は target を再解決します。UI document は authored hierarchy を表し、editor で編集／preview します。

Noto Sans、CJK faces と Font Awesome を使用します。FGMiraiRen.ttf という旧 default filename の内容は本版では Noto Sans Regular で、OFL を添付しています。

## Editor

Outliner、Inspector、Content Browser、viewport、Undo/Redo と Edit/Play を備えます。material/instance/graph、Texture Viewer、model/mesh/skeleton、animation set、retarget、UI、font、VFX、sequencer、graphics settings、profiler、render graph などの tool があります。

非同期 job の受付と完了を区別し、依存する save/inspect の前に待ってください。Play の停止では編集 world を復元します。

## 診断と拡張

bounded run は ledger、frame evidence、log と任意 capture/state を残します。local control channel は opt-in で engine binding を実行するため sandbox ではありません。status 応答だけで frame 進行を判断しないでください。

拡張時は ownership、serialization/reflection、CPU/GPU layout と寿命を揃えます。starter startup test が独自のゲーム全体を検証するわけではありません。
