#pragma once
#include "Anim/Core/AnimPoseSource.h"
#include "Anim/Graph/AnimGraphInstance.h"
#include "ECS/AnimGraphComponent.h"
#include "Resource/ResourceManager.h"
#include "Resource/AnimationClipSystem.h"
#include <chrono>
#include <nlohmann/json_fwd.hpp>
#include "Resource/AnimSetLoader.h"

class AnimationSystem;
namespace GameplayTime { struct Clocks; }
class AnimGraphSystem final : public Anim::IAnimPoseSource {
public:
    AnimGraphSystem(AnimationSystem&, SkeletonRegistry&, ClipLibrary&);
    ~AnimGraphSystem();
    void Init(Resource::ResourceManager& resources) { m_resources = &resources; }
    void SetClipSystem(Resource::AnimationClipSystem* clips) { m_clipSystem = clips; }
    // Each instance's attack clock advances by its entity's GameplayTime::EntityDt of @p clocks.
    void Prepare(World&, const GameplayTime::Clocks& clocks);
    void Publish(World&);
    void ResolvePendingBinds(World&);
    bool OwnsPose(Entity e) const override;
    bool EvaluatePose(Entity, const SkeletonAsset&, Anim::LocalPose*, uint32_t, float) override;
    void CaptureExternalPose(Entity, const Anim::LocalPose*, uint32_t) override;
    void Reset() override;
    Anim::Graph::Instance* GetInstance(Entity) const;
    bool Set(World&, Entity, std::string_view, Anim::Graph::ParamValue);
    Anim::Graph::ParamHandle FindParam(World&,Entity,std::string_view)const;
    bool SetTyped(World&,Entity,Anim::Graph::ParamHandle&,Anim::Graph::ParamType,Anim::Graph::ParamValue);
    bool SetFloat(World& w,Entity e,Anim::Graph::ParamHandle& h,float v){return SetTyped(w,e,h,Anim::Graph::ParamType::Float,Anim::Graph::ParamValue::FromFloat(v));}
    bool SetInt(World& w,Entity e,Anim::Graph::ParamHandle& h,int32_t v){return SetTyped(w,e,h,Anim::Graph::ParamType::Int,Anim::Graph::ParamValue::FromInt(v));}
    bool SetBool(World& w,Entity e,Anim::Graph::ParamHandle& h,bool v){return SetTyped(w,e,h,Anim::Graph::ParamType::Bool,Anim::Graph::ParamValue::FromInt(v));}
    bool SetId(World& w,Entity e,Anim::Graph::ParamHandle& h,std::string_view v){return SetTyped(w,e,h,Anim::Graph::ParamType::Id,Anim::Graph::ParamValue::FromInt(static_cast<int32_t>(Anim::Graph::Hash(v))));}
    bool SetVec(World& w,Entity e,Anim::Graph::ParamHandle& h,DirectX::XMFLOAT3 v){Anim::Graph::ParamValue p;p.vector={v.x,v.y,v.z};return SetTyped(w,e,h,Anim::Graph::ParamType::Vector3,p);}
    bool Trigger(World&,Entity,Anim::Graph::ParamHandle&);
    Anim::Graph::StateInfo GetState(World&,Entity,std::string_view)const;
    bool Trigger(World&, Entity, std::string_view);
    bool Reload(const std::string& path);
    nlohmann::json VerifyLegacy(Entity,int frames,int actors,bool layers);
    void SetPreviewDefinition(const std::string& path, std::shared_ptr<const Anim::Graph::Definition> definition){m_assets[path].definition=std::move(definition);}
    bool Rewind(Entity e,size_t frame){auto* instance=GetInstance(e);return instance && instance->Rewind(m_clips,frame);}
    Anim::Graph::SlotHandle PlaySlot(World&, Entity, const Anim::Graph::SlotRequestDesc&);
    bool StopSlot(World&, Entity, Anim::Graph::SlotHandle, float blendOut);
    Anim::Graph::SlotStatus SlotStatus(World&, Entity, Anim::Graph::SlotHandle) const;
    float SlotTime(World&, Entity, Anim::Graph::SlotHandle) const;
    Anim::Graph::Instance::SlotRecord* ActiveSlot(Entity, std::string_view slot="DefaultSlot") const;
    float ClipDuration(Entity, std::string_view alias) const;
    void ForEachOutput(World&, const std::function<void(Entity, Anim::Graph::Instance&)>&);
    void DrainRetired(World&, const std::function<void(Entity, const Anim::Graph::SampledNotify&)>&);

private:
    struct Entry {
        uint32_t generation = 0, skeleton = ~0u;
        std::string path, setPath;
        std::unique_ptr<Anim::Graph::Instance> instance;
        bool owns = false, warned = false;
        bool suspended = false;
        float previewDt = -1;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        std::unordered_map<std::string, Resource::AnimHandle> directClips;
    };
    struct Asset { Resource::Handle handle; std::shared_ptr<const Anim::Graph::Definition> definition; bool warned = false; };

    struct Retired { Entity entity; uint32_t generation; std::vector<Anim::Graph::SampledNotify> events; };

    struct AnimSetAsset {Resource::Handle handle; std::shared_ptr<const Resource::AnimSetResource> definition;};

    void Release(Entry&);

    AnimationSystem& m_animation;
    SkeletonRegistry& m_skeletons;
    ClipLibrary& m_clips;
    Resource::AnimationClipSystem* m_clipSystem = nullptr;
    Resource::ResourceManager* m_resources = nullptr;
    uint32_t m_epoch = ~0u;
    uint32_t m_instanceGeneration = 0;
    bool m_abDone=false;
    std::unordered_map<Entity, Entry> m_entries;
    std::unordered_map<std::string, Asset> m_assets;

    std::vector<Retired> m_retired;

    std::unordered_map<std::string,AnimSetAsset> m_animSets;
    std::map<std::pair<uint64_t,uint32_t>, std::weak_ptr<const Anim::Graph::BoundGraph>> m_bindings;
    std::unordered_set<std::string> m_parameterWarnings;
};
