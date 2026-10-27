// Editor::PreviewHarness - the editor host's harness flags, the steps they
// schedule and the files they put back; see Editor/PreviewHarness.h.

#include "Editor/PreviewHarness.h"

#if defined(WITH_HARNESS)

#include "Diagnostics/OwnerAccounting.h"     // Diag::DrainCause
#include "Editor/ChainPhysicsPreviewHarness.h"
#include "Editor/EditorLayer.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/MaterialPreviewWidget.h"
#include "Editor/MeshEditorWindow.h"
#include "Editor/NodeEditorSmoke.h"
#include "Editor/PreviewThumbnailCache.h"
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/TextureViewerWindow.h"
#include "Editor/UIEditorWindow.h"    // -uieditor_*
#include "Harness/ExitCodes.h"
#include "Harness/FlagRefusal.h"
#include "Renderer/Preview/PreviewStudioRig.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"
#include "Scene/GameModeContext.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // ==== the crash-safe half of the harness file restores ====
    // The in-memory restore in Shutdown runs on the ORDERLY exit path only, so the
    // original bytes also go to a sidecar BEFORE the overwrite and the next boot
    // puts them back (see PreviewHarness.h). The sidecar directory is under the
    // writable data root, not under -artifacts= (a fresh directory per run, which
    // the next boot cannot find). Format, readable in a hex editor:
    //     <absolute destination path>\n<decimal byte count>\n<raw bytes>
    // Absolute, so the next boot need not re-resolve a root-relative name through
    // an AssetFS that may be configured differently.
    const char* kRewriteSidecarExt = ".rewrite";
    // -preview_touch_at changes no byte, only the write time, so its sidecar
    // carries the time alone:
    //     <absolute destination path>\n<file_time_type tick count>\n
    // Without it a run that died after the touch would leave the tracked asset with
    // the touched time: the in-memory restore in Shutdown() never runs.
    const char* kTouchSidecarExt = ".touch";

    std::string HarnessRewriteSidecarDir()
    {
        return Sys::WritableDataDir() + "/harness_restore";
    }

    // Puts back the write time a .touch sidecar recorded. True = consumed (the
    // time is back, or the file is gone and there is nothing to put it on).
    bool RestoreTouchSidecar(const std::filesystem::path& sidecarPath)
    {
        const std::string sidecar = Sys::PathToUtf8(sidecarPath);
        std::ifstream in(sidecarPath, std::ios::binary);
        std::string target, ticksLine;
        if (!in || !std::getline(in, target) || !std::getline(in, ticksLine))
        {
            LOG_ERROR("[PREVIEW] touch: sidecar '%s' is unreadable; kept for inspection",
                      sidecar.c_str());
            return false;
        }
        in.close();
        long long ticks = 0;
        try { ticks = std::stoll(ticksLine); }
        catch (...)
        {
            LOG_ERROR("[PREVIEW] touch: sidecar '%s' has no time; kept for inspection",
                      sidecar.c_str());
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::exists(Sys::U8Path(target), ec))
        {
            LOG_WARNING("[PREVIEW] touch: '%s' named by sidecar '%s' no longer exists; "
                        "nothing to put back", target.c_str(), sidecar.c_str());
            return true;
        }
        std::filesystem::last_write_time(
            Sys::U8Path(target),
            std::filesystem::file_time_type(std::filesystem::file_time_type::duration(ticks)), ec);
        if (ec)
        {
            LOG_ERROR("[PREVIEW] touch: could not put the write time of '%s' back from "
                      "its sidecar (%s)", target.c_str(), ec.message().c_str());
            return false;
        }
        LOG_INFO("[PREVIEW] touch: boot restore put back the write time of '%s' from a "
                 "sidecar left by a run that did not shut down", target.c_str());
        return true;
    }

    // Returns the number of files it put back. Runs at BOOT, before anything
    // reads an asset, and deletes each sidecar it consumed.
    std::size_t RestorePendingHarnessRewrites()
    {
        std::error_code ec;
        const std::string dir = HarnessRewriteSidecarDir();
        if (!std::filesystem::is_directory(Sys::U8Path(dir), ec)) return 0;
        std::size_t restored = 0;
        for (const auto& ent :
             std::filesystem::directory_iterator(Sys::U8Path(dir), ec))
        {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            const std::string sidecar = Sys::PathToUtf8(ent.path());
            const std::string touchExt(kTouchSidecarExt);
            if (sidecar.size() > touchExt.size() &&
                sidecar.compare(sidecar.size() - touchExt.size(), touchExt.size(), touchExt) == 0)
            {
                if (RestoreTouchSidecar(ent.path()))
                {
                    std::error_code rec;
                    std::filesystem::remove(ent.path(), rec);
                }
                continue;
            }
            const std::string ext(kRewriteSidecarExt);
            if (sidecar.size() <= ext.size() ||
                sidecar.compare(sidecar.size() - ext.size(), ext.size(), ext) != 0)
                continue;
            std::ifstream in(ent.path(), std::ios::binary);
            if (!in) continue;
            std::string target, countLine;
            if (!std::getline(in, target) || !std::getline(in, countLine)) continue;
            std::size_t want = 0;
            try { want = static_cast<std::size_t>(std::stoull(countLine)); }
            catch (...) { continue; }
            std::vector<char> bytes(want);
            in.read(bytes.data(), static_cast<std::streamsize>(want));
            const bool full = (static_cast<std::size_t>(in.gcount()) == want);
            in.close();
            // A TRUNCATED SIDECAR IS NEVER APPLIED. Writing half the original
            // bytes over the fixture would corrupt a tracked file for good,
            // which is strictly worse than leaving the rewrite in place - and
            // the file is kept so the failure is visible rather than swept up.
            if (!full || want == 0)
            {
                LOG_ERROR("[PREVIEW] asset rewrite: sidecar '%s' is truncated "
                          "(want %zu bytes); '%s' was NOT restored and the "
                          "sidecar is kept for inspection",
                          sidecar.c_str(), want, target.c_str());
                continue;
            }
            bool ok = false;
            {
                std::ofstream out(Sys::U8Path(target),
                                  std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(bytes.data(), static_cast<std::streamsize>(want));
                    out.close();
                    ok = !out.fail();
                }
            }
            if (ok)
            {
                ++restored;
                std::error_code rec;
                std::filesystem::remove(ent.path(), rec);
                LOG_INFO("[PREVIEW] asset rewrite: restored %zu byte(s) of '%s' "
                         "from a sidecar left by a run that did not shut down",
                         want, target.c_str());
            }
            else
                LOG_ERROR("[PREVIEW] asset rewrite: could not write '%s' back "
                          "from its sidecar", target.c_str());
        }
        if (restored)
            LOG_INFO("[PREVIEW] asset rewrite: boot restore put %zu file(s) back",
                     restored);
        return restored;
    }

    // The restore sidecar write (format and location above), shared by every
    // harness step that overwrites a file. Call it BEFORE the file changes.
    // Returns the sidecar path, or "" when it could not be written.
    std::string WriteHarnessRestoreSidecar(const std::string& dstFull,
                                           const std::vector<std::uint8_t>& original)
    {
        std::error_code sec;
        std::filesystem::create_directories(
            Sys::U8Path(HarnessRewriteSidecarDir()), sec);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%016llx",
                 static_cast<unsigned long long>(
                     std::hash<std::string>{}(dstFull)));
        const std::string side =
            HarnessRewriteSidecarDir() + "/" + stamp + kRewriteSidecarExt;
        // ABSOLUTE, and that is not a detail: AssetFS hands back a path
        // relative to the working directory, and the NEXT boot is a different
        // process that may not have the same one. A relative target in a
        // sidecar is a restore that silently writes nothing (or, worse, into
        // some other tree).
        std::error_code aec;
        std::string absDst = Sys::PathToUtf8(
            std::filesystem::absolute(Sys::U8Path(dstFull), aec));
        if (aec || absDst.empty()) absDst = dstFull;
        std::ofstream so(Sys::U8Path(side), std::ios::binary | std::ios::trunc);
        if (!so) return std::string();
        so << absDst << "\n" << original.size() << "\n";
        so.write(reinterpret_cast<const char*>(original.data()),
                 static_cast<std::streamsize>(original.size()));
        so.close();
        return so.fail() ? std::string() : side;
    }

    // The .touch twin of the sidecar above (format at kTouchSidecarExt). Call
    // BEFORE the touch; returns the sidecar path, or "" when it could not be written.
    std::string WriteTouchSidecar(const std::string& dstFull,
                                  std::filesystem::file_time_type original)
    {
        std::error_code sec;
        std::filesystem::create_directories(
            Sys::U8Path(HarnessRewriteSidecarDir()), sec);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%016llx",
                 static_cast<unsigned long long>(
                     std::hash<std::string>{}(dstFull)));
        const std::string side =
            HarnessRewriteSidecarDir() + "/" + stamp + kTouchSidecarExt;
        // Absolute, for the reason the rewrite sidecar's target is.
        std::error_code aec;
        std::string absDst = Sys::PathToUtf8(
            std::filesystem::absolute(Sys::U8Path(dstFull), aec));
        if (aec || absDst.empty()) absDst = dstFull;
        std::ofstream so(Sys::U8Path(side), std::ios::binary | std::ios::trunc);
        if (!so) return std::string();
        so << absDst << "\n"
           << static_cast<long long>(original.time_since_epoch().count()) << "\n";
        so.close();
        return so.fail() ? std::string() : side;
    }

    // THIS checkout's copy of an asset a harness step is about to WRITE. A
    // worktree in shared-content mode (tools/worktree_content.py) reads the
    // payload it does not check out from the main checkout, so ResolveOnDisk
    // can answer a path in ANOTHER tree - and a write, a restore or an mtime
    // bump through it would change a checkout this run does not own. A path
    // only the shared root has is refused, loudly, with the command that makes
    // a local copy.
    std::string HarnessWritablePath(const std::string& rel, const char* flag,
                                    bool* sharedOnly = nullptr)
    {
        const auto& afs = ::Resource::AssetFS::Get();
        const std::string local = afs.ResolveLocal(rel);
        const bool onlyShared = local.empty() && !afs.ResolveOnDisk(rel).empty();
        if (onlyShared)
            LOG_ERROR("%s: '%s' is only in the shared content root '%s'; a harness "
                      "step never writes another checkout. Make a local copy first: "
                      "python tools/worktree_content.py materialize %s",
                      flag, rel.c_str(), afs.SharedRootDir().c_str(), rel.c_str());
        if (sharedOnly) *sharedOnly = onlyShared;
        return local;
    }
}

std::size_t Editor::PreviewHarness::RestorePendingRewritesAtBoot()
{
    return RestorePendingHarnessRewrites();
}

// ONE mechanism for every harness flag that overwrites a file in the asset
// tree (see the header).
bool Editor::PreviewHarness::ArmHarnessFileRestore(const std::string& dstFull, const char* flag,
                                const char* tag)
{
    // ALREADY ARMED WINS. Re-reading the file now would capture whatever the
    // first write left there and pin THAT as "the original" - the exact way a
    // restore turns into a corruption.
    for (const HarnessFileRestore& r : m_harnessFileRestores)
        if (r.path == dstFull) return true;

    std::ifstream in(Sys::U8Path(dstFull), std::ios::binary);
    std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
    in.close();
    if (original.empty()) return false;

    HarnessFileRestore r;
    r.path    = dstFull;
    r.flag    = flag ? flag : "";
    r.tag     = tag  ? tag  : "harness file";
    r.bytes   = std::move(original);
    r.sidecar = WriteHarnessRestoreSidecar(dstFull, r.bytes);
    std::error_code rec;
    r.time      = std::filesystem::last_write_time(Sys::U8Path(dstFull), rec);
    r.timeValid = !rec;
    if (r.sidecar.empty())
        LOG_WARNING("%s: could not write the restore sidecar under '%s'; only "
                    "the in-memory restore protects '%s' from an abnormal exit.",
                    r.flag.c_str(), HarnessRewriteSidecarDir().c_str(),
                    dstFull.c_str());
    LOG_INFO("[PREVIEW] harness file restore armed: '%s' (%zu B, %s) by %s",
             dstFull.c_str(), r.bytes.size(),
             r.sidecar.empty() ? "in memory only" : "sidecar + memory",
             r.flag.c_str());
    m_harnessFileRestores.push_back(std::move(r));
    return true;
}

// ==== the editor-host harness flags ====
// Each flag is latched here and acted on later at a frame boundary, never
// mid-frame (AfterFrame, DeliverDue or the panel's own serial window). App calls
// this only in WITH_EDITOR + WITH_HARNESS builds and only when
// Sys::IsEditorHost(); the order of the refusals and their log lines is the
// packs' contract.
void Editor::PreviewHarness::ParseBootFlags()
{
    const auto& pcl = Sys::CommandLine::Get();

    // One shape for "this flag needs a value": Harness::RequireValue
    // (Harness/FlagRefusal.h).
    auto needValue = [&pcl](const char* flag, const char* usage)
    {
        Harness::RequireValue(pcl, flag, usage);
    };

    // ---- a -preview_* flag forces the pref on, and says so ----
    // editor_prefs.ini's `preview.enabled` (half of the preview kill switch) is a
    // TRACKED file the editor rewrites on exit, so one unticked panel would turn
    // every preview pack red on a machine where nothing changed. A pack that names
    // the feature is an explicit request for it. The ENV half (DX12_PREVIEW=0) is
    // NOT overridden: it is set per run and is the escape hatch of last resort.
    bool anyPreviewFlag = false;
    // A LITERAL prefix test, so the chain preview's flags are named too:
    // "-chainpreview_subject" does not start with "-preview_", and without it
    // every chain_preview_* pack goes red on a machine whose editor_prefs says
    // preview.enabled=0.
    for (const std::string& a : pcl.Args())
        if (a.rfind("-preview_", 0) == 0 || a.rfind("-chainpreview_", 0) == 0)
        { anyPreviewFlag = true; break; }
    if (anyPreviewFlag && !Editor::MaterialPreviewWidget::Get().PrefEnabled())
    {
        // TRANSIENT: editor_prefs.ini is a TRACKED file this editor rewrites on exit,
        // so a persistent SetPrefEnabled(true) would make every Editor preview pack
        // commit `preview.enabled=1` into the working tree over a developer's
        // deliberate 0. The force applies to THIS RUN; the save writes the value that
        // was there before it.
        Editor::MaterialPreviewWidget::Get().ForcePrefEnabledForHarness();
        LOG_INFO("[PREVIEW] editor_prefs preview.enabled was 0 and a "
                 "-preview_* flag was given; forcing it ON for this run "
                 "ONLY - the saved value is untouched (and the DX12_PREVIEW "
                 "env half is untouched too).");
    }

    previewMaterial   = pcl.Get("-preview_material");
    previewStartFrame = pcl.GetInt("-preview_frames", 0);
    // -preview_screenshot is consumed by HarnessCaptureSystem (same one-shot
    // trigger as -screenshot), so it is deliberately absent from this block.

    // A BARE FLAG IS A REFUSAL, not a no-op. `Get` returns the fallback for
    // `-preview_material` with no `=value`, so without these checks a typo'd pack
    // argument runs the whole scenario with the panel never opened and goes GREEN
    // having tested nothing - the false pass this harness exists to remove.
    needValue("-preview_material",
              "-preview_material=<path to .imat/.imatinst>");
    needValue("-preview_frames", "-preview_frames=<N>");
    // -ui_screenshot: the whole editor window as presented
    // (HarnessCaptureSystem + GraphicsDX12::EndFrame; editor host only).
    needValue("-ui_screenshot", "-ui_screenshot=<file.png>@<frame>[,...]");
    needValue("-inspector_scroll_to", "-inspector_scroll_to=<component label>");

    // ---- size / shape overrides ----
    // Under -hidden the panel's content region is <= 0 every frame (the window is
    // SW_SHOWMINNOACTIVE, io.DisplaySize 0x0), so without these flags a harness run
    // stays at 256x256 and never reaches the RESIZE path (SetFixedDimensions +
    // MarkDirty + Compile, recreating the preview graph's seven physical textures
    // under live targets and a live ImGui descriptor). A value the widget cannot
    // snap to by hand is REFUSED OUT LOUD: it would exercise a recompile no user
    // can provoke, and a silently ignored flag is a false pass.
    const std::string previewSizeStr   = pcl.Get("-preview_size");
    const std::string previewResizeStr = pcl.Get("-preview_resize");
    const std::string previewShapeStr  = pcl.Get("-preview_shape");
    needValue("-preview_size",   "-preview_size=<128|256|384|512>");
    needValue("-preview_resize", "-preview_resize=<frame>:<size>");
    needValue("-preview_shape",  "-preview_shape=<sphere|cube|plane|torus>");

    // The size the panel actually COMMITTED to, or 0 when the flag was
    // absent or refused. -preview_parity reads this rather than re-parsing
    // the flag, so the quantisation is enforced once, by the one function
    // that owns it (EditorLayer::HarnessSetPreviewSize).
    std::uint32_t previewSizePx = 0;
    if (!previewSizeStr.empty())
    {
        const std::uint32_t px = static_cast<std::uint32_t>(
            pcl.GetInt("-preview_size", 0));
        if (ENGINE_CHECK(m_editorLayer.HarnessSetPreviewSize(px),
                         "-preview_size=%s is not one of the four sizes the panel "
                         "quantises to (128/256/384/512); ignored.",
                         previewSizeStr.c_str()))
            previewSizePx = px;
    }
    if (!previewResizeStr.empty())
    {
        // "<frame>:<size>", both decimal. Parsed by hand rather than with
        // sscanf so a missing colon is a refusal rather than a partial read.
        const std::size_t colon = previewResizeStr.find(':');
        bool ok = false;
        if (colon != std::string::npos && colon > 0 &&
            colon + 1 < previewResizeStr.size())
        {
            const std::uint64_t frame = std::strtoull(
                previewResizeStr.substr(0, colon).c_str(), nullptr, 10);
            const std::uint32_t px = static_cast<std::uint32_t>(std::strtoul(
                previewResizeStr.substr(colon + 1).c_str(), nullptr, 10));
            ok = m_editorLayer.HarnessSetPreviewResize(frame, px);
        }
        ENGINE_CHECK(ok,
                     "-preview_resize='%s' is not <frame>:<size> with a size of "
                     "128/256/384/512; ignored, and the pack would assert on a "
                     "recompile that never happens.",
                     previewResizeStr.c_str());
    }
    if (!previewShapeStr.empty())
    {
        ENGINE_CHECK(m_editorLayer.HarnessSetPreviewShape(previewShapeStr),
                     "-preview_shape='%s' is not sphere/cube/plane/torus; ignored.",
                     previewShapeStr.c_str());
    }

    // ---- view controls ----
    // Same discipline as the three above, for the same reason: a bare flag or an
    // unrecognised value is REFUSED OUT LOUD, because otherwise a pack goes green
    // having rendered the default configuration under the new flag's name.
    {
        needValue("-preview_background",
                  "-preview_background=<solid|checker|skybox>");
        const std::string bg = pcl.Get("-preview_background");
        if (!bg.empty())
            ENGINE_CHECK(m_editorLayer.HarnessSetPreviewBackground(bg),
                         "-preview_background='%s' is not solid/checker/skybox; "
                         "ignored.", bg.c_str());
    }
    {
        needValue("-preview_mesh", "-preview_mesh=<path.meshlib>[#meshId]");
        const std::string mesh = pcl.Get("-preview_mesh");
        if (!mesh.empty())
            ENGINE_CHECK(m_editorLayer.HarnessSetPreviewMesh(mesh),
                         "-preview_mesh='%s' is not <path.meshlib>[#id]; ignored, "
                         "and the preview would draw its primitive under the "
                         "flag's name. NOTE .imsh is refused by design - the "
                         "legacy single-mesh render pool no longer exists.",
                         mesh.c_str());
    }
    {
        needValue("-preview_orbit", "-preview_orbit=<yawDeg>,<pitchDeg>");
        const std::string orbit = pcl.Get("-preview_orbit");
        if (!orbit.empty())
            ENGINE_CHECK(m_editorLayer.HarnessSetPreviewOrbit(orbit),
                         "-preview_orbit='%s' is not <yawDeg>,<pitchDeg>; "
                         "ignored, and the pack would measure the default "
                         "framing under the flag's name.", orbit.c_str());
    }
    {
        needValue("-preview_rig", "-preview_rig=<full|key>");
        const std::string rig = pcl.Get("-preview_rig");
        if (!rig.empty())
            ENGINE_CHECK(m_editorLayer.HarnessSetPreviewRig(rig),
                         "-preview_rig='%s' is not full/key; ignored.", rig.c_str());
    }
    needValue("-preview_shadow", "-preview_shadow=<0|1>");
    if (pcl.Has("-preview_shadow"))
    {
        const std::string v = pcl.Get("-preview_shadow");
        ENGINE_CHECK(v == "0" || v == "1",
                     "-preview_shadow needs 0 or 1 (-preview_shadow=<0|1>); "
                     "got '%s'.", v.c_str());
        if (v == "0" || v == "1")
            m_editorLayer.HarnessSetPreviewShadow(v == "1");
    }
    needValue("-preview_scene_lighting", "-preview_scene_lighting=<0|1>");
    if (pcl.Has("-preview_scene_lighting"))
    {
        const std::string v = pcl.Get("-preview_scene_lighting");
        ENGINE_CHECK(v == "0" || v == "1",
                     "-preview_scene_lighting needs 0 or 1; got '%s'.", v.c_str());
        if (v == "0" || v == "1")
            m_editorLayer.HarnessSetPreviewSceneLighting(v == "1");
    }

    // ---- fixture coverage ----
    // -preview_fixture=<name>[,<name>...] cycles SYNTHESISED materials through the
    // panel - one per shading model / blend mode the fidelity matrix makes a claim
    // about. Nine of the ten have no .imat anywhere in the tree, so without this no
    // pack can reach them (see PreviewFixtures.h).
    const std::string previewFixtureStr = pcl.Get("-preview_fixture");
    needValue("-preview_fixture",       "-preview_fixture=<name>[,<name>...]");
    needValue("-preview_fixture_every", "-preview_fixture_every=<N frames>");
    needValue("-preview_edit_at",       "-preview_edit_at=<frame>");
    needValue("-preview_asset_edit",    "-preview_asset_edit=<.imat path>");
    needValue("-preview_parity",        "-preview_parity=<fixture>");
    needValue("-preview_shadow_alpha",  "-preview_shadow_alpha=<0|1>[,<0|1>...]");
    needValue("-preview_shadow_win",    "-preview_shadow_win=<0|1>[,<0|1>...]");
    // THE SAME PAIRING REFUSAL -preview_asset_edit takes. The lever is flipped per
    // FIXTURE WINDOW and nothing else opens one, so without -preview_fixture the
    // flag is silently inert - the false pass every bare-flag check in this file
    // exists to refuse.
    if (pcl.Has("-preview_shadow_alpha") && previewFixtureStr.empty())
    {
        LOG_ERROR("-preview_shadow_alpha needs -preview_fixture=<name>[,...] "
                  "beside it: the lever is applied when a fixture WINDOW "
                  "opens, so on its own it changes nothing at all.");
        PostQuitMessage(Harness::PreconditionFail);
    }
    // -preview_shadow_win: THE SAME TWO REFUSALS. The studio shadow PASS (not its
    // alpha stage) per fixture window, so one run can capture the same fixture with
    // the shadow and without it and expect.imagePair - which compares two captures
    // of ONE run - can bound the difference.
    if (pcl.Has("-preview_shadow_win") && previewFixtureStr.empty())
    {
        LOG_ERROR("-preview_shadow_win needs -preview_fixture=<name>[,...] "
                  "beside it: the lever is applied when a fixture WINDOW "
                  "opens, so on its own it changes nothing at all.");
        PostQuitMessage(Harness::PreconditionFail);
    }
    // AND THE CONTRADICTION. -preview_shadow is a start-up setting applied once;
    // -preview_shadow_win rewrites the same bit at every window boundary. Declaring
    // both makes the pack's stated shadow state whichever ran last.
    if (pcl.Has("-preview_shadow_win") && pcl.Has("-preview_shadow"))
    {
        LOG_ERROR("-preview_shadow_win and -preview_shadow both set the "
                  "studio shadow: the first per fixture window, the second "
                  "once at start-up. Refusing to run rather than letting "
                  "the window schedule silently overwrite the flag the pack "
                  "declared.");
        PostQuitMessage(Harness::PreconditionFail);
    }
    // A PAIRING, not a value: -preview_asset_edit on its own seeds the
    // before-material and never edits it, so the pack asserts a one-frame
    // update that cannot happen. Same refusal for the same reason.
    if (pcl.Has("-preview_asset_edit") && !pcl.Has("-preview_edit_at"))
    {
        LOG_ERROR("-preview_asset_edit needs -preview_edit_at=<frame> to say "
                  "WHEN. Refusing to run: on its own it seeds the "
                  "before-material and never edits it.");
        PostQuitMessage(Harness::PreconditionFail);
    }

    if (!previewFixtureStr.empty())
    {
        std::vector<std::string> names;
        for (std::size_t i = 0; i <= previewFixtureStr.size(); )
        {
            const std::size_t c = previewFixtureStr.find(',', i);
            const std::size_t e = (c == std::string::npos)
                                ? previewFixtureStr.size() : c;
            if (e > i) names.emplace_back(previewFixtureStr.substr(i, e - i));
            if (c == std::string::npos) break;
            i = c + 1;
        }
        const int every = pcl.GetInt("-preview_fixture_every", 40);
        ENGINE_CHECK(m_editorLayer.HarnessSetPreviewFixtures(
                         names,
                         static_cast<std::uint64_t>(
                             previewStartFrame > 0 ? previewStartFrame : 1),
                         static_cast<std::uint32_t>(every > 0 ? every : 40)),
                     "-preview_fixture='%s' names a fixture that does not "
                     "exist; nothing was scheduled.",
                     previewFixtureStr.c_str());

        // ---- -preview_shadow_alpha=<0|1>[,<0|1>...] ----
        // The studio shadow pass's alpha-test PIXEL STAGE, one entry per fixture window
        // (the last entry repeats). Parsed HERE, inside the fixture block, because that
        // is the only clock it has: the lever is flipped when a window opens.
        //
        // MALFORMED IS AN ENGINE_CHECK AND A DOCUMENTED FALLBACK, not a refusal - the
        // shipped behaviour (every window clipped) is a perfectly good run and the
        // CHECK is what makes the typo visible. A MISSING VALUE is the hard refusal
        // (needValue, before this block), with every other flag.
        const std::string shadowAlphaStr = pcl.Get("-preview_shadow_alpha");
        if (!shadowAlphaStr.empty())
        {
            std::vector<std::uint8_t> perWindow;
            bool bad = false;
            for (std::size_t i = 0; i <= shadowAlphaStr.size(); )
            {
                const std::size_t c = shadowAlphaStr.find(',', i);
                const std::size_t e = (c == std::string::npos)
                                    ? shadowAlphaStr.size() : c;
                const std::string tok = shadowAlphaStr.substr(i, e - i);
                if      (tok == "0") perWindow.push_back(0);
                else if (tok == "1") perWindow.push_back(1);
                else                 bad = true;
                if (c == std::string::npos) break;
                i = c + 1;
            }
            ENGINE_CHECK(!bad && !perWindow.empty(),
                "-preview_shadow_alpha='%s' must be a comma list of 0 and 1, "
                "one per fixture window; the run keeps the shipped "
                "alpha-tested studio shadow for every window.",
                shadowAlphaStr.c_str());
            if (!bad && !perWindow.empty())
                m_editorLayer.HarnessSetPreviewShadowAlpha(perWindow);
        }

        // ---- -preview_shadow_win=<0|1>[,<0|1>...] ----
        // The studio shadow PASS per fixture window, parsed here for the same reason as
        // the lever above: a fixture window is its only clock. Malformed is an
        // ENGINE_CHECK with a documented fallback (the shipped default, shadow ON for
        // every window); a missing value is the hard refusal, with every other flag.
        const std::string shadowWinStr = pcl.Get("-preview_shadow_win");
        if (!shadowWinStr.empty())
        {
            std::vector<std::uint8_t> perWindow;
            bool bad = false;
            for (std::size_t i = 0; i <= shadowWinStr.size(); )
            {
                const std::size_t c = shadowWinStr.find(',', i);
                const std::size_t e = (c == std::string::npos)
                                    ? shadowWinStr.size() : c;
                const std::string tok = shadowWinStr.substr(i, e - i);
                if      (tok == "0") perWindow.push_back(0);
                else if (tok == "1") perWindow.push_back(1);
                else                 bad = true;
                if (c == std::string::npos) break;
                i = c + 1;
            }
            ENGINE_CHECK(!bad && !perWindow.empty(),
                "-preview_shadow_win='%s' must be a comma list of 0 and 1, "
                "one per fixture window; the run keeps the shipped studio "
                "shadow ON for every window.", shadowWinStr.c_str());
            if (!bad && !perWindow.empty())
                m_editorLayer.HarnessSetPreviewShadowWindows(perWindow);
        }
    }

    if (pcl.Has("-preview_edit_at"))
        m_editorLayer.HarnessSetPreviewEditAt(
            static_cast<std::uint64_t>(pcl.GetInt("-preview_edit_at", 0)));

    // -preview_asset_edit: retarget that one-shot at an asset PART material,
    // edited the way the editor's Save button edits one - by writing the file.
    if (!pcl.Get("-preview_asset_edit").empty())
    {
        const std::string editRel = pcl.Get("-preview_asset_edit");
        // ---- protect the file before anything writes ----
        // This flag overwrites an asset-tree .imat TWICE per run (a dull "before" at
        // the first harness tick, a bright "after" at -preview_edit_at); a run killed
        // between the two would leave the checked-in fixture holding the BEFORE
        // material, a dirty tree nobody connects to a scenario run. Armed at PARSE
        // TIME, the only moment certainly before the first write (the seed is harness
        // tick 2); AssetFS is mounted first thing in App::Run, so the resolve is real.
        bool editShared = false;
        const std::string editFull =
            HarnessWritablePath(editRel, "-preview_asset_edit", &editShared);
        if (editShared)
        {
            // Only the shared content root has it (logged above): the save
            // would create a local copy nothing restores.
            PostQuitMessage(Harness::PreconditionFail);
        }
        else if (editFull.empty() ||
            !ArmHarnessFileRestore(editFull, "-preview_asset_edit",
                                   "asset edit"))
        {
            // NOT a refusal: SaveMaterial will create the file and the run
            // is still a valid measurement. It IS a warning, because the
            // run then leaves a file behind that nothing puts back.
            LOG_WARNING("-preview_asset_edit='%s' does not resolve to an "
                        "existing file on disk (%s), so no restore was "
                        "armed: this run will leave whatever it writes "
                        "there. Point the flag at a fixture that exists.",
                        editRel.c_str(),
                        editFull.empty() ? "unresolved" : "unreadable");
        }
        if (!editShared) m_editorLayer.HarnessSetPreviewAssetEditMaterial(editRel);
    }

    // -preview_parity: the main pipeline renders the SAME fixture under the SAME
    // rig, into the SAME square viewport, so the two pictures can be compared. The
    // square is the preview's own committed size, taken from -preview_size rather
    // than assumed - see PreviewHarness::m_previewParityPx.
    previewParityFixture = pcl.Get("-preview_parity");
    if (!previewParityFixture.empty())
    {
        // previewSizePx, NOT a fresh GetInt of the same flag: a raw -preview_size=300
        // would pin the panel to its default 256 (the quantiser refuses it) while
        // forcing the MAIN viewport to 300x300, and the run would die in the runner
        // with "the two captures are not the same size" - a message about the
        // comparator rather than about the flag that was wrong. Gated on the committed
        // size, a bad -preview_size is refused twice, by name, before a frame is drawn.
        ENGINE_CHECK(previewSizePx != 0,
                     "-preview_parity needs -preview_size=<128|256|384|512> "
                     "as well, and that size must be one the panel quantises "
                     "to: the two pictures must be the same size and the "
                     "editor viewport is not square. Nothing was configured "
                     "and the A/B comparison cannot run.");
        m_previewParityPx = previewSizePx;
    }

    // ---- -preview_lights_parity + -preview_pip_parity ----
    // The SECONDARY-VIEW parity pair: -preview_parity compares the main pipeline
    // against the SYNTHETIC lane, this one against the WORLDCULL lane (a second
    // camera on the same world), so it needs no -preview_size and no fixture
    // material - the picture under test is the PiP (-preview_pip).
    // -preview_pip_parity=<px> forces the MAIN viewport SQUARE at px
    // (m_previewParityPx: compare_images refuses two sizes, and 16:9 would move the
    // geometry's sub-pixel centres) and switches PinParityCamera to
    // Preview::kLightsParityCamera - the CONSTANT, never the camera entity, so
    // CameraResolveSystem and the VCam blend stack cannot add a frame of lag that
    // reads as shading.
    needValue("-preview_lights_parity", "-preview_lights_parity=<baseline|lights>");
    needValue("-preview_pip_parity",    "-preview_pip_parity=<px>");
    previewLightsParityArm = pcl.Get("-preview_lights_parity");
    if (!previewLightsParityArm.empty())
    {
        if (previewLightsParityArm != "baseline" &&
            previewLightsParityArm != "lights")
        {
            LOG_ERROR("-preview_lights_parity='%s' is not a known arm "
                      "(baseline|lights). Refusing to run: an unknown arm "
                      "builds no fixture world and the pack would compare "
                      "two pictures of the boot scene.",
                      previewLightsParityArm.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        const int px = pcl.GetInt("-preview_pip_parity", 0);
        if (px <= 0)
        {
            LOG_ERROR("-preview_lights_parity needs -preview_pip_parity=<px> "
                      "beside it: without it the main viewport keeps the "
                      "editor's 16:9 size, the two captures are different "
                      "sizes and compare_images refuses the pair - a failure "
                      "about the comparator rather than about the flag.");
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
        {
            ENGINE_CHECK(previewParityFixture.empty(),
                "-preview_lights_parity and -preview_parity both build a "
                "fixture World and the second one wins; declare exactly one.");
            m_previewParityPx  = static_cast<uint32_t>(px);
            m_previewPipParity = true;
            LOG_INFO("-preview_lights_parity='%s' with the main viewport "
                     "forced to %dx%d and the main camera pinned to "
                     "Preview::kLightsParityCamera.",
                     previewLightsParityArm.c_str(), px, px);
        }
    }
    else if (pcl.Has("-preview_pip_parity"))
    {
        LOG_ERROR("-preview_pip_parity without -preview_lights_parity=<arm>: "
                  "it would square and re-pin the main viewport for a run "
                  "with no fixture world to compare against. Refusing.");
        PostQuitMessage(Harness::PreconditionFail);
    }

    // ---- asset-browser material tiles ----
    // -preview_thumb_cache_dir FIRST OF ALL, before anything can resolve the
    // default. Sys::WritableDataDir() is NOT moved by -artifacts=, so without this
    // flag every scenario run reads, writes and (with =clear) DELETES the
    // developer's real thumbnail cache. A relative value is taken relative to
    // Sys::ArtifactDir(), which -artifacts= does move, so a pack says "thumbcache"
    // and gets a directory inside its own artifacts folder that no other run can
    // see.
    {
        needValue("-preview_thumb_cache_dir", "-preview_thumb_cache_dir=<dir>");
        const std::string cacheDir = pcl.Get("-preview_thumb_cache_dir");
        if (!cacheDir.empty())
            ENGINE_CHECK(Editor::PreviewThumbnailCache::Get()
                             .SetDirOverride(cacheDir),
                         "-preview_thumb_cache_dir='%s' was refused - the "
                         "cache directory had already been resolved.",
                         cacheDir.c_str());
    }
    // -preview_thumb_cache=clear before anything reads the cache. With the
    // directory pointed at the artifacts folder this is normally a no-op
    // (the folder is new), and it stays in the pack because the COLD claim
    // should not depend on the runner having handed out a fresh directory.
    {
        needValue("-preview_thumb_cache", "-preview_thumb_cache=clear");
        const std::string cacheOp = pcl.Get("-preview_thumb_cache");
        if (!cacheOp.empty())
        {
            ENGINE_CHECK(cacheOp == "clear",
                         "-preview_thumb_cache='%s' is not understood; the only "
                         "value is 'clear'.", cacheOp.c_str());
            if (cacheOp == "clear") m_editorLayer.HarnessClearThumbnailCache();
        }
    }
    needValue("-preview_thumb_evict_ms", "-preview_thumb_evict_ms=<ms>");
    if (pcl.Has("-preview_thumb_evict_ms"))
        m_editorLayer.HarnessSetThumbnailEvictMs(
            static_cast<std::uint32_t>(pcl.GetInt("-preview_thumb_evict_ms", 8000)));

    needValue("-preview_churn_passes", "-preview_churn_passes=<N>");
    if (pcl.Has("-preview_churn_passes"))
        m_editorLayer.HarnessSetPreviewChurnPasses(
            static_cast<std::uint32_t>(pcl.GetInt("-preview_churn_passes", 1)));

    {
        needValue("-preview_churn", "-preview_churn=<dir>");
        const std::string churnDir = pcl.Get("-preview_churn");
        if (!churnDir.empty())
            ENGINE_CHECK(m_editorLayer.HarnessSetPreviewChurn(churnDir),
                         "-preview_churn='%s' found no .imat/.imatinst to churn.",
                         churnDir.c_str());
    }

    // ---- -preview_touch_at=<frame>:<rootRelativePath> ----
    // Proves the texture / shader-source invalidation and the watcher prefixes: at
    // editor frame <frame> it bumps the file's mtime and delivers the
    // dependency-changed event ITSELF, through the watcher-relative ->
    // root-relative normalisation the poll performs. The OS watcher's latency is
    // unbounded, so waiting for it would be flaky; delivering the SAME string keeps
    // the prefixing and the dep index honest and the frame deterministic. (The real
    // event arrives a frame or two later and finds nothing: the entries are already
    // evicted.) The touch must be real - a changed dependency must produce a NEW
    // tile filename - and the original mtime is restored at shutdown.
    {
        needValue("-preview_touch_at",
                  "-preview_touch_at=<frame>:<root-relative path>");
        const std::string touchStr = pcl.Get("-preview_touch_at");
        if (!touchStr.empty())
        {
            const std::size_t colon = touchStr.find(':');
            bool ok = false;
            if (colon != std::string::npos && colon > 0 &&
                colon + 1 < touchStr.size())
            {
                const std::uint64_t f = std::strtoull(
                    touchStr.substr(0, colon).c_str(), nullptr, 10);
                ok = m_editorLayer.HarnessSetPreviewTouchAt(
                         f, touchStr.substr(colon + 1));
            }
            ENGINE_CHECK(ok,
                         "-preview_touch_at='%s' is not <frame>:<path> with a "
                         "non-zero frame; nothing was armed.",
                         touchStr.c_str());
        }
    }
    // ---- -preview_camera=<entityName>[:<w>x<h>] ----
    // Opens the Camera Preview panel on a named scene camera; the panel retries the
    // name every frame until the scene has loaded. A bare flag is refused: a pack
    // that asked for a camera panel and got none would measure the no-panel case
    // under the camera pack's name.
    if (pcl.Has("-preview_camera"))
    {
        needValue("-preview_camera", "-preview_camera=<entityName>[:<w>x<h>]");
        const std::string spec = pcl.Get("-preview_camera");
        if (!spec.empty())
        {
            std::string name = spec;
            std::uint32_t cw = 0, ch = 0;
            const std::size_t colon = spec.find(':');
            if (colon != std::string::npos)
            {
                name = spec.substr(0, colon);
                const std::string dims = spec.substr(colon + 1);
                const std::size_t xpos = dims.find('x');
                ENGINE_CHECK(xpos != std::string::npos && xpos > 0 &&
                             xpos + 1 < dims.size(),
                             "-preview_camera size field '%s' is not <w>x<h>; "
                             "the panel will measure its own size, which is 0 "
                             "under -hidden.", dims.c_str());
                if (xpos != std::string::npos && xpos > 0 && xpos + 1 < dims.size())
                {
                    cw = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(0, xpos).c_str(), nullptr, 10));
                    ch = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(xpos + 1).c_str(), nullptr, 10));
                }
            }
            // PERCENT-DECODED, because entity names contain spaces
            // ("Main Camera") and a pack argument that relies on the
            // runner's shell quoting is one platform change from silently
            // matching nothing. The .iscene format encodes names the same
            // way, so %20 is the spelling a pack author already knows.
            name = Resource::AssetRefs::PercentDecode(name);
            // Same trap as -preview_pip: "-preview_camera=:640x360" has a non-empty
            // VALUE and an empty NAME, so the bare-flag check above passes and the panel
            // would open on nothing.
            if (name.empty())
            {
                LOG_ERROR("-preview_camera='%s' parses to an EMPTY entity "
                          "name. Refusing to run: the panel will never "
                          "resolve a camera and the pack would measure the "
                          "no-panel case under its own name.",
                          spec.c_str());
                PostQuitMessage(Harness::PreconditionFail);
            }
            LOG_INFO("-preview_camera='%s' at %ux%u; opening the Camera "
                     "Preview panel.", name.c_str(), cw, ch);
            m_editorLayer.HarnessOpenCameraPreview(name, cw, ch);
        }
    }
    // ---- -preview_asset_deadline=<panelFrames> ----
    // BEFORE -preview_asset, so the panel is opened with the deadline the pack
    // asked for rather than being retro-fitted with it. See
    // AssetPreviewWindow::HarnessSetDeadlineFrames for why raising it is not the
    // same as disarming it, and why 0 is refused.
    if (pcl.Has("-preview_asset_deadline"))
    {
        const std::uint32_t dl = static_cast<std::uint32_t>(
            pcl.GetInt("-preview_asset_deadline", 0));
        if (!m_editorLayer.HarnessSetAssetPreviewDeadline(dl))
        {
            LOG_ERROR("-preview_asset_deadline='%s' is not a panel-frame "
                      "count above zero. Refusing to run - a deadline of "
                      "zero would disarm the one check that stops an empty "
                      "asset panel from passing.",
                      pcl.Get("-preview_asset_deadline").c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -mesheditor_*: the Mesh Editor ----
    // The Mesh Editor reads its own flags (the subject, the pinned render size, the
    // view-mode / show / section / UV / orbit steps, the shots, the two reverse
    // arms); a bare or malformed value, or a step with no -mesheditor_open, is exit
    // 12 before a frame is drawn. The subject itself opens on the first editor
    // frame, in the panel's serial window.
    {
        std::string why;
        if (!Editor::MeshEditorWindow::Get().HarnessConfigure(why))
        {
            LOG_ERROR("Mesh Editor harness: %s. Refusing to run - a Mesh Editor pack with no "
                      "subject measures the no-panel case under the pack's name.", why.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -skeleton_editor*: the Skeleton Editor ----
    // The Skeleton Editor reads its own flags (the subject, the pinned render
    // size, the select / scope / show / orbit steps, the shots, the reverse arm); a
    // bare or malformed value, or a step with no -skeleton_editor, is exit 12
    // before a frame. The subject opens on the first editor frame.
    {
        std::string why;
        if (!Editor::SkeletonEditorWindow::Get().HarnessConfigure(why))
        {
            LOG_ERROR("Skeleton Editor harness: %s. Refusing to run - a Skeleton Editor pack with no "
                      "subject measures the no-panel case under the pack's name.", why.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -texviewer_*: the Texture Viewer ----
    // The Texture Viewer reads its own flags (the subject, the pinned output size,
    // the view / shot steps, the close, the three reverse arms); a bare or
    // malformed value, or a step with no -texviewer_open, is exit 12 before a
    // frame. The subject opens on the first editor frame.
    {
        std::string why;
        if (!Editor::TextureViewerWindow::Get().HarnessConfigure(why))
        {
            LOG_ERROR("Texture Viewer harness: %s. Refusing to run - a Texture Viewer pack with no "
                      "subject measures the no-panel case under the pack's name.", why.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -uieditor_*: the UI Editor ----
    // The UI Editor reads its own flags (the subject canvas by name, the pinned
    // view box, the res / zoom / pan / view / shot steps, the close, the parity
    // reverse arm); a bare or malformed value, or a step with no -uieditor_open, is
    // exit 12 before a frame. The subject opens once its canvas exists.
    {
        std::string why;
        if (!Editor::UIEditorWindow::Get().HarnessConfigure(why))
        {
            LOG_ERROR("UI Editor harness: %s. Refusing to run - a UI Editor pack with no "
                      "subject measures the no-panel case under the pack's name.", why.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -mateditor_*: the Material Editor ----
    {
        std::string why;
        if (!Editor::MaterialEditorWindow::Get().HarnessConfigure(why))
        {
            LOG_ERROR("Material Editor harness: %s. Refusing to run - a Material Editor pack with no "
                      "subject measures the no-panel case under the pack's name.", why.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -nodeeditor_smoke*: the node editor smoke test ----
    {
        std::string why;
        if (!Editor::NodeEditorSmoke::Get().HarnessConfigure(why))
        {
            LOG_ERROR("Node editor smoke: %s. Refusing to run.", why.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    if (pcl.Has("-preview_asset"))
    {
        // -preview_asset=<path>[:<px>]: opens the Asset Preview panel on a `.meshlib`
        // or a `.ipfb`. A HARD PRECONDITION, exit 12, the same shape -scene and -exec
        // keep: a bare flag, an empty path, an extension the panel does not accept or a
        // size outside the quantised set would leave the run going with no panel (or
        // with a panel at a size nobody asked for), and a pack asserting on the asset
        // lane would measure the NO-PANEL case under the asset pack's name. That is not
        // a degradation the engine can usefully continue past.
        const std::string spec = pcl.Get("-preview_asset");
        std::string   path = spec;
        std::uint32_t px   = 0;
        const char*   why  = nullptr;

        if (spec.empty())
            why = "a bare flag (-preview_asset=<path>[:<px>]) opens nothing";
        else
        {
            // SPLIT ON THE LAST COLON, and only when what follows is a pure
            // number. An asset path is project-relative here, but a caller
            // who pastes an absolute Windows path would otherwise have the
            // drive letter's colon eaten and be told the file does not
            // exist - a confusing failure for a typo the flag can simply
            // survive.
            const std::size_t colon = spec.find_last_of(':');
            if (colon != std::string::npos && colon + 1 < spec.size())
            {
                const std::string tail = spec.substr(colon + 1);
                const bool numeric = std::all_of(tail.begin(), tail.end(),
                    [](unsigned char c) { return c >= '0' && c <= '9'; });
                if (numeric)
                {
                    px   = static_cast<std::uint32_t>(std::strtoul(tail.c_str(),
                                                                   nullptr, 10));
                    path = spec.substr(0, colon);
                }
            }
            // PERCENT-DECODED for the same reason -preview_camera is: asset
            // paths contain spaces ("GirlsFrontline CheyanneDefault"), and a
            // pack argument that relies on the runner's shell quoting is one
            // platform change from silently naming nothing.
            path = Resource::AssetRefs::PercentDecode(path);
            if (path.empty())
                why = "it parses to an EMPTY path";
            else if (!m_editorLayer.HarnessOpenAssetPreview(path, px))
                why = "the panel refused it - the size field must be one of "
                      "the quantised 128/256/384/512 the panel can reach by "
                      "hand (any other exercises a recompile a user cannot "
                      "provoke), and the file must be a .meshlib or a .ipfb "
                      "(a .imsh has no runtime path to a mesh-descriptor "
                      "slot since the P1-P6 rewrite)";
        }

        if (why)
        {
            LOG_ERROR("-preview_asset='%s' cannot open the Asset Preview "
                      "panel: %s. Refusing to run - a preview pack with no "
                      "panel measures the no-panel case under the pack's "
                      "name.", spec.c_str(), why);
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
            LOG_INFO("-preview_asset='%s' at %u px; opening the Asset "
                     "Preview panel.", path.c_str(), px);
    }
    // ---- -chainpreview_* ----
    // Parsed and handed to the Chain Physics Preview panel by
    // Editor::ChainPreviewHarness::ApplyBootFlags. Called HERE because the order
    // of the refusals and their log lines is part of the packs' contract: after
    // -preview_asset, before -preview_asset_appear_at.
    Editor::ChainPreviewHarness::ApplyBootFlags(pcl);
    // ---- -preview_asset_appear_at=<frame>:<src>:<dst> ----
    // Copies a shipped asset onto a path that does NOT exist, at a chosen editor
    // frame, and removes it at shutdown. The only way to produce the case the retry
    // clock exists for: a preview pointed at a file that is missing NOW and present
    // LATER. Nothing is notified - the panel has to come back on its own clock,
    // which is the assertion.
    if (pcl.Has("-preview_asset_appear_at"))
    {
        const std::string spec = pcl.Get("-preview_asset_appear_at");
        bool ok = false;
        // Two colons, left to right: <frame>:<src>:<dst>. Both paths are
        // root-relative here (no drive letters), so a plain split is right.
        const std::size_t c1 = spec.find(':');
        const std::size_t c2 = (c1 == std::string::npos)
                                   ? std::string::npos
                                   : spec.find(':', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos &&
            c1 > 0 && c2 > c1 + 1 && c2 + 1 < spec.size())
        {
            const std::uint64_t f =
                std::strtoull(spec.substr(0, c1).c_str(), nullptr, 10);
            ok = m_editorLayer.HarnessSetPreviewAppearAt(
                     f, spec.substr(c1 + 1, c2 - c1 - 1),
                     spec.substr(c2 + 1));
        }
        if (!ok)
        {
            LOG_ERROR("-preview_asset_appear_at='%s' is not "
                      "<frame>:<src>:<dst> with a non-zero frame and two "
                      "different paths. Refusing to run - the recovery the "
                      "pack asserts would never happen.", spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_asset_rewrite_at=<frame>:<src>:<dst> ----
    // The CONTENT-CHANGE step. Same grammar as the appear above and deliberately a
    // SEPARATE flag: the appear creates a file that was missing (the retry clock's
    // case) and tells nobody, while this one overwrites a file that opens fine (the
    // WATCHER's case, the only route an in-place importer rewrite has) and is meant
    // to be paired with -preview_touch_at on the same path so the pack's own
    // arguments name the mechanism under test. The original bytes are restored at
    // shutdown, so a TRACKED fixture can be the destination.
    if (pcl.Has("-preview_asset_rewrite_at"))
    {
        const std::string spec = pcl.Get("-preview_asset_rewrite_at");
        bool ok = false;
        const std::size_t c1 = spec.find(':');
        const std::size_t c2 = (c1 == std::string::npos)
                                   ? std::string::npos
                                   : spec.find(':', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos &&
            c1 > 0 && c2 > c1 + 1 && c2 + 1 < spec.size())
        {
            const std::uint64_t f =
                std::strtoull(spec.substr(0, c1).c_str(), nullptr, 10);
            ok = m_editorLayer.HarnessSetPreviewRewriteAt(
                     f, spec.substr(c1 + 1, c2 - c1 - 1),
                     spec.substr(c2 + 1));
        }
        if (!ok)
        {
            LOG_ERROR("-preview_asset_rewrite_at='%s' is not "
                      "<frame>:<src>:<dst> with a non-zero frame and two "
                      "different paths. Refusing to run - the content change "
                      "the pack asserts would never happen.", spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_bake_restore_at=<frame>[:<meshlib>] ----
    // The collision baker's preview RESTORE (a GUI button) is the only route to
    // Renderer::OnWorldClear(idsPreserved=false, worldSurvives=true); this queues
    // the baker's own intents (Load, then Restore kBakeRestoreGap frames later) so a
    // pack executes the branch that keeps a bake from cancelling an armed
    // SetGamePiP. The library MUST EXIST: a failed MeshLibrary::Load leaves
    // g_previewBakedLib invalid, the restore takes its early-out, and the pack
    // would pass having run nothing.
    if (pcl.Has("-preview_bake_restore_at"))
    {
        const std::string spec = pcl.Get("-preview_bake_restore_at");
        const std::size_t c1   = spec.find(':');
        const std::uint64_t f  = std::strtoull(
            (c1 == std::string::npos ? spec : spec.substr(0, c1)).c_str(),
            nullptr, 10);
        std::string lib = (c1 == std::string::npos || c1 + 1 >= spec.size())
                        ? std::string() : spec.substr(c1 + 1);
        // The DEFAULT is the smallest tracked .meshlib in the tree, the
        // same one asset_preview_relib composes: one box, 1392 bytes. The
        // library's CONTENT is irrelevant here - it is loaded only so the
        // restore has something real to release.
        if (lib.empty()) lib = "asset/_preview_fixtures/preview_relib.meshlib";
        // Where MeshLibrary::Load would find it (a search root, the pak or
        // the shared content root), not the working directory.
        const bool exists = ::Resource::AssetFS::Get().Exists(lib);
        if (f == 0 || !exists ||
            !m_editorLayer.HarnessSetBakeRestoreAt(f, lib))
        {
            LOG_ERROR("-preview_bake_restore_at='%s' needs a non-zero editor "
                      "frame and an EXISTING .meshlib ('%s' %s). Refusing to "
                      "run: a failed load takes the restore's early-out, so "
                      "OnWorldClear(worldSurvives=true) would never be "
                      "reached and the pack would pass having tested "
                      "nothing.", spec.c_str(), lib.c_str(),
                      exists ? "exists" : "is missing");
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_asset_verify=<path>[,<path>...] ----
    // The ORDER SELF-CHECK. See EditorLayer::HarnessSetAssetVerify. A bare flag is
    // refused for the same reason every other -preview_* flag is: a pack whose
    // self-check silently did nothing is the false pass this harness exists to
    // remove.
    if (pcl.Has("-preview_asset_verify"))
    {
        const std::string spec = pcl.Get("-preview_asset_verify");
        if (spec.empty())
        {
            LOG_ERROR("-preview_asset_verify needs a value "
                      "(-preview_asset_verify=<path>[,<path>...]); a bare "
                      "flag verifies nothing and the pack would go green "
                      "having checked nothing.");
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
            m_editorLayer.HarnessSetAssetVerify(spec);
    }
    // ---- -preview_split_cams=<baseEntityName>:<n> ----
    // BEFORE -preview_split, so the cameras are armed before the panel that names
    // them is opened. Spawning is deferred to a frame with a live World; see
    // EditorLayer::HarnessSetSplitCameras for why the Editor packs cannot do this
    // with a Lua driver.
    if (pcl.Has("-preview_split_cams"))
    {
        const std::string spec = pcl.Get("-preview_split_cams");
        bool ok = false;
        const std::size_t colon = spec.find_last_of(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < spec.size())
        {
            const std::uint32_t n = static_cast<std::uint32_t>(
                std::strtoul(spec.substr(colon + 1).c_str(), nullptr, 10));
            ok = m_editorLayer.HarnessSetSplitCameras(
                     Resource::AssetRefs::PercentDecode(spec.substr(0, colon)), n);
        }
        if (!ok)
        {
            LOG_ERROR("-preview_split_cams='%s' is not "
                      "<baseEntityName>:<2..4> (n cameras at that entity's "
                      "position, yaws evenly spaced over 360 degrees - 360/n "
                      "apart). Refusing to run - the split pack would point "
                      "every cell at the same camera and could not tell n "
                      "views from one drawn n times.",
                      spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_split=<cam1>,<cam2>[,...]:<w>x<h>[:<tick>] ----
    // A HARD PRECONDITION, exit 12, exactly the shape -preview_asset keeps: a
    // refusal that carried on would let a pack asserting on the split lane measure
    // the no-panel case under the split pack's name.
    if (pcl.Has("-preview_split"))
    {
        const std::string spec = pcl.Get("-preview_split");
        const char* why = nullptr;
        std::vector<std::string> names;
        std::uint32_t sw = 0, sh = 0, tick = 1;

        if (spec.empty())
            why = "a bare flag (-preview_split=<cams>:<w>x<h>[:<tick>]) "
                  "opens nothing";
        else
        {
            // FIELDS LEFT TO RIGHT: names, then WxH, then the optional tick.
            // The name list comes first and is comma-separated, so the two
            // colons are unambiguous - unlike -preview_asset, whose value is
            // a PATH that may carry a drive letter.
            const std::size_t c1 = spec.find(':');
            if (c1 == std::string::npos || c1 == 0 || c1 + 1 >= spec.size())
                why = "it has no ':<w>x<h>' field";
            else
            {
                std::string dims = spec.substr(c1 + 1);
                const std::size_t c2 = dims.find(':');
                if (c2 != std::string::npos)
                {
                    tick = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(c2 + 1).c_str(), nullptr, 10));
                    dims = dims.substr(0, c2);
                }
                const std::size_t xpos = dims.find('x');
                if (xpos == std::string::npos || xpos == 0 ||
                    xpos + 1 >= dims.size())
                    why = "its size field is not <w>x<h>";
                else
                {
                    sw = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(0, xpos).c_str(), nullptr, 10));
                    sh = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(xpos + 1).c_str(), nullptr, 10));
                    // PERCENT-DECODED, because entity names contain spaces
                    // and a pack argument that relies on the runner's shell
                    // quoting is one platform change from naming nothing.
                    std::string list = spec.substr(0, c1);
                    std::size_t at = 0;
                    while (at <= list.size())
                    {
                        const std::size_t comma = list.find(',', at);
                        const std::string one = (comma == std::string::npos)
                            ? list.substr(at) : list.substr(at, comma - at);
                        if (!one.empty())
                            names.push_back(
                                Resource::AssetRefs::PercentDecode(one));
                        if (comma == std::string::npos) break;
                        at = comma + 1;
                    }
                    if (names.size() < 2 || names.size() > 4)
                        why = "it does not name 2..4 cameras (one cell is "
                              "the Camera Preview panel, and a fifth has no "
                              "HDR slot to live in)";
                    else if (!m_editorLayer.HarnessOpenSplitViewport(
                                 names, sw, sh, tick))
                        why = "the panel refused it - the size must be a "
                              "multiple of 16 inside 160..960 x 96..540 "
                              "(anything else exercises a recompile a user "
                              "cannot provoke by dragging) and the tick must "
                              "be one of 1 / 2 / 3 / 4 / 8, the set the "
                              "per-cell combo offers (M7-09: a tick the "
                              "combo cannot display is a run whose tick the "
                              "first click on that combo would change)";
                }
            }
        }

        if (why)
        {
            LOG_ERROR("-preview_split='%s' cannot open the Split Viewports "
                      "panel: %s. Refusing to run - a split pack with no "
                      "panel measures the no-panel case under the pack's "
                      "name.", spec.c_str(), why);
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
            LOG_INFO("-preview_split: %zu cell(s) at %ux%u tick %u; opening "
                     "the Split Viewports panel.", names.size(), sw, sh, tick);
    }
    // ---- -preview_split_hover=<cell> ----
    // AFTER -preview_split, because it names one of that flag's cells.
    if (pcl.Has("-preview_split_hover"))
    {
        const int cell = pcl.GetInt("-preview_split_hover", -1);
        if (cell < 0 ||
            !m_editorLayer.HarnessSetSplitHover(static_cast<std::uint32_t>(cell)))
        {
            // THE BOUND IS THE CELL COUNT -preview_split CONFIGURED, not
            // Preview::kMaxPreviews: an index past the open cells, or the flag on its own
            // (no -preview_split, no cells), would be dropped by the panel and the run
            // would measure the UNHOVERED cadence under the hover pack's name.
            LOG_ERROR("-preview_split_hover='%s' does not name a cell this "
                      "run opens. It must be 0..N-1 for the N cameras "
                      "-preview_split named, and -preview_split must be "
                      "given: without it there is no panel to hover. "
                      "Refusing to run - the pack would measure the "
                      "unhovered cadence under the hover pack's name.",
                      pcl.Get("-preview_split_hover").c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_split_layout_at=<f>:<l>[,<f>:<l>...] ----
    // AFTER -preview_split, for the same reason the hover flag is: it names that
    // panel.
    //
    // A COMMA-SEPARATED LIST, not a repeated flag: CommandLine is last-wins, so
    // `-preview_split_layout_at=120:0 -preview_split_layout_at=200:1` would
    // silently apply only the second one and a two-step pack would measure a
    // one-step run under its name.
    if (pcl.Has("-preview_split_layout_at"))
    {
        const std::string spec = pcl.Get("-preview_split_layout_at");
        bool ok = !spec.empty();
        std::size_t at = 0;
        while (ok && at <= spec.size())
        {
            const std::size_t comma = spec.find(',', at);
            const std::string one = (comma == std::string::npos)
                ? spec.substr(at) : spec.substr(at, comma - at);
            const std::size_t colon = one.find(':');
            if (one.empty() || colon == std::string::npos || colon == 0 ||
                colon + 1 >= one.size())
                ok = false;
            else
            {
                const std::uint64_t f = static_cast<std::uint64_t>(
                    std::strtoull(one.substr(0, colon).c_str(), nullptr, 10));
                const int lay = std::atoi(one.substr(colon + 1).c_str());
                ok = m_editorLayer.HarnessSetSplitLayoutAt(f, lay);
            }
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        if (!ok)
        {
            LOG_ERROR("-preview_split_layout_at='%s' was refused. It must "
                      "be one or more comma-separated <frame>:<layout> steps "
                      "with distinct frames above 0 and a layout of 0 (1x2) "
                      "or 1 (2x2), AND -preview_split must be "
                      "given - it names that panel, and there is nothing to "
                      "switch without it. Refusing to run: a pack asserting "
                      "on the shrink would measure the un-switched layout "
                      "under its name.", spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_split_resize_at=<f>:<w>x<h>[,<f>:<w>x<h>...] ----
    // AFTER -preview_split, which is the panel it moves.
    //
    // A COMMA-SEPARATED LIST, the shape of -preview_split_layout_at and for the
    // same reason: CommandLine is LAST-WINS, so repeating the flag installs only
    // the final step and a two-step pack would measure a one-step run under its
    // name.
    if (pcl.Has("-preview_split_resize_at"))
    {
        const std::string spec = pcl.Get("-preview_split_resize_at");
        bool ok = !spec.empty();
        std::size_t at = 0;
        while (ok && at <= spec.size())
        {
            const std::size_t comma = spec.find(',', at);
            const std::string one = (comma == std::string::npos)
                ? spec.substr(at) : spec.substr(at, comma - at);
            const std::size_t colon = one.find(':');
            if (one.empty() || colon == std::string::npos || colon == 0 ||
                colon + 1 >= one.size())
                ok = false;
            else
            {
                const std::uint64_t f = static_cast<std::uint64_t>(
                    std::strtoull(one.substr(0, colon).c_str(), nullptr, 10));
                const std::string dims = one.substr(colon + 1);
                const std::size_t xpos = dims.find('x');
                if (xpos == std::string::npos || xpos == 0 ||
                    xpos + 1 >= dims.size())
                    ok = false;
                else
                {
                    const std::uint32_t rw = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(0, xpos).c_str(), nullptr, 10));
                    const std::uint32_t rh = static_cast<std::uint32_t>(
                        std::strtoul(dims.substr(xpos + 1).c_str(), nullptr, 10));
                    ok = m_editorLayer.HarnessSetSplitResizeAt(f, rw, rh);
                }
            }
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        if (!ok)
        {
            LOG_ERROR("-preview_split_resize_at='%s' was refused. It must be "
                      "one or more comma-separated <frame>:<w>x<h> steps "
                      "with distinct frames above 0 and sizes the panel's "
                      "quantiser leaves unchanged (a multiple of 16 inside "
                      "160..960 x 96..540), AND -preview_split must "
                      "be given - it is what opens the panel and pins its "
                      "size. Refusing to run: the pack would measure the "
                      "UN-resized cells and its 'the lane moved' assertions "
                      "would describe the first size.", spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_match_size_at=<frame> ----
    // AFTER -preview_split, which is the panel whose button it presses.
    if (pcl.Has("-preview_match_size_at"))
    {
        const std::string spec = pcl.Get("-preview_match_size_at");
        const std::uint64_t f = static_cast<std::uint64_t>(
            std::strtoull(spec.c_str(), nullptr, 10));
        if (!m_editorLayer.HarnessSetSplitMatchSizeAt(f))
        {
            LOG_ERROR("-preview_match_size_at='%s' was refused. It must be a "
                      "PANEL frame above 0, AND -preview_split must be "
                      "given - it is what opens the panel that owns the "
                      "button. Refusing to run: a pack asserting that the "
                      "adopted size STICKS would measure a run in which "
                      "nothing was ever adopted.", spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    // ---- -preview_camera_resize=<frame>:<w>x<h> ----
    // AFTER -preview_camera, which is the panel it moves.
    if (pcl.Has("-preview_camera_resize"))
    {
        const std::string spec = pcl.Get("-preview_camera_resize");
        bool ok = false;
        const std::size_t colon = spec.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < spec.size())
        {
            const std::uint64_t f = static_cast<std::uint64_t>(
                std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10));
            const std::string dims = spec.substr(colon + 1);
            const std::size_t xpos = dims.find('x');
            if (xpos != std::string::npos && xpos > 0 && xpos + 1 < dims.size())
            {
                const std::uint32_t rw = static_cast<std::uint32_t>(
                    std::strtoul(dims.substr(0, xpos).c_str(), nullptr, 10));
                const std::uint32_t rh = static_cast<std::uint32_t>(
                    std::strtoul(dims.substr(xpos + 1).c_str(), nullptr, 10));
                ok = m_editorLayer.HarnessSetCameraPreviewResize(f, rw, rh);
            }
        }
        if (!ok)
        {
            LOG_ERROR("-preview_camera_resize='%s' was refused. It must "
                      "be <frame>:<w>x<h> with a frame above 0 and a non-zero "
                      "extent, AND -preview_camera must be given - it is what "
                      "opens the panel and pins its size, and without it the "
                      "panel measures its own instead. Refusing to run: the "
                      "pack would measure the UN-resized view and its 'the "
                      "lane moved' assertions would describe the first size.",
                      spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- -preview_camera_close_at=<frame> ----
    // AFTER -preview_camera, which is the panel it closes.
    if (pcl.Has("-preview_camera_close_at"))
    {
        const std::string spec = pcl.Get("-preview_camera_close_at");
        const std::uint64_t f = static_cast<std::uint64_t>(
            std::strtoull(spec.c_str(), nullptr, 10));
        if (!m_editorLayer.HarnessSetCameraPreviewCloseAt(f))
        {
            LOG_ERROR("-preview_camera_close_at='%s' was refused. It must "
                      "be a frame above 0, AND -preview_camera must be "
                      "given - it is what opens the panel, and there is "
                      "nothing to close without it. Refusing to run: a pack "
                      "that asserts on what the close RELEASES would "
                      "measure a lane nobody ever handed back.",
                      spec.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }

    // ---- -preview_close_at / -preview_open_at ----
    // LAST in this block, because a close takes away every panel the flags above
    // opened and the order in which they were opened does not matter to it. Comma
    // lists rather than repeated flags: CommandLine is last-wins, so
    // `-preview_close_at=120 -preview_close_at=320` would schedule ONE step and
    // silently drop the other (the same shape -preview_split_layout_at documents).
    {
        // Each step is <frame>[:<which>], with `which` one of
        // material | camera | asset | split | all (default all). The per-surface form
        // is what makes "tear down m_live while m_world stays compiled" and "drive the
        // split panel's close without the camera panel" expressible at all.
        auto parseSurfaces = [](const std::string& w,
                                std::uint8_t& out) -> bool
        {
            if (w == "all")      out = EditorLayer::kPreviewSurfaceAll;
            else if (w == "material") out = EditorLayer::kPreviewSurfaceMaterial;
            else if (w == "camera")   out = EditorLayer::kPreviewSurfaceCamera;
            else if (w == "asset")    out = EditorLayer::kPreviewSurfaceAsset;
            else if (w == "split")    out = EditorLayer::kPreviewSurfaceSplit;
            else return false;
            return true;
        };
        auto parseFrames = [&parseSurfaces](const std::string& spec,
            std::vector<std::pair<std::uint64_t, std::uint8_t>>& out) -> bool
        {
            std::size_t start = 0;
            while (start <= spec.size())
            {
                const std::size_t comma = spec.find(',', start);
                const std::string tok = spec.substr(
                    start, comma == std::string::npos ? std::string::npos
                                                      : comma - start);
                if (tok.empty()) return false;
                std::string num = tok;
                std::uint8_t mask = EditorLayer::kPreviewSurfaceAll;
                const std::size_t colon = tok.find(':');
                if (colon != std::string::npos)
                {
                    num = tok.substr(0, colon);
                    if (!parseSurfaces(tok.substr(colon + 1), mask))
                        return false;
                }
                char* end = nullptr;
                const unsigned long long v =
                    std::strtoull(num.c_str(), &end, 10);
                if (!end || *end != '\0' || v == 0ull) return false;
                out.emplace_back(static_cast<std::uint64_t>(v), mask);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            return !out.empty();
        };
        for (int pass = 0; pass < 2; ++pass)
        {
            const char* flag = pass ? "-preview_open_at" : "-preview_close_at";
            if (!pcl.Has(flag)) continue;
            const std::string spec = pcl.Get(flag);
            std::vector<std::pair<std::uint64_t, std::uint8_t>> steps;
            if (!parseFrames(spec, steps) ||
                !m_editorLayer.HarnessSetPreviewToggleFrames(steps,
                                                             pass != 0))
            {
                LOG_ERROR("%s='%s' was refused. It is a COMMA LIST of "
                          "<frame>[:<which>] steps - editor frames above 0, "
                          "`which` one of material|camera|asset|split|all "
                          "(default all) - at most 8 steps between the two "
                          "flags, and no frame may carry two toggles. "
                          "Refusing to run: a pack that asserts on what a "
                          "close RELEASES would otherwise measure a session "
                          "in which nothing ever closed.",
                          flag, spec.c_str());
                PostQuitMessage(Harness::PreconditionFail);
            }
        }
    }

    m_editorLayer.HarnessSetPreviewStartFrame(
        static_cast<std::uint64_t>(previewStartFrame > 0 ? previewStartFrame : 0));
    if (!previewMaterial.empty())
        LOG_INFO("Preview budget: open '%s', start requesting at frame %d "
                 "(size='%s' resize='%s' shape='%s')",
                 previewMaterial.c_str(), previewStartFrame,
                 previewSizeStr.empty()   ? "panel" : previewSizeStr.c_str(),
                 previewResizeStr.empty() ? "none"  : previewResizeStr.c_str(),
                 previewShapeStr.empty()  ? "sphere" : previewShapeStr.c_str());
}

void Editor::PreviewHarness::DeliverDue(const GameModeContext& ctx,
                                        const char* const (&kShaderWatchPrefix)[2],
                                        const char* const (&kAssetWatchPrefix)[2])
{
    // ---- -preview_touch_at, delivered HERE ----
    // Beside App's watcher poll rather than beside the flag, because that is the
    // one place that knows the watcher-relative <-> root-relative mapping. The path
    // travels through EXACTLY the transformation a real event takes: lowercase +
    // forward slashes + relative to the watched root (what FileWatcher::Normalize
    // produces), then back through the same kAssetWatchPrefix / kShaderWatchPrefix
    // table the poll uses. If the prefixes were wrong, this would report 0 tiles
    // invalidated - which is the assertion.
    {
        std::string touchRel;
        if (m_editorLayer.HarnessTakeDuePreviewTouch(touchRel))
        {
            // Park the lane first, as HotReload::WatchService::Poll does for an asset change.
            if (m_renderOwnerEnabled) drainFrameWork(Diag::DrainCause::Harness);
            // 1. A REAL mtime bump. The dependency has to genuinely
            //    change or the re-rendered tile hashes to the same
            //    filename and proves nothing about the content
            //    addressing. The original time is restored at shutdown.
            bool touchShared = false;
            const std::string full =
                HarnessWritablePath(touchRel, "-preview_touch_at", &touchShared);
            if (full.empty())
            {
                if (!touchShared)
                    LOG_ERROR("-preview_touch_at: '%s' does not resolve under "
                              "any asset search root; nothing was touched.",
                              touchRel.c_str());
            }
            else
            {
                std::error_code tec;
                m_previewTouchRestorePath = full;
                m_previewTouchRestoreTime =
                    std::filesystem::last_write_time(Sys::U8Path(full), tec);
                m_previewTouchRestoreValid = !tec;
                // Crash-safe first: the next boot puts the time back if this
                // run never reaches Shutdown().
                if (m_previewTouchRestoreValid)
                {
                    m_previewTouchSidecar = WriteTouchSidecar(full, m_previewTouchRestoreTime);
                    if (m_previewTouchSidecar.empty())
                        LOG_WARNING("-preview_touch_at: could not write the restore sidecar "
                                    "under '%s'; only the in-memory restore protects '%s' "
                                    "from an abnormal exit.",
                                    HarnessRewriteSidecarDir().c_str(), full.c_str());
                }
                std::filesystem::last_write_time(
                    Sys::U8Path(full),
                    std::filesystem::file_time_type::clock::now(), tec);

                // 2. Down to the watcher-relative form, then back up
                //    through the prefix table. FileWatcher::Normalize
                //    lowercases and forward-slashes; the root itself
                //    never appears in a ReadDirectoryChangesW record,
                //    so stripping it is what makes this the same string.
                std::string norm = touchRel;
                for (char& c : norm)
                {
                    if (c == '\\') c = '/';
                    else c = static_cast<char>(
                        ::tolower(static_cast<unsigned char>(c)));
                }
                std::string watcherRel = norm, delivered;
                auto tryRoot = [&](const char* prefix) -> bool
                {
                    std::string p = prefix;
                    for (char& c : p)
                        c = static_cast<char>(
                            ::tolower(static_cast<unsigned char>(c)));
                    if (norm.size() <= p.size() ||
                        norm.compare(0, p.size(), p) != 0) return false;
                    watcherRel = norm.substr(p.size());
                    delivered  = std::string(prefix) + watcherRel;
                    return true;
                };
                // Longest prefix first: "asset/Shaders/" before "asset/".
                if (!tryRoot(kShaderWatchPrefix[1]) &&
                    !tryRoot(kShaderWatchPrefix[0]) &&
                    !tryRoot(kAssetWatchPrefix[0])  &&
                    !tryRoot(kAssetWatchPrefix[1]))
                    delivered = norm;

                LOG_INFO("[PREVIEW] touch: '%s' -> watcher-relative '%s' "
                         "-> delivered '%s'", touchRel.c_str(),
                         watcherRel.c_str(), delivered.c_str());
                m_editorLayer.NotifyThumbnailDependencyChanged(
                    std::vector<std::string>{ delivered });
            }
        }
    }

    // ---- -preview_asset_appear_at, delivered HERE ----
    // The touch's opposite: it tells nobody. The preview must find the file on its
    // own retry clock (the assertion), so all that happens here is a file appearing.
    // The destination does not exist, so AssetFS cannot resolve it: its root is
    // taken from the source's resolved path (the tail swapped), so the copy lands in
    // the search root the pack named it under - a LOCAL one, since a source only the
    // shared content root has would put the copy in another checkout.
    {
        std::string appearSrc, appearDst;
        if (m_editorLayer.HarnessTakeDuePreviewAppear(appearSrc, appearDst))
        {
            if (m_renderOwnerEnabled) drainFrameWork(Diag::DrainCause::Harness);
            const std::string srcFull =
                HarnessWritablePath(appearSrc, "-preview_asset_appear_at");
            std::string dstFull;
            if (!srcFull.empty() && srcFull.size() >= appearSrc.size())
            {
                // srcFull ends with appearSrc (modulo separators); the
                // prefix that remains is the root.
                std::string a = srcFull, b = appearSrc;
                for (char& c : a) if (c == '\\') c = '/';
                for (char& c : b) if (c == '\\') c = '/';
                if (a.size() >= b.size() &&
                    a.compare(a.size() - b.size(), b.size(), b) == 0)
                    dstFull = a.substr(0, a.size() - b.size()) + appearDst;
            }
            std::error_code aec;
            if (dstFull.empty())
            {
                LOG_ERROR("-preview_asset_appear_at: source '%s' does not "
                          "resolve under any asset search root; nothing "
                          "was created.", appearSrc.c_str());
            }
            else if (std::filesystem::exists(Sys::U8Path(dstFull), aec))
            {
                // REFUSED, not overwritten. The pack's premise is that
                // the destination is MISSING; a file already there means
                // a previous run died before its cleanup, and copying
                // over it would make this run measure the always-present
                // case while looking identical in the log.
                LOG_ERROR("-preview_asset_appear_at: destination '%s' "
                          "already exists. The preview was supposed to "
                          "start with it MISSING, so this run would "
                          "measure the wrong case; nothing was copied.",
                          dstFull.c_str());
            }
            else
            {
                std::filesystem::copy_file(Sys::U8Path(srcFull),
                                           Sys::U8Path(dstFull), aec);
                if (aec)
                    LOG_ERROR("-preview_asset_appear_at: copy '%s' -> '%s' "
                              "FAILED (%s)", srcFull.c_str(),
                              dstFull.c_str(), aec.message().c_str());
                else
                {
                    m_previewAppearRemovePath = dstFull;
                    LOG_INFO("[PREVIEW] asset appear at editor frame %llu: "
                             "created '%s' from '%s' - NOTHING was "
                             "notified; the panel must find it on its own "
                             "retry clock",
                             static_cast<unsigned long long>(ctx.frame),
                             appearDst.c_str(), appearSrc.c_str());
                }
            }
        }
    }

    // ---- -preview_asset_rewrite_at, delivered HERE ----
    // The IN-PLACE content change. Both paths EXIST, so both resolve through AssetFS
    // directly - no root derivation, which would let a typo'd destination create a
    // file instead of failing. The original bytes are armed for restore before the
    // copy: the destination is a TRACKED fixture.
    {
        std::string rwSrc, rwDst;
        if (m_editorLayer.HarnessTakeDuePreviewRewrite(rwSrc, rwDst))
        {
            if (m_renderOwnerEnabled) drainFrameWork(Diag::DrainCause::Harness);
            // The source is only read; the destination is written.
            const std::string srcFull =
                ::Resource::AssetFS::Get().ResolveOnDisk(rwSrc);
            const std::string dstFull =
                HarnessWritablePath(rwDst, "-preview_asset_rewrite_at");
            std::error_code rec;
            if (srcFull.empty() || dstFull.empty())
            {
                LOG_ERROR("-preview_asset_rewrite_at: source '%s' (%s) or "
                          "destination '%s' (%s) does not resolve on disk. "
                          "The destination must ALREADY EXIST - this step "
                          "changes content, it does not create a file.",
                          rwSrc.c_str(), srcFull.empty() ? "missing" : "ok",
                          rwDst.c_str(), dstFull.empty() ? "missing" : "ok");
            }
            else
            {
                std::ifstream in(Sys::U8Path(dstFull), std::ios::binary);
                std::vector<std::uint8_t> original(
                    (std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
                in.close();
                std::ifstream srcIn(Sys::U8Path(srcFull), std::ios::binary);
                const std::vector<std::uint8_t> incoming(
                    (std::istreambuf_iterator<char>(srcIn)),
                     std::istreambuf_iterator<char>());
                srcIn.close();
                if (original.empty())
                {
                    LOG_ERROR("-preview_asset_rewrite_at: could not read the "
                              "original bytes of '%s'; refusing to overwrite "
                              "a file this run could not put back.",
                              dstFull.c_str());
                }
                else if (!incoming.empty() && original == incoming)
                {
                    // REFUSED, not performed - the appear step's guard, in the shape this step
                    // needs it. The premise is that the destination holds DIFFERENT bytes and the
                    // rewrite CHANGES it; a destination that already holds the source's bytes
                    // means a previous run died before its restore, and rewriting it would measure
                    // the always-rewritten case while looking identical in the log - and then
                    // "restore" would pin the wrong content into the tracked fixture for good.
                    // Hard exit rather than a warning: everything after this point in the pack is
                    // about a change that is not going to happen.
                    LOG_ERROR("-preview_asset_rewrite_at: destination '%s' "
                              "ALREADY holds the %zu bytes of source '%s'. "
                              "The rewrite was supposed to CHANGE it, so "
                              "this run would measure the wrong case; "
                              "nothing was copied. A previous run almost "
                              "certainly died before its restore - check "
                              "%s and `git status`.",
                              dstFull.c_str(), incoming.size(),
                              srcFull.c_str(),
                              HarnessRewriteSidecarDir().c_str());
                    PostQuitMessage(Harness::PreconditionFail);
                }
                else
                {
                    // ---- the sidecar, written before the overwrite ----
                    // Order is the whole point: a crash between this and the copy leaves a sidecar
                    // for a file that was never changed, and the next boot writes the SAME bytes
                    // back - a no-op. The other order leaves a changed file with no record of what
                    // it was. ArmHarnessFileRestore (shared with -preview_asset_edit) reads the
                    // file itself; `original` is read above only for the two guards before this
                    // branch ("could not read the original" and "already holds the source bytes").
                    ArmHarnessFileRestore(dstFull,
                                          "-preview_asset_rewrite_at",
                                          "asset rewrite");

                    std::filesystem::copy_file(
                        Sys::U8Path(srcFull), Sys::U8Path(dstFull),
                        std::filesystem::copy_options::overwrite_existing, rec);
                    if (rec)
                        LOG_ERROR("-preview_asset_rewrite_at: copy '%s' -> "
                                  "'%s' FAILED (%s)", srcFull.c_str(),
                                  dstFull.c_str(), rec.message().c_str());
                    else
                        LOG_INFO("[PREVIEW] asset rewrite at editor frame "
                                 "%llu: '%s' now holds the bytes of '%s' "
                                 "(%zu B original held for restore) - the "
                                 "file OPENS FINE either way, so only a "
                                 "watcher event can invalidate it",
                                 static_cast<unsigned long long>(ctx.frame),
                                 rwDst.c_str(), rwSrc.c_str(),
                                 original.size());
                }
            }
        }
    }
}

void Editor::PreviewHarness::PinParityCamera(Renderer& renderer) const
{
    // -preview_parity: pin the main camera to the studio rig. Called LAST, so it
    // wins over the VCam stack and the ortho override. NOT a camera entity: a
    // CameraComponent goes through CameraResolveSystem, the VCam blend stack and
    // LiveCameraComponent, any of which can add a frame of lag, a blend or an
    // aspect-driven FOV change that would show up as a shading difference in the
    // A/B image. This is the eye and forward the preview builds, from the same
    // Preview::StudioCameraBasis call. The orbit ANGLES are the rig's defaults (a
    // harness run never drags the panel); the DISTANCE is kParityDistanceScale,
    // which both sides read.
    if (m_previewParityPx)
    {
        float eye[3], fwd[3];
        // The SAME zoom the widget was pinned to - one constant, read
        // twice, never typed twice (PreviewStudioRig::kParityDistanceScale).
        Preview::StudioCameraBasis(0.f, 0.f, Preview::kParityDistanceScale,
                                   eye, fwd);
        RenderCamera rc;
        rc.fov      = Preview::kStudioCamera.fovY;
        rc.nearZ    = Preview::kStudioCamera.nearZ;
        rc.farZ     = Preview::kStudioCamera.farZ;
        // ---- the secondary-view arm takes the other constant ----
        // -preview_pip_parity pins this camera to the SAME table the PiP's camera
        // ENTITY was spawned from, so the two pipelines see the same eye, the same
        // forward and the same lens. Still from the CONSTANT rather than from that
        // entity, for the reason the studio pin states above: the entity goes through
        // CameraResolveSystem and the VCam stack, either of which can add a frame of
        // lag - and a frame of lag in a world with six lights reads as a shading
        // difference.
        if (m_previewPipParity)
        {
            const Preview::LightsParityCamera& lc = Preview::kLightsParityCamera;
            eye[0] = lc.eye[0]; eye[1] = lc.eye[1]; eye[2] = lc.eye[2];
            fwd[0] = lc.forward[0]; fwd[1] = lc.forward[1]; fwd[2] = lc.forward[2];
            rc.fov   = lc.fovY;
            rc.nearZ = lc.nearZ;
            rc.farZ  = lc.farZ;
        }
        rc.position = { eye[0], eye[1], eye[2] };
        rc.forward  = { fwd[0], fwd[1], fwd[2] };
        rc.up       = { 0.f, 1.f, 0.f };
        // The camera never moves, so history is valid from the second frame
        // on; saying otherwise would re-arm a temporal cut every frame.
        rc.historyValid = true;
        renderer.SetCamera(rc);
    }
}

void Editor::PreviewHarness::AfterFrame()
{
    // Opened once, after boot. Not during Init: OpenMaterialAsset loads the
    // .imat through the resource layer and clears the entity selection, and
    // doing that before the first frame has settled would race the asset
    // scan that resolves the path.
    // Under -renderowner these run while the lane may be recording: park it
    // first on the one frame one of them fires (each builds world or
    // editor state the render half reads).
    if (m_renderOwnerEnabled &&
        ((!previewOpenSent && !previewMaterial.empty()) ||
         (!previewParitySent && !previewParityFixture.empty()) ||
         (!previewLightsParitySent && !previewLightsParityArm.empty())))
        drainFrameWork(Diag::DrainCause::Harness);
    if (!previewOpenSent && !previewMaterial.empty())
    {
        previewOpenSent = true;
        LOG_INFO("-preview_material='%s'; opening the material asset inspector.",
                 previewMaterial.c_str());
        m_editorLayer.HarnessOpenMaterialAsset(previewMaterial);
    }
    if (!previewParitySent && !previewParityFixture.empty())
    {
        previewParitySent = true;
        LOG_INFO("-preview_parity='%s'; building the A/B fixture world.",
                 previewParityFixture.c_str());
        ENGINE_CHECK(m_editorLayer.HarnessSetPreviewParity(previewParityFixture),
                     "-preview_parity='%s' is not a known fixture; the world "
                     "was left as loaded and the comparison would be "
                     "meaningless.", previewParityFixture.c_str());
    }
    // -preview_lights_parity: the secondary-view lights fixture, same point, same
    // reason.
    if (!previewLightsParitySent && !previewLightsParityArm.empty())
    {
        previewLightsParitySent = true;
        LOG_INFO("-preview_lights_parity='%s'; building the secondary-view "
                 "lights fixture world.", previewLightsParityArm.c_str());
        ENGINE_CHECK(m_editorLayer.HarnessSetLightsParity(previewLightsParityArm),
                     "-preview_lights_parity='%s' is not a known arm; the "
                     "world was left as loaded and the comparison would be "
                     "meaningless.", previewLightsParityArm.c_str());
    }
}

void Editor::PreviewHarness::Shutdown()
{
    // -preview_split_cams SPAWNED FIXTURE ENTITIES, so the cleanup is a destroy.
    // Called while the World is whole (after ReleaseThumbnailTiles, before
    // GameFlow::ShutdownRuntime). Unconditional and cheap when the flag was never
    // passed (the tracked list is empty).
    m_editorLayer.HarnessReleaseSplitCameras();
    // -preview_touch_at PUT THE FILE'S ORIGINAL mtime BACK. The touch has to be
    // real (a dependency that did not change hashes to the same tile filename
    // and proves nothing), but a scenario run leaving a tracked asset with a
    // modified write time would silently invalidate every OTHER mtime-keyed
    // cache in the tree on the next build.
    if (!m_previewTouchRestorePath.empty() && m_previewTouchRestoreValid)
    {
        std::error_code rec;
        std::filesystem::last_write_time(Sys::U8Path(m_previewTouchRestorePath),
                                         m_previewTouchRestoreTime, rec);
        LOG_INFO("[PREVIEW] touch: restored the original mtime of '%s'%s",
                 m_previewTouchRestorePath.c_str(),
                 rec ? " FAILED" : "");
        // Put back: the next boot must not apply the sidecar a second time.
        // Kept when the restore failed, so the next boot tries again.
        if (!rec && !m_previewTouchSidecar.empty())
        {
            std::error_code sec;
            std::filesystem::remove(Sys::U8Path(m_previewTouchSidecar), sec);
        }
    }
    // -preview_asset_appear_at CREATED the file, so the cleanup is a delete.
    // Unconditional (not "if the run passed"): the whole point of the pack is
    // that the path is MISSING at boot, and a leftover copy would silently turn
    // the next run into the always-present case.
    if (!m_previewAppearRemovePath.empty())
    {
        std::error_code rec;
        const bool gone =
            std::filesystem::remove(Sys::U8Path(m_previewAppearRemovePath), rec);
        LOG_INFO("[PREVIEW] asset appear: removed the copy at '%s'%s",
                 m_previewAppearRemovePath.c_str(),
                 (gone && !rec) ? "" : " FAILED");
    }
    // The armed file restores (-preview_asset_rewrite_at, -preview_asset_edit),
    // in arm order: each OVERWROTE a file in the asset tree, so the cleanup is a
    // write-back of the bytes read before the first write, plus the original mtime.
    // Unconditional, for the appear's reason: a leftover rewrite would make the
    // next run start from the post-change content and measure nothing.
    for (const HarnessFileRestore& r : m_harnessFileRestores)
    {
        if (r.path.empty() || r.bytes.empty()) continue;
        bool ok = false;
        {
            std::ofstream out(Sys::U8Path(r.path),
                              std::ios::binary | std::ios::trunc);
            if (out)
            {
                out.write(reinterpret_cast<const char*>(r.bytes.data()),
                          static_cast<std::streamsize>(r.bytes.size()));
                // CLOSED BEFORE `ok` IS READ: a write that only fails when the buffer is
                // FLUSHED - a full disk, a read-only file, a network share going away - would
                // otherwise report success, and the log would say the tracked fixture had been
                // put back when it had not.
                out.close();
                ok = !out.fail();
            }
        }
        if (ok && r.timeValid)
        {
            std::error_code rec;
            std::filesystem::last_write_time(Sys::U8Path(r.path), r.time, rec);
        }
        // THE SIDECAR GOES ONLY WHEN THE RESTORE WORKED. Kept on failure, so
        // the next boot finishes the job this shutdown could not.
        if (ok && !r.sidecar.empty())
        {
            std::error_code rec;
            std::filesystem::remove(Sys::U8Path(r.sidecar), rec);
        }
        // THE WORDING IS PER FLAG, because asset_preview_relib and
        // asset_preview_empty pin this sentence verbatim (and the FAILED
        // variant of it as a forbidden literal).
        LOG_INFO("[PREVIEW] %s: restored the original %zu bytes of '%s'%s",
                 r.tag.c_str(), r.bytes.size(), r.path.c_str(),
                 ok ? "" : " FAILED");
    }
}

#endif // WITH_HARNESS
