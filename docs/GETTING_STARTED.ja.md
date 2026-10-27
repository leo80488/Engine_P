[English](GETTING_STARTED.md) | **日本語**

# Getting Started

最初の source build、scene authoring、独立した Game run の手順です。dependency/option は [Build](BUILD.ja.md)、検証範囲は [Status](STATUS.ja.md) を確認してください。

## 1. Build して editor を開く

repository root から実行します。

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
```

有効な Python interpreter と x64 VS C++ toolchain を使用します。CMake が見つけられない場合は content を変更する前に Build の Troubleshooting を確認します。

## 2. Starter を確認する

標準の BaseScene は camera、directional light、cube を持ちます。F11 で full viewport/docking editor を切り替えます。Outliner で cube を選び、Inspector で Transform/material、Content Browser で内蔵 resource を確認します。

character、title screen、元の game scene は収録していません。starter の成功は core path の確認であり、過去の demo video の全 feature を確認するものではありません。

## 3. Scene を作成する

editor の Create / 3D Object tool で基本形状を作るか、使用権を持つ resource を project asset root に import します。material、camera、light を設定し、新しい iscene として asset/ に保存します。

authored resource と meta を一緒に管理し、依存 texture/material も維持します。native file は versioned container なので、binary を直接編集せず editor/codec を使用してください。optional preview environment や game asset は自動提供されません。

## 4. Scene を登録する

既存 DX12/game.json の該当 field に追記し、name、version、module、package 設定を保持します。以下は asset/MyScene.iscene を保存済みの場合の例です。

```json
{
  "scenes": {
    "BaseScene": { "path": "engine/world/BaseScene.iscene" },
    "MyScene": { "path": "asset/MyScene.iscene" }
  },
  "startup_scene": "asset/MyScene.iscene"
}
```

registry name と file path は異なります。-scene=MyScene は登録名を使用し、startup_scene は通常の boot scene を選びます。この部分的な例で project manifest 全体を上書きしないでください。

## 5. Game host で確認する

DX12/ から実行します。

```powershell
& .\build\ci\cmake\vs-x64\bin\Release\Game.exe -scene=MyScene -settings=settings.default.json
```

Editor の成功だけでは Game の動作は保証できません。runtime host の camera selection、resource readiness、script lifecycle を確認します。optional code を有効にする前に、CMake の link 選択と runtime manifest を合わせます。

## 6. 検査して配布する

repository root に戻って収録 startup test を実行します。

```powershell
Set-Location ..
ctest --preset vs-x64-release
```

収録 test は意図的に BaseScene を使用し、MyScene を自動検証しません。独自 scene には別の evidence を用意し、該当 run.json、frames.jsonl、log、screenshot を確認します。

完全な loose-file distribution は [Build](BUILD.ja.md) の install/package 手順で作成します。shader、defaults、content、runtime DLL を同じ場所で解決できるよう installed root から起動します。

## 次に読む文書

| 目的 | Guide |
| --- | --- |
| Compiler/SDK/module option の変更 | [Build](BUILD.ja.md) |
| Ownership と frame handoff | [Architecture](ARCHITECTURE.ja.md) |
| Material、lighting、pass の変更 | [Rendering](RENDERING.ja.md) |
| ECS、resource、animation、tool の変更 | [Engine Systems](ENGINE.ja.md) |
| Integration と制約の確認 | [Status](STATUS.ja.md) |
