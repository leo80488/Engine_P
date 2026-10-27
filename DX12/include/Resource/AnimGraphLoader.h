#pragma once
#include "Resource/IResourceLoader.h"
#include "Anim/Graph/AnimGraphCompiler.h"
#include "Resource/AssetFS.h"
namespace Resource {
struct AnimGraphResource final : Resource { Anim::Graph::CompileResult compiled; };
class AnimGraphLoader final : public IResourceLoader {
public:
    const char* GetExtension() const override { return ".ianimgraph"; }
    LoadResult Load(const std::string&, std::span<const std::uint8_t> bytes) override {
        auto resource = std::make_unique<AnimGraphResource>();
        resource->compiled = Anim::Graph::CompileText({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, [](const std::string& path){std::string text;if(!AssetFS::Get().ReadFileText(path,text))throw std::runtime_error("SubGraph not found: "+path);return nlohmann::json::parse(text);});
        return {std::move(resource), nullptr};
    }
};
}
