#pragma once

// Font Awesome icon codepoints (Private Use Area) as UTF-8 strings for ImGui.
// Source font: engine/font/fontawesome/Font Awesome 7 Free-Solid-900.otf.
//
// EditorTheme merges the WHOLE Private Use Area (U+E000..U+F8FF) of that font into
// every editor face, so any Solid icon is usable by adding a constant here (there
// is no glyph-range list). If an icon renders as a box, the codepoint is not in the
// FREE SOLID set (Regular/Brands and Pro-only icons are not loaded).

#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace FontAwesome
{
    // ---- Asset-browser icons ----
    inline const char* kUser         = "\xef\x80\x87";  // U+F007  fa-user        (skeleton/character)
    inline const char* kFilm         = "\xef\x80\x88";  // U+F008  fa-film        (animation clip)
    inline const char* kFolder       = "\xef\x81\xbb";  // U+F07B  fa-folder
    inline const char* kFolderOpen   = "\xef\x81\xbc";  // U+F07C  fa-folder-open
    inline const char* kImage        = "\xef\x80\xbe";  // U+F03E  fa-image
    inline const char* kVideoCamera  = "\xef\x80\xbd";  // U+F03D  fa-video-camera (cinematic sequence)
    inline const char* kVolumeUp     = "\xef\x80\xa8";  // U+F028  fa-volume-up   (audio)
    // ---- Viewport toolbar icons ----
    inline const char* kTh           = "\xef\x80\x8a";  // U+F00A  fa-th (grid)
    inline const char* kThList       = "\xef\x80\x8b";  // U+F00B  fa-th-list (list view)
    inline const char* kTable        = "\xef\x83\x8e";  // U+F0CE  fa-table   (details view)
    inline const char* kMagnet       = "\xef\x81\xb6";  // U+F076  fa-magnet (snap)
    inline const char* kPlay         = "\xef\x81\x8b";  // U+F04B  fa-play
    inline const char* kPause        = "\xef\x81\x8c";  // U+F04C  fa-pause
    inline const char* kStop         = "\xef\x81\x8d";  // U+F04D  fa-stop
    inline const char* kStepForward  = "\xef\x81\x91";  // U+F051  fa-step-forward
    // ---- More asset-browser icons ----
    inline const char* kFileText     = "\xef\x83\xb6";  // U+F0F6  fa-file-text-o (script)
    inline const char* kFont         = "\xef\x80\xb1";  // U+F031  fa-font       (font/TTF)
    inline const char* kCode         = "\xef\x84\xa1";  // U+F121  fa-code       (shader)
    inline const char* kCube         = "\xef\x86\xb2";  // U+F1B2  fa-cube       (mesh)
    inline const char* kCubes        = "\xef\x86\xb3";  // U+F1B3  fa-cubes      (scene)
    inline const char* kPaintBrush   = "\xef\x87\xbc";  // U+F1FC  fa-paint-brush (material)
    // ---- Hierarchy entity-type icons (Unreal-style outliner) ----
    inline const char* kCamera       = "\xef\x80\xb0";  // U+F030  fa-camera      (camera)
    inline const char* kLightbulb    = "\xef\x83\xab";  // U+F0EB  fa-lightbulb   (light)
    inline const char* kSun          = "\xef\x86\x85";  // U+F185  fa-sun         (sun light)
    inline const char* kMoon         = "\xef\x86\x86";  // U+F186  fa-moon        (moon light)
    inline const char* kCloud        = "\xef\x83\x82";  // U+F0C2  fa-cloud       (sky / atmosphere)
    inline const char* kGlobe        = "\xef\x82\xac";  // U+F0AC  fa-globe       (world environment)
    inline const char* kGear         = "\xef\x80\x93";  // U+F013  fa-gear        (GI / settings)
    inline const char* kClock        = "\xef\x80\x97";  // U+F017  fa-clock       (time of day)
    inline const char* kFire         = "\xef\x81\xad";  // U+F06D  fa-fire        (VFX / particles)
    inline const char* kDroplet      = "\xef\x81\x83";  // U+F043  fa-tint        (water)
    inline const char* kMountain     = "\xef\x9b\xbc";  // U+F6FC  fa-mountain    (terrain)
    inline const char* kLeaf         = "\xef\x81\xac";  // U+F06C  fa-leaf        (grass)
    inline const char* kCircle       = "\xef\x84\x91";  // U+F111  fa-circle      (generic entity)
    // ---- Editor chrome: menus, viewport toolbar, status bar, settings ----
    inline const char* kMove         = "\xef\x82\xb2";  // U+F0B2  up-down-left-right (translate)
    inline const char* kRotate       = "\xef\x8b\xb1";  // U+F2F1  rotate            (rotate)
    inline const char* kScale        = "\xef\x90\xa4";  // U+F424  up-right-and-down-left-from-center (scale)
    inline const char* kEye          = "\xef\x81\xae";  // U+F06E  eye               (Show menu)
    inline const char* kEyeSlash     = "\xef\x81\xb0";  // U+F070  eye-slash
    inline const char* kLock         = "\xef\x80\xa3";  // U+F023  lock       (UI Editor: locked in the designer)
    inline const char* kLockOpen     = "\xef\x8f\x81";  // U+F3C1  lock-open
    inline const char* kFloppy       = "\xef\x83\x87";  // U+F0C7  floppy-disk       (save)
    inline const char* kFile         = "\xef\x85\x9b";  // U+F15B  file              (new)
    inline const char* kSearch       = "\xef\x80\x82";  // U+F002  magnifying-glass
    inline const char* kPlus         = "\xef\x81\xa7";  // U+F067  plus
    inline const char* kSliders      = "\xef\x87\x9e";  // U+F1DE  sliders           (settings)
    inline const char* kKeyboard     = "\xef\x84\x9c";  // U+F11C  keyboard
    inline const char* kInfo         = "\xef\x81\x9a";  // U+F05A  circle-info
    inline const char* kWarning      = "\xef\x81\xb1";  // U+F071  triangle-exclamation
    inline const char* kError        = "\xef\x81\x97";  // U+F057  circle-xmark
    inline const char* kTerminal     = "\xef\x84\xa0";  // U+F120  terminal          (log / console)
    inline const char* kPalette      = "\xef\x94\xbf";  // U+F53F  palette           (appearance)
    inline const char* kDisplay      = "\xef\x8e\x90";  // U+F390  display           (display / output)
    inline const char* kGauge        = "\xef\x98\xa5";  // U+F625  gauge-high        (performance)
    inline const char* kBug          = "\xef\x86\x88";  // U+F188  bug               (debug)
    inline const char* kHammer       = "\xef\x9b\xa3";  // U+F6E3  hammer            (build / bake)
    inline const char* kBoxArchive   = "\xef\x86\x87";  // U+F187  box-archive       (package)
    inline const char* kLayerGroup   = "\xef\x97\xbd";  // U+F5FD  layer-group       (layout / panels)
    inline const char* kWand         = "\xef\x83\x90";  // U+F0D0  wand-magic        (effects)
    inline const char* kUndo         = "\xef\x8b\xaa";  // U+F2EA  rotate-left
    inline const char* kRedo         = "\xef\x8b\xb9";  // U+F2F9  rotate-right
    inline const char* kTrash        = "\xef\x8b\xad";  // U+F2ED  trash-can
    inline const char* kCopy         = "\xef\x83\x85";  // U+F0C5  copy
    inline const char* kPaste        = "\xef\x83\xaa";  // U+F0EA  paste
    inline const char* kCrosshairs   = "\xef\x81\x9b";  // U+F05B  crosshairs        (focus)
    inline const char* kArrowDown    = "\xef\x81\xa3";  // U+F063  arrow-down        (snap to floor)
    inline const char* kRunning      = "\xef\x9c\x8c";  // U+F70C  person-running    (animation)
    inline const char* kChart        = "\xef\x88\x81";  // U+F201  chart-line        (profiler)
    inline const char* kBorderAll    = "\xef\xa1\x8c";  // U+F84C  border-all        (wireframe)
    inline const char* kExternal     = "\xef\x8d\x9d";  // U+F35D  up-right-from-square (open folder)
    inline const char* kBolt         = "\xef\x83\xa7";  // U+F0E7  bolt              (command palette)
    inline const char* kPen          = "\xef\x8c\x83";  // U+F303  pencil            (rename)
    inline const char* kDoorOpen     = "\xef\x94\xab";  // U+F52B  door-open         (exit)
    inline const char* kGamepad      = "\xef\x84\x9b";  // U+F11B  gamepad           (input)
    inline const char* kCheck        = "\xef\x80\x8c";  // U+F00C  check             (log: success)
}
