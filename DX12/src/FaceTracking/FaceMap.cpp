#include "FaceTracking/FaceMap.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace FaceTracking {
namespace {

using Json = nlohmann::json;

bool Fail(std::string& error, const std::string& what)
{
    error = what;
    return false;
}

bool ReadNumber(const Json& j, const char* key, float& out, std::string& error, const std::string& where)
{
    if (!j.contains(key)) return true;
    if (!j[key].is_number() || !std::isfinite(j[key].get<double>()))
        return Fail(error, where + ": \"" + key + "\" is not a number");
    out = static_cast<float>(j[key].get<double>());
    return true;
}

bool ReadString(const Json& j, const char* key, std::string& out, std::string& error, const std::string& where)
{
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return Fail(error, where + ": \"" + key + "\" is not a string");
    out = j[key].get<std::string>();
    return true;
}

bool ParseOp(const std::string& s, MapOp& op)
{
    static const std::pair<const char*, MapOp> kOps[] = {
        {"direct", MapOp::Direct}, {"max", MapOp::Max}, {"min", MapOp::Min}, {"avg", MapOp::Avg},
        {"sum", MapOp::Sum},       {"sub", MapOp::Sub}, {"mul", MapOp::Mul}, {"weighted", MapOp::Weighted}};
    for (const auto& [name, value] : kOps)
        if (s == name) { op = value; return true; }
    return false;
}

bool ParseRule(const Json& j, size_t index, MapRule& r, std::string& error)
{
    const std::string where = "morphs[" + std::to_string(index) + "]";
    if (!j.is_object()) return Fail(error, where + " is not an object");
    if (!j.contains("target") || !j["target"].is_string() || j["target"].get<std::string>().empty())
        return Fail(error, where + " has no \"target\"");
    r.target = j["target"].get<std::string>();
    if (j.contains("op"))
    {
        if (!j["op"].is_string() || !ParseOp(j["op"].get<std::string>(), r.op))
            return Fail(error, where + " (" + r.target + "): unknown \"op\"");
    }
    if (!j.contains("src")) return Fail(error, where + " (" + r.target + ") has no \"src\"");
    const Json& src = j["src"];
    auto addSource = [&](const Json& s) -> bool {
        MapSource ms;
        if (s.is_string()) ms.name = s.get<std::string>();
        else if (s.is_object() && s.contains("name") && s["name"].is_string())
        {
            ms.name = s["name"].get<std::string>();
            if (s.contains("weight"))
            {
                if (!s["weight"].is_number()) return Fail(error, where + " (" + r.target + "): a source weight is not a number");
                ms.weight = static_cast<float>(s["weight"].get<double>());
            }
        }
        else
            return Fail(error, where + " (" + r.target + "): a source is neither a name nor {name, weight}");
        if (ms.name.empty() || ms.name == "@") return Fail(error, where + " (" + r.target + "): an empty source name");
        r.src.push_back(std::move(ms));
        return true;
    };
    if (src.is_array())
    {
        for (const Json& s : src)
            if (!addSource(s)) return false;
    }
    else if (!addSource(src))
        return false;
    if (r.src.empty()) return Fail(error, where + " (" + r.target + "): \"src\" is empty");
    if ((r.op == MapOp::Direct) && r.src.size() != 1)
        return Fail(error, where + " (" + r.target + "): op \"direct\" takes one source");
    if (r.op == MapOp::Sub && r.src.size() < 2)
        return Fail(error, where + " (" + r.target + "): op \"sub\" takes a source and at least one to subtract");
    if (!ReadNumber(j, "gain", r.gain, error, where) || !ReadNumber(j, "offset", r.offset, error, where) ||
        !ReadNumber(j, "gamma", r.gamma, error, where) || !ReadNumber(j, "min", r.minV, error, where) ||
        !ReadNumber(j, "max", r.maxV, error, where) || !ReadNumber(j, "suppressK", r.suppressK, error, where) ||
        !ReadString(j, "suppressBy", r.suppressBy, error, where))
        return false;
    if (r.minV > r.maxV) return Fail(error, where + " (" + r.target + "): \"min\" is above \"max\"");
    if (j.contains("curve"))
    {
        const Json& c = j["curve"];
        if (!c.is_array() || c.size() < 2) return Fail(error, where + " (" + r.target + "): \"curve\" needs two or more [x, y] points");
        for (const Json& p : c)
        {
            if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
                return Fail(error, where + " (" + r.target + "): a curve point is not [x, y]");
            const float x = static_cast<float>(p[0].get<double>()), y = static_cast<float>(p[1].get<double>());
            if (!r.curve.empty() && !(x > r.curve.back().first))
                return Fail(error, where + " (" + r.target + "): curve x values must rise");
            r.curve.emplace_back(x, y);
        }
    }
    return true;
}

bool ParseVowels(const Json& j, VowelMap& v, std::string& error)
{
    if (!j.is_object()) return Fail(error, "\"vowels\" is not an object");
    DefaultVowelModel(v);
    v.enabled = true;
    if (j.contains("enabled"))
    {
        if (!j["enabled"].is_boolean()) return Fail(error, "vowels.enabled is not true / false");
        v.enabled = j["enabled"].get<bool>();
    }
    if (j.contains("targets"))
    {
        const Json& t = j["targets"];
        if (!t.is_object()) return Fail(error, "vowels.targets is not {a, i, u, e, o}");
        const char* keys[kVowelCount] = {"a", "i", "u", "e", "o"};
        for (int k = 0; k < kVowelCount; ++k)
            if (!ReadString(t, keys[k], v.targets[k], error, "vowels.targets")) return false;
    }
    if (j.contains("model"))
    {
        const Json& m = j["model"];
        if (m.is_string())
        {
            if (m.get<std::string>() != "default") return Fail(error, "vowels.model: only \"default\" or {matrix, bias}");
        }
        else if (m.is_object() && m.contains("matrix"))
        {
            const Json& mat = m["matrix"];
            if (!mat.is_array() || mat.size() != kVowelCount) return Fail(error, "vowels.model.matrix is not 5 rows (a i u e o)");
            for (int k = 0; k < kVowelCount; ++k)
            {
                if (!mat[k].is_array() || mat[k].size() != kVowelInputs) return Fail(error, "vowels.model.matrix rows have 6 columns");
                for (int c = 0; c < kVowelInputs; ++c)
                {
                    if (!mat[k][c].is_number()) return Fail(error, "vowels.model.matrix holds a non-number");
                    v.matrix[k][c] = static_cast<float>(mat[k][c].get<double>());
                }
            }
            if (m.contains("bias"))
            {
                const Json& b = m["bias"];
                if (!b.is_array() || b.size() != kVowelCount) return Fail(error, "vowels.model.bias is not 5 numbers");
                for (int k = 0; k < kVowelCount; ++k)
                {
                    if (!b[k].is_number()) return Fail(error, "vowels.model.bias holds a non-number");
                    v.bias[k] = static_cast<float>(b[k].get<double>());
                }
            }
        }
        else
            return Fail(error, "vowels.model is neither \"default\" nor {matrix, bias}");
    }
    return true;
}

bool ParseBones(const Json& j, BoneMap& b, std::string& error)
{
    if (!j.is_object()) return Fail(error, "\"bones\" is not an object");
    if (j.contains("chain"))
    {
        if (!j["chain"].is_array()) return Fail(error, "bones.chain is not an array");
        for (const Json& c : j["chain"])
        {
            ChainBone cb;
            if (!c.is_object()) return Fail(error, "bones.chain holds a non-object");
            if (!ReadString(c, "role", cb.role, error, "bones.chain") || !ReadString(c, "bone", cb.bone, error, "bones.chain") ||
                !ReadString(c, "fallbackRole", cb.fallbackRole, error, "bones.chain") ||
                !ReadNumber(c, "weight", cb.weight, error, "bones.chain"))
                return false;
            if (cb.role.empty() && cb.bone.empty()) return Fail(error, "bones.chain: an entry needs a \"role\" or a \"bone\"");
            if (cb.weight < 0.0f) return Fail(error, "bones.chain: a negative weight");
            b.chain.push_back(std::move(cb));
        }
    }
    if (j.contains("gain"))
    {
        const Json& g = j["gain"];
        if (!ReadNumber(g, "yaw", b.gainYaw, error, "bones.gain") || !ReadNumber(g, "pitch", b.gainPitch, error, "bones.gain") ||
            !ReadNumber(g, "roll", b.gainRoll, error, "bones.gain"))
            return false;
    }
    if (j.contains("limitsDeg"))
    {
        const Json& l = j["limitsDeg"];
        if (!ReadNumber(l, "yaw", b.limitYawDeg, error, "bones.limitsDeg") ||
            !ReadNumber(l, "pitch", b.limitPitchDeg, error, "bones.limitsDeg") ||
            !ReadNumber(l, "roll", b.limitRollDeg, error, "bones.limitsDeg"))
            return false;
        // They become clamp bounds [-limit, limit]: a negative one would pin the head at it.
        if (b.limitYawDeg < 0.0f || b.limitPitchDeg < 0.0f || b.limitRollDeg < 0.0f)
            return Fail(error, "bones.limitsDeg: a limit is an angle >= 0");
    }
    if (j.contains("faceBasis"))
    {
        const Json& fb = j["faceBasis"];
        if (fb.is_string())
        {
            if (fb.get<std::string>() != "auto") return Fail(error, "bones.faceBasis: \"auto\" or {forward, up}");
            b.basisAuto = true;
        }
        else if (fb.is_object())
        {
            b.basisAuto = false;
            if (!ReadString(fb, "forward", b.forward, error, "bones.faceBasis") || !ReadString(fb, "up", b.up, error, "bones.faceBasis"))
                return false;
        }
        else
            return Fail(error, "bones.faceBasis: \"auto\" or {forward, up}");
    }
    if (j.contains("eyes"))
    {
        const Json& e = j["eyes"];
        std::string mode = "none";
        if (!e.is_object() || !ReadString(e, "mode", mode, error, "bones.eyes") || !ReadString(e, "left", b.leftEye, error, "bones.eyes") ||
            !ReadString(e, "right", b.rightEye, error, "bones.eyes") || !ReadNumber(e, "maxYawDeg", b.eyeMaxYawDeg, error, "bones.eyes") ||
            !ReadNumber(e, "maxPitchDeg", b.eyeMaxPitchDeg, error, "bones.eyes"))
            return error.empty() ? Fail(error, "bones.eyes is not an object") : false;
        if (b.eyeMaxYawDeg < 0.0f || b.eyeMaxPitchDeg < 0.0f)
            return Fail(error, "bones.eyes: maxYawDeg / maxPitchDeg are angles >= 0");
        if (mode == "bones") b.eyes = EyesMode::Bones;
        else if (mode == "morphs") b.eyes = EyesMode::Morphs;
        else if (mode == "none") b.eyes = EyesMode::None;
        else return Fail(error, "bones.eyes.mode: bones, morphs or none");
    }
    if (j.contains("jaw"))
    {
        const Json& jw = j["jaw"];
        std::string mode = "none";
        if (!jw.is_object() || !ReadString(jw, "mode", mode, error, "bones.jaw") || !ReadString(jw, "bone", b.jawBone, error, "bones.jaw") ||
            !ReadNumber(jw, "maxDeg", b.jawMaxDeg, error, "bones.jaw"))
            return error.empty() ? Fail(error, "bones.jaw is not an object") : false;
        if (b.jawMaxDeg < 0.0f) return Fail(error, "bones.jaw: maxDeg is an angle >= 0");
        if (mode == "bone") b.jaw = JawMode::Bone;
        else if (mode == "none") b.jaw = JawMode::None;
        else return Fail(error, "bones.jaw.mode: bone or none");
    }
    return true;
}

float ApplyCurve(const std::vector<std::pair<float, float>>& c, float x)
{
    if (x <= c.front().first) return c.front().second;
    if (x >= c.back().first) return c.back().second;
    for (size_t i = 1; i < c.size(); ++i)
        if (x <= c[i].first)
        {
            const float t = (x - c[i - 1].first) / (c[i].first - c[i - 1].first);
            return c[i - 1].second + (c[i].second - c[i - 1].second) * t;
        }
    return c.back().second;
}

void EmitMax(std::vector<std::pair<int, float>>& out, int slot, float w)
{
    for (auto& [s, v] : out)
        if (s == slot)
        {
            v = std::max(v, w);
            return;
        }
    out.emplace_back(slot, w);
}

bool Present(const uint64_t present[2], int ch)
{
    return ch >= 0 && ch < kMaxFaceChannels && ((present[ch >> 6] >> (ch & 63)) & 1ull) != 0;
}

float Channel(const float* values, const uint64_t present[2], int ch)
{
    return Present(present, ch) ? values[ch] : 0.0f;
}

// Resolves a target (a morph, or "preset:<name>") to its output slots; false = nothing on this model.
bool ResolveTarget(const std::string& name, const FaceMapBindHooks& hooks, std::vector<std::pair<int, float>>& out)
{
    constexpr std::string_view kPreset = "preset:";
    if (name.compare(0, kPreset.size(), kPreset) == 0)
    {
        std::vector<std::pair<std::string, float>> morphs;
        if (!hooks.expandPreset || !hooks.expandPreset(std::string_view(name).substr(kPreset.size()), morphs)) return false;
        for (const auto& [morph, w] : morphs)
        {
            const int slot = hooks.findMorphSlot ? hooks.findMorphSlot(morph) : -1;
            if (slot >= 0) out.emplace_back(slot, w);
        }
        return !out.empty();
    }
    const int slot = hooks.findMorphSlot ? hooks.findMorphSlot(name) : -1;
    if (slot < 0) return false;
    out.emplace_back(slot, 1.0f);
    return true;
}

} // namespace

void DefaultVowelModel(VowelMap& v)
{
    // Columns: jawOpen, mouthFunnel, mouthPucker, stretch (avg L/R), smile (avg L/R), mouthClose.
    static const float kMatrix[kVowelCount][kVowelInputs] = {
        {1.20f, -0.60f, -0.60f, -0.30f, -0.20f, -0.50f}, // a: open jaw, no rounding
        {0.20f, -0.40f, -0.40f, 0.90f, 0.60f, -0.30f},   // i: lips spread
        {0.10f, 0.50f, 1.00f, -0.40f, -0.30f, -0.20f},   // u: lips rounded and pushed out
        {0.60f, -0.30f, -0.30f, 0.60f, 0.20f, -0.30f},   // e: half open, spread
        {0.70f, 1.00f, 0.30f, -0.40f, -0.30f, -0.30f},   // o: open and rounded
    };
    for (int k = 0; k < kVowelCount; ++k)
    {
        for (int c = 0; c < kVowelInputs; ++c) v.matrix[k][c] = kMatrix[k][c];
        v.bias[k] = 0.0f;
    }
}

void SolveVowels(const VowelMap& v, const float* values, const uint64_t present[2], float out[kVowelCount])
{
    static const int kJaw = FindArkitChannel("jawOpen"), kFunnel = FindArkitChannel("mouthFunnel"),
                     kPucker = FindArkitChannel("mouthPucker"), kStretchL = FindArkitChannel("mouthStretchLeft"),
                     kStretchR = FindArkitChannel("mouthStretchRight"), kSmileL = FindArkitChannel("mouthSmileLeft"),
                     kSmileR = FindArkitChannel("mouthSmileRight"), kClose = FindArkitChannel("mouthClose");
    const float x[kVowelInputs] = {
        Channel(values, present, kJaw),
        Channel(values, present, kFunnel),
        Channel(values, present, kPucker),
        0.5f * (Channel(values, present, kStretchL) + Channel(values, present, kStretchR)),
        0.5f * (Channel(values, present, kSmileL) + Channel(values, present, kSmileR)),
        Channel(values, present, kClose),
    };
    float sum = 0.0f;
    for (int k = 0; k < kVowelCount; ++k)
    {
        float y = v.bias[k];
        for (int c = 0; c < kVowelInputs; ++c) y += v.matrix[k][c] * x[c];
        out[k] = std::clamp(y, 0.0f, 1.0f);
        sum += out[k];
    }
    // The mouth can only be as open as the jaw (and the rounded lips) allow.
    const float limit = std::clamp(1.5f * x[0] + 0.5f * std::max(x[1], x[2]), 0.0f, 1.0f);
    if (sum > limit && sum > 0.0f)
        for (int k = 0; k < kVowelCount; ++k) out[k] *= limit / sum;
}

bool ParseFaceMap(const std::string& text, FaceMapProfile& out, std::string& error)
{
    out = FaceMapProfile{};
    std::string body = text;
    if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);
    const Json j = Json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return Fail(error, "not valid JSON");
    if (!j.is_object()) return Fail(error, "the mapping is not a JSON object");
    if (j.contains("version"))
    {
        if (!j["version"].is_number_integer() || j["version"].get<int>() != 1)
            return Fail(error, "\"version\" must be 1");
    }
    if (!ReadString(j, "name", out.name, error, "mapping") || !ReadString(j, "basePreset", out.basePreset, error, "mapping"))
        return false;
    if (j.contains("morphs"))
    {
        if (!j["morphs"].is_array()) return Fail(error, "\"morphs\" is not an array");
        size_t i = 0;
        for (const Json& r : j["morphs"])
        {
            MapRule rule;
            if (!ParseRule(r, i++, rule, error)) return false;
            out.morphs.push_back(std::move(rule));
        }
    }
    if (j.contains("vowels"))
    {
        if (!ParseVowels(j["vowels"], out.vowels, error)) return false;
        out.hasVowels = true;
    }
    if (j.contains("bones"))
    {
        if (!ParseBones(j["bones"], out.bones, error)) return false;
        out.hasBones = true;
    }
    if (j.contains("filters"))
    {
        const Json& f = j["filters"];
        if (!f.is_object() || !ReadString(f, "preset", out.filterPreset, error, "filters"))
            return error.empty() ? Fail(error, "\"filters\" is not an object") : false;
        out.hasFilters = true;
    }
    return true;
}

FaceMapProfile MergeFaceMaps(const FaceMapProfile& base, const FaceMapProfile& over)
{
    FaceMapProfile m = base;
    m.name = over.name.empty() ? base.name : over.name;
    m.basePreset = over.basePreset;
    std::set<std::string> replaced;
    for (const MapRule& r : over.morphs) replaced.insert(r.target);
    m.morphs.erase(std::remove_if(m.morphs.begin(), m.morphs.end(), [&](const MapRule& r) { return replaced.count(r.target) != 0; }),
                   m.morphs.end());
    m.morphs.insert(m.morphs.end(), over.morphs.begin(), over.morphs.end());
    if (over.hasVowels) { m.vowels = over.vowels; m.hasVowels = true; }
    if (over.hasBones) { m.bones = over.bones; m.hasBones = true; }
    if (over.hasFilters) { m.filterPreset = over.filterPreset; m.hasFilters = true; }
    return m;
}

bool CompileFaceMap(const FaceMapProfile& profile, const FaceSourceChannels& channels, const FaceMapBindHooks& hooks,
                    CompiledFaceMap& out, std::string& error)
{
    out = CompiledFaceMap{};
    // Targets in first-appearance order, each with its rules.
    std::vector<std::string> names;
    std::map<std::string, size_t> indexOf;
    std::vector<std::vector<const MapRule*>> rulesOf;
    for (const MapRule& r : profile.morphs)
    {
        auto [it, inserted] = indexOf.emplace(r.target, names.size());
        if (inserted)
        {
            names.push_back(r.target);
            rulesOf.emplace_back();
        }
        rulesOf[it->second].push_back(&r);
    }
    // Dependencies: "@x" in a source or suppressBy.
    const size_t n = names.size();
    std::vector<std::vector<size_t>> deps(n);
    for (size_t t = 0; t < n; ++t)
        for (const MapRule* r : rulesOf[t])
        {
            auto ref = [&](const std::string& s) -> bool {
                if (s.empty() || s[0] != '@') return true;
                const auto it = indexOf.find(s.substr(1));
                if (it == indexOf.end()) return Fail(error, "'" + names[t] + "' refers to " + s + ", which no rule targets");
                deps[t].push_back(it->second);
                return true;
            };
            for (const MapSource& s : r->src)
                if (!ref(s.name)) return false;
            if (!ref(r->suppressBy)) return false;
        }
    // Depth-first, visiting targets in first-appearance order: every @ref comes before the target
    // that reads it, and the same mapping always compiles to the same order.
    std::vector<int> state(n, 0); // 0 new, 1 on the stack, 2 done
    std::vector<size_t> order;
    std::function<bool(size_t)> visit = [&](size_t t) -> bool {
        if (state[t] == 2) return true;
        if (state[t] == 1) return Fail(error, "the rules refer to each other in a cycle through '" + names[t] + "'");
        state[t] = 1;
        for (size_t d : deps[t])
            if (!visit(d)) return false;
        state[t] = 2;
        order.push_back(t);
        return true;
    };
    for (size_t t = 0; t < n; ++t)
        if (!visit(t)) return false;
    std::vector<int> nodeOf(n, -1);
    for (size_t i = 0; i < order.size(); ++i) nodeOf[order[i]] = static_cast<int>(i);

    std::set<std::string> warned;
    auto warn = [&](const std::string& w) {
        if (warned.insert(w).second) out.warnings.push_back(w);
    };
    auto input = [&](const std::string& s, float weight) -> CompiledFaceMap::Input {
        CompiledFaceMap::Input in;
        in.weight = weight;
        if (!s.empty() && s[0] == '@') in.node = nodeOf[indexOf.at(s.substr(1))];
        else
        {
            in.channel = channels.Find(s);
            if (in.channel < 0) warn("channel '" + s + "' is neither ARKit nor one of the source's extras (reads 0)");
        }
        return in;
    };
    for (size_t t : order)
    {
        CompiledFaceMap::Target target;
        target.name = names[t];
        for (const MapRule* r : rulesOf[t])
        {
            CompiledFaceMap::Rule cr;
            cr.op = r->op;
            for (const MapSource& s : r->src) cr.in.push_back(input(s.name, s.weight));
            if (!r->suppressBy.empty())
            {
                cr.suppress = input(r->suppressBy, 1.0f);
                cr.hasSuppress = true;
            }
            cr.gain = r->gain;
            cr.offset = r->offset;
            cr.gamma = r->gamma;
            cr.minV = r->minV;
            cr.maxV = r->maxV;
            cr.suppressK = r->suppressK;
            cr.curve = r->curve;
            target.rules.push_back(std::move(cr));
        }
        if (!ResolveTarget(target.name, hooks, target.out)) warn("'" + target.name + "' is not on this model");
        out.targets.push_back(std::move(target));
    }
    if (profile.hasVowels && profile.vowels.enabled)
    {
        out.vowelsEnabled = true;
        out.vowels = profile.vowels;
        for (int k = 0; k < kVowelCount; ++k)
        {
            if (profile.vowels.targets[k].empty()) continue;
            std::vector<std::pair<int, float>> slots;
            if (!ResolveTarget(profile.vowels.targets[k], hooks, slots))
            {
                warn("vowel '" + profile.vowels.targets[k] + "' is not on this model");
                continue;
            }
            for (const auto& [slot, scale] : slots)
            {
                out.vowelSlots[k].push_back(slot);
                out.vowelScales[k].push_back(scale);
            }
        }
    }
    out.extrasSeen = channels.Extras().size();
    return true;
}

void CompiledFaceMap::Evaluate(const float* values, const uint64_t present[2], Scratch& scratch,
                               std::vector<std::pair<int, float>>& slotWeights) const
{
    scratch.value.assign(targets.size(), 0.0f);
    scratch.live.assign(targets.size(), 0);
    slotWeights.clear();
    auto read = [&](const Input& in) -> float {
        if (in.channel >= 0) return Channel(values, present, in.channel);
        if (in.node >= 0) return scratch.value[static_cast<size_t>(in.node)];
        return 0.0f;
    };
    auto isLive = [&](const Input& in) -> bool {
        if (in.channel >= 0) return Present(present, in.channel);
        if (in.node >= 0) return scratch.live[static_cast<size_t>(in.node)] != 0;
        return false;
    };
    for (size_t t = 0; t < targets.size(); ++t)
    {
        const Target& target = targets[t];
        float best = 0.0f;
        bool any = false;
        for (const Rule& r : target.rules)
        {
            bool live = false;
            for (const Input& in : r.in) live = live || isLive(in);
            if (!live) continue;
            float v = 0.0f;
            switch (r.op)
            {
            case MapOp::Direct: v = read(r.in[0]); break;
            case MapOp::Max:
                v = read(r.in[0]);
                for (size_t i = 1; i < r.in.size(); ++i) v = std::max(v, read(r.in[i]));
                break;
            case MapOp::Min:
                v = read(r.in[0]);
                for (size_t i = 1; i < r.in.size(); ++i) v = std::min(v, read(r.in[i]));
                break;
            case MapOp::Avg:
                for (const Input& in : r.in) v += read(in);
                v /= static_cast<float>(r.in.size());
                break;
            case MapOp::Sum:
                for (const Input& in : r.in) v += read(in);
                v = std::min(v, 1.0f);
                break;
            case MapOp::Sub:
                v = read(r.in[0]);
                for (size_t i = 1; i < r.in.size(); ++i) v -= read(r.in[i]);
                v = std::max(v, 0.0f);
                break;
            case MapOp::Mul:
                v = 1.0f;
                for (const Input& in : r.in) v *= read(in);
                break;
            case MapOp::Weighted:
                for (const Input& in : r.in) v += in.weight * read(in);
                break;
            }
            v = v * r.gain + r.offset;
            if (!r.curve.empty()) v = ApplyCurve(r.curve, v);
            if (r.gamma > 0.0f) v = std::pow(std::max(v, 0.0f), r.gamma);
            if (r.hasSuppress) v *= 1.0f - read(r.suppress) * r.suppressK;
            v = std::clamp(v, r.minV, r.maxV);
            best = any ? std::max(best, v) : v;
            any = true;
        }
        if (!any) continue;
        scratch.value[t] = best;
        scratch.live[t] = 1;
        for (const auto& [slot, scale] : target.out) EmitMax(slotWeights, slot, best * scale);
    }
    if (vowelsEnabled)
    {
        static constexpr std::string_view kInputs[] = {"jawOpen", "mouthFunnel", "mouthPucker", "mouthStretchLeft",
                                                       "mouthStretchRight", "mouthSmileLeft", "mouthSmileRight", "mouthClose"};
        bool live = false;
        for (std::string_view name : kInputs) live = live || Present(present, FindArkitChannel(name));
        if (!live) return;
        float w[kVowelCount];
        SolveVowels(vowels, values, present, w);
        for (int k = 0; k < kVowelCount; ++k)
            for (size_t i = 0; i < vowelSlots[k].size(); ++i) EmitMax(slotWeights, vowelSlots[k][i], w[k] * vowelScales[k][i]);
    }
}

// ==== The editor's automatic mapping ====

namespace {

// One UTF-8 code point at s[i] (advancing i); 0xFFFD for a broken sequence.
uint32_t NextCodePoint(std::string_view s, size_t& i)
{
    const auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
    const uint8_t c = b(i);
    if (c < 0x80) { ++i; return c; }
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
    if (n < 0 || i + static_cast<size_t>(n) >= s.size()) { ++i; return 0xFFFD; }
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; ++k)
    {
        if (i + static_cast<size_t>(k) >= s.size() || (b(i + static_cast<size_t>(k)) & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (b(i + static_cast<size_t>(k)) & 0x3F);
    }
    i += static_cast<size_t>(n) + 1;
    return cp;
}

void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Half-width katakana U+FF66..U+FF9D as their full-width letters (U+30xx); 0 = not one.
uint32_t FullWidthKatakana(uint32_t cp)
{
    static const uint16_t kTable[] = {
        0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, // FF66-FF6F
        0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, 0x30B1, // FF70-FF79
        0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1, 0x30C4, 0x30C6, // FF7A-FF83
        0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, // FF84-FF8D
        0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, // FF8E-FF97
        0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3,                                 // FF98-FF9D
    };
    if (cp >= 0xFF66 && cp <= 0xFF9D) return kTable[cp - 0xFF66];
    return 0;
}

// A full-width katakana letter with a (han)dakuten after it: its voiced form; 0 = none.
uint32_t Voiced(uint32_t base, bool handakuten)
{
    if (handakuten)
        return (base >= 0x30CF && base <= 0x30DB && (base - 0x30CF) % 3 == 0) ? base + 2 : 0; // ha row -> pa row (handakuten)
    if ((base >= 0x30AB && base <= 0x30C1 && (base - 0x30AB) % 2 == 0) || base == 0x30C4 || base == 0x30C6 || base == 0x30C8)
        return base + 1;                                                                      // ka / sa / ta rows -> ga / za / da rows
    if (base >= 0x30CF && base <= 0x30DB && (base - 0x30CF) % 3 == 0) return base + 1;        // ha row -> ba row
    if (base == 0x30A6) return 0x30F4;                                                        // katakana u -> vu
    return 0;
}

bool EndsWithNoCase(std::string_view s, std::string_view tail)
{
    if (s.size() < tail.size()) return false;
    for (size_t i = 0; i < tail.size(); ++i)
    {
        char a = s[s.size() - tail.size() + i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != tail[i]) return false;
    }
    return true;
}

double Short(float v) { return std::round(static_cast<double>(v) * 1e6) / 1e6; }

const char* OpName(MapOp op)
{
    switch (op)
    {
    case MapOp::Direct: return "direct";
    case MapOp::Max: return "max";
    case MapOp::Min: return "min";
    case MapOp::Avg: return "avg";
    case MapOp::Sum: return "sum";
    case MapOp::Sub: return "sub";
    case MapOp::Mul: return "mul";
    case MapOp::Weighted: return "weighted";
    }
    return "direct";
}

} // namespace

std::string MorphNameKey(std::string_view name)
{
    // A "blendShape1." / "BS." prefix: everything up to the last '.', when more than a side
    // letter follows it ("Eye.L" keeps its ".L").
    if (const size_t dot = name.rfind('.'); dot != std::string_view::npos && name.size() - dot - 1 > 2)
        name = name.substr(dot + 1);
    // The side marker, moved to the end.
    const char* side = "";
    static const std::pair<const char*, const char*> kSides[] = {
        {"_left", "left"}, {".left", "left"}, {"-left", "left"}, {"left", "left"},
        {"_right", "right"}, {".right", "right"}, {"-right", "right"}, {"right", "right"},
        {"_l", "left"}, {".l", "left"}, {"-l", "left"}, {" l", "left"},
        {"_r", "right"}, {".r", "right"}, {"-r", "right"}, {" r", "right"},
    };
    for (const auto& [tail, which] : kSides)
        if (name.size() > std::strlen(tail) && EndsWithNoCase(name, tail))
        {
            name = name.substr(0, name.size() - std::strlen(tail));
            side = which;
            break;
        }
    std::string out;
    out.reserve(name.size() + 5);
    for (size_t i = 0; i < name.size();)
    {
        uint32_t cp = NextCodePoint(name, i);
        if (cp == '_' || cp == '-' || cp == '.' || cp == ' ') continue;
        if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
        if (const uint32_t full = FullWidthKatakana(cp))
        {
            cp = full;
            // A half-width (han)dakuten right after it voices it.
            size_t j = i;
            if (j < name.size())
            {
                const uint32_t mark = NextCodePoint(name, j);
                if (mark == 0xFF9E || mark == 0xFF9F)
                    if (const uint32_t v = Voiced(cp, mark == 0xFF9F))
                    {
                        cp = v;
                        i = j;
                    }
            }
        }
        AppendUtf8(out, cp);
    }
    out += side;
    return out;
}

AutoMapResult AutoMapMorphs(const std::vector<std::string>& morphs, const FaceMapProfile& base, const std::string& baseName)
{
    AutoMapResult r;
    r.profile.name = "auto (" + (baseName.empty() ? std::string("no base") : baseName) + ")";
    r.profile.basePreset = baseName;
    std::set<std::string> exact(morphs.begin(), morphs.end());
    std::map<std::string, std::string> byKey; // the first morph of each key
    for (const std::string& m : morphs) byKey.emplace(MorphNameKey(m), m);

    std::set<std::string> driven;
    // 1. Base rules the model has under another spelling: copied under its name.
    for (const MapRule& rule : base.morphs)
    {
        if (rule.target.rfind("preset:", 0) == 0) continue;
        if (exact.count(rule.target)) { driven.insert(rule.target); continue; }
        const auto it = byKey.find(MorphNameKey(rule.target));
        if (it == byKey.end() || driven.count(it->second)) continue;
        MapRule copy = rule;
        copy.target = it->second;
        r.profile.morphs.push_back(copy);
        r.renamed.emplace_back(rule.target, it->second);
        driven.insert(it->second);
    }
    if (base.hasVowels && base.vowels.enabled)
        for (const std::string& t : base.vowels.targets)
            if (exact.count(t)) driven.insert(t);
    // 2. ARKit 1:1 for the morphs the base leaves alone.
    for (int ch = 0; ch < kArkitChannelCount; ++ch)
    {
        const std::string channel(ArkitChannelName(ch));
        const auto it = byKey.find(MorphNameKey(channel));
        if (it == byKey.end() || driven.count(it->second)) continue;
        MapRule d;
        d.target = it->second;
        d.src.push_back({channel, 1.0f});
        r.profile.morphs.push_back(d);
        r.matches.emplace_back(channel, it->second);
        driven.insert(it->second);
    }

    // 3. Coverage of the base and the new rules together: a channel counts when a rule whose
    // target the model has reads it (through @refs too), or the vowels are on with a target.
    const FaceMapProfile merged = MergeFaceMaps(base, r.profile);
    std::map<std::string, const MapRule*> ruleOf;
    for (const MapRule& rule : merged.morphs) ruleOf[rule.target] = &rule;
    std::set<std::string> read;
    std::set<std::string> visiting;
    std::function<void(const MapRule&)> collect = [&](const MapRule& rule) {
        for (const MapSource& s : rule.src)
        {
            if (!s.name.empty() && s.name[0] == '@')
            {
                const std::string ref = s.name.substr(1);
                const auto it = ruleOf.find(ref);
                if (it != ruleOf.end() && visiting.insert(ref).second) collect(*it->second);
            }
            else
                read.insert(s.name);
        }
    };
    for (const MapRule& rule : merged.morphs)
        if (exact.count(rule.target))
        {
            visiting.clear();
            collect(rule);
        }
    bool vowelsLive = false;
    if (merged.hasVowels && merged.vowels.enabled)
        for (const std::string& t : merged.vowels.targets) vowelsLive = vowelsLive || exact.count(t) != 0;
    if (vowelsLive)
        for (const char* c : {"jawOpen", "mouthFunnel", "mouthPucker", "mouthStretchLeft", "mouthStretchRight", "mouthSmileLeft",
                              "mouthSmileRight", "mouthClose"})
            read.insert(c);
    for (int ch = 0; ch < kArkitChannelCount; ++ch)
        if (!read.count(std::string(ArkitChannelName(ch)))) r.unmappedChannels.emplace_back(ArkitChannelName(ch));
    std::set<std::string> written;
    for (const MapRule& rule : merged.morphs)
        if (exact.count(rule.target)) written.insert(rule.target);
    if (vowelsLive)
        for (const std::string& t : merged.vowels.targets)
            if (exact.count(t)) written.insert(t);
    for (const std::string& m : morphs)
        if (!written.count(m)) r.undrivenMorphs.push_back(m);
    return r;
}

std::string FaceMapToJson(const FaceMapProfile& p)
{
    Json j = Json::object();
    j["version"] = 1;
    if (!p.name.empty()) j["name"] = p.name;
    if (!p.basePreset.empty()) j["basePreset"] = p.basePreset;
    Json rules = Json::array();
    for (const MapRule& r : p.morphs)
    {
        Json o = Json::object();
        o["target"] = r.target;
        if (r.op != MapOp::Direct) o["op"] = OpName(r.op);
        if (r.src.size() == 1 && r.op != MapOp::Weighted) o["src"] = r.src.front().name;
        else
        {
            Json src = Json::array();
            for (const MapSource& s : r.src)
            {
                if (r.op == MapOp::Weighted) src.push_back({{"name", s.name}, {"weight", Short(s.weight)}});
                else src.push_back(s.name);
            }
            o["src"] = src;
        }
        if (r.gain != 1.0f) o["gain"] = Short(r.gain);
        if (r.offset != 0.0f) o["offset"] = Short(r.offset);
        if (!r.curve.empty())
        {
            Json c = Json::array();
            for (const auto& [x, y] : r.curve) c.push_back({Short(x), Short(y)});
            o["curve"] = c;
        }
        if (r.gamma > 0.0f) o["gamma"] = Short(r.gamma);
        if (r.minV != 0.0f) o["min"] = Short(r.minV);
        if (r.maxV != 1.0f) o["max"] = Short(r.maxV);
        if (!r.suppressBy.empty())
        {
            o["suppressBy"] = r.suppressBy;
            if (r.suppressK != 1.0f) o["suppressK"] = Short(r.suppressK);
        }
        rules.push_back(o);
    }
    j["morphs"] = rules;
    if (p.hasVowels)
    {
        const char* keys[kVowelCount] = {"a", "i", "u", "e", "o"};
        Json v = Json::object();
        v["enabled"] = p.vowels.enabled;
        Json t = Json::object();
        for (int k = 0; k < kVowelCount; ++k) t[keys[k]] = p.vowels.targets[k];
        v["targets"] = t;
        Json m = Json::array(), b = Json::array();
        for (int k = 0; k < kVowelCount; ++k)
        {
            Json row = Json::array();
            for (int c = 0; c < kVowelInputs; ++c) row.push_back(Short(p.vowels.matrix[k][c]));
            m.push_back(row);
            b.push_back(Short(p.vowels.bias[k]));
        }
        v["model"] = {{"matrix", m}, {"bias", b}};
        j["vowels"] = v;
    }
    if (p.hasBones)
    {
        const BoneMap& bm = p.bones;
        Json bones = Json::object();
        Json chain = Json::array();
        for (const ChainBone& c : bm.chain)
        {
            Json o = Json::object();
            if (!c.role.empty()) o["role"] = c.role;
            if (!c.bone.empty()) o["bone"] = c.bone;
            if (!c.fallbackRole.empty()) o["fallbackRole"] = c.fallbackRole;
            o["weight"] = Short(c.weight);
            chain.push_back(o);
        }
        bones["chain"] = chain;
        bones["gain"] = {{"yaw", Short(bm.gainYaw)}, {"pitch", Short(bm.gainPitch)}, {"roll", Short(bm.gainRoll)}};
        bones["limitsDeg"] = {{"yaw", Short(bm.limitYawDeg)}, {"pitch", Short(bm.limitPitchDeg)}, {"roll", Short(bm.limitRollDeg)}};
        if (bm.basisAuto) bones["faceBasis"] = "auto";
        else bones["faceBasis"] = {{"forward", bm.forward}, {"up", bm.up}};
        Json eyes = Json::object();
        eyes["mode"] = bm.eyes == EyesMode::Bones ? "bones" : (bm.eyes == EyesMode::Morphs ? "morphs" : "none");
        if (!bm.leftEye.empty()) eyes["left"] = bm.leftEye;
        if (!bm.rightEye.empty()) eyes["right"] = bm.rightEye;
        eyes["maxYawDeg"] = Short(bm.eyeMaxYawDeg);
        eyes["maxPitchDeg"] = Short(bm.eyeMaxPitchDeg);
        bones["eyes"] = eyes;
        Json jaw = Json::object();
        jaw["mode"] = bm.jaw == JawMode::Bone ? "bone" : "none";
        if (!bm.jawBone.empty()) jaw["bone"] = bm.jawBone;
        jaw["maxDeg"] = Short(bm.jawMaxDeg);
        bones["jaw"] = jaw;
        j["bones"] = bones;
    }
    if (p.hasFilters) j["filters"] = {{"preset", p.filterPreset}};
    return j.dump(2, ' ', /*ensure_ascii=*/false) + "\n";
}

} // namespace FaceTracking
