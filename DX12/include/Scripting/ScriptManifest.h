#pragma once

// ScriptManifest: the explicit boot-script registry.
// One JSON file at the well-known path declares what boot-loads, as which kind, in what
// order; without the file the folder scan runs (directory implies kind). Logic / Scene /
// Config / BT scripts stay owner-driven and never appear here.
// Sol-free and json-free: the parser is plain data in / data out, so validation runs
// before any Lua executes.

#include <cstdint>
#include <string>
#include <vector>

namespace Scripting
{
    // Fixed well-known path: its existence selects manifest mode. It ships inside game.ipak
    // unchanged because the packager bundles asset/scripts/ wholesale (no extension filter).
    inline constexpr const char* kScriptManifestPath =
        "asset/scripts/script_manifest.json";

    enum class BootScriptKind : uint8_t { Global, Service, System, UI };
    const char* BootScriptKindName(BootScriptKind kind);

    struct BootScriptEntry
    {
        BootScriptKind kind     = BootScriptKind::Global;
        std::string    path;       // project-relative, forward slashes
        std::string    stem;       // filename stem: the registration id
        int32_t        order    = 0;
        // Set when the manifest declares `order`: an entry without the field must not stomp a
        // table-declared `order` back to 0.
        bool           hasOrder = false;
        bool           enabled  = true;
        std::string    systemSet;  // system entries only
        // `scope`, system entries only: "scene" runs the System only while the entered scene
        // lists it. Without the field the table's own `scope` (default "engine") applies.
        bool           sceneScoped = false;
        bool           hasScope    = false;
    };

    struct ScriptManifest
    {
        uint32_t version = 0;
        // Validated entries sorted by (stage, order, stem); stage order is
        // Global -> Service -> System -> UI. Disabled entries are kept so
        // the loader can log them; invalid ones are dropped into `errors`.
        std::vector<BootScriptEntry> entries;
    };

    // Parse and validate the manifest at @p manifestPath.
    // Returns false only on a file-level hard error (unreadable file, JSON parse failure,
    // wrong `version`, `entries` not an array): the manifest is unusable and the caller picks
    // the failure mode. A missing file is the caller's own check, not this error.
    // Entry-level violations (unknown kind, bad / missing path, duplicate path or (kind, stem),
    // both case-folded, `set` or `scope` on a non-system, a `scope` other than "engine" /
    // "scene", nonexistent file) append one message each to
    // @p errors and drop the entry; the function still returns true. An attended editor may
    // boot the survivors; an unattended run refuses on !errors.empty() before any chunk runs.
    // @p pakOnly validates enabled entry paths with HasInPakExact instead of the
    // pak + roots + disk Exists(): a packed non-editor boot must not honor loose files, or a
    // dropped-in manifest becomes arbitrary boot-time Lua.
    // Warning-grade issues (non-integer `order`, non-bool `enabled`) are logged in place and
    // do not populate @p errors.
    bool LoadScriptManifest(const std::string& manifestPath,
                            bool pakOnly,
                            ScriptManifest& out,
                            std::vector<std::string>& errors);
}
