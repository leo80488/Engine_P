#include "Editor/ColorWidgets.h"
// MaterialPreviewWidget.cpp - see the header for the three UI invariants.

#include "Editor/MaterialPreviewWidget.h"
#include "Graphics/DynamicShaderRegistry.h"   // the rest-shape log line names the VS

#include "Editor/MeshEditorWindow.h"   // the view modes' names
#include "Renderer/Renderer.h"
#include "Renderer/Preview/PreviewTypes.h"
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // ImGuiWindow::SkipItems - see the visibility gate

#include <algorithm>   // std::clamp on the orbit offsets, std::max on the edit buffer
#include <cstdio>
#include <cstring>     // std::memcpy into the mesh-path edit buffer
#include <string_view>
#include <vector>      // the mesh-path edit buffer, sized from the path

namespace
{
    // Invariant 3: the panel's pixel width is snapped to one of four sizes
    // before it ever reaches the renderer. A rebuild destroys and recreates the
    // target, so an unquantised width would do that on every pixel of a drag.
    std::uint32_t QuantiseSize(float px)
    {
        if (px <= 192.f) return 128u;
        if (px <= 320.f) return 256u;
        if (px <= 448.f) return 384u;
        return 512u;
    }

    constexpr double kResizeDebounceSeconds = 0.200;   // invariant 3
}

namespace Editor
{

MaterialPreviewWidget& MaterialPreviewWidget::Get()
{
    static MaterialPreviewWidget s_inst;
    return s_inst;
}

void MaterialPreviewWidget::Draw(Renderer* renderer, const std::string& assetPath,
                                 const MaterialComponent& mat)
{
    m_visibleThisFrame = false;

    ImGui::SeparatorText("Preview");

    if (!renderer)
    {
        ImGui::TextDisabled("Preview unavailable (no renderer)");
        return;
    }

    // Kill switch. Renderer::IsPreviewEnabled is the single answer for both
    // halves (env and pref), so this widget cannot disagree with
    // Debug.Stats().previewEnabled. The pref is tested first only to name which
    // half is off: a text line, because an empty panel and a disabled feature
    // look identical otherwise.
    if (!m_prefEnabled)
    {
        ImGui::TextDisabled("Preview disabled (editor_prefs preview.enabled=0)");
        return;
    }
    if (!renderer->IsPreviewEnabled())
    {
        ImGui::TextDisabled("Preview disabled (DX12_PREVIEW=0)");
        return;
    }

    // Switching to a different asset releases the old view through the deferred
    // path. Doing it inline would tear down a descriptor this same ImGui frame
    // may already have handed to AddImage.
    if (!m_boundAsset.empty() && m_boundAsset != assetPath)
    {
        NotifyClosed();
        // The rest-shape notice belongs to the material, not to the session.
        m_restShapeBadgeLogged = false;
    }
    m_boundAsset = assetPath;

    // Collapsed or zero-width is not visible. Unlike the main viewport panel,
    // which latches stale hover/drag state on a collapsed tab, this panel resets
    // its state every frame and treats "no content region" as "not on screen".
    // Calling a collapsed tab visible would hold the view id and its HDR slot for
    // a panel nobody sees, render it on every dirty edge, and flip the quantised
    // size to 128 and back on collapse/expand (rebuilding the target each way).
    // Returning with m_visibleThisFrame false hands the view back at the next
    // frame boundary; the next real Draw re-acquires it, and AcquireView arms
    // needsRender so the picture comes straight back.
    {
        // RenderInspectorPanel discards the result of
        // `ImGui::Begin(kWindowInspector);`, as RenderViewportPanel does, so a
        // collapsed or unselected Inspector tab still runs every line below.
        // SkipItems is that discarded return value, inverted: it covers collapsed,
        // unselected dock tab and fully clipped alike.
        const ImGuiWindow* win = ImGui::GetCurrentWindow();
        const bool notDrawn = ImGui::IsWindowCollapsed()
                           || (win && win->SkipItems)
                           || ImGui::GetContentRegionAvail().x <= 0.f;

        // Only when the host has a UI surface at all - load-bearing, not defensive.
        // Under -hidden the window is SW_SHOWMINNOACTIVE, so io.DisplaySize is 0x0 and
        // every ImGui window is skipped with a negative content region. Applying the
        // visibility test there would make the preview permanently invisible to the
        // harness; with no surface the question has no answer, so it is not asked.
        const ImGuiIO& io = ImGui::GetIO();
        const bool hostHasSurface = io.DisplaySize.x > 0.f && io.DisplaySize.y > 0.f;

        if (hostHasSurface && notDrawn)
        {
            // Deliberately without setting m_visibleThisFrame: the next
            // ProcessPendingActions hands the view back at the frame boundary (the only
            // legal release window).
            return;
        }
    }

    if (m_frameTick < m_startFrame)
    {
        ImGui::TextDisabled("Preview starts at frame %llu (-preview_frames)",
                            static_cast<unsigned long long>(m_startFrame));
        return;
    }

    m_visibleThisFrame = true;

    if (m_releaseQueued)
    {
        // The outgoing view is torn down at the next frame boundary. Do nothing this
        // frame: acquiring now would hold two views for one frame, and a request on
        // the outgoing one is dropped by the generation guard anyway.
        // m_visibleThisFrame is already set, so the auto-release does not fire again.
        ImGui::TextDisabled("Preview: switching...");
        return;
    }

    if (m_viewId == ~0u)
    {
        m_viewId = renderer->AcquirePreviewView();
        if (m_viewId == ~0u)
        {
            // Logged once as well as shown: Preview::kMaxPreviews views are the whole
            // pool, and a full split viewport (one view per cell) uses them all, so a user
            // reaches this by opening two panels; a headless pack can only read the log.
            // One-shot because this runs every frame; re-armed when an acquire succeeds.
            if (!m_noViewLogged)
            {
                m_noViewLogged = true;
                LOG_WARNING("[PREVIEW] material inspector: no free view (all %u "
                            "are in use - the split viewport panel holds one per "
                            "cell). The panel shows 'no free view' until one is "
                            "released.", Preview::kMaxPreviews);
            }
            ImGui::TextDisabled("Preview unavailable (no free view)");
            return;
        }
        if (m_noViewLogged)
        {
            // The recovery line: "no free view" alone cannot tell a slot that came back
            // from one that never did, and a log assertion can gate on this rather than
            // on "view N acquired", which every panel emits.
            m_noViewLogged = false;
            LOG_INFO("[PREVIEW] material inspector: acquired view %u after "
                     "waiting for a free slot", m_viewId);
        }
    }

    // ---- Size: quantise, then debounce (invariant 3) ----
    // Unless the harness pinned it (see SetHarnessSize): under -hidden the
    // content region is always <= 0, so the measured branch only produces 256. A
    // pinned value also skips the debounce, which would make the commit frame a
    // function of wall-clock speed when a pack asserts on a frame number.
    std::uint32_t harnessWant = m_harnessSize;
    if (m_harnessResizeFrame != 0 && m_harnessResizePx != 0 &&
        m_frameTick >= m_harnessResizeFrame)
        harnessWant = m_harnessResizePx;

    if (harnessWant != 0)
    {
        if (harnessWant != m_sizeCommitted)
            LOG_INFO("[PREVIEW] harness size -> %ux%u at editor frame %llu",
                     harnessWant, harnessWant,
                     static_cast<unsigned long long>(m_frameTick));
        m_sizePending = m_sizeCommitted = harnessWant;
    }
    else
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const std::uint32_t want = QuantiseSize(avail > 0.f ? avail : 256.f);
        const double now = ImGui::GetTime();
        if (want != m_sizePending)
        {
            m_sizePending   = want;
            m_sizePendingAt = now;
        }
        else if (m_sizePending != m_sizeCommitted &&
                 (now - m_sizePendingAt) >= kResizeDebounceSeconds)
        {
            m_sizeCommitted = m_sizePending;
        }
    }

    // ---- Controls ----
    // Shape first: the control users reach for most (a Plane reads a tiling
    // pattern better than a sphere, a Torus shows anisotropy).
    {
        static const char* kShapeNames[] = { "Sphere", "Cube", "Plane", "Torus" };
        int shapeIdx = static_cast<int>(m_shape);
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::Combo("Shape", &shapeIdx, kShapeNames, IM_ARRAYSIZE(kShapeNames)))
            m_shape = static_cast<Preview::Shape>(shapeIdx);
    }
    ImGui::SameLine();
    // ColorEdit3 rather than 4: alpha is meaningless for a background nothing
    // composites over.
    ImGui::SetNextItemWidth(140.f);
    EditorUI::ColorEdit3("Background", m_color);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view"))
    {
        m_yawOffset = m_pitchOffset = 0.f;
        m_distScale = 1.f;
    }

    // Backdrop mode. Solid (the plain clear colour) stays the default.
    {
        static const char* kBgNames[] = { "Solid", "Checker", "Skybox" };
        int bgIdx = static_cast<int>(m_background);
        ImGui::SetNextItemWidth(110.f);
        if (ImGui::Combo("Backdrop", &bgIdx, kBgNames, IM_ARRAYSIZE(kBgNames)))
            m_background = static_cast<Preview::Background>(bgIdx);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Shadow", &m_studioShadow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Render the studio key light's 512x512 shadow map.\n"
                          "Visible on a torus, a cube or a mesh override; a\n"
                          "sphere's self-shadow lands exactly where N.L already\n"
                          "darkens it.");
    ImGui::SameLine();
    ImGui::Checkbox("Scene lighting", &m_sceneLighting);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Light the material with the OPEN SCENE's sky and sun\n"
                          "instead of the fixed studio rig.\n\n"
                          "The preview then changes with time of day, so it is\n"
                          "NOT reproducible between runs - and asset-browser\n"
                          "thumbnails always use the studio rig whatever this\n"
                          "says, because they are cached to disk.");
    // Mesh override: a text field plus a drop target rather than a browse dialog.
    // The drop target is how the Asset Browser hands one over; the field is what
    // a harness flag and a copy-paste need.
    {
        // Sized from the string, not a fixed 260: paths can exceed MAX_PATH (a
        // \\?\ path, a deep asset tree), and a fixed buffer fails silently - the path
        // is truncated into the control and the next InputText edit writes the
        // truncated text back to m_meshPath. Rebuilt per frame from m_meshPath, so
        // the headroom follows the text as it is typed.
        std::vector<char> buf((std::max)(std::size_t{ 512 },
                                         m_meshPath.size() + 64), '\0');
        std::memcpy(buf.data(), m_meshPath.data(), m_meshPath.size());
        ImGui::SetNextItemWidth(240.f);
        if (ImGui::InputText("Mesh", buf.data(), buf.size()))
            m_meshPath = buf.data();
        if (ImGui::BeginDragDropTarget())
        {
            // The Asset Browser's payload id. A dropped .meshlib replaces the primitive;
            // anything else is refused by Renderer::ResolvePreviewMesh and the primitive
            // stays.
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IMESHLIB_PATH"))
                if (pl->Data && pl->DataSize > 0)
                    m_meshPath.assign(static_cast<const char*>(pl->Data),
                                      static_cast<std::size_t>(pl->DataSize - 1));
            ImGui::EndDragDropTarget();
        }
        if (!m_meshPath.empty())
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ImGui::InputInt("id", &m_meshId, 0, 0);
            if (m_meshId < 0) m_meshId = 0;
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear mesh")) { m_meshPath.clear(); m_meshId = 0; }
        }
    }

    // The view mode (Lit, Unlit, Wireframe, the GBuffer channels), only where the
    // host asked for the control (the Material Editor).
    if (m_showViewMode)
    {
        static const char* kModes[] = { "lit", "unlit", "wireframe", "basecolor", "normal", "roughness",
                                        "metallic", "ao", "depth" };
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::BeginCombo("View", MeshEditorWindow::ViewModeName(m_viewMode)))
        {
            for (const char* name : kModes)
            {
                std::uint32_t mode = 0;
                if (!MeshEditorWindow::ParseViewMode(name, mode)) continue;
                if (ImGui::Selectable(name, mode == m_viewMode)) m_viewMode = mode;
            }
            ImGui::EndCombo();
        }
    }

    Preview::ViewDesc desc;
    desc.w = m_sizeCommitted;
    desc.h = m_sizeCommitted;
    desc.clearColor[0] = m_color[0];
    desc.clearColor[1] = m_color[1];
    desc.clearColor[2] = m_color[2];
    // 0, not 1 - see Preview::ViewDesc::clearColor. The HDR slot's optimised
    // clear value is {0,0,0,0} and D3D12 compares alpha too, so a 1 here raises
    // the MISMATCHINGCLEARVALUE perf warning on the preview's own clear every
    // rendered frame.
    desc.clearColor[3] = 0.f;
    desc.shape         = m_shape;
    desc.viewMode      = m_viewMode;
    desc.background    = m_background;
    desc.meshPath      = m_meshPath;
    desc.meshId        = static_cast<std::uint32_t>(m_meshId);
    // Harness-pinned feature bits (e.g. Feature::kNoAmbientFill for the parity
    // run) ORed with the ones the controls own. The harness bits are additive, so
    // a pack that pins one does not switch the others off - except through
    // m_harnessFeatureClear below.
    desc.features      = m_harnessFeatures;
    if (m_studioShadow)   desc.features |= Preview::Feature::kStudioShadow;
    if (m_sceneLighting)  desc.features |= Preview::Feature::kSceneLighting;
    if (desc.background != Preview::Background::Solid)
        desc.features |= Preview::Feature::kSkybox;
    // The harness's veto, applied last (see SetHarnessFeatureClear).
    desc.features &= ~m_harnessFeatureClear;
    desc.yawOffset     = m_yawOffset;
    desc.pitchOffset   = m_pitchOffset;
    desc.distanceScale = m_distScale;

    // ---- The image ----
    // Invariant 2: resolved here, once, every frame; never stored in a member.
    // Resolved before the request so the drag below can attach to the image item.
    const std::uint64_t srv = renderer->GetPreviewSrv(m_viewId);
    const ImVec2 imgSize(static_cast<float>(m_sizeCommitted),
                         static_cast<float>(m_sizeCommitted));
    if (srv)
    {
        // An invisible button under the image, not ImGui::Image + IsItemActive:
        // ImGui::Image is a non-interactive item that never becomes active, so a drag
        // started on it leaks to whatever is behind. The button takes the rect and
        // the draw list paints the picture into it.
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##previewOrbit", imgSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        ImGui::GetWindowDrawList()->AddImage(
            ImTextureRef(static_cast<ImTextureID>(srv)),
            p0, ImVec2(p0.x + imgSize.x, p0.y + imgSize.y));

        // ---- Orbit (invariant 1) ----
        // Reads ImGui's own per-item hover/active state and writes three floats on
        // this widget. Do not touch EditorLayer's m_vpMouseDX/DY: they drive
        // App::TickCamera, so a drag here would fly the main editor camera.
        // The state is ImGui's, so there is no latched "was dragging" flag to go
        // stale, and a collapsed or clipped panel never reaches this line.
        if (hovered)
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f)
                m_distScale = std::clamp(m_distScale * (1.f - wheel * 0.1f), 0.4f, 3.0f);
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            m_yawOffset   += d.x * 0.012f;
            // Clamped short of the poles here as well as in the camera builder: the
            // builder's clamp keeps the matrix finite, this one keeps the control from
            // winding up an offset the user then has to unwind.
            m_pitchOffset  = std::clamp(m_pitchOffset + d.y * 0.012f, -1.2f, 1.2f);
        }
        // Re-read after the drag so the request carries this frame's camera; last
        // frame's would add a second frame of lag to the gesture.
        desc.yawOffset     = m_yawOffset;
        desc.pitchOffset   = m_pitchOffset;
        desc.distanceScale = m_distScale;
    }
    else
    {
        // A rectangle, not empty space: the first frame after opening has no image
        // (the target is built by the next PrepareFrame), and a blank gap reads as a
        // broken feature rather than a frame of latency.
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + imgSize.x, p0.y + imgSize.y);
        ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, IM_COL32(32, 32, 36, 255));
        ImGui::GetWindowDrawList()->AddRect(p0, p1, IM_COL32(90, 90, 96, 255));
        ImGui::Dummy(imgSize);
        ImGui::TextDisabled("rendering...");
    }

    // The request goes last, after the drag has updated `desc`, every visible
    // frame. Renderer::RequestPreview decides whether it is a new picture (see
    // PreviewRenderer::SetRequest / ExtractRequests): the widget tracks no
    // dirtiness, because "differs from the picture on screen" needs the resolved
    // material runtime, which only the game half has.
    renderer->RequestPreview(m_viewId, desc, mat);

    // Streaming hint. The subject is drawn at once with the engine's white /
    // flat-normal fallbacks and re-rendered when the real maps land; without this
    // line the first frames look like a material with no textures assigned.
    if (!renderer->IsPreviewMaterialRuntimeReady(
            renderer->BuildPreviewMaterialRuntimeKey(mat)))
    {
        ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.25f, 1.f), "textures streaming...");
    }

    // ---- Water downgrade, said out loud ----
    // The preview graph has no WaterSurfacePass (no refraction or depth snapshot,
    // no SSR result), so PreviewRenderer::WantsForward keeps a water-domain
    // material on the opaque route whatever its blend mode. The result is a
    // plausible opaque sphere, so the panel says so; WantsForward logs the
    // engine-side "[PREVIEW] water material downgraded:" line. `mat` is the
    // caller's buffer by const reference, read only during this call.
    if (mat.IsWaterShadingModel())
    {
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "water material: previewed on the OPAQUE route - no "
                           "refraction, no SSR");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The preview graph has no WaterSurfacePass: that\n"
                              "pass needs its own refraction and depth snapshots\n"
                              "and an SSR result, none of which this graph owns.\n"
                              "The material is drawn through the deferred route\n"
                              "instead, so its base colour, normal and roughness\n"
                              "are real and everything the water pass adds is\n"
                              "absent. Secondary camera views exclude water for\n"
                              "the same reason.");
    }

    // ---- The studio shadow is the rest shape ----
    // PreviewStudioShadowPass draws with an engine VS (GBuffer_VS, or Shadow_VS for
    // an alpha-tested caster), never the material's custom VS - the pass's deliberate
    // choice (see PreviewStudioShadowPass::Execute) - so a material whose custom VS
    // moves the surface (e.g. wind) casts an undeformed shadow; the panel says so.
    // The query is the read-only one: LookupMaterialCustomVSID neither inserts a
    // RuntimeMaterialShaderState nor registers a dynamic shader (game-half-only),
    // and answers -1 until a material has resolved this shader key, so the badge
    // appears only once the VS is really registered.
    const bool studioShadowOn =
        (desc.features & Preview::Feature::kStudioShadow) != 0;
    const int  vsIdForBadge   = renderer->LookupMaterialCustomVSID(mat);
    // Except a masked custom material - a custom PS whose permutation clips
    // (PreviewRenderer::BuildOnePacket's rule: Masked and Dithered always, Opaque
    // with alpha test). Its studio shadow draws with its own hook pair, custom VS
    // included (ShadowCasterShaders.h), so that shadow does follow the
    // deformation.
    const bool clips = mat.userBlendMode == BlendMode::Masked ||
                       mat.userBlendMode == BlendMode::Dithered ||
                       (mat.userBlendMode == BlendMode::Opaque && mat.IsAlphaTestEnabled());
    const bool hookShadow = clips && renderer->ShadowHookEnabled() &&
                            renderer->LookupMaterialCustomPSID(mat) > 0;
    if (studioShadowOn && vsIdForBadge > 0 && !hookShadow)
    {
        ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.25f, 1.f),
                           "studio shadow = REST shape (custom VS not used)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This material has a custom VERTEX shader, so its\n"
                              "surface can move (World-Position-Offset). The\n"
                              "preview's studio shadow pass runs the ENGINE vertex\n"
                              "shader, so the shadow is cast by the UNDEFORMED\n"
                              "mesh. The main pipeline's shadow pass does follow\n"
                              "the custom VS - this is a preview limitation.");
        if (!m_restShapeBadgeLogged)
        {
            m_restShapeBadgeLogged = true;
            LOG_INFO("[PREVIEW] studio shadow is the REST shape: material '%s' "
                     "has customVS %s and PreviewStudioShadowPass uses the "
                     "engine GBuffer_VS", assetPath.c_str(),
                     DynamicShaderRegistry::Get().Label(
                         static_cast<uint32_t>(vsIdForBadge)).c_str());   // the VS by name, not its id
        }
    }
    ImGui::TextDisabled("view %u  %ux%u  (drag to orbit, wheel to zoom)%s",
                        m_viewId, m_sizeCommitted, m_sizeCommitted,
                        // Said out loud: with -preview_size / -preview_resize the panel's measured
                        // size is ignored, and a human watching a harness-driven session should not
                        // have to guess why dragging the splitter does nothing.
                        (m_harnessSize || m_harnessResizeFrame || m_harnessShapePinned)
                            ? "  [harness-pinned]" : "");
}

void MaterialPreviewWidget::NotifyClosed()
{
    if (m_viewId == ~0u) return;
    // Queue only. ProcessPendingActions performs the release, which in turn
    // queues the device teardown for Renderer::ServicePreviewReleases - two
    // deferrals, because the ImGui handler cannot prove either is unreferenced.
    m_releaseQueued = true;
    m_boundAsset.clear();
}

void MaterialPreviewWidget::ProcessPendingActions(Renderer* renderer)
{
    ++m_frameTick;

    // Push the editor half of the kill switch (see SetPrefEnabled). Latched, not a
    // per-frame write: SetPrefEnabled clears m_prefPushed, and one store lands here
    // once there is a Renderer.
    if (renderer && !m_prefPushed)
    {
        renderer->SetPreviewPrefEnabled(m_prefEnabled);
        m_prefPushed = true;
    }

    // Auto-release. The Inspector can stop drawing the preview without reaching
    // the Close button (an entity gets selected, a scene load clears the asset,
    // the kill switch flips), and relying on each route to call NotifyClosed
    // would leak the view id. This runs before this frame's Draw (App calls
    // ProcessPendingActions in the game half, OnUIRender after the seam), so the
    // flag read here means "the panel did not draw last frame".
    if (m_viewId != ~0u && !m_visibleThisFrame) m_releaseQueued = true;
    m_visibleThisFrame = false;   // Draw re-arms it later this frame

    if (!m_releaseQueued) return;
    m_releaseQueued = false;
    if (m_viewId != ~0u && renderer)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[PREVIEW] widget released view %u at the frame boundary", m_viewId);
    }
    m_viewId = ~0u;
}

// Shutdown release. Not a second lifecycle: it does what the frame-boundary
// path does, minus the "did the panel draw" question that has no answer once
// the loop has stopped. Everything downstream (the HDR slot, the
// runtime-material references, the ToneMapPass) is given back by the same
// ReleaseView the ordinary close takes, drained by
// EditorLayer::ReleasePreviewViews.
void MaterialPreviewWidget::ReleaseViewsAtShutdown(Renderer* renderer)
{
    m_releaseQueued    = false;
    m_visibleThisFrame = false;
    if (m_viewId == ~0u) return;
    if (renderer)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[PREVIEW] widget released view %u at shutdown", m_viewId);
    }
    m_viewId = ~0u;
}

} // namespace Editor
