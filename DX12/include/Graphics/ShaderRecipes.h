#pragma once
// Shader recipes: every shader compile this process asked for, so a hot reload can compile
// the new sources before it tears anything down.
// Both loaders record here: ShaderLibrary::LoadOrCompile (the pass libraries, static and
// dynamic ids) and ShaderDiskCache::LoadOrCompile (the passes that compile their own
// shaders). A recipe is the exact DxcCompiler::CompileOptions the loader used, keyed by its
// .ishdr, plus who holds it: a ShaderLibrary (by address, never dereferenced) or, on the
// ShaderDiskCache path, the pass's log tag.
// The reload (HotReload::ShaderReloader, driven by App's shader watcher):
//   1. RunPreflight(changed files) compiles - in parallel, touching no cache - every recipe
//      whose #include closure holds a changed file.
//   2. A recipe that compiled before and fails now refuses the reload: nothing is cleared,
//      so every PSO built from the old bytecode stays in use. One that failed before and
//      still fails is reported but does not refuse: only a failure the edit introduced
//      blocks the reload.
//   3. Otherwise Commit() writes the new .ishdr files atomically, BeginReload() opens a
//      generation and the renderer drops its caches; the lazy path then loads exactly what
//      the pre-flight compiled.
//   4. CheckCoverage(): which holders let go of the old version (a library that was
//      cleared, or a pass that loaded the recipe again). A holder that did neither (a pass
//      with no hot-reload support, e.g. ToneMapPass) keeps the old shader until a restart,
//      and the receipt says so instead of reporting a reload that did not happen.
// Thread-safe. The registry is small (one entry per .ishdr the process loaded).

#include "Graphics/DxcCompiler.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ShaderRecipes
{
    // One load, as a loader performed it (cache hit, compile, or failure).
    struct Load
    {
        DxcCompiler::CompileOptions opts;       // exactly what DXC got; sourceName = the path read
        std::string                 cachePath;  // the .ishdr: the recipe's identity
        std::string                 scanDir;    // where #include scanning falls back to (MaxIncludeTime's shaderDir)
        const void*                 owner = nullptr;   // the ShaderLibrary holding it; nullptr = a pass holds the bytes
        std::string                 tag;        // who, for humans: a pass's log tag, or "ShaderLibrary"
        bool                        ok = true;  // false: this compile failed
    };
    void Note(const Load& load);

    // ShaderLibrary::ClearCaches: everything @p owner holds reloads on next use.
    void NoteOwnerCleared(const void* owner);
    // ShaderLibrary's destructor / Shutdown: its holdings are gone.
    void ForgetOwner(const void* owner);

    struct Failure
    {
        std::string file;             // the recipe's source (as the loader named it)
        std::string entry;
        std::string message;          // DXC's diagnostics
        uint32_t    variants = 0;     // recipes that failed with this (file, entry, message)
        bool        preexisting = false;   // it did not compile before the edit either
    };

    struct Preflight
    {
        uint32_t recipes      = 0;    // recorded in total
        uint32_t affected     = 0;    // whose #include closure holds a changed file
        uint32_t compiled     = 0;
        uint32_t failed       = 0;    // NEW failures: these refuse the reload
        uint32_t stillFailing = 0;    // failed before and still do: reported, not refusing
        double   ms           = 0;
        std::vector<std::string> sources;    // distinct sources the edit reached
        std::vector<Failure>     failures;   // new failures first
        // Filled for Commit / CheckCoverage.
        std::vector<std::pair<std::string, std::vector<uint8_t>>> containers;   // cachePath -> .ishdr bytes
        std::vector<std::string> compiledKeys;                                    // cachePaths compiled OK

        bool Refused() const { return failed != 0; }
    };
    // @p changedFiles: paths as the file watcher saw them (absolute or relative
    // to the working directory; case and separators do not matter).
    Preflight RunPreflight(const std::vector<std::string>& changedFiles);

    // Write every compiled container (atomically). Returns how many were written;
    // one that could not be written is recompiled from the source on next use.
    uint32_t Commit(Preflight& pf);

    // Open a reload generation - call right before the renderer drops its caches.
    uint64_t BeginReload();

    struct Coverage
    {
        uint32_t holders  = 0;   // (recipe, holder) pairs the edit reached
        uint32_t reloaded = 0;   // ... that let go of the old version
        std::vector<std::string> stale;   // "<file>:<entry> (<holder>)" still on the old bytecode
    };
    Coverage CheckCoverage(const Preflight& pf, uint64_t generation);

    // Diagnostics / tests.
    size_t Count();

    // Every recipe this process recorded, kept when its holders go: ForgetOwner drops a recipe
    // from the live registry when the last library holding it is destroyed (a preview graph's
    // idle teardown, the end of the run), but this list never forgets. The variant dump
    // (-shader_variants) reads it.
    struct Recorded
    {
        std::string                 cachePath;   // the .ishdr, as the loader named it
        DxcCompiler::CompileOptions opts;        // its last load's options
        bool                        ok = true;   // its last load succeeded
        std::vector<std::string>    tags;        // who loaded it: "ShaderLibrary", a pass's log tag
    };
    std::vector<Recorded> Snapshot();   // sorted by cachePath
}
