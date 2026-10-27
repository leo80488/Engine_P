#pragma once

namespace Sys
{
    // ---- Editor-host policy ----
    // Is the process hosting this engine the authoring tool (Editor.exe) rather
    // than the shipped Game.exe?
    //
    // A RUNTIME flag, deliberately not `#ifdef WITH_EDITOR`: EngineCore.lib is
    // compiled once and linked into both exes, and only Editor.vcxproj defines
    // WITH_EDITOR, so an #ifdef inside EngineCore silently takes the shipped-game
    // branch in every exe, the editor included. Code compiled into EngineCore.lib
    // must not branch on WITH_EDITOR; ask this instead. Only main.cpp (and the
    // per-exe TUs listed in each .vcxproj) may use the macros directly.
    //
    // Set once from main.cpp, before the first frame. Defaults to false (the
    // answer for Game.exe), so a host that forgets to call it degrades to
    // shipped-game behaviour rather than to editor behaviour.
    void SetEditorHost(bool isEditorHost);
    bool IsEditorHost();

    // ---- Loose-file hot reload ----
    // Lua / behaviour-tree mtime polling and the asset watcher (material
    // reloads) run in the authoring host - and in a harness build of Game.exe
    // started with -hotreload, so a resident agent session (tools/engine_tool.py)
    // can edit a script or a material and check it without a restart. A shipped
    // Game.exe never pays the per-frame stat. Shader hot reload is not gated by
    // this: its watcher runs whenever the shader directories are loose files.
    bool HotReloadEnabled();
}
