#pragma once

// IDeviceFrameControl - the frame executor's contract with the device.
//
// FramePipeline (EngineCore's frame executor) drives the device's frame threading through this:
// which thread owns the device frame, the drain hook of the bounded frame transaction, the
// device-execution tokens, the render frame, the upload-slot acquisition and its audits. Nothing
// else calls it - a pass never does - so it is its own interface, handed out by
// IGraphicsDevice::FrameControl() (null for a device without frame coordination). GraphicsDX12
// implements both.

#include "System/ExecutionDomain.h"   // Sys::ExecutionDomain::Token
#include "System/OwnerTask.h"         // Sys::OwnerTask

#include <cstdint>
#include <functional>
#include <thread>

class IDeviceFrameControl
{
public:
    virtual ~IDeviceFrameControl() = default;

    // Hard-sync operations from outside an executing device segment call @p hook to drain the
    // bounded frame transaction first (FramePipeline's DrainFrameWork); an empty function removes it.
    virtual void SetFrameDrainHook(std::function<void()> hook) = 0;
    // The thread that owns the device frame: main, or the render-owner lane once it runs.
    // IssueDeviceExecution refuses every other thread.
    virtual void SetDeviceFrameThread(std::thread::id id) = 0;
    // The graph frame mode: one logical frame coordinator for both halves.
    virtual void EnableLogicalFrameCoordinator() = 0;
    // Opens frame @p frame's device-execution token, stamped with the render upload slot. Device
    // frame thread only.
    virtual Sys::ExecutionDomain::Token IssueDeviceExecution(uint64_t frame) = 0;
    virtual void RetireDeviceExecution(const Sys::ExecutionDomain::Token& token) = 0;
    // The engine frame the render half is about to record.
    virtual void SetRenderFrame(uint64_t frame) = 0;
    // The game half's acquire of frame @p frame's upload slot: waits for that slot's retirement
    // fences without touching swap-chain state.
    virtual Sys::OwnerTask AcquireUploadSlotAsync(uint64_t frame) = 0;
    // Anim overlap (-animoverlap): permit AcquireUploadSlotAsync while the render-owner lane has a
    // frame open.
    virtual void SetConcurrentUploadAcquire(bool on) = 0;
    // -animoverlapverify: count GetUploadSlot() calls made from the render half.
    virtual void SetGameSlotAudit(bool on) = 0;
    virtual uint32_t GameSlotReadsFromRender() const = 0;
    // Lifetime flush count of upload context @p ctx (0 game, 1 render).
    virtual uint64_t UploadFlushesLifetime(int ctx) const = 0;
    // Stop publishing GPU completions (the pipeline's teardown).
    virtual void CloseGpuCompletions() = 0;
};
