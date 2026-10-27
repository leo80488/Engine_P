// The built-in face tracker on ONNX Runtime (include/FaceTracking/FaceTracker.h). ONNX Runtime is
// loaded at the first tracker by its full path beside the exe and reached only through the OrtApi
// table that DLL hands back (ORT_API_MANUAL_INIT): nothing links onnxruntime.lib, so an exe without
// the DLL starts and just has no tracker.
// The models load once per process (on a thread of their own; a video's tracker waits for them) into
// sessions every tracker shares; a tracker holds only its region of interest, its smoothing and buffers.
// The runtime and the model cache are never destroyed by static destruction: an exit that skips
// Service::Shutdown (an exception out of App::Run) would otherwise join nothing and terminate on the
// loader's thread, and release the Env before the sessions. ReleaseFaceTrackerModels (Shutdown) is
// the orderly end: the loader joined, the sessions released, then the Env.

#include "FaceTracking/FaceTracker.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#if ENGINE_WITH_ONNXRUNTIME
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT
#endif

namespace FaceTracking {
namespace {
constexpr const char* kModelDir = "engine/models/face/";

double NowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if ENGINE_WITH_ONNXRUNTIME

struct Runtime
{
    std::mutex mutex;
    bool tried = false, loaded = false;
    std::string why, version;
    std::unique_ptr<Ort::Env> env;
};

struct Model
{
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inNames, outNames;
    std::vector<const char*> inPtrs, outPtrs;
    std::vector<std::vector<int64_t>> inShapes, outShapes;
};

// The three sessions and the canonical face, shared by every tracker (Run is safe from several threads).
struct SharedModels
{
    Model det, mesh, blend;
    std::vector<uint16_t> ids;
    std::vector<float> canonical, weights;
    std::vector<Tracker::Anchor> anchors;
    std::string version;
    int threads = 2;
};

// The process's models: loaded once - on a thread of its own when the first asker cannot wait - and kept
// until ReleaseFaceTrackerModels.
struct ModelCache
{
    std::mutex mutex;
    std::condition_variable cv;
    enum class State { Idle, Loading, Ready, Failed } state = State::Idle;
    std::string error;
    std::shared_ptr<SharedModels> models;
    std::thread loader;
};

class MediaPipeTracker final : public IFaceTracker
{
public:
    explicit MediaPipeTracker(std::shared_ptr<SharedModels> models, const FaceTrackerOptions& options)
        : m_models(std::move(models)), m_opt(options)
    {
        m_detIn.resize(static_cast<size_t>(Tracker::kDetectorSize) * Tracker::kDetectorSize * 3);
        m_meshIn.resize(static_cast<size_t>(Tracker::kLandmarkSize) * Tracker::kLandmarkSize * 3);
        m_blendIn.resize(static_cast<size_t>(Tracker::kBlendshapeInputs) * 2);
        m_reg.resize(static_cast<size_t>(Tracker::kAnchors) * Tracker::kDetectorCoords);
        m_logits.resize(Tracker::kAnchors);
        m_raw.resize(static_cast<size_t>(Tracker::kLandmarks) * 3);
    }

    bool Track(const FaceImage& image, int64_t tUs, FaceTrackerResult& out) override
    {
        out = FaceTrackerResult{};
        if (image.Empty()) return true;
        const SharedModels& M = *m_models;
        const double t0 = NowMs();
        const int w = static_cast<int>(image.width), h = static_cast<int>(image.height);
        try
        {
            if (!m_tracking)
            {
                out.detected = true;
                const double td = NowMs();
                Tracker::SampleNv12(image.y.data(), image.uv.data(), w, h, Tracker::LetterboxRoi(w, h), Tracker::kDetectorSize, 2.0f, -1.0f,
                                    m_detIn.data());
                float* outs[2] = {m_reg.data(), m_logits.data()};
                Run(M.det, m_detIn.data(), outs);
                Tracker::DecodeDetections(m_reg.data(), m_logits.data(), M.anchors, m_opt.minDetection, m_dets);
                Tracker::Detection best;
                out.ms[0] = static_cast<float>(NowMs() - td);
                if (!Tracker::BestWeighted(m_dets, Tracker::kNmsIou, best))
                {
                    out.ms[3] = static_cast<float>(NowMs() - t0);
                    Note(out);
                    return true;
                }
                out.detectionScore = best.score;
                m_roi = Tracker::RoiFromDetection(best, w, h);
            }
            const double tl = NowMs();
            out.roi = m_roi; // this image's crop (m_roi becomes the next image's below)
            Tracker::SampleNv12(image.y.data(), image.uv.data(), w, h, m_roi, Tracker::kLandmarkSize, 1.0f, 0.0f, m_meshIn.data(),
                                Tracker::Border::Replicate);
            float presenceLogit = 0.0f, other = 0.0f;
            float* outs[3] = {m_raw.data(), &presenceLogit, &other};
            Run(M.mesh, m_meshIn.data(), outs);
            out.presence = 1.0f / (1.0f + std::exp(-std::clamp(presenceLogit, -80.0f, 80.0f)));
            out.ms[1] = static_cast<float>(NowMs() - tl);
            if (!(out.presence >= m_opt.minPresence))
            {
                Reset();
                out.ms[3] = static_cast<float>(NowMs() - t0);
                Note(out);
                return true;
            }
            Tracker::ProjectLandmarks(m_raw.data(), m_roi, out.landmarks);
            m_roi = Tracker::RoiFromLandmarks(out.landmarks); // the next image's region, from the raw landmarks
            m_tracking = true;
            if (m_opt.smooth) m_smoother.Apply(tUs, out.landmarks);
            const double tb = NowMs();
            for (int i = 0; i < Tracker::kBlendshapeInputs; ++i)
            {
                const float* p = out.landmarks + static_cast<size_t>(Tracker::kBlendshapeLandmarks[static_cast<size_t>(i)]) * 3;
                m_blendIn[static_cast<size_t>(i) * 2] = p[0];
                m_blendIn[static_cast<size_t>(i) * 2 + 1] = p[1];
            }
            float* bouts[1] = {out.blendshapes};
            Run(M.blend, m_blendIn.data(), bouts);
            for (float& v : out.blendshapes) v = std::clamp(v, 0.0f, 1.0f);
            out.ms[2] = static_cast<float>(NowMs() - tb);
            out.head = Tracker::HeadFromLandmarks(out.landmarks, w, h, M.ids.data(), M.canonical.data(), M.weights.data(),
                                                  static_cast<int>(M.ids.size()));
            out.found = true;
        }
        catch (const Ort::Exception& e)
        {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            m_lastError = std::string("ONNX Runtime: ") + e.what();
            Reset();
            return false;
        }
        out.ms[3] = static_cast<float>(NowMs() - t0);
        Note(out);
        return true;
    }

    void Reset() override
    {
        m_tracking = false;
        m_smoother.Reset();
    }

    std::string Describe() const override
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "mediapipe on onnxruntime %s, %d thread(s): detector %.1f ms, landmarks %.1f ms, blendshapes %.1f ms",
                      m_models->version.c_str(), m_models->threads, m_avg[0].load(), m_avg[1].load(), m_avg[2].load());
        return buf;
    }

    std::string LastError() const override
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

private:
    // One float input, every output into the caller's buffers (no allocation per run).
    static void Run(const Model& m, float* input, float* const* outputs)
    {
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto count = [](const std::vector<int64_t>& s) {
            size_t n = 1;
            for (int64_t d : s) n *= static_cast<size_t>(d);
            return n;
        };
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, input, count(m.inShapes[0]), m.inShapes[0].data(), m.inShapes[0].size());
        std::vector<Ort::Value> outs;
        outs.reserve(m.outShapes.size());
        for (size_t i = 0; i < m.outShapes.size(); ++i)
            outs.push_back(Ort::Value::CreateTensor<float>(mem, outputs[i], count(m.outShapes[i]), m.outShapes[i].data(), m.outShapes[i].size()));
        m.session->Run(Ort::RunOptions{nullptr}, m.inPtrs.data(), &in, 1, m.outPtrs.data(), outs.data(), outs.size());
    }

    // Running averages of the stage times, for Describe (a source's detail).
    void Note(const FaceTrackerResult& r)
    {
        for (int k = 0; k < 3; ++k)
            if (r.ms[k] > 0.0f) m_avg[k].store(m_avg[k].load() * 0.9 + 0.1 * static_cast<double>(r.ms[k]));
    }

    std::shared_ptr<SharedModels> m_models;
    FaceTrackerOptions m_opt;
    std::vector<Tracker::Detection> m_dets;
    std::vector<float> m_detIn, m_meshIn, m_blendIn, m_reg, m_logits, m_raw;
    Tracker::Roi m_roi;
    bool m_tracking = false;
    Tracker::LandmarkSmoother m_smoother;
    std::atomic<double> m_avg[3] = {};
    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ONNX Runtime's own log lines, into the engine's (its errors as warnings: the tracker's failure says the rest).
void ORT_API_CALL OrtLog(void*, OrtLoggingLevel severity, const char* category, const char*, const char*, const char* message)
{
    if (severity >= ORT_LOGGING_LEVEL_WARNING)
        LOG_WARNING("[FACETRACK] onnxruntime %s: %s", category ? category : "", message ? message : "");
}

Runtime& GetRuntime()
{
    static Runtime* s_runtime = new Runtime(); // never destroyed (see the top of the file)
    return *s_runtime;
}

std::string SystemMessage(DWORD code)
{
    char* text = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                   code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string s = (n && text) ? std::string(text, n) : std::string();
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s;
}

// Loads onnxruntime.dll once, from the exe's own folder (a Windows 11 System32 copy must never be the one),
// checks that it serves this build's API version, and makes the process's one Env.
bool LoadRuntime(std::string& why)
{
    Runtime& r = GetRuntime();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.tried)
    {
        why = r.why;
        return r.loaded;
    }
    r.tried = true;
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::wstring dll = dir + L"onnxruntime.dll";
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        const DWORD code = GetLastError();
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
            r.why = "onnxruntime.dll is not beside the exe (" + Utf8(dll) + "; tools/vendor_onnxruntime.py, then a build)";
        else // there, but Windows would not load it: damaged, another architecture, a dependency missing, no access
            r.why = "onnxruntime.dll beside the exe would not load (" + Utf8(dll) + ": error " + std::to_string(code) + ", " +
                    SystemMessage(code) + ")";
        why = r.why;
        return false;
    }
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto getBase = reinterpret_cast<GetApiBase>(GetProcAddress(module, "OrtGetApiBase"));
    const OrtApiBase* base = getBase ? getBase() : nullptr;
    const OrtApi* api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api)
    {
        r.why = std::string("onnxruntime.dll ") + (base ? base->GetVersionString() : "(no OrtGetApiBase)") +
                " does not serve this build's API version " + std::to_string(ORT_API_VERSION);
        why = r.why;
        return false;
    }
    Ort::InitApi(api);
    r.version = base->GetVersionString();
    try
    {
        r.env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FaceTrack", OrtLog, nullptr);
    }
    catch (const Ort::Exception& e)
    {
        r.why = std::string("ONNX Runtime's environment: ") + e.what();
        why = r.why;
        return false;
    }
    r.loaded = true;
    LOG_INFO("[FACETRACK] tracker: ONNX Runtime %s from %s", r.version.c_str(), Utf8(dll).c_str());
    return true;
}

bool LoadGeometry(const std::string& text, SharedModels& m, std::string& error)
{
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("procrustes") || !j["procrustes"].is_array())
    {
        error = "face_geometry.json is not the canonical face's procrustes basis";
        return false;
    }
    for (const nlohmann::json& e : j["procrustes"])
    {
        if (!e.is_array() || e.size() != 5) continue;
        bool ok = e[0].is_number_unsigned();
        for (size_t k = 1; ok && k < 5; ++k) ok = e[k].is_number();
        if (!ok || e[0].get<uint64_t>() >= static_cast<uint64_t>(Tracker::kLandmarks)) continue;
        m.ids.push_back(static_cast<uint16_t>(e[0].get<uint64_t>()));
        m.weights.push_back(e[1].get<float>());
        for (size_t k = 2; k < 5; ++k) m.canonical.push_back(e[k].get<float>());
    }
    if (m.ids.size() < 4)
    {
        error = "face_geometry.json has fewer than 4 procrustes landmarks";
        return false;
    }
    return true;
}

// The session, its names, and that its tensors have the shapes this pipeline is written for.
bool Open(Model& m, const std::vector<uint8_t>& bytes, const Ort::SessionOptions& so, const char* name,
          const std::vector<std::vector<int64_t>>& ins, const std::vector<std::vector<int64_t>>& outs, std::string& error)
{
    m.session = std::make_unique<Ort::Session>(*GetRuntime().env, bytes.data(), bytes.size(), so);
    Ort::AllocatorWithDefaultOptions alloc;
    if (m.session->GetInputCount() != ins.size() || m.session->GetOutputCount() != outs.size())
    {
        error = std::string(name) + ": " + std::to_string(m.session->GetInputCount()) + " inputs and " +
                std::to_string(m.session->GetOutputCount()) + " outputs, not the model this build reads";
        return false;
    }
    for (size_t i = 0; i < ins.size(); ++i)
    {
        m.inNames.emplace_back(m.session->GetInputNameAllocated(i, alloc).get());
        m.inShapes.push_back(m.session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        if (m.inShapes.back() != ins[i])
        {
            error = std::string(name) + ": input " + std::to_string(i) + " has another shape than this build reads";
            return false;
        }
    }
    for (size_t i = 0; i < outs.size(); ++i)
    {
        m.outNames.emplace_back(m.session->GetOutputNameAllocated(i, alloc).get());
        m.outShapes.push_back(m.session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        if (m.outShapes.back() != outs[i])
        {
            error = std::string(name) + ": output " + std::to_string(i) + " has another shape than this build reads";
            return false;
        }
    }
    for (const std::string& s : m.inNames) m.inPtrs.push_back(s.c_str());
    for (const std::string& s : m.outNames) m.outPtrs.push_back(s.c_str());
    return true;
}

std::shared_ptr<SharedModels> LoadModels(const FaceTrackerOptions& options, std::string& error)
{
    if (!LoadRuntime(error)) return nullptr;
    const double t0 = NowMs();
    auto m = std::make_shared<SharedModels>();
    m->threads = std::clamp(options.threads, 1, 8);
    std::vector<uint8_t> det, mesh, blend;
    std::string geometry;
    auto read = [&](const char* name, std::vector<uint8_t>& out) {
        if (Resource::AssetFS::Get().ReadFile(std::string(kModelDir) + name, out) && !out.empty()) return true;
        error = std::string("no ") + kModelDir + name + " (tools/face_models.py convert)";
        return false;
    };
    if (!read("face_detector.onnx", det) || !read("face_landmarks.onnx", mesh) || !read("face_blendshapes.onnx", blend)) return nullptr;
    if (!Resource::AssetFS::Get().ReadFileText(std::string(kModelDir) + "face_geometry.json", geometry))
    {
        error = std::string("no ") + kModelDir + "face_geometry.json";
        return nullptr;
    }
    if (!LoadGeometry(geometry, *m, error)) return nullptr;
    try
    {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(m->threads);
        so.SetInterOpNumThreads(1);
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Idle intra-op threads sleep: spinning would take cores from the TaskSystem's workers.
        so.AddConfigEntry("session.intra_op.allow_spinning", "0");
        if (!Open(m->det, det, so, "face_detector.onnx", {{1, Tracker::kDetectorSize, Tracker::kDetectorSize, 3}},
                  {{1, Tracker::kAnchors, Tracker::kDetectorCoords}, {1, Tracker::kAnchors, 1}}, error) ||
            !Open(m->mesh, mesh, so, "face_landmarks.onnx", {{1, Tracker::kLandmarkSize, Tracker::kLandmarkSize, 3}},
                  {{1, 1, 1, Tracker::kLandmarks * 3}, {1, 1, 1, 1}, {1, 1}}, error) ||
            !Open(m->blend, blend, so, "face_blendshapes.onnx", {{1, Tracker::kBlendshapeInputs, 2}}, {{Tracker::kBlendshapes}}, error))
            return nullptr;
    }
    catch (const Ort::Exception& e)
    {
        error = std::string("ONNX Runtime: ") + e.what();
        return nullptr;
    }
    m->anchors = Tracker::MakeAnchors();
    m->version = GetRuntime().version;
    LOG_INFO("[FACETRACK] tracker: models loaded in %.0f ms (%d intra-op thread(s) each)", NowMs() - t0, m->threads);
    return m;
}

ModelCache& Cache()
{
    static ModelCache* s_cache = new ModelCache(); // never destroyed (see the top of the file)
    return *s_cache;
}

std::shared_ptr<SharedModels> AcquireModels(const FaceTrackerOptions& options, bool wait, bool& pending, std::string& error)
{
    ModelCache& c = Cache();
    std::unique_lock<std::mutex> lock(c.mutex);
    pending = false;
    if (c.state == ModelCache::State::Idle)
    {
        c.state = ModelCache::State::Loading;
        if (c.loader.joinable()) c.loader.join();
        c.loader = std::thread([options] {
            std::string why;
            std::shared_ptr<SharedModels> m = LoadModels(options, why);
            ModelCache& cache = Cache();
            {
                std::lock_guard<std::mutex> guard(cache.mutex);
                cache.models = m;
                cache.error = why;
                cache.state = m ? ModelCache::State::Ready : ModelCache::State::Failed;
            }
            cache.cv.notify_all();
        });
    }
    if (c.state == ModelCache::State::Loading)
    {
        if (!wait)
        {
            pending = true;
            return nullptr;
        }
        c.cv.wait(lock, [&c] { return c.state != ModelCache::State::Loading; });
    }
    if (c.state == ModelCache::State::Failed)
    {
        error = c.error;
        return nullptr;
    }
    return c.models;
}

#endif

} // namespace

std::unique_ptr<IFaceTracker> MakeFaceTracker(const FaceTrackerOptions& options, bool wait, bool& pending, std::string& error)
{
#if ENGINE_WITH_ONNXRUNTIME
    std::shared_ptr<SharedModels> models = AcquireModels(options, wait, pending, error);
    if (!models) return nullptr;
    return std::make_unique<MediaPipeTracker>(std::move(models), options);
#else
    (void)options;
    (void)wait;
    (void)NowMs;
    (void)kModelDir;
    pending = false;
    error = "the built-in tracker is not in this build (ONNX Runtime's headers were absent: tools/vendor_onnxruntime.py)";
    return nullptr;
#endif
}

void ReleaseFaceTrackerModels()
{
#if ENGINE_WITH_ONNXRUNTIME
    ModelCache& c = Cache();
    std::thread loader;
    std::shared_ptr<SharedModels> models;
    {
        std::unique_lock<std::mutex> lock(c.mutex);
        c.cv.wait(lock, [&c] { return c.state != ModelCache::State::Loading; });
        loader = std::move(c.loader);
        models = std::move(c.models);
        c.state = ModelCache::State::Idle;
        c.error.clear();
    }
    if (loader.joinable()) loader.join();
    // The sessions go here, if no tracker is left holding them - and then the Env, which must outlive
    // every session. A tracker still holding them (none should, after Shutdown's sources) keeps both.
    const bool held = models && models.use_count() > 1;
    models.reset();
    if (held) return;
    Runtime& r = GetRuntime();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.env.reset();
    r.tried = r.loaded = false;
    r.why.clear();
#endif
}

} // namespace FaceTracking
