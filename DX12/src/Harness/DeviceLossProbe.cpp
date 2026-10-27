#include "Harness/DeviceLossProbe.h"

#ifdef WITH_HARNESS

#include "Graphics/GraphicsDX12.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <wrl/client.h>

#include <chrono>
#include <thread>

namespace Harness
{
void ArmDeviceLossProbe(IGraphicsDevice& device)
{
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    if (!cl.Has("-test_remove_device_after_ms"))
        return;
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&device);
    ID3D12Device5* native = dx12 ? dx12->GetDevice5() : nullptr;
    if (!native)
    {
        LOG_ERROR("[TEST] -test_remove_device_after_ms: this device cannot be removed (no ID3D12Device5)");
        return;
    }
    const int ms = cl.GetInt("-test_remove_device_after_ms", 3000);
    LOG_WARNING("[TEST] -test_remove_device_after_ms=%d: the device will be removed", ms);
    // The thread holds its own reference: the device may be gone (the run ended first) when it wakes.
    Microsoft::WRL::ComPtr<ID3D12Device5> held(native);
    std::thread([held, ms]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 0));
        LOG_WARNING("[TEST] -test_remove_device_after_ms: removing the device now");
        held->RemoveDevice();
    }).detach();
}
}

#endif // WITH_HARNESS
