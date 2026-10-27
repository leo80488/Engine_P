#pragma once

// VideoMemory - the process's local video memory against the OS budget (DXGI QueryVideoMemoryInfo).
//
// Past the budget the driver demotes allocations to system memory and every access pays over PCIe, as GPU and
// driver time that compounds with each scene load; nothing else reports it. The budget the engine plans against
// is the OS's (it changes while the process runs: the notification event below), capped by the harness's
// -vram_budget_mb, and squeezed by -vram_budget_squeeze=<sample>:<mb> (the budget becomes the usage at that
// sample minus <mb>: what the OS does when another process takes the card).

#include <cstdint>

struct IDXGIFactory6;
struct ID3D12Device;
struct ID3D12Pageable;
struct ID3D12Resource;

namespace VideoMemory
{
    struct Info
    {
        uint64_t usage  = 0;   // bytes this process holds in local video memory
        uint64_t budget = 0;   // bytes it may hold (0 until the first sample)
    };

    // Once per frame, from the render owner: samples the local segment, feeds run.json's vram block, warns the
    // moment usage crosses the budget, logs every ~600 frames, and notes the OS's budget-change event.
    void Sample(IDXGIFactory6* factory, ID3D12Device* device);

    // The last sample (zeros before the first).
    Info Last();

    // How many times the OS signalled a budget change (RegisterVideoMemoryBudgetChangeNotificationEvent).
    uint32_t BudgetChanges();

    // Raises @p object's residency priority above the default: render targets and depth are what a paging
    // driver should demote last (a frame that misses one stalls whole). Counted in PrioritizedCount.
    void PrioritizeHigh(ID3D12Device* device, ID3D12Pageable* object);
    // A committed texture the device just made: high priority when it is a render target or depth.
    void PrioritizeTexture(ID3D12Device* device, ID3D12Resource* texture);
    uint32_t PrioritizedCount();

    // Gives the notification back. Before the device is destroyed.
    void Shutdown();
}
