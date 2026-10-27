# imgui-node-editor (vendored)

The node canvas of the Material Graph (docs/design/TextureViewer_MaterialEditor_Plan.md
Part C, decided in §0.0 Q2: the ORIGINAL library, not a fork).

| | |
|---|---|
| Upstream | https://github.com/thedmd/imgui-node-editor |
| Branch / commit | `develop` @ `b302971455b3719ec9b5fb94b2f92d27c62b9ff0` |
| License | MIT (`LICENSE`) |
| Built by | `EditorCore.vcxproj` (the four `.cpp`, no PCH, like `external/imgui`) |
| Dear ImGui | `external/imgui` 1.92.7 WIP (`IMGUI_VERSION_NUM` 19263) |

Only the library files are here: no `examples/`, `docs/` (its changelog is
`CHANGELOG.upstream.txt`) or `misc/` natvis files.

## Local patches

Every change is marked `[DX12Engine patch N]` in the source. Re-apply them when
updating from upstream (and drop any upstream has made unnecessary).

1. **`ImRect::Floor()` is gone from Dear ImGui** (removed with 1.90's `IM_FLOOR`
   → `IM_TRUNC` rename). `imgui_extra_math.h` gains `ImRect_Floor(ImRect&)` - the
   same per-corner truncation - and the eight `x.Floor();` calls in
   `imgui_node_editor.cpp` call it. This was the only compile error against 1.92.7.
2. **Sharp text when zoomed** (`imgui_canvas.h/.cpp`). The canvas draws in local
   space and scales the vertices by the view in `LeaveLocalSpace`, so glyphs baked
   at the base font size came out magnified - blocky - when zoomed in. Dear ImGui
   1.92 bakes glyphs at a *rasterizer density* with the layout unchanged:
   `EnterLocalSpace` sets it to the view's scale (quantized to quarter steps,
   0.25..4, so a zoom does not bake the font again every frame) and
   `LeaveLocalSpace` restores it. Needs `ImGuiBackendFlags_RendererHasTextures`
   (our DX12 backend has it). `ImGuiEx::CanvasZoomedTextDensity = false` is the
   upstream behaviour (the harness's reverse arm, `-nodeeditor_smoke_density_off`).
   MEASURED at 10.7x on the smoke's zoomed text: edge strength 5.44 vs 2.66,
   5317 vs 17883 mid-tone pixels.

## How the engine uses it (rules the patches do not enforce)

* `ax::NodeEditor::Config::SettingsFile = nullptr` - never write `NodeEditor.json`
  into the working directory; a graph stores its node positions itself.
* `Config::EnableSmoothZoom = false` under the harness (a zoom lands in one frame)
  and no `ed::Flow()` calls there: captures are deterministic.
* The canvas brackets its draw range with a sentinel callback
  (`ImDrawCallback_ImCanvas`, `(ImDrawCallback)-2`) and erases it in
  `LeaveLocalSpace`. A leftover one would be an application callback in the frame's
  draw data, which makes the render owner finish before the next simulation
  (`ImGuiManager`'s owner-callback rule) - the smoke run counts them (0).

## G0 acceptance (TextureViewer_MaterialEditor_Plan.md §4.8)

Pack `tests/scenarios/nodeeditor_smoke` (harness-only window,
`src/Editor/NodeEditorSmoke.cpp`): 500 nodes / 499 links cost p50 1.4 ms,
p99 2.1-2.2 ms of CPU per frame (Release); 0 leftover callbacks; the unchanged
view captured twice is the same pixels; zoomed-in text at density 4.
