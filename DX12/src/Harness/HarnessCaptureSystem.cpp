#include <filesystem>
#include "Harness/HarnessCaptureSystem.h"

#ifdef WITH_HARNESS
#include "Harness/AttachmentDecode.h"
#include "Harness/CaptureRequests.h"
#include "Harness/ControlChannelJobs.h"
#include "Harness/GameFlowDump.h"
#include "Harness/RenderGraphDump.h"
#include "Harness/RunLedger.h"
#include "Harness/SettingsStoreScript.h"
#include "Harness/WorldStateDump.h"
#include "Harness/ExitCodes.h"

#include "ECS/ECS.h"
#include "ECS/Components.h"     // CameraComponent: a named-camera capture checks it
#include "ECS/FrameContext.h"   // FrameContext: ISystem.h only forward-declares it
#include "Renderer/Renderer.h"
#include "Physics/ChainPhysicsSystem.h" // -provenance_settings: culling.chainPhysicsLod, read live
#include "RenderGraph/FrameCapture.h"   // -rgdump: RG::FrameCaptures
#include "RenderGraph/RenderPass/AutoExposurePass.h"   // the nanPixels readback
#include "Graphics/PipelineStats.h"                     // compile / PSO totals
#include "Graphics/UploadStats.h"                       // upload-wait totals
#include "UI/GlyphStats.h"                             // glyph bake totals
#include "UI/FontLibrary.h"                            // the [FONT_BAKE] summary
#include "Renderer/GatherGateStats.h"                   // the retained-gather gates
#include "Resource/PrefabSpawn.h"                       // runtime spawn totals
#include "Resource/PrefabSpawnQueue.h"                  // async spawn totals
#include "Resource/EngineSettings.h"
#include "Resource/EngineDevSettings.h"   // -enginejson_roundtrip
#include "Graphics/ShaderLibrary.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/HDROutput.h"   // HDROutputModeToString for run.json
#include "Diagnostics/FrameTiming.h"  // split baseline rows in frames.jsonl
#include "Diagnostics/ProfilerFrame.h" // engine profiler headline rows
#include "Renderer/UIView.h"          // -ui_render
#include "Renderer/UIViewService.h"
#include "ECS/HierarchyComponents.h"   // SceneTransientTag (-ui_render's spawn)
#include "ECS/HierarchyOps.h"          // Hierarchy::DestroySubtree
#include "UI/UIAsset.h"
#include "UI/UICanvas.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost: the host named in an app_ui capture
#include "System/PathUtil.h"
#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "DirectXTex.h"          // WIC PNG I/O: the object-ID image and the app_ui panel crop
#include <nlohmann/json.hpp>

namespace Harness
{


HarnessCaptureSystem::HarnessCaptureSystem(Renderer& renderer, IGraphicsDevice& gfx)
    : m_renderer(renderer), m_gfx(gfx)
{
    const auto& cl = Sys::CommandLine::Get();
    m_enabled      = Sys::HasArtifactDir();
    m_every        = cl.Has("-framelog_every") ? cl.GetInt("-framelog_every", 1) : 1;
    m_requireDraws = cl.Has("-require_draws");

    m_screenshotPath = cl.Get("-screenshot");
    const int captureCount = cl.GetInt("-screenshot_count", 1);
    const int captureEvery = cl.GetInt("-screenshot_every", 0);
    ENGINE_CHECK(captureCount >= 1 && captureCount <= 256 && captureEvery >= 0,
                 "Invalid bounded screenshot sequence");
    ENGINE_CHECK(captureCount == 1 || (!m_screenshotPath.empty() && captureEvery > 0),
                 "Screenshot sequence requires -screenshot and positive -screenshot_every");
    m_captureEvery = static_cast<uint32_t>(captureEvery);
    m_captureRemaining = static_cast<uint32_t>(captureCount-1);
    m_dumpStatePath  = cl.Get("-dumpstate");
    m_dumpFlowPath   = cl.Get("-dumpflow");
    m_previewShotPath = cl.Get("-preview_screenshot");
    // A bare flag is an error, not a no-op: `Get` returns the empty fallback for a bare
    // -preview_screenshot, so a typo would capture nothing and only the pack's
    // artifactsExist would notice - after the whole run, about a missing file.
    ENGINE_CHECK(!cl.Has("-preview_screenshot") || !m_previewShotPath.empty(),
                 "-preview_screenshot needs a value (-preview_screenshot=<file.png>); "
                 "a bare flag captures nothing.");

    // -at_frame defaults to the last frame of a bounded run ("a picture of the result");
    // frame 0 would capture the boot frame, before the scene has loaded.
    const int frames = cl.GetInt("-frames", 0);
    const int defaultAt = (frames > 1) ? (frames - 1) : 0;
    m_atFrame = static_cast<std::uint64_t>(cl.GetInt("-at_frame", defaultAt));

    // Second capture slot. -at_frame2 has no default: the slot exists to compare two
    // moments, and a fallback to the first frame would write identical files that every
    // imagePair passes. A bare flag, a missing -at_frame2 and -at_frame2 == -at_frame are
    // hard preconditions (m_configRefused, exit 12), not ENGINE_CHECKs: a check logs and
    // runs on with the slot dropped, so the pack would fail later on a missing file. App
    // reads m_configRefused right after registration (RegisterTickSystems), on the main
    // thread.
    m_screenshot2Path = cl.Get("-screenshot2");
    if (cl.Has("-screenshot2") && m_screenshot2Path.empty())
    {
        LOG_ERROR("-screenshot2 needs a value (-screenshot2=<file.png>); "
                  "a bare flag captures nothing, and a pack that declares an "
                  "imagePair over it would compare a file that is never "
                  "written. Refusing to run.");
        m_configRefused = true;
    }
    // The preview half of the second slot: the same preconditions and exit 12.
    m_previewShot2Path = cl.Get("-preview_screenshot2");
    if (cl.Has("-preview_screenshot2") && m_previewShot2Path.empty())
    {
        LOG_ERROR("-preview_screenshot2 needs a value "
                  "(-preview_screenshot2=<file.png>); a bare flag captures "
                  "nothing, and a pack that declares an imagePair over it would "
                  "compare a file that is never written. Refusing to run.");
        m_configRefused = true;
    }
    if (!m_screenshot2Path.empty() || !m_previewShot2Path.empty())
    {
        if (!cl.Has("-at_frame2"))
        {
            LOG_ERROR("-screenshot2 / -preview_screenshot2 need -at_frame2=<n>: "
                      "the second slot exists to capture a DIFFERENT moment, and "
                      "without its own frame it would write a duplicate of the "
                      "first slot that any imagePair passes. Refusing to run.");
            m_configRefused = true;
            m_screenshot2Path.clear();
            m_previewShot2Path.clear();
        }
        else
        {
            m_atFrame2 = static_cast<std::uint64_t>(cl.GetInt("-at_frame2", 0));
            if (m_atFrame2 == m_atFrame)
            {
                LOG_ERROR("-at_frame2=%llu is the same frame as -at_frame; the "
                          "two captures would be the same picture and every "
                          "imagePair over them would pass having compared "
                          "nothing. Refusing to run.",
                          static_cast<unsigned long long>(m_atFrame2));
                m_configRefused = true;
                m_screenshot2Path.clear();
                m_previewShot2Path.clear();
            }
        }
    }
    // -rgdump=<file.json>[@<frame>] (see ServiceRgDump). Refused like -screenshot2 when
    // bare or malformed, so a typo fails before the run, not on a missing file after it.
    if (cl.Has("-rgdump"))
    {
        const std::string spec = cl.Get("-rgdump");
        const std::size_t at   = spec.rfind('@');
        std::string       file = at == std::string::npos ? spec : spec.substr(0, at);
        bool ok = !file.empty();
        if (ok && at != std::string::npos)
        {
            const std::string fs = spec.substr(at + 1);
            char* end = nullptr;
            const unsigned long long f = fs.empty() ? 0ull : std::strtoull(fs.c_str(), &end, 10);
            ok = !fs.empty() && end && *end == '\0';
            if (ok) m_rgDumpAt = static_cast<std::uint64_t>(f);
        }
        if (!ok)
        {
            LOG_ERROR("-rgdump needs <file.json>[@<frame>] (got '%s'); a malformed value "
                      "writes no graph snapshot. Refusing to run.", spec.c_str());
            m_configRefused = true;
        }
        else if (!m_enabled)
            LOG_WARNING("-rgdump=%s needs -artifacts=<dir>: the snapshot is written there; "
                        "ignored", spec.c_str());
        else
            m_rgDumpPath = std::move(file);
    }
    // -probedump[=<dir>] (see ServiceProbeDumps).
    if (cl.Has("-probedump"))
    {
        const std::string sub = cl.Get("-probedump");
        if (!m_enabled)
            LOG_WARNING("-probedump needs -artifacts=<dir>: the probe faces are written there; ignored");
        else
            m_probeDumpDir = Sys::ArtifactDir() + "/" + (sub.empty() ? std::string("probes") : sub);
    }

    m_drawGrace    = static_cast<std::uint64_t>(
        cl.GetInt("-require_draws", static_cast<int>(kDefaultDrawGrace)));

    if (m_every < 1) m_every = 1;
    if (!m_enabled)  return;

    // -settings_roundtrip=<dir> / -enginejson_roundtrip=<dir>: every *.json directly in
    // <dir>, in name order, loaded and serialized again into <artifacts>/<sub>/<name>. They
    // pin the bytes settings.json's and engine.json's IO produce
    // (tests/unit/test_settings_roundtrip.py, test_enginejson_roundtrip.py). Pure IO: the
    // struct is a local, nothing is applied, no project file is touched.
    auto roundTrip = [&](const char* flag, const char* sub, const char* tag,
                         bool (*one)(const std::string& path, std::string& text))
    {
        const std::string dir = cl.Get(flag);
        if (dir.empty())
            return;
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<fs::path> inputs;
        for (const auto& e : fs::directory_iterator(Sys::U8Path(dir), ec))
            if (e.is_regular_file() && e.path().extension() == ".json")
                inputs.push_back(e.path());
        std::sort(inputs.begin(), inputs.end());
        if (ec || inputs.empty())
        {
            LOG_ERROR("%s=%s: no *.json files there%s%s. Refusing to run.",
                      flag, dir.c_str(), ec ? ": " : "", ec ? ec.message().c_str() : "");
            m_configRefused = true;
        }
        for (const fs::path& in : inputs)
        {
            std::string text;
            const bool loaded = one(Sys::PathToUtf8(in), text);
            const std::string name = Sys::PathToUtf8(in.filename());
            const std::string out  = Sys::ArtifactDir() + "/" + sub + "/" + name;
            Sys::CreateParentDirs(out);
            auto f = Sys::OpenOfstream(out, std::ios::binary);
            if (f) f.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!f)
            {
                LOG_ERROR("%s: cannot write '%s'", flag, out.c_str());
                m_configRefused = true;
                continue;
            }
            LOG_INFO("[%s] file=%s loaded=%d bytes=%zu", tag, name.c_str(), loaded ? 1 : 0, text.size());
        }
    };
    roundTrip("-settings_roundtrip", "settings_roundtrip", "SETTINGS_ROUNDTRIP",
              [](const std::string& path, std::string& text) {
                  // A load and the save that follows it, recording the rows the load counted as the
                  // player's choices (PlayerRowsToRecord).
                  Resource::EngineSettings s;
                  Resource::SettingsLoadInfo info;
                  const bool loaded = Resource::LoadEngineSettings(path, s, &info);
                  text = Resource::SerializeEngineSettings(s, Resource::PlayerRowsToRecord(s, info));
                  return loaded;
              });
    roundTrip("-enginejson_roundtrip", "enginejson_roundtrip", "ENGINEJSON_ROUNDTRIP",
              [](const std::string& path, std::string& text) {
                  Resource::EngineDevSettings s;
                  const bool loaded = Resource::LoadEngineDevSettingsFile(path, s);
                  text = Resource::SerializeEngineDevSettings(s);
                  return loaded;
              });

    // -settings_storetest=<ops.jsonl>: the settings store's layers, policies and
    // validation, driven by a script against a store of its own
    // (Harness/SettingsStoreScript.h); results to <artifacts>/settings_store/results.jsonl
    // (tests/unit/test_settings_store.py).
    if (const std::string ops = cl.Get("-settings_storetest"); !ops.empty())
    {
        const std::string out = Sys::ArtifactDir() + "/settings_store/results.jsonl";
        std::string error;
        if (!RunSettingsStoreScript(ops, out, error))
        {
            LOG_ERROR("-settings_storetest=%s: %s. Refusing to run.", ops.c_str(), error.c_str());
            m_configRefused = true;
        }
        else
            LOG_INFO("[SETTINGS_STORETEST] ops=%s results=%s", ops.c_str(), out.c_str());
    }

    const std::string path = Sys::ArtifactDir() + "/frames.jsonl";

    // Plain ofstream, not the Logger: the Logger is asynchronous and its crash path has a
    // lock order (m_writeMutex -> m_mutex) a second sink would risk deadlocking, and this
    // data is main-thread-only and already final here.
    // No exception mask: a failure sets the fail bit instead of throwing (see the
    // never-throw note in the header).
    m_out = Sys::OpenOfstream(path, std::ios::out | std::ios::trunc);
    m_opened = m_out.is_open();

    if (!ENGINE_CHECK(m_opened, "HarnessCaptureSystem: cannot open '%s' for writing; "
                                "no per-frame record will be produced", path.c_str()))
    {
        m_enabled = false;
        return;
    }

    // A bare filename means "inside this run's artifact directory", so callers need not
    // repeat the directory in every path flag.
    auto anchor = [](std::string& path)
    {
        if (path.empty()) return;
        const bool absolute = path.size() > 1 &&
                              (path[1] == ':' || path[0] == '/' || path[0] == 92);
        if (!absolute) path = Sys::ArtifactDir() + "/" + path;
    };
    anchor(m_screenshotPath);
    anchor(m_dumpStatePath);
    anchor(m_dumpFlowPath);
    anchor(m_previewShotPath);
    anchor(m_screenshot2Path);
    anchor(m_previewShot2Path);
    anchor(m_rgDumpPath);
    if (!m_rgDumpPath.empty())
        LOG_INFO("Harness: render graph snapshot asked at frame %llu -> %s (-rgdump)",
                 static_cast<unsigned long long>(m_rgDumpAt), m_rgDumpPath.c_str());

    // -ui_screenshot=<png>@<frame>[,...]: the presented back buffer, in either host (Game.exe's is the
    // picture the player sees, after the present shader). A missing value is refused by
    // Editor::PreviewHarness (exit 12, before a frame); a malformed entry fails an ENGINE_CHECK and
    // is skipped.
    if (cl.Has("-ui_screenshot"))
    {
        const std::string spec = cl.Get("-ui_screenshot");
        std::size_t pos = 0;
        while (pos < spec.size())
        {
            std::size_t comma = spec.find(',', pos);
            if (comma == std::string::npos) comma = spec.size();
            const std::string item = spec.substr(pos, comma - pos);
            pos = comma + 1;
            if (item.empty()) continue;
            const std::size_t at = item.rfind('@');
            char* end = nullptr;
            const std::string fs = at == std::string::npos ? std::string() : item.substr(at + 1);
            const unsigned long long f = fs.empty() ? 0ull : std::strtoull(fs.c_str(), &end, 10);
            const bool ok = at != std::string::npos && at > 0 && !fs.empty() && end && *end == '\0';
            if (!ENGINE_CHECK(ok, "-ui_screenshot item '%s' is not <file.png>@<frame>; skipped.",
                              item.c_str()))
                continue;
            UiShot s;
            s.path = item.substr(0, at);
            s.at   = static_cast<std::uint64_t>(f);
            anchor(s.path);
            m_uiShots.push_back(s);
        }
        if (!m_uiShots.empty())
            LOG_INFO("Harness: %zu UI screenshot(s) requested (-ui_screenshot)", m_uiShots.size());
    }

    // -ui_render=<x.iui>[:<W>x<H>][:<out.png>] (a relative asset path: ':' separates the
    // parts). Both hosts: the view service is EngineCore's.
    if (cl.Has("-ui_render"))
    {
        const std::string spec = cl.Get("-ui_render");
        UiRender u;
        u.path = "ui_render.png";
        bool ok = !spec.empty();
        std::size_t pos = 0;
        for (int part = 0; ok && pos <= spec.size(); ++part)
        {
            std::size_t colon = spec.find(':', pos);
            if (colon == std::string::npos) colon = spec.size();
            const std::string item = spec.substr(pos, colon - pos);
            pos = colon + 1;
            unsigned w = 0, h = 0;
            char tail = 0;
            if (part == 0) u.asset = item;
            // A size outside 16..UIViewService::kMaxSize is refused: the view service makes no
            // bigger target.
            else if (sscanf_s(item.c_str(), "%ux%u%c", &w, &h, &tail, 1) == 2 && w >= 16 && h >= 16 &&
                     w <= UIViewService::kMaxSize && h <= UIViewService::kMaxSize)
            {
                u.w = w;
                u.h = h;
            }
            else if (item.size() > 4 && item.compare(item.size() - 4, 4, ".png") == 0) u.path = item;
            else ok = false;
        }
        ok = ok && u.asset.size() > 4 && u.asset.compare(u.asset.size() - 4, 4, ".iui") == 0;
        if (!ok)
        {
            LOG_ERROR("-ui_render needs <x.iui>[:<W>x<H>][:<out.png>], W and H 16..%u (got '%s'). Refusing to run.",
                      UIViewService::kMaxSize, spec.c_str());
            m_configRefused = true;
        }
        else
        {
            anchor(u.path);
            m_uiRender = std::move(u);
            LOG_INFO("Harness: -ui_render %s at %ux%u -> %s", m_uiRender.asset.c_str(), m_uiRender.w, m_uiRender.h,
                     m_uiRender.path.c_str());
        }
    }

    if (!m_screenshotPath.empty() || !m_dumpStatePath.empty() ||
        !m_dumpFlowPath.empty()   || !m_previewShotPath.empty())
        LOG_INFO("Harness: one-shot capture at frame %llu%s%s%s%s",
                 static_cast<unsigned long long>(m_atFrame),
                 m_screenshotPath.empty() ? "" : " [screenshot]",
                 m_dumpStatePath.empty()  ? "" : " [state dump]",
                 m_dumpFlowPath.empty()   ? "" : " [flow dump]",
                 m_previewShotPath.empty()? "" : " [preview shot]");
    if (!m_screenshot2Path.empty() || !m_previewShot2Path.empty())
        LOG_INFO("Harness: second capture at frame %llu%s%s",
                 static_cast<unsigned long long>(m_atFrame2),
                 m_screenshot2Path.empty()  ? "" : " [main viewport]",
                 m_previewShot2Path.empty() ? "" : " [preview shot]");

    LOG_INFO("Harness: per-frame record -> %s (every %d frame(s)%s)",
             path.c_str(), m_every, m_requireDraws ? ", -require_draws" : "");

    // The pipe's `capture` verb files requests this system takes at PostRender
    // (CaptureRequests.h); it refuses them while no service exists.
    CaptureRequests::SetServiced(true);
}

HarnessCaptureSystem::~HarnessCaptureSystem()
{
    CaptureRequests::SetServiced(false);
    if (!m_opened) return;
    m_out.flush();
    m_out.close();
}

void HarnessCaptureSystem::OnUnregister(World& /*world*/)
{
    // Called from App::ShutdownAllSystems -> SystemRegistry::Shutdown: the frame loop is
    // over and the fonts are still open (FontLibrary lives to process exit). A next-frame
    // render the last flush handed off is finished first, so the summary counts every cell.
    if (!m_enabled) return;
    UI::FontLibrary::Get().FinishAsyncRender();
    UI::FontLibrary::Get().LogBakeSummary();
}

void HarnessCaptureSystem::Update(World& world, const FrameContext& ctx)
{
    if (!m_enabled || m_writeFailed) return;

    // frameIndex, not GameModeContext::frame: only FrameContext reaches a system. App copies
    // ctx.frame into frameCtx.frameIndex before the phase dispatch and increments at the
    // bottom of the loop, so in PostRender this is the frame being finished.
    const std::uint64_t frame = ctx.frameIndex;

    // Opt-in provenance: snapshot the live persisted settings contract, with
    // the frame where a change was observed. Never write the source profile.
    if (Sys::CommandLine::Get().Has("-provenance_settings"))
    {
        try
        {
            // The store's values under what the owners report: a row with no capture (a quality
            // level, ui.language) reads the store, not the struct default.
            Resource::EngineSettings settings;
            Resource::ProjectStoreValues(settings);
            settings.CaptureFrom(m_renderer, m_gfx, nullptr);
            auto values = nlohmann::json::parse(Resource::SerializeEngineSettings(settings));
            // No Window is available here: omit unobserved window defaults.
            for (const char* key : {"fullscreen", "windowWidth", "windowHeight"}) values["display"].erase(key);
            const std::string text = values.dump();
            if (text != m_lastResolvedSettings)
            {
                auto out = Sys::OpenOfstream(Sys::ArtifactDir() + "/settings.resolved.jsonl", std::ios::binary | std::ios::app);
                const auto row = nlohmann::json{{"frame", frame}, {"settings", nlohmann::json::parse(text)},
                    {"scope", "EngineSettings persisted fields; window state in run.json display"}}.dump();
                out << row << '\n';
                out.flush();
                if (out) m_lastResolvedSettings = text;
                else LOG_ERROR("Harness: failed to record resolved settings");
            }
        }
        catch (const std::exception& e) { LOG_ERROR("Harness: resolved settings: %s", e.what()); }

        // engine.json's half: what EngineDevSettings::CaptureFrom reads back, plus the one
        // value it applies but never captures (culling.chainPhysicsLod), read live. Keys
        // CaptureFrom leaves alone stay at their defaults.
        try
        {
            Resource::EngineDevSettings dev;
            dev.CaptureFrom(m_renderer);
            auto values = nlohmann::json::parse(Resource::SerializeEngineDevSettings(dev));
            if (ChainPhysicsSystem* cps = m_renderer.GetChainPhysicsSystem())
                values["applied"]["culling.chainPhysicsLod"] = cps->lodEnabled;
            const std::string text = values.dump();
            if (text != m_lastResolvedEngine)
            {
                auto out = Sys::OpenOfstream(Sys::ArtifactDir() + "/engine.resolved.jsonl", std::ios::binary | std::ios::app);
                const auto row = nlohmann::json{{"frame", frame}, {"engine", values},
                    {"scope", "EngineDevSettings::CaptureFrom; 'applied' holds what is applied and never captured"}}.dump();
                out << row << '\n';
                out.flush();
                if (out) m_lastResolvedEngine = text;
                else LOG_ERROR("Harness: failed to record resolved engine settings");
            }
        }
        catch (const std::exception& e) { LOG_ERROR("Harness: resolved engine settings: %s", e.what()); }
    }

    // Record the RESOLVED display configuration whenever it changes, so run.json keeps the
    // last one - in effect when the screenshot was taken. On the first frame the swap chain
    // has not settled (it can report 1280x720 for a 1920x1080 PNG). Comparing the cached
    // scalars per frame is free; taking the mutex per frame would not be.
    {
        const HDROutputMode mode = m_gfx.MainSurface().GetHDROutputMode();
        const std::uint32_t ow = m_gfx.GetWidth();
        const std::uint32_t oh = m_gfx.GetHeight();
        const std::uint32_t rw = m_gfx.GetRenderWidth();
        const std::uint32_t rh = m_gfx.GetRenderHeight();
        if (!m_displayNoted || mode != m_lastHdrMode ||
            ow != m_lastOutputW || oh != m_lastOutputH ||
            rw != m_lastRenderW || rh != m_lastRenderH)
        {
            m_displayNoted = true;
            m_lastHdrMode  = mode;
            m_lastOutputW  = ow;
            m_lastOutputH  = oh;
            m_lastRenderW  = rw;
            m_lastRenderH  = rh;
            RunLedger::Get().NoteDisplay(
                HDROutputModeToString(mode),
                mode == HDROutputMode::SDR ? "R8G8B8A8_UNORM"
                                           : "R16G16B16A16_FLOAT",
                ow, oh, rw, rh, m_gfx.MainSurface().GetPresentSettings().renderScale);
        }
    }

    // frames.jsonl's nanPixels column is AutoExposure's readback, which runs only while its
    // TTL is renewed - here, every frame (an 8-byte copy and one barrier; the image is
    // unchanged).
    if (AutoExposurePass* ae = m_renderer.GetAutoExposurePass())
        ae->SetDebugReadbackEnabled(true);

    // BEFORE the sampling early-out: a -framelog_every=10 run must still
    // take its screenshot on the exact frame asked for.
    ServiceOneShots(world, frame);
    ServiceSecondShot(frame);   // -screenshot2 / -preview_screenshot2: own frame, own latch
    ServiceRgDump(frame);       // -rgdump: ask on one frame, write when published
    ServiceProbeDumps(frame);   // -probedump: a baked probe's six faces
    ServiceUiShots(frame);      // -ui_screenshot: the whole editor window
    ServiceCaptureRequests(world, frame);   // the pipe's `capture` verb
    ServiceAppUi(frame, /*mayRequest=*/!m_backBufferBeforePost);
    ServiceCameraCaptures(world, frame);
    ServiceViewCaptures(frame);
    ServiceUiRender(world, frame);   // -ui_render
    ServicePicks(world, frame);      // the pipe's `pick` verb

    if (m_every > 1 && (frame % static_cast<std::uint64_t>(m_every)) != 0) return;

    WriteFrameRecord(frame);
}

void HarnessCaptureSystem::WriteFrameRecord(std::uint64_t frame)
{
    const Renderer::FrameStats& fs = m_renderer.GetFrameStats();

    // drawPackets counts every packet SortAndEmit emitted, including - while a PiP or the
    // editor's camera preview is live - the secondary-view packets for geometry the main
    // frustum rejected, which only the preview graph draws. So it steps up when a PiP opens
    // without the main view gaining a draw; a pack gating on drawPackets across a PiP toggle
    // must expect that. The indirect / cull-command range (SceneEmit::indirectDrawCount, and
    // visibleDrawCmds with it) excludes them - see the trim at the tail of SortAndEmit.
    if (fs.drawPackets == 0) ++m_emptyFrames;

    // snprintf into fixed buffers, not a stringstream: the record is flat scalars with no
    // user text, and this per-frame loop should not allocate.
    // GPU frame cost: null when unmeasured, never 0.0 (the profiler is off without
    // -gpuprofile, and the first frames have no resolved results) - a zero would read as a
    // free frame. gpuAgeFrames is how many frames old the numbers are (subtract it from
    // "frame"); it tracks in-flight depth, so it is emitted, not assumed. One scalar, not a
    // per-pass table: an overflowing line disables the whole file.
    // Read from FrameTiming, not the device: GetGPUFrameTiming reads non-atomic GPU profiler
    // members the render half writes concurrently with PostRender; FrameTiming carries the
    // same numbers, applied on this thread from the render->game mailbox.
    const auto& _ft      = Diag::FrameTiming::Get();
    const float gpuMs        = _ft.gpuBusyMs;
    const float gpuOverlapMs = _ft.gpuOverlapMs;
    const uint32_t gpuAge    = _ft.gpuAgeFrames;
    const bool haveGpu       = _ft.gpuValid;

    char gpuField[144];
    if (haveGpu)
        std::snprintf(gpuField, sizeof(gpuField),
                      "%.3f,\"gpuOverlapMs\":%.3f,\"gpuAgeFrames\":%u",
                      static_cast<double>(gpuMs),
                      static_cast<double>(gpuOverlapMs), gpuAge);
    else
        std::snprintf(gpuField, sizeof(gpuField),
                      "null,\"gpuOverlapMs\":null,\"gpuAgeFrames\":null");

    // Post-cull survivors: drawPackets is what the CPU submitted, this is what GPU culling
    // let through, and a Hi-Z regression drives this to zero while drawPackets stays
    // healthy. null while GPU compaction is inactive or the readback ring has not primed.
    char visField[32];
    if (fs.visibleDrawCmdsValid)
        std::snprintf(visField, sizeof(visField), "%u", fs.visibleDrawCmds);
    else
        std::snprintf(visField, sizeof(visField), "null");

    // Split baseline rows, read from the one shared timing object so the HUD,
    // the editor Profiler, Lua and this line cannot disagree.
    const auto& ft = Diag::FrameTiming::Get();
    char joinField[32];
    if (ft.joinWaitValid)
        std::snprintf(joinField, sizeof(joinField), "%.3f", static_cast<double>(ft.joinWaitMs));
    else
        std::snprintf(joinField, sizeof(joinField), "null");

    // Absent rather than 0 when the accounting ring no longer holds the frame
    // - the gpuEffectiveMs / joinWaitMs contract, for the same reason.
    char ownerField[32];
    if (ft.renderOwnerValid)
        std::snprintf(ownerField, sizeof(ownerField), "%.3f",
                      static_cast<double>(ft.renderOwnerMs));
    else
        std::snprintf(ownerField, sizeof(ownerField), "null");

    // Engine profiler headline (Diag::Prof), null unless -profile is on. It describes
    // profFrame, the PREVIOUS frame: a frame's profiler window is the main loop period and
    // closes at the next loop top, after this row. The per-thread record and the timeline
    // (all-idle, concurrency) are in <artifacts>/profile/cpu.jsonl, not here - an overflow
    // of this line disables the whole file. vramMB / wsMB come from the collector's
    // per-frame sample, null until it has taken profFrame's; allocs counts operator-new
    // calls on every thread in profFrame's window.
    char profField[384];
    {
        Diag::Prof::FrameRecord pr;
        if (Diag::Prof::GetLatestFrameRecord(pr))
        {
            char memField[96];
            if (pr.memValid)
                std::snprintf(memField, sizeof(memField), "%.1f,\"wsMB\":%.1f",
                              static_cast<double>(pr.vramLocalBytes) / 1048576.0,
                              static_cast<double>(pr.workingSetBytes) / 1048576.0);
            else
                std::snprintf(memField, sizeof(memField), "null,\"wsMB\":null");
            std::snprintf(profField, sizeof(profField),
                "%llu,\"util\":%.4f,\"sMainMs\":%.3f,\"sLaneMs\":%.3f,\"wTotalMs\":%.3f,"
                "\"workerWorkMs\":%.3f,\"mainUntrackedMs\":%.3f,\"allocs\":%u,\"vramMB\":%s",
                static_cast<unsigned long long>(pr.frame),
                static_cast<double>(pr.util), static_cast<double>(pr.sMainMs),
                static_cast<double>(pr.sLaneMs), static_cast<double>(pr.wTotalMs),
                static_cast<double>(pr.workerWorkMs), static_cast<double>(pr.mainUntrackedMs),
                pr.allocs, memField);
        }
        else
            std::snprintf(profField, sizeof(profField),
                "null,\"util\":null,\"sMainMs\":null,\"sLaneMs\":null,\"wTotalMs\":null,"
                "\"workerWorkMs\":null,\"mainUntrackedMs\":null,\"allocs\":null,"
                "\"vramMB\":null,\"wsMB\":null");
    }

    // First-compile observability: shaderCompiles / psoCreates / psoLibMisses are running
    // totals since boot (Graphics/PipelineStats.h); a row where one moved paid for a DXC
    // compile or a driver PSO build that frame (scenario_run.py frames.psoCreatesAfterFrame).
    // nanPixels: the metered pixels AutoExposure rejected as NaN / Inf, kFrameCount frames
    // old; null without a reading (auto exposure off, or the readback not primed).
    char nanField[24];
    {
        const AutoExposurePass* ae = m_renderer.GetAutoExposurePass();
        const int64_t nonFinite = ae ? ae->GetLastNonFinitePixels() : -1;
        if (nonFinite >= 0)
            std::snprintf(nanField, sizeof(nanField), "%lld", static_cast<long long>(nonFinite));
        else
            std::snprintf(nanField, sizeof(nanField), "null");
    }

    // null until the first hand-off has filled a snapshot.
    char settingsRevField[24];
    {
        const Settings::Snapshot& snap = m_renderer.SettingsPublished();
        if (snap.revision != Settings::Snapshot::kNever)
            std::snprintf(settingsRevField, sizeof(settingsRevField), "%llu",
                          static_cast<unsigned long long>(snap.revision));
        else
            std::snprintf(settingsRevField, sizeof(settingsRevField), "null");
    }

    // Runtime prefab spawns and the two retained-gather gates a spawn trips. spawns is a
    // running total; spawnSyncMs the synchronous spawn time since the previous row (this
    // frame's on an every-frame record). gatherFull / ddgiFull: 1 when this frame's gate sent
    // the whole scene to a full gather, with the GatherGateStats::Reason bits that vetoed
    // the patch; null when that gate did not run this frame (no DDGI / TLAS). gatherFulls /
    // ddgiFulls: running totals of full frames, the column a window of frames is gated on
    // (scenario_run.py frames.totalGrowth).
    char spawnField[320];
    {
        const uint64_t spawnNs = Resource::SpawnStats::syncNs.load(std::memory_order_relaxed);
        const double   spawnMs = static_cast<double>(spawnNs - m_lastSpawnNs) / 1.0e6;
        m_lastSpawnNs = spawnNs;
        auto gate = [frame](const GatherGateStats::Gate& g, char* full, char* reason)
        {
            if (g.runs.load(std::memory_order_relaxed) != 0 && g.frame.load(std::memory_order_relaxed) == frame)
            {
                std::snprintf(full, 8, "%u", g.full.load(std::memory_order_relaxed));
                std::snprintf(reason, 16, "%u", g.reason.load(std::memory_order_relaxed));
            }
            else
            {
                std::snprintf(full, 8, "null");
                std::snprintf(reason, 16, "null");
            }
        };
        char candFull[8], candWhy[16], ddgiFull[8], ddgiWhy[16];
        gate(GatherGateStats::cand, candFull, candWhy);
        gate(GatherGateStats::ddgi, ddgiFull, ddgiWhy);
        std::snprintf(spawnField, sizeof(spawnField),
            "\"spawns\":%llu,\"spawnSyncMs\":%.3f,\"gatherFull\":%s,\"gatherFullReason\":%s,"
            "\"gatherFulls\":%llu,\"ddgiFull\":%s,\"ddgiFullReason\":%s,\"ddgiFulls\":%llu,"
            "\"spawnCommits\":%llu,\"spawnPending\":%u,",
            static_cast<unsigned long long>(Resource::SpawnStats::spawns.load(std::memory_order_relaxed)),
            spawnMs, candFull, candWhy,
            static_cast<unsigned long long>(GatherGateStats::cand.fulls.load(std::memory_order_relaxed)),
            ddgiFull, ddgiWhy,
            static_cast<unsigned long long>(GatherGateStats::ddgi.fulls.load(std::memory_order_relaxed)),
            // The asynchronous spawns committed (a running total) and pending now.
            static_cast<unsigned long long>(Resource::PrefabSpawnQueue::AsyncStats().commits),
            Resource::PrefabSpawnQueue::AsyncStats().pending);
    }

    // 3072 bytes: the widest measured row is ~1600 bytes, and an overflow disables the
    // recorder for the rest of the run (the guard below), so headroom is far cheaper than the
    // failure. Re-measure the widest row when adding columns.
    char line[3072];
    const int n = std::snprintf(line, sizeof(line),
        "{\"frame\":%llu,\"cpuMs\":%.3f,\"drawPackets\":%u,\"staticInstSlots\":%u,"
        "\"dynamicInstances\":%u,\"lights\":%u,\"shadersFailed\":%u,"
        "\"engineErrors\":%u,\"visibleDrawCmds\":%s,\"gpuEffectiveMs\":%s,"
        "\"gameMs\":%.3f,\"renderPhaseMs\":%.3f,\"renderThreadMs\":%.3f,\"gpuWaitMs\":%.3f,"
        "\"cpuPeriodMs\":%.3f,\"joinWaitMs\":%s,\"renderMode\":\"%s\","
        "\"previewViews\":%u,\"frameMs\":%.3f,\"drawMs\":%.3f,"
        "\"beginWaitMs\":%.3f,\"presentMs\":%.3f,"
        "\"loopTopMs\":%.3f,\"renderTimingFrame\":%llu,\"renderTimingValid\":%s,"
        "\"gpuCalibrated\":%s,\"renderOwnerMs\":%s,\"tailDrainMs\":%.3f,"
        "\"blockingDrainMs\":%.3f,\"ownerContinuationsPump\":%u,"
        "\"ownerContinuationsDrain\":%u,\"parkHookCount\":%u,"
        "\"statsFrame\":%llu,\"statsValid\":%s,"
        "\"brsPlanMs\":%.3f,\"brsEmitMs\":%.3f,"
        "\"shaderCompiles\":%u,\"psoCreates\":%u,\"psoLibMisses\":%u,\"shaderMisses\":%u,"
        "\"psoCacheMisses\":%u,\"texCreates\":%llu,\"texCreatesGame\":%llu,\"nanPixels\":%s,"
        "\"uploadFlushes\":%llu,\"uploadStallMs\":%.3f,"
        "\"uploadFlushesGame\":%llu,\"loaderFlushes\":%llu,\"loaderStallMs\":%.3f,"
        "\"imguiUploadWaits\":%llu,\"imguiUploadWaitMs\":%.3f,"
        "\"streamUploadRequests\":%llu,\"streamUploadBytes\":%llu,\"streamReplaced\":%llu,"
        "\"streamDropped\":%llu,\"streamCarriedOver\":%llu,\"streamFallback\":%llu,"
        "\"streamRingHighWaterBytes\":%llu,"
        "\"glyphRendered\":%llu,\"glyphRenderedOffMain\":%llu,\"glyphMainMs\":%.3f,"
        "\"glyphWorkerMs\":%.3f,\"glyphLate\":%llu,"
        "\"settingsRev\":%s,\"settingsFills\":%llu,"
        "%s"
        "\"profFrame\":%s}\n",
        static_cast<unsigned long long>(frame),
        static_cast<double>(FrameProbe::Get().cpuFrameMs),
        fs.drawPackets, fs.staticInstSlots, fs.dynamicInstances, fs.lights,
        ShaderLibrary::TotalFailedShaders(),
        Logger::Get().ErrorTotal(), visField, gpuField,
        // cpuMs keeps its name and meaning (the App bracket), so pack thresholds and
        // tools/ai_bench.py's fit hold; the split rows are additive. joinWaitMs is null on the
        // serial path - absent, never 0.
        static_cast<double>(ft.gameMs),
        static_cast<double>(ft.renderPhaseMs),
        static_cast<double>(ft.renderThreadMs),
        static_cast<double>(ft.gpuWaitMs),
        static_cast<double>(ft.cpuPeriodMs),
        joinField,
        ft.renderMode == 3 ? "graph-overlap" : (ft.renderMode == 2 ? "graph" : (ft.renderMode == 0 ? "serial" : "threaded")),
        // previewViews: a per-frame scalar, so the preview's zero idle cost ("0 on every frame
        // with no panel open") is a fieldEquals assertion. One frame behind the render half that
        // produced it - see FrameStats.
        fs.previewViewsRendered,
        static_cast<double>(ft.frameMs),
        static_cast<double>(ft.drawMs),
        static_cast<double>(ft.beginWaitMs),
        static_cast<double>(ft.presentMs),
        static_cast<double>(ft.loopTopMs),
        static_cast<unsigned long long>(ft.renderTimingFrame),
        ft.renderTimingValid ? "true" : "false",
        haveGpu ? (ft.gpuCalibrated ? "true" : "false") : "null",
        // Owner-active rows. renderOwnerMs describes renderTimingFrame, like
        // drawMs/renderPhaseMs do; the drain figures and parkHookCount are THIS
        // frame's game half.
        ownerField,
        static_cast<double>(ft.tailDrainMs),
        static_cast<double>(ft.blockingDrainMs),
        ft.ownerContinuationsPump,
        ft.ownerContinuationsDrain,
        ft.parkHookCount,
        // statsFrame: which frame the four emit-half counters at the head of the row describe -
        // "frame" by default, one less under -brslane (the emit half runs on the render-owner
        // lane after this frame handed off). statsValid is false on rows written before the
        // first emit half published (frame 0 under the lane): those counters are absent, not
        // zero, so scenario_run.py fieldEquals skips the row.
        static_cast<unsigned long long>(fs.statsFrame),
        fs.statsValid ? "true" : "false",
        // The BuildRenderScene split: brsPlanMs is the sort + preamble + ResolveBatchPlan half,
        // brsEmitMs the nest + tail. -brslane moves the emit half to the lane, and the plan half
        // with it wherever the lane emit is on (-brsplanlaneoff keeps the plan on main). A half
        // run on the lane is published by the emit half and applied on main
        // (Renderer::ConsumeEmitStats), so it carries statsFrame's one-frame offset.
        static_cast<double>(ft.brsPlanMs),
        static_cast<double>(ft.brsEmitMs),
        PipelineStats::shaderCompiles.load(std::memory_order_relaxed),
        PipelineStats::psoCreates.load(std::memory_order_relaxed),
        PipelineStats::psoLibMisses.load(std::memory_order_relaxed),
        // ShaderLibrary in-memory misses on every thread (PipelineStats.h).
        PipelineStats::shaderMisses.load(std::memory_order_relaxed),
        // PSOCache builds (driver or library) on every thread, and texture creations - all /
        // on the game thread (UploadStats.h).
        PipelineStats::psoCacheMisses.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(UploadStats::textureCreates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::textureCreatesGame.load(std::memory_order_relaxed)),
        nanField,
        // CPU-blocking upload waits, running totals since boot (Graphics/UploadStats.h):
        // FlushUploadAndWait round trips on both lanes, and the ImGui backend's own texture
        // uploads (Editor only). A row where one moved waited behind the graphics queue's whole
        // backlog that frame; read a window with scenario_run.py frames.totalGrowth.
        static_cast<unsigned long long>(UploadStats::flushes.load(std::memory_order_relaxed)),
        UploadStats::NsToMs(UploadStats::stallNs.load(std::memory_order_relaxed)),
        // Of those, the ones the game thread waited in; then the loader workers' own.
        static_cast<unsigned long long>(UploadStats::flushesGame.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::loaderFlushes.load(std::memory_order_relaxed)),
        UploadStats::NsToMs(UploadStats::loaderStallNs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::imguiWaits.load(std::memory_order_relaxed)),
        UploadStats::NsToMs(UploadStats::imguiWaitNs.load(std::memory_order_relaxed)),
        // The streaming upload: totals the uploader publishes (the ring's high-water mark is
        // the one maximum); 0 on a run that never streams.
        static_cast<unsigned long long>(UploadStats::streamRequests.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::streamBytes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::streamReplaced.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::streamDropped.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::streamCarriedOver.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::streamFallback.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(UploadStats::streamRingHighWater.load(std::memory_order_relaxed)),
        // The font atlas bake, running totals (UI/GlyphStats.h): real-glyph cells rendered,
        // those a TaskSystem worker rendered, the main thread's baking wall time, the workers'
        // render time, and the cells that became visible a frame after they were asked for.
        static_cast<unsigned long long>(UI::GlyphStats::Load(UI::GlyphStats::rendered)),
        static_cast<unsigned long long>(UI::GlyphStats::Load(UI::GlyphStats::renderedOffMain)),
        UI::GlyphStats::NsToMs(UI::GlyphStats::Load(UI::GlyphStats::mainNs)),
        UI::GlyphStats::NsToMs(UI::GlyphStats::Load(UI::GlyphStats::workerNs)),
        static_cast<unsigned long long>(UI::GlyphStats::Load(UI::GlyphStats::late)),
        // The settings snapshot this frame's hand-off published: the store revision of its
        // values, and how often that buffer was refilled (a change refills each of the two
        // buffers once).
        settingsRevField,
        static_cast<unsigned long long>(m_renderer.SettingsPublished().fills),
        spawnField,
        profField);

    if (n <= 0 || n >= static_cast<int>(sizeof(line)))
    {
        // Truncation would emit a syntactically broken JSON line, which is worse
        // than a missing one: it makes the whole file unparseable for a reader
        // that streams it. Stop, once, loudly.
        m_writeFailed = true;
        LOG_ERROR("Harness: frame record formatting overflowed; per-frame "
                  "recording disabled for the rest of this run.");
        return;
    }

    m_out.write(line, n);
    if (!m_out)
    {
        m_writeFailed = true;
        LOG_ERROR("Harness: write to frames.jsonl failed at frame %llu; "
                  "per-frame recording disabled for the rest of this run.",
                  static_cast<unsigned long long>(frame));
        return;
    }

    // Flushed every record: the file must be readable after an abnormal end (a watchdog
    // kill, a TDR, a crash), exactly when a buffered tail would be lost. The cost is one
    // small write per frame.
    m_out.flush();
    ++m_written;

    // -require_draws: fail the run the first time a frame submits nothing.
    // Latched into the ledger rather than acted on here, so the verdict is
    // resolved in one place after the logger drains.
    if (m_requireDraws && fs.drawPackets == 0 && frame >= m_drawGrace &&
        !m_drawFailureReported)
    {
        LOG_ERROR("Harness: -require_draws — frame %llu submitted ZERO draw "
                  "packets. This is the signature of a culling or visibility "
                  "regression: the window and any screenshot are black, yet "
                  "nothing logs an error.",
                  static_cast<unsigned long long>(frame));
        LOG_ERROR("Harness: (grace was %llu frames; %u empty frame(s) so far)",
                  static_cast<unsigned long long>(m_drawGrace), m_emptyFrames);
        m_drawFailureReported = true;
        RunLedger::Get().NoteReason("no_draws");
        PostQuitMessage(ExitCode::NoDraws);
    }
}

void HarnessCaptureSystem::ServiceUiShots(std::uint64_t frame)
{
    for (UiShot& s : m_uiShots)
    {
        if (s.done || frame < s.at) continue;
        s.done = true;   // latch first: a refusal must not retry every frame
        // The NEXT EndFrame records the copy; the PNG is written once the GPU
        // has finished it (GraphicsDX12 logs "[UI_SCREENSHOT] wrote ...").
        if (!m_gfx.MainSurface().RequestBackBufferCapture(s.path.c_str()))
            LOG_ERROR("Harness: -ui_screenshot '%s' at frame %llu was refused (a capture is "
                      "already pending)", s.path.c_str(),
                      static_cast<unsigned long long>(frame));
        else
            LOG_INFO("Harness: -ui_screenshot requested at frame %llu -> %s",
                     static_cast<unsigned long long>(frame), s.path.c_str());
        break;   // one per frame: the device holds a single pending request
    }
}

void HarnessCaptureSystem::ServiceRgDump(std::uint64_t frame)
{
    if (m_rgDumpDone || m_rgDumpPath.empty() || frame < m_rgDumpAt) return;
    // A frame capture: the main graph's snapshot and the device's frame (command lists,
    // submission, barrier journal) of one frame.
    RG::FrameCaptureBoard& board = RG::FrameCaptures();
    if (!m_rgDumpAsked)
    {
        m_rgDumpSeq     = board.Request();
        m_rgDumpAskedOn = frame;
        m_rgDumpAsked   = true;
        return;
    }
    if (board.PublishedSequence() <= m_rgDumpSeq)
    {
        // 120 frames: two seconds at a normal rate, longer when stalled. A main graph that never
        // executes again (one with every pass inactive still publishes) is a fault the caller
        // must see, not a silently missing file.
        if (frame < m_rgDumpAskedOn + 120) return;
        m_rgDumpDone = true;
        LOG_ERROR("Harness: -rgdump asked at frame %llu and no render graph snapshot was "
                  "published in 120 frames; nothing written to %s",
                  static_cast<unsigned long long>(m_rgDumpAskedOn), m_rgDumpPath.c_str());
        RunLedger::Get().NoteScriptFault("-rgdump", "no snapshot published");
        return;
    }
    m_rgDumpDone = true;
    const std::shared_ptr<const RG::FrameCapture> cap = board.Latest();
    if (!cap || !WriteRenderGraphDump(*cap, m_rgDumpPath))
    {
        RunLedger::Get().NoteScriptFault("-rgdump", "render graph dump failed");
        return;
    }
    LOG_INFO("[RG_SNAPSHOT] wrote frame %llu (asked at %llu) -> %s",
             static_cast<unsigned long long>(cap->graph ? cap->graph->frame : cap->frame),
             static_cast<unsigned long long>(m_rgDumpAskedOn), m_rgDumpPath.c_str());
}

void HarnessCaptureSystem::ServiceSecondShot(std::uint64_t frame)
{
    if (m_secondShotDone) return;
    if (m_screenshot2Path.empty() && m_previewShot2Path.empty()) return;
    if (frame < m_atFrame2) return;
    m_secondShotDone = true;   // latch first: a failure must not retry every frame

    if (!m_screenshot2Path.empty())
    {
        const bool ok = m_renderer.CaptureViewportToPNG(m_screenshot2Path.c_str());
        if (ok) LOG_INFO("Harness: screenshot2 at frame %llu -> %s",
                         static_cast<unsigned long long>(frame),
                         m_screenshot2Path.c_str());
        else
        {
            // Same verdict as the first slot's: a caller who asked for a picture
            // and silently got none would conclude the frame was fine.
            LOG_ERROR("Harness: screenshot2 FAILED at frame %llu ('%s')",
                      static_cast<unsigned long long>(frame),
                      m_screenshot2Path.c_str());
            RunLedger::Get().NoteScriptFault("-screenshot2",
                                             "capture refused or failed");
        }
    }
    // The preview half, through the first slot's helper, so a split panel's second moment
    // is named like its first.
    if (!m_previewShot2Path.empty())
        CapturePreviewShot(m_previewShot2Path, frame, "-preview_screenshot2");
}

void HarnessCaptureSystem::CapturePreviewShot(const std::string& path,
                                              std::uint64_t frame,
                                              const char* flag)
{
    // More than one view with output (a split panel): one PNG per view,
    // `<stem>_v<id><ext>`, and the pack asserts on each - "the newest view" would name
    // whichever cell the round-robin served last, a file that changes run to run. With one
    // view the file is exactly the path asked for, nothing extra.
    std::uint32_t withOutput = 0;
    for (std::uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
        if (m_renderer.GetPreviewOutputTexture(v)) ++withOutput;

    if (withOutput > 1)
    {
        const std::size_t dot = path.find_last_of('.');
        const std::string stem = (dot == std::string::npos)
            ? path : path.substr(0, dot);
        const std::string ext  = (dot == std::string::npos)
            ? std::string(".png") : path.substr(dot);
        std::uint32_t wrote = 0;
        for (std::uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
        {
            const RHI::Texture* t = m_renderer.GetPreviewOutputTexture(v);
            if (!t) continue;
            const std::string p = stem + "_v" + std::to_string(v) + ext;
            if (m_gfx.CaptureTextureToPNG(
                    *t, RHI::ResourceState::SHADER_RESOURCE, p.c_str()))
                ++wrote;
            else
            {
                LOG_ERROR("Harness: preview screenshot FAILED at frame %llu "
                          "('%s')",
                          static_cast<unsigned long long>(frame), p.c_str());
                RunLedger::Get().NoteScriptFault(flag,
                                                 "capture refused or failed");
            }
        }
        LOG_INFO("Harness: preview screenshot at frame %llu -> %u per-view "
                 "PNG(s) '%s_v<id>%s' (%u views had pixels)",
                 static_cast<unsigned long long>(frame), wrote,
                 stem.c_str(), ext.c_str(), withOutput);
        return;
    }

    // The view id belongs to the editor widget, so ask the Renderer which view
    // has the newest pixels rather than plumbing an id through the harness.
    // Null means no preview ever completed a render.
    const std::uint32_t viewId = m_renderer.GetLastRenderedPreviewView();
    const RHI::Texture* tex    = m_renderer.GetPreviewOutputTexture(viewId);
    if (!tex)
    {
        // WARNING, not a run fault: "a preview capture on a run whose panel
        // never opened" is a caller mistake, and the pack that cares asserts
        // artifactsExist, which turns this into a failure there instead of here.
        LOG_WARNING("Harness: %s at frame %llu skipped - no preview view has "
                    "rendered (view id %u)", flag,
                    static_cast<unsigned long long>(frame), viewId);
        return;
    }
    // ToneMapPass::TransitionForDisplay left it in SHADER_RESOURCE at the end of
    // the preview command list, the same state CaptureViewportToPNG assumes of
    // the main output.
    const bool ok = m_gfx.CaptureTextureToPNG(
        *tex, RHI::ResourceState::SHADER_RESOURCE, path.c_str());
    if (ok) LOG_INFO("Harness: preview screenshot at frame %llu -> %s",
                     static_cast<unsigned long long>(frame), path.c_str());
    else
    {
        LOG_ERROR("Harness: preview screenshot FAILED at frame %llu ('%s')",
                  static_cast<unsigned long long>(frame), path.c_str());
        RunLedger::Get().NoteScriptFault(flag, "capture refused or failed");
    }
}

void HarnessCaptureSystem::ServiceOneShots(World& world, std::uint64_t frame)
{
    if (m_oneShotsDone)
    {
        if (m_captureRemaining && frame >= m_atFrame + m_captureEvery)
        {
            m_atFrame = frame;
            --m_captureRemaining;
            const auto base = Sys::U8Path(m_screenshotPath);
            const auto output = base.parent_path() / Sys::U8Path(Sys::PathToUtf8(base.stem())+"."+std::to_string(frame)+Sys::PathToUtf8(base.extension()));
            const std::string path = Sys::PathToUtf8(output);
            if (!m_renderer.CaptureViewportToPNG(path.c_str()))
            {
                LOG_ERROR("Harness: sequence screenshot failed at frame %llu", static_cast<unsigned long long>(frame));
                RunLedger::Get().NoteScriptFault("-screenshot_count", "capture failed");
            }
        }
        return;
    }
    if (m_screenshotPath.empty() && m_dumpStatePath.empty() &&
        m_dumpFlowPath.empty() && m_previewShotPath.empty()) return;
    if (frame < m_atFrame) return;

    m_oneShotsDone = true;   // latch first: a failure must not retry every frame

    if (!m_screenshotPath.empty())
    {
        const bool ok = m_renderer.CaptureViewportToPNG(m_screenshotPath.c_str());
        if (ok) LOG_INFO("Harness: screenshot at frame %llu -> %s",
                         static_cast<unsigned long long>(frame), m_screenshotPath.c_str());
        else
        {
            // CaptureViewportToPNG returns false when a hard GPU sync would be illegal here. A run
            // failure: a caller who asked for a picture and silently got none would conclude the
            // frame was fine.
            LOG_ERROR("Harness: screenshot FAILED at frame %llu ('%s')",
                      static_cast<unsigned long long>(frame), m_screenshotPath.c_str());
            RunLedger::Get().NoteScriptFault("-screenshot", "capture refused or failed");
        }
    }

    // Both preview slots go through CapturePreviewShot (per-view naming, the "no view has
    // rendered" warning, the ledger fault).
    if (!m_previewShotPath.empty())
        CapturePreviewShot(m_previewShotPath, frame, "-preview_screenshot");

    if (!m_dumpStatePath.empty())
    {
        if (!WriteWorldStateDump(world, m_dumpStatePath))
            RunLedger::Get().NoteScriptFault("-dumpstate", "world state dump failed");
    }

    if (!m_dumpFlowPath.empty())
    {
        // The state machine lives on App, reachable only through the probe App binds at
        // registration (GameFlowDump.h). An unbound probe is a wiring fault, reported like a
        // refused screenshot: the caller asked for a file.
        const GameFlowProbe& probe = GameFlowProbe::Get();
        if (!probe.Bound())
        {
            LOG_ERROR("Harness: -dumpflow was asked for but the GameFlow probe "
                      "was never bound; no flow dump was written.");
            RunLedger::Get().NoteScriptFault("-dumpflow", "GameFlow probe unbound");
        }
        else if (!WriteGameFlowDump(*probe.flow, *probe.systems, *probe.scripts,
                                    m_dumpFlowPath))
        {
            RunLedger::Get().NoteScriptFault("-dumpflow", "game flow dump failed");
        }
    }
}

// ==== The pipe's `capture` verb (CaptureRequests.h) ====
// At PostRender this frame's passes are recorded, and CaptureTextureToPNG parks the
// render lane and flushes the queue before it copies, so the picture is THIS frame's
// ToneMap output and its index is `frame`.
void HarnessCaptureSystem::ServiceCaptureRequests(World& world, std::uint64_t frame)
{
    for (const CaptureRequests::Request& r : CaptureRequests::TakeDue(frame))
    {
        if (!r.attachment.empty())
        {
            // After the drain the render half of `frame` has run, so "this frame" is `frame`
            // (ServiceAttachment drains again, a no-op then). A debug target made only on request
            // (shadingModel) that this frame did not make is asked for, and the capture waits a
            // frame for it - three times at most.
            m_gfx.DrainCpuFrame();
            // Right after a pass: the main graph's copy.
            if (r.afterPassGiven)
            {
                ServiceAfterPassCapture(world, r, frame);
                continue;
            }
            if (r.produceWaits < 3 && m_renderer.DebugTargetNeedsProduce(r.attachment))
            {
                CaptureRequests::Request again = r;
                again.notBefore = frame + 1;
                ++again.produceWaits;
                LOG_INFO("[CAPTURE] attachment '%s' is made on request and frame %llu did not make it: job %llu "
                         "waits for the next frame (%u/3)", r.attachment.c_str(),
                         static_cast<unsigned long long>(frame), static_cast<unsigned long long>(r.jobId),
                         again.produceWaits);
                CaptureRequests::Requeue(std::move(again));
                continue;
            }
            // A render target as data: read back and written right here.
            ServiceAttachment(world, r.jobId, r.attachment, r.path, r.afterFrame, frame);
            continue;
        }
        if (r.view >= 0)
        {
            // An existing preview view: checked and taken by ServiceViewCaptures.
            ViewCapture c;
            c.jobId        = r.jobId;
            c.path         = r.path;
            c.afterFrame   = r.afterFrame;
            c.afterRenders = r.afterRenders;
            c.dueFrame     = frame;
            c.view         = static_cast<std::uint32_t>(r.view);
            c.lease        = r.lease;
            m_viewCaptures.push_back(std::move(c));
            continue;
        }
        if (r.appUi)
        {
            // The presented window: asked for and finished by ServiceAppUi.
            AppUiCapture c;
            c.jobId      = r.jobId;
            c.path       = r.path;
            c.panel      = r.panel;
            c.afterFrame = r.afterFrame;
            c.dueFrame   = frame;
            // A panel: the whole window goes beside the crop, as <stem>.window.png.
            if (c.panel.empty())
                c.shotPath = c.path;
            else
            {
                std::filesystem::path stem = Sys::U8Path(c.path);
                stem.replace_extension();
                c.shotPath = Sys::PathToUtf8(stem) + ".window.png";
            }
            m_appUiCaptures.push_back(std::move(c));
            continue;
        }
        if (r.camera != 0)
        {
            // A named camera takes several frames: queued for ServiceCameraCaptures.
            CameraCapture c;
            c.jobId      = r.jobId;
            c.path       = r.path;
            c.afterFrame = r.afterFrame;
            c.packedId   = r.camera;
            c.width      = r.width;
            c.height     = r.height;
            m_cameraCaptures.push_back(std::move(c));
            continue;
        }
        Jobs::Start(r.jobId);
        if (!m_renderer.CaptureViewportToPNG(r.path.c_str()))
        {
            LOG_WARNING("Harness: capture job %llu at frame %llu FAILED ('%s')",
                        static_cast<unsigned long long>(r.jobId),
                        static_cast<unsigned long long>(frame), r.path.c_str());
            Jobs::Fail(r.jobId, "capture_failed",
                       "the main view could not be captured at frame " + std::to_string(frame) +
                       " (refused or failed - see the log)", {});
            continue;
        }
        // Width / height from the PNG's IHDR, so the job reports the picture
        // that was written, not what the texture was meant to be.
        std::uint32_t w = 0, h = 0;
        std::uint64_t bytes = 0;
        {
            std::error_code ec;
            bytes = static_cast<std::uint64_t>(std::filesystem::file_size(Sys::U8Path(r.path), ec));
            std::ifstream f(Sys::U8Path(r.path), std::ios::binary);
            unsigned char hdr[24] = {};
            if (f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)))
            {
                w = (std::uint32_t(hdr[16]) << 24) | (std::uint32_t(hdr[17]) << 16) |
                    (std::uint32_t(hdr[18]) << 8)  |  std::uint32_t(hdr[19]);
                h = (std::uint32_t(hdr[20]) << 24) | (std::uint32_t(hdr[21]) << 16) |
                    (std::uint32_t(hdr[22]) << 8)  |  std::uint32_t(hdr[23]);
            }
        }
        nlohmann::json out{
            { "path", r.path }, { "source", "view_output" }, { "view", "main" },
            { "afterFrame", r.afterFrame }, { "renderedFrame", frame },
            { "width", w }, { "height", h }, { "bytes", bytes } };
        AddMainViewMetadata(world, out);   // camera, worldEpoch, exposure, debugTarget, hdrMode, sizes
        Jobs::Succeed(r.jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        LOG_INFO("Harness: capture job %llu at frame %llu -> %s (%ux%u)",
                 static_cast<unsigned long long>(r.jobId), static_cast<unsigned long long>(frame),
                 r.path.c_str(), w, h);
    }
}

// ==== The window as presented: app_ui (CaptureRequests.h) ====
namespace
{
    nlohmann::json WindowsJson(const std::vector<Sys::UiWindows::Window>& ws)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const Sys::UiWindows::Window& w : ws)
            arr.push_back({ { "name", w.name }, { "label", w.label }, { "x", w.x }, { "y", w.y }, { "w", w.w },
                            { "h", w.h }, { "order", w.order }, { "focused", w.focused },
                            { "collapsed", w.collapsed }, { "docked", w.docked },
                            { "background", w.background } });
        return arr;
    }

    // The panel a request names: by its title first, then by ImGui's full
    // name; the front-most when two share it. Null = not laid out that frame.
    const Sys::UiWindows::Window* FindPanel(const std::vector<Sys::UiWindows::Window>& ws, const std::string& want)
    {
        const Sys::UiWindows::Window* best = nullptr;
        for (int pass = 0; pass < 2 && !best; ++pass)
            for (const Sys::UiWindows::Window& w : ws)
                if ((pass == 0 ? w.label : w.name) == want && (!best || w.order > best->order)) best = &w;
        return best;
    }

    // The pixels [x0, x1) x [y0, y1) of the PNG at @p src, as a PNG at @p dst -
    // the bytes as they were, whatever format WIC hands back.
    bool CropPng(const std::string& src, const std::string& dst, std::uint32_t x0, std::uint32_t y0,
                 std::uint32_t x1, std::uint32_t y1, std::string& error)
    {
        DirectX::ScratchImage full;
        const std::wstring wsrc = Sys::U8Path(src).wstring();
        if (FAILED(DirectX::LoadFromWICFile(wsrc.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, full)))
        {
            error = "cannot read " + src;
            return false;
        }
        const DirectX::Image* img = full.GetImage(0, 0, 0);
        const size_t bpp = img ? DirectX::BitsPerPixel(img->format) / 8 : 0;
        if (!img || bpp == 0 || x0 >= x1 || y0 >= y1 || x1 > img->width || y1 > img->height)
        {
            error = "the rectangle is not inside the picture";
            return false;
        }
        DirectX::ScratchImage crop;
        if (FAILED(crop.Initialize2D(img->format, x1 - x0, y1 - y0, 1, 1)))
        {
            error = "cannot allocate the crop";
            return false;
        }
        const DirectX::Image* out = crop.GetImage(0, 0, 0);
        for (std::uint32_t y = y0; y < y1; ++y)
            std::memcpy(out->pixels + static_cast<size_t>(y - y0) * out->rowPitch,
                        img->pixels + static_cast<size_t>(y) * img->rowPitch + static_cast<size_t>(x0) * bpp,
                        static_cast<size_t>(x1 - x0) * bpp);
        const std::wstring wdst = Sys::U8Path(dst).wstring();
        if (FAILED(DirectX::SaveToWICFile(*out, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                          wdst.c_str())))
        {
            error = "cannot write " + dst;
            return false;
        }
        return true;
    }
}

// The device holds one back-buffer request at a time (a -ui_screenshot may have
// it): an ask it refuses is asked again next frame. Its outcome arrives after
// that frame's Present - on the render lane under -renderowner - so it is taken
// on a later call; outcomes for other files are some other caller's. The
// panels are sampled when the capture is asked for: that frame's UI is built
// (PostRender, or before the post under -renderowner) and it is the frame the
// picture is of.
void HarnessCaptureSystem::ServiceAppUi(std::uint64_t frame, bool mayRequest)
{
    constexpr std::uint64_t kAskFrames = 240, kWriteFrames = 120;
    if (m_appUiCaptures.empty()) return;
    AppUiCapture& c = m_appUiCaptures.front();
    if (!c.requested)
    {
        if (frame > c.dueFrame + kAskFrames)
        {
            Jobs::Fail(c.jobId, "capture_failed", "the device took no back-buffer capture in " +
                                                  std::to_string(kAskFrames) + " frames (another stayed pending)", {});
            m_appUiCaptures.erase(m_appUiCaptures.begin());
            return;
        }
        if (!mayRequest || frame < c.afterFrame || !m_gfx.MainSurface().RequestBackBufferCapture(c.shotPath.c_str())) return;
        c.requested      = true;
        c.requestedFrame = frame;
        c.windows        = Sys::UiWindows::Sample();
        Jobs::Start(c.jobId);
        return;
    }
    ISurface::BackBufferShot shot;
    while (m_gfx.MainSurface().TakeBackBufferShot(shot))
    {
        if (shot.path != c.shotPath) continue;
        if (!shot.ok)
        {
            LOG_WARNING("Harness: app_ui capture job %llu (frame %llu) FAILED: %s",
                        static_cast<unsigned long long>(c.jobId),
                        static_cast<unsigned long long>(c.requestedFrame), shot.error.c_str());
            Jobs::Fail(c.jobId, "capture_failed", "the presented frame " + std::to_string(c.requestedFrame) +
                                                  " was not captured: " + shot.error, {});
        }
        else
        {
            nlohmann::json out{
                { "path", c.path }, { "source", "app_ui" }, { "host", Sys::IsEditorHost() ? "editor" : "game" },
                { "afterFrame", c.afterFrame }, { "renderedFrame", c.requestedFrame },
                { "width", shot.width }, { "height", shot.height }, { "uniform", shot.uniform },
                { "windows", WindowsJson(c.windows) } };
            std::string failCode, failWhy;
            if (!c.panel.empty())
            {
                // ONE PANEL: its rectangle of the window, clamped to it. What
                // was COMPOSITED there - a window drawn over it (a larger
                // order) is in the crop, and visibleFraction says how much.
                const Sys::UiWindows::Window* p = FindPanel(c.windows, c.panel);
                const auto clampTo = [](float v, std::uint32_t hi)
                { return static_cast<std::uint32_t>(std::clamp(v, 0.f, static_cast<float>(hi))); };
                std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (p)
                {
                    x0 = clampTo(std::floor(p->x), shot.width);
                    y0 = clampTo(std::floor(p->y), shot.height);
                    x1 = clampTo(std::ceil(p->x + p->w), shot.width);
                    y1 = clampTo(std::ceil(p->y + p->h), shot.height);
                }
                std::string cropError;
                if (!p || x1 <= x0 || y1 <= y0)
                {
                    std::string seen;
                    for (const Sys::UiWindows::Window& w : c.windows) seen += (seen.empty() ? "" : ", ") + w.label;
                    failCode = "no_panel";
                    failWhy  = "no panel '" + c.panel + "' was " + (p ? "inside the window" : "laid out") +
                               " in frame " + std::to_string(c.requestedFrame) + " - the panels were: " +
                               (seen.empty() ? std::string("none (Game.exe draws no editor UI)") : seen);
                }
                else if (!CropPng(c.shotPath, c.path, x0, y0, x1, y1, cropError))
                {
                    failCode = "capture_failed";
                    failWhy  = "the panel could not be cropped: " + cropError;
                }
                else
                {
                    // How much of it is not under a window drawn after it.
                    const std::uint32_t cw = x1 - x0, ch = y1 - y0;
                    std::vector<std::uint8_t> covered(static_cast<size_t>(cw) * ch, 0);
                    nlohmann::json occludedBy = nlohmann::json::array();
                    for (const Sys::UiWindows::Window& w : c.windows)
                    {
                        // Drawn after it and opaque: an overlay (no background) hides nothing.
                        if (w.order <= p->order || !w.background) continue;
                        const std::uint32_t ox0 = std::max(x0, clampTo(std::floor(w.x), shot.width));
                        const std::uint32_t oy0 = std::max(y0, clampTo(std::floor(w.y), shot.height));
                        const std::uint32_t ox1 = std::min(x1, clampTo(std::ceil(w.x + w.w), shot.width));
                        const std::uint32_t oy1 = std::min(y1, clampTo(std::ceil(w.y + w.h), shot.height));
                        if (ox1 <= ox0 || oy1 <= oy0) continue;
                        occludedBy.push_back(w.label);
                        for (std::uint32_t y = oy0; y < oy1; ++y)
                            std::fill_n(covered.begin() + static_cast<std::ptrdiff_t>((y - y0) * cw + (ox0 - x0)),
                                        ox1 - ox0, std::uint8_t{ 1 });
                    }
                    const double hidden = static_cast<double>(std::count(covered.begin(), covered.end(), 1));
                    out["window"] = c.shotPath;
                    out["width"]  = cw;
                    out["height"] = ch;
                    out["panel"]  = { { "name", p->name }, { "label", p->label },
                                      { "rect", { x0, y0, cw, ch } },
                                      { "visibleFraction", 1.0 - hidden / static_cast<double>(covered.size()) },
                                      { "occludedBy", occludedBy } };
                }
            }
            if (!failCode.empty())
            {
                LOG_WARNING("Harness: app_ui capture job %llu (panel '%s') failed: %s",
                            static_cast<unsigned long long>(c.jobId), c.panel.c_str(), failWhy.c_str());
                Jobs::Fail(c.jobId, failCode, failWhy, {});
            }
            else
            {
                std::error_code ec;
                const auto bytes = std::filesystem::file_size(Sys::U8Path(c.path), ec);
                out["bytes"] = ec ? 0 : static_cast<std::uint64_t>(bytes);
                Jobs::Succeed(c.jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                LOG_INFO("Harness: app_ui capture job %llu at frame %llu -> %s (%ux%u, %zu panel(s))",
                         static_cast<unsigned long long>(c.jobId),
                         static_cast<unsigned long long>(c.requestedFrame), c.path.c_str(),
                         out["width"].get<std::uint32_t>(), out["height"].get<std::uint32_t>(), c.windows.size());
            }
        }
        m_appUiCaptures.erase(m_appUiCaptures.begin());
        return;
    }
    if (frame > c.requestedFrame + kWriteFrames)
    {
        Jobs::Fail(c.jobId, "capture_failed", "the presented frame " + std::to_string(c.requestedFrame) +
                                              " was not written within " + std::to_string(kWriteFrames) +
                                              " frames", {});
        m_appUiCaptures.erase(m_appUiCaptures.begin());
    }
}

// ==== A NAMED camera (CaptureRequests.h) ====
// In Edit mode the main view is the editor's own camera, which no entity names, so
// "what does camera X see" goes through the in-game PiP view: point it at X (SetGamePiP
// only queues - it applies at the next BuildRenderScene), wait until the view has
// RECORDED two pictures since (the counter is per view slot and monotonic, so the
// baseline is read once the PiP names X), take its output at this safe point, and put
// the PiP back the way it was.
void HarnessCaptureSystem::ServiceCameraCaptures(World& world, std::uint64_t frame)
{
    if (m_cameraCaptures.empty()) return;
    CameraCapture& c = m_cameraCaptures.front();
    auto done = [&](bool restorePiP)
    {
        if (restorePiP)
        {
            if (c.prevCamera != 0 && world.IsAlive(c.prevCamera))
                m_renderer.SetGamePiP(c.prevCamera, c.prevW, c.prevH, c.prevTick);
            else
                m_renderer.ClearGamePiP();
        }
        m_cameraCaptures.erase(m_cameraCaptures.begin());
    };
    const unsigned long long job = c.jobId;

    if (!c.armed)
    {
        Jobs::Start(c.jobId);
        // The id as Lua hands it out: (generation & 0x1FFFFF) << 32 | index.
        const Entity e = static_cast<Entity>(c.packedId & 0xFFFFFFFFull);
        const std::uint32_t gen = static_cast<std::uint32_t>((c.packedId >> 32) & 0x1FFFFFu);
        if (e == NullEntity || !world.IsAlive(e) || (gen != 0 && (world.GetGeneration(e) & 0x1FFFFFu) != gen))
        {
            Jobs::Fail(c.jobId, "stale_handle", "entity " + std::to_string(c.packedId) +
                       " is dead, or its slot was recycled (a world since)", {});
            done(false);
            return;
        }
        if (!world.HasComponent<CameraComponent>(e))
        {
            Jobs::Fail(c.jobId, "no_camera", "entity " + std::to_string(c.packedId) + " has no Camera component", {});
            done(false);
            return;
        }
        c.camera     = e;
        c.prevCamera = m_renderer.GetGamePiPEntity();
        m_renderer.GetGamePiPSettings(c.prevW, c.prevH, c.prevTick);
        if (!m_renderer.SetGamePiP(e, c.width, c.height, 1))
        {
            Jobs::Fail(c.jobId, "unavailable", "the preview layer is off (DX12_PREVIEW=0 / preview.enabled=0)", {});
            done(false);
            return;
        }
        c.armed      = true;
        c.armedFrame = frame;
        LOG_INFO("Harness: capture job %llu: the PiP view looks through camera %u (%ux%u) from frame %llu",
                 job, static_cast<unsigned>(e), c.width, c.height, static_cast<unsigned long long>(frame + 1));
        return;
    }

    const std::uint32_t view = m_renderer.GetGamePiPView();
    if (view != Preview::kInvalidView && m_renderer.GetGamePiPEntity() == c.camera)
    {
        const std::uint64_t n = m_renderer.GetPreviewViewRenderCount(view);
        if (!c.baselineKnown)
        {
            c.baselineKnown = true;
            c.rendersAtArm  = n;
            c.rendersSeen   = n;
        }
        if (n != c.rendersSeen)
        {
            c.rendersSeen     = n;
            c.lastRenderFrame = frame;
        }
        if (n >= c.rendersAtArm + 2 && frame >= c.afterFrame)
        {
            const RHI::Texture* tex = m_renderer.GetPreviewOutputTexture(view);
            if (!tex || !m_gfx.CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, c.path.c_str()))
            {
                Jobs::Fail(c.jobId, "capture_failed", "the camera's view could not be captured (see the log)", {});
                done(true);
                return;
            }
            std::uint32_t w = 0, h = 0;
            std::uint64_t bytes = 0;
            {
                std::error_code ec;
                bytes = static_cast<std::uint64_t>(std::filesystem::file_size(Sys::U8Path(c.path), ec));
                std::ifstream f(Sys::U8Path(c.path), std::ios::binary);
                unsigned char hdr[24] = {};
                if (f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)))
                {
                    w = (std::uint32_t(hdr[16]) << 24) | (std::uint32_t(hdr[17]) << 16) |
                        (std::uint32_t(hdr[18]) << 8)  |  std::uint32_t(hdr[19]);
                    h = (std::uint32_t(hdr[20]) << 24) | (std::uint32_t(hdr[21]) << 16) |
                        (std::uint32_t(hdr[22]) << 8)  |  std::uint32_t(hdr[23]);
                }
            }
            const nlohmann::json out{
                { "path", c.path }, { "source", "view_output" }, { "view", "camera" }, { "viewId", view },
                { "camera", c.packedId }, { "afterFrame", c.afterFrame },
                { "renderedFrame", c.lastRenderFrame ? c.lastRenderFrame : frame }, { "capturedFrame", frame },
                { "viewRenders", n - c.rendersAtArm }, { "width", w }, { "height", h }, { "bytes", bytes } };
            Jobs::Succeed(c.jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            LOG_INFO("Harness: capture job %llu at frame %llu -> %s (camera %u, view %u, %ux%u)", job,
                     static_cast<unsigned long long>(frame), c.path.c_str(), static_cast<unsigned>(c.camera),
                     view, w, h);
            done(true);
            return;
        }
    }
    if (frame > c.armedFrame + 240)
    {
        Jobs::Fail(c.jobId, "view_not_rendered",
                   "the camera's view recorded no picture in 240 frames (another camera panel may hold the "
                   "view lane at another size - the log's [PREVIEW] lines say)", {});
        done(true);
    }
}

// ==== An EXISTING preview view by (id, lease) (CaptureRequests.h) ====
// Render.views() handed the caller the pair; the lease is the slot's lifetime generation,
// so "the view I looked at" is still the slot's tenant exactly when both match. Anything
// else - released (not live), or live under a newer lease (closed and reopened, or
// another panel took the id) - is stale_view: whoever holds the id now is the wrong
// picture with a right name.
void HarnessCaptureSystem::ServiceViewCaptures(std::uint64_t frame)
{
    constexpr std::uint64_t kRenderFrames = 240;
    if (m_viewCaptures.empty()) return;
    ViewCapture& c = m_viewCaptures.front();
    if (frame < c.afterFrame) return;
    if (!c.started)
    {
        c.started = true;
        Jobs::Start(c.jobId);
    }
    auto done = [&] { m_viewCaptures.erase(m_viewCaptures.begin()); };

    // A secondary view of the main pipeline (Renderer::AcquireView) has an id from
    // Renderer::kSecondaryViewIdBase on; below it, a preview view's.
    const bool          secondary = c.view >= Renderer::kSecondaryViewIdBase;
    const std::uint32_t slot      = secondary ? c.view - Renderer::kSecondaryViewIdBase : 0u;
    const Renderer::ViewInfo info = secondary ? m_renderer.GetViewInfo(slot) : Renderer::ViewInfo{};
    std::uint32_t vw = 0, vh = 0;
    const bool          live  = secondary ? info.live : m_renderer.GetPreviewViewRequestSize(c.view, vw, vh);
    const std::uint32_t lease = secondary ? info.lease : m_renderer.GetPreviewViewLease(c.view);
    if (!live || lease != c.lease)
    {
        const std::string now = !live ? std::string("the view was released")
                                      : "the slot now holds lease " + std::to_string(lease) +
                                        " (closed and reopened, or another panel took the id)";
        Jobs::Fail(c.jobId, "stale_view", "view " + std::to_string(c.view) + " lease " + std::to_string(c.lease) +
                                          " is gone: " + now + " - read Render.views() again", {});
        done();
        return;
    }
    const std::uint64_t renders = secondary ? info.renders : m_renderer.GetPreviewViewRenderCount(c.view);
    // null until THIS tenant drew
    const RHI::Texture* tex = secondary ? m_renderer.GetViewOutputTexture(slot) : m_renderer.GetPreviewOutputTexture(c.view);
    if (!tex || renders < c.afterRenders)
    {
        if (frame > c.dueFrame + kRenderFrames)
        {
            Jobs::Fail(c.jobId, "view_not_rendered",
                       "view " + std::to_string(c.view) + " recorded " +
                       (tex ? "no picture past " + std::to_string(renders) + " (afterRenders " +
                                  std::to_string(c.afterRenders) + ")"
                            : std::string("no picture yet")) +
                       " in " + std::to_string(kRenderFrames) + " frames (a hidden panel, or its size refused "
                       "- the log's [PREVIEW] lines say)", {});
            done();
        }
        return;
    }
    if (!m_gfx.CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, c.path.c_str()))
    {
        Jobs::Fail(c.jobId, "capture_failed", "view " + std::to_string(c.view) + " could not be captured (see the log)",
                   {});
        done();
        return;
    }
    std::uint32_t w = 0, h = 0;
    std::uint64_t bytes = 0;
    {
        std::error_code ec;
        bytes = static_cast<std::uint64_t>(std::filesystem::file_size(Sys::U8Path(c.path), ec));
        std::ifstream f(Sys::U8Path(c.path), std::ios::binary);
        unsigned char hdr[24] = {};
        if (f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)))
        {
            w = (std::uint32_t(hdr[16]) << 24) | (std::uint32_t(hdr[17]) << 16) |
                (std::uint32_t(hdr[18]) << 8)  |  std::uint32_t(hdr[19]);
            h = (std::uint32_t(hdr[20]) << 24) | (std::uint32_t(hdr[21]) << 16) |
                (std::uint32_t(hdr[22]) << 8)  |  std::uint32_t(hdr[23]);
        }
    }
    const nlohmann::json out{
        { "path", c.path }, { "source", "view_output" }, { "view", "preview" }, { "viewId", c.view },
        { "lease", c.lease },
        { "kind", secondary ? "view" : m_renderer.IsPreviewWorldCullSlot(c.view) ? "camera" : "material" },
        { "renders", renders }, { "afterRenders", c.afterRenders }, { "afterFrame", c.afterFrame },
        { "capturedFrame", frame }, { "width", w }, { "height", h }, { "bytes", bytes } };
    Jobs::Succeed(c.jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    LOG_INFO("Harness: capture job %llu at frame %llu -> %s (view %u lease %u, %u renders, %ux%u)",
             static_cast<unsigned long long>(c.jobId), static_cast<unsigned long long>(frame), c.path.c_str(),
             c.view, c.lease, static_cast<unsigned>(renders), w, h);
    done();
}

// ==== -ui_render ====
// A .iui drawn off screen by the Renderer's UI view - the UI Editor's designer view,
// without an editor. Requested here at PostRender, painted by the next frame's UI paint
// window, recorded on its restoreCL; written once final (legal here for the reason
// CaptureTextureToPNG is: the frame's work is submitted).
void HarnessCaptureSystem::ServiceUiRender(World& world, std::uint64_t frame)
{
    UiRender& u = m_uiRender;
    if (u.asset.empty() || u.done) return;
    UIViewService* views = m_renderer.GetUIViews();
    auto finish = [&](bool ok, const char* what, const std::string& why) {
        if (!ok) LOG_ERROR("-ui_render %s: %s", u.asset.c_str(), (std::string(what) + ": " + why).c_str());
        if (views && u.view != ~0u) views->Release(u.view);
        u.view = ~0u;
        u.done = true;
        // The spawned asset was the picture's only: not left in the world (an Editor.exe
        // run would otherwise save it with the scene).
        if (u.root != NullEntity && world.IsAlive(u.root)) Hierarchy::DestroySubtree(world, u.root);
        u.root = NullEntity;
    };
    if (!views) { finish(false, "no UI view service", ""); return; }
    if (!u.spawned)
    {
        u.spawned   = true;
        u.startedAt = frame;
        std::string why;
        const std::shared_ptr<const UI::UIAsset> asset = UI::LoadUIAsset(u.asset, &why);
        if (!asset) { finish(false, "load", why); return; }
        UI::UISpawnOptions o;
        o.assetKey = Sys::RelativizeAssetPath(u.asset);
        o.seed     = ECS::Guid::FromHex("0000000000000001000000000000a5e7");   // reproducible node GUIDs
        std::vector<std::pair<std::string, Entity>> made;
        u.root = UI::SpawnUIAsset(world, *asset, o, &why, &made);
        if (u.root == NullEntity) { finish(false, "spawn", why); return; }
        // Never a scene's: a save before the picture is written skips every node.
        for (const auto& m : made)
            if (!world.HasComponent<SceneTransientTag>(m.second))
                world.AddComponent<SceneTransientTag>(m.second, SceneTransientTag{});
        // A canvas asset would otherwise be laid out and drawn by the game screen too.
        if (world.HasComponent<UI::UICanvas>(u.root))
            world.AddComponent<UI::UICanvasOffscreen>(u.root, UI::UICanvasOffscreen{ UI::UICanvasOffscreen::kUIRender });
        u.view = views->Acquire();
        if (u.view == ~0u) { finish(false, "acquire", "no free UI view"); return; }
    }
    if (!world.IsAlive(u.root)) { finish(false, "render", "the spawned root died (a scene load?)"); return; }

    // The description every frame: requesting it is what keeps a view alive.
    UIView::Desc d;
    d.subject     = u.root;
    d.worldEpoch  = world.Epoch();
    d.designSize  = UI::Vec2{ static_cast<float>(u.w), static_cast<float>(u.h) };
    d.targetW     = u.w;
    d.targetH     = u.h;
    d.zoom        = 1.f;
    d.pan         = UI::Vec2{ 0.f, 0.f };
    d.background  = UIView::Background::Checker;
    d.workspaceRgba = 0xFF1A1A1Au;   // the UI Editor's (outside the frame: none at zoom 1, pan 0)
    d.clipToFrame = true;
    d.realtime    = false;           // a still picture: jitter and text shaders at time 0
    d.serial      = 1;
    views->Request(u.view, d);

    const UIView::Status st = views->GetStatus(u.view);
    const bool final = st.current && st.paintedSerial == d.serial &&
                       (st.pendingImages == 0 || frame > u.startedAt + 120);
    if (!final)
    {
        u.steady = false;
        if (frame > u.startedAt + 600) finish(false, "render", "the picture never settled in 600 frames");
        return;
    }
    if (!u.steady)
    {
        u.steady      = true;
        u.steadySince = frame;
        return;
    }
    if (frame < u.steadySince + 4) return;
    const RHI::Texture* tex = views->Output(u.view);
    const bool ok = tex && m_gfx.CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, u.path.c_str());
    if (ok)
        LOG_INFO("[UIRENDER] wrote %s: file='%s' w=%u h=%u nodes=%u cmds=%u verts=%u hash=%016llx pendingImages=%u "
                 "frame=%llu", u.asset.c_str(), u.path.c_str(), st.width, st.height, st.nodes, st.cmds, st.verts,
                 static_cast<unsigned long long>(st.designHash), st.pendingImages, static_cast<unsigned long long>(frame));
    finish(ok, "capture", "CaptureTextureToPNG failed for '" + u.path + "'");
}

// ==== The pipe's `pick` verb (CaptureRequests.h) ====
// The editor's own picking pass: RequestPick marks the slot the harness's, the next
// frame's render draws the id buffer around that pixel and records the one-pixel copy
// (IsPickReadable), and ResolvePick - FlushAndWait, legal here for the reason
// CaptureTextureToPNG is - turns the instance slot into the entity. The pass has ONE
// slot: a pick asked for while an editor click is in it waits, and one lost to a graph
// rebuild or an editor click is asked again. A secondary view's pick is drawn by that
// view's segment, in its pixels.
void HarnessCaptureSystem::ServicePicks(World& world, std::uint64_t frame)
{
    for (const CaptureRequests::PickRequest& r : CaptureRequests::TakePicks())
    {
        PickJob p;
        p.jobId = r.jobId;
        p.x     = r.x;
        p.y     = r.y;
        p.path  = r.path;
        p.view  = r.view;
        p.lease = r.lease;
        (r.all ? m_pickImages : m_picks).push_back(p);
    }
    ServicePickImages(world, frame);
    if (m_picks.empty()) return;
    PickJob& p = m_picks.front();
    auto pop = [&] { m_picks.erase(m_picks.begin()); };

    if (!p.started)
    {
        Jobs::Start(p.jobId);
        p.started        = true;
        p.requestedFrame = frame;
    }
    std::uint32_t slot = 0, vw = 0, vh = 0;
    if (!PickView(p, slot, vw, vh))
    {
        if (p.requested) m_renderer.CancelHarnessPick();
        pop();
        return;
    }
    if (!p.requested)
    {
        if (m_renderer.IsPickBusy()) return;   // an editor click holds the slot: next frame
        const bool sized = vw != 0 && vh != 0;
        if (sized)
        {
            p.px = std::min(static_cast<std::uint32_t>(p.x * static_cast<float>(vw)), vw - 1);
            p.py = std::min(static_cast<std::uint32_t>(p.y * static_cast<float>(vh)), vh - 1);
        }
        if (!sized || !m_renderer.RequestPick(static_cast<float>(p.px), static_cast<float>(p.py),
                                              /*forHarness=*/true, slot))
        {
            // A secondary view's first picture is on its way.
            if (slot != 0 && frame <= p.requestedFrame + 120) return;
            Jobs::Fail(p.jobId, "no_view",
                       slot != 0 ? "the view rendered no picture in 120 frames"
                                 : sized ? "this run has no picking pass" : "the main view has no size yet", {});
            pop();
            return;
        }
        p.requested      = true;
        p.requestedFrame = frame;
        ++p.attempts;
        return;
    }
    if (m_renderer.IsHarnessPick() && m_renderer.IsPickReadable())
    {
        Entity e = NullEntity;
        m_renderer.ResolvePick(e);
        const bool hit = e != NullEntity && world.IsAlive(e);
        const std::uint64_t packed = hit
            ? ((static_cast<std::uint64_t>(world.GetGeneration(e) & 0x1FFFFFu) << 32) | e) : 0;
        nlohmann::json out{
            { "entity", packed }, { "name", hit ? world.GetName(e) : std::string() },
            { "x", p.x }, { "y", p.y }, { "pixel", { p.px, p.py } }, { "frame", frame } };
        if (p.view >= 0)
        {
            out["view"]  = p.view;
            out["lease"] = p.lease;
        }
        Jobs::Succeed(p.jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        pop();
        return;
    }
    if (!m_renderer.IsHarnessPick() || !m_renderer.IsPickBusy())
    {
        // Lost: a graph rebuild cleared the pass, or an editor click replaced it.
        if (p.attempts < 3) { p.requested = false; return; }
        m_renderer.CancelHarnessPick();
        Jobs::Fail(p.jobId, "pick_lost", "the pick was dropped three times (graph rebuilds or editor clicks)", {});
        pop();
        return;
    }
    if (frame > p.requestedFrame + 120)
    {
        m_renderer.CancelHarnessPick();
        Jobs::Fail(p.jobId, "pick_not_rendered", "the picking pass did not draw the point in 120 frames", {});
        pop();
    }
}

// ==== `pick {"all": true}`: the object-ID image (CaptureRequests.h) ====
namespace
{
    // A fixed color per entity - from its packed id, so an entity keeps its
    // color from image to image: a golden-ratio hue walk, bright and
    // saturated. Black is the background, white a slot no entity owns; the
    // legend, not the color, is the answer (two entities may look alike).
    std::array<std::uint8_t, 3> IdColor(std::uint64_t packed)
    {
        const double h = std::fmod(static_cast<double>(packed % 1000003u) * 0.618033988749895, 1.0) * 6.0;
        const double s = 0.75, v = 0.95;
        const int    i = static_cast<int>(h) % 6;
        const double f = h - std::floor(h);
        const double p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
        const double rgb[6][3] = { { v, t, p }, { q, v, p }, { p, v, t }, { p, q, v }, { t, p, v }, { v, p, q } };
        return { static_cast<std::uint8_t>(rgb[i][0] * 255.0 + 0.5), static_cast<std::uint8_t>(rgb[i][1] * 255.0 + 0.5),
                 static_cast<std::uint8_t>(rgb[i][2] * 255.0 + 0.5) };
    }

    bool WriteRgbaPng(const std::string& path, std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba)
    {
        DirectX::Image img{};
        img.width      = w;
        img.height     = h;
        img.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
        img.rowPitch   = static_cast<size_t>(w) * 4u;
        img.slicePitch = img.rowPitch * h;
        img.pixels     = const_cast<std::uint8_t*>(rgba.data());
        const std::wstring wpath = Sys::U8Path(path).wstring();
        return SUCCEEDED(DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
                                                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), wpath.c_str()));
    }
}

// The picking pass draws the whole view on the next frame and copies it back
// (its own buffer, beside the pick's slot); resolved at PostRender of the frame
// it became readable, against that frame's slot table - FlushAndWait is legal
// here for the reason CaptureTextureToPNG is.
void HarnessCaptureSystem::ServicePickImages(World& world, std::uint64_t frame)
{
    if (m_pickImages.empty()) return;
    PickJob& p = m_pickImages.front();
    auto pop = [&] { m_pickImages.erase(m_pickImages.begin()); };

    if (!p.started)
    {
        Jobs::Start(p.jobId);
        p.started        = true;
        p.requestedFrame = frame;
    }
    std::uint32_t viewSlot = 0, vw = 0, vh = 0;
    if (!PickView(p, viewSlot, vw, vh))
    {
        if (p.requested) m_renderer.CancelPickImage();
        pop();
        return;
    }
    if (!p.requested)
    {
        if (!m_renderer.RequestPickImage(viewSlot))
        {
            // No id texture yet (the first frames, a rebuild; a view's first picture): wait for one.
            if (frame > p.requestedFrame + 120)
            {
                Jobs::Fail(p.jobId, "no_view", viewSlot != 0 ? "the view rendered no picture in 120 frames"
                                                             : "the picking pass had no id buffer for 120 frames", {});
                pop();
            }
            return;
        }
        p.requested      = true;
        p.requestedFrame = frame;
        ++p.attempts;
        return;
    }
    if (m_renderer.IsPickImageReadable())
    {
        std::vector<std::uint32_t> slots;
        std::uint32_t w = 0, h = 0;
        if (!m_renderer.ResolvePickImage(slots, w, h) || w == 0 || h == 0)
        {
            Jobs::Fail(p.jobId, "readback_failed", "the id image could not be read back", {});
            pop();
            return;
        }

        // One pass over the pixels: per entity its pixels, box and centroid.
        struct Acc
        {
            Entity        e = NullEntity;
            std::uint64_t pixels = 0;
            std::uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
            double        sx = 0.0, sy = 0.0;
        };
        std::unordered_map<std::uint32_t, Entity> entityOfSlot;     // a slot resolved once
        std::unordered_map<Entity, Acc>           byEntity;
        std::uint64_t background = 0, unknown = 0;
        std::vector<Entity> perPixel(slots.size(), NullEntity);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
            {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const std::uint32_t v = slots[i];
                if (v == 0) { ++background; continue; }
                auto it = entityOfSlot.find(v);
                if (it == entityOfSlot.end())
                {
                    Entity e = m_renderer.PickSlotEntity(v);
                    if (e != NullEntity && !world.IsAlive(e)) e = NullEntity;
                    it = entityOfSlot.emplace(v, e).first;
                }
                const Entity e = it->second;
                if (e == NullEntity) { ++unknown; continue; }
                perPixel[i] = e;
                Acc& a = byEntity[e];
                a.e = e;
                ++a.pixels;
                a.x0 = std::min(a.x0, x); a.y0 = std::min(a.y0, y);
                a.x1 = std::max(a.x1, x); a.y1 = std::max(a.y1, y);
                a.sx += x + 0.5; a.sy += y + 0.5;
            }

        std::vector<const Acc*> order;
        order.reserve(byEntity.size());
        for (const auto& [e, a] : byEntity) order.push_back(&a);
        std::sort(order.begin(), order.end(), [](const Acc* a, const Acc* b)
                  { return a->pixels != b->pixels ? a->pixels > b->pixels : a->e < b->e; });

        const double total = static_cast<double>(w) * h;
        auto packedOf = [&](Entity e)
        { return (static_cast<std::uint64_t>(world.GetGeneration(e) & 0x1FFFFFu) << 32) | e; };
        constexpr std::size_t kMaxListed = 256;
        nlohmann::json entities = nlohmann::json::array();
        std::unordered_map<Entity, std::array<std::uint8_t, 3>> colorOf;
        for (const Acc* a : order)
        {
            const std::uint64_t packed = packedOf(a->e);
            const auto color = IdColor(packed);
            colorOf[a->e] = color;
            if (entities.size() >= kMaxListed) continue;
            const double n = static_cast<double>(a->pixels);
            entities.push_back({
                { "entity", packed }, { "name", world.GetName(a->e) }, { "pixels", a->pixels },
                { "fraction", n / total },
                { "box", { static_cast<double>(a->x0) / w, static_cast<double>(a->y0) / h,
                           static_cast<double>(a->x1 + 1) / w, static_cast<double>(a->y1 + 1) / h } },
                { "center", { a->sx / n / w, a->sy / n / h } },
                { "boxPx", { a->x0, a->y0, a->x1 + 1, a->y1 + 1 } },
                { "color", { color[0], color[1], color[2] } } });
        }
        nlohmann::json out{
            { "width", w }, { "height", h }, { "frame", frame }, { "display", { vw, vh } },
            { "background", { { "pixels", background }, { "fraction", static_cast<double>(background) / total } } },
            { "unknownPixels", unknown }, { "entityCount", byEntity.size() },
            { "truncated", byEntity.size() > kMaxListed }, { "entities", std::move(entities) } };
        if (p.view >= 0)
        {
            out["view"]  = p.view;
            out["lease"] = p.lease;
        }

        if (!p.path.empty())
        {
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
            for (std::size_t i = 0; i < slots.size(); ++i)
            {
                std::uint8_t* o = &rgba[i * 4u];
                if (slots[i] == 0)                 { o[0] = o[1] = o[2] = 0; }
                else if (perPixel[i] == NullEntity) { o[0] = o[1] = o[2] = 255; }
                else
                {
                    const auto& c = colorOf[perPixel[i]];
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
                }
                o[3] = 255;
            }
            if (!WriteRgbaPng(p.path, w, h, rgba))
            {
                Jobs::Fail(p.jobId, "write_failed", "cannot write " + p.path, {},
                           out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                pop();
                return;
            }
            out["path"] = p.path;
        }
        LOG_INFO("[CAPTURE] object-ID image: %ux%u at frame %llu, %zu entit%s, background %.1f%%%s%s", w, h,
                 static_cast<unsigned long long>(frame), byEntity.size(), byEntity.size() == 1 ? "y" : "ies",
                 100.0 * static_cast<double>(background) / total, p.path.empty() ? "" : " -> ", p.path.c_str());
        Jobs::Succeed(p.jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        pop();
        return;
    }
    if (!m_renderer.IsPickImageBusy())
    {
        // Lost: a graph rebuild dropped it (or its texture outgrew the buffer).
        if (p.attempts < 3) { p.requested = false; p.requestedFrame = frame; return; }
        Jobs::Fail(p.jobId, "pick_lost", "the id image was dropped three times (graph rebuilds)", {});
        pop();
        return;
    }
    if (frame > p.requestedFrame + 120)
    {
        m_renderer.CancelPickImage();
        Jobs::Fail(p.jobId, "pick_not_rendered", "the picking pass did not draw the id image in 120 frames", {});
        pop();
    }
}

bool HarnessCaptureSystem::PickView(const PickJob& p, std::uint32_t& slot, std::uint32_t& w, std::uint32_t& h)
{
    slot = 0;
    if (p.view < 0)
    {
        w = m_renderer.GetDisplayWidth();
        h = m_renderer.GetDisplayHeight();
        return true;
    }
    if (p.view < static_cast<std::int32_t>(Renderer::kSecondaryViewIdBase))
    {
        Jobs::Fail(p.jobId, "bad_view", "view " + std::to_string(p.view) + " is a preview view: pick reads the main "
                                        "view or a view Render.addView made", {});
        return false;
    }
    slot = static_cast<std::uint32_t>(p.view) - Renderer::kSecondaryViewIdBase;
    const Renderer::ViewInfo info = m_renderer.GetViewInfo(slot);
    if (!info.live || info.lease != p.lease)
    {
        const std::string now = !info.live ? std::string("the view was released")
                                           : "the slot now holds lease " + std::to_string(info.lease);
        Jobs::Fail(p.jobId, "stale_view", "view " + std::to_string(p.view) + " lease " + std::to_string(p.lease) +
                                          " is gone: " + now + " - read Render.views() again", {});
        return false;
    }
    w = info.width;
    h = info.height;
    return true;
}

// ==== capture {"attachment"}: a render target as data (CaptureRequests.h) ====
// At PostRender, like view_output. ServiceAttachment's drain parks the render lane, so
// the graph's tracked states are final when read (the readback transitions from them
// and back), and its hard sync makes the texels THIS frame's.
void HarnessCaptureSystem::AddMainViewMetadata(World& world, nlohmann::json& into)
{
    const Renderer::AttachmentCamera c = m_renderer.GetAttachmentCamera();
    // The same fields as an attachment manifest's camera (AttachmentDecode).
    into["camera"] = {
        { "nearZ", c.nearZ }, { "farZ", c.farZ }, { "fovY", c.fovY }, { "orthographic", c.orthographic },
        { "position", { c.position[0], c.position[1], c.position[2] } },
        { "forward", { c.forward[0], c.forward[1], c.forward[2] } },
        { "renderWidth", c.renderWidth }, { "renderHeight", c.renderHeight } };
    into["worldEpoch"] = world.Epoch();
    into["hdrMode"]    = HDROutputModeToString(m_gfx.MainSurface().GetHDROutputMode());
    into["renderSize"] = { m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight() };
    into["outputSize"] = { m_gfx.GetWidth(), m_gfx.GetHeight() };
    float exposure = 0.f;
    bool  autoExposure = false;
    std::string why;
    if (m_renderer.ReadFrameExposure(exposure, autoExposure, why) && std::isfinite(exposure) && exposure > 0.f)
        into["exposure"] = { { "value", exposure }, { "ev", std::log2(exposure) },
                             { "mode", autoExposure ? "auto" : "manual" } };
    else
        into["exposure"] = { { "error", why.empty() ? std::string("not a positive finite value") : why } };
    // What the main view's picture WAS: null = the lit image; otherwise the
    // render target the debug target view painted over it (Renderer/
    // DebugTargets.h) - a view_output capture of it is that target, mapped.
    const DebugTargets::Shown shown = m_renderer.GetDebugTargetShown();
    if (shown.active)
        into["debugTarget"] = {
            { "name", shown.view.name }, { "mode", DebugTargets::ModeName(shown.view.mode) },
            { "channel", shown.view.channel }, { "min", shown.view.rangeMin }, { "max", shown.view.rangeMax },
            { "exposure", shown.view.exposure }, { "scale", shown.view.scale },
            { "available", shown.available }, { "why", shown.why }, { "frame", shown.frame } };
    else
        into["debugTarget"] = nullptr;
}

void HarnessCaptureSystem::ServiceAttachment(World& world, std::uint64_t jobId, const std::string& attachment,
                                             const std::string& path, std::uint64_t afterFrame,
                                             std::uint64_t frame, const PassCapture::Status* afterPass)
{
    Jobs::Start(jobId);
    auto fail = [&](const char* code, const std::string& why)
    {
        LOG_WARNING("Harness: attachment capture job %llu ('%s') at frame %llu failed: %s",
                    static_cast<unsigned long long>(jobId), attachment.c_str(),
                    static_cast<unsigned long long>(frame), why.c_str());
        Jobs::Fail(jobId, code, why, {});
    };

    m_gfx.DrainCpuFrame();
    Renderer::AttachmentSource src;
    std::string why;
    IGraphicsDevice::RawTexture raw;
    if (afterPass)
    {
        // The pass capture's copy (with the texture's own desc), read back as the texture
        // itself would be.
        std::string   graphName;
        std::uint32_t plane = 0;
        if (!m_renderer.ResolvePassCaptureTarget(attachment, graphName, plane, src.info, why))
            return fail("unavailable", why);
        RHI::ResourceState state = RHI::ResourceState::UNDEFINED;
        const RHI::Texture* copy = m_renderer.GetPassCaptureCopy(state);
        if (!copy) return fail("capture_failed", "the pass capture's copy is gone");
        if (!m_gfx.ReadTextureRaw(*copy, state, plane, raw, 0))
            return fail("capture_failed", "the readback failed: " + raw.error);
    }
    else
    {
        // Any name the debug target registry holds (Renderer/DebugTargets.h):
        // the GBuffer, hdr, the AO / shadow masks, a render-graph texture.
        if (!m_renderer.GetAttachmentSource(attachment, src, why))
            return fail("unavailable", why);
        const bool read = src.deviceHdr ? m_gfx.ReadHdrSceneRaw(raw)
                                        : m_gfx.ReadTextureRaw(*src.texture, src.state, src.plane, raw,
                                                               src.arraySlice);
        if (!read) return fail("capture_failed", "the readback failed: " + raw.error);
    }

    const Renderer::AttachmentCamera rc = m_renderer.GetAttachmentCamera();
    Attachments::Camera cam;
    cam.nearZ        = rc.nearZ;
    cam.farZ         = rc.farZ;
    cam.fovY         = rc.fovY;
    cam.orthographic = rc.orthographic;
    for (int i = 0; i < 3; ++i)
    {
        cam.position[i] = rc.position[i];
        cam.forward[i]  = rc.forward[i];
    }
    cam.renderWidth  = rc.renderWidth;
    cam.renderHeight = rc.renderHeight;
    // The registry's own name for it ("GBuffer1_Normal" is "normal"), and its
    // semantics for a target the decoder has no words of its own for.
    const std::string& name = src.info.name;
    Attachments::Decoded dec;
    if (!Attachments::Decode(name, raw, cam, dec, &src.info)) return fail("capture_failed", dec.error);
    if (name != attachment) dec.manifest["requested"] = attachment;

    // <stem>.bin and <stem>.json beside the .png preview at `path`.
    std::filesystem::path stem = Sys::U8Path(path);
    stem.replace_extension();
    const std::string binPath  = Sys::PathToUtf8(stem) + ".bin";
    const std::string jsonPath = Sys::PathToUtf8(stem) + ".json";
    dec.manifest["renderedFrame"] = frame;
    if (afterPass)   // the pass it followed, the frame it was copied in
    {
        dec.manifest["afterPass"]   = afterPass->afterPass;
        dec.manifest["copiedFrame"] = afterPass->frame;
    }
    dec.manifest["files"] = { { "raw", binPath }, { "preview", path } };
    AddMainViewMetadata(world, dec.manifest);   // + worldEpoch, exposure, debugTarget, hdrMode, sizes
    {
        std::ofstream f(Sys::U8Path(binPath), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(raw.bytes.data()), static_cast<std::streamsize>(raw.bytes.size()));
        if (!f.good()) return fail("capture_failed", "cannot write " + binPath);
    }
    {
        std::ofstream f(Sys::U8Path(jsonPath), std::ios::binary | std::ios::trunc);
        f << dec.manifest.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!f.good()) return fail("capture_failed", "cannot write " + jsonPath);
    }
    if (!WriteRgbaPng(path, raw.width, raw.height, dec.previewRgba))
        return fail("capture_failed", "cannot write " + path);

    nlohmann::json out{
        { "path", path }, { "raw", binPath }, { "manifest", jsonPath },
        { "source", "attachment" }, { "attachment", name }, { "semantic", dec.manifest["semantic"] },
        { "view", "main" }, { "afterFrame", afterFrame }, { "renderedFrame", frame },
        { "width", raw.width }, { "height", raw.height }, { "format", dec.manifest["format"] },
        { "channels", dec.manifest["channels"] },
        { "nonFiniteTexels", dec.manifest["preview"]["nonFiniteTexels"] } };
    if (name != attachment) out["requested"] = attachment;
    for (const char* key : { "camera", "worldEpoch", "exposure", "hdrMode", "renderSize", "outputSize",
                             "debugTarget" })
        out[key] = dec.manifest[key];
    if (afterPass)
    {
        out["afterPass"]   = afterPass->afterPass;
        out["copiedFrame"] = afterPass->frame;
    }
    Jobs::Succeed(jobId, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    LOG_INFO("[CAPTURE] attachment '%s'%s%s %ux%u (%s) at frame %llu -> %s", name.c_str(),
             afterPass ? " after " : "", afterPass ? afterPass->afterPass.c_str() : "", raw.width,
             raw.height, dec.manifest["format"].get<std::string>().c_str(), static_cast<unsigned long long>(frame),
             path.c_str());
}

// ==== -probedump, then capture {"attachment", "afterPass"} ====
void HarnessCaptureSystem::ServiceProbeDumps(std::uint64_t frame)
{
    if (m_probeDumpDir.empty()) return;
    // One acquire load on the frames nothing was baked (the common case).
    const std::uint64_t count = m_renderer.ProbeBakeCount();
    if (count == m_probeDumped) return;
    // The bakes since the last service; the ring keeps the last kProbeBakeLog.
    std::uint64_t first = m_probeDumped;
    std::uint64_t missed = 0;
    if (count - first > Renderer::kProbeBakeLog)
    {
        missed = count - first - Renderer::kProbeBakeLog;
        first  = count - Renderer::kProbeBakeLog;
    }
    m_probeDumped = count;

    // The bakes were recorded by render halves; drained, their records are
    // stable and the readback below (synchronous) queues behind their GPU work.
    // The probe array keeps a slice until it is baked again, so a slice baked
    // twice since the last service reads as its latest bake both times.
    m_gfx.DrainCpuFrame();
    const RHI::Texture& probes = m_renderer.GetProbeArrayTexture();
    std::error_code ec;
    std::filesystem::create_directories(Sys::U8Path(m_probeDumpDir), ec);

    // A probe face as the decoder describes it: no registry entry of its own.
    DebugTargets::Info info;
    info.name     = "probe";
    info.label    = "Reflection probe face";
    info.group    = "Lighting";
    info.semantic = "radiance";
    info.meaning  = "a reflection probe face as baked: mip 0 of the prefiltered cube, linear radiance; "
                    "a = geometry coverage (0 where only the sky was drawn)";
    info.channels = "r,g,b,coverage";
    const Attachments::Camera cam{};
    for (std::uint64_t n = first; n < count; ++n)
    {
        const Renderer::ProbeBakeRecord rec = m_renderer.ProbeBake(n);
        auto fault = [&](const std::string& why)
        {
            LOG_ERROR("Harness: -probedump of probe %u (slice %u, baked at frame %llu) failed: %s",
                      static_cast<unsigned>(rec.entity), rec.slice, static_cast<unsigned long long>(rec.frame),
                      why.c_str());
            RunLedger::Get().NoteScriptFault("-probedump", why.c_str());
        };
        if (!probes.IsValid()) return fault("the probe array does not exist");
        bool ok = true;
        for (std::uint32_t face = 0; face < 6 && ok; ++face)
        {
            IGraphicsDevice::RawTexture raw;
            if (!m_gfx.ReadTextureRaw(probes, RHI::ResourceState::SHADER_RESOURCE, 0, raw, rec.slice * 6u + face))
            { fault("face " + std::to_string(face) + ": " + raw.error); ok = false; break; }
            Attachments::Decoded dec;
            if (!Attachments::Decode("probe", raw, cam, dec, &info))
            { fault("face " + std::to_string(face) + ": " + dec.error); ok = false; break; }
            const std::string stem = m_probeDumpDir + "/probe" + std::to_string(static_cast<unsigned>(rec.entity)) +
                                     "_s" + std::to_string(rec.slice) + "_f" + std::to_string(face);
            // kept / copies: what this face's CullView drew, and of that the probe's own copies of
            // what the main view dropped.
            dec.manifest["probe"] = { { "entity", static_cast<unsigned>(rec.entity) }, { "slice", rec.slice },
                                      { "face", face }, { "bakedFrame", rec.frame }, { "bake", n },
                                      { "kept", rec.kept[face] }, { "copies", rec.copies[face] } };
            dec.manifest["renderedFrame"] = frame;
            dec.manifest["files"] = { { "raw", stem + ".bin" }, { "preview", stem + ".png" } };
            {
                std::ofstream f(Sys::U8Path(stem + ".bin"), std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(raw.bytes.data()), static_cast<std::streamsize>(raw.bytes.size()));
                if (!f.good()) { fault("cannot write " + stem + ".bin"); ok = false; break; }
            }
            {
                std::ofstream f(Sys::U8Path(stem + ".json"), std::ios::binary | std::ios::trunc);
                f << dec.manifest.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
                if (!f.good()) { fault("cannot write " + stem + ".json"); ok = false; break; }
            }
            if (!WriteRgbaPng(stem + ".png", raw.width, raw.height, dec.previewRgba))
            { fault("cannot write " + stem + ".png"); ok = false; break; }
        }
        if (!ok) continue;
        LOG_INFO("[PROBE_DUMP] entity=%u slice=%u baked=%llu bake=%llu frame=%llu faces=6 missed=%llu "
                 "kept=%u/%u/%u/%u/%u/%u copies=%u/%u/%u/%u/%u/%u dir=%s",
                 static_cast<unsigned>(rec.entity), rec.slice, static_cast<unsigned long long>(rec.frame),
                 static_cast<unsigned long long>(n), static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(missed), rec.kept[0], rec.kept[1], rec.kept[2], rec.kept[3],
                 rec.kept[4], rec.kept[5], rec.copies[0], rec.copies[1], rec.copies[2], rec.copies[3], rec.copies[4],
                 rec.copies[5], m_probeDumpDir.c_str());
    }
}

void HarnessCaptureSystem::ServiceAfterPassCapture(World& world, const CaptureRequests::Request& r,
                                                   std::uint64_t frame)
{
    constexpr std::uint32_t kWaits = 12;
    auto fail = [&](const char* code, const std::string& why)
    {
        Jobs::Start(r.jobId);
        LOG_WARNING("Harness: capture job %llu ('%s' after '%s') at frame %llu failed: %s",
                    static_cast<unsigned long long>(r.jobId), r.attachment.c_str(), r.afterPass.c_str(),
                    static_cast<unsigned long long>(frame), why.c_str());
        Jobs::Fail(r.jobId, code, why, {});
    };
    auto again = [&](CaptureRequests::Request next)
    {
        next.notBefore = frame + 1;
        CaptureRequests::Requeue(std::move(next));
    };
    if (!r.passCaptureAsked)
    {
        // Asked once: the next frame boundary arms the main graph, that frame copies.
        std::string        graphName, why;
        std::uint32_t      plane = 0;
        DebugTargets::Info info;
        if (!m_renderer.ResolvePassCaptureTarget(r.attachment, graphName, plane, info, why))
            return fail("unavailable", why);
        PassCapture::Desc d;
        d.texture   = graphName;
        d.afterPass = r.afterPass;
        d.id        = r.jobId;
        m_renderer.RequestPassCapture(d);
        CaptureRequests::Request next = r;
        next.passCaptureAsked = true;
        again(std::move(next));
        return;
    }
    const PassCapture::Status st = m_renderer.GetPassCaptureStatus();
    if (st.id != r.jobId || st.pending)
    {
        if (r.passCaptureWaits >= kWaits)
            return fail("unavailable", "no copy within " + std::to_string(kWaits) + " frames" +
                                       (st.id != r.jobId ? std::string(" (another request took the pass capture)")
                                                         : st.why.empty() ? std::string() : ": " + st.why));
        CaptureRequests::Request next = r;
        ++next.passCaptureWaits;
        again(std::move(next));
        return;
    }
    if (!st.done) return fail("unavailable", st.why);
    ServiceAttachment(world, r.jobId, r.attachment, r.path, r.afterFrame, frame, &st);
}

}  // namespace Harness

#endif  // WITH_HARNESS
