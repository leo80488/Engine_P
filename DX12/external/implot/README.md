# ImPlot (vendored)

The Profiler panel's plots (docs/design/Editor_UI_Architecture_Assessment.md §6.2, X1): frame time
with its budget line and percentiles, the concurrency histogram, VRAM and GPU pass history.

| | |
|---|---|
| Upstream | https://github.com/epezent/implot |
| Tag / commit | `v1.0` @ `524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39` |
| License | MIT (`LICENSE`) |
| Built by | `EditorCore.vcxproj` (`implot.cpp`, `implot_items.cpp`; no PCH, like `external/imgui`) |
| Dear ImGui | `external/imgui` 1.92.7 WIP (`IMGUI_VERSION_NUM` 19263, upstream docking `934c6a5f`) |

Only the library files are here: no `implot_demo.cpp`, `example/` or docs.

## Local patches

None. Every change to these files is marked `[DX12Engine patch N]` and listed here, so an update
from upstream can re-apply it.

## How the engine uses it

* Editor-only: ImGui and ImPlot compile into EditorCore, never into EngineCore or Game.exe.
* `ImGuiManager` owns the context: `ImPlot::CreateContext()` right after `ImGui::CreateContext()`,
  `ImPlot::DestroyContext()` right before `ImGui::DestroyContext()`.
* Plots read the panel's model (rings filled once per frame, outside the draw); a plot never
  samples the profiler itself.
* Updating Dear ImGui: rebuild and run the Profiler's checks (`verify.py --files
  external/implot/implot.cpp`) - ImPlot's own `IMGUI_VERSION_NUM` guards cover older ImGui only.
