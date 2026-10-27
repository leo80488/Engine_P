// Media Foundation cameras.
//
// Every MF object of a camera is made and destroyed on that camera's own control thread, which
// runs in the multithreaded apartment (the main thread is an STA, src/main.cpp): the reader is
// asynchronous (IMFSourceReaderCallback - a blocking ReadSample could not be interrupted on
// Stop), so the callback runs on MF's work queue threads, copies the NV12 sample into the
// mailbox and asks for the next one. The control thread owns the capture state machine:
//   Opening -> Running, or Denied (the privacy setting), Busy (in use: retried every 3 s),
//   Lost (unplugged: reopened when DevicesChanged brings the same symbolic link back).
// A camera that stops while running is Busy again (retried every 3 s) while it is still
// connected, Lost when it is not.
// Stop: the flag, Flush, wait for OnFlush, detach the callback, shut the source down, release -
// on that thread. The callback is a COM object of its own (ReaderCallback): MF may release it on
// a work-queue thread after the camera is gone, so it never holds the camera, only a link that
// Close empties.

#include "FaceTracking/ImageCapture.h"
#include "FaceTracking/FaceSource.h" // SteadyNowUs
#include "FaceTracking/FrameMailbox.h"
#include "System/Log.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace FaceTracking {
namespace {

std::string Utf8(const wchar_t* w)
{
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string HrText(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// MTA + MF for the calling thread, for its lifetime.
struct MfThreadScope
{
    bool com = false, mf = false;
    MfThreadScope()
    {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MfThreadScope()
    {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

void ListNativeFormats(IMFSourceReader* reader, std::vector<CameraFormat>& formats, std::vector<ComPtr<IMFMediaType>>* types)
{
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t))) break;
        GUID sub{};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        formats.push_back({sub.Data1, w, h, den ? static_cast<float>(num) / static_cast<float>(den) : 0.0f});
        if (types) types->push_back(t);
    }
}

bool CreateDeviceSource(const std::string& id, ComPtr<IMFMediaSource>& out, HRESULT& hr)
{
    ComPtr<IMFAttributes> attr;
    hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    const std::wstring link = Wide(id);
    if (SUCCEEDED(hr)) hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attr.Get(), &out);
    return SUCCEEDED(hr);
}

bool DevicePresent(const std::string& id)
{
    std::vector<CameraDevice> devices;
    std::string error;
    if (!EnumerateCameras(devices, error)) return false;
    for (const CameraDevice& d : devices)
        if (d.id == id) return true;
    return false;
}

class MfCamera;

// What a reader's callback reaches its camera through. Close empties it under its mutex - a
// callback running then finishes first, a later one finds no camera - and every reader gets a
// link of its own, so a late call from a reader already released never reaches the next one.
struct ReaderLink
{
    std::mutex mutex;
    MfCamera* camera = nullptr;
};

// The source reader's async callback: deleted by its last Release (MF's, possibly on one of its
// work-queue threads after the camera is gone), holding only the link.
class ReaderCallback final : public IMFSourceReaderCallback
{
public:
    explicit ReaderCallback(std::shared_ptr<ReaderLink> link) : m_link(std::move(link)) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback))
        {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++m_refs); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = --m_refs;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD stream, DWORD flags, LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD stream) override;
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    ~ReaderCallback() = default;
    std::atomic<long> m_refs{1};
    std::shared_ptr<ReaderLink> m_link;
};

class MfCamera final : public IImageCapture
{
public:
    MfCamera(std::string id, uint32_t w, uint32_t h, float fps) : m_id(std::move(id)), m_wantW(w), m_wantH(h), m_wantFps(fps) {}
    ~MfCamera() override { Stop(); }

    // ---- from the reader's callback (MF's work queue threads, under the link's mutex) ----
    void OnSample(HRESULT hrStatus, DWORD flags, LONGLONG timestamp, IMFSample* sample)
    {
        if (FAILED(hrStatus) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
        {
            Signal(FAILED(hrStatus) ? hrStatus : MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED);
            return;
        }
        if (sample) CopySample(sample, timestamp);
        if (!m_stopping.load())
        {
            std::lock_guard<std::mutex> lock(m_readerMutex);
            if (m_reader) m_reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
        }
    }
    void OnFlushed()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_flushed = true;
        }
        m_cv.notify_all();
    }

    // ---- IImageCapture ----
    bool Start(std::string& error) override
    {
        (void)error;
        Stop();
        m_stop = false;
        m_stopping = false;
        SetState(CaptureState::Opening, {});
        m_thread = std::thread([this] { Run(); });
        return true; // Denied / Busy / Lost are states of an open source, not a failed open
    }
    void Stop() override
    {
        if (!m_thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
        SetState(CaptureState::Stopped, {});
    }
    bool Pull() const override { return false; }
    bool Next(int64_t, FaceImage& out) override
    {
        const FaceImage* img = m_mailbox.TakeNewest();
        if (!img) return false;
        out = *img;
        return true;
    }
    CaptureState State() const override { return m_state.load(); }
    std::string LastError() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastError;
    }
    std::string Describe() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        char buf[288];
        std::snprintf(buf, sizeof(buf), "%s %ux%u %c%c%c%c@%.0f -> NV12, %llu frames (%llu dropped)", m_name.empty() ? "camera" : m_name.c_str(),
                      m_format.width, m_format.height, m_format.fourcc & 0xFF, (m_format.fourcc >> 8) & 0xFF,
                      (m_format.fourcc >> 16) & 0xFF, (m_format.fourcc >> 24) & 0xFF, m_format.fps,
                      static_cast<unsigned long long>(m_mailbox.Published()), static_cast<unsigned long long>(m_mailbox.Dropped()));
        std::string s = buf;
        if (const uint64_t n = m_short.load(); n > 0) s += ", " + std::to_string(n) + " buffers shorter than their frame";
        return s;
    }
    void OnDeviceChange() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_deviceChanged = true;
        }
        m_cv.notify_all();
    }

private:
    void SetState(CaptureState s, std::string error)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = std::move(error);
        }
        const CaptureState old = m_state.exchange(s);
        if (old != s) LOG_INFO("[FACETRACK] camera %s: %s%s%s", m_id.c_str(), CaptureStateName(s), m_lastError.empty() ? "" : " - ", m_lastError.c_str());
    }

    void Signal(HRESULT hr)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_readerError = hr;
        }
        m_cv.notify_all();
    }

    void CopySample(IMFSample* sample, LONGLONG timestamp)
    {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) return;
        const uint32_t w = m_outW, h = m_outH;
        if (w == 0 || h == 0) return;
        FaceImage& img = m_mailbox.Back();
        bool copied = false;
        ComPtr<IMF2DBuffer2> b2d;
        if (SUCCEEDED(buf.As(&b2d)))
        {
            BYTE* scan0 = nullptr;
            BYTE* start = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            if (FAILED(b2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length))) return;
            // A bottom-up buffer (pitch < 0) does not happen for these formats: refuse it rather than guess.
            if (pitch > 0 && scan0 >= start && static_cast<size_t>(scan0 - start) <= length)
            {
                const size_t p = static_cast<size_t>(pitch);
                const size_t avail = length - static_cast<size_t>(scan0 - start);
                if (m_yuy2)
                {
                    if (avail >= p * h)
                    {
                        img.FromYuy2(w, h, scan0, p);
                        copied = true;
                    }
                }
                else
                {
                    // The chroma plane follows the luma plane's rows: the frame's height - or, from a
                    // driver that pads the planes to 16 rows without saying so, the padded height when
                    // the buffer is exactly that size.
                    size_t rows = h;
                    const size_t padded = (static_cast<size_t>(h) + 15) & ~static_cast<size_t>(15);
                    if (padded != h && avail >= p * padded * 3 / 2 && avail < p * (padded + 2) * 3 / 2) rows = padded;
                    if (avail >= p * rows + p * (h / 2))
                    {
                        img.CopyFrom(w, h, scan0, p, scan0 + p * rows, p);
                        copied = true;
                    }
                }
            }
            b2d->Unlock2D();
        }
        else
        {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (FAILED(buf->Lock(&data, &maxLen, &curLen))) return;
            if (m_yuy2)
            {
                const size_t pitch = m_stride > 0 ? static_cast<size_t>(m_stride) : static_cast<size_t>(w) * 2;
                if (curLen >= pitch * h)
                {
                    img.FromYuy2(w, h, data, pitch);
                    copied = true;
                }
            }
            else
            {
                const size_t pitch = m_stride > 0 ? static_cast<size_t>(m_stride) : w;
                if (curLen >= pitch * h * 3 / 2)
                {
                    img.CopyFrom(w, h, data, pitch, data + pitch * h, pitch);
                    copied = true;
                }
            }
            buf->Unlock();
        }
        if (!copied)
        {
            if (m_short.fetch_add(1) == 0)
                LOG_WARNING("[FACETRACK] camera %s: a sample's buffer is shorter than its %ux%u NV12 frame - dropped "
                            "(counted in the source's detail)", m_id.c_str(), w, h);
            return;
        }
        img.ptsUs = timestamp / 10; // 100 ns units
        img.captureUs = SteadyNowUs();
        img.seq = ++m_seq;
        m_mailbox.Publish();
    }

    // On the control thread. True: Running.
    bool Open(HRESULT& hr, std::string& why)
    {
        ComPtr<IMFMediaSource> source;
        if (!CreateDeviceSource(m_id, source, hr))
        {
            why = "cannot open the camera (" + HrText(hr) + ")";
            return false;
        }
        auto link = std::make_shared<ReaderLink>();
        link->camera = this;
        ComPtr<ReaderCallback> callback;
        callback.Attach(new ReaderCallback(link));
        ComPtr<IMFAttributes> attr;
        hr = MFCreateAttributes(&attr, 2);
        if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
        if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source.Get(), attr.Get(), &reader);
        if (FAILED(hr))
        {
            source->Shutdown();
            why = "the camera's source reader could not be made (" + HrText(hr) + ")";
            return false;
        }
        // Before the first ReadSample nothing calls back; a failure from here empties the link
        // all the same before the reader goes.
        auto fail = [&](std::string text) {
            {
                std::lock_guard<std::mutex> lock(link->mutex);
                link->camera = nullptr;
            }
            reader.Reset();
            source->Shutdown();
            why = std::move(text);
            return false;
        };
        std::vector<CameraFormat> formats;
        std::vector<ComPtr<IMFMediaType>> natives;
        ListNativeFormats(reader.Get(), formats, &natives);
        // A format the reader refuses falls to the next best.
        for (;;)
        {
            const int k = ChooseCameraFormat(formats, m_wantW, m_wantH, m_wantFps);
            if (k < 0)
            {
                hr = MF_E_INVALIDMEDIATYPE;
                return fail("the camera offers no NV12 / YUY2 / MJPG / RGB24 mode");
            }
            hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, natives[static_cast<size_t>(k)].Get());
            // NV12 and YUY2 are taken as the camera gives them (YUY2 converted in CopySample): asking the reader
            // for NV12 inserts its video processor, which hands a 60 Hz camera's frames over four at a time.
            const uint32_t fourcc = formats[static_cast<size_t>(k)].fourcc;
            m_yuy2 = fourcc == kFourccYUY2;
            if (SUCCEEDED(hr) && fourcc != kFourccNV12 && fourcc != kFourccYUY2)
            {
                ComPtr<IMFMediaType> out;
                hr = MFCreateMediaType(&out);
                if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                if (SUCCEEDED(hr))
                    hr = MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, formats[static_cast<size_t>(k)].width, formats[static_cast<size_t>(k)].height);
                if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out.Get());
            }
            if (SUCCEEDED(hr))
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_format = formats[static_cast<size_t>(k)];
                break;
            }
            formats.erase(formats.begin() + k);
            natives.erase(natives.begin() + k);
        }
        ComPtr<IMFMediaType> current;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current)))
        {
            MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
            m_stride = static_cast<LONG>(MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, m_yuy2 ? w * 2 : w));
        }
        m_outW = w & ~1u;
        m_outH = h & ~1u;
        if (m_outW == 0 || m_outH == 0) return fail("the camera's NV12 output has no frame size");
        {
            std::lock_guard<std::mutex> lock(m_readerMutex);
            m_reader = reader;
        }
        m_source = source;
        m_link = link;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr))
        {
            why = "ReadSample failed (" + HrText(hr) + ")";
            Close();
            return false;
        }
        return true;
    }

    void Close()
    {
        ComPtr<IMFSourceReader> reader;
        {
            std::lock_guard<std::mutex> lock(m_readerMutex);
            reader = m_reader;
        }
        if (reader)
        {
            m_stopping = true;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_flushed = false;
            }
            if (SUCCEEDED(reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS))))
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::seconds(2), [this] { return m_flushed; });
            }
        }
        // From here nothing MF still holds reaches this camera: a callback running now finishes
        // first (it holds the link's mutex); a later one - after a flush that did not answer in
        // 2 s, or MF's last Release on a work-queue thread - finds no camera.
        if (m_link)
        {
            std::lock_guard<std::mutex> lock(m_link->mutex);
            m_link->camera = nullptr;
        }
        m_link.reset();
        {
            std::lock_guard<std::mutex> lock(m_readerMutex);
            m_reader.Reset();
        }
        reader.Reset();
        if (m_source)
        {
            m_source->Shutdown();
            m_source.Reset();
        }
        m_stopping = false;
    }

    void Run()
    {
        MfThreadScope scope;
        {
            std::vector<CameraDevice> devices; // enumerated outside the lock: it opens each device
            std::string error;
            std::string name;
            if (EnumerateCameras(devices, error))
                for (const CameraDevice& d : devices)
                    if (d.id == m_id) name = d.name;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_name = name;
        }
        auto waitFor = [this](std::chrono::milliseconds t) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, t, [this] { return m_stop || m_deviceChanged || m_readerError != S_OK; });
            const bool changed = m_deviceChanged;
            m_deviceChanged = false;
            return changed;
        };
        const char* const kBusy = "the camera is in use by another program, or the USB bandwidth is short - retrying every 3 s";
        const char* const kDenied = "camera access is off for desktop apps (Settings > Privacy & security > Camera)";
        auto inUse = [](HRESULT hr) {
            return hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
                   hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED;
        };
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stop) break;
            }
            const CaptureState s = m_state.load();
            if (s == CaptureState::Opening)
            {
                HRESULT hr = S_OK;
                std::string why;
                if (Open(hr, why)) SetState(CaptureState::Running, {});
                else if (hr == E_ACCESSDENIED)
                    SetState(CaptureState::Denied, kDenied);
                else if (inUse(hr))
                    SetState(CaptureState::Busy, kBusy);
                else if (!DevicePresent(m_id))
                    SetState(CaptureState::Lost, "the camera is not connected - it reopens when it is plugged in again");
                else
                    SetState(CaptureState::Failed, why);
                continue;
            }
            if (s == CaptureState::Running)
            {
                waitFor(std::chrono::milliseconds(500));
                HRESULT err = S_OK;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    err = m_readerError;
                    m_readerError = S_OK;
                }
                if (err != S_OK)
                {
                    Close();
                    if (err == E_ACCESSDENIED)
                        SetState(CaptureState::Denied, kDenied);
                    else if (inUse(err))
                        SetState(CaptureState::Busy, kBusy); // another program took it: try again
                    else if (DevicePresent(m_id))
                        SetState(CaptureState::Busy, "the camera stopped (" + HrText(err) + ") - retrying every 3 s");
                    else
                        SetState(CaptureState::Lost, "the camera stopped (" + HrText(err) + ") - it reopens when it is plugged in again");
                }
                continue;
            }
            if (s == CaptureState::Busy)
            {
                waitFor(std::chrono::seconds(3));
                SetState(CaptureState::Opening, {});
                continue;
            }
            // Lost / Denied / Failed: wait for the device list to change, then try again.
            if (waitFor(std::chrono::seconds(1)))
                if (s != CaptureState::Lost || DevicePresent(m_id)) SetState(CaptureState::Opening, {});
        }
        Close();
    }

    const std::string m_id;
    const uint32_t m_wantW, m_wantH;
    const float m_wantFps;
    std::string m_name;
    std::atomic<CaptureState> m_state{CaptureState::Stopped};
    std::atomic<bool> m_stopping{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false, m_deviceChanged = false, m_flushed = false;
    HRESULT m_readerError = S_OK;
    std::string m_lastError;
    CameraFormat m_format;
    std::mutex m_readerMutex;
    ComPtr<IMFSourceReader> m_reader;
    ComPtr<IMFMediaSource> m_source;
    std::shared_ptr<ReaderLink> m_link; // the current reader's (control thread only)
    uint32_t m_outW = 0, m_outH = 0;
    LONG m_stride = 0;
    bool m_yuy2 = false; // the reader gives the camera's YUY2 as is; CopySample converts it to NV12
    uint64_t m_seq = 0;
    std::atomic<uint64_t> m_short{0};   // samples dropped: a buffer shorter than its frame
    FrameMailbox m_mailbox;
    std::thread m_thread;
};

STDMETHODIMP ReaderCallback::OnReadSample(HRESULT hrStatus, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample)
{
    std::lock_guard<std::mutex> lock(m_link->mutex);
    if (m_link->camera) m_link->camera->OnSample(hrStatus, flags, timestamp, sample);
    return S_OK;
}

STDMETHODIMP ReaderCallback::OnFlush(DWORD)
{
    std::lock_guard<std::mutex> lock(m_link->mutex);
    if (m_link->camera) m_link->camera->OnFlushed();
    return S_OK;
}

} // namespace

bool EnumerateCameras(std::vector<CameraDevice>& out, std::string& error)
{
    out.clear();
    // On a thread of its own, in the MTA: the caller may be the main thread's STA.
    bool ok = false;
    std::thread worker([&] {
        MfThreadScope scope;
        if (!scope.mf)
        {
            error = "MFStartup failed";
            return;
        }
        ComPtr<IMFAttributes> attr;
        HRESULT hr = MFCreateAttributes(&attr, 1);
        if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(hr))
        {
            error = "the camera list's attributes (" + HrText(hr) + ")";
            return;
        }
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attr.Get(), &devices, &count);
        if (FAILED(hr))
        {
            error = "MFEnumDeviceSources failed (" + HrText(hr) + ")";
            return;
        }
        for (UINT32 i = 0; i < count; ++i)
        {
            CameraDevice d;
            WCHAR* text = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &text, &len)))
            {
                d.name = Utf8(text);
                CoTaskMemFree(text);
            }
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &text, &len)))
            {
                d.id = Utf8(text);
                CoTaskMemFree(text);
            }
            ComPtr<IMFMediaSource> source;
            if (SUCCEEDED(devices[i]->ActivateObject(IID_PPV_ARGS(&source))))
            {
                ComPtr<IMFSourceReader> reader;
                if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader)))
                    ListNativeFormats(reader.Get(), d.formats, nullptr);
                reader.Reset();
                source->Shutdown();
                devices[i]->ShutdownObject();
            }
            devices[i]->Release();
            out.push_back(std::move(d));
        }
        CoTaskMemFree(devices);
        ok = true;
    });
    worker.join();
    return ok;
}

std::unique_ptr<IImageCapture> MakeMfCamera(const std::string& deviceId, uint32_t width, uint32_t height, float fps)
{
    return std::make_unique<MfCamera>(deviceId, width, height, fps);
}

} // namespace FaceTracking
