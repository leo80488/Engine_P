#pragma once
// face_calibration.json: each source's calibration, kept by source name in the user's data
// folder. The format's one home: the Service reads and writes it through these, and
// tests/unit/face_map_test.cpp feeds them broken files. The file is the user's - a hand edit,
// an older or a newer build may have written it - so reading checks every value and never
// throws (a malformed file must not end the program at startup).
//
//   {"version": 1, "sources": {"<name>": {"neutral": {"<ARKit channel>": v}, "max": {...},
//    "head": [x, y, z, w], "hasNeutral": bool, "hasRange": bool, "frames": n}}}

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FacePoseMath.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <string>

namespace FaceTracking {

constexpr int kCalibrationFileVersion = 1;

struct StoredCalibration
{
    FacePose::Calibration cal;
    bool hasNeutral = false, hasRange = false;
    uint64_t frames = 0;
};
using StoredCalibrations = std::map<std::string, StoredCalibration, std::less<>>;

// Entries with neither a neutral nor a range are left out: there is nothing in them to keep.
inline std::string WriteCalibrationFile(const StoredCalibrations& all)
{
    nlohmann::json sources = nlohmann::json::object();
    for (const auto& [name, c] : all)
    {
        if (!c.hasNeutral && !c.hasRange) continue;
        nlohmann::json neutral = nlohmann::json::object(), maximum = nlohmann::json::object();
        for (int ch = 0; ch < kArkitChannelCount; ++ch)
        {
            const std::string n(ArkitChannelName(ch));
            if (c.cal.neutral[ch] != 0.0f) neutral[n] = c.cal.neutral[ch];
            if (c.cal.maximum[ch] != 1.0f) maximum[n] = c.cal.maximum[ch];
        }
        const DirectX::XMFLOAT4& h = c.cal.headNeutral;
        sources[name] = {{"neutral", neutral}, {"max", maximum}, {"head", {h.x, h.y, h.z, h.w}},
                         {"hasNeutral", c.hasNeutral}, {"hasRange", c.hasRange}, {"frames", c.frames}};
    }
    nlohmann::json root = nlohmann::json::object();
    root["version"] = kCalibrationFileVersion;
    root["sources"] = sources;
    return root.dump(2) + "\n";
}

// The calibrations in `text` into `out` (cleared first). False, and why, when it is not a file
// this build reads: not a JSON object, no version or another one, no "sources" object. Otherwise
// true, with every entry or value that is not what the format says left out (the rest of its entry
// kept) and counted in `skipped`: a channel value outside 0..1 or not a number, an unknown channel,
// a head that is not four finite numbers of some length (it stays the identity), a flag that is
// not a boolean, frames that are not a count, an entry with neither a neutral nor a range.
inline bool ReadCalibrationFile(const std::string& text, StoredCalibrations& out, std::string& why, int& skipped)
{
    out.clear();
    skipped = 0;
    try
    {
        const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            why = "not a JSON object";
            return false;
        }
        const auto version = root.find("version");
        if (version == root.end() || !version->is_number_integer())
        {
            why = "no version";
            return false;
        }
        if (version->get<int64_t>() != kCalibrationFileVersion)
        {
            why = "version " + std::to_string(version->get<int64_t>()) + ", this build reads " +
                  std::to_string(kCalibrationFileVersion);
            return false;
        }
        const auto sources = root.find("sources");
        if (sources == root.end() || !sources->is_object())
        {
            why = "no \"sources\" object";
            return false;
        }
        for (auto it = sources->begin(); it != sources->end(); ++it)
        {
            const nlohmann::json& j = it.value();
            if (!j.is_object())
            {
                ++skipped;
                continue;
            }
            StoredCalibration c;
            auto channels = [&](const char* key, float* values) {
                const auto o = j.find(key);
                if (o == j.end()) return;
                if (!o->is_object())
                {
                    ++skipped;
                    return;
                }
                for (auto v = o->begin(); v != o->end(); ++v)
                {
                    const int ch = FindArkitChannel(v.key());
                    const double x = v->is_number() ? v->get<double>() : -1.0;
                    if (ch < 0 || !(x >= 0.0 && x <= 1.0))
                    {
                        ++skipped;
                        continue;
                    }
                    values[ch] = static_cast<float>(x);
                }
            };
            channels("neutral", c.cal.neutral);
            channels("max", c.cal.maximum);
            // The capture's own rule: a maximum less than 0.05 above its neutral leaves no span.
            for (int ch = 0; ch < kArkitChannelCount; ++ch)
                if (c.cal.maximum[ch] < c.cal.neutral[ch] + 0.05f) c.cal.maximum[ch] = 1.0f;
            if (const auto h = j.find("head"); h != j.end())
            {
                bool ok = h->is_array() && h->size() == 4;
                double q[4] = {};
                for (size_t k = 0; ok && k < 4; ++k)
                {
                    ok = (*h)[k].is_number();
                    if (ok) q[k] = (*h)[k].get<double>();
                    ok = ok && std::isfinite(q[k]);
                }
                const double n = ok ? std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) : 0.0;
                if (n > 1e-6)
                    c.cal.headNeutral = {static_cast<float>(q[0] / n), static_cast<float>(q[1] / n),
                                         static_cast<float>(q[2] / n), static_cast<float>(q[3] / n)};
                else
                    ++skipped;
            }
            auto flag = [&](const char* key, bool& value) {
                const auto o = j.find(key);
                if (o == j.end()) return;
                if (o->is_boolean()) value = o->get<bool>();
                else ++skipped;
            };
            flag("hasNeutral", c.hasNeutral);
            flag("hasRange", c.hasRange);
            if (const auto fr = j.find("frames"); fr != j.end())
            {
                if (fr->is_number_unsigned()) c.frames = fr->get<uint64_t>();
                else ++skipped;
            }
            if (!c.hasNeutral && !c.hasRange)
            {
                ++skipped;
                continue;
            }
            out[it.key()] = c;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        out.clear();
        why = e.what();
        return false;
    }
}

} // namespace FaceTracking
