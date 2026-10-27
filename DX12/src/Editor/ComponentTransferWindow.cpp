// ComponentTransferWindow.cpp - Tools > Component Transfer.
//
// Pick two targets (each a live entity OR a .ipfb prefab), see one merged row
// per component type with its status (Only A / Only B / Both / identical), and
// push components across in either direction. Part of EditorLayer, compiled
// separately (like ChainPhysicsEditorWindow.cpp / VFXEditorWindow.cpp).
//
// The contract, the transfer paths and the undo model are documented in
// Editor/ComponentTransferWindow.h - read that first. This file is the ImGui
// and the EditorLayer wiring, plus the per-component-type policy table; the
// .ipfb text model and the tag <-> type lookups live in ComponentTransferPrefab.cpp
// so they can be tested without an editor (tests/unit/ct_prefab_test.cpp).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/EditorLayer.h"
#include "Editor/ComponentTransferWindow.h"
#include "Editor/Undo/ComponentBlob.h"     // Serialize/DeserializeComponentBlob
#include "Editor/Undo/EditHistory.h"       // EditMacro
#include "Editor/Undo/ValueEditAction.h"
#include "System/Stage.h"                  // the prefab picker is modal

// Component headers: typeid() for the policy table below.
#include "Audio/AudioComponents.h"         // AudioListenerComponent (singleton)
#include "ECS/AnimationComponents.h"       // skeleton / anim / morph / socket / skinned
#include "ECS/BeamComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/Components.h"                // MaterialComponent, ActiveCameraTag
#include "ECS/DDGIComponents.h"            // DDGIVolumeRuntime, IndirectLightingSettings
#include "ECS/EditorFolderComponent.h"
#include "ECS/FaceShadingComponent.h"
#include "ECS/FacialAnimationComponent.h"  // morph-target control (names, not indices)
#include "ECS/IKRigComponent.h"
#include "ECS/GrassComponent.h"
#include "ECS/GuidComponent.h"
#include "ECS/HairShadowCasterComponent.h"
#include "ECS/HeadTrackComponent.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/StaticMeshComponents.h"      // MeshSectionOf (blocked: derived)
#include "ECS/TrailComponent.h"
#include "ECS/TODComponents.h"             // TODConfig, Sun/MoonLightTag
#include "ECS/WorldEnvironment.h"
#include "Physics/ChainPhysicsSystem.h"    // ChainPhysicsComponent + Invalidate()
#include "Renderer/Renderer.h"             // GetChainPhysicsSystem()
#include "UI/UICanvas.h"                   // UI::UIParent, UI::UIComputedRect

#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"   // the destination keeps its runtime state

#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace CT = Editor::ComponentTransfer;

// ==== File-local helpers ====
namespace
{

// Build the RestoreFn half of an undo entry for one component on one entity.
// A null holder encodes ABSENCE: restoring "the component was not there" is a
// removal, which is what makes a single ValueEditAction cover add, overwrite
// and (on revert) remove without three action classes.
Editor::Undo::RestoreFn MakeRestore(std::type_index ti, std::shared_ptr<void> holder)
{
    return [ti, holder](World& w, Entity e)
    {
        if (holder) w.RestoreComponentByType(e, ti, holder);
        else        w.RemoveComponentByType(e, ti);

        // MaterialComponent carries a dirty flag the renderer polls to re-sync
        // its GPU material slot; a value restore writes the fields behind that
        // flag's back (the Inspector's remove-undo in EditorLayer's Material
        // section does the same SetDirty for the same reason).
        if (holder && ti == std::type_index(typeid(MaterialComponent)))
            if (auto* mc = w.GetComponent<MaterialComponent>(e)) mc->SetDirty();
    };
}

// One side of the merge, per component.
struct SideEntry
{
    std::type_index ti        = std::type_index(typeid(void));
    bool            typeKnown = false;   // resolves to a real component type here
    std::string     tag;                 // "" = no serializer registered
    std::string     blob;                // serializer text; "" = unavailable
    bool            present   = false;
};

// Merge key: the serializer tag when there is one (that is what a prefab and an
// entity have in common), else a type-derived key so serializer-less components
// still get a row.
std::string MergeKey(std::type_index ti, const std::string& tag)
{
    return tag.empty() ? ("#" + std::string(ti.name())) : tag;
}

const ImVec4 kColA      { 0.55f, 0.78f, 1.00f, 1.f };
const ImVec4 kColB      { 1.00f, 0.80f, 0.45f, 1.f };
const ImVec4 kColOk     { 0.55f, 0.85f, 0.55f, 1.f };
const ImVec4 kColWarn   { 1.00f, 0.78f, 0.30f, 1.f };
const ImVec4 kColBlock  { 0.95f, 0.45f, 0.45f, 1.f };
const ImVec4 kColMuted  { 0.60f, 0.60f, 0.60f, 1.f };

// Single-file .ipfb open dialog rooted at asset/: a UTF-8 path, empty on
// cancel. Local to this TU; EditorLayerShared.h's OpenFileDialog is the general
// wrapper.
std::string PickPrefabFile()
{
    static wchar_t s_buf[MAX_PATH];
    s_buf[0] = L'\0';
    // OPENFILENAME filters are double-NUL terminated, which a std::string
    // literal cannot carry: build the wide spec explicitly.
    static const wchar_t kFilter[] = L"Prefab (*.ipfb)\0*.ipfb\0All Files\0*.*\0\0";
    const std::wstring wInitDir = Sys::Utf8ToWide("asset");

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.lpstrFilter     = kFilter;
    ofn.lpstrFile       = s_buf;
    ofn.nMaxFile        = static_cast<DWORD>(std::size(s_buf));
    ofn.lpstrInitialDir = wInitDir.empty() ? nullptr : wInitDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    // Modal: the frame loop waits for the person - `status` says so.
    Sys::Stage::Scope stage("modal_dialog", "pick prefab");
    return GetOpenFileNameW(&ofn) ? Sys::WideToUtf8(s_buf) : std::string{};
}

} // anonymous namespace

// ==== Policy - what a transfer of this component type is allowed to do ====
// Lives here rather than in ComponentTransferPrefab.cpp because it needs
// typeid() of ~40 component types across a dozen headers, and because it is UI
// policy rather than file format.
//
// Three tiers, and the line between them is deliberate:
//   Blocked - the transfer would corrupt something the user cannot see and
//             cannot easily undo: the scene graph, the GUID registry, a GPU
//             descriptor slot, or the on-disk representation of the target.
//   Warn    - mechanically fine, but it means something the user has to intend.
//   Free    - ordinary authored data; copying it is exactly what it looks like.
//
// Anything unlisted defaults to Free, EXCEPT that a type with no serializer is
// downgraded to Warn at row-build time (its only transport is the raw value
// copy). That default is a judgement call: this engine has ~150 component
// types and the tool would be useless if every unvetted one were blocked. The
// hazards that are NOT obvious from a component's name are all enumerated
// below: structural edges, identity, derived-per-frame state, GPU slot
// owners, world singletons, and rig-bound indices.
namespace Editor::ComponentTransfer
{

PolicyInfo PolicyFor(std::type_index ti)
{
    static const std::unordered_map<std::type_index, PolicyInfo> kTable = []
    {
        std::unordered_map<std::type_index, PolicyInfo> m;
        const auto block = [&m](std::type_index t, const char* why)
            { m.emplace(t, PolicyInfo{ Policy::Blocked, Transport::Auto, why }); };
        const auto warn = [&m](std::type_index t, const char* why)
            { m.emplace(t, PolicyInfo{ Policy::Warn, Transport::Auto, why }); };
        // Transferable by TEXT only. Each of these owns something allocated
        // outside the component (a bindless slot, a cubemap slice, a Jolt body),
        // and a raw value copy would hand the same allocation to a second
        // entity, so destroying EITHER frees it under the other. Their
        // serializers are safe because every one of them resets the id to
        // "unallocated" on the way in; the engine re-acquires per entity.
        const auto blobOnly = [&m](std::type_index t, const char* why)
            { m.emplace(t, PolicyInfo{ Policy::Warn, Transport::BlobOnly, why }); };

        // ---- Scene / canvas graph ----
        block(std::type_index(typeid(Parent)),
              "Scene-graph link. Copying it would splice the target under the\n"
              "source's parent without touching either parent's Children list.\n"
              "Re-parent by dragging in the Hierarchy instead.");
        block(std::type_index(typeid(Children)),
              "Scene-graph link. Copying it would make two entities claim the\n"
              "same children, and TransformSystem would drive them twice.");
        block(std::type_index(typeid(UI::UIParent)),
              "Canvas-graph link — the UI equivalent of Parent.");

        // ---- Identity ----
        block(std::type_index(typeid(GuidComponent)),
              "Stable identity. Its deserializer RE-REGISTERS the parsed GUID, so\n"
              "copying it does not just duplicate an id — every AttachmentRef in\n"
              "the world that pointed at the source starts resolving to the\n"
              "target.");
        block(std::type_index(typeid(EditorFolderComponent)),
              "Outliner folder membership, not entity data.");

        // ---- Derived every frame ----
        block(std::type_index(typeid(GlobalTransform)),
              "Derived — TransformSystem rewrites it from Parent x\n"
              "LocalTransform every frame. Copy Local Transform instead.");
        block(std::type_index(typeid(WorldAabb)),
              "Derived from LocalAabb x GlobalTransform every frame.");
        block(std::type_index(typeid(UI::UIComputedRect)),
              "Layout OUTPUT — UICanvasSystem recomputes it every frame, and its\n"
              "cloner blanks it on any copy.");
        block(std::type_index(typeid(MeshSectionOf)),
              "Marks a section entity a Static Mesh DERIVED. MeshInstanceSystem\n"
              "creates and destroys those; copy the owner's Static Mesh instead.");

        // ---- Prefab-instance bookkeeping ----
        block(std::type_index(typeid(PrefabSourcePath)),
              "Marks the entity as a prefab INSTANCE ROOT. Copying it makes the\n"
              "target claim to be an instance of the source's prefab, and\n"
              "SaveScene then writes it as a bare reference — dropping every\n"
              "component it actually owns.");
        block(std::type_index(typeid(PrefabInstanceMember)),
              "Runtime back-pointer to a prefab instance root; LoadPrefab\n"
              "re-establishes it on every instantiation.");

        // ---- GPU slot owners with no serializer ----
        // These hold live descriptor-heap indices, GPU buffers and pose-ring
        // byte offsets. A raw value copy ALIASES them onto a second entity,
        // the same shape as the skinned-reload TDR: two entities reading and
        // freeing one set of descriptors.
        block(std::type_index(typeid(MeshSkinnedComponent)),
              "Owns GPU buffers and a mesh-descriptor slot. A copy would alias\n"
              "one set of descriptors onto two entities — the shape that\n"
              "produces a device hang, not a visual glitch. Spawn the mesh on\n"
              "the target instead (drag its .iscn / .ipfb in).");
        block(std::type_index(typeid(SkinningOutputComponent)),
              "Per-frame pose-ring byte offsets, rewritten every frame.");
        block(std::type_index(typeid(DDGIVolumeRuntimeComponent)),
              "Holds an allocated DDGI volume slot; the manager owns it.");

        // ---- World singletons: copying STEALS them from the source ----
        // Each of these is documented as "exactly one per world" and resolved
        // first-entity-wins, so a second copy does not add anything: it makes
        // which entity is authoritative depend on pool order.
        static constexpr const char* kSingleton =
            "This component is a WORLD SINGLETON (exactly one per scene,\n"
            "resolved first-entity-wins). A second copy does not give the target\n"
            "its own — it makes which of the two is authoritative depend on\n"
            "internal pool order, and the source can silently lose.";
        warn(std::type_index(typeid(WorldEnvironmentComponent)),         kSingleton);
        warn(std::type_index(typeid(TODConfigComponent)),                kSingleton);
        warn(std::type_index(typeid(CloudComponent)),                    kSingleton);
        warn(std::type_index(typeid(GrassComponent)),                    kSingleton);
        warn(std::type_index(typeid(IndirectLightingSettingsComponent)), kSingleton);
        warn(std::type_index(typeid(HairShadowCasterComponent)),
             "One global hair-shadow map is shared by every tagged entity —\n"
             "tagging more of them collapses its texel density.");
        warn(std::type_index(typeid(ActiveCameraTag)),                   kSingleton);
        warn(std::type_index(typeid(Audio::AudioListenerComponent)),     kSingleton);
        warn(std::type_index(typeid(SunLightTag)),                       kSingleton);
        warn(std::type_index(typeid(MoonLightTag)),                      kSingleton);

        // ---- Transferable, but the user has to mean it ----
        warn(std::type_index(typeid(LocalTransform)),
             "Moves, rotates and rescales the target to the source's pose.");
        warn(std::type_index(typeid(LocalAabb)),
             "Derived from the mesh — a box from a different mesh mis-culls\n"
             "the target (too small = pops out, too large = never culls).");
        warn(std::type_index(typeid(MeshLibRef)),
             "Binds a mesh id inside one specific .meshlib. Only meaningful if\n"
             "both entities came from the same library.");
        warn(std::type_index(typeid(SkeletonRef)),
             "Holds a raw Entity id pointing at the SOURCE's character root. Right\n"
             "when both meshes belong to the same character, wrong otherwise —\n"
             "the target would be skinned by someone else's skeleton.");
        warn(std::type_index(typeid(SceneSourcePath)),
             "Provenance. The editor's Duplicate uses it to decide whether to\n"
             "re-instantiate from the .iscn instead of copying components.");
        warn(std::type_index(typeid(MaterialSourcePath)),
             "Points the target at the source's .imat. While present, the\n"
             "inline-material serializer stands down, so the target saves as a\n"
             "REFERENCE to that file rather than as its own data.");

        // ---- Rig-bound: indices resolved against ONE skeleton ----
        static constexpr const char* kRigBound =
            "Holds bone INDICES / clip handles resolved against the source's\n"
            "skeleton. They only mean the same thing on a target with an\n"
            "identical bone list — otherwise it drives the wrong bones.";
        warn(std::type_index(typeid(SkeletonComponent)),
             "Skinned-mesh runtime wiring: assetIndex / boneCount / pose-ring\n"
             "offset only mean something for the SOURCE's skeleton asset.");
        warn(std::type_index(typeid(AnimationComponent)),
             "Playback state indexing the source's clip handles.");
        warn(std::type_index(typeid(MorphComponent)),
             "Morph slots indexed against the source mesh's blend shapes.");
        warn(std::type_index(typeid(AnimationSetComponent)), kRigBound);
        warn(std::type_index(typeid(ChainPhysicsComponent)), kRigBound);
        warn(std::type_index(typeid(SocketComponent)),       kRigBound);
        warn(std::type_index(typeid(IKRigComponent)),       kRigBound);
        warn(std::type_index(typeid(HeadTrackComponent)),    kRigBound);
        warn(std::type_index(typeid(FaceShadingComponent)),  kRigBound);
        // Deliberately NOT kRigBound: that string says "bone INDICES", which is
        // factually wrong here and would send the user looking for the wrong hazard.
        // FacialAnimationComponent stores morph-target NAMES and resolves them to mesh
        // slots every tick, so it is the one rig-adjacent component that IS portable,
        // as long as the destination rig was authored in the same naming convention.
        // The names (UTF-8 Japanese in practice) are not spelled out here: this file
        // has no BOM and the project sets no /utf-8, so narrow literals are read in the
        // system codepage. The Facial Animation inspector shows them correctly.
        warn(std::type_index(typeid(FacialAnimationComponent)),
             "Holds morph-target NAMES (the PMX blink / vowel names), not bone\n"
             "indices. They are portable between rigs authored in the same naming\n"
             "convention, and mean NOTHING on a rig that lacks them: an unresolved\n"
             "channel is silent (one warning per name per run, then nothing).\n"
             "Check the target's morph list after pasting - the Facial Animation\n"
             "inspector marks missing authored rows in red.\n"
             "Only the authored fields travel; channels, blink phase and the\n"
             "resolved slots are runtime and are re-derived on the target.");

        // ---- Text transport only (owns an allocation, see blobOnly above) ----
        blobOnly(std::type_index(typeid(RigidBodyComponent)),
                 "Owns a Jolt body. Copied as text the body id is reset and the\n"
                 "target gets its own; a raw copy would make both entities drive\n"
                 "ONE body. Note a rigid body without a Collider never gets a\n"
                 "body at all, and a CharacterController on the same entity wins.");
        blobOnly(std::type_index(typeid(ColliderComponent)),
                 "Owns a Jolt shape; the id is reset on the text path.");
        blobOnly(std::type_index(typeid(CharacterControllerComponent)),
                 "Owns a Jolt character body; the id is reset on the text path.\n"
                 "Takes precedence over a RigidBody on the same entity.");
        blobOnly(std::type_index(typeid(TrailComponent)),
                 "Owns a trail slot in the VFX pool.");
        blobOnly(std::type_index(typeid(BeamComponent)),
                 "Owns a beam slot in the VFX pool.");
        blobOnly(std::type_index(typeid(ReflectionProbeComponent)),
                 "Owns a slice of the reflection-probe cubemap array.");

        // ---- Raw copy is the CORRECT path here ----
        m.emplace(std::type_index(typeid(MaterialComponent)),
                  PolicyInfo{ Policy::Warn, Transport::ValueOk,
                  "Copied by value, then marked dirty so the renderer re-syncs its\n"
                  "GPU slot. Note the inline-material serializer stands down on an\n"
                  "entity that has a Material Source Path, so on those the text\n"
                  "path is empty by design and this is the only route." });
        return m;
    }();

    const auto it = kTable.find(ti);
    return it == kTable.end() ? PolicyInfo{} : it->second;
}

} // namespace Editor::ComponentTransfer

// ==== EditorLayer - row model ====
void EditorLayer::ComponentTransferRebuildRows()
{
    auto& S = m_compTransfer;
    S.rows.clear();
    S.rowsDirty = false;
    if (!m_world) return;

    ComponentSerializerRegistry& reg = GetComponentRegistry();

    // Building the diff means SERIALIZING both sides, and serializing is not
    // read-only in this engine: FollowSocket / FollowEntity / Script / CvParent
    // all call ECS::EnsureGuidOn on the way OUT, which stamps a GuidComponent onto
    // whatever entity the ref points at so the edge survives a save. Right for a
    // save; here it is a scene mutation nobody asked for.
    //
    // It is idempotent and harmless in itself, so it is not suppressed (that would
    // break the "identical / differs" column, the whole point of the window) but
    // made VISIBLE: watch the GuidComponent pool for growth and dirty the scene if
    // it moved, so the stamp is saved rather than silently lost on the next load.
    const size_t guidsBefore = m_world->GetPool<GuidComponent>()
                             ? m_world->GetPool<GuidComponent>()->Size() : 0;

    const auto collect = [&](CT::Target& t, std::map<std::string, SideEntry>& out)
    {
        if (t.IsEntity())
        {
            if (!m_world->IsHandleValid(t.entity)) return;
            const Entity e = t.entity.entity;
            for (std::type_index ti : m_world->GetComponentTypeIndices(e))
            {
                SideEntry se;
                se.ti        = ti;
                se.typeKnown = true;
                se.present   = true;
                if (const std::string* tag = CT::TagForType(reg, ti))
                {
                    se.tag  = *tag;
                    // May legitimately come back empty: a serializer's `has`
                    // predicate can decline (MaterialInline stands down when the
                    // entity also carries MaterialSourcePath). Empty blob just
                    // means "the text path is unavailable for this one".
                    se.blob = Editor::Undo::SerializeComponentBlob(reg, *m_world, e, ti);
                }
                out[MergeKey(ti, se.tag)] = std::move(se);
            }
        }
        else if (t.IsPrefab() && t.prefab.valid &&
                 t.prefabNode >= 0 && t.prefabNode < static_cast<int>(t.prefab.nodes.size()))
        {
            const size_t ni = static_cast<size_t>(t.prefabNode);
            for (const CT::PrefabDoc::Comp& c : t.prefab.nodes[ni].comps)
            {
                SideEntry se;
                se.tag     = c.tag;
                se.present = true;
                se.ti      = CT::TypeForTag(reg, c.tag, &se.typeKnown);
                se.blob    = CT::GetNodeComponentBlob(t.prefab, ni, c.tag);
                out[c.tag] = std::move(se);
            }
        }
    };

    std::map<std::string, SideEntry> aMap, bMap;
    collect(S.a, aMap);
    collect(S.b, bMap);

    // Union of both key sets, alphabetical by key (std::map already is).
    std::map<std::string, std::pair<const SideEntry*, const SideEntry*>> merged;
    for (const auto& [k, v] : aMap) merged[k].first  = &v;
    for (const auto& [k, v] : bMap) merged[k].second = &v;

    for (const auto& [key, pair] : merged)
    {
        const SideEntry* a = pair.first;
        const SideEntry* b = pair.second;
        const SideEntry* any = a ? a : b;

        CT::Row r;
        r.inA       = a != nullptr;
        r.inB       = b != nullptr;
        r.type      = any->ti;
        r.tag       = any->tag;
        // Known on EITHER side is enough: an entity always resolves its own
        // type, so a row is only "unknown" when both sides are prefab tags this
        // build has no serializer for.
        r.typeKnown = (a && a->typeKnown) || (b && b->typeKnown);
        r.identical = a && b && !a->blob.empty() && a->blob == b->blob;

        if (!r.typeKnown)
        {
            r.label  = r.tag;
            r.policy = CT::IsPrefabLocalTag(r.tag)
                ? CT::PolicyInfo{ CT::Policy::Blocked, CT::Transport::Auto,
                                  "Prefab-local link record. Its value is a POSITIONAL\n"
                                  "node index inside this one file — meaningless anywhere\n"
                                  "else. LoadPrefab resolves it; the registry never sees it." }
                : CT::PolicyInfo{ CT::Policy::Blocked, CT::Transport::Auto,
                                  "No serializer registered under this tag in this build —\n"
                                  "the component type cannot be resolved, so there is\n"
                                  "nothing to transfer it into." };
        }
        else
        {
            r.label  = ComponentLabelFor(r.type);
            r.policy = CT::PolicyFor(r.type);
            if (r.policy.policy == CT::Policy::Free && r.tag.empty())
                r.policy = { CT::Policy::Warn, CT::Transport::Auto,
                             "No serializer for this type — only the raw deep value copy\n"
                             "can move it, and only entity to entity. Enable\n"
                             "\"allow raw value copy\" in Options." };
        }

        S.rows.push_back(std::move(r));
    }

    const size_t guidsAfter = m_world->GetPool<GuidComponent>()
                            ? m_world->GetPool<GuidComponent>()->Size() : 0;
    if (guidsAfter > guidsBefore)
    {
        MarkSceneDirty();
        LOG_INFO("ComponentTransfer: building the diff stamped %zu GuidComponent(s) "
                 "(serializers ensure a GUID on every entity a ref points at) — "
                 "scene marked dirty so the stamp is not lost",
                 guidsAfter - guidsBefore);
    }
}

// ==== EditorLayer - the transfer itself ====
bool EditorLayer::ComponentTransferApply(bool aToB)
{
    auto& S = m_compTransfer;
    CT::Target& src = aToB ? S.a : S.b;
    CT::Target& dst = aToB ? S.b : S.a;

    S.status.clear();
    S.statusIsError = false;

    const auto fail = [&](std::string m) { S.status = std::move(m); S.statusIsError = true; return false; };

    if (!m_world)                       return fail("no world");
    if (src.kind == CT::TargetKind::None || dst.kind == CT::TargetKind::None)
        return fail("pick both a source and a destination first");
    if (src.IsPrefab() && dst.IsPrefab() && src.prefab.path == dst.prefab.path &&
        src.prefabNode == dst.prefabNode)
        return fail("source and destination are the same prefab node");
    if (src.IsEntity() && dst.IsEntity() && src.entity.entity == dst.entity.entity)
        return fail("source and destination are the same entity");

    ComponentSerializerRegistry& reg = GetComponentRegistry();

    // Pull the source's serializer text for one row. Empty = unavailable.
    const auto sourceBlob = [&](const CT::Row& r) -> std::string
    {
        if (src.IsEntity())
        {
            if (!m_world->IsHandleValid(src.entity) || r.tag.empty()) return {};
            return Editor::Undo::SerializeComponentBlob(reg, *m_world, src.entity.entity, r.type);
        }
        if (src.IsPrefab() && src.prefabNode >= 0 &&
            src.prefabNode < static_cast<int>(src.prefab.nodes.size()))
            return CT::GetNodeComponentBlob(src.prefab, static_cast<size_t>(src.prefabNode), r.tag);
        return {};
    };

    const auto srcHas = [&](const CT::Row& r) { return aToB ? r.inA : r.inB; };
    const auto dstHas = [&](const CT::Row& r) { return aToB ? r.inB : r.inA; };

    int         done = 0, skipped = 0;
    std::string firstError;
    const auto  note = [&](const std::string& m) { if (firstError.empty()) firstError = m; ++skipped; };
    // Components that landed on a destination the editor's own Add Component
    // menu would have hidden them on (AnimationComponent needs a
    // SkeletonComponent, DecalComponent needs a GlobalTransform, ...). Reported,
    // never refused: the copy is what the user asked for, and the requirement
    // may be about to be satisfied by the next component in the same batch.
    std::vector<std::string> unmet;

    // ---- Destination = live entity ----
    if (dst.IsEntity())
    {
        if (!m_world->IsHandleValid(dst.entity))
            return fail("the destination entity no longer exists");
        const Entity de = dst.entity.entity;

        auto macro = std::make_unique<Editor::Undo::EditMacro>(
            aToB ? "Transfer components A -> B" : "Transfer components B -> A");

        for (const CT::Row& r : S.rows)
        {
            if (!r.selected) continue;
            if (r.policy.policy == CT::Policy::Blocked) { note("'" + r.label + "' is blocked"); continue; }
            if (!srcHas(r))                             { note("'" + r.label + "' is not on the source"); continue; }
            if (!S.overwrite && dstHas(r))              { ++skipped; continue; }

            // Snapshot BEFORE mutating. A null holder means "was absent", which
            // is exactly what the undo side needs to know.
            std::shared_ptr<void> before = m_world->CloneComponentByType(de, r.type);

            bool applied = false;
            // ValueOk means the text path is not the authority for this type, so
            // do not even try it from a live entity. (From a PREFAB there is
            // nothing but text, so the blob is still the only option there.)
            const std::string blob =
                (r.policy.transport == CT::Transport::ValueOk && src.IsEntity())
                    ? std::string{} : sourceBlob(r);
            if (!blob.empty())
            {
                // CLEAR THE SLOT FIRST when the destination already had this
                // component. A serializer's `deserialize` is an UPDATE, not a
                // replace, and at least one of them APPENDS: ScriptComponent
                // emits one line per script slot and its deserializer push_backs a
                // slot per line, so a destination that already had two scripts
                // would silently end up with four. SaveGame's ApplyEntityBlock
                // clears Script the same way.
                //
                // Removing first also gives the operation the meaning the window
                // advertises: after the copy, B's component EQUALS A's, rather
                // than being B's merged with A's.
                //
                // Equal in what the text carries; what it cannot carry stays B's
                // (ComponentWrite): B's light keeps its gather order instead of
                // becoming the newest (a directional one would take over as the
                // sun), B's playing voice stays stoppable, B's decal keeps its
                // material. B's Jolt body survives only when A's rigid-body
                // values equal B's; otherwise the re-added RigidBody has no
                // bodyId, and PhysicsSystem reaps the body the removed one owned
                // (it keys ownership on the id, so a remove + re-add inside one
                // frame does not leak it) and builds B a new one from A's values.
                const bool had = m_world->HasComponentByType(de, r.type);
                const ComponentWrite::RuntimeState carry =
                    had ? ComponentWrite::Capture(*m_world, de, r.type) : ComponentWrite::RuntimeState{};
                if (had) m_world->RemoveComponentByType(de, r.type);

                applied = Editor::Undo::DeserializeComponentBlob(reg, *m_world, de, r.type,
                                                                 blob, m_assetMgr);
                if (applied) carry.Restore(*m_world, de);

                // A deserializer that only ever UPDATES an existing component
                // would leave the slot empty now. Put the destination back
                // exactly as it was rather than silently deleting a component
                // the user asked to overwrite.
                if (had && !m_world->HasComponentByType(de, r.type))
                {
                    m_world->RestoreComponentByType(de, r.type, before);
                    applied = false;
                    note("'" + r.label + "' could not be rewritten from text "
                         "(its serializer only updates in place) — left unchanged");
                    continue;
                }
            }

            // The raw path is NOT a general fallback. It is correct only when
            // there is no text transport at all, or when the type is explicitly
            // marked ValueOk: for a component that owns a GPU slot or a physics
            // body, "the serializer declined" and "raw-copying this is safe" are
            // unrelated facts, and guessing gets two entities sharing one
            // allocation.
            const bool mayValueCopy =
                r.policy.transport != CT::Transport::BlobOnly &&
                (r.tag.empty() || r.policy.transport == CT::Transport::ValueOk);

            // The global opt-in guards the UNVETTED raw copies. A ValueOk type
            // is vetted by name, so it does not need the user to arm anything.
            const bool valueArmed =
                S.allowValueCopy || r.policy.transport == CT::Transport::ValueOk;

            if (!applied && mayValueCopy && valueArmed && src.IsEntity() &&
                m_world->IsHandleValid(src.entity))
            {
                // Raw path: same ComponentCloner<T> deep copy Edit<->Play uses.
                // GPU handles are shared by refcount; move-only runtime backing
                // is scrubbed by the cloner, not carried across.
                if (auto holder = m_world->CloneComponentByType(src.entity.entity, r.type))
                    applied = m_world->RestoreComponentByType(de, r.type, holder);
            }

            if (!applied)
            {
                if (!mayValueCopy && !r.tag.empty())
                    note("'" + r.label + "' can only travel as text and its "
                         "serializer produced none on the source");
                else if (r.tag.empty() && !S.allowValueCopy)
                    note("'" + r.label + "' has no serializer — tick \"allow raw "
                         "value copy\" to move it");
                else
                    note("'" + r.label + "' produced no data on the source");
                continue;
            }

            // ---- Post-transfer pokes ----
            // Two components cache state OUTSIDE themselves, so writing their
            // fields is not enough to make the change take.
            if (r.type == std::type_index(typeid(MaterialComponent)))
            {
                // The renderer re-syncs a material's GPU slot only when the
                // component says it is dirty.
                if (auto* mc = m_world->GetComponent<MaterialComponent>(de)) mc->SetDirty();
            }
            else if (r.type == std::type_index(typeid(ChainPhysicsComponent)))
            {
                // ChainPhysicsSystem keeps per-entity solver data keyed by
                // entity, built from the PREVIOUS chain config. Without this the
                // target keeps simulating its old chains and the copied groups
                // look like they did nothing; the chain parameter tree calls
                // Invalidate after every config edit for the same reason.
                if (m_renderer)
                    if (auto* cps = m_renderer->GetChainPhysicsSystem()) cps->Invalidate(de);
            }

            if (const auto ce = m_componentEditors.find(r.type);
                ce != m_componentEditors.end() && ce->second.condition &&
                !ce->second.condition(*m_world, de))
                unmet.push_back(r.label);

            std::shared_ptr<void> after = m_world->CloneComponentByType(de, r.type);
            macro->Add(std::make_unique<Editor::Undo::ValueEditAction>(
                dst.entity, MakeRestore(r.type, std::move(before)),
                MakeRestore(r.type, std::move(after)),
                r.label + " (transfer)"));
            ++done;
        }

        if (done > 0)
        {
            // Play-mode mutations are transient (the Stop snapshot restore wipes
            // them), so recording them would put un-revertable entries on the
            // authoring history: the same gate the Inspector uses.
            if (UndoEnabled() && !macro->Empty())
            {
                SyncedEditContext();
                PushEdit(std::move(macro));
            }
            // A changed component set can change an entity's outliner icon and
            // its logical parent (UIParent / Follow*).
            MarkHierarchyDirty();
            S.rowsDirty = true;
        }
    }
    // ---- Destination = prefab file (staged in memory; [Save] writes it) ----
    else
    {
        if (!dst.prefab.valid ||
            dst.prefabNode < 0 || dst.prefabNode >= static_cast<int>(dst.prefab.nodes.size()))
            return fail("the destination prefab node is not valid");
        const size_t dn = static_cast<size_t>(dst.prefabNode);

        for (const CT::Row& r : S.rows)
        {
            if (!r.selected) continue;
            if (r.policy.policy == CT::Policy::Blocked) { note("'" + r.label + "' is blocked"); continue; }
            if (!srcHas(r))                             { note("'" + r.label + "' is not on the source"); continue; }
            if (!S.overwrite && dstHas(r))              { ++skipped; continue; }
            if (r.tag.empty())
            {
                // A .ipfb stores components as serializer text and nothing else,
                // so "no serializer" is a hard wall here, not a preference.
                note("'" + r.label + "' has no serializer — it cannot be stored in a .ipfb");
                continue;
            }

            const std::string blob = sourceBlob(r);
            if (blob.empty()) { note("'" + r.label + "' produced no data on the source"); continue; }

            if (CT::SetNodeComponentBlob(dst.prefab, dn, r.tag, blob))
            {
                dst.prefabDirty = true;
                ++done;
            }
            else note("'" + r.label + "' could not be written into the prefab");
        }
        if (done > 0) S.rowsDirty = true;
    }

    if (done == 0)
    {
        return fail(firstError.empty()
                    ? "nothing transferred — tick some component rows first"
                    : ("nothing transferred — " + firstError));
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "transferred %d component%s %s%s%s",
                  done, done == 1 ? "" : "s", aToB ? "A -> B" : "B -> A",
                  skipped ? ", skipped " : "", skipped ? std::to_string(skipped).c_str() : "");
    S.status        = buf;
    S.statusIsError = false;
    if (!firstError.empty()) S.status += " (" + firstError + ")";
    if (!unmet.empty())
    {
        S.status += " | unmet requirement on: " + unmet.front();
        if (unmet.size() > 1)
            S.status += " +" + std::to_string(unmet.size() - 1);
    }
    return true;
}

// ==== EditorLayer - one target panel ====
void EditorLayer::DrawComponentTransferTarget(CT::Target& t, const char* id, bool isA)
{
    auto& S = m_compTransfer;
    ImGui::PushID(id);

    ImGui::TextColored(isA ? kColA : kColB, "%s", isA ? "A" : "B");
    ImGui::SameLine();

    int kind = static_cast<int>(t.kind);
    ImGui::SetNextItemWidth(110.f);
    if (ImGui::Combo("##kind", &kind, "(none)\0Entity\0Prefab\0"))
    {
        const auto want = static_cast<CT::TargetKind>(kind);
        if (want != t.kind) { t.Clear(); t.kind = want; S.rowsDirty = true; }
    }

    ImGui::BeginChild("body", ImVec2(0.f, 92.f), true);
    switch (t.kind)
    {
    case CT::TargetKind::EntityTarget:
    {
        if (m_world && m_world->IsHandleValid(t.entity))
        {
            ImGui::TextWrapped("%s", m_world->GetName(t.entity.entity).c_str());
            ImGui::TextDisabled("id=%u  |  %zu components", t.entity.entity,
                                m_world->GetComponentTypeIndices(t.entity.entity).size());
        }
        else if (t.entity.entity != NullEntity)
        {
            ImGui::TextColored(kColBlock, "entity no longer exists");
        }
        else
        {
            ImGui::TextDisabled("Drag an entity here from the Hierarchy,\nor press [Use Selected].");
        }

        const bool canUse = m_world && m_selectedEntity != NullEntity &&
                            m_world->IsAlive(m_selectedEntity);
        ImGui::BeginDisabled(!canUse);
        if (ImGui::Button("Use Selected"))
        {
            t.entity    = m_world->MakeHandle(m_selectedEntity);
            S.rowsDirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { t.entity = NullEntityHandle; S.rowsDirty = true; }
        break;
    }
    case CT::TargetKind::PrefabTarget:
    {
        if (t.prefab.valid)
        {
            ImGui::TextWrapped("%s", t.prefab.path.c_str());
            if (t.prefabDirty) { ImGui::SameLine(); ImGui::TextColored(kColWarn, "*"); }

            // A prefab is a TREE. Which node receives the component matters
            // (on a character the root holds the skeleton and the children hold
            // the meshes), so make the choice explicit rather than assuming 0.
            std::string preview = "(none)";
            if (t.prefabNode >= 0 && t.prefabNode < static_cast<int>(t.prefab.nodes.size()))
            {
                const auto& n = t.prefab.nodes[static_cast<size_t>(t.prefabNode)];
                preview = std::to_string(n.idx) + ": " + (n.name.empty() ? "(unnamed)" : n.name);
            }
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##node", preview.c_str()))
            {
                for (size_t i = 0; i < t.prefab.nodes.size(); ++i)
                {
                    const auto& n = t.prefab.nodes[i];
                    char lbl[192];
                    std::snprintf(lbl, sizeof(lbl), "%d: %s  (%zu comps%s)",
                                  n.idx, n.name.empty() ? "(unnamed)" : n.name.c_str(),
                                  n.comps.size(), n.parent < 0 ? ", root" : "");
                    if (ImGui::Selectable(lbl, static_cast<int>(i) == t.prefabNode))
                    {
                        t.prefabNode = static_cast<int>(i);
                        S.rowsDirty  = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("Drag a .ipfb here from the Resource panel,\nor press [Browse...].");
        }

        if (ImGui::Button("Browse..."))
        {
            const std::string p = PickPrefabFile();
            if (!p.empty())
            {
                std::string err;
                if (CT::LoadPrefabDoc(p, t.prefab, &err))
                {
                    t.prefabNode = 0; t.prefabDirty = false; S.rowsDirty = true;
                    S.status.clear(); S.statusIsError = false;
                }
                else { S.status = "prefab load failed: " + err; S.statusIsError = true; }
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!t.prefab.valid || !t.prefabDirty);
        if (ImGui::Button("Save Prefab")) ImGui::OpenPopup("##savepfb");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!t.prefab.valid);
        if (ImGui::Button("Reload"))
        {
            std::string err, p = t.prefab.path;
            if (CT::LoadPrefabDoc(p, t.prefab, &err)) { t.prefabNode = 0; t.prefabDirty = false; }
            else { S.status = "prefab reload failed: " + err; S.statusIsError = true; }
            S.rowsDirty = true;
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopupModal("##savepfb", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextColored(kColWarn, "Writing a prefab is NOT undoable.");
            ImGui::TextUnformatted("The current file is copied to <name>.ipfb.bak first.");
            ImGui::TextDisabled("%s", t.prefab.path.c_str());
            ImGui::Separator();
            ImGui::TextDisabled("Already-spawned instances do not update until the\n"
                                "scene is reloaded.");
            ImGui::Separator();
            if (ImGui::Button("Write file", ImVec2(120.f, 0.f)))
            {
                std::string err;
                if (CT::SavePrefabDoc(t.prefab, &err))
                {
                    t.prefabDirty   = false;
                    S.status        = "saved " + t.prefab.path;
                    S.statusIsError = false;
                }
                else { S.status = "prefab save failed: " + err; S.statusIsError = true; }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120.f, 0.f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        break;
    }
    default:
        ImGui::TextDisabled("Choose Entity or Prefab, then drop a target here.");
        break;
    }
    ImGui::EndChild();

    // Drop target covers the whole body child. Payload ids match the Hierarchy's
    // drag source (EditorLayer_Outliner.cpp) and the asset browser's
    // (EditorLayer_AssetBrowser.cpp); dropping does not change the kind
    // selector for you, it sets the kind that matches what you dropped.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ENTITY"))
        {
            if (m_world && p->DataSize == static_cast<int>(sizeof(Entity)))
            {
                Entity e = NullEntity;
                std::memcpy(&e, p->Data, sizeof(Entity));
                if (m_world->IsAlive(e))
                {
                    t.kind      = CT::TargetKind::EntityTarget;
                    t.entity    = m_world->MakeHandle(e);
                    S.rowsDirty = true;
                }
            }
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
        {
            const std::string path = static_cast<const char*>(p->Data);
            std::string err;
            if (CT::LoadPrefabDoc(path, t.prefab, &err))
            {
                t.kind       = CT::TargetKind::PrefabTarget;
                t.prefabNode = 0;
                t.prefabDirty = false;
                S.rowsDirty  = true;
                S.status.clear(); S.statusIsError = false;
            }
            else { S.status = "prefab load failed: " + err; S.statusIsError = true; }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopID();
}

// ==== EditorLayer - the window ====
void EditorLayer::RenderComponentTransferWindow()
{
    auto& S = m_compTransfer;

    ImGui::SetNextWindowSize(ImVec2(860.f, 560.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Component Transfer", &m_showComponentTransfer))
    {
        ImGui::End();
        return;
    }

    if (!m_world)
    {
        ImGui::TextDisabled("No world.");
        ImGui::End();
        return;
    }

    // A target entity destroyed while the window is open must not silently
    // retarget a recycled id: the handle catches it, this just drops the row
    // cache so the table stops showing a dead entity's components.
    if ((S.a.IsEntity() && S.a.entity.entity != NullEntity && !m_world->IsHandleValid(S.a.entity)) ||
        (S.b.IsEntity() && S.b.entity.entity != NullEntity && !m_world->IsHandleValid(S.b.entity)))
        S.rowsDirty = true;

    if (!UndoEnabled())
        ImGui::TextColored(kColWarn,
            "Play mode: transfers are NOT recorded in undo and are discarded on Stop.");

    // ---- Targets ----
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(half);
    ImGui::BeginChild("##ta", ImVec2(half, 150.f), false);
    DrawComponentTransferTarget(S.a, "A", true);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::PushItemWidth(half);
    ImGui::BeginChild("##tb", ImVec2(half, 150.f), false);
    DrawComponentTransferTarget(S.b, "B", false);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    if (ImGui::Button("Swap A <-> B"))
    {
        std::swap(S.a, S.b);
        S.rowsDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) S.rowsDirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(the entity side is re-read every refresh; the prefab side is a snapshot)");

    ImGui::Separator();

    // ---- Options ----
    ImGui::SetNextItemWidth(180.f);
    ImGui::InputTextWithHint("##filter", "filter components...", S.filter, sizeof(S.filter));
    ImGui::SameLine();
    ImGui::Checkbox("Overwrite existing", &S.overwrite);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: only components the destination is MISSING are written.");
    ImGui::SameLine();
    ImGui::Checkbox("Show blocked", &S.showBlocked);
    ImGui::SameLine();
    ImGui::Checkbox("Allow raw value copy", &S.allowValueCopy);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Entity -> entity only. Reaches components with no serializer\n"
                          "(SkeletonComponent, MeshLibRef, MorphComponent, ...) using the\n"
                          "same deep copy Edit<->Play uses. GPU handles are shared by\n"
                          "refcount; per-instance runtime state is scrubbed, not copied.");

    if (S.rowsDirty) ComponentTransferRebuildRows();

    // ---- Selection helpers ----
    const auto visible = [&](const CT::Row& r)
    {
        if (!S.showBlocked && r.policy.policy == CT::Policy::Blocked) return false;
        if (S.filter[0] == '\0') return true;
        std::string hay = r.label + " " + r.tag;
        std::string ndl = S.filter;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        std::transform(ndl.begin(), ndl.end(), ndl.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return hay.find(ndl) != std::string::npos;
    };

    if (ImGui::SmallButton("Select all transferable"))
        for (CT::Row& r : S.rows)
            if (visible(r) && r.policy.policy != CT::Policy::Blocked) r.selected = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Select differences"))
        for (CT::Row& r : S.rows)
            if (visible(r) && r.policy.policy != CT::Policy::Blocked && !r.identical) r.selected = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear selection"))
        for (CT::Row& r : S.rows) r.selected = false;

    int selCount = 0;
    for (const CT::Row& r : S.rows) if (r.selected) ++selCount;
    ImGui::SameLine();
    ImGui::TextDisabled("%d selected", selCount);

    // ---- Table ----
    const float tableH = ImGui::GetContentRegionAvail().y - 60.f;
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##ctrows", 6, kFlags, ImVec2(0.f, tableH > 80.f ? tableH : 80.f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed, 24.f);
        ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthFixed, 34.f);
        ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthFixed, 34.f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Copy", ImGuiTableColumnFlags_WidthFixed, 78.f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < S.rows.size(); ++i)
        {
            CT::Row& r = S.rows[i];
            if (!visible(r)) continue;

            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            const bool blocked = r.policy.policy == CT::Policy::Blocked;

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(blocked);
            ImGui::Checkbox("##s", &r.selected);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            if (blocked)                                ImGui::TextColored(kColBlock, "%s", r.label.c_str());
            else if (r.policy.policy == CT::Policy::Warn) ImGui::TextColored(kColWarn, "%s", r.label.c_str());
            else                                         ImGui::TextUnformatted(r.label.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("tag: %s", r.tag.empty() ? "(no serializer)" : r.tag.c_str());
                if (r.policy.reason)
                {
                    ImGui::Separator();
                    ImGui::TextColored(blocked ? kColBlock : kColWarn, "%s",
                                       blocked ? "BLOCKED" : "CAUTION");
                    ImGui::TextUnformatted(r.policy.reason);
                }
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(2);
            if (r.inA) ImGui::TextColored(kColA, "*"); else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3);
            if (r.inB) ImGui::TextColored(kColB, "*"); else ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(4);
            if (r.inA && r.inB)
            {
                if (r.identical) ImGui::TextColored(kColOk,    "identical");
                else             ImGui::TextColored(kColWarn,  "differs");
            }
            else if (r.inA) ImGui::TextColored(kColMuted, "only A");
            else            ImGui::TextColored(kColMuted, "only B");

            // Per-row one-shot copy: tick just this row, run, restore the
            // previous ticks. Cheaper for the user than "clear, tick one, press
            // the big button, re-tick".
            ImGui::TableSetColumnIndex(5);
            ImGui::BeginDisabled(blocked || !r.inA);
            if (ImGui::SmallButton(">"))
            {
                std::vector<char> saved;
                saved.reserve(S.rows.size());
                for (CT::Row& q : S.rows) { saved.push_back(q.selected ? 1 : 0); q.selected = false; }
                S.rows[i].selected = true;
                ComponentTransferApply(true);
                if (saved.size() == S.rows.size())
                    for (size_t k = 0; k < S.rows.size(); ++k) S.rows[k].selected = saved[k] != 0;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("copy A -> B");
            ImGui::SameLine();
            ImGui::BeginDisabled(blocked || !r.inB);
            if (ImGui::SmallButton("<"))
            {
                std::vector<char> saved;
                saved.reserve(S.rows.size());
                for (CT::Row& q : S.rows) { saved.push_back(q.selected ? 1 : 0); q.selected = false; }
                S.rows[i].selected = true;
                ComponentTransferApply(false);
                if (saved.size() == S.rows.size())
                    for (size_t k = 0; k < S.rows.size(); ++k) S.rows[k].selected = saved[k] != 0;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("copy B -> A");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- Bulk actions + status ----
    const bool bothSet = S.a.kind != CT::TargetKind::None && S.b.kind != CT::TargetKind::None;
    ImGui::BeginDisabled(!bothSet || selCount == 0);
    if (ImGui::Button("Copy selected  A -> B", ImVec2(200.f, 0.f))) ComponentTransferApply(true);
    ImGui::SameLine();
    if (ImGui::Button("Copy selected  B -> A", ImVec2(200.f, 0.f))) ComponentTransferApply(false);
    ImGui::EndDisabled();

    if (!S.status.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(S.statusIsError ? kColBlock : kColOk, "%s", S.status.c_str());
    }

    ImGui::End();
}
