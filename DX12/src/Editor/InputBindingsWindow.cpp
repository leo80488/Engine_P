// InputBindingsWindow.cpp - the rebind / tuning / diagnostics panel.
//
// 1. REBIND. One row per GameAction (Key 1, Key 2, Gamepad), plus a table for the MOVE
//    composite (W/S/A/D) and the two stick pickers. A click hands the next physical control
//    to Input's one-shot capture. Mouse buttons capture too (Input reads them from a raw
//    pre-quarantine snapshot); the click that opened the cell cannot bind itself, because
//    anything already down when BeginCapture ran is ignored until released.
// 2. TUNE. Deadzone, response curve, look rate, mouse look scale and trigger threshold,
//    live-applied through Input::SetBindings.
// 3. DIAGNOSE. The readout reads the DEVICE (GamepadDevice, gated only on connected &&
//    focused && gamepadEnabled) and shows the gated ActionState beside it. ActionState drops
//    the pad while suppressGamepad is set (a focused ImGui text field) and zeroes movement
//    and non-host actions while the sim is not advancing (EngineMode::Edit, the editor
//    default), so "device 0.8, gameplay 0.0" is the diagnosis, not a bug in this panel.
// 4. DISCLAIM. A mapping context (Input::PushContext) can redefine an action, or with
//    blockLower unbind it, without changing a row here, so the active stack is bannered
//    above the table and listed under "Mapping contexts".
//
// Engine surfaces: Input::Get(), the InputTypes.h free functions and the host's
// saveCallback only. Do not include EngineSettings: persistence belongs to the host, which
// lets ShaderLab host this panel unchanged.
// Design: docs/design/gamepad_input_architecture.md.

#include "Editor/InputBindingsWindow.h"

#include "Input/InputSystem.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cfloat>      // FLT_MIN; otherwise only reached transitively through imgui.h
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace
{
// An unfinished capture keeps every gameplay action zeroed, so it self-cancels: long
// enough to fumble a stick click, short enough never to outlive a forgotten tab.
constexpr double kCaptureTimeoutSeconds = 5.0;

constexpr double kStatusFadeSeconds = 6.0;

// Test-buzz length before it silences itself. XInput vibration latches in the device
// until a zero is written, so the user pressing Stop is not a shutdown mechanism.
constexpr double kRumbleTestSeconds = 1.5;

// Ring radius for the stick readouts, in pixels.
constexpr float kStickRadius = 54.f;

const ImU32 kConflictBg   = IM_COL32(150,  45,  45, 110);
const ImU32 kCapturingBg  = IM_COL32( 45,  95, 150, 130);

const char* const kNoReceiverTooltip =
    "Nothing consumes this action yet.\n"
    "\n"
    "It ships UNBOUND in InputBindings::Defaults() on purpose: "
    "CharacterAnimDriver drains Dodge and Interact with 'default: break;', "
    "and nothing reads CharacterControllerComponent::wantsCrouch.\n"
    "\n"
    "You can bind it here and the binding will work -- Input will report the "
    "press -- but it will produce ZERO visible result, which is the hardest "
    "kind of bug to chase. Bind it the day the consumer lands.";

// Crouch / Interact ship UNBOUND in InputBindings::Defaults() on purpose: nothing
// consumes them yet (see the GameAction enum in InputTypes.h; kNoReceiverTooltip says why).
bool HasNoReceiver(GameAction a) noexcept
{
    return a == GameAction::Crouch
        || a == GameAction::Interact;
}

// Appends s to out[cap] at len, always terminating; returns the new length. Unlike
// accumulating snprintf's would-be length, it never walks the cursor past the buffer.
int AppendStr(char* out, int cap, int len, const char* s)
{
    if (cap <= 0) return 0;
    while (len < cap - 1 && s && *s) out[len++] = *s++;
    out[len] = '\0';
    return len;
}

void KeyLabel(std::uint16_t vk, char* out, int cap)
{
    if (vk == 0) { std::snprintf(out, cap, "-"); return; }
    if (const char* n = VirtualKeyName(vk)) { std::snprintf(out, cap, "%s", n); return; }
    // Unknown VK: show the code so a corrupt settings.json stays diagnosable.
    std::snprintf(out, cap, "vk 0x%02X", static_cast<unsigned>(vk));
}

void PadLabel(std::uint16_t mask, char* out, int cap)
{
    if (cap <= 0) return;
    out[0] = '\0';
    if (mask == 0) { std::snprintf(out, cap, "-"); return; }

    // ActionBinding::pad is an OR of bits and any set bit fires, so a multi-bit mask is legal
    // (hand-authored settings.json). Name every bit, joined by '+', so none is hidden.
    int len = 0;
    for (int b = 0; b < 16; ++b)
    {
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << b);
        if (!(mask & bit)) continue;

        char        fallback[16];
        const char* n = PadButtonName(bit);
        if (!n)
        {
            std::snprintf(fallback, sizeof(fallback), "bit%d", b);
            n = fallback;
        }
        if (len) len = AppendStr(out, cap, len, "+");
        len = AppendStr(out, cap, len, n);
    }
}

// ==== Conflict domains ====
// Gameplay, UI (UIConfirm / UICancel) and Host (MenuToggle, IsHostAction(): fires while
// paused and opens the menu) are never consumed at the same time, so sharing a control
// across them is intended. Defaults() ships such pairs: Jump + UIConfirm on PadBtn::A,
// Dodge + UICancel on PadBtn::B, MenuToggle + UICancel on Escape. Only same-domain
// collisions are conflicts; tinting the others would paint the untouched defaults red.
bool IsUIAction(GameAction a) noexcept
{
    return a == GameAction::UIConfirm
        || a == GameAction::UICancel;
}

constexpr int kDomainGameplay  = 0;
constexpr int kDomainUI        = 1;
constexpr int kDomainHost      = 2;
constexpr int kDomainComposite = 3;

// UI, Host or Gameplay, never kDomainComposite: that fourth domain is the MOVE composite
// (W/S/A/D), which is not a GameAction. It conflicts with gameplay but not with UI or host,
// hence DomainsConflict() is a compatibility test instead of ==.
int ActionDomain(GameAction a) noexcept
{
    if (IsUIAction(a))   return kDomainUI;
    if (IsHostAction(a)) return kDomainHost;
    return kDomainGameplay;
}

// True when a control shared between these two domains is a REAL mistake.
bool DomainsConflict(int da, int db) noexcept
{
    if (da == db) return true;

    // WASD is held while gameplay actions fire, so Jump on W breaks both. The composite does
    // not conflict with UI or host: those are never consumed at the same time, and Escape is
    // already MenuToggle + UICancel in the defaults.
    return (da == kDomainComposite && db == kDomainGameplay)
        || (da == kDomainGameplay  && db == kDomainComposite);
}

// DomainsConflict() for two ACTION INDICES.
bool SameDomain(int a, int b) noexcept
{
    return DomainsConflict(ActionDomain(static_cast<GameAction>(a)),
                           ActionDomain(static_cast<GameAction>(b)));
}

// The move-composite keys by index: 0 forward, 1 back, 2 left, 3 right - the order of
// CaptureSlot::MoveFwd..MoveRight and of the table rows.
std::uint16_t CompositeVkAt(const InputBindings& b, int i) noexcept
{
    switch (i)
    {
    case 0:  return b.move.vkPosY;
    case 1:  return b.move.vkNegY;
    case 2:  return b.move.vkNegX;
    case 3:  return b.move.vkPosX;
    default: return 0;
    }
}

const char* const kCompositeRowNames[4] =
{
    "Move Forward", "Move Back", "Move Left", "Move Right"
};

const char* const kCompositeFieldNames[4] =
{
    "move.vkPosY", "move.vkNegY", "move.vkNegX", "move.vkPosX"
};

// A control conflicts when the same control is reachable from a second place in a
// conflicting domain. Comparing against every other slot, the action's other slot
// included, also catches "Key 1 == Key 2". The move-composite keys are part of the scan:
// without them Jump on W tints nothing and the character jumps on every step forward.
bool KeyConflicts(const InputBindings& b, int action, int slot)
{
    const std::uint16_t vk = b.action[action].vk[slot];
    if (!vk) return false;                       // unbound never conflicts
    for (int a = 0; a < kGameActionCount; ++a)
    {
        if (a != action && !SameDomain(a, action)) continue;
        for (int s = 0; s < 2; ++s)
        {
            if (a == action && s == slot) continue;
            if (b.action[a].vk[s] == vk) return true;
        }
    }

    if (DomainsConflict(ActionDomain(static_cast<GameAction>(action)), kDomainComposite))
        for (int i = 0; i < 4; ++i)
            if (CompositeVkAt(b, i) == vk) return true;

    return false;
}

// KeyConflicts for a move-composite key: catches another direction on the same key (a
// jammed axis) and a gameplay action sharing it.
bool CompositeKeyConflicts(const InputBindings& b, int which)
{
    const std::uint16_t vk = CompositeVkAt(b, which);
    if (!vk) return false;

    for (int i = 0; i < 4; ++i)
        if (i != which && CompositeVkAt(b, i) == vk) return true;

    for (int a = 0; a < kGameActionCount; ++a)
    {
        if (!DomainsConflict(kDomainComposite, ActionDomain(static_cast<GameAction>(a))))
            continue;
        for (int s = 0; s < 2; ++s)
            if (b.action[a].vk[s] == vk) return true;
    }
    return false;
}

bool PadConflicts(const InputBindings& b, int action)
{
    const std::uint16_t m = b.action[action].pad;
    if (!m) return false;
    for (int a = 0; a < kGameActionCount; ++a)
    {
        if (a == action) continue;
        if (!SameDomain(a, action)) continue;
        if (b.action[a].pad & m) return true;     // ANY shared bit is a conflict
    }
    return false;
}

// Dot-in-a-ring: outer circle = full deflection, inner = the deadzone. The bright dot is
// the RAW stick: the cooked value has the deadzone subtracted and is rescaled to [0,1], so
// it cannot be compared with the ring. Raw vs ring answers "is my stick inside the
// deadzone?"; the dim dot shows where the remap sent it, so the curve is visible too.
void DrawStickRing(const char* caption,
                   float rawX,    float rawY,
                   float cookedX, float cookedY,
                   float deadzone, ImU32 dotCol)
{
    ImGui::BeginGroup();
    ImGui::TextUnformatted(caption);

    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + kStickRadius, p.y + kStickRadius);

    dl->AddLine(ImVec2(c.x - kStickRadius, c.y), ImVec2(c.x + kStickRadius, c.y),
                IM_COL32(80, 80, 80, 180));
    dl->AddLine(ImVec2(c.x, c.y - kStickRadius), ImVec2(c.x, c.y + kStickRadius),
                IM_COL32(80, 80, 80, 180));
    dl->AddCircle(c, kStickRadius, IM_COL32(140, 140, 140, 220), 48, 1.5f);
    if (deadzone > 0.f)
        dl->AddCircle(c, kStickRadius * deadzone, IM_COL32(220, 140, 60, 220), 32, 1.5f);

    // GamepadState/ActionState use +y = stick up; ImGui screen y grows down, hence the negate.
    auto plot = [&](float x, float y, float r, ImU32 col)
    {
        const float cx = std::clamp(x, -1.f, 1.f);
        const float cy = std::clamp(y, -1.f, 1.f);
        dl->AddCircleFilled(ImVec2(c.x + cx * kStickRadius, c.y - cy * kStickRadius), r, col);
    };

    // Cooked first, so the raw dot draws on top when they coincide at rest.
    plot(cookedX, cookedY, 4.f, IM_COL32(160, 160, 160, 130));
    plot(rawX,    rawY,    5.f, dotCol);

    ImGui::Dummy(ImVec2(kStickRadius * 2.f, kStickRadius * 2.f));
    ImGui::Text("raw %+.2f, %+.2f", rawX, rawY);
    ImGui::TextDisabled("out %+.2f, %+.2f", cookedX, cookedY);
    ImGui::EndGroup();
}

} // namespace

namespace Editor
{

InputBindingsWindow& InputBindingsWindow::Get() noexcept
{
    static InputBindingsWindow s_instance;
    return s_instance;
}

bool InputBindingsWindow::WantsExclusiveKeyboard() const noexcept
{
    return m_capture.slot != CaptureSlot::None
        || m_consumedKeyFrame == ImGui::GetFrameCount();
}

void InputBindingsWindow::Close() noexcept
{
    m_open = false;
    // Never leave the engine in capture mode with no UI able to end it: that zeroes every
    // gameplay action and reads as "input stopped working".
    if (m_capture.slot != CaptureSlot::None)
    {
        Input::Get().CancelCapture();
        m_capture = {};
    }

    // XInput vibration latches until a zero is written and the engine does not know about
    // the test buzz, so stop it unconditionally on close: one queued zero, and
    // m_rumbleStopTime may be stale.
    Input::Get().QueueRumble(-1, 0.f, 0.f);
    m_rumbleStopTime = 0.0;
}

void InputBindingsWindow::Toggle() noexcept
{
    if (m_open) Close();
    else        Open();
}

// ==== Movement composite slots ====

bool InputBindingsWindow::IsCompositeSlot(CaptureSlot s) noexcept
{
    return s >= CaptureSlot::MoveFwd && s <= CaptureSlot::MoveRight;
}

int InputBindingsWindow::CompositeIndex(CaptureSlot s) noexcept
{
    if (!IsCompositeSlot(s)) return -1;
    return static_cast<int>(s) - static_cast<int>(CaptureSlot::MoveFwd);
}

const char* InputBindingsWindow::CompositeSlotName(CaptureSlot s) noexcept
{
    const int i = CompositeIndex(s);
    return (i < 0) ? "" : kCompositeRowNames[i];
}

std::uint16_t* InputBindingsWindow::CompositeVkPtr(CaptureSlot s) noexcept
{
    switch (s)
    {
    case CaptureSlot::MoveFwd:   return &m_edit.move.vkPosY;
    case CaptureSlot::MoveBack:  return &m_edit.move.vkNegY;
    case CaptureSlot::MoveLeft:  return &m_edit.move.vkNegX;
    case CaptureSlot::MoveRight: return &m_edit.move.vkPosX;
    default:                     return nullptr;
    }
}

// ==== Capture ====

void InputBindingsWindow::BeginCaptureFor(int actionIndex, CaptureSlot slot)
{
    // Re-targeting mid-capture is legal: BeginCapture() clears the engine-side pending vk/pad,
    // so the previous target never resolves.
    Input::Get().BeginCapture();
    m_capture.slot      = slot;
    m_capture.action    = actionIndex;
    m_capture.startTime = ImGui::GetTime();
}

void InputBindingsWindow::AbortCapture(const char* why)
{
    Input::Get().CancelCapture();
    m_capture = {};
    if (why && *why) SetStatus("%s", why);
}

void InputBindingsWindow::WriteCapturedBinding(bool isPad, std::uint16_t vk, std::uint16_t pad)
{
    const int         a    = m_capture.action;
    const CaptureSlot slot = m_capture.slot;
    m_capture = {};

    // Movement composite. Handled before the action-index guard: composite targets carry
    // action = -1 and the guard would drop them.
    if (std::uint16_t* dst = CompositeVkPtr(slot))
    {
        // A pad capture is REJECTED, not redirected: the composite's analog source is the one
        // stick chosen by the combo under this table, and swallowing the press would read as a
        // dead cell.
        if (isPad)
        {
            SetStatus("%s takes a KEY -- movement axes take keys; "
                      "the stick is chosen below.", CompositeSlotName(slot));
            return;
        }

        *dst = vk;
        PushToEngine();

        char label[64];
        KeyLabel(vk, label, sizeof(label));

        char path[64];
        if (FormatBindingPath(false, vk, path, static_cast<int>(sizeof(path))))
            SetStatus("%s -> %s  (%s)", CompositeSlotName(slot), label, path);
        else
            SetStatus("%s -> %s", CompositeSlotName(slot), label);
        return;
    }

    if (a < 0 || a >= kGameActionCount) return;

    // A device/column mismatch is REJECTED, not redirected: the control would land in a
    // column the player did not click, while a refusal can be retried at once.
    const bool wantsPad = (slot == CaptureSlot::Pad);
    if (wantsPad != isPad)
    {
        SetStatus("%s", isPad ? "That is a gamepad control -- click the Gamepad cell."
                              : "That is a keyboard key -- click a Key cell.");
        return;
    }

    char label[64];
    if (wantsPad)
    {
        m_edit.action[a].pad = pad;
        PadLabel(pad, label, sizeof(label));
    }
    else
    {
        m_edit.action[a].vk[(slot == CaptureSlot::Key0) ? 0 : 1] = vk;
        KeyLabel(vk, label, sizeof(label));
    }

    PushToEngine();

    // Echo the settings.json path so a later hand-edit of the file is a copy/paste.
    char path[64];
    if (FormatBindingPath(wantsPad, wantsPad ? pad : vk, path, static_cast<int>(sizeof(path))))
        SetStatus("%s -> %s  (%s)", ToString(static_cast<GameAction>(a)), label, path);
    else
        SetStatus("%s -> %s", ToString(static_cast<GameAction>(a)), label);
}

void InputBindingsWindow::TickCapture()
{
    // Idempotent per ImGui frame: the host calls this early (before its undo shortcuts) and
    // Render() calls it again; without the guard a capture resolved by the early call could be
    // read twice.
    const int frame = ImGui::GetFrameCount();
    if (frame == m_lastTickFrame) return;
    m_lastTickFrame = frame;

    TickRumbleTest();

    if (m_capture.slot == CaptureSlot::None) return;

    // ImGui's title-bar X writes through the bool* given to Begin() and never calls Close().
    if (!m_open) { AbortCapture(nullptr); return; }

    Input& input = Input::Get();

    bool          isPad = false;
    std::uint16_t vk    = 0;
    std::uint16_t pad   = 0;
    if (input.TakeCapture(isPad, vk, pad))
    {
        // The key edge that ended the capture stays visible to ImGui::IsKeyPressed for the rest
        // of the frame, so claim the frame's keyboard; otherwise binding Ctrl+Z also undoes.
        m_consumedKeyFrame = frame;

        // Escape never arrives here: Input::TickCapture() handles VK_ESCAPE itself without
        // arming m_captureReady, so that path lands in the desync check below.
        WriteCapturedBinding(isPad, vk, pad);
        return;
    }

    // The engine can end a capture without handing over a control: Input::TickCapture()
    // consumes Escape, and Input::Reset() (focus loss, world teardown) clears the pending
    // capture. Without this check the panel keeps "listening" until the timeout, swallows the
    // host's undo/redo/copy shortcuts meanwhile and reports "timed out" for a cancel.
    // Keep it after the TakeCapture() branch: a successful capture also clears
    // CapturePending(), so testing this first would discard every rebind as a cancel.
    if (!input.CapturePending())
    {
        m_consumedKeyFrame = frame;   // Escape's edge is still live for ImGui
        AbortCapture("Rebind cancelled.");
        return;
    }

    // Second cancel path: an Escape swallowed by ImGui (a focused text field, a popup) never
    // reaches the engine's key table, so watch the ImGui key too.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_consumedKeyFrame = frame;
        AbortCapture("Rebind cancelled.");
        return;
    }

    if (ImGui::GetTime() - m_capture.startTime > kCaptureTimeoutSeconds)
    {
        // A timeout is not a key press, but claiming the frame keeps "capture ended" one uniform
        // state for the host.
        m_consumedKeyFrame = frame;
        AbortCapture("Rebind timed out.");
    }
}

void InputBindingsWindow::TickRumbleTest()
{
    if (m_rumbleStopTime <= 0.0) return;
    if (ImGui::GetTime() < m_rumbleStopTime) return;

    // Deadline reached: nothing else tracks the test buzz, and the motors run until a zero is
    // written.
    Input::Get().QueueRumble(-1, 0.f, 0.f);
    m_rumbleStopTime = 0.0;
}

// ==== Bindings plumbing ====

void InputBindingsWindow::SeedFromEngine()
{
    m_edit   = Input::Get().Bindings();
    m_seeded = true;
}

void InputBindingsWindow::PushToEngine()
{
    Input& input = Input::Get();
    input.SetBindings(m_edit);
    // SetBindings Sanitize()s its input; re-pull so a clamped value shows what the engine
    // runs, not the out-of-range number the user typed.
    m_edit = input.Bindings();
}

void InputBindingsWindow::SetStatus(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(m_status, sizeof(m_status), fmt, args);
    va_end(args);
    m_statusTime = ImGui::GetTime();
}

// ==== Draw ====

void InputBindingsWindow::DrawBindingCell(int actionIndex, CaptureSlot slot, bool conflict)
{
    // Composite cells pass actionIndex = -1; the slot alone identifies them, so the
    // (slot, action) pair stays unique and the capture bookkeeping needs no special case.
    std::uint16_t* const composite = CompositeVkPtr(slot);
    const bool capturing = (m_capture.slot == slot && m_capture.action == actionIndex);

    char label[80];
    if (capturing)
    {
        // Mouse and pad buttons are both capturable; "press any key" would suggest a click is
        // ignored or cancels.
        std::snprintf(label, sizeof(label), "%s",
                      composite ? "[press any key...]" : "[press any key or button...]");
    }
    else if (composite)
    {
        KeyLabel(*composite, label, sizeof(label));
    }
    else if (slot == CaptureSlot::Pad)
    {
        PadLabel(m_edit.action[actionIndex].pad, label, sizeof(label));
    }
    else
    {
        KeyLabel(m_edit.action[actionIndex].vk[(slot == CaptureSlot::Key0) ? 0 : 1],
                 label, sizeof(label));
    }

    if (capturing)      ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kCapturingBg);
    else if (conflict)  ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kConflictBg);

    // Stride 16 exceeds the eight CaptureSlot enumerators, so a composite row never shares an
    // action row's ID.
    ImGui::PushID(actionIndex * 16 + static_cast<int>(slot));
    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.f)))
        BeginCaptureFor(actionIndex, slot);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        // Clearing the cell being captured must also disarm the capture, or the next press writes
        // straight back into it. Abort first, then clear.
        if (capturing) AbortCapture(nullptr);

        // Right-click clears: a capture can only produce a bound control, so this is the only
        // way back to "unbound" short of editing settings.json.
        if (composite)                     *composite = 0;
        else if (slot == CaptureSlot::Pad) m_edit.action[actionIndex].pad = 0;
        else m_edit.action[actionIndex].vk[(slot == CaptureSlot::Key0) ? 0 : 1] = 0;
        PushToEngine();
        SetStatus("%s: cleared.", composite ? CompositeSlotName(slot)
                                            : ToString(static_cast<GameAction>(actionIndex)));
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip())
    {
        char path[64];
        const bool isPad = (!composite && slot == CaptureSlot::Pad);
        const std::uint16_t val = composite
            ? *composite
            : (isPad ? m_edit.action[actionIndex].pad
                     : m_edit.action[actionIndex].vk[(slot == CaptureSlot::Key0) ? 0 : 1]);

        if (composite)
            ImGui::Text("settings.json field: input.%s",
                        kCompositeFieldNames[CompositeIndex(slot)]);

        if (FormatBindingPath(isPad, val, path, static_cast<int>(sizeof(path))))
            ImGui::Text("settings.json path: %s", path);
        else
            ImGui::TextUnformatted("Unbound");

        if (conflict)
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.45f, 1.f),
                               composite
                                   ? "Conflict: this key is also a gameplay action "
                                     "or another movement direction."
                                   : "Conflict: this control is bound to another action.");

        if (composite)
            ImGui::TextDisabled("Keyboard only -- the analog source is the "
                                "\"Move stick\" combo below.");

        ImGui::Separator();
        ImGui::TextUnformatted("Left-click: rebind   Right-click: clear");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

// Joins the ACTIVE context names into out, highest priority first. Returns the count
// written.
static int FormatActiveContextNames(const Input& input, char* out, int cap)
{
    if (cap > 0) out[0] = '\0';
    const int n = input.ActiveContextCount();
    int len = 0, written = 0;
    for (int i = 0; i < n; ++i)
    {
        const char* nm = input.ActiveContextName(i);
        if (!nm || !*nm) continue;
        if (written) len = AppendStr(out, cap, len, " > ");
        len = AppendStr(out, cap, len, nm);
        ++written;
    }
    return written;
}

// The table below is the BASE table. A higher-priority context can redefine an action and
// a blockLower context can unbind one without touching a row, so while any context is
// active this banner says so; otherwise the table reads as authoritative.
void InputBindingsWindow::DrawContextBanner()
{
    const Input& input = Input::Get();
    const int    n     = input.ActiveContextCount();
    if (n <= 0) return;

    char names[256];
    FormatActiveContextNames(input, names, static_cast<int>(sizeof(names)));

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.72f, 0.30f, 1.f));
    ImGui::TextWrapped(
        "%d mapping context%s ACTIVE (%s). The editable cells below are the BASE "
        "table; the \"Live now\" column shows what each action actually resolves "
        "to through the context stack. See \"Mapping contexts\" further down.",
        n, (n == 1) ? " is" : "s are", names);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void InputBindingsWindow::DrawBindingTable()
{
    DrawContextBanner();

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;

    // The "Live" column only shows while a context is active; with an empty stack it would
    // restate the base table.
    const bool showLive = Input::Get().ActiveContextCount() > 0;
    const int  columns  = showLive ? 5 : 4;

    if (!ImGui::BeginTable("##bindings", columns, kFlags)) return;

    ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Key 1",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Key 2",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Gamepad", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    if (showLive)
        ImGui::TableSetupColumn("Live now", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableHeadersRow();

    for (int a = 0; a < kGameActionCount; ++a)
    {
        const GameAction act = static_cast<GameAction>(a);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ToString(act));

        if (IsHostAction(act))
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(host)");
            ImGui::SetItemTooltip(
                "Host-level action: keeps firing while the sim is paused "
                "(exempt from the simAdvancing gate).");
        }

        if (HasNoReceiver(act))
        {
            ImGui::SameLine();
            ImGui::TextDisabled("[no receiver]");
            ImGui::SetItemTooltip("%s", kNoReceiverTooltip);
        }

        ImGui::TableSetColumnIndex(1);
        DrawBindingCell(a, CaptureSlot::Key0, KeyConflicts(m_edit, a, 0));
        ImGui::TableSetColumnIndex(2);
        DrawBindingCell(a, CaptureSlot::Key1, KeyConflicts(m_edit, a, 1));
        ImGui::TableSetColumnIndex(3);
        DrawBindingCell(a, CaptureSlot::Pad, PadConflicts(m_edit, a));

        if (showLive)
        {
            // Ask the ENGINE what drives this action (Input::ResolvedBinding) instead of re-walking
            // the context stack here: two copies of the resolution rule would drift.
            ImGui::TableSetColumnIndex(4);
            ImGui::AlignTextToFramePadding();

            const char* owner   = nullptr;
            bool        blocked = false;
            const ActionBinding& live =
                Input::Get().ResolvedBinding(act, &owner, &blocked);

            if (blocked)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.45f, 0.40f, 1.f));
                ImGui::Text("blocked by %s", owner ? owner : "?");
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip(
                    "A context with blockLower is active and does not define this "
                    "action, so it reads as unbound. Host actions are exempt.");
            }
            else
            {
                char keyBuf[48] = "-";
                char padBuf[48] = "-";
                if (live.vk[0]) FormatBindingPath(false, live.vk[0], keyBuf, sizeof(keyBuf));
                if (live.pad)   FormatBindingPath(true,  live.pad,   padBuf, sizeof(padBuf));

                if (owner)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.f, 1.f));
                    ImGui::Text("%s / %s", keyBuf, padBuf);
                    ImGui::PopStyleColor();
                    ImGui::SetItemTooltip("Overridden by context \"%s\".", owner);
                }
                else
                {
                    ImGui::TextDisabled("%s / %s", keyBuf, padBuf);
                    ImGui::SetItemTooltip("No context defines this action; the base "
                                          "binding on the left is live.");
                }
            }
        }
    }

    ImGui::EndTable();

    // Mouse buttons are ordinary virtual keys (named "mouse1".."mouse5" in InputBindings.cpp)
    // and Input reads them from a raw pre-quarantine snapshot, so they capture even though
    // ImGui owns the pointer. Say so: otherwise a click looks like it cancels the capture and
    // Attack's default (mouse1) looks unbindable.
    ImGui::TextDisabled("Mouse buttons bind too: click a Key cell, then click "
                        "in the viewport (mouse1..mouse5). The click that OPENED "
                        "the cell is ignored until you release it.");
}

// ==== Movement axes ====
// move is an Axis2DBinding, not an ActionBinding, so it needs its own table to be rebound
// and conflict-tinted. The four cells are KEYBOARD ONLY: the composite's analog half is one
// whole stick, chosen by the combo underneath.
void InputBindingsWindow::DrawMovementTable()
{
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##moveaxes", 2, kFlags))
    {
        ImGui::TableSetupColumn("Movement axis", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Key",           ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        // Same order as CaptureSlot::MoveFwd..MoveRight and kCompositeRowNames.
        const CaptureSlot slots[4] = {
            CaptureSlot::MoveFwd, CaptureSlot::MoveBack,
            CaptureSlot::MoveLeft, CaptureSlot::MoveRight
        };

        for (int i = 0; i < 4; ++i)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kCompositeRowNames[i]);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", kCompositeFieldNames[i]);

            ImGui::TableSetColumnIndex(1);
            DrawBindingCell(-1, slots[i], CompositeKeyConflicts(m_edit, i));
        }

        ImGui::EndTable();
    }

    // ---- Stick assignment ----
    static const char* const kStickItems[2] = { "Left", "Right" };

    // Axis2DBinding::stick selects left (0) / right (1); Sanitize() clamps anything else, so
    // a corrupt value shows as "Left" and is repaired on the next push.
    int moveStick = (m_edit.move.stick != 0) ? 1 : 0;
    int lookStick = (m_edit.look.stick != 0) ? 1 : 0;

    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("Move stick", &moveStick, kStickItems, 2))
    {
        m_edit.move.stick = static_cast<std::uint8_t>(moveStick);
        PushToEngine();
        SetStatus("Move stick -> %s.", kStickItems[moveStick]);
    }
    ImGui::SetItemTooltip("Analog source for the movement composite. "
                          "Defaults to the LEFT stick.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("Look stick", &lookStick, kStickItems, 2))
    {
        m_edit.look.stick = static_cast<std::uint8_t>(lookStick);
        PushToEngine();
        SetStatus("Look stick -> %s.", kStickItems[lookStick]);
    }
    ImGui::SetItemTooltip("Analog source for look. Defaults to the RIGHT stick.");

    // move.stick defaults to 0 (left) and look.stick to 1 (right), but the in-class
    // initialiser of both is 0, so a look rebuilt from a value-initialised Axis2DBinding
    // points at the movement stick and walking spins the camera. It is a legal config, so warn
    // and change nothing: forcing them apart would hide the corruption this line exposes.
    if (m_edit.move.stick == m_edit.look.stick)
    {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.45f, 1.f),
                           "Move and Look are BOTH on the %s stick: walking will "
                           "also spin the camera.", kStickItems[moveStick]);
        ImGui::SetItemTooltip(
            "Legal, but almost never intended. The shipped defaults are "
            "move = Left, look = Right.\n\n"
            "If you did not set this yourself, look.stick was probably rebuilt "
            "from a value-initialised Axis2DBinding (whose stick is 0) instead "
            "of from InputBindings::Defaults().");
    }
}

void InputBindingsWindow::DrawTuning()
{
    bool changed = false;

    // Ranges mirror the bracketed limits on InputBindings in InputTypes.h, so Sanitize()
    // never moves a value mid-drag and the re-pull in PushToEngine cannot fight the drag.
    changed |= ImGui::SliderFloat("Stick deadzone",   &m_edit.stickDeadzone,    0.f,  0.9f, "%.3f");
    ImGui::SetItemTooltip("Radial, both sticks. Watch the ring in the readout below.");

    changed |= ImGui::SliderFloat("Stick curve exp",  &m_edit.stickCurveExp,    1.f,  4.f,  "%.2f");
    ImGui::SetItemTooltip("Exponent applied to stick MAGNITUDE only -- direction is untouched.");

    changed |= ImGui::SliderFloat("Look rate (deg/s)", &m_edit.lookRateDegPerSec, 10.f, 720.f, "%.0f");
    ImGui::SetItemTooltip("Pad look speed at full stick deflection. A RATE: the consumer multiplies by dt.");

    changed |= ImGui::SliderFloat("Mouse look scale", &m_edit.mouseLookScale,   0.05f, 20.f, "%.3f",
                                  ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("MULTIPLIES CameraControllerComponent::mouseSensitivity; "
                          "the per-entity value stays authoritative.");

    changed |= ImGui::SliderFloat("Trigger threshold", &m_edit.triggerThreshold, 0.f, 0.9f, "%.3f");
    ImGui::SetItemTooltip("Below this, LT/RT read 0 and the synthesized LT/RT buttons do not fire.");

    // No analog speed slider: stick deflection supplies DIRECTION only. Any input past the
    // deadzone moves at the gait's full speed, so the deadzone is the only control over when
    // movement starts.
    changed |= ImGui::Checkbox("Invert pad look Y",   &m_edit.padInvertLookY);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Invert mouse look Y", &m_edit.mouseInvertLookY);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Gamepad enabled",     &m_edit.gamepadEnabled);

    if (changed) PushToEngine();   // live-apply: tuning by feel needs no Apply click
}

// ==== Mapping contexts ====
// The ACTIVE stack is read-only: gameplay pushes and pops contexts (a menu opening, a
// vehicle entered), and editing it here would desync from the code that owns the push.
// The DEFINED list gets Push/Pop buttons as a labelled DEBUG affordance, so "B cancels in
// a menu but dodges in combat" can be checked before the menu exists.
void InputBindingsWindow::DrawContexts()
{
    const Input& input = Input::Get();
    const int    n     = input.ActiveContextCount();

    if (n <= 0)
    {
        ImGui::TextDisabled("No mapping context is active -- the binding table "
                            "above IS what gameplay resolves against.");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f),
                           "%d active. Resolution walks this list TOP DOWN; the "
                           "base table is only reached if nothing above claims "
                           "or blocks the action.", n);

        if (ImGui::BeginTable("##ctxstack", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Context");
            ImGui::TableSetupColumn("Priority");
            ImGui::TableHeadersRow();

            for (int i = 0; i < n; ++i)
            {
                const char* nm = input.ActiveContextName(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // i = 0 is the HIGHEST priority (InputSystem contract).
                ImGui::Text("%d%s", i, (i == 0) ? " *" : "");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted((nm && *nm) ? nm : "<unnamed>");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", input.ActiveContextPriority(i));
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("* = highest priority, evaluated first.");
    }

    // ---- Defined contexts ----
    ImGui::Separator();
    const int defined = input.DefinedContextCount();
    if (defined <= 0)
    {
        ImGui::TextDisabled("No contexts are DEFINED. Define them from Lua "
                            "(Input.DefineContext) -- the shipped starting point "
                            "is asset/scripts/config/InputContexts.lua.");
    }
    else
    {
        ImGui::Text("%d defined:", defined);
        ImGui::SameLine();
        ImGui::TextDisabled("(push/pop here is a DEBUG aid -- gameplay owns these)");
        ImGui::SetItemTooltip(
            "Pushing from the editor lets you verify a context resolves the way "
            "you expect before the gameplay code that pushes it exists.\n\n"
            "The next real push/pop from script will override whatever you do "
            "here, which is correct: script owns the stack.");

        for (int i = 0; i < defined; ++i)
        {
            const char* nm = input.DefinedContextName(i);
            if (!nm || !*nm) continue;

            ImGui::PushID(i);
            const bool active = input.IsContextActive(nm);

            if (ImGui::Button(active ? "Pop" : "Push"))
            {
                if (active) Input::Get().PopContext(nm);
                else        Input::Get().PushContext(nm);
            }
            ImGui::SameLine();
            if (active) ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f), "%s", nm);
            else        ImGui::TextUnformatted(nm);
            ImGui::PopID();
        }
    }

    ImGui::TextDisabled(
        "This panel edits the BASE table only. A context's own bindings are not "
        "editable here -- they are authored in Lua. The \"Live now\" column in "
        "the binding table shows what each action resolves to through the stack.");
}

void InputBindingsWindow::DrawButtons()
{
    if (ImGui::Button("Apply"))
    {
        PushToEngine();
        SetStatus("Applied to the running session.");
    }
    ImGui::SetItemTooltip("Tuning is already live-applied; this is here for the "
                          "paranoid and to re-push after an external change.");

    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults"))
    {
        m_edit = InputBindings::Defaults();
        PushToEngine();
        SetStatus("Reset to InputBindings::Defaults().");
    }

    ImGui::SameLine();
    const bool canSave = static_cast<bool>(saveCallback);
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("Save"))
    {
        // Persistence is the host's (this TU must not see EngineSettings); report only what the
        // callback returned.
        SetStatus("%s", saveCallback() ? "Saved." : "Save FAILED -- see the log.");
    }
    ImGui::EndDisabled();
    if (!canSave)
        ImGui::SetItemTooltip("No save callback installed by the host.");

    if (m_status[0] && (ImGui::GetTime() - m_statusTime) < kStatusFadeSeconds)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_status);
    }
}

void InputBindingsWindow::DrawDiagnostics()
{
    const Input&         input = Input::Get();
    const ActionState&   cmd   = input.Cmd();
    const GamepadDevice& pad   = input.Pad();

    // GamepadDevice::State() masks the slot with &3, so ActiveSlot() == -1 would read pad 3
    // instead of "nothing". Clamp it here, once.
    const int           activeSlot = pad.ActiveSlot();
    const GamepadState& dev        = pad.State(activeSlot < 0 ? 0 : activeSlot);

    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1.f), "DEVICE  (ungated)");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip(
        "Everything in this section comes straight from GamepadDevice, which\n"
        "gates only on (connected && focused && gamepadEnabled).\n"
        "\n"
        "It deliberately does NOT come from ActionState. The usercmd is gated\n"
        "twice more: suppressGamepad is true whenever ImGui wants the mouse --\n"
        "which is true while your pointer is over THIS panel -- and simAdvancing\n"
        "is false in EngineMode::Edit, the editor's default mode. A readout built\n"
        "on ActionState would therefore be all zeros exactly when you opened it.\n"
        "\n"
        "The gated usercmd is shown separately below, on purpose: 'device 0.8,\n"
        "gameplay 0.0' is the diagnosis, and you need both halves to see it.");

    // ---- Per-slot connection ----
    if (ImGui::BeginTable("##pads", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Packet");
        ImGui::TableHeadersRow();

        for (int s = 0; s < GamepadDevice::kMaxPads; ++s)
        {
            const GamepadState& st = pad.State(s);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d%s", s, (s == pad.ActiveSlot()) ? " *" : "");
            ImGui::TableSetColumnIndex(1);
            if (st.connected) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.45f, 1.f), "connected");
            else              ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", static_cast<unsigned>(st.packet));
        }
        ImGui::EndTable();
    }

    // Dead slots are probed one per GamepadDevice::kRescanSeconds, round-robin, so a pad
    // plugged in without a WM_DEVICECHANGE can take ~2 s to appear; the countdown keeps that
    // from looking like a hang.
    ImGui::Text("Next dead-slot probe in %.2f s", pad.SecondsToNextRescan());

    ImGui::Separator();

    // Sticks (device). Bright dot = PRE-deadzone, the only value the deadzone ring can be
    // compared with; dim dot = the same frame's cooked axis, so the deadzone subtraction and
    // the curve are both visible.
    DrawStickRing("Left stick",  dev.rawLx, dev.rawLy, dev.lx, dev.ly,
                  m_edit.stickDeadzone, IM_COL32(120, 200, 255, 255));
    ImGui::SameLine(0.f, 24.f);
    DrawStickRing("Right stick", dev.rawRx, dev.rawRy, dev.rx, dev.ry,
                  m_edit.stickDeadzone, IM_COL32(255, 190, 110, 255));
    ImGui::SameLine(0.f, 24.f);

    ImGui::BeginGroup();
    char bar[40];
    std::snprintf(bar, sizeof(bar), "LT %.2f", dev.lt);
    ImGui::ProgressBar(dev.lt, ImVec2(170.f, 0.f), bar);
    std::snprintf(bar, sizeof(bar), "RT %.2f", dev.rt);
    ImGui::ProgressBar(dev.rt, ImVec2(170.f, 0.f), bar);

    // Raw button mask, named: PadLabel joins every set bit with '+', so a chord is fully
    // visible.
    char btns[256];
    PadLabel(dev.buttons, btns, static_cast<int>(sizeof(btns)));
    ImGui::TextUnformatted("Buttons");
    if (dev.buttons) ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.5f, 1.f), "%s", btns);
    else             ImGui::TextDisabled("-");
    ImGui::TextDisabled("packet %u", static_cast<unsigned>(dev.packet));
    ImGui::EndGroup();

    ImGui::Separator();

    // The SAME frame as gameplay sees it, side by side with the device readout: every value
    // here has been through the gates, a mismatch IS the report, and the gate flags underneath
    // say which gate ate it.
    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.4f, 1.f), "AS GAMEPLAY SEES IT");
    ImGui::SameLine();
    ImGui::TextDisabled("(the gated ActionState -- what every consumer reads)");

    ImGui::BeginGroup();
    ImGui::Text("move       %+.3f, %+.3f   mag %.3f", cmd.moveX, cmd.moveY, cmd.moveMag);
    // Mouse look is a PIXEL DISPLACEMENT and the stick is a RATE: separate carriers, so a
    // stick value can never reach a pixel consumer (no framerate-dependent look). Never sum
    // these two readouts.
    ImGui::Text("lookMouse  %+.2f, %+.2f px", cmd.lookMouseDX, cmd.lookMouseDY);
    ImGui::Text("lookStick  %+.3f, %+.3f",    cmd.lookStickX,  cmd.lookStickY);
    ImGui::Text("trigger    LT %.2f  RT %.2f", cmd.trigger[0], cmd.trigger[1]);
    ImGui::EndGroup();

    ImGui::SameLine(0.f, 32.f);

    ImGui::BeginGroup();
    ImGui::Text("padSlot    %d",  cmd.padSlot);
    ImGui::Text("analogMove %s",  cmd.analogMove ? "yes" : "no");
    ImGui::Text("lastDevice %s",  ToString(cmd.lastDevice));
    ImGui::Text("lookActive %s",  cmd.lookActive ? "yes" : "no");
    ImGui::EndGroup();

    // The two gates behind "everything reads zero". App sets suppressGamepad from ImGui's
    // WantTextInput only, so the WantCaptureMouse half of this test over-reports.
    // simAdvancing lives above this layer and is only inferable, hence the softer wording.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || io.WantTextInput)
    {
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.4f, 1.f),
                           "suppressGamepad: ON (ImGui owns the mouse/keyboard) -- "
                           "the pad contributes 0 to the rows above.");
    }
    if (activeSlot >= 0 && cmd.padSlot < 0)
    {
        ImGui::TextDisabled("A pad is connected but the usercmd reports no pad slot: "
                            "gamepadEnabled off, window unfocused, or the sim is not "
                            "advancing (EngineMode::Edit).");
    }

    ImGui::Separator();

    // Digital. The Key / Pad / Pad edge columns are recomputed here from the binding table
    // against ungated sources (Input::IsKeyDown, GamepadState), not cmd.held/pressed/released:
    // in Edit mode BuildCommand zeroes every non-host action. The last three columns ARE the
    // usercmd, side by side for the same reason.
    if (ImGui::BeginTable("##edges", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Pad");
        ImGui::TableSetupColumn("Pad edge");
        ImGui::TableSetupColumn("Held");
        ImGui::TableSetupColumn("Pressed");
        ImGui::TableSetupColumn("Released");
        ImGui::TableHeadersRow();

        auto bitCell = [](bool on)
        {
            if (on) ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.5f, 1.f), "X");
            else    ImGui::TextDisabled(".");
        };

        for (int a = 0; a < kGameActionCount; ++a)
        {
            const GameAction     act = static_cast<GameAction>(a);
            const ActionBinding& ab  = m_edit.action[a];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ToString(act));

            const bool keyDown = (ab.vk[0] && input.IsKeyDown(ab.vk[0]))
                              || (ab.vk[1] && input.IsKeyDown(ab.vk[1]));
            const bool padDown = ab.pad && dev.Down(ab.pad);
            const bool padEdge = ab.pad && dev.Pressed(ab.pad);

            const bool bits[6] = {
                keyDown, padDown, padEdge,
                cmd.Held(act), cmd.Pressed(act), cmd.Released(act)
            };
            for (int c = 0; c < 6; ++c)
            {
                ImGui::TableSetColumnIndex(1 + c);
                bitCell(bits[c]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Key/Pad = live device through the binding table. "
                        "Held/Pressed/Released = the gated usercmd.");

    ImGui::Separator();

    // ---- Rumble test ----
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("Rumble low",  &m_rumbleLo, 0.f, 1.f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("Rumble high", &m_rumbleHi, 0.f, 1.f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Buzz"))
    {
        Input::Get().QueueRumble(-1, m_rumbleLo, m_rumbleHi);
        // Arm the auto-stop: XInput vibration latches in the device, so a buzz that waits for
        // Stop outlives the panel and the session.
        m_rumbleStopTime = ImGui::GetTime() + kRumbleTestSeconds;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        Input::Get().QueueRumble(-1, 0.f, 0.f);
        m_rumbleStopTime = 0.0;
    }
    ImGui::SetItemTooltip("Rumble is QUEUED and drained at the top of the next poll, "
                          "so nothing here touches XInput from the UI thread.");

    if (m_rumbleStopTime > 0.0)
    {
        ImGui::SameLine();
        const double left = m_rumbleStopTime - ImGui::GetTime();
        ImGui::TextDisabled("auto-stop in %.1fs", left < 0.0 ? 0.0 : left);
    }
}

void InputBindingsWindow::Render()
{
    // Runs even when closed, so a capture left pending by the title-bar X is torn down the
    // next frame.
    TickCapture();

    // Re-pull on every closed -> open transition. Bindings change behind our back (settings
    // load, Lua Input.SetBinding, another panel), and a stale working copy would be re-pushed
    // by the first slider drag, clobbering those changes.
    const bool justOpened = m_open && !m_wasOpen;
    m_wasOpen = m_open;

    if (!m_open) return;
    if (justOpened || !m_seeded) SeedFromEngine();

    ImGui::SetNextWindowSize(ImVec2(720.f, 780.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input Bindings", &m_open))
    {
        ImGui::End();
        if (!m_open) Close();   // collapsed AND closed in the same frame
        return;
    }

    if (m_capture.slot != CaptureSlot::None)
    {
        const double left = kCaptureTimeoutSeconds - (ImGui::GetTime() - m_capture.startTime);
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1.f),
                           "Listening for a control...  Escape cancels  (%.1fs)",
                           left < 0.0 ? 0.0 : left);
    }
    else
    {
        ImGui::TextDisabled("Left-click a cell to rebind (key, mouse button or "
                            "pad button), right-click to clear.");
    }
    ImGui::Separator();

    DrawBindingTable();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Movement axes", ImGuiTreeNodeFlags_DefaultOpen))
        DrawMovementTable();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Mapping contexts"))
        DrawContexts();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Tuning", ImGuiTreeNodeFlags_DefaultOpen))
        DrawTuning();

    ImGui::Spacing();
    DrawButtons();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Live diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
        DrawDiagnostics();

    ImGui::End();

    // ImGui wrote through &m_open if the user hit the title-bar X; route that through Close()
    // so the capture teardown lives in one place.
    if (!m_open) Close();
}

} // namespace Editor
