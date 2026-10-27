[English](BUILD.md) | **日本語**

# Build

## Prerequisites

- Windows x64、ログイン済み desktop、engine の Shader Model 6.6 path に対応する実機 DX12 GPU。
- Visual Studio 2022、MSVC v143、Desktop development with C++、Windows SDK。
- CMake 3.24 以上、Python 3.10 以上。
- Ninja は任意。使用時は x64 Visual Studio developer shell で実行します。

hidden run も window と GPU を必要とします。GPU 不要の headless や WARP fallback は提供しません。

## Configure と build

repository root から実行します。

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
cmake --build --preset vs-x64-debug
```

最後の行は Debug が必要な場合に実行します。Python が Store alias を指す場合、インストール済み interpreter を選択します。

```powershell
cmake --preset vs-x64 -DPython3_EXECUTABLE="C:/path/to/python.exe"
```

VS preset は x64 tool を選択し、DX12/build/ci/cmake/vs-x64/ に出力します。その下の bin/Release/ または bin/Debug/ に executable があります。Visual Studio で CMake project、または build directory に生成された solution を開きます。debugger の working directory は DX12/ です。

### Ninja

Ninja を PATH に設定した x64 Native Tools for VS 2022 shell から実行します。

```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
```

Ninja は現在の shell の compiler environment を使用し、Visual Studio architecture を自動選択しません。

### Manual configure

```powershell
cmake -S . -B DX12/build/ci/cmake/custom -G "Visual Studio 17 2022" -A x64 -T host=x64
cmake --build DX12/build/ci/cmake/custom --config Release --parallel 12
```

CMake が engine と Jolt を直接コンパイルします。configure 時の Python は source ownership/module registration、build 時の generator は material layout と build identity を生成します。

## Dependencies

core header、library、DLL を同梱しています。configure は optional SDK を download せず、package manager も起動しません。

| Dependency | 必要な layout / 用途 |
| --- | --- |
| DirectX headers、DirectXTex、DXC | DX12/external/。DXC DLL を app の横に stage |
| Jolt | external/Jolt/。EngineJolt として source からコンパイル |
| FreeType | external 配下の header/library と DX12/freetype.dll |
| Assimp | external/assimp/ の import library/runtime。Editor のみ |
| NRD | Include/、_Bin/config/NRD.lib、_Build/_deps/shadermake-build/config/ShaderMakeBlob.lib |
| FidelityFX | FidelityFX/api/include、upscalers/include、FidelityFX/signedbin の loader import library と DLL |
| Geometry/navigation | meshoptimizer と Recast/Detour の header、および対応する Debug/Release library |
| UI/scripting | ImGui、ImPlot、node editor、Lua、sol2、JSON、zstd、Tracy header |

ENGINE_NRD_SDK_DIR または ENGINE_FFX_SDK_DIR で core root を上書きできます。上表の layout を守ってください。必要な file がない場合、configure はその path を示して停止します。

DX12/ から python tools/fetch_deps.py --verify を実行すると、固定 hash 対象の payload を確認できます。対象 file は Git で追跡しています。古い manifest の download URL は、すべての依存関係を公開ネットワークから復元できる完全な bootstrap service ではありません。

## Options

| Option | Default | 意味 |
| --- | --- | --- |
| ENGINE_BUILD_EDITOR | ON | Editor と EditorCore |
| ENGINE_BUILD_GAME | ON | Game host |
| ENGINE_ENABLE_HARNESS | ON | bounded run と ledger |
| ENGINE_ENABLE_CONTROL_CHANNEL | ON | local channel をコンパイル。runtime でも明示して opt-in |
| ENGINE_ENABLE_TRACY | OFF | Tracy client/instrumentation |
| ENGINE_ENABLE_NAV | ON | Navigation module |
| ENGINE_ENABLE_MEDIA | OFF | FFmpeg header/library/DLL が必要 |
| ENGINE_ENABLE_VIDEO | OFF | FFmpeg payload が必要 |
| ENGINE_ENABLE_FACE_TRACKING | OFF | Media を有効化。ONNX Runtime と content が必要 |
| ENGINE_ENABLE_STREAMLINE | OFF | 別途取得する SDK/runtime が必要 |
| BUILD_TESTING | ON | CTest startup test |

compile option を変更したら rebuild します。game.json だけで有効にしても、link 時に除外した C++ module は利用できません。optional SDK build は標準構成の検証範囲外です。

native configuration は Debug と Release です。upstream project の Profile/Retail cook/shipping matrix 全体を再現するものではありません。

## Run

```powershell
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
& .\build\ci\cmake\vs-x64\bin\Release\Game.exe -scene=BaseScene -settings=settings.default.json
```

starter は camera、directional light、cube を持ちます。F11 で full viewport/docking editor を切り替えます。runtime の生成 state は Git に入りません。

## Test

```powershell
ctest --preset vs-x64-release
```

CTest は実際の CMake executable を起動し、bounded completion、process/ledger exit code、frame evidence、error/access counter、unread flag を確認します。出力は DX12/tests/_artifacts/cmake/ です。

test wrapper は Python standard library のみを使用します。意図した画像かどうかは screenshot/log も確認してください。startup test は全 Renderer mode や optional SDK の動作を証明しません。

## Install と package

```powershell
cmake --install DX12/build/ci/cmake/vs-x64 --config Release --prefix DX12/build/package/standalone
cmake --build --preset vs-x64-release --target package
```

app、必須 runtime DLL、shader、最小 content、defaults、notice をインストールし、asset identity sidecar も保持します。installed app は installed root を working directory にして起動します。

CPack は DX12/build/package/ に ZIP を出力します。runtime shader compilation を使用する loose-file development build であり、元 project の cooked Retail product ではありません。対応する x64 Visual C++ runtime がない machine では別途インストールしてください。

## Troubleshooting

| 症状 | 確認事項 |
| --- | --- |
| Compiler/x64 configure error | VS C++ workload と x64 preset |
| Python が Store を開く | Python3_EXECUTABLE を明示 |
| NRD/FFX file がない | SDK root と正確な directory layout |
| Link failure | Debug/Release の flavor と vendor library の不足 |
| Runtime DLL がない | post-build staging。異なる SDK version を混在させない |
| Shader/resource がない | working directory、または完全な installed tree |
| 初回 build が遅い | Jolt、PCH、全 engine/editor source のコンパイルが必要 |
| 想定外の scene | game.json と -scene。元の game content は除外済み |
| 黒い／過露出の画面 | 現在の light/material と当該 run の evidence |
| run.json がない | 有効な harness result より前に startup が失敗 |
