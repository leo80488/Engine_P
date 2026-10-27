#include "Resource/AnimationClipSystem.h"
#include "ECS/AnimGraphSystem.h"
#include "ECS/AnimationComponents.h"
#include "ECS/AnimEntityResolve.h"       // Anim::ResolveAnimEntityOrSelf
#include "ECS/CharacterStateComponent.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/LocomotionComponents.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace Resource
{
    // ==== Helpers ====
    static std::string NormalizePathImpl(const std::string& path)
    {
        namespace fs = std::filesystem;
        std::string norm = Sys::PathToUtf8(Sys::U8Path(path).lexically_normal());
        for (char& c : norm) if (c == '\\') c = '/';
        for (char& c : norm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return norm;
    }

    // Which entity owns the AnimationSet / Animator state for `entity`. From a character
    // root (what gameplay holds; it has no SkeletonRef) this walks down to the animated
    // descendant, where CharacterAnimDriver reads the aliases - stopping short would
    // author aliases nothing finds. A non-character caller (a morph-only prop) falls
    // back to itself instead of becoming a no-op.
    static Entity ResolveAnimatorOwner(::World& world, Entity entity)
    {
        return Anim::ResolveAnimEntityOrSelf(world, entity);
    }

    static void ResetAnimatorPlayback(::World& world, Entity entity)
    {
        if (auto* anim = world.GetComponent<AnimationComponent>(entity))
        {
            anim->primaryClip      = kInvalidAnimHandle;
            anim->secondaryClip    = kInvalidAnimHandle;
            anim->primaryTime      = 0.f;
            anim->secondaryTime    = 0.f;
            anim->blendWeight      = 0.f;
            anim->speed            = 1.f;
            anim->looping          = true;
            anim->paused           = false;
            anim->externalTime     = false;
            anim->activeLegDrive   = 0;
            anim->pendingEventMask = 0;
            anim->prevNotifyTime   = -1.f;
            anim->lastNotifyClip   = kInvalidAnimHandle;
        }
        if (auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity))
            *transition = AnimatorTransitionComponent{};
        world.RemoveComponent<AnimatorPlayRequestComponent>(entity);
    }

    // The selector rule lives in AnimationResource.h (Resource::FindSourceClip)
    // so the offline asset tool's `compat` picks the clip playback will.
    template<typename ClipVector>
    static int32_t ResolveSourceClipIndex(const ClipVector& clips,
                                           const std::string& selector)
    {
        return ::Resource::FindSourceClip(clips, selector);   // :: - here `Resource` is the class
    }

    template<typename ClipVector>
    static std::vector<std::string> BuildSourceClipNames(const ClipVector& clips)
    {
        std::vector<std::string> names;
        names.reserve(clips.size());
        for (size_t i = 0; i < clips.size(); ++i)
        {
            bool duplicate = clips[i].name[0] == '\0';
            if (!duplicate)
                for (size_t j = 0; j < clips.size(); ++j)
                    if (i != j && std::strcmp(clips[i].name, clips[j].name) == 0)
                    { duplicate = true; break; }
            names.emplace_back(duplicate
                ? ("#" + std::to_string(i))
                : clips[i].name);
        }
        return names;
    }

    // ==== AnimationClipSystem ====
    void AnimationClipSystem::Init(ResourceManager& rm)
    {
        m_rm = &rm;
    }

    AnimHandle AnimationClipSystem::AcquireClip(const std::string& path)
    {
        // Without Init, m_rm->Load below would dereference null. Reported once per process:
        // the state persists, so every later acquire would say the same.
        if (!ENGINE_CHECK_ONCE(m_rm != nullptr,
                "AnimationClipSystem::AcquireClip('%s'): Init not called", path.c_str()))
            return kInvalidAnimHandle2;
        const std::string norm = NormalizePathImpl(path);

        std::lock_guard<std::mutex> lk(m_mutex);

        auto it = m_byPath.find(norm);
        if (it != m_byPath.end())
        {
            ++it->second.refCount;
            return it->second.handle;
        }

        // New entry: allocate a slot and start the async load.
        const uint32_t slot = m_nextSlot++;
        Handle rmHandle = m_rm->Load(path, ResourceType::Animation);

        AnimHandle ah = Handle::Make(slot, ResourceType::Animation, 1u);

        ClipEntry entry{};
        entry.handle   = ah;
        entry.rmHandle = rmHandle;
        entry.refCount = 1;
        entry.state    = ClipState::Loading;

        m_byPath[norm]    = entry;
        m_pathBySlot[slot] = norm;

        LOG_INFO("AnimationClipSystem: AcquireClip '%s' slot=%u", path.c_str(), slot);
        return ah;
    }

    void AnimationClipSystem::ReleaseClip(AnimHandle handle)
    {
        if (handle == kInvalidAnimHandle2) return;

        std::lock_guard<std::mutex> lk(m_mutex);

        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return;

        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return;

        ClipEntry& entry = entryIt->second;
        if (--entry.refCount == 0)
        {
            if (m_rm) m_rm->Unload(entry.rmHandle);
            m_byPath.erase(entryIt);
            m_pathBySlot.erase(pathIt);
            LOG_INFO("AnimationClipSystem: released slot=%u", slot);
        }
    }

    bool AnimationClipSystem::IsReady(AnimHandle handle) const
    {
        if (handle == kInvalidAnimHandle2) return false;
        std::lock_guard<std::mutex> lk(m_mutex);
        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return false;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return false;
        return entryIt->second.state == ClipState::Ready;
    }

    const AnimationResource* AnimationClipSystem::GetResource(AnimHandle handle) const
    {
        if (handle == kInvalidAnimHandle2) return nullptr;
        std::lock_guard<std::mutex> lk(m_mutex);
        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return nullptr;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end() || entryIt->second.state != ClipState::Ready)
            return nullptr;
        return m_rm->Get<AnimationResource>(entryIt->second.rmHandle);
    }

    AnimationResource* AnimationClipSystem::GetResourceMutable(AnimHandle handle)
    {
        // The resource is owned (non-const) by ResourceManager; GetResource
        // only hands out const for safety. Editor authoring is an explicit
        // opt-in to mutate the asset in memory before re-serializing.
        return const_cast<AnimationResource*>(GetResource(handle));
    }

    void AnimationClipSystem::Tick()
    {
        if (!m_rm) return;
        std::lock_guard<std::mutex> lk(m_mutex);

        for (auto& [path, entry] : m_byPath)
        {
            if (entry.state != ClipState::Loading) continue;
            if (m_rm->GetState(entry.rmHandle) == ResourceState::Ready)
            {
                entry.state = ClipState::Ready;
                LOG_SUCCESS("AnimationClipSystem: '%s' ready", path.c_str());
            }
            else if (m_rm->GetState(entry.rmHandle) == ResourceState::Failed)
            {
                LOG_ERROR("AnimationClipSystem: load failed for '%s'", path.c_str());
                // Marked Ready to stop polling (and logging every Tick); the resource stays null, so every
                // bind sees no data (an AnimationSet alias is marked failed, a deferred slot disabled).
                entry.state = ClipState::Ready;
            }
        }
    }

    bool AnimationClipSystem::QueueAnimatorPlay(::World& world, uint32_t rawEntity,
                                                 const std::string& clipName,
                                                 float blendDuration,
                                                 bool restart)
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity || clipName.empty()) return false;
        const auto* claim=world.GetComponent<AnimPoseOverrideComponent>(entity);
        const auto* playback=world.GetComponent<AnimationComponent>(entity);
        if(m_graph && m_graph->OwnsPose(entity) && !(claim && (claim->layers&AnimPoseOverrideComponent::Base)) && !(playback && playback->externalTime)) {
            const auto* instance=m_graph->GetInstance(entity);
            if(!restart && instance)for(const auto& request:instance->requests)
                if(request.desc.slot=="DefaultSlot" && request.desc.alias==clipName && request.status==Anim::Graph::SlotStatus::Playing)return true;
            Anim::Graph::SlotRequestDesc desc;desc.slot="DefaultSlot";desc.alias=clipName;desc.blendIn=std::max(0.f,blendDuration);desc.interrupt=true;desc.queueUntilReady=true;
            return m_graph->PlaySlot(world,entity,desc).id!=0;
        }

        const auto* set = world.GetComponent<AnimationSetComponent>(entity);
        const auto* ref = set ? set->Find(clipName) : nullptr;
        if (!ref || ref->path.empty()) return false;

        if (!restart)
        {
            if (const auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity))
            {
                if (transition->active && transition->targetClipName == clipName)
                    return true;
                if (!transition->active && transition->currentClipName == clipName)
                {
                    // Same-clip dedupe, EXCEPT a finished one-shot: repeated
                    // ATTACK / TAKE_DAMAGE must retrigger even though the alias
                    // is still "current" (non-looping clips clamp on their last
                    // frame instead of clearing). While the clip is still
                    // advancing (or looping) the call stays a no-op.
                    const auto* anim = world.GetComponent<AnimationComponent>(entity);
                    bool finished = false;
                    if (anim && !anim->looping &&
                        anim->primaryClip != kInvalidAnimHandle)
                    {
                        const EntityHandle owner = world.MakeHandle(entity);
                        const auto bIt = m_animatorBindings.find(
                            AnimatorBindingKey(owner.entity, owner.generation));
                        if (bIt != m_animatorBindings.end())
                        {
                            const auto cIt = bIt->second.clips.find(clipName);
                            if (cIt != bIt->second.clips.end() &&
                                cIt->second.duration > 0.f &&
                                anim->primaryTime >= cIt->second.duration - 1e-4f)
                                finished = true;
                        }
                    }
                    if (!finished) return true;
                    restart = true; // implicit restart of the finished clip
                }
            }
        }

        AnimatorPlayRequestComponent request;
        request.clipName     = clipName;
        request.blendDuration = (std::max)(0.f, blendDuration);
        request.restart       = restart;
        if (auto* existing = world.GetComponent<AnimatorPlayRequestComponent>(entity))
            *existing = std::move(request);
        else
            world.AddComponent<AnimatorPlayRequestComponent>(entity, std::move(request));
        return true;
    }

    void AnimationClipSystem::StopAnimator(::World& world, uint32_t rawEntity)
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return;
        if(m_graph && m_graph->OwnsPose(entity)) {
            if(auto* instance=m_graph->GetInstance(entity))for(const auto& request:instance->requests)
                if(request.status==Anim::Graph::SlotStatus::Pending || request.status==Anim::Graph::SlotStatus::Playing)m_graph->StopSlot(world,entity,request.handle,0);
            return;
        }
        ResetAnimatorPlayback(world, entity);
        if (auto* cs = world.GetComponent<CharacterStateComponent>(entity))
        {
            cs->currentIdx  = -1;
            cs->pendingIdx  = -1;
            cs->blendElapsed = 0.f;
        }
    }

    void AnimationClipSystem::ReleaseAnimatorBindings(uint64_t key)
    {
        auto it = m_animatorBindings.find(key);
        if (it == m_animatorBindings.end()) return;
        for (auto& [name, binding] : it->second.clips)
        {
            (void)name;
            if (binding.handle.IsValid()) ReleaseClip(binding.handle);
        }
        m_animatorBindings.erase(it);
    }

    void AnimationClipSystem::UnassignAnimator(::World& world, uint32_t rawEntity,
                                                bool clearSet)
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return;

        const EntityHandle owner = world.MakeHandle(entity);
        ReleaseAnimatorBindings(AnimatorBindingKey(owner.entity, owner.generation));

        if (auto* pending = world.GetComponent<PendingAnimBind>(entity))
        {
            AnimHandle handle;
            handle.packed = pending->handlePacked;
            world.RemoveComponent<PendingAnimBind>(entity);
            if (handle.IsValid()) ReleaseClip(handle);
        }

        world.RemoveComponent<AnimationSourcePath>(entity); // AnimRef single-clip path, kept only for migration
        ResetAnimatorPlayback(world, entity);
        if (auto* cs = world.GetComponent<CharacterStateComponent>(entity))
        {
            cs->currentIdx   = -1;
            cs->pendingIdx   = -1;
            cs->blendElapsed = 0.f;
        }

        if (auto* morph = world.GetComponent<MorphComponent>(entity))
        {
            morph->primaryMorphClip = kInvalidAnimHandle;
            morph->time             = 0.f;
            morph->count            = 0;
            std::fill_n(morph->weights, MorphComponent::MAX, 0.f);
        }

        if (clearSet)
        {
            if (auto* set = world.GetComponent<AnimationSetComponent>(entity))
            {
                set->clips.clear();
                set->defaultClip.clear();
                set->playDefaultOnLoad = false;
            }
        }
    }

    std::string AnimationClipSystem::GetCurrentAnimatorClip(::World& world,
                                                             uint32_t rawEntity) const
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return {};
        if(m_graph && m_graph->OwnsPose(entity)) {const auto* active=m_graph->ActiveSlot(entity);return active?active->desc.alias:std::string{};}
        const auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity);
        return transition ? transition->currentClipName : std::string{};
    }

    std::string AnimationClipSystem::GetTargetAnimatorClip(::World& world,
                                                            uint32_t rawEntity) const
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return {};
        if(m_graph && m_graph->OwnsPose(entity)) {
            if(const auto* instance=m_graph->GetInstance(entity))for(const auto& request:instance->requests)
                if(request.desc.slot=="DefaultSlot" && request.status==Anim::Graph::SlotStatus::Pending)return request.desc.alias;
            return {};
        }
        const auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity);
        return transition && transition->active
             ? transition->targetClipName : std::string{};
    }

    float AnimationClipSystem::GetAnimatorDuration(::World& world,
                                                    uint32_t rawEntity) const
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return 0.f;
        if(m_graph && m_graph->OwnsPose(entity)) {const auto* active=m_graph->ActiveSlot(entity);return active?m_graph->ClipDuration(entity,active->desc.alias):0.f;}
        const auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity);
        if (!transition || transition->currentClipName.empty()) return 0.f;

        const EntityHandle owner = world.MakeHandle(entity);
        const auto stateIt = m_animatorBindings.find(
            AnimatorBindingKey(owner.entity, owner.generation));
        if (stateIt == m_animatorBindings.end()) return 0.f;
        const auto clipIt = stateIt->second.clips.find(transition->currentClipName);
        return clipIt == stateIt->second.clips.end() ? 0.f : clipIt->second.duration;
    }

    bool AnimationClipSystem::SetAnimatorClip(::World& world, uint32_t rawEntity,
                                               const std::string& alias,
                                               const std::string& path,
                                               bool looping, float speed,
                                               const std::string& sourceClip,
                                               const std::string& sourceMorphClip)
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity || alias.empty() || path.empty()) return false;

        auto* set = world.GetComponent<AnimationSetComponent>(entity);
        if (!set)
        {
            world.AddComponent<AnimationSetComponent>(entity, AnimationSetComponent{});
            set = world.GetComponent<AnimationSetComponent>(entity);
        }
        set->Set(alias, Sys::RelativizeAssetPath(path), looping, speed,
                 sourceClip, sourceMorphClip);
        return true;
    }

    bool AnimationClipSystem::RemoveAnimatorClip(::World& world, uint32_t rawEntity,
                                                  const std::string& alias)
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return false;
        auto* set = world.GetComponent<AnimationSetComponent>(entity);
        if (!set) return false;

        const auto it = std::find_if(set->clips.begin(), set->clips.end(),
            [&alias](const AnimationClipReference& ref) { return ref.name == alias; });
        if (it == set->clips.end()) return false;
        set->clips.erase(it);
        if (set->defaultClip == alias) set->defaultClip.clear();

        const auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity);
        const auto* request = world.GetComponent<AnimatorPlayRequestComponent>(entity);
        if ((transition && (transition->currentClipName == alias ||
                            transition->targetClipName  == alias)) ||
            (request && request->clipName == alias))
            StopAnimator(world, entity);
        return true;
    }

    bool AnimationClipSystem::HasAnimatorClip(::World& world, uint32_t rawEntity,
                                               const std::string& alias) const
    {
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return false;
        const auto* set = world.GetComponent<AnimationSetComponent>(entity);
        return set && set->Find(alias);
    }

    std::vector<std::string> AnimationClipSystem::GetAnimatorClipAliases(
        ::World& world, uint32_t rawEntity) const
    {
        std::vector<std::string> aliases;
        const Entity entity = ResolveAnimatorOwner(world, static_cast<Entity>(rawEntity));
        if (entity == NullEntity) return aliases;
        const auto* set = world.GetComponent<AnimationSetComponent>(entity);
        if (!set) return aliases;
        aliases.reserve(set->clips.size());
        for (const auto& ref : set->clips)
            aliases.push_back(ref.name);
        return aliases;
    }

    std::vector<std::string> AnimationClipSystem::GetBoneClipNames(
        const std::string& path) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto entryIt = m_byPath.find(NormalizePathImpl(path));
        if (entryIt == m_byPath.end() || entryIt->second.state != ClipState::Ready || !m_rm)
            return {};
        const auto* resource = m_rm->Get<AnimationResource>(entryIt->second.rmHandle);
        return resource ? BuildSourceClipNames(resource->clips) : std::vector<std::string>{};
    }

    std::vector<std::string> AnimationClipSystem::GetMorphClipNames(
        const std::string& path) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto entryIt = m_byPath.find(NormalizePathImpl(path));
        if (entryIt == m_byPath.end() || entryIt->second.state != ClipState::Ready || !m_rm)
            return {};
        const auto* resource = m_rm->Get<AnimationResource>(entryIt->second.rmHandle);
        return resource ? BuildSourceClipNames(resource->morphClips)
                        : std::vector<std::string>{};
    }

    bool AnimationClipSystem::GetBoundAnimatorClip(::World& world, uint32_t entity,
        const std::string& alias, uint32_t& clip, bool& failed) const {
        const auto state = m_animatorBindings.find(AnimatorBindingKey(entity, world.GetGeneration(entity)));
        if (state == m_animatorBindings.end()) return false;
        const auto binding = state->second.clips.find(alias);
        if (binding == state->second.clips.end()) return false;
        clip = binding->second.clipIndex; failed = binding->second.failed;
        return failed || clip != kInvalidClipIndex;
    }

    void AnimationClipSystem::OnWorldClear(::World& world)
    {
        std::vector<uint64_t> bindingKeys;
        bindingKeys.reserve(m_animatorBindings.size());
        for (const auto& [key, unused] : m_animatorBindings)
        {
            (void)unused;
            bindingKeys.push_back(key);
        }
        for (uint64_t key : bindingKeys) ReleaseAnimatorBindings(key);

        if (auto* pool = world.GetPool<PendingAnimBind>())
        {
            for (auto& pending : pool->Data())
            {
                AnimHandle handle{};
                handle.packed = pending.handlePacked;
                if (handle.IsValid()) ReleaseClip(handle);
                pending.handlePacked = 0;
            }
        }
        if (auto* pool = world.GetPool<CharacterStateComponent>())
        {
            for (auto& states : pool->Data())
                for (auto& state : states.states)
                {
                    if (state.clipResHandle.IsValid()) ReleaseClip(state.clipResHandle);
                    state.clipResHandle = {};
                }
        }
        if (auto* pool = world.GetPool<LocomotionComponent>())
        {
            for (auto& locomotion : pool->Data())
                for (int i = 0; i < locomotion.sampleCount &&
                                i < static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++i)
                {
                    AnimHandle handle{};
                    handle.packed = locomotion.samples[i].pendingPacked;
                    if (handle.IsValid()) ReleaseClip(handle);
                    locomotion.samples[i].pendingPacked = 0;
                }
        }
        if (auto* pool = world.GetPool<AnimLayerComponent>())
        {
            for (auto& layers : pool->Data())
                for (int i = 0; i < layers.layerCount && i < AnimLayerComponent::MAX_LAYERS; ++i)
                {
                    AnimHandle handle{};
                    handle.packed = layers.layers[i].pendingPacked;
                    if (handle.IsValid()) ReleaseClip(handle);
                    layers.layers[i].pendingPacked = 0;
                }
        }
        if (auto* pool = world.GetPool<LocomotionTransitionComponent>())
        {
            for (auto& tc : pool->Data())
                for (int i = 0; i < tc.count &&
                                i < LocomotionTransitionComponent::MAX_TRANSITIONS; ++i)
                {
                    AnimHandle handle{};
                    handle.packed = tc.transitions[i].pendingPacked;
                    if (handle.IsValid()) ReleaseClip(handle);
                    tc.transitions[i].pendingPacked = 0;
                }
        }
    }

    void AnimationClipSystem::ResolveAnimationSets(::World& world, ::Renderer& renderer)
    {
        // One-way runtime migration of CharacterStates-only scenes / prefabs: keep the
        // Character.* data, but mirror missing aliases into the AnimationSet so Lua state
        // changes use the same Animator request / cross-fade path.
        if (auto* legacyPool = world.GetPool<CharacterStateComponent>())
        {
            const auto& legacyEntities = legacyPool->Entities();
            auto& legacyStates = legacyPool->Data();
            for (size_t i = 0; i < legacyStates.size(); ++i)
            {
                CharacterStateComponent& legacyComp = legacyStates[i];
                if (legacyComp.animatorMigrationComplete) continue;
                const Entity entity = ResolveAnimatorOwner(world, legacyEntities[i]);
                if (entity == NullEntity) continue;

                std::string resumeAlias;
                if (legacyComp.pendingIdx >= 0 &&
                    static_cast<size_t>(legacyComp.pendingIdx) < legacyComp.states.size())
                    resumeAlias = legacyComp.states[legacyComp.pendingIdx].name;
                else if (legacyComp.currentIdx >= 0 &&
                         static_cast<size_t>(legacyComp.currentIdx) < legacyComp.states.size())
                    resumeAlias = legacyComp.states[legacyComp.currentIdx].name;

                if (!legacyComp.states.empty())
                {
                    auto* set = world.GetComponent<AnimationSetComponent>(entity);
                    if (!set)
                    {
                        world.AddComponent<AnimationSetComponent>(
                            entity, AnimationSetComponent{});
                        set = world.GetComponent<AnimationSetComponent>(entity);
                    }
                    for (const auto& legacy : legacyComp.states)
                        if (!legacy.name.empty() && !legacy.clipPath.empty() &&
                            !set->Find(legacy.name))
                            set->Set(legacy.name, legacy.clipPath, legacy.looping,
                                     legacy.playbackSpeed);
                }

                legacyComp.currentIdx = legacyComp.pendingIdx = -1;
                legacyComp.blendElapsed = 0.f;
                legacyComp.animatorMigrationComplete = true;
                if (!resumeAlias.empty())
                    QueueAnimatorPlay(world, entity, resumeAlias, 0.f, true);
            }
        }

        // Release resources for destroyed/recycled entities and removed sets.
        std::vector<uint64_t> staleKeys;
        staleKeys.reserve(m_animatorBindings.size());
        for (const auto& [key, state] : m_animatorBindings)
        {
            const EntityHandle owner{ state.entity, state.generation };
            if (!world.IsHandleValid(owner) ||
                !world.HasComponent<AnimationSetComponent>(state.entity))
                staleKeys.push_back(key);
        }
        for (uint64_t key : staleKeys) ReleaseAnimatorBindings(key);

        auto* setPool = world.GetPool<AnimationSetComponent>();
        if (!setPool) return;

        const auto& entities = setPool->Entities();
        auto&       sets     = setPool->Data();
        for (size_t i = 0; i < sets.size(); ++i)
        {
            const Entity entity = entities[i];
            if (!world.IsAlive(entity)) continue;

            const auto* skeleton = world.GetComponent<SkeletonComponent>(entity);
            if (!skeleton || skeleton->assetIndex == kInvalidAnimHandle ||
                skeleton->assetIndex >= renderer.GetSkeletonRegistry().Count())
                continue;

            AnimationSetComponent& set = sets[i];
            const EntityHandle owner = world.MakeHandle(entity);
            const uint64_t key = AnimatorBindingKey(owner.entity, owner.generation);
            AnimatorEntityBindings& state = m_animatorBindings[key];
            state.entity     = owner.entity;
            state.generation = owner.generation;

            if (state.defaultClip != set.defaultClip)
            {
                state.defaultClip   = set.defaultClip;
                state.defaultQueued = false;
            }

            // Remove deleted/path-changed aliases and invalidate selector-only
            // changes. Selector changes reuse the same acquired resource and
            // per-skeleton bind cache instead of appending duplicate clips.
            std::vector<std::string> requeue;
            for (auto it = state.clips.begin(); it != state.clips.end(); )
            {
                const AnimationClipReference* desired = set.Find(it->first);
                const bool removed = !desired || desired->path.empty();
                const bool pathChanged = !removed &&
                    NormalizePathImpl(desired->path) != NormalizePathImpl(it->second.path);
                const bool selectorChanged = !removed && !pathChanged &&
                    (desired->sourceClip      != it->second.sourceClip ||
                     desired->sourceMorphClip != it->second.sourceMorphClip);
                if (!removed && !pathChanged && !selectorChanged) { ++it; continue; }

                bool wasActive = false;
                if (const auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity))
                    wasActive = transition->currentClipName == it->first ||
                                transition->targetClipName  == it->first;

                const std::string alias = it->first;
                if (removed || pathChanged)
                {
                    if (it->second.handle.IsValid()) ReleaseClip(it->second.handle);
                    it = state.clips.erase(it);
                }
                else
                {
                    it->second.sourceClip      = desired->sourceClip;
                    it->second.sourceMorphClip = desired->sourceMorphClip;
                    it->second.clipIndex       = kInvalidClipIndex;
                    it->second.morphIndex      = kInvalidClipIndex;
                    it->second.duration        = 0.f;
                    it->second.failed          = false;
                    ++it;
                }
                if (wasActive)
                {
                    ResetAnimatorPlayback(world, entity);
                    if (!removed) requeue.push_back(alias);
                }
            }

            const SkeletonAsset& skeletonAsset =
                renderer.GetSkeletonRegistry().Get(skeleton->assetIndex);

            // Acquire and bind every authored alias. Duplicate names are
            // ignored after the first entry; the editor prevents creating them.
            std::unordered_set<std::string> visitedNames;
            for (const AnimationClipReference& ref : set.clips)
            {
                if (ref.name.empty() || ref.path.empty()) continue;
                if (!visitedNames.insert(ref.name).second)
                {
                    LOG_WARNING("AnimationSet: duplicate alias '%s' on entity %u",
                                ref.name.c_str(), entity);
                    continue;
                }

                auto bindingIt = state.clips.find(ref.name);
                if (bindingIt == state.clips.end())
                {
                    BoundAnimatorClip binding;
                    binding.path            = ref.path;
                    binding.sourceClip      = ref.sourceClip;
                    binding.sourceMorphClip = ref.sourceMorphClip;
                    binding.handle          = AcquireClip(ref.path);
                    binding.authoredLooping = ref.looping;
                    binding.authoredSpeed   = ref.speed;
                    bindingIt = state.clips.emplace(ref.name, std::move(binding)).first;
                }

                BoundAnimatorClip& binding = bindingIt->second;
                // Live-sync authored looping / speed edits (editor tweaks while a clip plays)
                // exactly once per change. Re-applying them every frame would stomp runtime
                // overrides such as Animator.SetSpeed; the play-request consumer applies the
                // authored values at play time, so an unchanged reference needs no re-assert.
                if (binding.authoredLooping != ref.looping ||
                    binding.authoredSpeed   != ref.speed)
                {
                    binding.authoredLooping = ref.looping;
                    binding.authoredSpeed   = ref.speed;
                    if (auto* transition = world.GetComponent<AnimatorTransitionComponent>(entity))
                    {
                        if (transition->currentClipName == ref.name)
                        {
                            if (auto* playback = world.GetComponent<AnimationComponent>(entity))
                            {
                                playback->looping = ref.looping;
                                playback->speed   = ref.speed;
                            }
                        }
                        if (transition->active && transition->targetClipName == ref.name)
                        {
                            transition->targetLooping = ref.looping;
                            transition->targetSpeed   = ref.speed;
                        }
                    }
                }
                if (binding.failed || binding.clipIndex != kInvalidClipIndex ||
                    !IsReady(binding.handle))
                    continue;

                const AnimationResource* resource = GetResource(binding.handle);
                if (!resource)
                {
                    binding.failed = true;
                    LOG_ERROR("AnimationSet: failed to load alias '%s' path '%s'",
                              ref.name.c_str(), ref.path.c_str());
                    continue;
                }

                const int32_t sourceIndex = ResolveSourceClipIndex(
                    resource->clips, ref.sourceClip);
                if (sourceIndex < 0)
                {
                    binding.failed = true;
                    LOG_ERROR("AnimationSet: alias '%s' source clip '%s' not found in '%s'",
                              ref.name.c_str(), ref.sourceClip.c_str(), ref.path.c_str());
                    continue;
                }

                const uint32_t firstClip = BindToSkeleton(
                    binding.handle, skeletonAsset, renderer.GetClipLibrary());
                if (firstClip == kInvalidClipIndex)
                {
                    binding.failed = true;
                    LOG_ERROR("AnimationSet: alias '%s' has no bone clip ('%s')",
                              ref.name.c_str(), ref.path.c_str());
                    continue;
                }
                binding.clipIndex = firstClip + static_cast<uint32_t>(sourceIndex);
                binding.duration  = resource->clips[sourceIndex].duration;

                if (!resource->morphClips.empty())
                {
                    int32_t morphSourceIndex = -1;
                    if (!ref.sourceMorphClip.empty())
                    {
                        morphSourceIndex = ResolveSourceClipIndex(
                            resource->morphClips, ref.sourceMorphClip);
                    }
                    else
                    {
                        // Pair by the selected bone clip's name when possible.
                        const std::string boneName = resource->clips[sourceIndex].name;
                        morphSourceIndex = boneName.empty()
                            ? 0 : ResolveSourceClipIndex(resource->morphClips, boneName);
                        if (morphSourceIndex < 0) morphSourceIndex = 0;
                    }

                    if (morphSourceIndex < 0)
                    {
                        LOG_WARNING("AnimationSet: alias '%s' morph clip '%s' not found in '%s'",
                                    ref.name.c_str(), ref.sourceMorphClip.c_str(), ref.path.c_str());
                    }
                    else
                    {
                        const uint32_t firstMorph = BindMorphClip(
                            binding.handle, renderer.GetMorphClipLibrary());
                        if (firstMorph != kInvalidClipIndex)
                            binding.morphIndex = firstMorph +
                                static_cast<uint32_t>(morphSourceIndex);
                    }
                }
            }

            for (const std::string& alias : requeue)
                QueueAnimatorPlay(world, entity, alias, 0.f, true);

            if (set.playDefaultOnLoad && !set.defaultClip.empty() &&
                !state.defaultQueued)
            {
                state.defaultQueued = true;
                const auto* anim = world.GetComponent<AnimationComponent>(entity);
                if (!anim || anim->primaryClip == kInvalidAnimHandle)
                    QueueAnimatorPlay(world, entity, set.defaultClip, 0.f, true);
            }

            auto* request = world.GetComponent<AnimatorPlayRequestComponent>(entity);
            if (!request) continue;

            const AnimationClipReference* ref = set.Find(request->clipName);
            auto bindingIt = state.clips.find(request->clipName);
            if (!ref || bindingIt == state.clips.end())
            {
                LOG_WARNING("Animator.Play: alias '%s' not found on entity %u",
                            request->clipName.c_str(), entity);
                world.RemoveComponent<AnimatorPlayRequestComponent>(entity);
                continue;
            }

            BoundAnimatorClip& binding = bindingIt->second;
            if (binding.failed)
            {
                world.RemoveComponent<AnimatorPlayRequestComponent>(entity);
                continue;
            }
            if (binding.clipIndex == kInvalidClipIndex) continue; // still loading

            AnimationComponent* anim = world.GetComponent<AnimationComponent>(entity);
            if (!anim)
            {
                world.AddComponent<AnimationComponent>(entity, AnimationComponent{});
                anim = world.GetComponent<AnimationComponent>(entity);
            }
            AnimatorTransitionComponent* transition =
                world.GetComponent<AnimatorTransitionComponent>(entity);
            if (!transition)
            {
                world.AddComponent<AnimatorTransitionComponent>(
                    entity, AnimatorTransitionComponent{});
                transition = world.GetComponent<AnimatorTransitionComponent>(entity);
            }

            const std::string requestedName = request->clipName;
            const float blendDuration = request->blendDuration;
            const bool restart = request->restart;

            if (!restart && !transition->active &&
                transition->currentClipName == requestedName)
            {
                world.RemoveComponent<AnimatorPlayRequestComponent>(entity);
                continue;
            }

            if (anim->primaryClip == kInvalidAnimHandle || blendDuration <= 1e-4f ||
                transition->currentClipName == requestedName)
            {
                anim->primaryClip   = binding.clipIndex;
                if (restart || transition->currentClipName != requestedName)
                    anim->primaryTime = 0.f;
                anim->secondaryClip = kInvalidAnimHandle;
                anim->secondaryTime = 0.f;
                anim->blendWeight   = 0.f;
                anim->looping       = ref->looping;
                anim->speed         = ref->speed;

                transition->currentClipName = requestedName;
                transition->targetClipName.clear();
                transition->duration = 0.f;
                transition->elapsed  = 0.f;
                transition->active   = false;
            }
            else
            {
                anim->secondaryClip = binding.clipIndex;
                anim->secondaryTime = 0.f;
                anim->blendWeight   = 0.f;

                transition->targetClipName = requestedName;
                transition->duration       = blendDuration;
                transition->elapsed        = 0.f;
                transition->targetSpeed    = ref->speed;
                transition->targetLooping  = ref->looping;
                transition->active         = true;
            }

            if (auto* morph = world.GetComponent<MorphComponent>(entity))
            {
                if (morph->primaryMorphClip != binding.morphIndex)
                    morph->time = 0.f;
                morph->primaryMorphClip = binding.morphIndex;
                if (binding.morphIndex == kInvalidClipIndex)
                {
                    morph->count = 0;
                    std::fill_n(morph->weights, MorphComponent::MAX, 0.f);
                }
            }
            else if (binding.morphIndex != kInvalidClipIndex)
            {
                MorphComponent newMorph{};
                newMorph.primaryMorphClip = binding.morphIndex;
                world.AddComponent<MorphComponent>(entity, newMorph);
            }

            // The Animator is the playback authority: keep the CharacterState definitions, but
            // cancel the active state so it cannot overwrite the requested clip next frame.
            if (auto* cs = world.GetComponent<CharacterStateComponent>(entity))
            {
                cs->currentIdx   = -1;
                cs->pendingIdx   = -1;
                cs->blendElapsed = 0.f;
            }
            world.RemoveComponent<AnimatorPlayRequestComponent>(entity);
        }
    }

    void AnimationClipSystem::ResolvePendingBinds(::World& world, ::Renderer& renderer)
    {
        // Walk the PendingAnimBind pool, not every entity: the pool is normally empty or
        // absent (deferred binds exist only while an .ianim loads), and a lookup per entity
        // per frame is expensive on 15,000+ entity scenes.
        //
        // The entity list is snapshotted because the body can AddComponent (MorphComponent);
        // removals are deferred to `resolved`. It is almost always 0-2 entries.
        //
        // The empty-pool check guards this block only. Do not early-return from the
        // function: the flows after it (locomotion samples, anim layers,
        // ResolveAnimationSets) must run every frame, or every character stops animating.
        if (auto* pendPool = world.GetPool<PendingAnimBind>();
            pendPool && !pendPool->Entities().empty())
        {
        const std::vector<Entity> pendingEnts(pendPool->Entities().begin(),
                                              pendPool->Entities().end());

        std::vector<std::pair<Entity, AnimHandle>> resolved;

        for (Entity e : pendingEnts)
        {
            if (!world.IsAlive(e)) continue;
            auto* pending = world.GetComponent<PendingAnimBind>(e);
            if (!pending) continue;

            // Reconstruct AnimHandle from stored packed value
            AnimHandle ah;
            ah.packed = pending->handlePacked;
            if (!IsReady(ah)) continue; // still loading

            auto* skelComp = world.GetComponent<SkeletonComponent>(e);
            if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle ||
                skelComp->assetIndex >= renderer.GetSkeletonRegistry().Count())
            { resolved.push_back({ e, ah }); continue; }

            const SkeletonAsset& skel = renderer.GetSkeletonRegistry().Get(skelComp->assetIndex);
            uint32_t clipIdx = BindToSkeleton(ah, skel, renderer.GetClipLibrary());
            if (clipIdx != kInvalidClipIndex)
            {
                auto* animComp = world.GetComponent<AnimationComponent>(e);
                if (animComp) animComp->primaryClip = clipIdx;
            }

            uint32_t morphIdx = BindMorphClip(ah, renderer.GetMorphClipLibrary());
            if (morphIdx != kInvalidClipIndex)
            {
                auto* morphComp = world.GetComponent<MorphComponent>(e);
                if (!morphComp)
                { MorphComponent mc{}; mc.primaryMorphClip = morphIdx; world.AddComponent(e, mc); }
                else morphComp->primaryMorphClip = morphIdx;
            }

            LOG_SUCCESS("AnimationClipSystem: deferred bind resolved for entity %u ('%s')",
                        e, pending->animPath.c_str());
            resolved.push_back({ e, ah });
        }

        for (const auto& [e, handle] : resolved)
        {
            world.RemoveComponent<PendingAnimBind>(e);
            // ClipLibrary / MorphClipLibrary own copies of the runtime data after the bind,
            // so the deferred request releases its resource.
            if (handle.IsValid()) ReleaseClip(handle);
        }
        }   // end of the PendingAnimBind block - the rest of the function always runs

        // Locomotion samples, anim layers and transitions: path -> ClipLibrary index.
        // Same deferred flow as PendingAnimBind, but the slots live inside the components
        // (a blend space references several clips at once).
        auto resolveSlot = [&](Entity e, std::string& path,
                               uint32_t& clip, uint32_t& pendingPacked)
        {
            if (clip != ~0u || path.empty()) return;
            if (pendingPacked == 0)
            {
                AnimHandle ah = AcquireClip(path);
                pendingPacked = ah.packed;
                return;
            }
            AnimHandle ah; ah.packed = pendingPacked;
            if (!IsReady(ah)) return;

            auto* skelComp = world.GetComponent<SkeletonComponent>(e);
            if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle ||
                skelComp->assetIndex >= renderer.GetSkeletonRegistry().Count())
                return; // keep waiting for the skeleton
            const SkeletonAsset& skel =
                renderer.GetSkeletonRegistry().Get(skelComp->assetIndex);

            const uint32_t idx = BindToSkeleton(ah, skel, renderer.GetClipLibrary());
            if (idx != kInvalidClipIndex)
            {
                clip = idx;
                LOG_INFO("AnimationClipSystem: bound '%s' -> ClipLibrary[%u] (entity %u)",
                         path.c_str(), idx, e);
                ReleaseClip(ah);
                pendingPacked = 0;
            }
            else
            {
                LOG_ERROR("AnimationClipSystem: '%s' has no bone clips — slot disabled", path.c_str());
                ReleaseClip(ah);
                path.clear(); // stop retrying a bad asset
                pendingPacked = 0;
            }
        };

        if (auto* pLoco = world.GetPool<LocomotionComponent>())
        {
            const auto& ents = pLoco->Entities();
            for (size_t i = 0; i < pLoco->Data().size(); ++i)
            {
                LocomotionComponent& lc = pLoco->Data()[i];
                for (int s = 0; s < lc.sampleCount &&
                                s < static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++s)
                    resolveSlot(ents[i], lc.samples[s].animPath,
                                lc.samples[s].clip, lc.samples[s].pendingPacked);
            }
        }
        if (auto* pLayer = world.GetPool<AnimLayerComponent>())
        {
            const auto& ents = pLayer->Entities();
            for (size_t i = 0; i < pLayer->Data().size(); ++i)
            {
                AnimLayerComponent& lc = pLayer->Data()[i];
                for (int s = 0; s < lc.layerCount && s < AnimLayerComponent::MAX_LAYERS; ++s)
                    resolveSlot(ents[i], lc.layers[s].animPath,
                                lc.layers[s].clip, lc.layers[s].pendingPacked);
            }
        }
        if (auto* pTrans = world.GetPool<LocomotionTransitionComponent>())
        {
            const auto& ents = pTrans->Entities();
            for (size_t i = 0; i < pTrans->Data().size(); ++i)
            {
                LocomotionTransitionComponent& tc = pTrans->Data()[i];
                for (int s = 0; s < tc.count &&
                                s < LocomotionTransitionComponent::MAX_TRANSITIONS; ++s)
                    resolveSlot(ents[i], tc.transitions[s].animPath,
                                tc.transitions[s].clip,
                                tc.transitions[s].pendingPacked);
            }
        }

        ResolveAnimationSets(world, renderer);
        if (auto* graph = renderer.GetAnimGraphSystem()) graph->ResolvePendingBinds(world);
    }

    uint32_t AnimationClipSystem::BindToSkeleton(AnimHandle           handle,
                                                   const SkeletonAsset& skeleton,
                                                   ClipLibrary&         clipLib)
    {
        if (!IsReady(handle)) return kInvalidClipIndex;

        std::lock_guard<std::mutex> lk(m_mutex);

        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return kInvalidClipIndex;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return kInvalidClipIndex;

        ClipEntry& entry = entryIt->second;

        // Per-skeleton cache: different skeletons need different bound clips
        // because bone ordering differs between models.
        const uint32_t skelBoneCount = skeleton.boneCount;
        // Skeleton identity key = FNV-32 over the bone count and every bone name: stable
        // per skeleton, and distinct rigs with the same bone count do not collide (one clip
        // can be bound to several humanoid rigs).
        uint32_t skelKey = 2166136261u;
        auto mixByte = [&skelKey](uint8_t b) { skelKey = (skelKey ^ b) * 16777619u; };
        for (int i = 0; i < 4; ++i) mixByte(static_cast<uint8_t>((skelBoneCount >> (i * 8)) & 0xFF));
        for (uint32_t bi = 0; bi < skelBoneCount; ++bi)
        {
            for (const char* p = skeleton.boneNames[bi]; *p; ++p)
                mixByte(static_cast<uint8_t>(*p));
            mixByte(0xFF); // name separator
        }

        auto cacheIt = entry.perSkelCache.find(skelKey);
        if (cacheIt != entry.perSkelCache.end())
            return cacheIt->second;

        const AnimationResource* res = m_rm->Get<AnimationResource>(entry.rmHandle);
        if (!res || res->clips.empty()) return kInvalidClipIndex;

        const uint32_t firstIdx = clipLib.Count();
        for (const AnimClipData& acd : res->clips)
        {
            uint32_t boundBones = 0;
            ClipAsset bound = acd.BindToSkeleton(skeleton, &boundBones);
            clipLib.Register(std::move(bound));
            // boundBones == 0: no channel names a bone of this skeleton, so the
            // clip plays the REST POSE - the binding itself never fails. INFO,
            // not a warning (content packs budget warnings); the offline check
            // is `tools/asset_tool.py compat`.
            LOG_INFO("AnimationClipSystem: bound clip '%s' → ClipLibrary[%u] (skelBones=%u, channels=%zu, "
                     "boundBones=%u%s)",
                     acd.name, clipLib.Count() - 1, skelBoneCount, acd.channels.size(), boundBones,
                     boundBones == 0 ? " - NO channel matches a bone: it plays the rest pose" : "");
        }

        entry.perSkelCache[skelKey] = firstIdx;
        if (entry.firstClipLibIdx == kInvalidClipIndex)
            entry.firstClipLibIdx = firstIdx;
        return firstIdx;
    }

    uint32_t AnimationClipSystem::BindMorphClip(AnimHandle        handle,
                                                  MorphClipLibrary& morphLib)
    {
        if (!IsReady(handle)) return kInvalidClipIndex;

        std::lock_guard<std::mutex> lk(m_mutex);

        const uint32_t slot = handle.Index();
        auto pathIt = m_pathBySlot.find(slot);
        if (pathIt == m_pathBySlot.end()) return kInvalidClipIndex;
        auto entryIt = m_byPath.find(pathIt->second);
        if (entryIt == m_byPath.end()) return kInvalidClipIndex;

        ClipEntry& entry = entryIt->second;

        // Already bound: return the cached first index.
        if (entry.firstMorphLibIdx != kInvalidClipIndex)
            return entry.firstMorphLibIdx;

        const AnimationResource* res = m_rm->Get<AnimationResource>(entry.rmHandle);
        if (!res || res->morphClips.empty()) return kInvalidClipIndex;

        const uint32_t firstIdx = morphLib.Count();
        for (const MorphClipData& mcd : res->morphClips)
        {
            MorphClipAsset asset = mcd.ToRuntimeClip();
            morphLib.Register(std::move(asset));
            LOG_INFO("AnimationClipSystem: bound morph clip '%s' → MorphClipLibrary[%u]",
                     mcd.name, morphLib.Count() - 1);
        }

        entry.firstMorphLibIdx = firstIdx;
        return firstIdx;
    }

    void AnimationClipSystem::Shutdown()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_animatorBindings.clear();
        if (m_rm)
        {
            for (auto& [path, entry] : m_byPath)
                m_rm->Unload(entry.rmHandle);
        }
        m_byPath.clear();
        m_pathBySlot.clear();
    }

    std::string AnimationClipSystem::NormalizePath(const std::string& path)
    {
        return NormalizePathImpl(path);
    }

} // namespace Resource
