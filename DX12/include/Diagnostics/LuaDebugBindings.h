#pragma once
// Debug.* - the stat-unit HUD's control + readout surface from Lua. Debug.SetDraw (the editor's
// debug-draw toggles) is registered here as a stub, which the editor replaces in the same table: it
// is the one function that differs per exe.

namespace sol { class state; }
class Renderer;
class GraphicsDX12;
namespace UI { class DebugHud; }
namespace Audio { class AudioEngine; class AudioClipSystem; }

namespace Diag
{
    // Registers the global `Debug` table. The parameters are App's members, named as the
    // closures read them; every one outlives the Lua VM (App declares the VM last).
    void RegisterLuaDebugBindings(sol::state& state, Renderer& renderer, UI::DebugHud& debugHud,
                                  Audio::AudioEngine& audioEngine,
                                  Audio::AudioClipSystem& audioClipSys, GraphicsDX12& gfx);
}
