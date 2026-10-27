#include "Editor/ColorWidgets.h"
#include "Editor/SequencerWindow.h"

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"   // SceneTransientTag
#include "Editor/FontAwesomeIcons.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include "Sequencer/SequenceComponents.h"
#include "Sequencer/SequenceEval.h"
#include "Sequencer/SequencerSystem.h"
#include "System/Log.h"
#include "System/PathUtil.h"   // Sys::RelativizeAssetPath

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Editor
{

namespace
{
    constexpr int kMaxUndo = 64;

    // RelativizeAssetPath keeps whatever separators the OS dialog returned, so a
    // picked path may mix in backslashes. AssetFS normalizes its own lookups, so
    // it still loads, but the path is serialized onto the player and compared
    // against the cache key: two spellings would be two cache entries for one file.
    std::string NormalizeSlashes(std::string p)
    {
        for (char& c : p) if (c == '\\') c = '/';
        return p;
    }

    ImU32 TrackColor(Seq::TrackType t)
    {
        switch (Seq::TrackEvalMode(t))
        {
        case Seq::EvalMode::Absolute:   return IM_COL32( 70, 120, 200, 255);   // blue
        case Seq::EvalMode::RangeState: return IM_COL32( 60, 150, 110, 255);   // green
        case Seq::EvalMode::OneShot:    return IM_COL32(190, 110,  60, 255);   // amber
        }
        return IM_COL32(100, 100, 110, 255);
    }

    const char* EvalModeLabel(Seq::EvalMode m)
    {
        switch (m)
        {
        case Seq::EvalMode::Absolute:   return "absolute (scrubbable)";
        case Seq::EvalMode::RangeState: return "range-state";
        case Seq::EvalMode::OneShot:    return "one-shot (suppressed on seek)";
        }
        return "?";
    }

    const char* RoleName(uint16_t r)
    {
        switch (r)
        {
        case Seq::Role::LocX:   return "Loc X";
        case Seq::Role::LocY:   return "Loc Y";
        case Seq::Role::LocZ:   return "Loc Z";
        case Seq::Role::RotX:   return "Rot X";
        case Seq::Role::RotY:   return "Rot Y";
        case Seq::Role::RotZ:   return "Rot Z";
        case Seq::Role::ScaleX: return "Scale X";
        case Seq::Role::ScaleY: return "Scale Y";
        case Seq::Role::ScaleZ: return "Scale Z";
        case Seq::Role::Weight:    return "Weight";
        case Seq::Role::AnimTime:  return "Anim Time";
        case Seq::Role::Volume:    return "Volume";
        case Seq::Role::Pitch:     return "Pitch";
        case Seq::Role::TimeScale: return "Time Scale";
        case Seq::Role::Fov:       return "FOV";
        default: break;
        }
        static char s_buf[32];
        std::snprintf(s_buf, sizeof(s_buf), "Scalar %u", static_cast<unsigned>(r));
        return s_buf;
    }
}

// ==== Document ====

void SequencerWindow::Open(const std::string& path)
{
    Seq::Sequence s;
    if (!Seq::LoadSequence(path, s))
    {
        LOG_WARNING("Sequencer: failed to open '%s'", path.c_str());
        return;
    }
    // Revoke this window's unsaved document from the cache before adopting the
    // new one, so a Reload really goes back to disk instead of re-publishing the
    // copy it is about to discard.
    RevokePreviewDoc();

    m_seq    = std::move(s);
    m_path   = NormalizeSlashes(path);
    m_dirty  = false;
    m_loaded = true;
    m_previewDirty = true;   // publish this document on the next previewed frame
    m_undo.clear();
    m_redo.clear();
    m_selTrack = m_selSection = m_selChannel = m_selKey = -1;
    m_frame = 0;
    m_workStart = 0;
    m_workEnd   = 0;
    m_playing = false;
    m_canvas.FrameAll();
    LOG_SUCCESS("Sequencer: opened '%s' (%zu bindings, %zu tracks)",
                path.c_str(), m_seq.bindings.size(), m_seq.tracks.size());
}

bool SequencerWindow::NewSequence(const std::string& path)
{
    if (path.empty()) return false;

    // A useful starting document, not an empty one: 5 seconds at 30 fps, a
    // spawnable shot camera, and a camera cut covering the whole range.
    Seq::Sequence s;
    s.frameRate      = 30.f;
    s.durationFrames = 150;
    s.restoreMode    = Seq::RestoreMode::EditorOnly;

    Seq::Binding cam;
    cam.kind      = Seq::BindingKind::Spawnable;
    cam.archetype = Seq::SpawnArchetype::Camera;
    cam.role      = "ShotCam";
    cam.spawnPos  = { 0.f, 1.6f, -4.f };
    s.bindings.push_back(std::move(cam));

    Seq::Track cut;
    cut.type    = Seq::TrackType::CameraCut;
    cut.binding = -1;
    cut.name    = "Camera Cuts";
    Seq::Section sec;
    sec.startFrame = 0;
    sec.endFrame   = s.durationFrames;
    sec.params.SetInt("binding", 0);
    cut.sections.push_back(std::move(sec));
    s.tracks.push_back(std::move(cut));

    s.Normalize();

    if (!Seq::SaveSequence(path, s))
    {
        LOG_ERROR("Sequencer: could not create '%s'", path.c_str());
        return false;
    }
    // Re-open from disk rather than adopting the in-memory copy: that proves the
    // file round-trips before any work goes into it.
    Seq::InvalidateSequenceCache(path);
    Open(path);
    return m_loaded;
}

bool SequencerWindow::Save()
{
    if (m_path.empty()) { LOG_WARNING("Sequencer: no path to save to"); return false; }
    m_seq.Normalize();
    if (Seq::SaveSequence(m_path, m_seq))
    {
        m_dirty = false;
        // Drop the cached copy so live players pick up the edit. The cache holds
        // shared_ptrs, so a player already holding this sequence keeps a valid object
        // until it re-acquires: erasing the entry cannot dangle. A published unsaved
        // copy now matches disk; forgetting the publish makes the next previewed frame
        // re-publish, so the preview evaluates what the window shows either way.
        RevokePreviewDoc();
        Seq::InvalidateSequenceCache(m_path);
        return true;
    }
    return false;
}

// ==== Live document publishing ====

void SequencerWindow::PublishPreviewDoc()
{
    if (!m_loaded || m_path.empty()) return;
    // Re-publish when the document changed or nothing of ours is in the cache:
    // the second case makes toggling Preview off and on show the unsaved document
    // again instead of reverting to disk.
    if (!m_previewDirty && m_publishedPath == m_path) return;

    Seq::PublishSequenceEdit(m_path, m_seq);
    m_publishedPath = m_path;
    m_previewDirty  = false;
}

void SequencerWindow::RevokePreviewDoc()
{
    if (m_publishedPath.empty()) return;
    Seq::InvalidateSequenceCache(m_publishedPath);
    m_publishedPath.clear();
    // The document is unchanged, so it still needs publishing the next time a
    // preview goes up.
    m_previewDirty = true;
}

void SequencerWindow::PushUndo()
{
    m_undo.push_back(Seq::BuildSequenceText(m_seq));
    if (static_cast<int>(m_undo.size()) > kMaxUndo) m_undo.erase(m_undo.begin());
    m_redo.clear();
    MarkEdited();
}

void SequencerWindow::Undo()
{
    if (m_undo.empty()) return;
    m_redo.push_back(Seq::BuildSequenceText(m_seq));
    Seq::ParseSequenceText(m_undo.back(), m_seq);
    m_undo.pop_back();
    MarkEdited();
    m_selSection = m_selChannel = m_selKey = -1;
}

void SequencerWindow::Redo()
{
    if (m_redo.empty()) return;
    m_undo.push_back(Seq::BuildSequenceText(m_seq));
    Seq::ParseSequenceText(m_redo.back(), m_seq);
    m_redo.pop_back();
    MarkEdited();
    m_selSection = m_selChannel = m_selKey = -1;
}

int32_t SequencerWindow::WorkEnd() const
{
    return (m_workEnd > m_workStart) ? m_workEnd : m_seq.durationFrames;
}

// ==== Preview ====
// The window never writes the scene directly. It drives a transient
// SequencePlayerComponent through the ordinary seek request, so previewing and
// shipping share one evaluator, one discontinuity contract, one restore path.

void SequencerWindow::SyncPreview(World* world)
{
    if (!world) return;

    Seq::Preview::SetOwnsViewport(m_cameraLock && m_previewOn);

    if (!m_previewOn || m_path.empty()) { ClosePreview(world); return; }

    // The edit reaches the evaluator here, once per frame, only while a preview
    // is up. SequencerTickSystem sees the cache revision move and hot-swaps the
    // document under the live player without moving its playhead.
    PublishPreviewDoc();

    Entity pe = static_cast<Entity>(m_previewEntity);
    if (pe == NullEntity || !world->IsAlive(pe))
    {
        pe = world->CreateEntity();
        world->SetName(pe, "[Sequencer Preview]");
        // SceneTransientTag keeps it out of a saved scene: a preview player baked
        // into the level would replay a cutscene nobody placed.
        world->AddComponent<SceneTransientTag>(pe, SceneTransientTag{});
        SequencePlayerComponent p;
        p.sequencePath = m_path;
        world->AddComponent<SequencePlayerComponent>(pe, p);
        m_previewEntity = static_cast<uint32_t>(pe);
    }

    auto* p = world->GetComponent<SequencePlayerComponent>(pe);
    if (!p) return;
    if (p->sequencePath != m_path) p->sequencePath = m_path;

    // Who owns the clock depends on whether we are playing. Playing: the player
    // owns it and the window mirrors it - seeking every frame would make every
    // frame a discontinuity, so one-shots would never fire and temporal history
    // would reset continuously. Stopped: the window owns it and every move is a
    // seek, which is right for scrubbing.
    if (m_playing)
    {
        if (!p->playing)
        {
            p->playRequested   = true;
            p->seekRequested   = true;
            p->seekTargetFrame = m_frame;
        }
        p->paused = false;
        p->rate   = m_playRate;

        // Loop (or stop) at the work range, not the asset duration: the work range
        // lets you polish four seconds of a long cinematic without re-watching it all.
        const int32_t we = WorkEnd();
        if (p->frame >= we || p->frame < m_workStart)
        {
            if (m_seq.looping)
            {
                p->seekRequested   = true;
                p->seekTargetFrame = (m_playRate < 0.f) ? we : m_workStart;
            }
            else
            {
                m_playing = false;
                p->paused = true;
            }
        }
        m_frame = std::clamp(p->frame, m_workStart, we);
    }
    else
    {
        p->paused = true;
        if (p->frame != m_frame)
        {
            p->seekRequested   = true;
            p->seekTargetFrame = m_frame;
        }
    }
}

void SequencerWindow::ClosePreview(World* world)
{
    // Unconditional: whatever else goes wrong, the user must get their camera
    // back. This reset is what makes the global safe to have.
    Seq::Preview::SetOwnsViewport(false);

    // Also unconditional: an unsaved document parked in the shared cache would be
    // what a level's own SequencePlayerComponent plays (in Play mode, in another
    // window). The preview owns it; when the preview goes, it goes. A no-op after
    // the first call, so the per-frame preview-off path is free.
    RevokePreviewDoc();

    if (!world) { m_previewEntity = 0; return; }
    const Entity pe = static_cast<Entity>(m_previewEntity);
    if (pe != NullEntity && world->IsAlive(pe))
    {
        // Synchronous release, not a request: stopRequested is consumed by
        // SequencerTickSystem::Update, which never sees a destroyed entity again, so a
        // request would leave the pre-animated state unrestored and orphan every entity
        // the preview spawned (shot cameras, fade quads, subtitle canvases, prefab
        // actors, child sub-sequences) in the live world.
        SequencerTickSystem::ReleasePlayer(*world, pe);
        world->DestroyEntity(pe);
    }
    m_previewEntity = 0;
}

// ==== Record mode ====

bool SequencerWindow::KeyTransformAt(World* world, uint32_t entity, int32_t frame)
{
    if (!world || !m_loaded) return false;
    const Entity e = static_cast<Entity>(entity);
    const auto* lt = world->GetComponent<LocalTransform>(e);
    if (!lt) return false;

    // Which binding is this entity? Resolve through the live preview player, so a
    // role bound by GUID or by child path resolves the way the runtime resolves
    // it; the document alone would miss both.
    int bindingIdx = -1;
    {
        auto* rt = world->GetComponent<SequenceRuntimeComponent>(
            static_cast<Entity>(m_previewEntity));
        if (rt)
            for (std::size_t i = 0; i < rt->bound.size(); ++i)
                if (rt->bound[i] == e) { bindingIdx = static_cast<int>(i); break; }
    }
    if (bindingIdx < 0) return false;

    // First Transform track on that binding; create one when recording onto an
    // actor that has none yet.
    Seq::Track* track = nullptr;
    for (Seq::Track& t : m_seq.tracks)
        if (t.type == Seq::TrackType::Transform && t.binding == bindingIdx) { track = &t; break; }

    PushUndo();

    if (!track)
    {
        Seq::Track t;
        t.type    = Seq::TrackType::Transform;
        t.binding = bindingIdx;
        t.name    = m_seq.bindings[static_cast<std::size_t>(bindingIdx)].role + " Transform";
        t.targetId = Seq::HashTarget(t.name);
        m_seq.tracks.push_back(std::move(t));
        track = &m_seq.tracks.back();
    }

    // The section that contains the playhead, else a new one spanning the whole
    // sequence: recording into a gap must not silently do nothing.
    Seq::Section* sec = nullptr;
    for (Seq::Section& sc : track->sections)
        if (frame >= sc.startFrame && frame < sc.endFrame) { sec = &sc; break; }
    if (!sec)
    {
        Seq::Section sc;
        sc.startFrame = 0;
        sc.endFrame   = std::max(1, m_seq.durationFrames);
        track->sections.push_back(std::move(sc));
        sec = &track->sections.back();
    }

    // Euler degrees, the channel convention. Derived from the live quaternion so
    // a gizmo rotation records exactly what is on screen.
    using namespace DirectX;
    XMFLOAT4X4 m{};
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(
        XMQuaternionNormalize(XMLoadFloat4(&lt->rotation))));
    const float sp = -m._32;
    float rx, ry, rz;
    if (sp >= 0.9999f || sp <= -0.9999f)
    { rx = std::asin(std::clamp(sp, -1.f, 1.f)); ry = std::atan2(-m._13, m._11); rz = 0.f; }
    else
    { rx = std::asin(sp); ry = std::atan2(m._31, m._33); rz = std::atan2(m._12, m._22); }

    const float vals[9] = {
        lt->translation.x, lt->translation.y, lt->translation.z,
        XMConvertToDegrees(rx), XMConvertToDegrees(ry), XMConvertToDegrees(rz),
        lt->scale.x, lt->scale.y, lt->scale.z,
    };

    for (int r = 0; r < 9; ++r)
    {
        Seq::Channel* ch = sec->Find(static_cast<uint16_t>(r));
        if (!ch)
        {
            Seq::Channel nc;
            nc.role = static_cast<uint16_t>(r);
            sec->channels.push_back(std::move(nc));
            ch = &sec->channels.back();
        }
        // Replace an existing key at this exact frame rather than stacking a second
        // one: SortAndDedupe would drop one anyway, and which one depends on insertion
        // order.
        bool replaced = false;
        for (Seq::Key& k : ch->keys)
            if (k.frame == frame) { k.value = vals[r]; replaced = true; break; }
        if (!replaced)
        {
            Seq::Key k;
            k.frame = frame;
            k.value = vals[r];
            ch->keys.push_back(k);
            ch->SortAndDedupe();
        }
    }

    MarkEdited();
    return true;
}

void SequencerWindow::OnTransformCommitted(World* world, uint32_t entity)
{
    if (!Recording()) return;
    if (KeyTransformAt(world, entity, m_frame))
        LOG_INFO("Sequencer: recorded transform keys at frame %d", m_frame);
}

// ==== Rows ====

void SequencerWindow::RebuildRows()
{
    m_rows.clear();
    for (int t = 0; t < static_cast<int>(m_seq.tracks.size()); ++t)
    {
        m_rows.push_back(Row{ t, -1, -1 });
        const bool expanded =
            std::find(m_expandedTracks.begin(), m_expandedTracks.end(), t) != m_expandedTracks.end();
        if (!expanded) continue;
        const Seq::Track& tr = m_seq.tracks[static_cast<std::size_t>(t)];
        for (int s = 0; s < static_cast<int>(tr.sections.size()); ++s)
            for (int c = 0; c < static_cast<int>(tr.sections[static_cast<std::size_t>(s)].channels.size()); ++c)
                m_rows.push_back(Row{ t, s, c });
    }
}

// ==== Toolbar ====

void SequencerWindow::DrawToolbar(World* world)
{
    if (ImGui::Button("New"))
    {
        const std::string p = m_pickSavePath ? m_pickSavePath() : std::string{};
        if (!p.empty()) { ClosePreview(world); NewSequence(NormalizeSlashes(Sys::RelativizeAssetPath(p))); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open"))
    {
        const std::string p = m_pickOpenPath ? m_pickOpenPath() : std::string{};
        if (!p.empty()) { ClosePreview(world); Open(NormalizeSlashes(Sys::RelativizeAssetPath(p))); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save") ) Save();
    ImGui::SameLine();
    if (ImGui::Button("Save As"))
    {
        const std::string p = m_pickSavePath ? m_pickSavePath() : std::string{};
        if (!p.empty())
        {
            m_path = NormalizeSlashes(Sys::RelativizeAssetPath(p));
            Save();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !m_path.empty()) Open(m_path);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty()); if (ImGui::Button("Undo")) Undo(); ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty()); if (ImGui::Button("Redo")) Redo(); ImGui::EndDisabled();

    ImGui::SameLine(); ImGui::TextUnformatted("|"); ImGui::SameLine();

    if (ImGui::Checkbox("Preview", &m_previewOn) && !m_previewOn)
    {
        m_playing = false;
        ClosePreview(world);
    }
    ImGui::SameLine();

    // Real playback. Pressing Play turns Preview on: a transport that silently
    // needs another toggle first reads as broken.
    if (m_playing)
    {
        if (ImGui::Button("Pause")) m_playing = false;
    }
    else if (ImGui::Button("Play"))
    {
        m_playing   = true;
        m_previewOn = true;
        if (m_frame >= WorkEnd()) m_frame = m_workStart;   // rewind at the end
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        m_playing = false;
        m_frame   = m_workStart;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    ImGui::DragFloat("##rate", &m_playRate, 0.01f, -4.f, 4.f, "%.2f x");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Playback rate. Negative plays the sequence backwards; "
                          "one-shots then fire on section END edges.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Record", &m_recordMode)) {}
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Gizmo edits on a bound actor become keys at the playhead. "
                          "Requires Preview (the cast is resolved through the preview player).");
    ImGui::SameLine();
    if (ImGui::Checkbox("Camera Lock", &m_cameraLock))
        Seq::Preview::SetOwnsViewport(m_cameraLock && m_previewOn);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Park the editor free-cam so the sequence's shot camera owns\n"
                          "the viewport. Out-bidding the free-cam on priority would leave\n"
                          "it enabled and blending in; parking it is the honest version.");
    ImGui::SameLine();
    if (ImGui::Button("|<")) { m_playing = false; m_frame = m_workStart; }
    ImGui::SameLine();
    if (ImGui::Button("<|")) { m_playing = false; m_frame = std::max(m_workStart, m_frame - 1); }
    ImGui::SameLine();
    if (ImGui::Button("|>")) { m_playing = false; m_frame = std::min(WorkEnd(), m_frame + 1); }
    ImGui::SameLine();
    if (ImGui::Button(">|")) { m_playing = false; m_frame = WorkEnd(); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    ImGui::DragInt("##frame", &m_frame, 0.5f, 0, m_seq.durationFrames, "f %d");
    ImGui::SameLine();
    ImGui::TextDisabled("%.3f s", m_seq.FrameToSeconds(static_cast<float>(m_frame)));

    ImGui::SameLine(); ImGui::TextUnformatted("|"); ImGui::SameLine();
    if (ImGui::Button("Frame All")) m_canvas.FrameAll();
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &m_snapToFrame);

    // ---- document properties ----
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::DragFloat("FPS", &m_seq.frameRate, 0.25f, 1.f, 240.f, "%.4g")) MarkEdited();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    if (ImGui::DragInt("Duration", &m_seq.durationFrames, 1.f, 1, 1000000, "%d f")) MarkEdited();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::DragInt("##workStart", &m_workStart, 1.f, 0, m_seq.durationFrames, "work %d");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::DragInt("##workEnd", &m_workEnd, 1.f, 0, m_seq.durationFrames, "..%d");
    ImGui::SameLine();
    if (ImGui::Button("Reset Range")) { m_workStart = 0; m_workEnd = 0; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Work range: the sub-span the transport scrubs and loops.\n"
                          "Polishing four seconds of a three-minute cinematic without one\n"
                          "means re-scrubbing the whole thing every iteration.");

    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &m_seq.looping)) MarkEdited();

    ImGui::SameLine();
    {
        int rm = static_cast<int>(m_seq.restoreMode);
        ImGui::SetNextItemWidth(150.f);
        if (ImGui::Combo("Restore", &rm, "Always\0Editor Only\0Never\0"))
        { m_seq.restoreMode = static_cast<Seq::RestoreMode>(rm); MarkEdited(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What happens to the actors when the sequence stops.\n"
                              "Editor Only protects the scene while authoring and leaves a\n"
                              "gameplay cutscene posed where it ended.");
    }

    if (m_dirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "* unsaved");
    }
    if (!m_publishedPath.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.f, 1.f), "live");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The preview evaluates THIS document, unsaved edits included —\n"
                              "no Save needed to see a change.\n\n"
                              "Any SequencePlayerComponent in the level pointing at the same\n"
                              ".iseq sees it too, until the preview is turned off.");
    }
}

// ==== Bindings ====

void SequencerWindow::DrawBindingPanel(World* world, uint32_t selected)
{
    // ---- Live state ----
    // Every failure mode of this system looks like "nothing happened", so the
    // window says what it resolved and what is live right now.
    if (m_previewOn && world)
    {
        const Entity pe = static_cast<Entity>(m_previewEntity);
        if (pe != NullEntity && world->IsAlive(pe))
            if (auto* rt = world->GetComponent<SequenceRuntimeComponent>(pe))
            {
                ImGui::TextDisabled("Live at frame %d", m_frame);
                for (std::size_t i = 0; i < m_seq.bindings.size(); ++i)
                {
                    const Entity b = (i < rt->bound.size()) ? rt->bound[i] : NullEntity;
                    if (b == NullEntity)
                        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "  %s -> UNRESOLVED",
                                           m_seq.bindings[i].role.c_str());
                    else
                        ImGui::TextDisabled("  %s -> %s (%u)", m_seq.bindings[i].role.c_str(),
                                            world->GetName(b).c_str(),
                                            static_cast<uint32_t>(b));
                }
                int liveSections = 0;
                for (const Seq::Track& t : m_seq.tracks)
                    for (const Seq::Section& sc : t.sections)
                        if (sc.EvaluateWeight(static_cast<float>(m_frame)) > 0.f) ++liveSections;
                ImGui::TextDisabled("  %d live section(s), %zu child player(s)",
                                    liveSections, rt->children.size());
                ImGui::Separator();
            }
    }

    ImGui::TextDisabled("Bindings — the sequence declares ROLES; the cast lives on the player");

    int removeIdx = -1;
    for (int i = 0; i < static_cast<int>(m_seq.bindings.size()); ++i)
    {
        Seq::Binding& b = m_seq.bindings[static_cast<std::size_t>(i)];
        ImGui::PushID(i);

        char label[160];
        std::snprintf(label, sizeof(label), "%d  %s  [%s]", i,
                      b.role.empty() ? "(unnamed role)" : b.role.c_str(),
                      b.IsSpawnable() ? Seq::SpawnArchetypeName(b.archetype) : "possess");
        if (ImGui::TreeNode(label))
        {
            char roleBuf[128];
            std::snprintf(roleBuf, sizeof(roleBuf), "%s", b.role.c_str());
            if (ImGui::InputText("Role", roleBuf, sizeof(roleBuf)))
            { PushUndo(); b.role = roleBuf; }

            int kind = static_cast<int>(b.kind);
            if (ImGui::Combo("Kind", &kind, "Possessable\0Spawnable\0"))
            { PushUndo(); b.kind = static_cast<Seq::BindingKind>(kind); }

            if (b.IsSpawnable())
            {
                int arch = static_cast<int>(b.archetype);
                if (ImGui::Combo("Archetype", &arch,
                                 "From Prefab\0Camera\0Post Process Volume\0Fade Quad\0Subtitle Text\0Audio Source\0"))
                { PushUndo(); b.archetype = static_cast<Seq::SpawnArchetype>(arch); }
                if (b.archetype == Seq::SpawnArchetype::FromPrefab)
                {
                    char pb[260];
                    std::snprintf(pb, sizeof(pb), "%s", b.prefabPath.c_str());
                    if (ImGui::InputText("Prefab", pb, sizeof(pb))) { PushUndo(); b.prefabPath = pb; }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IPFB_PATH"))
                        { PushUndo(); b.prefabPath = static_cast<const char*>(pl->Data); }
                        ImGui::EndDragDropTarget();
                    }
                }
                // MarkEdited, not a bare write: without it the toolbar shows no "* unsaved"
                // and the preview never re-publishes the spawn position.
                if (ImGui::DragFloat3("Spawn Pos",   &b.spawnPos.x,   0.01f)) MarkEdited();
                if (ImGui::DragFloat3("Spawn Scale", &b.spawnScale.x, 0.01f)) MarkEdited();
            }
            else
            {
                // The GUID is an authoring convenience only: at runtime the cast on
                // SequencePlayerComponent wins. Say so, or someone bakes a scene GUID into a
                // shared asset and the cutscene works in one level only.
                ImGui::TextDisabled("Default GUID: %s",
                                    b.defaultGuid.IsValid() ? b.defaultGuid.ToHex().c_str() : "(none)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Editor preview fallback only.\n"
                                      "At runtime the cast on the SequencePlayerComponent wins,\n"
                                      "which is what lets this asset run in another level.");
                const Entity sel = static_cast<Entity>(selected);
                ImGui::BeginDisabled(!world || sel == NullEntity);
                if (ImGui::Button("Bind to selection"))
                {
                    PushUndo();
                    auto* gc = world->GetComponent<GuidComponent>(sel);
                    if (!gc)
                    {
                        GuidComponent ng; ng.guid = ECS::Guid::Generate();
                        world->AddComponent<GuidComponent>(sel, ng);
                        ECS::GuidRegistry::Get().Register(ng.guid, sel);
                        gc = world->GetComponent<GuidComponent>(sel);
                    }
                    if (gc) b.defaultGuid = gc->guid;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Clear")) { PushUndo(); b.defaultGuid = ECS::kInvalidGuid; }

                char cb[260];
                std::snprintf(cb, sizeof(cb), "%s", b.childPath.c_str());
                if (ImGui::InputText("Child Path", cb, sizeof(cb))) { PushUndo(); b.childPath = cb; }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("'/'-separated entity names under the bound root.\n"
                                      "Prefab descendants share GUIDs across instances, so they\n"
                                      "are addressed by name path, not by GUID.");
            }

            if (ImGui::Button("Remove Binding")) removeIdx = i;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (removeIdx >= 0)
    {
        PushUndo();
        m_seq.bindings.erase(m_seq.bindings.begin() + removeIdx);
        // Tracks pointing past the end would index out of bounds in the evaluator;
        // Normalize clamps them to -1 (master).
        m_seq.Normalize();
    }

    if (ImGui::Button("+ Binding"))
    {
        PushUndo();
        Seq::Binding b;
        b.role = "Role" + std::to_string(m_seq.bindings.size());
        m_seq.bindings.push_back(std::move(b));
    }
}

// ==== Timeline ====

void SequencerWindow::DrawTrackHeader(int trackIdx, float rowY)
{
    Seq::Track& tr = m_seq.tracks[static_cast<std::size_t>(trackIdx)];
    ImGui::PushID(trackIdx * 1000);

    ImGui::SetCursorScreenPos(ImVec2(m_canvas.origin.x + 2.f, rowY + 2.f));

    const bool expanded =
        std::find(m_expandedTracks.begin(), m_expandedTracks.end(), trackIdx) != m_expandedTracks.end();
    if (ImGui::SmallButton(expanded ? "v" : ">"))
    {
        auto it = std::find(m_expandedTracks.begin(), m_expandedTracks.end(), trackIdx);
        if (it != m_expandedTracks.end()) m_expandedTracks.erase(it);
        else                              m_expandedTracks.push_back(trackIdx);
    }
    ImGui::SameLine(0.f, 4.f);

    // Colour chip = evaluation lane. The lane decides whether scrubbing a track
    // is meaningful, so it belongs in the visual language.
    const ImVec2 chip = ImGui::GetCursorScreenPos();
    m_canvas.dl->AddRectFilled(chip, ImVec2(chip.x + 4.f, chip.y + 14.f), TrackColor(tr.type));
    ImGui::Dummy(ImVec2(6.f, 14.f));
    ImGui::SameLine(0.f, 4.f);

    const bool sel = (m_selTrack == trackIdx && m_selSection < 0);
    if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.9f, 0.5f, 1.f));
    ImGui::TextUnformatted(tr.name.empty() ? Seq::TrackTypeLabel(tr.type) : tr.name.c_str());
    if (sel) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked()) { m_selTrack = trackIdx; m_selSection = -1; m_selChannel = -1; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s%s", Seq::TrackTypeLabel(tr.type),
                          EvalModeLabel(Seq::TrackEvalMode(tr.type)),
                          Seq::TrackIsMaster(tr.type) ? "\nmaster track (no binding)" : "");

    ImGui::SameLine(m_canvas.headerWidth - 74.f);
    if (ImGui::SmallButton(tr.mute ? "M!" : "M")) { PushUndo(); tr.mute = !tr.mute; }
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::SmallButton(tr.solo ? "S!" : "S")) { PushUndo(); tr.solo = !tr.solo; }
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::SmallButton(tr.locked ? "L!" : "L")) { PushUndo(); tr.locked = !tr.locked; }

    ImGui::PopID();
}

void SequencerWindow::DrawSections(int trackIdx, float rowY)
{
    const Seq::Track& tr = m_seq.tracks[static_cast<std::size_t>(trackIdx)];
    const float h  = m_canvas.rowHeight - 6.f;
    const ImU32 col = TrackColor(tr.type);
    const ImU32 colMuted = IM_COL32(90, 90, 95, 200);

    for (int s = 0; s < static_cast<int>(tr.sections.size()); ++s)
    {
        const Seq::Section& sec = tr.sections[static_cast<std::size_t>(s)];
        const float x0 = m_canvas.FrameToX(static_cast<float>(sec.startFrame));
        const float x1 = m_canvas.FrameToX(static_cast<float>(sec.endFrame));
        if (x1 < m_canvas.lanesX - 4.f || x0 > m_canvas.lanesX + m_canvas.lanesW + 4.f) continue;

        const bool selected = (m_selTrack == trackIdx && m_selSection == s);
        const ImU32 body = (!m_seq.TrackAudible(tr) || !sec.active) ? colMuted : col;

        // preRoll is drawn as a hatched lead-in: a section that drives its subsystem
        // before it is "on screen" is otherwise invisible in the UI.
        if (sec.preRoll > 0)
        {
            const float px = m_canvas.FrameToX(static_cast<float>(sec.startFrame - sec.preRoll));
            m_canvas.dl->AddRectFilled(ImVec2(px, rowY + 3.f), ImVec2(x0, rowY + 3.f + h),
                                       IM_COL32(120, 120, 130, 70), 2.f);
        }

        m_canvas.dl->AddRectFilled(ImVec2(x0, rowY + 3.f), ImVec2(x1, rowY + 3.f + h), body, 3.f);
        m_canvas.dl->AddRect(ImVec2(x0, rowY + 3.f), ImVec2(x1, rowY + 3.f + h),
                             selected ? IM_COL32(255, 220, 120, 255) : IM_COL32(20, 20, 24, 200),
                             3.f, 0, selected ? 2.f : 1.f);

        // Ease envelope as two triangles: the blend weight is the shape, so it should
        // be legible without opening the inspector.
        if (sec.easeInFrames > 0)
        {
            const float ex = m_canvas.FrameToX(static_cast<float>(sec.startFrame + sec.easeInFrames));
            m_canvas.dl->AddTriangleFilled(ImVec2(x0, rowY + 3.f + h), ImVec2(ex, rowY + 3.f),
                                           ImVec2(x0, rowY + 3.f), IM_COL32(0, 0, 0, 90));
        }
        if (sec.easeOutFrames > 0)
        {
            const float ex = m_canvas.FrameToX(static_cast<float>(sec.endFrame - sec.easeOutFrames));
            m_canvas.dl->AddTriangleFilled(ImVec2(ex, rowY + 3.f), ImVec2(x1, rowY + 3.f),
                                           ImVec2(x1, rowY + 3.f + h), IM_COL32(0, 0, 0, 90));
        }

        if (x1 - x0 > 30.f)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d-%d", sec.startFrame, sec.endFrame);
            m_canvas.dl->AddText(ImVec2(x0 + 4.f, rowY + 4.f), IM_COL32(240, 240, 250, 220), buf);
        }
    }
}

void SequencerWindow::DrawChannelHeader(int trackIdx, int sectionIdx, int channelIdx, float rowY)
{
    const Seq::Track&   tr  = m_seq.tracks[static_cast<std::size_t>(trackIdx)];
    const Seq::Section& sec = tr.sections[static_cast<std::size_t>(sectionIdx)];
    const Seq::Channel& ch  = sec.channels[static_cast<std::size_t>(channelIdx)];

    ImGui::SetCursorScreenPos(ImVec2(m_canvas.origin.x + 26.f, rowY + 3.f));
    const bool sel = (m_selTrack == trackIdx && m_selSection == sectionIdx &&
                      m_selChannel == channelIdx);
    if (sel) ImGui::TextColored(ImVec4(1.f, 0.9f, 0.5f, 1.f), "%s", RoleName(ch.role));
    else     ImGui::TextDisabled("%s", RoleName(ch.role));
    ImGui::SameLine();
    ImGui::TextDisabled("[s%d, %zu]", sectionIdx, ch.keys.size());
}

void SequencerWindow::DrawChannelKeys(int trackIdx, int sectionIdx, int channelIdx, float rowY)
{
    const Seq::Track& tr = m_seq.tracks[static_cast<std::size_t>(trackIdx)];
    const Seq::Section& sec = tr.sections[static_cast<std::size_t>(sectionIdx)];
    const Seq::Channel& ch  = sec.channels[static_cast<std::size_t>(channelIdx)];

    // Baseline.
    const float mid = rowY + m_canvas.rowHeight * 0.5f;
    m_canvas.dl->AddLine(ImVec2(m_canvas.lanesX, mid),
                         ImVec2(m_canvas.lanesX + m_canvas.lanesW, mid),
                         IM_COL32(60, 60, 70, 120), 1.f);

    for (int k = 0; k < static_cast<int>(ch.keys.size()); ++k)
    {
        const Seq::Key& key = ch.keys[static_cast<std::size_t>(k)];
        const float x = m_canvas.FrameToX(static_cast<float>(key.frame));
        if (x < m_canvas.lanesX - 6.f || x > m_canvas.lanesX + m_canvas.lanesW + 6.f) continue;

        const bool selected = (m_selTrack == trackIdx && m_selSection == sectionIdx &&
                               m_selChannel == channelIdx && m_selKey == k);
        // The diamond shape carries the interpolation mode, so a stepped key is told
        // apart from a cubic one at a glance.
        const float r = selected ? 5.f : 4.f;
        const ImU32 kc = selected ? IM_COL32(255, 225, 120, 255) : IM_COL32(220, 220, 235, 255);
        if (key.interp == Seq::Interp::Constant)
            m_canvas.dl->AddRectFilled(ImVec2(x - r, mid - r), ImVec2(x + r, mid + r), kc);
        else if (key.interp == Seq::Interp::Linear)
            m_canvas.dl->AddTriangleFilled(ImVec2(x, mid - r), ImVec2(x + r, mid + r),
                                           ImVec2(x - r, mid + r), kc);
        else
            m_canvas.dl->AddQuadFilled(ImVec2(x, mid - r), ImVec2(x + r, mid),
                                       ImVec2(x, mid + r), ImVec2(x - r, mid), kc);
    }
}

void SequencerWindow::HandleTimelineInput(World* /*world*/)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool inLanes = m_canvas.lanesHovered;

    auto snap = [&](float f) -> int32_t
    {
        return m_snapToFrame ? m_canvas.SnapFrame(f) : static_cast<int32_t>(std::floor(f));
    };

    // ---- start a drag ----
    if (inLanes && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_drag == Drag::None)
    {
        const float mf = m_canvas.XToFrame(io.MousePos.x);
        // Ruler click -> scrub.
        if (io.MousePos.y < m_canvas.lanesY)
        {
            // Grabbing the scrubber takes the clock back from the player; otherwise the
            // drag and playback fight for the playhead.
            m_playing = false;
            m_drag    = Drag::Playhead;
            m_frame   = std::clamp(snap(mf), 0, m_seq.durationFrames);
        }
        else
        {
            const int rowIdx = m_canvas.RowAtY(io.MousePos.y);
            if (rowIdx >= 0 && rowIdx < static_cast<int>(m_rows.size()))
            {
                const Row& row = m_rows[static_cast<std::size_t>(rowIdx)];
                m_selTrack = row.track;
                Seq::Track& tr = m_seq.tracks[static_cast<std::size_t>(row.track)];

                if (row.channel < 0)
                {
                    // Section row: hit-test the bars, with a 4 px grab zone at each edge for
                    // resize.
                    m_selSection = -1;
                    for (int s = 0; s < static_cast<int>(tr.sections.size()); ++s)
                    {
                        const Seq::Section& sec = tr.sections[static_cast<std::size_t>(s)];
                        const float x0 = m_canvas.FrameToX(static_cast<float>(sec.startFrame));
                        const float x1 = m_canvas.FrameToX(static_cast<float>(sec.endFrame));
                        if (io.MousePos.x < x0 - 4.f || io.MousePos.x > x1 + 4.f) continue;
                        m_selSection    = s;
                        m_selChannel    = -1;
                        m_dragTrack     = row.track;
                        m_dragSection   = s;
                        m_dragGrabFrame = snap(mf);
                        m_dragOrigStart = sec.startFrame;
                        m_dragOrigEnd   = sec.endFrame;
                        if (tr.locked) break;
                        // Snapshot before the drag mutates anything, so the whole drag undoes as one
                        // step.
                        PushUndo();
                        if      (io.MousePos.x <= x0 + 4.f) m_drag = Drag::SectionResizeL;
                        else if (io.MousePos.x >= x1 - 4.f) m_drag = Drag::SectionResizeR;
                        else                                m_drag = Drag::SectionMove;
                        break;
                    }
                }
                else
                {
                    // Channel row: hit-test keys.
                    m_selSection = row.section;
                    m_selChannel = row.channel;
                    m_selKey     = -1;
                    Seq::Section& sec = tr.sections[static_cast<std::size_t>(row.section)];
                    Seq::Channel& ch  = sec.channels[static_cast<std::size_t>(row.channel)];
                    for (int k = 0; k < static_cast<int>(ch.keys.size()); ++k)
                    {
                        const float x = m_canvas.FrameToX(static_cast<float>(ch.keys[static_cast<std::size_t>(k)].frame));
                        if (std::fabs(io.MousePos.x - x) > 6.f) continue;
                        m_selKey = k;
                        if (tr.locked) break;
                        PushUndo();
                        m_drag        = Drag::Key;
                        m_dragTrack   = row.track;
                        m_dragSection = row.section;
                        m_dragChannel = row.channel;
                        m_dragKey     = k;
                        break;
                    }
                }
            }
        }
    }

    // ---- continue a drag ----
    if (m_drag != Drag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const int32_t mf = std::clamp(snap(m_canvas.XToFrame(io.MousePos.x)),
                                      -100000, 100000);
        switch (m_drag)
        {
        case Drag::Playhead:
            m_frame = std::clamp(mf, 0, m_seq.durationFrames);
            break;
        // Each case marks the document edited only when the value moved: marking on
        // every frame of a held drag would re-publish the document, re-seek the player
        // and reset temporal history every frame while the mouse sits still.
        case Drag::SectionMove:
        {
            Seq::Section& sec = m_seq.tracks[static_cast<std::size_t>(m_dragTrack)]
                                    .sections[static_cast<std::size_t>(m_dragSection)];
            const int32_t d  = mf - m_dragGrabFrame;
            const int32_t s0 = m_dragOrigStart + d;
            const int32_t e0 = m_dragOrigEnd   + d;
            if (sec.startFrame != s0 || sec.endFrame != e0)
            {
                sec.startFrame = s0;
                sec.endFrame   = e0;
                MarkEdited();
            }
            break;
        }
        case Drag::SectionResizeL:
        {
            Seq::Section& sec = m_seq.tracks[static_cast<std::size_t>(m_dragTrack)]
                                    .sections[static_cast<std::size_t>(m_dragSection)];
            const int32_t s0 = std::min(mf, sec.endFrame - 1);
            if (sec.startFrame != s0) { sec.startFrame = s0; MarkEdited(); }
            break;
        }
        case Drag::SectionResizeR:
        {
            Seq::Section& sec = m_seq.tracks[static_cast<std::size_t>(m_dragTrack)]
                                    .sections[static_cast<std::size_t>(m_dragSection)];
            const int32_t e0 = std::max(mf, sec.startFrame + 1);
            if (sec.endFrame != e0) { sec.endFrame = e0; MarkEdited(); }
            break;
        }
        case Drag::Key:
        {
            Seq::Channel& ch = m_seq.tracks[static_cast<std::size_t>(m_dragTrack)]
                                   .sections[static_cast<std::size_t>(m_dragSection)]
                                   .channels[static_cast<std::size_t>(m_dragChannel)];
            if (m_dragKey >= 0 && m_dragKey < static_cast<int>(ch.keys.size()))
            {
                Seq::Key& k = ch.keys[static_cast<std::size_t>(m_dragKey)];
                if (k.frame != mf) { k.frame = mf; MarkEdited(); }
            }
            break;
        }
        default: break;
        }
    }

    if (m_drag != Drag::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (m_drag == Drag::Key)
        {
            // Re-sort after the drag, not during: sorting mid-drag reshuffles indices
            // under the cursor and the key jumps to a different one.
            Seq::Channel& ch = m_seq.tracks[static_cast<std::size_t>(m_dragTrack)]
                                   .sections[static_cast<std::size_t>(m_dragSection)]
                                   .channels[static_cast<std::size_t>(m_dragChannel)];
            ch.SortAndDedupe();
            m_selKey = -1;
        }
        m_drag = Drag::None;
    }
}

void SequencerWindow::DrawTimeline(World* world)
{
    RebuildRows();
    const float contentH = m_rows.size() * m_canvas.rowHeight;

    std::vector<int32_t> markers;
    for (const Seq::Track& tr : m_seq.tracks)
        if (tr.type == Seq::TrackType::Marker)
            for (const Seq::Section& s : tr.sections) markers.push_back(s.startFrame);

    // The timeline owns the full height of its column; the inspector has its own
    // column on the right.
    const float avail = std::max(120.f, ImGui::GetContentRegionAvail().y);
    if (m_canvas.Begin("##seqCanvas", avail, m_seq.durationFrames, m_seq.frameRate, contentH))
    {
        m_canvas.HandleZoomPan();
        m_canvas.DrawRangeShading(m_workStart, WorkEnd());
        m_canvas.DrawRuler(&markers);

        // Two passes: pass 1 draws the header column, pass 2 the lane content under a
        // narrowed clip. Do not draw both under one clip: the lane clip culls the
        // widgets that name each track.
        auto rowVisible = [&](float y)
        {
            return !(y + m_canvas.rowHeight < m_canvas.lanesY ||
                     y > m_canvas.lanesY + m_canvas.lanesH);
        };

        m_canvas.PushHeaderClip();
        for (int r = 0; r < static_cast<int>(m_rows.size()); ++r)
        {
            const float y = m_canvas.RowY(r);
            if (!rowVisible(y)) continue;                      // row virtualization
            const Row& row = m_rows[static_cast<std::size_t>(r)];
            if (row.channel < 0) DrawTrackHeader(row.track, y);
            else                 DrawChannelHeader(row.track, row.section, row.channel, y);
        }
        m_canvas.PopHeaderClip();

        m_canvas.PushLaneClip();
        for (int r = 0; r < static_cast<int>(m_rows.size()); ++r)
        {
            const float y = m_canvas.RowY(r);
            if (!rowVisible(y)) continue;
            const Row& row = m_rows[static_cast<std::size_t>(r)];

            if ((r & 1) == 0)
                m_canvas.dl->AddRectFilled(ImVec2(m_canvas.lanesX, y),
                                           ImVec2(m_canvas.lanesX + m_canvas.lanesW, y + m_canvas.rowHeight),
                                           IM_COL32(255, 255, 255, 6));

            if (row.channel < 0) DrawSections(row.track, y);
            else                 DrawChannelKeys(row.track, row.section, row.channel, y);
        }
        m_canvas.PopLaneClip();

        m_canvas.DrawPlayhead(m_frame);
        HandleTimelineInput(world);
    }
    m_canvas.End();
}

// ==== Inspector ====

void SequencerWindow::DrawInspector()
{
    ImGui::BeginChild("##seqInspector", ImVec2(0, 0), true);
    ImGui::TextDisabled("Properties");
    ImGui::Separator();

    if (m_selTrack < 0 || m_selTrack >= static_cast<int>(m_seq.tracks.size()))
    {
        ImGui::TextDisabled("Select a track, section or key in the timeline.");
        ImGui::EndChild();
        return;
    }

    Seq::Track& tr = m_seq.tracks[static_cast<std::size_t>(m_selTrack)];

    // ---- track ----
    ImGui::Text("%s", Seq::TrackTypeLabel(tr.type));
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", EvalModeLabel(Seq::TrackEvalMode(tr.type)));

    char nb[128];
    std::snprintf(nb, sizeof(nb), "%s", tr.name.c_str());
    if (ImGui::InputText("Name", nb, sizeof(nb)))
    {
        PushUndo();
        tr.name = nb;
        tr.targetId = Seq::HashTarget(tr.name);   // Property tracks address by hashed name.
    }

    if (!Seq::TrackIsMaster(tr.type))
    {
        int bindingIdx = tr.binding;
        std::string items;
        for (const Seq::Binding& b : m_seq.bindings) { items += b.role; items.push_back('\0'); }
        items.push_back('\0');
        if (!m_seq.bindings.empty() &&
            ImGui::Combo("Binding", &bindingIdx, items.c_str()))
        { PushUndo(); tr.binding = bindingIdx; }
        if (m_seq.bindings.empty())
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f),
                               "No bindings — this track can never resolve a target.");
    }

    // ---- section ----
    if (m_selSection >= 0 && m_selSection < static_cast<int>(tr.sections.size()))
    {
        Seq::Section& sec = tr.sections[static_cast<std::size_t>(m_selSection)];
        ImGui::Separator();
        ImGui::TextDisabled("Section %d", m_selSection);

        if (ImGui::DragInt("Start", &sec.startFrame, 1.f)) MarkEdited();
        if (ImGui::DragInt("End",   &sec.endFrame,   1.f)) MarkEdited();
        if (ImGui::DragInt("Row",   &sec.row,        0.1f, 0, 16)) MarkEdited();
        if (ImGui::DragFloat("Weight", &sec.weight, 0.01f, 0.f, 4.f)) MarkEdited();
        if (ImGui::Checkbox("Active", &sec.active)) MarkEdited();

        if (ImGui::DragInt("Pre-Roll", &sec.preRoll, 1.f, 0, 600, "%d f"))
        { MarkEdited(); sec.preRoll = std::max(0, sec.preRoll); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Frames evaluated BEFORE the section starts, with the event\n"
                              "lane suppressed and the driven clock held at the section's\n"
                              "first frame — so cloth, chain physics and animation have\n"
                              "settled by the time the shot is actually on screen.");

        if (ImGui::DragInt("Ease In",  &sec.easeInFrames,  1.f, 0, 600, "%d f")) MarkEdited();
        if (ImGui::DragInt("Ease Out", &sec.easeOutFrames, 1.f, 0, 600, "%d f")) MarkEdited();
        int ci = static_cast<int>(sec.easeInCurve), co = static_cast<int>(sec.easeOutCurve);
        if (ImGui::Combo("In Curve",  &ci, "Linear\0Ease In\0Ease Out\0Ease In-Out\0"))
        { PushUndo(); sec.easeInCurve = static_cast<Seq::EaseCurve>(ci); }
        if (ImGui::Combo("Out Curve", &co, "Linear\0Ease In\0Ease Out\0Ease In-Out\0"))
        { PushUndo(); sec.easeOutCurve = static_cast<Seq::EaseCurve>(co); }

        // ---- generic PropertyBag editor ----
        // Reusing the engine's typed variant makes a new track type authorable before
        // it has a dedicated inspector panel.
        ImGui::Separator();
        ImGui::TextDisabled("Params");
        std::vector<std::string> keys;
        keys.reserve(sec.params.values.size());
        for (const auto& kv : sec.params.values) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        std::string removeKey;
        for (const std::string& k : keys)
        {
            ImGui::PushID(k.c_str());
            auto& v = sec.params.values[k];
            if (auto* pi = std::get_if<int>(&v))
            { if (ImGui::DragInt(k.c_str(), pi)) MarkEdited(); }
            else if (auto* pf = std::get_if<float>(&v))
            { if (ImGui::DragFloat(k.c_str(), pf, 0.01f)) MarkEdited(); }
            else if (auto* ps = std::get_if<std::string>(&v))
            {
                char b[512]; std::snprintf(b, sizeof(b), "%s", ps->c_str());
                if (ImGui::InputText(k.c_str(), b, sizeof(b))) { *ps = b; MarkEdited(); }
            }
            else if (auto* p3 = std::get_if<DirectX::XMFLOAT3>(&v))
            { if (ImGui::DragFloat3(k.c_str(), &p3->x, 0.01f)) MarkEdited(); }
            else if (auto* p4 = std::get_if<DirectX::XMFLOAT4>(&v))
            { if (EditorUI::ColorEdit4(k.c_str(), &p4->x)) MarkEdited(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeKey = k;
            ImGui::PopID();
        }
        if (!removeKey.empty()) { PushUndo(); sec.params.values.erase(removeKey); }

        static char s_newKey[64] = "";
        static int  s_newType    = 1;
        ImGui::SetNextItemWidth(120.f);
        ImGui::InputText("##nk", s_newKey, sizeof(s_newKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        ImGui::Combo("##nt", &s_newType, "int\0float\0string\0vec3\0color\0");
        ImGui::SameLine();
        if (ImGui::Button("+ Param") && s_newKey[0])
        {
            PushUndo();
            switch (s_newType)
            {
            case 0: sec.params.values[s_newKey] = 0;            break;
            case 1: sec.params.values[s_newKey] = 0.f;          break;
            case 2: sec.params.values[s_newKey] = std::string(); break;
            case 3: sec.params.values[s_newKey] = DirectX::XMFLOAT3{ 0,0,0 }; break;
            default:sec.params.values[s_newKey] = DirectX::XMFLOAT4{ 1,1,1,1 }; break;
            }
            s_newKey[0] = '\0';
        }

        // ---- channels ----
        ImGui::Separator();
        ImGui::TextDisabled("Channels");
        int removeCh = -1;
        for (int c = 0; c < static_cast<int>(sec.channels.size()); ++c)
        {
            Seq::Channel& ch = sec.channels[static_cast<std::size_t>(c)];
            ImGui::PushID(c);
            ImGui::Text("%s  (%zu keys)", RoleName(ch.role), ch.keys.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Key here"))
            {
                PushUndo();
                Seq::Key k;
                k.frame = m_frame;
                k.value = ch.Evaluate(static_cast<float>(m_frame));
                ch.keys.push_back(k);
                ch.SortAndDedupe();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeCh = c;
            ImGui::PopID();
        }
        if (removeCh >= 0) { PushUndo(); sec.channels.erase(sec.channels.begin() + removeCh); }

        static int s_newRole = 0;
        ImGui::SetNextItemWidth(140.f);
        ImGui::Combo("##role", &s_newRole,
                     "Loc X\0Loc Y\0Loc Z\0Rot X\0Rot Y\0Rot Z\0Scale X\0Scale Y\0Scale Z\0"
                     "Scalar 0\0Weight\0Anim Time\0Volume\0Pitch\0Time Scale\0FOV\0");
        ImGui::SameLine();
        if (ImGui::Button("+ Channel"))
        {
            static const uint16_t kRoles[] = {
                Seq::Role::LocX, Seq::Role::LocY, Seq::Role::LocZ,
                Seq::Role::RotX, Seq::Role::RotY, Seq::Role::RotZ,
                Seq::Role::ScaleX, Seq::Role::ScaleY, Seq::Role::ScaleZ,
                Seq::Role::Scalar0, Seq::Role::Weight, Seq::Role::AnimTime,
                Seq::Role::Volume, Seq::Role::Pitch, Seq::Role::TimeScale, Seq::Role::Fov };
            PushUndo();
            Seq::Channel ch;
            ch.role = kRoles[std::clamp(s_newRole, 0, 15)];
            sec.channels.push_back(std::move(ch));
        }

        // ---- selected key ----
        if (m_selChannel >= 0 && m_selChannel < static_cast<int>(sec.channels.size()) &&
            m_selKey >= 0)
        {
            Seq::Channel& ch = sec.channels[static_cast<std::size_t>(m_selChannel)];
            if (m_selKey < static_cast<int>(ch.keys.size()))
            {
                Seq::Key& k = ch.keys[static_cast<std::size_t>(m_selKey)];
                ImGui::Separator();
                ImGui::TextDisabled("Key");
                if (ImGui::DragInt("Frame", &k.frame, 1.f)) MarkEdited();
                if (ImGui::DragFloat("Value", &k.value, 0.01f)) MarkEdited();
                if (ImGui::DragFloat("Tangent In",  &k.tangentIn,  0.01f)) MarkEdited();
                if (ImGui::DragFloat("Tangent Out", &k.tangentOut, 0.01f)) MarkEdited();
                int ip = static_cast<int>(k.interp);
                if (ImGui::Combo("Interp", &ip, "Constant\0Linear\0Cubic\0"))
                { PushUndo(); k.interp = static_cast<Seq::Interp>(ip); }
                if (ImGui::Button("Delete Key"))
                {
                    PushUndo();
                    ch.keys.erase(ch.keys.begin() + m_selKey);
                    m_selKey = -1;
                }
            }
        }

        if (ImGui::Button("Delete Section"))
        {
            PushUndo();
            tr.sections.erase(tr.sections.begin() + m_selSection);
            m_selSection = -1;
        }
    }
    else
    {
        ImGui::Separator();
        if (ImGui::Button("+ Section"))
        {
            PushUndo();
            Seq::Section s;
            s.startFrame = m_frame;
            s.endFrame   = m_frame + std::max(1, static_cast<int32_t>(m_seq.frameRate));
            tr.sections.push_back(std::move(s));
            m_selSection = static_cast<int>(tr.sections.size()) - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Track"))
        {
            PushUndo();
            m_seq.tracks.erase(m_seq.tracks.begin() + m_selTrack);
            m_selTrack = m_selSection = -1;
            m_expandedTracks.clear();
        }
    }

    ImGui::EndChild();
}

// ==== Draw ====

void SequencerWindow::Draw(bool* open, World* world, uint32_t selected)
{
    // The writer/loader fixpoint is what stands between an artist and silent data
    // loss on Save, so it runs once per session, the first time the window opens
    // (the .ivfx editor does the same).
    static bool s_selfTested = false;
    if (!s_selfTested) { s_selfTested = true; Seq::SequenceSelfTest(); }

    ImGui::SetNextWindowSize(ImVec2(1100.f, 600.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", open, ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::End();
        return;
    }

    if (!m_loaded)
    {
        ImGui::TextDisabled("No sequence open.");
        ImGui::Spacing();

        if (ImGui::Button("New Sequence..."))
        {
            const std::string p = m_pickSavePath ? m_pickSavePath() : std::string{};
            if (!p.empty()) NewSequence(NormalizeSlashes(Sys::RelativizeAssetPath(p)));
        }
        ImGui::SameLine();
        if (ImGui::Button("Open..."))
        {
            const std::string p = m_pickOpenPath ? m_pickOpenPath() : std::string{};
            if (!p.empty()) Open(NormalizeSlashes(Sys::RelativizeAssetPath(p)));
        }

        // Inline path fallback: EditorLayer installs the dialogs; in a host without
        // them (ShaderLab) a typed path is still a working way to make an asset.
        ImGui::Spacing();
        ImGui::TextDisabled("...or create one at a path directly:");
        ImGui::SetNextItemWidth(420.f);
        ImGui::InputText("##newpath", m_newPathBuf, sizeof(m_newPathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Create") && m_newPathBuf[0])
            NewSequence(m_newPathBuf);

        ImGui::Spacing();
        ImGui::TextDisabled("Double-click a .iseq in the asset browser, or drop one here.");
        ImGui::Dummy(ImVec2(400.f, 60.f));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ISEQ_PATH"))
                Open(static_cast<const char*>(pl->Data));
            ImGui::EndDragDropTarget();
        }
        ImGui::End();
        return;
    }

    // Window-scoped hotkeys, deliberately not routed through the scene undo stack
    // (see the header comment).
    ImGuiIO& io = ImGui::GetIO();
    m_wasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_wasFocused)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift) Undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))                 Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) &&  io.KeyShift) Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))                 Save();
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            m_playing = !m_playing;
            if (m_playing) m_previewOn = true;
        }
        // K keys the editor's selection at the playhead, Record or not: the manual
        // path for when auto-key is too eager.
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K) && selected != 0)
            KeyTransformAt(world, selected, m_frame);
    }

    DrawToolbar(world);
    ImGui::Separator();

    // Left: bindings + add-track. Middle: timeline. Right: inspector.
    ImGui::BeginChild("##seqLeft", ImVec2(300.f, 0), true);
    DrawBindingPanel(world, selected);
    ImGui::Separator();
    ImGui::TextDisabled("Tracks");
    static int s_newTrackType = static_cast<int>(Seq::TrackType::Transform);
    {
        std::string items;
        for (int i = 0; i < static_cast<int>(Seq::TrackType::COUNT); ++i)
        { items += Seq::TrackTypeLabel(static_cast<Seq::TrackType>(i)); items.push_back('\0'); }
        items.push_back('\0');
        ImGui::SetNextItemWidth(-1.f);
        ImGui::Combo("##ntt", &s_newTrackType, items.c_str());
    }
    if (ImGui::Button("+ Track", ImVec2(-1.f, 0.f)))
    {
        PushUndo();
        Seq::Track t;
        t.type = static_cast<Seq::TrackType>(s_newTrackType);
        t.name = Seq::TrackTypeLabel(t.type);
        t.targetId = Seq::HashTarget(t.name);
        t.binding = Seq::TrackIsMaster(t.type) ? -1 : (m_seq.bindings.empty() ? -1 : 0);
        m_seq.tracks.push_back(std::move(t));
        m_selTrack = static_cast<int>(m_seq.tracks.size()) - 1;
        m_selSection = -1;
    }
    ImGui::EndChild();

    // Timeline in the middle, inspector in its own right-hand column: stacked
    // under the timeline it would take half the height the timeline needs.
    ImGui::SameLine();
    ImGui::BeginChild("##seqCenter", ImVec2(-(m_inspectorWidth + 8.f), 0), false);
    DrawTimeline(world);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##seqRight", ImVec2(0, 0), false);
    DrawInspector();
    ImGui::EndChild();

    // Work range loop, applied after any input this frame.
    if (m_workEnd > m_workStart)
        m_frame = std::clamp(m_frame, m_workStart, m_workEnd);

    SyncPreview(world);

    // While the gizmo is dragged the evaluator must leave that entity alone;
    // otherwise it rewrites LocalTransform every frame under the drag and the
    // handle cannot move.
    Seq::Suppress::Set(ImGuizmo::IsUsing() ? static_cast<Entity>(selected) : NullEntity);

    ImGui::End();
}

} // namespace Editor

// ==== EditorLayer glue ====
// Defined here rather than in EditorLayer.cpp so the whole Sequencer window
// lives in one translation unit, like VFXEditorWindow.cpp.

#include "Editor/EditorLayer.h"

void EditorLayer::RenderSequencerWindow()
{
    const std::string before = m_sequencerWindow.Path();

    m_sequencerWindow.Draw(&m_showSequencer, m_world,
                           static_cast<uint32_t>(m_selectedEntity));

    // A New / Save As writes a file the asset browser has never seen; without a
    // rescan its tile appears only after the user changes folder.
    if (m_sequencerWindow.Path() != before)
        m_needsRescan = true;

    // Closing the window must tear the preview player down: a transient player
    // that outlives its window keeps holding its actors and nobody can stop it.
    if (!m_showSequencer)
        m_sequencerWindow.ClosePreview(m_world);
}

void EditorLayer::SequencerOnTransformCommitted(uint32_t entity)
{
    m_sequencerWindow.OnTransformCommitted(m_world, entity);
}

void EditorLayer::OpenSequenceAsset(const std::string& path)
{
    // Drop the previous preview before switching documents, or the old
    // sequence's actors stay claimed with their pre-animated state unreleased.
    m_sequencerWindow.ClosePreview(m_world);
    m_sequencerWindow.Open(path);
    m_showSequencer = true;
    ImGui::SetWindowFocus("Sequencer");
}
