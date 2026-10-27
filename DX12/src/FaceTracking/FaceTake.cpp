#include "FaceTracking/FaceTake.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <istream>
#include <ostream>

namespace FaceTracking {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

std::string LineError(int line, const std::string& what)
{
    return "line " + std::to_string(line) + ": " + what;
}

bool ReadFloats(const Json& j, float* out, size_t n)
{
    if (!j.is_array() || j.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
    {
        if (!j[i].is_number()) return false;
        const double v = j[i].get<double>();
        if (!std::isfinite(v)) return false;
        out[i] = static_cast<float>(v);
    }
    return true;
}

bool ParseHeader(const Json& j, int lineNo, FaceTake& out, std::string& error)
{
    if (!j.is_object() || !j.contains("iface") || !j["iface"].is_number_integer())
    {
        error = LineError(lineNo, "the first line is not an .iface header (no integer \"iface\" version)");
        return false;
    }
    const int version = j["iface"].get<int>();
    if (version != kFaceTakeVersion)
    {
        error = LineError(lineNo, "take version " + std::to_string(version) + " - this engine reads version " +
                                      std::to_string(kFaceTakeVersion));
        return false;
    }
    if (j.contains("convention"))
    {
        if (!j["convention"].is_number_integer() || j["convention"].get<int>() != kFaceConvention)
        {
            error = LineError(lineNo, "\"convention\" must be " + std::to_string(kFaceConvention) +
                                          " (the face-space sign convention of FaceMath.h)");
            return false;
        }
    }
    if (j.contains("source"))
    {
        if (!j["source"].is_string())
        {
            error = LineError(lineNo, "\"source\" is not a string");
            return false;
        }
        out.sourceKind = j["source"].get<std::string>();
    }
    if (j.contains("mirrored"))
    {
        if (!j["mirrored"].is_boolean())
        {
            error = LineError(lineNo, "\"mirrored\" is not true / false");
            return false;
        }
        out.mirrored = j["mirrored"].get<bool>();
    }
    if (j.contains("fps"))
    {
        if (!j["fps"].is_number())
        {
            error = LineError(lineNo, "\"fps\" is not a number");
            return false;
        }
        out.fps = j["fps"].get<double>();
    }
    if (j.contains("channels"))
    {
        const Json& ch = j["channels"];
        if (!ch.is_array())
        {
            error = LineError(lineNo, "\"channels\" is not an array of names");
            return false;
        }
        for (const Json& name : ch)
        {
            if (!name.is_string() || name.get<std::string>().empty())
            {
                error = LineError(lineNo, "\"channels\" holds something that is not a channel name");
                return false;
            }
            if (out.channels.FindOrAdd(name.get<std::string>()) < 0)
            {
                error = LineError(lineNo, "more than " + std::to_string(kMaxExtraChannels) +
                                              " extra (non-ARKit) channels");
                return false;
            }
        }
    }
    return true;
}

bool ParseFrame(const Json& j, int lineNo, const FaceTake& take, FaceFrame& f, std::string& error)
{
    if (!j.is_object())
    {
        error = LineError(lineNo, "a frame is a JSON object");
        return false;
    }
    if (!j.contains("t") || !j["t"].is_number())
    {
        error = LineError(lineNo, "a frame needs its time \"t\" (seconds)");
        return false;
    }
    const double t = j["t"].get<double>();
    if (!std::isfinite(t) || t < 0.0)
    {
        error = LineError(lineNo, "\"t\" is not a finite number of seconds >= 0");
        return false;
    }
    f.sourceTimeUs = std::llround(t * 1e6);
    f.faceFound = 1;
    if (j.contains("found"))
    {
        if (!j["found"].is_boolean())
        {
            error = LineError(lineNo, "\"found\" is not true / false");
            return false;
        }
        f.faceFound = j["found"].get<bool>() ? 1 : 0;
    }
    f.confidence = 1.0f;
    if (j.contains("conf"))
    {
        if (!j["conf"].is_number())
        {
            error = LineError(lineNo, "\"conf\" is not a number");
            return false;
        }
        f.confidence = static_cast<float>(std::fmin(1.0, std::fmax(0.0, j["conf"].get<double>())));
    }
    if (j.contains("v"))
    {
        const Json& v = j["v"];
        if (!v.is_object())
        {
            error = LineError(lineNo, "\"v\" is not an object of channel: value");
            return false;
        }
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            const int ch = take.channels.Find(it.key());
            if (ch < 0)
            {
                error = LineError(lineNo, "channel '" + it.key() +
                                              "' is neither an ARKit channel nor in the header's \"channels\"");
                return false;
            }
            if (!it.value().is_number() || !std::isfinite(it.value().get<double>()))
            {
                error = LineError(lineNo, "channel '" + it.key() + "' is not a finite number");
                return false;
            }
            f.Set(ch, static_cast<float>(it.value().get<double>()));
        }
    }
    if (j.contains("head"))
    {
        float q[4];
        if (!ReadFloats(j["head"], q, 4))
        {
            error = LineError(lineNo, "\"head\" is not a quaternion [x, y, z, w]");
            return false;
        }
        const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (len < 1e-6f)
        {
            error = LineError(lineNo, "\"head\" is a zero quaternion");
            return false;
        }
        f.headRot = {q[0] / len, q[1] / len, q[2] / len, q[3] / len};
        f.flags |= kFaceHasHead;
    }
    if (j.contains("pos"))
    {
        float p[3];
        if (!ReadFloats(j["pos"], p, 3))
        {
            error = LineError(lineNo, "\"pos\" is not a position [x, y, z] in metres");
            return false;
        }
        f.headPos = {p[0], p[1], p[2]};
        f.flags |= kFaceHasHeadPos;
    }
    if (j.contains("eyes"))
    {
        const Json& e = j["eyes"];
        float left[2], right[2];
        if (!e.is_array() || e.size() != 2 || !ReadFloats(e[0], left, 2) || !ReadFloats(e[1], right, 2))
        {
            error = LineError(lineNo, "\"eyes\" is not [[yawLeft, pitchLeft], [yawRight, pitchRight]] in radians");
            return false;
        }
        f.SetEye(0, left[0], left[1]);
        f.SetEye(1, right[0], right[1]);
    }
    return true;
}

} // namespace

bool ParseTake(std::istream& in, FaceTake& out, std::string& error)
{
    out = FaceTake{};
    std::string line;
    int lineNo = 0;
    bool haveHeader = false;
    while (std::getline(in, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        const Json j = Json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded())
        {
            error = LineError(lineNo, "not valid JSON");
            return false;
        }
        if (!haveHeader)
        {
            if (!ParseHeader(j, lineNo, out, error)) return false;
            haveHeader = true;
            continue;
        }
        FaceFrame f;
        if (!ParseFrame(j, lineNo, out, f, error)) return false;
        if (!out.frames.empty() && f.sourceTimeUs < out.frames.back().sourceTimeUs)
        {
            error = LineError(lineNo, "\"t\" goes backwards");
            return false;
        }
        out.frames.push_back(f);
    }
    if (!haveHeader)
    {
        error = "the take is empty (no header line)";
        return false;
    }
    return true;
}

bool ReadTakeFile(const std::string& path, FaceTake& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open '" + path + "'";
        return false;
    }
    return ParseTake(in, out, error);
}

bool WriteTake(std::ostream& o, const FaceTake& take, std::string& error)
{
    uint64_t used[2] = {};
    for (const FaceFrame& f : take.frames)
    {
        used[0] |= f.present[0];
        used[1] |= f.present[1];
    }
    OrderedJson header;
    header["iface"] = kFaceTakeVersion;
    header["source"] = take.sourceKind;
    header["convention"] = kFaceConvention;
    header["mirrored"] = take.mirrored;
    if (take.fps > 0.0) header["fps"] = take.fps;
    OrderedJson channels = OrderedJson::array();
    for (int ch = 0; ch < kMaxFaceChannels; ++ch)
    {
        if (!((used[ch >> 6] >> (ch & 63)) & 1ull)) continue;
        const std::string_view name = take.channels.Name(ch);
        if (name.empty())
        {
            error = "channel " + std::to_string(ch) + " is present but has no name";
            return false;
        }
        channels.push_back(std::string(name));
    }
    header["channels"] = channels;
    o << header.dump() << '\n';
    for (const FaceFrame& f : take.frames)
    {
        OrderedJson j;
        j["t"] = static_cast<double>(f.sourceTimeUs) / 1e6;
        j["found"] = f.faceFound != 0;
        j["conf"] = f.confidence;
        OrderedJson v = OrderedJson::object();
        for (int ch = 0; ch < kMaxFaceChannels; ++ch)
            if (f.Has(ch)) v[std::string(take.channels.Name(ch))] = f.values[ch];
        j["v"] = v;
        if (f.flags & kFaceHasHead) j["head"] = {f.headRot.x, f.headRot.y, f.headRot.z, f.headRot.w};
        if (f.flags & kFaceHasHeadPos) j["pos"] = {f.headPos.x, f.headPos.y, f.headPos.z};
        if (f.flags & kFaceHasEyes)
            j["eyes"] = {{f.eyeYawPitch[0][0], f.eyeYawPitch[0][1]}, {f.eyeYawPitch[1][0], f.eyeYawPitch[1][1]}};
        o << j.dump() << '\n';
    }
    if (!o)
    {
        error = "the stream failed while writing";
        return false;
    }
    return true;
}

bool WriteTakeFile(const std::string& path, const FaceTake& take, std::string& error)
{
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o)
    {
        error = "cannot write '" + path + "'";
        return false;
    }
    return WriteTake(o, take, error);
}

} // namespace FaceTracking
