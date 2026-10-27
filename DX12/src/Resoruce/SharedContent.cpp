#include "Resource/SharedContent.h"

#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Resource::SharedContent
{
namespace
{
    Config g_active;
    bool   g_configured = false;

    std::string Slashes(std::string s)
    {
        for (char& c : s) if (c == '\\') c = '/';
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }

    std::string ReadText(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    // The main checkout's current HEAD commit, read from its .git the way git
    // stores it (HEAD -> a loose ref or packed-refs). "" when unreadable.
    std::string MainHead(const fs::path& dx12)
    {
        const fs::path git = dx12.parent_path() / ".git";
        std::string head = ReadText(git / "HEAD");
        while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) head.pop_back();
        if (head.rfind("ref: ", 0) != 0) return head;
        const std::string ref = head.substr(5);
        std::string sha = ReadText(git / Sys::U8Path(ref));
        while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();
        if (!sha.empty()) return sha;
        const std::string packed = ReadText(git / "packed-refs");
        for (size_t pos = 0; pos < packed.size();)
        {
            const size_t eol = packed.find('\n', pos);
            const std::string line = packed.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            if (line.size() > 41 && line.compare(41, std::string::npos, ref) == 0) return line.substr(0, 40);
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        return {};
    }
}

const std::vector<std::string>& Prefixes()
{
    static const std::vector<std::string> kPrefixes{ "asset/", "engine/" };
    return kPrefixes;
}

const Config& Active() { return g_active; }

// Read one config and register its root after every root registered so far;
// the first one registered is Active(). @p named: -shared_content asked for
// this file, so its absence is an error rather than the norm.
static bool Register(const std::string& file, bool named)
{
    std::error_code ec;
    if (!fs::exists(Sys::U8Path(file), ec))
    {
        if (named) LOG_ERROR("[CONTENT] -shared_content='%s' does not exist: no shared root from it", file.c_str());
        return false;
    }

    const nlohmann::json j = nlohmann::json::parse(ReadText(Sys::U8Path(file)), nullptr, false);
    if (!j.is_object() || !j.contains("root") || !j["root"].is_string())
    {
        LOG_ERROR("[CONTENT] '%s' is not {\"root\": \"<main checkout DX12 folder>\", ...}: no shared root",
                  file.c_str());
        return false;
    }
    Config c;
    c.file = file;
    c.root = Slashes(j["root"].get<std::string>());
    c.mainHead = j.value("mainHead", std::string());
    if (j.contains("masked") && j["masked"].is_array())
        for (const auto& m : j["masked"])
            if (m.is_string()) c.masked.push_back(m.get<std::string>());

    const fs::path root = Sys::U8Path(c.root);
    if (!fs::is_directory(root / "asset", ec))
    {
        LOG_ERROR("[CONTENT] shared root '%s' has no asset/ folder: no shared root", c.root.c_str());
        return false;
    }
    if (fs::equivalent(root, Sys::U8Path(Sys::ProjectDir()), ec))
    {
        LOG_ERROR("[CONTENT] '%s' names this checkout itself as its shared root - ignored", file.c_str());
        return false;
    }

    // The shared root must come AFTER this checkout's own roots: a caller that registered none
    // (the offline asset tool) first gets the two roots App registers, project and engine.
    if (AssetFS::Get().SearchRootCount() == 0)
    {
        AssetFS::Get().AddSearchRoot("project", Sys::ProjectDir());
        AssetFS::Get().AddSearchRoot("engine", Sys::EngineDir());
    }
    AssetFS::Get().AddSharedRoot(c.root, Prefixes(), c.masked);
    const std::string head = MainHead(root);
    LOG_INFO("[CONTENT] shared root '%s': asset/ and engine/ files this checkout lacks are read from it "
             "(main checkout at %s, %zu path(s) deleted on this branch masked)",
             c.root.c_str(), head.empty() ? "?" : head.substr(0, 10).c_str(), c.masked.size());
    if (!c.mainHead.empty() && !head.empty() && head != c.mainHead)
        LOG_WARNING("[CONTENT] the main checkout moved (%s -> %s) since the mask was taken: an asset this "
                    "branch deleted or changed may now read the main checkout's version - "
                    "`python tools/worktree_content.py refresh`",
                    c.mainHead.substr(0, 10).c_str(), head.substr(0, 10).c_str());
    if (g_active.root.empty()) g_active = std::move(c);
    return true;
}

bool Configure()
{
    if (g_configured) return !g_active.root.empty();
    g_configured = true;

    // This checkout's own config, then -shared_content=<file> as ANOTHER root after it (a test's
    // temporary one; a worktree that reads the main checkout keeps doing so). -no_shared_content
    // drops both.
    // The config belongs to the checkout: a -project= folder outside it (a fixture project) has
    // none, and the engine root's - the checkout the exe runs from - applies.
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    std::string own = Sys::ProjectDir() + "/.shared_content.json";
    std::error_code ownEc;
    if (!fs::exists(Sys::U8Path(own), ownEc) && Sys::EngineDir() != Sys::ProjectDir())
        own = Sys::EngineDir() + "/.shared_content.json";
    const std::string named = cl.Get("-shared_content");
    if (cl.Has("-no_shared_content"))
    {
        std::error_code ec;
        if (!named.empty() || fs::exists(Sys::U8Path(own), ec))
            LOG_INFO("[CONTENT] -no_shared_content: no shared root - only this checkout's files resolve");
        return false;
    }
    bool any = Register(own, /*named*/ false);
    if (!named.empty()) any = Register(Slashes(named), /*named*/ true) || any;
    return any;
}
}
