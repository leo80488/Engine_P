#pragma once

// Upload-path waits: running totals since boot that frames.jsonl carries on every row
// (HarnessCaptureSystem) and Lua reads through Profiler.Upload(). A row where one moves paid for a
// CPU-blocking upload on that frame - the stall the streaming upload path exists to remove
// (scenario_run.py frames.totalGrowth gates a window of them).
//
// Totals, not per-frame counts, for the reason PipelineStats.h gives: a -framelog_every=N run that
// skips rows loses nothing, and the difference between two rows is what happened between them.
// Relaxed atomics - both upload lanes flush (the game half's and the render half's, see
// GraphicsDX12::Upload) and every reader is on some other thread.
//
// Never reset. GraphicsDX12's ResetUploadStats / GetUploadStallMs keep their own resettable window
// for the editor's world-load profile and the preview builds; that one cannot answer "what did
// frames 300-420 cost".

#include <atomic>
#include <chrono>
#include <cstdint>

namespace UploadStats
{
    // GraphicsDX12::FlushUploadAndWait round trips, both lanes: Close, execute,
    // signal a private fence, CPU-wait for it. A flush with nothing recorded
    // does none of that and is not counted.
    inline std::atomic<uint64_t> flushes{ 0 };
    // Wall time spent inside those round trips, in nanoseconds.
    inline std::atomic<uint64_t> stallNs{ 0 };
    // Which queue a flush waited on; a flush that ran both counts in both. copyFlushes: the copies
    // into resources just created with initial data, on the COPY queue - the wait is the copy itself.
    // directFlushes: everything that touches an existing resource (UpdateTexture,
    // CopyD3D12ResourceToTexture), and the creation copies too under -upload_directqueue, on the
    // graphics queue - the signal queues behind everything already submitted there, so the wait is as
    // long as the GPU's backlog, not the copy.
    inline std::atomic<uint64_t> copyFlushes{ 0 };
    inline std::atomic<uint64_t> directFlushes{ 0 };

    // The ImGui DX12 backend's own texture uploads (ImGui_ImplDX12_UpdateTexture
    // on a WantCreate / WantUpdates texture, reached from ImGuiManager): each one
    // executes on the same graphics queue and waits on the backend's fence with
    // WaitForSingleObject(INFINITE) - the same FIFO stall, which
    // FlushUploadAndWait never sees. Editor only; 0 in Game.
    inline std::atomic<uint64_t> imguiWaits{ 0 };
    inline std::atomic<uint64_t> imguiWaitNs{ 0 };
    // ...and the ones its TextureUploadFn hook handed to the streaming upload instead: nothing
    // waited for them. Editor only.
    inline std::atomic<uint64_t> imguiStreamed{ 0 };

    // The streaming upload (GraphicsDX12_StreamUpload.cpp): the uploader keeps these under its own
    // mutex and publishes them here after every change, so a reader on another thread takes no lock.
    // Totals too, except the ring's high-water mark (the most bytes one slot's ring has held).
    inline std::atomic<uint64_t> streamRequests{ 0 };
    inline std::atomic<uint64_t> streamBytes{ 0 };
    inline std::atomic<uint64_t> streamReplaced{ 0 };
    inline std::atomic<uint64_t> streamCarriedOver{ 0 };
    inline std::atomic<uint64_t> streamFallback{ 0 };
    inline std::atomic<uint64_t> streamDropped{ 0 };     // incl. LatestWins lost with an unsubmitted frame
    inline std::atomic<uint64_t> streamRejected{ 0 };
    inline std::atomic<uint64_t> streamStale{ 0 };       // skipped at record: the texture was replaced
    inline std::atomic<uint64_t> streamCopies{ 0 };      // copies recorded into StreamUpload lists
    inline std::atomic<uint64_t> streamRingHighWater{ 0 };

    // Every GraphicsDX12::CreateTexture (any thread; render targets too), and the ones made on the
    // game thread - neither the Render context nor a TaskSystem worker: a texture promotion
    // (TextureSystem::Tick), a pass resource the game half creates, a preload's step. A preloaded
    // prefab's spawn window makes none on the game thread.
    inline std::atomic<uint64_t> textureCreates{ 0 };
    inline std::atomic<uint64_t> textureCreatesGame{ 0 };

    // The flushes above that the game thread waited in (neither the Render context nor a TaskSystem
    // worker, textureCreatesGame's rule) - a preload's and an async spawn's frames make none with the
    // loader uploads on. And the loader contexts' own flushes (IGraphicsDevice::BeginLoaderUploads),
    // each a worker's batch of creation copies waited for on that worker: not in flushes / stallNs /
    // copyFlushes / directFlushes, which stay the frame threads' waits.
    inline std::atomic<uint64_t> flushesGame{ 0 };
    inline std::atomic<uint64_t> loaderFlushes{ 0 };
    inline std::atomic<uint64_t> loaderStallNs{ 0 };

    inline uint64_t ToNs(std::chrono::steady_clock::duration d) noexcept
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        return ns > 0 ? static_cast<uint64_t>(ns) : 0u;
    }

    inline void NoteFlush(std::chrono::steady_clock::duration d) noexcept
    {
        flushes.fetch_add(1, std::memory_order_relaxed);
        stallNs.fetch_add(ToNs(d), std::memory_order_relaxed);
    }

    inline void NoteImGuiWait(std::chrono::steady_clock::duration d) noexcept
    {
        imguiWaits.fetch_add(1, std::memory_order_relaxed);
        imguiWaitNs.fetch_add(ToNs(d), std::memory_order_relaxed);
    }

    inline double NsToMs(uint64_t ns) noexcept { return static_cast<double>(ns) / 1.0e6; }
}
