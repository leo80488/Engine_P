#pragma once
// Platform/DisplayMonitor.h - the monitor the main window is on, as one immutable value.
//
// What the engine knows about its display (the monitor's rectangle and desktop mode, the exact
// refresh rate, the HDR capability of its DXGI output, Windows' SDR white level) is read in one pass
// on the main thread (SurfaceDX12::RefreshDisplayInfo, at a frame boundary) and published whole:
// a reader on any thread - the render-owner lane's present path, the frame limiter, the settings UI,
// Lua - takes the pointer and sees one consistent monitor, never a half-updated pair of plain members.
//
// The decision logic (when to look again, what a fractional rate or a white level means, whether a
// borderless fullscreen window still covers its monitor) is plain functions here, so a unit test
// (tests/unit/display_monitor_test.cpp) runs it with no window and no device.

#include <cstdint>
#include <memory>

#ifndef _WINDEF_
struct HWND__;
typedef struct HWND__* HWND;
#endif

namespace Plat
{
    // numerator / denominator Hz, reduced: 59.94 Hz is 60000 / 1001, not 60.
    struct RefreshRate
    {
        std::uint32_t numerator   = 0;
        std::uint32_t denominator = 1;

        // 0 or 1 is how a driver says "hardware default": no rate known.
        bool   Known() const noexcept { return denominator != 0 && numerator > denominator; }
        double Hz() const noexcept { return denominator != 0 ? static_cast<double>(numerator) / denominator : 0.0; }
        // The whole Hz at or below the exact rate (59.94 -> 59): a frame cap derived from it never exceeds the panel.
        int    WholeHz() const noexcept { return denominator != 0 ? static_cast<int>(numerator / denominator) : 0; }
        bool operator==(const RefreshRate& o) const noexcept
        { return numerator == o.numerator && denominator == o.denominator; }
    };

    inline std::uint32_t Gcd(std::uint32_t a, std::uint32_t b) noexcept
    {
        while (b != 0)
        {
            const std::uint32_t t = a % b;
            a = b;
            b = t;
        }
        return a;
    }

    // A rate as the OS reports it (a DISPLAYCONFIG_RATIONAL, or an integer dmDisplayFrequency over 1).
    inline RefreshRate MakeRefreshRate(std::uint32_t numerator, std::uint32_t denominator) noexcept
    {
        if (numerator == 0 || denominator == 0)
            return {};
        const std::uint32_t g = Gcd(numerator, denominator);
        return { numerator / g, denominator / g };
    }

    // DISPLAYCONFIG_SDR_WHITE_LEVEL::SDRWhiteLevel is 1000 for 80 nits (the Windows "SDR content
    // brightness" slider, 80..480). 0 means the query failed or HDR is off: the reference 80.
    inline float SdrWhiteNitsFromLevel(std::uint32_t level) noexcept
    {
        if (level == 0)
            return 80.0f;
        float nits = static_cast<float>(level) * 80.0f / 1000.0f;
        if (nits < 80.0f)  nits = 80.0f;
        if (nits > 480.0f) nits = 480.0f;
        return nits;
    }

    struct DisplayMonitor
    {
        // Bumped on every published change; 0 = nothing published yet.
        std::uint64_t  generation = 0;
        // The HMONITOR value the snapshot describes.
        std::uintptr_t monitor    = 0;
        // The monitor's rectangle in virtual-desktop coordinates, and the desktop mode (physical pixels).
        int left = 0, top = 0, right = 0, bottom = 0;
        int desktopWidth = 0, desktopHeight = 0;
        RefreshRate refresh;
        // The DXGI output in HDR mode (DXGI_OUTPUT_DESC1) and its luminance.
        bool  hdrCapable        = false;
        float maxLuminanceNits  = 0.0f;
        float minLuminanceNits  = 0.0f;
        float maxFullFrameNits  = 0.0f;
        // Windows' SDR content brightness for this monitor, in nits (80 when it is not known).
        float sdrWhiteNits      = 80.0f;

        // Same monitor, same mode, same rate, same HDR state: only the generation differs.
        bool SameDisplay(const DisplayMonitor& o) const noexcept
        {
            return monitor == o.monitor && left == o.left && top == o.top && right == o.right && bottom == o.bottom &&
                   desktopWidth == o.desktopWidth && desktopHeight == o.desktopHeight && refresh == o.refresh &&
                   hdrCapable == o.hdrCapable && maxLuminanceNits == o.maxLuminanceNits &&
                   minLuminanceNits == o.minLuminanceNits && maxFullFrameNits == o.maxFullFrameNits &&
                   sdrWhiteNits == o.sdrWhiteNits;
        }
    };

    using DisplayMonitorPtr = std::shared_ptr<const DisplayMonitor>;

    // The OS and DXGI, read once for the monitor @p hwnd is on (the nearest one when it straddles).
    // Main thread, at a frame boundary: it creates a DXGI factory, which is not a per-frame cost.
    // A failed step leaves its fields at the safe default (not HDR, 80 nits, unknown rate).
    DisplayMonitor QueryDisplayMonitor(HWND hwnd);

    // ---- When to look again ----
    struct RefreshInputs
    {
        bool           displayChanged = false;   // WM_DISPLAYCHANGE since the last look
        std::uintptr_t windowMonitor  = 0;       // the monitor the window is on now
        std::uintptr_t knownMonitor   = 0;       // the snapshot's
        // The HDR mode asked for is not the one in effect: Windows' HDR switch moves no HMONITOR, so the
        // capability is polled until the two agree.
        bool           hdrUnsatisfied = false;
    };
    struct RefreshPoll
    {
        std::uint32_t tick  = 0;     // the caller's counter, passed in and back
        std::uint32_t every = 120;   // frames between polls while hdrUnsatisfied
    };

    // The monitor handle is compared on every look, whatever the G-Sync or HDR state: an SDR window
    // dragged to another monitor must update the desktop size and the refresh rate too.
    inline bool NeedsDisplayRefresh(const RefreshInputs& in, RefreshPoll& poll) noexcept
    {
        bool refresh = in.displayChanged || in.windowMonitor != in.knownMonitor;
        if (in.hdrUnsatisfied)
        {
            if (++poll.tick >= poll.every)
            {
                poll.tick = 0;
                refresh   = true;
            }
        }
        else
        {
            poll.tick = 0;
        }
        return refresh;
    }

    // ---- Borderless fullscreen ----
    struct RectI
    {
        int left = 0, top = 0, right = 0, bottom = 0;
        bool operator==(const RectI& o) const noexcept
        { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
    };

    // A fullscreen window must cover its monitor exactly; after a mode change, an unplugged monitor or a
    // Win+Shift+Arrow it no longer does and is placed again.
    inline bool FullscreenNeedsRefit(const RectI& window, const RectI& monitor) noexcept
    {
        const bool monitorKnown = monitor.right > monitor.left && monitor.bottom > monitor.top;
        return monitorKnown && !(window == monitor);
    }
}
