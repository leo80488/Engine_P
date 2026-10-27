#include "Audio/AudioSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioClipSystem.h"
#include "Audio/AudioClipResource.h"
#include "Audio/AudioComponents.h"
#include "Audio/AudioEvents.h"
#include "System/EventBus.h"
#include "System/Log.h"

namespace Audio
{

AudioSystem::AudioSystem()  = default;
AudioSystem::~AudioSystem() { Unbind(); }

void AudioSystem::BindClipSystem(AudioClipSystem& clipSystem)
{
    m_clipSystem = &clipSystem;
}

void AudioSystem::BindEngine(AudioEngine& engine)
{
    m_engine = &engine;

    EventBus& bus = EventBus::Get();

    auto play = bus.Subscribe<PlaySoundEvent>(
        [this](const PlaySoundEvent& e) { OnPlaySound(e); });
    m_unsubscribers.emplace_back([play]{ EventBus::Get().Unsubscribe<PlaySoundEvent>(play); });

    auto stop = bus.Subscribe<StopSoundEvent>(
        [this](const StopSoundEvent& e) { OnStopSound(e); });
    m_unsubscribers.emplace_back([stop]{ EventBus::Get().Unsubscribe<StopSoundEvent>(stop); });

    auto sp = bus.Subscribe<SetAudioParamEvent>(
        [this](const SetAudioParamEvent& e) { OnSetParam(e); });
    m_unsubscribers.emplace_back([sp]{ EventBus::Get().Unsubscribe<SetAudioParamEvent>(sp); });

    auto bv = bus.Subscribe<BusVolumeChangedEvent>(
        [this](const BusVolumeChangedEvent& e) { OnBusVolume(e); });
    m_unsubscribers.emplace_back([bv]{ EventBus::Get().Unsubscribe<BusVolumeChangedEvent>(bv); });
}

void AudioSystem::BindWorld(World& world)
{
    m_world = &world;
    m_destroyListenerId = world.AddEntityDestroyListener(
        [this](Entity e) { OnEntityDestroyed(e); });
}

void AudioSystem::Unbind()
{
    for (auto& fn : m_unsubscribers) fn();
    m_unsubscribers.clear();

    if (m_world && m_destroyListenerId != 0)
    {
        m_world->RemoveEntityDestroyListener(m_destroyListenerId);
        m_destroyListenerId = 0;
    }
    m_world      = nullptr;
    m_engine     = nullptr;
    m_clipSystem = nullptr;
}

void AudioSystem::Update(World& world, float /*dt*/)
{
    if (!m_engine || !m_clipSystem) return;

    // Drain finished fire-and-forget refcounts. Done before the per-source
    // pass so a clip that finished this frame is freed for reuse.
    {
        size_t w = 0;
        for (size_t r = 0; r < m_pendingReleases.size(); ++r)
        {
            const PendingRelease& pr = m_pendingReleases[r];
            if (m_engine->IsVoicePlaying(pr.voice))
                m_pendingReleases[w++] = pr;
            else
                m_clipSystem->ReleaseClip(pr.handle);
        }
        m_pendingReleases.resize(w);
    }

    // Iterate the pool's Data() directly (the fast path), not GetEntities + GetComponent.
    auto* pool = world.GetPool<AudioSourceComponent>();
    if (!pool) return;
    auto& data = pool->Data();
    const size_t n = data.size();

    for (size_t i = 0; i < n; ++i)
    {
        AudioSourceComponent& src = data[i];

        // 1. Lazy clip acquisition: the first Update that sees a clipPath without a handle starts the
        //    async load. A clipPath that no longer names the bound clip (an inspector edit, a serializer
        //    rewrite carrying runtime state) swaps it: stop the voice on the old clip before releasing that
        //    clip (the voice reads its PCM), then acquire.
        if (src.clipHandle.IsValid() && src.clipPath != src.boundClipPath)
        {
            if (src.activeVoice.IsValid()) m_engine->StopVoice(src.activeVoice, 0.f);
            src.activeVoice = {};
            src.state       = PlayState::Stopped;
            src.paused      = false;
            m_clipSystem->ReleaseClip(src.clipHandle);
            src.clipHandle  = Resource::kInvalidAudioHandle;
        }
        if (!src.clipHandle.IsValid() && !src.clipPath.empty())
        {
            src.clipHandle    = m_clipSystem->AcquireClip(src.clipPath);
            src.boundClipPath = src.clipPath;
        }

        // 2. Stop request (inspector / Lua / MotionDirector): release the active
        //    voice now and cancel any queued play.
        if (src.stopRequested)
        {
            src.stopRequested = false;
            src.playRequested = false;
            if (src.activeVoice.IsValid()) m_engine->StopVoice(src.activeVoice, 0.f);
            src.activeVoice = {};
            src.state       = PlayState::Stopped;
            src.paused      = false;
        }

        // 3. Sync runtime state from the engine: a finished one-shot voice flips
        //    the component back to Stopped. A paused voice still reports
        //    IsVoicePlaying() == true, so a deliberate pause is not clobbered.
        if (src.activeVoice.IsValid() && !m_engine->IsVoicePlaying(src.activeVoice))
        {
            src.activeVoice = {};
            src.state       = PlayState::Stopped;
        }

        // 4. (Re)start request: a Play while already sounding restarts from the
        //    top, so stop the current voice first; the start path below plays a
        //    fresh one this frame.
        if (src.playRequested && src.activeVoice.IsValid())
        {
            m_engine->StopVoice(src.activeVoice, 0.f);
            src.activeVoice = {};
            src.state       = PlayState::Stopped;
        }

        // 5. Start a voice for an explicit playRequested OR a playOnEnable. Both
        //    wait for the clip to finish loading (a frame-1 component on a slow
        //    disk still plays once ready); the trigger is consumed once the voice
        //    starts OR the load is known-failed, so a permanent failure does not
        //    re-pump every frame.
        const bool wantStart = src.playRequested
            || (src.playOnEnable && !src.autoPlayFired);
        if (wantStart && !src.activeVoice.IsValid()
            && src.clipHandle.IsValid() && m_clipSystem->IsReady(src.clipHandle))
        {
            const bool started = StartSourceVoice(src);
            src.playRequested = false;
            // playOnEnable is serialized config: the one-shot is guarded by the runtime autoPlayFired flag
            // instead, so saving during Play keeps the authored value.
            if (src.playOnEnable) src.autoPlayFired = true;
            if (started) src.paused = false;   // a fresh play is never born paused
        }

        // 6. Pause reconcile: apply the src.paused level (set by the inspector / a MotionDirector
        //    transport pause) to the live voice.
        if (src.activeVoice.IsValid())
        {
            if (src.paused && src.state == PlayState::Playing)
            {
                m_engine->PauseVoice(src.activeVoice);
                src.state = PlayState::Paused;
            }
            else if (!src.paused && src.state == PlayState::Paused)
            {
                m_engine->ResumeVoice(src.activeVoice);
                src.state = PlayState::Playing;
            }
        }
    }
}

bool AudioSystem::StartSourceVoice(AudioSourceComponent& src)
{
    const AudioClipResource* clip = m_clipSystem->GetResource(src.clipHandle);
    if (!clip) return false;

    PlayParams pp;
    pp.bus         = src.bus;
    pp.volume      = src.volume;
    pp.pitch       = src.pitch;
    pp.loop        = src.loop;
    pp.is3D        = src.is3D;
    pp.minDistance = src.minDistance;
    pp.maxDistance = src.maxDistance;
    pp.curve       = src.curve;
    // One-shot: consumed with the play edge so an ordinary Play never inherits
    // a scrub offset from a previous cinematic seek.
    pp.startOffsetSec      = src.playStartOffsetSec;
    src.playStartOffsetSec = 0.f;

    const VoiceHandle h = m_engine->PlayClip(clip, pp);
    if (!h.IsValid()) return false;
    src.activeVoice = h;
    src.state       = PlayState::Playing;
    return true;
}

void AudioSystem::OnWorldClear()
{
    // Stop and drop the fire-and-forget voices. Stop each voice before releasing its clip ref: the
    // release frees the PCM XAudio2 is still reading, and an unstopped one-shot would bleed into the
    // next scene.
    for (const PendingRelease& pr : m_pendingReleases)
    {
        if (m_engine)     m_engine->StopVoice(pr.voice, 0.f);
        if (m_clipSystem) m_clipSystem->ReleaseClip(pr.handle);
    }
    m_pendingReleases.clear();

    // Release each live source's clip ref + stop its voice. Resetting clipHandle
    // to invalid keeps this idempotent against a subsequent OnEntityDestroyed.
    if (!m_world) return;
    auto* pool = m_world->GetPool<AudioSourceComponent>();
    if (!pool) return;
    for (AudioSourceComponent& src : pool->Data())
    {
        if (m_engine && src.activeVoice.IsValid())
            m_engine->StopVoice(src.activeVoice, 0.f);
        if (m_clipSystem && src.clipHandle.IsValid())
            m_clipSystem->ReleaseClip(src.clipHandle);
        src.clipHandle  = Resource::kInvalidAudioHandle;
        src.boundClipPath.clear();
        src.activeVoice = {};
        src.state       = PlayState::Stopped;
    }
}

void AudioSystem::AdoptClip(AudioSourceComponent& src, Resource::AudioHandle handle,
                            const std::string& path)
{
    if (src.clipHandle == handle)
    {
        m_clipSystem->ReleaseClip(handle);
    }
    else if (src.clipHandle.IsValid())
    {
        if (src.activeVoice.IsValid()) m_engine->StopVoice(src.activeVoice, 0.f);
        src.activeVoice = {};
        src.state       = PlayState::Stopped;
        m_clipSystem->ReleaseClip(src.clipHandle);
    }
    src.clipHandle    = handle;
    src.clipPath      = path;
    src.boundClipPath = path;
}

void AudioSystem::OnPlaySound(const PlaySoundEvent& e)
{
    if (!m_engine || !m_clipSystem) return;

    // Resolve clip identity. Acquiring is cheap (path-deduped) and stamps a
    // refCount that ReleaseClip later balances; PlaySoundEvents that target
    // an entity's AudioSource hand that ref to the component (AdoptClip), so a
    // destroyed entity releases automatically. Any other play hands it to the
    // pending-release list, dropped once its voice finishes.
    Resource::AudioHandle handle = m_clipSystem->AcquireClip(e.clipPath);
    if (!handle.IsValid()) return;

    AudioSourceComponent* src = nullptr;
    if (e.entity.entity != NullEntity && m_world && m_world->IsHandleValid(e.entity))
        src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity);

    if (!m_clipSystem->IsReady(handle))
    {
        // The clip is still loading. Drop plays with no AudioSource to
        // remember the request on; a source takes the clip so playOnEnable
        // can fire once loading completes.
        if (!src) { m_clipSystem->ReleaseClip(handle); return; }
        AdoptClip(*src, handle, e.clipPath);
        src->bus          = e.bus;
        src->volume       = e.volume;
        src->pitch        = e.pitch;
        src->loop         = e.loop;
        src->is3D         = e.is3D;
        src->playOnEnable = true;     // Update() will fire once the clip is ready
        return;
    }

    const AudioClipResource* clip = m_clipSystem->GetResource(handle);
    if (!clip) { m_clipSystem->ReleaseClip(handle); return; }

    PlayParams pp;
    pp.bus         = e.bus;
    pp.volume      = e.volume;
    pp.pitch       = e.pitch;
    pp.loop        = e.loop;
    pp.is3D        = e.is3D;
    pp.position    = e.position;
    pp.minDistance = e.minDistance;
    pp.maxDistance = e.maxDistance;
    pp.curve       = e.curve;

    // Entity-targeted: the AudioSource takes the clip reference (released on
    // destroy or when the path changes) and the new voice.
    if (src)
    {
        if (src->activeVoice.IsValid()) m_engine->StopVoice(src->activeVoice, 0.f);
        src->activeVoice = {};
        src->state       = PlayState::Stopped;
        AdoptClip(*src, handle, e.clipPath);

        const VoiceHandle h = m_engine->PlayClip(clip, pp);
        if (h.IsValid())
        {
            src->activeVoice = h;
            src->state       = PlayState::Playing;
        }
        return;
    }

    // Fire-and-forget, including a play addressed to an entity without an AudioSource. The engine
    // borrows clip->data for the voice's lifetime, so releasing the handle now would free that buffer
    // under XAudio2: park the pair and let Update release it once the voice finished.
    const VoiceHandle vh = m_engine->PlayClip(clip, pp);
    if (vh.IsValid())
        m_pendingReleases.push_back({ vh, handle });
    else
        m_clipSystem->ReleaseClip(handle);
}

void AudioSystem::OnStopSound(const StopSoundEvent& e)
{
    if (!m_engine) return;

    if (e.voice.IsValid())
    {
        m_engine->StopVoice(e.voice, e.fadeOutSec);
        return;
    }
    if (e.entity.entity != NullEntity && m_world && m_world->IsHandleValid(e.entity))
    {
        if (auto* src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity))
        {
            if (src->activeVoice.IsValid())
                m_engine->StopVoice(src->activeVoice, e.fadeOutSec);
            src->state       = PlayState::Stopped;
            src->activeVoice = {};
        }
    }
}

void AudioSystem::OnSetParam(const SetAudioParamEvent& e)
{
    if (!m_engine) return;

    VoiceHandle target = e.voice;
    if (!target.IsValid() && e.entity.entity != NullEntity
        && m_world && m_world->IsHandleValid(e.entity))
    {
        if (auto* src = m_world->GetComponent<AudioSourceComponent>(e.entity.entity))
            target = src->activeVoice;
    }
    if (!target.IsValid()) return;

    switch (e.param)
    {
        case SetAudioParamEvent::Volume: m_engine->SetVoiceVolume(target, e.value); break;
        case SetAudioParamEvent::Pitch:  m_engine->SetVoicePitch (target, e.value); break;
    }
}

void AudioSystem::OnBusVolume(const BusVolumeChangedEvent& e)
{
    if (m_engine) m_engine->SetBusVolume(e.bus, e.volume);
}

void AudioSystem::OnEntityDestroyed(Entity e)
{
    // Stop the voice of a destroyed entity's source and release its clip ref: the component goes away
    // next, so this is the last chance to balance its AcquireClip.
    if (!m_engine || !m_world) return;
    if (auto* src = m_world->GetComponent<AudioSourceComponent>(e))
    {
        if (src->activeVoice.IsValid())
            m_engine->StopVoice(src->activeVoice, 0.f);
        if (m_clipSystem && src->clipHandle.IsValid())
            m_clipSystem->ReleaseClip(src->clipHandle);
        src->clipHandle  = Resource::kInvalidAudioHandle;
        src->boundClipPath.clear();
        src->activeVoice = {};
        src->state       = PlayState::Stopped;
    }
}

} // namespace Audio
