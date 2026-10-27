#pragma once
// Harness/DeviceLossProbe.h - -test_remove_device_after_ms=N: the GPU device is removed N ms in.
//
// ID3D12Device5::RemoveDevice() simulates a TDR: every fence completes at UINT64_MAX, the next Present returns
// DXGI_ERROR_DEVICE_REMOVED and the device-lost path runs for real (the ledger's device_lost verdict, DRED, the
// player-facing dialog, the restart offer). Without it nothing can make the device go away on demand, and
// that path was only ever read, never run (tests/unit/test_device_lost.py).

class IGraphicsDevice;

namespace Harness
{
    // Starts the timer thread when the flag is given (harness builds only; a no-op otherwise).
    void ArmDeviceLossProbe(IGraphicsDevice& device);
}
