#pragma once

// Input - the engine's single per-frame input snapshot point.
//
// Update() runs exactly once per frame from the App tick, before any system reads input:
// it rolls prev -> curr for the 256-entry keyboard table, polls the gamepad and resolves
// both devices through the bindings into one immutable ActionState (the usercmd).
// Everything downstream (the player brain, the camera, Lua) reads that struct and cannot
// tell which device produced a value.
//
//   Raw keyboard queries  (IsKeyDown / WasKeyPressed / Axis)  device-aware
//   Action queries        (Cmd())                             device-agnostic
//
// New gameplay code reads Cmd(); the raw queries serve editor shortcuts, the free-cam and
// host hotkeys (F11), where rebinding buys nothing.
//
// Threading: Update() is main-thread only. Every query is const and safe from any
// thread, reading the snapshot of the latest Update().
//
// Keyboard: the window's KeyboardState (its WndProc fills it from WM_KEY* /
// WM_*BUTTON*), handed in through UpdateInputs; without a window, a GetAsyncKeyState
// poll into the same table layout.
// Mouse: the window's PointerState; App / EditorLayer turn it into a pixel delta handed
// in here, so mouse displacement and stick rate share one usercmd.
//
// Design: docs/design/gamepad_input_architecture.md

#include "Input/InputTypes.h"
#include "Input/GamepadDevice.h"

#include <cstdint>
#include <cstring>
#include <optional>

namespace Plat { class KeyboardState; }

class Input
{
public:
    // ---- Per-frame snapshot ----
    struct UpdateInputs
    {
        // UI capture: while true, mouse button states and edges are hidden from gameplay,
        // and a hold captured this way stays hidden until it is physically released.
        bool suppressMouseButtons = false;

        // ImGui/Canvas owns the pad this frame: the pad's contribution to
        // `held` is forced to 0, but release edges STILL fire so nothing
        // latches down.
        bool suppressGamepad = false;

        // PlayModeController::SimAdvancing(). When false, gameplay actions and
        // move axes are zeroed BEFORE edges are derived, so `pressed` can never
        // be true while `held` is false on the pause frame. Host actions
        // (MenuToggle) are exempt.
        bool simAdvancing = true;

        // This frame's viewport mouse delta in PIXELS, and whether the look
        // gesture is engaged (RMB over the viewport).
        float mouseLookDX   = 0.f;
        float mouseLookDY   = 0.f;
        bool  mouseLookHeld = false;

        // The window's keyboard (Plat::Window::Keyboard()), sampled once per
        // focused frame. Null: poll GetAsyncKeyState instead (no window).
        Plat::KeyboardState* keyboard = nullptr;

        // The host withholds input from the game this frame (the editor's Game View has neither focus nor the
        // capture): every key, button and the look read up, as when another application has the foreground -
        // synthetic input included - and held controls fire their release once.
        bool hostFocus = true;

        // A key the host took for itself (the Esc that released the Game View's capture): hidden from the
        // game from this frame until it is physically released, so its press never reaches an action.
        // 0 = none.
        int quarantineKey = 0;
    };

    // ---- Mapping contexts: priority-stacked binding layers (see InputContext) ----
    // Define once (at boot / from Lua config), then push and pop at runtime. Pushing an
    // already-active context re-seats it at the top of its priority band rather than
    // duplicating it.
    static constexpr int kMaxContexts = 8;

    // ---- Singleton ----
    static Input& Get() noexcept;

    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    // ---- Per-frame snapshot ----
    // Roll prev -> curr, sample the keyboard, poll the pad and rebuild the ActionState. Call
    // once per frame at the start of the App tick, before any system reads input, or every
    // edge query is unreliable.
    void Update(const UpdateInputs& in);

    // Convenience overload: no mouse delta, no pad suppression, sim advancing.
    void Update(bool suppressMouseButtons = false)
    {
        UpdateInputs u;
        u.suppressMouseButtons = suppressMouseButtons;
        Update(u);
    }

    // The inputs the last Update() resolved: the caller's, plus - in a harness
    // build - the synthetic mouse look (SynthLook, -input_replay). App's camera
    // gates (fly on RMB, the look delta) read these after Update(), so they
    // agree with Cmd().
    const UpdateInputs& LastInputs() const noexcept { return m_lastIn; }

    // Clear all keyboard, pad and action state. Use when the window loses focus
    // for good, or on teardown, to avoid "stuck" inputs.
    void Reset() noexcept;
    void NeutralizeGameplay() noexcept;

    // ---- Raw keyboard queries (device-aware; prefer Cmd() for gameplay) ----
    // Hold queries. @p vk is a Windows virtual-key code (VK_*), or an ASCII
    // upper-case letter / digit for keyboard keys (e.g. 'W', '0').
    bool IsKeyDown(int vk) const noexcept
    {
        return (vk >= 0 && vk < kNumKeys) && m_curr[vk] != 0;
    }

    // Edge queries (false except on the single frame of the transition).
    // Reliable only when Update() ran exactly once per frame.
    bool WasKeyPressed(int vk) const noexcept
    {
        return (vk >= 0 && vk < kNumKeys) && m_curr[vk] != 0 && m_prev[vk] == 0;
    }
    bool WasKeyReleased(int vk) const noexcept
    {
        return (vk >= 0 && vk < kNumKeys) && m_curr[vk] == 0 && m_prev[vk] != 0;
    }

    // 1D axis helper. Result is (posVk ? 1 : 0) - (negVk ? 1 : 0). Returns
    // -1, 0, or +1. Convenience for movement code: Axis('D','A') reads strafe.
    float Axis(int posVk, int negVk) const noexcept
    {
        return (IsKeyDown(posVk) ? 1.f : 0.f) - (IsKeyDown(negVk) ? 1.f : 0.f);
    }

    // ---- Device-agnostic read surface: const, lock-free, immutable for the frame ----
    const ActionState&   Cmd()        const noexcept { return m_cmd; }
    const GamepadDevice& Pad()        const noexcept { return m_pad; }
    InputDeviceKind      LastDevice() const noexcept { return m_cmd.lastDevice; }

    // "pad/a" / "kbm/space": the first bound control for @p a on the active device, for
    // prompt glyphs; nullptr when unbound. The pointer is valid until the next call on this
    // thread.
    const char* GlyphToken(GameAction a) const noexcept;

    // ---- Bindings ----
    const InputBindings& Bindings() const noexcept { return m_bindings; }
    void SetBindings(const InputBindings& b) noexcept;   // Sanitize()s on entry

    void QueueRumble(int slot, float lo, float hi) noexcept
    { m_pad.QueueRumble(slot, lo, hi); }

    // Installed as Window's WM_DEVICECHANGE hook by App.
    void OnDeviceChange() noexcept { m_pad.NotifyDeviceChange(); }

    // ---- Mapping contexts: priority-stacked binding layers (see InputContext) ----
    // Registers or replaces a context by name; false when the table is full. Redefining a
    // pushed context takes effect immediately, so a Lua hot-reload retunes live.
    bool DefineContext(const InputContext& c) noexcept;

    // Activate / deactivate by name. Push returns false for an unknown name or
    // a full stack; Pop returns false when it was not active. Both are
    // idempotent-safe to call from gameplay code.
    bool PushContext(const char* name) noexcept;
    bool PopContext(const char* name) noexcept;
    void ClearContexts() noexcept;               // deactivate all (keeps definitions)
    bool IsContextActive(const char* name) const noexcept;

    // Active stack, HIGHEST priority first. For the editor readout and for a
    // Lua "what owns the pad right now" query.
    int         ActiveContextCount() const noexcept { return m_activeCount; }
    const char* ActiveContextName(int i) const noexcept;
    int         ActiveContextPriority(int i) const noexcept;

    // Defined (registered) contexts, active or not, so the editor can offer a push / pop
    // test button per context.
    int         DefinedContextCount() const noexcept;
    const char* DefinedContextName(int i) const noexcept;

    // What drives @p a right now, after the context walk, so a rebind UI can show the live
    // table. It shares the resolution rule (blockLower included) with BuildCommand; do not
    // re-implement it in a panel: a second copy drifts and the tool starts giving wrong
    // answers.
    // @p outOwner (optional) receives the deciding context's name, nullptr when the base
    // table won. @p outBlocked (optional) is set when a blockLower context swallowed the
    // action; the returned binding is then empty.
    const ActionBinding& ResolvedBinding(GameAction a,
                                         const char** outOwner = nullptr,
                                         bool* outBlocked = nullptr) const noexcept;
    const Axis2DBinding& ResolvedMove(const char** outOwner = nullptr) const noexcept;
    const Axis2DBinding& ResolvedLook(const char** outOwner = nullptr) const noexcept;

    // ---- Rebind capture (editor / options screen) ----
    // While a capture is pending BuildCommand zeroes all action output, so the capturing
    // press cannot also fire gameplay. The capture takes the first key, mouse button or pad
    // button edge. Mouse buttons come from a raw pre-quarantine snapshot, so whether a click
    // can be bound does not depend on ImGui's WantCaptureMouse. Every control already down
    // at BeginCapture is ignored until observed released, so the click that opens a capture
    // cannot bind itself.
    void BeginCapture() noexcept;
    void CancelCapture() noexcept { m_capturePending = false; }
    bool CapturePending() const noexcept { return m_capturePending; }
    // True once a control was captured; fills the outputs and ends the capture.
    bool TakeCapture(bool& outIsPad, std::uint16_t& outVk, std::uint16_t& outPad) noexcept;

#ifdef WITH_HARNESS

    // ---- Synthetic device (harness builds only: input replay) ----
    // Keys it holds are OR-ed into the sampled keyboard table before the bindings resolve,
    // so an injected press takes the path a physical one does (bindings, mapping contexts,
    // the pause gate, edges, device provenance) into ActionState: a run driven by it is a
    // game-action test. It creates no OS input event and ignores focus (a hidden run has
    // none), so it is not an OS / GUI input test.
    //   -input_record=<file.jsonl>  the cooked keyboard table (physical and synthetic keys)
    //                               whenever it changes, by Update() index:
    //                               {"u": N, "keys": [vk...]}, plus the active pad ("pad")
    //                               and the mouse look ("look") while they are non-neutral
    //   -input_replay=<file.jsonl>  such a file fed back through this device, update for
    //                               update; with -fixeddt the run repeats. The last line's
    //                               input stays on.
    static constexpr int kHoldUntilReleased = -1;

    struct SynthStatus
    {
        int           held      = 0;       // keys the device holds (SynthHold) now
        bool          pad       = false;   // the synthetic pad is set (SynthPad / a replay)
        bool          look      = false;   // the synthetic look is set (SynthLook / a replay)
        std::uint64_t updates   = 0;       // Update() calls so far
        bool          recording = false;
        bool          replaying = false;   // a replay with changes still ahead
        std::uint64_t replayEnd = 0;       // the update index of its last change
    };

    // Down for the next @p frames Update()s (1 = a tap), kHoldUntilReleased
    // until SynthHold(vk, 0). False for a vk outside 1..255. Main thread.
    bool SynthHold(int vk, int frames) noexcept;
    void SynthReleaseAll() noexcept;
    // The synthetic PAD (GamepadDevice::SetSynthetic - a virtual fifth slot
    // that outranks a physical pad and needs no focus): @p s's COOKED values -
    // buttons (PadBtn::Mask), sticks [-1,1] with +y up, triggers [0,1] - for the
    // next @p frames Update()s (kHoldUntilReleased; 0 lets go: one neutral
    // frame of release edges, then the slot leaves). Recorded and replayed
    // with the keys ("pad": {b, lx, ly, rx, ry, lt, rt}).
    void SynthPad(const GamepadState& s, int frames) noexcept;
    // The synthetic MOUSE LOOK: @p dx, @p dy PIXELS (screen +y down) added to
    // the mouse's own displacement at each of the next @p frames Update()s
    // (kHoldUntilReleased; 0 stops), with the look gesture held - RMB over the
    // viewport - unless @p held is false. It joins UpdateInputs before
    // anything resolves, so it reaches Cmd().lookMouseDX/DY (x mouseLookScale),
    // lookActive and the device latch, and through LastInputs() App's camera
    // gates, exactly as a drag does. Recorded and replayed with the keys
    // ("look": {dx, dy, held}).
    void SynthLook(float dx, float dy, int frames, bool held = true) noexcept;

    SynthStatus Synth() const noexcept;

    // The focus gate's answer, forced (Lua Test.InputFocus): true makes a
    // -hidden run sample the window's keyboard, which the window probe
    // (Test.Window key_down / key_up) fills with real key messages - the only
    // way a pack can drive WndProc -> KeyboardState -> Input end to end.
    // nullopt: the foreground test again. Unlike Synth*, this injects nothing.
    void SetFocusOverride(std::optional<bool> focused) noexcept { m_focusOverride = focused; }
#endif

private:
    Input() = default;

    UpdateInputs  m_lastIn{};              // what the last Update() resolved (LastInputs)

#ifdef WITH_HARNESS

    struct SynthIO;                        // the record / replay files (InputSystem.cpp)

    struct SynthLookState { float dx = 0.f, dy = 0.f; bool held = false; };

    void SynthApply(UpdateInputs& in) noexcept;         // Update() step 4b: keys, pad and look into @p in
    void SynthRecord(const UpdateInputs& in) noexcept;  // Update() step 5b: after the pad poll

    SynthIO*      m_synthIO   = nullptr;   // opened at the first Update(); process lifetime
    bool          m_synthInit = false;
    int           m_synthFrames[256] = {}; // per vk: Update()s left down, -1 = until released
    std::uint8_t  m_replayKeys[256]  = {}; // the replay's current key set
    GamepadState  m_synthPadState{};       // SynthPad's pad
    int           m_synthPadFrames = 0;    // Update()s left, -1 = until released
    GamepadState  m_replayPad{};           // the replay's current pad
    bool          m_replayPadOn = false;

    SynthLookState m_synthLook{};          // SynthLook's look
    int           m_synthLookFrames = 0;   // Update()s left, -1 = until released
    SynthLookState m_replayLook{};         // the replay's current look
    bool          m_replayLookOn = false;
    std::uint64_t m_updates = 0;
    std::uint64_t m_recordU = 0;           // the update SynthApply stamped (SynthRecord writes it)
#endif

    static constexpr int kNumKeys = 256;

    static constexpr int kNumMouseVks = 5;   // L, R, M, X1, X2

    void BuildCommand(const UpdateInputs& in, bool focused) noexcept;
    void TickCapture() noexcept;
    int  FindContext(const char* name) const noexcept;   // index into m_contexts, -1 = none
    void SortActiveContexts() noexcept;                  // priority DESC, stable

    // Resolve one action through the active context stack. Returns the winning
    // binding; `outBlocked` is set when a blockLower context swallowed it.
    const ActionBinding& ResolveAction(GameAction a, bool& outBlocked) const noexcept;
    const Axis2DBinding& ResolveMove(bool& outBlocked) const noexcept;
    const Axis2DBinding& ResolveLook(bool& outBlocked) const noexcept;

    std::uint8_t m_curr[kNumKeys] = {};
    std::uint8_t m_prev[kNumKeys] = {};
    // L, R, M, X1, X2 (kMouseVkList's order); all five are quarantined while the UI owns
    // the mouse.
    bool         m_mouseButtonSuppressed[5] = {};
    // Keys the host quarantined (UpdateInputs::quarantineKey), until each is physically released.
    std::uint8_t m_keyQuarantined[kNumKeys] = {};
    // Harness only (Test.InputFocus): answers the focus gate instead of the
    // foreground test, so a pack can drive the window's real key messages
    // under -hidden, whose window is never the foreground one.
    std::optional<bool> m_focusOverride;

    // Pre-quarantine mouse-button snapshot. The cooked m_curr hides mouse buttons whenever
    // the UI owns the pointer, which is exactly when a rebind panel is up, so the capture
    // path needs an unfiltered view. Gameplay must never read these.
    std::uint8_t m_rawMouseCurr[kNumMouseVks] = {};
    std::uint8_t m_rawMousePrev[kNumMouseVks] = {};

    GamepadDevice m_pad {};
    InputBindings m_bindings = InputBindings::Defaults();
    ActionState   m_cmd {};
    std::uint32_t m_heldPrev = 0;

    // ---- Mapping contexts ----
    InputContext m_contexts[kMaxContexts] {};
    int          m_contextCount = 0;                 // defined
    int          m_active[kMaxContexts] = {};        // indices, HIGHEST priority first
    int          m_activeCount = 0;                  // pushed

    // ---- Rebind capture ----
    bool          m_capturePending = false;
    bool          m_captureIsPad   = false;
    std::uint16_t m_captureVk      = 0;
    std::uint16_t m_capturePad     = 0;
    bool          m_captureReady   = false;
    // Controls that were already down when the capture opened. They cannot be
    // captured until observed released, so the click that opens a cell (or the
    // Enter that activated it via ImGui keyboard nav) cannot bind itself.
    std::uint8_t  m_captureIgnoreKey[kNumKeys] = {};
    std::uint16_t m_captureIgnorePad = 0;
};
