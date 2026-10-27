#include "Harness/AssetToolUpgrade.h"

#ifdef WITH_HARNESS

#include "Resource/AssetHeader.h"
#include "Resource/MaterialSerializer.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Harness
{
namespace
{
    using Json = nlohmann::json;
    namespace fs = std::filesystem;

    constexpr int kDone = 0, kRefused = 1, kUsage = 2;

    // Every extension whose files start with an AssetHeader.
    constexpr const char* kExtensions[] = {
        ".itex", ".imsh", ".imat", ".imatinst", ".iscn", ".iscene", ".ipfb", ".iskel", ".ianim", ".imorph",
        ".meshlib", ".aclip", ".ppprofile",
    };

    bool HasAssetExtension(const fs::path& p)
    {
        std::string ext = Sys::PathToUtf8(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* x : kExtensions)
            if (ext == x) return true;
        return false;
    }

    // The files under @p root (or @p root itself) with an asset extension, sorted; a directory that is a
    // reparse point (another checkout's content) is listed in @p skipped and not entered.
    bool Collect(const std::string& root, std::vector<fs::path>& files, Json& skipped)
    {
        std::error_code ec;
        const fs::path r = Sys::U8Path(root);
        if (fs::is_directory(r, ec))
        {
            for (auto it = fs::recursive_directory_iterator(r, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skipped.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && HasAssetExtension(it->path())) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            return true;
        }
        if (!fs::exists(r, ec)) return false;
        files.push_back(r);
        return true;
    }

    // The upgrader of a format, or nullptr when it has none. A format whose migration chain is empty
    // (current == minimum) never reaches here.
    using Upgrader = bool (*)(const std::string& path, const std::vector<std::uint8_t>& in,
                              std::vector<std::uint8_t>& out, std::string& why);

    Upgrader FindUpgrader(std::uint32_t magic)
    {
        if (magic == Resource::MAGIC_MATERIAL) return &Resource::UpgradeMaterialBlob;
        return nullptr;
    }
}

int RunUpgrade(Json& out)
{
    auto& cl = Sys::CommandLine::Get();
    const std::string asset = cl.Get("-asset");
    const bool        apply = cl.Has("-apply");
    if (asset.empty())
    {
        out = Json{ { "ok", false }, { "error", "usage" }, { "message", "-asset=<file or directory> is required" } };
        return kUsage;
    }
    std::vector<fs::path> files;
    Json skipped = Json::array();
    if (!Collect(asset, files, skipped))
    {
        out = Json{ { "ok", false }, { "error", "usage" }, { "message", "no file or directory at '" + asset + "'" } };
        return kUsage;
    }

    Json rows = Json::array();
    std::size_t nCurrent = 0, nUpgraded = 0, nWould = 0, nRefused = 0, nNewer = 0, nTooOld = 0, nNone = 0,
                nUnreadable = 0, nWritten = 0;
    for (const fs::path& p : files)
    {
        const std::string ps = Sys::RelativizeAssetPath(Sys::PathToUtf8(p));
        Json row{ { "path", ps } };
        std::vector<std::uint8_t> bytes;
        Resource::AssetHeader     hdr{};
        if (!Sys::ReadFileBytes(Sys::PathToUtf8(p), bytes) || bytes.size() < sizeof(hdr))
        {
            row["state"] = "unreadable";
            row["why"]   = "not readable, or shorter than an asset header";
            ++nUnreadable;
            rows.push_back(std::move(row));
            continue;
        }
        std::memcpy(&hdr, bytes.data(), sizeof(hdr));
        const Resource::AssetFormat* format = Resource::FindAssetFormat(hdr.magic);
        row["magic"] = Resource::MagicText(hdr.magic);
        if (!format)
        {
            row["state"] = "unreadable";
            row["why"]   = "a magic this engine has no format row for";
            ++nUnreadable;
            rows.push_back(std::move(row));
            continue;
        }
        row["format"] = format->kind;
        row["from"]   = hdr.version;
        row["to"]     = format->current;

        if (hdr.version == format->current)
        {
            row["state"] = "current";
            ++nCurrent;
        }
        else if (hdr.version > format->current)
        {
            row["state"] = "newer";
            row["why"]   = Resource::DescribeHeaderCheck(
                Resource::CheckHeader(bytes.data(), bytes.size(), hdr.magic));
            ++nNewer;
        }
        else if (hdr.version < format->minimum)
        {
            row["state"] = "too_old";
            row["why"]   = Resource::DescribeHeaderCheck(
                Resource::CheckHeader(bytes.data(), bytes.size(), hdr.magic));
            ++nTooOld;
        }
        else if (const Upgrader upgrade = FindUpgrader(hdr.magic); !upgrade)
        {
            row["state"] = "no_upgrader";
            row["why"]   = "the format has a migration chain but no upgrader here";
            ++nNone;
        }
        else
        {
            std::vector<std::uint8_t> upgraded;
            std::string               why;
            if (!upgrade(ps, bytes, upgraded, why))
            {
                row["state"] = "refused";
                row["why"]   = why;
                ++nRefused;
            }
            else if (apply && Sys::ThroughReparsePoint(Sys::PathToUtf8(p)))
            {
                row["state"] = "refused";
                row["why"]   = "reached through a junction (another checkout's file)";
                ++nRefused;
            }
            else if (!apply)
            {
                row["state"] = "would_upgrade";
                row["bytes"] = upgraded.size();
                ++nWould;
            }
            else if (!Sys::WriteFileBytesAtomic(Sys::PathToUtf8(p), upgraded.data(), upgraded.size()))
            {
                row["state"] = "refused";
                row["why"]   = "the write failed";
                ++nRefused;
            }
            else
            {
                std::vector<std::uint8_t> back;
                const bool ok = Sys::ReadFileBytes(Sys::PathToUtf8(p), back) &&
                                Resource::CheckHeader(back.data(), back.size(), hdr.magic).status ==
                                    Resource::HeaderStatus::Ok &&
                                Resource::GetHeader(back.data())->version == format->current;
                if (ok)
                {
                    row["state"] = "upgraded";
                    ++nUpgraded;
                    ++nWritten;
                }
                else
                {
                    row["state"] = "refused";
                    row["why"]   = "the file read back is not a current-version header";
                    ++nRefused;
                }
            }
        }
        rows.push_back(std::move(row));
    }

    const std::size_t nRefusals = nRefused + nNewer + nTooOld + nNone;
    out = Json{ { "ok", nRefusals == 0 }, { "apply", apply }, { "files", std::move(rows) },
                { "summary", { { "count", files.size() }, { "current", nCurrent }, { "upgraded", nUpgraded },
                               { "wouldUpgrade", nWould }, { "refused", nRefused }, { "newer", nNewer },
                               { "tooOld", nTooOld }, { "noUpgrader", nNone }, { "unreadable", nUnreadable },
                               { "written", nWritten } } } };
    if (!skipped.empty()) out["skippedReparse"] = std::move(skipped);
    LOG_INFO("[ASSETTOOL] upgrade %s: %zu file(s) - %zu current, %zu upgraded, %zu would upgrade, %zu refused, "
             "%zu newer, %zu too old, %zu without an upgrader, %zu unreadable",
             asset.c_str(), files.size(), nCurrent, nUpgraded, nWould, nRefused, nNewer, nTooOld, nNone, nUnreadable);
    return nRefusals == 0 ? kDone : kRefused;
}
} // namespace Harness

#endif // WITH_HARNESS
