#include "Resource/AssetHeader.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace Resource
{
    bool StrictAssetVersions()
    {
#if WITH_HARNESS
        // Harness only: every format reads only its current version, as the one global ASSET_VERSION
        // did. A reverse arm of the migration packs: an older file is refused, so the pack that
        // loads one fails.
        static const bool s_strict = Sys::CommandLine::Get().Has("-asset_strict_version");
        return s_strict;
#else
        return false;
#endif
    }

    const char* HeaderStatusName(HeaderStatus status)
    {
        switch (status)
        {
        case HeaderStatus::Ok:         return "ok";
        case HeaderStatus::TooShort:   return "too_short";
        case HeaderStatus::WrongMagic: return "wrong_magic";
        case HeaderStatus::Newer:      return "newer";
        case HeaderStatus::Older:      return "older";
        case HeaderStatus::Truncated:  return "truncated";
        }
        return "unknown";
    }

    std::string MagicText(uint32_t magic)
    {
        std::string s(4, '?');
        for (int i = 0; i < 4; ++i)
        {
            const char c = static_cast<char>((magic >> (24 - 8 * i)) & 0xFF);
            if (c >= 32 && c < 127)
                s[i] = c;
        }
        return s;
    }

    std::string DescribeHeaderCheck(const HeaderCheck& check)
    {
        const AssetFormat* f    = FindAssetFormat(check.expectedMagic);
        const std::string  name = f ? f->kind : MagicText(check.expectedMagic);
        char buf[256];
        switch (check.status)
        {
        case HeaderStatus::Ok:
            return name + " header: ok";
        case HeaderStatus::TooShort:
            return name + " header: the file is shorter than the " + std::to_string(sizeof(AssetHeader)) +
                   "-byte asset header";
        case HeaderStatus::WrongMagic:
            return name + " header: magic '" + MagicText(check.foundMagic) + "' is not '" +
                   MagicText(check.expectedMagic) + "'";
        case HeaderStatus::Newer:
            std::snprintf(buf, sizeof(buf),
                          "%s header: version %u is newer than this engine reads (%u..%u) - written by a "
                          "newer build; open it with that build",
                          name.c_str(), check.version, check.minimum, check.current);
            return buf;
        case HeaderStatus::Older:
            if (check.version >= check.minimum)   // inside the range: only -asset_strict_version refuses it
                std::snprintf(buf, sizeof(buf),
                              "%s header: version %u is not the current %u and -asset_strict_version is on",
                              name.c_str(), check.version, check.current);
            else
                std::snprintf(buf, sizeof(buf),
                              "%s header: version %u is older than the oldest this engine still reads "
                              "(%u..%u); re-save it with an older build that reads it",
                              name.c_str(), check.version, check.minimum, check.current);
            return buf;
        case HeaderStatus::Truncated:
            return name + " header: the file ends before the metadata and payload the header declares";
        }
        return name + " header: unknown status";
    }

    std::string HeaderRefusal(const uint8_t* blob, size_t blobSize, uint32_t expectedMagic)
    {
        const HeaderCheck check = CheckHeader(blob, blobSize, expectedMagic);
        switch (check.status)
        {
        case HeaderStatus::WrongMagic:
        case HeaderStatus::Newer:
        case HeaderStatus::Older:
            return DescribeHeaderCheck(check);
        default:
            return {};
        }
    }

    void ReportUnsupportedVersion(const HeaderCheck& check)
    {
        static std::mutex                                  s_mutex;
        static std::vector<std::pair<uint32_t, uint16_t>> s_seen;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (const auto& [magic, version] : s_seen)
                if (magic == check.expectedMagic && version == check.version)
                    return;
            s_seen.emplace_back(check.expectedMagic, check.version);
        }
        LOG_ERROR("[ASSET_VERSION] %s", DescribeHeaderCheck(check).c_str());
    }
}
