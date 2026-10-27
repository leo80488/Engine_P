[English](README.md) | [日本語](README.ja.md) | **繁體中文**

# DX12Engine

使用 C++20／Direct3D 12 開發的即時渲染引擎與編輯器，透過原生 CMake 建置，HLSL 使用 DXC 編譯為 Shader Model 6.6／DXIL。

這份專案提供最新版引擎程式與最小啟動內容，不包含原遊戲的角色、動畫庫、商用場景及其他遊戲素材。

## 建置與啟動

需求：Windows x64、實體 DX12 GPU、Visual Studio 2022 C++ 桌面開發工具、Windows SDK、CMake 3.24+、Python 3.10+。

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64-release
ctest --preset vs-x64-release
Set-Location DX12
& .\build\ci\cmake\vs-x64\bin\Release\Editor.exe
```

CMake 直接編譯 EngineCore、EditorCore、模組與 Jolt，不依靠呼叫原本 solution 來完成建置。預設開啟 Nav，其他需要額外 SDK 或遊戲內容的模組預設關閉。

## 文件

文件沿用原公開版本的英文／日文編排，依目前程式重寫：

| 文件 | 內容 |
| --- | --- |
| [BUILD](docs/BUILD.md) | CMake、環境需求、相依套件、安裝、封裝與排錯 |
| [ARCHITECTURE](docs/ARCHITECTURE.md) | 建置單位、系統分層與每幀流程 |
| [RENDERING](docs/RENDERING.md) | Renderer、Render Graph、材質、光照與效果 |
| [ENGINE](docs/ENGINE.md) | ECS、資源、動畫、物理、腳本、UI 與編輯器 |
| [CREDITS](docs/CREDITS.md) | 第三方套件、字型與參考來源 |
| [STATUS](docs/STATUS.md) | 配布範圍、已驗證內容與限制 |

只保留給專案使用者閱讀的文件，不帶入原專案的設計提案、歷史計畫、開發待辦或內部驗證報告。

第三方套件保留原授權；引擎本身的開源授權仍由作者決定。詳見 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。
