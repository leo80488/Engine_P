#include "Audio/AudioEngine.h"
#include "System/Log.h"

#pragma comment(lib, "xaudio2.lib")

namespace Audio
{

namespace
{

// Helper: log + return false if HRESULT failed.
bool Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        LOG_ERROR("[Audio] %s failed (hr=0x%08lX)", what, static_cast<long>(hr));
        return false;
    }
    return true;
}

} // anonymous

AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Initialize()
{
    if (m_initialized) return true;

    // main.cpp initialises COM on the main thread (single-threaded apartment)
    // before App exists. XAudio2Create works regardless.
    HRESULT hr = XAudio2Create(m_xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (!Check(hr, "XAudio2Create")) return false;

    hr = m_xaudio2->CreateMasteringVoice(&m_master);
    if (!Check(hr, "CreateMasteringVoice")) { Shutdown(); return false; }

    // Query master voice details for X3DAudio.
    XAUDIO2_VOICE_DETAILS masterDetails{};
    m_master->GetVoiceDetails(&masterDetails);
    m_masterChannels = masterDetails.InputChannels;

    DWORD channelMask = 0;
    if (FAILED(m_master->GetChannelMask(&channelMask)))
    {
        // Fall back to stereo if the driver doesn't report a mask.
        channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    m_channelMask = channelMask;

    // ---- Submix bus graph ----
    // BusType::Master is the mastering voice itself (no submix); the other five submixes are created
    // with no send list, so XAudio2 routes them to the mastering voice.
    m_submixes[(size_t)BusType::Master] = nullptr;
    auto MakeSubmix = [&](BusType bus, const char* name) -> bool
    {
        IXAudio2SubmixVoice* v = nullptr;
        hr = m_xaudio2->CreateSubmixVoice(
            &v, masterDetails.InputChannels, masterDetails.InputSampleRate,
            0, 0, nullptr, nullptr);
        if (!Check(hr, name)) return false;
        m_submixes[(size_t)bus] = v;
        return true;
    };
    if (!MakeSubmix(BusType::Music,   "CreateSubmixVoice(Music)"))   { Shutdown(); return false; }
    if (!MakeSubmix(BusType::SFX,     "CreateSubmixVoice(SFX)"))     { Shutdown(); return false; }
    if (!MakeSubmix(BusType::Voice,   "CreateSubmixVoice(Voice)"))   { Shutdown(); return false; }
    if (!MakeSubmix(BusType::Ambient, "CreateSubmixVoice(Ambient)")) { Shutdown(); return false; }
    if (!MakeSubmix(BusType::UI,      "CreateSubmixVoice(UI)"))      { Shutdown(); return false; }

    hr = X3DAudioInitialize(m_channelMask, X3DAUDIO_SPEED_OF_SOUND, m_x3dInstance);
    if (!Check(hr, "X3DAudioInitialize")) { Shutdown(); return false; }

    // Default listener: origin, facing +Z, up +Y. World units are metres.
    m_listener            = {};
    m_listener.OrientFront = { 0.f, 0.f, 1.f };
    m_listener.OrientTop   = { 0.f, 1.f, 0.f };

    // Slot 0 is the invalid-handle sentinel; the free list holds 1..kMaxVoices-1.
    m_freeSlots.clear();
    m_freeSlots.reserve(kMaxVoices);
    for (uint32_t i = kMaxVoices; i-- > 1;) m_freeSlots.push_back(i);

    m_initialized = true;
    LOG_INFO("[Audio] AudioEngine initialized (master ch=%u rate=%u)",
             m_masterChannels, masterDetails.InputSampleRate);
    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_xaudio2 && !m_master) return;

    // Tear down all live source voices first; their callback objects must
    // outlive the voice (XAudio2 may still be flushing on its thread).
    {
        std::lock_guard<std::mutex> lk(m_voicesMutex);
        for (auto& rec : m_voices)
        {
            if (rec.source)
            {
                rec.source->Stop(0);
                rec.source->FlushSourceBuffers();
                rec.source->DestroyVoice();
                rec.source = nullptr;
            }
            rec.callback.reset();
            rec.clip = nullptr;
            rec.inUse = false;
        }
        m_freeSlots.clear();
    }

    for (auto*& v : m_submixes)
    {
        if (v) { v->DestroyVoice(); v = nullptr; }
    }

    if (m_master) { m_master->DestroyVoice(); m_master = nullptr; }
    m_xaudio2.Reset();
    m_initialized = false;
    LOG_INFO("[Audio] AudioEngine shut down");
}

void AudioEngine::Update(float /*dt*/)
{
    if (!m_initialized) return;

    // Reclaim voices flagged finished by the audio worker thread.
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    for (uint32_t i = 1; i < m_voices.size(); ++i)
    {
        VoiceRecord& rec = m_voices[i];
        if (!rec.inUse) continue;
        if (!rec.finished.load(std::memory_order_acquire)) continue;

        if (rec.source)
        {
            rec.source->Stop(0);
            rec.source->FlushSourceBuffers();
            rec.source->DestroyVoice();
            rec.source = nullptr;
        }
        rec.callback.reset();
        rec.clip = nullptr;
        rec.finished.store(false, std::memory_order_relaxed);
        ReleaseSlot(i);
    }
}

uint32_t AudioEngine::AcquireSlot()
{
    if (m_freeSlots.empty()) return 0;
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    VoiceRecord& rec = m_voices[slot];
    rec.inUse = true;
    // Generation bumps every reuse so stale handles fail ResolveHandle.
    if (++rec.generation == 0) rec.generation = 1;
    return slot;
}

void AudioEngine::ReleaseSlot(uint32_t slot)
{
    if (slot == 0 || slot >= m_voices.size()) return;
    VoiceRecord& rec = m_voices[slot];
    rec.inUse = false;
    m_freeSlots.push_back(slot);
}

bool AudioEngine::ResolveHandle(VoiceHandle h, VoiceRecord*& out)
{
    out = nullptr;
    if (!h.IsValid()) return false;
    const uint32_t idx = h.Index();
    if (idx == 0 || idx >= m_voices.size()) return false;
    VoiceRecord& rec = m_voices[idx];
    if (!rec.inUse) return false;
    if ((rec.generation & VoiceHandle::kGenerationMask) != h.Generation()) return false;
    out = &rec;
    return true;
}

bool AudioEngine::ResolveHandle(VoiceHandle h, const VoiceRecord*& out) const
{
    out = nullptr;
    if (!h.IsValid()) return false;
    const uint32_t idx = h.Index();
    if (idx == 0 || idx >= m_voices.size()) return false;
    const VoiceRecord& rec = m_voices[idx];
    if (!rec.inUse) return false;
    if ((rec.generation & VoiceHandle::kGenerationMask) != h.Generation()) return false;
    out = &rec;
    return true;
}

void AudioEngine::RouteToBus(IXAudio2SourceVoice* voice, BusType bus)
{
    // CreateSourceVoice routes to master by default; any other bus overrides the send list with
    // source -> submix (-> master).
    if (bus == BusType::Master) return;

    IXAudio2SubmixVoice* submix = m_submixes[(size_t)bus];
    if (!submix) return;

    XAUDIO2_SEND_DESCRIPTOR sendDesc{};
    sendDesc.Flags     = 0;
    sendDesc.pOutputVoice = submix;
    XAUDIO2_VOICE_SENDS sends{};
    sends.SendCount = 1;
    sends.pSends    = &sendDesc;
    voice->SetOutputVoices(&sends);
}

VoiceHandle AudioEngine::PlayClip(const AudioClipResource* clip, const PlayParams& params)
{
    if (!m_initialized || !clip) return {};

    // Only fully loaded PCM plays (the same gate as AudioClipLoader); anything else is rejected rather
    // than fed to XAudio2 as garbage.
    // TODO(audio_system_architecture 6.4): Compressed / Streaming clips (decoders, streamer thread).
    if (!clip->IsPCM() || !clip->IsFullyLoaded() || clip->data.empty())
    {
        LOG_WARNING("[Audio] PlayClip rejected unsupported clip "
                    "(codec=%u strategy=%u dataBytes=%zu)",
                    clip->header.codec, clip->header.strategy, clip->data.size());
        return {};
    }

    // Acquire the slot before CreateSourceVoice so a failed creation leaves the table consistent.
    uint32_t slot = 0;
    {
        std::lock_guard<std::mutex> lk(m_voicesMutex);
        slot = AcquireSlot();
    }
    if (slot == 0)
    {
        LOG_WARNING("[Audio] Out of voice slots; dropping play");
        return {};
    }

    VoiceRecord& rec = m_voices[slot];
    rec.clip        = clip;
    rec.srcChannels = clip->format.nChannels;
    rec.bus         = params.bus;
    rec.is3D        = params.is3D;
    rec.minDistance = params.minDistance;
    rec.maxDistance = params.maxDistance;
    rec.curve       = params.curve;
    rec.userPaused  = false;          // recycled slot: fresh voice is not paused
    rec.finished.store(false, std::memory_order_relaxed);

    rec.callback = std::make_unique<VoiceCallback>();
    rec.callback->finishedFlag = &rec.finished;
    rec.callback->loop         = params.loop;

    IXAudio2SourceVoice* src = nullptr;
    HRESULT hr = m_xaudio2->CreateSourceVoice(
        &src, &clip->format,
        0, XAUDIO2_DEFAULT_FREQ_RATIO, rec.callback.get(), nullptr, nullptr);
    if (!Check(hr, "CreateSourceVoice"))
    {
        rec.callback.reset();
        rec.clip = nullptr;
        std::lock_guard<std::mutex> lk(m_voicesMutex);
        ReleaseSlot(slot);
        return {};
    }

    rec.source = src;
    RouteToBus(src, params.bus);

    src->SetVolume(params.volume);
    src->SetFrequencyRatio(params.pitch);

    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(clip->data.size());
    buf.pAudioData = clip->data.data();
    buf.Flags      = XAUDIO2_END_OF_STREAM;
    buf.LoopCount  = params.loop ? XAUDIO2_LOOP_INFINITE : 0;

    // Start part-way in. PlayBegin is in samples and only meaningful when the whole clip is one
    // resident PCM buffer; any other clip warns and plays from the start rather than from a garbage
    // position.
    if (params.startOffsetSec > 0.f && clip->IsPCM() && clip->IsFullyLoaded()
        && clip->format.nSamplesPerSec > 0 && clip->format.nBlockAlign > 0)
    {
        const UINT32 totalSamples =
            static_cast<UINT32>(clip->data.size() / clip->format.nBlockAlign);
        const UINT32 begin = static_cast<UINT32>(
            params.startOffsetSec * static_cast<float>(clip->format.nSamplesPerSec));
        if (begin < totalSamples)
        {
            buf.PlayBegin  = begin;
            buf.PlayLength = totalSamples - begin;
        }
    }
    else if (params.startOffsetSec > 0.f)
    {
        LOG_WARNING("AudioEngine: startOffsetSec ignored — clip is not a fully-resident "
                    "PCM buffer (codec=%d strategy=%d)",
                    static_cast<int>(clip->header.codec),
                    static_cast<int>(clip->header.strategy));
    }
    src->SubmitSourceBuffer(&buf);
    src->Start(0);

    return VoiceHandle::Make(slot, rec.generation);
}

void AudioEngine::StopVoice(VoiceHandle h, float fadeOutSec)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (!ResolveHandle(h, rec) || !rec->source) return;

    if (fadeOutSec > 0.0f)
    {
        // TODO(audio_system_architecture 3.2): a real fade-out. XAudio2 has no volume ramp (it needs a
        // SetVolume curve or an effect), so the voice stops at once; fadeOutSec stays in the API.
        (void)fadeOutSec;
    }
    rec->source->Stop(0);
    // Mark finished; the next Update() reclaims the slot. Do not tear down here: the audio worker
    // thread may still touch the buffer.
    rec->finished.store(true, std::memory_order_release);
}

void AudioEngine::SetVoiceVolume(VoiceHandle h, float volume)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (ResolveHandle(h, rec) && rec->source) rec->source->SetVolume(volume);
}

void AudioEngine::SetVoicePitch(VoiceHandle h, float pitch)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (ResolveHandle(h, rec) && rec->source) rec->source->SetFrequencyRatio(pitch);
}

void AudioEngine::PauseVoice(VoiceHandle h)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    // Stop(0) WITHOUT FlushSourceBuffers keeps the queued buffer + read cursor,
    // so ResumeVoice continues from the same sample. `finished` stays false, so
    // the reclaim pass in Update() leaves the paused voice alone. userPaused
    // marks it so a global pause/resume cycle won't override the intent.
    if (ResolveHandle(h, rec) && rec->source)
    {
        rec->source->Stop(0);
        rec->userPaused = true;
    }
}

void AudioEngine::ResumeVoice(VoiceHandle h)
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (ResolveHandle(h, rec) && rec->source)
    {
        rec->source->Start(0);
        rec->userPaused = false;
    }
}

uint32_t AudioEngine::ActiveVoiceCount() const
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    uint32_t n = 0;
    for (uint32_t i = 1; i < m_voices.size(); ++i)
        if (m_voices[i].inUse) ++n;
    return n;
}

void AudioEngine::SetGlobalPause(bool paused)
{
    if (!m_initialized || paused == m_globalPaused) return;
    m_globalPaused = paused;

    std::lock_guard<std::mutex> lk(m_voicesMutex);
    for (uint32_t i = 1; i < m_voices.size(); ++i)
    {
        VoiceRecord& rec = m_voices[i];
        if (!rec.inUse || !rec.source) continue;
        // Leave individually-paused voices stopped (don't resume them on a global
        // resume) and don't restart a voice that already finished playing.
        if (rec.userPaused) continue;
        if (rec.finished.load(std::memory_order_acquire)) continue;
        // Stop(0) / Start(0) without a flush freezes / unfreezes in place.
        if (paused) rec.source->Stop(0);
        else        rec.source->Start(0);
    }
}

void AudioEngine::StopAll()
{
    if (!m_initialized) return;

    std::lock_guard<std::mutex> lk(m_voicesMutex);
    for (uint32_t i = 1; i < m_voices.size(); ++i)
    {
        VoiceRecord& rec = m_voices[i];
        if (!rec.inUse) continue;
        if (rec.source)
        {
            rec.source->Stop(0);
            rec.source->FlushSourceBuffers();
            rec.source->DestroyVoice();
            rec.source = nullptr;
        }
        rec.callback.reset();
        rec.clip = nullptr;
        rec.finished.store(false, std::memory_order_relaxed);
        ReleaseSlot(i);
    }
    m_globalPaused = false;
}

bool AudioEngine::IsVoicePlaying(VoiceHandle h) const
{
    std::lock_guard<std::mutex> lk(m_voicesMutex);
    const VoiceRecord* rec = nullptr;
    if (!ResolveHandle(h, rec)) return false;
    return rec->source != nullptr && !rec->finished.load(std::memory_order_acquire);
}

void AudioEngine::SetListener(const X3DAUDIO_LISTENER& listener)
{
    // No mutex: the listener is read only by Apply3D, also on the main thread. Guard it if Apply3D
    // ever runs on a worker.
    m_listener = listener;
}

void AudioEngine::Apply3D(VoiceHandle h, const X3DAUDIO_EMITTER& emitter)
{
    if (!m_initialized) return;

    std::lock_guard<std::mutex> lk(m_voicesMutex);
    VoiceRecord* rec = nullptr;
    if (!ResolveHandle(h, rec) || !rec->source || !rec->is3D) return;

    // Every bus outputs m_masterChannels: the submixes were created with the master's input channel
    // count.
    const UINT32 srcChannels = rec->srcChannels ? rec->srcChannels : 1;
    const UINT32 dstChannels = m_masterChannels;

    float matrix[8 * 8] = {};   // up to 8x8: covers stereo, 5.1, 7.1
    if (srcChannels * dstChannels > 8 * 8) return;

    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.SrcChannelCount     = srcChannels;
    dsp.DstChannelCount     = dstChannels;
    dsp.pMatrixCoefficients = matrix;

    // Local copy so an unset channel count / curve-distance / doppler scaler gets a default.
    X3DAUDIO_EMITTER e = emitter;
    if (e.ChannelCount == 0) e.ChannelCount = srcChannels;
    if (e.CurveDistanceScaler <= 0.f)
        e.CurveDistanceScaler = rec->maxDistance > 0.f ? rec->maxDistance : 1.f;
    if (e.DopplerScaler <= 0.f) e.DopplerScaler = 1.f;

    X3DAudioCalculate(
        m_x3dInstance, &m_listener, &e,
        X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER,
        &dsp);

    rec->source->SetOutputMatrix(nullptr, srcChannels, dstChannels, matrix);
    rec->source->SetFrequencyRatio(dsp.DopplerFactor);
}

void AudioEngine::SetBackgroundMute(bool muted)
{
    if (muted == m_backgroundMuted) return;
    m_backgroundMuted = muted;
    if (m_initialized && m_master)
        m_master->SetVolume(muted ? 0.0f : m_masterVolume);
}

void AudioEngine::SetBusVolume(BusType bus, float volume)
{
    if (!m_initialized) return;
    if (bus == BusType::Master)
    {
        m_masterVolume = volume;
        if (m_master) m_master->SetVolume(m_backgroundMuted ? 0.0f : volume);
        return;
    }
    IXAudio2SubmixVoice* sub = m_submixes[(size_t)bus];
    if (sub) sub->SetVolume(volume);
}

} // namespace Audio
