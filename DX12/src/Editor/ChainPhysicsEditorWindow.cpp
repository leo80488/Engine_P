// ChainPhysicsEditorWindow.cpp - the chain PARAMETER TREE of a
// ChainPhysicsComponent (KawaiiPhysics-style chain groups): the reflected
// global sliders, the authored rest pose, per-group root-bone pick + exclude
// tree + parameter overrides, Cloth-piece synchronized physics and
// Auto-detect. Part of EditorLayer, compiled separately.
//
// The Chain Physics Preview panel is the one place chains are edited: it draws
// this tree on its staged copy in Edit mode (ChainPhysicsPreviewWindow::
// DrawStagedParams) and, in Play, calls DrawChainLiveEdit (DrawChainEditorTarget:
// the live component inside DrawComponentWithUndo, transient). The Inspector's
// "Edit Chains..." opens that panel. An expanded group node writes
// m_chainHighlightGroup only when the tree describes the main overlay's own
// target (ChainParamHooks::mainOverlayTarget); OnUIRender resets it every frame.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/EditorLayer.h"
#include "Editor/ChainPhysicsPreviewWindow.h"  // the LIVE section's panel state
#include "Editor/ChainPresetLibrary.h"         // reference bands
#include "Editor/ImGuiWidgets.h"
#include "Editor/ImGuiFlow.h"            // width-aware rows
#include "Reflection/ReflectionEditor.h"   // wrapped REFLECT_INFO
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Resource/SkeletonAsset.h"
#include "Renderer/Renderer.h"
#include "Reflection/ComponentReflection.h"
#include "Resource/AnimationResource.h"    // AnimClipData / AnimationResource (rest-pose .ianim bake)
#include "Resource/AnimationSerializer.h"  // SerializeAnimation
#include "System/Log.h"
#include "System/PathUtil.h"
#include "imgui/imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <typeindex>
#include <vector>

namespace
{
// Resolve a chain carrier from a selection: follow the SkeletonRef hop (mesh
// child -> character root) and require the chain + skeleton components. The
// Play LIVE section's selection rule.
Entity ResolveChainTarget(World& world, Entity e)
{
    if (e == NullEntity || !world.IsAlive(e)) return NullEntity;
    if (const SkeletonRef* ref = world.GetComponent<SkeletonRef>(e))
        if (ref->entity != NullEntity && world.IsAlive(ref->entity))
            e = ref->entity;
    if (!world.HasComponent<ChainPhysicsComponent>(e) ||
        !world.HasComponent<SkeletonComponent>(e))
        return NullEntity;
    return e;
}

// The re-initialise marker beside a STRUCTURAL control: a small circular
// arrow drawn with the window draw list (the editor font has no arrow glyph)
// plus the tooltip. Drawn only when the caller asks (hooks.markStructural, the
// preview's staged tree).
void DrawReinitMarker(const ChainParamHooks& hooks)
{
    if (!hooks.markStructural) return;
    const float  sz = ImGui::GetTextLineHeight();
    // A width-aware tree wraps the marker rather than pushing the row past the
    // visible edge.
    if (hooks.labelColumn > 0.f) EditorUI::Flow::SameLineIfFits(sz);
    else                         ImGui::SameLine();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(sz, sz));
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    const float  r   = sz * 0.32f;
    const ImU32  col = IM_COL32(255, 190, 80, 255);
    constexpr float kA0 = 0.6f, kA1 = 5.4f;   // radians: an open ring
    dl->PathArcTo(c, r, kA0, kA1, 12);
    dl->PathStroke(col, 0, 1.5f);
    const ImVec2 tip(c.x + std::cos(kA1) * r, c.y + std::sin(kA1) * r);
    dl->AddTriangleFilled(ImVec2(tip.x + 3.5f, tip.y - 0.5f),
                          ImVec2(tip.x - 1.0f, tip.y - 3.5f),
                          ImVec2(tip.x - 0.5f, tip.y + 2.5f), col);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changing this re-initialises the chains (the topology is\n"
                          "rebuilt from the current pose - no engine hot-applies it).");
}
} // namespace

// ==== Authored rest pose: "modelled straight but must hang curled" ====
// UNDO: everything here is drawn INSIDE DrawComponentWithUndo, and that bracket
// already covers buttons: ImGui::ButtonBehavior takes ActiveId on mouse-down and
// clears it on the release frame that returns true - exactly the open/commit
// edge the bracket keys on. The registered ChainPhysics captureValue deep-copies
// the whole component, restPose included, and re-Invalidates on restore. Do not
// push an undo entry from these controls: the edit would be recorded twice and
// Ctrl+Z would need two presses.
// The viewport gizmo runs outside this bracket, which is what
// UpdateBonePoseEditTracking / PushBoneRestPoseEdit are for. The invalidate()
// calls (the caller's ChainParamHooks::invalidate) are still required: the
// bracket re-Invalidates only when an undo/redo is APPLIED, not on the edit.
void EditorLayer::DrawAuthoredRestPoseSection(ChainPhysicsComponent& comp,
                                              const SkeletonAsset& skel,
                                              const ChainParamHooks& hooks)
{
    // The entity the rows describe, and the caller's structural hook (see
    // ChainParamTree.h). Every rest-pose edit is STRUCTURAL (a rotation moves a
    // cloth piece's ring / shear rest lengths).
    const Entity entity = hooks.entity;
    auto invalidate = [&]() { if (hooks.invalidate) hooks.invalidate(); };
    using namespace DirectX;
    // THIS tree's pose-tool state (the stage's is the panel's own; the main
    // viewport's only for the overlay's target), never a global a different
    // skeleton's bone index could leak through.
    ChainPoseState& ps = PoseStateFor(hooks);
    if (ps.poseBone != ~0u && ps.poseBone >= skel.boneCount) ps.poseBone = ~0u;
    // A width-aware caller (labelColumn > 0) gets flowing rows and widths derived
    // from the column; 0 keeps the fixed widths.
    const bool flow = hooks.labelColumn > 0.f;
    auto sameLine = [flow](float nextW) { if (flow) EditorUI::Flow::SameLineIfFits(nextW); else ImGui::SameLine(); };
    namespace Flow = EditorUI::Flow;

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "Authored rest pose  (%d/%d bones)###restpose",
             static_cast<int>(comp.restPose.size()),
             ChainPhysicsComponent::MAX_REST_POSE_BONES);
    if (hooks.expandAll) ImGui::SetNextItemOpen(true, ImGuiCond_Always);   // UI layout harness
    if (!ImGui::CollapsingHeader(hdr)) return;

    ImGui::Indent();

    // Groups can be removed / the target re-latched while this index is held, and
    // a stale out-of-range value would disable every pose tool while the combo
    // below still displays "All groups".
    if (ps.focusGroup >= static_cast<int>(comp.groups.size()))
        ps.focusGroup = -1;

    ChainPhysicsSystem* cps = m_renderer ? m_renderer->GetChainPhysicsSystem() : nullptr;
    // Per ENTITY: the Chain Physics Preview panel's stage simulates on its own
    // lane while the editor is Stopped, where the transport-wide flag reads false.
    const bool simRunning = cps && cps->IsSimulating(entity);

    {
        static const char* kWhat =
            "Holds these bones at an authored rotation instead of the modelled bind "
            "pose. The chain still swings from body motion and relaxes back to THIS "
            "shape, because the solver derives every rest target from the same pose.";
        if (flow) Flow::Wrapped("%s", kWhat);
        else      ImGui::TextWrapped("%s", kWhat);
    }

    // ---- Viewport posing toggle ----
    if (ImGui::Checkbox(hooks.mainOverlayTarget ? "Pose bones in main viewport"
                                                : "Pose bones in preview", &ps.poseMode))
    {
        // The main viewport's picker and overlay share one gate: only when this tree
        // IS that overlay's target.
        if (ps.poseMode && hooks.mainOverlayTarget)
            m_chainOverlayEnabled = true;
        else if (!ps.poseMode)
            ps.poseBone = ~0u;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(hooks.mainOverlayTarget
            ? "Click a chain bone in the viewport to select it, then drag the "
              "rotation ring.\nNot available in F11 fullscreen (that path has no gizmo)."
            : "Select a bone (a row's Select button, or click it in the preview "
              "picture), then drag its rotation ring in the preview.");

    if (ps.poseMode)
    {
        // Focus group. ps.focusGroup, NOT m_chainHighlightGroup: the
        // latter is reset unconditionally at the top of OnUIRender and only
        // re-asserted while a tree draws, so it always reads -1 by the time
        // the viewport draws.
        std::string preview = (ps.focusGroup >= 0 &&
                               ps.focusGroup < static_cast<int>(comp.groups.size()))
            ? (comp.groups[ps.focusGroup].name[0]
                   ? comp.groups[ps.focusGroup].name : "(chain)")
            : "All groups";
        if (ImGui::BeginCombo("Focus group", preview.c_str()))
        {
            if (ImGui::Selectable("All groups", ps.focusGroup < 0))
                ps.focusGroup = -1;
            for (int gi = 0; gi < static_cast<int>(comp.groups.size()); ++gi)
            {
                char lbl[112];
                snprintf(lbl, sizeof(lbl), "%s  |  %s##pf%d",
                         comp.groups[gi].name[0] ? comp.groups[gi].name : "(chain)",
                         comp.groups[gi].rootBone[0] ? comp.groups[gi].rootBone : "(no root)",
                         gi);
                if (ImGui::Selectable(lbl, ps.focusGroup == gi))
                    ps.focusGroup = gi;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restricts which bones are drawn and clickable.\n"
                              "A character with 20 hair chains is unusable without this.");

        if (ps.poseBone < skel.boneCount)
        {
            if (flow) Flow::Wrapped("Selected bone: %s", skel.boneNames[ps.poseBone]);
            else      ImGui::Text("Selected bone: %s", skel.boneNames[ps.poseBone]);
        }
        else
        {
            const char* none = hooks.mainOverlayTarget
                               ? "Selected bone: (click one in the viewport)"
                               : "Selected bone: (none - use a row's Select)";
            if (flow) Flow::DisabledWrapped("%s", none);
            else      ImGui::TextDisabled("%s", none);
        }
    }

    // ---- Master switches ----
    {
        bool en = comp.restPoseEnabled;
        if (ImGui::Checkbox("Enabled##restpose", &en))
        {
            comp.restPoseEnabled = en;
            invalidate();
        }
        DrawReinitMarker(hooks);
    }
    if (!flow) ImGui::SetNextItemWidth(200.f);   // flow: the tree's pushed width
    ImGui::SliderFloat("Weight##restpose", &comp.restPoseWeight, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        invalidate();
        // The slider itself is inside DrawComponentWithUndo's active-id bracket,
        // so it already records; the Invalidate is what this hook is for.
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fade from the MODELLED shape to the authored shape.\n"
                          "NOT an animation-vs-authored blend: the blend base is\n"
                          "skel.restPoseLocal, because using the live pose as the\n"
                          "base makes a partial weight creep to 1 over a few seconds.");

    // ---- Add / capture / mirror / clear ----
    const bool full = comp.restPose.size() >=
                      static_cast<size_t>(ChainPhysicsComponent::MAX_REST_POSE_BONES);

    ImGui::BeginDisabled(full || ps.poseBone >= skel.boneCount);
    if (ImGui::Button("Add selected bone"))
    {
        if (m_world) EnsureBoneRestPoseEntry(comp, skel, *m_world, entity, ps.poseBone);
        invalidate();
    }
    ImGui::EndDisabled();

    sameLine(Flow::ButtonW("Capture current pose"));
    ImGui::BeginDisabled(simRunning);
    if (ImGui::Button("Capture current pose"))
    {
        std::vector<uint8_t> pickable;
        CollectChainBones(skel, comp, ps.focusGroup, pickable);
        int added = 0, skipped = 0;
        for (uint32_t b = 0; b < skel.boneCount && b < pickable.size(); ++b)
        {
            if (!pickable[b]) continue;
            if (!m_world) break;
            // reseedExisting: capture MEANS overwrite, otherwise re-capturing
            // after moving the character silently keeps the old rotations.
            if (!EnsureBoneRestPoseEntry(comp, skel, *m_world, entity, b, true))
            { ++skipped; continue; }
            ++added;
        }
        if (skipped > 0)
            LOG_WARNING("ChainPhysics: rest-pose capture stopped at the %d-bone cap "
                        "(%d captured, %d skipped)",
                        ChainPhysicsComponent::MAX_REST_POSE_BONES, added, skipped);
        invalidate();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(simRunning
            ? "Disabled while the simulation runs: WriteBonePoses REPLACES the\n"
              "rotation of every mid-chain bone, so this would bake the current\n"
              "SWING into the authored rest instead of the pose you see at rest."
            : "Snapshot every bone in the focus group at its current rotation.");

    sameLine(Flow::ButtonW("Mirror L/R"));
    if (ImGui::Button("Mirror L/R"))
    {
        // Name substitution + component flip. PMX rigs use the Japanese left / right
        // characters as prefixes, most FBX rigs _L/_R or Left/Right; try each in turn.
        struct Pair { const char* a; const char* b; };
        static const Pair kPairs[] = {
            { "\xE5\xB7\xA6", "\xE5\x8F\xB3" },  // Japanese left / right (U+5DE6 / U+53F3)
            { "Left",  "Right" }, { "left",  "right" },
            { "_L",    "_R"    }, { "_l",    "_r"    },
        };
        // "_L"/"_R" are the dangerous pair: a bare substring match rewrites
        // "Spine_Lower" to "Spine_Rower". Require the token to end the name or be
        // followed by a non-letter (so "Hair_L", "Hair_L_02" match, "_Lower" does
        // not). The word-shaped pairs above it are matched plainly.
        auto tokenOk = [](const char* p, const char* tok, bool strict) -> bool {
            if (!strict) return true;
            const char c = p[std::strlen(tok)];
            return c == '\0' || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        };
        auto mirrorName = [&](const char* src, char* dst, size_t n) -> bool {
            for (const Pair& p : kPairs)
            {
                const bool strict = (p.a[0] == '_');
                const char* hit = nullptr;
                const char* tok = nullptr;
                const char* rep = nullptr;
                for (const char* q = std::strstr(src, p.a); q; q = std::strstr(q + 1, p.a))
                    if (tokenOk(q, p.a, strict)) { hit = q; tok = p.a; rep = p.b; break; }
                if (!hit)
                    for (const char* q = std::strstr(src, p.b); q; q = std::strstr(q + 1, p.b))
                        if (tokenOk(q, p.b, strict)) { hit = q; tok = p.b; rep = p.a; break; }
                if (!hit) continue;
                const size_t pre = static_cast<size_t>(hit - src);
                snprintf(dst, n, "%.*s%s%s", static_cast<int>(pre), src, rep,
                         hit + std::strlen(tok));
                return true;
            }
            return false;
        };
        const size_t original = comp.restPose.size();
        int made = 0, refused = 0;
        for (size_t i = 0; i < original; ++i)
        {
            char mn[64];
            if (!mirrorName(comp.restPose[i].boneName, mn, sizeof(mn))) continue;
            bool exists = false;
            for (const auto& r : comp.restPose)
                if (std::strcmp(r.boneName, mn) == 0) { exists = true; break; }
            if (exists) continue;
            if (comp.restPose.size() >=
                static_cast<size_t>(ChainPhysicsComponent::MAX_REST_POSE_BONES))
            { ++refused; continue; }
            BoneRestPose m = comp.restPose[i];
            m.cachedBoneIdx = ~0u; m.resolvedSkel = 0xFFFFFFFFu;
            m.resolvedName[0] = '\0'; m.resolveFailed = false;
            m.warnedMissing = false; m.appliedThisFrame = false;
            snprintf(m.boneName, sizeof(m.boneName), "%s", mn);
            // Mirror across YZ: negate the components that carry the X axis.
            m.rot.y = -m.rot.y;
            m.rot.z = -m.rot.z;
            comp.restPose.push_back(m);
            ++made;
        }
        if (refused > 0)
            LOG_WARNING("ChainPhysics: mirror hit the %d-bone cap (%d mirrored, %d refused)",
                        ChainPhysicsComponent::MAX_REST_POSE_BONES, made, refused);
        invalidate();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Duplicates each entry onto its L/R counterpart\n"
                          "(左/右, Left/Right, _L/_R), mirrored across YZ.");

    if (!comp.restPose.empty())
    {
        sameLine(Flow::ButtonW("Clear all##restpose"));
        if (ImGui::Button("Clear all##restpose"))
        {
            comp.restPose.clear();
            invalidate();
        }

        // ---- Export as a 1-frame .ianim ----
        // Pure EXPORT, not a runtime path: it hands the pose to DCC round-trips and to
        // anything that consumes clips. The runtime source of truth stays this
        // component (per-instance, and immune to the AnimLayer gates).
        if (ImGui::Button("Bake rest pose to .ianim..."))
        {
            {
                Resource::AnimationResource res;
                Resource::AnimClipData clip;
                snprintf(clip.name, sizeof(clip.name), "RestPose");
                clip.frameCount = 1;
                // duration must NOT be 0: a looping layer would then hit fmod(t, 0), the
                // NaN-pose path. One frame at 30 fps.
                clip.frameRate  = 30.f;
                clip.duration   = 1.f / 30.f;
                for (const BoneRestPose& r : comp.restPose)
                {
                    if (!r.enabled || !r.boneName[0]) continue;
                    uint32_t bi = ~0u;
                    for (uint32_t b = 0; b < skel.boneCount; ++b)
                        if (std::strcmp(skel.boneNames[b], r.boneName) == 0) { bi = b; break; }
                    if (bi == ~0u) continue;

                    Resource::AnimClipChannel ch;
                    snprintf(ch.name, sizeof(ch.name), "%s", r.boneName);
                    // ABSOLUTE local transform, matching the FBX/GLTF convention
                    // (positionsAreOffsets / rotationsAreOffsets stay false).
                    // Translation and scale come from the modelled rest, since the
                    // authored pose is rotation-only by construction.
                    XMVECTOR s, q, t;
                    XMFLOAT3 pos{ 0.f, 0.f, 0.f }, scl{ 1.f, 1.f, 1.f };
                    if (XMMatrixDecompose(&s, &q, &t, XMLoadFloat4x4(&skel.restPoseLocal[bi])))
                    { XMStoreFloat3(&pos, t); XMStoreFloat3(&scl, s); }
                    ch.positions.push_back(pos);
                    ch.rotations.push_back(r.rot);
                    ch.scales.push_back(scl);
                    clip.channels.push_back(std::move(ch));
                }
                if (clip.channels.empty())
                {
                    LOG_WARNING("ChainPhysics: nothing to bake — no enabled rest-pose "
                                "entry resolved against this skeleton.");
                }
                else
                {
                    const size_t chCount = clip.channels.size();
                    res.clips.push_back(std::move(clip));
                    const std::vector<uint8_t> blob = Resource::SerializeAnimation(res);
                    std::string savePath;
                    if (SaveBlobWithDialog(
                            "Animation (*.ianim)\0*.ianim\0All Files\0*.*\0\0",
                            "ianim", "asset/", blob, &savePath))
                        LOG_SUCCESS("ChainPhysics: baked rest pose -> '%s' (%zu bytes, %zu channels)",
                                    Sys::RelativizeAssetPath(savePath).c_str(),
                                    blob.size(), chCount);
                    else if (!savePath.empty())
                        LOG_ERROR("ChainPhysics: failed to write '%s'", savePath.c_str());
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Export only. One frame at 30 fps (duration 1/30, never 0 —\n"
                              "a looping layer on a zero-duration clip produces a NaN pose).\n"
                              "If you play it back as an AnimLayer, leave Looping OFF.");
    }

    // ---- Entry list ----
    int removeIdx = -1;
    for (int i = 0; i < static_cast<int>(comp.restPose.size()); ++i)
    {
        BoneRestPose& r = comp.restPose[i];
        ImGui::PushID(i);

        uint32_t bi = ~0u;
        for (uint32_t b = 0; b < skel.boneCount; ++b)
            if (std::strcmp(skel.boneNames[b], r.boneName) == 0) { bi = b; break; }

        const bool isSel = (bi != ~0u && bi == ps.poseBone);
        if (bi == ~0u)
        {
            // Unresolved. A retarget renames every bone at once and turns the
            // whole list into dead data whose only signal is a line in the log,
            // so it has to be loud and fixable here.
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "! %s", r.boneName);
            if (flow)
            {
                // the field takes what is left of the row beside the "X" button
                const float xW = Flow::ButtonW("X") + ImGui::GetStyle().ItemSpacing.x;
                Flow::SameLineIfFits(120.f + xW);
                ImGui::SetNextItemWidth(std::max(60.f, std::min(180.f, ImGui::GetContentRegionAvail().x - xW)));
            }
            else
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(180.f);
            }
            ImGui::InputText("##remap", r.boneName, sizeof(r.boneName));
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                r.resolvedSkel = 0xFFFFFFFFu;   // force a re-resolve next frame
                r.resolveFailed = false;
                invalidate();
            }
            sameLine(Flow::ButtonW("X"));
            if (ImGui::SmallButton("X")) removeIdx = i;
            ImGui::PopID();
            continue;
        }

        if (isSel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.3f, 1.f));
        // A strand TIP (and a non-rootSolo strand root) is neither simulated nor
        // reset, so an authored rotation there is permanent and invisible to the
        // solver.
        ImGui::Text("%s", skel.boneNames[bi]);
        if (isSel) ImGui::PopStyleColor();
        sameLine(Flow::ButtonW("Select"));
        if (ImGui::SmallButton("Select"))
        {
            ps.poseBone = bi;
            ps.poseMode = true;
            if (hooks.mainOverlayTarget) m_chainOverlayEnabled = true;   // only the main overlay's target arms it
        }
        sameLine(Flow::ButtonW("Reset"));
        if (ImGui::SmallButton("Reset"))
        {
            XMVECTOR s, q, t;
            if (XMMatrixDecompose(&s, &q, &t, XMLoadFloat4x4(&skel.restPoseLocal[bi])))
                XMStoreFloat4(&r.rot, XMQuaternionNormalize(q));
            invalidate();
        }
        sameLine(Flow::ButtonW("Remove"));
        if (ImGui::SmallButton("Remove")) removeIdx = i;

        ImGui::Indent();
        // Each of these needs a live Invalidate: for a CLOTH group the authored
        // rotation moves the CROSS-STRAND ring/shear rest lengths that InitEntity
        // baked (Constraint::restLength is a particle-to-particle world distance,
        // not a bone length), so without a re-bake the piece fights its own new
        // rest shape. A pure Chain group would not need it; the editor does not
        // know which group a bone belongs to here, and re-baking is cheap.
        if (ImGui::Checkbox("On", &r.enabled)) { invalidate(); }
        sameLine(Flow::LabeledW(60.f, "W"));
        ImGui::SetNextItemWidth(flow ? Flow::FitWidth(110.f, "W", 40.f) : 110.f);
        ImGui::SliderFloat("W", &r.weight, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) { invalidate(); }

        // Euler view of the ABSOLUTE local rotation. Stored as a quaternion; the
        // widget converts both ways, so gimbal-flipped readouts are cosmetic.
        XMFLOAT3 eul{};
        {
            const XMVECTOR q = XMQuaternionNormalize(XMLoadFloat4(&r.rot));
            XMFLOAT4 v; XMStoreFloat4(&v, q);
            const float sinp = 2.f * (v.w * v.x - v.y * v.z);
            eul.x = std::fabs(sinp) >= 1.f ? std::copysign(1.5707963f, sinp) : std::asin(sinp);
            eul.y = std::atan2(2.f * (v.w * v.y + v.z * v.x),
                               1.f - 2.f * (v.x * v.x + v.y * v.y));
            eul.z = std::atan2(2.f * (v.w * v.z + v.x * v.y),
                               1.f - 2.f * (v.x * v.x + v.z * v.z));
            eul.x *= 57.29578f; eul.y *= 57.29578f; eul.z *= 57.29578f;
        }
        if (!flow) ImGui::SetNextItemWidth(240.f);   // flow: the tree's pushed width
        if (ImGui::DragFloat3("XYZ (deg)", &eul.x, 0.5f, -180.f, 180.f, "%.1f"))
        {
            const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                eul.x * 0.01745329252f, eul.y * 0.01745329252f, eul.z * 0.01745329252f);
            XMStoreFloat4(&r.rot, XMQuaternionNormalize(q));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { invalidate(); }
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (removeIdx >= 0 && removeIdx < static_cast<int>(comp.restPose.size()))
    {
        comp.restPose.erase(comp.restPose.begin() + removeIdx);
        invalidate();
    }

    // ---- Why an authored curl can read as "nothing happened" ----
    // All three of these gate the local pull that turns the authored pose into an
    // equilibrium, and all three default to values that make a strong curl look
    // ignored at the tip.
    {
        const ChainGroupDef* fg =
            (ps.focusGroup >= 0 &&
             ps.focusGroup < static_cast<int>(comp.groups.size()))
                ? &comp.groups[ps.focusGroup] : nullptr;
        const bool cloth = fg && fg->type == ChainGroupType::Cloth;
        const float ls = (fg && fg->ovrEnabled) ? fg->ovrLocalStiffness
                       : (cloth ? comp.skirtLocalStiffness : comp.localStiffness);
        const float th = (fg && fg->ovrEnabled) ? fg->ovrTipHold
                       : (cloth ? comp.skirtTipHold : comp.tipHold);
        const float la = (fg && fg->ovrEnabled) ? fg->ovrLimitAngle
                       : (cloth ? comp.skirtLimitAngle : comp.limitAngle);

        ImGui::Separator();
        static const char* kLs0 =
            "localStiffness is 0 — the pull toward the authored shape is skipped\n"
            "entirely, so gravity alone decides where the chain hangs.";
        static const char* kTh0 =
            "tipHold is 0 — the hold falls off as (1-t)^2, so the TIP is fully\n"
            "free and a strong curl will read as \"nothing happened\" there.\n"
            "Raise it (0.2-0.4) to keep the authored silhouette.";
        if (flow)
        {
            Flow::DisabledWrapped("Hold-shape (focus group): localStiffness %.3f | tipHold %.2f | limitAngle %.0f",
                                  ls, th, la);
            if (ls <= 0.f)      Flow::ColoredWrapped(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s", kLs0);
            else if (th <= 0.f) Flow::ColoredWrapped(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", kTh0);
        }
        else
        {
            ImGui::TextDisabled("Hold-shape (focus group): localStiffness %.3f | tipHold %.2f | limitAngle %.0f",
                                ls, th, la);
            if (ls <= 0.f)      ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s", kLs0);
            else if (th <= 0.f) ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", kTh0);
        }
    }

    ImGui::Unindent();
    ImGui::Separator();
}

// ==== Pose-tool state of the tree being drawn ====
ChainPoseState& EditorLayer::PoseStateFor(const ChainParamHooks& hooks)
{
    const bool main = hooks.pose == nullptr && hooks.mainOverlayTarget;
    ChainPoseState& ps = hooks.pose ? *hooks.pose : main ? m_chainMainPose : m_chainScratchPose;
    if (ps.owner != hooks.entity)
    {
        // A bone index names ONE skeleton: a different entity starts clean.
        // (The main viewport's arm switch is the Inspector's - it survives.)
        const bool keepMode = main && ps.poseMode;
        ps.Reset(hooks.entity);
        ps.poseMode = keepMode;
    }
    return ps;
}

// A SCENE entity's live parameters: the body of the preview panel's Play-mode
// "LIVE" section (DrawChainLiveEdit). The preview stage is edited by the
// panel's own tree on m_staged, never here. The harness probes drive this
// code; `probeEdit` (harness only, empty otherwise) is handed the component
// after the tree, inside the same undo bracket.
void EditorLayer::DrawChainEditorTarget(World& worldRef, Renderer& rendererRef, Entity entity,
                                        const SkeletonAsset& skelRef,
                                        const std::function<void(ChainPhysicsComponent&)>& probeEdit)
{
    World*    world    = &worldRef;
    Renderer* renderer = &rendererRef;
    const SkeletonAsset* skel = &skelRef;

    // The preview STAGE is edited by the panel's own tree only: the panel's sync
    // is the one writer of its live component.
    if (world->HasComponent<PreviewStageTag>(entity) ||
        !world->HasComponent<ChainPhysicsComponent>(entity))
    {
        ImGui::TextDisabled("(not a scene chain carrier)");
        return;
    }

    ImGui::Checkbox("Show chain overlay in viewport", &m_chainOverlayEnabled);
    ImGui::Separator();

    // Undo bracketing, the same machinery as the Inspector dispatch: the
    // ChainPhysics entry carries the custom capture that re-Invalidates the
    // simulation on undo/redo restore. In Play UndoEnabled() is false - the edit
    // is transient and Stop's AdoptState reverts it.
    Editor::Undo::CaptureFn                    capture;
    std::function<std::string(World&, Entity)> fingerprint;
    {
        const auto it = m_componentEditors.find(
            std::type_index(typeid(ChainPhysicsComponent)));
        if (it != m_componentEditors.end())
        {
            capture     = it->second.captureValue;
            fingerprint = it->second.fingerprint;
        }
    }

    DrawComponentWithUndo(entity, std::type_index(typeid(ChainPhysicsComponent)),
        capture, fingerprint, [&]()
    {
        auto& comp = *world->GetComponent<ChainPhysicsComponent>(entity);
        // A scene entity: structural controls Invalidate the live entity on the spot,
        // inside the undo bracket.
        ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem();
        ChainParamHooks hooks;
        hooks.entity     = entity;
        hooks.invalidate = [cps, entity]() { if (cps) cps->Invalidate(entity); };
        // The main overlay's highlight / arm switch only when this IS that overlay's
        // target (the selection).
        hooks.mainOverlayTarget = ChainOverlayCandidate(*world) == entity;
        // The LIVE body is drawn in the preview panel's resizable parameter column,
        // width-aware like the staged tree.
        hooks.labelColumn = EditorUI::Flow::LabelColumn();
        DrawChainParamTree(comp, *skel, hooks);
        if (probeEdit) probeEdit(comp);   // the harness probe's edit
    });
}

// The LIVE section's target, in the order below. Every candidate must be a
// scene chain carrier (never the stage).
Entity EditorLayer::ResolveChainLiveTarget(const char** outWhy) const
{
    const char* dummy = nullptr;
    const char*& why = outWhy ? *outWhy : dummy;
    why = "none";
    if (!m_world) return NullEntity;
    World& w = *m_world;
    const auto& panel = Editor::ChainPhysicsPreviewWindow::Get();
    auto ok = [&](Entity e)
    {
        return e != NullEntity && w.IsAlive(e) && !w.HasComponent<PreviewStageTag>(e) &&
               w.HasComponent<ChainPhysicsComponent>(e) && w.HasComponent<SkeletonComponent>(e);
    };
    // 1. the Inspector's "Edit Chains..." pressed during this Play session
    if (const EntityHandle r = panel.LiveRequest(); w.IsHandleValid(r) && ok(r.entity))
    { why = "inspector"; return r.entity; }
    // 2. the panel's Scene target (Use Selected's source; kept across the
    //    Play snapshot - ids and generations are the same world's)
    if (const EntityHandle t = panel.SceneTargetHandle(); w.IsHandleValid(t))
        if (const Entity c = panel.SceneCarrierOf(w, t.entity); ok(c))
        { why = "scene target"; return c; }
    // 3. the selection (a mesh child hops to its character)
    if (const Entity s = ResolveChainTarget(w, m_selectedEntity); ok(s))
    { why = "selection"; return s; }
    if (m_selectedEntity != NullEntity && w.IsAlive(m_selectedEntity))
        if (const Entity c = panel.SceneCarrierOf(
                w, Editor::ChainPhysicsPreviewWindow::SceneRootOf(w, m_selectedEntity)); ok(c))
        { why = "selection"; return c; }
    // 4. the first scene instance of the subject prefab
    if (!panel.SubjectInPlace() && !panel.SubjectPath().empty())
        if (auto* pp = w.GetPool<PrefabSourcePath>())
        {
            auto norm = [](const std::string& p)
            {
                std::string r = Sys::RelativizeAssetPath(p);
                if (r.empty()) r = p;
                for (char& ch : r)
                {
                    if (ch == '\\') ch = '/';
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                }
                return r;
            };
            const std::string want = norm(panel.SubjectPath());
            auto& ents = pp->Entities();
            auto& data = pp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
            {
                if (data[i].path.empty() || norm(data[i].path) != want) continue;
                if (const Entity c = panel.SceneCarrierOf(w, ents[i]); ok(c))
                { why = "subject instance"; return c; }
            }
        }
    return NullEntity;
}

// The Play-mode Parameters body of the Chain Physics Preview panel.
// World-touching, so it is EditorLayer code the panel only calls (its Draw
// stays World-free).
void EditorLayer::DrawChainLiveEdit(const std::function<void(ChainPhysicsComponent&)>& probeEdit)
{
    auto& panel = Editor::ChainPhysicsPreviewWindow::Get();
    if (!m_world || !m_renderer)
    {
        ImGui::TextDisabled("(no world)");
        return;
    }
    const char* why = "";
    const Entity carrier = ResolveChainLiveTarget(&why);
    namespace Flow = EditorUI::Flow;   // the panel's column
    if (panel.HasPlayKeep())
    {
        Flow::ColoredWrapped(ImVec4(0.55f, 0.85f, 0.55f, 1.f),
                             "Kept: '%s' - staged in the preview after Stop.",
                             panel.PlayKeepLabel().c_str());
        Flow::SameLineIfFits(Flow::ButtonW("Forget##cpv_keep"));
        if (ImGui::SmallButton("Forget##cpv_keep")) panel.ForgetPlayKeep();
    }
    if (carrier == NullEntity)
    {
        Flow::Wrapped("LIVE editing: no scene character. Select one, or press "
                      "\"Edit Chains...\" on its Chain Physics component.");
        return;
    }
    const Entity root = Editor::ChainPhysicsPreviewWindow::SceneRootOf(*m_world, carrier);
    Flow::ColoredWrapped(ImVec4(1.f, 0.55f, 0.35f, 1.f), "LIVE: %s  (#%u)",
                         m_world->GetName(root).c_str(), static_cast<unsigned>(carrier));
    Flow::DisabledWrapped("(transient - reverted on Stop; target from %s)", why);
    if (ImGui::Button("Keep these values")) KeepChainLiveValues(carrier);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stop reverts every Play-time edit. This keeps a copy of the values NOW:\n"
                          "after Stop the preview stages them (the character becomes the preview's\n"
                          "subject first if it is not) - one undoable step that still needs Apply.\n"
                          "A prefab instance keeps its physics material only (its rest pose is\n"
                          "saved per instance). Press again after further edits.");
    const SkeletonComponent* skc = m_world->GetComponent<SkeletonComponent>(carrier);
    if (!skc || skc->assetIndex == kInvalidSkeletonIndex ||
        skc->assetIndex >= m_renderer->GetSkeletonRegistry().Count())
    {
        ImGui::TextDisabled("(entity has no skeleton asset)");
        return;
    }
    DrawChainEditorTarget(*m_world, *m_renderer, carrier,
                          m_renderer->GetSkeletonRegistry().Get(skc->assetIndex), probeEdit);
}

void EditorLayer::KeepChainLiveValues(Entity carrier)
{
    if (!m_world || carrier == NullEntity || !m_world->IsAlive(carrier)) return;
    const ChainPhysicsComponent* c = m_world->GetComponent<ChainPhysicsComponent>(carrier);
    if (!c) return;
    const Entity root = Editor::ChainPhysicsPreviewWindow::SceneRootOf(*m_world, carrier);
    const PrefabSourcePath* psp = m_world->GetComponent<PrefabSourcePath>(root);
    // A prefab-REF instance keeps the physics MATERIAL only (Apply's rule: its
    // rest pose is a per-instance CPR line, never the prefab's).
    const bool materialOnly = psp && !psp->path.empty();
    Editor::ChainPhysicsPreviewWindow::Get().KeepLiveValues(
        *c, materialOnly, m_world->MakeHandle(root), m_world->MakeHandle(carrier),
        m_world->GetName(root), psp ? psp->path : std::string());
}

// ==== DrawChainParamTree ====
// The chain parameter tree: globals, rest pose, the Chain / Cloth / Spring
// group panels, auto-detect.
void EditorLayer::DrawChainParamTree(ChainPhysicsComponent& comp,
                                     const SkeletonAsset& skelAsset,
                                     const ChainParamHooks& hooks)
{
    const SkeletonAsset* skel = &skelAsset;

    // Every STRUCTURAL control calls this. The caller decides what it means:
    // Invalidate(entity) on a scene entity, or "raise the panel's structural flag"
    // on the preview's staged copy (the panel Invalidates in its serial window;
    // the tree never touches the solver).
    auto invalidate = [&]() { if (hooks.invalidate) hooks.invalidate(); };
    auto marker = [&]() { DrawReinitMarker(hooks); };
    // A width-aware caller keeps `labelColumn` px free for the labels. ONE
    // PushItemWidth for the whole body (no early return; the matching Pop is at
    // the end) - PushItemWidth, not SetNextItemWidth, because
    // ChainPresets::DrawBand re-reads the slider's width with CalcItemWidth().
    const bool flow = hooks.labelColumn > 0.f;
    namespace Flow = EditorUI::Flow;
    if (flow) ImGui::PushItemWidth(-hooks.labelColumn);
    // A structural InputInt with a marker: the marker needs its own room.
    auto fitMarked = [&](const char* label)
    {
        if (!flow) return;
        const float mk = hooks.markStructural
                       ? ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x : 0.f;
        const float room = ImGui::GetContentRegionAvail().x - mk -
                           ImGui::GetStyle().ItemInnerSpacing.x - Flow::TextW(label);
        ImGui::SetNextItemWidth(std::max(60.f, room));
    };
    // The reference band (typical range) and the PBD-saturation veil, painted
    // onto the slider just drawn: the panel's STAGED tree only
    // (hooks.referenceBands), no layout change.
    auto band = [&](const char* member, bool cloth, float v, float lo, float hi)
    {
        if (!hooks.referenceBands) return;
        namespace CPS = Editor::ChainPresets;
        const CPS::Target t = cloth ? CPS::Target::Cloth : CPS::Target::Chain;
        const CPS::FieldSpec* f = CPS::FindByMember(member, t);
        bool covered = false;
        if (f && hooks.coveredFields)
            for (const std::string& k : *hooks.coveredFields) covered |= (k == f->key);
        CPS::DrawBand(member, t, v, lo, hi, covered);
    };

    // World-motion inertia: a group's OWN share of the entity's motion
    // (ChainGroupDef::ovrWorldInertia*, < 0 = follow the component's World
    // Motion values). Drawn OUTSIDE the override block, like "Collide with
    // body": the authored rigs override every group, and an ovr-gated value
    // would leave the entity-level slider dead on them. Live, no re-init.
    auto worldInertiaRow = [&](ChainGroupDef& wg, const char* id) -> bool
    {
        bool changed = false;
        ImGui::PushID(id);
        bool own = wg.ovrWorldInertiaLocation >= 0.f || wg.ovrWorldInertiaRotation >= 0.f;
        if (flow ? Flow::Checkbox("Own world inertia", &own)
                 : ImGui::Checkbox("Own world inertia", &own))
        {
            // Entering: start from the value it followed, so nothing jumps.
            wg.ovrWorldInertiaLocation = own ? comp.worldInertiaLocation : -1.f;
            wg.ovrWorldInertiaRotation = own ? comp.worldInertiaRotation : -1.f;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: this group feels the ENTITY's motion by World Motion (entity)\n"
                              "above. On: its own share - e.g. 0 for a stiff ornament, 1 for a\n"
                              "long cape. 0 = rides the entity rigidly, 1 = physical.");
        if (own)
        {
            if (wg.ovrWorldInertiaLocation < 0.f) wg.ovrWorldInertiaLocation = comp.worldInertiaLocation;
            if (wg.ovrWorldInertiaRotation < 0.f) wg.ovrWorldInertiaRotation = comp.worldInertiaRotation;
            changed |= EditorUI::DragFloat("World inertia: location", &wg.ovrWorldInertiaLocation,
                                           0.f, 1.f, "%.2f");
            changed |= EditorUI::DragFloat("World inertia: rotation", &wg.ovrWorldInertiaRotation,
                                           0.f, 1.f, "%.2f");
        }
        else if (flow)
            Flow::DisabledWrapped("World inertia: follows the entity (location %.2f, rotation %.2f)",
                                  comp.worldInertiaLocation, comp.worldInertiaRotation);
        else
            ImGui::TextDisabled("World inertia: follows the entity (location %.2f, rotation %.2f)",
                                comp.worldInertiaLocation, comp.worldInertiaRotation);
        ImGui::PopID();
        return changed;
    };

    // bone NAME -> index (linear scan; authoring only, never per-frame hot).
    auto resolveBone = [&](const char* nm) -> uint32_t {
        if (!nm || !nm[0]) return ~0u;
        for (uint32_t b = 0; b < skel->boneCount; ++b)
            if (std::strcmp(skel->boneNames[b], nm) == 0) return b;
        return ~0u;
    };
    // case-insensitive substring (self-contained - no <cctype>/<algorithm>).
    auto icontains = [](const char* hay, const char* needle) -> bool {
        if (!needle || !needle[0]) return true;
        for (const char* h = hay; *h; ++h) {
            const char* a = h; const char* b = needle;
            while (*a && *b) {
                char ca = *a, cb = *b;
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) break;
                ++a; ++b;
            }
            if (!*b) return true;
        }
        return false;
    };

    // children[] / roots[] for the bone trees.
    std::vector<std::vector<uint32_t>> children(skel->boneCount);
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < skel->boneCount; ++i) {
        if (skel->parentIndex[i] < 0) roots.push_back(i);
        else children[static_cast<uint32_t>(skel->parentIndex[i])].push_back(i);
    }

    // ---- Global parameters (reflected sliders) ----
    if (hooks.expandAll) ImGui::SetNextItemOpen(true, ImGuiCond_Always);   // UI layout harness
    if (ImGui::CollapsingHeader("Global parameters"))
    {
        ChainPhysicsComponent* compPtr = &comp;
        if (flow) Reflect::PushInfoWrap();   // REFLECT_INFO wraps in the column
        if (hooks.expandAll) Reflect::SetForceOpenSections(true);
        Reflect::DrawObject(compPtr);
        if (hooks.expandAll) Reflect::SetForceOpenSections(false);
        if (flow) Reflect::PopInfoWrap();
        // The reflected sliders are drawn generically, so their bands (and the active
        // preset's coverage) are one table.
        if (hooks.referenceBands)
            Editor::ChainPresets::DrawGlobalBands(comp, hooks.coveredFields);
    }

    // Global cloth collision thickness. Live - no Invalidate. Added to every
    // capsule's radius so the skinned cloth surface (offset from the bone) clears
    // the body, not just the bone centerline. Per-group override lives in each
    // chain's "Override global params" block.
    EditorUI::DragFloat("Cloth thickness (collision)", &comp.collisionPadding, 0.f, 0.3f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Extra collision radius for ALL cloth on this character.\n"
                          "Adds to each capsule's radius so the cloth rests a\n"
                          "thickness OFF the body. 0 = old behaviour. Try 0.02-0.05.");

    if (comp.groups.empty())
    {
        static const char* kNoChains =
            "No authored chains — using legacy bone-name auto-detection.\n"
            "Add a chain or press Auto-detect to take explicit control.";
        if (flow) Flow::DisabledWrapped("%s", kNoChains);
        else      ImGui::TextDisabled("%s", kNoChains);
    }

    static const char* kTypeNames[] = { "Chain", "Cloth", "Spring" };

    DrawAuthoredRestPoseSection(comp, *skel, hooks);

    // Draw one group's editable body (root picker + ring/pair ids +
    // exclude tree). Returns true if the user clicked Remove. Shared by
    // all three type panels below; the group's TYPE is fixed by the panel
    // it lives in, so there is no per-group type combo here.
    auto drawGroupBody = [&](ChainGroupDef& g) -> bool
    {
        const uint32_t rootIdx = resolveBone(g.rootBone);

        if (ImGui::Checkbox("Enabled", &g.enabled)) invalidate();
        marker();
        ImGui::InputText("Name", g.name, sizeof(g.name));

        // ---- Root bone picker ----
        if (flow) Flow::Wrapped("Root: %s", g.rootBone[0] ? g.rootBone : "(none)");
        else      ImGui::Text("Root: %s", g.rootBone[0] ? g.rootBone : "(none)");
        if (flow) Flow::SameLineIfFits(Flow::ButtonW("Pick..."));
        else      ImGui::SameLine();
        const bool pickRoot = ImGui::Button("Pick...");
        marker();
        if (pickRoot)
        {
            // The filter resets on every open: one buffer serves every group and entity.
            m_chainBoneFilter[0] = '\0';
            ImGui::OpenPopup("pick_root");
        }
        // The picker follows the column it was opened from (a popup is its own
        // window: measured before it opens).
        const float pickW = flow ? std::clamp(ImGui::GetContentRegionAvail().x + hooks.labelColumn,
                                              220.f, 420.f) : 300.f;
        if (ImGui::BeginPopup("pick_root"))
        {
            ImGui::SetNextItemWidth(flow ? pickW - Flow::TextW("Filter") -
                                           ImGui::GetStyle().ItemInnerSpacing.x : ImGui::CalcItemWidth());
            ImGui::InputText("Filter", m_chainBoneFilter, sizeof(m_chainBoneFilter));
            ImGui::BeginChild("bonetree", ImVec2(pickW, 320), true,
                              flow ? ImGuiWindowFlags_HorizontalScrollbar : 0);
            auto pick = [&](uint32_t b) {
                snprintf(g.rootBone, sizeof(g.rootBone), "%s", skel->boneNames[b]);
                if (g.name[0] == '\0')
                    snprintf(g.name, sizeof(g.name), "%s", skel->boneNames[b]);
                invalidate();
                ImGui::CloseCurrentPopup();
            };
            if (m_chainBoneFilter[0]) {
                for (uint32_t b = 0; b < skel->boneCount; ++b) {
                    if (!skel->boneNames[b][0] || !icontains(skel->boneNames[b], m_chainBoneFilter)) continue;
                    char lbl[96]; snprintf(lbl, sizeof(lbl), "[%u] %s", b, skel->boneNames[b]);
                    if (ImGui::Selectable(lbl)) pick(b);
                }
            } else {
                // The bone NAME is the tree-node label so a single click on
                // the row selects it (IsItemClicked); the expand arrow /
                // double-click toggles children. Using a separate SameLine
                // Selectable over a SpanAvailWidth node makes their hitboxes
                // overlap -> clicks land unpredictably ("sometimes can't pick").
                std::function<void(uint32_t)> drawTree = [&](uint32_t b) {
                    const char* nm = skel->boneNames[b][0] ? skel->boneNames[b] : "(unnamed)";
                    const bool leaf = children[b].empty();
                    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth
                                         | ImGuiTreeNodeFlags_OpenOnArrow
                                         | ImGuiTreeNodeFlags_OpenOnDoubleClick;
                    if (leaf) f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    ImGui::PushID(static_cast<int>(b));
                    const bool open = ImGui::TreeNodeEx(nm, f);
                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                        pick(b);
                    if (open && !leaf) {
                        for (uint32_t c : children[b]) drawTree(c);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };
                for (uint32_t r : roots) drawTree(r);
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        if (g.rootBone[0] && rootIdx >= skel->boneCount)
        {
            if (flow) Flow::ColoredWrapped(ImVec4(1,0.4f,0.4f,1), "Root bone not found in this skeleton!");
            else      ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Root bone not found in this skeleton!");
        }

        // ---- Ring / pair ids ----
        if (g.type == ChainGroupType::Cloth) {
            fitMarked("Cloth piece id");
            if (ImGui::InputInt("Cloth piece id", &g.ringGroup)) invalidate();
            marker();
            fitMarked("Order in ring (-1 = auto)");
            if (ImGui::InputInt("Order in ring (-1 = auto)", &g.ringIndex)) invalidate();
            marker();
        } else if (g.type == ChainGroupType::Spring) {
            fitMarked("Pair id (-1 = independent)");
            if (ImGui::InputInt("Pair id (-1 = independent)", &g.pairGroupId)) invalidate();
            marker();
            if (flow) Flow::DisabledWrapped("Spring: the root bone itself jiggles (no subtree).");
            else      ImGui::TextDisabled("Spring: the root bone itself jiggles (no subtree).");
        }

        // Cloth physics is authored once at the Cloth-piece header and synchronized
        // to every chain in that ringGroup; a per-chain material here would let one
        // piece's chains diverge.
        if (g.type == ChainGroupType::Cloth)
        {
            ImGui::TextDisabled("Physics: controlled by this Cloth piece.");
        }
        else
        {
            // Live params (read every frame in Simulate) -> no Invalidate.
            bool groupOverride = g.ovrEnabled;
            if (flow ? Flow::Checkbox("Override global params", &groupOverride)
                     : ImGui::Checkbox("Override global params", &groupOverride)) {
                if (groupOverride && !g.ovrEnabled) {
                    // Preserve the current effective motion on enable.
                    if (g.type == ChainGroupType::Spring) {
                        g.ovrSpringStiffness      = comp.springStiffness;
                        g.ovrSpringDamping        = comp.springDamping;
                        g.ovrSpringMass           = comp.springMass;
                        g.ovrSpringGravity        = comp.springGravity;
                        g.ovrSpringMaxDisp        = comp.springMaxDisp;
                        g.ovrSpringChildStiffness = comp.springChildStiffness;
                        g.ovrSpringChildDamping   = comp.springChildDamping;
                        g.ovrSpringChildMass      = comp.springChildMass;
                        g.ovrSpringChildGravity   = comp.springChildGravity;
                        g.ovrSpringChildMaxDisp   = comp.springChildMaxDisp;
                    } else {
                        g.ovrDamping          = comp.damping;
                        g.ovrGravity          = comp.gravity;
                        g.ovrStructuralStiffness = comp.structuralStiffness;
                        g.ovrRingStiffness       = comp.ringStiffness;
                        g.ovrShearStiffness      = comp.shearStiffness;
                        g.ovrMaxVelocity      = comp.maxVelocity;
                        g.ovrLocalStiffness   = comp.localStiffness;
                        g.ovrTipHold          = comp.tipHold;
                        g.ovrCollisionPadding = comp.collisionPadding;
                        g.ovrContactFriction  = comp.contactFriction;
                        g.ovrLimitAngle       = comp.limitAngle;
                        g.ovrLimitTipScale    = comp.limitTipScale;
                        g.ovrInertiaScale     = comp.inertiaScale;
                        g.ovrWindStrength     = comp.windStrength;
                        g.ovrWindFreq         = comp.windFreq;
                    }
                }
                g.ovrEnabled = groupOverride;
            }
            if (g.ovrEnabled) {
                ImGui::Indent();
                if (g.type == ChainGroupType::Spring) {
                    // A pair's VIRTUAL root borrows the group index of the LOWEST bone index in
                    // that pair (the solver fills its pair table in ascending bone order), and the
                    // root pass resolves its override from that one group only. The other
                    // member's root sliders are read by nothing, so they are disabled.
                    bool rootOwner   = true;
                    int  pairMembers = 0;
                    if (g.pairGroupId >= 0) {
                        const uint32_t myBone = resolveBone(g.rootBone);
                        for (const auto& og : comp.groups) {
                            if (og.type != ChainGroupType::Spring) continue;
                            if (og.pairGroupId != g.pairGroupId) continue;
                            ++pairMembers;
                            if (resolveBone(og.rootBone) < myBone) rootOwner = false;
                        }
                    }
                    const bool sharedRoot = (g.pairGroupId >= 0 && pairMembers >= 2);

                    if (sharedRoot)
                        ImGui::TextDisabled(rootOwner
                            ? "Virtual root (pair %d) — authored HERE (lowest bone)"
                            : "Virtual root (pair %d) — authored on the lower bone",
                            g.pairGroupId);
                    else
                        ImGui::TextDisabled("Independent bone");

                    ImGui::BeginDisabled(sharedRoot && !rootOwner);
                    ImGui::DragFloat("Stiffness##ovr", &g.ovrSpringStiffness, 1.0f,  1.f,  500.f, "%.1f");
                    ImGui::DragFloat("Damping##ovr",   &g.ovrSpringDamping,   0.1f,  0.f,  50.f,  "%.2f");
                    ImGui::DragFloat("Mass##ovr",      &g.ovrSpringMass,      0.05f, 0.05f,20.f,  "%.2f");
                    ImGui::DragFloat("Gravity##ovr",   &g.ovrSpringGravity,   0.1f, -50.f, 0.f,   "%.2f");
                    ImGui::DragFloat("Max disp##ovr",  &g.ovrSpringMaxDisp,   0.1f,  0.01f,10.f,  "%.2f");
                    ImGui::EndDisabled();
                    // The child fields are read only when 2+ Spring groups share a Pair id; a
                    // lone bone runs the root pass above. They are separate from the root fields,
                    // so the child material is its own.
                    ImGui::Spacing();
                    ImGui::TextDisabled("Paired children (needs a shared Pair id)");
                    ImGui::DragFloat("Child stiffness##ovr", &g.ovrSpringChildStiffness, 1.0f,  1.f,  700.f, "%.1f");
                    ImGui::DragFloat("Child damping##ovr",   &g.ovrSpringChildDamping,   0.1f,  0.f,  50.f,  "%.2f");
                    ImGui::DragFloat("Child mass##ovr",      &g.ovrSpringChildMass,      0.05f, 0.05f,20.f,  "%.2f");
                    ImGui::DragFloat("Child gravity##ovr",   &g.ovrSpringChildGravity,   0.1f, -50.f, 0.f,   "%.2f");
                    ImGui::DragFloat("Child max disp##ovr",  &g.ovrSpringChildMaxDisp,   0.1f,  0.01f,10.f,  "%.2f");
                } else {
                    EditorUI::DragFloat("Damping##ovr",     &g.ovrDamping,        0.f,    1.f,  "%.3f");
                    band("ovrDamping", false, g.ovrDamping, 0.f, 1.f);
                    ImGui::DragFloat  ("Gravity##ovr",     &g.ovrGravity,        0.5f, -100.f, 0.f, "%.1f");
                    band("ovrGravity", false, g.ovrGravity, -100.f, 0.f);
                    EditorUI::DragFloat("Structural stiffness##ovr", &g.ovrStructuralStiffness, 0.1f, 1.f, "%.2f");
                    band("ovrStructuralStiffness", false, g.ovrStructuralStiffness, 0.1f, 1.f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Along-chain link stiffness. PBD projection is\n"
                                          "clamped to 1.0, and the vertical bucket already\n"
                                          "contributes 1.0, so this slider goes inert above\n"
                                          "1.0 at ANY substep count. Tune below 1.0 to soften.");
                    EditorUI::DragFloat("Max velocity##ovr",&g.ovrMaxVelocity,    0.f,    5.f,  "%.3f");
                    band("ovrMaxVelocity", false, g.ovrMaxVelocity, 0.f, 5.f);
                    EditorUI::DragFloat("Hold shape##ovr",  &g.ovrLocalStiffness, 0.f,    1.f,  "%.3f");
                    band("ovrLocalStiffness", false, g.ovrLocalStiffness, 0.f, 1.f);
                    EditorUI::DragFloat("Tip hold##ovr",    &g.ovrTipHold,        0.f,    1.f,  "%.2f");
                    band("ovrTipHold", false, g.ovrTipHold, 0.f, 1.f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Min hold-shape force along the whole chain.\n"
                                          "0 = tip fully free (floppy); 1 = whole chain holds\n"
                                          "its silhouette uniformly. Raise for short bangs.");
                    EditorUI::DragFloat("Collision thickness##ovr", &g.ovrCollisionPadding, 0.f, 0.3f, "%.3f");
                    band("ovrCollisionPadding", false, g.ovrCollisionPadding, 0.f, 0.3f);
                    EditorUI::DragFloat("Contact friction##ovr",    &g.ovrContactFriction,  0.f, 1.f,  "%.2f");
                    band("ovrContactFriction", false, g.ovrContactFriction, 0.f, 1.f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Tangential velocity removed on contact.\n"
                                          "0 = slides freely, 1 = grips the body.\n"
                                          "Raise it when the chain shivers ALONG the\n"
                                          "surface rather than popping in and out.");
                    ImGui::Spacing();
                    ImGui::DragFloat  ("Limit angle##ovr",  &g.ovrLimitAngle,     0.5f, 0.f, 180.f, "%.1f");
                    band("ovrLimitAngle", false, g.ovrLimitAngle, 0.f, 180.f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Hard cap on how far each bone may swing from its\n"
                                          "ANIMATED rest direction. 0 = unlimited (physical).\n"
                                          "Unlike Hold shape (a spring), a hard impulse can never\n"
                                          "exceed it — which is what makes \"Collide with body\"\n"
                                          "safe to turn off. Try 15-30 for bangs, 35-60 for a skirt.");
                    EditorUI::DragFloat("Limit tip scale##ovr", &g.ovrLimitTipScale, 0.f, 4.f, "%.2f");
                    band("ovrLimitTipScale", false, g.ovrLimitTipScale, 0.f, 4.f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Widens the cone toward the tip. 1 = uniform,\n"
                                          "2 = the tip may swing twice as far as the root.");
                    EditorUI::DragFloat("Inertia##ovr",    &g.ovrInertiaScale,   0.f, 1.f, "%.2f");
                    band("ovrInertiaScale", false, g.ovrInertiaScale, 0.f, 1.f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How much of the body's motion this chain FEELS.\n"
                                          "1 = full physical reaction; 0 = rides the root rigidly.\n"
                                          "Lowers the reaction amplitude WITHOUT stiffening the\n"
                                          "chain, so it stays lively at small scale. Try 0.3-0.6.");
                    EditorUI::DragFloat("Sway strength##ovr", &g.ovrWindStrength, 0.f, 0.5f, "%.3f");
                    band("ovrWindStrength", false, g.ovrWindStrength, 0.f, 0.5f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Idle drift amplitude at the tip, in world units.\n"
                                          "Independent of stiffness. Needs Hold shape > 0.\n"
                                          "Direction/turbulence come from Ambient Sway (global).");
                    EditorUI::DragFloat("Sway freq (Hz)##ovr", &g.ovrWindFreq,    0.f, 5.f, "%.2f");
                    band("ovrWindFreq", false, g.ovrWindFreq, 0.f, 5.f);
                }
                ImGui::Unindent();
            } else {
                ImGui::TextDisabled("Inherits the global %s params above.",
                                    g.type == ChainGroupType::Spring ? "Spring" : "Chain");
            }
        }

        // Collision opt-out is a COST decision, not a physics material, so it
        // sits outside the override block and applies whether or not this
        // group overrides the globals. Cloth authors it at the PIECE header
        // instead: a piece is one island, and the skip only takes effect when
        // every chain in the island opts out, so a per-chain toggle here would
        // read as broken.
        if (g.type == ChainGroupType::Chain)
        {
            ImGui::Checkbox("Collide with body", &g.collide);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off = skip capsule collision for this chain entirely.\n"
                                  "Capsule solving is the system's dominant cost. Safe to turn\n"
                                  "off only when Limit angle keeps the chain away from the body\n"
                                  "(bangs, ribbons, ornaments). Takes effect when EVERY chain in\n"
                                  "the same simulation island opts out.");
        }
        // World-motion inertia, per group (Cloth: at the PIECE header below).
        if (g.type != ChainGroupType::Cloth)
            worldInertiaRow(g, "wi");

        // ---- Exclude tree (Hair / Skirt only) ----
        if (g.type != ChainGroupType::Spring && rootIdx < skel->boneCount)
        {
            if (flow ? Flow::Checkbox("Exclude whole subtree on uncheck", &g.excludeSubtree)
                     : ImGui::Checkbox("Exclude whole subtree on uncheck", &g.excludeSubtree))
                invalidate();
            marker();

            // live "simulated bone" count (subtree minus excludes).
            int simCount = 0;
            {
                std::vector<uint8_t> mk(skel->boneCount, 0), ex(skel->boneCount, 0);
                mk[rootIdx] = 1;
                for (uint32_t b = rootIdx + 1; b < skel->boneCount; ++b)
                    if (skel->parentIndex[b] >= 0 && mk[skel->parentIndex[b]]) mk[b] = 1;
                for (int j = 0; j < g.excludeCount; ++j) {
                    uint32_t exb = resolveBone(g.excludeBones[j]);
                    if (exb < skel->boneCount) ex[exb] = 1;
                }
                if (g.excludeSubtree)
                    for (uint32_t b = 0; b < skel->boneCount; ++b)
                        if (skel->parentIndex[b] >= 0 && ex[skel->parentIndex[b]]) ex[b] = 1;
                for (uint32_t b = 0; b < skel->boneCount; ++b)
                    if (mk[b] && !ex[b]) ++simCount;
            }
            if (flow) Flow::Wrapped("Simulated bones: %d  (excluded: %d)", simCount, g.excludeCount);
            else      ImGui::Text("Simulated bones: %d  (excluded: %d)", simCount, g.excludeCount);
            if (g.excludeCount >= ChainGroupDef::MAX_EXCLUDE)
            {
                if (flow)
                    Flow::ColoredWrapped(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                                         "Exclude list full (%d) — unchecking more bones has no effect.",
                                         ChainGroupDef::MAX_EXCLUDE);
                else
                    ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                                       "Exclude list full (%d) — unchecking more bones has no effect.",
                                       ChainGroupDef::MAX_EXCLUDE);
            }

            if (ImGui::TreeNode("Bones (uncheck to exclude)"))
            {
                auto findExcl = [&](const char* nm) -> int {
                    for (int j = 0; j < g.excludeCount; ++j)
                        if (std::strcmp(g.excludeBones[j], nm) == 0) return j;
                    return -1;
                };
                auto setExcl = [&](const char* nm, bool excl) {
                    int idx = findExcl(nm);
                    if (excl && idx < 0 && g.excludeCount < ChainGroupDef::MAX_EXCLUDE) {
                        snprintf(g.excludeBones[g.excludeCount], 64, "%s", nm);
                        g.excludeCount++;
                    } else if (!excl && idx >= 0) {
                        for (int k = idx; k < g.excludeCount - 1; ++k)
                            snprintf(g.excludeBones[k], 64, "%s", g.excludeBones[k + 1]);
                        g.excludeCount--;
                    }
                };
                std::function<void(uint32_t)> drawExcl = [&](uint32_t b) {
                    const char* nm = skel->boneNames[b][0] ? skel->boneNames[b] : "(unnamed)";
                    bool sim = (findExcl(nm) < 0);
                    ImGui::PushID(static_cast<int>(b));
                    if (ImGui::Checkbox("##e", &sim)) { setExcl(nm, !sim); invalidate(); }
                    ImGui::SameLine();
                    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
                    if (children[b].empty()) f |= ImGuiTreeNodeFlags_Leaf;
                    if (ImGui::TreeNodeEx(nm, f)) {
                        for (uint32_t c : children[b]) drawExcl(c);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };
                for (uint32_t c : children[rootIdx]) drawExcl(c);
                ImGui::TreePop();
            }
        }
        else if (g.type != ChainGroupType::Spring)
        {
            ImGui::TextDisabled("(pick a valid root bone to edit exclusions)");
        }

        ImGui::Spacing();
        const bool removeClicked = ImGui::Button("Remove chain");
        marker();
        return removeClicked;
    };

    // ---- One collapsing panel per physics type; chains live inside ----
    int removeGroup = -1;
    const bool full = comp.groups.size() >= static_cast<size_t>(ChainPhysicsComponent::MAX_GROUPS);

    // One chain's collapsible node (header + editable body); shared by the
    // flat panels (Chain/Spring) and the Cloth-piece sub-groups. The
    // expanded node re-asserts the viewport highlight each frame (reset
    // happens once per frame at the top of OnUIRender).
    auto drawGroupNode = [&](int gi) {
        ChainGroupDef& g = comp.groups[gi];
        ImGui::PushID(gi);
        char gh[176];
        snprintf(gh, sizeof(gh), "%s  |  %s###grp%d",
                 g.name[0] ? g.name : "(chain)",
                 g.rootBone[0] ? g.rootBone : "(no root)", gi);
        // A node label cannot wrap: in a narrow column it drops the root bone, then
        // ellipsizes (the id is unchanged, so the open state survives; the full label
        // is the tooltip).
        bool shortened = false;
        if (flow)
        {
            const float room = ImGui::GetContentRegionAvail().x - ImGui::GetTreeNodeToLabelSpacing();
            const std::string full = std::string(g.name[0] ? g.name : "(chain)") + "  |  " +
                                     (g.rootBone[0] ? g.rootBone : "(no root)");
            if (ImGui::CalcTextSize(full.c_str()).x > room)
            {
                const std::string s = Flow::Ellipsize(g.name[0] ? g.name : "(chain)", std::max(room, 24.f));
                snprintf(gh, sizeof(gh), "%s###grp%d", s.c_str(), gi);
                shortened = true;
            }
        }
        if (hooks.expandAll) ImGui::SetNextItemOpen(true, ImGuiCond_Always);   // UI layout harness
        const bool nodeOpen = ImGui::TreeNodeEx(gh, ImGuiTreeNodeFlags_SpanAvailWidth);
        if (shortened && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  |  %s", g.name[0] ? g.name : "(chain)",
                              g.rootBone[0] ? g.rootBone : "(no root)");
        if (nodeOpen) {
            // Expanded -> highlight in the MAIN viewport, but only when this tree
            // describes that overlay's target: the preview stage's tree must not dim a
            // scene character.
            if (hooks.mainOverlayTarget) m_chainHighlightGroup = gi;
            if (drawGroupBody(g)) removeGroup = gi;
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    const ChainGroupType kTypes[3] = {
        ChainGroupType::Chain, ChainGroupType::Cloth, ChainGroupType::Spring
    };

    for (int ti = 0; ti < 3; ++ti)
    {
        const ChainGroupType T = kTypes[ti];
        int count = 0;
        for (int gi = 0; gi < static_cast<int>(comp.groups.size()); ++gi)
            if (comp.groups[gi].type == T) ++count;

        char secHdr[64];
        snprintf(secHdr, sizeof(secHdr), "%s  (%d)###sec%d", kTypeNames[ti], count, ti);
        if (!ImGui::CollapsingHeader(secHdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::PushID(ti);
        ImGui::Indent();

        if (T == ChainGroupType::Cloth)
        {
            // Cloth chains are sub-grouped into "Cloth pieces" by ringGroup: each piece
            // is one ring of horizontal/shear constraints, so multiple cloth pieces (skirt
            // + cape + ...) stay separated and chains within a piece tie together.
            std::vector<int> pieces; int maxRg = -1;
            for (int gi = 0; gi < static_cast<int>(comp.groups.size()); ++gi) {
                if (comp.groups[gi].type != ChainGroupType::Cloth) continue;
                const int rg = comp.groups[gi].ringGroup;
                if (rg > maxRg) maxRg = rg;
                bool seen = false; for (int pv : pieces) if (pv == rg) { seen = true; break; }
                if (!seen) pieces.push_back(rg);
            }
            // small insertion sort (avoids an <algorithm> dependency)
            for (size_t a = 1; a < pieces.size(); ++a) {
                int v = pieces[a]; size_t b = a;
                while (b > 0 && pieces[b - 1] > v) { pieces[b] = pieces[b - 1]; --b; }
                pieces[b] = v;
            }

            for (int rg : pieces) {
                std::vector<int> pieceGroups;
                for (int gi = 0; gi < static_cast<int>(comp.groups.size()); ++gi)
                    if (comp.groups[gi].type == ChainGroupType::Cloth && comp.groups[gi].ringGroup == rg)
                        pieceGroups.push_back(gi);
                const int pc = static_cast<int>(pieceGroups.size());

                ImGui::PushID(rg);
                char ph[72];
                snprintf(ph, sizeof(ph), "Cloth piece %d  (%d chains)###piece%d", rg, pc, rg);
                if (ImGui::CollapsingHeader(ph, ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Indent();

                    // One physics material per Cloth piece. The values live in every chain def of
                    // the piece (that is what is serialized); this UI keeps them synchronized.
                    auto copyClothPhysics = [](ChainGroupDef& dst, const ChainGroupDef& src) {
                        dst.ovrEnabled          = src.ovrEnabled;
                        dst.ovrDamping          = src.ovrDamping;
                        dst.ovrGravity          = src.ovrGravity;
                        dst.ovrStructuralStiffness = src.ovrStructuralStiffness;
                        dst.ovrRingStiffness       = src.ovrRingStiffness;
                        dst.ovrShearStiffness      = src.ovrShearStiffness;
                        dst.ovrMaxVelocity      = src.ovrMaxVelocity;
                        dst.ovrLocalStiffness   = src.ovrLocalStiffness;
                        dst.ovrHorizDecay       = src.ovrHorizDecay;
                        dst.ovrTipHold          = src.ovrTipHold;
                        dst.ovrCollisionPadding = src.ovrCollisionPadding;
                        dst.ovrContactFriction  = src.ovrContactFriction;
                        dst.ovrLimitAngle       = src.ovrLimitAngle;
                        dst.ovrLimitTipScale    = src.ovrLimitTipScale;
                        dst.ovrInertiaScale     = src.ovrInertiaScale;
                        dst.ovrWindStrength     = src.ovrWindStrength;
                        dst.ovrWindFreq         = src.ovrWindFreq;
                        dst.ovrWorldInertiaLocation = src.ovrWorldInertiaLocation;
                        dst.ovrWorldInertiaRotation = src.ovrWorldInertiaRotation;
                        dst.collide             = src.collide;
                    };

                    ChainGroupDef& piecePhysics = comp.groups[pieceGroups.front()];
                    auto sameClothPhysics = [](const ChainGroupDef& a, const ChainGroupDef& b) {
                        return a.ovrEnabled          == b.ovrEnabled &&
                               a.ovrDamping          == b.ovrDamping &&
                               a.ovrGravity          == b.ovrGravity &&
                               a.ovrStructuralStiffness == b.ovrStructuralStiffness &&
                               a.ovrRingStiffness       == b.ovrRingStiffness &&
                               a.ovrShearStiffness      == b.ovrShearStiffness &&
                               a.ovrMaxVelocity      == b.ovrMaxVelocity &&
                               a.ovrLocalStiffness   == b.ovrLocalStiffness &&
                               a.ovrHorizDecay       == b.ovrHorizDecay &&
                               a.ovrTipHold          == b.ovrTipHold &&
                               a.ovrCollisionPadding == b.ovrCollisionPadding &&
                               a.ovrContactFriction  == b.ovrContactFriction &&
                               a.ovrLimitAngle       == b.ovrLimitAngle &&
                               a.ovrLimitTipScale    == b.ovrLimitTipScale &&
                               a.ovrInertiaScale     == b.ovrInertiaScale &&
                               a.ovrWindStrength     == b.ovrWindStrength &&
                               a.ovrWindFreq         == b.ovrWindFreq &&
                               a.ovrWorldInertiaLocation == b.ovrWorldInertiaLocation &&
                               a.ovrWorldInertiaRotation == b.ovrWorldInertiaRotation &&
                               a.collide             == b.collide;
                    };
                    bool mixedLegacyValues = false;
                    for (int gi : pieceGroups)
                        if (!sameClothPhysics(piecePhysics, comp.groups[gi])) {
                            mixedLegacyValues = true;
                            break;
                        }

                    ImGui::TextUnformatted("Piece physics");
                    bool pieceOverride = piecePhysics.ovrEnabled;
                    if (flow ? Flow::Checkbox("Override Cloth global for this piece", &pieceOverride)
                             : ImGui::Checkbox("Override Cloth global for this piece", &pieceOverride)) {
                        if (pieceOverride && !piecePhysics.ovrEnabled) {
                            // Start from the current effective globals so
                            // enabling isolation never makes the cloth jump.
                            piecePhysics.ovrDamping          = comp.skirtDamping;
                            piecePhysics.ovrGravity          = comp.skirtGravity;
                            // Structural comes from the CHAIN-side global, not the cloth one: a Cloth
                            // piece's along-chain links read structuralStiffness. Seeding it from
                            // skirtStiffness would make the vertical links jump the instant this box is
                            // ticked.
                            piecePhysics.ovrStructuralStiffness = comp.structuralStiffness;
                            piecePhysics.ovrRingStiffness       = comp.ringStiffness;
                            piecePhysics.ovrShearStiffness      = comp.shearStiffness;
                            piecePhysics.ovrMaxVelocity      = comp.skirtMaxVelocity;
                            piecePhysics.ovrLocalStiffness   = comp.skirtLocalStiffness;
                            piecePhysics.ovrHorizDecay       = comp.skirtHorizDecay;
                            piecePhysics.ovrTipHold          = comp.skirtTipHold;
                            piecePhysics.ovrCollisionPadding = comp.collisionPadding;
                            piecePhysics.ovrContactFriction  = comp.contactFriction;
                            piecePhysics.ovrLimitAngle       = comp.skirtLimitAngle;
                            piecePhysics.ovrLimitTipScale    = comp.skirtLimitTipScale;
                            piecePhysics.ovrInertiaScale     = comp.skirtInertiaScale;
                            piecePhysics.ovrWindStrength     = comp.windStrength;
                            piecePhysics.ovrWindFreq         = comp.windFreq;
                        }
                        piecePhysics.ovrEnabled = pieceOverride;
                        for (int gi : pieceGroups)
                            copyClothPhysics(comp.groups[gi], piecePhysics);
                        mixedLegacyValues = false;
                    }

                    if (mixedLegacyValues)
                    {
                        if (flow)
                            Flow::ColoredWrapped(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                                "Mixed legacy chain values; editing below will unify this piece.");
                        else
                            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                                "Mixed legacy chain values; editing below will unify this piece.");
                    }

                    if (piecePhysics.ovrEnabled) {
                        bool changed = false;
                        changed |= EditorUI::DragFloat("Damping##piece",       &piecePhysics.ovrDamping,          0.f,    1.f, "%.3f");
                        band("ovrDamping", true, piecePhysics.ovrDamping, 0.f, 1.f);
                        changed |= ImGui::DragFloat  ("Gravity##piece",       &piecePhysics.ovrGravity,          0.5f, -100.f, 0.f, "%.1f");
                        band("ovrGravity", true, piecePhysics.ovrGravity, -100.f, 0.f);
                        changed |= EditorUI::DragFloat("Structural stiffness##piece", &piecePhysics.ovrStructuralStiffness, 0.1f, 1.f, "%.2f");
                        band("ovrStructuralStiffness", true, piecePhysics.ovrStructuralStiffness, 0.1f, 1.f);
                        changed |= EditorUI::DragFloat("Ring stiffness##piece",       &piecePhysics.ovrRingStiffness,       0.1f, 5.f, "%.2f");
                        band("ovrRingStiffness", true, piecePhysics.ovrRingStiffness, 0.1f, 5.f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Hoop links between neighbouring chains.\n"
                                              "High = the piece holds a barrel; low = the\n"
                                              "panels drift apart and swing independently.\n"
                                              "Without decay, values >= 1.25 are fully rigid.\n"
                                              "Higher values only affect links weakened by decay.");
                        changed |= EditorUI::DragFloat("Shear stiffness##piece",      &piecePhysics.ovrShearStiffness,      0.1f, 5.f, "%.2f");
                        band("ovrShearStiffness", true, piecePhysics.ovrShearStiffness, 0.1f, 5.f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Diagonal cross-level links — resistance to\n"
                                              "scissoring/twisting. Was welded to Ring at a\n"
                                              "fixed 0.625x until it got its own value.\n"
                                              "Low = flowy, lagging panels (anime skirt).\n"
                                              "Without decay, values >= 2 are fully rigid.\n"
                                              "Higher values only affect links weakened by decay.");
                        changed |= EditorUI::DragFloat("Max velocity##piece",  &piecePhysics.ovrMaxVelocity,      0.f,    5.f, "%.3f");
                        band("ovrMaxVelocity", true, piecePhysics.ovrMaxVelocity, 0.f, 5.f);
                        changed |= EditorUI::DragFloat("Hold shape##piece",    &piecePhysics.ovrLocalStiffness,   0.f,    1.f, "%.3f");
                        band("ovrLocalStiffness", true, piecePhysics.ovrLocalStiffness, 0.f, 1.f);
                        changed |= EditorUI::DragFloat("Tip hold##piece",      &piecePhysics.ovrTipHold,          0.f,    1.f, "%.2f");
                        band("ovrTipHold", true, piecePhysics.ovrTipHold, 0.f, 1.f);
                        changed |= EditorUI::DragFloat("Ring+Shear decay##piece", &piecePhysics.ovrHorizDecay,    0.f,    1.f, "%.3f");
                        band("ovrHorizDecay", true, piecePhysics.ovrHorizDecay, 0.f, 1.f);
                        changed |= EditorUI::DragFloat("Collision thickness##piece", &piecePhysics.ovrCollisionPadding, 0.f, 0.3f, "%.3f");
                        band("ovrCollisionPadding", true, piecePhysics.ovrCollisionPadding, 0.f, 0.3f);
                        changed |= EditorUI::DragFloat("Contact friction##piece",    &piecePhysics.ovrContactFriction,  0.f, 1.f,  "%.2f");
                        band("ovrContactFriction", true, piecePhysics.ovrContactFriction, 0.f, 1.f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Tangential velocity removed on contact.\n"
                                              "0 = satin lining sliding over the leg;\n"
                                              "high = heavy coat that drapes and grips.\n"
                                              "Raise it when the piece shivers ALONG the\n"
                                              "body rather than popping in and out.");
                        ImGui::Spacing();
                        changed |= ImGui::DragFloat  ("Limit angle##piece",      &piecePhysics.ovrLimitAngle,    0.5f, 0.f, 180.f, "%.1f");
                        band("ovrLimitAngle", true, piecePhysics.ovrLimitAngle, 0.f, 180.f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Hard cap on how far each bone may swing from its\n"
                                              "ANIMATED rest direction. 0 = unlimited (physical).\n"
                                              "Bounds the worst failures a capsule solver struggles\n"
                                              "with — a skirt flipping through the torso on a fast\n"
                                              "spin. Try 35-60 for a skirt, 20-40 for a coat.");
                        changed |= EditorUI::DragFloat("Limit tip scale##piece", &piecePhysics.ovrLimitTipScale, 0.f, 4.f, "%.2f");
                        band("ovrLimitTipScale", true, piecePhysics.ovrLimitTipScale, 0.f, 4.f);
                        changed |= EditorUI::DragFloat("Inertia##piece",         &piecePhysics.ovrInertiaScale,  0.f, 1.f, "%.2f");
                        band("ovrInertiaScale", true, piecePhysics.ovrInertiaScale, 0.f, 1.f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("How much of the body's motion this piece FEELS.\n"
                                              "1 = full physical reaction; 0 = rides the root rigidly.\n"
                                              "Reduces the swing WITHOUT stiffening the cloth.");
                        changed |= EditorUI::DragFloat("Sway strength##piece",   &piecePhysics.ovrWindStrength,  0.f, 0.5f, "%.3f");
                        band("ovrWindStrength", true, piecePhysics.ovrWindStrength, 0.f, 0.5f);
                        changed |= EditorUI::DragFloat("Sway freq (Hz)##piece",  &piecePhysics.ovrWindFreq,      0.f, 5.f, "%.2f");
                        band("ovrWindFreq", true, piecePhysics.ovrWindFreq, 0.f, 5.f);
                        if (changed) {
                            for (int gi : pieceGroups)
                                copyClothPhysics(comp.groups[gi], piecePhysics);
                        }
                    } else {
                        if (flow) Flow::DisabledWrapped("Inherits Cloth (global); other pieces may override independently.");
                        else      ImGui::TextDisabled("Inherits Cloth (global); other pieces may override independently.");
                    }

                    // Collision opt-out is per PIECE (= per island): the skip only takes effect
                    // when every chain in the island opts out, so it is authored here rather than
                    // per chain. Shown regardless of the override toggle: it is a cost decision,
                    // not part of the physics material.
                    if (ImGui::Checkbox("Collide with body##piece", &piecePhysics.collide)) {
                        for (int gi : pieceGroups)
                            comp.groups[gi].collide = piecePhysics.collide;
                    }
                    // World-motion inertia: per PIECE (every chain of it), outside
                    // the override block like the collide toggle above.
                    if (worldInertiaRow(piecePhysics, "wipiece"))
                        for (int gi : pieceGroups)
                        {
                            comp.groups[gi].ovrWorldInertiaLocation = piecePhysics.ovrWorldInertiaLocation;
                            comp.groups[gi].ovrWorldInertiaRotation = piecePhysics.ovrWorldInertiaRotation;
                        }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Off = skip capsule collision for this whole piece.\n"
                                          "Capsule solving runs substeps*(1+iterations)+1 times per\n"
                                          "frame and dominates this system's cost. Safe only when\n"
                                          "Limit angle keeps the piece clear of the body.");

                    ImGui::Separator();
                    for (int gi : pieceGroups)
                        drawGroupNode(gi);
                    ImGui::BeginDisabled(full);
                    if (ImGui::Button("+ Add chain to this piece")) {
                        ChainGroupDef ng{}; ng.type = ChainGroupType::Cloth;
                        ng.ringGroup = rg; ng.ringIndex = -1;
                        copyClothPhysics(ng, piecePhysics);
                        snprintf(ng.name, sizeof(ng.name), "Cloth %d.%d", rg, pc);
                        comp.groups.push_back(ng); invalidate();
                    }
                    ImGui::EndDisabled();
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }

            ImGui::BeginDisabled(full);
            if (ImGui::Button("+ Add Cloth piece")) {
                ChainGroupDef ng{}; ng.type = ChainGroupType::Cloth;
                ng.ringGroup = maxRg + 1; ng.ringIndex = -1;
                snprintf(ng.name, sizeof(ng.name), "Cloth %d.0", maxRg + 1);
                comp.groups.push_back(ng); invalidate();
            }
            ImGui::EndDisabled();
        }
        else
        {
            for (int gi = 0; gi < static_cast<int>(comp.groups.size()); ++gi)
                if (comp.groups[gi].type == T) drawGroupNode(gi);

            ImGui::BeginDisabled(full);
            char addLbl[40];
            snprintf(addLbl, sizeof(addLbl), "+ Add %s", kTypeNames[ti]);
            if (ImGui::Button(addLbl)) {
                ChainGroupDef ng{}; ng.type = T;
                snprintf(ng.name, sizeof(ng.name), "%s %d", kTypeNames[ti], count);
                comp.groups.push_back(ng); invalidate();
            }
            ImGui::EndDisabled();
        }

        ImGui::Unindent();
        ImGui::PopID();
    }

    if (removeGroup >= 0 && removeGroup < static_cast<int>(comp.groups.size())) {
        comp.groups.erase(comp.groups.begin() + removeGroup);
        invalidate();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Total chains: %d / %d",
                        static_cast<int>(comp.groups.size()), ChainPhysicsComponent::MAX_GROUPS);
    if (ImGui::Button("Auto-detect from bone names")) {
        ChainPhysicsSystem::BuildGroupsFromNames(*skel, comp);
        invalidate();
    }
    if (!comp.groups.empty()) {
        if (flow) Flow::SameLineIfFits(Flow::ButtonW("Clear all"));
        else      ImGui::SameLine();
        if (ImGui::Button("Clear all")) { comp.groups.clear(); invalidate(); }
    }
    if (full)
    {
        static const char* kFull = "Max chains reached — remove one (or consolidate skirt "
                                   "panels under a single root + excludes) to add more.";
        if (flow) Flow::ColoredWrapped(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s", kFull);
        else      ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s", kFull);
    }
    if (flow) ImGui::PopItemWidth();   // the body's PushItemWidth
}
