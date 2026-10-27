// The -chainpreview_* harness flags: see ChainPhysicsPreviewHarness.h.

#include "Editor/ChainPhysicsPreviewHarness.h"

#if defined(WITH_HARNESS)

#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Harness/ExitCodes.h"
#include "Harness/FlagRefusal.h"
#include "Resource/AssetRefs.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// The -preview_* discipline exactly: a BARE value flag is exit 12 before a
// frame is drawn; a MALFORMED value is an ENGINE_CHECK and a documented
// default. Two refusals are specific to this panel: -chainpreview_size is
// REQUIRED beside a subject (under -hidden the panel's content region is <= 0,
// so an unpinned run could never exercise a size), and any other
// -chainpreview_* flag without a subject refuses the run (the panel would
// never open).
void Editor::ChainPreviewHarness::ApplyBootFlags(const Sys::CommandLine& pcl)
{
    auto needValue = [&pcl](const char* flag, const char* usage)
    {
        Harness::RequireValue(pcl, flag, usage);
    };
    auto& chainPanel = Editor::ChainPhysicsPreviewWindow::Get();
    needValue("-chainpreview_subject",  "-chainpreview_subject=<path.ipfb>");
    needValue("-chainpreview_size",     "-chainpreview_size=<w>x<h>");
    needValue("-chainpreview_frames",   "-chainpreview_frames=<editor frames>");
    needValue("-chainpreview_close_at", "-chainpreview_close_at=<editor frame>");
    needValue("-chainpreview_open_at",  "-chainpreview_open_at=<editor frame>");
    needValue("-chainpreview_clip",     "-chainpreview_clip=<name|path.ianim>");
    needValue("-chainpreview_time",     "-chainpreview_time=<seconds>");
    // The preview lane: dt, sim, reset, dump.
    needValue("-chainpreview_dt",       "-chainpreview_dt=<seconds, 0..1/30>");
    needValue("-chainpreview_sim",      "-chainpreview_sim=<0|1>");
    needValue("-chainpreview_reset_at", "-chainpreview_reset_at=<editor frame>");
    needValue("-chainpreview_dump",     "-chainpreview_dump=<file.json>");
    // The parameter injector and the stage-edit probe.
    needValue("-chainpreview_param",    "-chainpreview_param=<key>=<value>[,<key>=<value>...]");
    needValue("-chainpreview_stage_edit_at", "-chainpreview_stage_edit_at=<editor frame>");
    // The stage clock + the simulation transport.
    needValue("-chainpreview_timescale", "-chainpreview_timescale=<0.1..2.0>");
    needValue("-chainpreview_steps",    "-chainpreview_steps=<n>[,<n>...] (each 0..4)");
    needValue("-chainpreview_realdt",   "-chainpreview_realdt=<sec>[,<sec>...]");
    needValue("-chainpreview_pause_at", "-chainpreview_pause_at=<editor frame>");
    needValue("-chainpreview_step_at",  "-chainpreview_step_at=<editor frame>");
    needValue("-chainpreview_ab",       "-chainpreview_ab=<seconds>[:steps=<list>|:realdt=<list>]");
    // The overlay + the panel-undo hooks.
    needValue("-chainpreview_overlay",      "-chainpreview_overlay=<hexmask>[@<editor frame>][,...]");
    needValue("-chainpreview_overlay_shot", "-chainpreview_overlay_shot=<file.png>@<editor frame>[,...]");
    needValue("-chainpreview_undo_at",      "-chainpreview_undo_at=<editor frame>[,...]");
    needValue("-chainpreview_redo_at",      "-chainpreview_redo_at=<editor frame>[,...]");
    needValue("-chainpreview_fullreset_at", "-chainpreview_fullreset_at=<editor frame>[,...]");
    // Scene, forces, driver (-chainpreview_nogravity and -chainpreview_drive_off
    // are switches).
    needValue("-chainpreview_scale",     "-chainpreview_scale=<scale>|auto");
    needValue("-chainpreview_floor",     "-chainpreview_floor=<0|1|collide>[:<gap>]");
    needValue("-chainpreview_gravity",   "-chainpreview_gravity=<x,y,z>[:<scale>][@<editor frame>]");
    needValue("-chainpreview_wind",      "-chainpreview_wind=<x,y,z>:<strength>[:<freq>[:<turb>]][@<editor frame>]");
    needValue("-chainpreview_drive",     "-chainpreview_drive=<move>,<rotDegPerSec>|rotate:<deg/s>|move:<u/s>[@<editor frame>]");
    needValue("-chainpreview_teleport_at", "-chainpreview_teleport_at=<editor frame>[:<dist>[:<angleDeg>[:reset]]][,...]");
    needValue("-chainpreview_teleport_reset", "-chainpreview_teleport_reset=<0|1>");
    needValue("-chainpreview_scale_at",  "-chainpreview_scale_at=<editor frame>:<scale>[,...]");
    needValue("-chainpreview_stress",    "-chainpreview_stress=<yawsweep|teleport|idle|walkrun>");
    needValue("-chainpreview_shot",      "-chainpreview_shot=<file.png>@<editor frame>[,...]");
    needValue("-chainpreview_follow",    "-chainpreview_follow=<entity name>");
    needValue("-chainpreview_follow_kill_at", "-chainpreview_follow_kill_at=<editor frame>");
    // Apply / Revert / presets (-chainpreview_preset_clamp_off / _sync_wholesale
    // are switches).
    needValue("-chainpreview_apply",      "-chainpreview_apply=<scene|prefab|instances>[:<arg>][@<editor frame>][,...]");
    needValue("-chainpreview_apply_seed", "-chainpreview_apply_seed=<src.ipfb>");
    needValue("-chainpreview_preset",     "-chainpreview_preset=<name|file.json>[:g<idx>][@<editor frame>][,...]");
    needValue("-chainpreview_revert_at",  "-chainpreview_revert_at=<editor frame>[,...]");
    // Scene save, subject requests, confirmation (-chainpreview_apply_restpose,
    // _scene_target_drop and _confirm_off are switches).
    needValue("-chainpreview_scene_save", "-chainpreview_scene_save=<file.iscene>@<editor frame>[,...]");
    needValue("-chainpreview_request",    "-chainpreview_request=<kind>[:<arg>]@<editor frame>[,...]");
    needValue("-chainpreview_confirm",    "-chainpreview_confirm=<keep|discard|apply>[,...]");
    needValue("-chainpreview_reapply",    "-chainpreview_reapply=<0|1>");
    // The parameter-tree probe (-chainpreview_tree_live, _play_keep and
    // _play_keep_off are switches).
    needValue("-chainpreview_panel_tree_at", "-chainpreview_panel_tree_at=<editor frame>");
    // Panel layout (-chainpreview_layout_fixed is a switch).
    needValue("-chainpreview_window",  "-chainpreview_window=<w>x<h>[+<x>+<y>][@<editor frame>][,...]");
    // Display mode (-chainpreview_display_off is a switch).
    needValue("-chainpreview_display", "-chainpreview_display=<both|overlay|mesh>[@<editor frame>][,...]");
    needValue("-chainpreview_sections", "-chainpreview_sections=<name>[+<name>...]@<editor frame>[,...]");
    needValue("-chainpreview_params_scroll", "-chainpreview_params_scroll=<0..1>@<editor frame>[,...]");
    // Splitter, Play-mode edit probe, stats box.
    needValue("-chainpreview_split",   "-chainpreview_split=<ratio 0..1|reset>@<editor frame>[,...]");
    needValue("-chainpreview_play_edit",  "-chainpreview_play_edit=<key>=<value>@<editor frame>");
    needValue("-chainpreview_stats",      "-chainpreview_stats=<0|1>[@<editor frame>][,...]");
    // A/B ghost, solo, poke (-chainpreview_ab_same / _solo_off / _poke_ttl_off
    // are switches).
    needValue("-chainpreview_abparam",    "-chainpreview_abparam=<key>=<value>[@<editor frame>][,...]");
    needValue("-chainpreview_ab_press",   "-chainpreview_ab_press=<swap|copy|promote|off>@<editor frame>[,...]");
    needValue("-chainpreview_solo",       "-chainpreview_solo=<group name|#idx|none>[@<editor frame>][,...]");
    needValue("-chainpreview_poke",       "-chainpreview_poke=<editor frame>,<particle|bone>,<strength>[,<blend s>]");
    needValue("-chainpreview_poke_probe", "-chainpreview_poke_probe=<after frames>,<settle frames>");
    // The in-picture rest-pose gizmo (-chainpreview_pose_local_order and
    // _pose_gesture_off are switches).
    needValue("-chainpreview_pose_gizmo", "-chainpreview_pose_gizmo=<bone|#idx>:<x|y|z|view>:<deg>:<frames>@<editor frame>[,...]");
    needValue("-chainpreview_main_pose",  "-chainpreview_main_pose=<entity name>[#k]:<bone|#idx|*>@<editor frame>");
    // World-motion inertia: -chainpreview_world_off is a switch (read by
    // ChainPhysicsSystem::Update itself, like _poke_ttl_off).
    const bool haveSubject = pcl.Has("-chainpreview_subject") &&
                             !pcl.Get("-chainpreview_subject").empty();
    bool anyOther = false;
    for (const std::string& a : pcl.Args())
        if (a.rfind("-chainpreview_", 0) == 0 &&
            a.rfind("-chainpreview_subject", 0) != 0) { anyOther = true; break; }
    if (anyOther && !haveSubject)
    {
        LOG_ERROR("-chainpreview_* flags were given without "
                  "-chainpreview_subject=<path.ipfb>. Refusing to run: "
                  "without a subject the Chain Physics Preview panel "
                  "never opens and the pack would measure nothing.");
        PostQuitMessage(Harness::PreconditionFail);
    }
    if (haveSubject)
    {
        const std::string subject = Resource::AssetRefs::PercentDecode(
            pcl.Get("-chainpreview_subject"));
        if (!pcl.Has("-chainpreview_size"))
        {
            LOG_ERROR("-chainpreview_subject needs -chainpreview_size=<w>x<h> "
                      "beside it. Refusing to run: under -hidden the panel "
                      "cannot measure itself, so an unpinned run never "
                      "exercises the size path.");
            PostQuitMessage(Harness::PreconditionFail);
        }
        else if (pcl.Get("-chainpreview_size") == "auto")
        {
            // No pin: the panel's layout sizes the render (a VISIBLE run; -hidden has
            // no region).
            chainPanel.HarnessSetSizeAuto();
            LOG_INFO("[CHAINPREVIEW] harness: -chainpreview_size=auto - the render "
                     "follows the panel layout%s",
                     pcl.Has("-hidden") ? " (WARNING: under -hidden the region is <= 0 "
                                          "and the size falls back to 640x360)" : "");
        }
        else
        {
            const std::string sz = pcl.Get("-chainpreview_size");
            // <digits>x<digits> and nothing else.
            const std::size_t xPos = sz.find('x');
            auto allDigits = [](const std::string& s)
            {
                return !s.empty() && s.size() <= 5 &&
                       std::all_of(s.begin(), s.end(), [](unsigned char c)
                                   { return c >= '0' && c <= '9'; });
            };
            const bool parsed = xPos != std::string::npos &&
                                allDigits(sz.substr(0, xPos)) &&
                                allDigits(sz.substr(xPos + 1));
            const unsigned w = parsed ? static_cast<unsigned>(
                std::strtoul(sz.substr(0, xPos).c_str(), nullptr, 10)) : 0u;
            const unsigned h = parsed ? static_cast<unsigned>(
                std::strtoul(sz.substr(xPos + 1).c_str(), nullptr, 10)) : 0u;
            if (!ENGINE_CHECK(parsed && chainPanel.HarnessSetSize(w, h),
                    "-chainpreview_size='%s' is not <w>x<h> with both "
                    "sides in 16..4096; using the documented default "
                    "640x360.", sz.c_str()))
                chainPanel.HarnessSetSize(640u, 360u);
        }
        if (pcl.Has("-chainpreview_frames"))
        {
            const int n = pcl.GetInt("-chainpreview_frames", 0);
            ENGINE_CHECK(n > 0 && chainPanel.HarnessSetFrames(
                             static_cast<std::uint32_t>(n)),
                         "-chainpreview_frames='%s' is not a positive "
                         "editor-frame count; the panel stays open for the "
                         "whole run (the documented default).",
                         pcl.Get("-chainpreview_frames").c_str());
        }
        if (pcl.Has("-chainpreview_close_at"))
        {
            const int f = pcl.GetInt("-chainpreview_close_at", 0);
            ENGINE_CHECK(f > 0 && chainPanel.HarnessSetCloseAt(
                             static_cast<std::uint64_t>(f)),
                         "-chainpreview_close_at='%s' is not an editor "
                         "frame above 0; the panel is never closed by the "
                         "harness (the documented default).",
                         pcl.Get("-chainpreview_close_at").c_str());
        }
        if (pcl.Has("-chainpreview_open_at"))
        {
            const int f = pcl.GetInt("-chainpreview_open_at", 0);
            ENGINE_CHECK(f > 0 && chainPanel.HarnessSetOpenAt(
                             static_cast<std::uint64_t>(f)),
                         "-chainpreview_open_at='%s' is not an editor "
                         "frame above 0; the panel opens at boot (the "
                         "documented default).",
                         pcl.Get("-chainpreview_open_at").c_str());
        }
        // -chainpreview_window=<w>x<h>[+<x>+<y>][@<f>][,...]: force the panel window's
        // size (and optionally its top-left) from editor frame f (default 0 = from the
        // start) and self-report the layout a settle later.
        if (pcl.Has("-chainpreview_window"))
        {
            const std::string spec = pcl.Get("-chainpreview_window");
            std::size_t pos = 0;
            while (pos <= spec.size())
            {
                std::size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const std::size_t at = item.find('@');
                const std::string geo = item.substr(0, at);
                // An optional +<x>+<y> top-left.
                const std::size_t plus = geo.find('+');
                const std::string wh = geo.substr(0, plus);
                const std::size_t x = wh.find('x');
                char* e1 = nullptr; char* e2 = nullptr; char* e3 = nullptr;
                const unsigned long w = x == std::string::npos ? 0ul
                    : std::strtoul(wh.substr(0, x).c_str(), &e1, 10);
                const unsigned long h = x == std::string::npos ? 0ul
                    : std::strtoul(wh.substr(x + 1).c_str(), &e2, 10);
                unsigned long long f = 0;
                bool ok = x != std::string::npos && e1 && *e1 == '\0' && e2 && *e2 == '\0';
                if (ok && at != std::string::npos)
                {
                    const std::string fs = item.substr(at + 1);
                    f = std::strtoull(fs.c_str(), &e3, 10);
                    ok = !fs.empty() && e3 && *e3 == '\0';
                }
                bool placed = false;
                float px = 0.f, py = 0.f;
                if (ok && plus != std::string::npos)
                {
                    const std::string xy = geo.substr(plus + 1);
                    const std::size_t p2 = xy.find('+');
                    char* e4 = nullptr; char* e5 = nullptr;
                    ok = p2 != std::string::npos && p2 > 0 && p2 + 1 < xy.size();
                    if (ok)
                    {
                        px = std::strtof(xy.substr(0, p2).c_str(), &e4);
                        py = std::strtof(xy.substr(p2 + 1).c_str(), &e5);
                        ok = e4 && *e4 == '\0' && e5 && *e5 == '\0';
                    }
                    placed = ok;
                }
                const bool added = ok &&
                    (placed ? chainPanel.HarnessAddWindowStepAt(
                                  static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                  px, py, static_cast<std::uint64_t>(f))
                            : chainPanel.HarnessAddWindowStep(
                                  static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                  static_cast<std::uint64_t>(f)));
                ENGINE_CHECK(added,
                             "-chainpreview_window item '%s' is not <w>x<h>[+<x>+<y>][@<editor "
                             "frame>] with w 200..8192, h 150..8192 and x, y 0..8192; the step "
                             "is ignored (the documented default: the panel keeps its own size).",
                             item.c_str());
            }
        }
        if (pcl.Has("-chainpreview_layout_fixed")) chainPanel.HarnessSetLayoutFixed();
        // -chainpreview_display=<both|overlay|mesh|chain|colliders>[@<editor frame>][,...]
        // (no @ = from the start) and its reverse arm -chainpreview_display_off.
        if (pcl.Has("-chainpreview_display"))
        {
            using DM = Editor::ChainPhysicsPreviewWindow::DisplayMode;
            const std::string spec = pcl.Get("-chainpreview_display");
            std::size_t pos = 0;
            while (pos < spec.size())
            {
                std::size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const std::size_t at = item.find('@');
                const std::string ms = item.substr(0, at);
                bool ok = true;
                DM mode = DM::Both;
                if      (ms == "both")    mode = DM::Both;
                else if (ms == "overlay") mode = DM::OverlayOnly;
                else if (ms == "mesh")    mode = DM::MeshOnly;
                else if (ms == "chain")   mode = DM::ChainOnly;
                else if (ms == "colliders") mode = DM::CollidersOnly;
                else ok = false;
                unsigned long long f = 0;
                if (ok && at != std::string::npos)
                {
                    const std::string fs = item.substr(at + 1);
                    char* e1 = nullptr;
                    f = std::strtoull(fs.c_str(), &e1, 10);
                    ok = !fs.empty() && e1 && *e1 == '\0';
                }
                ENGINE_CHECK(ok && chainPanel.HarnessAddDisplay(mode, static_cast<std::uint64_t>(f)),
                             "-chainpreview_display item '%s' is not <both|overlay|mesh|chain|colliders>[@<editor "
                             "frame>]; ignored (the documented default: the panel keeps its own "
                             "display mode).", item.c_str());
            }
        }
        if (pcl.Has("-chainpreview_display_off")) chainPanel.HarnessSetDisplayOff();
        // -chainpreview_params_scroll=<0..1>@<editor frame>[,...]
        if (pcl.Has("-chainpreview_params_scroll"))
        {
            const std::string spec = pcl.Get("-chainpreview_params_scroll");
            std::size_t pos = 0;
            while (pos < spec.size())
            {
                std::size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const std::size_t at = item.find('@');
                char* e1 = nullptr; char* e2 = nullptr;
                const std::string vs = item.substr(0, at);
                const std::string fs = at == std::string::npos ? std::string() : item.substr(at + 1);
                const float v = std::strtof(vs.c_str(), &e1);
                const unsigned long long f = std::strtoull(fs.c_str(), &e2, 10);
                const bool ok = at != std::string::npos && !vs.empty() && !fs.empty() &&
                                e1 && *e1 == '\0' && e2 && *e2 == '\0';
                ENGINE_CHECK(ok && chainPanel.HarnessAddParamsScroll(v, static_cast<std::uint64_t>(f)),
                             "-chainpreview_params_scroll item '%s' is not <0..1>@<editor frame>; "
                             "ignored (the column keeps its own scroll - the documented default).",
                             item.c_str());
            }
        }
        // -chainpreview_split=<ratio 0..1|reset>@<editor frame>[,...]
        if (pcl.Has("-chainpreview_split"))
        {
            const std::string spec = pcl.Get("-chainpreview_split");
            std::size_t pos = 0;
            while (pos < spec.size())
            {
                std::size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const std::size_t at = item.find('@');
                char* e1 = nullptr; char* e2 = nullptr;
                const std::string vs = item.substr(0, at);
                const std::string fs = at == std::string::npos ? std::string() : item.substr(at + 1);
                const bool reset = vs == "reset";
                const float v = reset ? -1.f : std::strtof(vs.c_str(), &e1);
                const unsigned long long f = std::strtoull(fs.c_str(), &e2, 10);
                const bool ok = at != std::string::npos && !vs.empty() && !fs.empty() &&
                                (reset || (e1 && *e1 == '\0')) && e2 && *e2 == '\0';
                ENGINE_CHECK(ok && chainPanel.HarnessAddSplit(v, static_cast<std::uint64_t>(f)),
                             "-chainpreview_split item '%s' is not <ratio 0..1|reset>@<editor "
                             "frame above 0>; ignored (the splitter keeps its ratio - the "
                             "documented default).", item.c_str());
            }
        }
        // -chainpreview_sections=<name>[+<name>...]@<editor frame>[,...]
        if (pcl.Has("-chainpreview_sections"))
        {
            using CPW = Editor::ChainPhysicsPreviewWindow;
            const std::string spec = pcl.Get("-chainpreview_sections");
            std::size_t pos = 0;
            while (pos < spec.size())
            {
                std::size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const std::size_t at = item.find('@');
                const std::string names = item.substr(0, at);
                std::uint32_t mask = 0;
                bool ok = at != std::string::npos && !names.empty();
                std::size_t np = 0;
                while (ok && np < names.size())
                {
                    std::size_t plus = names.find('+', np);
                    if (plus == std::string::npos) plus = names.size();
                    const std::string n = names.substr(np, plus - np);
                    np = plus + 1;
                    if      (n == "preview")    mask |= CPW::kSecPreview;
                    else if (n == "simulation") mask |= CPW::kSecSimulation;
                    else if (n == "scene")      mask |= CPW::kSecScene;
                    else if (n == "params")     mask |= CPW::kSecParams;
                    else if (n == "ab")         mask |= CPW::kSecAb;
                    else if (n == "show")       mask |= CPW::kSecShow;
                    else if (n == "tree")       mask |= CPW::kSecTree;
                    else if (n == "all")        mask |= CPW::kSecAll;
                    else ok = false;
                }
                char* end = nullptr;
                const std::string fs = ok ? item.substr(at + 1) : std::string();
                const unsigned long long f = ok ? std::strtoull(fs.c_str(), &end, 10) : 0ull;
                ok = ok && !fs.empty() && end && *end == '\0';
                ENGINE_CHECK(ok && chainPanel.HarnessAddSections(mask, static_cast<std::uint64_t>(f)),
                             "-chainpreview_sections item '%s' is not <name>[+<name>...]@<editor "
                             "frame> (names: preview simulation scene params ab show tree all); "
                             "ignored - the sections keep their own state (the documented "
                             "default).", item.c_str());
            }
        }
        if (pcl.Has("-chainpreview_strip_off"))   chainPanel.HarnessSetStripOff();
        if (pcl.Has("-chainpreview_release_off")) chainPanel.HarnessSetReleaseOff();
        if (pcl.Has("-chainpreview_play_teardown_off")) chainPanel.HarnessSetPlayTeardownOff();
        // The animation transport: clip, time, pin_off.
        if (pcl.Has("-chainpreview_clip"))
            chainPanel.HarnessSetClip(Resource::AssetRefs::PercentDecode(
                pcl.Get("-chainpreview_clip")));
        if (pcl.Has("-chainpreview_time"))
        {
            const std::string ts = pcl.Get("-chainpreview_time");
            char* end = nullptr;
            const double sec = std::strtod(ts.c_str(), &end);
            const bool parsed = end && end != ts.c_str() && *end == '\0';
            ENGINE_CHECK(parsed && chainPanel.HarnessSetTime(sec),
                         "-chainpreview_time='%s' is not a clip time in "
                         "seconds (0..36000); the playhead starts at 0 (the "
                         "documented default).", ts.c_str());
        }
        if (pcl.Has("-chainpreview_pin_off")) chainPanel.HarnessSetPinOff();
        // The chain solver's preview lane.
        if (pcl.Has("-chainpreview_dt"))
        {
            const std::string ds = pcl.Get("-chainpreview_dt");
            // "follow" = the dt dropdown's "follow editor"
            // (chain_preview_fixeddt's reverse arm).
            if (ds == "follow")
                chainPanel.HarnessSetDtFollow();
            else
            {
                char* end = nullptr;
                const double dtv = std::strtod(ds.c_str(), &end);
                const bool parsed = end && end != ds.c_str() && *end == '\0';
                // 0 is legal (the lane never runs - chain_preview_sim's
                // reverse arm); above 1/30 the solver would clamp silently.
                ENGINE_CHECK(parsed && chainPanel.HarnessSetDt(static_cast<float>(dtv)),
                             "-chainpreview_dt='%s' is not a step in seconds "
                             "within 0..1/30 (or 'follow'); the stage clock "
                             "uses the documented default 1/60.", ds.c_str());
            }
        }
        // ---- The stage clock ----
        // <a>[,<b>...] -> numbers; false on any malformed entry.
        auto splitNumbers = [](const std::string& list, std::vector<double>& out)
        {
            out.clear();
            std::size_t start = 0;
            while (start <= list.size())
            {
                const std::size_t comma = list.find(',', start);
                const std::string item = list.substr(start,
                    comma == std::string::npos ? std::string::npos : comma - start);
                start = (comma == std::string::npos) ? list.size() + 1 : comma + 1;
                char* end = nullptr;
                const double v = std::strtod(item.c_str(), &end);
                if (item.empty() || !end || *end != '\0' || !std::isfinite(v)) return false;
                out.push_back(v);
            }
            return !out.empty();
        };
        auto toSteps = [](const std::vector<double>& v, std::vector<std::uint32_t>& out)
        {
            out.clear();
            for (double d : v)
            {
                if (d < 0.0 || d > 1000.0 || d != std::floor(d)) return false;
                out.push_back(static_cast<std::uint32_t>(d));
            }
            return true;
        };
        auto toSeconds = [](const std::vector<double>& v, std::vector<float>& out)
        {
            out.clear();
            for (double d : v) out.push_back(static_cast<float>(d));
            return true;
        };
        if (pcl.Has("-chainpreview_timescale"))
        {
            const std::string v = pcl.Get("-chainpreview_timescale");
            char* end = nullptr;
            const double ts = std::strtod(v.c_str(), &end);
            const bool parsed = end && end != v.c_str() && *end == '\0';
            ENGINE_CHECK(parsed && chainPanel.HarnessSetTimeScale(static_cast<float>(ts)),
                         "-chainpreview_timescale='%s' is not a time scale in "
                         "0.1..2.0; the stage clock runs at the documented "
                         "default 1.0.", v.c_str());
        }
        const bool haveSteps  = pcl.Has("-chainpreview_steps");
        const bool haveRealDt = pcl.Has("-chainpreview_realdt");
        ENGINE_CHECK(!(haveSteps && haveRealDt),
                     "-chainpreview_steps and -chainpreview_realdt both given; "
                     "-chainpreview_steps (the exact pin) wins.");
        if (haveSteps)
        {
            const std::string v = pcl.Get("-chainpreview_steps");
            std::vector<double> nums;
            std::vector<std::uint32_t> steps;
            ENGINE_CHECK(splitNumbers(v, nums) && toSteps(nums, steps) &&
                         chainPanel.HarnessSetSteps(steps),
                         "-chainpreview_steps='%s' is not a comma list of step "
                         "counts 0..4; the harness keeps its documented "
                         "default of one step per editor frame.", v.c_str());
        }
        else if (haveRealDt)
        {
            const std::string v = pcl.Get("-chainpreview_realdt");
            std::vector<double> nums;
            std::vector<float> secs;
            ENGINE_CHECK(splitNumbers(v, nums) && toSeconds(nums, secs) &&
                         chainPanel.HarnessSetRealDt(secs),
                         "-chainpreview_realdt='%s' is not a comma list of "
                         "frame times in (0, 1] seconds; the harness keeps its "
                         "documented default of one step per editor frame.",
                         v.c_str());
        }
        if (pcl.Has("-chainpreview_pause_at"))
        {
            const int f = pcl.GetInt("-chainpreview_pause_at", 0);
            ENGINE_CHECK(f > 0 && chainPanel.HarnessSetPauseAt(
                             static_cast<std::uint64_t>(f)),
                         "-chainpreview_pause_at='%s' is not an editor frame "
                         "above 0; the stage clock is never paused by the "
                         "harness (the documented default).",
                         pcl.Get("-chainpreview_pause_at").c_str());
        }
        if (pcl.Has("-chainpreview_step_at"))
        {
            const int f = pcl.GetInt("-chainpreview_step_at", 0);
            ENGINE_CHECK(f > 0 && chainPanel.HarnessSetStepAt(
                             static_cast<std::uint64_t>(f)),
                         "-chainpreview_step_at='%s' is not an editor frame "
                         "above 0; no harness Step (the documented default).",
                         pcl.Get("-chainpreview_step_at").c_str());
        }
        if (pcl.Has("-chainpreview_ab"))
        {
            // <seconds>[:steps=<list>|:realdt=<list>]... - one extra
            // window per ':' segment (at most three).
            const std::string v = pcl.Get("-chainpreview_ab");
            std::vector<std::string> segs;
            for (std::size_t start = 0; start <= v.size();)
            {
                const std::size_t colon = v.find(':', start);
                segs.push_back(v.substr(start, colon == std::string::npos
                                            ? std::string::npos : colon - start));
                start = (colon == std::string::npos) ? v.size() + 1 : colon + 1;
            }
            char* end = nullptr;
            const double sec = std::strtod(segs[0].c_str(), &end);
            bool ok = !segs[0].empty() && end && *end == '\0';
            std::vector<Editor::ChainPhysicsPreviewWindow::AbPacing> extra;
            for (std::size_t i = 1; ok && i < segs.size(); ++i)
            {
                Editor::ChainPhysicsPreviewWindow::AbPacing p;
                std::vector<double> nums;
                const std::string& spec = segs[i];
                if (spec.rfind("steps=", 0) == 0)
                    ok = splitNumbers(spec.substr(6), nums) && toSteps(nums, p.steps);
                else if (spec.rfind("realdt=", 0) == 0)
                    ok = splitNumbers(spec.substr(7), nums) && toSeconds(nums, p.realDt);
                else
                    ok = false;
                extra.push_back(std::move(p));
            }
            ENGINE_CHECK(ok && chainPanel.HarnessSetAb(sec, extra),
                         "-chainpreview_ab='%s' is not <seconds in (0,60]>"
                         "[:steps=<0..4 list>|:realdt=<(0,1] list>]... (at most "
                         "three extra windows); no A/B windows run (the "
                         "documented default).", v.c_str());
        }
        if (pcl.Has("-chainpreview_hold_off")) chainPanel.HarnessSetHoldOff();
        if (pcl.Has("-chainpreview_sim"))
        {
            const std::string sv = pcl.Get("-chainpreview_sim");
            const bool ok = (sv == "0" || sv == "1");
            if (ENGINE_CHECK(ok, "-chainpreview_sim='%s' is not 0 or 1; the "
                                 "simulation stays ON (the documented default).",
                             sv.c_str()))
                chainPanel.HarnessSetSim(sv == "1");
        }
        if (pcl.Has("-chainpreview_reset_at"))
        {
            const int f = pcl.GetInt("-chainpreview_reset_at", 0);
            ENGINE_CHECK(f > 0 && chainPanel.HarnessSetResetAt(
                             static_cast<std::uint64_t>(f)),
                         "-chainpreview_reset_at='%s' is not an editor frame "
                         "above 0; the simulation is never reset by the "
                         "harness (the documented default).",
                         pcl.Get("-chainpreview_reset_at").c_str());
        }
        if (pcl.Has("-chainpreview_dump"))
            chainPanel.HarnessSetDump(Resource::AssetRefs::PercentDecode(
                pcl.Get("-chainpreview_dump")));
        // -chainpreview_param is REPEATABLE. The parsed command line keeps only the
        // LAST occurrence of a key, so every occurrence is read (GetAll), in order,
        // and each may also be a comma list; a later value for the same field is
        // applied later (last wins). A malformed entry is ENGINE_CHECK + ignored (the
        // field keeps its authored value - the documented default).
        for (const std::string& list : pcl.GetAll("-chainpreview_param"))
        {
            std::size_t start = 0;
            while (start <= list.size())
            {
                const std::size_t comma = list.find(',', start);
                const std::string item = list.substr(start,
                    comma == std::string::npos ? std::string::npos : comma - start);
                start = (comma == std::string::npos) ? list.size() + 1 : comma + 1;
                if (item.empty()) continue;
                const std::size_t eq = item.find('=');
                // An optional "@<editor frame>" suffix applies the edit at
                // that frame (none = the first frame with a stage). No value
                // form uses '@'.
                std::string value = eq == std::string::npos ? std::string()
                                                            : item.substr(eq + 1);
                std::uint64_t at = 0;
                bool atOk = true;
                const std::size_t atPos = value.rfind('@');
                if (atPos != std::string::npos)
                {
                    const std::string fs = value.substr(atPos + 1);
                    char* fe = nullptr;
                    const unsigned long long f = std::strtoull(fs.c_str(), &fe, 10);
                    atOk = !fs.empty() && fe && *fe == 0 && f > 0;
                    at = atOk ? f : 0;
                    value = value.substr(0, atPos);
                }
                const bool ok = eq != std::string::npos && eq > 0 && atOk &&
                    chainPanel.HarnessAddParamAt(item.substr(0, eq), value, at);
                ENGINE_CHECK(ok, "-chainpreview_param entry '%s' is not "
                             "<ChainPhysicsComponent REFLECT field>=<value> or "
                             "g<idx>_<ChainGroupDef member>=<value>, optionally "
                             "@<editor frame above 0>; ignored (the field keeps its "
                             "authored value).", item.c_str());
            }
        }
        if (pcl.Has("-chainpreview_carry_off")) chainPanel.HarnessSetCarryOff();
        // ---- The overlay + panel undo ----
        // Comma lists; a malformed list is ENGINE_CHECK + the documented default (no
        // schedule / no shot / no harness undo).
        auto splitCsv = [](const std::string& s)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start <= s.size())
            {
                const std::size_t c = s.find(',', start);
                out.push_back(s.substr(start, c == std::string::npos
                                                  ? std::string::npos : c - start));
                start = (c == std::string::npos) ? s.size() + 1 : c + 1;
            }
            return out;
        };
        auto parseFrame = [](const std::string& s, std::uint64_t& f)
        {
            char* e = nullptr;
            const unsigned long long v = std::strtoull(s.c_str(), &e, 10);
            if (s.empty() || !e || *e != 0 || v == 0) return false;
            f = v;
            return true;
        };
        if (pcl.Has("-chainpreview_overlay"))
        {
            // <hexmask>[@<editor frame>] - no '@' = from the start.
            const std::string v = pcl.Get("-chainpreview_overlay");
            std::vector<std::pair<std::uint32_t, std::uint64_t>> sched;
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                const std::size_t at = it.find('@');
                const std::string hx = it.substr(0, at);
                char* e = nullptr;
                const unsigned long m = std::strtoul(hx.c_str(), &e, 16);
                std::uint64_t f = 0;
                ok = ok && !hx.empty() && e && *e == 0 &&
                     (at == std::string::npos || parseFrame(it.substr(at + 1), f));
                sched.emplace_back(static_cast<std::uint32_t>(m), f);
            }
            ENGINE_CHECK(ok && chainPanel.HarnessSetOverlay(sched),
                         "-chainpreview_overlay='%s' is not <hexmask up to 7fff>"
                         "[@<editor frame>][,...]; the overlay keeps the panel's "
                         "default masks (the documented default).", v.c_str());
        }
        if (pcl.Has("-chainpreview_overlay_shot"))
        {
            const std::string v = pcl.Get("-chainpreview_overlay_shot");
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                const std::size_t at = it.rfind('@');
                std::uint64_t f = 0;
                const bool one = at != std::string::npos && at > 0 &&
                                 parseFrame(it.substr(at + 1), f) &&
                                 chainPanel.HarnessAddOverlayShot(
                                     Resource::AssetRefs::PercentDecode(it.substr(0, at)), f);
                ok = ok && one;
            }
            ENGINE_CHECK(ok, "-chainpreview_overlay_shot='%s' is not "
                         "<file.png>@<editor frame above 0>[,...]; the malformed "
                         "entries write nothing (the documented default).", v.c_str());
        }
        if (pcl.Has("-chainpreview_overlay_badproj")) chainPanel.HarnessSetOverlayBadProj();
        if (pcl.Has("-chainpreview_reinit_off")) chainPanel.HarnessSetReinitOff();
        auto frameList = [&](const char* flag, int which)
        {
            if (!pcl.Has(flag)) return;
            const std::string v = pcl.Get(flag);
            std::vector<std::uint64_t> frames;
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                std::uint64_t f = 0;
                ok = ok && parseFrame(it, f);
                frames.push_back(f);
            }
            ok = ok && (which == 0 ? chainPanel.HarnessSetUndoAt(frames)
                      : which == 1 ? chainPanel.HarnessSetRedoAt(frames)
                                   : chainPanel.HarnessSetFullResetAt(frames));
            ENGINE_CHECK(ok, "%s='%s' is not a comma list of editor frames above "
                         "0; ignored (the documented default).", flag, v.c_str());
        };
        frameList("-chainpreview_undo_at", 0);
        frameList("-chainpreview_redo_at", 1);
        frameList("-chainpreview_fullreset_at", 2);
        if (pcl.Has("-chainpreview_lod_keep"))  chainPanel.HarnessSetLodKeep();
        if (pcl.Has("-chainpreview_stage_edit_at"))
        {
            // Read by EditorLayer::TickChainStageEditProbe; checked here.
            const int f = pcl.GetInt("-chainpreview_stage_edit_at", 0);
            ENGINE_CHECK(f > 0, "-chainpreview_stage_edit_at='%s' is not an "
                         "editor frame above 0; the stage-edit probe never "
                         "runs (the documented default).",
                         pcl.Get("-chainpreview_stage_edit_at").c_str());
        }
        // ---- Scene, forces, driver ----
        // Scene levers take an optional "@<editor frame>" and the gravity / wind /
        // drive flags are REPEATABLE (every raw argv occurrence, in order - the parsed
        // command line keeps only the last), so ONE process can run several segments.
        // A malformed value is ENGINE_CHECK + ignored (the documented default: the
        // lever keeps its current value).
        auto parseF = [](const std::string& t, float& o)
        {
            if (t.empty()) return false;
            char* e = nullptr;
            const double d = std::strtod(t.c_str(), &e);
            if (!e || *e != 0 || !std::isfinite(d)) return false;
            o = static_cast<float>(d);
            return true;
        };
        // "<body>[@<frame>]" -> body, frame (0 = the first frame with a stage)
        auto splitAt = [&](const std::string& v, std::string& body, std::uint64_t& at)
        {
            const std::size_t p = v.rfind('@');
            at = 0;
            body = v.substr(0, p);
            return p == std::string::npos || parseFrame(v.substr(p + 1), at);
        };
        auto parseVec3 = [&](const std::string& t, float o[3])
        {
            const std::vector<std::string> c = splitCsv(t);
            return c.size() == 3 && parseF(c[0], o[0]) && parseF(c[1], o[1]) &&
                   parseF(c[2], o[2]);
        };
        auto eachArg = [&](const char* flag, const std::function<bool(const std::string&)>& fn)
        {
            for (const std::string& v : pcl.GetAll(flag))
                ENGINE_CHECK(fn(v), "%s='%s' is malformed; ignored (the lever keeps its "
                             "current value - the documented default).", flag, v.c_str());
        };
        if (pcl.Has("-chainpreview_scale"))
        {
            const std::string v = pcl.Get("-chainpreview_scale");
            float s = 0.f;
            const bool ok = v == "auto" ? chainPanel.HarnessSetScale(0.f)
                                        : (parseF(v, s) && s > 0.f && chainPanel.HarnessSetScale(s));
            ENGINE_CHECK(ok, "-chainpreview_scale='%s' is not a scale in (0,100] or "
                         "'auto'; the default rule applies (the scene instance's "
                         "scale, else the prefab's authored one).", v.c_str());
        }
        if (pcl.Has("-chainpreview_floor"))
        {
            const std::string v = pcl.Get("-chainpreview_floor");
            const std::size_t c = v.find(':');
            const std::string m = v.substr(0, c);
            float gap = 0.f;
            const int mode = m == "0" ? 0 : m == "1" ? 1 : m == "collide" ? 2 : -1;
            const bool ok = mode >= 0 &&
                (c == std::string::npos || parseF(v.substr(c + 1), gap)) &&
                chainPanel.HarnessSetFloor(mode, gap);
            ENGINE_CHECK(ok, "-chainpreview_floor='%s' is not <0|1|collide>[:<gap>=0..100]; "
                         "the harness default applies (no floor).", v.c_str());
        }
        eachArg("-chainpreview_gravity", [&](const std::string& v)
        {
            std::string body;
            std::uint64_t at = 0;
            if (!splitAt(v, body, at)) return false;
            const std::size_t c = body.find(':');
            float down[3] = {}, scale = 1.f;
            return parseVec3(body.substr(0, c), down) &&
                   (c == std::string::npos || parseF(body.substr(c + 1), scale)) &&
                   chainPanel.HarnessAddGravity(down, scale, at);
        });
        if (pcl.Has("-chainpreview_nogravity"))
        {
            // Bare = from the first frame; "=@<frame>" = from that frame.
            const std::string v = pcl.Get("-chainpreview_nogravity");
            std::uint64_t at = 0;
            const bool ok = v.empty() || (v[0] == '@' && parseFrame(v.substr(1), at));
            if (ENGINE_CHECK(ok, "-chainpreview_nogravity='%s' is not bare or @<editor "
                             "frame>; No Gravity starts at the first frame.", v.c_str()))
                chainPanel.HarnessSetNoGravityAt(at);
            else
                chainPanel.HarnessSetNoGravityAt(0);
        }
        eachArg("-chainpreview_wind", [&](const std::string& v)
        {
            // <x,y,z>:<strength>[:<freq>[:<turbulence>]][@<frame>]
            std::string body;
            std::uint64_t at = 0;
            if (!splitAt(v, body, at)) return false;
            std::vector<std::string> p;
            std::size_t s0 = 0;
            while (s0 <= body.size())
            {
                const std::size_t c = body.find(':', s0);
                p.push_back(body.substr(s0, c == std::string::npos ? std::string::npos : c - s0));
                s0 = c == std::string::npos ? body.size() + 1 : c + 1;
            }
            float dir[3] = {}, str = 0.f, fr = -1.f, tb = -1.f;
            return p.size() >= 2 && p.size() <= 4 && parseVec3(p[0], dir) &&
                   parseF(p[1], str) && (p.size() < 3 || parseF(p[2], fr)) &&
                   (p.size() < 4 || parseF(p[3], tb)) &&
                   chainPanel.HarnessAddWind(dir, str, fr, tb, at);
        });
        eachArg("-chainpreview_drive", [&](const std::string& v)
        {
            // <move>,<rotDegPerSec>  |  rotate:<deg/s>  |  move:<u/s>   [@<frame>]
            std::string body;
            std::uint64_t at = 0;
            if (!splitAt(v, body, at)) return false;
            float mv = 0.f, rt = 0.f;
            if (body.rfind("rotate:", 0) == 0)
                return parseF(body.substr(7), rt) && chainPanel.HarnessAddDrive(0.f, rt, at);
            if (body.rfind("move:", 0) == 0)
                return parseF(body.substr(5), mv) && chainPanel.HarnessAddDrive(mv, 0.f, at);
            const std::vector<std::string> c = splitCsv(body);
            return c.size() == 2 && parseF(c[0], mv) && parseF(c[1], rt) &&
                   chainPanel.HarnessAddDrive(mv, rt, at);
        });
        if (pcl.Has("-chainpreview_drive_off")) chainPanel.HarnessSetDriveOff();
        if (pcl.Has("-chainpreview_teleport_at"))
        {
            // <frame>[:<dist>[:<angleDeg>]][,...]; dist <= 0 = auto
            const std::string v = pcl.Get("-chainpreview_teleport_at");
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                std::vector<std::string> p;
                std::size_t s0 = 0;
                while (s0 <= it.size())
                {
                    const std::size_t c = it.find(':', s0);
                    p.push_back(it.substr(s0, c == std::string::npos ? std::string::npos : c - s0));
                    s0 = c == std::string::npos ? it.size() + 1 : c + 1;
                }
                std::uint64_t f = 0;
                float dist = 0.f, ang = 0.f;
                ok = ok && !p.empty() && p.size() <= 4 && parseFrame(p[0], f) &&
                     (p.size() < 2 || parseF(p[1], dist)) &&
                     (p.size() < 3 || parseF(p[2], ang)) &&
                     (p.size() < 4 || p[3] == "reset") &&
                     chainPanel.HarnessAddTeleportAt(f, dist, ang, p.size() == 4);
            }
            ENGINE_CHECK(ok, "-chainpreview_teleport_at='%s' is not <editor frame>"
                         "[:<dist>[:<angleDeg>[:reset]]][,...]; the malformed entries "
                         "never teleport (the documented default).", v.c_str());
        }
        if (pcl.Has("-chainpreview_scale_at"))
        {
            // <frame>:<scale>[,...] - the Scale control, pressed
            const std::string v = pcl.Get("-chainpreview_scale_at");
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                const std::size_t c = it.find(':');
                std::uint64_t f = 0;
                float sc = 0.f;
                ok = ok && c != std::string::npos && parseFrame(it.substr(0, c), f) &&
                     parseF(it.substr(c + 1), sc) && chainPanel.HarnessAddScaleAt(f, sc);
            }
            ENGINE_CHECK(ok, "-chainpreview_scale_at='%s' is not <editor frame>:<scale "
                         "in (0,100]>[,...]; the malformed entries change nothing.",
                         v.c_str());
        }
        if (pcl.Has("-chainpreview_teleport_reset"))
        {
            const std::string v = pcl.Get("-chainpreview_teleport_reset");
            const bool ok = v == "0" || v == "1";
            if (ENGINE_CHECK(ok, "-chainpreview_teleport_reset='%s' is not 0 or 1; the "
                             "chains integrate across a teleport (the default).", v.c_str()))
                chainPanel.HarnessSetTeleportReset(v == "1");
        }
        if (pcl.Has("-chainpreview_stress"))
            ENGINE_CHECK(chainPanel.HarnessSetStress(pcl.Get("-chainpreview_stress")),
                         "-chainpreview_stress='%s' is not yawsweep|teleport|idle|walkrun; "
                         "no stress motion is picked (the default).",
                         pcl.Get("-chainpreview_stress").c_str());
        if (pcl.Has("-chainpreview_shot"))
        {
            const std::string v = pcl.Get("-chainpreview_shot");
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                const std::size_t at = it.rfind('@');
                std::uint64_t f = 0;
                ok = ok && at != std::string::npos && at > 0 &&
                     parseFrame(it.substr(at + 1), f) &&
                     chainPanel.HarnessAddShot(
                         Resource::AssetRefs::PercentDecode(it.substr(0, at)), f);
            }
            ENGINE_CHECK(ok, "-chainpreview_shot='%s' is not <file.png>@<editor frame "
                         "above 0>[,...]; the malformed entries write nothing.", v.c_str());
        }
        if (pcl.Has("-chainpreview_follow"))
            ENGINE_CHECK(chainPanel.HarnessSetFollow(pcl.Get("-chainpreview_follow")),
                         "-chainpreview_follow needs an entity name");
        if (pcl.Has("-chainpreview_follow_kill_at"))
        {
            std::uint64_t f = 0;
            const std::string v = pcl.Get("-chainpreview_follow_kill_at");
            ENGINE_CHECK(parseFrame(v, f) && chainPanel.HarnessSetFollowKillAt(f),
                         "-chainpreview_follow_kill_at='%s' is not an editor frame above "
                         "0; the follow source is never destroyed.", v.c_str());
        }
        // ---- Apply / Revert / presets ----
        // Comma lists with an optional "@<editor frame>" per entry (none = the first
        // frame with a stage). Malformed entries are ENGINE_CHECK + ignored (the
        // documented default: nothing applied). splitAt / splitCsv / parseFrame are
        // the helpers above. A seed needs a validated prefab entry to land on (below).
        bool validatedPrefabApply = false;
        if (pcl.Has("-chainpreview_apply"))
        {
            const std::string v = pcl.Get("-chainpreview_apply");
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                bool ok = !it.empty() && splitAt(it, body, at);
                // <dest>[:<arg>] - the FIRST ':' ends the destination (an
                // absolute Windows path keeps its drive colon in <arg>).
                const std::size_t c = body.find(':');
                const std::string dest = body.substr(0, c);
                const std::string arg  = c == std::string::npos ? std::string()
                                                                : body.substr(c + 1);
                using AD = Editor::ChainPhysicsPreviewWindow::ApplyDest;
                AD d = AD::Prefab;
                if      (dest == "scene")     d = AD::Scene;
                else if (dest == "prefab")    d = AD::Prefab;
                else if (dest == "instances") d = AD::Instances;
                else ok = false;
                // A HARNESS prefab apply (and the -chainpreview_apply_seed copied over its
                // target) may only write inside this run's -artifacts directory, and never
                // the subject itself: a bare "prefab" would rewrite the subject's TRACKED
                // .ipfb.
                if (ok && d == AD::Prefab)
                {
                    namespace fs = std::filesystem;
                    const std::string a = Resource::AssetRefs::PercentDecode(arg);
                    const bool absolute = a.size() > 1 &&
                                          (a[1] == ':' || a[0] == '/' || a[0] == '\\');
                    std::error_code ec1, ec2, ec3;
                    const fs::path art = fs::weakly_canonical(
                        Sys::U8Path(Sys::DiagnosticsDir()), ec1);
                    const fs::path tgt = fs::weakly_canonical(
                        Sys::U8Path(absolute ? a : Sys::DiagnosticsDir() + "/" + a), ec2);
                    const fs::path sub = fs::weakly_canonical(Sys::U8Path(subject), ec3);
                    const auto rel = tgt.lexically_relative(art);
                    const bool inside = Sys::HasArtifactDir() && !a.empty() && !ec1 && !ec2 &&
                                        !rel.empty() && *rel.begin() != fs::path("..");
                    const bool isSubject = !ec3 && tgt == sub;
                    if (!inside || isSubject)
                    {
                        LOG_ERROR("-chainpreview_apply entry '%s': a harness prefab apply "
                                  "must name a target INSIDE this run's -artifacts "
                                  "directory ('%s') and never the subject ('%s') - %s. "
                                  "Refusing to run: the write (and "
                                  "-chainpreview_apply_seed's copy over it) would land "
                                  "on a tracked asset.", it.c_str(),
                                  Sys::DiagnosticsDir().c_str(), subject.c_str(),
                                  a.empty() ? "no target given (= the subject's own .ipfb)"
                                  : isSubject ? "it IS the subject" : "it is outside");
                        PostQuitMessage(Harness::PreconditionFail);
                        ok = false;
                    }
                    validatedPrefabApply = validatedPrefabApply || ok;
                }
                ok = ok && chainPanel.HarnessAddApply(
                    d, Resource::AssetRefs::PercentDecode(arg), at);
                ENGINE_CHECK(ok, "-chainpreview_apply entry '%s' is not <scene|prefab|"
                             "instances>[:<arg>][@<editor frame above 0>]; nothing is "
                             "applied for it.", it.c_str());
            }
        }
        if (pcl.Has("-chainpreview_apply_seed"))
        {
            const std::string v = Resource::AssetRefs::PercentDecode(
                pcl.Get("-chainpreview_apply_seed"));
            // A seed with no validated -chainpreview_apply=prefab:<inside -artifacts>
            // entry has nothing it may be copied over: the only other prefab write (a
            // confirmation answered "apply") targets the subject.
            if (!validatedPrefabApply)
            {
                LOG_ERROR("-chainpreview_apply_seed='%s' without a validated "
                          "-chainpreview_apply=prefab:<target inside -artifacts> entry. "
                          "Refusing to run: a seed is only ever copied over such a "
                          "target, never over the subject.", v.c_str());
                PostQuitMessage(Harness::PreconditionFail);
            }
            ENGINE_CHECK(chainPanel.HarnessSetApplySeed(v),
                         "-chainpreview_apply_seed='%s' does not exist; the apply "
                         "target is not seeded.", v.c_str());
        }
        if (pcl.Has("-chainpreview_preset"))
        {
            const std::string v = pcl.Get("-chainpreview_preset");
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                bool ok = !it.empty() && splitAt(it, body, at);
                // An optional ":g<idx>" group scope at the END.
                int group = -1;
                const std::size_t g = body.rfind(":g");
                if (ok && g != std::string::npos && g + 2 < body.size() &&
                    body.find_first_not_of("0123456789", g + 2) == std::string::npos)
                {
                    group = std::atoi(body.c_str() + g + 2);
                    body  = body.substr(0, g);
                }
                ok = ok && chainPanel.HarnessAddPreset(
                    Resource::AssetRefs::PercentDecode(body), group, at);
                ENGINE_CHECK(ok, "-chainpreview_preset entry '%s' is not a built-in "
                             "(hair_long, hair_short, skirt_pleated, skirt_tight, cape, "
                             "tail, accessory), a user preset or an existing .json, "
                             "optionally :g<idx> and @<editor frame above 0>; not "
                             "loaded.", it.c_str());
            }
        }
        if (pcl.Has("-chainpreview_preset_clamp_off")) chainPanel.HarnessSetPresetClampOff();
        if (pcl.Has("-chainpreview_sync_wholesale"))   chainPanel.HarnessSetSyncWholesale();
        if (pcl.Has("-chainpreview_revert_at"))
        {
            std::vector<std::uint64_t> fr;
            bool ok = true;
            for (const std::string& it : splitCsv(pcl.Get("-chainpreview_revert_at")))
            {
                std::uint64_t f = 0;
                ok = ok && parseFrame(it, f);
                fr.push_back(f);
            }
            ENGINE_CHECK(ok && chainPanel.HarnessSetRevertAt(fr),
                         "-chainpreview_revert_at='%s' is not a comma list of editor "
                         "frames above 0; Revert is never pressed.",
                         pcl.Get("-chainpreview_revert_at").c_str());
        }
        // ---- Rest-pose apply / scene-target drop / confirmation switches ----
        if (pcl.Has("-chainpreview_apply_restpose"))     chainPanel.HarnessSetApplyRestPose();
        if (pcl.Has("-chainpreview_scene_target_drop"))  chainPanel.HarnessSetSceneTargetDrop();
        if (pcl.Has("-chainpreview_confirm_off"))        chainPanel.HarnessSetConfirmOff();
        // ---- Reverse arms: stash path key, bare re-init ----
        if (pcl.Has("-chainpreview_stash_pathkey"))      chainPanel.HarnessSetStashPathKey();
        if (pcl.Has("-chainpreview_reinit_bare"))        chainPanel.HarnessSetReinitBare();
        if (pcl.Has("-chainpreview_scene_save"))
        {
            // <file.iscene>@<editor frame> - like the prefab apply target,
            // only ever inside -artifacts (a relative name lands there).
            const std::string v = pcl.Get("-chainpreview_scene_save");
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                const std::size_t at = it.rfind('@');
                std::uint64_t f = 0;
                const std::string file = at == std::string::npos ? std::string()
                    : Resource::AssetRefs::PercentDecode(it.substr(0, at));
                const bool absolute = file.size() > 1 &&
                                      (file[1] == ':' || file[0] == '/' || file[0] == '\\');
                if (!file.empty() && (absolute || file.find("..") != std::string::npos))
                {
                    LOG_ERROR("-chainpreview_scene_save entry '%s': the file must be a "
                              "RELATIVE name (it lands in -artifacts). Refusing to run.",
                              it.c_str());
                    PostQuitMessage(Harness::PreconditionFail);
                    ok = false;
                    continue;
                }
                ok = ok && at != std::string::npos && !file.empty() &&
                     parseFrame(it.substr(at + 1), f) && chainPanel.HarnessAddSceneSave(file, f);
            }
            ENGINE_CHECK(ok, "-chainpreview_scene_save='%s' is not <relative file.iscene>@"
                         "<editor frame above 0>[,...]; nothing is saved for the malformed "
                         "entries.", v.c_str());
        }
        if (pcl.Has("-chainpreview_request"))
        {
            // <kind>[:<arg>]@<editor frame>, comma separated (a load path
            // may not contain ',' or '@').
            const std::string v = pcl.Get("-chainpreview_request");
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                bool ok = it.find('@') != std::string::npos && splitAt(it, body, at);
                const std::size_t c = body.find(':');
                const std::string kind = body.substr(0, c);
                const std::string arg = c == std::string::npos ? std::string()
                    : Resource::AssetRefs::PercentDecode(body.substr(c + 1));
                ok = ok && chainPanel.HarnessAddRequest(kind, arg, at);
                ENGINE_CHECK(ok, "-chainpreview_request entry '%s' is not <load:<path>|reload|"
                             "close|useselected:<name>[#k]|select:<name>[#k]|"
                             "edit:<name>[#k][/<child>]|group:<new>=<a>+<b>|worldload>"
                             "@<editor frame above 0>; not requested.", it.c_str());
            }
        }
        if (pcl.Has("-chainpreview_confirm"))
        {
            const std::string v = pcl.Get("-chainpreview_confirm");
            std::vector<int> ans;
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                if      (it == "keep")    ans.push_back(0);
                else if (it == "discard") ans.push_back(1);
                else if (it == "apply")   ans.push_back(2);
                else ok = false;
            }
            ENGINE_CHECK(ok && chainPanel.HarnessSetConfirm(ans),
                         "-chainpreview_confirm='%s' is not a comma list of keep|discard|"
                         "apply; confirmations stay unanswered (the modal waits).", v.c_str());
        }
        if (pcl.Has("-chainpreview_reapply"))
        {
            const std::string v = pcl.Get("-chainpreview_reapply");
            const bool ok = v == "0" || v == "1";
            if (ENGINE_CHECK(ok, "-chainpreview_reapply='%s' is not 0 or 1; the re-apply "
                             "offer stays unanswered.", v.c_str()))
                chainPanel.HarnessSetReapply(v == "1" ? 1 : 0);
        }
        if (pcl.Has("-chainpreview_panel_tree_at"))
        {
            // Read by EditorLayer::TickChainPanelTreeProbe; checked here.
            std::uint64_t f = 0;
            ENGINE_CHECK(parseFrame(pcl.Get("-chainpreview_panel_tree_at"), f),
                         "-chainpreview_panel_tree_at='%s' is not an editor frame above 0; "
                         "the probe never runs.",
                         pcl.Get("-chainpreview_panel_tree_at").c_str());
        }
        if (pcl.Has("-chainpreview_play_edit"))
        {
            // Read by EditorLayer::TickChainPlayEditProbe: the -chainpreview_param grammar
            // plus the editor frame, checked here (a malformed spec never runs - the probe
            // re-parses the same text).
            const std::string v = pcl.Get("-chainpreview_play_edit");
            const std::size_t at = v.rfind('@');
            const std::string body = at == std::string::npos ? v : v.substr(0, at);
            const std::size_t eq = body.find('=');
            std::uint64_t f = 0;
            const bool ok = at != std::string::npos && eq != std::string::npos &&
                            parseFrame(v.substr(at + 1), f) &&
                            Editor::ChainPhysicsPreviewWindow::HarnessParamValid(
                                body.substr(0, eq), body.substr(eq + 1));
            ENGINE_CHECK(ok, "-chainpreview_play_edit='%s' is not <key>=<value>@<editor frame "
                         "above 0> with a -chainpreview_param key; the probe never runs.",
                         v.c_str());
        }
        if (pcl.Has("-chainpreview_play_keep_off")) chainPanel.HarnessSetPlayKeepOff();
        if (pcl.Has("-chainpreview_stats"))
        {
            const std::string v = pcl.Get("-chainpreview_stats");
            std::vector<std::pair<bool, std::uint64_t>> sched;
            bool ok = true;
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                ok = ok && splitAt(it, body, at) && (body == "0" || body == "1");
                sched.emplace_back(body == "1", at);
            }
            ENGINE_CHECK(ok && chainPanel.HarnessSetStats(sched),
                         "-chainpreview_stats='%s' is not <0|1>[@<editor frame>][,...]; the "
                         "stats box stays off (a harness subject's default).", v.c_str());
        }
        // ---- A/B ghost, solo, poke ----
        // -chainpreview_abparam and -chainpreview_poke are REPEATABLE (every argv
        // occurrence via GetAll, in order - the parsed command line keeps only the
        // last); malformed entries are ENGINE_CHECK + ignored.
        for (const std::string& list : pcl.GetAll("-chainpreview_abparam"))
        {
            for (const std::string& item : splitCsv(list))
            {
                if (item.empty()) continue;
                const std::size_t eq = item.find('=');
                std::string value = eq == std::string::npos ? std::string() : item.substr(eq + 1);
                std::uint64_t at = 0;
                bool atOk = true;
                const std::size_t atPos = value.rfind('@');
                if (atPos != std::string::npos)
                {
                    atOk = parseFrame(value.substr(atPos + 1), at);
                    value = value.substr(0, atPos);
                }
                const bool ok = eq != std::string::npos && eq > 0 && atOk &&
                    chainPanel.HarnessAddAbParam(item.substr(0, eq), value, at);
                ENGINE_CHECK(ok, "-chainpreview_abparam entry '%s' is not <key>=<value>"
                             "[@<editor frame above 0>] with a -chainpreview_param key; "
                             "ignored (B keeps A's value).", item.c_str());
            }
        }
        if (pcl.Has("-chainpreview_ab_same")) chainPanel.HarnessSetAbSame();
        if (pcl.Has("-chainpreview_ab_press"))
        {
            const std::string v = pcl.Get("-chainpreview_ab_press");
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                using AbPress = Editor::ChainPhysicsPreviewWindow::AbPress;
                const bool atOk = it.find('@') != std::string::npos && splitAt(it, body, at);
                const bool ok = atOk &&
                    (body == "swap"    ? chainPanel.HarnessAddAbPress(AbPress::Swap, at)
                   : body == "copy"    ? chainPanel.HarnessAddAbPress(AbPress::Copy, at)
                   : body == "promote" ? chainPanel.HarnessAddAbPress(AbPress::Promote, at)
                   : body == "off"     ? chainPanel.HarnessAddAbPress(AbPress::Off, at) : false);
                ENGINE_CHECK(ok, "-chainpreview_ab_press entry '%s' is not <swap|copy|promote|off>"
                             "@<editor frame above 0>; not pressed.", it.c_str());
            }
        }
        if (pcl.Has("-chainpreview_solo"))
        {
            const std::string v = pcl.Get("-chainpreview_solo");
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                const bool ok = splitAt(it, body, at) && !body.empty() &&
                    chainPanel.HarnessAddSolo(Resource::AssetRefs::PercentDecode(body), at);
                ENGINE_CHECK(ok, "-chainpreview_solo entry '%s' is not <group name|#idx|none>"
                             "[@<editor frame above 0>]; not applied.", it.c_str());
            }
        }
        if (pcl.Has("-chainpreview_solo_off")) chainPanel.HarnessSetSoloOff();
        eachArg("-chainpreview_poke", [&](const std::string& v)
        {
            const std::vector<std::string> c = splitCsv(v);
            std::uint64_t at = 0;
            float strength = 0.f, blend = 0.1f;
            return (c.size() == 3 || c.size() == 4) && parseFrame(c[0], at) &&
                   !c[1].empty() && parseF(c[2], strength) &&
                   (c.size() == 3 || parseF(c[3], blend)) &&
                   chainPanel.HarnessAddPoke(at, Resource::AssetRefs::PercentDecode(c[1]),
                                             strength, blend);
        });
        if (pcl.Has("-chainpreview_poke_probe"))
        {
            const std::string v = pcl.Get("-chainpreview_poke_probe");
            const std::vector<std::string> c = splitCsv(v);
            std::uint64_t a = 0, s = 0;
            const bool ok = c.size() == 2 && parseFrame(c[0], a) && parseFrame(c[1], s) &&
                chainPanel.HarnessSetPokeProbe(static_cast<std::uint32_t>(a),
                                               static_cast<std::uint32_t>(s));
            ENGINE_CHECK(ok, "-chainpreview_poke_probe='%s' is not <after>,<settle> editor "
                         "frames (0 < after < settle <= 5000); the default 3,180 applies.",
                         v.c_str());
        }
        // The in-picture rest-pose gizmo's harness gestures:
        // <bone|#idx>:<x|y|z|view>:<deg>:<frames>@<f>. The bone is everything before
        // the LAST three ':' fields (percent-decoded, so a ':' / ',' in a name can be
        // encoded).
        if (pcl.Has("-chainpreview_pose_gizmo"))
        {
            const std::string v = pcl.Get("-chainpreview_pose_gizmo");
            for (const std::string& it : splitCsv(v))
            {
                std::string body;
                std::uint64_t at = 0;
                bool ok = it.find('@') != std::string::npos && splitAt(it, body, at) && at != 0;
                std::string f[4];
                for (int k = 3; ok && k > 0; --k)
                {
                    const std::size_t c = body.rfind(':');
                    if (c == std::string::npos) { ok = false; break; }
                    f[k] = body.substr(c + 1);
                    body = body.substr(0, c);
                }
                f[0] = body;
                const int axis = f[1] == "x" ? 0 : f[1] == "y" ? 1 : f[1] == "z" ? 2
                               : f[1] == "view" ? 3 : -1;
                float deg = 0.f;
                std::uint64_t frames = 0;
                ok = ok && !f[0].empty() && axis >= 0 && parseF(f[2], deg) &&
                     parseFrame(f[3], frames) &&
                     chainPanel.HarnessAddPoseGesture(Resource::AssetRefs::PercentDecode(f[0]),
                                                      axis, deg,
                                                      static_cast<std::uint32_t>(frames), at);
                ENGINE_CHECK(ok, "-chainpreview_pose_gizmo entry '%s' is not <bone|#idx>:"
                             "<x|y|z|view>:<deg (|deg| <= 360)>:<frames 1..600>@<editor frame "
                             "above 0>; that gesture is not driven.", it.c_str());
            }
        }
        if (pcl.Has("-chainpreview_pose_local_order")) chainPanel.HarnessSetPoseLocalOrder();
        if (pcl.Has("-chainpreview_pose_gesture_off")) chainPanel.HarnessSetPoseGestureOff();
        if (pcl.Has("-chainpreview_main_pose"))
        {
            // Read by EditorLayer::TickChainMainPoseProbe (which re-parses the same
            // text); the grammar is checked here so a malformed spec never runs.
            const std::string v = pcl.Get("-chainpreview_main_pose");
            std::string body;
            std::uint64_t at = 0;
            const std::size_t c = v.find(':');
            const bool ok = v.find('@') != std::string::npos && splitAt(v, body, at) &&
                            at != 0 && c != std::string::npos && c > 0 &&
                            c + 1 < body.size();
            ENGINE_CHECK(ok, "-chainpreview_main_pose='%s' is not <entity name>[#k]:<bone|#idx|*>"
                         "@<editor frame above 0>; the probe never runs.", v.c_str());
        }
        chainPanel.HarnessSetSubject(subject);
        LOG_INFO("-chainpreview_subject='%s' size='%s' frames=%s close_at=%s%s%s; "
                 "opening the Chain Physics Preview panel.",
                 subject.c_str(), pcl.Get("-chainpreview_size").c_str(),
                 pcl.Has("-chainpreview_frames")
                     ? pcl.Get("-chainpreview_frames").c_str() : "-",
                 pcl.Has("-chainpreview_close_at")
                     ? pcl.Get("-chainpreview_close_at").c_str() : "-",
                 pcl.Has("-chainpreview_strip_off")   ? " strip_off" : "",
                 pcl.Has("-chainpreview_release_off") ? " release_off" : "");
    }
}

#endif // WITH_HARNESS
