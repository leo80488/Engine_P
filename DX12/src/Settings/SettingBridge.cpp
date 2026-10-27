#include "Settings/SettingBridge.h"

#include <utility>
#include <vector>

namespace Settings
{
namespace
{
    std::vector<std::pair<std::string_view, BoolBridge>>& BoolBridges()
    {
        static std::vector<std::pair<std::string_view, BoolBridge>> s_bridges;
        return s_bridges;
    }
}

void RegisterBoolBridge(std::string_view path, BoolBridge bridge)
{
    for (auto& [p, b] : BoolBridges())
        if (p == path) { b = bridge; return; }
    BoolBridges().emplace_back(path, bridge);
}

const BoolBridge* FindBoolBridge(std::string_view path)
{
    for (const auto& [p, b] : BoolBridges())
        if (p == path) return &b;
    return nullptr;
}
}
