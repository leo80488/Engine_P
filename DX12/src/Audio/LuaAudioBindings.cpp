#include "Audio/LuaAudioBindings.h"

#include "Audio/AudioComponents.h"
#include "Audio/AudioEvents.h"
#include "Audio/AudioTypes.h"
#include "ECS/ECS.h"
#include "System/EventBus.h"
#include "System/Log.h"
#include "Resource/EngineSettings.h" // Resource::LiveBusVolume: the only bus getter

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include <algorithm>
#include <string>

namespace Audio
{
namespace
{
    // Map a Lua bus name to BusType (lower case or capitalized); anything unrecognized falls back to
    // SFX, the safest default channel.
    BusType BusFromString(const std::string& s)
    {
        if (s == "master" || s == "Master")   return BusType::Master;
        if (s == "music"  || s == "Music")    return BusType::Music;
        if (s == "voice"  || s == "Voice")    return BusType::Voice;
        if (s == "ambient"|| s == "Ambient")  return BusType::Ambient;
        if (s == "ui"     || s == "UI" || s == "Ui") return BusType::UI;
        return BusType::SFX;
    }
}

void RegisterLuaAudioBindings(sol::state& lua, World& world)
{
    sol::table tbl = lua.create_named_table("Audio");

    // Audio.playOneShot{ clip=, volume=, pitch=, bus=, loop=, is3D=, x=,y=,z=, minDistance=,
    // maxDistance= }: fire-and-forget (no entity). Publishes a PlaySoundEvent; AudioSystem owns and
    // reclaims the voice. x / y / z are carried but not spatialized yet (see PlayParams::position).
    tbl.set_function("playOneShot",
        [](sol::table opts)
        {
            PlaySoundEvent e;
            e.clipPath = opts.get_or<std::string>("clip", "");
            if (e.clipPath.empty()) return;
            e.bus         = BusFromString(opts.get_or<std::string>("bus", "sfx"));
            e.volume      = opts.get_or("volume", 1.0f);
            e.pitch       = opts.get_or("pitch",  1.0f);
            e.loop        = opts.get_or("loop", false);
            e.is3D        = opts.get_or("is3D", false);
            e.position    = { opts.get_or("x", 0.0f),
                              opts.get_or("y", 0.0f),
                              opts.get_or("z", 0.0f) };
            e.minDistance = opts.get_or("minDistance", 1.0f);
            e.maxDistance = opts.get_or("maxDistance", 50.0f);
            EventBus::Get().Publish(e);
        });

#ifdef WITH_HARNESS
    // Audio.debugPlaySoundEvent(entity, { clip=, volume=, pitch=, bus=, loop=, is3D= }) -> bool,
    // harness builds only (Retail has no such field). Publishes a PlaySoundEvent addressed to `entity`
    // through its live handle: the per-entity half of AudioSystem::OnPlaySound, which no shipped caller
    // reaches (AudioPlaySystem stamps generation 0, which IsHandleValid never matches, so its plays run
    // fire-and-forget). The positive control for that half's clip references
    // (tests/unit/test_component_writes.py). false = a dead entity or no clip; nothing is published.
    tbl.set_function("debugPlaySoundEvent",
        [&world](LuaEntity entity, sol::table opts) -> bool
        {
            const Entity target = LuaId::ResolveLive(world, entity);
            PlaySoundEvent e;
            e.clipPath = opts.get_or<std::string>("clip", "");
            if (target == NullEntity || e.clipPath.empty()) return false;
            e.entity = world.MakeHandle(target);
            e.bus    = BusFromString(opts.get_or<std::string>("bus", "sfx"));
            e.volume = opts.get_or("volume", 1.0f);
            e.pitch  = opts.get_or("pitch",  1.0f);
            e.loop   = opts.get_or("loop", false);
            e.is3D   = opts.get_or("is3D", false);
            EventBus::Get().Publish(e);
            return true;
        });
#endif

    // Audio.play(entity): (re)start the entity's AudioSource from the top.
    tbl.set_function("play",
        [&world](LuaEntity entity) -> bool
        {
            auto* a = world.GetComponent<AudioSourceComponent>(LuaId::ResolveLive(world, entity));
            if (!a) return false;
            a->playRequested = true;
            a->paused        = false;
            return true;
        });

    // Audio.stop(entity): stop and release the entity's active voice.
    tbl.set_function("stop",
        [&world](LuaEntity entity) -> bool
        {
            auto* a = world.GetComponent<AudioSourceComponent>(LuaId::ResolveLive(world, entity));
            if (!a) return false;
            a->stopRequested = true;
            return true;
        });

    // Audio.pause(entity, paused?): freeze / resume the entity's voice in place (paused defaults to
    // true).
    tbl.set_function("pause",
        [&world](LuaEntity entity, sol::optional<bool> paused) -> bool
        {
            auto* a = world.GetComponent<AudioSourceComponent>(LuaId::ResolveLive(world, entity));
            if (!a) return false;
            a->paused = paused.value_or(true);
            return true;
        });

    // Audio.setBusVolume("music", 0.3): mixer control (linear gain, 1.0 = unity).
    // Clamped here as in EngineSettings::ApplyTo: AudioEngine::SetBusVolume passes the float straight to
    // IXAudio2Voice::SetVolume, where a negative gain phase-inverts, a large one clips the mastering
    // voice and NaN reaches XAudio2. The same [0, 2] as ApplyTo (and Load's sanitize) keeps a menu
    // value unchanged across save / load.
    tbl.set_function("setBusVolume",
        [](const std::string& bus, float volume)
        {
            if (!(volume == volume))   // NaN: std::clamp would pass it through
            {
                LOG_WARNING("Audio.setBusVolume('%s'): refused NaN", bus.c_str());
                return;
            }
            BusVolumeChangedEvent e;
            e.bus    = BusFromString(bus);
            e.volume = std::clamp(volume, 0.0f, 2.0f);
            EventBus::Get().Publish(e);
        });

    // Audio.getBusVolume("music") -> number: the value last published for that bus by anyone (boot
    // settings, this table, an options menu). Reads the EngineSettings mirror because AudioEngine keeps
    // no copy (the live value lives only inside XAudio2); a volume slider needs it to initialise.
    tbl.set_function("getBusVolume",
        [](const std::string& bus) -> float
        {
            return Resource::LiveBusVolume(BusFromString(bus));
        });

    // Audio.setVolume(entity, v) / Audio.setPitch(entity, p): set the component value for the next
    // play and, through a SetAudioParamEvent on the active voice handle, the live voice.
    tbl.set_function("setVolume",
        [&world](LuaEntity entity, float v)
        {
            auto* a = world.GetComponent<AudioSourceComponent>(LuaId::ResolveLive(world, entity));
            if (!a) return;
            a->volume = v;
            if (a->activeVoice.IsValid())
            {
                SetAudioParamEvent e;
                e.voice = a->activeVoice;
                e.param = SetAudioParamEvent::Volume;
                e.value = v;
                EventBus::Get().Publish(e);
            }
        });

    tbl.set_function("setPitch",
        [&world](LuaEntity entity, float p)
        {
            auto* a = world.GetComponent<AudioSourceComponent>(LuaId::ResolveLive(world, entity));
            if (!a) return;
            a->pitch = p;
            if (a->activeVoice.IsValid())
            {
                SetAudioParamEvent e;
                e.voice = a->activeVoice;
                e.param = SetAudioParamEvent::Pitch;
                e.value = p;
                EventBus::Get().Publish(e);
            }
        });
}

} // namespace Audio
