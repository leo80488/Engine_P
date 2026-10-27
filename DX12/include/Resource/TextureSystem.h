#pragma once

#include "Resource/SystemHandles.h"
#include "Resource/ResourceManager.h"
#include "Resource/TextureResource.h"
#include "Graphics/GraphicsStruct.h"
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <atomic>

class IGraphicsDevice;

namespace Resource
{
    // TextureSystem - GPU texture lifetime in a generational slot array.
    //
    // Every Acquire() returns a Handle whose generation gives O(1) stale detection: a re-load of the
    // same path after eviction gets a new generation, so old handles held by an AssetEntry or a runtime
    // material are detectably stale with no extra bookkeeping.
    //
    // Usage (main thread; Acquire once - every call takes a reference - and Release when done):
    //   TextureHandle h = texSys.Acquire(path, rm, gfx);
    //   if (texSys.IsReady(h)) {                                // each frame
    //       gfx.BindResource(slot, *texSys.GetTexture(h), cmd);
    //   }
    //
    // Thread safety: Acquire/Release/IsReady hold m_mutex; GetTexture is main-thread only (no lock on
    // the hot path).
    class TextureSystem
    {
    public:
        struct Status
        {
            enum class State { Invalid, Loading, Ready, Failed } state = State::Invalid;
            std::string reason;
        };

        struct Info
        {
            uint32_t    width = 0, height = 0;   // width .. revision: set once GPU-resident
            uint32_t    depth = 1;
            uint32_t    mipLevels = 0;
            uint32_t    arraySize = 0;           // slices; a cube counts 6
            bool        isCube = false;
            RHI::Format format = RHI::Format::UNKNOWN;
            uint64_t    gpuBytes = 0;            // the resource's allocation (GetResourceAllocationInfo)
            // Bumped whenever THIS texture's contents change (upload, reload):
            // a viewer re-renders when it moves. 0 = never uploaded.
            uint64_t    revision = 0;
            uint32_t    refCount = 0;            // live acquisitions
            bool        gpuReady = false;
        };

        struct EntryView
        {
            TextureHandle handle;
            std::string   path;
            Info          info;
            Status        status;
        };

        enum class ReloadRequest { NotLoaded, Unchanged, Requested };

        TextureSystem()  = default;
        ~TextureSystem() = default;

        TextureSystem(const TextureSystem&)            = delete;
        TextureSystem& operator=(const TextureSystem&) = delete;

        // Acquires a GPU texture for @p path. The same path -> the same slot, ref count incremented; a new
        // path allocates a slot and starts an async load through ResourceManager.
        TextureHandle Acquire(const std::string& path,
                              ResourceManager&   rm,
                              IGraphicsDevice&   gfx);

        // Acquire for a preload. A texture whose slot this call creates is budgeted: Tick promotes such
        // textures under a per-frame time budget (2 ms, at least one a frame; -tex_promote_budget_ms=N), or
        // on a loader worker when loader uploads are on, instead of in the one burst every finished read
        // gets. The class belongs to the slot: an ordinary Acquire of the path clears it (a spawn that needs
        // the texture now promotes it with the burst); a slot that already exists keeps its class.
        TextureHandle AcquireBudgeted(const std::string& path,
                                      ResourceManager&   rm,
                                      IGraphicsDevice&   gfx);
        // Acquire for a reader of the decoded pixels after the upload (the terrain's heightmap collision):
        // the slot keeps its CPU image (GetResourceManagerHandle), which every other texture releases once
        // it is on the GPU. A slot that already released it loads the file again for the image alone.
        TextureHandle AcquireWithCpuImage(const std::string& path,
                                          ResourceManager&   rm,
                                          IGraphicsDevice&   gfx);
        // Budgeted textures that had finished reading and still waited for a
        // promotion when the last Tick ended (0 when none).
        uint32_t PendingBudgetedPromotions() const;
        // Budgeted textures the last Tick promoted on the main thread that are larger than one frame's
        // budget can carry (> 8 MB on the GPU; a 4096^2 BC7 chain is 22 MB): each lands alone in its frame
        // and is that frame's cost, and the preload lists such a frame as a big step. Loader-worker
        // promotions are not counted.
        uint32_t LastTickOversizedBudgetedPromotions() const;
        // The wall time the last Tick spent promoting budgeted textures - the
        // preload's own steps share one per-frame budget with it.
        double LastTickBudgetedMs() const;

        // Decrements the ref count. At 0 the GPU texture is queued for deferred destruction (Tick hands it
        // to the device's frame-stamped release queue) and the slot is freed.
        void Release(TextureHandle handle, IGraphicsDevice& gfx);

        // True only when the GPU texture is fully resident.
        bool IsReady(TextureHandle handle) const;
        // Changes whenever visible texture contents/residency can change, even
        // if a descriptor slot is recycled. Reference accumulation observes it.
        uint64_t ContentRevision() const { return m_contentRevision.load(std::memory_order_relaxed); }

        // Main-thread accessor. O(1) generational lookup; nullptr for invalid, stale or not-yet-ready
        // handles.
        const RHI::Texture* GetTexture(TextureHandle handle) const;

        // The ResourceManager handle backing this texture, or an invalid Handle for a stale / unknown one:
        // the way to the CPU-side TextureResource (DirectX::ScratchImage), e.g. for the terrain collision
        // path decoding an R16_UNORM heightmap into a HeightField. Once the texture is on the GPU only a
        // slot acquired with AcquireWithCpuImage still has one. Main-thread only.
        Handle GetResourceManagerHandle(TextureHandle handle) const;

        // Where one texture's load stands, for diagnostics (Render.explain):
        // Ready (GPU-resident), Loading, or Failed with the reason - the
        // ResourceManager's (file, import, loader) or the GPU upload's
        // (unsupported format, CreateTexture). Invalid = no such handle.
        // A material slot whose texture never becomes Ready samples the
        // shader's fallback (white / flat normal), so "loading forever" and
        // "failed" are the states a black or wrong-looking surface needs named.
        Status GetStatus(TextureHandle handle, const ResourceManager& rm) const;

        // The texture loaded from @p path WITHOUT acquiring it (no reference is
        // taken) - an invalid handle when nothing loaded it. Matched as Acquire
        // matches (separators and letter case folded). Diagnostics: the Lua
        // `Asset` table (Renderer/AssetQuery.h).
        TextureHandle Find(const std::string& path) const;

        bool Describe(TextureHandle handle, Info& out) const;   // false: stale / unknown handle

        // Every live texture (refCount > 0) with its path and state, for the Loaded Textures window and Lua
        // Asset.textures. The path comes from the ResourceManager handle (rm.GetPath).
        std::vector<EntryView> Snapshot(const ResourceManager& rm) const;

        // A reimport: re-read `path` from disk and give its texture the new
        // contents IN PLACE once the file has loaded - same handle, same
        // bindless slot, every descriptor a consumer cached now reads the new
        // resource (IGraphicsDevice::ReplaceTexture), so no material has to
        // re-resolve. Until then, and for good if the new file fails to load or
        // upload, the previous contents keep drawing.
        //
        // Requested: opens a hot-reload receipt of kind "texture"
        // (System/HotReloadReceipt.h; its id in *outReceipt), closed by Tick
        // (in effect from Tick's frame, load_failed, reload_error), by a newer
        // RequestReload (superseded) or by the last Release (not_in_use).
        // NotLoaded: nothing loaded reads `path` - its receipt is opened and
        // closed at once as not_in_use (none for an empty path). Unchanged: a
        // reload of this very write was already requested (an import reloads
        // its own output, then the file watcher reports the same write) - no
        // receipt.
        ReloadRequest RequestReload(const std::string& path, ResourceManager& rm,
                                    std::uint64_t* outReceipt = nullptr);

        // Promote RM-ready entries to GPU-ready, and swap in reloads whose new
        // file has loaded (one GPU idle for all of them, before this frame's
        // deferred destroys - see App's AssetTick). @p frame stamps their
        // receipts. Call once per frame on the main thread, outside any device
        // frame.
        void Tick(ResourceManager& rm, IGraphicsDevice& gfx, std::uint64_t frame = 0);

        // Releases all GPU textures. Call once before device shutdown.
        void Shutdown(IGraphicsDevice& gfx);

    private:
        struct TextureEntry
        {
            RHI::Texture texture;
            uint32_t     refCount   = 0;
            Handle       rmHandle;           // IO-layer handle for polling state
            bool         gpuReady   = false;
            uint32_t     generation = 0;     // TextureSystem's own generation
            uint64_t     pathHash   = 0;     // for reverse-lookup on FreeSlot
            // Non-empty = the GPU upload failed (unsupported format, CreateTexture). The entry stays failed,
            // with no retry and no ERROR every frame, until the slot is released and re-acquired (or reloaded).
            std::string  uploadError;

            // Info::gpuBytes / Info::revision, written by UploadToGPU, FinishReload and the async publish.
            std::uint64_t gpuBytes = 0;
            std::uint64_t revision = 0;

            // A reload in flight (RequestReload). reloadRm: the NEW file's load
            // while the live texture keeps drawing (gpuReady slots; a slot that
            // never became ready reloads through rmHandle and the ordinary
            // promotion instead). reloadReceipt: the open receipt that Tick, a
            // newer RequestReload or FreeSlot closes.
            Handle        reloadRm;
            std::uint64_t reloadReceipt = 0;
            // The write RequestReload last read (size and mtime), 0 = none yet:
            // a second notice of the same write is not a second reload.
            std::uint64_t fileStamp     = 0;
            // Promoted under Tick's per-frame budget (AcquireBudgeted).
            bool          budgeted      = false;
            // Its texture is being created on a loader worker (AsyncPromotion).
            bool          promoting     = false;
            // The path the slot was created with: what the texture is called once its ResourceManager
            // entry is released.
            std::string   path;
            // Keep the CPU image after the upload (AcquireWithCpuImage).
            bool          keepCpuImage  = false;
        };

        // A budgeted texture created on a loader worker. The job owns texture / gpuBytes / error until
        // `done` (release store; Tick loads it with acquire, then publishes or destroys).
        struct AsyncPromotion
        {
            uint32_t          slot = 0, generation = 0;
            Handle            rmHandle;      // kept loaded until published (Tick's unload pass)
            std::string       name;
            RHI::Texture      texture;
            uint64_t          gpuBytes = 0;
            std::string       error;
            std::atomic<bool> done{ false };
        };
        static constexpr uint32_t kMaxAsyncPromotions = 2;   // the device's loader contexts

        // Tick: publish the finished ones (m_mutex held).
        void PublishAsyncPromotions(IGraphicsDevice& gfx, ResourceManager& rm);
        // Tick: hand @p entry (slot @p idx) to a loader worker (m_mutex held).
        void StartAsyncPromotion(TextureEntry& entry, uint32_t idx, ResourceManager& rm, IGraphicsDevice& gfx);

        TextureHandle AcquireImpl(const std::string& path, ResourceManager& rm, bool budgeted, bool keepCpuImage);

        // After a successful upload: unloads the slot's ResourceManager entry - the file's bytes decoded into
        // a ScratchImage, which the GPU copy no longer needs - unless the slot keeps its CPU image. The
        // device copied the pixels into its staging buffer before CreateTexture / ReplaceTexture returned.
        // The caller holds m_mutex.
        void DropCpuImage(TextureEntry& entry, ResourceManager& rm);

        // The desc and per-subresource data a TextureResource uploads with;
        // false (and @p error) for a format the RHI cannot take.
        static bool DescribeUpload(const TextureResource& res, RHI::TextureDesc& desc,
                                   std::vector<RHI::SubresourceData>& subData, std::string& error);

        // Uploads a TextureResource to the GPU and marks the entry ready. @p name: the path it was loaded
        // from, the texture's debug name, which the GPU memory ledger lists it under (Profiler.MemoryTop).
        // The caller must hold m_mutex.
        void UploadToGPU(const TextureResource& res, TextureEntry& entry, IGraphicsDevice& gfx,
                         const std::string& name);

        // Swap a loaded (or failed) reload into its slot and close its
        // receipt. Caller must hold m_mutex; the GPU is idle.
        void FinishReload(TextureEntry& entry, ResourceManager& rm, IGraphicsDevice& gfx,
                          std::uint64_t frame);

        // Maps DXGI_FORMAT -> RHI::Format for the TextureDesc.
        static RHI::Format DxgiToRhiFormat(uint32_t dxgiFmt);

        // FNV-1a 64-bit, as ResourceManager's, but with '\' folded to '/' and A-Z to lower case while
        // hashing: the dedup key only - the file is opened from the original path.
        static uint64_t HashPath(const std::string& path);

        // Slot pool helpers; the caller holds m_mutex, except IsValidHandle from the main thread's
        // lock-free GetTexture / GetResourceManagerHandle.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);      // bumps generation, removes path mapping
        bool     IsValidHandle(Handle h) const; // generation check

        mutable std::mutex m_mutex;
        std::atomic<uint64_t> m_contentRevision{1};
        std::vector<TextureEntry>              m_slots;
        std::vector<uint32_t>                  m_freeList;
        std::unordered_map<uint64_t, uint32_t> m_pathHashToSlot; // path hash -> slot index
        std::vector<RHI::Texture>              m_pendingDestroy; // released; Tick hands them to the frame-stamped release queue
        std::vector<Handle>                    m_pendingRmUnload; // last ref released; Tick retires CPU data
        uint32_t                               m_budgetedWaiting = 0;  // last Tick's leftovers
        uint32_t                               m_budgetedOversized = 0; // last Tick's > 8 MB ones
        double                                 m_budgetedMs = 0.0;       // last Tick's budgeted time
        // The loader workers' promotions, and their jobs in flight (Shutdown waits).
        std::vector<std::shared_ptr<AsyncPromotion>> m_asyncPromotions;
        std::mutex                             m_asyncMutex;
        std::condition_variable                m_asyncCv;
        int                                    m_asyncJobs = 0;
    };
}
