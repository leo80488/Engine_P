#include "Harness/CaptureRequests.h"

#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

#include "Harness/AttachmentDecode.h"   // the attachment names
#include "Harness/ControlChannelJobs.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/PathUtil.h"
#include "Platform/MainWindow.h"        // MainWindowMinimized: an app_ui picture of a minimized window

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>

namespace Harness::CaptureRequests
{
namespace
{
    using Json = nlohmann::json;

    std::mutex           g_mutex;
    std::vector<Request> g_queue;
    std::atomic<bool>    g_serviced{ false };
    constexpr std::size_t kMaxQueued = 16;   // each one is a hard GPU sync

    std::string Dump(const Json& j)
    {
        return j.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    std::string Error(const char* code, const std::string& message)
    {
        return Dump(Json{ { "ok", false }, { "verb", "capture" }, { "error", code }, { "message", message },
                          { "usage", "capture {\"path\": \"<absolute .png>\", \"afterFrame\": <frame; default: "
                                     "the next one>, \"camera\": <entity id; default: the main view>, "
                                     "\"width\": <px>, \"height\": <px> (the camera's picture; default 640x360), "
                                     "\"source\": \"view_output\" (default: the view's picture, no UI) | "
                                     "\"app_ui\" (the whole window as presented, UI included), \"panel\": "
                                     "\"<editor panel>\" (with app_ui: that panel's part of it), \"view\": <preview "
                                     "view id>, \"lease\": <its lease> (both from Render.views()), "
                                     "\"afterRenders\": <that view's picture count to wait for>, \"attachment\": "
                                     "\"<a render target: albedo | normal | surface | depth | velocity | extra | "
                                     "hdr | ssao | rtao | rtshadow | a render-graph texture's name - Lua "
                                     "Render.targets() lists them>\" (the main view's render target as data: .bin "
                                     "+ .json beside the .png preview), \"afterPass\": \"<a pass's name; \\\"\\\" = "
                                     "the last pass that runs>\" (with attachment: the texture right after that "
                                     "pass - any main-graph texture's name)}" } });
    }
}

void SetServiced(bool serviced)
{
    g_serviced.store(serviced, std::memory_order_release);
}

std::string ServeVerb(const std::string& args, std::uint64_t currentFrame)
{
    if (!g_serviced.load(std::memory_order_acquire))
        return Error("unavailable", "no capture service in this run: it needs a harness build started "
                                    "with -artifacts=<dir> (HarnessCaptureSystem)");

    const Json req = Json::parse(args, nullptr, false);
    if (!req.is_object())
        return Error("bad_request", "the argument is one JSON object");
    for (const auto& [key, value] : req.items())
        if (key != "path" && key != "afterFrame" && key != "camera" && key != "width" && key != "height" &&
            key != "source" && key != "view" && key != "lease" && key != "afterRenders" && key != "attachment" &&
            key != "panel" && key != "afterPass")
            return Error("bad_request", "unknown key '" + key + "'");
    if (!req.contains("path") || !req["path"].is_string())
        return Error("bad_request", "\"path\" (a string) is required");

    // A render target as data: the main view's, so nothing that names another view or another
    // source goes with it.
    std::string attachment;
    // Right after a pass: the copy the main graph makes there (Renderer/PassCapture.h); "" = after
    // the last pass that runs.
    const bool  afterPassGiven = req.contains("afterPass");
    std::string afterPass;
    if (afterPassGiven)
    {
        if (!req["afterPass"].is_string())
            return Error("bad_request", "\"afterPass\" is a pass's name (a string; \"\" = the last pass that runs)");
        if (!req.contains("attachment"))
            return Error("bad_request", "\"afterPass\" goes with \"attachment\": the texture to read after that pass");
        afterPass = req["afterPass"].get<std::string>();
    }
    if (req.contains("attachment"))
    {
        if (!req["attachment"].is_string())
            return Error("bad_request", "\"attachment\" is a string");
        attachment = req["attachment"].get<std::string>();
        // The debug target registry's names (Renderer/DebugTargets.h), as the renderer published them;
        // whether the target exists is the service's question, at the frame it runs. After a pass any
        // main-graph texture's name goes too - the service resolves it against the graph.
        if (!afterPassGiven && !Attachments::IsKnown(attachment))
        {
            std::string names;
            for (const std::string& n : Attachments::Names()) names += (names.empty() ? "" : ", ") + n;
            return Error("unsupported_source", "no attachment '" + attachment + "' - this engine reads " + names);
        }
        for (const char* other : { "source", "camera", "width", "height", "view", "lease", "afterRenders" })
            if (req.contains(other))
                return Error("bad_request", std::string("\"attachment\" is the main view's render target: no \"") +
                                            other + "\" with it");
    }

    // The source: what the picture is of. view_output = the view's own picture (the ToneMap output:
    // no editor UI); app_ui = the window as presented - the editor's panels, or the game's final frame.
    bool appUi = false;
    if (req.contains("source"))
    {
        if (!req["source"].is_string())
            return Error("bad_request", "\"source\" is a string");
        const std::string source = req["source"].get<std::string>();
        if (source == "app_ui")
            appUi = true;
        else if (source != "view_output")
            return Error("unsupported_source", "no source '" + source + "' - this engine captures "
                                               "\"view_output\" and \"app_ui\"");
        if (appUi && (req.contains("camera") || req.contains("width") || req.contains("height")))
            return Error("bad_request", "\"app_ui\" is the whole window as presented: no \"camera\", "
                                        "\"width\" or \"height\" with it");
        // -hidden minimizes the window: a minimized editor gives ImGui a 0x0
        // display and draws no panels at all - the picture would be blank.
        const Sys::CommandLine& cl = Sys::CommandLine::Get();
        if (appUi && Sys::IsEditorHost() && cl.Has("-hidden") && !cl.Has("-hidden_ui"))
            return Error("unavailable", "this editor runs -hidden without -hidden_ui: its window is minimized "
                                        "and ImGui draws nothing, so the window holds no panels to capture - "
                                        "launch it with -hidden_ui (the UI laid out at the back buffer's size) "
                                        "or visible");
        // A window the person minimized (an editor they opened, the channel opened from its Tools menu)
        // presents nothing new, so a picture of it would be whatever was last drawn. The main view keeps
        // rendering minimized and still captures.
        if (appUi && !cl.Has("-hidden") && Plat::MainWindowMinimized())
            return Error("window_minimized", "the window is minimized, so it presents nothing to capture - "
                                             "ask the person to restore it, or capture the main view "
                                             "(no \"source\")");
    }

    // One editor panel: a crop of the window as presented.
    std::string panel;
    if (req.contains("panel"))
    {
        if (!req["panel"].is_string() || req["panel"].get<std::string>().empty())
            return Error("bad_request", "\"panel\" is a panel's name - its title (\"Inspector\") or ImGui's full "
                                        "name");
        if (!appUi)
            return Error("bad_request", "\"panel\" crops the window as presented: it goes with \"source\": "
                                        "\"app_ui\"");
        panel = req["panel"].get<std::string>();
    }

    // An existing preview view: {"view": <id>, "lease": <n>}, both from Render.views(). The pair
    // names one tenancy of the slot, so a view closed since (or its id handed to another panel) is
    // refused stale_view by the service rather than captured as whoever holds the id now.
    std::int32_t  view = -1;
    std::uint32_t lease = 0;
    std::uint64_t afterRenders = 0;
    if (req.contains("view") || req.contains("lease") || req.contains("afterRenders"))
    {
        if (!req.contains("view") || !req.contains("lease"))
            return Error("bad_request", "an existing view is {\"view\": <id>, \"lease\": <n>} - both from "
                                        "Render.views()");
        const Json& v = req["view"];
        const Json& l = req["lease"];
        if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 63)
            return Error("bad_request", "\"view\" is a preview view's id (Render.views())");
        if (!l.is_number_integer() || l.get<long long>() <= 0 || l.get<long long>() > 0xFFFFFFFFll)
            return Error("bad_request", "\"lease\" is the view's lease (a positive integer, Render.views())");
        if (appUi || req.contains("camera") || req.contains("width") || req.contains("height"))
            return Error("bad_request", "\"view\" is an existing view's own picture: no \"camera\", \"width\", "
                                        "\"height\" or \"source\": \"app_ui\" with it");
        if (req.contains("afterRenders"))
        {
            const Json& a = req["afterRenders"];
            if (!a.is_number_integer() || a.get<long long>() < 0)
                return Error("bad_request", "\"afterRenders\" is the view's picture count to wait for (an "
                                            "integer >= 0; Render.views() `renders` + 1 = the next one)");
            afterRenders = static_cast<std::uint64_t>(a.get<long long>());
        }
        view  = static_cast<std::int32_t>(v.get<long long>());
        lease = static_cast<std::uint32_t>(l.get<long long>());
    }

    const std::string path = req["path"].get<std::string>();
    const std::filesystem::path p = Sys::U8Path(path);
    if (!p.is_absolute())
        return Error("bad_path", "the path must be absolute: " + path);
    std::string ext = Sys::PathToUtf8(p.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".png")
        return Error("bad_path", "the path must end in .png: " + path);

    std::uint64_t after = currentFrame + 1;   // default: a frame rendered after this request
    if (req.contains("afterFrame"))
    {
        const Json& a = req["afterFrame"];
        if (!a.is_number_integer() || a.get<long long>() < 0)
            return Error("bad_request", "\"afterFrame\" is a frame index (an integer >= 0)");
        after = static_cast<std::uint64_t>(a.get<long long>());
    }

    // A named camera: its id is checked where the world is (the service), the
    // numbers here.
    std::uint64_t camera = 0;
    std::uint32_t width = 0, height = 0;
    if (req.contains("camera"))
    {
        const Json& c = req["camera"];
        if (!c.is_number_integer() || c.get<long long>() <= 0)
            return Error("bad_request", "\"camera\" is an entity id (a positive integer)");
        camera = static_cast<std::uint64_t>(c.get<long long>());
        width  = 640;
        height = 360;
    }
    if (req.contains("width") || req.contains("height"))
    {
        if (!camera)
            return Error("bad_request", "\"width\" / \"height\" size a camera's picture: give \"camera\" too");
        if (!req.contains("width") || !req.contains("height"))
            return Error("bad_request", "give both \"width\" and \"height\"");
        const Json& w = req["width"];
        const Json& h = req["height"];
        if (!w.is_number_integer() || !h.is_number_integer() || w.get<long long>() < 16 ||
            h.get<long long>() < 16 || w.get<long long>() > 4096 || h.get<long long>() > 4096)
            return Error("bad_request", "\"width\" and \"height\" are integers in 16..4096");
        width  = static_cast<std::uint32_t>(w.get<long long>());
        height = static_cast<std::uint32_t>(h.get<long long>());
    }

    // Only a request that is otherwise valid may create its directory.
    std::error_code ec;
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    if (!std::filesystem::is_directory(p.parent_path(), ec))
        return Error("bad_path", "cannot create the directory of " + path);

    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_queue.size() >= kMaxQueued)
            return Error("queue_full", std::to_string(kMaxQueued) + " captures are already waiting");
        id = Jobs::TryCreate("capture", path, 0);
        if (!id) return Error("busy", "active job admission limit reached");
        Request r{ id, path, after, camera, width, height, appUi, view, lease, afterRenders, attachment, panel };
        r.afterPassGiven = afterPassGiven;
        r.afterPass      = afterPass;
        g_queue.push_back(std::move(r));
    }
    Json ok{ { "ok", true }, { "verb", "capture" }, { "jobId", id }, { "afterFrame", after }, { "path", path },
             { "source", !attachment.empty() ? "attachment" : appUi ? "app_ui" : "view_output" } };
    if (!attachment.empty()) ok["attachment"] = attachment;
    if (afterPassGiven) ok["afterPass"] = afterPass;
    if (!panel.empty()) ok["panel"] = panel;
    if (camera) ok["camera"] = camera;
    if (view >= 0)
    {
        ok["view"]  = view;
        ok["lease"] = lease;
    }
    return Dump(ok);
}

namespace
{
    std::vector<PickRequest> g_picks;   // under g_mutex

    std::string PickError(const char* code, const std::string& message)
    {
        return Dump(Json{ { "ok", false }, { "verb", "pick" }, { "error", code }, { "message", message },
                          { "usage", "pick {\"x\": <0..1 from the left>, \"y\": <0..1 from the top>} - which "
                                     "entity the main view draws there; pick {\"all\": true [, \"path\": "
                                     "\"<absolute .png>\"]} - every entity it draws, where and how much (the "
                                     "object-ID image); either with \"view\": <id>, \"lease\": <n> (from "
                                     "Render.views()): a view Render.addView made instead of the main view" } });
    }

    // "" when @p path is an absolute .png whose directory exists - made here,
    // so only an otherwise valid request may call it - else why not.
    std::string PngPathProblem(const std::string& path)
    {
        const std::filesystem::path p = Sys::U8Path(path);
        if (!p.is_absolute())
            return "the path must be absolute: " + path;
        std::string ext = Sys::PathToUtf8(p.extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png")
            return "the path must end in .png: " + path;
        std::error_code ec;
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
        if (!std::filesystem::is_directory(p.parent_path(), ec))
            return "cannot create the directory of " + path;
        return {};
    }
}

std::string ServePickVerb(const std::string& args)
{
    if (!g_serviced.load(std::memory_order_acquire))
        return PickError("unavailable", "no pick service in this run: it needs a harness build started "
                                        "with -artifacts=<dir> (HarnessCaptureSystem)");
    const Json req = Json::parse(args, nullptr, false);
    if (!req.is_object())
        return PickError("bad_request", "the argument is one JSON object");
    for (const auto& [key, value] : req.items())
        if (key != "x" && key != "y" && key != "all" && key != "path" && key != "view" && key != "lease")
            return PickError("bad_request", "unknown key '" + key + "'");
    if (req.contains("all") && !req["all"].is_boolean())
        return PickError("bad_request", "\"all\" is true or false");
    // A secondary view: {"view": <id>, "lease": <n>}, both from Render.views(), naming one tenancy
    // of the slot (a view released since is refused stale_view by the service).
    std::int32_t  view = -1;
    std::uint32_t lease = 0;
    if (req.contains("view") || req.contains("lease"))
    {
        if (!req.contains("view") || !req.contains("lease"))
            return PickError("bad_request", "a view is {\"view\": <id>, \"lease\": <n>} - both from "
                                            "Render.views()");
        const Json& v = req["view"];
        const Json& l = req["lease"];
        if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 1024)
            return PickError("bad_request", "\"view\" is a view's id (Render.views())");
        if (!l.is_number_integer() || l.get<long long>() < 1 || l.get<long long>() > 0xFFFFFFFFll)
            return PickError("bad_request", "\"lease\" is the view's lease (a positive integer, Render.views())");
        view  = static_cast<std::int32_t>(v.get<long long>());
        lease = static_cast<std::uint32_t>(l.get<long long>());
    }
    if (req.value("all", false))
    {
        // The object-ID image: the whole view, so no point.
        if (req.contains("x") || req.contains("y"))
            return PickError("bad_request", "\"all\" is the whole view: no \"x\" / \"y\" with it");
        std::string path;
        if (req.contains("path"))
        {
            if (!req["path"].is_string())
                return PickError("bad_request", "\"path\" is a string");
            path = req["path"].get<std::string>();
            if (const std::string problem = PngPathProblem(path); !problem.empty())
                return PickError("bad_path", problem);
        }
        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_picks.size() >= kMaxQueued)
                return PickError("queue_full", std::to_string(kMaxQueued) + " picks are already waiting");
            id = Jobs::TryCreate("pick", path.empty() ? std::string("all") : "all " + path, 0);
            if (!id) return PickError("busy", "active job admission limit reached");
            g_picks.push_back(PickRequest{ id, 0.f, 0.f, true, path, view, lease });
        }
        Json ok{ { "ok", true }, { "verb", "pick" }, { "jobId", id }, { "all", true } };
        if (!path.empty()) ok["path"] = path;
        if (view >= 0)
        {
            ok["view"]  = view;
            ok["lease"] = lease;
        }
        return Dump(ok);
    }
    if (req.contains("path"))
        return PickError("bad_request", "\"path\" writes the object-ID image: it goes with \"all\": true");
    if (!req.contains("x") || !req.contains("y") || !req["x"].is_number() || !req["y"].is_number())
        return PickError("bad_request", "\"x\" and \"y\" (numbers) are required");
    const double x = req["x"].get<double>(), y = req["y"].get<double>();
    if (!(x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0))
        return PickError("bad_request", "\"x\" and \"y\" are fractions of the view: 0..1");
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_picks.size() >= kMaxQueued)
            return PickError("queue_full", std::to_string(kMaxQueued) + " picks are already waiting");
        id = Jobs::TryCreate("pick", std::to_string(x) + "," + std::to_string(y), 0);
        if (!id) return PickError("busy", "active job admission limit reached");
        g_picks.push_back(PickRequest{ id, static_cast<float>(x), static_cast<float>(y), false, {}, view, lease });
    }
    Json ok{ { "ok", true }, { "verb", "pick" }, { "jobId", id }, { "x", x }, { "y", y } };
    if (view >= 0)
    {
        ok["view"]  = view;
        ok["lease"] = lease;
    }
    return Dump(ok);
}

std::vector<PickRequest> TakePicks()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<PickRequest> out = std::move(g_picks);
    g_picks.clear();
    return out;
}

void Requeue(Request r)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_queue.push_back(std::move(r));
}

std::vector<Request> TakeDue(std::uint64_t frame)
{
    std::vector<Request> due;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto it = g_queue.begin(); it != g_queue.end();)
    {
        if (it->afterFrame <= frame && it->notBefore <= frame)
        {
            due.push_back(std::move(*it));
            it = g_queue.erase(it);
        }
        else
            ++it;
    }
    return due;
}
} // namespace Harness::CaptureRequests

#endif // WITH_HARNESS && WITH_CONTROL
