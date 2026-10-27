#pragma once

// InputTypes - the device-agnostic input vocabulary, the boundary of the input system.
// Above it (GamepadDevice, Input::BuildCommand, InputBindings) code is device-aware and
// may name XInput, VK codes and stick geometry; below it (the player brain, the camera,
// Lua) code reads ActionState and cannot tell which device produced a value. Swapping
// keyboard <-> gamepad changes only who fills the struct.
// Dependency-free on purpose (no <windows.h>, <xinput.h>, ECS or ImGui): pure PODs that
// link into every exe without pulling anything in.
// Design: docs/design/gamepad_input_architecture.md

#include <cstdint>

// Device identity, cosmetic only: it picks prompt glyphs. No gameplay code may branch on
// it.
enum class InputDeviceKind : std::uint8_t
{
    None          = 0,
    KeyboardMouse = 1,
    Gamepad       = 2,
};

const char* ToString(InputDeviceKind d) noexcept;

// ==== Digital actions ====
// Fixed and engine-owned, not an asset. Adding one = an enumerator (before Count) + a
// Defaults() row + a kActionNames entry (InputBindings.cpp); no cook step, no schema
// migration.
// Crouch / Interact ship unbound in InputBindings::Defaults(): nothing reads
// CharacterControllerComponent::wantsCrouch and no engine system consumes
// GameAction::Interact, so a binding would be working input with no visible result, the
// hardest kind of bug to chase. Bind them when the consumer exists.
// A game's own verbs (RossiDuel's Guard, LockTarget, Controls, ...) are not enumerators: they are
// actions of an input profile (InputProfile.Define, asset/RossiDuel/InputProfile.lua).
// UIConfirm / UICancel drive UI submit / cancel (UIInputSampler). Their defaults
// deliberately overlap the gameplay set (A / Escape); the rebind UI treats UI actions as
// a separate namespace, so the overlap is not reported as a conflict.
enum class GameAction : std::uint8_t
{
    Jump = 0,
    Sprint,
    Crouch,        // unbound by default: no consumer yet
    Attack,
    Dodge,         // Ctrl / B; RossiDuel consumes the action in Lua
    Interact,      // unbound by default: no consumer yet
    MenuToggle,    // host-level (play/pause); EXEMPT from the simAdvancing gate
    UIConfirm,     // UI submit (UIInputSampler)
    UICancel,
    Restart,       // restart the level / run; a game binds what it does
    Count
};

inline constexpr int kGameActionCount = static_cast<int>(GameAction::Count);
static_assert(kGameActionCount <= 32,
              "ActionState held/pressed/released are uint32 bitsets");

const char* ToString(GameAction a) noexcept;                       // "Jump", ...
bool        ParseGameAction(const char* name, GameAction& out) noexcept;

// True for actions that must keep working while the sim is paused.
inline bool IsHostAction(GameAction a) noexcept
{
    return a == GameAction::MenuToggle;
}

// ==== Pad buttons ====
// The engine's own button vocabulary. XINPUT_GAMEPAD_* never escapes GamepadDevice.cpp,
// so a GameInput / DualSense / Raw-HID backend maps onto these same bits and saved
// bindings keep working.
namespace PadBtn
{
    enum Mask : std::uint16_t
    {
        None   = 0,
        A      = 1u << 0,
        B      = 1u << 1,
        X      = 1u << 2,
        Y      = 1u << 3,
        LB     = 1u << 4,
        RB     = 1u << 5,
        Back   = 1u << 6,
        Start  = 1u << 7,
        LS     = 1u << 8,   // left stick click
        RS     = 1u << 9,   // right stick click
        DUp    = 1u << 10,
        DDown  = 1u << 11,
        DLeft  = 1u << 12,
        DRight = 1u << 13,
        LT     = 1u << 14,  // synthesized from the analog trigger
        RT     = 1u << 15,  // synthesized from the analog trigger
    };
}

// Canonical lowercase name for one pad bit ("a", "leftShoulder", ...).
// Returns nullptr when `mask` is not exactly one known bit.
const char*   PadButtonName(std::uint16_t mask) noexcept;
// Inverse. Returns PadBtn::None when unrecognised.
std::uint16_t ParsePadButton(const char* name) noexcept;

// ==== Gamepad state ====
// One pad's cooked state for one frame: deadzone, response curve and trigger threshold
// are already applied; no consumer re-derives them.
struct GamepadState
{
    bool          connected   = false;
    std::uint16_t buttons     = 0;   // PadBtn::Mask, this frame
    std::uint16_t prevButtons = 0;   // last frame: the edge source
    float         lx = 0.f, ly = 0.f;  // [-1,1] post-deadzone, +y = stick UP
    float         rx = 0.f, ry = 0.f;
    float         lt = 0.f, rt = 0.f;  // [0,1] post-threshold
    std::uint32_t packet = 0;          // XInput dwPacketNumber (change detect / debug)

    // Pre-deadzone, pre-curve axes in [-1,1], for diagnostics only; gameplay reads the cooked
    // lx/ly/rx/ry. The editor plots these so its deadzone ring at `stickDeadzone` is the
    // boundary the dot crosses (a cooked dot leaves the ring instantly).
    float         rawLx = 0.f, rawLy = 0.f;
    float         rawRx = 0.f, rawRy = 0.f;

    bool Down(std::uint16_t mask) const noexcept { return (buttons & mask) != 0; }
    bool Pressed(std::uint16_t mask) const noexcept
    { return (buttons & mask) != 0 && (prevButtons & mask) == 0; }
};

// ==== Bindings ====
// Two keyboard slots + one pad mask per action. No chords, no modifiers: ActionBinding
// stays POD and settings.json stays trivial.
struct ActionBinding
{
    std::uint16_t vk[2] = { 0, 0 };  // VK_* or ASCII 'W'. 0 = unbound.
    std::uint16_t pad   = 0;         // OR of PadBtn::Mask: any set bit fires.

    bool Bound() const noexcept { return vk[0] || vk[1] || pad; }
};

// Unity-style "2D vector composite", hard-coded to the only two axes we need.
struct Axis2DBinding
{
    std::uint16_t vkPosX = 0, vkNegX = 0;   // D, A
    std::uint16_t vkPosY = 0, vkNegY = 0;   // W, S
    std::uint8_t  stick  = 0;               // 0 = left stick, 1 = right stick
};

struct InputBindings
{
    ActionBinding action[kGameActionCount] = {};
    Axis2DBinding move {};
    Axis2DBinding look {};

    // ---- Tuning (player-authored, persisted under settings.json "input") ----
    float lookRateDegPerSec = 180.f;  // pad look at full deflection  [10 .. 720]
    float mouseLookScale    = 1.0f;   // x CameraControllerComponent::mouseSensitivity (per entity)  [0.05 .. 20]
    float stickDeadzone     = 0.24f;  // radial, both sticks           [0 .. 0.9]
    float stickCurveExp     = 2.0f;   // exponent on MAGNITUDE only    [1 .. 4]
    float triggerThreshold  = 0.12f;  // [0 .. 0.9]
    // No analog speed-scaling knob on purpose: stick deflection supplies direction only; any
    // input past stickDeadzone moves at the gait's full speed, and run is an explicit action
    // (Sprint).
    bool  padInvertLookY    = false;
    bool  mouseInvertLookY  = false;
    bool  gamepadEnabled    = true;

    static InputBindings Defaults() noexcept;
    // Clamps every scalar into range. Never unbinds anything: a corrupt settings.json must
    // not lock the player out of moving.
    void Sanitize() noexcept;
};

// "pad/a" | "kbm/space" | "kbm/w". Parses into (isPad, vk, padMask).
// Returns false and leaves the outputs untouched on an unknown path.
bool ParseBindingPath(const char* path, bool& outIsPad,
                      std::uint16_t& outVk, std::uint16_t& outPad) noexcept;
// Inverse. Writes at most `cap` bytes including the terminator; returns false
// if unbound or the buffer is too small.
bool FormatBindingPath(bool isPad, std::uint16_t vkOrPad,
                       char* out, int cap) noexcept;

// Canonical lowercase name for a virtual key ("space", "w", "lshift", ...).
const char*   VirtualKeyName(std::uint16_t vk) noexcept;
std::uint16_t ParseVirtualKey(const char* name) noexcept;

// ==== Mapping contexts ====
// InputContext - a priority-stacked layer of bindings, the "B cancels in a menu but
// dodges in combat" mechanism: UE5's InputMappingContext minus the modifier / trigger
// pipeline. A context binds some actions; contexts are pushed and popped at runtime.
// Resolution per action (Input::ResolveAction, used by BuildCommand):
//   walk the active contexts, highest priority first
//     - the context defines this action  -> use its binding, stop
//     - else if it blocks lower           -> the action is unbound, stop
//     - else                              -> keep walking
//   nothing resolved                      -> the base InputBindings
// blockLower lets a modal menu silence gameplay without listing every action. Contexts
// change which control fires an action, never the vocabulary, so nothing below
// ActionState can tell a context is active.
struct InputContext
{
    static constexpr int kNameCap = 32;

    char name[kNameCap] = {};   // identity for Push/Pop. Case-insensitive.
    int  priority       = 0;    // higher wins. Ties break by push order (later wins).

    // Actions this context does not define resolve to unbound instead of falling through to
    // lower contexts / the base table: the "menu owns the pad" switch. Host actions
    // (IsHostAction: MenuToggle) are exempt, or a blocking context that forgets MenuToggle
    // would swallow the button that closes it. A context may still redefine a host action
    // explicitly; it cannot silence one by omission.
    bool blockLower = false;

    // Bit i set = this context defines action i. A context that defines an
    // action with an EMPTY binding is meaningful: it explicitly unbinds it.
    std::uint32_t definedMask = 0;
    ActionBinding action[kGameActionCount] = {};

    // Composites are all-or-nothing per axis pair.
    bool          definesMove = false;
    bool          definesLook = false;
    Axis2DBinding move {};
    Axis2DBinding look {};

    bool Defines(GameAction a) const noexcept
    { return (definedMask >> static_cast<int>(a)) & 1u; }

    void Define(GameAction a, const ActionBinding& b) noexcept
    {
        const int i = static_cast<int>(a);
        if (i < 0 || i >= kGameActionCount) return;
        action[i] = b;
        definedMask |= (1u << i);
    }
};

// ==== ActionState: the usercmd ====
// Built once per frame in Input::Update() on the main thread, then const for the rest of
// the frame. Trivially copyable, no heap, no pointers: safe to read from ECS workers with
// no synchronisation. Every consumer reads the same latched pressed / released bits, so
// nothing can double-consume or miss an edge.
struct ActionState
{
    // ---- Analog movement (post-deadzone, post-curve, clamped) ----
    float moveX   = 0.f;   // [-1,1] strafe  (+ = right)
    float moveY   = 0.f;   // [-1,1] forward (+ = forward)
    float moveMag = 0.f;   // [0,1] = min(1, len(moveX,moveY))

    // ---- Look: two carriers, separated by physical unit ----
    // A stick value cannot reach a pixel consumer, so look cannot become
    // framerate-dependent.
    float lookMouseDX = 0.f;  // PIXELS this frame (x mouseLookScale). DISPLACEMENT.
    float lookMouseDY = 0.f;  //   Consumer multiplies by mouseSensitivity. NO dt.
    float lookStickX  = 0.f;  // [-1,1] post-deadzone right stick. A RATE.
    float lookStickY  = 0.f;  //   Consumer multiplies by lookRateDegPerSec AND dt.
    bool  lookActive  = false;// RMB held || right stick past deadzone; gates rotation only, never WASD fly

    float trigger[2] = { 0.f, 0.f };   // LT, RT in [0,1]

    // ---- Digital ----
    std::uint32_t held     = 0;
    std::uint32_t pressed  = 0;   //  held & ~heldPrev   (exactly one frame)
    std::uint32_t released = 0;   // ~held &  heldPrev   (exactly one frame)

    // ---- Provenance (cosmetic: glyph prompts only) ----
    InputDeviceKind lastDevice = InputDeviceKind::KeyboardMouse;
    bool            analogMove = false;  // move came from a stick, not keys
    int             padSlot    = -1;     // active pad slot, -1 = none

    bool Held    (GameAction a) const noexcept { return (held     >> int(a)) & 1u; }
    bool Pressed (GameAction a) const noexcept { return (pressed  >> int(a)) & 1u; }
    bool Released(GameAction a) const noexcept { return (released >> int(a)) & 1u; }
};

// TODO(gamepad_input_architecture local co-op): local co-op / split screen. ActionState
// is a value type and GamepadDevice tracks 4 slots, so the shape is
// Input::Cmd(int playerSlot) + a PlayerInputComponent{ playerSlot } read inside
// PlayerControllerSystem's entity loop. That component must then be added to
// PlayerControlTickSystem::DeclareAccess, or the parallel batcher co-schedules a
// conflict.
