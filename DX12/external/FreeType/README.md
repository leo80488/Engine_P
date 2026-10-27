# FreeType

What the engine bakes its glyph SDFs with (`src/UI/Font.cpp`, `src/UI/FontLibrary.cpp`; ImGui uses
stb_truetype). Licence: FreeType License (FTL), credited in the repo root's `THIRD_PARTY_NOTICES.txt`.

| file | what |
|---|---|
| `include/` | the headers of the source below (`freetype.h` says 2.14.1) |
| `lib/freetype.lib` | the import library of `DX12/freetype.dll` (219 exports) |
| `DX12/freetype.dll` | the DLL. No build step copies it beside an exe: Game.exe and Editor.exe started from `DX12/` load it through the working directory, and `tools/package_game.py` copies it into a package |

## The build in this tree (2026-10-18, `docs/design/Glyph_Bake_Async_Plan.md` GB1)

- Source: the local tree `F:\librarires\freetype-master\freetype-master`, a FreeType master snapshot taken after
  2.14.1 (`freetype.h` reports 2.14.1; `docs/CHANGES` opens an unreleased "2.14.1 and 2.14.2" section; files dated
  2025-12-21, no VCS metadata). The DLL used before this one was the Debug build of the same tree.
- Project and configuration: `builds\windows\vc2010\freetype.vcxproj`, **Release|x64** (`/O2 /Ob2 /Oi /MD /Gy`,
  `NDEBUG`, `DLL_EXPORT`, no `FT_DEBUG_*`), MSVC 14.44.35207 (Visual Studio 2022). Built 2026-10-18 from a copy
  of that tree, nothing changed in it.
- Imports: `KERNEL32.dll`, `VCRUNTIME140.dll` and five `api-ms-win-crt-*` sets: the redistributable runtime.
- SHA-256: `freetype.dll` `3943656902b242a185d76d71473589bb0d575a55df5738e8332670b7bb1edf50`,
  `freetype.lib` `d4afcf7a8eda8b4bbd721d13a4324ad1e8b5cf500cf5146f4e6fcc1f910b635b`.

The Debug build it replaced (`objs\x64\Debug` of the same tree, `freetype.dll` SHA-256 `101b4cc1...`) was compiled
with `/Od` and `FT_DEBUG_LEVEL_TRACE` and imported `VCRUNTIME140D.dll` and `ucrtbased.dll`, which only a Visual
Studio install provides: a package started on another machine failed with 0xC0000135. It also baked a CJK glyph
2.3x slower. The two builds bake the same bytes: the outline SDF and bsdf output of every real glyph of the 9
engine fonts (51,178) hash the same, and ft_ui's pinned prewarm hash did not move (plan sec.10).

## Rebuilding

The project's `AfterBuild` target copies the outputs into `<tree>\objs` and `<tree>\..\freetype-demos\bin` whatever
`OutDir` says (`freetype.vcxproj:518-524`), so build a copy of the tree, never the tree itself:

```
robocopy <tree> <work>\freetype-master /E /XD .vs objs
MSBuild.exe <work>\freetype-master\builds\windows\vc2010\freetype.vcxproj -p:Configuration=Release -p:Platform=x64 -p:OutDir=<out>\ -p:IntDir=<obj>\ -m
```

(`Current\Bin\amd64\MSBuild.exe`, from a `vcvars64` prompt.) Then copy `freetype.dll` to `DX12/` and `freetype.lib`
here, and check:

- `dumpbin /dependents freetype.dll` names no `*D.dll` (Debug CRT);
- no older `freetype.dll` sits beside an exe in `build/ide`, `build/ci` or `build/pkg` - the loader prefers the
  exe's folder over the working directory, and `package_game.py` takes the first copy it finds in `build/pkg`,
  then `build/ide`, then `DX12/`;
- `python tools/verify.py --mode affected --matrix` at least (the `freetype.dll` / `external/FreeType/**` rule selects
  `test_font_prewarm.py`, `test_font_sdf_fallback.py` (four prewarm hashes, outline SDF and the 8SSEDT fallback over
  FreeType's coverage render), the ft_ui pin `[FONT] prewarm ... hash=a89545996487ff79`, the on-demand CJK bakes of
  `font_glyph_flood`, `font_glyph_flood_editor` and `font_glyph_same_frame`, the ui group and core; third-party code
  asks for the Retail / no-Streamline matrix).
