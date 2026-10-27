#include "Resource/ResourceManager.h"
#include "System/LockStats.h"   // SYS_COUNTED_LOCK contention probe
#include "Diagnostics/Profiler.h"
#include "Resource/AssetFS.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace Resource
{
    // ==== Construction ====

    ResourceManager::ResourceManager()
    {
        m_slots.reserve(4096);
        m_freeList.reserve(256);
    }

    ResourceManager::~ResourceManager()
    {
        // Signal that no new loads may be queued.
        m_shuttingDown.store(true, std::memory_order_release);

        // Block until every in-flight ProcessOneLoad task has exited: worker lambdas
        // that captured `this` must not run after the members (m_loaderMap, m_mutex)
        // are destroyed.
        std::unique_lock<std::mutex> lk(m_drainMutex);
        m_drainCV.wait(lk, [this]
        {
            return m_activeLoads.load(std::memory_order_acquire) == 0;
        });
    }

    // ==== Importer / Loader registry ====

    void ResourceManager::BuildImporterMap()
    {
        m_importerMap.clear();
        for (const auto& imp : m_importers)
        {
            if (!imp) continue;
            for (const char* ext : imp->GetSourceExtensions())
                if (ext) m_importerMap[ext] = imp.get();
        }
    }

    void ResourceManager::RegisterImporter(std::shared_ptr<IImporter> importer)
    {
        if (!importer) return;
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        m_importers.push_back(std::move(importer));
        BuildImporterMap();
    }

    void ResourceManager::BuildLoaderMap()
    {
        m_loaderMap.clear();
        for (const auto& loader : m_loaders)
            if (loader && loader->GetExtension())
                m_loaderMap[loader->GetExtension()] = loader.get();
    }

    void ResourceManager::RegisterLoader(std::shared_ptr<IResourceLoader> loader)
    {
        if (!loader) return;
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        m_loaders.push_back(std::move(loader));
        BuildLoaderMap();
    }

    // ==== Path utilities ====

    std::string ResourceManager::NormalizePath(const std::string& path) const
    {
        std::string s = path;
        for (char& c : s) if (c == '\\') c = '/';
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }

    std::string ResourceManager::GetExtension(const std::string& path) const
    {
        size_t pos = path.find_last_of('.');
        if (pos == std::string::npos) return {};
        std::string ext = path.substr(pos);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext;
    }

    std::string ResourceManager::DeriveCachePath(const std::string& sourcePath,
        const char* internalExtension)
    {
        size_t dot = sourcePath.find_last_of('.');
        if (dot == std::string::npos) return sourcePath + internalExtension;
        return sourcePath.substr(0, dot) + internalExtension;
    }

    bool ResourceManager::FileExists(const std::string& path)
    {
        // AssetFS, not the cwd: the imported cache (an .itex beside its .png) may be
        // in the pak, an engine root, or a worktree's shared root; a cwd-only test
        // would re-import each of those and write a copy into the cwd.
        return ::Resource::AssetFS::Get().Exists(path);
    }

    uint64_t ResourceManager::HashPath(const std::string& path)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : path) { hash ^= c; hash *= 1099511628211ULL; }
        return hash;
    }

    IImporter* ResourceManager::FindImporter(const std::string& ext) const
    {
        auto it = m_importerMap.find(ext);
        return it != m_importerMap.end() ? it->second : nullptr;
    }

    IResourceLoader* ResourceManager::FindLoader(const std::string& ext) const
    {
        auto it = m_loaderMap.find(ext);
        return it != m_loaderMap.end() ? it->second : nullptr;
    }

    // ==== Slot pool (callers hold m_mutex) ====

    uint32_t ResourceManager::AllocSlot()
    {
        if (!m_freeList.empty())
        {
            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            return idx;
        }
        m_slots.emplace_back();
        return static_cast<uint32_t>(m_slots.size() - 1);
    }

    void ResourceManager::FreeSlot(uint32_t index)
    {
        // Checked in Release too: an out-of-range index would write past m_slots and
        // push a slot that does not exist onto the free list. Freeing nothing leaks at
        // worst.
        if (!ENGINE_CHECK(index < m_slots.size(),
                "ResourceManager::FreeSlot: slot %u out of range (%zu slots)", index, m_slots.size()))
            return;
        ResourceEntry& e = m_slots[index];

        m_pathHashToSlot.erase(HashPath(e.path));

        // Bump the generation - invalidates ALL existing Handles to this slot.
        // Wraps around but never lands on 0 (the null-handle sentinel).
        e.generation = (e.generation % Handle::MAX_GEN) + 1;

        // Release ownership at once: the slot is the sole owner, so the object dies here.
        e.resource.reset();
        e.state = ResourceState::NotLoaded;
        e.failReason.clear();
        e.path.clear();
        e.type = ResourceType::Unknown;
        e.handle = Handle{};

        m_freeList.push_back(index);
    }

    bool ResourceManager::IsValid(Handle h) const
    {
        if (!h.IsValid()) return false;
        uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        // Generation mismatch -> stale handle; the slot may already be reused.
        return m_slots[idx].generation == h.Generation();
    }

    // ==== Load ====

    Handle ResourceManager::Load(const std::string& path, ResourceType type, int priority)
    {
        std::string norm = NormalizePath(path);
        if (norm.empty()) return Handle{};

        const uint64_t key = HashPath(norm);
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);

        auto it = m_pathHashToSlot.find(key);
        if (it != m_pathHashToSlot.end())
        {
            uint32_t       idx = it->second;
            ResourceEntry& entry = m_slots[idx];

            if (entry.state == ResourceState::Loading) return entry.handle;
            if (entry.state == ResourceState::Ready && entry.resource) return entry.handle;

            // Failed, or Ready without a resource -> recycle the slot (FreeSlot bumps the
            // generation).
            FreeSlot(idx);
        }

        uint32_t       idx = AllocSlot();
        ResourceEntry& entry = m_slots[idx];

        if (entry.generation == 0) entry.generation = 1; // never issue gen=0 handles

        entry.path = norm;
        entry.type = type;
        entry.state = ResourceState::Loading;
        entry.handle = Handle::Make(idx, type, static_cast<uint16_t>(entry.generation));

        m_pathHashToSlot[key] = idx;

        // Do not queue new loads after shutdown has started.
        if (m_shuttingDown.load(std::memory_order_acquire))
            return Handle{};

        const TaskSystem::TaskPriority tp =
            priority > 0 ? TaskSystem::TaskPriority::High
            : TaskSystem::TaskPriority::Low;

        // Increment before Push so the destructor can never see zero while a
        // task is queued but not yet running.
        m_activeLoads.fetch_add(1, std::memory_order_relaxed);

        TaskSystem::Get().Push([this, handle = entry.handle, norm]()
        {
            ProcessOneLoad(handle, norm);

            // Decrement and wake the drain wait UNDER m_drainMutex: the destructor
            // re-checks its predicate under the same mutex, so it cannot destroy
            // m_drainCV / m_drainMutex while this thread is inside notify_all() (notify
            // on a freed CV = UB). Do not decrement lock-free and notify outside the
            // lock: the destructor could observe 0 and tear them down mid-notify (the
            // same race class as Scheduler::RunPhase).
            std::lock_guard<std::mutex> lk(m_drainMutex);
            if (m_activeLoads.fetch_sub(1, std::memory_order_acq_rel) == 1)
                m_drainCV.notify_all();
        }, tp);

        return entry.handle;
    }

    // ==== Unload (explicit eviction) ====

    void ResourceManager::Unload(Handle handle)
    {
        if (!handle.IsValid()) return;
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        if (!IsValid(handle)) return;  // already stale, nothing to do
        FreeSlot(handle.Index());
    }

    // ==== Queries (O(1)) ====

    ResourceState ResourceManager::GetState(Handle handle) const
    {
        if (!handle.IsValid()) return ResourceState::NotLoaded;
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        if (!IsValid(handle)) return ResourceState::NotLoaded;
        const ResourceEntry& e = m_slots[handle.Index()];
        if (e.state == ResourceState::Ready && !e.resource) return ResourceState::NotLoaded;
        return e.state;
    }

    // Borrowed pointer: do not store it (see the declaration).
    const Resource* ResourceManager::GetResource(Handle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        if (!IsValid(handle)) return nullptr;
        return m_slots[handle.Index()].resource.get();
    }

    std::string ResourceManager::GetPath(Handle handle) const
    {
        if (!handle.IsValid()) return {};
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        if (!IsValid(handle)) return {};
        return m_slots[handle.Index()].path;
    }

    std::vector<ResourceManager::EntryInfo> ResourceManager::Snapshot() const
    {
        std::vector<EntryInfo> out;
        {
            SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
            out.reserve(m_pathHashToSlot.size());
            for (const auto& kv : m_pathHashToSlot)
            {
                const ResourceEntry& e = m_slots[kv.second];
                if (e.path.empty()) continue;
                EntryInfo info;
                info.path  = e.path;
                info.type  = e.type;
                info.state = (e.state == ResourceState::Ready && !e.resource) ? ResourceState::NotLoaded : e.state;
                if (e.state == ResourceState::Failed) info.failReason = e.failReason;
                out.push_back(std::move(info));
            }
        }
        std::sort(out.begin(), out.end(), [](const EntryInfo& a, const EntryInfo& b) { return a.path < b.path; });
        return out;
    }

    bool ResourceManager::Find(const std::string& path, EntryInfo& out) const
    {
        const std::string norm = NormalizePath(path);
        if (norm.empty()) return false;
        auto lower = [](std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        auto fill = [&out](const ResourceEntry& e)
        {
            out.path       = e.path;
            out.type       = e.type;
            out.state      = (e.state == ResourceState::Ready && !e.resource) ? ResourceState::NotLoaded : e.state;
            out.failReason = e.state == ResourceState::Failed ? e.failReason : std::string{};
        };
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        if (const auto it = m_pathHashToSlot.find(HashPath(norm)); it != m_pathHashToSlot.end())
        {
            fill(m_slots[it->second]);
            return true;
        }
        const std::string low = lower(norm);
        for (const auto& kv : m_pathHashToSlot)
        {
            const ResourceEntry& e = m_slots[kv.second];
            if (e.path.size() == low.size() && lower(NormalizePath(e.path)) == low)
            {
                fill(e);
                return true;
            }
        }
        return false;
    }

    std::string ResourceManager::GetFailReason(Handle handle) const
    {
        if (!handle.IsValid()) return {};
        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        if (!IsValid(handle)) return {};
        const ResourceEntry& e = m_slots[handle.Index()];
        return e.state == ResourceState::Failed ? e.failReason : std::string{};
    }

    // ==== Background worker ====

    void ResourceManager::ProcessOneLoad(Handle handle, std::string sourcePath)
    {
        // Every write below goes to the slot only while it still holds THIS
        // load (IsValid: the generation), never by index alone - see the Commit step.
        const uint32_t slotIndex = handle.Index();

        auto readFile = [](const std::string& p, std::vector<uint8_t>& out) -> bool
            {
                return ::Resource::AssetFS::Get().ReadFile(p, out);
            };

        auto failSlot = [&](const std::string& reason)
            {
                LOG_ERROR("ResourceManager: %s (path: '%s')", reason.c_str(), sourcePath.c_str());
                SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
                if (IsValid(handle))
                {
                    m_slots[slotIndex].state      = ResourceState::Failed;
                    m_slots[slotIndex].failReason = reason;
                }
            };

        const std::string srcExt = GetExtension(sourcePath);
        std::string       loadPath;
        std::vector<uint8_t> internalData;

        // --- Decide import vs direct load (brief lock) ---
        bool needsImport = false;
        {
            SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
            IImporter* imp = FindImporter(srcExt);
            if (imp)
            {
                const std::string cachePath = DeriveCachePath(sourcePath, imp->GetInternalExtension());
                if (FileExists(cachePath)) { loadPath = cachePath; }
                else { loadPath = cachePath; needsImport = true; }
            }
            else
            {
                loadPath = sourcePath;
            }
        }

        // --- File I/O (no lock held) ---
        if (needsImport)
        {
            std::vector<uint8_t> sourceData;
            if (!readFile(sourcePath, sourceData)) { failSlot("failed to open source file"); return; }

            IImporter* imp = nullptr;
            { SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager); imp = FindImporter(srcExt); }

            internalData = imp->Import(sourcePath, sourceData);
            if (internalData.empty()) { failSlot("importer returned empty blob"); return; }

            // Write cache (best-effort)
            try
            {
                // Temp + rename (creates the directory): another engine loading
                // the same asset - parallel packs, a second worktree - reads the
                // old cache or the new one, never half of it.
                if (!Sys::WriteFileBytesAtomic(loadPath, internalData.data(), internalData.size()))
                    LOG_WARNING("ResourceManager: could not write cache '%s'", loadPath.c_str());
            }
            catch (...) { LOG_WARNING("ResourceManager: exception writing cache '%s'", loadPath.c_str()); }
        }
        else
        {
            if (!readFile(loadPath, internalData)) { failSlot("failed to read internal file"); return; }
        }

        // --- Dispatch to loader (no lock held) ---
        const std::string internalExt = GetExtension(loadPath);
        IResourceLoader* loader = nullptr;
        { SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager); loader = FindLoader(internalExt); }

        if (!loader) { failSlot("no loader for internal extension"); return; }

        LoadResult result;
        try { result = loader->Load(loadPath, internalData); }
        catch (const std::exception& ex)
        {
            LOG_ERROR("ResourceManager: loader threw for '%s': %s", loadPath.c_str(), ex.what());
            SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
            if (IsValid(handle))
            {
                m_slots[slotIndex].state      = ResourceState::Failed;
                m_slots[slotIndex].failReason = std::string("loader threw: ") + ex.what();
            }
            return;
        }

        if (!result.resource)
        {
            failSlot(result.failReason.empty() ? "loader returned null resource" : result.failReason);
            return;
        }

        // --- Commit (unique_ptr move, no refcount) ---
        // Only into the load that started this task. Unload() frees the slot at
        // once, even while this task still runs, and the next Load() takes the
        // same index back (the free list is LIFO) under a new generation -
        // TextureSystem::RequestReload does exactly that to a texture still
        // loading. Committed by index, this abandoned result would land in the new
        // load's slot, and the new load's own commit would destroy it while the
        // main thread copies its pixels to the GPU (an access violation in
        // CreateTexture). An abandoned result is dropped instead, with the pending
        // upload that refers to it.
        bool committed = false;
        {
            SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
            if (IsValid(handle))
            {
                ResourceEntry& e = m_slots[slotIndex];
                // Transfer ownership into the slot - a single-owner move.
                e.resource = std::move(result.resource);
                e.state = ResourceState::Ready;
                committed = true;
            }
        }
        if (!committed)
        {
            LOG_INFO("ResourceManager: dropped an abandoned load of '%s' (unloaded while it loaded)",
                     sourcePath.c_str());
            return;   // result is destroyed here, outside the lock
        }

        if (result.pendingUpload)
        {
            SYS_COUNTED_LOCK(uploadLock, m_uploadMutex, ResourceUploads);
            m_pendingUploads.push(std::move(result.pendingUpload));
        }
    }

    // ==== GPU upload pump (main thread) ====

    void ResourceManager::ProcessPendingGPUUploads(float maxMs)
    {
        PROF_SCOPE("RM.PendingUploads");
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::duration<float, std::milli>(maxMs);

        std::queue<std::unique_ptr<IPendingGPUUpload>> local;
        { SYS_COUNTED_LOCK(lock, m_uploadMutex, ResourceUploads); local.swap(m_pendingUploads); }

        while (!local.empty() && Clock::now() < deadline)
        {
            auto task = std::move(local.front()); local.pop();
            if (task) task->Execute();
        }

        if (!local.empty())
        {
            SYS_COUNTED_LOCK(lock, m_uploadMutex, ResourceUploads);
            while (!local.empty())
            {
                m_pendingUploads.push(std::move(local.front())); local.pop();
            }
        }
    }

    // ==== Shutdown ====

    void ResourceManager::Shutdown()
    {
        // Prevent new loads from being queued.
        m_shuttingDown.store(true, std::memory_order_release);

        SYS_COUNTED_LOCK(lock, m_mutex, ResourceManager);
        m_slots.clear();       // unique_ptr destructors run here - no leaked objects
        m_freeList.clear();
        m_pathHashToSlot.clear();
    }

} // namespace Resource