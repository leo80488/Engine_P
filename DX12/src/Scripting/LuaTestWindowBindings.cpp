// LuaTestWindowBindings.cpp - Test.Window / Test.WindowState / Test.InputFocus / Test.TypedText: the
// harness's window probe. Registered only for a harness run (WITH_HARNESS), from the Test table
// ScriptSystem_Bindings.cpp builds.

#include "Scripting/LuaDoc.h"

#ifdef WITH_HARNESS

#include "Input/InputSystem.h"
#include "Platform/MainWindow.h"

#include <sol/sol.hpp>

#include <optional>
#include <string>
#include <tuple>

namespace Scripting
{
namespace
{
    // Test.Window("cover")'s popup.
    HWND g_cover = nullptr;
}

void RegisterLuaTestWindow(sol::table& test, int (*resolveKey)(sol::this_state, const sol::object&))
{
    // ---- The window probe ----
    // Test.Window(kind [, a, b, c]) POSTS a real message to the main window, so a pack
    // drives the WndProc itself - the close veto, the focus edge, Alt / F10, keys, the
    // mouse - where OS input cannot reach (a -hidden window is never foreground). Unlike
    // Test.Hold it is not a game-action injector: the message goes through the same
    // Plat::Window::HandleMsg a real one does, at the next message pump (the loop top).
    //   "close" | "activate" | "deactivate" | "minimize" | "restore" | "maximize"
    //   "key_down" | "key_up" | "syskey_down" | "syskey_up"   a = key (VK or Keys name)
    //   "char"                                               a = UTF-16 code unit
    //   "mouse_move" | "lbutton_down" | "lbutton_up" |
    //   "rbutton_down" | "rbutton_up" | "mbutton_down" | "mbutton_up" |
    //   "x1_down" | "x1_up" | "x2_down" | "x2_up"            a, b = client x, y
    //   "wheel" | "hwheel"                                   a = delta (120 = one notch), b, c = client x, y
    //   "cover" | "uncover"                                  a topmost popup over the whole window / gone again
    //   "display_change"                                     WM_DISPLAYCHANGE for the desktop's current mode
    //   "move"                                               a, b = screen x, y: SetWindowPos, synchronous
    //   "devices_changed"                                    a = how many WM_DEVICECHANGE (default 1)
    //   "power_suspend" | "power_resume"                     WM_POWERBROADCAST, sent
    //   "query_end_session" | "end_session"                  sent; the answer is the second return value
    //                                                        ("allow" / "block"); end_session a = 1 (ending) | 0
    // Returns true, or false and a reason. Never raises (see the Test.* note).
    LuaDoc::Fn(test, "Test", "Window", [resolveKey](sol::this_state ts, const std::string& kind,
                                             sol::optional<sol::object> a, sol::optional<int> b,
                                             sol::optional<int> c) -> std::tuple<bool, std::string>
    {
        const HWND h = Plat::MainWindowHandle();
        if (!h) return { false, "no main window" };
        const auto arg = [&](int def) -> int
        {
            if (!a || !a->valid() || a->get_type() == sol::type::lua_nil) return def;
            return resolveKey(ts, *a);
        };
        const auto keyParam = [](int vk, bool up, bool sys) -> LPARAM
        {
            // repeat count 1, the scan code, the context bit (Alt held) for
            // the SYS forms, and on release the previous-state and
            // transition bits - the layout a real keystroke carries.
            const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            LPARAM lp = 1 | (static_cast<LPARAM>(sc & 0xFF) << 16);
            if (sys) lp |= (static_cast<LPARAM>(1) << 29);
            if (up)  lp |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
            return lp;
        };
        const auto post = [h](UINT msg, WPARAM wp, LPARAM lp) -> std::tuple<bool, std::string>
        {
            if (!PostMessageW(h, msg, wp, lp))
                return { false, "PostMessage failed (" + std::to_string(GetLastError()) + ")" };
            return { true, std::string() };
        };
        const auto send = [h](UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) -> std::tuple<bool, std::string>
        {
            DWORD_PTR result = 0;
            if (!SendMessageTimeoutW(h, msg, wp, lp, SMTO_ABORTIFHUNG, 5000, &result))
                return { false, "SendMessage failed (" + std::to_string(GetLastError()) + ")" };
            out = static_cast<LRESULT>(result);
            return { true, std::string() };
        };
        const auto xy = [](int x, int y) -> LPARAM
        {
            return static_cast<LPARAM>(MAKELPARAM(static_cast<WORD>(static_cast<short>(x)),
                                                  static_cast<WORD>(static_cast<short>(y))));
        };
        const int bx = b.value_or(0), cy = c.value_or(0);

        if (kind == "close")      return post(WM_CLOSE, 0, 0);
        if (kind == "activate")   return post(WM_ACTIVATEAPP, TRUE, 0);
        if (kind == "deactivate") return post(WM_ACTIVATEAPP, FALSE, 0);
        // NOACTIVATE forms: a pack must not take the person's focus. ShowWindow
        // delivers WM_SIZE synchronously; the device latches it as it would
        // a person's minimize.
        if (kind == "minimize")   { ShowWindow(h, SW_SHOWMINNOACTIVE); return { true, std::string() }; }
        if (kind == "restore")    { ShowWindow(h, SW_SHOWNOACTIVATE);  return { true, std::string() }; }
        // No NOACTIVATE form exists for maximize; only the visible-window
        // packs use it (window_fullscreen_toggle's F10 round).
        if (kind == "maximize")   { ShowWindow(h, SW_MAXIMIZE);        return { true, std::string() }; }
        if (kind == "key_down" || kind == "key_up" || kind == "syskey_down" || kind == "syskey_up")
        {
            const int vk = arg(0);
            if (vk <= 0 || vk > 0xFE) return { false, "unknown key" };
            const bool up  = kind == "key_up" || kind == "syskey_up";
            const bool sys = kind == "syskey_down" || kind == "syskey_up";
            const UINT msg = sys ? (up ? WM_SYSKEYUP : WM_SYSKEYDOWN) : (up ? WM_KEYUP : WM_KEYDOWN);
            return post(msg, static_cast<WPARAM>(vk), keyParam(vk, up, sys));
        }
        if (kind == "char")
        {
            const int cu = arg(0);
            if (cu <= 0 || cu > 0xFFFF) return { false, "char needs a UTF-16 code unit" };
            return post(WM_CHAR, static_cast<WPARAM>(cu), 1);
        }
        if (kind == "mouse_move")   return post(WM_MOUSEMOVE,   0,          xy(arg(0), bx));
        if (kind == "lbutton_down") return post(WM_LBUTTONDOWN, MK_LBUTTON, xy(arg(0), bx));
        if (kind == "lbutton_up")   return post(WM_LBUTTONUP,   0,          xy(arg(0), bx));
        if (kind == "rbutton_down") return post(WM_RBUTTONDOWN, MK_RBUTTON, xy(arg(0), bx));
        if (kind == "rbutton_up")   return post(WM_RBUTTONUP,   0,          xy(arg(0), bx));
        if (kind == "mbutton_down") return post(WM_MBUTTONDOWN, MK_MBUTTON, xy(arg(0), bx));
        if (kind == "mbutton_up")   return post(WM_MBUTTONUP,   0,          xy(arg(0), bx));
        if (kind == "x1_down")      return post(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), xy(arg(0), bx));
        if (kind == "x1_up")        return post(WM_XBUTTONUP,   MAKEWPARAM(0, XBUTTON1),           xy(arg(0), bx));
        if (kind == "x2_down")      return post(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), xy(arg(0), bx));
        if (kind == "x2_up")        return post(WM_XBUTTONUP,   MAKEWPARAM(0, XBUTTON2),           xy(arg(0), bx));
        if (kind == "hwheel")
        {
            POINT p{ bx, cy };
            ClientToScreen(h, &p);
            const WPARAM wp = MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(arg(0))));
            return post(WM_MOUSEHWHEEL, wp, xy(p.x, p.y));
        }
        if (kind == "wheel")
        {
            // WM_MOUSEWHEEL carries SCREEN coordinates.
            POINT p{ bx, cy };
            ClientToScreen(h, &p);
            const WPARAM wp = MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(arg(0))));
            return post(WM_MOUSEWHEEL, wp, xy(p.x, p.y));
        }
        // The desktop mode as it is: a real resolution change cannot be made from a test.
        if (kind == "display_change")
            return post(WM_DISPLAYCHANGE, 32, xy(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
        // A person's Win+Shift+Arrow or a dragged title bar: the window moves, WM_MOVE and WM_SIZE are
        // delivered before this returns.
        if (kind == "move")
        {
            if (!SetWindowPos(h, nullptr, arg(0), bx, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
                return { false, "SetWindowPos failed (" + std::to_string(GetLastError()) + ")" };
            return { true, std::string() };
        }
        // Another window over the whole of this one - what alt-tab to a maximized app does: the swap chain's
        // Present answers DXGI_STATUS_OCCLUDED. A topmost, never-activated popup of the main window's size.
        if (kind == "topmost")   // so only a later topmost window (cover) can be above it
        {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return { true, std::string() };
        }
        if (kind == "cover")
        {
            if (!g_cover)
                // A BUTTON, not a STATIC: a static control answers WM_NCHITTEST with HTTRANSPARENT, so
                // WindowFromPoint (and the mouse) would see straight through it.
                g_cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"BUTTON", L"cover",
                                          WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!g_cover)
                return { false, "CreateWindow failed (" + std::to_string(GetLastError()) + ")" };
            RECT wr{};
            GetWindowRect(h, &wr);
            SetWindowPos(g_cover, HWND_TOPMOST, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return { true, std::string() };
        }
        if (kind == "uncover")
        {
            if (g_cover)
                ShowWindow(g_cover, SW_HIDE);
            return { true, std::string() };
        }
        if (kind == "devices_changed")
        {
            const int n = arg(1);
            for (int i = 0; i < n; ++i)
            {
                const auto r = post(WM_DEVICECHANGE, 0x0007 /*DBT_DEVNODES_CHANGED*/, 0);
                if (!std::get<0>(r)) return r;
            }
            return { true, std::string() };
        }
        if (kind == "power_suspend" || kind == "power_resume")
        {
            LRESULT out = 0;
            return send(WM_POWERBROADCAST, kind == "power_suspend" ? PBT_APMSUSPEND : PBT_APMRESUMEAUTOMATIC, 0, out);
        }
        if (kind == "query_end_session")
        {
            LRESULT out = 0;
            auto r = send(WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF, out);
            if (std::get<0>(r)) std::get<1>(r) = out ? "allow" : "block";
            return r;
        }
        if (kind == "end_session")
        {
            LRESULT out = 0;
            return send(WM_ENDSESSION, arg(1) != 0 ? TRUE : FALSE, ENDSESSION_LOGOFF, out);
        }
        return { false, "unknown kind '" + kind + "'" };
    },
        { .doc = "Post a real window message to the main window (handled at the next message pump): close, activate / deactivate, minimize / restore / maximize, key_down / key_up / syskey_down / syskey_up, char, mouse_move, lbutton_* / rbutton_* / mbutton_* / x1_* / x2_*, wheel, hwheel; display_change, move, devices_changed, power_suspend / power_resume, query_end_session / end_session (sent, answered at once).",
          .params = { { .name = "kind" },
                      { .name = "a", .doc = "the key (VK or Keys name), the UTF-16 code unit, the client x, or the wheel delta (120 = one notch)", .type = "integer|string" },
                      { .name = "b", .doc = "client y (wheel: client x)" },
                      { .name = "c", .doc = "wheel: client y" } },
          .returnsDoc = "true, or false and a reason",
          .effect = LuaDoc::Effect::Process, .context = LuaDoc::Context::Harness });

    // Test.WindowState() -> {exists, minimized, maximized, foreground, appFocused, clientW, clientH,
    // borderless, keyMenuSwallowed, ...}: what the window IS, read from Win32 (not the requested
    // settings), and what it counted (Plat::MainWindowStats).
    LuaDoc::Fn(test, "Test", "WindowState", [](sol::this_state ts) -> sol::table
    {
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        const HWND h = Plat::MainWindowHandle();
        t["exists"] = h != nullptr;
        t["appFocused"]       = Plat::MainWindowAppFocused();
        t["keyMenuSwallowed"] = Plat::MainWindowKeyMenuSwallowed();
        if (!h) return t;
        RECT rc{};
        GetClientRect(h, &rc);
        t["minimized"]  = IsIconic(h) != FALSE;
        t["maximized"]  = IsZoomed(h) != FALSE;
        t["foreground"] = GetForegroundWindow() == h;
        t["clientW"]    = static_cast<int>(rc.right - rc.left);
        t["clientH"]    = static_cast<int>(rc.bottom - rc.top);
        t["borderless"] = (GetWindowLongPtrW(h, GWL_STYLE) & WS_POPUP) != 0;
        t["visible"]    = IsWindowVisible(h) != FALSE;

        RECT wr{};
        GetWindowRect(h, &wr);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const bool haveMonitor = GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) != FALSE;
        sol::table window = lua.create_table_with("left", wr.left, "top", wr.top, "right", wr.right, "bottom", wr.bottom);
        t["windowRect"] = window;
        if (haveMonitor)
        {
            t["monitorRect"] = lua.create_table_with("left", mi.rcMonitor.left, "top", mi.rcMonitor.top,
                                                     "right", mi.rcMonitor.right, "bottom", mi.rcMonitor.bottom);
            t["coversMonitor"] = wr.left == mi.rcMonitor.left && wr.top == mi.rcMonitor.top &&
                                 wr.right == mi.rcMonitor.right && wr.bottom == mi.rcMonitor.bottom;
        }

        const Plat::MainWindowStats s = Plat::MainWindowStatsNow();
        t["shown"]             = s.shown;
        t["longestUnpumpedMs"] = s.longestUnpumpedMs;
        t["pumps"]             = s.pumps;
        t["nestedPumps"]       = s.nestedPumps;
        t["nestedMessages"]    = s.nestedMessages;
        t["eventsDropped"]     = s.eventsDropped;
        t["eventsMerged"]      = s.eventsMerged;
        t["displayRefits"]     = s.displayRefits;
        t["queryEndSessions"]  = s.queryEndSessions;
        t["endSessions"]       = s.endSessions;
        t["shutdownBlocks"]    = s.shutdownBlocks;
        t["powerSuspends"]     = s.powerSuspends;
        t["powerResumes"]      = s.powerResumes;
        t["occluded"]          = s.occluded;
        t["throttling"]        = s.throttling;
        return t;
    },
        { .doc = "What the main window is, read from Win32 (not the requested settings), and what it counted: shown, longestUnpumpedMs, pumps, nestedPumps, eventsDropped / eventsMerged, displayRefits, query / end session and power counts, windowRect, monitorRect, coversMonitor.",
          .returns = "{exists:boolean, minimized:boolean, maximized:boolean, foreground:boolean, appFocused:boolean, clientW:integer, clientH:integer, borderless:boolean, visible:boolean, keyMenuSwallowed:integer, shown:boolean, longestUnpumpedMs:number, pumps:integer, nestedPumps:integer, nestedMessages:integer, eventsDropped:integer, eventsMerged:integer, displayRefits:integer, queryEndSessions:integer, endSessions:integer, shutdownBlocks:integer, powerSuspends:integer, powerResumes:integer, occluded:boolean, throttling:boolean, windowRect:table, monitorRect:table, coversMonitor:boolean}",
          .context = LuaDoc::Context::Harness });

    // Test.KeyEdges(key) -> how many times the WndProc recorded the key going down. A posted Alt+Enter
    // makes DefWindowProc give the (hidden) window the focus, whose WM_SETFOCUS re-reads the physical
    // keyboard and wipes the table - so what Input sees cannot say whether an Enter was recorded;
    // this counter can.
    LuaDoc::Fn(test, "Test", "KeyEdges", [resolveKey](sol::this_state ts, sol::object key) -> int
    {
        return static_cast<int>(Plat::MainWindowKeyDownEdges(resolveKey(ts, key)));
    },
        { .doc = "How many times the main window's WndProc recorded the key (a VK or a Keys name) going down, over the run: not cleared by a focus change, unlike what Input sees.",
          .params = { { .name = "key", .type = "integer|string" } }, .returnsDoc = "the count", .context = LuaDoc::Context::Harness });

    // Test.InputFocus(true | false | nil): the input focus gate's answer,
    // forced. A -hidden window is never the foreground one, so without this
    // the key messages Test.Window posts reach the window's KeyboardState
    // and stop at Input's gate. nil restores the foreground test.
    LuaDoc::Fn(test, "Test", "InputFocus", [](sol::optional<bool> focused)
    {
        Input::Get().SetFocusOverride(focused ? std::optional<bool>(*focused) : std::nullopt);
    },
        { .doc = "Force Input's focus gate (true / false), or restore the foreground test (nil), so posted key messages reach Input under -hidden.",
          .params = { { .name = "focused" } }, .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Harness });

    // Test.TypedText() -> the UTF-8 the window collected from WM_CHAR since
    // the last call (surrogate pairs joined, control characters dropped).
    LuaDoc::Fn(test, "Test", "TypedText", []() -> std::string { return Plat::MainWindowConsumeTypedText(); },
        { .doc = "The text typed into the main window since the last call (UTF-8, from WM_CHAR).",
          .returnsDoc = "UTF-8 text", .context = LuaDoc::Context::Harness });
}

} // namespace Scripting

#endif // WITH_HARNESS
