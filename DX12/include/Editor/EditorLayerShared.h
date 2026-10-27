#pragma once
// EditorLayerShared.h - the small set of file-scope helpers/state shared between
// EditorLayer.cpp and its split panel translation units (EditorLayer_*.cpp).
// Shared mutable state lives here as ONE inline instance: TU-local copies would
// split silently (each TU reading/writing its own).
//
// Keep this header MINIMAL: a helper belongs here only when more than one
// EditorLayer_* TU uses it. Single-TU helpers stay in that TU's anonymous namespace.

#include <cstdint>
#include <string>
#include <vector>

class World;
using Entity = std::uint32_t;   // same as ECS/ECS.h
namespace PostProcess { struct PostProcessProfile; }

// ---- Dock-space window titles ----
// SetupDockSpace / PrimeViewportLayout (EditorLayer.cpp) and the split panel TUs
// must agree on these exact strings: ImGui docks windows by title.
inline constexpr const char* kWindowHierarchy      = "Hierarchy";
inline constexpr const char* kWindowViewport       = "Viewport";
inline constexpr const char* kWindowInspector      = "Inspector";
inline constexpr const char* kWindowContentBrowser = "Content Browser";
// The Output Log's visible title carries an error/warning badge
// ("Output Log (3!)"), so its ImGui ID is pinned with "###": every Begin and
// DockBuilder call must hash THIS string (ImHashStr restarts at "###").
inline constexpr const char* kWindowOutputLog      = "Output Log###OutputLog";

// Font Awesome availability, mirrored from EditorLayer::m_hasFontAwesomeIcons
// so the file-local Hierarchy helpers (DrawEntityTree) can gate icon drawing.
// Written once by EditorLayer::OnAttach (EditorLayer.cpp); read by the outliner
// TU. C++17 inline variable: ONE instance across every TU that includes this.
inline bool s_faIconsLoaded = false;

// Single-file save dialog; returns UTF-8 path (empty on cancel). Defined in
// EditorLayer.cpp (next to the other Win32 common-dialog wrappers, which stay
// TU-local there); declared here because the outliner's "Save as Prefab" path
// needs it too.
std::string SaveFileDialog(const char* filter, const char* defaultExt, const char* initDir);

// Single-file open dialog; returns UTF-8 path (empty on cancel). Defined in
// EditorLayer.cpp; shared with the main-menu TU (File > Open Scene..., Build >
// Set Startup Scene...).
std::string OpenFileDialog(const char* filter, const char* initDir);

// Multi-select open dialog; every chosen path as UTF-8 (empty on cancel).
// Defined in EditorLayer.cpp with the other dialog wrappers; the asset import
// picks its source files with it.
std::vector<std::string> OpenFileDialogMulti(const char* filter, const char* initDir);

// Point game.json's "startup_scene" at a registry name or a project-relative
// .iscene path, preserving the rest of the manifest. Defined in EditorLayer.cpp.
void WriteStartupSceneToManifest(const std::string& sceneIdOrPath);

// Whole-file read / write, UTF-8 path aware (Sys::ReadFileBytes /
// WriteFileBytes: the read is empty on failure, the write creates the
// directories). Defined in EditorLayer.cpp; the retarget, import and
// bone-overlay code read and write source files with them.
std::vector<uint8_t> ReadFileFully(const std::string& path);
bool WriteFileFully(const std::string& path, const std::vector<uint8_t>& data);

// The model (.iscn) an entity was spawned from - its own or an ancestor's
// SceneSourcePath; "" = none (a hand-built entity). Defined in EditorLayer.cpp;
// the retarget panel and the component editors use it.
std::string ModelPathOfEntity(World& world, Entity e);

// Every override group of one post-process profile as ImGui rows; true when a
// value changed. Defined in EditorLayer.cpp next to the Post-Process panel; the
// PostProcessVolume component editor draws its profile with it too.
bool DrawProfileInspector(PostProcess::PostProcessProfile& p);

// True while the Collision Mesh Baker's "Preview baked" swap is in the world
// (entities render the baked library; EditorLayer.cpp keeps the originals and
// restores them). Applying the baked collision refuses meanwhile.
bool BakePreviewSwapActive();
