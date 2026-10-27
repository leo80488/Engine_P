# Dear ImGui Test Engine (vendored)

Real widget interaction for the editor's UI tests (docs/design/Editor_UI_Architecture_Assessment.md
§6.4, X3): focus a window, press a chord, click or drag an item by its path, then check the editor's
state. The rest of the harness (Lua / control pipe packs) keeps testing engine state and pictures.

| | |
|---|---|
| Upstream | https://github.com/ocornut/imgui_test_engine |
| Commit | `main` @ `c043664d845b02b7e92c8f2f144219b9c9f03ff7` (2026-02-19: the last one before the local ImGui) |
| License | Dear ImGui Test Engine License v1.04 (`LICENSE.txt`) - see below |
| Built by | `EditorCore.vcxproj` (the library `.cpp`; no PCH, like `external/imgui`) |
| Dear ImGui | `external/imgui` 1.92.7 WIP (`IMGUI_VERSION_NUM` 19263, upstream docking `934c6a5f`, 2026-02-25) |

Only the library is here (upstream's `imgui_test_engine/` folder, with `thirdparty/Str/Str.h` and
`thirdparty/stb/imstb_image_write.h`): no `imgui_test_suite/`, `app_minimal/`, `shared/`, projects
or `.natvis` files.

## License

The license is free for individuals (natural persons), non-profits, education, open-source derivative
software, and legal entities whose turnover in the last fiscal year is under USD 2 million; anyone
else has a 45-day trial and then needs a paid license. Check which case applies to this project
before shipping a build that contains it (Editor.exe only - Game.exe and Retail never link it).

## Local patches

None. Every change to these files is marked `[DX12Engine patch N]` and listed here.

## How the engine uses it

* `external/imgui/imconfig.h` defines `IMGUI_ENABLE_TEST_ENGINE` (the hooks in imgui.cpp, compiled
  into EditorCore only) and `IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL` (upstream's
  std::thread coroutines).
* The engine starts only when a run asks for UI tests; otherwise the hooks see no engine and cost a
  pointer test per item.
* Tests live in `src/Editor/UITests/`, registered by area; they find windows by their `###ID`.
* Updating Dear ImGui: move this to the test engine commit that matches it (upstream tags pair with
  ImGui releases), then run the UI test packs.
