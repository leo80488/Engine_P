#include "Graphics/VideoMemory.h"

#include "Graphics/MemoryPolicy.h"
#include "Harness/RunLedger.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/ProcessMemory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace VideoMemory
{
namespace
{
    constexpr uint64_t kMiB = 1ull << 20;

    std::atomic<uint64_t> g_usage{ 0 };
    std::atomic<uint64_t> g_budget{ 0 };
    std::atomic<uint32_t> g_changes{ 0 };
    std::atomic<uint32_t> g_prioritized{ 0 };
    // Set once, by the first Sample; the adapter outlives the device's use of it.
    Microsoft::WRL::ComPtr<IDXGIAdapter3> g_adapter;
    HANDLE g_changeEvent = nullptr;
    DWORD  g_changeCookie = 0;

    // The harness's budget arms, read once. The cap is a fake budget; the squeeze lowers it below the usage at
    // a given sample, the way the OS does when another process takes the card.
    struct Fake
    {
        uint64_t capBytes       = 0;     // -vram_budget_mb
        uint32_t squeezeSample  = 0;     // -vram_budget_squeeze=<sample>:<mb>
        uint64_t squeezeBytes   = 0;
        uint64_t squeezedBudget = 0;     // set when the squeeze fires
    };
    Fake& FakeBudget()
    {
        static Fake s_fake = []
        {
            Fake f;
#if defined(WITH_HARNESS) && WITH_HARNESS
            const Sys::CommandLine& cl = Sys::CommandLine::Get();
            f.capBytes = static_cast<uint64_t>((std::max)(0, cl.GetInt("-vram_budget_mb", 0))) * kMiB;
            const std::string squeeze = cl.Get("-vram_budget_squeeze");
            char* colon = nullptr;
            const unsigned long sample = std::strtoul(squeeze.c_str(), &colon, 10);
            if (colon && *colon == ':')
            {
                f.squeezeSample = static_cast<uint32_t>(sample);
                f.squeezeBytes  = static_cast<uint64_t>(std::strtoul(colon + 1, nullptr, 10)) * kMiB;
            }
#endif
            return f;
        }();
        return s_fake;
    }

    bool Query(uint64_t& usage, uint64_t& budget)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
        if (!g_adapter || FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
            return false;
        usage  = vm.CurrentUsage;
        budget = vm.Budget;
        return true;
    }

    // The budget the engine plans against: the OS's, under the harness's cap and squeeze.
    uint64_t Effective(uint64_t osBudget)
    {
        const Fake& f = FakeBudget();
        uint64_t b = osBudget;
        if (f.capBytes) b = (std::min)(b, f.capBytes);
        if (f.squeezedBudget) b = (std::min)(b, f.squeezedBudget);
        return b;
    }

    bool ProbeEffective(uint64_t& usage, uint64_t& budget)
    {
        if (!Query(usage, budget)) return false;
        budget = Effective(budget);
        return true;
    }
}

void Sample(IDXGIFactory6* factory, ID3D12Device* device)
{
    static bool     s_init          = false;
    static uint32_t s_tick          = 0;
    static bool     s_wasOverBudget = false;
    if (!s_init)
    {
        s_init = true;
        if (factory && device)
            factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&g_adapter));
        if (g_adapter)
        {
            Sys::SetVideoMemoryProbe(&ProbeEffective);
            g_changeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_changeEvent &&
                FAILED(g_adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(g_changeEvent, &g_changeCookie)))
            {
                ::CloseHandle(g_changeEvent);
                g_changeEvent = nullptr;
            }
        }
    }
    uint64_t usage = 0, osBudget = 0;
    if (!Query(usage, osBudget))
        return;
    ++s_tick;

    Fake& fake = FakeBudget();
    if (fake.squeezeSample && s_tick == fake.squeezeSample)
    {
        fake.squeezedBudget = usage > fake.squeezeBytes + 256 * kMiB ? usage - fake.squeezeBytes : 256 * kMiB;
        LOG_INFO("[BUDGET] squeezed (harness): %llu MB used, the budget is now %llu MB",
                 (unsigned long long)(usage >> 20), (unsigned long long)(fake.squeezedBudget >> 20));
    }
    // The OS says its budget moved (another process took or gave memory back): the numbers read below are the
    // new ones; ViewBudget plans against them at the next frame boundary.
    if (g_changeEvent && ::WaitForSingleObject(g_changeEvent, 0) == WAIT_OBJECT_0)
    {
        g_changes.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("[BUDGET] the OS changed the video memory budget: %llu MB used, %llu MB budget",
                 (unsigned long long)(usage >> 20), (unsigned long long)(osBudget >> 20));
    }

    const uint64_t budget = Effective(osBudget);
    g_usage.store(usage, std::memory_order_relaxed);
    g_budget.store(budget, std::memory_order_relaxed);
    // Peak, lowest budget and frames over it go into run.json's "vram": what scenario_run --jobs reads to
    // tell a paging machine from a regression, and to size each pack's VRAM need.
    Harness::RunLedger::Get().NoteVram(usage, budget);
    const bool over = usage > budget;
    if (over && !s_wasOverBudget)
        LOG_WARNING("VRAM OVER BUDGET: %llu MB used / %llu MB budget - the driver is now paging over PCIe; "
                    "expect both CPU and GPU frame time to degrade and to keep degrading. This is the usual "
                    "cause of 'repeated scene loads get slower'.",
                    (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20));
    s_wasOverBudget = over;
    if ((s_tick % 600u) == 0)
        LOG_DIAG(ENGINE_LOG_VRAM, "VRAM: %llu MB used / %llu MB budget (%s)",
                 (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                 over ? "OVER BUDGET" : "ok");
}

Info Last()
{
    return { g_usage.load(std::memory_order_relaxed), g_budget.load(std::memory_order_relaxed) };
}

uint32_t BudgetChanges() { return g_changes.load(std::memory_order_relaxed); }

void PrioritizeTexture(ID3D12Device* device, ID3D12Resource* texture)
{
    if (!device || !texture) return;
    const D3D12_RESOURCE_FLAGS flags = texture->GetDesc().Flags;
    if (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        PrioritizeHigh(device, texture);
}

void PrioritizeHigh(ID3D12Device* device, ID3D12Pageable* object)
{
    if (!device || !object || MemoryPolicy::NoResidencyPriority()) return;
    Microsoft::WRL::ComPtr<ID3D12Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return;
    const D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_HIGH;
    ID3D12Pageable* objects[1] = { object };
    if (SUCCEEDED(device1->SetResidencyPriority(1, objects, &priority)))
        g_prioritized.fetch_add(1, std::memory_order_relaxed);
}

uint32_t PrioritizedCount() { return g_prioritized.load(std::memory_order_relaxed); }

void Shutdown()
{
    if (g_adapter && g_changeEvent)
        g_adapter->UnregisterVideoMemoryBudgetChangeNotification(g_changeCookie);
    if (g_changeEvent) ::CloseHandle(g_changeEvent);
    g_changeEvent = nullptr;
}
}
