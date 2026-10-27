#pragma once
#include "Anim/Core/ClipTracks.h"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <stdexcept>
namespace Resource {
inline std::string EncodeAnimTracks(const std::vector<Anim::RootMotionKey>& root,const std::vector<Anim::FloatCurve>& curves) {
    nlohmann::json j={{"schema",1},{"root",nlohmann::json::array()},{"curves",nlohmann::json::array()}};
    for(auto& k:root)j["root"].push_back({k.time,k.position.x,k.position.y,k.position.z,k.yaw});
    for(auto& c:curves){nlohmann::json keys=nlohmann::json::array();for(auto& k:c.keys)keys.push_back({k.time,k.value});j["curves"].push_back({{"name",c.name},{"keys",keys}});}return j.dump();
}
inline bool DecodeAnimTracks(std::string_view text,float duration,std::vector<Anim::RootMotionKey>& root,std::vector<Anim::FloatCurve>& curves) {
    try {auto j=nlohmann::json::parse(text);if(j.at("schema")!=1 || j.at("root").size()>1000000 || j.at("curves").size()>4096)return false;
        std::vector<Anim::RootMotionKey> r;std::vector<Anim::FloatCurve> c;size_t count=0;float previous=-1;
        auto finite=[](const auto& a){for(const auto& v:a)if(!v.is_number() || !std::isfinite(v.template get<float>()))return false;return true;};
        for(auto& k:j.at("root")){if(!k.is_array() || k.size()!=5 || !finite(k))return false;float t=k[0];if(t<0 || t<=previous || t>duration+1e-4f)return false;previous=t;r.push_back({t,{k[1],k[2],k[3]},k[4]});}
        std::unordered_set<std::string> names;
        for(auto& v:j.at("curves")){Anim::FloatCurve curve;curve.name=v.at("name");if(curve.name.empty() || !names.insert(curve.name).second)return false;previous=-1;
            for(auto& k:v.at("keys")){if(++count>1000000 || !k.is_array() || k.size()!=2 || !finite(k))return false;float t=k[0];if(t<0 || t<=previous || t>duration+1e-4f)return false;previous=t;curve.keys.push_back({t,k[1]});}c.push_back(std::move(curve));}
        root=std::move(r);curves=std::move(c);return true;
    }catch(...){return false;}
}
}
