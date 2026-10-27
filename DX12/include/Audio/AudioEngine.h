#pragma once

// AudioEngine - the XAudio2 wrapper, lowest layer of the audio stack: knows nothing about ECS,
// events or scenes. Design: docs/design/audio_system_architecture.md.
// Owns IXAudio2 + the mastering voice, one submix voice per BusType (Music / SFX / Voice / Ambient /
// UI) feeding master (BusType::Master is the mastering voice itself), the live-voice table (slot
// index + generation packed into VoiceHandle), and the X3DAudio instance + listener pose.
// A source voice is created per PlayClip and destroyed when reclaimed.
// TODO(audio_system_architecture 2.2): a voice pool bucketed by format, to avoid CreateSourceVoice churn.
// Clips are not stored here: AudioClipResource lives in the resource manager (reached through
// AudioClipSystem); PlayClip borrows the resource for the voice's lifetime.

#include "Audio/AudioTypes.h"
#include "Audio/AudioClipResource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Audio
{

class AudioEngine
{
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Initialize();
    bool IsInitialized() const { return m_initialized; }
    void Shutdown();

    // Per-frame sync point: reclaims the source voices the XAudio2 worker thread flagged finished.
    void Update(float dt);

    // Starts @p clip with @p params. The caller (AudioSystem, once AudioClipSystem reports the clip
    // ready) owns the resource; the engine borrows its format and PCM bytes for the voice's lifetime.
    // Returns an invalid VoiceHandle on failure (null clip, not fully loaded PCM, no free slot,
    // CreateSourceVoice failed).
    VoiceHandle PlayClip(const AudioClipResource* clip, const PlayParams& params);

    // Live-voice control; each is a no-op for a stale handle. StopVoice stops at once (fadeOutSec is
    // accepted but not applied yet).
    void StopVoice(VoiceHandle h, float fadeOutSec = 0.0f);
    void SetVoiceVolume(VoiceHandle h, float volume);
    void SetVoicePitch (VoiceHandle h, float pitch);

    // Pause / resume one voice in place: Stop(0) without a flush keeps the queued buffer and read
    // cursor, Start(0) resumes. A paused voice still reports IsVoicePlaying() == true, so its owning
    // component is not reclaimed.
    void PauseVoice (VoiceHandle h);
    void ResumeVoice(VoiceHandle h);

    // False once the voice finished or its slot was released / reused.
    bool IsVoicePlaying(VoiceHandle h) const;

    // Source voices currently holding a slot (playing, paused, or finished and
    // not yet reclaimed by Update). Diagnostics: Debug.Stats().audioVoices.
    uint32_t ActiveVoiceCount() const;

    // ---- Transport-wide control ----
    // SetGlobalPause freezes / unfreezes every live voice except individually paused ones (idempotent).
    // The editor drives it from the play mode: XAudio2 runs on its own thread and the audio ECS systems
    // are Play-gated, so nothing else halts PIE audio while the editor is Paused. StopAll tears every
    // voice down (the Play -> Edit edge, so no sound lingers).
    void SetGlobalPause(bool paused);
    void StopAll();
    // The application is in the background (Plat::BackgroundPolicy muteOnFocusLoss): the mastering voice
    // is silenced and every voice keeps playing, so the sound is where it would have been when the
    // person comes back. The master bus volume the mixer last set is restored then. Idempotent.
    void SetBackgroundMute(bool muted);
    bool BackgroundMuted() const { return m_backgroundMuted; }

    // ---- 3D ----
    // The listener pose is one global (last writer wins); Audio3DSystem sets it every tick from the
    // active AudioListenerComponent.
    void SetListener(const X3DAUDIO_LISTENER& listener);

    // Runs X3DAudio (matrix + doppler) for @p emitter against the listener and writes the voice's
    // output matrix and frequency ratio; the doppler factor replaces the voice's pitch. No-op for a
    // non-3D voice.
    void Apply3D(VoiceHandle h, const X3DAUDIO_EMITTER& emitter);

    // ---- Mixer ----
    // Volume is a linear gain (0..N), 1.0 = unity.
    void SetBusVolume(BusType bus, float volume);

private:
    // Per-voice record, in m_voices at VoiceHandle::Index(). generation increments on every slot reuse,
    // so a handle with a stale generation is dropped (use-after-free safety).
    struct VoiceCallback;

    struct VoiceRecord
    {
        IXAudio2SourceVoice*             source       = nullptr;
        // Borrowed: owned by the resource manager; AudioClipSystem's ref count keeps the PCM alive while a
        // voice references it.
        const AudioClipResource*         clip         = nullptr;
        // Source-channel count cached at PlayClip time so Apply3D can resolve
        // matrix dims without dereferencing `clip`.
        uint16_t                         srcChannels  = 1;
        std::unique_ptr<VoiceCallback>   callback;
        BusType                          bus          = BusType::SFX;
        bool                             is3D         = false;
        float                            minDistance  = 1.0f;
        float                            maxDistance  = 50.0f;
        AttenuationCurve                 curve        = AttenuationCurve::Linear;
        uint32_t                         generation   = 1;     // 0 = handle invalid
        // Set by VoiceCallback::OnStreamEnd on the XAudio2 worker thread, or by StopVoice; the main
        // thread's Update polls it and tears the voice down.
        std::atomic<bool>                finished     { false };
        bool                             inUse        = false;
        // True while this voice is individually paused (PauseVoice: inspector / Lua / MotionDirector).
        // SetGlobalPause skips it, so an editor pause / resume never un-pauses it.
        bool                             userPaused   = false;
    };

    // XAudio2 callback on the audio worker thread. Keep it minimal: it only flips an atomic, and the
    // main thread reclaims the voice on the next Update.
    struct VoiceCallback : public IXAudio2VoiceCallback
    {
        std::atomic<bool>* finishedFlag = nullptr;
        bool               loop         = false;

        void STDMETHODCALLTYPE OnStreamEnd() noexcept override
        {
            if (!loop && finishedFlag) finishedFlag->store(true, std::memory_order_release);
        }
        // Other callbacks unused but must be defined for the vtable.
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd()         noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart (void*)              noexcept override {}
        void STDMETHODCALLTYPE OnBufferEnd   (void*)              noexcept override {}
        void STDMETHODCALLTYPE OnLoopEnd     (void*)              noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError  (void*, HRESULT)     noexcept override {}
    };

    // ---- Voice table ----
    static constexpr uint32_t                  kMaxVoices = 128;

    // Internal helpers: the caller holds m_voicesMutex (or the voice is not yet visible to the audio
    // thread).
    uint32_t AcquireSlot();
    void     ReleaseSlot(uint32_t slot);
    bool     ResolveHandle(VoiceHandle h, VoiceRecord*& out);
    bool     ResolveHandle(VoiceHandle h, const VoiceRecord*& out) const;

    // ---- Mixer ----
    // Send-list helper: routes a fresh source voice into the bus submix.
    void RouteToBus(IXAudio2SourceVoice* voice, BusType bus);

    // ---- Core XAudio2 objects ----
    Microsoft::WRL::ComPtr<IXAudio2>           m_xaudio2;
    IXAudio2MasteringVoice*                    m_master = nullptr;
    std::array<IXAudio2SubmixVoice*, (size_t)BusType::Count> m_submixes{};

    // ---- Voice table ----
    // m_voicesMutex guards m_voices + m_freeSlots and is held briefly (PlayClip, Update, Apply3D, ...);
    // the XAudio2 callbacks never take it. A fixed std::array because VoiceRecord holds non-movable
    // members (std::atomic, std::unique_ptr); slot 0 is the invalid-handle sentinel.
    mutable std::mutex                         m_voicesMutex;
    std::array<VoiceRecord, kMaxVoices>        m_voices;
    std::vector<uint32_t>                      m_freeSlots;

    // ---- 3D ----
    X3DAUDIO_HANDLE                            m_x3dInstance{};
    X3DAUDIO_LISTENER                          m_listener{};
    DWORD                                      m_channelMask = 0;
    UINT32                                     m_masterChannels = 2;

    bool                                       m_initialized = false;
    bool                                       m_globalPaused = false;   // SetGlobalPause state
    bool                                       m_backgroundMuted = false;
    float                                      m_masterVolume = 1.0f;    // the mixer's, under the mute
};

} // namespace Audio
