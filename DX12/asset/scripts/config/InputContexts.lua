-- InputContexts.lua — starting template for mapping contexts.
--
-- Loaded ONCE at ScriptSystem init (src/Scripting/ScriptSystem.cpp,
-- RegisterBindings), AFTER Keys.lua / Pad.lua and after the Input, Keys, Pad and
-- Action tables exist. Non-fatal: delete this file and the engine falls back to
-- the base InputBindings table with no contexts at all.
--
-- WHAT THIS SOLVES: "B cancels in a menu but dodges in combat" — without
-- teaching the dodge code anything about menus. A context changes WHICH CONTROL
-- fires an action; the action vocabulary never changes, so nothing below
-- ActionState can tell a context is active.
--
-- USAGE from a Lua UI script:
--
--     Input.PushContext("Menu")   -- on menu open
--     Input.PopContext("Menu")    -- on menu close
--
-- Push and pop MUST be paired. Pushing an already-active context re-seats it
-- instead of stacking a duplicate, and popping one that is not active is a
-- harmless false — but a menu that pushes on open and forgets to pop on close
-- leaves the pad in menu mode. (World teardown calls Input.ClearContexts() from
-- C++ so an unloaded scene cannot silence the pad forever, but that is a safety
-- net, not the contract.)
--
-- Resolution, per action, highest priority first:
--     context DEFINES the action  -> its binding wins, stop
--     else context has blockLower -> the action is UNBOUND, stop
--     else                        -> keep walking; nothing = base bindings
--
-- blockLower = true means EVERY gameplay action goes quiet while the menu is up.
-- Not a side effect — the point. A modal menu should not have to enumerate the
-- forty actions it wants to suppress, and a stick nudge should not walk the
-- player off a ledge behind the pause screen.
--
-- Two ways to say "no":
--     Dodge = {}      -- key PRESENT, empty table: explicitly UNBOUND here.
--                        Lower contexts never get a look at it.
--     (key absent)    -- not this context's business: falls through to the next
--                        context down, and finally to the base bindings.

-- ── Gameplay (priority 0) ─────────────────────────────────────────────────
-- The always-on layer. No blockLower: anything it does not name (Jump, Sprint,
-- move, look) falls straight through to the base InputBindings table.
Input.DefineContext{
    name     = "Gameplay",
    priority = 0,
    actions  = {
        -- Use the player's base bindings; overrides here would defeat rebinding.
    },
}

-- ── Menu (priority 100, blocks everything below) ──────────────────────────
-- Same physical B button, different action — and while this is pushed, Dodge
-- (and every other gameplay action) resolves to UNBOUND because of blockLower.
Input.DefineContext{
    name       = "Menu",
    priority   = 100,
    blockLower = true,
    actions    = {
        UICancel  = { pad = Pad.B, vk1 = Keys.Escape },
        UIConfirm = { pad = Pad.A, vk1 = Keys.Enter },
        -- MenuToggle MUST be restated here even though the base table already
        -- binds it. blockLower kills fall-through, so leaving it out would mute
        -- the one button that gets the player back out of the menu.
        MenuToggle = { pad = Pad.Start, vk1 = Keys.Escape },
    },
}

-- ── UIScreen (priority 200) ───────────────────────────────────────────────
-- For a MODAL, SELF-CLOSING screen — the settings menu, a confirmation dialog —
-- as opposed to the "Menu" layer above, which is for an overlay that the host
-- transport is still allowed to toggle.
--
-- The one thing this adds is `MenuToggle = {}`: the key PRESENT with an EMPTY
-- table, i.e. explicitly UNBOUND here rather than left to fall through. That
-- matters more than it looks:
--
--   In a shipped Game.exe, Escape / pad-Start reaches App.cpp, which hands it to the
--   game flow (GameFlow's 'pause' trigger, else its pause layer: GameFlow.Pause).
--   With MenuToggle left bound, the key that closes a modal screen (UICancel is
--   Escape too) would also toggle the game's pause underneath it: the screen
--   closes and the game stays paused, or the other way round. The screen owns
--   Escape while it is open.
--
--   MenuToggle is a HOST action and is therefore exempt from blockLower, so
--   blockLower alone does NOT save you — the exemption is specifically so a modal
--   can never swallow the button that escapes it. Redefining the action to
--   nothing is the sanctioned override (Input::ResolveAction honours an explicit
--   redefinition of a host action, including to nothing).
--
-- The screen closes itself on UICancel, which is a normal action and unaffected.
-- THE CONTRACT: whoever pushes this MUST pop it. An unpopped push leaves Escape
-- doing nothing for the session. C++ calls Input.ClearContexts() on world
-- teardown as a safety net, not as a substitute.
Input.DefineContext{
    name       = "UIScreen",
    priority   = 200,
    blockLower = true,
    actions    = {
        UICancel   = { pad = Pad.B, vk1 = Keys.Escape },
        UIConfirm  = { pad = Pad.A, vk1 = Keys.Enter },
        MenuToggle = {},
    },
}
