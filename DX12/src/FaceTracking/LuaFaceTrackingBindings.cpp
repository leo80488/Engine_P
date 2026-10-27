#include "FaceTracking/LuaFaceTrackingBindings.h"

#include "ECS/AnimationComponents.h"   // SkeletonComponent, kInvalidAnimHandle
#include "ECS/FaceTrackingComponent.h"
#include "ECS/FaceTrackingSystem.h"    // FaceTrackingSystem::BakeTakeFile (FaceTracking.Bake)
#include "ECS/FacialAnimationSystem.h" // FacialAnimationComponent, Facial::FindMorphSlot
#include "ECS/FacialPresets.h"         // Facial::ResolveSkeletonCarrier
#include "FaceTracking/FaceBinding.h"
#include "FaceTracking/FaceMath.h"
#include "FaceTracking/FaceSource.h"           // NetworkSourceOptions (OpenNetwork)
#include "FaceTracking/FaceTrackingService.h"
#include "FaceTracking/ImageCapture.h"        // CameraDevice (ListCameras)
#include "FaceTracking/FaceMap.h"             // AutoMapMorphs, FaceMapToJson (FaceTracking.AutoMap)
#include "Resource/AssetFS.h"
#include "Resource/SkeletonAsset.h"
#include "Scripting/LuaDoc.h"
#include "System/PathUtil.h"                  // the .ifacemap AutoMap writes

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"     // LuaEntity + LuaId::* (MUST follow sol)

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

// ==== FaceTracking.* Lua bindings ====
// Angles cross this boundary in DEGREES, in FaceMath.h's convention: yaw > 0 turns the face
// to the subject's right, pitch > 0 looks up, roll > 0 tips the head to the right shoulder.
// Optional fields are read through sol::object and type-checked first: converting a missing
// field straight to sol::table terminates the process.
namespace FaceTracking
{
namespace
{
    constexpr float kDegToRad = FaceMath::kPi / 180.0f;
    constexpr float kRadToDeg = 180.0f / FaceMath::kPi;

    [[noreturn]] void Fail(const char* api, const char* code, const std::string& message)
    {
        throw sol::error(std::string(api) + ": " + code + ": " + message);
    }

    // NaN and the infinities are refused here: one would stay in a binding's filter state and pose
    // the character with NaN until something reset it.
    float NumberField(const sol::table& t, const char* key, float fallback, const char* api)
    {
        const sol::object o = t[key];
        if (o.get_type() == sol::type::lua_nil) return fallback;
        if (o.get_type() != sol::type::number) Fail(api, "bad_frame", std::string("'") + key + "' is not a number");
        const double v = o.as<double>();
        if (!std::isfinite(v) || std::fabs(v) > 1e30) Fail(api, "bad_frame", std::string("'") + key + "' is not a finite number");
        return static_cast<float>(v);
    }

    // A pushed frame from its Lua table; blend names resolve (or register, for extras) on `source`.
    FaceFrame FrameFromTable(Service& svc, const std::string& source, const sol::table& t, const char* api)
    {
        FaceFrame f;
        f.faceFound = 1;
        f.confidence = 1.0f;
        const sol::object found = t["found"];
        if (found.get_type() == sol::type::boolean) f.faceFound = found.as<bool>() ? 1 : 0;
        else if (found.get_type() != sol::type::lua_nil) Fail(api, "bad_frame", "'found' is not a boolean");
        f.confidence = std::clamp(NumberField(t, "conf", 1.0f, api), 0.0f, 1.0f);

        const sol::object blend = t["blend"];
        if (blend.get_type() == sol::type::table)
        {
            for (const auto& kv : blend.as<sol::table>())
            {
                if (kv.first.get_type() != sol::type::string || kv.second.get_type() != sol::type::number)
                    Fail(api, "bad_frame", "blend is a table of channel name = number");
                const std::string name = kv.first.as<std::string>();
                const double v = kv.second.as<double>();
                if (!std::isfinite(v) || std::fabs(v) > 1e30) Fail(api, "bad_frame", "blend." + name + " is not a finite number");
                const int ch = svc.FindChannel(source, name, /*addExtra=*/true);
                if (ch < 0) Fail(api, "bad_frame", "no room for another extra channel ('" + name + "')");
                f.Set(ch, static_cast<float>(v));
            }
        }
        else if (blend.get_type() != sol::type::lua_nil)
            Fail(api, "bad_frame", "'blend' is not a table");

        const sol::object head = t["head"];
        if (head.get_type() == sol::type::table)
        {
            const sol::table h = head.as<sol::table>();
            const DirectX::XMVECTOR q = FaceMath::ComposeHead(NumberField(h, "yaw", 0.0f, api) * kDegToRad,
                                                              NumberField(h, "pitch", 0.0f, api) * kDegToRad,
                                                              NumberField(h, "roll", 0.0f, api) * kDegToRad);
            DirectX::XMStoreFloat4(&f.headRot, q);
            f.flags |= kFaceHasHead;
        }
        else if (head.get_type() != sol::type::lua_nil)
            Fail(api, "bad_frame", "'head' is not a table {yaw, pitch, roll}");

        const sol::object eyes = t["eyes"];
        if (eyes.get_type() == sol::type::table)
        {
            const sol::table e = eyes.as<sol::table>();
            const char* sides[2] = {"left", "right"};
            for (int side = 0; side < 2; ++side)
            {
                const sol::object one = e[sides[side]];
                if (one.get_type() == sol::type::lua_nil) continue;
                if (one.get_type() != sol::type::table) Fail(api, "bad_frame", std::string("eyes.") + sides[side] + " is not a table {yaw, pitch}");
                const sol::table et = one.as<sol::table>();
                f.SetEye(side, NumberField(et, "yaw", 0.0f, api) * kDegToRad, NumberField(et, "pitch", 0.0f, api) * kDegToRad);
            }
        }
        else if (eyes.get_type() != sol::type::lua_nil)
            Fail(api, "bad_frame", "'eyes' is not a table {left = {yaw, pitch}, right = {yaw, pitch}}");
        return f;
    }

    sol::table StatusTable(sol::state_view& lv, const SourceStatus& s)
    {
        sol::table t = lv.create_table();
        t["name"] = s.name;
        t["kind"] = s.kind;
        t["detail"] = s.detail;
        t["epoch"] = static_cast<long long>(s.epoch);
        t["received"] = static_cast<long long>(s.received);
        t["refused"] = static_cast<long long>(s.refused);
        t["history"] = static_cast<long long>(s.historyFrames);
        t["newestSeq"] = static_cast<long long>(s.newestSeq);
        t["newestTime"] = static_cast<double>(s.newestTimeUs) / 1e6;
        t["clock"] = static_cast<double>(s.clockUs) / 1e6;
        t["live"] = s.live;
        t["state"] = s.state;
        if (s.port != 0) t["port"] = static_cast<int>(s.port);
        if (s.live || !s.lastError.empty()) t["lastError"] = s.lastError;
        t["found"] = s.faceFound;
        t["conf"] = s.confidence;
        t["finished"] = s.finished;
        t["recording"] = s.recording;
        if (s.recording)
        {
            t["recordedFrames"] = static_cast<long long>(s.recordedFrames);
            t["recordedSeconds"] = s.recordedSeconds;
        }
        return t;
    }

    sol::table FrameTable(sol::state_view& lv, const Service& svc, const std::string& source, const FaceFrame& f)
    {
        sol::table t = lv.create_table();
        t["seq"] = static_cast<long long>(f.seq);
        t["t"] = static_cast<double>(f.sourceTimeUs) / 1e6;
        t["found"] = f.faceFound != 0;
        t["conf"] = f.confidence;
        sol::table blend = lv.create_table();
        for (int ch = 0; ch < kMaxFaceChannels; ++ch)
            if (f.Has(ch)) blend[svc.ChannelName(source, ch)] = f.values[ch];
        t["blend"] = blend;
        if (f.flags & kFaceHasHead)
        {
            const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(DirectX::XMLoadFloat4(&f.headRot));
            sol::table h = lv.create_table();
            h["yaw"] = ypr.yaw * kRadToDeg;
            h["pitch"] = ypr.pitch * kRadToDeg;
            h["roll"] = ypr.roll * kRadToDeg;
            h["q"] = lv.create_table_with("x", f.headRot.x, "y", f.headRot.y, "z", f.headRot.z, "w", f.headRot.w);
            t["head"] = h;
        }
        if (f.flags & kFaceHasHeadPos)
            t["pos"] = lv.create_table_with("x", f.headPos.x, "y", f.headPos.y, "z", f.headPos.z);
        if (f.flags & kFaceHasEyes)
        {
            sol::table e = lv.create_table();
            e["left"] = lv.create_table_with("yaw", f.eyeYawPitch[0][0] * kRadToDeg, "pitch", f.eyeYawPitch[0][1] * kRadToDeg);
            e["right"] = lv.create_table_with("yaw", f.eyeYawPitch[1][0] * kRadToDeg, "pitch", f.eyeYawPitch[1][1] * kRadToDeg);
            t["eyes"] = e;
        }
        return t;
    }

    void RequireOn(const char* api)
    {
        if (!Service::Get().Enabled()) Fail(api, "off", "face tracking is off (-facetrack_off)");
    }

    // ---- Characters ----

    // The skeleton carrier of a character, the only entity a binding works on.
    Entity CarrierOf(World& world, const LuaEntity& id, const char* api)
    {
        const Entity live = LuaId::ResolveLive(world, id);
        if (live == NullEntity)
            Fail(api, "stale_handle", "entity " + std::to_string(id.raw()) + " is dead, or its slot was recycled");
        const Entity carrier = Facial::ResolveSkeletonCarrier(world, live);
        if (carrier == NullEntity)
            Fail(api, "no_skeleton", "entity " + std::to_string(id.raw()) + " has no SkeletonComponent under it");
        return carrier;
    }

    FaceTrackingComponent& BoundComponent(World& world, const LuaEntity& id, const char* api)
    {
        FaceTrackingComponent* comp = world.GetComponent<FaceTrackingComponent>(CarrierOf(world, id, api));
        if (!comp) Fail(api, "not_attached", "the character has no face tracking binding (FaceTracking.Attach)");
        return *comp;
    }

    float Weight(const sol::table& t, const char* key, float current, const char* api)
    {
        const sol::object o = t[key];
        if (o.get_type() == sol::type::lua_nil) return current;
        if (o.get_type() != sol::type::number) Fail(api, "bad_args", std::string("'") + key + "' is not a number");
        return std::clamp(static_cast<float>(o.as<double>()), 0.0f, 1.0f);
    }

    bool Flag(const sol::table& t, const char* key, bool current, const char* api)
    {
        const sol::object o = t[key];
        if (o.get_type() == sol::type::lua_nil) return current;
        if (o.get_type() != sol::type::boolean) Fail(api, "bad_args", std::string("'") + key + "' is not a boolean");
        return o.as<bool>();
    }

    float Seconds(const sol::table& t, const char* key, float current, const char* api)
    {
        const sol::object o = t[key];
        if (o.get_type() == sol::type::lua_nil) return current;
        if (o.get_type() != sol::type::number || !(o.as<double>() >= 0.0))
            Fail(api, "bad_args", std::string("'") + key + "' is not a number of seconds >= 0");
        return static_cast<float>(o.as<double>());
    }

    void ApplyOptions(FaceTrackingComponent& comp, const sol::table& o, const char* api)
    {
        const sol::object source = o["source"];
        if (source.get_type() == sol::type::string) comp.source = source.as<std::string>();
        else if (source.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "'source' is not a string");
        const sol::object profile = o["profile"];
        if (profile.get_type() == sol::type::string)
        {
            const std::string p = profile.as<std::string>();
            if (!p.empty() && !Resource::AssetFS::Get().Exists(p))
                Fail(api, "no_profile", "no mapping at '" + p + "'");
            comp.profile = p;
        }
        else if (profile.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "'profile' is not a string");
        comp.mirror = Flag(o, "mirror", comp.mirror, api);
        comp.weightMorph = Weight(o, "morph", comp.weightMorph, api);
        comp.weightHead = Weight(o, "head", comp.weightHead, api);
        comp.weightEyes = Weight(o, "eyes", comp.weightEyes, api);
        comp.holdSec = Seconds(o, "hold", comp.holdSec, api);
        comp.fadeOutSec = Seconds(o, "fadeOut", comp.fadeOutSec, api);
        comp.fadeInSec = Seconds(o, "fadeIn", comp.fadeInSec, api);
        comp.suppressAutoBlink = Flag(o, "suppressBlink", comp.suppressAutoBlink, api);
        comp.headTrackHandoff = Flag(o, "handoff", comp.headTrackHandoff, api);
        const sol::object enabled = o["enabled"];
        if (enabled.get_type() == sol::type::boolean) comp.enabled = enabled.as<bool>();
        else if (enabled.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "'enabled' is not a boolean");
    }

    sol::table YawPitchRollTable(sol::state_view& lv, const DirectX::XMFLOAT4& q)
    {
        const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(DirectX::XMLoadFloat4(&q));
        return lv.create_table_with("yaw", ypr.yaw * kRadToDeg, "pitch", ypr.pitch * kRadToDeg, "roll", ypr.roll * kRadToDeg);
    }

    sol::table DebugTable(sol::state_view& lv, const FaceTrackingComponent& comp, const SkeletonAsset* skel)
    {
        auto boneName = [skel](int b) -> std::string {
            return (skel && b >= 0 && static_cast<uint32_t>(b) < skel->boneCount) ? std::string(skel->boneNames[b]) : std::string();
        };
        sol::table t = lv.create_table();
        t["enabled"] = comp.enabled;
        t["source"] = comp.source;
        t["profile"] = comp.profile;
        t["mirror"] = comp.mirror;
        t["headAuthority"] = comp.headAuthority;
        const BindingRuntime* rt = comp.rt.get();
        t["bound"] = rt != nullptr;
        if (!rt) return t;
        t["mapping"] = rt->profilePath;
        t["compiled"] = rt->compiled;
        t["error"] = rt->compileError;
        t["state"] = TrackStateName(rt->authority.State());
        t["authority"] = rt->authorityValue;
        t["sample"] = SampleStateName(rt->sampleState);
        t["clock"] = static_cast<double>(rt->nowUs) / 1e6;
        t["consumed"] = static_cast<long long>(rt->framesConsumed);
        t["missed"] = static_cast<long long>(rt->cursor.missed);
        t["history"] = static_cast<long long>(rt->processed.Size());
        t["sourceMissing"] = rt->sourceMissing;
        t["morphs"] = static_cast<long long>(rt->trackedCount);
        t["morphAlpha"] = rt->morphAlpha;
        t["blinkHeld"] = rt->blinkHeld;
        t["targets"] = static_cast<long long>(rt->map.targets.size());
        t["vowels"] = rt->map.vowelsEnabled;
        if (rt->haveHead) t["head"] = YawPitchRollTable(lv, rt->headTarget);
        if (rt->haveEyes)
            t["eyes"] = lv.create_table_with(
                "left", lv.create_table_with("yaw", rt->eyeYawPitchDeg[0][0], "pitch", rt->eyeYawPitchDeg[0][1]),
                "right", lv.create_table_with("yaw", rt->eyeYawPitchDeg[1][0], "pitch", rt->eyeYawPitchDeg[1][1]));
        if (rt->haveJaw) t["jaw"] = rt->jawAngle * kRadToDeg;
        sol::table chain = lv.create_table();
        for (int k = 0; k < rt->chainCount; ++k)
            chain[k + 1] = lv.create_table_with("bone", boneName(rt->chainBone[k]), "weight", rt->chainWeight[k]);
        t["chain"] = chain;
        t["headBone"] = boneName(rt->headBone);
        t["leftEye"] = boneName(rt->leftEye);
        t["rightEye"] = boneName(rt->rightEye);
        t["jawBone"] = boneName(rt->jawBone);
        t["basis"] = rt->basisFromEyes ? "eyes" : "axes";
        sol::table warnings = lv.create_table();
        for (size_t i = 0; i < rt->warnings.size(); ++i) warnings[i + 1] = rt->warnings[i];
        t["warnings"] = warnings;
        return t;
    }
} // namespace

void RegisterLuaBindings(sol::state& state, World& world, SkeletonRegistry& skeletons)
{
    sol::state* lua = &state;
    sol::table ft = lua->create_table();

    LuaDoc::Fn(ft, "FaceTracking", "OpenReplay", [](const std::string& path, sol::optional<sol::table> opts) -> std::string
    {
        constexpr const char* api = "FaceTracking.OpenReplay";
        RequireOn(api);
        std::string name = "default";
        bool loop = true;
        if (opts)
        {
            const sol::object n = (*opts)["name"];
            if (n.get_type() == sol::type::string) name = n.as<std::string>();
            else if (n.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.name is not a string");
            const sol::object l = (*opts)["loop"];
            if (l.get_type() == sol::type::boolean) loop = l.as<bool>();
            else if (l.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.loop is not a boolean");
        }
        SourceStatus existing;
        if (Service::Get().Status(name, existing)) Fail(api, "exists", "a source named '" + name + "' is open already");
        std::string error;
        if (!Service::Get().OpenReplay(path, name, loop, error)) Fail(api, "open_failed", error);
        return name;
    },
        { .doc = "Opens a take (.iface) as a replay source played against its own simulation clock (it advances with "
                 "the animation chain's dt, so it pauses in Edit mode and repeats exactly under a fixed time step); "
                 "returns the source name.",
          .params = { { "path", "the take, cwd-relative or an asset path" },
                      { "opts", "{name = 'default', loop = true}", "table", true } },
          .returnsDoc = "the source name",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "bad_args", "exists", "open_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "OpenNetwork", [](sol::table opts) -> std::string
    {
        constexpr const char* api = "FaceTracking.OpenNetwork";
        RequireOn(api);
        const sol::object proto = opts["protocol"];
        if (proto.get_type() != sol::type::string) Fail(api, "bad_args", "protocol is \"vmc\" or \"ifacialmocap\"");
        NetworkSourceOptions o;
        uint16_t defaultPort = 0;
        if (!ParseNetworkProtocol(proto.as<std::string>(), o.protocol, defaultPort))
            Fail(api, "bad_args", "protocol is \"vmc\" or \"ifacialmocap\", not '" + proto.as<std::string>() + "'");
        o.port = defaultPort;
        const sol::object port = opts["port"];
        if (port.get_type() == sol::type::number)
        {
            const double p = port.as<double>();
            if (!(p >= 0.0 && p <= 65535.0) || p != static_cast<double>(static_cast<int>(p)))
                Fail(api, "bad_args", "port is an integer 0..65535 (0 = any free port)");
            o.port = static_cast<uint16_t>(p);
        }
        else if (port.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "port is a number");
        const sol::object lan = opts["lan"];
        if (lan.get_type() == sol::type::boolean) o.lan = lan.as<bool>();
        else if (lan.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "lan is a boolean");
        const sol::object delay = opts["delayMs"];
        if (delay.get_type() == sol::type::number)
        {
            const double ms = delay.as<double>();
            if (!(ms >= 0.0 && ms <= 1000.0)) Fail(api, "bad_args", "delayMs is 0..1000");
            o.delayUs = static_cast<int64_t>(ms * 1000.0);
        }
        else if (delay.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "delayMs is a number");
        std::string name = "default";
        const sol::object n = opts["name"];
        if (n.get_type() == sol::type::string) name = n.as<std::string>();
        else if (n.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "name is a string");
        SourceStatus existing;
        if (Service::Get().Status(name, existing)) Fail(api, "exists", "a source named '" + name + "' is open already");
        std::string error;
        if (!Service::Get().OpenNetwork(o, name, error)) Fail(api, "open_failed", error);
        return name;
    },
        { .doc = "Listens for a face over UDP: protocol \"vmc\" (OSC, port 39539 by default; Perfect Sync ARKit names "
                 "or VRM presets, the head and eye bones) or \"ifacialmocap\" (port 49983; the app is answered so it "
                 "starts streaming). port = 0 takes any free port (read it back from FaceTracking.Status). Binds "
                 "127.0.0.1 unless lan = true (0.0.0.0: Windows asks about the firewall once). The source runs on "
                 "the wall clock; a binding samples delayMs (50) behind the newest packet. Returns the source name.",
          .params = { { "opts", "{protocol, port, lan = false, name = 'default', delayMs = 50}", "table" } },
          .returnsDoc = "the source name",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "bad_args", "exists", "open_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "CreateSynthetic", [](const std::string& name) -> std::string
    {
        constexpr const char* api = "FaceTracking.CreateSynthetic";
        RequireOn(api);
        SourceStatus existing;
        if (Service::Get().Status(name, existing)) Fail(api, "exists", "a source named '" + name + "' is open already");
        std::string error;
        if (!Service::Get().CreateSynthetic(name, error)) Fail(api, "bad_args", error);
        return name;
    },
        { .doc = "Creates a synthetic source: frames come from FaceTracking.Push and are stamped with the source's "
                 "simulation clock when the next animation chain drains them.",
          .params = { { "name", "the source name" } },
          .returnsDoc = "the source name",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "exists", "bad_args" } });

    LuaDoc::Fn(ft, "FaceTracking", "Close", [](const std::string& name) -> bool
    {
        return Service::Get().Close(name);
    },
        { .doc = "Closes a source; false when there is none of that name.",
          .params = { { "name", "the source name" } },
          .effect = LuaDoc::Effect::Process });

    LuaDoc::Fn(ft, "FaceTracking", "Push", [](const std::string& name, sol::object frame) -> long long
    {
        constexpr const char* api = "FaceTracking.Push";
        RequireOn(api);
        if (frame.get_type() != sol::type::table) Fail(api, "bad_frame", "the frame is a table");
        Service& svc = Service::Get();
        SourceStatus st;
        if (!svc.Status(name, st)) Fail(api, "no_source", "no source named '" + name + "'");
        if (st.kind != "synthetic") Fail(api, "not_synthetic", "'" + name + "' is a " + st.kind + " source");
        const FaceFrame f = FrameFromTable(svc, name, frame.as<sol::table>(), api);
        std::string error;
        const uint64_t seq = svc.Push(name, f, error);
        if (seq == 0) Fail(api, "ring_full", error);
        return static_cast<long long>(seq);
    },
        { .doc = "Pushes one frame to a synthetic source: {blend = {eyeBlinkLeft = 1, ...}, head = {yaw, pitch, roll}, "
                 "eyes = {left = {yaw, pitch}, right = {yaw, pitch}}, found = true, conf = 1}, angles in degrees. ARKit "
                 "channel names are exact; any other name becomes one of the source's extra channels. Every number must "
                 "be finite (bad_frame for NaN or an infinity).",
          .params = { { "name", "the synthetic source" },
                      { "frame", "the frame", "table" } },
          .returnsDoc = "the frame's seq",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "bad_frame", "no_source", "not_synthetic", "ring_full" } });

    LuaDoc::Fn(ft, "FaceTracking", "Sources", [](sol::this_state ts) -> sol::table
    {
        sol::state_view lv(ts);
        sol::table out = lv.create_table();
        int i = 0;
        for (const SourceStatus& s : Service::Get().Sources()) out[++i] = StatusTable(lv, s);
        return out;
    },
        { .doc = "Every open source's status, by name: {name, kind, detail, epoch, received, refused, history, newestSeq, "
                 "newestTime, clock, live, state, port, lastError, found, conf, finished}; times in seconds of the "
                 "source's clock (the wall clock for a live source). state is running, or a capture's opening / "
                 "denied / busy / lost / ended / failed; port for a network source; lastError for a live source "
                 "and for any source with an error; recording, and while it records recordedFrames and "
                 "recordedSeconds (the take's length on the source's clock).",
          .returns = "table" });

    LuaDoc::Fn(ft, "FaceTracking", "Status", [](sol::this_state ts, const std::string& name) -> sol::object
    {
        sol::state_view lv(ts);
        SourceStatus s;
        if (!Service::Get().Status(name, s)) return sol::make_object(lv, sol::lua_nil);
        return sol::make_object(lv, StatusTable(lv, s));
    },
        { .doc = "One source's status (see FaceTracking.Sources), or nil.",
          .params = { { "name", "the source name" } },
          .returns = "table?" });

    LuaDoc::Fn(ft, "FaceTracking", "GetFrame", [](sol::this_state ts, const std::string& name) -> sol::object
    {
        sol::state_view lv(ts);
        FaceFrame f;
        if (!Service::Get().Newest(name, f)) return sol::make_object(lv, sol::lua_nil);
        return sol::make_object(lv, FrameTable(lv, Service::Get(), name, f));
    },
        { .doc = "The newest frame in a source's history, canonical: {seq, t, found, conf, blend = {name = value}, "
                 "head = {yaw, pitch, roll, q = {x, y, z, w}}, pos, eyes = {left, right}}, angles in degrees; nil "
                 "before its first frame.",
          .params = { { "name", "the source name" } },
          .returns = "table?" });

    // ---- Image sources: cameras, video files, the harness's fake camera ----

    LuaDoc::Fn(ft, "FaceTracking", "ListCameras", [](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        constexpr const char* api = "FaceTracking.ListCameras";
        RequireOn(api);
        bool refresh = false;
        if (opts)
        {
            const sol::object r = (*opts)["refresh"];
            if (r.get_type() == sol::type::boolean) refresh = r.as<bool>();
            else if (r.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.refresh is a boolean");
        }
        if (!Service::Get().CameraAllowed()) Fail(api, "camera_off", "the camera is off: the player has not allowed it (faceTracking.camera)");
        std::vector<CameraDevice> cameras;
        std::string error;
        if (!Service::Get().ListCameras(cameras, refresh, error)) Fail(api, "list_failed", error);
        sol::state_view lv(ts);
        sol::table out = lv.create_table();
        int i = 0;
        for (const CameraDevice& d : cameras)
        {
            sol::table c = lv.create_table();
            c["id"] = d.id;
            c["name"] = d.name;
            sol::table formats = lv.create_table();
            int k = 0;
            for (const CameraFormat& f : d.formats)
            {
                sol::table one = lv.create_table();
                const char cc[5] = {static_cast<char>(f.fourcc & 0xFF), static_cast<char>((f.fourcc >> 8) & 0xFF),
                                    static_cast<char>((f.fourcc >> 16) & 0xFF), static_cast<char>((f.fourcc >> 24) & 0xFF), 0};
                one["format"] = f.fourcc == kFourccRGB24 ? std::string("RGB24") : std::string(cc);
                one["width"] = static_cast<int>(f.width);
                one["height"] = static_cast<int>(f.height);
                one["fps"] = f.fps;
                formats[++k] = one;
            }
            c["formats"] = formats;
            out[++i] = c;
        }
        return out;
    },
        { .doc = "The cameras Windows lists, each {id, name, formats = {{format, width, height, fps}}}; id is the "
                 "device's symbolic link, stable across replugging (open a camera by it). Listing opens every "
                 "camera, so the answer is cached until refresh = true or a device is plugged in or out.",
          .params = { { "opts", "{refresh = false}", "table", true } },
          .returns = "table",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "camera_off", "bad_args", "list_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "CameraAllowed", []() -> bool
    {
        return Service::Get().CameraAllowed();
    },
        { .doc = "Whether face tracking may use a camera: the player's setting faceTracking.camera (off by default; "
                 "Settings.set and Settings.apply change it). While it is off ListCameras and OpenCamera answer "
                 "camera_off, and turning it off closes every open camera. Replays, network apps, video files and "
                 "the fake camera do not need it." });

    LuaDoc::Fn(ft, "FaceTracking", "OpenCamera", [](sol::optional<sol::table> opts) -> std::string
    {
        constexpr const char* api = "FaceTracking.OpenCamera";
        RequireOn(api);
        std::string id, name = "default";
        double width = 0, height = 0, fps = 0;
        if (opts)
        {
            auto str = [&](const char* key, std::string& out) {
                const sol::object o = (*opts)[key];
                if (o.get_type() == sol::type::string) out = o.as<std::string>();
                else if (o.get_type() != sol::type::lua_nil) Fail(api, "bad_args", std::string("opts.") + key + " is a string");
            };
            auto num = [&](const char* key, double& out, double hi) {
                const sol::object o = (*opts)[key];
                if (o.get_type() == sol::type::number) out = o.as<double>();
                else if (o.get_type() != sol::type::lua_nil) Fail(api, "bad_args", std::string("opts.") + key + " is a number");
                if (!(out >= 0.0 && out <= hi)) Fail(api, "bad_args", std::string("opts.") + key + " is out of range");
            };
            str("id", id);
            str("name", name);
            num("width", width, 8192);
            num("height", height, 8192);
            num("fps", fps, 240);
        }
        if ((width == 0) != (height == 0)) Fail(api, "bad_args", "give both width and height, or neither");
        if (!Service::Get().CameraAllowed()) Fail(api, "camera_off", "the camera is off: the player has not allowed it (faceTracking.camera)");
        SourceStatus existing;
        if (Service::Get().Status(name, existing)) Fail(api, "exists", "a source named '" + name + "' is open already");
        std::string error;
        if (!Service::Get().OpenCamera(id, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                       static_cast<float>(fps), name, error))
            Fail(api, "open_failed", error);
        return name;
    },
        { .doc = "Opens a camera as a face source: id from FaceTracking.ListCameras (\"\" = the first camera); width "
                 "/ height / fps 0 = automatic (at least 640x480, native NV12 or YUY2 first, 60 fps when such a mode "
                 "has it). A camera that is refused by the privacy setting, held by another program or unplugged is "
                 "still opened: its Status says denied / busy / lost and lastError says what to do; a busy camera "
                 "retries every 3 s, a lost one reopens when it is plugged back. Returns the source name.",
          .params = { { "opts", "{id = '', width = 0, height = 0, fps = 0, name = 'default'}", "table", true } },
          .returnsDoc = "the source name",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "camera_off", "bad_args", "exists", "open_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "OpenVideo", [](const std::string& path, sol::optional<sol::table> opts) -> std::string
    {
        constexpr const char* api = "FaceTracking.OpenVideo";
        RequireOn(api);
        std::string name = "default";
        if (opts)
        {
            const sol::object n = (*opts)["name"];
            if (n.get_type() == sol::type::string) name = n.as<std::string>();
            else if (n.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.name is a string");
        }
        SourceStatus existing;
        if (Service::Get().Status(name, existing)) Fail(api, "exists", "a source named '" + name + "' is open already");
        std::string error;
        if (!Service::Get().OpenVideo(path, name, error)) Fail(api, "open_failed", error);
        return name;
    },
        { .doc = "Opens a video file as a face source: every frame, in order, each once its time has come on the "
                 "source's simulation clock (it pauses in Edit mode, and two runs under a fixed time step see the "
                 "same frames at the same times). Software-decoded. Returns the source name.",
          .params = { { "path", "the video, cwd-relative or an asset path" },
                      { "opts", "{name = 'default'}", "table", true } },
          .returnsDoc = "the source name",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "bad_args", "exists", "open_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "OpenFakeCamera", [](sol::optional<sol::table> opts) -> std::string
    {
        constexpr const char* api = "FaceTracking.OpenFakeCamera";
        RequireOn(api);
        std::string fault, name = "default", video;
        int fps = 30, trackDelayMs = 0;
        if (opts)
        {
            const sol::object f = (*opts)["fault"];
            if (f.get_type() == sol::type::string) fault = f.as<std::string>();
            else if (f.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.fault is a string");
            const sol::object n = (*opts)["name"];
            if (n.get_type() == sol::type::string) name = n.as<std::string>();
            else if (n.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.name is a string");
            const sol::object v = (*opts)["video"];
            if (v.get_type() == sol::type::string) video = v.as<std::string>();
            else if (v.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.video is a string");
            const sol::object r = (*opts)["fps"];
            if (r.get_type() == sol::type::number)
            {
                const double d = r.as<double>();
                if (!(d >= 1.0 && d <= 1000.0) || d != std::floor(d)) Fail(api, "bad_args", "opts.fps is a whole number from 1 to 1000");
                fps = static_cast<int>(d);
            }
            else if (r.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.fps is a number");
            const sol::object dl = (*opts)["trackDelayMs"];
            if (dl.get_type() == sol::type::number)
            {
                const double d = dl.as<double>();
                if (!(d >= 0.0 && d <= 1000.0) || d != std::floor(d)) Fail(api, "bad_args", "opts.trackDelayMs is a whole number from 0 to 1000");
                trackDelayMs = static_cast<int>(d);
            }
            else if (dl.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.trackDelayMs is a number");
        }
        if (fault == "none") fault.clear();
        SourceStatus existing;
        if (Service::Get().Status(name, existing)) Fail(api, "exists", "a source named '" + name + "' is open already");
        std::string error;
        if (!Service::Get().OpenFakeCamera(fault, name, error, video, fps, trackDelayMs)) Fail(api, "open_failed", error);
        return name;
    },
        { .doc = "Opens a synthetic camera (640x480 at 30 Hz, a moving pattern) that behaves as a real one, or fails "
                 "as one: fault = \"denied\" (the privacy setting), \"busy\" (in use: retried every 3 s) or \"lost\" "
                 "(unplugged after 30 frames, back on the next device change); video = a file whose frames it loops "
                 "instead of the pattern (a live source with a face); fps = its rate, 1 to 1000; trackDelayMs = a "
                 "sleep before each of its images is tracked, 0 to 1000 (a slower CPU: a camera faster than its "
                 "tracker). For tests on machines without a camera; builds without the test harness answer open_failed.",
          .params = { { "opts", "{fault = 'none', name = 'default', video = nil, fps = 30, trackDelayMs = 0}", "table", true } },
          .returnsDoc = "the source name",
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "bad_args", "exists", "open_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "RequestPreview", [](const std::string& name) -> bool
    {
        SourceStatus existing;
        if (!Service::Get().Status(name, existing)) return false;
        Service::Get().RequestPreview(name);
        return true;
    },
        { .doc = "Asks for a source's images in the preview texture for this frame and the next (call it every frame "
                 "while the preview is shown; nothing is uploaded once nobody asks). At most 15 uploads a second, "
                 "480x360, letterboxed. False when there is no source of that name.",
          .params = { { "name", "the source name" } },
          .effect = LuaDoc::Effect::Process });

    LuaDoc::Fn(ft, "FaceTracking", "PreviewStats", [](sol::this_state ts) -> sol::table
    {
        const PreviewStats p = Service::Get().GetPreviewStats();
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["source"] = p.source;
        t["shown"] = p.shown;
        t["visible"] = p.visible;
        t["images"] = static_cast<long long>(p.images);
        t["uploads"] = static_cast<long long>(p.uploads);
        t["queued"] = static_cast<long long>(p.queued);
        t["replaced"] = static_cast<long long>(p.replaced);
        t["carriedOver"] = static_cast<long long>(p.carriedOver);
        t["dropped"] = static_cast<long long>(p.dropped);
        t["rejected"] = static_cast<long long>(p.rejected);
        t["fallback"] = static_cast<long long>(p.fallback);
        PreviewOverlay ov;
        const bool overlay = Service::Get().GetPreviewOverlay(ov);
        t["overlay"] = overlay;
        t["overlayFace"] = overlay && ov.face.found;
        t["overlayPresence"] = overlay ? ov.face.presence : 0.0f;
        return t;
    },
        { .doc = "The preview's counts since boot: {source, shown, visible, images, uploads, queued, replaced, carriedOver, "
                 "dropped, rejected, fallback, overlay, overlayFace, overlayPresence} - uploads is how many images went to "
                 "the texture (each one queued, replaced, carried over, dropped, rejected or a synchronous fallback); visible "
                 "is whether something asked this frame or the last; shown is whose image the texture holds; overlay is "
                 "whether that image has the tracker's output (the Face Tracking window draws it over the preview), "
                 "overlayFace whether a face was found in it, overlayPresence its presence (0-1).",
          .returns = "table" });

    LuaDoc::Fn(ft, "FaceTracking", "NotifyDeviceChange", []()
    {
        Service::Get().OnDeviceChange();
    },
        { .doc = "Acts as if Windows reported a device change (what plugging a camera in or out sends): the camera "
                 "list is listed again on the next ask, and a lost camera looks for its device and reopens when it is "
                 "back. For a script that knows better than the window messages, and for tests.",
          .effect = LuaDoc::Effect::Process });

    // ---- Characters: bindings on the skeleton carrier ----

    LuaDoc::Fn(ft, "FaceTracking", "Attach", [&world, &skeletons](LuaEntity id, sol::optional<sol::table> opts) -> LuaEntity
    {
        constexpr const char* api = "FaceTracking.Attach";
        RequireOn(api);
        const Entity carrier = CarrierOf(world, id, api);
        world.EnsurePool<FaceTrackingComponent>();
        if (!world.GetComponent<FaceTrackingComponent>(carrier))
            world.AddComponent<FaceTrackingComponent>(carrier, FaceTrackingComponent{});
        // The morphs go through the carrier's FacialAnimationComponent. One added here auto-blinks only
        // when the model has its blinkMorph: a model without that morph (MMD's "mabataki") would
        // otherwise log that auto-blink is inert.
        if (!world.GetComponent<FacialAnimationComponent>(carrier))
        {
            world.EnsurePool<FacialAnimationComponent>();
            FacialAnimationComponent fa;
            fa.autoBlinkEnabled = false;
            if (const auto* sk = world.GetComponent<SkeletonComponent>(carrier);
                sk && sk->assetIndex != kInvalidAnimHandle && sk->assetIndex < skeletons.Count())
                fa.autoBlinkEnabled = Facial::FindMorphSlot(skeletons.Get(sk->assetIndex), fa.blinkMorph) >= 0;
            world.AddComponent<FacialAnimationComponent>(carrier, fa);
        }
        FaceTrackingComponent* comp = world.GetComponent<FaceTrackingComponent>(carrier);
        if (opts) ApplyOptions(*comp, *opts, api);
        return LuaId::Out(world, carrier);
    },
        { .doc = "Binds a character to a face source: a FaceTrackingComponent on its skeleton entity (and a "
                 "FacialAnimationComponent when it has none - the morphs go through its fold, after Authored). "
                 "Options: source ('default'), profile (a .ifacemap; empty = the built-in mapping that fits the "
                 "model), mirror (true), morph / head / eyes weights (0..1), hold / fadeOut / fadeIn seconds, "
                 "suppressBlink, handoff (HeadTrack gives way). Attaching again updates the given options.",
          .params = { { "id", "the character (its root or its skeleton entity)" },
                      { "opts", "{source, profile, mirror, morph, head, eyes, hold, fadeOut, fadeIn, suppressBlink, handoff}", "table", true } },
          .returnsDoc = "the skeleton entity that carries the binding",
          .effect = LuaDoc::Effect::World,
          .errors = { "off", "stale_handle", "no_skeleton", "bad_args", "no_profile" } });

    LuaDoc::Fn(ft, "FaceTracking", "Detach", [&world](LuaEntity id) -> bool
    {
        constexpr const char* api = "FaceTracking.Detach";
        const Entity carrier = CarrierOf(world, id, api);
        if (!world.GetComponent<FaceTrackingComponent>(carrier)) return false;
        world.RemoveComponent<FaceTrackingComponent>(carrier);
        return true;
    },
        { .doc = "Removes a character's binding (its FacialAnimationComponent stays); the face is as animated from "
                 "the next frame. false when it had none.",
          .params = { { "id", "the character" } },
          .effect = LuaDoc::Effect::World,
          .errors = { "stale_handle", "no_skeleton" } });

    LuaDoc::Fn(ft, "FaceTracking", "SetWeights", [&world](LuaEntity id, sol::table weights)
    {
        constexpr const char* api = "FaceTracking.SetWeights";
        FaceTrackingComponent& comp = BoundComponent(world, id, api);
        comp.weightMorph = Weight(weights, "morph", comp.weightMorph, api);
        comp.weightHead = Weight(weights, "head", comp.weightHead, api);
        comp.weightEyes = Weight(weights, "eyes", comp.weightEyes, api);
    },
        { .doc = "How much of the tracked face reaches a character: {morph, head, eyes}, each 0..1 (absent keys keep "
                 "their value).",
          .params = { { "id", "the character" }, { "weights", "{morph, head, eyes}", "table" } },
          .effect = LuaDoc::Effect::World,
          .errors = { "stale_handle", "no_skeleton", "not_attached", "bad_args" } });

    LuaDoc::Fn(ft, "FaceTracking", "SetMirror", [&world](LuaEntity id, bool mirror)
    {
        BoundComponent(world, id, "FaceTracking.SetMirror").mirror = mirror;
    },
        { .doc = "Whether the character mirrors the performer (the live default) or moves as recorded; the binding "
                 "re-reads its source's last 2 s through the new setting.",
          .params = { { "id", "the character" }, { "mirror", "true = a mirror image" } },
          .effect = LuaDoc::Effect::World,
          .errors = { "stale_handle", "no_skeleton", "not_attached" } });

    LuaDoc::Fn(ft, "FaceTracking", "GetDebug", [&world, &skeletons](sol::this_state ts, LuaEntity id) -> sol::object
    {
        sol::state_view lv(ts);
        const Entity live = LuaId::ResolveLive(world, id);
        const Entity carrier = live == NullEntity ? NullEntity : Facial::ResolveSkeletonCarrier(world, live);
        const FaceTrackingComponent* comp = carrier == NullEntity ? nullptr : world.GetComponent<FaceTrackingComponent>(carrier);
        if (!comp) return sol::make_object(lv, sol::lua_nil);
        const SkeletonAsset* skel = nullptr;
        if (const auto* sk = world.GetComponent<SkeletonComponent>(carrier);
            sk && sk->assetIndex != kInvalidAnimHandle && sk->assetIndex < skeletons.Count())
            skel = &skeletons.Get(sk->assetIndex);
        return sol::make_object(lv, DebugTable(lv, *comp, skel));
    },
        { .doc = "A binding as the system sees it, for a person to read: state, authority, the mapping it compiled "
                 "and what that found, this frame's morph count, head / eye / jaw targets (degrees), the resolved "
                 "bones. Self-reported - a test asserts on Face.GetResolved, Morph.GetPoolWeights and "
                 "Skeleton.BoneWorld instead. nil when the character has no binding.",
          .params = { { "id", "the character" } },
          .returns = "table?" });

    // ---- Calibration and the automatic mapping ----

    LuaDoc::Fn(ft, "FaceTracking", "Calibrate", [](const std::string& name, sol::optional<sol::table> opts)
    {
        constexpr const char* api = "FaceTracking.Calibrate";
        RequireOn(api);
        std::string mode = "neutral";
        double seconds = 1.0;
        if (opts)
        {
            const sol::object m = (*opts)["mode"];
            if (m.get_type() == sol::type::string) mode = m.as<std::string>();
            else if (m.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.mode is \"neutral\" or \"range\"");
            const sol::object s = (*opts)["seconds"];
            if (s.get_type() == sol::type::number) seconds = s.as<double>();
            else if (s.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.seconds is a number");
        }
        if (mode != "neutral" && mode != "range") Fail(api, "bad_args", "opts.mode is \"neutral\" or \"range\", not '" + mode + "'");
        SourceStatus existing;
        if (!Service::Get().Status(name, existing)) Fail(api, "no_source", "no source named '" + name + "'");
        std::string error;
        if (!Service::Get().StartCalibration(name, mode == "range", seconds, error)) Fail(api, "bad_args", error);
    },
        { .doc = "Starts calibrating a source from the performer's own face: mode \"neutral\" (relax, look ahead) "
                 "averages the next `seconds` (1) of found frames - each channel's resting value and the head's "
                 "resting turn; \"range\" (pull faces) keeps each channel's highest. Every character bound to the "
                 "source then reads c' = (c - neutral) / (max - neutral) and its head relative to the neutral one. "
                 "Kept by source name and saved beside the user's settings, never under -unattended. Follow it "
                 "with FaceTracking.CalibrationStatus.",
          .params = { { "name", "the source name" },
                      { "opts", "{mode = 'neutral', seconds = 1}", "table", true } },
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "no_source", "bad_args" } });

    LuaDoc::Fn(ft, "FaceTracking", "CalibrationStatus", [](sol::this_state ts, const std::string& name) -> sol::table
    {
        const CalibrationStatus c = Service::Get().GetCalibrationStatus(name);
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["capturing"] = c.capturing;
        t["mode"] = c.mode;
        t["progress"] = c.progress;
        t["generation"] = static_cast<long long>(c.generation);
        t["hasNeutral"] = c.hasNeutral;
        t["hasRange"] = c.hasRange;
        t["frames"] = static_cast<long long>(c.frames);
        if (!c.error.empty()) t["error"] = c.error;
        return t;
    },
        { .doc = "A source's calibration: {capturing, mode, progress (0..1), generation (0 = none; bumped by every "
                 "change), hasNeutral, hasRange, frames (what the last capture took), error (why the last capture "
                 "ended without a calibration: no face came, the source's time stood still, the source closed)}. "
                 "A capture its source does not finish within max(10, 3 x seconds + 5) s gives up.",
          .params = { { "name", "the source name" } },
          .returns = "table" });

    LuaDoc::Fn(ft, "FaceTracking", "ResetCalibration", [](const std::string& name)
    {
        Service::Get().ResetCalibration(name);
    },
        { .doc = "Forgets a source's calibration (and stops a capture): its characters read the raw face again.",
          .params = { { "name", "the source name" } },
          .effect = LuaDoc::Effect::Process });

    LuaDoc::Fn(ft, "FaceTracking", "AutoMap", [&world, &skeletons](sol::this_state ts, LuaEntity id, sol::optional<std::string> outPath) -> sol::table
    {
        constexpr const char* api = "FaceTracking.AutoMap";
        const Entity live = LuaId::ResolveLive(world, id);
        const Entity carrier = live == NullEntity ? NullEntity : Facial::ResolveSkeletonCarrier(world, live);
        const SkeletonComponent* sk = carrier == NullEntity ? nullptr : world.GetComponent<SkeletonComponent>(carrier);
        if (!sk || sk->assetIndex == kInvalidAnimHandle || sk->assetIndex >= skeletons.Count())
            Fail(api, "no_skeleton", "the character has no loaded skeleton");
        const SkeletonAsset& skel = skeletons.Get(sk->assetIndex);
        std::vector<std::string> morphs;
        for (uint32_t i = 0; i < skel.morphTargetCount; ++i) morphs.emplace_back(skel.morphTargetNames[i]);
        const std::string basePath = FaceTrackingSystem::AutoProfilePath(skel);
        FaceMapProfile base;
        std::string error;
        if (!FaceTrackingSystem::LoadProfileFile(basePath, base, error)) Fail(api, "base_failed", error);
        const std::string baseName = Sys::PathToUtf8(Sys::U8Path(basePath).stem());
        const AutoMapResult r = AutoMapMorphs(morphs, base, baseName);
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["base"] = baseName;
        t["rules"] = static_cast<long long>(r.profile.morphs.size());
        auto pairs = [&](const std::vector<std::pair<std::string, std::string>>& v, const char* a, const char* b) {
            sol::table out = lv.create_table();
            for (size_t i = 0; i < v.size(); ++i)
            {
                sol::table row = lv.create_table();
                row[a] = v[i].first;
                row[b] = v[i].second;
                out[i + 1] = row;
            }
            return out;
        };
        auto list = [&](const std::vector<std::string>& v) {
            sol::table out = lv.create_table();
            for (size_t i = 0; i < v.size(); ++i) out[i + 1] = v[i];
            return out;
        };
        t["matches"] = pairs(r.matches, "channel", "morph");
        t["renamed"] = pairs(r.renamed, "from", "to");
        t["unmapped"] = list(r.unmappedChannels);
        t["undriven"] = list(r.undrivenMorphs);
        if (outPath && !outPath->empty())
        {
            const std::string text = FaceMapToJson(r.profile);
            std::error_code ec;
            const std::filesystem::path parent = Sys::U8Path(*outPath).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent, ec);
            if (!Sys::WriteFileBytesAtomic(*outPath, text.data(), text.size())) Fail(api, "write_failed", "cannot write '" + *outPath + "'");
            t["path"] = *outPath;
        }
        return t;
    },
        { .doc = "Makes a mapping for a character from its morph names: the built-in one for its kind (arkit / vroid "
                 "/ mmd_jp) as the base, its rules on morphs the model spells differently copied under the model's "
                 "names, and every ARKit channel whose name matches a morph the base leaves alone driving it (names "
                 "matched loosely: case, _L / .L / Left, a blendShape. prefix, half-width katakana). Returns {base, "
                 "rules, matches = {{channel, morph}}, renamed = {{from, to}}, unmapped (channels nothing reads), "
                 "undriven (morphs nothing writes), path}; with outPath it writes the .ifacemap there (set it as "
                 "the FaceTracking component's profile).",
          .params = { { "id", "the character" }, { "outPath", "the .ifacemap to write", "string", true } },
          .returns = "table",
          .effect = LuaDoc::Effect::Disk,
          .errors = { "no_skeleton", "base_failed", "write_failed" } });

    // ---- Recording and baking ----

    LuaDoc::Fn(ft, "FaceTracking", "Record", [](const std::string& name, const std::string& path, sol::optional<sol::table> opts)
    {
        constexpr const char* api = "FaceTracking.Record";
        RequireOn(api);
        double maxSeconds = 600.0;
        if (opts)
        {
            const sol::object m = (*opts)["maxSeconds"];
            if (m.get_type() == sol::type::number) maxSeconds = m.as<double>();
            else if (m.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.maxSeconds is a number");
        }
        SourceStatus existing;
        if (!Service::Get().Status(name, existing)) Fail(api, "no_source", "no source named '" + name + "'");
        if (existing.recording) Fail(api, "recording", "'" + name + "' is being recorded already");
        std::string error;
        if (!Service::Get().StartRecording(name, path, maxSeconds, error)) Fail(api, "bad_args", error);
    },
        { .doc = "Starts recording a source to a take (.iface, JSON Lines): every frame it delivers from now on, as "
                 "the source gave it - before any character's calibration, mirror or filter, so the take replays "
                 "under any of them - timed from the first recorded frame. At most maxSeconds (600) of it. "
                 "FaceTracking.StopRecording writes the file; closing the source writes it too.",
          .params = { { "name", "the source name" },
                      { "path", "the take to write (.iface), cwd-relative or absolute" },
                      { "opts", "{maxSeconds = 600}", "table", true } },
          .effect = LuaDoc::Effect::Process,
          .errors = { "off", "no_source", "recording", "bad_args" } });

    LuaDoc::Fn(ft, "FaceTracking", "StopRecording", [](sol::this_state ts, const std::string& name) -> sol::table
    {
        constexpr const char* api = "FaceTracking.StopRecording";
        RequireOn(api);
        SourceStatus existing;
        if (!Service::Get().Status(name, existing)) Fail(api, "no_source", "no source named '" + name + "'");
        if (!existing.recording) Fail(api, "not_recording", "'" + name + "' is not being recorded");
        RecordingResult r;
        std::string error;
        if (!Service::Get().StopRecording(name, r, error)) Fail(api, "write_failed", error);
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["path"] = r.path;
        t["frames"] = static_cast<long long>(r.frames);
        t["seconds"] = r.seconds;
        t["truncated"] = r.truncated;
        return t;
    },
        { .doc = "Stops a source's recording and writes its take: {path, frames, seconds, truncated} - truncated when "
                 "it reached maxSeconds. write_failed when no frame arrived while it recorded (nothing is written) "
                 "or the file could not be written.",
          .params = { { "name", "the source name" } },
          .returns = "table",
          .effect = LuaDoc::Effect::Disk,
          .errors = { "off", "no_source", "not_recording", "write_failed" } });

    LuaDoc::Fn(ft, "FaceTracking", "Bake", [&world, &skeletons](sol::this_state ts, LuaEntity id, const std::string& takePath,
                                                                 const std::string& outPath, sol::optional<sol::table> opts) -> sol::table
    {
        constexpr const char* api = "FaceTracking.Bake";
        FaceTrackingSystem::BakeOptions o;
        if (opts)
        {
            const sol::object fps = (*opts)["fps"];
            if (fps.get_type() == sol::type::number) o.fps = static_cast<float>(fps.as<double>());
            else if (fps.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.fps is a number");
            const sol::object mirror = (*opts)["mirror"];
            if (mirror.get_type() == sol::type::boolean) o.mirror = mirror.as<bool>();
            else if (mirror.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.mirror is a boolean");
            const sol::object n = (*opts)["name"];
            if (n.get_type() == sol::type::string) o.clipName = n.as<std::string>();
            else if (n.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.name is a string");
            const sol::object c = (*opts)["calibration"];
            if (c.get_type() == sol::type::string) o.calibration = c.as<std::string>();
            else if (c.get_type() != sol::type::lua_nil) Fail(api, "bad_args", "opts.calibration is a source name");
        }
        const Entity live = LuaId::ResolveLive(world, id);
        if (live == NullEntity) Fail(api, "no_entity", "the entity is not alive");
        FaceTrackingSystem::BakeReport report;
        std::string error;
        if (!FaceTrackingSystem::BakeTakeFile(world, skeletons, live, takePath, outPath, o, report, error))
            Fail(api, "bake_failed", error);
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["path"] = outPath;
        t["keys"] = static_cast<long long>(report.keys);
        t["duration"] = report.duration;
        t["frames"] = static_cast<long long>(report.framesRead);
        t["morphs"] = static_cast<long long>(report.morphs);
        t["bones"] = static_cast<long long>(report.bones);
        t["root"] = report.root;
        t["folded"] = report.folded;
        t["profile"] = report.profile;
        sol::table warnings = lv.create_table();
        for (size_t i = 0; i < report.warnings.size(); ++i) warnings[i + 1] = report.warnings[i];
        t["warnings"] = warnings;
        return t;
    },
        { .doc = "Bakes a take into an .ianim for a character: the take runs through the character's mapping (its "
                 "FaceTracking settings when it has a binding: mapping, weights, timings) at fps keys a second, the "
                 "face at full authority from its first frame, unmirrored unless mirror = true. The file holds one "
                 "bone clip - the bones the mapping turns, from the Neck down (a chest share is folded into the "
                 "Neck): play it on an AnimLayer masked CustomRoot = root - and one morph clip of the same name, for "
                 "the Animator. calibration names the source whose calibration to apply (the performer's, kept by "
                 "FaceTracking.Calibrate; none by default - a take is recorded raw). Returns {path, keys, duration, "
                 "frames, morphs, bones, root, folded, profile, warnings}.",
          .params = { { "id", "the character" },
                      { "takePath", "the take (.iface), cwd-relative or an asset path" },
                      { "outPath", "the .ianim to write" },
                      { "opts", "{fps = 30, mirror = false, name = the take's file name, calibration = ''}", "table", true } },
          .returns = "table",
          .effect = LuaDoc::Effect::Disk,
          .errors = { "bad_args", "no_entity", "bake_failed" } });

    (*lua)["FaceTracking"] = ft;
}

} // namespace FaceTracking
