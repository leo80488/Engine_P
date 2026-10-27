[English](BUILD.md) | **日本語**

# ビルド

## 必要環境

Windows x64、実機 DX12 GPU とログイン済みデスクトップ、Visual Studio 2022 の MSVC v143／C++ デスクトップ開発、Windows SDK、CMake 3.24+、Python 3.10+ を使用します。Ninja は任意です。

-hidden はウィンドウを隠す指定で、GPU 不要の headless や WARP ではありません。

## Configure と build

リポジトリのルートから実行します。

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
```

Debug は cmake --build --preset vs-x64-debug で選択します。Python が Store alias の場合は、インストール済み interpreter を明示します。

```powershell
cmake --preset vs-x64 -DPython3_EXECUTABLE="C:/path/to/python.exe"
```

出力は DX12/build/ci/cmake/vs-x64/bin/Release または Debug です。Visual Studio で CMake project または生成 solution を開けます。debugger の作業ディレクトリーは DX12/ です。

### Ninja

x64 Native Tools for VS 2022 shell で Ninja を PATH に入れ、実行します。

```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
```

Ninja は現在の compiler 環境を使用します。通常の shell から architecture を自動選択する仕組みではありません。

## 依存関係

標準ビルドの headers、libraries、DLL は同梱します。configure で package manager や任意 SDK の自動ダウンロードは実行しません。

| 套件 | 配置 |
| --- | --- |
| DXC / DirectXTex / headers | DX12/external 以下 |
| Jolt | external/Jolt のソースを直接ビルド |
| FreeType | external の headers/lib と DX12/freetype.dll |
| Assimp | external/assimp。Editor のみ |
| NRD | Include、_Bin/config、_Build/_deps/shadermake-build/config |
| FidelityFX | FidelityFX/api/include、upscalers/include、signedbin |
| Navigation / geometry | Recast/Detour と meshoptimizer |
| UI / scripting | ImGui、ImPlot、node editor、Lua、sol2、JSON、zstd、Tracy |

ENGINE_NRD_SDK_DIR と ENGINE_FFX_SDK_DIR で core SDK の場所を変更できます。必要ファイルがない場合は configure がパスを示して終了します。

DX12/ から python tools/fetch_deps.py --verify で固定ハッシュ対象を確認できます。この manifest の download URL は全依存関係を公開ネットワークから復元する完全なサービスではありません。

## オプション

| オプション | 標準 |
| --- | --- |
| ENGINE_BUILD_EDITOR / ENGINE_BUILD_GAME | ON |
| ENGINE_ENABLE_HARNESS | ON |
| ENGINE_ENABLE_CONTROL_CHANNEL | ON。実行時にも明示して有効化 |
| ENGINE_ENABLE_TRACY | OFF |
| ENGINE_ENABLE_NAV | ON |
| ENGINE_ENABLE_MEDIA / ENGINE_ENABLE_VIDEO | OFF。FFmpeg が必要 |
| ENGINE_ENABLE_FACE_TRACKING | OFF。Media、ONNX Runtime、モデルが必要 |
| ENGINE_ENABLE_STREAMLINE | OFF。別途取得した SDK/runtime が必要 |
| BUILD_TESTING | ON |

設定変更後は再ビルドします。game.json だけで未リンクの C++ module を追加することはできません。native CMake は Debug/Release を提供し、元 project の Profile/Retail shipping matrix 全体は再現していません。

## 起動とテスト

```powershell
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
```

camera、directional light、cube の最小 scene が起動します。F11 で docking/full viewport を切り替えます。リポジトリルートでテストします。

```powershell
ctest --preset vs-x64-release
```

実際の CMake executable を bounded run し、exit、run.json、frame evidence、error/access counters、unread flags を検査します。証拠は DX12/tests/_artifacts/cmake に保存します。全 renderer 機能の検証ではありません。

## インストールと ZIP

```powershell
cmake --install DX12/build/ci/cmake/vs-x64 --config Release --prefix DX12/build/package/standalone
cmake --build --preset vs-x64-release --target package
```

app、DLL、shader、最小資源、設定と notices を含みます。meta も維持します。installed root を作業ディレクトリーにして実行してください。

ZIP は DX12/build/package に生成します。runtime shader compilation を使う development 配布で、元の cooked Retail 製品ではありません。必要な x64 Visual C++ runtime は別途インストールします。

## 問題の確認

compiler/x64 エラーは VS workload と preset、SDK missing は root/layout、link エラーは Debug/Release と payload を確認します。missing shader/resource は作業ディレクトリーと install tree を確認してください。run.json がない実行は成功と扱いません。
