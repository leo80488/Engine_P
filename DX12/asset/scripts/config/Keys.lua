-- Keys.lua — global Windows Virtual-Key (VK) code map.
--
-- Loaded ONCE at ScriptSystem init (src/Scripting/ScriptSystem.cpp,
-- RegisterBindings) into the shared Lua state, so the resulting `Keys` table is
-- a GLOBAL every script can use directly — no require/import needed:
--
--     if Input.WasKeyPressed(Keys.Enter) then ... end
--     if Input.IsKeyDown(Keys.Space)     then ... end
--     Engine ... (any Logic/System/Service/scene script sees `Keys`)
--
-- Values are raw Win32 VK codes (the integer Input.* expects). This file is the
-- single source of truth: add/rename keys here and re-pack (tools/package_game.py)
-- — no C++ rebuild required. It MERGES onto whatever `Keys` the engine already
-- set in C++, so the built-in subset (W/A/S/D/Space/…) stays valid even if this
-- file is missing.
--
-- Ref: learn.microsoft.com/windows/win32/inputdev/virtual-key-codes

local Keys = rawget(_G, "Keys") or {}

-- ── Mouse buttons (VK_*BUTTON) ────────────────────────────────────────────
Keys.LButton  = 0x01
Keys.RButton  = 0x02
Keys.MButton  = 0x04
Keys.XButton1 = 0x05
Keys.XButton2 = 0x06

-- ── Editing / control ─────────────────────────────────────────────────────
Keys.Backspace = 0x08
Keys.Tab       = 0x09
Keys.Enter     = 0x0D
Keys.Return    = 0x0D   -- alias
Keys.Pause     = 0x13
Keys.CapsLock  = 0x14
Keys.Escape    = 0x1B
Keys.Esc       = 0x1B   -- alias
Keys.Space     = 0x20
Keys.PageUp    = 0x21
Keys.PgUp      = 0x21   -- alias
Keys.PageDown  = 0x22
Keys.PgDn      = 0x22   -- alias
Keys["End"]    = 0x23   -- reserved word → bracket form
Keys.Home      = 0x24
Keys.Insert    = 0x2D
Keys.Ins       = 0x2D   -- alias
Keys.Delete    = 0x2E
Keys.Del       = 0x2E   -- alias
Keys.PrintScreen = 0x2C

-- ── Arrows ────────────────────────────────────────────────────────────────
Keys.Left  = 0x25
Keys.Up    = 0x26
Keys.Right = 0x27
Keys.Down  = 0x28

-- ── Modifiers (generic + side-specific) ───────────────────────────────────
Keys.Shift   = 0x10
Keys.Ctrl    = 0x11
Keys.Control = 0x11   -- alias
Keys.Alt     = 0x12
Keys.Menu    = 0x12   -- alias (Win32 name for Alt)
Keys.LShift  = 0xA0
Keys.RShift  = 0xA1
Keys.LCtrl   = 0xA2
Keys.RCtrl   = 0xA3
Keys.LAlt    = 0xA4
Keys.RAlt    = 0xA5
Keys.LWin    = 0x5B
Keys.RWin    = 0x5C

-- ── Number row (VK 0x30..0x39) ────────────────────────────────────────────
Keys.Num0 = 0x30
Keys.Num1 = 0x31
Keys.Num2 = 0x32
Keys.Num3 = 0x33
Keys.Num4 = 0x34
Keys.Num5 = 0x35
Keys.Num6 = 0x36
Keys.Num7 = 0x37
Keys.Num8 = 0x38
Keys.Num9 = 0x39

-- ── Letters (VK == ASCII uppercase, 0x41..0x5A) ───────────────────────────
for i = 0, 25 do
    Keys[string.char(65 + i)] = 0x41 + i   -- Keys.A .. Keys.Z
end

-- ── Numpad ────────────────────────────────────────────────────────────────
Keys.Numpad0  = 0x60
Keys.Numpad1  = 0x61
Keys.Numpad2  = 0x62
Keys.Numpad3  = 0x63
Keys.Numpad4  = 0x64
Keys.Numpad5  = 0x65
Keys.Numpad6  = 0x66
Keys.Numpad7  = 0x67
Keys.Numpad8  = 0x68
Keys.Numpad9  = 0x69
Keys.Multiply = 0x6A
Keys.Add      = 0x6B
Keys.Subtract = 0x6D
Keys.Decimal  = 0x6E
Keys.Divide   = 0x6F
Keys.NumLock  = 0x90

-- ── Function keys (VK_F1 0x70 .. VK_F24 0x87) ─────────────────────────────
for i = 1, 24 do
    Keys["F" .. i] = 0x6F + i
end

-- ── OEM punctuation (US layout) ───────────────────────────────────────────
Keys.Semicolon    = 0xBA   -- ; :
Keys.Equals       = 0xBB   -- = +
Keys.Plus         = 0xBB   -- alias
Keys.Comma        = 0xBC   -- , <
Keys.Minus        = 0xBD   -- - _
Keys.Period       = 0xBE   -- . >
Keys.Slash        = 0xBF   -- / ?
Keys.Backtick     = 0xC0   -- ` ~
Keys.Tilde        = 0xC0   -- alias
Keys.Grave        = 0xC0   -- alias
Keys.LBracket     = 0xDB   -- [ {
Keys.Backslash    = 0xDC   -- \ |
Keys.RBracket     = 0xDD   -- ] }
Keys.Quote        = 0xDE   -- ' "
Keys.Apostrophe   = 0xDE   -- alias

_G.Keys = Keys
return Keys
