// Asset preview panel; the header has the contract and the invariants.

#include "Editor/AssetPreviewWindow.h"

#include "ECS/ECS.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>   // std::snprintf for the "not previewed:" line

namespace Editor
{
namespace
{
    // The material inspector's four square sizes (MaterialPreviewWidget's QuantiseSize), for the same
    // reason: a recompile recreates seven physical textures, so the reachable sizes must be few and the
    // commit debounced. Square because the studio rig frames a unit box.
    std::uint32_t QuantiseSize(float px)
    {
        if (px <= 192.f) return 128u;
        if (px <= 320.f) return 256u;
        if (px <= 448.f) return 384u;
        return 512u;
    }
    bool IsQuantised(std::uint32_t px)
    {
        return px == 128u || px == 256u || px == 384u || px == 512u;
    }

    constexpr double kResizeDebounceSeconds = 0.200;
}

AssetPreviewWindow& AssetPreviewWindow::Get()
{
    static AssetPreviewWindow s_instance;
    return s_instance;
}

// Refused rather than rounded: a size the panel cannot reach by hand would exercise a recompile
// no user can provoke.
bool AssetPreviewWindow::HarnessSetSize(std::uint32_t px)
{
    if (!IsQuantised(px)) return false;
    m_harnessSize = px;
    return true;
}

bool AssetPreviewWindow::HarnessSetDeadlineFrames(std::uint32_t frames)
{
    // Zero is refused: disabling the deadline is the false pass it exists to remove.
    if (frames == 0) return false;
    m_harnessDeadline = frames;
    LOG_INFO("[PREVIEW] asset preview deadline set to %u panel frame(s) "
             "(-preview_asset_deadline; default 120)", frames);
    return true;
}

bool AssetPreviewWindow::Show(const std::string& assetPath, bool fromHarness)
{
    // A .meshlib, a .ipfb, a model (.iscn) or one Mesh of a model ("<model .iscn>#<MeshKey hex>").
    const Preview::AssetKind kind = Preview::AssetKindForPath(assetPath);

    if (kind == Preview::AssetKind::None)
    {
        // Refused rather than ignored: silently keeping the previous asset on an .imsh drop would read as
        // a broken preview, and .imsh is the file a user would try, but nothing at runtime turns one into a
        // mesh-descriptor slot. An ERROR when a harness asked: a pack that named the wrong file would
        // otherwise run its whole frame budget against whatever was open and assert on that.
        if (fromHarness)
            LOG_ERROR("[PREVIEW] -preview_asset refuses '%s': only .meshlib, .ipfb, "
                      ".iscn and <model .iscn>#<MeshKey> carry library geometry (a "
                      ".imsh has no runtime path to a mesh-descriptor slot since the "
                      "P1-P6 rewrite). The panel is NOT opened on it.", assetPath.c_str());
        else
            LOG_WARNING("[PREVIEW] asset preview refuses '%s': only .meshlib, .ipfb, "
                        ".iscn and <model .iscn>#<MeshKey> carry library geometry (a "
                        ".imsh has no runtime path to a mesh-descriptor slot since the "
                        "P1-P6 rewrite)", assetPath.c_str());
        return false;
    }
    if (m_assetPath == assetPath && m_assetKind == kind) return true;

    m_assetPath = assetPath;
    m_assetKind = kind;
    // A new asset re-arms the deadline (else a pack that switched assets would keep the first one's
    // resolved latch) and drops m_harnessPath, so a panel the browser re-pointed does not fire quoting
    // the harness's asset. HarnessShow sets it again right after for the harness route.
    m_harnessResolved        = false;
    m_harnessTries           = 0;
    m_harnessDeadlineExpired = false;
    m_harnessPath.clear();
    LOG_INFO("[PREVIEW] asset preview showing '%s' (%s)", assetPath.c_str(),
             kind == Preview::AssetKind::Mesh      ? "mesh library"
           : kind == Preview::AssetKind::Prefab    ? "prefab"
           : kind == Preview::AssetKind::Model     ? "model"
                                                   : "mesh");
    return true;
}

void AssetPreviewWindow::Draw(Renderer* renderer, World* world, bool* open)
{
    ImGui::SetNextWindowSize(ImVec2(360.f, 460.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Preview", open))
    {
        // Collapsed is not visible: m_visibleThisFrame stays false, so the next ProcessPendingActions
        // hands the view back and a folded tab does not hold the HDR slot.
        ImGui::End();
        return;
    }

    if (!renderer)
    {
        ImGui::TextDisabled("No renderer attached.");
        ImGui::End();
        return;
    }
    if (!renderer->IsPreviewEnabled())
    {
        // The preview kill switch, reported rather than shown as a blank box: the reverse test of every
        // preview pack is turning it off and watching the assertions fail.
        ImGui::TextDisabled("Preview disabled (DX12_PREVIEW=0 or "
                            "editor_prefs preview.enabled=0)");
        ImGui::End();
        return;
    }

    m_visibleThisFrame = true;

    // ---- A world reload invalidates the composition ----
    // Its parts are mesh-descriptor slots, which MeshManager::OnWorldClear invalidates by bumping the
    // generation. The Renderer drops its own cache there; the panel re-asks for the same asset so the
    // next extract composes it again.
    if (world)
    {
        const std::uint32_t epoch = world->Epoch();
        if (epoch != m_worldEpoch)
        {
            if (m_worldEpochSeen && !m_assetPath.empty())
            {
                LOG_INFO("[PREVIEW] asset panel: world epoch %u -> %u, recomposing "
                         "'%s' (mesh-descriptor slots are invalidated by a reload)",
                         m_worldEpoch, epoch, m_assetPath.c_str());
                // Queued, not done here: Draw runs from OnUIRender, a render-side thread context
                // (FramePipeline::BuildEarlyEditorUI or RenderHalf, safe only because the render half is
                // parked), while the composition memo is game-half state, dropped in ProcessPendingActions
                // like the view release.
                m_recomposeQueued = true;
                m_harnessResolved = false;
                m_harnessTries    = 0;
            }
            m_worldEpoch     = epoch;
            m_worldEpochSeen = true;
        }
    }

    if (m_assetPath.empty())
    {
        if (m_viewId != ~0u) m_releaseQueued = true;
        ImGui::TextDisabled("Select a Model (.iscn), one of its Meshes, a Geometry\n"
                            "library (.meshlib) or a Prefab (.ipfb) in the\n"
                            "Content Browser and choose Preview.");
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("%s", m_assetPath.c_str());
    ImGui::Separator();

    // ---- Controls ----
    {
        static const char* kBgNames[] = { "Solid", "Checker", "Skybox" };
        int bgIdx = static_cast<int>(m_background);
        ImGui::SetNextItemWidth(110.f);
        if (ImGui::Combo("Backdrop", &bgIdx, kBgNames, IM_ARRAYSIZE(kBgNames)))
            m_background = static_cast<Preview::Background>(bgIdx);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Shadow", &m_studioShadow);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view"))
    {
        m_yawOffset = m_pitchOffset = 0.f;
        m_distScale = 1.f;
    }

    // ---- Size: quantise, then debounce (invariant 3) ----
    if (m_harnessSize != 0)
    {
        // Pinned, bypassing the debounce too: under -hidden the content region is <= 0, and a debounce
        // makes the commit frame depend on wall-clock speed while a pack asserts on a frame number.
        if (m_size != m_harnessSize)
            LOG_INFO("[PREVIEW] asset panel harness size -> %ux%u",
                     m_harnessSize, m_harnessSize);
        m_size = m_sizePending = m_harnessSize;
    }
    else
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const std::uint32_t want = QuantiseSize(avail > 0.f ? avail : 256.f);
        const double now = ImGui::GetTime();
        if (want != m_sizePending) { m_sizePending = want; m_sizePendingAt = now; }
        else if (m_sizePending != m_size &&
                 (now - m_sizePendingAt) >= kResizeDebounceSeconds)
            m_size = m_sizePending;
    }

    if (m_releaseQueued)
    {
        ImGui::TextDisabled("Asset preview: switching...");
        ImGui::End();
        return;
    }

    if (m_viewId == ~0u)
    {
        // worldCull=false: a Synthetic view - hand-built packets against the preview's own instance and
        // material tables, no World and no second frustum cull.
        m_viewId = renderer->AcquirePreviewView(/*worldCull=*/false);
        if (m_viewId == ~0u)
        {
            ImGui::TextDisabled("Asset preview unavailable (no free view)");
            ImGui::End();
            return;
        }
    }

    Preview::ViewDesc desc{};
    desc.w = desc.h    = m_size;
    desc.source        = Preview::Source::Synthetic;
    desc.assetKind     = m_assetKind;
    desc.assetPath     = m_assetPath;
    desc.background    = m_background;
    desc.features      = (m_background != Preview::Background::Solid
                            ? Preview::Feature::kSkybox : 0u)
                       | (m_studioShadow ? Preview::Feature::kStudioShadow : 0u);
    // Alpha 0, not 1 (see Preview::ViewDesc::clearColor): the HDR slot's optimised clear value is
    // {0,0,0,0} and D3D12 compares alpha too, so 1 raises the MISMATCHINGCLEARVALUE warning every frame.
    desc.clearColor[0] = desc.clearColor[1] = desc.clearColor[2] = 0.f;
    desc.clearColor[3] = 0.f;
    desc.yawOffset     = m_yawOffset;
    desc.pitchOffset   = m_pitchOffset;
    desc.distanceScale = m_distScale;
    // The material payload is meaningless for an asset view (each part carries
    // its own), so a default component is passed rather than a fabricated one.
    renderer->RequestPreview(m_viewId, desc, MaterialComponent{});

    // ---- The image (invariant 2: resolved here, once, every frame) ----
    const std::uint64_t srv = renderer->GetPreviewSrv(m_viewId);
    const ImVec2 imgSize(static_cast<float>(m_size), static_cast<float>(m_size));

    std::uint32_t parts = 0, materials = 0, dropped = 0;
    Resource::PrefabSkipCounts skipped{};
    renderer->GetPreviewAssetStats(m_viewId, parts, materials, dropped, &skipped);

    if (srv)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), imgSize);
        // ---- Orbit, on the panel's own state (invariant 1) ----
        // The InvisibleButton over the image writes m_yawOffset / m_pitchOffset / m_distScale and nothing
        // else; writing m_vpMouseDX/DY would fly the main editor camera (App::TickCamera).
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##assetorbit", imgSize);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            m_yawOffset   += d.x * 0.01f;
            m_pitchOffset -= d.y * 0.01f;
            // Clamped short of the poles: the studio rig builds its view matrix
            // with a fixed world up, so +-90 degrees is a degenerate basis.
            m_pitchOffset = std::clamp(m_pitchOffset, -1.4f, 1.4f);
        }
        if (ImGui::IsItemHovered())
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f)
                m_distScale = std::clamp(m_distScale * (1.f - wheel * 0.1f), 0.3f, 4.f);
            // The tooltip states only the rest pose, true of every skinned asset (a property of the draw
            // path); what a prefab omits is named per asset, with counts, by the amber line below.
            ImGui::SetTooltip("Drag to orbit, wheel to zoom.\n\n"
                              "SKINNED MESHES ARE AT THEIR REST (BIND) POSE - the\n"
                              "preview draws library geometry with no skeleton.");
        }

        if (dropped > 0)
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                               "%u part(s) of %u drawn - %u dropped (over the cap "
                               "of %u, or a mesh that did not resolve)",
                               parts, parts + dropped, dropped,
                               Preview::kMaxPreviewParts);
        else
            ImGui::TextDisabled("%u x %u  |  %u part(s), %u material(s)  |  view %u",
                                m_size, m_size, parts, materials, m_viewId);

        // ---- What the prefab walk declined ----
        // A picture that is partial by design must say so on screen, or an artist whose prefab's identity
        // is an emitter reads the preview as broken. Only non-zero kinds are printed, and only when there
        // is one.
        if (skipped.Total() > 0)
        {
            char msg[256];
            int  n = std::snprintf(msg, sizeof(msg), "not previewed:");
            auto add = [&](std::uint32_t c, const char* one, const char* many)
            {
                if (c == 0 || n <= 0 || n >= static_cast<int>(sizeof(msg))) return;
                const int w = std::snprintf(
                    msg + n, sizeof(msg) - static_cast<std::size_t>(n), "%s %u %s",
                    (msg[n - 1] == ':') ? "" : ",", c, (c == 1) ? one : many);
                if (w > 0) n += w;
            };
            add(skipped.emitters,   "emitter",   "emitters");
            add(skipped.trails,     "trail",     "trails");
            add(skipped.lights,     "light",     "lights");
            add(skipped.decals,     "decal",     "decals");
            add(skipped.billboards, "billboard", "billboards");
            // `surfaces` is Grass / Terrain / Water and `worldUI` the four WorldUI* blocks: both draw through
            // passes the preview graph does not own, and a shipped prefab carries the latter.
            add(skipped.surfaces,   "surface",   "surfaces");
            add(skipped.worldUI,    "world UI element", "world UI elements");
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "%s", msg);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "The prefab places these and the preview does not draw\n"
                    "them: it composes STATIC MESHES only (a SceneRef's\n"
                    "library entries and MeshHandle primitives). Counted per\n"
                    "NODE - a node carrying ParticleRender + ParticleEmit is\n"
                    "ONE emitter.");
        }

        // No auto-fit is stated, not silent: when every library entry's AABB is zero the merged bounds
        // have no extent, the studio fit cannot derive a scale and the subject is drawn at its authored
        // size, which among fitted thumbnails reads as a renderer bug.
        if (renderer->IsPreviewAssetUnfitted(m_assetPath))
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                               "No computed bounds - drawn at AUTHORED size, not "
                               "fitted to the studio box.");
    }
    else
    {
        ImGui::Dummy(imgSize);
        ImGui::TextDisabled("Rendering...");
    }

    // ---- Never-resolved deadline: retrying forever in silence is a false pass ----
    // A -preview_asset path with no drawable part leaves an empty panel; image assertions then fail
    // like a renderer bug, and a pack without them goes green having shown nothing. Counts panel draws
    // with zero parts: the path resolves at once, but its mesh library streams.
    if (!m_harnessPath.empty() && !m_harnessResolved)
    {
        if (parts > 0)
        {
            m_harnessResolved = true;
            LOG_INFO("[PREVIEW] -preview_asset='%s' resolved to %u part(s), "
                     "%u material(s)", m_harnessPath.c_str(), parts, materials);
        }
        else if (++m_harnessTries == m_harnessDeadline)
        {
            // A precondition failure, not a logged complaint: every assertion the pack makes about the asset
            // lane would now be about the empty case. The exit is posted on the game half (TickHarnessPreview
            // reads HarnessDeadlineExpired) because Draw runs in a render-side thread context.
            m_harnessDeadlineExpired = true;
            LOG_ERROR(
                "-preview_asset='%s' never resolved to a drawable part in %u "
                "panel frames. The Asset Preview panel is showing nothing and "
                "this run is measuring the no-panel case. Check the path "
                "(project-relative, percent-encode spaces), that the file is a "
                ".meshlib or a .ipfb, and that its mesh library exists.",
                m_harnessPath.c_str(), m_harnessDeadline);
        }
    }

    ImGui::End();
}

void AssetPreviewWindow::ProcessPendingActions(Renderer* renderer)
{
    // Auto-release: the panel can stop drawing without a Close (menu item unticked, window collapsed,
    // F11 hides every panel), and relying on each route to notify leaks the view for the session. This
    // runs before this frame's Draw, so the flag is last frame's answer, the one wanted.
    if (m_viewId != ~0u && !m_visibleThisFrame) m_releaseQueued = true;
    m_visibleThisFrame = false;

    // The composition memo, dropped on the half that owns it (see Draw).
    if (m_recomposeQueued && renderer)
    {
        m_recomposeQueued = false;
        if (!m_assetPath.empty()) renderer->InvalidatePreviewAsset(m_assetPath);
    }

    if (!m_releaseQueued) return;
    m_releaseQueued = false;
    if (m_viewId != ~0u && renderer)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[PREVIEW] asset panel released view %u at the frame boundary",
                 m_viewId);
    }
    m_viewId = ~0u;
}

// Shutdown release (see the header): the frame-boundary release minus the "did the panel draw"
// question, through the same ReleasePreviewView.
void AssetPreviewWindow::ReleaseViewsAtShutdown(Renderer* renderer)
{
    m_releaseQueued = false;
    if (m_viewId == ~0u) return;
    if (renderer)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[PREVIEW] asset panel released view %u at shutdown", m_viewId);
    }
    m_viewId = ~0u;
}

} // namespace Editor
