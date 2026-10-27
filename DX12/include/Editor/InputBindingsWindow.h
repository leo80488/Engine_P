#pragma once

// InputBindingsWindow - point-and-click rebinding + tuning + live diagnostics.
//
// A standalone class, not an EditorLayer member function: the panel owns real
// cross-frame state (a pending capture, an in-progress binding table, a status
// line) and, unlike other editor windows, must be TICKED AT A SPECIFIC POINT IN THE
// FRAME (see capture ordering below). Self-contained, it costs the host two calls.
//
// CAPTURE ORDERING - THE ONE TRAP.
//   While a capture is pending, Input::BuildCommand zeroes ALL action output so the
//   capturing press cannot also fire gameplay. It does NOT (and cannot) suppress
//   the EDITOR's own ImGui hotkeys: if the host's undo-shortcut handler runs before
//   this panel consumes the capture, rebinding an action to Z while Ctrl is down
//   performs an editor undo on the very frame you bind it. TickCapture() therefore
//   MUST run BEFORE EditorLayer::HandleUndoShortcuts(). Render() calls
//   TickCapture() itself (idempotent per ImGui frame), so a host that only calls
//   Render() still works - it just has to call it early.
//
//   Ordering ALONE is not enough: on the frame the capture resolves,
//   ImGui::IsKeyPressed(Z) is STILL true when the undo handler runs afterwards, so
//   Ctrl+Z would both bind and undo. The host must also early-out its shortcut
//   handler on WantsExclusiveKeyboard().
//
// Closing the window CANCELS a pending capture: a capture left pending keeps every
// gameplay action zeroed for the rest of the session, which reads as "the game
// stopped responding to input".
//
// Closing the window also QUEUES A ZERO RUMBLE. XInput vibration LATCHES in the
// device until somebody writes zero to it; nothing in the engine remembers that the
// editor started a test buzz, so closing the panel mid-buzz would leave the pad
// humming with no UI left able to stop it.
//
// MOUSE BUTTONS ARE CAPTURABLE. Input reads them from a raw pre-quarantine
// snapshot, so binding "mouse2" is a click into the viewport, not a settings.json
// edit - and the click that OPENS a cell can never bind itself, because every
// control already down when BeginCapture ran is ignored until it has been observed
// released. Cells are therefore labelled "press any key or button".
//
// THE TABLE THIS PANEL EDITS IS THE **BASE** BINDING TABLE. Input::PushContext can
// layer a higher-priority InputContext over it at runtime, and a context with
// blockLower can make an action UNBOUND without touching a row here. The panel
// therefore banners the active context stack above the table: a user chasing "why
// does B not dodge" must not read this table as authoritative.
//
// Design: docs/design/gamepad_input_architecture.md.
// Editor-only TU: compiled into EditorCore (Editor.exe), NEVER into EngineCore
// (which must not see ImGui).

#include "Input/InputTypes.h"

#include <cstdint>
#include <functional>
// _Printf_format_string_ makes MSVC check SetStatus() call sites: without it,
// SetStatus(cond ? "a" : "b") sails through as a runtime-valued format string.
#include <sal.h>

namespace Editor
{

class InputBindingsWindow
{
public:
    // One editor, one panel: a function-local static, exposed because the host
    // reaches it from both the menu bar and the frame tick.
    static InputBindingsWindow& Get() noexcept;

    // ---- Menu / visibility plumbing ----
    bool  IsOpen()   const noexcept { return m_open; }
    // Bind straight into ImGui::MenuItem("Input Bindings", nullptr, OpenFlag()).
    bool* OpenFlag()       noexcept { return &m_open; }
    void  Open()           noexcept { m_open = true; }
    void  Close()          noexcept;      // cancels a pending capture AND stops rumble
    void  Toggle()         noexcept;

    // ---- Frame hooks ----
    // Consume any control the engine captured for us, and service the Escape /
    // timeout cancel paths. Cheap, opens no ImGui window. MUST be called before the
    // host's undo-shortcut handling (see the header note). Safe to call more than once
    // per frame: the body runs at most once per ImGui frame.
    void TickCapture();

    // Draw the window. No-op (beyond cancelling a stale capture) when closed.
    void Render();

    // True while the panel owns the next key press / pad button.
    bool CapturePending() const noexcept { return m_capture.slot != CaptureSlot::None; }

    // CapturePending() OR "a capture was resolved/cancelled on THIS ImGui
    // frame". The second half is the part ordering cannot fix: the key edge
    // that ended the capture is still visible to ImGui::IsKeyPressed for the
    // rest of the frame. Hosts must early-out their global keyboard shortcuts
    // (undo/redo/duplicate/copy/paste) on this.
    bool WantsExclusiveKeyboard() const noexcept;

private:
    enum class CaptureSlot : std::uint8_t
    {
        None = 0,
        Key0,       // ActionBinding::vk[0]
        Key1,       // ActionBinding::vk[1]
        Pad,        // ActionBinding::pad

        // ---- Movement composite (InputBindings::move) ----
        // KEYBOARD ONLY. The stick is picked ONCE by the "Move stick" combo, not per
        // direction, so a pad capture landing here is REJECTED with a message instead of
        // silently dropped, which would read as "the panel is broken".
        // ORDER IS LOAD-BEARING: CompositeIndex() subtracts MoveFwd.
        MoveFwd,    // move.vkPosY
        MoveBack,   // move.vkNegY
        MoveLeft,   // move.vkNegX
        MoveRight,  // move.vkPosX
    };

    struct CaptureTarget
    {
        CaptureSlot slot      = CaptureSlot::None;
        // Index into InputBindings::action[], or -1 for a composite slot (the
        // slot alone identifies the target there).
        int         action    = -1;
        double      startTime = 0.0;     // ImGui::GetTime() when BeginCapture ran
    };

    InputBindingsWindow() = default;

    // ---- sub-draws ----
    void DrawBindingTable();
    void DrawMovementTable();     // the move composite + the two stick pickers
    void DrawTuning();
    void DrawButtons();
    void DrawDiagnostics();
    void DrawContexts();          // the ACTIVE mapping-context stack
    // Banner above the binding table naming the active contexts. Without it the
    // table looks authoritative while a pushed context is quietly overriding it.
    void DrawContextBanner();

    // Draws one clickable cell and starts/cancels a capture on click.
    // `conflict` tints the cell red. `actionIndex` is -1 for composite slots.
    void DrawBindingCell(int actionIndex, CaptureSlot slot, bool conflict);

    // ---- movement composite slot helpers ----
    static bool        IsCompositeSlot(CaptureSlot s) noexcept;
    static int         CompositeIndex(CaptureSlot s) noexcept;   // 0..3, -1 otherwise
    static const char* CompositeSlotName(CaptureSlot s) noexcept;
    // The m_edit.move field a composite slot addresses. nullptr for non-composite.
    std::uint16_t*     CompositeVkPtr(CaptureSlot s) noexcept;

    // ---- capture helpers ----
    void BeginCaptureFor(int actionIndex, CaptureSlot slot);
    void AbortCapture(const char* why);
    void WriteCapturedBinding(bool isPad, std::uint16_t vk, std::uint16_t pad);

    // Expire a running test buzz. Lives on the per-frame tick rather than in
    // DrawDiagnostics because the user can collapse that header (or the whole
    // window) mid-buzz, and the pad would then never be told to stop.
    void TickRumbleTest();

    // ---- bindings helpers ----
    void SeedFromEngine();                 // pull the live table into m_edit
    void PushToEngine();                   // SetBindings + re-pull the sanitized result
    void SetStatus(_Printf_format_string_ const char* fmt, ...);

public:
    // Host-supplied persistence (EngineSettings lives above this layer, so this
    // file must not include it). Return true on success. Null by default, which
    // greys the Save button out rather than silently doing nothing.
    std::function<bool()> saveCallback;

private:
    bool          m_open    = false;
    bool          m_wasOpen = false;       // for the closed->open re-seed edge
    bool          m_seeded  = false;       // m_edit valid? (lazy first-open pull)
    InputBindings m_edit {};               // working copy; live-applied on change
    CaptureTarget m_capture {};

    // TickCapture() is idempotent per ImGui frame so the host can call it early
    // AND let Render() call it again without double-consuming a capture.
    int m_lastTickFrame = -1;

    // ImGui frame on which a capture last resolved or cancelled. Drives
    // WantsExclusiveKeyboard() for the remainder of that frame.
    int m_consumedKeyFrame = -1;

    // Transient status line under the buttons (auto-fades).
    char   m_status[192] = {};
    double m_statusTime  = 0.0;

    // Rumble test sliders: the only way to verify the pad's output path from the
    // editor, placed next to the readout.
    float m_rumbleLo = 0.35f;
    float m_rumbleHi = 0.35f;
    // ImGui::GetTime() at which a test buzz must be silenced, 0 = not buzzing.
    // The buzz auto-stops: XInput vibration latches in the DEVICE, so relying on
    // the user pressing Stop means one distraction leaves the pad running.
    double m_rumbleStopTime = 0.0;
};

} // namespace Editor
