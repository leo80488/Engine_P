// EditorLayer_EditStack.cpp - the editor's edit stack: the undo / redo ring
// (UndoEnabled, PushEdit, EditUndo / EditRedo, the shortcuts, the selection an
// undo restores, recorded spawns, the Lua entity-write observer), the
// transform edit tracking the gizmo commits through, and duplicate / copy /
// paste with the subtree clone and reference remap they use.

#include "Editor/EditorLayer.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "ECS/IKRigComponent.h"
#include "Editor/EditorLayerShared.h"
#include "Editor/MaterialPreviewWidget.h"
#include "Editor/CameraPreviewWindow.h"
#include "Editor/AssetPreviewWindow.h"
#include "Editor/MeshEditorWindow.h"       // the Mesh Editor
#include "Editor/SkeletonEditorWindow.h"   // the Skeleton Editor
#include "Editor/TextureViewerWindow.h"    // the Texture Viewer
#include "Editor/UIEditorWindow.h"         // a designer drag holds Ctrl+Z off
#include "Editor/MaterialPropertyPanel.h" // the material controls
#include "Editor/MaterialEditorWindow.h"  // the Material Editor
#include "Editor/NodeEditorSmoke.h"       // the node editor's acceptance smoke run
#include "Editor/SplitViewportPanel.h"
#include "Editor/ChainPhysicsPreviewWindow.h" // chain physics preview
#include "System/CommandLine.h"
#include "Editor/EntityRefPicker.h"
#include "ECS/PlayModeController.h"   // authoritative Edit/Play state machine
#include "Reflection/ComponentReflection.h"
#include "Sequencer/SequenceEval.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "AI/AIComponents.h"
#include "AI/AISystem.h"
#include "AI/BTNode.h"
#include "ECS/Components.h"           // Transform, MaterialComponent
#include "ECS/HierarchyOps.h"
#include "ECS/PerObjectShadowComponent.h" // per-object (high-res) shadow opt-in tag
#include "ECS/FollowComponents.h"
#include "ECS/FollowSystem.h"
#include "ECS/LifetimeComponent.h"       // transient-attachment filter in the duplicate path
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"     // UI::UIChildrenOf (a UI subtree duplicates whole)
#include "UI/UIDocument.h"         // UI::DocumentHostOf: instance nodes are read-only
#include "UI/WorldSpaceUI.h"
#include "ECS/TerrainComponent.h"
#include "ECS/BillboardFXComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/AnimFXGateComponent.h"
#include "ECS/BeamComponent.h"
#include "Graphics/TracerSystem.h"
#include "ECS/ReflectionProbeComponent.h"
#include "Scene/MeshSpawner.h"
#include "Scene/SceneDefaults.h"
#include "Resource/SceneSerializer.h"  // SaveScene / LoadScene
#include "Resource/EngineSettings.h"
#include "Editor/InputBindingsWindow.h" // Window > Input Bindings (rebind + pad diagnostics)
#include "ECS/HierarchyComponents.h"  // LocalTransform, GlobalTransform, Parent, Children, etc.
#include "ECS/AnimEntityResolve.h"    // a model root's skeleton carrier
#include "ECS/StaticMeshComponents.h" // StaticMesh owners and their derived sections
#include "Resource/MeshLibIdentity.h" // MeshKeyToHex - StaticMesh slot keys
#include "Tools/CollisionMeshBaker.h" // Bake render meshes into combined .meshlib
#include "Resource/MeshLibrary.h"     // the MeshLibrary a re-instantiated duplicate loads into
#include "Resource/AssetHeader.h"
#include "System/TaskSystem.h"
#include "Editor/Undo/ComponentEditAction.h" // Add/RemoveComponentAction (pulls ComponentBlob.h)
#include "Editor/Undo/TransformEditAction.h" // Entity.set's Transform writes
#include "Scripting/LuaEntityBindings.h"     // SetLuaEntityWriteObserver
#include "Editor/Undo/ValueEditAction.h"
#include "Editor/Undo/StructuralAction.h"    // StructuralAction/ReparentAction + EntitySubtreeSnapshot
#include "Editor/Undo/RenameAction.h"
#include "Physics/PhysicsSystem.h"
#include "Nav/NavComponents.h"
#include "ECS/AIIntentComponent.h"
#include "Scene/SceneInstanceLoader.h"
#include "Editor/HumanoidMapping.h"             // the mapping rows, the .iskel save
#include "Renderer/Renderer.h"
#include <chrono>
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "ECS/IKSystem.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "System/Log.h"
#include "Resource/MaterialOverrideApply.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"      // DuplicateSceneInstance rewrites what the fresh instance has
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "ECS/FacialAnimationSystem.h"
#include "ECS/NotifyTypes.h"
#include "ECS/FollowComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Editor/DebugDrawSystem.h"
#include "Scripting/ScriptSystem.h"     // exposed-var schema + live push
#include "Graphics/GraphicsDX12.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include "Editor/ChainRestPoseMath.h"      // ChainPreview UI gizmo: shared rest-pose delta math
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <optional>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

using namespace DirectX;

namespace
{
    // Pre-order entity list of a subtree (parents first, Children order kept).
    // Iterative: character skeletons reach depth ~70, and this can run from deep
    // inside the ImGui call stack.
    void CollectSubtreeDFS(World& world, Entity root, std::vector<Entity>& out)
    {
        if (!world.IsAlive(root)) return;
        std::vector<Entity> stack{ root };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!world.IsAlive(e)) continue;
            out.push_back(e);
            if (const Children* ch = world.GetComponent<Children>(e))
                for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it)
                    stack.push_back(*it);   // reversed -> children pop in author order
        }
    }

    // True when any entity in the subtree carries GPU skinning state. Such a
    // subtree cannot be duplicated by copying components (see DuplicateSceneInstance).
    bool SubtreeHasSkinnedMesh(World& world, Entity root)
    {
        std::vector<Entity> nodes;
        CollectSubtreeDFS(world, root, nodes);
        for (Entity e : nodes)
            if (world.HasComponent<MeshSkinnedComponent>(e)) return true;
        return false;
    }

    // Hook the clone walk calls for a skinned .iscn instance root (see
    // EditorLayer::DuplicateSceneInstance). Returns the fresh root, or NullEntity
    // to let the generic clone handle it after all.
    using SceneInstanceCloneFn = std::function<Entity(Entity srcRoot, Entity newParent)>;

    // Duplicates an entity and its descendants via the ComponentSerializerRegistry round-trip.
    // The tree walk rebuilds the hierarchy, so serialized child ids (which point at src) are ignored.
    // @p srcToDst (optional) accumulates every source -> copy pair the walk produces,
    // which RemapClonedRefs then uses to re-point in-subtree entity references.
    Entity CloneEntityRecursive(World* world,
                                Resource::AssetManager* assetMgr,
                                Entity src,
                                Entity newParent,
                                const SceneInstanceCloneFn& cloneSceneInstance = {},
                                std::unordered_map<Entity, Entity>* srcToDst = nullptr,
                                bool uiChild = false)   // a UI child copy: no "(Copy)", no transform parent
    {
        // Skinned characters can't be value-copied (their skinning descriptor
        // slots / skeleton registration are per-instance GPU state), so hand the
        // whole subtree to the re-instantiating path and stop recursing here.
        // Gated on SceneSourcePath first: the subtree scan would otherwise run at
        // every node of a big static scene tree (O(n^2) on a 20k-node instance).
        if (cloneSceneInstance && world->HasComponent<SceneSourcePath>(src)
            && SubtreeHasSkinnedMesh(*world, src))
        {
            if (const Entity reinstanced = cloneSceneInstance(src, newParent);
                reinstanced != NullEntity)
                return reinstanced;
            // Reload failed (missing/renamed .iscn) - fall through to the generic
            // clone so the user still gets a static rest-pose copy.
        }

        Entity dst = world->CreateEntity();
        if (srcToDst) (*srcToDst)[src] = dst;

        // Top-level clone gets a "(Copy)" suffix; descendants keep their names.
        std::string newName = world->GetName(src);
        if (newParent == NullEntity && !uiChild) newName += " (Copy)";
        world->SetName(dst, newName);

        // Manually copy transform components (not in registry).
        if (const LocalTransform* lt = world->GetComponent<LocalTransform>(src))
            world->AddComponent<LocalTransform>(dst, *lt);
        if (world->HasComponent<GlobalTransform>(src))
            world->AddComponent<GlobalTransform>(dst, GlobalTransform{}); // recomputed by TransformSystem

        // Round-trip every registered component. Uses the shared line-aware
        // helper so multi-line components (e.g. ScriptComponent emits one line
        // per script slot) duplicate ALL of their slots, not just the first.
        auto& reg = GetComponentRegistry();
        for (const auto& [type, ser] : reg.All())
        {
            if (!ser.has || !ser.has(*world, src)) continue;
            std::ostringstream ss;
            ser.serialize(*world, src, ss);
            Editor::Undo::DeserializeComponentBlob(reg, *world, dst, type, ss.str(), assetMgr);
        }

        // ---- Live material beats the round-trip ----
        // A mesh entity with a MaterialRef persists as (path + MaterialOverride
        // diff bag), and the scene/prefab loader, not the serializer, applies the
        // override. Replaying both here would reload the pristine .imat and drop
        // every per-entity tweak. The source's runtime MaterialComponent has all of
        // it baked in (pure CPU data; GPU bindings are renderer-side, keyed by
        // entity), so stamp it straight onto the clone.
        if (const MaterialComponent* srcMat = world->GetComponent<MaterialComponent>(src))
        {
            MaterialComponent mat = *srcMat;    // value-copy first: AddComponent may grow the pool
            mat.SetDirty();
            world->AddComponent<MaterialComponent>(dst, std::move(mat));
        }

        // ---- Runtime-only render components (no serializer) ----
        // An imported (non-procedural) mesh entity carries its geometry in
        // MeshLibRef, which SceneInstanceLoader attaches at load time and no
        // serializer covers (MeshRef/MeshSourcePath is only a path string).
        // Without this block the clone has every authored component but no
        // geometry and is never drawn (BuildScene_GatherMeshLibRefs iterates the
        // MeshLibRef pool).
        // Sharing the library handle is safe: MeshLibrary ownership is tracked per
        // world (Renderer::m_worldMeshLibs / TrackWorldMeshLibrary) and released
        // only at OnWorldClear; an entity never owns a reference, so no
        // Retain/Release balancing is needed.
        if (const MeshLibRef* srcRef = world->GetComponent<MeshLibRef>(src))
        {
            MeshLibRef ref;                     // value-copy first: AddComponent may grow the pool
            ref.libHandle = srcRef->libHandle;
            ref.meshId    = srcRef->meshId;
            // cachedDescSlot/cachedGeneration stay default-invalid so the
            // renderer resolves a fresh mesh-descriptor slot for this entity.
            world->AddComponent<MeshLibRef>(dst, ref);
        }
        // Culling bounds + layer mask the scene loader attaches alongside it.
        // LocalAabb feeds the per-frame WorldAabb recompute; without WorldAabb
        // the clone falls back to its pivot for depth sort and never enters the
        // cull BVH properly.
        if (const LocalAabb* la = world->GetComponent<LocalAabb>(src))
        {
            const LocalAabb v = *la;
            world->AddComponent<LocalAabb>(dst, v);
        }
        if (const WorldAabb* wa = world->GetComponent<WorldAabb>(src))
        {
            const WorldAabb v = *wa;
            world->AddComponent<WorldAabb>(dst, v);
        }
        if (const RenderLayer* rl = world->GetComponent<RenderLayer>(src))
        {
            const RenderLayer v = *rl;
            world->AddComponent<RenderLayer>(dst, v);
        }
        if (world->HasComponent<SceneNodeTag>(src))
            world->AddComponent<SceneNodeTag>(dst, SceneNodeTag{});

        // ---- Fresh identity ----
        // GuidComponent round-trips verbatim and its deserializer re-registers the
        // parsed Guid, so without this the clone would steal the source's Guid
        // binding and every AttachmentRef / UIParent-by-guid pointing at the
        // original would silently re-resolve to the copy. Stamp the clone with a
        // new Guid and hand the original its own Guid back.
        if (GuidComponent* gc = world->GetComponent<GuidComponent>(dst))
        {
            const ECS::Guid srcGuid = gc->guid;
            gc->guid = ECS::Guid::Generate();
            ECS::GuidRegistry::Get().Register(gc->guid, dst);
            if (srcGuid.IsValid() && world->IsAlive(src))
                ECS::GuidRegistry::Get().Register(srcGuid, src);
        }

        // Hook into the new parent's Children list (or stay a root if none).
        if (newParent != NullEntity)
        {
            Parent p; p.entity = newParent;
            world->AddComponent<Parent>(dst, p);
            if (Children* pc = world->GetComponent<Children>(newParent))
                pc->entities.push_back(dst);
            else
            {
                Children newCh;
                newCh.entities.push_back(dst);
                world->AddComponent<Children>(newParent, std::move(newCh));
            }
        }

        // Snapshot the child list before recursing - the world mutates during the loop.
        //
        // A transient child is runtime output, not part of the object: a
        // StaticMesh owner's derived sections (the clone's own StaticMesh line
        // derives fresh ones - copies would draw the mesh twice), a Sequencer
        // spawnable, a VFX spawn. Cloned, it would lose SceneTransientTag (the
        // tag has no serializer) and become a permanent, saved part.
        if (const Children* ch = world->GetComponent<Children>(src))
        {
            const std::vector<Entity> kids = ch->entities;
            for (Entity child : kids)
            {
                if (world->HasComponent<SceneTransientTag>(child)) continue;
                CloneEntityRecursive(world, assetMgr, child, dst, cloneSceneInstance, srcToDst);
            }
        }

        // Canvas-UI children hang off UIParent, not Children: without this a duplicated
        // Button would lose its Label and a duplicated Canvas would come out empty.
        // Cloned in paint order, each copy's edge points at this copy, which has a fresh
        // GUID, so the copied edges never re-resolve to the original.
        //
        // A transient UI child of a transient node is part of it, not runtime output
        // hanging off it: every node of a UI Editor asset tab's stage is transient, and a
        // duplicated stage Button keeps its Label. (The copies come out untagged - the
        // tag has no serializer - and the tab tags its stage again.)
        if (UI::IsUINode(*world, src))
        {
            const bool srcTransient = world->HasComponent<SceneTransientTag>(src);
            std::vector<Entity> uiKids;
            UI::UIChildrenOf(*world, src, uiKids);
            for (Entity k : uiKids)
            {
                if (world->HasComponent<SceneTransientTag>(k) && !srcTransient) continue;
                if (srcToDst && srcToDst->count(k)) continue;   // reached already
                const Entity kc = CloneEntityRecursive(world, assetMgr, k, NullEntity, cloneSceneInstance,
                                                       srcToDst, /*uiChild*/ true);
                if (kc != NullEntity) UI::AttachUIParent(*world, kc, dst);
            }
        }
        return dst;
    }

    // ---- Attachment followers travel with the copy ----
    // FollowSocket / FollowEntity are flat bindings: Command::EquipToSocket only
    // adds a FollowSocketComponent, so an equipped weapon (or an attached VFX /
    // HP bar) is not a child of the character and a subtree duplicate leaves it
    // behind. Clone the followers of everything copied so far, and repeat (up to
    // 8 rounds) so attachments-of-attachments come too. RemapClonedRefs then
    // re-points each follower copy at the copied target.
    //
    // Only these two components count as "attached". FollowCamera / AimCamera /
    // CameraController.followTarget / HeadTrack / MotionDirector merely watch a
    // target - duplicating a character must not clone the camera watching it.
    uint32_t CloneAttachedFollowers(World* world,
                                    Resource::AssetManager* assetMgr,
                                    const SceneInstanceCloneFn& cloneSceneInstance,
                                    std::unordered_map<Entity, Entity>& srcToDst)
    {
        uint32_t cloned = 0;
        for (int round = 0; round < 8; ++round)
        {
            // Copies produced so far. A follower's copy still points at the
            // original target until RemapClonedRefs runs, so without this it
            // would look like another follower to clone - every round.
            std::unordered_set<Entity> copies;
            copies.reserve(srcToDst.size());
            for (const auto& [s, d] : srcToDst) copies.insert(d);

            std::vector<Entity> followers;
            auto scan = [&](auto* pool)
            {
                if (!pool) return;
                const std::vector<Entity>& ents = pool->Entities();
                auto&                      data = pool->Data();
                for (size_t i = 0; i < ents.size(); ++i)
                {
                    const Entity f = ents[i];
                    if (!world->IsAlive(f)) continue;
                    if (srcToDst.count(f) || copies.count(f)) continue;   // already copied / is a copy
                    // Runtime-spawned attachments (notify-driven VFX, transient
                    // rigs) are not authored state: they respawn on their own,
                    // and LifetimeComponent has no serializer - a clone of one
                    // would lose its timer and never despawn.
                    if (world->HasComponent<SceneTransientTag>(f)) continue;
                    if (world->HasComponent<LifetimeComponent>(f))   continue;
                    const Entity target = data[i].target.Resolve(*world);
                    if (target == NullEntity || !srcToDst.count(target)) continue;
                    followers.push_back(f);
                }
            };
            scan(world->GetPool<FollowSocketComponent>());
            scan(world->GetPool<FollowEntityComponent>());
            if (followers.empty()) break;

            // Clone after the scan - CloneEntityRecursive mutates every pool.
            for (Entity f : followers)
            {
                if (srcToDst.count(f)) continue;   // pulled in by an earlier follower's subtree
                Entity fParent = NullEntity;
                if (const Parent* p = world->GetComponent<Parent>(f)) fParent = p->entity;
                if (CloneEntityRecursive(world, assetMgr, f, fParent,
                                         cloneSceneInstance, &srcToDst) != NullEntity)
                    ++cloned;
            }
        }
        return cloned;
    }

    // ---- Intra-subtree entity-reference remap ----
    // A duplicate must reference its own parts: the copied weapon follows the
    // copied character, the copied probe uses the copied proxy volume. Refs
    // pointing outside the duplicated subtree (a shared camera, a level-wide
    // proxy volume) are deliberately left on the original target.
    //
    // Every ref below is an AttachmentRef (guid + resolve cache). The copy
    // carries the source's guid - clones get a fresh Guid of their own, so the
    // source's guid still resolves to the source entity - and @p srcToDst says
    // which clone replaces it.
    bool RemapAttachment(World& world, AttachmentRef& ref,
                         const std::unordered_map<Entity, Entity>& srcToDst)
    {
        if (!ref.HasGuid()) return false;
        const Entity target = ref.Resolve(world);
        if (target == NullEntity) return false;
        const auto it = srcToDst.find(target);
        if (it == srcToDst.end() || !world.IsAlive(it->second)) return false;
        // BindAndStamp, not Bind: a re-instantiated scene node has no Guid yet,
        // and Bind on a Guid-less target would CLEAR the ref instead of moving it.
        ref.BindAndStamp(world, it->second);
        return true;
    }

    // @p getRef returns the component's AttachmentRef&. Holding `c` across
    // RemapAttachment is safe: BindAndStamp only ever adds a GuidComponent, and
    // pools are heap-stable - it cannot move T's storage (T is never
    // GuidComponent here).
    template <typename T, typename GetRef>
    uint32_t RemapComponentRef(World& world, Entity e,
                               const std::unordered_map<Entity, Entity>& srcToDst,
                               GetRef getRef)
    {
        T* c = world.GetComponent<T>(e);
        if (!c) return 0u;
        return RemapAttachment(world, getRef(*c), srcToDst) ? 1u : 0u;
    }

    void RemapClonedRefs(World& world, const std::unordered_map<Entity, Entity>& srcToDst)
    {
        if (srcToDst.empty()) return;
        uint32_t remapped = 0;

        for (const auto& [srcE, dstE] : srcToDst)
        {
            if (!world.IsAlive(dstE)) continue;

            remapped += RemapComponentRef<FollowSocketComponent>(world, dstE, srcToDst,
                [](FollowSocketComponent& c)     -> AttachmentRef& { return c.target; });
            remapped += RemapComponentRef<FollowEntityComponent>(world, dstE, srcToDst,
                [](FollowEntityComponent& c)     -> AttachmentRef& { return c.target; });
            remapped += RemapComponentRef<FollowCameraComponent>(world, dstE, srcToDst,
                [](FollowCameraComponent& c)     -> AttachmentRef& { return c.target; });
            remapped += RemapComponentRef<AimCameraComponent>(world, dstE, srcToDst,
                [](AimCameraComponent& c)        -> AttachmentRef& { return c.target; });
            remapped += RemapComponentRef<CameraControllerComponent>(world, dstE, srcToDst,
                [](CameraControllerComponent& c) -> AttachmentRef& { return c.followTarget; });
            remapped += RemapComponentRef<HeadTrackComponent>(world, dstE, srcToDst,
                [](HeadTrackComponent& c)        -> AttachmentRef& { return c.targetRef; });
            remapped += RemapComponentRef<PlayerComponent>(world, dstE, srcToDst,
                [](PlayerComponent& c)           -> AttachmentRef& { return c.cameraEntity; });
            remapped += RemapComponentRef<ReflectionProbeComponent>(world, dstE, srcToDst,
                [](ReflectionProbeComponent& c)  -> AttachmentRef& { return c.proxyVolume; });

            // MotionDirector holds one ref per track.
            if (MotionDirectorComponent* md = world.GetComponent<MotionDirectorComponent>(dstE))
                for (MotionDirectorTrack& track : md->tracks)
                    remapped += RemapAttachment(world, track.target, srcToDst) ? 1u : 0u;

            // Canvas-UI hierarchy edge - a raw (entity + guid) pair, not an
            // AttachmentRef, so it needs its own rebind.
            if (UI::UIParent* up = world.GetComponent<UI::UIParent>(dstE))
            {
                Entity parent = up->parent;
                if ((parent == NullEntity || !world.IsAlive(parent)) && up->parentGuid.IsValid())
                    parent = ECS::GuidRegistry::Get().Find(up->parentGuid);
                const auto it = srcToDst.find(parent);
                if (parent != NullEntity && it != srcToDst.end() && world.IsAlive(it->second))
                {
                    const ECS::Guid guid = ECS::EnsureGuidOn(world, it->second);
                    up = world.GetComponent<UI::UIParent>(dstE);   // re-fetch after the stamp
                    if (up)
                    {
                        up->parent     = it->second;
                        up->parentGuid = guid;
                        ++remapped;
                    }
                }
            }
        }

        if (remapped)
            LOG_INFO("EditorLayer: duplicate — re-pointed %u in-subtree entity reference(s) "
                     "at the copy", remapped);
    }
}

// ==== Undo / Redo ====

bool EditorLayer::UndoEnabled() const
{
    // Only record/apply edits in Edit mode; Play-mode mutations are transient.
    return !m_playModeController || GetPlayState() == ViewportPlayState::Stopped;
}

// Rising ImGuizmo::IsUsing() edge: latch the idle baselines (every selection root) as
// "before"; falling edge: CommitTransformEdit pushes one entry (before -> current).
void EditorLayer::UpdateTransformEditTracking()
{
    if (!m_world) { m_xformRecording = false; m_xformBaselineValid = false; return; }

    const bool manipulating = ImGuizmo::IsUsing();

    if (manipulating)
    {
        if (!m_xformRecording
            && m_xformBaselineValid
            && m_xformBaselineEntity == m_selectedEntity
            && !m_xformBaselines.empty())
        {
            m_xformRecording    = true;
            m_xformRecordEntity = m_selectedEntity;
            m_xformBefore       = m_xformBaseline;   // last idle frame = pre-drag
            m_xformBefores      = m_xformBaselines;  // ...for every moved entity
        }
        return;   // hold the baseline frozen for the duration of the drag
    }

    // Idle this frame: commit a just-finished drag, then refresh the baseline.
    if (m_xformRecording)
    {
        CommitTransformEdit();
        m_xformRecording = false;
    }

    // Baseline everything a drag would move - the selection roots, matching
    // what DrawGizmo applies to. Anything else and the undo would restore a
    // subset of what moved.
    m_xformBaselines.clear();
    {
        const std::vector<Entity> roots =
            (m_selectedEntities.size() > 1) ? SelectionRoots()
                                            : std::vector<Entity>{ m_selectedEntity };
        for (Entity e : roots)
        {
            if (e == NullEntity || !m_world->IsAlive(e)) continue;
            if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(e))
                m_xformBaselines.emplace_back(e, *lt);
        }
    }

    m_xformBaselineEntity = m_selectedEntity;
    if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(m_selectedEntity))
        m_xformBaseline = *lt;
    m_xformBaselineValid = !m_xformBaselines.empty();
}

void EditorLayer::CommitTransformEdit()
{
    if (!m_world || !UndoEnabled()) return;
    if (m_xformBefores.empty()) return;

    // Collect the entities that ACTUALLY moved. A click that did not move the
    // handle must not push an undo entry, and in a multi-select drag an entity
    // whose transform is unchanged (e.g. a zero delta on a constrained axis)
    // should not appear in the history either.
    struct Moved { Entity e; LocalTransform before, after; };
    std::vector<Moved> moved;
    moved.reserve(m_xformBefores.size());

    for (const auto& [e, before] : m_xformBefores)
    {
        if (!m_world->IsAlive(e)) continue;
        const LocalTransform* lt = m_world->GetComponent<LocalTransform>(e);
        if (!lt) continue;
        const LocalTransform& b = *lt;
        const bool changed =
            memcmp(&before.translation, &b.translation, sizeof(b.translation)) != 0 ||
            memcmp(&before.rotation,    &b.rotation,    sizeof(b.rotation))    != 0 ||
            memcmp(&before.scale,       &b.scale,       sizeof(b.scale))       != 0;
        if (changed) moved.push_back({ e, before, b });
    }
    if (moved.empty()) return;

    if (moved.size() == 1)
    {
        // Single entity: push the bare action so TransformEditAction::TryMerge
        // can still collapse it with a back-to-back edit of the same entity.
        PushEdit(std::make_unique<Editor::Undo::TransformEditAction>(
            m_world->MakeHandle(moved[0].e), moved[0].before, moved[0].after));
    }
    else
    {
        // One gesture is one Ctrl+Z, however many entities it touched.
        auto macro = std::make_unique<Editor::Undo::EditMacro>("Transform selection");
        for (const Moved& m : moved)
            macro->Add(std::make_unique<Editor::Undo::TransformEditAction>(
                m_world->MakeHandle(m.e), m.before, m.after));
        PushEdit(std::move(macro));
    }

    // Sequencer record mode. Deliberately after PushEdit: the scene edit is what the
    // user did and the keys are its consequence - undoing the drag puts the
    // transform back, and the Sequencer's own undo ring holds the key insertion
    // (the two histories are intentionally separate).
    for (const Moved& m : moved)
        SequencerOnTransformCommitted(static_cast<uint32_t>(m.e));
}

// Re-instantiates rather than copies: two entities sharing one skinning slot would
// both draw through the same patched descriptor (contract at the declaration).
Entity EditorLayer::DuplicateSceneInstance(Entity src, Entity newParent,
                                           std::unordered_map<Entity, Entity>* srcToDst)
{
    if (!m_world || !m_assetMgr) return NullEntity;

    const SceneSourcePath* ssp = m_world->GetComponent<SceneSourcePath>(src);
    if (!ssp || ssp->path.empty()) return NullEntity;
    Resource::MeshLibrary* meshLib = m_renderer ? m_renderer->GetMeshLibrary() : nullptr;
    if (!meshLib) return NullEntity;

    // `ssp` dangles once the loader touches the pools - copy the path out now.
    // Everything else is read after the load: SceneInstanceLoader only creates
    // entities, it never mutates the source subtree, so re-fetching by entity id
    // afterwards is safe (holding raw component pointers across it is not).
    const std::string scenePath = ssp->path;
    const std::string srcName   = m_world->GetName(src);

    const auto res = SceneInstanceLoader::Load(scenePath, *m_world, *m_assetMgr,
                                               *meshLib, m_renderer);
    if (!res.success || res.rootEntity == NullEntity)
    {
        LOG_ERROR("EditorLayer: duplicate — failed to re-instantiate '%s'", scenePath.c_str());
        return NullEntity;
    }
    const Entity dst = res.rootEntity;

    // Merge the source's authored state onto the fresh instance by walking both
    // trees in parallel, matching children by name. Nodes the user added under
    // the character (a weapon, a light, a world-space UI bar) have no
    // counterpart in the .iscn, so they are cloned whole - otherwise
    // re-instantiating would silently drop them.
    auto& reg = GetComponentRegistry();
    const SceneInstanceCloneFn sceneClone =
        [this, srcToDst](Entity s, Entity p) { return DuplicateSceneInstance(s, p, srcToDst); };

    // Recursive lambda (std::function so it can call itself).
    std::function<void(Entity, Entity)> mergeNode = [&](Entity s, Entity d)
    {
        if (srcToDst) (*srcToDst)[s] = d;

        // Authored components; Guid stays unique per entity and SceneRef is
        // already on the fresh root. `d` is not empty: the load gave it the
        // .iscn's own components (Visibility, a part's MaterialRef / MeshRef,
        // a collision entity's Collider + RigidBody, the ChainPhysics the
        // skinning setup auto-attaches). What it has is rewritten
        // (ComponentWrite) rather than deserialized over, which would reset
        // runtime state and, for Script, append the slots to any already
        // there; what it lacks is deserialized fresh.
        for (const auto& [type, ser] : reg.All())
        {
            if (ser.tag == "SceneRef" || ser.tag == "Guid") continue;
            if (!ser.has || !ser.has(*m_world, s)) continue;
            std::ostringstream ss;
            ser.serialize(*m_world, s, ss);
            const std::string blob = ss.str();
            if (blob.empty()) continue;
            if (m_world->HasComponentByType(d, type))
                ComponentWrite::RewriteFromBlob(reg, *m_world, d, type, blob, m_assetMgr);
            else
                Editor::Undo::DeserializeComponentBlob(reg, *m_world, d, type, blob, m_assetMgr);
        }
        // Live material wins over the MaterialRef replay - same reason as in
        // CloneEntityRecursive (overrides are applied at load, not on parse).
        if (const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(s))
        {
            MaterialComponent mat = *mc;
            mat.SetDirty();
            m_world->AddComponent<MaterialComponent>(d, std::move(mat));
        }

        std::vector<Entity> sKids, dKids;
        if (const Children* ch = m_world->GetComponent<Children>(s)) sKids = ch->entities;
        if (const Children* ch = m_world->GetComponent<Children>(d)) dKids = ch->entities;
        std::vector<uint8_t> taken(dKids.size(), 0u);

        for (size_t i = 0; i < sKids.size(); ++i)
        {
            if (!m_world->IsAlive(sKids[i])) continue;
            const std::string kidName = m_world->GetName(sKids[i]);

            // Same slot first (the common case: untouched instance), then any
            // unclaimed same-name sibling (covers reordering in the outliner).
            size_t match = dKids.size();
            if (i < dKids.size() && !taken[i] && m_world->GetName(dKids[i]) == kidName)
                match = i;
            else
                for (size_t j = 0; j < dKids.size(); ++j)
                    if (!taken[j] && m_world->GetName(dKids[j]) == kidName) { match = j; break; }

            if (match < dKids.size())
            {
                taken[match] = 1u;
                mergeNode(sKids[i], dKids[match]);
            }
            else
            {
                // Not part of the asset - a user-added subtree: clone it whole
                // (with the same hook so a nested character re-instantiates too).
                CloneEntityRecursive(m_world, m_assetMgr, sKids[i], d, sceneClone, srcToDst);
            }
        }

        // Parts present in the asset but not under the source (deleted or
        // renamed by the user) stay in the copy: dropping them would delete a
        // correctly-skinned part just because a rename broke the name match.
        for (size_t j = 0; j < dKids.size(); ++j)
            if (!taken[j])
                LOG_INFO("EditorLayer: duplicate '%s' — asset node '%s' has no counterpart "
                         "in the source subtree; kept in the copy",
                         scenePath.c_str(), m_world->GetName(dKids[j]).c_str());
    };
    mergeNode(src, dst);

    // Root pose / name / hierarchy link - mirrors CloneEntityRecursive, including
    // its naming rule: only a top-level copy gets the "(Copy)" suffix.
    LocalTransform rootLT{};
    if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(src)) rootLT = *lt;
    if (LocalTransform* lt = m_world->GetComponent<LocalTransform>(dst)) *lt = rootLT;
    else m_world->AddComponent<LocalTransform>(dst, rootLT);
    m_world->SetName(dst, newParent == NullEntity ? srcName + " (Copy)" : srcName);

    if (newParent != NullEntity && m_world->IsAlive(newParent))
    {
        m_world->AddComponent<Parent>(dst, Parent{ newParent });
        if (Children* pc = m_world->GetComponent<Children>(newParent))
            pc->entities.push_back(dst);
        else
        {
            Children ch;
            ch.entities.push_back(dst);
            m_world->AddComponent<Children>(newParent, std::move(ch));
        }
    }
    return dst;
}

// Skinned .iscn instances re-instantiate (DuplicateSceneInstance); everything else
// goes through CloneEntityRecursive, which keeps per-entity edits verbatim.
Entity EditorLayer::DuplicateEntity(Entity src)
{
    if (!m_world || src == NullEntity || !m_world->IsAlive(src)) return NullEntity;
    // A UI Document's instance node belongs to its .iui: the copy would be a saved scene
    // node inside the instance (the clone drops the instance's tags), deleted unrecorded
    // by the next respawn. Ctrl+D / Paste reach here from any panel.
    if (UI::DocumentHostOf(*m_world, src) != NullEntity)
    {
        LOG_WARNING("EditorLayer: Duplicate skipped - '%s' is a node of a UI Document instance (read-only: edit its "
                    ".iui)", m_world->GetName(src).c_str());
        return NullEntity;
    }

    Entity srcParent = NullEntity;
    if (const Parent* p = m_world->GetComponent<Parent>(src)) srcParent = p->entity;

    // A skinned mesh part (a character's child entity, no SceneRef of its own)
    // has nothing to re-instantiate - say so instead of silently shipping a
    // rest-pose copy.
    if (m_world->HasComponent<MeshSkinnedComponent>(src)
        && !m_world->HasComponent<SceneSourcePath>(src))
        LOG_WARNING("EditorLayer: entity %u is a skinned mesh part with no SceneRef — "
                    "the copy will be a static rest-pose mesh (duplicate the character "
                    "root instead)", src);

    Entity   dup      = NullEntity;
    uint32_t attached = 0;

    // RecordSpawn (not a single Capture): attachment followers are cloned as
    // separate roots, so the undo entry has to cover every root the operation
    // created - otherwise undoing a duplicate would delete the character copy
    // and orphan its weapon copy. RecordSpawn diffs the entity list and groups
    // multi-root creates into one macro.
    RecordSpawn("Duplicate Entity", [&]
    {
        // Source -> copy pairs for every entity produced, so references pointing
        // inside what was copied can be re-pointed at the copy afterwards.
        std::unordered_map<Entity, Entity> srcToDst;
        const SceneInstanceCloneFn sceneClone =
            [this, &srcToDst](Entity s, Entity p) { return DuplicateSceneInstance(s, p, &srcToDst); };

        dup = CloneEntityRecursive(m_world, m_assetMgr, src, srcParent, sceneClone, &srcToDst);
        if (dup == NullEntity) return;

        attached = CloneAttachedFollowers(m_world, m_assetMgr, sceneClone, srcToDst);
        // Inside the record scope so the undo snapshot stores the remapped refs.
        RemapClonedRefs(*m_world, srcToDst);
    });

    if (dup == NullEntity) return NullEntity;
    // A UI asset tab's copy is its stage's: the tab selects it, not the scene.
    if (!m_editSink) m_selectedEntity = dup;
    MarkHierarchyDirty();
    LOG_INFO("EditorLayer: duplicated entity %u -> %u ('%s')%s",
             src, dup, m_world->GetName(dup).c_str(),
             attached ? " + attached followers" : "");
    if (attached)
        LOG_INFO("EditorLayer: duplicate — brought %u attached follower(s) "
                 "(FollowSocket/FollowEntity) along with the copy", attached);
    return dup;
}

void EditorLayer::CopySelected()
{
    if (!(m_world && m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity)))
        return;
    // The clipboard snapshot is a SavePrefab, which does not honour
    // SceneTransientTag - a copied preview-stage entity would paste back as a
    // stripped, y=-4096 copy. Selectable only with Internal shown.
    if (m_world->HasComponent<PreviewStageTag>(m_selectedEntity))
    {
        LOG_WARNING("EditorLayer: Copy ignored - entity %u belongs to the Chain "
                    "Physics Preview stage", static_cast<unsigned>(m_selectedEntity));
        return;
    }
    // A UI Document's instance node belongs to its .iui: a paste would land a scene
    // copy inside the instance (its next respawn deletes it, unrecorded).
    if (UI::DocumentHostOf(*m_world, m_selectedEntity) != NullEntity)
    {
        LOG_WARNING("EditorLayer: Copy ignored - '%s' is a node of a UI Document instance (read-only: edit its .iui)",
                    m_world->GetName(m_selectedEntity).c_str());
        return;
    }
    m_clipboardEntity = m_world->MakeHandle(m_selectedEntity);

    // Serialized clipboard. The live handle above stays the fast path
    // (same-session paste = DuplicateEntity, keeps follower closure); the .ipfb
    // snapshot lets a paste survive deleting the source, loading another scene,
    // or a second editor on the same project. The prefab pipeline gives
    // entity-id remap and per-instance GUID reseeding for free (LoadPrefab's
    // instanceSeed contract) - the two hard parts of any snapshot-paste.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = Sys::U8Path(Sys::WritableDataDir()) / "clipboard";
    fs::create_directories(dir, ec);
    const std::string clip = Sys::PathToUtf8(dir / "clipboard.ipfb");
    if (Resource::SavePrefab(m_selectedEntity, *m_world, clip))
        m_clipboardFile = clip;
    else
    {
        m_clipboardFile.clear();
        LOG_WARNING("EditorLayer: clipboard snapshot failed — paste will "
                    "require the source entity to stay alive");
    }
    LOG_INFO("EditorLayer: copied entity %u%s", m_selectedEntity,
             m_clipboardFile.empty() ? "" : " (+ serialized snapshot)");
}

void EditorLayer::PasteClipboard()
{
    if (!m_world) return;

    // Fast path - source still alive in this world.
    if (m_world->IsHandleValid(m_clipboardEntity))
    {
        DuplicateEntity(m_clipboardEntity.entity);
        return;
    }

    // Snapshot path - source deleted or the scene changed since the copy.
    if (m_clipboardFile.empty() || !m_assetMgr) return;
    std::error_code ec;
    if (!std::filesystem::exists(Sys::U8Path(m_clipboardFile), ec))
        return;

    Entity spawned = NullEntity;
    RecordSpawn("Paste Entity", [&]
    {
        spawned = Resource::LoadPrefab(m_clipboardFile, *m_world, *m_assetMgr,
                                       m_renderer, m_animClipSys);
        // The clipboard file is transient (overwritten by the next copy) - a
        // PrefabSourcePath pointing at it would make SaveScene write a dangling
        // prefab reference. Detach immediately: the paste is plain scene data,
        // exactly like the fast path's duplicate.
        if (spawned != NullEntity
            && m_world->HasComponent<PrefabSourcePath>(spawned))
            m_world->RemoveComponent<PrefabSourcePath>(spawned);
    });
    if (spawned != NullEntity)
    {
        m_selectedEntity = spawned;
        MarkHierarchyDirty();
        LOG_INFO("EditorLayer: pasted entity %u from the clipboard snapshot", spawned);
    }
    else
        LOG_ERROR("EditorLayer: clipboard paste failed ('%s' unreadable?)",
                  m_clipboardFile.c_str());
}

// Opens a macro in @p sink's history the first time an edit of the current
// Editor.Batch reaches it, so a batch is all or nothing across histories: the
// scene's macro plus one in every asset tab ring the batch touched.
void EditorLayer::JoinBatch(const std::shared_ptr<Editor::Undo::EditSink>& sink)
{
    if (!sink || std::find(m_batchSinks.begin(), m_batchSinks.end(), sink) != m_batchSinks.end()) return;
    sink->history.BeginMacro(m_batchLabel.empty() ? std::string("Batch") : m_batchLabel);
    m_batchSinks.push_back(sink);
}

std::size_t EditorLayer::AbortBatchSinks()
{
    std::vector<std::shared_ptr<Editor::Undo::EditSink>> sinks;
    sinks.swap(m_batchSinks);
    std::size_t n = 0;
    for (const auto& s : sinks)
    {
        if (!s) continue;
        SinkScope scope(*this, s);
        SyncedEditContext();   // the ring's context names this world
        n += s->history.AbortMacro();
        if (s->changed) s->changed();
    }
    return n;
}

void EditorLayer::EndBatchSinks()
{
    std::vector<std::shared_ptr<Editor::Undo::EditSink>> sinks;
    sinks.swap(m_batchSinks);
    for (const auto& s : sinks)
        if (s)
        {
            s->history.EndMacro();
            if (s->changed) s->changed();
        }
}

void EditorLayer::PushEdit(std::unique_ptr<Editor::Undo::IEditAction> action)
{
    if (!action) return;
    // A UI asset tab's edit lands in its own ring and dirties its file (MarkSceneDirty
    // tells the sink), never the scene's.
    EditTarget().Push(std::move(action));
    if (m_editSink) MarkSceneDirty();
    else            SyncSceneDirtyToHistory();
}

void EditorLayer::EditUndo()
{
    if (!m_world || !UndoEnabled() || !m_editHistory.CanUndo()) return;
    SyncedEditContext();
    m_editHistory.Undo();
    MarkHierarchyDirty();
    // Clean again only when the undo walked back to the state last saved or loaded.
    SyncSceneDirtyToHistory();
    // Force the gizmo to re-cache its matrix from the restored GlobalTransform.
    m_gizmoCachedEntity  = NullEntity;
    m_xformBaselineValid = false;
}

void EditorLayer::EditRedo()
{
    if (!m_world || !UndoEnabled() || !m_editHistory.CanRedo()) return;
    SyncedEditContext();
    m_editHistory.Redo();
    MarkHierarchyDirty();
    SyncSceneDirtyToHistory();
    m_gizmoCachedEntity  = NullEntity;
    m_xformBaselineValid = false;
}

Editor::Undo::EditContext& EditorLayer::SyncedEditContext()
{
    // The history in scope: a UI asset tab's ring keeps its own setSelection (the
    // tab's selection), set when the tab made it - only what is unset is filled here.
    auto& c  = EditTarget().Context();
    c.world  = m_world;
    c.reg    = &GetComponentRegistry();
    c.assets = m_assetMgr;
    // Route structural GPU-slot lifetime to the renderer: park skinned-mesh
    // descriptor slots before a delete, un-park on restore, free on discard.
    // Without this a resurrected mesh draws through a freed descriptor -> GPU hang.
    if (!c.onEntitiesRestored && m_renderer)
    {
        Renderer* r = m_renderer;
        c.onEntitiesWillDetach = [r](const std::vector<Entity>& ids) { r->ParkEntitiesForUndo(ids); };
        c.onEntitiesRestored   = [r](const std::vector<Entity>& ids) { r->OnEntitiesRestored(ids); };
        c.onEntitiesDiscarded  = [r](const std::vector<Entity>& ids) { r->FreeUndoParkedEntities(ids); };
    }
    if (!c.setSelection)
        c.setSelection = [this](Entity e) { SetSelectionFromUndo(e); };
    return c;
}

void EditorLayer::SetSelectionFromUndo(Entity e)
{
    m_selectedEntity = (e != NullEntity && m_world && m_world->IsAlive(e)) ? e : NullEntity;
    // Let the OnUIRender two-click sync re-align the hierarchy highlight.
    m_lastSeenSelectedEntity = m_selectedEntity;
    m_hierarchyHighlight     = m_selectedEntity;
}

// Each new root gets a subtree CreateAction; several roots share one EditMacro.
void EditorLayer::RecordSpawn(const char* label, const std::function<void()>& spawn)
{
    if (!m_world || !UndoEnabled())
    {
        if (spawn) spawn();
        MarkHierarchyDirty();
        return;
    }

    std::unordered_set<Entity> before(m_world->GetEntities().begin(),
                                      m_world->GetEntities().end());
    if (spawn) spawn();
    MarkHierarchyDirty();

    std::unordered_set<Entity> created;
    for (Entity e : m_world->GetEntities())
        if (!before.count(e)) created.insert(e);
    if (created.empty()) return;

    // Roots = created entities whose (scene-graph or UI) parent is not itself new.
    std::vector<Entity> roots;
    for (Entity e : created)
    {
        const Parent* p = m_world->GetComponent<Parent>(e);
        const bool parentNew = p && p->entity != NullEntity && created.count(p->entity);
        const UI::UIParent* up = m_world->GetComponent<UI::UIParent>(e);
        const bool uiParentNew = up && up->parent != NullEntity && created.count(up->parent);
        if (!parentNew && !uiParentNew) roots.push_back(e);
    }
    if (roots.empty()) return;   // shouldn't happen, but never record an empty entry

    SyncedEditContext();
    if (roots.size() == 1)
    {
        auto snap = Editor::Undo::EntitySubtreeSnapshot::Capture(*m_world, roots[0]);
        PushEdit(Editor::Undo::MakeCreateAction(std::move(snap), label));
    }
    else
    {
        auto macro = std::make_unique<Editor::Undo::EditMacro>(label);
        for (Entity r : roots)
        {
            auto snap = Editor::Undo::EntitySubtreeSnapshot::Capture(*m_world, r);
            macro->Add(Editor::Undo::MakeCreateAction(std::move(snap), label));
        }
        PushEdit(std::move(macro));
    }
}

// Play mode records nothing: the world is runtime state that Stop discards.
void EditorLayer::InstallEntityWriteObserver()
{
    LuaEntityWriteObserver o;
    o.component = [this](Entity e, std::type_index type, const std::string& before, const std::string& after)
    {
        if (!m_world || !UndoEnabled() || before == after) return;
        // A node of a UI Editor asset tab's stage belongs to the file: a step of that
        // tab's ring, its dirt. A UI Editor stage with no ring (a read-only template)
        // records nothing, and the scene's stack never sees it.
        std::shared_ptr<Editor::Undo::EditSink> sink;
        if (const PreviewStageTag* st = m_world->GetComponent<PreviewStageTag>(e);
            st && st->owner == PreviewStageTag::kUIEditor)
        {
            sink = Editor::UIEditorWindow::Get().SinkForStageEntity(*m_world, e);
            if (!sink) return;
        }
        SinkScope scope(*this, sink);
        PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
            m_world->MakeHandle(e), type, before, after, ComponentLabelFor(type)));
    };
    o.transform = [this](Entity e, const LocalTransform& before, const LocalTransform& after)
    {
        if (!m_world || !UndoEnabled()) return;
        // Same routing as a component write: a UI Editor stage node's step is its
        // tab's (none for a read-only template), never the scene's.
        std::shared_ptr<Editor::Undo::EditSink> sink;
        if (const PreviewStageTag* st = m_world->GetComponent<PreviewStageTag>(e);
            st && st->owner == PreviewStageTag::kUIEditor)
        {
            sink = Editor::UIEditorWindow::Get().SinkForStageEntity(*m_world, e);
            if (!sink) return;
        }
        SinkScope scope(*this, sink);
        PushEdit(std::make_unique<Editor::Undo::TransformEditAction>(m_world->MakeHandle(e), before, after));
    };
    SetLuaEntityWriteObserver(std::move(o));
}
