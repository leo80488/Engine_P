#pragma once
// System/PathUtil.h - UTF-8 filesystem boundary helpers.
//
// Engine-wide: every std::string that holds a filesystem PATH is UTF-8.
// Windows filesystem APIs are UTF-16, and the narrow (char* / std::string)
// overloads of std::ifstream / std::ofstream / std::fstream, the
// std::filesystem::path(std::string) constructor and fs::path::string() all go
// through the process ANSI code page, which cannot represent Japanese / Chinese
// / Cyrillic / etc. characters: a UTF-8 path handed to them silently fails to
// open (or is mangled to '?'). These helpers convert UTF-8 <-> UTF-16 exactly at
// the OS boundary; keep paths as UTF-8 std::string and cross into wide only at
// the moment you touch the disk.
//
//   std::ifstream f(Sys::U8Path(path), std::ios::binary);   // open for read
//   std::ofstream o(Sys::U8Path(path), std::ios::binary);   // open for write
//   Sys::ReadFileBytes(path, bytes);                        // whole-file read
//   Sys::WriteFileBytes(path, bytes);                       // whole-file write (+mkdirs)
//   fs::exists(Sys::U8Path(path));                          // any fs query
//   std::string utf8 = Sys::PathToUtf8(p);                  // fs::path -> UTF-8
//                                                           // (NOT p.string()!)

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>

namespace Sys
{
    // ---- The two roots ----
    // The base split between shipped data and what the running app writes (the
    // sections below add the writable-root policy, the artifact root and the
    // content roots):
    //
    //   ExeDir()      READ-ONLY INSTALL ROOT. Shipped, immutable data laid down by
    //                 the installer: asset/, shaders/, game.ipak, sealed game.json.
    //                 Never write here at runtime.
    //
    //   UserDataDir() WRITABLE PER-USER ROOT (%LOCALAPPDATA%\<ProductName>).
    //                 Anything the running app produces: settings.json,
    //                 save<N>.json, pso_cache.bin, and log/ (the log and the
    //                 crash dumps; see LogDir()).
    //
    // Do not open a writable file by a bare relative path: it resolves against the
    // working directory, so under C:\Program Files the write is denied by UAC or
    // silently redirected into the per-user VirtualStore, two Windows users share
    // (and stomp) one save file, and a build launched from a shortcut, the debugger
    // or elevated (CWD = system32) reads a different settings.json and logs
    // somewhere nobody looks. Neither root is CWD-derived.
    //
    // Both return an absolute UTF-8 path with FORWARD slashes and NO trailing
    // separator, so `Sys::UserDataDir() + "/save0.json"` is always correct.
    // Both cache on first call (one Win32 query per process).

    // Directory containing the running .exe. Derived from GetModuleFileNameW,
    // never from the working directory.
    const std::string& ExeDir();

    // ---- Product name (the folder a shipped build owns) ----
    // %LOCALAPPDATA%\<ProductName>. Defaults to the running exe's own stem, so
    // `package_game.py --name MyGame` emits MyGame.exe and that build owns
    // %LOCALAPPDATA%\MyGame with no extra configuration: one flag decides both.
    // Derived from the exe, not from game.json: the name is needed before ANYTHING
    // resolves a writable path (the logger opens its file first), and game.json is
    // sealed inside game.ipak, which is not mounted until App::BootContent.
    //
    // SetProductName overrides it for hosts that want an explicit name (or a
    // name that differs from the exe). Call it BEFORE Logger::Initialize; after
    // the per-user root is resolved it logs a warning and does nothing, because
    // honouring it late would split the app's files across two folders.
    // Illegal filename characters are stripped rather than rejected.
    void               SetProductName(std::string name);
    const std::string& ProductName();

    // %LOCALAPPDATA%\<ProductName>, created on first call. Falls back to
    // ExeDir() (with a warning) if the known folder cannot be resolved or
    // created: a portable/dev layout still works, it just loses the per-user
    // separation.
    const std::string& UserDataDir();

    // ---- Writable root POLICY (settings.json / save<N>.json / log/) ----
    // The per-user root is right for a SHIPPED game and wrong for daily
    // authoring: in the editor these files sit in the project, so settings.json
    // stays diffable and committable and the log is next to the code. So the
    // host exe picks the root at startup - a RUNTIME policy, not #ifdef
    // WITH_EDITOR, because PathUtil.cpp lives in EngineCore.lib (compiled once,
    // linked into both exes); main.cpp is compiled per target and makes the choice.
    //
    // Call ONCE, before anything resolves a writable path (i.e. before
    // Logger::Initialize). Empty/absent -> defaults to UserDataDir().
    //
    // EVERY writable file goes through this: settings.json, save<N>.json,
    // pso_cache.bin, and log/ (the log and the crash dumps, one level down via
    // LogDir()). Everything the editor produces stays with the project; only a
    // packaged build writes into the profile. pso_cache.bin also READS a
    // prewarmed copy from ExeDir() but WRITES here (System/PsoCachePaths.cpp).
    void               SetWritableDataRoot(std::string absoluteRootUtf8);
    const std::string& WritableDataDir();

    // ---- Artifact root (harness runs only) ----
    // A THIRD root, owned by an automated run: `-artifacts=<dir>` on the command
    // line. Everything one run produces for a machine to read - DX12Log.txt,
    // run.json, frames.jsonl, screenshots, state dumps, the crash minidump - lands
    // here, so a caller can hand the whole directory to a reviewer, diff two runs,
    // or delete it wholesale.
    //
    // Do not repoint SetWritableDataRoot instead: the writable root also owns
    // settings.json and save<N>.json, so every automated run would silently boot
    // with DEFAULT settings - an invisible variable in exactly the runs meant to
    // be comparable. The artifact root is OUTPUT-only.
    //
    // Accepts an absolute path or one relative to the working directory (e.g.
    // `-artifacts=tests/_artifacts/adhoc/<name>` with cwd=DX12/).
    //
    // Returns false if the directory could not be created. It does NOT log:
    // it runs before Logger::Initialize (the log's own destination is this
    // directory), so reporting is the caller's job once the logger is up.
    bool               SetArtifactDir(std::string rootUtf8);

    // Empty when no `-artifacts=` was given. Test with HasArtifactDir() and fall
    // back to WritableDataDir(); never assume this is non-empty.
    const std::string& ArtifactDir();
    bool               HasArtifactDir();

    // The diagnostics ROOT: run.json, frames.jsonl, screenshots, state dumps,
    // and (via LogDir(), which may nest one level below this) the log and the
    // dumps. ArtifactDir() when one was given, WritableDataDir() otherwise.
    //
    // USE THIS, not WritableDataDir(), for anything a post-mortem reads. The log
    // and the dump must never be split across two directories (a bug report is
    // one folder): a dump left in %LOCALAPPDATA% is never found by whoever
    // collects the artifact directory.
    const std::string& DiagnosticsDir();

    // Where the LOG, its rotated .prev and the crash/hang dumps land:
    //
    //   with -artifacts=<dir> : <dir>          (flat)
    //   without it            : <writable>/log
    //
    // `log/` declutters a root that holds other things (for the editor the
    // writable root is the source tree). An artifact directory holds only one
    // run's output, and the tools read <artifacts>/DX12Log.txt (ci_check.py,
    // scenario_run.py, golden.py, ai_benchmark.py), so it stays flat.
    //
    // The dumps follow the log in BOTH cases, so a bug report is one folder
    // (see DiagnosticsDir()).
    //
    // Pure string concatenation: it creates nothing. Logger::Initialize makes
    // the directory, so the crash path can call this without doing filesystem
    // work while the process is dying.
    std::string LogDir();


    // ---- Encoding conversion ----
    // UTF-8 (std::string) -> UTF-16 (std::wstring).
    std::wstring Utf8ToWide(const std::string& utf8);
    // UTF-16 -> UTF-8.
    std::string  WideToUtf8(const wchar_t* wide);
    std::string  WideToUtf8(const std::wstring& wide);

    // ---- Path <-> string (lossless for non-ASCII) ----
    // Build a std::filesystem::path from a UTF-8 string. Unlike
    // fs::path(std::string) on MSVC, this does NOT reinterpret the bytes as the
    // ANSI code page: it round-trips non-ASCII correctly via UTF-16.
    std::filesystem::path U8Path(const std::string& utf8);

    // Extract a UTF-8 std::string from a path. Use this instead of
    // fs::path::string(), which narrows the native wide path to the ANSI code
    // page and destroys non-ASCII characters.
    std::string PathToUtf8(const std::filesystem::path& p);

    // ---- Stream helpers (UTF-8 path aware) ----
    // Return a stream opened on a UTF-8 path. Movable, so:
    //   auto f = Sys::OpenIfstream(path, std::ios::binary | std::ios::ate);
    //   if (!f) { ... }
    std::ifstream OpenIfstream(const std::string& utf8Path,
                               std::ios::openmode mode = std::ios::in);
    std::ofstream OpenOfstream(const std::string& utf8Path,
                               std::ios::openmode mode = std::ios::out);

    // ---- Whole-file convenience ----
    // Read an entire file into bytes / text. Returns false on open/read failure.
    bool ReadFileBytes(const std::string& utf8Path, std::vector<std::uint8_t>& out);
    bool ReadFileText (const std::string& utf8Path, std::string& out);

    // Write an entire file (binary). Creates parent directories as needed.
    bool WriteFileBytes(const std::string& utf8Path, const void* data, std::size_t size);
    bool WriteFileBytes(const std::string& utf8Path, const std::vector<std::uint8_t>& data);

    // The same, through a temporary sibling (unique per process and thread) and
    // a rename: a reader - another engine sharing the file, as parallel scenario
    // packs and several worktrees do with the shader and PSO caches - sees the
    // old file or the new one, never a torn one. A reader holding the old file
    // open makes the rename wait (retried for about a second); on failure the
    // old file is left as it was and the temporary is removed. @p replaceFailed
    // (optional) tells the two failures apart: true = the bytes were written
    // but the target stayed locked by a reader the whole time (another process
    // is using the file - usually not an error), false = the write itself failed.
    bool WriteFileBytesAtomic(const std::string& utf8Path, const void* data, std::size_t size,
                              bool* replaceFailed = nullptr);

    // ---- Filesystem queries on UTF-8 paths ----
    bool FileExists      (const std::string& utf8Path);
    bool CreateParentDirs(const std::string& utf8Path);   // mkdir -p on parent
    bool CreateDirs      (const std::string& utf8Path);   // mkdir -p on the path itself
    // True when @p utf8Path (a file) is reached through a reparse point between
    // it and the working directory - another checkout's content through a
    // junction (asset\Bistro): an editor never writes an asset there.
    bool ThroughReparsePoint(const std::string& utf8Path);

    // ---- Content roots (engine vs project) ----
    //   EngineDir()  read-only content shipped WITH the engine: shaders, default
    //                textures, fonts, the base scene, the UI widget kit.
    //   ProjectDir() the content of the project currently open. Writable; this
    //                is what the editor imports into and saves to.
    // Both default to the working directory (one root for both) until something
    // sets them.
    const std::string& EngineDir();
    void               SetEngineDir(std::string absoluteOrRelativeUtf8);
    const std::string& ProjectDir();
    void               SetProjectDir(std::string absoluteOrRelativeUtf8);

    // The open project's name, from its manifest. EMPTY until a manifest
    // supplies one, and that emptiness is load-bearing: ProjectDataDir() keeps
    // the flat layout while it is empty, so an existing install keeps finding
    // the saves it already wrote.
    const std::string& ProjectName();
    void               SetProjectName(std::string name);

    // Where per-project writable state belongs (saves, editor prefs; the PSO
    // cache stays in WritableDataDir()). WritableDataDir() while no project
    // name is set; WritableDataDir()/Projects/<name> once one is, so two
    // projects never share (and silently overwrite) one save0.json.
    std::string ProjectDataDir();

    // ---- Asset-path canonicalisation ----
    // Every asset path the engine SERIALISES must be project-root-relative with
    // forward slashes ("asset/Foo/bar.itex"): that is the key a packed game.ipak
    // is built with and the string AssetFS looks up at runtime, and an absolute
    // path hard-codes one machine's layout. The AssetBrowser already yields
    // relative paths; run an OS file dialog's result (always fully qualified)
    // through this before it reaches a component field or a serializer.
    //
    // Rules, in order:
    //   1. Backslashes -> forward slashes; strip any leading "./".
    //   2. Already rooted at "asset/" or "shaders/" -> unchanged.
    //   3. Contains a "/asset/" or "/shaders/" segment -> cut at the FIRST one.
    //      First, not last: a nested "asset/Foo/asset/bar.itex" must keep both
    //      segments. (This misfires only if the PROJECT ROOT itself is inside a
    //      directory named "asset" - accepted.)
    //   4. Absolute and under the working directory -> made relative to it.
    //   5. Otherwise returned unchanged: a genuinely external file the user
    //      browsed to (an .fbx on another drive) has no project-relative form,
    //      and mangling it would break the importers that read it.
    std::string RelativizeAssetPath(const std::string& utf8Path);

    // ---- Filename sanitisation ----
    // Turn an ARBITRARY authored name (a DCC material name, an entity name, a
    // morph name) into one filename component. UTF-8 IN, UTF-8 OUT.
    // Use this, not a hand-rolled "keep [A-Za-z0-9_-], else '_'" loop: `char` is
    // SIGNED on MSVC, so such a loop drops every UTF-8 byte (a Japanese name
    // collapses to ""), and its '_' mangles legal names ("Eyes+" -> "Eyes_").
    //
    // What it replaces, and nothing else:
    //   * the characters Win32 actually rejects:  < > : " / \ | ? *
    //   * control characters (0x00-0x1F, 0x7F)
    //   * '%' and '=': the engine's text containers (.iscn / .iscene / .ipfb) are
    //     "key=value" tokens with %XX escaping, so a filename carrying either
    //     would need every reader to be escape-aware to survive a round trip.
    //   * ' , ; [ ] { }: tools/package_game.py's path stop bytes - a reference
    //     containing one is truncated by the cook's backward walk, fails to
    //     resolve, and aborts a --strict package.
    // Each becomes @p replacement (default '_'). Runs of replacements are NOT
    // collapsed: "a<>b" stays "a__b", so two distinct names stay distinct.
    //
    // Everything else survives: every byte >= 0x80, space, '.', '#', '(', ')',
    // '+', '&', '-', '_', '`'. So "Eyes+" stays "Eyes+" and a Japanese name
    // stays Japanese; only "Finger's" becomes "Finger_s".
    //
    // It also handles the two ways Windows silently rewrites a name: trailing
    // dots/spaces are trimmed (the OS drops them, so a later exists() on the
    // string you kept would miss), and the reserved device names (CON, PRN, AUX,
    // NUL, COM1-9, LPT1-9, with or without an extension) are prefixed with '_'.
    //
    // Returns "" only for input that was entirely illegal or empty; callers
    // supply their own fallback ("mat_3", the entity index, ...).
    std::string SanitizeFileName(const std::string& utf8Name, char replacement = '_');
}
