#include "Audio/AudioClipSystem.h"
#include "Audio/AudioClipResource.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <cctype>
#include <filesystem>

namespace Audio
{

std::string AudioClipSystem::NormalizePath(const std::string& path)
{
    namespace fs = std::filesystem;
    std::string norm = Sys::PathToUtf8(Sys::U8Path(path).lexically_normal());
    for (char& c : norm) if (c == '\\') c = '/';
    for (char& c : norm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return norm;
}

void AudioClipSystem::Init(Resource::ResourceManager& rm) { m_rm = &rm; }

void AudioClipSystem::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_rm)
        for (auto& [_, entry] : m_byPath)
            m_rm->Unload(entry.rmHandle);
    m_byPath.clear();
    m_pathBySlot.clear();
    m_rm = nullptr;
}

Resource::AudioHandle AudioClipSystem::AcquireClip(const std::string& path)
{
    // A checked failure, not an assert: in Release m_rm->Load below would dereference null. Shutdown
    // clears m_rm too, so this also catches an acquire after Shutdown. Reported once per process: every
    // later acquire would say the same.
    if (!ENGINE_CHECK_ONCE(m_rm != nullptr,
            "AudioClipSystem::AcquireClip('%s'): Init not called, or called after Shutdown",
            path.c_str()))
        return Resource::kInvalidAudioHandle;
    const std::string norm = NormalizePath(path);

    std::lock_guard<std::mutex> lk(m_mutex);

    if (auto it = m_byPath.find(norm); it != m_byPath.end())
    {
        ++it->second.refCount;
        return it->second.handle;
    }

    const uint32_t        slot     = m_nextSlot++;
    Resource::Handle      rmHandle = m_rm->Load(path, Resource::ResourceType::AudioClip);
    Resource::AudioHandle ah       = Resource::Handle::Make(slot, Resource::ResourceType::AudioClip, 1u);

    ClipEntry entry{};
    entry.handle   = ah;
    entry.rmHandle = rmHandle;
    entry.refCount = 1;
    entry.state    = ClipState::Loading;

    m_byPath[norm]      = entry;
    m_pathBySlot[slot]  = norm;

    LOG_INFO("AudioClipSystem: AcquireClip '%s' slot=%u", path.c_str(), slot);
    return ah;
}

void AudioClipSystem::ReleaseClip(Resource::AudioHandle handle)
{
    if (handle == Resource::kInvalidAudioHandle) return;

    std::lock_guard<std::mutex> lk(m_mutex);

    const uint32_t slot = handle.Index();
    auto pathIt = m_pathBySlot.find(slot);
    if (pathIt == m_pathBySlot.end()) return;

    auto entryIt = m_byPath.find(pathIt->second);
    if (entryIt == m_byPath.end()) return;

    if (--entryIt->second.refCount == 0)
    {
        if (m_rm) m_rm->Unload(entryIt->second.rmHandle);
        m_byPath.erase(entryIt);
        m_pathBySlot.erase(pathIt);
        LOG_INFO("AudioClipSystem: released slot=%u", slot);
    }
}

bool AudioClipSystem::IsReady(Resource::AudioHandle handle) const
{
    if (handle == Resource::kInvalidAudioHandle) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto pathIt = m_pathBySlot.find(handle.Index());
    if (pathIt == m_pathBySlot.end()) return false;
    auto entryIt = m_byPath.find(pathIt->second);
    return entryIt != m_byPath.end()
        && entryIt->second.state == ClipState::Ready;
}

const AudioClipResource* AudioClipSystem::GetResource(Resource::AudioHandle handle) const
{
    if (handle == Resource::kInvalidAudioHandle) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto pathIt = m_pathBySlot.find(handle.Index());
    if (pathIt == m_pathBySlot.end()) return nullptr;
    auto entryIt = m_byPath.find(pathIt->second);
    if (entryIt == m_byPath.end() || entryIt->second.state != ClipState::Ready)
        return nullptr;
    return m_rm->Get<AudioClipResource>(entryIt->second.rmHandle);
}

void AudioClipSystem::Tick()
{
    if (!m_rm) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [path, entry] : m_byPath)
    {
        if (entry.state != ClipState::Loading) continue;
        const Resource::ResourceState rs = m_rm->GetState(entry.rmHandle);
        if (rs == Resource::ResourceState::Ready)
        {
            entry.state = ClipState::Ready;
            LOG_SUCCESS("AudioClipSystem: '%s' ready", path.c_str());
        }
        else if (rs == Resource::ResourceState::Failed)
        {
            // Mark Ready to stop polling; GetResource still returns null since ResourceManager holds no
            // resource (as in AnimationClipSystem).
            entry.state = ClipState::Ready;
            LOG_ERROR("AudioClipSystem: load failed for '%s'", path.c_str());
        }
    }
}

AudioClipSystem::Counts AudioClipSystem::GetCounts() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    Counts c;
    c.clips = static_cast<uint32_t>(m_byPath.size());
    for (const auto& [_, entry] : m_byPath) c.refs += entry.refCount;
    return c;
}

} // namespace Audio
