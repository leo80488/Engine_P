#include "Platform/Window.h"
#include "Platform/MainWindow.h"
#include "Platform/DisplayMonitor.h"   // FullscreenNeedsRefit
#include "Platform/AppResources.h"     // the icon's resource ID (src/Platform/App.rc)
#include "Graphics/ISurface.h"         // the attached surface: Resize / SetFullscreen / NoteDisplayChanged
#include "System/Log.h"
#include "System/CommandLine.h"        // -hidden / -unattended / -throttle_minimized
#include "System/HostPolicy.h"         // IsEditorHost: only Game.exe keeps the display awake
#include "System/BootTimeline.h"       // the "firstFrame" mark shows a hidden window
#include "Harness/RunLedger.h"         // run.json's window block

#include <shellapi.h>   // DragQueryFileW / DragAcceptFiles (WM_DROPFILES)
#include <imm.h>        // ImmGetVirtualKey: the real key behind an IME's VK_PROCESSKEY
#pragma comment(lib, "imm32.lib")

// std::min / std::max. NOMINMAX already comes from the force-included pch.h;
// the parenthesised (std::min)(...) form below is belt and braces.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <sstream>
#include <utility>

namespace
{
    // The application-foreground latch, and the answer IsAppFocused gives.
    // Seeded in the Window ctor so a handler subscribed later still starts from
    // the truth; updated only on a genuine WM_ACTIVATEAPP edge.
    bool g_appFocused = false;

    // The main window, for the any-thread queries (the control channel's pipe
    // thread asks IsMinimized). Set once the window exists, cleared before it goes.
    std::atomic<HWND> g_mainHwnd{ nullptr };

    // SC_KEYMENU requests swallowed (Plat::MainWindowKeyMenuSwallowed).
    std::atomic<std::uint32_t> g_keyMenuSwallowed{ 0 };

    // The main Window object, for the main-thread harness queries
    // (MainWindowConsumeTypedText). Set and cleared with g_mainHwnd.
    Plat::Window* g_mainWindow = nullptr;

    // -hidden is a minimized window by construction, and -unattended is a
    // harness run: neither is a person's minimized window. Read once; the
    // command line is parsed before any Window exists (main.cpp).
    bool HostHidden()
    {
        static const bool s_hidden = Sys::CommandLine::Get().Has("-hidden");
        return s_hidden;
    }
    bool HostUnattended()
    {
        static const bool s_unattended = Sys::CommandLine::Get().Has("-unattended");
        return s_unattended;
    }

    // -session_legacy: WM_QUERYENDSESSION always allows and WM_ENDSESSION runs no save handler, the way a
    // session end was handled before (window_session's and editor_session_block's reverse arm).
    bool HostSessionLegacy()
    {
#ifdef WITH_HARNESS
        static const bool s_on = Sys::CommandLine::Get().Has("-session_legacy");
        return s_on;
#else
        return false;
#endif
    }

    // -background_policy_off: nothing is throttled, muted or paused in the background (the reverse arm of
    // window_background_policy and window_occluded_throttle).
    bool HostBackgroundDisabled()
    {
#ifdef WITH_HARNESS
        return Sys::CommandLine::Get().Has("-background_policy_off");
#else
        return false;
#endif
    }

    // -events_unmerged: every notification takes a queue slot and a full queue drops whatever comes next,
    // a FocusChanged included (window_event_queue's reverse arm).
    bool HostEventsUnmerged()
    {
#ifdef WITH_HARNESS
        static const bool s_on = Sys::CommandLine::Get().Has("-events_unmerged");
        return s_on;
#else
        return false;
#endif
    }

    // -display_nofit: a fullscreen window is not placed on its monitor again after a display change
    // (window_display_change's reverse arm).
    bool HostNoFullscreenRefit()
    {
#ifdef WITH_HARNESS
        static const bool s_on = Sys::CommandLine::Get().Has("-display_nofit");
        return s_on;
#else
        return false;
#endif
    }

    // -altenter_records_enter: Alt+Enter's Enter reaches the keyboard table as it did before the fullscreen
    // chord consumed it (window_alt_menu's reverse arm).
    bool HostAltEnterRecordsEnter()
    {
#ifdef WITH_HARNESS
        static const bool s_on = Sys::CommandLine::Get().Has("-altenter_records_enter");
        return s_on;
#else
        return false;
#endif
    }

    // -window_boot_visible: the window shows in its constructor and its pump clock starts there, as every
    // host booted before windows waited for their first frame (window_boot_pump's reverse arm).
    bool HostBootVisible()
    {
#ifdef WITH_HARNESS
        static const bool s_visible = Sys::CommandLine::Get().Has("-window_boot_visible");
        return s_visible;
#else
        return false;
#endif
    }
}

namespace Plat
{
// ==== Any-thread queries ====
bool Window::IsAppFocused() noexcept { return g_appFocused; }
bool Window::IsMinimized() noexcept  { return MainWindowMinimized(); }

HWND MainWindowHandle() noexcept { return g_mainHwnd.load(std::memory_order_acquire); }
bool MainWindowMinimized() noexcept
{
    const HWND h = g_mainHwnd.load(std::memory_order_acquire);
    return h && ::IsIconic(h);
}
bool MainWindowAppFocused() noexcept { return g_appFocused; }
std::uint32_t MainWindowKeyMenuSwallowed() noexcept
{
    return g_keyMenuSwallowed.load(std::memory_order_relaxed);
}
std::string MainWindowConsumeTypedText()
{
    return g_mainWindow ? g_mainWindow->Keyboard().ConsumeText() : std::string();
}
std::uint32_t MainWindowKeyDownEdges(int vk) noexcept
{
    return g_mainWindow ? g_mainWindow->Keyboard().DownEdges(vk) : 0u;
}
MainWindowStats MainWindowStatsNow()
{
    return g_mainWindow ? g_mainWindow->Stats() : MainWindowStats{};
}

MainWindowStats Window::Stats() const noexcept
{
    MainWindowStats s = m_counts;
    s.shown             = m_shown;
    s.longestUnpumpedMs = m_longestUnpumpedMs;
    s.pumps             = m_pumpsSinceShown;
    s.occluded          = m_covered || (m_surface && m_surface->IsOccluded());
    s.throttling        = m_throttling;
    return s;
}

// ==== Subscriptions ====
void Subscription::Reset() noexcept
{
    if (m_window)
        m_window->Unsubscribe(m_id);
    m_window = nullptr;
    m_id = 0;
}

Subscription Window::AddMessageFilter(MessageFilter filter)
{
    const std::uint32_t id = m_nextId++;
    m_filters.push_back({ id, std::move(filter) });
    return Subscription(this, id);
}

Subscription Window::AddCapturePredicate(CaptureKind kind, std::function<bool()> predicate)
{
    const std::uint32_t id = m_nextId++;
    m_predicates.push_back({ id, kind, std::move(predicate) });
    return Subscription(this, id);
}

Subscription Window::AddCloseHandler(int order, std::function<CloseVerdict()> handler)
{
    const std::uint32_t id = m_nextId++;
    m_closeHandlers.push_back({ id, order, std::move(handler) });
    std::stable_sort(m_closeHandlers.begin(), m_closeHandlers.end(),
                     [](const CloseEntry& a, const CloseEntry& b) { return a.order < b.order; });
    return Subscription(this, id);
}

Subscription Window::AddEventHandler(std::function<void(const Event&)> handler)
{
    const std::uint32_t id = m_nextId++;
    m_eventHandlers.push_back({ id, std::move(handler) });
    return Subscription(this, id);
}

Subscription Window::AddSessionQuery(std::function<std::wstring()> reason)
{
    const std::uint32_t id = m_nextId++;
    m_sessionQueries.push_back({ id, std::move(reason) });
    return Subscription(this, id);
}

Subscription Window::AddSessionEndHandler(std::function<void()> save)
{
    const std::uint32_t id = m_nextId++;
    m_sessionSaves.push_back({ id, std::move(save) });
    return Subscription(this, id);
}

void Window::Unsubscribe(std::uint32_t id) noexcept
{
    const auto drop = [id](auto& list)
    {
        list.erase(std::remove_if(list.begin(), list.end(), [id](const auto& e) { return e.id == id; }),
                   list.end());
    };
    drop(m_filters);
    drop(m_predicates);
    drop(m_closeHandlers);
    drop(m_eventHandlers);
    drop(m_sessionQueries);
    drop(m_sessionSaves);
}

bool Window::UIWants(CaptureKind kind) const
{
    for (const PredicateEntry& p : m_predicates)
        if (p.kind == kind && p.fn && p.fn())
            return true;
    return false;
}

void Window::Push(Event e)
{
    // A notification that repeats is folded into the one already queued: the handlers get one
    // DevicesChanged for a USB hub's burst, and the LATEST focus / DPI / power state (the first one's
    // place in the queue), so a flood of one kind can never push another kind out.
    switch (HostEventsUnmerged() ? Event::Type::FilesDropped : e.type)
    {
    case Event::Type::FocusChanged:
    case Event::Type::DpiChanged:
    case Event::Type::PowerChanged:
        for (Event& queued : m_events)
            if (queued.type == e.type)
            {
                queued = std::move(e);
                ++m_counts.eventsMerged;
                return;
            }
        break;
    case Event::Type::DevicesChanged:
    case Event::Type::ModalLoopEnded:
    case Event::Type::SessionEnding:
        for (const Event& queued : m_events)
            if (queued.type == e.type)
            {
                ++m_counts.eventsMerged;
                return;
            }
        break;
    case Event::Type::FilesDropped:
        break;
    }
    if (m_events.size() >= kMaxQueuedEvents)
    {
        ++m_counts.eventsDropped;
        if (m_eventsDropped++ == 0)
            LOG_WARNING("Window: the notification queue is full (%zu) - dropping until it drains",
                        kMaxQueuedEvents);
        return;
    }
    m_events.push_back(std::move(e));
}

void Window::DispatchEvents()
{
    // The input context follows the UI's keyboard capture (ManageImeContext).
    if (m_manageIme && m_hwnd)
    {
        const bool wantText = UIWants(CaptureKind::Keyboard);
        if (wantText != m_imeAssociated)
        {
            ImmAssociateContextEx(m_hwnd, nullptr, wantText ? IACE_DEFAULT : 0);
            m_imeAssociated = wantText;
        }
    }

    if (m_events.empty())
        return;
    // Swapped out first: a handler may cause a message to be pumped (a
    // SetWindowPos, a dialog), and what that queues belongs to the next drain,
    // not to the vector being walked.
    std::vector<Event> events;
    events.swap(m_events);
    m_eventsDropped = 0;
    for (const Event& e : events)
    {
        // A copy of the handler list for the same reason: a handler may
        // subscribe or unsubscribe.
        const std::vector<HandlerEntry> handlers = m_eventHandlers;
        for (const HandlerEntry& h : handlers)
            if (h.fn)
                h.fn(e);
    }
}

void Window::AcceptFileDrops(bool accept)
{
    if (m_hwnd)
        DragAcceptFiles(m_hwnd, accept ? TRUE : FALSE);
}

// ==== The window class ====
Window::WindowClass::WindowClass() noexcept
    : hInst(GetModuleHandle(nullptr))
{
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(wc);
    // No CS_OWNDC: nothing draws with GDI, and a flip-model swap chain does not
    // use the window's DC.
    wc.style = 0;
    wc.lpfnWndProc = HandleMsgSetup;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    // hInst, not GetInstance(): this runs while Get()'s static is being
    // constructed, so calling back into Get() would recurse.
    wc.hInstance = hInst;
    // From the EXE's resources (src/Platform/App.rc, both exes through
    // Engine.App.props). An exe without it answers nullptr: the default icon.
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(DX12ENGINE_IDI_APP));
    // A NULL class cursor means DefWindowProc's WM_SETCURSOR handling sets
    // NOTHING, so the pointer keeps whatever shape the previously-hovered
    // window left it with. The editor masks this (imgui_impl_win32 calls
    // ::SetCursor every frame); Game.exe has no ImGui. Give the class an arrow.
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = GetName();
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(DX12ENGINE_IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_DEFAULTCOLOR));
    atom = RegisterClassEx(&wc);
    if (atom == 0)
        registerError = GetLastError();
}

Window::WindowClass::~WindowClass()
{
    if (atom != 0)
        UnregisterClass(wndClassName, hInst);
}

Window::WindowClass& Window::WindowClass::Get() noexcept
{
    static WindowClass s_class;
    return s_class;
}

const wchar_t* Window::WindowClass::GetName() noexcept { return wndClassName; }
HINSTANCE Window::WindowClass::GetInstance() noexcept  { return Get().hInst; }
ATOM Window::WindowClass::Atom() noexcept               { return Get().atom; }
DWORD Window::WindowClass::RegisterError() noexcept     { return Get().registerError; }

// ==== Construction ====
Window::Window(const WindowDesc& desc)
    : m_width(desc.clientWidth), m_height(desc.clientHeight)
{
    // Per-monitor V2 DPI awareness comes from the exe's manifest
    // (src/Platform/App.manifest), which applies before any window exists - the
    // frame and dialogs scale too. This is the fallback for an exe built
    // without it; with it, the call fails with ACCESS_DENIED and changes nothing.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The size is a CLIENT size in physical pixels - the swap chain's size,
    // i.e. the game's resolution - so DPI scales only the frame around it.
    constexpr DWORD kWindowStyle = WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_THICKFRAME;
    int clientWidth  = desc.clientWidth;
    int clientHeight = desc.clientHeight;
    const wchar_t* title = desc.title;

    // Fit the work area of the primary monitor (where CW_USEDEFAULT opens it). A
    // 1920x1080 client plus its frame does not fit a 1080p desktop with a taskbar,
    // let alone 1366x768 or 1080p at 125%, and Windows then shrinks the window
    // behind the swap chain's back. Not under -hidden: a minimized window has no
    // visible extent, and its size is the harness's render resolution.
    if (!HostHidden())
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            RECT frame{ 0, 0, 0, 0 };
            AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, 0, GetDpiForSystem());
            const int maxW = (mi.rcWork.right - mi.rcWork.left) - (frame.right - frame.left);
            const int maxH = (mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top);
            const int fitW = (std::max)(320, (std::min)(clientWidth,  maxW));
            const int fitH = (std::max)(240, (std::min)(clientHeight, maxH));
            if (fitW != clientWidth || fitH != clientHeight)
            {
                LOG_INFO("Window: a %dx%d client does not fit the work area - opening at %dx%d",
                         clientWidth, clientHeight, fitW, fitH);
                clientWidth  = fitW;
                clientHeight = fitH;
                m_width  = fitW;
                m_height = fitH;
            }
        }
    }

    RECT wr{ 100, 100, 100 + clientWidth, 100 + clientHeight };
    AdjustWindowRectExForDpi(&wr, kWindowStyle, FALSE, 0, GetDpiForSystem());

    if (WindowClass::Atom() == 0)
        throw Window::Exception(__LINE__, __FILE__, static_cast<HRESULT>(WindowClass::RegisterError()));

    m_hwnd = CreateWindow(
        WindowClass::GetName(), title,
        kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, WindowClass::GetInstance(), this);
    if (!m_hwnd)
        throw CHWND_LAST_EXCEPT();
    g_mainHwnd.store(m_hwnd, std::memory_order_release);
    g_mainWindow = this;

    // A throw from here to the end of the constructor means the destructor
    // never runs, so this guard takes the window down instead of leaving a
    // live HWND and a g_mainHwnd pointing at it.
    struct CreationGuard
    {
        HWND& h;
        bool  armed = true;
        ~CreationGuard()
        {
            if (!armed || !h) return;
            g_mainHwnd.store(nullptr, std::memory_order_release);
            g_mainWindow = nullptr;
            DestroyWindow(h);
            h = nullptr;
        }
    } creationGuard{ m_hwnd };

    // CW_USEDEFAULT may put the window on a monitor whose DPI is not the
    // system DPI the frame was sized for above. Re-fit the frame to the DPI the
    // window actually got, so the CLIENT is exactly the size asked for. Before
    // ShowWindow and before any device: the WM_SIZE this sends only records.
    const UINT windowDpi = GetDpiForWindow(m_hwnd);
    if (windowDpi != GetDpiForSystem())
    {
        RECT r{ 0, 0, clientWidth, clientHeight };
        AdjustWindowRectExForDpi(&r, kWindowStyle, FALSE, 0, windowDpi);
        SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    {
        const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        const char* awareness = "Unaware";
        if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            awareness = "PerMonitorV2";
        else if (GetAwarenessFromDpiAwarenessContext(ctx) == DPI_AWARENESS_PER_MONITOR_AWARE)
            awareness = "PerMonitorV1";
        else if (GetAwarenessFromDpiAwarenessContext(ctx) == DPI_AWARENESS_SYSTEM_AWARE)
            awareness = "System";
        // Both come from the manifest: a line without PerMonitorV2 / 65001 is
        // an exe that lost it (window_focus_edge pins both).
        LOG_INFO("Window: DPI awareness %s, window DPI %u, ANSI code page %u",
                 awareness, windowDpi, GetACP());
    }

    // -hidden: minimize WITHOUT activating, instead of a normal show. This is
    // NOT a headless mode - the HWND, the swap chain and the D3D12 device are
    // all real (App builds the device from this window right after it), so an
    // automated run still needs a logged-in desktop and a real GPU; -hidden
    // only keeps the window off the person's screen.
    //
    // KNOWN INTERACTION: a minimized window is never the foreground window, and
    // Input::Update clears its key / mouse state then - so NO OS input reaches
    // gameplay under -hidden. Scenarios drive through the Lua gameplay API,
    // Test.Hold / Test.Pad, or the window probe (Test.Window).
    // A window that waits for its first frame (WindowDesc::startHidden) stays invisible through the
    // boot: the device and the content take seconds in which nothing pumps its messages. -hidden shows
    // at once (minimized, no activation), and so does the harness's -window_boot_visible.
    if (!desc.startHidden || HostHidden() || HostBootVisible())
    {
        ShowWindow(m_hwnd, HostHidden() ? SW_SHOWMINNOACTIVE : SW_SHOWDEFAULT);
        NoteShown();
    }
    else
    {
        m_bootMarkListening = Sys::Boot::AddMarkListener(&Window::OnBootMark);
        if (!m_bootMarkListening)
        {
            LOG_WARNING("Window: no boot-mark listener slot free - showing the window now");
            ShowWindow(m_hwnd, SW_SHOWDEFAULT);
            NoteShown();
        }
    }

    // Seed the focus latch from the same test Input::Update uses: the
    // WM_ACTIVATEAPP that would have set it was delivered SYNCHRONOUSLY inside
    // the ShowWindow above (or not at all - the system skips it when the
    // activated window already belongs to the foreground application).
    {
        DWORD fgPid = 0;
        if (HWND fg = GetForegroundWindow())
            GetWindowThreadProcessId(fg, &fgPid);
        g_appFocused = (fgPid == GetCurrentProcessId());
    }
    m_minimized = IsIconic(m_hwnd) != FALSE;
    UpdateKeepAwake();

    // Raw mouse input (WM_INPUT) - motion counts for PointerState.
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; // mouse page
    rid.usUsage = 0x02;     // mouse usage
    rid.dwFlags = 0;
    rid.hwndTarget = nullptr;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)) == FALSE)
    {
        // A warning, not a throw: raw deltas are optional, and an unusual input stack
        // must not stop the process from starting.
        LOG_WARNING("Window: RegisterRawInputDevices failed (error %lu) - raw mouse deltas are unavailable",
                    GetLastError());
    }

    creationGuard.armed = false;
}

Window::~Window()
{
    // The device is gone by now: App declares it after the window, so it is
    // destroyed first, while the HWND its swap chain belongs to is still alive.
    // Every subscription is gone too (its owner was destroyed before the window).
    m_surface = nullptr;
    if (m_bootMarkListening)
        Sys::Boot::RemoveMarkListener(&Window::OnBootMark);
    g_mainHwnd.store(nullptr, std::memory_order_release);
    g_mainWindow = nullptr;
    if (m_pumpClock)
        LOG_INFO("[WINDOW] longest unpumped interval: unpumpedMs=%.0f pump=%llu pumps=%llu", m_longestUnpumpedMs,
                 static_cast<unsigned long long>(m_longestAtPump), static_cast<unsigned long long>(m_pumpsSinceShown));
    LOG_INFO("[WINDOW] notifications: dropped=%u merged=%u nestedPumps=%u nestedMessages=%u displayRefits=%u",
             m_counts.eventsDropped, m_counts.eventsMerged, m_counts.nestedPumps, m_counts.nestedMessages,
             m_counts.displayRefits);
    {
        const MainWindowStats s = Stats();
        Harness::RunLedger::Get().NoteWindow(s.longestUnpumpedMs, s.pumps, s.eventsDropped, s.eventsMerged,
                                             s.nestedPumps, s.displayRefits);
    }
    DestroyWindow(m_hwnd);
}

// ==== Visibility and responsiveness ====
void Window::OnBootMark(const char* name)
{
    if (g_mainWindow && std::strcmp(name, "firstFrame") == 0)
        g_mainWindow->Show();
}

void Window::Show()
{
    if (m_shown || !m_hwnd)
    {
        if (m_bootMarkListening)
        {
            Sys::Boot::RemoveMarkListener(&Window::OnBootMark);
            m_bootMarkListening = false;
        }
        return;
    }
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    // A boot that took seconds ends here: the person's own clicks in the meantime may have left
    // another window in front, and a window nobody sees defeats the point.
    if (!HostUnattended() && g_appFocused == false && GetForegroundWindow() != m_hwnd)
        SetForegroundWindow(m_hwnd);
    NoteShown();
    if (m_bootMarkListening)
    {
        Sys::Boot::RemoveMarkListener(&Window::OnBootMark);
        m_bootMarkListening = false;
    }
    LOG_INFO("[WINDOW] shown at the first frame, %.0f ms after the process started", Sys::Boot::NowMs());
}

void Window::NoteShown() noexcept
{
    if (m_shown)
        return;
    m_shown = true;
    m_pumpClock = true;
    m_shownAt = m_lastPump = std::chrono::steady_clock::now();
}

void Window::NoteUnpumped() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (m_pumpClock)
    {
        ++m_pumpsSinceShown;
        const double ms = std::chrono::duration<double, std::milli>(now - m_lastPump).count();
        if (ms > m_longestUnpumpedMs)
        {
            m_longestUnpumpedMs = ms;
            m_longestAtPump     = m_pumpsSinceShown;
        }
    }
    m_lastPump = now;
}

void Window::SetTitle(const std::wstring& title)
{
    SetWindowText(m_hwnd, title.c_str());
}

void Window::EnableCursor() noexcept
{
    m_cursorEnabled = true;
    ShowCursor();
    FreeCursor();
}

void Window::DisableCursor() noexcept
{
    m_cursorEnabled = false;
    HideCursor();
    // The clip is global: a background or hidden window (a harness run) must not trap the user's mouse.
    // WM_SETFOCUS and a click on the window confine it when it comes to the front.
    if (GetForegroundWindow() == m_hwnd)
        ConfineCursor();
}

// ==== Fullscreen and size ====
void Window::SetFullscreen(bool fullscreen)
{
    // The one source of truth for IsFullscreen(), which EngineSettings::CaptureFrom
    // saves to settings.json.
    m_fullscreen = fullscreen;

    // BORDERLESS fullscreen: the window drops its frame and covers its monitor
    // (DXGI-exclusive fails every Present on a tearing-capable swap chain - see
    // SurfaceDX12::ApplyFullscreenNow). The window owns its styling:
    // GetWindowPlacement / SetWindowPlacement keep the maximized state and the
    // restored rect apart, so a maximized window comes back maximized (a saved
    // GetWindowRect would restore it as a normal window the size of the screen).
    //
    // Each SetWindowPos delivers WM_SIZE, which latches the surface's resize; the
    // surface's own fullscreen op, latched below, then fits the swap chain to the
    // client at the same frame boundary.
    if (fullscreen)
    {
        if (!m_haveSavedPlacement)
        {
            m_savedPlacement = {};
            m_savedPlacement.length = sizeof(m_savedPlacement);
            GetWindowPlacement(m_hwnd, &m_savedPlacement);
            m_savedStyle   = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
            m_savedExStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
            m_haveSavedPlacement = true;
        }
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(m_hwnd, GWL_STYLE,
                          (m_savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                            WS_SYSMENU)) | WS_POPUP);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
                          m_savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                                             WS_EX_STATICEDGE));
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    else if (m_haveSavedPlacement)
    {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE,   m_savedStyle);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle);
        // Never come back minimized: entering fullscreen from a minimized
        // window is not a placement anyone wants restored.
        if (m_savedPlacement.showCmd == SW_SHOWMINIMIZED || m_savedPlacement.showCmd == SW_MINIMIZE)
            m_savedPlacement.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(m_hwnd, &m_savedPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_haveSavedPlacement = false;
    }

    if (m_surface)
        m_surface->SetFullscreen(fullscreen);
}

// -hidden forces this to a no-op: settings.json is applied at startup and
// re-requests whatever fullscreen state it stored, which would pull the window
// onto the person's screen and silently defeat -hidden.
void Window::RequestFullscreen(bool fullscreen) noexcept
{
    if (HostHidden()) return;
    m_pendingFullscreen = fullscreen;
}

void Window::RefitFullscreen()
{
    if (!m_refitPending)
        return;
    m_refitPending = false;
    if (!m_fullscreen || !m_hwnd || IsIconic(m_hwnd) || HostNoFullscreenRefit())
        return;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    const RectI window{ wr.left, wr.top, wr.right, wr.bottom };
    const RectI monitor{ mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom };
    if (!FullscreenNeedsRefit(window, monitor))
        return;
    SetWindowPos(m_hwnd, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                 SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    ++m_counts.displayRefits;
    LOG_INFO("Window: the fullscreen window no longer covered its monitor (%d,%d %dx%d) - placed on it again (%d,%d %dx%d)",
             window.left, window.top, window.right - window.left, window.bottom - window.top, monitor.left, monitor.top,
             monitor.right - monitor.left, monitor.bottom - monitor.top);
}

void Window::ServicePendingFullscreen()
{
    RefitFullscreen();
    if (!m_pendingFullscreen)
        return;

    const bool want = *m_pendingFullscreen;
    m_pendingFullscreen.reset();
    if (want == m_fullscreen)
        return;

    // Safe here and only here: the caller guarantees we are between frames.
    SetFullscreen(want);
    LOG_INFO("Fullscreen mode: %s", want ? "ON" : "OFF");
}

void Window::ServicePendingSize()
{
    if (!m_pendingSize)
        return;

    const SizeRequest req = *m_pendingSize;
    m_pendingSize.reset();

    // Dropped, not queued, while fullscreen - see RequestSize.
    if (m_fullscreen)
    {
        LOG_INFO("Window: ignoring a %dx%d resize request while fullscreen", req.w, req.h);
        return;
    }
    if (req.w <= 0 || req.h <= 0)
        return;
    if (req.w == m_width && req.h == m_height)
        return;   // idempotence: an unconditional Apply must not rebuild the swap chain

    // Clamp to the work area of the monitor we are on: a resolution larger than
    // the desktop is how a title bar ends up off-screen with no way back.
    int maxW = req.w, maxH = req.h;
    if (HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST))
    {
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi))
        {
            maxW = mi.rcWork.right  - mi.rcWork.left;
            maxH = mi.rcWork.bottom - mi.rcWork.top;
        }
    }
    const int wantW = (std::max)(320, (std::min)(req.w, maxW));
    const int wantH = (std::max)(240, (std::min)(req.h, maxH));

    // The request is a CLIENT size; the window takes the outer rect, at the
    // window's own DPI and its real style.
    RECT wr{ 0, 0, wantW, wantH };
    const DWORD style   = static_cast<DWORD>(GetWindowLongPtr(m_hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(m_hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, GetDpiForWindow(m_hwnd));

    if (IsIconic(m_hwnd))
    {
        // Minimized (a person's minimize, or -hidden): SetWindowPos would size the
        // minimized window, not the one the person gets back. Move the RESTORED
        // rectangle; its WM_SIZE, and the swap-chain resize, come with the restore.
        // SW_SHOWMINNOACTIVE: keep it minimized without taking focus.
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(m_hwnd, &wp))
        {
            wp.rcNormalPosition.right  = wp.rcNormalPosition.left + (wr.right - wr.left);
            wp.rcNormalPosition.bottom = wp.rcNormalPosition.top  + (wr.bottom - wr.top);
            wp.showCmd = SW_SHOWMINNOACTIVE;
            SetWindowPlacement(m_hwnd, &wp);
        }
        LOG_INFO("Window: client size -> %dx%d when restored (minimized now)%s", wantW, wantH,
                 (wantW != req.w || wantH != req.h) ? " (clamped to the monitor work area)" : "");
        return;
    }

    // Legal here and only here: the caller guarantees we are between frames.
    // SetWindowPos dispatches WM_SIZE synchronously, which latches the surface's
    // resize for the swap-chain service right after.
    SetWindowPos(m_hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    LOG_INFO("Window: client size -> %dx%d%s", wantW, wantH,
             (wantW != req.w || wantH != req.h) ? " (clamped to the monitor work area)" : "");
}

// ==== The loop top ====
std::optional<int> Window::Pump()
{
    NoteUnpumped();
    MSG msg;
    for (;;)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return static_cast<int>(msg.wParam);
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!m_closeRequested)
            return {};
        // The close handlers run HERE, at the loop top, not in the WndProc. Allowing
        // posts WM_QUIT, which the next round of the loop above returns - so a close
        // ends the run at this loop top.
        m_closeRequested = false;
        RunCloseHandlers();
    }
}

bool Window::PumpWhileWaiting()
{
    if (m_nestedQuit)
        return false;
    ++m_counts.nestedPumps;
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            // Not ours to act on: the next Pump returns it. Re-posted, and the wait stops pumping so it
            // does not spin on a quit that never leaves the queue.
            PostQuitMessage(static_cast<int>(msg.wParam));
            m_nestedQuit = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        ++m_counts.nestedMessages;
    }
    // The window answered: this is not an unpumped interval.
    m_lastPump = std::chrono::steady_clock::now();
    return true;
}

void Window::RunCloseHandlers()
{
    // A copy: a handler may subscribe or unsubscribe (the editor's modal).
    const std::vector<CloseEntry> handlers = m_closeHandlers;
    for (const CloseEntry& h : handlers)
    {
        if (h.fn && h.fn() == CloseVerdict::Veto)
        {
            // Vetoed from the taskbar's Close while minimized: the answer the
            // host now wants (the editor's Save / Discard modal, a game's
            // quit-confirm screen) is drawn in a window nobody can see. Bring it
            // back. Never under -hidden: that window stays off the screen.
            if (IsIconic(m_hwnd) && !HostHidden())
                ShowWindow(m_hwnd, SW_RESTORE);
            return;
        }
    }
    PostQuitMessage(0);
}

bool Window::IsCovered()
{
    constexpr std::uint32_t kEvery = 6;   // loop tops between looks: ~10 per second at 60 fps
    if (m_coverTick++ % kEvery != 0)
        return m_covered;
    m_covered = false;
    RECT rc{};
    if (!m_shown || !m_hwnd || IsIconic(m_hwnd) || !IsWindowVisible(m_hwnd) || !GetClientRect(m_hwnd, &rc) ||
        rc.right <= 0 || rc.bottom <= 0)
        return false;
    for (int gy = 1; gy <= 5; gy += 2)
        for (int gx = 1; gx <= 5; gx += 2)
        {
            POINT p{ rc.right * gx / 6, rc.bottom * gy / 6 };
            ClientToScreen(m_hwnd, &p);
            const HWND hit = WindowFromPoint(p);
            if (hit && GetAncestor(hit, GA_ROOT) == m_hwnd)
                return false;   // this point shows our window
        }
    m_covered = true;
    return true;
}

BackgroundHost Window::BackgroundHostFor(bool controlChannelOpen)
{
    // -throttle_minimized forces the throttle on in a harness run (window_minimize_throttle);
    // -background_policy forces the whole policy, the focus half included (window_background_policy).
    static const bool s_forced = []
    {
        const Sys::CommandLine& cl = Sys::CommandLine::Get();
        bool forced = cl.Has("-throttle_minimized");
#ifdef WITH_HARNESS
        forced = forced || cl.Has("-background_policy");
#endif
        return forced;
    }();
    // -background_policy_off: the reverse arm of the packs that judge the policy.
    static const bool s_disabled = HostBackgroundDisabled();
    return BackgroundHost{ Sys::IsEditorHost(), HostHidden() || HostUnattended(), controlChannelOpen, s_forced,
                           s_disabled };
}

void Window::ThrottleInBackground(bool exempt)
{
    // About 10 Hz: enough to keep audio, the control channel and a slow job
    // moving, at a fraction of the GPU and CPU an unthrottled minimized window burns.
    constexpr auto kInterval = std::chrono::milliseconds(100);

    const auto now = std::chrono::steady_clock::now();
    BackgroundState state;
    state.minimized = m_minimized && m_hwnd && IsIconic(m_hwnd);
    state.occluded  = m_shown && ((m_surface && m_surface->IsOccluded()) || IsCovered());
    state.focused   = g_appFocused;
    const BackgroundDecision decision = DecideBackground(CurrentBackgroundPolicy(), state, BackgroundHostFor(exempt));
    const bool want = decision.throttle;
    if (want != m_throttling)
    {
        m_throttling = want;
        if (want)
            LOG_INFO("Window: %s - the frame loop is throttled to about 10 Hz", decision.throttleReason);
        else
            LOG_INFO("Window: throttling off");
    }
    if (!want)
    {
        m_lastThrottleWake = now;
        return;
    }

    const auto elapsed = now - m_lastThrottleWake;
    if (elapsed < kInterval)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(kInterval - elapsed);
        // Wakes early on ANY input or posted message (a restore, a close, a
        // device change); the messages themselves are pumped at the next Pump.
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(remaining.count()), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
    m_lastThrottleWake = std::chrono::steady_clock::now();
}

void Window::UpdateKeepAwake() noexcept
{
    // Game.exe only, and only while it is what the person is looking at. The
    // editor is a tool: Windows' idle policy is right for it. ES_CONTINUOUS
    // alone clears the request (per thread; this is the main thread).
    const bool want = !Sys::IsEditorHost() && !HostHidden() && g_appFocused && !m_minimized;
    if (want == m_keepAwake)
        return;
    m_keepAwake = want;
    SetThreadExecutionState(want ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                 : ES_CONTINUOUS);
    LOG_INFO("Window: %s", want ? "keeping the display awake (foreground game)" : "display idle policy restored");
}

// ==== The cursor ====
void Window::SetCursorClipRect(const RECT* clientRect) noexcept
{
    if (clientRect) m_cursorClip = *clientRect;
    else            m_cursorClip.reset();
    if (!m_cursorEnabled && GetForegroundWindow() == m_hwnd)
        ConfineCursor();
}

void Window::ConfineCursor() noexcept
{
    RECT rect;
    if (m_cursorClip) rect = *m_cursorClip;
    else              GetClientRect(m_hwnd, &rect);
    MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&rect), 2);
    ClipCursor(&rect);
}

void Window::FreeCursor() noexcept
{
    ClipCursor(nullptr);
}

void Window::HideCursor() noexcept
{
    while (::ShowCursor(FALSE) >= 0);
}

void Window::ShowCursor() noexcept
{
    while (::ShowCursor(TRUE) < 0);
}

// ==== The WndProc ====
LRESULT Window::HandleMsgSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    if (msg == WM_NCCREATE)
    {
        const CREATESTRUCTW* const pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        Window* const pWnd = static_cast<Window*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWnd));
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::HandleMsgThunk));
        return pWnd->HandleMsg(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMsgThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    Window* const pWnd = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    // The last message a window receives. Forget the Window so nothing that
    // arrives for this HWND afterwards can reach an object being destroyed.
    if (msg == WM_NCDESTROY)
    {
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    if (!pWnd)
        return DefWindowProc(hwnd, msg, wParam, lParam);
    return pWnd->HandleMsg(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    // The PHYSICAL pointer, for every message and before any capture layer can
    // consume it: the retained Canvas UI needs it precisely while ImGui or the
    // Canvas owns the gesture. The gameplay view below stays capture-gated.
    switch (msg)
    {
    case WM_MOUSEMOVE:
    {
        const POINTS pt = MAKEPOINTS(lParam);
        m_pointer.OnPhysicalMove(pt.x, pt.y);
        break;
    }
    case WM_MOUSEWHEEL:
        // Recorded here, before the gameplay switch drops it on a UI capture:
        // the one time a scroll view wants the wheel is when the UI owns it.
        m_pointer.OnPhysicalWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    case WM_MOUSEHWHEEL:
        m_pointer.OnPhysicalHWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    {
        const POINTS pt = MAKEPOINTS(lParam);
        m_pointer.OnPhysicalLeft(msg == WM_LBUTTONDOWN, pt.x, pt.y);
        m_keyboard.OnMouseButton(VK_LBUTTON, msg == WM_LBUTTONDOWN);
        break;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    {
        const POINTS pt = MAKEPOINTS(lParam);
        m_pointer.OnPhysicalRight(msg == WM_RBUTTONDOWN, pt.x, pt.y);
        m_keyboard.OnMouseButton(VK_RBUTTON, msg == WM_RBUTTONDOWN);
        break;
    }
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        m_pointer.OnPhysicalMiddle(msg == WM_MBUTTONDOWN);
        m_keyboard.OnMouseButton(VK_MBUTTON, msg == WM_MBUTTONDOWN);
        break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    {
        const int which = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 1 : 2;
        m_pointer.OnPhysicalX(which, msg == WM_XBUTTONDOWN);
        m_keyboard.OnMouseButton(which == 1 ? VK_XBUTTON1 : VK_XBUTTON2, msg == WM_XBUTTONDOWN);
        break;
    }
    // The keyboard, for Input::Update. Recorded for every message, before the
    // filters - the keyboard is deliberately NOT gated by the UI. A key an IME is
    // composing comes as VK_PROCESSKEY, and ImmGetVirtualKey names the real one, so
    // WASD still moves with a Chinese or Japanese input method switched on.
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        int  vk     = static_cast<int>(wParam);
        bool viaIme = false;
        if (vk == VK_PROCESSKEY)
        {
            vk     = static_cast<int>(ImmGetVirtualKey(hwnd));
            viaIme = true;
        }
        // Alt+Enter is the fullscreen toggle (below): its Enter must not also reach the keyboard,
        // where it would confirm a UI screen or submit a Canvas field.
        if (vk == VK_RETURN && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && !HostAltEnterRecordsEnter())
            break;
        if (vk != VK_PROCESSKEY && vk != 0)
            m_keyboard.OnKey(vk, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN,
                             static_cast<std::uint32_t>(lParam), viaIme);
        break;
    }
    case WM_CHAR:
        m_keyboard.OnChar(static_cast<wchar_t>(wParam));
        break;
    default:
        break;
    }

    // The message filters (ImGui's backend) see it next. True = consumed.
    for (const FilterEntry& f : m_filters)
        if (f.fn && f.fn(hwnd, msg, wParam, lParam))
            return true;

    switch (msg)
    {
    // Files dragged in from Explorer (accepted only while AcceptFileDrops).
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        Event e{ Event::Type::FilesDropped };
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len, L'\0');
            if (DragQueryFileW(drop, i, w.data(), len + 1) == 0) continue;
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                              nullptr, nullptr);
            std::string u(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
            e.paths.push_back(std::move(u));
        }
        POINT pt{};
        DragQueryPoint(drop, &pt);
        DragFinish(drop);
        e.x = pt.x;
        e.y = pt.y;
        if (!e.paths.empty())
            Push(std::move(e));
        return 0;
    }
    // Swallowed: returning 0 without PostQuitMessage keeps the window alive.
    // The close handlers (a GameFlow quit-confirm state, the editor's unsaved
    // changes modal) run at the next Pump, at the loop top - see Pump.
    case WM_CLOSE:
        m_closeRequested = true;
        return 0;
    // Windows is logging off or shutting down. QUERY: a handler that has unsaved work (the editor)
    // names the reason; Windows is told (ShutdownBlockReasonCreate) and the logoff waits for the person's
    // answer - the modal itself cannot run, the session does not wait for a frame loop. END: the process
    // is terminated as soon as this returns, so what must survive is saved HERE, synchronously, and the
    // log flushed, or the last seconds of it are lost.
    case WM_QUERYENDSESSION:
    {
        ++m_counts.queryEndSessions;
        std::wstring reason;
        for (const QueryEntry& q : m_sessionQueries)
            if (q.fn && reason.empty() && !HostSessionLegacy())
                reason = q.fn();
        if (!reason.empty())
        {
            ShutdownBlockReasonCreate(hwnd, reason.c_str());
            if (!m_shutdownBlocked)
                ++m_counts.shutdownBlocks;
            m_shutdownBlocked = true;
            LOG_WARNING("Window: the Windows session asked to end - held back by unsaved work (%ls)", reason.c_str());
            return FALSE;
        }
        if (m_shutdownBlocked)
        {
            ShutdownBlockReasonDestroy(hwnd);
            m_shutdownBlocked = false;
        }
        return TRUE;
    }
    case WM_ENDSESSION:
        if (m_shutdownBlocked)
        {
            ShutdownBlockReasonDestroy(hwnd);
            m_shutdownBlocked = false;
        }
        if (wParam)
        {
            ++m_counts.endSessions;
            LOG_WARNING("Window: the Windows session is ending - the process will be terminated");
            for (const SaveEntry& s : m_sessionSaves)
            {
                if (HostSessionLegacy())
                    break;
                try { if (s.fn) s.fn(); }
                catch (const std::exception& e) { LOG_ERROR("Window: a session-end save failed: %s", e.what()); }
            }
            Logger::Get().FlushBlocking();
            Push(Event{ Event::Type::SessionEnding });
        }
        else
        {
            LOG_INFO("Window: the Windows session end was cancelled");
        }
        return 0;
    // Suspend / resume: the machine slept (or is about to). A resume is a discontinuity for the clock and
    // often a different display set, so the surface looks at the display again and a fullscreen window is
    // fitted to its monitor.
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
        {
            const bool suspending = wParam == PBT_APMSUSPEND;
            ++(suspending ? m_counts.powerSuspends : m_counts.powerResumes);
            if (!suspending)
            {
                if (m_surface)
                    m_surface->NoteDisplayChanged();
                m_refitPending = true;
            }
            Event e{ Event::Type::PowerChanged };
            e.suspending = suspending;
            Push(std::move(e));
        }
        return TRUE;
    // The size / move modal loop: while the person drags the frame or the title
    // bar, DispatchMessage does not return and no frame runs.
    case WM_ENTERSIZEMOVE:
        m_inSizeMove = true;
        break;
    case WM_EXITSIZEMOVE:
        if (m_inSizeMove)
        {
            m_inSizeMove = false;
            m_lastPump = std::chrono::steady_clock::now();   // the modal loop pumped by itself
            Push(Event{ Event::Type::ModalLoopEnded });
        }
        break;
    // Visible by any path (a fullscreen SetWindowPos with SWP_SHOWWINDOW included).
    case WM_SHOWWINDOW:
        if (wParam)
            NoteShown();
        break;
    // The desktop's mode, the monitor set or the Windows HDR switch changed:
    // let the surface re-read refresh rate, desktop size and HDR capability.
    case WM_DISPLAYCHANGE:
        if (m_surface)
            m_surface->NoteDisplayChanged();
        m_refitPending = true;
        break;
    // A controller was plugged in or unplugged: the input layer re-probes at
    // once instead of waiting for its throttled rescan.
    case WM_DEVICECHANGE:
        Push(Event{ Event::Type::DevicesChanged });
        break;
    case WM_SIZE:
    {
        const UINT w = static_cast<UINT>(LOWORD(lParam));
        const UINT h = static_cast<UINT>(HIWORD(lParam));
        // SIZE_MINIMIZED reports 0x0. Keep the last restored size instead: it
        // is what settings capture, and what the window comes back at.
        const bool minimizedNow = (wParam == SIZE_MINIMIZED);
        if (minimizedNow != m_minimized)
        {
            m_minimized = minimizedNow;
            UpdateKeepAwake();
        }
        if (!minimizedNow)
        {
            m_width  = static_cast<int>(w);
            m_height = static_cast<int>(h);
        }
        if (m_surface && w > 0 && h > 0)
            m_surface->Resize(w, h);
        // A captured cursor's clip is in SCREEN space, so a new size leaves it
        // describing the old one. Never while minimized: the client rect is
        // empty and ClipCursor would pin the pointer to a point.
        if (!m_cursorEnabled && w > 0 && h > 0 && GetForegroundWindow() == hwnd)
            ConfineCursor();
        return 0;
    }
    // Moved onto a monitor with another DPI. Windows suggests a rect that keeps
    // the window's LOGICAL size, which would change the client's pixel size -
    // the game's resolution - just because the window crossed a monitor edge.
    // Keep the client's pixels: take the suggested position, re-fit only the
    // frame. Borderless fullscreen covers its monitor and is left alone.
    case WM_DPICHANGED:
    {
        if (!m_fullscreen)
        {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            RECT client{};
            GetClientRect(hwnd, &client);
            RECT r{ 0, 0, client.right - client.left, client.bottom - client.top };
            AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE)), FALSE,
                                     static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE)), HIWORD(wParam));
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            LOG_INFO("Window: DPI changed to %u - client kept at %ldx%ld", static_cast<unsigned>(HIWORD(wParam)),
                     client.right - client.left, client.bottom - client.top);
        }
        else
        {
            m_refitPending = true;   // fullscreen: covers its monitor, whatever the monitor's DPI
        }
        Event e{ Event::Type::DpiChanged };
        e.dpi = HIWORD(wParam);
        Push(std::move(e));
        return 0;
    }
    // The smallest window a drag may make: a 320x240 client (the floor
    // ServicePendingSize clamps a settings request to), plus the frame.
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT r{ 0, 0, 320, 240 };
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_MOVE:
        // A fullscreen window moved off its monitor (Win+Shift+Arrow): RefitFullscreen compares at the boundary.
        if (m_fullscreen)
            m_refitPending = true;
        // Same reason as WM_SIZE: the clip rect is in screen space.
        if (!m_cursorEnabled && !IsIconic(hwnd) && GetForegroundWindow() == hwnd)
            ConfineCursor();
        break;
    // A lone Alt or F10 (SC_KEYMENU with lParam 0), or Alt+letter, makes
    // DefWindowProc enter its menu loop: a modal loop inside DispatchMessage,
    // so the frame loop stops until another key arrives. There is no Win32
    // menu bar to open. Alt+Space (lParam ' ') still opens the window menu.
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam != ' ')
        {
            g_keyMenuSwallowed.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        break;
    // Alt+letter that matches no mnemonic: close the menu quietly instead of
    // the MessageBeep DefWindowProc would play.
    case WM_MENUCHAR:
        return MAKELRESULT(0, MNC_CLOSE);
    // Focus lost: every button reads released (a button held through an
    // alt-tab sends its WM_*BUTTONUP to the other application).
    case WM_KILLFOCUS:
        m_pointer.ReleaseAll();
        // Every key reads released too: the WM_KEYUP of a key let go while
        // another application has focus goes to that application.
        m_keyboard.Clear();
        // The next absolute raw sample after focus returns starts a new run.
        m_rawAbsValid = false;
        break;
    // Focus is back: what is held RIGHT NOW is the truth (a key pressed in the
    // other application and still down here, or released there). Re-read it.
    case WM_SETFOCUS:
        m_keyboard.ResyncFromAsync();
        break;
    // WM_XBUTTON* expect TRUE when handled (the side buttons are recorded above).
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return TRUE;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))
        {
            // Deferred through the request, like the settings panel: applying a
            // fullscreen change here would be safe only by accident of where the pump runs.
            // Bit 30 is the previous key state: skip auto-repeat, or holding Alt+Enter
            // toggles at the key-repeat rate.
            if (!(lParam & 0x40000000))
                RequestFullscreen(!m_fullscreen);
            return 0;
        }
        break;
    // The GAMEPLAY pointer: presses, moves and the wheel dropped while the UI
    // wants the mouse; releases never (a gesture captured mid-drag must not
    // leave a button latched).
    case WM_MOUSEMOVE:
    {
        if (UIWants(CaptureKind::Mouse))
            break;
        const POINTS pt = MAKEPOINTS(lParam);
        if (pt.x >= 0 && pt.x < m_width && pt.y >= 0 && pt.y < m_height)
        {
            m_pointer.OnMove(pt.x, pt.y);
            if (!m_pointer.IsInWindow())
            {
                SetCapture(hwnd);
                m_pointer.OnEnter();
            }
        }
        else if (m_pointer.LeftIsPressed() || m_pointer.RightIsPressed())
        {
            m_pointer.OnMove(pt.x, pt.y);
        }
        else
        {
            ReleaseCapture();
            m_pointer.OnLeave();
        }
        break;
    }
    case WM_LBUTTONDOWN:
        SetForegroundWindow(hwnd);
        if (!m_cursorEnabled)
        {
            ConfineCursor();
            HideCursor();
        }
        if (!UIWants(CaptureKind::Mouse))
            m_pointer.OnLeft(true);
        break;
    case WM_RBUTTONDOWN:
        if (!UIWants(CaptureKind::Mouse))
            m_pointer.OnRight(true);
        break;
    case WM_LBUTTONUP:
        m_pointer.OnLeft(false);
        break;
    case WM_RBUTTONUP:
        m_pointer.OnRight(false);
        break;
    case WM_MOUSEWHEEL:
        if (!UIWants(CaptureKind::Mouse))
            m_pointer.OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        break;

    // The application-level foreground edge (alt-tab), as opposed to
    // WM_ACTIVATE below: sent ONLY when activation crosses an application
    // boundary, so a modal dialog this process owns does not count. Deduped
    // here, so every handler sees edges, not the repeats the message comes with.
    case WM_ACTIVATEAPP:
    {
        const bool focused = (wParam != FALSE);
        if (focused != g_appFocused)
        {
            g_appFocused = focused;
            Event e{ Event::Type::FocusChanged };
            e.focused = focused;
            Push(std::move(e));
            UpdateKeepAwake();
        }
        break;
    }
    case WM_ACTIVATE:
        // Confine / free a disabled cursor as the window gains / loses
        // activation. LOWORD is WA_INACTIVE (0), WA_ACTIVE (1) or
        // WA_CLICKACTIVE (2) - a click-activation is an activation.
        if (!m_cursorEnabled)
        {
            if (LOWORD(wParam) != WA_INACTIVE)
            {
                ConfineCursor();
                HideCursor();
            }
            else
            {
                FreeCursor();
                ShowCursor();
            }
        }
        break;

    // Raw mouse motion.
    case WM_INPUT:
    {
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
            break;
        m_rawBuffer.resize(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, m_rawBuffer.data(), &size,
                            sizeof(RAWINPUTHEADER)) != size)
            break;
        const auto& ri = reinterpret_cast<const RAWINPUT&>(*m_rawBuffer.data());
        if (ri.header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE& rm = ri.data.mouse;
            int dx = rm.lLastX;
            int dy = rm.lLastY;
            if (rm.usFlags & MOUSE_MOVE_ABSOLUTE)
            {
                // Remote Desktop, VMs and pen tablets report a POSITION
                // normalised to 0..65535, not a relative count. Turn it into a
                // pixel delta against the previous absolute sample.
                const bool virtualDesk = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                const int  sw = GetSystemMetrics(virtualDesk ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
                const int  sh = GetSystemMetrics(virtualDesk ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
                const int  ax = static_cast<int>((static_cast<float>(rm.lLastX) / 65535.0f) * sw);
                const int  ay = static_cast<int>((static_cast<float>(rm.lLastY) / 65535.0f) * sh);
                dx = m_rawAbsValid ? ax - m_rawAbsX : 0;
                dy = m_rawAbsValid ? ay - m_rawAbsY : 0;
                m_rawAbsX = ax;
                m_rawAbsY = ay;
                m_rawAbsValid = true;
            }
            else
            {
                m_rawAbsValid = false;
            }
            if (dx != 0 || dy != 0)
                m_pointer.OnRawDelta(dx, dy);
        }
        break;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ==== Exception ====
Window::Exception::Exception(int line, const char* file, HRESULT hr)
    : ExceptionHandle(line, file), hr(hr)
{
}

const char* Window::Exception::what() const
{
    std::ostringstream oss;
    oss << GetType() << std::endl
        << "[Error Code] " << GetErrorCode() << std::endl
        << "[Description] " << GetErrorString() << std::endl
        << GetOriginString();
    whatBuffer = oss.str();
    return whatBuffer.c_str();
}

const char* Window::Exception::GetType() const
{
    return "Window Exception";
}

std::string Window::Exception::TranslateErrorCode(HRESULT hr)
{
    wchar_t* pMsgBuf = nullptr;
    const DWORD nMsgLen = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&pMsgBuf), 0, nullptr);
    if (nMsgLen == 0 || pMsgBuf == nullptr)
        return "Unidentified error code";
    const int n = WideCharToMultiByte(CP_UTF8, 0, pMsgBuf, static_cast<int>(nMsgLen), nullptr, 0, nullptr, nullptr);
    std::string errorString(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, pMsgBuf, static_cast<int>(nMsgLen), errorString.data(), n, nullptr, nullptr);
    LocalFree(pMsgBuf);
    return errorString;
}

HRESULT Window::Exception::GetErrorCode() const
{
    return hr;
}

std::string Window::Exception::GetErrorString() const
{
    return TranslateErrorCode(hr);
}
}   // namespace Plat
