#include "Input/InputSystem.h"
#include "Input/InputProfiles.h"
#include "Platform/KeyboardState.h"   // the window's keys: the sampled table
#include "System/Log.h"

#ifdef WITH_HARNESS
#include "Harness/RunLedger.h"
#include "System/CommandLine.h"
#include "System/PathUtil.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

// ==== The per-frame input snapshot ====
// The order inside Update() is load-bearing: roll prev -> curr, decide focus once,
// sample the keyboard, poll the pad, then resolve both devices into one ActionState.
// Nothing downstream derives its own edges, so no consumer can double-consume or miss
// one.

namespace
{
    // Device-latch thresholds, deliberately above the deadzone: a worn stick resting at 0.26
    // must not own `lastDevice` and pin the prompt glyphs on controller art.
    constexpr float kLatchStickMag   = 0.50f;
    constexpr float kLatchMousePixels = 6.0f;

    // VK order must match Input::m_rawMouseCurr / m_rawMousePrev.
    constexpr int kMouseVkList[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                                     VK_XBUTTON1, VK_XBUTTON2 };

    inline float Len2(float x, float y) noexcept { return std::sqrt(x * x + y * y); }
}

Input& Input::Get() noexcept
{
    static Input s_instance;
    return s_instance;
}

void Input::Reset() noexcept
{
    InputProfiles::Registry::Get().ResetState();
    std::memset(m_curr, 0, sizeof(m_curr));
    std::memset(m_prev, 0, sizeof(m_prev));
    std::memset(m_mouseButtonSuppressed, 0, sizeof(m_mouseButtonSuppressed));
    m_pad.Reset();
    m_cmd      = ActionState{};
    m_heldPrev = 0;
    m_capturePending = false;
    m_captureReady   = false;
}

void Input::SetBindings(const InputBindings& b) noexcept
{
    m_bindings = b;
    m_bindings.Sanitize();
}

void Input::NeutralizeGameplay() noexcept
{
    const auto keep=(1u<<int(GameAction::MenuToggle))|(1u<<int(GameAction::UIConfirm))|(1u<<int(GameAction::UICancel));
    m_cmd.held&=keep; m_cmd.pressed&=keep; m_cmd.released&=keep; m_heldPrev&=keep;
    m_cmd.moveX=m_cmd.moveY=m_cmd.moveMag=0;
    m_cmd.lookMouseDX=m_cmd.lookMouseDY=m_cmd.lookStickX=m_cmd.lookStickY=0;
    m_cmd.lookActive=false;
}

void Input::Update(const UpdateInputs& callerIn)
{
    // What this update resolves: the caller's inputs, plus - harness builds -
    // the synthetic mouse look (step 4b). LastInputs() hands them back to App.
    m_lastIn = callerIn;
    UpdateInputs& in = m_lastIn;

    // ---- 1. Roll the keyboard table ----
    std::memcpy(m_prev, m_curr, sizeof(m_curr));
    std::memcpy(m_rawMousePrev, m_rawMouseCurr, sizeof(m_rawMouseCurr));

    // ---- 2. Focus gate ----
    // XInput and the GetAsyncKeyState fallback both read global state whatever window has
    // focus, so sample only while our process owns the foreground window; otherwise report
    // every control up this frame. m_prev (and the pad's prevButtons) are kept, so anything
    // held when focus is lost fires its release edge once and nothing sticks.
    bool focused = false;
    if (HWND fg = GetForegroundWindow())
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        focused = (pid == GetCurrentProcessId());
    }
#ifdef WITH_HARNESS
    if (m_focusOverride)
        focused = *m_focusOverride;   // Test.InputFocus
#endif

    if (!focused)
    {
        // Do not return here: the pad poll and the command build must still run, or a button
        // held at alt-tab stays latched and never emits its release edge. Skip only the key scan.
        std::memset(m_curr, 0, sizeof(m_curr));
        std::memset(m_rawMouseCurr, 0, sizeof(m_rawMouseCurr));
        std::memset(m_mouseButtonSuppressed, 0, sizeof(m_mouseButtonSuppressed));
        // A tap latched while unfocused must not replay when focus returns.
        if (in.keyboard)
            in.keyboard->DiscardTaps();
    }
    else
    {
        // ---- 3. Sample the keyboard ----
        // The window's KeyboardState: its WndProc's key and button messages, plus any key
        // pressed and released since the last frame (a tap a poll would miss). Without a window,
        // the poll (bit 0x8000 = down). Either way this prev/curr table provides the edges,
        // consistent for every reader this frame.
        if (in.keyboard)
            in.keyboard->SnapshotForFrame(m_curr);
        else
            for (int vk = 0; vk < kNumKeys; ++vk)
                m_curr[vk] = (GetAsyncKeyState(vk) & 0x8000) ? std::uint8_t(1) : std::uint8_t(0);

        // Snapshot the mouse buttons before the quarantine below hides them. Only the rebind
        // capture may read this: it is the unfiltered physical state, and feeding it to gameplay
        // would defeat the UI-capture gate.
        for (int i = 0; i < kNumMouseVks; ++i)
            m_rawMouseCurr[i] = m_curr[kMouseVkList[i]];

        // ---- 4. Mouse-button quarantine (all five buttons) ----
        // Canvas owns mouse gestures before gameplay phases run. Suppress hold and edge queries,
        // and keep a captured hold quarantined until its physical release, so leaving the UI
        // cannot synthesize a mid-drag press. BuildCommand reads this cooked table, never
        // GetAsyncKeyState, so the quarantine reaches every action bound to a mouse button
        // (Attack, UIConfirm).
        for (int i = 0; i < kNumMouseVks; ++i)
        {
            const int  vk = kMouseVkList[i];
            const bool physicalDown = m_curr[vk] != 0;
            if (in.suppressMouseButtons && physicalDown)
                m_mouseButtonSuppressed[i] = true;

            if (in.suppressMouseButtons || m_mouseButtonSuppressed[i])
            {
                m_curr[vk] = 0;
                m_prev[vk] = 0;
                if (!physicalDown)
                    m_mouseButtonSuppressed[i] = false;
            }
        }
    }

    // ---- 4a. The host's key quarantine ----
    // A key the host took (UpdateInputs::quarantineKey) stays up for the game until physically released.
    if (in.quarantineKey > 0 && in.quarantineKey < kNumKeys)
        m_keyQuarantined[in.quarantineKey] = 1;
    for (int vk = 0; vk < kNumKeys; ++vk)
    {
        if (!m_keyQuarantined[vk]) continue;
        const bool physicalDown = m_curr[vk] != 0;
        m_curr[vk] = 0;
        m_prev[vk] = 0;
        if (!physicalDown)
            m_keyQuarantined[vk] = 0;
    }

#ifdef WITH_HARNESS
    // ---- 4b. The harness's synthetic device (SynthHold, -input_replay) ----
    // After the focus gate on purpose (a hidden run never has focus) and before anything
    // resolves: from here on an injected key, pad or look is indistinguishable from a
    // physical one. -input_record writes the result.
    SynthApply(in);
#endif

    // ---- 4c. The host's focus ----
    // After the synthetic device on purpose: a test's keys are withheld exactly like a player's. m_prev is
    // kept, so a control held when the host took the input fires its release once.
    if (!in.hostFocus)
    {
        std::memset(m_curr, 0, sizeof(m_curr));
        std::memset(m_mouseButtonSuppressed, 0, sizeof(m_mouseButtonSuppressed));
        in.mouseLookDX   = 0.f;
        in.mouseLookDY   = 0.f;
        in.mouseLookHeld = false;
        focused          = false;
    }

    // ---- 5. Poll the gamepad ----
    // Runs focused or not (see step 2): the device neutralises its values while unfocused
    // but keeps prevButtons.
    m_pad.Poll(focused, m_bindings);

#ifdef WITH_HARNESS
    // ---- 5b. -input_record ----
    // The keyboard table, the active pad and the look as the bindings will read them; after
    // the poll, so a pad is its cooked state.
    SynthRecord(in);
#endif

    // ---- 6. Rebind capture ----
    // Before BuildCommand, so the capturing press is swallowed and cannot also fire gameplay.
    TickCapture();

    // ---- 7. Resolve both devices into the usercmd ----
    BuildCommand(in, focused);
}

#ifdef WITH_HARNESS
// ==== The synthetic device (see InputSystem.h) ====
// A test's keys join the sampled table, so everything from the bindings on runs as for a
// physical press.
struct Input::SynthIO
{
    struct Change
    {
        std::uint64_t    u = 0;
        std::vector<int> keys;
        bool             hasPad = false;
        GamepadState     pad{};
        bool             hasLook = false;
        SynthLookState   look{};
    };

    std::string   recordPath, replayPath;
    std::ofstream record;
    std::string   lastRecorded;            // the last line's body, to write changes only
    bool          recordedAny = false;

    // One entry per change, ascending by update index.
    std::vector<Change> replay;
    std::size_t   next = 0;
};

void Input::SynthPad(const GamepadState& s, int frames) noexcept
{
    m_synthPadState  = s;
    m_synthPadFrames = (frames < 0) ? kHoldUntilReleased : frames;
}

void Input::SynthLook(float dx, float dy, int frames, bool held) noexcept
{
    m_synthLook       = { dx, dy, held };
    m_synthLookFrames = (frames < 0) ? kHoldUntilReleased : frames;
}

bool Input::SynthHold(int vk, int frames) noexcept
{
    if (vk <= 0 || vk >= kNumKeys) return false;
    m_synthFrames[vk] = (frames < 0) ? kHoldUntilReleased : frames;
    return true;
}

void Input::SynthReleaseAll() noexcept
{
    std::memset(m_synthFrames, 0, sizeof(m_synthFrames));
}

Input::SynthStatus Input::Synth() const noexcept
{
    SynthStatus s;
    for (int vk = 1; vk < kNumKeys; ++vk)
        if (m_synthFrames[vk] != 0) ++s.held;
    s.pad     = m_synthPadFrames != 0 || m_replayPadOn;
    s.look    = m_synthLookFrames != 0 || m_replayLookOn;
    s.updates = m_updates;
    if (m_synthIO)
    {
        s.recording = m_synthIO->record.is_open();
        s.replaying = m_synthIO->next < m_synthIO->replay.size();
        if (!m_synthIO->replay.empty()) s.replayEnd = m_synthIO->replay.back().u;
    }
    return s;
}

void Input::SynthApply(UpdateInputs& in) noexcept
{
    if (!m_synthInit)
    {
        // Read once, at the first Update: the flags are the run's, not a frame's.
        m_synthInit = true;
        const auto& cl = Sys::CommandLine::Get();
        const std::string rec = cl.Get("-input_record");
        const std::string rep = cl.Get("-input_replay");
        if (!rec.empty() || !rep.empty())
        {
            m_synthIO = new SynthIO();   // process lifetime, like the singleton
            if (!rep.empty())
            {
                m_synthIO->replayPath = rep;
                std::ifstream in(Sys::U8Path(rep));
                std::string line;
                int lineNo = 0, bad = 0;
                while (in && std::getline(in, line))
                {
                    ++lineNo;
                    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                    const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
                    if (!j.is_object() || !j.contains("u") || !j["u"].is_number_unsigned() ||
                        !j.contains("keys") || !j["keys"].is_array())
                    {
                        if (bad++ == 0)
                            LOG_ERROR("[INPUT] -input_replay %s:%d is not {\"u\": N, \"keys\": [...]} - skipped",
                                      rep.c_str(), lineNo);
                        continue;
                    }
                    SynthIO::Change c;
                    c.u = j["u"].get<std::uint64_t>();
                    for (const auto& k : j["keys"])
                        if (k.is_number_integer() && k.get<int>() > 0 && k.get<int>() < kNumKeys)
                            c.keys.push_back(k.get<int>());
                    if (const auto p = j.find("pad"); p != j.end() && p->is_object())
                    {
                        c.hasPad     = true;
                        c.pad.buttons = static_cast<std::uint16_t>(p->value("b", 0));
                        c.pad.lx = p->value("lx", 0.f); c.pad.ly = p->value("ly", 0.f);
                        c.pad.rx = p->value("rx", 0.f); c.pad.ry = p->value("ry", 0.f);
                        c.pad.lt = p->value("lt", 0.f); c.pad.rt = p->value("rt", 0.f);
                    }
                    if (const auto l = j.find("look"); l != j.end() && l->is_object())
                    {
                        c.hasLook   = true;
                        c.look.dx   = l->value("dx", 0.f);
                        c.look.dy   = l->value("dy", 0.f);
                        c.look.held = l->value("held", 0) != 0;
                    }
                    m_synthIO->replay.push_back(std::move(c));
                }
                std::stable_sort(m_synthIO->replay.begin(), m_synthIO->replay.end(),
                                 [](const auto& a, const auto& b) { return a.u < b.u; });
                if (m_synthIO->replay.empty())
                    LOG_ERROR("[INPUT] -input_replay %s: no input in it (missing or empty file)", rep.c_str());
                else
                    LOG_INFO("[INPUT] replaying %s: %zu change(s) through update %llu (%d bad line(s))",
                             rep.c_str(), m_synthIO->replay.size(),
                             static_cast<unsigned long long>(m_synthIO->replay.back().u), bad);
                Harness::RunLedger::Get().NoteInputFiles(rep, rec);
            }
            if (!rec.empty())
            {
                m_synthIO->recordPath = rec;
                Sys::CreateParentDirs(rec);
                m_synthIO->record.open(Sys::U8Path(rec), std::ios::binary | std::ios::trunc);
                if (m_synthIO->record.is_open())
                    LOG_INFO("[INPUT] recording the keyboard table, the active pad and the mouse look to %s",
                             rec.c_str());
                else
                    LOG_ERROR("[INPUT] -input_record %s: cannot open it for writing", rec.c_str());
                Harness::RunLedger::Get().NoteInputFiles(rep, rec);
            }
        }
    }

    const std::uint64_t u = m_updates++;
    m_recordU = u;
    if (m_synthIO && m_synthIO->next < m_synthIO->replay.size() && m_synthIO->replay[m_synthIO->next].u <= u)
    {
        // Every change due by now (a file may list several for one update:
        // the last wins).
        while (m_synthIO->next < m_synthIO->replay.size() && m_synthIO->replay[m_synthIO->next].u <= u)
        {
            const SynthIO::Change& c = m_synthIO->replay[m_synthIO->next];
            std::memset(m_replayKeys, 0, sizeof(m_replayKeys));
            for (int vk : c.keys) m_replayKeys[vk] = 1;
            m_replayPadOn  = c.hasPad;
            m_replayPad    = c.pad;
            m_replayLookOn = c.hasLook;
            m_replayLook   = c.look;
            ++m_synthIO->next;
        }
        if (m_synthIO->next == m_synthIO->replay.size())
            LOG_INFO("[INPUT] replay: its last change applied at update %llu", static_cast<unsigned long long>(u));
    }

    bool any = false;
    for (int vk = 1; vk < kNumKeys; ++vk)
    {
        if (m_synthFrames[vk] != 0)
        {
            m_curr[vk] = 1;
            any = true;
            if (m_synthFrames[vk] > 0) --m_synthFrames[vk];
        }
        if (m_replayKeys[vk])
        {
            m_curr[vk] = 1;
            any = true;
        }
    }

    // The pad: SynthPad's while it lasts, else the replay's, else none. The
    // device applies it in the Poll that follows.
    if (m_synthPadFrames != 0)
    {
        m_pad.SetSynthetic(&m_synthPadState);
        if (m_synthPadFrames > 0) --m_synthPadFrames;
        any = true;
    }
    else if (m_replayPadOn)
    {
        m_pad.SetSynthetic(&m_replayPad);
        any = true;
    }
    else
        m_pad.SetSynthetic(nullptr);

    // The look: SynthLook's while it lasts, else the replay's - ADDED to the
    // mouse's own displacement (a hidden run has none), the gesture held as a
    // drag holds it.
    const SynthLookState* look = nullptr;
    if (m_synthLookFrames != 0)
    {
        look = &m_synthLook;
        if (m_synthLookFrames > 0) --m_synthLookFrames;
    }
    else if (m_replayLookOn)
        look = &m_replayLook;
    if (look)
    {
        in.mouseLookDX  += look->dx;
        in.mouseLookDY  += look->dy;
        in.mouseLookHeld = in.mouseLookHeld || look->held;
        any = true;
    }

    if (any) Harness::RunLedger::Get().NoteSyntheticInputFrame();
}

void Input::SynthRecord(const UpdateInputs& in) noexcept
{
    if (!m_synthIO || !m_synthIO->record.is_open()) return;
    std::string body = "\"keys\": [";
    bool first = true;
    for (int vk = 1; vk < kNumKeys; ++vk)
        if (m_curr[vk])
        {
            body += (first ? "" : ", ") + std::to_string(vk);
            first = false;
        }
    body += "]";
    // The ACTIVE pad as the bindings will read it (physical or synthetic),
    // only while it says something.
    const int slot = m_pad.ActiveSlot();
    if (slot >= 0)
    {
        const GamepadState& p = m_pad.State(slot);
        if (p.buttons || p.lx || p.ly || p.rx || p.ry || p.lt || p.rt)
        {
            // %.9g: a float32 survives the text round trip exactly, so a
            // replayed stick is the recorded one bit for bit.
            char pad[256];
            std::snprintf(pad, sizeof(pad),
                          ", \"pad\": {\"b\": %u, \"lx\": %.9g, \"ly\": %.9g, \"rx\": %.9g, \"ry\": %.9g, "
                          "\"lt\": %.9g, \"rt\": %.9g}",
                          static_cast<unsigned>(p.buttons), p.lx, p.ly, p.rx, p.ry, p.lt, p.rt);
            body += pad;
        }
    }
    // The look as the bindings will read it (the mouse's and the synthetic
    // one's), only while it says something. A displacement repeated on the
    // next update is not a change, so a steady drag is one line.
    if (in.mouseLookDX != 0.f || in.mouseLookDY != 0.f || in.mouseLookHeld)
    {
        char look[128];
        std::snprintf(look, sizeof(look), ", \"look\": {\"dx\": %.9g, \"dy\": %.9g, \"held\": %d}",
                      in.mouseLookDX, in.mouseLookDY, in.mouseLookHeld ? 1 : 0);
        body += look;
    }
    if (m_synthIO->recordedAny && body == m_synthIO->lastRecorded) return;
    m_synthIO->record << "{\"u\": " << m_recordU << ", " << body << "}\n";
    m_synthIO->record.flush();
    m_synthIO->lastRecorded = std::move(body);
    m_synthIO->recordedAny  = true;
}
#endif

// ==== Mapping contexts ====
// Contexts are only a lookup change: BuildCommand asks ResolveAction() which control
// drives an action instead of reading m_bindings, and nothing below the ActionState
// boundary learns that contexts exist. Keep it so: a feature that cannot be expressed
// inside BuildCommand belongs above that boundary.
namespace
{
    bool NameEqualsCI(const char* a, const char* b) noexcept
    {
        if (!a || !b) return false;
        for (;; ++a, ++b)
        {
            const int ca = std::tolower(static_cast<unsigned char>(*a));
            const int cb = std::tolower(static_cast<unsigned char>(*b));
            if (ca != cb) return false;
            if (ca == 0)  return true;
        }
    }
}

int Input::FindContext(const char* name) const noexcept
{
    if (!name || !*name) return -1;
    for (int i = 0; i < m_contextCount; ++i)
        if (NameEqualsCI(m_contexts[i].name, name))
            return i;
    return -1;
}

bool Input::DefineContext(const InputContext& c) noexcept
{
    if (c.name[0] == '\0')
    {
        LOG_WARNING("[Input] DefineContext: empty name ignored");
        return false;
    }

    const int existing = FindContext(c.name);
    if (existing >= 0)
    {
        // Redefining a live context takes effect immediately, so a Lua config hot-reload retunes
        // the running game. The active stack stores indices (no repair needed), but the priority
        // may have changed: re-sort.
        m_contexts[existing] = c;
        SortActiveContexts();
        return true;
    }

    if (m_contextCount >= kMaxContexts)
    {
        LOG_WARNING("[Input] DefineContext: table full (%d), '%s' ignored",
                    kMaxContexts, c.name);
        return false;
    }

    m_contexts[m_contextCount++] = c;
    return true;
}

void Input::SortActiveContexts() noexcept
{
    // Insertion sort by priority DESC, stable, so a later push wins a tie. n <= 8, and it
    // runs on push / pop, not per frame.
    for (int i = 1; i < m_activeCount; ++i)
    {
        const int  cur  = m_active[i];
        const int  prio = m_contexts[cur].priority;
        int j = i - 1;
        while (j >= 0 && m_contexts[m_active[j]].priority < prio)
        {
            m_active[j + 1] = m_active[j];
            --j;
        }
        m_active[j + 1] = cur;
    }
}

bool Input::PushContext(const char* name) noexcept
{
    const int idx = FindContext(name);
    if (idx < 0)
    {
        LOG_WARNING("[Input] PushContext: no context named '%s'", name ? name : "(null)");
        return false;
    }

    // Already active: re-seat rather than duplicate, so unbalanced push/pop
    // from script cannot stack a context N deep and require N pops to clear.
    for (int i = 0; i < m_activeCount; ++i)
    {
        if (m_active[i] == idx)
        {
            for (int j = i; j + 1 < m_activeCount; ++j)
                m_active[j] = m_active[j + 1];
            --m_activeCount;
            break;
        }
    }

    if (m_activeCount >= kMaxContexts) return false;
    m_active[m_activeCount++] = idx;
    SortActiveContexts();
    return true;
}

bool Input::PopContext(const char* name) noexcept
{
    const int idx = FindContext(name);
    if (idx < 0) return false;

    for (int i = 0; i < m_activeCount; ++i)
    {
        if (m_active[i] == idx)
        {
            for (int j = i; j + 1 < m_activeCount; ++j)
                m_active[j] = m_active[j + 1];
            --m_activeCount;
            return true;
        }
    }
    return false;
}

void Input::ClearContexts() noexcept
{
    m_activeCount = 0;   // definitions survive; only the stack is emptied
}

bool Input::IsContextActive(const char* name) const noexcept
{
    const int idx = FindContext(name);
    if (idx < 0) return false;
    for (int i = 0; i < m_activeCount; ++i)
        if (m_active[i] == idx) return true;
    return false;
}

const char* Input::ActiveContextName(int i) const noexcept
{
    if (i < 0 || i >= m_activeCount) return nullptr;
    return m_contexts[m_active[i]].name;
}

int Input::ActiveContextPriority(int i) const noexcept
{
    if (i < 0 || i >= m_activeCount) return 0;
    return m_contexts[m_active[i]].priority;
}

int Input::DefinedContextCount() const noexcept { return m_contextCount; }

const char* Input::DefinedContextName(int i) const noexcept
{
    if (i < 0 || i >= m_contextCount) return nullptr;
    return m_contexts[i].name;
}

// Resolution: walk the active stack, highest priority first:
//   defines it        -> that binding wins
//   blocks lower      -> unbound (how a modal menu silences gameplay without listing
//                        every action it suppresses)
//   neither           -> keep walking
//   nothing resolved  -> the base table
const ActionBinding& Input::ResolveAction(GameAction a, bool& outBlocked) const noexcept
{
    static const ActionBinding kUnbound {};
    outBlocked = false;

    const int idx = static_cast<int>(a);
    if (idx < 0 || idx >= kGameActionCount) return kUnbound;

    // Host actions are exempt from blockLower: otherwise a blocking menu context that forgets
    // to define MenuToggle swallows the button that would close it, locking the player in. A
    // context can still redefine a host action explicitly (even to nothing); it cannot
    // silence one by omission.
    const bool exemptFromBlock = IsHostAction(a);

    for (int i = 0; i < m_activeCount; ++i)
    {
        const InputContext& c = m_contexts[m_active[i]];
        if (c.Defines(a))                      return c.action[idx];
        if (c.blockLower && !exemptFromBlock) { outBlocked = true; return kUnbound; }
    }
    return m_bindings.action[idx];
}

const Axis2DBinding& Input::ResolveMove(bool& outBlocked) const noexcept
{
    static const Axis2DBinding kUnbound {};
    outBlocked = false;
    for (int i = 0; i < m_activeCount; ++i)
    {
        const InputContext& c = m_contexts[m_active[i]];
        if (c.definesMove)    return c.move;
        if (c.blockLower)   { outBlocked = true; return kUnbound; }
    }
    return m_bindings.move;
}

const Axis2DBinding& Input::ResolveLook(bool& outBlocked) const noexcept
{
    static const Axis2DBinding kUnbound {};
    outBlocked = false;
    for (int i = 0; i < m_activeCount; ++i)
    {
        const InputContext& c = m_contexts[m_active[i]];
        if (c.definesLook)    return c.look;
        if (c.blockLower)   { outBlocked = true; return kUnbound; }
    }
    return m_bindings.look;
}

// Public "what is actually live" queries. They share the private Resolve* walk, so a
// tool that explains input cannot disagree with the code that produces it.
const ActionBinding& Input::ResolvedBinding(GameAction a, const char** outOwner,
                                            bool* outBlocked) const noexcept
{
    if (outOwner) *outOwner = nullptr;

    // Find the deciding context for reporting. Cheap: kMaxContexts is 8 and
    // this runs from editor UI, not per frame.
    const bool exemptFromBlock = IsHostAction(a);
    for (int i = 0; i < m_activeCount; ++i)
    {
        const InputContext& c = m_contexts[m_active[i]];
        if (c.Defines(a))                     { if (outOwner) *outOwner = c.name; break; }
        if (c.blockLower && !exemptFromBlock) { if (outOwner) *outOwner = c.name; break; }
    }

    bool blocked = false;
    const ActionBinding& r = ResolveAction(a, blocked);
    if (outBlocked) *outBlocked = blocked;
    return r;
}

const Axis2DBinding& Input::ResolvedMove(const char** outOwner) const noexcept
{
    if (outOwner) *outOwner = nullptr;
    for (int i = 0; i < m_activeCount; ++i)
    {
        const InputContext& c = m_contexts[m_active[i]];
        if (c.definesMove || c.blockLower) { if (outOwner) *outOwner = c.name; break; }
    }
    bool blocked = false;
    return ResolveMove(blocked);
}

const Axis2DBinding& Input::ResolvedLook(const char** outOwner) const noexcept
{
    if (outOwner) *outOwner = nullptr;
    for (int i = 0; i < m_activeCount; ++i)
    {
        const InputContext& c = m_contexts[m_active[i]];
        if (c.definesLook || c.blockLower) { if (outOwner) *outOwner = c.name; break; }
    }
    bool blocked = false;
    return ResolveLook(blocked);
}

// ==== Rebind capture ====
void Input::BeginCapture() noexcept
{
    // Inside a member so it can see the private kNumMouseVks. The two must
    // agree or the raw snapshot loop walks off the end of the array.
    static_assert(sizeof(kMouseVkList) / sizeof(kMouseVkList[0]) == kNumMouseVks,
                  "kNumMouseVks must match kMouseVkList");

    m_capturePending = true;
    m_captureReady   = false;
    m_captureVk      = 0;
    m_capturePad     = 0;

    // Quarantine everything already held. The click that opened this cell is
    // still physically down (and an ImGui keyboard-nav activation leaves Enter
    // or Space down), so without this the capture would instantly bind the
    // control that started it. Each quarantined control clears the moment it is
    // observed released.
    for (int vk = 0; vk < kNumKeys; ++vk)
        m_captureIgnoreKey[vk] = m_curr[vk];

    // Mouse buttons come from the RAW table: the cooked one hides them while
    // the UI owns the pointer, which is exactly now.
    for (int i = 0; i < kNumMouseVks; ++i)
        if (m_rawMouseCurr[i])
            m_captureIgnoreKey[kMouseVkList[i]] = 1;

    const int slot = m_pad.ActiveSlot();
    m_captureIgnorePad = (slot >= 0) ? m_pad.State(slot).buttons : std::uint16_t(0);
}

void Input::TickCapture() noexcept
{
    if (!m_capturePending)
        return;

    // Escape always cancels instead of being captured, or the only way out of a mis-clicked
    // cell would be to bind something. The panel detects the cancel by CapturePending()
    // going false with no control captured.
    if (WasKeyPressed(VK_ESCAPE))
    {
        m_capturePending = false;
        return;
    }

    // ---- Gamepad ----
    const int slot = m_pad.ActiveSlot();
    if (slot >= 0)
    {
        const GamepadState& gs = m_pad.State(slot);
        // Release-clears the quarantine, one bit at a time.
        m_captureIgnorePad = static_cast<std::uint16_t>(m_captureIgnorePad & gs.buttons);

        const std::uint16_t edges =
            static_cast<std::uint16_t>(gs.buttons & ~gs.prevButtons & ~m_captureIgnorePad);
        if (edges)
        {
            // Isolate the lowest set bit so a simultaneous press binds one control.
            m_capturePad     = static_cast<std::uint16_t>(edges & (~edges + 1u));
            m_captureIsPad   = true;
            m_captureReady   = true;
            m_capturePending = false;
            return;
        }
    }

    // ---- Mouse buttons ----
    // Read from the raw pre-quarantine snapshot: the cooked table hides mouse buttons
    // whenever the UI owns the pointer, which is exactly when a rebind panel is up. Any
    // click other than the one that opened the cell binds.
    for (int i = 0; i < kNumMouseVks; ++i)
    {
        const int vk = kMouseVkList[i];
        if (!m_rawMouseCurr[i])
            m_captureIgnoreKey[vk] = 0;              // released -> quarantine lifted
        if (m_captureIgnoreKey[vk])
            continue;

        if (m_rawMouseCurr[i] && !m_rawMousePrev[i])
        {
            m_captureVk      = static_cast<std::uint16_t>(vk);
            m_captureIsPad   = false;
            m_captureReady   = true;
            m_capturePending = false;
            return;
        }
    }

    // ---- Keyboard ----
    for (int vk = 1; vk < kNumKeys; ++vk)
    {
        // Mouse VKs were handled above off the raw table; skip their (cooked,
        // possibly suppressed) duplicates here so one click cannot bind twice.
        bool isMouse = false;
        for (int m : kMouseVkList)
            if (vk == m) { isMouse = true; break; }
        if (isMouse) continue;

        if (!m_curr[vk])
            m_captureIgnoreKey[vk] = 0;              // released -> quarantine lifted
        if (m_captureIgnoreKey[vk])
            continue;

        if (WasKeyPressed(vk))
        {
            m_captureVk      = static_cast<std::uint16_t>(vk);
            m_captureIsPad   = false;
            m_captureReady   = true;
            m_capturePending = false;
            return;
        }
    }
}

bool Input::TakeCapture(bool& outIsPad, std::uint16_t& outVk, std::uint16_t& outPad) noexcept
{
    if (!m_captureReady)
        return false;
    outIsPad = m_captureIsPad;
    outVk    = m_captureVk;
    outPad   = m_capturePad;
    m_captureReady = false;
    m_captureVk    = 0;
    m_capturePad   = 0;
    return true;
}

// BuildCommand - the device-agnostic boundary: everything above it knows VK codes and
// sticks; nothing that reads m_cmd does.
void Input::BuildCommand(const UpdateInputs& in, bool focused) noexcept
{
    const InputBindings& b = m_bindings;
    ActionState cmd;

    const int slot = m_pad.ActiveSlot();
    // Both operands must be lvalues, or the ternary yields a prvalue and `pad`
    // silently binds to a lifetime-extended COPY made every frame.
    static const GamepadState kNeutralPad{};
    const GamepadState& pad = (slot >= 0) ? m_pad.State(slot) : kNeutralPad;
    bool padFocus = focused;
#ifdef WITH_HARNESS
    // The harness's synthetic pad needs no focus: a hidden test run has none.
    padFocus = padFocus || slot == GamepadDevice::kSynthSlot;
#endif
    const bool padUsable = padFocus && b.gamepadEnabled && !in.suppressGamepad && slot >= 0;

    cmd.padSlot = slot;

    // ---- Device provenance (cosmetic: drives prompt glyphs only) ----
    // Latch only on significant activity, so neither a drifting stick nor an incidental
    // mouse jiggle flips the prompts.
    InputDeviceKind latch = m_cmd.lastDevice;
    {
        bool kbmActive = std::fabs(in.mouseLookDX) > kLatchMousePixels
                      || std::fabs(in.mouseLookDY) > kLatchMousePixels;
        if (!kbmActive)
        {
            for (int vk = 1; vk < kNumKeys && !kbmActive; ++vk)
                kbmActive = (m_curr[vk] != 0 && m_prev[vk] == 0);
        }

        bool padActive = false;
        if (padUsable)
        {
            padActive = (pad.buttons & ~pad.prevButtons) != 0
                     || Len2(pad.lx, pad.ly) > kLatchStickMag
                     || Len2(pad.rx, pad.ry) > kLatchStickMag
                     || pad.lt > 0.f || pad.rt > 0.f;
        }

        // Keyboard wins a tie: it is unambiguous (a key is physically down),
        // whereas a stick can be worn.
        if (kbmActive)      latch = InputDeviceKind::KeyboardMouse;
        else if (padActive) latch = InputDeviceKind::Gamepad;
    }
    cmd.lastDevice = latch;

    // ---- Movement ----
    // Source priority is not `lastDevice`: a held move key is definite intent, while a stick
    // reading can come from wear. So the keyboard wins whenever a move key is down and the
    // stick supplies movement otherwise, which makes a mid-session swap (drop the pad, grab
    // WASD) work with no mode flag.
    {
        bool moveBlocked = false;
        const Axis2DBinding& mv = ResolveMove(moveBlocked);

        const float kx = (IsKeyDown(mv.vkPosX) ? 1.f : 0.f) - (IsKeyDown(mv.vkNegX) ? 1.f : 0.f);
        const float ky = (IsKeyDown(mv.vkPosY) ? 1.f : 0.f) - (IsKeyDown(mv.vkNegY) ? 1.f : 0.f);

        if (kx != 0.f || ky != 0.f)
        {
            // Keyboard is binary: normalise so a diagonal is not faster than a cardinal, and report
            // full magnitude.
            const float len = Len2(kx, ky);
            cmd.moveX   = kx / len;
            cmd.moveY   = ky / len;
            cmd.moveMag = 1.f;
            cmd.analogMove = false;
        }
        else if (padUsable && !moveBlocked)
        {
            const float sx = (mv.stick == 0) ? pad.lx : pad.rx;
            const float sy = (mv.stick == 0) ? pad.ly : pad.ry;
            const float mag = Len2(sx, sy);
            if (mag > 0.f)
            {
                cmd.moveX   = sx;
                cmd.moveY   = sy;
                cmd.moveMag = (mag > 1.f) ? 1.f : mag;
                cmd.analogMove = true;
            }
        }
    }

    // ---- Look: two carriers, kept apart by unit ----
    // lookMouse* is a displacement in pixels (never dt-scaled); lookStick* is a rate in
    // [-1,1] (always dt-scaled). As separate fields, a stick value cannot reach a consumer
    // that treats it as pixels, so framerate-dependent look cannot happen.
    // Mouse look is not context-gated: App::TickCamera is already gated by
    // ctx.cameraLookActive / viewport hover, and gating here too would freeze the editor
    // free-cam whenever a menu context exists.
    cmd.lookMouseDX = in.mouseLookDX * b.mouseLookScale;
    cmd.lookMouseDY = in.mouseLookDY * b.mouseLookScale;

    bool lookBlocked = false;
    const Axis2DBinding& lk = ResolveLook(lookBlocked);
    if (padUsable && !lookBlocked)
    {
        cmd.lookStickX = (lk.stick == 1) ? pad.rx : pad.lx;
        cmd.lookStickY = (lk.stick == 1) ? pad.ry : pad.ly;
    }
    cmd.lookActive = in.mouseLookHeld
                  || (cmd.lookStickX != 0.f || cmd.lookStickY != 0.f);

    if (padUsable)
    {
        cmd.trigger[0] = pad.lt;
        cmd.trigger[1] = pad.rt;
    }

    // ---- Digital actions ----
    // Rebuilt every frame from the cooked keyboard table, so the mouse-button quarantine
    // applies. Each action resolves through the context stack, so "B cancels in a menu but
    // dodges in combat" is a different control feeding the same action; no consumer below
    // ActionState can tell.
    std::uint32_t held = 0;
    for (int i = 0; i < kGameActionCount; ++i)
    {
        bool blocked = false;
        const ActionBinding& ab = ResolveAction(static_cast<GameAction>(i), blocked);
        if (blocked) continue;          // a blockLower context swallowed it

        bool down = (ab.vk[0] && IsKeyDown(ab.vk[0]))
                 || (ab.vk[1] && IsKeyDown(ab.vk[1]));
        if (!down && padUsable && ab.pad)
            down = (pad.buttons & ab.pad) != 0;
        if (down)
            held |= (1u << i);
    }

    // ---- Gates, applied before edges are derived ----
    // Zeroing after the edge derivation would let `pressed` be true on a frame where `held`
    // is false: a one-frame ghost press on the Play -> Pause frame.
    if (!in.simAdvancing)
    {
        for (int i = 0; i < kGameActionCount; ++i)
            if (!IsHostAction(static_cast<GameAction>(i)))
                held &= ~(1u << i);

        // The sim is not advancing: no movement intent either. Look is not gated: the editor
        // free-cam must stay navigable while stopped.
        cmd.moveX = cmd.moveY = cmd.moveMag = 0.f;
        cmd.analogMove = false;
    }

    // A pending rebind capture swallows everything, so the capturing press
    // cannot also fire gameplay.
    if (m_capturePending)
    {
        held = 0;
        cmd.moveX = cmd.moveY = cmd.moveMag = 0.f;
        cmd.analogMove = false;
    }

    cmd.held     = held;
    cmd.pressed  = held & ~m_heldPrev;
    cmd.released = ~held &  m_heldPrev;
    m_heldPrev   = held;

    m_cmd = cmd;
    InputProfiles::Frame frame;
    frame.keys=m_curr;
    if(padUsable) frame.pad=pad;
    frame.mouseX=cmd.lookMouseDX; frame.mouseY=cmd.lookMouseDY;
    frame.allowed=focused&&in.simAdvancing&&!m_capturePending;
    for(int i=0;i<m_activeCount;++i) if(m_contexts[m_active[i]].blockLower) frame.allowed=false;
    auto& profiles=InputProfiles::Registry::Get();
    profiles.Evaluate(frame);
    profiles.Bridge(m_cmd);
}

// Prefers the last-touched device (lastDevice), so a HUD hint follows the player.
const char* Input::GlyphToken(GameAction a) const noexcept
{
    const int idx = static_cast<int>(a);
    if (idx < 0 || idx >= kGameActionCount)
        return nullptr;

    // Resolve through the context stack, not the base table: a HUD hint must name the
    // control live right now (with a menu context pushed, "press B to cancel" must say B
    // even though the base table has B on Dodge).
    bool blocked = false;
    const ActionBinding& ab = ResolveAction(a, blocked);
    if (blocked) return nullptr;        // swallowed by a blocking context

    static thread_local char t_buf[48];

    const bool preferPad = (m_cmd.lastDevice == InputDeviceKind::Gamepad);

    if (preferPad && ab.pad)
    {
        if (FormatBindingPath(true, ab.pad, t_buf, sizeof(t_buf)))
            return t_buf;
    }
    if (ab.vk[0])
    {
        if (FormatBindingPath(false, ab.vk[0], t_buf, sizeof(t_buf)))
            return t_buf;
    }
    // Fall back to the other device rather than showing nothing: a pad user
    // whose action is keyboard-only still gets a readable hint.
    if (ab.pad)
    {
        if (FormatBindingPath(true, ab.pad, t_buf, sizeof(t_buf)))
            return t_buf;
    }
    return nullptr;
}
