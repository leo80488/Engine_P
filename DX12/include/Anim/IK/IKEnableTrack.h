#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

struct IKEnableKey { uint32_t frame = 0; uint8_t enabled = 1; };
struct IKEnableTrack
{
    std::string name;
    std::vector<IKEnableKey> keys;
    uint32_t chainIndex = ~0u; // bound at load; never persisted
    bool Sample(float time, float rate) const
    {
        const float frame = std::max(0.f, time * rate);
        auto it = std::upper_bound(keys.begin(), keys.end(), frame,
            [](float f, const IKEnableKey& k) { return f < k.frame; });
        return it == keys.begin() || (it - 1)->enabled != 0;
    }
};
namespace Anim::IK {
inline std::string CanonicalIKName(std::string name)
{
    for (const auto& pair : {std::pair{"Ｉ", "I"}, std::pair{"Ｋ", "K"}})
        for (size_t i = 0; (i = name.find(pair.first, i)) != std::string::npos; )
            name.replace(i, std::char_traits<char>::length(pair.first), pair.second);
    return name;
}
}
