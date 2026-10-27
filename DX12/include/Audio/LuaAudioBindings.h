#pragma once

// Lua bindings for the audio system. The script layer touches only ECS + the event bus, never
// AudioEngine: one-shots and bus / param changes publish events; entity-targeted play / stop /
// pause set the AudioSourceComponent transport flags that AudioSystem reconciles.

namespace sol { class state; }
class World;

namespace Audio
{
    // Registers the global `Audio` table. Captures @p world so entity-targeted
    // helpers can reach AudioSourceComponent; one-shots / bus / param go through
    // the EventBus singleton.
    void RegisterLuaAudioBindings(sol::state& lua, World& world);
}
