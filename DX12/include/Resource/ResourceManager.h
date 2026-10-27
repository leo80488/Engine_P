#pragma once

#include "Resource/Resource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/IResourceLoader.h"
#include "Resource/IImporter.h"
#include "Resource/IPendingGPUUpload.h"
#include "System/TaskSystem.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    enum class ResourceState
    {
        NotLoaded,
        Loading,
        Ready,
        Failed
    };

    // ResourceEntry - one slot in the flat pool.
    //
    // Ownership: resource is a unique_ptr and ResourceManager is its sole owner.
    // Callers never hold a Resource* across frames; they ask ResourceManager each
    // time. FreeSlot() bumps the generation, so any Handle whose generation does
    // not match the slot is stale and gets nullptr / NotLoaded from every public
    // API - no UB, no use-after-free.
    //
    // Not shared_ptr: a cached shared_ptr<Resource> would keep the object alive
    // after FreeSlot(), so a stale handle would look invalid while the memory stays
    // held until the last external holder drops it, defeating generational
    // indexing.
    struct ResourceEntry
    {
        Handle                   handle;
        std::unique_ptr<Resource> resource;   // OWNED exclusively by ResourceManager
        ResourceState            state = ResourceState::NotLoaded;
        std::string              path;
        uint32_t                 generation = 0;
        ResourceType             type = ResourceType::Unknown;
        std::string              failReason;   // state == Failed: why (the logged reason)
    };

    class ResourceManager
    {
    public:
        // One slot in use, COPIED out for diagnostics - the Lua `Asset` table
        // (Renderer/AssetQuery.h): loaded, loading, or failed and why.
        struct EntryInfo
        {
            std::string   path;          // the spelling it was loaded with
            ResourceType  type  = ResourceType::Unknown;
            ResourceState state = ResourceState::NotLoaded;
            std::string   failReason;
        };

        ResourceManager();
        ~ResourceManager();

        ResourceManager(const ResourceManager&) = delete;
        ResourceManager& operator=(const ResourceManager&) = delete;

        void RegisterImporter(std::shared_ptr<IImporter> importer);
        void RegisterLoader(std::shared_ptr<IResourceLoader> loader);

        // Asynchronous: returns a Handle at once.
        //
        // Extension with a registered importer: derive the cache path (same dir,
        // internal extension); a cache hit loads it, a miss reads the source ->
        // Import() -> writes the cache -> loads it. Without an importer the file is
        // loaded directly through the matching IResourceLoader.
        //
        // A path already Loading / Ready returns its existing Handle; a Failed entry is
        // recycled under a bumped generation. @p priority > 0 queues the load on the
        // High task priority.
        Handle Load(const std::string& path, ResourceType type = ResourceType::Unknown, int priority = 0);

        // Queries, all O(1).
        //
        // GetResource() returns a BORROWED pointer, valid only within the current call
        // stack while no other thread can Unload() / Shutdown(). Do not store it: keep
        // the Handle and query again next frame; to hand a resource to another system
        // in the same frame, pass the Handle.
        ResourceState GetState(Handle handle) const;
        const Resource* GetResource(Handle handle) const; // borrowed, do NOT store
        std::string     GetPath(Handle handle) const;
        // Why a Failed load failed ("failed to open source file", "loader
        // returned null resource", ...); empty otherwise. The same text the
        // ERROR line logged - kept so a diagnostic (Render.explain) can answer
        // after the log line has scrolled away.
        std::string     GetFailReason(Handle handle) const;

        // Typed accessor - a static_cast of GetResource(): nullptr on a stale handle;
        // the type is NOT checked.
        template<typename T>
        const T* Get(Handle handle) const
        {
            return static_cast<const T*>(GetResource(handle));
        }

        // Every slot in use, sorted by path.
        std::vector<EntryInfo> Snapshot() const;
        // The slot loaded from @p path - that spelling, or one that differs only
        // in separators or letter case (the key here is case-sensitive, the file
        // system is not). False when nothing loaded it.
        bool Find(const std::string& path, EntryInfo& out) const;

        // Free a slot immediately; all existing Handles to it become stale.
        // Safe to call from any thread while not iterating over resources, and
        // while the slot is still Loading: that load's result is dropped, never
        // committed into whatever reuses the slot.
        void Unload(Handle handle);

        // Main-thread GPU upload pump; maxMs is the per-frame time budget.
        void ProcessPendingGPUUploads(float maxMs = 2.0f);

        void Shutdown();

    private:
        void ProcessOneLoad(Handle handle, std::string sourcePath);

        std::string      NormalizePath(const std::string& path) const;
        std::string      GetExtension(const std::string& path) const;
        static std::string DeriveCachePath(const std::string& sourcePath,
            const char* internalExtension);
        static bool        FileExists(const std::string& path);
        static uint64_t    HashPath(const std::string& path);

        IImporter* FindImporter(const std::string& extension) const;
        IResourceLoader* FindLoader(const std::string& extension) const;
        void BuildLoaderMap();
        void BuildImporterMap();

        // Slot pool helpers - callers hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);   // bumps generation, resets entry
        bool     IsValid(Handle h) const;   // generation check only

        // ---- In-flight task drain ----
        // The destructor waits for m_activeLoads to reach zero, so worker lambdas never
        // outlive the ResourceManager.
        std::atomic<int>        m_activeLoads{0};
        std::atomic<bool>       m_shuttingDown{false};
        std::condition_variable m_drainCV;
        std::mutex              m_drainMutex;   // used only with m_drainCV

        mutable std::mutex m_mutex;

        std::vector<ResourceEntry>             m_slots;
        std::vector<uint32_t>                  m_freeList;
        std::unordered_map<uint64_t, uint32_t> m_pathHashToSlot;

        std::vector<std::shared_ptr<IImporter>>           m_importers;
        std::unordered_map<std::string, IImporter*>       m_importerMap;

        std::vector<std::shared_ptr<IResourceLoader>>     m_loaders;
        std::unordered_map<std::string, IResourceLoader*> m_loaderMap;

        std::mutex                                         m_uploadMutex;
        std::queue<std::unique_ptr<IPendingGPUUpload>>     m_pendingUploads;
    };
}