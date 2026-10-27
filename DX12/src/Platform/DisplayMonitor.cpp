#include "Platform/DisplayMonitor.h"

#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cwchar>
#include <vector>

namespace Plat
{
namespace
{
    using Microsoft::WRL::ComPtr;

    // The path of the active display configuration that drives the GDI device @p device: it carries the
    // exact refresh rate (a fraction, where EnumDisplaySettings rounds) and the SDR white level.
    void ReadDisplayConfig(const wchar_t* device, DisplayMonitor& out)
    {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0)
            return;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
            ERROR_SUCCESS)
            return;
        for (UINT32 i = 0; i < pathCount; ++i)
        {
            const DISPLAYCONFIG_PATH_INFO& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size      = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id        = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                std::wcscmp(source.viewGdiDeviceName, device) != 0)
                continue;

            const RefreshRate rate = MakeRefreshRate(path.targetInfo.refreshRate.Numerator,
                                                     path.targetInfo.refreshRate.Denominator);
            if (rate.Known())
                out.refresh = rate;

            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size      = sizeof(white);
            white.header.adapterId = path.targetInfo.adapterId;
            white.header.id        = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
                out.sdrWhiteNits = SdrWhiteNitsFromLevel(white.SDRWhiteLevel);
            return;
        }
    }

    // DXGI_OUTPUT_DESC1 of the output on @p monitor, from a FRESH factory: a factory made before an HDR
    // toggle or a monitor change keeps reporting the old colour space (IsCurrent() turns false), so the
    // engine's own would keep sending PQ to a display switched back to SDR. Nothing here touches the
    // swap chain, which the render-owner lane presents on.
    void QueryOutputHdr(HMONITOR monitor, DisplayMonitor& out)
    {
        ComPtr<IDXGIFactory1> factory;
        if (!monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
        {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
            {
                DXGI_OUTPUT_DESC desc{};
                ComPtr<IDXGIOutput6> output6;
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor && SUCCEEDED(output.As(&output6)))
                {
                    DXGI_OUTPUT_DESC1 desc1{};
                    if (SUCCEEDED(output6->GetDesc1(&desc1)))
                    {
                        out.hdrCapable       = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                        out.maxLuminanceNits = desc1.MaxLuminance;
                        out.minLuminanceNits = desc1.MinLuminance;
                        out.maxFullFrameNits = desc1.MaxFullFrameLuminance;
                    }
                    return;
                }
                output.Reset();
            }
            adapter.Reset();
        }
    }
}

DisplayMonitor QueryDisplayMonitor(HWND hwnd)
{
    DisplayMonitor d;
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    d.monitor = reinterpret_cast<std::uintptr_t>(monitor);
    if (!monitor)
        return d;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
    {
        d.left   = info.rcMonitor.left;
        d.top    = info.rcMonitor.top;
        d.right  = info.rcMonitor.right;
        d.bottom = info.rcMonitor.bottom;

        // The active mode: the desktop size a resolution picker filters its presets against, and the
        // integer refresh used when the display configuration has no fraction.
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        {
            if (mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0)
            {
                d.desktopWidth  = static_cast<int>(mode.dmPelsWidth);
                d.desktopHeight = static_cast<int>(mode.dmPelsHeight);
            }
            d.refresh = MakeRefreshRate(mode.dmDisplayFrequency, 1);
            if (!d.refresh.Known())
                d.refresh = {};
        }
        ReadDisplayConfig(info.szDevice, d);
    }
    QueryOutputHdr(monitor, d);
    return d;
}
}
