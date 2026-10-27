#pragma once

// System/BuildInfo.h - which build this is: game.json's product name and version, the git commit and
// whether the tree was dirty, the branch, the configuration, and a buildId over all of them. The
// values come from tools/gen_build_info.py, run before each compile (Generators.targets); without Python
// they read "unknown". The boot log, run.json, crash reports and the exe's VERSIONINFO carry them.

namespace Sys
{
    struct BuildInfo
    {
        const char* product;
        const char* version;   // "major.minor.patch"
        const char* gitSha;    // 12 hex, or "unknown"
        bool        dirty;     // the tree had uncommitted changes to tracked files
        const char* branch;
        const char* config;    // Debug, Release, Profile, Retail
        const char* buildId;   // 16 hex
    };

    const BuildInfo& Build();

    // What a player sees the product called (the window title, which a Retail second launch
    // finds the first by): game.json's name, or "DX12 Engine" in a build made without it. Not
    // Sys::ProductName (PathUtil.h), the per-user folder's name, which follows the exe's.
    const char* ProductTitle();
}
