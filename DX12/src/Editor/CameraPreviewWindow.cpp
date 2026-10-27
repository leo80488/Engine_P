// Camera Preview panel: a live WorldCull preview of a scene camera. The
// header holds the contract and the UI invariants.

#include "Editor/CameraPreviewWindow.h"

#include "Editor/PreviewFrozenSubmit.h"  // SubmitFrozenImage
#include "ECS/CameraStackComponents.h"   // VirtualCameraComponent
#include "ECS/Components.h"              // CameraComponent
#include "ECS/ECS.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"
#include "System/CommandLine.h"          // -panelvisibleverify (WITH_HARNESS oracle)
#include "System/Log.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <iterator>
#include <cstdio>

namespace Editor
{
namespace
{
    // 16-pixel steps of WIDTH, 16:9 height: a scene view is not square, so the
    // material inspector's square size table does not apply. Sizes are quantised
    // and the commit debounced because a resize recompiles the graph (seven
    // physical textures destroyed and recreated).
    constexpr double kResizeDebounceSeconds = 0.2;
    constexpr std::uint32_t kMinW = 192, kMaxW = 960;

    // Every entity that can serve as a preview camera, with its display name.
    // Both pools, because a VCam rig carries VirtualCameraComponent and need not
    // carry CameraComponent.
    struct CamRow { Entity e; std::string label; };

    std::uint32_t QuantiseWidth(float avail)
    {
        std::uint32_t w = static_cast<std::uint32_t>(std::max(avail, 0.f));
        w = (w / 16u) * 16u;
        return std::clamp(w, kMinW, kMaxW);
    }
    // The framings the panel offers: an aspect fitted to the panel's width, or a fixed render size
    // (what the game renders at) shown scaled into the panel.
    struct Frame
    {
        const char*   label;
        std::uint32_t aspectW, aspectH;   // a fitted aspect
        std::uint32_t fixedW, fixedH;     // a fixed size (0 = fitted)
    };
    constexpr Frame kFrames[] = {
        { "16:9",        16, 9, 0, 0 },
        { "4:3",         4, 3, 0, 0 },
        { "21:9",        21, 9, 0, 0 },
        { "1:1",         1, 1, 0, 0 },
        { "1280 x 720",  16, 9, 1280, 720 },
        { "1920 x 1080", 16, 9, 1920, 1080 },
    };

    std::uint32_t HeightFor(std::uint32_t w, const Frame& f = kFrames[0])
    {
        // Rounded to an even number: an odd height is legal but makes every
        // half-resolution debug view off by half a texel for no benefit.
        std::uint32_t h = (w * f.aspectH) / f.aspectW;
        return (h < 2u) ? 2u : (h & ~1u);
    }

    void GatherCameras(World& world, std::vector<CamRow>& out)
    {
        out.clear();
        auto add = [&](Entity e, const char* kind)
        {
            for (const CamRow& r : out) if (r.e == e) return;
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s  [%s #%u]",
                          world.GetName(e).c_str(), kind,
                          static_cast<unsigned>(e));
            out.push_back({ e, buf });
        };
        world.ForEach<CameraComponent>(
            [&](Entity e, CameraComponent&) { add(e, "Camera"); });
        world.ForEach<VirtualCameraComponent>(
            [&](Entity e, VirtualCameraComponent&) { add(e, "VCam"); });
    }
}

CameraPreviewWindow& CameraPreviewWindow::Get()
{
    static CameraPreviewWindow s_instance;
    return s_instance;
}

void CameraPreviewWindow::Draw(Renderer* renderer, World* world, bool* open,
                               Entity editorSelection)
{
    ImGui::SetNextWindowSize(ImVec2(520.f, 400.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Camera Preview", open))
    {
        // Collapsed is not visible: m_visibleThisFrame stays false, so the next
        // ProcessPendingActions hands the view back at the frame boundary and a folded
        // tab holds no HDR slot. The panel resets its own per-frame state rather than
        // latching it.
        // A DOCKED window whose dockspace is skipped or degenerate also lands here
        // (Begin returns false), and -hidden minimises the window to a ~32x29
        // dockspace. The layout policy in EditorLayer::OnAttach keeps -unattended runs
        // out of that state; this counter makes it diagnosable when it happens.
        ++m_beginRefusedFrames;
        ImGui::End();
        return;
    }

    if (!renderer || !world)
    {
        ImGui::TextDisabled("No renderer / world attached.");
        ImGui::End();
        return;
    }
    if (!renderer->IsPreviewEnabled())
    {
        // The preview kill switch, reported rather than shown as a blank box.
        ImGui::TextDisabled("Preview disabled (DX12_PREVIEW=0 or "
                            "editor_prefs preview.enabled=0)");
        ImGui::End();
        return;
    }

    // ---- -preview_camera_close_at=<frame> ----
    // A panel close a pack can trigger: under -hidden there is no title-bar X and
    // the menu is unreachable. preview_split_recover needs it: the WorldCull lane
    // keeps its size until the LAST camera view goes, so closing one holder makes a
    // refused size reachable.
    // m_visibleThisFrame is deliberately left false and the release queued here:
    // clearing `open` stops the draw only from the next frame, so the auto-release
    // path would cost a frame and tie the release frame to panel draw order.
    if (m_harnessCloseFrame != 0 && m_frameTick >= m_harnessCloseFrame)
    {
        if (!m_harnessClosedLogged)
        {
            m_harnessClosedLogged = true;
            LOG_INFO("[PREVIEW] camera panel closed by the harness at panel "
                     "frame %llu - view %u is handed back at the next frame "
                     "boundary, and the WorldCull lane's size with it if this "
                     "was the last camera view",
                     static_cast<unsigned long long>(m_frameTick), m_viewId);
        }
        if (m_viewId != ~0u) m_releaseQueued = true;
        if (open) *open = false;
        ImGui::End();
        return;
    }

    m_visibleThisFrame = true;

    // ---- A world reload invalidates the selection ----
    // Entity is a bare uint32 id with no generation (World::IsAlive is a bit
    // test), so a selection carried across a Load or a Play->Stop restore would
    // silently follow whatever entity now holds that id. World::Epoch() is bumped
    // by Clear and AdoptState - exactly the events that recycle ids - so comparing
    // it needs no hook in the places that reload a world.
    // The harness resolve is re-armed too: the flag names a camera by NAME, and
    // the name is resolved again in the new world.
    const std::uint32_t epoch = world->Epoch();
    if (epoch != m_worldEpoch)
    {
        if (m_worldEpochSeen && (m_selected != NullEntity || m_harnessResolved))
            LOG_INFO("[PREVIEW] camera panel: world epoch %u -> %u, dropping the "
                     "camera selection (entity ids are recycled across a reload)",
                     m_worldEpoch, epoch);
        m_worldEpoch      = epoch;
        m_worldEpochSeen  = true;
        m_selected        = NullEntity;
        m_harnessResolved = false;
        m_harnessTries    = 0;
    }

    // ---- The camera combo ----
    std::vector<CamRow> cams;
    GatherCameras(*world, cams);

    // The harness's one-shot selection, retried until the scene has loaded.
    if (!m_harnessResolved && !m_harnessName.empty())
    {
        for (const CamRow& r : cams)
            if (world->GetName(r.e) == m_harnessName)
            {
                m_selected = r.e;
                m_harnessResolved = true;
                // Epoch-tagged, and says whether this is the same entity as the first
                // resolve. `resolved to entity=` keeps the substring packs match. `same` is 1
                // by construction on the first resolve; packs assert it on a line with
                // resolves >= 2 (were ids preserved across the reload).
                if (m_resolveCount == 0) m_firstResolved = r.e;
                ++m_resolveCount;
                LOG_INFO("[PREVIEW] -preview_camera='%s' resolved to entity=%u "
                         "epoch=%u resolves=%u same=%u",
                         m_harnessName.c_str(), static_cast<unsigned>(r.e),
                         static_cast<unsigned>(epoch),
                         static_cast<unsigned>(m_resolveCount),
                         (r.e == m_firstResolved) ? 1u : 0u);
                break;
            }
        // Retrying forever in silence is a false pass: a name that never matches
        // leaves the panel on "(none)", world-cull log assertions then fail looking
        // like a renderer bug, and a pack without them goes green having shown
        // nothing. Counted in panel draws (once per frame); 120 is ~2 s at 60 Hz, and
        // every preview pack runs 200+ frames.
        if (!m_harnessResolved && ++m_harnessTries == 120)
            ENGINE_CHECK_ONCE(false,
                "-preview_camera='%s' never resolved to a scene camera in 120 "
                "panel frames. The Camera Preview panel is showing nothing and "
                "this run is measuring the no-panel case. Check the entity name "
                "(percent-encode spaces) and that the scene contains it.",
                m_harnessName.c_str());
    }

    if (m_syncToSelection && editorSelection != NullEntity)
        for (const CamRow& r : cams)
            if (r.e == editorSelection) { m_selected = editorSelection; break; }

    // A selection that no longer exists (the entity was deleted, the scene
    // reloaded) is dropped rather than carried: ExtractPreviewCameras would
    // report it invalid every frame and the panel would sit on a stale picture.
    if (m_selected != NullEntity && !world->IsAlive(m_selected))
        m_selected = NullEntity;

    const char* preview = "(none)";
    for (const CamRow& r : cams) if (r.e == m_selected) preview = r.label.c_str();
    if (ImGui::BeginCombo("Camera", preview))
    {
        if (ImGui::Selectable("(none)", m_selected == NullEntity))
            m_selected = NullEntity;
        for (const CamRow& r : cams)
            if (ImGui::Selectable(r.label.c_str(), r.e == m_selected))
                m_selected = r.e;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Sync to selection", &m_syncToSelection);

    ImGui::SetNextItemWidth(140.f);
    if (ImGui::SliderInt("Tick", &m_tickInterval, 1, 8, "1 / %d frames"))
        m_tickInterval = std::clamp(m_tickInterval, 1, 8);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Render this view at most once every N frames. The "
                          "scheduler round-robins camera views, so raising this "
                          "is how an unfocused panel gets out of the way.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    if (ImGui::BeginCombo("Frame", kFrames[m_frame].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(kFrames)); ++i)
            if (ImGui::Selectable(kFrames[i].label, i == m_frame)) { m_frame = i; m_measuredW = 0; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An aspect fitted to the panel, or a fixed render size (what the game renders at) "
                          "shown scaled into it.");

    if (cams.empty())
        ImGui::TextDisabled("This scene has no CameraComponent or "
                            "VirtualCameraComponent entity.");

    // ---- Size: quantise, then debounce (invariant 3) ----
    if (m_harnessW != 0 && m_harnessH != 0)
    {
        // PINNED, bypassing the debounce as well as the measurement: a debounce
        // makes the commit frame a function of wall-clock speed, and a pack has to
        // assert on a frame number.
        // -preview_camera_resize moves the pin at a chosen panel frame: a drag-resize
        // without the wall clock - the same commit, the same lane re-fix, the same
        // single graph recompile.
        std::uint32_t wantW = m_harnessW, wantH = m_harnessH;
        if (m_harnessResizeFrame != 0 && m_harnessResizeW != 0 &&
            m_harnessResizeH != 0 && m_frameTick >= m_harnessResizeFrame)
        { wantW = m_harnessResizeW; wantH = m_harnessResizeH; }
        if (m_sizeW != wantW || m_sizeH != wantH)
            LOG_INFO("[PREVIEW] camera panel harness size -> %ux%u (panel frame "
                     "%llu)", wantW, wantH,
                     static_cast<unsigned long long>(m_frameTick));
        m_sizeW = m_pendW = wantW;
        m_sizeH = m_pendH = wantH;
    }
    else
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const Frame& frame = kFrames[m_frame];
        const std::uint32_t wantW = frame.fixedW ? frame.fixedW : QuantiseWidth(avail > 0.f ? avail : 480.f);
        const std::uint32_t wantH = frame.fixedH ? frame.fixedH : HeightFor(wantW, frame);
        const double now = ImGui::GetTime();
        // Re-arm off the measured width, not the pending one (see m_measuredW): a
        // rectangle that has not moved re-triggers nothing.
        if (wantW != m_measuredW)
        { m_measuredW = wantW; m_pendW = wantW; m_pendH = wantH; m_pendAt = now; }
        else if (m_pendW != m_sizeW && (now - m_pendAt) >= kResizeDebounceSeconds)
        { m_sizeW = m_pendW; m_sizeH = m_pendH; }
    }

    if (m_selected == NullEntity)
    {
        // No camera, no view: the slot is handed back rather than held on a
        // picture nothing updates.
        if (m_viewId != ~0u) m_releaseQueued = true;
        ImGui::TextDisabled("Select a camera to preview.");
        ImGui::End();
        return;
    }

    if (m_releaseQueued)
    {
        ImGui::TextDisabled("Camera preview: switching...");
        ImGui::End();
        return;
    }

    // ---- Acquire: no size pre-check ----
    // A request is judged once, in RequestPreview. A pre-check here would refuse a
    // plain drag-resize while this panel is the only camera view (the lane is
    // still fixed at the old size). Moving the lane is the explicit
    // RequestPreviewWorldCullLaneSize call below: this panel passes its one view
    // id, the split panel all of its. A real refusal (a second panel open at
    // another size) is reported on the live view by IsPreviewViewSizeRefused.

    if (m_viewId == ~0u)
    {
        m_viewId = renderer->AcquirePreviewView(/*worldCull=*/true);
        if (m_viewId == ~0u)
        {
            ImGui::TextDisabled("Camera preview unavailable (no free view - at "
                                "most %u live preview views exist).",
                                Preview::kMaxPreviews);
            ImGui::End();
            return;
        }
    }

    // The entity is pushed every frame: ExtractPreviewCameras reads this one
    // store on the game half, and latching it on change only would leave the
    // resolver on a deleted entity across a reload that reused the id.
    renderer->SetPreviewCameraEntity(m_viewId, m_selected);

    // ---- Lane move ----
    // Asked while the committed size differs from the size the lane last
    // accepted. One view id: this panel owns the whole lane whenever it is the
    // only camera view open, which is the drag-resize case. With a split cell also
    // live at another size the move is refused and IsPreviewViewSizeRefused
    // reports it below.
    if (m_sizeW != m_laneAskedW || m_sizeH != m_laneAskedH)
    {
        const std::uint32_t one = m_viewId;
        // The latch moves on success only, so a refused move does not consume the
        // edge: the lane's size is handed back when the LAST camera view goes, and the
        // wanted size becomes reachable the moment the other holder closes.
        // Re-asking every frame while refused is cheap: the call early-outs on a size
        // already fixed, and the engine logs the refusal once.
        if (renderer->RequestPreviewWorldCullLaneSize(m_sizeW, m_sizeH, &one, 1u))
        {
            m_laneAskedW = m_sizeW;
            m_laneAskedH = m_sizeH;
        }
    }

    Preview::ViewDesc desc{};
    desc.w      = m_sizeW;
    desc.h      = m_sizeH;
    desc.source = Preview::Source::WorldCull;
    desc.tickIntervalFrames = static_cast<std::uint32_t>(m_tickInterval);
    // The scene's own sky behind the geometry, which is what makes this look
    // like the game rather than like a cut-out on the clear colour.
    desc.features   = Preview::Feature::kSkybox;
    desc.background = Preview::Background::Skybox;
    // The material payload is meaningless for a WorldCull view (the scene's own
    // material table is bound), so a default component is passed rather than a
    // fabricated one.
    renderer->RequestPreview(m_viewId, desc, MaterialComponent{});

    // ---- The refusal, on the live view, showing the last picture ----
    // Reachable only when a SECOND camera view (a split cell) is live at another
    // size; a lane this panel owns alone is moved by the call above. The engine
    // suspends the view (no turn, no generation), so this branch draws its last
    // picture, greyed, at its OWN size and returns - never stretched into the
    // panel's current rect. The split panel renders the refused state the same
    // way.
    if (renderer->IsPreviewViewSizeRefused(m_viewId))
    {
        std::uint32_t lw = 0, lh = 0;
        renderer->GetPreviewWorldCullSize(lw, lh);
        std::uint32_t ow = 0, oh = 0;
        const bool haveLast = renderer->GetPreviewOutputSize(m_viewId, ow, oh);
        const std::uint64_t lastSrv =
            haveLast ? renderer->GetPreviewSrv(m_viewId) : 0ull;
        if (!m_sizeRefusedLogged)
        {
            m_sizeRefusedLogged = true;
            LOG_INFO("[PREVIEW] camera panel is showing the size refusal (wanted "
                     "%ux%u, lane is %ux%u) - %s", m_sizeW, m_sizeH, lw, lh,
                     lastSrv ? "the panel is showing its LAST picture, frozen"
                             : "the panel has no picture to show yet");
        }
        if (lastSrv)
        {
            // The same submitter and line shape as the split panel's suspended cell, so
            // the two panels cannot drift. It draws the image, so this panel has no rect
            // expression of its own.
            SubmitFrozenImage("[PREVIEW] camera panel:", m_viewId,
                              ImTextureRef(static_cast<ImTextureID>(lastSrv)),
                              ow, oh, m_sizeW, m_sizeH, lw, lh);
            ImGui::TextDisabled("FROZEN %u x %u", ow, oh);
        }
        ImGui::TextDisabled("Size refused: camera views are pinned to %u x %u "
                            "and this one wants %u x %u. They share ONE render "
                            "graph, which has ONE size.", lw, lh, m_sizeW, m_sizeH);
        if (lw != 0 && lh != 0 && ImGui::SmallButton("Match size"))
        {
            // m_measuredW is deliberately not touched: it is the last value the panel's
            // rectangle produced, and the debounce re-arms on a change to it. An
            // unchanged rectangle re-measures to the same number, so the adopted size
            // sticks; writing lw into it would make the adoption oscillate.
            m_sizeW = m_pendW = lw;
            m_sizeH = m_pendH = lh;
            if (m_harnessW != 0) { m_harnessW = lw; m_harnessH = lh; }
        }
        ++m_frameTick;
        ImGui::End();
        return;
    }
    m_sizeRefusedLogged = false;

    // ---- The image (invariant 2: resolved HERE, once, every frame) ----
    const std::uint64_t srv = renderer->GetPreviewSrv(m_viewId);
    // A fixed render size wider than the panel is shown scaled down to it, its aspect kept.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float fit = (availW > 0.f && static_cast<float>(m_sizeW) > availW) ? availW / static_cast<float>(m_sizeW) : 1.f;
    const ImVec2 imgSize(static_cast<float>(m_sizeW) * fit, static_cast<float>(m_sizeH) * fit);
    if (srv)
    {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), imgSize);
        // The limitation tooltip belongs to the image: IsItemHovered() names the
        // last submitted item, so the call sits immediately after the Image.
        // Point/spot lights are shown (a frustum-culled, importance-capped subset over
        // the view's own grid) but WITHOUT shadows: this graph binds no punctual
        // shadow atlas and every record carries shadowSliceIdx 0xFFFFFFFF. That
        // plausible-looking wrongness is what the tooltip has to say.
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Secondary view: point/spot lights are shown but "
                              "WITHOUT their shadows, and only the nearest few "
                              "inside this camera's frustum; directional "
                              "shadows are borrowed from the main camera's "
                              "cascades; no reflection probes, no water "
                              "surface, and no post-processing.");
        // No input capture (invariant 1): ImGui::Image submits a non-interactive
        // item on purpose - this panel has no camera of its own to fly, and an
        // InvisibleButton would take hover and active state for no gain.
        // `secondary` counts kept packets the MAIN camera cannot see at all; a camera
        // pointed away from the main one that reports 0 there is showing only the
        // intersection of the two frusta plus sky.
        // The counts are this panel's view, not whichever view recorded last.
        ImGui::TextDisabled("%u x %u  |  %u draw(s) of %u (%u secondary)  |  view %u",
                            m_sizeW, m_sizeH,
                            renderer->GetPreviewWorldCullDraws(m_viewId),
                            renderer->GetPreviewWorldCullCandidates(m_viewId),
                            renderer->GetPreviewWorldCullSecondary(m_viewId),
                            m_viewId);
    }
    else
    {
        ImGui::Dummy(imgSize);
        ImGui::TextDisabled("Rendering...");
    }

    // PANEL FRAMES, counted at the one place the panel actually drew a picture
    // path to its end. -preview_camera_resize reads it, and counting only the
    // complete frames is what keeps "resize at frame N" meaning the same thing
    // whether or not the camera resolved on frame 0.
    ++m_frameTick;
    ImGui::End();
}

void CameraPreviewWindow::ProcessPendingActions(Renderer* renderer)
{
    // AUTO-RELEASE. The panel can stop drawing by several routes that never
    // reach a Close button - the menu item is unticked, the window is collapsed,
    // F11 hides every panel - and relying on each of those to notify is how a
    // view id leaks for the rest of the session. This runs BEFORE this frame's
    // Draw, so the flag read here is the previous frame's answer, which is the
    // correct one: "the panel did not draw last frame".
    if (m_visibleThisFrame)            ++m_drewFrames;
    else if (!m_harnessName.empty())   ++m_hiddenFrames;

    if (m_viewId != ~0u && !m_visibleThisFrame)
    {
        ++m_autoReleases;
        // ---- The oracle (-panelvisibleverify) ----
        // A panel the harness asked for that stops drawing is a false-pass generator:
        // every pack assertion (the PNG, the world-cull line, renders, hdrSlotsPeak)
        // is downstream of a draw, so all go red with no reason. This turns the
        // silent auto-release into a named failure where it happens.
        // Opt-in, because -preview_camera_close_at and -preview_close_at both
        // legitimately end here and the panel sees only the first
        // (m_harnessCloseFrame); packs that never close this panel pass the flag.
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (Sys::CommandLine::Get().Has("-panelvisibleverify") &&
            !m_harnessName.empty() && m_harnessCloseFrame == 0)
            ENGINE_CHECK_ONCE(false,
                "-preview_camera='%s' was asked for, but the Camera Preview "
                "panel stopped drawing after %llu frame(s) (beginRefused=%llu) "
                "and view %u is being handed back. beginRefused>0 means "
                "ImGui::Begin() refused the window - usually a DOCKED panel "
                "under -hidden, where the dockspace has no area; see the layout "
                "policy in EditorLayer::OnAttach. Nothing downstream of this "
                "can produce a picture.",
                m_harnessName.c_str(),
                static_cast<unsigned long long>(m_drewFrames),
                static_cast<unsigned long long>(m_beginRefusedFrames),
                m_viewId);
#endif
        m_releaseQueued = true;
    }
    m_visibleThisFrame = false;

    if (!m_releaseQueued) return;
    m_releaseQueued = false;
    if (m_viewId != ~0u && renderer)
    {
        renderer->SetPreviewCameraEntity(m_viewId, NullEntity);
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[PREVIEW] camera panel released view %u at the frame boundary",
                 m_viewId);
    }
    m_viewId = ~0u;
}

// Shutdown release (see the header): what the frame-boundary path does, minus
// the "did the panel draw" question, through the same ReleasePreviewView.
void CameraPreviewWindow::ReleaseViewsAtShutdown(Renderer* renderer)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // One line at the end, so a pack can BOUND the panel's visibility instead of
    // inferring it from a missing PNG: `drew` is the assertion, `beginRefused`
    // the diagnosis. Emitted whenever the flag is on, including the healthy case,
    // so the good value is known.
    if (Sys::CommandLine::Get().Has("-panelvisibleverify"))
        LOG_INFO("[PANELVIS] camera asked=%u drew=%llu hidden=%llu "
                 "beginRefused=%llu autoReleases=%u resolves=%u",
                 m_harnessName.empty() ? 0u : 1u,
                 static_cast<unsigned long long>(m_drewFrames),
                 static_cast<unsigned long long>(m_hiddenFrames),
                 static_cast<unsigned long long>(m_beginRefusedFrames),
                 m_autoReleases, m_resolveCount);
#endif
    m_releaseQueued    = false;
    m_visibleThisFrame = false;
    if (m_viewId == ~0u) return;
    if (renderer)
    {
        renderer->SetPreviewCameraEntity(m_viewId, NullEntity);
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[PREVIEW] camera panel released view %u at shutdown", m_viewId);
    }
    m_viewId = ~0u;
}

} // namespace Editor
