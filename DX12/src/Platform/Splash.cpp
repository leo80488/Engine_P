#include "Platform/Splash.h"

#include "System/BootTimeline.h"
#include "System/CommandLine.h"
#include "System/PathUtil.h"   // Utf8ToWide

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace Plat::Splash
{
namespace
{
    // The boot marks the bar steps through, in the order a boot takes them; the last closes it.
    constexpr const char* kSteps[] = { "logger", "window", "preDevice", "device", "renderer", "uiFont",
                                       "runSetup", "sceneLoaded", "firstFrame" };
    constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
    constexpr UINT kMsgStep  = WM_APP + 1;   // wParam: the step reached
    constexpr int  kWidth    = 520;
    constexpr int  kHeight   = 170;

    std::mutex        g_mutex;   // guards the thread's start and join
    std::thread       g_thread;
    std::atomic<HWND> g_hwnd{ nullptr };
    std::wstring      g_title;
    std::wstring      g_line;
    int               g_step = 0;   // the splash thread's only
    bool              g_closeAtExit = true;   // -splash_leak_at_exit (harness) clears it: the unit's reverse arm

    void OnMark(const char* name)
    {
        for (int i = 0; i < kStepCount; ++i)
            if (std::strcmp(kSteps[i], name) == 0)
            {
                if (HWND h = g_hwnd.load()) ::PostMessageW(h, kMsgStep, static_cast<WPARAM>(i + 1), 0);
                return;
            }
    }

    void Paint(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        HBRUSH back = ::CreateSolidBrush(RGB(18, 18, 22));
        ::FillRect(dc, &rc, back);
        ::DeleteObject(back);
        ::SetBkMode(dc, TRANSPARENT);

        // Segoe UI links to a CJK face for the characters it lacks, so one font serves every language.
        HFONT titleFont = ::CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                        0, L"Segoe UI");
        HFONT lineFont  = ::CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                        L"Segoe UI");
        HGDIOBJ old = ::SelectObject(dc, titleFont);
        ::SetTextColor(dc, RGB(235, 235, 240));
        RECT titleRc{ 28, 34, rc.right - 28, 80 };
        ::DrawTextW(dc, g_title.c_str(), -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(dc, lineFont);
        ::SetTextColor(dc, RGB(160, 160, 170));
        RECT lineRc{ 28, 92, rc.right - 28, 116 };
        ::DrawTextW(dc, g_line.c_str(), -1, &lineRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(dc, old);
        ::DeleteObject(titleFont);
        ::DeleteObject(lineFont);

        const int barW = rc.right - 56;
        RECT track{ 28, 132, 28 + barW, 136 };
        HBRUSH trackBrush = ::CreateSolidBrush(RGB(48, 48, 56));
        ::FillRect(dc, &track, trackBrush);
        ::DeleteObject(trackBrush);
        RECT fill{ 28, 132, 28 + barW * g_step / kStepCount, 136 };
        HBRUSH fillBrush = ::CreateSolidBrush(RGB(110, 160, 255));
        ::FillRect(dc, &fill, fillBrush);
        ::DeleteObject(fillBrush);
        ::EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case kMsgStep:
            if (static_cast<int>(wParam) > g_step) g_step = static_cast<int>(wParam);
            if (g_step >= kStepCount)
            {
                ::DestroyWindow(hwnd);   // the first frame is on screen
                return 0;
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_hwnd.store(nullptr);
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void Run()
    {
        HINSTANCE inst = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = inst;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_APPSTARTING);
        wc.lpszClassName = L"DX12EngineSplash";
        ::RegisterClassExW(&wc);
        const int x = (::GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
        const int y = (::GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;
        HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, g_title.c_str(), WS_POPUP,
                                      x, y, kWidth, kHeight, nullptr, nullptr, inst, nullptr);
        if (!hwnd) return;
        g_hwnd.store(hwnd);
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd);
        MSG m;
        while (::GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        ::UnregisterClassW(wc.lpszClassName, inst);
    }
}

void Show(const std::string& title, const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_thread.joinable()) return;
    g_title = Sys::Utf8ToWide(title);
    g_line  = Sys::Utf8ToWide(line);
    g_step  = 0;
#ifdef WITH_HARNESS
    g_closeAtExit = !Sys::CommandLine::Get().Has("-splash_leak_at_exit");
#endif
    Sys::Boot::SetMarkListener(&OnMark);
    g_thread = std::thread(&Run);
}

void Close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_thread.joinable()) return;
    Sys::Boot::SetMarkListener(nullptr);
    // The thread may not have made its window yet: wait for it, then ask it to close.
    for (int i = 0; i < 200 && !g_hwnd.load(); ++i)
    {
        if (::WaitForSingleObject(g_thread.native_handle(), 0) == WAIT_OBJECT_0) break;   // already gone
        ::Sleep(5);
    }
    if (HWND h = g_hwnd.load()) ::PostMessageW(h, WM_CLOSE, 0, 0);
    g_thread.join();
}

namespace
{
    // A path that returns before the first frame (a headless import, -assettool, a refused -artifacts) never calls
    // Close: the static thread above would be destroyed still joinable while its window reads the strings beside it,
    // and the crash net would turn the run's own exit code into a crash. Defined last, so destroyed first.
    struct CloseAtExit
    {
        ~CloseAtExit()
        {
            if (g_closeAtExit)
                Close();
        }
    };
    CloseAtExit g_closeAtExitGuard;
}
}
