#pragma once

// The key/value helpers every component serializer uses (ComponentSerializers.cpp and a
// module's own serializers): one parse and one encoding, so a module's line reads and writes
// exactly like a core one.

#include "Resource/ComponentSerializers.h"   // KVMap

#include <cmath>
#include <cstdint>
#include <string>

namespace ComponentKV
{
    // ' ' and '%' escaped: a value is one whitespace-free token.
    inline std::string PercentEncode(const std::string& s)
    {
        std::string r; r.reserve(s.size());
        for (char c : s) {
            if (c == '%') r += "%25";
            else if (c == ' ') r += "%20";
            else r += c;
        }
        return r;
    }

    inline std::string PercentDecode(const std::string& s)
    {
        std::string r; r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return -1;
                };
                int h = hex(s[i+1]), l = hex(s[i+2]);
                if (h >= 0 && l >= 0) { r += static_cast<char>(h*16+l); i += 2; continue; }
            }
            r += s[i];
        }
        return r;
    }

    inline float GetF(const KVMap& kv, const char* key, float def = 0.f)
    {
        auto it = kv.find(key);
        if (it == kv.end()) return def;
        try { return std::stof(it->second); } catch (...) { return def; }
    }

    // Blend weights are always [0,1] and must be finite: a NaN weight flows into
    // XMQuaternionSlerp and out as a NaN pose, which reaches the skin matrix and
    // access-violates SceneBVH::BuildRecursive.
    inline float SanitizeWeight01(float w)
    {
        if (!std::isfinite(w)) return 1.f;
        return (w < 0.f) ? 0.f : (w > 1.f ? 1.f : w);
    }

    inline int GetI(const KVMap& kv, const char* key, int def = 0)
    {
        auto it = kv.find(key);
        if (it == kv.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }

    // 32-bit unsigned. std::stoul handles the full 0..0xFFFFFFFF range that bit
    // masks (viewMask, RenderLayer.mask, etc.) need; stoi throws out_of_range on
    // anything past INT_MAX and silently resets the value to def.
    inline uint32_t GetU(const KVMap& kv, const char* key, uint32_t def = 0)
    {
        auto it = kv.find(key);
        if (it == kv.end()) return def;
        try { return static_cast<uint32_t>(std::stoul(it->second)); }
        catch (...) { return def; }
    }

    inline std::string GetS(const KVMap& kv, const char* key, const std::string& def = "")
    {
        auto it = kv.find(key);
        return (it != kv.end()) ? PercentDecode(it->second) : def;
    }
}
