#pragma once

// The editor's personal files (imgui.ini, editor_prefs.ini) and the tracked seeds they start from
// (imgui.default.ini, editor_prefs.default.ini, beside them under System/PathUtil's EngineDir()).
// The personal files are not tracked: a person's editor rewrites them on exit.

#include <filesystem>

struct ImGuiIO;

namespace Editor::PersonalFiles
{
    // <EngineDir>/<stem>.default<ext>: the seed of the personal file `live`.
    std::filesystem::path SeedPath(const std::filesystem::path& live);

    // Makes `live` from its seed when it does not exist. False when it still does not exist (no seed,
    // or the copy failed): the caller starts from compiled-in defaults.
    bool EnsureFromSeed(const std::filesystem::path& live);

    // The editor_prefs file a run loads, empty when there is none. A harness run (-unattended) reads
    // the seed, so a pack never depends on a person's preferences; -editor_prefs_developer (a harness
    // build's reverse arm) reads the person's file again. Any other run seeds a missing file and
    // reads it.
    std::filesystem::path PrefsToRead(const std::filesystem::path& live);

    // Points ImGui at <WritableDataDir>/imgui.ini, made from the seed when it is missing.
    void UseImGuiIni(ImGuiIO& io);
}
