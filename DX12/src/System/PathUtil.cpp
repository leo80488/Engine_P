#include "System/PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ShlObj.h>          // SHGetKnownFolderPath / FOLDERID_LocalAppData
#pragma comment(lib, "Shell32.lib")

#include "System/Log.h"

#include <system_error>
#include <cstring>           // std::strchr - SanitizeFolderName / SanitizeFileName
#include <cwchar>            // swprintf_s - WriteFileBytesAtomic's temporary name
#include <cctype>            // std::toupper - SanitizeFileName device-name test

namespace fs = std::filesystem;

namespace
{
    // Roots are concatenated ("root + \"/settings.json\"") and fed to the asset-path
    // helpers, which assume forward slashes; Win32 returns backslashes, so normalise once here.
    std::string NormalizeRoot(std::string p)
    {
        for (char& c : p) if (c == '\\') c = '/';
        // No trailing separator: callers rely on it. Keep "C:/" intact - the bare "C:" means
        // the drive's current directory on Windows.
        while (p.size() > 1 && p.back() == '/')
        {
            if (p.size() == 3 && p[1] == ':') break;   // "C:/": leave alone
            p.pop_back();
        }
        return p;
    }
}

namespace Sys
{
    // ==== Roots ====
    // See PathUtil.h for why no root is derived from the working directory. These may run
    // before Logger::Initialize (main.cpp asks UserDataDir() where to put the log); that is
    // fine: Logger::Log only enqueues and Initialize drains the queue.
    // ---- Content roots ----
    // Both default to the working directory, so a build that never calls the setters
    // resolves "asset/..." against the working directory.
    namespace
    {
        std::string& EngineDirStorage()
        {
            static std::string s_dir;
            return s_dir;
        }
        std::string& ProjectDirStorage()
        {
            static std::string s_dir;
            return s_dir;
        }
        std::string& ProjectNameStorage()
        {
            static std::string s_name;
            return s_name;
        }

        // "." is the honest spelling of "the working directory" and is what the
        // AssetFS search roots want: joining it produces "./asset/x", which
        // resolves the same as a bare "asset/x".
        std::string NormalizeRootDir(std::string d)
        {
            for (char& c : d) if (c == '\\') c = '/';
            while (d.size() > 1 && d.back() == '/') d.pop_back();
            return d.empty() ? std::string(".") : d;
        }
    }

    const std::string& EngineDir()
    {
        std::string& d = EngineDirStorage();
        if (d.empty()) d = ".";
        return d;
    }

    void SetEngineDir(std::string dir)
    {
        EngineDirStorage() = NormalizeRootDir(std::move(dir));
        LOG_INFO("PathUtil: engine content root = '%s'", EngineDirStorage().c_str());
    }

    const std::string& ProjectName()
    {
        static std::string s_empty;
        std::string& n = ProjectNameStorage();
        return n.empty() ? s_empty : n;
    }

    void SetProjectName(std::string name)
    {
        // Filesystem-safe: this becomes a directory name under the writable
        // root, and a project called "My Game: Part 2" must not silently fail
        // to create it.
        std::string clean;
        clean.reserve(name.size());
        for (char c : name)
        {
            const bool bad = (c == '\\' || c == '/' || c == ':' || c == '*' ||
                              c == '?'  || c == '"' || c == '<' || c == '>' ||
                              c == '|'  || static_cast<unsigned char>(c) < 0x20);
            clean.push_back(bad ? '_' : c);
        }
        while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
            clean.pop_back();
        ProjectNameStorage() = std::move(clean);
        if (!ProjectNameStorage().empty())
            LOG_INFO("PathUtil: project name = '%s'", ProjectNameStorage().c_str());
    }

    std::string ProjectDataDir()
    {
        const std::string& n = ProjectName();
        if (n.empty()) return WritableDataDir();   // flat layout: no Projects/ subfolder
        return WritableDataDir() + "/Projects/" + n;
    }

    const std::string& ProjectDir()
    {
        std::string& d = ProjectDirStorage();
        if (d.empty()) d = ".";
        return d;
    }

    void SetProjectDir(std::string dir)
    {
        ProjectDirStorage() = NormalizeRootDir(std::move(dir));
        LOG_INFO("PathUtil: project content root = '%s'", ProjectDirStorage().c_str());
    }

    const std::string& ExeDir()
    {
        static const std::string s_dir = []() -> std::string
        {
            std::wstring buf(MAX_PATH, L'\0');
            bool ok = false;
            for (;;)
            {
                const DWORD n = ::GetModuleFileNameW(
                    nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0)
                    break;                                  // hard failure
                if (n < buf.size()) { buf.resize(n); ok = true; break; }
                if (buf.size() >= 32768)
                    break;                                  // longer than any NT path
                buf.resize(buf.size() * 2);                 // ERROR_INSUFFICIENT_BUFFER
            }

            if (!ok)
            {
                // Should be unreachable; if it ever happens, CWD is strictly
                // better than an empty root (which would silently make every
                // path drive-relative).
                LOG_WARNING("PathUtil: GetModuleFileNameW failed (err=%lu); "
                            "falling back to the working directory",
                            static_cast<unsigned long>(::GetLastError()));
                std::error_code ec;
                const fs::path cwd = fs::current_path(ec);
                return ec ? std::string(".") : NormalizeRoot(PathToUtf8(cwd));
            }

            return NormalizeRoot(PathToUtf8(fs::path(buf).parent_path()));
        }();
        return s_dir;
    }

    // ==== Product name: the folder under %LOCALAPPDATA% a shipped build owns ====
    namespace
    {
        std::string g_productName;        // empty => derive from the exe stem
        bool        g_userDataResolved = false;

        // Windows forbids <>:"/\|?* and control chars in a name and silently strips trailing
        // dots/spaces ("Game." becomes "Game" and an exact-match compare fails). Sanitize
        // rather than reject, so an exe named "My Game v1.0" still works.
        std::string SanitizeFolderName(const std::string& in)
        {
            std::string out;
            out.reserve(in.size());
            for (unsigned char c : in)
            {
                if (c < 0x20 || std::strchr("<>:\"/\\|?*", static_cast<char>(c)))
                    continue;
                out.push_back(static_cast<char>(c));
            }
            while (!out.empty() && (out.back() == '.' || out.back() == ' '))
                out.pop_back();
            if (out.size() > 64) out.resize(64);
            return out;
        }
    }

    void SetProductName(std::string name)
    {
        const std::string clean = SanitizeFolderName(name);
        if (clean.empty())
        {
            LOG_WARNING("PathUtil: SetProductName('%s') sanitises to nothing; "
                        "keeping '%s'", name.c_str(), ProductName().c_str());
            return;
        }
        if (g_userDataResolved && clean != ProductName())
        {
            // Silently ignoring this would split the app's files across two
            // folders for the rest of the process, which is worse than the log.
            LOG_WARNING("PathUtil: SetProductName('%s') came too late — the "
                        "per-user root is already resolved to '%s'. Call it "
                        "before Logger::Initialize.",
                        clean.c_str(), UserDataDir().c_str());
            return;
        }
        g_productName = clean;
    }

    const std::string& ProductName()
    {
        // Default: the exe's own stem. package_game.py --name MyGame emits MyGame.exe, so the
        // shipped build owns %LOCALAPPDATA%\MyGame with no configuration, and the name is known
        // before anything initialises (unlike anything sealed in game.ipak).
        static const std::string s_derived = []() -> std::string
        {
            std::wstring buf(MAX_PATH, L'\0');
            const DWORD n = ::GetModuleFileNameW(
                nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0 || n >= buf.size())
                return "DX12Engine";
            buf.resize(n);
            const std::string stem =
                SanitizeFolderName(PathToUtf8(fs::path(buf).stem()));
            return stem.empty() ? std::string("DX12Engine") : stem;
        }();
        return g_productName.empty() ? s_derived : g_productName;
    }

    const std::string& UserDataDir()
    {
        static const std::string s_dir = []() -> std::string
        {
            PWSTR raw = nullptr;
            // KF_FLAG_CREATE: LocalAppData exists on every sane profile, but a
            // freshly-provisioned one may not have it materialised yet.
            const HRESULT hr = ::SHGetKnownFolderPath(
                FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw);

            std::string base;
            if (SUCCEEDED(hr) && raw)
                base = WideToUtf8(raw);
            if (raw)
                ::CoTaskMemFree(raw);

            if (base.empty())
            {
                LOG_WARNING("PathUtil: SHGetKnownFolderPath(LocalAppData) failed "
                            "(hr=0x%08X); user data falls back to the exe directory",
                            static_cast<unsigned int>(hr));
                return ExeDir();
            }

            const std::string candidate = NormalizeRoot(base) + "/" + ProductName();
            std::error_code ec;
            fs::create_directories(U8Path(candidate), ec);
            // create_directories treats "already exists" as success, so a non-empty ec is a real
            // failure (read-only profile, roaming policy, disk full). Fall back now rather than
            // fail file-by-file later, far from the cause.
            if (ec)
            {
                LOG_WARNING("PathUtil: cannot create '%s' (%s); user data falls "
                            "back to the exe directory",
                            candidate.c_str(), ec.message().c_str());
                return ExeDir();
            }
            return candidate;
        }();
        g_userDataResolved = true;   // any SetProductName after this is too late
        return s_dir;
    }

    // ==== Writable root ====
    // A runtime setter, not #ifdef WITH_EDITOR: EngineCore.lib is compiled once for every
    // exe and only main.cpp sees the per-target defines (see PathUtil.h).
    namespace
    {
        std::string g_writableRoot;   // empty => fall through to UserDataDir()
    }

    void SetWritableDataRoot(std::string absoluteRootUtf8)
    {
        if (absoluteRootUtf8.empty())
        {
            g_writableRoot.clear();
            return;
        }
        g_writableRoot = NormalizeRoot(absoluteRootUtf8);

        std::error_code ec;
        fs::create_directories(U8Path(g_writableRoot), ec);
        if (ec)
        {
            LOG_WARNING("PathUtil: writable root '%s' unusable (%s); falling back "
                        "to the per-user root", g_writableRoot.c_str(),
                        ec.message().c_str());
            g_writableRoot.clear();
        }
    }

    const std::string& WritableDataDir()
    {
        // Deliberately re-checked on every call rather than cached: the setter
        // runs before Logger::Initialize, but a caller that resolves a path even
        // earlier must not freeze the default in place.
        return g_writableRoot.empty() ? UserDataDir() : g_writableRoot;
    }

    // ==== Artifact root (contract in PathUtil.h) ====
    // Output only, deliberately not the writable root, so settings.json and save<N>.json
    // keep resolving where they live.
    namespace { std::string g_artifactRoot; }   // empty => no -artifacts= given

    bool SetArtifactDir(std::string rootUtf8)
    {
        if (rootUtf8.empty())
        {
            g_artifactRoot.clear();
            return true;
        }

        // Store the root ABSOLUTE (resolved against the CWD): the harness writes into it from
        // several places, and the crash handler may run after the working directory changed - a
        // relative root would scatter one run's artifacts across two folders.
        std::error_code ec;
        fs::path p = U8Path(rootUtf8);
        if (!p.is_absolute())
        {
            const fs::path cwd = fs::current_path(ec);
            if (ec) return false;
            p = cwd / p;
        }

        ec.clear();
        fs::create_directories(p, ec);
        if (ec) return false;

        // weakly_canonical rather than canonical: the directory exists by now,
        // but a parent may be a symlink/junction, and canonical throws on the
        // paths weakly_canonical merely simplifies.
        const fs::path resolved = fs::weakly_canonical(p, ec);
        g_artifactRoot = NormalizeRoot(PathToUtf8(ec ? p : resolved));
        return true;
    }

    const std::string& ArtifactDir()  { return g_artifactRoot; }
    bool               HasArtifactDir() { return !g_artifactRoot.empty(); }

    const std::string& DiagnosticsDir()
    {
        return g_artifactRoot.empty() ? WritableDataDir() : g_artifactRoot;
    }

    // Contract in PathUtil.h. Creates nothing: Logger::Initialize makes the directory, so the
    // crash handler can build a dump path here without touching the filesystem.
    std::string LogDir()
    {
        // -artifacts=<dir> is a per-run directory holding only this run's output, so the log
        // goes at its top: tools read <artifacts>/DX12Log.txt.
        if (HasArtifactDir()) return ArtifactDir();

        // The default root is long-lived and shared (for the editor, the source tree beside
        // settings.json, imgui.ini and pso_cache.bin), so the log gets its own subfolder.
        return WritableDataDir() + "/log";
    }


    // ==== Encoding conversion (CP_UTF8) ====
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        const int wlen = ::MultiByteToWideChar(
            CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0) return {};
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        ::MultiByteToWideChar(
            CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
        return w;
    }

    std::string WideToUtf8(const wchar_t* wide)
    {
        if (!wide || !*wide) return {};
        const int len = ::WideCharToMultiByte(
            CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};                       // len includes null terminator
        std::string s(static_cast<size_t>(len - 1), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, wide, -1, s.data(), len, nullptr, nullptr);
        return s;
    }

    std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty()) return {};
        const int len = ::WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string s(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            s.data(), len, nullptr, nullptr);
        return s;
    }

    // ==== Path <-> UTF-8 string ====
    // Route through the wide form so non-ASCII survives: fs::path(std::string) and
    // path::string() go through the ANSI code page.
    fs::path U8Path(const std::string& utf8)
    {
        return fs::path(Utf8ToWide(utf8));
    }

    std::string PathToUtf8(const fs::path& p)
    {
        // path::native() is std::wstring on Windows: convert it directly.
        return WideToUtf8(p.native());
    }

    // ==== Stream helpers ====
    // The fstream path constructor (C++17) opens the path's native wide form on Windows.
    std::ifstream OpenIfstream(const std::string& utf8Path, std::ios::openmode mode)
    {
        return std::ifstream(U8Path(utf8Path), mode);
    }

    std::ofstream OpenOfstream(const std::string& utf8Path, std::ios::openmode mode)
    {
        return std::ofstream(U8Path(utf8Path), mode);
    }

    // ==== Whole-file helpers ====
    bool ReadFileBytes(const std::string& utf8Path, std::vector<std::uint8_t>& out)
    {
        std::ifstream f(U8Path(utf8Path), std::ios::binary | std::ios::ate);
        if (!f) return false;
        const std::streamsize sz = f.tellg();
        if (sz < 0) return false;
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(sz));
        if (sz == 0) return true;
        return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), sz));
    }

    bool ReadFileText(const std::string& utf8Path, std::string& out)
    {
        std::vector<std::uint8_t> bytes;
        if (!ReadFileBytes(utf8Path, bytes)) return false;
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    bool WriteFileBytes(const std::string& utf8Path, const void* data, std::size_t size)
    {
        const fs::path p = U8Path(utf8Path);
        std::error_code ec;
        if (p.has_parent_path())
            fs::create_directories(p.parent_path(), ec);   // best-effort; open still validates
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        if (size)
            f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(f);
    }

    bool WriteFileBytes(const std::string& utf8Path, const std::vector<std::uint8_t>& data)
    {
        return WriteFileBytes(utf8Path, data.data(), data.size());
    }

    bool WriteFileBytesAtomic(const std::string& utf8Path, const void* data, std::size_t size,
                              bool* replaceFailed)
    {
        if (replaceFailed) *replaceFailed = false;
        const fs::path target = U8Path(utf8Path);
        std::error_code ec;
        if (target.has_parent_path())
            fs::create_directories(target.parent_path(), ec);

        // The name does not keep the target's extension, so nothing that lists
        // a cache by extension picks up a leftover from a crash.
        wchar_t suffix[48];
        swprintf_s(suffix, L".%lu.%lu.tmp", ::GetCurrentProcessId(), ::GetCurrentThreadId());
        const std::wstring tmp = target.wstring() + suffix;
        {
            std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
            if (!f) return false;
            if (size)
                f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            f.flush();
            if (!f)
            {
                f.close();
                ::DeleteFileW(tmp.c_str());
                return false;
            }
        }
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            if (::MoveFileExW(tmp.c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING))
                return true;
            ::Sleep(20);
        }
        ::DeleteFileW(tmp.c_str());
        if (replaceFailed) *replaceFailed = true;
        return false;
    }

    // ==== Filesystem queries ====
    bool FileExists(const std::string& utf8Path)
    {
        std::error_code ec;
        return fs::exists(U8Path(utf8Path), ec);
    }

    bool CreateParentDirs(const std::string& utf8Path)
    {
        const fs::path p = U8Path(utf8Path);
        if (!p.has_parent_path()) return true;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        return !ec;
    }

    bool ThroughReparsePoint(const std::string& utf8Path)
    {
        std::error_code ec;
        const fs::path cwd = fs::current_path(ec);
        const fs::path abs = fs::absolute(U8Path(utf8Path), ec).lexically_normal();
        for (fs::path cur = abs.parent_path(); !cur.empty() && cur != cwd; cur = cur.parent_path())
        {
            const DWORD a = GetFileAttributesW(cur.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
            if (cur == cur.parent_path()) break;
        }
        return false;
    }

    bool CreateDirs(const std::string& utf8Path)
    {
        std::error_code ec;
        fs::create_directories(U8Path(utf8Path), ec);
        return !ec;
    }

    std::string RelativizeAssetPath(const std::string& utf8Path)
    {
        if (utf8Path.empty()) return utf8Path;

        std::string p = utf8Path;
        for (char& c : p) if (c == '\\') c = '/';
        while (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);

        if (p.rfind("asset/", 0) == 0 || p.rfind("shaders/", 0) == 0)
            return p;

        std::size_t cut = std::string::npos;
        for (const char* root : { "/asset/", "/shaders/" })
        {
            const std::size_t at = p.find(root);
            if (at != std::string::npos && (cut == std::string::npos || at < cut))
                cut = at;
        }
        if (cut != std::string::npos)
            return p.substr(cut + 1);   // drop the leading '/'

        // No engine root segment. fs::relative only helps when the file lives under the working
        // directory; otherwise it makes a "../.." chain, worse than the absolute path.
        const fs::path abs = U8Path(p);
        if (!abs.is_absolute()) return p;

        std::error_code ec;
        const fs::path cwd = fs::current_path(ec);
        if (ec) return p;
        const fs::path rel = fs::relative(abs, cwd, ec);
        if (ec || rel.empty()) return p;

        std::string r = PathToUtf8(rel);
        for (char& c : r) if (c == '\\') c = '/';
        return r.rfind("..", 0) == 0 ? p : r;
    }

    // ==== SanitizeFileName ====
    // Contract in the header. Byte-oriented on purpose: every byte of a UTF-8 multi-byte
    // sequence is >= 0x80 and collides with nothing rejected here, so a byte scan keeps the
    // sequence intact. Compare as unsigned char: as signed char those bytes are negative.
    std::string SanitizeFileName(const std::string& utf8Name, char replacement)
    {
        std::string out;
        out.reserve(utf8Name.size());

        for (unsigned char c : utf8Name)
        {
            // Three groups are replaced; everything else (bytes >= 0x80, space, '.', '#', '(',
            // ')', '+', '&', '-') passes through:
            //   1. control characters and the set Win32 rejects;
            //   2. '%' and '=': container syntax - .iscn/.iscene/.ipfb values are "key=value" tokens
            //      with %XX escaping, and package_game.py normalize() percent-decodes every token;
            //   3. the cook's other path stop bytes (package_game.py _PATH_STOP_BYTES). They are legal
            //      filename characters, but the packager finds a reference by walking back from the
            //      extension to one, so "Finger'sNail.imat" captures as "sNail.imat" and --strict
            //      aborts the build. Narrowing that set would grow every capture instead.
            // Replaced, never dropped, so names differing only in punctuation stay distinct.
            const bool illegal = c < 0x20 || c == 0x7F ||
                                 std::strchr("<>:\"/\\|?*", static_cast<char>(c)) != nullptr ||
                                 std::strchr("%=", static_cast<char>(c)) != nullptr ||
                                 std::strchr("',;[]{}", static_cast<char>(c)) != nullptr;
            out += illegal ? replacement : static_cast<char>(c);
        }

        // Windows silently DROPS trailing dots and spaces when it creates the
        // file, so a caller that kept the un-trimmed string would later look up
        // a name that does not exist on disk. Trim them here instead.
        while (!out.empty() && (out.back() == '.' || out.back() == ' '))
            out.pop_back();
        // Leading spaces are legal but invisible, and a leading '.' would make
        // the whole name an extension.
        size_t lead = 0;
        while (lead < out.size() && (out[lead] == ' ' || out[lead] == '.')) ++lead;
        if (lead) out.erase(0, lead);

        if (out.empty()) return out;

        // Reserved DOS device names are rejected by the OS with or without an
        // extension ("CON", "con.imat", "COM3.txt" all fail), so test the stem.
        {
            const size_t dot  = out.find('.');
            std::string  stem = (dot == std::string::npos) ? out : out.substr(0, dot);
            for (char& sc : stem)
                sc = static_cast<char>(std::toupper(static_cast<unsigned char>(sc)));

            static const char* kDevices[] = { "CON", "PRN", "AUX", "NUL" };
            bool reserved = false;
            for (const char* d : kDevices)
                if (stem == d) { reserved = true; break; }
            if (!reserved && stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
                (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0))
                reserved = true;

            if (reserved) out.insert(out.begin(), '_');
        }

        return out;
    }
}
