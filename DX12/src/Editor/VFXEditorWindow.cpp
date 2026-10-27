// VFXEditorWindow.cpp - the authoring window for a .ivfx prefab (the coordinated
// multi-emitter effect set an AnimNotify fires). Part of EditorLayer, compiled in its
// own file.
//
// Field widgets come from reflection: every emitter param struct is an ECS component
// drawn through its ComponentReflection.h descriptor (Reflect::DrawObject(&spec.particle))
// or a small Lane-B POD reflected in VFXSpecReflection.h; Resource::SaveVFXPrefab writes
// the file. Emitters carry stable ids (the selection survives reorder / delete); undo is
// a coalesced whole-prefab snapshot ring (one entry per drag). Also: a timeline canvas,
// .ivfx drag-drop, a live preview and Import from a scene entity.
//
// Persistent editor state is a file-static (there is exactly one editor); only
// the m_showVFXEditor toggle lives on EditorLayer so the menu item can bind it.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/EditorLayer.h"
#include "Resource/VFXPrefab.h"
#include "Reflection/ComponentReflection.h"   // Particle/Trail/Beam/LocalTransform descriptors
#include "Reflection/ReflectionEditor.h"      // DrawObject
#include "Editor/VFXSpecReflection.h"         // Decal/Tracer/Afterimage/Mesh/Beam-wrapper descriptors
#include "ECS/NotifyConsumerSystems.h"        // InvalidateVFXPrefabCache / SetVFXPreviewPrefab / kVFXPreviewAssetKey
#include "ECS/NotifyTypes.h"                  // PendingVFXSpawns / VFXAttachMode
#include "ECS/HierarchyComponents.h"          // GlobalTransform / SceneTransientTag / Parent / LocalTransform / MaterialSourcePath
#include "ECS/FollowComponents.h"             // FollowSocketComponent / FollowEntityComponent (logical-child walk for Import)
#include "ECS/Components.h"                   // MaterialComponent / BlendMode (beam look extraction)
#include "ECS/LifetimeComponent.h"            // LifetimeComponent (preview spawner auto-cleanup)
#include "System/Log.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Resource::VFXPrefab;
using Resource::VFXEmitterSpec;
using Resource::VFXEmitterType;

namespace
{
const char* kTypeNames[] = { "Particle", "Trail", "Beam", "Decal", "Tracer", "Afterimage", "Mesh" };
constexpr std::size_t kMaxUndo = 64;

// A whole-file snapshot: files are tiny, so a snapshot ring is far simpler and
// safer than an id-keyed command log, and it captures list ops + field edits
// uniformly. Selection is stored by stable id so it survives reorder / delete.
struct VFXSnapshot
{
    VFXPrefab              prefab;
    std::vector<uint32_t>  ids;
    uint32_t               selId = 0;
};

struct VFXEditorState
{
    VFXPrefab              prefab;
    std::vector<uint32_t>  ids;             // ids[i] is the stable id of emitters[i]
    uint32_t               nextId   = 1;    // monotonic; 0 means "no selection"
    uint32_t               selId    = 0;

    char                   pathBuf[260] = "asset/FX/new_effect.ivfx";
    std::string            loadedPath;
    bool                   loaded   = false;
    bool                   dirty    = false;
    bool                   pendingChange = false;   // an edit happened; commit to undo when idle
    std::string            status;

    VFXSnapshot                committed;           // last-committed baseline
    std::vector<VFXSnapshot>   undoStack;
    std::vector<VFXSnapshot>   redoStack;

    // ---- Timeline canvas view / drag state ----
    float                  pps      = 180.f;    // pixels per second (zoom)
    float                  scrollX  = 0.f;      // horizontal time pan (px)
    int                    dragMode = 0;        // 0 none / 1 move / 2 resize-start / 3 resize-end
    uint32_t               dragId   = 0;
    float                  dragStartDelay = 0.f;
    float                  dragDur        = 0.f;
    float                  dragMouseX     = 0.f;

    // ---- Live preview ----
    // Generation-checked handle (never a raw Entity): a raw id survives a world
    // Clear / LifetimeComponent recycle and would let Stop / Preview DestroyEntity
    // an unrelated recycled entity. Also reset when the world epoch moves.
    EntityHandle           previewSpawner{};
    uint32_t               lastWorldEpoch = 0;
};
VFXEditorState g_vfx;

enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE_START, DRAG_RESIZE_END };
constexpr float kMinBarDur = 0.02f;

VFXSnapshot Capture()               { return { g_vfx.prefab, g_vfx.ids, g_vfx.selId }; }
void        Restore(const VFXSnapshot& s) { g_vfx.prefab = s.prefab; g_vfx.ids = s.ids; g_vfx.selId = s.selId; }

int SelIndex()
{
    for (std::size_t k = 0; k < g_vfx.ids.size(); ++k)
        if (g_vfx.ids[k] == g_vfx.selId) return static_cast<int>(k);
    return -1;
}

void RebuildIds()
{
    g_vfx.ids.clear();
    for (std::size_t i = 0; i < g_vfx.prefab.emitters.size(); ++i)
        g_vfx.ids.push_back(g_vfx.nextId++);
    g_vfx.selId = g_vfx.ids.empty() ? 0u : g_vfx.ids.front();
}

void ResetHistory()
{
    g_vfx.undoStack.clear();
    g_vfx.redoStack.clear();
    g_vfx.pendingChange = false;
    g_vfx.committed = Capture();
}

// Called wherever the prefab is mutated. Defers the undo push to the first
// frame with no active widget, so a multi-frame slider drag coalesces into ONE
// undo entry and discrete button ops commit the same frame they fire.
void MarkChanged() { g_vfx.pendingChange = true; }

void CommitTick()
{
    if (!g_vfx.pendingChange) return;
    g_vfx.dirty = true;
    if (!ImGui::IsAnyItemActive())
    {
        g_vfx.undoStack.push_back(g_vfx.committed);       // pre-edit baseline
        if (g_vfx.undoStack.size() > kMaxUndo) g_vfx.undoStack.erase(g_vfx.undoStack.begin());
        g_vfx.redoStack.clear();
        g_vfx.committed = Capture();                       // new baseline
        g_vfx.pendingChange = false;
    }
}

void DoUndo()
{
    if (g_vfx.undoStack.empty()) return;
    g_vfx.redoStack.push_back(Capture());
    Restore(g_vfx.undoStack.back());
    g_vfx.undoStack.pop_back();
    g_vfx.committed = Capture();
    g_vfx.pendingChange = false;
    g_vfx.dirty = true;
    g_vfx.status = "Undo";
}

void DoRedo()
{
    if (g_vfx.redoStack.empty()) return;
    g_vfx.undoStack.push_back(Capture());
    Restore(g_vfx.redoStack.back());
    g_vfx.redoStack.pop_back();
    g_vfx.committed = Capture();
    g_vfx.pendingChange = false;
    g_vfx.dirty = true;
    g_vfx.status = "Redo";
}

void NewPrefab()
{
    g_vfx.prefab = VFXPrefab{};
    g_vfx.prefab.emitters.emplace_back();              // default = Particle
    RebuildIds();
    g_vfx.loaded = true;
    g_vfx.dirty  = true;
    g_vfx.status = "New effect (unsaved)";
    ResetHistory();
}

// ==== List mutations keep emitters[] and ids[] in lockstep ====
void AddEmitter()
{
    g_vfx.prefab.emitters.emplace_back();
    g_vfx.ids.push_back(g_vfx.nextId++);
    g_vfx.selId = g_vfx.ids.back();
    MarkChanged();
}
void DuplicateEmitter(int i)
{
    g_vfx.prefab.emitters.insert(g_vfx.prefab.emitters.begin() + i + 1, g_vfx.prefab.emitters[i]);
    g_vfx.ids.insert(g_vfx.ids.begin() + i + 1, g_vfx.nextId++);
    g_vfx.selId = g_vfx.ids[i + 1];
    MarkChanged();
}
void RemoveEmitter(int i)
{
    g_vfx.prefab.emitters.erase(g_vfx.prefab.emitters.begin() + i);
    g_vfx.ids.erase(g_vfx.ids.begin() + i);
    const int n = static_cast<int>(g_vfx.ids.size());
    g_vfx.selId = (n == 0) ? 0u : g_vfx.ids[i < n ? i : n - 1];
    MarkChanged();
}
void SwapEmitter(int a, int b)
{
    std::swap(g_vfx.prefab.emitters[a], g_vfx.prefab.emitters[b]);
    std::swap(g_vfx.ids[a], g_vfx.ids[b]);   // ids follow their emitter -> selId still valid
    MarkChanged();
}

// std::string <-> ImGui::InputText without imgui_stdlib: refresh a scratch
// buffer from the string each frame, copy back on edit. Optional drag-drop.
bool StringInput(const char* label, std::string& s, const char* payload = nullptr)
{
    char buf[260];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    bool changed = false;
    if (ImGui::InputText(label, buf, sizeof(buf))) { s = buf; changed = true; }
    if (payload && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(payload))
        {
            s.assign(static_cast<const char*>(p->Data), p->DataSize > 0 ? p->DataSize - 1 : 0);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

// ==== Import from an existing scene entity ====
// A VFXEmitterSpec EMBEDS the real component structs, so pulling a tuned
// prefab's params in is a direct struct COPY - no field mapping. The walk covers
// the selected entity's LOGICAL subtree (Parent + FollowSocket / FollowEntity, the
// same tree the Hierarchy panel shows) so effects on attached child entities
// come along too.
Entity LogicalParent(World& w, Entity e)
{
    if (auto* pc = w.GetComponent<Parent>(e); pc && pc->entity != NullEntity && w.IsAlive(pc->entity))
        return pc->entity;
    if (auto* fs = w.GetComponent<FollowSocketComponent>(e))
    { const Entity t = fs->target.Resolve(w); if (t != NullEntity && t != e && w.IsAlive(t)) return t; }
    if (auto* fe = w.GetComponent<FollowEntityComponent>(e))
    { const Entity t = fe->target.Resolve(w); if (t != NullEntity && t != e && w.IsAlive(t)) return t; }
    return NullEntity;
}

void ImportEffectsFromEntity(World& w, Entity root, int& added, int& skipped)
{
    auto isInSubtree = [&](Entity e) {
        for (int guard = 0; e != NullEntity && guard < 64; ++guard) {
            if (e == root) return true;
            e = LogicalParent(w, e);
        }
        return false;
    };
    auto localOf = [&](Entity e) -> LocalTransform {
        if (auto* lt = w.GetComponent<LocalTransform>(e)) return *lt;
        return LocalTransform{};
    };
    auto matPathOf = [&](Entity e) -> std::string {
        if (auto* mp = w.GetComponent<MaterialSourcePath>(e)) return mp->path;
        return {};
    };
    auto append = [&](VFXEmitterSpec&& s) {
        g_vfx.prefab.emitters.push_back(std::move(s));
        g_vfx.ids.push_back(g_vfx.nextId++);
        ++added;
    };

    if (auto* pool = w.GetPool<ParticleEmitterComponent>()) {
        const auto& ents = pool->Entities(); const auto& data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i) {
            if (!isInSubtree(ents[i])) continue;
            VFXEmitterSpec s; s.type = VFXEmitterType::Particle;
            s.particle = data[i];
            s.particle.meshSourceEntity = 0;   // a cross-instance entity id does not survive a prefab
            if (auto* rc = w.GetComponent<ParticleRenderComponent>(ents[i])) s.particleRender = *rc;
            s.localTransform = localOf(ents[i]);
            s.materialPath   = matPathOf(ents[i]);
            append(std::move(s));
        }
    }
    if (auto* pool = w.GetPool<TrailComponent>()) {
        const auto& ents = pool->Entities(); const auto& data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i) {
            if (!isInSubtree(ents[i])) continue;
            VFXEmitterSpec s; s.type = VFXEmitterType::Trail;
            s.trail = data[i];
            s.localTransform = localOf(ents[i]);
            s.materialPath   = matPathOf(ents[i]);
            append(std::move(s));
        }
    }
    if (auto* pool = w.GetPool<BeamComponent>()) {
        const auto& ents = pool->Entities(); const auto& data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i) {
            if (!isInSubtree(ents[i])) continue;
            VFXEmitterSpec s; s.type = VFXEmitterType::Beam;
            s.beam.beam = data[i];
            if (auto* mc = w.GetComponent<MaterialComponent>(ents[i])) {
                if (mc->useCustomShader && !mc->customShaderPath.empty()) s.beam.shaderPath = mc->customShaderPath;
                s.beam.additive = (mc->userBlendMode == BlendMode::Additive);
            }
            s.localTransform = localOf(ents[i]);
            append(std::move(s));
        }
    }
    // Flock / Ambient particle behaviours have no .ivfx representation: count
    // them so the user knows they were not imported (they stay authored in-scene).
    auto countSubtree = [&](auto* pool) {
        if (!pool) return;
        const auto& ents = pool->Entities();
        for (const Entity e : ents) if (isInSubtree(e)) ++skipped;
    };
    countSubtree(w.GetPool<ParticleFlockComponent>());
    countSubtree(w.GetPool<ParticleAmbientComponent>());
}

void DrawEmitterHeader(VFXEmitterSpec& e)
{
    int ti = static_cast<int>(e.type);
    if (ImGui::Combo("Type", &ti, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
    {
        e.type = static_cast<VFXEmitterType>(ti);   // keeps every embedded block - no data loss
        MarkChanged();
    }

    if (ImGui::DragFloat("Start Delay (s)", &e.startDelay, 0.01f, 0.f, 30.f, "%.3f")) MarkChanged();

    bool ovr = (e.durationOverride >= 0.f);
    if (ImGui::Checkbox("Override lifetime", &ovr))
    {
        e.durationOverride = ovr ? 1.0f : -1.0f;    // <0 = inherit notify/prefab
        MarkChanged();
    }
    if (ovr)
    {
        ImGui::SameLine();
        float d = e.durationOverride < 0.f ? 0.f : e.durationOverride;
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::DragFloat("DUR (s)", &d, 0.01f, 0.f, 120.f, "%.3f")) { e.durationOverride = d < 0.f ? 0.f : d; MarkChanged(); }
        if (e.durationOverride == 0.f)
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                               "DUR 0 = immortal (no lifetime) — the entity is never destroyed.");
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(inherits notify Duration, then the file's DURATION)");
    }

    if (ImGui::CollapsingHeader("Local Transform (P / R / S)"))
        if (Reflect::DrawObject(&e.localTransform)) MarkChanged();

    ImGui::Separator();
}

void DrawEmitterBody(VFXEmitterSpec& e)
{
    bool changed = false;
    switch (e.type)
    {
    case VFXEmitterType::Particle:
        ImGui::SeparatorText("Appearance");
        changed |= Reflect::DrawObject(&e.particleRender);
        ImGui::SeparatorText("Emit Behaviour");
        changed |= Reflect::DrawObject(&e.particle);
        ImGui::SeparatorText("Look Material (optional)");
        changed |= StringInput("material (.imat)", e.materialPath, "IMAT_PATH");
        ImGui::TextDisabled("Overrides the look via a MaterialComponent (custom shader/blend/params).");
        break;
    case VFXEmitterType::Trail:
        ImGui::SeparatorText("Trail");
        changed |= Reflect::DrawObject(&e.trail);
        ImGui::SeparatorText("Look Material (optional)");
        changed |= StringInput("material (.imat)", e.materialPath, "IMAT_PATH");
        break;
    case VFXEmitterType::Beam:
        ImGui::SeparatorText("Beam (radius / wobble / control points)");
        changed |= Reflect::DrawObject(&e.beam.beam);
        ImGui::SeparatorText("Beam Shader");
        changed |= Reflect::DrawObject(&e.beam);
        break;
    case VFXEmitterType::Decal:
        ImGui::SeparatorText("Decal");
        changed |= Reflect::DrawObject(&e.decal);
        break;
    case VFXEmitterType::Tracer:
        ImGui::SeparatorText("Tracer");
        changed |= Reflect::DrawObject(&e.tracer);
        break;
    case VFXEmitterType::Afterimage:
        ImGui::SeparatorText("Afterimage");
        changed |= Reflect::DrawObject(&e.afterimage);
        break;
    case VFXEmitterType::Mesh:
        ImGui::SeparatorText("Mesh");
        changed |= Reflect::DrawObject(&e.mesh);
        break;
    default: break;
    }
    if (changed) MarkChanged();
}

// ==== Timeline canvas ====
// One lane per emitter; each emitter is ONE bar spanning [startDelay,
// startDelay + effectiveDuration]. Body-drag moves startDelay; edge-drag
// resizes (and, for an inherit-duration bar, flips it to a concrete
// durationOverride). Clicking a bar drives the same stable-id selection the
// list uses. View math / interactions mirror NotifyTrackEditor.
float EffDur(const VFXEmitterSpec& e)
{
    return e.durationOverride >= 0.f ? e.durationOverride : g_vfx.prefab.defaultDuration;
}
ImU32 TypeColor(VFXEmitterType t)
{
    switch (t) {
    case VFXEmitterType::Particle:   return IM_COL32( 90, 150, 230, 255);
    case VFXEmitterType::Trail:      return IM_COL32(230, 170,  80, 255);
    case VFXEmitterType::Beam:       return IM_COL32(200, 110, 220, 255);
    case VFXEmitterType::Decal:      return IM_COL32(120, 200, 120, 255);
    case VFXEmitterType::Tracer:     return IM_COL32(230, 110, 110, 255);
    case VFXEmitterType::Afterimage: return IM_COL32(110, 210, 210, 255);
    case VFXEmitterType::Mesh:       return IM_COL32(180, 180, 120, 255);
    default:                         return IM_COL32(150, 150, 150, 255);
    }
}
float NiceStep(float target)
{
    const float steps[] = { 0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 20.f };
    for (float s : steps) if (s >= target) return s;
    return 30.f;
}

void DrawTimeline()
{
    const int   N      = static_cast<int>(g_vfx.prefab.emitters.size());
    const float ruler  = 22.f, laneH = 24.f;
    const float wantH  = ruler + N * laneH + 8.f;
    const float childH = wantH < 70.f ? 70.f : (wantH > 260.f ? 260.f : wantH);

    ImGui::BeginChild("vfxTimeline", ImVec2(0, childH), true, ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO&    io = ImGui::GetIO();

    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  availW  = ImGui::GetContentRegionAvail().x;
    const float  canvasH = ruler + N * laneH + 4.f;

    ImGui::InvisibleButton("##vfxTLcanvas", ImVec2(availW, canvasH < 1.f ? 1.f : canvasH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool  hovered = ImGui::IsItemHovered();
    const ImVec2 mouse  = io.MousePos;

    auto pxOf = [&](float t) { return origin.x + t * g_vfx.pps - g_vfx.scrollX; };
    auto tOf  = [&](float px){ return (px - origin.x + g_vfx.scrollX) / g_vfx.pps; };
    auto snap = [&](float t) { if (io.KeyShift) t = std::round(t * 60.f) / 60.f; return t < 0.f ? 0.f : t; };

    // Longest emitter end (for scroll clamp).
    float maxT = 1.f;
    for (const auto& e : g_vfx.prefab.emitters) maxT = std::max(maxT, e.startDelay + EffDur(e));
    auto clampScroll = [&]() {
        const float maxScroll = std::max(0.f, maxT * g_vfx.pps - availW + 60.f);
        if (g_vfx.scrollX < 0.f)         g_vfx.scrollX = 0.f;
        if (g_vfx.scrollX > maxScroll)   g_vfx.scrollX = maxScroll;
    };

    // Wheel-zoom pivoting on the cursor; MMB pan.
    if (hovered && io.MouseWheel != 0.f) {
        const float tBefore = tOf(mouse.x);
        g_vfx.pps = std::clamp(g_vfx.pps * (io.MouseWheel > 0.f ? 1.15f : 1.f / 1.15f), 20.f, 2000.f);
        g_vfx.scrollX = (tBefore * g_vfx.pps) - (mouse.x - origin.x);
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        g_vfx.scrollX -= io.MouseDelta.x;
    clampScroll();

    // Background + ruler.
    dl->AddRectFilled(origin, ImVec2(origin.x + availW, origin.y + canvasH), IM_COL32(28, 28, 32, 255));
    dl->AddRectFilled(origin, ImVec2(origin.x + availW, origin.y + ruler),   IM_COL32(40, 40, 46, 255));
    const float step = NiceStep(80.f / g_vfx.pps);
    for (float t = 0.f; ; t += step) {
        const float x = pxOf(t);
        if (x > origin.x + availW) break;
        if (x >= origin.x) {
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + canvasH), IM_COL32(60, 60, 68, 120));
            char lbl[24]; std::snprintf(lbl, sizeof(lbl), "%.2fs", t);
            dl->AddText(ImVec2(x + 2, origin.y + 3), IM_COL32(170, 170, 180, 255), lbl);
        }
        if (t > maxT + step) break;
    }

    // Lanes + bars.
    int   hitId    = 0;   // emitter clicked this frame (press)
    int   hitMode  = DRAG_NONE;
    for (int i = 0; i < N; ++i) {
        VFXEmitterSpec& e = g_vfx.prefab.emitters[i];
        const float laneY = origin.y + ruler + i * laneH;
        if ((i & 1) == 0)
            dl->AddRectFilled(ImVec2(origin.x, laneY), ImVec2(origin.x + availW, laneY + laneH),
                              IM_COL32(34, 34, 40, 255));

        const float t0 = e.startDelay, t1 = e.startDelay + EffDur(e);
        float x0 = pxOf(t0), x1 = pxOf(t1);
        if (x1 - x0 < 3.f) x1 = x0 + 3.f;
        const ImVec2 bmin(x0, laneY + 3.f), bmax(x1, laneY + laneH - 3.f);
        const bool selected = (g_vfx.selId == g_vfx.ids[i]);
        const bool inherit  = (e.durationOverride < 0.f);
        ImU32 col = TypeColor(e.type);
        dl->AddRectFilled(bmin, bmax, inherit ? (col & 0x60FFFFFF) : (col & 0xC0FFFFFF), 3.f);
        dl->AddRect(bmin, bmax, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 180),
                    3.f, 0, selected ? 2.f : 1.f);
        // resize handles
        dl->AddRectFilled(ImVec2(x0, bmin.y), ImVec2(x0 + 3.f, bmax.y), IM_COL32(255, 255, 255, 90));
        dl->AddRectFilled(ImVec2(x1 - 3.f, bmin.y), ImVec2(x1, bmax.y), IM_COL32(255, 255, 255, 90));
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%s%s", kTypeNames[static_cast<int>(e.type)], inherit ? " (inherit)" : "");
        dl->PushClipRect(bmin, bmax, true);
        dl->AddText(ImVec2(x0 + 5.f, laneY + 4.f), IM_COL32(15, 15, 20, 255), lbl);
        dl->PopClipRect();

        // Hit-test on left press.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            mouse.y >= laneY && mouse.y < laneY + laneH &&
            mouse.x >= x0 - 4.f && mouse.x <= x1 + 4.f)
        {
            hitId = static_cast<int>(g_vfx.ids[i]);
            if (mouse.x <= x0 + 5.f)                        hitMode = DRAG_RESIZE_START;
            else if (mouse.x >= x1 - 5.f)                   hitMode = DRAG_RESIZE_END;
            else                                            hitMode = DRAG_MOVE;
        }
    }

    // Begin a drag.
    if (hitId != 0) {
        g_vfx.selId    = static_cast<uint32_t>(hitId);
        g_vfx.dragId   = static_cast<uint32_t>(hitId);
        g_vfx.dragMode = hitMode;
        const int si = SelIndex();
        if (si >= 0) {
            g_vfx.dragStartDelay = g_vfx.prefab.emitters[si].startDelay;
            g_vfx.dragDur        = EffDur(g_vfx.prefab.emitters[si]);
            g_vfx.dragMouseX     = mouse.x;
        }
    }

    // Apply the active drag (find the dragged emitter by id - index-safe).
    if (g_vfx.dragMode != DRAG_NONE && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int di = -1;
        for (std::size_t k = 0; k < g_vfx.ids.size(); ++k) if (g_vfx.ids[k] == g_vfx.dragId) { di = static_cast<int>(k); break; }
        if (di >= 0) {
            VFXEmitterSpec& e = g_vfx.prefab.emitters[di];
            const float dt = (mouse.x - g_vfx.dragMouseX) / g_vfx.pps;
            // Only write + MarkChanged on an ACTUAL value change. A plain
            // click-to-select is a zero-delta press on the same frame; without
            // this guard it would dirty the file, push a no-op undo entry, and
            // wipe the redo stack (CommitTick fires on release).
            bool changed = false;
            if (g_vfx.dragMode == DRAG_MOVE) {
                const float ns = snap(g_vfx.dragStartDelay + dt);
                if (ns != e.startDelay) { e.startDelay = ns; changed = true; }
            } else if (g_vfx.dragMode == DRAG_RESIZE_END) {
                const float nd = std::max(kMinBarDur, g_vfx.dragDur + dt);   // concrete now
                if (nd != e.durationOverride) { e.durationOverride = nd; changed = true; }
            } else { // DRAG_RESIZE_START: move the left edge, keep the end fixed
                const float end = g_vfx.dragStartDelay + g_vfx.dragDur;
                float       ns  = snap(g_vfx.dragStartDelay + dt);
                if (ns > end - kMinBarDur) ns = end - kMinBarDur;
                if (ns < 0.f) ns = 0.f;                                       // never author a negative Start Delay
                const float nd  = std::max(kMinBarDur, end - ns);
                if (ns != e.startDelay || nd != e.durationOverride) { e.startDelay = ns; e.durationOverride = nd; changed = true; }
            }
            if (changed) MarkChanged();
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_vfx.dragMode = DRAG_NONE;

    ImGui::EndChild();
    ImGui::TextDisabled("Timeline: drag a bar to set Start Delay; drag an edge to set duration "
                        "(flips 'inherit' to a fixed DUR). Wheel = zoom, middle-drag = pan, Shift = frame-snap.");
}
} // namespace

void EditorLayer::OpenVFXAsset(const std::string& path)
{
    std::snprintf(g_vfx.pathBuf, sizeof(g_vfx.pathBuf), "%s", path.c_str());
    VFXPrefab p;
    if (Resource::LoadVFXPrefab(path, p) && !p.emitters.empty())
    {
        g_vfx.prefab     = std::move(p);
        g_vfx.loadedPath = path;
        g_vfx.loaded     = true;
        g_vfx.dirty      = false;
        RebuildIds();
        ResetHistory();
        g_vfx.status = "Loaded";
    }
    else g_vfx.status = "Load failed / empty (see log)";
    m_showVFXEditor = true;
}

void EditorLayer::RenderVFXEditorWindow()
{
    if (!m_showVFXEditor) return;

    // One-time round-trip self-test: checks the writer / loader fixpoint on the
    // shipped sample files and logs PASS / FAIL.
    static bool s_selfTestRan = false;
    if (!s_selfTestRan) { s_selfTestRan = true; Resource::VFXPrefabSelfTest(); }

    // The preview spawner belongs to the world it was made in: drop the
    // reference when the epoch moves (a Clear, or the editor's Stop bringing
    // back the pre-Play world, whose handles validate again).
    if (m_world && m_world->Epoch() != g_vfx.lastWorldEpoch)
    {
        g_vfx.previewSpawner = EntityHandle{};
        g_vfx.lastWorldEpoch = m_world->Epoch();
    }

    ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_FirstUseEver);
    // The visible title is the Window-menu label. Do not change the "###" id: a saved
    // imgui.ini layout finds the window by it.
    if (!ImGui::Begin("VFX Editor###VFX Editor (.ivfx)", &m_showVFXEditor))
    {
        ImGui::End();
        return;
    }

    // Ctrl+Z / Ctrl+Y (only when not typing into a text field, which owns them).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) DoUndo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) DoRedo();
    }

    // ---- Toolbar ----
    if (ImGui::Button("New")) NewPrefab();
    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        VFXPrefab p;
        if (Resource::LoadVFXPrefab(g_vfx.pathBuf, p) && !p.emitters.empty())
        {
            g_vfx.prefab     = std::move(p);
            g_vfx.loadedPath = g_vfx.pathBuf;
            g_vfx.loaded     = true;
            g_vfx.dirty      = false;
            RebuildIds();
            ResetHistory();
            g_vfx.status = "Loaded";
        }
        else g_vfx.status = "Load failed / empty (see log)";
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_world || m_selectedEntity == NullEntity);
    if (ImGui::Button("Import from Selected"))
    {
        // Pull every trail / particle / beam off the selected entity's logical
        // subtree into emitter specs (direct struct copy). Appends to the open
        // effect (or starts a fresh one if none is loaded), as one undo step.
        if (m_world && m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity))
        {
            if (!g_vfx.loaded) { g_vfx.prefab = VFXPrefab{}; g_vfx.ids.clear(); g_vfx.loaded = true; ResetHistory(); }
            int added = 0, skipped = 0;
            ImportEffectsFromEntity(*m_world, m_selectedEntity, added, skipped);
            if (added > 0) { g_vfx.selId = g_vfx.ids.back(); MarkChanged(); }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Imported %d emitter(s)%s", added,
                          skipped > 0 ? " (flock/ambient not importable)" : (added == 0 ? " — none found on selection" : ""));
            g_vfx.status = buf;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reads trail/particle/beam components off the selected entity\n"
                          "(and its attached child entities) into this effect. Instantiate\n"
                          "a prefab in the scene, select it, then Import.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_vfx.loaded);
    if (ImGui::Button("Save"))
    {
        if (Resource::SaveVFXPrefab(g_vfx.pathBuf, g_vfx.prefab))
        {
            g_vfx.loadedPath = g_vfx.pathBuf;
            g_vfx.dirty      = false;
            // Evict the runtime cache so the just-saved file is re-read the next
            // time a notify (or preview) fires it - no editor restart needed.
            InvalidateVFXPrefabCache(g_vfx.pathBuf);
            g_vfx.status = "Saved";
        }
        else g_vfx.status = "Save failed (see log)";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_vfx.loaded || !m_world);
    if (ImGui::Button("Preview"))
    {
        // Kill any prior transient spawner, inject the CURRENT (possibly unsaved)
        // prefab, and fire it. Prefer the selected scene entity (rides its motion:
        // a swinging weapon shows real trails); else a transient spawner at the
        // origin that self-cleans via a LifetimeComponent.
        if (m_world->IsHandleValid(g_vfx.previewSpawner))
            m_world->DestroyEntity(g_vfx.previewSpawner.entity);
        g_vfx.previewSpawner = EntityHandle{};

        // Preview a copy with immortal emitters (explicit DUR 0) clamped to a
        // finite lifetime - otherwise each Preview leaks a Detached, never-dying
        // instance that Stop cannot reach. The user's edited prefab is untouched.
        VFXPrefab previewCopy = g_vfx.prefab;
        for (auto& e : previewCopy.emitters)
            if (e.durationOverride == 0.f) e.durationOverride = 5.0f;
        SetVFXPreviewPrefab(previewCopy);

        Entity        spawner;
        VFXAttachMode mode;
        if (m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity) &&
            m_world->HasComponent<GlobalTransform>(m_selectedEntity))
        {
            spawner = m_selectedEntity;
            mode    = VFXAttachMode::FollowEntity;
        }
        else
        {
            spawner = m_world->CreateEntity();
            GlobalTransform gt;
            DirectX::XMStoreFloat4x4(&gt.matrix, DirectX::XMMatrixTranslation(0.f, 1.f, 0.f));
            m_world->AddComponent<GlobalTransform>(spawner, gt);
            m_world->AddComponent<SceneTransientTag>(spawner, {});
            LifetimeComponent lt; lt.remaining = 12.f; lt.total = 12.f;
            m_world->AddComponent<LifetimeComponent>(spawner, lt);
            g_vfx.previewSpawner = m_world->MakeHandle(spawner);
            mode = VFXAttachMode::Detached;
        }

        auto* mb = m_world->GetComponent<PendingVFXSpawns>(spawner);
        if (!mb) { m_world->AddComponent<PendingVFXSpawns>(spawner, {}); mb = m_world->GetComponent<PendingVFXSpawns>(spawner); }
        PendingVFXSpawns::Spawn sp;
        sp.assetPath = kVFXPreviewAssetKey;
        sp.mode      = mode;
        sp.boneIndex = -1;
        sp.localPos  = { 0.f, 0.f, 0.f };
        sp.duration  = std::max(g_vfx.prefab.defaultDuration, 1.0f);  // positive so inherit-lifetime emitters clean up
        mb->spawns.push_back(std::move(sp));
        g_vfx.status = (mode == VFXAttachMode::FollowEntity)
                     ? "Preview fired on selected entity" : "Preview fired at origin";
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop") && m_world)
    {
        if (m_world->IsHandleValid(g_vfx.previewSpawner))
            m_world->DestroyEntity(g_vfx.previewSpawner.entity);
        g_vfx.previewSpawner = EntityHandle{};
        g_vfx.status = "Preview stopped (in-flight emitters fade on their timers)";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_vfx.undoStack.empty());
    if (ImGui::Button("Undo")) DoUndo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_vfx.redoStack.empty());
    if (ImGui::Button("Redo")) DoRedo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320.f);
    ImGui::InputText("##path", g_vfx.pathBuf, sizeof(g_vfx.pathBuf));
    if (ImGui::BeginDragDropTarget())   // drop a .ivfx from the Asset panel to open it
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IVFX_PATH"))
        {
            const std::string dropped(static_cast<const char*>(p->Data),
                                      p->DataSize > 0 ? p->DataSize - 1 : 0);
            OpenVFXAsset(dropped);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button("Self-Test"))
    {
        const bool ok = Resource::VFXPrefabSelfTest();
        g_vfx.status = ok ? "Round-trip self-test PASS (see log)"
                          : "Round-trip self-test FAIL (see log)";
    }

    if (!g_vfx.status.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.f), "%s%s",
                           g_vfx.status.c_str(), g_vfx.dirty ? " *" : "");
    }

    if (!g_vfx.loaded)
    {
        ImGui::Separator();
        ImGui::TextDisabled("No effect open. Type a path and press Load, or press New.");
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(160.f);
    if (ImGui::DragFloat("Default Duration (s)", &g_vfx.prefab.defaultDuration, 0.01f, 0.f, 120.f, "%.3f"))
        MarkChanged();
    ImGui::Separator();

    DrawTimeline();
    ImGui::Separator();

    const int sel = SelIndex();

    // ---- Left: emitter list ----
    ImGui::BeginChild("vfxEmitterList", ImVec2(210, 0), true);
    ImGui::TextDisabled("Emitters (%d)", static_cast<int>(g_vfx.prefab.emitters.size()));
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(g_vfx.prefab.emitters.size()); ++i)
    {
        const VFXEmitterSpec& e = g_vfx.prefab.emitters[i];
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%d: %s###em%u", i, kTypeNames[static_cast<int>(e.type)], g_vfx.ids[i]);
        if (ImGui::Selectable(lbl, g_vfx.selId == g_vfx.ids[i])) g_vfx.selId = g_vfx.ids[i];
    }
    ImGui::Separator();
    if (ImGui::Button("+ Add")) AddEmitter();
    ImGui::SameLine();
    ImGui::BeginDisabled(sel < 0);
    if (ImGui::Button("Duplicate")) DuplicateEmitter(sel);
    if (ImGui::Button("Remove"))    RemoveEmitter(sel);
    ImGui::SameLine();
    if (ImGui::Button("Up")   && sel > 0)                                                        SwapEmitter(sel, sel - 1);
    ImGui::SameLine();
    if (ImGui::Button("Down") && sel >= 0 && sel < static_cast<int>(g_vfx.prefab.emitters.size()) - 1) SwapEmitter(sel, sel + 1);
    ImGui::EndDisabled();
    ImGui::EndChild();

    // ---- Right: field editor for the selected emitter ----
    ImGui::SameLine();
    ImGui::BeginChild("vfxEmitterEditor", ImVec2(0, 0), true);
    const int sel2 = SelIndex();   // re-resolve (a list op above may have changed it)
    if (sel2 >= 0)
    {
        VFXEmitterSpec& e = g_vfx.prefab.emitters[sel2];
        ImGui::PushID(static_cast<int>(g_vfx.selId));
        DrawEmitterHeader(e);
        DrawEmitterBody(e);
        ImGui::PopID();
    }
    else
    {
        ImGui::TextDisabled("Select an emitter, or press + Add.");
    }
    ImGui::EndChild();

    CommitTick();   // fold this frame's edits into one undo entry when idle
    ImGui::End();
}

// ==== Unsaved-documents guard ====

std::string EditorLayer::UnsavedVfxPath() const
{
    if (!g_vfx.loaded || !g_vfx.dirty) return std::string();
    return g_vfx.loadedPath.empty() ? std::string(g_vfx.pathBuf) : g_vfx.loadedPath;
}

bool EditorLayer::SaveVfxNow(std::string& why)
{
    // The same write as the toolbar's Save: to the path field, then the runtime cache re-reads it.
    if (!Resource::SaveVFXPrefab(g_vfx.pathBuf, g_vfx.prefab))
    {
        why = std::string("could not save '") + g_vfx.pathBuf + "'";
        g_vfx.status = "Save failed (see log)";
        return false;
    }
    g_vfx.loadedPath = g_vfx.pathBuf;
    g_vfx.dirty      = false;
    InvalidateVFXPrefabCache(g_vfx.pathBuf);
    g_vfx.status = "Saved";
    return true;
}
