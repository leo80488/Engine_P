#include "ECS/NotifySerialization.h"

#include "ECS/StateToggle.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

namespace NotifyIO
{
    namespace
    {
        // ---- PropertyBag::Value <-> JSON ----
        nlohmann::json EncodeValue(const PropertyBag::Value& v)
        {
            nlohmann::json j;
            std::visit([&](auto const& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, int>)              { j["t"] = "i";  j["v"] = x; }
                else if constexpr (std::is_same_v<T, float>)       { j["t"] = "f";  j["v"] = x; }
                else if constexpr (std::is_same_v<T, std::string>) { j["t"] = "s";  j["v"] = x; }
                else if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>) {
                    j["t"] = "v3"; j["v"] = { x.x, x.y, x.z };
                } else if constexpr (std::is_same_v<T, DirectX::XMFLOAT4>) {
                    j["t"] = "v4"; j["v"] = { x.x, x.y, x.z, x.w };
                }
            }, v);
            return j;
        }

        PropertyBag::Value DecodeValue(const nlohmann::json& j)
        {
            const std::string t = j.value("t", "i");
            if (t == "i") return j.value("v", 0);
            if (t == "f") return j.value("v", 0.f);
            if (t == "s") return j.value("v", std::string{});
            if (t == "v3") { const auto& a = j["v"]; return DirectX::XMFLOAT3{ a[0], a[1], a[2] }; }
            if (t == "v4") { const auto& a = j["v"]; return DirectX::XMFLOAT4{ a[0], a[1], a[2], a[3] }; }
            return 0;
        }

        nlohmann::json EncodeBag(const PropertyBag& bag)
        {
            nlohmann::json j = nlohmann::json::object();
            for (const auto& [k, v] : bag.values) j[k] = EncodeValue(v);
            return j;
        }

        PropertyBag DecodeBag(const nlohmann::json& j)
        {
            PropertyBag bag;
            if (!j.is_object()) return bag;
            for (auto it = j.begin(); it != j.end(); ++it)
                bag.values[it.key()] = DecodeValue(it.value());
            return bag;
        }
    } // namespace

    std::string TracksToJsonString(const std::vector<NotifyTrack>& tracks,
                                   float                           clipDuration,
                                   uint32_t                        nextNotifyId)
    {
        nlohmann::json root;
        root["dur"] = clipDuration;
        root["nid"] = nextNotifyId;
        root["trk"] = nlohmann::json::array();

        for (const auto& tr : tracks)
        {
            nlohmann::json jtr;
            jtr["nm"]  = tr.name;
            jtr["cat"] = static_cast<int>(tr.category);
            jtr["mu"]  = tr.muted ? 1 : 0;

            jtr["n"] = nlohmann::json::array();
            for (const auto& n : tr.notifies)
            {
                nlohmann::json jn;
                jn["id"]  = n.id;
                jn["cat"] = static_cast<int>(n.category);
                jn["t"]   = n.time;
                jn["nm"]  = n.displayName;
                jn["col"] = n.color;
                jn["pb"]  = EncodeBag(n.params);
                jtr["n"].push_back(std::move(jn));
            }

            jtr["s"] = nlohmann::json::array();
            for (const auto& s : tr.states)
            {
                nlohmann::json js;
                js["id"]  = s.id;
                js["cat"] = static_cast<int>(s.category);
                js["t0"]  = s.startTime;
                js["t1"]  = s.endTime;
                js["nm"]  = s.displayName;
                js["col"] = s.color;
                js["pb"]  = EncodeBag(s.params);
                jtr["s"].push_back(std::move(js));
            }

            root["trk"].push_back(std::move(jtr));
        }

        return root.dump();
    }

    bool TracksFromJsonString(const std::string&        json,
                              std::vector<NotifyTrack>& outTracks,
                              float*                    outClipDuration,
                              uint32_t*                 outNextNotifyId)
    {
        outTracks.clear();
        if (json.empty()) return true; // nothing to parse: empty track set

        nlohmann::json root;
        try { root = nlohmann::json::parse(json); }
        catch (...) { return false; }

        if (outClipDuration) *outClipDuration = root.value("dur", 1.f);
        if (outNextNotifyId) *outNextNotifyId = root.value("nid", 1u);

        if (root.contains("trk") && root["trk"].is_array())
        {
            for (const auto& jtr : root["trk"])
            {
                NotifyTrack tr;
                tr.name     = jtr.value("nm", "");
                tr.category = static_cast<NotifyCategory>(jtr.value("cat", 5));
                tr.muted    = jtr.value("mu", 0) != 0;

                if (jtr.contains("n")) for (const auto& jn : jtr["n"])
                {
                    Notify n;
                    n.id          = jn.value("id", 0u);
                    n.category    = static_cast<NotifyCategory>(jn.value("cat", 5));
                    n.time        = jn.value("t", 0.f);
                    n.displayName = jn.value("nm", "");
                    n.color       = jn.value("col", 0xFF80C8FFu);
                    if (jn.contains("pb")) n.params = DecodeBag(jn["pb"]);
                    tr.notifies.push_back(std::move(n));
                }

                if (jtr.contains("s")) for (const auto& js : jtr["s"])
                {
                    NotifyState s;
                    s.id          = js.value("id", 0u);
                    s.category    = static_cast<NotifyCategory>(js.value("cat", 5));
                    s.startTime   = js.value("t0", 0.f);
                    s.endTime     = js.value("t1", 0.1f);
                    s.displayName = js.value("nm", "");
                    s.color       = js.value("col", 0xFF80C8FFu);
                    if (js.contains("pb")) s.params = DecodeBag(js["pb"]);
                    tr.states.push_back(std::move(s));
                }

                outTracks.push_back(std::move(tr));
            }
        }
        // A StateToggle that names no legal tag grants nothing at run time: this is where it is reported.
        for (const std::string& problem : StateToggle::Problems(outTracks))
            LOG_ERROR("[AUTHORING] %s", problem.c_str());
        return true;
    }
} // namespace NotifyIO
