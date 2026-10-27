-- Pad.lua — global gamepad button-mask map.
--
-- Loaded ONCE at ScriptSystem init (src/Scripting/ScriptSystem.cpp,
-- RegisterBindings) into the shared Lua state, so the resulting `Pad` table is a
-- GLOBAL every script can use directly — no require/import needed:
--
--     if Input.PadDown(Pad.A)                      then ... end
--     Input.SetBinding(Action.Interact, { pad = Pad.X })
--
-- Values are OUR OWN bit vocabulary (PadBtn::Mask in include/Input/InputTypes.h),
-- deliberately NOT XInput's. A GameInput / DualSense / Raw-HID backend maps onto
-- these same bits, so a mask written into a script here — or saved into
-- settings.json as a binding — keeps meaning the same physical button after a
-- backend swap.
--
-- MERGES onto whatever `Pad` the engine already set in C++, exactly like
-- Keys.lua does for `Keys`. Two consequences worth knowing:
--
--   * The ENGINE value always wins. Each primary below is written
--     `Pad.A = Pad.A or (1 << 0)` — the literal is a readable fallback so this
--     file doubles as the designer-facing button reference, but it can never
--     silently disagree with InputTypes.h. Aliases then just point at whatever
--     the primary resolved to, so there is exactly one number per button.
--   * Add ALIASES here freely and re-pack (tools/package_game.py) — no C++
--     rebuild. Adding a whole new BUTTON is a PadBtn::Mask edit, not a Lua one.
--
-- Masks are single bits, so a chord is just an OR:
--     Input.PadDown(Pad.LB | Pad.RB)

local Pad = rawget(_G, "Pad") or {}

-- ── Face buttons ──────────────────────────────────────────────────────────
Pad.A = Pad.A or (1 << 0)
Pad.B = Pad.B or (1 << 1)
Pad.X = Pad.X or (1 << 2)
Pad.Y = Pad.Y or (1 << 3)

-- ── Shoulders ─────────────────────────────────────────────────────────────
Pad.LB = Pad.LB or (1 << 4)
Pad.RB = Pad.RB or (1 << 5)

-- ── System buttons ────────────────────────────────────────────────────────
-- Start drives Action.MenuToggle by default, which is the one action EXEMPT
-- from the sim-advancing gate: it must keep working while the game is paused,
-- because otherwise nothing could ever un-pause it.
Pad.Back  = Pad.Back  or (1 << 6)
Pad.Start = Pad.Start or (1 << 7)

-- ── Stick clicks ──────────────────────────────────────────────────────────
Pad.LS = Pad.LS or (1 << 8)
Pad.RS = Pad.RS or (1 << 9)

-- ── D-pad ─────────────────────────────────────────────────────────────────
Pad.DUp    = Pad.DUp    or (1 << 10)
Pad.DDown  = Pad.DDown  or (1 << 11)
Pad.DLeft  = Pad.DLeft  or (1 << 12)
Pad.DRight = Pad.DRight or (1 << 13)

-- ── Triggers, as DIGITAL bits ─────────────────────────────────────────────
-- Synthesized once the analog trigger crosses InputBindings::triggerThreshold.
-- For the 0..1 value use Input.Trigger(1) / Input.Trigger(2) — Pad.LT is a
-- boolean bit and structurally cannot express "half pulled".
Pad.LT = Pad.LT or (1 << 14)
Pad.RT = Pad.RT or (1 << 15)

-- ── Aliases ───────────────────────────────────────────────────────────────
-- Same bits under other vendors' names, so a PlayStation- or Nintendo-facing
-- project reads naturally and nothing has to translate at the binding layer.
Pad.Cross    = Pad.A
Pad.Circle   = Pad.B
Pad.Square   = Pad.X
Pad.Triangle = Pad.Y

Pad.L1 = Pad.LB
Pad.R1 = Pad.RB
Pad.L2 = Pad.LT
Pad.R2 = Pad.RT
Pad.L3 = Pad.LS
Pad.R3 = Pad.RS

Pad.LeftShoulder  = Pad.LB
Pad.RightShoulder = Pad.RB
Pad.LeftTrigger   = Pad.LT
Pad.RightTrigger  = Pad.RT
Pad.LeftStick     = Pad.LS
Pad.RightStick    = Pad.RS

Pad.Up    = Pad.DUp
Pad.Down  = Pad.DDown
Pad.Left  = Pad.DLeft
Pad.Right = Pad.DRight

Pad.Select  = Pad.Back    -- PS / generic
Pad["View"] = Pad.Back    -- current Xbox naming
Pad.Options = Pad.Start
Pad.Menu    = Pad.Start

_G.Pad = Pad
return Pad
