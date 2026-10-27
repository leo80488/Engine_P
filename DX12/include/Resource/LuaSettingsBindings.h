#pragma once

// LuaSettingsBindings - the `Settings.*` Lua table: a player-settings surface an
// in-game options menu is written against with no new C++ per field.
//
// Path-keyed, not one function per setting: a setting is one row of
// EngineSettingsSchema.inl (Settings/SettingsRows.h), and settings.json's load /
// save / apply / capture walk the same rows. One binding per field would be another
// list to keep in step, so this table walks the rows too: a new row brings its
// script surface with it (as PostProcess.setEngineDefault / getResolved do over
// PostProcessProperties.inl).
//
// PENDING / APPLIED: editor widgets write the live device immediately, and
// EngineSettings has no Defaults(), operator==, dirty flag or revert. A menu needs
// all four, so the bridge owns two copies:
//     pending - what the menu is editing; Settings.get / set see only this.
//     applied - the last state pushed to the engine; Settings.revert restores
//               from it, Settings.dirty compares against it.
// Nothing reaches the engine until Settings.apply(), so Apply / Revert / Reset are
// honest and a risky change (HDR mode, Frame Generation) has no half-applied state.
//
// LUA SURFACE
//   Settings.get(path)            -> boolean | number | string | nil
//   Settings.set(path, value)     -> boolean            (writes `pending` only)
//   Settings.options(path)        -> { ... } | nil      labels (enum) / values (choice)
//   Settings.range(path)          -> lo, hi | nil
//   Settings.meta(path)           -> { path=, kind=, restart=, group=, name=, title=, tip= } | nil
//   Settings.list([groupPrefix])  -> { path, ... }      declaration order
//   Settings.dirty()              -> boolean            pending ~= applied
//   Settings.apply()              -> boolean            pending -> engine
//   Settings.revert()             -> boolean            applied -> pending -> engine
//   Settings.reset()              -> boolean            defaults -> pending (NOT applied)
//   Settings.save()               -> boolean            read-modify-write settings.json
//   Settings.reload()             -> boolean            settings.json -> pending
//   Settings.caps()               -> table              hardware capability snapshot
//   Settings.path()               -> string             absolute settings.json path
//   Settings.where(path)          -> table | nil        who set it: per layer, history
//   Settings.setRuntime(path, v)  -> boolean            Runtime layer: applied NOW, never saved
//   Settings.clearRuntime(path)   -> boolean            drop it; the other layers' value applies
// plus the presets (setLightingTier / lightingTier, scalability / setScalability)
// and the developer helpers (saveDeveloper / reloadDeveloper, setRestir* / setPT*),
// documented at their bindings.
//
// THE RUNTIME LAYER: setRuntime is a developer's live change outside the pending /
// applied model - it applies at once, only that setting (Settings::ApplyPlayerRow),
// is never saved, and the bridge's pending and applied copies follow it so a later
// apply keeps it. The pipe's `setting` verb (ServeSettingVerb) is the same writer.
//
// Settings.save() is a READ-MODIFY-WRITE that never touches the `input` block;
// Input.SaveBindings owns that half and is the mirror image. Between them, an
// "apply graphics" button cannot clobber a rebind and an "apply controls" button
// cannot clobber graphics.
//
// NOT COVERED: input bindings and tuning (Input.*), engine.json developer config
// (not player-facing, sealed into the pak; only where / setRuntime / clearRuntime
// and the developer helpers reach it), and the live renderer knobs with no settings
// row at all, such as FSR sharpening (they cannot persist). The .inl's tail gives
// the reasons.

#include <sol/forward.hpp>

#include <string>

class Renderer;
class IGraphicsDevice;
namespace Plat { class Window; }

namespace Resource
{
    // Registers the `Settings` table. Call from App::InitLuaBindings, after
    // ScriptSystem has created the Lua state and after the boot EngineSettings::ApplyTo:
    // the bridge seeds `applied` from the store and CaptureFrom, so the engine must
    // already hold the player's real values or the first Settings.revert() reverts to
    // the struct defaults.
    //
    // @p window may be null (headless / capture hosts): fullscreen then reads back
    // false and applying it is skipped, as in ApplyTo.
    //
    // Lifetime: the bridge keeps these references for the process, like every other
    // Register*Bindings; all three outlive the Lua state, which App destroys with the
    // ScriptSystem.
    void RegisterLuaSettingsBindings(sol::state& lua,
                                     Renderer& renderer,
                                     IGraphicsDevice& gfx,
                                     Plat::Window* window);

    // The control channel's `setting` verb (main thread), JSON in one line:
    //   setting list [<prefix>]        every row: path, domain, value, text, source, policy, restart
    //   setting get <path>             one row, the same fields
    //   setting where <path>           + every layer's value, the revisions, the last writes
    //   setting set <path> <value>     a Runtime-layer write, applied at once (Settings.setRuntime)
    //   setting clear <path>           drop the Runtime value (Settings.clearRuntime)
    // A path matches any case; a value is text, read as -set:<path>=<value> is
    // (Settings::RequestFromText). Answers ok:false with an error for an
    // unknown path, a refused value, or before the bridge is registered.
    std::string ServeSettingVerb(const std::string& payload);
}
