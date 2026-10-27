#pragma once
// Platform/Window.h - the Win32 main window.
//
// The HWND, its class, the message pump, the window's size / mode / DPI /
// cursor, and what its WndProc records (PointerState, KeyboardState). It owns NO
// GPU object and knows no device: App owns the device and attaches its main
// surface (AttachSurface, an ISurface), so a WM_SIZE or a mode change reaches the
// swap chain's latch while the surface exists.
//
// THE WNDPROC RULE: application logic never runs inside it. The WndProc is
// re-entrant - a MessageBox, a file dialog, SetWindowPos or DXGI's own mode
// change pump messages from inside a frame - so what it does must be safe at any
// point of the frame. It may:
//   * answer Win32 itself (SC_KEYMENU, WM_MENUCHAR, WM_GETMINMAXINFO,
//     WM_DPICHANGED, WM_QUERYENDSESSION);
//   * record (PointerState, the focus latch, the size) and queue a notification
//     (Event) - DispatchEvents hands those to their handlers at the loop top,
//     where an EventBus publish is safe;
//   * call the attached surface's latch (Resize, SetFullscreen,
//     NoteDisplayChanged), which only records a request for the frame boundary;
//   * ask a capture predicate (a pure query: "does the UI want the mouse") or a session query ("is
//     there unsaved work": the answer WM_QUERYENDSESSION needs);
//   * at WM_ENDSESSION(TRUE) - the one exception - run the session save handlers: the process is
//     terminated when the WndProc returns, so there is no loop top to wait for;
//   * run a message filter - exactly one exists: ImGui's Win32 backend
//     (installed by EditorLayer). It reads GetKeyState and calls SetCapture at
//     the moment of the message, so it cannot be replayed later.
// WM_CLOSE is swallowed and the close handlers run in Pump(), at the loop top.
// Every hook is a Subscription (Platform/Subscription.h) its owner holds.

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "System/Exception_handle.h"
#include "Platform/MainWindow.h"
#include "Platform/BackgroundPolicy.h"
#include "Platform/KeyboardState.h"
#include "Platform/PointerState.h"
#include "Platform/Subscription.h"

class ISurface;

namespace Plat
{
    enum class CaptureKind : std::uint8_t { Mouse, Keyboard };
    enum class CloseVerdict : std::uint8_t { Allow, Veto };

    // What the window is created with. The client size is in physical pixels -
    // the swap chain's size, i.e. the game's resolution. App fills it from
    // settings.json before anything else exists (Resource::PeekSettingsWindowSize).
    struct WindowDesc
    {
        int            clientWidth  = 1920;
        int            clientHeight = 1080;
        const wchar_t* title        = L"";
        // Created but not shown: the window appears at the first presented frame (the "firstFrame" boot
        // mark, when the splash closes). The device and the content take seconds, during which the main
        // thread pumps no messages; a visible window would be frozen for all of it. Ignored under
        // -hidden, which is a minimized window from the start.
        bool           startHidden  = false;
    };

    // A notification the WndProc queued. Handlers see it at the loop top
    // (Window::DispatchEvents), never inside the WndProc.
    struct Event
    {
        enum class Type : std::uint8_t
        {
            FocusChanged,     // the application gained / lost the foreground (WM_ACTIVATEAPP, deduped): focused
            DevicesChanged,   // WM_DEVICECHANGE: a controller may have come or gone
            FilesDropped,     // WM_DROPFILES (only while AcceptFileDrops(true)): paths (UTF-8), x / y client point
            ModalLoopEnded,   // the size / move modal loop ended: the next dt is wall time the game did not run
            DpiChanged,       // WM_DPICHANGED: dpi
            SessionEnding,    // WM_ENDSESSION: Windows is logging off or shutting down
            PowerChanged,     // WM_POWERBROADCAST: the machine is suspending (suspending) or has resumed
        };
        // The queue folds repeats: one DevicesChanged / ModalLoopEnded / SessionEnding stands for all the
        // ones before its dispatch, and FocusChanged / DpiChanged / PowerChanged keep the LATEST value. Only
        // FilesDropped accumulates, bounded (kMaxQueuedEvents); what it drops is counted (Stats).
        Type                     type;
        bool                     suspending = false;
        bool                     focused = false;
        int                      x = 0, y = 0;
        unsigned                 dpi = 0;
        std::vector<std::string> paths;
    };

    class Window
    {
    public:
        class Exception : public ExceptionHandle
        {
        public:
            Exception(int line, const char* file, HRESULT hr);
            const char* what() const override;
            const char* GetType() const override;
            // The system message for @p hr, as UTF-8 (FormatMessageW's wide text,
            // converted: read as a char* it would stop after the first character).
            static std::string TranslateErrorCode(HRESULT hr);
            HRESULT GetErrorCode() const;
            std::string GetErrorString() const;
        private:
            HRESULT hr;
        };

        // ---- Subscriptions ----
        using MessageFilter = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;

        // ---- Lifetime ----
        // A client that does not fit the work area of the monitor it opens on is
        // shrunk to fit (a title bar off-screen has no way back), except under
        // -hidden, which keeps its size exactly (harness determinism).
        explicit Window(const WindowDesc& desc);
        Window(int clientWidth, int clientHeight, const wchar_t* title)
            : Window(WindowDesc{ clientWidth, clientHeight, title }) {}
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        // ---- The loop top (main thread, in this order) ----
        // Drains the message queue (PeekMessage, never blocks), then runs the close
        // handlers for a WM_CLOSE it swallowed. Returns the exit code on WM_QUIT.
        std::optional<int> Pump();
        // Hands the queued notifications to the event handlers.
        void DispatchEvents();
        // Shows a window created with WindowDesc::startHidden (the boot mark does it); a no-op when it is
        // shown. From the first show on, the time between two Pump calls is measured: the longest is
        // what a person sees as "not responding" (LongestUnpumpedMs, logged when the window goes).
        void Show();
        bool IsShown() const noexcept { return m_shown; }
        MainWindowStats Stats() const noexcept;
        // While nobody can see the window (minimized, or covered: the surface's Present reports
        // DXGI_STATUS_OCCLUDED), holds the loop to about 10 Hz, waking at once on any input or message.
        // The decision is Plat::DecideBackground over the project's BackgroundPolicy: never under
        // -hidden or -unattended (a harness run IS a minimized window), and not while @p exempt (App
        // passes "the control channel is open"). -throttle_minimized forces it on (the
        // window_minimize_throttle pack).
        void ThrottleInBackground(bool exempt);
        // What this host is, for DecideBackground: the editor or a game, a harness run, an open control
        // channel (@p controlChannelOpen), and whether a pack forced the policy on.
        static BackgroundHost BackgroundHostFor(bool controlChannelOpen);
        // Dispatches the messages that are waiting, for a main thread that waits on another one (the
        // render-owner lane) and would otherwise leave its window unanswered. Not the loop top's Pump: it
        // runs no close handler. False once a WM_QUIT was seen (re-posted for the next Pump): stop
        // pumping, the wait must not spin on it.
        bool PumpWhileWaiting();
        // Apply a pending RequestFullscreen / RequestSize. MUST be called OUTSIDE
        // BeginFrame..EndFrame: both resize the window, which re-sizes the swap
        // chain the device then rebuilds at the same frame boundary.
        void ServicePendingFullscreen();
        void ServicePendingSize();

        // ---- Requests (safe anywhere on the main thread) ----
        // Recorded, applied by the services above. RequestFullscreen is a no-op under
        // -hidden (settings.json would otherwise pull the window onto the screen).
        // RequestSize is DROPPED while fullscreen: "resize me whenever you leave
        // fullscreen" is a surprise, not a feature.
        void RequestFullscreen(bool fullscreen) noexcept;
        void RequestSize(int clientW, int clientH) noexcept { m_pendingSize = SizeRequest{ clientW, clientH }; }

        bool IsFullscreen() const noexcept { return m_fullscreen; }
        // The client size in effect now (the last restored size while minimized), and
        // the one that WILL be once the latch drains - what EngineSettings::CaptureFrom
        // must record: capture REQUESTED, never EFFECTIVE, or a boot-time capture saves
        // the old size back.
        int ClientWidth()  const noexcept { return m_width;  }
        int ClientHeight() const noexcept { return m_height; }
        int RequestedClientWidth()  const noexcept { return m_pendingSize ? m_pendingSize->w : m_width;  }
        int RequestedClientHeight() const noexcept { return m_pendingSize ? m_pendingSize->h : m_height; }

        void SetTitle(const std::wstring& title);
        void EnableCursor() noexcept;
        void DisableCursor() noexcept;
        bool CursorEnabled() const noexcept { return m_cursorEnabled; }
        // Confines a captured cursor to @p clientRect (client pixels: the editor's Game View picture) instead of
        // the whole client area; null = the client area again. Re-applied at once while captured.
        void SetCursorClipRect(const RECT* clientRect) noexcept;

        // WM_DROPFILES reaches FilesDropped only while accepted (the editor).
        void AcceptFileDrops(bool accept);

        // ---- Text input (IME) ----
        // While managed, the window's input context follows CaptureKind::Keyboard (a focused text
        // field): with none, an IME has no composition window to open over the picture and keys
        // reach gameplay as typed. Applied at DispatchEvents. Unmanaged (the editor), ImGui owns it.
        void ManageImeContext(bool manage) noexcept { m_manageIme = manage; }

        // ---- What it records ----
        PointerState&       Pointer() noexcept       { return m_pointer; }
        const PointerState& Pointer() const noexcept { return m_pointer; }
        // The keyboard and the mouse buttons by VK - what Input::Update samples.
        // Recorded for every message, before the filters: the keyboard is deliberately
        // NOT gated by the UI.
        KeyboardState&       Keyboard() noexcept       { return m_keyboard; }
        const KeyboardState& Keyboard() const noexcept { return m_keyboard; }

        // ---- The surface it presents through (App's) ----
        HWND NativeHandle() const noexcept { return m_hwnd; }
        // App attaches the main surface for exactly the surface's life (App's
        // SurfaceAttachment): WM_SIZE, the fullscreen service and
        // WM_DISPLAYCHANGE reach it while attached and are dropped while not.
        void AttachSurface(ISurface* surface) noexcept { m_surface = surface; }

        // ---- Subscriptions ----
        // A filter sees every message first (after PointerState's physical view records
        // it). Returning true consumes it. See the WndProc rule in the file header: this
        // is ImGui's backend and nothing else.
        [[nodiscard]] Subscription AddMessageFilter(MessageFilter filter);
        // "Does the UI want the mouse / keyboard": the predicates are OR-ed, asked in
        // the WndProc, and must be pure queries. Each UI layer registers its own (ImGui,
        // the Canvas), so neither overwrites the other.
        [[nodiscard]] Subscription AddCapturePredicate(CaptureKind kind, std::function<bool()> predicate);
        // WM_CLOSE (the title-bar X, Alt+F4, the taskbar, the editor's Exit item) is
        // swallowed; at the next Pump the handlers run in ascending @p order and the
        // first Veto keeps the window (a vetoed close of a minimized window restores it,
        // so its modal can be seen). No veto: the window quits.
        [[nodiscard]] Subscription AddCloseHandler(int order, std::function<CloseVerdict()> handler);
        // Notifications, at DispatchEvents.
        [[nodiscard]] Subscription AddEventHandler(std::function<void(const Event&)> handler);
        // WM_QUERYENDSESSION (logoff, shutdown): a pure query, asked in the WndProc, answering the reason
        // to hold the session back (the editor's unsaved work) or an empty string. A reason is registered
        // with Windows (ShutdownBlockReasonCreate) and the query is answered FALSE, so the person sees
        // what is holding the logoff up; it is withdrawn when the session continues or is cancelled.
        [[nodiscard]] Subscription AddSessionQuery(std::function<std::wstring()> reason);
        // WM_ENDSESSION(TRUE): the process is terminated as soon as the WndProc returns, so this runs
        // synchronously there - for what must survive (the PSO library, the log). Keep it short.
        [[nodiscard]] Subscription AddSessionEndHandler(std::function<void()> save);

        // ---- Any thread ----
        // The application-foreground latch (WM_ACTIVATEAPP, deduped; seeded at
        // construction, so a handler subscribed later starts from the truth).
        static bool IsAppFocused() noexcept;
        static bool IsMinimized() noexcept;

    private:
        class WindowClass
        {
        public:
            static const wchar_t* GetName() noexcept;
            static HINSTANCE GetInstance() noexcept;
            // Registered on first use (the first Window), not during static
            // initialisation: the offline tools (-assettool, the headless UE import) never
            // open a window and so never register a class. 0 means RegisterClassEx failed;
            // RegisterError() is its GetLastError().
            static ATOM Atom() noexcept;
            static DWORD RegisterError() noexcept;

        private:
            static constexpr const wchar_t* wndClassName = L"DX12EngineWindow";

            WindowClass() noexcept;
            ~WindowClass();
            WindowClass(const WindowClass&) = delete;
            WindowClass& operator=(const WindowClass&) = delete;
            static WindowClass& Get() noexcept;

            HINSTANCE hInst;
            ATOM  atom = 0;
            DWORD registerError = 0;
        };

        // ---- Hook lists (ids are unique for the window's life) ----
        struct FilterEntry    { std::uint32_t id; MessageFilter fn; };
        struct PredicateEntry { std::uint32_t id; CaptureKind kind; std::function<bool()> fn; };
        struct CloseEntry     { std::uint32_t id; int order; std::function<CloseVerdict()> fn; };
        struct HandlerEntry   { std::uint32_t id; std::function<void(const Event&)> fn; };
        struct QueryEntry     { std::uint32_t id; std::function<std::wstring()> fn; };
        struct SaveEntry      { std::uint32_t id; std::function<void()> fn; };

        // ---- Window state ----
        static constexpr std::size_t kMaxQueuedEvents = 256;

        struct SizeRequest { int w = 0; int h = 0; };

        // ---- Subscriptions ----
        friend class Subscription;

        // ---- Internal helpers and the WndProc ----
        // PRIVATE ON PURPOSE - the only caller is ServicePendingFullscreen. It applies
        // immediately, which is legal only at a frame boundary: called from inside a
        // frame (EngineSettings::ApplyTo, Alt+Enter, the Graphics Settings checkbox) it
        // destroys a back buffer an open command list is recording against. Use
        // RequestFullscreen().
        void SetFullscreen(bool fullscreen);

        // Whether another window hides all of this one: nine points of the client area are hit-tested
        // (WindowFromPoint) every few calls, and a miss at every one is a covered window. The swap chain's
        // own answer (Present's DXGI_STATUS_OCCLUDED) is not always given - a tearing present never says it -
        // so the policy does not depend on it. Main thread.
        bool IsCovered();

        // A borderless fullscreen window that no longer covers its monitor (a display change, an unplugged
        // monitor, Win+Shift+Arrow) is placed on it again. Called by ServicePendingFullscreen, at the frame
        // boundary: the WM_SIZE it causes latches the surface's resize for the swap-chain service.
        void RefitFullscreen();

        void ConfineCursor() noexcept;
        void FreeCursor() noexcept;
        void ShowCursor() noexcept;
        void HideCursor() noexcept;
        void UpdateKeepAwake() noexcept;
        // The window became visible (Show, or any other path to a visible window): starts the pump clock.
        void NoteShown() noexcept;
        void NoteUnpumped() noexcept;
        static void OnBootMark(const char* name);
        bool UIWants(CaptureKind kind) const;
        void Push(Event e);
        void RunCloseHandlers();
        void Unsubscribe(std::uint32_t id) noexcept;

        static LRESULT CALLBACK HandleMsgSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
        static LRESULT CALLBACK HandleMsgThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
        LRESULT HandleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

        // ---- Hook lists (ids are unique for the window's life) ----
        std::vector<FilterEntry>    m_filters;
        std::vector<PredicateEntry> m_predicates;
        std::vector<CloseEntry>     m_closeHandlers;
        std::vector<HandlerEntry>   m_eventHandlers;
        std::vector<QueryEntry>     m_sessionQueries;
        std::vector<SaveEntry>      m_sessionSaves;
        std::uint32_t               m_nextId = 1;

        // ---- Window state ----
        // Notifications queued by the WndProc, handed out by DispatchEvents. Bounded:
        // nothing that queues here repeats fast, so hitting the cap means a handler-less
        // flood, and dropping is the right answer.
        std::vector<Event> m_events;
        std::uint32_t      m_eventsDropped = 0;

        // A WM_CLOSE swallowed since the last Pump.
        bool m_closeRequested = false;

        HWND          m_hwnd = nullptr;
        ISurface*     m_surface = nullptr;   // not owned: App's (AttachSurface)
        PointerState  m_pointer;
        KeyboardState m_keyboard;

        int  m_width  = 0;   // the client size; the last RESTORED one while minimized
        int  m_height = 0;
        bool m_cursorEnabled = true;
        // SetCursorClipRect's rectangle, client pixels; empty = the whole client area.
        std::optional<RECT> m_cursorClip;
        bool m_fullscreen = false;
        // ManageImeContext, and whether the window holds its default input context right now.
        bool m_manageIme     = false;
        bool m_imeAssociated = true;
        // The windowed placement and styles borderless fullscreen restores
        // (SetFullscreen): the placement keeps a maximized window maximized.
        WINDOWPLACEMENT m_savedPlacement{};
        LONG_PTR        m_savedStyle   = 0;
        LONG_PTR        m_savedExStyle = 0;
        bool            m_haveSavedPlacement = false;
        // Set by RequestFullscreen / RequestSize (main thread), consumed by the
        // services at the frame boundary.
        std::optional<bool> m_pendingFullscreen;

        std::optional<SizeRequest> m_pendingSize;

        std::vector<BYTE> m_rawBuffer;
        // Raw mouse reported as ABSOLUTE (Remote Desktop, VMs, pen tablets): the
        // previous sample in screen pixels, so WM_INPUT can turn it into a delta.
        int  m_rawAbsX = 0;
        int  m_rawAbsY = 0;
        bool m_rawAbsValid = false;

        // SIZE_MINIMIZED is a state, not a size (m_width / m_height keep the restored
        // one).
        bool m_minimized = false;
        // The size / move modal loop (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE).
        bool m_inSizeMove = false;
        // ThrottleWhileMinimized: the last wake and whether it is throttling now.
        std::chrono::steady_clock::time_point m_lastThrottleWake{};
        bool m_throttling = false;
        // Game.exe keeps the display awake while it is the foreground,
        // unminimized app (a pad-only player never touched the mouse).
        bool m_keepAwake = false;

        // ---- Visibility and responsiveness ----
        // Visible to the person (Show, the constructor's show, a fullscreen SetWindowPos). The pump
        // clock runs from then: m_lastPump is the previous Pump's start, the longest gap between two
        // is what the person could have seen frozen. The size / move modal loop pumps by itself and
        // is not counted.
        bool m_shown = false;
        bool m_pumpClock = false;
        std::chrono::steady_clock::time_point m_shownAt{};
        std::chrono::steady_clock::time_point m_lastPump{};
        double        m_longestUnpumpedMs = 0.0;
        std::uint64_t m_pumpsSinceShown   = 0;
        std::uint64_t m_longestAtPump     = 0;
        // Registered with the boot marks while a hidden window waits for its first frame.
        bool m_bootMarkListening = false;
        // The counters Stats() reports besides the ones above.
        MainWindowStats m_counts{};
        // Set when the display or the window's place may have changed under a fullscreen window
        // (WM_DISPLAYCHANGE, WM_DPICHANGED, WM_MOVE): RefitFullscreen compares at the next boundary.
        bool m_refitPending = false;
        // IsCovered's last answer and the calls since it looked.
        bool          m_covered = false;
        std::uint32_t m_coverTick = 0;
        // A ShutdownBlockReason is registered with Windows (a session query held the logoff back).
        bool m_shutdownBlocked = false;
        // PumpWhileWaiting saw WM_QUIT.
        bool m_nestedQuit = false;
    };
}

#define CHWND_EXCEPT(hr) Plat::Window::Exception(__LINE__, __FILE__, hr)
#define CHWND_LAST_EXCEPT() Plat::Window::Exception(__LINE__, __FILE__, GetLastError())
