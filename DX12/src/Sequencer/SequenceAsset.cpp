#include "Sequencer/SequenceAsset.h"

#include "Resource/AssetFS.h"   // pak-first read so .iseq loads in packed builds
#include "System/Log.h"
#include "System/PathUtil.h"    // Sys::OpenOfstream / CreateParentDirs (UTF-8, loose-disk write)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Seq
{

// ==== Small helpers ====

namespace
{
    // ---- number formatting ----
    // %g, as in .ivfx / ComponentSerializer. Lossy at the 7th significant digit, which the fixpoint
    // self-test relies on: the first save rounds, every later save reproduces the rounded text exactly.
    std::string F(float v)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        return buf;
    }

    std::string I(int32_t v)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d", v);
        return buf;
    }

    // ---- tokenizer ----
    // Splits on ASCII whitespace. The writer percent-encodes every free-form string, so a token never
    // contains a space and a naive split is exact.
    void Tokenize(const std::string& line, std::vector<std::string>& out)
    {
        out.clear();
        std::size_t i = 0;
        const std::size_t n = line.size();
        while (i < n)
        {
            while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i >= n) break;
            const std::size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            out.emplace_back(line, start, i - start);
        }
    }

    float ToF(const std::string& s) { return static_cast<float>(std::atof(s.c_str())); }
    int32_t ToI(const std::string& s) { return static_cast<int32_t>(std::atol(s.c_str())); }
    bool  ToB(const std::string& s) { return ToI(s) != 0; }

    // Strip a trailing \r so files authored on either line ending parse the same.
    void StripCR(std::string& s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    }

    bool IsBlank(const std::string& s)
    {
        for (unsigned char c : s) if (!std::isspace(c)) return false;
        return true;
    }

    int HexVal(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // ---- vector KV (underscore-separated, matching ComponentSerializers) ----
    std::string Vec3Str(const DirectX::XMFLOAT3& v)
    {
        return F(v.x) + "_" + F(v.y) + "_" + F(v.z);
    }
    std::string Vec4Str(const DirectX::XMFLOAT4& v)
    {
        return F(v.x) + "_" + F(v.y) + "_" + F(v.z) + "_" + F(v.w);
    }
    DirectX::XMFLOAT3 ParseVec3(const std::string& s)
    {
        DirectX::XMFLOAT3 v{ 0.f, 0.f, 0.f };
        sscanf_s(s.c_str(), "%f_%f_%f", &v.x, &v.y, &v.z);
        return v;
    }
    DirectX::XMFLOAT4 ParseVec4(const std::string& s)
    {
        DirectX::XMFLOAT4 v{ 0.f, 0.f, 0.f, 0.f };
        sscanf_s(s.c_str(), "%f_%f_%f_%f", &v.x, &v.y, &v.z, &v.w);
        return v;
    }
}

// ==== Percent-encoding ====
// Safe set is [A-Za-z0-9._/-]; everything else becomes %XX. Required because:
//   1. a role, a track name or a subtitle can contain spaces, and the grammar is
//      whitespace-tokenized;
//   2. tools/package_game.py walks BACKWARD from an asset extension to find a path's start and
//      stops at ':' (not at a space): a raw colon before a path token truncates the path and the
//      asset never gets cooked.

std::string PctEncode(const std::string& s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '.' || c == '_' || c == '-' || c == '/';
        if (safe) out.push_back(static_cast<char>(c));
        else { out.push_back('%'); out.push_back(kHex[c >> 4]); out.push_back(kHex[c & 0xF]); }
    }
    // The empty string must survive a round trip through a whitespace-
    // tokenized grammar, where it would otherwise vanish entirely.
    return out.empty() ? std::string("%00") : out;
}

std::string PctDecode(const std::string& s)
{
    if (s == "%00") return std::string();
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            const int hi = HexVal(s[i + 1]);
            const int lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// ==== Hashing ====

uint32_t HashTarget(const char* s) noexcept
{
    uint32_t h = 0x811C9DC5u;
    if (s) { while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 0x01000193u; } }
    return h ? h : 1u;   // never 0 - 0 stays usable as "unset"
}

// ==== Ease / curve evaluation ====
// Identical to Camera::ApplyBlendCurve (CameraStackComponents.h), so a sequencer camera ease and a
// gameplay VCam blend of the same curve have the same shape.

float ApplyEase(EaseCurve c, float p)
{
    if (p <= 0.f) return 0.f;
    if (p >= 1.f) return 1.f;
    switch (c)
    {
    case EaseCurve::Linear:    return p;
    case EaseCurve::EaseIn:    return p * p;
    case EaseCurve::EaseOut:   return 1.f - (1.f - p) * (1.f - p);
    case EaseCurve::EaseInOut: return p * p * (3.f - 2.f * p);   // smoothstep
    }
    return p;
}

// ==== Channel ====

void Channel::SortAndDedupe()
{
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Key& a, const Key& b) { return a.frame < b.frame; });
    // Last key at a given frame wins - matches "drop a key on top of a key".
    auto w = keys.begin();
    for (auto r = keys.begin(); r != keys.end(); ++r)
    {
        if (w != keys.begin() && (w - 1)->frame == r->frame) *(w - 1) = *r;
        else                                                 *w++     = *r;
    }
    keys.erase(w, keys.end());
}

float Channel::Evaluate(float frameF) const
{
    if (keys.empty())      return defaultValue;
    if (keys.size() == 1)  return keys.front().value;

    if (frameF <= static_cast<float>(keys.front().frame)) return keys.front().value;
    if (frameF >= static_cast<float>(keys.back().frame))  return keys.back().value;

    // First key strictly after frameF. Both guards above make this a valid
    // interior iterator, so k0/k1 are always dereferenceable.
    const auto it = std::upper_bound(
        keys.begin(), keys.end(), frameF,
        [](float f, const Key& k) { return f < static_cast<float>(k.frame); });

    const Key& k0 = *(it - 1);
    const Key& k1 = *it;

    const float span = static_cast<float>(k1.frame - k0.frame);
    if (span <= 0.f) return k1.value;
    const float t = (frameF - static_cast<float>(k0.frame)) / span;

    switch (k0.interp)
    {
    case Interp::Constant:
        return k0.value;
    case Interp::Linear:
        return k0.value + (k1.value - k0.value) * t;
    case Interp::Cubic:
    default:
    {
        // Cubic Hermite. Tangents are authored in value-units per FRAME, so
        // they scale by the segment span to become the basis' derivatives.
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 =  2.f * t3 - 3.f * t2 + 1.f;
        const float h10 =        t3 - 2.f * t2 + t;
        const float h01 = -2.f * t3 + 3.f * t2;
        const float h11 =        t3 -       t2;
        return h00 * k0.value + h10 * k0.tangentOut * span
             + h01 * k1.value + h11 * k1.tangentIn  * span;
    }
    }
}

// ==== Section ====

const Channel* Section::Find(uint16_t role) const
{
    for (const Channel& c : channels) if (c.role == role) return &c;
    return nullptr;
}

Channel* Section::Find(uint16_t role)
{
    for (Channel& c : channels) if (c.role == role) return &c;
    return nullptr;
}

float Section::ChannelValue(uint16_t role, float frameF, float fallback) const
{
    const Channel* c = Find(role);
    return c ? c->Evaluate(frameF) : fallback;
}

// PURE GEOMETRY: active * authored weight * ease envelope. Does NOT fold in the Role::Weight
// channel: a track that wants a weight curve multiplies ChannelValue(Role::Weight, ...) itself.
// As a pure function of (frameF, section geometry) the weight at a frame is identical whether
// forward playback or a backward scrub reached it.
float Section::EvaluateWeight(float frameF) const
{
    if (!active) return 0.f;
    if (frameF < static_cast<float>(startFrame)) return 0.f;
    if (frameF >= static_cast<float>(endFrame))  return 0.f;

    float w = weight;
    const float local = frameF - static_cast<float>(startFrame);
    const float dur   = static_cast<float>(endFrame - startFrame);

    if (easeInFrames > 0 && local < static_cast<float>(easeInFrames))
        w *= ApplyEase(easeInCurve, local / static_cast<float>(easeInFrames));

    if (easeOutFrames > 0)
    {
        const float fromEnd = dur - local;
        if (fromEnd < static_cast<float>(easeOutFrames))
            w *= ApplyEase(easeOutCurve, fromEnd / static_cast<float>(easeOutFrames));
    }
    return w;
}

// ==== Track metadata tables ====

namespace
{
    struct TrackMeta
    {
        const char* token;    // .iseq directive spelling
        const char* label;    // human-facing
        EvalMode    mode;
        bool        master;
    };

    // Indexed by TrackType. Order MUST match the enum.
    constexpr TrackMeta kTrackMeta[] = {
        { "CAMERACUT",    "Camera Cut",        EvalMode::RangeState, true  },
        { "SHOT",         "Shot",              EvalMode::RangeState, true  },
        { "SUBSEQUENCE",  "Sub-Sequence",      EvalMode::RangeState, true  },
        { "FADE",         "Fade",              EvalMode::Absolute,   true  },
        { "TIMEDILATION", "Time Dilation",     EvalMode::Absolute,   true  },
        { "MARKER",       "Marker",            EvalMode::OneShot,    true  },
        { "SCENELOAD",    "Scene Load",        EvalMode::OneShot,    true  },

        { "TRANSFORM",    "Transform",         EvalMode::Absolute,   false },
        { "SKELETALANIM", "Skeletal Animation",EvalMode::Absolute,   false },
        { "CAMERAMOTION", "Camera Motion",     EvalMode::Absolute,   false },
        { "DIRECTOR",     "Motion Director",   EvalMode::Absolute,   false },
        { "PROPERTY",     "Property",          EvalMode::Absolute,   false },
        { "MATERIALPARAM","Material Parameter",EvalMode::Absolute,   false },
        { "VISIBILITY",   "Visibility",        EvalMode::RangeState, false },
        { "POSTPROCESS",  "Post Process",      EvalMode::Absolute,   false },
        { "AUDIO",        "Audio",             EvalMode::OneShot,    false },
        { "VFX",          "VFX",               EvalMode::OneShot,    false },
        { "CAMERASHAKE",  "Camera Shake",      EvalMode::OneShot,    false },
        { "SUBTITLE",     "Subtitle",          EvalMode::RangeState, false },
        { "EVENT",        "Event",             EvalMode::OneShot,    false },
    };
    static_assert(sizeof(kTrackMeta) / sizeof(kTrackMeta[0]) ==
                      static_cast<std::size_t>(TrackType::COUNT),
                  "kTrackMeta must have exactly one row per TrackType");

    struct ArchMeta { const char* token; SpawnArchetype value; };
    constexpr ArchMeta kArchMeta[] = {
        { "PREFAB",       SpawnArchetype::FromPrefab        },
        { "CAMERA",       SpawnArchetype::Camera            },
        { "PPVOLUME",     SpawnArchetype::PostProcessVolume },
        { "FADEQUAD",     SpawnArchetype::FadeQuad          },
        { "SUBTITLETEXT", SpawnArchetype::SubtitleText      },
        { "AUDIOSOURCE",  SpawnArchetype::AudioSource       },
    };

    const TrackMeta& Meta(TrackType t)
    {
        const auto i = static_cast<std::size_t>(t);
        return kTrackMeta[i < static_cast<std::size_t>(TrackType::COUNT) ? i : 0];
    }
}

EvalMode    TrackEvalMode(TrackType t) { return Meta(t).mode;  }
bool        TrackIsMaster(TrackType t) { return Meta(t).master; }
const char* TrackTypeName(TrackType t) { return Meta(t).token;  }
const char* TrackTypeLabel(TrackType t){ return Meta(t).label;  }

bool TrackTypeFromName(const char* s, TrackType& out)
{
    if (!s) return false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(TrackType::COUNT); ++i)
        if (std::strcmp(s, kTrackMeta[i].token) == 0)
        {
            out = static_cast<TrackType>(i);
            return true;
        }
    return false;
}

const char* SpawnArchetypeName(SpawnArchetype a)
{
    for (const ArchMeta& m : kArchMeta) if (m.value == a) return m.token;
    return "PREFAB";
}

bool SpawnArchetypeFromName(const char* s, SpawnArchetype& out)
{
    if (!s) return false;
    for (const ArchMeta& m : kArchMeta)
        if (std::strcmp(s, m.token) == 0) { out = m.value; return true; }
    return false;
}

void Track::SortSections()
{
    std::stable_sort(sections.begin(), sections.end(),
                     [](const Section& a, const Section& b)
                     {
                         if (a.row != b.row) return a.row < b.row;
                         return a.startFrame < b.startFrame;
                     });
}

// ==== Sequence ====

bool Sequence::AnySolo() const
{
    for (const Track& t : tracks) if (t.solo) return true;
    return false;
}

bool Sequence::TrackAudible(const Track& t) const
{
    if (t.mute) return false;
    return !AnySolo() || t.solo;
}

int32_t Sequence::FindBindingByRole(const std::string& role) const
{
    for (std::size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i].role == role) return static_cast<int32_t>(i);
    return -1;
}

void Sequence::Normalize()
{
    if (frameRate <= 0.f) frameRate = 30.f;
    if (durationFrames < 0) durationFrames = 0;
    if (maxForwardJump < 1) maxForwardJump = 1;

    for (Track& t : tracks)
    {
        t.targetId = HashTarget(t.name);
        // A master track can never carry a binding, and an object track with a
        // dangling binding index would index out of bounds in the evaluator.
        if (TrackIsMaster(t.type))                                   t.binding = -1;
        else if (t.binding >= static_cast<int32_t>(bindings.size())) t.binding = -1;

        t.SortSections();
        for (Section& s : t.sections)
        {
            if (s.endFrame < s.startFrame) s.endFrame = s.startFrame;
            if (s.preRoll  < 0)            s.preRoll  = 0;
            for (Channel& c : s.channels) c.SortAndDedupe();
        }
    }
}

// ==== Parser ====
// Line-based state machine with four nested scopes; the innermost OPEN scope owns any line this
// reader does not recognize:
//     sequence -> BIND  -> (binding)
//              -> TRACK -> SECTION -> CH
// A channel has no unknownLines, so lines inside a CH block are retained by the enclosing
// SECTION. That keeps the writer a fixpoint: it emits a scope's retained lines at the END of its
// block, and re-parsing lands them in the same scope again.

bool ParseSequenceText(const std::string& text, Sequence& out)
{
    out = Sequence{};

    // MINVER is checked against a pre-scan so a refusal costs nothing and does
    // not leave a half-populated sequence behind.
    {
        std::istringstream pre(text);
        std::string line;
        std::vector<std::string> tok;
        while (std::getline(pre, line))
        {
            StripCR(line);
            if (IsBlank(line) || line[line.find_first_not_of(" \t")] == '#') continue;
            Tokenize(line, tok);
            if (tok.size() >= 2 && tok[0] == "MINVER")
            {
                const int32_t minver = ToI(tok[1]);
                if (minver > kFormatVersion)
                {
                    LOG_ERROR("Sequence: file requires reader version %d but this build is %d "
                              "— refusing to load rather than silently dropping its data",
                              minver, kFormatVersion);
                    return false;
                }
                break;
            }
        }
    }

    Binding* curBind    = nullptr;
    Track*   curTrack   = nullptr;
    Section* curSection = nullptr;
    Channel* curChannel = nullptr;

    // Innermost open scope's retention vector.
    auto retain = [&](const std::string& raw)
    {
        if (curSection)   curSection->unknownLines.push_back(raw);
        else if (curTrack) curTrack->unknownLines.push_back(raw);
        else if (curBind)  curBind->unknownLines.push_back(raw);
        else               out.unknownLines.push_back(raw);
    };

    std::istringstream iss(text);
    std::string line;
    std::vector<std::string> t;

    while (std::getline(iss, line))
    {
        StripCR(line);
        if (IsBlank(line)) continue;

        // Comments are RETAINED, not dropped, so saving a documented sample asset keeps its
        // documentation (the .ivfx writer drops them).
        const std::size_t firstNs = line.find_first_not_of(" \t");
        if (firstNs != std::string::npos && line[firstNs] == '#') { retain(line); continue; }

        Tokenize(line, t);
        if (t.empty()) continue;
        const std::string& d = t[0];
        const std::size_t  n = t.size();

        // ---- sequence header (valid at any depth; does not change scope) ----
        if (d == "ISEQ")    { continue; }                                    // version marker, informational
        if (d == "MINVER")  { continue; }                                    // handled by the pre-scan
        if (d == "FPS")     { if (n >= 2) out.frameRate      = ToF(t[1]); continue; }
        if (d == "DUR")     { if (n >= 2) out.durationFrames = ToI(t[1]); continue; }
        if (d == "LOOP")    { if (n >= 2) out.looping        = ToB(t[1]); continue; }
        if (d == "MAXJUMP") { if (n >= 2) out.maxForwardJump = ToI(t[1]); continue; }
        if (d == "RESTORE") { if (n >= 2) out.restoreMode = static_cast<RestoreMode>(ToI(t[1])); continue; }
        if (d == "CLOCK")   { if (n >= 2) out.clockSource = static_cast<ClockSource>(ToI(t[1])); continue; }
        if (d == "LAYER")   { if (n >= 2) out.layer          = ToI(t[1]); continue; }

        // ---- scope openers ----
        if (d == "BIND")
        {
            out.bindings.emplace_back();
            curBind    = &out.bindings.back();
            curTrack   = nullptr;
            curSection = nullptr;
            curChannel = nullptr;

            if (n >= 2 && t[1] == "SPAWN")
            {
                curBind->kind = BindingKind::Spawnable;
                if (n >= 3) SpawnArchetypeFromName(t[2].c_str(), curBind->archetype);
            }
            else
            {
                curBind->kind = BindingKind::Possessable;
            }
            continue;
        }

        if (d == "TRACK")
        {
            out.tracks.emplace_back();
            curTrack   = &out.tracks.back();
            curBind    = nullptr;
            curSection = nullptr;
            curChannel = nullptr;

            if (n >= 2) TrackTypeFromName(t[1].c_str(), curTrack->type);
            if (n >= 3) curTrack->binding = ToI(t[2]);
            continue;
        }

        if (d == "SECTION")
        {
            if (!curTrack) { retain(line); continue; }
            curTrack->sections.emplace_back();
            curSection = &curTrack->sections.back();
            curChannel = nullptr;
            if (n >= 2) curSection->startFrame = ToI(t[1]);
            if (n >= 3) curSection->endFrame   = ToI(t[2]);
            continue;
        }

        if (d == "CH")
        {
            if (!curSection) { retain(line); continue; }
            curSection->channels.emplace_back();
            curChannel = &curSection->channels.back();
            if (n >= 2) curChannel->role         = static_cast<uint16_t>(ToI(t[1]));
            if (n >= 3) curChannel->defaultValue = ToF(t[2]);
            continue;
        }

        // ---- binding body ----
        if (curBind && !curTrack)
        {
            if (d == "ROLE")   { if (n >= 2) curBind->role        = PctDecode(t[1]); continue; }
            if (d == "GUID")   { if (n >= 2) curBind->defaultGuid = ECS::Guid::FromHex(t[1]); continue; }
            if (d == "CHILD")  { if (n >= 2) curBind->childPath   = PctDecode(t[1]); continue; }
            if (d == "PREFAB") { if (n >= 2) curBind->prefabPath  = PctDecode(t[1]); continue; }
            if (d == "SP")     { if (n >= 4) curBind->spawnPos   = { ToF(t[1]), ToF(t[2]), ToF(t[3]) }; continue; }
            if (d == "SS")     { if (n >= 4) curBind->spawnScale = { ToF(t[1]), ToF(t[2]), ToF(t[3]) }; continue; }
            if (d == "SR")     { if (n >= 5) curBind->spawnRot   = { ToF(t[1]), ToF(t[2]), ToF(t[3]), ToF(t[4]) }; continue; }
        }

        // ---- track body ----
        if (curTrack && !curSection)
        {
            if (d == "NAME") { if (n >= 2) curTrack->name   = PctDecode(t[1]); continue; }
            if (d == "MUTE") { if (n >= 2) curTrack->mute   = ToB(t[1]); continue; }
            if (d == "SOLO") { if (n >= 2) curTrack->solo   = ToB(t[1]); continue; }
            if (d == "LOCK") { if (n >= 2) curTrack->locked = ToB(t[1]); continue; }
        }

        // ---- section body ----
        if (curSection)
        {
            if (d == "K")
            {
                if (!curChannel) { retain(line); continue; }
                Key k;
                if (n >= 2) k.frame      = ToI(t[1]);
                if (n >= 3) k.value      = ToF(t[2]);
                if (n >= 4) k.tangentIn  = ToF(t[3]);
                if (n >= 5) k.tangentOut = ToF(t[4]);
                if (n >= 6) k.interp     = static_cast<Interp>(ToI(t[5]));
                curChannel->keys.push_back(k);
                continue;
            }
            if (d == "ROW")     { if (n >= 2) curSection->row          = ToI(t[1]); continue; }
            if (d == "WEIGHT")  { if (n >= 2) curSection->weight       = ToF(t[1]); continue; }
            if (d == "PREROLL") { if (n >= 2) curSection->preRoll      = ToI(t[1]); continue; }
            if (d == "ACTIVE")  { if (n >= 2) curSection->active       = ToB(t[1]); continue; }
            if (d == "EASE")
            {
                if (n >= 3) { curSection->easeInFrames = ToI(t[1]); curSection->easeOutFrames = ToI(t[2]); }
                if (n >= 5)
                {
                    curSection->easeInCurve  = static_cast<EaseCurve>(ToI(t[3]));
                    curSection->easeOutCurve = static_cast<EaseCurve>(ToI(t[4]));
                }
                continue;
            }
            if (d == "PARAM" && n >= 4)
            {
                const std::string& ty  = t[1];
                const std::string  key = PctDecode(t[2]);
                if      (ty == "i")  curSection->params.values[key] = ToI(t[3]);
                else if (ty == "f")  curSection->params.values[key] = ToF(t[3]);
                else if (ty == "s")  curSection->params.values[key] = PctDecode(t[3]);
                else if (ty == "v3") curSection->params.values[key] = ParseVec3(t[3]);
                else if (ty == "v4") curSection->params.values[key] = ParseVec4(t[3]);
                else { retain(line); }
                continue;
            }
        }

        // ---- not understood: keep it verbatim in the innermost open scope ----
        retain(line);
    }

    out.Normalize();
    return true;
}

// ==== Writer - the exact inverse of the parser ====
// Only non-default values are emitted. That keeps the text readable and stays invertible: the
// parser default-constructs every scope before applying directives, so an omitted key parses back
// to the same value.

std::string BuildSequenceText(const Sequence& seq)
{
    std::string o;
    o.reserve(4096);

    auto line = [&](const std::string& s) { o += s; o += '\n'; };

    line("ISEQ " + I(kFormatVersion));
    line("MINVER " + I(kFormatVersion));
    line("FPS " + F(seq.frameRate));
    line("DUR " + I(seq.durationFrames));
    if (seq.looping)            line("LOOP 1");
    if (seq.maxForwardJump != 15) line("MAXJUMP " + I(seq.maxForwardJump));
    if (seq.restoreMode != RestoreMode::EditorOnly)
        line("RESTORE " + I(static_cast<int32_t>(seq.restoreMode)));
    if (seq.clockSource != ClockSource::Unscaled)
        line("CLOCK " + I(static_cast<int32_t>(seq.clockSource)));
    if (seq.layer != 0)         line("LAYER " + I(seq.layer));

    for (const std::string& u : seq.unknownLines) line(u);

    // ---- bindings ----
    for (const Binding& b : seq.bindings)
    {
        line("");
        if (b.IsSpawnable()) line(std::string("BIND SPAWN ") + SpawnArchetypeName(b.archetype));
        else                 line("BIND POSSESS");

        line("ROLE " + PctEncode(b.role));

        if (!b.IsSpawnable())
        {
            if (b.defaultGuid.IsValid()) line("GUID " + b.defaultGuid.ToHex());
            if (!b.childPath.empty())    line("CHILD " + PctEncode(b.childPath));
        }
        else
        {
            if (b.archetype == SpawnArchetype::FromPrefab && !b.prefabPath.empty())
                line("PREFAB " + PctEncode(b.prefabPath));
            line("SP " + F(b.spawnPos.x)   + " " + F(b.spawnPos.y)   + " " + F(b.spawnPos.z));
            line("SR " + F(b.spawnRot.x)   + " " + F(b.spawnRot.y)   + " " + F(b.spawnRot.z)
                       + " " + F(b.spawnRot.w));
            line("SS " + F(b.spawnScale.x) + " " + F(b.spawnScale.y) + " " + F(b.spawnScale.z));
        }

        for (const std::string& u : b.unknownLines) line(u);
    }

    // ---- tracks ----
    for (const Track& tr : seq.tracks)
    {
        line("");
        line(std::string("TRACK ") + TrackTypeName(tr.type) + " " + I(tr.binding));
        if (!tr.name.empty()) line("NAME " + PctEncode(tr.name));
        if (tr.mute)   line("MUTE 1");
        if (tr.solo)   line("SOLO 1");
        if (tr.locked) line("LOCK 1");

        for (const Section& s : tr.sections)
        {
            line("SECTION " + I(s.startFrame) + " " + I(s.endFrame));
            if (s.row     != 0)   line("ROW "     + I(s.row));
            if (s.weight  != 1.f) line("WEIGHT "  + F(s.weight));
            if (s.preRoll != 0)   line("PREROLL " + I(s.preRoll));
            if (!s.active)        line("ACTIVE 0");
            if (s.easeInFrames  != 0 || s.easeOutFrames != 0 ||
                s.easeInCurve  != EaseCurve::EaseOut ||
                s.easeOutCurve != EaseCurve::EaseIn)
            {
                line("EASE " + I(s.easeInFrames) + " " + I(s.easeOutFrames) + " " +
                     I(static_cast<int32_t>(s.easeInCurve)) + " " +
                     I(static_cast<int32_t>(s.easeOutCurve)));
            }

            // PropertyBag is an unordered_map, so key order is not stable
            // across runs or across std library versions. Sort before emitting
            // or the fixpoint test becomes a coin flip.
            {
                std::vector<const std::string*> keys;
                keys.reserve(s.params.values.size());
                for (const auto& kv : s.params.values) keys.push_back(&kv.first);
                std::sort(keys.begin(), keys.end(),
                          [](const std::string* a, const std::string* b) { return *a < *b; });

                for (const std::string* k : keys)
                {
                    const auto& v = s.params.values.at(*k);
                    const std::string ek = PctEncode(*k);
                    if      (const auto* p = std::get_if<int>(&v))
                        line("PARAM i "  + ek + " " + I(*p));
                    else if (const auto* p = std::get_if<float>(&v))
                        line("PARAM f "  + ek + " " + F(*p));
                    else if (const auto* p = std::get_if<std::string>(&v))
                        line("PARAM s "  + ek + " " + PctEncode(*p));
                    else if (const auto* p = std::get_if<DirectX::XMFLOAT3>(&v))
                        line("PARAM v3 " + ek + " " + Vec3Str(*p));
                    else if (const auto* p = std::get_if<DirectX::XMFLOAT4>(&v))
                        line("PARAM v4 " + ek + " " + Vec4Str(*p));
                }
            }

            for (const Channel& c : s.channels)
            {
                line("CH " + I(static_cast<int32_t>(c.role)) + " " + F(c.defaultValue));
                for (const Key& k : c.keys)
                {
                    line("K " + I(k.frame) + " " + F(k.value) + " " +
                         F(k.tangentIn) + " " + F(k.tangentOut) + " " +
                         I(static_cast<int32_t>(k.interp)));
                }
            }

            for (const std::string& u : s.unknownLines) line(u);
        }

        // Emitted after the last section's retained lines. On re-parse they land in that section's
        // retention vector at exactly this position in the text, so the fixpoint holds even though the
        // owning scope moved.
        for (const std::string& u : tr.unknownLines) line(u);
    }

    return o;
}

// ==== I/O ====

bool LoadSequence(const std::string& path, Sequence& out)
{
    out = Sequence{};

    // AssetFS, not std::ifstream: in a packed build the .iseq lives only
    // inside game.ipak, and a raw file read there silently yields "no
    // sequence" rather than an error anyone notices.
    std::string text;
    if (!Resource::AssetFS::Get().ReadFileText(path, text))
    {
        LOG_WARNING("Sequence: cannot open '%s'", path.c_str());
        return false;
    }
    if (!ParseSequenceText(text, out))
    {
        LOG_ERROR("Sequence: '%s' refused by MINVER", path.c_str());
        return false;
    }
    if (out.tracks.empty())
        LOG_WARNING("Sequence: '%s' parsed but contains no TRACK blocks", path.c_str());
    return true;
}

bool SaveSequence(const std::string& path, const Sequence& seq)
{
    Sys::CreateParentDirs(path);
    std::ofstream f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f) { LOG_ERROR("Sequence: cannot write '%s'", path.c_str()); return false; }
    const std::string text = BuildSequenceText(seq);
    f.write(text.c_str(), static_cast<std::streamsize>(text.size()));
    LOG_SUCCESS("Sequence: saved '%s' (%zu bindings, %zu tracks, %zu bytes)",
                path.c_str(), seq.bindings.size(), seq.tracks.size(), text.size());
    return true;
}

// ==== Self-test ====

namespace
{
    std::size_t CountKeys(const Sequence& s)
    {
        std::size_t n = 0;
        for (const Track& t : s.tracks)
            for (const Section& sec : t.sections)
                for (const Channel& c : sec.channels)
                    n += c.keys.size();
        return n;
    }

    // text(parse(x)) == text(parse(text(parse(x)))): no silent key loss.
    bool Fixpoint(const char* label, const std::string& text)
    {
        Sequence s1;
        if (!ParseSequenceText(text, s1))
        {
            LOG_ERROR("Sequence self-test: '%s' PARSE REFUSED", label);
            return false;
        }
        const std::string a = BuildSequenceText(s1);

        Sequence s2;
        ParseSequenceText(a, s2);
        const std::string b = BuildSequenceText(s2);

        const bool ok = (a == b)
                     && s1.bindings.size() == s2.bindings.size()
                     && s1.tracks.size()   == s2.tracks.size()
                     && CountKeys(s1)      == CountKeys(s2);
        if (ok)
        {
            LOG_SUCCESS("Sequence self-test: '%s' round-trips (%zu bindings, %zu tracks, %zu keys)",
                        label, s1.bindings.size(), s1.tracks.size(), CountKeys(s1));
        }
        else
        {
            LOG_ERROR("Sequence self-test: '%s' FIXPOINT FAIL "
                      "(bindings %zu->%zu, tracks %zu->%zu, keys %zu->%zu, text %zu vs %zu bytes)",
                      label, s1.bindings.size(), s2.bindings.size(),
                      s1.tracks.size(), s2.tracks.size(),
                      CountKeys(s1), CountKeys(s2), a.size(), b.size());
        }
        return ok;
    }
}

bool SequenceSelfTest()
{
    bool allOk = true;

    // ---- 1. the shipped samples ----
    const char* files[] = {
        "asset/Content/Sequence/sample_shot.iseq",
        "asset/Content/Sequence/sample_multitrack.iseq",
    };
    for (const char* path : files)
    {
        std::string text;
        if (!Resource::AssetFS::Get().ReadFileText(path, text))
        {
            LOG_WARNING("Sequence self-test: skip '%s' (not found)", path);
            continue;
        }
        allOk &= Fixpoint(path, text);
    }

    // ---- 2. forward compatibility ----
    // A v-next file with directives this reader has never seen. The test is not "does it parse" but
    // "does an unknown line SURVIVE a full save cycle, in every scope": an older editor saving a newer
    // file must not lose its unknown tracks.
    {
        const std::string vNext =
            "ISEQ 1\n"
            "MINVER 1\n"
            "FPS 30\n"
            "DUR 120\n"
            "# a hand-authored comment that must survive Save\n"
            "FUTURE_GLOBAL 7\n"
            "\n"
            "BIND POSSESS\n"
            "ROLE Hero\n"
            "FUTURE_BINDING alpha\n"
            "\n"
            "TRACK TRANSFORM 0\n"
            "NAME Hero%20Motion\n"
            "FUTURE_TRACK beta\n"
            "SECTION 0 60\n"
            "FUTURE_SECTION gamma\n"
            "CH 0 0\n"
            "K 0 1 0 0 2\n"
            "K 60 5 0 0 2\n";

        Sequence s;
        ParseSequenceText(vNext, s);
        const std::string saved = BuildSequenceText(s);

        const char* mustSurvive[] = {
            "# a hand-authored comment that must survive Save",
            "FUTURE_GLOBAL 7",
            "FUTURE_BINDING alpha",
            "FUTURE_TRACK beta",
            "FUTURE_SECTION gamma",
        };
        bool preserved = true;
        for (const char* needle : mustSurvive)
        {
            if (saved.find(needle) == std::string::npos)
            {
                LOG_ERROR("Sequence self-test: forward-compat LOST '%s' across a save cycle", needle);
                preserved = false;
            }
        }
        if (preserved) LOG_SUCCESS("Sequence self-test: forward-compat retains all 5 unknown lines");
        allOk &= preserved;
        allOk &= Fixpoint("<forward-compat synthetic>", vNext);
    }

    // ---- 3. MINVER refusal ----
    {
        Sequence s;
        const bool refused = !ParseSequenceText("ISEQ 99\nMINVER 99\nFPS 30\nDUR 10\n", s);
        if (refused) LOG_SUCCESS("Sequence self-test: MINVER refusal works");
        else         LOG_ERROR("Sequence self-test: MINVER 99 was ACCEPTED — data loss is possible");
        allOk &= refused;
    }

    // ---- 4. curve evaluation ----
    // Guards what the text fixpoint cannot: that Evaluate is a pure function of frame, which the
    // scrubbing contract rests on.
    {
        Channel c;
        c.role = Role::LocX;
        c.keys = {
            { 0,  0.f, 0.f, 0.f, Interp::Linear   },
            { 10, 10.f,0.f, 0.f, Interp::Constant },
            { 20, 30.f,0.f, 0.f, Interp::Cubic    },
            { 30, 30.f,0.f, 0.f, Interp::Cubic    },
        };
        struct Case { float f; float want; const char* what; };
        const Case cases[] = {
            { -5.f,  0.f, "clamp before first key" },
            {  0.f,  0.f, "at first key"           },
            {  5.f,  5.f, "linear midpoint"        },
            { 10.f, 10.f, "at second key"          },
            { 15.f, 10.f, "constant holds"         },
            { 20.f, 30.f, "at third key"           },
            { 25.f, 30.f, "flat cubic stays flat"  },
            { 99.f, 30.f, "clamp after last key"   },
        };
        bool curveOk = true;
        for (const Case& cs : cases)
        {
            const float got = c.Evaluate(cs.f);
            if (std::fabs(got - cs.want) > 1e-4f)
            {
                LOG_ERROR("Sequence self-test: curve '%s' frame %g -> %g, expected %g",
                          cs.what, static_cast<double>(cs.f),
                          static_cast<double>(got), static_cast<double>(cs.want));
                curveOk = false;
            }
        }

        // Ease envelope: symmetric in and out, zero outside the range.
        Section s;
        s.startFrame = 0; s.endFrame = 100;
        s.easeInFrames = 10; s.easeOutFrames = 10;
        s.easeInCurve = EaseCurve::Linear; s.easeOutCurve = EaseCurve::Linear;
        struct WCase { float f; float want; const char* what; };
        const WCase wcases[] = {
            { -1.f, 0.0f, "before start"     },
            {  0.f, 0.0f, "at start"         },
            {  5.f, 0.5f, "mid ease-in"      },
            { 50.f, 1.0f, "plateau"          },
            { 95.f, 0.5f, "mid ease-out"     },
            {100.f, 0.0f, "at end, exclusive"},
        };
        for (const WCase& cs : wcases)
        {
            const float got = s.EvaluateWeight(cs.f);
            if (std::fabs(got - cs.want) > 1e-4f)
            {
                LOG_ERROR("Sequence self-test: ease '%s' frame %g -> %g, expected %g",
                          cs.what, static_cast<double>(cs.f),
                          static_cast<double>(got), static_cast<double>(cs.want));
                curveOk = false;
            }
        }
        if (curveOk) LOG_SUCCESS("Sequence self-test: curve + ease evaluation matches expectations");
        allOk &= curveOk;
    }

    // ---- 5. percent-encoding round trip ----
    {
        const char* samples[] = {
            "", "Hero", "Camera Cuts", "asset/FX/hit.ivfx",
            "Chapter 1: The Fall", "%", "  leading and trailing  ",
            "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97\xE5\xB9\x95",   // UTF-8 must survive byte-for-byte
        };
        bool pctOk = true;
        for (const char* s : samples)
        {
            const std::string enc = PctEncode(s);
            if (PctDecode(enc) != std::string(s))
            {
                LOG_ERROR("Sequence self-test: pct round-trip failed for '%s' (enc '%s')", s, enc.c_str());
                pctOk = false;
            }
            if (enc.find(' ') != std::string::npos || enc.find(':') != std::string::npos)
            {
                LOG_ERROR("Sequence self-test: pct output '%s' still contains a space or colon", enc.c_str());
                pctOk = false;
            }
        }
        if (pctOk) LOG_SUCCESS("Sequence self-test: percent-encoding round-trips (incl. UTF-8)");
        allOk &= pctOk;
    }

    return allOk;
}

} // namespace Seq
