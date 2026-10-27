#pragma once
#include "Resource/IResourceLoader.h"
#include <nlohmann/json.hpp>
#include <map>
#include <cmath>
namespace Resource {
struct AnimSetResource final : Resource {
    struct Clip {std::string path,sourceClip;bool looping=true;float rate=1;};
    std::map<std::string,Clip> slots;
    std::string error;
};
class AnimSetLoader final : public IResourceLoader {
public:
    const char* GetExtension()const override{return ".ianimset";}
    LoadResult Load(const std::string&,std::span<const std::uint8_t> bytes)override {
        auto result=std::make_unique<AnimSetResource>();
        try {
            const auto doc=nlohmann::json::parse(bytes.begin(),bytes.end());
            if(doc.value("schema",0)!=1)throw std::runtime_error("Expected AnimSet schema 1");
            for(auto it=doc.at("slots").begin();it!=doc.at("slots").end();++it) {
                AnimSetResource::Clip clip;
                if(it.value().is_string())clip.path=it.value().get<std::string>();
                else {clip.path=it.value().at("path");clip.sourceClip=it.value().value("sourceClip","");clip.looping=it.value().value("loop",true);clip.rate=it.value().value("rate",1.f);}
                if(it.key().empty() || clip.path.empty() || !std::isfinite(clip.rate))throw std::runtime_error("Invalid AnimSet slot");
                result->slots.emplace(it.key(),std::move(clip));
            }
        }catch(const std::exception& e){result->error=e.what();result->slots.clear();}
        return {std::move(result),nullptr};
    }
};
}
