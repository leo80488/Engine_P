# imgui_club: imgui_memory_editor (vendored)

The editor's read-only byte views (docs/design/Editor_UI_Architecture_Assessment.md §6.7, X6): a
component's bytes with its reflected fields marked, an asset container's header, a material's GPU
record.

| | |
|---|---|
| Upstream | https://github.com/ocornut/imgui_club |
| Commit | `main` @ `b083116866e05e0d4bccb2c7f28d94a76ee4965e` (`imgui_memory_editor.h` last changed in `019ed61`, 2025-08-04) |
| License | MIT (`LICENSE.txt`) |
| Built by | header-only; included by the one editor file that draws byte views |
| Dear ImGui | `external/imgui` 1.92.7 WIP (`IMGUI_VERSION_NUM` 19263) |

Only `imgui_memory_editor.h` is here.

## Local patches

None.

## How the engine uses it

* Always read-only (`MemoryEditor::ReadOnly = true`): a byte written here would bypass the property
  writer and the undo history.
