#include "Resource/MaterialLoader.h"
#include "Resource/MaterialResource.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include <sstream>
#include <cctype>

namespace Resource
{
    // Leading / trailing whitespace trimmed (returns a copy).
    static std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    LoadResult MaterialLoader::Load(const std::string& path, std::span<const std::uint8_t> data)
    {
        if (data.empty())
        {
            LOG_ERROR("MaterialLoader: empty data for '%s'", path.c_str());
            return {};
        }

        LoadResult refused;
        if (!ValidateHeader(data.data(), data.size(), MAGIC_MATERIAL, refused.failReason))
        {
            LOG_ERROR("MaterialLoader: invalid .imat header in '%s': %s", path.c_str(), refused.failReason.c_str());
            return refused;
        }

        auto resource = std::make_unique<MaterialResource>();

        // Parse line-by-line key=value text.
        std::istringstream ss(std::string(GetPayloadText(data.data(), data.size())));
        std::string line;
        while (std::getline(ss, line))
        {
            const std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#')
                continue;

            const size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key   = Trim(trimmed.substr(0, eq));
            const std::string value = Trim(trimmed.substr(eq + 1));
            if (!key.empty())
                resource->Set(key, value);
        }

        return { std::move(resource), nullptr };
    }
}
