#include "PostProcess/ProfileSystem.h"
#include "PostProcess/PostProcessProfileSerializer.h"
#include "System/Log.h"

namespace PostProcess
{

using Resource::Handle;
using Resource::ResourceType;

ProfileSystem& ProfileSystem::Get()
{
    static ProfileSystem s_instance;
    return s_instance;
}

void ProfileSystem::ApplyAuthoredTo(PostProcessProfile& p) const
{
    // The five postProcess.* rows (EngineSettingsSchema.inl), as their apply
    // hooks write them.
    p.cas.enabled.Set(m_authored.cas);
    p.dof.enabled.Set(m_authored.dof);
    p.lensFlare.enabled.Set(m_authored.lensFlare);
    p.autoExposure.enabled.Set(m_authored.autoExposure);
    p.colorGrading.enabled.Set(m_authored.colorGrading);
}

uint64_t ProfileSystem::HashPath(const std::string& p)
{
    // FNV-1a 64-bit, matching the other resource systems' path dedup.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : p) { h ^= c; h *= 1099511628211ull; }
    return h;
}

uint32_t ProfileSystem::AllocSlot()
{
    if (!m_freeList.empty())
    {
        const uint32_t idx = m_freeList.back();
        m_freeList.pop_back();
        m_slots[idx].occupied = true;
        m_slots[idx].refCount = 0;
        m_slots[idx].path.clear();
        m_slots[idx].profile = PostProcessProfile{};
        return idx;
    }
    const uint32_t idx = static_cast<uint32_t>(m_slots.size());
    m_slots.emplace_back();
    m_slots[idx].occupied = true;
    return idx;
}

void ProfileSystem::FreeSlot(uint32_t idx)
{
    Slot& s = m_slots[idx];
    s.occupied = false;
    s.refCount = 0;
    s.path.clear();
    s.profile = PostProcessProfile{};
    // Bump generation so stale handles fail validation. Never wrap to 0.
    s.generation = (s.generation % Handle::MAX_GEN) + 1;
    m_freeList.push_back(idx);
}

ProfileSystem::Slot* ProfileSystem::Resolve(ProfileHandle h)
{
    if (!h.IsValid() || h.Type() != ResourceType::PostProcessProfile) return nullptr;
    const uint32_t idx = h.Index();
    if (idx >= m_slots.size()) return nullptr;
    Slot& s = m_slots[idx];
    if (!s.occupied || s.generation != h.Generation()) return nullptr;
    return &s;
}

const ProfileSystem::Slot* ProfileSystem::Resolve(ProfileHandle h) const
{
    return const_cast<ProfileSystem*>(this)->Resolve(h);
}

ProfileHandle ProfileSystem::Acquire(const std::string& path)
{
    if (path.empty()) return {};

    const uint64_t key = HashPath(path);
    if (auto it = m_pathToSlot.find(key); it != m_pathToSlot.end())
    {
        Slot& s = m_slots[it->second];
        if (s.occupied)
        {
            ++s.refCount;
            return Handle::Make(it->second, ResourceType::PostProcessProfile, s.generation);
        }
    }

    const uint32_t idx = AllocSlot();
    Slot& s = m_slots[idx];
    if (!LoadProfile(path, s.profile))
    {
        FreeSlot(idx);
        return {};
    }
    s.path     = path;
    s.refCount = 1;
    m_pathToSlot[key] = idx;
    return Handle::Make(idx, ResourceType::PostProcessProfile, s.generation);
}

ProfileHandle ProfileSystem::CreateRuntime(const PostProcessProfile& profile)
{
    const uint32_t idx = AllocSlot();
    Slot& s = m_slots[idx];
    s.profile  = profile;
    s.refCount = 1;
    return Handle::Make(idx, ResourceType::PostProcessProfile, s.generation);
}

void ProfileSystem::Release(ProfileHandle h)
{
    Slot* s = Resolve(h);
    if (!s) return;
    if (s->refCount > 0) --s->refCount;
    if (s->refCount == 0)
    {
        if (!s->path.empty()) m_pathToSlot.erase(HashPath(s->path));
        FreeSlot(h.Index());
    }
}

PostProcessProfile* ProfileSystem::Get(ProfileHandle h)
{
    Slot* s = Resolve(h);
    return s ? &s->profile : nullptr;
}

const PostProcessProfile* ProfileSystem::Get(ProfileHandle h) const
{
    const Slot* s = Resolve(h);
    return s ? &s->profile : nullptr;
}

const std::string& ProfileSystem::GetPath(ProfileHandle h) const
{
    const Slot* s = Resolve(h);
    return s ? s->path : m_emptyPath;
}

bool ProfileSystem::Save(ProfileHandle h, const std::string& path)
{
    Slot* s = Resolve(h);
    if (!s) return false;
    if (!SaveProfile(s->profile, path)) return false;
    if (s->path != path)
    {
        if (!s->path.empty()) m_pathToSlot.erase(HashPath(s->path));
        s->path = path;
        m_pathToSlot[HashPath(path)] = h.Index();
    }
    return true;
}

void ProfileSystem::Clear()
{
    m_slots.clear();
    m_freeList.clear();
    m_pathToSlot.clear();
}

} // namespace PostProcess
